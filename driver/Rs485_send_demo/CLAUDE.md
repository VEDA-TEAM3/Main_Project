# VEDA RS-485 경보 시스템 — Master

## 전체 구성 (프로젝트 3개가 한 시스템이다)

```
Raspberry Pi ──USART6(바이너리 27B)──▶ Master STM32 ──USART1/RS-485(ASCII)──▶ Slave ×2
  (관제 서버)  ◀──USART6(상행 19B)────                ◀──ACK──────────────────
```

| 경로 | 역할 | 핵심 |
|---|---|---|
| `C:\Users\3-10\STM32\Rs485_send_demo` | **Master** (이 폴더) | 스케줄러, RPi 중계 |
| `C:\Users\3-10\STM32\Rs485_receive_demo` | **Slave #1** | `MY_SLAVE_ID=1` → CH1, CH2 |
| `C:\Users\3-10\STM32\rs485_receive_2` | **Slave #2** | `MY_SLAVE_ID=2` → CH3, CH4 |

**두 Slave 프로젝트는 `MY_SLAVE_ID` 한 줄만 다르다.** 한쪽을 고치면 반드시 다른 쪽에도
같은 수정을 복사할 것 (diff 로 그 한 줄만 남는지 확인).

보드: STM32F401RE Nucleo + FreeRTOS(CMSIS-RTOS2). 채널 4개, Slave 한 대가 2채널 담당.
위험 등급은 `NONE(0) / WARNING(1) / DANGER(2)`.

## Master 구조 (`Core/Src/main.c`)

핵심 설계는 **접수와 실행의 분리**다.

- `ctrl_task` — 검증된 이벤트를 받아 `desired`(원하는 등급)만 적는다. 송신하지 않는다
- `sched_task` — **유일한 RS-485 송신자.** 20ms 틱마다 `desired != applied` 인 채널을
  하나씩 골라 내보내고 ACK 를 기다린다. DANGER 우선 → 라운드로빈 → 2초 주기 리프레시
- `rx_task` / `tx_task` / `hb_task` / `StartDefaultTask`(감시·로그)

ACK 를 받아야만 `applied` 를 갱신하므로 실패는 자동으로 재시도된다.

### 공유 `.inc` 파일 — 호스트 테스트와 같은 소스를 쓴다

`Core/Inc/` 의 `.inc` 는 컴파일 단위가 아니라 `main.c` 가 `#include` 하는 조각이다.
**추상화가 아니라 검증이 목적**이다 — 호스트 테스트가 같은 파일을 포함해 실기와 갈라지지
않게 한다. 각 파일 맨 위 주석에 "포함하는 쪽이 갖춰야 할 것(seam)" 목록이 있다.

| 파일 | 내용 | 대응 테스트 |
|---|---|---|
| `sched_select.inc` | `pick_pending()` — 이번 틱에 누구를 고르나 | `test_sched_select.c` |
| `sched_dispatch.inc` | `dispatch_channel()` / `sched_tick()` — 고른 뒤 상태 전이 | `test_sched_loop.c` |
| `ack_parse.inc` | ACK 줄 조립·해석 (ISR 문맥) | `test_ack_parse.c` |

## 테스트 (PC 에서, 보드 없이)

`tools/` 에서 gcc 한 줄. 프레임워크 없이 `assert` + `main()` 만 쓴다.

```sh
gcc -Wall -Wextra -I../Core/Inc test_sched_select.c -o test_sched_select && ./test_sched_select
gcc -Wall -Wextra -I../Core/Inc test_ack_parse.c   -o test_ack_parse   && ./test_ack_parse
gcc -Wall -Wextra -I../Core/Inc test_sched_loop.c  -o test_sched_loop  && ./test_sched_loop
```

`test_sched_loop.c` 는 가짜 RPi 가 33ms 마다 랜덤 명령을 던지고 가짜 Slave 가 응답하는
닫힌 루프 시뮬레이션이다. 가상 시계라 300시드 × 30초를 0.1초에 돈다. **랜덤이므로 시드를
고정**하며, 실패 시 `FAIL (seed=.. t=..ms)` 로 재현 정보를 찍는다.

스케줄러를 건드렸으면 **커밋 전에 세 개 다 돌릴 것.** 자세한 규칙은 `tools/TESTING.md`.

### 테스트가 진짜 보고 있는지 확인

```sh
python3 mutation_check.py     # 코드 뮤턴트 7 + 상수 회귀 3, 전부 검출되어야 정상
```

펌웨어를 일부러 망가뜨린 사본으로 테스트를 돌린다. "전부 통과"가 **코드가 옳은 것**인지
**테스트가 아무것도 안 보는 것**인지 가려 준다. 실제로 이 검사가 세 개의 구멍을 찾아냈다
(커서 전진 미검증, DANGER 우선 패스 호출 미검증, 리프레시 약화 미검출).

> **합격선을 검사 대상에서 파생시키지 말 것.** 리프레시 상한을 `SCHED_REFRESH_MSEC * 2` 로
> 두었더니 상수를 늘리면 합격선도 같이 늘어 검사가 무력해졌다. 지금은 절대값(4000ms).

## 절대 잊으면 안 되는 것들

**가짜 seam 은 '시간'도 모델링해야 한다.** 가짜 ACK 가 즉답하면 실패한 ACK 가 공짜가 되어
실기에서 가장 크게 밀리는 구간이 사라진다. 실제로 이것 때문에 굶주림 버그를 놓치고 있었다.

**`SCHED_RETRY_BACKOFF_MSEC` 는 `RS485_ACK_TIMEOUT_MSEC` 보다 충분히 커야 한다.**
같으면(둘 다 100ms) 실패 중인 DANGER 채널이 둘 이상일 때 서로 번갈아 도는 사이에 각자의
백오프가 저절로 지나가, 우선순위 패스가 영영 양보하지 않고 살아 있는 채널이 굶는다.
현재 500ms. 되돌리면 `test_sched_loop` 가 실패한다.

**Slave 에는 링크 두절 failsafe 가 없다(fail-loud, 의도된 선택).** 선이 끊기면 마지막
상태를 그대로 유지한다(NONE 이면 초록 줄조명이 계속 켜져 있다). 그래서 **어긋난 Slave 를
되돌리는 경로는 Master 의 2초 주기 리프레시 하나뿐**이다 — 리프레시를 약화시키는 변경은
곧 복구 능력을 없애는 것이다.

**프로젝트 경계를 넘는 상수는 양쪽을 함께 고쳐야 한다.**
`driver_protocol.h`(RPi 와 공유, 구조체 크기는 `_Static_assert` 로 못박음),
`CHANNELS_PER_SLAVE`, 등급→출력 매핑(Master `risk_to_status()` ↔ Slave `apply_channel_state()`).

```
등급       줄 조명   경광등   부저
NONE       초록      OFF      OFF
WARNING    노랑      ON       OFF
DANGER     빨강      ON       ON
```

## 실기 관찰

USART2 = ST-Link VCP, **115200 8N1**. 보드마다 COM 포트가 하나씩 생긴다.
Master 는 5초마다 `[stat]` 과 `[stack]` 을, Slave 는 수신할 때마다 `RX "..." -> 판정` 을 찍는다.

PowerShell 로 포트를 훑어 어느 게 Master 인지 찾는다 (`=== VEDA MASTER READY ===` 배너):

```powershell
$p = New-Object System.IO.Ports.SerialPort COM8,115200,None,8,one
$p.Open(); while($true){ if($p.BytesToRead){ Write-Host -NoNewline $p.ReadExisting() }; Start-Sleep -m 50 }
```

`[stat]` 에서 볼 것: `ack_ok`(증가해야 정상), `ack_to`/`retry`(0 근처),
`lat_dgr`(정상 운용 시 **36ms 근처**. 218ms 는 보드 한 대가 죽었을 때의 값이다).

COM 포트는 한 번에 한 프로그램만 쓸 수 있다 — 굽기 전에 터미널을 닫을 것.

## 남은 미검증

- 실제 틱 주기 (`sched_tick` 호출 수를 세면 5초에 250회여야 한다. 아직 계측 코드 없음)
- 늦은 ACK 처리 (`drain_ack_queue` — ISR/큐 영역이라 `.inc` 밖, 위험은 낮음)
- Slave 루프 실제 주기 (추정 16ms. 40ms 를 넘으면 지연 상한을 깬다)
