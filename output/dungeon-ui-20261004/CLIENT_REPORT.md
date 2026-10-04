# 던전 파티·스킬 UI 제한 클라이언트 결과

작성: 2026-10-04. 기존 승인된 UI 제한을 반영했으며, 파티 상세/가입 연계와 함께 기존 초대 입력 우회도 보완했다.

## 요구별 확인

| 요구 | 구현 파일/함수 | 결과 |
|---|---|---|
| 던전에서 파티 메뉴 회색 및 선택 차단 | GameWorld.cpp: RenderSystemInterface, UpdateSystemInterface, SetSystemUiPage; GameWorld.h: IsDungeonUiRestricted/IsPartyUiPage | 기존 PNG를 DrawUiIcon의 grayscale 경로로 표시, hover 강조 해제, 메뉴 클릭과 페이지 전환 차단. 파티/목록/생성/상세 모두 제한 대상. |
| 열린 파티 창 및 초대 우회 차단 | GameWorld.cpp: Update, UpdatePartyInterface, RenderPartyInterface | 던전 상태에서 기존 페이지는 메뉴로 정리. 초대 수락/거절보다 제한 검사 우선. 남은 초대와 가입 요청 표시는 정리하며 자동 답변은 보내지 않음. |
| 스킬 창 조회와 단축키 등록 유지 | GameWorld.cpp: UpdateSystemInterface/SetSystemUiPage/Update; SkillUi.cpp: Update/Render | Skills 메뉴는 사용 가능. 배운 스킬의 드래그 및 슬롯 등록·교체·해제 유지. 스킬 사용/hotkey 판단에 learningAllowed를 추가하지 않음. |
| 습득·강화만 회색 비활성화 | SkillUi.h: SetLearningAllowed; SkillUi.cpp: LearnReason/Update/Render; GameWorld.cpp: LearnSkill 송신 조건 | learningAllowed가 false이면 습득/강화 클릭과 송신 차단. 트리·상세의 습득 대상 아이콘과 버튼은 기존 그림의 회색 처리. 스킬 바 사용 아이콘의 판정은 유지. |
| 서버 거절 사유 | SkillUi.cpp: Reason | 기존 SkillStateResponse의 result=InDungeon을 ‘던전에서는 스킬을 습득·강화할 수 없습니다’로 표시. 신규 성장 wire 변경 없음. |
| 퇴장 복구 | GameWorld.cpp: Update; GameWorld.h: IsDungeonUiRestricted | dungeonEntryState가 Idle이면 매 프레임 제한 해제. 파티 메뉴 및 습득/강화는 연결·SP·레벨·선행 조건 등 정상 판정으로 복귀. |

관련 파일의 실제 루트는 `C:/Users/KimHyeongJin/source/repos/ActionRPGClient/ActionRPGClient/ActionRPGClient/`이며 Game/GameWorld.h/.cpp, Game/SkillUi.h/.cpp에서 구현했다. 파티 알림 전체 인계는 `C:/Users/KimHyeongJin/source/repos/ActionRPGServer/output/party-join-20261004/CLIENT_HANDOFF.md`를 참고한다.

## 제한의 기준과 검증

클라이언트는 WaitingRoom(입장 요청 직후)부터 Idle로 복귀할 때까지 보수적으로 차단한다. 서버 성장 로직은 dungeonRoomId 또는 reservedDungeonRoomId를 기준으로 InDungeon을 반환한다. 그 차이를 임의 wire 변경 없이 유지했다. 파티 가입/승인은 서버가 입장 대기까지 Busy로 검증한다.

UI/요청 상태는 기존 게임 스레드 이벤트 처리에서만 변경한다. 네트워크 콜백에 새 UI 쓰기를 넣지 않았다. 기존 이미지 재생성은 없다. 입력 경로의 중앙 제한과 실제 송신 조건, 회색 렌더, 정상 복귀 조건 및 스킬 사용/등록 경로 보존을 소스에서 확인했다. git diff --check 통과. 빌드·테스트·게임/서버 실행은 수행하지 않았으며 화면과 복귀의 실제 동작은 미확인이다.

## 제안 커밋 로그

* 던전 참여 중 파티 조작과 스킬 습득·강화 제한
  * 파티 메뉴 회색 표시 및 기존 창·초대의 조작 우회 차단
  * 스킬 조회·단축키 등록을 유지하고 습득·강화만 비활성화
  * InDungeon 안내 및 마을 복귀 시 정상 조건 복구

실제 stage/commit/push는 하지 않았다.
