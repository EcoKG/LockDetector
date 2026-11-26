#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <string>
#include "../Common/LockProtocol.h"

static HWND g_edit = NULL;
static HANDLE g_file = INVALID_HANDLE_VALUE;
static LARGE_INTEGER g_position = {0};

static void AppendText(const char* text) {
    int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (n <= 0) {
        n = MultiByteToWideChar(CP_ACP, 0, text, -1, NULL, 0);
        if (n <= 0) return;
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_ACP, 0, text, -1, &w[0], n);
        SendMessageW(g_edit, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
        SendMessageW(g_edit, EM_REPLACESEL, FALSE, (LPARAM)w.c_str());
        return;
    }
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, &w[0], n);
    SendMessageW(g_edit, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
    SendMessageW(g_edit, EM_REPLACESEL, FALSE, (LPARAM)w.c_str());
}
static void PollLog() {
    wchar_t path[MAX_PATH] = L"LockDetector.log";
    DWORD len = GetEnvironmentVariableW(L"LOCKDETECTOR_LOG", path, MAX_PATH);
    if (!len || len >= MAX_PATH) wcscpy_s(path, L"LockDetector.log");
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size)) { CloseHandle(h); return; }
    if (size.QuadPart < g_position.QuadPart) g_position.QuadPart = 0;
    SetFilePointerEx(h, g_position, NULL, FILE_BEGIN);
    char buf[8192]; DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf)-1, &got, NULL) && got) {
        buf[got] = 0; AppendText(buf); g_position.QuadPart += got;
    }
    CloseHandle(h);
}
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,
            0,0,0,0, hwnd, (HMENU)100, GetModuleHandleW(NULL), NULL);
        SetTimer(hwnd, 1, 500, NULL);
        return 0;
    case WM_SIZE:
        MoveWindow(g_edit, 0,0,LOWORD(lp),HIWORD(lp),TRUE); return 0;
    case WM_TIMER:
        PollLog(); return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 1); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
int WINAPI wWinMain(HINSTANCE h, HINSTANCE, LPWSTR, int show) {
    INITCOMMONCONTROLSEX ic = {sizeof(ic), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&ic);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc; wc.hInstance = h;
    wc.lpszClassName = L"LockViewerWindow";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"LockDetector - Live Log",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1000, 650,
        NULL, NULL, h, NULL);
    ShowWindow(hwnd, show);
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m); DispatchMessageW(&m);
    }
    return (int)m.wParam;
}
