// Low Latency ASIO for UMC22 - ASIO driver (in-process COM DLL) for the Behringer UMC22 (USB Audio CODEC / PCM2902)
// Copyright (C) 2026 Tohzzicklao
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under the terms of the
// GNU General Public License as published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version. This program is distributed in the hope that it
// will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE. See the LICENSE file for details.
//
// Not affiliated with or endorsed by Steinberg, Behringer / Music Tribe or Neural DSP.
// ASIO is a trademark of Steinberg Media Technologies GmbH.
//
// - Opens the UMC22 input and output in WASAPI EXCLUSIVE mode (minimum period ~3 ms).
// - Mixes into the output the audio of another Windows endpoint captured via loopback
//   (by default "High Definition Audio Device"), so YouTube/browser audio plays through
//   the UMC22 headphones at the same time even though the UMC22 is in exclusive mode.
// - Config: %APPDATA%\UMC22ASIO\config.ini   Log: %APPDATA%\UMC22ASIO\log.txt
//
// The ASIO interface is re-implemented at the ABI level (x64), without the Steinberg SDK.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <timeapi.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <shlobj.h>
#include <shellapi.h>
#include <commctrl.h>
#include <atomic>
#include <vector>
#include <string>
#include <thread>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cwchar>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
// control panel uses the modern (v6) common controls; build.bat embeds the manifest as resource ID 2
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#include "src/asio_abi.h"

// {6B3E2A71-9C4D-4F1E-B8A2-5D7C0E91F3A4}
static const CLSID CLSID_UMC22 = { 0x6b3e2a71, 0x9c4d, 0x4f1e, { 0xb8, 0xa2, 0x5d, 0x7c, 0x0e, 0x91, 0xf3, 0xa4 } };
static const wchar_t* kClsidStr = L"{6B3E2A71-9C4D-4F1E-B8A2-5D7C0E91F3A4}";
static const wchar_t* kDriverNameW = L"Low Latency ASIO for UMC22";
static const char* kDriverName = "Low Latency ASIO for UMC22";
static const long kDriverVersion = 0x010200;     // 1.2.0, reported to hosts and in problem reports
static const char* kRepoUrl = "https://github.com/Tohzzicklao/umc22-asio";
static const wchar_t* kLegacyNameW = L"UMC22 Low Latency"; // name used by v1.0.x, removed on (un)register

static HMODULE g_hInst = nullptr;
static std::atomic<long> g_locks{ 0 };

#include "src/config.h"
#include "src/wasapi.h"
#include "src/dsp.h"
#include "src/default_output.h"
#include "src/driver.h"
#include "src/panel.h"
#include "src/com.h"
