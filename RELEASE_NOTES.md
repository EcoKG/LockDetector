# LockDetector {{TAG}} (x64)

`std::mutex` 지원을 추가한 버전입니다. v1.0.0 에서는 동적 CRT(`/MD`)로 빌드한 앱의 `std::mutex` 락 순서 역전을 놓쳤습니다.

## 첨부 파일
- `LockDetector-{{TAG}}-x64-bin.zip` : `LockDetector.dll`, `LockViewer.exe`, `InversionDemo.exe`, `InversionDemoMT.exe`(정적 CRT 예제), PDB, 테스트 결과(`test-results/`), README, Detours 라이선스
- `LockDetector-{{TAG}}-src.zip` : 소스 전체 (CMake 빌드, 예제, CI 스크립트 포함)

바이너리는 GitHub Actions `windows-2022` 에서 **VS2022(v143) Release x64** 로 빌드했습니다. VS2013(v120) 빌드는 검증하지 못했습니다.

## v1.0.0 대비 변경
- `msvcp140.dll` 의 `_Mtx_lock / _trylock / _timedlock / _unlock / _destroy_in_situ` 를 후킹해 `/MD` 빌드의 `std::mutex`, `std::recursive_mutex` 를 추적합니다. 시작 시점에 `msvcp140.dll` 이 이미 로드되어 있어야 하며, 후킹된 개수가 로그에 남습니다(`5/5`).
- `/MT`(정적 CRT) 는 이미 내부 SRWLOCK 호출로 추적됩니다(경고 주소는 객체 주소 +0x10).
- `std::mutex` 검증 시나리오 추가: 역전, 일관된 순서, `recursive_mutex` 재귀, 실제 데드락 — `/MD`·`/MT` 모두에서 CI 통과.
- 검사가 하나라도 실패하면 릴리스가 만들어지지 않도록 CI 를 변경했습니다.

## 실측 결과 (windows-2022, 2 vCPU, Release)
| 항목 | 원본 | 이 버전 |
|---|---|---|
| 역전락 CS/Mutex/SRW 탐지 | 크래시 또는 hang, 경고 0건 | 3종 모두 경고 1건, 거짓 양성 0건 |
| `std::mutex` 역전·데드락 (`/MD`,`/MT`) | 미지원 | 둘 다 탐지 |
| 실제 데드락 | 경고 없음 | 블록된 상태에서 경고 기록 |
| 락 300개 중첩 획득(라운드당) | 약 1,190~1,440 ms | 약 8~16 ms |
| CS lock+unlock (1스레드) | 측정 불가 | 약 10~14 ns → 약 95~118 ns |
| SRW lock+unlock (1스레드) | 측정 불가 | 약 10 ns → 약 53 ns |

세부 값은 zip 의 `test-results/` 를 참고하세요. `LOCKDETECTOR_VERBOSE=1` 이면 모든 이벤트를 기록하며 부하 시 INFO 로그가 일부 드롭될 수 있습니다(WARNING 은 예약 공간 사용).

## 한계
`WaitForMultipleObjects` 등은 후킹하지 않으며, 시작 이전에 만든 뮤텍스 핸들과 시작 이후에 로드된 `msvcp140.dll` 은 추적하지 않습니다. 이 도구는 순서 역전을 찾는 휴리스틱이며 데드락 부재를 증명하지 않습니다. 진단용 빌드이므로 성능 측정에는 사용하지 마세요.
