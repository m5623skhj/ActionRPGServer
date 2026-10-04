# 던전 참여 중 스킬 습득·강화 제한 서버 결과

작성: 2026-10-04. 총괄이 전달한 사용자 '진행' 승인 범위에서 최소 서버 변경을 적용했다. 이전 마을 이동 및 기존 성장·전투 변경을 보존했다.

## 변경 파일과 구현

* C:/Users/KimHyeongJin/source/repos/ActionRPGServer/ActionRPGServer/TownServer/TownInstance.cpp
  * TownInstance::LearnSkill에서 dungeonRoomId 또는 reservedDungeonRoomId가 0이 아니면 즉시 거부한다.
  * 기존 성장 상태를 복사하거나 SkillTreeCatalog::Learn을 호출하기 전에 판정하므로 SP 차감 및 스킬 단계 증가가 없다. NotifyProgression도 호출하지 않는다.
  * 거부 응답은 기존 SkillStateResponse JSON에 result="InDungeon"을 넣고 현재 성장·스킬 상태를 함께 반환한다. 패킷 필드와 번호는 변경하지 않았다.
* C:/Users/KimHyeongJin/source/repos/ActionRPGServer/output/dungeon-ui-20261004/SERVER_REPORT.md
  * 상태 범위, 정적 확인, 클라이언트 연계 계약 및 미실시 검사 기록.

## 상태 범위와 순서 검토

* 최초 습득과 기존 스킬 강화 모두 LearnSkillRequest -> TownInstance::LearnSkill -> SkillTreeCatalog::Learn을 공유한다. 어느 스킬 단계이든 같은 제한을 적용한다.
* 룸 생성 성공 후 예약을 설정하고, 입장 확인에서 dungeonRoomId로 옮기면서 예약을 지운다. 예약도 판정하여 룸은 마련됐지만 입장 통지가 늦는 구간과 재도전 입장 구간의 변경을 차단한다.
* 룸 생성 요청 대기만으로 별도 제한을 추가하지 않았다. 룸 예약이나 실제 참여 상태가 없는 마을 상태에서는 기존 검증 경로를 사용한다.
* 정상 마을 복귀는 LeaveDungeon에서 참여 상태를 해제한다. 예약 취소·입장 누락·비정상 룸 종료도 기존 LeaveDungeon 경로로 예약/참여 상태를 해제한다. 다음 요청은 기존 캐릭터 일치·요구 레벨·선행 조건·SP·최대 단계·expectedSkillLevel 검증을 거친다.
* 재도전은 복귀 처리 직후 같은 strand 콜백에서 새 룸 예약을 설정하므로 계속 제한한다. 클리어됐지만 아직 마을로 나가지 않은 상태도 참여 중이므로 제한한다.
* 상태 설정/해제 및 LearnSkill의 판정·검증·SP/레벨 변경은 동일 Town strand에서 실행된다. 판정과 성장 반영 사이에 다른 입장/퇴장 콜백이 끼어드는 비동기 구간을 추가하지 않았다. 처리 순서상 예약/참여 상태를 설정하기 전에 완료된 마을 습득은 유지된다.
* SkillStateRequest, GetProgression, 배운 스킬 사용, 단축키 등록, 캐릭터 레벨 갱신 및 파티 처리에는 이번 제한을 추가하지 않았다.

## 정적 확인과 제한

* PlayerSession의 LearnSkillRequest 호출, 공용 SkillTreeCatalog::Learn의 SP/단계 변경, 예약·입장·복귀·재도전 처리 경로를 정적으로 대조했다.
* 대상 C++ 파일 git diff --check 통과. 코드상 거부 분기는 성장 상태 복사와 Learn 호출보다 앞에 있으며 현재 상태 전송 후 반환한다.
* 빌드, 테스트, 게임/서버 실행, 백업 생성, stage/commit/push는 하지 않았다. 직접 패킷 우회, 입퇴장 경계의 실제 동시 요청 및 UI 결과 표시는 실행 검증하지 않았다.

## 총괄이 클라이언트 담당에게 전달할 계약

* 거부 결과 문자열은 InDungeon이다. SkillStateResponse의 JSON result 값을 읽어 '던전에서는 스킬을 습득·강화할 수 없습니다' 등의 사유를 표시해야 한다.
* 거부 응답에 현재 progression, skillTrees, playerSkills, characterId가 포함된다. 실패를 성공으로 표시하거나 클라이언트에서 SP/단계를 먼저 확정하지 않아야 한다.
* 서버는 입장 예약부터 제한하므로 클라이언트의 던전 연결 중 습득 버튼도 해당 상태와 맞추는 것이 필요하다. 조회·단축키 등록·기존 스킬 사용은 이번 제한과 분리한다.
* 직접 다른 담당에게 메시지를 보내지 않았다. UI 및 결과 사유 표시는 클라이언트 담당의 작업 범위다.

## 제안 커밋 로그

* 던전 참여 중 스킬 습득과 강화 요청 차단
  * 룸 참여·입장 예약 상태를 Town strand에서 판정
  * 거부 시 SP와 스킬 단계를 보존하고 현재 상태 및 InDungeon 결과 반환
  * 퇴장 후 기존 습득 조건 유지와 클라이언트 연계 계약 기록

제안 로그이며 실제 커밋 및 푸시는 수행하지 않았다.
