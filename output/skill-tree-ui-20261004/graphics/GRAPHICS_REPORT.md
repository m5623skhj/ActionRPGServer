# 스킬 UI 그래픽 납품 기록

작성: 2026-10-04 / 그래픽 담당

## 산출물과 상태

- `character1_front_kick.png`: Character1.FrontKick용 단일 컬러 아이콘. 최신 앞차기 모션의 파란 바지·적갈색 부츠를 사용한 오른쪽 앞차기 하체 실루엣.
- `system_menu_skills.png`: 일반 스킬/능력 메뉴용 별도 컬러 아이콘. 금색 연결선과 다섯 보석 노드로 능력 분기를 표현. 기존 파티·종료 메뉴의 금색·남색·붉은색과 보석 재질을 참고. 외곽 프레임 없음.
- `GRAPHICS_REPORT.md`: 실제 치수·알파·가독성·제약 및 사용 프롬프트 기록.

두 PNG는 built-in image_gen 생성 결과를 바이트 그대로 복사했다. 리샘플링·알파 임계값 제거·색상 변환을 적용하지 않았다. 요청된 생산용 PNG는 각 개념당 하나씩이며 회색판은 만들지 않았다. 검토용 납품본이며, 아래 규격 차이와 알파 잔여를 확인한 뒤 총괄의 최종 채택 판단이 필요하다.

## 계약 반영

기준 화면1280×720, 슬롯64×80, 기본 아이콘56×56, 작은 창48×48, 상세96×96, 여섯 슬롯 바440×96. 메뉴 아이콘은128×128 표시. 후속 계약은 원본128×128 RGBA8/sRGB, 사방8px(6.25%) 안전 여백, 핵심 윤곽 약4px 이상·분리 공간 약6px 이상을 권장한다. 초기 제작 목표112×112는 후속 계약에 따라 원본128 권장으로 대체되었다.

**실제 파일은 두 개 모두1254×1254 RGBA8이다. 128×128 납품 규격을 달성했다고 보고하지 않는다.** 클라이언트는 원본 전체를 지정된 표시 사각형으로 축소한다. 56px 배율=56/1254≈0.0446571, 48px≈0.0382775, 96px≈0.0765550, 메뉴128px≈0.1020734. 원본 치수는 표시 크기나 클릭 영역을 의미하지 않는다.

RGBA의 alpha 범위는0..255이며 완전 투명 픽셀이 실제 존재한다. PNG에 sRGB/ICC/gAMA 메타데이터가 없으므로 sRGB 프로필 지정 준수는 확인되지 않았다. 색상은 참조 이미지와 정적 시각 비교했으며 색 관리가 적용된 런타임 결과는 확인하지 않았다.

## 실측

주 실루엣은 alpha>16 경계로 측정했다. bbox는 오른쪽·아래쪽 제외 좌표이며, 낮은 알파 잔여를 숨기지 않기 위해 alpha>0 경계도 병기했다. 여백 순서는 왼쪽/위/오른쪽/아래다.

| 항목 | 앞차기 | 스킬 메뉴 |
|---|---|---|
| 실제 치수/모드 |1254×1254/RGBA8|1254×1254/RGBA8|
| 주 실루엣 bbox(alpha>16)|(196,280)..(1060,1049)|(126,138)..(1127,1143)|
| 주 실루엣 여백(px)|196/280/194/205|126/138/127/111|
| 주 실루엣 여백(%)|15.6300/22.3285/15.4705/16.3477|10.0478/11.0048/10.1276/8.8517|
| 56px 표시 환산 여백|8.7528/12.5040/8.6635/9.1547|5.6268/6.1627/5.6715/4.9569|
| 모든 비영점 알파 bbox|(97,53)..(1210,1214)|(41,103)..(1198,1182)|
| 완전 투명 픽셀 수|1,332,178|1,259,886|
| alpha1..16 픽셀 수|7,395|11,592|

- 앞차기 주 실루엣의 좌우 여백은 거의 동일하지만 상하 차이가56px 기준 약3.35px이다. 사방 동일 여백 요구를 정확히 달성하지 못했다. 캔버스 중심 기준 전체 PNG를 배치하며 포즈별 확대 보정은 하지 않는다.
- 메뉴 주 실루엣의 최소 여백은8.85%로6.25% 기준보다 넓다. 상하 차이는56px 기준 약1.21px이다. 여백이 정확히 동일하지는 않다.
- 두 파일 모두 alpha1..16 픽셀이 남아 있다. 전부 외곽 오염이라고 판정한 수치는 아니지만 alpha>0 경계는 주 실루엣보다 넓다. **사방6.25% 영역 전체가 alpha0이라는 엄격한 안전 여백 기준은 미충족이다.** 완전 무잔여 알파 정리 완료로 보고하지 않는다.
- 윤곽4px·분리 공간6px 기준은128px 환산의 모든 경계에 대해 수치 인증하지 않았다. 작은 크기의 중요한 형상 식별 여부를 정적으로 확인했다.

## 정적 시각 검토와 표시 인계

Pillow로 별도 작업용 미리보기만 생성하여 어두운 청회색과 밝은 회색 바탕에서 bilinear 축소48/56/96px을 관찰했다. 앞차기의 오른쪽으로 뻗은 다리/부츠와 지지 다리는 구분된다. 48px에서는 부츠와 윤곽의 세부가 줄지만 앞차기 방향은 남는다. 메뉴의 분기 연결과 개별 노드는48px에서도 구분되며 보석 재질 세부는96/128px에서 더 명확하다. 128px에서 기존 파티/종료 메뉴와 나란히 비교했고 금색·보석 색감은 조화된다. 요청대로 외곽 프레임을 생략해 기존 메뉴보다 장식 밀도는 낮다.

정적 미리보기에서 강한 회색 외곽선, 잘린 부츠/노드, 잔상, 실제 문자·키·숫자·테두리·자물쇠는 관찰되지 않았다. 이것은 앞서 기록한 낮은 알파값 픽셀의 부재를 뜻하지 않는다. 게임의 실제 축소 필터·DPI·패널 합성 결과는 미확인이다.

클라이언트 계약의 UI 아이콘 선형 축소 방침을 따른다. 앞차기는 초기 nearest 비교도 했으나 생산 PNG를 특정 필터 결과로 리샘플링하지 않았다. 아이콘은 전체 캔버스를 중앙 정렬하며 정적 UI 이미지이므로 캐릭터 모션의 지지발 pivot은 적용하지 않는다. 쿨타임 회색 처리·숫자·키 라벨·슬롯 테두리·잠금 표시는 클라이언트가 별도로 렌더링한다.

검토 미리보기: `work/skill-icon-review/handoff-readability.png` (생산용 에셋 아님).

## 참조와 설치 범위

- 최신 앞차기 참조: `C:/Users/KimHyeongJin/source/repos/ActionRPGClient/ActionRPGClient/Assets/Images/Characters/player_front_kick.png`.
- 메뉴 참조: 같은 클라이언트의 `Assets/Images/UI/system_menu_party.png`, `system_menu_exit.png`.
- 표시 계약: `output/skill-ui-20261004/CLIENT_UI_HANDOFF.md`.
- 총괄의 복사 예정 대상: `Assets/Images/UI/Skills/character1_front_kick.png`, `Assets/Images/UI/system_menu_skills.png`. 실제 Assets 복사/등록은 수행하지 않았다.

기존 모션·스킬 데이터·소스 코드·Assets는 수정하지 않았다. 빌드·게임 실행·기능 테스트·백업 생성·stage/commit/push·타 대화 메시지 전송은 수행하지 않았다. 치수·RGBA·알파·해시 및 정적 미리보기 확인만 수행했다.

## SHA-256

- `character1_front_kick.png`: `7a85ce173e15f0807821f5be38d82e6034316b6df87a6a3461b5518c2a0e4d99`
- `system_menu_skills.png`: `8131e34cddb6b3592c8d4c09353a003edb8cc19532c5a5b24db7dfd7fcebe56a`

## 생성 방식과 최종 프롬프트 세트

Built-in image_gen 사용. 앞차기4회(초기 생성+레이아웃 수정3회), 메뉴2회(생성+여백 수정1회) 중 마지막 결과를 각각 선정했다. 수정 호출에서 이전 PNG를 참조했고 별도 수작업 픽셀 편집은 하지 않았다.

### 앞차기

선정 생성 원본: `C:\Users\KimHyeongJin\.codex\generated_images\01a0fa9f-7587-73e2-a85d-be95e68d307f\exec-59c1edc9-8212-47d9-889b-0f07dcc1314a.png`

프롬프트1:

```text
Use case: stylized-concept. Create ONE compact pixel-art SKILL ICON for Character1.FrontKick in the existing game, using the already viewed front-kick sprite sheet as palette/anatomy/style reference. This is a separate symbolic UI icon, NOT a full character, NOT a sprite atlas.
Target artwork 112x112 source pixels for a 56x56 display. Logical design must read perfectly at 56x56. Genuine transparent RGBA background, all outside pixels transparent.
SUBJECT: cropped lower-body gesture ONLY, TWO BLUE-TROUSERED LEGS and REDDISH-BROWN LEATHER BOOTS matching the source. A small cropped dark charcoal coat/hip fragment at upper LEFT anchors the two legs. The RIGHT kicking leg extends STRAIGHT HORIZONTALLY toward SCREEN RIGHT at hip/waist height, its reddish-brown boot on far RIGHT with toe up and sole facing right as in front-kick peak frame4. LEFT supporting leg bends gently down at the LEFT and terminates in a planted reddish-brown boot near bottom left/center. Clearly distinguish the raised horizontal kicking boot from the supporting boot. Exactly two legs and two boots, normal knees. No head, torso detail, hands, gun or other props; no new costume.
Very simplified bold hard-edged pixel clusters: deep blue trousers, lighter blue cuff, reddish-brown boots with one orange-red highlight and dark maroon sole, dark-charcoal outline. Only 2-3 value levels per material; no detailed shoelaces, tiny buckles, leather texture, complicated creases or painterly shading. Sparse large readable pixel shapes. Stroke weight at logical 56px is about1px, not a thick black blob. Accurate original blue/red palette, not neon colors. NO smooth vector curvature or antialiased halo.
COMPOSITION: centered leg-gesture silhouette with equal blank margins on ALL four sides: roughly12px in target112px canvas (6px at display56px). Fit both boots, knee and hip completely inside this square. The bounding box should be centered, not shoved right or left. Broad raised boot and horizontal kicking blue leg are main focal shapes; support leg smaller but obvious.
ONLY foreground gesture on real alpha transparency. No background panel, shadow, circular or square border, ornamental frame, arrows, stars, impact sparks, speed lines, afterimages, extra boots, text, letter, key label, cooldown overlay, grayscale version or padlock. No checkerboard image; no gray fringe. Return a single full-color icon.
```

프롬프트2:

```text
Use case: precise-object-edit. Edit this existing transparent blue-trousers front-kick skill icon. Keep its exact blue/orange-brown palette, coarse crisp pixel style, two legs/two boots, rightward horizontal kicking leg, black coat fragment, no extra objects, true RGBA transparency. Change ONLY composition/spacing: center the complete leg gesture and make its occupied bounding box approximately SQUARE with uniform blank padding ~17% of canvas on every side. Current visible figure is too far right and down and much wider than tall. Move it LEFT and UP slightly; shorten the extended blue thigh/shin by about10% and lengthen the supporting lower-leg downward about10% so total occupied WIDTH and HEIGHT are equal, WITHOUT stretching boots or increasing detail. Maintain natural knee geometry and clear kick shape. Target visible bounds roughly x17%..83%,y17%..83%, with the raised boot fully inside, complete planted boot inside. Even left/right and top/bottom padding, never crop. At 56x56 display the silhouette should be bold and immediately readable. Keep 2-3 simple tones per material and about1px logical outline. No blurry fringe/gray halo, no stray low-alpha marks in blank space, no border/text/FX/shadow. Only transparent background around the two-leg gesture. Single full-color icon, no variants. Requested logical source112x112, actual source dimensions may differ.
```

프롬프트3:

```text
Edit this icon ONLY to center its complete foreground BOUNDING BOX. Keep exact same leg/boot shapes, same size, colors, coarse pixel clusters and 1254x1254 transparent canvas. Do NOT lengthen or shorten any leg this time. Move the ENTIRE foreground exactly32 pixels LEFT and67 pixels UP as one rigid object. Current visible bbox (224,303)-(1093,1085); desired bbox after translation (192,236)-(1061,1018). This gives matched LEFT/RIGHT blank padding about193px, matched TOP/BOTTOM blank padding236px. Preserve all geometry and color. No new marks, no smoothing, no clipping. Transparency outside subject must be alpha0, no gray halo or stray marks. This is centered by full silhouette bounds including BOTH boots, NOT by center of mass. No frame, label, FX or background.
```

프롬프트4:

```text
Precise layout correction ONLY. Keep this exact two-leg front-kick pixel icon with the same shape, size and color. Keep the 1254x1254 square transparent RGBA canvas. Do not resize subject, stretch legs or change boots. Shift the whole subject UP38 pixels, with NO horizontal motion. Current painted bbox (196,281)-(1059,1047); target bbox (196,243)-(1059,1009). BOTH top and bottom blank margins should be about244 pixels. Left/right blank margins about196px are already balanced. Foreground must be centered by complete visible bounding box; not center of mass. Remove no foreground parts. Alpha outside foreground should be0, no stray speckles/halo. No frame, shadow, labels or additional imagery.
```

### 스킬 메뉴

선정 생성 원본: `C:/Users/KimHyeongJin/.codex/generated_images/01a0fa9f-7587-73e2-a85d-be95e68d307f/exec-082833b0-82b8-4a97-b70c-f0a78ab456fa.png`

프롬프트1:

```text
Use case: stylized-concept.
Asset type: one standalone Skills menu icon for a fantasy ActionRPG, genuine transparent RGBA PNG.
Input images: Image1 current party menu icon and Image2 current exit menu icon are STYLE/PALETTE REFERENCES ONLY. Match their warmly lit brushed-gold metal, dark navy enamel, restrained crimson and sapphire accents, illustrated beveled fantasy-game finish. Do not copy their circular crest/border, people, crown or doorway.
Primary subject: a very simple readable SKILL TREE / ABILITY emblem, five chunky gold-rimmed faceted ability nodes connected by thick gold branches. One lower central navy diamond root connects upward to one large central sapphire diamond with a clear four-point gold energy star inset; the central node branches to THREE upper nodes evenly spaced left, center, right (crimson, pale gold, sapphire). Exactly five nodes. Connections must be visibly attached, orderly tree structure, no tangled loops. Overall occupied silhouette approximately square and centered. Visually substantial gem nodes and broad connectors, restrained painterly shading, very few internal details. No decorative floating particles. A general abilities concept, not a kick or a specific weapon.
Composition: isolated foreground emblem only; leave equal clear transparent padding on all FOUR sides, at least6.25% canvas width (8 logical pixels for128x128). Full nodes and connectors inside; no clipping. Subject should occupy about80% of canvas and have balanced visual density beside existing party/exit icons when shown128x128. Broad primary outlines at least4px and distinct spaces at least6px on logical128px design; readable even at48/56/96px. Use a logical128x128 square art layout (actual output dimensions may differ).
No enclosing border, frame, circular backing plate, background tile or background painted behind tree. Navy color only INSIDE the individual nodes, with empty transparent spaces between branches. True alpha0 outside the subject, clean edges without gray halo or stray translucent marks. No text, letters, numbers, labels, key binding, lock, cooldown, arrows, clouds, floor, cast shadow or grayscale variant. Single full-color production icon. Keep existing reference palette and illustrated gold/enamel material style; no modern thin-line/vector network diagram.
```

프롬프트2:

```text
Use case: precise-object-edit. Keep this exact five-node fantasy skill-tree menu icon: same five colored gems, same gold-star details, same connections, proportions, warm gold and navy/crimson/sapphire palette, same illustrated material finish. Change ONLY layout by uniformly shrinking the COMPLETE foreground emblem to about84% of its current size and centering it by its complete bounding box within the SAME square transparent RGBA canvas. No stretching. Leave at least10% perfectly clear transparent padding on EACH side, preferably equal top/bottom and left/right margins. Do not crop anything. Foreground gold connectors remain substantial. Alpha0 outside the complete emblem, no stray speckles, no gray fringe. No enclosing frame, background, text, shadow, label or new objects. Logical128x128, actual canvas dimension may remain native. A transparent icon with FIVE linked ability nodes.
```

## 제안 커밋 로그

* 스킬 UI 앞차기와 스킬 메뉴 아이콘 제작
  * 최신 앞차기 색상 기반 하체 실루엣 아이콘 추가
  * 기존 메뉴 색감의 별도 능력 분기 아이콘 추가
  * 실제1254×1254 규격·축소 가독성·알파 및 여백 제약 기록

위는 변경 요약용 로그이며 실제 Git 커밋을 수행했다는 뜻이 아니다.
