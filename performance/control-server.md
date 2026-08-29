# Control-Server
> 관제 서버 최적화 과정 및 지표를 정리한 마크다운입니다.

## Zone 경계 안정화

`SpatialZoneMapper`는 객체를 재출력하지 않고 GID별 zone 선택 이력만 최대 5개 누락 윈도우 동안 유지한다.
33ms 집계 기준 약 165ms의 짧은 채널 누락에도 최근접 CCTV 히스테리시스가 초기화되지 않는다.

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
4. 채널별 최신 도착 스냅샷 유지: CCTV timestamp 중복·보정과 무관하게 단일 FIFO의 마지막 도착 프레임을 사용
5. `GridFuser` coast 복원: 미관측 트랙을 최대 5윈도우 동안 마지막 좌표로 유지해 채널 인계 시 UI 깜박임 방지

### 이전 버전과의 비교

| 지표 | TimeWindowAggregator | TimeWindowAggregatorV2 | 변화량 |
| :--- | :--- | :--- | :--- |
| 평균 락 보유 시간 | **71.95 us** | **2.92 us** | **95.9% 감소** |
| `push()` 호출 횟수 | 6,078회 | 6,188회 | 1.8% 증가 |
| 윈도우 마감 횟수 | 50회 | 49회 | 거의 동일 |
| 측정 구간 | 5,000 ms | 5,000 ms | 동일 |

---
