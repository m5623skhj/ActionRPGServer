# 몬스터 기본 전투 통합 데이터

이번 범위는 FallenCitadel 던전에 등록된 일반 몬스터 Data ID 2와 보스 Data ID 3의 최소 추적·근접 공격 패턴입니다.
새 이미지, 특수 기믹, 드랍, 런타임 변경은 포함하지 않습니다.

## 수정·통합 대상

- `ActionRPGServer/GameRoomServer/Data/Monsters/rusted_armor_soldier.ai.json`
- `ActionRPGServer/GameRoomServer/Data/Monsters/fallen_citadel_warden.ai.json`
- `output/combat-integration/monster-combat-effects.json`: 서버 담당에게 전달할 프로필·스킬 효과.

효과 파일의 `monsters` 항목을 동일한 `dataId`의 `Data/Combat.json` 프로필에 반영해야 합니다.
효과 파일은 ID 2·3만 포함한 통합 자료이며 실행 시 자동으로 로드되는 설정이나 Combat.json 전체 대체 파일이 아닙니다.
Dummy ID 1과 player/reactions 설정은 서버 담당이 유지합니다. 이 작업에서는 Combat.json을 수정하지 않았습니다.

**AI 정의만 적용하고 스킬 효과를 통합하지 않으면 CombatDefinition의 UseSkill 필수 효과 검사가 서버 시작을 거부합니다.**
서버 담당은 AI 정의와 효과를 함께 설치하고 ID·스킬 참조를 확인해야 합니다.

## 패턴과 실행 근거

두 몬스터는 동일한 최소 구조를 사용합니다.

1. Idle: 0.5초 Wait를 반복하며 OnUpdate의 HasTarget으로 감지된 대상을 추적합니다.
2. Chase: 걷기 속도로 공격 최대 거리보다 10단위 가까이 접근합니다. 대상 소실 시 Return으로 이동합니다.
3. Ready: 0.1초 Wait 반복 중 거리·쿨다운을 재평가합니다. 대상 이탈 시 Chase, 소실 시 Return입니다.
4. Attack: 준비 0~3 → 타격 4~5 → 회수 6~7의 기존 8프레임 전체를 UseSkill로 실행합니다.
5. Recover: 일반 0.3초·보스 0.4초 추가 대기 후 대상이 있으면 Chase, 없으면 Return입니다.
6. Return: 생성 위치의 10단위 안으로 귀환하면 Idle, 귀환 중 새 대상이 감지되면 Chase입니다.

현재 실행기의 SkillReady는 쿨다운만 검사하므로 ReadyAttack에 TargetInRange AND SkillReady를 명시합니다.
추적 행동은 완료 후 자동 재시작되지 않으므로 Ready에서 대상의 거리 변화와 쿨다운을 확인하고 추적을 재개합니다.
공격 상태에는 대상 이탈 전환을 넣지 않았습니다. 시작된 공격은 준비·회수까지 진행하고,
서버가 타격 시 잠근 대상의 생존·같은 방·거리·높이·앞쪽 여부를 다시 검사하여 회피를 허용합니다.
피격·사망은 공통 전투 상태가 공격을 중단하며 이 그래프에 별도 노드를 중복하지 않습니다.

모든 반복에는 OnUpdate 또는 실제 Wait/UseSkill 시간이 있으며 Immediate 연결은 없습니다.
각 그래프는 6개 상태·13개 연결이고 상태별 우선순위는 중복되지 않습니다.

## 임시 수치

| 항목 | 녹슨 갑옷병 / ID 2 | 몰락한 성채 수호자 / ID 3 |
|---|---:|---:|
| 스킬 ID | RustedArmorSlash | CitadelWardenSlash |
| HP | 100 유지 | 100 유지 |
| 피해량 | 10 | 20 |
| 최소/최대 시작 거리 | 0 / 80 | 0 / 120 |
| 추적 정지 거리 | 70 | 110 |
| 공격 FPS / 프레임 수 | 14 / 8 | 10 / 8 |
| 전체 공격 시간 | 8/14 ≈ 0.571429초 | 8/10 = 0.8초 |
| 타격 시간 | 4/14 ≈ 0.285714초 | 4/10 = 0.4초 |
| 쿨다운 | 1.2초 | 1.8초 |
| 타격 허용 높이 | 80 | 100 |
| 피격 유형 | Normal | Normal |

쿨다운은 스킬 시작부터 계산됩니다. Recover가 끝나도 남은 쿨다운은 Ready에서 기다립니다.
두 공격 모두 기존 단일 대상 근접 타격만 사용합니다. 보스의 Airborne·광역·특수 패턴은 추가하지 않았습니다.
HP는 기존 정의를 유지했으며 보스 전용 체력 밸런스를 확정한 것이 아닙니다.
피해량은 현재 플레이어 HP 100을 기준으로 비교 가능한 기본 공격을 제공하기 위한 임시값입니다.
사거리·타격 높이는 월드 단위의 통합용 값이며 이미지 검 길이·게임 접지점에 대한 최종 승인값이 아닙니다.

속도/감지/충돌 프로필은 기존 Combat.json의 값을 그대로 복사했습니다.
일반 walk/run 140/220, detectionRange 600, bodyHeight 96, hitRadius 24;
보스 walk/run 110/180, detectionRange 800, bodyHeight 158, hitRadius 36입니다.
걷기를 사용하므로 runSpeed는 이번 패턴에서 사용하지 않습니다.

## 클라이언트 애니메이션 문자열 계약

두 스킬의 `animationId`는 `attack`입니다. 이는 전역 INI 섹션이나 PNG 경로가 아니라
클라이언트에서 `dataId → monsterId → animations.json.characters[monsterId].motions.attack`으로
선택하는 로컬 모션 키입니다. 현재 MonsterCatalog/Monster.cpp도 이 키를 MonsterMotion::Attack으로 읽습니다.
서버 상태 적용 담당은 UseSkill의 attack 문자열을 해당 개체의 MonsterMotion::Attack에 연결해야 합니다.

사용 에셋은 `ActionRPGClient/Assets/Images/Monsters/animations.json`과
`rusted_armor_soldier_attack.png` / `fallen_citadel_warden_attack.png`입니다.
메타데이터의 실제 FPS·8프레임·suggestedImpactFrame=4를 기준으로 durationSeconds와 hitSeconds를 계산했습니다.
프레임은 등분하지 않고 기존 sourceRect/pivot/scaleToMovement를 그대로 사용합니다.
서버 20Hz 실행에서는 타격·완료가 임계 시간을 지난 업데이트에 발생하므로 시뮬레이션 틱 단위로 양자화됩니다.
네트워크 상태 전송과 실제 렌더링의 타이밍 일치는 클라이언트 통합 범위입니다.

## 정적 확인 결과와 제한

- 기존 MonsterEditor/model.js의 validateDocument로 두 정의를 정적 검사: 각각 오류 0, 경고 0.
- JSON 파싱, 등록 ID·monsterId·정의 파일·스킬 effect 참조, 이동 속도와 프로필 수치 제한을 확인했습니다.
- 상태 도달성·중복 우선순위·무시간 순환은 기존 정적 검사기로 확인했습니다.
- 공격 PNG 존재, 메타데이터 FPS·프레임 수·4번 프레임의 타격 시간, 전체 재생 시간과 사거리 조건을 대조했습니다.
- approval.status는 draft를 유지했습니다. 정적 확인을 게임 실행 승인이나 플레이 검증으로 표시하지 않았습니다.
- 빌드·서버/클라이언트 실행·던전 입장·전투 테스트·커밋·푸시는 수행하지 않았습니다.

Combat.json 효과 통합과 클라이언트 상태·애니메이션 연결은 관련 담당의 작업입니다.
장애물 경로 탐색, 실제 전투 밸런스, 보스 특수 패턴, 드랍은 이번 결과로 검증하거나 구현한 것으로 간주하지 않습니다.
방 strand에서 개체 상태를 변경하는 계약은 기존 서버 실행기가 담당하며 이 작업은 불변 정의 데이터만 변경합니다.
