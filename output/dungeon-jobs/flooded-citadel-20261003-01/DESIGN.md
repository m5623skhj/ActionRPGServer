# 침수된 성채 설계

작업 ID: flooded-citadel-20261003-01 / 요청 ID: flooded-citadel-20261003-01-02-design / revision: 1 / 요구사항 revision: 1.
규칙: C:\Users\KimHyeongJin\source\repos\ActionRPGServer\docs\workflows\DUNGEON_CREATION.md v1.0.1.
작성일: 2026-10-03. 이번 결과는 설계·그래픽 제작 명세이며 이미지 생성·최종 작업 파일·패키지·설치를 완료한 결과가 아닙니다.

## 컨셉과 진행

고인 물이 성채 외곽과 무너진 안뜰을 차지했다. 플레이어는 물보다 높게 남은 마른 석재 제방과 넓은 석조 발판으로 이동하며, 마지막 왕좌를 지키는 수호자를 상대한다. 회갈색 석재, 채도가 낮은 청록색 물, 낡은 남색 깃발과 차가운 외광을 사용한다. 물은 이동 영역 밖의 시각적 배경이며 수영·감속·피해·물결 시스템을 추가하지 않는다.

던전 ID **FloodedCitadel**, 숫자 Data ID **6**, 최대 **4명**. 일반 방 5개와 보스방 1개, 양방향 연결 5개다. 주 동선은 성문 → 제방길 → 중앙 안뜰 → 성벽 회랑 → 왕좌이며, 중앙 안뜰 남쪽 보급대는 선택 분기다.

```text
Floodgate — BrokenCauseway — ReservoirCourt — CollapsedGallery — DrownedThrone
                                  |
                             SupplyTerrace
```

| 방 ID·이름 | 종류 | 격자 (X,Y) | 공유 배경 | 초안 월드 크기 | 배치 |
|---|---|---|---|---|---|
| Floodgate · 물막이 성문 | normal | (0,0) | causeway | 1280×720 | 없음 |
| BrokenCauseway · 무너진 제방길 | normal | (1,0) | causeway | 1280×720 | ID 2 갑옷병 3 |
| ReservoirCourt · 침수된 중앙 안뜰 | normal | (2,0) | courtyard | 1280×720 | ID 2 갑옷병 5 |
| SupplyTerrace · 버려진 보급대 | normal | (2,1) | courtyard | 1280×720 | ID 2 갑옷병 3 |
| CollapsedGallery · 무너진 성벽 회랑 | normal | (3,0) | courtyard | 1280×720 | ID 2 갑옷병 4 |
| DrownedThrone · 침수된 왕좌 | boss | (4,0) | throne | 1600×900 | ID 3 수호자 1 |

총 갑옷병 **15마리**, 수호자 **1마리**. 보스는 DrownedThrone에만 있고 해당 방에는 일반 몬스터를 넣지 않는다. Dummy는 쓰지 않는다. 현재 방의 몬스터가 남아 있으면 서버가 워프를 잠그므로, 선택 분기에 들어간 경우 그 방의 3마리를 처치한 후 돌아온다. 재방문 시 재생성하지 않는 기존 동작을 사용한다. 보스 처치가 던전 완료를 일으키므로 분기를 필수 진행 조건으로 만들지 않는다. 상자·스위치·보상·신규 스킬은 추가하지 않는다.

## 연결·경계 계약

좌표 원점은 방 이미지의 왼쪽 위다. 방마다 sectorWidth/sectorHeight는 기존 기본값 480/480을 사용한다. 속도는 Combat.json과 캐릭터 정보에 유지하고 던전 속성에 넣지 않는다.

| 연결 ID | 양방향 방 연결 | 출발/반대 방향 |
|---|---|---|
| Floodgate_BrokenCauseway | Floodgate ↔ BrokenCauseway | east / west |
| BrokenCauseway_ReservoirCourt | BrokenCauseway ↔ ReservoirCourt | east / west |
| ReservoirCourt_CollapsedGallery | ReservoirCourt ↔ CollapsedGallery | east / west |
| CollapsedGallery_DrownedThrone | CollapsedGallery ↔ DrownedThrone | east / west |
| ReservoirCourt_SupplyTerrace | ReservoirCourt ↔ SupplyTerrace | south / north |

일반 방의 경계 구간은 좌우 0..128 / 1152..1280, 위아래 0..128 / 592..720이다. 보스방 좌우는 0..128 / 1472..1600이며 위아래는 0..128 / 772..900이다. 이는 모델의 min(128, 해당 축 크기×0.2) 규칙이다. 모든 워프 영역 전체와 반대편 도착점이 해당 구간 안에 있다.
좌우 일반 출구는 중심 Y=480, 보스방 서쪽은 Y=570이다. 안뜰 남쪽 ↔ 보급대 북쪽만 상하 연결을 사용한다. 도착점은 워프 밖이고 가로 반경 32·세로 반경 18의 발 영역 전체가 이동 다각형 안에 있다.

## 이동 영역·물·배경 재사용

모든 이동 영역은 하나의 볼록 다각형이다. 내부 기둥·갈림길 장애물·가느다란 다리·섬을 두지 않아 기존 직접 이동/축별 슬라이딩 AI에 새 경로 탐색 기능을 요구하지 않는다. 배경 공유는 그래픽 재사용이며 몬스터 개체·입장점·워프·상태 공유를 뜻하지 않는다.

### causeway (1280×720)

- 공유 방: Floodgate, BrokenCauseway.
- 이동 다각형: (40,320) → (1240,320) → (1240,650) → (40,650).
- 진입 불가 영역: (0,0) → (1280,0) → (1280,300) → (0,300); (0,670) → (1280,670) → (1280,720) → (0,720).
- 그림 속 물 영역: (40,260) → (460,260) → (460,300) → (40,300); (820,260) → (1240,260) → (1240,300) → (820,300); (0,670) → (1280,670) → (1280,720) → (0,720).
- 물·잔해·벽·장식은 이동 다각형 밖에만 둔다. 횃불·깃발은 외곽 배경으로 사용하고 게이트는 그림에 합성하지 않는다.

### courtyard (1280×720)

- 공유 방: ReservoirCourt, SupplyTerrace, CollapsedGallery.
- 이동 다각형: (480,30) → (800,30) → (1240,360) → (1240,700) → (40,700) → (40,360).
- 진입 불가 영역: (40,40) → (440,40) → (40,340); (840,40) → (1240,40) → (1240,340); (0,702) → (1280,702) → (1280,720) → (0,720).
- 그림 속 물 영역: (40,40) → (440,40) → (40,340); (840,40) → (1240,40) → (1240,340); (0,702) → (1280,702) → (1280,720) → (0,720).
- 물·잔해·벽·장식은 이동 다각형 밖에만 둔다. 횃불·깃발은 외곽 배경으로 사용하고 게이트는 그림에 합성하지 않는다.

### throne (1600×900)

- 공유 방: DrownedThrone.
- 이동 다각형: (40,340) → (1560,340) → (1560,820) → (40,820).
- 진입 불가 영역: (0,0) → (1600,0) → (1600,320) → (0,320); (0,840) → (1600,840) → (1600,900) → (0,900).
- 그림 속 물 영역: (40,260) → (430,260) → (430,320) → (40,320); (1170,260) → (1560,260) → (1560,320) → (1170,320); (0,840) → (1600,840) → (1600,900) → (0,900).
- 물·잔해·벽·장식은 이동 다각형 밖에만 둔다. 횃불·깃발은 외곽 배경으로 사용하고 게이트는 그림에 합성하지 않는다.

안뜰의 북쪽 진입로는 폭 320에서 전투 바닥으로 넓어지는 볼록 육각형이다. 물은 좌우 상단 삼각형과 남쪽 경계 바깥에 배치한다. 공유 안뜰 배경을 사용하는 회랑·보급대에는 활성 게이트만 별도 배치하며 미사용 출입구를 배경에 표시하지 않는다.

## 방별 상세 배치

### Floodgate · 물막이 성문

- 기본 도착점: `Entry_east`.
- 몬스터: 없음.
- 입장 슬롯: Party1 (240,420); Party2 (340,420); Party3 (240,540); Party4 (340,540).
- 도착점: Entry_east (1168,480).

| 출구 ID | 방향·연결 대상 | 워프 사각형 (좌상단 → 우하단) | 도착 참조 | 표시 |
|---|---|---|---|---|
| Gate_BrokenCauseway | east → BrokenCauseway | (1200,420) → (1240,540) | BrokenCauseway/Entry_west | gate, 중심 (1220,480), 64×112 |

### BrokenCauseway · 무너진 제방길

- 기본 도착점: `Entry_west`.
- 몬스터: Soldier1 / Data ID 2 / 발 (480,420) / 왼쪽 보기; Soldier2 / Data ID 2 / 발 (750,540) / 왼쪽 보기; Soldier3 / Data ID 2 / 발 (1000,430) / 왼쪽 보기.
- 입장 슬롯: 최초 입장 방이 아니므로 없음.
- 도착점: Entry_west (112,480); Entry_east (1168,480).

| 출구 ID | 방향·연결 대상 | 워프 사각형 (좌상단 → 우하단) | 도착 참조 | 표시 |
|---|---|---|---|---|
| Gate_Floodgate | west → Floodgate | (40,420) → (80,540) | Floodgate/Entry_east | gate, 중심 (60,480), 64×112 |
| Gate_ReservoirCourt | east → ReservoirCourt | (1200,420) → (1240,540) | ReservoirCourt/Entry_west | gate, 중심 (1220,480), 64×112 |

### ReservoirCourt · 침수된 중앙 안뜰

- 기본 도착점: `Entry_west`.
- 몬스터: Soldier1 / Data ID 2 / 발 (360,450) / 왼쪽 보기; Soldier2 / Data ID 2 / 발 (500,590) / 왼쪽 보기; Soldier3 / Data ID 2 / 발 (750,430) / 왼쪽 보기; Soldier4 / Data ID 2 / 발 (920,590) / 왼쪽 보기; Soldier5 / Data ID 2 / 발 (1080,470) / 왼쪽 보기.
- 입장 슬롯: 최초 입장 방이 아니므로 없음.
- 도착점: Entry_west (112,480); Entry_east (1168,480); Entry_south (640,620).

| 출구 ID | 방향·연결 대상 | 워프 사각형 (좌상단 → 우하단) | 도착 참조 | 표시 |
|---|---|---|---|---|
| Gate_BrokenCauseway | west → BrokenCauseway | (40,420) → (80,540) | BrokenCauseway/Entry_east | gate, 중심 (60,480), 64×112 |
| Gate_CollapsedGallery | east → CollapsedGallery | (1200,420) → (1240,540) | CollapsedGallery/Entry_west | gate, 중심 (1220,480), 64×112 |
| Gate_SupplyTerrace | south → SupplyTerrace | (600,660) → (680,700) | SupplyTerrace/Entry_north | pad, 중심 (640,680), 80×48 |

### SupplyTerrace · 버려진 보급대

- 기본 도착점: `Entry_north`.
- 몬스터: Soldier1 / Data ID 2 / 발 (490,460) / 왼쪽 보기; Soldier2 / Data ID 2 / 발 (660,550) / 왼쪽 보기; Soldier3 / Data ID 2 / 발 (840,460) / 왼쪽 보기.
- 입장 슬롯: 최초 입장 방이 아니므로 없음.
- 도착점: Entry_north (640,112).

| 출구 ID | 방향·연결 대상 | 워프 사각형 (좌상단 → 우하단) | 도착 참조 | 표시 |
|---|---|---|---|---|
| Gate_ReservoirCourt | north → ReservoirCourt | (600,40) → (680,80) | ReservoirCourt/Entry_south | pad, 중심 (640,60), 80×48 |

### CollapsedGallery · 무너진 성벽 회랑

- 기본 도착점: `Entry_west`.
- 몬스터: Soldier1 / Data ID 2 / 발 (360,460) / 왼쪽 보기; Soldier2 / Data ID 2 / 발 (560,590) / 왼쪽 보기; Soldier3 / Data ID 2 / 발 (820,440) / 왼쪽 보기; Soldier4 / Data ID 2 / 발 (1040,580) / 왼쪽 보기.
- 입장 슬롯: 최초 입장 방이 아니므로 없음.
- 도착점: Entry_west (112,480); Entry_east (1168,480).

| 출구 ID | 방향·연결 대상 | 워프 사각형 (좌상단 → 우하단) | 도착 참조 | 표시 |
|---|---|---|---|---|
| Gate_ReservoirCourt | west → ReservoirCourt | (40,420) → (80,540) | ReservoirCourt/Entry_east | gate, 중심 (60,480), 64×112 |
| Gate_DrownedThrone | east → DrownedThrone | (1200,420) → (1240,540) | DrownedThrone/Entry_west | gate, 중심 (1220,480), 64×112 |

### DrownedThrone · 침수된 왕좌

- 기본 도착점: `Entry_west`.
- 몬스터: Warden1 / Data ID 3 / 발 (1080,600) / 왼쪽 보기.
- 입장 슬롯: 최초 입장 방이 아니므로 없음.
- 도착점: Entry_west (112,570).

| 출구 ID | 방향·연결 대상 | 워프 사각형 (좌상단 → 우하단) | 도착 참조 | 표시 |
|---|---|---|---|---|
| Gate_CollapsedGallery | west → CollapsedGallery | (40,510) → (80,630) | CollapsedGallery/Entry_east | gate, 중심 (60,570), 64×112 |

## 현재 지원 범위와 선행 요청

최신 서버 MonsterCatalog 및 정의, Combat.json, GameRoom/GameRoomCombat 소스와 클라이언트 monsters.json/Monster.cpp를 대조했다. ID 2·3의 감지→추적→대기→공격→회복→귀환 AI와 공격 판정, 클라이언트 모션 표시 소스가 존재한다. 예전 FallenCitadel 제작 초안의 대기 전용 AI·미연동 설명을 현재 상태로 간주하지 않는다. 두 정의의 최대 HP는 현재 모두 100이며, 일반 피해 10·보스 피해 20 등 현 수치는 임시 통합 수치를 그대로 재사용한다. 신규 밸런스를 확정하거나 EXE 동작을 검증한 것은 아니다.

편집기 팔레트·검사에는 ID 1·2·3이 등록되어 있다. 필요한 신규 서버/클라이언트/편집기 기능 개발은 **없음**이다. 피격은 현재 Combat.json의 몸 높이/반경과 기존 상태 판정을 재사용하며 신규 피격 영역·스킬 타입을 도입하지 않는다. 서버 판정 상태는 기존 방 strand에서 갱신하므로 새 공유 가변 상태를 만들지 않는다.

원본 TownServer 카탈로그에는 **Data ID 5만** 있으며 6은 미등록이다. 서버 원본 던전 manifest도 FallenCitadel/5만 확인했고, 현재 원본에서 6 및 FloodedCitadel의 충돌은 없다. 카탈로그 등록 권한은 이번 설계에 포함되지 않는다. 총괄이 서버 담당에 원본 west_coast_dungeons 그룹의 ID 6 등록을 별도 순차 요청해야 한다. **dungeon-package.cjs Dependencies는 설치 없이 출력만 해도 원본 ID 등록을 요구하므로 패키지 제작 전 반드시 필요하다.** 실행 카탈로그 설치는 요청하지 않는다.

## 제작·출력 계획 및 치수 차이 처리

그래픽 요청 3개는 GRAPHICS_REQUESTS.json에 저장했다. 총괄만 causeway → courtyard → throne 순으로 하나씩 전달하며 담당이 직접 메시지를 보내지 않는다. 출력 경로는 클라이언트 DungeonEditor/output/FloodedCitadel/graphics이며 서버에는 이미지 파일을 생성하지 않는다. 각 요청은 실제 PNG와 측정 치수·원본·수정 기록을 반환해야 한다. 현재 요청 크기의 정확한 도구 출력 가능 여부는 **미확인**이다.

요청 크기는 설계 기준이지 실제 생성 치수가 아니다. 원본 비율 차이가 1% 이내면 파일을 재샘플링하지 않고 기존 images 표시 사각형에 배치하되 scaleX=요청폭/실제폭, scaleY=요청높이/실제높이를 기록한다. 차이가 크면 일률적인 늘이기 대신 균일 배율·월드 크기·바닥/장식 좌표를 재조정하고, 128px 상한이 있는 경계·도착점·워프를 다시 계산한다. 이 변경은 총괄 확인 후 revision을 갱신하고 기존 모델로 재검사한다.

09단계에서 기존 DungeonEditor의 createDocument/createRoom/parse/validate와 UI를 사용해 실제 배경을 자산 3개로 포함한 schemaVersion 3 작업 파일을 저장한다. 10단계에서 기존 package.js의 DungeonPackage.Build 및 tools/dungeon-package.cjs를 사용하며 별도 런타임 JSON 작성기나 검사기를 복제하지 않는다. ZIP·INSTALL_PLAN·REPORT는 클라이언트 또는 별도 산출물 폴더에 출력하고 --install을 사용하지 않는다. 설치는 별도 승인 범위다.

## 이번 정적 확인과 남은 일

- 기존 모델로 설계 다각형·연결·도착점·발 영역·슬롯·몬스터 배치를 확인했다. 배경이 아직 없으므로 모델의 전체 검사에는 방별 배경 누락 오류 6개가 예상대로 남으며, 그 외 오류/경고는 0개다. **최종 편집기 정적 검사 통과로 보고하지 않는다.**
- 몬스터 AI는 기존 MonsterEditor 검사기, 모션 메타데이터는 기존 CharacterEditor 검사기를 재사용해 정적으로 확인했다.
- 기존 파일 경로와 해시는 DESIGN_RESULT.json에 기록했다. 생성하지 않은 그래픽 출력 경로는 예정 경로로 표시했다.
- 남은 일: ID 6 원본 등록, 총괄의 몬스터/스킬 재사용 확인, 그래픽 3종 생성·치수 대조, 실제 편집 파일 구성·공용 패키지 및 설치 계획 생성.
- 이번에는 이미지 생성, 다른 세션 메시지, 공용 코드/카탈로그 수정, job.json 갱신, 패키지 생성/설치, 빌드·게임 실행·기능 테스트를 하지 않았다. 전체 경로 탐색·플레이 난이도·게임 접지 확인은 미실시다.
