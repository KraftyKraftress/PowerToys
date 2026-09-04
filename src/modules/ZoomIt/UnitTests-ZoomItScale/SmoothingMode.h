//==============================================================================
//
// SmoothingMode.h
//
// The ways ZoomIt can paint a magnified frame, selectable at run time so one
// binary can exercise all of them.
//
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
//==============================================================================
#pragma once

#include <windows.h>

#include <string>

enum class SmoothingMode
{
    Off, // "Smooth the zoomed image" unchecked: COLORONCOLOR StretchBlt
    Halftone, // checked, as previously shipped: HALFTONE StretchBlt
    New, // checked: SmoothStretchBlt
};

const wchar_t* ModeName(SmoothingMode mode);

// Case-insensitive; returns false for an unknown name so the caller can report
// it rather than guess.
bool TryParseMode(const std::wstring& text, SmoothingMode& mode);

// Paints one magnified frame the way ZoomIt's WM_PAINT would for `mode`.
// Returns what the underlying blit returned; there is no fallback between
// modes, so a FALSE means that mode did not draw.
BOOL RenderZoom(SmoothingMode mode,
                HDC hdcDst,
                int dstW,
                int dstH,
                HDC hdcSrc,
                int srcX,
                int srcY,
                int srcW,
                int srcH);
