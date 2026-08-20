# VEDA RS-485 경보 시스템 — Slave #2

이 프로젝트는 **3개짜리 시스템의 일부**다. 전체 설계·테스트 규칙은 Master 쪽 문서에 있다:
`C:\Users\3-10\STM32\Rs485_send_demo\CLAUDE.md`

| 경로 | 역할 |
|---|---|
| `C:\Users\3-10\STM32\Rs485_send_demo` | Master (스케줄러, RPi 중계) |
| `C:\Users\3-10\STM32\Rs485_receive_demo` | Slave #1 — `MY_SLAVE_ID=1` → CH1, CH2 |
| `C:\Users\3-10\STM32\rs485_receive_2` | **Slave #2** (이 폴더) — `MY_SLAVE_ID=2` → CH3, CH4 |

## !! 두 Slave 프로젝트는 `MY_SLAVE_ID` 한 줄만 다르다

한쪽을 고치면 **반드시 다른 쪽에도 같은 수정을 복사할 것.** 확인 방법:

```sh
diff -r Rs485_receive_demo/Core rs485_receive_2/Core
# -> veda_config.h 의 MY_SLAVE_ID 한 줄만 나와야 정상
```

`MY_SLAVE_ID` 는 이제 `main.c` 가 아니라 **`Core/Inc/veda_config.h`** 에 있다.

## 주석은 헤더에만 쓴다

설명·근거·경고는 전부 `.h` 에 두고 `.c` 에는 코드만 둔다(`main.c` 의 CubeMX `USER CODE`
마커는 예외). 이유와 규칙은 Master 문서의 같은 절에 있다.

## 파일 배치

`main.c` 에는 **CubeMX 가 생성하는 것만** 남아 있다. 나머지는 `veda_*` 모듈이고,
`main()` 은 `USER CODE` 블록에서 `channel_hardware_init()` 을,
`StartDefaultTask()` 는 `veda_supervisor_run()` 을 부르기만 한다.

| 모듈 | 내용 |
|---|---|
| `veda_config.h` | **`MY_SLAVE_ID` 를 비롯한 모든 설정 상수와 그 근거** |
| `veda_channel.*` | 채널↔하드웨어 대응표, `apply_channel_state()` (등급별 정책이 모인 곳) |
| `veda_rs485.*` | 프레임 조립·해석, DE 방향 제어, ACK 송신 |
| `veda_strip.*` | NeoPixel 줄 조명 (ISR 이 예약 → 태스크가 송신) |
| `veda_debug.*` | USART2 로그 + ISR→태스크 로그 전달 + `[loop]` 주기 실측 |
| `veda_isr.*` | USART1 수신·오류 콜백 |
| `veda_supervisor.*` | 부팅 절차(LD2 점멸·배너·스트립 초기화)와 메인 루프 |
| `neopixel.*` | WS2812 드라이버 (이번 분리 전부터 있던 모듈) |

## 동작

Master 가 RS-485(USART1)로 보낸 `S<슬레이브>:CH<채널>:R<위험도>` 한 줄을 받는다.

- 버스를 공유하므로 남의 프레임도 들어온다 → `MY_SLAVE_ID` 가 다르면 버린다
- 고정 9자 형식. 등급은 `0/1/2` 만 받고 그 밖의 값은 `VERDICT_BAD_RISK` 로 **거절**한다
  (임의로 NONE 이나 DANGER 로 바꿔 적용하지 않는다 — 회선 오류가 오동작이 되면 안 된다)
- GPIO 는 ISR 에서 바로 쓴다(빠르다). ACK 송신·로그·NeoPixel 은 블로킹이라 태스크가 맡는다
- ACK 슬롯은 **하나뿐**이다(`veda_rs485_ack_offer()`). 앞 ACK 가 안 나갔으면 명령은 적용하되 ACK 는 버린다.
  Master 가 ACK 를 받아야 다음을 보내므로 정상 운용에서는 겹치지 않는다

```
등급       줄 조명   경광등   부저
NONE       초록      OFF      OFF
WARNING    노랑      ON       OFF
DANGER     빨강      ON       ON
```

이 표는 Master 의 `risk_to_status()` 와 **반드시 같아야 한다.** 정책을 바꾸면 양쪽을 함께 고칠 것.

## 링크 두절 failsafe 는 **의도적으로 없다** (fail-loud)

Master 가 죽거나 선이 끊기면 마지막 상태를 그대로 유지한다. 줄 조명은 WS2812 라 색을
래치하므로 회선이 조용해져도 꺼지지 않는다(NONE 이면 초록이 계속 켜져 있다).

"일정 시간 뒤 소등"(fail-silent)도 구현해 봤다가 되돌렸다 — 진짜 위험이 진행 중인데 선이
끊기면 경보까지 사라지기 때문이다. **놓치는 쪽이 더 나쁘다고 판단했다.**

대가: 링크가 끊긴 동안 경광등은 현재 위험도가 아니라 마지막 명령의 잔상이고, 끄려면 전원을
뽑아야 한다. 다만 링크가 돌아오면 Master 의 2초 주기 리프레시가 참값을 다시 밀어 넣는다.

## 실기 관찰

USART2 = ST-Link VCP, **115200 8N1**. 수신할 때마다 판정을 찍는다:

```
RX "S2:CH3:R2" -> APPLIED (relay+buzzer+strip)
RX "S1:CH1:R2" -> dropped: another slave
RX "A1:CH1:R0" -> dropped: bad format      ← 옆 Slave 의 ACK를 엿들은 것. 정상이다
```

부팅 시 LD2 를 `MY_SLAVE_ID` 번 깜빡여 이 보드가 몇 번으로 구워졌는지 알려 준다.
