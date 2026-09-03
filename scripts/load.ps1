# scripts\load.ps1 — 여러 접속을 동시에 붙잡고 왕복 지연을 잰다
#
#   send.ps1 은 한 번 주고받고 닫는다. 그래서 「동시 접속」을 만들 수 없다.
#   이 스크립트는 N 개를 먼저 전부 연결해 두고, 요청을 한꺼번에 던진 뒤
#   응답이 각각 언제 돌아오는지를 잰다.
#
#   스레드를 쓰지 않는다.
#     보내는 일은 순식간이므로, 8개를 연달아 write 하면 8개가 거의 동시에 서버에 도착한다.
#     그 다음 응답이 「몇 번째로」 돌아오는지가 곧 서버가 얼마나 병렬로 처리했는지다.
#
#   사용법:
#     1) app\bootstrap.cpp 의 kIoWorkerCount = 1  + on_recv 에 Sleep(50) → .\load.ps1 -Clients 8
#     2) kIoWorkerCount = 4              (Sleep 유지)          → .\load.ps1 -Clients 8
#     두 결과의 max / spread 를 비교한다.

param(
    [int]$Port    = 9000,
    [int]$Clients = 8,
    [int]$Rounds  = 3,
    [int]$Size    = 8,
    [int]$MsgId   = 1,
    [int]$Timeout = 10000,
    [switch]$Framed
)

$ErrorActionPreference = 'Stop'

function New-Frame([byte[]]$Body, [int]$Id) {
    $h = [byte[]]::new(4)
    $h[0] = [byte](($Body.Length -shr 8) -band 0xFF)
    $h[1] = [byte]( $Body.Length         -band 0xFF)
    $h[2] = [byte](($Id          -shr 8) -band 0xFF)
    $h[3] = [byte]( $Id                  -band 0xFF)
    return $h + $Body
}

$body = [System.Text.Encoding]::ASCII.GetBytes('A' * $Size)
$unit = if ($Framed) { New-Frame $body $MsgId } else { $body }

# ── 1. 전부 먼저 연결한다. 이 시점에 서버에는 $Clients 개의 세션이 살아 있다 ──
$conns = @()
for ($i = 0; $i -lt $Clients; $i++) {
    $c = [System.Net.Sockets.TcpClient]::new("127.0.0.1", $Port)
    $c.NoDelay = $true          # 측정이 목적이라 송신 지연 요인을 뺀다
    $conns += [pscustomobject]@{
        Index  = $i + 1
        Client = $c
        Stream = $c.GetStream()
        Got    = 0
        Ms     = $null
    }
}
"connected: $Clients   unit=$($unit.Length)B   framed=$($Framed.IsPresent)"
""

$buf = [byte[]]::new(65536)
$allMax = @()

for ($r = 1; $r -le $Rounds; $r++) {

    foreach ($x in $conns) { $x.Got = 0; $x.Ms = $null }

    # ── 2. 요청을 한꺼번에 던진다 ──
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    foreach ($x in $conns) { $x.Stream.Write($unit, 0, $unit.Length) }

    # ── 3. 누가 언제 돌아오는지 잰다. Start-Sleep 을 쓰지 않는다 ──
    #      PowerShell 의 Sleep 은 타이머 해상도 때문에 최소 ~15ms 를 자서 측정을 망친다.
    $pending = $Clients
    while ($pending -gt 0 -and $sw.ElapsedMilliseconds -lt $Timeout) {
        foreach ($x in $conns) {
            if ($null -ne $x.Ms) { continue }
            if ($x.Stream.DataAvailable) {
                $n = $x.Stream.Read($buf, 0, $buf.Length)
                $x.Got += $n
                if ($x.Got -ge $unit.Length) {
                    $x.Ms = $sw.ElapsedMilliseconds
                    $pending--
                }
            }
        }
    }
    $sw.Stop()

    $done = @($conns | Where-Object { $null -ne $_.Ms })
    $lost = $Clients - $done.Count

    if ($done.Count -eq 0) {
        "round $r : 전부 타임아웃"
        continue
    }

    $ms  = @($done | ForEach-Object { $_.Ms } | Sort-Object)
    $min = $ms[0]
    $max = $ms[-1]
    $avg = [math]::Round(($ms | Measure-Object -Average).Average, 1)
    $allMax += $max

    $order = ($done | Sort-Object Ms | ForEach-Object { "#$($_.Index):$($_.Ms)" }) -join '  '

    "round $r : min=${min}ms  avg=${avg}ms  max=${max}ms  spread=$($max - $min)ms" +
        $(if ($lost -gt 0) { "  (타임아웃 $lost 개)" } else { "" })
    "          $order"
}

""
if ($allMax.Count -gt 0) {
    $worst = ($allMax | Measure-Object -Maximum).Maximum
    "worst max = ${worst}ms   ← 워커 수를 바꿔 가며 이 값을 비교한다"
}

foreach ($x in $conns) { $x.Client.Close() }
