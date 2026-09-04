//==============================================================================
//
// ScaleMetrics.h
//
// Full-reference image comparison for the zoomed-image smoothing paths.
//
// PSNR alone is a poor way to compare two *different* resampling filters: they
// disagree most exactly where the detail is, so a large per-pixel error can
// mean "sharper" as easily as "wrong".  Three published metrics are used
// together instead, each answering a different question:
//
//   RMSE / PSNR   How far apart are they numerically.  Reported as
//          rmsePercent (root-mean-square pixel error as a percentage of the
//          0..255 range: 0% identical, unbounded above) and as psnrDb, the
//          same number on a log scale.  Baseline only; there is no standard
//          threshold for "close enough" on either scale.
//
//   SSIM   Structural similarity, Wang, Bovik, Sheikh & Simoncelli, "Image
//          Quality Assessment: From Error Visibility to Structural
//          Similarity", IEEE TIP 13(4), 2004.  Luminance/contrast/structure
//          over a local Gaussian window - insensitive to a uniform brightness
//          or contrast shift, sensitive to structural damage.  Range 0..1
//          (negative is possible in theory, never seen on real images);
//          1.0 = identical.
//
//   GMSD   Gradient Magnitude Similarity Deviation, Xue, Zhang, Mou & Bovik,
//          IEEE TIP 23(2), 2014 (arXiv:1308.3052).  Prewitt gradient
//          magnitudes compared pixel-wise, then pooled by *standard
//          deviation* rather than mean, which is what makes it sensitive to
//          localized edge degradation - precisely the artefact that matters
//          for text.  Range 0..0.5 (a standard deviation of values in 0..1);
//          0.0 = identical, higher = more edge distortion.
//
// Plus edge/ink statistics, because the question "does one filter render
// strokes darker than the other" is a contrast question that none of the
// three similarity indices is designed to answer.
//
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
//==============================================================================
#pragma once

#include <cstdint>
#include <vector>

// 32-bpp BGRA, top-down, tightly packed.
struct Image
{
    int w = 0;
    int h = 0;
    std::vector<uint32_t> px;

    bool Valid() const { return w > 0 && h > 0 && px.size() == static_cast<size_t>(w) * h; }
};

struct Similarity
{
    // rmsePercent and psnrDb are the same pixel-error measurement on two
    // scales: PSNR = 20 log10( 255 / RMSE ).
    double rmsePercent = 0; // RMSE as % of 0..255; 0 identical
    double psnrDb = 0; // higher is closer; 99 = identical by convention
    int maxChannelDelta = 0; // 0..255, worst single channel anywhere
    double ssim = 0; // 0..1, 1 identical
    double gmsd = 0; // 0..0.5, 0 identical
};

Similarity Compare(const Image& a, const Image& b);

// Single-image descriptive statistics.  `inkThreshold` is the luma below which
// a pixel counts as part of a glyph stroke.
struct InkStats
{
    double meanLuma = 0; // whole image
    int minLuma = 255;
    double inkCoverage = 0; // fraction of pixels below the threshold
    double inkMeanLuma = 0; // mean luma of those pixels - "how dark are strokes"
    double meanGradient = 0; // mean Prewitt gradient magnitude - "how crisp"
};

InkStats MeasureInk(const Image& im, int inkThreshold = 128);

// Stroke darkness of `cand` relative to `ref`, measured over the pixels that
// are ink in `ref` only.  Comparing each image's own MeasureInk would compare
// different pixel sets, and at low coverage a few pixels crossing the threshold
// move the mean more than the effect being measured.
struct InkDelta
{
    double refMean = 0; // mean luma of ref over its ink mask
    double candMean = 0; // mean luma of cand over that same mask
    double delta = 0; // candMean - refMean; positive means lighter strokes
    double coverage = 0; // fraction of the image in the mask
};

InkDelta CompareInk(const Image& ref, const Image& cand, int inkThreshold = 128);
