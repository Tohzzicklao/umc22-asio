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

// ============================================================ ABI ASIO
typedef long ASIOBool;
typedef long ASIOError;
typedef double ASIOSampleRate;
typedef long ASIOSampleType;
struct ASIOSamples { unsigned long hi, lo; };
typedef ASIOSamples ASIOTimeStamp;
enum {
    ASE_OK = 0, ASE_SUCCESS = 0x3f4847a0, ASE_NotPresent = -1000, ASE_HWMalfunction,
    ASE_InvalidParameter, ASE_InvalidMode, ASE_SPNotAdvancing, ASE_NoClock, ASE_NoMemory
};
enum { ASIOSTInt32LSB = 18 };
enum { kAsioSelectorSupported = 1, kAsioEngineVersion = 2, kAsioResetRequest = 3 };
struct ASIOClockSource { long index, associatedChannel, associatedGroup; ASIOBool isCurrentSource; char name[32]; };
struct ASIOChannelInfo { long channel; ASIOBool isInput, isActive; long channelGroup; ASIOSampleType type; char name[32]; };
struct ASIOBufferInfo { ASIOBool isInput; long channelNum; void* buffers[2]; };
struct ASIOTime;
struct ASIOCallbacks {
    void (*bufferSwitch)(long doubleBufferIndex, ASIOBool directProcess);
    void (*sampleRateDidChange)(ASIOSampleRate sRate);
    long (*asioMessage)(long selector, long value, void* message, double* opt);
    ASIOTime* (*bufferSwitchTimeInfo)(ASIOTime* params, long doubleBufferIndex, ASIOBool directProcess);
};

struct IASIO : public IUnknown {
    virtual ASIOBool init(void* sysHandle) = 0;
    virtual void getDriverName(char* name) = 0;
    virtual long getDriverVersion() = 0;
    virtual void getErrorMessage(char* string) = 0;
    virtual ASIOError start() = 0;
    virtual ASIOError stop() = 0;
    virtual ASIOError getChannels(long* numInputChannels, long* numOutputChannels) = 0;
    virtual ASIOError getLatencies(long* inputLatency, long* outputLatency) = 0;
    virtual ASIOError getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) = 0;
    virtual ASIOError canSampleRate(ASIOSampleRate sampleRate) = 0;
    virtual ASIOError getSampleRate(ASIOSampleRate* sampleRate) = 0;
    virtual ASIOError setSampleRate(ASIOSampleRate sampleRate) = 0;
    virtual ASIOError getClockSources(ASIOClockSource* clocks, long* numSources) = 0;
    virtual ASIOError setClockSource(long reference) = 0;
    virtual ASIOError getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) = 0;
    virtual ASIOError getChannelInfo(ASIOChannelInfo* info) = 0;
    virtual ASIOError createBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize, ASIOCallbacks* callbacks) = 0;
    virtual ASIOError disposeBuffers() = 0;
    virtual ASIOError controlPanel() = 0;
    virtual ASIOError future(long selector, void* opt) = 0;
    virtual ASIOError outputReady() = 0;
};

// {6B3E2A71-9C4D-4F1E-B8A2-5D7C0E91F3A4}
static const CLSID CLSID_UMC22 = { 0x6b3e2a71, 0x9c4d, 0x4f1e, { 0xb8, 0xa2, 0x5d, 0x7c, 0x0e, 0x91, 0xf3, 0xa4 } };
static const wchar_t* kClsidStr = L"{6B3E2A71-9C4D-4F1E-B8A2-5D7C0E91F3A4}";
static const wchar_t* kDriverNameW = L"Low Latency ASIO for UMC22";
static const char* kDriverName = "Low Latency ASIO for UMC22";
static const wchar_t* kLegacyNameW = L"UMC22 Low Latency"; // name used by v1.0.x, removed on (un)register

static HMODULE g_hInst = nullptr;
static std::atomic<long> g_locks{ 0 };

// ============================================================ utilities
static std::wstring appDir() {
    PWSTR p = nullptr; std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) { s = p; CoTaskMemFree(p); }
    s += L"\\UMC22ASIO";
    CreateDirectoryW(s.c_str(), nullptr);
    return s;
}
static std::wstring configPath() { return appDir() + L"\\config.ini"; }

static void logf(const char* fmt, ...) {
    static std::wstring path = appDir() + L"\\log.txt";
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 512 * 1024) {
        std::wstring old = appDir() + L"\\log.old.txt";
        MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    FILE* f = _wfopen(path.c_str(), L"a");
    if (!f) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(f, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a);
    fputc('\n', f); fclose(f);
}

struct Config {
    std::wstring render = L"USB Audio CODEC";
    std::wstring capture = L"USB Audio CODEC";
    std::wstring loopback = L"High Definition Audio Device";
    float ytVolume = 1.0f;
    bool monoAuto = true;
    float ytLatencyMs = 40.0f;
    long preferredBuffer = 144;
    bool autoDefault = true;
    bool debugMeter = false;
    bool lowLatency = true;
};

static void ensureConfig() {
    std::wstring p = configPath();
    if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    FILE* f = _wfopen(p.c_str(), L"w");
    if (!f) return;
    fputs("; Low Latency ASIO for UMC22 - configuration\n"
          "; Device names are substrings of the device name shown in Windows (Sound settings).\n"
          "[driver]\n"
          "render=USB Audio CODEC\n"
          "capture=USB Audio CODEC\n"
          "; Device whose audio (YouTube, system sounds...) is mixed into the headphones. Empty = disabled.\n"
          "loopback=High Definition Audio Device\n"
          "; Volume of the Windows/YouTube mix (0.0 - 2.0)\n"
          "ytVolume=1.0\n"
          "; 1 = if the host only sends the guitar on L, copy it to R as well\n"
          "monoAuto=1\n"
          "; Windows/YouTube audio buffer in ms (raise it if YouTube drops out)\n"
          "ytLatencyMs=40\n"
          "; Preferred ASIO buffer size in frames\n"
          "preferredBuffer=144\n"
          "; 1 = while the driver is open, the Windows default output is moved to the 'loopback'\n"
          ";     device (so its audio gets mixed) and restored to the UMC22 on close\n"
          "autoDefault=1\n"
          "; 1 = write levels/phase to log.txt every 5 s (diagnostics)\n"
          "debugMeter=0\n"
          "; 1 = no extra output buffer when the device runs at the ASIO buffer size (one buffer\n"
          ";     less output latency: -3 ms at 144 frames). Set to 0 if you hear clicks.\n"
          "lowLatency=1\n", f);
    fclose(f);
}

static Config loadConfig() {
    ensureConfig();
    Config c; std::wstring p = configPath(); wchar_t buf[256];
    GetPrivateProfileStringW(L"driver", L"render", c.render.c_str(), buf, 256, p.c_str()); c.render = buf;
    GetPrivateProfileStringW(L"driver", L"capture", c.capture.c_str(), buf, 256, p.c_str()); c.capture = buf;
    GetPrivateProfileStringW(L"driver", L"loopback", c.loopback.c_str(), buf, 256, p.c_str()); c.loopback = buf;
    GetPrivateProfileStringW(L"driver", L"ytVolume", L"1.0", buf, 256, p.c_str()); c.ytVolume = (float)_wtof(buf);
    GetPrivateProfileStringW(L"driver", L"ytLatencyMs", L"40", buf, 256, p.c_str()); c.ytLatencyMs = std::max(10.0f, (float)_wtof(buf));
    c.monoAuto = GetPrivateProfileIntW(L"driver", L"monoAuto", 1, p.c_str()) != 0;
    c.preferredBuffer = std::clamp<long>(GetPrivateProfileIntW(L"driver", L"preferredBuffer", 144, p.c_str()), 32, 2048);
    c.autoDefault = GetPrivateProfileIntW(L"driver", L"autoDefault", 1, p.c_str()) != 0;
    c.debugMeter = GetPrivateProfileIntW(L"driver", L"debugMeter", 0, p.c_str()) != 0;
    c.lowLatency = GetPrivateProfileIntW(L"driver", L"lowLatency", 1, p.c_str()) != 0;
    return c;
}

static std::wstring lower(std::wstring s) { for (auto& ch : s) ch = (wchar_t)towlower(ch); return s; }

// Finds an active endpoint whose friendly name contains 'needle'. Returns its ID.
static std::wstring findEndpoint(IMMDeviceEnumerator* en, EDataFlow flow, const std::wstring& needle, std::wstring* nameOut) {
    if (needle.empty()) return L"";
    IMMDeviceCollection* col = nullptr; std::wstring result;
    if (FAILED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col))) return L"";
    UINT n = 0; col->GetCount(&n);
    std::wstring nl = lower(needle);
    for (UINT i = 0; i < n && result.empty(); i++) {
        IMMDevice* d = nullptr; if (FAILED(col->Item(i, &d))) continue;
        IPropertyStore* ps = nullptr;
        if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
            PROPVARIANT v; PropVariantInit(&v);
            if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) {
                std::wstring name = v.pwszVal;
                if (lower(name).find(nl) != std::wstring::npos) {
                    LPWSTR id = nullptr;
                    if (SUCCEEDED(d->GetId(&id))) { result = id; CoTaskMemFree(id); if (nameOut) *nameOut = name; }
                }
            }
            PropVariantClear(&v); ps->Release();
        }
        d->Release();
    }
    col->Release();
    return result;
}

// ---------------------------------------------------------- device sample format
struct DevFmt { int ch = 2, bytes = 2, block = 4; bool flt = false; };

static inline float rdS(const BYTE* p, const DevFmt& f) {
    if (f.flt) return *(const float*)p;
    switch (f.bytes) {
    case 2: return *(const int16_t*)p * (1.0f / 32768.0f);
    case 3: return (int32_t)(((uint32_t)p[0] << 8) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 24)) * (1.0f / 2147483648.0f);
    case 4: return *(const int32_t*)p * (1.0f / 2147483648.0f);
    }
    return 0;
}
static inline void wrS(BYTE* p, const DevFmt& f, float x) {
    x = std::clamp(x, -1.0f, 1.0f);
    if (f.flt) { *(float*)p = x; return; }
    switch (f.bytes) {
    case 2: *(int16_t*)p = (int16_t)lrintf(x * 32767.0f); break;
    case 3: { int32_t v = (int32_t)lrint(x * 8388607.0); p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8); p[2] = (BYTE)(v >> 16); } break;
    case 4: *(int32_t*)p = (int32_t)llrint((double)x * 2147483647.0); break;
    }
}

static DevFmt fmtFromWave(const WAVEFORMATEX* w) {
    DevFmt f; f.ch = w->nChannels; f.bytes = w->wBitsPerSample / 8; f.block = w->nBlockAlign;
    f.flt = w->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
        (w->wFormatTag == WAVE_FORMAT_EXTENSIBLE && ((const WAVEFORMATEXTENSIBLE*)w)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    return f;
}

static void makeFmt(WAVEFORMATEXTENSIBLE& w, int ch, int sr, int bits, int valid) {
    ZeroMemory(&w, sizeof w);
    w.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE; w.Format.nChannels = (WORD)ch; w.Format.nSamplesPerSec = sr;
    w.Format.wBitsPerSample = (WORD)bits; w.Format.nBlockAlign = (WORD)(ch * bits / 8);
    w.Format.nAvgBytesPerSec = sr * w.Format.nBlockAlign; w.Format.cbSize = 22;
    w.Samples.wValidBitsPerSample = (WORD)valid;
    w.dwChannelMask = ch == 1 ? SPEAKER_FRONT_CENTER : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
    w.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
}

// Opens an endpoint in exclusive, event-driven mode. periodFrames = requested period.
static HRESULT openExclusive(IMMDevice* dev, int sr, int periodFrames, HANDLE ev, IAudioClient** out, DevFmt& df, UINT32& bufFrames) {
    IAudioClient* ac = nullptr;
    HRESULT hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac);
    if (FAILED(hr)) return hr;
    struct { int bits, valid; } tries[] = { {32, 24}, {24, 24}, {16, 16} };
    WAVEFORMATEXTENSIBLE w{}; bool found = false;
    // Keep the real reason of the failure: if every format fails with the same non-format
    // error (device in use, exclusive mode disabled...), return that instead of UNSUPPORTED_FORMAT.
    HRESULT realHr = S_OK; bool sameHr = true;
    char tried[512] = ""; // rejected formats, logged only if none works (the UMC22 always rejects 24-bit)
    for (int chs : {2, 1}) {
        for (auto t : tries) {
            makeFmt(w, chs, sr, t.bits, t.valid);
            HRESULT fhr = ac->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &w.Format, nullptr);
            if (fhr == S_OK) { found = true; break; }
            size_t len = strlen(tried);
            _snprintf_s(tried + len, sizeof tried - len, _TRUNCATE, " %dch/%d/%d:0x%08lX", chs, t.bits, t.valid, (unsigned long)fhr);
            if (realHr == S_OK) realHr = fhr; else if (realHr != fhr) sameHr = false;
        }
        if (found) break;
    }
    if (!found) {
        logf("  no exclusive format at %d Hz (channels/bits/valid:hr):%s", sr, tried);
        ac->Release();
        if (sameHr && FAILED(realHr) && realHr != AUDCLNT_E_UNSUPPORTED_FORMAT && realHr != S_FALSE) return realHr;
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    }
    REFERENCE_TIME defP = 0, minP = 0; ac->GetDevicePeriod(&defP, &minP);
    REFERENCE_TIME per = std::max<REFERENCE_TIME>(minP, (REFERENCE_TIME)(1e7 * periodFrames / sr + 0.5));
    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST;
    hr = ac->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, flags, per, per, &w.Format, nullptr);
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        UINT32 n = 0; ac->GetBufferSize(&n); ac->Release(); ac = nullptr;
        per = (REFERENCE_TIME)(1e7 * n / sr + 0.5);
        hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac);
        if (FAILED(hr)) return hr;
        hr = ac->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, flags, per, per, &w.Format, nullptr);
    }
    if (FAILED(hr)) { ac->Release(); return hr; }
    if (FAILED(hr = ac->SetEventHandle(ev))) { ac->Release(); return hr; }
    ac->GetBufferSize(&bufFrames);
    df = fmtFromWave(&w.Format);
    *out = ac;
    return S_OK;
}

// ---------------------------------------------------------- FIFOs
struct Fifo { // stereo float, single thread
    std::vector<float> d; size_t cap = 0, r = 0, n = 0;
    void init(size_t frames) { d.assign(frames * 2, 0.0f); cap = frames; r = n = 0; }
    void push(float l, float rr) {
        if (n == cap) { r = (r + 1) % cap; n--; }
        size_t w = (r + n) % cap; d[w * 2] = l; d[w * 2 + 1] = rr; n++;
    }
    bool pop(float& l, float& rr) {
        if (!n) return false;
        l = d[r * 2]; rr = d[r * 2 + 1]; r = (r + 1) % cap; n--; return true;
    }
    void drop(size_t k) { k = std::min(k, n); r = (r + k) % cap; n -= k; }
};

struct Spsc { // stereo float, single producer / single consumer
    std::vector<float> d; size_t cap = 0;
    std::atomic<size_t> w{ 0 }, r{ 0 };
    void init(size_t frames) { d.assign(frames * 2, 0.0f); cap = frames; w = 0; r = 0; }
    size_t avail() const { return w.load(std::memory_order_acquire) - r.load(std::memory_order_relaxed); }
    void push(float l, float rr) {
        size_t ww = w.load(std::memory_order_relaxed);
        if (ww - r.load(std::memory_order_acquire) >= cap) return; // full: drop
        d[(ww % cap) * 2] = l; d[(ww % cap) * 2 + 1] = rr;
        w.store(ww + 1, std::memory_order_release);
    }
    bool pop(float& l, float& rr) {
        size_t rr0 = r.load(std::memory_order_relaxed);
        if (rr0 == w.load(std::memory_order_acquire)) return false;
        l = d[(rr0 % cap) * 2]; rr = d[(rr0 % cap) * 2 + 1];
        r.store(rr0 + 1, std::memory_order_release); return true;
    }
    void skip(size_t k) { k = std::min(k, avail()); r.fetch_add(k, std::memory_order_release); }
};

// 4-point cubic Hermite resampler with smoothed drift correction and fades (Windows audio -> exclusive output)
struct YtMix {
    double pos = 0, step = 1, fillAvg = 0; float h[4][2]{}; bool primed = false;
    float fade = 0.0f, last[2]{};
    static constexpr float kFadeStep = 1.0f / 480.0f; // ~10 ms
    void block(Spsc& ring, double src, double dst, double latMs) {
        size_t fill = ring.avail();
        double target = src * latMs / 1000.0;
        if (!primed) {
            if (fill >= target) { primed = true; pos = 0; memset(h, 0, sizeof h); fillAvg = (double)fill; }
        } else if (fill > target * 3) {
            ring.skip(fill - (size_t)target);
            fill = (size_t)target; fillAvg = target;
        }
        // moving average (~0.3 s) so packet jitter doesn't modulate the pitch
        fillAvg += 0.01 * ((double)fill - fillAvg);
        double err = (fillAvg - target) / target;
        double corr = std::clamp(err * 0.001, -0.002, 0.002);
        step = src / dst * (1.0 + corr);
    }
    void next(Spsc& ring, float& l, float& r) {
        if (!primed) { // fade out from the last sample: no click on pause
            fade = std::max(0.0f, fade - kFadeStep);
            l = last[0] * fade; r = last[1] * fade; return;
        }
        pos += step;
        while (pos >= 1.0) {
            memmove(h[0], h[1], sizeof(float) * 6);
            if (!ring.pop(h[3][0], h[3][1])) { primed = false; l = last[0] * fade; r = last[1] * fade; return; }
            pos -= 1.0;
        }
        fade = std::min(1.0f, fade + kFadeStep);
        const float t = (float)pos;
        float out[2];
        for (int c = 0; c < 2; c++) { // interpolate between h[1] and h[2]
            float x0 = h[0][c], x1 = h[1][c], x2 = h[2][c], x3 = h[3][c];
            float c1 = 0.5f * (x2 - x0);
            float c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3;
            float c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
            out[c] = ((c3 * t + c2) * t + c1) * t + x1;
        }
        last[0] = out[0]; last[1] = out[1];
        l = out[0] * fade; r = out[1] * fade;
    }
};

// ---------------------------------------------------------- Windows default output
// IPolicyConfig (undocumented interface, stable since Windows 7) used to change the default device.
struct __declspec(uuid("f8679f50-850a-41cf-9c72-430f290290c8")) IPolicyConfig : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};
static const CLSID CLSID_PolicyConfigClient = { 0x870af99c, 0x171d, 0x4f9e, { 0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9 } };

// Runs f on its own MTA thread (independent of the host's COM apartment)
template <class F> static void runMta(F f) {
    std::thread t([&] { CoInitializeEx(nullptr, COINIT_MULTITHREADED); f(); CoUninitialize(); });
    t.join();
}
static std::wstring getDefaultRender() {
    std::wstring id;
    runMta([&] {
        IMMDeviceEnumerator* en = nullptr; IMMDevice* d = nullptr; LPWSTR s = nullptr;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) &&
            SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, &d)) && SUCCEEDED(d->GetId(&s))) { id = s; CoTaskMemFree(s); }
        if (d) d->Release(); if (en) en->Release();
    });
    return id;
}
static bool setDefaultRender(const std::wstring& id) {
    bool ok = false;
    runMta([&] {
        IPolicyConfig* pc = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_PolicyConfigClient, nullptr, CLSCTX_ALL, __uuidof(IPolicyConfig), (void**)&pc))) {
            ok = true;
            for (ERole r : { eConsole, eMultimedia, eCommunications }) ok &= SUCCEEDED(pc->SetDefaultEndpoint(id.c_str(), r));
            pc->Release();
        }
    });
    return ok;
}

// Peak limiter (instant attack, smooth release): avoids hard clipping when summing guitar + Windows audio
struct Limiter {
    float g = 1.0f, thr = 0.98f /* -0.2 dBFS: real overs only */, rel = 0.0f;
    void init(double sr, double releaseMs) { g = 1.0f; rel = (float)(1.0 - std::exp(-1.0 / (sr * releaseMs / 1000.0))); }
    inline void process(float& l, float& r) {
        float pk = std::max(std::fabs(l), std::fabs(r));
        float target = pk > thr ? thr / pk : 1.0f;
        if (target < g) g = target; else g += (target - g) * rel;
        l *= g; r *= g;
    }
};

// TPDF dither for 16-bit output
struct Dither {
    uint32_t s = 0x12345678u;
    inline float rnd() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return (s >> 8) * (1.0f / 16777216.0f); }
    inline float tpdf(float lsb) { return (rnd() - rnd()) * lsb; }
};

static inline int32_t f2i(float x) { return (int32_t)llrint((double)std::clamp(x, -1.0f, 1.0f) * 2147483647.0); }
static inline float i2f(int32_t v) { return v * (1.0f / 2147483648.0f); }

// ============================================================ Driver
class Driver : public IASIO {
public:
    Driver() { g_locks++; }
    ~Driver() { disposeBuffers(); g_locks--; }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == CLSID_UMC22) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = --ref_; if (!r) delete this; return r; }

    // IASIO
    ASIOBool init(void*) override {
        cfg_ = loadConfig();
        applyLive(cfg_);
        logf("init: render='%ls' capture='%ls' loopback='%ls'", cfg_.render.c_str(), cfg_.capture.c_str(), cfg_.loopback.c_str());
        HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        IMMDeviceEnumerator* en = nullptr;
        bool ok = SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)));
        if (ok) {
            std::wstring n1, n2, n3;
            // a UMC22 plugged in a moment ago can take a little while to show up in Windows
            for (int tries = 0; tries < 6; tries++) {
                if (tries) Sleep(200);
                renderId_ = findEndpoint(en, eRender, cfg_.render, &n1);
                captureId_ = findEndpoint(en, eCapture, cfg_.capture, &n2);
                if (!renderId_.empty() && !captureId_.empty()) break;
            }
            loopId_ = findEndpoint(en, eRender, cfg_.loopback, &n3);
            if (loopId_ == renderId_) loopId_.clear();
            logf("  render  -> %ls", n1.c_str());
            logf("  capture -> %ls", n2.c_str());
            logf("  loopback-> %ls", loopId_.empty() ? L"(disabled)" : n3.c_str());
            en->Release();
        }
        if (SUCCEEDED(hrCo)) CoUninitialize();
        if (renderId_.empty() || captureId_.empty()) {
            setErr("UMC22 (USB Audio CODEC) not found. Check config.ini.");
            logf("init FAIL: %s", err_);
            return 0;
        }
        return 1;
    }
    void getDriverName(char* name) override { strcpy_s(name, 32, kDriverName); }
    long getDriverVersion() override { return 0x010200; } // 1.2.0
    void getErrorMessage(char* s) override { strcpy_s(s, 124, err_); }

    ASIOError start() override {
        if (!engineReady_) return ASE_InvalidMode;
        if (running_) return ASE_OK;
        samplePos_ = 0; underrunsLive_ = 0;
        startYt();
        running_ = true;
        ResetEvent(stoppedEv_);
        SetEvent(startEv_);
        logf("start");
        return ASE_OK;
    }
    ASIOError stop() override {
        if (running_) {
            running_ = false;
            WaitForSingleObject(stoppedEv_, 3000);
            logf("stop");
        }
        stopYt();
        return ASE_OK;
    }
    ASIOError getChannels(long* in, long* out) override { *in = 2; *out = 2; return ASE_OK; }
    ASIOError getLatencies(long* in, long* out) override {
        long b = engineReady_ ? (long)devFrames_ : cfg_.preferredBuffer;
        long n = engineReady_ ? outPrime_ : (bufSize_ ? bufSize_ : cfg_.preferredBuffer);
        *in = b; *out = b + n; return ASE_OK;
    }
    ASIOError getBufferSize(long* mn, long* mx, long* pref, long* gran) override {
        *mn = 32; *mx = 2048; *pref = cfg_.preferredBuffer; *gran = 16; return ASE_OK;
    }
    ASIOError canSampleRate(ASIOSampleRate sr) override { return (sr == 44100.0 || sr == 48000.0) ? ASE_OK : ASE_NoClock; }
    ASIOError getSampleRate(ASIOSampleRate* sr) override { *sr = sampleRate_; return ASE_OK; }
    ASIOError setSampleRate(ASIOSampleRate sr) override {
        if (sr == 0.0) return ASE_OK;
        if (canSampleRate(sr) != ASE_OK) return ASE_NoClock;
        if (sr == sampleRate_) return ASE_OK;
        sampleRate_ = sr;
        logf("setSampleRate %.0f", sr);
        if (engineReady_ && cb_ && cb_->asioMessage && cb_->asioMessage(kAsioSelectorSupported, kAsioResetRequest, nullptr, nullptr))
            cb_->asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
        return ASE_OK;
    }
    ASIOError getClockSources(ASIOClockSource* c, long* num) override {
        if (!c || !num || *num < 1) return ASE_InvalidParameter;
        ZeroMemory(c, sizeof *c); c->associatedChannel = -1; c->associatedGroup = -1; c->isCurrentSource = 1;
        strcpy_s(c->name, "Internal"); *num = 1; return ASE_OK;
    }
    ASIOError setClockSource(long ref) override { return ref == 0 ? ASE_OK : ASE_InvalidParameter; }
    ASIOError getSamplePosition(ASIOSamples* sp, ASIOTimeStamp* ts) override {
        uint64_t p = samplePos_.load(); uint64_t t = stampNs_.load();
        sp->hi = (unsigned long)(p >> 32); sp->lo = (unsigned long)p;
        ts->hi = (unsigned long)(t >> 32); ts->lo = (unsigned long)t;
        return running_ ? ASE_OK : ASE_SPNotAdvancing;
    }
    ASIOError getChannelInfo(ASIOChannelInfo* info) override {
        if (info->channel < 0 || info->channel > 1) return ASE_InvalidParameter;
        info->isActive = info->isInput ? (inBuf_[info->channel][0] != nullptr) : (outBuf_[info->channel][0] != nullptr);
        info->channelGroup = 0; info->type = ASIOSTInt32LSB;
        static const char* inN[] = { "UMC22 In 1", "UMC22 In 2" };
        static const char* outN[] = { "UMC22 Out L", "UMC22 Out R" };
        strcpy_s(info->name, info->isInput ? inN[info->channel] : outN[info->channel]);
        return ASE_OK;
    }

    ASIOError createBuffers(ASIOBufferInfo* bi, long num, long size, ASIOCallbacks* cb) override {
        if (engineReady_) disposeBuffers();
        if (!cb || size < 16 || size > 8192) return ASE_InvalidParameter;
        bufSize_ = size; cb_ = cb;
        mem_.assign((size_t)num * 2 * size, 0);
        for (long i = 0; i < num; i++) {
            long ch = bi[i].channelNum;
            if (ch < 0 || ch > 1) { freeBufs(); return ASE_InvalidParameter; }
            int32_t* base = mem_.data() + (size_t)i * 2 * size;
            bi[i].buffers[0] = base; bi[i].buffers[1] = base + size;
            auto& slot = bi[i].isInput ? inBuf_[ch] : outBuf_[ch];
            slot[0] = (int32_t*)bi[i].buffers[0]; slot[1] = (int32_t*)bi[i].buffers[1];
        }
        if (!switchedDefault_) moveDefaultOff();
        // The audio thread opens the devices (WASAPI lives in its own MTA apartment)
        quit_ = false; openResult_ = E_FAIL;
        startEv_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        stoppedEv_ = CreateEventW(nullptr, TRUE, TRUE, nullptr);
        HANDLE opened = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        engine_ = std::thread([this, opened] { engineThread(opened); });
        WaitForSingleObject(opened, 5000);
        CloseHandle(opened);
        if (FAILED(openResult_)) {
            quit_ = true; SetEvent(startEv_);
            if (engine_.joinable()) engine_.join();
            CloseHandle(startEv_); CloseHandle(stoppedEv_); startEv_ = stoppedEv_ = nullptr;
            freeBufs();
            restoreDefault();
            if (openResult_ == AUDCLNT_E_DEVICE_IN_USE)
                setErr("The UMC22 is in use by another app. Move Windows/browser output to the loopback device.");
            else if (openResult_ == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED)
                setErr("Exclusive mode is disabled in the UMC22 properties (Sound > Advanced).");
            else
                setErr("Could not open the UMC22 in exclusive mode (see log.txt).");
            logf("createBuffers FAIL hr=0x%08lX", (unsigned long)openResult_);
            return ASE_HWMalfunction;
        }
        engineReady_ = true;
        logf("createBuffers ok: asio=%ld frames, device period=%u frames, %.0f Hz, output slack %ld frames", size, devFrames_, sampleRate_, outPrime_);
        return ASE_OK;
    }
    ASIOError disposeBuffers() override {
        stop();
        if (engine_.joinable()) {
            quit_ = true; SetEvent(startEv_);
            engine_.join();
        }
        if (startEv_) { CloseHandle(startEv_); startEv_ = nullptr; }
        if (stoppedEv_) { CloseHandle(stoppedEv_); stoppedEv_ = nullptr; }
        engineReady_ = false;
        freeBufs();
        restoreDefault(); // the UMC22 is released now: apps can go back to it
        return ASE_OK;
    }
    ASIOError controlPanel() override;
    ASIOError future(long, void*) override { return ASE_InvalidParameter; }
    ASIOError outputReady() override { return ASE_NotPresent; }

    // ---------------------------------------------------- control panel interface
    struct Live {
        bool ready, running, lost; long buf, prime; UINT32 dev; double sr; unsigned underruns;
        float in[2], out[2], yt; // peaks since the previous call (linear)
    };
    void live(Live& l) {
        l.ready = engineReady_; l.running = running_; l.lost = deviceLost_;
        l.buf = bufSize_; l.prime = outPrime_; l.dev = devFrames_; l.sr = sampleRate_; l.underruns = underrunsLive_;
        for (int c = 0; c < 2; c++) { l.in[c] = pkIn_[c].exchange(0); l.out[c] = pkOut_[c].exchange(0); }
        l.yt = pkYt_.exchange(0);
    }
    // settings that take effect immediately, without reopening the device
    void applyLive(const Config& c) {
        liveVol_ = c.ytVolume; liveYtLat_ = c.ytLatencyMs; liveMono_ = c.monoAuto; liveMeter_ = c.debugMeter;
    }
    void setLiveVolume(float v) { liveVol_ = v; }
    bool canReset() const { return engineReady_ && cb_ && cb_->asioMessage; }
    void resetFromPanel() { requestReset("settings changed in the control panel"); }

private:
    static void pubMax(std::atomic<float>& a, float v) { if (v > a.load(std::memory_order_relaxed)) a.store(v, std::memory_order_relaxed); }
    void setErr(const char* s) { strncpy_s(err_, s, _TRUNCATE); }
    void restoreDefault() {
        if (!switchedDefault_) return;
        switchedDefault_ = false;
        // only if nobody changed the default output in the meantime
        if (lower(getDefaultRender()) == lower(loopId_) && setDefaultRender(renderId_))
            logf("Windows default output restored to the UMC22");
    }
    void freeBufs() {
        for (int c = 0; c < 2; c++) { inBuf_[c][0] = inBuf_[c][1] = outBuf_[c][0] = outBuf_[c][1] = nullptr; }
        mem_.clear(); bufSize_ = 0;
    }

    // ---------------------------------------------------- main audio thread
    // The exclusive WASAPI streams of the UMC22 (render + capture)
    struct Streams {
        IAudioClient* rCl = nullptr; IAudioClient* cCl = nullptr;
        IAudioRenderClient* rr = nullptr; IAudioCaptureClient* cc = nullptr;
        DevFmt rf, cf; UINT32 rFrames = 0, cFrames = 0;
        bool open() const { return rr && cc; }
        void release() {
            if (rr) rr->Release(); if (cc) cc->Release();
            if (rCl) rCl->Release(); if (cCl) cCl->Release();
            rr = nullptr; cc = nullptr; rCl = nullptr; cCl = nullptr;
        }
    };

    HRESULT openStreams(IMMDeviceEnumerator* en, HANDLE rEv, HANDLE cEv, Streams& s, bool quiet) {
        IMMDevice* rDev = nullptr; IMMDevice* cDev = nullptr;
        const int sr = (int)sampleRate_;
        HRESULT hr = en->GetDevice(renderId_.c_str(), &rDev);
        if (SUCCEEDED(hr)) hr = en->GetDevice(captureId_.c_str(), &cDev);
        // retry briefly in case an app is still releasing the UMC22
        for (int tries = 0; SUCCEEDED(hr) || hr == AUDCLNT_E_DEVICE_IN_USE; tries++) {
            if (tries) Sleep(100);
            hr = openExclusive(rDev, sr, bufSize_, rEv, &s.rCl, s.rf, s.rFrames);
            if (hr != AUDCLNT_E_DEVICE_IN_USE || tries >= 10) break;
        }
        if (FAILED(hr) && !quiet) logf("render open hr=0x%08lX", (unsigned long)hr);
        if (SUCCEEDED(hr)) { hr = openExclusive(cDev, sr, (int)s.rFrames, cEv, &s.cCl, s.cf, s.cFrames); if (FAILED(hr) && !quiet) logf("capture open hr=0x%08lX", (unsigned long)hr); }
        if (SUCCEEDED(hr)) hr = s.rCl->GetService(IID_PPV_ARGS(&s.rr));
        if (SUCCEEDED(hr)) hr = s.cCl->GetService(IID_PPV_ARGS(&s.cc));
        if (SUCCEEDED(hr)) logf("devices: render %d ch %d-bit %u frames | capture %d ch %d-bit %u frames",
                                s.rf.ch, s.rf.bytes * 8, s.rFrames, s.cf.ch, s.cf.bytes * 8, s.cFrames);
        else s.release();
        if (rDev) rDev->Release(); if (cDev) cDev->Release();
        return hr;
    }

    // When the ASIO buffer equals the device period, the host output of a block can go straight into
    // the device buffer of the same period; otherwise one ASIO buffer of slack is needed.
    void setDeviceFrames(UINT32 frames) {
        devFrames_ = frames;
        outPrime_ = (cfg_.lowLatency && bufSize_ == (long)frames) ? 0 : bufSize_;
    }

    // If Windows uses the UMC22 as default output, move it to the loopback device to free the UMC22
    void moveDefaultOff() {
        if (!cfg_.autoDefault || loopId_.empty()) return;
        if (lower(getDefaultRender()) == lower(renderId_) && setDefaultRender(loopId_)) {
            switchedDefault_ = true;
            logf("Windows default output -> loopback device (restored on close)");
            Sleep(150); // give apps time to migrate their streams
        }
    }

    // Called every ~0.5 s while the UMC22 is missing. Returns true once it is open again.
    bool tryReconnect(IMMDeviceEnumerator* en, HANDLE rEv, HANDLE cEv, Streams& s, HRESULT& lastHr, ULONGLONG& seenAt) {
        for (int i = 0; i < 5 && running_; i++) Sleep(100);
        if (!running_) return false;
        std::wstring n1, n2;
        std::wstring r = findEndpoint(en, eRender, cfg_.render, &n1);
        std::wstring c = findEndpoint(en, eCapture, cfg_.capture, &n2);
        if (r.empty() || c.empty()) { seenAt = 0; return false; }
        // Right after it appears, Windows is still setting the UMC22 up and restarts its input once:
        // a stream opened in that window stops delivering audio. Let it settle first.
        if (!seenAt) { seenAt = GetTickCount64(); logf("UMC22 is back, letting Windows set it up"); }
        if (GetTickCount64() - seenAt < 1500) return false;
        // the IDs can change if it was plugged into another USB port
        renderId_ = r; captureId_ = c;
        if (loopId_ == renderId_) loopId_.clear();
        moveDefaultOff(); // Windows may have made it the default output again when it came back
        HRESULT hr = openStreams(en, rEv, cEv, s, true);
        if (FAILED(hr)) {
            if (hr != lastHr) logf("UMC22 is back but cannot be opened yet hr=0x%08lX, retrying", (unsigned long)hr);
            lastHr = hr;
            return false;
        }
        bool periodChanged = s.rFrames != devFrames_;
        setDeviceFrames(s.rFrames);
        logf("UMC22 reconnected: render -> %ls", n1.c_str());
        if (periodChanged) requestReset("device period changed after reconnecting");
        return true;
    }

    void engineThread(HANDLE opened) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IMMDeviceEnumerator* en = nullptr;
        HANDLE rEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE cEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Streams s;

        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
        if (SUCCEEDED(hr)) hr = openStreams(en, rEv, cEv, s, false);
        setDeviceFrames(s.rFrames);
        openResult_ = hr;
        SetEvent(opened);

        if (SUCCEEDED(hr)) {
            for (;;) {
                WaitForSingleObject(startEv_, INFINITE);
                if (quit_) break;
                ULONGLONG lostAt = 0, seenAt = 0; HRESULT lastHr = S_OK;
                while (running_) {
                    if (!s.open()) {
                        // UMC22 unplugged: the host keeps the driver; we reopen it as soon as it is back
                        if (!lostAt) { lostAt = GetTickCount64(); deviceLost_ = true; logf("UMC22 unavailable: waiting to reopen it"); }
                        if (!tryReconnect(en, rEv, cEv, s, lastHr, seenAt)) continue;
                        logf("audio resumed after %.1f s", (GetTickCount64() - lostAt) / 1000.0);
                        lostAt = 0; seenAt = 0; lastHr = S_OK; deviceLost_ = false;
                    }
                    if (runStream(s, rEv, cEv)) break; // stopped by the host
                    s.release();                       // device lost
                }
                SetEvent(stoppedEv_);
            }
        }
        s.release();
        if (en) en->Release();
        CloseHandle(rEv); CloseHandle(cEv);
        CoUninitialize();
    }

    void processBlock() {
        const long N = bufSize_; const int idx = bufIdx_;
        float pkL = 0, pkR = 0;
        for (long i = 0; i < N; i++) {
            float l = 0, r = 0; inF_.pop(l, r);
            pkL = std::max(pkL, std::fabs(l)); pkR = std::max(pkR, std::fabs(r));
            if (inBuf_[0][idx]) inBuf_[0][idx][i] = f2i(l);
            if (inBuf_[1][idx]) inBuf_[1][idx][i] = f2i(r);
        }
        pubMax(pkIn_[0], pkL); pubMax(pkIn_[1], pkR);
        LARGE_INTEGER q, f; QueryPerformanceCounter(&q); QueryPerformanceFrequency(&f);
        stampNs_ = (uint64_t)((double)q.QuadPart * 1e9 / (double)f.QuadPart);
        cb_->bufferSwitch(idx, 1);
        samplePos_ += N;
        // If the host only sends the guitar on L (R missing or silent), copy it to R
        bool copyL = !outBuf_[1][idx] && outBuf_[0][idx];
        if (liveMono_.load(std::memory_order_relaxed) && !copyL && outBuf_[0][idx] && outBuf_[1][idx]) {
            int32_t ml = 0, mr = 0;
            for (long i = 0; i < N; i++) { ml |= outBuf_[0][idx][i]; mr |= outBuf_[1][idx][i]; }
            // hysteresis: signal on R -> stereo immediately; signal on L with R silent for >1 s -> copy L to R
            if (mr != 0) monoSilentFrames_ = 0;
            else if (ml != 0) monoSilentFrames_ += N;
            copyL = monoSilentFrames_ > (int64_t)sampleRate_;
        }
        if (copyL != monoCopying_) { monoCopying_ = copyL; logf("mono guitar L->R: %s", copyL ? "on" : "off"); }
        for (long i = 0; i < N; i++) {
            float l = outBuf_[0][idx] ? i2f(outBuf_[0][idx][i]) : 0.0f;
            float r = copyL ? l : (outBuf_[1][idx] ? i2f(outBuf_[1][idx][i]) : 0.0f);
            outF_.push(l, r);
        }
        bufIdx_ ^= 1;
    }

    // Streams until the host stops (returns true) or the UMC22 disappears (returns false).
    bool runStream(Streams& s, HANDLE rEv, HANDLE cEv) {
        IAudioClient* rCl = s.rCl; IAudioClient* cCl = s.cCl;
        IAudioRenderClient* rr = s.rr; IAudioCaptureClient* cc = s.cc;
        const DevFmt& rf = s.rf; const DevFmt& cf = s.cf; const UINT32 B = s.rFrames;
        bool lost = false;
        DWORD task = 0; HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
        if (mm) AvSetMmThreadPriority(mm, AVRT_PRIORITY_CRITICAL);
        const long N = bufSize_;
        inF_.init(65536); outF_.init(65536); bufIdx_ = 0;
        const long prime = outPrime_;
        for (long i = 0; i < prime; i++) outF_.push(0, 0);
        YtMix yt;
        const double dstRate = sampleRate_;
        Limiter lim; lim.init(dstRate, 150.0);
        Dither dth;
        float pkGtr = 0, pkYt = 0; uint64_t limFrames = 0;
        // periodic meter (every 5 s) for diagnostics
        struct Meter { double gl = 0, gr = 0, glr = 0, yl = 0, yr = 0, ylr = 0, ol = 0, orr = 0, olr = 0; float pg = 0, py = 0, po = 0; uint64_t lim = 0, n = 0; } mt;
        auto corr = [](double a, double b, double ab) { return ab / std::sqrt(a * b + 1e-30); };
        auto dbf = [](double x) { return x > 1e-10 ? 20.0 * std::log10(x) : -99.0; };
        ULONGLONG lastMeter = GetTickCount64();
        monoCopying_ = false; monoSilentFrames_ = 0;

        BYTE* p = nullptr;
        if (SUCCEEDED(rr->GetBuffer(B, &p))) rr->ReleaseBuffer(B, AUDCLNT_BUFFERFLAGS_SILENT);
        cCl->Start(); rCl->Start();
        unsigned underruns = 0; uint64_t capFrames = 0, rEvents = 0, dupFrames = 0; ULONGLONG t0 = GetTickCount64();
        uint64_t expectPos = 0; bool havePos = false;
        ULONGLONG lastCap = t0;
        HANDLE evs[2] = { cEv, rEv };

        while (running_) {
            DWORD w = WaitForMultipleObjects(2, evs, FALSE, 500);
            if (w == WAIT_TIMEOUT) {
                // an unplugged device can simply stop signalling: ask WASAPI whether it is still there
                UINT32 pad = 0;
                HRESULT phr = rCl->GetCurrentPadding(&pad);
                if (phr == AUDCLNT_E_DEVICE_INVALIDATED || FAILED(cCl->GetCurrentPadding(&pad))) { lost = true; logf("UMC22 stopped responding"); break; }
                logf("timeout waiting for the UMC22");
                continue;
            }
            bool capSig = (w == WAIT_OBJECT_0), renSig = (w == WAIT_OBJECT_0 + 1);
            if (capSig && WaitForSingleObject(rEv, 0) == WAIT_OBJECT_0) renSig = true;

            // 1) input: only when the capture event fires; drop repeated packets by device position
            HRESULT hr = S_OK; UINT32 pk = 0;
            while (capSig && SUCCEEDED(hr = cc->GetNextPacketSize(&pk)) && pk) {
                BYTE* d; UINT32 fr; DWORD fl; UINT64 devPos = 0;
                if (FAILED(hr = cc->GetBuffer(&d, &fr, &fl, &devPos, nullptr))) break;
                UINT32 skip = 0;
                if (havePos && devPos < expectPos) skip = (UINT32)std::min<uint64_t>(fr, expectPos - devPos);
                if (!havePos || devPos + fr > expectPos) expectPos = devPos + fr;
                havePos = true; dupFrames += skip;
                for (UINT32 i = skip; i < fr; i++) {
                    if (fl & AUDCLNT_BUFFERFLAGS_SILENT) { inF_.push(0, 0); continue; }
                    const BYTE* s = d + (size_t)i * cf.block;
                    float l = rdS(s, cf);
                    float r = cf.ch > 1 ? rdS(s + cf.bytes, cf) : l;
                    inF_.push(l, r);
                }
                capFrames += fr - skip;
                lastCap = GetTickCount64();
                cc->ReleaseBuffer(fr);
            }
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED) { lost = true; logf("capture device invalidated"); break; }
            if (FAILED(hr)) { lost = true; logf("capture error hr=0x%08lX: reopening", (unsigned long)hr); break; }
            if (!renSig) continue;
            rEvents++;

            // Watchdog: the input can stop delivering audio without any error (e.g. Windows restarted the
            // device under us) while the output keeps running. Reopen both streams in that case.
            ULONGLONG now = GetTickCount64();
            if (now - t0 > 1000 && now - lastCap > 250) {
                UINT32 pend = 0; HRESULT phr = cc->GetNextPacketSize(&pend);
                if (FAILED(phr) || !pend) { lost = true; logf("input stopped delivering audio (hr=0x%08lX): reopening", (unsigned long)phr); break; }
            }

            // 2) process ASIO blocks
            while (inF_.n >= (size_t)N && running_) processBlock();

            // 3) output + Windows audio mix
            hr = rr->GetBuffer(B, &p);
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED) { lost = true; logf("render device invalidated"); break; }
            if (FAILED(hr)) continue;
            bool ytOn = ytActive_.load(std::memory_order_acquire);
            if (ytOn) yt.block(ytRing_, ytSrcRate_, dstRate, liveYtLat_.load(std::memory_order_relaxed));
            const float vol = liveVol_.load(std::memory_order_relaxed);
            float pkOL = 0, pkOR = 0, pkY = 0;
            if (outF_.n < B) {
                // the first render events can arrive before any input (start-up): not a real glitch
                if (rEvents > 2) { underruns++; underrunsLive_++; if (underruns <= 5) logf("underrun at %.3f s (%zu of %u frames ready)", (GetTickCount64() - t0) / 1000.0, outF_.n, B); }
            }
            const bool dith = !rf.flt && rf.bytes == 2;
            for (UINT32 i = 0; i < B; i++) {
                float l = 0, r = 0; outF_.pop(l, r);
                pkGtr = std::max(pkGtr, std::max(std::fabs(l), std::fabs(r)));
                mt.gl += l * l; mt.gr += r * r; mt.glr += l * r; mt.pg = std::max(mt.pg, std::max(std::fabs(l), std::fabs(r)));
                if (ytOn) {
                    float yl, yr; yt.next(ytRing_, yl, yr); yl *= vol; yr *= vol;
                    pkYt = std::max(pkYt, std::max(std::fabs(yl), std::fabs(yr)));
                    pkY = std::max(pkY, std::max(std::fabs(yl), std::fabs(yr)));
                    mt.yl += yl * yl; mt.yr += yr * yr; mt.ylr += yl * yr; mt.py = std::max(mt.py, std::max(std::fabs(yl), std::fabs(yr)));
                    l += yl; r += yr;
                }
                lim.process(l, r);
                pkOL = std::max(pkOL, std::fabs(l)); pkOR = std::max(pkOR, std::fabs(r));
                if (lim.g < 0.98f) { limFrames++; mt.lim++; }
                mt.ol += l * l; mt.orr += r * r; mt.olr += l * r; mt.po = std::max(mt.po, std::max(std::fabs(l), std::fabs(r))); mt.n++;
                if (dith) { l += dth.tpdf(1.0f / 32768.0f); r += dth.tpdf(1.0f / 32768.0f); }
                BYTE* o = p + (size_t)i * rf.block;
                wrS(o, rf, l);
                if (rf.ch > 1) wrS(o + rf.bytes, rf, r);
                for (int c = 2; c < rf.ch; c++) wrS(o + c * rf.bytes, rf, 0);
            }
            rr->ReleaseBuffer(B, 0);
            pubMax(pkOut_[0], pkOL); pubMax(pkOut_[1], pkOR); pubMax(pkYt_, pkY);
            if (liveMeter_.load(std::memory_order_relaxed) &&GetTickCount64() - lastMeter >= 5000 && mt.n) {
                double n = (double)mt.n;
                logf("meter: guitar RMS L %.1f R %.1f peak %.1f corr %.2f | loopback RMS L %.1f R %.1f peak %.1f corr %.2f | output RMS L %.1f R %.1f peak %.1f corr %.2f | limiter %.1f%% | mono %s",
                     dbf(std::sqrt(mt.gl / n)), dbf(std::sqrt(mt.gr / n)), dbf(mt.pg), corr(mt.gl, mt.gr, mt.glr),
                     dbf(std::sqrt(mt.yl / n)), dbf(std::sqrt(mt.yr / n)), dbf(mt.py), corr(mt.yl, mt.yr, mt.ylr),
                     dbf(std::sqrt(mt.ol / n)), dbf(std::sqrt(mt.orr / n)), dbf(mt.po), corr(mt.ol, mt.orr, mt.olr),
                     100.0 * mt.lim / n, monoCopying_ ? "on" : "off");
                mt = Meter{}; lastMeter = GetTickCount64();
            }
            // cap latency if it builds up
            if (outF_.n > (size_t)(prime + 2 * B)) outF_.drop(outF_.n - (prime + B));
        }
        rCl->Stop(); cCl->Stop(); rCl->Reset(); cCl->Reset();
        double secs = (GetTickCount64() - t0) / 1000.0;
        logf("stats: %.1f s, render events %.0f/s, capture %.0f frames/s, duplicates %llu, underruns %u", secs, rEvents / secs, capFrames / secs, (unsigned long long)dupFrames, underruns);
        auto db = [](float x) { return x > 0 ? 20.0 * std::log10(x) : -99.0; };
        logf("levels: guitar peak %.1f dBFS, loopback peak %.1f dBFS, limiter active %.1f%% of the time",
             db(pkGtr), db(pkYt), 100.0 * limFrames / std::max(1.0, secs * dstRate));
        if (mm) AvRevertMmThreadCharacteristics(mm);
        return !lost;
    }

    void requestReset(const char* why) {
        logf("reset: %s", why);
        if (cb_ && cb_->asioMessage && cb_->asioMessage(kAsioSelectorSupported, kAsioResetRequest, nullptr, nullptr))
            cb_->asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
    }

    // ---------------------------------------------------- loopback (Windows/YouTube audio)
    void startYt() {
        if (loopId_.empty() || ytThread_.joinable()) return;
        ytRun_ = true;
        ytThread_ = std::thread([this] { ytLoop(); });
    }
    void stopYt() {
        ytRun_ = false;
        if (ytThread_.joinable()) ytThread_.join();
        ytActive_ = false;
    }
    void ytLoop() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        timeBeginPeriod(1);
        IMMDeviceEnumerator* en = nullptr; IMMDevice* dev = nullptr; IAudioClient* ac = nullptr;
        IAudioCaptureClient* cc = nullptr; WAVEFORMATEX* mix = nullptr;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
        if (SUCCEEDED(hr)) hr = en->GetDevice(loopId_.c_str(), &dev);
        if (SUCCEEDED(hr)) hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac);
        if (SUCCEEDED(hr)) hr = ac->GetMixFormat(&mix);
        if (SUCCEEDED(hr)) hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, 0, mix, nullptr);
        if (SUCCEEDED(hr)) hr = ac->GetService(IID_PPV_ARGS(&cc));
        if (SUCCEEDED(hr)) {
            DevFmt f = fmtFromWave(mix);
            ytSrcRate_ = mix->nSamplesPerSec;
            ytRing_.init((size_t)mix->nSamplesPerSec); // 1 s
            ytActive_.store(true, std::memory_order_release);
            ac->Start();
            logf("loopback active: %d ch %d Hz %s", f.ch, (int)mix->nSamplesPerSec, f.flt ? "float" : "pcm");
            while (ytRun_) {
                Sleep(3);
                UINT32 pk = 0;
                while (SUCCEEDED(cc->GetNextPacketSize(&pk)) && pk) {
                    BYTE* d; UINT32 fr; DWORD fl;
                    if (FAILED(cc->GetBuffer(&d, &fr, &fl, nullptr, nullptr))) break;
                    for (UINT32 i = 0; i < fr; i++) {
                        if (fl & AUDCLNT_BUFFERFLAGS_SILENT) { ytRing_.push(0, 0); continue; }
                        const BYTE* s = d + (size_t)i * f.block;
                        float l = rdS(s, f), r = f.ch > 1 ? rdS(s + f.bytes, f) : l;
                        ytRing_.push(l, r);
                    }
                    cc->ReleaseBuffer(fr);
                }
            }
            ac->Stop();
        } else {
            logf("loopback unavailable hr=0x%08lX (Windows audio will not be mixed)", (unsigned long)hr);
        }
        ytActive_ = false;
        if (mix) CoTaskMemFree(mix);
        if (cc) cc->Release(); if (ac) ac->Release(); if (dev) dev->Release(); if (en) en->Release();
        timeEndPeriod(1);
        CoUninitialize();
    }

    std::atomic<ULONG> ref_{ 1 };
    Config cfg_;
    std::wstring renderId_, captureId_, loopId_;
    char err_[124] = "";
    double sampleRate_ = 48000.0;
    long bufSize_ = 0;
    ASIOCallbacks* cb_ = nullptr;
    std::vector<int32_t> mem_;
    int32_t* inBuf_[2][2]{};
    int32_t* outBuf_[2][2]{};
    int bufIdx_ = 0;
    bool monoCopying_ = false;
    bool switchedDefault_ = false;
    int64_t monoSilentFrames_ = 0;
    std::atomic<uint64_t> samplePos_{ 0 }, stampNs_{ 0 };

    std::thread engine_;
    HANDLE startEv_ = nullptr, stoppedEv_ = nullptr;
    std::atomic<bool> quit_{ false }, running_{ false }, engineReady_{ false };
    HRESULT openResult_ = E_FAIL;
    UINT32 devFrames_ = 0;
    long outPrime_ = 0; // silence queued ahead of the host output (= extra output latency)
    Fifo inF_, outF_;

    std::thread ytThread_;
    std::atomic<bool> ytRun_{ false }, ytActive_{ false };
    Spsc ytRing_;
    double ytSrcRate_ = 48000.0;

    // live settings (written by the control panel) and meters (read by it)
    std::atomic<float> liveVol_{ 1.0f }, liveYtLat_{ 40.0f };
    std::atomic<bool> liveMono_{ true }, liveMeter_{ false }, deviceLost_{ false };
    std::atomic<unsigned> underrunsLive_{ 0 };
    std::atomic<float> pkIn_[2]{}, pkOut_[2]{}, pkYt_{ 0.0f };
};

// ============================================================ Control panel
// Native Win32 settings window. Opened by the host (ASIO controlPanel) or standalone with
//   rundll32 umc22asio.dll,ControlPanel
// It edits config.ini; live settings apply immediately, the rest via an ASIO reset request.
struct DevEntry { std::wstring name, id; bool exclusive = true; };

// "Allow applications to take exclusive control of this device" (Sound > device > Advanced).
// Windows only writes this property when the box is unchecked (value 0).
static const PROPERTYKEY PKEY_ExclusiveModeAllowed = { { 0xb3f8fa53, 0x0004, 0x438e, { 0x90, 0x03, 0x51, 0xa4, 0x6e, 0x13, 0x9b, 0xfc } }, 3 };

static void listEndpoints(EDataFlow flow, std::vector<DevEntry>& out) {
    runMta([&] {
        IMMDeviceEnumerator* en = nullptr; IMMDeviceCollection* col = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return;
        if (SUCCEEDED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col))) {
            UINT n = 0; col->GetCount(&n);
            for (UINT i = 0; i < n; i++) {
                IMMDevice* d = nullptr; if (FAILED(col->Item(i, &d))) continue;
                IPropertyStore* ps = nullptr; LPWSTR id = nullptr;
                if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
                    PROPVARIANT v; PropVariantInit(&v);
                    if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR && SUCCEEDED(d->GetId(&id))) {
                        DevEntry e{ v.pwszVal, id }; CoTaskMemFree(id);
                        PROPVARIANT x; PropVariantInit(&x);
                        if (SUCCEEDED(ps->GetValue(PKEY_ExclusiveModeAllowed, &x))) {
                            if (x.vt == VT_UI4) e.exclusive = x.ulVal != 0;
                            else if (x.vt == VT_BOOL) e.exclusive = x.boolVal != VARIANT_FALSE;
                        }
                        PropVariantClear(&x);
                        out.push_back(e);
                    }
                    PropVariantClear(&v); ps->Release();
                }
                d->Release();
            }
            col->Release();
        }
        en->Release();
    });
}

static void writeConfig(const Config& c) {
    ensureConfig();
    std::wstring p = configPath(); wchar_t b[64];
    auto put = [&](const wchar_t* k, const std::wstring& v) { WritePrivateProfileStringW(L"driver", k, v.c_str(), p.c_str()); };
    put(L"render", c.render); put(L"capture", c.capture); put(L"loopback", c.loopback);
    swprintf_s(b, L"%.2f", c.ytVolume); put(L"ytVolume", b);
    put(L"monoAuto", c.monoAuto ? L"1" : L"0");
    swprintf_s(b, L"%.0f", c.ytLatencyMs); put(L"ytLatencyMs", b);
    put(L"preferredBuffer", std::to_wstring(c.preferredBuffer));
    put(L"autoDefault", c.autoDefault ? L"1" : L"0");
    put(L"debugMeter", c.debugMeter ? L"1" : L"0");
    put(L"lowLatency", c.lowLatency ? L"1" : L"0");
}

class Panel {
public:
    static void show(HWND owner, Driver* drv) {
        // v6 common controls through the manifest embedded in this DLL (resource ID 2)
        static HANDLE actCtx = [] {
            ACTCTXW ac{ sizeof ac };
            ac.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;
            ac.hModule = g_hInst; ac.lpResourceName = MAKEINTRESOURCEW(2);
            return CreateActCtxW(&ac);
        }();
        ULONG_PTR cookie = 0;
        bool act = actCtx != INVALID_HANDLE_VALUE && ActivateActCtx(actCtx, &cookie);
        { Panel p(drv); p.run(owner); }
        if (act) DeactivateActCtx(0, cookie);
    }

private:
    enum {
        ID_RENDER = 101, ID_CAPTURE, ID_LOOP, ID_BUF, ID_LOWLAT, ID_VOL, ID_VOLTXT, ID_YTLAT, ID_MONO, ID_AUTODEF,
        ID_METER, ID_STATUS, ID_M_IN, ID_M_OUT, ID_M_YT, ID_OPENLOG, ID_OPENCFG, ID_APPLY, ID_CHECKS, ID_SOUNDCPL,
        ID_HELP = 300 // help "?" icons: ID_HELP, ID_HELP + 1...
    };
    struct CheckItem { int level; std::wstring text; }; // level: 0 ok, 1 warning, 2 problem
    Driver* drv_; HWND hwnd_ = nullptr, tip_ = nullptr; HFONT font_ = nullptr, bold_ = nullptr; int dpi_ = 96;
    Config cfg_, orig_;
    std::vector<DevEntry> ren_, cap_;
    std::vector<CheckItem> checks_;
    bool done_ = false, pendingReset_ = false;
    int helpCount_ = 0, ticks_ = 0;
    float dIn_[2]{ -90, -90 }, dOut_[2]{ -90, -90 }, dYt_ = -90;
    std::wstring lastStatus_;

    explicit Panel(Driver* d) : drv_(d) {}
    ~Panel() { if (font_) DeleteObject(font_); if (bold_) DeleteObject(bold_); }
    int S(int v) const { return MulDiv(v, dpi_, 96); }
    HWND ctl(int id) const { return GetDlgItem(hwnd_, id); }

    HWND add(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, bool bold = false) {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), hwnd_,
                                 (HMENU)(INT_PTR)id, g_hInst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)(bold ? bold_ : font_), FALSE);
        return c;
    }
    void label(const wchar_t* t, int x, int y, int w, bool bold = false) { add(L"STATIC", t, SS_LEFT | SS_NOPREFIX, x, y, w, 18, -1, bold); }
    HWND combo(int id, int x, int y, int w) { return add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, x, y, w, 300, id); }
    void check(int id, const wchar_t* t, int x, int y, int w, bool on) {
        add(WC_BUTTONW, t, BS_AUTOCHECKBOX | WS_TABSTOP, x, y, w, 20, id);
        SendMessageW(ctl(id), BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    int addItem(int id, const std::wstring& t, LPARAM data) {
        int i = (int)SendMessageW(ctl(id), CB_ADDSTRING, 0, (LPARAM)t.c_str());
        SendMessageW(ctl(id), CB_SETITEMDATA, i, data); return i;
    }
    static bool matches(const std::wstring& name, const std::wstring& needle) {
        return !needle.empty() && lower(name).find(lower(needle)) != std::wstring::npos;
    }

    // device list; data = index in 'list', -1 = disabled, -2 = configured but not connected
    void fillDevices(int id, const std::vector<DevEntry>& list, const std::wstring& value, bool allowNone) {
        int sel = -1;
        if (allowNone) { int i = addItem(id, L"(disabled)", -1); if (value.empty()) sel = i; }
        for (size_t k = 0; k < list.size(); k++) {
            int i = addItem(id, list[k].name, (LPARAM)k);
            if (sel < 0 && matches(list[k].name, value)) sel = i;
        }
        if (sel < 0 && !value.empty()) sel = addItem(id, value + L"  (not connected)", -2);
        SendMessageW(ctl(id), CB_SETCURSEL, sel < 0 ? 0 : sel, 0);
    }
    // keeps the configured substring if it still selects the chosen device (e.g. "USB Audio CODEC"
    // survives the "2- " prefix Windows adds when the UMC22 moves to another USB port)
    std::wstring pickedDevice(int id, const std::vector<DevEntry>& list, const std::wstring& current) {
        int i = (int)SendMessageW(ctl(id), CB_GETCURSEL, 0, 0);
        LPARAM k = SendMessageW(ctl(id), CB_GETITEMDATA, i, 0);
        if (k == -1) return L"";
        if (k < 0 || (size_t)k >= list.size()) return current;
        return matches(list[k].name, current) ? current : list[k].name;
    }
    // round "?" icon at (x, y) that shows 'text' on hover
    void help(int x, int y, const wchar_t* text) {
        HWND h = add(L"STATIC", L"", SS_OWNERDRAW | SS_NOTIFY, x, y, 18, 18, ID_HELP + helpCount_++);
        TTTOOLINFOW ti{ sizeof ti };
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS; ti.hwnd = hwnd_; ti.uId = (UINT_PTR)h; ti.lpszText = (LPWSTR)text;
        SendMessageW(tip_, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }

    // what the driver needs to work, re-evaluated every 2 s while the window is open
    std::vector<CheckItem> evaluate() {
        std::vector<DevEntry> ren, cap; listEndpoints(eRender, ren); listEndpoints(eCapture, cap);
        auto find = [](const std::vector<DevEntry>& l, const std::wstring& n) -> const DevEntry* {
            for (auto& e : l) if (matches(e.name, n)) return &e;
            return nullptr;
        };
        const DevEntry* r = find(ren, cfg_.render); const DevEntry* c = find(cap, cfg_.capture);
        const DevEntry* lb = cfg_.loopback.empty() ? nullptr : find(ren, cfg_.loopback);
        std::vector<CheckItem> v;
        if (r) v.push_back({ 0, L"Output connected: " + r->name });
        else v.push_back({ 2, L"Output not found: plug in the UMC22 or pick it above" });
        if (c) v.push_back({ 0, L"Input connected: " + c->name });
        else v.push_back({ 2, L"Input not found: plug in the UMC22 or pick it above" });
        if (r || c) {
            bool ok = (!r || r->exclusive) && (!c || c->exclusive);
            if (ok) v.push_back({ 0, L"Exclusive mode allowed on the UMC22" });
            else v.push_back({ 2, std::wstring(L"Exclusive mode disabled on the ") + (r && !r->exclusive ? (c && !c->exclusive ? L"output and input" : L"output") : L"input") +
                                  L": Sound settings > UMC22 > Properties > Advanced" });
        }
        if (cfg_.loopback.empty()) v.push_back({ 1, L"Windows audio mixing is disabled" });
        else if (!lb) v.push_back({ 2, L"Windows audio device not found: pick another one above" });
        else if (r && lb->id == r->id) v.push_back({ 2, L"The Windows audio device must not be the UMC22 itself" });
        else v.push_back({ 0, L"Windows audio mixed from: " + lb->name });
        // registration: which DLL do ASIO hosts load?
        wchar_t reg[MAX_PATH] = L"", self[MAX_PATH] = L""; DWORD sz = sizeof reg;
        GetModuleFileNameW(g_hInst, self, MAX_PATH);
        std::wstring key = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kClsidStr + L"\\InprocServer32";
        bool registered = RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, reg, &sz) == ERROR_SUCCESS &&
                          RegGetValueW(HKEY_LOCAL_MACHINE, (std::wstring(L"SOFTWARE\\ASIO\\") + kDriverNameW).c_str(), L"CLSID", RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
        if (!registered) v.push_back({ 2, L"Driver not registered: run the installer (or install.bat)" });
        else if (_wcsicmp(reg, self) != 0) v.push_back({ 1, std::wstring(L"Hosts load the driver from another folder: ") + reg });
        else v.push_back({ 0, L"Driver registered for ASIO hosts" });
        return v;
    }
    void refreshChecks() {
        auto v = evaluate();
        bool same = v.size() == checks_.size();
        for (size_t i = 0; same && i < v.size(); i++) same = v[i].level == checks_[i].level && v[i].text == checks_[i].text;
        if (same) return;
        checks_ = std::move(v);
        InvalidateRect(ctl(ID_CHECKS), nullptr, TRUE);
    }

    LPARAM comboData(int id) { return SendMessageW(ctl(id), CB_GETITEMDATA, SendMessageW(ctl(id), CB_GETCURSEL, 0, 0), 0); }
    bool checked(int id) { return SendMessageW(ctl(id), BM_GETCHECK, 0, 0) == BST_CHECKED; }
    int volume() { return (int)SendMessageW(ctl(ID_VOL), TBM_GETPOS, 0, 0); }

    void build() {
        const int L = 16, X = 180, W = 324, H = 514; // H = column of the "?" icons
        tip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                               0, 0, 0, 0, hwnd_, nullptr, g_hInst, nullptr);
        SendMessageW(tip_, TTM_SETMAXTIPWIDTH, 0, S(340));
        SendMessageW(tip_, TTM_SETDELAYTIME, TTDT_INITIAL, 150);
        SendMessageW(tip_, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);

        label(L"Devices", L, 12, 200, true);
        label(L"UMC22 output", L, 39, 160);       combo(ID_RENDER, X, 35, W);
        help(H, 37, L"The UMC22 output, opened in exclusive mode. Your guitar and the Windows audio are mixed here: "
                    L"headphones and the monitor outputs.");
        label(L"UMC22 input", L, 69, 160);        combo(ID_CAPTURE, X, 65, W);
        help(H, 67, L"The UMC22 input. Choose In 1 or In 2 (where your guitar is plugged in) in your host.");
        label(L"Windows audio from", L, 99, 160); combo(ID_LOOP, X, 95, W);
        help(H, 97, L"While the driver runs, Windows plays to this device and the driver mixes what it plays into the "
                    L"UMC22 output, so YouTube, games and system sounds reach your headphones.\n\n"
                    L"The onboard audio works: nothing needs to be plugged into it. (disabled) = no Windows audio on the UMC22.");
        fillDevices(ID_RENDER, ren_, cfg_.render, false);
        fillDevices(ID_CAPTURE, cap_, cfg_.capture, false);
        fillDevices(ID_LOOP, ren_, cfg_.loopback, true);

        label(L"Audio", L, 136, 200, true);
        label(L"Buffer size", L, 163, 160); combo(ID_BUF, X, 159, 190);
        std::vector<long> sizes = { 32, 48, 64, 96, 128, 144, 192, 256, 288, 384, 512, 768, 1024, 2048 };
        if (std::find(sizes.begin(), sizes.end(), cfg_.preferredBuffer) == sizes.end()) { sizes.push_back(cfg_.preferredBuffer); std::sort(sizes.begin(), sizes.end()); }
        for (long s : sizes) {
            wchar_t t[64]; swprintf_s(t, L"%ld samples  (%.1f ms)", s, s * 1000.0 / 48000.0);
            int i = addItem(ID_BUF, t, s);
            if (s == cfg_.preferredBuffer) SendMessageW(ctl(ID_BUF), CB_SETCURSEL, i, 0);
        }
        help(H, 161, L"Buffer size the driver suggests to the host. 144 samples = 3 ms, the UMC22 minimum at 48 kHz.\n\n"
                     L"Larger buffers add latency but are safer on slow PCs. Your host may use its own setting.");
        check(ID_LOWLAT, L"Low latency output (one buffer less output latency)", X, 189, W, cfg_.lowLatency);
        help(H, 190, L"The driver runs the UMC22 at the host buffer size, so the host output can go straight to the "
                     L"device without an extra buffer: one buffer less output latency (3 ms at 144 samples).\n\n"
                     L"Turn it off if you hear clicks. If the UMC22 cannot run at the host buffer size, the extra "
                     L"buffer is used anyway.");

        label(L"Windows audio volume", L, 220, 160);
        add(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, X - 4, 216, 260, 26, ID_VOL);
        SendMessageW(ctl(ID_VOL), TBM_SETRANGE, TRUE, MAKELPARAM(0, 200));
        SendMessageW(ctl(ID_VOL), TBM_SETPAGESIZE, 0, 10);
        SendMessageW(ctl(ID_VOL), TBM_SETPOS, TRUE, (LPARAM)std::lround(cfg_.ytVolume * 100.0f));
        add(L"STATIC", L"", SS_RIGHT, X + 262, 220, 62, 18, ID_VOLTXT);
        updateVolText();
        help(H, 219, L"Volume of the Windows audio (YouTube, games...) in the UMC22 output, relative to your guitar. "
                     L"Applied immediately while you move it.");

        label(L"Windows audio buffer", L, 253, 160); combo(ID_YTLAT, X, 249, 190);
        std::vector<int> lats = { 20, 30, 40, 60, 80, 100, 150, 200 };
        int curLat = (int)std::lround(cfg_.ytLatencyMs);
        if (std::find(lats.begin(), lats.end(), curLat) == lats.end()) { lats.push_back(curLat); std::sort(lats.begin(), lats.end()); }
        for (int v : lats) {
            int i = addItem(ID_YTLAT, std::to_wstring(v) + L" ms" + (v == 40 ? L"  (default)" : L""), v);
            if (v == curLat) SendMessageW(ctl(ID_YTLAT), CB_SETCURSEL, i, 0);
        }
        help(H, 251, L"Safety margin for the Windows audio. Raise it (60-80 ms) if YouTube drops out or crackles. "
                     L"It does not affect the latency of your guitar.");
        check(ID_MONO, L"Copy a mono guitar on the left channel to the right", X, 280, W, cfg_.monoAuto);
        help(H, 281, L"Many amp sims send the guitar only on the left output. When the right channel stays silent for "
                     L"1 second, the left one is copied to it so you hear it on both sides.");
        check(ID_AUTODEF, L"Move Windows sounds off the UMC22 while in use", X, 304, W, cfg_.autoDefault);
        help(H, 305, L"Apps playing to the UMC22 would block exclusive mode. While the driver runs, the Windows default "
                     L"output is moved to the Windows audio device above, and moved back to the UMC22 when it stops.");
        check(ID_METER, L"Write levels to log.txt every 5 s (diagnostics)", X, 328, W, cfg_.debugMeter);
        help(H, 329, L"Writes guitar, Windows audio and output levels, stereo correlation and limiter activity to "
                     L"log.txt every 5 seconds. Only useful to diagnose a problem.");

        label(L"System check", L, 366, 200, true);
        add(WC_BUTTONW, L"Sound settings", BS_PUSHBUTTON | WS_TABSTOP, X + 214, 361, 110, 26, ID_SOUNDCPL);
        help(H, 365, L"What the driver needs to work, checked every 2 seconds.\n\n"
                     L"Sound settings opens the classic Windows sound panel, where exclusive mode is allowed: "
                     L"select the UMC22 > Properties > Advanced.");
        add(L"STATIC", L"", SS_OWNERDRAW, L, 392, 498, 5 * 22, ID_CHECKS);
        checks_ = evaluate();

        const int Y = 516; // status
        label(L"Status", L, Y, 200, true);
        add(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, L, Y + 24, 488, 36, ID_STATUS);
        if (drv_) { // level meters only make sense while a host has the driver open
            label(L"Input", L, Y + 68, 70);   add(L"STATIC", L"", SS_OWNERDRAW, 90, Y + 69, 414, 16, ID_M_IN);
            label(L"Output", L, Y + 94, 70);  add(L"STATIC", L"", SS_OWNERDRAW, 90, Y + 95, 414, 16, ID_M_OUT);
            label(L"Windows", L, Y + 120, 70); add(L"STATIC", L"", SS_OWNERDRAW, 90, Y + 124, 414, 7, ID_M_YT);
            help(H, Y + 68, L"Peak levels while the host runs. Input = what the UMC22 receives (your guitar), "
                            L"Output = everything sent to the UMC22, Windows = the Windows audio mixed in.\n\n"
                            L"Ticks every 12 dB. Red = closer than 3 dB to clipping.");
        }

        const int B = drv_ ? Y + 160 : Y + 76; // buttons
        add(WC_BUTTONW, L"Open log", BS_PUSHBUTTON | WS_TABSTOP, L, B, 84, 28, ID_OPENLOG);
        add(WC_BUTTONW, L"Edit config.ini", BS_PUSHBUTTON | WS_TABSTOP, L + 90, B, 110, 28, ID_OPENCFG);
        add(WC_BUTTONW, L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP, 252, B, 80, 28, IDOK);
        add(WC_BUTTONW, L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 338, B, 80, 28, IDCANCEL);
        add(WC_BUTTONW, L"Apply", BS_PUSHBUTTON | WS_TABSTOP, 424, B, 80, 28, ID_APPLY);
        updateStatus();
    }
    int clientHeight() const { return (drv_ ? 516 + 160 : 516 + 76) + 28 + 14; }

    static void fillCircle(HDC dc, int x, int y, int d, COLORREF c) {
        HBRUSH b = CreateSolidBrush(c); HPEN p = CreatePen(PS_SOLID, 1, c);
        HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
        Ellipse(dc, x, y, x + d, y + d);
        SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b); DeleteObject(p);
    }
    void glyph(HDC dc, RECT r, const wchar_t* t) {
        HGDIOBJ of = SelectObject(dc, bold_); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
        DrawTextW(dc, t, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, of);
    }
    void drawHelp(const DRAWITEMSTRUCT* di) {
        RECT rc = di->rcItem;
        FillRect(di->hDC, &rc, GetSysColorBrush(COLOR_BTNFACE));
        fillCircle(di->hDC, rc.left, rc.top, rc.right - rc.left, RGB(0, 103, 192));
        glyph(di->hDC, rc, L"?");
    }
    void drawChecks(const DRAWITEMSTRUCT* di) {
        HDC dc = di->hDC; RECT rc = di->rcItem;
        FillRect(dc, &rc, GetSysColorBrush(COLOR_BTNFACE));
        static const COLORREF col[] = { RGB(46, 160, 67), RGB(219, 154, 4), RGB(207, 34, 46) };
        static const wchar_t* sym[] = { L"\x2713", L"!", L"\x00D7" }; // check mark, !, multiplication sign
        int line = S(22), d = S(16);
        HGDIOBJ of = SelectObject(dc, font_);
        for (size_t i = 0; i < checks_.size(); i++) {
            int y = rc.top + (int)i * line;
            fillCircle(dc, rc.left, y + (line - d) / 2, d, col[checks_[i].level]);
            glyph(dc, RECT{ rc.left, y + (line - d) / 2, rc.left + d, y + (line - d) / 2 + d }, sym[checks_[i].level]);
            SelectObject(dc, font_); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
            RECT t{ rc.left + d + S(8), y, rc.right, y + line };
            DrawTextW(dc, checks_[i].text.c_str(), -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
        SelectObject(dc, of);
    }

    void updateVolText() {
        wchar_t t[16]; swprintf_s(t, L"%d %%", volume());
        SetWindowTextW(ctl(ID_VOLTXT), t);
    }

    void updateStatus() {
        std::wstring s;
        if (!drv_) {
            s = L"Not opened by an ASIO host. Changes apply the next time a host opens the driver.";
        } else {
            Driver::Live l; drv_->live(l);
            wchar_t t[256];
            if (!l.ready) s = L"Driver loaded, audio not started. Changes apply when the host starts it.";
            else if (l.lost) s = L"UMC22 disconnected: waiting for it to come back. Audio resumes automatically.";
            else {
                swprintf_s(t, L"%s  |  %ld samples @ %.0f Hz\nLatency: in %.1f ms / out %.1f ms  |  underruns since start: %u",
                           l.running ? L"Running" : L"Ready (stopped)", l.buf, l.sr,
                           l.dev * 1000.0 / l.sr, (l.dev + l.prime) * 1000.0 / l.sr, l.underruns);
                s = t;
            }
            auto db = [](float v) { return v > 1e-5f ? 20.0f * std::log10(v) : -90.0f; };
            auto fall = [&](float& d, float v) { d = std::max(db(v), d - 1.5f); };
            for (int c = 0; c < 2; c++) { fall(dIn_[c], l.in[c]); fall(dOut_[c], l.out[c]); }
            fall(dYt_, l.yt);
        }
        if (s != lastStatus_) { lastStatus_ = s; SetWindowTextW(ctl(ID_STATUS), s.c_str()); }
        if (drv_) for (int id : { ID_M_IN, ID_M_OUT, ID_M_YT }) InvalidateRect(ctl(id), nullptr, FALSE); // never NULL: that repaints the whole desktop
    }

    void drawMeter(const DRAWITEMSTRUCT* di) {
        RECT rc = di->rcItem; HDC dc = di->hDC;
        const float* v = di->CtlID == ID_M_IN ? dIn_ : di->CtlID == ID_M_OUT ? dOut_ : &dYt_;
        int bars = di->CtlID == ID_M_YT ? 1 : 2;
        HBRUSH bg = CreateSolidBrush(RGB(48, 48, 48)); FillRect(dc, &rc, bg); DeleteObject(bg);
        int w = rc.right - rc.left, gap = bars > 1 ? S(2) : 0, h = (rc.bottom - rc.top - gap) / bars;
        auto xAt = [&](float db) { return rc.left + (int)(w * (std::clamp(db, -60.0f, 0.0f) + 60.0f) / 60.0f); };
        struct { float from, to; COLORREF c; } zones[] = { { -60, -12, RGB(64, 192, 96) }, { -12, -3, RGB(230, 200, 60) }, { -3, 0, RGB(230, 70, 60) } };
        for (int b = 0; b < bars; b++) {
            int top = rc.top + b * (h + gap);
            for (auto& z : zones) {
                if (v[b] <= z.from) break;
                RECT r{ xAt(z.from), top, xAt(std::min(v[b], z.to)), top + h };
                HBRUSH br = CreateSolidBrush(z.c); FillRect(dc, &r, br); DeleteObject(br);
            }
        }
        // ticks every 12 dB
        HBRUSH tk = CreateSolidBrush(RGB(110, 110, 110));
        for (int db = -48; db < 0; db += 12) { RECT r{ xAt((float)db), rc.top, xAt((float)db) + 1, rc.bottom }; FillRect(dc, &r, tk); }
        DeleteObject(tk);
    }

    Config collect() {
        Config n = cfg_;
        n.render = pickedDevice(ID_RENDER, ren_, cfg_.render);
        n.capture = pickedDevice(ID_CAPTURE, cap_, cfg_.capture);
        n.loopback = pickedDevice(ID_LOOP, ren_, cfg_.loopback);
        n.preferredBuffer = (long)comboData(ID_BUF);
        n.lowLatency = checked(ID_LOWLAT);
        n.ytVolume = volume() / 100.0f;
        n.ytLatencyMs = (float)comboData(ID_YTLAT);
        n.monoAuto = checked(ID_MONO);
        n.autoDefault = checked(ID_AUTODEF);
        n.debugMeter = checked(ID_METER);
        return n;
    }
    void apply() {
        Config n = collect();
        bool reopen = n.render != cfg_.render || n.capture != cfg_.capture || n.loopback != cfg_.loopback ||
                      n.preferredBuffer != cfg_.preferredBuffer || n.lowLatency != cfg_.lowLatency || n.autoDefault != cfg_.autoDefault;
        writeConfig(n);
        if (drv_) drv_->applyLive(n);
        if (reopen) pendingReset_ = true;
        cfg_ = n;
        logf("control panel: settings saved%s", reopen ? " (driver reset requested)" : "");
    }
    void close(bool ok) {
        if (ok) apply();
        else if (drv_) drv_->applyLive(cfg_); // undo the live volume preview
        done_ = true;
    }

    static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM lp) {
        Panel* p = (Panel*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (m == WM_NCCREATE) { p = (Panel*)((CREATESTRUCTW*)lp)->lpCreateParams; SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)p); p->hwnd_ = h; }
        if (!p) return DefWindowProcW(h, m, w, lp);
        switch (m) {
        case WM_COMMAND:
            switch (LOWORD(w)) {
            case IDOK: p->close(true); return 0;
            case IDCANCEL: p->close(false); return 0;
            case ID_APPLY: p->apply(); p->refreshChecks(); return 0;
            case ID_SOUNDCPL: ShellExecuteW(h, L"open", L"control.exe", L"mmsys.cpl", nullptr, SW_SHOWNORMAL); return 0;
            case ID_OPENCFG: ensureConfig(); ShellExecuteW(h, L"open", L"notepad.exe", configPath().c_str(), nullptr, SW_SHOWNORMAL); return 0;
            case ID_OPENLOG: ShellExecuteW(h, L"open", L"notepad.exe", (appDir() + L"\\log.txt").c_str(), nullptr, SW_SHOWNORMAL); return 0;
            }
            break;
        case WM_HSCROLL:
            if ((HWND)lp == p->ctl(ID_VOL)) { p->updateVolText(); if (p->drv_) p->drv_->setLiveVolume(p->volume() / 100.0f); }
            return 0;
        case WM_DRAWITEM: {
            auto di = (const DRAWITEMSTRUCT*)lp;
            if (di->CtlID >= ID_HELP) p->drawHelp(di);
            else if (di->CtlID == ID_CHECKS) p->drawChecks(di);
            else p->drawMeter(di);
            return TRUE;
        }
        case WM_TIMER:
            p->updateStatus();
            if (++p->ticks_ % 40 == 0) p->refreshChecks(); // every 2 s
            return 0;
        case WM_CLOSE: p->close(false); return 0;
        }
        return DefWindowProcW(h, m, w, lp);
    }

    void run(HWND owner) {
        listEndpoints(eRender, ren_); listEndpoints(eCapture, cap_);
        cfg_ = orig_ = loadConfig();
        INITCOMMONCONTROLSEX icc{ sizeof icc, ICC_STANDARD_CLASSES | ICC_BAR_CLASSES };
        InitCommonControlsEx(&icc);
        HDC sdc = GetDC(nullptr); dpi_ = GetDeviceCaps(sdc, LOGPIXELSY); ReleaseDC(nullptr, sdc);
        NONCLIENTMETRICSW ncm{ sizeof ncm };
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
        font_ = CreateFontIndirectW(&ncm.lfMessageFont);
        ncm.lfMessageFont.lfWeight = FW_SEMIBOLD; bold_ = CreateFontIndirectW(&ncm.lfMessageFont);

        static ATOM cls = [] {
            WNDCLASSEXW wc{ sizeof wc };
            wc.lpfnWndProc = proc; wc.hInstance = g_hInst; wc.lpszClassName = L"UMC22AsioPanel";
            wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512) /* IDC_ARROW */); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
            wc.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512) /* IDI_APPLICATION */);
            return RegisterClassExW(&wc);
        }();
        if (!cls) return;
        DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_CLIPCHILDREN;
        RECT r{ 0, 0, S(548), S(clientHeight()) }; AdjustWindowRectEx(&r, style, FALSE, WS_EX_DLGMODALFRAME);
        int ww = r.right - r.left, wh = r.bottom - r.top;
        RECT area; if (!owner || !GetWindowRect(owner, &area)) SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
        int x = (area.left + area.right - ww) / 2, y = std::max<int>(area.top, (area.top + area.bottom - wh) / 2);
        std::wstring title = std::wstring(kDriverNameW) + L" - Settings";
        CreateWindowExW(WS_EX_DLGMODALFRAME | (owner ? 0 : WS_EX_APPWINDOW), L"UMC22AsioPanel", title.c_str(), style,
                        x, y, ww, wh, owner, nullptr, g_hInst, this);
        if (!hwnd_) return;
        build();
        SetTimer(hwnd_, 1, 50, nullptr);
        if (owner) EnableWindow(owner, FALSE);
        ShowWindow(hwnd_, SW_SHOW);
        SetFocus(ctl(IDOK));
        MSG msg; bool quit = false; WPARAM quitCode = 0;
        while (!done_) {
            BOOL got = GetMessageW(&msg, nullptr, 0, 0);
            if (got <= 0) { quit = got == 0; quitCode = msg.wParam; break; }
            if (!IsDialogMessageW(hwnd_, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        }
        if (owner) EnableWindow(owner, TRUE);
        KillTimer(hwnd_, 1);
        DestroyWindow(hwnd_); hwnd_ = nullptr;
        if (owner) SetForegroundWindow(owner);
        if (quit) PostQuitMessage((int)quitCode); // the host is closing: hand WM_QUIT back to its loop
        if (pendingReset_ && drv_ && drv_->canReset()) drv_->resetFromPanel();
    }
};

ASIOError Driver::controlPanel() {
    HWND owner = GetActiveWindow();
    if (!owner) {
        HWND fg = GetForegroundWindow(); DWORD pid = 0;
        if (fg && GetWindowThreadProcessId(fg, &pid) && pid == GetCurrentProcessId()) owner = fg;
    }
    Panel::show(owner, this);
    return ASE_OK;
}

// rundll32 umc22asio.dll,ControlPanel  -> settings window without an ASIO host
extern "C" void CALLBACK ControlPanelW(HWND, HINSTANCE, LPWSTR, int) {
    SetProcessDPIAware();
    Panel::show(nullptr, nullptr);
}

// ============================================================ COM
class Factory : public IClassFactory {
    std::atomic<ULONG> ref_{ 1 };
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IClassFactory) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = --ref_; if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        Driver* d = new (std::nothrow) Driver();
        if (!d) return E_OUTOFMEMORY;
        HRESULT hr = d->QueryInterface(riid, ppv);
        d->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override { lock ? g_locks++ : g_locks--; return S_OK; }
};

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { g_hInst = h; DisableThreadLibraryCalls(h); }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    if (clsid != CLSID_UMC22) return CLASS_E_CLASSNOTAVAILABLE;
    Factory* f = new (std::nothrow) Factory();
    if (!f) return E_OUTOFMEMORY;
    HRESULT hr = f->QueryInterface(riid, ppv);
    f->Release();
    return hr;
}

STDAPI DllCanUnloadNow() { return g_locks == 0 ? S_OK : S_FALSE; }

static LSTATUS setStr(HKEY root, const std::wstring& key, const wchar_t* name, const std::wstring& val) {
    return RegSetKeyValueW(root, key.c_str(), name, REG_SZ, val.c_str(), (DWORD)((val.size() + 1) * sizeof(wchar_t)));
}

STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH]; GetModuleFileNameW(g_hInst, path, MAX_PATH);
    std::wstring clsKey = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kClsidStr;
    std::wstring asioKey = std::wstring(L"SOFTWARE\\ASIO\\") + kDriverNameW;
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(L"SOFTWARE\\ASIO\\") + kLegacyNameW).c_str());
    LSTATUS s = setStr(HKEY_LOCAL_MACHINE, clsKey, nullptr, kDriverNameW);
    if (s == ERROR_SUCCESS) s = setStr(HKEY_LOCAL_MACHINE, clsKey + L"\\InprocServer32", nullptr, path);
    if (s == ERROR_SUCCESS) s = setStr(HKEY_LOCAL_MACHINE, clsKey + L"\\InprocServer32", L"ThreadingModel", L"Apartment");
    if (s == ERROR_SUCCESS) s = setStr(HKEY_LOCAL_MACHINE, asioKey, L"CLSID", kClsidStr);
    if (s == ERROR_SUCCESS) s = setStr(HKEY_LOCAL_MACHINE, asioKey, L"Description", kDriverNameW);
    if (s == ERROR_SUCCESS) ensureConfig();
    return s == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(s);
}

STDAPI DllUnregisterServer() {
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kClsidStr).c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(L"SOFTWARE\\ASIO\\") + kDriverNameW).c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(L"SOFTWARE\\ASIO\\") + kLegacyNameW).c_str());
    return S_OK;
}
