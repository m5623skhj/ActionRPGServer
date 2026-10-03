# 던전 클리어 선택 창 변경 인계

상태: 승인된 코드 변경 적용 완료. 정적 확인 완료. 빌드/서버·게임 실행/기능 테스트 미실시.
실제 stage/commit/push는 수행하지 않았다. 클라이언트 DungeonEditor의 기존 별도 변경은 보존했다.

## 동작

- `cleared=true`의 최초 수신에 마을 이동/재도전 모달을 표시한다. 같은 serverTick의 최초 클리어 전이도 허용한다.
- 마우스, 방향키와 Enter로 선택한다. 전투 입력과 중복 선택은 차단하며 실패/지연은 안내하고 재선택을 제공한다.
- 파티장은 전체 파티의 행동을 결정한다. TownServer는 인증 ID/현재 방/파티장/참여자/중복 요청을 검사한다.
- GameRoomServer는 Cleared 상태와 실제 입장한 참여자 집합을 검증한다. 관련 상태 변경은 각각의 기존 strand에서 처리한다.
- 마을 이동은 서버 방을 제거하고 TownServer의 던전 참여 상태와 가시성을 복구한다.
- 재도전은 같은 dungeonId로 새 roomId/seed/플레이어·몬스터·투사체 상태를 생성한다. 생성 실패 시 기존 방을 유지하며 방 한도에 도달해도 교체를 허용한다.
- 기존 RUDP 코어 종료는 비동기로 기다린다. 종료 완료 전에 새 코어를 시작하지 않는다. 그동안 도착한 마을 이벤트는 게임 스레드에서 보관·재처리한다.

## 변경 계약

원본: `Tool/TownPacketDefine.yml`. 기존 Town packet ID 1~24는 유지했다.
Town TCP 25=`DungeonCompletionRequest`, 26=`DungeonCompletionResponse`.
RoomControl TCP 8=`FinishRoom`, 9=`FinishRoomResult`.
필드 순서/타입/검증은 `ActionRPGServer/GameRoomServer/COMBAT_PROTOCOL.md`의 '클리어 후 선택'을 따른다.
기존 Dungeon RUDP 패킷과 전투 JSON 버전은 변경하지 않았다.
클라이언트/TownServer/GameRoomServer를 함께 빌드·갱신해야 한다.

## 변경 파일

서버 17개: COMBAT_PROTOCOL.md, GameRoom.cpp/h, RoomManager.cpp/h, TownControlClient.cpp,
Shared/RoomControlProtocol.cpp/h, TownServer/PlayerSession.cpp, Protocol.cpp/h,
RoomControlTcpServer.cpp/h, TownInstance.cpp/h, TownPacket.generated.h, Tool/TownPacketDefine.yml.
클라이언트 9개: Game/GameWorld.cpp/h, Network/DungeonClient.cpp/h, TownClient.cpp/h,
TownProtocol.cpp/h, TownPacket.generated.h.
이 인계 문서 외에 총 26개 파일을 적용했다. 생성/검사 작업 파일은 무시되는 artifacts/dungeon-clear-edit에 있다.

## 확인

- 원본 PacketGenerator로 생성한 양쪽 헤더 일치 및 `--check` 상당 검사를 확인했다.
- 양쪽 새 Town 직렬화/역직렬화 본문과 기존 packet ID 보존을 정적으로 확인했다.
- 소스 괄호 균형, UI/전환/권한 가드, 적용 파일 SHA-256 일치, 두 저장소 `git diff --check`를 확인했다.
- 실제 솔로/파티 클리어, 마을 복귀 후 플레이어 표시, 반복 재도전, 생성 실패 및 연결 실패 동작은 실행 검증하지 않았다.

## 다음 작업의 주의점

이 변경은 완료되어 이동 보간 담당이 위 파일의 현재 내용을 기준으로 작업할 수 있다.
같은 tick 클리어 처리, completionStopping 동안의 네트워크 이벤트 보관,
복귀/재도전의 ResetDungeonEntry와 비동기 종료 순서를 유지해야 한다.
이동 보간 요청 movement-smoothing-20261003은 별도 후속 작업이며 이 변경에 포함하지 않았다.
