# Control-Server
> 관제 서버 최적화 과정 및 지표를 정리한 마크다운입니다.

# IAggregate

### TimeWindowAggregator

#### 성능 지표
```bash
[2026-07-21-21:16:16] Aggregator - Success: 최근 5000ms 지표 - 
push() 6078회,
윈도우 마감 50회,
평균 락 보유시간 71.95us
```

---

### TimeWindowAggregatorV2

#### 성능 지표
```bash
[2026-07-21-21:20:52] Aggregator - Success: 최근 5000ms 지표 - 
push() 6188회,
윈도우 마감 49회,
평균 락 보유시간 2.92us
```

#### 개선 사항
1. 콜백을 락 밖에서 호출: 다른 채널의 `push()`가 더 이상 파이프라인 처리 시간만큼 블로킹되지 않음
2. `std::move`로 꺼내기: 윈도우 마감 시 슬롯에서 `flushFrames`로 옮길 때 복사 대신 `move` (어차피 슬롯을 비울 거라 안전)
3. `std::unordered_map` → `std::vector<std::optional<TopViewFrame>>`: 채널 수가 고정이라 해싱 없이 `channelId`로 바로 인덱싱

### 이전 버전과의 비교

| 지표 | TimeWindowAggregator | TimeWindowAggregatorV2 | 변화량 |
| :--- | :--- | :--- | :--- |
| 평균 락 보유 시간 | **71.95 us** | **2.92 us** | **95.9% 감소** |
| `push()` 호출 횟수 | 6,078회 | 6,188회 | 1.8% 증가 |
| 윈도우 마감 횟수 | 50회 | 49회 | 거의 동일 |
| 측정 구간 | 5,000 ms | 5,000 ms | 동일 |

---