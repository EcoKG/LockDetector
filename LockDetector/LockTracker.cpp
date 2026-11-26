// LockTracker - lock-order graph with deadlock-safe, allocation-free hook path.
//
// Design rules (derived from CI measurements of the original implementation, whose
// tracker allocated from the heap and took a global critical section inside hooks of
// ntdll-internal locks, which self-deadlocked threads during their own start-up):
//  * hooks only track calls coming from application modules (return-address filter)
//  * the hook path uses fixed, pre-allocated tables and short spin locks only
//  * no heap, no OS lock, no file I/O inside a hook; log lines go through a ring
//    buffer drained by a dedicated writer thread
//  * order edges are recorded BEFORE a blocking acquire so a real deadlock is
//    reported before the threads block
#include "LockTracker.h"
#include "../Common/LockProtocol.h"
#include <psapi.h>
#include <string.h>
#include <wchar.h>

#ifndef _WIN64
#error LockDetector supports x64 only
#endif

typedef unsigned int u32;
typedef unsigned char u8;
typedef unsigned __int64 u64;

enum {
    MAX_NODES   = 1 << 17,
    SLOT_CAP    = 1 << 18,
    MAX_EDGES   = 1 << 20,
    MAX_HELD    = 512,
    MAX_THREADS = 2048,
    CACHE_CAP   = 4096,
    MUTEX_CAP   = 1 << 14,
    RING_SIZE   = 1 << 22,
    DRAIN_CHUNK = 1 << 20,
    RING_RESERVE = 1 << 18      // headroom only WARNING/ERROR lines may use
};
static const u32 SLOT_TOMB = 0xFFFFFFFFu;
enum { MODE_EX = 0, MODE_SH = 1 };

struct Node { void* obj; u32 head; u32 stamp; u32 kind; };
struct Edge { u32 to; u32 next; };

struct LD_Thread {
    DWORD tid;
    LONG busy;
    int depth;
    LD_Thread* nextFree;
    u32 held[MAX_HELD];
    u8 mode[MAX_HELD];
};

// ---- global state ----------------------------------------------------------------
static bool g_ready = false;
static volatile LONG g_lock = 0;        // graph + registry
static volatile LONG g_ringLock = 0;
static volatile LONG g_threadLock = 0;

static Node* g_nodes = NULL;
static u32 g_nodeCount = 0;
static u32* g_slots = NULL;
static Edge* g_edges = NULL;
static u32 g_edgeCount = 0;
static u32* g_stk = NULL;
static u32* g_it = NULL;
static u32 g_gen = 0;
static HANDLE* g_mtx = NULL;
static LD_Thread* g_pool = NULL;
static u32 g_poolUsed = 0;
static LD_Thread* g_free = NULL;
static DWORD g_tls = TLS_OUT_OF_INDEXES;
static volatile u64 g_cache[CACHE_CAP];
static HMODULE g_self = NULL;

static wchar_t g_logPath[2 * MAX_PATH] = L"LockDetector.log";
static HANDLE g_file = INVALID_HANDLE_VALUE;
static HANDLE g_writer = NULL;
static volatile LONG g_stopWriter = 0;
static char* g_ring = NULL;
static u64 g_rhead = 0, g_rtail = 0;
static bool g_verbose = false;
static bool g_edgeFullReported = false;
static bool g_nodeFullReported = false;
static volatile LONG g_nWarn = 0, g_nDropped = 0, g_nEvents = 0;

// ---- spin locks --------------------------------------------------------------------
static void SpinLock(volatile LONG* l) {
    int spins = 0;
    while (InterlockedCompareExchange(l, 1, 0) != 0) {
        if (++spins < 64) YieldProcessor();
        else { SwitchToThread(); spins = 0; }
    }
}
static void SpinUnlock(volatile LONG* l) { InterlockedExchange(l, 0); }

// ---- tiny formatter (no CRT, no locks) ----------------------------------------------
struct Out { char* p; char* end; };
static void PutS(Out& o, const char* s) { while (*s && o.p < o.end) *o.p++ = *s++; }
static void PutDec(Out& o, u64 v) {
    char t[24]; int n = 0;
    if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n && o.p < o.end) *o.p++ = t[--n];
}
static void PutDecPad(Out& o, u64 v, int w) {
    char t[24];
    for (int i = w - 1; i >= 0; --i) { t[i] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < w && o.p < o.end; ++i) *o.p++ = t[i];
}
static void PutPtr(Out& o, const void* ptr) {      // same shape as MSVC %p on x64
    u64 v = (u64)ptr;
    for (int i = 15; i >= 0 && o.p < o.end; --i) *o.p++ = "0123456789ABCDEF"[(v >> (i * 4)) & 15];
}
static const char* KindName(u32 k) {
    switch (k) { case LD_CRITICAL_SECTION: return "CS"; case LD_MUTEX: return "MUTEX";
                 case LD_SRW_EXCLUSIVE: case LD_SRW_SHARED: return "SRW"; default: return "STL"; }
}
static void PutHeader(Out& o, const char* level) {
    SYSTEMTIME st; GetLocalTime(&st);
    PutS(o, "[");
    PutDecPad(o, st.wYear, 4); PutS(o, "-"); PutDecPad(o, st.wMonth, 2); PutS(o, "-"); PutDecPad(o, st.wDay, 2);
    PutS(o, " "); PutDecPad(o, st.wHour, 2); PutS(o, ":"); PutDecPad(o, st.wMinute, 2); PutS(o, ":");
    PutDecPad(o, st.wSecond, 2); PutS(o, "."); PutDecPad(o, st.wMilliseconds, 3);
    PutS(o, "] ["); PutS(o, level); PutS(o, "] TID="); PutDec(o, GetCurrentThreadId()); PutS(o, " ");
}

// ---- ring buffer -> writer thread -----------------------------------------------------
// Verbose INFO traffic can outrun the writer thread and is dropped (counted in 'dropped').
// WARNING/ERROR lines (critical) may also use the reserved headroom so they are not lost.
static void EmitLine(const char* line, size_t len, bool critical) {
    if (!g_ring || !len) return;
    SpinLock(&g_ringLock);
    u64 reserve = critical ? 0 : (u64)RING_RESERVE;
    if ((u64)RING_SIZE - (g_rhead - g_rtail) < len + reserve) {
        InterlockedIncrement(&g_nDropped);
    } else {
        for (size_t i = 0; i < len; ++i) g_ring[(g_rhead + i) & (RING_SIZE - 1)] = line[i];
        g_rhead += len;
    }
    SpinUnlock(&g_ringLock);
}
static void Finish(char* base, Out& o, bool critical = false) {
    PutS(o, "\r\n");
    EmitLine(base, (size_t)(o.p - base), critical);
}

static DWORD WINAPI WriterProc(LPVOID) {
    // permanently "busy": the writer thread is never tracked
    LD_Thread* self = NULL;
    SpinLock(&g_threadLock);
    if (g_free) { self = g_free; g_free = self->nextFree; }
    else if (g_poolUsed < MAX_THREADS) self = &g_pool[g_poolUsed++];
    SpinUnlock(&g_threadLock);
    if (self) { self->busy = 1; self->depth = 0; TlsSetValue(g_tls, self); }

    char* local = (char*)VirtualAlloc(NULL, DRAIN_CHUNK, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!local) return 1;
    for (;;) {
        Sleep(10);
        SpinLock(&g_ringLock);
        u64 n = g_rhead - g_rtail;
        if (n > DRAIN_CHUNK) n = DRAIN_CHUNK;
        for (u64 i = 0; i < n; ++i) local[i] = g_ring[(g_rtail + i) & (RING_SIZE - 1)];
        g_rtail += n;
        bool empty = (g_rhead == g_rtail);
        SpinUnlock(&g_ringLock);
        if (n && g_file != INVALID_HANDLE_VALUE) {
            DWORD w = 0;
            WriteFile(g_file, local, (DWORD)n, &w, NULL);
        }
        if (g_stopWriter && empty) break;
    }
    VirtualFree(local, 0, MEM_RELEASE);
    return 0;
}

// ---- node / edge tables (call with g_lock held) ---------------------------------------
static u32 NodeClass(int kind) { return kind == LD_SRW_SHARED ? (u32)LD_SRW_EXCLUSIVE : (u32)kind; }
static u32 HashKey(void* obj, u32 cls) {
    u64 v = ((u64)obj >> 3) * 0x9E3779B97F4A7C15ull;
    return (u32)(v >> 32) ^ (cls * 0x85EBCA6Bu);
}
static u32 GetNode(void* obj, u32 cls, bool create) {
    u32 h = HashKey(obj, cls) & (SLOT_CAP - 1);
    u32 firstTomb = SLOT_TOMB;
    for (u32 n = 0; n < SLOT_CAP; ++n) {
        u32 idx = (h + n) & (SLOT_CAP - 1);
        u32 id = g_slots[idx];
        if (id == 0) {
            if (!create) return 0;
            if (g_nodeCount >= MAX_NODES) {
                if (!g_nodeFullReported) {
                    g_nodeFullReported = true;
                    char buf[LD_MAX_LINE]; Out o = { buf, buf + sizeof(buf) - 4 };
                    PutHeader(o, "WARNING"); PutS(o, "node table full: new locks are no longer tracked");
                    Finish(buf, o, true);
                }
                return 0;
            }
            id = ++g_nodeCount;
            g_nodes[id].obj = obj; g_nodes[id].kind = cls; g_nodes[id].head = 0; g_nodes[id].stamp = 0;
            g_slots[firstTomb != SLOT_TOMB ? firstTomb : idx] = id;
            return id;
        }
        if (id == SLOT_TOMB) { if (firstTomb == SLOT_TOMB) firstTomb = idx; continue; }
        if (g_nodes[id].obj == obj && g_nodes[id].kind == cls) return id;
    }
    return 0;
}
static void DropNode(void* obj, u32 cls) {
    u32 h = HashKey(obj, cls) & (SLOT_CAP - 1);
    for (u32 n = 0; n < SLOT_CAP; ++n) {
        u32 idx = (h + n) & (SLOT_CAP - 1);
        u32 id = g_slots[idx];
        if (id == 0) return;
        if (id != SLOT_TOMB && g_nodes[id].obj == obj && g_nodes[id].kind == cls) { g_slots[idx] = SLOT_TOMB; return; }
    }
}
static bool AddEdge(u32 from, u32 to) {
    for (u32 e = g_nodes[from].head; e; e = g_edges[e].next)
        if (g_edges[e].to == to) return false;
    if (g_edgeCount >= MAX_EDGES) {
        if (!g_edgeFullReported) {
            g_edgeFullReported = true;
            char buf[LD_MAX_LINE]; Out o = { buf, buf + sizeof(buf) - 4 };
            PutHeader(o, "WARNING"); PutS(o, "edge table full: new lock-order edges are no longer recorded");
            Finish(buf, o, true);
        }
        return false;
    }
    u32 e = ++g_edgeCount;
    g_edges[e].to = to; g_edges[e].next = g_nodes[from].head; g_nodes[from].head = e;
    return true;
}
// iterative DFS start ~> target; on success g_stk[0..len-1] holds the path
static u32 FindPath(u32 start, u32 target) {
    ++g_gen;
    g_stk[0] = start; g_it[0] = g_nodes[start].head; g_nodes[start].stamp = g_gen;
    if (start == target) return 1;
    u32 sp = 1;
    while (sp) {
        u32 e = g_it[sp - 1];
        if (!e) { --sp; continue; }
        g_it[sp - 1] = g_edges[e].next;
        u32 nx = g_edges[e].to;
        if (g_nodes[nx].stamp == g_gen) continue;
        g_nodes[nx].stamp = g_gen;
        if (nx == target) { g_stk[sp] = nx; return sp + 1; }
        if (sp < MAX_NODES - 1) { g_stk[sp] = nx; g_it[sp] = g_nodes[nx].head; ++sp; }
    }
    return 0;
}

static void ReportCycle(u32 held, u32 wanted, u32 len, const char* op) {
    char buf[LD_MAX_LINE]; Out o = { buf, buf + sizeof(buf) - 4 };
    PutHeader(o, "WARNING");
    PutS(o, "LOCK ORDER CYCLE: held="); PutPtr(o, g_nodes[held].obj);
    PutS(o, " wanted="); PutPtr(o, g_nodes[wanted].obj);
    PutS(o, " operation="); PutS(o, op);
    PutS(o, " cycle=");
    for (u32 i = 0; i < len && i < 12; ++i) {
        PutPtr(o, g_nodes[g_stk[i]].obj); PutS(o, "("); PutS(o, KindName(g_nodes[g_stk[i]].kind)); PutS(o, ") -> ");
    }
    if (len > 12) PutS(o, "... -> ");
    PutPtr(o, g_nodes[wanted].obj); PutS(o, "("); PutS(o, KindName(g_nodes[wanted].kind)); PutS(o, ")");
    Finish(buf, o, true);
    InterlockedIncrement(&g_nWarn);
}

// order check for acquiring node 'w' while thread t holds other locks
static void CheckOrder(LD_Thread* t, u32 w, int mode, const char* op) {
    for (int i = 0; i < t->depth; ++i) {
        u32 h = t->held[i];
        if (h == w) continue;                                   // recursive acquisition
        if (t->mode[i] == MODE_SH && mode == MODE_SH) continue; // shared->shared cannot deadlock
        bool exists = false;
        for (u32 e = g_nodes[h].head; e; e = g_edges[e].next) if (g_edges[e].to == w) { exists = true; break; }
        if (exists) continue;
        u32 len = FindPath(w, h);                               // path w ~> h closes a cycle with h -> w
        AddEdge(h, w);
        if (len) ReportCycle(h, w, len, op);
    }
}

// ---- caller classification ----------------------------------------------------------------
static int Classify(void* addr) {   // 1 = application code (track), 2 = ignore
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(addr, &mbi, sizeof(mbi)) != sizeof(mbi)) return 2;
    if (mbi.Type != MEM_IMAGE) return 1;                        // generated code etc.
    if (mbi.AllocationBase == (void*)g_self) return 2;
    wchar_t path[MAX_PATH];
    DWORD n = GetMappedFileNameW(GetCurrentProcess(), mbi.AllocationBase, path, MAX_PATH);
    if (!n) return 1;
    for (DWORD i = 0; i < n; ++i) if (path[i] >= L'A' && path[i] <= L'Z') path[i] = (wchar_t)(path[i] + 32);
    if (wcsstr(path, L"\\windows\\system32\\") || wcsstr(path, L"\\windows\\syswow64\\") ||
        wcsstr(path, L"\\windows\\winsxs\\")) return 2;
    return 1;
}
static bool IsUserCaller(LD_Thread* t, void* ret) {
    u64 tag = (u64)ret >> 16;
    u64 e = g_cache[tag & (CACHE_CAP - 1)];
    if ((e >> 2) == tag && (e & 3)) return (e & 3) == 1;
    t->busy = 1;
    int cls = Classify(ret);
    t->busy = 0;
    g_cache[tag & (CACHE_CAP - 1)] = (tag << 2) | (u64)cls;
    return cls == 1;
}

// ---- thread state ---------------------------------------------------------------------------
static __forceinline LD_Thread* TlsFast() {
    if (g_tls < 64) return (LD_Thread*)__readgsqword(0x1480 + g_tls * 8);   // TEB.TlsSlots
    return (LD_Thread*)TlsGetValue(g_tls);
}
static LD_Thread* AllocThread() {
    SpinLock(&g_threadLock);
    LD_Thread* t = g_free;
    if (t) g_free = t->nextFree;
    else if (g_poolUsed < MAX_THREADS) t = &g_pool[g_poolUsed++];
    SpinUnlock(&g_threadLock);
    if (t) { t->tid = GetCurrentThreadId(); t->busy = 0; t->depth = 0; t->nextFree = NULL; }
    return t;
}
void LD_ThreadExit() {
    if (!g_ready) return;
    LD_Thread* t = TlsFast();
    if (!t) return;
    TlsSetValue(g_tls, NULL);
    t->depth = 0; t->busy = 0;
    SpinLock(&g_threadLock);
    t->nextFree = g_free; g_free = t;
    SpinUnlock(&g_threadLock);
}

LD_Thread* LD_Begin(void* ret) {
    if (!g_ready) return NULL;
    LD_Thread* t = TlsFast();
    if (!t) {
        t = AllocThread();
        if (!t) return NULL;
        TlsSetValue(g_tls, t);
    }
    if (t->busy) return NULL;
    if (ret && !IsUserCaller(t, ret)) return NULL;
    t->busy = 1;
    return t;
}
void LD_End(LD_Thread* t) { if (t) t->busy = 0; }

// ---- public tracking API ----------------------------------------------------------------------
static void LogEvent(LD_Thread* t, const char* what, const char* op, void* object, int kind, int depth) {
    char buf[LD_MAX_LINE]; Out o = { buf, buf + sizeof(buf) - 4 };
    PutHeader(o, "INFO");
    PutS(o, what); PutS(o, " "); PutS(o, op); PutS(o, " object="); PutPtr(o, object);
    PutS(o, " kind="); PutDec(o, (u64)kind);
    if (depth >= 0) { PutS(o, " depth="); PutDec(o, (u64)depth); }
    Finish(buf, o);
    (void)t;
}

void LD_BeforeAcquire(LD_Thread* t, void* object, int kind, const char* op) {
    if (!t || !object) return;
    SpinLock(&g_lock);
    u32 w = GetNode(object, NodeClass(kind), true);
    if (w) CheckOrder(t, w, kind == LD_SRW_SHARED ? MODE_SH : MODE_EX, op);
    SpinUnlock(&g_lock);
}

void LD_AfterAcquire(LD_Thread* t, void* object, int kind, const char* op, bool checkOrder) {
    if (!t || !object) return;
    SpinLock(&g_lock);
    u32 w = GetNode(object, NodeClass(kind), true);
    int mode = (kind == LD_SRW_SHARED) ? MODE_SH : MODE_EX;
    if (w && checkOrder) CheckOrder(t, w, mode, op);
    SpinUnlock(&g_lock);
    if (w && t->depth < MAX_HELD) { t->held[t->depth] = w; t->mode[t->depth] = (u8)mode; ++t->depth; }
    InterlockedIncrement(&g_nEvents);
    if (g_verbose) LogEvent(t, "ACQUIRE", op, object, kind, t->depth);
}

void LD_Release(LD_Thread* t, void* object, int kind, const char* op) {
    if (!t || !object) return;
    SpinLock(&g_lock);
    u32 w = GetNode(object, NodeClass(kind), false);
    SpinUnlock(&g_lock);
    if (w) {
        for (int i = t->depth - 1; i >= 0; --i) {
            if (t->held[i] == w) {
                for (int j = i; j + 1 < t->depth; ++j) { t->held[j] = t->held[j + 1]; t->mode[j] = t->mode[j + 1]; }
                --t->depth;
                break;
            }
        }
    }
    InterlockedIncrement(&g_nEvents);
    if (g_verbose) LogEvent(t, "RELEASE", op, object, kind, -1);
}

void LD_ObjectDestroyed(void* object, int kind) {
    if (!object) return;
    SpinLock(&g_lock);
    DropNode(object, NodeClass(kind));
    SpinUnlock(&g_lock);
}

// ---- mutex handle registry -----------------------------------------------------------------------
static u32 HandleHash(HANDLE h) { return (u32)(((u64)h >> 2) * 2654435761u) & (MUTEX_CAP - 1); }
void LD_RegisterMutex(HANDLE h) {
    if (!g_ready || !h || h == INVALID_HANDLE_VALUE) return;
    SpinLock(&g_lock);
    u32 i = HandleHash(h); int first = -1;
    for (u32 n = 0; n < MUTEX_CAP; ++n, i = (i + 1) & (MUTEX_CAP - 1)) {
        HANDLE v = g_mtx[i];
        if (v == h) { first = -2; break; }
        if (v == (HANDLE)1 && first < 0) first = (int)i;
        if (v == NULL) { if (first < 0) first = (int)i; break; }
    }
    if (first >= 0) g_mtx[first] = h;
    SpinUnlock(&g_lock);
}
bool LD_IsMutex(HANDLE h) {
    if (!g_ready || !h) return false;
    bool found = false;
    SpinLock(&g_lock);
    u32 i = HandleHash(h);
    for (u32 n = 0; n < MUTEX_CAP; ++n, i = (i + 1) & (MUTEX_CAP - 1)) {
        HANDLE v = g_mtx[i];
        if (v == h) { found = true; break; }
        if (v == NULL) break;
    }
    SpinUnlock(&g_lock);
    return found;
}
void LD_UnregisterMutex(HANDLE h) {
    if (!g_ready || !h) return;
    SpinLock(&g_lock);
    u32 i = HandleHash(h);
    for (u32 n = 0; n < MUTEX_CAP; ++n, i = (i + 1) & (MUTEX_CAP - 1)) {
        HANDLE v = g_mtx[i];
        if (v == h) { g_mtx[i] = (HANDLE)1; break; }
        if (v == NULL) break;
    }
    SpinUnlock(&g_lock);
}

// ---- lifecycle ----------------------------------------------------------------------------------------
static void* Reserve(size_t bytes) {
    return VirtualAlloc(NULL, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}
void LD_SetSelfModule(HMODULE self) { g_self = self; }

BOOL LD_InitializeTracker() {
    if (g_ready) return TRUE;
    g_nodes = (Node*)Reserve(sizeof(Node) * (MAX_NODES + 1));
    g_slots = (u32*)Reserve(sizeof(u32) * SLOT_CAP);
    g_edges = (Edge*)Reserve(sizeof(Edge) * (MAX_EDGES + 1));
    g_stk   = (u32*)Reserve(sizeof(u32) * (MAX_NODES + 1));
    g_it    = (u32*)Reserve(sizeof(u32) * (MAX_NODES + 1));
    g_mtx   = (HANDLE*)Reserve(sizeof(HANDLE) * MUTEX_CAP);
    g_pool  = (LD_Thread*)Reserve(sizeof(LD_Thread) * MAX_THREADS);
    g_ring  = (char*)Reserve(RING_SIZE);
    if (!g_nodes || !g_slots || !g_edges || !g_stk || !g_it || !g_mtx || !g_pool || !g_ring) return FALSE;
    g_tls = TlsAlloc();
    if (g_tls == TLS_OUT_OF_INDEXES) return FALSE;
    wchar_t v[8];
    DWORD n = GetEnvironmentVariableW(L"LOCKDETECTOR_VERBOSE", v, 8);
    g_verbose = (n > 0 && n < 8 && v[0] != L'0');
    g_ready = true;
    return TRUE;
}

void LD_SetLogPath(const wchar_t* path) {
    wchar_t env[2 * MAX_PATH];
    if (!path || !*path) {
        DWORD n = GetEnvironmentVariableW(LD_LOG_PATH_ENV, env, 2 * MAX_PATH);
        if (n > 0 && n < 2 * MAX_PATH) path = env;
        else path = LD_DEFAULT_LOG;
    }
    size_t i = 0;
    for (; path[i] && i < 2 * MAX_PATH - 1; ++i) g_logPath[i] = path[i];
    g_logPath[i] = 0;
}

BOOL LD_StartLogger() {
    if (!g_ready) return FALSE;
    if (g_writer) return TRUE;
    g_file = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_file == INVALID_HANDLE_VALUE) return FALSE;
    g_stopWriter = 0;
    g_writer = CreateThread(NULL, 0, WriterProc, NULL, 0, NULL);
    return g_writer != NULL;
}

void LD_StopLogger() {
    if (!g_writer) return;
    g_stopWriter = 1;
    WaitForSingleObject(g_writer, 5000);
    CloseHandle(g_writer); g_writer = NULL;
    if (g_file != INVALID_HANDLE_VALUE) { CloseHandle(g_file); g_file = INVALID_HANDLE_VALUE; }
}

void LD_LogMessage(const char* level, const char* message) {
    if (!g_ready) return;
    char buf[LD_MAX_LINE]; Out o = { buf, buf + sizeof(buf) - 4 };
    PutHeader(o, level);
    PutS(o, message);
    Finish(buf, o, level[0] != 'I');
}

void LD_LogStats() {
    char buf[LD_MAX_LINE]; Out o = { buf, buf + sizeof(buf) - 4 };
    PutHeader(o, "INFO");
    PutS(o, "STATS nodes="); PutDec(o, g_nodeCount);
    PutS(o, " edges="); PutDec(o, g_edgeCount);
    PutS(o, " warnings="); PutDec(o, (u64)g_nWarn);
    PutS(o, " events="); PutDec(o, (u64)g_nEvents);
    PutS(o, " dropped="); PutDec(o, (u64)g_nDropped);
    Finish(buf, o, true);
}

// ---- original API (kept for compatibility / STL wrapper strategy) ---------------------------------------
void LD_TrackAcquire(void* object, int kind, const char* operation) {
    LD_Thread* t = LD_Begin(NULL);
    if (!t) return;
    LD_AfterAcquire(t, object, kind, operation, true);
    LD_End(t);
}
void LD_TrackRelease(void* object, int kind, const char* operation) {
    LD_Thread* t = LD_Begin(NULL);
    if (!t) return;
    LD_Release(t, object, kind, operation);
    LD_End(t);
}
