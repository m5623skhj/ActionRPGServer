# 스킬 창·습득·단축키 클라이언트 인계

작성: 2026-10-04 / 클라이언트 담당. 승인된 구현을 클라이언트 소스24개 파일에 적용했다. 이전의 UI 결정 전용 보고서를 이 구현 보고서로 대체한다. 기존 조작감·앞차기 미커밋 변경은 보존했다. 빌드·게임/서버 실행·기능 테스트·백업 생성·stage/commit/push·타 대화 직접 메시지는 수행하지 않았다.

## 구현과 사용 흐름

- ESC → 스킬 메뉴에서 현재 캐릭터의 모든 트리를 표시한다. 미습득·레벨 부족·선행 조건 부족 스킬도 표시한다. 노드 클릭으로 설명, 현재/다음 공격력, 쿨타임, 요구 캐릭터 레벨, SP, 선행 스킬 최소 단계, 커맨드·지상/공중 조건을 확인한다.
- 습득/강화 버튼은 로컬 조건 확인 후 현재 단계를 expectedSkillLevel로 서버에 전송하고 응답으로 갱신한다. 대기 중 중복 클릭을 막는다. 10초 응답 대기 후에는 상태만 재요청하며 습득 요청을 재전송하지 않는다.
- 습득한 스킬만 A/S/D/F/G/H 슬롯에 드래그한다. 동일 스킬은 한 슬롯, 목적지 교체, 기존 슬롯 이동, 오른쪽 클릭 해제, 바깥 드롭 취소를 구현했다. 메뉴·포커스/캡처·크기·피격·맵 변경·방 리셋에서 드래그를 취소한다.
- 키다운1회만 사용한다. 메뉴·파티·초대·던전 선택·클리어 처리 중 입력을 막으며 ESC로 닫는 프레임에도 발동하지 않는다. 메뉴가 처리한 클릭은 uiClickConsumed 확인으로 새 스킬 창에 재전달하지 않는다.
- 사용할 수 없는 스킬 입력은 즉시 버린다. 이전 pendingCommandSkill 250ms 예약을 삭제했으며 기본 공격·점프 버퍼와5회 공격 제한은 유지했다. 미습득 커맨드의 마지막 X/C 입력이 일반 공격/점프로도 실행되지 않도록 매칭을 소비한다.
- 마을은 습득 권한을 확인한 로컬 모션/쿨타임, 던전은 서버 승인과 reliable JSON 잔여 시간을 표시한다. 같은 스킬의 커맨드와 단축키는 같은 쿨타임을 쓴다. 회색+잔여 시간은 실제0까지 유지한다. 60초 미만은 올림한 소수점 한 자리 `x.x초`, 60초 이상은 내림한 정수 분(min1) `1m`, `2m` 형식이다.
- 로컬 배치는 `%LOCALAPPDATA%/ActionRPGClient/skill-slots-Character{id}.json`에 저장한다. SkillSlots/version1, 슬롯6개, 최대8KB이며 중복·다른 캐릭터·미습득 스킬을 거른다. 프로세스 ID 임시 파일과 RAII 정리/원자 교체를 사용한다. 동일 캐릭터를 여러 프로세스에서 편집하면 마지막 저장이 남는다. 레벨·SP·습득 정보는 저장하지 않는다.
- 연결 종료와 새 Town 진입 응답에서 기존 습득 권한을 무효화한다. 새 세션에 같은 캐릭터 배치 파일이 남아 있어도 미습득 스킬은 등록/사용할 수 없다.

## 적용 계약과 상태 순서

- PlayerSkills/schemaVersion1, SkillTrees/schemaVersion1을 사용한다. 프런트 PlayerSkills와 Town 응답 카탈로그의 JSON 의미 값이 일치해야 한다. PlayerSkillCatalog.h/CharacterProgression.h/SkillTreeCatalog.h는 서버 Shared와 바이트 일치를 확인했다. 총괄 소유인 공통 헤더·YAML·스키마·TownProtocol 구현은 이 작업에서 수정하지 않았다.
- Town 생성 패킷 헤더도 서버와 바이트 일치한다. SkillStateRequest=27, LearnSkillRequest=28(skillId, expectedSkillLevel), SkillStateResponse=29(payload)를 기존 Encode/Decode로 TownClient에 연결했다.
- Town payload는 characterId/progression/skillTrees/playerSkills/result이며 progression은 level/skillPoints/skillLevels이다. **성장 권위는 Town SkillStateResponse**에 둔다. 별도 소켓의 오래된 Room JSON이 새 습득·SP·레벨을 되돌리지 않도록 Room progression은 파싱/검증한 전투 정보로만 유지한다. SkillUi는 Room cooldowns/combatReady만 반영한다. LevelAdvanced는 한국어 상태로 표시한다.
- 서버는 접속 캐릭터의 성장만 메모리에서 관리한다. 초기Lv1/SP0, 레벨 상승당200SP, 다음 단계 요구 캐릭터 레벨=기본 요구+2×(다음 단계−1), 단계마다 같은 SP 비용이다. 앞차기는 요구1/비용20/기본 공격력25+단계당5, 지상 전용, maxSkillLevel=null이다. 경험치·클라이언트 레벨 조작 패킷·영구 프로필을 추가하지 않았다. 초기SP0에서는 바로 습득할 수 없으며 실제 레벨 상승 호출은 서버 AdvancePlayerLevel과 연결해야 한다.
- 전투 JSON의 level/skillPoints/skillLevels/skillCooldowns를 파싱한다. **realtime SKL1 wire는 그대로 유지**했다. 바이너리 hasSkillState=false는 기존 쿨타임을 빈 맵으로 덮어쓰지 않는다.
- GameWorld는 reliable JSON의 스킬 상태를 별도 monotonic tick 경로에서 적용한다. 더 최신 realtime 위치가 표시 중이어도 쿨타임은 갱신한다. 이동/HP에는 기존 realtime 우선 규칙을 적용한다. 방/맵/epoch/로컬 플레이어를 확인하며 마지막 승인 스킬의 actionSequence보다 이전 JSON은 쿨타임에 적용하지 않는다.
- 승인 DungeonActionResult의 sequence→skillId로 표시를 시작하고 JSON refresh를 요청한다. 게임 스레드 플래그로 요청을 합치고 기존 청크 전송 완료 후 새 스냅샷을 요청한다. 동시 JSON 전송/wire 변경은 없다. TCP는 기존 mutex 이벤트 큐로 GameWorld에 전달하며 UI/렌더 리소스를 직접 변경하지 않는다.

## UI 크기와 데이터

기본 클라이언트1280×720, 현재 D2D96DPI 코드 기준이다. 실제 실행 창/DPI를 측정한 결과는 아니다. UI 수치·폰트·색은 Assets/Data/SkillUi.json 필수 설정으로 관리한다. Application.cpp/.h·명령행·RunLocalTest는 수정하지 않았다. GameWindow 게임 스레드 API가 포인터 edge를 고정 업데이트에 전달하고 JSON 최소640×360을 WM_GETMINMAXINFO에 반영한다.

|항목|기본|작은 창(폭<1100 또는 높이<640)|
|---|---:|---:|
|슬롯/아이콘|64×80 /56×56|56×72 /48×48|
|키/쿨타임 글자|14 /18|14 /16|
|간격/내부/하단 여백|8 /8 /16|6 /8 /12|
|6슬롯 바|440×96|382×88|
|트리 카드/아이콘|96×96 /56|80×80 /48|
|상세 아이콘|96×96|96×96|

패널 최대960×552를 바 위 가용 영역에 맞춘다. 기본 외곽32/바 간격24/내부20, 작은 창 외곽12/바 간격12/내부12이다. 폭960 미만은 트리/상세 탭을 사용한다. 휠 세로·Shift+휠 가로 스크롤과 크기 변경 후 범위 보정을 제공한다. **640×360 탭 본문은84px**이므로 compact 카드80px를 사용한다. 이전 문서의124px/96px 카드 가정은 수정했다.

|클라이언트|바 x,y /크기|패널 x,y /크기|본문 높이|
|---|---|---|---:|
|1920×1080|740,968 /440×96|480,212 /960×552|408|
|1280×720|420,608 /440×96|160,32 /960×552|408|
|1024×768|321,668 /382×88|32,58 /960×552|440|
|960×540|289,440 /382×88|12,12 /936×416|304|
|800×450|209,350 /382×88|12,12 /776×326|174|
|640×360|129,260 /382×88|12,12 /616×236|84|

스킬 편집 중 바는 오버레이 위에 표시하고 다른 모달에서는 숨긴다. 고정 좌상단HUD(24,24)..(360,128)와 기본 바는 겹치지 않는다. 게임의 미니맵 표시 경로는 찾지 못했으며 우상단208×208/여백16 예약은 향후 배치 기준이다. 미니맵을 추가하지 않았다. 기존 파티 창 최소 높이560의 작은 창 넘침은 별도 문제로 유지했다.

## 그래픽

총괄이 설치한 `Images/UI/Skills/character1_front_kick.png`를 SkillTrees icon 경로로, `Images/UI/system_menu_skills.png`를 assets.ini의 SystemMenuSkills로 연결했다. 두 PNG 실제1254×1254 RGBA8이다. 원본128 권장·엄격한 투명 여백·sRGB 메타데이터는 충족했다고 보고하지 않는다. 자세한 제약은 output/skill-tree-ui-20261004/graphics/GRAPHICS_REPORT.md에 있다.

- 앞차기 SHA256:7a85ce173e15f0807821f5be38d82e6034316b6df87a6a3461b5518c2a0e4d99
- 메뉴 SHA256:8131e34cddb6b3592c8d4c09353a003edb8cc19532c5a5b24db7dfd7fcebe56a
- 전체 캔버스를56/48/96/메뉴128 사각형에 선형 축소한다. 월드 nearest는 유지한다. 재사용 D2D grayscale effect와 별도 키/숫자/테두리를 사용한다. initguid로 새 effect GUID 정의를 제공한다.
- 이미지 누락/실패 시 스킬 이름 placeholder/메뉴 라벨을 유지한다. 미습득 제한은 그대로 적용한다. 이미지 생성/편집은 수행하지 않았다.

## 변경 파일

이 구현의 적용24개 파일(클라이언트 저장소 상대 경로):

- ActionRPGClient/ActionRPGClient/Game/SkillUi.h, SkillUi.cpp (신규)
- ActionRPGClient/ActionRPGClient/Game/GameWorld.h, GameWorld.cpp
- ActionRPGClient/ActionRPGClient/Game/Player.h, Player.cpp
- ActionRPGClient/ActionRPGClient/Game/PlayerSkillPresentation.h, PlayerSkillPresentation.cpp
- ActionRPGClient/ActionRPGClient/Game/DungeonCombat.h, DungeonCombat.cpp
- ActionRPGClient/ActionRPGClient/Input/InputState.h
- ActionRPGClient/ActionRPGClient/Platform/GameWindow.h, GameWindow.cpp
- ActionRPGClient/ActionRPGClient/Graphics/D2DRenderer.h, D2DRenderer.cpp
- ActionRPGClient/ActionRPGClient/Network/TownClient.h, TownClient.cpp
- ActionRPGClient/ActionRPGClient/Network/DungeonClient.h, DungeonClient.cpp
- ActionRPGClient/ActionRPGClient/ActionRPGClient.vcxproj, ActionRPGClient.vcxproj.filters
- ActionRPGClient/Assets/Data/SkillUi.json (신규), assets.ini, system_menu.ini

공통 헤더·프로토콜·SkillTrees.json·PNG는 총괄 변경에 의존한다. 기존 조작감 Character/InputCommandQueue/앞차기 데이터·이미지는 위 목록에 포함하지 않았으며 보존했다. ignored artifacts/skill-ui/manifest.json에 적용 전/후 SHA와 파일 목록을 기록했다. 원본 백업은 없다. 기존 CopyRuntimeAssets Build 타깃이 새 JSON/PNG도 포함하므로 별도 런타임 복사/실행 파일 갱신은 하지 않았다.

## 정적 확인과 미검증

정적 확인: 적용 파일 SHA 대조, C++ 괄호/중복 include, 프로젝트 XML/중복 등록, JSON 구문과6개 크기의 좌표, 선언/호출 연결, 공통 헤더/생성 패킷 바이트 일치, YAML27~29 필드 대응, SKL1 유지, 게임 스레드 이벤트/포인터/refresh 소유 흐름, PNG 해시. Git diff --check는 CR-at-EOL 인정 설정으로 통과했다. 최종 클릭 소비는 호출 직전1줄로 추가했다.

미검증: C++ 컴파일/링크, 실제 창/DPI·폰트 폭·effect 지원, 긴 설명 스크롤/클리핑, 드래그·교체·바깥 취소·포커스, 초기SP0/레벨 상승/습득·중복·서버 거절, 마을·던전 쿨타임·ACK/JSON 지연, 클리어/복귀/재도전/재연결의 실제 동작. 별도 승인 후 빌드/기능 확인이 필요하다. 현재 **소스 통합 완료**이며 런타임 정상 동작 확인을 뜻하지 않는다.

## 제안 커밋 로그

* 스킬 트리 습득 창과 캐릭터별 단축키 구현
  * 전체 트리·조건·공격력·SP 상세와 서버 습득/강화 응답 연결
  * A·S·D·F·G·H 드래그 배치·교체·해제·로컬 저장 추가
  * 커맨드·단축키 공통 쿨타임과 회색·잔여 시간 표시 구현
  * 사용 불가 스킬 예약 제거 및 모달·연결 리셋의 권한 보호
  * 작은 창 탭·스크롤·데이터 기반 크기·선형 아이콘 표시 추가
  * Town 성장 권위와 Room 쿨타임 분리 및 소비한 메뉴 클릭 재처리 방지

제안 로그이며 실제 stage/commit/push는 하지 않았다. 기존 skill_ui.proposed.ini는 미설치 초기 규격 초안이다. 런타임 설정은 SkillUi.json이다.
