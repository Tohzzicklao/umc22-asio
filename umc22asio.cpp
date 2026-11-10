// UMC22 Low Latency - ASIO driver (in-process COM DLL) for the Behringer UMC22 (USB Audio CODEC / PCM2902)
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
static const wchar_t* kDriverNameW = L"UMC22 Low Latency";
static const char* kDriverName = "UMC22 Low Latency";

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
};

static void ensureConfig() {
    std::wstring p = configPath();
    if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    FILE* f = _wfopen(p.c_str(), L"w");
    if (!f) return;
    fputs("; UMC22 Low Latency - configuration\n"
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
          "debugMeter=0\n", f);
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
    for (int chs : {2, 1}) {
        for (auto t : tries) {
            makeFmt(w, chs, sr, t.bits, t.valid);
            if (ac->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &w.Format, nullptr) == S_OK) { found = true; break; }
        }
        if (found) break;
    }
    if (!found) { ac->Release(); return AUDCLNT_E_UNSUPPORTED_FORMAT; }
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
        logf("init: render='%ls' capture='%ls' loopback='%ls'", cfg_.render.c_str(), cfg_.capture.c_str(), cfg_.loopback.c_str());
        HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        IMMDeviceEnumerator* en = nullptr;
        bool ok = SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)));
        if (ok) {
            std::wstring n1, n2, n3;
            renderId_ = findEndpoint(en, eRender, cfg_.render, &n1);
            captureId_ = findEndpoint(en, eCapture, cfg_.capture, &n2);
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
    long getDriverVersion() override { return 1; }
    void getErrorMessage(char* s) override { strcpy_s(s, 124, err_); }

    ASIOError start() override {
        if (!engineReady_) return ASE_InvalidMode;
        if (running_) return ASE_OK;
        samplePos_ = 0;
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
        long n = bufSize_ ? bufSize_ : cfg_.preferredBuffer;
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
        // If Windows uses the UMC22 as default output, move it to the loopback device to free the UMC22
        if (cfg_.autoDefault && !loopId_.empty() && !switchedDefault_) {
            std::wstring cur = getDefaultRender();
            if (lower(cur) == lower(renderId_) && setDefaultRender(loopId_)) {
                switchedDefault_ = true;
                logf("Windows default output -> loopback device (restored on close)");
                Sleep(150); // give apps time to migrate their streams
            }
        }
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
        logf("createBuffers ok: asio=%ld frames, device period=%u frames, %.0f Hz", size, devFrames_, sampleRate_);
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
    ASIOError controlPanel() override {
        ensureConfig();
        ShellExecuteW(nullptr, L"open", L"notepad.exe", configPath().c_str(), nullptr, SW_SHOWNORMAL);
        return ASE_OK;
    }
    ASIOError future(long, void*) override { return ASE_InvalidParameter; }
    ASIOError outputReady() override { return ASE_NotPresent; }

private:
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
    void engineThread(HANDLE opened) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IMMDeviceEnumerator* en = nullptr; IMMDevice* rDev = nullptr; IMMDevice* cDev = nullptr;
        IAudioClient* rCl = nullptr; IAudioClient* cCl = nullptr;
        IAudioRenderClient* rr = nullptr; IAudioCaptureClient* cc = nullptr;
        HANDLE rEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE cEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        DevFmt rf, cf; UINT32 rFrames = 0, cFrames = 0;
        int sr = (int)sampleRate_;

        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
        if (SUCCEEDED(hr)) hr = en->GetDevice(renderId_.c_str(), &rDev);
        if (SUCCEEDED(hr)) hr = en->GetDevice(captureId_.c_str(), &cDev);
        // retry briefly in case an app is still releasing the UMC22
        for (int tries = 0; SUCCEEDED(hr) || hr == AUDCLNT_E_DEVICE_IN_USE; tries++) {
            if (tries) Sleep(100);
            hr = openExclusive(rDev, sr, bufSize_, rEv, &rCl, rf, rFrames);
            if (hr != AUDCLNT_E_DEVICE_IN_USE || tries >= 10) break;
        }
        if (FAILED(hr)) logf("render open hr=0x%08lX", (unsigned long)hr);
        if (SUCCEEDED(hr)) { hr = openExclusive(cDev, sr, (int)rFrames, cEv, &cCl, cf, cFrames); if (FAILED(hr)) logf("capture open hr=0x%08lX", (unsigned long)hr); }
        if (SUCCEEDED(hr)) hr = rCl->GetService(IID_PPV_ARGS(&rr));
        if (SUCCEEDED(hr)) hr = cCl->GetService(IID_PPV_ARGS(&cc));
        if (SUCCEEDED(hr)) logf("devices: render %d ch %d-bit %u frames | capture %d ch %d-bit %u frames",
                                rf.ch, rf.bytes * 8, rFrames, cf.ch, cf.bytes * 8, cFrames);
        devFrames_ = rFrames;
        openResult_ = hr;
        SetEvent(opened);

        if (SUCCEEDED(hr)) {
            for (;;) {
                WaitForSingleObject(startEv_, INFINITE);
                if (quit_) break;
                if (running_) runStream(rCl, cCl, rr, cc, rEv, cEv, rf, cf, rFrames);
                SetEvent(stoppedEv_);
            }
        }
        if (rr) rr->Release(); if (cc) cc->Release();
        if (rCl) rCl->Release(); if (cCl) cCl->Release();
        if (rDev) rDev->Release(); if (cDev) cDev->Release();
        if (en) en->Release();
        CloseHandle(rEv); CloseHandle(cEv);
        CoUninitialize();
    }

    void processBlock() {
        const long N = bufSize_; const int idx = bufIdx_;
        for (long i = 0; i < N; i++) {
            float l = 0, r = 0; inF_.pop(l, r);
            if (inBuf_[0][idx]) inBuf_[0][idx][i] = f2i(l);
            if (inBuf_[1][idx]) inBuf_[1][idx][i] = f2i(r);
        }
        LARGE_INTEGER q, f; QueryPerformanceCounter(&q); QueryPerformanceFrequency(&f);
        stampNs_ = (uint64_t)((double)q.QuadPart * 1e9 / (double)f.QuadPart);
        cb_->bufferSwitch(idx, 1);
        samplePos_ += N;
        // If the host only sends the guitar on L (R missing or silent), copy it to R
        bool copyL = !outBuf_[1][idx] && outBuf_[0][idx];
        if (cfg_.monoAuto && !copyL && outBuf_[0][idx] && outBuf_[1][idx]) {
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

    void runStream(IAudioClient* rCl, IAudioClient* cCl, IAudioRenderClient* rr, IAudioCaptureClient* cc,
                   HANDLE rEv, HANDLE cEv, const DevFmt& rf, const DevFmt& cf, UINT32 B) {
        DWORD task = 0; HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
        if (mm) AvSetMmThreadPriority(mm, AVRT_PRIORITY_CRITICAL);
        const long N = bufSize_;
        inF_.init(65536); outF_.init(65536); bufIdx_ = 0;
        for (long i = 0; i < N; i++) outF_.push(0, 0);
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
        HANDLE evs[2] = { cEv, rEv };

        while (running_) {
            DWORD w = WaitForMultipleObjects(2, evs, FALSE, 500);
            if (w == WAIT_TIMEOUT) { logf("timeout waiting for the UMC22"); continue; }
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
                cc->ReleaseBuffer(fr);
            }
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED) { requestReset("capture device invalidated"); break; }
            if (!renSig) continue;
            rEvents++;

            // 2) process ASIO blocks
            while (inF_.n >= (size_t)N && running_) processBlock();

            // 3) output + Windows audio mix
            hr = rr->GetBuffer(B, &p);
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED) { requestReset("render device invalidated"); break; }
            if (FAILED(hr)) continue;
            bool ytOn = ytActive_.load(std::memory_order_acquire);
            if (ytOn) yt.block(ytRing_, ytSrcRate_, dstRate, cfg_.ytLatencyMs);
            const float vol = cfg_.ytVolume;
            if (outF_.n < B) underruns++;
            const bool dith = !rf.flt && rf.bytes == 2;
            for (UINT32 i = 0; i < B; i++) {
                float l = 0, r = 0; outF_.pop(l, r);
                pkGtr = std::max(pkGtr, std::max(std::fabs(l), std::fabs(r)));
                mt.gl += l * l; mt.gr += r * r; mt.glr += l * r; mt.pg = std::max(mt.pg, std::max(std::fabs(l), std::fabs(r)));
                if (ytOn) {
                    float yl, yr; yt.next(ytRing_, yl, yr); yl *= vol; yr *= vol;
                    pkYt = std::max(pkYt, std::max(std::fabs(yl), std::fabs(yr)));
                    mt.yl += yl * yl; mt.yr += yr * yr; mt.ylr += yl * yr; mt.py = std::max(mt.py, std::max(std::fabs(yl), std::fabs(yr)));
                    l += yl; r += yr;
                }
                lim.process(l, r);
                if (lim.g < 0.98f) { limFrames++; mt.lim++; }
                mt.ol += l * l; mt.orr += r * r; mt.olr += l * r; mt.po = std::max(mt.po, std::max(std::fabs(l), std::fabs(r))); mt.n++;
                if (dith) { l += dth.tpdf(1.0f / 32768.0f); r += dth.tpdf(1.0f / 32768.0f); }
                BYTE* o = p + (size_t)i * rf.block;
                wrS(o, rf, l);
                if (rf.ch > 1) wrS(o + rf.bytes, rf, r);
                for (int c = 2; c < rf.ch; c++) wrS(o + c * rf.bytes, rf, 0);
            }
            rr->ReleaseBuffer(B, 0);
            if (cfg_.debugMeter && GetTickCount64() - lastMeter >= 5000 && mt.n) {
                double n = (double)mt.n;
                logf("meter: guitar RMS L %.1f R %.1f peak %.1f corr %.2f | loopback RMS L %.1f R %.1f peak %.1f corr %.2f | output RMS L %.1f R %.1f peak %.1f corr %.2f | limiter %.1f%% | mono %s",
                     dbf(std::sqrt(mt.gl / n)), dbf(std::sqrt(mt.gr / n)), dbf(mt.pg), corr(mt.gl, mt.gr, mt.glr),
                     dbf(std::sqrt(mt.yl / n)), dbf(std::sqrt(mt.yr / n)), dbf(mt.py), corr(mt.yl, mt.yr, mt.ylr),
                     dbf(std::sqrt(mt.ol / n)), dbf(std::sqrt(mt.orr / n)), dbf(mt.po), corr(mt.ol, mt.orr, mt.olr),
                     100.0 * mt.lim / n, monoCopying_ ? "on" : "off");
                mt = Meter{}; lastMeter = GetTickCount64();
            }
            // cap latency if it builds up
            if (outF_.n > (size_t)(N + 2 * B)) outF_.drop(outF_.n - (N + B));
        }
        rCl->Stop(); cCl->Stop(); rCl->Reset(); cCl->Reset();
        double secs = (GetTickCount64() - t0) / 1000.0;
        logf("stats: %.1f s, render events %.0f/s, capture %.0f frames/s, duplicates %llu, underruns %u", secs, rEvents / secs, capFrames / secs, (unsigned long long)dupFrames, underruns);
        auto db = [](float x) { return x > 0 ? 20.0 * std::log10(x) : -99.0; };
        logf("levels: guitar peak %.1f dBFS, loopback peak %.1f dBFS, limiter active %.1f%% of the time",
             db(pkGtr), db(pkYt), 100.0 * limFrames / std::max(1.0, secs * dstRate));
        if (mm) AvRevertMmThreadCharacteristics(mm);
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
    Fifo inF_, outF_;

    std::thread ytThread_;
    std::atomic<bool> ytRun_{ false }, ytActive_{ false };
    Spsc ytRing_;
    double ytSrcRate_ = 48000.0;
};

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
    return S_OK;
}
