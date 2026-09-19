//==============================================================================
//
// SmoothingMode.cpp
//
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
//==============================================================================
#include "SmoothingMode.h"

#include "ZoomItScale.h"

#include <algorithm>
#include <cwctype>

namespace
{
    struct Alias
    {
        const wchar_t* name;
        SmoothingMode mode;
    };

    // The first entry for each mode is the name ModeName returns.
    const Alias kAliases[] = {
        { L"off", SmoothingMode::Off },
        { L"none", SmoothingMode::Off },
        { L"halftone", SmoothingMode::Halftone },
        { L"old", SmoothingMode::Halftone },
        { L"new", SmoothingMode::New },
        { L"smooth", SmoothingMode::New },
    };

    std::wstring Lower(const std::wstring& s)
    {
        std::wstring out = s;
        std::transform(out.begin(), out.end(), out.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        return out;
    }
}

const wchar_t* ModeName(SmoothingMode mode)
{
    for (const Alias& a : kAliases)
        if (a.mode == mode)
            return a.name;
    return L"?";
}

bool TryParseMode(const std::wstring& text, SmoothingMode& mode)
{
    const std::wstring t = Lower(text);
    for (const Alias& a : kAliases)
        if (t == a.name)
        {
            mode = a.mode;
            return true;
        }
    return false;
}

BOOL RenderZoom(SmoothingMode mode,
                HDC hdcDst,
                int dstW,
                int dstH,
                HDC hdcSrc,
                int srcX,
                int srcY,
                int srcW,
                int srcH)
{
    if (mode == SmoothingMode::New)
    {
        return SmoothStretchBlt(hdcDst, 0, 0, dstW, dstH, hdcSrc, srcX, srcY, srcW, srcH);
    }

    SetStretchBltMode(hdcDst, mode == SmoothingMode::Off ? COLORONCOLOR : HALFTONE);
    if (mode != SmoothingMode::Off)
    {
        SetBrushOrgEx(hdcDst, 0, 0, nullptr);
    }

    return StretchBlt(hdcDst, 0, 0, dstW, dstH, hdcSrc, srcX, srcY, srcW, srcH, SRCCOPY);
}
