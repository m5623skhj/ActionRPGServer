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

현재 구현은 MySQL 8.0.47 / InnoDB, 64비트 Windows PowerShell 5.1 / System.Data.Odbc의
수동 실행기다. 서버 기동 시 자동으로 실행하지 않는다. SQL 파일 최신 버전은 000003이며 실제 DB
적용 버전은 이 문서 작업에서 미확인이다.

1. 별도로 생성한 대상 스키마, 64비트 MySQL ODBC 드라이버, TLS·전용 실행 주체와 권한을 준비한다.
2. Auth·Town·Room과 DB를 사용하는 기존 접속을 종료하고 서비스 정지를 확인한다.
3. 보호된 환경에 `ACTIONRPG_MIGRATION_CONNECTION_STRING`을 제공한다. 암호를 파일·Git·
   명령 인자에 쓰지 않는다. runtime의 `ACTIONRPG_DB_CONNECTION_STRING`을 대신 사용하지 않는다.
4. 아래 명령을 저장소 루트에서 실행한다. `actionrpg`는 실제 스키마 이름으로 바꾼다.

`-Database`는 연결에서 이미 선택한 DATABASE()를 대조하는 필수 인자이며 스키마를 생성/선택하지
않는다. `-ServicesStopped`도 필수이며 운영자의 정지 확인일 뿐 자동 정지·검사 기능은 아니다.

```bat
Tool\Database\UpMigration.bat -Database actionrpg -ServicesStopped
Tool\Database\DownMigration.bat -Database actionrpg -ServicesStopped
```

Up은 모든 미적용 버전을 순서대로 적용하고, Down은 현재 적용된 마지막 버전 하나만 되돌린다.
공통 실행기 [Database/Migrate.ps1](Database/Migrate.ps1)이 대상 확인·잠금·정규화 SHA-256·감사 이력을
관리한다. Up/Down 두 파일의 체크섬은 최초 적용 후 고정하며 과거 성공 기록도 삭제하지 않는다.

| 현재 상태/명령 | 실제 동작 |
|---|---|
| 빈 스키마 + Up | 000000 이력 기반 생성 → 000001 계정 테이블 → 000002 Google 로그인 → 000003 상태 조회 |
| 정상 관리 DB + Up | 현재 head 다음부터 000003까지 적용; 이미 3이면 검사만 수행 |
| head=3/2 + Down | 각각 상태 조회/로그인 프로시저 하나 삭제; 계정 데이터 유지 |
| head=1 + Down | 계정·외부 식별자 테이블이 모두 비어 있을 때만 삭제 |
| head=0 + Down | 이력 기반 유지, 새 감사 기록 없이 종료 |
| 비어 있지 않은 무관리 DB, FAILED/RUNNING, 이름/체크섬/구조 불일치 | 중단; 자동 기준 버전 등록·repair·부분 SQL 재실행 없음 |

출력의 host:port/DB, 적용 계획, 마지막 Active version과 종료 코드를 확인한다. 계획을 표시한 뒤
바로 실행하며 추가 확인 프롬프트는 없다. 성공은 종료 코드 0, 오류는 1이다. 현재는 000001~000003
계약만 지원하며 임의 목표 버전, 조회 전용/Status, dry-run, force 데이터 삭제 옵션은 없다.
새 버전은 실행기·Auth 실제 구조 검증 계약도 함께 갱신해야 한다.

실패 시 실제 객체와 감사 이력을 확인하고 별도 복구를 협의한다. 실패 이력을 지우거나 부분 적용 SQL을
재실행하지 않는다. Down은 일반 역변환이며 삭제된 데이터 복원이나 실패 복구를 보장하지 않는다.
다른 MySQL 버전·다른 DBMS 및 자동 데이터 이관은 지원하지 않는다.

성공 후 같은 Up/Down/Infrastructure SQL을 `ACTIONRPG_DB_MIGRATIONS_DIRECTORY`의 절대
경로에 배포하고, Auth의 기대 스키마 `ACTIONRPG_DB_SCHEMA`를 맞춘다. Auth 실제 head=3 검증
성공 후 타운/룸을 연결한다. 서버 상세는 [Auth 개발 계약](../ActionRPGServer/AuthServer/DEVELOPMENT.md),
스키마·현재 버전 조회·권한·실패 복구는
[DB 마이그레이션 사용 및 규칙 v1.2.0](../docs/workflows/DATABASE_MIGRATIONS.md#9-수동-updown-실행-계약)을 따른다.
현재 버전 확인에 Up을 사용하면 대기 SQL이 적용될 수 있으므로 문서의 조회 절차를 사용한다.
이번 문서 작업은 실제 DB 접속·적용·빌드·테스트를 수행하지 않았다.
