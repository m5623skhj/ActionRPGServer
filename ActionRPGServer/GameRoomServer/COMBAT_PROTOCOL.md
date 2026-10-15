# 서버 전투 실행과 클라이언트 연결 계약

플레이어 스킬 편집기의 신규 직접 공격·투사체·버프 실행과 SKL1 확장 계약은 [PLAYER_SKILLS.md](PLAYER_SKILLS.md)를 참고합니다.

## 구현 범위와 현재 콘텐츠

- 방 strand의 목표 30Hz 업데이트에서 플레이어 사격·점프·슬라이딩, 탄환, 몬스터 AI, HP·피격·사망을 처리합니다.
- 클라이언트가 보낸 위치·피해량·피격 대상은 사용하지 않습니다. 인증된 세션의 플레이어 ID로 입력을 처리합니다.
- MonsterEditor schemaVersion 1은 유지합니다. AI 정의의 스킬·동작 참조를 서버 전용 `Data/Combat.json`과 함께 검사합니다.
- Dummy는 대기 정의를 유지합니다. 녹슨 갑옷병과 수호자는 감지·추적·공격 대기·공격·회복·귀환의 최소 패턴을 정의했습니다.
  일반 RustedArmorSlash는 14FPS의 4번 프레임(4/14초)에 피해 10,
  보스 CitadelWardenSlash는 10FPS의 4번 프레임(0.4초)에 피해 20을 적용합니다.
  HP와 피해·속도 수치는 통합용 임시값이며 상세 근거는 `output/combat-integration/README.md`에 있습니다.
- 보스방에 배치된 몬스터가 모두 죽으면 한 번만 기존 RoomManager 완료 흐름으로 연결합니다.
  보스방에 몬스터가 없으면 자동 완료하지 않습니다.
- 현재 방에 살아 있는 몬스터가 있으면 모든 출구 워프를 차단합니다. 모두 처치하면 워프를 허용하며,
  처음부터 몬스터가 없는 방은 바로 이동할 수 있습니다. 방 안에서의 이동은 기존대로 유지합니다.
  몬스터는 던전 인스턴스 생성 시 한 번만 만들며, 같은 진행 중 방을 다시 방문해도 처치 상태를 유지합니다.
  새로운 던전 인스턴스를 만들면 해당 인스턴스의 몬스터를 새로 생성합니다.
- 퀘스트 제출·보상과 영속 인벤토리는 Town이 처리합니다. 전리품 드랍, 부활·전멸 종료, 장애물 경로 탐색은 포함되지 않습니다.
  기존 TownServer는 완료 통지를 받아 보상 대상만 기록합니다.
- 빌드·게임 실행·기능 테스트는 수행하지 않았습니다. 정적 검토만으로 실제 플레이 동작을 보장하지 않습니다.

## 몬스터 추적 분산

몬스터마다 방 seed와 instanceId에서 고정 접근 방향을 정하고, 걷기/달리기 속도와 공격 쿨타임에
0.9~1.1 배율을 적용합니다. 추적 종료 지점은 기존 정지 거리의 0.8~0.95배 반경 위에 분산합니다.
공격 시작 전 0.04~0.20초 대기는 행동 sequence에 따라 달라지며, 대기 중 대상이 사거리 밖으로
나가면 공격을 취소합니다. 공격이 시작된 뒤의 타격 시점과 동작 길이는 기존 정의를 유지합니다.
접근 지점이 벽에 막히면 기존 직접 추적 경로를 사용하며, 별도 경로 탐색은 추가하지 않습니다.

같은 맵에서 가까운 몬스터는 발 위치 기준으로 조금씩 벌어집니다. 맵 이동 가능 영역을 지키고,
개체당 한 tick 이동량을 걷기 속도의 30% 시간분과 최대 8 월드 단위로 제한합니다.
공격·피격·역경직·사망 중인 개체와 고정 Dummy는 밀려나지 않습니다. 좁은 지형에서는 겹침이
남을 수 있습니다. 맵당 최대 256개에서 쌍 비교는 O(n²)이며 먼 쌍은 거리 제곱으로 제외합니다.
방 strand의 인스턴스 상태만 수정하고 공유 정의·전역 난수 상태는 변경하지 않습니다.

갑옷병과 수호자의 공격자 역경직은 기존 `hitstopSeconds=0.05`와 `ApplyDamage` 경로가 이미
적용되어 있어 유지합니다. 클라이언트도 해당 시퀀스·시각으로 동작과 공격 궤적을 멈춥니다.

## 서버 전투 설정

서버 시작 시 실행 파일 옆 `Data/Combat.json`을 읽습니다. 프로젝트의 Data 복사 대상에 포함됩니다.
version 3, 최대 1MB이며 모든 등록 몬스터에 정확히 한 개의 프로필이 있어야 합니다.
`player.characters`에는 PlayerSkills 캐릭터 목록의 모든 캐릭터를 중복 없이 등록합니다.
각 항목은 `characterId`, `attackPower`(1~1000000), `slide.durationSeconds`(0.05~2초),
`slide.distancePerRunSpeedSeconds`(0.05~5초), `slide.motionId`를 포함합니다.
현재 캐릭터 1~3은 공격력 20, 지속시간 0.4초, 거리 계수 1.25초, 모션 `slide`입니다.
없는 몬스터·스킬 참조, 중복 프로필/스킬, 누락된 필드, 비정상 수치, 이동 AI에 속도 0,
UseSkill 행동에 타격 정의 누락을 거부합니다. 오류가 있으면 서버 시작을 중단합니다.
AI 실행 수치는 서버의 float 범위 안이어야 하며, 범위를 넘는 값은 몬스터 정의 로딩 때 거부합니다.

현재 파일의 플레이어 HP 100·탄환 피해 20·사격/점프 타이밍·몬스터 속도와 충돌 크기는
서버 실행 계약을 구성하기 위한 **임시 수치**입니다. 최종 밸런스나 이미지 크기 승인이 아닙니다.
몬스터 최대 HP는 기존 `.ai.json`에서 읽습니다. 움직임 속도는 각 몬스터 프로필에 귀속됩니다.

`player`는 HP·걷기/달리기 속도·몸 높이·피격 반경, `shot`은 피해량·준비/발사 간격/회수 시간,
탄환 속도/사거리/반경·지상/공중 총구 위치·공중 반동, `jump`는 준비 시간·초기 수직 속도·중력을 정의합니다.
`reactions`는 경직·쓰러짐·기상 시간을 정의합니다. 서버 좌표는 기존 방 데이터와 같은 월드 단위입니다.
초기 월드 JSON에 `combatRules`를 추가하여 클라이언트가 서버 타이밍을 읽을 수 있도록 했습니다.
`combatRules.version=3`이며 `slideDefinitions` 배열은 캐릭터별
`characterId`, `attackPower`, `durationSeconds`, `distancePerRunSpeedSeconds`, `hitStartRatio`, `hitEndRatio`, `motionId`, `hitRecovery`, `hitstopSeconds`를 전달합니다.
`shotHitstopSeconds`도 초기 월드에 포함합니다. 일반 경직의 기본 `hitStunSeconds`는 0.25초입니다.
기본 사격의 `shot.damage`와 플레이어 스킬의 피해량·랭크 계산은 캐릭터 `attackPower`로 대체하지 않습니다.

기본 사격의 `muzzleForward`/`muzzleHeight`는 지상, `airMuzzleForward`/`airMuzzleHeight`는 공중 총구의
전방 거리와 발 기준 높이입니다. 모두 0~1000 월드 단위이며 피격용 `bodyHeight`와 독립적으로 검사합니다.
왼쪽을 바라볼 때 전방 X만 반전하고, 점프 높이는 총구 높이에 더합니다. 총구까지의 지면 구간이
막혀 있으면 탄환을 생성하지 않되 해당 발사 입력과 반동은 처리합니다. 생성된 실제 좌표는 기존
투사체 상태로 전달하므로 클라이언트가 추가 표시 오프셋을 적용하지 않습니다.

현재 256×256 발사 프레임의 pivot (128,244), 표시 크기 202×202를 기준으로 지상 총구 픽셀
(225,69)는 (76.5390625,138.0859375), 공중 (186,135)는 (45.765625,86.0078125)로 환산했습니다.
클라이언트 `projectiles.ini`의 `PlayerBullet`/`PlayerAirBullet` 발생 위치도 같은 값입니다.
이미지·pivot·표시 크기가 바뀌면 양쪽 정의를 함께 재계산해야 합니다. 몬스터 피격 높이는 현재
클라이언트 `monsters.json`의 표시 높이에 맞춰 갑옷병(ID 2) 200, 수호자(ID 3) 330입니다.
Dummy(ID 1)는 현재 표시 높이와 같은 96을 유지합니다. 낮은 대상은 수평 탄환보다 아래에 있으면 맞지 않습니다.
수정된 서버 소스와 추가 총구 필드가 있는 `Combat.json`을 함께 반영해야 합니다.

지상 기본 사격은 발사 시 몸의 앞쪽부터 총구까지의 구간도 같은 탄환 높이·반경으로 검사합니다.
해당 구간의 가장 먼저 겹치는 생존 몬스터 한 마리를 타격하면 탄환을 소모하며,
뒤쪽에 중심이 있는 몬스터는 이 근접 검사에서 제외합니다. 기존 총구까지의 벽 검사를 먼저 통과해야 합니다.
공중 기본 사격은 기존 총구 위치에서 45도 아래로 진행합니다. 근접 검사와 이동 중 탄환 검사는
동일한 원통 교차 계산을 사용하며, HP·피격 상태 변경은 기존 방 strand 안에서 처리합니다.

### 점프 높이와 입력 유예

2026-10-04 승인에 따라 던전 점프 초기 속도를 `Combat.json`의 851.43에서 567.62(정확히 2/3)로 낮춥니다.
중력 1200과 준비 시간 0.1초는 유지합니다. 같은 중력에서 연속 시간 계산상 높이 비율은 4/9(약 44%)이며,
높이를 2/3로 맞추는 보정은 적용하지 않습니다. 기존 30Hz 적분에서는 반동 없는 최고 높이가 약 288에서
약 125로 낮아지는 수식상 추정이며 실행 측정은 하지 않았습니다. 서버는 현재 공통 player 점프 데이터를 사용합니다.
마을 로컬 점프는 클라이언트 담당이 기존 1035를 690으로 조정하고 캐릭터별 데이터 로딩에 반영해야 합니다.
던전 클라이언트는 월드 `combatRules.jumpSpeed`의 567.62를 그대로 읽으며 추가로 2/3를 곱하지 않습니다.
점프 공격의 상승 반동은 이 기본 높이에 추가됩니다. 에어본 피격의 서버 상승 속도는 별도 상수 420을 유지합니다.

연속 사격은 마지막 발사 후 0.4초까지 입력을 이어갈 수 있습니다. 유예 중에도 기존 회수 시간이 끝나면
사격 상태는 None이 되어 이동할 수 있으며, 늦은 후속 입력은 준비 동작을 반복하지 않고 Fire로 이어집니다.
기존 발사 간격·최대 5발 및 한 번의 점프에서 공중 최대 5발 제한은 유지합니다.
사격 준비·발사·회수와 아직 5발에 도달하지 않은 연속 입력 유예 구간에서는 최초 사격 방향을 유지합니다.
후속 사격 입력과 이동 입력의 방향은 이 고정을 바꾸지 않습니다. 회수·유예 중 이동 허용은 그대로입니다.
5발 후 회수 종료/기존 이동에 의한 회수 취소, 유예 만료, 새 점프, 착지에 따른 공중 공격 취소, 피격·사망,
맵 전환·퇴장·룸 종료 때 해당 고정이 해제됩니다. 고정 해제 후 유효한 유지 이동 입력의 수평 방향을 반영합니다.
기존 허용된 회수 중 스킬 전환은 사격 방향이 고정돼 있으면 그 방향을 이어받습니다. 독립 스킬은 요청 방향으로
시작하며 ActiveSkill의 방향은 시전 동안 유지합니다. 캐릭터 표시·총구·지상 근접 판정·공중 탄환·수평 반동은
같은 방향을 사용하며 기존 facingLeft 상태 필드로 전달합니다. 새 방향 잠금 필드나 패킷은 추가하지 않습니다.
기본 공격·점프 입력은 즉시 실행할 수 없으면 최대 0.25초 동안 순서대로 예약합니다.
예약은 최대 6개이고 사격 예약도 5발 한도에 포함합니다. 만료·피격·사망·맵 전환 때 제거하며,
예약 접수 accepted=1은 만료 전에 실행이 가능해진 경우에만 실행한다는 의미입니다.
예약과 유예 상태는 방 strand에서만 변경하고 기존 상태 패킷 구조는 유지합니다.
클라이언트의 마을 로컬 동작에도 같은 입력 유예·예약을 적용하며, 달리기 더블 탭 간격은 0.4초,
기존 skills.ini 방향키 스킬의 키 사이 간격은 0.55초입니다. 던전 스킬 계약은 [PLAYER_SKILLS.md](PLAYER_SKILLS.md)를 따릅니다.

### 던전 전투 템포

`Combat.json` 루트와 초기 월드 `combatRules.tempoScale`은 현재 0.7이며, 누락 시 1을 사용합니다(허용 0.1~1).
수치는 전투 데이터의 속도·피해·HP·지속시간을 다시 작성하는 대신 전투 시계에 한 번 적용합니다.
30Hz 룸 tick/송신 주기, `serverTick`, `serverTimeMs`, 네트워크 보간, heartbeat, 입력 유효기간·요청 제한·인증 timeout과
마을 이동은 실제 시간 기준으로 유지합니다. Room과 Town에는 같은 Combat 원본과 새 서버/클라이언트를 함께 배포해야 합니다.

동작 시간은 `(실제 tick 시간 - 실제 역경직으로 멈춘 시간) × tempoScale`입니다. 플레이어·몬스터 이동,
점프·중력·피격·사격·스킬·아이템 투척 동작·슬라이딩·AI 전투 행동·예약/연타 창은 이 시간을 사용합니다.
월드 전투 시간은 `실제 tick 시간 × tempoScale`로, 이미 발사한 투사체와 스킬/몬스터 쿨다운·버프가 소비합니다.
이 월드 타이머는 기존대로 개체의 역경직 중에도 진행하며, 동작 시간을 다시 감속하지 않습니다.

| 전달 값 | 단위와 클라이언트 처리 |
|---|---|
| shot/jump/skill/slide/itemUse/action 경과·지속시간, reaction 남은/전체 시간, projectile.ageSeconds | 전투 초. 모션 데이터도 전투 초이며 외삽의 실제 age에 tempoScale을 한 번 곱함 |
| verticalSpeed/slideSpeed/projectileSpeed, combatRules의 이동·중력·기본 지속시간 | 원본 전투 단위. 속도나 데이터 값을 다시 줄이지 않고 전투 dt로 진행 |
| hitstopStartTimeMs/remaining/duration | 실제 시각과 실제 초. 시작 시 원본 hitstopSeconds/tempoScale로 한 번 변환; 예측에서 재감속하지 않음 |
| JSON skillCooldowns, JSON/SKL1 buffs.remainingSeconds | 실제 남은 초. 서버 내부 전투 초를 tempoScale로 나눠 전송하고 UI는 실제 초로 감소 |
| 아이템 cooldownServerTimeMs/ready/remaining | 기존 DB UTC 실제 밀리초. 추가 감속/역경직 차감 없음 |

현재 달리기 480은 실제 초당 336, 걷기 280은 196으로 진행합니다. 슬라이딩 거리 600은 유지하고
전투 지속시간 0.4초가 실제 약 0.5714초, 전투 속도 1500이 실제 1050이 됩니다. 투사체 사거리와 이동 버프의
상대 거리를 바꾸지 않습니다. jumpSpeed/gravity 및 연속 모델의 점프 높이는 유지합니다. 기존 semi-implicit
적분을 유지하므로 작은 이산 최고 높이 차이가 남으며 실행으로 검증하지 않았습니다.
쿨다운/버프 데이터의 의미는 전투 초를 유지합니다. UI 검증 상한도 기존 전투 초 상한/tempoScale로 변환해야 합니다.
version 3과 binary 필드 순서는 유지하며, tempoScale=0.7 서버의 예측에는 새 단위를 처리하는 클라이언트가 필요합니다.

### 슬라이딩

지상에서 달리기 이동 의도와 action=3 입력을 함께 보내면 즉시 시작합니다. 이동 의도는 수평·수직·대각선을
허용하되 실제 슬라이딩은 왼쪽 또는 오른쪽 직선으로만 진행합니다. 같은 프레임의 `directionX`가 0이 아니면
그 부호를 사용하고, 상하 입력만 있으면 시작 직전 서버 `facingLeft`를 사용합니다. `slideDirectionX`는 -1 또는 1,
`slideDirectionY`는 0이며 시작 방향에 맞춰 `facingLeft`도 확정합니다. 요청의 `facingLeft` 값은 방향 결정에 사용하지 않습니다.
공중·점프 준비·피격·사망·스킬 시전·사격 준비/발사 중과 미처리 행동 예약이
있을 때는 거절합니다. 이동이 허용되는 사격 회수 구간과 연속 입력 유예 중에는 시작할 수 있으며 기존 사격을 종료합니다.
슬라이딩 중 사격·점프·스킬·다음 슬라이딩은 거절하며 예약하지 않습니다.

시작 시 방향과 실제 달리기 속도(`runSpeed × 현재 가장 높은 movementMultiplier`)를 읽습니다.
장애물이 없을 때 총 이동 거리는 `실제 달리기 속도 × distancePerRunSpeedSeconds`이며,
전투 시계 기준 슬라이딩 속도는 `총 이동 거리 / durationSeconds`로 확정합니다. 거리 계수의 단위는 초로,
좌표/초인 달리기 속도에 곱해 좌표 거리를 얻습니다. 현재 기본 달리기 속도 480과 계수 1.25는 총거리 600,
지속시간 0.4초는 슬라이딩 속도 1500을 뜻합니다. 기존 계수 2.5의 거리 1200·속도 3000에서 각각 절반으로 줄었으며
속도에 별도 감소 계수를 다시 곱하지 않습니다. 이동 입력이 이후 바뀌거나 버프가 만료되어도
진행 중인 슬라이딩의 방향·속도를 바꾸지 않습니다. 벽·맵 경계·출구 워프 영역 앞에서 멈추며 슬라이딩 중 맵을 전환하지 않습니다.
피격·사망·클리어·퇴장·룸 종료 때 즉시 취소합니다. 종료 뒤에는 최신 유지 이동 입력을 일반 이동에 적용합니다.

접촉 피해는 시작 시 `attackPower × 현재 가장 높은 damageMultiplier × 1.0`으로 확정하고
양의 uint32 범위로 제한한 뒤 소수 부분을 버립니다. 투사체 없이 공격 유효 시간에 해당하는 이동 경로의 접촉과 높이·벽 가림을 검사하여
슬라이딩 한 번당 각 몬스터를 한 번만 타격합니다. 타격 시 피해 버프를 다시 곱하지 않습니다.
`player.characters[].slide`와 월드 `combatRules.slideDefinitions[]`의 필수 필드
`hitStartRatio`, `hitEndRatio`는 유한한 수이며 `0 <= hitStartRatio < hitEndRatio <= 1`이어야 합니다.
유효 시간은 `[durationSeconds × hitStartRatio, durationSeconds × hitEndRatio)`입니다.
현재 모든 캐릭터는 `0.225`, `0.5`로 동작 시간 `[0.09, 0.20)`초, 8프레임 모션의 0-based 2~3번에 해당합니다.
준비·마무리에는 피해가 없으며 시작 패킷 처리 중 즉시 접촉 피해를 주지 않습니다.
벽·게이트는 기존 4단위 이동 단계마다 검사합니다. 몬스터 접촉 검사는 틱당 실제로 허용된 이동 시간과
공격 유효 시간의 교집합으로 자른 경로에 한 번만 수행합니다. 큰 틱이 양쪽 경계를 넘더라도 준비/마무리
경로를 포함하지 않고, 끝 경계에서 처음 접촉한 대상은 제외합니다. 시작 경계의 정지 접촉은 포함합니다.
벽에 막힌 틱도 실제로 허용된 경로만 검사한 뒤 종료하며, 준비 중 벽에 막히면 공격 구간까지 진행한 것으로
취급하지 않습니다. 첫 이동 단계가 막혀도 이미 공격 유효 시간 안에 있던 위치의 접촉은 검사합니다.
슬라이딩 거리·속도 계산은 행동 시간 기준이므로 역경직은 이동과 진행 시간을 함께 멈추며 총 목표 거리를 줄이지 않습니다.
서버 데이터 상한(달리기 2000, 이동 버프 10, 거리 계수 5, 최소 지속시간 0.05)에 따른 슬라이딩 속도 상한은
2000000좌표/초입니다. 30Hz에서 틱당 최대 약 16667개의 4단위 이동 검사가 필요하며 int 변환 범위 안입니다.
기본값은 tempoScale=1에서 틱당 약 13회, 현재 0.7에서는 약 9회이며 분할 수를 줄여 충돌 검사를 건너뛰지 않습니다.
최대값 조합의 성능은 실행 검증하지 않았습니다.
클라이언트 예측도 같은 거리 공식과 검증 상한을 사용하고 서버 `slideSpeed`를 실제 좌표/초로 해석해야 합니다.
기존 combatRules/version 3과 슬라이딩 상태 패킷 필드·구조는 유지하며 신규 필드가 포함된 서버 데이터를 함께 배포합니다.
클라이언트는 `slideSequence`로 시작을 구분하고 서버 진행 시간과 `motionId`를 사용하여 표시합니다.
프레임·이미지 메타데이터는 클라이언트가 관리하며 서버 저장소에 생성하지 않습니다.

### 히트 리커버리와 공격자 역경직

`player.characters[].hitRecovery`, `monsters[].hitRecovery`는 모든 캐릭터에 적용하며 누락 기본값은 0입니다.
유한한 비음수 float 범위만 허용합니다. 일반 Hit 전체 시간은
`hitStunSeconds / (1 + hitRecovery / 100)`으로 계산합니다. 일반 Hit 중 재피격은 계산된 전체 시간으로
다시 설정합니다. Falling 물리·Down·Rising 시간 및 다운/기상 중 일반 재피격의 기존 의미는 유지합니다.

공격 데이터의 `hitstopSeconds`는 공격자만 멈추는 시간입니다. 기본 사격은 `player.shot.hitstopSeconds=0`,
캐릭터별 슬라이딩은 `player.characters[].slide.hitstopSeconds=0.18`(tempoScale=0.7에서 실제 약 0.257초), 몬스터 근접 공격은
`monsters[].skills[].hitstopSeconds=0.05`입니다. 스킬의 `execution.hitstopSeconds`는 [PLAYER_SKILLS.md](PLAYER_SKILLS.md)를 따릅니다.
유한한 비음수 float로 검증하고 임의의 시간 상한은 두지 않습니다. tempoScale로 나눈 실제 지속시간도
float 범위 안인지 로딩 때 검증합니다. 명시적 0은 정지를 비활성화합니다.
새 시간·능력치의 양수를 float로 변환했을 때 0이 되는 underflow도 거부합니다.

실제 피해가 적용된 살아 있는 대상의 명중에서만 공격자의 정지를 설정합니다. 이미 사망했거나 피격 반응 중인
공격자는 정지시키지 않습니다. 다중 명중은 `max(남은 시간, 새 시간)`이며 합산하지 않습니다.
직접 공격은 실제 공격자 actor를 전달하고, 플레이어 투사체는 발사 당시 시간을 보관한 뒤
같은 맵에 참여 중인 발사자를 player ID로 찾습니다. 몬스터 instance ID와 혼용하지 않습니다.

룸 틱 시작 때 모든 개체의 동작 시간을 한 번 계산합니다. 정지가 끝나는 틱에는 남은 동작 시간만 사용합니다.
이동·점프 물리·사격·스킬·슬라이딩·AI 행동과 연속 공격 유예/행동 예약 시간은 이 동작 시간을 사용합니다.
기존 룸 시간 기준의 쿨타임·버프 지속시간과 이미 발사한 투사체는 계속 진행합니다.
현재 동작은 재시작하지 않으며 일반 사격/점프 예약은 기존 제한 안에서 접수합니다. 정지 중 스킬/슬라이딩은 예약하지 않습니다.
슬라이딩 마지막 이동 틱에 명중하면 `slideSeconds == slideDurationSeconds`에서도 역경직이 풀릴 때까지
`slideActive`를 유지합니다. 클라이언트는 이 정지 구간에서 시간만으로 슬라이딩 모션을 조기 종료하지 않습니다.
피격·사망·맵 전환·퇴장·클리어·룸 종료는 정지보다 우선하며 해당 역경직을 취소합니다.

플레이어와 몬스터 JSON에는 `hitRecovery`, `reactionDurationSeconds`, `hitstopRemainingSeconds`,
`hitstopSequence`, `hitstopStartTimeMs`, `hitstopDurationSeconds`를 전달합니다.
`reactionDurationSeconds`는 Hit·Down·Rising의 전투 시계 전체 시간이고 None/Falling/Dead에서는 0입니다.
실제 초 단위 전체 시간은 이 값을 tempoScale로 나누며, 역경직 필드들은 이미 실제 초 단위입니다.
클라이언트는 공통 기본 시간 대신 이 전체 시간으로 반응 모션 비율을 계산합니다.

역경직 발생 번호는 양의 정지가 설정될 때 증가하며, 시작 시간은 `serverTimeMs`와 같은 steady_clock 밀리초입니다.
새 시작의 `hitstopDurationSeconds`는 max 처리된 잔여 시간입니다. 자연 종료 뒤에도 마지막 번호·시작·지속시간을 보존합니다.
취소는 발생 번호를 증가시키고 시작 시간을 갱신하며 잔여/지속시간을 0으로 기록합니다.
클라이언트는 최신 취소 번호를 우선하고 도착 시각부터 과거 정지 시간을 다시 시작하지 않습니다.
0.05초는 목표 15Hz 상태 주기보다 짧으므로 발생/종료/취소가 있는 틱에는 상태를 추가 전달하되
같은 틱의 다중 명중은 틱 끝에서 한 번으로 합칩니다. 같은 serverTick도 더 최신 snapshotSequence로 적용해야 합니다.
역경직 이력은 최신 이벤트를 보존하며 별도 전체 명중 로그는 아닙니다. 상태 손실 시 다음 전체 상태와 JSON 복구를 사용합니다.
실제 시간 분해능은 30Hz 틱이며 이번 작업은 빌드·실행·시각적 체감 검증을 수행하지 않았습니다.

### 피격 밀림과 Force

`force`는 지면 XY의 총 밀림 거리(world units)이며 화면 픽셀·질량·수직 상승 속도가 아니다.
유한한 0~500 float만 허용하고 누락은 0이다. float 변환 시 양수가 0이 되는 데이터는 거부한다.
기본 사격은 `Combat.json.player.shot`, 슬라이딩은 `player.characters[].slide`의 `force`와
`hitType`을 읽는다. 플레이어 스킬은 `PlayerSkills.json.execution`, 몬스터는 원본 `.ai.json.skills[]`의
같은 필드를 사용한다. 몬스터 Combat 효과에 중복 저장하지 않는다. `hitType`은 `Normal`/`Airborne`이며
사격·슬라이딩·플레이어 스킬의 누락 기본값은 `Normal`, 몬스터의 기존 필수 hitType은 유지한다.
기본 수치는 사격 24, 앞차기 60, 슬라이딩 40, 갑옷병 베기 24, 수호자 베기 48이다.
Stone은 기존 force=0/Normal을 유지하며 아이템 계약과 DB는 변경하지 않는다.

`reactions.knockbackSeconds`는 기본 밀림 시간이다. 누락 기본값 0.18, 유한한 0.01~1 전투 초를 허용하며
초기 월드 `combatRules.knockbackSeconds`로 전달한다. `k=1/(1+hitRecovery/100)`으로 계산하여
일반 경직은 `hitStunSeconds*k`, 밀림 거리와 시간은 각각 `force*k`, `knockbackSeconds*k`이다.
밀림 시간은 지상 피격 상태의 남은 시간도 넘지 않는다. 다운/기상 중 일반 재피격은 기존 반응 시간을
재설정하지 않으며 그 단계의 잔여 시간 안에서만 새 밀림을 적용한다. HR=100이면 현재 사격 거리는 12,
밀림 시간은 0.09 전투 초다. 회복계수 적용 뒤 거리나 시간이 0이면 밀림을 비활성화하여 0 나눗셈을 피한다.
피격 해제 시 모든 잔여 밀림을 삭제하여 일반 자세에서 계속 밀리지 않는다.

에어본도 수평 밀림에 같은 k를 적용하되 기존 상승 속도 420, 중력·낙하·Down·Rising 규칙은 유지한다.
이미 공중인 피해자는 다시 띄우지 않고 즉시 하강한다. `Falling`은 상승 구간도 포함하는 기존 반응이다.
공격자 역경직만 유지하며 피해자의 이전 역경직은 취소한다. 밀림은 이미 한 번 감속된 `ActionDelta`로
진행하고 거리·dt에 tempoScale을 다시 곱하지 않는다. 현재 0.18 전투 초는 약 0.257 실제 초다.

방향은 명중 당시 피해자 XY에서 공격 원점 XY를 뺀 단위벡터다. 직접 공격·슬라이딩·몬스터는 명중 시
공격자 위치, 기본/스킬 투사체는 생성 시 저장한 발사자 위치를 사용한다. 같은 좌표이면 저장한 공격 방향을
사용하며 수직 스킬 투사체도 yaw/시전자 방향으로 지면 헤딩을 보존한다. 발사자의 이후 이동/퇴장과 히트스톱 owner 조회는
투사체 밀림 방향에 영향을 주지 않는다. 재피격은 이전 밀림을 새 값으로 교체하며 거리를 합산하지 않는다.

누적 이동량은 `D(t)=distance*(2u-u*u)`, `u=clamp(t/duration,0,1)`이며 매 틱 차이만 이동한다.
룸 strand에서 4 world unit 이하 하위 이동마다 발 타원과 장애물·맵 경계 및 출입구 교차를 검사한다.
막히면 남은 밀림을 삭제하며 벽 축슬라이드나 자동 방 이동은 하지 않는다. 피격 중 기존 입력/AI/분리 이동은
진행하지 않는다. 사망·방 이동·퇴장·클리어·룸 종료에도 밀림을 초기화한다.

### KBK1 실시간 밀림 상태

기존 realtime version=3 고정 레코드와 tail을 유지하고 `HUD1` 뒤에 항상 `KBK1`을 추가한다.
헤더는 ASCII `KBK1`, 플레이어 수 uint16, 플레이어 행, 몬스터 수 uint16, 몬스터 행 순서다.
현재 맵의 모든 actor를 비활성 상태까지 포함한다. 각 행은 패딩 없는 33바이트, little endian이다.

| 순서 | 형식 | 의미 |
|---|---|---|
| id | uint64 | playerId 또는 monster instanceId |
| knockbackSequence | uint32 | 해당 actor의 마지막 reactionSequence |
| knockbackActive | uint8 | 0/1 |
| knockbackDirectionX/Y | float32 두 개 | 지면 단위벡터 |
| knockbackDistance | float32 | HR 적용 후 총거리 |
| knockbackElapsedSeconds | float32 | 전투 초 경과 |
| knockbackDurationSeconds | float32 | HR/피격 잔여 시간 상한 적용 후 전투 초 |

JSON player/monster 행에도 위 knockback 필드 이름을 동일하게 제공한다. 비활성은 방향/거리/시간이 모두 0이며
번호는 마지막 피격 번호를 유지한다. active=true는 살아 있는 피격 actor에서만 나오며 reaction=None과 공존하지 않는다.
새 피격/교체/충돌/자연 종료/피격 해제가 발생한 틱 끝에는 목표 15Hz 주기와 별도로 상태를 전송한다.
아주 짧은 밀림은 같은 틱에서 종료될 수 있으므로 새 번호의 비활성 행도 최종 권위 XY를 적용해야 한다.

클라이언트는 snapshot XY에서 `D(clamp(elapsed+실제age*tempoScale,0,duration))-D(elapsed)`만 외삽하며
기간 끝에서 멈춘다. 밀림 중/새 번호/종료 권위 위치를 기존 16px deadzone 또는 35% 일반 이동 보정으로 버리지 않는다.
active=false, 새 피격 번호, reaction=None/Dead, 맵·룸 변경은 이전 밀림 외삽을 즉시 폐기한다.
같은 피격 번호의 종료는 최신 snapshotSequence/serverTick 순서로 판단하며 늦은 JSON이 되돌리지 않는다.
KBK1이 없는 이전 서버 프레임은 새 클라이언트에서 비활성으로 처리한다. 구형 클라이언트의 미지 태그 처리는
보장하지 않으므로 서버·클라이언트·카탈로그/편집기 계약을 함께 배포한다. 기존 48KiB frame 한도를 유지하며
초과 frame은 잘라 보내지 않는다. 이번 변경은 정적 검토만 했으며 빌드·실행·전투 테스트는 수행하지 않았다.

몬스터 프로필의 `skills` 예:

```json
{ "id": "Slash", "damage": 10, "hitSeconds": 0.4, "reachHeight": 80 }
```

동일 몬스터 문서의 skills에 `Slash`가 실제로 있어야 합니다. 기존 스킬 정의의 `durationSeconds`,
`cooldownSeconds`, `minRange`, `maxRange`, `hitType`과 함께 실행합니다. 서버 effect는 단일 대상 근접 타격입니다.
스킬 시작 때 중심 거리와 쿨다운을 검사하고 대상을 고정합니다. 타격 시점에 같은 방·생존·범위·앞쪽·높이를
다시 확인하므로 대상은 피할 수 있습니다. 타격 범위는 플레이어의 피격 반경을 포함하며 스킬당 한 번 판정합니다.
`hitSeconds`는 0부터 durationSeconds까지입니다. 스킬/동작 길이는 3600초 이하, 쿨다운은 86400초 이하입니다.
다른 투사체·광역·특수 스킬 효과는 별도 구현과 계약이 필요합니다.

## AI 실행 의미

- Wait, MoveToTarget, ReturnToSpawn, UseSkill, PlayMotion과 기존 모든 조건을 처리합니다.
- 같은 방의 준비가 끝난 생존 플레이어 중 detectionRange 안의 가장 가까운 대상을 선택합니다. 동률은 작은 playerId입니다.
- 작은 priority를 먼저 검사합니다. OnUpdate는 틱당 한 번, Immediate는 상태 진입 때,
  AfterAction은 행동 완료 때 검사합니다. 동시에 가능한 연결은 priority로 결정합니다.
- 한 틱의 시간은 행동 실행에 한 번만 소비합니다. 무시간 전환은 틱당 64회까지이며 나머지는 다음 틱에서 이어갑니다.
- 정지 거리/귀환 허용 거리까지 이동하며, 기존 이동 가능/불가 다각형을 4단위 이동 단계로 검사합니다.
  축별 이동은 지원하지만 최단 경로 탐색은 없으므로 복잡한 장애물을 우회한다고 보장하지 않습니다.
- 대상이 없어 이동/스킬을 시작할 수 없으면 그 행동에서 기다립니다. TargetLost 연결로 귀환 등 다음 행동을 지정할 수 있습니다.
- 반복 motion은 완료되지 않습니다. 스킬 쿨다운은 시작 시 소비하고 피격으로 취소되어도 유지합니다.
- HP 0이면 AI를 중단합니다. 일반 지상 피격은 경직, 에어본 타격은 띄워짐→추락→쓰러짐→기상입니다.
  이미 공중인 피해자는 수직 상승을 취소하고 추락합니다. 회복한 몬스터는 initialNodeId부터 재개합니다.
- 월드 전송 후 첫 이동·행동·상태 요청을 받은 플레이어만 AI 대상이 됩니다. 상태는 방 strand에서만 변경됩니다.

## 패킷

새 클라이언트는 초기 월드 수신 후 ID 11~13의 realtime 바이너리 프레임을 구독하여
이동·동작을 목표 15Hz로 받을 수 있습니다. 패킷 ID와 JSON 전투 복구 경로는 유지합니다.
시간·연결/맵 epoch·레코드 규격과 클라이언트 보간 규칙은
[`SERVER_HANDOFF.md`](../../output/movement-smoothing/SERVER_HANDOFF.md)에 있습니다.
서버가 상태를 전달하는 것과 클라이언트가 시간에 맞춰 보간하는 것 모두 필요합니다.
위 handoff의 버전 1 규격에 대해서는 아래 버전 3 변경이 우선합니다. 서버·클라이언트를 함께 갱신해야 합니다.

원본은 `Tool/PacketDefine.yml`입니다. ID 7·8의 필드 순서는 슬라이딩 버전 2와 같으며 version 값은 3입니다.
이번 작업에서는 생성 도구·빌드·게임을 실행하지 않았습니다. 클라이언트도 같은 원본의 필드 순서로 갱신해야 합니다.

| ID | 패킷 | 필드 |
|---|---|---|
| 7 | DungeonActionInput | version:uint16, sequence:uint32, action:uint8, facingLeft:uint8, mapEpoch:uint32, moveSequence:uint32, directionX:int8, directionY:int8, running:uint8 |
| 8 | DungeonActionResult | version:uint16, sequence:uint32, accepted:uint8, serverTick:uint64 |
| 9 | DungeonCombatStateRequest | snapshotId:uint32, offset:uint32 |
| 10 | DungeonCombatStateChunk | snapshotId:uint32, totalBytes:uint32, offset:uint32, status:uint8, retryAfterMs:uint32, payload:string |

### 행동 입력

초기 월드 수신이 끝난 후 요청합니다. `version=3`, action 1=사격, 2=점프, 3=슬라이딩;
facingLeft 0=오른쪽, 1=왼쪽입니다. ID 7의 필드 바이트 수는 19, ID 8은 15입니다(패킷 ID·RUDP 프레이밍 제외).
sequence는 이동 sequence와 독립적이며 연결 내에서 1부터 증가합니다. 중복·역순 요청은 다시 실행하지 않습니다.
플레이어 ID·맵 ID·위치·피해량은 요청하지 않습니다. 잘못된 값과 초당 20개를 넘는 행동 요청은 무시합니다.
accepted는 요청 접수 여부이며 타격 성공을 뜻하지 않습니다. HP/탄환/피격 결과는 스냅샷을 사용합니다.

action=3은 같은 프레임의 `mapEpoch`, `moveSequence`, `directionX/Y`(-1~1), `running=1`을 포함합니다.
현재 맵 epoch와 일치해야 하며 moveSequence는 0보다 크고 서버가 받은 이동 sequence 이상이어야 합니다.
동일 sequence이면 방향과 달리기 상태도 기존 입력과 같아야 합니다. 새로운 sequence이면 이 이동 의도를
슬라이딩 시작과 함께 원자적으로 반영하여 늦게 도착한 이전 이동 패킷이 덮어쓰지 못하게 합니다.
방향 0/0 또는 현재 워프 영역 안에서의 시작은 거절합니다. 이동 의도는 그대로 저장하지만 슬라이딩의
실제 방향은 수평 성분이 있으면 그 부호, 상하 입력만 있으면 기존 서버 facingLeft로 결정하고 필드 깊이 성분을
0으로 설정합니다. 시작 시 facingLeft를 이 방향에 맞춰 고정하므로 좌우 전환과 슬라이딩이 같은 프레임에 입력돼도
해당 이동 스냅샷의 좌우 방향을 사용합니다.
action=1/2에서는 추가 이동 스냅샷 필드를 사용하지 않습니다.
슬라이딩 accepted=1은 즉시 시작, 0은 거절입니다. 유효 형식의 거절도 action sequence를 소비하며 예약하지 않습니다.
버전·action 범위·facingLeft·sequence 형식 오류와 요청 제한 초과는 기존처럼 응답 없이 버립니다.
스킬 입력의 결과도 같은 ID 8을 사용하므로 `version=3`가 포함됩니다.

한 공격 구간은 최대 5발입니다. 공격 중 추가 사격 입력은 다음 발을 예약합니다. 준비→발사 간격→회수 시간을
서버에서 처리하며 공중 사격은 한 번의 점프에서 최대 5발, 바라보는 쪽으로 45도 아래 방향입니다.
공격 중 바라보는 방향은 위 연속 입력 유예 규칙에 따라 고정합니다. 일반 이동의 기존 준비·발사·스킬·피격 상태
제한과 회수·유예 구간 허용은 유지하며 방향 고정을 별도의 이동 금지로 사용하지 않습니다.
공중 사격은 약한 상승·후퇴 반동을 적용합니다.
점프는 준비 후 도약합니다. 준비 중 사격 입력은 도약 후 시작합니다.
착지·피격·사망하면 해당 공중 공격을 취소합니다. 탄환은 같은 방의 살아 있는 몬스터에만 타격하고 벽·지면·사거리에서 사라집니다.
플레이어 간 피해는 없습니다. 방당 탄환은 최대 256개입니다.

### 상태 조각 수신

1. snapshotId=0, offset=0으로 새 스냅샷을 요청합니다.
2. status=0이면 받은 snapshotId를 고정하고 payload의 **바이트 수**만큼 offset을 증가시켜 다음 조각을 요청합니다.
3. 누적 바이트 수가 totalBytes가 될 때 UTF-8 JSON으로 파싱하고 버전·serverTick·mapId를 검사한 뒤 적용합니다.
   부분 조각을 따로 디코딩하지 않습니다. 조각 경계가 UTF-8 문자 중간일 수 있습니다.
4. 새 스냅샷은 최소 200ms 간격입니다. 전송 중 값은 불변이고 새 요청은 이전 스트림을 대체합니다.
5. status=1은 제한/준비 중입니다. retryAfterMs 뒤 **같은 요청**을 재시도합니다.
   첫 응답에 snapshotId가 지정됐다면 그 ID와 해당 offset으로 재시도합니다.
   status=2는 상태 없음, status=3은 ID/offset 불일치입니다. 현재 조립을 버리고 새 스냅샷을 요청합니다.

스냅샷은 최대 512KB, payload는 최대 768바이트, 연결당 전투 조각은 초당 64KB입니다.
클라이언트는 stop-and-wait로 한 요청의 응답을 처리한 뒤 다음 요청을 보내야 합니다.
요청 폭주나 중복 요청을 정상적인 폴링 방식으로 사용하지 않습니다.
serverTick은 30Hz 시뮬레이션 번호이며 매 조각의 수신 시각을 뜻하지 않습니다.
초기 월드 combatRules와 전투 JSON의 tickRate=30, snapshotRate=15, tickIntervalSeconds=1/30을 사용합니다.
전투 JSON의 serverTimeMs는 realtime과 같은 서버 steady_clock 기준이며, 과거의 tick*50 시간으로 덮어쓰지 않습니다.
구독 결과의 정수 밀리초 안내값은 tickIntervalMs=33, snapshotIntervalMs=67입니다.
큰 방에서는 조각 수와 대역폭 제한으로 전체 상태 갱신에 더 오래 걸립니다.

JSON 최상위: `version=3`, `roomId`, `serverTick`, `mapId`, `state`, `cleared`, `players`, `monsters`, `projectiles`.
state는 WaitingForPlayers/Running/Cleared/Stopped입니다. 다른 roomId 또는 이전 serverTick의 상태는 적용하지 않습니다.

- players: playerId, x/y, hp/maxHp, height/verticalSpeed, facingLeft, reaction/reactionSeconds, shotPhase,
  shotSeconds/shotCount/airShotCount, jumpPhase/jumpSeconds, airAttack, actionSequence, moveSequence, running,
  slideActive, slideSequence, slideSeconds, slideDurationSeconds, slideDirectionX, slideDirectionY, slideSpeed.
- monsters: instanceId/dataId, x/y, hp/maxHp, height/verticalSpeed, facingLeft, reaction/reactionSeconds,
  aiNodeId, actionType, actionStarted/actionComplete, actionSeconds, animationId.
- projectiles: id/ownerId, x/y, height, direction, heightDirection.

reaction은 None/Hit/Falling/Down/Rising/Dead, shotPhase는 None/Prepare/Fire/Recover입니다.
jumpPhase는 Grounded/Prepare/Airborne입니다. reaction/HP를 우선하여 표시하고 일반 상태에서 actionType/animationId를 사용합니다.
몬스터 image/sourceRect/pivot/fps는 클라이언트의 기존 모션 메타데이터에서 읽습니다.
Dead 개체는 스냅샷에 남고 HP 0입니다. 사망 애니메이션은 개체당 한 번 시작하고 마지막 프레임을 유지해야 합니다.
스냅샷의 개체 나열 순서는 고정하지 않으므로 항상 ID로 대응합니다.
mapId가 바뀌면 이전 방의 표시 목록을 교체합니다. 삭제된 탄환은 해당 방에서 제거합니다.
클리어 뒤에도 상태 요청은 가능하며 cleared=true입니다.

### Realtime 버전 3

ID 11 요청, ID 12 구독 결과, ID 13 조각의 `version`은 모두 3입니다. 서버는 버전 3 요청만 승인합니다.
플레이어·몬스터의 공통 actor 레코드에서 `reactionSequence` 바로 뒤에 다음 필드를 추가합니다.
공통 actor는 46바이트에서 74바이트가 되며, 플레이어 기본 레코드는 슬라이딩 v2의 101바이트에서 129바이트가 됩니다.

| 필드 | 형식 | 바이트 수 |
|---|---|---|
| hitRecovery | float32 | 4 |
| reactionDurationSeconds | float32 | 4 |
| hitstopRemainingSeconds | float32 | 4 |
| hitstopSequence | uint32 | 4 |
| hitstopStartTimeMs | uint64 | 8 |
| hitstopDurationSeconds | float32 | 4 |

바이너리는 기존 little-endian 순서와 float32 표현을 유지하며, 각 플레이어 기본 레코드의 마지막 `running` 뒤에
다음 슬라이딩 v2 필드를 그대로 유지합니다.

| 필드 | 형식 | 바이트 수 |
|---|---|---|
| slideActive | uint8 | 1 |
| slideSequence | uint32 | 4 |
| slideSeconds | float32 | 4 |
| slideDurationSeconds | float32 | 4 |
| slideDirectionX | float32 | 4 |
| slideDirectionY | float32 | 4 |
| slideSpeed | float32 | 4 |

슬라이딩 종료·취소 때 slideActive=false로 전환하고 마지막 번호·시간·방향·속도는 보존합니다.
모든 기본 플레이어·몬스터·투사체 레코드 뒤에 오는 기존 SKL1 tail의 내부 순서는 유지합니다.
버전 1/2 클라이언트는 변경된 기본 actor 레코드를 읽을 수 없으므로 서버와 함께 갱신해야 합니다.

## 통합 결과와 후속 범위

- 편집기: FallenCitadel 출력·설치 자동화와 Debug·Release 실행 데이터 및 필수 자산 의존성 설치를 완료했습니다.
  새 몬스터를 추가할 때에는 Combat.json 프로필과 AI 스킬의 effect 참조도 확인해야 합니다.
- 클라이언트: ID 2·3 렌더링, X 사격·C 점프 입력/수신, 서버 상태 적용과 보간, HP·피격·사망·클리어 표시를 연결했습니다.
- 최종 소스의 빌드와 실제 입장·전투 확인은 수행하지 않았습니다. 양쪽 EXE를 함께 갱신해야 합니다.
- 설치와 정적 확인 결과는 `output/combat-integration/INTEGRATION_REPORT.md`에 정리했습니다.
- 몬스터/스킬: 최소 패턴·타격 event는 연결했으며 최종 밸런스와 특수 패턴은 별도 합의가 필요합니다.
- 사용자 합의가 필요한 추가 기능: 보상/드랍 정책·데이터, 전멸 종료·부활, 특수 보스 패턴.

## 클리어 후 선택

`cleared=true` 수신 시 마을 이동/재도전 선택 창을 표시하고 전투 입력을 차단한다.
같은 serverTick의 최초 클리어 상태도 적용한다. 방향키·Enter 또는 마우스로 선택하며,
파티에서는 파티장만 전체 파티의 다음 행동을 결정한다. 던전 참여 중 파티 구성 변경은 Busy로 거절한다.

- Town TCP `DungeonCompletionRequest`(25): roomId(uint64), retry(bool).
- Town TCP `DungeonCompletionResponse`(26): previousRoomId(uint64), succeeded(bool), retry(bool),
  roomId(uint64), combatSeed(uint64), sessionBrokerAddress(string), sessionBrokerPort(uint16).
- RoomControl TCP `FinishRoom`(8): requestId(uint64), roomId(uint64), retry(bool), participantPlayerIds(vector<uint64>).
- RoomControl TCP `FinishRoomResult`(9): requestId(uint64), previousRoomId(uint64), succeeded(bool),
  roomId(uint64), combatSeed(uint64), sessionBrokerAddress(string), sessionBrokerPort(uint16).

TownServer는 인증된 요청자의 현재 roomId·파티장·참여자·진행 중 요청을 검사한다.
방 생성 응답 후에도 참가자 예약을 유지한다. 인증된 EnterRoom에서 예약을 실제 입장으로
전환하고, RoomStarted(10)의 실제 참가자 목록에 없는 미입장자는 예약을 해제한다.
RoomStarted는 roomId(uint64), participantPlayerIds(vector<uint64>) 형식이다.
인증 실패·RoomEnded(Aborted)·제어 연결 종료도 예약을 해제한다. 재도전의 새 방에도
같은 예약을 적용한다. ConfirmJoin은 해당 플레이어에게 예약된 roomId만 허용한다.
복귀·재도전 대상은 현재 파티 전체가 아닌 같은 룸에 실제 입장한 유저다. 파티장이
미입장 또는 이탈한 경우 참여 중인 파티원 중 슬롯 순서가 빠른 유저에게 권한을 넘긴다.
Town TCP 연결 종료 시 TownServer가 RoomControl LeaveRoom(6)을 요청하고,
GameRoomServer는 실제 입장 집합에서 제거한 뒤 같은 패킷으로 통지한다.
GameRoomServer는 실제 Cleared 상태와 입장한 참여자 집합의 일치를 확인한다.
마을 이동 성공 시 roomId는 0이다. 재도전은 기존 dungeonId로 새 roomId/seed와 전체 전투 상태를 생성한다.
새 방 생성이 실패하면 기존 클리어 방을 유지한다. 방 한도에 도달해도 기존 방의 교체는 허용한다.
성공 응답을 모든 참여자에게 먼저 보내고 마을 가시성을 복구한다.
클라이언트는 기존 RUDP 코어 종료를 비동기로 기다린 뒤 마을 복귀 또는 새 방 인증을 시작한다.
종료가 끝나기 전에 새 코어를 시작하지 않으며, 대기 중 마을 이벤트를 보관했다가 복귀 후 처리한다.
룸 중단 또는 제어 연결 단절 시 서버는 기존 DungeonCompletionResponse를
previousRoomId=중단된 룸, succeeded=true, retry=false, roomId=0으로 먼저 보내고 타운
가시성을 복구한다. 이 응답은 클리어 성공이 아닌 타운 복귀 성공을 뜻하며, 입장 완료한
클라이언트는 요청을 보내지 않았더라도 기존 종료·복귀 흐름으로 처리한다.
입장 대기 중인 유저에는 EnterDungeonResponse(succeeded=false)를 보낸다.
입장 연결 중에는 기존 RUDP 연결 실패·입장 시간 초과 경로로 클라이언트가 정리한다.
거절·응답 지연 시 안내와 재선택을 제공한다. 중복 클릭과 이전 roomId 응답은 적용하지 않는다.

RoomControl RegisterRoomServer(1)은 roomServerId(uint64), maxRoomCount(uint32),
authenticationKey(string) 순서다. 두 서버는 같은 무작위 64자리 16진수
ACTIONRPG_ROOM_CONTROL_KEY 환경 변수를 사용하며 제어 연결은 loopback에 한정한다.
이번 리뷰 수정은 RoomControl 내부 패킷을 변경하므로 TownServer·GameRoomServer를 함께
빌드/갱신해야 한다. Town·Dungeon 클라이언트 패킷 형식은 이번 수정에서 변경하지 않았다.
이번 변경의 빌드·실행·실제 클리어/복귀/재도전 검증은 수행하지 않았다.

## 아이템 사용 표시 확장

HP 회복과 Stone 직선 투척은 Town의 46 Use/UseStatus와 DB V6 사용 원장을 거쳐 룸 strand에서 확정한다.
일반 사격 shotPhase/shotSequence/탄환 생성에는 투척 표시 동작을 연결하지 않는다.
Stone은 기존 직선 projectile 충돌 계산을 사용하며 visual은 Stone, itemDefinitionId는 Stone이다.
신규 이미지 자산 없이 클라이언트의 기존 표시/fallback을 사용한다.
투척 모션은 기존 AttackFire 아트를 재사용하므로 전용 투척 애니메이션은 포함하지 않는다.

JSON 플레이어 행에 itemUseSequence(uint32), itemUseDefinitionId, itemUseMotionId(AttackFire 또는 빈 문자열),
itemUseElapsedSeconds(float), itemUseDurationSeconds(float)를 추가한다. 투척 표시 시간은 0.15초다.
HP 회복은 동작 문자열과 시간을 빈 값/0으로 두고 실제 HP를 갱신한다.
투사체 JSON에는 itemDefinitionId와 visual을 추가하며 일반 탄환의 두 필드는 빈 문자열이다.

기존 기본 frame과 SKL1 내부 순서는 유지하며 그 뒤에 다음 확장을 추가한다. 정수/float는 little endian,
text16은 UTF-8 바이트 길이 uint16과 해당 문자열이다. 클라이언트는 tail 끝까지 태그를 반복해서 읽는다.
스킬 카탈로그가 비어 있어도 해당 맵에 투사체가 있으면 SKL1을 전송한다. 아이템 투사체의
directionY·speed·radius·age는 이 기존 확장에서 읽으며 클라이언트 기본값으로 대체하지 않는다.

| 태그 | 헤더 | 반복 행 |
|---|---|---|
| IUS1 | 플레이어 수 uint16 | playerId uint64, itemUseSequence uint32, definitionId text16, motionId text16, elapsed float32, duration float32 |
| ITP1 | 투사체 수 uint16 | projectileId uint64, itemDefinitionId text16, visual text16 |

유효한 최신 실시간 프레임이 있으면 이전 JSON이 itemUseSequence와 동작 시각을 되돌리지 않는다.
같은 sequence의 elapsed만 새 서버 상태에 맞춰 보간하며 클라이언트가 예측 투사체를 만들지 않는다.
같은 룸에서 중복 요청은 보존된 영수증으로 답하고, 종료 룸의 미확정 사용은 Settle 완료까지 유지한다.
등록된 원래 roomServerId를 통한 조정은 gameplay 입장 mapping의 종료와 분리되어 있다.
소유권·실패·쿨타임·재접속 계약은 [Town 문서](../TownServer/DEVELOPMENT.md#v6-아이템-사용-hp-회복과-직선-투척)를 따른다.
서버와 클라이언트를 함께 갱신해야 하며 이번 변경의 빌드·전투 실행 검증은 수행하지 않았다.

## HUD 자원과 마지막 명중 대상

초기 월드의 맵별 몬스터와 전투 JSON 몬스터 행은 `name`, `level`(정수 또는 null), `isBoss`(bool)를 담는다.
이름은 `.ai.json`, level/isBoss는 서버 MonsterCatalog 항목에서 읽는다. Dummy·갑옷병은 Lv1 일반,
수호자는 Lv5 보스다. 기존 bosses 집합의 던전 클리어 조건은 유지하며 HUD 등급에는 사용하지 않는다.
보스 방의 일반 몬스터도 카탈로그 isBoss=false이면 일반 표시를 사용한다. HP/maxHp는 기존 actor 필드다.

Combat.json의 `player.maxMp`는 선택 정수(1~1000000)이고 현재 값은 100이다.
`player.characters` 항목의 선택 `maxMp`가 기본값을 재정의하며, 양쪽 모두 없으면 MP는 미정이다.
Shared PlayerResourceCatalog가 Town과 Room의 동일 자원 정의 해석을 담당한다. 룸 캐릭터 입장 때
current MP를 그 캐릭터의 유효 maxMp로 초기화한다. 초기 world 플레이어는 hp/maxHp와 0/0의
lastHitSequence/lastHitMonsterId도 명시한다. 초기 world 플레이어와 전투 JSON은 mp/maxMp를
동시에 정수 또는 null로 전달한다. 초기 world는 캐릭터 확정 후 자원 값을 갱신하고, 다른 executor의
읽기는 고정 캐시 슬롯의 공유 포인터 atomic load/store로 동기화한다. MP 소모·회복·DB 저장은 없다.

실제 몬스터 HP가 감소할 때만 공격자에 `lastHitSequence`와 `lastHitMonsterId`(uint64)를 기록한다.
일반 사격의 몸~총구 검사, 이동 투사체(기본/스킬/Stone), 직접 공격 스킬, 슬라이딩을 같은 함수로 처리한다.
룸 strand의 단조증가 피해 적용 순번으로 같은 tick 안의 복수 명중도 구분한다. 배열 순서·반응 sequence·
다른 플레이어 공격·헛공격에서 대상을 추정하지 않는다. 최초 값은 0/0이며 JSON 플레이어 행에도 포함한다.
마지막 대상이 사망해도 이전 대상 ID로 돌아가지 않는다. 최신 대상 HP=0, 맵 이동, 대상/로컬 actor 부재에는
클라이언트가 상단 창을 숨긴다. 맵 이동 뒤 같은 이전 sequence가 대상을 다시 선택하지 않아야 한다.

실시간 기본 frame·SKL1·IUS1·ITP1 뒤에 `HUD1`을 추가한다. 기존 태그 내부 순서와 전투 version=3은 유지한다.

| 순서 | 형식 |
|---|---|
| 플레이어 수 | uint16 |
| 플레이어 반복 행 | playerId uint64, mpKnown uint8, mp uint32, maxMp uint32, lastHitSequence uint64, lastHitMonsterId uint64 |
| 몬스터 수 | uint16 |
| 몬스터 반복 행 | instanceId uint64, levelKnown uint8, level uint32, isBoss uint8, name text16 |

모든 정수는 little endian이다. text16은 UTF-8 길이 uint16과 해당 바이트다. known 플래그는 0/1이며
미정 MP·level의 숫자 자리는 0이다. known=0의 0을 실제 MP/level로 표시하지 않는다. known MP는
0<=mp<=maxMp, maxMp>=1이며 known level>=1이다. JSON null과 같은 의미로 처리한다.
HP는 기본 actor 레코드에서 읽는다. 서버의 기존 frame 48KiB 한도는 유지하고 초과 frame은 잘라 보내지 않는다.

최신 RT를 적용한 뒤 늦은 JSON이 MP나 마지막 명중 sequence/대상을 되돌리지 않도록 한다.
룸 교체 시 sequence 기준을 새로 시작한다. Town SkillState의 자원은 Town HUD 전용이며 Dungeon의
Room 자원 상태를 덮어쓰지 않는다. 서버·클라이언트를 함께 갱신해야 하며 빌드/실제 HUD 실행은 확인하지 않았다.

## 퀘스트 진행 사실 전달

RoomControl 기존 1~13은 유지하고 14 QuestEvents/15 QuestEventsAck/16 QuestContextRequest/
17 QuestContextResult를 추가한다. 14/15는 roomId와 최대 16KiB JSON, 16은 requestId·roomId·
playerId, 17은 같은 식별자와 최대 16KiB 권위 위치 JSON이다. 기존 BE 정수/text16 형식을 따른다.
등록된 제어 연결과 roomToServerSession 경로가 일치하는 방의 사실만 Town이 처리한다.

사실은 프로세스 incarnation 32자리 hex, 방의 단조 sequence 십진 문자열, dungeonId/mapId,
kind(MonsterKilled/RegionReached/DungeonCleared), targetId와 creditedPlayerIds를 포함한다.
처치는 HP 양수→0일 때 한 번 생성하고 같은 맵의 입장·worldReady 참여자에게 인정한다.
사망한 참여자도 포함하며 다른 맵·미입장 플레이어는 제외한다. 지역 도달은 실제 도달한
플레이어 한 명에게만 인정한다. 클리어는 마지막 보스 처치 맵의 같은 조건 참여자에게 인정한다.
Town은 수락 전에 Room strand의 sequence·위치 barrier를 조회하여 수락 전 지연 사실을 제외하고,
이미 도착한 장소 및 이후 단계의 Reach도 권위 현재 위치로 판정한다.

방 outbox는 512개, 한 배치는 최대 32개/16KiB다. 같은 배치를 실제 시간 1초 간격으로
재전송하며 Town의 DB 확정·중복 확인 뒤 받은 동일 ACK만 제거한다. 대기열이 가득 차면
새 치명타와 클리어 전환을 보류하며 지역 진입은 등록하지 않고 다음 tick에서 다시 확인한다.
퇴장 확인과 RoomEnded는 대기 사실 저장이 끝난 뒤 보낸다. 퀘스트 진행으로 증가한
공유 캐릭터 revision도 UpdatePlayerProgress로 Room에 반영하며 HP를 초기화하지 않는다.

outbox는 메모리이므로 DB 확정 전 프로세스 장애로 잃은 사실의 자동 복구는 보장하지 않는다.
DB에 확정된 보상·처치는 요청 receipt와 Room fact 식별자로 중복 반영을 막는다.
이번 구현은 정적 대조만 수행했으며 서버 실행·DB 적용·실제 전투 검증은 하지 않았다.
