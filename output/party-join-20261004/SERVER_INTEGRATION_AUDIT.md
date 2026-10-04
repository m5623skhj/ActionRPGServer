# 던전 UI 제한·파티 기능 서버 연계 재점검

작성: 2026-10-04. 총괄 감찰 후 재점검 요청 범위에서 현재 서버 소스를 정적으로 확인했다. 추가 서버 코드 누락을 발견하지 않아 코드와 패킷을 변경하지 않았다. 이번 변경은 이 점검 기록 파일뿐이다.

## 항목별 확인과 근거

| 항목 | 근거 파일·함수 | 확인 결과 |
|---|---|---|
| 스키마와 생성 선언 | Tool/TownPacketDefine.yml, TownServer/TownPacket.generated.h; PacketSchema.ValidateSchema, PacketGenerator.RenderPlainHeader | 스키마의 현재 packet/field 명세를 읽어 기존 검증·렌더링 함수로 산출한 내용과 헤더 전체가 일치. 총 35개이며 30~35는 상세 조회/응답, 가입 요청/답변/상태, 강퇴 통지다. 비교는 파일을 쓰지 않는 정적 명세 대조이며 C++ 실행 테스트가 아니다. |
| 코덱 및 세션 분기 | TownServer/Protocol.h·Protocol.cpp의 신규 Encode/Decode, PlayerSession::HandlePacket | 30~35마다 Encode/Decode 선언·정의가 하나씩 존재. 요청 30·32·33의 인증 및 dispatch 분기가 하나씩 존재. 읽기/쓰기 필드 순서와 u8 count/enum/bool, ID/문자열/후행 데이터 검증을 대조했다. PartyOperationResult의 operation/result 최대 범위는 8/14다. |
| 승인 전 미가입 | TownInstance::RequestPartyJoin | 중복 대기 및 조건 검증 후 PendingPartyJoin만 저장하고 Pending 이벤트를 전송. PartyManager의 멤버 변경 함수는 호출하지 않는다. RequestJoin의 Succeeded는 요청 접수 성공이다. |
| 승인 시 재검증 | TownInstance::AnswerPartyJoin·ValidatePartyJoin, PartyManager::JoinApproved | 세션에서 얻은 실제 playerId와 요청 당시 파티장 ID를 대조하고 현재 파티장, 파티 존재·공개 상태, 접속·기존 가입·정원·던전 상태를 다시 확인. accepted=true이며 검증 성공인 경우에만 JoinApproved 호출. 실제 가입 함수도 파티장·공개 상태·중복 가입·빈 슬롯을 재확인한다. 요청 ID는 한 번 소비되며 중복 답변은 JoinRequestNotFound다. |
| 무효화 종료 통지 | TownInstance::PrunePartyJoinRequests·SendPartyJoinUpdate | 해산, 파티장 변경, 공개 해제, 요청자 종료/다른 파티 가입, 정원 충족, 던전 전환을 현재 상태로 검사하고 Invalidated + 실패 사유를 요청자 및 요청 당시 파티장 중 남아 있는 접속자에게 보낸 후 제거한다. 승인/거절도 Accepted/Rejected 종료 이벤트를 양쪽에 보낸다. |
| 무효화 연결 | TownInstance::ValidateDungeonRequest·CompleteDungeonRequest·EnterDungeonOnStrand·LeaveOnStrand·BroadcastPartySnapshot·NotifyPartyDirectoryChanged, RequestPartyJoin·AnswerPartyJoin | 모든 PartyManager 변경 호출과 실제 던전 상태 설정 경로의 정리 연결을 대조했다. 퇴장/재도전은 입장 시 이미 제거된 요청을 복원하지 않는다. 상세/승인 경로에서 새 패킷이나 취소·전체 큐 스냅샷을 요구하지 않는다. |
| 강퇴 구분 | TownInstance::KickPartyMember, PartyManager::Kick, Protocol::Encode(PartyKicked) | Kick 성공 분기에서 실제 targetPlayerId 세션에만 PartyKicked(35)를 보내고 빈 PartySnapshot을 이어 보낸다. PartyKicked 발송 경로는 이 한 곳이며 자진 탈퇴·해산·종료·강퇴 실패에는 발송하지 않는다. |
| 던전 스킬 제한 | TownInstance::LearnSkill·LeaveDungeon | dungeonRoomId/reservedDungeonRoomId를 검사하고 성장 상태 복사·Learn 호출 전에 InDungeon 현재 상태 응답 후 반환. SP·스킬 단계와 NotifyProgression 변경 없음. 복귀 후 기존 조건 검증을 다시 사용한다. 조회·배운 스킬 사용은 제한하지 않는다. |
| 던전 가입 우회 제한 | TownInstance::IsPlayerPartyBusy·IsPartyBusy·ValidatePartyJoin·RequestPartyDetail·InviteToParty·AnswerPartyInvitation | pendingDungeonPlayers, reservedDungeonRoomId, dungeonRoomId 기반으로 요청자 및 대상 파티의 Busy 여부를 판정. 신규 가입/승인과 기존 초대/수락 모두 실제 던전 참여 상태를 확인하며 가입을 적용하지 않는다. 권한 없는 답변은 NotLeader/존재하지 않는 요청은 JoinRequestNotFound로 거부될 수 있다. |
| 상태 소유권 | TownInstance::asio::dispatch(strand) 경로, tickTimer(strand), PlayerSession::GetPlayerId | 요청 보관·무효화·성장 변경·파티 가입 및 던전 상태 변경은 Town strand에서 직렬 처리한다. PlayerSession의 playerId는 atomic 읽기/쓰기이며 파티 요청의 주체는 Town sessionToPlayer로 다시 확인한다. 판정과 적용 사이에 비동기 작업을 추가하지 않았다. |

위 TownServer 경로의 절대 디렉터리는 C:/Users/KimHyeongJin/source/repos/ActionRPGServer/ActionRPGServer/TownServer다. 스키마 절대 경로는 C:/Users/KimHyeongJin/source/repos/ActionRPGServer/Tool/TownPacketDefine.yml이다. 상세 계약은 C:/Users/KimHyeongJin/source/repos/ActionRPGServer/output/party-join-20261004/SERVER_REPORT.md에 있다.

## 남은 클라이언트 연계 위험

* 서버 확인만으로 기능 전체 또는 UI 완료를 판정할 수 없다. 클라이언트의 생성물·코덱·네트워크 이벤트·UI 처리 연결은 클라이언트 담당 및 총괄의 확인 범위다.
* PartyJoinRequestUpdate는 requestId별 증분 계약이다. Pending은 중복 제거해 큐에 넣고 Accepted/Rejected/Invalidated는 현재 표시와 뒤의 대기 큐에서 모두 제거한다. 화면을 닫았다는 이유만으로 서버의 유효한 대기 요청을 잊으면 다음 요청이 AlreadyRequested가 될 수 있다. 접속 종료와 세션 교체 시에는 이전 큐를 폐기한다.
* RequestJoin/AnswerJoin의 Succeeded만으로 가입 완료를 표시하면 안 된다. Rejected의 result도 처리 성공인 Succeeded다. 가입 완료는 Accepted 및 PartySnapshot으로 반영한다.
* 전체 대기 스냅샷과 사용자 취소 패킷은 현재 계약에 없고 이번에 추가하지 않았다. 파티장 변경 시 이전 요청은 무효화하며 새 파티장으로 자동 이전하지 않는다.
* 상세 응답은 partyId와 현재 선택을 대조하고 목록 revision 변경 시 필요한 상세를 다시 조회해야 한다. 상세 자체에 갱신 구독을 추가하지 않았다.
* 강퇴 중앙 확인 알림은 PartyKicked를 근거로 표시한다. 빈 스냅샷만으로 추정하지 않으며 상태 초기화가 확인 알림을 지우지 않도록 해야 한다.
* 서버 제한은 실제 strand 처리 순서가 기준이다. 입장 상태 설정 전에 완료된 정상 마을 요청은 유지되고, 상태 설정 이후의 요청은 차단한다. 현재 클라이언트 UI가 이 순서를 어떻게 표시하는지는 실행 미확인이다.

## 수행·미수행

스키마/생성 선언 비교, 코덱·세션 분기 존재와 필드 순서 대조, 권한·정원·상태 변경 및 이벤트 발송 경로 검토, 대상 소스 git diff --check를 수행했다. PyYAML이 없는 환경에서 이전과 동일하게 현재 스키마의 단순 명세를 추출해 기존 ValidateSchema/RenderPlainHeader 함수를 읽기 비교에 재사용했으며 생성물을 다시 쓰지 않았다.

빌드, 기능·통합 테스트, 게임/서버 실행, 프로세스 종료, 백업 생성, stage/commit/push 및 다른 채팅으로의 메시지 발송은 하지 않았다. 실제 네트워크 순서·동시 요청·UI 알림 표시 및 부하는 미검증이다.

## 이번 변경의 제안 커밋 로그

* 던전 UI 제한과 파티 서버 연계 재점검 기록
  * 패킷·세션 분기·가입 승인 및 무효화 경로 근거 정리
  * 던전 우회 차단과 강퇴 대상 통지 및 클라이언트 연계 위험 기록

문서 추가만을 요약한 제안 로그이며 실제 커밋 및 푸시는 수행하지 않았다.
