# 점프력 데이터 감소·공격 방향 고정 클라이언트 결과

작성: 2026-10-04. 승인된 점프 초기 속도 감소와 공격 방향 고정을 클라이언트 원본 코드·데이터에 반영했다. 이전 파티 상세·가입 승인·강퇴 중앙 알림 및 던전 UI 제한을 보존했다. 이번 작업에서 빌드·테스트·게임/서버 실행·프로세스 종료·백업·stage/commit/push는 하지 않았다.

## 실제 점프력 변경 및 서버 일치

| 사용처 | 변경 전 초기 속도 | 변경 후 | 중력 | 데이터/적용 경로 |
|---|---:|---:|---:|---|
| 마을 기본 및 Character1/2/3 | 1035 | 690 | 1800 유지 | Assets/Data/characters.ini → Character::ConfigureJumpSpeed → jumpSpeed → UpdateJump |
| 던전 서버 공통 캐릭터 | 851.43 | 567.62 | 1200 유지 | 서버 Data/Combat.json → CombatDefinition::Load → UpdateCombat / 최초 월드 combatRules |
| 던전 클라이언트 | 서버 제공 값 | 서버 제공 567.62 그대로 | 서버 제공 1200 그대로 | DungeonWorld::Parse → CombatRules::Parse / CombatPlayerState.height·verticalSpeed → Character::UpdateCombatPresentation |

690=1035×2/3, 567.62=851.43×2/3다. 점프 높이를 픽셀로 하드코딩하지 않았다. 동일 중력에서 연속 시간의 최고 높이 비율은 4/9이며 실제 프레임 적분/반동을 포함한 높이는 실행 측정하지 않았다. 기존 마을/던전의 속도와 중력이 서로 다르므로 같은 숫자로 합치지 않고 각 원본에 승인된 비율만 적용했다.

마을의 기존 JUMP_SPEED 상수를 제거했다. 기존 Characters 자산에 등록된 `characters.ini`의 `[Default]`, `[Character1]`, `[Character2]`, `[Character3]`에 `jump_speed=690`을 작성했다. 새 캐릭터 데이터 프레임워크나 다른 물리 수치의 데이터화는 추가하지 않았다.

Character 생성 때는 Default를 읽는다. 로그인 EnterTownResponse의 characterId를 처리할 때 `GameWorld::ProcessNetworkEvents`가 `player.ConfigureJumpSpeed(characterDefinitions, localCharacterId)`를 호출한다. CharacterN의 jump_speed가 있으면 그 값을 사용하며 없으면 Default로 돌아간다. 따라서 캐릭터별로 다른 수치를 데이터에서 선택할 수 있고, 현재 모두 동일했던 이전 기준값은 모두 같은 비율로 조정했다. 숫자 파싱·전체 문자열 소비·유한값·양수/2000 이하를 확인한다. 실행 중 2/3 배율을 다시 곱하지 않는다.

던전에서는 Player::Update의 HasCombatState 분기가 로컬 UpdateActions/UpdateJump를 사용하지 않는다. 서버 상태의 높이·수직 속도와 서버 중력으로 표시를 예측한다. 새 마을 jump_speed 값이 던전의 서버 높이/속도에 다시 적용되지 않는다. 서버 제공 CombatRules.jumpSpeed의 기존 파싱 경로도 변경하지 않았다.

서버 일치는 `C:/Users/KimHyeongJin/source/repos/ActionRPGServer/output/jump-facing-20261004/SERVER_REPORT.md`, 실제 `GameRoomServer/Data/Combat.json`, `GameRoomCombat.cpp`, `GameRoom.cpp`, `GameRoomSkills.cpp`와 대조했다. 서버 담당 채팅의 완료 보고도 읽기 확인했다. 서버 원본은 클라이언트 담당이 수정하지 않았다.

## 방향 잠금 시작·유지·해제

| 요구 | 클라이언트 파일·함수 | 정적 확인 결과 |
|---|---|---|
| 기본 공격 시작 방향 유지 | Game/Character.cpp: BeginAttack, UpdateActions, IsAttackFacingLocked | 준비/발사/회수 단계 동안 이동 입력이 facingLeft를 덮어쓰지 않음. 처음 바라보던 방향으로 시작하고 후속 발사에도 같은 값 사용. |
| 1~4발 뒤 0.4초 입력 유예 포함 | Character.cpp: IsAttackFacingLocked; 기존 shotInputRemainingSeconds/attackShotCount | 회수→None 취소 후에도 유예가 남은 연속 구간의 방향 유지. 유예 만료 시 정상 전환. |
| 5발 완료 후 해제 | Character.cpp: IsAttackFacingLocked, 기존 UpdateAttack/CancelAttackRecovery | 사격 단계 중에는 계속 잠금. None이 된 5발 완료 구간에서는 방향을 풀며 기존 발수 제한과 유예 수락 규칙 유지. |
| 던전 권위 상태와 표시 | Character.cpp: ApplyCombatState, UpdateCombatPresentation, IsAttackFacingLocked | 기존 shotPhase와 shotCount를 사용. Prepare/Fire/Recover 또는 None의 1~4발 유예에서 로컬 이동 방향으로 자세를 뒤집지 않음. 서버가 유예 종료 시 count=0으로 정리하는 경로와 대조. |
| 첫 입력 직후 제한된 예측 | Character.cpp: PredictAttackFacing, ResolvePredictedAttackFacing, ClearPredictedAttackFacing; GameWorld.cpp: SendCombatActions, ProcessDungeonEvents | 기본 공격과 스킬 송신의 sequence에 방향을 묶음. 스킬 전환은 현재 잠금 방향을 이어받으며 기존 예측 기간을 연장하지 않음. 해당 입력 이후 실제 공격/스킬 상태에서 서버 방향으로 넘김. 정확한 ID의 거절, 제한 시간, 피격·사망·착지·초기화에서 정리. 피해/탄환 생성은 예측하지 않음. |
| 지상/45도 공중 사격 및 반동 | Character.cpp: QueueProjectileRequest, ApplyAirShotRecoil, Render; Assets/Data/projectiles.ini / Projectile.cpp 읽기 검토 | 기본 탄환 X와 뒤 반동이 동일한 facingLeft 사용. PlayerAirBullet의 수평 속도 742.46212와 수직 속도 -742.46212 및 gravity=0의 기존 45도 관계 유지. 모션도 같은 facingLeft로 그림. |
| 기존 스킬 방향 | Character.cpp: PrepareCommandSkill; Player.cpp: ActivateCatalogSkill/Update/Render; GameWorld.cpp: SendCombatActions | 회수 중 스킬 전환은 허용. 기본 공격 잠금 방향을 스킬 시작에 이어받고 기본 공격 유예는 취소. 독립 스킬도 시작 facingLeft를 preview/서버 요청에 전달. 서버 ActiveSkill.facingLeft 처리와 대조. |
| 이동 허용 보존 | Character.cpp: UpdateActions, UpdateCombatPresentation; GameWorld.cpp: SendMovementInput | 방향 잠금을 이동 허용 조건에 추가하지 않음. 기존 공격 중 이동 제한, 회수 취소, 던전 Recover 이동 및 달리기 입력을 유지. |
| 종료·취소·초기화 | Character.cpp: CancelAttack/ApplyHit/BeginJump/UpdateJump/ResetActionState/ApplyCombatState; GameWorld.cpp: ApplyMap/ApplyDungeonMap/ResetDungeonEntry/ProcessDungeonEvents/ApplyCombatState | 마을 피격·착지·새 점프·스킬 전환에서 기본 사격 상태 취소. 던전의 피격/사망/착지 상태에서 예측 잠금 정리. 맵 전환·퇴장·복귀/재도전의 기존 ResetActionState, 실시간 epoch 리셋·클리어·연결 리셋에서 예측 잠금 정리. |

주 잠금은 별도 영구 bool을 저장하지 않고 기존 사격 단계/발수/유예에서 계산한다. 던전은 유예 남은 시간 필드가 없으므로 서버의 유효 shotCount를 사용한다. 서버가 phase=None·유예=0에서 ResetShotState로 shotCount=0을 만드는 것을 확인했다. 5발 None 상태는 새 방향 전환을 허용하지만 기존 사격 수락 한도까지 바꾸지 않는다.

첫 던전 입력의 예측 잠금은 기존 서버 입력 버퍼 250ms + 기존 표시 예측 상한 250ms + 실제 서버 tick 간격으로 제한한다. 서버 30Hz면 약 533ms다. 독립 스킬에도 요청 당시 방향을 같은 방식으로 예측하고, 기본 공격에서 스킬로 전환할 때는 잠긴 방향을 새 요청 ID에 이어받는다. 이미 진행 중인 예측의 기간은 새 스킬 요청으로 연장하지 않는다. accepted ACK만으로 발사가 확정됐다고 가정하지 않고 해당 sequence 이후의 실제 사격/스킬 상태를 기다린다. ACK 역순으로 전역 최신 결과 검사에 걸리더라도 본인 예측 ID의 거절은 먼저 처리한다. 기간이 끝나면 기존 권위 상태로 되돌려 영구 잠금이 남지 않게 했다.

원격 플레이어는 기존 서버 시간 보간 버퍼의 facingLeft·shotPhase·shotCount로 표시한다. 원격 캐릭터에 본인 입력 예측을 적용하지 않는다. 원격 버퍼/네트워크 패킷 구조를 바꾸지 않았다. 기본 공격/스킬 입력 패킷의 facingLeft 및 JSON/SKL1 상태 규격도 유지했다.

## 실제 변경 파일

* C:/Users/KimHyeongJin/source/repos/ActionRPGClient/ActionRPGClient/ActionRPGClient/Game/Character.h
* C:/Users/KimHyeongJin/source/repos/ActionRPGClient/ActionRPGClient/ActionRPGClient/Game/Character.cpp
* C:/Users/KimHyeongJin/source/repos/ActionRPGClient/ActionRPGClient/ActionRPGClient/Game/GameWorld.cpp
* C:/Users/KimHyeongJin/source/repos/ActionRPGClient/ActionRPGClient/Assets/Data/characters.ini

## 정적 검토와 미검증

* 교체 전 원본 해시와 반영 후 결과 해시를 확인했다. 지정 4개 파일만 반영했다. 캐릭터 데이터의 기존 애니메이션 값은 동일하다.
* 괄호, 선언/정의, 캐릭터 ID 선택/default, 수치 파싱, 상수 제거 및 중복 배율 부재를 확인했다. 서버 데이터 567.62/중력1200과 방향 잠금·유예 종료·스킬 캡처 경로를 대조했다.
* 기존 공격 준비/발사/회수·후속 입력·5발 제한 및 공중 누적 발수·ProjectileRequest/반동 함수가 동일함을 반영 전에 확인했다. UpdateActions의 이동은 바라보기 조건만 바꿨으며, UpdateJump는 초기 속도 입력만 바꿨다.
* 이전 승인된 파티·스킬·마을 이동·클리어 관련 15개 GameWorld 보호 함수가 동일함을 반영 전에 대조했다. 이전 파티 변경 파일의 결과 해시도 새 GameWorld 추가분을 제외하고 일치한다. 서버/클라이언트 TownPacket.generated.h는 계속 바이트 단위로 동일하다.
* 새 방향 상태와 점프 데이터 적용은 기존 게임 스레드에서만 처리한다. 네트워크 콜백이나 공유 상태 버퍼에 쓰기를 추가하지 않았다.
* git diff --check 통과. C++ 빌드/링크, 실제 점프 최고 높이, 빠른 반대 방향 5발 입력, 피격/착지/스킬 전환, 맵/연결 리셋 및 원격 플레이어 표시의 실제 동작은 미확인이다. 실행 중인 서버/클라이언트와 기존 룸에는 소스/데이터 수정만으로 즉시 적용되지 않는다.
* 승인된 소스 연계의 미완료 항목은 없다. 제한된 예측 시간보다 상태/ACK가 더 늦게 오는 환경에서 방향 보정이 눈에 보일 수 있으며 실행 검증이 필요하다. 이전 보고에 기록한 작은 창의 기존 파티 목록 배치 한계도 그대로다.

정적 확인 스크립트는 `C:/Users/KimHyeongJin/source/repos/ActionRPGServer/artifacts/jump-attack-facing-20261004/check_static.py`, `check_skill_facing.py`, `verify_installed.py`다. 게임 동작 테스트가 아니다.

## 제안 커밋 로그

* 캐릭터 점프력 데이터 감소와 연속 공격 방향 고정
  * 캐릭터별 마을 jump_speed를 1035에서 690으로 조정하고 데이터 로딩 연결
  * 서버 던전 점프력 567.62를 추가 배율 없이 표시·예측에 사용
  * 최대 5발 공격과 0.4초 유예 동안 모션·사격·반동 방향 유지
  * 스킬 전환 방향 및 거절·피격·착지·맵 전환 시 예측 잠금 정리
  * 기존 이동 허용·파티·스킬·클리어 처리 보존

제안 로그이며 실제 커밋/푸시는 하지 않았다.
