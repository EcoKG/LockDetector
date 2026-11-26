LockDetector x64 - Visual Studio 2013 / C++11
================================================

WHAT THIS SAMPLE DOES
---------------------
- Detours hooks:
  EnterCriticalSection / TryEnterCriticalSection / LeaveCriticalSection
  CreateMutexW / WaitForSingleObject / ReleaseMutex
  SRW exclusive/shared acquire/release and try-acquire APIs
- Tracks per-thread held locks.
- Builds a lock-order dependency graph and reports cycles.
- Writes LockDetector.log.
- LockViewer tails the log file every 500 ms.

IMPORTANT LIMITATIONS
---------------------
1. This is a starter runtime detector, not a proof of deadlock freedom.
2. std::mutex/std::recursive_mutex are NOT directly hooked. Use the optional
   wrapper approach below or add a tested hook for the exact MSVC runtime.
3. Mutex handles created before LockDetectorStart are not registered.
4. CreateMutexA, OpenMutex, WaitForMultipleObjects, SignalObjectAndWait,
   DuplicateHandle and CloseHandle are not covered in this sample.
5. SRW shared-lock dependencies can produce conservative/false-positive
   cycles because shared ownership is not modeled as a reader/writer graph.
6. Critical-section deletion/reinitialization and handle reuse need lifecycle
   instrumentation for long-running processes.
7. Do not use this diagnostic build for performance measurements.
8. Run only on test systems first. API hooks can affect timing and expose
   pre-existing races.

BUILD REQUIREMENTS
------------------
- Visual Studio 2013 Update 5
- Windows 8.1 SDK or Windows SDK matching target
- Microsoft Detours source built for x64 with the same toolset
- Set DETOURS_ROOT to the Detours directory.

Build Detours x64 first using its VS solution / nmake instructions.
The Detours library and headers must match x64.

PROJECT SETUP
-------------
Create two Win32 projects:
A) LockDetector: Dynamic-Link Library, x64, v120, /MDd (Debug)
B) LockViewer: Windows Application, x64, v120

Add LockDetector.cpp, LockTracker.cpp, LockHooks.cpp and the .def file
to the DLL project. Add LockViewer.cpp to the GUI project.

DLL project:
- C/C++ -> Additional Include Directories: $(DETOURS_ROOT)\src
- Linker -> Additional Library Directories: directory containing detours.lib
- Linker -> Input -> Additional Dependencies: detours.lib;%(AdditionalDependencies)
- Linker -> Module Definition File: LockDetector.def
- C/C++ -> Preprocessor: _WIN32_WINNT=0x0601;WIN32_LEAN_AND_MEAN
- Character Set: Use Unicode Character Set
- Platform: x64
- Toolset: Visual Studio 2013 (v120)

Viewer project:
- Platform x64
- Toolset v120
- Subsystem Windows
- Link: Comctl32.lib

STARTUP
-------
1. Start LockViewer.exe.
2. In the target process, load LockDetector.dll using LoadLibraryW.
3. Resolve LockDetectorStart using GetProcAddress and call it.
4. Pass an absolute log path, e.g. L"C:\\Temp\\LockDetector.log".
5. Reproduce the workload.
6. Review [WARNING] LOCK ORDER CYCLE entries.
7. Call LockDetectorStop before unloading the DLL.

Example injection-free initialization in the target application:
    HMODULE h = LoadLibraryW(L"LockDetector.dll");
    typedef BOOL (WINAPI *PFN_START)(LPCWSTR);
    PFN_START start = (PFN_START)GetProcAddress(h, "LockDetectorStart");
    if (start) start(L"C:\\Temp\\LockDetector.log");

Do not call LoadLibrary/LockDetectorStart from DllMain of another DLL.
The DLL should be loaded after process startup and before worker threads
begin, otherwise early locks will not be observed.

STL LOCK WRAPPER STRATEGY
-------------------------
For locks that can be changed with minimal source edits, introduce a wrapper
around std::mutex and std::recursive_mutex. The wrapper must call
LD_TrackAcquire only after successful lock/try_lock and LD_TrackRelease
before unlock. This requires exposing a small C ABI from the detector or
compiling the tracker helper into the wrapper. Do not assume MSVC's internal
std::mutex implementation is stable across toolset versions.

PRODUCTION HARDENING BEFORE BROAD DEPLOYMENT
--------------------------------------------
- Add hooks for CreateMutexA/OpenMutex/WaitForMultipleObjects and handle
  duplication/close lifecycle.
- Add SRW reader/writer-aware dependency semantics.
- Add critical-section init/delete tracking and object generation IDs.
- Add TLS recursion protection for every hook entry.
- Add bounded lock-free event buffering if file I/O overhead is too high.
- Add PDB symbolization with StackWalk64/DbgHelp outside hook critical paths.
- Add thread-exit cleanup and abandoned mutex ownership reconciliation.
- Test recursive locks, timeout, abandoned mutex, TryLock failure, shared
  SRW readers, handle reuse and lock destruction.
