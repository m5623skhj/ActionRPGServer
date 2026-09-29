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

실제 서버 맵 정의는 `Data/TownMapHillside.json`, `Data/TownMapMarket.json`, `Data/TownMapCapital.json`에 있습니다. 배경 이미지의 경계가 통행 영역과 충돌 영역을 결정하지는 않으므로, 타일을 배치한 뒤 맵 에디터에서 이동 가능 영역과 출입구를 별도로 조정하면 됩니다.
