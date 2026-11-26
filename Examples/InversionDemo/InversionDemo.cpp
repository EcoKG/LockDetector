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
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

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

static HANDLE Spawn(LPTHREAD_START_ROUTINE f, LPVOID p) {
    return CreateThread(NULL, 0, f, p, 0, NULL);
}
static void RunAndJoin(LPTHREAD_START_ROUTINE f) {
    HANDLE h = Spawn(f, NULL);
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
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

static void ScenarioInversion(const char* kind, bool consistent) {
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
static volatile LONG g_ready = 0;
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
    if (!dll.empty() && !StartDetector(dll.c_str(), log.c_str())) return 1;

    int rc = 0;
    if      (scenario == "inversion-cs")    ScenarioInversion("cs", false);
    else if (scenario == "inversion-mutex") ScenarioInversion("mutex", false);
    else if (scenario == "inversion-srw")   ScenarioInversion("srw", false);
    else if (scenario == "consistent-order")ScenarioInversion("cs", true);
    else if (scenario == "deadlock")        rc = ScenarioDeadlock();
    else if (scenario == "bench" && pos.size() >= 3)
        rc = ScenarioBench(pos[0].c_str(), atoi(pos[1].c_str()), (DWORD)atol(pos[2].c_str()));
    else if (scenario == "edges" && pos.size() >= 2)
        rc = ScenarioEdges(atoi(pos[0].c_str()), atoi(pos[1].c_str()));
    else { Say("ERROR unknown scenario or missing args"); rc = 1; }

    StopDetector();
    return rc;
}
