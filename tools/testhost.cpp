// Host de prueba: carga umc22asio.dll sin registrar, abre 2in/2out y cuenta callbacks (salida en silencio).
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <cstdio>
#include <cmath>
#include <atomic>
#pragma comment(lib, "ole32.lib")

typedef long ASIOBool; typedef long ASIOError; typedef double ASIOSampleRate;
struct ASIOSamples { unsigned long hi, lo; };
struct ASIOClockSource { long a, b, c; ASIOBool d; char n[32]; };
struct ASIOChannelInfo { long channel; ASIOBool isInput, isActive; long group; long type; char name[32]; };
struct ASIOBufferInfo { ASIOBool isInput; long channelNum; void* buffers[2]; };
struct ASIOCallbacks { void (*bs)(long, ASIOBool); void (*src)(double); long (*msg)(long, long, void*, double*); void* (*bsti)(void*, long, ASIOBool); };
struct IASIO : IUnknown {
    virtual ASIOBool init(void*) = 0; virtual void getDriverName(char*) = 0; virtual long getDriverVersion() = 0;
    virtual void getErrorMessage(char*) = 0; virtual ASIOError start() = 0; virtual ASIOError stop() = 0;
    virtual ASIOError getChannels(long*, long*) = 0; virtual ASIOError getLatencies(long*, long*) = 0;
    virtual ASIOError getBufferSize(long*, long*, long*, long*) = 0; virtual ASIOError canSampleRate(double) = 0;
    virtual ASIOError getSampleRate(double*) = 0; virtual ASIOError setSampleRate(double) = 0;
    virtual ASIOError getClockSources(ASIOClockSource*, long*) = 0; virtual ASIOError setClockSource(long) = 0;
    virtual ASIOError getSamplePosition(ASIOSamples*, ASIOSamples*) = 0; virtual ASIOError getChannelInfo(ASIOChannelInfo*) = 0;
    virtual ASIOError createBuffers(ASIOBufferInfo*, long, long, ASIOCallbacks*) = 0; virtual ASIOError disposeBuffers() = 0;
    virtual ASIOError controlPanel() = 0; virtual ASIOError future(long, void*) = 0; virtual ASIOError outputReady() = 0;
};
static const CLSID CLSID_UMC22 = { 0x6b3e2a71, 0x9c4d, 0x4f1e, { 0xb8, 0xa2, 0x5d, 0x7c, 0x0e, 0x91, 0xf3, 0xa4 } };

static ASIOBufferInfo bi[4]; static long N = 144; static std::atomic<int> calls{0}; static double peak = 0;
static void bs(long idx, ASIOBool) {
    calls++;
    for (int c = 0; c < 2; c++) { int* in = (int*)bi[c].buffers[idx]; for (long i = 0; i < N; i++) { double v = fabs(in[i] / 2147483648.0); if (v > peak) peak = v; } }
    for (int c = 2; c < 4; c++) memset(bi[c].buffers[idx], 0, N * 4);
}
static void src(double) {}
static long msg(long sel, long, void*, double*) { printf("asioMessage %ld\n", sel); return sel == 1 ? 1 : 0; }

int main(int argc, char** argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HMODULE m = LoadLibraryW(L"umc22asio.dll");
    if (!m) { printf("LoadLibrary fallo %lu\n", GetLastError()); return 1; }
    auto gco = (HRESULT(__stdcall*)(REFCLSID, REFIID, void**))GetProcAddress(m, "DllGetClassObject");
    IClassFactory* f = nullptr; gco(CLSID_UMC22, IID_IClassFactory, (void**)&f);
    IASIO* a = nullptr; HRESULT hr = f->CreateInstance(nullptr, CLSID_UMC22, (void**)&a);
    printf("CreateInstance hr=0x%08lX\n", hr); if (!a) return 1;
    char buf[128]; a->getDriverName(buf); printf("driver: %s\n", buf);
    if (!a->init(nullptr)) { a->getErrorMessage(buf); printf("init fallo: %s\n", buf); return 1; }
    long in, out, mn, mx, pr, gr; a->getChannels(&in, &out); a->getBufferSize(&mn, &mx, &pr, &gr);
    printf("canales %ld/%ld  buffer min %ld max %ld pref %ld gran %ld\n", in, out, mn, mx, pr, gr);
    if (argc > 1) N = atol(argv[1]); else N = pr;
    printf("setSampleRate(48000)=%ld\n", a->setSampleRate(48000));
    for (int c = 0; c < 4; c++) { bi[c].isInput = c < 2; bi[c].channelNum = c % 2; }
    ASIOCallbacks cb{ bs, src, msg, nullptr };
    ASIOError e = a->createBuffers(bi, 4, N, &cb);
    if (e) { a->getErrorMessage(buf); printf("createBuffers=%ld: %s\n", e, buf); a->Release(); return 1; }
    long li, lo; a->getLatencies(&li, &lo); printf("latencias in %ld out %ld frames (%.2f / %.2f ms)\n", li, lo, li / 48.0, lo / 48.0);
    a->start(); Sleep(3000); a->stop();
    printf("callbacks en 3 s: %d (esperado ~%d), pico entrada %.4f\n", calls.load(), (int)(3 * 48000 / N), peak);
    a->disposeBuffers(); a->Release();
    return 0;
}
