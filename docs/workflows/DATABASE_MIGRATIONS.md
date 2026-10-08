# DB 구현·마이그레이션 사용 및 작업 규칙

문서 버전: 1.4.0

작성일: 2026-10-05

수정일: 2026-10-08 — V000005 캐릭터·인벤토리 저장/복원·소유권·중복 요청 계약 추가.

적용 범위: ActionRPG 총괄·DB 담당, DB 연계가 필요한 서버 및 다른 담당

## 1. 담당 범위와 현재 상태

DB 담당은 데이터 모델, 스키마·인덱스·제약·프로시저, 영속 데이터 변경, 마이그레이션 파일과 적용 이력, DB 이동 시 호환성과 데이터 변환을 맡는다. 서버의 접속·쿼리 호출·게임 상태 연계 코드는 서버 담당과 계약을 정하고 총괄이 수정 파일 소유자를 배분한다.

작업 저장소는 `C:/Users/KimHyeongJin/source/repos/ActionRPGServer`다. 클라이언트 저장소는 `C:/Users/KimHyeongJin/source/repos/ActionRPGClient`다. 공통 역할·협업 규칙은 같은 디렉터리의 `DUNGEON_CREATION.md`를 따른다.

2026-10-08 시점의 저장소 구현을 기준으로 설명한다. 초기 담당 온보딩 당시의 DB 미구현 설명을
현재 상태로 사용하지 않는다. 현재 서버는 AuthServer, TownServer, GameRoomServer로 책임을
나누며 계정 DB 호출은 AuthServer가 소유한다. DB 전용 서버 프로세스는 구현하지 않았다.

| 구분 | 현재 소스 구현 | 실제 환경에서 확인한 상태 |
|---|---|---|
| DBMS | MySQL 8.0.46 / InnoDB에 한정한 SQL·검증 | 사용자 로컬 조회 결과 8.0.46; 에이전트 직접 접속·엔진 검증 미수행 |
| 런타임 접근 | `Shared/Database`의 ODBC 풀·작업 큐·`IStoreProcedure<Req, Res>` | 2026-10-07 사용자가 로컬 Google 로그인 후 정상 입장 확인; 에이전트 직접 왕복 검증 미수행 |
| 영속 구조 | 계정/외부 식별자 V000001, 로그인 V000002, 상태 조회 V000003, 캐릭터/습득 스킬 V000004, 저장/복원 V000005 | 로컬 V4 이력·검사 권한 확인; V5 적용 별도 |
| 마이그레이션 | PowerShell 5.1 / System.Data.Odbc 수동 Up/Down, V000000 이력 기반 | 사용자 제공 V0/V1 복구·V2/V3 Up 완료 로그, 종료 코드 0 |
| 서버 검증 | Auth 기동 시 DB 이력·실제 구조와 배포 SQL을 대조, 요구 head=5 | 이전 V3 로컬 입장 확인; 로컬 V4 적용·검사 EXECUTE 조회 확인; V5 신규 구현·Auth 왕복은 미검증 |

에이전트는 이 문서 작업에서 실제 DB에 접속하거나 적용하지 않았다. 위 로컬 결과는 사용자 로그와
확인에 근거하며 다른 DB의 상태를 보장하지 않는다. 파일 최신 버전 000005, 새 Auth 요구 버전 5,
실제 대상 DB의 현재 버전은 구분한다. 직전 로컬 V4 조회 확인은 이번 V5 적용을 증명하지 않는다. 새 PC 절차와 실제 오류 사례는
[로컬 설정·트러블슈팅](LOCAL_DEVELOPMENT_SETUP.md), 사용 계약은 9절, 테이블·프로시저 계약은 8절을 따른다.

로그인 흐름과 서버 구성 상세는 [AuthServer 개발 계약](../../ActionRPGServer/AuthServer/DEVELOPMENT.md),
마을 상태·콘텐츠 연계는 [TownServer 개발 가이드](../../ActionRPGServer/TownServer/DEVELOPMENT.md)를
참조한다. 이 문서는 DB와의 계약을 설명하며 서버 API 설명을 중복 관리하지 않는다.

이 문서는 작업 규칙과 소스 실행 계약이다. 파일 작성으로 실제 DB 적용을 증명하지 않는다.

## 2. 공통 작업 원칙

* 사용자 지시와 적용되는 AGENTS.md가 우선한다. 코드 수정은 기술적 근거와 `## Plan` 번호 목록을 먼저 제시하고 사용자 Approve 후 진행한다. 명시적으로 즉시 진행하라는 사용자 지시는 그 범위의 승인으로 취급한다. 단순 오타는 예외다.
* 중요한 가정을 밝히고, 필요한 최소 범위만 수정한다. 다른 담당의 변경을 되돌리거나 관련 없는 리팩토링을 하지 않는다.
* C++에서는 지역·멤버 변수 camelCase, 함수·클래스·구조체 PascalCase, 충돌하는 인자에는 in 접두사, 상수·매크로 UPPER_SNAKE_CASE를 사용한다. RAII·스마트 포인터를 따르고 복잡한 로직의 의도를 주석으로 남긴다.
* 기존 Town/GameRoom strand의 상태 소유권과 스레드 안전성을 유지한다. DB I/O가 게임 처리 스레드를 막는지, 완료 콜백의 상태 접근이 올바른 소유자에게 돌아오는지 검토한다.
* 성능 저하, 잠금, 데이터 손실, 보안 또는 스레드 안전성 문제가 예상되면 계획에서 구체적으로 설명한다. 접속 문자열·암호·토큰·개인정보를 Git이나 보고서에 기록하지 않는다.
* 별도 승인 없이 빌드·기능 테스트·게임/서버 실행·프로세스 종료를 하지 않는다. 실제 DB 접속, 마이그레이션 적용, 데이터 이동은 대상과 실행 범위를 확인하여 승인받은 작업에서만 수행한다.
* before/backup 사본을 만들지 않는다. 소스 복구는 Git 이력을 사용한다. Git 이력은 DB의 실제 저장 데이터를 복원하지 못하므로 파괴적인 데이터 변경의 복구 방법은 적용 전에 별도로 협의한다.
* 수정 완료 후 최종 응답에 대표 제목부터 `*`와 하위 들여쓰기로 커밋 로그를 출력한다. 로그 출력만으로 stage/commit/push하지 않는다. 사용자의 별도 Git 실행 지시가 있을 때 수행한다.
* 총괄이 작업과 파일 소유자를 배분한다. 다른 담당 채팅으로 메시지를 보내거나 새 담당·서브에이전트를 만들려면 사용자에게 직접 승인받은 범위가 있어야 한다. 답신 권한이 없으면 결과와 연계 사항을 자신의 대화·산출물에 남기고 총괄이 취합한다.

## 3. DB 변경은 반드시 마이그레이션 파일로 관리

1. 테이블·컬럼·타입·제약·인덱스·뷰·함수·프로시저 변경, 기준 데이터 등록, 기존 데이터 일괄 보정은 모두 검토 가능한 버전 마이그레이션 파일로 남긴다. 관리 화면에서만 DDL/DML을 실행하고 파일을 생략하지 않는다.
2. 게임의 정상 플레이에 따른 저장·갱신은 일반 트랜잭션이며 매 요청마다 마이그레이션을 만들지 않는다. 구조 변경과 일괄 데이터 변환에 이 규칙을 적용한다.
3. 버전은 전체 마이그레이션 순서에서 유일하고 증가해야 한다. 예를 들어 `V000001__initial_schema`처럼 버전과 목적을 식별한다. 실제 확장자·폴더·도구는 DBMS와 실행기 승인 후 확정한다. 이 예시는 등록된 실행 명령이 아니다.
4. 이미 공유 환경에서 성공 적용한 파일은 수정·삭제·재번호 부여하지 않는다. 후속 변경은 새 버전으로 추가한다. DBMS별 실행 파일의 체크섬도 적용 이후 고정한다.
5. 빈 DB를 현재 스키마까지 만들 수 있도록 초기 구조부터 이력을 보존한다. 기존 DB를 최초 도입하는 경우 실제 스키마와 검증한 초기 기준 버전을 명시해야 하며, 검증 없이 최신 버전을 적용됐다고 등록하지 않는다.
6. 버전·이름·선행 버전·목적·대상 DBMS·실행 파일·체크섬을 추적한다. 파괴적 변경은 영향 범위, 예상 잠금/시간, 실패 복구와 서버 코드 배포 순서를 함께 기록한다. 기존 도구의 기능을 재사용하고 임의 실행 프레임워크를 먼저 만들지 않는다.

## 4. 대상 DB 자체의 적용 이력

* 각 실제 DB는 자체 마이그레이션 이력 테이블을 가진다. 도구의 기존 테이블이 있으면 활용한다. 없다면 승인된 실행기 구현에서 `schema_migrations` 같은 테이블을 마련한다.
* 성공 적용한 버전, 이름, 해당 DBMS 실행 파일의 체크섬, 적용 시각을 DB에 기록한다. 도구가 지원하는 실패 상태·수행 시간 등도 활용하되 성공 버전과 혼동하지 않는다.
* 현재 버전은 단순한 최대 번호가 아니라, 파일/이력의 순서·선행 관계·체크섬을 대조해 누락 없이 성공 적용한 마지막 버전으로 판단한다.
* 시작 전 대상 DB와 DBMS를 확인하고 적용 이력·대기 파일을 대조한다. 이미 성공한 같은 버전은 다시 적용하지 않는다. 체크섬 불일치, 버전 누락, 알 수 없는 적용 버전이나 이전 실패가 있으면 중단하고 원인을 보고한다.
* 성공 기록은 해당 변경의 실제 성공 후에만 남긴다. 적용되지 않은 변경을 파일 존재나 서버 시작만으로 성공 처리하지 않는다. 이력 테이블 수동 수정으로 검증을 우회하지 않는다.
* 서버 코드가 요구하는 스키마 버전과 실제 DB 버전의 호환 범위를 확인한다. 호환되지 않는 DB에 서비스를 연결하는 방식으로 진행하지 않는다.

## 5. 실행과 실패 처리

1. 먼저 현재 버전, 목표 버전, 적용 파일 목록과 영향을 조회 가능한 형태로 제시한다. 이후 승인된 대상에만 적용한다.
2. 동일 DB에 마이그레이션을 적용하는 실행자는 한 번에 하나다. 여러 서버/프로세스가 동시에 시작해도 중복 실행하지 않도록 DBMS/도구가 제공하는 잠금으로 직렬화한다. 개별 프로세스의 메모리 락만으로 다중 실행자 문제를 해결했다고 보고하지 않는다.
3. 트랜잭션과 DDL 롤백 가능 여부는 선정한 DBMS·버전·도구의 실제 지원 범위로 판단한다. 지원될 때는 변경과 성공 이력 기록을 일관되게 처리한다.
4. 비트랜잭션 DDL, 장시간 데이터 변환 등은 중간 실패 지점과 재개/복구 절차를 명시한다. 일부 적용된 파일을 무조건 처음부터 재실행하거나 성공 이력만 덧붙이지 않는다.
5. 실패 시 후속 버전을 적용하지 않는다. 실제 DB 상태·성공 이력·실패 위치를 확인한 뒤 승인된 복구를 진행한다. 적용 파일의 과거 내용을 고쳐 이력과 달라지게 하지 않는다.
6. 되돌리기 파일은 실제 안전하게 역변환 가능한 경우만 제공한다. 컬럼 삭제·데이터 손실을 자동 복원할 수 있다고 주장하지 않는다. 되돌릴 수 없는 변경은 사전 대응과 이후 보정 계획을 협의한다.
7. 서버 기동 시 자동 마이그레이션이나 운영 데이터 변경을 임의로 활성화하지 않는다. 실행 주체·시점·환경은 별도 구현 계획에서 결정한다.

## 6. 다른 DB로 이동할 때

### 동일 DBMS의 새 DB 또는 인스턴스

현재 실행기는 같은 MySQL 8.0.46의 빈 목적 스키마에 구조를 만드는 데 사용할 수 있다.
Up의 `-CreateDatabase`로 목적 DB가 없을 때 생성할 수 있다. 덤프/복원, 계정 데이터 전송,
접속 주소 전환, 자동 비교·이관 도구는 구현하지 않았다.
아래 항목은 이동 시 지켜야 할 규칙이며 자동 이관 기능의 설명이 아니다.

* 같은 원본 마이그레이션 목록을 초기 버전부터 목표 버전까지 순서대로 적용한다. 목적지 DB에 실제 적용 이력을 기록하고 목표 버전에 도달했는지 확인한다.
* 데이터가 있는 DB의 이전은 스키마 생성과 실제 데이터 이관을 구분한다. 이력만 복사하고 구조·데이터를 생성했다고 보고하지 않는다.
* DB 복제/덤프에 이력이 함께 포함됐다면 실제 스키마·체크섬과 대조한 후 미적용 버전만 적용한다. 대상 환경 차이도 확인한다.

### 다른 DBMS 엔진

현재 다른 MySQL 버전, MariaDB, PostgreSQL, SQL Server 등의 SQL·타입 변환·마이그레이션
실행 어댑터는 제공하지 않는다. ODBC는 접속 API이며 MySQL SQL의 다른 엔진 호환성을 보장하지
않는다. 실행기와 Auth 검증은 MySQL 8.0.46 계약을 검사하므로 드라이버만 바꿔 이동할 수 없다.

* 공통으로 유지할 것은 논리적인 마이그레이션 버전·의존 순서·변경 의미다. 모든 DBMS가 동일 SQL을 실행할 수 있다고 가정하지 않는다.
* 사용자가 DBMS 이동을 승인하면 필요한 엔진별 실행 파일/변환을 동일 논리 버전에 대응시킨다. 해당 엔진의 초기 버전부터 목표 버전까지 필요한 파일이 모두 준비되어야 한다. 아직 요구되지 않은 모든 DBMS용 구현을 미리 만들지 않는다.
* 엔진별 SQL과 체크섬은 별도로 관리한다. 목적지 이력에는 실제 실행한 목적지 파일의 체크섬을 저장하며 원본 엔진의 체크섬이나 성공 이력을 실행 대신 복사하지 않는다.
* 타입·정밀도·자동 증가/시퀀스·NULL·문자열 비교/정렬·시간대·제약·인덱스·프로시저·트랜잭션 차이와 실제 데이터 변환을 검토한다. 기본키/참조 관계를 보존하고 이관 결과를 확인할 계획을 세운다.
* 완료 조건은 목적지의 스키마·이력·실제 데이터가 합의한 목표 버전 및 서버 계약과 일치하는 것이다. 버전 번호만 동일한 상태를 이관 완료로 표시하지 않는다.

## 7. 작업 요청과 완료 보고

요청에는 DBMS/버전, 대상 환경, 현재/목표 마이그레이션 버전, 데이터 유지 조건, 수정 담당과 파일 범위, 서버 의존성, 승인된 실행·검증 범위를 포함한다. 정해지지 않은 항목은 현황을 먼저 확인하고 총괄에 제안한다.

DB 작업에 따른 서버 연계가 필요하면 DB 담당은 스키마/쿼리 계약·요구 버전을 기록한다. 총괄이 서버 담당에게 배분하고 양쪽 결과를 확인한다. 마이그레이션 파일 작성, 정적 확인, 실제 DB 적용, 서버 연결, 데이터 이관 검증은 각각 완료 여부를 보고한다.

완료 보고에는 다음을 남긴다.

* 실제 추가/수정 파일과 버전·목적·체크섬 및 적용 순서.
* 대상 DB와 변경 전후의 실제 버전. 해당 작업에서 실행하지 않은 것은 미실행으로 명시하고,
  실제 DB 적용 상태를 조회하지 않았으면 버전을 미확인으로 기록한다.
* 실패/복구 가능성, 서버 호환성과 배포 순서, 남은 의존 작업.
* 수행한 확인과 하지 않은 빌드·테스트·실행을 구분한 결과.
* 사용자 형식의 `*` 커밋 로그. 실제 Git 실행 여부는 별도로 명시.

새 DB 담당의 첫 작업은 이 문서와 기존 프로젝트 현황을 읽고 역할·마이그레이션 원칙·미정 항목을 확인하는 것이다. 별도 승인 없이 실제 DB 도입, 마이그레이션 실행기 구현, 이력 테이블 생성이나 영속화 기능을 시작하지 않는다.

## 8. AuthServer 분리 DB 계약

2026-10-06 승인된 분리 작업의 DB 계약이다. AuthServer 한 대가 Google 인증과 계정 DB
호출·메모리 게임 세션을 소유하고, TownServer는 대상 서버 전용 일회 입장 티켓을 AuthServer에
확인한다. 계정 프로시저는 AuthServer만 호출하며 TownServer에 계정 DB 실행 권한을 주지 않는다.
세션·티켓·로그아웃을 위한 영속 테이블과 재접속 토큰·캐릭터·성장 테이블은 추가하지 않는다.

### SQL 버전과 적용 조건

대상은 MySQL 8.0.46 / InnoDB이며 파일 경로는 `ActionRPGServer/Database/Migrations/MySQL/`다.
기존 SQL의 `Target: MySQL 8.0.47` 주석은 작성 당시 표기로 유지한다. SQL 파일의 주석도
체크섬 대상이므로 버전 정정은 실행기·Auth의 대상 검사와 이 문서에 반영하며 SQL은 변경하지 않는다.
8.0.46에서의 실제 적용·로그인 동작은 이 정적 수정만으로 검증되지 않는다.

| 버전 | 선행 버전 | 실행 파일 | 목적 |
|---|---|---|---|
| 000000 | 빈 스키마 | [Infrastructure/V000000__migration_history.sql](../../ActionRPGServer/Database/Migrations/MySQL/Infrastructure/V000000__migration_history.sql) | 초기 이력 테이블·조회 프로시저; Down 없음 |
| 000001 | 000000 기반 | [V000001__create_login_accounts.sql](../../ActionRPGServer/Database/Migrations/MySQL/V000001__create_login_accounts.sql) | 계정·외부 식별자 테이블 |
| 000002 | 000001 | [V000002__create_google_login_procedure.sql](../../ActionRPGServer/Database/Migrations/MySQL/V000002__create_google_login_procedure.sql) | Google 로그인 조회·자동 가입 |
| 000003 | 000002 | [V000003__create_auth_account_status_procedure.sql](../../ActionRPGServer/Database/Migrations/MySQL/V000003__create_auth_account_status_procedure.sql) | 티켓 발급·입장 승인 전 계정 상태 재확인 |
| 000004 | 000003 | [V000004__create_characters_and_skills.sql](../../ActionRPGServer/Database/Migrations/MySQL/V000004__create_characters_and_skills.sql) | 캐릭터·습득 스킬 테이블과 확장 검사 |
| 000005 | 000004 | [V000005__create_character_inventory_persistence.sql](../../ActionRPGServer/Database/Migrations/MySQL/V000005__create_character_inventory_persistence.sql) | 영속 캐릭터·인벤토리 저장/복원·소유권·중복 요청 |

기존 Infrastructure와 000001~000003 Up/Down SQL은 변경하지 않는다. `login_google_account(subject)`의 결과는 기존대로
`result_code(int32), account_id(uint64), account_status(int32), was_created(int32)` 한 행이다.
0 정상/1 정지, was_created 0/1, 모든 값 NOT NULL, 정지에서는 was_created=0 계약을 유지한다.
새 AuthServer가 요구하는 논리 버전은 000005다. 향후 상위 버전 호환성은 별도로
선언하며 단순히 `MAX(version) >= 4`이라는 이유로 허용하지 않는다.

적용 도구와 이력 계약은 9절처럼 구현했으며 실제 대상 DB와 적용 상태는 이 문서 작업에서 미확인이다.
SQL 파일 작성과 서버 분리 승인은 설치·DB 접속·마이그레이션 실행 승인에 해당하지 않는다.
000001은 두 DDL 문장이 각각 커밋되므로 일부 생성 후 실패할 수 있다. 000002/000003은
DELIMITER 처리가 필요한 CREATE PROCEDURE이며 이력 기록과 DDL 전체를 하나의 롤백 가능한
트랜잭션으로 가정하지 않는다. 중간 실패 시 후속 적용과 성공 기록을 중단하고 상태를 확인한다.
부분 실패를 자동 DROP/undo하거나 IF NOT EXISTS로 숨겨 재적용하지 않는다. 성공 적용된
최신 버전을 수동으로 되돌리는 별도의 Down SQL만 제공한다.
DEFINER는 생성 주체를 사용하므로 실제 적용 전에 전용 주체의 수명·최소 권한을 정해야 한다.
AuthServer에는 필요한 프로시저 실행 권한과 향후 이력 조회 권한만 부여하며 DDL·이력 쓰기
권한을 주지 않는다. 사용자 생성·GRANT와 서버 기동 시 자동 적용은 포함하지 않는다.

### 계정 상태 재확인: `get_auth_account_status`

입력은 AuthServer가 인증된 메모리 세션에 연결한 `account_id` 하나다.
SQL 타입은 BIGINT UNSIGNED, C++ 바인딩 타입은 uint64_t이며 NULL·0을 거절한다.
클라이언트나 TownServer가 주장한 계정 ID만으로 호출·승인하지 않는다.

결과는 정확히 한 결과셋·한 행·세 컬럼이며 다음 순서다.

| 순서 | 컬럼 | C++ 읽기 타입 / 의미 |
|---|---|---|
| 1 | `result_code` | int32_t, NOT NULL: 0 정상, 1 정지, 2 계정 없음 |
| 2 | `account_id` | uint64_t, NOT NULL: 요청 계정 ID와 일치, 0보다 큼 |
| 3 | `account_status` | optional<int32_t>: 정상 0, 정지 1, 계정 없음에서만 NULL |

서버는 행·결과셋·컬럼 개수와 위 값 조합을 커밋 전에 검사한다. 통신·커밋 성공과 게임 입장
허용을 구분한다. result_code=0만 승인 가능하며 정지/계정 없음/DB 오류/무효 결과는 거절한다.
이 조회는 인증 증명이 아니며 Google 로그인이나 마지막 로그인 시각 갱신을 대신하지 않는다.

기존 ODBC 실행기가 autocommit OFF로 전체 트랜잭션과 결과 처리·커밋을 소유한다.
프로시저는 FOR SHARE 잠금 조회로 현재 커밋된 상태를 읽고 내부 DDL·커밋·세션 변경을 하지 않는다.
계정 상태 변경과 잠금 대기가 발생할 수 있으므로 AuthServer 상태 소유 스레드에서 DB I/O를
기다리지 않고, DB 타임아웃·오류에는 자동 재시도나 승인 우회를 하지 않는다.

AuthServer는 티켓 발급 전, TownServer의 티켓 소비·입장 승인 요청과 타운 lease 갱신 시 이 조회를
새로 수행한다.
DB 응답 후 메모리 세션의 생존·세대, 티켓의 미사용·만료·대상 서버를 같은 상태 소유 실행 문맥에서
다시 검사한 뒤 생성/소비해야 한다. 조회 중 로그아웃된 세션이나 취소된 티켓은 늦은 성공 응답으로
되살리지 않는다. 정지/계정 없음으로 확인한 세션과 미사용 티켓은 무효화한다.

DB 조회의 잠금은 커밋 후 해제된다. 따라서 조회 직후의 계정 정지를 메모리 티켓 생성/소비 및
이미 입장한 Town 세션까지 원자적으로 반영하지는 않는다. 즉시 정지·강제 퇴장이 필요하면
정지 변경 경로와 Auth/Town 세션 취소 통지를 함께 설계하는 별도 작업이 필요하다.
로그아웃 자체는 DB 조회나 정상 계정 상태를 전제로 하지 않고 메모리 세션·미사용 티켓을
폐기해야 한다. 실제 Town 연결 종료는 서버 간 취소 통신 범위에 따른다.

근거: [MySQL 잠금 조회](https://dev.mysql.com/doc/refman/8.0/en/innodb-locking-reads.html).

### 적용 이력 검증 계약

승인된 도구는 `schema_migrations` 이력과 `get_schema_migration_history()` 조회를 사용한다.
정확한 결과·체크섬·잠금 계약은 9절을 따른다. `SELECT 3`, 설정 파일의 성공 버전 플래그,
프로시저 존재, 소스 파일의 체크섬만으로 실제 적용 이력 검증을 대체할 수 없다.

실제 이력을 읽는 서버 어댑터는 다음 정보를 검증한다.

* 대상 DB 식별과 실제 DBMS·버전, 실행 파일/마이그레이션 이름, 논리 버전과 선행 관계.
* 실제 실행 순서와 성공·실패 상태, 성공 적용 시각, 실행 파일의 체크섬과 알고리즘/정규화 규칙.
* 실패·누락·알 수 없는 버전을 숨기지 않은 전체 관련 이력. 단순 성공 행의 MAX 조회는 불충분하다.

AuthServer는 승인된 배포 SQL 목록을 기준으로 000001 → 000002 → 000003 → 000004 → 000005가 누락 없이 성공했고,
이름·순서·실행 파일 체크섬이 해당 도구의 규칙과 일치하는지 확인한다. 원본 SHA-256과 도구의
다른 알고리즘 체크섬을 같은 값으로 비교하지 않는다. 도구가 선행 관계를 저장하지 않으면 승인된
버전 파일 순서와 실제 이력을 대조한다. 이후 실패 이력·미지원 버전·체크섬 불일치도 거절한다.
필요한 테이블/프로시저의 실제 계약 역시 확인하며 이력만으로 구조가 동일하다고 가정하지 않는다.

검증은 실제 사용할 계정 DB를 대상으로, 마이그레이션 실행자 잠금과 서비스/DDL 배포 순서가
협의된 상태에서 수행한다. 검증 도중 변경된 이력이나 서비스 중 임의 DDL을 허용하지 않는
배포 절차가 필요하다. 조회 불가·대상 불명·검증 실패 시 로그인·새 티켓 발급/입장 승인을 차단하며,
성공을 가정하는 증명 객체를 생성하지 않는다. 로그아웃과 이미 발급된 권한 취소는 계속 허용한다.

남은 배포 결정은 실제 대상 DB/스키마·환경, ODBC 드라이버와 접속 보안, 실행 주체의 최소 권한,
서비스 정지·코드 배포 순서다. 000003은 계정 상태 조회이며 V0 이력 조회는 Infrastructure,
캐릭터 구조까지 포함한 새 Auth 이력 조회는 V000005의 확장 검사다.

### 계정 스키마와 Google 매핑

V000001은 다음 두 InnoDB 테이블을 만든다. DATETIME(6)은 UTC 의미로 사용하는 시각이며
생성·가입·허용 로그인 시 프로시저가 `UTC_TIMESTAMP(6)`을 넣는다. DB가 자동으로 시각을
채우는 컬럼 기본값이나 갱신 트리거는 없다.

| 테이블·컬럼 | SQL 타입·NULL·기본값 | 의미 |
|---|---|---|
| accounts.account_id | BIGINT UNSIGNED, NOT NULL, AUTO_INCREMENT | 내부 게임 계정 ID |
| accounts.status | INT, NOT NULL, DEFAULT 0 | 0 정상, 1 정지 |
| accounts.created_at | DATETIME(6), NOT NULL, 기본값 없음 | 계정 생성 시각 |
| accounts.last_login_at | DATETIME(6), NULL 허용 | 마지막으로 허용한 DB 로그인 시각 |
| account_identities.account_id | BIGINT UNSIGNED, NOT NULL | 연결된 내부 계정 ID |
| account_identities.provider | VARBINARY(32), NOT NULL | 현재는 바이트 값 `google`만 허용 |
| account_identities.subject | VARBINARY(255), NOT NULL | Google 검증 토큰의 `sub` |
| account_identities.linked_at | DATETIME(6), NOT NULL | 외부 식별자 연결 시각 |

* accounts의 기본 키는 account_id이며 `ck_accounts_status`가 status IN (0,1)을 강제한다.
* account_identities의 기본 키는 (provider, subject)다. 문자열 정렬 규칙이 아닌 바이트 값으로
  비교하며, 같은 Google subject를 다른 계정에 중복 연결하지 않는다.
* `ix_account_identities_account_id`는 account_id의 비고유 인덱스다. account_id 자체를 UNIQUE로
  제한하지 않지만, 다른 로그인 제공자와 계정 연결/해제 기능은 현재 구현하지 않았다.
* `fk_account_identities_account`는 같은 스키마의 accounts.account_id를 참조하며 UPDATE/DELETE
  모두 RESTRICT다. 식별자가 있는 계정의 ID 변경·삭제를 자동 전파하지 않는다.
* `ck_account_identities_provider`는 provider=_binary'google', `ck_account_identities_subject`는
  OCTET_LENGTH(subject)>0을 강제한다. ASCII·길이 제한은 로그인 프로시저와 C++ 입력에서도 검사한다.

외부 식별자와 내부 계정 ID를 분리한다. 이메일은 계정 키나 계정 합치기 기준으로 사용하지 않으며
이름·프로필·이메일·Google ID/access/refresh 토큰을 저장하는 컬럼도 없다. 동일 sub의 정상 로그인은
같은 account_id를 반환하고 최초 로그인은 계정과 식별자를 같은 런타임 트랜잭션에서 자동 생성한다.
계정 정지 값을 변경하는 관리 API와 즉시 퇴장 통지는 현재 DB 계약에 포함하지 않는다.

### Google 로그인: `login_google_account`

입력 `inSubject`는 TEXT CHARACTER SET utf8mb4 COLLATE utf8mb4_bin이다. C++의
`LoginGoogleAccountRequest.subject`는 std::wstring이며 1~255개의 ASCII 문자, NUL 제외 조건을
검사한다. SQL도 바이트 길이 1~255, ASCII, NUL 제외를 확인하고 내부적으로 binary로 비교한다.
DB는 Google 토큰을 검증하지 않는다. AuthServer가 서명·issuer/audience·유효 시각·nonce 등을
검증한 후 추출한 subject만 전달한다. 클라이언트가 지정한 subject를 그대로 신뢰하지 않는다.

결과는 정확히 한 데이터 결과셋·한 행·네 컬럼이며 모두 NOT NULL이다.

| 순서 | SQL 컬럼 / C++ 응답 필드 | 타입·허용 값 |
|---|---|---|
| 1 | result_code / resultCode | int32_t, 0 정상 또는 1 정지 |
| 2 | account_id / accountId | uint64_t, 0보다 큼 |
| 3 | account_status / accountStatus | int32_t, result_code와 같음 |
| 4 | was_created / wasCreated | SQL int32_t 0/1을 C++ bool로 매핑; 정지에서 0 |

기존 식별자가 없으면 새 계정을 생성한다. 같은 subject의 동시 가입으로 식별자 INSERT에서
1062 충돌이 발생하면 SAVEPOINT 이후의 자신의 계정 생성을 되돌리고, 잠금 조회로 승자의 기존
식별자/계정을 읽는다. 다른 오류는 외부 실행기로 전파한다. 정상 계정만 last_login_at을 갱신하며
정지 계정은 갱신하거나 게임 세션을 발급하지 않는다. AUTO_INCREMENT 값의 연속성을 요구하지 않는다.

로그인·상태 조회 프로시저는 `SQL SECURITY DEFINER`이며 runtime ODBC가 autocommit OFF의
트랜잭션을 소유해야 한다. 내부에서 전체 COMMIT/ROLLBACK·DDL을 실행하지 않는다. C++ 어댑터는
행/컬럼/결과셋/값 조합을 커밋 전에 검사한다. 프로시저 호출 성공만으로 로그인 허용을 판단하지 않는다.

서버 코드에서는 `LoginGoogleAccountProcedure` 또는 `GetAuthAccountStatusProcedure` 객체 하나를
만들고 `req.subject`/`req.accountId`를 채운 뒤 `OdbcDatabase::Run`으로 소유권을 넘긴다.
게임 요청 코드가 SQL 문자열을 작성하지 않는다. 응답·오류는 지정한 executor로 전달되며 DB 작업은
전용 worker가 처리한다. Auth의 HTTP worker는 최대 15초 기다리는 현재 구현이며 전투 프레임은
대기 주체가 아니다. ODBC는 전체 결과·출력 값을 처리하고 유효성 검사 후 커밋한다. 완료 여부가
불확실한 통신·커밋 실패를 자동 재시도해 중복 가입/갱신을 만들지 않는다.

### 캐릭터·습득 스킬 스키마: V000004

V4는 기존 계정·Google 계약을 유지하고 characters/character_skills 두 InnoDB 테이블을 추가한다.
초기 행은 만들지 않는다. 이번 작업은 스키마·검증 계약이며 캐릭터 생성/조회/저장 프로시저와
V4 단독 작업에는 런타임 복원·저장을 포함하지 않았으며 현재 V5 계약이 이를 추가한다.

| 테이블·컬럼 | SQL 타입·키·범위 | 의미 |
|---|---|---|
| characters.character_id | BIGINT UNSIGNED, AUTO_INCREMENT, PK | 영속 캐릭터 고유 ID; uint64_t |
| characters.account_id | BIGINT UNSIGNED, 비고유 ix_characters_account_id, accounts FK | 계정당 여러 캐릭터 |
| characters.character_definition_id | INT UNSIGNED, >0 | 기존 characterId/dataId의 종류 ID; 고유 PK/임시 playerId와 다름 |
| characters.level | INT UNSIGNED, 1~1000000 | 현재 CharacterProgression 파서 범위 |
| characters.name | VARCHAR(32), utf8mb4/utf8mb4_0900_bin, 전역 UK | CHECK로 UTF-8 1~32바이트 제한 |
| characters.skill_points | INT UNSIGNED, 0~4294967295 | 미사용 보유 SP |
| characters.created_at / updated_at | DATETIME(6), NOT NULL, 기본값 없음 | 작성자가 UTC 생성/수정 시각을 함께 저장 |
| character_skills.character_id | BIGINT UNSIGNED, characters FK | 습득 정보의 소유 캐릭터 |
| character_skills.skill_id | VARBINARY(64), 비어 있지 않음 | Catalog::IsId의 ASCII 스킬 ID |
| character_skills.skill_level | INT UNSIGNED, 1~1000000 | 습득 단계; 미습득 0은 행 생략 |

character_skills의 PK는 (character_id, skill_id)이며 선두 컬럼이 FK 인덱스 역할을 하므로
중복 인덱스는 만들지 않는다. 두 FK 모두 ON UPDATE/DELETE RESTRICT이며 삭제를 자동 전파하지 않는다.
이름 UK는 계정/종류와 무관하게 전체 캐릭터에 적용한다.

이름 길이는 [PlayerSession.cpp](../../ActionRPGServer/TownServer/PlayerSession.cpp)의
playerName.empty()/size()>32 바이트 제한에 맞췄다. 대소문자·악센트·앞뒤 공백을 구분하며
trim·대소문자 접기·Unicode 정규화를 하지 않는다. 기존 서버에도 없는 공백 금지 정책을 임의
추가하지 않는다. Foo/foo/Foo+공백은 다른 이름이다.
[MySQL NO PAD 설명](https://dev.mysql.com/doc/refman/8.0/en/charset-unicode-sets.html)을 따른다.
유효한 UTF-8과 금칙어·표시 정책은 향후 생성 API에서 검증해야 한다.

레벨/SP/시각은 SQL 기본값 없이 서버가 승인된
[CharacterProgression 정책](../../ActionRPGServer/TownServer/Data/CharacterProgression.json)으로
명시한다. 스킬 트리·선행 조건·비용·피해는
[SkillTrees.json](../../ActionRPGServer/TownServer/Data/SkillTrees.json),
[SkillTreeCatalog](../../ActionRPGServer/Shared/SkillTreeCatalog.h),
[PlayerSkills.json](../../ActionRPGServer/TownServer/Data/PlayerSkills.json)의 소유다.
초기 SP0·레벨업당 SP200·앞차기 SP20·단계 요구 레벨+2/피해+5를 DDL에 복제하지 않는다.
종류 ID의 카탈로그 존재, skill_id 문법/종류/선행 조건과 습득 최대 256개는 향후 서버가 검증한다.
파일 정의에 DB FK나 정의 테이블을 임의로 만들지 않는다.

향후 습득 저장은 인증 account_id와 character_id 소유권을 확인하고 같은 트랜잭션에서
characters 행을 FOR UPDATE로 잠근 뒤 레벨·SP·예상 스킬 단계와 카탈로그 조건을 검증해야 한다.
SP 차감·스킬 단계 상승/신규 행·updated_at 갱신을 함께 커밋하거나 모두 롤백한다.
잠금 순서는 캐릭터 행 → 해당 스킬 행으로 통일하고 DB 성공 후 소유 strand에서 상태를 적용한다.
전체 메모리 상태 덮어쓰기로 동시 구매/레벨업의 SP를 잃지 않아야 하며 자동 재시도는 하지 않는다.
원자적 저장 프로시저·멱등성·런타임 상태 연동은 이번 범위 밖이다.

V4는 get_character_schema_migration_history()를 새로 만들고 기존 V0 검사 프로시저를
교체하지 않는다. 같은 DB 이름 기반 잠금 안에서 5테이블·4루틴과 원본 정의를 조회한다.
기존 파일·프로시저·권한을 보존하고 새 EXECUTE만 별도로 준비한다. 세 CREATE는 각각 커밋되므로
중간 실패는 자동 Down/repair하지 않는다. 기존 V0/V1 복구 옵션은 V4 실패를 복구하지 않는다.

V4 당시 전환은 서비스 정지 → 수동 Up3→4 → V4 검사 EXECUTE → V4 Auth와 SQL 9개 배포 →
head=4 검증 → 타운/룸 연결 순서였다. 현재 배포에는 아래 V5 전환 계약을 적용한다.
V4 Down은 두 캐릭터 테이블이 비어 있을 때만 잠금 하에 함께 삭제한 뒤 새 검사 프로시저를 삭제한다.
계정·기존 프로시저·감사 이력은 유지한다. Down 후에도 4UP/4DOWN 이력이 남아 과거 Auth
바이너리는 이를 해석하지 못한다. 되돌린 head=3을 서비스하려면 V4 이력을 이해하는 별도 호환
서버 계약이 필요하며 V4 당시 구현은 head=4만 활성화했으며 현재 V5 계약이 이를 확장한다. V4 재Up 후 새 EXECUTE도 재확인한다.

### 영속 데이터와 메모리·파일 데이터의 구분

| 데이터 | 현재 위치/수명 | DB 저장 여부 |
|---|---|---|
| 내부 계정·정지 상태·로그인 시각 | accounts | 저장하도록 SQL 구현됨; 실제 DB 적용 미확인 |
| Google provider/sub 매핑·연결 시각 | account_identities | 동일 |
| 마이그레이션 시도·방향·체크섬·시각·성공/실패 | schema_migrations | 동일; 감사 이력은 Down에도 유지 |
| 로그인 challenge/nonce, 게임 토큰, 입장 티켓, 타운 연결 소유권/lease | 단일 Auth 프로세스 SessionRegistry | DB 저장 없음; Auth 재시작으로 유실 |
| 플레이어 캐릭터·레벨·SP·습득 스킬·아이템 | Town의 DB 상태와 메모리 캐시, Room 전달 상태 | V5 캐릭터 선택/복원·변경 시 원자적 저장; 실제 왕복은 별도 검증 |
| 던전·몬스터·스킬/성장 정책 정의 | 기존 JSON 등 서버 데이터 파일 | 콘텐츠 정의이며 계정 진행 저장을 대신하지 않음 |
| 우편·인벤토리·재화·재접속 토큰·로그인 감사 이벤트 | 이 DB 마이그레이션에 테이블 없음 | 미구현; 관련 기능 구현을 주장하지 않음 |

DB 연결을 추가했다고 캐릭터 진행의 영속 저장이 생기지 않는다. 타운 간 접속 권한 이동도 진행 데이터
이관을 뜻하지 않는다. 단일 Auth 메모리 세션의 재시작/장애 조건은 Auth 개발 계약을 따른다.

소스 근거: [ODBC 실행기](../../ActionRPGServer/Shared/Database/OdbcDatabase.cpp),
[프로시저 인터페이스](../../ActionRPGServer/Shared/Database/StoreProcedure.h),
[Google 호출 어댑터](../../ActionRPGServer/AuthServer/Database/LoginGoogleAccountProcedure.cpp),
[상태 호출 어댑터](../../ActionRPGServer/AuthServer/Database/GetAuthAccountStatusProcedure.cpp),
[Auth 로그인 연계](../../ActionRPGServer/AuthServer/main.cpp),
[메모리 세션](../../ActionRPGServer/AuthServer/SessionRegistry.h),
[접속 중 진행 상태](../../ActionRPGServer/TownServer/TownInstance.h).

### 캐릭터·인벤토리 저장/복원: V000005

V4 파일은 이미 적용한 체크섬 그대로 보존한다. V5는 세 테이블과 공개 5개·내부 1개·검사 1개
프로시저를 추가한다. 신규 캐릭터나 테스트 아이템, 아이템 마스터, 게임 밸런스 기본값을 seed하지 않는다.

| 테이블 | 저장 내용·키 |
|---|---|
| character_state | character_id PK/FK, revision, owner_generation, nullable BINARY(32) owner_token |
| character_items | BINARY(16) instance_id PK, character_id FK, VARBINARY(64) definition_id, quantity, container, slot; (character_id, container, slot) UK |
| character_operations | (account_id, BINARY(32) request_id) PK, character_id FK, request_kind, payload_hash, 저장 당시 revision, UTC created_at; 성공 요청만 기록 |

container 0/1/2/3은 장비/재료/소모품/퀘스트 가방이며 slot 0~39다. container 4는 장착이며
slot 0~6을 무기/상의/하의/신발/반지/목걸이/팔찌 순서로 사용한다. 장비 가방·장착 수량은 1,
다른 수량은 1~UINT32_MAX다. 위치 UK와 instance PK가 한 아이템의 여러 위치·동일 칸 중복을 막는다.
테이블 CHECK는 분류별 구조와 범위를 검사한다. 아이템의 존재·분류·maxStack·장착 제한과 실제
성장 계산은 신뢰된 Town의 JSON 카탈로그/도메인 책임이다. 클라이언트가 스냅샷이나 이 규칙을 결정하지 않는다.

**공개 프로시저 IN 인자 순서**

- list_characters(inAccountId BIGINT UNSIGNED)
- create_character(inAccountId BIGINT UNSIGNED, inRequestId VARCHAR(64) ASCII ascii_bin,
  inName TEXT utf8mb4 utf8mb4_0900_bin, inDefinitionId INT UNSIGNED,
  inInitialLevel INT UNSIGNED, inInitialSp INT UNSIGNED)
- claim_character(inAccountId BIGINT UNSIGNED, inCharacterId BIGINT UNSIGNED,
  inOwnerToken VARCHAR(64) ASCII ascii_bin, inExpectedOwnerGeneration BIGINT UNSIGNED)
- save_character_state(inAccountId BIGINT UNSIGNED, inCharacterId BIGINT UNSIGNED,
  inOwnerToken VARCHAR(64) ASCII ascii_bin, inOwnerGeneration BIGINT UNSIGNED,
  inRequestId VARCHAR(64) ASCII ascii_bin, inExpectedRevision BIGINT UNSIGNED,
  inOperationJson TEXT utf8mb4 utf8mb4_bin, inProgressionJson TEXT utf8mb4 utf8mb4_bin,
  inInventoryJson TEXT utf8mb4 utf8mb4_bin)
- release_character(inAccountId BIGINT UNSIGNED, inCharacterId BIGINT UNSIGNED,
  inOwnerToken VARCHAR(64) ASCII ascii_bin, inOwnerGeneration BIGINT UNSIGNED)

ownerToken/requestId는 서버의 CSPRNG로 만든 64자리 lowercase hex다. instanceId는 32자리
lowercase hex이며 저장 시 UNHEX, 복원 시 LOWER(HEX)로 변환한다. definitionId와 skill ID는
ASCII 1~64자이며 기존 Catalog::IsId 형식이다. 계정 ID는 검증된 세션에서 가져온다.

모든 공개 프로시저는 **한 데이터 결과셋, 같은 8열**을 반환한다.

| 순서 | 컬럼·타입 |
|---|---|
| 0 | result_code SIGNED 정수 |
| 1 | character_id UNSIGNED BIGINT |
| 2 | revision UNSIGNED BIGINT |
| 3 | owner_generation UNSIGNED BIGINT |
| 4 | name UTF-8 문자열 |
| 5 | definition_id UNSIGNED 정수 |
| 6 | progression_json UTF-8 문자열 |
| 7 | inventory_json UTF-8 문자열 |

list는 캐릭터당 한 행이며 inventory_json은 목록용 빈 items다. 빈 목록/계정 오류는
character_id=0 한 행이고 name/JSON은 빈 문자열이다. 다른 호출은 정확히 한 행이다.
character_id=0 행의 JSON은 파싱하지 않는다. 소유권 실패에는 다른 세션의 상태를 반환하지 않는다.
revision/request 충돌은 현재 소유자가 확인된 경우에만 최신 상태를 반환한다.

| result_code | 의미 |
|---|---|
| 0 | 성공, 동일 성공 요청 재조회 포함 |
| 1 | 계정 없음/사용 불가 |
| 2 | 해당 계정의 캐릭터 없음 |
| 3 | 기대 revision 충돌 |
| 4 | 생성 이름 중복 |
| 5 | 소유 토큰/세대 충돌 |
| 6 | 같은 요청 ID의 종류·원본 명령 불일치 |

잘못된 JSON/ID/범위와 SQL 제약 위반은 SIGNAL/SQL 오류로 전체 롤백한다.
DB 조회 실패·매핑 실패를 새 캐릭터나 빈 가방으로 대체하지 않는다.

**JSON과 중복·동시성**

성장은 기존 {level,skillPoints,skillLevels:{skillId:rank}}다. 가방은
{items:[{instanceId,definitionId,count,container,slot}]}이며 최대 167행이다.
instanceId 32자·definitionId ASCII 최대 64자·count uint32에서 서버 직렬화 최대 29236바이트로
ODBC 32768자 안에 들어간다. 두 스냅샷 각각 UTF-8 32768바이트 이내, operationJson은 JSON
object·UTF-8 2048바이트 이내다. 레벨/스킬 단계는 1~1000000, SP는 uint32, 습득 스킬은 최대 256개다.
SQL은 초기 SP·maxStack·스킬 비용이나 장비 효과를 하드코딩하지 않는다.

잠금은 account→character→character_state→요청 행 순서다. account 잠금으로 같은 계정의
create/save 요청 ID 경쟁도 직렬화한다. 긴 작업이나 네트워크 호출을 이 트랜잭션 안에 넣지 않는다.
현재 설계는 같은 계정의 짧은 DB 변경을 직렬화하므로 동시 다중 캐릭터 활동이 늘면 재검토한다.

목록에서 받은 owner_generation으로 조건부 claim한다. 다른 토큰의 claim은 세대 일치 시에만
세대를 1 증가시키고 이전 세션을 차단한다. 동일 현재 토큰의 재시도는 세대를 증가시키지 않는다.
release는 동일 세대·현재 토큰에만 적용하며 이후 지연된 save를 거절한다. TTL/heartbeat는 추가하지 않는다.
Town의 현재 admission·세션 검사와 함께 사용하며, 늦은 콜백이 새 세션 상태를 덮지 못하게 한다.

save 순서는 계정/캐릭터/현재 토큰·세대 확인 → 성공 요청 종류/해시 조회 → revision 비교 →
스냅샷 구조 검증 → 성장·스킬·아이템 교체 → revision 증가·성공 요청 기록이다.
본문 해시는 SAVE 종류·accountId·characterId·원래 expectedRevision·JSON 정규화한 원본 operationJson으로
DB가 계산한다. 토큰·세대와 재계산한 proposedSnapshot은 해시에서 제외한다.
같은 성공 요청이면 이번 스냅샷을 적용하지 않고 최신 상태만 반환한다.
create는 CREATE 종류·이름·종류·초기 레벨/SP로 별도 해시를 계산한다.

클라이언트 명령을 재시도할 때 requestId·원래 revision·원본 명령을 유지한다. 오래된 revision의
요청은 도메인 재계산에서 빨리 거절하지 말고 receipt 조회에 도달시켜야 한다. 기록이 없으면
RevisionConflict이며 새 스냅샷을 저장하지 않는다. 명령이 다르면 RequestConflict다.
연결 오류는 커밋 여부가 불명확할 수 있으므로 조회 또는 동일 원본 요청으로 확인한다.
영속 성공 이력은 자동 삭제하지 않는다. 향후 보존 정책을 도입할 때 재전송 허용 기간도 함께 정해야 한다.

성장·스킬·아이템은 서버가 검증한 전체 스냅샷을 한 CALL에서 교체한다. 아이템 행은 같은
instanceId로 재삽입하므로 꽉 찬 가방의 장비 교체도 위치 UK와 충돌하지 않는다.
모든 결과를 읽고 ValidateResults가 성공한 뒤 기존 ODBC 실행기가 COMMIT한다.
프로시저에는 별도 COMMIT·전체 ROLLBACK·DDL이 없다. 멀티스레드 메모리 변경은 Town strand에서
DB 성공 후 수행하고 현재 세션·캐릭터를 재검증한다.

**배포·권한·역변환**

서비스 정지 → 정식 Up4→5 → EXECUTE 부여 → 동일 SQL 11개/새 Auth·Town 배포 → head5 검증
순서다. 새 Auth 검사 프로시저는 get_inventory_schema_migration_history이며 history_format=3,
18결과셋, 11루틴, 29입력 파라미터다. 기존 V0/V4 inspector와 권한은 보존한다.
기존 head4는 새 Auth에서 준비 실패한다.

Auth에는 새 inspector EXECUTE, Town에는 공개 5프로시저와 inspector EXECUTE가 필요하다. 로컬에서 같은
runtime 계정을 쓰면 합집합이다. 내부 emit_character_state에는 런타임 EXECUTE를 부여하지 않는다.
테이블 직접 접근·DDL·감사 쓰기는 런타임 권한에 추가하지 않는다. 모든 루틴의 원본을 조회할 수 있는
동일한 지속 migration DEFINER를 유지한다.

V5 Down은 세 신규 테이블이 모두 비어 있을 때만 허용한다. WRITE LOCKS 아래 검사하고
세 테이블을 공동 DROP한 직후 UNLOCK한 다음 루틴을 삭제한다. V4 캐릭터/스킬과 모든 이력은 유지한다.
한 번이라도 생성/선택/저장해 보조 상태나 성공 이력이 생겼다면 일반 Down을 거절한다.
DDL 부분 실패에는 자동 재실행/감사 삭제/체크섬 변경을 하지 않는다. 별도 상태 확인이 필요하다.


## 9. 수동 Up/Down 실행 계약

### 로컬 최초 설정의 DB 준비 계약

RunLocalTest 최초 설정의 입력·보호 저장·환경 구성은 서버 담당 구현 범위다. 아래는 승인된
DB 연계 계약이며, 이 문서 작업에서 설정 도구를 구현하거나 실제 접속을 검증했다는 뜻은 아니다.
Google Desktop 클라이언트 ID와 Auth/Town HTTPS 설정은 서버·클라이언트 문서를 따른다.

| 입력/공급 | 최소 계약 |
|---|---|
| ODBC 드라이버 | 설치된 64비트 MySQL Connector/ODBC Unicode 드라이버의 정확한 등록명 선택; 이름·버전을 추측하지 않음 |
| DB 대상 | 기존 MySQL 호스트·포트·이미 준비된 스키마 이름; 포트 3306은 후보이며 실제 대상 확인 필요 |
| DB 계정·비밀번호 | runtime 실행 주체를 로컬에서 입력; 비밀번호 입력을 화면·로그에 표시하지 않음 |
| DB 접속 보안 | 실제 대상에 맞는 TLS 모드·CA 등; Auth HTTPS 인증서와 별도 설정 |
| ACTIONRPG_DB_CONNECTION_STRING | 입력을 OdbcConnectionStringBuilder로 구성·형식 검사 후 프로세스 환경으로 공급 |
| ACTIONRPG_DB_SCHEMA | 동일 입력 스키마; 연결에서 선택한 DATABASE()와 정확히 대조 |
| ACTIONRPG_DB_MIGRATIONS_DIRECTORY | 저장소의 ActionRPGServer/Database/Migrations/MySQL 절대 경로 재사용; Infrastructure 1개·Up 4개·Down 4개 모두 확인 |

로컬 설정은 저장소 밖 사용자 전용 경로에 두고 연결 비밀은 DPAPI CurrentUser로 보호한다.
다음 실행에서 복호화한 값은 필요한 자식 프로세스 환경으로 전달하며 배치·명령 인자·Git·로그에
쓰지 않는다. 설정 저장 성공은 DB 접속이나 스키마 준비 성공을 의미하지 않는다. 기존 보호
설정이 읽히지 않으면 로컬에서 다시 입력하며 비밀 원문이나 예외 메시지를 진단에 출력하지 않는다.
runtime 설정을 마이그레이션용 환경 변수로 자동 복사하거나 생성 권한을 요구하지 않는다.

접속 전에 64비트 실행 환경, 선택한 드라이버의 64비트 등록과 DLL 존재, 배포 SQL 파일을 검사한다.
드라이버 목록 조회 실패는 미확인으로, 정확한 등록명 부재는 드라이버 미준비로 구분한다.
MySQL Server/Workbench 설치나 32비트 드라이버만으로 준비됐다고 판단하지 않는다.
드라이버가 없으면 서버 기동을 중단하고 [공식 Connector/ODBC 배포 페이지](https://dev.mysql.com/downloads/connector/odbc/)와
[Windows 설치 안내](https://dev.mysql.com/doc/connector-odbc/en/connector-odbc-installation-binary-windows.html)를
제공한다. 호환되는 Windows 64비트 패키지와 그 패키지의 필수 런타임을 준비하며 도구가 자동으로
다운로드·설치하지 않는다. 드라이버 버전과 MySQL 서버 버전은 별개이고 드라이버 등록·DLL 존재만으로
연결·트랜잭션·다중 결과셋·타임아웃 호환성을 검증한 것은 아니다.

2026-10-07 읽기 조사에서 로컬 mysqld.exe 파일 버전은 8.0.46.0으로 관측했고, 이후 사용자가
로컬 서버에서 실행한 `SELECT VERSION()` 결과를 8.0.46으로 전달했다. 에이전트가 직접 DB에
접속한 것은 아니다. 실제 접속이 승인되고 입력이 준비된 실행 단계에서 DATABASE()·@@version·
@@version_comment 및 기존 이력/구조 검증으로 대상과 현재 상태를 확인한다.
접속·조회 실패 시 버전/head는 미확인으로 남긴다. 현재 MySQL 8.0.46과
head=5 검증 계약은 유지하며 버전 문자열을 설정값으로 대신하거나 허용 범위를 자동 확장하지 않는다.

진단은 설정/파일 미준비, ODBC 미준비, 접속/DB 검증 실패를 구분하되 드라이버 원문 예외·연결
문자열·비밀 값은 출력하지 않는다. Auth의 현재 HTTP 503과 일반 검증 실패만으로 서버 버전
불일치·권한 부족·미적용을 특정하지 않는다. 추가 대상 조회는 별도로 승인된 실행 범위에서만 한다.
이력 없음·낮은 head·FAILED/RUNNING·구조/체크섬 불일치에는 자동 생성·Up·repair를 수행하지
않는다. 별도로 승인된 수동 Up/Down 절차로 준비한 뒤 Auth가 동일 배포 SQL과 실제 head=5를
검증해야 로그인·입장을 활성화한다. 로컬 최초 설정이 이 검증을 우회하지 않는다.

### 파일과 실행

정식 도구는 `Tool/Database/UpMigration.bat`, `DownMigration.bat`이며 공통 실행기는
`Migrate.ps1`이다. 64비트 Windows PowerShell 5.1과 동일 비트의 MySQL ODBC 드라이버를 사용한다.
별도 마이그레이션 제품 설치 없이 ODBC로 실행한다. 서비스용 접속 문자열을 자동으로 재사용하지
않으며 전용 환경 변수 `ACTIONRPG_MIGRATION_CONNECTION_STRING`을 사용한다. 암호가 포함된
값을 명령 인자·배치 파일·Git에 넣거나 로그로 출력하지 않는다. DB 접속의 TLS/인증 설정은 실제
환경에 맞게 별도로 준비한다. 로컬 PowerShell 실행 정책은 존중하며 도구가 자동 우회하지 않는다.

모든 DB 사용 서비스를 정지한 뒤 실행한다. 다음 `actionrpg`는 실제 DB 이름으로 바꾸는 예시다.
기본 모드에서 `-Database`는 접속 문자열이 선택한 스키마와 정확히 일치해야 한다.
Up의 `-CreateDatabase` 옵션은 대상 DB 생성·선택부터 수행하며 아래 별도 조건을 따른다.
`-ServicesStopped`는 운영자가 정지를 확인했다는 표시이며 프로세스를 자동 종료하거나 정지를
검증하는 기능이 아니다. 계정 생성·권한 부여는 자동 수행하지 않는다.

명령 예시는 저장소 루트에서 실행한다. 다른 디렉터리에서는 배치의 전체 경로를 사용한다.
배치는 자기 위치의 Migrate.ps1을 호출하고 실행기는 자기 위치를 기준으로 SQL을 찾으므로,
배포 시 `Tool/Database`와 `ActionRPGServer/Database/Migrations/MySQL`의 상대 배치를 보존한다.

| 인자/설정 | 구현된 의미 |
|---|---|
| UpMigration.bat / DownMigration.bat | 공통 실행기에 각각 -Direction Up/Down을 전달 |
| -Database 이름 | 필수, 1~64자; 기본 모드는 연결에서 선택된 DATABASE()와 정확히 대조 |
| -CreateDatabase | Up 전용; DB가 없으면 생성하고 선택한 뒤 기존 이력·구조 검사와 적용 수행 |
| -InspectOnly -InspectVersion 0..5 | Up 배치의 읽기 전용 진단; 지정 구조와 비교하며 실제 head/복구 성공을 확정하지 않음 |
| -RecoverBootstrap | Up 전용; 완전한 V0와 초기 실패 1건을 재검증해 원본 보존 및 복구 확인 행 추가, V0에서 종료 |
| -RecoverAccounts | Up 전용; 정상 V0 이력과 완전한 빈 V1의 첫 실패를 재검증해 복구 확인 행 추가, V1에서 종료 |
| -ServicesStopped | 필수 정지 확인; 빠지면 DB 접속 전에 중단 |
| ACTIONRPG_MIGRATION_CONNECTION_STRING | 실행기 전용 ODBC 연결; 값 없으면 접속하지 않음 |
| ACTIONRPG_DB_CONNECTION_STRING | Auth runtime 전용 ODBC 연결; 마이그레이션 도구가 대신 사용하지 않음 |
| ACTIONRPG_DB_SCHEMA | Auth가 기대하는 DB 이름; runtime 연결의 DATABASE()와 대조 |
| ACTIONRPG_DB_MIGRATIONS_DIRECTORY | Auth에 배포한 MySQL SQL 디렉터리의 절대 경로; Up·Down·Infrastructure 모두 필요 |

접속 환경 값은 보호된 로컬/배포 환경에서 제공하고 문서·저장소에 실제 연결 문자열을 남기지 않는다.
Auth의 Google/TLS/타운 설정은 Auth 개발 계약을 따른다. 실행기 연결은 autocommit ON, strict 모드,
NO_BACKSLASH_ESCAPES·ANSI_QUOTES·PIPES_AS_CONCAT 부재를 요구한다. runtime CALL의
autocommit OFF 조건과 다르므로 마이그레이션 SQL을 runtime 프로시저 객체로 실행하지 않는다.

```bat
Tool\Database\UpMigration.bat -Database actionrpg -ServicesStopped
Tool\Database\UpMigration.bat -Database actionrpg -CreateDatabase -ServicesStopped
Tool\Database\DownMigration.bat -Database actionrpg -ServicesStopped
```

### 명시적 DB 생성: Up -CreateDatabase

* 소문자 영문자로 시작하는 1~64자의 소문자 영문·숫자·밑줄 이름만 받는다.
  mysql, information_schema, performance_schema, sys는 거부하며 Down과 함께 쓸 수 없다.
* 같은 전용 `ACTIONRPG_MIGRATION_CONNECTION_STRING`을 사용하되 DSN/FILEDSN/SAVEFILE은
  허용하지 않고 DRIVER 기반 연결을 요구한다. DATABASE는 생략하거나 `-Database`와 정확히
  일치해야 한다. DATABASE를 제거한 연결로 접속하므로 아직 없는 DB 때문에 접속이 실패하지 않는다.
* SQL 파일·입력 검증 후 MySQL 버전·SQL 모드·autocommit과 기본 DB 미선택을 확인한다.
  지정한 이름으로 기존 `actionrpg:migrate:<hash>` 잠금을 먼저 획득하며, DB 생성·선택·이력
  검증·전체 마이그레이션 동안 같은 ODBC 연결을 유지한다. 잠금 획득 실패 시 DB를 생성하지 않는다.
* DB가 없을 때만 CREATE DATABASE로 utf8mb4 / utf8mb4_0900_ai_ci를 지정한다.
  기존 DB는 삭제·재생성·ALTER하지 않는다. USE 후 DATABASE()와 잠금 이름을 재확인하고
  빈 스키마 또는 정상 관리 DB에 대한 기존 적용 흐름을 따른다. 비어 있지 않은 무관리 DB는 거부한다.
* DB 생성은 그 DB의 버전 이력을 만들기 전 준비 단계로, 버전 마이그레이션 성공으로 기록하지 않는다.
  CREATE DATABASE의 암묵적 커밋 때문에 후속 실패에도 DB는 남으며 자동 DROP하지 않는다.
  DB 생성만 완료되고 비어 있다면 다음 Up은 기존 빈 스키마 절차를 따르지만, 일부 SQL이 적용됐다면
  이력·실제 구조를 조사하고 별도 복구 승인을 받는다.
* 실행 주체에는 해당 DB의 CREATE 권한과 기존 마이그레이션·DEFINER 권한이 필요하다.
  계정 생성·GRANT는 별도이며 Auth runtime 주체에 생성 권한을 부여하지 않는다.

### 적용과 역변환

* Up: 실제 성공 이력을 재생해 현재 상태를 계산한 뒤 모든 미적용 버전을 오름차순 적용한다.
  현재 배포 계약은 000001~000005이며 최신 상태에서는 새 실행 이력을 만들지 않는다.
* Down: 현재 적용된 가장 높은 버전 하나만 역변환하고 DOWN 성공 이력을 추가한다.
  과거 UP 기록은 삭제하지 않는다. 이후 Up은 되돌린 버전부터 다시 적용한다.
* 000000: 이력 관리 기반이다. 빈 스키마에 최초 Up할 때만 생성하며 Down 대상에서 제외한다.
  테이블만 남은 중단된 초기화나 기존 비어 있지 않은 DB를 성공 상태로 추정해 등록하지 않는다.
* 파일은 Up `VNNNNNN__name.sql`, Down `Down/동일파일명.sql` 한 쌍이다. 이미 적용된 Up/Down과
  Infrastructure 파일은 모두 고정한다. 새 버전에는 변경에 맞는 실행기·Auth 실제 구조 검증 계약도
  함께 갱신해야 한다. 현재 코드에 파일만 추가해 미지원 스키마를 승인하지 않는다.

| Down 버전 | 역변환 | 데이터 조건 |
|---|---|---|
| [000004 Down](../../ActionRPGServer/Database/Migrations/MySQL/Down/V000004__create_characters_and_skills.sql) | characters/character_skills·확장 검사 삭제 | 두 테이블이 비어 있을 때만 허용; 계정·기존 검사 유지 |
| [000005 Down](../../ActionRPGServer/Database/Migrations/MySQL/Down/V000005__create_character_inventory_persistence.sql) | 상태·아이템·성공 요청/신규 루틴 삭제 | 세 신규 테이블이 모두 비어 있을 때만 공동 DROP; V4 캐릭터/스킬 유지 |
| [000003 Down](../../ActionRPGServer/Database/Migrations/MySQL/Down/V000003__create_auth_account_status_procedure.sql) | `get_auth_account_status` 삭제 | 계정 데이터 유지; 최신 Auth 코드와 호환되지 않음 |
| [000002 Down](../../ActionRPGServer/Database/Migrations/MySQL/Down/V000002__create_google_login_procedure.sql) | `login_google_account` 삭제 | 계정 데이터 유지 |
| [000001 Down](../../ActionRPGServer/Database/Migrations/MySQL/Down/V000001__create_login_accounts.sql) | `account_identities`, `accounts` 삭제 | 두 테이블이 모두 비어 있을 때만 허용 |

000001 Down은 두 계정 테이블과 이력 테이블에 WRITE 잠금을 잡고 빈 상태를 확인한 뒤 두 계정
테이블을 한 DROP 문장으로 삭제한다. 빈 상태 검사를 통과하지 못하면 DDL과 실행 이력 추가를
하지 않는다. 잠금 대기와 연결 오류는 실패로 처리한다. 런타임 계정이 있는 DB의 계정 테이블 삭제나
데이터 복원은 이 도구의 범위가 아니다. [MySQL 테이블 잠금](https://dev.mysql.com/doc/refman/8.0/en/lock-tables.html).

### 잠금·체크섬·감사 이력

한 ODBC 연결이 전체 실행 동안 이름 기반 DB 잠금을 보유한다. `-CreateDatabase`는 기본 DB를
선택하기 전 지정한 이름으로 동일 잠금을 얻고 DB 생성부터 보호하며, 선택 후 이름 일치를 재확인한다. 이름은
`CONCAT('actionrpg:migrate:', LEFT(SHA2(DATABASE(), 256), 40))`이다. `GET_LOCK(name, 0)`으로
즉시 획득하지 못하면 중단하며, 각 실행 전에 `IS_USED_LOCK(name)=CONNECTION_ID()`를 확인한다.
연결을 재생성하거나 실패한 SQL을 자동 재시도하지 않는다. 종료 시 잠금을 해제하고 연결을 닫는다.
잠금은 같은 MySQL 인스턴스 안의 실행자를 직렬화한다. 다중 쓰기 인스턴스 토폴로지는 지원하지
않는다. [MySQL 이름 기반 잠금](https://dev.mysql.com/doc/refman/8.0/en/locking-functions.html).

체크섬 형식은 `sha256-utf8-lf-v1`이다. 엄격한 UTF-8로 읽고 최초 UTF-8 BOM 하나만 제외하며
CRLF와 나머지 CR을 LF로 바꾼다. 공백·주석·마지막 줄바꿈은 그대로 보존한다. BOM 없는 UTF-8
바이트의 SHA-256을 소문자 64자리로 기록한다. 원시 파일 SHA-256과 혼동하지 않는다.
실행 전 모든 Up/Down 파일을 읽고 문장 분리까지 완료해 같은 메모리 내용으로 실행한다.
DELIMITER `;`/`$$`, 문자열·식별자·일반 주석을 처리하며 실행형 `/*!...*/` 주석은 거절한다.

`Infrastructure/V000000__migration_history.sql`은 이력 테이블과 조회 프로시저를 만든다.
`schema_migrations` 컬럼 순서는 execution_id, version, name, direction, up_checksum,
down_checksum, state, started_at, finished_at이다. 이름은 파일의 `__` 뒤 확장자를 제외한 부분이며
000000만 `migration_history`다. 000000의 down_checksum은 NULL, 나머지는 항상 Up/Down 두
체크섬을 기록한다. 실행 전 RUNNING, DDL·실제 구조 검사 후 SUCCEEDED로 UTC 시각을 기록하고,
오류는 연결과 잠금이 유효하면 FAILED로 남긴다. 종료/통신 실패로 RUNNING이 남을 수도 있다.

schema_migrations도 InnoDB이며 유일한 인덱스는 execution_id 기본 키다. version에는 UNIQUE가
없으므로 성공 Up→Down→다시 Up 같은 시도가 모두 남는다. 문자열은 ascii/ascii_bin이다.

| 컬럼 | 실제 타입·NULL | 내용 |
|---|---|---|
| execution_id | BIGINT UNSIGNED, NOT NULL, AUTO_INCREMENT | 감사 실행 순서; 번호 간격은 허용 |
| version | INT UNSIGNED, NOT NULL | 0 기반 또는 해당 논리 버전 |
| name | VARCHAR(128), NOT NULL | 파일 이름의 목적 부분 |
| direction | ENUM('UP','DOWN'), NOT NULL | 이번 시도의 방향 |
| up_checksum | CHAR(64), NOT NULL | 정규화한 Up SQL SHA-256 |
| down_checksum | CHAR(64), NULL 허용 | Down SQL SHA-256; 기반 0에서만 NULL |
| state | ENUM('RUNNING','SUCCEEDED','FAILED'), NOT NULL | 시도의 현재 상태 |
| started_at | DATETIME(6), NOT NULL | UTC 시작 시각 |
| finished_at | DATETIME(6), NULL 허용 | UTC 완료 시각; RUNNING에서 NULL |

`ck_migrations_completion`은 RUNNING/finished_at NULL과 완료 상태/NOT NULL 조합을 강제한다.
`ck_migrations_bootstrap`은 version=0/UP/down_checksum NULL 또는 version>0/down_checksum
NOT NULL 조합을 강제한다. 방향·선행 관계·성공 여부·양방향 체크섬·시각 순서는 실행기와 Auth가
전체 감사 이력을 읽어 추가 검증한다. 테이블 제약만으로 유효한 버전 흐름이 증명되지는 않는다.

현재 버전은 execution_id 증가 순서 전체 이력을 재생해 판단한다. 최초 행은 000000 UP 성공,
이후 UP은 현재+1, DOWN은 현재 버전이며 0은 되돌리지 않는다. 실행 ID 간격은 허용하되 역순,
중복·버전 누락·알 수 없는 버전·이름/체크섬 불일치·미복구 FAILED/RUNNING은 거절한다. 초기
V0 FAILED 및 정상 V0 직후 첫 V1 UP FAILED 각각 바로 다음의 명시적 복구 확인 1건만
아래 계약으로 인정한다. 실패를 걸러낸
MAX(version) 조회나 이력 삭제로 현재 버전을 계산하지 않는다. 완료 시각은 시작 시각 이후여야 한다.

MySQL DDL과 이력 쓰기를 한 트랜잭션으로 롤백하지 않는다. 초기 이력 테이블 생성 직후 감사 행
쓰기 이전에도 중단될 수 있다. SQL 실패·이력 쓰기 실패·구조 검사 실패에는 다음 버전을 적용하지
않는다. 실제 구조와 마지막 감사 상태를 확인해 별도로 승인받은 복구를 한다. 일반 Down은 성공
적용된 버전의 역변환이며 부분 실패 복구 명령이 아니다. [MySQL 암묵적 커밋](https://dev.mysql.com/doc/refman/8.0/en/implicit-commit.html).

### 새 DB 초기 구축과 기존 DB 갱신

1. 실제 대상 MySQL 8.0.46의 호스트·포트·스키마, 적용할 승인 SQL 배포본, 데이터 유지 조건을
   정한다. 스키마는 별도로 준비하거나 Up -CreateDatabase로 생성한다. 전용 생성 주체·runtime
   주체·TLS/ODBC 드라이버·권한은 별도로 준비하며 실행기는 사용자를 만들거나 GRANT하지 않는다.
2. Auth·Town·Room의 기존 접속을 종료하고 정지 상태를 확인한다. Auth 메모리 소유권이 사라지는
   재시작 전에 타운/룸의 이전 연결까지 종료하는 조건은 Auth 개발 계약을 따른다.
3. 빈 신규 스키마에는 이력 테이블·기존 테이블/뷰·프로시저/함수·이벤트·트리거가 없어야 한다.
   Up은 V000000 이력 기반을 생성한 후 000001→000002→000003→000004→000005를 순서대로 적용한다.
4. 기존 관리 DB는 아래 조회 절차로 전체 이력과 구조를 먼저 확인한다. 정상 head=1이면 2→3→4,
   head=2이면 3→4→5, head=3이면 4→5, head=4이면 5, head=5이면 새 감사 행 없이 검사만 끝낸다. Down 성공으로 낮아진 head에도
   같은 규칙을 사용한다. 이력이 없는 비어 있지 않은 DB를 자동으로 기준 버전 등록하지 않는다.
5. 보호된 환경의 전용 접속 문자열과 실제 스키마 이름으로 Up 배치를 실행한다. 대상 host:port,
   DB 이름, 계획, 각 버전의 성공, 마지막 `Complete. Active version: V000005.`을 확인한다.
   표시한 계획 뒤에 바로 SQL을 실행하며 추가 확인 프롬프트나 dry-run 단계는 없다.
6. 종료 코드 0과 최종 상태를 확인하고 동일 Up/Down/Infrastructure 파일을 Auth 배포 디렉터리에
   둔다. 종료 코드 1·중간 중단·불명확한 완료에서는 후속 서비스 배포를 진행하지 않는다.
7. Auth를 기동해 실제 계정 DB 검증과 로그인/입장 준비 상태를 확인한 후 타운·룸을 연결한다.
   Auth는 마이그레이션을 실행하지 않는다. 실제 환경에서 로그인·상태 조회·연계 동작 확인은
   별도로 승인받은 검증 범위에서 진행한다.

현재 Up의 목표는 준비된 계약의 최신 000005로 고정된다. `-TargetVersion`, 특정 버전만 실행,
기준 버전 등록, `-Status`/`-WhatIf`/별도 조회 배치는 구현하지 않았다. `-InspectOnly`는 지정한 구조와
비교하는 진단이며 실제 적용 버전이나 복구 성공 판정이 아니다. DB 생성은 위의 명시적
Up -CreateDatabase로만 수행한다. 임의 버전의 SQL을 골라 관리 도구에서 실행하는 방식으로
순서·감사 기록을 우회하지 않는다. 새 버전을 추가할
때는 실행기의 4개 버전/구조 검사와 Auth의 5개 기반·버전 파일/요구 head 계약을 함께 변경해야 한다.

### 현재 버전의 조회와 확인

이 문서에 기재한 000005는 배포 파일 버전이다. 실제 버전을 확인하려면 승인된 관리 환경에서
선택 DB와 V5의 `get_inventory_schema_migration_history()` 결과를 조회해야 한다.
V0~V3은 기존 `get_schema_migration_history()`, V4는 `get_character_schema_migration_history()`를 사용하며 V5 전체 구조 검사의 대체는 아니다. 아래 SQL은 조회 예시이며
문서 작성 중 실행하지 않았다. 호출은 검사 잠금을 잠시 획득하지만 감사 행이나 스키마를 바꾸지 않는다.

```sql
SELECT DATABASE(), @@version, @@version_comment;
CALL get_inventory_schema_migration_history();
```

1. 첫 SELECT의 대상이 의도한 스키마·엔진인지 확인한다. CALL의 첫 결과셋(아래 계약의 번호 0)에서도
   같은 DB·버전과 history_format=2(V0~V3은 1), checksum_format=sha256-utf8-lf-v1을 확인한다.
2. 두 번째 결과셋(번호 1)의 전체 감사 행을 execution_id 순서로 확인한다. 최초 000000 UP 성공
   또는 아래 초기 V0 실패/복구 조합 이후 UP은 head+1, DOWN은 현재 head를 되돌린다.
   NULL 완료 시각이나 미복구 FAILED/RUNNING을
   숨겨 계산하지 않는다. 예: 0UP→1UP→2UP→3UP→3DOWN의 현재 head는 2이며 MAX(version)은 3이다.
3. 이름·양방향 체크섬·시각·선행 관계를 배포 파일과 대조한다. 이어지는 실제 컬럼·제약·인덱스·루틴
   입력·SHOW 원문도 해당 head의 계약과 비교한다. 표/프로시저가 존재하는 것만으로 완료를 판단하지 않는다.
4. 실행기의 Get-Head/Get-Snapshot/Assert-Structure와 Auth의 ValidateHistory/ValidateStructure가
   위 검증을 구현한다. 수동 CALL이나 눈으로 읽은 최대 번호는 이 검증의 대체가 아니다.
   Auth 준비를 위해서는 현재 DB에서 검증에 성공한 head=5 증명이 필요하다.

조회 불가·원문 NULL·잠금 획득 실패라면 현재 버전 확인에 실패한 것이다. 0 또는 3으로 가정하지
않는다. Up을 단순 조회용으로 실행하면 미적용 SQL을 적용할 수 있으므로 상태 확인에 사용하지 않는다.

### Down과 실패 후 대응

실패 원인의 구조 비교만 필요하면 다음 진단 경로를 사용한다. 보호된 전용 연결 환경과
`-ServicesStopped` 확인은 일반 실행과 같고, 기존 이력 테이블·검사 프로시저가 필요하다.

```bat
Tool\Database\UpMigration.bat -Database actionrpg -InspectOnly -InspectVersion 0 -ServicesStopped
```

두 진단 인자는 반드시 함께 주고 `-CreateDatabase`와 함께 사용할 수 없다. `InspectVersion`은
운영자가 선택한 비교 구조(0~4)이지 실제 적용 head를 추측한 값이 아니다. 진단은 같은 연결로
잠금을 잡고 기존 검사 프로시저를 호출해 감사 행과 CHECK 원문·정규화 실제 값·기대값을 출력한다.
실패/RUNNING 이력을 거절하는 일반 적용 검사 전에 분기하며, DB 생성·마이그레이션 SQL·감사 행
추가/수정/삭제 경로로 들어가지 않는다. 정상 종료도 선택한 구조 비교만 통과했다는 뜻이며
감사 이력의 유효성, 실제 head, 복구 완료나 Auth 준비 완료를 보장하지 않는다. 기존 이력이 없거나
검사 프로시저가 없으면 생성하지 않고 중단한다. 연결 정보·원문 ODBC 예외는 출력하지 않는다.

### 초기 V000000 전용 명시적 복구

초기 V0의 DDL은 모두 완료됐으나 구조 검사가 실패한 경우에 한해 다음 명령을 사용한다.
진단 결과를 확인하고 복구를 승인한 운영자가 같은 전용 연결·서비스 정지 조건으로 실행한다.

```bat
Tool\Database\UpMigration.bat -Database actionrpg -RecoverBootstrap -ServicesStopped
```

* Up 전용이며 `-CreateDatabase`, `-InspectOnly`와 함께 사용할 수 없다. DB·이력·검사 프로시저가
  이미 있어야 한다. 전용 연결/이름 기반 잠금을 유지하고 V0의 배포 체크섬·컬럼·제약·인덱스·루틴
  원문/생성 모드·입력 계약을 다시 검증한다. 객체 수는 이력 테이블 1개·검사 루틴 1개·이벤트/트리거
  0개여야 한다. 객체 조회는 실제 적용 주체의 메타데이터 가시성에 따르며, 모든 DB 변경 주체는
  기존 서비스 정지/동일 잠금 계약을 준수해야 한다.
* 감사 행은 정확히 1건이며 version=0, name=migration_history, direction=UP, 현재 V0 체크섬,
  down_checksum=NULL, state=FAILED와 유효한 완료 시각이어야 한다. RUNNING·다른 시도·부분
  구조·체크섬 불일치에는 쓰기 전에 중단한다.
* 재검증 후 하나의 INSERT로 version=0, name=migration_history_recovery, direction=UP,
  같은 up_checksum, down_checksum=NULL, state=SUCCEEDED와 현재 UTC 시작/완료 시각을
  기록한다. INSERT 자체에서도 실패 행/체크섬/다른 감사 행 부재와 현재 시각이 실패 완료 시각
  이후임을 확인한다. 기존 FAILED 행은 수정·삭제하지 않으며 버전 SQL/DDL을 재실행하지 않는다.
* 도구와 Auth는 첫 V0 FAILED 바로 다음의 위 확인 행만 복구로 인정한다. 두 행의 체크섬은 배포
  V0과 같고 실행 ID는 증가하며 확인 시작 시각은 실패 완료 시각 이후여야 한다. 다른 위치의 실패,
  중복/단독 확인 행, RUNNING, 누락·잘못된 순서는 계속 차단한다. 확인 행은 구조 재검증의 완료
  기록으로 V0 적용 SQL을 다시 실행했다는 뜻이 아니다.
* 쓰기 후 원래 실패 행 불변·전체 이력·V0 구조/객체 수를 재확인하고
  `Recovery complete. Active version: V000000.`에서 종료한다. 000001~000005는 별도 일반 Up으로
  적용한다. 복구 재실행은 다른 감사 행이 있으므로 추가 기록 없이 거절한다. INSERT 후 통신/검사
  실패에는 확인 행이 이미 커밋됐을 수 있으므로 자동 재시도하지 말고 읽기 전용 진단으로 조사한다.
* 수정된 Auth 소스를 다시 빌드해야 복구 이력을 인정한다. 구버전 Auth는 FAILED를 계속 거절한다.
  새 Auth도 실제 head=5 및 전체 구조/배포 체크섬 검증 전에는 로그인·입장을 열지 않는다.

자기 테이블을 SELECT 원본으로 사용하는 INSERT는
[MySQL INSERT ... SELECT 계약](https://dev.mysql.com/doc/refman/8.0/en/insert-select.html)을 따른다.

### 초기 V000001 전용 명시적 복구

초기 V1의 두 테이블은 생성됐으나 검사가 실패했고 계정 데이터가 없다면 다음 별도 복구를 사용한다.

```bat
Tool\Database\UpMigration.bat -Database actionrpg -RecoverAccounts -ServicesStopped
```

`-RecoverAccounts`는 Up 전용이며 다른 복구/생성/진단 옵션과 함께 쓸 수 없다. 같은 전용 연결·
서비스 정지·잠금 조건에서 V0-only 이력(정상 최초 성공 또는 V0 실패/복구 확인 조합)과 마지막
첫 V1 UP FAILED를 확인한다. 감사 행은 각각 2개 또는 3개뿐이어야 하고 V1의 이름·양방향 체크섬·
실행 ID 증가·유효한 시작/완료 시각을 검증한다. V1 전체 구조와 테이블 3개/검사 루틴 1개/
이벤트·트리거 0개, 계정/식별자 테이블의 비어 있음을 재검증한다. 부분 생성·계정 데이터·추가 시도·
다른 실패에는 쓰지 않는다. 객체 조회 가시성과 변경 주체의 동일 잠금 계약은 V0 복구와 같다.

한 INSERT로 V1/UP/SUCCEEDED `create_login_accounts_recovery` 확인 행을 기록하고 기존
모든 행이 불변인지 다시 검사한다. 양방향 체크섬은 실패 행과 배포 파일 모두에 결합되며,
추가된 더 나중의 감사 행 부재와 계정 데이터 부재, 현재 시각이 실패 완료 이후임을 INSERT에서도
확인한다. 원래 V1 FAILED 행을 수정·삭제하지 않고 DDL도 재실행하지 않는다. 성공은
`Recovery complete. Active version: V000001.`이며 별도의 일반 Up으로 2~4를 적용한다.
확인 INSERT 후 오류에는 이미 기록됐을 수 있으므로 자동 재시도하지 않고 실제 이력을 조사한다.

도구와 수정된 Auth는 초기 V1 실패 바로 다음의 위 확인 행만 해소된 실패로 인정한다. V1 이전에
다른 애플리케이션 버전 실행이 있거나 복구 행이 단독/중복/잘못된 위치·시각·상태에 있으면 거절한다.
정상 Up/Down 이후 V1 재실패에는 이 예외를 적용하지 않는다. 기존 V0 복구와 함께 존재할 수 있다.

### 일반 Down과 미지원 실패 복구

Down도 관련 서비스를 정지하고 같은 대상·파일·이력·구조 검사를 거친 뒤 현재 head 한 단계만
되돌린다. 반복 Down은 4→3→2→1→0 순서이며 head=0에서는 기반을 지우거나 새 감사 행을 쓰지 않는다.
4 Down은 두 캐릭터 테이블이 비어 있을 때만 함께 삭제하고 확장 검사를 삭제한다.
3/2 Down은 루틴 삭제 후 계정 데이터를 유지한다. 1 Down은 두 테이블에 데이터가 없어야 하며
테이블 구조를 삭제한다. 전체 데이터 삭제를 허용하는 force 옵션은 없다. Git checkout이나 이전
서버 바이너리 배포는 DB 데이터를 복구하지 않는다. 데이터 복원 도구·백업 생성도 제공하지 않는다.

실패 시에는 출력의 대상·version/statement 단계와 실제 감사 상태·실제 생성된 객체를 확인한다.
연결 문자열·원문 DB 예외를 공개 로그로 옮기지 않는다. 연결이 끊겨 결과가 불명확하거나 기반 테이블만
생성된 경우도 자동 재시작·Down·이력 삭제를 하지 않는다. 000001의 두 CREATE는 따로 커밋되므로
첫 테이블만 생성된 중단 상태가 가능하다. 도구는 정상 성공 기록이 생길 때까지 대기 SQL을 건너뛰지 않는다.

체크섬 불일치는 승인된 배포 원문을 대조하고 코드/SQL 배포 실수를 확인한다. 이력의 체크섬을 현재
파일에 맞춰 덮어쓰지 않는다. 위 초기 V0/V1 복구 조합을 제외한 FAILED/RUNNING은 일반 Up/Down을
계속 차단한다. 자동 repair/resume이나 다른 버전의 복구 기능은 없으며, 원인·부분 적용 상태·데이터
유지 조건을 근거로 별도의 복구 SQL과 이력 처리 계약을 승인받아야 한다. 이 소스 변경만으로
실제 DB 복구/SQL 적용을 수행한 것은 아니다.

실행기는 ODBC 명령에 30초 타임아웃을 요청하고 조회 전체 4096행·값 32768문자, SQL 파일
1MiB 한도를 사용한다. 실제 드라이버의 타임아웃 동작은 운영 환경에서 확인해야 한다.
Auth runtime ODBC는 기본 2연결/대기 128건, 연결 5초·쿼리 10초·큐 대기 30초, 결과 전체
4096행/16개 데이터 결과셋/4MiB 한도를 사용한다. Auth HTTP의 15초 대기가 먼저 끝나도 이미
시작된 DB 실행을 취소했다는 뜻은 아니다. 누적 감사 이력/메타데이터가 한도를 넘으면 검증이 차단되며
이력을 임의로 잘라 해결하지 않는다. 조회 페이지화·승인된 감사 보존/한도 확장은 별도 구현 대상이다.

### AuthServer 조회 계약

V0의 `get_schema_migration_history()`, V4의 `get_character_schema_migration_history()`, V5의 `get_inventory_schema_migration_history()`는
같은 이름 기반 잠금을 획득해 조회하고 해제한다. 실행기는 프로시저 존재 여부로 검사 경로를
선택한 뒤 head별 형식·구조·원문을 대조한다. 새 Auth는 V5 검사만 사용한다. 잠금 획득
실패 시 SQL 오류이며 성공 플래그를 반환하지 않는다. 실행기 연결이 이미 잠금을 가진 경우 MySQL의
재귀 잠금 횟수 중 조회가 추가한 횟수만 해제한다. V0 데이터 결과셋은 10개, V4는 11개, V5는 18개이며 첫 7개는
문자열로 캐스팅한다. 나머지는 원본 SHOW CREATE 결과다. 각 결과셋은 SQL 파일의 순서를 따른다.

| 결과셋 | 컬럼 순서 |
|---|---|
| 0 (5) | history_format=V0 `1`/V4 `2`/V5 `3`, checksum_format, database_name, engine_version, engine_comment |
| 1 (9) | execution_id, version, name, direction, up_checksum, down_checksum, state, started_at, finished_at |
| 2 (9) | table_name, column_name, column_type, is_nullable, collation_name, extra, engine, column_default, character_set_name |
| 3 (11) | table_name, constraint_name, constraint_type, column_name, referenced_table_name, referenced_column_name, check_clause, enforced, referenced_table_schema, update_rule, delete_rule |
| 4 (6) | table_name, index_name, non_unique, seq_in_index, column_name, sub_part |
| 5 (4) | routine_name, security_type, sql_data_access, routine_definition |
| 6 (7) | routine_name, ordinal_position, parameter_mode, parameter_name, dtd_identifier, character_set_name, collation_name |
| 7 (6) | get_schema_migration_history의 SHOW CREATE: Procedure, sql_mode, Create Procedure, character_set_client, collation_connection, Database Collation |
| 8 (6) | login_google_account의 동일 SHOW CREATE 컬럼 |
| 9 (6) | get_auth_account_status의 동일 SHOW CREATE 컬럼 |
| 10 (6), V4 이상 | get_character_schema_migration_history의 동일 SHOW CREATE 컬럼 |
| 11~17 (6), V5 | emit_character_state, list_characters, create_character, claim_character, save_character_state, release_character, get_inventory_schema_migration_history 순서의 SHOW CREATE |

NULL은 원본 NULL과 동일하며 임의로 빈 문자열과 합치지 않는다. 대상 테이블은 schema_migrations,
accounts, account_identities, 대상 루틴은 get_schema_migration_history, login_google_account,
get_auth_account_status다. V4는 characters/character_skills와 get_character_schema_migration_history도
포함한다. V5는 신규 세 테이블과 공개/내부/검사 7루틴을 추가해 총 11루틴·29입력 계약을 검사한다. 실제 기본값·InnoDB·CHECK 활성화·PK/인덱스·현재 스키마를 가리키는
FK RESTRICT·프로시저 본문/입력 문자셋을 대조한다. 정보 조회 권한 부족이나 본문 NULL도 거절한다.

로컬 MySQL 8.0.46의 사용자 진단 결과에서 CHECK 문자열 경계가 `_utf8mb4\'UP\'`처럼
반환되는 것을 확인했다. 실행기와 Auth는 실제 CHECK 메타데이터에만 알려진 문자셋 뒤의
영문·숫자·밑줄 문자열 경계 이스케이프를 해제한다. 문자열 대소문자와 `_binary` 의미는
보존하고 다른 미지원 역슬래시는 거절한다. 배포 SQL·프로시저 본문·체크섬이나 DB 제약조건은
변경하지 않는다. 이 표기 처리만으로 FAILED 이력이 복구되거나 적용 성공으로 바뀌지는 않는다.

CHECK의 `OCTET_LENGTH(subject)`는 로컬 메타데이터에서 `LENGTH(subject)`로 출력됐다.
둘은 [MySQL의 동일한 바이트 길이 함수](https://dev.mysql.com/doc/refman/8.0/en/string-functions.html#function_octet-length)로
실행기와 Auth의 CHECK 비교에서 함수 호출 토큰만 같은 표기로 정규화한다. 문자열 리터럴,
배포 원문·체크섬·프로시저 본문에는 이 별칭 변환을 적용하지 않는다.

ROUTINE_DEFINITION은 내부 definition_utf8에서 가져오며 `_binary` 같은 문자셋 introducer가
제거되므로 배포 원문과 직접 본문을 비교하지 않는다. 결과셋 5의 이름·보안·접근 특성을 확인하고,
실제 본문은 결과셋 7~17(V4는 7~10, V0는 7~9)의 Create Procedure에서 인용된 DEFINER와 헤더를 제외한 BEGIN~END를
사용한다. 문자열 내용과 `_binary`를 보존하고 일반 주석·공백·식별자 표기 차이를 토큰으로 정규화한다.
SHOW는 프로시저 안에서 결과셋을 반환할 수 있다.
근거: [MySQL SHOW CREATE](https://dev.mysql.com/doc/refman/8.0/en/show-create-procedure.html),
[프로시저 내 SHOW](https://dev.mysql.com/doc/refman/8.0/en/create-procedure.html),
[원본 definition을 사용하는 구현](https://github.com/mysql/mysql-server/blob/8.0/sql/sp.cc),
[definition_utf8 조회](https://github.com/mysql/mysql-server/blob/8.0/sql/dd/impl/system_views/routines.cc).

각 SHOW 결과는 정확히 6열·1행이다. Up/Down 중 실제 존재하지 않는 루틴만 6개 값이 모두 NULL인
동일 형태의 행으로 대체한다. 존재해야 하는 버전에서 NULL 또는 이름 불일치는 거절하며, 존재하면
SHOW의 다른 메타데이터도 비어 있으면 안 된다. 생성 당시 sql_mode는 strict 모드가 있어야 하고
NO_BACKSLASH_ESCAPES·ANSI_QUOTES·PIPES_AS_CONCAT은 허용하지 않는다. 실행 연결에도 같은
조건을 요구한다. 이 조회 형식은 소스 계약이며 실제 적용 이력은 대상 DB에서 별도로 확인해야 한다.

AuthServer는 서비스 접속 변수와 함께 기대 DB 이름 `ACTIONRPG_DB_SCHEMA`, 배포 SQL 절대
경로 `ACTIONRPG_DB_MIGRATIONS_DIRECTORY`를 사용한다. 새 Auth Runtime에는 login_google_account/get_auth_account_status/
get_inventory_schema_migration_history의 EXECUTE가 필요하며 테이블 직접 읽기/쓰기·DDL·감사 쓰기를 부여하지 않는다. DEFINER는 지속적으로 유효해야
하며 테이블 접근과 루틴 정의 조회에 필요한 범위를 실제 환경에서 검토한다. SHOW 원문은 해당
루틴의 DEFINER이거나 SHOW_ROUTINE 등의 전체 조회 권한이 있어야 보인다. EXECUTE만 가진
주체는 Create Procedure가 NULL일 수 있다. 열한 루틴의 DEFINER를 같은 전용 생성 주체로 유지하면
런타임에 광범위 조회 권한을 추가하지 않고 조회 프로시저의 DEFINER 문맥에서 확인할 수 있다.
권한 부족을 성공으로 처리하지 않는다. 도구용 주체와 런타임 주체를 분리하며 GRANT는 포함하지 않는다.

마이그레이션을 끝낸 뒤 필요한 SQL 파일을 함께 배포하고 AuthServer가 실제 현재 버전 000005와
구조를 검증한 뒤 DB 연계 기능을 활성화한다. 요구 버전은
[LoginSchemaVerifier.h](../../ActionRPGServer/AuthServer/Database/LoginSchemaVerifier.h)의
`REQUIRED_SCHEMA_VERSION=5`이며 [검증 구현](../../ActionRPGServer/AuthServer/Database/LoginSchemaVerifier.cpp)은
전체 이력과 구조 검증에 성공해야 검사한 OdbcDatabase 인스턴스에 연결된 증명을 만든다.
다른 DB 객체나 설정 파일의 버전 플래그로 이 증명을 대체하지 않는다.

Auth main은 기동 검증을 최대 15초 기다린다. 실패·시간 초과·조회 불가·head<5·미지원 상위 버전에는
challenge/로그인/티켓 발급·소비·갱신 기능이 HTTP 503으로 차단된다. HTTPS 프로세스가 반드시
종료되는 것은 아니며, 로그아웃과 기존 소유권 release는 이 게이트 밖에 있다. 000005 Down 후에는
최신 Auth의 로그인·입장을 활성화할 수 없다. DB가 복구돼도 기동 시 확정한 준비 상태는 자동으로
바뀌지 않으므로 서비스를 정지하고 재기동해 다시 검증해야 한다. Google 인증 실패나 이후 runtime
DB 요청 오류는 별도의 거절 경로이며 성공한 기동 증명이 계속된 요청 성공을 보장하지 않는다.

서버 시작은 마이그레이션 실행 시점이 아니다. 검증 후 서비스 중 DDL 또는 배포 SQL 파일을 바꾸지
않는 조건이 필수이며, 이 증명은 지속 감시나 DB 스키마 변경 방지 기능이 아니다.

소스 근거: [수동 실행기](../../Tool/Database/Migrate.ps1),
[Up 배치](../../Tool/Database/UpMigration.bat), [Down 배치](../../Tool/Database/DownMigration.bat),
[이력 결과 매핑](../../ActionRPGServer/AuthServer/Database/SchemaHistoryProcedure.cpp),
[Auth 기동·HTTP 게이트](../../ActionRPGServer/AuthServer/main.cpp).

이번 문서 정리는 코드·SQL·경로·계약을 대조한 정적 확인만 수행했다. 빌드·기능 테스트·서버 실행·
프로세스 종료·DB 접속·SQL 적용은 수행하지 않았다. 실제 DB 적용 버전, 드라이버/TLS/
최소 권한 호환성, MySQL 메타데이터/원문 조회와 서버 DB 왕복 동작을 에이전트가 직접 검증한 것은 아니다.
기존 로컬 V3 입장 확인과 이후 V4 이력/검사 조회 결과는 1절에 별도로 기록했다.
