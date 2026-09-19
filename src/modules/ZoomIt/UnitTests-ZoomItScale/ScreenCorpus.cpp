//==============================================================================
//
// ScreenCorpus.cpp
//
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
//==============================================================================
#include "ScreenCorpus.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>

using Microsoft::WRL::ComPtr;

namespace
{
    struct ComScope
    {
        bool owned = false;
        ComScope()
        {
            const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            owned = SUCCEEDED(hr);
        }
        ~ComScope()
        {
            if (owned)
                CoUninitialize();
        }
    };

    ComPtr<IWICImagingFactory> Factory()
    {
        ComPtr<IWICImagingFactory> f;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
        return f;
    }
}

bool LoadPng(const std::wstring& path, Image& out)
{
    ComScope com;
    auto factory = Factory();
    if (!factory)
        return false;

    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec)))
        return false;

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame)))
        return false;

    ComPtr<IWICFormatConverter> conv;
    if (FAILED(factory->CreateFormatConverter(&conv)))
        return false;
    if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
        return false;

    UINT w = 0, h = 0;
    if (FAILED(conv->GetSize(&w, &h)) || w == 0 || h == 0)
        return false;

    out.w = static_cast<int>(w);
    out.h = static_cast<int>(h);
    out.px.resize(static_cast<size_t>(w) * h);

    const UINT stride = w * 4;
    const UINT bytes = stride * h;
    return SUCCEEDED(conv->CopyPixels(nullptr, stride, bytes, reinterpret_cast<BYTE*>(out.px.data())));
}

bool SavePng(const std::wstring& path, const Image& im)
{
    if (!im.Valid())
        return false;

    ComScope com;
    auto factory = Factory();
    if (!factory)
        return false;

    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream)))
        return false;
    if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)))
        return false;

    ComPtr<IWICBitmapEncoder> enc;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)))
        return false;
    if (FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)))
        return false;

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(enc->CreateNewFrame(&frame, &props)))
        return false;
    if (FAILED(frame->Initialize(props.Get())))
        return false;
    if (FAILED(frame->SetSize(static_cast<UINT>(im.w), static_cast<UINT>(im.h))))
        return false;

    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&fmt)))
        return false;

    const UINT stride = static_cast<UINT>(im.w) * 4;
    if (FAILED(frame->WritePixels(static_cast<UINT>(im.h), stride, stride * im.h, reinterpret_cast<BYTE*>(const_cast<uint32_t*>(im.px.data())))))
        return false;

    return SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
}

bool CaptureScreenToPng(const std::wstring& path)
{
    // Full virtual desktop at its real pixel size - no scaling, no cropping.
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (w <= 0 || h <= 0)
        return false;

    HDC screen = GetDC(nullptr);
    if (screen == nullptr)
        return false;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC mem = CreateCompatibleDC(screen);
    bool ok = false;

    if (bmp != nullptr && mem != nullptr && bits != nullptr)
    {
        HGDIOBJ prev = SelectObject(mem, bmp);
        // CAPTUREBLT so layered windows are included.
        if (BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY | CAPTUREBLT))
        {
            GdiFlush();
            Image im;
            im.w = w;
            im.h = h;
            im.px.assign(static_cast<uint32_t*>(bits),
                         static_cast<uint32_t*>(bits) + static_cast<size_t>(w) * h);
            ok = SavePng(path, im);
        }
        SelectObject(mem, prev);
    }

    if (mem != nullptr)
        DeleteDC(mem);
    if (bmp != nullptr)
        DeleteObject(bmp);
    ReleaseDC(nullptr, screen);
    return ok;
}

std::vector<std::wstring> EnumerateCorpus(const std::wstring& dir)
{
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW((dir + L"\\*.png").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return out;
    do
    {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            out.push_back(dir + L"\\" + fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

std::wstring CorpusDirectory()
{
    wchar_t env[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"ZOOMIT_SCALE_CORPUS", env, MAX_PATH) > 0)
        return env;

    // The module this code lives in, not the process: under vstest that is the
    // test DLL, and nullptr would name the test host's directory instead.
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&CorpusDirectory),
                       &self);

    wchar_t module[MAX_PATH] = {};
    GetModuleFileNameW(self, module, MAX_PATH);
    std::wstring p = module;
    const size_t slash = p.find_last_of(L'\\');
    if (slash != std::wstring::npos)
        p.resize(slash);
    return p + L"\\corpus";
}

Image SyntheticTextScreen(int w, int h)
{
    // Deterministic, text-like: thin vertical stems, diagonals and a serif-ish
    // horizontal bar, on a light ground.  No RNG, so a failure reproduces.
    //
    // Strokes get a one-pixel grey ramp on each side, as greyscale
    // anti-aliasing gives real screen text.  Hard binary edges are not what
    // any screen shows, and the cubic's overshoot on them is larger than on
    // anti-aliased ones - measured, not assumed.
    Image im;
    im.w = w;
    im.h = h;
    im.px.assign(static_cast<size_t>(w) * h, 0xFFF4F4F4u);

    auto put = [&](int x, int y, uint8_t v) {
        if (x < 0 || y < 0 || x >= w || y >= h)
            return;
        uint32_t& px = im.px[static_cast<size_t>(y) * w + x];
        // Darkest writer wins, so ramps never lighten a neighbouring stroke.
        if ((px & 0xFF) > v)
            px = 0xFF000000u | (static_cast<uint32_t>(v) << 16) | (static_cast<uint32_t>(v) << 8) | v;
    };

    // Ink value v at (x, y) with half-coverage neighbours left and right, or
    // above and below for horizontals.
    auto stroke = [&](int x, int y, uint8_t v, bool vertical) {
        const uint8_t edge = static_cast<uint8_t>((v + 0xF4) / 2);
        put(x, y, v);
        if (vertical)
        {
            put(x - 1, y, edge);
            put(x + 1, y, edge);
        }
        else
        {
            put(x, y - 1, edge);
            put(x, y + 1, edge);
        }
    };

    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            const int cx = x % 24;
            const int cy = y % 32;
            if (cy >= 4 && cy <= 27)
            {
                if (cx == 3 || cx == 4)
                    stroke(x, y, 24, true); // stem
                else if (cx == 12 && (cy & 1))
                    stroke(x, y, 90, true); // dotted stem
                else if (cx == (cy / 2) + 14)
                    stroke(x, y, 40, true); // diagonal
                else if (cy == 15 && cx >= 3 && cx <= 18)
                    stroke(x, y, 60, false); // crossbar
            }
            if (cy == 29 && cx >= 2 && cx <= 6)
                stroke(x, y, 20, false); // serif foot
        }
    }

    // A 64x64 colour gradient in the centre, inside the source window at every
    // zoom level.  HALFTONE only filters when the window holds more than a few
    // dozen distinct colours; grey text alone is about six, on which HALFTONE
    // degenerates to nearest neighbour and comparisons against it mean nothing.
    for (int y = 0; y < 64; ++y)
    {
        for (int x = 0; x < 64; ++x)
        {
            const int px = w / 2 - 32 + x;
            const int py = h / 2 - 32 + y;
            if (px < 0 || py < 0 || px >= w || py >= h)
                continue;
            const uint32_t r = static_cast<uint32_t>(x * 4) & 0xFF;
            const uint32_t g = static_cast<uint32_t>(y * 4) & 0xFF;
            const uint32_t b = static_cast<uint32_t>((x + y) * 2) & 0xFF;
            im.px[static_cast<size_t>(py) * w + px] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
    return im;
}
