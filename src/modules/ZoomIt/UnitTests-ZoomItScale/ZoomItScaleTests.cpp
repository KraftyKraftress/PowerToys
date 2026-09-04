//==============================================================================
//
// ZoomItScaleTests.cpp
//
// Compares the three "Smooth the zoomed image" code paths against each other
// and against the reference kernels they are meant to approximate, over
// full-resolution lossless screenshots.  ZoomIt itself is never launched.
//
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
//==============================================================================
#include "CppUnitTest.h"

#include "GdiSurface.h"
#include "ScaleMetrics.h"
#include "ScreenCorpus.h"
#include "SmoothingMode.h"
#include "ZoomItScale.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace
{
    // The zoom slider positions from ZoomIt.h's g_ZoomLevels.
    const double kZoomLevels[] = { 1.25, 2.0, 4.0 };

    const SmoothingMode kAllModes[] = { SmoothingMode::Off, SmoothingMode::Halftone, SmoothingMode::New };

    void Log(const std::wstring& s)
    {
        Logger::WriteMessage((s + L"\n").c_str());
    }

    std::wstring Num(double v, int dp = 2)
    {
        wchar_t buf[64];
        swprintf_s(buf, L"%.*f", dp, v);
        return buf;
    }

    // One screenshot loaded once and shared by every test.  A corpus that is
    // present but unreadable is a failure, not a reason to test something else.
    const Image& Screen()
    {
        static Image cached = [] {
            const std::wstring dir = CorpusDirectory();
            const auto files = EnumerateCorpus(dir);
            if (files.empty())
            {
                Log(L"no corpus in " + dir + L" - using the synthetic screen. "
                                             L"Run PowerToys.ZoomItSmoothingBench.exe capture <dir>\\screen.png to record a real one.");
                return SyntheticTextScreen(1920, 1080);
            }

            Image im;
            Assert::IsTrue(LoadPng(files.front(), im) && im.Valid(),
                           (L"corpus image could not be read: " + files.front()).c_str());
            Log(L"corpus: " + files.front() + L"  " + std::to_wstring(im.w) + L"x" + std::to_wstring(im.h));
            return im;
        }();
        return cached;
    }

    // Independent floating-point reference resampler.  taps == 2 is the
    // radius-1 tent (bilinear); taps == 4 is Mitchell-Netravali with the given
    // B and C.  Deliberately naive - it exists to check the fast path, so it
    // shares no code with it.
    Image RefResample(const Image& src, const Geometry& g, int taps, double B, double C)
    {
        auto kern = [&](double t) {
            t = std::fabs(t);
            if (taps == 2)
                return t < 1.0 ? 1.0 - t : 0.0;
            const double t2 = t * t, t3 = t2 * t;
            if (t < 1.0)
                return ((12 - 9 * B - 6 * C) * t3 + (-18 + 12 * B + 6 * C) * t2 + (6 - 2 * B)) / 6.0;
            if (t < 2.0)
                return ((-B - 6 * C) * t3 + (6 * B + 30 * C) * t2 + (-12 * B - 48 * C) * t + (8 * B + 24 * C)) / 6.0;
            return 0.0;
        };

        // Clamp to the source region, not the whole screenshot: the kernel
        // mirrors only that region, so nothing outside it is available.
        auto at = [&](int px, int py) {
            px = std::clamp(px, g.srcX, g.srcX + g.srcW - 1);
            py = std::clamp(py, g.srcY, g.srcY + g.srcH - 1);
            return src.px[static_cast<size_t>(py) * src.w + px];
        };

        Image out;
        out.w = g.dstW;
        out.h = g.dstH;
        out.px.resize(static_cast<size_t>(g.dstW) * g.dstH);

        const double sx = static_cast<double>(g.srcW) / g.dstW;
        const double sy = static_cast<double>(g.srcH) / g.dstH;
        const int off = taps == 2 ? 0 : 1;

        for (int y = 0; y < g.dstH; ++y)
        {
            const double cy = g.srcY + (y + 0.5) * sy - 0.5;
            const int y0 = static_cast<int>(std::floor(cy)) - off;
            double wy[4], sumy = 0;
            for (int k = 0; k < taps; ++k)
            {
                wy[k] = kern(static_cast<double>(y0) + k - cy);
                sumy += wy[k];
            }
            for (int k = 0; k < taps; ++k)
                wy[k] /= sumy;

            for (int x = 0; x < g.dstW; ++x)
            {
                const double cx = g.srcX + (x + 0.5) * sx - 0.5;
                const int x0 = static_cast<int>(std::floor(cx)) - off;
                double wx[4], sumx = 0;
                for (int k = 0; k < taps; ++k)
                {
                    wx[k] = kern(static_cast<double>(x0) + k - cx);
                    sumx += wx[k];
                }
                for (int k = 0; k < taps; ++k)
                    wx[k] /= sumx;

                uint32_t px = 0;
                for (int ch = 0; ch < 4; ++ch)
                {
                    const int sh = ch * 8;
                    double acc = 0;
                    for (int ky = 0; ky < taps; ++ky)
                        for (int kx = 0; kx < taps; ++kx)
                            acc += wy[ky] * wx[kx] * ((at(x0 + kx, y0 + ky) >> sh) & 0xFF);
                    const int v = std::clamp(static_cast<int>(std::lround(acc)), 0, 255);
                    px |= static_cast<uint32_t>(v) << sh;
                }
                out.px[static_cast<size_t>(y) * g.dstW + x] = px;
            }
        }
        return out;
    }

    // Magnify the centre of `src` by `zoom` through one code path.  Every
    // step is asserted so a failure names itself instead of surfacing later
    // as an odd metric.
    Image Render(const Image& src, double zoom, SmoothingMode mode)
    {
        const Geometry g = GeometryFor(src, zoom);

        const ScreenDc screen;
        Assert::IsNotNull(screen.dc);

        Surface full, ddb, dst;
        Assert::IsTrue(full.Dib(screen.dc, src.w, src.h), L"source DIB");
        Assert::IsTrue(ddb.Ddb(screen.dc, src.w, src.h), L"source DDB");
        Assert::IsTrue(dst.Dib(screen.dc, g.dstW, g.dstH), L"destination DIB");

        memcpy(full.bits, src.px.data(), src.px.size() * 4);
        Assert::IsTrue(BitBlt(ddb.dc, 0, 0, src.w, src.h, full.dc, 0, 0, SRCCOPY) != FALSE);

        const BOOL drawn = RenderZoom(mode, dst.dc, g.dstW, g.dstH, ddb.dc, g.srcX, g.srcY, g.srcW, g.srcH);
        GdiFlush();
        Assert::IsTrue(drawn != FALSE,
                       (std::wstring(L"mode ") + ModeName(mode) + L" did not draw").c_str());

        const Image out = dst.Snapshot();

        SmoothStretchRelease();
        return out;
    }
}

namespace ZoomItScaleTests
{
    TEST_CLASS (SmoothingModeSelection)
    {
    public:
        TEST_METHOD (ModesParseFromText)
        {
            SmoothingMode m{};
            Assert::IsTrue(TryParseMode(L"off", m));
            Assert::IsTrue(m == SmoothingMode::Off);
            Assert::IsTrue(TryParseMode(L"HALFTONE", m));
            Assert::IsTrue(m == SmoothingMode::Halftone);
            Assert::IsTrue(TryParseMode(L"New", m));
            Assert::IsTrue(m == SmoothingMode::New);
            Assert::IsFalse(TryParseMode(L"bilinear", m));
        }

        TEST_METHOD (EveryModeRenders)
        {
            // Render asserts that the mode's blit reported success, so a
            // SmoothStretchBlt that declines the geometry fails here rather
            // than turning every later comparison into HALFTONE vs itself.
            const Image& src = Screen();
            for (SmoothingMode mode : kAllModes)
            {
                for (double zoom : kZoomLevels)
                {
                    Assert::IsTrue(Render(src, zoom, mode).Valid(),
                                   (std::wstring(L"mode ") + ModeName(mode) + L" produced nothing").c_str());
                }
            }
        }
    };

    TEST_CLASS (KernelIdentity)
    {
    public:
        // The kernel is a separable Mitchell-Netravali cubic, B=1/6 C=1/2.
        // Pin it against an independently computed floating-point reference so
        // a future change to the weights is caught here rather than by eye.
        TEST_METHOD (NewKernelMatchesCubicReference)
        {
            const Image& src = Screen();
            const double zoom = 4.0;
            const Geometry g = GeometryFor(src, zoom);

            const Image mine = Render(src, zoom, SmoothingMode::New);

            const Image ref = RefResample(src, g, 4, 1.0 / 6.0, 1.0 / 2.0);
            const Similarity s = Compare(mine, ref);

            Log(L"new kernel vs float cubic reference: psnr " + Num(s.psnrDb, 1) + L" dB, max delta " + std::to_wstring(s.maxChannelDelta) + L", ssim " + Num(s.ssim, 4));

            // Q14 weights and an int16 intermediate, so a rounding step of
            // slack - but no more than that.
            Assert::IsTrue(s.maxChannelDelta <= 3,
                           L"the kernel has drifted away from its cubic definition");
            Assert::IsTrue(s.ssim > 0.999, L"structural mismatch against the reference");
        }
    };

    TEST_CLASS (SmoothingComparison)
    {
    public:
        // Both smoothed paths must be far away from nearest neighbour,
        // otherwise "smoothing on" is not smoothing.
        TEST_METHOD (BothSmoothedPathsDifferFromNearest)
        {
            const Image& src = Screen();
            for (double zoom : kZoomLevels)
            {
                const Image off = Render(src, zoom, SmoothingMode::Off);
                for (SmoothingMode mode : { SmoothingMode::Halftone, SmoothingMode::New })
                {
                    const Image sm = Render(src, zoom, mode);
                    const Similarity s = Compare(off, sm);
                    Log(std::wstring(L"zoom ") + Num(zoom) + L"  off vs " + ModeName(mode) + L": psnr " + Num(s.psnrDb, 1) + L" dB, ssim " + Num(s.ssim, 4) + L", gmsd " + Num(s.gmsd, 4));
                    Assert::IsTrue(s.psnrDb < 40.0,
                                   L"a smoothed path is suspiciously close to nearest neighbour");
                }
            }
        }

        // The headline claim: the new path is a faithful stand-in for HALFTONE.
        TEST_METHOD (NewMatchesHalftoneStructurally)
        {
            const Image& src = Screen();
            for (double zoom : kZoomLevels)
            {
                const Image ht = Render(src, zoom, SmoothingMode::Halftone);
                const Image nw = Render(src, zoom, SmoothingMode::New);
                const Similarity s = Compare(ht, nw);

                Log(std::wstring(L"zoom ") + Num(zoom) + L"  halftone vs new: psnr " + Num(s.psnrDb, 1) + L" dB, max delta " + std::to_wstring(s.maxChannelDelta) + L", ssim " + Num(s.ssim, 4) + L", gmsd " + Num(s.gmsd, 4));

                // SSIM is the right gate here: the two are different filters, so
                // PSNR is naturally modest, but the *structure* must survive.
                Assert::IsTrue(s.ssim > 0.90,
                               L"the new kernel is structurally different from HALFTONE");
                Assert::IsTrue(s.gmsd < 0.20,
                               L"the new kernel distorts edges relative to HALFTONE");
            }
        }

        // Stroke darkness needs its own gate: SSIM and GMSD both call a kernel
        // that renders text several luma levels lighter than HALFTONE "near
        // identical", and that difference is visible.  Bilinear, being a
        // non-negative average, cannot match HALFTONE here, so it serves as
        // the bar the shipping kernel must beat.
        TEST_METHOD (StrokeDarknessMatchesHalftone)
        {
            const Image& src = Screen();

            int worse = 0;

            for (double zoom : kZoomLevels)
            {
                const Image ht = Render(src, zoom, SmoothingMode::Halftone);
                const Image nw = Render(src, zoom, SmoothingMode::New);

                // All three measured on HALFTONE's ink mask, so the same
                // pixels are compared in every image.
                const InkDelta dNew = CompareInk(ht, nw);
                const InkDelta dOff = CompareInk(ht, Render(src, zoom, SmoothingMode::Off));
                const InkDelta dBilin = CompareInk(ht, RefResample(src, GeometryFor(src, zoom), 2, 0, 0));

                Log(std::wstring(L"zoom ") + Num(zoom) + L"  HALFTONE stroke luma " + Num(dNew.refMean) + L" over " + Num(dNew.coverage * 100, 2) + L"% of frame");
                Log(L"    delta:  nearest " + Num(dOff.delta) + L"   bilinear " + Num(dBilin.delta) + L"   shipping " + Num(dNew.delta));

                // Self-calibrating rather than a hand-picked threshold: the
                // shipping kernel must track HALFTONE's stroke darkness more
                // closely than a plain bilinear kernel would, on this content,
                // at this zoom.  Reverting to 2 taps fails this by
                // construction, and there is no magic constant to maintain.
                if (std::fabs(dNew.delta) >= std::fabs(dBilin.delta))
                {
                    ++worse;
                    Log(L"    ^ shipping kernel is no closer than bilinear here");
                }
            }

            Assert::AreEqual(0, worse, L"the shipping kernel tracks HALFTONE's stroke darkness no better "
                                       L"than plain bilinear would - the contrast gain has been lost");
        }

        // Edge crispness, the other half of what "looks the same" means for
        // text.  Bilinear measurably rounds edges off; the cubic does not.
        // The band is a tolerance, not a fit: observed ratios are 1.01-1.08
        // across the synthetic screen and the archived screenshots.
        TEST_METHOD (EdgeCrispnessMatchesHalftone)
        {
            const Image& src = Screen();
            const InkStats ht = MeasureInk(Render(src, 4.0, SmoothingMode::Halftone));
            const InkStats nw = MeasureInk(Render(src, 4.0, SmoothingMode::New));

            const double ratio = ht.meanGradient > 0 ? nw.meanGradient / ht.meanGradient : 0;
            Log(L"mean gradient ratio new/halftone: " + Num(ratio, 3));
            Assert::IsTrue(ratio > 0.90 && ratio < 1.15,
                           L"edge contrast has drifted away from HALFTONE");
        }
    };

    TEST_CLASS (ContractTests)
    {
    public:
        TEST_METHOD (MinificationIsRejected)
        {
            // The kernel is magnification-only and says so, rather than
            // aliasing through a kernel that is too narrow for a downscale.
            const ScreenDc screen;
            Surface src, dst;
            Assert::IsTrue(src.Dib(screen.dc, 400, 300));
            Assert::IsTrue(dst.Dib(screen.dc, 100, 75));
            const BOOL ok = SmoothStretchBlt(dst.dc, 0, 0, 100, 75, src.dc, 0, 0, 400, 300);
            Assert::IsFalse(ok != FALSE, L"minification should be rejected");
        }

        TEST_METHOD (DegenerateRectanglesAreRejected)
        {
            const ScreenDc screen;
            Surface src, dst;
            Assert::IsTrue(src.Dib(screen.dc, 64, 64));
            Assert::IsTrue(dst.Dib(screen.dc, 64, 64));
            Assert::IsFalse(SmoothStretchBlt(dst.dc, 0, 0, 0, 64, src.dc, 0, 0, 64, 64) != FALSE);
            Assert::IsFalse(SmoothStretchBlt(dst.dc, 0, 0, 64, 64, src.dc, 0, 0, 64, 0) != FALSE);
            Assert::IsFalse(SmoothStretchBlt(dst.dc, 0, 0, 64, 64, nullptr, 0, 0, 64, 64) != FALSE);
        }

        // ZoomIt paints 1:1 while zoomed at the end of a zoom-out, and the cubic
        // is not an identity at scale 1, so that case must be an exact copy.
        TEST_METHOD (UnityScaleIsAnExactCopy)
        {
            const Image& src = Screen();
            const Image copy = Render(src, 1.0, SmoothingMode::New);
            const Similarity s = Compare(src, copy);
            Assert::AreEqual(0, s.maxChannelDelta, L"a 1:1 SmoothStretchBlt changed pixels");
        }

        TEST_METHOD (ReleaseIsIdempotent)
        {
            SmoothStretchRelease();
            SmoothStretchRelease();
        }

        // ZoomIt pre-allocates the scratch surfaces at full screen size when it
        // enters zoom mode, so the first animation frame reads a source region
        // narrower than the surface it sits in.  The output must not depend on
        // that stride.
        TEST_METHOD (PreparedCacheRendersIdentically)
        {
            const Image& src = Screen();
            const Image cold = Render(src, 4.0, SmoothingMode::New);

            const ScreenDc screen;
            SmoothStretchPrepare(screen.dc, src.w, src.h);
            const Geometry g = GeometryFor(src, 4.0);
            Surface full, ddb, dst;
            Assert::IsTrue(full.Dib(screen.dc, src.w, src.h) && ddb.Ddb(screen.dc, src.w, src.h) && dst.Dib(screen.dc, g.dstW, g.dstH));
            memcpy(full.bits, src.px.data(), src.px.size() * 4);
            BitBlt(ddb.dc, 0, 0, src.w, src.h, full.dc, 0, 0, SRCCOPY);
            Assert::IsTrue(SmoothStretchBlt(dst.dc, 0, 0, g.dstW, g.dstH, ddb.dc, g.srcX, g.srcY, g.srcW, g.srcH) != FALSE);
            GdiFlush();
            const Image warm = dst.Snapshot();
            SmoothStretchRelease();

            Assert::AreEqual(0, Compare(cold, warm).maxChannelDelta, L"output depends on the scratch surface size");
        }
    };

    TEST_CLASS (MetricSanity)
    {
    public:
        // The metrics are the measuring instrument; check the instrument.
        TEST_METHOD (IdenticalImagesScorePerfectly)
        {
            const Image a = SyntheticTextScreen(320, 240);
            const Similarity s = Compare(a, a);
            Assert::AreEqual(99.0, s.psnrDb, 0.001);
            Assert::AreEqual(0, s.maxChannelDelta);
            Assert::IsTrue(s.ssim > 0.9999);
            Assert::IsTrue(s.gmsd < 0.0001);
        }

        TEST_METHOD (BlurLowersSsimAndRaisesGmsd)
        {
            const Image sharp = SyntheticTextScreen(320, 240);

            Image blurred = sharp; // 3x1 box blur
            for (int y = 0; y < sharp.h; ++y)
                for (int x = 1; x < sharp.w - 1; ++x)
                {
                    uint32_t out = 0;
                    for (int ch = 0; ch < 3; ++ch)
                    {
                        const int sh = ch * 8;
                        const int v = (((sharp.px[static_cast<size_t>(y) * sharp.w + x - 1] >> sh) & 0xFF) + ((sharp.px[static_cast<size_t>(y) * sharp.w + x] >> sh) & 0xFF) + ((sharp.px[static_cast<size_t>(y) * sharp.w + x + 1] >> sh) & 0xFF)) / 3;
                        out |= static_cast<uint32_t>(v) << sh;
                    }
                    blurred.px[static_cast<size_t>(y) * sharp.w + x] = out | 0xFF000000u;
                }

            const Similarity s = Compare(sharp, blurred);
            Log(L"blur: ssim " + Num(s.ssim, 4) + L", gmsd " + Num(s.gmsd, 4));
            Assert::IsTrue(s.ssim < 0.99, L"SSIM did not react to blur");
            Assert::IsTrue(s.gmsd > 0.01, L"GMSD did not react to blur");
        }
    };
}
