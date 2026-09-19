//==============================================================================
//
// ScaleMetrics.cpp
//
// See ScaleMetrics.h for what these metrics are and where they come from.
//
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
//==============================================================================
#include "ScaleMetrics.h"

#include <algorithm>
#include <cmath>

namespace
{
    // Rec.601 luma, the plane SSIM and GMSD are conventionally evaluated on.
    constexpr double Luma(uint32_t bgra)
    {
        const double b = static_cast<double>(bgra & 0xFF);
        const double g = static_cast<double>((bgra >> 8) & 0xFF);
        const double r = static_cast<double>((bgra >> 16) & 0xFF);
        return 0.299 * r + 0.587 * g + 0.114 * b;
    }

    struct Plane
    {
        int w = 0;
        int h = 0;
        std::vector<double> v;
        double At(int x, int y) const { return v[static_cast<size_t>(y) * w + x]; }
        double& At(int x, int y) { return v[static_cast<size_t>(y) * w + x]; }
    };

    Plane MakePlane(const Image& im)
    {
        Plane p;
        p.w = im.w;
        p.h = im.h;
        p.v.resize(static_cast<size_t>(im.w) * im.h);
        for (size_t i = 0; i < p.v.size(); ++i)
            p.v[i] = Luma(im.px[i]);
        return p;
    }

    // 2x2 average pool, the preprocessing step the GMSD reference
    // implementation applies before taking gradients.
    Plane Downsample2(const Plane& s)
    {
        Plane d;
        d.w = (std::max)(1, s.w / 2);
        d.h = (std::max)(1, s.h / 2);
        d.v.resize(static_cast<size_t>(d.w) * d.h);
        for (int y = 0; y < d.h; ++y)
            for (int x = 0; x < d.w; ++x)
                d.At(x, y) = (s.At(2 * x, 2 * y) + s.At(2 * x + 1, 2 * y) + s.At(2 * x, 2 * y + 1) + s.At(2 * x + 1, 2 * y + 1)) * 0.25;
        return d;
    }

    // Prewitt gradient magnitude, as used by GMSD.
    Plane PrewittMagnitude(const Plane& s)
    {
        static const double kx[3][3] = { { 1.0 / 3, 0, -1.0 / 3 }, { 1.0 / 3, 0, -1.0 / 3 }, { 1.0 / 3, 0, -1.0 / 3 } };
        static const double ky[3][3] = { { 1.0 / 3, 1.0 / 3, 1.0 / 3 }, { 0, 0, 0 }, { -1.0 / 3, -1.0 / 3, -1.0 / 3 } };

        Plane g;
        g.w = s.w;
        g.h = s.h;
        g.v.assign(static_cast<size_t>(s.w) * s.h, 0.0);

        for (int y = 0; y < s.h; ++y)
        {
            for (int x = 0; x < s.w; ++x)
            {
                double sx = 0, sy = 0;
                for (int j = -1; j <= 1; ++j)
                {
                    for (int i = -1; i <= 1; ++i)
                    {
                        const int px = std::clamp(x + i, 0, s.w - 1);
                        const int py = std::clamp(y + j, 0, s.h - 1);
                        const double v = s.At(px, py);
                        sx += kx[j + 1][i + 1] * v;
                        sy += ky[j + 1][i + 1] * v;
                    }
                }
                g.At(x, y) = std::sqrt(sx * sx + sy * sy);
            }
        }
        return g;
    }

    // 11x11 Gaussian, sigma 1.5 - the window from the SSIM paper.
    const std::vector<double>& SsimWindow()
    {
        static std::vector<double> w = [] {
            const int R = 5;
            const double sigma = 1.5;
            std::vector<double> k(11 * 11);
            double sum = 0;
            for (int row = 0; row < 11; ++row)
                for (int col = 0; col < 11; ++col)
                {
                    const int y = row - R;
                    const int x = col - R;
                    const double v = std::exp(-(x * x + y * y) / (2.0 * sigma * sigma));
                    k[static_cast<size_t>(row) * 11 + static_cast<size_t>(col)] = v;
                    sum += v;
                }
            for (double& v : k)
                v /= sum;
            return k;
        }();
        return w;
    }

    double Ssim(const Plane& a, const Plane& b)
    {
        const double C1 = (0.01 * 255) * (0.01 * 255);
        const double C2 = (0.03 * 255) * (0.03 * 255);
        const auto& w = SsimWindow();
        const int R = 5;

        if (a.w < 11 || a.h < 11)
            return 0.0;

        double total = 0;
        size_t count = 0;

        for (int y = R; y < a.h - R; ++y)
        {
            for (int x = R; x < a.w - R; ++x)
            {
                double ma = 0, mb = 0;
                for (int row = 0; row < 11; ++row)
                    for (int col = 0; col < 11; ++col)
                    {
                        const double k = w[static_cast<size_t>(row) * 11 + static_cast<size_t>(col)];
                        ma += k * a.At(x + col - R, y + row - R);
                        mb += k * b.At(x + col - R, y + row - R);
                    }

                double va = 0, vb = 0, cov = 0;
                for (int row = 0; row < 11; ++row)
                    for (int col = 0; col < 11; ++col)
                    {
                        const double k = w[static_cast<size_t>(row) * 11 + static_cast<size_t>(col)];
                        const double da = a.At(x + col - R, y + row - R) - ma;
                        const double db = b.At(x + col - R, y + row - R) - mb;
                        va += k * da * da;
                        vb += k * db * db;
                        cov += k * da * db;
                    }

                const double num = (2 * ma * mb + C1) * (2 * cov + C2);
                const double den = (ma * ma + mb * mb + C1) * (va + vb + C2);
                total += den > 0 ? num / den : 1.0;
                ++count;
            }
        }
        return count ? total / static_cast<double>(count) : 0.0;
    }

    double Gmsd(const Plane& a, const Plane& b)
    {
        // c = 170 is the paper's constant for an 8-bit range (0.0026 * 255^2).
        const double c = 170.0;

        const Plane ga = PrewittMagnitude(Downsample2(a));
        const Plane gb = PrewittMagnitude(Downsample2(b));
        if (ga.v.empty())
            return 0.0;

        std::vector<double> gms(ga.v.size());
        double mean = 0;
        for (size_t i = 0; i < ga.v.size(); ++i)
        {
            const double x = ga.v[i], y = gb.v[i];
            gms[i] = (2 * x * y + c) / (x * x + y * y + c);
            mean += gms[i];
        }
        mean /= static_cast<double>(gms.size());

        // Deviation pooling: the standard deviation of the GMS map.
        double var = 0;
        for (double v : gms)
            var += (v - mean) * (v - mean);
        return std::sqrt(var / static_cast<double>(gms.size()));
    }
}

Similarity Compare(const Image& a, const Image& b)
{
    Similarity s;
    if (!a.Valid() || !b.Valid() || a.w != b.w || a.h != b.h)
        return s;

    double sse = 0;
    int mx = 0;
    const auto* pa = reinterpret_cast<const uint8_t*>(a.px.data());
    const auto* pb = reinterpret_cast<const uint8_t*>(b.px.data());
    const size_t n = a.px.size();

    // Alpha is meaningless in these surfaces; colour channels only.
    for (size_t i = 0; i < n; ++i)
        for (int ch = 0; ch < 3; ++ch)
        {
            const int d = static_cast<int>(pa[i * 4 + ch]) - static_cast<int>(pb[i * 4 + ch]);
            sse += static_cast<double>(d) * d;
            mx = (std::max)(mx, std::abs(d));
        }

    const double mse = sse / (static_cast<double>(n) * 3.0);
    s.rmsePercent = 100.0 * std::sqrt(mse) / 255.0;
    s.psnrDb = mse > 0 ? 10.0 * std::log10(255.0 * 255.0 / mse) : 99.0;
    s.maxChannelDelta = mx;

    const Plane pla = MakePlane(a);
    const Plane plb = MakePlane(b);
    s.ssim = Ssim(pla, plb);
    s.gmsd = Gmsd(pla, plb);
    return s;
}

InkDelta CompareInk(const Image& ref, const Image& cand, int inkThreshold)
{
    InkDelta d;
    if (!ref.Valid() || !cand.Valid() || ref.w != cand.w || ref.h != cand.h)
        return d;

    const Plane a = MakePlane(ref);
    const Plane b = MakePlane(cand);

    double sumRef = 0, sumCand = 0;
    size_t n = 0;

    for (size_t i = 0; i < a.v.size(); ++i)
    {
        if (a.v[i] >= inkThreshold)
            continue; // mask comes from ref only
        sumRef += a.v[i];
        sumCand += b.v[i];
        ++n;
    }

    if (n == 0)
        return d;
    d.refMean = sumRef / static_cast<double>(n);
    d.candMean = sumCand / static_cast<double>(n);
    d.delta = d.candMean - d.refMean;
    d.coverage = static_cast<double>(n) / static_cast<double>(a.v.size());
    return d;
}

InkStats MeasureInk(const Image& im, int inkThreshold)
{
    InkStats st;
    if (!im.Valid())
        return st;

    const Plane p = MakePlane(im);

    double sum = 0, inkSum = 0;
    size_t inkCount = 0;
    double minV = 255;

    for (double v : p.v)
    {
        sum += v;
        minV = (std::min)(minV, v);
        if (v < inkThreshold)
        {
            inkSum += v;
            ++inkCount;
        }
    }

    st.meanLuma = sum / static_cast<double>(p.v.size());
    st.minLuma = static_cast<int>(std::lround(minV));
    st.inkCoverage = static_cast<double>(inkCount) / static_cast<double>(p.v.size());
    st.inkMeanLuma = inkCount ? inkSum / static_cast<double>(inkCount) : 0.0;

    const Plane g = PrewittMagnitude(p);
    double gsum = 0;
    for (double v : g.v)
        gsum += v;
    st.meanGradient = gsum / static_cast<double>(g.v.size());

    return st;
}
