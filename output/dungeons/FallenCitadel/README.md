# 몰락한 성채 작업 파일·서버 데이터

Data ID **5**, 던전 ID **FallenCitadel**, 최대 4명입니다. 방 6개, 연결 5개, 전환 영역 10개이며 ID 2 병사 15마리와 ThroneRoom의 ID 3 보스 1마리를 배치했습니다.

## 현재 보관 파일

- `FallenCitadel.dungeon-project.json`: 편집기 schemaVersion 3 작업 파일. 배경을 base64로 포함하므로 기존 DungeonEditor에서 다시 편집할 수 있습니다.
- `prepared-package/Dungeon.json`, `prepared-package/Maps/*.json`: 최초 준비 단계의 서버 JSON. 실제 설치 위치는 서버 `ActionRPGServer/GameRoomServer/Data/Dungeons/FallenCitadel` 및 두 GameRoomServer 실행 Data 폴더입니다.
- `STATIC_CHECKS.json`: 최초 준비 단계의 정적 검사 기록. 임시 AI·미설치 등의 당시 제한은 최종 상태와 다릅니다. 현재 통합 상태는 `output/combat-integration/INTEGRATION_REPORT.md`를 참조하세요.

## 제거·보존한 제작 자료

서버에서 읽지 않는 prepared-package의 PNG/SVG 및 Assets, layout-preview.png, FallenCitadel.prepared.zip, tools/prepare_dungeon.cjs·render_layout.py·update_editor.py를 제거했습니다. 재실행하지 않고 최초 제작 기록으로 보존합니다.

백업: `C:/Users/KimHyeongJin/Documents/Codex/2026-10-02/actionrpg-editors/outputs/ServerOutputCleanup-20261003/removed-files.zip`

클라이언트가 사용하는 방 배경·게이트·발판·몬스터 이미지는 클라이언트 원본/실행 Assets에 유지했습니다. 미니맵 PNG/SVG와 일반·보스 아이콘은 공용 ZIP 자료이며 서버에는 설치하지 않습니다.

## 재출력

기존 DungeonEditor UI 또는 `DungeonEditor/tools/dungeon-package.cjs`를 사용합니다. 작업 파일을 입력하여 검사·공용 ZIP·설치 계획을 생성하며, 새 설치에서 서버 던전 파일은 Dungeon.json과 Maps/*.json으로 제한합니다. 공용 ZIP의 출력 위치는 클라이언트 또는 별도 산출물 폴더로 지정하세요.

게임 빌드·실행·입장·전투 테스트는 수행하지 않았습니다.
