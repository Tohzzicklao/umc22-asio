// Copyright (C) 2026 Tohzzicklao
// SPDX-License-Identifier: GPL-3.0-or-later
// Plays a mono signal (220 Hz + 2500 Hz, identical on L and R) on the loopback endpoint and measures,
// via loopback capture, the level and phase of those frequencies on each channel
// (synchronous detection: unaffected by other audio playing at the same time).
// Set ID below to your endpoint ID (tools\bin\probe.exe lists the devices).
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <cstdio>
#include <cmath>
#include <thread>
#include <atomic>
#pragma comment(lib, "ole32.lib")
static const wchar_t* ID = L"{0.0.0.00000000}.{c087ccd6-f990-4c31-94f6-1bcc30aadf8c}";
static const double PI = 3.14159265358979;
static const double FR[2] = { 220.0, 2500.0 };
static std::atomic<bool> run{ true };

static IAudioClient* open(bool loop, WAVEFORMATEX** m) {
    IMMDeviceEnumerator* e; CoCreateInstance(__uuidof(MMDeviceEnumerator), 0, CLSCTX_ALL, IID_PPV_ARGS(&e));
    IMMDevice* d; e->GetDevice(ID, &d);
    IAudioClient* a; d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, 0, (void**)&a);
    a->GetMixFormat(m);
    a->Initialize(AUDCLNT_SHAREMODE_SHARED, loop ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0, loop ? 2000000 : 1000000, 0, *m, 0);
    return a;
}

static void play() {
    CoInitializeEx(0, COINIT_MULTITHREADED);
    WAVEFORMATEX* m; IAudioClient* a = open(false, &m);
    IAudioRenderClient* r; a->GetService(IID_PPV_ARGS(&r));
    UINT32 bs; a->GetBufferSize(&bs); a->Start();
    double p1 = 0, p2 = 0;
    while (run) {
        Sleep(10);
        UINT32 pad; a->GetCurrentPadding(&pad); UINT32 n = bs - pad; BYTE* b;
        if (FAILED(r->GetBuffer(n, &b))) continue;
        float* f = (float*)b;
        for (UINT32 i = 0; i < n; i++) {
            float v = 0.03f * (float)sin(p1) + 0.02f * (float)sin(p2);
            p1 += 2 * PI * FR[0] / m->nSamplesPerSec; p2 += 2 * PI * FR[1] / m->nSamplesPerSec;
            for (int c = 0; c < m->nChannels; c++) f[i * m->nChannels + c] = v;
        }
        r->ReleaseBuffer(n, 0);
    }
    a->Stop();
}

int main() {
    std::thread t(play); Sleep(300);
    CoInitializeEx(0, COINIT_MULTITHREADED);
    WAVEFORMATEX* m; IAudioClient* a = open(true, &m);
    IAudioCaptureClient* c; a->GetService(IID_PPV_ARGS(&c)); a->Start();
    double re[2][2] = {}, im[2][2] = {}; long n = 0; DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < 2500) {
        Sleep(10); UINT32 pk;
        while (SUCCEEDED(c->GetNextPacketSize(&pk)) && pk) {
            BYTE* b; UINT32 fr; DWORD fl; c->GetBuffer(&b, &fr, &fl, 0, 0); float* f = (float*)b;
            for (UINT32 i = 0; i < fr; i++, n++) {
                double s[2] = { f[i * m->nChannels], f[i * m->nChannels + 1] };
                for (int k = 0; k < 2; k++) {
                    double w = 2 * PI * FR[k] * n / m->nSamplesPerSec;
                    for (int ch = 0; ch < 2; ch++) { re[k][ch] += s[ch] * cos(w); im[k][ch] += s[ch] * sin(w); }
                }
            }
            c->ReleaseBuffer(fr);
        }
    }
    run = false; t.join();
    for (int k = 0; k < 2; k++) {
        double mL = hypot(re[k][0], im[k][0]), mR = hypot(re[k][1], im[k][1]);
        double dp = (atan2(im[k][1], re[k][1]) - atan2(im[k][0], re[k][0])) * 180 / PI;
        while (dp > 180) dp -= 360; while (dp < -180) dp += 360;
        printf("%5.0f Hz: level L %.1f dB, R %.1f dB, R/L difference %.2f dB, R-L phase %.1f degrees\n",
               FR[k], 20 * log10(2 * mL / n), 20 * log10(2 * mR / n), 20 * log10(mR / mL), dp);
    }
}
