# 계정·캐릭터·인벤토리 DB와 마이그레이션

현재 SQL과 검증 계약은 **MySQL 8.0.46 / InnoDB**를 대상으로 한다. 계정 DB 호출은
AuthServer가 소유하며, [공유 ODBC 모듈](../Shared/Database)과 `IStoreProcedure<Req, Res>`
객체로 저장 프로시저를 비동기 실행한다. TownServer는 V5 공개 프로시저로 캐릭터·성장·인벤토리를 저장/복원한다. DB 전용 서버 프로세스는 구현하지 않았다.

DB에는 내부 계정, Google `sub`와 계정의 연결, 계정 상태·시각, 마이그레이션 감사 이력을
저장한다. V4에 캐릭터·보유 SP·습득 스킬 저장용 테이블을 추가했다.
로그인 세션·티켓은 메모리에서 관리한다. V5는 캐릭터별 revision·소유 토큰 세대와 성공 요청 이력을 영속화한다.
Google 토큰이나 이메일은 현재 계정 스키마에 저장하지 않는다.

## SQL 구성과 요구 버전

기존 SQL 파일의 `Target: MySQL 8.0.47` 주석은 작성 당시 표기다. 현재 대상 버전은 위의
8.0.46이며, 적용 이력의 체크섬을 보존하기 위해 SQL 파일 자체는 변경하지 않는다.
2026-10-07 사용자가 로컬 8.0.46의 마이그레이션 성공과 Google 로그인 후 정상 입장을 확인했다.
새 PC·운영 환경은 별도 검증이 필요하며 에이전트가 직접 DB를 조회한 결과는 아니다.

| 버전 | 파일 | 역할 |
|---|---|---|
| 000000 | [Infrastructure/V000000__migration_history.sql](Migrations/MySQL/Infrastructure/V000000__migration_history.sql) | 이력 테이블·조회 프로시저; Down 없음 |
| 000001 | [V000001__create_login_accounts.sql](Migrations/MySQL/V000001__create_login_accounts.sql) | 계정·외부 식별자 테이블 |
| 000002 | [V000002__create_google_login_procedure.sql](Migrations/MySQL/V000002__create_google_login_procedure.sql) | Google 로그인 조회·최초 자동 가입 |
| 000003 | [V000003__create_auth_account_status_procedure.sql](Migrations/MySQL/V000003__create_auth_account_status_procedure.sql) | 계정 상태 재확인 |
| 000004 | [V000004__create_characters_and_skills.sql](Migrations/MySQL/V000004__create_characters_and_skills.sql) | 캐릭터·습득 스킬과 확장 검사 프로시저 |
| 000005 | [V000005__create_character_inventory_persistence.sql](Migrations/MySQL/V000005__create_character_inventory_persistence.sql) | 캐릭터 소유권·인벤토리·중복 요청과 저장/복원 프로시저 |

역변환 SQL은 [Migrations/MySQL/Down](Migrations/MySQL/Down)에 있다. 파일 최신 버전은
000005이며, [Auth 검증 계약](../AuthServer/Database/LoginSchemaVerifier.h)의 요구 head는
**5**이다. Auth는 기동 시 전체 적용 이력·실제 구조·배포 SQL을 대조하고 로그인·입장 기능의
준비 여부를 결정한다. 2026-10-08 로컬 DB는 V4 적용 성공과 런타임 검사 권한을 조회로 확인했다.
V5의 실제 적용 결과는 적용 단계에서 별도로 확인하며, 다른 대상 DB의 버전을 추정하지 않는다.

`characters`는 고유 character_id PK, account_id 비고유 IX·FK, character_definition_id(종류),
level, 전역 UK name, skill_points, UTC 생성/수정 시각을 가진다. `character_skills`는
(character_id, skill_id) PK와 skill_level을 가진다. 이름은 UTF-8 1~32바이트이며 대소문자·악센트·
공백을 구분한다. 성장·스킬 트리 규칙은 기존 JSON에 유지한다. 상세 계약과 향후 SP 원자적 사용은
[캐릭터 스키마](../../docs/workflows/DATABASE_MIGRATIONS.md#캐릭터습득-스킬-스키마-v000004)를 따른다.

V5의 character_items는 장비 한 개 또는 스택 하나를 한 행으로 저장한다. instance_id는
서버가 생성한 32자리 lowercase hex를 BINARY(16)으로 저장하며, definition_id는 ASCII ID다.
container 0/1/2/3은 장비/재료/소모품/퀘스트 가방, 4는 장착이다. 가방 slot은 0~39,
장착 slot은 0~6(무기/상의/하의/신발/반지/목걸이/팔찌)이며 위치 UK로 중복을 막는다.
아이템 정의·maxStack·장착 조건·능력치·효과는 서버 JSON에 유지한다. DB가 마스터를 복제하지 않는다.

공개 호출은 list_characters/create_character/claim_character/save_character_state/release_character다.
저장은 원본 명령과 요청 ID의 성공 이력을 먼저 확인하고, 소유 토큰·세대 및 revision으로
이전 세션과 오래된 상태의 쓰기를 거절한다. 성장·스킬·가방·장비는 한 CALL에서 함께 저장한다.
SQL 안에서 COMMIT하지 않으며 ODBC가 결과 검증 후 커밋한다.
[전체 인자·결과·동시성 계약](../../docs/workflows/DATABASE_MIGRATIONS.md#캐릭터인벤토리-저장복원-v000005)을 따른다.

기존 head=4는 새 Auth에서 준비 실패한다. 서비스 정지 → 수동 Up4→5 → Auth/Town 새 검사와 Town 공개
프로시저 EXECUTE 부여 → 같은 SQL 11개와 새 Auth/Town 배포 → head=5 검증 순서다.
내부 emit_character_state에는 런타임 EXECUTE를 부여하지 않는다. V5 Down은 세 신규 테이블이
모두 비어 있을 때만 허용한다. Down 후 감사 이력이 남으므로 과거 바이너리로 즉시 복귀할 수 없다.

## 수동 적용

로컬 최초 설정의 DB 입력·64비트 ODBC 준비·보호 저장·실제 대상 검증은
[DB 준비 계약](../../docs/workflows/DATABASE_MIGRATIONS.md#로컬-최초-설정의-db-준비-계약)을 따른다.
설정 저장이나 MySQL 설치 파일 버전으로 실제 서버 버전·마이그레이션 상태를 판단하지 않는다.
드라이버가 없으면 공식 설치 안내 후 중단하며 자동 설치·DB 생성·마이그레이션 적용은 하지 않는다.

서버 기동 시 마이그레이션을 자동 실행하지 않는다. 운영자가
[UpMigration.bat](../../Tool/Database/UpMigration.bat) 또는
[DownMigration.bat](../../Tool/Database/DownMigration.bat)를 실행하며, 공통 실행기는
[Migrate.ps1](../../Tool/Database/Migrate.ps1)이다.

Up은 모든 미적용 버전을 순서대로 최신화하고, Down은 현재 head 한 단계만 되돌린다.
이력 기반 000000은 유지하며, 계정 테이블을 삭제하는 000001 Down은 두 테이블이 비어 있어야 한다.
대상 스키마를 별도로 준비하거나 Up의 `-CreateDatabase`로 생성한다. DB 사용 서비스를 정지한 뒤,
64비트 MySQL ODBC 드라이버와
보호된 `ACTIONRPG_MIGRATION_CONNECTION_STRING` 환경 변수를 제공한다. 접속 비밀은 Git에 넣지 않는다.

`-CreateDatabase`는 MySQL 8.0.46을 확인하고 이름 기반 잠금을 잡은 후 DB가 없을 때만
utf8mb4 / utf8mb4_0900_ai_ci로 생성한다. 그 접속에서 DB를 선택하고 기존 이력·구조 검사를
수행한다. DB 생성은 버전 이력 이전 단계이며 후속 실패에도 DB가 남는다. 실행 주체의 CREATE
권한이 필요하고 계정 생성·권한 부여는 별도다. 이름·접속 문자열 조건은 아래 툴 안내를 따른다.

실행 명령과 인자는 [툴 README의 DB 안내](../../Tool/README.md#db-마이그레이션)를 따른다.
현재 버전 조회에 Up을 사용하면 대기 SQL이 적용될 수 있다. 실패나 부분 적용 시 이력을 지우거나
SQL을 재실행하지 말고 상세 문서의 조회·복구 절차를 따른다.

## 상세 문서

* [새 Windows PC 설정·트러블슈팅](../../docs/workflows/LOCAL_DEVELOPMENT_SETUP.md):
  신규 설치 순서, DB 계정·권한·마이그레이션 전체 예제와 실제 오류 해결 기록.
* [DB 구현·마이그레이션 사용 및 작업 규칙](../../docs/workflows/DATABASE_MIGRATIONS.md):
  스키마·프로시저 계약, 권한·체크섬·감사 이력, 현재 버전 조회, 실패 복구와 지원 한계.
* [AuthServer 개발 계약](../AuthServer/DEVELOPMENT.md): Google 로그인·세션·입장 흐름과 배포 전제.

이 README는 소스 기준 안내다. 이번 작업에서는 빌드·서버 실행·DB 접속·SQL 적용을 수행하지 않았다.
