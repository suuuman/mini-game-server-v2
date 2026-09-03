# scripts\zone_block.ps1 — 「DB 폭탄이 커넥션 풀을 다 잠가도 무관한 요청이 밀리는가」 측정
#
#   옛 구조에서는 이 하네스가 「존 스레드 하나를 한 세션이 독점하는가」를 봤다
#   (동기 DB 왕복이 그 존 스레드를 막아 같은 존의 다른 세션까지 밀리는 축).
#   존 스레드(zone_manager.cpp)가 지워지고 handle_inventory 가
#   try_acquire 기반 1홉 동기 호출로 바뀌면서(§7-4) 그 축 자체가 없어졌다 —
#   커넥션을 못 빌리면 그 자리에서 즉시 kBusy 를 돌려주지 재큐잉·대기를
#   안 한다. 그래서 이 하네스가 겨누는 축도 바뀐다:
#
#   지금 무엇을 보는가 —
#     플러드 세션 여러 개(FloodClients, 기본 [db] pool_size(4) 와 [app]
#     workers(8) 둘 다보다 많이)가 동시에 kInventoryReq 를 쏟아 DB 커넥션
#     풀을 전부 점유하게 만든 채로, DB 를 전혀 안 타는 B(kEchoReq — F-3:
#     게이트도 안 밟는다)의 왕복을 잰다. 워커는 세션마다 직렬 큐가 따로라
#     플러드 세션들이 워커 몇 개를 물고 있어도 B 는 자신의 직렬 큐 실행권만
#     받으면 되므로, "DB 풀이 꽉 찼다"와 "B 가 밀린다"는 이제 서로 다른
#     질문이다 — 이 하네스는 그 둘이 실제로 분리돼 있는지를 잰다.
#     ⚠️ FloodClients 는 워커 수(8)도 넘겨야 한다 — pool_size(4) 만 넘기고
#     워커 수보다 적으면(예: 6) 빈 워커가 남아 B 가 그리로 배정돼 판정이
#     안 갈린다(M4 뮤턴트로 실측 확인 — 6개일 때 거짓 통과가 났다).
#
#   ⛔ 위 판정만으로는 "풀이 실제로 고갈됐다"는
#     직접 증거가 없다(B 가 안 밀린 것이 "풀이 안 고갈돼서"인지 "고갈돼도
#     안 밀려서"인지 이 판정 하나로는 안 갈린다). 그래서 두 판정을 더한다:
#     ① 종료 로그의 [POOL2] try_failed(db_pool.h — try_acquire 가 그 자리서
#        실패한 횟수) 를 파싱해 > 0 을 단언한다 — 이게 "풀이 실제로 꽉 찼다"의
#        직접 증거다. 0 이면 플러드가 애초에 풀을 못 잠근 것이라 이 시나리오
#        자체가 무효다(FAIL).
#     ② 풀이 고갈된 바로 그 구간에 별도 세션(C)이 kTradeConfirmReq 를 쏴
#        본다 — handle_trade_confirm 은 §7-4 로 try_acquire 를 거래 상태
#        검사보다 먼저 하므로(frame_router.cpp:907-921, 재확인할 것 —
#        라인은 조사 시점 값이다) 풀이 없으면 거래 유무와 무관하게 즉시
#        kBusy 다. B(에코)만으로는 handle_inventory 경로 하나만 보였는데,
#        이 관측은 "풀 고갈이 try_acquire 를 쓰는 다른 핸들러에도 같은
#        모양으로 보이는가"를 추가로 확인한다.
#
#   판정 — 부하 중 B 왕복 max 가 기준선 max + 50ms 안이면 통과.
#     50ms 자체는 안 쟀다(여유값) — 근거는 하드웨어 편차 흡수용 상수라는
#     것뿐이고, 엄밀한 SLA 가 아니다.
#
#   사용:
#     .\zone_block.ps1                    # 기본 (풀·워커 수보다 많은 10개 플러드 세션)
#     .\zone_block.ps1 -FloodClients 16 -Floods 40

param(
    [int]$Port         = 9000,
    [int]$FloodClients = 10,      # 동시에 인벤토리를 쏟는 세션 수 — [db] pool_size(4) 뿐 아니라
                                   #   [app] workers(8) 도 넘겨야 한다(M4 뮤턴트 실측 — 6개로는
                                   #   워커 8개 중 6개만 물려 B 가 빈 워커를 받아 통과가 안 갈린다.
                                   #   — M4 뮤턴트 실측 회차의 결론이다)
    [int]$Floods       = 30,      # 세션당 던질 인벤토리 요청 수
    [int]$Probes       = 10,      # B 가 잴 에코 왕복 횟수
    [int]$ConfirmTries = 5,       # 풀 고갈 구간을 노려 kTradeConfirmReq 를 쏘는 관측 세션 수 —
                                   #   ⚠️ 이 관측은 확률적이다(s2s.ps1 ⑱ 과 같은 성격 — 실측 다회차
                                   #   중 대략 3/4 hit, 세션 수를 5→8 로 늘려도 표본 4회 안에서는
                                   #   개선이 안 보였다·100% 는 아니다). 판정2 가 유일하게 실패해도
                                   #   판정3(try_failed>0)이 「풀이 실제로 꽉 찼다」를 이미 결정적으로
                                   #   증명하므로 그 실패를 "메커니즘 없음"으로 오독하지 않는다.
    [int]$Timeout      = 10000,
    [string]$Config  = 'Release',   # 예약 경유 접속을 위해 스스로 스폰할 village.exe 구성
    [int]$Seconds    = 60           # 스폰한 village.exe 의 수명(초)
)

$ErrorActionPreference = 'Stop'

# kLoginReq/직행 접속이 폐지돼서 플러드 세션(예약 경유)은 예약을
#   거쳐야 한다. 실물 session.exe 는 안 띄운다 — 이 파일이 가짜 세션 서버
#   노릇을 해서 스스로 띄운 village.exe 에 직접 Reserve 를 찔러 넣는다(harness_common.ps1 참조).
#   B(에코 전용)는 손대지 않는다 — kEchoReq 는 1단계 switch 에서 즉시
#   반환돼 게이트/로그인 여부와 무관하다(§7 F-3).
. (Join-Path $PSScriptRoot 'harness_common.ps1')

$MSG_INV_REQ            = 4
$MSG_ECHO_REQ           = 1
$MSG_TRADE_CONFIRM_REQ  = 9      # proto::MsgId::kTradeConfirmReq — body: [ confirm : u8 ]
$MSG_TRADE_ACK          = 109    # proto::MsgId::kTradeAck — body: [ result : u8 ][ peer_session : u32 ]
$RESULT_BUSY            = 4      # proto::ResultCode::kBusy

function New-Frame([byte[]]$Body, [int]$Id) {
    $len = if ($null -eq $Body) { 0 } else { $Body.Length }
    $h = [byte[]]::new(4)
    $h[0] = [byte](($len -shr 8) -band 0xFF)
    $h[1] = [byte]( $len         -band 0xFF)
    $h[2] = [byte](($Id  -shr 8) -band 0xFF)
    $h[3] = [byte]( $Id          -band 0xFF)
    if ($len -eq 0) { return $h }
    return $h + $Body
}
function New-Client([int]$p) {
    $c = New-Object System.Net.Sockets.TcpClient
    $c.NoDelay = $true
    $c.Connect('127.0.0.1', $p)
    $s = $c.GetStream()
    $s.ReadTimeout = $Timeout; $s.WriteTimeout = $Timeout
    return @{ Client = $c; Stream = $s }
}

# drain.ps1·gate.ps1·idle.ps1 과 같은 헬퍼다(사본 — find_copies.ps1 이 동기화를
#   지킨다) — 서버가 쓰는 중인 로그를 FileShare.ReadWrite 로 열어야 공유
#   충돌 없이 읽는다.
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

$scratchRoot = Join-Path $env:TEMP ("zone_block_harness_" + [guid]::NewGuid().ToString('N'))
$listener = $null
$villageProc = $null
$vhome = $null
try {
$vhome = New-HarnessHome $scratchRoot ([string]$Port)
$listener = Start-FakeSession 9100
$villageProc = Start-Village $Config $vhome $Seconds
$link = Accept-FakeSessionLink $listener

"flood : $FloodClients 개 세션 동시 투입 (풀 크기 기본 4 를 넘긴다) x $Floods 건씩"

# 플러드 세션 전부를 예약 경유로 Enter 시킨다. 안 하면 서버가 kNotLoggedIn 을
#   즉시 돌려주고 DB 를 아예 타지 않아, 「DB 요청이 풀을 잠그는가」라는 이
#   측정 자체가 무의미해진다. Connect-Reserved 가 kEnterAck 확인까지 끝내므로
#   응답을 따로 안 읽어도 된다.
$floodSessions = @()
for ($i = 0; $i -lt $FloodClients; $i++) {
    $reserved = Connect-Reserved $link $Port ([uint64]($i + 1)) $Timeout
    $floodSessions += @{ Client = $reserved.Client; Stream = $reserved.Stream }
}
$B = New-Client $Port      # 에코 왕복 측정 — F-3: kEchoReq 는 게이트를 안 밟는다

# C1..CN — 풀 고갈 구간에 kTradeConfirmReq 를 하나씩 쏠 관측 전용 세션
#   ConfirmTries 개. 세션을 여러 개 쓰는 이유 — 한 세션에 여러 프레임을
#   몰아넣으면 직렬 큐 하나가 한 워커에게 한 배치로 뽑혀 「전부 같은 순간에
#   같이 처리」된다(worker_pool.h — 세션당 in-flight 실행권 1개). 그러면
#   창을 하나만 재는 것과 같아 명중률이 낮다(실측: 세션 1개·프레임 5개로는
#   3회 중 1회만 kBusy 를 잡았다). 세션을 나누면 각자 다른 워커에 다른
#   시점으로 배정될 수 있어 같은 시도 수로 창을 더 넓게 훑는다.
#   TradeConfirmReq 도 게이트를 밟으므로(player_id!=0 필요) 예약 경유로
#   미리 Enter 시켜 둔다 — 거래 상대는 필요 없다. try_acquire 실패는
#   session.trade 를 보기 전에 나기 때문이다(frame_router.cpp:921-929).
$confirmSessions = @()
for ($i = 0; $i -lt $ConfirmTries; $i++) {
    $reservedC = Connect-Reserved $link $Port ([uint64]($FloodClients + 2 + $i)) $Timeout
    $confirmSessions += @{ Client = $reservedC.Client; Stream = $reservedC.Stream }
}

# ── 1. 기준선 — 플러드가 조용할 때 B 의 왕복 ─────────────────────────
$echo = New-Frame ([byte[]]::new(8)) $MSG_ECHO_REQ
$buf  = [byte[]]::new(4096)

function Measure-Roundtrip($cli, [int]$n) {
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
    return $times
}

$base = Measure-Roundtrip $B $Probes
$baseAvg = [math]::Round(($base | Measure-Object -Average).Average, 2)
$baseMax = [math]::Round(($base | Measure-Object -Maximum).Maximum, 2)
"기준선 : B 에코 왕복  avg=$baseAvg ms  max=$baseMax ms   (플러드 조용함)"

# ── 2. 플러드 세션 전부가 인벤토리 요청을 동시에 쏟는다(응답은 안 기다린다) ──
#      동시에 여러 세션에서 나가야 풀(4)이 실제로 꽉 찬다 — 세션 하나만
#      쏘면 그 세션 직렬 큐가 순서대로 비워질 뿐이라 동시 점유 개수가 1이다.
$inv = New-Frame $null $MSG_INV_REQ      # body 없음. 대상은 세션이 안다
foreach ($fs in $floodSessions) {
    for ($i = 0; $i -lt $Floods; $i++) {
        $fs.Stream.Write($inv, 0, $inv.Length)
    }
    $fs.Stream.Flush()
}

# ── 2b. C1..CN 각자 kTradeConfirmReq 하나씩을 「응답을 기다리지 않고」
#        쏜다(위 플러드 write 와 같은 정신). 왜 여기서 응답까지 안 읽는가 —
#        왕복까지 기다리면 그 자체가 지연이 되어, 뮤턴트(M4·워커 전원 점유)
#        에서는 그 대기가 플러드 배수 시간을 벌어줘 버려서 B 측정이
#        「이미 뚫린 뒤」를 재는 거짓 통과를 냈다(뮤턴트 재검증 실측).
#        쏘기만 하고 넘어가면 그 문제가 없다 — 요청이 큐에 서는 시점은 플러드와
#        같고, B 측정 타이밍도 안 밀린다.
$confirmFrame = New-Frame ([byte[]]@(1)) $MSG_TRADE_CONFIRM_REQ
foreach ($cs in $confirmSessions) {
    $cs.Stream.Write($confirmFrame, 0, $confirmFrame.Length)
    $cs.Stream.Flush()
}

# ── 3. 그 직후(플러드 직후 — 지연 없이 바로) B 의 왕복을 다시 잰다 ──────
$under = Measure-Roundtrip $B $Probes
$underAvg = [math]::Round(($under | Measure-Object -Average).Average, 2)
$underMax = [math]::Round(($under | Measure-Object -Maximum).Maximum, 2)
"부하중 : B 에코 왕복  avg=$underAvg ms  max=$underMax ms   (플러드 $FloodClients 세션 x $Floods 건)"

# ── 3b. C1..CN 각자의 응답을 걷는다(세션마다 독립 소켓이라 순서 무관).
#        하나라도 kBusy 면 "풀 고갈이 이 경로에도 같은 모양으로 보인다"가
#        성립한다. 이 판정은 확률적이다(위 ConfirmTries 주석 — 3/4 hit 실측)
#        라 1회만 보고 실패로 단정하면 정상 코드도 거짓 실패를 낸다. 그래서
#        최대 3회까지 재시도한다 — 1회차는 이미 2b 에서 쏜 요청의 응답이고,
#        놓치면(무응답·timeout·busy 아닌 응답 포함) 플러드+confirm 을 다시
#        쏴 고갈 구간을 새로 만든다. B(에코) 측정(3)은 이미 끝난 뒤라 이
#        재시도가 그 타이밍을 밀지 않는다. 판정3(try_failed>0)이 "풀이
#        실제로 찼다"를 이미 결정적으로 증명하므로, 3회 전패는 종합 판정을
#        끌어내리지 않고 "미관측"으로만 보고한다(아래 최종 판정 참고).
#   위반 축(하드)과 관측 축(소프트)을 가른다. 개별 응답 하나로는 못 가른다 —
#   C1..CN 은 서로 다른 세션이라 각자 다른 직렬 큐 워커가 다른 시점에
#   집는다. 같은 플러드 구간 안에서도 어떤 프로브는 풀이 아직 안 찬 순간에
#   처리돼 정당하게 kBusy 아닌 응답을 받고, 다른 프로브는 그 사이 풀이 찬
#   순간에 처리돼 kBusy 를 받는다 — 그래서 개별 응답의 busy/non-busy 혼재는
#   정상이고(정상 코드 3연속 실측 — 매 회 busy·non-busy 가 함께 관측됐다),
#   「전부 kBusy 여야 한다」는 전제 자체가 이 구조와 안 맞는다. 대신 「시도를
#   다 쓰고도 kBusy 를 단 한 번도 못 받았는가」로 축을 잡는다 — §7-4 가
#   역전되면 거래 검사가 try_acquire 보다 먼저 돌아 kBusy 가 원천적으로
#   나올 수 없으므로, 「응답은 받았는데 busy 가 0건」이 그 결정적 시그니처다.
$confirmBusyHit = $false
$confirmViolation = $false
$confirmRead = 0
$maxConfirmAttempts = 3
$confirmAckHeader = [byte[]]::new(4)
$confirmAckBody   = [byte[]]::new(5)
for ($attempt = 1; $attempt -le $maxConfirmAttempts; $attempt++) {
    if ($attempt -gt 1) {
        foreach ($fs in $floodSessions) {
            for ($i = 0; $i -lt $Floods; $i++) {
                $fs.Stream.Write($inv, 0, $inv.Length)
            }
            $fs.Stream.Flush()
        }
        foreach ($cs in $confirmSessions) {
            $cs.Stream.Write($confirmFrame, 0, $confirmFrame.Length)
            $cs.Stream.Flush()
        }
    }
    foreach ($cs in $confirmSessions) {
        try {
            $gotH = 0
            while ($gotH -lt 4) {
                $r = $cs.Stream.Read($confirmAckHeader, $gotH, 4 - $gotH)
                if ($r -le 0) { break }
                $gotH += $r
            }
            if ($gotH -lt 4) { continue }
            $confirmRead++
            $bodyLen = ([int]$confirmAckHeader[0] -shl 8) -bor [int]$confirmAckHeader[1]
            $msgId   = ([int]$confirmAckHeader[2] -shl 8) -bor [int]$confirmAckHeader[3]
            $gotB = 0
            while ($gotB -lt $bodyLen -and $gotB -lt $confirmAckBody.Length) {
                $r = $cs.Stream.Read($confirmAckBody, $gotB, [math]::Min($bodyLen, $confirmAckBody.Length) - $gotB)
                if ($r -le 0) { break }
                $gotB += $r
            }
            if ($msgId -eq $MSG_TRADE_ACK -and $confirmAckBody[0] -eq $RESULT_BUSY) {
                $confirmBusyHit = $true
            }
        } catch {
            # ReadTimeout 등 — 이 세션은 놓친 것으로 보고 다음 세션을 본다
            continue
        }
    }
    if ($confirmBusyHit) { break }
}
# 응답은 받았는데(무응답만은 아니었는데) 그 안에 kBusy 가 한 번도 없다 — 위 머리말의
#   결정적 시그니처.
$confirmViolation = (-not $confirmBusyHit) -and ($confirmRead -gt 0)
"확인풀 : C1..C$ConfirmTries 의 kTradeConfirmReq — 최대 $maxConfirmAttempts 회 시도 · 누적 응답 $confirmRead 개 수신 — kBusy 관측=$confirmBusyHit · 위반(응답 있음·busy 0건)=$confirmViolation"

""
$ratio = if ($baseAvg -gt 0) { [math]::Round($underAvg / $baseAvg, 2) } else { 0 }
"배수   : avg $ratio 배"
$echoOk = $underMax -lt ($baseMax + 50)
if ($echoOk) {
    "판정1  : ○ DB 풀이 꽉 차도 B 가 밀리지 않았다 — try_acquire 가 즉시 실패해 워커를 안 묶는다"
} else {
    "판정1  : ✕ B 가 함께 밀렸다 — DB 점유가 워커/직렬 큐 실행을 막았다"
    "          (커넥션 풀 부족이 문제가 아니라 「그 부족을 기다린 것」이 문제다)"
}
if ($confirmBusyHit) {
    "판정2  : ○ TradeConfirm 경로도 풀 고갈 중 즉시 kBusy — try_acquire 우선 검사가 실제로 걸렸다"
} else {
    "판정2  : △ TradeConfirm 이 $maxConfirmAttempts 회 재시도에도 kBusy 를 못 봤다 — 확률 판정 미관측이다"
    "          (판정3 이 풀 고갈 자체는 이미 결정적으로 증명한다 — 이 판정은 종합 판정에 넣지 않는다)"
}
if ($confirmViolation) {
    "판정4  : ✕ §7-4 위반 — $maxConfirmAttempts 회 시도·응답 $confirmRead 개 중 kBusy 가 0건이다(try_acquire 가 거래 검사보다 뒤로 밀려 원천적으로 못 나온 것)"
} else {
    "판정4  : ○ §7-4 순서 위반 없음 — 응답 중 kBusy 가 최소 1건 있었다(무응답만으로는 이 판정이 안 걸린다)"
}

foreach ($fs in $floodSessions) { $fs.Client.Close() }
$B.Client.Close()
foreach ($cs in $confirmSessions) { $cs.Client.Close() }

# ── 4. 정상 종료시켜 [POOL2] try_failed 를 얻는다 — Stop-Harness 가 나중에
#      또 정상 종료를 시도하지만 이미 exited 라 안전하다(무해한 재확인).
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
$poolMatch = [regex]::Match($finalLog, '\[POOL2\] db conns[^\r\n]*try_failed=(\d+)')
$tryFailed = if ($poolMatch.Success) { [int]$poolMatch.Groups[1].Value } else { -1 }
$tryFailedOk = $tryFailed -gt 0
if ($tryFailedOk) {
    "판정3  : ○ [POOL2] try_failed=$tryFailed (>0) — 풀이 실제로 꽉 찼다는 직접 증거"
} else {
    "판정3  : ✕ [POOL2] try_failed=$tryFailed — 풀이 안 잠겼다(0 이면 이 시나리오 자체가 무효)"
}

""
if ($echoOk -and $tryFailedOk -and (-not $confirmViolation)) {
    "판정   : ○ 전 항목 통과 — 풀 고갈이 실제로 일어났고, 그 안에서도 무관 경로는 안 밀렸다"
    if (-not $confirmBusyHit) {
        "          (판정2 는 미관측 — 확률 판정이라 종합 판정에 포함하지 않는다)"
    }
} else {
    "판정   : ✕ 판정1·판정3·판정4 중 실패 항목을 볼 것(판정2 는 확률 판정이라 하드 게이트가 아니다)"
}
} finally {
    Stop-Harness $scratchRoot $listener $villageProc
}
