# ActionRPGServer

이 저장소에는 용도가 다른 두 서버 프로젝트가 있습니다.

- `TownServer`: 마을의 접속, 이동, 다른 플레이어의 등장·퇴장을 처리하는 독립 TCP 서버입니다. `MultiSocketRUDP`를 사용하지 않습니다.
- `GameRoomServer`: 던전 서버용 `MultiSocketRUDP` 연결을 준비한 프로젝트입니다. 현재는 코어 생성과 링크만 확인하며 접속을 받지 않습니다.

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
ActionRPGServer/x64/Debug/TownServer.exe 7777 4
```

인자는 순서대로 TCP 포트와 I/O 스레드 수이며 생략할 수 있습니다. 마을의 공유 상태는 I/O 스레드 수와 관계없이 하나의 strand에서 직렬화됩니다.

서버는 시작할 때 **실행 파일 옆**의 `Data/TownMap.json`을 읽습니다. 빌드 시 프로젝트의 `ActionRPGServer/TownServer/Data/TownMap.json`이 실행 폴더로 복사됩니다. 맵을 수정했다면 실행 폴더의 파일도 갱신하고 서버를 재시작해야 합니다.

현재 TownServer의 주요 기능은 다음과 같습니다.

- 길이 정보가 앞에 붙는 TCP 패킷과 세션별 비동기 송신 큐
- 서버 권위의 20Hz 이동 처리와 이동 중 10Hz 위치·속도 전송
- 이동 가능·진입 금지 다각형을 이용한 이동 보정과 마을 내 걷기 전용 이동
- 현재 맵의 1280×720 직사각형 섹터 및 자신을 포함한 3×3 섹터의 등장·퇴장 관리
- 이동 입력 시간 초과와 세션 송신 대기열 제한

로컬에서 서버와 클라이언트 두 개를 함께 실행하려면 두 프로젝트를 `Debug | x64`로 빌드한 뒤 저장소 루트의 `RunTownLocalTest.bat`을 실행합니다. 배치 파일은 TCP 7777 포트가 이미 사용 중이면 시작하지 않습니다. 서버와 클라이언트는 입장 패킷 형식을 공유하므로 프로토콜이 바뀌면 둘 다 다시 빌드해야 합니다.

콘텐츠 및 패킷을 추가하는 절차는 [TownServer 개발 가이드](ActionRPGServer/TownServer/DEVELOPMENT.md)를 참고합니다.

## GameRoomServer 빌드 상태

`GameRoomServer`는 `MultiSocketRUDP` 서버 코어와 `Logger`를 C++ 프로젝트로 참조합니다. x64 빌드 결과는 `artifacts/bin/x64/<Configuration>/`, 중간 파일은 `artifacts/obj/`에 저장됩니다. `Directory.Build.targets`가 이 저장소의 경로에 맞춰 상위 라이브러리의 include 경로를 조정하며 서브모듈 소스는 수정하지 않습니다.

```powershell
msbuild ActionRPGServer/GameRoomServer/GameRoomServer.vcxproj /m /p:Configuration=Debug /p:Platform=x64
```

현재 `main.cpp`는 코어 객체를 생성한 뒤 종료합니다. 던전 서버가 실제로 연결을 받으려면 `StartServer()` 호출, 세션·패킷 처리, 설정 파일, TLS 인증서 등을 별도로 구현해야 합니다. 상위 라이브러리의 `ContentsServer`는 참고용이지 이 프로젝트의 의존성은 아닙니다.

## MultiSocketRUDP 버전 갱신

`External/MultiSocketRUDP`에서 검증할 커밋을 체크아웃하고 해당 서브모듈의 `CommonCode`를 갱신한 뒤 `GameRoomServer`를 다시 빌드합니다. 이후 변경된 서브모듈 포인터를 이 저장소에 커밋합니다. 라이브러리 소스를 게임 콘텐츠 디렉터리로 복사하지 않습니다.
