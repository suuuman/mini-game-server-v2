# scripts\inventory.ps1 — 인벤토리 조회 (DB 왕복) 검증 (L13 · L14 에서 수정)
#
#   무엇을 보는가
#     로그인 → kInventoryReq(body 없음) → kInventoryAck 한 바퀴.
#     로직 → DB 워커 → 로직이 실제로 도는지 확인한다.
#
#   L14 에서 바뀐 것 — 조회 대상을 body 로 보내지 않는다.
#     L13 은 kInventoryReq 에 player_id 를 실었고, 그래서 아무나 남의 인벤토리를
#     볼 수 있었다. 이제 「이 세션이 누구인가」로 정해진다.
#     → 한 연결은 한 플레이어다. -Mixed 가 연결을 여러 개 만드는 이유가 이것이다.
#
#   기대값 (sql\01_schema.sql 의 테스트 데이터)
#     player 1 (alice) : item 100 x10, item 200 x5
#     player 2 (bob)   : item 100 x3
#     player 99        : 없음 → count 0   (플레이어가 없어도 조회 자체는 성공한다)
#
#   사용:
#     .\inventory.ps1 -Player 1
#     .\inventory.ps1 -Player 1 -Repeat 20      # 같은 워커로 가는지 (서버 로그의 [DB ] tid)
#     .\inventory.ps1 -Mixed                    # 1,2,99 를 각각 별도 연결로
#     .\inventory.ps1 -NoLogin                  # 로그인 없이 조회 → kNotLoggedIn 이어야 정상
#
#   ──  DB 상태 조성 스위치 (N1·N2·N4·N5) ────────────────────────
#   아래 넷은 전부 "DB 상태 조성 → 서버 재기동 → 이 스위치로 실행 → 원복" 이
#     한 묶음이다. `prepare_*` 는 기동 시에만 돈다 — 서버를 안 내리면 SP 를
#     DROP 해도 이미 준비된 문장이 남아 있어 「통과했다」가 거짓말이 된다.
#     상태 조성 SQL 은 이 파일 밖에서 손으로 친다.
#     (이 스크립트는 SQL 을 치지 않는다 — 프로토콜 클라이언트 성격을 지킨다).
#
#     .\inventory.ps1 -Mixed -ExpectDbError
#       N1. 「실패 vs 빈 결과」 구분. SP 가 미배포(또는 강제 실패)된 상태에서 돌린다.
#       count=0·result=OK 로 온 응답은 "조용한 빈 목록" 버그이므로 **판정을 X 로 떨어뜨린다** —
#         행이 0개인 것과 질의가 실패한 것은 다른 사건이다(CODING_RULES.md §3).
#
#     .\inventory.ps1 -Player 901 -ExpectCount 511
#       N2. kMaxRows(=511) 경계 확인. -Player 는 seed_load.ps1 -BulkPlayer 로 미리 심어 둔
#       900(510행)/901(511행)/902(512행 → 511로 잘림) 중 하나를 겨눈다.
#
#     .\inventory.ps1 -Mixed -AfterPartialDeploy
#       N4·N5. 부분 배포(거래 SP 만 / 조회 SP 만) 상태에서 돌린다.
#       이 스위치는 자체 판정을 안 내린다 — 기대값이 「폴백 생존」과 「폴백 제거」 사이에서
#       갈리기 때문이다(N4·N5 의 기대값은 서로 비대칭이다).
#       진짜 값어치는 이 응답 자체가 아니라, **바로 이어서 돌리는 `trade.ps1`(N6)** 이 커넥션 오염 없이
#       통과하는가다 — 판정은 그쪽이 낸다.

param(
    [int]$Port    = 9000,
    [int]$Player  = 1,
    [int]$Repeat  = 1,
    [int]$Timeout = 5000,
    [switch]$Mixed,               # 1,2,99 를 각각 별도 연결로
    [switch]$NoLogin,             # 로그인을 건너뛴다 — 거부되는지 보는 용도
    [switch]$ExpectDbError,       # N1 — 모든 응답이 정확히 kDbError(=1) 인지 검사한다
    [int]$ExpectCount = -1,       # N2 — 모든 응답의 count 가 이 값과 정확히 같은지 검사한다 (-1 = 검사 안 함)
    [switch]$AfterPartialDeploy,  # N4·N5 — 부분 배포 상태 표시용. 판정은 이 스위치가 안 내린다(위 사용법 참조)
    [string]$Config  = 'Release', # 예약 경유 접속을 위해 스스로 스폰할 village.exe 구성
    [int]$Seconds    = 120        # 스폰한 village.exe 의 수명(초)
)

$ErrorActionPreference = 'Stop'

# kLoginReq/직행 접속이 폐지돼서 이 하네스도 예약을 거쳐야 한다.
#   DB 왕복만 재는 게 목적이라 실물 session.exe 는 안 띄운다 — 이 파일이
#   가짜 세션 서버 노릇을 해서 스스로 띄운 village.exe 에 직접 Reserve 를
#   찔러 넣는다. harness_common.ps1 참조.
#   -NoLogin 은 여전히 "Enter 자체를 안 한다"로 재현한다 — 로그인 없이
#   조회가 거부되는지 보는 목적이 그대로 유지된다.
. (Join-Path $PSScriptRoot 'harness_common.ps1')

$MSG_INV_REQ   = 4
$MSG_INV_ACK   = 104

# proto\packet.h 의 ResultCode 와 같은 순서
$RESULT_NAME = @('OK', 'DB오류', '잔량부족', '인자오류', '바쁨', '로그인안함', '상대없음')

function Get-ResultName([int]$Code) {
    if ($Code -ge 0 -and $Code -lt $RESULT_NAME.Count) { return $RESULT_NAME[$Code] }
    return "알수없음($Code)"
}

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

function New-U32BE([int]$Value) {
    $b = [byte[]]::new(4)
    $b[0] = [byte](($Value -shr 24) -band 0xFF)
    $b[1] = [byte](($Value -shr 16) -band 0xFF)
    $b[2] = [byte](($Value -shr  8) -band 0xFF)
    $b[3] = [byte]( $Value          -band 0xFF)
    return $b
}

# [int] 캐스팅 필수. PowerShell 의 -shl 은 왼쪽이 [byte] 면 결과도 byte 로 자른다.
#    L5 에서 이것 때문에 「서버가 스트림을 깨뜨렸다」는 거짓 진단이 나왔다.
function Get-U16([byte[]]$d, [int]$off) {
    return ([int]$d[$off] -shl 8) -bor [int]$d[$off + 1]
}
function Get-U32([byte[]]$d, [int]$off) {
    return ([int]$d[$off]     -shl 24) -bor ([int]$d[$off + 1] -shl 16) -bor `
           ([int]$d[$off + 2] -shl  8) -bor  [int]$d[$off + 3]
}

# 프레임 $Want 개가 모일 때까지 읽는다
function Read-Frames($Stream, [int]$Want, [int]$TimeoutMs) {
    $sink = [System.IO.MemoryStream]::new()
    $buf  = [byte[]]::new(65536)
    $deadline = [datetime]::UtcNow.AddMilliseconds($TimeoutMs)
    try {
        while ([datetime]::UtcNow -lt $deadline) {
            if ($Stream.DataAvailable) {
                $n = $Stream.Read($buf, 0, $buf.Length)
                if ($n -le 0) { break }
                $sink.Write($buf, 0, $n)
            } else {
                Start-Sleep -Milliseconds 30
            }
            if ($sink.Length -gt 0) {
                $tmp = $sink.ToArray(); $off = 0; $cnt = 0
                while ($off + 4 -le $tmp.Length) {
                    $len = Get-U16 $tmp $off
                    if ($off + 4 + $len -gt $tmp.Length) { break }
                    $cnt++; $off += 4 + $len
                }
                if ($cnt -ge $Want) { break }
            }
        }
    } catch [System.IO.IOException] { }
    $data = $sink.ToArray()
    $sink.Dispose()
    return ,$data
}

# ── 한 연결 = 한 플레이어 ────────────────────────────────────────────
function Invoke-PlayerSession([int]$PlayerId, [int]$Times, [bool]$SkipLogin) {
    $loginText = '건너뜀'
    if ($SkipLogin) {
        # -NoLogin 의 목적은 "Enter 를 아예 안 한 세션이 거부되는가" 다 —
        #   예약도 받지 않고 그냥 맨몸으로 붙인다.
        $client = New-Object System.Net.Sockets.TcpClient
        $client.NoDelay = $true
        $client.Connect('127.0.0.1', $Port)
        $stream = $client.GetStream()
        $stream.ReadTimeout  = $Timeout
        $stream.WriteTimeout = $Timeout
    } else {
        $reserved = Connect-Reserved $link $Port ([uint64]$PlayerId) $Timeout
        $client = $reserved.Client
        $stream = $reserved.Stream
        $stream.ReadTimeout  = $Timeout
        $stream.WriteTimeout = $Timeout
        $loginText = "OK (player=$($reserved.PlayerId) session=$($reserved.SessionId))"
    }

    for ($i = 0; $i -lt $Times; $i++) {
        $f = New-Frame $null $MSG_INV_REQ      # body 0 바이트
        $stream.Write($f, 0, $f.Length)
    }
    $stream.Flush()

    $data = Read-Frames $stream $Times $Timeout

    # N1·N2 는 표시된(최대 3개) 것뿐 아니라 **모든** 응답의 result/count 를 검사해야 한다 —
    #   그래서 $results·$counts 는 acks 전부를 담는다. $lines 는 화면 출력용(기존 3개 제한)만 남긴다.
    $off = 0; $acks = 0; $bad = 0; $lines = @(); $results = @(); $counts = @()
    while ($off + 4 -le $data.Length) {
        $len = Get-U16 $data $off
        $id  = Get-U16 $data ($off + 2)
        if ($off + 4 + $len -gt $data.Length) { $lines += "  ! 불완전 프레임"; break }

        $b = $off + 4
        if ($id -eq $MSG_INV_ACK) {
            $acks++
            # [result:u8][count:u16]( [item_id:u32][item_count:i32] x count )
            $result = [int]$data[$b]
            $count  = Get-U16 $data ($b + 1)
            $results += $result
            $counts  += $count
            $items  = @()
            for ($i = 0; $i -lt $count; $i++) {
                $p = $b + 3 + ($i * 8)
                $itemId = Get-U32 $data $p
                $qtyRaw = Get-U32 $data ($p + 4)
                # 서버가 int32 를 비트 그대로 싣는다. 부호를 되살린다.
                $qty = if ($qtyRaw -gt 2147483647) { $qtyRaw - 4294967296 } else { $qtyRaw }
                $items += "item $itemId x$qty"
            }
            if ($result -ne 0) { $bad++ }
            if ($acks -le 3) {
                $shown = if ($result -ne 0) { '-' } elseif ($count -eq 0) { '(없음)' } else { $items -join ' · ' }
                $lines += "    ack #$acks : $(Get-ResultName $result)  count=$count   $shown"
            }
        } else {
            $lines += "    예상 밖 msg_id=$id"
        }
        $off += 4 + $len
    }

    $client.Close()
    return @{ Player = $PlayerId; Login = $loginText; Acks = $acks; Bad = $bad; Lines = $lines; Results = $results; Counts = $counts }
}

# ── 실행 ─────────────────────────────────────────────────────────────
$scratchRoot = Join-Path $env:TEMP ("inventory_harness_" + [guid]::NewGuid().ToString('N'))
$listener = $null
$villageProc = $null
try {
$vhome = New-HarnessHome $scratchRoot ([string]$Port)
$listener = Start-FakeSession 9100
$villageProc = Start-Village $Config $vhome $Seconds
$link = Accept-FakeSessionLink $listener

$targets = if ($Mixed) { @(1, 2, 99) } else { @($Player) }

Write-Host ""
$totalAck = 0; $totalBad = 0; $allResults = @(); $allCounts = @()
foreach ($t in $targets) {
    $r = Invoke-PlayerSession $t $Repeat ([bool]$NoLogin)
    Write-Host "  player $($r.Player)  로그인: $($r.Login)   응답 $($r.Acks)/$Repeat"
    foreach ($l in $r.Lines) { Write-Host $l }
    $totalAck += $r.Acks
    $totalBad += $r.Bad
    $allResults += $r.Results
    $allCounts  += $r.Counts
}

$want = $targets.Count * $Repeat
Write-Host ""

if ($ExpectDbError) {
    # N1 — 「실패 vs 빈 결과」. count 는 절대 보지 않는다: OK 인데 count=0 이 오는 것이
    #   바로 CODING_RULES.md §3 이 금지한 「조용한 빈 목록」이다. result 코드만 근거로 삼는다.
    $kDbError   = 1   # $RESULT_NAME 인덱스 — proto::ResultCode::kDbError
    $silentOk   = 0   # OK(0) 인데 count=0 인 응답 개수 — 있으면 그 자체가 버그의 증거
    $wrongOther = 0   # kDbError 도 아니고 「조용한 빈 목록」도 아닌, 그 밖의 예상 밖 응답
    for ($i = 0; $i -lt $allResults.Count; $i++) {
        if ($allResults[$i] -eq $kDbError) { continue }
        if ($allResults[$i] -eq 0 -and $allCounts[$i] -eq 0) { $silentOk++ }
        else { $wrongOther++ }
    }
    $errCount = ($allResults | Where-Object { $_ -eq $kDbError }).Count
    # $wrongOther 는 「받은 응답 중」 kDbError 도 조용한빈목록도
    #   아닌 것만 센다($allResults.Count == $totalAck 라서다). 응답 자체가 안 온
    #   것(누락, want - totalAck)은 여기 안 잡힌다 — 「누락」을 「예상 밖」으로
    #   뭉뚱그리지 않고 아래에서 따로 표시한다.
    $missing = $want - $totalAck
    if ($totalAck -eq $want -and $errCount -eq $want) {
        Write-Host "판정  : O $errCount / $want 전부 DB오류로 왔다 — 실패가 빈 목록으로 안 흐른다"
    } elseif ($silentOk -gt 0) {
        Write-Host "판정  : X $silentOk 건이 OK·count=0 으로 왔다 — 「조용한 빈 목록」이다(§3 위반)"
        Write-Host "        실패와 빈 결과를 구분 못 하고 있다. 열 수 검사 / kDbError 전파를 확인할 것"
    } else {
        $detail = "예상 밖 결과 $wrongOther 건"
        if ($missing -gt 0) { $detail += " · 응답 자체가 누락된 것 $missing 건" }
        Write-Host "판정  : X $errCount / $want 만 DB오류다. 나머지 — $detail"
    }
} elseif ($ExpectCount -ge 0) {
    # N2 — kMaxRows 경계. result 는 전부 OK 여야 하고, count 는 전부 $ExpectCount 와 같아야 한다.
    $mismatch  = ($allCounts | Where-Object { $_ -ne $ExpectCount }).Count
    $notOk     = ($allResults | Where-Object { $_ -ne 0 }).Count
    if ($totalAck -eq $want -and $mismatch -eq 0 -and $notOk -eq 0) {
        Write-Host "판정  : O $totalAck / $want 전부 count=$ExpectCount — kMaxRows 상계가 파싱 위치와 무관하게 맞다"
    } else {
        Write-Host "판정  : X count 불일치 $mismatch 건 · 비정상 result $notOk 건 (기대 count=$ExpectCount)"
    }
} elseif ($AfterPartialDeploy) {
    # N4·N5 — 이 스위치는 스스로 O/X 를 내지 않는다. 위 사용법 주석 참조.
    Write-Host "판정  : (없음) N4·N5 는 응답만 기록한다 — 위 result/count 를 SP 배포 상태와 눈으로 대조하라"
    Write-Host "        ⛔ 진짜 판정은 이 응답이 아니라, 이어서 돌리는 trade.ps1(N6) 의 성공 여부다"
    Write-Host "        (커넥션이 오염됐다면 그 요청이 아니라 다음 요청이 죽는다)"
} elseif ($NoLogin) {
    # 이 경로는 「거부되는 것」이 정상이다. kNotLoggedIn = 5
    if ($totalAck -eq $want -and $totalBad -eq $want) {
        Write-Host "판정  : O $totalAck / $want 전부 거부됐다 — 로그인 없이는 조회할 수 없다"
    } else {
        Write-Host "판정  : X 로그인 없이 조회가 통과했다. 세션 바인딩이 안 걸려 있다"
    }
} elseif ($totalAck -eq $want -and $totalBad -eq 0) {
    Write-Host "판정  : O $totalAck / $want 응답. DB 왕복이 돈다"
} elseif ($totalAck -eq $want) {
    Write-Host "판정  : ~ $totalAck / $want 응답은 왔지만 $totalBad 건이 실패다"
    Write-Host "        결과 코드를 볼 것 — 「빈 인벤토리」와 「못 읽었다」는 다르다"
} else {
    Write-Host "판정  : X $totalAck / $want — 응답이 모자란다. 서버 로그의 [WARN] 을 확인할 것"
}
} finally {
    Stop-Harness $scratchRoot $listener $villageProc
}
