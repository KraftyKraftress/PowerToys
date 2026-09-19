//==============================================================================
//
//  ZoomItSmoothingBench.cpp - ZoomIt zoomed-image smoothing: capture, compare, time.
//
//  The setting under test is ZoomIt's "Smooth the zoomed image" checkbox.  The
//  three code paths are selectable at run time, so one binary covers all of
//  them without a rebuild:
//
//     off        checkbox OFF - COLORONCOLOR + StretchBlt   (ships today)
//     halftone   checkbox ON  - HALFTONE + StretchBlt       (ships today)
//     new        checkbox ON  - SmoothStretchBlt            (this branch)
//
//  ZoomIt is never launched.  Capture a screen once, then every run resamples
//  those exact pixels.
//
//     PowerToys.ZoomItSmoothingBench capture shots\desk.png
//     PowerToys.ZoomItSmoothingBench compare shots            --zoom 4
//     PowerToys.ZoomItSmoothingBench bench   shots --mode all --zoom 1.25,2,4
//     PowerToys.ZoomItSmoothingBench dump    shots --mode new --zoom 4 --out frames
//
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
//==============================================================================
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "GdiSurface.h"
#include "ScaleMetrics.h"
#include "ScreenCorpus.h"
#include "SmoothingMode.h"
#include "ZoomItScale.h"

namespace
{
    // ZOOM_LEVEL_STEP_TIME from ZoomIt.h - the telescoping animation's budget.
    constexpr double kFrameBudgetMs = 20.0;

    // Where the zoom is centred.  Negative means "middle of the frame", which
    // is the default; --at lets a run target a specific feature instead, since
    // the thing worth inspecting is rarely dead centre.
    int g_atX = -1, g_atY = -1;

    // constexpr because the repo's analyzer ruleset (C26497) asks for it.
    constexpr double Median(std::vector<double> v)
    {
        std::sort(v.begin(), v.end());
        return v.empty() ? 0.0 : v[v.size() / 2];
    }

    // Renders one frame and, optionally, times `reps` of them.
    bool RunMode(const Image& src, double zoom, SmoothingMode mode, Image& out, double* medianMs, int reps)
    {
        const Geometry g = GeometryFor(src, zoom, g_atX, g_atY);

        const ScreenDc screen;
        Surface full, ddb, dst;
        if (!full.Dib(screen.dc, src.w, src.h) || !ddb.Ddb(screen.dc, src.w, src.h) || !dst.Dib(screen.dc, g.dstW, g.dstH))
            return false;

        memcpy(full.bits, src.px.data(), src.px.size() * 4);
        BitBlt(ddb.dc, 0, 0, src.w, src.h, full.dc, 0, 0, SRCCOPY);

        LARGE_INTEGER freq, t0, t1;
        QueryPerformanceFrequency(&freq);
        std::vector<double> times;

        for (int r = 0; r < (medianMs ? reps : 1); ++r)
        {
            QueryPerformanceCounter(&t0);
            const BOOL ok = RenderZoom(mode, dst.dc, g.dstW, g.dstH, ddb.dc, g.srcX, g.srcY, g.srcW, g.srcH);
            GdiFlush();
            QueryPerformanceCounter(&t1);
            if (!ok)
            {
                fwprintf(stderr, L"mode %s could not render this geometry\n", ModeName(mode));
                return false;
            }
            times.push_back((t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart);
        }

        if (medianMs)
            *medianMs = Median(times);
        out = dst.Snapshot();

        SmoothStretchRelease();
        return out.Valid();
    }

    // The frames of ZoomIt's zoom-in animation to `target`: zoomLevel starts
    // at ZOOM_LEVEL_STEP_IN and multiplies by it every ZOOM_LEVEL_STEP_TIME
    // until it reaches the target, so the geometry changes on every frame.
    std::vector<double> TelescopeFrames(double target)
    {
        std::vector<double> seq;
        for (double z = 1.1; z < target; z *= 1.1)
            seq.push_back(z);
        seq.push_back(target);
        return seq;
    }

    std::vector<double> ParseZooms(const std::wstring& s)
    {
        std::vector<double> out;
        size_t start = 0;
        while (start <= s.size())
        {
            const size_t comma = s.find(L',', start);
            const std::wstring piece = s.substr(start, comma == std::wstring::npos ? std::wstring::npos : comma - start);
            if (!piece.empty())
            {
                const double v = _wtof(piece.c_str());
                if (v > 0)
                    out.push_back(v);
            }
            if (comma == std::wstring::npos)
                break;
            start = comma + 1;
        }
        if (out.empty())
            out = { 1.25, 2.0, 4.0 };
        return out;
    }

    std::vector<Image> LoadInputs(const std::wstring& path, std::vector<std::wstring>& names)
    {
        std::vector<Image> out;
        DWORD attr = GetFileAttributesW(path.c_str());

        std::vector<std::wstring> files;
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
            files = EnumerateCorpus(path);
        else
            files.push_back(path);

        for (const auto& f : files)
        {
            Image im;
            if (LoadPng(f, im) && im.Valid())
            {
                out.push_back(std::move(im));
                const size_t slash = f.find_last_of(L"\\/");
                names.push_back(slash == std::wstring::npos ? f : f.substr(slash + 1));
            }
            else
            {
                fwprintf(stderr, L"skipping unreadable %s\n", f.c_str());
            }
        }
        return out;
    }

    std::wstring Arg(int argc, wchar_t** argv, const wchar_t* name, const wchar_t* fallback)
    {
        for (int i = 1; i + 1 < argc; ++i)
            if (_wcsicmp(argv[i], name) == 0)
                return argv[i + 1];
        return fallback;
    }

    int Usage()
    {
        wprintf(
            L"ZoomIt \"Smooth the zoomed image\" - capture, compare, time\n\n"
            L"  PowerToys.ZoomItSmoothingBench capture <file.png>\n"
            L"      Save a full-resolution lossless screenshot to use as input.\n\n"
            L"  PowerToys.ZoomItSmoothingBench compare <png-or-dir> [--zoom 1.25,2,4]\n"
            L"      Quality of every mode against the others (PSNR / SSIM / GMSD),\n"
            L"      plus stroke-darkness statistics.\n\n"
            L"  PowerToys.ZoomItSmoothingBench bench <png-or-dir> [--mode ...] [--zoom ...] [--reps 31]\n"
            L"      Median frame time per mode against the %.0f ms animation budget.\n\n"
            L"  PowerToys.ZoomItSmoothingBench dump <png-or-dir> [--mode ...] [--zoom ...] [--out <dir>]\n"
            L"      Write the rendered frames as PNGs to look at.\n\n"
            L"  --mode takes a comma list of off, halftone, new - or 'all'.\n"
            L"  --at X,Y centres the zoom on a feature instead of the middle.\n",
            kFrameBudgetMs);
        return 1;
    }

    std::vector<SmoothingMode> ModesFrom(const std::wstring& text)
    {
        std::vector<SmoothingMode> out;
        size_t start = 0;
        for (;;)
        {
            const size_t comma = text.find(L',', start);
            const std::wstring piece = text.substr(start, comma == std::wstring::npos ? std::wstring::npos : comma - start);

            SmoothingMode m{};
            if (_wcsicmp(piece.c_str(), L"all") == 0)
            {
                out.insert(out.end(), { SmoothingMode::Off, SmoothingMode::Halftone, SmoothingMode::New });
            }
            else if (TryParseMode(piece, m))
            {
                out.push_back(m);
            }
            else if (!piece.empty())
            {
                fwprintf(stderr, L"unknown mode '%s'\n", piece.c_str());
                return {};
            }
            if (comma == std::wstring::npos)
                break;
            start = comma + 1;
        }
        return out;
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
        return Usage();

    const std::wstring cmd = argv[1];

    if (_wcsicmp(cmd.c_str(), L"capture") == 0)
    {
        if (argc < 3)
            return Usage();
        if (!CaptureScreenToPng(argv[2]))
        {
            fwprintf(stderr, L"capture failed\n");
            return 2;
        }
        wprintf(L"captured %s\n", argv[2]);
        return 0;
    }

    if (argc < 3)
        return Usage();

    const std::wstring input = argv[2];

    { // --at X,Y centres the zoom somewhere other than the middle.
        const std::wstring at = Arg(argc, argv, L"--at", L"");
        const size_t comma = at.find(L',');
        if (comma != std::wstring::npos)
        {
            g_atX = _wtoi(at.substr(0, comma).c_str());
            g_atY = _wtoi(at.substr(comma + 1).c_str());
            wprintf(L"zoom centred at %d,%d\n", g_atX, g_atY);
        }
    }
    const std::vector<double> zooms = ParseZooms(Arg(argc, argv, L"--zoom", L"1.25,2,4"));
    const std::vector<SmoothingMode> modes = ModesFrom(Arg(argc, argv, L"--mode", L"all"));
    if (modes.empty())
        return 2;

    std::vector<std::wstring> names;
    std::vector<Image> inputs = LoadInputs(input, names);
    if (inputs.empty())
    {
        fwprintf(stderr, L"no readable PNGs at %s - run 'PowerToys.ZoomItSmoothingBench capture' first\n", input.c_str());
        return 2;
    }

    if (_wcsicmp(cmd.c_str(), L"bench") == 0)
    {
        const int reps = (std::max)(3, _wtoi(Arg(argc, argv, L"--reps", L"31").c_str()));

        wprintf(L"%d rounds, budget %.0f ms/frame\n", reps, kFrameBudgetMs);
        wprintf(L"Modes are interleaved within each round and HALFTONE is re-measured every\n"
                L"round, so thermal drift or a background process hits every mode equally\n"
                L"rather than biasing whichever ran first.\n"
                L"Reported as median [p25-p75], and as a percentage of that run's HALFTONE.\n\n");

        for (size_t i = 0; i < inputs.size(); ++i)
        {
            wprintf(L"== %s (%dx%d)\n", names[i].c_str(), inputs[i].w, inputs[i].h);

            // Surfaces built once so the timed region is only the resample.
            const ScreenDc screen;
            Surface full, ddb, dst;
            if (!full.Dib(screen.dc, inputs[i].w, inputs[i].h) || !ddb.Ddb(screen.dc, inputs[i].w, inputs[i].h) || !dst.Dib(screen.dc, inputs[i].w, inputs[i].h))
            {
                fwprintf(stderr, L"surface creation failed\n");
                return 3;
            }
            memcpy(full.bits, inputs[i].px.data(), inputs[i].px.size() * 4);
            BitBlt(ddb.dc, 0, 0, inputs[i].w, inputs[i].h, full.dc, 0, 0, SRCCOPY);

            LARGE_INTEGER freq, t0, t1;
            QueryPerformanceFrequency(&freq);

            auto timeFrame = [&](SmoothingMode m, const Geometry& g) {
                QueryPerformanceCounter(&t0);
                const BOOL ok = RenderZoom(m, dst.dc, g.dstW, g.dstH, ddb.dc, g.srcX, g.srcY, g.srcW, g.srcH);
                GdiFlush();
                QueryPerformanceCounter(&t1);
                return ok ? (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart : -1.0;
            };

            for (double z : zooms)
            {
                const Geometry g = GeometryFor(inputs[i], z, g_atX, g_atY);
                wprintf(L"   zoom %.2fx  src %dx%d -> dst %dx%d\n", z, g.srcW, g.srcH, g.dstW, g.dstH);

                // One untimed warm-up frame per mode: it allocates the weight
                // tables and scratch surfaces, which is not steady-state cost.
                // A mode that cannot render this geometry is skipped rather
                // than timed as a no-op.
                std::vector<SmoothingMode> live;
                for (SmoothingMode m : modes)
                {
                    if (timeFrame(m, g) >= 0)
                        live.push_back(m);
                    else
                        wprintf(L"      %-12s unavailable\n", ModeName(m));
                }
                if (live.empty())
                    continue;

                std::vector<std::vector<double>> times(live.size());
                for (int r = 0; r < reps; ++r)
                    for (size_t mi = 0; mi < live.size(); ++mi)
                        times[mi].push_back(timeFrame(live[mi], g));

                SmoothStretchRelease();

                double halftoneMs = 0;
                for (size_t mi = 0; mi < live.size(); ++mi)
                    if (live[mi] == SmoothingMode::Halftone)
                        halftoneMs = Median(times[mi]);

                for (size_t mi = 0; mi < live.size(); ++mi)
                {
                    std::vector<double> v = times[mi];
                    std::sort(v.begin(), v.end());
                    const double med = Median(v);
                    const double p25 = v[v.size() / 4];
                    const double p75 = v[(v.size() * 3) / 4];

                    wprintf(L"      %-12s %8.2f ms  [%6.2f - %6.2f]", ModeName(live[mi]), med, p25, p75);
                    if (halftoneMs > 0)
                        wprintf(L"  %6.1f%% of HALFTONE", 100.0 * med / halftoneMs);
                    wprintf(L"   %s\n", med <= kFrameBudgetMs ? L"OK" : L"OVER BUDGET");
                }
            }

            // The animation: every frame has a different geometry, so this
            // includes the per-frame rebuild of the weight tables and any
            // growth of the scratch surface, which the fixed-zoom rows above
            // deliberately exclude.  The cache is dropped before each run, as
            // it is when ZoomIt enters zoom mode.
            {
                const double target = *std::max_element(zooms.begin(), zooms.end());
                const std::vector<double> frames = TelescopeFrames(target);
                wprintf(L"   zoom-in animation 1.10x -> %.2fx, %zu frames, median over %d runs\n", target, frames.size(), reps);

                for (SmoothingMode m : modes)
                {
                    std::vector<double> means, worsts;
                    bool ok = true;
                    for (int r = 0; r < reps && ok; ++r)
                    {
                        SmoothStretchRelease();
                        if (m == SmoothingMode::New)
                            SmoothStretchPrepare(dst.dc, inputs[i].w, inputs[i].h); // as ZoomIt does at zoom entry
                        double sum = 0, worst = 0;
                        for (double z : frames)
                        {
                            const double ms = timeFrame(m, GeometryFor(inputs[i], z, g_atX, g_atY));
                            if (ms < 0)
                            {
                                ok = false;
                                break;
                            }
                            sum += ms;
                            worst = (std::max)(worst, ms);
                        }
                        means.push_back(sum / static_cast<double>(frames.size()));
                        worsts.push_back(worst);
                    }
                    if (!ok)
                    {
                        wprintf(L"      %-12s unavailable\n", ModeName(m));
                        continue;
                    }
                    const double worst = Median(worsts);
                    wprintf(L"      %-12s %8.2f ms/frame mean, worst frame %6.2f ms   %s\n",
                            ModeName(m),
                            Median(means),
                            worst,
                            worst <= kFrameBudgetMs ? L"OK" : L"OVER BUDGET");
                }
                SmoothStretchRelease();
            }
            wprintf(L"\n");
        }
        return 0;
    }

    if (_wcsicmp(cmd.c_str(), L"compare") == 0)
    {
        for (size_t i = 0; i < inputs.size(); ++i)
        {
            wprintf(L"== %s (%dx%d)\n", names[i].c_str(), inputs[i].w, inputs[i].h);
            for (double z : zooms)
            {
                wprintf(L"   zoom %.2fx\n", z);

                // HALFTONE is the reference every candidate is scored against,
                // and nearest neighbour is rendered alongside so "how far is
                // HALFTONE from not smoothing at all" is always on the page.
                Image off, ht;
                RunMode(inputs[i], z, SmoothingMode::Off, off, nullptr, 1);
                RunMode(inputs[i], z, SmoothingMode::Halftone, ht, nullptr, 1);

                struct Cand
                {
                    SmoothingMode mode;
                    Image im;
                };
                std::vector<Cand> cands;
                for (SmoothingMode m : modes)
                {
                    if (m == SmoothingMode::Off || m == SmoothingMode::Halftone)
                        continue;
                    Cand c{ m, {} };
                    if (RunMode(inputs[i], z, m, c.im, nullptr, 1))
                        cands.push_back(std::move(c));
                }

                {
                    const Similarity s = Compare(off, ht);
                    wprintf(L"      halftone vs %-12s   psnr %5.1f dB   max delta %3d   ssim %.4f   gmsd %.4f\n",
                            L"off",
                            s.psnrDb,
                            s.maxChannelDelta,
                            s.ssim,
                            s.gmsd);
                }
                for (const Cand& c : cands)
                {
                    const Similarity s = Compare(ht, c.im);
                    wprintf(L"      halftone vs %-12s   psnr %5.1f dB   max delta %3d   ssim %.4f   gmsd %.4f\n",
                            ModeName(c.mode),
                            s.psnrDb,
                            s.maxChannelDelta,
                            s.ssim,
                            s.gmsd);
                }

                // Stroke darkness measured on HALFTONE's ink mask, so every
                // path is compared over exactly the same pixels.  Each image's
                // own mask would differ, and at low coverage that moves the
                // mean more than the effect being measured.
                const InkDelta dOff = CompareInk(ht, off);
                wprintf(L"      stroke darkness on HALFTONE's ink mask (%.2f%% of frame, ref %6.2f)\n",
                        dOff.coverage * 100,
                        dOff.refMean);
                wprintf(L"         %-12s %6.2f (%+6.2f)\n", L"off", dOff.candMean, dOff.delta);
                for (const Cand& c : cands)
                {
                    const InkDelta d = CompareInk(ht, c.im);
                    wprintf(L"         %-12s %6.2f (%+6.2f)\n", ModeName(c.mode), d.candMean, d.delta);
                }

                {
                    const InkStats st = MeasureInk(ht);
                    wprintf(L"      %-12s   mean gradient %6.2f   min luma %3d\n", L"halftone", st.meanGradient, st.minLuma);
                }
                for (const Cand& c : cands)
                {
                    const InkStats st = MeasureInk(c.im);
                    wprintf(L"      %-12s   mean gradient %6.2f   min luma %3d\n",
                            ModeName(c.mode),
                            st.meanGradient,
                            st.minLuma);
                }
                wprintf(L"\n");
            }
        }
        return 0;
    }

    if (_wcsicmp(cmd.c_str(), L"dump") == 0)
    {
        const std::wstring out = Arg(argc, argv, L"--out", L"frames");
        CreateDirectoryW(out.c_str(), nullptr);
        for (size_t i = 0; i < inputs.size(); ++i)
            for (double z : zooms)
                for (SmoothingMode m : modes)
                {
                    Image img;
                    if (!RunMode(inputs[i], z, m, img, nullptr, 1))
                        continue;
                    wchar_t name[MAX_PATH];
                    swprintf_s(name, L"%s\\%s_z%03.0f_%s.png", out.c_str(), names[i].substr(0, names[i].find_last_of(L'.')).c_str(), z * 100, ModeName(m));
                    if (SavePng(name, img))
                        wprintf(L"wrote %s\n", name);
                }
        return 0;
    }

    return Usage();
}
