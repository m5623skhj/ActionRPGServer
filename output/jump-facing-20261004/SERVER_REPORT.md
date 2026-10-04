# 점프 초기 속도 감소 및 연속 공격 방향 고정 서버 결과

작성: 2026-10-04. 총괄이 전달한 사용자 승인 범위에서 수정했다. 이전 던전 스킬 제한·파티 가입 승인·강퇴 통지 및 마을 이동 변경을 보존했다. 현재 서버 요구 범위의 소스 수정과 정적 검토를 완료했으며 실행 검증은 하지 않았다.

## 점프 실제 변경과 계산 경로

| 항목 | 변경 전 | 변경 후 |
|---|---:|---:|
| GameRoomServer/Data/Combat.json player.jump.speed | 851.43 | 567.62 |
| player.jump.gravity | 1200 | 1200 |
| player.jump.prepareSeconds | 0.1 | 0.1 |

567.62는 851.43의 정확한 2/3다. CombatDefinition::Load가 이 데이터를 jumpSpeed로 읽고 GameRoom::UpdateCombat이 준비 완료 시 actor.verticalSpeed에 대입한다. UpdateActor는 기존 gravity로 매 틱 수직 속도와 높이를 계산한다. 새로운 하드코딩 배율을 추가하지 않았고 이동/중력/반동/피격 상승 수치를 수정하지 않았다.

같은 중력에서 연속 시간 계산의 최고 높이 비율은 (2/3)^2=4/9다. 현재 30Hz의 반암시적 오일러 적분은 반동 없는 높이를 기존 약 288에서 약 125로 산출하는 수식상 추정이다. 실제 실행 측정 결과가 아니며 높이를 2/3로 맞추기 위해 속도나 중력을 재조정하지 않았다.

서버는 현재 캐릭터별 점프 프로필 없이 공통 player 점프 데이터를 사용한다. 이번 승인 범위에서 이 구조를 확대하거나 캐릭터별 차이를 덮어쓰는 추가 상수를 만들지 않았다. 최초 월드의 combatRules.jumpSpeed에도 새 값이 그대로 포함된다. 이미 실행 중인 서버 또는 생성된 기존 룸에 소스 파일 수정이 즉시 반영되는 것은 아니다.

## 방향 고정 시작·유지·해제

* 시작: TryQueueAction에서 새 기본 공격을 실제로 시작할 때 입력 facingLeft를 캡처한다. 아직 사격을 시작하지 못한 FIFO 예약 접수 시에는 방향을 덮어쓰지 않는다.
* 유지: IsShotFacingLocked는 shotPhase가 Prepare/Fire/Recover이거나, phase=None이어도 1~4발 발사 후 0.4초 유예가 남으면 true다. 별도 잠금 bool을 저장하지 않고 기존 상태에서 계산한다.
* UpdateInput은 이동 의도 directionX/Y와 running을 기존처럼 갱신하지만 고정 중 facingLeft는 변경하지 않는다. 이동에 의한 기존 Recover→None 전환은 유지한다. TryQueueAction은 같은 지상/공중 연속 구간의 후속 입력 방향을 무시하고 처음 방향을 유지한다.
* UpdateShots는 그 facingLeft로 총구 X, 지상 몸→총구 타격 대상의 앞뒤 판정, 투사체 수평 방향 및 공중 수평 반동을 계산한다. 공중 탄환은 X=±0.70710678, 높이 방향=-0.70710678로 45도 아래를 유지한다. 위 반동 airFireLift=60, 뒤 이동 airRecoilDistance=8은 그대로다.
* 5발 후 회수 완료, 기존 이동에 의한 5발 후 회수 취소 또는 1~4발 뒤 입력 유예 만료에서는 방향 고정이 끝난다. 기존 5발 제한과 한 점프 공중 5발 제한은 유지한다. 5발 후 회수 이동 취소로 phase=None이 되어도 기존 사격 수락 한도/유예 상태는 유지하며 방향 고정만 해제된다.
* 새 점프 시작, 착지로 인한 공중 공격 취소, 피격/사망, 맵 전환, Leave/RemoveUnannouncedPlayer, Stop/CompleteDungeon에서 ResetShotState로 기본 사격 상태를 정리한다. 착지/피격 판정은 phase=None인 유예에서도 실행한다. 착지 시 per-jump airShotCount 초기화는 기존 UpdateCombat 경로를 유지한다.
* ResetShotState는 사격 단계·발사 대기·연속 발수·사격 시간·유예·airAttack만 정리한다. FIFO와 공중 누적 발수는 호출 이유에 맞춰 기존 생명주기에서 관리한다. 피격·맵 전환·퇴장·룸 종료는 FIFO도 제거하고, 착지에서 실행할 수 있는 예약된 새 지상 입력은 유지한다.
* UpdateCombat은 고정 해제 후 살아 있고 피격/스킬 중이 아닌 플레이어에게 1초 이내의 유효한 유지 이동 방향을 반영한다. 새 이동 패킷이 올 때까지 반대 방향 이동과 이전 바라보기가 계속 남지 않게 한다.
* 스킬: 기존 회수/유예 중 스킬 전환 수락 규칙은 유지한다. 기본 공격 고정 중에는 SubmitSkill이 그 방향을 ActiveSkill.facingLeft로 이어받는다. 독립 스킬은 요청 방향을 캡처한다. 기존 직접 타격·스킬 투사체는 cast.facingLeft를 사용하고, 스킬 종료/피격 취소는 기존 UpdateSkills 생명주기로 해제한다. 현재 스킬 실행 경로에는 별도 사격 반동 로직이 없다.

방향 고정은 이동 금지 조건으로 사용하지 않았다. UpdatePlayers의 이동 허용 검사는 그대로이며 기존 이동/공격 순서와 최대 발수 정책을 유지한다. 상태 변경은 입력 dispatch 및 틱·생명주기 모두 기존 GameRoom strand에 있다.

## 요구별 근거 파일·함수와 변경 파일

절대 소스 디렉터리: C:/Users/KimHyeongJin/source/repos/ActionRPGServer/ActionRPGServer/GameRoomServer

| 요구 | 파일·함수 |
|---|---|
| 실제 점프 데이터 2/3 | Data/Combat.json 변경; CombatDefinition.cpp::Load, GameRoom.cpp의 combatRules 생성, GameRoomCombat.cpp::UpdateCombat/UpdateActor 읽기 검토 |
| 입력 및 후속 사격 방향 고정 | GameRoom.h 선언; GameRoomCombat.cpp::IsShotFacingLocked/TryQueueAction; GameRoom.cpp::UpdateInput |
| 실제 사격·근접·공중 반동 방향 | GameRoomCombat.cpp::UpdateShots의 facingLeft 소비 경로 확인 |
| 공격 구간 종료 및 상태 초기화 | GameRoomCombat.cpp::ResetShotState/UpdateShots/UpdateCombat/UpdateMonster; GameRoom.cpp::UpdatePlayers/Leave/RemoveUnannouncedPlayer/Stop/CompleteDungeon |
| 스킬 방향 | GameRoomSkills.cpp::SubmitSkill 수정; UpdateSkills의 cast.facingLeft 소비 및 종료/취소 확인 |
| 문서·연계 | COMBAT_PROTOCOL.md 수정; 이 SERVER_REPORT.md 추가 |

이번 코드·데이터 수정 파일은 Data/Combat.json, GameRoom.h, GameRoom.cpp, GameRoomCombat.cpp, GameRoomSkills.cpp다. CombatDefinition.cpp 및 GameRoomRealtime.cpp는 읽기 검토만 했다. 별도 패킷 스키마나 생성물을 변경하지 않았다.

## 클라이언트 확정 연계

* 던전: 서버 월드 combatRules.jumpSpeed=567.62를 그대로 사용한다. 추가로 2/3를 곱하지 않는다. 클라이언트 읽기 경로는 C:/Users/KimHyeongJin/source/repos/ActionRPGClient/ActionRPGClient/ActionRPGClient/Game/DungeonCombat.cpp의 CombatRules::Parse다.
* 마을: 검토 시 초기 속도가 Character.h의 JUMP_SPEED=1035였으며 기존 characters.ini는 애니메이션만 정의했다. 총괄의 추가 인계에 따라 클라이언트 담당이 캐릭터별 점프 데이터 로딩 경로를 만들고 각 기존 값에 2/3를 적용한다. 현재 1035 기준 새 값은 690이며 마을 중력 1800은 유지한다. 서버 담당은 클라이언트 파일을 수정하지 않았다.
* 기존 facingLeft JSON 및 realtime actor 필드에 고정 방향이 전달된다. 신규 잠금 플래그, 상태 필드, 패킷 번호 또는 버전은 추가하지 않았다. 회수·유예 중 위치 이동을 허용하는 클라이언트 코드가 facingLeft까지 이동 방향으로 덮어쓰지 않아야 한다.
* 로컬 동작에서도 1~4발 뒤 0.4초 유예와 최대 5발을 같은 공격 구간으로 처리한다. 새 공격 시작 시 방향 캡처, 같은 구간 재입력 무시, 착지·피격·새 점프·맵/세션 초기화 및 유예 종료 때 해제를 맞춘다.
* 기본 공격 고정 중 스킬로 전환할 때도 시작 방향을 이어받는 규칙을 맞춘다. 독립 스킬은 자기 요청 방향을 사용한다. 캐릭터 좌우 반전, 총구, 45도 아래 투사체, 위/뒤 반동이 같은 방향을 사용해야 한다.
* 기존 스냅샷에는 유예 남은 시간을 별도 필드로 제공하지 않는다. 클라이언트는 기존 타이밍과 권한 있는 facingLeft/shotCount/shotPhase를 사용한다. 실제 표시·보간·원격 캐릭터 방향 일치는 클라이언트 담당의 확인 범위다.

## 정적 확인 및 제한

JSON 파싱과 데이터값 851.43×2/3=567.62를 확인했다. 선언/정의, 플레이어 facingLeft 변경 지점 전체, 투사체·근접·반동 소비 지점, 스킬 캡처, 이동 허용 검사 보존 및 취소·초기화 호출을 대조했다. 대상 소스 git diff --check 통과.

빌드, 기능/통합 테스트, 게임/서버 실행, 프로세스 종료, 백업 생성, stage/commit/push 및 다른 담당 채팅으로의 메시지 발송은 하지 않았다. 실제 jump apex, 방향 전환 입력·5발·착지·피격·맵 전환·스킬 전환 및 네트워크 표시 결과는 실행 미확인이다. 기존 파티/던전 제한 감사 기록을 유지했으며 이번 작업으로 해당 코드를 바꾸지 않았다.

## 제안 커밋 로그

* 점프 초기 속도 감소와 연속 공격 방향 고정 보완
  * 점프 데이터를 851.43에서 567.62로 조정하고 중력 유지
  * 연속 사격 유예 중 이동·후속 입력의 방향 덮어쓰기 차단
  * 공중 사격·반동 및 스킬 전환에 고정 방향 적용
  * 공격 취소·초기화와 고정 해제 후 이동 방향 반영
  * 전투 계약과 클라이언트 연계 사항 기록

제안 로그이며 실제 커밋 및 푸시는 수행하지 않았다.
