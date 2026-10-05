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

MySQL 8.0.47 마이그레이션은 수동으로 실행한다. DB를 사용하는 서비스를 모두 정지하고,
64비트 MySQL ODBC 드라이버 및 전용 접속 환경 변수 `ACTIONRPG_MIGRATION_CONNECTION_STRING`을
준비한다. 접속 암호는 파일이나 명령 인자에 저장하지 않는다. 아래 `actionrpg`를 실제 스키마 이름으로
바꾼다. `-ServicesStopped`는 운영자의 정지 확인이며 자동 정지 기능은 아니다.

```bat
Tool\Database\UpMigration.bat -Database actionrpg -ServicesStopped
Tool\Database\DownMigration.bat -Database actionrpg -ServicesStopped
```

Up은 모든 미적용 버전을 순서대로 적용하고, Down은 현재 적용된 마지막 버전 하나만 되돌린다.
공통 실행기 `Database/Migrate.ps1`이 대상 확인·잠금·체크섬·감사 이력을 관리한다.
이력 기반 000000은 유지하며, 계정 테이블을 삭제하는 000001 Down은 계정 데이터가 있으면 거절한다.
현재 지원 스키마는 000001~000003이다. 새 스키마 버전에는 실행기와 Auth 검증 계약도 갱신한다.
실패 이력을 수동 삭제하거나 부분 적용된 SQL을 다시 실행하지 않는다. 설치·DB 생성·접속·적용은
아직 수행하지 않았다. 권한·실패 복구·서버 연계 상세는
[DB 마이그레이션 규칙](../docs/workflows/DATABASE_MIGRATIONS.md#9-수동-updown-실행-계약)을 따른다.
