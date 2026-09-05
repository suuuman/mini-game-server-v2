# scripts\dbload.ps1 — DB 커넥션 풀 크기 실측 (L13-B)
#
#   무엇을 재는가
#     N개 연결이 「서로 다른 player_id」로 로그인해 인벤토리 조회를 한꺼번에 던진다.
#     세션마다 직렬 큐가 따로 있고 그 실행권이 공용 큐를 오가며 워커에 유동
#     배정된다(docs/ARCHITECTURE.md §3 — player_id 해시로 워커를 고정 배정하지
#     않는다). 그래서 연결을 여러 개 써야(=세션을 여러 개 열어야) 여러 워커가
#     동시에 DB 를 타는 상황을 만들 수 있다. zone_block.ps1 은 player 1 하나만
#     쓴다 — 워커 하나만 돌아서 이 측정엔 안 맞는다.
#
#   판정은 「서버 종료 로그」에서 한다
#     [POOL2] db conns peak=? / ?  acquired=?  open_failed=?  discarded=?  try_failed=?
#       peak        동시에 실제로 쓰인 연결 수   ← 이게 「몇 개 필요한가」의 답
#       try_failed  못 빌려서 즉시 포기한 횟수    ← 0 이 아니면 풀이 모자라다
#
#   동시성 형태 — 클라당 라운드 몫(perRound)을 응답을 기다리지 않고 한꺼번에
#     쓰고 넘어간다(완전 파이프라이닝). 그래서 클라당 in-flight 상한은
#     사실상 없다(라운드 안 요청 전부가 동시에 미응답 상태일 수 있다).
#
#   사용:
#     .\dbload.ps1 -Clients 8 -Repeat 50
#     .\dbload.ps1 -Clients 8 -Repeat 50 -AppWorkers 2 -PoolSize 1   # [app] workers x [db] pool_size 격자

param(
    [int]$Port    = 9000,
    [int]$Clients = 8,
    [int]$Repeat  = 50,
    [int]$Timeout = 20000,

    # 「자연스러운」 player_id 분포로 재기 위한 옵션. 지금 구조(세션마다 직렬
    #   큐 · 실행권이 공용 큐를 오가며 워커에 유동 배정 — player_id 해시
    #   배정은 없다)에서는 워커 분산 자체를 이 옵션으로 만드는 게 아니라,
    #   「같은 유저 집합을 재현 가능하게 뽑는다」가 목적이다.
    #
    #   기본(PlayerPool = 0)은 예전 그대로 player 1..Clients 를 쓴다 (회귀 호환).
    #
    #   PlayerPool 을 주면 [PlayerBase, PlayerBase+PlayerPool) 에서 Clients 명을
    #   중복 없이 무작위로 뽑는다. 실제 운영에 가까운 조건이다 —
    #   player_id 는 순차 증가하지만 「지금 접속 중인 유저」는 그 안에 흩어져 있다.
    #
    #   Seed 를 주면 같은 뽑기를 재현할 수 있다. 분리 전/후를 비교하려면
    #     「같은 유저 집합」으로 재야 하므로 쓰기 혼합 측정에서 이게 필요해진다.
    [int]$PlayerBase = 1000,
    [int]$PlayerPool = 0,
    [int]$Seed       = 0,

    # [app] workers · [db] pool_size 격자 실측용 — 스크래치 config 를 스스로
    #   패치한다. 0 이면 커밋본 server.ini 값 그대로(패치 안 함).
    [int]$AppWorkers = 0,
    [int]$PoolSize   = 0,

    # 쓰기 혼합 — 이 스크립트의 본편.
    #
    #   읽기/쓰기를 나눈 목적이 「느린 쓰기 뒤에 빠른 읽기가 줄 서지 않게」이므로,
    #   쓰기가 없으면 분리의 효과가 아예 안 보인다. 읽기만 던지는 부하로는
    #   「나아졌다」도 「그대로다」도 말할 수 없다.
    #
    #   Traders = 거래 「쌍」의 수. 2N 개 클라가 추가로 접속해 서로 거래한다.
    #   TradeRepeat = 쌍당 거래 횟수.
    #
    #   거래는 6단계 왕복(로그인·입장 → 요청 → 수락 → 등록 ×2 → 확인 ×2)이라
    #     한 쌍씩 순차로 돌리면 부하가 안 된다. 모든 쌍을 같은 단계씩 묶어
    #     라운드로 진행한다 — 그래야 여러 거래가 서버에서 실제로 겹친다.
    [int]$Traders     = 0,
    [int]$TradeRepeat = 10,
    [int]$TradeZone   = 7,

    [string]$Config  = 'Release',   # 예약 경유 접속을 위해 스스로 스폰할 village.exe 구성
    [int]$Seconds    = 180          # 스폰한 village.exe 의 수명(초) — 부하 측정 전체를 덮을 여유
)

$ErrorActionPreference = 'Stop'

# kLoginReq/직행 접속이 폐지돼서 이 측정용 하네스도 예약을 거쳐야
#   한다. DB 커넥션 풀 크기만 재는 게 목적이라 실물 session.exe 는 안 띄운다 —
#   이 파일이 가짜 세션 서버 노릇을 해서 스스로 띄운 village.exe 에 직접
#   Reserve 를 찔러 넣는다. harness_common.ps1 참조.
#   로그인/거래 접속 시간은 아래 타이밍 측정 구간(Stopwatch) 시작 "전"이라
#   Connect-Reserved 의 왕복 한 번(예약+Enter)이 추가돼도 QPS 측정에는
#   영향이 없다.
. (Join-Path $PSScriptRoot 'harness_common.ps1')

$MSG_INV_REQ   = 4
$MSG_INV_ACK   = 104

# 거래 (쓰기 부하용)
$MSG_JOIN_REQ     = 2
$MSG_TRADE_REQ    = 6
$MSG_TRADE_ANS    = 7
$MSG_TRADE_ITEM   = 8
$MSG_TRADE_CONF   = 9
$MSG_TRADE_ACK    = 109

function New-Frame([byte[]]$Body, [int]$Id) {
    $len = if ($null -eq $Body) { 0 } else { $Body.Length }
    $h = [byte[]]::new(4)
    $h[0] = [byte](($len -shr 8) -band 0xFF); $h[1] = [byte]($len -band 0xFF)
    $h[2] = [byte](($Id  -shr 8) -band 0xFF); $h[3] = [byte]($Id  -band 0xFF)
    if ($len -eq 0) { return $h }
    return $h + $Body
}
function New-U32BE([int]$v) {
    $b = [byte[]]::new(4)
    $b[0] = [byte](($v -shr 24) -band 0xFF); $b[1] = [byte](($v -shr 16) -band 0xFF)
    $b[2] = [byte](($v -shr  8) -band 0xFF); $b[3] = [byte]( $v          -band 0xFF)
    return $b
}
# [int] 캐스팅 필수 (8차 함정)
function Get-U16([byte[]]$d, [int]$o) { return ([int]$d[$o] -shl 8) -bor [int]$d[$o+1] }

# zone_block.ps1:101-118 의 사본이다(find_copies.ps1 이 동기화를 지킨다) — 서버가
#   쓰는 중인 로그를 FileShare.ReadWrite 로 열어야 공유 충돌 없이 읽는다.
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

# ⛔ ? 규칙(§4-5) — 종료를 못 시켰거나 줄을 못 찾으면 이 값은 $null 로 남기고
#   호출부가 '?' 로 찍는다. 0 으로 찍지 않는다. Match.Success 를 먼저 보고
#   실패면 $null 을 대입한다 — Groups[1].Value 를 확인 없이 [int] 캐스팅하면
#   빈 문자열이 예외 없이 조용히 0 이 된다.
function Get-LogInt([string]$Pattern, [string]$Text) {
    $m = [regex]::Match($Text, $Pattern)
    if ($m.Success) { return [int]$m.Groups[1].Value }
    return $null
}
function Format-MaybeInt($v) { if ($null -eq $v) { '?' } else { $v } }

# ── 접속할 player_id 를 먼저 정한다 ──────────────────────────────────
if ($PlayerPool -gt 0) {
    if ($PlayerPool -lt $Clients) {
        throw "PlayerPool($PlayerPool) 이 Clients($Clients) 보다 작다 — 중복 없이 못 뽑는다"
    }
    if ($Seed -ne 0) { $null = Get-Random -SetSeed $Seed }
    $ids = @(($PlayerBase)..($PlayerBase + $PlayerPool - 1) | Get-Random -Count $Clients)
    $desc = "player $PlayerBase..$($PlayerBase + $PlayerPool - 1) 중 무작위 $Clients 명 (seed=$Seed)"
}
else {
    $ids  = @(1..$Clients)
    $desc = "player 1..$Clients (연속 — 회귀 호환용 기본값. 실제 접속 분포와는 다르다)"
}

$scratchRoot = Join-Path $env:TEMP ("dbload_harness_" + [guid]::NewGuid().ToString('N'))
$listener = $null
$villageProc = $null
$vhome = $null
try {
$vhome = New-HarnessHome $scratchRoot ([string]$Port)
# [app] workers · [db] pool_size 격자 실측 — New-HarnessHome 의 반환값은
#   스크래치 루트 문자열 하나다(harness_common.ps1:473 `return $ScratchRoot`).
#   ini 경로는 여기서 직접 조립한다.
if ($AppWorkers -ne 0 -or $PoolSize -ne 0) {
    $scratchIni = Join-Path $vhome 'config\server.ini'
    if ($AppWorkers -ne 0) { Set-IniKeyInSection $scratchIni 'app' 'workers' $AppWorkers }
    if ($PoolSize   -ne 0) { Set-IniKeyInSection $scratchIni 'db'  'pool_size' $PoolSize }
}
$listener = Start-FakeSession 9100
$villageProc = Start-Village $Config $vhome $Seconds
$link = Accept-FakeSessionLink $listener

# ── 연결 + 로그인(예약 경유) — 이 왕복은 아래 Stopwatch 시작 "전"이라
#    QPS 측정에 안 섞인다. ──────────────────────────────────────────
$conns = @()
foreach ($id in $ids) {
    # player_id 를 전부 다르게 준다 — 세션마다 직렬 큐가 따로라 연결을
    #   나눠야 여러 워커가 동시에 DB 를 탄다(player_id 해시 배정은 없다)
    $reserved = Connect-Reserved $link $Port ([uint64]$id) $Timeout
    $conns += @{ Client = $reserved.Client; Stream = $reserved.Stream; Player = $id }
}
Write-Host "connect : $Clients 개 — $desc"

# ── 거래 쌍 접속 (쓰기 부하) ─────────────────────────────────────────
#   읽기 클라와 player_id 가 겹치면 안 된다. 겹치면 같은 플레이어의 조회와 거래가
#     섞여 「읽기가 밀렸다」인지 「자기 거래를 기다린 것」인지 구분이 안 된다.
$traderPairs = @()
if ($Traders -gt 0) {
    $used  = @{}; foreach ($id in $ids) { $used[$id] = $true }
    $free  = @(($PlayerBase)..($PlayerBase + [math]::Max($PlayerPool, 512) - 1) |
              Where-Object { -not $used.ContainsKey($_) })
    if ($free.Count -lt ($Traders * 2)) { throw "거래용 player_id 가 모자란다" }

    function Connect-Trader([int]$PlayerId) {
        $reserved = Connect-Reserved $link $Port ([uint64]$PlayerId) $Timeout
        $s = $reserved.Stream
        $f = New-Frame (New-U32BE $TradeZone) $MSG_JOIN_REQ
        $s.Write($f, 0, $f.Length); $s.Flush()
        return @{ Client = $reserved.Client; Stream = $s; Player = $PlayerId
                  Session = $reserved.SessionId; Buf = [System.IO.MemoryStream]::new() }
    }

    for ($i = 0; $i -lt $Traders; $i++) {
        $traderPairs += ,@{ A = (Connect-Trader $free[$i*2]); B = (Connect-Trader $free[$i*2+1]) }
    }

    # session_id 는 Connect-Reserved(kEnterAck)가 이미 확정해 줬다 — 남는 건
    #   방금 보낸 JoinZoneReq 의 kJoinZoneAck 뿐이니 걸러서 버린다.
    Start-Sleep -Milliseconds 400
    $tmp = [byte[]]::new(65536)
    foreach ($p in $traderPairs) {
        foreach ($c in @($p.A, $p.B)) {
            while ($c.Stream.DataAvailable) {
                $n = $c.Stream.Read($tmp, 0, $tmp.Length)
                if ($n -gt 0) { $c.Buf.Write($tmp, 0, $n) }
            }
            $c.Buf.SetLength(0)
        }
    }
    Write-Host "traders : $Traders 쌍 (거래 $TradeRepeat 회씩 · 존 $TradeZone)"
}

# ── 한꺼번에 던진다 ──────────────────────────────────────────────────
$inv = New-Frame $null $MSG_INV_REQ
$sw = [System.Diagnostics.Stopwatch]::StartNew()

# 거래를 「라운드」로 진행한다. 모든 쌍이 같은 단계를 함께 밟아야
#   여러 거래가 서버에서 실제로 겹친다 — 한 쌍씩 끝내면 부하가 아니라 순차 실행이다.
#   응답을 기다리지 않고 다음 단계로 넘어간다. 서버가 순서대로 처리하므로
#     프로토콜은 성립하고, 클라가 왕복에 묶이지 않아 쓰기가 계속 쌓인다.
function Invoke-TradeRound {
    foreach ($p in $traderPairs) {
        $p.A.Stream.Write((New-Frame (New-U32BE $p.B.Session) $MSG_TRADE_REQ), 0, 8)
    }
    foreach ($p in $traderPairs) {
        $p.B.Stream.Write((New-Frame ((New-U32BE $p.A.Session) + [byte[]]@(1)) $MSG_TRADE_ANS), 0, 9)
    }
    foreach ($p in $traderPairs) {
        $p.A.Stream.Write((New-Frame ((New-U32BE 100) + (New-U32BE 1)) $MSG_TRADE_ITEM), 0, 12)
        $p.B.Stream.Write((New-Frame ((New-U32BE 200) + (New-U32BE 1)) $MSG_TRADE_ITEM), 0, 12)
    }
    foreach ($p in $traderPairs) {
        $p.A.Stream.Write((New-Frame ([byte[]]@(1)) $MSG_TRADE_CONF), 0, 5)
        $p.B.Stream.Write((New-Frame ([byte[]]@(1)) $MSG_TRADE_CONF), 0, 5)
        $p.A.Stream.Flush(); $p.B.Stream.Flush()
    }
    # 다음 라운드 전에 거래가 끝나야 한다 — 「세션당 거래는 하나」라
    #   안 기다리면 두 번째 요청이 kBusy 로 거절돼 쓰기 부하가 안 생긴다.
    foreach ($p in $traderPairs) {
        foreach ($c in @($p.A, $p.B)) {
            $deadline = [datetime]::UtcNow.AddMilliseconds(3000)
            $got = $false
            while (-not $got -and [datetime]::UtcNow -lt $deadline) {
                if ($c.Stream.DataAvailable) {
                    $n = $c.Stream.Read($tmp, 0, $tmp.Length)
                    if ($n -gt 0) { $c.Buf.Write($tmp, 0, $n) }
                    $d = $c.Buf.ToArray(); $off = 0
                    while ($off + 4 -le $d.Length) {
                        $len = Get-U16 $d $off
                        if ($off + 4 + $len -gt $d.Length) { break }
                        if ((Get-U16 $d ($off+2)) -eq $MSG_TRADE_ACK) { $got = $true }
                        $off += 4 + $len
                    }
                }
                else { Start-Sleep -Milliseconds 2 }
            }
            $c.Buf.SetLength(0)
        }
    }
}

# 읽기를 라운드로 쪼개 거래 사이에 끼운다.
#   한 번에 다 던지고 나서 거래를 돌리면 서버에서 두 부하가 안 겹친다 —
#   그러면 「쓰기 뒤에 읽기가 줄 섰나」를 잴 수가 없다. 겹쳐야 측정이 성립한다.
$rounds   = if ($Traders -gt 0) { [math]::Max($TradeRepeat, 1) } else { 1 }
$perRound = [math]::Ceiling($Repeat / $rounds)
$sent     = 0

for ($r = 0; $r -lt $rounds; $r++) {
    $n = [math]::Min($perRound, $Repeat - $sent)
    if ($n -gt 0) {
        foreach ($c in $conns) {
            for ($k = 0; $k -lt $n; $k++) { $c.Stream.Write($inv, 0, $inv.Length) }
            $c.Stream.Flush()
        }
        $sent += $n
    }
    if ($Traders -gt 0) { Invoke-TradeRound }
}

# ── 응답을 전부 받을 때까지 ──────────────────────────────────────────
#   kInventoryAck 의 body 첫 바이트가 result 코드다(packet.h:107 —
#   kInventoryAck = 104 // body: [result:u8][count:u16]… · 값은 packet.h:195-201
#   enum class ResultCode — kOk=0 · kBusy=4). ok/busy/other 로 나눠 센다.
#   qps 는 ok 기준이다.
$want  = $Clients * $sent
$ok = 0; $busy = 0; $other = 0; $total = 0
$buf   = [byte[]]::new(65536)
$deadline = [datetime]::UtcNow.AddMilliseconds($Timeout)

$pending = @{}
foreach ($c in $conns) { $pending[$c.Player] = [System.IO.MemoryStream]::new() }

while ($total -lt $want -and [datetime]::UtcNow -lt $deadline) {
    $any = $false
    foreach ($c in $conns) {
        if ($c.Stream.DataAvailable) {
            $any = $true
            $n = $c.Stream.Read($buf, 0, $buf.Length)
            if ($n -gt 0) { $pending[$c.Player].Write($buf, 0, $n) }
        }
    }
    if (-not $any) { Start-Sleep -Milliseconds 5 }

    # 프레임 세기 — ok/busy/other 분리
    $ok = 0; $busy = 0; $other = 0
    foreach ($c in $conns) {
        $d = $pending[$c.Player].ToArray()
        $off = 0
        while ($off + 4 -le $d.Length) {
            $len = Get-U16 $d $off
            if ($off + 4 + $len -gt $d.Length) { break }
            if ((Get-U16 $d ($off + 2)) -eq $MSG_INV_ACK) {
                $result = [int]$d[$off + 4]
                if ($result -eq 0) { $ok++ }
                elseif ($result -eq 4) { $busy++ }
                else { $other++ }
            }
            $off += 4 + $len
        }
    }
    $total = $ok + $busy + $other
}
$sw.Stop()

foreach ($c in $conns) { $pending[$c.Player].Dispose(); $c.Client.Close() }
foreach ($p in $traderPairs) {
    foreach ($c in @($p.A, $p.B)) { $c.Buf.Dispose(); $c.Client.Close() }
}

$ms  = [math]::Round($sw.Elapsed.TotalMilliseconds, 1)
$qps = if ($ms -gt 0) { [math]::Round($ok / ($ms / 1000.0), 0) } else { 0 }

Write-Host "sent    : $want 건  ($Clients 클라 x $sent)"
Write-Host "recv    : $total 건  (ok=$ok busy=$busy other=$other)"
if ($Traders -gt 0) {
    Write-Host "trades  : $($Traders * $TradeRepeat) 건 시도  ($Traders 쌍 x $TradeRepeat 회)"
}
Write-Host "time    : $ms ms   ($qps 건/초 · ok 기준)"
Write-Host ""
if ($total -lt $want) {
    Write-Host "판정  : X 응답이 모자란다 — 타임아웃이거나 DB 오류. 서버 로그를 볼 것"
} else {
    Write-Host "판정  : O 전부 응답 (ok=$ok busy=$busy other=$other)"
    if ($busy -gt 0) {
        Write-Host "        busy>0 — 풀이 모자랐다는 뜻이다(결함이 아니라 측정 결과)"
    }
}

# ── 정상 종료 → 서버 종료 로그에서 [POOL2]·[WORK ]·[NET  ] 파싱 ─────────────
#   zone_block.ps1:314-333 과 같은 방식(stdin 개행 → WaitForExit) — 그래야
#   [POOL2] 등 종료 지표가 찍힌다. Stop-Harness 가 finally 에서 다시 한 번
#   정상 종료를 시도하지만 이미 exited 라 안전하다(무해한 재확인).
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

$poolPeak  = Get-LogInt '\[POOL2\] db conns peak=(\d+)' $finalLog
$tryFailed = Get-LogInt '\[POOL2\][^\r\n]*try_failed=(\d+)' $finalLog
$capHits   = Get-LogInt '\[WORK \][^\r\n]*cap_hits=(\d+)' $finalLog
$resubmits = Get-LogInt '\[WORK \][^\r\n]*resubmits=(\d+)' $finalLog
$kicked    = Get-LogInt '\[NET  \][^\r\n]*send_full_kicked=(\d+)' $finalLog
$wActual   = Get-LogInt '\[INFO\] config:[^\r\n]*\bworkers=(\d+)' $finalLog

foreach ($miss in @(
    @{ N = 'poolPeak';  V = $poolPeak;  L = '[POOL2] peak' }
    @{ N = 'tryFailed'; V = $tryFailed; L = '[POOL2] try_failed' }
    @{ N = 'capHits';   V = $capHits;   L = '[WORK ] cap_hits' }
    @{ N = 'resubmits'; V = $resubmits; L = '[WORK ] resubmits' }
    @{ N = 'kicked';    V = $kicked;    L = '[NET  ] send_full_kicked' }
)) {
    if ($null -eq $miss.V) {
        Write-Host "미확인  : $($miss.L) — 종료 로그에서 못 찾음" -ForegroundColor Yellow
    }
}

$wReport = if ($AppWorkers -ne 0) { $AppWorkers } elseif ($null -ne $wActual) { $wActual } else { 'ini' }
$pReport = if ($PoolSize -ne 0) { $PoolSize } else { 'ini' }

Write-Host ("RESULT workers={0} pool={1} clients={2} repeat={3} traders={4} ok={5} busy={6} other={7} ms={8} qps={9} pool2_peak={10} try_failed={11} cap_hits={12} resubmits={13} kicked={14}" -f `
    $wReport, $pReport, $Clients, $Repeat, $Traders, $ok, $busy, $other, $ms, $qps, `
    (Format-MaybeInt $poolPeak), (Format-MaybeInt $tryFailed), (Format-MaybeInt $capHits), (Format-MaybeInt $resubmits), (Format-MaybeInt $kicked))
} finally {
    Stop-Harness $scratchRoot $listener $villageProc
}
