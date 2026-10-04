# 파티 상세 조회·가입 승인·강퇴 통지 서버 결과

작성: 2026-10-04. 총괄이 전달한 사용자 승인에 따라 서버 원본 스키마와 서버 구현을 수정했다. 클라이언트 파일은 수정하지 않았다. 기존 성장·전투·마을 이동 변경을 보존했다.

## 확정 패킷 계약

기존 TCP 프레임과 packetType:u16, 빅엔디언 정수, bool:u8(0/1), UTF-8 문자열의 바이트 길이:u16 계약을 사용한다. 기존 패킷 1~29 번호 및 필드는 유지한다. 아래 필드는 실제 직렬화 순서다.

| 번호 | 이름 / 방향 | 필드 |
|---|---|---|
| 30 | PartyDetailRequest / C→S | partyId:u64 |
| 31 | PartyDetailResponse / S→C | partyId:u64, result:u8, title:string, leaderPlayerId:u64, isPublic:bool, busy:bool, memberCount:u8, members |
| 32 | PartyJoinRequest / C→S | partyId:u64 |
| 33 | PartyJoinAnswer / C→S | requestId:u64, accepted:bool |
| 34 | PartyJoinRequestUpdate / S→C | requestId:u64, partyId:u64, requesterPlayerId:u64, requesterName:string, state:u8, result:u8 |
| 35 | PartyKicked / S→C | partyId:u64, leaderPlayerId:u64 |

각 member는 기존 PartyMemberInfo와 동일한 playerId:u64, playerName:string, slot:u8 순서다. 최대 8명이며 slot은 0~7이다. 공개 파티 상세 조회 성공 시 파티장 ID는 members에 포함된다. 상세 응답의 partyId는 요청한 ID이며 UI가 현재 선택과 대조해야 한다. 실패 시 result 외에 제목은 빈 문자열, 파티장 ID는 0, 두 bool은 false, 멤버 개수는 0이다.

PartyJoinRequestState는 Pending=1, Accepted=2, Rejected=3, Invalidated=4다. Pending/Accepted/Rejected의 result는 Succeeded=0이고 Invalidated는 실패 사유다. 승인과 거절은 state로 구분한다. 거절이 Succeeded인 것은 거절 처리가 성공했다는 뜻이며 가입 성공을 뜻하지 않는다.

PartyOperationType에 RequestJoin=7, AnswerJoin=8을 추가했다. PartyResultCode의 기존 0~10은 유지하고 AlreadyRequested=11, JoinRequestNotFound=12, PartyNotFound=13, NotPublic=14를 추가했다. PartyOperationResult 디코더의 최대 operation/result도 8/14로 넓혀야 한다. 기존 결과 Busy=9, AlreadyInParty=2, NotLeader=4, PartyFull=5, PlayerNotFound=1도 사용한다.

## 가입 요청과 처리 흐름

1. 현재 로그인 세션에서 선택한 공개 파티 ID로 상세 조회한다. 멤버 이름·슬롯과 파티장 ID를 반환한다. 요청자가 던전 처리 중이면 Busy, 파티가 사라졌으면 PartyNotFound, 비공개면 NotPublic을 반환한다. 조회하는 공개 파티가 던전 처리 중이면 상세 응답의 busy=true로 가입 불가 상태를 알린다.
2. 가입 요청의 주체는 인증된 sessionToPlayer 값이다. 요청자는 다른 playerId를 지정할 수 없다. 파티 존재/공개 여부, 파티장과 요청자의 접속 상태, 요청자의 기존 가입 여부, 파티 정원 및 던전 상태를 확인한다.
3. 유효하면 서버 requestId를 발급해 보관하고 요청자에게 PartyOperationResult(RequestJoin, Succeeded)를 보낸 뒤 요청자와 당시 파티장에게 Pending 업데이트를 보낸다. 이 시점에는 파티 멤버를 변경하지 않는다.
4. 요청자별 대기 요청 하나만 보관한다. 같은 파티 또는 다른 파티로의 중복 대기는 AlreadyRequested로 거부하며 기존 대기를 유지한다. 클라이언트는 requestId로 중복 알림을 제거하고 요청 ID 순서로 여러 대기를 표시한다. 서버는 파티장이 답변한 유효 ID를 처리하며 UI의 표시 순서를 강제하는 별도 정책은 추가하지 않았다.
5. 답변 처리 시 낡은 요청을 정리한 뒤 실제 sessionToPlayer가 요청의 파티장인지 확인한다. 다른 사용자의 답변은 NotLeader이며 요청을 소비하지 않는다. 처리 전에 파티장 변경, 존재/공개 상태, 정원, 요청자 가입·접속 상태 및 던전 상태를 다시 검증한다.
6. 승인일 때만 PartyManager::JoinApproved를 호출한다. 여기서도 공개 파티, 파티장 ID, 중복 가입 및 빈 슬롯을 재검증해 실제 멤버를 추가한다. 소비한 requestId를 먼저 제거해 중복 답변이 다시 가입시키지 못한다. 승인은 Accepted, 거절은 Rejected, 조건 변경으로 실패하면 Invalidated 업데이트를 양쪽에 보낸다. 성공 가입 후 PartySnapshot 및 PartyDirectoryChanged를 보낸다.

이미 소비되거나 무효화된 ID의 답변은 JoinRequestNotFound다. 이 답변 자체로 새 대기 알림을 제거하지 말고, 해당 requestId의 종료 이벤트로 제거해야 한다. 요청 ID는 같은 TownInstance 수명 동안 증가하며 클라이언트 재접속 시 이전 알림을 유지하면 안 된다.

## 무효화와 던전 제한

* 해산: PartyNotFound. 파티장 변경: NotLeader. 비공개 전환: NotPublic. 요청자 종료: PlayerNotFound. 다른 파티 가입: AlreadyInParty. 정원 충족: PartyFull. 던전 상태 전환: Busy. 현재 상태에 따라 첫 번째로 해당하는 실패 사유를 보낸다.
* 요청자와 요청 당시 파티장 중 아직 접속 상태가 있는 쪽에 Invalidated를 보내고 서버 대기를 제거한다. 파티장이 바뀌어도 옛 요청을 새 파티장에게 자동 이전하지 않는다. 요청자가 다시 요청할 수 있다.
* 기존 파티 멤버/설정/파티장 갱신의 스냅샷·목록 갱신, 접속 종료, 던전 요청 대기·입장 예약·입장 상태 변경과 새 요청/답변 처리에서 정리한다. 매 이동 틱마다 전체 요청을 반복 검사하지 않는다.
* 요청자 또는 파티 멤버의 pendingDungeonPlayers, reservedDungeonRoomId, dungeonRoomId를 기존 Busy 상태로 판정한다. 따라서 입장 대기·예약·실제 참여 중에는 새 가입 및 승인이 이뤄지지 않는다. UI 비활성화를 우회하는 직접 패킷도 같은 경로를 거친다.
* 기존 초대/초대 수락에서도 대상의 실제 dungeonRoomId를 Busy 판정에 포함하도록 보완했다. 파티 전체 기능의 새로운 정책이나 관계없는 제한은 추가하지 않았다.
* 유효한 대기에 임의 시간 만료, 재요청 대기 시간 또는 신규 설정을 추가하지 않았다. 대기 수는 접속 중 요청자 수 이하이며 중복 확인·무효화 검사는 대기 수에 비례한다. 실제 부하는 실행 검증하지 않았다.

## 강퇴 통지

기존 PartyOperationResult(Kick)는 파티장에게만 전달되고 강퇴 대상은 빈 PartySnapshot만 받아 자진 탈퇴와 구분할 수 없었다. 중복 추정 로직 대신 PartyKicked(35)를 추가했다.

KickPartyMember에서 권한/상태 검증 및 PartyManager::Kick 성공 후 실제 강퇴 대상에게만 PartyKicked{partyId, leaderPlayerId}를 보내고 이어서 빈 PartySnapshot을 보낸다. 강퇴 실패, 자진 탈퇴, 접속 종료, 파티 해산 경로에서는 PartyKicked를 보내지 않는다. 빈 스냅샷은 상태 정리용이며 강퇴 메시지의 근거가 아니다.

클라이언트는 PartyKicked 이벤트에서 화면 중앙에 '파티에서 강퇴되었습니다.' 확인 알림을 표시해야 한다. 상세 창이나 파티 스냅샷 초기화가 이 알림을 취소하지 않도록 별도 확인 알림 상태로 관리해야 한다.

## 변경 파일

* C:/Users/KimHyeongJin/source/repos/ActionRPGServer/Tool/TownPacketDefine.yml
* C:/Users/KimHyeongJin/source/repos/ActionRPGServer/ActionRPGServer/TownServer/TownPacket.generated.h
* C:/Users/KimHyeongJin/source/repos/ActionRPGServer/ActionRPGServer/TownServer/Protocol.h
* C:/Users/KimHyeongJin/source/repos/ActionRPGServer/ActionRPGServer/TownServer/Protocol.cpp
* C:/Users/KimHyeongJin/source/repos/ActionRPGServer/ActionRPGServer/TownServer/PlayerSession.cpp
* C:/Users/KimHyeongJin/source/repos/ActionRPGServer/ActionRPGServer/TownServer/PartyManager.h
* C:/Users/KimHyeongJin/source/repos/ActionRPGServer/ActionRPGServer/TownServer/PartyManager.cpp
* C:/Users/KimHyeongJin/source/repos/ActionRPGServer/ActionRPGServer/TownServer/TownInstance.h
* C:/Users/KimHyeongJin/source/repos/ActionRPGServer/ActionRPGServer/TownServer/TownInstance.cpp
* C:/Users/KimHyeongJin/source/repos/ActionRPGServer/output/party-join-20261004/SERVER_REPORT.md

동시에 완료한 던전 스킬 습득·강화 제한은 output/dungeon-ui-20261004/SERVER_REPORT.md에 기록했다. 해당 변경도 TownInstance.cpp에 있으며 result="InDungeon"을 기존 SkillStateResponse에 반환한다. 이전 마을 이동 보완은 output/town-movement-20261004/SERVER_REVIEW.md에 있다.

## 정적 검토와 남은 연계

* 세션 인증 → Town strand dispatch → 상태 재검증 → 단일 승인 가입 → 종료 이벤트 → 스냅샷 경로의 선언/정의 및 호출을 대조했다. 요청 보관과 PartyManager 변경은 같은 Town strand에서 처리하며 검증과 가입 사이에 비동기 작업을 넣지 않았다.
* 모든 PartyManager 변경 호출과 던전 상태 설정 경로의 무효화 연결을 확인했다. 가입 확정 뒤 다른 요청의 정원 검사와 중복 답변 제거를 확인했다. 상태 처리 순서는 Town strand에서 실제 실행된 순서가 기준이다.
* 새 코덱은 ID 0, bool/enum 범위, 문자열 길이, 멤버 개수·ID·슬롯 중복, 파티장 멤버 포함, 후행 데이터 및 상태/result 조합을 검증한다. 기존 packetId 1~29와 스킬·성장 선언은 유지했다.
* PyYAML이 없어 PacketGenerator의 일반 CLI는 실행되지 않았다. 현재 스키마의 단순 packet/field 명세를 추출하고 기존 PacketSchema::ValidateSchema 및 PacketGenerator.py의 RenderPlainHeader 함수를 재사용해 서버 헤더만 작성했다. 클라이언트 출력 대상은 쓰지 않았다. 생성한 35개 패킷과 실제 코덱의 순서를 정적으로 대조했다.
* 대상 소스의 git diff --check 통과. C++ 빌드, 테스트, 게임/서버 실행, 백업, stage/commit/push는 하지 않았다. 동시 가입·해산·파티장 변경·강퇴 UI의 실제 동작은 실행 미확인이다.
* 클라이언트 담당은 자체 생성물/외부 enum, 신규 코덱 및 이벤트를 연결하고 operation/result 최대 범위를 확장해야 한다. 클라이언트 소스와 생성물을 서버 담당이 수정하지 않았다.
* 상세 응답의 partyId와 현재 선택을 대조하고 목록 revision 변경 시 필요한 상세를 다시 조회해야 한다. 상세는 조회 시점의 상태이며 별도 상세 구독을 추가하지 않았다. 실제 가입 판단은 서버가 매번 다시 수행한다.
* 대기 큐는 requestId로 중복을 제거하고 종료 이벤트에서 현재 알림과 뒤의 큐에서 모두 제거해야 한다. AnswerJoin 성공 결과만으로 가입 성공을 표시하지 말고 Accepted 업데이트/PartySnapshot을 사용해야 한다.
* 던전 전환과 접속 종료에서 관련 창·알림 큐를 정리하고, 강퇴 통지는 중앙 확인 창으로 처리해야 한다. 다른 담당에게 직접 메시지는 보내지 않았다.

## 제안 커밋 로그

* 던전 스킬 제한과 파티 가입 승인·강퇴 통지 구현
  * 던전 참여·입장 예약 중 습득과 강화 차단 및 InDungeon 상태 반환
  * 공개 파티 상세 조회와 파티장 승인 후 가입 처리 추가
  * 중복 요청과 해산·파티장 변경·종료·던전 전환 시 요청 무효화 통지
  * 기존 초대 경로의 던전 참여 판정과 강퇴 대상 명시적 통지 보완
  * 서버 패킷 스키마·선언·코덱 및 클라이언트 연계 계약 기록

제안 로그이며 실제 커밋 및 푸시는 수행하지 않았다.
