# scripts\session.ps1 — 2단계 세션 서버 통합 하네스 (session.exe + village.exe)
#
#   s2s.ps1 과 반대로 이 스크립트는 "클라이언트"만 흉내 낸다 — session.exe 의
#   클라 포트(9200)와 S2S 수용 포트(9100)에 접속해 로그인·가짜 마을 등록을
#   보낸다. 실물 village.exe 도 같이 띄워 실연동(#1~#5·#9·#10)을 본다.
#
#   판정 정본은 각 프로세스의 logs\server.log 다(stdout 리다이렉트는 버퍼링
#   절단 — s2s.ps1 과 같은 이유). 프로세스마다 스크래치 cwd 를 분리해 로그가
#   섞이지 않게 한다. 로그 판정은 바이트 오프셋 체크포인트로 구간을 자른다 —
#   같은 문구가 다른 시나리오에서도 찍히기 때문이다.
#
#   ⚠️ 알려진 환경 함정 2건 (s2s.ps1 에서 실측으로 확인 — 승계):
#   ① 리스너 없는 포트로의 connect 실패가 즉시(RST)가 아니라 매번 약 2~2.6초씩
#      걸린다(원인 불명 — 방화벽/보안 소프트웨어의 SYN 드롭 추정). 이 스크립트는
#      session.exe 를 먼저 띄워 대상 포트가 항상 듣고 있게 하는 것으로 회피한다.
#   ② TcpListener.Stop()/프로세스 종료 직후에도 포트가 잠깐 살아 있을 수 있다 —
#      페이즈 사이(같은 9100/9200 재사용)에 프로세스 종료 확인 + 대기를 둔다.
#
#   사용:
#     .\scripts\session.ps1                 # Release 로 전 항목
#     .\scripts\session.ps1 -Config ASan    # ASan 구성 1회(세션 수명 검증 의무)

param(
    [string]$Config = 'Release'
)

$ErrorActionPreference = 'Stop'

# K7~K9(가짜 마을의 Kick 수신 대기) 전용으로 harness_common.ps1 의 Wait-S2sFrame
# 을 쓴다 — 이 파일의 로컬 Read-S2sFrameFiltered 는 Heartbeat 자동 ack 가 없다
# (사본 비대칭). 이 파일이 아래서 정의하는 동명 함수(New-S2sHeader·Read-S2sFrame·
# Send-S2sFrame 등 15개)가 dot-source 뒤에 다시 정의되므로 그 재정의가 이긴다
# — 기존 78항목은 전부 이 파일 자신의 구현을 그대로 쓰고, Wait-S2sFrame 처럼
# 이 파일에 없는 이름만 harness_common.ps1 것이 새로 들어온다.
. (Join-Path $PSScriptRoot 'harness_common.ps1')

# ── 상수 — proto 와 값으로 맞춘다(리터럴 사유는 s2s.ps1 과 같다 — find_copies 로 걸린다) ──
$MsgRegister      = 0x8001
$MsgUnregister    = 0x8002
$MsgHeartbeat     = 0x8003
$MsgPlayerEnter   = 0x8004
$MsgPlayerLeave   = 0x8005
$MsgFullSync      = 0x8006
$MsgDrainComplete = 0x8007
$MsgReserve       = 0x8101
$MsgKick          = 0x8102
$MsgSetMode       = 0x8103
$MsgRegisterAck   = 0x8201
$MsgUnregisterAck = 0x8202
$MsgHeartbeatAck  = 0x8203
$MsgReserveAck    = 0x8301
$MsgKickAck       = 0x8302
$MsgSetModeAck    = 0x8303
$MsgUnsupported   = 0x8FFF

$CliSessionLoginReq = 12      # proto::MsgId::kSessionLoginReq
$CliSessionLoginAck = 113     # proto::MsgId::kSessionLoginAck

# Phase 6(§7) 이 실물 마을에 직접 붙는 클라 역할도 해야 해서 늘었다 — 세션
# 서버 전용 위 둘과 이름을 분리해 어느 쪽 프로토콜인지 리터럴만 보고 알게 한다.
$VilJoinZoneReq = 2           # proto::MsgId::kJoinZoneReq
$VilJoinZoneAck = 102         # proto::MsgId::kJoinZoneAck
$VilEnterReq    = 13          # proto::MsgId::kEnterReq (u64)
$VilEnterAck    = 114         # proto::MsgId::kEnterAck
$VilInventoryReq = 4          # proto::MsgId::kInventoryReq (body 없음 — 세션의 player_id 를 쓴다)
$VilInventoryAck = 104        # proto::MsgId::kInventoryAck
$VilPingReq      = 11         # proto::MsgId::kPingReq (body 없음) — 게이트 앞 스위치라 미인증 소켓도 통과한다.
$VilPongAck      = 111        # proto::MsgId::kPongAck                    P6-2-F1~F3 의 생존 확인이 이걸로 바뀐 이유는 아래 참조.

$ResultOk         = 0         # proto::ResultCode::kOk
$ResultInvalidArg = 3         # proto::ResultCode::kInvalidArg

$ResultFull = 2               # proto::s2s::kResultFull
$VerOk  = 0x0100              # proto::s2s::ver() — major 1 minor 0
$VerBad = 0x0200              # major 2 — #8 불일치용

$SessionClientPort = 9200
$SessionS2sPort    = 9100
$VillageClientPort = 9000     # config/server.ini [server] port 기본값 — New-VillageHome 의
                               # ClientPort 인자로 마을마다 다르게 패치할 수도 있다

# ── 결과 수집 ────────────────────────────────────────────────────────────────
$script:MatrixResults = New-Object System.Collections.Generic.List[object]

# 항목을 콘솔에 찍는 자리를 하나로 모은다 — Add-Result 와 Add-Skip 이 각자
#   짜면 [MATRIX ...] 형식이 갈라져서 grep 한 줄로 훑을 수 없게 된다.
function Write-MatrixLine([string]$Id, [string]$Status, [string]$Detail) {
    $color = if ($Status -eq 'PASS') { 'Green' } elseif ($Status -eq 'SKIP') { 'Yellow' } else { 'Red' }
    Write-Host ("[MATRIX {0}] {1} — {2}" -f $Id, $Status, $Detail) -ForegroundColor $color
}

function Add-Result([string]$Id, [bool]$Pass, [string]$Detail) {
    $status = if ($Pass) { 'PASS' } else { 'FAIL' }
    Write-MatrixLine $Id $status $Detail
    $script:MatrixResults.Add([pscustomobject]@{ Id = $Id; Pass = $Pass; Detail = $Detail; Status = $status })
}

# 폐지로 재현 불가가 된 항목용 — 실패로도 통과로도 세지 않는다. 조용히
#   지우면 「전체 통과 (총 N 항목)」의 N 이 줄어 무회귀 판정이 흐려지므로
#   SKIP 으로 따로 센다.
#   Add-Result 에 [string]$Status = $null 인자를 덧붙이는 안은 쓰지 않는다 —
#   [string] 로 형을 못박은 파라미터의 기본값 $null 은 빈 문자열로 강제
#   변환되어 $null 판정이 영원히 거짓이 된다(§7-b 가 모으는 PS 5.1 함정과
#   같은 계열). 별도 함수로 두면 이 문제 자체가 안 생긴다 — 두 인자 다
#   필수고 기본값이 없다.
function Add-Skip([string]$Id, [string]$Detail) {
    Write-MatrixLine $Id 'SKIP' $Detail
    $script:MatrixResults.Add([pscustomobject]@{ Id = $Id; Pass = $true; Detail = $Detail; Status = 'SKIP' })
}

# ── S2S 8B 헤더 — s2s.ps1 과 같은 코덱(빅엔디언) ─────────────────────────────
function New-S2sHeader([int]$BodySize, [int]$MsgId, [uint32]$Seq) {
    $h = [byte[]]::new(8)
    $h[0] = [byte](($BodySize -shr 8) -band 0xFF)
    $h[1] = [byte]( $BodySize         -band 0xFF)
    $h[2] = [byte](($MsgId    -shr 8) -band 0xFF)
    $h[3] = [byte]( $MsgId            -band 0xFF)
    $h[4] = [byte](($Seq -shr 24) -band 0xFF)
    $h[5] = [byte](($Seq -shr 16) -band 0xFF)
    $h[6] = [byte](($Seq -shr 8)  -band 0xFF)
    $h[7] = [byte]( $Seq          -band 0xFF)
    return $h
}

function ConvertTo-S2sHeader([byte[]]$Bytes) {
    $bodySize = ([int]$Bytes[0] -shl 8) -bor [int]$Bytes[1]
    $msgId    = ([int]$Bytes[2] -shl 8) -bor [int]$Bytes[3]
    $seq      = ([uint32]$Bytes[4] -shl 24) -bor ([uint32]$Bytes[5] -shl 16) `
                -bor ([uint32]$Bytes[6] -shl 8) -bor [uint32]$Bytes[7]
    [pscustomobject]@{ BodySize = $bodySize; MsgId = $msgId; Seq = $seq }
}

# ── 소켓 I/O — s2s.ps1 의 함정 주석 승계: 빈/1요소 배열은 콤마로 감싸 언롤링을 막는다 ──
function Read-ExactBytes([System.Net.Sockets.NetworkStream]$Stream, [int]$Count, [int]$TimeoutMs) {
    if ($Count -eq 0) { return ,[byte[]]::new(0) }
    $buf = [byte[]]::new($Count)
    $got = 0
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($got -lt $Count) {
        if ($Stream.DataAvailable) {
            $n = $Stream.Read($buf, $got, $Count - $got)
            if ($n -le 0) { return $null }      # 상대가 닫았다
            $got += $n
        } elseif ($sw.ElapsedMilliseconds -gt $TimeoutMs) {
            return $null
        } else {
            Start-Sleep -Milliseconds 10
        }
    }
    return ,$buf
}

function Read-S2sFrame($Stream, [int]$TimeoutMs) {
    $header = Read-ExactBytes $Stream 8 $TimeoutMs
    if ($null -eq $header) { return $null }
    $h = ConvertTo-S2sHeader $header
    $body = Read-ExactBytes $Stream $h.BodySize $TimeoutMs
    if ($null -eq $body) { return $null }
    return [pscustomobject]@{ MsgId = $h.MsgId; Seq = $h.Seq; Body = $body }
}

# 같은 S2S 연결에는 요청의 응답 말고도 마을이 스스로 발신하는 알림(등록 직후
#   FullSync — 집합이 비어 있어도 chunk_total=1 로 한 번은 나간다 · 주기적
#   Heartbeat)이 아무 때나 낄 수 있다. 원하는 msg_id 가 나올 때까지 그 사이에
#   낀 프레임을 버리며 읽는다 — 총 대기시간 상한 안에서 못 찾으면 null 이다
#   (조용히 통과시키지 않는다). 버린 프레임의 msg_id 는 SkippedIds 에 쌓아
#   호출부가 보고에 남길 수 있게 한다. 응답을 안 읽고 fire-and-forget 으로
#   보내는 기존 호출부는 이 문제를 겪지 않으므로 그대로 두고, 응답을 실제로
#   읽는 자리에서만 이 함수를 쓴다.
function Read-S2sFrameFiltered($Stream, [int]$WantMsgId, [int]$TotalTimeoutMs,
        [System.Collections.Generic.List[int]]$SkippedIds) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($true) {
        $remain = $TotalTimeoutMs - [int]$sw.ElapsedMilliseconds
        if ($remain -le 0) { return $null }
        $frame = Read-S2sFrame $Stream $remain
        if ($null -eq $frame) { return $null }
        if ($frame.MsgId -eq $WantMsgId) { return $frame }
        $SkippedIds.Add($frame.MsgId)
    }
}

function Send-S2sFrame($Stream, [int]$MsgId, [uint32]$Seq, [byte[]]$Body) {
    $header = New-S2sHeader $Body.Length $MsgId $Seq
    $frame = $header + $Body
    $Stream.Write($frame, 0, $frame.Length)
    $Stream.Flush()
}

# ── 여기부터 s2s.ps1 과 반대 방향(가짜 세션이 실물 village 를 받는다) 전용 —
#    이 스크립트는 원래 클라만 흉내 내지만, MUT5 재시도(§9)가 S2S 스레드
#    자체를 필요로 해서 s2s.ps1 의 리스너·인코더를 값으로 맞춰 들여왔다.
function New-S2sListener([int]$Port) {
    for ($attempt = 1; $attempt -le 20; $attempt++) {
        try {
            $l = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $Port)
            $l.Server.SetSocketOption(
                [System.Net.Sockets.SocketOptionLevel]::Socket,
                [System.Net.Sockets.SocketOptionName]::ReuseAddress, $true)
            $l.Start()
            return $l
        } catch {
            if ($attempt -eq 20) { throw }
            Start-Sleep -Milliseconds 100
        }
    }
}

function Wait-Accept($Listener, [int]$TimeoutMs, [string]$Label) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while (-not $Listener.Pending()) {
        if ($sw.ElapsedMilliseconds -gt $TimeoutMs) {
            Write-Host "  ($Label — ${TimeoutMs}ms 안에 연결이 안 왔다)" -ForegroundColor Yellow
            return $null
        }
        Start-Sleep -Milliseconds 20
    }
    return $Listener.AcceptTcpClient()
}

function New-RegisterAckBody([uint32]$ServerId, [byte]$Result) {
    $b = [byte[]]::new(5)
    $b[0] = [byte](($ServerId -shr 24) -band 0xFF); $b[1] = [byte](($ServerId -shr 16) -band 0xFF)
    $b[2] = [byte](($ServerId -shr 8)  -band 0xFF); $b[3] = [byte]( $ServerId          -band 0xFF)
    $b[4] = $Result
    return ,$b
}

# Reserve body: [ player_id : u64 ][ expire_ms : u32 ] (12B)
function New-ReserveBody([uint64]$PlayerId, [uint32]$ExpireMs) {
    $b = [byte[]]::new(12)
    for ($i = 0; $i -lt 8; $i++) { $b[$i] = [byte](($PlayerId -shr (8 * (7 - $i))) -band 0xFF) }
    $b[8]  = [byte](($ExpireMs -shr 24) -band 0xFF); $b[9]  = [byte](($ExpireMs -shr 16) -band 0xFF)
    $b[10] = [byte](($ExpireMs -shr 8)  -band 0xFF); $b[11] = [byte]( $ExpireMs          -band 0xFF)
    return ,$b
}

# ── 클라 4B 헤더 [body_size:u16][msg_id:u16] — packet.h 와 값으로 맞춘다 ─────
function Send-ClientFrame($Stream, [int]$MsgId, [byte[]]$Body) {
    $f = [byte[]]::new(4 + $Body.Length)
    $f[0] = [byte](($Body.Length -shr 8) -band 0xFF)
    $f[1] = [byte]( $Body.Length         -band 0xFF)
    $f[2] = [byte](($MsgId -shr 8) -band 0xFF)
    $f[3] = [byte]( $MsgId         -band 0xFF)
    if ($Body.Length -gt 0) { [Array]::Copy($Body, 0, $f, 4, $Body.Length) }
    $Stream.Write($f, 0, $f.Length)
    $Stream.Flush()
}

function Read-ClientFrame($Stream, [int]$TimeoutMs) {
    $header = Read-ExactBytes $Stream 4 $TimeoutMs
    if ($null -eq $header) { return $null }
    $bodySize = ([int]$header[0] -shl 8) -bor [int]$header[1]
    $msgId    = ([int]$header[2] -shl 8) -bor [int]$header[3]
    $body = Read-ExactBytes $Stream $bodySize $TimeoutMs
    if ($null -eq $body) { return $null }
    return [pscustomobject]@{ MsgId = $msgId; Body = $body }
}

# PS 5.1 의 비트 연산은 [long] 이 안전하다 — 하네스 player_id 는 작은 값만 쓴다.
function New-LoginBody([long]$PlayerId) {
    $b = [byte[]]::new(8)
    for ($i = 0; $i -lt 8; $i++) {
        $b[$i] = [byte](($PlayerId -shr (8 * (7 - $i))) -band 0xFF)
    }
    return ,$b
}

# LoginAck body: [result:u8][port:u16][host:str16(길이 선행+UTF-8)]
function ConvertFrom-LoginAckBody([byte[]]$Body) {
    $result  = [int]$Body[0]
    $port    = ([int]$Body[1] -shl 8) -bor [int]$Body[2]
    $hostLen = ([int]$Body[3] -shl 8) -bor [int]$Body[4]
    $hostStr = if ($hostLen -gt 0) { [System.Text.Encoding]::UTF8.GetString($Body, 5, $hostLen) } else { '' }
    [pscustomobject]@{ Result = $result; Port = $port; HostName = $hostStr }
}

# Register body: [ver:u16][port:u16][capacity:u32][current:u32][host:str16]
function New-RegisterBody([int]$Ver, [int]$Port, [uint32]$Capacity, [uint32]$Current, [string]$HostName) {
    $hb = [System.Text.Encoding]::UTF8.GetBytes($HostName)
    $b = [byte[]]::new(14 + $hb.Length)
    $b[0] = [byte](($Ver -shr 8) -band 0xFF);  $b[1] = [byte]($Ver -band 0xFF)
    $b[2] = [byte](($Port -shr 8) -band 0xFF); $b[3] = [byte]($Port -band 0xFF)
    $b[4] = [byte](($Capacity -shr 24) -band 0xFF); $b[5] = [byte](($Capacity -shr 16) -band 0xFF)
    $b[6] = [byte](($Capacity -shr 8) -band 0xFF);  $b[7] = [byte]($Capacity -band 0xFF)
    $b[8] = [byte](($Current -shr 24) -band 0xFF);  $b[9] = [byte](($Current -shr 16) -band 0xFF)
    $b[10] = [byte](($Current -shr 8) -band 0xFF);  $b[11] = [byte]($Current -band 0xFF)
    $b[12] = [byte](($hb.Length -shr 8) -band 0xFF); $b[13] = [byte]($hb.Length -band 0xFF)
    if ($hb.Length -gt 0) { [Array]::Copy($hb, 0, $b, 14, $hb.Length) }
    return ,$b
}

# Kick body(수신 — 세션 서버가 보낸 요청): [ player_id : u64 ][ reason : u8 ] (9B)
function ConvertFrom-KickBody([byte[]]$Body) {
    $playerId = [uint64]0
    for ($i = 0; $i -lt 8; $i++) { $playerId = ($playerId -shl 8) -bor [uint64]$Body[$i] }
    [pscustomobject]@{ PlayerId = $playerId; Reason = [int]$Body[8] }
}

function ConvertFrom-RegisterAckBody([byte[]]$Body) {
    $serverId = ([uint32]$Body[0] -shl 24) -bor ([uint32]$Body[1] -shl 16) `
                -bor ([uint32]$Body[2] -shl 8) -bor [uint32]$Body[3]
    [pscustomobject]@{ ServerId = $serverId; Result = [int]$Body[4] }
}

# ── 실물 마을에 직접 붙는 클라 코덱(Phase 6) — packet.h 와 값으로 맞춘다 ──────
# kEnterReq body: [ player_id : u64 ] — New-LoginBody 와 폭이 같아 그대로 쓴다.
# kEnterAck body: [ result : u8 ][ player_id : u64 ][ session_id : u32 ] (13B)
function ConvertFrom-EnterAckBody([byte[]]$Body) {
    $result = [int]$Body[0]
    $playerId = [uint64]0
    for ($i = 0; $i -lt 8; $i++) { $playerId = ($playerId -shl 8) -bor [uint64]$Body[1 + $i] }
    $sessionId = ([uint32]$Body[9] -shl 24) -bor ([uint32]$Body[10] -shl 16) `
                -bor ([uint32]$Body[11] -shl 8) -bor [uint32]$Body[12]
    [pscustomobject]@{ Result = $result; PlayerId = $playerId; SessionId = $sessionId }
}

# kInventoryAck body: [ result : u8 ][ count : u16 ]( [ item_id : u32 ][ item_count : i32 ] × count )
function ConvertFrom-InventoryAckBody([byte[]]$Body) {
    $result = [int]$Body[0]
    $count = ([int]$Body[1] -shl 8) -bor [int]$Body[2]
    $items = New-Object System.Collections.Generic.List[object]
    $off = 3
    for ($i = 0; $i -lt $count; $i++) {
        $itemId = ([uint32]$Body[$off] -shl 24) -bor ([uint32]$Body[$off+1] -shl 16) `
                -bor ([uint32]$Body[$off+2] -shl 8) -bor [uint32]$Body[$off+3]
        $qty = ([int]$Body[$off+4] -shl 24) -bor ([int]$Body[$off+5] -shl 16) `
                -bor ([int]$Body[$off+6] -shl 8) -bor [int]$Body[$off+7]
        $items.Add([pscustomobject]@{ ItemId = $itemId; Count = $qty })
        $off += 8
    }
    [pscustomobject]@{ Result = $result; Count = $count; Items = $items }
}

# kJoinZoneReq body: [ zone_id : u32 ] · kJoinZoneAck body: [ zone_id : u32 ][ member_count : u32 ]
function New-JoinZoneBody([uint32]$ZoneId) {
    $b = [byte[]]::new(4)
    $b[0] = [byte](($ZoneId -shr 24) -band 0xFF); $b[1] = [byte](($ZoneId -shr 16) -band 0xFF)
    $b[2] = [byte](($ZoneId -shr 8) -band 0xFF);  $b[3] = [byte]($ZoneId -band 0xFF)
    return ,$b
}

function ConvertFrom-JoinZoneAckBody([byte[]]$Body) {
    $zoneId = ([uint32]$Body[0] -shl 24) -bor ([uint32]$Body[1] -shl 16) `
                -bor ([uint32]$Body[2] -shl 8) -bor [uint32]$Body[3]
    $memberCount = ([uint32]$Body[4] -shl 24) -bor ([uint32]$Body[5] -shl 16) `
                -bor ([uint32]$Body[6] -shl 8) -bor [uint32]$Body[7]
    [pscustomobject]@{ ZoneId = $zoneId; MemberCount = $memberCount }
}

# 게이트 도입 뒤로 "연결이 살아 있다"를 kJoinZoneReq 로 확인하면 안 된다 —
#   Enter 가 실패한 세션은 player_id 가 여전히 0 이라, 그 확인 자체가 게이트에
#   걸려 끊긴다("살아 있는지 보려다 죽인다"). kPingReq 는 on_frame 의 1단계
#   스위치(배정을 안 보는 메시지)에 있어 게이트보다 앞에서 처리되므로
#   미인증 소켓에도 그대로 통한다(F-3·packet.h 참조). P6-2-F1~F3 은 전부
#   이걸로 생존을 확인한다.
function Test-VillageAlivePing($Stream, [int]$TimeoutMs) {
    Send-ClientFrame $Stream $VilPingReq ([byte[]]::new(0))
    $pong = Read-ClientFrame $Stream $TimeoutMs
    return ($null -ne $pong) -and ($pong.MsgId -eq $VilPongAck)
}

function Connect-Tcp([int]$Port) {
    $c = [System.Net.Sockets.TcpClient]::new('127.0.0.1', $Port)
    return $c
}

# ── ini 사본 패치 — s2s.ps1 과 같은 두 단계 단언(치환 1줄 + 재읽기 대조, P4) ──
function Set-IniKeyInSection([string]$Path, [string]$Section, [string]$Key, [string]$Value) {
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $text = [System.Text.Encoding]::UTF8.GetString($bytes).TrimStart([char]0xFEFF)
    $lines = $text -split "`n"
    $inSection = $false
    $replaced = 0
    $keyPattern = '^\s*' + [regex]::Escape($Key) + '\s*='
    for ($i = 0; $i -lt $lines.Length; $i++) {
        $line = $lines[$i]
        if ($line -match '^\s*\[(.+?)\]\s*$') {
            $inSection = ($Matches[1].Trim() -eq $Section)
            continue
        }
        if ($inSection -and ($line -match $keyPattern)) {
            $pad = ' ' * [Math]::Max(1, 21 - $Key.Length)
            $lines[$i] = "$Key$pad= $Value"
            $replaced++
        }
    }
    if ($replaced -ne 1) {
        throw "설정 치환 실패 — [$Section] $Key 는 정확히 1줄이어야 하는데 ${replaced}줄 매치됐다"
    }
    $newText = [string]::Join("`n", $lines)
    $utf8Bom = New-Object System.Text.UTF8Encoding($true)
    [System.IO.File]::WriteAllText($Path, $newText, $utf8Bom)
}

function Assert-IniValue([string]$Path, [string]$Section, [string]$Key, [string]$Expected) {
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $text = [System.Text.Encoding]::UTF8.GetString($bytes).TrimStart([char]0xFEFF)
    $lines = $text -split "`n"
    $inSection = $false
    $found = $null
    $keyPattern = '^\s*' + [regex]::Escape($Key) + '\s*=\s*(.*?)\s*$'
    foreach ($line in $lines) {
        if ($line -match '^\s*\[(.+?)\]\s*$') {
            $inSection = ($Matches[1].Trim() -eq $Section)
            continue
        }
        if ($inSection -and ($line -match $keyPattern)) {
            $found = $Matches[1]
            break
        }
    }
    if ($found -ne $Expected) {
        throw "설정 재검증 실패 — [$Section] $Key 기대값 '$Expected' 실제 '$found'"
    }
}

# ── 프로세스 스폰 — s2s.ps1 골격(stdout/stderr 비동기 드레인 · 스크래치 cwd) ──
# 파라미터명이 HomeDir 인 이유 — $Home 은 PowerShell 읽기 전용 자동 변수라 못 쓴다.
function Start-ServerProcess([string]$ExePath, [string]$HomeDir, [string]$Arguments) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $ExePath
    $psi.Arguments = $Arguments
    $psi.WorkingDirectory = $HomeDir
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $p = [System.Diagnostics.Process]::Start($psi)
    # 파이프 버퍼(4KB)가 차면 자식이 콘솔 출력에서 블록된다 — 비동기로 비운다.
    $p.BeginOutputReadLine()
    $p.BeginErrorReadLine()
    Register-ObjectEvent -InputObject $p -EventName OutputDataReceived -Action {
        if ($EventArgs.Data) { Add-Content -LiteralPath $Event.MessageData -Value $EventArgs.Data }
    } -MessageData (Join-Path $HomeDir 'proc.stdout.log') | Out-Null
    Register-ObjectEvent -InputObject $p -EventName ErrorDataReceived -Action {
        if ($EventArgs.Data) { Add-Content -LiteralPath $Event.MessageData -Value $EventArgs.Data }
    } -MessageData (Join-Path $HomeDir 'proc.stderr.log') | Out-Null
    return $p
}

# ── 드레인 콘솔 전용 스폰 — Win32 CreateProcess 직결(harness_common.ps1 사본 —
#   find_copies.ps1 가 동기화를 지킨다). 위 Start-ServerProcess(.NET
#   Process.Start 3중 리다이렉트)로 띄우면 getline 이 스폰 직후 EOF 를 받아
#   stdin 콘솔이 무의미해진다(실측 — harness_common.ps1 머리
#   주석 참조). 기존 30여 곳의 --seconds 스폰은 이 문제를 안 겪는다(그
#   경로는 애초에 stdin 을 안 읽는다) — 그래서 저 함수는 그대로 두고, 이
#   함수는 stdin 명령이 실제로 필요한 드레인 시나리오 전용으로 새로 연다. ──
if (-not ([System.Management.Automation.PSTypeName]'MiniGameHarness.Native').Type) {
    Add-Type -Namespace MiniGameHarness -Name Native -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)]
public struct SECURITY_ATTRIBUTES {
    public int nLength;
    public IntPtr lpSecurityDescriptor;
    public bool bInheritHandle;
}
[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
public struct STARTUPINFO {
    public int cb;
    public string lpReserved;
    public string lpDesktop;
    public string lpTitle;
    public int dwX; public int dwY; public int dwXSize; public int dwYSize;
    public int dwXCountChars; public int dwYCountChars;
    public int dwFillAttribute; public int dwFlags;
    public short wShowWindow; public short cbReserved2;
    public IntPtr lpReserved2;
    public IntPtr hStdInput; public IntPtr hStdOutput; public IntPtr hStdError;
}
[StructLayout(LayoutKind.Sequential)]
public struct PROCESS_INFORMATION {
    public IntPtr hProcess; public IntPtr hThread; public int dwProcessId; public int dwThreadId;
}
[DllImport("kernel32.dll", EntryPoint = "CreateProcessW", SetLastError = true, CharSet = CharSet.Unicode)]
public static extern bool CreateProcess(
    string lpApplicationName, System.Text.StringBuilder lpCommandLine,
    IntPtr lpProcessAttributes, IntPtr lpThreadAttributes,
    bool bInheritHandles, uint dwCreationFlags, IntPtr lpEnvironment,
    string lpCurrentDirectory, ref STARTUPINFO lpStartupInfo,
    out PROCESS_INFORMATION lpProcessInformation);
[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool CreatePipe(out IntPtr hReadPipe, out IntPtr hWritePipe, ref SECURITY_ATTRIBUTES lpPipeAttributes, uint nSize);
[DllImport("kernel32.dll", EntryPoint = "CreateFileW", SetLastError = true, CharSet = CharSet.Unicode)]
public static extern IntPtr CreateFile(string lpFileName, uint dwDesiredAccess, uint dwShareMode,
    ref SECURITY_ATTRIBUTES lpSecurityAttributes, uint dwCreationDisposition, uint dwFlagsAndAttributes, IntPtr hTemplateFile);
[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool SetHandleInformation(IntPtr hObject, uint dwMask, uint dwFlags);
[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool CloseHandle(IntPtr hObject);
'@
}

function Start-ServerProcessWithStdin([string]$ExePath, [string]$HomeDir) {
    $sa = New-Object MiniGameHarness.Native+SECURITY_ATTRIBUTES
    $sa.nLength = [System.Runtime.InteropServices.Marshal]::SizeOf([type][MiniGameHarness.Native+SECURITY_ATTRIBUTES])
    $sa.bInheritHandle = $true
    $sa.lpSecurityDescriptor = [IntPtr]::Zero

    $stdinRead = [IntPtr]::Zero
    $stdinWrite = [IntPtr]::Zero
    if (-not [MiniGameHarness.Native]::CreatePipe([ref]$stdinRead, [ref]$stdinWrite, [ref]$sa, 0)) {
        throw "CreatePipe(stdin) 실패: $([System.Runtime.InteropServices.Marshal]::GetLastWin32Error())"
    }
    [void][MiniGameHarness.Native]::SetHandleInformation($stdinWrite, 1, 0)

    $GENERIC_WRITE = 0x40000000
    $FILE_SHARE_RW = 0x3
    $CREATE_ALWAYS = 2
    $FILE_ATTRIBUTE_NORMAL = 0x80
    $stdoutHandle = [MiniGameHarness.Native]::CreateFile((Join-Path $HomeDir 'proc.stdout.log'),
        $GENERIC_WRITE, $FILE_SHARE_RW, [ref]$sa, $CREATE_ALWAYS, $FILE_ATTRIBUTE_NORMAL, [IntPtr]::Zero)
    $stderrHandle = [MiniGameHarness.Native]::CreateFile((Join-Path $HomeDir 'proc.stderr.log'),
        $GENERIC_WRITE, $FILE_SHARE_RW, [ref]$sa, $CREATE_ALWAYS, $FILE_ATTRIBUTE_NORMAL, [IntPtr]::Zero)
    if ($stdoutHandle -eq [IntPtr]-1 -or $stderrHandle -eq [IntPtr]-1) {
        $err = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
        [void][MiniGameHarness.Native]::CloseHandle($stdinRead)
        [void][MiniGameHarness.Native]::CloseHandle($stdinWrite)
        if ($stdoutHandle -ne [IntPtr]-1) { [void][MiniGameHarness.Native]::CloseHandle($stdoutHandle) }
        if ($stderrHandle -ne [IntPtr]-1) { [void][MiniGameHarness.Native]::CloseHandle($stderrHandle) }
        throw "CreateFile(proc.stdout/stderr.log) 실패: $err"
    }

    $si = New-Object MiniGameHarness.Native+STARTUPINFO
    $si.cb = [System.Runtime.InteropServices.Marshal]::SizeOf([type][MiniGameHarness.Native+STARTUPINFO])
    $si.dwFlags = 0x100   # STARTF_USESTDHANDLES
    $si.hStdInput = $stdinRead
    $si.hStdOutput = $stdoutHandle
    $si.hStdError = $stderrHandle

    $pi = New-Object MiniGameHarness.Native+PROCESS_INFORMATION
    $cmdLine = New-Object System.Text.StringBuilder("`"$ExePath`"")
    $ok = [MiniGameHarness.Native]::CreateProcess($ExePath, $cmdLine, [IntPtr]::Zero, [IntPtr]::Zero,
        $true, 0, [IntPtr]::Zero, $HomeDir, [ref]$si, [ref]$pi)
    if (-not $ok) {
        $err = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
        [void][MiniGameHarness.Native]::CloseHandle($stdinRead)
        [void][MiniGameHarness.Native]::CloseHandle($stdinWrite)
        [void][MiniGameHarness.Native]::CloseHandle($stdoutHandle)
        [void][MiniGameHarness.Native]::CloseHandle($stderrHandle)
        throw "CreateProcess 실패($ExePath): $err"
    }
    [void][MiniGameHarness.Native]::CloseHandle($stdinRead)
    [void][MiniGameHarness.Native]::CloseHandle($stdoutHandle)
    [void][MiniGameHarness.Native]::CloseHandle($stderrHandle)
    [void][MiniGameHarness.Native]::CloseHandle($pi.hThread)
    # hProcess 는 여기서 안 닫는다 — GetProcessById 가 PID 로 다시 찾는데, 이 핸들을
    #   쥐고 있는 동안은 OS 가 그 PID 를 다른 프로세스에 재할당하지 못한다(핸들이
    #   프로세스 객체를 참조로 붙들기 때문). 먼저 닫으면 그 사이(TOCTOU 창)에 같은
    #   PID 가 재사용된 경우 GetProcessById 가 엉뚱한 프로세스를 가리킬 수 있다.
    try {
        $proc = [System.Diagnostics.Process]::GetProcessById($pi.dwProcessId)
    } catch {
        [void][MiniGameHarness.Native]::CloseHandle($stdinWrite)
        [void][MiniGameHarness.Native]::CloseHandle($pi.hProcess)
        throw "자식이 기동 직후 종료됐다(pid=$($pi.dwProcessId)) — $HomeDir\proc.stderr.log 확인. 원 예외: $($_.Exception.Message)"
    }
    [void][MiniGameHarness.Native]::CloseHandle($pi.hProcess)   # GetProcessById 가 제 핸들을 새로 연다 — 이제 닫아도 안전하다
    $safeStdin = New-Object Microsoft.Win32.SafeHandles.SafeFileHandle($stdinWrite, $true)
    $stdinStream = New-Object System.IO.FileStream($safeStdin, [System.IO.FileAccess]::Write)
    $stdinWriter = New-Object System.IO.StreamWriter($stdinStream)
    $proc | Add-Member -MemberType NoteProperty -Name HarnessStdin -Value $stdinWriter -Force
    return $proc
}

# ── 로그 대기 — 바이트 오프셋 스코핑. UTF-8 로그라 바이트로 자르고 바이트로 잰다 ──
function Get-LogLength([string]$Path) {
    if (Test-Path -LiteralPath $Path) { return (Get-Item -LiteralPath $Path).Length }
    return 0
}

# 서버가 쓰는 중인 로그를 읽는다 — File.ReadAllBytes 는 FileShare.Read 로 열어
# 쓰기 중인 파일과 공유 충돌한다(실측). ReadWrite 공유로 열어야 산 로그를 읽는다.
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

function Wait-LogMatch([string]$Path, [string]$Pattern, [int]$TimeoutMs, [long]$FromOffset = 0) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $TimeoutMs) {
        if (Test-Path -LiteralPath $Path) {
            $bytes = Read-FileBytesShared $Path
            if ($bytes.Length -gt $FromOffset) {
                $text = [System.Text.Encoding]::UTF8.GetString($bytes, $FromOffset, $bytes.Length - $FromOffset)
                $m = [regex]::Match($text, $Pattern)
                if ($m.Success) { return $m }
            }
        }
        Start-Sleep -Milliseconds 100
    }
    return $null
}

function Read-LogText([string]$Path, [long]$FromOffset = 0) {
    if (-not (Test-Path -LiteralPath $Path)) { return '' }
    $bytes = Read-FileBytesShared $Path
    if ($bytes.Length -le $FromOffset) { return '' }
    return [System.Text.Encoding]::UTF8.GetString($bytes, $FromOffset, $bytes.Length - $FromOffset)
}

# ═══════════════════════════════════════════════════════════════════════════
#  준비
# ═══════════════════════════════════════════════════════════════════════════

$root = Split-Path -Parent $PSScriptRoot
$sessionExe = Join-Path $root "build\x64\$Config\session.exe"
$villageExe = Join-Path $root "build\x64\$Config\village.exe"
if (-not (Test-Path $sessionExe)) { throw "$sessionExe 가 없다 — 먼저 .\scripts\build.ps1 로 빌드하라" }
if (-not (Test-Path $villageExe)) { throw "$villageExe 가 없다 — 먼저 .\scripts\build.ps1 로 빌드하라" }

Get-Process village, session -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300

if ($Config -eq 'ASan') {
    # run-asan.ps1 의 DLL PATH 선행 처리를 복제한다 — ASan DLL 을 못 찾으면 기동
    # 자체가 죽는다. ⚠️ run-asan.ps1·s2s.ps1 과 동기화돼야 한다 — 공용 함수로 뽑을
    # 수 없어 리터럴이 같은 동안은 find_copies.ps1 이 잡아 준다.
    $dll = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\clang_rt.asan_dynamic-x86_64.dll' -ErrorAction SilentlyContinue |
           Sort-Object -Property FullName -Descending | Select-Object -First 1
    if ($dll) {
        $env:PATH = "$($dll.DirectoryName);$env:PATH"
        Write-Host "asan  : $($dll.DirectoryName)"
    } else {
        Write-Host "asan  : DLL 을 못 찾음 — 그냥 실행해 봅니다" -ForegroundColor Yellow
    }
    $env:ASAN_OPTIONS = 'abort_on_error=0'
}

$scratchRoot = Join-Path $env:TEMP ("session_harness_" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scratchRoot -Force | Out-Null

# 세션 서버 홈 — config 사본 + 축소값 패치
function New-SessionHome([string]$Name, [string]$Capacity, [string]$RequestTimeout = '500',
        [string]$ExpireMs = '2000', [string]$SweepMs = '500', [string]$UnregisterGraceMs = '800') {
    $home2 = Join-Path $scratchRoot $Name
    $cfgDir = Join-Path $home2 'config'
    New-Item -ItemType Directory -Path $cfgDir -Force | Out-Null
    $dst = Join-Path $cfgDir 'session.ini'
    Copy-Item -LiteralPath (Join-Path $root 'config\session.ini') -Destination $dst
    # request_timeout_ms 축소 기본 500 — pending 항등식의 「타임아웃」 항을 같은
    # 페이즈 안에서 관측하기 위해서다. 종료 시 잔량은 shutdown 이 stop 갈래로
    # 회수하므로 「잔량 0」의 근거가 아니다 — stop·링크 다운 갈래는 별도 페이즈가
    # 큰 타임아웃(경주 제거)으로 본다.
    $patch = [ordered]@{ health_period_ms = '500'; health_fail_count = '3'; orphan_grace_ms = '2000'
        unregister_grace_ms = $UnregisterGraceMs }
    foreach ($k in $patch.Keys) { Set-IniKeyInSection $dst 'registry' $k $patch[$k] }
    Set-IniKeyInSection $dst 'registry' 'capacity' $Capacity
    $patch2 = [ordered]@{ expire_ms = $ExpireMs; sweep_ms = $SweepMs; request_timeout_ms = $RequestTimeout }
    foreach ($k in $patch2.Keys) { Set-IniKeyInSection $dst 'reserve' $k $patch2[$k] }
    foreach ($k in $patch.Keys) { Assert-IniValue $dst 'registry' $k $patch[$k] }
    Assert-IniValue $dst 'registry' 'capacity' $Capacity
    foreach ($k in $patch2.Keys) { Assert-IniValue $dst 'reserve' $k $patch2[$k] }
    return $home2
}

# 마을 홈 — [s2s] host 활성 + 축소값 패치 (s2s.ps1 의 패치 세트와 같은 값).
# ClientPort/MaxConnections/HeartbeatMs/ReserveExpireMs/SweepMs/FullsyncChunkMax 는
# Phase 6(§7)이 실물 마을 여러 대를 서로 다른 조건으로 띄워야 해서 늘렸다 —
# 동시에 두 마을을 띄우려면 [server] port 가 겹치면 안 되고(클라 접속 포트),
# capacity·예약·스윕·청크 상한은 항목별로 서로 배타적인 값을 요구한다(§7
# sub-phase 표). 기본값은 전부 config/server.ini 원본과 같다 — Phase 1~5 호출부
# (인자 없이 이름만 주는 지금 형태)는 그대로 원래 동작이다.
function New-VillageHome([string]$Name, [string]$ClientPort = '9000',
        [string]$MaxConnections = '4096', [string]$HeartbeatMs = '500',
        [string]$ReserveExpireMs = '10000', [string]$SweepMs = '30000',
        [string]$FullsyncChunkMax = '0') {
    $home2 = Join-Path $scratchRoot $Name
    $cfgDir = Join-Path $home2 'config'
    New-Item -ItemType Directory -Path $cfgDir -Force | Out-Null
    $dst = Join-Path $cfgDir 'server.ini'
    Copy-Item -LiteralPath (Join-Path $root 'config\server.ini') -Destination $dst
    $patchServer = [ordered]@{ port = $ClientPort; max_connections = $MaxConnections }
    foreach ($k in $patchServer.Keys) { Set-IniKeyInSection $dst 'server' $k $patchServer[$k] }
    # 커밋값([net] idle_timeout_sec)이 90 으로 올라간 뒤에도 이 마을들은 ping 을
    #   안 보내는 하네스용이다 — [net] 절이다([server] 절과 다르다, 절 함정 주의).
    $patchNet = [ordered]@{ idle_timeout_sec = '0' }
    foreach ($k in $patchNet.Keys) { Set-IniKeyInSection $dst 'net' $k $patchNet[$k] }
    $patch = [ordered]@{
        host               = '127.0.0.1'
        port               = '9100'
        backoff_initial_ms = '250'
        backoff_max_ms     = '1000'
        request_timeout_ms = '1500'
        heartbeat_ms       = $HeartbeatMs
    }
    foreach ($k in $patch.Keys) { Set-IniKeyInSection $dst 's2s' $k $patch[$k] }
    $patchEntry = [ordered]@{ reserve_expire_ms = $ReserveExpireMs; sweep_ms = $SweepMs
        fullsync_chunk_max = $FullsyncChunkMax }
    foreach ($k in $patchEntry.Keys) { Set-IniKeyInSection $dst 'village_entry' $k $patchEntry[$k] }
    foreach ($k in $patchServer.Keys) { Assert-IniValue $dst 'server' $k $patchServer[$k] }
    foreach ($k in $patchNet.Keys) { Assert-IniValue $dst 'net' $k $patchNet[$k] }
    foreach ($k in $patch.Keys) { Assert-IniValue $dst 's2s' $k $patch[$k] }
    foreach ($k in $patchEntry.Keys) { Assert-IniValue $dst 'village_entry' $k $patchEntry[$k] }
    return $home2
}

$sessProc = $null
$procs = New-Object System.Collections.Generic.List[object]

try {
    # ═══════════════════════════════════════════════════════════════════════
    #  Phase 1 — capacity=16. #1~#6, #8~#18
    # ═══════════════════════════════════════════════════════════════════════
    $sessHome = New-SessionHome 'sess1' '16'
    $sessLog = Join-Path $sessHome 'logs\server.log'
    $sessProc = Start-ServerProcess $sessionExe $sessHome '--seconds 90'
    $procs.Add($sessProc)
    $phase1Start = Get-Date
    if ($null -eq (Wait-LogMatch $sessLog 'session server up' 8000)) {
        throw 'session.exe 기동 로그가 없다 — 진행 불가'
    }

    # ── #6 — 레지스트리 빈 상태(마을을 아예 안 띄움)에서 Login → no_server ──
    $cli = Connect-Tcp $SessionClientPort
    $st = $cli.GetStream()
    Send-ClientFrame $st $CliSessionLoginReq (New-LoginBody 77)
    $ackFrame = Read-ClientFrame $st 3000
    $ack = if ($null -ne $ackFrame -and $ackFrame.MsgId -eq $CliSessionLoginAck) { ConvertFrom-LoginAckBody $ackFrame.Body } else { $null }
    Add-Result '#6' (($null -ne $ack) -and ($ack.Result -eq 1)) `
        ("빈 레지스트리 Login → result=" + $(if ($ack) { $ack.Result } else { '(무응답)' }) + " (기대 1=no_server)")
    $cli.Close()

    # ── village A 기동 → #1 등록 ───────────────────────────────────────────
    $vilAHome = New-VillageHome 'vilA'
    $vilALog = Join-Path $vilAHome 'logs\server.log'
    $ckRegA = Get-LogLength $sessLog
    $vilA = Start-ServerProcess $villageExe $vilAHome '--seconds 12'
    $procs.Add($vilA)
    $mA = Wait-LogMatch $sessLog 'register -> server_id=(\d+)' 8000 $ckRegA
    $serverIdA = if ($mA) { [int]$mA.Groups[1].Value } else { 0 }
    $mAv = Wait-LogMatch $vilALog '\[S2S\s+\] registered server_id=(\d+)' 8000
    Add-Result '#1' (($null -ne $mA) -and ($null -ne $mAv) -and ($serverIdA -eq [int]$mAv.Groups[1].Value)) `
        ("session 등록 server_id=$serverIdA / village registered server_id=" + $(if ($mAv) { $mAv.Groups[1].Value } else { '(없음)' }))

    # ── #3 — Login(u64) → LoginAck{ok, host/port=village 값} · #4 Reserve→ReserveAck ──
    Start-Sleep -Milliseconds 1200      # 하트비트(500ms)가 최소 2회 오갈 시간 (#2 재료)
    $ck4 = Get-LogLength $sessLog
    $cli = Connect-Tcp $SessionClientPort
    $st = $cli.GetStream()
    Send-ClientFrame $st $CliSessionLoginReq (New-LoginBody 1001)
    $ackFrame = Read-ClientFrame $st 3000
    $ack = if ($null -ne $ackFrame -and $ackFrame.MsgId -eq $CliSessionLoginAck) { ConvertFrom-LoginAckBody $ackFrame.Body } else { $null }
    Add-Result '#3' (($null -ne $ack) -and ($ack.Result -eq 0) -and ($ack.Port -eq 9000) -and ($ack.HostName -eq '127.0.0.1')) `
        ("LoginAck result=" + $(if ($ack) { "$($ack.Result) port=$($ack.Port) host=$($ack.HostName)" } else { '(무응답)' }) + " (기대 0/9000/127.0.0.1)")

    # #4 — 그 Login 이 발신한 Reserve 에 village 가 정상 ReserveAck 로 답한다(마을이
    # Reserve 를 처리한다 — 더 이상 Unsupported 로 안 돌아온다). 성공
    # 경로에는 로그가 없어(§8-4 성공은 카운터로만 남긴다) 실패 WARN 의 부재로
    # 확인한다 — 대기는 짧게 둔다(reserve.expire_ms=2000 보다 한참 짧아야 이
    # 대기가 그 예약을 만료시켜 #5 의 덮어쓰기 판정을 오염시키지 않는다).
    $m4 = Wait-LogMatch $sessLog 'reserve unsupported' 500 $ck4
    Add-Result '#4' ($null -eq $m4) `
        ("session WARN 'reserve unsupported' " + $(if ($m4) { '관측(회귀)' } else { '없음' }) + " — Reserve 가 정상 ReserveAck 로 처리됨(ack 카운터는 [SESS ] pending 요약에서 재확인)")

    # ── #5 — 같은 player_id 재로그인(덮어쓰기 — 최종 [SESS ] overwrite 로 판정) ──
    Send-ClientFrame $st $CliSessionLoginReq (New-LoginBody 1001)
    $ackFrame2 = Read-ClientFrame $st 3000
    $ack2 = if ($null -ne $ackFrame2 -and $ackFrame2.MsgId -eq $CliSessionLoginAck) { ConvertFrom-LoginAckBody $ackFrame2.Body } else { $null }
    $relogin0k = ($null -ne $ack2) -and ($ack2.Result -eq 0)
    $cli.Close()

    # ── village A 자연 종료 대기 → #2·#4(마을 회신) 판정 재료 확보 ───────────
    $vilA.WaitForExit(20000) | Out-Null
    $vilAText = Read-LogText $vilALog
    $mSum = [regex]::Match($vilAText,
        'connects=(\d+) reconnects=(\d+) sent=(\d+) recv=(\d+) timeouts=(\d+) failed=(\d+) unsupported_tx=(\d+)')
    $hb2 = $false; $unsup = 0
    if ($mSum.Success) {
        $sent = [int]$mSum.Groups[3].Value; $recv = [int]$mSum.Groups[4].Value
        $tmo = [int]$mSum.Groups[5].Value; $unsup = [int]$mSum.Groups[7].Value
        $hb2 = ($sent -ge 3) -and ($recv -ge 3) -and ($tmo -eq 0)
        Add-Result '#2' $hb2 "village A 요약 sent=$sent recv=$recv timeouts=$tmo (하트비트 왕복 지속 — seq 매칭 실패면 timeouts>0)"
        # unsupported_tx 는 이름과 달리 respond() 전량을 센다(s2s_connector.cpp 참조) —
        # vilA 의 phase1 수명 동안 respond() 는 이 Reserve 하나뿐이라 이 값은 지금
        # ReserveAck 발신 횟수와 같다. 이 카운터가 서 있다는 것 자체가
        # "Unsupported 가 아니라 ReserveAck 로 답했다"의 증거다(회신이 없었다면 0).
        if ($unsup -lt 1) { Add-Result '#4(마을 회신)' $false "unsupported_tx=$unsup (기대 >=1)" }
        else { Add-Result '#4(마을 회신)' $true "village A unsupported_tx=$unsup — Reserve 에 ReserveAck 회신 확인(카운터명은 이름과 달리 respond() 전량 집계)" }
    } else {
        Add-Result '#2' $false 'village A 종료 요약 줄을 못 찾았다'
        Add-Result '#4(마을 회신)' $false 'village A 종료 요약 줄을 못 찾았다'
    }

    # A 의 소켓이 닫혔으므로 session 쪽은 orphan → 유예(2000ms) 초과 → sweep 삭제.
    # 다음 시나리오(#10 부활)와 격리하기 위해 삭제까지 기다린다.
    $mDelA = Wait-LogMatch $sessLog 'sweep: .*항목 삭제=[1-9]' 6000 $ck4
    if ($null -eq $mDelA) { Write-Host '  (경고: A 항목 삭제 로그를 6s 안에 못 봤다 — 이후 판정에 영향 가능)' -ForegroundColor Yellow }

    # ── village B 기동 → #10 (kill → 유예 내 재기동 → server_id 유지 부활) ──
    $vilBHome = New-VillageHome 'vilB'
    $ckRegB = Get-LogLength $sessLog
    $vilB = Start-ServerProcess $villageExe $vilBHome '--seconds 30'
    $procs.Add($vilB)
    $mB = Wait-LogMatch $sessLog 'register -> server_id=(\d+)' 8000 $ckRegB
    if ($null -eq $mB) { throw 'village B 등록이 안 됐다 — #10 진행 불가' }
    $serverIdB = [int]$mB.Groups[1].Value

    $ck10 = Get-LogLength $sessLog
    Stop-Process -Id $vilB.Id -Force        # kill — F10: Unregister 없이 소켓만 닫힌다
    # 유예(2000ms) 안에 같은 host:port 로 재기동해야 부활 경로다 — 즉시 띄운다.
    $vilCHome = New-VillageHome 'vilC'
    $vilC = Start-ServerProcess $villageExe $vilCHome '--seconds 25'
    $procs.Add($vilC)
    $mGone10 = Wait-LogMatch $sessLog '링크 다운 — orphan \(항목 유지\)' 4000 $ck10
    $mC = Wait-LogMatch $sessLog 'register -> server_id=(\d+)' 8000 $ck10
    $serverIdC = if ($mC) { [int]$mC.Groups[1].Value } else { -1 }
    Add-Result '#10' (($null -ne $mGone10) -and ($serverIdC -eq $serverIdB)) `
        ("kill 후 orphan 로그 " + $(if ($mGone10) { '관측' } else { '없음' }) + " → 유예 내 재등록 server_id=$serverIdC (기대 $serverIdB 유지 = 부활)")

    # ── #9 — kill → orphan 전이 → 유예 초과 삭제 (session_gone 경유 즉시 orphan) ──
    # ⚠️ 「헬스체크 3회 초과」라는 설명과 실경로가 다르다 — kill 은 소켓이
    #    닫히므로 lazy 헬스 판정이 아니라 session_gone 이 즉시 orphan 을 찍는다.
    #    lazy 헬스 판정 경로는 #18(행 모사 — 연결 유지·하트비트만 중단)이 덮는다.
    $ck9 = Get-LogLength $sessLog
    Stop-Process -Id $vilC.Id -Force
    $mGone9 = Wait-LogMatch $sessLog '링크 다운 — orphan \(항목 유지\)' 4000 $ck9
    $mDel9 = Wait-LogMatch $sessLog 'sweep: .*항목 삭제=[1-9]' 6000 $ck9
    Add-Result '#9' (($null -ne $mGone9) -and ($null -ne $mDel9)) `
        ("orphan 전이 로그 " + $(if ($mGone9) { '관측' } else { '없음' }) + " → 유예(2000ms) 초과 sweep 삭제 로그 " + $(if ($mDel9) { '관측' } else { '없음' }))

    # ── #16 — 가짜 Register 2건(capacity 10 vs 100) → Login 반복 → load 최소 배정 ──
    $fake1 = Connect-Tcp $SessionS2sPort
    $fs1 = $fake1.GetStream()
    Send-S2sFrame $fs1 $MsgRegister 1 (New-RegisterBody $VerOk 7001 10 0 '127.0.0.1')
    $r1 = Read-S2sFrame $fs1 3000
    $fake2 = Connect-Tcp $SessionS2sPort
    $fs2 = $fake2.GetStream()
    Send-S2sFrame $fs2 $MsgRegister 1 (New-RegisterBody $VerOk 7002 100 0 '127.0.0.1')
    $r2 = Read-S2sFrame $fs2 3000
    $reg16Ok = ($null -ne $r1) -and ($null -ne $r2) -and ($r1.MsgId -eq $MsgRegisterAck) -and ($r2.MsgId -eq $MsgRegisterAck)

    # 기대값은 current=0 과도기 식(예약 수/capacity)으로 계산한다(§18-2 원식으로
    # 적으면 거짓 실패). 첫 회는 0/10 vs 0/100 동률 → server_id 최소(fake1=7001),
    # 둘째부터 1/10 > k/100 (k<10) → fake2=7002.
    $ports16 = @()
    foreach ($pid16 in 2001..2003) {
        $cli = Connect-Tcp $SessionClientPort
        $st = $cli.GetStream()
        Send-ClientFrame $st $CliSessionLoginReq (New-LoginBody $pid16)
        $af = Read-ClientFrame $st 3000
        if ($null -ne $af -and $af.MsgId -eq $CliSessionLoginAck) {
            $a = ConvertFrom-LoginAckBody $af.Body
            $ports16 += $a.Port
        } else {
            $ports16 += -1
        }
        $cli.Close()
    }
    Add-Result '#16' ($reg16Ok -and ($ports16[0] -eq 7001) -and ($ports16[1] -eq 7002) -and ($ports16[2] -eq 7002)) `
        ("배정 포트 수열=" + ($ports16 -join ',') + " (기대 7001,7002,7002 — 동률 첫 회 server_id 최소, 이후 load 최소)")
    # 예약 3건의 pending 이 타임아웃(500ms)으로 끝나도록 링크를 잠시 살려 둔다 —
    # 즉시 닫으면 같은 pending 이 링크 다운 갈래로 끝나 항등식의 타임아웃 항이
    # 비는 경주가 생긴다. 그 뒤 닫아 orphan → 유예 삭제로 이후와 격리한다.
    Start-Sleep -Milliseconds 1300
    $fake1.Close(); $fake2.Close()

    # ── #8 — 가짜 Register major 불일치 → result=1 · 연결 유지 ─────────────
    # 연결 유지 확인은 같은 소켓에 같은(나쁜 버전) Register 를 다시 보내 두
    # 번째 RegisterAck 를 받는 것으로 한다 — handle_register 는 이전 등록
    # 상태를 안 따지므로 몇 번을 보내도 매번 회신하고, "버전 거부는 끊지
    # 않는다"는 계약 자체가 이 테스트가 검증하려는 대상이라 프로브와 대상이
    # 일치한다(별도 메커니즘에 기대지 않아 그 메커니즘이 나중에 구현되며
    # 깨질 일이 없다). 이전에는 미정의 msg_id(0x80FF) → Unsupported 왕복을
    # 썼으나 성립하지 않았다 — Unsupported 회신은 is_request_from_village(id)
    # 가 참일 때만 도는 스위치의 default: 안에서만 나가고
    # (session_router.cpp:345-545), 그 목록 밖의 값은
    # 완전히 별개인 무응답 catch-all(:678-682)로 떨어진다. FullSync·DrainComplete 도 못 쓴다 —
    # 둘 다 전용 case 로 갔고 정상 처리(알림)면 무응답이라 "살아있다"의
    # 증거가 안 된다.
    $fake8 = Connect-Tcp $SessionS2sPort
    $fs8 = $fake8.GetStream()
    Send-S2sFrame $fs8 $MsgRegister 1 (New-RegisterBody $VerBad 7008 10 0 '127.0.0.1')
    $r8 = Read-S2sFrame $fs8 3000
    $nack = if ($null -ne $r8 -and $r8.MsgId -eq $MsgRegisterAck) { ConvertFrom-RegisterAckBody $r8.Body } else { $null }
    Send-S2sFrame $fs8 $MsgRegister 2 (New-RegisterBody $VerBad 7008 10 0 '127.0.0.1')
    $r8b = Read-S2sFrame $fs8 3000
    $nack2 = if ($null -ne $r8b -and $r8b.MsgId -eq $MsgRegisterAck) { ConvertFrom-RegisterAckBody $r8b.Body } else { $null }
    Add-Result '#8' (($null -ne $nack) -and ($nack.Result -eq 1) -and ($nack.ServerId -eq 0) -and ($null -ne $nack2) -and ($nack2.Result -eq 1) -and ($nack2.ServerId -eq 0) -and ($r8b.Seq -eq 2)) `
        ("RegisterAck result=" + $(if ($nack) { $nack.Result } else { '(무응답)' }) + " server_id=" + $(if ($nack) { $nack.ServerId } else { '-' }) + " · 재전송 RegisterAck " + $(if ($nack2) { "수신(result=$($nack2.Result), 연결 유지)" } else { '없음' }))

    # ── #8b — 미등록 연결의 Heartbeat → 절단 (영구 고립 방지) ──────────────
    $ck8b = Get-LogLength $sessLog
    Send-S2sFrame $fs8 $MsgHeartbeat 3 ([byte[]](0, 0, 0, 0))
    $dead8b = Read-S2sFrame $fs8 3000
    $m8b = Wait-LogMatch $sessLog 'heartbeat 미등록 연결 — 끊는다' 3000 $ck8b
    Add-Result '#8b' (($null -eq $dead8b) -and ($null -ne $m8b)) `
        ("미등록 Heartbeat → Ack 없음·절단=" + ($null -eq $dead8b) + " · 로그=" + ($null -ne $m8b) + " (재연결→Register 자가 복구 경로)")
    $fake8.Close()

    # ── #11 — body_size > 4092 프레임 주입 → 즉시 절단 ────────────────────
    # 직전 시나리오(#16·#8)가 닫은 소켓들의 'closed' 로그가 늦게 찍힐 수 있다 —
    # 체크포인트가 그 잔여 로그를 #11 것으로 오인하지 않게 잠깐 흘려보낸다.
    Start-Sleep -Milliseconds 400
    $ck11 = Get-LogLength $sessLog
    $fake11 = Connect-Tcp $SessionS2sPort
    $fs11 = $fake11.GetStream()
    $hdr = New-S2sHeader 4093 $MsgHeartbeat 1     # 헤더만 보내도 sizer 가 -1 로 판정한다
    $fs11.Write($hdr, 0, 8); $fs11.Flush()
    $dead11 = Read-S2sFrame $fs11 3000            # 절단이면 null (0바이트 종료)
    # 'closed' 는 임의 세션 종료에도 찍힌다 — sizer 가 음수를 반환할 때만 나오는
    # protocol violation 줄이 「명시 절단」과 「조용한 멈춤」을 정확히 가른다.
    $mClosed = Wait-LogMatch $sessLog '#\d+ protocol violation — closing' 3000 $ck11
    Add-Result '#11' (($null -eq $dead11) -and ($null -ne $mClosed)) `
        ("body_size=4093 주입 → 응답 없이 소켓 종료=" + ($null -eq $dead11) + " · session closed 로그=" + ($null -ne $mClosed) + " (조용한 멈춤 아님)")
    $fake11.Close()

    # ── #12 — FullSync(빈 청크, seq≠0 이어도) 주입 → 무응답(알림) ───────────
    # FullSync 는 전용 case 로 갔다(§6 E-b) — 대응 Ack 가 proto 에
    # 없는 알림이라 seq 값과 무관하게 항상 무응답이다. body 는 유효한 최소
    # 프레임(chunk_idx=0·chunk_total=1·count=0, 6B)이어야 한다 — 0B 를 넣으면
    # decode_full_sync 가 실패해 bad_body 로 끊기므로, 그건 이 항목이 재는
    # "정상 처리"가 아니라 "규약 위반" 갈래가 된다(그 갈래는 별도로 재지 않는다
    # — decode 실패 처리는 #19·#20 과 같은 형태이고 이 파일에서 이미 여러 번
    # 검증된 패턴이다).
    $fake12 = Connect-Tcp $SessionS2sPort
    $fs12 = $fake12.GetStream()
    Send-S2sFrame $fs12 $MsgFullSync 7 ([byte[]](0, 0, 0, 1, 0, 0))
    $r12 = Read-S2sFrame $fs12 1200
    Add-Result '#12' ($null -eq $r12) `
        ("FullSync(빈 청크) seq=7 주입 → 무응답=" + ($null -eq $r12) + " (알림이라 seq 무관 무응답)")

    # ── #13 — PlayerEnter(알림, 유효 8B) 주입 → 응답 없음 ──────────────────
    # 미등록 연결이라 registry_.server_id_of 가 0 을 돌려주므로 접속 테이블에는
    # 안 들어간다(그 검증은 이 항목이 아니라 §6 E-a 전용 시나리오 — 아래
    # "#26" 참조) — 여기서는 seq 와 무관하게 무응답인 것만 본다(#12 와 짝).
    Send-S2sFrame $fs12 $MsgPlayerEnter 0 ([byte[]](0, 0, 0, 0, 0, 0, 4, 210))
    $r13 = Read-S2sFrame $fs12 1200
    Add-Result '#13' ($null -eq $r13) `
        ("PlayerEnter(유효 8B) seq=0 주입 → 무응답=" + ($null -eq $r13))
    $fake12.Close()

    # ── #15 — 고정 길이 != 위반 (s2s Heartbeat 3B · 클라 Login 7B) → 각 절단 ──
    $ck15 = Get-LogLength $sessLog
    $fake15 = Connect-Tcp $SessionS2sPort
    $fs15 = $fake15.GetStream()
    Send-S2sFrame $fs15 $MsgHeartbeat 1 ([byte[]](1, 2, 3))
    $dead15a = Read-S2sFrame $fs15 3000
    $m15a = Wait-LogMatch $sessLog 's2s heartbeat body=3 \(want 4\)' 3000 $ck15
    $fake15.Close()
    $cli15 = Connect-Tcp $SessionClientPort
    $st15 = $cli15.GetStream()
    Send-ClientFrame $st15 $CliSessionLoginReq ([byte[]](1, 2, 3, 4, 5, 6, 7))
    $dead15b = Read-ClientFrame $st15 3000
    $m15b = Wait-LogMatch $sessLog 'session-login body=7 \(want 8\)' 3000 $ck15
    $cli15.Close()
    Add-Result '#15' (($null -eq $dead15a) -and ($null -ne $m15a) -and ($null -eq $dead15b) -and ($null -ne $m15b)) `
        ("hb3B: 절단=" + ($null -eq $dead15a) + " 로그=" + ($null -ne $m15a) + " · login7B: 절단=" + ($null -eq $dead15b) + " 로그=" + ($null -ne $m15b))

    # ── #23 — 클라 포트 body_size > kMaxBodySize(4096) 주입 → 즉시 절단 ──────
    # #11 의 클라 포트 쌍둥이 — client_frame_size 의 상한 검사가 죽으면(뮤턴트)
    # 이 프레임은 4100B 수신 버퍼에 영원히 안 담겨 조용한 멈춤이 된다.
    $ck23 = Get-LogLength $sessLog
    $cli23 = Connect-Tcp $SessionClientPort
    $st23 = $cli23.GetStream()
    $hdr23 = [byte[]](0x10, 0x01, 0, 11)          # body_size=4097 · msg=kPingReq — 헤더만 보낸다
    $st23.Write($hdr23, 0, 4); $st23.Flush()
    $dead23 = Read-ClientFrame $st23 3000
    $m23 = Wait-LogMatch $sessLog '#\d+ protocol violation — closing' 3000 $ck23
    Add-Result '#23' (($null -eq $dead23) -and ($null -ne $m23)) `
        ("클라 body_size=4097 주입 → 응답 없음=" + ($null -eq $dead23) + " · protocol violation 로그=" + ($null -ne $m23))
    $cli23.Close()

    # ── #17 — Unregister(0B) → 즉시 삭제 → 같은 Unregister 재주입(멱등) ─────
    $fake17 = Connect-Tcp $SessionS2sPort
    $fs17 = $fake17.GetStream()
    Send-S2sFrame $fs17 $MsgRegister 1 (New-RegisterBody $VerOk 7017 10 0 '127.0.0.1')
    $r17 = Read-S2sFrame $fs17 3000
    $ck17 = Get-LogLength $sessLog
    Send-S2sFrame $fs17 $MsgUnregister 2 ([byte[]]::new(0))
    $u1 = Read-S2sFrame $fs17 3000
    Send-S2sFrame $fs17 $MsgUnregister 3 ([byte[]]::new(0))
    $u2 = Read-S2sFrame $fs17 3000
    $mIdem = Wait-LogMatch $sessLog '항목 없음 — 멱등 성공' 3000 $ck17
    Add-Result '#17' (($null -ne $r17) -and ($null -ne $u1) -and ($u1.MsgId -eq $MsgUnregisterAck) -and ($u1.Seq -eq 2) `
        -and ($null -ne $u2) -and ($u2.MsgId -eq $MsgUnregisterAck) -and ($u2.Seq -eq 3) -and ($null -ne $mIdem)) `
        ("1회차 Ack=" + ($null -ne $u1) + " · 2회차 Ack=" + ($null -ne $u2) + " · 멱등 로그=" + ($null -ne $mIdem) + " (없음도 성공)")
    $fake17.Close()

    # ── #19 — Unregister body 1B (0B 확정 위반) → 절단 ─────────────────────
    $ck19 = Get-LogLength $sessLog
    $fake19 = Connect-Tcp $SessionS2sPort
    $fs19 = $fake19.GetStream()
    Send-S2sFrame $fs19 $MsgUnregister 1 ([byte[]](7))
    $dead19 = Read-S2sFrame $fs19 3000
    $m19 = Wait-LogMatch $sessLog 's2s unregister body=1 \(want 0\)' 3000 $ck19
    Add-Result '#19' (($null -eq $dead19) -and ($null -ne $m19)) `
        ("Unregister 1B → 절단=" + ($null -eq $dead19) + " · bad_body 로그=" + ($null -ne $m19))
    $fake19.Close()

    # ── #20 — Register body 5B (고정부 12B 미만 — decode 실패) → 절단 ──────
    $ck20 = Get-LogLength $sessLog
    $fake20 = Connect-Tcp $SessionS2sPort
    $fs20 = $fake20.GetStream()
    Send-S2sFrame $fs20 $MsgRegister 1 ([byte[]](1, 2, 3, 4, 5))
    $dead20 = Read-S2sFrame $fs20 3000
    $m20 = Wait-LogMatch $sessLog 'register decode 실패 body=5' 3000 $ck20
    Add-Result '#20' (($null -eq $dead20) -and ($null -ne $m20)) `
        ("Register 5B → 절단=" + ($null -eq $dead20) + " · decode 실패 로그=" + ($null -ne $m20))
    $fake20.Close()

    # ── #18 — 행 모사(lazy orphan): 등록 후 하트비트 중단·연결 유지 → 헬스 초과
    #          → Login(lazy 전이) → 같은 host:port 재 Register → 부활+stale 회수 ──
    $fakeG = Connect-Tcp $SessionS2sPort
    $fsG = $fakeG.GetStream()
    Send-S2sFrame $fsG $MsgRegister 1 (New-RegisterBody $VerOk 7020 5 0 '127.0.0.1')
    $rG = Read-S2sFrame $fsG 3000
    $regGOk = ($null -ne $rG) -and ($rG.MsgId -eq $MsgRegisterAck)
    Start-Sleep -Milliseconds 1800      # health 500ms×3=1500ms 초과 — 하트비트는 일부러 안 보낸다(행 모사)
    $ck18 = Get-LogLength $sessLog
    $cli18 = Connect-Tcp $SessionClientPort
    $st18 = $cli18.GetStream()
    Send-ClientFrame $st18 $CliSessionLoginReq (New-LoginBody 3001)
    $af18 = Read-ClientFrame $st18 3000
    $a18 = if ($null -ne $af18 -and $af18.MsgId -eq $CliSessionLoginAck) { ConvertFrom-LoginAckBody $af18.Body } else { $null }
    $cli18.Close()
    # G 는 연결이 살아 있는 채 lazy orphan 됐다 — 같은 host:port 새 연결로 재 Register 하면
    # 부활 + 잔존 홀드(stale_link) 회수가 일어난다((a) 갈래).
    $fakeH = Connect-Tcp $SessionS2sPort
    $fsH = $fakeH.GetStream()
    Send-S2sFrame $fsH $MsgRegister 1 (New-RegisterBody $VerOk 7020 5 0 '127.0.0.1')
    $rH = Read-S2sFrame $fsH 3000
    $mRevive = Wait-LogMatch $sessLog '부활 — 잔존 홀드 release' 3000 $ck18
    $fakeG.Close()                       # 옛 연결의 뒤늦은 session_gone — 매칭 실패 no-op ((b) 갈래 로그)
    $mNoop = Wait-LogMatch $sessLog '항목 없음 no-op' 3000 $ck18
    $fakeH.Close()
    Add-Result '#18' ($regGOk -and ($null -ne $a18) -and ($a18.Result -eq 1) -and ($null -ne $rH) -and ($null -ne $mRevive) -and ($null -ne $mNoop)) `
        ("lazy 전이 후 Login=no_server(" + $(if ($a18) { $a18.Result } else { '-' }) + ") · 부활+stale 회수 로그=" + ($null -ne $mRevive) + " · 옛 연결 no-op 로그=" + ($null -ne $mNoop) + " — 홀드 수지는 #14 요약에서")

    # ── #18b — lazy orphan 을 부활 없이 방치 → sweep 삭제가 stale 홀드 회수 ──
    # #18 은 부활(register)이 stale 을 회수하는 (a) 갈래였다 — 여기는 회수 주체가
    # sweep 인 갈래다. 연결은 계속 산 채(행 모사) 유예를 넘긴다.
    $fakeK = Connect-Tcp $SessionS2sPort
    $fsK = $fakeK.GetStream()
    Send-S2sFrame $fsK $MsgRegister 1 (New-RegisterBody $VerOk 7030 5 0 '127.0.0.1')
    $rK = Read-S2sFrame $fsK 3000
    Start-Sleep -Milliseconds 1800                # 헬스(500ms×3) 초과 — 하트비트 없음
    $ck18b = Get-LogLength $sessLog
    $cliK = Connect-Tcp $SessionClientPort        # lazy 판정은 assign 만 한다 — Login 으로 트리거
    $stK = $cliK.GetStream()
    Send-ClientFrame $stK $CliSessionLoginReq (New-LoginBody 3002)
    Read-ClientFrame $stK 3000 | Out-Null
    $cliK.Close()
    $mStale = Wait-LogMatch $sessLog 'sweep: .*stale 홀드=[1-9]' 6000 $ck18b
    Add-Result '#18b' (($null -ne $rK) -and ($null -ne $mStale)) `
        ("lazy orphan 방치 → 유예 초과 sweep 의 stale 회수 로그=" + ($null -ne $mStale))
    $fakeK.Close()

    # ── #26 — Unregister 유예 안/밖의 heartbeat (E-c — 항목13도
    #    이 항목이 덮는다 — Unregister 직후 연결을 유지한 채 heartbeat 를
    #    보내는 모양이 그 항목이 요구하는 것과 같다. 실물 드레인 타이밍
    #    자체는 #28 조사로 이미 "이 하네스로는 구조적으로 못 잰다"로 닫혔다 —
    #    #26 은 유예 판정 로직을, #28 조사는 그 실물 타이밍 재현 불가를 각각
    #    맡는다) ──────────────────────
    # sess1 은 unregister_grace_ms=800 로 떴다(New-SessionHome 기본값). Register
    # → Unregister 직후(유예 안) heartbeat 는 Ack 를 받고 살아 있어야 하고,
    # 유예(800ms)를 넘긴 뒤의 heartbeat 는 기존 미등록 절단 분기를 그대로 타야
    # 한다 — 이 항목이 없으면 E-c 의 두 갈래(유예 안/밖) 중 어느 쪽도 관측되지
    # 않는다(#8b 는 애초에 Register 를 안 한 연결이라 유예 자체가 안 걸린다).
    $fake26 = Connect-Tcp $SessionS2sPort
    $fs26 = $fake26.GetStream()
    Send-S2sFrame $fs26 $MsgRegister 1 (New-RegisterBody $VerOk 7026 10 0 '127.0.0.1')
    Read-S2sFrame $fs26 3000 | Out-Null
    Send-S2sFrame $fs26 $MsgUnregister 2 ([byte[]]::new(0))
    $u26 = Read-S2sFrame $fs26 3000
    Send-S2sFrame $fs26 $MsgHeartbeat 3 ([byte[]](0, 0, 0, 0))
    $hbGrace = Read-S2sFrame $fs26 1500
    $graceOk = ($null -ne $hbGrace) -and ($hbGrace.MsgId -eq $MsgHeartbeatAck) -and ($hbGrace.Seq -eq 3)
    Start-Sleep -Milliseconds 900        # unregister_grace_ms(800) 초과
    Send-S2sFrame $fs26 $MsgHeartbeat 4 ([byte[]](0, 0, 0, 0))
    $hbAfter = Read-S2sFrame $fs26 3000
    $afterOk = ($null -eq $hbAfter)
    Add-Result '#26' (($null -ne $u26) -and ($u26.MsgId -eq $MsgUnregisterAck) -and $graceOk -and $afterOk) `
        ("Unregister 뒤 유예 안 heartbeat Ack=" + $graceOk + " · 유예(800ms) 초과 후 heartbeat 절단=" + $afterOk)
    $fake26.Close()

    # ── #27 — 접속 테이블(E-a) — PlayerEnter/PlayerLeave/FullSync 가 실제로
    #         반영되는가. [SESS ] connections 줄로 판정한다(아래 최종 카운터
    #         절). 이 연결은 끝까지 열어 둔다 — 닫으면 orphan 유예 초과로
    #         drop_server_connections 가 한 번 더 돌아 removed 항이 이 항목의
    #         명시 조작분과 섞인다.
    # ⛔ 순서가 중요하다 — FullSync 를 먼저, PlayerLeave 를 나중에 보낸다.
    # PlayerLeave 를 먼저 보내고 그 뒤에 FullSync(clear) 를 보내면, clear 가
    # 남은 항목을 다시 지우면서 removed 값이 "PlayerLeave 가 지운 것"과
    # "FullSync 가 지운 것"을 구분 못 하게 상쇄한다 — player_left() 를
    # 통째로 no-op 으로 바꿔도 같은 최종 숫자(added=4 removed=2 remain=2)가
    # 나온다(실측 확인 — 항목16 "총계는 상쇄에 뚫린다"와 같은
    # 함정). 이 순서(FullSync 로 5001·5002 를 깐 뒤 5003 을 Enter·Leave)면
    # PlayerLeave 가 안 도는 뮤턴트에서 removed·remain 이 둘 다 갈린다.
    $fake27 = Connect-Tcp $SessionS2sPort
    $fs27 = $fake27.GetStream()
    Send-S2sFrame $fs27 $MsgRegister 1 (New-RegisterBody $VerOk 7027 10 0 '127.0.0.1')
    Read-S2sFrame $fs27 3000 | Out-Null
    # FullSync(첫 청크, 5001·5002) — 새로 등록된 서버라 클리어는 빈 상태에
    # 대한 것이다(added 만 오른다). chunk_idx=0·chunk_total=1·count=2(6B)
    # 뒤에 u64 빅엔디언 player_id 둘(5001=0x1389·5002=0x138A)을 붙인다 —
    # 리터럴로 적는 이유는 배열 이어붙이기(+)가 New-LoginBody 의 comma
    # 래핑과 얽혀 원소 수를 조용히 틀리게 만든 적이 있어서다(실측).
    $fsList = New-Object System.Collections.Generic.List[byte]
    $fsList.AddRange([byte[]](0, 0, 0, 1, 0, 2))
    $fsList.AddRange([byte[]](0, 0, 0, 0, 0, 0, 0x13, 0x89))
    $fsList.AddRange([byte[]](0, 0, 0, 0, 0, 0, 0x13, 0x8A))
    $fsBody = $fsList.ToArray()
    Send-S2sFrame $fs27 $MsgFullSync 0 $fsBody
    Send-S2sFrame $fs27 $MsgPlayerEnter 0 (New-LoginBody 5003)
    Send-S2sFrame $fs27 $MsgPlayerLeave 0 (New-LoginBody 5003)   # 5003 제거 — 5001·5002 는 안 건드린다
    Send-S2sFrame $fs27 $MsgPlayerLeave 0 (New-LoginBody 5003)   # 재차 — no-op 이어야 한다(removed 불변이 그 증거)
    Start-Sleep -Milliseconds 300        # 처리가 로그·집계에 반영될 시간
    Add-Result '#27(전제)' $true "FullSync(5001,5002 주입) · PlayerEnter 5003 · PlayerLeave 5003 x2(1 성공 1 no-op) 주입 완료"

    # ── #28 조사 결과 — 실물 마을 정상 종료 드레인 중 heartbeat 유예 실경로는
    # 여기서 결정적으로 못 만든다(E-c 자체는 실재 — 시도 자체는 못 만든 것이다).
    # 원인을 village.exe·session.exe 양쪽 로그의 타임스탬프로 직접 쟀다 —
    # unregister_and_wait() 가 돌아온 뒤 server.stop() 의 idle 드레인이 끝나
    # s2s_link.stop() 이 그 스레드를 세울 때까지의 창이 실측 약 3ms 다
    # (예: "23:47:49.051 accept loop out" → "23:47:49.054 [S2S ] connects=…").
    # heartbeat_ms 를 50ms → 5ms → 1ms 로 줄여 가며 반복해도 이 3ms 창 안에
    # 한 틱도 못 걸었다 — GetTickCount64() 의 ms 해상도와 실제 스케줄링 지터가
    # 이 창보다 넓어서, 통과시키려면 운영 코드에 테스트 전용 지연을 넣어야
    # 하는데 그러면 검증 대상 자체가 오염된다. 유예 메커니즘의 판정 로직
    # (미등록 연결의 heartbeat 을 유예 창 안/밖으로 가르는 분기)은 #26 이 가짜
    # 연결로 이미 결정적으로 덮는다 — 못 덮는 것은 "실물 드레인이 그 분기를
    # 실제로 타는가"라는 타이밍 그 자체뿐이다. session_router.cpp 의
    # "heartbeat 유예 중(unregister)" 로그 줄은 운영 관측용으로 남긴다.

    # ── session 자연 종료(--seconds 90 만료) → #14 + 최종 카운터 판정 ───────
    # 마진 관측 — 시나리오 총소요가 예산(90s)에 접근하면 값을 올리기 전에 여기서
    # 먼저 보인다.
    Write-Host ("phase1 시나리오 소요 {0:n1}s / 예산 90s" -f ((Get-Date) - $phase1Start).TotalSeconds)
    $sessProc.WaitForExit(60000) | Out-Null
    if (-not $sessProc.HasExited) { throw 'session.exe 가 --seconds 만료로 종료되지 않았다' }
    $sessText = Read-LogText $sessLog
    $exit1Ok = ($sessProc.ExitCode -eq 0)

    $mS1 = [regex]::Match($sessText, '\[SESS \] register=(\d+) reject_full=(\d+) reject_ver=(\d+) orphan=(\d+) removed=(\d+)')
    $mS2 = [regex]::Match($sessText, '\[SESS \] assign ok=(\d+) no_server=(\d+) \| reserve issued=(\d+) overwrite=(\d+) expired=(\d+) revoked=(\d+) remain=(\d+)')
    $mS3 = [regex]::Match($sessText, '\[SESS \] link hold acquire=(\d+) release=(\d+) \(unreg=(\d+) gone=(\d+) stale_revive=(\d+) stale_sweep=(\d+)\) stop_leftover=(\d+)')
    $mS4 = [regex]::Match($sessText, '\[SESS \] pending sent=(\d+) send_fail=(\d+) ack=(\d+) rejected=(\d+) unsupported=(\d+) timeout=(\d+) link_down=(\d+) stop=(\d+) skip_nolink=(\d+) remain=(\d+)')
    $mS5 = [regex]::Match($sessText, '\[SESS \] connections added=(\d+) removed=(\d+) fullsync_replaced=(\d+) remain=(\d+)')
    if ($mS1.Success -and $mS2.Success -and $mS3.Success -and $mS4.Success -and $mS5.Success) {
        # 종료 요약 전문 — 스크래치가 finally 에서 지워지므로 판정 정본 5줄을
        # 하네스 출력에 그대로 남긴다(회귀 보고가 원문을 인용할 수 있게).
        Write-Host '--- [SESS ] 종료 요약 전문 (phase 1) ---'
        Write-Host $mS1.Value
        Write-Host $mS2.Value
        Write-Host $mS3.Value
        Write-Host $mS4.Value
        Write-Host $mS5.Value
        # #27 — 접속 테이블(E-a) 최종 집계. added/removed/remain 은 #27 시나리오
        # 만의 순수 결과다 — PlayerEnter/PlayerLeave 를 보내는 연결은 #27 뿐이고
        # (#12·#13 은 미등록 연결이라 server_id_of 가 0 을 돌려줘 registry 를 안
        # 만진다), #27 이 쓰는 server_id 의 접속 항목을 다른 시나리오가 안
        # 건드린다. FullSync(5001·5002) 를 먼저 깐 뒤 PlayerEnter/Leave 를
        # 5003 하나로 국한한 것이 핵심이다 — PlayerLeave 가 통째로 no-op 인
        # 뮤턴트여도 순서가 반대(Leave 먼저·FullSync 나중)면 clear 가 그
        # 차이를 지워 added=4 removed=2 remain=2 로 뮤턴트와 정상이 같은
        # 숫자를 낸다(실측 확인 — 항목16 "총계는
        # 상쇄에 뚫린다"와 같은 함정). 지금 순서면 뮤턴트는 removed=0
        # remain=3 으로 갈라진다. 추가 3(FullSync 의 5001·5002 + PlayerEnter
        # 5003) · 제거 1(PlayerLeave 5003 성공, 재차는 no-op — removed 불변이
        # 그 증거) · 잔여 2(5001·5002).
        # ⛔ fullsync_replaced 만은 >= 로 본다 — 이 카운터는 서버 전역이라, 이
        # 페이즈에서 등록·재등록에 성공하는 마을(vilA 최초 등록·#9/#10 의
        # 부활 등)마다 자동으로 나가는 FullSync(마을 on_register_ack)도 같이
        # 센다 — #27 은 그중 하나(자기 몫)를 보탤 뿐이다.
        $connAdded = [int]$mS5.Groups[1].Value; $connRemoved = [int]$mS5.Groups[2].Value
        $fsReplaced = [int]$mS5.Groups[3].Value; $connRemain = [int]$mS5.Groups[4].Value
        Add-Result '#27' (($connAdded -eq 3) -and ($connRemoved -eq 1) -and ($fsReplaced -ge 1) -and ($connRemain -eq 2)) `
            ("connections added=$connAdded(기대 3) removed=$connRemoved(기대 1) fullsync_replaced=$fsReplaced(기대 >=1 — 마을 자동 발신 포함) remain=$connRemain(기대 2)")
        $issued = [int]$mS2.Groups[3].Value; $overwrite = [int]$mS2.Groups[4].Value
        $expired = [int]$mS2.Groups[5].Value; $revoked = [int]$mS2.Groups[6].Value
        $remainRes = [int]$mS2.Groups[7].Value
        $acq = [int]$mS3.Groups[1].Value; $rel = [int]$mS3.Groups[2].Value
        $leftover = [int]$mS3.Groups[7].Value
        $pSent = [int]$mS4.Groups[1].Value; $pSendFail = [int]$mS4.Groups[2].Value
        $pAck = [int]$mS4.Groups[3].Value
        $pRej = [int]$mS4.Groups[4].Value; $pUnsup = [int]$mS4.Groups[5].Value
        $pTimeout = [int]$mS4.Groups[6].Value; $pDown = [int]$mS4.Groups[7].Value
        $pStop = [int]$mS4.Groups[8].Value; $pSkip = [int]$mS4.Groups[9].Value

        # #5 — 같은 player_id 재로그인이 덮어쓰기로 집계됐는가(중복 차단 아님).
        Add-Result '#5' ($relogin0k -and ($overwrite -ge 1)) `
            ("재로그인 LoginAck ok=" + $relogin0k + " · [SESS ] overwrite=$overwrite (기대 >=1 — 잠정 정책: 차단 아님)")

        # #14 — 정상 종료 판정. 「pending 잔량 0」은 shutdown 이 비운 뒤의 값이라
        # 항진명제다 — 대신 발급·완료 갈래의 항등식으로 잔량 부재를 증명한다.
        # 홀드도 총계(acq==rel)만 보면 release 를 엉뚱한 하위 칸에 귀속시키는
        # 변경이 무검출이라(#22 의 총계 상쇄와 동형) 하위 4갈래 각각의 하한을
        # 함께 단언한다. 1번 줄 카운터도 시나리오 역산 하한으로 잰다 — register 는
        # A·B·C·f1·f2·F·G·H·K 의 9, orphan 은 A·B·C·f1·f2·G·H·K 의 8(여유 2),
        # removed 는 A·B·C·f1·f2·F·K 의 7(여유 2).
        # skip_nolink==0 은 현 구조에서 도달 불가다(link 탈착이 전부 orphan 표시와
        # 같은 임계구역이라 「비-orphan 인데 link 없음」이 안 생긴다) — 검증이
        # 아니라 방어가 살아 있는지의 하한 확인용으로 둔다.
        # leftover==0 은 「이 하네스는 모든 소켓을 닫고 끝낸다」가 전제다 — stop
        # 시점까지 S2S 연결을 여는 항목이 생기면 그 회차는 관찰로 강등할 것.
        $reg1 = [int]$mS1.Groups[1].Value; $rejVer = [int]$mS1.Groups[3].Value
        $orph1 = [int]$mS1.Groups[4].Value; $rem1 = [int]$mS1.Groups[5].Value
        $resIdentity = ($issued -eq ($overwrite + $expired + $revoked + $remainRes))
        $pendIdentity = ($pSent -eq ($pAck + $pRej + $pUnsup + $pTimeout + $pDown + $pStop))
        $relBreakdown = ([int]$mS3.Groups[3].Value -ge 1) -and ([int]$mS3.Groups[4].Value -ge 1) `
            -and ([int]$mS3.Groups[5].Value -ge 1) -and ([int]$mS3.Groups[6].Value -ge 1)
        Add-Result '#14' ($exit1Ok -and $resIdentity -and $pendIdentity -and ($pTimeout -ge 1) -and ($pSkip -eq 0) -and ($pSendFail -eq 0) `
            -and ($acq -eq $rel) -and $relBreakdown -and ($leftover -eq 0) `
            -and ($reg1 -ge 9) -and ($orph1 -ge 6) -and ($rem1 -ge 5)) `
            ("exit=$($sessProc.ExitCode) · 예약 $issued=$overwrite+$expired+$revoked+$remainRes($resIdentity) · pending $pSent=$pAck+$pRej+$pUnsup+$pTimeout+$pDown+$pStop($pendIdentity) timeout=$pTimeout skip=$pSkip send_fail=$pSendFail · 홀드 $acq=$rel 하위4갈래>=1($relBreakdown) leftover=$leftover · register=$reg1(>=9) orphan=$orph1(>=6) removed=$rem1(>=5)")

        # #8(집계) — 버전 거부가 [SESS ] 카운터에도 잡혔는가(와이어 응답과 별개 축).
        Add-Result '#8(집계)' ($rejVer -ge 1) "[SESS ] reject_ver=$rejVer (기대 >=1 — #8 의 major 불일치 거부)"

        # #21 — 타임아웃·만료 갈래가 실제로 돌았는가(빈 갈래 봉쇄): #16 의 예약
        # 3건은 current=0 식으로 타임아웃 3·만료 경로로 끝나야 한다.
        Add-Result '#21' (($pTimeout -ge 3) -and ($expired -ge 4)) `
            ("pending timeout=$pTimeout (기대 >=3) · 예약 만료=$expired (기대 >=4)")
    } else {
        Add-Result '#5' $false '[SESS ] 요약 5줄을 못 찾았다'
        Add-Result '#14' $false '[SESS ] 요약 5줄을 못 찾았다'
        Add-Result '#21' $false '[SESS ] 요약 5줄을 못 찾았다'
        Add-Result '#27' $false '[SESS ] 요약 5줄을 못 찾았다'
    }

    # ═══════════════════════════════════════════════════════════════════════
    #  Phase 2 — capacity=1 로 재기동. #7 (정원 초과 거절)
    #  ⚠️ 함정 ②: 같은 포트 재사용 — 직전 프로세스 종료 후 짧은 대기를 둔다.
    # ═══════════════════════════════════════════════════════════════════════
    Start-Sleep -Milliseconds 700
    $sess2Home = New-SessionHome 'sess2' '1'
    $sess2Log = Join-Path $sess2Home 'logs\server.log'
    $sess2 = Start-ServerProcess $sessionExe $sess2Home '--seconds 10'
    $procs.Add($sess2)
    if ($null -eq (Wait-LogMatch $sess2Log 'session server up' 8000)) {
        throw 'phase 2 session.exe 기동 로그가 없다'
    }
    $fakeP = Connect-Tcp $SessionS2sPort
    $fsP = $fakeP.GetStream()
    Send-S2sFrame $fsP $MsgRegister 1 (New-RegisterBody $VerOk 7101 10 0 '127.0.0.1')
    $rP = Read-S2sFrame $fsP 3000
    $fakeQ = Connect-Tcp $SessionS2sPort
    $fsQ = $fakeQ.GetStream()
    Send-S2sFrame $fsQ $MsgRegister 1 (New-RegisterBody $VerOk 7102 10 0 '127.0.0.2')
    $rQ = Read-S2sFrame $fsQ 3000
    $ackQ = if ($null -ne $rQ -and $rQ.MsgId -eq $MsgRegisterAck) { ConvertFrom-RegisterAckBody $rQ.Body } else { $null }
    Add-Result '#7' (($null -ne $rP) -and ($null -ne $ackQ) -and ($ackQ.Result -eq $ResultFull) -and ($ackQ.ServerId -eq 0)) `
        ("capacity=1: 1건째 등록=" + ($null -ne $rP) + " · 2건째 result=" + $(if ($ackQ) { $ackQ.Result } else { '(무응답)' }) + " (기대 2=full)")

    # #7(멱등) — 가득 찬 테이블에서도 「같은 연결의 재등록」은 성공해야 한다(§8-2).
    # 재등록·부활은 항목을 늘리지 않으므로 정원 검사보다 매칭이 먼저다 — 검사를
    # 앞으로 옮기는 회귀는 이 단언만이 잡는다.
    $ackP1 = if ($null -ne $rP -and $rP.MsgId -eq $MsgRegisterAck) { ConvertFrom-RegisterAckBody $rP.Body } else { $null }
    Send-S2sFrame $fsP $MsgRegister 2 (New-RegisterBody $VerOk 7101 10 0 '127.0.0.1')
    $rP2 = Read-S2sFrame $fsP 3000
    $ackP2 = if ($null -ne $rP2 -and $rP2.MsgId -eq $MsgRegisterAck) { ConvertFrom-RegisterAckBody $rP2.Body } else { $null }
    Add-Result '#7(멱등)' (($null -ne $ackP1) -and ($null -ne $ackP2) -and ($ackP2.Result -eq 0) -and ($ackP2.ServerId -eq $ackP1.ServerId)) `
        ("만석 재등록 result=" + $(if ($ackP2) { $ackP2.Result } else { '(무응답)' }) + " server_id=" + $(if ($ackP2) { $ackP2.ServerId } else { '-' }) + " (기대 0·" + $(if ($ackP1) { $ackP1.ServerId } else { '?' }) + " 유지 — 정원 검사가 매칭보다 앞이면 full 거부가 된다)")
    $fakeP.Close(); $fakeQ.Close()
    $sess2.WaitForExit(15000) | Out-Null
    Add-Result '정상 종료(p2)' ($sess2.HasExited -and ($sess2.ExitCode -eq 0)) "phase 2 session.exe exit code = $($sess2.ExitCode)"

    # #7(수지) — 정원 거부는 「거부 갈래 acquire 금지」를 실행하는 유일한 시나리오다.
    # exit code 만으로는 홀드 미아가 안 보인다 — [SESS ] 로 직접 잰다.
    # leftover==0 단언은 「모든 소켓을 닫고 끝낸다」가 전제다(#14 와 같다).
    $s2Text = Read-LogText $sess2Log
    $p2S1 = [regex]::Match($s2Text, '\[SESS \] register=(\d+) reject_full=(\d+)')
    $p2S3 = [regex]::Match($s2Text, '\[SESS \] link hold acquire=(\d+) release=(\d+) \(unreg=\d+ gone=\d+ stale_revive=\d+ stale_sweep=\d+\) stop_leftover=(\d+)')
    if ($p2S1.Success -and $p2S3.Success) {
        $p2Full = [int]$p2S1.Groups[2].Value
        $p2Acq = [int]$p2S3.Groups[1].Value; $p2Rel = [int]$p2S3.Groups[2].Value
        $p2Left = [int]$p2S3.Groups[3].Value
        Add-Result '#7(수지)' (($p2Full -ge 1) -and ($p2Acq -eq $p2Rel) -and ($p2Left -eq 0)) `
            ("reject_full=$p2Full (기대 >=1) · 홀드 acquire=$p2Acq release=$p2Rel leftover=$p2Left (거부 갈래 acquire 금지 검증)")
    } else {
        Add-Result '#7(수지)' $false 'phase 2 [SESS ] 요약을 못 찾았다'
    }

    # ═══════════════════════════════════════════════════════════════════════
    #  Phase 3 — pending 의 종료 시점 회수. request_timeout 을 크게 잡아 타임아웃
    #  갈래와의 경주를 없앤다. stop 갈래 단독 유도는 실측으로 불가능했다 —
    #  서버 stop 이 소켓을 닫으며 session_gone 이 먼저 발화하면 같은 pending 이
    #  링크 다운 갈래로 회수되고, 그 발화 여부 자체가 보장 없는 레이스라(워커
    #  조기 이탈) stop 갈래는 그 레이스의 안전망이다. 그래서 판정은 「어느
    #  갈래인가」가 아니라 「두 갈래의 합이 전량인가」다.
    # ═══════════════════════════════════════════════════════════════════════
    Start-Sleep -Milliseconds 700
    # expire 도 크게 — 축소값(2000ms)이면 3차 로그인이 그 창을 넘겼을 때 예약
    # 만료가 「가드 없음」과 같은 실패 서명(overwrite=1)을 만들어 오진한다.
    # 만료 갈래 검증은 phase 1(#21)이 이미 덮는다.
    $sess3Home = New-SessionHome 'sess3' '16' '60000' '60000'
    $sess3Log = Join-Path $sess3Home 'logs\server.log'
    $sess3 = Start-ServerProcess $sessionExe $sess3Home '--seconds 12'
    $procs.Add($sess3)
    if ($null -eq (Wait-LogMatch $sess3Log 'session server up' 8000)) {
        throw 'phase 3 session.exe 기동 로그가 없다'
    }
    # fakeS — 두 건의 Reserve 를 받아 거절(result=1)로 답한다 (#22 재료)
    $fakeS = Connect-Tcp $SessionS2sPort
    $fsS = $fakeS.GetStream()
    Send-S2sFrame $fsS $MsgRegister 1 (New-RegisterBody $VerOk 7301 10 0 '127.0.0.1')
    $rS = Read-S2sFrame $fsS 3000
    $cli3 = Connect-Tcp $SessionClientPort
    $st3 = $cli3.GetStream()
    Send-ClientFrame $st3 $CliSessionLoginReq (New-LoginBody 4001)
    $a3a = Read-ClientFrame $st3 3000
    # 같은 player 재로그인 — S 뿐이라 다시 S 로 배정(덮어쓰기·새 발급 세대).
    # 1차 발급분의 「늦은 거절」이 이 산 예약을 지우면 안 된다 — 세대 가드 검증 재료.
    Send-ClientFrame $st3 $CliSessionLoginReq (New-LoginBody 4001)
    $a3a2 = Read-ClientFrame $st3 3000
    # fakeT — pending 을 링크 다운으로 끝낸다 (S 에 유효 예약 1건이 실려 load
    # 차이로 다음 로그인은 T 로 간다 — current=0 식)
    $fakeT = Connect-Tcp $SessionS2sPort
    $fsT = $fakeT.GetStream()
    Send-S2sFrame $fsT $MsgRegister 1 (New-RegisterBody $VerOk 7302 10 0 '127.0.0.2')
    $rT = Read-S2sFrame $fsT 3000
    Send-ClientFrame $st3 $CliSessionLoginReq (New-LoginBody 4002)
    $a3b = Read-ClientFrame $st3 3000
    $fakeT.Close()      # 즉시 — 이 pending 은 링크 다운 갈래로 끝나야 한다

    # ── #22 — ReserveAck 거절 → pending rejected + 예약 회수(세대 가드) ─────
    # revoked 총계만 보면 가드 유무를 못 가른다(가드 있음: 옛 세대 불발+현행 회수
    # =1 / 가드 없음: 옛 세대가 현행을 오회수+다음은 미발견=1 — 상쇄. 세대 가드를 제거한 회차가
    # 실측으로 잡은 탈출). 그래서 「거절 처리 뒤 재로그인」을 끼운다 — 가드가
    # 있으면 산 예약이 남아 덮어쓰기(overwrite 2)로, 없으면 오회수로 빈 자리라
    # 신규 발급(overwrite 1)으로 갈라지고, 이후 연쇄 거절의 revoked 도 1 vs 2 로
    # 갈라진다.
    $rsv1 = Read-S2sFrame $fsS 3000
    $rsv2 = Read-S2sFrame $fsS 3000
    if ($null -ne $rsv1) { Send-S2sFrame $fsS $MsgReserveAck $rsv1.Seq ([byte[]](1)) }
    Start-Sleep -Milliseconds 400       # seq1 거절이 처리된 뒤에 재로그인해야 결정적이다
    # 여기까지 등록 후 1.5초를 넘길 수 있다 — 하트비트로 S 의 신선도를 되살려야
    # 재로그인이 lazy 헬스 판정(500ms×3)에 걸려 no_server 로 새지 않는다.
    Send-S2sFrame $fsS $MsgHeartbeat 90 ([byte[]](0, 0, 0, 0))
    Read-S2sFrame $fsS 2000 | Out-Null
    Send-ClientFrame $st3 $CliSessionLoginReq (New-LoginBody 4001)
    $a3c = Read-ClientFrame $st3 3000
    $rsv3 = Read-S2sFrame $fsS 3000
    # seq2 거절은 죽은 세대(3차 로그인이 이미 덮었다) — 세대 가드의 불발 갈래다.
    # 수락(result=0)은 「살아 있는 세대」인 seq3 에 건다 — 죽은 세대에 걸면 세대
    # 가드가 result 검사를 가려 「수락도 회수한다」 결함이 안 보인다(항진명제).
    if ($null -ne $rsv2) { Send-S2sFrame $fsS $MsgReserveAck $rsv2.Seq ([byte[]](1)) }
    if ($null -ne $rsv3) { Send-S2sFrame $fsS $MsgReserveAck $rsv3.Seq ([byte[]](0)) }
    Start-Sleep -Milliseconds 400       # 수락이 처리된 「뒤」의 4차 로그인이어야 판별이 선다
    # 4차 로그인 — 수락이 예약을 지우지 않았다면 산 예약(3차 발급분)을 덮어써
    # overwrite=3 이 되고, 지웠다면(뮤턴트) 빈자리 신규 발급이라 overwrite=2 에
    # 머문다. 이 로그인의 발급분은 마지막 거절이 회수한다(revoked 1 vs 뮤턴트 2).
    Send-S2sFrame $fsS $MsgHeartbeat 91 ([byte[]](0, 0, 0, 0))
    Read-S2sFrame $fsS 2000 | Out-Null
    Send-ClientFrame $st3 $CliSessionLoginReq (New-LoginBody 4001)
    $a3d = Read-ClientFrame $st3 3000
    $cli3.Close()
    $rsv4 = Read-S2sFrame $fsS 3000
    $rsvOk = ($null -ne $rsv1) -and ($rsv1.MsgId -eq $MsgReserve) -and ($rsv1.Seq -eq 1) `
        -and ($null -ne $rsv2) -and ($rsv2.MsgId -eq $MsgReserve) -and ($rsv2.Seq -eq 2) `
        -and ($null -ne $rsv3) -and ($rsv3.MsgId -eq $MsgReserve) -and ($rsv3.Seq -eq 3) `
        -and ($null -ne $rsv4) -and ($rsv4.MsgId -eq $MsgReserve) -and ($rsv4.Seq -eq 4)
    if ($null -ne $rsv4) { Send-S2sFrame $fsS $MsgReserveAck $rsv4.Seq ([byte[]](1)) }
    Start-Sleep -Milliseconds 500       # 응답들이 처리될 시간

    $sess3.WaitForExit(20000) | Out-Null
    $s3Text = Read-LogText $sess3Log
    $p3S2 = [regex]::Match($s3Text, '\[SESS \] assign ok=(\d+) no_server=(\d+) \| reserve issued=(\d+) overwrite=(\d+) expired=(\d+) revoked=(\d+) remain=(\d+)')
    $p3S3 = [regex]::Match($s3Text, '\[SESS \] link hold acquire=(\d+) release=(\d+) \(unreg=\d+ gone=\d+ stale_revive=\d+ stale_sweep=\d+\) stop_leftover=(\d+)')
    $p3S4 = [regex]::Match($s3Text, '\[SESS \] pending sent=(\d+) send_fail=(\d+) ack=(\d+) rejected=(\d+) unsupported=(\d+) timeout=(\d+) link_down=(\d+) stop=(\d+) skip_nolink=(\d+) remain=(\d+)')
    if ($p3S4.Success -and $p3S2.Success) {
        $q = $p3S4.Groups
        $qSent = [int]$q[1].Value
        $qSum = [int]$q[3].Value + [int]$q[4].Value + [int]$q[5].Value + [int]$q[6].Value + [int]$q[7].Value + [int]$q[8].Value
        $qAck = [int]$q[3].Value; $qRej = [int]$q[4].Value
        $qDown = [int]$q[7].Value; $qStop = [int]$q[8].Value
        # stop/link_down 의 배분과 fakeS 홀드 잔존 여부는 단언하지 않는다 — 둘 다
        # session_gone 발화 레이스의 양면이라 어느 쪽도 정상이다(위 헤더 주석).
        # 단언은 「타임아웃 없이 전 갈래 합이 전량(4)이고, 종료 시점 회수가 T 의
        # 1건」이다.
        $p3Left = if ($p3S3.Success) { $p3S3.Groups[3].Value } else { '?' }
        Add-Result '#14b' (($null -ne $rS) -and ($null -ne $rT) -and ($qSent -eq 5) -and ($qSent -eq $qSum) -and (($qDown + $qStop) -eq 1)) `
            ("pending sent=$qSent=완료합$qSum · 종료 시점 회수 link_down=$qDown + stop=$qStop = 1 · stop_leftover=$p3Left(관찰만)")

        $g2 = $p3S2.Groups
        $p3Issued = [int]$g2[3].Value; $p3Ow = [int]$g2[4].Value
        $p3Exp = [int]$g2[5].Value; $p3Rev = [int]$g2[6].Value; $p3Rem = [int]$g2[7].Value
        $p3Identity = ($p3Issued -eq ($p3Ow + $p3Exp + $p3Rev + $p3Rem))
        Add-Result '#22' ($rsvOk -and ($qAck -eq 1) -and ($qRej -eq 3) -and ($p3Rev -eq 1) -and ($p3Ow -eq 3) -and $p3Identity) `
            ("Reserve 4건 수신 회신=" + $rsvOk + " · ack=$qAck (기대 1 — 산 세대 수락) · rejected=$qRej (기대 3) · revoked=$p3Rev (기대 1 — 수락이 지우는 뮤턴트면 2·세대 가드 제거면 3) · overwrite=$p3Ow (기대 3 — 수락이 지우는 뮤턴트면 2·세대 가드 제거면 1) · 예약 $p3Issued=$p3Ow+$p3Exp+$p3Rev+$p3Rem($p3Identity)")
    } else {
        Add-Result '#14b' $false 'phase 3 [SESS ] 요약을 못 찾았다'
        Add-Result '#22' $false 'phase 3 [SESS ] 요약을 못 찾았다'
    }
    try { $fakeS.Close() } catch {}

    # ═══════════════════════════════════════════════════════════════════════
    #  Phase 4 — #24: 만료 예약은 부하 계산에서 빠진다(§18-1). sweep 을 크게 잡아
    #  만료 예약이 「회수되지 않은 채」 테이블에 남게 만든다 — valid_reservations
    #  의 시간 필터가 없으면(뮤턴트) 이 유령이 배정을 비틀어 3차 로그인이 다른
    #  서버로 간다. sweep 이 지워 주는 정상 경로에서는 이 필터가 최대 sweep 주기
    #  만큼의 창에서만 일하므로, 그 창을 크게 벌려야 결정적으로 관측된다.
    # ═══════════════════════════════════════════════════════════════════════
    Start-Sleep -Milliseconds 700
    $sess4Home = New-SessionHome 'sess4' '16' '60000' '800' '60000'
    $sess4Log = Join-Path $sess4Home 'logs\server.log'
    $sess4 = Start-ServerProcess $sessionExe $sess4Home '--seconds 10'
    $procs.Add($sess4)
    if ($null -eq (Wait-LogMatch $sess4Log 'session server up' 8000)) {
        throw 'phase 4 session.exe 기동 로그가 없다'
    }
    $fakeU = Connect-Tcp $SessionS2sPort
    $fsU = $fakeU.GetStream()
    Send-S2sFrame $fsU $MsgRegister 1 (New-RegisterBody $VerOk 7401 10 0 '127.0.0.1')
    $rU = Read-S2sFrame $fsU 3000
    $fakeV = Connect-Tcp $SessionS2sPort
    $fsV = $fakeV.GetStream()
    Send-S2sFrame $fsV $MsgRegister 1 (New-RegisterBody $VerOk 7402 100 0 '127.0.0.2')
    $rV = Read-S2sFrame $fsV 3000
    $ports24 = @()
    foreach ($pid24 in 5001..5002) {
        $c24 = Connect-Tcp $SessionClientPort
        $s24 = $c24.GetStream()
        Send-ClientFrame $s24 $CliSessionLoginReq (New-LoginBody $pid24)
        $a24 = Read-ClientFrame $s24 3000
        $ports24 += $(if ($null -ne $a24 -and $a24.MsgId -eq $CliSessionLoginAck) { (ConvertFrom-LoginAckBody $a24.Body).Port } else { -1 })
        $c24.Close()
    }
    Start-Sleep -Milliseconds 1200      # expire 800ms 경과 — sweep(60000)이 없어 만료 예약이 잔존한다
    # 헬스(500ms×3) 리프레시 — 하트비트가 없으면 3차 로그인의 lazy 판정이 둘 다 orphan 시킨다
    Send-S2sFrame $fsU $MsgHeartbeat 50 ([byte[]](0, 0, 0, 0))
    Read-S2sFrame $fsU 2000 | Out-Null
    Send-S2sFrame $fsV $MsgHeartbeat 50 ([byte[]](0, 0, 0, 0))
    Read-S2sFrame $fsV 2000 | Out-Null
    $c24 = Connect-Tcp $SessionClientPort
    $s24 = $c24.GetStream()
    Send-ClientFrame $s24 $CliSessionLoginReq (New-LoginBody 5003)
    $a24 = Read-ClientFrame $s24 3000
    $ports24 += $(if ($null -ne $a24 -and $a24.MsgId -eq $CliSessionLoginAck) { (ConvertFrom-LoginAckBody $a24.Body).Port } else { -1 })
    $c24.Close()
    # 기대 — L1: 0/10 vs 0/100 동률 → server_id 최소(7401) · L2: 1/10 > 0/100 →
    # 7402 · L3: 두 예약이 다 만료됐으므로 다시 0 vs 0 동률 → 7401.
    # 시간 필터가 죽으면 L3 은 1/10 vs 1/100 → 7402 로 간다.
    Add-Result '#24' (($null -ne $rU) -and ($null -ne $rV) -and ($ports24[0] -eq 7401) -and ($ports24[1] -eq 7402) -and ($ports24[2] -eq 7401)) `
        ("배정 포트 수열=" + ($ports24 -join ',') + " (기대 7401,7402,7401 — 만료 예약이 부하에서 빠져야 3차가 동률 최소 id 로 돌아온다)")
    $fakeU.Close(); $fakeV.Close()
    $sess4.WaitForExit(15000) | Out-Null

    # ═══════════════════════════════════════════════════════════════════════
    #  Phase 5 — #25: 타임아웃이 pending 을 다 비워도 연결의 seq 공간은 보존된다.
    #  빈 LinkPending 을 지우면 next_seq 가 1 로 재시작해, 방금 타임아웃된 옛
    #  요청의 늦은 응답이 「같은 seq 의 새 요청」에 오매칭된다(router on_sweep
    #  주석이 막는 결함). 관측 축 둘 — ① 재발신 Reserve 의 seq 가 이어지는가(2)
    #  ② 옛 seq 의 늦은 거절이 무시되는가(rejected=0).
    # ═══════════════════════════════════════════════════════════════════════
    Start-Sleep -Milliseconds 700
    $sess5Home = New-SessionHome 'sess5' '16'
    $sess5Log = Join-Path $sess5Home 'logs\server.log'
    $sess5 = Start-ServerProcess $sessionExe $sess5Home '--seconds 10'
    $procs.Add($sess5)
    if ($null -eq (Wait-LogMatch $sess5Log 'session server up' 8000)) {
        throw 'phase 5 session.exe 기동 로그가 없다'
    }
    $fakeW = Connect-Tcp $SessionS2sPort
    $fsW = $fakeW.GetStream()
    Send-S2sFrame $fsW $MsgRegister 1 (New-RegisterBody $VerOk 7501 10 0 '127.0.0.1')
    $rW = Read-S2sFrame $fsW 3000
    $c25 = Connect-Tcp $SessionClientPort
    $s25 = $c25.GetStream()
    Send-ClientFrame $s25 $CliSessionLoginReq (New-LoginBody 6001)
    Read-ClientFrame $s25 3000 | Out-Null
    $rsvA = Read-S2sFrame $fsW 3000               # Reserve seq=1 — 일부러 응답하지 않는다
    Start-Sleep -Milliseconds 1200                # timeout(500)+sweep(500) — pending 이 비워진다
    Send-S2sFrame $fsW $MsgHeartbeat 60 ([byte[]](0, 0, 0, 0))
    Read-S2sFrame $fsW 2000 | Out-Null            # 헬스 리프레시(500ms×3 초과 방지)
    Send-ClientFrame $s25 $CliSessionLoginReq (New-LoginBody 6002)
    Read-ClientFrame $s25 3000 | Out-Null
    $c25.Close()
    $rsvB = Read-S2sFrame $fsW 3000               # 두 번째 Reserve — seq 는 2 여야 한다
    $seqCont = ($null -ne $rsvA) -and ($rsvA.MsgId -eq $MsgReserve) -and ($rsvA.Seq -eq 1) `
        -and ($null -ne $rsvB) -and ($rsvB.MsgId -eq $MsgReserve) -and ($rsvB.Seq -eq 2)
    # 옛 seq(1) 로 늦은 거절을 흘린다 — 새 요청에 오매칭되면 rejected 가 오른다.
    Send-S2sFrame $fsW $MsgReserveAck 1 ([byte[]](1))
    Start-Sleep -Milliseconds 400
    $fakeW.Close()
    $sess5.WaitForExit(15000) | Out-Null
    $s5Text = Read-LogText $sess5Log
    $p5S4 = [regex]::Match($s5Text, '\[SESS \] pending sent=(\d+) send_fail=(\d+) ack=(\d+) rejected=(\d+)')
    $p5Rej = if ($p5S4.Success) { [int]$p5S4.Groups[4].Value } else { -1 }
    Add-Result '#25' ($seqCont -and ($p5Rej -eq 0)) `
        ("재발신 Reserve seq=" + $(if ($rsvB) { $rsvB.Seq } else { '(없음)' }) + " (기대 2 — 빈 pending erase 뮤턴트면 1) · 늦은 옛 seq 거절의 오매칭 rejected=$p5Rej (기대 0)")

    # ═══════════════════════════════════════════════════════════════════════
    #  Phase 6a — 실물 Enter 기본 흐름·실패 5종·current 정밀 증감·PlayerEnter/
    #  Leave 발신 대칭 (항목 1·2·3·4·18·19). 마을 둘을 동시에
    #  띄운다 — F3(다른 마을 예약)이 실물 마을 둘을 요구하고, 두 마을의 load
    #  비교가 「current 가 0 아님」이 아니라 「정확히 그 순간값」을 재는 척도가
    #  된다: F2 는 절대 Enter 하지 않아 current 가 늘 0 인 비교 기준점이고, F1
    #  이 그 위로 올라가는 순간(로그인 라우팅이 F2 로 넘어가는 순간)이 곧
    #  "F1 의 current 가 정확히 늘었다"의 증거다.
    # ═══════════════════════════════════════════════════════════════════════
    Start-Sleep -Milliseconds 700
    $sess6aHome = New-SessionHome 'sess6a' '16'
    $sess6aLog = Join-Path $sess6aHome 'logs\server.log'
    # K1~K9(Kick) 이 이 페이즈에 들어오며 폴링(K2·K4 최대 5s 씩)과 고정 관찰
    # (K7 의 재발신 억제 확인 2s)이 추가돼, 원래 --seconds 12 로는 세션이
    # K9 완료 전에 스스로 멈출 수 있다 — 40/45(F1·F2) 로 넉넉히 늘렸다(안 쟀다
    # — 여유를 크게 잡아 재는 대신 회피).
    $sess6a = Start-ServerProcess $sessionExe $sess6aHome '--seconds 40'
    $procs.Add($sess6a)
    if ($null -eq (Wait-LogMatch $sess6aLog 'session server up' 8000)) {
        throw 'phase 6a session.exe 기동 로그가 없다'
    }

    # F1 — capacity 5 · heartbeat 200ms(current 갱신을 빨리 관측하려 축소).
    # F2 — 기본 capacity(4096) — 절대 Enter 하지 않아 load 비교의 고정 기준이 된다.
    $vil6aF1Home = New-VillageHome 'vil6aF1' '9010' '5' '200'
    $vil6aF1Log = Join-Path $vil6aF1Home 'logs\server.log'
    $ck6aF1 = Get-LogLength $sess6aLog
    $vil6aF1 = Start-ServerProcess $villageExe $vil6aF1Home '--seconds 45'
    $procs.Add($vil6aF1)
    $m6aF1 = Wait-LogMatch $sess6aLog 'register -> server_id=(\d+)' 8000 $ck6aF1
    if ($null -eq $m6aF1) { throw 'phase 6a: F1 등록 실패' }

    $vil6aF2Home = New-VillageHome 'vil6aF2' '9020'
    $vil6aF2Log = Join-Path $vil6aF2Home 'logs\server.log'
    $ck6aF2 = Get-LogLength $sess6aLog
    $vil6aF2 = Start-ServerProcess $villageExe $vil6aF2Home '--seconds 45'
    $procs.Add($vil6aF2)
    $m6aF2 = Wait-LogMatch $sess6aLog 'register -> server_id=(\d+)' 8000 $ck6aF2
    if ($null -eq $m6aF2) { throw 'phase 6a: F2 등록 실패' }

    # ── P6-1 — Login(A1) → 둘 다 load=0 이라 낮은 server_id(F1) 가 이긴다 →
    #    실물 Enter → kJoinZoneReq 스모크. P6-2-F4 — 같은 연결에서 재Enter. ──
    $cliA1 = Connect-Tcp $SessionClientPort
    $stA1 = $cliA1.GetStream()
    Send-ClientFrame $stA1 $CliSessionLoginReq (New-LoginBody 61001)
    $ackA1 = Read-ClientFrame $stA1 3000
    $loginA1 = if ($null -ne $ackA1 -and $ackA1.MsgId -eq $CliSessionLoginAck) { ConvertFrom-LoginAckBody $ackA1.Body } else { $null }
    $cliA1.Close()
    $loginA1ToF1 = ($null -ne $loginA1) -and ($loginA1.Result -eq 0) -and ($loginA1.Port -eq 9010)

    $vclA1 = Connect-Tcp 9010
    $vstA1 = $vclA1.GetStream()
    Send-ClientFrame $vstA1 $VilEnterReq (New-LoginBody 61001)
    $enterA1 = Read-ClientFrame $vstA1 3000
    $enterA1Ok = ($null -ne $enterA1) -and ($enterA1.MsgId -eq $VilEnterAck)
    $enterA1Body = if ($enterA1Ok) { ConvertFrom-EnterAckBody $enterA1.Body } else { $null }

    Send-ClientFrame $vstA1 $VilEnterReq (New-LoginBody 61001)
    $enterA1b = Read-ClientFrame $vstA1 3000
    $enterA1bOk = ($null -ne $enterA1b) -and ($enterA1b.MsgId -eq $VilEnterAck)
    $enterA1bBody = if ($enterA1bOk) { ConvertFrom-EnterAckBody $enterA1b.Body } else { $null }

    Send-ClientFrame $vstA1 $VilJoinZoneReq (New-JoinZoneBody 61)
    $joinA1 = Read-ClientFrame $vstA1 3000
    $joinA1Ok = ($null -ne $joinA1) -and ($joinA1.MsgId -eq $VilJoinZoneAck)
    $joinA1Body = if ($joinA1Ok) { ConvertFrom-JoinZoneAckBody $joinA1.Body } else { $null }

    Add-Result 'P6-1(Enter 기본)' ($loginA1ToF1 -and $enterA1Ok -and ($enterA1Body.Result -eq $ResultOk) `
        -and ($enterA1Body.PlayerId -eq 61001) -and $joinA1Ok -and ($joinA1Body.ZoneId -eq 61)) `
        ("Login->F1(9010)=$loginA1ToF1 · Enter result=$(if ($enterA1Body) { $enterA1Body.Result } else { '-' })(기대 0) player_id=$(if ($enterA1Body) { $enterA1Body.PlayerId } else { '-' }) · JoinZoneAck zone=$(if ($joinA1Body) { $joinA1Body.ZoneId } else { '-' })(스모크)")
    Add-Result 'P6-2-F4(재Enter)' ($enterA1bOk -and ($enterA1bBody.Result -eq $ResultInvalidArg) -and $joinA1Ok) `
        ("Enter 재시도 result=$(if ($enterA1bBody) { $enterA1bBody.Result } else { '-' })(기대 3=InvalidArg) · 연결 유지=$enterA1bOk(뒤이은 JoinZone 성공으로 재확인)")

    # 헬스 리프레시 겸 current 반영 대기 — F1 heartbeat_ms=200 이니 500ms 면 최소 2회 왕복한다.
    Start-Sleep -Milliseconds 500

    $cliA2 = Connect-Tcp $SessionClientPort
    $stA2 = $cliA2.GetStream()
    Send-ClientFrame $stA2 $CliSessionLoginReq (New-LoginBody 61002)
    $ackA2 = Read-ClientFrame $stA2 3000
    $loginA2 = if ($null -ne $ackA2 -and $ackA2.MsgId -eq $CliSessionLoginAck) { ConvertFrom-LoginAckBody $ackA2.Body } else { $null }
    $cliA2.Close()
    Add-Result 'P6-3a(current 증가)' (($null -ne $loginA2) -and ($loginA2.Result -eq 0) -and ($loginA2.Port -eq 9020)) `
        ("Enter 1건 이후 재로그인 -> port=$(if ($loginA2) { $loginA2.Port } else { '-' })(기대 9020=F2 — F1 load 가 0 보다 커져야 F2 로 넘어간다 — current 가 정확히 1 로 올랐다는 뜻)")

    # ── P6-18-3 — Enter 를 안 보낸 세션의 종료는 PlayerEnter/PlayerLeave 를
    #    안 남긴다. F2 에 연결만 하고 아무 것도 보내지 않은 채 바로 닫는다.
    #    (예전엔 kLoginReq 직행 경로로 이 시나리오를 만들었으나 그 요청이
    #    와이어에서 사라졌다 — 이 계약이 요구하는 것은 "Enter 를 안 거친
    #    세션"이지 kLoginReq 자체가 아니므로, 연결만 하고 Enter 를 안
    #    보내는 세션으로 그대로 옮긴다. 항목 수는 그대로다.) ──
    $vclDirect = Connect-Tcp 9020
    $directOk = $vclDirect.Connected
    $vclDirect.Close()
    Add-Result 'P6-18-3(Enter 없이 종료)' $directOk `
        ("F2 연결=$directOk (Enter 미발신) — 이 연결 종료는 아래 접속 테이블 카운터에 무변화로 확인")

    # ── P6-2-F1 — 예약 없이 Enter(F2 에 직접, 세션 로그인 생략) → InvalidArg · 연결 유지 ──
    $vclF1 = Connect-Tcp 9020
    $vstF1 = $vclF1.GetStream()
    Send-ClientFrame $vstF1 $VilEnterReq (New-LoginBody 62001)
    $enterF1 = Read-ClientFrame $vstF1 3000
    $enterF1Ok = ($null -ne $enterF1) -and ($enterF1.MsgId -eq $VilEnterAck)
    $enterF1Body = if ($enterF1Ok) { ConvertFrom-EnterAckBody $enterF1.Body } else { $null }
    $aliveF1 = Test-VillageAlivePing $vstF1 3000
    $vclF1.Close()
    Add-Result 'P6-2-F1(예약 없음)' ($enterF1Ok -and ($enterF1Body.Result -eq $ResultInvalidArg) -and $aliveF1) `
        ("예약 없이 Enter -> result=$(if ($enterF1Body) { $enterF1Body.Result } else { '-' })(기대 3) · 연결 유지=$aliveF1")

    # ── P6-2-F2 — 만료된 예약으로 Enter. 세션 Login 으로 정식 예약을 발급받은
    #    뒤 만료(sess6a 의 reserve.expire_ms=2000, New-SessionHome 기본값)를 기다린다 ──
    $cliF2 = Connect-Tcp $SessionClientPort
    $stF2 = $cliF2.GetStream()
    Send-ClientFrame $stF2 $CliSessionLoginReq (New-LoginBody 62002)
    $ackF2 = Read-ClientFrame $stF2 3000
    $loginF2 = if ($null -ne $ackF2 -and $ackF2.MsgId -eq $CliSessionLoginAck) { ConvertFrom-LoginAckBody $ackF2.Body } else { $null }
    $cliF2.Close()
    $enterF2Ok = $false; $enterF2Body = $null; $aliveF2 = $false
    if ($null -ne $loginF2 -and $loginF2.Result -eq 0) {
        Start-Sleep -Milliseconds 2300      # expire_ms(2000) 초과 대기
        $vclF2 = Connect-Tcp $loginF2.Port
        $vstF2 = $vclF2.GetStream()
        Send-ClientFrame $vstF2 $VilEnterReq (New-LoginBody 62002)
        $enterF2 = Read-ClientFrame $vstF2 3000
        $enterF2Ok = ($null -ne $enterF2) -and ($enterF2.MsgId -eq $VilEnterAck)
        $enterF2Body = if ($enterF2Ok) { ConvertFrom-EnterAckBody $enterF2.Body } else { $null }
        $aliveF2 = Test-VillageAlivePing $vstF2 3000
        $vclF2.Close()
    }
    Add-Result 'P6-2-F2(만료 예약)' (($null -ne $loginF2) -and ($loginF2.Result -eq 0) -and $enterF2Ok `
        -and ($enterF2Body.Result -eq $ResultInvalidArg) -and $aliveF2) `
        ("만료 대기 후 Enter -> result=$(if ($enterF2Body) { $enterF2Body.Result } else { '-' })(기대 3) · 연결 유지=$aliveF2")

    # ── P6-2-F3 — 다른 마을의 예약으로 Enter. 세션 Login 이 준 마을이 아닌
    #    쪽에 붙는다(어느 쪽이 배정되든 동적으로 반대쪽을 고른다). ──
    $cliF3 = Connect-Tcp $SessionClientPort
    $stF3 = $cliF3.GetStream()
    Send-ClientFrame $stF3 $CliSessionLoginReq (New-LoginBody 62003)
    $ackF3 = Read-ClientFrame $stF3 3000
    $loginF3 = if ($null -ne $ackF3 -and $ackF3.MsgId -eq $CliSessionLoginAck) { ConvertFrom-LoginAckBody $ackF3.Body } else { $null }
    $cliF3.Close()
    $enterF3Ok = $false; $enterF3Body = $null; $aliveF3 = $false; $otherPort = 0
    if ($null -ne $loginF3 -and $loginF3.Result -eq 0) {
        $otherPort = if ($loginF3.Port -eq 9010) { 9020 } else { 9010 }
        $vclF3 = Connect-Tcp $otherPort
        $vstF3 = $vclF3.GetStream()
        Send-ClientFrame $vstF3 $VilEnterReq (New-LoginBody 62003)
        $enterF3 = Read-ClientFrame $vstF3 3000
        $enterF3Ok = ($null -ne $enterF3) -and ($enterF3.MsgId -eq $VilEnterAck)
        $enterF3Body = if ($enterF3Ok) { ConvertFrom-EnterAckBody $enterF3.Body } else { $null }
        $aliveF3 = Test-VillageAlivePing $vstF3 3000
        $vclF3.Close()
    }
    Add-Result 'P6-2-F3(다른 마을 예약)' (($null -ne $loginF3) -and ($loginF3.Result -eq 0) -and ($otherPort -ne $loginF3.Port) `
        -and $enterF3Ok -and ($enterF3Body.Result -eq $ResultInvalidArg) -and $aliveF3) `
        ("예약 마을=" + $(if ($loginF3) { $loginF3.Port } else { '-' }) + " · 실제 Enter=$otherPort -> result=$(if ($enterF3Body) { $enterF3Body.Result } else { '-' })(기대 3) · 연결 유지=$aliveF3")

    # ── P6-2-F5 — Enter body 7B 위반 → 절단 + 로그(#15/#19/#20 과 같은 패턴) ──
    $ck6aF5 = Get-LogLength $vil6aF1Log
    $vclF5 = Connect-Tcp 9010
    $vstF5 = $vclF5.GetStream()
    Send-ClientFrame $vstF5 $VilEnterReq ([byte[]](1, 2, 3, 4, 5, 6, 7))
    $deadF5 = Read-ClientFrame $vstF5 3000
    $mF5 = Wait-LogMatch $vil6aF1Log 'enter body=7 \(want 8\)' 3000 $ck6aF5
    $vclF5.Close()
    Add-Result 'P6-2-F5(body 위반)' (($null -eq $deadF5) -and ($null -ne $mF5)) `
        ("Enter 7B 주입 -> 절단=" + ($null -eq $deadF5) + " · bad_body 로그=" + ($null -ne $mF5))

    # ── P6-3b·P6-18-2 — A1 연결 종료(PlayerLeave 발신) → 재로그인이 F1 로
    #    돌아오는지(current 가 정확히 0 으로 내려갔다는 뜻). ──
    $vclA1.Close()
    Start-Sleep -Milliseconds 600      # PlayerLeave 왕복 + heartbeat 반영 시간
    $cliA3 = Connect-Tcp $SessionClientPort
    $stA3 = $cliA3.GetStream()
    Send-ClientFrame $stA3 $CliSessionLoginReq (New-LoginBody 61003)
    $ackA3 = Read-ClientFrame $stA3 3000
    $loginA3 = if ($null -ne $ackA3 -and $ackA3.MsgId -eq $CliSessionLoginAck) { ConvertFrom-LoginAckBody $ackA3.Body } else { $null }
    $cliA3.Close()
    Add-Result 'P6-3b(current 감소)' (($null -ne $loginA3) -and ($loginA3.Result -eq 0) -and ($loginA3.Port -eq 9010)) `
        ("PlayerLeave 이후 재로그인 -> port=$(if ($loginA3) { $loginA3.Port } else { '-' })(기대 9010=F1 — current 가 정확히 0 으로 내려가 동률 tie-break 로 최소 id 로 돌아온다)")

    # ── K1~K6b — 동기 Kick. 항목7-b 원형(entry.enter() 의 중복 거부)은
    #    이제 재현 불가하다 — 두 번째 Login 이 세션 서버 단계에서 먼저
    #    Busy 로 거절돼(ADR-024 결정 2) village 의 entry.enter() 까지
    #    안 온다. 그 village 쪽 분기(중복 Enter 거절)는 여전히 살아 있는
    #    코드고 커버리지가 필요해 gate.ps1 N7 로 옮겼다(session_router.cpp
    #    의 handle_session_login 이 이 시나리오의 실제 무대다). ──
    $cliG1 = Connect-Tcp $SessionClientPort
    $stG1 = $cliG1.GetStream()
    Send-ClientFrame $stG1 $CliSessionLoginReq (New-LoginBody 63001)
    $ackG1 = Read-ClientFrame $stG1 3000
    $loginG1 = if ($null -ne $ackG1 -and $ackG1.MsgId -eq $CliSessionLoginAck) { ConvertFrom-LoginAckBody $ackG1.Body } else { $null }
    $cliG1.Close()
    $enterG1Ok = $false; $enterG1Body = $null; $vclG1 = $null; $vilG1Log = $vil6aF1Log
    if ($null -ne $loginG1 -and $loginG1.Result -eq 0) {
        if ($loginG1.Port -eq 9020) { $vilG1Log = $vil6aF2Log }
        $vclG1 = Connect-Tcp $loginG1.Port
        $vstG1 = $vclG1.GetStream()
        Send-ClientFrame $vstG1 $VilEnterReq (New-LoginBody 63001)
        $entG1 = Read-ClientFrame $vstG1 3000
        $enterG1Ok = ($null -ne $entG1) -and ($entG1.MsgId -eq $VilEnterAck)
        $enterG1Body = if ($enterG1Ok) { ConvertFrom-EnterAckBody $entG1.Body } else { $null }
        # vclG1 은 열어 둔다 — Kick 이 이 소켓을 끊는 것 자체가 K3 의 관측
        # 대상이다(닫으면 스스로의 종료와 Kick 에 의한 종료를 못 가른다).
    }
    Add-Result 'K1(G1 Login+Enter)' ($enterG1Ok -and ($enterG1Body.Result -eq $ResultOk)) `
        ("Login->$(if ($loginG1) { $loginG1.Port } else { '-' }) · Enter result=$(if ($enterG1Body) { $enterG1Body.Result } else { '-' })(기대 0)")

    # K2 — G2 가 같은 player_id 로 로그인한다. Busy(2) 가 나올 때까지
    #    폴링한다(200ms×25=5s) — PlayerEnter 전파 창(U6)이 있어 초기 회차는
    #    result=0(정상 배정)일 수 있다. 그 회차가 만드는 예약은
    #    add_reservation 의 덮어쓰기(§8-2 멱등)로만 쌓여 무해하다.
    $k2Busy = $false; $k2ZeroCount = 0; $k2LastResult = -1
    for ($i = 0; $i -lt 25; $i++) {
        $cliG2p = Connect-Tcp $SessionClientPort
        $stG2p = $cliG2p.GetStream()
        Send-ClientFrame $stG2p $CliSessionLoginReq (New-LoginBody 63001)
        $ackG2p = Read-ClientFrame $stG2p 3000
        $loginG2p = if ($null -ne $ackG2p -and $ackG2p.MsgId -eq $CliSessionLoginAck) { ConvertFrom-LoginAckBody $ackG2p.Body } else { $null }
        $cliG2p.Close()
        $k2LastResult = if ($loginG2p) { $loginG2p.Result } else { -1 }
        if ($k2LastResult -eq 0) { $k2ZeroCount++ }
        if ($k2LastResult -eq 2) { $k2Busy = $true; break }
        Start-Sleep -Milliseconds 200
    }
    Add-Result 'K2(G2 Busy 관측)' $k2Busy `
        ("폴링 결과 result=$k2LastResult(기대 2=Busy) · 그 전 result=0 관측 $k2ZeroCount 회(전파 지연 실측)")

    # K3 — Kick 이 G1 소켓을 실제로 끊는지. gate.ps1 Wait-ConnectionOutcome ·
    #    s2s.ps1 ⑫/⑰ 와 같은 Poll(SelectRead)+Available==0 기법이다 —
    #    "아직 응답이 없다"와 "닫혔다"가 블로킹 read 하나로는 안 갈린다.
    $k3Closed = $false
    if ($null -ne $vclG1) {
        $ksw3 = [System.Diagnostics.Stopwatch]::StartNew()
        while ($ksw3.ElapsedMilliseconds -lt 5000) {
            if ($vclG1.Client.Poll(50000, [System.Net.Sockets.SelectMode]::SelectRead)) {
                if ($vclG1.Client.Available -eq 0) { $k3Closed = $true; break }
                $drain = [byte[]]::new(256)
                try { $vstG1.Read($drain, 0, $drain.Length) | Out-Null } catch {}
            }
        }
    }
    if ($k3Closed) {
        Add-Result 'K3(G1 강제 종료)' $true '5000ms 안에 G1 소켓 종료 관측(Kick 수행)'
    } else {
        $sawKickLog = (Read-LogText $vilG1Log) -match '\[KICK \]'
        Add-Result 'K3(G1 강제 종료)' $false `
            ("5000ms 안에 소켓 종료 관측 실패 — village [KICK ] 로그 유무=$sawKickLog(마을 미수행 vs 소켓 관측 실패 구분)")
    }

    # K4 — G2 가 재시도하면 Ok 를 받는다(옛 세션 정리가 끝난 뒤). 간격
    #    500ms·상한 10회로 폴링하고, 몇 회째에 Ok 인지를 남긴다(U4 실측값
    #    — §16-4 정정문에 옮겨 적을 숫자).
    $k4Ok = $false; $k4Attempt = 0; $loginG2 = $null
    for ($i = 1; $i -le 10; $i++) {
        Start-Sleep -Milliseconds 500
        $cliG2 = Connect-Tcp $SessionClientPort
        $stG2 = $cliG2.GetStream()
        Send-ClientFrame $stG2 $CliSessionLoginReq (New-LoginBody 63001)
        $ackG2 = Read-ClientFrame $stG2 3000
        $loginG2 = if ($null -ne $ackG2 -and $ackG2.MsgId -eq $CliSessionLoginAck) { ConvertFrom-LoginAckBody $ackG2.Body } else { $null }
        $cliG2.Close()
        if ($null -ne $loginG2 -and $loginG2.Result -eq 0) { $k4Ok = $true; $k4Attempt = $i; break }
    }
    Add-Result 'K4(G2 재시도 Ok)' $k4Ok `
        ("재시도 $k4Attempt 회째 Ok(기대 10 이내) — result=$(if ($loginG2) { $loginG2.Result } else { '-' })")

    # K5 — G2 가 재입장에 성공한다(entered_ 소유권이 G1 에서 G2 로 넘어갔다).
    $enterG2Ok = $false; $enterG2Body = $null; $vclG2 = $null
    if ($k4Ok) {
        $vclG2 = Connect-Tcp $loginG2.Port
        $vstG2 = $vclG2.GetStream()
        Send-ClientFrame $vstG2 $VilEnterReq (New-LoginBody 63001)
        $entG2 = Read-ClientFrame $vstG2 3000
        $enterG2Ok = ($null -ne $entG2) -and ($entG2.MsgId -eq $VilEnterAck)
        $enterG2Body = if ($enterG2Ok) { ConvertFrom-EnterAckBody $entG2.Body } else { $null }
    }
    Add-Result 'K5(G2 재입장)' ($enterG2Ok -and ($enterG2Body.Result -eq $ResultOk)) `
        ("Enter result=$(if ($enterG2Body) { $enterG2Body.Result } else { '-' })(기대 0 — 재입장 성립)")

    # K6 — G2 로 게임 요청 1회가 정상 응답한다(킥 잔재가 새 세션을 오염 안
    #    한다는 뜻) — P6-1 과 같은 kJoinZoneReq 스모크를 재사용한다.
    $joinG2Ok = $false
    if ($enterG2Ok) {
        Send-ClientFrame $vstG2 $VilJoinZoneReq (New-JoinZoneBody 61)
        $joinG2 = Read-ClientFrame $vstG2 3000
        $joinG2Ok = ($null -ne $joinG2) -and ($joinG2.MsgId -eq $VilJoinZoneAck)
        $vclG2.Close()
    }
    Add-Result 'K6(G2 게임 요청)' $joinG2Ok 'kJoinZoneReq -> kJoinZoneAck 정상 응답'

    # G1 소켓 정리 — K3 판정 직후가 아니라 K4~K6 판정이 전부 끝난 뒤 닫는다.
    #   K3 안에서 닫으면(하네스가 스스로 닫아) 서버가 실제로 안 끊었어도
    #   K4~K6 이 "G1 이 여전히 살아 있다"를 못 보고 통과해버린다(자기-마스킹).
    if ($null -ne $vclG1) { try { $vclG1.Close() } catch {} }

    # K6b — village 로그의 [KICK ] 줄이 정확히 1건이다 — K2 폴링이 Busy 를
    #    여러 번 받는 동안 village 쪽에 두 번째 Kick 이 안 나갔다는 결과만
    #    본다. 억제(already_pending)가 실제로 그 경로를 탔는지는 이 관찰
    #    하나로는 못 가른다 — pending 이 없어서 애초에 재발신할 계기가 없었을
    #    수도 있다. 억제 자체의 실동작(2차 요청이 와도 억제가 막는다)은
    #    K10 의 mid-check 가 직접 관찰한다.
    $kickLines = @([regex]::Matches((Read-LogText $vilG1Log), '\[KICK \]'))
    Add-Result 'K6b(Kick 로그 1건 — 중복 없음 · 억제 자체는 K10 mid-check 가 검증)' ($kickLines.Count -eq 1) `
        ("village 로그 [KICK ] 건수=$($kickLines.Count)(기대 1 — 재발신 없음)")

    # ── K7~K9 — 가짜 마을(Phase 1 의 S2S 등록 기구 재사용, §7 플랜). 세
    #    항목 다 Wait-S2sFrame(harness_common.ps1)으로 Kick 수신을 기다린다
    #    — 이 파일 로컬 Read-S2sFrameFiltered 는 Heartbeat 자동 ack 가 없어
    #    새 의존을 안 만든다는 규율의 적용이다. Kick 은 세션 서버가 먼저
    #    보내는 요청(unsolicited)이라 Wait-S2sFrame 을 이런 용법으로 쓰는
    #    첫 사례다 — 이 세 항목의 통과 자체가 그 실측 확인이다.
    #    실물 F1·F2 와 이름이 겹치지 않게 가짜 마을은 ghostVil/orphanVil 로 부른다.

    # K7 — 유령 정리(NotFound): 가짜 마을이 실입장 없이 PlayerEnter 를 보내
    #    접속 테이블에 유령을 만든다 → 그 player_id 로 로그인하면 Busy +
    #    가짜 마을이 Kick 을 받는다 → NotFound 로 답하면 그 유령이 지워져
    #    재로그인이 성공한다 — 그사이 두 번째 Kick 은 안 온다(재발신 억제).
    $ghostVil = Connect-Tcp $SessionS2sPort
    $fsGhost = $ghostVil.GetStream()
    Send-S2sFrame $fsGhost $MsgRegister 1 (New-RegisterBody $VerOk 7701 10 0 '127.0.0.3')
    Read-S2sFrame $fsGhost 3000 | Out-Null
    Send-S2sFrame $fsGhost $MsgPlayerEnter 0 (New-LoginBody 63101)   # 유령 — 실입장 없음
    Start-Sleep -Milliseconds 300

    $cliK7 = Connect-Tcp $SessionClientPort
    $stK7 = $cliK7.GetStream()
    Send-ClientFrame $stK7 $CliSessionLoginReq (New-LoginBody 63101)
    $ackK7 = Read-ClientFrame $stK7 3000
    $loginK7 = if ($null -ne $ackK7 -and $ackK7.MsgId -eq $CliSessionLoginAck) { ConvertFrom-LoginAckBody $ackK7.Body } else { $null }
    $cliK7.Close()
    $k7Busy = ($null -ne $loginK7) -and ($loginK7.Result -eq 2)

    $skippedK7 = New-Object System.Collections.Generic.List[int]
    $kickK7 = Wait-S2sFrame $fsGhost $MsgKick 5000 $skippedK7
    $k7GotKick = $null -ne $kickK7
    $kickK7Body = if ($k7GotKick) { ConvertFrom-KickBody $kickK7.Body } else { $null }
    if ($k7GotKick) {
        Send-S2sFrame $fsGhost $MsgKickAck $kickK7.Seq ([byte[]]@(1))   # NotFound
    }

    $k7RetryOk = $false; $k7RetryAttempt = 0
    for ($i = 1; $i -le 10; $i++) {
        Start-Sleep -Milliseconds 500
        $cliK7r = Connect-Tcp $SessionClientPort
        $stK7r = $cliK7r.GetStream()
        Send-ClientFrame $stK7r $CliSessionLoginReq (New-LoginBody 63101)
        $ackK7r = Read-ClientFrame $stK7r 3000
        $loginK7r = if ($null -ne $ackK7r -and $ackK7r.MsgId -eq $CliSessionLoginAck) { ConvertFrom-LoginAckBody $ackK7r.Body } else { $null }
        $cliK7r.Close()
        if ($null -ne $loginK7r -and $loginK7r.Result -eq 0) { $k7RetryOk = $true; $k7RetryAttempt = $i; break }
    }
    # 재발신 억제 관찰 — 유령이 지워진 뒤에도 이 가짜 마을은 여전히
    #   registered 상태(capacity=10·current=0, 부하가 가벼워 재시도 로그인이
    #   실제로 이 마을에 새로 배정돼 Reserve 를 받을 수 있다 — 실측으로
    #   확인했다). 그래서 "아무 프레임도 안 온다"가 아니라 "Kick 이 안
    #   온다"를 정확히 본다.
    $k7NoSecondKick = $true
    $k7ObserveSw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($k7ObserveSw.ElapsedMilliseconds -lt 2000) {
        $remain = 2000 - [int]$k7ObserveSw.ElapsedMilliseconds
        if ($remain -le 0) { break }
        $extra = Read-S2sFrame $fsGhost ([Math]::Min(300, [Math]::Max(50, $remain)))
        if ($null -ne $extra -and $extra.MsgId -eq $MsgKick) { $k7NoSecondKick = $false; break }
    }
    $ghostVil.Close()
    Add-Result 'K7(유령 정리)' ($k7Busy -and $k7GotKick -and ($null -ne $kickK7Body) -and ($kickK7Body.PlayerId -eq 63101) -and $k7RetryOk -and $k7NoSecondKick) `
        ("Busy=$k7Busy · Kick 수신 player=$(if ($kickK7Body) { $kickK7Body.PlayerId } else { '-' }) · 재시도 $k7RetryAttempt 회째 Ok=$k7RetryOk · 두번째 Kick 없음=$k7NoSecondKick")

    # K8 — orphan 허용: 가짜 마을이 PlayerEnter 뒤 S2S 소켓을 끊는다(orphan
    #    전이) — Kick 을 보낼 대상이 없으므로(§5-3) 즉시 다른 서버(실물
    #    village)로 배정해 허용한다. 배정이 가짜 쪽(7702)으로 안 갔다는
    #    확인은 LoginAck 의 port 로 한다(실물 F1=9010/F2=9020 중 하나).
    $orphanVil = Connect-Tcp $SessionS2sPort
    $fsOrphan = $orphanVil.GetStream()
    Send-S2sFrame $fsOrphan $MsgRegister 1 (New-RegisterBody $VerOk 7702 10 0 '127.0.0.4')
    Read-S2sFrame $fsOrphan 3000 | Out-Null
    Send-S2sFrame $fsOrphan $MsgPlayerEnter 0 (New-LoginBody 63102)
    Start-Sleep -Milliseconds 300
    $orphanVil.Close()      # orphan 전이 — 재연결 없음
    Start-Sleep -Milliseconds 300

    $cliK8 = Connect-Tcp $SessionClientPort
    $stK8 = $cliK8.GetStream()
    Send-ClientFrame $stK8 $CliSessionLoginReq (New-LoginBody 63102)
    $ackK8 = Read-ClientFrame $stK8 3000
    $loginK8 = if ($null -ne $ackK8 -and $ackK8.MsgId -eq $CliSessionLoginAck) { ConvertFrom-LoginAckBody $ackK8.Body } else { $null }
    $cliK8.Close()
    Add-Result 'K8(orphan 허용)' (($null -ne $loginK8) -and ($loginK8.Result -eq 0) -and (($loginK8.Port -eq 9010) -or ($loginK8.Port -eq 9020))) `
        ("즉시 Ok(다른 서버=실물 village 배정) port=$(if ($loginK8) { $loginK8.Port } else { '-' })(기대 9010/9020 중 하나 — orphan 마을(7702) 아님. 그 마을은 재연결이 없어 Kick 프레임 0건이다)")

    # K9 — 미매칭 seq 무시: 요청받지 않은 KickAck 를 서버가 받아도 끊거나
    #    죽지 않는다(§8-4) — 직후 정상 Login 1회가 성공하는 것으로 생존을
    #    확인한다.
    $unmatchedVil = Connect-Tcp $SessionS2sPort
    $fsUnmatched = $unmatchedVil.GetStream()
    Send-S2sFrame $fsUnmatched $MsgRegister 1 (New-RegisterBody $VerOk 7703 10 0 '127.0.0.5')
    Read-S2sFrame $fsUnmatched 3000 | Out-Null
    Send-S2sFrame $fsUnmatched $MsgKickAck 0xDEAD ([byte[]]@(0))   # 요청 안 한 응답 — 미매칭 seq
    Start-Sleep -Milliseconds 300

    $cliK9 = Connect-Tcp $SessionClientPort
    $stK9 = $cliK9.GetStream()
    Send-ClientFrame $stK9 $CliSessionLoginReq (New-LoginBody 63103)
    $ackK9 = Read-ClientFrame $stK9 3000
    $loginK9 = if ($null -ne $ackK9 -and $ackK9.MsgId -eq $CliSessionLoginAck) { ConvertFrom-LoginAckBody $ackK9.Body } else { $null }
    $cliK9.Close()
    $unmatchedVil.Close()
    Add-Result 'K9(미매칭 seq 무시)' (($null -ne $loginK9) -and ($loginK9.Result -eq 0)) `
        ("미매칭 KickAck(seq=0xDEAD) 주입 뒤 정상 Login result=$(if ($loginK9) { $loginK9.Result } else { '-' })(기대 0 — 서버 생존)")

    # K10 — 타임아웃이 pending 을 지우면 재발신 억제가 풀리는지(kick_timeout
    #   경로 실증). 세션 스크래치는 이 페이즈 전체가 New-SessionHome 의
    #   기본값(request_timeout=500ms·sweep=500ms — 이미 이 페이즈 전체가
    #   그 값으로 돈다)이라 손 볼 필요가 없었다 — 1500ms 대기면 sweep 이
    #   최소 2주기 돈다(500ms 주기 · 최악의 경우도 요청 시각과 주기 시작이
    #   어긋나 봐야 1주기 손해). 정확한 발화 시각은 안 쟀다 — 그 대신 "2차
    #   Kick 이 실제로 온다" 자체가 타임아웃이 이미 지났다는 증거로 선다.
    $f3Vil = Connect-Tcp $SessionS2sPort
    $fsF3 = $f3Vil.GetStream()
    Send-S2sFrame $fsF3 $MsgRegister 1 (New-RegisterBody $VerOk 7704 10 0 '127.0.0.6')
    Read-S2sFrame $fsF3 3000 | Out-Null
    Send-S2sFrame $fsF3 $MsgPlayerEnter 0 (New-LoginBody 63105)
    Start-Sleep -Milliseconds 300

    $cliK10a = Connect-Tcp $SessionClientPort
    $stK10a = $cliK10a.GetStream()
    Send-ClientFrame $stK10a $CliSessionLoginReq (New-LoginBody 63105)
    $ackK10a = Read-ClientFrame $stK10a 3000
    $loginK10a = if ($null -ne $ackK10a -and $ackK10a.MsgId -eq $CliSessionLoginAck) { ConvertFrom-LoginAckBody $ackK10a.Body } else { $null }
    $cliK10a.Close()
    $k10Busy1 = ($null -ne $loginK10a) -and ($loginK10a.Result -eq 2)

    $skippedK10a = New-Object System.Collections.Generic.List[int]
    $kickK10a = Wait-S2sFrame $fsF3 $MsgKick 5000 $skippedK10a
    $k10GotFirstKick = $null -ne $kickK10a
    # 응답하지 않는다 — pending 이 타임아웃으로 회수돼야 한다.

    # mid-check — 타임아웃 전(1차 pending 이 아직 살아 있는 동안) 억제가
    #   실제로 도는지 직접 본다. MUT-B(억제 삭제)가 놓쳤던 자리다 — 억제가
    #   무력화돼도 K10 의 기존 두 단언(1차/타임아웃 뒤 2차)만으로는 안 걸린다
    #   (둘 다 "Kick 이 온다"만 보고 "억제 중엔 안 온다"를 안 본다). 재로그인은
    #   같은 유령이 그대로 있으니 Busy 를 다시 받아야 하고, 그 뒤 2초 동안은
    #   kick_by_seq 에 이미 pending 이 있으므로 재Kick 이 오면 안 된다.
    $cliK10mid = Connect-Tcp $SessionClientPort
    $stK10mid = $cliK10mid.GetStream()
    Send-ClientFrame $stK10mid $CliSessionLoginReq (New-LoginBody 63105)
    $ackK10mid = Read-ClientFrame $stK10mid 3000
    $loginK10mid = if ($null -ne $ackK10mid -and $ackK10mid.MsgId -eq $CliSessionLoginAck) { ConvertFrom-LoginAckBody $ackK10mid.Body } else { $null }
    $cliK10mid.Close()
    $k10BusyMid = ($null -ne $loginK10mid) -and ($loginK10mid.Result -eq 2)

    $k10NoMidKick = $true
    $k10MidSw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($k10MidSw.ElapsedMilliseconds -lt 2000) {
        $remain = 2000 - [int]$k10MidSw.ElapsedMilliseconds
        if ($remain -le 0) { break }
        $extraMid = Read-S2sFrame $fsF3 ([Math]::Min(300, [Math]::Max(50, $remain)))
        if ($null -ne $extraMid -and $extraMid.MsgId -eq $MsgKick) { $k10NoMidKick = $false; break }
    }

    Start-Sleep -Milliseconds 1500

    $cliK10b = Connect-Tcp $SessionClientPort
    $stK10b = $cliK10b.GetStream()
    Send-ClientFrame $stK10b $CliSessionLoginReq (New-LoginBody 63105)
    $ackK10b = Read-ClientFrame $stK10b 3000
    $loginK10b = if ($null -ne $ackK10b -and $ackK10b.MsgId -eq $CliSessionLoginAck) { ConvertFrom-LoginAckBody $ackK10b.Body } else { $null }
    $cliK10b.Close()
    $k10Busy2 = ($null -ne $loginK10b) -and ($loginK10b.Result -eq 2)

    $skippedK10b = New-Object System.Collections.Generic.List[int]
    $kickK10b = Wait-S2sFrame $fsF3 $MsgKick 5000 $skippedK10b
    $k10GotSecondKick = $null -ne $kickK10b
    $f3Vil.Close()   # 2차 Kick 도 응답 안 함 — 이 절단이 link_down 경로로 정리한다(K11 항등식 참조)

    Add-Result 'K10(타임아웃→재발신 재개)' ($k10Busy1 -and $k10GotFirstKick -and $k10BusyMid -and $k10NoMidKick -and $k10Busy2 -and $k10GotSecondKick) `
        ("1차 Busy=$k10Busy1 Kick수신=$k10GotFirstKick · 타임아웃 전 mid Busy=$k10BusyMid 재Kick없음=$k10NoMidKick(억제 실동작 직접 관찰) · 타임아웃 대기 뒤 2차 Busy=$k10Busy2 Kick수신=$k10GotSecondKick(기대 전부 True — pending 타임아웃이 억제를 풀었다)")

    # K12 — 링크 다운이 pending Kick 을 정리하는지(kick_link_down 경로 실증).
    #   Kick 수신을 확인한 즉시 소켓을 끊어 무응답인 채로 링크가 죽는다.
    $f4Vil = Connect-Tcp $SessionS2sPort
    $fsF4 = $f4Vil.GetStream()
    Send-S2sFrame $fsF4 $MsgRegister 1 (New-RegisterBody $VerOk 7705 10 0 '127.0.0.8')
    Read-S2sFrame $fsF4 3000 | Out-Null
    Send-S2sFrame $fsF4 $MsgPlayerEnter 0 (New-LoginBody 63104)
    Start-Sleep -Milliseconds 300

    $cliK12 = Connect-Tcp $SessionClientPort
    $stK12 = $cliK12.GetStream()
    Send-ClientFrame $stK12 $CliSessionLoginReq (New-LoginBody 63104)
    $ackK12 = Read-ClientFrame $stK12 3000
    $loginK12 = if ($null -ne $ackK12 -and $ackK12.MsgId -eq $CliSessionLoginAck) { ConvertFrom-LoginAckBody $ackK12.Body } else { $null }
    $cliK12.Close()
    $k12Busy = ($null -ne $loginK12) -and ($loginK12.Result -eq 2)

    $skippedK12 = New-Object System.Collections.Generic.List[int]
    $kickK12 = Wait-S2sFrame $fsF4 $MsgKick 5000 $skippedK12
    $k12GotKick = $null -ne $kickK12
    $f4Vil.Close()      # 무응답인 채 즉시 절단 — 링크 다운

    Start-Sleep -Milliseconds 500      # on_s2s_gone 처리 시간

    $cliK12b = Connect-Tcp $SessionClientPort
    $stK12b = $cliK12b.GetStream()
    Send-ClientFrame $stK12b $CliSessionLoginReq (New-LoginBody 63106)
    $ackK12b = Read-ClientFrame $stK12b 3000
    $loginK12b = if ($null -ne $ackK12b -and $ackK12b.MsgId -eq $CliSessionLoginAck) { ConvertFrom-LoginAckBody $ackK12b.Body } else { $null }
    $cliK12b.Close()
    Add-Result 'K12(링크다운 정리)' ($k12Busy -and $k12GotKick -and ($null -ne $loginK12b) -and ($loginK12b.Result -ne 2)) `
        ("Busy=$k12Busy Kick수신=$k12GotKick · 링크 절단 뒤 무관 로그인(63106) result=$(if ($loginK12b) { $loginK12b.Result } else { '-' })(기대 0/1 — Busy(2) 아님 = 서버 생존)")

    $sess6a.WaitForExit(50000) | Out-Null
    if (-not $sess6a.HasExited) { $sess6a.Kill() }
    $vil6aF1.WaitForExit(3000) | Out-Null
    if (-not $vil6aF1.HasExited) { $vil6aF1.Kill() }
    $vil6aF2.WaitForExit(3000) | Out-Null
    if (-not $vil6aF2.HasExited) { $vil6aF2.Kill() }

    $s6aText = Read-LogText $sess6aLog
    $s6aS3 = [regex]::Match($s6aText, '\[SESS \] link hold acquire=(\d+) release=(\d+) \(unreg=\d+ gone=\d+ stale_revive=\d+ stale_sweep=\d+\) stop_leftover=(\d+)')
    $s6aS5 = [regex]::Match($s6aText, '\[SESS \] connections added=(\d+) removed=(\d+) fullsync_replaced=(\d+) remain=(\d+)')
    if ($s6aS3.Success -and $s6aS5.Success) {
        $s6aAcq = [int]$s6aS3.Groups[1].Value; $s6aRel = [int]$s6aS3.Groups[2].Value
        $s6aLeftover = [int]$s6aS3.Groups[3].Value
        # P6-19 — 접속 테이블이 늘어도 세션 수명 경로(홀드)는 이 작업에서 신설
        # 되지 않았다. acquire==release·leftover=0 이 유지돼야 한다(§1 "세션
        # 수명 경로 신설 0건"의 관측 지점).
        Add-Result 'P6-19(홀드 수지)' (($s6aAcq -eq $s6aRel) -and ($s6aLeftover -eq 0)) `
            ("[SESS ] 홀드 acquire=$s6aAcq release=$s6aRel leftover=$s6aLeftover (세션 수명 경로 신설 0건 확인)")

        # P6-18-1·P6-18-2 — K10·K12 가 이 카운터를 또 만진다(각자 유령
        # PlayerEnter 1건씩). 손으로 다시 셌다 —
        #   added 7   = A1 Enter(P6-1) · K1 G1 Enter · K5 G2 재입장 ·
        #               K7 유령(63101) · K8 유령(63102) · K10 유령(63105) ·
        #               K12 유령(63104)
        #   removed 7 = A1 Leave(P6-3b) · K3 G1 Kick · K6 종료 시 G2 Close ·
        #               K7 KickAck(NotFound) → player_left · K8/K10/K12 유령
        #               셋 다 orphan 유예(2000ms) 초과 sweep 의
        #               drop_server_connections 로 정리된다(K10·K12 는
        #               kick_link_down 이 pending Kick 만 지우고 connections_
        #               항목 자체는 orphan sweep 이 지운다 — 별개 경로다.
        #               K12~자연 종료 대기 사이 2s 는 확실히 지난다)
        #   remain 0  = 일곱 다 정리돼 아무도 안 남는다.
        # F1~F5·Enter 없이 종료(P6-18-3)·kJoinZoneReq 스모크 자체(응답만 볼 뿐
        # 접속 테이블은 안 건드림)·K2/K4/K9 의 순수 Login(Enter 없음)은 이
        # 카운터를 안 만진다. fullsync_replaced 는 그대로 >= 로 본다 — K7·
        # K8·K9·K10·K12 는 가짜 S2S 클라이언트라 FullSync 를 안 보낸다.
        $s6aAdded = [int]$s6aS5.Groups[1].Value; $s6aRemoved = [int]$s6aS5.Groups[2].Value
        $s6aReplaced = [int]$s6aS5.Groups[3].Value; $s6aRemain = [int]$s6aS5.Groups[4].Value
        Add-Result 'P6-18-1(PlayerEnter 관통)' ($s6aAdded -eq 7) `
            ("[SESS ] connections added=$s6aAdded (기대 7=A1·K1 G1·K5 G2·K7/K8/K10/K12 유령 4건)")
        Add-Result 'P6-18-2(PlayerLeave 관통)' ($s6aRemoved -eq 7) `
            ("[SESS ] connections removed=$s6aRemoved (기대 7=A1 Leave·K3 Kick·K6 G2 Close·K7 NotFound 정리·K8/K10/K12 orphan sweep 3건)")
        Add-Result 'P6-4(관통 집계)' (($s6aReplaced -ge 2) -and ($s6aRemain -eq 0)) `
            ("[SESS ] fullsync_replaced=$s6aReplaced(기대 >=2 — F1·F2 최초 등록) remain=$s6aRemain(기대 0 — 일곱 다 정리됐다)")

        # K11 — [SESS ] kick 종료 요약의 항등식 + 이번 런의 기대값을 손으로
        #   계산해 함께 건다(P6-19 의 홀드 수지 패턴 재사용). kick_sent 는
        #   busy 갈래에서 pending 이 없을 때만 오르고, login_busy 는 busy
        #   갈래마다(억제돼도) 오른다 — K10 의 mid-check(1차 Kick 이 아직
        #   pending 인 채로 오는 재로그인)가 바로 그 억제 발동 사례라 sent
        #   와 login_busy 가 여기서 처음으로 갈라진다(둘 다 같던 이전 값에서
        #   +0/+1).
        #     sent 5      = K1~K6b(G1/G2 중복, 1) · K7 유령(1) ·
        #                   K10 1차+2차(2, mid 는 억제돼 안 셈) · K12 유령(1)
        #     acked 1     = K1~K6b — G1 이 실제로 Kicked(0) 응답
        #     not_found 1 = K7 — 유령이 KickAck(NotFound=1) 로 응답
        #     timeout 1   = K10 1차 — 무응답인 채 request_timeout 경과
        #     link_down 2 = K10 2차(F3Vil.Close 로 즉시 절단) · K12(F4Vil.Close)
        #     stop 0      = 전부 위 넷으로 소진돼 종료 시점 잔존이 없다
        #     login_busy 6 = K1~K6b(1) · K7(1) · K10 1차+mid+2차(3, mid 는
        #                   억제돼도 busy 판정 자체는 그대로 센다) · K12(1)
        $s6aKick = [regex]::Match($s6aText,
            '\[SESS \] kick sent=(\d+) acked=(\d+) not_found=(\d+) timeout=(\d+) link_down=(\d+) stop=(\d+) \| login_busy=(\d+)')
        if ($s6aKick.Success) {
            $kSent = [int]$s6aKick.Groups[1].Value; $kAcked = [int]$s6aKick.Groups[2].Value
            $kNotFound = [int]$s6aKick.Groups[3].Value; $kTimeout = [int]$s6aKick.Groups[4].Value
            $kLinkDown = [int]$s6aKick.Groups[5].Value; $kStop = [int]$s6aKick.Groups[6].Value
            $kBusy = [int]$s6aKick.Groups[7].Value
            $kIdentity = $kSent -eq ($kAcked + $kNotFound + $kTimeout + $kLinkDown + $kStop)
            Add-Result 'K11(종료 항등식)' ($kIdentity -and ($kSent -eq 5) -and ($kAcked -eq 1) `
                -and ($kNotFound -eq 1) -and ($kTimeout -eq 1) -and ($kLinkDown -eq 2) -and ($kStop -eq 0) -and ($kBusy -eq 6)) `
                ("[SESS ] kick sent=$kSent=$kAcked+$kNotFound+$kTimeout+$kLinkDown+$kStop(항등식 $kIdentity) login_busy=$kBusy " +
                 "(기대 sent=5·acked=1·not_found=1·timeout=1·link_down=2·stop=0·login_busy=6)")
        } else {
            Add-Result 'K11(종료 항등식)' $false 'phase 6a 로그에서 [SESS ] kick 요약 줄을 못 찾았다'
        }
    } else {
        Add-Result 'P6-19(홀드 수지)' $false 'phase 6a [SESS ] 요약을 못 찾았다'
        Add-Result 'P6-18-1(PlayerEnter 관통)' $false 'phase 6a [SESS ] 요약을 못 찾았다'
        Add-Result 'P6-18-2(PlayerLeave 관통)' $false 'phase 6a [SESS ] 요약을 못 찾았다'
        Add-Result 'P6-4(관통 집계)' $false 'phase 6a [SESS ] 요약을 못 찾았다'
        Add-Result 'K11(종료 항등식)' $false 'phase 6a [SESS ] 요약을 못 찾았다'
    }

    # ═══════════════════════════════════════════════════════════════════════
    #  Phase 6d — 가짜 마을(S2S 프레임 직접 조작), 항목 9·10·16·17②. 실물
    #  마을 타이밍에 안 기댄다 — 전부 하네스가 프레임을 직접 쥐고 만드는
    #  결정적 시나리오다(#26·#27 과 같은 기법).
    # ═══════════════════════════════════════════════════════════════════════
    Start-Sleep -Milliseconds 700
    $sess6dHome = New-SessionHome 'sess6d' '16'
    $sess6dLog = Join-Path $sess6dHome 'logs\server.log'
    $sess6d = Start-ServerProcess $sessionExe $sess6dHome '--seconds 12'
    $procs.Add($sess6d)
    if ($null -eq (Wait-LogMatch $sess6dLog 'session server up' 8000)) {
        throw 'phase 6d session.exe 기동 로그가 없다'
    }

    # ── 항목9 — FullSync count 와 실제 본문 크기가 어긋나면 절단 + 로그 ──────
    # chunk_idx=0·chunk_total=1·count=2(6B) 라고 선언해 놓고 실제로는 id 1개
    # (8B)만 실어 총 14B 를 보낸다 — decode_full_sync 의 일치 검사(6+count*8
    # != len)만이 이걸 잡는다.
    $ck9d = Get-LogLength $sess6dLog
    $fake9d = Connect-Tcp $SessionS2sPort
    $fs9d = $fake9d.GetStream()
    $fsBad = New-Object System.Collections.Generic.List[byte]
    $fsBad.AddRange([byte[]](0, 0, 0, 1, 0, 2))
    $fsBad.AddRange([byte[]](0, 0, 0, 0, 0, 0, 0x27, 0x11))
    Send-S2sFrame $fs9d $MsgFullSync 0 $fsBad.ToArray()
    $dead9d = Read-S2sFrame $fs9d 3000
    $m9d = Wait-LogMatch $sess6dLog 's2s full-sync body=14 \(want 6\)' 3000 $ck9d
    $fake9d.Close()
    Add-Result 'P6-9(FullSync 프레임)' (($null -eq $dead9d) -and ($null -ne $m9d)) `
        ("count·본문 불일치 FullSync(14B) 주입 -> 절단=" + ($null -eq $dead9d) + " · bad_body 로그=" + ($null -ne $m9d))

    # ── 항목10 — PlayerEnter/PlayerLeave 고정 길이(8B) 위반 → 각각 절단 + 로그 ──
    $ck10d = Get-LogLength $sess6dLog
    $fake10a = Connect-Tcp $SessionS2sPort
    $fs10a = $fake10a.GetStream()
    Send-S2sFrame $fs10a $MsgPlayerEnter 0 ([byte[]](1, 2, 3, 4, 5, 6, 7))
    $dead10a = Read-S2sFrame $fs10a 3000
    $m10a = Wait-LogMatch $sess6dLog 's2s player-enter body=7 \(want 8\)' 3000 $ck10d
    $fake10a.Close()
    $fake10b = Connect-Tcp $SessionS2sPort
    $fs10b = $fake10b.GetStream()
    Send-S2sFrame $fs10b $MsgPlayerLeave 0 ([byte[]](1, 2, 3, 4, 5, 6, 7))
    $dead10b = Read-S2sFrame $fs10b 3000
    $m10b = Wait-LogMatch $sess6dLog 's2s player-leave body=7 \(want 8\)' 3000 $ck10d
    $fake10b.Close()
    Add-Result 'P6-10(PlayerEnter/Leave 위반)' (($null -eq $dead10a) -and ($null -ne $m10a) -and ($null -eq $dead10b) -and ($null -ne $m10b)) `
        ("enter 7B: 절단=" + ($null -eq $dead10a) + " 로그=" + ($null -ne $m10a) + " · leave 7B: 절단=" + ($null -eq $dead10b) + " 로그=" + ($null -ne $m10b))

    # ── 항목16 — 끊긴 동안의 유실이 재연결로 복구되는가. player_id 단위로
    #    단언한다(개수 상쇄 함정 회피 — 항목16 경고). ──
    #    ① 등록 → PlayerEnter(B) — "이미 들어와 있던 자"를 흉내낸다.
    #    ② 연결을 끊는다 — 그동안(가정상) B 는 나가고 A 가 들어왔지만 링크가
    #       없어 알림이 하나도 못 나간다 — 그래서 여기서 아무 프레임도 안 보낸다.
    #    ③ 같은 host:port 로 재연결(유예 안이라 server_id 부활) → 그 순간의
    #       실제 전량(A 만)을 FullSync(chunk_idx=0)로 보낸다 — 실물 마을이
    #       재등록마다 하는 일(§5-2)을 하네스가 그대로 흉내낸다.
    $fake16a = Connect-Tcp $SessionS2sPort
    $fs16a = $fake16a.GetStream()
    Send-S2sFrame $fs16a $MsgRegister 1 (New-RegisterBody $VerOk 7601 10 0 '127.0.0.1')
    Read-S2sFrame $fs16a 3000 | Out-Null
    Send-S2sFrame $fs16a $MsgPlayerEnter 0 (New-LoginBody 81002)     # B
    Start-Sleep -Milliseconds 200
    $fake16a.Close()      # 링크 끊김 — orphan(유예 안), 접속 테이블은 안 건드려진다
    Start-Sleep -Milliseconds 300
    $fake16b = Connect-Tcp $SessionS2sPort
    $fs16b = $fake16b.GetStream()
    Send-S2sFrame $fs16b $MsgRegister 1 (New-RegisterBody $VerOk 7601 10 0 '127.0.0.1')   # 같은 host:port → 부활
    $r16b = Read-S2sFrame $fs16b 3000
    $fsRecover = New-Object System.Collections.Generic.List[byte]
    $fsRecover.AddRange([byte[]](0, 0, 0, 1, 0, 1))
    $fsRecover.AddRange([byte[]](0, 0, 0, 0, 0, 0, 0x27, 0xD1))      # A = 81001
    Send-S2sFrame $fs16b $MsgFullSync 0 $fsRecover.ToArray()
    Start-Sleep -Milliseconds 300
    Add-Result 'P6-16(재연결 유실 복구)' ($null -ne $r16b) `
        ("재연결 부활=" + ($null -ne $r16b) + " · FullSync(전량=A 만)로 재동기 — B(끊긴 동안 나간 자) 는 지워지고 A(끊긴 동안 들어온 자) 만 남아야 한다(집계는 뒤 [SESS ] 로 재확인)")

    # ── 항목17② — Unregister 유예 창 안에서 링크가 끊겼다 재연결되면, 새
    #    연결은 미등록 상태이므로 유예가 적용되지 않고 정상 절단 경로를 탄다.
    #    유예는 net_session_id 로 걸리는데, 재연결은 새 net_session_id 라
    #    적용될 수 없다는 것이 이 항목의 핵심이다. ──
    $fake17a = Connect-Tcp $SessionS2sPort
    $fs17a = $fake17a.GetStream()
    Send-S2sFrame $fs17a $MsgRegister 1 (New-RegisterBody $VerOk 7602 10 0 '127.0.0.2')
    Read-S2sFrame $fs17a 3000 | Out-Null
    Send-S2sFrame $fs17a $MsgUnregister 2 ([byte[]]::new(0))
    $u17a = Read-S2sFrame $fs17a 3000
    $fake17a.Close()      # 유예(800ms) 안 — 하지만 이 net_session_id 자체가 곧 사라진다
    $fake17b = Connect-Tcp $SessionS2sPort      # "재연결" — 실제로는 새 net_session_id
    $fs17b = $fake17b.GetStream()
    $ck17b = Get-LogLength $sess6dLog
    Send-S2sFrame $fs17b $MsgHeartbeat 1 ([byte[]](0, 0, 0, 0))      # Register 없이 바로 heartbeat
    $dead17b = Read-S2sFrame $fs17b 3000
    $m17b = Wait-LogMatch $sess6dLog 'heartbeat 미등록 연결 — 끊는다' 3000 $ck17b
    $fake17b.Close()
    Add-Result 'P6-17-2(유예는 재연결에 안 붙는다)' (($null -ne $u17a) -and ($null -eq $dead17b) -and ($null -ne $m17b)) `
        ("Unregister 유예(800ms) 안이지만 새 연결(새 net_session_id)의 미등록 heartbeat → 절단=" + ($null -eq $dead17b) + " · 정상 절단 로그=" + ($null -ne $m17b) + " (유예가 net_session_id 단위라 새 연결엔 안 붙는다)")

    # fake16b 는 닫지 않는다 — 닫으면 orphan 유예(2000ms) 초과 뒤 sweep 이
    # drop_server_connections 를 한 번 더 돌려 removed 항이 이 항목의 명시
    # 조작분과 섞인다(#27 과 같은 함정, 실측으로 확인).
    $sess6d.WaitForExit(20000) | Out-Null
    if (-not $sess6d.HasExited) { $sess6d.Kill() }
    $s6dText = Read-LogText $sess6dLog
    $s6dS5 = [regex]::Match($s6dText, '\[SESS \] connections added=(\d+) removed=(\d+) fullsync_replaced=(\d+) remain=(\d+)')
    if ($s6dS5.Success) {
        # 이 페이즈에서 접속 테이블을 만지는 시나리오는 항목16 뿐이다(항목9·10 은
        # decode 실패로 끊기고, 항목17② 는 Register 자체를 안 한다). added=2(B
        # 최초+A 재동기) · removed=1(B, FullSync 클리어가 지운다) · remain=1(A) —
        # 개수만 보면(순변화 +1) A 가 안 들어오고 B 가 안 빠져도 통과하는 함정이
        # 있어(항목16 경고) 세 값을 각각 정확히 건다.
        $s6dAdded = [int]$s6dS5.Groups[1].Value; $s6dRemoved = [int]$s6dS5.Groups[2].Value
        $s6dRemain = [int]$s6dS5.Groups[4].Value
        Add-Result 'P6-16(집계)' (($s6dAdded -eq 2) -and ($s6dRemoved -eq 1) -and ($s6dRemain -eq 1)) `
            ("[SESS ] connections added=$s6dAdded(기대 2=B 최초+A 재동기) removed=$s6dRemoved(기대 1=B 클리어) remain=$s6dRemain(기대 1=A 만 남음)")
    } else {
        Add-Result 'P6-16(집계)' $false 'phase 6d [SESS ] 요약을 못 찾았다'
    }

    # ═══════════════════════════════════════════════════════════════════════
    #  Phase 6c — FullSync 청크 경계(항목8, 실물 마을 재등록으로 관통) +
    #  FullSync 중간 끊김(항목15, 가짜 마을 — 설계가 명시한 대체 기법).
    #  fullsync_chunk_max=3 으로 축소해 510명을 실제로 안 태우고 같은 분기를
    #  잰다(§2 클램프 규칙, ADR-019 결정 9 의 축소 기법과 같은 방식).
    # ═══════════════════════════════════════════════════════════════════════
    Start-Sleep -Milliseconds 700
    $sessC1Home = New-SessionHome 'sessC1' '16'
    $sessC1Log = Join-Path $sessC1Home 'logs\server.log'
    $sessC1 = Start-ServerProcess $sessionExe $sessC1Home '--seconds 15'
    $procs.Add($sessC1)
    if ($null -eq (Wait-LogMatch $sessC1Log 'session server up' 8000)) {
        throw 'phase 6c: sessC1 기동 로그가 없다'
    }
    $vilCHome = New-VillageHome 'vilC' '9030' '10' '500' '10000' '30000' '3'
    $vilCLog = Join-Path $vilCHome 'logs\server.log'
    $ckC1 = Get-LogLength $sessC1Log
    $vilC = Start-ServerProcess $villageExe $vilCHome '--seconds 40'
    $procs.Add($vilC)
    if ($null -eq (Wait-LogMatch $sessC1Log 'register -> server_id=(\d+)' 8000 $ckC1)) {
        throw 'phase 6c: vilC 최초 등록 실패'
    }

    # 실물 Enter 3건(90001~90003) — session Login → 실물 village Enter. 연결은
    # 열어 둔다(닫으면 PlayerLeave 가 나가 current 가 줄어 청크 수를 흔든다).
    $vilCConns = New-Object System.Collections.Generic.List[object]
    foreach ($pid6c in 90001..90003) {
        $cli6c = Connect-Tcp $SessionClientPort
        $st6c = $cli6c.GetStream()
        Send-ClientFrame $st6c $CliSessionLoginReq (New-LoginBody $pid6c)
        Read-ClientFrame $st6c 3000 | Out-Null
        $cli6c.Close()
        $vcl6c = Connect-Tcp 9030
        $vst6c = $vcl6c.GetStream()
        Send-ClientFrame $vst6c $VilEnterReq (New-LoginBody $pid6c)
        Read-ClientFrame $vst6c 3000 | Out-Null
        $vilCConns.Add($vcl6c)
    }

    # sessC1 을 내리고 sessC2 를 새로 띄운다 — vilC 의 S2S 커넥터가 재연결하며
    # 재등록하고, 그 즉시(§5-2) 3명 전량을 실은 FullSync 를 새로 보낸다.
    # chunk_max=3·인원=3 이면 한 청크(chunk_total=1)에 다 담긴다.
    $sessC1.Kill()
    $sessC1.WaitForExit(5000) | Out-Null
    Start-Sleep -Milliseconds 700
    $sessC2Home = New-SessionHome 'sessC2' '16'
    $sessC2Log = Join-Path $sessC2Home 'logs\server.log'
    $sessC2 = Start-ServerProcess $sessionExe $sessC2Home '--seconds 15'
    $procs.Add($sessC2)
    if ($null -eq (Wait-LogMatch $sessC2Log 'session server up' 8000)) {
        throw 'phase 6c: sessC2 기동 로그가 없다'
    }
    $m8a = Wait-LogMatch $sessC2Log 'full-sync server_id=\d+ chunk=(\d+)/(\d+) count=(\d+)' 8000
    $chunk8aOk = ($null -ne $m8a) -and ([int]$m8a.Groups[2].Value -eq 1) -and ([int]$m8a.Groups[3].Value -eq 3)
    Add-Result 'P6-8a(청크=1)' $chunk8aOk `
        ("3명 재등록 FullSync -> " + $(if ($m8a) { $m8a.Value } else { '(관측 안 됨)' }) + " (기대 chunk_total=1 count=3)")

    # 4번째(N+1) 를 넣고 sessC3 로 다시 재등록시킨다 — chunk_max=3·인원=4 면
    # 두 청크(1·2)로 나뉘고 마지막 청크의 count 는 1(4-3)이어야 한다.
    $cli6c4 = Connect-Tcp $SessionClientPort
    $st6c4 = $cli6c4.GetStream()
    Send-ClientFrame $st6c4 $CliSessionLoginReq (New-LoginBody 90004)
    Read-ClientFrame $st6c4 3000 | Out-Null
    $cli6c4.Close()
    $vcl6c4 = Connect-Tcp 9030
    $vst6c4 = $vcl6c4.GetStream()
    Send-ClientFrame $vst6c4 $VilEnterReq (New-LoginBody 90004)
    Read-ClientFrame $vst6c4 3000 | Out-Null
    $vilCConns.Add($vcl6c4)

    $sessC2.Kill()
    $sessC2.WaitForExit(5000) | Out-Null
    Start-Sleep -Milliseconds 700
    $sessC3Home = New-SessionHome 'sessC3' '16'
    $sessC3Log = Join-Path $sessC3Home 'logs\server.log'
    $sessC3 = Start-ServerProcess $sessionExe $sessC3Home '--seconds 15'
    $procs.Add($sessC3)
    if ($null -eq (Wait-LogMatch $sessC3Log 'session server up' 8000)) {
        throw 'phase 6c: sessC3 기동 로그가 없다'
    }
    # 체크포인트를 "session server up" 로그 확인 뒤에 뜨는 방식으로 잡지 않는다
    # — vilC 의 재연결은 세션이 포트를 여는 순간부터 곧장 시도되고 실측
    # 24ms 만에 성공한 적이 있다(폴링 100ms 간격보다 좁다). sessC3Log 는 이
    # 인스턴스 전용 새 파일이라 오프셋 0(기본값)이 항상 안전하다.
    $m8b1 = Wait-LogMatch $sessC3Log 'full-sync server_id=\d+ chunk=1/2 count=3' 8000
    $m8b2 = Wait-LogMatch $sessC3Log 'full-sync server_id=\d+ chunk=2/2 count=1' 3000
    Add-Result 'P6-8b(청크=2)' (($null -ne $m8b1) -and ($null -ne $m8b2)) `
        ("4명 재등록 FullSync -> 1번째=" + ($null -ne $m8b1) + "(chunk=1/2 count=3) · 2번째=" + ($null -ne $m8b2) + "(chunk=2/2 count=1)")

    foreach ($c in $vilCConns) { try { $c.Close() } catch {} }
    $sessC3.Kill(); $sessC3.WaitForExit(5000) | Out-Null
    $vilC.Kill(); $vilC.WaitForExit(3000) | Out-Null

    # ── 항목15 — FullSync 중간 끊김. 설계가 명시한 대체 기법대로 하네스가 가짜
    #    마을이 되어 첫 청크만 보내고 스스로 소켓을 닫는다(타이밍 불필요 —
    #    #26·#27·P6-16 과 같은 결정적 기법). ──
    Start-Sleep -Milliseconds 700
    $sess15Home = New-SessionHome 'sess15' '16'
    $sess15Log = Join-Path $sess15Home 'logs\server.log'
    $sess15 = Start-ServerProcess $sessionExe $sess15Home '--seconds 12'
    $procs.Add($sess15)
    if ($null -eq (Wait-LogMatch $sess15Log 'session server up' 8000)) {
        throw 'phase 6c(항목15) session.exe 기동 로그가 없다'
    }
    $fake15a = Connect-Tcp $SessionS2sPort
    $fs15a = $fake15a.GetStream()
    Send-S2sFrame $fs15a $MsgRegister 1 (New-RegisterBody $VerOk 7701 10 0 '127.0.0.1')
    Read-S2sFrame $fs15a 3000 | Out-Null
    # 청크 2개 중 첫 번째만 보낸다(chunk_idx=0·chunk_total=2·count=2) — 두 번째
    # 청크는 영원히 안 온다(링크가 여기서 끊긴다는 뜻).
    $partial15 = New-Object System.Collections.Generic.List[byte]
    $partial15.AddRange([byte[]](0, 0, 0, 2, 0, 2))
    $partial15.AddRange([byte[]](0, 0, 0, 0, 0, 0, 0x30, 0x39))    # 12345
    $partial15.AddRange([byte[]](0, 0, 0, 0, 0, 0, 0x30, 0x3A))    # 12346
    Send-S2sFrame $fs15a $MsgFullSync 0 $partial15.ToArray()
    Start-Sleep -Milliseconds 200
    $fake15a.Close()      # "링크를 끊는다" — 두 번째 청크는 결코 안 온다
    Start-Sleep -Milliseconds 300
    $fake15b = Connect-Tcp $SessionS2sPort
    $fs15b = $fake15b.GetStream()
    Send-S2sFrame $fs15b $MsgRegister 1 (New-RegisterBody $VerOk 7701 10 0 '127.0.0.1')   # 같은 host:port → 부활
    Read-S2sFrame $fs15b 3000 | Out-Null
    # 재연결한 마을이 처음부터 다시 전량을 보낸다(실물 on_register_ack 의 계약
    # 그대로) — 이번엔 1개 id 뿐인 완전한 단일 청크.
    $fresh15 = New-Object System.Collections.Generic.List[byte]
    $fresh15.AddRange([byte[]](0, 0, 0, 1, 0, 1))
    $fresh15.AddRange([byte[]](0, 0, 0, 0, 0, 0, 0x4E, 0x21))      # 20001
    Send-S2sFrame $fs15b $MsgFullSync 0 $fresh15.ToArray()
    Start-Sleep -Milliseconds 300
    # fake15b 도 닫지 않는다 — P6-16 과 같은 이유(위 주석 참조). 닫으면 이
    # 항목이 결정적으로 만들려는 "부분 누적분 폐기" 증거가 이후의 orphan
    # 유예 초과 sweep 회수와 섞여 못 가른다(실측으로 확인 — 첫 시도에서
    # remain 이 0 으로 나와 이 오염을 직접 봤다).
    $sess15.WaitForExit(20000) | Out-Null
    if (-not $sess15.HasExited) { $sess15.Kill() }
    $s15Text = Read-LogText $sess15Log
    $s15S5 = [regex]::Match($s15Text, '\[SESS \] connections added=(\d+) removed=(\d+) fullsync_replaced=(\d+) remain=(\d+)')
    if ($s15S5.Success) {
        # 부분 누적분(12345·12346)이 그대로 남아 있었다면(뮤턴트 — chunk_idx==0
        # 판정이 죽거나 재연결이 이전 상태를 안 지운다면) remain 이 3(12345·
        # 12346·20001)이 된다. 정상이라면 두 번째 FullSync 의 chunk_idx==0 이
        # 부분 누적분을 통째로 비우고 20001 하나만 남는다 — added=3(부분
        # 2건+전량 1건) · removed=2(부분 2건이 재동기에 클리어됨) · remain=1.
        $s15Added = [int]$s15S5.Groups[1].Value; $s15Removed = [int]$s15S5.Groups[2].Value
        $s15Remain = [int]$s15S5.Groups[4].Value
        Add-Result 'P6-15(FullSync 중간 끊김)' (($s15Added -eq 3) -and ($s15Removed -eq 2) -and ($s15Remain -eq 1)) `
            ("[SESS ] connections added=$s15Added(기대 3) removed=$s15Removed(기대 2 — 부분 누적분 클리어) remain=$s15Remain(기대 1 — 재전송 전량으로 교체됨, 부분 누적 잔존 아님)")
    } else {
        Add-Result 'P6-15(FullSync 중간 끊김)' $false 'phase 6c(항목15) [SESS ] 요약을 못 찾았다'
    }

    # ═══════════════════════════════════════════════════════════════════════
    #  Phase 6e — 항목11. capacity=2 실물 마을을 정원까지 채우면 배정이
    #  그 마을을 피하는지(①) · 세션 재기동 직후 첫 Register.current 가 그
    #  순간의 실제 인원인지(②, 0 이 아닌지).
    # ═══════════════════════════════════════════════════════════════════════
    Start-Sleep -Milliseconds 700
    $sessE1Home = New-SessionHome 'sessE1' '16'
    $sessE1Log = Join-Path $sessE1Home 'logs\server.log'
    $sessE1 = Start-ServerProcess $sessionExe $sessE1Home '--seconds 15'
    $procs.Add($sessE1)
    if ($null -eq (Wait-LogMatch $sessE1Log 'session server up' 8000)) {
        throw 'phase 6e: sessE1 기동 로그가 없다'
    }
    $vilEHome = New-VillageHome 'vilE' '9040' '2' '200'
    $ckE1 = Get-LogLength $sessE1Log
    $vilE = Start-ServerProcess $villageExe $vilEHome '--seconds 25'
    $procs.Add($vilE)
    if ($null -eq (Wait-LogMatch $sessE1Log 'register -> server_id=(\d+)' 8000 $ckE1)) {
        throw 'phase 6e: vilE 등록 실패'
    }

    # 정원(2)까지 실물 Enter 시킨다 — 연결은 열어 둔다.
    $vilEConns = New-Object System.Collections.Generic.List[object]
    foreach ($pid6e in 91001..91002) {
        $cli6e = Connect-Tcp $SessionClientPort
        $st6e = $cli6e.GetStream()
        Send-ClientFrame $st6e $CliSessionLoginReq (New-LoginBody $pid6e)
        Read-ClientFrame $st6e 3000 | Out-Null
        $cli6e.Close()
        $vcl6e = Connect-Tcp 9040
        $vst6e = $vcl6e.GetStream()
        Send-ClientFrame $vst6e $VilEnterReq (New-LoginBody $pid6e)
        Read-ClientFrame $vst6e 3000 | Out-Null
        $vilEConns.Add($vcl6e)
    }
    Start-Sleep -Milliseconds 500      # heartbeat(200ms) 로 current=2 반영 대기

    # 비교 기준점 — capacity 100·current=0 인 가짜 마을. vilE 가 포화(2/2=1.0)면
    # 이 가짜(0/100=0)가 항상 이긴다 — vilE 를 "피한다"의 관측 지점이다.
    $fakeE = Connect-Tcp $SessionS2sPort
    $fsE = $fakeE.GetStream()
    Send-S2sFrame $fsE $MsgRegister 1 (New-RegisterBody $VerOk 9999 100 0 '127.0.0.3')
    Read-S2sFrame $fsE 3000 | Out-Null

    $cliE3 = Connect-Tcp $SessionClientPort
    $stE3 = $cliE3.GetStream()
    Send-ClientFrame $stE3 $CliSessionLoginReq (New-LoginBody 91003)
    $ackE3 = Read-ClientFrame $stE3 3000
    $loginE3 = if ($null -ne $ackE3 -and $ackE3.MsgId -eq $CliSessionLoginAck) { ConvertFrom-LoginAckBody $ackE3.Body } else { $null }
    $cliE3.Close()
    Add-Result 'P6-11a(포화 회피)' (($null -ne $loginE3) -and ($loginE3.Result -eq 0) -and ($loginE3.Port -ne 9040)) `
        ("정원 2 채운 뒤 3차 로그인 -> port=$(if ($loginE3) { $loginE3.Port } else { '-' })(기대 9040 아님 — current==capacity 라 회피, 가짜 비교 마을로 감)")
    $fakeE.Close()

    # ② 세션 재기동 직후 첫 Register.current 가 0 이 아니라 실제 인원(2)인지.
    $sessE1.Kill()
    $sessE1.WaitForExit(5000) | Out-Null
    Start-Sleep -Milliseconds 700
    $sessE2Home = New-SessionHome 'sessE2' '16'
    $sessE2Log = Join-Path $sessE2Home 'logs\server.log'
    $sessE2 = Start-ServerProcess $sessionExe $sessE2Home '--seconds 10'
    $procs.Add($sessE2)
    if ($null -eq (Wait-LogMatch $sessE2Log 'session server up' 8000)) {
        throw 'phase 6e: sessE2 기동 로그가 없다'
    }
    $mE2 = Wait-LogMatch $sessE2Log 'register -> server_id=\d+ 127\.0\.0\.1:9040 cap=2 cur=(\d+)' 8000
    Add-Result 'P6-11b(재연결 직후 current)' (($null -ne $mE2) -and ([int]$mE2.Groups[1].Value -eq 2)) `
        ("재기동 뒤 첫 Register 의 cur=" + $(if ($mE2) { $mE2.Groups[1].Value } else { '(관측 안 됨)' }) + " (기대 2 — 0 으로 리셋되지 않는다)")

    foreach ($c in $vilEConns) { try { $c.Close() } catch {} }
    $sessE2.Kill(); $sessE2.WaitForExit(5000) | Out-Null
    $vilE.Kill(); $vilE.WaitForExit(3000) | Out-Null

    # ═══════════════════════════════════════════════════════════════════════
    #  Phase 6f — 항목12. village 정상 종료 → Unregister 발신 → 세션이
    #  orphan 유예 없이 즉시 삭제하는지(§4 Step4 의 unregister_and_wait 계약).
    # ═══════════════════════════════════════════════════════════════════════
    Start-Sleep -Milliseconds 700
    $sessFHome = New-SessionHome 'sessF' '16'
    $sessFLog = Join-Path $sessFHome 'logs\server.log'
    $sessF = Start-ServerProcess $sessionExe $sessFHome '--seconds 12'
    $procs.Add($sessF)
    if ($null -eq (Wait-LogMatch $sessFLog 'session server up' 8000)) {
        throw 'phase 6f: sessF 기동 로그가 없다'
    }
    $vilFHome = New-VillageHome 'vilF' '9050'
    $ckF = Get-LogLength $sessFLog
    # village 를 짧게 띄워(3초) 스스로 정상 종료(--seconds 만료)하게 만든다 —
    # F10 의 "정상 종료도 기본은 Unregister 없는 끊김"과 달리, Step4 의
    # unregister_and_wait() 가 이 종료 경로에서 실제로 Unregister 를 보낸다.
    $vilF = Start-ServerProcess $villageExe $vilFHome '--seconds 3'
    $procs.Add($vilF)
    if ($null -eq (Wait-LogMatch $sessFLog 'register -> server_id=(\d+)' 8000 $ckF)) {
        throw 'phase 6f: vilF 등록 실패'
    }
    $vilF.WaitForExit(15000) | Out-Null
    $vilFOk = $vilF.HasExited -and ($vilF.ExitCode -eq 0)
    $mUnregF = Wait-LogMatch $sessFLog 's2s #\d+ unregister' 5000
    $mOrphanF = Wait-LogMatch $sessFLog '링크 다운 — orphan \(항목 유지\)' 500
    Add-Result 'P6-12(정상종료 Unregister)' ($vilFOk -and ($null -ne $mUnregF) -and ($null -eq $mOrphanF)) `
        ("village 정상 종료=$vilFOk(exit=$($vilF.ExitCode)) · unregister 로그=" + ($null -ne $mUnregF) + " · orphan 로그 없음(즉시 삭제)=" + ($null -eq $mOrphanF))

    $sessF.WaitForExit(20000) | Out-Null
    if (-not $sessF.HasExited) { $sessF.Kill() }
    $sFText = Read-LogText $sessFLog
    $sFS3 = [regex]::Match($sFText, '\[SESS \] link hold acquire=(\d+) release=(\d+) \(unreg=(\d+) gone=(\d+) stale_revive=\d+ stale_sweep=\d+\) stop_leftover=(\d+)')
    if ($sFS3.Success) {
        # unreg 갈래로 회수된 것이 이 항목의 증거다(orphan 을 거쳐 "gone" 으로
        # 늦게 회수되는 F10 의 기본 경로와 다른 갈래).
        $sFUnreg = [int]$sFS3.Groups[3].Value
        Add-Result 'P6-12(집계)' ($sFUnreg -ge 1) `
            ("[SESS ] link hold unreg=$sFUnreg (기대 >=1 — Unregister 갈래로 즉시 회수됐다는 뜻)")
    } else {
        Add-Result 'P6-12(집계)' $false 'phase 6f [SESS ] 요약을 못 찾았다'
    }

    # ═══════════════════════════════════════════════════════════════════════
    #  Phase 6h — 항목14. 예약 만료 스윕 자체 — 만료 「직전」과 「직후」
    #  둘 다. village 의 sweep_ms 를 축소해 스윕 스레드가 실제로 도는지와,
    #  아직 안 지난 예약을 잘못 지우지 않는지를 함께 잰다.
    #  ⚠️ 세션의 reserve.expire_ms(2000, New-SessionHome 기본값)와 마을의
    #  reserve_expire_ms 클램프(5000)를 같은 값도 배수도 아니게 잡았다 —
    #  §7 하네스 함정②. 실제 만료는 항상 더 작은 쪽(세션 2000ms)이 결정한다.
    # ═══════════════════════════════════════════════════════════════════════
    Start-Sleep -Milliseconds 700
    $sessHHome = New-SessionHome 'sessH' '16'
    $sessHLog = Join-Path $sessHHome 'logs\server.log'
    $sessH = Start-ServerProcess $sessionExe $sessHHome '--seconds 15'
    $procs.Add($sessH)
    if ($null -eq (Wait-LogMatch $sessHLog 'session server up' 8000)) {
        throw 'phase 6h: sessH 기동 로그가 없다'
    }
    $vilHHome = New-VillageHome 'vilH' '9060' '10' '500' '5000' '300'
    $vilHLog = Join-Path $vilHHome 'logs\server.log'
    $ckH = Get-LogLength $sessHLog
    $vilH = Start-ServerProcess $villageExe $vilHHome '--seconds 15'
    $procs.Add($vilH)
    if ($null -eq (Wait-LogMatch $sessHLog 'register -> server_id=(\d+)' 8000 $ckH)) {
        throw 'phase 6h: vilH 등록 실패'
    }

    # ① 만료 직전 — 예약(H1, 유효 2000ms)이 스윕(300ms) 여러 번을 겪고도
    # 살아 있는지, 1000ms(만료 전) 시점의 Enter 성공으로 확인한다.
    $cliH1 = Connect-Tcp $SessionClientPort
    $stH1 = $cliH1.GetStream()
    Send-ClientFrame $stH1 $CliSessionLoginReq (New-LoginBody 92001)
    Read-ClientFrame $stH1 3000 | Out-Null
    $cliH1.Close()
    Start-Sleep -Milliseconds 1000      # 스윕(300ms) 3회 이상 경과 · 만료(2000ms) 전
    $vclH1 = Connect-Tcp 9060
    $vstH1 = $vclH1.GetStream()
    Send-ClientFrame $vstH1 $VilEnterReq (New-LoginBody 92001)
    $enterH1 = Read-ClientFrame $vstH1 3000
    $enterH1Ok = ($null -ne $enterH1) -and ($enterH1.MsgId -eq $VilEnterAck)
    $enterH1Body = if ($enterH1Ok) { ConvertFrom-EnterAckBody $enterH1.Body } else { $null }
    Add-Result 'P6-14a(만료 직전 생존)' ($enterH1Ok -and ($enterH1Body.Result -eq $ResultOk)) `
        ("스윕 여러 번 경과 후(만료 전) Enter result=$(if ($enterH1Body) { $enterH1Body.Result } else { '-' })(기대 0 — 살아 있는 예약을 스윕이 안 지웠다)")
    $vclH1.Close()

    # ② 만료 직후 — 예약(H2)을 만료·스윕 주기 둘 다 지나도록 방치한 뒤 마을
    # 로그에 회수 건수가 찍히는지 확인한다.
    $ckH2 = Get-LogLength $vilHLog
    $cliH2 = Connect-Tcp $SessionClientPort
    $stH2 = $cliH2.GetStream()
    Send-ClientFrame $stH2 $CliSessionLoginReq (New-LoginBody 92002)
    Read-ClientFrame $stH2 3000 | Out-Null
    $cliH2.Close()
    Start-Sleep -Milliseconds 2400      # 만료(2000ms) + 스윕(300ms) 한 번 이상 경과
    $mSweepH = Wait-LogMatch $vilHLog 'entry sweep: removed=[1-9]' 2000 $ckH2
    Add-Result 'P6-14b(만료 직후 회수)' ($null -ne $mSweepH) `
        ("만료+스윕 경과 후 마을 로그 -> " + $(if ($mSweepH) { $mSweepH.Value } else { '(관측 안 됨)' }) + " (기대 회수>=1)")

    $sessH.Kill(); $sessH.WaitForExit(5000) | Out-Null
    $vilH.Kill(); $vilH.WaitForExit(3000) | Out-Null

    # ═══════════════════════════════════════════════════════════════════════
    #  Phase 6i — 항목17①. 필요한 상태는 "끊긴 링크로 발신
    #  시도"가 아니라 "세션은 예약을 기억하는데 마을에는 없다"다 — 마을을
    #  죽였다 유예 안에 재기동시키면 만들어진다(#10 의 kill→부활 인프라를
    #  그대로 쓴다). EntryTable 은 마을 프로세스 메모리라 재기동하면 비고,
    #  세션의 reservations_ 는 그 예약을 그대로 들고 있다 — 그 어긋남이
    #  Enter 거부의 유일한 원인이 되도록 세션 reserve.expire_ms 를 넉넉히
    #  잡아 F2(만료 예약)의 원인과 안 섞이게 한다.
    # ═══════════════════════════════════════════════════════════════════════
    Start-Sleep -Milliseconds 700
    $sessIHome = New-SessionHome 'sessI' '16' '500' '30000'
    $sessILog = Join-Path $sessIHome 'logs\server.log'
    $sessI = Start-ServerProcess $sessionExe $sessIHome '--seconds 15'
    $procs.Add($sessI)
    if ($null -eq (Wait-LogMatch $sessILog 'session server up' 8000)) {
        throw 'phase 6i: sessI 기동 로그가 없다'
    }
    $vilI1Home = New-VillageHome 'vilI1' '9070'
    $ckI1 = Get-LogLength $sessILog
    $vilI1 = Start-ServerProcess $villageExe $vilI1Home '--seconds 20'
    $procs.Add($vilI1)
    $mI1 = Wait-LogMatch $sessILog 'register -> server_id=(\d+)' 8000 $ckI1
    if ($null -eq $mI1) { throw 'phase 6i: vilI1 등록 실패' }
    $sidI1 = [int]$mI1.Groups[1].Value

    $cliI1 = Connect-Tcp $SessionClientPort
    $stI1 = $cliI1.GetStream()
    Send-ClientFrame $stI1 $CliSessionLoginReq (New-LoginBody 94001)
    $ackI1 = Read-ClientFrame $stI1 3000
    $loginI1 = if ($null -ne $ackI1 -and $ackI1.MsgId -eq $CliSessionLoginAck) { ConvertFrom-LoginAckBody $ackI1.Body } else { $null }
    $cliI1.Close()

    # 마을을 kill 한다(#10 과 같은 F10 경로 — Unregister 없는 끊김 → orphan).
    # 세션은 이 server_id 항목과 그 예약을 유예(2000ms) 안이면 그대로 들고
    # 있다. 유예 안에 같은 host:port 로 재기동해 부활시킨다.
    $ckI2 = Get-LogLength $sessILog
    Stop-Process -Id $vilI1.Id -Force
    $vilI2Home = New-VillageHome 'vilI2' '9070'
    $vilI2 = Start-ServerProcess $villageExe $vilI2Home '--seconds 15'
    $procs.Add($vilI2)
    $mI2 = Wait-LogMatch $sessILog 'register -> server_id=(\d+)' 8000 $ckI2
    $sidI2 = if ($mI2) { [int]$mI2.Groups[1].Value } else { -1 }
    $revivedOk = ($sidI2 -eq $sidI1)

    # 부활한 마을(프로세스는 새것 — EntryTable 은 비어 있다)에 옛 예약으로
    # Enter → 거부돼야 한다. 만료 대기 없이 "마을에 없다"만으로 거부되는지
    # 보려면 여기서 곧바로 시도해야 한다(reserve.expire_ms=30000 이라 아직
    # 안 만료됐다).
    $vclI1 = Connect-Tcp 9070
    $vstI1 = $vclI1.GetStream()
    Send-ClientFrame $vstI1 $VilEnterReq (New-LoginBody 94001)
    $entI1 = Read-ClientFrame $vstI1 3000
    $entI1Ok = ($null -ne $entI1) -and ($entI1.MsgId -eq $VilEnterAck)
    $entI1Body = if ($entI1Ok) { ConvertFrom-EnterAckBody $entI1.Body } else { $null }
    $vclI1.Close()
    Add-Result 'P6-17-1a(부활 마을엔 예약 없음)' (($null -ne $loginI1) -and ($loginI1.Result -eq 0) -and $revivedOk `
        -and $entI1Ok -and ($entI1Body.Result -eq $ResultInvalidArg)) `
        ("부활 server_id " + $sidI1 + "->" + $sidI2 + "(" + $revivedOk + ") · 옛 예약으로 Enter result=$(if ($entI1Body) { $entI1Body.Result } else { '-' })(기대 3 — 만료 아니라 '마을에 없음'이 원인, expire_ms=30000 이라 아직 안 만료됐다)")

    # 클라가 세션부터 다시 타면(§18-4 재접속 경로) 새 Reserve 가 지금 살아
    # 있는 마을(vilI2)에 도달해 저장되고, 그 예약으로는 Enter 가 성공한다.
    $cliI2 = Connect-Tcp $SessionClientPort
    $stI2 = $cliI2.GetStream()
    Send-ClientFrame $stI2 $CliSessionLoginReq (New-LoginBody 94001)
    $ackI2b = Read-ClientFrame $stI2 3000
    $loginI2b = if ($null -ne $ackI2b -and $ackI2b.MsgId -eq $CliSessionLoginAck) { ConvertFrom-LoginAckBody $ackI2b.Body } else { $null }
    $cliI2.Close()
    $entI2Ok = $false; $entI2Body = $null
    if ($null -ne $loginI2b -and $loginI2b.Result -eq 0) {
        $vclI2 = Connect-Tcp $loginI2b.Port
        $vstI2 = $vclI2.GetStream()
        Send-ClientFrame $vstI2 $VilEnterReq (New-LoginBody 94001)
        $entI2 = Read-ClientFrame $vstI2 3000
        $entI2Ok = ($null -ne $entI2) -and ($entI2.MsgId -eq $VilEnterAck)
        $entI2Body = if ($entI2Ok) { ConvertFrom-EnterAckBody $entI2.Body } else { $null }
        $vclI2.Close()
    }
    Add-Result 'P6-17-1b(세션부터 재접속하면 성공)' (($null -ne $loginI2b) -and ($loginI2b.Result -eq 0) `
        -and $entI2Ok -and ($entI2Body.Result -eq $ResultOk)) `
        ("재로그인 -> port=$(if ($loginI2b) { $loginI2b.Port } else { '-' }) · 새 예약으로 Enter result=$(if ($entI2Body) { $entI2Body.Result } else { '-' })(기대 0 — 살아있는 마을에 새로 도달한 예약)")

    $sessI.Kill(); $sessI.WaitForExit(5000) | Out-Null
    $vilI2.Kill(); $vilI2.WaitForExit(3000) | Out-Null

    # ═══════════════════════════════════════════════════════════════════════
    #  Phase 6j — Registry::player_left() 의 server_id 가드
    #  (registry.cpp:138-142 — 늦게 도착한 PlayerLeave 가 다른 서버가 넣은 산
    #  항목을 지우는 것을 막는다)를 실제로 태우는 항목이 하나도 없었다 —
    #  removed 항이 안 늘고 remain 에 그 player_id 가 남아 있는지로 확인한다.
    #  가드를 지운 뮤턴트면 removed +1 · remain -1 로 갈린다.
    # ═══════════════════════════════════════════════════════════════════════
    Start-Sleep -Milliseconds 700
    $sessJHome = New-SessionHome 'sessJ' '16'
    $sessJLog = Join-Path $sessJHome 'logs\server.log'
    $sessJ = Start-ServerProcess $sessionExe $sessJHome '--seconds 5'
    $procs.Add($sessJ)
    if ($null -eq (Wait-LogMatch $sessJLog 'session server up' 8000)) {
        throw 'phase 6j: sessJ 기동 로그가 없다'
    }
    $fakeJX = Connect-Tcp $SessionS2sPort
    $fsJX = $fakeJX.GetStream()
    Send-S2sFrame $fsJX $MsgRegister 1 (New-RegisterBody $VerOk 7801 10 0 '127.0.0.1')
    Read-S2sFrame $fsJX 3000 | Out-Null
    Send-S2sFrame $fsJX $MsgPlayerEnter 0 (New-LoginBody 95001)   # N -> X

    $fakeJY = Connect-Tcp $SessionS2sPort
    $fsJY = $fakeJY.GetStream()
    Send-S2sFrame $fsJY $MsgRegister 1 (New-RegisterBody $VerOk 7802 10 0 '127.0.0.2')
    Read-S2sFrame $fsJY 3000 | Out-Null
    Send-S2sFrame $fsJY $MsgPlayerEnter 0 (New-LoginBody 95001)   # N 을 Y 가 덮어쓴다(N -> Y)
    Start-Sleep -Milliseconds 300

    # X 가 늦게 도착한 PlayerLeave(N) 를 보낸다 — 지금 N 의 소유자는 Y 라
    # 가드가 있으면 no-op 이어야 한다(X 는 자기가 넣은 적 없는 걸 지우려는
    # 셈이다).
    Send-S2sFrame $fsJX $MsgPlayerLeave 0 (New-LoginBody 95001)
    Start-Sleep -Milliseconds 300

    $sessJ.WaitForExit(15000) | Out-Null
    if (-not $sessJ.HasExited) { $sessJ.Kill() }
    $sJText = Read-LogText $sessJLog
    $sJS5 = [regex]::Match($sJText, '\[SESS \] connections added=(\d+) removed=(\d+) fullsync_replaced=(\d+) remain=(\d+)')
    if ($sJS5.Success) {
        $sJAdded = [int]$sJS5.Groups[1].Value; $sJRemoved = [int]$sJS5.Groups[2].Value
        $sJRemain = [int]$sJS5.Groups[4].Value
        # added=2(X 의 최초 삽입 + Y 의 덮어쓰기 — player_entered 는 매번 센다)
        # · removed=0(X 의 뒤늦은 PlayerLeave 가 가드에 막혀 no-op) · remain=1
        # (N 은 Y 소유로 그대로 남는다). 가드가 죽은 뮤턴트면 removed=1
        # remain=0 으로 갈린다.
        Add-Result 'P6-MED1(server_id 가드)' (($sJAdded -eq 2) -and ($sJRemoved -eq 0) -and ($sJRemain -eq 1)) `
            ("[SESS ] connections added=$sJAdded(기대 2) removed=$sJRemoved(기대 0 — X 의 뒤늦은 Leave 가 가드에 막힘) remain=$sJRemain(기대 1 — N 은 Y 소유로 잔존)")
    } else {
        Add-Result 'P6-MED1(server_id 가드)' $false 'phase 6j [SESS ] 요약을 못 찾았다'
    }
    try { $fakeJX.Close() } catch {}
    try { $fakeJY.Close() } catch {}

    # ═══════════════════════════════════════════════════════════════════════
    #  Phase 6k — 예약 1건에 다수 연결이 몰려도 정확히 하나만 통과하는지.
    #  존 스레드 시절엔 이 항목이 EntryTable 뮤텍스를 검증하지 않았다 — kEnterReq 가
    #  존 스레드 고정 배정(zone_id % N, 로그인 전 placement 초기값 0 이라 항상
    #  존 스레드 0)을 거쳐 FIFO 로 하나씩 처리됐으므로 EntryTable 동시 진입
    #  자체가 안 일어났다(뮤텍스가 있든 없든 결과가 같았다).
    #  ⚠️ 그 배정 자체가 지워졌다(placement.h·zone_manager 삭제) — 지금은
    #  kEnterReq 가 이 N 개의 서로 다른 세션 각각의 직렬 큐 실행권으로
    #  app::WorkerPool 의 공유 큐에 올라가고, 워커 최대 8개([app] workers)가
    #  그 실행권을 동시에 뽑아 돌 수 있다(app/worker_pool.h). 즉 "같은
    #  player_id 로 EntryTable::consume_reservation 을 동시에 두드리는" 축이
    #  이제 구조적으로 가능해졌다 — 존 스레드 0 고정이라는 옛 전제가 사라졌으므로
    #  이 항목이 뮤텍스를 검증하지 않는다는 옛 결론은 그대로 옮겨 적을 수
    #  없다. ⚠️ 다만 이 결론을 뒤집는 재검증(락을 지운 채 이 항목이 깨지는지)은
    #  당시 뮤턴트 5종(M1~M5)에 없다 — M5 는
    #  notifier 원자화 해제고 뮤텍스 자체 제거가 아니다. 그래서 결과 자체는
    #  이름에 옮겨 적지 않고 "미확정"으로만 남긴다(확인 안 한 것을 확인한
    #  것처럼 쓰지 않는다). 뮤텍스의 필요성을 재는 다른 시도(존·S2S·스윕 세
    #  스레드를 실제로 동시에 건드리는 축)는 Phase 6n 을 본다.
    # ═══════════════════════════════════════════════════════════════════════
    Start-Sleep -Milliseconds 700
    $sessKHome = New-SessionHome 'sessK' '16'
    $sessKLog = Join-Path $sessKHome 'logs\server.log'
    $sessK = Start-ServerProcess $sessionExe $sessKHome '--seconds 12'
    $procs.Add($sessK)
    if ($null -eq (Wait-LogMatch $sessKLog 'session server up' 8000)) {
        throw 'phase 6k: sessK 기동 로그가 없다'
    }
    $vilKHome = New-VillageHome 'vilK' '9080'
    $ckK = Get-LogLength $sessKLog
    $vilK = Start-ServerProcess $villageExe $vilKHome '--seconds 15'
    $procs.Add($vilK)
    if ($null -eq (Wait-LogMatch $sessKLog 'register -> server_id=(\d+)' 8000 $ckK)) {
        throw 'phase 6k: vilK 등록 실패'
    }
    $cliK = Connect-Tcp $SessionClientPort
    $stK = $cliK.GetStream()
    Send-ClientFrame $stK $CliSessionLoginReq (New-LoginBody 96001)
    $ackK = Read-ClientFrame $stK 3000
    $loginK = if ($null -ne $ackK -and $ackK.MsgId -eq $CliSessionLoginAck) { ConvertFrom-LoginAckBody $ackK.Body } else { $null }
    $cliK.Close()

    $raceOk = $false; $raceOkCount = 0; $raceInvalidCount = 0; $raceNoRespCount = 0
    if ($null -ne $loginK -and $loginK.Result -eq 0) {
        $raceN = 40
        $raceConns = New-Object System.Collections.Generic.List[object]
        for ($i = 0; $i -lt $raceN; $i++) {
            try { $raceConns.Add((Connect-Tcp $loginK.Port)) } catch {}
        }
        # 전부 연결한 뒤에야 프레임을 쏜다 — 응답을 하나도 기다리지 않고
        # 몰아 넣어 서버 쪽 동시 처리 창을 넓힌다(zone_race.ps1 과 같은 정신).
        foreach ($rc in $raceConns) {
            try { Send-ClientFrame $rc.GetStream() $VilEnterReq (New-LoginBody 96001) } catch {}
        }
        foreach ($rc in $raceConns) {
            $rack = Read-ClientFrame $rc.GetStream() 2000
            if ($null -eq $rack -or $rack.MsgId -ne $VilEnterAck) {
                $raceNoRespCount++
            } else {
                $rbody = ConvertFrom-EnterAckBody $rack.Body
                if ($rbody.Result -eq $ResultOk) { $raceOkCount++ } else { $raceInvalidCount++ }
            }
            try { $rc.Close() } catch {}
        }
        $raceOk = ($raceOkCount -eq 1) -and ($raceInvalidCount -eq $raceN - 1) -and ($raceNoRespCount -eq 0)
    }
    $sessK.WaitForExit(20000) | Out-Null
    if (-not $sessK.HasExited) { $sessK.Kill() }
    $vilK.Kill(); $vilK.WaitForExit(3000) | Out-Null
    Add-Result 'P6-6k(예약 1건 단일 통과 — 뮤텍스 필요성 미확정)' $raceOk `
        ("app::WorkerPool 워커(최대 8개)에 흩어질 수 있는 Enter 40건 -> ok=$raceOkCount(기대 1) invalid=$raceInvalidCount(기대 39) 무응답=$raceNoRespCount(기대 0) — 통과 자체는 뮤텍스가 지키는 것과 일치하지만, 락 제거 재시도는 이 Step 의 뮤턴트 목록(M1~M5)에 없어 필요성 증명은 미확정이다")

    # ═══════════════════════════════════════════════════════════════════════
    #  Phase 6m — 항목5(u32 절단 회귀). player_id 의 하위 32비트가 실재 DB
    #  player_id 와 겹치는 값(0x100000001 vs 1)으로 로그인·Enter·kInventoryReq
    #  까지 실물로 태운다 — 다른 P6-* 는 전부 작은 정수 player_id 를 쓰므로
    #  상위 32비트가 항상 0 이라 절단이 재도입돼도 안 걸린다(다른 하네스가 구조적으로
    #  못 잡는 회귀다). player 1(alice, sql\01_schema.sql:82-83
    #  의 초기 시드는 item 100 x10 · item 200 x5)이 이 harness 들과 실DB 를
    #  공유해 값이 드리프트할 수 있다 — 그래서 판정은 정확한 수량이 아니라
    #  count(비었나/2건 보이나)만 본다. 존재 자체가 절단의 증거다.
    # ═══════════════════════════════════════════════════════════════════════
    Start-Sleep -Milliseconds 700
    $sessMHome = New-SessionHome 'sessM' '16'
    $sessMLog = Join-Path $sessMHome 'logs\server.log'
    $sessM = Start-ServerProcess $sessionExe $sessMHome '--seconds 12'
    $procs.Add($sessM)
    if ($null -eq (Wait-LogMatch $sessMLog 'session server up' 8000)) {
        throw 'phase 6m: sessM 기동 로그가 없다'
    }
    $vilMHome = New-VillageHome 'vilM' '9090'
    $ckM = Get-LogLength $sessMLog
    $vilM = Start-ServerProcess $villageExe $vilMHome '--seconds 15'
    $procs.Add($vilM)
    if ($null -eq (Wait-LogMatch $sessMLog 'register -> server_id=(\d+)' 8000 $ckM)) {
        throw 'phase 6m: vilM 등록 실패'
    }
    $truncPid = 4294967297    # 0x100000001 — 하위 32비트가 정확히 1(DB player 1=alice 와 충돌)
    $cliM = Connect-Tcp $SessionClientPort
    $stM = $cliM.GetStream()
    Send-ClientFrame $stM $CliSessionLoginReq (New-LoginBody $truncPid)
    $ackM = Read-ClientFrame $stM 3000
    $loginM = if ($null -ne $ackM -and $ackM.MsgId -eq $CliSessionLoginAck) { ConvertFrom-LoginAckBody $ackM.Body } else { $null }
    $cliM.Close()

    $invOk = $false; $invCount = -1; $invDetail = '(Enter 실패)'
    if ($null -ne $loginM -and $loginM.Result -eq 0) {
        $vclM = Connect-Tcp $loginM.Port
        $vstM = $vclM.GetStream()
        Send-ClientFrame $vstM $VilEnterReq (New-LoginBody $truncPid)
        $enterM = Read-ClientFrame $vstM 3000
        $enterMOk = ($null -ne $enterM) -and ($enterM.MsgId -eq $VilEnterAck)
        $enterMBody = if ($enterMOk) { ConvertFrom-EnterAckBody $enterM.Body } else { $null }
        if ($enterMOk -and ($enterMBody.Result -eq $ResultOk)) {
            Send-ClientFrame $vstM $VilInventoryReq ([byte[]]@())
            $invAck = Read-ClientFrame $vstM 3000
            if ($null -ne $invAck -and $invAck.MsgId -eq $VilInventoryAck) {
                $invBody = ConvertFrom-InventoryAckBody $invAck.Body
                $invOk = $true
                $invCount = $invBody.Count
                $invDetail = ($invBody.Items | ForEach-Object { "item$($_.ItemId)x$($_.Count)" }) -join ','
            } else {
                $invDetail = '(InventoryAck 무응답)'
            }
        } else {
            $invDetail = "(Enter result=$(if ($enterMBody) { $enterMBody.Result } else { '무응답' }))"
        }
        $vclM.Close()
    }
    $sessM.WaitForExit(15000) | Out-Null
    if (-not $sessM.HasExited) { $sessM.Kill() }
    $vilM.Kill(); $vilM.WaitForExit(3000) | Out-Null

    # 정상 코드: player 4294967297 는 DB 에 없다 → count=0.
    # MUT8(u32 절단 재도입): 하위 32비트가 1 로 잘려 alice 로 조회된다
    #   → count=2(실측: item100x10·item200x0 — 시드값 item200x5 에서 다른
    #   harness 의 거래로 드리프트됐다. count>0 자체가 절단의 증거다).
    Add-Result 'P6-M8(player_id u32 절단 회귀)' ($invOk -and ($invCount -eq 0)) `
        ("Enter(player_id=0x100000001) 후 kInventoryReq -> count=$invCount(기대 0 — DB 에 없는 player) $invDetail")

    # ═══════════════════════════════════════════════════════════════════════
    #  Phase 6n — EntryTable 을 실제로 만지는 세 스레드 축을 동시에 겨눈다 —
    #  app::WorkerPool 워커(consume_reservation·enter·leave — kEnterReq 를
    #  처리하는 쪽. 옛 구조에서는 이 자리가 고정된 "존 스레드" 였다) · S2S
    #  스레드(add_reservation·current·snapshot) · EntrySweeper 스레드
    #  (sweep_expired). 가짜 세션이 서로 다른 player_id 로 Reserve 를 연속
    #  발사하고(S2S 스레드), 짝수 인덱스만 실물 클라가 즉시 kEnterReq 로
    #  소비하고(워커), 홀수 인덱스는 일부러 안 건드려 만료시켜 sweep_ms=100
    #  (config 하한)의 스윕 스레드가 지우게 한다 — 세 스레드가 같은
    #  EntryTable::mutex_ 를 같은 창에서 두드린다는 것은 sweep 발화(=30 제거)로
    #  실측 확인됐다.
    #  ⚠️ 아래 "MUT5 재시도" 문구는 존 스레드 고정 배정 시절의 측정값이다
    #  — 그 배정이 worker_pool 로 바뀌어 워커 스레드 수가 최대 8개로
    #  늘었다(위 6k 머리말과 같은 변화). 그 재측정은 이번 Step 의 뮤턴트
    #  목록(M1~M5)에 없어 지금 구조 위에서 다시 걷지 않았다 — 아래 수치는
    #  옛 구조에 대한 기록으로만 읽는다(검증 시점과 대상을 함께 적는다).
    #  「ASan 은 데이터 레이스를 안 본다」(TSan 의 일이고 이 저장소에 TSan
    #  구성이 없다)는 결론 자체는 구조와 무관하게 여전히 유효하다.
    # ═══════════════════════════════════════════════════════════════════════
    Start-Sleep -Milliseconds 700
    $fakeSessListenerN = New-S2sListener $SessionS2sPort
    # sweep_ms 는 config get_int 의 허용 하한이 100 이다(bootstrap.cpp — 그
    # 밖이면 [WARN] 과 함께 기본값 30000 으로 조용히 돌아간다. 처음 10 으로
    # 시도했다가 실측으로 이 clamp 에 걸려 스윕이 한 번도 안 도는 것을 봤다).
    $vilNHome = New-VillageHome 'vilN' '9095' '4096' '500' '10000' '100' '0'   # sweep_ms=100(하한)
    $vilNLog = Join-Path $vilNHome 'logs\server.log'
    $vilN = Start-ServerProcess $villageExe $vilNHome '--seconds 15'
    $procs.Add($vilN)

    $raceN2 = 60
    $raceOkCount2 = 0; $raceOtherCount2 = 0; $raceNoRespCount2 = 0
    $badResultCount2 = 0
    $registeredN = $false
    $fakeSessConnN = Wait-Accept $fakeSessListenerN 8000 'phase 6n vilN 연결'
    if ($null -ne $fakeSessConnN) {
        $fsN = $fakeSessConnN.GetStream()
        $regFrameN = Read-S2sFrame $fsN 3000
        if ($null -ne $regFrameN -and $regFrameN.MsgId -eq $MsgRegister) {
            Send-S2sFrame $fsN $MsgRegisterAck 1 (New-RegisterAckBody 1 0)
            $registeredN = $true
        }
    }
    if ($registeredN) {
        # 실물 클라 연결을 먼저 다 연다(P6-6k 와 같은 정신 — 응답을 기다리지
        # 않고 몰아 넣어 처리 창을 넓힌다). player_id 는 전부 다르게 잡아
        # "예약 1건에 다수 몰림"(단일 통과 안전망이 걸리는 축)이 아니라 "서로
        # 다른 예약을 스윕·S2S·워커가 동시에 건드린다"는 축을 만든다.
        # 짝수 인덱스만 kEnterReq 를 보낸다(worker_pool 워커가
        # consume_reservation·enter 로 지운다) — 홀수 인덱스는 일부러 안 보내
        # 만료(150ms)까지 살려 둔다(스윕 스레드가 sweep_expired 로 지운다).
        # 그래야 "S2S 스레드가 넣는 동안 워커와 스윕 스레드가 동시에 서로 다른
        # 원소를 지운다"는 축이 실제로 만들어진다 — 전부 즉시 소비되면 스윕이
        # 지울 게 안 남는다.
        $raceConns2 = New-Object System.Collections.Generic.List[object]
        for ($i = 0; $i -lt $raceN2; $i++) {
            try { $raceConns2.Add((Connect-Tcp 9095)) } catch {}
        }

        # add_reservation 덮어쓰기(entry_table.h — "같은 player_id 로 다시 오면
        #   덮어쓴다 · 옛 예약의 만료 여부와 무관하게 최신 expire_at_ms 로 대체된다")
        #   전용 항목 — 위 raceN2 시나리오는 player_id 가 전부 달라 이 경로를 한
        #   번도 안 지난다. 짧은(150ms) 예약을 먼저 걸고 곧바로 긴(5000ms) 예약을
        #   같은 player_id 로 다시 걸어, 900ms 뒤(짧은 쪽은 만료·긴 쪽은 아직
        #   유효한 지점)에 Enter 해 어느 예약이 남았는지로 덮어쓰기 여부를 가른다.
        # ReserveAck 두 번을 직접 읽어 kResultOk 를 단언하는 이유 — Enter 성공만
        #   보면 "짧은 쪽 전송이 (예: 예외로) 애초에 안 나가 긴 쪽 하나만 존재"해도
        #   같은 결과가 나와 덮어쓰기가 안 도는 채로 통과할 수 있다. 두 요청이 실제로
        #   서버에 도달해 각각 응답을 받았다는 것까지 확인해야 그 구멍이 닫힌다.
        # Read-S2sFrame 이 아니라 Read-S2sFrameFiltered 를 쓰는 이유 — 이 연결은
        #   등록(RegisterAck) 직후라 마을이 곧바로 FullSync 를 발신하고(집합이
        #   비어 있어도 한 번은 나간다는 계약), 그 뒤로도 heartbeat_ms(500ms)
        #   주기로 Heartbeat 가 낀다. 위 raceN2 시나리오가 이 문제를 안 겪은
        #   것은 응답을 아예 안 읽어서다 — 이 항목이 이 연결에서 응답을 읽는
        #   첫 자리라 여기서 처음 드러난다.
        $owPid = 97000 + $raceN2
        $owConn = Connect-Tcp 9095
        $owSkipped1 = New-Object System.Collections.Generic.List[int]
        $owSkipped2 = New-Object System.Collections.Generic.List[int]
        try { Send-S2sFrame $fsN $MsgReserve 500 (New-ReserveBody $owPid 150) } catch {}
        $owAck1 = Read-S2sFrameFiltered $fsN $MsgReserveAck 3000 $owSkipped1
        try { Send-S2sFrame $fsN $MsgReserve 501 (New-ReserveBody $owPid 5000) } catch {}
        $owAck2 = Read-S2sFrameFiltered $fsN $MsgReserveAck 3000 $owSkipped2
        $owReservedOk = ($null -ne $owAck1) -and ($owAck1.Body[0] -eq $ResultOk) `
            -and ($null -ne $owAck2) -and ($owAck2.Body[0] -eq $ResultOk)

        for ($i = 0; $i -lt $raceConns2.Count; $i++) {
            $pidN = 97000 + $i
            try { Send-S2sFrame $fsN $MsgReserve (100 + $i) (New-ReserveBody $pidN 150) } catch {}
            if ($i % 2 -eq 0) {
                try { Send-ClientFrame $raceConns2[$i].GetStream() $VilEnterReq (New-LoginBody $pidN) } catch {}
            }
        }
        Start-Sleep -Milliseconds 900   # sweep_ms=100 이 이 사이 여러 회 돈다(expire_ms=150 보다 넉넉히 길다)
        for ($i = 0; $i -lt $raceConns2.Count; $i++) {
            $rc = $raceConns2[$i]
            if ($i % 2 -eq 0) {
                $rack = Read-ClientFrame $rc.GetStream() 1000
                if ($null -eq $rack -or $rack.MsgId -ne $VilEnterAck) {
                    $raceNoRespCount2++
                } else {
                    $rbody = ConvertFrom-EnterAckBody $rack.Body
                    if ($rbody.Result -eq $ResultOk) { $raceOkCount2++ }
                    elseif ($rbody.Result -eq $ResultInvalidArg) { $raceOtherCount2++ }
                    else { $badResultCount2++ }   # 0·3 밖의 값 — 손상의 징후
                }
            }
            try { $rc.Close() } catch {}
        }

        # 900ms 는 짧은 예약(150ms)의 만료 이후·긴 예약(5000ms)의 만료 이전이다 —
        #   덮어쓰기가 됐으면 남은 것은 긴 예약뿐이라 Enter 가 성공(kOk)하고,
        #   안 됐으면(첫 예약이 그대로 남아 있으면) 이미 만료돼 있어 실패(kInvalidArg)한다.
        $owStream = $owConn.GetStream()
        try { Send-ClientFrame $owStream $VilEnterReq (New-LoginBody $owPid) } catch {}
        $owEnterAck = Read-ClientFrame $owStream 1000
        $owEnterResult = -1
        $owEnterOk = $false
        if ($null -ne $owEnterAck -and $owEnterAck.MsgId -eq $VilEnterAck) {
            $owBody = ConvertFrom-EnterAckBody $owEnterAck.Body
            $owEnterResult = $owBody.Result
            $owEnterOk = ($owBody.Result -eq $ResultOk) -and ($owBody.PlayerId -eq $owPid)
        }
        try { $owConn.Close() } catch {}

        $owOverwriteOk = $owReservedOk -and $owEnterOk
        $owSkipped1Text = if ($owSkipped1.Count -gt 0) { ($owSkipped1 | ForEach-Object { "0x{0:X4}" -f $_ }) -join ',' } else { '없음' }
        $owSkipped2Text = if ($owSkipped2.Count -gt 0) { ($owSkipped2 | ForEach-Object { "0x{0:X4}" -f $_ }) -join ',' } else { '없음' }
        Add-Result 'P6-6n2(add_reservation 덮어쓰기 — 최신 expire_at_ms 로 대체)' $owOverwriteOk `
            ("Reserve(150ms)->Reserve(5000ms) 둘 다 ReserveAck=$ResultOk 수신($owReservedOk, 기대 True) " + `
             "· 1차 읽기 전 낀 프레임=$owSkipped1Text · 2차 읽기 전 낀 프레임=$owSkipped2Text " + `
             "· 900ms 뒤 Enter result=$owEnterResult(기대 $ResultOk — 덮어쓰기 실패 시 만료로 $ResultInvalidArg)")
    }
    try { $fakeSessConnN.Close() } catch {}
    try { $fakeSessListenerN.Stop() } catch {}
    $vilN.WaitForExit(20000) | Out-Null
    if (-not $vilN.HasExited) { $vilN.Kill() }
    $vilNExitOk = $vilN.HasExited -and ($vilN.ExitCode -eq 0)
    $vilNText = Read-LogText $vilNLog
    $sweepMatches = [regex]::Matches($vilNText, 'entry sweep: removed=(\d+)')
    $sweepFires = $sweepMatches.Count
    $sweepRemovedTotal = 0
    foreach ($m in $sweepMatches) { $sweepRemovedTotal += [int]$m.Groups[1].Value }
    $totalResponded2 = $raceOkCount2 + $raceOtherCount2 + $badResultCount2

    # 판정: 응답이 전부 왔고(무응답 0) · 값이 전부 0·3 안이고(손상 징후 없음) ·
    #   village 가 정상 종료했고(크래시·ASan 발화 없음) · 스윕이 실제로 돌아
    #   홀수 인덱스(Enter 를 안 보낸 쪽)를 지웠다(경합 창이 실제로 열렸다는
    #   증거 — 로그는 removed>0 인 회차만 찍으므로 발화 횟수 자체는 몇 회든
    #   무관하고 "지운 적이 있는가"만 본다). 이 항목 자체는 유지한다 —
    #   "이 세 스레드가 이 창에서 겹쳐도 응답·값이 항상 정상"이라는 계약은
    #   락이 있는 정상 코드에 대해 여전히 유효한 회귀 방지선이다. 락 제거
    #   재시도(MUT5)의 옛 결과("ASan 이 데이터 레이스를 못 본다" —
    #   위 Phase 6n 머리말 참조)는 옛 존 스레드 구조에 대한 기록이라 이름에서
    #   "결정 불가로 확정"을 걷어낸다 — 지금 구조(worker_pool)에서 다시 걷지
    #   않았으므로 그 결론을 그대로 승계하지 않는다.
    $mut5RetryOk = $registeredN -and ($raceNoRespCount2 -eq 0) -and ($badResultCount2 -eq 0) `
        -and $vilNExitOk -and ($sweepFires -ge 1)
    Add-Result 'P6-6n(EntryTable 3-스레드 경합 — 정상 코드 회귀 방지선)' $mut5RetryOk `
        ("등록=$registeredN · ok=$raceOkCount2 invalid=$raceOtherCount2 손상값=$badResultCount2 무응답=$raceNoRespCount2(전부 기대 0) · village exit=$vilNExitOk(기대 True) · sweep 발화=$sweepFires 회·누적제거=$sweepRemovedTotal(기대 발화>=1 — 경합 창이 실제로 열렸다는 증거) — 락 필요성 자체의 결정 가능 여부는 지금 구조에서 미재검증(위 Phase 6n 머리말 참조)")

    # ═════════════════════════════════════════════════════════════════
    #  드레인 콘솔(S1~S4) — 실물 session.exe stdin 경로
    # ═════════════════════════════════════════════════════════════════
    #   Start-ServerProcessWithStdin(위 정의)로 --seconds 없이 띄운다 —
    #   getline 대기 경로를 타야 stdin 명령이 먹는다. 빈 줄은 종료 신호라
    #   드레인 명령 전송에는 항상 "drain <id>"/"undrain <id>" 뒤에
    #   개행 하나만 실어 보낸다(WriteLine 이 그렇게 한다) — 빈 줄을 별도로
    #   보내는 일이 없게 한다.
    Start-Sleep -Milliseconds 700
    # RequestTimeout 을 기본(500ms)보다 넉넉히 준다 — S3 가 A 의 Reserve 를
    #   읽고 회신하기까지 PowerShell 쪽 왕복이 몇 번 겹치는데, 기본값이면
    #   스윕(500ms 주기)이 그 사이 pending 을 먼저 타임아웃으로 회수해
    #   회신이 matched=false 로 무시된다(실측으로 걸림 — set_draining 이
    #   전혀 안 불려 S3b 가 no_server 로 샜다).
    $sDrainHome = New-SessionHome 'sDrain' '16' '3000'
    $sDrainLog = Join-Path $sDrainHome 'logs\server.log'
    $sDrain = Start-ServerProcessWithStdin $sessionExe $sDrainHome
    $procs.Add($sDrain)
    if ($null -eq (Wait-LogMatch $sDrainLog 'session server up' 8000)) {
        throw 'phase drain: sDrain 기동 로그가 없다'
    }

    $pS1 = 51001; $pS2 = 51002; $pS4 = 51004; $pS3a = 51005; $pS3b = 51006

    # ── 마을 A 등록(S1·S2·S4·S3 앞부분이 공유한다) ─────────────────────
    $fakeDA = Connect-Tcp $SessionS2sPort
    $fsDA = $fakeDA.GetStream()
    $ckDA = Get-LogLength $sDrainLog
    Send-S2sFrame $fsDA $MsgRegister 1 (New-RegisterBody $VerOk 7801 10 0 '127.0.0.1')
    Read-S2sFrame $fsDA 3000 | Out-Null
    $mDA = Wait-LogMatch $sDrainLog 'register -> server_id=(\d+)' 3000 $ckDA
    if ($null -eq $mDA) { throw 'phase drain: 마을 A 등록 실패' }
    $serverIdA = [int]$mDA.Groups[1].Value

    # ── S1 — stdin drain <A> → kSessionLoginReq 가 no_server(단일 서버 구성) ──
    $sDrain.HarnessStdin.WriteLine("drain $serverIdA")
    $sDrain.HarnessStdin.Flush()
    Start-Sleep -Milliseconds 400
    $cliS1 = Connect-Tcp $SessionClientPort
    $stS1 = $cliS1.GetStream()
    Send-ClientFrame $stS1 $CliSessionLoginReq (New-LoginBody $pS1)
    $ackS1r = Read-ClientFrame $stS1 3000
    $ackS1 = if ($null -ne $ackS1r -and $ackS1r.MsgId -eq $CliSessionLoginAck) { ConvertFrom-LoginAckBody $ackS1r.Body } else { $null }
    Add-Result 'S1(drain <A> → 로그인 no_server)' `
        (($null -ne $ackS1) -and ($ackS1.Result -eq 1)) `
        ("drain $serverIdA 이후 로그인 result=" + $(if ($ackS1) { $ackS1.Result } else { '(무응답)' }) + "(기대 1=no_server — 등록 서버가 A 하나뿐이라 전부 배정 제외됨)")
    try { $cliS1.Close() } catch {}
    Send-S2sFrame $fsDA $MsgHeartbeat 2 ([byte[]](0, 0, 0, 0))   # A 를 orphan 유예 밖에 유지

    # ── S2 — stdin undrain <A> → 배정 재개 ──────────────────────────────
    $sDrain.HarnessStdin.WriteLine("undrain $serverIdA")
    $sDrain.HarnessStdin.Flush()
    Start-Sleep -Milliseconds 400
    $cliS2 = Connect-Tcp $SessionClientPort
    $stS2 = $cliS2.GetStream()
    Send-ClientFrame $stS2 $CliSessionLoginReq (New-LoginBody $pS2)
    $ackS2r = Read-ClientFrame $stS2 3000
    $ackS2 = if ($null -ne $ackS2r -and $ackS2r.MsgId -eq $CliSessionLoginAck) { ConvertFrom-LoginAckBody $ackS2r.Body } else { $null }
    Add-Result 'S2(undrain <A> → 배정 재개)' `
        (($null -ne $ackS2) -and ($ackS2.Result -eq 0) -and ($ackS2.Port -eq 7801)) `
        ("undrain $serverIdA 이후 로그인 result=" + $(if ($ackS2) { $ackS2.Result } else { '(무응답)' }) + " port=" + $(if ($ackS2) { $ackS2.Port } else { '-' }) + "(기대 result=0 port=7801)")
    try { $cliS2.Close() } catch {}
    Send-S2sFrame $fsDA $MsgHeartbeat 3 ([byte[]](0, 0, 0, 0))

    # ── S4 — 미존재 server_id 로 drain 999 → 경고 로그 대조 + 기존 서버 무영향 ──
    $ckS4 = Get-LogLength $sDrainLog
    $sDrain.HarnessStdin.WriteLine('drain 999')
    $sDrain.HarnessStdin.Flush()
    $mS4 = Wait-LogMatch $sDrainLog '\[WARN\] setmode: unknown server_id=999' 3000 $ckS4
    $cliS4 = Connect-Tcp $SessionClientPort
    $stS4 = $cliS4.GetStream()
    Send-ClientFrame $stS4 $CliSessionLoginReq (New-LoginBody $pS4)
    $ackS4r = Read-ClientFrame $stS4 3000
    $ackS4 = if ($null -ne $ackS4r -and $ackS4r.MsgId -eq $CliSessionLoginAck) { ConvertFrom-LoginAckBody $ackS4r.Body } else { $null }
    Add-Result 'S4(미존재 server_id drain — 경고 로그 + 기존 서버 무영향)' `
        (($null -ne $mS4) -and ($null -ne $ackS4) -and ($ackS4.Result -eq 0) -and ($ackS4.Port -eq 7801)) `
        ("경고 로그=" + ($null -ne $mS4) + "(기대 True) · 직후 로그인 result=" + $(if ($ackS4) { $ackS4.Result } else { '(무응답)' }) + " port=" + $(if ($ackS4) { $ackS4.Port } else { '-' }) + "(기대 result=0 port=7801 — A 는 안 건드려졌다)")
    try { $cliS4.Close() } catch {}
    Send-S2sFrame $fsDA $MsgHeartbeat 4 ([byte[]](0, 0, 0, 0))

    # ── S3 — 가짜 마을 갈래: ReserveAck result=3 회신 → 다음 배정에서 제외 ──
    #   A 가 server_id 최소(먼저 등록됨)라 새 로그인은 항상 A 를 먼저 고른다
    #   (동률 시 최소 id — registry.cpp assign() 관례). A 의 Reserve 를
    #   draining(3)으로 거절해 세션이 "A 는 드레인 중"을 스스로 배우게 한
    #   뒤, 다음 로그인이 유일한 대안인 E 로 넘어가는지를 본다.
    $fakeDE = Connect-Tcp $SessionS2sPort
    $fsDE = $fakeDE.GetStream()
    $ckDE = Get-LogLength $sDrainLog
    Send-S2sFrame $fsDE $MsgRegister 1 (New-RegisterBody $VerOk 7802 10 0 '127.0.0.1')
    Read-S2sFrame $fsDE 3000 | Out-Null
    $mDE = Wait-LogMatch $sDrainLog 'register -> server_id=(\d+)' 3000 $ckDE
    if ($null -eq $mDE) { throw 'phase drain: 마을 E 등록 실패' }

    # S2·S4 의 로그인이 A 에 남긴 예약이 아직 유효하면 load(=current+유효
    #   예약수)가 A 만 커져 있어 동률 시 최소 id 규칙까지 안 가고 load
    #   비교에서 이미 E 로 갈릴 수 있다 — S3a 가 "A 를 고른다"를 보려는
    #   것이므로 New-SessionHome 의 expire_ms(2000ms 기본)를 넘겨 그 예약을
    #   비우고 시작한다. 그사이 A 가 orphan 유예(health_period_ms×
    #   health_fail_count=1500ms 기본)를 넘기지 않도록 하트비트를 나눠 보낸다.
    Send-S2sFrame $fsDA $MsgHeartbeat 5 ([byte[]](0, 0, 0, 0))
    Send-S2sFrame $fsDE $MsgHeartbeat 2 ([byte[]](0, 0, 0, 0))
    Start-Sleep -Milliseconds 1100
    Send-S2sFrame $fsDA $MsgHeartbeat 6 ([byte[]](0, 0, 0, 0))
    Send-S2sFrame $fsDE $MsgHeartbeat 3 ([byte[]](0, 0, 0, 0))
    Start-Sleep -Milliseconds 1100

    # $fsDA 는 A 역의 가짜 마을이라, 세션이 A 로 보낸 것(S1 의 drain 1·S2 의
    #   undrain 1 SetMode · S2·S4 자신의 로그인이 낸 Reserve)이 전부 이 소켓에
    #   안 읽힌 채 쌓여 있다. 아래 S3a Reserve 읽기가 MsgId 만 걸러 skip 하므로
    #   「가장 먼저 온 Reserve」를 집으면 S3a 게 아니라 S2 의 옛 Reserve를 집는다
    #   — session_router.cpp 의 revoke_reservation 호출부에 임시로 인자를
    #   찍어 실측했다: revoked=0·issue_id=1(S3a 는 이 Registry 의 3번째 발급이라
    #   issue_id=3 이어야 정상 대조다). 옛 issue_id 로 거절을 보내니 대조가
    #   실패해 S3a 의 산 예약이 안 지워지고, 그 잔여 부하(load=1)가 다음
    #   로그인을 E 로 미는 진짜 이유였다 — draining 학습과 무관하게 이 시나리오가
    #   항상 통과했다. S3a 로그인 전에 큐를 통째로 비운다.
    $drainedS3 = 0
    while ($true) {
        $f = Read-S2sFrame $fsDA 200
        if ($null -eq $f) { break }
        $drainedS3++
    }

    $cliS3a = Connect-Tcp $SessionClientPort
    $stS3a = $cliS3a.GetStream()
    Send-ClientFrame $stS3a $CliSessionLoginReq (New-LoginBody $pS3a)
    $ackS3aR = Read-ClientFrame $stS3a 3000
    $ackS3a = if ($null -ne $ackS3aR -and $ackS3aR.MsgId -eq $CliSessionLoginAck) { ConvertFrom-LoginAckBody $ackS3aR.Body } else { $null }
    $s3aPickedA = ($null -ne $ackS3a) -and ($ackS3a.Result -eq 0) -and ($ackS3a.Port -eq 7801)
    try { $cliS3a.Close() } catch {}

    $skippedS3 = New-Object System.Collections.Generic.List[int]
    $rsvS3 = Read-S2sFrameFiltered $fsDA $MsgReserve 3000 $skippedS3
    $s3ReserveSeen = ($null -ne $rsvS3)
    if ($s3ReserveSeen) {
        Send-S2sFrame $fsDA $MsgReserveAck $rsvS3.Seq ([byte[]]@(3))   # kResultDraining — A 를 draining 으로 학습시킨다
    }
    # E 는 등록 뒤 하트비트 2회(위)만 받았다 — S3a 로그인·Reserve 왕복·이
    #   대기가 누적되면 마지막 하트비트로부터 1500ms(orphan 유예 기본값)를
    #   넘길 수 있다(실측으로 걸림 — 1차 시도에서 S3b 가 no_server 로 샜다).
    #   S3b 직전에 한 번 더 갱신해 그 경주를 없앤다.
    Send-S2sFrame $fsDE $MsgHeartbeat 4 ([byte[]](0, 0, 0, 0))
    # A 도 같은 경주를 탄다 — 마지막 하트비트(6)가 그 앞 1100ms 대기 직전이라
    #   S3a 로그인 + Reserve 왕복 + 이 대기가 겹치면 A 도 1500ms 를 넘길 수
    #   있다 — registry.cpp 의 assign() 에 임시로 상태를 찍어 실측했다:
    #   deadline 을 16ms 초과해 S3b 의 그 assign() 호출 안에서 A 가 lazy
    #   orphan 판정을 맞는 것을 확인했다. orphan 으로 빠지면 entry.draining
    #   검사까지 가지도 않고 후보에서 빠져, draining 학습이 없어도 S3b 가
    #   우연히 E 로 간다 — 이 시나리오가 실제로 검증하려는 것과 다른 경로다.
    #   E 와 같은 이유로 A 도 갱신한다.
    Send-S2sFrame $fsDA $MsgHeartbeat 7 ([byte[]](0, 0, 0, 0))
    Start-Sleep -Milliseconds 400   # ReserveAck 처리(뮤텍스 안 set_draining)가 다음 로그인보다 먼저 끝나게

    $cliS3b = Connect-Tcp $SessionClientPort
    $stS3b = $cliS3b.GetStream()
    Send-ClientFrame $stS3b $CliSessionLoginReq (New-LoginBody $pS3b)
    $ackS3bR = Read-ClientFrame $stS3b 3000
    $ackS3b = if ($null -ne $ackS3bR -and $ackS3bR.MsgId -eq $CliSessionLoginAck) { ConvertFrom-LoginAckBody $ackS3bR.Body } else { $null }
    Add-Result 'S3(ReserveAck result=3 학습 → 다음 배정에서 제외)' `
        ($s3aPickedA -and $s3ReserveSeen -and ($null -ne $ackS3b) -and ($ackS3b.Result -eq 0) -and ($ackS3b.Port -eq 7802)) `
        ("1차 로그인 A 선택=$s3aPickedA(port=$(if ($ackS3a) { $ackS3a.Port } else { '-' })) · A 의 Reserve 수신=$s3ReserveSeen → result=3 회신 · 2차 로그인 result=" `
            + $(if ($ackS3b) { $ackS3b.Result } else { '(무응답)' }) + " port=" + $(if ($ackS3b) { $ackS3b.Port } else { '-' }) + "(기대 result=0 port=7802 — E 로 넘어갔다)")
    try { $cliS3b.Close() } catch {}

    # ── [SESS ] pending sent 무증가 — 드레인 명령(S1·S2·S4)이 pending 을
    #   안 태운다는 실증. S3 는 의도적으로 Reserve 왕복을 냈으므로 그 항은
    #   빼고, "드레인 콘솔 자체"가 낸 신규 pending 이 0 인지만 본다. 로그인
    #   4회(S1·S2·S4·S3a·S3b = 5회)가 낸 정상 Reserve 발신은 이 단언의 대상이
    #   아니다 — sent 카운터 자체가 아니라 "drain/undrain 호출이 그 카운터를
    #   추가로 올렸는가"를 보려는 것이므로, SetMode 발신 3회(S1·S2·S4)가
    #   반영 안 됐는지를 로그 문구로 대조한다(sent 는 Reserve 전용이라
    #   SetMode 가 늘려도 이 값 자체는 그대로다 — 즉 이 단언은 "늘지 않았다"
    #   가 아니라 "SetMode 가 이 카운터에 안 잡힌다"를 코드로 보여준다).
    try { $fakeDA.Close() } catch {}
    try { $fakeDE.Close() } catch {}
    try {
        if (-not $sDrain.HasExited) {
            $sDrain.HarnessStdin.WriteLine('')
            $sDrain.HarnessStdin.Flush()
            $sDrain.HarnessStdin.Close()
            if (-not $sDrain.WaitForExit(5000)) { $sDrain.Kill() }
        }
    } catch {
        try { if (-not $sDrain.HasExited) { $sDrain.Kill() } } catch {}
    }
    Start-Sleep -Milliseconds 300
    $drainFinalLog = Read-LogText $sDrainLog
    $mPending = [regex]::Match($drainFinalLog, '\[SESS \] pending sent=(\d+) send_fail=(\d+) ack=(\d+) rejected=(\d+) unsupported=(\d+) timeout=(\d+) link_down=(\d+) stop=(\d+) skip_nolink=(\d+) remain=(\d+)')
    $pendingSent = if ($mPending.Success) { [int]$mPending.Groups[1].Value } else { -1 }
    # 로그인은 이 페이즈에서 5 회(S1·S2·S4·S3a·S3b) 났다 — 배정이 A 로 갔든
    # E 로 갔든 매 성공 로그인마다 Reserve 발신 1건이 sent 를 올린다(링크
    # 부재 스킵 갈래는 여기서 안 탄다 — A·E 둘 다 링크가 있다). S1 만 결과가
    # no_server 라 assign() 이 스킵돼 Reserve 발신 자체가 없다 — 그래서
    # 기대값은 5 가 아니라 4(S2·S4·S3a·S3b)다. 드레인 명령 3회(S1·S2·S4 의
    # drain/undrain/drain)가 여기에 하나라도 얹혔다면 sent 가 5 이상으로
    # 나온다 — 그러면 이 단언이 그 오염을 잡는다.
    Add-Result '[SESS ] pending sent 드레인 무증가(하네스 로그 인용)' `
        ($mPending.Success -and ($pendingSent -eq 4)) `
        ("$($mPending.Value) (기대 sent=4 — 성공 로그인 4건(S2·S4·S3a·S3b)의 Reserve 발신만. S1 은 no_server 라 Reserve 자체가 없다. drain/undrain 3회는 여기 안 얹혔다)")

    # ═══════════════════════════════════════════════════════════════════════
    #  Phase R — 재연결 FullSync 대조 Kick. FullSync 에 타서버 소유 player 가
    #  실려 오면 테이블을 덮어쓰지 않고(소유 유지) 발신 마을에 Kick 으로 진위를
    #  묻는다 — 그 대조·NotFound 오삭제 방지·first_chunk 술어·집계를 잰다.
    #  ⛔ 별도 인스턴스인 이유: K11·P6-16 집계가 프로세스 전체 런의 종료 요약
    #  절대값을 봐서 기존 인스턴스(6a·6d)에 얹으면 배치 순서와 무관하게
    #  오염된다 — P6-15 의 sess15 별도 인스턴스 패턴을 재사용한다.
    # ═══════════════════════════════════════════════════════════════════════
    Start-Sleep -Milliseconds 700
    # request_timeout 을 넉넉히(5000ms) — 하네스가 KickAck 를 회신하기 전에
    # 스윕이 pending 을 타임아웃으로 회수하면 acked/not_found 가 timeout 으로
    # 새서 R5 절대값이 흔들린다(위 드레인 콘솔 페이즈가 실측으로 걸린 그 경주).
    # R6 의 remain=2 는 「orphan 전이는 assign() 안에서만 lazy 하게 일어난다」에
    # 기대고 있다 — 이 페이즈의 유일한 Login(R3)이 busy 갈래에서 끝나 assign 이
    # 한 번도 안 불리므로 하트비트 침묵에도 항목이 안 지워진다. 이 페이즈에
    # assign 에 닿는 로그인을 추가하면 그 전제가 깨져 remain 이 조용히 갈린다.
    $sessRHome = New-SessionHome 'sessR' '16' '5000'
    $sessRLog = Join-Path $sessRHome 'logs\server.log'
    $sessR = Start-ServerProcess $sessionExe $sessRHome '--seconds 12'
    $procs.Add($sessR)
    if ($null -eq (Wait-LogMatch $sessRLog 'session server up' 8000)) {
        throw 'phase R: sessR 기동 로그가 없다'
    }
    $pidRX = 98001; $pidRY = 98002; $pidRZ = 98003

    # V1 = 재연결 마을 역할(FullSync 발신자) · V2 = 타서버 역할(X 의 소유자).
    $fakeRV1 = Connect-Tcp $SessionS2sPort
    $fsRV1 = $fakeRV1.GetStream()
    Send-S2sFrame $fsRV1 $MsgRegister 1 (New-RegisterBody $VerOk 7901 10 0 '127.0.0.1')
    Read-S2sFrame $fsRV1 3000 | Out-Null
    $fakeRV2 = Connect-Tcp $SessionS2sPort
    $fsRV2 = $fakeRV2.GetStream()
    Send-S2sFrame $fsRV2 $MsgRegister 1 (New-RegisterBody $VerOk 7902 10 0 '127.0.0.2')
    Read-S2sFrame $fsRV2 3000 | Out-Null

    # R1 — V2 가 X 를 소유한 상태에서 V1 의 FullSync 에 X 가 실려 오면 V1 소켓에
    # Kick{X} + 요약 로그 kicked=1(스킵 카운터 — 소유자 잔존의 직접 증거는 R3·R5).
    Send-S2sFrame $fsRV2 $MsgPlayerEnter 0 (New-LoginBody $pidRX)
    Start-Sleep -Milliseconds 300
    $ckR1 = Get-LogLength $sessRLog
    $fsBodyR1 = New-Object System.Collections.Generic.List[byte]
    $fsBodyR1.AddRange([byte[]](0, 0, 0, 1, 0, 1))          # chunk_idx=0 · chunk_total=1 · count=1
    $fsBodyR1.AddRange((New-LoginBody $pidRX))
    Send-S2sFrame $fsRV1 $MsgFullSync 0 $fsBodyR1.ToArray()
    $skippedR1 = New-Object System.Collections.Generic.List[int]
    $kickR1 = Wait-S2sFrame $fsRV1 $MsgKick 4000 $skippedR1
    $kickR1Body = if ($null -ne $kickR1) { ConvertFrom-KickBody $kickR1.Body } else { $null }
    $mR1sum = Wait-LogMatch $sessRLog 'full-sync server_id=\d+ chunk=1/1 count=1 kicked=1' 3000 $ckR1
    $mR1kick = Wait-LogMatch $sessRLog ('full-sync 대조 kick player=' + $pidRX) 3000 $ckR1
    Add-Result 'R1(대조 Kick 발신)' (($null -ne $kickR1Body) -and ($kickR1Body.PlayerId -eq $pidRX) `
        -and ($kickR1Body.Reason -eq 0) `
        -and ($null -ne $mR1sum) -and ($null -ne $mR1kick)) `
        ("V1 소켓에 Kick 수신 player=$(if ($kickR1Body) { $kickR1Body.PlayerId } else { '-' })(기대 $pidRX) · reason=$(if ($kickR1Body) { $kickR1Body.Reason } else { '-' })(기대 0=kKickReasonDuplicate) · 요약 kicked=1 로그=" + ($null -ne $mR1sum) + " · 대조 kick 로그=" + ($null -ne $mR1kick))

    # R1b — R1 의 KickAck 회신 전(pending 생존 중)에 같은 충돌 FullSync 를
    # 재전송하면 스킵 카운터(kicked=1)는 다시 잡히되 새 Kick 발신은 억제된다
    # (has_pending_kick — login 경로와 같은 헬퍼). 발신이 안 늘었다는 집계
    # 증거는 R5 의 sent=3 절대값이다 — 억제가 죽으면 sent=4 로 갈린다.
    $ckR1b = Get-LogLength $sessRLog
    Send-S2sFrame $fsRV1 $MsgFullSync 0 $fsBodyR1.ToArray()   # R1 과 같은 본문 재전송
    $mR1bsum = Wait-LogMatch $sessRLog 'full-sync server_id=\d+ chunk=1/1 count=1 kicked=1' 3000 $ckR1b
    $skippedR1b = New-Object System.Collections.Generic.List[int]
    $kickR1b = Wait-S2sFrame $fsRV1 $MsgKick 1000 $skippedR1b
    Add-Result 'R1b(pending 중 재전송 억제)' (($null -ne $mR1bsum) -and ($null -eq $kickR1b)) `
        ("재전송 후 kicked=1 로그 재매칭(체크포인트 이후 신규분)=" + ($null -ne $mR1bsum) + " · 새 Kick 프레임 없음=" + ($null -eq $kickR1b) + " (sent 불변은 R5 의 sent=3 절대값이 판정)")

    # R2 — 자기 소유(Y)·신규(Z)만 실린 FullSync 는 정상 교체 — Kick 0건·kicked=0.
    Send-S2sFrame $fsRV1 $MsgHeartbeat 2 ([byte[]](0, 0, 0, 0))
    Send-S2sFrame $fsRV2 $MsgHeartbeat 2 ([byte[]](0, 0, 0, 0))
    Send-S2sFrame $fsRV1 $MsgPlayerEnter 0 (New-LoginBody $pidRY)
    Start-Sleep -Milliseconds 200
    $ckR2 = Get-LogLength $sessRLog
    $fsBodyR2 = New-Object System.Collections.Generic.List[byte]
    $fsBodyR2.AddRange([byte[]](0, 0, 0, 1, 0, 2))
    $fsBodyR2.AddRange((New-LoginBody $pidRY))
    $fsBodyR2.AddRange((New-LoginBody $pidRZ))
    Send-S2sFrame $fsRV1 $MsgFullSync 0 $fsBodyR2.ToArray()
    $mR2sum = Wait-LogMatch $sessRLog 'full-sync server_id=\d+ chunk=1/1 count=2 kicked=0' 3000 $ckR2
    $skippedR2 = New-Object System.Collections.Generic.List[int]
    $kickR2 = Wait-S2sFrame $fsRV1 $MsgKick 800 $skippedR2
    Add-Result 'R2(자기 소유·신규 정상 교체)' (($null -ne $mR2sum) -and ($null -eq $kickR2)) `
        ("count=2 kicked=0 로그=" + ($null -ne $mR2sum) + " · Kick 없음=" + ($null -eq $kickR2) + " (Y=자기 소유 재기재·Z=신규 — 둘 다 충돌 아님)")

    # R3 — R1 의 Kick 에 NotFound(1) 회신. pending 의 server_id 가 발신 링크(V1)
    # 라 소유자 대조가 막아 X=V2 항목이 잔존한다 — 직후 Login(X) 가 Busy(2)이고
    # 그 Kick 이 소유자 링크(V2)로 가는 것이 그 증거다. 오삭제 회귀면 항목이
    # 지워져 Login 이 배정 성공(0)으로 갈린다.
    if ($null -ne $kickR1) {
        Send-S2sFrame $fsRV1 $MsgKickAck $kickR1.Seq ([byte[]]@(1))   # NotFound
    }
    Start-Sleep -Milliseconds 300
    Send-S2sFrame $fsRV2 $MsgHeartbeat 3 ([byte[]](0, 0, 0, 0))
    $cliR3 = Connect-Tcp $SessionClientPort
    $stR3 = $cliR3.GetStream()
    Send-ClientFrame $stR3 $CliSessionLoginReq (New-LoginBody $pidRX)
    $ackR3r = Read-ClientFrame $stR3 3000
    $ackR3 = if ($null -ne $ackR3r -and $ackR3r.MsgId -eq $CliSessionLoginAck) { ConvertFrom-LoginAckBody $ackR3r.Body } else { $null }
    try { $cliR3.Close() } catch {}
    $skippedR3 = New-Object System.Collections.Generic.List[int]
    $kickR3 = Wait-S2sFrame $fsRV2 $MsgKick 4000 $skippedR3
    $kickR3Body = if ($null -ne $kickR3) { ConvertFrom-KickBody $kickR3.Body } else { $null }
    if ($null -ne $kickR3) {
        Send-S2sFrame $fsRV2 $MsgKickAck $kickR3.Seq ([byte[]]@(0))   # Kicked — R5 집계를 결정적으로
    }
    Add-Result 'R3(NotFound 오삭제 방지)' (($null -ne $ackR3) -and ($ackR3.Result -eq 2) `
        -and ($null -ne $kickR3Body) -and ($kickR3Body.PlayerId -eq $pidRX)) `
        ("NotFound 회신 뒤 Login(X) result=" + $(if ($ackR3) { $ackR3.Result } else { '(무응답)' }) + "(기대 2=Busy — X=V2 항목 잔존) · 그 Kick 이 소유자 V2 로 수신됨=" + ($null -ne $kickR3Body))

    # R4 — 충돌 pid 를 2번째 청크에 실어도 같은 발화 — first_chunk 술어 회귀
    # 게이트(P6-15 partial-chunk 기법 재사용 — 단 여기는 두 청크를 다 보낸다).
    Send-S2sFrame $fsRV1 $MsgHeartbeat 3 ([byte[]](0, 0, 0, 0))
    $ckR4 = Get-LogLength $sessRLog
    $fsBodyR4a = New-Object System.Collections.Generic.List[byte]
    $fsBodyR4a.AddRange([byte[]](0, 0, 0, 2, 0, 1))         # chunk_idx=0 · chunk_total=2 · count=1
    $fsBodyR4a.AddRange((New-LoginBody $pidRY))
    Send-S2sFrame $fsRV1 $MsgFullSync 0 $fsBodyR4a.ToArray()
    $fsBodyR4b = New-Object System.Collections.Generic.List[byte]
    $fsBodyR4b.AddRange([byte[]](0, 1, 0, 2, 0, 1))         # chunk_idx=1 · chunk_total=2 · count=1
    $fsBodyR4b.AddRange((New-LoginBody $pidRX))
    Send-S2sFrame $fsRV1 $MsgFullSync 0 $fsBodyR4b.ToArray()
    $skippedR4 = New-Object System.Collections.Generic.List[int]
    $kickR4 = Wait-S2sFrame $fsRV1 $MsgKick 4000 $skippedR4
    $kickR4Body = if ($null -ne $kickR4) { ConvertFrom-KickBody $kickR4.Body } else { $null }
    $mR4sum = Wait-LogMatch $sessRLog 'full-sync server_id=\d+ chunk=2/2 count=1 kicked=1' 3000 $ckR4
    if ($null -ne $kickR4) {
        Send-S2sFrame $fsRV1 $MsgKickAck $kickR4.Seq ([byte[]]@(0))   # R5 집계를 결정적으로
    }
    Add-Result 'R4(2번째 청크 충돌도 발화)' (($null -ne $kickR4Body) -and ($kickR4Body.PlayerId -eq $pidRX) -and ($null -ne $mR4sum)) `
        ("2청크 FullSync(1청크=Y·2청크=X) -> Kick 수신 player=$(if ($kickR4Body) { $kickR4Body.PlayerId } else { '-' })(기대 $pidRX) · chunk=2/2 kicked=1 로그=" + ($null -ne $mR4sum))

    # R5 — 종료 요약 절대값 + 항등식. 조립 결과로 확정한 기대값: sent=3(R1 대조
    # + R3 login-busy + R4 대조) · acked=2(R3 의 V2 회신 0 + R4 의 V1 회신 0) ·
    # not_found=1(R3 의 R1 회신 1) · timeout/link_down/stop=0(전부 회신됨) ·
    # login_busy=1(R3). 집계 축의 오삭제·이중 가산 회귀 게이트다.
    $sessR.WaitForExit(20000) | Out-Null
    if (-not $sessR.HasExited) { $sessR.Kill() }
    $sRText = Read-LogText $sessRLog
    $sRKick = [regex]::Match($sRText,
        '\[SESS \] kick sent=(\d+) acked=(\d+) not_found=(\d+) timeout=(\d+) link_down=(\d+) stop=(\d+) \| login_busy=(\d+)')
    if ($sRKick.Success) {
        $rSent = [int]$sRKick.Groups[1].Value; $rAcked = [int]$sRKick.Groups[2].Value
        $rNotFound = [int]$sRKick.Groups[3].Value; $rTimeout = [int]$sRKick.Groups[4].Value
        $rLinkDown = [int]$sRKick.Groups[5].Value; $rStop = [int]$sRKick.Groups[6].Value
        $rBusy = [int]$sRKick.Groups[7].Value
        $rIdentity = $rSent -eq ($rAcked + $rNotFound + $rTimeout + $rLinkDown + $rStop)
        Add-Result 'R5(kick 요약 절대값)' ($rIdentity -and ($rSent -eq 3) -and ($rAcked -eq 2) -and ($rNotFound -eq 1) `
            -and ($rTimeout -eq 0) -and ($rLinkDown -eq 0) -and ($rStop -eq 0) -and ($rBusy -eq 1)) `
            ("[SESS ] kick sent=$rSent=$rAcked+$rNotFound+$rTimeout+$rLinkDown+$rStop(항등식 $rIdentity) login_busy=$rBusy (기대 sent=3·acked=2·not_found=1·timeout=0·link_down=0·stop=0·login_busy=1)")
    } else {
        Add-Result 'R5(kick 요약 절대값)' $false 'phase R [SESS ] kick 요약 줄을 못 찾았다'
    }

    # R6 — 접속 테이블 종료 요약 절대값 — 「충돌 원소는 connections_added 미가산」
    # 계약의 유일한 판정이다(가산 뮤턴트면 added 가 갈린다). 조립 흐름 손계산:
    #   added=5  = X(R1 의 V2 PlayerEnter) + Y(R2 의 V1 PlayerEnter)
    #              + Y·Z(R2 FullSync 재기재) + Y(R4 1청크 재기재).
    #              충돌 스킵 3건(R1·R1b·R4 2청크의 X)은 미가산 — 가산 뮤턴트면 8.
    #   removed=3 = Y(R2 first_chunk 클리어) + Y·Z(R4 first_chunk 클리어)
    #   remain=2  = X(V2 소유 잔존 — R3 오삭제 방지의 집계 재확인) + Y(V1 소유)
    #   fullsync_replaced=4 = first_chunk 4회(R1·R1b·R2·R4 1청크) — 이 페이즈의
    #              FullSync 는 전부 하네스 발신이라 자동 발신 오염이 없어 == 로 건다.
    #   검산: added(5) = removed(3) + remain(2).
    $sRConn = [regex]::Match($sRText,
        '\[SESS \] connections added=(\d+) removed=(\d+) fullsync_replaced=(\d+) remain=(\d+)')
    if ($sRConn.Success) {
        $cAdded = [int]$sRConn.Groups[1].Value; $cRemoved = [int]$sRConn.Groups[2].Value
        $cReplaced = [int]$sRConn.Groups[3].Value; $cRemain = [int]$sRConn.Groups[4].Value
        Add-Result 'R6(connections 절대값)' (($cAdded -eq 5) -and ($cRemoved -eq 3) -and ($cReplaced -eq 4) -and ($cRemain -eq 2)) `
            ("[SESS ] connections added=$cAdded(기대 5 — 충돌 스킵 3건 미가산, 가산 뮤턴트면 8) removed=$cRemoved(기대 3) fullsync_replaced=$cReplaced(기대 4) remain=$cRemain(기대 2 — X·Y)")
    } else {
        Add-Result 'R6(connections 절대값)' $false 'phase R [SESS ] connections 요약 줄을 못 찾았다'
    }
    try { $fakeRV1.Close() } catch {}
    try { $fakeRV2.Close() } catch {}
}
finally {
    foreach ($p in $procs) {
        try { if ($p -and -not $p.HasExited) { $p.Kill() } } catch {}
    }
    # stdout 드레인 이벤트를 먼저 내린다 — 스크래치 삭제 뒤에 늦게 발화하면
    # "경로를 못 찾는다" 소음을 낸다(판정 무관·정리 순서 문제).
    try { Get-EventSubscriber | Unregister-Event -ErrorAction SilentlyContinue } catch {}
    Start-Sleep -Milliseconds 300
    try { Remove-Item -Recurse -Force -LiteralPath $scratchRoot -ErrorAction SilentlyContinue } catch {}
}

# ── 요약 ─────────────────────────────────────────────────────────────────────
Write-Host ''
Write-Host '==================== 요약 ===================='
$failCount = 0
$skipCount = 0
foreach ($r in $script:MatrixResults) {
    if ($r.Status -eq 'SKIP') { $skipCount++ }
    elseif (-not $r.Pass) { $failCount++ }
}
foreach ($r in $script:MatrixResults) {
    # $r.Pass 만 보면 SKIP 이 PASS 로 찍힌다 — Status 를 먼저 본다.
    $mark = if ($r.Status -eq 'SKIP') { 'SKIP' } elseif ($r.Pass) { 'PASS' } else { 'FAIL' }
    Write-Host ("{0,-16} {1}  {2}" -f $r.Id, $mark, $r.Detail)
}
Write-Host ''
if ($failCount -eq 0) {
    Write-Host "전체 통과 (총 $($script:MatrixResults.Count) 항목 · 그중 SKIP $skipCount)" -ForegroundColor Green
} else {
    Write-Host "$failCount 건 실패 (총 $($script:MatrixResults.Count) 항목 · 그중 SKIP $skipCount)" -ForegroundColor Red
}

# SKIP 은 실패가 아니다 — 종료 코드는 failCount 만 본다.
if ($failCount -gt 0) { exit 1 }
exit 0
