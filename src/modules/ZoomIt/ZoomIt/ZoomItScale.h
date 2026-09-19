//==============================================================================
//
// ZoomItScale.h
//
// Smooth magnification for the zoomed-image path.
//
// Replaces SetStretchBltMode( HALFTONE ) + StretchBlt, a single-threaded scalar
// path inside gdi32 that takes 40-77 ms per frame at 3840x2160 against the
// 20 ms ZOOM_LEVEL_STEP_TIME repaint interval.  SmoothStretchBlt is a
// separable Mitchell-Netravali cubic, fixed point, split across the thread
// pool, using SSE2 (x64/x86, with AVX2 when the CPU has it) or NEON (ARM64).
//
// Magnification only.  ZoomIt clamps zoomLevel to ZOOM_LEVEL_MIN, so the
// destination is never smaller than the source; at exactly 1:1 the region
// is copied unchanged.
//
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
//==============================================================================
#pragma once

#include <windows.h>

// Scales the srcW x srcH region at (srcX, srcY) of hdcSrc up to dstW x dstH on
// hdcDst.  Scratch surfaces and filter weights are cached across calls and
// rebuilt only when the geometry changes.
//
// Returns FALSE, having drawn nothing, for a null DC, a degenerate rectangle,
// a minification, or an allocation failure - the same contract as StretchBlt,
// and ZoomIt treats the two the same way.  Not thread-safe: ZoomIt paints on
// the UI thread.
BOOL SmoothStretchBlt(HDC hdcDst, int dstX, int dstY, int dstW, int dstH, HDC hdcSrc, int srcX, int srcY, int srcW, int srcH);

// Allocates the scratch surfaces for a width x height source and destination
// ahead of the first frame, so the zoom-in animation's first frame does not
// pay for two DIB sections and their page faults.  Optional; SmoothStretchBlt
// allocates on demand.
void SmoothStretchPrepare(HDC hdc, int width, int height);

// Frees the cached surfaces and weight tables.  Call when leaving zoom mode.
void SmoothStretchRelease();
