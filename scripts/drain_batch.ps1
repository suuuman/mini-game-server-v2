# scripts\drain_batch.ps1 — kSerialDrainBatch 상한이 만드는 지연·비용 실측 (T014 작업 C)
#
#   무엇을 재는가
#     플러더(Flooders 개 세션)가 kInventoryReq 를 Floods 개씩 한 번에 몰아 보내
#     그 세션의 직렬 큐를 깊게 만든다. 그동안 별도 프로브 세션의 Echo 왕복이
#     얼마나 늦어지는가(지연) · 플러더 응답이 전부 오는 데 얼마나 걸리는가
#     (비용) 를 잰다.
#     ⚠️ flood_done_ms 는 「마지막 응답이 회수된 시각」이다 — 회수가 프로브 1왕복 뒤에 한 번씩이라
#       실제 도착보다 최대 ProbeGapMs + 프로브 왕복 1회(수 ms)만큼 늦게 찍힌다. K 간 비교에는
#       무시할 크기지만 절대값으로 읽지 않는다.
#
#   왜 Echo 가 아니라 DB 왕복(kInventoryReq)을 플러드 단위로 쓰는가
#     µs 단위인 Echo 로는 kSerialDrainBatch 상한이 만드는 지연 차이가 하드웨어
#     노이즈에 묻힌다. handle_inventory 는 DB 왕복이 있어 자체로 몇 ms 가
#     걸리므로 배치 상한의 효과가 눈에 보이는 크기로 나온다. 프로브(지연을
#     재는 쪽)는 반대로 DB 를 안 타는 Echo 를 쓴다 — 프로브 자신의 처리
#     시간이 아니라 「직렬 큐 실행권을 못 받아 기다린 시간」만 보고 싶어서다.
#
#   왜 -AppWorkers 1 이 격리 측정인가
#     워커가 하나면 플러더의 직렬 큐 드레인과 프로브의 직렬 큐 드레인이
#     반드시 같은 워커 스레드를 놓고 순서를 다툰다(세션마다 직렬 큐는
#     따로지만 워커 N 개를 공유한다 — worker_pool.h). 워커가 여럿이면
#     우연히 다른 워커에 배정돼 상한 효과가 안 보일 수 있다.
#
#   cap_hits 유효성의 뜻
#     cap_hits>0 이면 이번 배치가 실제로 kSerialDrainBatch(K) 개를 꽉 채웠다는
#     직접 증거다. 0 이면 Floods 가 K 이하라 상한에 안 닿았다는 뜻이니 이
#     측정 자체가 무효다(지연·비용이 상한과 무관하게 나온 값이 된다).
#
#   송신 큐 넘침 산술(§4-6-②) — kInventoryAck 는 3+8×rows 바이트다. 시드
#     (player 1000..1255, 플레이어당 2행 → 3+16=19B)에서는 200 개를 받아도
#     약 3.8KB 로 kMaxSendQueue(1MB, session.h:57) 에 전혀 닿지 않는다. 훨씬
#     큰 인벤토리로 돌리는 사람은 이 산술이 안 통하니 스스로 다시 재야 한다.
#
#   -Restore 를 잊으면 생기는 사고
#     소스는 16 으로 남았는데 exe 는 이전 회차의 K 로 패치된 채라(또는 그
#     반대), 코드를 읽고 내린 판단과 실제 동작이 어긋난다 — 회귀가 거짓말을
#     하는 것과 같은 종류의 사고다(tick.ps1 6차 함정 참고).
#
#   동시성 형태 — 플러더는 Floods 개를 응답을 기다리지 않고 한 번의 Write 로
#     전부 보낸다(완전 파이프라이닝). 프로브는 반대로 1왕복씩 순차로 잰다.
#
#   사용:
#     .\drain_batch.ps1 -AppWorkers 1 -Floods 100 -Probes 20             # 기본(소스 그대로, K=16)
#     .\drain_batch.ps1 -Batch 4 -AppWorkers 1 -Floods 100 -Probes 20    # K=4 로 낮춰 격리 측정(빌드 포함)
#     .\drain_batch.ps1 -Restore                                        # 소스를 16 으로 원복 + Release 빌드만

param(
    [int]$Port = 9000, [string]$Config = 'Release', [int]$Seconds = 240,
    [int]$AppWorkers = 0,   # 0 = ini 그대로. 1 이면 격리 측정
    [int]$PoolSize = 0,
    [int]$Flooders = 1,     # 직렬 큐를 깊게 만드는 세션 수(각각 Connect-Reserved)
    [int]$Floods = 200,     # 플러더당 kInventoryReq 개수 — 한 번의 Write 로 전부 보낸다
    [int]$Probes = 50,      # 프로브 세션의 Echo 왕복 횟수(기준선·부하중 각각)
    [int]$ProbeGapMs = 5,
    [int]$Batch = 0,        # 0 = 소스 그대로. >0 이면 worker_pool.h 를 패치하고 Release 를 빌드한다
    [switch]$KeepBuild,     # 패치 회차 뒤 원복 빌드를 생략(스윕용 — 마지막엔 반드시 -Restore)
    [switch]$Restore,       # 소스를 16 으로 원복하고 Release 빌드만 하고 끝낸다
    [int]$Timeout = 20000, [int]$PlayerBase = 1000
)

$ErrorActionPreference = 'Stop'

$root          = Split-Path -Parent $PSScriptRoot
$hPath         = Join-Path $root 'src\app\worker_pool.h'
# -KeepBuild 로 exe 만 K 로 남긴 상태를 다음 실행이 알 수 있게 exe 옆에 마커를 둔다(리뷰 test 렌즈 MED).
#   콘솔 WARN 한 줄은 그 실행이 끝나면 사라지므로 영속 표식이 따로 필요하다.
$markerPath    = Join-Path $root 'build\x64\Release\village.exe.kSerialDrainBatch'
$anchorPattern = 'constexpr size_t kSerialDrainBatch = \d+;'

# kLoginReq/직행 접속이 폐지돼서 플러더·프로브 세션(예약 경유)은 예약을
#   거쳐야 한다. 실물 session.exe 는 안 띄운다 — 이 파일이 가짜 세션 서버
#   노릇을 해서 스스로 띄운 village.exe 에 직접 Reserve 를 찔러 넣는다.
. (Join-Path $PSScriptRoot 'harness_common.ps1')

$MSG_INV_REQ  = 4
$MSG_INV_ACK  = 104
$MSG_ECHO_REQ = 1

function New-Frame([byte[]]$Body, [int]$Id) {
    $len = if ($null -eq $Body) { 0 } else { $Body.Length }
    $h = [byte[]]::new(4)
    $h[0] = [byte](($len -shr 8) -band 0xFF); $h[1] = [byte]($len -band 0xFF)
    $h[2] = [byte](($Id  -shr 8) -band 0xFF); $h[3] = [byte]($Id  -band 0xFF)
    if ($len -eq 0) { return $h }
    return $h + $Body
}
# [int] 캐스팅 필수 (8차 함정)
function Get-U16([byte[]]$d, [int]$o) { return ([int]$d[$o] -shl 8) -bor [int]$d[$o+1] }

# zone_block.ps1:89-94 의 사본 — 예약 없는 클라(kEchoReq 는 게이트 이전,
#   frame_router.cpp:1243-1246)
function New-Client([int]$p) {
    $c = New-Object System.Net.Sockets.TcpClient
    $c.NoDelay = $true
    $c.Connect('127.0.0.1', $p)
    $s = $c.GetStream()
    $s.ReadTimeout = $Timeout; $s.WriteTimeout = $Timeout
    return @{ Client = $c; Stream = $s }
}

# zone_block.ps1:101-118 의 사본이다(drain·idle·gate·chat·dbload 도 같은 사본을
#   둔다 — find_copies.ps1 이 동기화를 지킨다). 서버가 쓰는 중인 로그를
#   FileShare.ReadWrite 로 열어야 공유 충돌 없이 읽는다.
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

# ⛔ ? 규칙 — 종료를 못 시켰거나 줄을 못 찾으면 이 값은 $null 로 남기고
#   호출부가 '?' 로 찍는다. 0 으로 찍지 않는다. Match.Success 를 먼저 보고
#   실패면 $null 을 대입한다 — Groups[1].Value 를 확인 없이 [int] 캐스팅하면
#   빈 문자열이 예외 없이 조용히 0 이 된다. 파싱 결과를 문자열 '?' 와 직접
#   비교하지 않는다 — PowerShell 5.1 은 "?" -gt 0 을 문화권 정렬 비교로
#   평가해 조용히 오판정을 낸다(이 환경 실측: False).
function Get-LogInt([string]$Pattern, [string]$Text) {
    $m = [regex]::Match($Text, $Pattern)
    if ($m.Success) { return [int]$m.Groups[1].Value }
    return $null
}
function Format-MaybeInt($v) { if ($null -eq $v) { '?' } else { $v } }
# worker_pool.h 는 저장소에서 BOM 없는 UTF-8 이다. PS 5.1 의 Set-Content -Encoding UTF8 은 BOM 을 붙이므로
#   그대로 쓰면 원복해도 첫 3바이트가 달라져 git diff 가 더러워진다(T014 실측 — 리뷰가 잡았다).
#   원본의 BOM 유무를 읽어 그대로 보존해 쓴다.
function Test-Utf8Bom([string]$Path) {
    $b = [System.IO.File]::ReadAllBytes($Path)
    return ($b.Length -ge 3 -and $b[0] -eq 0xEF -and $b[1] -eq 0xBB -and $b[2] -eq 0xBF)
}
function Write-Utf8PreserveBom([string]$Path, [string]$Text, [bool]$Bom) {
    [System.IO.File]::WriteAllText($Path, $Text, (New-Object System.Text.UTF8Encoding($Bom)))
}

# zone_block.ps1:163-179 Measure-Roundtrip 의 사본 + p99 추가.
function Measure-RoundtripStats($cli, [int]$n) {
    $echo = New-Frame ([byte[]]::new(8)) $MSG_ECHO_REQ
    $buf  = [byte[]]::new(4096)
    $times = @()
    for ($i = 0; $i -lt $n; $i++) {
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $cli.Stream.Write($echo, 0, $echo.Length)
        $cli.Stream.Flush()
        $got = 0
        while ($got -lt 12) {
            $r = $cli.Stream.Read($buf, 0, $buf.Length)
            if ($r -le 0) { break }
            $got += $r
        }
        $sw.Stop()
        $times += $sw.Elapsed.TotalMilliseconds
    }
    $sorted = @($times | Sort-Object)
    $avg = if ($times.Count -gt 0) { ($times | Measure-Object -Average).Average } else { 0 }
    $max = if ($times.Count -gt 0) { ($times | Measure-Object -Maximum).Maximum } else { 0 }
    $p99idx = if ($sorted.Count -gt 0) {
        [Math]::Min($sorted.Count - 1, [Math]::Max(0, [int][Math]::Ceiling($sorted.Count * 0.99) - 1))
    } else { 0 }
    $p99 = if ($sorted.Count -gt 0) { $sorted[$p99idx] } else { 0 }
    return [pscustomobject]@{
        Avg = [math]::Round($avg, 2); P99 = [math]::Round($p99, 2); Max = [math]::Round($max, 2)
    }
}

# ── -Restore: 소스를 16 으로 원복하고 Release 빌드만 하고 끝낸다 ────────────
if ($Restore) {
    if ($Config -ne 'Release') { throw "-Restore 는 Release 로만 빌드한다(현재 -Config=$Config)" }
    $hBom = Test-Utf8Bom $hPath
    $body = Get-Content $hPath -Encoding UTF8 -Raw
    if ([string]::IsNullOrWhiteSpace($body)) { throw "worker_pool.h 를 못 읽었다(비어 있음). 아무것도 안 했다." }
    $hit = ([regex]::Matches($body, $anchorPattern)).Count
    if ($hit -ne 1) { throw "kSerialDrainBatch 대입문 앵커를 ${hit}개 찾았다(1이어야 한다). 아무것도 안 했다." }
    $restored = [regex]::Replace($body, $anchorPattern, 'constexpr size_t kSerialDrainBatch = 16;')
    if ($restored.Length -lt 500) { throw "치환 결과가 너무 짧다($($restored.Length)B). 아무것도 안 썼다." }
    if ($restored -ne $body) {
        $tmp = "$hPath.tmp"
        Write-Utf8PreserveBom $tmp $restored $hBom
        if ((Get-Item $tmp).Length -lt 500) { Remove-Item $tmp; throw '임시 파일이 비었다. 원본은 그대로다.' }
        Move-Item $tmp $hPath -Force
        Write-Host "worker_pool.h : kSerialDrainBatch -> 16 로 원복"
    } else {
        Write-Host "worker_pool.h : 이미 16 이다 — 치환 불필요"
    }
    Get-Process village -ErrorAction SilentlyContinue | Stop-Process -Force
    & (Join-Path $PSScriptRoot 'build.ps1') -Config Release
    if ($LASTEXITCODE -ne 0) { throw '원복 빌드 실패' }
    if (Test-Path -LiteralPath $markerPath) { Remove-Item -LiteralPath $markerPath -Force; Write-Host "마커 제거: $markerPath" }
    return
}

if ($Batch -gt 0 -and $Config -ne 'Release') {
    throw "-Batch>0 은 -Config Release 에서만 쓴다(패치 빌드는 Release 만) — 현재 -Config=$Config"
}

if (Test-Path -LiteralPath $markerPath) {
    $mk = (Get-Content -LiteralPath $markerPath -Raw).Trim()
    if ($Batch -le 0) {
        Write-Host "WARN: 이전 -KeepBuild 회차가 exe 를 $mk 로 남겨 두었다(마커 $markerPath). 이 실행은 그 exe 로 잰다 — 의도가 아니면 먼저 -Restore" -ForegroundColor Yellow
    }
}
$patched = $false
$originalBody = $null
$hBom = $false

try {
    # ── 1. 상수 패치(Batch>0) — tick.ps1:57-73 의 3겹 안전장치를 그대로 옮긴다:
    #      ① 빈 원본이면 throw(파일을 안 건드린다) ② 앵커 개수 불일치·결과
    #      과소면 throw ③ 임시 파일에 먼저 쓰고 Move-Item 으로 갈아끼운다.
    #      원본은 $originalBody 에 보관하고, 무슨 일이 있어도 아래 finally 에서
    #      반드시 원복한다.
    if ($Batch -gt 0) {
        $hBom = Test-Utf8Bom $hPath
        $originalBody = Get-Content $hPath -Encoding UTF8 -Raw
        if ([string]::IsNullOrWhiteSpace($originalBody)) {
            throw "worker_pool.h 를 못 읽었다(비어 있음). 아무것도 안 썼다."
        }
        $hit = ([regex]::Matches($originalBody, $anchorPattern)).Count
        if ($hit -ne 1) {
            throw "kSerialDrainBatch 대입문 앵커를 ${hit}개 찾았다(1이어야 한다). 아무것도 안 썼다."
        }
        $patchedBody = [regex]::Replace($originalBody, $anchorPattern,
            "constexpr size_t kSerialDrainBatch = $Batch;")
        if ($patchedBody.Length -lt 500) {
            throw "치환 결과가 너무 짧다($($patchedBody.Length)B). 아무것도 안 썼다."
        }
        $tmp = "$hPath.tmp"
        Write-Utf8PreserveBom $tmp $patchedBody $hBom
        if ((Get-Item $tmp).Length -lt 500) { Remove-Item $tmp; throw '임시 파일이 비었다. 원본은 그대로다.' }
        Move-Item $tmp $hPath -Force
        $patched = $true
        Write-Host "worker_pool.h : kSerialDrainBatch -> $Batch 로 패치"

        # 빌드 산출물 경로는 Start-Village 가 build\x64\$Config\village.exe 를
        #   쓴다(harness_common.ps1:482-). 서버가 떠 있으면 링크가 막힌다.
        Get-Process village -ErrorAction SilentlyContinue | Stop-Process -Force
        & (Join-Path $PSScriptRoot 'build.ps1') -Config Release
        if ($LASTEXITCODE -ne 0) {
            throw "Batch=$Batch 패치 빌드 실패 — 아래 finally 에서 원복한다"
        }
    }

    # ── 2. 스폰 ─────────────────────────────────────────────────────────
    #      New-HarnessHome 의 반환값은 스크래치 루트 문자열 하나다
    #      (harness_common.ps1:473 `return $ScratchRoot`). ini 경로는
    #      Join-Path 로 조립한다(작업 B 1번과 같은 방식).
    $scratchRoot = Join-Path $env:TEMP ("drain_batch_harness_" + [guid]::NewGuid().ToString('N'))
    $listener    = $null
    $villageProc = $null
    $vhome       = $null
    $floodConns  = @()
    $probe       = $null

    try {
        $vhome = New-HarnessHome $scratchRoot ([string]$Port)
        if ($AppWorkers -ne 0 -or $PoolSize -ne 0) {
            $scratchIni = Join-Path $vhome 'config\server.ini'
            if ($AppWorkers -ne 0) { Set-IniKeyInSection $scratchIni 'app' 'workers' $AppWorkers }
            if ($PoolSize   -ne 0) { Set-IniKeyInSection $scratchIni 'db'  'pool_size' $PoolSize }
        }
        $listener    = Start-FakeSession 9100
        $villageProc = Start-Village $Config $vhome $Seconds
        $link        = Accept-FakeSessionLink $listener

        # ── 3. 접속 — 플러더는 Connect-Reserved, 프로브는 예약 없는 New-Client ──
        for ($i = 0; $i -lt $Flooders; $i++) {
            $reserved = Connect-Reserved $link $Port ([uint64]($PlayerBase + $i)) $Timeout
            $floodConns += @{ Client = $reserved.Client; Stream = $reserved.Stream }
        }
        $probe = New-Client $Port
        Write-Host "접속    : 플러더 $Flooders 개(player $PlayerBase..$($PlayerBase + $Flooders - 1)) + 프로브 1개"

        # ── 4. 기준선 ─────────────────────────────────────────────────
        $base = Measure-RoundtripStats $probe $Probes
        Write-Host "기준선  : probe echo 왕복  avg=$($base.Avg)ms  p99=$($base.P99)ms  max=$($base.Max)ms"

        # ── 5. 플러드 — 플러더마다 Floods 개를 한 번의 Write 로 이어 보낸다 ──
        #      drain_frames(iocp_server.cpp:675-771)가 for(;;) 루프로 수신 버퍼의
        #      프레임을 반복 절단해 프레임마다 job_sink_(743행 가드·744-745행
        #      호출)로 개별 submit 하므로, 한 번에 도착한 프레임 수만큼 직렬 큐
        #      깊이가 즉시 생긴다. 「한 recv 에 전부」는 TCP 가 보장하는 게 아니라
        #      loopback·소용량에서 실무적으로 성립하는 것이다 — 그래서 유효성은
        #      아래 8번의 cap_hits>0 로 판정한다(Floods=200 이 K=16 을 훨씬
        #      넘으므로 몇 번의 recv 로 나뉘어도 상한에는 닿는다).
        $invFrame = New-Frame $null $MSG_INV_REQ
        $frameLen = $invFrame.Length
        $payload  = [byte[]]::new($frameLen * $Floods)
        for ($i = 0; $i -lt $Floods; $i++) {
            [Array]::Copy($invFrame, 0, $payload, $i * $frameLen, $frameLen)
        }
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        foreach ($fc in $floodConns) {
            $fc.Stream.Write($payload, 0, $payload.Length)
            $fc.Stream.Flush()
        }

        # ── 6. 부하중 프로브 + 응답 회수(단일 스레드 — 교대로) ────────────
        #      ⛔ 송신 큐 넘침 킥 위험 회피 ① — 매 루프마다 모든 플러더 스트림을
        #      DataAvailable 이 0 이 될 때까지 비운다(아래 while 이 그것이다).
        $want         = $Flooders * $Floods
        $recvOk       = 0; $recvBusy = 0; $recvOther = 0
        $floodBuf     = [byte[]]::new(65536)
        $pendingFlood = @{}
        foreach ($fc in $floodConns) { $pendingFlood[$fc] = [System.IO.MemoryStream]::new() }
        $underTimes = @()
        $lastRespAtMs = 0.0
        $deadline   = [datetime]::UtcNow.AddMilliseconds($Timeout)
        $probeDone  = 0

        while (([datetime]::UtcNow -lt $deadline) -and
               ((($recvOk + $recvBusy + $recvOther) -lt $want) -or ($probeDone -lt $Probes))) {
            if ($probeDone -lt $Probes) {
                $t1 = Measure-RoundtripStats $probe 1
                $underTimes += $t1.Avg
                $probeDone++
            }
            $any = $false
            foreach ($fc in $floodConns) {
                while ($fc.Stream.DataAvailable) {
                    $any = $true
                    $n = $fc.Stream.Read($floodBuf, 0, $floodBuf.Length)
                    if ($n -gt 0) { $pendingFlood[$fc].Write($floodBuf, 0, $n) }
                }
            }
            if ($any) {
                $recvOk = 0; $recvBusy = 0; $recvOther = 0
                foreach ($fc in $floodConns) {
                    $d = $pendingFlood[$fc].ToArray()
                    $off = 0
                    while ($off + 4 -le $d.Length) {
                        $len = Get-U16 $d $off
                        if ($off + 4 + $len -gt $d.Length) { break }
                        if ((Get-U16 $d ($off + 2)) -eq $MSG_INV_ACK) {
                            $result = [int]$d[$off + 4]
                            if ($result -eq 0) { $recvOk++ }
                            elseif ($result -eq 4) { $recvBusy++ }
                            else { $recvOther++ }
                        }
                        $off += 4 + $len
                    }
                }
                $lastRespAtMs = $sw.Elapsed.TotalMilliseconds
            }
            Start-Sleep -Milliseconds $ProbeGapMs
        }
        $floodDoneMs = [math]::Round($lastRespAtMs, 1)
        $totalRecv   = $recvOk + $recvBusy + $recvOther
        $lost        = if ($want -gt $totalRecv) { $want - $totalRecv } else { 0 }

        $sortedUnder = @($underTimes | Sort-Object)
        $underAvg = if ($underTimes.Count -gt 0) { [math]::Round(($underTimes | Measure-Object -Average).Average, 2) } else { 0 }
        $underMax = if ($underTimes.Count -gt 0) { [math]::Round(($underTimes | Measure-Object -Maximum).Maximum, 2) } else { 0 }
        $p99idx = if ($sortedUnder.Count -gt 0) {
            [Math]::Min($sortedUnder.Count - 1, [Math]::Max(0, [int][Math]::Ceiling($sortedUnder.Count * 0.99) - 1))
        } else { 0 }
        $underP99 = if ($sortedUnder.Count -gt 0) { [math]::Round($sortedUnder[$p99idx], 2) } else { 0 }

        $ratio = if ($base.Avg -gt 0) { [math]::Round($underAvg / $base.Avg, 2) } else { 0 }

        Write-Host "부하중  : probe echo 왕복  avg=$underAvg ms  p99=$underP99 ms  max=$underMax ms  ($probeDone 회)"
        Write-Host "배수    : avg $ratio 배"
        Write-Host "플러드  : 완료(마지막 실제 응답) $floodDoneMs ms  응답 ok=$recvOk busy=$recvBusy  미회수=$lost"
        if ($lost -gt 0) {
            Write-Host "        Timeout=$Timeout ms 안에 $lost 건을 못 받았다 — flood_done_ms 는 완료 시각이 아니라 마지막 응답 시각이다" -ForegroundColor Yellow
        }

        # ── 7. 종료·로그 — 작업 B 4번과 같은 방식(zone_block.ps1:314-333) ──────
        foreach ($fc in $floodConns) {
            if ($pendingFlood -and $pendingFlood.ContainsKey($fc)) { $pendingFlood[$fc].Dispose() }
            $fc.Client.Close()
        }
        if ($probe) { $probe.Client.Close() }
        if ($villageProc -and -not $villageProc.HasExited) {
            try {
                $villageProc.HarnessStdin.WriteLine('')
                $villageProc.HarnessStdin.Flush()
                $villageProc.HarnessStdin.Close()
                $villageProc.WaitForExit(5000) | Out-Null
            } catch {}
        }
        Start-Sleep -Milliseconds 300
        $finalLog = Read-ServerLog (Join-Path $vhome 'logs\server.log')

        $drains    = Get-LogInt '\[WORK \][^\r\n]*drains=(\d+)' $finalLog
        $jobs      = Get-LogInt '\[WORK \][^\r\n]*jobs=(\d+)' $finalLog
        $capHits   = Get-LogInt '\[WORK \][^\r\n]*cap_hits=(\d+)' $finalLog
        $resubmits = Get-LogInt '\[WORK \][^\r\n]*resubmits=(\d+)' $finalLog
        $kBatch    = Get-LogInt '\[WORK \][^\r\n]*batch=(\d+)' $finalLog
        $tryFailed = Get-LogInt '\[POOL2\][^\r\n]*try_failed=(\d+)' $finalLog
        $kicked    = Get-LogInt '\[NET  \][^\r\n]*send_full_kicked=(\d+)' $finalLog

        foreach ($miss in @(
            @{ V = $drains;    L = '[WORK ] drains' }
            @{ V = $jobs;      L = '[WORK ] jobs' }
            @{ V = $capHits;   L = '[WORK ] cap_hits' }
            @{ V = $resubmits; L = '[WORK ] resubmits' }
            @{ V = $tryFailed; L = '[POOL2] try_failed' }
            @{ V = $kicked;    L = '[NET  ] send_full_kicked' }
        )) {
            if ($null -eq $miss.V) {
                Write-Host "미확인  : $($miss.L) — 종료 로그에서 못 찾음" -ForegroundColor Yellow
            }
        }

        $kReport = if ($Batch -gt 0) { $Batch } elseif ($null -ne $kBatch) { $kBatch } else { '?' }

        Write-Host "[WORK ] : drains=$(Format-MaybeInt $drains) jobs=$(Format-MaybeInt $jobs) cap_hits=$(Format-MaybeInt $capHits) resubmits=$(Format-MaybeInt $resubmits) batch=$kReport"
        Write-Host "[POOL2] : try_failed=$(Format-MaybeInt $tryFailed)"
        Write-Host "[NET  ] : send_full_kicked=$(Format-MaybeInt $kicked)"

        # ── 8. 유효성 3종 ────────────────────────────────────────────────
        #      ⛔ ? 는 숫자가 아니다 — capHits/kicked 는 [int] 또는 $null 로만
        #      담겨 있고, '?' 문자열과 직접 비교하지 않는다(위 Get-LogInt 주석).
        $capKnown  = ($null -ne $capHits)
        $capOk     = $capKnown -and ($capHits -gt 0)
        $kickKnown = ($null -ne $kicked)
        $kickBad   = $kickKnown -and ($kicked -gt 0)
        $lostBad   = $lost -gt 0

        if (-not $capKnown) {
            Write-Host "유효성  : △ cap_hits 미확인 — 판정 불가"
        } elseif ($capOk) {
            Write-Host "유효성  : ○ 상한이 실제로 걸렸다(cap_hits=$capHits)"
        } else {
            Write-Host "유효성  : ✕ 무효 — 상한에 안 닿았다(cap_hits=0. Floods <= Batch 인가?)"
        }
        if (-not $kickKnown) {
            Write-Host "유효성  : △ send_full_kicked 미확인 — 판정 불가"
        } elseif ($kickBad) {
            Write-Host "유효성  : ✕ 무효 — 플러더가 송신 큐 넘침으로 끊겼다(kicked=$kicked)"
        } else {
            Write-Host "유효성  : ○ 킥 없음(kicked=0)"
        }
        if ($lostBad) {
            Write-Host "유효성  : ✕ 무효 — 타임아웃 미회수 $lost 건, flood_done_ms 는 플러드 완료 시각이 아니라 마지막으로 실제 응답이 온 시각이다"
        }

        $hasX = (($capKnown) -and (-not $capOk)) -or $kickBad -or $lostBad
        $hasTriangle = (-not $capKnown) -or (-not $kickKnown)
        $valid = if ($hasX) { '0' } elseif ($hasTriangle) { '?' } else { '1' }

        $wReport = if ($AppWorkers -ne 0) { $AppWorkers } else { 'ini' }
        $pReport = if ($PoolSize -ne 0) { $PoolSize } else { 'ini' }
        $drainsFmt    = Format-MaybeInt $drains
        $jobsFmt      = Format-MaybeInt $jobs
        $capHitsFmt   = Format-MaybeInt $capHits
        $resubmitsFmt = Format-MaybeInt $resubmits
        $tryFailedFmt = Format-MaybeInt $tryFailed
        $kickedFmt    = Format-MaybeInt $kicked

        Write-Host ("RESULT batch={0} workers={1} pool={2} flooders={3} floods={4} base_avg={5} base_p99={6} base_max={7} flood_avg={8} flood_p99={9} flood_max={10} flood_done_ms={11} ok={12} busy={13} lost={14} drains={15} jobs={16} cap_hits={17} resubmits={18} try_failed={19} kicked={20} valid={21}" -f `
            $kReport, $wReport, $pReport, $Flooders, $Floods, `
            $base.Avg, $base.P99, $base.Max, `
            $underAvg, $underP99, $underMax, `
            $floodDoneMs, $recvOk, $recvBusy, $lost, `
            $drainsFmt, $jobsFmt, $capHitsFmt, $resubmitsFmt, $tryFailedFmt, $kickedFmt, $valid)

    } finally {
        # ── 9. 소켓 정리 → Stop-Harness ─────────────────────────────────
        Stop-Harness $scratchRoot $listener $villageProc
    }
} finally {
    # 패치했으면 무슨 일이 있어도(성공·실패·throw 무관) 여기서 원복한다.
    if ($patched -and $null -ne $originalBody) {
        $cur = Get-Content $hPath -Encoding UTF8 -Raw
        if ($cur -ne $originalBody) {
            $tmp = "$hPath.tmp"
            Write-Utf8PreserveBom $tmp $originalBody $hBom
            if ((Get-Item $tmp).Length -lt 500) {
                Remove-Item $tmp
                Write-Host "!! 원복용 임시 파일이 비었다 — worker_pool.h 를 손대지 않았다. 수동 확인 필요" -ForegroundColor Red
            } else {
                Move-Item $tmp $hPath -Force
                Write-Host "worker_pool.h : 원복 완료(16)"
            }
        }
        if (-not $KeepBuild) {
            Get-Process village -ErrorAction SilentlyContinue | Stop-Process -Force
            & (Join-Path $PSScriptRoot 'build.ps1') -Config Release
            if ($LASTEXITCODE -ne 0) {
                Write-Host "!! 원복 빌드 실패 — 수동으로 .\scripts\build.ps1 -Config Release 를 돌릴 것" -ForegroundColor Red
            } elseif (Test-Path -LiteralPath $markerPath) {
                Remove-Item -LiteralPath $markerPath -Force
            }
        } else {
            [System.IO.File]::WriteAllText($markerPath, "K=$Batch", (New-Object System.Text.UTF8Encoding($false)))
            Write-Host "WARN: exe 가 Batch=$Batch 로 남아 있다(마커 $markerPath). 끝나면 -Restore 를 부를 것" -ForegroundColor Yellow
        }
    }
}
