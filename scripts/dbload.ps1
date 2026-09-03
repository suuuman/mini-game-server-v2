# scripts\dbload.ps1 — DB 커넥션 풀 크기 실측 (L13-B)
#
#   무엇을 재는가
#     N개 연결이 「서로 다른 player_id」로 로그인해 인벤토리 조회를 한꺼번에 던진다.
#     player_id 가 다르면 워커도 다르므로(player_id % 워커수), 그래야 연결이 여러 개 쓰인다.
#     zone_block.ps1 은 player 1 하나만 쓴다 — 워커 하나만 돌아서 이 측정엔 안 맞는다.
#
#   판정은 「서버 종료 로그」에서 한다
#     [POOL2] db conns peak=? / ?  acquired=?  open_failed=?  discarded=?  try_failed=?
#       peak        동시에 실제로 쓰인 연결 수   ← 이게 「몇 개 필요한가」의 답
#       try_failed  못 빌려서 즉시 포기한 횟수    ← 0 이 아니면 풀이 모자라다
#
#   사용:
#     .\dbload.ps1 -Clients 8 -Repeat 50

param(
    [int]$Port    = 9000,
    [int]$Clients = 8,
    [int]$Repeat  = 50,
    [int]$Timeout = 20000,

    # 「자연스러운」 player_id 분포로 재기 위한 옵션.
    #
    #   기본(PlayerPool = 0)은 예전 그대로 player 1..Clients 를 쓴다 (회귀 호환).
    #   그런데 그 조건으로는 편중을 못 잰다 —
    #     Clients 가 워커수의 배수면 player_id % 워커수 가 정확히 균등해진다.
    #     8명 / 4워커 = 워커당 정확히 2명. 그건 잰 게 아니라 「만든 균등」이다.
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

    # 서버의 db workers 값. 클라가 「예상 분포」를 미리 찍어 두면,
    #   서버 종료 로그의 [SKEW ] 와 대조해서 계측 자체가 맞는지 확인할 수 있다.
    #   (7차 교훈 — 「무엇이 깨지는가」를 먼저 정하고 도구를 만든다)
    [int]$Workers = 4,

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
    $desc = "player 1..$Clients (연속 — 배수면 인위적 균등이니 편중 측정엔 부적합)"
}

# 클라가 「예상 분포」를 먼저 찍는다. 서버 [SKEW ] 와 이 값이 어긋나면
#   계측이 아니라 라우팅이 깨진 것이다 — 두 숫자가 서로를 검산한다.
$expect = @{}
foreach ($w in 0..($Workers - 1)) { $expect[$w] = 0 }
foreach ($id in $ids) { $expect[[int]($id % $Workers)] += $Repeat }
$emax = ($expect.Values | Measure-Object -Maximum).Maximum
$eavg = ($expect.Values | Measure-Object -Average).Average
$eskew = if ($eavg -gt 0) { [math]::Round($emax / $eavg, 2) } else { 0 }
$edist = (0..($Workers - 1) | ForEach-Object { "w$_=$($expect[$_])" }) -join ' '

$scratchRoot = Join-Path $env:TEMP ("dbload_harness_" + [guid]::NewGuid().ToString('N'))
$listener = $null
$villageProc = $null
try {
$vhome = New-HarnessHome $scratchRoot ([string]$Port)
$listener = Start-FakeSession 9100
$villageProc = Start-Village $Config $vhome $Seconds
$link = Accept-FakeSessionLink $listener

# ── 연결 + 로그인(예약 경유) — 이 왕복은 아래 Stopwatch 시작 "전"이라
#    QPS 측정에 안 섞인다. ──────────────────────────────────────────
$conns = @()
foreach ($id in $ids) {
    # player_id 를 전부 다르게 준다 — 그래야 워커가 갈린다
    $reserved = Connect-Reserved $link $Port ([uint64]$id) $Timeout
    $conns += @{ Client = $reserved.Client; Stream = $reserved.Stream; Player = $id }
}
Write-Host "connect : $Clients 개 — $desc"
Write-Host "expect  : $edist   max/avg = $eskew   (서버 [SKEW ] 와 대조할 것)"

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
$want  = $Clients * $sent
$total = 0
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

    # 프레임 세기
    $total = 0
    foreach ($c in $conns) {
        $d = $pending[$c.Player].ToArray()
        $off = 0
        while ($off + 4 -le $d.Length) {
            $len = Get-U16 $d $off
            if ($off + 4 + $len -gt $d.Length) { break }
            if ((Get-U16 $d ($off + 2)) -eq $MSG_INV_ACK) { $total++ }
            $off += 4 + $len
        }
    }
}
$sw.Stop()

foreach ($c in $conns) { $pending[$c.Player].Dispose(); $c.Client.Close() }
foreach ($p in $traderPairs) {
    foreach ($c in @($p.A, $p.B)) { $c.Buf.Dispose(); $c.Client.Close() }
}

$ms  = [math]::Round($sw.Elapsed.TotalMilliseconds, 1)
$qps = if ($ms -gt 0) { [math]::Round($total / ($ms / 1000.0), 0) } else { 0 }

Write-Host "sent    : $want 건  ($Clients 클라 x $sent)"
Write-Host "recv    : $total 건"
if ($Traders -gt 0) {
    Write-Host "trades  : $($Traders * $TradeRepeat) 건 시도  ($Traders 쌍 x $TradeRepeat 회)"
}
Write-Host "time    : $ms ms   ($qps 건/초)"
Write-Host ""
if ($total -lt $want) {
    Write-Host "판정  : X 응답이 모자란다 — 타임아웃이거나 DB 오류. 서버 로그를 볼 것"
} else {
    Write-Host "판정  : O 전부 응답. 서버 종료 로그의 [SKEW ] db queue wait 를 볼 것"
}
} finally {
    Stop-Harness $scratchRoot $listener $villageProc
}
