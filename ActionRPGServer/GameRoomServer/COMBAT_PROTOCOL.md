# 서버 전투 실행과 클라이언트 연결 계약

## 구현 범위와 현재 콘텐츠

- 방 strand의 20Hz 업데이트에서 플레이어 사격·점프, 탄환, 몬스터 AI, HP·피격·사망을 처리합니다.
- 클라이언트가 보낸 위치·피해량·피격 대상은 사용하지 않습니다. 인증된 세션의 플레이어 ID로 입력을 처리합니다.
- MonsterEditor schemaVersion 1은 유지합니다. AI 정의의 스킬·동작 참조를 서버 전용 `Data/Combat.json`과 함께 검사합니다.
- Dummy는 대기 정의를 유지합니다. 녹슨 갑옷병과 수호자는 감지·추적·공격 대기·공격·회복·귀환의 최소 패턴을 정의했습니다.
  일반 RustedArmorSlash는 14FPS의 4번 프레임(4/14초)에 피해 10,
  보스 CitadelWardenSlash는 10FPS의 4번 프레임(0.4초)에 피해 20을 적용합니다.
  HP와 피해·속도 수치는 통합용 임시값이며 상세 근거는 `output/combat-integration/README.md`에 있습니다.
- 보스방에 배치된 몬스터가 모두 죽으면 한 번만 기존 RoomManager 완료 흐름으로 연결합니다.
  보스방에 몬스터가 없으면 자동 완료하지 않습니다.
- 현재 방에 살아 있는 몬스터가 있으면 모든 출구 워프를 차단합니다. 모두 처치하면 워프를 허용하며,
  처음부터 몬스터가 없는 방은 바로 이동할 수 있습니다. 방 안에서의 이동은 기존대로 유지합니다.
  몬스터는 던전 인스턴스 생성 시 한 번만 만들며, 같은 진행 중 방을 다시 방문해도 처치 상태를 유지합니다.
  새로운 던전 인스턴스를 만들면 해당 인스턴스의 몬스터를 새로 생성합니다.
- 아이템 카탈로그·드랍·인벤토리·보상 지급, 부활·전멸 종료, 장애물 경로 탐색은 이 구현에 포함되지 않습니다.
  기존 TownServer는 완료 통지를 받아 보상 대상만 기록합니다.
- 빌드·게임 실행·기능 테스트는 수행하지 않았습니다. 정적 검토만으로 실제 플레이 동작을 보장하지 않습니다.

## 서버 전투 설정

서버 시작 시 실행 파일 옆 `Data/Combat.json`을 읽습니다. 프로젝트의 Data 복사 대상에 포함됩니다.
version 1, 최대 1MB이며 모든 등록 몬스터에 정확히 한 개의 프로필이 있어야 합니다.
없는 몬스터·스킬 참조, 중복 프로필/스킬, 누락된 필드, 비정상 수치, 이동 AI에 속도 0,
UseSkill 행동에 타격 정의 누락을 거부합니다. 오류가 있으면 서버 시작을 중단합니다.
AI 실행 수치는 서버의 float 범위 안이어야 하며, 범위를 넘는 값은 몬스터 정의 로딩 때 거부합니다.

현재 파일의 플레이어 HP 100·탄환 피해 20·사격/점프 타이밍·몬스터 속도와 충돌 크기는
서버 실행 계약을 구성하기 위한 **임시 수치**입니다. 최종 밸런스나 이미지 크기 승인이 아닙니다.
몬스터 최대 HP는 기존 `.ai.json`에서 읽습니다. 움직임 속도는 각 몬스터 프로필에 귀속됩니다.

`player`는 HP·걷기/달리기 속도·몸 높이·피격 반경, `shot`은 피해량·준비/발사 간격/회수 시간,
탄환 속도/사거리/반경·총구 높이·공중 반동, `jump`는 준비 시간·초기 수직 속도·중력을 정의합니다.
`reactions`는 경직·쓰러짐·기상 시간을 정의합니다. 서버 좌표는 기존 방 데이터와 같은 월드 단위입니다.
초기 월드 JSON에 `combatRules`를 추가하여 클라이언트가 서버 타이밍을 읽을 수 있도록 했습니다.

몬스터 프로필의 `skills` 예:

```json
{ "id": "Slash", "damage": 10, "hitSeconds": 0.4, "reachHeight": 80 }
```

동일 몬스터 문서의 skills에 `Slash`가 실제로 있어야 합니다. 기존 스킬 정의의 `durationSeconds`,
`cooldownSeconds`, `minRange`, `maxRange`, `hitType`과 함께 실행합니다. 서버 effect는 단일 대상 근접 타격입니다.
스킬 시작 때 중심 거리와 쿨다운을 검사하고 대상을 고정합니다. 타격 시점에 같은 방·생존·범위·앞쪽·높이를
다시 확인하므로 대상은 피할 수 있습니다. 타격 범위는 플레이어의 피격 반경을 포함하며 스킬당 한 번 판정합니다.
`hitSeconds`는 0부터 durationSeconds까지입니다. 스킬/동작 길이는 3600초 이하, 쿨다운은 86400초 이하입니다.
다른 투사체·광역·특수 스킬 효과는 별도 구현과 계약이 필요합니다.

## AI 실행 의미

- Wait, MoveToTarget, ReturnToSpawn, UseSkill, PlayMotion과 기존 모든 조건을 처리합니다.
- 같은 방의 준비가 끝난 생존 플레이어 중 detectionRange 안의 가장 가까운 대상을 선택합니다. 동률은 작은 playerId입니다.
- 작은 priority를 먼저 검사합니다. OnUpdate는 틱당 한 번, Immediate는 상태 진입 때,
  AfterAction은 행동 완료 때 검사합니다. 동시에 가능한 연결은 priority로 결정합니다.
- 한 틱의 시간은 행동 실행에 한 번만 소비합니다. 무시간 전환은 틱당 64회까지이며 나머지는 다음 틱에서 이어갑니다.
- 정지 거리/귀환 허용 거리까지 이동하며, 기존 이동 가능/불가 다각형을 4단위 이동 단계로 검사합니다.
  축별 이동은 지원하지만 최단 경로 탐색은 없으므로 복잡한 장애물을 우회한다고 보장하지 않습니다.
- 대상이 없어 이동/스킬을 시작할 수 없으면 그 행동에서 기다립니다. TargetLost 연결로 귀환 등 다음 행동을 지정할 수 있습니다.
- 반복 motion은 완료되지 않습니다. 스킬 쿨다운은 시작 시 소비하고 피격으로 취소되어도 유지합니다.
- HP 0이면 AI를 중단합니다. 일반 지상 피격은 경직, 에어본 타격은 띄워짐→추락→쓰러짐→기상입니다.
  이미 공중인 피해자는 수직 상승을 취소하고 추락합니다. 회복한 몬스터는 initialNodeId부터 재개합니다.
- 월드 전송 후 첫 이동·행동·상태 요청을 받은 플레이어만 AI 대상이 됩니다. 상태는 방 strand에서만 변경됩니다.

## 패킷

원본은 `Tool/PacketDefine.yml`입니다. 기존 ID 1~6을 유지하고 아래를 끝에 추가했습니다.
생성 파일은 직접 수정하지 않습니다. 서버와 클라이언트 생성 파일을 각각 갱신했고,
양쪽 결과가 공유 YAML과 일치하는 것을 정적으로 확인했습니다. 클라이언트 송수신 핸들러와 상태 표시도 연결했습니다.

| ID | 패킷 | 필드 |
|---|---|---|
| 7 | DungeonActionInput | sequence:uint32, action:uint8, facingLeft:uint8 |
| 8 | DungeonActionResult | sequence:uint32, accepted:uint8, serverTick:uint64 |
| 9 | DungeonCombatStateRequest | snapshotId:uint32, offset:uint32 |
| 10 | DungeonCombatStateChunk | snapshotId:uint32, totalBytes:uint32, offset:uint32, status:uint8, retryAfterMs:uint32, payload:string |

### 행동 입력

초기 월드 수신이 끝난 후 요청합니다. action 1=사격, 2=점프; facingLeft 0=오른쪽, 1=왼쪽입니다.
sequence는 이동 sequence와 독립적이며 연결 내에서 1부터 증가합니다. 중복·역순 요청은 다시 실행하지 않습니다.
플레이어 ID·맵 ID·위치·피해량은 요청하지 않습니다. 잘못된 값과 초당 20개를 넘는 행동 요청은 무시합니다.
accepted는 요청 접수 여부이며 타격 성공을 뜻하지 않습니다. HP/탄환/피격 결과는 스냅샷을 사용합니다.

한 공격 구간은 최대 5발입니다. 공격 중 추가 사격 입력은 다음 발을 예약합니다. 준비→발사 간격→회수 시간을
서버에서 처리하며 공중 사격은 한 번의 점프에서 최대 5발, 바라보는 쪽으로 45도 아래 방향입니다.
공격 중 바라보는 방향은 고정합니다. 공격·점프·피격 중 일반 이동은 정지합니다. 공중 사격은 약한 상승·후퇴 반동을 적용합니다.
점프는 준비 후 도약합니다. 준비 중 사격 입력은 도약 후 시작합니다.
착지·피격·사망하면 해당 공중 공격을 취소합니다. 탄환은 같은 방의 살아 있는 몬스터에만 타격하고 벽·지면·사거리에서 사라집니다.
플레이어 간 피해는 없습니다. 방당 탄환은 최대 256개입니다.

### 상태 조각 수신

1. snapshotId=0, offset=0으로 새 스냅샷을 요청합니다.
2. status=0이면 받은 snapshotId를 고정하고 payload의 **바이트 수**만큼 offset을 증가시켜 다음 조각을 요청합니다.
3. 누적 바이트 수가 totalBytes가 될 때 UTF-8 JSON으로 파싱하고 버전·serverTick·mapId를 검사한 뒤 적용합니다.
   부분 조각을 따로 디코딩하지 않습니다. 조각 경계가 UTF-8 문자 중간일 수 있습니다.
4. 새 스냅샷은 최소 200ms 간격입니다. 전송 중 값은 불변이고 새 요청은 이전 스트림을 대체합니다.
5. status=1은 제한/준비 중입니다. retryAfterMs 뒤 **같은 요청**을 재시도합니다.
   첫 응답에 snapshotId가 지정됐다면 그 ID와 해당 offset으로 재시도합니다.
   status=2는 상태 없음, status=3은 ID/offset 불일치입니다. 현재 조립을 버리고 새 스냅샷을 요청합니다.

스냅샷은 최대 512KB, payload는 최대 768바이트, 연결당 전투 조각은 초당 64KB입니다.
클라이언트는 stop-and-wait로 한 요청의 응답을 처리한 뒤 다음 요청을 보내야 합니다.
요청 폭주나 중복 요청을 정상적인 폴링 방식으로 사용하지 않습니다.
serverTick은 20Hz 시뮬레이션 번호이며 매 조각의 수신 시각을 뜻하지 않습니다.
큰 방에서는 조각 수와 대역폭 제한으로 전체 상태 갱신에 더 오래 걸립니다.

JSON 최상위: `version=1`, `roomId`, `serverTick`, `mapId`, `state`, `cleared`, `players`, `monsters`, `projectiles`.
state는 WaitingForPlayers/Running/Cleared/Stopped입니다. 다른 roomId 또는 이전 serverTick의 상태는 적용하지 않습니다.

- players: playerId, x/y, hp/maxHp, height/verticalSpeed, facingLeft, reaction/reactionSeconds, shotPhase,
  shotSeconds/shotCount/airShotCount, jumpPhase/jumpSeconds, airAttack, actionSequence, moveSequence.
- monsters: instanceId/dataId, x/y, hp/maxHp, height/verticalSpeed, facingLeft, reaction/reactionSeconds,
  aiNodeId, actionType, actionStarted/actionComplete, actionSeconds, animationId.
- projectiles: id/ownerId, x/y, height, direction, heightDirection.

reaction은 None/Hit/Falling/Down/Rising/Dead, shotPhase는 None/Prepare/Fire/Recover입니다.
jumpPhase는 Grounded/Prepare/Airborne입니다. reaction/HP를 우선하여 표시하고 일반 상태에서 actionType/animationId를 사용합니다.
몬스터 image/sourceRect/pivot/fps는 클라이언트의 기존 모션 메타데이터에서 읽습니다.
Dead 개체는 스냅샷에 남고 HP 0입니다. 사망 애니메이션은 개체당 한 번 시작하고 마지막 프레임을 유지해야 합니다.
스냅샷의 개체 나열 순서는 고정하지 않으므로 항상 ID로 대응합니다.
mapId가 바뀌면 이전 방의 표시 목록을 교체합니다. 삭제된 탄환은 해당 방에서 제거합니다.
클리어 뒤에도 상태 요청은 가능하며 cleared=true입니다.

## 통합 결과와 후속 범위

- 편집기: FallenCitadel 출력·설치 자동화와 Debug·Release 실행 데이터 및 필수 자산 의존성 설치를 완료했습니다.
  새 몬스터를 추가할 때에는 Combat.json 프로필과 AI 스킬의 effect 참조도 확인해야 합니다.
- 클라이언트: ID 2·3 렌더링, X 사격·C 점프 입력/수신, 서버 상태 적용과 보간, HP·피격·사망·클리어 표시를 연결했습니다.
- 최종 소스의 빌드와 실제 입장·전투 확인은 수행하지 않았습니다. 양쪽 EXE를 함께 갱신해야 합니다.
- 설치와 정적 확인 결과는 `output/combat-integration/INTEGRATION_REPORT.md`에 정리했습니다.
- 몬스터/스킬: 최소 패턴·타격 event는 연결했으며 최종 밸런스와 특수 패턴은 별도 합의가 필요합니다.
- 사용자 합의가 필요한 추가 기능: 보상/드랍 정책·데이터, 전멸 종료·부활, 특수 보스 패턴.
