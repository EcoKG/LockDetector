# LockDetector v1.0.0 (x64)

원본 샘플을 Windows CI에서 실측한 뒤 결함을 수정한 버전입니다.

## 첨부 파일
- `LockDetector-v1.0.0-x64-bin.zip` : `LockDetector.dll`, `LockViewer.exe`, `InversionDemo.exe`, PDB, 테스트 결과(`test-results/`), README, Detours 라이선스
- `LockDetector-v1.0.0-src.zip` : 소스 전체 (CMake 빌드, 예제, CI 스크립트 포함)

바이너리는 GitHub Actions `windows-2022` 에서 **VS2022(v143) Release x64** 로 빌드했습니다. VS2013(v120) 빌드는 검증하지 못했습니다.

## 원본 대비 수정 사항
1. 원본은 컴파일되지 않았습니다 (헤더 include/선언 누락).
2. 탐지기를 로드하면 스레드를 만드는 모든 시나리오가 크래시 또는 hang 했습니다 (신규 스레드가 ntdll 내부 락 후크에서 트래커에 중첩 재진입 후 자기 교착). 호출 모듈 필터와 무할당·무블로킹 후크 경로로 해결했습니다.
3. 실제 데드락이 로그에 남지 않던 문제를 획득 전(pre-acquire) 순서 검사로 해결했습니다.
4. 락 그래프 탐색을 인접 리스트 + 반복 DFS로 바꿨습니다.
5. 로그 경로 불일치, 뷰어 텍스트 한도, Detours 스레드 갱신 범위를 수정했습니다.

## 실측 결과 (windows-2022, 2 vCPU, Release)
| 항목 | 원본 | v1.0.0 |
|---|---|---|
| 역전락 CS/Mutex/SRW 탐지 | 크래시 또는 hang, 경고 0건 | 3종 모두 경고 1건, 거짓 양성 0건 |
| 실제 데드락 | 경고 없음 | 블록된 상태에서 경고 기록 |
| 락 300개 중첩 획득(라운드당) | 약 1,190~1,440 ms | 약 8~14 ms |
| CS lock+unlock (1스레드) | 측정 불가 | 10 ns → 약 95 ns |
| SRW lock+unlock (1스레드) | 측정 불가 | 10 ns → 약 53 ns |

`LOCKDETECTOR_VERBOSE=1` 이면 이벤트를 모두 기록하며(CS 1스레드 약 870 ns), 부하 시 INFO 로그가 일부 드롭될 수 있습니다. WARNING 은 예약 공간을 사용합니다.

자세한 값은 zip 안의 `test-results/` 와 `README.md` 를 참고하세요.

## 한계
`std::mutex`, `WaitForMultipleObjects` 등은 후킹하지 않으며, 시작 이전에 만든 뮤텍스 핸들은 추적하지 않습니다. 이 도구는 순서 역전을 찾는 휴리스틱이며 데드락 부재를 증명하지 않습니다. 진단용 빌드이므로 성능 측정에는 사용하지 마세요.
