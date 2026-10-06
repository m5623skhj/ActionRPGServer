# 마을 타일 조합 샘플

마을 배경은 모두 `1536 x 1024` 크기의 타일로 제작되어 있습니다. 맵 에디터에서 각 타일의 `x` 좌표만 타일 너비만큼 증가시키면 가로로 이어 붙일 수 있습니다.

| 샘플 | 전체 크기 | 타일 배치 |
| --- | --- | --- |
| 소형 산촌 | `1536 x 1024` | `town_hillside.png` → `(0, 0)` |
| 중형 시장 마을 | `3072 x 1024` | `town_market_west.png` → `(0, 0)`, `town_market_east.png` → `(1536, 0)` |
| 대형 왕도 | `4608 x 1024` | `town_capital_west.png` → `(0, 0)`, `town_capital_central.png` → `(1536, 0)`, `town_capital_east.png` → `(3072, 0)` |

중형 시장 마을의 이미지 배치 예시는 다음과 같습니다.

```json
"images": [
  {
    "asset": "Images/Towns/town_market_west.png",
    "x": 0,
    "y": 0,
    "width": 1536,
    "height": 1024
  },
  {
    "asset": "Images/Towns/town_market_east.png",
    "x": 1536,
    "y": 0,
    "width": 1536,
    "height": 1024
  }
]
```

실제 서버 맵 정의는 [TownMapHillside.json](../Data/TownMapHillside.json),
[TownMapMarket.json](../Data/TownMapMarket.json), [TownMapCapital.json](../Data/TownMapCapital.json)에 있습니다.
배경 이미지의 경계가 통행 영역과 충돌 영역을 결정하지는 않으므로, 타일을 배치한 뒤 맵 에디터에서
이동 가능 영역과 출입구를 별도로 조정하면 됩니다. 위 이미지 경로는 클라이언트 Assets 기준이며
이미지를 서버 저장소에 설치하는 안내가 아닙니다.

서버는 실행 파일 옆 Data의 맵을 읽으므로 원본 수정 후 배포 데이터를 갱신하고 재시작해야 합니다.
마을 입장·맵 전환·패킷은 [Town 개발 가이드](../DEVELOPMENT.md),
전체 구성과 실행 전제는 [서버 README](../../../README.md)를 참고하세요.
