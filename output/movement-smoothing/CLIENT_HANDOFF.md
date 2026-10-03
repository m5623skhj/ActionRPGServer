# 이동 보간 클라이언트 인계

- 공통 요청 ID: `movement-smoothing-20261003`
- 적용 계약: `SERVER_HANDOFF.md`의 실시간 payload **v1**, `Tool/PacketDefine.yml`의 패킷 ID **11~13**. 기존 ID 1~10 유지.
- 상태: **클라이언트 소스 적용 및 정적 확인 완료**. 빌드·게임/서버 실행·기능 테스트는 요청에 따라 수행하지 않음.
- 클라이언트 저장소: `C:\Users\KimHyeongJin\source\repos\ActionRPGClient`.
- 기준 HEAD: `05bd222`. 기존 던전 클리어 선택 창·마을 복귀·재도전 작업이 반영된 코드를 기준으로 진행함.
- 서버 세션 `01a0fa9f-44e9-7750-bf89-1583ad8f1334`의 완료 보고와 요청 ID·계약 내용을 확인한 뒤 적용함. 이 작업에서 서버 소스는 수정하지 않음.

## 2026-10-03 추가 적용: 서버 30Hz / 전송 15Hz

- 서버 담당 세션의 사용자 승인 메시지와 서버 구현·정적 검토 완료 보고를 확인했다. 갱신한 `SERVER_HANDOFF.md`의 시간 규격을 적용했다. 바이너리 **v1**, 패킷 ID/필드 순서/레코드는 이번 변경에서 유지된다.
- 이번 추가 작업 기준 클라이언트 HEAD는 `769578d`이다. 최초 보간 작업 이후의 스킬/에디터 등 기존 작업 중 변경을 유지하고 클라이언트 **4개 파일만** 수정했다.
- `Network/DungeonClient.cpp`: 기존 20Hz의 50ms와 새 30Hz의 33ms 안내값을 모두 허용한다. 전송 간격은 기존 50~100ms 허용 범위 안의 67ms도 받는다. 응답의 반올림 값 33/67ms를 시뮬레이션 dt나 누적 시간 계산에 사용하지 않는다.
- `Game/DungeonCombat.h` / `Game/DungeonCombat.cpp`: 초기 월드 `combatRules.tickIntervalSeconds`를 double로 보관하며 누락 시 기존 0.05초를 사용한다. reliable JSON의 `serverTimeMs` 존재 여부와 값을 파싱하고, 스냅샷의 선택적 `tickIntervalSeconds`도 검증·보관한다. 간격이 제공되면 숫자·유한값·양수·최대 1초를 검사한다. 제공된 timestamp는 0도 누락으로 취급하지 않는다.
- `Game/GameWorld.cpp`: 서버가 제공한 JSON timestamp를 보존한다. 없는 경우에만 `serverTick × (스냅샷 간격 → 초기 월드 간격 → 구형 0.05초) × 1000`을 반올림하여 uint64 밀리초로 변환하고 범위를 확인한다. `serverTick * 50` 고정 계산을 제거했다. 따라서 30Hz에서 2틱은 약 67ms로 계산되며 매 틱 33ms를 누적하지 않는다.
- 서버 `TICK_SECONDS`는 실제 float `1.0f/30`이며 클라이언트는 JSON에 제공된 값을 사용한다. 현재 서버의 JSON timestamp와 realtime timestamp는 같은 steady_clock epoch이다. 정확한 timestamp가 있을 때 합성 시간으로 덮어쓰지 않는다.
- 기존 125ms 표시 지연을 유지한다. 목표 전달 간격 약 66.667ms의 1.875배로, 한 번의 유실/지연에 대한 여유를 유지하는 시작값이다. 이것은 실제 전달 성능의 측정 결과가 아니다. 100ms 예측 상한과 500ms 정체 기준도 유지한다.
- 본인 입력 예측·원격 보간·동작 지속시간·클리어 선택·복귀·재도전·게임 스레드 소유권 로직은 이번 수정에서 변경하지 않았다. 신규 공유 버퍼나 수신 스레드 접근을 추가하지 않았다.

이번 추가 작업의 변경 파일(클라이언트 저장소 기준):

1. `ActionRPGClient/ActionRPGClient/Game/DungeonCombat.h`
2. `ActionRPGClient/ActionRPGClient/Game/DungeonCombat.cpp`
3. `ActionRPGClient/ActionRPGClient/Game/GameWorld.cpp`
4. `ActionRPGClient/ActionRPGClient/Network/DungeonClient.cpp`

정적 확인: 수정 전 원본 해시/적용 후 후보 해시 일치, C++ 괄호 균형, JSON 시간 존재 분기와 기본 간격·double 파싱·유한값/범위 검사, 20Hz/30Hz 간격 허용, 서버 시간 메타데이터 대조, 서버/클라이언트 생성 패킷 파일 일치, 30Hz/20Hz 밀리초 환산 계산, 125ms/100ms 상수 유지, 본인/클리어 경로 유지, 해당 4개 파일의 `git diff --check`를 확인했다. 이는 C++ 빌드나 기능 실행 검증이 아니다.

이번 작업 전후 Git index SHA256은 모두 `c7970181576de69b3fb0c2f0facf6ff25faf7865cf650ea43f44c4a0ba8d5917`이다. stage/commit/push와 빌드·게임/서버 실행·기능 테스트는 하지 않았다. 실제 30Hz/15Hz 전달·이동감·유실 조건·클리어/복귀/재도전 실행 결과는 미검증이다. 아래의 16개 파일 및 이전 index 값은 최초 이동 보간 작업의 기록이다.

이번 추가 작업의 제안 커밋 로그:

```text
* 서버 30Hz·상태 전송 15Hz에 맞춰 클라이언트 시간 처리 개선
  * 실시간 v1 구독의 33/67ms 안내값과 기존 20Hz 간격 허용
  * JSON 서버 시간 보존 및 정확한 틱 간격 기반 구형 상태 환산
  * 125ms 보간·본인 예측·클리어 복귀 흐름 유지
  * 시간 규격 정적 확인 및 클라이언트 인계 문서 갱신
```

## 적용 내용

1. 초기 월드 수신 후 현재 challenge와 room ID로 실시간 v1 상태를 구독한다. reliable 구독 응답과 unreliable 조각의 도착 순서가 바뀌어도 처리하며 첫 유효 프레임/응답의 숫자 dungeon ID를 고정한다. 최초에는 20Hz/10Hz를 기준으로 연결했고 현재는 위 추가 적용에 따라 30Hz/15Hz를 지원한다.
2. 조각은 768byte 정렬·정확한 길이·48KiB 상한·일관된 메타데이터를 확인한다. 최대 두 미완료 프레임만 보관하고 500ms 후 폐기한다. 중복 조각, 완료 순번 이하, 낮은 mapEpoch, 과거 tick/time, 다른 challenge/room/dungeon은 표시 상태에 적용하지 않는다.
3. binary v1은 명시적인 little-endian 정수와 IEEE binary32로 읽는다. 레코드의 필드 순서·크기는 공통 actor 46byte, player 76byte, monster 61byte+문자열, projectile 36byte이다. 개수·ID 중복·부동소수·HP·enum·불리언·문자열·남는 바이트를 검사한다.
4. 게임 스레드가 소유하는 최대 8개 전체 상태 이력에서 서버 시간에 따라 위치·높이·수직 속도를 보간한다. 표시 지연 시작값은 125ms이며 clock offset을 완만하게 보정하고 표시 시간이 역행하지 않게 한다. 마지막 두 상태의 속도 예측은 최대 100ms이고 500ms 이상 새 상태가 없으면 마지막 표시 위치를 유지한다. 같은 시간의 새 순번은 최근 상태를 교체한다.
5. 본인 캐릭터는 최신 서버 권위 상태와 기존 입력 예측을 즉시 적용한다. 지연 이력은 원격 플레이어·몬스터·투사체 표시에만 사용한다. 전체 프레임의 최신 HP와 생성/삭제 목록은 서버 권위로 처리하며 사망, 신규 생성, 200unit 이상의 불연속 이동 사이에는 위치를 보간하지 않는다. 방/맵 이동은 mapEpoch로 별도 구간을 구분한다.
6. 방향·반응·행동 단계는 같은 표시 시간의 상태를 사용한다. 실제 발사/점프 시작 식별자 `shotSequence`/`jumpSequence`, 몬스터 `actionSequence`, `reactionSequence`로 새 동작을 구분한다. 입력 ACK인 플레이어 `actionSequence`로 애니메이션을 재시작하지 않는다. 반복 상태에서는 동일 클립 재생 시간이 뒤로 가지 않도록 한다. 원격 플레이어 걷기/달리기는 측정 이동 속도와 기존 walk/run 속도에 맞춰 제한된 재생 배율을 적용한다.
7. 연결 종료·복귀·재도전·새 challenge에서 수신 이력을 지우고, 높은 mapEpoch 수신 즉시 이전 표시 이력을 폐기한다. 맵 전환과 클리어에서도 이력을 초기화한다. 기존 클리어 UI와 비동기 연결 종료/재도전 흐름은 유지한다.
8. 기존 전체 JSON 복구는 계속한다. 실시간 프레임이 정상 도착하면 전체 JSON 완료 뒤 약 1초 간격으로 요청하고, 500ms 동안 완성 프레임이 없으면 복구를 앞당긴 뒤 기존 200ms 간격으로 진행한다. 이 전환은 한 번만 앞당겨 요청 폭주를 막는다. 실시간으로 놓친 클리어 상태도 reliable JSON으로 확인한다.
9. RUDP 수신 큐 → `ConsumeEvents` → `GameWorld` 전달 흐름을 유지한다. 조각 재조립과 표시 이력은 게임 스레드에서만 접근하며 비동기 Stop 중 impl/수신 이력에 접근하지 않는다. 표시 보간은 피해·충돌·서버 판정을 변경하지 않는다.

## 변경 파일

아래 경로는 모두 클라이언트 저장소 기준이며 총 16개이다.

- `ActionRPGClient/ActionRPGClient/Game/DungeonCombat.h`
- `ActionRPGClient/ActionRPGClient/Game/DungeonCombat.cpp`
- `ActionRPGClient/ActionRPGClient/Game/DungeonCombatBuffer.h` (신규)
- `ActionRPGClient/ActionRPGClient/Game/DungeonCombatBuffer.cpp` (신규)
- `ActionRPGClient/ActionRPGClient/Game/Character.h`
- `ActionRPGClient/ActionRPGClient/Game/Character.cpp`
- `ActionRPGClient/ActionRPGClient/Game/Monster.h`
- `ActionRPGClient/ActionRPGClient/Game/Monster.cpp`
- `ActionRPGClient/ActionRPGClient/Game/GameWorld.h`
- `ActionRPGClient/ActionRPGClient/Game/GameWorld.cpp`
- `ActionRPGClient/ActionRPGClient/Network/DungeonClient.h`
- `ActionRPGClient/ActionRPGClient/Network/DungeonClient.cpp`
- `ActionRPGClient/ActionRPGClient/Network/DungeonProtocol.h` (동일 서버 YAML에서 생성)
- `ActionRPGClient/ActionRPGClient/Network/DungeonProtocol.cpp` (동일 서버 YAML에서 생성)
- `ActionRPGClient/ActionRPGClient/ActionRPGClient.vcxproj`
- `ActionRPGClient/ActionRPGClient/ActionRPGClient.vcxproj.filters`

추가 산출물: 서버 저장소의 `output/movement-smoothing/CLIENT_HANDOFF.md` (이 문서).

## 정적 확인 결과

- 공식 PacketGenerator의 읽기 전용 `Generate(schema, True)` 확인 통과: 클라이언트와 서버 생성 헤더/소스 모두 현재 스키마와 일치.
- 기존 ID 유지, v1 필드 순서 및 46/76/61/36byte wire 크기 계산 확인.
- C++ 소스 괄호 균형, vcxproj/filters XML 및 신규 파일 등록 확인.
- 순번/epoch 하한, 미완료 프레임 한도, 예측/정체 한도, 본인 캐릭터 비지연 경로, 클리어 선택/연결 종료 경로, 동작 식별자 및 단조 재생 시간 조건을 소스로 확인.
- 검토 후보의 원본 해시를 적용 직전 확인해 동시 수정 충돌을 방지했고, 적용 후 16개 파일 해시가 검토 후보와 일치함을 확인.
- 클라이언트 working diff 및 기존 cached diff의 `git diff --check` 통과.
- Git index SHA256은 적용 전후 모두 `CD894A48842208778C7620D71746E7CD503177C834222AB41C3A7EB6529DF435`. 기존 사용자 staged DungeonEditor 4개 파일을 보존함. 이 작업으로 stage/commit/push하지 않음.

## 미검증 사항과 후속 확인 범위

- C++ 컴파일·링크 성공, 게임 실제 이동감, 실제 서버 15Hz 전달 달성 여부, 패킷 손실/지연/역순 조건, 사망/맵 이동/클리어/복귀/재도전의 실행 결과는 미검증이다. 정적 확인이 실행 성공을 의미하지 않는다.
- 125ms는 최초 10Hz 목표에서 정했고 새 15Hz 목표에서도 유실 여유를 위해 유지한 시작값이다. 기존 5Hz JSON 복구에서도 충분하다고 보장하지 않는다. 실제 갱신 주기와 지연 분포를 측정한 뒤 조정 여부를 판단해야 한다.
- 몬스터 이동 클립은 기존 재생 속도를 유지한다. 원격 플레이어는 이동 속도 배율을 연결했지만 몬스터 클립의 보폭/AI별 속도 적합성은 실행 관찰 없이 확정하지 않았다.
- 동일 mapEpoch 안의 200unit 미만 순간 이동은 일반 이동과 구분할 별도 wire 표식이 없으므로 거리 기준만으로 모두 감지할 수 없다. 서버의 방/맵 이동은 epoch로 확실히 구분한다.
- 실제 적용 확인에는 변경된 서버와 클라이언트의 빌드 및 실행이 필요하다. 이번 승인 범위에서는 실행하지 않았다.

## 제안 커밋 로그

```text
* 던전 몬스터 및 원격 플레이어 이동 보간 개선
  * 실시간 상태 v1 구독·검증·조각 재조립 연결
  * 서버 시간 기준 위치·높이 보간 및 제한된 예측 적용
  * 동작 식별자 기반 애니메이션 재시작·되감기 방지
  * 맵 이동·클리어·복귀·재도전 초기화 및 JSON 복구 유지
  * 패킷·프로젝트 정적 확인 및 클라이언트 인계 문서 작성
```
