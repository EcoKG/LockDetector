// InversionDemo - lock-order inversion / deadlock / overhead scenarios used to
// validate LockDetector. Windows only.
//
// usage: InversionDemo <scenario> [--detector <dll>] [--log <path>] [args...]
//
// scenarios
//   inversion-cs | inversion-mutex | inversion-srw
//        Thread 1 takes A->B, finishes; thread 2 then takes B->A. The two
//        threads never overlap, so the process never hangs, but the lock order
//        is inverted (potential deadlock). Detector must warn.
//   consistent-order
//        Both threads take A->B. Negative control: detector must NOT warn
//        about these locks.
//   deadlock
//        Two threads really deadlock (T1: A->B, T2: B->A, overlapped).
//        The process is terminated after a timeout with exit code 3.
//   bench <mode> <threads> <iters>
//        mode: cs-own | cs-shared | srw-own | mutex-own | event-wait
//   edges <locks> <rounds>
//        One thread acquires <locks> distinct critical sections while holding
//        all previous ones (lock-order graph growth), repeated <rounds> times.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <mutex>

typedef BOOL (WINAPI *PFN_START)(LPCWSTR);
typedef void (WINAPI *PFN_STOP)();

static HMODULE g_det = NULL;
static PFN_STOP g_stop = NULL;

static void Say(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

static bool StartDetector(const wchar_t* dll, const wchar_t* log) {
    g_det = LoadLibraryW(dll);
    if (!g_det) { Say("ERROR LoadLibrary failed gle=%lu", GetLastError()); return false; }
    PFN_START start = (PFN_START)GetProcAddress(g_det, "LockDetectorStart");
    g_stop = (PFN_STOP)GetProcAddress(g_det, "LockDetectorStop");
    if (!start) { Say("ERROR LockDetectorStart not exported"); return false; }
    if (!start(log)) { Say("ERROR LockDetectorStart returned FALSE"); return false; }
    Say("DETECTOR started log=%ls", log);
    return true;
}

static void StopDetector() {
    if (g_stop) g_stop();
}

extern HANDLE g_threads[64]; extern volatile LONG g_threadCount;
static HANDLE Spawn(LPTHREAD_START_ROUTINE f, LPVOID p) {
    HANDLE h = CreateThread(NULL, 0, f, p, 0, NULL);
    LONG i = InterlockedIncrement(&g_threadCount) - 1;
    if (i < 64) g_threads[i] = h;
    return h;
}
static void RunAndJoin(LPTHREAD_START_ROUTINE f) {
    HANDLE h = Spawn(f, NULL);
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
}


// ------------------------------------------------------------ crash report
// Lock-free crash reporter: uses only WriteFile and manual unwinding so it keeps
// working when the crash happens inside the hooked critical-section functions.
// Addresses are printed as module+offset; resolve them with the linker .map.
struct ModInfo { char name[64]; ULONG64 base; ULONG64 size; };
static ModInfo g_mods[256];
static int g_modCount = 0;
static HANDLE g_out = NULL;
static volatile LONG g_inCrash = 0;

static void SnapshotModules() {
    g_modCount = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32 me; me.dwSize = sizeof(me);
    if (Module32First(snap, &me)) do {
        if (g_modCount >= 256) break;
        ModInfo& m = g_mods[g_modCount++];
        strncpy(m.name, me.szModule, 63); m.name[63] = 0;
        m.base = (ULONG64)me.modBaseAddr; m.size = me.modBaseSize;
    } while (Module32Next(snap, &me));
    CloseHandle(snap);
}
static void RawWrite(const char* s) {
    DWORD w; WriteFile(g_out, s, (DWORD)strlen(s), &w, NULL);
}
static void RawHex(char* dst, ULONG64 v) {
    static const char* d = "0123456789ABCDEF";
    int n = 0; char t[20];
    if (!v) t[n++] = '0';
    while (v) { t[n++] = d[v & 15]; v >>= 4; }
    int k = 0; t[n] = 0;
    dst[k++] = '0'; dst[k++] = 'x';
    while (n) dst[k++] = t[--n];
    dst[k] = 0;
}
static void RawAddr(char* dst, ULONG64 a) {
    char h[24];
    for (int i = 0; i < g_modCount; ++i) {
        if (a >= g_mods[i].base && a < g_mods[i].base + g_mods[i].size) {
            strcpy(dst, g_mods[i].name); strcat(dst, "+"); RawHex(h, a - g_mods[i].base); strcat(dst, h);
            return;
        }
    }
    RawHex(dst, a);
}
static void DumpFrames(CONTEXT c) {
    char line[256], a[128], h[24];
    for (int i = 0; i < 40; ++i) {
        ULONG64 pc = c.Rip;
        RawAddr(a, pc);
        strcpy(line, "  frame#"); RawHex(h, i); strcat(line, h); strcat(line, " "); strcat(line, a); strcat(line, "\r\n");
        RawWrite(line);
        ULONG64 base = 0;
        PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(pc, &base, NULL);
        if (!rf) { c.Rip = *(ULONG64*)c.Rsp; c.Rsp += 8; }
        else { PVOID hd = NULL; ULONG64 ef = 0; RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, pc, rf, &c, &hd, &ef, NULL); }
        if (!c.Rip) break;
    }
}

// Thread handles created by this demo, dumped by the watchdog when a run hangs.
HANDLE g_threads[64];
volatile LONG g_threadCount = 0;
static DWORD g_watchdogSec = 25;
static DWORD WINAPI Watchdog(LPVOID) {
    Sleep(g_watchdogSec * 1000);
    RawWrite("WATCHDOG expired: dumping demo threads (lock-free)\r\n");
    char h[24], line[128];
    for (LONG i = 0; i < g_threadCount && i < 64; ++i) {
        HANDLE th = g_threads[i];
        if (!th) continue;
        if (SuspendThread(th) == (DWORD)-1) continue;
        CONTEXT c; memset(&c, 0, sizeof(c)); c.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(th, &c)) {
            strcpy(line, "WATCHDOG thread#"); RawHex(h, i); strcat(line, h);
            strcat(line, " tid="); RawHex(h, GetThreadId(th)); strcat(line, h); strcat(line, "\r\n");
            RawWrite(line);
            DumpFrames(c);
        }
    }
    TerminateProcess(GetCurrentProcess(), 4);
    return 0;
}
static LONG CALLBACK CrashHandler(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_IN_PAGE_ERROR &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedExchange(&g_inCrash, 1)) TerminateProcess(GetCurrentProcess(), 0xC0000005);
    char line[256], a[128], h[24];
    RawWrite("CRASH code="); RawHex(h, code); RawWrite(h);
    RawWrite(" tid="); RawHex(h, GetCurrentThreadId()); RawWrite(h);
    if (ep->ExceptionRecord->NumberParameters >= 2) {
        RawWrite(" av-kind="); RawHex(h, ep->ExceptionRecord->ExceptionInformation[0]); RawWrite(h);
        RawWrite(" av-target="); RawHex(h, ep->ExceptionRecord->ExceptionInformation[1]); RawWrite(h);
    }
    RawWrite("\r\n");
    DumpFrames(*ep->ContextRecord);
    TerminateProcess(GetCurrentProcess(), 0xC0000005);
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------- inversion
static CRITICAL_SECTION csA, csB;
static HANDLE mtxA, mtxB;
static SRWLOCK srwA = SRWLOCK_INIT, srwB = SRWLOCK_INIT;

static DWORD WINAPI CsAB(LPVOID) {
    EnterCriticalSection(&csA); EnterCriticalSection(&csB);
    LeaveCriticalSection(&csB); LeaveCriticalSection(&csA); return 0;
}
static DWORD WINAPI CsBA(LPVOID) {
    EnterCriticalSection(&csB); EnterCriticalSection(&csA);
    LeaveCriticalSection(&csA); LeaveCriticalSection(&csB); return 0;
}
static DWORD WINAPI MtxAB(LPVOID) {
    WaitForSingleObject(mtxA, INFINITE); WaitForSingleObject(mtxB, INFINITE);
    ReleaseMutex(mtxB); ReleaseMutex(mtxA); return 0;
}
static DWORD WINAPI MtxBA(LPVOID) {
    WaitForSingleObject(mtxB, INFINITE); WaitForSingleObject(mtxA, INFINITE);
    ReleaseMutex(mtxA); ReleaseMutex(mtxB); return 0;
}
static DWORD WINAPI SrwAB(LPVOID) {
    AcquireSRWLockExclusive(&srwA); AcquireSRWLockExclusive(&srwB);
    ReleaseSRWLockExclusive(&srwB); ReleaseSRWLockExclusive(&srwA); return 0;
}
static DWORD WINAPI SrwBA(LPVOID) {
    AcquireSRWLockExclusive(&srwB); AcquireSRWLockExclusive(&srwA);
    ReleaseSRWLockExclusive(&srwA); ReleaseSRWLockExclusive(&srwB); return 0;
}

static DWORD WINAPI SrwShAB(LPVOID) {
    AcquireSRWLockShared(&srwA); AcquireSRWLockShared(&srwB);
    ReleaseSRWLockShared(&srwB); ReleaseSRWLockShared(&srwA); return 0;
}
static DWORD WINAPI SrwShBA(LPVOID) {
    AcquireSRWLockShared(&srwB); AcquireSRWLockShared(&srwA);
    ReleaseSRWLockShared(&srwA); ReleaseSRWLockShared(&srwB); return 0;
}
extern volatile LONG g_ready;
static std::mutex smA, smB;
static DWORD WINAPI StdAB(LPVOID) { smA.lock(); smB.lock(); smB.unlock(); smA.unlock(); return 0; }
static DWORD WINAPI StdBA(LPVOID) { smB.lock(); smA.lock(); smA.unlock(); smB.unlock(); return 0; }
static std::recursive_mutex smR;
static DWORD WINAPI StdRecursive(LPVOID) {   // recursive lock of one mutex, then another: must not warn
    smR.lock(); smR.lock(); smB.lock(); smB.unlock(); smR.unlock(); smR.unlock(); return 0;
}
static DWORD WINAPI StdDlT1(LPVOID) {
    smA.lock(); InterlockedIncrement(&g_ready); while (g_ready < 2) Sleep(1); Sleep(50);
    smB.lock(); smB.unlock(); smA.unlock(); return 0;
}
static DWORD WINAPI StdDlT2(LPVOID) {
    smB.lock(); InterlockedIncrement(&g_ready); while (g_ready < 2) Sleep(1); Sleep(50);
    smA.lock(); smA.unlock(); smB.unlock(); return 0;
}
static void ScenarioInversion(const char* kind, bool consistent) {
    if (!strcmp(kind, "std-consistent")) {
        Say("LOCKS A=%p B=%p", (void*)&smA, (void*)&smB);
        RunAndJoin(StdAB); RunAndJoin(StdAB);
        Say("RESULT scenario-finished");
        return;
    }
    if (!strcmp(kind, "std-recursive")) {
        Say("LOCKS A=%p B=%p", (void*)&smR, (void*)&smB);
        RunAndJoin(StdRecursive); RunAndJoin(StdRecursive);
        Say("RESULT scenario-finished");
        return;
    }
    if (!strcmp(kind, "std-deadlock")) {
        Say("LOCKS A=%p B=%p", (void*)&smA, (void*)&smB);
        HANDLE h[2] = { Spawn(StdDlT1, NULL), Spawn(StdDlT2, NULL) };
        DWORD r = WaitForMultipleObjects(2, h, TRUE, 3000);
        if (r == WAIT_TIMEOUT) { Say("RESULT deadlock-confirmed"); TerminateProcess(GetCurrentProcess(), 3); }
        Say("RESULT no-deadlock r=%lu", r);
        return;
    }
    if (!strcmp(kind, "std")) {          // std::mutex, opposite order, threads do not overlap
        Say("LOCKS A=%p B=%p", (void*)&smA, (void*)&smB);
        RunAndJoin(StdAB);
        RunAndJoin(StdBA);
        Say("RESULT scenario-finished");
        return;
    }
    if (!strcmp(kind, "srw-shared")) {   // opposite order, but shared/shared can never deadlock
        Say("LOCKS A=%p B=%p", (void*)&srwA, (void*)&srwB);
        RunAndJoin(SrwShAB);
        RunAndJoin(SrwShBA);
        Say("RESULT scenario-finished");
        return;
    }
    if (!strcmp(kind, "cs")) {
        InitializeCriticalSection(&csA); InitializeCriticalSection(&csB);
        Say("LOCKS A=%p B=%p", (void*)&csA, (void*)&csB);
        RunAndJoin(CsAB);
        RunAndJoin(consistent ? CsAB : CsBA);
    } else if (!strcmp(kind, "mutex")) {
        mtxA = CreateMutexW(NULL, FALSE, NULL); mtxB = CreateMutexW(NULL, FALSE, NULL);
        Say("LOCKS A=%p B=%p", (void*)mtxA, (void*)mtxB);
        RunAndJoin(MtxAB);
        RunAndJoin(consistent ? MtxAB : MtxBA);
    } else {
        Say("LOCKS A=%p B=%p", (void*)&srwA, (void*)&srwB);
        RunAndJoin(SrwAB);
        RunAndJoin(consistent ? SrwAB : SrwBA);
    }
    Say("RESULT scenario-finished");
}

// ----------------------------------------------------------------- deadlock
volatile LONG g_ready = 0;
static DWORD WINAPI DlT1(LPVOID) {
    EnterCriticalSection(&csA);
    InterlockedIncrement(&g_ready);
    while (g_ready < 2) Sleep(1);
    Sleep(50);
    EnterCriticalSection(&csB);          // blocks forever
    LeaveCriticalSection(&csB); LeaveCriticalSection(&csA); return 0;
}
static DWORD WINAPI DlT2(LPVOID) {
    EnterCriticalSection(&csB);
    InterlockedIncrement(&g_ready);
    while (g_ready < 2) Sleep(1);
    Sleep(50);
    EnterCriticalSection(&csA);          // blocks forever
    LeaveCriticalSection(&csA); LeaveCriticalSection(&csB); return 0;
}
static int ScenarioDeadlock() {
    InitializeCriticalSection(&csA); InitializeCriticalSection(&csB);
    Say("LOCKS A=%p B=%p", (void*)&csA, (void*)&csB);
    HANDLE h[2] = { Spawn(DlT1, NULL), Spawn(DlT2, NULL) };
    DWORD r = WaitForMultipleObjects(2, h, TRUE, 3000);
    if (r == WAIT_TIMEOUT) {
        Say("RESULT deadlock-confirmed");
        TerminateProcess(GetCurrentProcess(), 3);
    }
    Say("RESULT no-deadlock r=%lu", r);
    return 0;
}

// -------------------------------------------------------------------- bench
struct BenchArg {
    int mode; DWORD iters; HANDLE startEvt; CRITICAL_SECTION* shared; HANDLE evt;
};
enum { M_CS_OWN, M_CS_SHARED, M_SRW_OWN, M_MUTEX_OWN, M_EVENT_WAIT };

static DWORD WINAPI BenchThread(LPVOID p) {
    BenchArg* a = (BenchArg*)p;
    CRITICAL_SECTION own; InitializeCriticalSection(&own);
    SRWLOCK srw = SRWLOCK_INIT;
    HANDLE m = (a->mode == M_MUTEX_OWN) ? CreateMutexW(NULL, FALSE, NULL) : NULL;
    WaitForSingleObject(a->startEvt, INFINITE);
    DWORD n = a->iters;
    switch (a->mode) {
    case M_CS_OWN:    for (DWORD i = 0; i < n; ++i) { EnterCriticalSection(&own); LeaveCriticalSection(&own); } break;
    case M_CS_SHARED: for (DWORD i = 0; i < n; ++i) { EnterCriticalSection(a->shared); LeaveCriticalSection(a->shared); } break;
    case M_SRW_OWN:   for (DWORD i = 0; i < n; ++i) { AcquireSRWLockExclusive(&srw); ReleaseSRWLockExclusive(&srw); } break;
    case M_MUTEX_OWN: for (DWORD i = 0; i < n; ++i) { WaitForSingleObject(m, INFINITE); ReleaseMutex(m); } break;
    case M_EVENT_WAIT:for (DWORD i = 0; i < n; ++i) { WaitForSingleObject(a->evt, 0); } break;
    }
    if (m) CloseHandle(m);
    DeleteCriticalSection(&own);
    return 0;
}
static int ScenarioBench(const char* mode, int threads, DWORD iters) {
    BenchArg a; a.iters = iters;
    if      (!strcmp(mode, "cs-own"))     a.mode = M_CS_OWN;
    else if (!strcmp(mode, "cs-shared"))  a.mode = M_CS_SHARED;
    else if (!strcmp(mode, "srw-own"))    a.mode = M_SRW_OWN;
    else if (!strcmp(mode, "mutex-own"))  a.mode = M_MUTEX_OWN;
    else if (!strcmp(mode, "event-wait")) a.mode = M_EVENT_WAIT;
    else { Say("ERROR bad bench mode"); return 1; }
    CRITICAL_SECTION shared; InitializeCriticalSection(&shared); a.shared = &shared;
    a.startEvt = CreateEventW(NULL, TRUE, FALSE, NULL);
    a.evt = CreateEventW(NULL, TRUE, TRUE, NULL);   // signaled, manual reset
    std::vector<HANDLE> hs;
    for (int i = 0; i < threads; ++i) hs.push_back(Spawn(BenchThread, &a));
    Sleep(300);                                      // let threads reach the start gate
    LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    SetEvent(a.startEvt);
    WaitForMultipleObjects((DWORD)hs.size(), &hs[0], TRUE, INFINITE);
    QueryPerformanceCounter(&t1);
    double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
    double nsPerOp = ms * 1e6 / ((double)iters);     // wall time / ops per thread
    Say("BENCH mode=%s threads=%d iters=%lu total_ms=%.2f ns_per_op=%.1f",
        mode, threads, (unsigned long)iters, ms, nsPerOp);
    return 0;
}

// -------------------------------------------------------------------- edges
static int ScenarioEdges(int n, int rounds) {
    std::vector<CRITICAL_SECTION> cs((size_t)n);
    for (int i = 0; i < n; ++i) InitializeCriticalSection(&cs[i]);
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    for (int r = 0; r < rounds; ++r) {
        LARGE_INTEGER t0, t1; QueryPerformanceCounter(&t0);
        for (int i = 0; i < n; ++i) EnterCriticalSection(&cs[i]);
        QueryPerformanceCounter(&t1);
        for (int i = n - 1; i >= 0; --i) LeaveCriticalSection(&cs[i]);
        double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
        Say("EDGES locks=%d round=%d acquire_ms=%.2f", n, r, ms);
    }
    return 0;
}


// ------------------------------------------------- msvcp120.dll (VS2013 / v120 STL)
// Calls the _Mtx_* exports of the real msvcp120.dll straight from this executable, exactly
// as an inlined VS2013 std::mutex::lock() would. No v120 compiler is needed. msvcp120.dll
// must be loaded BEFORE the detector starts (hooks only attach to already loaded DLLs).
typedef int (__cdecl *PFN_MInit)(void**, int);
typedef int (__cdecl *PFN_MOp)(void*);
static PFN_MInit mInit; static PFN_MOp mLock, mUnlock;
static void *mA, *mB, *mR;
static bool Msvcp120Load() {
    HMODULE h = LoadLibraryW(L"msvcp120.dll");
    if (!h) return false;
    mInit   = (PFN_MInit)GetProcAddress(h, "_Mtx_init");
    mLock   = (PFN_MOp)GetProcAddress(h, "_Mtx_lock");
    mUnlock = (PFN_MOp)GetProcAddress(h, "_Mtx_unlock");
    return mInit && mLock && mUnlock;
}
static DWORD WINAPI M120AB(LPVOID) { mLock(mA); mLock(mB); mUnlock(mB); mUnlock(mA); return 0; }
static DWORD WINAPI M120BA(LPVOID) { mLock(mB); mLock(mA); mUnlock(mA); mUnlock(mB); return 0; }
static DWORD WINAPI M120Rec(LPVOID) { mLock(mR); mLock(mR); mLock(mB); mUnlock(mB); mUnlock(mR); mUnlock(mR); return 0; }
static DWORD WINAPI M120Dl1(LPVOID) {
    mLock(mA); InterlockedIncrement(&g_ready); while (g_ready < 2) Sleep(1); Sleep(50);
    mLock(mB); mUnlock(mB); mUnlock(mA); return 0;
}
static DWORD WINAPI M120Dl2(LPVOID) {
    mLock(mB); InterlockedIncrement(&g_ready); while (g_ready < 2) Sleep(1); Sleep(50);
    mLock(mA); mUnlock(mA); mUnlock(mB); return 0;
}
static int ScenarioMsvcp120(const std::string& scenario) {
    // _Mtx_plain(1) | _Mtx_try(2), recursive adds 0x100
    mInit(&mA, 0x03); mInit(&mB, 0x03); mInit(&mR, 0x103);
    if (scenario == "recursive-mtx120") Say("LOCKS A=%p B=%p", mR, mB);
    else                                Say("LOCKS A=%p B=%p", mA, mB);
    if (scenario == "inversion-mtx120")  { RunAndJoin(M120AB); RunAndJoin(M120BA); }
    else if (scenario == "consistent-mtx120") { RunAndJoin(M120AB); RunAndJoin(M120AB); }
    else if (scenario == "recursive-mtx120")  { RunAndJoin(M120Rec); RunAndJoin(M120Rec); }
    else if (scenario == "deadlock-mtx120") {
        HANDLE h[2] = { Spawn(M120Dl1, NULL), Spawn(M120Dl2, NULL) };
        DWORD r = WaitForMultipleObjects(2, h, TRUE, 3000);
        if (r == WAIT_TIMEOUT) { Say("RESULT deadlock-confirmed"); TerminateProcess(GetCurrentProcess(), 3); }
        Say("RESULT no-deadlock r=%lu", r);
        return 0;
    }
    Say("RESULT scenario-finished");
    return 0;
}

// --------------------------------------------------------------------- main
static std::wstring W(const char* s) {
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, NULL, 0);
    std::wstring w((size_t)(n > 0 ? n : 1), L'\0');
    if (n > 0) MultiByteToWideChar(CP_ACP, 0, s, -1, &w[0], n);
    return w.c_str();
}

int main(int argc, char** argv) {
    if (argc < 2) { Say("usage: InversionDemo <scenario> [--detector dll] [--log path] [args]"); return 1; }
    std::string scenario = argv[1];
    std::wstring dll, log;
    std::vector<std::string> pos;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--detector") && i + 1 < argc) dll = W(argv[++i]);
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) log = W(argv[++i]);
        else pos.push_back(argv[i]);
    }
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    AddVectoredExceptionHandler(1, CrashHandler);
    {   // watchdog: started before the detector is loaded
        HANDLE hm = NULL;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &hm, 0, FALSE, DUPLICATE_SAME_ACCESS);
        g_threads[0] = hm; g_threadCount = 1;
        CloseHandle(CreateThread(NULL, 0, Watchdog, NULL, 0, NULL));
    }
    bool isM120 = scenario.size() > 7 && scenario.compare(scenario.size() - 7, 7, "-mtx120") == 0;
    if (isM120 && !Msvcp120Load()) { Say("SKIP msvcp120.dll (VC++ 2013 runtime) not available"); return 77; }
    if (!dll.empty() && !StartDetector(dll.c_str(), log.c_str())) return 1;
    SnapshotModules();
    Say("MODULES %d main_tid=%lu", g_modCount, GetCurrentThreadId());
    for (int i = 0; i < g_modCount; ++i)
        if (!_stricmp(g_mods[i].name, "LockDetector.dll") || !_stricmp(g_mods[i].name, "InversionDemo.exe"))
            Say("MODULE %s base=%p size=%llu", g_mods[i].name, (void*)g_mods[i].base, (unsigned long long)g_mods[i].size);

    int rc = 0;
    if      (scenario == "inversion-cs")    ScenarioInversion("cs", false);
    else if (scenario == "inversion-mutex") ScenarioInversion("mutex", false);
    else if (scenario == "inversion-srw")   ScenarioInversion("srw", false);
    else if (scenario == "consistent-order")ScenarioInversion("cs", true);
    else if (scenario == "srw-shared-only") ScenarioInversion("srw-shared", false);
    else if (scenario == "inversion-stdmutex") ScenarioInversion("std", false);
    else if (scenario == "consistent-stdmutex") ScenarioInversion("std-consistent", false);
    else if (scenario == "recursive-stdmutex") ScenarioInversion("std-recursive", false);
    else if (scenario == "deadlock-stdmutex") ScenarioInversion("std-deadlock", false);
    else if (scenario == "deadlock")        rc = ScenarioDeadlock();
    else if (isM120) rc = ScenarioMsvcp120(scenario);
    else if (scenario == "thread-trivial") {   // just create + join one empty thread
        HANDLE h = Spawn([](LPVOID) -> DWORD { return 0; }, NULL);
        WaitForSingleObject(h, INFINITE); CloseHandle(h);
        Say("RESULT thread-trivial-finished");
    }
    else if (scenario == "bench" && pos.size() >= 3)
        rc = ScenarioBench(pos[0].c_str(), atoi(pos[1].c_str()), (DWORD)atol(pos[2].c_str()));
    else if (scenario == "edges" && pos.size() >= 2)
        rc = ScenarioEdges(atoi(pos[0].c_str()), atoi(pos[1].c_str()));
    else { Say("ERROR unknown scenario or missing args"); rc = 1; }

    StopDetector();
    return rc;
}
