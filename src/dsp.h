// Low Latency ASIO for UMC22 - FIFOs, Windows audio resampler, limiter and dither
// Copyright (C) 2026 Tohzzicklao
// SPDX-License-Identifier: GPL-3.0-or-later
// Part of the single translation unit built from umc22asio.cpp: include it only from there.
#pragma once

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
