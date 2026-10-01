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

각 게임 방에서 배치 목록을 복제하여 몬스터 instance ID를 생성합니다. 현재 Dummy
(monster Data ID 1)는 HP 100인 고정 표적이며 AI·공격·드랍·전투 판정은 별도 기능입니다.
입장한 모든 파티원에게 같은 instance ID와 배치 정보가 전달됩니다.
월드 JSON은 4MB 이하이며, 인증된 연결에 요청/응답 방식으로 분할 전송됩니다.
방별 이동과 워프 판정은 서버의 방 strand에서 처리합니다.
