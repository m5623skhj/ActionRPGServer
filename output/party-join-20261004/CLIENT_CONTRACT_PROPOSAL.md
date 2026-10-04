# 파티 연계 적용 계약

2026-10-04. 이 파일은 확정된 서버 계약을 기록한다. 현재 클라이언트 구현은 아래 기준을 적용 완료했다.

기준: `C:/Users/KimHyeongJin/source/repos/ActionRPGServer/output/party-join-20261004/SERVER_REPORT.md` 및 서버 `Tool/TownPacketDefine.yml`, `ActionRPGServer/TownServer/Protocol.h/.cpp`.

* 30 PartyDetailRequest: 선택 partyId로 실제 상세 조회.
* 31 PartyDetailResponse: partyId/result/title/leaderPlayerId/isPublic/busy/members.
* 32 PartyJoinRequest: partyId로 가입 요청. 실제 가입은 하지 않음.
* 33 PartyJoinAnswer: requestId/accepted로 파티장 승인·거절.
* 34 PartyJoinRequestUpdate: requestId/partyId/requesterPlayerId/requesterName/state/result.
* 35 PartyKicked: partyId/leaderPlayerId. 실제 강퇴된 본인에게만 전달.

state는 Pending1/Accepted2/Rejected3/Invalidated4, operation은 RequestJoin7/AnswerJoin8, result 최대값은 NotPublic14다. 상세 응답은 현재 선택 ID와 대조한다. 대기 큐는 requestId 순서로 중복 없이 표시하며 종료 이벤트에서 정확한 ID를 제거한다. 신규 취소 요청 또는 전체 큐 스냅샷 패킷은 없다. 빈 PartySnapshot으로 강퇴를 추측하지 않는다.

상세 결과·필드 순서·구현 파일·정적 확인·실행 미검증은 `C:/Users/KimHyeongJin/source/repos/ActionRPGServer/output/party-join-20261004/CLIENT_HANDOFF.md`에 기록했다. 서버 원본 스키마/서버 소스는 클라이언트 담당이 수정하지 않았다.
