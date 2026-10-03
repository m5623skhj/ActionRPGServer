# ActionRPGServer

이 저장소에는 용도가 다른 두 서버 프로젝트가 있습니다.

- `TownServer`: 마을 클라이언트 TCP와 GameRoomServer 내부 제어 TCP를 함께 관리합니다.
- `GameRoomServer`: 여러 던전 룸과 `MultiSocketRUDP` 세션을 운영하고 TownServer에 능동적으로 연결합니다.

## 준비 사항

Visual Studio의 C++ 도구 집합(`v145`), Windows SDK, vcpkg를 사용합니다. `TownServer`의 Asio와 JSON 의존성은 저장소의 `vcpkg.json`에 정의되어 있습니다.

`GameRoomServer`를 빌드하려면 `External/MultiSocketRUDP` 서브모듈이 필요합니다. 새로 체크아웃했다면 다음 명령을 실행합니다.

```powershell
git submodule update --init --recursive
```

상위 라이브러리의 테스트 의존성을 제외하고 서버 빌드에 필요한 `CommonCode`만 가져오려면 다음 명령을 사용합니다.

```powershell
git submodule update --init External/MultiSocketRUDP
git -C External/MultiSocketRUDP submodule update --init external/CommonCode
```

`TownServer` 자체에는 이 서브모듈이 필요하지 않습니다.

## TownServer 빌드와 실행

`ActionRPGServer/ActionRPGServer.slnx`를 Visual Studio에서 열고 `Debug | x64` 또는 `Release | x64`로 `TownServer` 프로젝트를 빌드합니다. Debug 솔루션 빌드의 실행 파일은 `ActionRPGServer/x64/Debug/TownServer.exe`입니다.

```powershell
ActionRPGServer/TownServer/x64/Debug/TownServer.exe 7777 4 7780
```

인자는 순서대로 클라이언트 TCP 포트, I/O 스레드 수, GameRoomServer 제어 TCP 포트이며 생략할 수 있습니다. 마을의 공유 상태는 I/O 스레드 수와 관계없이 하나의 strand에서 직렬화됩니다.

서버는 시작할 때 **실행 파일 옆**의 `Data/TownMap.json`을 읽습니다. 빌드 시 프로젝트의 `ActionRPGServer/TownServer/Data/TownMap.json`이 실행 폴더로 복사됩니다. 맵을 수정했다면 실행 폴더의 파일도 갱신하고 서버를 재시작해야 합니다.

현재 TownServer의 주요 기능은 다음과 같습니다.

- 길이 정보가 앞에 붙는 TCP 패킷과 세션별 비동기 송신 큐
- 서버 권위의 20Hz 이동 처리와 이동 중 10Hz 위치·속도 전송
- 이동 가능·진입 금지 다각형을 이용한 이동 보정과 마을 내 걷기 전용 이동
- 현재 맵의 1280×720 직사각형 섹터 및 자신을 포함한 3×3 섹터의 등장·퇴장 관리
- 이동 입력 시간 초과와 세션 송신 대기열 제한

로컬에서 TownServer·GameRoomServer와 클라이언트 두 개를 함께 실행하려면 세 프로젝트를 `Debug | x64`로 빌드한 뒤 저장소 루트의 `RunLocalTest.bat`을 실행합니다. 배치 파일은 TCP 7777 또는 7780 포트가 이미 사용 중이면 시작하지 않습니다. TownServer의 접속 대기 확인 후 GameRoomServer 한 개와 클라이언트 두 개를 실행합니다. 서버와 클라이언트는 패킷 형식을 공유하므로 프로토콜이 바뀌면 양쪽을 다시 빌드해야 합니다.

콘텐츠 및 패킷을 추가하는 절차는 [TownServer 개발 가이드](ActionRPGServer/TownServer/DEVELOPMENT.md)를 참고합니다.

## GameRoomServer 빌드와 실행

`GameRoomServer`는 `MultiSocketRUDP` 서버 코어와 `Logger`를 C++ 프로젝트로 참조합니다. x64 빌드 결과는 `artifacts/bin/x64/<Configuration>/`, 중간 파일은 `artifacts/obj/`에 저장됩니다. `Directory.Build.targets`가 이 저장소의 경로에 맞춰 상위 라이브러리의 include 경로를 조정하며 서브모듈 소스는 수정하지 않습니다.

```powershell
msbuild ActionRPGServer/GameRoomServer/GameRoomServer.vcxproj /m /p:Configuration=Debug /p:Platform=x64
```

TownServer를 먼저 실행한 뒤 다음과 같이 GameRoomServer를 실행합니다.

```powershell
artifacts/bin/x64/Debug/GameRoomServer.exe 127.0.0.1 7780 1 1000 4
```

인자는 순서대로 TownServer 주소, 제어 포트, 룸 서버 ID, 최대 룸 수, Asio I/O 스레드 수입니다. 모두 생략할 수 있습니다. 서로 다른 GameRoomServer는 고유한 룸 서버 ID를 사용해야 합니다.

여섯 번째와 일곱 번째 인자로 RUDP 코어 옵션 파일과 세션 브로커 옵션 파일 경로를 지정할 수 있습니다. 여러 GameRoomServer를 같은 호스트에서 실행할 때는 각 프로세스가 서로 다른 `SESSION_BROKER_PORT`를 가진 옵션 파일을 사용해야 합니다. 세션 브로커 옵션은 UTF-16 LE BOM 형식이어야 합니다.

```powershell
artifacts/bin/x64/Debug/GameRoomServer.exe 127.0.0.1 7780 2 1000 4 D:/Config/Room2Core.txt D:/Config/Room2Broker.txt
```

GameRoomServer는 다음 기능을 포함합니다.

- TownServer에 등록하고 생성 요청을 받는 길이 프레임 기반 내부 TCP 채널
- 룸별 Asio strand, 20Hz 틱, 참가 인원 확정용 30초 입장 제한 시간
- 참가 예정 인원과 실제 입장 인원의 분리 관리
- RUDP 연결 직후 challenge 발급 및 TownServer TCP 세션을 통한 유저 확인
- GameRoomServer ID가 포함된 전역 고유 룸 ID와 공유 전투 시드
- 클리어 순간 남아 있는 유저를 기준으로 한 보상 대상 전달
- 던전 편집기에서 출력한 방·지형·게이트·몬스터 배치 로딩
- 방 종류·미니맵 배치·연결 정보 보관 및 실제 게이트 방향·도착점 검증
- 몬스터 편집기 JSON 로딩·그래프 검증 및 숫자 Data ID 카탈로그 연결
- 던전별 몬스터 instance ID 생성 및 인증된 클라이언트에 월드 데이터 분할 전달
- 서버 플레이어 이동과 워프 판정, 방별 몬스터 표시를 위한 상태 전달

던전 편집기의 서버 던전 Data ID는 TownServer의 `Data/DungeonCatalog.json` 숫자 ID와
일치해야 합니다. ZIP은 `ActionRPGServer/GameRoomServer/Data/Dungeons/<던전폴더>/`에 풀고,
ZIP 안 `Assets`는 클라이언트 Assets에 복사합니다. 빌드 시 서버 Data 폴더도 실행 폴더로
복사되며, 서버 재시작 후 적용됩니다. 아직 설치하지 않은 던전의 입장은 실패합니다.
자세한 형식과 설치 경로는 [던전 데이터 안내](ActionRPGServer/GameRoomServer/Data/Dungeons/README.md)를 참고하세요.

몬스터 정의는 `Data/Monsters/MonsterCatalog.json`을 통해 등록합니다.
기본 Dummy는 Data ID 1이며, 던전 개체의 최대 HP와 초기 AI 상태는 정의 JSON에서 가져옵니다.
AI 그래프 실행과 기본 전투 판정 코드, ID 2·3의 최소 추적·공격 패턴을 추가했습니다.
클라이언트 전투 패킷·상태 표시 연결과 FallenCitadel 데이터 설치를 통합했습니다. 드랍·보상 지급은 아직 구현하지 않았습니다.
정적 확인만 수행했으며, 기존 실행 파일에는 새 소스의 빌드가 필요합니다. 실제 입장·전투 동작은 검증하지 않았습니다.
담당별 결과와 설치 기록은 [통합 보고서](output/combat-integration/INTEGRATION_REPORT.md)에 정리했습니다.
서버 전투 설정과 연결 계약은 [전투 연결 안내](ActionRPGServer/GameRoomServer/COMBAT_PROTOCOL.md)를 참고하세요.
설치와 검증 규칙은 [몬스터 정의 안내](ActionRPGServer/GameRoomServer/Data/Monsters/README.md)를 참고하세요.

RUDP 서버를 시작하려면 `MY/DevServerCert` 개발 인증서가 로컬 인증서 저장소에 설치되어 있어야 합니다. 빌드 시 기본 옵션 파일이 실행 폴더의 `ServerOptionFile`로 복사됩니다.

## MultiSocketRUDP 버전 갱신

`External/MultiSocketRUDP`에서 검증할 커밋을 체크아웃하고 해당 서브모듈의 `CommonCode`를 갱신한 뒤 `GameRoomServer`를 다시 빌드합니다. 이후 변경된 서브모듈 포인터를 이 저장소에 커밋합니다. 라이브러리 소스를 게임 콘텐츠 디렉터리로 복사하지 않습니다.
