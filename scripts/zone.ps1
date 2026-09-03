# scripts\zone.ps1 — 존 입장 · 채팅 브로드캐스트 검증 (L7)
#
#   무엇을 보는가
#     N개 클라이언트가 같은 존에 들어가서 각자 M번 채팅한다.
#     브로드캐스트가 맞다면 「각 클라이언트」가 N×M 개의 ChatNtf 를 받아야 한다.
#
#   왜 이걸로 상태 깨짐을 볼 수 있는가
#     명부(EntryTable)의 zone_index_ 갱신 지점이 enter·leave·move_zone
#     셋으로 고정돼 있다는 규약이 깨지면(다른 자리가 그 버킷을 직접
#     건드리면), 같은 뮤텍스 아래라는 전제가 무너져 member 목록이 깨지고
#     수신 개수가 어긋나거나 중복된다.
#
#   사용:
#     .\zone.ps1 -Clients 8 -Chats 100
#     .\zone.ps1 -Clients 8 -Chats 500 -ZoneId 5

param(
    [int]$Port    = 9000,
    [int]$Clients = 8,
    [int]$ZoneId  = 5,
    [int]$Zones   = 1,          # 클라를 이 개수의 존에 나눠 넣는다 (ZoneId, ZoneId+1, ...)
    [int]$Chats   = 100,
    [int]$Size    = 8,          # 채팅 본문 길이
    [int]$Churn   = 0,          # 채팅 N회마다 절반이 옆 존으로 갔다 돌아온다 (0=안 함)
    [int]$Timeout = 5000,
    [int]$Settle  = 1500,       # 마지막 채팅 뒤 이만큼 더 기다렸다 읽는다 (ms)
    [string]$Config  = 'Release',   # 예약 경유 접속을 위해 스스로 스폰할 village.exe 구성
    [int]$Seconds    = 180          # 스폰한 village.exe 의 수명(초) — 이 하네스 시나리오 전체를 덮을 여유
)

$ErrorActionPreference = 'Stop'

# kLoginReq/직행 접속이 폐지돼서 이 하네스도 예약을 거쳐야 한다.
#   존·채팅 브로드캐스트만 재는 게 목적이라 실물 session.exe 는 안 띄운다 —
#   이 파일이 가짜 세션 서버 노릇을 해서 스스로 띄운 village.exe 에 직접
#   Reserve 를 찔러 넣는다. harness_common.ps1 참조.
. (Join-Path $PSScriptRoot 'harness_common.ps1')

# proto::MsgId
$MSG_JOIN_REQ = 2
$MSG_CHAT_REQ = 3
$MSG_JOIN_ACK = 102
$MSG_CHAT_NTF = 103

function New-Frame([byte[]]$Body, [int]$Id) {
    $h = [byte[]]::new(4)
    $h[0] = [byte](($Body.Length -shr 8) -band 0xFF)
    $h[1] = [byte]( $Body.Length         -band 0xFF)
    $h[2] = [byte](($Id          -shr 8) -band 0xFF)
    $h[3] = [byte]( $Id                  -band 0xFF)
    return $h + $Body
}

function New-U32BE([int]$Value) {
    $b = [byte[]]::new(4)
    $b[0] = [byte](($Value -shr 24) -band 0xFF)
    $b[1] = [byte](($Value -shr 16) -band 0xFF)
    $b[2] = [byte](($Value -shr  8) -band 0xFF)
    $b[3] = [byte]( $Value          -band 0xFF)
    return $b
}

# [int] 캐스팅이 필수다.
#    PowerShell 의 -shl 은 왼쪽이 [byte] 면 결과도 [byte] 로 잘라낸다.
#    [byte]15 -shl 8 == 0 이다. 빠뜨리면 「서버가 스트림을 깨뜨렸다」는 거짓 진단이 나온다.
#    L5 에서 실제로 겪었다.
function Get-FrameStats([byte[]]$Data) {
    $off = 0
    $byId = @{}
    $partial = 0
    while ($off + 4 -le $Data.Length) {
        $len = ([int]$Data[$off]     -shl 8) -bor [int]$Data[$off + 1]
        $id  = ([int]$Data[$off + 2] -shl 8) -bor [int]$Data[$off + 3]
        if ($off + 4 + $len -gt $Data.Length) { $partial = $Data.Length - $off; break }
        if ($byId.ContainsKey($id)) { $byId[$id]++ } else { $byId[$id] = 1 }
        $off += 4 + $len
    }
    return [pscustomobject]@{ ById = $byId; PartialBytes = $partial }
}

function Read-Available($Stream, $Sink, [byte[]]$Buf) {
    while ($Stream.DataAvailable) {
        $n = $Stream.Read($Buf, 0, $Buf.Length)
        if ($n -le 0) { break }
        $Sink.Write($Buf, 0, $n)
    }
}

if ($Zones -lt 1) { $Zones = 1 }
$perZone = [math]::Floor($Clients / $Zones)
if ($perZone -lt 1) { throw "Clients($Clients) 가 Zones($Zones) 보다 적습니다" }

# 존이 여럿이면 「내 존 사람들 것만」 와야 한다. 다른 존 것이 오면 경계가 샌 것이다.
$expect = $perZone * $Chats

"zones : $ZoneId .. $($ZoneId + $Zones - 1)   ($Zones 개)"
"        존마다 EntryTable::zone_index_ 의 버킷 하나 — 고정 스레드 배정은 없다(app::WorkerPool 이 세션 실행권을 유동 배정한다)"
"client: $Clients 개   존당 $perZone 명   chats/client: $Chats"
"expect: 각 클라가 ChatNtf $expect 개  (자기 존 $perZone 명 × $Chats)"
""

$scratchRoot = Join-Path $env:TEMP ("zone_harness_" + [guid]::NewGuid().ToString('N'))
$listener = $null
$villageProc = $null
try {
    $vhome = New-HarnessHome $scratchRoot ([string]$Port)
    $listener = Start-FakeSession 9100
    $villageProc = Start-Village $Config $vhome $Seconds
    $link = Accept-FakeSessionLink $listener

    # ── 1. 접속 — 예약 발급 → Enter 까지 Connect-Reserved 하나로 끝낸다 ──
    $cli = @()
    for ($i = 0; $i -lt $Clients; $i++) {
        $reserved = Connect-Reserved $link $Port ([uint64]($i + 1)) $Timeout
        $s = $reserved.Stream
        $s.ReadTimeout  = $Timeout
        $s.WriteTimeout = $Timeout
        $cli += [pscustomobject]@{
            Client = $reserved.Client
            Stream = $s
            Sink   = [System.IO.MemoryStream]::new()
            Buf    = [byte[]]::new(65536)
            Zone   = $ZoneId + ($i % $Zones)      # 라운드 로빈 배정
        }
    }
    "connect: $Clients 개 완료(예약 경유)"

# ── 2. 존 입장 ───────────────────────────────────────────────────────
foreach ($x in $cli) {
    $f = New-Frame (New-U32BE $x.Zone) $MSG_JOIN_REQ
    $x.Stream.Write($f, 0, $f.Length)
    $x.Stream.Flush()
}

# JoinAck 가 전부 돌아올 때까지 기다린다.
# 여기서 기다리는 이유 — 입장이 끝나기 전에 채팅을 보내면 그건 「이동 중」이라
#   서버가 조용히 버린다. 그건 정상 동작이지 버그가 아니므로, 측정에서 제외해야 한다.
$deadline = [datetime]::UtcNow.AddMilliseconds($Timeout)
$joined = 0
while ($joined -lt $Clients -and [datetime]::UtcNow -lt $deadline) {
    $joined = 0
    foreach ($x in $cli) {
        Read-Available $x.Stream $x.Sink $x.Buf
        $st = Get-FrameStats $x.Sink.ToArray()
        if ($st.ById.ContainsKey($MSG_JOIN_ACK)) { $joined++ }
    }
    if ($joined -lt $Clients) { Start-Sleep -Milliseconds 50 }
}
"join   : $joined / $Clients 입장 확인"
if ($joined -lt $Clients) {
    "  ! 입장이 다 안 됐다. 서버 로그를 확인할 것"
}

# 입장 단계에서 받은 것은 버린다 — 이제부터 세는 것만 센다
foreach ($x in $cli) { $x.Sink.SetLength(0) }

# ── 3. 채팅 — 라운드 로빈으로 인터리브 ───────────────────────────────
# 한 클라가 몰아서 보내지 않고 번갈아 보낸다.
#   서버 큐에 여러 세션의 Job 이 섞여 들어가야 라우팅이 깨졌을 때 경쟁이 드러난다.
# kChatReq body 맨 앞 1B 가 type 이다(서버 §6-3 레이아웃 추종) — 이
#   하네스는 지역 채팅만 재므로 0(Zone)을 고정으로 선행한다.
$text = [byte[]]::new($Size)
for ($k = 0; $k -lt $Size; $k++) { $text[$k] = [byte](65 + ($k % 26)) }
$body = [byte[]]@(0) + $text
$chatFrame = New-Frame $body $MSG_CHAT_REQ

$sw = [System.Diagnostics.Stopwatch]::StartNew()
for ($m = 0; $m -lt $Chats; $m++) {
    foreach ($x in $cli) {
        $x.Stream.Write($chatFrame, 0, $chatFrame.Length)
    }

    # -Churn : 채팅이 흐르는 「도중에」 멤버 목록을 흔든다.
    #   채팅만 하면 zone_index_ 버킷은 「읽기만」 되므로(snapshot_zone), 레이스가
    #   있어도 증상이 안 나온다 — mutex_ 는 그 읽기끼리도 직렬화하지만, 읽기만
    #   반복해서는 그 직렬화가 실제로 도는지 드러나지 않는다.
    #   입·퇴장을 섞으면 move_zone 이 버킷을 지우고/push_back 하므로(erase_from_bucket
    #   이 swap-and-pop 을 쓴다), 같은 버킷을 스냅샷이 도는 순간과 겹칠 여지가 생긴다.
    #   zone_index_ 갱신 지점 규약(enter·leave·move_zone 셋뿐)이 깨졌을 때만 증상이
    #   보이는 자리다 — 정상 구현이면 mutex_ 가 이 겹침 자체를 막는다.
    #
    #   +1001 인 이유 — 관찰 중인 존과 겹치지 않게.
    if ($Churn -gt 0 -and (($m % $Churn) -eq ($Churn - 1))) {
        for ($j = 1; $j -lt $cli.Count; $j += 2) {
            $x    = $cli[$j]
            $away = New-Frame (New-U32BE ($x.Zone + 1001)) $MSG_JOIN_REQ
            $back = New-Frame (New-U32BE  $x.Zone)         $MSG_JOIN_REQ
            $x.Stream.Write($away, 0, $away.Length)
            $x.Stream.Write($back, 0, $back.Length)
        }
    }
    # 보내는 동안에도 주기적으로 빨아들인다. 안 그러면 소켓 수신 버퍼가 차서
    # 서버 송신이 막히고, 그게 측정이 아니라 흐름 제어를 재는 실험이 된다.
    if (($m % 10) -eq 9) {
        foreach ($x in $cli) { Read-Available $x.Stream $x.Sink $x.Buf }
    }
}
foreach ($x in $cli) { $x.Stream.Flush() }
$sw.Stop()
"send   : $($Clients * $Chats) 개 전송 완료 ($($sw.ElapsedMilliseconds) ms)"

# ── 4. 남은 것 수거 ──────────────────────────────────────────────────
$deadline = [datetime]::UtcNow.AddMilliseconds($Timeout + $Settle)
while ([datetime]::UtcNow -lt $deadline) {
    $done = 0
    foreach ($x in $cli) {
        Read-Available $x.Stream $x.Sink $x.Buf
        $st = Get-FrameStats $x.Sink.ToArray()
        $got = if ($st.ById.ContainsKey($MSG_CHAT_NTF)) { $st.ById[$MSG_CHAT_NTF] } else { 0 }
        if ($got -ge $expect) { $done++ }
    }
    if ($done -eq $Clients) { break }
    Start-Sleep -Milliseconds 100
}
Start-Sleep -Milliseconds $Settle
foreach ($x in $cli) { Read-Available $x.Stream $x.Sink $x.Buf }

# ── 5. 판정 ──────────────────────────────────────────────────────────
""
# -Churn 모드에서는 「개수」로 판정하지 않는다.
#   이동 중에는 그 존의 채팅을 못 받는 게 정상이라 개수가 원래 안 맞는다.
#   대신 「서버가 살아 있나 · 스트림이 멀쩡한가」를 본다. 그게 상태 깨짐의 직접 증거다.
$churnMode = ($Churn -gt 0)
$bad = 0
$idx = 0
$brokenStream = 0
foreach ($x in $cli) {
    $st  = Get-FrameStats $x.Sink.ToArray()
    $got = if ($st.ById.ContainsKey($MSG_CHAT_NTF)) { $st.ById[$MSG_CHAT_NTF] } else { 0 }
    $known = @($MSG_CHAT_NTF, $MSG_JOIN_ACK)
    $other = ($st.ById.Keys | Where-Object { $known -notcontains $_ }) -join ','
    if ($other -or $st.PartialBytes -gt 0) { $brokenStream++ }
    $mark  = if ($churnMode) { '--' } elseif ($got -eq $expect) { 'OK' } else { 'X ' }
    if (-not $churnMode -and $got -ne $expect) { $bad++ }
    $extra = ''
    if ($st.PartialBytes -gt 0) { $extra += "  partial=$($st.PartialBytes)B" }
    if ($other)                 { $extra += "  other-ids=$other" }
    "  client $idx (zone $($x.Zone)) : $mark  ChatNtf $got / $expect$extra"
    $idx++
}

""
if ($churnMode) {
    $alive = [bool](Get-Process -Name village -ErrorAction SilentlyContinue)
    "churn : 채팅 중 입·퇴장을 섞었다 (절반이 $Churn 회마다 옆 존 왕복)"
    "        서버 생존   : $alive"
    "        깨진 스트림 : $brokenStream / $Clients"
    if ($alive -and $brokenStream -eq 0) {
        "판정  : ○ 부하 중 멤버 목록이 흔들려도 서버가 멀쩡하다"
    } else {
        "판정  : ✕ 상태가 깨졌다 — 같은 존을 여러 스레드가 만졌을 때 나오는 증상이다"
    }
} elseif ($bad -eq 0) {
    if ($Zones -gt 1) {
        "판정  : ○ 브로드캐스트가 「자기 존에만」 갔다. 존 경계가 지켜졌다"
        "        (다른 존 것까지 왔다면 $($Clients * $Chats) 개가 됐을 것)"
    } else {
        "판정  : ○ 브로드캐스트 정확. 모든 클라가 $expect 개를 받았다"
    }
} else {
    "판정  : ✕ $bad / $Clients 개 클라의 수신 개수가 어긋남"
    "        기대보다 많으면 존 경계가 샌 것이고,"
    "        적거나 들쭉날쭉하면 같은 존을 여러 스레드가 만진 것이다."
    "        Debug 빌드라면 서버 쪽 assert 로그도 함께 확인할 것."
}

foreach ($x in $cli) {
    $x.Sink.Dispose()
    $x.Client.Close()
}
} finally {
    Stop-Harness $scratchRoot $listener $villageProc
}
