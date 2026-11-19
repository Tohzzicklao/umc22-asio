// Low Latency ASIO for UMC22 - Device sample formats and exclusive-mode WASAPI streams
// Copyright (C) 2026 Tohzzicklao
// SPDX-License-Identifier: GPL-3.0-or-later
// Part of the single translation unit built from umc22asio.cpp: include it only from there.
#pragma once

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
