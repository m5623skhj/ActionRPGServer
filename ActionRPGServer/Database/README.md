# 계정 DB와 마이그레이션

현재 SQL과 검증 계약은 **MySQL 8.0.47 / InnoDB**를 대상으로 한다. 계정 DB 호출은
AuthServer가 소유하며, [공유 ODBC 모듈](../Shared/Database)과 `IStoreProcedure<Req, Res>`
객체로 저장 프로시저를 비동기 실행한다. DB 전용 서버 프로세스는 구현하지 않았다.

DB에는 내부 계정, Google `sub`와 계정의 연결, 계정 상태·시각, 마이그레이션 감사 이력을
저장한다. 로그인 세션·티켓·입장 소유권과 캐릭터 성장 상태는 현재 메모리에서 관리한다.
Google 토큰이나 이메일은 현재 계정 스키마에 저장하지 않는다.

## SQL 구성과 요구 버전

| 버전 | 파일 | 역할 |
|---|---|---|
| 000000 | [Infrastructure/V000000__migration_history.sql](Migrations/MySQL/Infrastructure/V000000__migration_history.sql) | 이력 테이블·조회 프로시저; Down 없음 |
| 000001 | [V000001__create_login_accounts.sql](Migrations/MySQL/V000001__create_login_accounts.sql) | 계정·외부 식별자 테이블 |
| 000002 | [V000002__create_google_login_procedure.sql](Migrations/MySQL/V000002__create_google_login_procedure.sql) | Google 로그인 조회·최초 자동 가입 |
| 000003 | [V000003__create_auth_account_status_procedure.sql](Migrations/MySQL/V000003__create_auth_account_status_procedure.sql) | 계정 상태 재확인 |

역변환 SQL은 [Migrations/MySQL/Down](Migrations/MySQL/Down)에 있다. 파일 최신 버전은
000003이며, [Auth 검증 계약](../AuthServer/Database/LoginSchemaVerifier.h)의 요구 head는
**3**이다. Auth는 기동 시 전체 적용 이력·실제 구조·배포 SQL을 대조하고 로그인·입장 기능의
준비 여부를 결정한다. **실제 DB 적용 버전은 이 문서 작업에서 미확인**이다.

## 수동 적용

서버 기동 시 마이그레이션을 자동 실행하지 않는다. 운영자가
[UpMigration.bat](../../Tool/Database/UpMigration.bat) 또는
[DownMigration.bat](../../Tool/Database/DownMigration.bat)를 실행하며, 공통 실행기는
[Migrate.ps1](../../Tool/Database/Migrate.ps1)이다.

Up은 모든 미적용 버전을 순서대로 최신화하고, Down은 현재 head 한 단계만 되돌린다.
이력 기반 000000은 유지하며, 계정 테이블을 삭제하는 000001 Down은 두 테이블이 비어 있어야 한다.
대상 스키마를 별도로 준비하고 DB 사용 서비스를 정지한 뒤, 64비트 MySQL ODBC 드라이버와
보호된 `ACTIONRPG_MIGRATION_CONNECTION_STRING` 환경 변수를 제공한다. 접속 비밀은 Git에 넣지 않는다.

실행 명령과 인자는 [툴 README의 DB 안내](../../Tool/README.md#db-마이그레이션)를 따른다.
현재 버전 조회에 Up을 사용하면 대기 SQL이 적용될 수 있다. 실패나 부분 적용 시 이력을 지우거나
SQL을 재실행하지 말고 상세 문서의 조회·복구 절차를 따른다.

## 상세 문서

* [DB 구현·마이그레이션 사용 및 작업 규칙](../../docs/workflows/DATABASE_MIGRATIONS.md):
  스키마·프로시저 계약, 권한·체크섬·감사 이력, 현재 버전 조회, 실패 복구와 지원 한계.
* [AuthServer 개발 계약](../AuthServer/DEVELOPMENT.md): Google 로그인·세션·입장 흐름과 배포 전제.

이 README는 소스 기준 안내다. 이번 작업에서는 빌드·서버 실행·DB 접속·SQL 적용을 수행하지 않았다.
