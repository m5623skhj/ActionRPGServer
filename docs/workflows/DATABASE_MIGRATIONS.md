# DB 구현·마이그레이션 사용 및 작업 규칙

문서 버전: 1.2.0

작성일: 2026-10-05

수정일: 2026-10-06 — 현재 구현 기준 스키마·저장 범위·운영 절차와 지원 한계 정리.

적용 범위: ActionRPG 총괄·DB 담당, DB 연계가 필요한 서버 및 다른 담당

## 1. 담당 범위와 현재 상태

DB 담당은 데이터 모델, 스키마·인덱스·제약·프로시저, 영속 데이터 변경, 마이그레이션 파일과 적용 이력, DB 이동 시 호환성과 데이터 변환을 맡는다. 서버의 접속·쿼리 호출·게임 상태 연계 코드는 서버 담당과 계약을 정하고 총괄이 수정 파일 소유자를 배분한다.

작업 저장소는 `C:/Users/KimHyeongJin/source/repos/ActionRPGServer`다. 클라이언트 저장소는 `C:/Users/KimHyeongJin/source/repos/ActionRPGClient`다. 공통 역할·협업 규칙은 같은 디렉터리의 `DUNGEON_CREATION.md`를 따른다.

2026-10-06 시점의 저장소 구현을 기준으로 설명한다. 초기 담당 온보딩 당시의 DB 미구현 설명을
현재 상태로 사용하지 않는다. 현재 서버는 AuthServer, TownServer, GameRoomServer로 책임을
나누며 계정 DB 호출은 AuthServer가 소유한다. DB 전용 서버 프로세스는 구현하지 않았다.

| 구분 | 현재 소스 구현 | 실제 환경에서 확인한 상태 |
|---|---|---|
| DBMS | MySQL 8.0.47 / InnoDB에 한정한 SQL·검증 | 접속 대상·실제 엔진/버전 미확인 |
| 런타임 접근 | `Shared/Database`의 ODBC 풀·작업 큐·`IStoreProcedure<Req, Res>` | 드라이버·권한·DB 왕복 동작 미확인 |
| 영속 구조 | 계정/외부 식별자 V000001, 로그인 V000002, 상태 조회 V000003 | 실제 생성·적용 버전 미확인 |
| 마이그레이션 | PowerShell 5.1 / System.Data.Odbc 수동 Up/Down, V000000 이력 기반 | 실제 대상에서 성공한 감사 이력 미확인 |
| 서버 검증 | Auth 기동 시 DB 이력·실제 구조와 배포 SQL을 대조, 요구 head=3 | 운영 DB 검증 성공 여부 미확인 |

이 문서 작업에서는 소스·SQL·문서만 읽고 실제 DB에 접속하거나 적용하지 않았다. 따라서 파일의
최신 버전 000003, Auth의 요구 버전 3, 실제 DB의 현재 버전은 서로 다른 정보다. 설치·빌드·운영
검증이 끝났다고 추정하지 않는다. 사용 절차는 9절, 테이블과 프로시저 계약은 8절을 따른다.

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

현재 실행기는 같은 MySQL 8.0.47의 빈 목적 스키마에 구조를 만드는 데 사용할 수 있다.
DB 생성, 덤프/복원, 계정 데이터 전송, 접속 주소 전환, 자동 비교·이관 도구는 구현하지 않았다.
아래 항목은 이동 시 지켜야 할 규칙이며 자동 이관 기능의 설명이 아니다.

* 같은 원본 마이그레이션 목록을 초기 버전부터 목표 버전까지 순서대로 적용한다. 목적지 DB에 실제 적용 이력을 기록하고 목표 버전에 도달했는지 확인한다.
* 데이터가 있는 DB의 이전은 스키마 생성과 실제 데이터 이관을 구분한다. 이력만 복사하고 구조·데이터를 생성했다고 보고하지 않는다.
* DB 복제/덤프에 이력이 함께 포함됐다면 실제 스키마·체크섬과 대조한 후 미적용 버전만 적용한다. 대상 환경 차이도 확인한다.

### 다른 DBMS 엔진

현재 다른 MySQL 버전, MariaDB, PostgreSQL, SQL Server 등의 SQL·타입 변환·마이그레이션
실행 어댑터는 제공하지 않는다. ODBC는 접속 API이며 MySQL SQL의 다른 엔진 호환성을 보장하지
않는다. 실행기와 Auth 검증은 MySQL 8.0.47 계약을 검사하므로 드라이버만 바꿔 이동할 수 없다.

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

대상은 MySQL 8.0.47 / InnoDB이며 파일 경로는 `ActionRPGServer/Database/Migrations/MySQL/`다.

| 버전 | 선행 버전 | 실행 파일 | 목적 |
|---|---|---|---|
| 000000 | 빈 스키마 | [Infrastructure/V000000__migration_history.sql](../../ActionRPGServer/Database/Migrations/MySQL/Infrastructure/V000000__migration_history.sql) | 초기 이력 테이블·조회 프로시저; Down 없음 |
| 000001 | 000000 기반 | [V000001__create_login_accounts.sql](../../ActionRPGServer/Database/Migrations/MySQL/V000001__create_login_accounts.sql) | 계정·외부 식별자 테이블 |
| 000002 | 000001 | [V000002__create_google_login_procedure.sql](../../ActionRPGServer/Database/Migrations/MySQL/V000002__create_google_login_procedure.sql) | Google 로그인 조회·자동 가입 |
| 000003 | 000002 | [V000003__create_auth_account_status_procedure.sql](../../ActionRPGServer/Database/Migrations/MySQL/V000003__create_auth_account_status_procedure.sql) | 티켓 발급·입장 승인 전 계정 상태 재확인 |

000001/000002의 SQL은 변경하지 않는다. `login_google_account(subject)`의 결과는 기존대로
`result_code(int32), account_id(uint64), account_status(int32), was_created(int32)` 한 행이다.
0 정상/1 정지, was_created 0/1, 모든 값 NOT NULL, 정지에서는 was_created=0 계약을 유지한다.
AuthServer의 전체 기능에 필요한 논리 버전은 이제 000003이다. 향후 상위 버전 호환성은 별도로
선언하며 단순히 `MAX(version) >= 3`이라는 이유로 허용하지 않는다.

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

AuthServer는 승인된 배포 SQL 목록을 기준으로 000001 → 000002 → 000003이 누락 없이 성공했고,
이름·순서·실행 파일 체크섬이 해당 도구의 규칙과 일치하는지 확인한다. 원본 SHA-256과 도구의
다른 알고리즘 체크섬을 같은 값으로 비교하지 않는다. 도구가 선행 관계를 저장하지 않으면 승인된
버전 파일 순서와 실제 이력을 대조한다. 이후 실패 이력·미지원 버전·체크섬 불일치도 거절한다.
필요한 테이블/프로시저의 실제 계약 역시 확인하며 이력만으로 구조가 동일하다고 가정하지 않는다.

검증은 실제 사용할 계정 DB를 대상으로, 마이그레이션 실행자 잠금과 서비스/DDL 배포 순서가
협의된 상태에서 수행한다. 검증 도중 변경된 이력이나 서비스 중 임의 DDL을 허용하지 않는
배포 절차가 필요하다. 조회 불가·대상 불명·검증 실패 시 로그인·새 티켓 발급/입장 승인을 차단하며,
성공을 가정하는 증명 객체를 생성하지 않는다. 로그아웃과 이미 발급된 권한 취소는 계속 허용한다.

남은 배포 결정은 실제 대상 DB/스키마·환경, ODBC 드라이버와 접속 보안, 실행 주체의 최소 권한,
서비스 정지·코드 배포 순서다. 000003은 계정 상태 조회이며 이력 조회는 Infrastructure의 000000이다.

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

### 영속 데이터와 메모리·파일 데이터의 구분

| 데이터 | 현재 위치/수명 | DB 저장 여부 |
|---|---|---|
| 내부 계정·정지 상태·로그인 시각 | accounts | 저장하도록 SQL 구현됨; 실제 DB 적용 미확인 |
| Google provider/sub 매핑·연결 시각 | account_identities | 동일 |
| 마이그레이션 시도·방향·체크섬·시각·성공/실패 | schema_migrations | 동일; 감사 이력은 Down에도 유지 |
| 로그인 challenge/nonce, 게임 토큰, 입장 티켓, 타운 연결 소유권/lease | 단일 Auth 프로세스 SessionRegistry | DB 저장 없음; Auth 재시작으로 유실 |
| 플레이어 캐릭터·레벨·SP·습득 스킬·접속 중 진행 | Town/GameRoom의 메모리 상태 | DB 테이블·로그인 시 복원·로그아웃 저장 없음 |
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

## 9. 수동 Up/Down 실행 계약

### 파일과 실행

정식 도구는 `Tool/Database/UpMigration.bat`, `DownMigration.bat`이며 공통 실행기는
`Migrate.ps1`이다. 64비트 Windows PowerShell 5.1과 동일 비트의 MySQL ODBC 드라이버를 사용한다.
별도 마이그레이션 제품 설치 없이 ODBC로 실행한다. 서비스용 접속 문자열을 자동으로 재사용하지
않으며 전용 환경 변수 `ACTIONRPG_MIGRATION_CONNECTION_STRING`을 사용한다. 암호가 포함된
값을 명령 인자·배치 파일·Git에 넣거나 로그로 출력하지 않는다. DB 접속의 TLS/인증 설정은 실제
환경에 맞게 별도로 준비한다. 로컬 PowerShell 실행 정책은 존중하며 도구가 자동 우회하지 않는다.

모든 DB 사용 서비스를 정지한 뒤 실행한다. 다음 `actionrpg`는 실제 DB 이름으로 바꾸는 예시다.
`-Database`는 접속 문자열이 선택한 스키마와 정확히 일치해야 한다.
`-ServicesStopped`는 운영자가 정지를 확인했다는 표시이며 프로세스를 자동 종료하거나 정지를
검증하는 기능이 아니다. DB 생성·계정 생성·권한 부여 역시 자동 수행하지 않는다.

명령 예시는 저장소 루트에서 실행한다. 다른 디렉터리에서는 배치의 전체 경로를 사용한다.
배치는 자기 위치의 Migrate.ps1을 호출하고 실행기는 자기 위치를 기준으로 SQL을 찾으므로,
배포 시 `Tool/Database`와 `ActionRPGServer/Database/Migrations/MySQL`의 상대 배치를 보존한다.

| 인자/설정 | 구현된 의미 |
|---|---|
| UpMigration.bat / DownMigration.bat | 공통 실행기에 각각 -Direction Up/Down을 전달 |
| -Database 이름 | 필수, 1~64자; 연결에서 선택된 DATABASE()와 정확히 대조. DB 생성·선택 인자가 아님 |
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
Tool\Database\DownMigration.bat -Database actionrpg -ServicesStopped
```

* Up: 실제 성공 이력을 재생해 현재 상태를 계산한 뒤 모든 미적용 버전을 오름차순 적용한다.
  현재 배포 계약은 000001~000003이며 최신 상태에서는 새 실행 이력을 만들지 않는다.
* Down: 현재 적용된 가장 높은 버전 하나만 역변환하고 DOWN 성공 이력을 추가한다.
  과거 UP 기록은 삭제하지 않는다. 이후 Up은 되돌린 버전부터 다시 적용한다.
* 000000: 이력 관리 기반이다. 빈 스키마에 최초 Up할 때만 생성하며 Down 대상에서 제외한다.
  테이블만 남은 중단된 초기화나 기존 비어 있지 않은 DB를 성공 상태로 추정해 등록하지 않는다.
* 파일은 Up `VNNNNNN__name.sql`, Down `Down/동일파일명.sql` 한 쌍이다. 이미 적용된 Up/Down과
  Infrastructure 파일은 모두 고정한다. 새 버전에는 변경에 맞는 실행기·Auth 실제 구조 검증 계약도
  함께 갱신해야 한다. 현재 코드에 파일만 추가해 미지원 스키마를 승인하지 않는다.

| Down 버전 | 역변환 | 데이터 조건 |
|---|---|---|
| [000003 Down](../../ActionRPGServer/Database/Migrations/MySQL/Down/V000003__create_auth_account_status_procedure.sql) | `get_auth_account_status` 삭제 | 계정 데이터 유지; 최신 Auth 코드와 호환되지 않음 |
| [000002 Down](../../ActionRPGServer/Database/Migrations/MySQL/Down/V000002__create_google_login_procedure.sql) | `login_google_account` 삭제 | 계정 데이터 유지 |
| [000001 Down](../../ActionRPGServer/Database/Migrations/MySQL/Down/V000001__create_login_accounts.sql) | `account_identities`, `accounts` 삭제 | 두 테이블이 모두 비어 있을 때만 허용 |

000001 Down은 두 계정 테이블과 이력 테이블에 WRITE 잠금을 잡고 빈 상태를 확인한 뒤 두 계정
테이블을 한 DROP 문장으로 삭제한다. 빈 상태 검사를 통과하지 못하면 DDL과 실행 이력 추가를
하지 않는다. 잠금 대기와 연결 오류는 실패로 처리한다. 런타임 계정이 있는 DB의 계정 테이블 삭제나
데이터 복원은 이 도구의 범위가 아니다. [MySQL 테이블 잠금](https://dev.mysql.com/doc/refman/8.0/en/lock-tables.html).

### 잠금·체크섬·감사 이력

한 ODBC 연결이 전체 실행 동안 이름 기반 DB 잠금을 보유한다. 이름은
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
중복·버전 누락·알 수 없는 버전·이름/체크섬 불일치·FAILED/RUNNING은 거절한다. 실패를 걸러낸
MAX(version) 조회나 이력 삭제로 현재 버전을 계산하지 않는다. 완료 시각은 시작 시각 이후여야 한다.

MySQL DDL과 이력 쓰기를 한 트랜잭션으로 롤백하지 않는다. 초기 이력 테이블 생성 직후 감사 행
쓰기 이전에도 중단될 수 있다. SQL 실패·이력 쓰기 실패·구조 검사 실패에는 다음 버전을 적용하지
않는다. 실제 구조와 마지막 감사 상태를 확인해 별도로 승인받은 복구를 한다. 일반 Down은 성공
적용된 버전의 역변환이며 부분 실패 복구 명령이 아니다. [MySQL 암묵적 커밋](https://dev.mysql.com/doc/refman/8.0/en/implicit-commit.html).

### 새 DB 초기 구축과 기존 DB 갱신

1. 실제 대상 MySQL 8.0.47의 호스트·포트·스키마, 적용할 승인 SQL 배포본, 데이터 유지 조건을
   정한다. DB/schema·전용 생성 주체·runtime 주체·TLS/ODBC 드라이버·권한은 별도로 준비한다.
   실행기는 DB 또는 사용자를 만들거나 GRANT하지 않는다.
2. Auth·Town·Room의 기존 접속을 종료하고 정지 상태를 확인한다. Auth 메모리 소유권이 사라지는
   재시작 전에 타운/룸의 이전 연결까지 종료하는 조건은 Auth 개발 계약을 따른다.
3. 빈 신규 스키마에는 이력 테이블·기존 테이블/뷰·프로시저/함수·이벤트·트리거가 없어야 한다.
   Up은 V000000 이력 기반을 생성한 후 000001→000002→000003을 순서대로 적용한다.
4. 기존 관리 DB는 아래 조회 절차로 전체 이력과 구조를 먼저 확인한다. 정상 head=1이면 2→3,
   head=2이면 3, head=3이면 새 감사 행 없이 검사만 끝낸다. Down 성공으로 낮아진 head에도
   같은 규칙을 사용한다. 이력이 없는 비어 있지 않은 DB를 자동으로 기준 버전 등록하지 않는다.
5. 보호된 환경의 전용 접속 문자열과 실제 스키마 이름으로 Up 배치를 실행한다. 대상 host:port,
   DB 이름, 계획, 각 버전의 성공, 마지막 `Complete. Active version: V000003.`을 확인한다.
   표시한 계획 뒤에 바로 SQL을 실행하며 추가 확인 프롬프트나 dry-run 단계는 없다.
6. 종료 코드 0과 최종 상태를 확인하고 동일 Up/Down/Infrastructure 파일을 Auth 배포 디렉터리에
   둔다. 종료 코드 1·중간 중단·불명확한 완료에서는 후속 서비스 배포를 진행하지 않는다.
7. Auth를 기동해 실제 계정 DB 검증과 로그인/입장 준비 상태를 확인한 후 타운·룸을 연결한다.
   Auth는 마이그레이션을 실행하지 않는다. 실제 환경에서 로그인·상태 조회·연계 동작 확인은
   별도로 승인받은 검증 범위에서 진행한다.

현재 Up의 목표는 준비된 계약의 최신 000003으로 고정된다. `-TargetVersion`, 특정 버전만 실행,
자동 DB 생성·기준 버전 등록, `-Status`/`-WhatIf`/조회 전용 배치는 구현하지 않았다. 임의 버전의
SQL을 골라 관리 도구에서 실행하는 방식으로 순서·감사 기록을 우회하지 않는다. 새 버전을 추가할
때는 실행기의 3개 파일/구조 검사와 Auth의 4개 기반·버전 파일/요구 head 계약을 함께 변경해야 한다.

### 현재 버전의 조회와 확인

이 문서에 기재한 000003은 배포 파일 버전이다. 실제 버전을 확인하려면 승인된 관리 환경에서
선택 DB와 `get_schema_migration_history()` 결과를 조회해야 한다. 아래 SQL은 조회 예시이며
문서 작성 중 실행하지 않았다. 호출은 검사 잠금을 잠시 획득하지만 감사 행이나 스키마를 바꾸지 않는다.

```sql
SELECT DATABASE(), @@version, @@version_comment;
CALL get_schema_migration_history();
```

1. 첫 SELECT의 대상이 의도한 스키마·엔진인지 확인한다. CALL의 첫 결과셋(아래 계약의 번호 0)에서도
   같은 DB·버전과 history_format=1, checksum_format=sha256-utf8-lf-v1을 확인한다.
2. 두 번째 결과셋(번호 1)의 전체 감사 행을 execution_id 순서로 확인한다. 최초 000000 UP 성공
   이후 UP은 head+1, DOWN은 현재 head를 되돌린다. NULL 완료 시각이나 FAILED/RUNNING을
   숨겨 계산하지 않는다. 예: 0UP→1UP→2UP→3UP→3DOWN의 현재 head는 2이며 MAX(version)은 3이다.
3. 이름·양방향 체크섬·시각·선행 관계를 배포 파일과 대조한다. 이어지는 실제 컬럼·제약·인덱스·루틴
   입력·SHOW 원문도 해당 head의 계약과 비교한다. 표/프로시저가 존재하는 것만으로 완료를 판단하지 않는다.
4. 실행기의 Get-Head/Get-Snapshot/Assert-Structure와 Auth의 ValidateHistory/ValidateStructure가
   위 검증을 구현한다. 수동 CALL이나 눈으로 읽은 최대 번호는 이 검증의 대체가 아니다.
   Auth 준비를 위해서는 현재 DB에서 검증에 성공한 head=3 증명이 필요하다.

조회 불가·원문 NULL·잠금 획득 실패라면 현재 버전 확인에 실패한 것이다. 0 또는 3으로 가정하지
않는다. Up을 단순 조회용으로 실행하면 미적용 SQL을 적용할 수 있으므로 상태 확인에 사용하지 않는다.

### Down과 실패 후 대응

Down도 관련 서비스를 정지하고 같은 대상·파일·이력·구조 검사를 거친 뒤 현재 head 한 단계만
되돌린다. 반복 Down은 3→2→1→0 순서이며 head=0에서는 기반을 지우거나 새 감사 행을 쓰지 않는다.
3/2 Down은 루틴 삭제 후 계정 데이터를 유지한다. 1 Down은 두 테이블에 데이터가 없어야 하며
테이블 구조를 삭제한다. 전체 데이터 삭제를 허용하는 force 옵션은 없다. Git checkout이나 이전
서버 바이너리 배포는 DB 데이터를 복구하지 않는다. 데이터 복원 도구·백업 생성도 제공하지 않는다.

실패 시에는 출력의 대상·version/statement 단계와 실제 감사 상태·실제 생성된 객체를 확인한다.
연결 문자열·원문 DB 예외를 공개 로그로 옮기지 않는다. 연결이 끊겨 결과가 불명확하거나 기반 테이블만
생성된 경우도 자동 재시작·Down·이력 삭제를 하지 않는다. 000001의 두 CREATE는 따로 커밋되므로
첫 테이블만 생성된 중단 상태가 가능하다. 도구는 정상 성공 기록이 생길 때까지 대기 SQL을 건너뛰지 않는다.

체크섬 불일치는 승인된 배포 원문을 대조하고 코드/SQL 배포 실수를 확인한다. 이력의 체크섬을 현재
파일에 맞춰 덮어쓰지 않는다. FAILED/RUNNING이 하나라도 있으면 현재 구현의 일반 Up/Down은
계속 차단한다. 자동 repair/resume/감사 상태 복구 기능은 없으며, 원인·부분 적용 상태·데이터 유지
조건을 근거로 별도의 복구 SQL과 이력 처리 계약을 승인받아야 한다. 이번 문서 변경은 그 복구를
수행하거나 새 복구 기능을 구현하지 않는다.

실행기는 ODBC 명령에 30초 타임아웃을 요청하고 조회 전체 4096행·값 32768문자, SQL 파일
1MiB 한도를 사용한다. 실제 드라이버의 타임아웃 동작은 운영 환경에서 확인해야 한다.
Auth runtime ODBC는 기본 2연결/대기 128건, 연결 5초·쿼리 10초·큐 대기 30초, 결과 전체
4096행/16개 데이터 결과셋/4MiB 한도를 사용한다. Auth HTTP의 15초 대기가 먼저 끝나도 이미
시작된 DB 실행을 취소했다는 뜻은 아니다. 누적 감사 이력/메타데이터가 한도를 넘으면 검증이 차단되며
이력을 임의로 잘라 해결하지 않는다. 조회 페이지화·승인된 감사 보존/한도 확장은 별도 구현 대상이다.

### AuthServer 조회 계약

`get_schema_migration_history()`는 같은 이름 기반 잠금을 획득해 조회하고 해제한다. 잠금 획득
실패 시 SQL 오류이며 성공 플래그를 반환하지 않는다. 실행기 연결이 이미 잠금을 가진 경우 MySQL의
재귀 잠금 횟수 중 조회가 추가한 횟수만 해제한다. 데이터 결과셋은 정확히 10개이며 첫 7개는
문자열로 캐스팅한다. 추가 3개는 원본 SHOW CREATE 결과다. 각 결과셋은 SQL 파일의 순서를 따른다.

| 결과셋 | 컬럼 순서 |
|---|---|
| 0 (5) | history_format=`1`, checksum_format, database_name, engine_version, engine_comment |
| 1 (9) | execution_id, version, name, direction, up_checksum, down_checksum, state, started_at, finished_at |
| 2 (9) | table_name, column_name, column_type, is_nullable, collation_name, extra, engine, column_default, character_set_name |
| 3 (11) | table_name, constraint_name, constraint_type, column_name, referenced_table_name, referenced_column_name, check_clause, enforced, referenced_table_schema, update_rule, delete_rule |
| 4 (6) | table_name, index_name, non_unique, seq_in_index, column_name, sub_part |
| 5 (4) | routine_name, security_type, sql_data_access, routine_definition |
| 6 (7) | routine_name, ordinal_position, parameter_mode, parameter_name, dtd_identifier, character_set_name, collation_name |
| 7 (6) | get_schema_migration_history의 SHOW CREATE: Procedure, sql_mode, Create Procedure, character_set_client, collation_connection, Database Collation |
| 8 (6) | login_google_account의 동일 SHOW CREATE 컬럼 |
| 9 (6) | get_auth_account_status의 동일 SHOW CREATE 컬럼 |

NULL은 원본 NULL과 동일하며 임의로 빈 문자열과 합치지 않는다. 대상 테이블은 schema_migrations,
accounts, account_identities, 대상 루틴은 get_schema_migration_history, login_google_account,
get_auth_account_status다. 실제 기본값·InnoDB·CHECK 활성화·PK/인덱스·현재 스키마를 가리키는
FK RESTRICT·프로시저 본문/입력 문자셋을 대조한다. 정보 조회 권한 부족이나 본문 NULL도 거절한다.

ROUTINE_DEFINITION은 내부 definition_utf8에서 가져오며 `_binary` 같은 문자셋 introducer가
제거되므로 배포 원문과 직접 본문을 비교하지 않는다. 결과셋 5의 이름·보안·접근 특성을 확인하고,
실제 본문은 결과셋 7~9의 Create Procedure에서 인용된 DEFINER와 헤더를 제외한 BEGIN~END를
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
경로 `ACTIONRPG_DB_MIGRATIONS_DIRECTORY`를 사용한다. Runtime에는 세 프로시저 EXECUTE만
필요하며 테이블 직접 읽기/쓰기·DDL·감사 쓰기를 부여하지 않는다. DEFINER는 지속적으로 유효해야
하며 테이블 접근과 루틴 정의 조회에 필요한 범위를 실제 환경에서 검토한다. SHOW 원문은 해당
루틴의 DEFINER이거나 SHOW_ROUTINE 등의 전체 조회 권한이 있어야 보인다. EXECUTE만 가진
주체는 Create Procedure가 NULL일 수 있다. 세 루틴의 DEFINER를 전용 생성 주체로 유지하면
런타임에 광범위 조회 권한을 추가하지 않고 조회 프로시저의 DEFINER 문맥에서 확인할 수 있다.
권한 부족을 성공으로 처리하지 않는다. 도구용 주체와 런타임 주체를 분리하며 GRANT는 포함하지 않는다.

마이그레이션을 끝낸 뒤 필요한 SQL 파일을 함께 배포하고 AuthServer가 실제 현재 버전 000003과
구조를 검증한 뒤 DB 연계 기능을 활성화한다. 요구 버전은
[LoginSchemaVerifier.h](../../ActionRPGServer/AuthServer/Database/LoginSchemaVerifier.h)의
`REQUIRED_SCHEMA_VERSION=3`이며 [검증 구현](../../ActionRPGServer/AuthServer/Database/LoginSchemaVerifier.cpp)은
전체 이력과 구조 검증에 성공해야 검사한 OdbcDatabase 인스턴스에 연결된 증명을 만든다.
다른 DB 객체나 설정 파일의 버전 플래그로 이 증명을 대체하지 않는다.

Auth main은 기동 검증을 최대 15초 기다린다. 실패·시간 초과·조회 불가·head<3·미지원 상위 버전에는
challenge/로그인/티켓 발급·소비·갱신 기능이 HTTP 503으로 차단된다. HTTPS 프로세스가 반드시
종료되는 것은 아니며, 로그아웃과 기존 소유권 release는 이 게이트 밖에 있다. 000003 Down 후에는
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
최소 권한 호환성, MySQL 메타데이터/원문 조회와 서버 DB 왕복 동작은 미확인이다.
