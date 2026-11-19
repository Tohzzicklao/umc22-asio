// Low Latency ASIO for UMC22 - Paths, log, config.ini and endpoint lookup
// Copyright (C) 2026 Tohzzicklao
// SPDX-License-Identifier: GPL-3.0-or-later
// Part of the single translation unit built from umc22asio.cpp: include it only from there.
#pragma once

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
    bool muteLoopback = true;
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
          "lowLatency=1\n"
          "; 1 = while the driver is open, mute the 'loopback' device (e.g. the laptop speakers):\n"
          ";     Windows audio is then heard only through the UMC22. Unmuted again on close.\n"
          "muteLoopback=1\n", f);
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
    c.muteLoopback = GetPrivateProfileIntW(L"driver", L"muteLoopback", 1, p.c_str()) != 0;
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
