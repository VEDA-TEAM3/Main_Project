# 테스트가 진짜 버그를 잡는지 확인한다 (뮤테이션 검사) — PowerShell 판.
#
#     powershell -ExecutionPolicy Bypass -File mutation_check.ps1
#
# mutation_check.py 와 하는 일이 같다. Python 이 없는 Windows 에서 쓰라고 만든 것이므로
# 둘 중 하나만 고치면 갈라진다 -- MUTANTS 목록을 바꾸면 양쪽을 함께 볼 것.
#
# 펌웨어를 일부러 망가뜨린 사본으로 테스트를 돌린다. 테스트가 실패하면 그 버그를 잡을 수
# 있다는 뜻이고, 통과하면 검사에 구멍이 있다는 뜻이다.
#
# !! 치환이 실제로 일어났는지 반드시 확인한다. 패턴이 안 맞아 파일이 그대로인데
#    "통과했다 = 구멍이다"로 잘못 읽은 적이 있다. Apply 함수가 그 자리에서 멈춘다.

# !! 'Stop' 으로 두면 안 된다. PowerShell 5.1 은 native 명령(gcc, 테스트 실행 파일)이
#    stderr 에 한 줄이라도 쓰면 NativeCommandError 를 던져 스크립트를 통째로 중단시킨다.
#    이 검사는 '테스트가 실패하는 것'이 정상 동작이라 stderr 가 늘 나온다.
#    성공/실패는 아래에서 $LASTEXITCODE 로 직접 판정한다.
$ErrorActionPreference = 'Continue'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$Tools = $PSScriptRoot
$Inc   = Join-Path (Split-Path $Tools -Parent) 'Core\Inc'
$Tests = @('test_sched_select.c', 'test_ack_parse.c', 'test_ack_match.c', 'test_sched_loop.c')

# !! 빌드 결과물을 %TEMP% 에 두면 안 된다. Application Control(WDAC/AppLocker) 이 설정된
#    PC 에서는 임시 폴더의 실행 파일이 차단되어 "An Application Control policy has blocked
#    this file" 로 죽는다. tools\ 아래의 하위 폴더(.muttmp)도 마찬가지로 차단됐다.
#    반면 tools\ '바로 아래'는 허용된다 -- 손으로 빌드한 test_sched_loop.exe 가 거기서
#    돌기 때문에 확인된 사실이다. 그래서 .exe 는 tools\ 에 직접, 고정된 이름으로 만들고
#    (경로 기반 정책이 매번 새 경로를 막지 않도록), 변형한 소스만 하위 폴더에 둔다.
$Work    = Join-Path $Tools '.muttmp'   # 변형된 .inc / .c (실행하지 않으므로 하위 폴더로 충분)
$BinName = @{
    'test_sched_select.c' = '_mut_sel.exe'
    'test_ack_parse.c'    = '_mut_ack.exe'
    'test_ack_match.c'    = '_mut_match.exe'
    'test_sched_loop.c'   = '_mut_loop.exe'
}
$ConstBin = Join-Path $Tools '_mut_const.exe'

if (-not (Get-Command gcc -ErrorAction SilentlyContinue)) {
    Write-Host "!! gcc 를 찾을 수 없다. MinGW/MSYS2 터미널에서 돌리거나 PATH 를 확인할 것." -ForegroundColor Red
    exit 2
}

# --- 코드 뮤턴트 (펌웨어 .inc 를 망가뜨린다) --------------------------------------
$Mutants = @(
    @{ Desc = 'ACK 검증을 건너뛰고 무조건 성공 처리'
       File = 'sched_dispatch.inc'
       Find = 'ok = wait_for_slave_ack(channel_index, risk_level);'
       Repl = 'ok = 1U;' }

    @{ Desc = '송신 실패해도 applied 를 갱신'
       File = 'sched_dispatch.inc'
       Find = @'
  if (ok != 0U)
  {
    channel_ctrl[channel_index].applied_risk = risk_level;
'@
       Repl = @'
  if (1)
  {
    channel_ctrl[channel_index].applied_risk = risk_level;
'@ }

    # 이 뮤턴트가 곧 Case E("옛 ACK 가 그 사이 바뀐 새 상태를 적용됐다고 기록")를 코드로 옮긴
    # 것이다. 스냅샷한 want 대신 '지금의 desired' 를 applied 에 쓰면, ACK 를 기다리는 동안
    # ctrl_task 가 desired 를 바꾼 경우 보내지도 않은 값이 적용된 것으로 남는다.
    # !! test_sched_loop 의 test_desired_changes_during_ack_wait 없이는 살아남는다(확인함).
    @{ Desc = 'ACK 대기 중 바뀐 desired 를 applied 에 기록 (옛 ACK 오염)'
       File = 'sched_dispatch.inc'
       Find = 'channel_ctrl[channel_index].applied_risk = risk_level;'
       Repl = 'channel_ctrl[channel_index].applied_risk = channel_ctrl[channel_index].desired_risk;' }

    @{ Desc = '커서를 전진시키지 않음 (라운드로빈 파괴)'
       File = 'sched_dispatch.inc'
       Find = 'sched_cursor = (uint8_t)((ch + 1U) % CHANNEL_COUNT);'
       Repl = '' }

    @{ Desc = 'DANGER 우선 패스를 부르지 않음'
       File = 'sched_dispatch.inc'
       Find = 'ch = pick_pending(1U);'
       Repl = 'ch = SCHED_NO_CHANNEL;' }

    @{ Desc = '실패한 채널의 백오프를 무시'
       File = 'sched_select.inc'
       Find = 'if (channel_ctrl[ch].retry != 0U && idle < SCHED_RETRY_BACKOFF_MSEC)'
       Repl = 'if (0)' }

    @{ Desc = '주기 리프레시를 아예 하지 않음'
       File = 'sched_select.inc'
       Find = 'if (danger_only == 0U && idle >= SCHED_REFRESH_MSEC)'
       Repl = 'if (0)' }

    @{ Desc = '리프레시가 DANGER 전환을 밀어냄 (우선순위 역전)'
       File = 'sched_select.inc'
       Find = 'if (danger_only == 0U && idle >= SCHED_REFRESH_MSEC)'
       Repl = 'if (idle >= SCHED_REFRESH_MSEC)' }

    @{ Desc = '보내기 전에 큐를 비우지 않음 (늦은 ACK 오인)'
       File = 'ack_match.inc'
       Find = 'while (ack_queue_take(&discarded, 0U) != 0U)'
       Repl = 'while (0)' }

    @{ Desc = 'ACK 의 채널을 대조하지 않음'
       File = 'ack_match.inc'
       Find = 'if (ack.channel == expect_channel -and-and ack.risk_level == risk_level)'.Replace('-and-and','&&')
       Repl = 'if (ack.risk_level == risk_level)' }

    @{ Desc = 'ACK 의 등급을 대조하지 않음'
       File = 'ack_match.inc'
       Find = 'if (ack.channel == expect_channel -and-and ack.risk_level == risk_level)'.Replace('-and-and','&&')
       Repl = 'if (ack.channel == expect_channel)' }

    # .py 에는 있었으나 이 스크립트에만 빠져 있던 항목이다. Python 이 없는 PC 는 이 스크립트가
    # 유일한 경로라, 빠진 만큼 검사가 약해진 채로 "전부 통과"가 나오고 있었다.
    @{ Desc = '남의 ACK 하나에 바로 포기'
       File = 'ack_match.inc'
       Find = @'
    ++stat_ack_mismatch;
  }
'@
       Repl = @'
    ++stat_ack_mismatch;
    return 0U;
  }
'@ }

    @{ Desc = '경과 시간을 감김에 취약하게 계산'
       File = 'ack_match.inc'
       Find = 'const uint32_t elapsed = ack_now_ms() - start;'
       Repl = 'const uint32_t elapsed = (ack_now_ms() > start) ? (ack_now_ms() - start) : 0U;' }
)

# --- 상수 회귀 (테스트 파일의 상수를 되돌린다) ------------------------------------
$ConstRegressions = @(
    @{ Desc = '재시도 백오프 500 -> 100ms (굶주림 버그 복원)'
       Find = '#define SCHED_RETRY_BACKOFF_MSEC 500U'
       Repl = '#define SCHED_RETRY_BACKOFF_MSEC 100U' }
    @{ Desc = '스케줄러 틱 20 -> 60ms'
       Find = '#define SCHED_TICK_MSEC 20U'
       Repl = '#define SCHED_TICK_MSEC 60U' }
    @{ Desc = '주기 리프레시 2000 -> 20000ms'
       Find = '#define SCHED_REFRESH_MSEC 2000U'
       Repl = '#define SCHED_REFRESH_MSEC 20000U' }
)

function Read-Normalized([string]$Path) {
    # 줄바꿈을 LF 로 통일한다. CRLF 파일에서 여러 줄 패턴이 조용히 안 맞는 것을 막는다.
    [System.IO.File]::ReadAllText($Path).Replace("`r`n", "`n")
}

function Write-Utf8([string]$Path, [string]$Text) {
    [System.IO.File]::WriteAllText($Path, $Text, (New-Object System.Text.UTF8Encoding($false)))
}

function Apply([string]$Text, [string]$Find, [string]$Repl, [string]$What) {
    # 이 파일이 CRLF 로 저장돼 있으면 here-string 패턴도 CRLF 를 품는다. 대상 파일은
    # Read-Normalized 로 LF 가 되어 있으므로, 패턴도 LF 로 맞춰야 여러 줄 매칭이 된다.
    $Find = $Find.Replace("`r`n", "`n")
    $Repl = $Repl.Replace("`r`n", "`n")
    # .Contains / .IndexOf 는 정규식이 아니라 문자 그대로 찾는다. 패턴에 ( ) [ ] % 가
    # 들어있어서 -replace 를 쓰면 정규식으로 해석돼 엉뚱하게 동작한다.
    if (-not $Text.Contains($Find)) {
        Write-Host ''
        Write-Host "!! 뮤테이션 실패: '$What' 의 패턴을 찾지 못했다." -ForegroundColor Red
        Write-Host "   찾던 것: $Find"
        Write-Host "   (소스가 바뀌었다면 이 스크립트의 목록도 함께 고칠 것)"
        exit 2
    }
    $i = $Text.IndexOf($Find)
    $Text.Substring(0, $i) + $Repl + $Text.Substring($i + $Find.Length)
}

function Invoke-Binary([string]$Bin, [string]$ErrFile) {
    # !! PowerShell 로 직접 '& $bin 2>$err' 하면 안 된다. PowerShell 5.1 은 native 명령이
    #    stderr 에 쓰면 그것을 NativeCommandError 레코드로 감싸서, 파일에는 프로그램이
    #    출력한 assert 메시지 대신 PowerShell 자신의 오류 텍스트가 들어간다
    #    ("+ FullyQualifiedErrorId : NativeCommandError").
    #    cmd 에게 리다이렉트를 맡기면 PowerShell 이 스트림을 건드리지 않아 원문이 남는다.
    & cmd /c ('"' + $Bin + '" 1>nul 2>"' + $ErrFile + '"')
    return $LASTEXITCODE
}

function Get-LastMessage([string]$Path) {
    # 빈 줄을 걸러 마지막 '내용 있는' 줄을 고른다. 그냥 [-1] 을 쓰면 끝의 빈 줄이 잡혀
    # 메시지가 사라진다.
    $lines = @(Get-Content $Path -ErrorAction SilentlyContinue | Where-Object { $_.Trim() -ne '' })
    if ($lines.Count -gt 0) { $lines[-1] } else { '(메시지 없음)' }
}

function Invoke-Suite([string]$IncDir, [string]$OutDir) {
    # 세 테스트를 모두 돌린다. 하나라도 실패하면 (실패했음, 마지막 메시지) 를 준다.
    foreach ($test in $Tests) {
        $bin     = Join-Path $Tools $BinName[$test]
        $src     = Join-Path $Tools $test
        $errFile = Join-Path $OutDir 'err.txt'

        & gcc -w "-I$IncDir" $src -o $bin 2>$null
        if ($LASTEXITCODE -ne 0) { return @{ Failed = $true; Message = "$test 빌드 실패" } }

        if ((Invoke-Binary $bin $errFile) -ne 0) {
            return @{ Failed = $true; Message = "${test}: " + (Get-LastMessage $errFile) }
        }
    }
    return @{ Failed = $false; Message = '' }
}

# 작업 폴더를 새로 만든다 (앞선 실행이 남긴 것이 있으면 지우고 시작)
if (Test-Path $Work) { Remove-Item $Work -Recurse -Force -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Path $Work | Out-Null

try {
    $survived = @()

    Write-Host ('=' * 78)
    Write-Host '뮤테이션 검사 — 일부러 망가뜨린 코드를 테스트가 잡아내는가'
    Write-Host '(뮤턴트마다 테스트 3개를 다시 빌드한다. 1~2분 걸린다)' -ForegroundColor DarkGray
    Write-Host ('=' * 78)

    foreach ($m in $Mutants) {
        Get-ChildItem (Join-Path $Inc '*.inc') | ForEach-Object { Copy-Item $_.FullName $Work -Force }
        $target = Join-Path $Work $m.File
        Write-Utf8 $target (Apply (Read-Normalized $target) $m.Find $m.Repl $m.Desc)

        Write-Host ("  " + $m.Desc + " ... ") -NoNewline
        $r = Invoke-Suite $Work $Work
        if ($r.Failed) {
            Write-Host '[검출]' -ForegroundColor Green
            Write-Host ("      └ " + $r.Message) -ForegroundColor DarkGray
        } else {
            Write-Host '[생존]' -ForegroundColor Red
            $survived += $m.Desc
        }
    }

    Write-Host ''
    Write-Host ('=' * 78)
    Write-Host '상수 회귀 검사 — 값을 되돌리면 테스트가 실패해야 정상'
    Write-Host ('=' * 78)

    $loopSrc = Read-Normalized (Join-Path $Tools 'test_sched_loop.c')

    foreach ($c in $ConstRegressions) {
        $mutated = Join-Path $Work 'test_sched_loop_const.c'
        Write-Utf8 $mutated (Apply $loopSrc $c.Find $c.Repl $c.Desc)

        Write-Host ("  " + $c.Desc + " ... ") -NoNewline
        $bin     = $ConstBin
        $errFile = Join-Path $Work 'err.txt'

        & gcc -w "-I$Inc" $mutated -o $bin 2>$null
        if ($LASTEXITCODE -ne 0) { Write-Host '[빌드 실패]' -ForegroundColor Red; continue }

        if ((Invoke-Binary $bin $errFile) -ne 0) {
            Write-Host '[검출]' -ForegroundColor Green
            Write-Host ("      └ " + (Get-LastMessage $errFile)) -ForegroundColor DarkGray
        } else {
            Write-Host '[생존]' -ForegroundColor Red
            $survived += $c.Desc
        }
    }

    Write-Host ''
    if ($survived.Count -gt 0) {
        Write-Host ("!! 살아남은 뮤턴트 " + $survived.Count + "개 -- 검사에 구멍이 있다:") -ForegroundColor Red
        $survived | ForEach-Object { Write-Host ("   - " + $_) }
        exit 1
    }

    Write-Host ("모든 뮤턴트 검출됨 (" + $Mutants.Count + " 코드 + " + $ConstRegressions.Count + " 상수)") -ForegroundColor Green
    exit 0
}
finally {
    Remove-Item $Work -Recurse -Force -ErrorAction SilentlyContinue
    @('_mut_sel.exe', '_mut_ack.exe', '_mut_match.exe', '_mut_loop.exe', '_mut_const.exe') | ForEach-Object {
        Remove-Item (Join-Path $Tools $_) -Force -ErrorAction SilentlyContinue
    }
}
