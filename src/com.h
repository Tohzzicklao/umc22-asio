// Low Latency ASIO for UMC22 - COM class factory, DLL entry points and registration
// Copyright (C) 2026 Tohzzicklao
// SPDX-License-Identifier: GPL-3.0-or-later
// Part of the single translation unit built from umc22asio.cpp: include it only from there.
#pragma once

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
