//==============================================================================
//
// GdiSurface.h
//
// GDI helpers shared by the tests and the bench tool: an owning screen DC, a
// bitmap-plus-DC pair, and the zoom geometry both use to magnify a screenshot.
//
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
//==============================================================================
#pragma once

#include <windows.h>

#include <algorithm>
#include <cstdint>

#include "ScaleMetrics.h"

// GetDC(nullptr), released on scope exit.  A colour reference DC is needed to
// create device-dependent bitmaps like ZoomIt's hbmpCompat.
struct ScreenDc
{
    HDC dc = GetDC(nullptr);

    ScreenDc() = default;
    ScreenDc(const ScreenDc&) = delete;
    ScreenDc& operator=(const ScreenDc&) = delete;
    ~ScreenDc()
    {
        if (dc != nullptr)
            ReleaseDC(nullptr, dc);
    }
};

// A bitmap selected into a memory DC.  Dib() makes a 32-bpp top-down DIB whose
// bits are readable through `bits`; Ddb() makes a device-dependent bitmap, the
// kind ZoomIt keeps its screen copy in, whose bits are not.
struct Surface
{
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ prev = nullptr;
    uint32_t* bits = nullptr;
    int w = 0, h = 0;

    Surface() = default;
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;

    bool Dib(HDC ref, int width, int height)
    {
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = width;
        bi.bmiHeader.biHeight = -height;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* raw = nullptr;
        bmp = CreateDIBSection(ref, &bi, DIB_RGB_COLORS, &raw, nullptr, 0);
        if (bmp == nullptr)
            return false;
        dc = CreateCompatibleDC(ref);
        if (dc == nullptr)
            return false;
        prev = SelectObject(dc, bmp);
        bits = static_cast<uint32_t*>(raw);
        w = width;
        h = height;
        return true;
    }

    bool Ddb(HDC ref, int width, int height)
    {
        bmp = CreateCompatibleBitmap(ref, width, height);
        if (bmp == nullptr)
            return false;
        dc = CreateCompatibleDC(ref);
        if (dc == nullptr)
            return false;
        prev = SelectObject(dc, bmp);
        w = width;
        h = height;
        return true;
    }

    // Copies the DIB's pixels out; empty for a DDB.
    Image Snapshot() const
    {
        Image im;
        if (bits == nullptr)
            return im;
        im.w = w;
        im.h = h;
        im.px.assign(bits, bits + static_cast<size_t>(w) * h);
        return im;
    }

    ~Surface()
    {
        if (dc != nullptr)
        {
            if (prev != nullptr)
                SelectObject(dc, prev);
            DeleteDC(dc);
        }
        if (bmp != nullptr)
            DeleteObject(bmp);
    }
};

// The source region of a screenshot that fills the whole frame at `zoom`.
struct Geometry
{
    int dstW, dstH, srcX, srcY, srcW, srcH;
};

// Centred on (atX, atY), or on the middle of the frame when either is
// negative.  The region is clamped to the screenshot, as ZoomIt clamps its
// own zoom window to the screen.
inline Geometry GeometryFor(const Image& src, double zoom, int atX = -1, int atY = -1)
{
    Geometry g;
    g.dstW = src.w;
    g.dstH = src.h;
    g.srcW = static_cast<int>(src.w / zoom);
    g.srcH = static_cast<int>(src.h / zoom);

    const int cx = atX >= 0 ? atX : src.w / 2;
    const int cy = atY >= 0 ? atY : src.h / 2;

    g.srcX = std::clamp(cx - g.srcW / 2, 0, src.w - g.srcW);
    g.srcY = std::clamp(cy - g.srcH / 2, 0, src.h - g.srcH);
    return g;
}
