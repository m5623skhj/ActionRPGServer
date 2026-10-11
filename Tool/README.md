# Packet Generator

`PacketDefine.yml`을 기준으로 GameRoomServer와 ActionRPGClient가 공유할 RUDP 패킷 코드를 생성하고,
`TownPacketDefine.yml`을 기준으로 TownServer와 ActionRPGClient가 공유할 TCP 패킷 선언을 생성한다.
패킷 ID는 YAML의 선언 순서대로 1부터 자동 부여된다. 기존 패킷의 순서를 바꾸거나 중간에서 삭제하지 않고 새 패킷은 마지막에 추가한다.

## 준비

```bat
python -m pip install -r Tool\PacketGenerator\requirements.txt
```

## 생성

```bat
Tool\PacketGenerate.bat
```

생성 결과가 YAML과 일치하는지만 확인하려면 다음 명령을 사용한다.

```bat
Tool\PacketGenerate.bat --check
```

출력 위치와 네임스페이스는 각 YAML의 `Protocol` 항목에서 관리한다. 생성된 `DungeonProtocol.h`,
`DungeonProtocol.cpp`, `TownPacket.generated.h`는 직접 수정하지 않는다.

## DB 마이그레이션

DB 개요와 버전 SQL 구성은 [계정 DB README](../ActionRPGServer/Database/README.md)를 참조한다.

현재 구현은 MySQL 8.0.46 / InnoDB, 64비트 Windows PowerShell 5.1 / System.Data.Odbc의
수동 실행기다. 서버 기동 시 자동으로 실행하지 않는다. SQL 파일 최신 버전은 000006이며 실제 DB
직전 확인 결과는 2026-10-08 로컬 V000004 적용 성공과 새 검사 EXECUTE 확인이다.
V5 적용 결과와 새 PC의 상태는 해당 대상에서 별도로 확인한다. 처음 구성하는 경우 [새 PC 설정·트러블슈팅](../docs/workflows/LOCAL_DEVELOPMENT_SETUP.md)의
계정 생성·권한·대화형 마이그레이션 안내부터 따른다.

1. 대상 MySQL·스키마 이름, 64비트 MySQL ODBC 드라이버, TLS·전용 실행 주체와 권한을 준비한다.
   스키마가 없으면 Up의 `-CreateDatabase`로 생성할 수 있다. 이때 대상 DB의 CREATE 권한이 필요하다.
2. Auth·Town·Room과 DB를 사용하는 기존 접속을 종료하고 서비스 정지를 확인한다.
3. `Tool\Database`의 UpMigration.bat 또는 DownMigration.bat을 실행하고 콘솔 안내를 따른다.
   대상 DB 기본값은 `actionrpg`이며 서버 종료 확인에 `Y` 또는 `y`를 입력한다. 연결 환경 변수가 없으면
   64비트 MySQL Unicode 드라이버를 찾아 호스트·포트·마이그레이션 계정과 숨김 비밀번호를 받는다.
   기본값은 `127.0.0.1:3306` / `actionrpg_migrator`이며 Enter로 선택한다. 드라이버가 여러 개면 번호를 고른다.
4. 기존 자동화는 `ACTIONRPG_MIGRATION_CONNECTION_STRING`과 아래 인자를 그대로 제공할 수 있다.
   암호를 파일·Git·명령 인자에 쓰지 않으며 runtime 연결 문자열을 대신 사용하지 않는다.

`-Database`를 생략하면 대상 이름을 묻는다. 기본 모드에서는 연결에서 이미 선택한 DATABASE()와 대조한다.
Up에 `-CreateDatabase`를 주면 DB가 없을 때 생성하고 선택한 뒤 기존 마이그레이션을 이어간다.
이 옵션은 소문자 영문자로 시작하는 1~64자의 소문자 영문·숫자·밑줄 이름만 받으며 시스템 DB는 거부한다.
`-ServicesStopped`를 생략하면 서버 종료 확인을 묻는다. 이 옵션과 콘솔 확인은 운영자의 정지 확인이며
프로세스를 자동으로 종료하거나 검사하지 않는다. 비밀번호·접속 정보는 저장하지 않고 환경 변수도 변경하지 않는다.
배치는 자식 PowerShell에만 RemoteSigned 실행 정책을 지정하며 상위 조직 정책은 변경하지 않는다.
인자 없이 실행하면 결과 창을 유지하기 위해 마지막에 키 입력을 기다리고 원래 종료 코드를 반환한다.

```bat
Tool\Database\UpMigration.bat -Database actionrpg -ServicesStopped
Tool\Database\UpMigration.bat -Database actionrpg -CreateDatabase -ServicesStopped
Tool\Database\DownMigration.bat -Database actionrpg -ServicesStopped
```

실패 이력의 구조 비교에는 Up 배치에 `-InspectOnly -InspectVersion 0`을 함께 전달한다.
`InspectVersion`은 비교할 구조 버전(0~6)이며 실제 적용 버전을 확정하거나 이력을 승인하지 않는다.
이 경로는 기존 DB·이력 테이블·검사 프로시저가 있어야 하며 `-CreateDatabase`와 함께 쓸 수 없다.
같은 전용 연결·서비스 정지 확인·잠금을 사용해 감사 행과 CHECK 원문/정규화 실제 값/기대값을 출력한다.
DB 생성, 버전 SQL, 감사 행 INSERT/UPDATE/DELETE는 수행하지 않으며 실패 이력을 그대로 유지한다.
검사 성공도 마이그레이션 성공이나 Auth 준비 완료를 뜻하지 않는다.

```bat
Tool\Database\UpMigration.bat -Database actionrpg -InspectOnly -InspectVersion 0 -ServicesStopped
```

구조가 완전한 초기 V000000 실패 1건에 한해 `-RecoverBootstrap`으로 명시적 복구할 수 있다.
같은 연결·잠금 안에서 배포 체크섬, 완료된 실패 이력 1건, V0 전체 구조와 객체 수를 재검증하고
기존 FAILED 행을 보존한 채 `migration_history_recovery`/V0/UP/SUCCEEDED 확인 행 1건만 추가한다.
다른 감사 행이나 객체, RUNNING, 불완전한 구조, 체크섬 불일치는 거절한다. 복구는 V0에서 종료한다.
`-CreateDatabase`·`-InspectOnly`·Down과 함께 사용할 수 없고 부분 DDL을 재실행하지 않는다.

```bat
Tool\Database\UpMigration.bat -Database actionrpg -RecoverBootstrap -ServicesStopped
```

`Recovery complete. Active version: V000000.`과 종료 코드 0을 확인한 뒤 별도의 일반 Up으로
000001~000006을 적용한다. 이미 복구된 상태에는 다시 확인 행을 추가하지 않고 중단한다.
복구 확인 INSERT 후 통신/검사 오류가 발생하면 행이 남았을 수 있으므로 진단으로 실제 이력을
확인한다. 실패 행과 확인 행의 즉시 연속 조합을 도구와 수정된 Auth만 인정한다. Auth는 해당 소스를
반영해 다시 빌드해야 하며, 여전히 실제 head=6과 전체 구조 검증을 통과해야 로그인할 수 있다.

V0 이력은 정상이고 첫 V1 UP 실패 1건만 마지막에 남았다면 `-RecoverAccounts`를 사용한다.
V1 전체 구조·양방향 체크섬·계정 테이블 2개의 비어 있음·테이블 3개/검사 루틴 1개/이벤트·트리거
0개를 재검증한다. 기존 모든 행을 보존하고 V1/UP/SUCCEEDED `create_login_accounts_recovery`
행 1건을 추가한 뒤 V1에서 종료한다. 추가 시도·부분 구조·계정 데이터가 있으면 거절한다.
다른 복구 옵션·생성·진단·Down과 함께 사용할 수 없다. 성공 후 일반 Up으로 2~3을 적용한다.

```bat
Tool\Database\UpMigration.bat -Database actionrpg -RecoverAccounts -ServicesStopped
```

Up은 모든 미적용 버전을 순서대로 적용하고, Down은 현재 적용된 마지막 버전 하나만 되돌린다.
공통 실행기 [Database/Migrate.ps1](Database/Migrate.ps1)이 대상 확인·잠금·정규화 SHA-256·감사 이력을
관리한다. Up/Down 두 파일의 체크섬은 최초 적용 후 고정하며 과거 성공 기록도 삭제하지 않는다.

`-CreateDatabase`에는 DSN/FILEDSN/SAVEFILE 없는 DRIVER 기반 접속 문자열을 사용한다.
DATABASE는 생략하거나 `-Database`와 같은 값을 넣는다. 도구는 처음 접속할 때 DATABASE를 제거하고
서버 버전·SQL 모드를 확인한 뒤 같은 이름 기반 잠금을 획득한다. 그 접속으로 DB 생성·선택과
V000000→000004를 수행한다. 새 DB는 utf8mb4 / utf8mb4_0900_ai_ci이며 기존 DB의 설정은 변경하지 않는다.
DB 생성은 버전 이력 기록 전 단계이고 암묵적으로 커밋되므로 후속 실패에도 DB가 남는다.
자동 삭제·계정 생성·GRANT는 수행하지 않으며 Auth 계정에는 생성 권한을 주지 않는다.

V5의 캐릭터/인벤토리 배포는 서비스 정지 후 수동 Up4→5, 새 검사 및 Town 공개
프로시저 EXECUTE 부여, 동일 SQL 11개·새 Auth/Town 배포를 함께 진행한다. V0~V4 SQL과
체크섬은 보존한다. V5 Down은 character_operations/character_items/character_state가
모두 비어 있을 때만 허용하며, 공동 DROP 직후 테이블 잠금을 해제한 다음 루틴을 삭제한다.

현재 신규 배포 계약은 head6다. V6 원장·4개 사용 절차·검사와 교체 claim/save/release를 적용하고
새 EXECUTE 및 교체 3개 EXECUTE를 관리자로 부여한 뒤 새 Auth/Town/Room을 배포한다.
검사는 24결과셋/17루틴/68 IN이며 V6 Down은 terminal을 포함해 원장이 비어 있을 때만 허용한다.
V0~V5 SQL/해시는 변경하지 않는다. 실제 DB 적용은 사용자 수동 절차다.

| 현재 상태/명령 | 실제 동작 |
|---|---|
| DB 없음 + Up -CreateDatabase | DB 생성 → 000000 이력 기반 → 000001~000006 적용 |
| 빈 스키마 + Up | 000000 이력 기반 → 000001~000005 계정·캐릭터·인벤토리 → 000006 사용 원장·쿨타임 |
| 정상 관리 DB + Up | 현재 head 다음부터 000006까지 적용; 이미 6이면 검사만 수행 |
| head=6 + Down | 사용 원장이 전부 비어 있을 때만 삭제하고 V5 claim/save/release 복원; 인벤토리 보존 |
| head=5 + Down | 신규 상태·아이템·요청 테이블이 모두 비어 있을 때만 V5 테이블/루틴 삭제; V4 캐릭터/스킬 유지 |
| head=4 + Down | 캐릭터/스킬 테이블이 모두 비어 있을 때만 두 테이블·확장 검사 삭제; 계정 유지 |
| head=3/2 + Down | 각각 상태 조회/로그인 프로시저 하나 삭제; 계정 데이터 유지 |
| head=1 + Down | 계정·외부 식별자 테이블이 모두 비어 있을 때만 삭제 |
| head=0 + Down | 이력 기반 유지, 새 감사 기록 없이 종료 |
| 완전한 V0 + 초기 FAILED 1건 + Up -RecoverBootstrap | 실패 행 보존, 검증된 복구 확인 행 1건 추가 후 V0에서 종료 |
| 정상 V0 이력 + 완전한 빈 V1 + 첫 V1 FAILED + Up -RecoverAccounts | 기존 이력 보존, V1 복구 확인 행 1건 추가 후 V1에서 종료 |
| 비어 있지 않은 무관리 DB, 미복구 FAILED/RUNNING, 이름/체크섬/구조 불일치 | 중단; 자동 기준 버전 등록·repair·부분 SQL 재실행 없음 |

출력의 host:port/DB, 적용 계획, 마지막 Active version과 종료 코드를 확인한다. 계획을 표시한 뒤
바로 실행하며 추가 확인 프롬프트는 없다. 성공은 종료 코드 0, 오류는 1이다. 현재는 000001~000006
계약만 지원한다. 진단은 별도의 Inspection 결과를 출력하며 임의 목표 버전 적용, 자동 Status 판정,
dry-run, force 데이터 삭제 옵션은 없다.
새 버전은 실행기·Auth 실제 구조 검증 계약도 함께 갱신해야 한다.

실패 시 실제 객체와 감사 이력을 확인하고 별도 복구를 협의한다. 실패 이력을 지우거나 부분 적용 SQL을
재실행하지 않는다. Down은 일반 역변환이며 삭제된 데이터 복원이나 실패 복구를 보장하지 않는다.
다른 MySQL 버전·다른 DBMS 및 자동 데이터 이관은 지원하지 않는다.

성공 후 같은 Up/Down/Infrastructure SQL을 `ACTIONRPG_DB_MIGRATIONS_DIRECTORY`의 절대
경로에 배포하고, Auth의 기대 스키마 `ACTIONRPG_DB_SCHEMA`를 맞춘다. Auth 실제 head=6 검증
성공 후 타운/룸을 연결한다. 서버 상세는 [Auth 개발 계약](../ActionRPGServer/AuthServer/DEVELOPMENT.md),
스키마·현재 버전 조회·권한·실패 복구는
[DB 마이그레이션 사용 및 규칙](../docs/workflows/DATABASE_MIGRATIONS.md#9-수동-updown-실행-계약)을 따른다.
현재 버전 확인에 Up을 사용하면 대기 SQL이 적용될 수 있으므로 문서의 조회 절차를 사용한다.
이번 문서 작업은 실제 DB 접속·적용·빌드·테스트를 수행하지 않았다.
