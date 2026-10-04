# 마을 이동 보정과 머리 외곽 수정

작성: 2026-10-04. 사용자 수정 지시에 따라 소스와 PNG에 반영했다. 게임 빌드·테스트·실행·Git 커밋·푸시는 하지 않았다. 기존 미커밋 작업은 보존했다.

## 이동 원인과 변경

* TownInstance는 Tick 처리 후 expires_after(50ms)로 다음 틱을 예약하면서 이동 계산에는 고정 0.05초를 사용했다. 콜백 지연과 처리 시간이 누적되면 실제 시간에 비해 서버 이동 거리가 작아진다.
* 다음 틱의 절대 시각을 유지하고, 실제 경과 시간으로 이동을 계산한다. 장시간 멈춤 이후에는 경과 시간을 최대 0.25초로 제한하고 지난 예약을 건너뛰어 즉각적인 틱 폭주를 피한다.
* 기존 클라이언트는 과거 서버 위치와 현재 예측 위치를 비교해 패킷 도착 때마다 오차의 35%를 즉시 이동시켰다.
* 새 마을 경로는 각 입력의 첫 응답에서 제한적인 도착 지연을 추정하고 서버 속도로 외삽한다. 입력 응답까지 걸린 시간에는 서버 틱/전송 대기도 포함되므로 순수한 RTT나 정밀한 시계 동기화라고 보지 않는다.
* 외삽은 최대 0.25초이며 맵 충돌 영역으로 제한한다. 작은 오차는 게임 프레임마다 지수적으로 분산해 반영하고 큰 차이는 즉시 보정한다. 기존 16단위 허용 오차와 200단위 즉시 보정 기준을 유지한다.
* 이전 입력/이전 틱 응답은 현재 마을 위치 보정에 사용하지 않으며 맵 전환에서 추정 상태를 초기화한다.
* 다른 플레이어가 정지하는 스냅샷도 기존 프레임 보간 경로로 처리한다. 큰 위치 변경에 대해서만 즉시 위치를 맞춘다.
* 마을 보정은 ReconcileTownGroundPosition으로 분리했고 기존 던전 ReconcileGroundPosition 호출과 동작은 보존했다.

Town의 예약/이동 상태는 기존 Town strand, 클라이언트 추정/보정 상태는 기존 게임 스레드에서만 변경한다. 패킷 계약이나 캐릭터 이동 속도 데이터는 바꾸지 않았다.

## 그래픽 변경

최종 반영 경로:

* C:/Users/KimHyeongJin/source/repos/ActionRPGClient/ActionRPGClient/Assets/Images/Characters/player_walk.png
* C:/Users/KimHyeongJin/source/repos/ActionRPGClient/ActionRPGClient/Assets/Images/Characters/player_run.png

imagegen 스킬과 built-in image_gen의 precise-object-edit 모드를 사용했다. 생성 원본은 각각 1984×793, 2171×724로 기존 시트 규격 및 몸 자세에 차이가 있어, 생성된 머리 색상만 기존 프레임 좌표에 맞춰 조립했다. C# 조립 코드는 artifacts/town-movement-20261004/PackGeneratedHair.cs에 있다. 원래 몸/발/따뜻한 피부색과 눈을 보존하도록 머리의 흰색·보라색 영역만 합성하고 낮은 알파 외곽을 정리했다. 생성본 전체를 기존 아틀라스로 교체하지 않았다.

* 걷기: 원래 1983×793, 5×2, 10프레임 유지. 머리 픽셀 54,929개 변경, 외곽 투명 픽셀 5,605개 정리.
* 달리기: 원래 1792×512, 7×2, 14프레임 유지. 머리 픽셀 18,514개 변경, 외곽 투명 픽셀 100개 정리.
* 픽셀 비교: 두 시트 모두 지정된 머리 영역 밖 변화 0, 원래 알파 160 이상인 불투명 실루엣의 알파 변화 0.
* animations.ini의 프레임 수, 크기, 앵커와 재생 속도는 이번에 변경하지 않았다.

원본 백업은 만들지 않았다. 생성 도구가 반환한 파일은 도구의 기본 생성 경로에 유지된다. 소스 Assets만 반영했으며 이미 실행 중인 프로그램의 메모리나 빌드 출력 Assets는 교체하지 않았다.

## 생성 프롬프트

아래 문장에서 걷기는 canvas=1983 by 793, grid=5 columns and 2 rows, EXACTLY 10 frames, 달리기는 canvas=1792 by 512, grid=7 columns and 2 rows, EXACTLY 14 frames를 사용했다. 입력은 각각 기존 player_walk.png와 player_run.png, transparent_background=true였다.

```text
Use case: precise-object-edit. Production sprite atlas color/alpha edge repair ONLY. The input is the edit target, not inspiration. Keep the EXACT original atlas canvas {canvas} pixels, {grid}, same cell positions and sizes, all poses in original order. Do not regenerate motion. Do not move, resize or recenter any figure. Preserve ALL original coat, arms, hands, pants, boots, face and interior hair pixels. Change ONLY the thin gray/blue/cyan fringe around the OUTER silhouette of the white hair in EVERY frame. Remove the bright gray halo on the outside, eliminating stray blue/cyan translucent pixels. Hair must remain white with existing light lavender internal shading, but its silhouette should have a restrained thin dark desaturated purple contour consistent across every frame. No thick gray ring or added dark band. Clean genuinely alpha-zero empty background with crisp pixel edges and no soft glow. Keep hair shape and exact pose, all dimensions, foot pivots, clothing palette, frame count and grid unchanged. No cropping, labels, borders, checkerboard, effects, weapons or new elements. Return ONLY the corrected original entire single transparent sprite sheet. This is a surgical fringe color/transparency repair, not a new illustration.
```

## 확인과 한계

선언/호출, 충돌 제한, 상태 초기화, strand 소유 및 변경 diff를 정적으로 검토했다. PNG 크기/알파/머리 영역 밖 픽셀 비교와 시각 확인을 수행했다. 실제 네트워크 지연, 방향 전환, 벽 접촉과 게임 화면에서의 결과는 실행하지 않아 미확인이다. 이동 수정 적용에는 클라이언트와 TownServer를 새 소스로 함께 빌드해야 한다.

## 제안 커밋 로그

* 마을 이동 보정과 이동 모션 머리 외곽 개선
  * 마을 서버 틱 지연 누적 방지 및 실제 경과 시간으로 이동 계산
  * 마을 입력 응답 지연을 반영한 제한적 외삽과 프레임별 위치 보정 추가
  * 다른 플레이어 정지 시 즉시 위치 변경 완화
  * 걷기·달리기 머리 외곽 정리와 원본 프레임·신체·발 위치 보존

제안 로그이며 실제 커밋과 푸시는 수행하지 않았다.
