# 마을 이동 보정 클라이언트 담당 인계

작성: 2026-10-04. 총괄이 적용한 Character.h/Player.h/.cpp/GameWorld.h/.cpp를 검토하고 필요한 범위만 추가 수정했다. 소스 적용 완료이며 빌드·테스트·게임/서버 실행·백업 생성·실제 stage/commit/push·다른 채팅 직접 메시지는 하지 않았다. 서버 틱 변경과 PNG는 이번 담당 작업에서 수정하지 않았다.

## 발견한 문제와 수정

1. **최신 입력과 동일한 ACK만 허용하여 높은 지연에서 보정을 놓침.** Town 입력은 방향 변경 시 즉시, 이동 유지 중250ms마다 새 sequence로 전송한다. 서버는 입력 수신 시 lastProcessedInput을 갱신하고50ms 틱/2틱마다 PlayerMove를 전달한다. 응답이 오는 동안 새 heartbeat를 보내면 기존 `ack == movementSequence`는 정상 응답을 제외한다. 같은 입력 번호의 뒤늦은 첫 응답에 최신 입력의 전송 시각을 사용해서도 안 된다.
2. **제한된 전송 시각 기록과 같은 상태 구간의 ACK 허용.** 최대64개 `sequence/sentTime`을 게임 스레드 deque로 관리한다. 현재 방향 상태가 시작된 sequence 이상이며 보낸 sequence 이하인 응답은, 실제 현재 입력도 같은 상태이면 최신 heartbeat보다 이전 번호여도 보정할 수 있다. 방향을 바꿨다가 같은 방향으로 돌아온 경우에도 이전 상태 구간의 ACK는 제외한다. 정지 응답도 같은 상태 규칙을 따른다.
3. **지연 추정은 해당 ACK의 전송 시각으로1회만.** 최초로 진행한 ACK마다 실제 대응 기록에서 경과 시간/2를 구하고0..0.25초로 제한해 기존0.2 EWMA에 반영한다. 같은 ACK의 후속 스냅샷에는 다시 샘플링하지 않는다. 스킵된 중간 ACK를 가짜 샘플로 만들지 않는다. 기록을 찾지 못하거나 이미 보낸 번호보다 큰 ACK는 적용하지 않는다. serverTick이 뒤로 가거나 중복이면 적용하지 않는다. 처리 완료 번호보다 이전 기록은 제거하되 현재 ACK 기록은 후속 스냅샷을 위해 유지한다.
4. **방향 변경 후 남은 보정 벡터 제거.** Player에 마을 보정만 취소하는 ClearTownPositionCorrection을 추가했다. 마을 프레임의 유효 방향이 전송 상태와 달라지면 Player.Update 전에 취소하고, 실제 새 이동 상태를 전송할 때도 취소한다. 기존 메뉴/피격/공격/로컬 스킬의 이동 차단 상태를 수신 판정에 반영해 원시 키 상태와 서버에 보낸 정지가 어긋나지 않게 했다.
5. **맵 초기화에서 기록 제거.** ApplyMap에서 deque/현재 상태 구간 번호/ACK/tick/지연 추정을 초기화한다. 기존 Player.ResetActionState의 pendingTownCorrection 초기화도 유지한다.

## 보존한 기존 변경

- Player::ReconcileTownGroundPosition의 서버 속도 기반 최대0.25초 외삽, 마을 충돌 영역 제한,16단위 dead zone,200단위 큰 차이 snap, 프레임별 지수 보정(rate12)을 검토하고 유지했다. 클라이언트와 TownMap의 충돌 반경32/18이 일치함을 확인했다.
- Player.Update는 HasCombatState 경로에서 반환한 뒤에만 마을 보정을 적용한다. 던전 ReconcileGroundPosition의16/200/0.35 동작과 GameWorld의 두 던전 호출을 보존했다.
- 준비 직전 소스와 비교해 ApplyCombatState/UpdateDungeonCombat/UpdateRemotePlayers 함수 영역 및 SendMovementInput의 던전 분기가 같음을 확인했다. Player.cpp와 Character.h는 추가 수정하지 않았다.
- 다른 플레이어가 정지하면 즉시 snap하지 않고 기존 프레임 보간을 계속 사용한다.200단위 큰 차이만 snap하는 총괄 변경을 유지했다. 원격 플레이어의 기존 마지막 위치 추종을 별도 시간 버퍼 구현으로 확대하지 않았다.
- 스킬 UI/성장 권한/단축키/마을 앞차기/기본 공격·점프 입력 및 미커밋 이미지·서버 변경을 보존했다.

## 이번 담당의 변경 파일

클라이언트 저장소 상대 경로3개:

- ActionRPGClient/ActionRPGClient/Game/GameWorld.h — 제한된 전송 기록과 현재 입력 상태 구간 번호.
- ActionRPGClient/ActionRPGClient/Game/GameWorld.cpp — 이전 heartbeat ACK 처리,1회 샘플, 상태 변경 취소, 맵 초기화.
- ActionRPGClient/ActionRPGClient/Game/Player.h — ClearTownPositionCorrection.

소스 전/후 SHA와 목록은 ignored artifacts/town-movement-20261004/CLIENT_MANIFEST.json에 기록했다. 원본 백업은 만들지 않았다. 기존 REPORT.md의 “이전 입력 응답은 사용하지 않음” 설명은 **이전 이동 상태 구간은 제외하되 현재 상태 구간의 이전 heartbeat ACK는 허용**으로 읽어야 한다.

## 정적 검토와 연계 사항

- 실제 TownProtocol MoveInput/PlayerMove 필드, 서버 SetMovementInput/Simulate/BroadcastMovement, 클라이언트 입력 heartbeat/게임 프레임 순서를 읽어 검토했다. 패킷 구조·버전과 TownClient 네트워크 전송은 변경하지 않았다.
- deque와 ACK/지연/잔여 보정은 GameWorld/Player의 기존 게임 스레드 소유다. asio 콜백과 공유 버퍼를 추가하지 않았다. TownClient는 기존 strand·이벤트 큐를 유지한다.
- 변경 파일 C++ 괄호/선언·호출/후행 공백과 보존 영역 소스 비교를 확인했다. 설치 직전에 전 파일 SHA를 대조해 다른 세션 변경을 덮어쓰지 않게 했다. diff --check와 적용 후 SHA를 확인한다.
- 서버 담당의 절대 틱 예약/실제 경과 시간 변경과 함께 적용해야 한다. 클라이언트에서는 서버 속도·타임아웃·틱 정책을 임의로 변경하지 않았다.

## 남은 한계와 미검증

- 입력 응답 시간에는 클라이언트 큐, 왕복 전송, 서버 틱/10Hz 발행 대기가 포함된다. 그 절반은 **정확한 RTT/one-way delay/스냅샷 나이가 아니다**. 패킷에 스냅샷 시각이나 동기화 시각을 추가하지 않았다. 비대칭 지연·지터에서는 과소/과대 외삽이 남을 수 있다.
- 외삽은0.25초로 제한한다. 방향 전환/정지 후에는 새 상태 구간의 응답을 기다린다. 빠른 방향 전환을 완전한 입력 재실행으로 복원하지 않는다.64개 기록 밖의 응답은 제외한다. 이는 메모리를 제한하고 다른 상태의 오래된 위치를 적용하지 않기 위한 처리다.
- 서버 입력 타임아웃500ms, 긴 전송 중단, 큰 authoritative 오차/텔레포트의 snap은 그대로 남는다. 보정 자체를 제거하거나 항상 로컬 위치를 신뢰한 변경은 아니다.
- 컴파일/링크, 실제 저지연/고지연·지터·방향 전환·정지·메뉴/공격/피격·벽 접촉·맵/세션/던전 왕복의 체감은 미검증이다. 별도 승인 후 빌드와 게임 확인이 필요하다.

## 제안 커밋 로그

* 마을 이동 보정의 지연 응답 처리 개선
  * 동일 이동 상태의 이전 heartbeat ACK 허용과 제한된 전송 시각 기록 추가
  * 대응 입력의 최초 응답으로 지연 추정하고 중복·오래된 상태 적용 방지
  * 방향 전환·정지 시 남은 마을 보정 취소 및 맵 전환 기록 초기화
  * 기존 던전 보정·원격 이동 보간·스킬 UI와 입력 개선 보존

실제 커밋/푸시는 수행하지 않았다.
