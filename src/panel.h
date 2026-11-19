// Low Latency ASIO for UMC22 - Settings window (ASIO control panel and rundll32 entry)
// Copyright (C) 2026 Tohzzicklao
// SPDX-License-Identifier: GPL-3.0-or-later
// Part of the single translation unit built from umc22asio.cpp: include it only from there.
#pragma once

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
        ID_METER, ID_STATUS, ID_M_IN, ID_M_OUT, ID_M_YT, ID_OPENLOG, ID_OPENCFG, ID_APPLY, ID_CHECKS, ID_SOUNDCPL, ID_REPORT,
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

    // ---------------------------------------------------- "Report a problem"
    static std::wstring windowsVersion() {
        typedef LONG(WINAPI* RtlGetVersionFn)(OSVERSIONINFOW*);
        OSVERSIONINFOW v{ sizeof v };
        auto fn = (RtlGetVersionFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
        if (!fn || fn(&v) != 0) return L"unknown";
        wchar_t t[64]; swprintf_s(t, L"%lu.%lu build %lu", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
        return t;
    }
    // the report goes to a public issue: replace the user profile path, which contains the user name
    static std::wstring anonymize(std::wstring s) {
        wchar_t prof[MAX_PATH] = L"";
        DWORD n = GetEnvironmentVariableW(L"USERPROFILE", prof, MAX_PATH);
        if (!n || n >= MAX_PATH) return s;
        std::wstring lp = lower(prof), ls = lower(s);
        for (size_t i = ls.find(lp); i != std::wstring::npos; i = ls.find(lp)) {
            s.replace(i, lp.size(), L"%USERPROFILE%");
            ls.replace(i, lp.size(), L"%userprofile%");
        }
        return s;
    }
    static std::string urlEncode(const std::wstring& w) {
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        std::string u(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &u[0], n, nullptr, nullptr);
        std::string o; char h[4];
        for (unsigned char c : u) {
            bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
            if (plain) o += (char)c; else { sprintf_s(h, "%%%02X", c); o += h; }
        }
        return o;
    }
    // Opens a new GitHub issue prefilled with the driver, Windows, settings and system check details,
    // and the log folder with log.txt selected so it can be dragged into the issue.
    void report() {
        logf("control panel: problem report opened");
        refreshChecks();
        wchar_t exe[MAX_PATH] = L""; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const wchar_t* host = wcsrchr(exe, L'\\') ? wcsrchr(exe, L'\\') + 1 : exe;
        std::wstring b, status = lastStatus_;
        std::replace(status.begin(), status.end(), L'\n', L' ');
        b += L"### What happened\n<!-- What did you do, what did you expect, and what happened instead? Does it happen every time? -->\n\n\n";
        b += L"### Log\n<!-- Please drag log.txt here. Its folder (%APPDATA%\\UMC22ASIO) has been opened in File Explorer, behind this window. -->\n\n\n";
        b += L"### System (filled in by the driver)\n";
        wchar_t t[512];
        swprintf_s(t, L"- Driver: %ld.%ld.%ld\n- Windows: %s\n- Host: %s\n",
                   (kDriverVersion >> 16) & 0xff, (kDriverVersion >> 8) & 0xff, kDriverVersion & 0xff,
                   windowsVersion().c_str(), drv_ ? host : L"(settings opened outside a host)");
        b += t;
        swprintf_s(t, L"- Settings: buffer %ld, lowLatency %d, ytLatencyMs %.0f, ytVolume %.2f, monoAuto %d, autoDefault %d\n",
                   cfg_.preferredBuffer, (int)cfg_.lowLatency, cfg_.ytLatencyMs, cfg_.ytVolume, (int)cfg_.monoAuto, (int)cfg_.autoDefault);
        b += t;
        b += L"- Devices: render `" + cfg_.render + L"`, capture `" + cfg_.capture + L"`, loopback `" +
             (cfg_.loopback.empty() ? std::wstring(L"(disabled)") : cfg_.loopback) + L"`\n";
        b += L"- Status: " + status + L"\n\n### System check\n";
        static const wchar_t* lv[] = { L"OK", L"Warning", L"Problem" };
        for (auto& c : checks_) b += std::wstring(L"- ") + lv[c.level] + L": " + c.text + L"\n";
        // log folder first, then the browser on top of it: the issue is what the user reads first
        std::wstring args = L"/select,\"" + appDir() + L"\\log.txt\"";
        ShellExecuteW(hwnd_, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        Sleep(1200); // Explorer opens its window asynchronously; give it time so it doesn't land on top
        std::string url = std::string(kRepoUrl) + "/issues/new?body=" + urlEncode(anonymize(b));
        ShellExecuteA(hwnd_, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
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
        label(L"UMC22 input", L, 69, 160);        combo(ID_CAPTURE, X, 65, W);
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

        label(L"System check", L, 366, 200, true);
        add(WC_BUTTONW, L"Sound settings", BS_PUSHBUTTON | WS_TABSTOP, X + 214, 361, 110, 26, ID_SOUNDCPL);
        help(H, 365, L"What the driver needs to work, checked every 2 seconds.\n\n"
                     L"Sound settings opens the classic Windows sound panel, where exclusive mode is allowed: "
                     L"select the UMC22 > Properties > Advanced.");
        add(L"STATIC", L"", SS_OWNERDRAW, L, 392, 498, 5 * 22, ID_CHECKS);
        checks_ = evaluate();

        const int Y = 516; // status
        label(L"Status", L, Y, 200, true);
        add(WC_BUTTONW, L"Report a problem...", BS_PUSHBUTTON | WS_TABSTOP, X + 194, Y - 5, 130, 26, ID_REPORT);
        add(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, L, Y + 24, 488, 36, ID_STATUS);
        if (drv_) { // level meters only make sense while a host has the driver open
            label(L"Input", L, Y + 68, 70);   add(L"STATIC", L"", SS_OWNERDRAW, 90, Y + 69, 414, 16, ID_M_IN);
            label(L"Output", L, Y + 94, 70);  add(L"STATIC", L"", SS_OWNERDRAW, 90, Y + 95, 414, 16, ID_M_OUT);
            label(L"Windows", L, Y + 120, 70); add(L"STATIC", L"", SS_OWNERDRAW, 90, Y + 124, 414, 7, ID_M_YT);
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
            case ID_REPORT: p->report(); return 0;
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
