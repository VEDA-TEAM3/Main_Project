#!/usr/bin/env python3
"""테스트가 진짜 버그를 잡는지 확인한다 (뮤테이션 검사).

    python3 mutation_check.py        # tools/ 에서

펌웨어 소스나 상수를 일부러 망가뜨린 사본을 만들고, 그 상태로 호스트 테스트를 돌린다.
테스트가 실패하면 그 버그를 잡을 수 있다는 뜻이고, **통과하면 검사에 구멍이 있다는 뜻**이다.

왜 필요한가:
  "테스트가 전부 통과했다"는 두 가지를 구분하지 못한다 -- 코드가 옳은 것과, 테스트가
  아무것도 안 보고 있는 것. 이 스크립트가 그 둘을 가른다.

!! 변형이 실제로 적용됐는지 반드시 확인할 것.
   손으로 sed 를 돌리다가 여러 줄 패턴이 매칭되지 않아 파일이 그대로였는데, 테스트가
   통과하는 것을 보고 "구멍이 있다"고 잘못 판단한 적이 있다. 아래 apply() 는 치환이
   일어나지 않으면 그 자리에서 에러를 낸다.
"""

import pathlib
import shutil
import subprocess
import sys
import tempfile

TOOLS = pathlib.Path(__file__).resolve().parent
INC = TOOLS.parent / "Core" / "Inc"

# (설명, 대상 파일, 찾을 것, 바꿀 것)
MUTANTS = [
    ("ACK 검증을 건너뛰고 무조건 성공 처리",
     "sched_dispatch.inc",
     "ok = wait_for_slave_ack(channel_index, risk_level);",
     "ok = 1U;"),

    ("송신 실패해도 applied 를 갱신",
     "sched_dispatch.inc",
     "  if (ok != 0U)\n  {\n    channel_ctrl[channel_index].applied_risk = risk_level;",
     "  if (1)\n  {\n    channel_ctrl[channel_index].applied_risk = risk_level;"),

    ("커서를 전진시키지 않음 (라운드로빈 파괴)",
     "sched_dispatch.inc",
     "sched_cursor = (uint8_t)((ch + 1U) % CHANNEL_COUNT);",
     ""),

    ("DANGER 우선 패스를 부르지 않음",
     "sched_dispatch.inc",
     "ch = pick_pending(1U);",
     "ch = SCHED_NO_CHANNEL;"),

    ("실패한 채널의 백오프를 무시",
     "sched_select.inc",
     "if (channel_ctrl[ch].retry != 0U && idle < SCHED_RETRY_BACKOFF_MSEC)",
     "if (0)"),

    ("주기 리프레시를 아예 하지 않음",
     "sched_select.inc",
     "if (danger_only == 0U && idle >= SCHED_REFRESH_MSEC)",
     "if (0)"),

    ("리프레시가 DANGER 전환을 밀어냄 (우선순위 역전)",
     "sched_select.inc",
     "if (danger_only == 0U && idle >= SCHED_REFRESH_MSEC)",
     "if (idle >= SCHED_REFRESH_MSEC)"),
]

# 상수를 되돌리는 회귀 검사 (테스트 파일 자체를 고친다)
CONST_REGRESSIONS = [
    ("재시도 백오프 500 -> 100ms (굶주림 버그 복원)",
     "#define SCHED_RETRY_BACKOFF_MSEC 500U",
     "#define SCHED_RETRY_BACKOFF_MSEC 100U"),
    ("스케줄러 틱 20 -> 60ms",
     "#define SCHED_TICK_MSEC 20U",
     "#define SCHED_TICK_MSEC 60U"),
    ("주기 리프레시 2000 -> 20000ms",
     "#define SCHED_REFRESH_MSEC 2000U",
     "#define SCHED_REFRESH_MSEC 20000U"),
]

TESTS = ["test_sched_select.c", "test_ack_parse.c", "test_sched_loop.c"]


def apply(text, find, replace, what):
    """치환이 실제로 일어났는지 확인한다. 안 일어나면 결과 전체가 무의미하다."""
    if find not in text:
        sys.exit(f"!! 뮤테이션 실패: '{what}' 의 패턴을 찾지 못했다.\n   찾던 것: {find!r}\n"
                 f"   (소스가 바뀌었다면 이 스크립트의 MUTANTS 도 함께 고칠 것)")
    return text.replace(find, replace, 1)


def run_suite(inc_dir, tools_dir):
    """세 테스트를 모두 돌려 하나라도 실패하면 (False, 메시지) 를 준다."""
    for test in TESTS:
        binary = pathlib.Path(tempfile.gettempdir()) / (test + ".bin")
        build = subprocess.run(
            ["gcc", "-w", f"-I{inc_dir}", str(tools_dir / test), "-o", str(binary)],
            capture_output=True, text=True)
        if build.returncode != 0:
            return False, f"{test} 빌드 실패"
        run = subprocess.run([str(binary)], capture_output=True, text=True)
        if run.returncode != 0:
            last = (run.stderr.strip().splitlines() or ["(메시지 없음)"])[-1]
            return False, f"{test}: {last}"
    return True, ""


def main():
    survived = []
    print("=" * 78)
    print("뮤테이션 검사 — 일부러 망가뜨린 코드를 테스트가 잡아내는가")
    print("=" * 78)

    for desc, filename, find, replace in MUTANTS:
        with tempfile.TemporaryDirectory() as tmp:
            tmp = pathlib.Path(tmp)
            for inc in INC.glob("*.inc"):
                shutil.copy(inc, tmp / inc.name)
            target = tmp / filename
            target.write_text(
                apply(target.read_text(encoding="utf-8"), find, replace, desc),
                encoding="utf-8")

            caught, msg = run_suite(tmp, TOOLS)
            caught = not caught
            print(f"  {'[검출]' if caught else '[생존]'} {desc}")
            if caught:
                print(f"          └ {msg}")
            else:
                survived.append(desc)

    print()
    print("=" * 78)
    print("상수 회귀 검사 — 값을 되돌리면 테스트가 실패해야 정상")
    print("=" * 78)

    loop_src = (TOOLS / "test_sched_loop.c").read_text(encoding="utf-8")
    for desc, find, replace in CONST_REGRESSIONS:
        with tempfile.TemporaryDirectory() as tmp:
            tmp = pathlib.Path(tmp)
            mutated = tmp / "test_sched_loop.c"
            mutated.write_text(apply(loop_src, find, replace, desc), encoding="utf-8")
            binary = tmp / "bin"
            subprocess.run(["gcc", "-w", f"-I{INC}", str(mutated), "-o", str(binary)],
                           capture_output=True, text=True, check=True)
            run = subprocess.run([str(binary)], capture_output=True, text=True)
            caught = run.returncode != 0
            print(f"  {'[검출]' if caught else '[생존]'} {desc}")
            if caught:
                last = (run.stderr.strip().splitlines() or ["?"])[-1]
                print(f"          └ {last}")
            else:
                survived.append(desc)

    print()
    if survived:
        print(f"!! 살아남은 뮤턴트 {len(survived)}개 -- 검사에 구멍이 있다:")
        for s in survived:
            print(f"   - {s}")
        return 1

    print(f"모든 뮤턴트 검출됨 ({len(MUTANTS)} 코드 + {len(CONST_REGRESSIONS)} 상수)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
