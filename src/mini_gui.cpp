// mini_gui.cpp — Brave Origin Mini (472x272, Scan + Patch only).
// Reuses the hardened in-process engine (engine.h): EngineScanChannel for
// read-only scans and EngineApplyChannel for repairs (same markers, same
// fail-closed rules, same atomic writes/timestamped backups). No network,
// no new deps beyond COMCTL32/GDI32/USER32/KERNEL32/msvcrt (+ole32/shell32
// already required by the engine). GUI subsystem (-mwindows): no console.
#include "engine.h"
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <string>
#include <atomic>

#ifndef IDI_MAIN
#define IDI_MAIN 101
#endif

static const wchar_t* kChannels[] = { L"Brave-Origin", L"Brave-Origin-Beta", L"Brave-Origin-Nightly" };
static const UINT WM_STEP = WM_APP + 1;   // wParam = progress 0..100, lParam = step index
static const UINT WM_DONE = WM_APP + 2;   // wParam = engine rc, lParam = Work*
static const wchar_t* kSteps[] = { L"Closing Brave", L"Backing up", L"Writing repair", L"Verifying" };

enum class MiniState { Fresh, Ready, NeedsRepair, NotInstalled, Patching, Done };
static HWND g_hwnd = nullptr, g_btn = nullptr, g_prog = nullptr, g_tip = nullptr;
static HFONT g_word = nullptr, g_head = nullptr, g_font = nullptr, g_small = nullptr, g_bold = nullptr;
static HBRUSH g_bg = nullptr;
static std::atomic<bool> g_busy(false);
static MiniState g_state = MiniState::Fresh;
static EngineChannelSnapshot g_snap[3];
static int g_target = -1;                 // channel index for Patch/Open
static std::wstring g_backup;             // last timestamped backup (full path)
static std::wstring g_step;               // live step text while patching
static int g_stepIdx = 0, g_dots = 0, g_pos = 0;
static bool g_scanned = false;

struct Work { int channel; int rc; std::wstring backup; };

static std::wstring statusText() {
    switch (g_state) {
        case MiniState::Fresh:       return L"Check your local purchase state";
        case MiniState::Ready:        return L"Ready \u2014 no repair needed";
        case MiniState::NeedsRepair:  return L"Needs repair";
        case MiniState::NotInstalled: return L"Not installed";
        case MiniState::Patching:     return g_step;
        case MiniState::Done:         return L"Ready \u2014 no repair needed";
    }
    return L"";
}
static COLORREF statusColor() {
    switch (g_state) {
        case MiniState::Ready:
        case MiniState::Done:        return RGB(46, 184, 119);
        case MiniState::NeedsRepair: return RGB(239, 132, 49);
        default:                     return RGB(137, 146, 160);
    }
}
static const wchar_t* buttonText() {
    if (g_busy.load()) return g_state == MiniState::Patching ? L"Working\u2026" : L"Scanning\u2026";
    switch (g_state) {
        case MiniState::NeedsRepair: return L"Patch now";
        case MiniState::Ready:
        case MiniState::Done:        return L"Open Brave Origin";
        default:                     return L"Scan";
    }
}
static bool buttonEnabled() {
    if (g_busy.load()) return false;
    if (g_state == MiniState::NotInstalled) return true;  // rescan
    if (g_state == MiniState::Fresh) return true;
    return g_state == MiniState::NeedsRepair || g_state == MiniState::Ready || g_state == MiniState::Done;
}

static void refreshChrome() {
    SetWindowTextW(g_btn, buttonText());
    EnableWindow(g_btn, buttonEnabled());
    ShowWindow(g_prog, g_state == MiniState::Patching ? SW_SHOW : SW_HIDE);
    if (g_state == MiniState::Patching) SetTimer(g_hwnd, 1, 350, nullptr);
    else KillTimer(g_hwnd, 1);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void doScan() {
    bool eb = false;
    if (!g_busy.compare_exchange_strong(eb, true)) return;
    refreshChrome();
    for (int i = 0; i < 3; i++) g_snap[i] = EngineScanChannel(kChannels[i]);
    g_scanned = true;
    g_backup.clear();
    g_target = -1;
    for (int i = 0; i < 3; i++)
        if (g_snap[i].status == EngineStatus::NeedsRepair) { g_target = i; break; }
    if (g_target < 0)
        for (int i = 0; i < 3; i++)
            if (g_snap[i].status == EngineStatus::Ready) { g_target = i; break; }
    if (g_target >= 0)
        g_state = (g_snap[g_target].status == EngineStatus::NeedsRepair) ? MiniState::NeedsRepair : MiniState::Ready;
    else
        g_state = MiniState::NotInstalled;
    g_busy = false;
    refreshChrome();
}

static DWORD WINAPI patchWorker(void* p) {
    Work* w = (Work*)p;
    HWND h = g_hwnd;
    PostMessageW(h, WM_STEP, 12, 0);   // Closing Brave
    PostMessageW(h, WM_STEP, 32, 1);   // Backing up
    w->rc = EngineApplyChannel(kChannels[w->channel], &w->backup);
    PostMessageW(h, WM_STEP, 62, 2);   // Writing repair
    PostMessageW(h, WM_STEP, 86, 3);   // Verifying
    PostMessageW(h, WM_DONE, (WPARAM)w->rc, (LPARAM)w);
    return 0;
}

static void startPatch() {
    if (g_target < 0) return;
    // Re-verify read-only immediately before confirming: fail closed.
    g_snap[g_target] = EngineScanChannel(kChannels[g_target]);
    if (g_snap[g_target].status != EngineStatus::NeedsRepair) {
        g_state = (g_snap[g_target].status == EngineStatus::Ready) ? MiniState::Ready : MiniState::NotInstalled;
        refreshChrome();
        MessageBoxW(g_hwnd, L"This channel no longer needs repair. Nothing was changed.",
                    L"Brave Origin", MB_ICONINFORMATION);
        return;
    }
    if (MessageBoxW(g_hwnd,
            L"Patch will close Brave Origin, save a backup, then repair. Cancel changes nothing.",
            L"Patch Brave Origin local settings?", MB_OKCANCEL | MB_ICONWARNING) != IDOK)
        return;
    bool eb = false;
    if (!g_busy.compare_exchange_strong(eb, true)) return;
    g_state = MiniState::Patching;
    g_stepIdx = 0; g_dots = 0; g_pos = 5; g_step = kSteps[0];
    SendMessageW(g_prog, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendMessageW(g_prog, PBM_SETPOS, 5, 0);
    refreshChrome();
    Work* w = new Work{ g_target, 2, L"" };
    HANDLE h = CreateThread(nullptr, 0, patchWorker, w, 0, nullptr);
    if (!h) {
        delete w;
        g_busy = false;
        g_state = MiniState::NeedsRepair;
        refreshChrome();
        MessageBoxW(g_hwnd, L"Unable to start the repair. No changes occurred.",
                    L"Brave Origin", MB_ICONERROR);
        return;
    }
    CloseHandle(h);
}

static void openTarget() {
    if (g_target < 0) return;
    ShellExecuteW(g_hwnd, L"open", g_snap[g_target].profilePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

static void setTip(const std::wstring& s) {
    TOOLINFOW t = { sizeof(t) };
    t.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    t.hwnd = g_hwnd;
    t.uId = (UINT_PTR)g_btn;
    t.lpszText = (LPWSTR)s.c_str();
    SendMessageW(g_tip, s.empty() ? TTM_DELTOOLW : TTM_UPDATETIPTEXTW, 0, (LPARAM)&t);
    if (!s.empty()) {
        // Ensure the tool exists (update alone does not add it).
        SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&t);
        SendMessageW(g_tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&t);
    }
}

// Approximate, clearly-unofficial geometric lion head: angular mane outline,
// ears, eyes, nose and muzzle strokes in white.
static void drawLion(HDC d, int cx, int cy, int r) {
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
    HGDIOBJ op = SelectObject(d, pen);
    SelectObject(d, GetStockObject(HOLLOW_BRUSH));
    const double jag[16][2] = { {0,-1.15},{0.38,-1.02},{0.5,-0.72},{0.86,-0.66},{0.82,-0.3},
        {1.1,-0.12},{0.88,0.18},{1.0,0.5},{0.62,0.6},{0.5,0.95},{0.14,0.86},{0,1.12},
        {-0.14,0.86},{-0.5,0.95},{-0.62,0.6},{-1.0,0.5} };
    POINT mane[17];
    for (int i = 0; i < 16; i++) {
        // Mirror the right-half table to the left for symmetry.
        double x = (i < 8) ? jag[i][0] : -jag[15 - i][0];
        double y = (i < 8) ? jag[i][1] : jag[15 - i][1];
        mane[i].x = cx + (int)(x * r);
        mane[i].y = cy + (int)(y * r);
    }
    mane[16] = mane[0];
    Polyline(d, mane, 17);
    int fr = (int)(r * 0.62);
    POINT face[9] = { {cx - fr, cy - fr / 2}, {cx - fr / 2, cy - fr}, {cx + fr / 2, cy - fr},
        {cx + fr, cy - fr / 2}, {cx + fr, cy + fr / 2}, {cx + fr / 2, cy + fr},
        {cx - fr / 2, cy + fr}, {cx - fr, cy + fr / 2}, {cx - fr, cy - fr / 2} };
    Polyline(d, face, 9);
    MoveToEx(d, cx - (int)(r * 0.34), cy - (int)(r * 0.12), nullptr);
    LineTo(d, cx - (int)(r * 0.14), cy - (int)(r * 0.12));
    MoveToEx(d, cx + (int)(r * 0.14), cy - (int)(r * 0.12), nullptr);
    LineTo(d, cx + (int)(r * 0.34), cy - (int)(r * 0.12));
    MoveToEx(d, cx - (int)(r * 0.12), cy + (int)(r * 0.16), nullptr);
    LineTo(d, cx + (int)(r * 0.12), cy + (int)(r * 0.16));
    MoveToEx(d, cx, cy + (int)(r * 0.16), nullptr);
    LineTo(d, cx, cy + (int)(r * 0.34));
    MoveToEx(d, cx - (int)(r * 0.2), cy + (int)(r * 0.42), nullptr);
    LineTo(d, cx, cy + (int)(r * 0.34));
    LineTo(d, cx + (int)(r * 0.2), cy + (int)(r * 0.42));
    SelectObject(d, op);
    DeleteObject(pen);
}

static void centerText(HDC d, int y, int w, const std::wstring& s, HFONT f, COLORREF c) {
    SelectObject(d, f);
    SetTextColor(d, c);
    RECT r = { 0, y, w, y + 60 };
    DrawTextW(d, s.c_str(), (int)s.size(), &r, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
}

static void paint(HWND h) {
    PAINTSTRUCT p;
    HDC d = BeginPaint(h, &p);
    RECT r;
    GetClientRect(h, &r);
    int W = r.right, H = r.bottom;
    // Near-black vertical gradient background.
    for (int y = 0; y < H; y++) {
        int t = (y * 10) / H;  // 0..10
        COLORREF c = RGB(16 + t, 20 + t, 28 + t);
        HBRUSH b = CreateSolidBrush(c);
        RECT row = { 0, y, W, y + 1 };
        FillRect(d, &row, b);
        DeleteObject(b);
    }
    SetBkMode(d, TRANSPARENT);
    // Centered rounded card.
    HBRUSH cb = CreateSolidBrush(RGB(28, 34, 46));
    HPEN cp = CreatePen(PS_SOLID, 1, RGB(61, 72, 91));
    HGDIOBJ ob = SelectObject(d, cb), opc = SelectObject(d, cp);
    RoundRect(d, 18, 12, W - 18, H - 12, 18, 18);
    SelectObject(d, ob);
    SelectObject(d, opc);
    DeleteObject(cb);
    DeleteObject(cp);
    drawLion(d, W / 2, 50, 24);
    centerText(d, 78, W, L"brave", g_word, RGB(255, 255, 255));
    centerText(d, 100, W, L"Brave Origin", g_head, RGB(242, 245, 249));
    std::wstring st = statusText();
    if (g_state == MiniState::Patching) {
        st = g_step;
        for (int i = 0; i < g_dots; i++) st += L".";
    }
    centerText(d, 132, W, st, g_font, statusColor());
    if (!g_backup.empty() && (g_state == MiniState::Done)) {
        std::wstring b = L"Backup: ";
        size_t x = g_backup.find_last_of(L"\\/");
        b += (x == std::wstring::npos) ? g_backup : g_backup.substr(x + 1);
        centerText(d, 228, W, b, g_small, RGB(159, 173, 191));
    } else if (g_state == MiniState::NeedsRepair && !g_scanned) {
        (void)0;
    } else if (g_state == MiniState::NeedsRepair) {
        centerText(d, 228, W, std::wstring(kChannels[g_target]), g_small, RGB(159, 173, 191));
    }
    EndPaint(h, &p);
}

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_CREATE: {
            g_hwnd = h;
            g_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            g_small = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            g_word = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            g_head = CreateFontW(-22, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            g_bold = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            g_btn = CreateWindowW(L"BUTTON", L"Scan", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                  136, 162, 200, 38, h, (HMENU)100, 0, 0);
            SendMessageW(g_btn, WM_SETFONT, (WPARAM)g_bold, TRUE);
            g_prog = CreateWindowW(PROGRESS_CLASSW, L"", WS_CHILD | PBS_SMOOTH, 126, 212, 220, 6, h, 0, 0, 0);
            g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, 0, WS_POPUP | TTS_ALWAYSTIP,
                                    CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, h, 0, 0, 0);
            refreshChrome();
            return 0;
        }
        case WM_DRAWITEM: {
            DRAWITEMSTRUCT* di = (DRAWITEMSTRUCT*)l;
            bool en = (di->itemState & ODS_DISABLED) == 0;
            bool press = (di->itemState & ODS_SELECTED) != 0;
            HBRUSH b = CreateSolidBrush(en ? (press ? RGB(225, 230, 238) : RGB(255, 255, 255)) : RGB(120, 128, 140));
            HPEN pn = CreatePen(PS_SOLID, 1, en ? RGB(255, 255, 255) : RGB(120, 128, 140));
            HGDIOBJ ob = SelectObject(di->hDC, b), opn = SelectObject(di->hDC, pn);
            RoundRect(di->hDC, di->rcItem.left, di->rcItem.top, di->rcItem.right, di->rcItem.bottom, 20, 20);
            SelectObject(di->hDC, ob);
            SelectObject(di->hDC, opn);
            DeleteObject(b);
            DeleteObject(pn);
            wchar_t t[64];
            GetWindowTextW(g_btn, t, 64);
            SelectObject(di->hDC, g_bold);
            SetBkMode(di->hDC, TRANSPARENT);
            SetTextColor(di->hDC, en ? RGB(20, 25, 34) : RGB(230, 234, 240));
            DrawTextW(di->hDC, t, -1, &di->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            return TRUE;
        }
        case WM_PAINT:
            paint(h);
            return 0;
        case WM_TIMER:
            if (g_state == MiniState::Patching) {
                g_dots = (g_dots + 1) % 4;
                g_pos = g_pos >= 100 ? 100 : g_pos + 3;
                SendMessageW(g_prog, PBM_SETPOS, g_pos, 0);
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        case WM_STEP:
            g_stepIdx = (int)l;
            g_step = kSteps[g_stepIdx < 4 ? g_stepIdx : 3];
            g_dots = 0;
            SendMessageW(g_prog, PBM_SETPOS, (int)w, 0);
            g_pos = (int)w;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_DONE: {
            Work* q = (Work*)l;
            int rc = (int)w;
            g_busy = false;
            KillTimer(h, 1);
            if (rc == 1) {
                g_backup = q->backup;
                g_snap[g_target] = EngineScanChannel(kChannels[g_target]);
                g_state = (g_snap[g_target].status == EngineStatus::Ready) ? MiniState::Done : MiniState::NeedsRepair;
                SendMessageW(g_prog, PBM_SETPOS, 100, 0);
                if (g_state == MiniState::Done && !g_backup.empty()) setTip(g_backup);
            } else {
                g_state = MiniState::NeedsRepair;
                MessageBoxW(h, L"Repair did not complete. Nothing was verified; see the full-size repair window for details.",
                            L"Brave Origin", MB_ICONERROR);
            }
            delete q;
            refreshChrome();
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(w) == 100 && !g_busy.load()) {
                if (g_state == MiniState::Ready || g_state == MiniState::Done) openTarget();
                else if (g_state == MiniState::NeedsRepair) startPatch();
                else doScan();
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            g_hwnd = nullptr;
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmd, int show) {
    if (cmd && *cmd) return EngineRunCli();
    HMODULE u = GetModuleHandleW(L"user32.dll");
    auto dpi = (BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT))GetProcAddress(u, "SetProcessDpiAwarenessContext");
    if (dpi) dpi(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX c = { sizeof(c), ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&c);
    g_bg = CreateSolidBrush(RGB(16, 20, 28));
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(IDI_MAIN));
    if (!wc.hIcon) wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hbrBackground = g_bg;
    wc.lpszClassName = L"BraveOriginMiniWindow";
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);
    RECT want = { 0, 0, 472, 272 };
    AdjustWindowRectEx(&want, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0);
    int winW = want.right - want.left, winH = want.bottom - want.top;
    int sx = GetSystemMetrics(SM_CXSCREEN), sy = GetSystemMetrics(SM_CYSCREEN);
    HWND h = CreateWindowExW(0, wc.lpszClassName, L"Brave Origin",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             (sx - winW) / 2, (sy - winH) / 2, winW, winH,
                             nullptr, nullptr, inst, nullptr);
    if (!h) return 2;
    ShowWindow(h, show);
    UpdateWindow(h);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(h, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    return (int)m.wParam;
}
