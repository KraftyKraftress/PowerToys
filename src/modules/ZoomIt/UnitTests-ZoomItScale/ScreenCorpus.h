//==============================================================================
//
// ScreenCorpus.h
//
// Test input: full-resolution, losslessly compressed screenshots.
//
// The comparison deliberately does not drive ZoomIt.  Capture a screen once
// with CaptureScreenToPng, then every later run resamples those exact pixels,
// so results are reproducible and a failure is never a mis-aimed zoom or a
// blanked monitor.
//
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
//==============================================================================
#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "ScaleMetrics.h"

// PNG - lossless, so a round trip through disk changes nothing.
bool CaptureScreenToPng(const std::wstring& path);

bool LoadPng(const std::wstring& path, Image& out);
bool SavePng(const std::wstring& path, const Image& im);

// Every *.png in `dir`, sorted, so a run is deterministic.
std::vector<std::wstring> EnumerateCorpus(const std::wstring& dir);

// A deterministic stand-in used when no corpus is present, so the tests still
// mean something on a machine that has never captured one.  Text-like:
// thin anti-aliased strokes, mixed contrast, plus a small colour gradient so
// that HALFTONE filters rather than degenerating to nearest neighbour.
Image SyntheticTextScreen(int w, int h);

// Where the corpus lives: %ZOOMIT_SCALE_CORPUS% if set, else a "corpus"
// directory beside the test binary.
std::wstring CorpusDirectory();
