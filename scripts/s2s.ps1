# scripts\s2s.ps1 — S2S 커넥터(1단계) 시나리오 하네스
#
#   village.exe 는 이 스크립트가 흉내 내는 "가짜 세션 서버"(TcpListener, 9100)에
#   connect 하는 쪽이다. 다른 하네스와 반대 방향이다 — 나머지는 village 에 접속하는
#   클라이언트를 흉내 내지만, 이건 village 가 접속해 오는 상대를 흉내 낸다.
#
#   가짜 세션 서버는 정적 msg_id→응답 매핑이 아니라 "연결 횟수·경과 시간·시나리오
#   진행 단계"에 의존하는 상태 기계다 — ⑥⑧⑨⑪ 이 전부 그렇다.
#
#   사용:
#     .\scripts\s2s.ps1                    # Release 로 전 항목 실행
#     .\scripts\s2s.ps1 -Config ASan        # ASan 구성(회차는 메인이 8단계에서 돌린다)

param(
    [string]$Config  = 'Release',
    [int]   $Seconds = 30,
    [int]   $SessionPort = 9100
)

$ErrorActionPreference = 'Stop'

# ── 상수 — proto::s2s::MsgId 와 값으로 맞춘다 (proto 를 이 스크립트가 include 할 수는
#    없으니 리터럴로 둔다 — 값이 바뀌면 find_copies.ps1 로 이 파일도 걸린다) ─────────
$MsgRegister      = 0x8001
$MsgUnregister    = 0x8002
$MsgHeartbeat     = 0x8003
$MsgFullSync      = 0x8006
$MsgKick          = 0x8102
$MsgReserve       = 0x8101
$MsgRegisterAck   = 0x8201
$MsgUnregisterAck = 0x8202
$MsgHeartbeatAck  = 0x8203
$MsgReserveAck    = 0x8301
$MsgKickAck       = 0x8302
$MsgUnsupported   = 0x8FFF
$MsgUnknownTest   = 0x8FF0   # 미정의 msg_id — 이 값 자체가 규약에 없다는 사실이 시나리오다

# ── ⑱ 전용 — 클라 프로토콜(packet.h) 리터럴 사본. 이 파일이 클라 역할을
#    하는 자리는 여기 하나뿐이다(나머지는 전부 세션 서버 역할) ────────────
$VilEnterReq = 13    # proto::MsgId::kEnterReq  — body: [ player_id : u64 ]
$VilEnterAck = 114   # proto::MsgId::kEnterAck  — body: [ result:u8 ][ player_id:u64 ][ session_id:u32 ]

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

# kEnterReq body: [ player_id : u64 ]
function New-EnterBody([uint64]$PlayerId) {
    $b = [byte[]]::new(8)
    for ($i = 0; $i -lt 8; $i++) { $b[$i] = [byte](($PlayerId -shr (8 * (7 - $i))) -band 0xFF) }
    return ,$b
}

$ResultOk               = 0
$ResultVersionRejected  = 1
$ResultFull             = 2

# ── 결과 수집 ────────────────────────────────────────────────────────────────
$script:MatrixResults = New-Object System.Collections.Generic.List[object]
$versionRejectCheckpoint = 0   # ⑨(오버라이드) 판정용 — 연결 #4 가 안 왔으면 0인 채로 남아 판정도 자연히 실패한다
$fullRejectCheckpoint    = 0   # ⑪(정원 거부) 판정용 — 같은 방식. 연결 #5 가 안 왔으면 0인 채로 남는다
$fullReconnectOk         = $false   # ⑪ 의 전반부(정원 거부 뒤 재접속·재Register) — 후반부(백오프 값)와 합쳐 한 항목으로 판정한다

function Add-Result([string]$Id, [bool]$Pass, [string]$Detail) {
    $mark = if ($Pass) { 'PASS' } else { 'FAIL' }
    Write-Host ("[MATRIX {0}] {1} — {2}" -f $Id, $mark, $Detail) -ForegroundColor $(if ($Pass) { 'Green' } else { 'Red' })
    $script:MatrixResults.Add([pscustomobject]@{ Id = $Id; Pass = $Pass; Detail = $Detail; Skip = $false })
}

# 검증 대상 자체(그 전제가 되는 상황)가 소멸한 항목용 — 실패로도 통과로도
#   세지 않는다. 조용히 지우면 「전체 통과」의 모집단이 줄어 무회귀 판정이
#   흐려지므로 SKIP 으로 따로 센다(trade.ps1·session.ps1 의 같은 신설과 짝이다).
function Add-Skip([string]$Id, [string]$Detail) {
    Write-Host ("[MATRIX {0}] SKIP — {1}" -f $Id, $Detail) -ForegroundColor Yellow
    $script:MatrixResults.Add([pscustomobject]@{ Id = $Id; Pass = $true; Detail = $Detail; Skip = $true })
}

# ── 8B 헤더 인코딩/디코딩 — proto::s2s::Header 와 값으로 맞춘다(빅엔디언) ──────────
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

function ConvertFrom-RegisterBody([byte[]]$Body) {
    $ver      = ([int]$Body[0] -shl 8) -bor [int]$Body[1]
    $port     = ([int]$Body[2] -shl 8) -bor [int]$Body[3]
    $capacity = ([uint32]$Body[4] -shl 24) -bor ([uint32]$Body[5] -shl 16) `
                -bor ([uint32]$Body[6] -shl 8) -bor [uint32]$Body[7]
    $current  = ([uint32]$Body[8] -shl 24) -bor ([uint32]$Body[9] -shl 16) `
                -bor ([uint32]$Body[10] -shl 8) -bor [uint32]$Body[11]
    $hostLen  = ([int]$Body[12] -shl 8) -bor [int]$Body[13]
    $hostName = [System.Text.Encoding]::UTF8.GetString($Body, 14, $hostLen)
    [pscustomobject]@{ Ver = $ver; Port = $port; Capacity = $capacity; Current = $current; Host = $hostName }
}

function ConvertFrom-FullSyncBody([byte[]]$Body) {
    $chunkIdx   = ([int]$Body[0] -shl 8) -bor [int]$Body[1]
    $chunkTotal = ([int]$Body[2] -shl 8) -bor [int]$Body[3]
    $count      = ([int]$Body[4] -shl 8) -bor [int]$Body[5]
    [pscustomobject]@{ ChunkIdx = $chunkIdx; ChunkTotal = $chunkTotal; Count = $count }
}

function New-RegisterAckBody([uint32]$ServerId, [byte]$Result) {
    $b = [byte[]]::new(5)
    $b[0] = [byte](($ServerId -shr 24) -band 0xFF)
    $b[1] = [byte](($ServerId -shr 16) -band 0xFF)
    $b[2] = [byte](($ServerId -shr 8)  -band 0xFF)
    $b[3] = [byte]( $ServerId          -band 0xFF)
    $b[4] = $Result
    return $b
}

# Reserve body: [ player_id : u64 ][ expire_ms : u32 ] (12B) — 항목6·7-a 용.
# ⑫(길이 위반)은 이 인코더 없이 11B 쓰레기로 만들어져 있다 — 이 함수는 유효한
# Reserve 를 보내야 하는 새 항목 전용이다.
function New-ReserveBody([uint64]$PlayerId, [uint32]$ExpireMs) {
    $b = [byte[]]::new(12)
    for ($i = 0; $i -lt 8; $i++) {
        $b[$i] = [byte](($PlayerId -shr (8 * (7 - $i))) -band 0xFF)
    }
    $b[8]  = [byte](($ExpireMs -shr 24) -band 0xFF); $b[9]  = [byte](($ExpireMs -shr 16) -band 0xFF)
    $b[10] = [byte](($ExpireMs -shr 8)  -band 0xFF); $b[11] = [byte]( $ExpireMs          -band 0xFF)
    return ,$b
}

# ReserveAck body: [ result : u8 ] (1B).
function ConvertFrom-ReserveAckBody([byte[]]$Body) {
    [pscustomobject]@{ Result = [int]$Body[0] }
}

# Kick body: [ player_id : u64 ][ reason : u8 ] (9B).
function New-KickBody([uint64]$PlayerId, [byte]$Reason) {
    $b = [byte[]]::new(9)
    for ($i = 0; $i -lt 8; $i++) {
        $b[$i] = [byte](($PlayerId -shr (8 * (7 - $i))) -band 0xFF)
    }
    $b[8] = $Reason
    return ,$b
}

# KickAck body: [ result : u8 ] (1B).
function ConvertFrom-KickAckBody([byte[]]$Body) {
    [pscustomobject]@{ Result = [int]$Body[0] }
}

# ── 소켓 I/O — 읽기는 전부 시간 상한을 받는다. 상대가 안 주면 영원히 안 막힌다 ──────
function Read-ExactBytes([System.Net.Sockets.NetworkStream]$Stream, [int]$Count, [int]$TimeoutMs) {
    # ⚠️ PowerShell 함정 — `return [byte[]]::new(0)` 처럼 그냥 배열을 return 하면
    #   파이프라인이 배열을 "풀어서" 내보내는데, 요소가 0개면 내보낼 것이 없어
    #   호출자는 빈 배열이 아니라 $null 을 받는다(실측으로 잡은 버그 — body_size=0
    #   인 프레임을 전부 "몸통을 못 읽었다"로 오판했다). 콤마 연산자로 배열 자체를
    #   "객체 하나"로 감싸야 언롤링이 안 걸린다.
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
            return $null                         # 시간 안에 안 왔다
        } else {
            Start-Sleep -Milliseconds 10
        }
    }
    return ,$buf   # 1바이트 본문(단일 요소 배열)도 스칼라로 풀리지 않게 콤마로 감싼다
}

function Read-S2sFrame($Stream, [int]$TimeoutMs) {
    $header = Read-ExactBytes $Stream 8 $TimeoutMs
    if ($null -eq $header) { return $null }
    $h = ConvertTo-S2sHeader $header
    $body = Read-ExactBytes $Stream $h.BodySize $TimeoutMs
    if ($null -eq $body) { return $null }
    return [pscustomobject]@{ MsgId = $h.MsgId; Seq = $h.Seq; Body = $body }
}

function Send-S2sFrame($Stream, [int]$MsgId, [uint32]$Seq, [byte[]]$Body) {
    $header = New-S2sHeader $Body.Length $MsgId $Seq
    $frame = $header + $Body
    $Stream.Write($frame, 0, $frame.Length)
    $Stream.Flush()
}

# 특정 msg_id 를 기다리되, 그 사이에 도착하는 Heartbeat 요청은 정상 ack 로 흘려보낸다.
#   village 는 heartbeat_ms 주기로 하트비트를 계속 보내므로(응답을 기다리지 않고
#   독립적으로), 우리가 다른 응답을 기다리는 동안에도 하트비트가 섞여 들어온다 —
#   이걸 무시하고 다음 프레임만 읽으면 엉뚱한 것을 "기대한 응답"으로 오판한다.
function Wait-ForFrame($Stream, [int]$ExpectedMsgId, [int]$TotalTimeoutMs) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $TotalTimeoutMs) {
        $remain = $TotalTimeoutMs - [int]$sw.ElapsedMilliseconds
        $frame = Read-S2sFrame $Stream ([Math]::Min(500, [Math]::Max(50, $remain)))
        if ($null -eq $frame) { continue }
        if ($frame.MsgId -eq $ExpectedMsgId) { return $frame }
        if ($frame.MsgId -eq $MsgHeartbeat) {
            Send-S2sFrame $Stream $MsgHeartbeatAck $frame.Seq ([byte[]]::new(0))
            continue
        }
        # 그 밖의 프레임은 이 대기의 관심사가 아니다 — 버리고 계속 기다린다.
    }
    return $null
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

# 리스너를 새로 만들 때(최초 기동 · ⑧ 재기동) SO_REUSEADDR 를 세우고 재시도한다 —
# 안 그러면 직전 소켓이 완전히 안 풀린 사이 재바인드가 "Only one usage of each
# socket address..." 로 실패할 수 있다(실측으로 걸림 — Windows 는 close 직후에도
# 짧게 자원을 붙들고 있을 수 있다).
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

# ── ini 사본 패치 — 키마다 정확히 1줄 치환 + 재읽기 대조 (조용한 실패 방지) ─────
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

# ═══════════════════════════════════════════════════════════════════════════
#  준비
# ═══════════════════════════════════════════════════════════════════════════

$root = Split-Path -Parent $PSScriptRoot
$exe  = Join-Path $root "build\x64\$Config\village.exe"
if (-not (Test-Path $exe)) {
    throw "$exe 가 없다 — 먼저 .\scripts\build.ps1 로 빌드하라"
}

Get-Process village -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300

if ($Config -eq 'ASan') {
    # run-asan.ps1 의 DLL PATH 선행 처리를 복제한다 — ASan DLL 을 못 찾으면 기동
    # 자체가 죽는다. ⚠️ run-asan.ps1 과 동기화돼야 한다 — 그 스크립트는 이번 수정
    # 상한 밖이라 공용 함수로 뽑아낼 수 없다(리터럴이 같은 동안은 find_copies.ps1
    # 이 잡아 준다. 탐색 로직 자체가 바뀌면 이 주석이 유일한 방어선이다).
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

$scratchRoot   = Join-Path $env:TEMP ("s2s_harness_" + [guid]::NewGuid().ToString('N'))
$scratchConfig = Join-Path $scratchRoot 'config'
New-Item -ItemType Directory -Path $scratchConfig -Force | Out-Null

$srcIni = Join-Path $root 'config\server.ini'
$dstIni = Join-Path $scratchConfig 'server.ini'
Copy-Item -LiteralPath $srcIni -Destination $dstIni

# ⛔ 두 단계 단언 — ① 키마다 치환 줄 수 1 ② 재읽기 값 대조. 조용한 치환 실패가
#   결함을 안고 진행되는 것을 막는다.
$patch = [ordered]@{
    host                 = '127.0.0.1'
    port                 = '9100'
    backoff_initial_ms   = '250'
    backoff_max_ms       = '1000'
    request_timeout_ms   = '1500'
    heartbeat_ms         = '500'
}
foreach ($k in $patch.Keys) {
    Set-IniKeyInSection $dstIni 's2s' $k $patch[$k]
}
foreach ($k in $patch.Keys) {
    Assert-IniValue $dstIni 's2s' $k $patch[$k]
}

# 커밋값(config/server.ini [net] idle_timeout_sec)이 90 으로 올라간 뒤에도
#   이 하네스는 ping 을 안 보낸다 — [net] 절이다([server] 아님).
$patchNet = [ordered]@{ idle_timeout_sec = '0' }
foreach ($k in $patchNet.Keys) {
    Set-IniKeyInSection $dstIni 'net' $k $patchNet[$k]
}
foreach ($k in $patchNet.Keys) {
    Assert-IniValue $dstIni 'net' $k $patchNet[$k]
}
Write-Host "config: 사본 패치 완료·재검증 통과 ($dstIni)"

$proc = $null
$exitOk = $false
$listener = $null

try {
    # ── village.exe 를 리스너보다 "먼저" 기동한다(⑧ 백오프 수열 pre-phase) ──────
    #
    # 127.0.0.1:9100 에 아직 아무도 안 듣고 있으므로 village 의 첫 ConnectEx 는
    # loopback 에서 즉시 거절(ECONNREFUSED)된다 — 이건 "TCP 는 붙었다가 0바이트로
    # 끊기는" 것과 달리 **진짜 연결 실패**다. on_connect_io_complete 의 실패
    # 분기는 성공 시에만 하는 backoff_current_ms_ 리셋을 안 타므로, 이 창에서는
    # 재시도 간격이 250→500→1000ms 로 실제로 배증한다(§5-2). 이 성질이
    # "세션 서버가 아직 안 떠 있을 때"를 그대로 재현한다.
    #
    # ⚠️ 리스너를 먼저 띄우고 Stop() 으로 내리는 방식은 이 환경에서 실측으로
    # 깨졌다 — TcpListener.Stop() 을 부른 지 50ms 뒤에도 127.0.0.1:9100 으로
    # 보낸 자체 프로브 연결이 성공했다(원인 불명 — Windows 소켓 스택의 이 환경
    # 특유의 동작으로 보인다. 다음에 이 포트를 또 내렸다 올리고 싶어지면 먼저
    # 이 관찰을 의심할 것). 리스너를 "아예 안 만든 상태"는 그 문제 자체가
    # 생길 수 없어 더 깨끗하다 — 그래서 순서를 뒤집었다.
    #
    # stdout/stderr 를 파일로 리다이렉트한다 — 안 하면 village.exe 의 콘솔 출력과
    # 이 스크립트의 Write-Host 가 같은 콘솔 버퍼를 두 프로세스가 동시에 써서
    # 줄 단위로 뒤섞이거나 잘린다(실측으로 확인된 문제 — 판정 정본은 어차피
    # logs\server.log 다. TESTING.md 의 "stdout 리다이렉트는 버퍼링 절단" 경고와
    # 같은 계열이라 아예 분리한다).
    $villageStdout = Join-Path $scratchRoot 'village.stdout.log'
    $villageStderr = Join-Path $scratchRoot 'village.stderr.log'
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = "--seconds $Seconds"
    $psi.WorkingDirectory = $scratchRoot
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $proc = [System.Diagnostics.Process]::Start($psi)
    # 리다이렉트한 스트림을 안 읽으면 OS 파이프 버퍼(4KB)가 차서 village.exe 가
    # 자기 콘솔 출력에서 블록될 수 있다 — 비동기로 계속 비워 준다.
    $proc.BeginOutputReadLine()
    $proc.BeginErrorReadLine()
    Register-ObjectEvent -InputObject $proc -EventName OutputDataReceived -Action {
        if ($EventArgs.Data) { Add-Content -LiteralPath $Event.MessageData -Value $EventArgs.Data }
    } -MessageData $villageStdout | Out-Null
    Register-ObjectEvent -InputObject $proc -EventName ErrorDataReceived -Action {
        if ($EventArgs.Data) { Add-Content -LiteralPath $Event.MessageData -Value $EventArgs.Data }
    } -MessageData $villageStderr | Out-Null

    $logPath = Join-Path $scratchRoot 'logs\server.log'

    # 250+500=750ms 면 세 번째 "retry in 1000ms" 줄이 이미 찍혀 있어야 한다 — 는
    # 첫 계산이었고, 그대로 1500ms 를 줬더니 재시도가 0건인 채로 리스너가 서
    # 버렸다(village 자신의 기동 지연 — 설정 읽기·DB 워커 프리웜·zone 기동 등 —
    # 이 그 1.5초를 이미 다 먹었다). 3500ms 로 늘려도 여전히 부족했다 — 로그를
    # 직접 재 보니 원인이 계산 자체에 있었다: **이 환경에서는 리스너 없는
    # 포트로의 ConnectEx 실패가 즉시(RST)가 아니라 매번 약 2~2.6초씩 걸린다**
    # ("retry in 250ms" 로그와 그다음 "retry in 500ms" 로그 사이 실측 간격이
    # 250ms 가 아니라 2.3~3.0초였다). 정상적인 loopback closed-port 동작과
    # 다른데, 원인은 못 밝혔다(이 머신의 방화벽/보안 소프트웨어가 SYN 을 조용히
    # 버려 TCP 재전송 타이머를 태우는 것으로 추정 — 안 쟀다). 그래서 필요한
    # 시간은 (연결 실패 자체의 지연 ~2~2.6초) × 3회 + 백오프 대기(250+500ms) ≈
    # 8.5초이고, 스케줄링 여유를 더해 10000ms 로 확정했다.
    Start-Sleep -Milliseconds 10000

    # pre-phase 구간의 끝 — 이 바이트 오프셋 "이전"의 "retry in" 줄만 백오프
    # 수열 판정에 쓴다. 리스너를 세운 뒤에도 재시도는 계속 로그를 남기는데,
    # 그건 이 판정의 대상이 아니다(정상 접속 성공/실패 각본은 뒤에서 따로 본다).
    $prePhaseLen = 0
    if (Test-Path $logPath) { $prePhaseLen = (Get-Item $logPath).Length }

    $listener = New-S2sListener $SessionPort

    # ══════════════════════════════════════════════════════════════════════
    #  연결 #0 — ⑰ Kick body 길이 위반(0B, 고정 9B 기대) → force_disconnect.
    #            ⑫(Reserve 길이 위반)와 같은 매커니즘(§4-a — on_request 의
    #            반환형이 void 라 응답 없이 force_disconnect() 로 끊는다)
    #            이지만 매트릭스의 다른 칸(Kick 의 body 검증)이라 별도
    #            항목이다. 전용 연결로 앞에 두는 이유 — 이 disconnect 가
    #            만드는 재접속을 뒤이은 「연결 #1」의 accept 가 그대로
    #            흡수한다(⑫ 의 disconnect 를 「연결 #2」가 흡수하는 것과
    #            같은 관용구 — 기존 시퀀스 중간에 끼워 넣으면 그 사이 번호
    #            (#2~#6)가 전부 하나씩 밀려 각 항목이 붙들고 있는 백오프
    #            타이밍 판정이 깨진다).
    # ══════════════════════════════════════════════════════════════════════
    $client0 = Wait-Accept $listener 8000 'connect #0 (⑰ malformed Kick)'
    if ($null -eq $client0) {
        Add-Result '⑰' $false '연결 #0 이 오지 않았다'
    } else {
        $stream0 = $client0.GetStream()
        $regFrame0 = Read-S2sFrame $stream0 3000
        if ($null -eq $regFrame0 -or $regFrame0.MsgId -ne $MsgRegister) {
            Add-Result '⑰' $false 'Register 수신 실패 — malformed Kick 시나리오 진행 불가'
            try { $client0.Close() } catch {}
        } else {
            Send-S2sFrame $stream0 $MsgRegisterAck 1 (New-RegisterAckBody 1000 $ResultOk)

            $kickCheckpoint = 0
            if (Test-Path $logPath) { $kickCheckpoint = (Get-Item $logPath).Length }
            Send-S2sFrame $stream0 $MsgKick 501 ([byte[]]::new(0))

            # ⑫ 와 같은 방식 — Poll(SelectRead)+Available==0 으로 "닫혔다"를
            # "아직 응답이 없다"와 가른다(Read-ExactBytes 의 null 하나로는
            # 안 갈린다).
            $deadKick = $false
            $ksw = [System.Diagnostics.Stopwatch]::StartNew()
            while ($ksw.ElapsedMilliseconds -lt 3000) {
                if ($client0.Client.Poll(50000, [System.Net.Sockets.SelectMode]::SelectRead)) {
                    if ($client0.Client.Available -eq 0) {
                        $deadKick = $true
                        break
                    }
                    Read-S2sFrame $stream0 500 | Out-Null
                }
            }
            $sawKickWarn = $false
            if (Test-Path $logPath) {
                $fs0 = [System.IO.File]::Open($logPath, 'Open', 'Read', 'ReadWrite')
                try {
                    $len0 = $fs0.Length
                    if ($len0 -gt $kickCheckpoint) {
                        $fs0.Seek($kickCheckpoint, 'Begin') | Out-Null
                        $buf0 = [byte[]]::new($len0 - $kickCheckpoint)
                        $fs0.Read($buf0, 0, $buf0.Length) | Out-Null
                        $sawKickWarn = ([System.Text.Encoding]::UTF8.GetString($buf0)) -match 'Kick 길이 위반.*끊는다'
                    }
                } finally { $fs0.Close() }
            }
            Add-Result '⑰' ($deadKick -and $sawKickWarn) `
                ("Kick body 길이 위반(0B) → 절단=" + $deadKick + " · 로그 확인=" + $sawKickWarn)
            try { $client0.Close() } catch {}
        }
    }

    # ══════════════════════════════════════════════════════════════════════
    #  연결 #1 — ① Register/RegisterAck · ② 하트비트 3왕복 · ③ 1건 드롭(타임아웃)
    #            ④ 대기 외 seq · ⑤ 미정의 msg_id · ⑦ Kick/Unsupported ·
    #            ⑫ Reserve body 길이 위반 → force_disconnect
    # ══════════════════════════════════════════════════════════════════════
    $client1 = Wait-Accept $listener 8000 'connect #1'
    if ($null -eq $client1) { throw '연결 #1 이 오지 않았다 — 시나리오를 진행할 수 없다' }
    $stream1 = $client1.GetStream()

    $regFrame = Read-S2sFrame $stream1 3000
    if ($null -eq $regFrame -or $regFrame.MsgId -ne $MsgRegister -or $regFrame.Seq -ne 1) {
        Add-Result '①' $false "Register 수신 실패 또는 seq!=1 (frame=$regFrame)"
    } else {
        $reg = ConvertFrom-RegisterBody $regFrame.Body
        # 기본 [server] 절 값(port=9000 · max_connections=4096) · advertise_host
        # 기본값(127.0.0.1) 과 대조한다 — 스크래치 사본은 [s2s] 절만 패치했다.
        $fieldsOk = ($reg.Ver -eq 0x0100) -and ($reg.Port -eq 9000) -and
                    ($reg.Capacity -eq 4096) -and ($reg.Host -eq '127.0.0.1')
        Add-Result '①(파싱)' $fieldsOk `
            ("ver=0x{0:X4} port={1} capacity={2} host={3}" -f $reg.Ver, $reg.Port, $reg.Capacity, $reg.Host)
        Send-S2sFrame $stream1 $MsgRegisterAck 1 (New-RegisterAckBody 1001 $ResultOk)
    }

    # 등록 확인 직후 마을이 FullSync 를 한 번 보낸다(Step4, 빈 집합이라
    #   chunk_total=1·count=0) — on_register_ack 안에서 동기로 나가므로 첫
    #   하트비트(heartbeat_ms 뒤)보다 항상 먼저 도착한다. 걷어내고 내용을
    #   한 번 단언한다.
    $fullSyncFrame = Read-S2sFrame $stream1 2000
    $fullSyncOk = ($null -ne $fullSyncFrame) -and ($fullSyncFrame.MsgId -eq $MsgFullSync)
    if ($fullSyncOk) {
        $fullSync = ConvertFrom-FullSyncBody $fullSyncFrame.Body
        $fullSyncOk = ($fullSync.ChunkTotal -eq 1) -and ($fullSync.Count -eq 0)
    }
    Add-Result '①(FullSync)' $fullSyncOk `
        $(if ($null -ne $fullSyncFrame) { "msg_id=0x$($fullSyncFrame.MsgId.ToString('X4'))" } else { '수신 실패' })

    # ② + ③ — 하트비트 4건을 받는다. 1~3번은 정상 ack, 4번은 일부러 드롭한다.
    $hbSeqs = New-Object System.Collections.Generic.List[uint32]
    $gotAll = $true
    for ($i = 1; $i -le 4; $i++) {
        $hb = Read-S2sFrame $stream1 3000
        if ($null -eq $hb -or $hb.MsgId -ne $MsgHeartbeat) { $gotAll = $false; break }
        $hbSeqs.Add($hb.Seq)
        if ($i -le 3) {
            Send-S2sFrame $stream1 $MsgHeartbeatAck $hb.Seq ([byte[]]::new(0))
        }
        # $i -eq 4 인 경우 응답하지 않는다 — request_timeout_ms(1500) 뒤 클라이언트가
        # 타임아웃으로 스스로 실패 완료한다.
    }
    $monotonic = $true
    for ($i = 1; $i -lt $hbSeqs.Count; $i++) {
        if ($hbSeqs[$i] -le $hbSeqs[$i - 1]) { $monotonic = $false }
    }
    Add-Result '②' ($gotAll -and $monotonic -and $hbSeqs.Count -ge 3) `
        ("heartbeat seq = " + ($hbSeqs -join ','))

    # ④ — 대기 중이지 않은 seq(9999) 로 HeartbeatAck 를 먼저 보낸다.
    Send-S2sFrame $stream1 $MsgHeartbeatAck 9999 ([byte[]]::new(0))

    # ⑤ — 미정의 msg_id.
    Send-S2sFrame $stream1 $MsgUnknownTest 0 ([byte[]]::new(0))

    # ⑦ — 이 항목의 전제(등록됐지만 미구현인 요청 = Kick)가 Kick 구현으로
    #     소멸했다. on_request 의 Unsupported 폴백은 §8-4 순차 패치(새 요청
    #     id 가 분류표에 먼저 등록되는 시기) 대비 방어로 유지된다 — 와이어로
    #     도달할 살아있는 id 가 지금 없다(classify_s2s 가 is_request_from_session
    #     의 정확히 세 열거값 Reserve·Kick·SetMode 만 kRequest 로 분류해
    #     on_request 로 보내는데, 그 셋을 이제 on_request 가 전부 처리하므로
    #     이 폴백에 닿는 갈래가 없다 — 그 밖의 값은 classify_s2s 단계에서
    #     kUnknown 으로 끝나 on_request 자체가 안 불린다).
    #     재활성 조건: 새 세션→마을 요청 MsgId 가 is_request_from_session 에
    #     등록되고 핸들러가 아직 없는 시기.
    Add-Skip '⑦' 'Unsupported 폴백 — 지금 와이어로 도달할 살아있는 미구현 요청 id 가 없다'

    # ⑭ — Reserve 정상 경로(항목6). session.ps1 은 실물
    # session·village 사이의 S2S 소켓을 엿볼 방법이 없어(제3자 프로세스) 이
    # 항목을 못 만든다 — 하네스가 가짜 세션 서버로서 그 소켓의 한쪽 끝을
    # 쥐고 있는 여기서만 "그 프레임으로" 단언할 수 있다(항목10 의 Reserve
    # 길이 위반과 같은 구조적 이유). 저장을 건너뛰고 무조건 ok 를
    # 회신하는 결함은 이 프레임 자체(ReserveAck.result)를 직접 보지 않으면
    # 못 잡는다 — Enter 성공 여부로 간접 추론하면 그 결함이 통과한다(§7
    # 항목6 경고).
    Send-S2sFrame $stream1 $MsgReserve 201 (New-ReserveBody 81001 10000)
    $reserveAck1 = Wait-ForFrame $stream1 $MsgReserveAck 4000
    $reserveAck1Body = if ($reserveAck1) { ConvertFrom-ReserveAckBody $reserveAck1.Body } else { $null }
    Add-Result '⑭' (($null -ne $reserveAck1) -and ($reserveAck1.Seq -eq 201) -and ($reserveAck1Body.Result -eq $ResultOk)) `
        ("Reserve(player=81001) -> ReserveAck seq=" + $(if ($reserveAck1) { $reserveAck1.Seq } else { '(없음)' }) + " result=" + $(if ($reserveAck1Body) { $reserveAck1Body.Result } else { '-' }) + "(기대 0)")

    # ⑮ — Reserve 재수신(경계값, 항목7-a). 같은 player_id 로 두 번째
    # Reserve 를 보내도 마을의 add_reservation 덮어쓰기(§8-2 멱등)가 실패
    # 갈래 없이 또 ok 를 회신하는지 — "Enter 가 한 번만 성공하는지"(항목7-b)
    # 는 실물 Enter 클라 경로가 필요해 session.ps1 몫이다(이
    # 항목은 그 절반).
    Send-S2sFrame $stream1 $MsgReserve 202 (New-ReserveBody 81001 10000)
    $reserveAck2 = Wait-ForFrame $stream1 $MsgReserveAck 4000
    $reserveAck2Body = if ($reserveAck2) { ConvertFrom-ReserveAckBody $reserveAck2.Body } else { $null }
    Add-Result '⑮' (($null -ne $reserveAck2) -and ($reserveAck2.Seq -eq 202) -and ($reserveAck2Body.Result -eq $ResultOk)) `
        ("같은 player_id 재수신 Reserve -> ReserveAck seq=" + $(if ($reserveAck2) { $reserveAck2.Seq } else { '(없음)' }) + " result=" + $(if ($reserveAck2Body) { $reserveAck2Body.Result } else { '-' }) + "(기대 0 — 덮어쓰기 멱등)")

    # ⑯ — Kick(마을에 없는 pid) → KickAck(NotFound=1). §8-2 「없음도 성공」의
    #   Kick 판 — 이 마을이 한 번도 들여보낸 적 없는 pid 를 지목해도 규약
    #   위반이 아니라 정상 응답으로 답하고, 그 응답 자체가 연결을 안 끊는지도
    #   함께 본다(다음 항목들이 같은 연결 #1 을 계속 쓴다).
    Send-S2sFrame $stream1 $MsgKick 203 (New-KickBody 81999 0)
    $kickAck1 = Wait-ForFrame $stream1 $MsgKickAck 4000
    $kickAck1Body = if ($kickAck1) { ConvertFrom-KickAckBody $kickAck1.Body } else { $null }
    Add-Result '⑯' (($null -ne $kickAck1) -and ($kickAck1.Seq -eq 203) -and ($kickAck1Body.Result -eq 1)) `
        ("Kick(player=81999, 미입장) -> KickAck seq=" + $(if ($kickAck1) { $kickAck1.Seq } else { '(없음)' }) + " result=" + $(if ($kickAck1Body) { $kickAck1Body.Result } else { '-' }) + "(기대 1=NotFound)")

    # ⑱ — 입장 상태 pid 에 Kick 2연발을 한 배치로(응답을 안 기다리고 연달아)
    #   보낸다. close_by_id 의 closing 선점 분기(net/iocp_server.cpp)가
    #   실제로 발화하는지는 EntryTable::leave() 가 직렬 큐 Job 으로(
    #   존 스레드 폐기로 정리 Job 이 도는 큐가 워커 직렬 큐로 바뀌었다)
    #   비동기 완료되는 타이밍에 달려 있어 이 하네스로는 결정적으로 못
    #   잡는다(idle 스윕과 같은 부류의 한계) — 그래서 이 항목의 목적은
    #   "그 분기를 확실히 태운다"가 아니라 "두 번 자극해도 무해하다"다.
    #   둘 다 성공값(Kicked 또는 NotFound)이고 마을이 살아 있으면 충분하다
    #   — 선점 분기 자체의 결정적 커버리지는 ASan 구성 회차의 데이터
    #   레이스 계측 몫이다.
    Send-S2sFrame $stream1 $MsgReserve 204 (New-ReserveBody 82001 10000)
    $reserveAck18 = Wait-ForFrame $stream1 $MsgReserveAck 4000
    $reserveAck18Ok = ($null -ne $reserveAck18) -and ($reserveAck18.Seq -eq 204)

    $vcl18 = New-Object System.Net.Sockets.TcpClient
    $vcl18.NoDelay = $true
    $vcl18.Connect('127.0.0.1', 9000)
    $vst18 = $vcl18.GetStream()
    Send-ClientFrame $vst18 $VilEnterReq (New-EnterBody 82001)
    $enter18 = Read-ClientFrame $vst18 3000
    $enter18Ok = ($null -ne $enter18) -and ($enter18.MsgId -eq $VilEnterAck) -and (([int]$enter18.Body[0]) -eq 0)

    # 배치 — 두 Kick 을 응답을 기다리지 않고 연달아 쓴다.
    Send-S2sFrame $stream1 $MsgKick 205 (New-KickBody 82001 0)
    Send-S2sFrame $stream1 $MsgKick 206 (New-KickBody 82001 0)

    $kick18a = Wait-ForFrame $stream1 $MsgKickAck 5000
    $kick18aOk = ($null -ne $kick18a) -and (($kick18a.Body[0] -eq 0) -or ($kick18a.Body[0] -eq 1))
    $kick18b = Wait-ForFrame $stream1 $MsgKickAck 5000
    $kick18bOk = ($null -ne $kick18b) -and (($kick18b.Body[0] -eq 0) -or ($kick18b.Body[0] -eq 1))
    try { $vcl18.Close() } catch {}

    Add-Result '⑱' ($reserveAck18Ok -and $enter18Ok -and $kick18aOk -and $kick18bOk) `
        ("Reserve/Enter 성공=" + ($reserveAck18Ok -and $enter18Ok) + " · Kick 2연발 응답값=" +
         $(if ($kick18a) { [int]$kick18a.Body[0] } else { '-' }) + "," + $(if ($kick18b) { [int]$kick18b.Body[0] } else { '-' }) +
         "(기대 둘 다 0 또는 1 — closing 선점 발화는 확률적, 자극+무해 관측 목적)")

    # ③ 의 타임아웃(1500ms)이 실제로 클라이언트 쪽에서 발화할 시간을 벌어 준다 —
    # 이 연결을 너무 빨리 닫으면 그 대기 요청이 "타임아웃"이 아니라 "끊김"으로
    # 소비돼 timeouts 대신 failed 로 잡힌다.
    Start-Sleep -Milliseconds 1900

    # ⑫ — Reserve 를 body 길이 위반(11B, 고정 12B 기대)으로 보낸다.
    #   S2sLink::on_request 의 반환형이 void 라 클라 핸들러의 bad_body 처럼
    #   응답을 준 뒤 false 로 끊는 구조가 없다 — force_disconnect() 를 직접
    #   불러 끊는다(§4-a). ⑦(Kick→Unsupported)이 같은 연결의 같은 자리에서
    #   "응답이 있는 요청"을 검증했으니, 이번엔 "응답 없이 끊기는 요청"을
    #   같은 자리에서 검증한다 — ③ 의 타임아웃 창을 침범하지 않도록 그 대기
    #   (위 Start-Sleep) 뒤로 미뤄 뒀다.
    $reserveCheckpoint = 0
    if (Test-Path $logPath) { $reserveCheckpoint = (Get-Item $logPath).Length }

    $badReserveBody = [byte[]]::new(11)
    Send-S2sFrame $stream1 $MsgReserve 88 $badReserveBody

    # force_disconnect() 가 걸리기 전에 이미 나가 있던 하트비트가 섞여 들어올 수
    # 있다(연결 #1 은 이미 여러 왕복을 거쳐 heartbeat_ms 주기가 돌고 있었다) —
    # 그런 프레임은 Wait-ForFrame 과 같이 정상 ack 로 흘려보내고 계속 본다.
    # 절단 자체는 0바이트 Read 를 기다리는 대신 Poll(SelectRead)+Available==0 으로
    # 직접 확인한다 — "아직 안 왔다"와 "닫혔다"가 Read-ExactBytes 의 null 하나로는
    # 안 갈리기 때문이다.
    $deadReserve = $false
    $rsw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($rsw.ElapsedMilliseconds -lt 3000) {
        if ($client1.Client.Poll(50000, [System.Net.Sockets.SelectMode]::SelectRead)) {
            if ($client1.Client.Available -eq 0) {
                $deadReserve = $true
                break
            }
            $frame = Read-S2sFrame $stream1 500
            if ($null -ne $frame -and $frame.MsgId -eq $MsgHeartbeat) {
                Send-S2sFrame $stream1 $MsgHeartbeatAck $frame.Seq ([byte[]]::new(0))
            }
        }
    }

    $sawReserveWarn = $false
    if (Test-Path $logPath) {
        $fs = [System.IO.File]::Open($logPath, 'Open', 'Read', 'ReadWrite')
        try {
            $len = $fs.Length
            if ($len -gt $reserveCheckpoint) {
                $fs.Seek($reserveCheckpoint, 'Begin') | Out-Null
                $buf = [byte[]]::new($len - $reserveCheckpoint)
                $fs.Read($buf, 0, $buf.Length) | Out-Null
                $tail = [System.Text.Encoding]::UTF8.GetString($buf)
                $sawReserveWarn = $tail -match 'Reserve 길이 위반.*끊는다'
            }
        } finally { $fs.Close() }
    }
    Add-Result '⑫' ($deadReserve -and $sawReserveWarn) `
        ("Reserve 길이 위반 → 절단=" + $deadReserve + " · 로그 확인=" + $sawReserveWarn)

    $client1.Close()

    # ══════════════════════════════════════════════════════════════════════
    #  연결 #2 — ⑥ RegisterAck 4B(짧게) + 같은 배치에 대기 외 seq 프레임을 붙여 보낸다
    # ══════════════════════════════════════════════════════════════════════
    $client2 = Wait-Accept $listener 3000 'connect #2 (⑥ 직전 재접속)'
    if ($null -eq $client2) {
        Add-Result '⑥' $false '재접속(#2)이 오지 않았다'
    } else {
        $stream2 = $client2.GetStream()
        $reg2 = Read-S2sFrame $stream2 3000

        # ⑥ 판정 체크포인트 — 이 배치를 보내기 "직전" 로그 크기를 기록해 둔다.
        #   ④ 가 이미 정상적으로 stray_seq 1건을 남겼으므로, 로그 전체를 보면
        #   ⑥ 이 맞게 막아도 그 옛 stray 가 걸려 판정이 항상 FAIL 한다 — 부재
        #   관측은 스코프를 직접 잡아야 한다(긍정 관측과 달리 자동으로 안 잡힌다).
        $checkpointLen = 0
        if (Test-Path $logPath) { $checkpointLen = (Get-Item $logPath).Length }

        if ($null -eq $reg2 -or $reg2.MsgId -ne $MsgRegister -or $reg2.Seq -ne 1) {
            Add-Result '⑥(전제)' $false "재접속 후 Register 수신 실패 또는 seq!=1"
        } else {
            # 위반 프레임(4B — 5B 기대) + 대기 외 seq 의 가짜 HeartbeatAck 를
            # 한 번의 Write 로 이어 붙여 보낸다 — 같은 recv 배치를 노린다.
            $shortBody = [byte[]]@(0, 0, 0, 0)                       # 4B, RegisterAck 는 5B 여야 한다
            $violation = (New-S2sHeader $shortBody.Length $MsgRegisterAck 1) + $shortBody
            $strayHb   = (New-S2sHeader 0 $MsgHeartbeatAck 8888) + [byte[]]::new(0)
            $batch = $violation + $strayHb
            $stream2.Write($batch, 0, $batch.Length)
            $stream2.Flush()
        }

        Start-Sleep -Milliseconds 400   # village 가 위반을 감지하고 끊을 시간

        # 체크포인트 이후 구간에서만 stray 부재를 검사한다.
        $sawNewStray = $false
        if (Test-Path $logPath) {
            $fs = [System.IO.File]::Open($logPath, 'Open', 'Read', 'ReadWrite')
            try {
                $len = $fs.Length
                if ($len -gt $checkpointLen) {
                    $fs.Seek($checkpointLen, 'Begin') | Out-Null
                    $buf = [byte[]]::new($len - $checkpointLen)
                    $fs.Read($buf, 0, $buf.Length) | Out-Null
                    $tail = [System.Text.Encoding]::UTF8.GetString($buf)
                    $sawNewStray = $tail -match '\[WARN\] s2s stray seq'
                }
            } finally { $fs.Close() }
        }
        $client2.Close()
        Add-Result '⑥' (-not $sawNewStray) `
            "체크포인트 이후 stray seq 경고 재발생 여부 = $sawNewStray (false 여야 차단 성공)"
    }

    # ══════════════════════════════════════════════════════════════════════
    #  연결 #3 — ⑥ 이 낸 재접속(재 Register 수신) 확인 + 잠깐 정상 교환 후
    #            리스너를 내려 ⑧ 준비
    # ══════════════════════════════════════════════════════════════════════
    $client3 = Wait-Accept $listener 3000 'connect #3 (⑥ 이후 재접속)'
    if ($null -eq $client3) {
        Add-Result '⑥(재등록)' $false '⑥ 이후 재접속이 오지 않았다'
    } else {
        $stream3 = $client3.GetStream()
        $reg3 = Read-S2sFrame $stream3 3000
        if ($null -eq $reg3 -or $reg3.MsgId -ne $MsgRegister -or $reg3.Seq -ne 1) {
            Add-Result '⑥(재등록)' $false 'Register 재수신 실패 또는 seq!=1'
        } else {
            Send-S2sFrame $stream3 $MsgRegisterAck 1 (New-RegisterAckBody 1002 $ResultOk)
            Add-Result '⑥(재등록)' $true '재접속 뒤 Register(seq=1) 재수신 확인'

            # in-flight 로 만들 하트비트 하나를 받되 응답하지 않는다 — 곧바로
            # 연결을 끊어 이 요청이 "failed" 로 잡히게 한다(⑧).
            $hb = Read-S2sFrame $stream3 2000
            if ($null -ne $hb) {
                # 응답하지 않는다 — 의도적.
            }
        }

        $client3.Close()
    }

    # ══════════════════════════════════════════════════════════════════════
    #  ⑧ — "세션 서버가 응답하지 않는다"를 흉내 낸다.
    #
    #  ⛔ 리스너 자체를 Stop() 했다가 새로 만드는 방식은 이 환경에서 실측으로
    #  깨졌다 — TcpListener.Stop() 을 부른 직후에도(자체 프로브로 확인) 그
    #  포트가 여전히 새 연결을 받았다. 원인을 더 캐는 대신(6단계 상한 안에서),
    #  리스너는 그대로 두고 다음 두 번의 접속을 "받자마자 바로 끊는" 방식으로
    #  같은 효과(연결이 안 되는 것처럼 보이는 반복)를 낸다 — village 입장에서는
    #  TCP 는 붙었다가 즉시 0바이트로 끊기므로 "정상 연결"이 아니라 여전히
    #  실패로 처리되고 백오프가 진행된다. 250→500→1000ms 수열은 village 자신의
    #  "[S2S  ] disconnected, retry in Nms" 로그로 사후 검증한다(판정은 로그가
    #  정본 — TESTING.md).
    # ══════════════════════════════════════════════════════════════════════
    for ($deadIdx = 1; $deadIdx -le 2; $deadIdx++) {
        $deadClient = Wait-Accept $listener 3000 "connect (⑧ 의도적 즉시 종료 #$deadIdx)"
        if ($null -ne $deadClient) {
            $deadClient.Close()
        }
    }

    $client4 = Wait-Accept $listener 4000 'connect #4 (⑧ 재접속)'
    if ($null -eq $client4) {
        Add-Result '⑧' $false '의도적 실패 반복 이후 재접속이 오지 않았다'
    } else {
        $stream4 = $client4.GetStream()
        $reg4 = Read-S2sFrame $stream4 3000
        $seqOk = ($null -ne $reg4) -and ($reg4.MsgId -eq $MsgRegister) -and ($reg4.Seq -eq 1)
        Add-Result '⑧' $seqOk `
            ("의도적 실패 반복 이후 재접속 + Register seq=" + $(if ($reg4) { $reg4.Seq } else { '(없음)' }) + " (초기화 검증)")

        # ⑨ — 이 Register 를 버전 거부로 응답한다.
        if ($null -ne $reg4) {
            # 오버라이드 단언 체크포인트 — 이 응답을 보내기 "직전" 로그 크기를
            # 기록해 둔다. s2s_link.cpp 의 on_register_ack 은 version_rejected 를
            # 보면 force_disconnect(backoff_max_ms) 로 "다음 재시도 1회를 최대
            # 백오프로 덮어쓴다"(1s 간격 거부 루프 방지 계약). 이 시점 이후 첫
            # "retry in Nms" 값이 정확히 backoff_max_ms(축소본 1000)여야 그
            # 오버라이드가 실제로 걸렸다는 증거다 — 회귀로 오버라이드 인자가
            # 빠지면(force_disconnect() 인자 없이) 방금 연결 성공으로 리셋된
            # backoff_current_ms_(250)이 대신 찍힌다.
            $versionRejectCheckpoint = 0
            if (Test-Path $logPath) { $versionRejectCheckpoint = (Get-Item $logPath).Length }
            Send-S2sFrame $stream4 $MsgRegisterAck 1 (New-RegisterAckBody 1003 $ResultVersionRejected)
        }
        $client4.Close()
    }

    # ══════════════════════════════════════════════════════════════════════
    #  연결 #5 — ⑨ 최대 백오프(1000ms) 뒤 재접속·재Register 확인.
    #            이 Register 는 ⑪(정원 거부, result=2)로 응답한다 — ⑨(버전 거부)와
    #            동형 골격이고, s2s_link 의 kResultFull 분기도 같은 계약
    #            (force_disconnect(backoff_max_ms))을 지켜야 한다.
    # ══════════════════════════════════════════════════════════════════════
    $client5 = Wait-Accept $listener 4000 'connect #5 (⑨ 버전 거부 이후 재접속)'
    if ($null -eq $client5) {
        Add-Result '⑨' $false '버전 거부 이후 재접속이 오지 않았다'
    } else {
        $stream5 = $client5.GetStream()
        $reg5 = Read-S2sFrame $stream5 3000
        $ok = ($null -ne $reg5) -and ($reg5.MsgId -eq $MsgRegister) -and ($reg5.Seq -eq 1)
        Add-Result '⑨' $ok '버전 거부 뒤 최대 백오프 경과 후 재접속 + Register 재수신(재시도 지속)'
        if ($ok) {
            # ⑪ 체크포인트 — ⑨와 같은 방식. 이 응답 이후 첫 "retry in Nms" 가
            # 정확히 backoff_max_ms(축소본 1000)여야 full 분기의 오버라이드가
            # 실제로 걸렸다는 증거다(판정은 로그 검증 절에서 한 항목으로 합친다).
            $fullRejectCheckpoint = 0
            if (Test-Path $logPath) { $fullRejectCheckpoint = (Get-Item $logPath).Length }
            Send-S2sFrame $stream5 $MsgRegisterAck 1 (New-RegisterAckBody 1004 $ResultFull)
        }
        $client5.Close()
    }

    # ══════════════════════════════════════════════════════════════════════
    #  연결 #6 — ⑪ 정원 거부(1000ms) 뒤 재접속·재Register, 이후 result=0 복귀
    #            → ⑩ 만료 직전 하트비트 무응답 → ⑬ 정상 종료 직전 Unregister
    # ══════════════════════════════════════════════════════════════════════
    $client6 = Wait-Accept $listener 4000 'connect #6 (⑪ 정원 거부 이후 재접속)'
    if ($null -eq $client6) {
        $fullReconnectOk = $false
    } else {
        $stream6 = $client6.GetStream()
        $reg6 = Read-S2sFrame $stream6 3000
        $fullReconnectOk = ($null -ne $reg6) -and ($reg6.MsgId -eq $MsgRegister) -and ($reg6.Seq -eq 1)
        if ($fullReconnectOk) {
            Send-S2sFrame $stream6 $MsgRegisterAck 1 (New-RegisterAckBody 1005 $ResultOk)
        }

        # ⑩ 을 위해 정확히 언제부터 하트비트 무응답으로 돌릴지 계산한다 —
        # village 프로세스 시작 시각 기준으로 (Seconds-3)초가 지나면 응답을 끊는다.
        $silenceAt = $proc.StartTime.AddSeconds([Math]::Max(1, $Seconds - 3))

        $ackedAfterRegister = 0
        $sawUnregister = $false
        while ($true) {
            $frame = Read-S2sFrame $stream6 1000
            if ($null -eq $frame) {
                if ([DateTime]::Now -gt $silenceAt.AddSeconds(6)) { break }   # 안전판 — 무한 대기 방지
                if ($proc.HasExited) { break }
                continue
            }
            if ($frame.MsgId -eq $MsgHeartbeat) {
                if ([DateTime]::Now -lt $silenceAt) {
                    Send-S2sFrame $stream6 $MsgHeartbeatAck $frame.Seq ([byte[]]::new(0))
                    $ackedAfterRegister++
                }
                # $silenceAt 이후로는 일부러 응답하지 않는다(⑩) — village 는
                # --seconds 만료로 stop() 하며 이 대기 요청들을 kStopped 로 실패시킨다.
            }
            elseif ($frame.MsgId -eq $MsgUnregister) {
                # ⑬ — village 가 정상 종료 직전 실제로 이 프레임을 내보내는지를
                #   여기서 직접 관측한다. 코드를 읽고 "나간다"고 주장하는 대신
                #   이 자리가 그 증거다. Unregister 는 알림이 아니라 요청이다 —
                #   실물 세션 서버(session_router.cpp)가 seq 를 그대로 돌려
                #   UnregisterAck 로 답하므로 여기서도 그렇게 답해야 village 의
                #   unregister_and_wait() 가 타임아웃이 아니라 응답으로 즉시
                #   돌아온다.
                #   여기서 곧바로 break 하고 연결을 닫지 않는다 — 그러면 이
                #   ack 가 village 에 도달하기 전이든 후든 harness 쪽 TCP 종료가
                #   먼저 검출돼 "정상 종료 드레인" 대신 "예기치 않은 끊김"
                #   경로(재연결 스케줄까지 포함)를 타 버린다(⑩ 의 "stopped, N
                #   pending failed" 단언이 그 경로에 가려 0으로 나오는 것으로
                #   실측 확인됨). 연결을 열어 둔 채 계속 읽어 village 가 스스로
                #   server.stop()/s2s_link.stop() 으로 끊을 때까지 기다린다.
                $sawUnregister = $true
                Send-S2sFrame $stream6 $MsgUnregisterAck $frame.Seq ([byte[]]::new(0))
            }
            if ($proc.HasExited) { break }
        }
        Add-Result '⑩(사전조건)' ($ackedAfterRegister -ge 1) `
            "만료 전 정상 하트비트 응답 $ackedAfterRegister 회 — 이후 무응답 구간 진입"
        Add-Result '⑬(Unregister)' $sawUnregister `
            "정상 종료 직전 Unregister 요청 수신 + UnregisterAck 회신 여부 = $sawUnregister"
        $client6.Close()
    }

    # ── village 종료 대기 (--seconds 만료) ───────────────────────────────────
    $waited = $proc.WaitForExit((($Seconds + 15) * 1000))
    if (-not $waited -or -not $proc.HasExited) {
        Write-Host '[FAIL] village.exe 가 예상 시간 안에 스스로 종료되지 않았다 — 강제 종료' -ForegroundColor Red
        try { $proc.Kill() } catch {}
    } else {
        $exitOk = ($proc.ExitCode -eq 0)
    }

    # ══════════════════════════════════════════════════════════════════════
    #  최종 판정 — logs\server.log 가 정본이다(stdout 리다이렉트는 버퍼링 절단)
    # ══════════════════════════════════════════════════════════════════════
    if (-not (Test-Path $logPath)) {
        throw "$logPath 가 없다 — village 가 사본 config 를 못 읽었을 수 있다"
    }
    $log = Get-Content -LiteralPath $logPath -Raw -Encoding UTF8

    # ⑧(백오프 수열) — pre-phase 구간(리스너가 아직 없어 매 시도가 진짜로 실패하던
    # 창)에서만 "retry in Nms" 값을 뽑는다. 바이트 오프셋으로 자르는 이유 —
    # 한글이 섞인 UTF-8 로그를 문자열로 통째로 읽은 뒤 문자 오프셋으로 자르면
    # 멀티바이트 경계가 어긋난다. $prePhaseLen 은 Get-Item.Length(바이트)로 잰
    # 값이라 바이트 슬라이스와 짝이 맞아야 한다.
    $logBytesForSeq = [System.IO.File]::ReadAllBytes($logPath)
    $prePhaseSlice = [Math]::Min($prePhaseLen, $logBytesForSeq.Length)
    $prePhaseText = [System.Text.Encoding]::UTF8.GetString($logBytesForSeq, 0, $prePhaseSlice)
    $retryMatches = [regex]::Matches($prePhaseText, 'disconnected, retry in (\d+)ms')
    $retryValues = @()
    foreach ($m in $retryMatches) { $retryValues += [int]$m.Groups[1].Value }

    $seqOk = $false
    if ($retryValues.Count -ge 3) {
        $seqOk = ($retryValues[0] -eq 250) -and ($retryValues[1] -eq 500) -and ($retryValues[2] -eq 1000)
        for ($i = 3; $i -lt $retryValues.Count; $i++) {
            if ($retryValues[$i] -ne 1000) { $seqOk = $false }
        }
    }
    Add-Result '⑧(백오프 수열)' $seqOk ("retry in " + ($retryValues -join ','))

    # ⑨(오버라이드) — version_rejected 응답 "직후" 첫 retry 값이 정확히
    # backoff_max_ms(축소본 1000)인지. 체크포인트 이전 바이트는 이 판정과
    # 무관하므로 잘라내고 그 뒤 첫 매치만 본다 — $logBytesForSeq 를 그대로
    # 재사용한다(파일은 village 종료 후라 더 안 바뀐다).
    $overrideTailStart = [Math]::Min($versionRejectCheckpoint, $logBytesForSeq.Length)
    $overrideTailText = [System.Text.Encoding]::UTF8.GetString(
        $logBytesForSeq, $overrideTailStart, $logBytesForSeq.Length - $overrideTailStart)
    $overrideMatch = [regex]::Match($overrideTailText, 'disconnected, retry in (\d+)ms')
    $overrideOk = $overrideMatch.Success -and ([int]$overrideMatch.Groups[1].Value -eq 1000)
    Add-Result '⑨(오버라이드)' $overrideOk `
        ("체크포인트 이후 첫 retry 값=" + $(if ($overrideMatch.Success) { $overrideMatch.Groups[1].Value } else { '(없음)' }) + " (기대 1000)")

    # ⑪(정원 거부) — full(result=2) 응답 뒤 ① 마을이 스스로 끊고 최대 백오프
    # 간격으로 재접속해 재Register 했는가(연결 #6 관측) ② 그 간격이 실제로
    # backoff_max_ms 오버라이드였는가(체크포인트 이후 첫 retry 값 = 1000, ⑨와
    # 같은 판정)를 한 항목으로 합쳐 본다.
    $fullTailStart = [Math]::Min($fullRejectCheckpoint, $logBytesForSeq.Length)
    $fullTailText = [System.Text.Encoding]::UTF8.GetString(
        $logBytesForSeq, $fullTailStart, $logBytesForSeq.Length - $fullTailStart)
    $fullMatch = [regex]::Match($fullTailText, 'disconnected, retry in (\d+)ms')
    $fullOverrideOk = $fullMatch.Success -and ([int]$fullMatch.Groups[1].Value -eq 1000)
    Add-Result '⑪(정원 거부)' ($fullReconnectOk -and $fullOverrideOk) `
        ("재접속·재Register=" + $fullReconnectOk + " / 체크포인트 이후 첫 retry 값=" + $(if ($fullMatch.Success) { $fullMatch.Groups[1].Value } else { '(없음)' }) + " (기대 1000)")

    $noConfigWarn = -not ($log -match '\[WARN\] config\\server\.ini 를 못 읽었다')
    Add-Result '설정 사본' $noConfigWarn '사본 config 를 읽었다([WARN] 못 읽음 로그 부재로 확인)'

    Add-Result '⑨(ERROR 로그)' ($log -match '\[ERROR\].*s2s.*버전 거부') 'version_rejected 시 [ERROR] 로그 확인'

    $summaryMatch = [regex]::Match($log,
        'connects=(\d+) reconnects=(\d+) sent=(\d+) recv=(\d+) timeouts=(\d+) failed=(\d+) unsupported_tx=(\d+) stray_seq=(\d+) unknown_msg=(\d+)')
    if ($summaryMatch.Success) {
        $m = $summaryMatch
        $connects      = [int]$m.Groups[1].Value
        $reconnects    = [int]$m.Groups[2].Value
        $sent          = [int]$m.Groups[3].Value
        $recv          = [int]$m.Groups[4].Value
        $timeouts      = [int]$m.Groups[5].Value
        $failed        = [int]$m.Groups[6].Value
        $unsupportedTx = [int]$m.Groups[7].Value
        $strayCount    = [int]$m.Groups[8].Value
        $unknownCount  = [int]$m.Groups[9].Value

        Write-Host (("[S2S 종료 요약] connects={0} reconnects={1} sent={2} recv={3} timeouts={4}" +
            " failed={5} unsupported_tx={6} stray_seq={7} unknown_msg={8}") -f `
            $connects, $reconnects, $sent, $recv, $timeouts, $failed, $unsupportedTx, $strayCount, $unknownCount)

        Add-Result '③' ($timeouts -ge 1) "timeouts=$timeouts (기대 >=1)"
        Add-Result '④' ($strayCount -eq 1) "stray_seq=$strayCount (기대 =1 — ⑥ 차단 실패 시 2)"
        Add-Result '⑤' ($unknownCount -eq 1) "unknown_msg=$unknownCount (기대 =1)"
        Add-Result '⑧(failed)' ($failed -ge 2) "failed=$failed (기대 >=2 — ⑧ in-flight + ⑩ stop)"
        Add-Result '재연결 횟수' ($reconnects -ge 5) "reconnects=$reconnects (⑥·⑧·⑨·⑪ 로 최소 5회 기대)"
        Add-Result '카운터 정합' ($sent -ge $recv) "sent=$sent recv=$recv (기대 sent>=recv)"
    } else {
        Add-Result '종료 요약' $false '[S2S  ] 종료 요약 줄을 로그에서 못 찾았다'
    }

    $stoppedMatch = [regex]::Match($log, '\[S2S\s*\] stopped, (\d+) pending failed')
    Add-Result '⑩' ($stoppedMatch.Success -and [int]$stoppedMatch.Groups[1].Value -ge 1) `
        $(if ($stoppedMatch.Success) { "stopped, $($stoppedMatch.Groups[1].Value) pending failed" } else { '해당 로그 줄 없음' })

    Add-Result '정상 종료' $exitOk "village.exe exit code = $($proc.ExitCode)"

    # ── 요약 ─────────────────────────────────────────────────────────────
    Write-Host ''
    Write-Host '==================== 요약 ===================='
    $failCount = 0
    $skipCount = 0
    foreach ($r in $script:MatrixResults) {
        if ($r.Skip) { $skipCount++ }
        elseif (-not $r.Pass) { $failCount++ }
    }
    foreach ($r in $script:MatrixResults) {
        $mark = if ($r.Skip) { 'SKIP' } elseif ($r.Pass) { 'PASS' } else { 'FAIL' }
        Write-Host ("{0,-16} {1}  {2}" -f $r.Id, $mark, $r.Detail)
    }
    Write-Host ''
    $passCount = $script:MatrixResults.Count - $failCount - $skipCount
    if ($failCount -eq 0) {
        Write-Host "$passCount 통과 + SKIP $skipCount (총 $($script:MatrixResults.Count) 항목)" -ForegroundColor Green
    } else {
        Write-Host "$failCount 건 실패 (총 $($script:MatrixResults.Count) 항목 · SKIP $skipCount)" -ForegroundColor Red
    }
    Write-Host ''
    Write-Host "logs\server.log 사본 경로: $logPath"
}
finally {
    try { if ($listener) { $listener.Stop() } } catch {}
    try { if ($proc -and -not $proc.HasExited) { $proc.Kill() } } catch {}
    Start-Sleep -Milliseconds 300
    try { Remove-Item -Recurse -Force -LiteralPath $scratchRoot -ErrorAction SilentlyContinue } catch {}
}

$anyFail = $false
foreach ($r in $script:MatrixResults) { if (-not $r.Skip -and -not $r.Pass) { $anyFail = $true } }
if ($anyFail) { exit 1 } else { exit 0 }
