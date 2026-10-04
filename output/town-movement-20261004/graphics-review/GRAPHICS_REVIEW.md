# 이동 모션 머리 외곽 독립 검토·개선

작성: 2026-10-04 / 그래픽 담당

## 결과

총괄이 이미 수정한 현재 Assets의 walk/run을 기준으로 독립 검토했다. 걷기 머리에 idle보다 부드러운 명암 혼합과 밝은 회색·보라색 외곽 띠가 남고, 달리기 일부 프레임은 더 회색으로 보여 추가 개선했다. idle은 변경하지 않았다.

생산용 수정 PNG:

- `output/town-movement-20261004/graphics-review/player_walk.png`:1983×793 RGBA,5×2,10프레임.
- `output/town-movement-20261004/graphics-review/player_run.png`:1792×512 RGBA,7×2,14프레임.

두 PNG는 백업이 아닌 최종 개선 산출물이다. 기존 소스 Assets 반영 상태는 아래 설치 기록에 기재한다. 총괄의 기존 `output/town-movement-20261004/REPORT.md`와 기존 조립 산출물은 수정하지 않았다.

## 처리 방법과 근거

built-in image_gen에 현재 walk/run을 편집 대상으로, 현재 idle을 머리 색상·픽셀 스타일 참조로 제공했다. 생성본은 walk1984×793/run2172×724로 원본과 다르고 몸·얼굴 및 자세 차이가 생겨 전체 시트를 사용하지 않았다.

작업용 C# 조립기 `work/movement-hair-review/AssembleHair.cs`로 생성 머리 색상을 원래 프레임의 흰색 머리 코어 좌표에 대응시켰다. 원래 머리 영역 안의 가장 큰 4방향 연결된 차가운 색 영역만 선택해 피부 경계·눈의 분리된 흰색 부분·몸을 제외했다. 생성본의 피부색/투명 샘플은 사용하지 않고 기존 샘플로 대체했다. 생성 실루엣을 가져오거나 몸을 리샘플링하지 않았다.

idle에서 불투명 머리 색을 명도 구간별로 관찰해 추출한7색 공통 팔레트(RGB):

`(55,51,113), (105,103,161), (130,127,184), (161,159,207), (192,191,225), (225,224,238), (248,248,248)`

연속 회색 명암을 이 팔레트에 맞춰 단계화했다. 기존 투명 배경과 인접한1원본픽셀 외곽은 어두운 보라색으로 정리했다. 선택된 머리 영역의 alpha<96 외곽만 투명하게 제거하고 그 외 원래 알파를 보존했다. 기존 불투명 머리 가시영역을 늘리거나 깎지 않았다. 이 방식은 기존 좌표/몸을 보존하면서 밝은 회색 테두리와 프레임마다 달라지는 회색 계열을 줄이기 위한 것이다.

초기 조립에서 걷기의 피부 경계색4픽셀까지 선택된 것을 정적 비교로 발견했고, 따뜻한 경계색 제외와 연결 영역 선택을 추가해 최종본에서는0으로 만들었다.

## 정적 확인

아래 비교 기준은 이번 작업 시작 시점의 **총괄 수정 후 현재 Assets**다. 그 이전 원본에 대한 복원이나 비교를 뜻하지 않는다.

| 항목 | 걷기 | 달리기 |
|---|---:|---:|
| 전체 RGBA 변경 픽셀 |49,460|19,584|
| 머리 ROI 밖 변경 |0|0|
| 선택한 머리 연결 영역 밖 변경 |0|0|
| 기존 alpha≥160의 알파 변경 |0|0|
| 투명 배경에 새 불투명 픽셀 추가 |0|0|
| 따뜻한 피부 경계색 변경 |0|0|
| 어두운 눈/윤곽색(최대 RGB≤75) 변경 |0|0|
| alpha<96 머리 외곽 제거 |740|96|

24개 모든 프레임의 선택된 변경 머리 RGB가 공통7색 팔레트에 속함을 확인했다. 피부와 분리된 흰색 눈 픽셀은 연결 영역 밖이라 보존했다. 원본 치수와 시트 배치, 몸·팔·다리·부츠·발 기준 픽셀, 기존 불투명 실루엣은 보존됐다.

모든 머리를 nearest 확대해 보았고, 24개 프레임 전체를 현재 animations.ini의 표시 크기(walk192×198/run202×202)로 정적 비교했다. 대표 idle/walk/run은 같은 바닥선과 밝고 어두운 바탕에서 기존 anchor를 적용해 비교했다. 밝은 회색 링이 줄고 흰색/라벤더의 단계가 idle에 가까워졌다. 검토 미리보기는 생산용 시트나 백업이 아니다.

- `work/movement-hair-review/heads-before.png`:수정 전 머리 정적 비교.
- `work/movement-hair-review/all-heads-after.png`:모든 수정 머리 확대.
- `work/movement-hair-review/all-configured-size.png`:모든 포즈의 기존 설정 크기 비교.
- `work/movement-hair-review/ground-and-contrast.png`:대표 포즈의 같은 바닥선/명암 바탕 비교.
- `work/movement-hair-review/inspection-final.json`:프레임별 실측과 보존 비교 기록.

## 남은 한계와 연계

실제 애니메이션 재생·좌우 반전·DPI·런타임 필터·게임 배경 합성은 확인하지 않았다. 시간에 따른 머리 흔들림이나 밝기 깜빡임이 없다고 단정하지 않는다. 프레임별 포즈와 명암 면적은 다르므로 공통 팔레트 사용이 평균 밝기의 완전 동일을 뜻하지 않는다.

원래 불투명 실루엣과 spike 위치를 보존해 기존 머리 모양 차이는 남는다. 가장 큰 연결 영역에서 떨어진 머리 잔여 조각은 피부/눈 보존을 우선해 수정하지 않았다. 시트 전체의 완전한 알파 무잔여 정리를 했다고 보고하지 않는다. 원본 단계에서 이미 달랐던 idle/walk/run 머리 비율과 캐릭터 점유율은 이번 범위에서 재조정하지 않았다.

animations.ini의 열·행·프레임 수·순서·재생 속도·render 크기·anchor는 변경하지 않았다. 후속 총괄/클라이언트 확인은 소스 PNG가 다음 승인 빌드에 포함되는지와 실제 재생 중 회색 테두리·전환 깜빡임 여부다. 실행 중 메모리나 빌드 출력 Assets는 이번에 교체하지 않는다. 다른 담당 채팅에는 메시지를 보내지 않았다.

빌드·기능 테스트·게임/서버 실행·백업·실제 stage/commit/push는 수행하지 않았다. 작업용 이미지 조립과 픽셀/메타데이터 관찰만 수행했다. 게임 C++/설정/서버 코드는 수정하지 않았다.

## 해시

- `player_walk.png` 작업 시작 SHA-256:`3b154bb1f8fa017a4e62529aec43e4e2101c9e851d24db23db382a6330d47457`
- `player_walk.png` 최종 SHA-256:`daea65ed41138bca16766860b284a715b53efb6522d967ff0d7c642fa148e046`
- `player_run.png` 작업 시작 SHA-256:`67c96432b9ded58c8388b19a1de2209151f87901297589541236bf3c1a60dcf5`
- `player_run.png` 최종 SHA-256:`631ce30e6dba02ba241fd4d9fc837f4845300a1bf7cd0d5f4cd026763e0ad175`

## 생성 프롬프트

방식: imagegen 스킬 / built-in image_gen, transparent_background=true.

### walk

생성 원본:`C:/Users/KimHyeongJin/.codex/generated_images/01a0fa9f-7587-73e2-a85d-be95e68d307f/exec-52ded843-8301-4646-8d90-d9300734cfcf.png`

```text
Use case: precise-object-edit. Input image1 is the EXISTING WALK atlas edit target, image2 is player_idle HEAD PIXEL STYLE AND PALETTE REFERENCE ONLY. Repair ONLY white hair in each of TEN frames. Canvas1983x793, grid5columns x2rows. Keep frame ordering/coordinates, exact original solid hair silhouette, face/skin/eyes, ALL black coat/arms/legs/blue trousers/red boots unchanged. Do not move or resize characters. No new motion.
Current walk hair has soft gray rim and blurry blended shades. Match idle's hard angular PIXEL CLUSTERS: white main light masses, light lavender stepped internal shade, dark muted violet contour. Restrained coherent 5-7color hair palette identical across all frames. No painterly gradient, smooth white noodles, blur or antialias gray rings. Remove the bright gray outside halo, replacing visible OUTERMOST gray hair edge with thin dark desaturated purple consistent with idle. Preserve the original existing opaque silhouette and spike shape; keep edge thickness about1native pixel, not broad dark band. Retain warm skin and black eye pixels. All white hair interiors have crisp chunky stepped highlights like idle, not soft gradients. Use each original frame pose independently, do not replace all heads with identical pose or tilt. No crop, no label, no frame, no background, no extra objects. Real transparent RGBA outside, alpha0 empty background. Return one corrected walk sprite sheet only. Preserve EVERYTHING except hair edge colors and hair pixel shading.
```

### run

생성 원본:`C:/Users/KimHyeongJin/.codex/generated_images/01a0fa9f-7587-73e2-a85d-be95e68d307f/exec-cbf1f925-fe69-431b-8b0d-5a970413de2b.png`

```text
Use case: precise-object-edit. Input image1 is the EXISTING RUN atlas edit target, image2 is player_idle HEAD PIXEL STYLE/PALETTE REFERENCE ONLY. Repair ONLY hair of FOURTEEN frames. Canvas1792x512, grid7columns x2rows. Preserve the original position and solid silhouette of each individual head and hairstyle, warm face/skin and black eyes, every body/arm/coat/leg pose, frame ordering, feet and cell dimensions. No motion regeneration or recentering.
Remove the thin pale-gray outer ring/halo around hair: thin dark muted lavender/violet silhouette edge consistent with idle instead, never a thick gray band. Hair interior is predominantly cool WHITE with angular light-lavender and medium lavender shadow clusters matching idle, crisp stepped pixel edges, no smooth gradients or blurry wisps. Use the same coherent idle-based hair palette in ALL14frames: no warmer gray head in later frames, no bright cyan contour. Do NOT change head rotation or shape. About1native pixel contour, same white highlight appearance from frame to frame. Keep all original opaque silhouette coverage, no removal of solid spikes. Transparent RGBA, alpha0 empty space, no glow or stray translucent marks. No text, labels, frame, background, extra object, scale change or crop. Return ONE complete transparent run atlas with only hair color/edge repair, all rest intact.
```

## 제안 커밋 로그

* 걷기·달리기 머리 회색 외곽과 색상 일관성 개선
  * idle 기반 흰색·라벤더 팔레트와 얇은 보라색 윤곽 적용
  * 머리 영역만 반영하고 몸·피부·눈·불투명 실루엣·프레임 좌표 보존
  * 전체 프레임 정적 비교와 실행 미확인 한계 기록

실제 Git 커밋을 수행한 로그가 아닌 변경 요약이다.

## 설치 기록

클라이언트 소스 Assets의 아래 두 파일에 최종 PNG를 반영했고 산출물과 설치 파일의 SHA-256 일치를 확인했다. 반영 전 해시를 검사해 작업 중 다른 변경이 없는 경우에만 교체했다.

- `C:/Users/KimHyeongJin/source/repos/ActionRPGClient/ActionRPGClient/Assets/Images/Characters/player_walk.png`
- `C:/Users/KimHyeongJin/source/repos/ActionRPGClient/ActionRPGClient/Assets/Images/Characters/player_run.png`

현재 idle과 animations.ini의 해시가 반영 전과 동일함을 확인했다. 실행 중 메모리와 빌드 출력은 변경하지 않았다.
