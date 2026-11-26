# LockDetector (x64)

실행 중인 Windows x64 프로세스의 락 API(CriticalSection / Mutex / SRWLock)를 Microsoft Detours로
후킹해 **락 순서 역전(lock-order inversion)** 을 찾는 진단 도구입니다. 스레드별 보유 락을 추적하고
락 순서 그래프에서 사이클이 생기면 `[WARNING] LOCK ORDER CYCLE` 을 로그에 남깁니다.

> 원본 샘플(`ORIGINAL_README.txt`)을 CI(Windows)에서 실측한 뒤 결함을 고친 버전입니다.
> 변경 근거와 측정값은 아래 "실측 결과"를 참고하세요.

## 구성

| 경로 | 역할 |
|---|---|
| `LockDetector/` | 탐지기 DLL (`LockDetectorStart(path)`, `LockDetectorStop()`) |
| `LockViewer/` | 로그 파일을 500ms마다 tail 하는 GUI |
| `Examples/InversionDemo/` | 역전락·실제 데드락·벤치마크·그래프 증가 시나리오 |
| `ci/run_tests.ps1`, `.github/workflows/ci.yml` | windows-2022 에서 빌드하고 시나리오를 검증 |

## 사용

```text
HMODULE h = LoadLibraryW(L"LockDetector.dll");       // 워커 스레드 시작 전에 로드
auto start = (BOOL (WINAPI*)(LPCWSTR))GetProcAddress(h, "LockDetectorStart");
start(L"C:\\Temp\\LockDetector.log");                  // NULL 이면 LOCKDETECTOR_LOG 환경변수 → 기본값
```

- `LOCKDETECTOR_VERBOSE=1` : ACQUIRE/RELEASE 이벤트를 모두 기록합니다(대용량, 부하 시 일부 드롭).
  기본값은 경고와 시작/종료/통계 줄만 기록합니다.
- 로그 예:
  `[WARNING] TID=5196 LOCK ORDER CYCLE: held=...3C0 wanted=...398 operation=EnterCriticalSection cycle=...398(CS) -> ...3C0(CS) -> ...398(CS)`

## 두 가지 DLL
| 위치 | 런타임 의존 | 용도 |
|---|---|---|
| `LockDetector.dll` | VC++ 2015-2022 x64 재배포 패키지 필요 | 일반 용도 |
| `static-crt/LockDetector.dll` | **`KERNEL32.dll` 만** (재배포 패키지 불필요) | VS2013(v120) 앱 등 재배포 패키지가 없는 환경 |

두 DLL 모두 x64 전용이며 동일한 시나리오 검증을 통과했습니다. 대상 프로세스도 x64 여야 합니다.

## 빌드 (CI와 동일)

```bash
git clone https://github.com/microsoft/Detours third_party/Detours
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DDETOURS_ROOT=third_party/Detours
cmake --build build --config Release
pwsh ci/run_tests.ps1 -BinDir build/bin/Release -OutDir results
```

## 원본에서 고친 것

1. 원본은 그대로는 **컴파일되지 않았습니다** (`LockTracker.cpp`에서 `LockProtocol.h` 누락, `LD_InitializeTracker` 선언 누락).
2. 탐지기를 로드하면 **스레드를 만드는 모든 시나리오가 크래시/hang** 했습니다. 스택 덤프 결과, 신규 스레드가
   ntdll 내부 락(로더·힙)의 후크에서 트래커에 중첩 재진입한 뒤 SRW 락에서 자기 교착에 빠졌습니다.
   → 호출 모듈 필터(반환 주소)로 시스템 DLL이 부른 락은 추적하지 않고, 후크 경로에서 힙/OS 락/파일 I/O를 제거했습니다.
3. 실제 데드락은 획득이 끝난 뒤에만 기록되어 **로그에 남지 않았습니다.** → 블로킹 획득 **전에** 순서 간선을 기록·검사합니다.
4. 간선 확인/DFS가 전체 간선 선형 탐색이라 락 수에 따라 급격히 느려졌습니다. → 인접 리스트 + 방문 스탬프 반복 DFS.
5. 뷰어와 DLL의 로그 경로 불일치, 뷰어 Edit 컨트롤 텍스트 한도, Detours 갱신이 현재 스레드에만 적용되던 문제를 수정했습니다.

## 실측 결과 (GitHub Actions `windows-2022`, 2 vCPU, Release)

| 항목 | 원본 | 개선 |
|---|---|---|
| 역전락 CS/Mutex/SRW 탐지 | 크래시 또는 hang, 경고 0건 | 3종 모두 경고 1건, 거짓 양성 0건 |
| 실제 데드락 | 경고 없음 | 블록된 상태에서 경고 기록 |
| 일관된 순서 / shared-only | (실행 불가) | 경고 없음 |
| `std::mutex` 역전·데드락 (`/MD`, `/MT`, VS2013 `msvcp120.dll`) | 해당 없음(미지원) | 모두 탐지, 재귀·일관 순서는 경고 없음 |
| 락 300개 중첩 획득(라운드당) | 약 1,190~1,440 ms | 약 8~14 ms |
| CS lock+unlock, 1스레드 | (측정 불가) | 10 ns → 약 95 ns |
| SRW lock+unlock, 1스레드 | (측정 불가) | 10 ns → 약 53 ns |
| Mutex / 비뮤텍스 Wait | (측정 불가) | 거의 변화 없음 |

`LOCKDETECTOR_VERBOSE=1` 이면 CS 1스레드 약 870 ns(로그 I/O 비용)입니다. 진단 빌드이므로 성능 측정에는 쓰지 마세요.

## 한계

- VS2013(v120) 빌드는 검증하지 못했습니다. CI는 VS2022(v143)로 빌드합니다. 소스는 C++11 범위를 유지했습니다.
- `std::mutex` / `std::recursive_mutex`:
  - `/MD` 동적 CRT: VS2015+는 `msvcp140.dll`, **VS2013(v120)은 `msvcp120.dll`** 의 `_Mtx_lock/_trylock/_timedlock/_unlock/_destroy` 를 후킹해 추적합니다(디버그 CRT `msvcp140d.dll`/`msvcp120d.dll` 포함). **탐지기를 시작하는 시점에 해당 DLL이 이미 로드되어 있어야 합니다.** 시작 로그에 모듈별 후킹 결과가 남습니다(예: `msvcp120.dll _Mtx_* 5/5 (VS2013/v120)`).
  - `/MT` 정적 CRT: 내부 SRWLOCK 호출로 추적됩니다. 경고의 락 주소는 `std::mutex` 객체 주소보다 0x10 큽니다.
  - VS2013 의 `_Mtx_*` 는 핸들이 아니라 **핸들의 주소(`_Mtx_t*`)** 를 인자로 받으므로, v120 에서는 경고의 락 주소가 `std::mutex` 객체 안의 `_Mtx` 멤버 주소입니다.
  - v120 검증 방식: CI 에서 VC++ 2013 x64 런타임을 설치하고 **실제 `msvcp120.dll` 의 export 를 데모가 직접 호출**해 확인했습니다. VS2013 으로 빌드한 실제 앱에서 돌려 본 것은 아닙니다. `Concurrency::critical_section` 을 앱이 직접 쓰는 경우는 추적하지 않습니다.
- `WaitForMultipleObjects`, `SignalObjectAndWait`, `OpenMutex`, `DuplicateHandle` 은 후킹하지 않습니다.
- 시작 이전에 만든 뮤텍스 핸들은 등록되지 않습니다.
- 시스템 폴더(`\Windows\System32` 등)의 모듈이 호출한 락은 추적하지 않습니다.
- 용량은 고정입니다: 락 131,072개, 간선 1,048,576개, 스레드 2,048개. 초과분은 경고 후 추적을 중단합니다.
- `LockDetectorStop` 은 다른 스레드가 후크 안에 있는 동안 호출하면 안전하지 않습니다(Detours 제약).
- 이 도구는 락 순서 역전을 찾는 휴리스틱입니다. 데드락이 없다는 것을 증명하지 않으며, 실행 중 관측된 순서만 검사합니다.
