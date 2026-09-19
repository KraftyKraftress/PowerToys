//==============================================================================
//
// ZoomItScale.cpp
//
// See ZoomItScale.h for what this replaces and why.
//
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
//==============================================================================
// Deliberately does not include pch.h: this kernel needs nothing beyond
// <windows.h>, and the project builds it with PrecompiledHeader=NotUsing.
#include "ZoomItScale.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#if defined(_M_X64) || defined(_M_IX86)
#include <emmintrin.h>
#include <immintrin.h>
#include <intrin.h>
#define ZI_SSE2 1
// AVX2 is not architecturally guaranteed anywhere PowerToys runs, so unlike
// SSE2 and NEON it is selected at run time, with the SSE2 path as the fallback.
#define ZI_AVX2 1
#elif defined(_M_ARM64)
#include <arm_neon.h>
#define ZI_NEON 1
#endif

#ifndef ZI_SSE2
#define ZI_SSE2 0
#endif
#ifndef ZI_NEON
#define ZI_NEON 0
#endif
#ifndef ZI_AVX2
#define ZI_AVX2 0
#endif

namespace
{

    // Q14 filter weights.  The horizontal pass leaves its result scaled by
    // 2^( kWeightBits - kMidShift ) in an int16, so the vertical pass shifts by
    // kWeightBits + ( kWeightBits - kMidShift ) to land back on 0..255.
    constexpr int kWeightBits = 14;
    constexpr int kWeightOne = 1 << kWeightBits;
    constexpr int kMidShift = 8;
    constexpr int kFinalShift = kWeightBits * 2 - kMidShift;
    constexpr int kFinalRound = 1 << (kFinalShift - 1);

    // Below this many destination rows the threading overhead is not worth it.
    constexpr int kMinRowsPerThread = 64;

    //------------------------------------------------------------------------------
    // Filter weights
    //------------------------------------------------------------------------------

    // Mitchell-Netravali cubic (SIGGRAPH 1988), support [-2,2], so 4 taps.
    //
    // Not bilinear, although bilinear is cheaper: a non-negative 2-tap average can
    // never be darker than the darkest pixel it reads, whereas HALFTONE overshoots
    // and renders text strokes visibly darker.  Matching that needs the cubic's
    // negative outer taps.  ZoomIt only magnifies, so the tap count is fixed and
    // the whole kernel is specialized on it.
    //
    // B and C were fitted against real HALFTONE output rather than taken from a
    // chart (tools/ZoomItSmoothingBench).
    constexpr int kTaps = 4;
    constexpr double kRadius = 2.0;
    constexpr double kCubicB = 1.0 / 6.0;
    constexpr double kCubicC = 1.0 / 2.0;

    // Mitchell-Netravali, evaluated on |x| in source-pixel units.
    double CubicWeight(double x)
    {
        x = std::fabs(x);
        const double x2 = x * x;
        const double x3 = x2 * x;

        if (x < 1.0)
            return ((12.0 - 9.0 * kCubicB - 6.0 * kCubicC) * x3 + (-18.0 + 12.0 * kCubicB + 6.0 * kCubicC) * x2 + (6.0 - 2.0 * kCubicB)) / 6.0;
        if (x < 2.0)
            return ((-kCubicB - 6.0 * kCubicC) * x3 + (6.0 * kCubicB + 30.0 * kCubicC) * x2 + (-12.0 * kCubicB - 48.0 * kCubicC) * x + (8.0 * kCubicB + 24.0 * kCubicC)) / 6.0;
        return 0.0;
    }

    // Per destination coordinate: the first source index and kTaps weights.
    struct WeightTable
    {
        std::vector<int> first; // [ dstN ]
        std::vector<int16_t> w; // [ dstN * kTaps ], Q14, sums to kWeightOne
    };

    void BuildWeights(WeightTable& t, int dstN, double srcExtent)
    {
        const double scale = static_cast<double>(dstN) / srcExtent;

        t.first.resize(static_cast<size_t>(dstN));
        t.w.assign(static_cast<size_t>(dstN) * kTaps, 0);

        double raw[kTaps] = {};

        for (int i = 0; i < dstN; ++i)
        {
            const double c = (i + 0.5) / scale - 0.5;
            const int x0 = static_cast<int>(std::floor(c - kRadius)) + 1;

            t.first[static_cast<size_t>(i)] = x0;

            // Mitchell-Netravali is a partition of unity, so sum is 1 up to rounding.
            double sum = 0;
            for (int k = 0; k < kTaps; ++k)
            {
                raw[k] = CubicWeight(static_cast<double>(x0) + k - c);
                sum += raw[k];
            }

            int16_t* row = t.w.data() + static_cast<size_t>(i) * kTaps;
            int acc = 0;
            int big = 0;

            for (int k = 0; k < kTaps; ++k)
            {
                row[k] = static_cast<int16_t>(std::lround(raw[k] / sum * kWeightOne));
                acc += row[k];
                if (row[k] > row[big])
                    big = k;
            }
            // Force an exact sum of 1.0 in Q14 so a flat region cannot drift a level.
            row[big] = static_cast<int16_t>(row[big] + (kWeightOne - acc));
        }
    }

    //------------------------------------------------------------------------------
    // Horizontal pass: one source row -> one row of int16 channel values
    //------------------------------------------------------------------------------

    uint32_t PackClamp(int b, int g, int r, int a)
    {
        b = std::clamp(b, 0, 255);
        g = std::clamp(g, 0, 255);
        r = std::clamp(r, 0, 255);
        a = std::clamp(a, 0, 255);
        return static_cast<uint32_t>(b) | (static_cast<uint32_t>(g) << 8) | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(a) << 24);
    }

    // The output is deliberately left unclamped: the cubic's negative outer taps
    // overshoot 0..255 and that overshoot is the sharpening.  It stays inside int16
    // (about -0.13..1.13 of full scale at this Q14 scaling) and is clamped once,
    // in the vertical pass.
    void HorizRow(const uint32_t* srow, int srcW, const WeightTable& wx, int dstW, int16_t* out)
    {
        for (int i = 0; i < dstW; ++i)
        {
            const int16_t* w = wx.w.data() + static_cast<size_t>(i) * kTaps;
            const int x0 = wx.first[static_cast<size_t>(i)];

            uint32_t p[kTaps];
            if (x0 >= 0 && x0 + kTaps - 1 < srcW)
            {
                for (int k = 0; k < kTaps; ++k)
                    p[k] = srow[x0 + k];
            }
            else
            {
                for (int k = 0; k < kTaps; ++k)
                    p[k] = srow[std::clamp(x0 + k, 0, srcW - 1)];
            }

            for (int ch = 0; ch < 4; ++ch)
            {
                const int shift = ch * 8;
                int acc = 0;
                for (int k = 0; k < kTaps; ++k)
                    acc += w[k] * static_cast<int>((p[k] >> shift) & 0xFF);
                out[i * 4 + ch] = static_cast<int16_t>(acc >> kMidShift);
            }
        }
    }

    //------------------------------------------------------------------------------
    // Vertical pass
    //
    // This is where the work is: the horizontal pass runs once per source row, the
    // vertical pass once per destination row, so at 4x zoom the vertical pass
    // touches four times the pixels.  It also vectorizes with no gathers - two
    // contiguous int16 rows combined with a constant weight pair.
    //------------------------------------------------------------------------------

    void VertRow4TapScalar(const int16_t* const* rows, const int16_t* w, int n, uint32_t* drow)
    {
        for (int i = 0; i < n; ++i)
        {
            int ch[4] = {};
            for (int k = 0; k < kTaps; ++k)
                for (int c = 0; c < 4; ++c)
                    ch[c] += w[k] * rows[k][i * 4 + c];

            drow[i] = PackClamp((ch[0] + kFinalRound) >> kFinalShift,
                                (ch[1] + kFinalRound) >> kFinalShift,
                                (ch[2] + kFinalRound) >> kFinalShift,
                                (ch[3] + kFinalRound) >> kFinalShift);
        }
    }

#if ZI_SSE2
    // Four destination pixels per iteration.  _mm_madd_epi16 multiplies vertically
    // and sums adjacent pairs, which is w0*a + w1*b once two rows are interleaved;
    // 4 taps is two of those plus an add.  _mm_packus_epi16 at the end is the
    // 0..255 clamp the cubic's overshoot needs.  SSE2 only, so no feature probe.
    void VertRow4TapSse2(const int16_t* const* rows, const int16_t* w, int n, uint32_t* drow)
    {
        const __m128i w01 = _mm_set1_epi32((w[1] << 16) | (w[0] & 0xFFFF));
        const __m128i w23 = _mm_set1_epi32((w[3] << 16) | (w[2] & 0xFFFF));
        const __m128i rnd = _mm_set1_epi32(kFinalRound);

        int i = 0;
        for (; i + 4 <= n; i += 4)
        {
            const size_t off = static_cast<size_t>(i) * 4;
            const __m128i a0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(rows[0] + off));
            const __m128i b0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(rows[1] + off));
            const __m128i c0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(rows[2] + off));
            const __m128i d0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(rows[3] + off));
            const __m128i a1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(rows[0] + off + 8));
            const __m128i b1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(rows[1] + off + 8));
            const __m128i c1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(rows[2] + off + 8));
            const __m128i d1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(rows[3] + off + 8));

            __m128i p0 = _mm_add_epi32(_mm_madd_epi16(_mm_unpacklo_epi16(a0, b0), w01),
                                       _mm_madd_epi16(_mm_unpacklo_epi16(c0, d0), w23));
            __m128i p1 = _mm_add_epi32(_mm_madd_epi16(_mm_unpackhi_epi16(a0, b0), w01),
                                       _mm_madd_epi16(_mm_unpackhi_epi16(c0, d0), w23));
            __m128i p2 = _mm_add_epi32(_mm_madd_epi16(_mm_unpacklo_epi16(a1, b1), w01),
                                       _mm_madd_epi16(_mm_unpacklo_epi16(c1, d1), w23));
            __m128i p3 = _mm_add_epi32(_mm_madd_epi16(_mm_unpackhi_epi16(a1, b1), w01),
                                       _mm_madd_epi16(_mm_unpackhi_epi16(c1, d1), w23));

            p0 = _mm_srai_epi32(_mm_add_epi32(p0, rnd), kFinalShift);
            p1 = _mm_srai_epi32(_mm_add_epi32(p1, rnd), kFinalShift);
            p2 = _mm_srai_epi32(_mm_add_epi32(p2, rnd), kFinalShift);
            p3 = _mm_srai_epi32(_mm_add_epi32(p3, rnd), kFinalShift);

            const __m128i s0 = _mm_packs_epi32(p0, p1);
            const __m128i s1 = _mm_packs_epi32(p2, p3);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(drow + i),
                             _mm_packus_epi16(s0, s1));
        }
        if (i < n)
        {
            const int16_t* tail[kTaps];
            for (int k = 0; k < kTaps; ++k)
                tail[k] = rows[k] + static_cast<size_t>(i) * 4;
            VertRow4TapScalar(tail, w, n - i, drow + i);
        }
    }
#endif // ZI_SSE2

#if ZI_NEON
    // Four destination pixels per iteration.  vmull/vmlal give the widening
    // multiply-accumulate, vrshrq the rounding shift, and vqmovun/vqmovn the
    // saturating narrow - vqmovun clamps the cubic's negative overshoot to 0 and
    // vqmovn the positive overshoot to 255, so the clamp stays free exactly as it
    // is on SSE2.  All base ARMv8-A.
    void VertRow4TapNeon(const int16_t* const* rows, const int16_t* w, int n, uint32_t* drow)
    {
        const int16_t c0 = w[0], c1 = w[1], c2 = w[2], c3 = w[3];

        int i = 0;
        for (; i + 4 <= n; i += 4)
        {
            const size_t off = static_cast<size_t>(i) * 4;
            const int16x8_t a0 = vld1q_s16(rows[0] + off);
            const int16x8_t b0 = vld1q_s16(rows[1] + off);
            const int16x8_t c0v = vld1q_s16(rows[2] + off);
            const int16x8_t d0 = vld1q_s16(rows[3] + off);
            const int16x8_t a1 = vld1q_s16(rows[0] + off + 8);
            const int16x8_t b1 = vld1q_s16(rows[1] + off + 8);
            const int16x8_t c1v = vld1q_s16(rows[2] + off + 8);
            const int16x8_t d1 = vld1q_s16(rows[3] + off + 8);

            int32x4_t r0 = vmull_n_s16(vget_low_s16(a0), c0);
            r0 = vmlal_n_s16(r0, vget_low_s16(b0), c1);
            r0 = vmlal_n_s16(r0, vget_low_s16(c0v), c2);
            r0 = vmlal_n_s16(r0, vget_low_s16(d0), c3);

            int32x4_t r1 = vmull_n_s16(vget_high_s16(a0), c0);
            r1 = vmlal_n_s16(r1, vget_high_s16(b0), c1);
            r1 = vmlal_n_s16(r1, vget_high_s16(c0v), c2);
            r1 = vmlal_n_s16(r1, vget_high_s16(d0), c3);

            int32x4_t r2 = vmull_n_s16(vget_low_s16(a1), c0);
            r2 = vmlal_n_s16(r2, vget_low_s16(b1), c1);
            r2 = vmlal_n_s16(r2, vget_low_s16(c1v), c2);
            r2 = vmlal_n_s16(r2, vget_low_s16(d1), c3);

            int32x4_t r3 = vmull_n_s16(vget_high_s16(a1), c0);
            r3 = vmlal_n_s16(r3, vget_high_s16(b1), c1);
            r3 = vmlal_n_s16(r3, vget_high_s16(c1v), c2);
            r3 = vmlal_n_s16(r3, vget_high_s16(d1), c3);

            const uint16x8_t s0 = vcombine_u16(vqmovun_s32(vrshrq_n_s32(r0, kFinalShift)),
                                               vqmovun_s32(vrshrq_n_s32(r1, kFinalShift)));
            const uint16x8_t s1 = vcombine_u16(vqmovun_s32(vrshrq_n_s32(r2, kFinalShift)),
                                               vqmovun_s32(vrshrq_n_s32(r3, kFinalShift)));

            vst1q_u8(reinterpret_cast<uint8_t*>(drow + i),
                     vcombine_u8(vqmovn_u16(s0), vqmovn_u16(s1)));
        }
        if (i < n)
        {
            const int16_t* tail[kTaps];
            for (int k = 0; k < kTaps; ++k)
                tail[k] = rows[k] + static_cast<size_t>(i) * 4;
            VertRow4TapScalar(tail, w, n - i, drow + i);
        }
    }
#endif // ZI_NEON

#if ZI_AVX2
    // Eight destination pixels per iteration - the same shape as the SSE2 kernel
    // in 256-bit registers.
    //
    // The 256-bit unpack/pack instructions work within each 128-bit lane rather
    // than across the register, so the eight pixels come out interleaved as
    // 0,1,4,5,2,3,6,7.  One _mm256_permute4x64_epi64 puts the four 64-bit halves
    // back in order; doing it once at the end is cheaper than shuffling the inputs.
    //
    // Selected only when CpuHasAvx2() says so - see the note there.
    void VertRow4TapAvx2(const int16_t* const* rows, const int16_t* w, int n, uint32_t* drow)
    {
        const __m256i w01 = _mm256_set1_epi32((w[1] << 16) | (w[0] & 0xFFFF));
        const __m256i w23 = _mm256_set1_epi32((w[3] << 16) | (w[2] & 0xFFFF));
        const __m256i rnd = _mm256_set1_epi32(kFinalRound);

        int i = 0;
        for (; i + 8 <= n; i += 8)
        {
            const size_t off = static_cast<size_t>(i) * 4;
            const __m256i a0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rows[0] + off));
            const __m256i b0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rows[1] + off));
            const __m256i c0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rows[2] + off));
            const __m256i d0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rows[3] + off));
            const __m256i a1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rows[0] + off + 16));
            const __m256i b1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rows[1] + off + 16));
            const __m256i c1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rows[2] + off + 16));
            const __m256i d1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rows[3] + off + 16));

            __m256i p0 = _mm256_add_epi32(_mm256_madd_epi16(_mm256_unpacklo_epi16(a0, b0), w01),
                                          _mm256_madd_epi16(_mm256_unpacklo_epi16(c0, d0), w23));
            __m256i p1 = _mm256_add_epi32(_mm256_madd_epi16(_mm256_unpackhi_epi16(a0, b0), w01),
                                          _mm256_madd_epi16(_mm256_unpackhi_epi16(c0, d0), w23));
            __m256i p2 = _mm256_add_epi32(_mm256_madd_epi16(_mm256_unpacklo_epi16(a1, b1), w01),
                                          _mm256_madd_epi16(_mm256_unpacklo_epi16(c1, d1), w23));
            __m256i p3 = _mm256_add_epi32(_mm256_madd_epi16(_mm256_unpackhi_epi16(a1, b1), w01),
                                          _mm256_madd_epi16(_mm256_unpackhi_epi16(c1, d1), w23));

            p0 = _mm256_srai_epi32(_mm256_add_epi32(p0, rnd), kFinalShift);
            p1 = _mm256_srai_epi32(_mm256_add_epi32(p1, rnd), kFinalShift);
            p2 = _mm256_srai_epi32(_mm256_add_epi32(p2, rnd), kFinalShift);
            p3 = _mm256_srai_epi32(_mm256_add_epi32(p3, rnd), kFinalShift);

            const __m256i s0 = _mm256_packs_epi32(p0, p1);
            const __m256i s1 = _mm256_packs_epi32(p2, p3);
            const __m256i u = _mm256_packus_epi16(s0, s1);

            _mm256_storeu_si256(reinterpret_cast<__m256i*>(drow + i),
                                _mm256_permute4x64_epi64(u, _MM_SHUFFLE(3, 1, 2, 0)));
        }
        if (i < n)
        {
            const int16_t* tail[kTaps];
            for (int k = 0; k < kTaps; ++k)
                tail[k] = rows[k] + static_cast<size_t>(i) * 4;
            VertRow4TapSse2(tail, w, n - i, drow + i);
        }
    }

    // CPUID leaf 7, EBX bit 5.  Probed once - CPUID is serializing and this is
    // consulted once per band.  PowerToys requires Windows 10 build 19041 or
    // later, and every such OS enables AVX register state whenever the CPU has
    // it, so the CPU feature bit alone is the whole question.
    bool CpuHasAvx2()
    {
        static const bool has = [] {
            int info[4] = {};
            __cpuid(info, 0);
            if (info[0] < 7)
                return false;

            __cpuidex(info, 7, 0);
            return (info[1] & (1 << 5)) != 0;
        }();
        return has;
    }
#endif // ZI_AVX2

    using VertRowFn = void (*)(const int16_t* const* rows, const int16_t* w, int n, uint32_t* drow);

    // The best vertical-pass kernel for this build and this CPU.
    VertRowFn PickVertRow()
    {
#if ZI_AVX2
        if (CpuHasAvx2())
            return VertRow4TapAvx2;
#endif
#if ZI_SSE2
        return VertRow4TapSse2;
#elif ZI_NEON
        return VertRow4TapNeon;
#else
        return VertRow4TapScalar;
#endif
    }

    //------------------------------------------------------------------------------
    // Fused two-pass over a band of destination rows
    //
    // Writing a full-frame intermediate would cost 33 MB stored and 66 MB re-read
    // at 4K, all of it DRAM traffic.  A destination row only ever needs kTaps
    // horizontally-filtered source rows, and the window slides by at most one as
    // the destination row advances, so a ring of kTaps rows is enough - 61 KB at
    // 4K wide, which stays resident in L2.
    //------------------------------------------------------------------------------

    struct ScaleJob
    {
        const uint32_t* srcBits = nullptr;
        int srcStride = 0; // in pixels
        int srcW = 0;
        int srcH = 0;
        uint32_t* dstBits = nullptr;
        int dstStride = 0; // in pixels
        int dstW = 0;
        int dstH = 0;
        const WeightTable* wx = nullptr;
        const WeightTable* wy = nullptr;
    };

    void ScaleBand(const ScaleJob& job, int jBegin, int jEnd)
    {
        const WeightTable& wx = *job.wx;
        const WeightTable& wy = *job.wy;

        const size_t stride = static_cast<size_t>(job.dstW) * 4;
        const VertRowFn vertRow = PickVertRow();

        std::vector<int16_t> ring(stride * kTaps);
        int held[kTaps] = { INT_MIN, INT_MIN, INT_MIN, INT_MIN };
        const int16_t* rows[kTaps] = {};

        for (int j = jBegin; j < jEnd; ++j)
        {
            const int16_t* w = wy.w.data() + static_cast<size_t>(j) * kTaps;
            const int y0 = wy.first[static_cast<size_t>(j)];

            for (int k = 0; k < kTaps; ++k)
            {
                const int sy = std::clamp(y0 + k, 0, job.srcH - 1);
                const int slot = ((y0 + k) % kTaps + kTaps) % kTaps;

                if (held[slot] != sy)
                {
                    HorizRow(job.srcBits + static_cast<size_t>(sy) * job.srcStride,
                             job.srcW,
                             wx,
                             job.dstW,
                             ring.data() + static_cast<size_t>(slot) * stride);
                    held[slot] = sy;
                }
                rows[k] = ring.data() + static_cast<size_t>(slot) * stride;
            }

            uint32_t* drow = job.dstBits + static_cast<size_t>(j) * job.dstStride;

            vertRow(rows, w, job.dstW, drow);
        }
    }

    //------------------------------------------------------------------------------
    // Threading
    //
    // ZoomIt paints on the UI thread, so bands are handed to the process thread
    // pool rather than to freshly created threads - at a 20 ms repaint interval,
    // per-frame thread creation would be pure overhead.
    //------------------------------------------------------------------------------

    struct BandWork
    {
        const ScaleJob* job = nullptr;
        int bands = 0;
        int rows = 0;
        LONG next = 0;
    };

    VOID CALLBACK BandCallback(PTP_CALLBACK_INSTANCE, PVOID context, PTP_WORK)
    {
        BandWork* bw = static_cast<BandWork*>(context);

        for (;;)
        {
            const LONG idx = InterlockedIncrement(&bw->next) - 1;
            if (idx >= bw->bands)
                break;

            const int b = idx * bw->rows;
            const int e = (std::min)(b + bw->rows, bw->job->dstH);
            if (b < e)
                ScaleBand(*bw->job, b, e);
        }
    }

    bool RunScale(const ScaleJob& job)
    {
        SYSTEM_INFO si;
        GetSystemInfo(&si);

        int threads = static_cast<int>(si.dwNumberOfProcessors);
        threads = std::clamp(threads, 1, (std::max)(1, job.dstH / kMinRowsPerThread));

        if (threads <= 1)
        {
            ScaleBand(job, 0, job.dstH);
            return true;
        }

        BandWork bw;
        bw.job = &job;
        bw.rows = (job.dstH + threads - 1) / threads;
        bw.bands = (job.dstH + bw.rows - 1) / bw.rows;
        bw.next = 0;

        PTP_WORK work = CreateThreadpoolWork(BandCallback, &bw, nullptr);
        if (work == nullptr)
            return false;

        // The workers hold a pointer to bw on this stack frame, so they must be
        // waited for on every exit, including an exception from this thread's
        // own share of the work.
        struct Finish
        {
            PTP_WORK w;
            ~Finish()
            {
                WaitForThreadpoolWorkCallbacks(w, FALSE);
                CloseThreadpoolWork(w);
            }
        } finish{ work };

        // One submission short of the band count: this thread takes a share too.
        for (int t = 1; t < threads; ++t)
            SubmitThreadpoolWork(work);

        BandCallback(nullptr, &bw, work);
        return true;
    }

    //------------------------------------------------------------------------------
    // Cached scratch surfaces
    //------------------------------------------------------------------------------

    // A 32-bpp top-down BGRA DIB section plus the DC it is selected into.
    struct Surface
    {
        HDC dc = nullptr;
        HBITMAP bmp = nullptr;
        HGDIOBJ prev = nullptr;
        uint32_t* bits = nullptr;
        int w = 0;
        int h = 0;

        void Release()
        {
            if (dc != nullptr)
            {
                if (prev != nullptr)
                    SelectObject(dc, prev);
                DeleteDC(dc);
            }
            if (bmp != nullptr)
                DeleteObject(bmp);
            dc = nullptr;
            bmp = nullptr;
            prev = nullptr;
            bits = nullptr;
            w = 0;
            h = 0;
        }

        // Keeps the surface if it is already large enough: the source region
        // changes size on every animation frame, and reallocating a 20 MB DIB
        // section per frame (with the page faults that follow) is not free.
        bool Ensure(HDC reference, int width, int height)
        {
            if (bits != nullptr && w >= width && h >= height)
                return true;
            Release();

            if (width <= 0 || height <= 0)
                return false;

            BITMAPINFO bi = {};
            bi.bmiHeader.biSize = sizeof bi.bmiHeader;
            bi.bmiHeader.biWidth = width;
            bi.bmiHeader.biHeight = -height; // top-down
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;

            void* raw = nullptr;
            bmp = CreateDIBSection(reference, &bi, DIB_RGB_COLORS, &raw, nullptr, 0);
            if (bmp == nullptr || raw == nullptr)
            {
                Release();
                return false;
            }

            dc = CreateCompatibleDC(reference);
            if (dc == nullptr)
            {
                Release();
                return false;
            }

            prev = SelectObject(dc, bmp);
            bits = static_cast<uint32_t*>(raw);
            w = width;
            h = height;
            return true;
        }
    };

    // Painting is single-threaded, so plain statics are sufficient here.
    struct ScaleCache
    {
        Surface scratch; // readable mirror of the source region
        Surface dest; // scaled output, blitted to the caller's DC
        WeightTable wx;
        WeightTable wy;

        // Geometry the weight tables were built for.
        int keyDstW = 0, keyDstH = 0;
        int keySrcW = 0, keySrcH = 0;
        bool keyValid = false;

        bool WeightsMatch(int dstW, int dstH, int srcW, int srcH) const
        {
            return keyValid && keyDstW == dstW && keyDstH == dstH && keySrcW == srcW && keySrcH == srcH;
        }

        void SetKey(int dstW, int dstH, int srcW, int srcH)
        {
            keyDstW = dstW;
            keyDstH = dstH;
            keySrcW = srcW;
            keySrcH = srcH;
            keyValid = true;
        }
    };

    ScaleCache g_cache;

} // namespace

//------------------------------------------------------------------------------

BOOL SmoothStretchBlt(HDC hdcDst, int dstX, int dstY, int dstW, int dstH, HDC hdcSrc, int srcX, int srcY, int srcW, int srcH)
{
    if (hdcDst == nullptr || hdcSrc == nullptr)
        return FALSE;
    if (dstW <= 0 || dstH <= 0 || srcW <= 0 || srcH <= 0)
        return FALSE;

    // Magnification only: a minification needs a wider kernel to avoid aliasing.
    // At 1:1 the cubic is not an identity (its integer-offset taps are
    // [1/36, 17/18, 1/36]), and ZoomIt does paint 1:1 while zoomed, at the end
    // of a zoom-out.  Copy instead.
    if (dstW == srcW && dstH == srcH)
        return BitBlt(hdcDst, dstX, dstY, dstW, dstH, hdcSrc, srcX, srcY, SRCCOPY);

    if (dstW < srcW || dstH < srcH)
        return FALSE;

    // Mirror the source region into a readable DIB.  ZoomIt's source is a
    // device-dependent bitmap, so its bits cannot be addressed directly.
    if (!g_cache.scratch.Ensure(hdcDst, srcW, srcH))
        return FALSE;
    if (!BitBlt(g_cache.scratch.dc, 0, 0, srcW, srcH, hdcSrc, srcX, srcY, SRCCOPY))
        return FALSE;

    if (!g_cache.dest.Ensure(hdcDst, dstW, dstH))
        return FALSE;

    // Weight tables depend only on the geometry, so a steady stream of frames
    // at one zoom level rebuilds nothing.
    if (!g_cache.WeightsMatch(dstW, dstH, srcW, srcH))
    {
        BuildWeights(g_cache.wx, dstW, static_cast<double>(srcW));
        BuildWeights(g_cache.wy, dstH, static_cast<double>(srcH));
        g_cache.SetKey(dstW, dstH, srcW, srcH);
    }

    // The bits are read directly, so GDI's per-thread batch has to be flushed
    // first or the frame can miss the most recently drawn annotations.
    GdiFlush();

    ScaleJob job;
    job.srcBits = g_cache.scratch.bits;
    job.srcStride = g_cache.scratch.w;
    job.srcW = srcW;
    job.srcH = srcH;
    job.dstBits = g_cache.dest.bits;
    job.dstStride = g_cache.dest.w;
    job.dstW = dstW;
    job.dstH = dstH;
    job.wx = &g_cache.wx;
    job.wy = &g_cache.wy;

    if (!RunScale(job))
        return FALSE;

    return BitBlt(hdcDst, dstX, dstY, dstW, dstH, g_cache.dest.dc, 0, 0, SRCCOPY);
}

void SmoothStretchPrepare(HDC hdc, int width, int height)
{
    if (hdc == nullptr || width <= 0 || height <= 0)
        return;

    // Touch every page as well: a fresh DIB section is only reserved, and
    // faulting 50 MB in during the first frame costs as much as the resample.
    for (Surface* s : { &g_cache.scratch, &g_cache.dest })
    {
        if (s->Ensure(hdc, width, height))
            memset(s->bits, 0, static_cast<size_t>(s->w) * s->h * 4);
    }
}

void SmoothStretchRelease()
{
    g_cache.scratch.Release();
    g_cache.dest.Release();
    g_cache.wx = WeightTable();
    g_cache.wy = WeightTable();
    g_cache.keyValid = false;
}
