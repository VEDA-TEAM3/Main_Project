# Compute-Server
> 연산 서버 최적화 과정 및 지표를 정리한 마크다운입니다.

---

# INetwork

### RtspClient

#### 성능 지표
```bash
[2026-07-21-15:57:12] Network - Success: 최근 5042ms 지표 - 프레임 25개,
평균 조립시간 73550.68us,
처리율 4.96fps,
recv() 호출/프레임 6.52회,
처리량 11.32KB/s
```

---

### RtspClientV2

#### 성능 지표
```bash
[2026-07-21-15:51:49] Network - Success: 최근 5075ms 지표 - 프레임 25개,
평균 조립시간 27211.47us,
처리율 4.93fps,
recv() 호출/프레임 2.04회,
처리량 11.23KB/s
```

#### 개선 사항
1. `syscall` 감소: `fillReadBuffer()`가 `64KB` 단위로 `recv()`를 한 번 호출해 사용자 공간 버퍼에 채우고, `readByte`/`readBytes`는 그 버퍼에서 소비<br> → 이전엔 패킷당 최소 3회의 `recv()` `syscall`이 있었지만, 이제 여러 패킷이 하나의 `recv()` 호출로 처리
2. 동적 할당 제거: `sockBuf_`(`64KB`), `rtpPacket_`(`65535B`)를 생성자에서 딱 1번만 `resize`<br> → 루프에서는 재할당도, `vector::resize`의 불필요한 `zero-init`도 없음
3. `TCP_NODELAY` / `SO_RCVBUF` 소켓 옵션 추가: `Nagle` 지연 제거, burst 수신 시 커널 드롭 방지

#### 이전 버전과의 비교

| 지표 | RtspClient | RtspClientV2 | 변화량 |
| :--- | :--- | :--- | :--- |
| `syscall` 호출 | **6.52회** | **2.04회** | **68.7% 감소** |
| 평균 조립시간 | **73,550.68 us** | **27,211.47 us** | **63.0% 단축** |
| 처리율 (`FPS`) | 4.96 fps | 4.93 fps | 거의 동일 |
| 처리량 | 11.32 KB/s | 11.23 KB/s | 거의 동일 |
| 측정 구간 (`25 frame`) | 5,042 ms | 5,075 ms | 거의 동일 |

---

# IMetadataSource

### RtspOnvifSource

#### 성능 지표
```bash
[2026-07-21-16:07:57] Source - Success: 최근 5019ms 지표 - 프레임 25개,
평균 큐 지연시간 54.98us,
처리율 4.98fps, 드랍 0개,
처리량 11.38KB/s
```

---

### RtspOnvifSourceV2

#### 성능 지표
```bash
[2026-07-21-16:04:36] Source - Success: 최근 5020ms 지표 - 프레임 25개,
평균 큐 지연시간 51.16us,
처리율 4.98fps, 드랍 0개,
처리량 11.38KB/s
```

#### 개선 사항
1. `std::queue` → 고정 크기 링버퍼로 변경
2. `drop-oldset` 정책 추가: 버퍼가 가득 차면 가장 오래된 프레임을 버림
3. 콜백에서 링 슬롯에 직접 `assign()`: 콜백 당 힙 할당 제거
4. `next()`는 `move` 대신 `copy`: `move`로 꺼내면 할당한 `capacity`가 사라져서 `copy`로 보존

#### 이전 버전과의 비교

| 지표 | RtspOnvifSource | RtspOnvifSourceV2 | 변화량 |
| :--- | :--- | :--- | :--- |
| 평균 큐 지연시간 | **54.98 us** | **51.16 us** | **6.9% 감소** |
| 처리율 (`FPS`) | 4.98 fps | 4.98 fps | 동일 |
| 프레임 드랍 | 0개 | 0개 | 동일 |
| 처리량 | 11.38 KB/s | 11.38 KB/s | 동일 |
| 측정 구간 (`25 frame`) | 5,019 ms | 5,020 ms | 거의 동일 |