// Low Latency ASIO for UMC22 - The ASIO driver: WASAPI engine, mixer, unplug recovery
// Copyright (C) 2026 Tohzzicklao
// SPDX-License-Identifier: GPL-3.0-or-later
// Part of the single translation unit built from umc22asio.cpp: include it only from there.
#pragma once

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
    long getDriverVersion() override { return kDriverVersion; }
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
