# LockDetector {{TAG}} (x64)

VS2013(v120) 프로세스에서도 쓸 수 있도록 **정적 CRT DLL** 과 **v120 `std::mutex` 추적**을 추가한 버전입니다.

## 첨부 파일
- `LockDetector-{{TAG}}-x64-bin.zip`
  - `LockDetector.dll` : 동적 CRT (VC++ 2015-2022 x64 재배포 패키지 필요)
  - `static-crt/LockDetector.dll` : **정적 CRT, `KERNEL32.dll` 만 의존** (재배포 패키지 불필요, v120 앱 등에 권장)
  - `LockViewer.exe`, `InversionDemo.exe`, `InversionDemoMT.exe`, PDB, 테스트 결과(`test-results/`, `test-results-static-crt/`), README, Detours 라이선스
- `LockDetector-{{TAG}}-src.zip` : 소스 전체 (CMake 빌드, 예제, CI 스크립트 포함)

두 DLL 모두 **x64 전용**입니다. 바이너리는 GitHub Actions `windows-2022` 에서 VS2022(v143) Release x64 로 빌드했고, VS2013(v120) 으로 빌드해 보지는 않았습니다.

## 이전 버전 대비 변경
- **정적 CRT(`/MT`) DLL 추가**: 의존성이 `KERNEL32.dll` 뿐임을 CI 에서 `dumpbin /dependents` 로 확인합니다.
- **VS2013(v120) `std::mutex` 추적**: `msvcp120.dll`(및 디버그 `msvcp120d.dll`)의 `_Mtx_*` 를 후킹합니다. VS2013 의 `_Mtx_*` 는 핸들이 아니라 핸들 주소를 인자로 받는다는 점을 디스어셈블리로 확인해 반영했습니다.
- 시작 로그에 STL 후킹 결과를 모듈별로 남깁니다(후킹 성공 `5/5`, 일부 누락 `PARTIAL`, 미로드 안내).
- CI 검증이 두 DLL 각각 44건(총 88건)으로 늘었고, 실패 시 릴리스가 만들어지지 않습니다.

## v120 관련 검증 범위 (정직한 고지)
- CI 에서 VC++ 2013 x64 런타임을 설치해 **실제 `msvcp120.dll` 의 `_Mtx_*` export 를 데모가 직접 호출**하는 방식으로 검증했습니다: 역전 탐지, 일관된 순서(경고 없음), 재귀 잠금(경고 없음), 실제 데드락(블록된 상태에서 경고).
- **VS2013 으로 빌드한 실제 애플리케이션에서는 실행해 보지 못했습니다.** 탐지기 시작 시점에 `msvcp120.dll` 이 이미 로드되어 있어야 하고, `Concurrency::critical_section` 직접 사용은 추적하지 않습니다.

## 실측 결과 (windows-2022, 2 vCPU, Release)
| 항목 | 원본 | 이 버전 |
|---|---|---|
| 역전락 CS/Mutex/SRW 탐지 | 크래시 또는 hang, 경고 0건 | 3종 모두 경고 1건, 거짓 양성 0건 |
| `std::mutex` 역전·데드락 (`/MD`, `/MT`, VS2013 `msvcp120.dll`) | 미지원 | 모두 탐지 |
| 실제 데드락 | 경고 없음 | 블록된 상태에서 경고 기록 |
| 락 300개 중첩 획득(라운드당) | 약 1,190~1,440 ms | 약 8~16 ms |
| CS lock+unlock (1스레드) | 측정 불가 | 약 10~14 ns → 약 95~118 ns |

세부 값은 zip 의 `test-results/` 를 참고하세요. `LOCKDETECTOR_VERBOSE=1` 이면 모든 이벤트를 기록하며 부하 시 INFO 로그가 일부 드롭될 수 있습니다(WARNING 은 예약 공간 사용).

## 한계
`WaitForMultipleObjects` 등은 후킹하지 않으며, 시작 이전에 만든 뮤텍스 핸들과 시작 이후에 로드된 STL DLL 은 추적하지 않습니다. 이 도구는 순서 역전을 찾는 휴리스틱이며 데드락 부재를 증명하지 않습니다. 진단용 빌드이므로 성능 측정에는 사용하지 마세요.
