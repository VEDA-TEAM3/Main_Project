#!/usr/bin/env python3
"""관제 서버(Raspberry Pi) 콘솔.

사용자에게 채널 번호를 입력받아 Master STM32의 USART6로 한 줄씩 보내고,
Master가 돌려주는 응답을 그대로 화면에 보여준다.
(Master의 USART1은 Slave와 연결된 RS-485 회선이므로 건드리지 않는다.)

배선 (RPi GPIO UART <-> Master USART6):
    RPi TXD (GPIO14, 8번 핀)  ->  Master PC7 (USART6_RX)
    RPi RXD (GPIO15, 10번 핀) <-  Master PC6 (USART6_TX)
    RPi GND (6번 핀)          <->  Master GND
    ※ RPi에서 시리얼 콘솔을 끄고 UART를 켜 둘 것:
      raspi-config -> Interface Options -> Serial Port
      (login shell: No / serial hardware: Yes)

사용법:
    python3 rpi_console.py                  # 기본 /dev/serial0 (GPIO UART)
    python3 rpi_console.py /dev/ttyUSB0     # USB-시리얼 어댑터를 쓰는 경우

필요 패키지: pip3 install pyserial
"""

import sys
import threading

import serial

DEFAULT_PORT = "/dev/serial0"
BAUDRATE = 115200
SUPPORTED_CHANNELS = ("1", "3")


def print_master_replies(port: serial.Serial) -> None:
    """Master가 보내는 응답을 계속 읽어 출력한다."""
    while True:
        line = port.readline()
        if not line:
            continue
        print("[master] " + line.decode("ascii", errors="replace").strip())


def main() -> int:
    port_name = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_PORT

    with serial.Serial(port_name, BAUDRATE, timeout=0.5) as port:
        reader = threading.Thread(target=print_master_replies, args=(port,), daemon=True)
        reader.start()

        print(f"연결: {port_name} @ {BAUDRATE}bps")
        print("제어할 채널 번호를 입력하세요 (1 또는 3). 종료는 q 또는 Ctrl+C.")

        while True:
            try:
                channel = input("channel> ").strip()
            except (EOFError, KeyboardInterrupt):
                print()
                return 0

            if channel in ("q", "quit", "exit"):
                return 0
            if channel not in SUPPORTED_CHANNELS:
                print("1 또는 3만 입력할 수 있습니다.")
                continue

            # Master는 한 줄 단위로 명령을 해석하므로 개행을 꼭 붙인다.
            port.write((channel + "\n").encode("ascii"))


if __name__ == "__main__":
    sys.exit(main())
