# [ZoomIt smoothing bench](/tools/ZoomItSmoothingBench)

Measures the ways ZoomIt can paint the zoom window when **Smooth the zoomed image** is on or off: for speed against the 20 ms zoom-animation interval (`ZOOM_LEVEL_STEP_TIME`), and for quality against each other. It compiles the product kernel `src/modules/ZoomIt/ZoomIt/ZoomItScale.cpp` directly and shares its helpers with `ZoomIt.Scale.UnitTests`.

Nothing here launches ZoomIt. Capture a screen once, losslessly, then every run resamples those exact pixels, so results are reproducible.

## Build

The project inherits the repo's C++ build settings (analyzer ruleset, vcpkg manifest), so it needs the same PowerToys development setup as the main solution. Open `ZoomItSmoothingBench.sln` in Visual Studio, or from a Developer Command Prompt:

```bat
msbuild tools\ZoomItSmoothingBench\ZoomItSmoothingBench.vcxproj /p:Platform=x64 /p:Configuration=Release
```

The executable is `x64\Release\ZoomItSmoothingBench\PowerToys.ZoomItSmoothingBench.exe` under the repo root.

## Run

```bat
PowerToys.ZoomItSmoothingBench capture shots\desk.png
PowerToys.ZoomItSmoothingBench compare shots --zoom 1.25,2,4
PowerToys.ZoomItSmoothingBench bench   shots --mode all --reps 31
PowerToys.ZoomItSmoothingBench dump    shots --mode new --zoom 4 --out frames
```

| Command | Reports |
|---|---|
| `compare` | PSNR (dB, higher = closer), SSIM (0-1, 1 = identical), GMSD (0-0.5, 0 = identical), and stroke darkness measured on `HALFTONE`'s ink mask, for every mode against `HALFTONE` |
| `bench` | median frame time and quartiles per mode, as a percentage of the same run's `HALFTONE`; modes are interleaved within each round so drift affects them equally |
| `dump` | the rendered frames as PNGs |

`--mode` is `off` (`COLORONCOLOR` `StretchBlt`), `halftone` (the `HALFTONE` `StretchBlt` ZoomIt used before), `new` (`SmoothStretchBlt`), or `all`. `--at X,Y` centres the zoom on a feature instead of the middle of the frame.

Any full-resolution PNG is valid input. `python make_collateral.py` (Python 3 with Pillow) writes five synthetic 3840x2160 text-heavy screens to `collateral\`.

## Measuring against HALFTONE

`HALFTONE` is a halftoning algorithm and keys off the source's distinct colour count: on plain terminal or code text (roughly 17 colours in the zoom window) it does almost no filtering, and below about 64-160 px of source width it is bit-identical to `COLORONCOLOR`. Compare against it on multi-colour content, such as a photo, an emoji, or the `collateral` set, or aim `--at` such a region.

## Unit tests

`src/modules/ZoomIt/UnitTests-ZoomItScale` (**ZoomIt.Scale.UnitTests** in the solution) runs the same comparisons as a native unit test. Point `%ZOOMIT_SCALE_CORPUS%` at a directory of captured PNGs, or place them in a `corpus` folder beside the test DLL; with no corpus it uses a deterministic synthetic screen.
