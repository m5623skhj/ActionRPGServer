# AuthServer 개발 계약

AuthServer는 계정 로그인과 게임 세션을 소유한다. TownServer는 Auth가 승인한 목적 서버
티켓을 소비하여 입장한다. 룸의 전투 프레임마다 Auth를 호출하지 않는다.

## 실행 전제와 현재 상태

솔루션에 C++20/x64 AuthServer 프로젝트를 추가했다. 기존 vcpkg manifest에 OpenSSL,
jwt-cpp, cpp-httplib의 HTTPS 기능을 선언했다. 의존성 설치·빌드·테스트·서버 실행은
수행하지 않았다. 고정 vcpkg baseline에서 실제 헤더/링크 호환성도 실행 확인하지 않았다.

계정 DB는 MySQL 8.0.47의 V000001 → V000002 → V000003 계약이다.
[DB 마이그레이션 규칙 v1.1.1](../../docs/workflows/DATABASE_MIGRATIONS.md)에 따라 서비스가 종료된
상태에서 별도 수동 Up/Down 실행기를 사용한다. Auth가 마이그레이션을 자동 실행하지 않는다.
배포 순서는 관련 서비스 종료 → 수동 Up → 같은 SQL 파일 배포 → Auth 검증 기동 → 타운/룸
기동이다. Up은 미적용 버전을 순서대로 모두 적용하고 Down은 현재 최상위 버전 한 단계만
되돌린다. V1 Down은 계정과 외부 식별자 테이블이 비어 있을 때만 허용하며 V0 이력은 유지한다.

기동 시 실제 계정 DB의 `get_schema_migration_history()`를 공통 ODBC 실행기로 호출한다.
조회 프로시저는 마이그레이션과 같은 DB 잠금을 획득해 메타데이터와 적용 이력을 읽는다.
전체 이력의 증가 순서·Up/Down 스택·성공 상태·양방향 파일 체크섬을 검증하며, 실제 테이블
컬럼·기본값·CHECK 활성화·인덱스·외래키 대상/규칙·프로시저 본문/입력 계약도 대조한다.
총 10개 결과셋 중 마지막 3개는 `SHOW CREATE PROCEDURE` 원본 정의다. MySQL의
`ROUTINE_DEFINITION`은 문자셋 표기를 제거하므로 본문 비교에는 원본 정의를 사용한다.
조회 프로시저의 definer는 대상 세 프로시저의 원본을 볼 권한이 필요하다. 원본이 NULL이거나
생성 SQL mode에 strict가 없거나 `NO_BACKSLASH_ESCAPES`/`ANSI_QUOTES`/`PIPES_AS_CONCAT`이
있으면 거절한다. 원본 조회를 위해 앱 계정에 SHOW_ROUTINE을 추가하지 않고 조회 프로시저
EXECUTE를 사용한다. 로그인·계정 상태 조회 프로시저의 EXECUTE 권한도 별도로 필요하다.
기반 V0은 최초 한 번이며 현재 적용 head=3에서만 해당 ODBC DB에 결합된 증명을 발급한다.

SQL 체크섬은 엄격한 UTF-8, 첫 BOM 제거, CRLF/CR→LF, 나머지 공백/끝 개행 보존 후
BOM 없는 UTF-8 SHA256 소문자 hex64다. 미적용·진행 중·실패 이력·구조/체크섬 불일치·조회
실패에는 로그인/입장/갱신 503을 반환한다. 로그아웃과 기존 소유권 해제는 이 게이트 밖이다.
기동 검증은 15초까지 기다리며 시간 초과/실패 후에는 DB가 정상화되어도 재시작하여 재검증한다.
이 증명은 기동 시점 확인이며 서비스 중 DDL/파일 변경을 허용하지 않는 배포 조건이 필수다.

현재 실제 DB 접속/적용은 수행하지 않았다. 파일과 코드 작성만으로 운영 준비를 확인한 것은
아니다. 공통 ODBC 한도는 전체 결과 4096행/16셋/4MiB이며 누적 이력이 이를 넘으면 차단한다.
실제 MySQL 메타데이터 표기와 프로시저 본문 매핑도 실행 확인이 필요하다.

## 설정

비밀 값을 저장소나 명령 인자에 기록하지 않고 프로세스 환경으로 제공한다.

| 환경 변수 | 용도 |
|---|---|
| `ACTIONRPG_AUTH_TLS_CERT` / `ACTIONRPG_AUTH_TLS_KEY` | HTTPS 인증서 체인 및 개인 키 파일 |
| `ACTIONRPG_AUTH_CA_FILE` | Google 및 타운의 Auth HTTPS 서버 검증용 CA PEM 파일 |
| `ACTIONRPG_GOOGLE_CLIENT_ID` | 허용 Google audience 및 authorized party |
| `ACTIONRPG_AUTH_TOWN_REGISTRY` | 타운 ID에서 각각의 64자리 소문자 hex 비밀 키로 매핑하는 JSON 객체 |
| `ACTIONRPG_DB_CONNECTION_STRING` | 계정 DB ODBC 연결 문자열 |
| `ACTIONRPG_DB_SCHEMA` | 실제 조회 DATABASE()와 대조할 계정 스키마 이름 |
| `ACTIONRPG_DB_MIGRATIONS_DIRECTORY` | Infrastructure/Down 및 Up SQL이 있는 배포 MySQL 디렉터리의 절대 경로 |

Auth HTTPS 포트는 8443이다. 타운은 `ACTIONRPG_AUTH_HOST`의 인증서 호스트명을 검증하며,
`ACTIONRPG_TOWN_ID`/`ACTIONRPG_TOWN_AUTH_KEY`를 Auth 등록값과 일치시킨다.
타운 클라이언트에도 별도 TLS 인증서와 키가 필요하다. 인증서 발급·키 생성은 수행하지 않았다.
내부 API는 HTTPS에서 서버별 키를 확인한다. 외부 클라이언트에 타운 키를 배포하지 않는다.
배포 시 내부 API 접근을 타운 네트워크로 제한한다. 현재 RoomControl의 기존 로컬 연결
계약은 유지한다. 여러 타운은 각자의 RoomControl/RoomServer 묶음과 고유 타운 ID를 사용한다.

## HTTPS API

모든 POST body는 JSON 객체다. 세션 토큰은 `Authorization: Bearer <gameToken>` 헤더로
제출한다. 응답은 `Cache-Control: no-store`이며 실패 body에 자격 증명을 포함하지 않는다.

| 경로 | 입력 | 성공 응답 |
|---|---|---|
| `/v1/challenges` | `{}` | `challengeId`, `nonce`, `expiresIn=300` |
| `/v1/login` | `challengeId`, `idToken` | `gameToken`, `expiresIn=28800` |
| `/v1/tickets` | Bearer + `serverId` | `ticket`, `expiresIn=30`, `ready` |
| `/v1/logout` | Bearer + `{}` | `{}` |
| `/internal/consume` | `ticket`, `connection` | `accountId`, `lease`, `expiresIn=15` |
| `/internal/renew` | `lease`, `connection` | `valid`, `expiresIn` |
| `/internal/release` | `lease`, `connection` | `{}` |

내부 요청은 `X-Town-Id`, `X-Town-Key` 헤더가 필수다. `connection`은 타운에서 새 연결마다
생성하는 256비트 임의 값이다. 티켓과 권한은 목적 타운·연결에 결합되고 클라이언트가 계정
ID를 선택할 수 없다. 권한을 해제하는 release는 타운이 모든 게임 접속 종료를 확인한 뒤 호출한다.
성공은 HTTP 200, 미준비 DB 이력 게이트는 503, 거절/의존 서비스 실패는 403이다.
서버 키 불일치는 401이다. 완료 여부가 불확실한 요청은 자동 재시도하지 않는다.

클라이언트 흐름: challenge 발급 → 해당 nonce를 Google OIDC 인증 요청에 연결 → ID 토큰
제출 → 게임 토큰 보관 → 목적 타운 티켓 발급 → 타운 TLS 첫 패킷 36 → 성공 패킷 37 →
기존 EnterTown. `ready=false`이면 기존 접속 종료 후 새 티켓을 받아야 한다. 아직 소유자가
남아 있을 때 소비한 티켓도 재사용되지 않는다. 토큰을 URL/로그에 넣지 않는다.
클라이언트의 Google SDK 선택과 nonce 연결 구현은 별도 작업이다.

## 검증과 중복 로그인

Google 토큰은 고정 Google JWKS HTTPS 주소에서 받은 RSA 키로 RS256 서명을 검사한다.
키 캐시는 Cache-Control 유효 기간을 최대 24시간으로 제한하며 unknown kid 갱신은
30초 간격으로 제한한다. 만료 키를 장애 시 계속 사용하지 않는다. issuer, audience,
authorized party, exp, iat, sub, 일회성 challenge nonce를 검사한다. 하나의 허용 client ID
계약이며 웹/네이티브의 여러 authorized party 허용은 별도 협의가 필요하다.

새 로그인은 기존 게임 토큰을 무효화하고 기존 타운 권한의 갱신을 거절한다. 기존 타운은
5초 갱신 주기에서 이를 확인해 TCP를 종료하고 던전 퇴장을 확인한다. 새 세션의 입장은
기존 소유권 해제 후 허용한다. 타운 이동도 같은 흐름이며 게임 토큰은 재사용한다.
로그인 계정 생성 결과와 상태 조회 결과는 커밋 전에 형식을 검증한다. 정지/없는 계정은
티켓 발급·소비·갱신에서 거절하고 기존 게임 토큰과 미사용 티켓을 폐기한다.
상태 조회 절차는 계정 로그인 시각을 갱신하지 않는다.

메모리 상태 변경은 하나의 mutex로 직렬화하고 DB/HTTPS 호출은 그 잠금 밖에서 수행한다.
Auth HTTP worker 8개/대기 64개, challenge·계정·티켓 각각 최대 10000개다. 만료 challenge와
티켓, 소유자가 없는 만료 계정은 제거한다. Auth DB 완료는 독립 I/O 스레드에서 전달된다.

## 장애 및 미구현 범위

세션은 단일 Auth 프로세스 메모리에만 존재한다. Auth 재시작은 토큰을 무효화한다.
재시작 전에 모든 타운/룸 접속을 종료하고 확인해야 한다. 이전 소유권이 사라진 상태에서
살아 있는 이전 게임 서버와 새 인증 프로세스를 동시에 운영하는 재시작은 지원하지 않는다.
다중 Auth, 공유 세션 저장소, 장애 후 자동 소유권 회복은 구현하지 않았다.

소유권은 타운 권한이 만료돼도 자동 해제하지 않는다. 퇴장 확인/해제 응답 유실이나 타운
장애 시 새 접속이 막히며, 운영자가 기존 타운과 룸을 종료하고 확인해야 한다. 이를 확인하지
않는 관리 해제 API는 제공하지 않는다. HTTP/DB 불확실한 결과의 무조건 재시도도 없다.

타운 간 캐릭터·스킬·진행 상태 저장/이관은 포함하지 않았다. 타운 이동은 접속 권한을
옮기는 기능이며 현재 메모리 기반 캐릭터 진행 상태를 보존하는 기능은 별도 구현 대상이다.

근거: [Google ID 토큰 서버 검증](https://developers.google.com/identity/sign-in/web/backend-auth),
[Google OIDC nonce](https://developers.google.com/identity/openid-connect/openid-connect),
[cpp-httplib HTTPS](https://github.com/yhirose/cpp-httplib), [jwt-cpp](https://github.com/Thalhammer/jwt-cpp).
