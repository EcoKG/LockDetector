#include "LockTracker.h"
#include <windows.h>
#include <map>
#include <vector>
#include <string>
#include <stdio.h>
#include <stdarg.h>

struct LockKey {
    void* object;
    int kind;
    bool operator<(const LockKey& r) const {
        if (object != r.object) return object < r.object;
        return kind < r.kind;
    }
    bool operator==(const LockKey& r) const {
        return object == r.object && kind == r.kind;
    }
};
struct Edge {
    LockKey from, to;
};
static CRITICAL_SECTION g_guard;
static bool g_guardReady = false;
static std::map<DWORD, std::vector<LockKey> > g_held;
static std::vector<Edge> g_edges;
static std::wstring g_logPath = L"LockDetector.log";
static __declspec(thread) int g_internal = 0;

static void LockInternal() {
    ++g_internal;
    if (g_guardReady) EnterCriticalSection(&g_guard);
}
static void UnlockInternal() {
    if (g_guardReady) LeaveCriticalSection(&g_guard);
    --g_internal;
}
static bool PathExists(const LockKey& from, const LockKey& target,
                       std::vector<LockKey>& visited) {
    if (from == target) return true;
    for (size_t i=0; i<visited.size(); ++i)
        if (visited[i] == from) return false;
    visited.push_back(from);
    for (size_t i=0; i<g_edges.size(); ++i) {
        if (g_edges[i].from == from &&
            PathExists(g_edges[i].to, target, visited))
            return true;
    }
    return false;
}
static void WriteLine(const char* line) {
    HANDLE h = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, line, (DWORD)strlen(line), &written, NULL);
    CloseHandle(h);
}
void LD_SetLogPath(const wchar_t* path) {
    if (!path || !*path) return;
    LockInternal();
    g_logPath = path;
    UnlockInternal();
}
void LD_LogMessage(const char* level, const char* message) {
    if (g_internal) return;
    char line[LD_MAX_LINE];
    SYSTEMTIME st; GetLocalTime(&st);
    sprintf_s(line, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] [%s] TID=%lu %s\r\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
        st.wSecond, st.wMilliseconds, level, GetCurrentThreadId(), message);
    LockInternal();
    WriteLine(line);
    UnlockInternal();
}
void LD_TrackAcquire(void* object, int kind, const char* operation) {
    if (!object || g_internal || !g_guardReady) return;
    LockInternal();
    DWORD tid = GetCurrentThreadId();
    LockKey acquired; acquired.object = object; acquired.kind = kind;

    std::vector<LockKey>& held = g_held[tid];
    char msg[LD_MAX_LINE];

    for (size_t i=0; i<held.size(); ++i) {
        if (held[i] == acquired) continue; // recursive acquisition
        bool exists = false;
        for (size_t e=0; e<g_edges.size(); ++e)
            if (g_edges[e].from == held[i] && g_edges[e].to == acquired)
                exists = true;
        if (!exists) {
            std::vector<LockKey> visited;
            if (PathExists(acquired, held[i], visited)) {
                sprintf_s(msg, "LOCK ORDER CYCLE: %p(kind=%d) -> %p(kind=%d); operation=%s",
                    held[i].object, held[i].kind, acquired.object, acquired.kind, operation);
                // Write directly while holding the internal guard.
                char line[LD_MAX_LINE];
                SYSTEMTIME st; GetLocalTime(&st);
                sprintf_s(line, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] [WARNING] TID=%lu %s\r\n",
                    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                    st.wSecond, st.wMilliseconds, tid, msg);
                WriteLine(line);
            }
            Edge edge; edge.from = held[i]; edge.to = acquired;
            g_edges.push_back(edge);
        }
    }
    held.push_back(acquired);
    sprintf_s(msg, "ACQUIRE %s object=%p kind=%d depth=%u",
        operation, object, kind, (unsigned)held.size());
    char line[LD_MAX_LINE];
    SYSTEMTIME st; GetLocalTime(&st);
    sprintf_s(line, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] [INFO] TID=%lu %s\r\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
        st.wSecond, st.wMilliseconds, tid, msg);
    WriteLine(line);
    UnlockInternal();
}
void LD_TrackRelease(void* object, int kind, const char* operation) {
    if (!object || g_internal || !g_guardReady) return;
    LockInternal();
    DWORD tid = GetCurrentThreadId();
    LockKey key; key.object = object; key.kind = kind;
    std::map<DWORD, std::vector<LockKey> >::iterator it = g_held.find(tid);
    if (it != g_held.end()) {
        std::vector<LockKey>& held = it->second;
        for (std::vector<LockKey>::reverse_iterator r=held.rbegin(); r!=held.rend(); ++r) {
            if (*r == key) {
                held.erase((r+1).base());
                break;
            }
        }
        if (held.empty()) g_held.erase(it);
    }
    char msg[LD_MAX_LINE], line[LD_MAX_LINE];
    sprintf_s(msg, "RELEASE %s object=%p kind=%d", operation, object, kind);
    SYSTEMTIME st; GetLocalTime(&st);
    sprintf_s(line, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] [INFO] TID=%lu %s\r\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
        st.wSecond, st.wMilliseconds, tid, msg);
    WriteLine(line);
    UnlockInternal();
}
void LD_InitializeTracker() {
    if (!g_guardReady) {
        InitializeCriticalSection(&g_guard);
        g_guardReady = true;
    }
}
