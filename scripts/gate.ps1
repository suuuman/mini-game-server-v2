# scripts\gate.ps1 — 예약 게이트 부정 경로 검증
#
#   8종이 통과하는 것은 "게이트가 막는다"의 증명이 아니다 — 8종은 예약 경유로
#   개조된 뒤라 전부 인증된 세션이고, 게이트를 통째로 빼도 8종은 그대로
#   전부 통과한다. 이 하네스가 예약 게이트의
#   존재 증명이다 — "미경유 접속이 실제로 거부된다"를 직접 잰다.
#
#   각 항목의 판정 방법이 기대만큼 중요하다. 통과하는데도 메커니즘이 하나도
#   안 돈 경우를 하나씩 차단한다:
#     N1  Enter 없이 kJoinZoneReq  → 끊긴다. 게이트는 session.player_id
#         하나만 본다(on_frame — frame_router.cpp:1269, 열어서 재확인) —
#         placement 필드 자체가 없어져서 "어느 필드를 보는가"의 모호함은
#         없어졌다. 그래도 N6 은 유지한다 — "정상 경로에서 게이트가 안
#         걸린다"는 별개의 사실이라 N1(거부)만으로는 증명이 안 된다.
#     N2  Enter 없이 kChatReq      → 끊긴다. 같은 게이트가 여러 case 에
#         실제로 걸리는지의 배선 확인.
#     N3  Enter 없이 kInventoryReq → 끊긴다. handle_inventory 자체에 이미
#         player_id==0 검사가 있어(kNotLoggedIn 응답 · 안 끊음) 게이트를
#         빼먹어도 "응답이 오면서 연결은 산다" — 그래서 판정은 반드시
#         "연결이 끊겼다"쪽이지 "정상 응답이 왔다"가 아니다.
#     N4a Enter 없이 미정의 id=5   → 끊긴다(fail-closed 증명). 조건 없이
#         건 게이트만 이걸 잡는다 — case 목록에만 건 게이트는 통과시킨다.
#     N4b Enter 성공 후 미정의 id=5 → handle_unhandled 로 가 bad_msg_score
#         만 오르고 안 끊긴다(§8-4 류 관용은 "자리를 얻은 세션"에 주는
#         것이지 미인증 소켓에 주는 게 아니다). N4a 하나만으로는 게이트가
#         "입장한 세션의 미정의 id 까지" 과도하게 끊는지 못 잡는다.
#     N5  예약 없이 Enter          → kInvalidArg · 연결 유지. player_id==0
#         가드와 예약 검사는 같은 result 코드를 내므로 서버 로그의
#         "예약 없음/만료" 문자열로 갈라야 한다.
#     N6  예약 발급 후 Enter → kJoinZoneReq → 정상 통과. Enter 는
#         session.zone 을 전혀 안 건드린다(쓰기는 handle_join_zone 안
#         한 곳뿐 — frame_router.cpp:483, 열어서 재확인) — Enter 직후는
#         player_id!=0·zone==0(초기값) 이다. 게이트가 player_id==0 검사를
#         빼먹거나 다른 조건으로 잘못 짰다면 여기서만 실패한다 — N1(거부
#         경로)이 못 가르는 "정상 경로가 막히지 않는다" 축을 채운다.
#
#   Connect-Reserved 등은 harness_common.ps1(8종과 공유)에서 그대로 가져온다.
#   N1~N4b 는 그 헬퍼가 항상 Enter 까지 끝내 버리므로 안 쓴다 — 이 넷은
#   "Enter 를 안 거친 소켓"이 핵심이라 raw TcpClient 로 직접 연결한다.
#
#   사용:
#     .\gate.ps1                      # 전체 흐름 한 번
#     .\gate.ps1 -Config ASan         # ASan 구성으로

param(
    [int]$Port      = 9000,
    [string]$Config = 'Release',
    [int]$Seconds   = 90,
    [int]$Timeout   = 3000
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'harness_common.ps1')

# ── 이 파일만의 msg_id — 나머지는 harness_common.ps1 의 $Harness_* 를 쓴다 ──
$Gate_VilJoinZoneReq = 2      # proto::MsgId::kJoinZoneReq   body: [ zone_id : u32 ]
$Gate_VilChatReq     = 3      # proto::MsgId::kChatReq       body: [ type:u8 ][ target:u64 — type=2 만 ][ text... ]
                              #   N2 는 게이트(미인증)가 body 파싱 전에 끊으므로 빈 body 로 충분하다
$Gate_VilInventoryReq = 4     # proto::MsgId::kInventoryReq  body: 없음
$Gate_VilJoinZoneAck = 102    # proto::MsgId::kJoinZoneAck
$Gate_UndefinedId    = 5      # kLoginReq 가 폐지되며 빈 값 — "미정의 id" 대역 대표로 쓴다
$Gate_ResultInvalidArg = 3    # proto::ResultCode::kInvalidArg

function New-U32BE([uint32]$v) {
    $b = [byte[]]::new(4)
    $b[0] = [byte](($v -shr 24) -band 0xFF); $b[1] = [byte](($v -shr 16) -band 0xFF)
    $b[2] = [byte](($v -shr  8) -band 0xFF); $b[3] = [byte]( $v          -band 0xFF)
    return ,$b
}

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
#   영원히 false 다. 그래서 Read-ClientFrame(harness_common.ps1) 로는 "끊겼다"
#   와 "그냥 응답이 없다"가 똑같이 타임아웃으로 보여 구분이 안 된다.
#   Socket.Poll(SelectRead) 로 직접 물어야 "읽기 가능해졌는데 내용이 0바이트"
#   (=상대가 닫았다)를 "아직 아무 일도 없다"와 가른다.
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

function Test-Name([string]$n) { Write-Host ''; Write-Host "== $n ==" }
function Check([string]$what, [bool]$ok) {
    $mark = if ($ok) { 'O' } else { 'X' }
    Write-Host "   [$mark] $what"
    return $ok
}

$pass = 0; $fail = 0
function Note([bool]$ok) { if ($ok) { $script:pass++ } else { $script:fail++ } }

$scratchRoot = Join-Path $env:TEMP ("gate_harness_" + [guid]::NewGuid().ToString('N'))
$listener = $null
$villageProc = $null
try {
$vhome = New-HarnessHome $scratchRoot ([string]$Port)
$listener = Start-FakeSession 9100
$villageProc = Start-Village $Config $vhome $Seconds
$link = Accept-FakeSessionLink $listener
$serverLog = Join-Path $vhome 'logs\server.log'

# ── N1 — Enter 없이 kJoinZoneReq ────────────────────────────────────────
Test-Name 'N1 — 미인증 kJoinZoneReq 는 끊긴다'
$h1 = Connect-Plain $Port $Timeout
Send-ClientFrame $h1.Stream $Gate_VilJoinZoneReq (New-U32BE 1)
$r1 = Wait-ConnectionOutcome $h1 1500
Note (Check "연결이 끊겼다 (frame=$(if ($r1.Frame) { $r1.Frame.MsgId } else { '-' }))" $r1.Closed)
try { $h1.Client.Close() } catch {}

# ── N2 — Enter 없이 kChatReq ─────────────────────────────────────────────
Test-Name 'N2 — 미인증 kChatReq 는 끊긴다'
$h2 = Connect-Plain $Port $Timeout
Send-ClientFrame $h2.Stream $Gate_VilChatReq ([byte[]]::new(0))
$r2 = Wait-ConnectionOutcome $h2 1500
Note (Check "연결이 끊겼다 (frame=$(if ($r2.Frame) { $r2.Frame.MsgId } else { '-' }))" $r2.Closed)
try { $h2.Client.Close() } catch {}

# ── N3 — Enter 없이 kInventoryReq ───────────────────────────────────────
#   handle_inventory 자신의 player_id==0 가드는 kInventoryAck(kNotLoggedIn)
#   로 "응답하고 연결은 유지"한다 — 게이트가 빠지면 이 갈래로 새서 통과처럼
#   보인다. 그래서 Frame 이 비어 있는지까지 함께 본다.
Test-Name 'N3 — 미인증 kInventoryReq 는 끊긴다 (응답이 오면 게이트 부재)'
$h3 = Connect-Plain $Port $Timeout
Send-ClientFrame $h3.Stream $Gate_VilInventoryReq ([byte[]]::new(0))
$r3 = Wait-ConnectionOutcome $h3 1500
Note (Check "연결이 끊겼다 — kInventoryAck 응답 없음 (frame=$(if ($r3.Frame) { $r3.Frame.MsgId } else { '(없음)' }))" `
    ($r3.Closed -and ($null -eq $r3.Frame)))
try { $h3.Client.Close() } catch {}

# ── N4a — Enter 없이 미정의 id ───────────────────────────────────────────
Test-Name 'N4a — Enter 없이 미정의 id 는 끊긴다 (fail-closed)'
$h4a = Connect-Plain $Port $Timeout
Send-ClientFrame $h4a.Stream $Gate_UndefinedId ([byte[]]::new(0))
$r4a = Wait-ConnectionOutcome $h4a 1500
Note (Check "연결이 끊겼다 (frame=$(if ($r4a.Frame) { $r4a.Frame.MsgId } else { '-' }))" $r4a.Closed)
try { $h4a.Client.Close() } catch {}

# ── N4b — Enter 성공 후 미정의 id ───────────────────────────────────────
#   N4a 의 반대 방향이다 — 둘 다 있어야 게이트의 범위(누구를 끊고 누구를
#   안 끊는지)가 확정된다.
Test-Name 'N4b — Enter 성공 후 미정의 id 는 안 끊긴다 (관용은 입장한 세션 몫)'
$logBefore4b = (Read-ServerLog $serverLog).Length
$h4b = Connect-Reserved $link $Port ([uint64]90001) $Timeout
Send-ClientFrame $h4b.Stream $Gate_UndefinedId ([byte[]]::new(0))
$r4b = Wait-ConnectionOutcome $h4b 1500
Send-ClientFrame $h4b.Stream $Gate_VilJoinZoneReq (New-U32BE 91)
$follow4b = Read-ClientFrame $h4b.Stream $Timeout
$aliveOk = (-not $r4b.Closed) -and ($null -ne $follow4b) -and ($follow4b.MsgId -eq $Gate_VilJoinZoneAck)
$log4bText = (Read-ServerLog $serverLog)
$log4bNew = if ($log4bText.Length -gt $logBefore4b) { $log4bText.Substring($logBefore4b) } else { '' }
$sawUnhandled = $log4bNew -match [regex]::Escape("unhandled msg_id=$Gate_UndefinedId")
Note (Check "연결 유지 + 뒤이은 kJoinZoneReq 정상 처리 (안 끊김 확인)" $aliveOk)
Note (Check "서버 로그에 unhandled msg_id=$Gate_UndefinedId 기록 (메커니즘이 실제로 돌았다)" $sawUnhandled)
try { $h4b.Client.Close() } catch {}

# ── N5 — 예약 없이 Enter ─────────────────────────────────────────────────
#   player_id==0 가드와 예약 검사가 같은 kInvalidArg 를 낸다 — 로그의
#   "예약 없음/만료" 문자열로 갈라야 어느 검사가 실제로 발동했는지 안다.
Test-Name 'N5 — 예약 없이 Enter 는 InvalidArg · 연결 유지'
$logBefore5 = (Read-ServerLog $serverLog).Length
$h5 = Connect-Plain $Port $Timeout
Send-ClientFrame $h5.Stream $Harness_VilEnterReq (New-EnterBody ([uint64]90002))
$ack5 = Read-ClientFrame $h5.Stream $Timeout
$ack5Ok = ($null -ne $ack5) -and ($ack5.MsgId -eq $Harness_VilEnterAck)
$body5 = if ($ack5Ok) { ConvertFrom-EnterAckBody $ack5.Body } else { $null }
$r5 = Wait-ConnectionOutcome $h5 800
$log5Text = (Read-ServerLog $serverLog)
$log5New = if ($log5Text.Length -gt $logBefore5) { $log5Text.Substring($logBefore5) } else { '' }
$sawNoReservation = $log5New -match '예약 없음/만료'
Note (Check "kEnterAck result=$(if ($body5) { $body5.Result } else { '-' })(기대 $Gate_ResultInvalidArg)" `
    ($ack5Ok -and ($body5.Result -eq $Gate_ResultInvalidArg)))
Note (Check "연결이 안 끊겼다" (-not $r5.Closed))
Note (Check "서버 로그에 '예약 없음/만료' 기록 (player_id==0 가드가 아니라 예약 검사가 발동)" $sawNoReservation)
try { $h5.Client.Close() } catch {}

# ── N6 — 예약 발급 후 Enter → kJoinZoneReq ──────────────────────────────
#   Enter 는 session.placement 를 안 건드린다(handle_join_zone 안 3곳만
#   쓴다) — 그래서 Enter 직후는 player_id!=0 · placement==0 이다. 게이트를
#   session.placement 로 잘못 짜면 이 case 만 골라 실패한다.
Test-Name 'N6 — 예약 경유 Enter 뒤 kJoinZoneReq 는 정상 통과'
$h6 = Connect-Reserved $link $Port ([uint64]90003) $Timeout
Send-ClientFrame $h6.Stream $Gate_VilJoinZoneReq (New-U32BE 92)
$join6 = Read-ClientFrame $h6.Stream $Timeout
$join6Ok = ($null -ne $join6) -and ($join6.MsgId -eq $Gate_VilJoinZoneAck)
if (-not $join6Ok) {
    Write-Host '   N6 실패 — N1~N5 가 전부 통과했다면 게이트 조건이'
    Write-Host '             session.player_id 대신 session.placement 를 보고 있을 가능성을 먼저 확인하라'
}
Note (Check "kJoinZoneAck 수신" $join6Ok)
try { $h6.Client.Close() } catch {}

# ── N7 — 같은 player_id 로 예약 2회 발급 후 각각 다른 연결로 Enter 2회 ──
#   entry.enter() 의 중복 거부(InvalidArg) 분기는 session.ps1 의 P6-7b 원형이
#   재던 결정적 커버리지였는데, 그 시나리오가 Kick 시나리오(K 계열)로
#   개편되며 이 분기를 더는 안 본다(동기 Kick 이 두 번째 Login 을 세션
#   서버 단계에서 먼저 거절하므로 village 까지 안 온다) — 그래서 이
#   분기를 실제로 타는 유일한 커버리지가 여기로 옮겨 온다.
Test-Name 'N7 — 같은 player_id 중복 Enter 는 두 번째만 InvalidArg · 연결 유지'
$h7a = Connect-Reserved $link $Port ([uint64]90005) $Timeout
Grant-Reservation $link ([uint64]90005)
$h7b = Connect-Plain $Port $Timeout
Send-ClientFrame $h7b.Stream $Harness_VilEnterReq (New-EnterBody ([uint64]90005))
$ack7b = Read-ClientFrame $h7b.Stream $Timeout
$ack7bOk = ($null -ne $ack7b) -and ($ack7b.MsgId -eq $Harness_VilEnterAck)
$body7b = if ($ack7bOk) { ConvertFrom-EnterAckBody $ack7b.Body } else { $null }
$r7b = Wait-ConnectionOutcome $h7b 800
Note (Check "2차 Enter(같은 player, 다른 연결) result=$(if ($body7b) { $body7b.Result } else { '-' })(기대 $Gate_ResultInvalidArg) · 연결 유지=$(-not $r7b.Closed)" `
    ($ack7bOk -and ($body7b.Result -eq $Gate_ResultInvalidArg) -and (-not $r7b.Closed)))
try { $h7a.Client.Close() } catch {}
try { $h7b.Client.Close() } catch {}

Write-Host ''
if ($fail -eq 0) {
    Write-Host "판정  : O $pass 개 전부 통과"
} else {
    Write-Host "판정  : X $fail 개 실패 / $($pass + $fail) 개 — 서버 로그의 [WARN] 을 볼 것"
}
} finally {
    Stop-Harness $scratchRoot $listener $villageProc
}

if ($fail -gt 0) { exit 1 }
exit 0
