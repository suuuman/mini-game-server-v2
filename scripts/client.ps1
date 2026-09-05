# scripts\client.ps1 — C++ 클라이언트(client.exe) 회귀 하네스, B 갈래 자체 스폰
#
#   무엇을 검증하는가 — client.exe 의 send/flow/selftest 세 서브커맨드를
#   실제 session.exe·village.exe 앞에서 35건 판정한다(T015-plan.md §11-2).
#   서버 소스는 이 워크트리에서 한 줄도 바뀌지 않았으므로 기존 PowerShell
#   하네스 14종을 다시 돌리지 않는다 — 이 파일은 그 대체가 아니라 client.exe
#   라는 새 소비자의 첫 회귀다(회귀는 이것을 더해 15종 — docs/TESTING.md §2).
#
#   단독 실행인 이유 — session.ps1 과 같은 두 서버(세션 클라 포트 9200 ·
#   S2S 수용 포트 9100 · 마을 A 클라 포트 9000)를 쓰고, S4 전용 마을 B 도
#   9010(session.ps1 내부 시나리오가 쓰는 포트)을 쓴다. 다른 하네스와 창을
#   같이 열면 포트가 겹친다 — 이 스크립트 하나만 단독으로 돈다.
#
#   종료 코드 규약 — client.exe 자체의 0/1/2/3(T015-plan.md 결정 4)과는
#   다른 층이다. 이 하네스는 35건 중 실패 건수를 그대로 프로세스 종료
#   코드로 낸다(0 이면 35건 전부 통과).
#
#   회귀 판정 대상이 아닌 옵션 — `--split`·`--read-delay` 는 client send 가
#   구현하지만 아래 35건에는 없다. send.ps1(PowerShell 14종의 1종)을 유지하기로
#   했으므로(T015-plan.md 결정 8) 그 옵션들의 시나리오는 이식하지 않는다 —
#   S1b 는 send.ps1 과의 "순서 판정 1종" 동치만 실증한다. `--drop-after-send`
#   는 S9 가 클라 쪽 close_rst 경로를 1회 실행한다 — send.ps1 의 RST
#   시나리오(다른 창에서 강제 종료하는 실습)를 이식한 것은 아니다.
#
#   사용:
#     .\scripts\client.ps1                  # Release, 3000 회
#     .\scripts\client.ps1 -Config ASan
#     .\scripts\client.ps1 -KeepScratch      # 실패 진단용 — 스크래치를 안 지운다

param(
    [string]$Config = 'Release',
    [int]$Repeat = 3000,
    [switch]$KeepScratch
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'harness_common.ps1')

$root = Split-Path -Parent $PSScriptRoot

# ── 자체 헬퍼(session.ps1:87 Write-MatrixLine · :92 Add-Result · :572 Read-FileBytesShared 형태를 그대로 따른다) ──

$script:MatrixResults = New-Object System.Collections.Generic.List[object]

function Write-MatrixLine([string]$Id, [string]$Status, [string]$Detail) {
    $color = if ($Status -eq 'PASS') { 'Green' } else { 'Red' }
    Write-Host ("[MATRIX {0}] {1} — {2}" -f $Id, $Status, $Detail) -ForegroundColor $color
}

function Add-Result([string]$Id, [bool]$Pass, [string]$Detail) {
    $status = if ($Pass) { 'PASS' } else { 'FAIL' }
    Write-MatrixLine $Id $status $Detail
    $script:MatrixResults.Add([pscustomobject]@{ Id = $Id; Pass = $Pass; Detail = $Detail; Status = $status })
}

# 서버가 그 순간에도 로그 파일에 쓰고 있을 수 있어 공유 읽기로 연다
# (session.ps1 의 Read-FileBytesShared 와 같은 이유).
function Read-FileBytesShared([string]$Path) {
    $fs = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    try {
        $len = [int]$fs.Length
        $buf = [byte[]]::new($len)
        $got = 0
        while ($got -lt $len) {
            $n = $fs.Read($buf, $got, $len - $got)
            if ($n -le 0) { break }
            $got += $n
        }
        return ,$buf
    } finally {
        $fs.Close()
    }
}

function Wait-LogMatch([string]$Path, [string]$Pattern, [int]$TimeoutMs) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $TimeoutMs) {
        if (Test-Path -LiteralPath $Path) {
            $bytes = Read-FileBytesShared $Path
            if ($bytes.Length -gt 0) {
                $text = [System.Text.Encoding]::UTF8.GetString($bytes)
                $m = [regex]::Match($text, $Pattern)
                if ($m.Success) { return $m }
            }
        }
        Start-Sleep -Milliseconds 100
    }
    return $null
}

function Get-LogText([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return '' }
    return [System.Text.Encoding]::UTF8.GetString((Read-FileBytesShared $Path))
}

# ⛔ Start-Process -RedirectStandardOutput 을 쓰지 않는다 — docs\TESTING.md §5
#   가 실측한 버퍼링 절단(4145B vs 5539B) 사고가 있다. client.exe 가 판정
#   줄마다 fflush 하므로 파이프 캡처만으로 온전하다.
function Invoke-Client([string[]]$ArgList) {
    $exe = Join-Path $root "build\x64\$Config\client.exe"
    Write-Host ("-- client " + ($ArgList -join ' '))
    $lines = & $exe @ArgList 2>&1
    $code = $LASTEXITCODE
    $lines | ForEach-Object { Write-Host "   $_" }
    Write-Host "   exit=$code"
    return @{ Code = $code; Lines = @($lines | ForEach-Object { "$_" }) }
}

# ── 실행 전 확인 ────────────────────────────────────────────────────────────
foreach ($n in @('client', 'session', 'village')) {
    $exePath = Join-Path $root "build\x64\$Config\$n.exe"
    if (-not (Test-Path $exePath)) { throw "$exePath 가 없다 — 먼저 .\scripts\build.ps1 로 빌드하라" }
}

# client 는 이 목록에 안 넣는다 — 범용 이름이라 무관한 프로세스를 죽일 수
# 있다(7단계 코드 리뷰 lead MED). client.exe 는 매 호출이 동기로 끝나
# 잔존하지 않으므로 사전 정리 대상에서 빼도 안전하다(위 exe 존재 확인은
# 그대로 client.exe 도 포함해서 한다).
$running = @(Get-Process village, session -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
    Write-Host "! 다른 하네스가 떠 있는 것으로 보인다 — $($running.Count)개 프로세스를 강제 종료한다" -ForegroundColor Yellow
    $running | ForEach-Object { Write-Host "    PID $($_.Id)  $($_.ProcessName)" }
    Get-Process village, session -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 300
}

$scratchRoot = Join-Path $env:TEMP ("client_harness_" + [guid]::NewGuid().ToString('N'))
$vilA = $null
$sess = $null
$vilB = $null
$script:exitCode = 1

try {
    # ── 기동: 마을 A → 세션 서버 ─────────────────────────────────────────
    $homeA = New-HarnessHome (Join-Path $scratchRoot 'vilA') '9000' '9100'
    $vilA = Start-Village $Config $homeA

    $sessHome = Join-Path $scratchRoot 'sess'
    New-Item -ItemType Directory -Path (Join-Path $sessHome 'config') -Force | Out-Null
    Copy-Item (Join-Path $root 'config\session.ini') (Join-Path $sessHome 'config\session.ini')
    $sess = Start-ServerProcess (Join-Path $root "build\x64\$Config\session.exe") $sessHome ''

    $sessLog = Join-Path $sessHome 'logs\server.log'
    $vilALog = Join-Path $homeA 'logs\server.log'

    $sessMatch = Wait-LogMatch $sessLog 'session server up' 8000
    if (-not $sessMatch) {
        Write-Host '=== sess log tail ===' ; if (Test-Path $sessLog) { Get-Content $sessLog -Tail 20 }
        Write-Host '=== sess proc.stderr.log ===' ; if (Test-Path (Join-Path $sessHome 'proc.stderr.log')) { Get-Content (Join-Path $sessHome 'proc.stderr.log') -Tail 20 }
        throw '세션 서버 기동 대기 실패("session server up" 을 8000ms 안에 못 봤다)'
    }
    $vilAMatch = Wait-LogMatch $vilALog '\[S2S\s+\] registered server_id=(\d+)' 15000
    if (-not $vilAMatch) {
        Write-Host '=== vilA log tail ===' ; if (Test-Path $vilALog) { Get-Content $vilALog -Tail 20 }
        Write-Host '=== vilA proc.stderr.log ===' ; if (Test-Path (Join-Path $homeA 'proc.stderr.log')) { Get-Content (Join-Path $homeA 'proc.stderr.log') -Tail 20 }
        throw '마을 A 의 S2S 등록 대기 실패(registered server_id 를 15000ms 안에 못 봤다)'
    }
    Write-Host "기동 : session up · vilA registered — $($vilAMatch.Value)"

    # ── S0 — selftest ────────────────────────────────────────────────────
    $r = Invoke-Client @('selftest')
    $m = [regex]::Match(($r.Lines -join "`n"), 'selftest:\s*(\d+)/(\d+)')
    $s0Pass = ($r.Code -eq 0) -and $m.Success -and ([int]$m.Groups[1].Value -eq [int]$m.Groups[2].Value) -and ([int]$m.Groups[2].Value -ge 11)
    Add-Result 'S0' $s0Pass "exit=$($r.Code) $(if ($m.Success) { $m.Value } else { '(selftest 줄 못 찾음)' })"

    # ── S1 — send --seq 순서 판정 ─────────────────────────────────────────
    $r = Invoke-Client @('send', '--port', '9000', '--repeat', "$Repeat", '--size', '8', '--framed', '--seq')
    $s1Pass = ($r.Code -eq 0) -and (($r.Lines -like "order : OK — $Repeat 개가*").Count -gt 0)
    Add-Result 'S1' $s1Pass "exit=$($r.Code)"

    # ── S1b — send.ps1 과의 순서 판정 동치 ────────────────────────────────
    Write-Host "-- send.ps1 -Port 9000 -Repeat $Repeat -Size 8 -Framed -Seq"
    $s1bLines = & powershell -NoProfile -File (Join-Path $PSScriptRoot 'send.ps1') -Port 9000 -Repeat $Repeat -Size 8 -Framed -Seq 2>&1
    $s1bLines | ForEach-Object { Write-Host "   $_" }
    $s1bPass = (($s1bLines | ForEach-Object { "$_" }) -like 'order : OK*').Count -gt 0
    Add-Result 'S1b' $s1bPass 'send.ps1 도 order : OK 를 냈다(순서 판정 1종 동치)'

    # ── S2 — flow 정상 경로(4건, 줄마다 1건) ──────────────────────────────
    $r = Invoke-Client @('flow', '--session-port', '9200', '--player', '7001')
    $s2Ok0 = ($r.Code -eq 0)
    Add-Result 'S2-login' ($s2Ok0 -and (($r.Lines -like 'login : result=0*').Count -gt 0)) "exit=$($r.Code)"
    Add-Result 'S2-enter' ($s2Ok0 -and (($r.Lines -like 'enter : result=0*').Count -gt 0)) "exit=$($r.Code)"
    Add-Result 'S2-echo'  ($s2Ok0 -and (($r.Lines -like 'echo  : OK 8B*').Count -gt 0)) "exit=$($r.Code)"
    Add-Result 'S2-pong'  ($s2Ok0 -and (($r.Lines -like 'pong  : OK*').Count -gt 0)) "exit=$($r.Code)"

    # ── S3a~f — 부정 대조군 ────────────────────────────────────────────────
    $r = Invoke-Client @('flow', '--session-port', '9000', '--player', '7002', '--timeout', '1500')
    Add-Result 'S3a' (($r.Code -eq 1) -and ($r.Lines -contains 'RESULT: FAIL closed by server at login')) "exit=$($r.Code)"

    $r = Invoke-Client @('send', '--port', '9200', '--repeat', '1', '--size', '8', '--framed', '--timeout', '1500')
    Add-Result 'S3b' (($r.Code -eq 1) -and (($r.Lines -like 'frames: 0 packets*').Count -gt 0)) "exit=$($r.Code)"

    $r = Invoke-Client @('flow', '--no-login', '--village-port', '9000', '--player', '7777', '--expect-enter-result', '3')
    Add-Result 'S3c' ($r.Code -eq 0) "exit=$($r.Code)"

    $r = Invoke-Client @('flow', '--session-port', '9200', '--player', '7003', '--enter-player', '7004', '--expect-enter-result', '3')
    Add-Result 'S3d' ($r.Code -eq 0) "exit=$($r.Code)"

    $r = Invoke-Client @('send', '--port', '9000', '--repeat', '1', '--size', '8', '--framed', '--raw-size', '60000', '--expect-close', '--timeout', '1500')
    Add-Result 'S3e' ($r.Code -eq 0) "exit=$($r.Code)"

    # S3f 는 recv_exact 의 누적 마감(--timeout 1500)이 실제로 동작하는지도
    # 겸해 잰다 — 세션 서버가 kEnterReq 를 버리기만 하면 이 클라가 타임아웃
    # 없이 영원히 기다릴 위험이 있다(7단계 코드 리뷰 test MED-6). 2배(3000ms)
    # 를 상한으로 둔다 — connect·프로세스 기동 오차를 흡수하되, 상한 자체가
    # 없는 것과는 구분되게.
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $r = Invoke-Client @('flow', '--no-login', '--village-port', '9200', '--player', '7005', '--timeout', '1500')
    $sw.Stop()
    $s3fPass = ($r.Code -eq 1) -and ($r.Lines -contains 'RESULT: FAIL timeout at enter') -and ($sw.ElapsedMilliseconds -le 3000)
    Add-Result 'S3f' $s3fPass "exit=$($r.Code) elapsed=$($sw.ElapsedMilliseconds)ms(상한 3000ms)"

    # ── S6 · S7 — 인자·접속 실패 종료 코드 ────────────────────────────────
    $r = Invoke-Client @('bogus')
    Add-Result 'S6' (($r.Code -eq 2) -and ($r.Lines.Count -gt 0) -and ($r.Lines[0] -eq 'client <subcommand> [--key value] [--flag]')) "exit=$($r.Code)"

    $r = Invoke-Client @('send', '--port', '9019', '--repeat', '1', '--framed')
    Add-Result 'S7' (($r.Code -eq 3) -and ($r.Lines -contains 'RESULT: FAIL connect')) "exit=$($r.Code)"

    # ── S8~S21 — 전부 마을 A(9000)·세션(9200) 앞에서 돈다(마을 A 정지 전에 끝낸다) ──

    $r = Invoke-Client @('send', '--port', '9000', '--repeat', '3', '--size', '4', '--expect-close', '--timeout', '1500')
    Add-Result 'S8' (($r.Code -eq 0) -and ($r.Lines -contains 'RESULT: PASS')) "exit=$($r.Code) (raw 판정 분기 + expect-close 긍정)"

    $r = Invoke-Client @('send', '--port', '9000', '--repeat', '1', '--size', '8', '--framed', '--drop-after-send')
    Add-Result 'S9' (($r.Code -eq 0) -and (($r.Lines -like 'drop  : RST 로 즉시 종료*').Count -gt 0)) "exit=$($r.Code) (close_rst 경로 실행)"

    $r = Invoke-Client @('send', '--port', '9000', '--repeat', '1', '--framed', '--bogus-flag')
    Add-Result 'S10' (($r.Code -eq 2) -and ($r.Lines.Count -gt 0) -and ($r.Lines[0] -eq 'client <subcommand> [--key value] [--flag]')) "exit=$($r.Code) (Args::unknown())"

    $r = Invoke-Client @('send', '--port', '9000', '--seq', '--size', '8')
    Add-Result 'S11' (($r.Code -eq 2) -and ($r.Lines.Count -gt 0) -and ($r.Lines[0] -eq 'client <subcommand> [--key value] [--flag]')) "exit=$($r.Code) (--seq 는 --framed 필요)"

    $r = Invoke-Client @('send', '--port', '9000', '--repeat', '1', '--size', '8', '--framed', '--expect-close', '--timeout', '1500')
    Add-Result 'S12' (($r.Code -eq 1) -and ($r.Lines -contains 'RESULT: FAIL not closed')) "exit=$($r.Code) (expect-close 부정)"

    $r = Invoke-Client @('flow', '--no-login', '--village-port', '9000', '--player', '7778', '--expect-enter-result', '0')
    Add-Result 'S13' (($r.Code -eq 1) -and ($r.Lines -contains 'RESULT: FAIL enter result=3')) "exit=$($r.Code) (expect-enter 불일치)"

    $r = Invoke-Client @('flow', '--session-port', '9200', '--player', '7006', '--expect-login-result', '1')
    Add-Result 'S14' (($r.Code -eq 1) -and ($r.Lines -contains 'RESULT: FAIL login result=0')) "exit=$($r.Code) (expect-login 불일치 — 7006 예약은 10s 뒤 만료)"

    $r = Invoke-Client @('flow', '--session-port', '9019', '--player', '7007')
    Add-Result 'S15' (($r.Code -eq 3) -and ($r.Lines -contains 'RESULT: FAIL connect')) "exit=$($r.Code) (flow connect 실패)"

    $r = Invoke-Client @('send', '--port', '9000', '--repeat', '1', '--size', '4', '--timeout', '1500')
    Add-Result 'S16' (($r.Code -eq 1) -and ($r.Lines -contains 'RESULT: FAIL total 0/4')) "exit=$($r.Code) (raw 고유 판정 분기 — --repeat 1 이라 송신 경합 없음)"

    $r = Invoke-Client @('flow', '--session-port', '9200', '--player', '7008', '--expect-enter-result', '5')
    Add-Result 'S17' (($r.Code -eq 1) -and ($r.Lines -contains 'RESULT: FAIL enter result=0')) "exit=$($r.Code) (기대!=kOk, 실제=kOk 조합 — Enter 는 실제 성공해 [ENTER] 를 남긴다)"

    $r = Invoke-Client @('flow', '--no-login', '--village-port', '9000', '--player', '7779', '--timeout', '1500')
    Add-Result 'S18' (($r.Code -eq 1) -and ($r.Lines -contains 'RESULT: FAIL enter result=3')) "exit=$($r.Code) (--expect-* 미지정 상태의 암묵 nonzero FAIL 분기)"

    $r = Invoke-Client @('flow', '--no-login', '--village-port', '9019', '--player', '7009')
    Add-Result 'S19' (($r.Code -eq 3) -and ($r.Lines -contains 'RESULT: FAIL connect')) "exit=$($r.Code) (Enter 단계 connect 실패)"

    $r = Invoke-Client @('send', '--port', '9000', '--raw-size', '5', '--size', '4')
    Add-Result 'S20' (($r.Code -eq 2) -and ($r.Lines.Count -gt 0) -and ($r.Lines[0] -eq 'client <subcommand> [--key value] [--flag]')) "exit=$($r.Code) (--raw-size 는 --framed 필요)"

    $r = Invoke-Client @('send', '--port', 'abc')
    Add-Result 'S21' (($r.Code -eq 2) -and ($r.Lines.Count -gt 0) -and ($r.Lines[0] -eq 'client <subcommand> [--key value] [--flag]')) "exit=$($r.Code) (args.ok()==false — 숫자 인자 오류)"

    # ── 마을 A 정지 → S5(3건) ─────────────────────────────────────────────
    # Stop-Harness 는 $ScratchRoot 를 마지막 Remove-Item(SilentlyContinue) 에만
    # 쓴다 — 없는 경로를 주면 정지 시퀀스(개행→WaitForExit→Kill 폴백)만
    # 재사용되고 진짜 스크래치는 하네스 끝에서 한 번만 지운다.
    Stop-Harness (Join-Path $scratchRoot ('nonexistent-' + [guid]::NewGuid().ToString('N'))) $null $vilA
    $vilALogText = Get-LogText $vilALog

    $connMatch = [regex]::Match($vilALogText, '\[CONN \] peak=\d+\s+rejected=0')
    Add-Result 'S5-conn' $connMatch.Success "$(if ($connMatch.Success) { $connMatch.Value } else { '[CONN ] ... rejected=0 줄 없음' })"

    $netMatch = [regex]::Match($vilALogText, '\[NET  \] idle_kicked=0\s+send_full_kicked=0')
    Add-Result 'S5-net' $netMatch.Success "$(if ($netMatch.Success) { $netMatch.Value } else { '[NET  ] idle_kicked=0 send_full_kicked=0 줄 없음' })"

    # S17 은 클라 판정은 FAIL 이지만 서버 쪽 입장은 성공이라 [ENTER] 를
    # 남긴다 — S3c·S3d·S13·S18 은 실패 갈래([WARN])라 안 남긴다.
    # 개수만 세지 않고 player 값까지 대조한다 — 어느 시나리오가 잘못 성공하고
    # 다른 것이 잘못 실패해 개수가 우연히 2 로 유지되는 경우를 가르기 위해서다
    # (11단계 test 렌즈 MED). 기대 집합은 S2 의 7001 과 S17 의 7008 뿐이다.
    $enterMatches = [regex]::Matches($vilALogText, '\[ENTER\] #\d+ -> player=(\d+)')
    $enterPlayers = @($enterMatches | ForEach-Object { $_.Groups[1].Value } | Sort-Object)
    $expectedPlayers = @('7001', '7008')
    $enterOk = ($enterMatches.Count -eq 2) -and (($enterPlayers -join ',') -eq ($expectedPlayers -join ','))
    Add-Result 'S5-enter' $enterOk "[ENTER] 줄 개수=$($enterMatches.Count) player=[$($enterPlayers -join ',')](기대 2 — S2 7001 · S17 7008 만 성공 Enter)"

    # ── 세션 서버 정지(판정 없음 — [SESS ] 요약만 기록) ───────────────────
    Stop-Harness (Join-Path $scratchRoot ('nonexistent-' + [guid]::NewGuid().ToString('N'))) $null $sess
    $sessLogText = Get-LogText $sessLog
    Write-Host ''
    Write-Host '=== 세션 서버 [SESS ] 종료 요약 ==='
    ($sessLogText -split "`n") | Where-Object { $_ -match '\[SESS \]' } | ForEach-Object { Write-Host $_.TrimEnd("`r") }
    Write-Host ''

    # ── 마을 B(S4 전용, idle_timeout_sec=2 · s2s 비활성) ──────────────────
    $homeB = New-HarnessHome (Join-Path $scratchRoot 'vilB') '9010' '9100'
    $serverIniB = Join-Path $homeB 'config\server.ini'
    Set-IniKeyInSection $serverIniB 'net' 'idle_timeout_sec' '2'
    Assert-IniValue $serverIniB 'net' 'idle_timeout_sec' '2'
    Set-IniKeyInSection $serverIniB 's2s' 'host' ''
    Assert-IniValue $serverIniB 's2s' 'host' ''

    $vilB = Start-Village $Config $homeB
    $vilBLog = Join-Path $homeB 'logs\server.log'
    $vilBMatch = Wait-LogMatch $vilBLog 'listening on 9010' 8000
    if (-not $vilBMatch) {
        Write-Host '=== vilB log tail ===' ; if (Test-Path $vilBLog) { Get-Content $vilBLog -Tail 20 }
        throw '마을 B 기동 대기 실패("listening on 9010" 을 8000ms 안에 못 봤다)'
    }

    # ── S4a · S4b — idle 2s 마을 앞 ping 생존/미생존 ──────────────────────
    # pongs 하한 5 의 근거 — hold 7s · ping 1s 면 이론상 7회(실측 Release·ASan
    # 전 회차 7)지만, 첫 ping 은 hold 진입 뒤 1s 에 나가고 마지막 회차는 hold
    # 만료와 겹칠 수 있어 2회 마진을 둔다. idle_timeout_sec=2 마을이라 ping 이
    # 2s 넘게 끊기면 어차피 절단(closed=true)돼 S4a 가 실패하므로, 하한은
    # 「ping 이 주기적으로 나갔다」를 보는 보조 단언이다(Step 2 스파이크:
    # hold 4 · ping 1s → pongs=4).
    $r = Invoke-Client @('send', '--port', '9010', '--repeat', '1', '--size', '8', '--framed', '--hold', '7', '--ping-ms', '1000', '--timeout', '1500')
    $pongsMatch = [regex]::Match(($r.Lines -join "`n"), 'hold\s*: done pongs=(\d+) closed=false')
    $s4aPass = ($r.Code -eq 0) -and $pongsMatch.Success -and ([int]$pongsMatch.Groups[1].Value -ge 5)
    Add-Result 'S4a' $s4aPass "exit=$($r.Code) $(if ($pongsMatch.Success) { $pongsMatch.Value } else { '(hold done 줄 못 찾음)' })"

    $r = Invoke-Client @('send', '--port', '9010', '--repeat', '1', '--size', '8', '--framed', '--hold', '7', '--timeout', '1500')
    $s4bPass = ($r.Code -eq 1) -and (($r.Lines -like 'hold  : closed by server after*').Count -gt 0)
    Add-Result 'S4b' $s4bPass "exit=$($r.Code)"

    # ── 마을 B 정지 → S4c ─────────────────────────────────────────────────
    Stop-Harness (Join-Path $scratchRoot ('nonexistent-' + [guid]::NewGuid().ToString('N'))) $null $vilB
    $vilBLogText = Get-LogText $vilBLog
    $netBMatch = [regex]::Match($vilBLogText, '\[NET  \] idle_kicked=1\b')
    Add-Result 'S4c' $netBMatch.Success "$(if ($netBMatch.Success) { $netBMatch.Value } else { '[NET  ] idle_kicked=1 줄 없음' })"

    # ── 총수 자기 검증 + 요약 ──────────────────────────────────────────────
    $expectedTotal = 35
    $actualTotal = $script:MatrixResults.Count
    $countMismatch = ($actualTotal -ne $expectedTotal)
    if ($countMismatch) {
        Write-Host "[FAIL] 판정 수 불일치 — 기대 $expectedTotal 실제 $actualTotal" -ForegroundColor Red
    }

    # ⛔ @() 로 감싼다 — PowerShell 5.1 은 Where-Object 결과가 정확히 1개면 배열이
    #   아니라 그 객체 하나를 돌려주고, [pscustomobject] 하나의 .Count 는 $null 이다.
    #   그러면 실패가 정확히 1건일 때 exit $null → 0 이 돼 「실패 건수 = 종료 코드」
    #   규약이 그 경우에만 깨진다(8단계 뮤턴트 MUT1 회차 실측 — 34/35 FAIL 인데 exit 0).
    $failCount = @($script:MatrixResults | Where-Object { -not $_.Pass }).Count
    $passCount = @($script:MatrixResults | Where-Object { $_.Pass }).Count

    Write-Host ''
    Write-Host '==================== 요약 ===================='
    foreach ($res in $script:MatrixResults) {
        Write-Host ("{0,-10} {1}  {2}" -f $res.Id, $res.Status, $res.Detail)
    }
    Write-Host ''
    if ($failCount -eq 0 -and -not $countMismatch) {
        Write-Host "client.ps1: $passCount/$expectedTotal PASS" -ForegroundColor Green
    } else {
        Write-Host "client.ps1: $passCount/$expectedTotal FAIL" -ForegroundColor Red
    }

    $script:exitCode = $failCount
    if ($countMismatch -and $script:exitCode -eq 0) { $script:exitCode = 1 }
} finally {
    # 안전망 — 위 흐름이 예외로 끊겨도 뜬 프로세스가 남지 않게 한다(정상
    # 경로에서는 세 프로세스 모두 이미 Stop-Harness 로 종료돼 있어 no-op 다).
    foreach ($p in @($vilA, $sess, $vilB)) {
        try { if ($p -and -not $p.HasExited) { $p.Kill() } } catch {}
    }
    if (-not $KeepScratch) {
        Start-Sleep -Milliseconds 300
        try { Remove-Item -Recurse -Force -LiteralPath $scratchRoot -ErrorAction SilentlyContinue } catch {}
    } else {
        Write-Host "스크래치 보존: $scratchRoot"
    }
}

exit $script:exitCode
