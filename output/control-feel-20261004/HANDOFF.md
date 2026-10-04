# 조작 전환 개선 및 앞차기 등록

사용자 승인: 마을·던전의 뻑뻑한 조작감 수정과 앞차기 미발동 원인 확인 요청. 코드·데이터 반영 완료. 빌드, 게임/서버 실행, 기능 테스트는 수행하지 않았다. Git stage/commit/push는 하지 않았다.

## 확인한 원인

1. 던전의 서버 이동과 본인 예측이 공중·점프 준비 상태에서 모두 막혀 있었다. 마을은 점프 준비·착지 애니메이션 동안 이동이 막혔다.
2. 기본 행동 및 스킬 시작 시 서버가 이동 방향을 지웠다. 누른 키가 그대로라도 다음 이동 갱신까지 의도가 사라질 수 있었다.
3. 사격 회복은 이동·점프·스킬 전환을 막았다. 던전 사격 준비 200ms, 회복 200ms가 추가됐다.
4. 마을에서는 기본 공격 때문에 로컬 이동이 멈춰도 서버에 방향 입력을 계속 보냈다. 서버 위치 보정이 공격 중 캐릭터를 밀어낼 수 있었다.
5. 제작된 `FrontKick.runtime.zip`의 REPORT.md와 ZIP_CONTENTS.json은 설치하지 않았다고 명시한다. 클라이언트 원본·Debug·Release, 서버 원본·Debug 카탈로그는 비어 있었고 Release 서버 카탈로그는 없었다. 마을은 기존 skills.ini의 색상 효과 경로만 사용했다.

## 반영 내용

- 점프 준비·공중·착지 중 지면 X/Y 이동을 허용하고 던전 본인 예측과 서버 조건을 맞췄다. 서버 높이·피해 판정은 그대로 권위 상태다.
- 마지막 탄 이후 사격 회복에서 이동·점프·스킬로 전환할 수 있다. 준비·발사 중 취소와 추가 탄 생성은 허용하지 않는다. 5발 제한, 공중 전체의 발수 제한, 피격·사망 제한을 유지한다.
- 서버는 행동 시작 시 이동 방향을 유지한다. 스킬/사격의 실제 활성 구간에는 이동을 막고 종료 후 입력을 다시 사용한다. 방 이동은 착지 후 처리하며 방 이동·피격의 정지 처리는 유지한다. 이동 패킷 형식 및 기존 100ms heartbeat를 변경하지 않는다.
- 사격 준비 100ms·회복 80ms, 점프 준비 100ms로 조정했다. 마을의 사격 준비·발사·회복 및 점프 준비 애니메이션 시간도 맞췄다. 걷기/달리기 속도와 점프 높이는 변경하지 않았다.
- 스킬 커맨드를 250ms 보관한다. 막힌 상태에서도 완성한 커맨드를 인식하며 X/C로 끝나는 커맨드가 기본 사격/점프로 동시에 처리되지 않게 한다. 지상/공중 조건을 유지하고 오래된 입력·UI 차단·피격·mapEpoch 변경·방/연결 초기화에서 보관 상태를 버린다.
- 마을에서 로그인 응답의 본인 characterId를 사용하여 등록된 스킬의 모션과 로컬 쿨타임을 표시한다. 마을은 표시 미리보기이며 피해·버프·투사체 전투 판정을 추가하지 않는다. 마을 기본 공격·스킬 중에는 서버에도 정지 입력을 보낸다.
- 기존 제작 패키지의 스킬 JSON, 모션 정의, PNG를 그대로 설치했다. 다른 스킬 목록이 비어 있음을 확인한 뒤 반영했으며 이미지 재생성/편집은 하지 않았다.

## 앞차기 사용 조건

`Character1.FrontKick`, 캐릭터 1, 지상 전용, **↑를 누른 다음 0.35초 이내에 Z**, 재사용 대기 2초. 위 방향을 오래 누른 뒤 Z만 누르면 커맨드가 완성되지 않는다. 모션 8프레임/16FPS/0.5초, 타격 프레임 3~4, 피해 25, 깊이 반경 24. 서버는 같은 시전의 같은 대상에게 한 번만 피해를 적용한다.

## 변경 파일

클라이언트 `C:\Users\KimHyeongJin\source\repos\ActionRPGClient`의 원본 13개:

- `ActionRPGClient/ActionRPGClient/Game/Character.cpp`, `Character.h`
- `ActionRPGClient/ActionRPGClient/Game/GameWorld.cpp`, `GameWorld.h`
- `ActionRPGClient/ActionRPGClient/Game/InputCommandQueue.cpp`, `InputCommandQueue.h`
- `ActionRPGClient/ActionRPGClient/Game/Player.cpp`, `Player.h`
- `ActionRPGClient/ActionRPGClient/Game/PlayerSkillPresentation.h`
- `ActionRPGClient/Assets/Data/animations.ini`, `PlayerSkills.json`, `PlayerSkillVisuals.json`
- 신규 `ActionRPGClient/Assets/Images/Characters/player_front_kick.png`

서버 `C:\Users\KimHyeongJin\source\repos\ActionRPGServer`의 원본 5개:

- `ActionRPGServer/GameRoomServer/GameRoom.cpp`, `GameRoomCombat.cpp`, `GameRoomSkills.cpp`
- `ActionRPGServer/GameRoomServer/Data/Combat.json`, `PlayerSkills.json`

추가로 기존 Debug·Release 실행 위치에 클라이언트 데이터/이미지 각 4개, 서버 데이터 각 2개를 반영했다. Release 서버에 없던 PlayerSkills.json을 생성했다. 총 코드/원본/실행 데이터 30개, 보고서 별도. 프로젝트의 Assets 재귀 복사 대상에 새 PNG가 포함된다.

## 정적 검증과 한계

- 공통 카탈로그 6개(양쪽 원본·Debug·Release)의 내용 일치, visual 카탈로그 3개 내용 일치, 캐릭터/스킬 ID·커맨드·조건·타격 프레임 참조를 확인했다.
- PNG 3개는 기존 승인 패키지 해시 `9c0e9ce2a9701402e78ff8a1c31fd903a1d376ebae214bf35892bd73ea9a1d2f`와 일치한다. 1774×887 PNG 헤더, 모션 sourceRect/pivot 경계와 프레임/FPS 일치를 확인했다.
- 양쪽 사격·점프 준비 시간 일치, C++ 구분자 구조, Git diff 공백 검사를 확인했다. 이것은 C++ 컴파일이나 실제 동작 검증을 대신하지 않는다.
- 신규 상태는 클라이언트 게임 스레드, 서버 기존 room strand에서만 처리한다. 기존 수신 이벤트 큐를 유지하며 공유 버퍼를 추가하지 않았다. wire v1/기존 SKL1 확장 및 패킷 생성 파일은 변경하지 않았다.
- 실제 조작감, 앞차기 피격 판정과 접지, 패킷 지연 중 전환, 클리어/복귀/재도전은 미검증이다. 던전 기본 행동 시작은 여전히 서버 상태 수신을 따르므로 네트워크 지연은 남는다.
- 코드 적용에는 클라이언트와 GameRoomServer의 재빌드가 필요하다. 실행 중에 읽었던 스킬 목록/초기 월드는 메모리에 남으므로 양쪽을 재실행해야 한다. 이 작업은 프로세스를 종료하거나 다시 시작하지 않았다.

백업·반영 전후 해시: `artifacts/control-feel/manifest.json`, `*.before`. 설치 확인: `artifacts/control-feel/INSTALLED_CHECKS.json`. 마지막 마을 정지 입력 수정 전 파일도 `*.pre-adjust`로 보존했다.

## 제안 커밋 로그

* 캐릭터 조작 전환 개선 및 앞차기 스킬 등록
  * 점프 이동·사격 회복 전환과 서버 이동 입력 유지 개선
  * 스킬 커맨드 버퍼 및 마을 행동·이동 상태 일치 처리
  * 앞차기 공통 정의·모션·PNG와 마을 미리보기 연결
  * Debug·Release 데이터 반영 및 정적 검증 기록
