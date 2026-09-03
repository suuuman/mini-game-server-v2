# scripts\idle.ps1 — idle_timeout_sec/sweep_interval_sec 검증
#
#   [net] idle_timeout_sec 는 커밋 기본값이 0(꺼짐)이라 8종 자체 스폰 하네스는
#   전부 무관하게 통과한다 — 그 값이 실제로 끊는지는 이 파일이 스크래치
#   config 를 idle_timeout_sec=2·sweep_interval_sec=1 로 축소해 재는 것 말고는
#   방법이 없다(9종은 New-HarnessHome/New-VillageHome 이 이제 [net] 을 0 으로
#   강제 고정하므로 — harness_common.ps1 참조).
#
#   무엇을 보는가
#     I1  무발신 소켓이 임계+스윕 여유 안에 실제로 끊기는가(장치가 정말 도는가).
#     I2  주기적으로 뭔가 보내는 소켓은 그 창에서 살아남는가(경계 대조 —
#         I1 만으로는 "다 끊는다"와 "적절히 끊는다"를 못 가른다).
#     I3  입장한 세션이 무발신으로 끊긴 뒤, 같은 player_id 로 다시 예약·입장할
#         수 있는가 — entry.leave() 가 idle 킥 경로에서도 정상 종료 경로와
#         같이 도는지의 실증(안 돌면 그 player_id 는 서버가 살아 있는 동안
#         영구히 재입장이 막힌다).
#     I4  정상 종료 후 [NET  ] idle_kicked 카운터로 I1·I3 가 실제로 그 장치를
#         통해 끊겼음을 재확인(연결이 끊긴 이유가 idle 인지, 다른 원인인지는
#         이 카운터 없이는 못 가른다).
#
#   사용:
#     .\scripts\idle.ps1
#     .\scripts\idle.ps1 -Config Debug

param(
    [int]$Port               = 9000,
    [string]$Config          = 'Release',
    [int]$Seconds            = 120,   # Start-Village 의 죽은 인자(harness_common.ps1 참조) — 자리만 맞춘다
    [int]$IdleTimeoutSec     = 2,
    [int]$SweepIntervalSec   = 1
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'harness_common.ps1')

$PlayerI3 = [uint64]71001

function Test-Name([string]$n) { Write-Host ''; Write-Host "== $n ==" }
function Check([string]$what, [bool]$ok) {
    $mark = if ($ok) { 'O' } else { 'X' }
    Write-Host "   [$mark] $what"
    return $ok
}
$script:pass = 0; $script:fail = 0
function Note([bool]$ok) { if ($ok) { $script:pass++ } else { $script:fail++ } }

# gate.ps1 과 같은 헬퍼 둘(사본 — find_copies.ps1 이 동기화를 지킨다).
function Connect-Plain([int]$ClientPort, [int]$TimeoutMs) {
    $client = New-Object System.Net.Sockets.TcpClient
    $client.NoDelay = $true
    $client.Connect('127.0.0.1', $ClientPort)
    $stream = $client.GetStream()
    $stream.ReadTimeout = $TimeoutMs
    $stream.WriteTimeout = $TimeoutMs
    return [pscustomobject]@{ Client = $client; Stream = $stream }
}

# NetworkStream.DataAvailable 은 Socket.Available>0 만 본다 — 상대가 정상
#   종료(FIN)만 보내고 더 안 보내면 Available 은 0 인 채라 DataAvailable 이
#   영원히 false 다. Socket.Poll(SelectRead) 로 직접 물어야 "읽기 가능해졌는데
#   내용이 0바이트"(=상대가 닫았다)를 "아직 아무 일도 없다"와 가른다.
function Wait-ConnectionOutcome($Handle, [int]$TimeoutMs) {
    $sock = $Handle.Client.Client
    $stream = $Handle.Stream
    $deadline = [datetime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([datetime]::UtcNow -lt $deadline) {
        if ($sock.Poll(20000, [System.Net.Sockets.SelectMode]::SelectRead)) {
            if ($sock.Available -eq 0) {
                return @{ Closed = $true; Frame = $null }
            }
            $frame = Read-ClientFrame $stream 500
            return @{ Closed = $false; Frame = $frame }
        }
    }
    return @{ Closed = $false; Frame = $null }
}

function Read-ServerLog([string]$LogPath) {
    if (-not (Test-Path -LiteralPath $LogPath)) { return '' }
    $fs = [System.IO.File]::Open($LogPath, [System.IO.FileMode]::Open,
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
        return [System.Text.Encoding]::UTF8.GetString($buf, 0, $got)
    } finally {
        $fs.Dispose()
    }
}

# 임계(2s)+스윕(1s)+오차 여유 — "실제로 끊기는 시각은 임계와 임계+주기 사이"
#   (config/server.ini 주석)라 최악(3s)에 폴링·전파 지연을 더해 4.5s 로 잡는다.
$idleWaitMs = ($IdleTimeoutSec + $SweepIntervalSec) * 1000 + 1500

$scratchRoot = Join-Path $env:TEMP ("idle_harness_" + [guid]::NewGuid().ToString('N'))
$listener = $null
$villageProc = $null
$vhome = $null
try {
    $vhome = New-HarnessHome $scratchRoot ([string]$Port)

    # New-HarnessHome 이 [net] idle_timeout_sec 을 0 으로 고정해 둔 그 자리를
    #   이 하네스만 축소값으로 다시 덮는다 — 이중 단언은 그대로 지킨다.
    $serverIni = Join-Path $vhome 'config\server.ini'
    $idlePatch = [ordered]@{
        idle_timeout_sec   = [string]$IdleTimeoutSec
        sweep_interval_sec = [string]$SweepIntervalSec
    }
    foreach ($k in $idlePatch.Keys) { Set-IniKeyInSection $serverIni 'net' $k $idlePatch[$k] }
    foreach ($k in $idlePatch.Keys) { Assert-IniValue $serverIni 'net' $k $idlePatch[$k] }
    Write-Host "config: [net] idle_timeout_sec=$IdleTimeoutSec sweep_interval_sec=$SweepIntervalSec 패치·재검증 통과"

    $listener = Start-FakeSession 9100
    $villageProc = Start-Village $Config $vhome $Seconds
    $link = Accept-FakeSessionLink $listener
    $serverLog = Join-Path $vhome 'logs\server.log'

    # ── I1 — 무발신 소켓 → 임계+스윕 여유 안에 연결 종료 ────────────────
    Test-Name "I1 — 무발신 소켓 → ${idleWaitMs}ms 안에 연결 종료"
    $h1 = Connect-Plain $Port 3000
    $r1 = Wait-ConnectionOutcome $h1 $idleWaitMs
    Note (Check "연결이 끊겼다(frame=$(if ($r1.Frame) { $r1.Frame.MsgId } else { '-' }))" $r1.Closed)
    try { $h1.Client.Close() } catch {}

    # ── I2 — 1s 주기 에코 소켓 → 동일 구간 생존(경계 대조) ──────────────
    Test-Name "I2 — 1s 주기 kPingReq 소켓 → 동일 구간(${idleWaitMs}ms) 생존"
    $h2 = Connect-Plain $Port 3000
    $pongCount = 0
    $sw2 = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw2.ElapsedMilliseconds -lt $idleWaitMs) {
        Send-ClientFrame $h2.Stream $Harness_VilPingReq ([byte[]]::new(0))
        $pong = Read-ClientFrame $h2.Stream 2000
        if ($null -ne $pong -and $pong.MsgId -eq $Harness_VilPongAck) { $pongCount++ }
        Start-Sleep -Milliseconds 1000
    }
    $r2 = Wait-ConnectionOutcome $h2 500   # 마지막 핑 뒤 잠깐 더 봐도 안 끊겼는지
    Note (Check "$pongCount 회 PongAck 수신 · 구간 끝에 연결 유지(끊김=$($r2.Closed), 기대 False)" `
        (($pongCount -ge 3) -and (-not $r2.Closed)))
    try { $h2.Client.Close() } catch {}

    # ── I3 — Enter 입장 세션 무발신 → kick → 같은 player_id 재예약·재입장 ──
    Test-Name 'I3 — 입장 세션 무발신 → idle kick → 같은 player_id 재예약·재입장 성공'
    $connI3 = Connect-Reserved $link $Port $PlayerI3
    $r3 = Wait-ConnectionOutcome $connI3 $idleWaitMs
    Note (Check "무발신 입장 세션이 끊겼다(kick)" $r3.Closed)
    try { $connI3.Client.Close() } catch {}

    $connI3b = $null
    $reenterOk = $false
    try {
        $connI3b = Connect-Reserved $link $Port $PlayerI3
        $reenterOk = ($connI3b.PlayerId -eq $PlayerI3)
    } catch {
        $reenterOk = $false
    }
    Note (Check "같은 player_id($PlayerI3) 로 재예약·재입장 성공(entry.leave 수명 정리 실증)" $reenterOk)
    if ($null -ne $connI3b) { try { $connI3b.Client.Close() } catch {} }

    Write-Host ''
    if ($fail -eq 0) {
        Write-Host "판정  : O I1~I3 $pass 개 전부 통과(I4 는 종료 지표 절에서 별도 확인)"
    } else {
        Write-Host "판정  : X I1~I3 중 $fail 개 실패 / $($pass + $fail) 개"
    }
} finally {
    # I4 — 정상 종료 뒤 [NET  ] idle_kicked 를 읽어야 해서 Stop-Harness 를
    #   그대로 안 쓴다(drain.ps1 D9 와 같은 이유 — 사본, find_copies.ps1 이
    #   동기화를 지킨다). 같은 절차를 반복한 뒤 로그를 읽고 정리한다.
    try { if ($listener) { $listener.Stop() } } catch {}
    try {
        if ($villageProc -and -not $villageProc.HasExited) {
            $villageProc.HarnessStdin.WriteLine('')
            $villageProc.HarnessStdin.Flush()
            $villageProc.HarnessStdin.Close()
            if (-not $villageProc.WaitForExit(5000)) {
                $villageProc.Kill()
            }
        }
    } catch {
        try { if ($villageProc -and -not $villageProc.HasExited) { $villageProc.Kill() } } catch {}
    }
    Start-Sleep -Milliseconds 300

    Test-Name 'I4 — 정상 종료 후 [NET  ] idle_kicked >= 2'
    $finalLog = if ($vhome -and (Test-Path -LiteralPath (Join-Path $vhome 'logs\server.log'))) {
        Read-ServerLog (Join-Path $vhome 'logs\server.log')
    } else { '' }
    $mNet = [regex]::Match($finalLog, '\[NET  \] idle_kicked=(\d+)')
    $idleKicked = if ($mNet.Success) { [int]$mNet.Groups[1].Value } else { -1 }
    Note (Check "[NET  ] idle_kicked=$idleKicked(기대 >=2 — I1·I3 가 각각 낸다)" `
        ($mNet.Success -and ($idleKicked -ge 2)))
    if ($mNet.Success) { Write-Host "   $($mNet.Value)" }

    # ASan 은 [ALLOC] 을 안 찍는다 — drain.ps1 D9 와 같은 이유(사본,
    #   find_copies.ps1 이 동기화를 지킨다). 그 한 지표만 존재 여부 기대를
    #   뒤집는다.
    $indicators = [ordered]@{
        'POOL ' = '\[POOL \]'
        'CONN ' = '\[CONN \]'
        'TICK ' = '\[TICK \]'
    }
    if ($Config -eq 'ASan') {
        Note (Check '종료 지표 [ALLOC] 부재 확인(ASan 은 안 찍는다)' (-not ($finalLog -match '\[ALLOC\]')))
    } else {
        Note (Check '종료 지표 [ALLOC] 확인' ($finalLog -match '\[ALLOC\]'))
    }
    foreach ($k in $indicators.Keys) {
        Note (Check "종료 지표 [$k] 확인" ($finalLog -match $indicators[$k]))
    }

    if ($fail -eq 0) {
        Write-Host ''
        Write-Host "판정(전체) : O I1~I4 $pass 개 전부 통과"
    } else {
        Write-Host ''
        Write-Host "판정(전체) : X $fail 개 실패 / $($pass + $fail) 개"
    }

    try { Remove-Item -Recurse -Force -LiteralPath $scratchRoot -ErrorAction SilentlyContinue } catch {}
}

if ($fail -gt 0) { exit 1 }
exit 0
