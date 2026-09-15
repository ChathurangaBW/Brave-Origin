#include "engine.h"
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <cstdio>
#include <atomic>

static const wchar_t* kChannels[] = {L"Brave-Origin", L"Brave-Origin-Beta", L"Brave-Origin-Nightly"};
static EngineChannelSnapshot g_snap[3];
static HWND g_hwnd, g_channel, g_scan, g_apply, g_restore, g_open, g_progress, g_log;
static HFONT g_titleFont, g_font, g_smallFont;
static HBRUSH g_bgBrush, g_editBrush;
static std::atomic<bool> g_busy(false);
static HANDLE g_worker = nullptr;
static const UINT WM_WORK_DONE = WM_APP + 1;

static std::wstring statusText(EngineStatus s) {
    switch (s) { case EngineStatus::Ready: return L"READY"; case EngineStatus::NeedsRepair: return L"NEEDS REPAIR"; case EngineStatus::Error: return L"CHECK ERROR"; default: return L"NOT INSTALLED"; }
}
static COLORREF statusColor(EngineStatus s) {
    switch (s) { case EngineStatus::Ready: return RGB(55,196,125); case EngineStatus::NeedsRepair: return RGB(255,121,45); case EngineStatus::Error: return RGB(238,80,80); default: return RGB(145,151,165); }
}
static void appendLog(const std::wstring& text) {
    SYSTEMTIME st; GetLocalTime(&st); wchar_t line[2048];
    swprintf(line, 2048, L"[%02d:%02d:%02d] %ls\r\n", st.wHour, st.wMinute, st.wSecond, text.c_str());
    int n = GetWindowTextLengthW(g_log); SendMessageW(g_log, EM_SETSEL, n, n); SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)line); SendMessageW(g_log, EM_SCROLLCARET, 0, 0);
}
static void setBusy(bool busy) {
    EnableWindow(g_channel, !busy); EnableWindow(g_scan, !busy); EnableWindow(g_apply, !busy); EnableWindow(g_restore, !busy); EnableWindow(g_open,!busy);
    ShowWindow(g_progress, busy ? SW_SHOW : SW_HIDE); if (busy) SendMessageW(g_progress, PBM_SETMARQUEE, TRUE, 30);
    if (!busy) SendMessageW(g_progress, PBM_SETMARQUEE, FALSE, 0);
}
static int selectedIndex() { return (int)SendMessageW(g_channel, CB_GETCURSEL, 0, 0); }
static void refreshApply() { int i = selectedIndex(); EnableWindow(g_apply, !g_busy.load() && i >= 0 && g_snap[i].status == EngineStatus::NeedsRepair); }
static void scanAll() {
    appendLog(L"Scanning local Brave Origin profiles (read-only)...");
    for (int i=0;i<3;i++) { g_snap[i] = EngineScanChannel(kChannels[i]); appendLog(std::wstring(kChannels[i]) + L": " + statusText(g_snap[i].status)); }
    refreshApply(); InvalidateRect(g_hwnd, nullptr, FALSE);
}
struct Work { bool restore; int channel; std::wstring path; bool testOnly=false; };
static DWORD WINAPI worker(void* p) {
    Work* w=(Work*)p; int rc; std::wstring backup;
    if(w->testOnly){Sleep(1500);rc=0;} else if (w->restore) rc=EngineRestoreBackup(w->path,&backup); else rc=EngineApplyChannel(kChannels[w->channel], &backup);
    w->path=backup; PostMessageW(g_hwnd, WM_WORK_DONE, (WPARAM)rc, (LPARAM)w); return 0;
}
static void startWork(Work* w) { bool expected=false; if(!g_busy.compare_exchange_strong(expected,true)){delete w;return;} setBusy(true); appendLog(w->restore ? L"Restoring selected backup..." : L"Applying repair and verifying marker after settle..."); g_worker=CreateThread(nullptr,0,worker,w,0,nullptr); if(!g_worker) { delete w; g_busy=false; setBusy(false); refreshApply(); MessageBoxW(g_hwnd,L"Unable to start worker.",L"Brave Origin Fix",MB_ICONERROR); } }
static void chooseRestore() {
    wchar_t file[32768]=L""; OPENFILENAMEW ofn={}; ofn.lStructSize=sizeof(ofn); ofn.hwndOwner=g_hwnd; ofn.lpstrFilter=L"Local State backups (Local State.bak.*)\0Local State.bak.*\0All files\0*.*\0"; ofn.lpstrFile=file; ofn.nMaxFile=32768; ofn.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return; std::wstring p=file; size_t slash=p.find_last_of(L"\\/"); std::wstring name=slash==std::wstring::npos?p:p.substr(slash+1);
    if (name.rfind(L"Local State.bak.",0)!=0) { MessageBoxW(g_hwnd,L"Choose a file named Local State.bak.*",L"Invalid backup",MB_ICONWARNING); return; }
    if (MessageBoxW(g_hwnd,L"Restore this backup over its matching Local State file? The backup must contain valid JSON.",L"Confirm restore",MB_YESNO|MB_ICONQUESTION)!=IDYES) return;
    Work* w=new Work{true,0,p,false}; startWork(w);
}
static void createControls(HWND h) {
    g_font=CreateFontW(-17,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    g_titleFont=CreateFontW(-29,0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    g_smallFont=CreateFontW(-14,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    g_scan=CreateWindowW(L"BUTTON",L"Scan",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,28,438,120,36,h,(HMENU)100,nullptr,nullptr);
    g_apply=CreateWindowW(L"BUTTON",L"Apply Repair",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,158,438,145,36,h,(HMENU)101,nullptr,nullptr);
    g_restore=CreateWindowW(L"BUTTON",L"Restore Backup",WS_CHILD|WS_VISIBLE|WS_TABSTOP,313,438,145,36,h,(HMENU)102,nullptr,nullptr);
    g_open=CreateWindowW(L"BUTTON",L"Open Data Folder",WS_CHILD|WS_VISIBLE|WS_TABSTOP,468,438,150,36,h,(HMENU)103,nullptr,nullptr);
    g_channel=CreateWindowW(WC_COMBOBOXW,L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|CBS_DROPDOWNLIST,646,438,274,200,h,(HMENU)104,nullptr,nullptr);
    for(int i=0;i<3;i++) SendMessageW(g_channel,CB_ADDSTRING,0,(LPARAM)kChannels[i]); SendMessageW(g_channel,CB_SETCURSEL,0,0);
    g_progress=CreateWindowW(PROGRESS_CLASSW,L"",WS_CHILD|PBS_MARQUEE,28,485,892,5,h,nullptr,nullptr,nullptr);
    g_log=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|WS_VSCROLL,28,520,892,145,h,(HMENU)105,nullptr,nullptr);
    for(HWND c: {g_scan,g_apply,g_restore,g_open,g_channel,g_log}) SendMessageW(c,WM_SETFONT,(WPARAM)g_font,TRUE);
    EnableWindow(g_apply,FALSE);
}
static void paint(HWND h) {
    PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps); RECT rc; GetClientRect(h,&rc); FillRect(dc,&rc,g_bgBrush); SetBkMode(dc,TRANSPARENT);
    SetTextColor(dc,RGB(255,121,45)); SelectObject(dc,g_titleFont); TextOutW(dc,28,22,L"Brave Origin Fix",16);
    SetTextColor(dc,RGB(177,183,196)); SelectObject(dc,g_smallFont); TextOutW(dc,30,60,L"v1.0.0  •  Local-state diagnostics & recovery",43);
    SetTextColor(dc,RGB(238,240,245)); SelectObject(dc,g_font); TextOutW(dc,28,95,L"Channel health",14);
    for(int i=0;i<3;i++) { int x=28+i*298; RECT card={x,128,x+276,405}; HBRUSH b=CreateSolidBrush(RGB(36,39,48)); FillRect(dc,&card,b); DeleteObject(b); HPEN pen=CreatePen(PS_SOLID,1,RGB(59,64,76)); SelectObject(dc,pen); SelectObject(dc,GetStockObject(NULL_BRUSH)); RoundRect(dc,card.left,card.top,card.right,card.bottom,12,12); DeleteObject(pen);
        SetTextColor(dc,RGB(244,245,248)); SelectObject(dc,g_font); std::wstring display=i==0?L"Stable":i==1?L"Beta":L"Nightly"; TextOutW(dc,x+18,148,display.c_str(),display.size());
        SetTextColor(dc,statusColor(g_snap[i].status)); SelectObject(dc,g_smallFont); std::wstring st=statusText(g_snap[i].status); TextOutW(dc,x+18,184,st.c_str(),st.size());
        SetTextColor(dc,RGB(153,160,174)); RECT tr={x+18,220,x+258,335}; DrawTextW(dc,g_snap[i].localStatePath.c_str(),-1,&tr,DT_WORDBREAK|DT_EDITCONTROL);
        std::wstring detail=g_snap[i].detail.empty()?L"Not checked yet":g_snap[i].detail; RECT dr={x+18,344,x+258,392}; DrawTextW(dc,detail.c_str(),-1,&dr,DT_WORDBREAK);
    }
    SetTextColor(dc,RGB(150,156,170)); SelectObject(dc,g_smallFont); const wchar_t* disclaimer=L"Unofficial community utility. It does not validate or issue legitimate purchase IDs. No network calls."; TextOutW(dc,28,683,disclaimer,lstrlenW(disclaimer)); EndPaint(h,&ps);
}
static LRESULT CALLBACK wndProc(HWND h,UINT m,WPARAM w,LPARAM l) {
    switch(m) {
    case WM_CREATE: g_hwnd=h; createControls(h); scanAll(); return 0;
    case WM_COMMAND: if(g_busy.load()) return 0; if(HIWORD(w)==CBN_SELCHANGE){refreshApply();return 0;} switch(LOWORD(w)) {
        case 100: scanAll(); break;
        case 101: { int i=selectedIndex(); g_snap[i]=EngineScanChannel(kChannels[i]); if(g_snap[i].status!=EngineStatus::NeedsRepair){MessageBoxW(h,L"This channel is no longer in a repairable state. Scan again.",L"No repair applied",MB_ICONINFORMATION);scanAll();break;} if(MessageBoxW(h,L"A timestamped backup will be created. Only the selected BROKEN profile will be modified. Continue?",L"Confirm Apply Repair",MB_YESNO|MB_DEFBUTTON2|MB_ICONWARNING)==IDYES) startWork(new Work{false,i,L"",false}); break; }
        case 102: chooseRestore(); break;
        case 103: { int i=selectedIndex(); ShellExecuteW(h,L"open",g_snap[i].profilePath.c_str(),nullptr,nullptr,SW_SHOWNORMAL); break; }
    } return 0;
    case WM_WORK_DONE: { Work* q=(Work*)l; if(g_worker){WaitForSingleObject(g_worker,INFINITE);CloseHandle(g_worker);g_worker=nullptr;} g_busy=false; setBusy(false); refreshApply(); if(q->testOnly){delete q;return 0;} if(q->restore) appendLog(w==0?L"Restore completed. Previous target backup: "+q->path:L"Restore failed; see safety diagnostics."); else appendLog(w==1?L"Repair completed. Backup: "+q->path:L"Repair did not complete successfully."); MessageBoxW(h,w==0||(!q->restore&&w==1)?(q->restore?(L"Backup restored. Previous target saved as:\n"+q->path).c_str():(L"Repair verified. Backup:\n"+q->path).c_str()):L"Operation failed. No successful mutation was recorded.",L"Brave Origin Fix",w==0||(!q->restore&&w==1)?MB_ICONINFORMATION:MB_ICONERROR); delete q; scanAll(); return 0; }
    case WM_APP+10: { wchar_t b[8]; if(GetEnvironmentVariableW(L"BRAVEORIGINFIX_UI_TEST",b,8)&&!g_busy.load()){startWork(new Work{false,0,L"",true});return 1;}return 0; }
    case WM_APP+11: return g_busy.load()&&!IsWindowEnabled(g_scan)&&!IsWindowEnabled(g_apply)&&!IsWindowEnabled(g_restore)&&!IsWindowEnabled(g_open)&&!IsWindowEnabled(g_channel);
    case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: { HDC dc=(HDC)w; SetTextColor(dc,RGB(230,232,238)); SetBkColor(dc,RGB(19,21,27)); return (LRESULT)g_editBrush; }
    case WM_CLOSE: if(g_busy.load()){MessageBoxW(h,L"An operation is still being verified. Wait for it to finish before closing.",L"Operation in progress",MB_OK|MB_ICONINFORMATION);return 0;} DestroyWindow(h);return 0;
    case WM_ERASEBKGND:return 1; case WM_PAINT:paint(h);return 0; case WM_DESTROY:if(g_worker){WaitForSingleObject(g_worker,INFINITE);CloseHandle(g_worker);g_worker=nullptr;}PostQuitMessage(0);return 0;
    } return DefWindowProcW(h,m,w,l);
}

int WINAPI wWinMain(HINSTANCE inst,HINSTANCE,LPWSTR cmd,int show) {
    if(cmd && *cmd) return EngineRunCli();
    HMODULE u=GetModuleHandleW(L"user32.dll"); auto dpi=(BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT))GetProcAddress(u,"SetProcessDpiAwarenessContext"); if(dpi) dpi(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX ic={sizeof(ic),ICC_PROGRESS_CLASS}; InitCommonControlsEx(&ic); g_bgBrush=CreateSolidBrush(RGB(25,27,34)); g_editBrush=CreateSolidBrush(RGB(19,21,27));
    WNDCLASSEXW wc={sizeof(wc)}; wc.lpfnWndProc=wndProc; wc.hInstance=inst; wc.hCursor=LoadCursor(nullptr,IDC_ARROW); wc.hIcon=LoadIcon(nullptr,IDI_APPLICATION); wc.hbrBackground=g_bgBrush; wc.lpszClassName=L"BraveOriginFixWindow"; wc.hIconSm=wc.hIcon; RegisterClassExW(&wc);
    HWND h=CreateWindowExW(0,wc.lpszClassName,L"Brave Origin Repair",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,CW_USEDEFAULT,CW_USEDEFAULT,966,742,nullptr,nullptr,inst,nullptr); if(!h)return 2;
    ShowWindow(h,show); UpdateWindow(h); MSG msg; while(GetMessageW(&msg,nullptr,0,0)>0){if(!IsDialogMessageW(h,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}} return (int)msg.wParam;
}
