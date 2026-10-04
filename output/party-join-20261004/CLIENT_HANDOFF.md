# 파티 상세·가입 승인·강퇴 통지 클라이언트 결과

작성: 2026-10-04. 기존 승인된 기능을 클라이언트 소스에 반영했다. 빌드·테스트·게임/서버 실행·프로세스 종료·백업·stage/commit/push는 이번 기능 작업에서 수행하지 않았다.

## 적용 계약

기준은 `C:/Users/KimHyeongJin/source/repos/ActionRPGServer/output/party-join-20261004/SERVER_REPORT.md`의 확정 계약(2026-10-04, 패킷 30~35)이다. 별도 wire 버전 필드를 추가하지 않았다. 서버의 `Tool/TownPacketDefine.yml`, `ActionRPGServer/TownServer/Protocol.h/.cpp`, 생성 헤더와 대조했다.

| 번호 | 패킷 | 실제 필드 순서 |
|---|---|---|
| 30 | PartyDetailRequest | partyId:u64 |
| 31 | PartyDetailResponse | partyId:u64, result:u8, title:string, leaderPlayerId:u64, isPublic:u8, busy:u8, count:u8, members |
| 32 | PartyJoinRequest | partyId:u64 |
| 33 | PartyJoinAnswer | requestId:u64, accepted:u8 |
| 34 | PartyJoinRequestUpdate | requestId:u64, partyId:u64, requesterPlayerId:u64, requesterName:string, state:u8, result:u8 |
| 35 | PartyKicked | partyId:u64, leaderPlayerId:u64 |

member는 playerId:u64, playerName:string, slot:u8 순서다. 기존 TCP 프레임, 빅엔디언 정수, UTF-8 문자열의 u16 바이트 길이를 유지한다. 기존 패킷 1~29의 구조체를 교체 전 대조해 동일함을 확인했다.

`PartyOperationType`은 RequestJoin=7, AnswerJoin=8까지, `PartyResultCode`는 AlreadyRequested=11, JoinRequestNotFound=12, PartyNotFound=13, NotPublic=14까지 확장했다. 기존 PartyOperationResult 디코더의 최대 범위도 8/14로 반영했다. state는 Pending=1, Accepted=2, Rejected=3, Invalidated=4다. Rejected의 result=Succeeded는 거절 처리 성공이며 가입 성공으로 해석하지 않는다.

서버의 신규 Encode/Decode 정의를 동일하게 사용한다. ID 0, bool/enum 범위, 문자열 크기, 8명 제한, ID·슬롯 중복, 파티장 멤버 포함, 실패 상세의 빈 필드, state/result 조합 및 후행 바이트 검증을 유지했다. 생성 헤더는 서버 파일과 바이트 단위로 동일하다. 별도 취소 패킷이나 대기 큐 스냅샷을 도입하지 않았다.

## 요구별 구현 및 정적 확인

파일 루트: `C:/Users/KimHyeongJin/source/repos/ActionRPGClient/ActionRPGClient/ActionRPGClient/`.

| 요구 | 구현 파일/함수 | 정적 확인 결과 |
|---|---|---|
| 목록의 파티 선택 시 상세 조회 | Game/GameWorld.cpp: UpdatePartyInterface, RequestSelectedPartyDetail, PartyDetailResponse 처리, RenderPartyDetails | 행 클릭에서 선택 partyId로 30 송신. 31의 partyId가 현재 선택과 같을 때만 적용. 실제 파티장 ID와 멤버 이름·슬롯을 표시. 임의 레벨·직업 정보 없음. |
| 가입 버튼은 요청만 전송 | Game/GameWorld.cpp: CanRequestPartyJoin, UpdatePartyInterface; Network/TownClient.cpp: RequestPartyJoin | 연결·던전·기존 파티·기존 요청·상세 조회 중·공개/Busy/정원 조건 확인 후 32 송신. 멤버를 클라이언트에서 추가하지 않음. |
| 파티장 오른쪽 아래 승인/거절 창 | Game/GameWorld.cpp: UpdatePartyNotifications, RenderPartyNotifications, IsPartyJoinNoticeVisible | 현재 파티장과 partyId가 일치하는 Pending만 보관. 요청 ID 순 lower_bound 삽입으로 정렬 및 중복 제거. 앞 요청 한 건 표시. 33의 정확한 requestId로 승인/거절. |
| 여러 대기와 종료 이벤트 정리 | Game/GameWorld.cpp: PartyJoinRequestUpdate 처리, ResetPartyRequestUi, PartySnapshot 처리 | Accepted/Rejected/Invalidated에서 해당 ID를 전체 큐에서 제거. 답변 결과에 ID가 없으므로 다른 요청을 제거하지 않음. 해산·파티 변경·파티장 변경·던전·연결 종료·EnterTown에서 적절히 정리. |
| 강퇴 대상 본인 중앙 확인 창 | Network/TownClient.cpp: PartyKicked 수신; Game/GameWorld.cpp: PartyKicked 처리, UpdatePartyNotifications, RenderPartyNotifications | 35에서만 표시. 빈 PartySnapshot은 알림을 취소하지 않음. 자진 탈퇴·해산·빈 스냅샷에서 추측하지 않음. 중앙 문구는 ‘파티에서 강퇴되었습니다.’. 확인 클릭/Enter/ESC로 닫음. |
| 강퇴 창의 입력 차단 | Game/GameWorld.cpp: Update, IsUiOverlayVisible | 일반 메뉴·파티·던전 선택 입력 전에 처리. 열림/닫힘 프레임에 게임 입력과 스킬 드래그를 차단. 최종 렌더 단계에서 다른 UI 위에 표시. |
| 상세 최신 상태 | Game/GameWorld.cpp: PartyDirectoryChanged/Page 처리, RequestSelectedPartyDetail, SetSystemUiPage | 목록↔상세에서 목록 구독을 유지. revision 갱신에 상세 재조회. 요청 중 추가 변경은 한 번의 후속 조회로 합침. 페이지 요청 중 revision을 기억해 갱신을 놓치지 않음. |
| 스레드 안전성 | Network/TownClient.h/.cpp의 TownEvent, RequestPartyDetail/Join/Answer, HandlePacket, ConsumeEvents; Game/GameWorld.cpp의 ProcessNetworkEvents | 송신은 기존 asio strand. 수신은 기존 mutex 이벤트 큐. UI/요청 큐는 게임 스레드에서만 변경. 네트워크 콜백이 UI 상태를 직접 쓰지 않음. |
| 던전 파티 UI 제한 보존·초대 우회 보완 | Game/GameWorld.h/.cpp: IsPartyUiPage, UpdatePartyInterface, Update, RenderPartyInterface, SetSystemUiPage | PartyDetails도 제한 대상. 제한 검사가 초대 처리보다 먼저 실행. 던전 전환 시 남은 초대/가입 표시 정리. 제한 중 파티 입력과 렌더 차단. |
| 던전 스킬 제한·표시/등록 보존 | Game/GameWorld.cpp: SetLearningAllowed, LearnSkill 송신 조건; Game/SkillUi.h/.cpp | 습득·강화 제한 유지. 스킬 조회/드래그/슬롯 등록/사용 경로를 변경하지 않음. 알림 배치를 위한 GetHotbarBounds만 공개. 기존 그림을 사용. |

가입 승인/거절 버튼을 누른 뒤에는 종료 업데이트가 도착할 때까지 같은 ID의 버튼을 비활성화한다. AnswerJoin 성공 결과만으로 큐나 멤버를 확정하지 않는다. 요청자에게는 응답 대기, 승인, 거절, 무효화 사유를 표시한다. 실제 가입 확정은 서버 PartySnapshot이다.

가입 알림은 스킬 바의 실제 배치 위 오른쪽에 그려 슬롯을 가리지 않는다. 대기 알림 자체는 모달이 아니며 본인 이동 입력을 멈추지 않는다. 알림 클릭은 뒤의 메뉴/스킬 UI로 전달하지 않는다. 강퇴 확인 창은 별도 모달 상태이므로 빈 스냅샷 및 파티 페이지 전환과 독립적이다.

## 변경 파일

* C:/Users/KimHyeongJin/source/repos/ActionRPGClient/ActionRPGClient/ActionRPGClient/Network/TownPacket.generated.h
* C:/Users/KimHyeongJin/source/repos/ActionRPGClient/ActionRPGClient/ActionRPGClient/Network/TownProtocol.h
* C:/Users/KimHyeongJin/source/repos/ActionRPGClient/ActionRPGClient/ActionRPGClient/Network/TownProtocol.cpp
* C:/Users/KimHyeongJin/source/repos/ActionRPGClient/ActionRPGClient/ActionRPGClient/Network/TownClient.h
* C:/Users/KimHyeongJin/source/repos/ActionRPGClient/ActionRPGClient/ActionRPGClient/Network/TownClient.cpp
* C:/Users/KimHyeongJin/source/repos/ActionRPGClient/ActionRPGClient/ActionRPGClient/Game/GameWorld.h
* C:/Users/KimHyeongJin/source/repos/ActionRPGClient/ActionRPGClient/ActionRPGClient/Game/GameWorld.cpp
* C:/Users/KimHyeongJin/source/repos/ActionRPGClient/ActionRPGClient/ActionRPGClient/Game/SkillUi.h

## 검증 및 남은 한계

* 교체 전 파일 해시를 검사하고 지정 8개 파일에만 반영했다. 반영 후 해시가 검토한 출력과 일치함을 확인했다.
* 서버/클라이언트 신규 12개 코덱 정의, 외부 enum, 생성 헤더를 정적으로 대조했다. 선언/정의 및 TownClient→TownEvent→GameWorld 처리를 확인했다.
* 소스 괄호, 입력 enum 사용, 요청 ID 정렬·중복·종료 제거, 강퇴 표시 근거와 렌더/입력 순서를 정적으로 검사했다.
* 반영 전 이동·원격 플레이어·전투·맵/던전·클리어 처리의 12개 보호 함수가 동일함을 확인했다. Player.cpp의 GameplayMap include 및 D2DRenderer.cpp의 d2d1effects_2.h 수정도 보존했다. 기존 성장·전투·마을 이동·이미지 변경은 건드리지 않았다.
* git diff --check 통과. C++ 컴파일 및 링크는 미확인이다. 실제 다중 클라이언트 승인/거절, 연속 대기, 해산·파티장 변경·던전 전환·재접속·강퇴 입력 동작은 실행 미확인이다.
* 상세는 조회 시점의 정보다. 서버가 실제 가입/승인 때 다시 검증한다. 가입 처리의 권위 판정은 UI의 사전 확인으로 대체하지 않았다.
* 신규 상세/알림 배치는 640×360에서 8명 및 버튼이 들어오도록 소스 계산을 확인했다. 기존 파티 목록/자기 파티 패널은 최소 높이가 560으로 작은 창에서 화면을 넘는 문제가 있으며, 별개의 기존 배치는 이번에 수정하지 않았다. 화면 렌더 결과는 미확인이다.
* 클라이언트는 던전 입장 요청 직후 WaitingRoom부터 선제적으로 파티/습득 UI를 막는다. 서버의 성장 InDungeon은 실제 참여/예약이 기준이다. UI에서 가린 초대는 로컬 표시만 정리하며 자동 수락/거절 패킷을 보내지 않는다.

정적 확인 스크립트: `C:/Users/KimHyeongJin/source/repos/ActionRPGServer/artifacts/party-join-20261004/check_static.py`, `verify_installed.py`. 기능 테스트가 아니다.

## 제안 커밋 로그

* 던전 UI 제한과 파티 상세·가입 승인·강퇴 알림 연결
  * 공개 파티 상세 조회와 서버 승인 전 가입 요청 대기 처리 추가
  * 파티장 요청 ID 순 승인·거절 알림 및 종료·전환 시 큐 정리
  * 강퇴 대상의 중앙 확인 창과 입력 전달 차단 추가
  * 확정 패킷 30~35와 enum·코덱·게임 스레드 이벤트 연계
  * 던전 파티 초대 우회 차단 및 스킬 조회·단축키 등록 유지

제안 로그이며 실제 커밋/푸시는 수행하지 않았다.
