# 새 Windows PC의 로컬 개발 환경 설정과 문제 해결

기준일: 2026-10-08. 다른 PC에 MySQL·Auth·Town·Room·클라이언트를 모두 설치하여 로컬에서
실행하는 절차다. 기존 PC의 DB 데이터 이관이나 원격 서버 접속용 배포 절차는 별도다.
`RunLocalTest.bat`은 localhost 구성과 Debug/x64 출력 경로를 사용한다.

사용자는 기존 PC에서 V000003 적용과 Google 로그인 후 정상 입장을 확인했다.
2026-10-08 로컬 DB의 V000004 성공 이력과 검사 EXECUTE도 조회로 확인했다.
현재 배포 파일은 저장/복원 V000005이며 새 Auth는 head=5를 요구한다. V5 적용 결과는
대상에서 별도로 확인한다. 기존 head=4는 새 Auth에서 준비 실패하므로 서비스 정지 후 Up4→5와
새 검사·Town 공개 프로시저 권한, 동일 SQL/서버 배포가 필요하다.
새 PC의 동작과 던전 전투·성능까지 확인된 것은 아니다. 다른 PC에서도 아래 순서를 진행한다.

## 1. 설치 및 저장소 준비

| 준비 항목 | 이 프로젝트에서 필요한 조건 |
|---|---|
| Windows | x64 개발 PC, 현재 Windows 사용자로 설정·실행 |
| Git | 서버·클라이언트 저장소와 서버의 MultiSocketRUDP/CommonCode 서브모듈 |
| Visual Studio | C++ 데스크톱 개발, Windows SDK, 서버의 MSVC v145 및 클라이언트의 v143 도구 집합 |
| vcpkg | MSBuild 통합과 각 저장소 manifest 복원; 서버의 기존 overlay 포트도 사용 |
| MySQL Server | **8.0.46 / InnoDB**. 현재 실행기·Auth는 이 버전을 검사하므로 임의의 다른 버전으로 대체하지 않음 |
| 관리 도구 | MySQL Workbench 또는 동등한 관리자 접속 도구 |
| MySQL Connector/ODBC | **64비트 Unicode** 드라이버. 기존 PC의 확인된 등록명은 `MySQL ODBC 26.7 Unicode Driver` |
| Windows PowerShell | **64비트 5.1**. 아래 DB 예제도 이 셸에서 실행 |
| OpenSSL / curl | OpenSSL 3 및 Windows `curl.exe`; 최초 인증서 준비와 Auth HTTPS 준비 검사에 필요 |

OpenSSL은 실행기가 찾는 `Program Files/Git/mingw64/bin/openssl.exe` 또는
`Program Files/OpenSSL-Win64/bin/openssl.exe`에 준비한다. 실행기는 도구를 설치하지 않으며
OpenSSL 3의 default/legacy provider 사용 가능 여부도 확인한다. Debug 실행에는 개발용 C++
런타임이 필요하므로 EXE만 다른 PC에 복사하는 배포 방식으로 대신하지 않는다.

vcpkg가 MSBuild에 통합되어 있지 않으면 사용하는 vcpkg에서 `vcpkg integrate install`을 한 번
수행한다. 프로젝트의 manifest와 baseline을 유지하고 개별 라이브러리를 임의 버전으로 섞지 않는다.
설치된 의존 DLL은 app-local 배포로 실행 폴더에 복사된다.
[공식 MSBuild 통합 안내](https://learn.microsoft.com/en-us/vcpkg/users/buildsystems/msbuild-integration).
ODBC는 [공식 Windows 설치 안내](https://dev.mysql.com/doc/connector-odbc/en/connector-odbc-installation-binary-windows.html)를 따른다.

두 저장소는 같은 부모 폴더에 둔다. 예시는 경로 구조이며 기존 PC의 사용자 이름은 필요 없다.

```text
C:/dev/
  ActionRPGServer/
    RunLocalTest.bat
    ActionRPGServer/ActionRPGServer.slnx
  ActionRPGClient/
    ActionRPGClient/ActionRPGClient.slnx
```

서버 저장소 루트에서 서브모듈을 준비한다.

```powershell
git submodule update --init External/MultiSocketRUDP
git -C External/MultiSocketRUDP submodule update --init external/CommonCode
```

클라이언트 저장소도 자체 문서에 따라 서브모듈·의존성을 준비한다. 최신 서버와 호환되는 클라이언트
소스를 사용한다. 상세 클라이언트 설정은
[클라이언트 README](../../../ActionRPGClient/ActionRPGClient/README.md)와
[인증·타운 설정](../../../ActionRPGClient/ActionRPGClient/TOWN_NETWORK.md)을 따른다.

## 2. Google Desktop OAuth 준비

1. [Google Cloud의 OAuth 클라이언트](https://console.cloud.google.com/apis/credentials)에서
   **데스크톱 앱** 유형의 클라이언트를 준비한다. 동의 화면과 테스트 사용자 등 해당 프로젝트의
   로그인 허용 조건도 갖춘다.
2. 같은 클라이언트의 **client ID와 client secret**을 준비한다. 기존 Desktop 등록을 사용할 수
   있으며 웹 애플리케이션의 ID/secret이나 다른 ID의 secret을 혼합하지 않는다.
3. 실제 값은 뒤의 실행기 질문에 입력한다. 소스·공개 JSON·명령 인자·채팅에 붙여 넣지 않는다.

클라이언트는 시스템 브라우저, loopback callback, PKCE, state와 Auth가 발급한 nonce를 사용한다.
Google이 secret을 요구하는 등록에서는 토큰 교환 POST에 같은 Desktop secret을 포함한다.
Desktop secret은 배포 앱에서 기밀을 보장할 수 있는 서버 비밀 키가 아니므로 이를 인증 증명으로
삼지 않으며 PKCE·nonce·ID 토큰 서명 검증을 유지한다.
[Google의 설치형 앱 설명](https://developers.google.com/identity/protocols/oauth2#installed),
[토큰 교환 매개변수](https://developers.google.com/identity/protocols/oauth2/native-app#exchange-authorization-code).

## 3. MySQL 계정과 권한 준비

MySQL 서비스를 시작하고 관리자 도구에서 **실제 서버 버전**을 확인한다. 설치 프로그램이나
Workbench의 버전이 서버 버전을 대신하지 않는다.

```sql
SELECT VERSION(), @@version_comment;
```

신규 로컬 인스턴스에서는 Workbench의 Users and Privileges 등으로 다음 계정을 먼저 생성한다.
계정별 암호는 로컬 보안 입력으로 설정하고 문서나 저장소에 저장하지 않는다.

| 계정 예시 | 접속 호스트 | 목적 |
|---|---|---|
| `actionrpg_migrator` | `127.0.0.1` | DB 생성·마이그레이션과 저장 프로시저의 지속적인 DEFINER |
| `actionrpg_auth` | `127.0.0.1` | 런타임 프로시저 호출 전용 |

MySQL의 사용자 이름과 호스트는 함께 계정을 식별한다. `localhost` 계정을 만든 뒤
`127.0.0.1` 계정에 GRANT하는 식으로 혼용하지 않는다. 기존 계정이면 암호를 임의로 바꾸지 않고
계정·호스트·권한을 확인한다. [공식 계정 관리 안내](https://dev.mysql.com/doc/workbench/en/wb-mysql-connections-navigator-management-users-and-privileges.html).

관리자로 신규 스키마에 사용할 migrator 권한을 준비한다. 다음은 `actionrpg` 이름을 사용하는 예시다.

```sql
GRANT SELECT, INSERT, UPDATE, CREATE, DROP, REFERENCES,
      CREATE ROUTINE, ALTER ROUTINE, EXECUTE, LOCK TABLES
ON actionrpg.* TO 'actionrpg_migrator'@'127.0.0.1';
```

실행기는 사용자나 권한을 만들지 않는다. migrator는 루틴의 DEFINER이므로 적용 후에도 계정과
필요한 권한을 유지한다. 런타임 계정에 위의 DDL·테이블 쓰기 권한을 복사하지 않는다.
기존 DB의 정의 주체와 권한 설계는 [DB 계약](DATABASE_MIGRATIONS.md#authserver-조회-계약)을 따른다.

## 4. 신규 DB 생성과 마이그레이션

Auth·Town·Room과 해당 DB를 사용하는 서비스를 정지한다. `-ServicesStopped`를 생략하면 콘솔에서
`Y` 또는 `y`로 정지를 확인한다. 프로세스를 자동으로 종료하지 않는다. 신규 DB는 `-CreateDatabase`로 만들 수 있다.
이미 데이터가 있는 DB를 초기화하거나 복제하는 명령이 아니다.

`Tool\Database` 폴더에서 **UpMigration.bat을 실행**하면 된다. 콘솔에서 DB 이름(기본 `actionrpg`),
서버 종료 확인(`Y` 또는 `y`), 호스트(기본 `127.0.0.1`), 포트(기본 `3306`), 마이그레이션 계정(기본
`actionrpg_migrator`), 비밀번호를 입력한다. 기본값은 Enter로 선택한다. 설치된 64비트 MySQL Unicode
ODBC 드라이버가 하나면 자동 선택하고 여러 개면 번호를 묻는다. 비밀번호는 숨김 입력으로 받고 저장하지 않는다.
인자 없이 실행한 배치는 종료 전에 키 입력을 기다리므로 결과를 확인할 수 있다.

```powershell
# Tool\Database 폴더에서 기존 DB를 최신화
.\UpMigration.bat

# DB가 없는 새 PC에서만 명시적으로 생성
.\UpMigration.bat -Database actionrpg -CreateDatabase

# 최신 이력 하나만 롤백 (해당 버전의 데이터 보존 조건을 만족해야 함)
.\DownMigration.bat
```

`ACTIONRPG_MIGRATION_CONNECTION_STRING`이 이미 있으면 입력 대신 사용한다. 원격 DB의 CA·호스트
검증 옵션도 이 연결 문자열로 지정한다. 대화형 연결은 `SSLMODE=REQUIRED`로 암호화를 요구한다.
자동화에서는 연결 환경 변수와 `-Database actionrpg -ServicesStopped`를 함께 제공하면 추가 입력이 없다.

기대 결과는 `Complete. Active version: V000006.`과 종료 코드 **0**이다. 기반 V000000 다음에
V000001→V000002→V000003→V000004→V000005→V000006이 적용된다. DB 생성 뒤 실패하면 DB는 남으며 자동 삭제하지 않는다.
실패 출력이 있으면 일반 Up을 반복하지 말고 아래 문제 해결 절차를 따른다.
배치는 실행 정책 RemoteSigned를 자식 PowerShell에만 지정한다. 조직의 상위 정책을 덮어쓰는 기능은 아니다.
[실행 정책 범위 설명](https://learn.microsoft.com/en-us/powershell/module/microsoft.powershell.security/set-executionpolicy?view=powershell-5.1).

마이그레이션 성공 후 관리자로 기존 EXECUTE를 유지하고 **V6 검사와 Town 사용 공개 프로시저 EXECUTE**를 추가한다.
새 Auth의 필수 호출은 login_google_account/get_auth_account_status/get_item_use_schema_migration_history다.
Town은 기존 5개 캐릭터 절차와 reserve_item_use/get_item_use/complete_item_use/cancel_item_use를 호출한다.
V6에서 교체한 claim_character/save_character_state/release_character의 EXECUTE도 아래와 같이 재부여한다.
아래는 로컬에서 Auth/Town이 actionrpg_auth를 공유하는 예제다. 별도 계정이면 해당 역할의 호출만 부여한다.
내부 emit_character_state/emit_item_use에는 EXECUTE를 부여하지 않는다.
기존 V0/V4 검사 EXECUTE는 새 검사 권한을 대신하지 않는다.
계정과 프로시저가 모두 존재하는지 먼저 확인한다.

```sql
GRANT EXECUTE ON PROCEDURE actionrpg.get_schema_migration_history
TO 'actionrpg_auth'@'127.0.0.1';
GRANT EXECUTE ON PROCEDURE actionrpg.login_google_account
TO 'actionrpg_auth'@'127.0.0.1';
GRANT EXECUTE ON PROCEDURE actionrpg.get_auth_account_status
TO 'actionrpg_auth'@'127.0.0.1';
GRANT EXECUTE ON PROCEDURE actionrpg.get_character_schema_migration_history
TO 'actionrpg_auth'@'127.0.0.1';
GRANT EXECUTE ON PROCEDURE actionrpg.get_inventory_schema_migration_history
TO 'actionrpg_auth'@'127.0.0.1';
GRANT EXECUTE ON PROCEDURE actionrpg.list_characters
TO 'actionrpg_auth'@'127.0.0.1';
GRANT EXECUTE ON PROCEDURE actionrpg.create_character
TO 'actionrpg_auth'@'127.0.0.1';
GRANT EXECUTE ON PROCEDURE actionrpg.claim_character
TO 'actionrpg_auth'@'127.0.0.1';
GRANT EXECUTE ON PROCEDURE actionrpg.save_character_state
TO 'actionrpg_auth'@'127.0.0.1';
GRANT EXECUTE ON PROCEDURE actionrpg.release_character
TO 'actionrpg_auth'@'127.0.0.1';
GRANT EXECUTE ON PROCEDURE actionrpg.get_item_use_schema_migration_history
TO 'actionrpg_auth'@'127.0.0.1';
GRANT EXECUTE ON PROCEDURE actionrpg.reserve_item_use
TO 'actionrpg_auth'@'127.0.0.1';
GRANT EXECUTE ON PROCEDURE actionrpg.get_item_use
TO 'actionrpg_auth'@'127.0.0.1';
GRANT EXECUTE ON PROCEDURE actionrpg.complete_item_use
TO 'actionrpg_auth'@'127.0.0.1';
GRANT EXECUTE ON PROCEDURE actionrpg.cancel_item_use
TO 'actionrpg_auth'@'127.0.0.1';
SHOW GRANTS FOR 'actionrpg_auth'@'127.0.0.1';
```

## 5. 서버와 클라이언트 빌드

`RunLocalTest.bat`은 실행 시 서버 솔루션과 클라이언트 게임 프로젝트를 **Debug / x64로 증분 빌드**한다.
vswhere로 서버 v145와 클라이언트 v143용 MSBuild를 각각 찾으며, 빌드 실패 시 서버를 시작하지 않는다.
프로젝트의 기존 복사 단계가 실행 Data·Assets·DLL을 준비하고 실행기가 공개 인증 설정을 복원한다.
직접 빌드하려면 Visual Studio에서 두 솔루션을 각각 **Debug / x64로 다시 빌드**한다. 명령줄에서는 Visual Studio
개발자 셸의 MSBuild를 사용하며 서버 예시는 [대표 README](../../README.md#빌드-준비와-출력)에 있다.
vcpkg manifest 복원과 app-local DLL 복사가 성공해야 한다. 최신 소스를 받는 것과 EXE가 갱신되는 것은 다르다.

| 대상 | 저장소 기준 실행 경로 |
|---|---|
| AuthServer | 서버 `ActionRPGServer/x64/Debug/AuthServer.exe` |
| TownServer | 서버 `ActionRPGServer/x64/Debug/TownServer.exe` |
| GameRoomServer | 서버 `artifacts/bin/x64/Debug/GameRoomServer.exe` |
| ActionRPGClient | 클라이언트 `ActionRPGClient/artifacts/bin/x64/Debug/ActionRPGClient.exe` |

Room 실행 폴더에는 해당 빌드의 `libssl-3-x64.dll`, `libcrypto-3-x64.dll`도 있어야 한다.
서버 Data·Room 옵션 및 클라이언트 Assets는 각 프로젝트의 복사 단계로 준비한다.
다른 출처의 DLL이나 클라이언트 이미지를 서버 저장소에 수동으로 복사하지 않는다.

## 6. 새 PC에서 최초 실행

서버 저장소 루트에서 실행한다. 기존 실행 창이 있으면 먼저 직접 종료한다.

```powershell
& '.\RunLocalTest.bat'
```

최초 실행 질문에는 준비한 Google Desktop ID, MySQL 호스트 `127.0.0.1`, 포트 `3306`, 스키마
`actionrpg`, **runtime 사용자 `actionrpg_auth`**, 그 암호, ODBC TLS 옵션을 입력한다.
실행기에는 migrator 계정이나 관리자 계정을 넣지 않는다. 추가 ODBC 옵션은 실제 드라이버의
`key=value;key=value` 형식이며 로컬 암호화 옵션 예시는 `SSLMODE=REQUIRED`다.
Google secret 질문에는 같은 Desktop ID의 값을 보안 입력한다.

설정은 새 PC의 `%LOCALAPPDATA%/ActionRPG/LocalTest`에 생성된다. 공개 `settings.json`과
DPAPI CurrentUser로 암호화한 `credentials.dpapi`, 사용자 전용 ACL의 Auth/Town PEM 키·인증서,
CA 파일이 준비된다. 신뢰 루트는 해당 사용자의 CurrentUser/Root에 추가된다. 기존의 유효한
DevServerCert가 있으면 재사용하고, 룸 서버가 선택할 수 있는 모든 같은 이름 인증서를 검사한다.

**기존 PC의 LocalTest 폴더·DPAPI 파일·인증서 개인 키를 새 PC로 복사하지 않는다.** 새 PC·사용자에서
최초 설정을 수행한다. 암호화 파일은 이동 가능한 설정 템플릿이 아니며 인증서 저장소와 소유권도 필요하다.
설정 생성 성공은 DB 준비 성공을 뜻하지 않으며 실행기는 DB 마이그레이션을 자동 적용하지 않는다.

실행기는 Auth → Town → Room → 클라이언트 두 개를 시작한다. TCP 8443(Auth), 7777(Town),
7780(Town↔Room), 기본 11011(Room 세션 브로커)을 사용한다. 게임 UDP 포트는 Room 코어 설정을
따른다. localhost 예제를 외부 접근용으로 사용하려면 별도의 DNS·인증서·CA·주소·방화벽 설정이 필요하다.

클라이언트 EXE 옆 `Assets/Data/AuthClient.json`에는 공개 주소·ID·타운 CA 경로·타운 목록만 공급한다.
Google secret은 ID와 짝지어 클라이언트 환경에만 전달되고 로더가 읽은 뒤 환경에서 제거한다.
서버에는 Desktop secret을, 클라이언트에는 DB·타운·RoomControl 비밀 키를 전달하지 않는다.
클라이언트 재빌드로 빈 원본 JSON이 다시 복사되면 실행기를 다시 실행해 설정을 공급한다.

브라우저 안내는 Google 콜백 수신을 뜻한다. 실제 성공은 게임의 로그인 완료·타운 선택·입장 화면으로
확인한다. 두 클라이언트는 서로 다른 실제 Google 계정으로 로그인한다. 같은 계정으로 새 로그인하면
기존 게임 토큰이 무효화된다. 종료할 때는 각 창을 직접 닫으며 실행기 실패 후 먼저 시작된 서버도 남는다.

## 7. 실제로 발생한 오류와 해결 방법

| 증상 | 확인할 내용과 조치 |
|---|---|
| 드라이버 대입 시 `PSObject`를 `String`으로 변환할 수 없음 | 목록의 실제 Name을 문자열로 변환하고 `.set_Driver([string]...)` 사용. 빈 번호나 드라이버 버전 숫자만 입력하지 않음 |
| 비밀번호 입력에서 멈춤·자격 증명 창이 안 보임 | 별도 `Get-Credential` 창 대신 위 예제와 실행기의 `Read-Host -AsSecureString` 사용. 콘솔에서 숨김 입력 후 Enter |
| `Migrate.ps1` 실행 정책 오류 | 배치는 자식 PowerShell에 `-ExecutionPolicy RemoteSigned`를 지정함. 상위 조직 정책 또는 다운로드 파일 차단 여부를 확인 |
| `GRANT` 오류 1410: user 생성 불가 | 대상 `'사용자'@'호스트'` 계정을 먼저 생성. MySQL 8에서 GRANT를 계정 생성 대신 사용하지 않음 |
| V0/V1 `Constraints mismatch` | 최신 실행기와 Auth 소스 사용. MySQL CHECK 메타데이터의 문자열 escape와 `OCTET_LENGTH`/`LENGTH` 표기 차이를 비교 단계에서만 정규화. 이미 FAILED가 남았다면 아래 진단·복구 절차로 이동 |
| private settings directory에서 `PrivilegeNotHeldException` | 최신 실행기는 생성 시 ACL을 적용하고 검증함. 기존 디렉터리의 소유자·허용 주체를 확인하며 관리자 실행이나 전체 사용자 접근 허용으로 우회하지 않음 |
| 64-bit MySQL Unicode ODBC driver not registered | `Get-OdbcDriver -Platform '64-bit'`로 실제 등록명 확인. 32비트/ANSI 드라이버와 혼동하지 않음. 최신 실행기는 연결 문자열 Driver의 중괄호 표기를 제거해 등록명과 대조 |
| Auth TLS 오류, curl 60 `revocation status is unknown` | 현재 사용자 CA 신뢰·localhost SAN·CA 경로 확인. 최신 실행기의 로컬 검사에는 `--ssl-revoke-best-effort`가 적용됨. `--insecure`나 전체 인증서 검증 해제로 대체하지 않음 |
| Auth listening 로그 뒤 종료 코드 1 | 이전 소스는 실제 bind 전에 listening을 출력했음. 최신 실행기로 빌드한 뒤 서버 콘솔의 bind/listen/accept stage와 숫자 WSA_error 확인. 8443 점유·Windows 제외 포트 범위·로컬 보안 정책을 검토하며 임의 포트 변경이나 관리자 실행으로 우회하지 않음 |
| Auth가 listen 중인데 DB verification failed/HTTP 503 | listen 로그만으로 준비 완료를 판단하지 않음. stage·database_error·SQLSTATE·native_code·context 확인. DB head=6, V6 검사/로그인/상태 조회 EXECUTE 권한, DEFINER, 배포 SQL 경로·원문을 대조하고 문제 해결 후 Auth 재시작 |
| `ODBC driver substituted the connection timeout` | 이전 공통 ODBC 코드의 MySQL 비지원 속성 검사. 최신 소스의 로그인 타임아웃 검사로 Auth와 Town을 모두 다시 빌드 |
| Room 출력 없이 종료 | EXE 옆 해당 빌드의 OpenSSL DLL 두 개와 Debug C++ 런타임 확인. Room과 의존 프로젝트를 재빌드해 app-local 배포. 최신 프로젝트는 후속 빌드의 DLL 삭제도 방지 |
| Room 종료 코드가 빈칸 | 최신 실행기는 네이티브 프로세스 핸들을 보유해 종료 코드를 읽음. 빈 코드 자체를 원인으로 보지 말고 위 런타임 파일 검사 |
| 브라우저 완료 안내 후 게임 요청 거절 | 브라우저 안내는 로그인 성공 확정이 아님. 최신 클라이언트의 실패 단계·HTTP·허용 OAuth 오류 코드를 확인 |
| `[Google 토큰 교환] HTTP 400 / invalid_request / 자격 증명 누락` | 같은 Desktop ID의 secret을 실행기에 입력. 기존 프로필에는 다음 실행에서 한 번 추가됨. 클라이언트의 환경 읽기·`client_secret` 전송 코드까지 빌드되어야 함 |
| secret 저장 후에도 같은 누락 오류 | EXE가 최신 소스보다 오래됐는지 확인. 오류 안내만 추가된 이전 EXE에는 secret 전송이 없었음. 모든 창 종료 → 클라이언트 Debug/x64 재빌드 → 실행기 재실행 |
| secret 저장 단계 파일 교체에서 `MethodInvocationException` | Windows PowerShell 5.1의 일반 `$null` 문자열 변환 문제. 최신 실행기는 `File.Replace`의 백업 인수에 `[NullString]::Value` 사용. 남는 오류는 내부 예외 종류/HRESULT로 구분 |
| 포트 사용 중·서버 already running | 이전 실행 창을 직접 닫고 재시도. 실행기는 기존 프로세스를 재사용하거나 종료하지 않음 |
| 새 사용자/PC에서 DPAPI 복호화·인증서 누락 | 기존 PC 프로필을 복사해 사용하지 않음. 새 사용자별 최초 설정 필요. 기존 파일을 임의로 덮어쓰거나 인증서를 삭제하지 않고 경로·소유권·프로필 상태부터 확인 |

소스와 EXE 갱신 시각은 서버 저장소 루트에서 다음처럼 확인할 수 있다. 빌드 성공 출력이 있어도
실행기가 여는 **Debug/x64 경로**가 갱신됐는지 확인한다.

```powershell
Get-Item '..\ActionRPGClient\ActionRPGClient\ActionRPGClient\Network\AuthClient.cpp',
         '..\ActionRPGClient\ActionRPGClient\ActionRPGClient\Network\AuthSettings.cpp',
         '..\ActionRPGClient\ActionRPGClient\artifacts\bin\x64\Debug\ActionRPGClient.exe' |
    Select-Object Name, LastWriteTime
Get-CimInstance Win32_Process -Filter "Name = 'ActionRPGClient.exe'" |
    Select-Object ProcessId, ExecutablePath, CreationDate
```

## 8. 마이그레이션 실패 후의 경계

`FAILED`/`RUNNING` 또는 부분 생성 상태에서 DB나 감사 행을 삭제하거나 Up/DDL을 무작정 재실행하지 않는다.
기존 연결 비밀을 제공한 상태로 `-InspectOnly -InspectVersion 0` 또는 `1`을 사용하면 해당 버전의
실제 구조를 읽기 전용 비교할 수 있다. 검사 성공은 이력 유효성이나 복구 성공을 뜻하지 않는다.

구조와 체크섬 등이 모두 맞는 **최초 V0 실패**는 승인된 `-RecoverBootstrap`, 정상 V0 뒤의
**최초 V1 실패·빈 계정 테이블**은 승인된 `-RecoverAccounts` 계약이 있다. 원래 FAILED 행을 유지하고
확인 행을 추가하며, 성공 후 별도의 일반 Up을 실행한다. 이 옵션은 새 PC 설치의 정규 단계가 아니며
불완전한 구조·다른 실패·데이터가 있는 상태에는 적용하지 않는다. 대상과 조건을 검토한 후 진행한다.
전체 조건과 명령은 [초기 V0 복구](DATABASE_MIGRATIONS.md#초기-v000000-전용-명시적-복구)와
[초기 V1 복구](DATABASE_MIGRATIONS.md#초기-v000001-전용-명시적-복구)에 있다.

문제 보고에는 실패 단계, 버전/문장 번호, 종료 코드, 안전한 예외 종류/HRESULT,
클라이언트 HTTP/OAuth 분류와 소스·EXE 시각을 전달한다. 비밀번호, 연결 문자열, 토큰, nonce,
원문 Google 응답, DPAPI 파일, 인증서 개인 키는 공유하지 않는다.
