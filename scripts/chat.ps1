# scripts\chat.ps1 — 채팅 3종(Zone/All/Whisper) 회귀 (C1~C11)
#
#   무엇을 보는가
#     지역/전체/귓속말 3갈래의 정상·경계값·실패 시 불변·수명 정리·워커 경합을
#     한 하네스에서 본다. zone.ps1 은 지역 채팅 회귀를 그대로 유지하므로
#     여기서는 전체·귓속말처럼 그 파일 구조로는 못 담는 시나리오를 더한다.
#
#   자체 스폰 B갈래 — harness_common.ps1 의 New-HarnessHome/Start-Village/
#     Stop-Harness 를 그대로 쓴다(스크래치 config·idle 0 오버라이드·정상 종료).
#     가짜 세션 서버로 예약 발급 후 Enter 하는 것도 gate.ps1·zone.ps1 과 같은
#     패턴이다(Connect-Reserved 재사용).
#
#   사용:
#     .\chat.ps1
#     .\chat.ps1 -Config ASan

param(
    [int]$Port      = 9000,
    [string]$Config = 'Release',
    [int]$Timeout   = 5000,
    [int]$Seconds   = 180
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'harness_common.ps1')

# ── 이 파일만의 와이어 리터럴 — proto include 가 안 돼 PS 사본이다 ──────────
#   값이 바뀌면 find_copies 대상이다(packet.h 의 kChatReq/kChatNtf/kChatAck
#   재정의를 다른 걸로 또 바꾸면 여기도 손으로 맞춰야 한다).
#     kChatReq(3)  body: [ type:u8 ][ target:u64 BE — type=2(Whisper) 만 ][ text... ]
#     kChatNtf(103) body: [ type:u8 ][ from:u64 BE ][ text... ]
#     kChatAck(115) body: [ result:u8 ]
$Chat_MsgChatReq     = 3
$Chat_MsgChatNtf     = 103
$Chat_MsgChatAck     = 115
$Chat_MsgJoinZoneReq = 2      # proto::MsgId::kJoinZoneReq   body: [ zone_id : u32 ]
$Chat_MsgJoinZoneAck = 102
$Chat_ResultNoPeer   = 6      # proto::ResultCode::kNoPeer

function New-U32BE([uint32]$Value) {
    $b = [byte[]]::new(4)
    $b[0] = [byte](($Value -shr 24) -band 0xFF); $b[1] = [byte](($Value -shr 16) -band 0xFF)
    $b[2] = [byte](($Value -shr 8)  -band 0xFF); $b[3] = [byte]( $Value          -band 0xFF)
    return ,$b
}

# type=2(Whisper) 만 target 필드를 싣는다 — 요청 오버헤드가 type 마다
#   다르다는 것이 서버 §6-3 그대로다(D1).
function New-ChatReqBody([byte]$Type, [uint64]$Target, [byte[]]$Text) {
    if ($Type -eq 2) {
        $b = [byte[]]::new(9 + $Text.Length)
        $b[0] = $Type
        for ($i = 0; $i -lt 8; $i++) { $b[1 + $i] = [byte](($Target -shr (8 * (7 - $i))) -band 0xFF) }
        if ($Text.Length -gt 0) { [Array]::Copy($Text, 0, $b, 9, $Text.Length) }
        return ,$b
    }
    $b = [byte[]]::new(1 + $Text.Length)
    $b[0] = $Type
    if ($Text.Length -gt 0) { [Array]::Copy($Text, 0, $b, 1, $Text.Length) }
    return ,$b
}

function ConvertFrom-ChatNtfBody([byte[]]$Body) {
    $type = [int]$Body[0]
    $from = [uint64]0
    for ($i = 0; $i -lt 8; $i++) { $from = ($from -shl 8) -bor [uint64]$Body[1 + $i] }
    $text = if ($Body.Length -gt 9) { $Body[9..($Body.Length - 1)] } else { [byte[]]::new(0) }
    [pscustomobject]@{ Type = $type; From = $from; Text = $text }
}

function ConvertFrom-ChatAckBody([byte[]]$Body) {
    [pscustomobject]@{ Result = [int]$Body[0] }
}

function Bytes-Equal([byte[]]$A, [byte[]]$B) {
    if ($A.Length -ne $B.Length) { return $false }
    for ($i = 0; $i -lt $A.Length; $i++) { if ($A[$i] -ne $B[$i]) { return $false } }
    return $true
}

# 큐에 남은 프레임을 전부 걷어낸다 — SettleMs 안에 새 프레임이 안 오면 끝.
#   존 입장 직후의 JoinZoneAck/ZoneMembersNtf 잔여물, 이전 시나리오가 남긴
#   찌꺼기를 다음 assert 전에 비우는 용도다.
function Drain($Stream, [int]$SettleMs = 300) {
    while ($true) {
        $f = Read-ClientFrame $Stream $SettleMs
        if ($null -eq $f) { break }
    }
}

# Drain 과 같지만 SettleMs 안에 걷어낸 것 중 WantMsgId 개수를 센다 —
#   "정상 개수만큼 받았다"와 "그 이상은 안 온다"는 다른 주장이다. 이중
#   발송 뮤턴트는 개수 판정만으로는 안 잡히고(먼저 온 것만 보고 통과),
#   settle 뒤 잔여가 0인지까지 봐야 잡힌다(C1/C2 초과 수신 검사).
function Drain-CountMsg($Stream, [int]$WantMsgId, [int]$SettleMs = 300) {
    $count = 0
    while ($true) {
        $f = Read-ClientFrame $Stream $SettleMs
        if ($null -eq $f) { break }
        if ($f.MsgId -eq $WantMsgId) { $count++ }
    }
    return $count
}

# 원하는 MsgId 가 올 때까지 다른 프레임(ZoneMembersNtf 등)은 버리며 기다린다 —
#   Wait-S2sFrame 과 같은 형태다.
function Wait-Frame($Stream, [int]$WantMsgId, [int]$TotalTimeoutMs) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $TotalTimeoutMs) {
        $remain = $TotalTimeoutMs - [int]$sw.ElapsedMilliseconds
        if ($remain -le 0) { break }
        $f = Read-ClientFrame $Stream ([Math]::Min(500, [Math]::Max(50, $remain)))
        if ($null -eq $f) { continue }
        if ($f.MsgId -eq $WantMsgId) { return $f }
    }
    return $null
}

# Socket.Poll(0, SelectRead) — 논블로킹으로 "지금 닫혀 있나"만 본다. 버스트
#   도중에도 스트림을 소비하지 않고 자주 불러도 된다(C9 가 이렇게 쓴다).
function Test-ClosedNow($ClientHandle) {
    $sock = $ClientHandle.Client
    if ($sock.Poll(0, [System.Net.Sockets.SelectMode]::SelectRead)) {
        return ($sock.Available -eq 0)
    }
    return $false
}

# 블로킹으로 "닫힐 때까지" 기다린다 — gate.ps1 의 Wait-ConnectionOutcome 과
#   같은 형태(폴링 간격만 다르다).
function Wait-Closed($ClientHandle, [int]$TimeoutMs) {
    $sock = $ClientHandle.Client
    $deadline = [datetime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([datetime]::UtcNow -lt $deadline) {
        if ($sock.Poll(20000, [System.Net.Sockets.SelectMode]::SelectRead)) {
            return ($sock.Available -eq 0)
        }
    }
    return $false
}

# gate.ps1 의 Read-ServerLog 와 같다 — FileShare.ReadWrite 로 열어야 아직
#   village 가 쓰고 있는 파일을 동시에 읽을 수 있다.
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

# Grant-Reservation + connect + kEnterReq 까지 한 번에 끝낸다
#   (harness_common.Connect-Reserved 그대로 — 실패하면 throw).
function New-ChatClient($Link, [int]$ClientPort, [uint64]$PlayerId, [int]$TimeoutMs) {
    $r = Connect-Reserved $Link $ClientPort $PlayerId $TimeoutMs
    $r.Stream.ReadTimeout = $TimeoutMs
    $r.Stream.WriteTimeout = $TimeoutMs
    return $r
}

function Join-ChatZone($Client, [uint32]$Zone, [int]$TimeoutMs) {
    Send-ClientFrame $Client.Stream $Chat_MsgJoinZoneReq (New-U32BE $Zone)
    $ack = Wait-Frame $Client.Stream $Chat_MsgJoinZoneAck $TimeoutMs
    if ($null -eq $ack) { throw "JoinZoneAck 를 못 받았다 — player=$($Client.PlayerId) zone=$Zone" }
}

$scratchRoot = Join-Path $env:TEMP ("chat_harness_" + [guid]::NewGuid().ToString('N'))
$listener = $null
$villageProc = $null
try {
$vhome = New-HarnessHome $scratchRoot ([string]$Port)
$listener = Start-FakeSession 9100
$villageProc = Start-Village $Config $vhome $Seconds
$link = Accept-FakeSessionLink $listener

# ── C1b — All(1) 존 미가입: JoinZone 없이도 본인+기존 입장자 정상 수신 ─────
#   frame_router 의 handle_chat 주석 전제("All 은 미가입 갈래에 사실상
#   도달하지 않는다")와 snapshot_all 이 in_zone=false 인 세션도 포함한다는
#   사실을 실행으로 고정한다 — 예약→Enter 직후, JoinZoneReq 를 한 번도
#   보내지 않은 세션끼리도 All 채팅이 정상 왕복해야 한다.
Test-Name 'C1b — All(1) 존 미가입: JoinZone 없이도 본인+기존 입장자 정상 수신'
$c1bExisting = New-ChatClient $link $Port ([uint64]8901) $Timeout
Drain $c1bExisting.Stream 300
$c1bSelf = New-ChatClient $link $Port ([uint64]8902) $Timeout
Drain $c1bSelf.Stream 300

$c1bText = [System.Text.Encoding]::UTF8.GetBytes('ALL-C1B-NOZONE')
Send-ClientFrame $c1bSelf.Stream $Chat_MsgChatReq (New-ChatReqBody 1 0 $c1bText)

$fSelf = Wait-Frame $c1bSelf.Stream $Chat_MsgChatNtf 2000
$okSelf = $false
if ($null -ne $fSelf) {
    $n = ConvertFrom-ChatNtfBody $fSelf.Body
    $okSelf = ($n.Type -eq 1) -and ($n.From -eq $c1bSelf.PlayerId) -and (Bytes-Equal $n.Text $c1bText)
}
Note (Check "본인(존 미가입) 수신·본문 일치" $okSelf)

$fExisting = Wait-Frame $c1bExisting.Stream $Chat_MsgChatNtf 2000
$okExisting = $false
if ($null -ne $fExisting) {
    $n = ConvertFrom-ChatNtfBody $fExisting.Body
    $okExisting = ($n.Type -eq 1) -and ($n.From -eq $c1bSelf.PlayerId) -and (Bytes-Equal $n.Text $c1bText)
}
Note (Check "기존 입장자(존 미가입) 수신·본문 일치" $okExisting)

$c1bExisting.Client.Close()
$c1bSelf.Client.Close()

# ── C1 — 전체 채팅: 3존 분산 6명 전원 수신 · 본문 대조 ─────────────────────
Test-Name 'C1 — All(1) 전체 채팅: 3존 분산 전원 수신·본문 대조'
$broadcastGroup = @()
for ($i = 0; $i -lt 6; $i++) {
    $playerId = [uint64](9000 + $i)
    $zone = [uint32](750 + ($i % 3))
    $c = New-ChatClient $link $Port $playerId $Timeout
    Join-ChatZone $c $zone $Timeout
    $broadcastGroup += [pscustomobject]@{ C = $c; Zone = $zone }
}
foreach ($g in $broadcastGroup) { Drain $g.C.Stream 400 }   # 입장 직후 ZoneMembersNtf 등 잔여물 제거

$c1Text = [System.Text.Encoding]::UTF8.GetBytes('ALL-C1-BROADCAST')
Send-ClientFrame $broadcastGroup[0].C.Stream $Chat_MsgChatReq (New-ChatReqBody 1 0 $c1Text)

$idx = 0
foreach ($g in $broadcastGroup) {
    $f = Wait-Frame $g.C.Stream $Chat_MsgChatNtf 2000
    $ok = $false
    if ($null -ne $f) {
        $n = ConvertFrom-ChatNtfBody $f.Body
        $ok = ($n.Type -eq 1) -and ($n.From -eq $broadcastGroup[0].C.PlayerId) -and (Bytes-Equal $n.Text $c1Text)
    }
    Note (Check "client $idx (zone $($g.Zone)) All 수신·type/from/본문 일치" $ok)
    $idx++
}

# 초과 수신 검사(이중 발송 뮤턴트 방어) — 개수 판정은 "먼저 온 것"만
#   보고 통과하므로, settle 후에도 ChatNtf 가 더 안 오는지 따로 본다.
$idx = 0
foreach ($g in $broadcastGroup) {
    $extra = Drain-CountMsg $g.C.Stream $Chat_MsgChatNtf 500
    Note (Check "client $idx 잔여 ChatNtf 0(초과 수신 없음)" ($extra -eq 0))
    $idx++
}

# ── C2 — 지역 채팅: 같은 존만 수신, 교차 미수신 ────────────────────────────
Test-Name 'C2 — Zone(0) 지역 채팅: 같은 존만 수신, 다른 존 미수신'
foreach ($g in $broadcastGroup) { Drain $g.C.Stream 400 }

$c2Text = [System.Text.Encoding]::UTF8.GetBytes('ZONE-C2-LOCAL')
$senderZone = $broadcastGroup[0].Zone
Send-ClientFrame $broadcastGroup[0].C.Stream $Chat_MsgChatReq (New-ChatReqBody 0 0 $c2Text)

$idx = 0
foreach ($g in $broadcastGroup) {
    $sameZone = ($g.Zone -eq $senderZone)
    if ($sameZone) {
        $f = Wait-Frame $g.C.Stream $Chat_MsgChatNtf 2000
        $ok = $false
        if ($null -ne $f) {
            $n = ConvertFrom-ChatNtfBody $f.Body
            $ok = ($n.Type -eq 0) -and ($n.From -eq $broadcastGroup[0].C.PlayerId) -and (Bytes-Equal $n.Text $c2Text)
        }
        Note (Check "client $idx (zone $($g.Zone) — 같은 존) 수신·본문 일치" $ok)
    } else {
        $f = Read-ClientFrame $g.C.Stream 700
        Note (Check "client $idx (zone $($g.Zone) — 다른 존) 미수신" ($null -eq $f))
    }
    $idx++
}

# 초과 수신 검사 — C1 과 같은 이유(이중 발송 뮤턴트 방어).
$idx = 0
foreach ($g in $broadcastGroup) {
    $extra = Drain-CountMsg $g.C.Stream $Chat_MsgChatNtf 500
    Note (Check "client $idx 잔여 ChatNtf 0(초과 수신 없음)" ($extra -eq 0))
    $idx++
}

# ── C3 — 귓속말 정상: 대상만 수신·from 대조·제3자 미수신 ───────────────────
Test-Name 'C3 — Whisper 정상: 대상만 수신 · from 대조 · 제3자 미수신'
$whA = New-ChatClient $link $Port ([uint64]9101) $Timeout
$whB = New-ChatClient $link $Port ([uint64]9102) $Timeout
$whC = New-ChatClient $link $Port ([uint64]9103) $Timeout
Drain $whA.Stream 300; Drain $whB.Stream 300; Drain $whC.Stream 300

$c3Text = [System.Text.Encoding]::UTF8.GetBytes('WHISPER-C3')
Send-ClientFrame $whA.Stream $Chat_MsgChatReq (New-ChatReqBody 2 $whB.PlayerId $c3Text)

$fB = Wait-Frame $whB.Stream $Chat_MsgChatNtf 2000
$okB = $false
if ($null -ne $fB) {
    $n = ConvertFrom-ChatNtfBody $fB.Body
    $okB = ($n.Type -eq 2) -and ($n.From -eq $whA.PlayerId) -and (Bytes-Equal $n.Text $c3Text)
}
Note (Check "대상(B) 수신·from=A·본문 일치" $okB)

$fC = Read-ClientFrame $whC.Stream 700
Note (Check "제3자(C) 미수신" ($null -eq $fC))

$fA = Read-ClientFrame $whA.Stream 700
Note (Check "송신자(A) 무응답(성공은 무응답)" ($null -eq $fA))

# ── C4 — 귓속말 자기 자신 ──────────────────────────────────────────────────
Test-Name 'C4 — Whisper 자기 자신: 막을 손해 없음(§18-7) — 자신 수신'
$whD = New-ChatClient $link $Port ([uint64]9201) $Timeout
Drain $whD.Stream 300

$c4Text = [System.Text.Encoding]::UTF8.GetBytes('SELF-WHISPER-C4')
Send-ClientFrame $whD.Stream $Chat_MsgChatReq (New-ChatReqBody 2 $whD.PlayerId $c4Text)
$fD = Wait-Frame $whD.Stream $Chat_MsgChatNtf 2000
$okD = $false
if ($null -ne $fD) {
    $n = ConvertFrom-ChatNtfBody $fD.Body
    $okD = ($n.Type -eq 2) -and ($n.From -eq $whD.PlayerId) -and (Bytes-Equal $n.Text $c4Text)
}
Note (Check "자기 귓속말도 자신에게 정상 도착" $okD)

# ── C5 — 귓속말 부재 대상: kChatAck(kNoPeer) · 연결 유지 ───────────────────
Test-Name 'C5 — Whisper 부재 대상: kChatAck{kNoPeer} 응답 · 연결 유지'
$whE = New-ChatClient $link $Port ([uint64]9202) $Timeout
Drain $whE.Stream 300

# 기대하는 실패 형태를 먼저 명확히 한다 — 성공 응답(무응답)이 아니라
#   kChatAck(115){result=kNoPeer(6)} 정확히 그 한 프레임이어야 한다. 그 뒤
#   연결이 살아 있는지는 kPingReq/kPongAck 왕복으로 별도 확인한다(D4 의
#   "기존 접속이 정상인가" 확인용과 같은 최소 도구 — harness_common 재사용).
Send-ClientFrame $whE.Stream $Chat_MsgChatReq (New-ChatReqBody 2 ([uint64]9999999) ([System.Text.Encoding]::UTF8.GetBytes('NOBODY-HOME')))
$fE = Wait-Frame $whE.Stream $Chat_MsgChatAck 2000
$okAck = $false
if ($null -ne $fE) {
    $ack = ConvertFrom-ChatAckBody $fE.Body
    $okAck = ($ack.Result -eq $Chat_ResultNoPeer)
}
Note (Check "kChatAck result=kNoPeer($Chat_ResultNoPeer)" $okAck)
Note (Check "연결 유지(ping 왕복 정상)" (Test-VilAlive $whE.Stream))

# ── C6 — 미정의 type(255) 절단 ─────────────────────────────────────────────
Test-Name 'C6 — 미정의 type(255): 프로토콜 위반 — 절단'
$whF = New-ChatClient $link $Port ([uint64]9301) $Timeout
Drain $whF.Stream 300
Send-ClientFrame $whF.Stream $Chat_MsgChatReq ([byte[]]@(255))
Note (Check "연결이 끊겼다" (Wait-Closed $whF.Client 2000))

# ── C7 — 로그인 후 빈 body(0B) 절단(OOB 방어 — type 읽기보다 길이 검증이 먼저) ──
Test-Name 'C7 — 로그인 후 빈 body(0B): OOB 방어 — 절단'
$whG = New-ChatClient $link $Port ([uint64]9302) $Timeout
Drain $whG.Stream 300
Send-ClientFrame $whG.Stream $Chat_MsgChatReq ([byte[]]::new(0))
Note (Check "연결이 끊겼다" (Wait-Closed $whG.Client 2000))

# ── C8 — type=2 · body_len 5B(target 잘림) 절단 ────────────────────────────
Test-Name 'C8 — type=2(Whisper) · body 5B(target 8B 중 4B만): 절단'
$whH = New-ChatClient $link $Port ([uint64]9303) $Timeout
Drain $whH.Stream 300
Send-ClientFrame $whH.Stream $Chat_MsgChatReq ([byte[]]@(2, 0, 0, 0, 0))
Note (Check "연결이 끊겼다" (Wait-Closed $whH.Client 2000))

# C9 전 격리 — C1~C5 가 만든 클라 전부를 명시적으로 닫는다. 안 닫으면 그
#   클라들도 아무도 안 드레인하는 "안 읽는 클라"로 남아 All(1) 폭탄에 함께
#   넘쳐 send_full_kicked 가 victim 하나보다 부풀어 오른다(1차 실행 실측
#   — send_full_kicked=12, C1~C5 의 방치된 연결이 전부 같이 킥됐다). 여기서
#   닫아야 아래 "정확히 1" 단언이 성립한다.
foreach ($g in $broadcastGroup) { $g.C.Client.Close() }
$whA.Client.Close(); $whB.Client.Close(); $whC.Client.Close()
$whD.Client.Close(); $whE.Client.Close()
Start-Sleep -Milliseconds 300   # 서버가 정상 종료(FIN)를 처리할 시간

# ── C9 — 안 읽는 클라 + 대량 전체 채팅 → 그 클라만 킥 ──────────────────────
Test-Name 'C9 — 큐 넘침 킥: 안 읽는 클라만 끊기고 send_full_kicked==1, 다른 클라 무영향'
$victim   = New-ChatClient $link $Port ([uint64]9401) $Timeout
$blaster  = New-ChatClient $link $Port ([uint64]9402) $Timeout
$bystander = New-ChatClient $link $Port ([uint64]9403) $Timeout
Drain $victim.Stream 300; Drain $blaster.Stream 300; Drain $bystander.Stream 300

# 기대하는 실패 형태를 먼저 명확히 한다 — victim 은 이 블록 안에서 단 한
#   번도 Stream.Read 를 하지 않는다(Test-ClosedNow 는 Poll 만 하고 소비하지
#   않는다). All(1) 은 자기 포함이라 blaster·bystander 도 이 방송을 도로
#   받는다 — 둘 다 안 비우면 victim 과 똑같이 "안 읽는 클라"가 되어 같이
#   넘친다(1차 실행 실측 — bystander 를 안 비웠더니 같이 끊겼다). 그래서
#   blaster·bystander 는 주기적으로 자기 큐를 비운다 — victim 만 예외다.
$maxMsgs = 3000
$textLen = 4087   # kMaxBodySize(4096) - 9 — Zone/All 릴레이 상한 그대로
$blastText = [byte[]]::new($textLen)
for ($k = 0; $k -lt $textLen; $k++) { $blastText[$k] = [byte](65 + ($k % 26)) }
$blastBody = New-ChatReqBody 1 0 $blastText

$victimClosed = $false
$sw = [System.Diagnostics.Stopwatch]::StartNew()
for ($k = 0; $k -lt $maxMsgs -and $sw.ElapsedMilliseconds -lt 20000; $k++) {
    Send-ClientFrame $blaster.Stream $Chat_MsgChatReq $blastBody
    if (($k % 15) -eq 14) { Drain $blaster.Stream 20; Drain $bystander.Stream 20 }
    if (($k % 50) -eq 49) {
        if (Test-ClosedNow $victim.Client) { $victimClosed = $true; break }
    }
}
if (-not $victimClosed) { $victimClosed = Wait-Closed $victim.Client 3000 }

if (-not $victimClosed) {
    Write-Host "  ! U3 미충족 — 1MB 송신 큐를 이번 실행에서 못 채웠다(중단하고 보고 — 이 하네스의 전제 조건)" -ForegroundColor Yellow
    Note (Check "victim 세션이 끊겼다 (U3 미충족)" $false)
} else {
    Note (Check "victim 세션이 끊겼다" $true)

    $liveLog = Read-ServerLog (Join-Path $vhome 'logs\server.log')
    $overflowMatched = $liveLog -match 'send queue overflow \d+ 회 연속'
    Note (Check "서버 로그에 'send queue overflow ... closing' 기록" $overflowMatched)
    if ($overflowMatched) { Write-Host "  ($($Matches[0]) — 보낸 메시지 $($k+1)개째)" }

    Drain $bystander.Stream 300
    Note (Check "다른 클라(bystander)는 에코 정상(ping 왕복)" (Test-VilAlive $bystander.Stream))
}

# ── C10 은 스크립트 말미(서버 정상 종료 직후)에서 이어서 본다 ──────────────

# ── C11 — 텍스트 상한 초과: Zone 릴레이 -9 재계산 경로 → 송신자 절단 ───────
Test-Name 'C11 — 텍스트 상한 초과(Zone): 릴레이 -9 재계산 경로 → 송신자 절단'
# Zone·All 이 같은 헬퍼·같은 검사(text_len > kMaxBodySize-9)를 타므로 한
#   시나리오로 양쪽을 대표한다(5단계 R1) — 여기서는 Zone(0)으로 대표한다.
$whL = New-ChatClient $link $Port ([uint64]9501) $Timeout
Join-ChatZone $whL 900 $Timeout
Drain $whL.Stream 400

$overLen = 4090   # kMaxBodySize(4096)-9=4087 을 넘는 값 — 요청 자체는
                  #   kMaxBodySize 이하(1+4090=4091<=4096)라 프레이밍은 통과하고
                  #   handle_chat 의 릴레이 재계산에서 걸린다.
$overText = [byte[]]::new($overLen)
for ($k = 0; $k -lt $overLen; $k++) { $overText[$k] = [byte](97 + ($k % 26)) }
Send-ClientFrame $whL.Stream $Chat_MsgChatReq (New-ChatReqBody 0 0 $overText)
Note (Check "송신자 연결이 끊겼다" (Wait-Closed $whL.Client 2000))

# ── 정상 종료 — Stop-Harness 가 스크래치(로그 포함)를 지우기 전에 먼저
#    로그를 읽어야 C10 을 볼 수 있다. 여기서 직접 stdin 개행으로 세우고,
#    finally 의 Stop-Harness 는 이미 끝난 프로세스에 대해 안전하게
#    no-op(정지부)+정리(삭제부)만 수행한다. ──────────────────────────────
if ($villageProc -and -not $villageProc.HasExited) {
    $villageProc.HarnessStdin.WriteLine('')
    $villageProc.HarnessStdin.Flush()
    $villageProc.HarnessStdin.Close()
    # ASan 은 종료 경로 자체가 느리다(전체 리포트 검사 오버헤드) — 넉넉히
    #   기다린다. Kill() 은 비동기라 그 뒤에도 한 번 더 WaitForExit 해야
    #   .ExitCode 를 읽을 수 있다(안 하면 "프로세스가 아직 안 끝났다"로
    #   빈 값이 된다 — 1차 ASan 회차 실측으로 확인).
    if (-not $villageProc.WaitForExit(15000)) {
        $villageProc.Kill()
        $villageProc.WaitForExit(5000) | Out-Null
    }
}
Start-Sleep -Milliseconds 300
$shutdownLog = Read-ServerLog (Join-Path $vhome 'logs\server.log')

# ── C9(계속) — 종료 로그로 수치를 확정한다 ─────────────────────────────
#   C9 격리(위) 뒤에는 victim 만 "안 읽는 클라"이므로 send_full_kicked 는
#   정확히 1 이어야 한다(>= 가 아니라 == 로 하드 단언 — blaster·bystander
#   는 15회마다 드레인해 큐가 60KB 안팎에서 유지되고 1MB 상한과는 자릿수가
#   달라 안전 마진이 크다). 3/4 수위 WARN 도 이 폭탄이 실제로 상한 근처까지
#   찼다는 증거로 같이 본다.
Test-Name 'C9(계속) — 종료 로그 수치 확정'
$sfkMatch = [regex]::Match($shutdownLog, 'send_full_kicked=(\d+)')
$sfk = if ($sfkMatch.Success) { [int]$sfkMatch.Groups[1].Value } else { -1 }
Note (Check "send_full_kicked == 1 (격리 후 victim 단독 — 실제 $sfk)" ($sfk -eq 1))
Note (Check "3/4 수위 [WARN] 로그 발생" ($shutdownLog -match '상한\(\d+B\)의 3/4 를 넘었다'))

# ASan 리포트·정상 종료 여부 — ASan 회차의 핵심 증거다. run-asan.ps1·s2s.ps1
#   과 같은 판정 방식이다(exit code + 리포트 무발화).
Test-Name '정상 종료 확인 (ASan 리포트 무발화 포함)'
# .ExitCode 는 안 본다 — Start-Village(harness_common.Start-ServerProcess)가
#   Win32 CreateProcess 로 띄운 뒤 GetProcessById 로 다시 찾은 Process 라,
#   .NET 이 Start() 로 직접 띄운 것과 달리 종료 코드 대기 내부 상태가 없어
#   HasExited=True 인데도 ExitCode 가 항상 비어 나온다(1차 ASan 회차 실측—
#   s2s.ps1 은 Process.Start() 를 직접 써서 이 문제가 없다). 그래서 이
#   하네스가 재사용하는 다른 자체 스폰 스크립트(zone·members·gate·drain·
#   idle)들도 전부 종료 코드 대신 종료 로그 내용으로만 판정한다 — 여기도
#   같은 근거로 로그 판정만 쓴다.
Note (Check 'village.exe 가 스스로 종료했다(HasExited)' $villageProc.HasExited)
Note (Check 'ASan/LeakSanitizer 리포트 무발화' (-not ($shutdownLog -match 'ERROR: (Address|Leak)Sanitizer')))

# ── C10 — 종료 지표 5종 파싱 ────────────────────────────────────────────
Test-Name 'C10 — 종료 지표 5종 파싱'
# ASan 구성은 core::alloc_counting() 이 false 라 [ALLOC] 줄 자체를 안 찍는다
#   (bootstrap.cpp — TESTING.md 의 "ASan 구성에서 [ALLOC] 숫자는 믿으면 안
#   된다"와 같은 이유). 그 구성에서는 줄이 없는 것 자체가 정상이라 대신
#   무발화를 확인한다.
if ($Config -eq 'ASan') {
    Note (Check '[ALLOC] 미발화(ASan 은 alloc_counting=false 라 정상)' (-not ($shutdownLog -match '\[ALLOC\]')))
} else {
    Note (Check '[ALLOC] 존재' ($shutdownLog -match '\[ALLOC\]'))
}
Note (Check '[POOL ] 존재' ($shutdownLog -match '\[POOL \]'))
Note (Check '[CONN ] 존재' ($shutdownLog -match '\[CONN \]'))
$netMatched = $shutdownLog -match '\[NET  \].*idle_kicked=\d+.*send_full_kicked=\d+'
Note (Check '[NET  ] 존재(idle_kicked·send_full_kicked 포함)' $netMatched)
if ($netMatched) { Write-Host "  ($($Matches[0]))" }
Note (Check '[TICK ] 존재' ($shutdownLog -match '\[TICK \]'))

Write-Host ''
Write-Host "판정  : $pass 개 통과 / $fail 개 실패"

} finally {
    Stop-Harness $scratchRoot $listener $villageProc
}
