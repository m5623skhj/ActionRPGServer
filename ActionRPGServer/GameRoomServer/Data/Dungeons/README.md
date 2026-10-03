# 던전 데이터 설치

던전 편집기의 ZIP을 이 폴더 아래 던전별 폴더에 풀어 놓습니다.
예: `Data/Dungeons/ForgottenHall/Dungeon.json`, `Data/Dungeons/ForgottenHall/Maps/*.json`.

- 편집기의 **서버 던전 Data ID**를 TownServer의 `DungeonCatalog.json` 숫자 ID와 맞춥니다.
  ForgottenHall은 1입니다. 동일한 Data ID를 중복 설치할 수 없습니다.
- 현재 출력 형식은 manifest version 3 / DungeonRoom version 5입니다. 기존 작업 파일을
  편집기로 열어 새로 출력하세요.
- ZIP의 `Assets` 내용은 클라이언트 `Assets` 폴더에 같은 상대 경로로 복사합니다.
- 빌드 시 Data 폴더가 실행 파일 옆으로 복사됩니다. 이미 빌드한 서버를 사용한다면
  실행 파일 옆 `Data/Dungeons`에 직접 설치하고 서버를 재시작하세요.
- 서버가 시작할 때 파일을 읽고 검사합니다. 손상된 던전은 로그에 사유를 남기고 제외합니다.
  설치되지 않은 던전의 입장은 실패합니다.
- 방 이름·종류·미니맵 배치·연결·미니맵 메타데이터는 던전 정의에 보관합니다.
  방 ID·미니맵 위치 중복, 없는 목적지, 연결 방향과 게이트 불일치, 반대편이 아닌 도착점,
  필요한 게이트 누락, 최초 방에서 도달할 수 없는 방을 검사합니다. 여러 게이트와 순환 연결은 가능합니다.

몬스터 배치 Data ID는 `Data/Monsters/MonsterCatalog.json`에 등록되어 있어야 합니다.
각 게임 방에서 배치 목록을 복제하여 몬스터 instance ID를 생성하고, 참조한 정의의
최대 HP를 현재 HP와 최대 HP에 적용합니다. 초기 AI 노드도 개체별로 보관합니다.
기본 Dummy(monster Data ID 1)의 최대 HP는 `Dummy.ai.json`에 100으로 설정되어 있습니다.
AI 그래프 실행과 기본 전투 판정 코드는 추가되었으며, `Data/Combat.json`과 실제 AI·스킬 정의,
클라이언트 패킷 연결이 필요합니다. 드랍·보상 지급은 별도 기능입니다.
FallenCitadel(Data ID 5)은 방 6개·병사 15마리·보스 1마리의 원본과 Debug·Release 데이터를 설치했고,
몬스터 AI·전투 설정·클라이언트 입력/표시를 통합했습니다. 빌드와 실제 플레이 확인은 하지 않았습니다.
실행 계약은 [전투 연결 안내](../../COMBAT_PROTOCOL.md)를 참고하세요.
등록 절차는 [몬스터 정의 안내](../Monsters/README.md)를 참고하세요.
입장한 모든 파티원에게 같은 instance ID와 배치 정보가 전달됩니다.
월드 JSON은 4MB 이하이며, 인증된 연결에 요청/응답 방식으로 분할 전송됩니다.
방별 이동과 워프 판정은 서버의 방 strand에서 처리합니다.
현재 방의 모든 몬스터 HP가 0이 된 뒤에만 출구 워프를 허용합니다. 몬스터가 없는 방은 바로 이동 가능합니다.
같은 던전 인스턴스에서는 방 재방문 시 몬스터를 재생성하거나 HP를 초기화하지 않습니다.
