// Low Latency ASIO for UMC22 - Moving the Windows default output (IPolicyConfig)
// Copyright (C) 2026 Tohzzicklao
// SPDX-License-Identifier: GPL-3.0-or-later
// Part of the single translation unit built from umc22asio.cpp: include it only from there.
#pragma once

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

// Mutes or unmutes an endpoint (the speaker icon of that device in Windows). Loopback capture taps the
// audio before the mute, so a muted loopback device still feeds the mix. 'was' gets the previous state.
static bool setEndpointMute(const std::wstring& id, bool mute, bool* was = nullptr) {
    bool ok = false;
    runMta([&] {
        IMMDeviceEnumerator* en = nullptr; IMMDevice* d = nullptr; IAudioEndpointVolume* v = nullptr;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) &&
            SUCCEEDED(en->GetDevice(id.c_str(), &d)) &&
            SUCCEEDED(d->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&v))) {
            BOOL m = FALSE; v->GetMute(&m);
            if (was) *was = m != FALSE;
            ok = SUCCEEDED(v->SetMute(mute ? TRUE : FALSE, nullptr));
        }
        if (v) v->Release(); if (d) d->Release(); if (en) en->Release();
    });
    return ok;
}
