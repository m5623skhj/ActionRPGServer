# 이동 보간 서버 계약 — movement-smoothing-20261003

상태: 서버 30Hz/상태 전송 15Hz 변경과 클라이언트 시간 처리 수정의 정적 통합 검토 완료.
양쪽 빌드·실행 검증은 수행하지 않습니다.
기존 전투 계약: `../../ActionRPGServer/GameRoomServer/COMBAT_PROTOCOL.md`.
원본 패킷 스키마는 `Tool/PacketDefine.yml`이며 기존 ID 1~10은 변경하지 않습니다.
클라이언트 생성 파일은 클라이언트 담당이 동일 YAML에서 생성합니다.

## 연결과 패킷

모든 바깥 필드는 YAML 순서대로 기존 NetBuffer 직렬화를 사용합니다.

| ID | 패킷 | 필드 순서 |
|---|---|---|
| 11 | DungeonRealtimeRequest, reliable | version:u16, enabled:u8, challenge:u64 |
| 12 | DungeonRealtimeResult, reliable | version:u16, accepted:u8, challenge:u64, roomId:u64, dungeonId:u32, tickIntervalMs:u16, snapshotIntervalMs:u16 |
| 13 | DungeonRealtimeChunk, unreliable | version:u16, challenge:u64, roomId:u64, dungeonId:u32, mapEpoch:u32, snapshotSequence:u64, serverTick:u64, serverTimeMs:u64, mapId:string, totalBytes:u32, offset:u32, state:u8, payload:string |

초기 월드 수신 완료 후 version=1, enabled=1, 현재 DungeonChallenge 값으로 구독합니다.
accepted=1은 구독 접수이며 첫 조각이 reliable 결과보다 먼저 도착할 수도 있습니다.
enabled=0은 구독 해제입니다. 잘못된 challenge·준비되지 않은 연결은 무시합니다.
구독 요청은 연결당 최대 1초에 한 번 처리합니다. 재구독 때도 완료한 sequence/epoch의 하한은 유지합니다.
지원하지 않는 version 또는 enabled 값에는 accepted=0을 반환합니다.
기존 클라이언트에는 요청하지 않은 새 패킷을 보내지 않습니다.
이동·행동 입력과 reliable 결과, 초기 월드와 기존 전체 JSON 복구 경로는 유지합니다.

## 시간·범위·수신 규칙

- 서버 시뮬레이션 목표 30Hz, 전달 목표 15Hz(약 33.333ms/66.667ms). 입장 구독 시 즉시 한 프레임을 보냅니다.
- 구독 결과는 정수 밀리초 반올림값 tickIntervalMs=33, snapshotIntervalMs=67을 보냅니다.
  이 값은 안내값이며 33ms를 정확한 시뮬레이션 dt로 사용하지 않습니다.
- 초기 월드 combatRules 및 reliable 전투 JSON에 tickRate=30, snapshotRate=15,
  tickIntervalSeconds=1/30을 제공합니다. reliable 전투 JSON에도 아래의 serverTimeMs를 제공합니다.
- serverTick은 시뮬레이션 틱이고 행동 타이머는 틱당 1/30초입니다. 이동·탄환·AI·점프·스킬 지속시간은 초 단위를 유지합니다.
- tickTimer는 이전 목표 시각에 다음 간격을 더해 예약하므로 처리 시간만큼 주기가 계속 늘어나는 것을 방지합니다.
  과부하로 다음 시각까지 늦어진 경우 밀린 틱을 무제한 몰아서 실행하지 않고 다음 예약 시각부터 진행합니다.
- serverTimeMs는 캡처 시 서버 steady_clock의 밀리초입니다. Unix 시각이 아니며 클라이언트 시계와 직접 빼지 않습니다.
  도착 시각과의 오프셋을 추정합니다. 기존 클라이언트의 125ms 보간 버퍼는 유실 여유를 검토하며 유지할 수 있습니다.
  틱 간격과 도착 간격을 혼동하지 않습니다.
- snapshotSequence는 방 strand에서 캡처할 때마다 증가하는 u64입니다. 같은 serverTick에 서로 다른 캡처가 가능합니다.
  serverTimeMs가 같은 최신 프레임은 해당 시각의 보간 표본을 교체하고, 상태·명단 변경은 적용합니다.
- challenge가 연결 epoch, roomId가 던전 실행 인스턴스입니다. dungeonId는 콘텐츠 ID입니다.
- mapEpoch는 플레이어별로 1부터 시작해 워프마다 증가합니다. 같은 mapId로 재방문해도 epoch가 바뀝니다.
- state: 0=WaitingForPlayers, 1=Running, 2=Cleared(클리어 요청 포함), 3=Stopped.
- 프레임은 요청자와 같은 맵의 준비된 참여 플레이어, 해당 맵의 모든 몬스터(HP 0 포함), 현재 탄환을 담습니다.
  전체 명단이므로 **완성한 최신 프레임**에서 사라진 ID만 제거합니다. 단편 수신만으로 제거하지 않습니다.
- 조각 payload 최대 768바이트, 전체 최대 48KiB, offset은 768의 배수입니다.
  프레임 키는 `(challenge, roomId, mapEpoch, snapshotSequence)`입니다.
  동일 키의 mapId·tick·time·state·totalBytes가 일치하는지 검사하고 범위 밖 offset·길이를 거부합니다.
  mapEpoch 감소, 완료된 sequence 이하, 중복 조각을 버립니다. 더 높은 epoch에서는 이전 맵 버퍼를 즉시 비웁니다.
- 불완전 프레임은 적용하지 않습니다. 조각 누락·역순은 다음 완전 프레임으로 회복합니다.
  RUDP unreliable 수신 자체가 낮은 전송 sequence를 버리므로 역순 단편은 유실될 수 있습니다.
  재조립 메모리는 최신 2개 프레임, 각 48KiB로 제한하고 오래된 미완성 프레임은 버립니다.
- 연결 변경·마을 복귀·재도전·Stopped 때 수신/보간/동작 버퍼를 비웁니다.
  재도전은 새 roomId와 새 challenge를 사용하므로 이전 던전 패킷을 적용하지 않습니다.
- 자기 플레이어 mapEpoch가 바뀌면 맵·좌표를 함께 전환하고 이동 보간을 새로 시작합니다.
  서로 다른 맵·epoch·존재 여부·사망 사이를 보간하지 않습니다. HP·상태는 최신 확정값을 사용합니다.

## payload 바이너리 v1

패딩 없이 **little-endian**, float은 IEEE754 binary32입니다. 문자열은 `u16 바이트 길이 + UTF-8 바이트`이며 NUL 종료가 없습니다.
NetBuffer의 바깥 string 안에 NUL이 포함된 바이너리를 그대로 저장합니다.
순서는 `playerCount:u16, monsterCount:u16, projectileCount:u16`, 플레이어 레코드들, 몬스터 레코드들, 탄환 레코드들입니다.
레코드 순서는 의미가 없으며 ID로 매칭합니다. 모든 길이·카운트·유한 float·ID 중복·잔여 바이트를 검증하세요.

공통 actor 레코드(46바이트):

`id:u64, dataId:u32, x:f32, y:f32, height:f32, verticalSpeed:f32, reactionSeconds:f32, hp:u32, maxHp:u32, facingLeft:u8, reaction:u8, reactionSequence:u32`

dataId는 플레이어=0, 몬스터=카탈로그 ID입니다. reaction은 0=None, 1=Hit, 2=Falling, 3=Down, 4=Rising, 5=Dead입니다.
reactionSequence는 피격 사건마다 증가합니다. 같은 Hit가 반복돼도 애니메이션 재시작을 구분할 수 있습니다.
reactionSeconds는 현재 단계의 남은 초이며 height/verticalSpeed는 월드 단위 및 월드 단위/초입니다.

플레이어 레코드: 공통 actor 다음 아래 30바이트(합계 76바이트).

`moveSequence:u32, actionSequence:u32, shotSequence:u32, jumpSequence:u32, shotPhase:u8, airAttack:u8, shotSeconds:f32, shotCount:u8, airShotCount:u8, jumpPhase:u8, jumpSeconds:f32, running:u8`

shotPhase: 0=None, 1=Prepare, 2=Fire, 3=Recover. jumpPhase: 0=Grounded, 1=Prepare, 2=Airborne.
shotSeconds는 현재 사격 단계의 경과 초, jumpSeconds는 점프 준비 경과 초입니다.
actionSequence는 기존 행동 입력 처리 번호(거절·추가 발사 입력에도 증가), moveSequence는 적용된 이동 입력 번호입니다.
shotSequence는 새 사격 묶음을 시작할 때, jumpSequence는 점프를 접수할 때만 증가합니다.
애니메이션은 이 두 번호와 단계 변화로 구분하고, 여러 발은 shotCount/airShotCount로 구분합니다.
running은 입력 달리기 여부이며 실제 이동 여부는 인접 좌표와 상태로 판별합니다.

몬스터 레코드: 공통 actor 다음 아래 필드(고정 합계 61바이트 + 두 문자열 바이트).

`actionSequence:u32, actionType:u8, actionStarted:u8, actionComplete:u8, actionSeconds:f32, aiNodeId:string16, animationId:string16`

actionType: 0=Wait, 1=MoveToTarget, 2=ReturnToSpawn, 3=UseSkill, 4=PlayMotion.
actionSequence는 노드 진입마다 증가하므로 같은 노드/스킬의 반복을 구분합니다.
actionSeconds는 현재 행동의 경과 초입니다. 캐릭터 전체 동작을 수신할 때마다 0으로 재설정하지 않습니다.
이동 행동은 기존 서버와 같이 actionSeconds=0일 수 있으며, 반복 motion은 정의된 길이에서 시간이 순환합니다.
actionStarted=0이면 시작을 기다리는 중입니다. animationId는 기존 서버 정의 값이며 이동/대기는 빈 문자열일 수 있습니다.

탄환 레코드(36바이트):

`id:u64, ownerId:u64, x:f32, y:f32, height:f32, direction:f32, heightDirection:f32`

direction은 기존 전투 JSON과 같은 수평 방향(-1/1)입니다. heightDirection은 수직 방향 성분입니다.
초기 월드의 projectileSpeed로 보간할 수 있습니다.

## 클라이언트 적용과 복구

1. YAML에서 DungeonProtocol을 생성하고 ID 12/13 핸들러와 구독 요청을 추가합니다.
2. 네트워크 스레드에서는 재조립·검증된 불변 상태만 게임 스레드에 전달합니다.
3. 원격 플레이어·몬스터 위치와 높이는 서버 시간에 맞춰 보간합니다. facing/HP/동작 전환은 상태 시점을 기준으로 처리합니다.
4. shot/action/reaction 식별자 및 경과 시간을 사용해 기존 애니메이션 타이머를 매 프레임 재시작하지 않도록 합니다.
5. 자신의 예측·이동 승인 처리와 기존 reliable 행동 결과를 유지합니다.
6. 초기 JSON 전투 상태와 정기 복구(예: 1초), realtime 중단 시 기존 폴링을 유지합니다.
   기존 JSON에 mapEpoch와 actor의 reactionSequence, 플레이어 shotSequence/jumpSequence,
   몬스터 actionSequence를 추가합니다. 기존 필드는 유지됩니다.
   realtime 활성화 후 오래된 JSON의 위치·동작이 최신 realtime을 덮어쓰지 않도록 serverTick을 비교합니다.
   JSON에 serverTimeMs가 있으면 그대로 사용하며 tick*50으로 덮어쓰지 않습니다.
   이전 서버 JSON에는 tickIntervalSeconds 또는 기존 0.05초 기준의 합성 시간을 사용합니다.
   50ms만 허용하던 구독 결과 검사에 새로운 33ms/67ms 규격을 반영합니다.
   약 500ms 이상 완성 프레임이 없으면 재조립을 버리고 reliable 전체 스냅샷으로 복구합니다.
   지원 응답이 없으면 기존 클라이언트 동작으로 돌아갑니다.
7. 클리어 상태는 한 번만 전달되거나 unreliable 유실될 수 있으므로 기존 reliable JSON cleared를 계속 확인합니다.
   클리어 선택·복귀·재도전 흐름은 기존 구현을 유지합니다.

## 서버 제약과 검증

- 변하는 방 상태는 strand에서만 읽고 변경합니다. 전송에는 공유 불변 프레임을 사용합니다.
- 연결 generation·challenge·roomId·playerId·구독 수명을 다시 확인한 후 전송합니다.
  연결/방 수명 안에서 sequence·epoch 카운터의 wrap은 지원하지 않습니다.
- 세션당 realtime 전송 예산은 최대 256KiB/초이며 전체 프레임 단위로 건너뜁니다.
  프레임이 48KiB를 넘으면 realtime을 보내지 않으므로 기존 JSON 복구가 필수입니다.
  15Hz는 보통 크기 방의 목표이며 밀집된 방·혼잡·유실에서 낮아질 수 있습니다.
- 새 경로는 reliable 재전송 대기열을 늘리지 않습니다. 기본 unreliable 대기열 64개는 최신 데이터를 우선합니다.
- 서버 수정 파일: Tool/PacketDefine.yml, DungeonProtocol.h/.cpp, DungeonSession.h/.cpp,
  GameRoom.h/.cpp, GameRoomCombat.cpp, GameRoomRealtime.cpp, 프로젝트와 필터, COMBAT_PROTOCOL.md.
- 서버 생성 파일과 YAML의 일치, HEAD 대비 기존 패킷 1~10의 필드 보존, 새 ID 11~13을 확인했습니다.
- 프로젝트·필터 XML의 신규 소스 포함과 레코드 크기(46/76/61+문자열/36바이트)를 정적으로 확인했습니다.
- 현재 성채 6개 맵의 몬스터 수(0/3/3/5/4/1)를 읽어 계산했습니다.
  4명·탄환 20개·모든 몬스터 식별 문자열 최대 길이를 가정해도 프레임은 최대 1,975바이트,
  목표 15Hz의 예산 상한 계산은 39,705바이트/초입니다. 실제 측정값이 아닙니다.
- 최대 조각의 IPv6/UDP·NetBuffer/RUDP·인증 태그 포함 크기 상한은 977바이트입니다.
  바깥 string은 기존 NetBuffer의 u16 길이와 바이트 복사이며 바이너리 NUL을 보존합니다.
- 기존 몬스터 전체 처치 워프 조건과 재생성 없음, 클리어 권한·마을 복귀·새 인스턴스 재도전 경로를 유지했습니다.
- `git diff --check`를 통과했습니다. 이번 변경은 stage·commit·push하지 않았으며 기존 index 항목을 보존했습니다.
- 실제 체감, 지연·유실 환경, 최대 부하 및 양쪽 컴파일은 확인하지 않았습니다.

## 2026-10-03 30Hz/15Hz 통합 확인

- GameRoom의 공유 상수로 타이머 간격, 이동·전투 dt, 2틱마다의 전송, 초기 월드/전투 JSON 시간 정보와 구독 안내값을 맞췄습니다.
- 틱 예약은 방 strand에서 처리합니다. 이동·AI·탄환·점프·스킬의 초 단위 설정은 바꾸지 않았습니다.
- 클라이언트 담당이 DungeonCombat.h/.cpp, GameWorld.cpp, DungeonClient.cpp를 수정했습니다.
  이후 별도 스킬 작업이 세 파일에 추가되어 전체 파일 해시는 달라졌지만, 시간 처리 변경은 유지됨을 현재 소스로 대조했습니다.
  ID 11~13의 서버·클라이언트 wire 선언이 같음을 확인했습니다.
- 클라이언트는 33/67ms 응답을 허용하며 이전 50/100ms와도 호환됩니다.
  JSON의 서버 시간은 보존하고 누락된 경우에만 서버 틱 간격으로 합성합니다.
- 기존 125ms 원격 보간 지연과 본인 입력 예측, 클리어/복귀/재도전 처리는 유지합니다.
  서버 틱레이트 변경으로 화면 FPS나 실제 왕복 지연을 측정·보장하지 않습니다.
- 양쪽 `git diff --check`를 통과했습니다. 기존 작업 중 변경을 보존하고 stage·commit·push하지 않았습니다.
- 클라이언트 상세 결과: [`CLIENT_HANDOFF.md`](CLIENT_HANDOFF.md).
