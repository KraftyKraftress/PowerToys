"""
Generate ZoomIt smoothing benchmark collateral.

Produces five 3840x2160 BGRA "screen captures" that stand in for what a
ZoomIt user actually magnifies.  Every sample is deliberately text-heavy:
the feature under test is font smoothing, so glyph edges are the signal.

Output: collateral/<name>.png, which PowerToys.ZoomItSmoothingBench.exe reads directly.
"""
import os
from PIL import Image, ImageDraw, ImageFont

W, H = 3840, 2160
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "collateral")

_FONTS = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "Fonts")

MONO = os.path.join(_FONTS, "consola.ttf")
SANS = os.path.join(_FONTS, "arial.ttf")
SANSB = os.path.join(_FONTS, "arialbd.ttf")
SERIF = os.path.join(_FONTS, "times.ttf")

for _f in (MONO, SANS, SANSB, SERIF):
    if not os.path.exists(_f):
        raise SystemExit("missing font: " + _f)


def font(path, size):
    return ImageFont.truetype(path, size)


CODE = """// ---------------------------------------------------------------------------
//  ScaleImage - area-average resampler used by the zoomed-image path.
// ---------------------------------------------------------------------------
void ScaleImage( HDC hdcDst, float xDst, float yDst, float wDst, float hDst,
                 HBITMAP bmSrc, float xSrc, float ySrc, float wSrc, float hSrc )
{
    Gdiplus::Graphics dstGraphics( hdcDst );
    {
        Gdiplus::Bitmap srcBitmap( bmSrc, NULL );
        if ( g_SmoothImage ) {
            dstGraphics.SetInterpolationMode( InterpolationModeHighQuality );
        } else {
            dstGraphics.SetInterpolationMode( InterpolationModeLowQuality );
        }
        dstGraphics.SetPixelOffsetMode( Gdiplus::PixelOffsetModeHalf );
        dstGraphics.DrawImage( &srcBitmap, RectF( xDst, yDst, wDst, hDst ),
                               xSrc, ySrc, wSrc, hSrc, Gdiplus::UnitPixel );
    }
}

static inline uint32_t BlendPremultiplied( uint32_t dst, uint32_t src, uint8_t a )
{
    const uint32_t rb = ( ( ( src & 0x00FF00FFu ) * a ) >> 8 ) & 0x00FF00FFu;
    const uint32_t g  = ( ( ( src & 0x0000FF00u ) * a ) >> 8 ) & 0x0000FF00u;
    const uint32_t ia = 255u - a;
    const uint32_t drb = ( ( ( dst & 0x00FF00FFu ) * ia ) >> 8 ) & 0x00FF00FFu;
    const uint32_t dg  = ( ( ( dst & 0x0000FF00u ) * ia ) >> 8 ) & 0x0000FF00u;
    return ( rb + drb ) | ( g + dg ) | 0xFF000000u;
}

// Telescoping zoom animation: 20 ms per step, ZOOM_LEVEL_STEP_IN == 1.1f.
case WM_TIMER:
    if ( zoomTelescopeStep != 0.0f ) {
        zoomLevel *= zoomTelescopeStep;
        if ( ( zoomTelescopeStep > 1 && zoomLevel >= zoomTelescopeTarget ) ||
             ( zoomTelescopeStep < 1 && zoomLevel <= zoomTelescopeTarget ) ) {
            zoomLevel = zoomTelescopeTarget;
            KillTimer( hWnd, wParam );
        }
        InvalidateRect( hWnd, NULL, FALSE );
    }
    break;
"""

KW = {"void", "static", "inline", "const", "if", "else", "return", "case",
      "break", "float", "uint32_t", "uint8_t", "true", "false", "NULL"}


def sample_code_editor():
    """Dark-theme editor, 13px monospace, syntax colouring, gutter, minimap."""
    im = Image.new("RGB", (W, H), (30, 30, 30))
    d = ImageDraw.Draw(im)
    fm = font(MONO, 13)
    fmb = font(MONO, 13)
    ui = font(SANS, 12)

    d.rectangle([0, 0, W, 30], fill=(60, 60, 60))
    d.text((12, 8), "ScaleImage.cpp - PowerToys - Visual Studio Code", font=ui, fill=(204, 204, 204))
    d.rectangle([0, 30, 260, H], fill=(37, 37, 38))
    tree = ["> src", "  > modules", "    v ZoomIt", "        Zoomit.cpp", "        ZoomIt.h",
            "        ScaleImage.cpp", "        PanoramaCapture.cpp", "        WebcamCapture.cpp",
            "  > common", "  > runner", "> tools", "> doc"]
    for i, t in enumerate(tree):
        d.text((16, 44 + i * 19), t, font=ui, fill=(190, 190, 190))

    # Three split panes - a 4K editor is not one 3500px-wide column of code.
    lines = (CODE.splitlines() * 40)
    panes = [272, 1450, 2620]
    for pi, px0 in enumerate(panes):
        if pi:
            d.rectangle([px0 - 18, 30, px0 - 16, H], fill=(60, 60, 60))
        y = 36
        ln = 1 + pi * 137
        while y < H - 16:
            line = lines[(ln - 1) % len(lines)]
            d.text((px0, y), "%4d" % ln, font=fm, fill=(133, 133, 133))
            x = px0 + 58
            for tok in line.replace("(", " ( ").replace(")", " ) ").split(" "):
                if not tok:
                    x += 8
                    continue
                if line.strip().startswith("//"):
                    col = (106, 153, 85)
                elif tok in KW:
                    col = (86, 156, 214)
                elif tok.startswith("0x") or tok.rstrip("f.,;)").replace(".", "").isdigit():
                    col = (181, 206, 168)
                elif tok.startswith("g_") or tok.startswith("m_"):
                    col = (156, 220, 254)
                else:
                    col = (212, 212, 212)
                d.text((x, y), tok, font=fm, fill=col)
                x += int(d.textlength(tok, font=fm)) + 8
            y += 17
            ln += 1

    # minimap
    d.rectangle([W - 160, 30, W, H], fill=(33, 33, 33))
    for i in range(0, (H - 40) // 3):
        wdt = 10 + (i * 37) % 120
        d.rectangle([W - 150, 36 + i * 3, W - 150 + wdt, 37 + i * 3], fill=(90, 100, 110))

    d.rectangle([0, H - 26, W, H], fill=(0, 122, 204))
    d.text((12, H - 21), "main*   Ln 1482, Col 17   Spaces: 4   UTF-8   CRLF   C++", font=ui, fill=(255, 255, 255))
    return im


def sample_terminal():
    """Black console, 15px mono, low-chroma output - worst case for HALFTONE."""
    im = Image.new("RGB", (W, H), (12, 12, 12))
    d = ImageDraw.Draw(im)
    fm = font(MONO, 15)
    rows = []
    for i in range(200):
        rows += [
            "PS C:\\src\\PowerToys> .\\tools\\build\\build.cmd -c Release -p x64",
            "  Determining projects to restore...",
            "  Restored C:\\src\\PowerToys\\src\\modules\\ZoomIt\\ZoomIt\\ZoomIt.vcxproj (in 412 ms).",
            "  ZoomItSettingsInterop -> C:\\src\\PowerToys\\x64\\Release\\ZoomItSettingsInterop.dll",
            "  Zoomit.cpp",
            "  PanoramaCapture.cpp",
            "  VideoRecordingSession.cpp",
            "  WebcamCapture.cpp",
            "  ZoomIt.vcxproj -> C:\\src\\PowerToys\\x64\\Release\\PowerToys.ZoomIt.exe",
            "",
            "Build succeeded in 84.19s   0 Warning(s)   0 Error(s)",
            "",
        ]
    # Two panes side by side, as a split Windows Terminal at 4K.
    for pi, px0 in enumerate((18, 1960)):
        if pi:
            d.rectangle([px0 - 20, 0, px0 - 18, H], fill=(70, 70, 70))
        y = 14
        i = pi * 53
        while y < H - 20:
            line = rows[i % len(rows)]
            col = (204, 204, 204)
            if line.startswith("PS "):
                col = (255, 255, 255)
            elif "succeeded" in line:
                col = (35, 209, 139)
            elif line.strip().startswith("Restored") or "->" in line:
                col = (229, 229, 16)
            d.text((px0, y), line, font=fm, fill=col)
            y += 19
            i += 1
    return im


PROSE = ("ZoomIt is a screen zoom, annotation, and recording tool for technical presentations and "
         "demos. It runs unobtrusively in the tray and activates with customizable hotkeys to zoom "
         "in on an area of the screen, move around while zoomed, and draw on the zoomed image. ")


def sample_article():
    """Light-background prose - serif body at 17px, the classic readability case."""
    im = Image.new("RGB", (W, H), (255, 255, 255))
    d = ImageDraw.Draw(im)
    h1 = font(SANSB, 44)
    h2 = font(SANSB, 24)
    body = font(SERIF, 17)
    cap = font(SANS, 13)

    d.rectangle([0, 0, W, 64], fill=(243, 243, 243))
    d.text((60, 20), "Microsoft Learn  /  Windows  /  PowerToys  /  ZoomIt utility", font=cap, fill=(80, 80, 80))

    col_w = 1080
    for c, x0 in enumerate([160, 1360, 2560]):
        y = 110
        if c == 0:
            d.text((x0, y), "ZoomIt utility", font=h1, fill=(20, 20, 20))
            y += 78
        words = (PROSE * 30).split()
        wi = c * 97
        section = 0
        while y < H - 60:
            if (y // 300) % 3 == 0 and y % 300 < 26 and y > 140:
                d.text((x0, y), ["Zoom mode", "Draw mode", "Break mode",
                                 "Record", "Demo type", "Settings"][section % 6],
                       font=h2, fill=(0, 90, 158))
                section += 1
                y += 40
                continue
            line = ""
            while wi < len(words):
                trial = (line + " " + words[wi]).strip()
                if d.textlength(trial, font=body) > col_w:
                    break
                line = trial
                wi += 1
            if wi >= len(words):
                wi = 0
            d.text((x0, y), line, font=body, fill=(36, 36, 36))
            y += 26
    return im


def sample_slide():
    """Presentation slide - huge glyphs, flat fills, hard edges."""
    im = Image.new("RGB", (W, H), (16, 24, 48))
    d = ImageDraw.Draw(im)
    for y in range(H):
        t = y / H
        d.line([(0, y), (W, y)], fill=(int(16 + 24 * t), int(24 + 34 * t), int(48 + 66 * t)))
    d.text((240, 300), "Font smoothing", font=font(SANSB, 190), fill=(255, 255, 255))
    d.text((240, 520), "in the zoomed image path", font=font(SANS, 96), fill=(140, 190, 255))
    d.rectangle([240, 700, 1040, 708], fill=(0, 160, 230))

    bullets = ["HALFTONE StretchBlt on every paint",
               "20 ms telescoping animation budget",
               "8.3 M destination pixels at 3840 x 2160",
               "Single-threaded, scalar, no GPU path",
               "Same toggle gates clipboard, OCR and save"]
    fy = font(SANS, 62)
    for i, b in enumerate(bullets):
        d.ellipse([250, 812 + i * 108, 274, 836 + i * 108], fill=(0, 190, 255))
        d.text((320, 796 + i * 108), b, font=fy, fill=(228, 234, 244))
    d.text((240, H - 130), "PowerToys  |  ZoomIt  |  Performance", font=font(SANS, 40), fill=(120, 140, 175))
    return im


def sample_dialog():
    """Settings UI - small labels, icon glyphs, controls, plus a photo-ish region."""
    im = Image.new("RGB", (W, H), (243, 243, 243))
    d = ImageDraw.Draw(im)
    fs = font(SANS, 15)
    fsb = font(SANSB, 15)
    fh = font(SANSB, 34)

    d.rectangle([0, 0, 420, H], fill=(238, 238, 238))
    nav = ["Dashboard", "General", "Advanced Paste", "Always on Top", "Awake",
           "Color Picker", "Command Palette", "Crop And Lock", "Environment Variables",
           "FancyZones", "File Explorer add-ons", "File Locksmith", "Hosts File Editor",
           "Image Resizer", "Keyboard Manager", "Light Switch", "Mouse utilities",
           "Mouse Without Borders", "New+", "Peek", "PowerRename", "PowerToys Run",
           "Quick Accent", "Registry Preview", "Screen Ruler", "Shortcut Guide",
           "Text Extractor", "Workspaces", "ZoomIt"]
    for i, n in enumerate(nav):
        y = 90 + i * 42
        sel = n == "ZoomIt"
        if sel:
            d.rectangle([8, y - 8, 404, y + 30], fill=(255, 255, 255))
            d.rectangle([8, y - 2, 12, y + 24], fill=(0, 95, 184))
        d.rectangle([28, y + 3, 44, y + 19], outline=(90, 90, 90))
        d.text((60, y), n, font=fsb if sel else fs, fill=(20, 20, 20) if sel else (60, 60, 60))

    d.text((470, 60), "ZoomIt", font=fh, fill=(20, 20, 20))
    d.text((470, 108), "Screen zoom, annotation and recording for technical presentations.",
           font=fs, fill=(96, 96, 96))

    cards = [("Enable ZoomIt", "Toggle", True), ("Zoom", "Section", None),
             ("Zoom toggle shortcut", "Ctrl + 1", None),
             ("Animate zoom in and out", "Checkbox", True),
             ("Smooth the zoomed image", "Checkbox", True),
             ("Zoom level", "Slider 2.0x", None),
             ("Live zoom toggle shortcut", "Ctrl + 4", None),
             ("Draw", "Section", None), ("Draw toggle shortcut", "Ctrl + 2", None),
             ("Pen colour", "Red", None), ("Font", "Segoe UI, 24pt", None),
             ("Break", "Section", None), ("Break timeout", "10 minutes", None),
             ("Show expired time", "Checkbox", False),
             ("Record", "Section", None), ("Capture audio input", "Checkbox", False),
             ("Frame rate", "30 fps", None), ("Scaling", "Fit to screen", None)]
    y = 170
    for label, val, chk in cards:
        if val == "Section":
            d.text((470, y + 14), label, font=font(SANSB, 21), fill=(20, 20, 20))
            y += 62
            continue
        d.rectangle([470, y, 2400, y + 64], fill=(251, 251, 251), outline=(225, 225, 225))
        d.rectangle([492, y + 22, 512, y + 42], outline=(120, 120, 120))
        d.text((532, y + 22), label, font=fs, fill=(26, 26, 26))
        if chk is None:
            d.rectangle([2160, y + 18, 2370, y + 46], fill=(255, 255, 255), outline=(160, 160, 160))
            d.text((2174, y + 24), val, font=fs, fill=(40, 40, 40))
        else:
            d.rounded_rectangle([2300, y + 20, 2370, y + 44], 12,
                                fill=(0, 95, 184) if chk else (220, 220, 220))
            cx = 2356 if chk else 2314
            d.ellipse([cx - 9, y + 23, cx + 9, y + 41], fill=(255, 255, 255))
        y += 74
        if y > H - 90:
            break

    # photo-ish preview region: smooth gradients + fine detail, no glyphs
    px0, py0 = 2460, 200
    for yy in range(700):
        for xx in range(0, 1300, 4):
            r = (xx * 255 // 1300 + yy // 4) & 0xFF
            g = (yy * 255 // 700) & 0xFF
            b = ((xx + yy) // 3) & 0xFF
            d.rectangle([px0 + xx, py0 + yy, px0 + xx + 3, py0 + yy], fill=(r, g, b))
    for i in range(120):
        d.line([(px0 + i * 11, py0), (px0 + i * 11 + 300, py0 + 700)], fill=(255, 255, 255), width=1)
    d.text((px0, py0 + 720), "Preview", font=font(SANSB, 21), fill=(20, 20, 20))
    return im


SAMPLES = [
    ("code_editor", sample_code_editor),
    ("terminal", sample_terminal),
    ("article", sample_article),
    ("slide", sample_slide),
    ("settings_ui", sample_dialog),
]


def main():
    os.makedirs(OUT, exist_ok=True)
    for name, fn in SAMPLES:
        im = fn().convert("RGB")
        assert im.size == (W, H), im.size
        im.save(os.path.join(OUT, name + ".png"), optimize=False)
        print("wrote %-14s %dx%d" % (name, W, H))


if __name__ == "__main__":
    main()
