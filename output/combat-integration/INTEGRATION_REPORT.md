# FallenCitadel 전투 통합 결과

## 판정과 확인 범위

서버·클라이언트·몬스터 소스와 던전 데이터를 통합했습니다. 사용자가 지정한 **정적 확인만** 수행했으며 게임 빌드·서버/클라이언트 실행·던전 입장/전투 테스트는 하지 않았습니다. 기존 EXE에는 이번 C++ 소스 변경을 빌드하지 않았으므로 실제 플레이 가능 여부는 확인되지 않았습니다.

## 담당별 결과

| 담당 | 반영 내용 | 상태 |
|---|---|---|
| 서버 | 사격·점프·탄환·몬스터 AI·HP·피격·사망·보스 처치 완료, 입력/스냅샷 패킷 | 구현 및 정적 확인 완료 |
| 몬스터 | ID 2/3 감지→추적→공격 대기→공격→회복→귀환, 기존 attack 모션의 타격 시점 | 원본 AI와 Combat.json 통합 완료 |
| 편집기 | 던전 원본/이미지 및 Debug·Release 데이터, 클라이언트 필수 자산 의존성 설치 | 설치·백업·정적 해시 확인 완료 |
| 클라이언트 | X 사격·C 점프, 상태 조각 수신, 서버 탄환·HP·피격·사망·클리어 표시 | 검토한 20개 파일을 원본에 적용, 정적 대조 완료 |

## 던전과 정적 검증

- 던전 Data ID 5, FallenCitadel, 최대 4명.
- 방 6개, 연결 5개, 유효한 MapTransfer 전환 영역 10개.
- ID 2 병사 15마리, ID 3 보스 1마리. 보스는 ThroneRoom에만 배치.
- 일반 공격은 피해 10, 4/14초에 타격하며 8/14초간 실행. 보스는 피해 20, 0.4초에 타격하며 0.8초간 실행.
- 피해·속도·충돌 크기·HP는 최소 통합을 위한 임시 수치입니다.
- 던전 설치 102개와 추가 클라이언트 자산 의존 파일 48개, 총 150개 설치 파일의 해시를 확인했습니다. 별도 Combat.json 설치는 2개입니다.
- 모션 PNG 10개의 크기/프레임 영역, 스킬/FPS/타격 시점, 양쪽 프로젝트 XML의 전투 소스 참조를 확인했습니다.
- 양쪽 생성 패킷은 원본 YAML과 일치하며 기존 패킷 ID 1~6을 유지하고 7~10을 추가했습니다.
- 클라이언트 원본 20개 파일이 검토한 작업본과 일치합니다. 원본 Assets/Data/assets.ini와 monsters.json 및 활성 카탈로그 참조 파일은 Debug·Release에 동일한 해시로 설치됐습니다.
- GameRoomServer Debug·Release의 Combat.json 및 몬스터 카탈로그/정의 해시는 최종 원본과 일치합니다.
- [정적 검사 기록](C:/Users/KimHyeongJin/source/repos/ActionRPGServer/output/combat-integration/STATIC_INTEGRATION_CHECKS.json).

## 설치 경로와 기록

서버 원본 던전: `ActionRPGServer/GameRoomServer/Data/Dungeons/FallenCitadel`.
GameRoomServer 실행 데이터: `artifacts/bin/x64/Debug/Data`, `artifacts/bin/x64/Release/Data`.
TownServer 실행 데이터: `ActionRPGServer/x64/Debug/Data`, `ActionRPGServer/x64/Release/Data`.
클라이언트 실행 데이터: `C:/Users/KimHyeongJin/source/repos/ActionRPGClient/ActionRPGClient/artifacts/bin/x64/{Debug,Release}/Assets`.

던전 설치는 `.dungeon-installs/2dd8a44e-f74f-4916-9556-de5b88f32f5f/journal.json` 및
`.dungeon-installs/e6a0b4e1-1e3a-4713-8d8c-8192fa7659e5/journal.json`에 기록됐습니다.
해당 폴더의 `.before`는 기존 파일 백업, `.after`는 설치 파일 스냅샷입니다.
Combat.json 신규 설치 2개는 `.dungeon-installs/combat-config-final/journal.json`의 별도 CombatConfigInstall 기록이며 DungeonEditor restore 대상이 아닙니다.

추가 클라이언트 의존성 설치 기록은 `.dungeon-installs/04ce4edf-2e57-42fe-9056-ff9b8698fa9b/journal.json`(Debug 22개)와
`.dungeon-installs/2281a21a-7a28-44ab-9779-0e3ae3edcd2a/journal.json`(Release 26개)에 있습니다. 형식은 RuntimeAssetDependencyInstall이며 동일 폴더에 교체 전 백업이 있습니다.

[편집기 설치 상세](C:/Users/KimHyeongJin/Documents/Codex/2026-10-02/actionrpg-editors/outputs/FallenCitadel-INSTALL_SUMMARY.md).

[클라이언트 변경·정적 확인 상세](C:/Users/KimHyeongJin/Documents/Codex/2026-10-02/actionrpg-client/outputs/client-combat-integration.md).

## 사용 전 남은 확인

최종 소스의 서버·클라이언트 빌드와 실제 입장→일반 전투→보스 처치→클리어 확인은 이번 작업에서 수행하지 않습니다. 프로토콜이 추가됐으므로 양쪽 실행 파일을 함께 갱신해야 합니다.

보상 실제 지급·드랍·부활·전멸 종료·일반방 게이트 잠금·장애물 경로 탐색·특수 보스 패턴은 이번 최소 전투 연결 범위에 포함하지 않습니다.
