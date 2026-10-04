# Character1 앞차기 모션 제작 요청

규칙: C:/Users/KimHyeongJin/source/repos/ActionRPGServer/docs/workflows/DUNGEON_CREATION.md / v1.0.1
작업 ID: character1-front-kick-v1
요청 ID: character1-front-kick-motion / revision 1 / 요구사항 revision 1
단계: 05 그래픽 (단독 스킬 제작의 그래픽 요청)
담당: ActionRPG 그래픽 제작 담당 / 01a0fa9f-7587-73e2-a85d-be95e68d307f / local
요청자: ActionRPG 스킬 담당 / 01a0fa9f-98aa-7a23-a679-2fced8fa5fdd

## 승인 근거와 범위

총괄이 스킬 담당 채팅을 직접 읽어, 사용자 요청 '총으로 탄환을 발사하는 스킬 말고 체술 스킬도 하나 만들어 볼 수 있겠어?'에 대한 계획과 후속 사용자 승인 '생성해봐'를 확인했습니다. 승인된 계획 1번은 총괄을 통해 그래픽 담당에게 기존 캐릭터 외형을 유지한 앞차기 8프레임 모션을 요청하는 것입니다.
새 스킬은 Character1, 지상 전용, Up → Z, 직접 공격, 피해 25, 쿨다운 2초, 총 0.5초이며 기존 SkillEditor 계약으로 구성합니다. 입력 단계 간격 0.35초는 스킬 담당 요청값이며 그래픽은 이 값을 수정하지 않습니다.
그래픽 담당의 현재 작업은 완료 상태이며 공통 큐에 활성 요청이 없음을 확인했습니다.

## 요구사항

- 기존 플레이어 외형을 유지한 오른쪽 보기의 앞차기 8프레임. 흰 머리·얼굴·검은 긴 코트·바지·장갑·신발·픽셀 스타일·팔레트·윤곽 일관성을 유지합니다.
- 참조 원본을 직접 읽고 확인합니다:
  - C:/Users/KimHyeongJin/source/repos/ActionRPGClient/ActionRPGClient/Assets/Images/Characters/player_idle.png
  - C:/Users/KimHyeongJin/source/repos/ActionRPGClient/ActionRPGClient/Assets/Images/Characters/player_run.png
  - C:/Users/KimHyeongJin/source/repos/ActionRPGClient/ActionRPGClient/Assets/Images/Characters/player_jump_start.png
- 총을 발사하지 않고 아래로 내려 유지합니다. 다리·몸통·균형 잡는 팔로 체술을 표현합니다.
- 오른다리로 앞차기, 왼다리로 지지. index 0~2 준비(체중 이동→무릎 들기), 3~4 타격(발 뻗기), 5~7 회수(무릎 접기→발 내리기→대기 복귀).
- 지지발이 지면에서 미끄러지지 않으며 좌우 다리가 서로 바뀌거나 중복 프레임이 되지 않아야 합니다. 모든 발·코트·머리를 프레임 안에 넣습니다.
- fps 16, durationSeconds 0.5, loop=false, holdLastFrame=true. 실제 공격 영역은 스킬 담당이 완성된 모션에 맞춰 설정합니다.
- 투명 RGBA PNG. 희망 프레임 320×320, 4열×2행, 총 1280×640. 기존 256×256 모션의 캐릭터 픽셀 크기를 유지하며 추가 캔버스는 발차기 여백입니다.
- 요청 크기와 실제 치수를 구분합니다. 기존 기준 배율은 202/256=0.7890625 월드 단위/원본 픽셀입니다. 균일 분할이 불가능하면 실제 프레임별 sourceRect/pivot를 제공합니다.

## 출력과 완료 조건

출력 절대 폴더: C:/Users/KimHyeongJin/Documents/Codex/2026-10-02/actionrpg-skills/outputs/character1-front-kick-v1/art
- player_front_kick.png
- animations.json: 기존 CharacterEditor version 1, Character1 / frontKick, 실제 이미지 치수·8프레임·16FPS·loop=false·holdLastFrame=true·0 기반 index/sourceRect/pivot·실제 표시 배율. 게임용 상대 이미지 경로는 Images/Characters/player_front_kick.png입니다.
- GRAPHICS_REPORT.md: 참조·제약·실제 치수·투명도·프레임 경계·기준점·동작 일관성 확인·미완료 항목·커밋 로그를 기록합니다.
- 권장 GRAPHICS_RESULT.json: jobId/requestId/revision/status 및 산출물 절대 경로를 기록합니다.
시트 분할이나 배율이 확정되지 않으면 제약으로 명시하며 임의 확정하지 않습니다.

## 실행 제한과 결과 전달

새 그래픽 산출물과 메타데이터만 생성합니다. 코드·기존 에셋·카탈로그 수정, 실행 경로 설치, 빌드·게임 실행·기능 테스트, stage/commit/push, 새 세션·서브에이전트 생성은 승인 범위에 없습니다. 정적 이미지 확인과 메타데이터 검사는 제작 범위에 포함됩니다.
총괄이 산출물과 현재 채팅 최종 응답을 읽고 취합합니다. 다른 채팅으로 답신하지 않아도 됩니다. 현재 요청 범위의 완료 사실과 커밋 로그를 최종 응답 및 GRAPHICS_REPORT.md에 남겨 주세요.
