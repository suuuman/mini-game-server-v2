# ============================================================================
#  scripts\members.ps1 — 존 멤버 목록 통지(kZoneMembersNtf) 검증
#
#  무엇을 검증하는가 — 「멤버가 바뀌면 그 존 전원이 같은 목록을 받는다」
#    이 하나가 깨지면 거래를 시작할 수 없다.
#      kTradeReqReq 는 상대를 session_id 로 지목하는데, 그 값을 아는 경로가 이 통지뿐이다.
#
#  검증 항목 (4개 변동 지점에 대응)
#    ① 입장     — 들어가면 목록이 온다 (본인 포함)          [지점 3/4]
#    ② 갱신     — 두 번째가 들어오면 첫 번째도 갱신을 받는다 [지점 3/4]
#    ③ 존 이동  — 나가면 남은 쪽이 갱신을 받는다             [지점 1/4]
#    ④ 접속 종료 — 끊으면 남은 쪽이 갱신을 받는다            [지점 4/4]
#
#  지점 2/4(이동 롤백)는 여기서 못 만든다 — add 직후 closing 을 만들어야 하는데
#    그 창이 마이크로초 단위다. 그건 churn.ps1 이 확률적으로 밟는다.
#
#  사용:
#    .\members.ps1
#    .\members.ps1 -ZoneId 40 -Port 9000
# ============================================================================

param(
    [int]$Port    = 9000,
    [int]$ZoneId  = 40,
    [int]$Timeout = 3000,
    [string]$Config  = 'Release',   # 예약 경유 접속을 위해 스스로 스폰할 village.exe 구성
    [int]$Seconds    = 60           # 스폰한 village.exe 의 수명(초)
)

$ErrorActionPreference = 'Stop'

# kLoginReq/직행 접속이 폐지돼서 이 하네스도 예약을 거쳐야 한다.
#   kZoneMembersNtf 배선만 재는 게 목적이라 실물 session.exe 는 안 띄운다 —
#   이 파일이 가짜 세션 서버 노릇을 해서 스스로 띄운 village.exe 에 직접
#   Reserve 를 찔러 넣는다. harness_common.ps1 참조.
. (Join-Path $PSScriptRoot 'harness_common.ps1')

# proto::MsgId
$MSG_JOIN_REQ    = 2
$MSG_JOIN_ACK    = 102
$MSG_MEMBERS_NTF = 112

$pass = 0
$fail = 0

function Test-Result([bool]$Ok, [string]$Label) {
    if ($Ok) {
        $script:pass++
        "   [O] $Label"
    } else {
        $script:fail++
        "   [X] $Label"
    }
}

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

# [int] 캐스팅이 필수다 — zone.ps1 의 같은 주석 참조.
#    [byte] -shl 8 은 0 이 된다. 빠뜨리면 「서버가 스트림을 깨뜨렸다」는 거짓 진단이 나온다.
function Read-Frames($Stream, [byte[]]$Buf) {
    $ms = [System.IO.MemoryStream]::new()
    $deadline = [DateTime]::UtcNow.AddMilliseconds(600)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($Stream.DataAvailable) {
            $n = $Stream.Read($Buf, 0, $Buf.Length)
            if ($n -gt 0) { $ms.Write($Buf, 0, $n) }
        } else {
            Start-Sleep -Milliseconds 20
        }
    }
    $data = $ms.ToArray()
    $frames = @()
    $off = 0
    while ($off + 4 -le $data.Length) {
        $len = ([int]$data[$off]     -shl 8) -bor [int]$data[$off + 1]
        $id  = ([int]$data[$off + 2] -shl 8) -bor [int]$data[$off + 3]
        if ($off + 4 + $len -gt $data.Length) { break }
        $body = [byte[]]::new($len)
        if ($len -gt 0) { [Array]::Copy($data, $off + 4, $body, 0, $len) }
        $frames += [pscustomobject]@{ Id = $id; Body = $body }
        $off += 4 + $len
    }
    return $frames
}

# kZoneMembersNtf body → session_id 배열
#   body: [ count : u16 ]( [ session_id : u32 ][ player_id : u32 ] x count )
function Get-Members([byte[]]$Body) {
    if ($Body.Length -lt 2) { return $null }
    $count = ([int]$Body[0] -shl 8) -bor [int]$Body[1]
    $ids = @()
    for ($i = 0; $i -lt $count; $i++) {
        $o = 2 + $i * 8
        if ($o + 8 -gt $Body.Length) { break }
        $sid = ([int]$Body[$o] -shl 24) -bor ([int]$Body[$o+1] -shl 16) -bor `
               ([int]$Body[$o+2] -shl 8) -bor [int]$Body[$o+3]
        $ids += $sid
    }
    return [pscustomobject]@{ Count = $count; Ids = $ids }
}

function Connect-Client([uint64]$PlayerId) {
    $reserved = Connect-Reserved $link $Port $PlayerId $Timeout
    $s = $reserved.Stream
    $s.ReadTimeout  = $Timeout
    $s.WriteTimeout = $Timeout
    return [pscustomobject]@{ Client = $reserved.Client; Stream = $s; Buf = [byte[]]::new(65536) }
}

function Send-Join($X, [int]$Zone) {
    $f = New-Frame (New-U32BE $Zone) $MSG_JOIN_REQ
    $X.Stream.Write($f, 0, $f.Length)
}

# 마지막 kZoneMembersNtf 를 돌려준다 (여러 번 왔으면 최신이 진실이다)
function Get-LastMembers($Frames) {
    $ntf = @($Frames | Where-Object { $_.Id -eq $MSG_MEMBERS_NTF })
    if ($ntf.Count -eq 0) { return $null }
    return Get-Members $ntf[-1].Body
}

"members: kZoneMembersNtf 검증   zone=$ZoneId (EntryTable::zone_index_ 버킷 $ZoneId — 고정 스레드 배정 없음)"
""

$scratchRoot = Join-Path $env:TEMP ("members_harness_" + [guid]::NewGuid().ToString('N'))
$listener = $null
$villageProc = $null
try {
$vhome = New-HarnessHome $scratchRoot ([string]$Port)
$listener = Start-FakeSession 9100
$villageProc = Start-Village $Config $vhome $Seconds
$link = Accept-FakeSessionLink $listener

# ── ① 입장 — 첫 번째 클라이언트 ──────────────────────────────────────
"== (1) 입장하면 목록이 온다 =="
$a = Connect-Client 1
Send-Join $a $ZoneId
$fa = Read-Frames $a.Stream $a.Buf

$ackA = @($fa | Where-Object { $_.Id -eq $MSG_JOIN_ACK })
Test-Result ($ackA.Count -eq 1) "JoinZoneAck 를 받았다"

$mA = Get-LastMembers $fa
Test-Result ($null -ne $mA) "kZoneMembersNtf 를 받았다"
if ($null -ne $mA) {
    Test-Result ($mA.Count -eq 1) "목록에 1 명 (본인) — 실제: $($mA.Count)"
    $sidA = if ($mA.Ids.Count -gt 0) { $mA.Ids[0] } else { 0 }
    "       내 session_id = $sidA"
}
""

# ── ② 두 번째 입장 — 기존 멤버도 갱신을 받는가 ───────────────────────
"== (2) 두 번째가 들어오면 첫 번째도 갱신을 받는다 =="
$b = Connect-Client 2
Send-Join $b $ZoneId
$fb = Read-Frames $b.Stream $b.Buf
$fa2 = Read-Frames $a.Stream $a.Buf

$mB = Get-LastMembers $fb
Test-Result ($null -ne $mB -and $mB.Count -eq 2) "B 의 목록에 2 명 — 실제: $(if ($mB) { $mB.Count } else { 'none' })"

$mA2 = Get-LastMembers $fa2
Test-Result ($null -ne $mA2) "★ A 도 갱신 통지를 받았다 (이게 없으면 새 사람이 안 보인다)"
if ($null -ne $mA2) {
    Test-Result ($mA2.Count -eq 2) "A 의 갱신 목록도 2 명 — 실제: $($mA2.Count)"
    Test-Result (($mA2.Ids -contains $sidA)) "A 의 목록에 자기 session_id 가 있다"
}
""

# ── ③ 존 이동 — 남은 쪽이 갱신을 받는가 ──────────────────────────────
"== (3) B 가 다른 존으로 가면 A 가 갱신을 받는다 =="
Send-Join $b ($ZoneId + 1)
[void](Read-Frames $b.Stream $b.Buf)
$fa3 = Read-Frames $a.Stream $a.Buf

$mA3 = Get-LastMembers $fa3
Test-Result ($null -ne $mA3) "★ A 가 퇴장 갱신을 받았다 (없으면 유령 멤버가 남는다)"
if ($null -ne $mA3) {
    Test-Result ($mA3.Count -eq 1) "A 의 목록이 1 명으로 줄었다 — 실제: $($mA3.Count)"
}
""

# ── ④ 접속 종료 — 남은 쪽이 갱신을 받는가 ────────────────────────────
"== (4) C 가 접속을 끊으면 A 가 갱신을 받는다 =="
$c = Connect-Client 3
Send-Join $c $ZoneId
[void](Read-Frames $c.Stream $c.Buf)
[void](Read-Frames $a.Stream $a.Buf)      # 입장 갱신은 흘려보낸다

$c.Client.Close()
Start-Sleep -Milliseconds 300
$fa4 = Read-Frames $a.Stream $a.Buf

$mA4 = Get-LastMembers $fa4
Test-Result ($null -ne $mA4) "★ A 가 종료 갱신을 받았다 (on_session_gone 경로)"
if ($null -ne $mA4) {
    Test-Result ($mA4.Count -eq 1) "A 의 목록이 다시 1 명 — 실제: $($mA4.Count)"
}
""

# ── 정리 ─────────────────────────────────────────────────────────────
$a.Client.Close()
$b.Client.Close()

""
if ($fail -eq 0) {
    "판정  : O $pass 개 통과 / 0 실패 — 4 개 변동 지점이 전부 통지를 낸다"
    $exitCode = 0
} else {
    "판정  : X $fail 개 실패 / $($pass + $fail) — 서버 로그의 [WARN] 을 볼 것"
    $exitCode = 1
}
} finally {
    Stop-Harness $scratchRoot $listener $villageProc
}
exit $exitCode
