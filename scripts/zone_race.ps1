# scripts\zone_race.ps1 — 존 이동 폭탄 + 교차 요청이 락 계층·명부 정합을 어긋내는지 본다
#
#   무엇을 재현하나 —
#     JoinZoneReq 두 개를 「한 번의 write」로 보낸다. 옛 구조에서는 이게 「배정
#     (placement)을 두 번 읽는 창」을 노렸다 — 프레임을 어느 존 스레드 큐에
#     넣을지 정할 때 한 번, 핸들러가 어느 방을 만질지 정할 때 또 한 번 읽어서
#     그 사이에 방 이동이 값을 바꾸면 A 존 스레드가 B 존 스레드 소유 자료구조를
#     만졌다. 존 스레드 자체(zone_manager.cpp)가 지워지고 그 자리가
#     app::WorkerPool(세션당 직렬 큐 실행권)로 바뀌면서 그 축은 구조적으로
#     없어졌다 — 같은 세션의 두 프레임이 동시에 도는 일이 없다(worker_pool.h
#     주석). 그래서 이 하네스가 겨누는 축도 바뀐다:
#
#   지금 무엇을 보는가 —
#     handle_join_zone 은 L1(EntryTable::move_zone — kRoster)로 인덱스를
#     옮긴 뒤 L2(자기·거래 상대 정리 — kSession)를 잡는다. 이 순서가
#     거꾸로 되는 구현(L2 를 먼저 쥔 채 L1 API — snapshot_zone 등 — 를
#     부르는 것)이 들어오면 core/lock_rank.h 의 LockRankGuard 가 Debug 빌드
#     에서 assert 로 잡는다("락 계층 역순"). 많은 세션이 동시에 JoinZone 을
#     쏟아내 이 경로를 뜨겁게 돌리는 것이 이 하네스의 역할이다 — 정적으로
#     코드만 읽어서는 「그 경로가 실제로 자주 실행되는가」를 알 수 없다.
#
#   판정 셋 —
#     ① Debug 에서 lock_rank 검사기 assert 무발화(락 계층 역순이 없다)
#     ② 서버 생존(그 경합 자체가 다른 방식으로 서버를 죽이지 않는다)
#     ③ 스트림 정합(응답 프레임이 깨지지 않았다 — 알려진 MsgId 만 오고
#        헤더 경계가 안 어긋난다. zone.ps1 의 Get-FrameStats 와 같은 검사다)
#
#   ⛔ 이 판정의 유효성은 판정 자체만으로는 증명되지 않는다 — "항상 통과"인
#     검사기는 메커니즘이 하나도 안 돌아도 통과한다(총계 상쇄 교훈).
#     뮤턴트(M2 — frame_router 에 L2 든 채 L1 호출을 주입)로 이
#     하네스가 실제로 assert 를 잡아내는지 실증한 뒤에만 이 판정을 믿는다
#     (정상 통과만으로 폐합하지 않는다).
#
#   증상(구현이 역순이면) —
#     Debug : lock_rank.h 의 assert 가 터지고 프로세스가 대화상자에 매달린다
#             (--seconds 자동 종료도 안 먹는다. 「응답 없음」으로 보인다)
#     ASan  : 같은 assert 가 abort 로 즉시 프로세스를 죽인다(_DEBUG 정의—
#             common.props — 라 검사기 자체는 살아 있지만 대화상자 대신
#             프로세스 종료로 나타난다. 이 하네스의 감지 방식은 대화상자
#             제목 문자열이라 ASan 에서는 "서버가 죽었다" 갈래로 잡힌다)
#     Release: 검사기가 NDEBUG 로 no-op 이라 아무 신호도 없다 — 이 하네스를
#             Release 로 돌리면 애초에 볼 수 있는 게 없다(그래서 기본 구성을
#             Debug 로 바꿨다).
#
#   사용:
#     .\zone_race.ps1                # 3000 쌍 (Debug)
#     .\zone_race.ps1 -Pairs 200     # 짧게
param(
    [int]$Port    = 9000,
    [int]$Pairs   = 3000,
    [string]$Config  = 'Debug',     # 예약 경유 접속을 위해 스스로 스폰할 village.exe 구성
                                     # lock_rank 검사기가 Debug/ASan 에서만 살아 있다(_DEBUG 정의)
    [int]$Seconds    = 60           # 스폰한 village.exe 의 수명(초)
)

$ErrorActionPreference = 'Stop'

# kLoginReq/직행 접속이 폐지돼서 이 하네스도 예약을 거쳐야 한다.
#   락 계층·명부 정합만 재는 게 목적이라 실물 session.exe 는 안 띄운다 —
#   이 파일이 가짜 세션 서버 노릇을 해서 스스로 띄운 village.exe 에 직접
#   Reserve 를 찔러 넣는다. harness_common.ps1 참조.
. (Join-Path $PSScriptRoot 'harness_common.ps1')

# proto::MsgId — 이 하네스가 받을 수 있는 것은 이 둘뿐이다. 다른 id 가 오면
#   프레이밍이 깨졌거나 엉뚱한 핸들러가 응답한 것이다.
$MSG_JOIN_ZONE_ACK   = 102
$MSG_ZONE_MEMBERS_NTF = 112

function New-Frame([int]$Id, [byte[]]$Body) {
    $n = $Body.Length
    $f = New-Object byte[] (4 + $n)
    $f[0] = [byte](($n -shr 8) -band 0xFF); $f[1] = [byte]($n -band 0xFF)
    $f[2] = [byte](($Id -shr 8) -band 0xFF); $f[3] = [byte]($Id -band 0xFF)
    if ($n -gt 0) { [Array]::Copy($Body, 0, $f, 4, $n) }
    return $f
}
function New-U32BE([uint32]$v) {
    return [byte[]]@([byte](($v -shr 24) -band 0xFF), [byte](($v -shr 16) -band 0xFF),
                     [byte](($v -shr 8) -band 0xFF), [byte]($v -band 0xFF))
}

# zone.ps1 의 Get-FrameStats 와 같은 계산이다([int] 캐스팅 필수 — PowerShell 의
#   -shl 은 왼쪽이 [byte] 면 결과도 [byte] 로 잘린다). 여기서는 개수가 아니라
#   "알려지지 않은 id·잘린 프레임이 있는가"만 본다 — 그게 스트림 정합의 전부다.
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

$scratchRoot = Join-Path $env:TEMP ("zone_race_harness_" + [guid]::NewGuid().ToString('N'))
$listener = $null
$villageProc = $null
try {
    $vhome = New-HarnessHome $scratchRoot ([string]$Port)
    $listener = Start-FakeSession 9100
    $villageProc = Start-Village $Config $vhome $Seconds
    $link = Accept-FakeSessionLink $listener

    $reserved = Connect-Reserved $link $Port ([uint64]1)
    $cli = $reserved.Client
    $st = $reserved.Stream

    "send  : JoinZone(a) + JoinZone(b) 를 한 번의 write 로 $Pairs 쌍"
    "        a % 4 = 1 · b % 4 = 2  (서로 다른 존 — 이동이 매번 실제로 갈아탄다)"
    "        같은 세션의 직렬 큐 실행권이 유일한 실행자라 두 JoinZone 이 동시에 돌 일은
                없다 — 이 하네스가 보는 것은 그 직렬 흐름 안에서 L1/L2 순서가 지켜지는가다"

    $sink = New-Object System.IO.MemoryStream
    $buf  = [byte[]]::new(65536)

    for ($i = 0; $i -lt $Pairs; $i++) {
        $f1 = New-Frame 2 (New-U32BE ([uint32](1 + $i * 8)))
        $f2 = New-Frame 2 (New-U32BE ([uint32](2 + $i * 8)))
        $both = New-Object byte[] ($f1.Length + $f2.Length)
        [Array]::Copy($f1, 0, $both, 0, $f1.Length)
        [Array]::Copy($f2, 0, $both, $f1.Length, $f2.Length)
        $st.Write($both, 0, $both.Length)      # 한 번의 write = 한 번의 drain 으로 둘 다 직렬 큐에 들어간다
        if ($i % 50 -eq 0) { $st.Flush(); Read-Available $st $sink $buf }
    }
    $st.Flush()
    Start-Sleep -Milliseconds 2000

    # 이름으로 찾지 않고 이 하네스가 스폰한 프로세스를 그대로 본다 — 이유는
    #   기존 zone_race.ps1 주석 그대로다(여러 워크트리·세션이 같이 떠 있으면
    #   이름 검색이 배열이 되어 판정이 어긋난다).
    $p = $villageProc
    if ($null -ne $p) { try { $p.Refresh() } catch {} }

    $judgeOk = $true
    if ($null -eq $p -or $p.HasExited) {
        Write-Host "판정1 : X 서버가 죽었다(생존 실패)" -ForegroundColor Red
        $judgeOk = $false
    }
    elseif ($p.MainWindowTitle -like '*Runtime Library*' -or $p.MainWindowTitle -like '*Assert*') {
        Write-Host "판정1 : X lock_rank 검사기 assert 발화 — '$($p.MainWindowTitle)'" -ForegroundColor Red
        Write-Host "        L1/L2 락 계층이 역순으로 잡혔다(core/lock_rank.h)."
        $judgeOk = $false
    }
    else {
        Write-Host "판정1 : O 서버 생존 · lock_rank assert 무발화" -ForegroundColor Green
    }

    # 스트림 정합 — 남은 응답을 마저 걷는다(짧은 폴링 — zone.ps1 의 Settle 과 같은 이유:
    #   서버가 아직 못 보낸 응답이 있을 수 있다).
    if ($judgeOk) {
        $deadline = [datetime]::UtcNow.AddMilliseconds(1000)
        while ([datetime]::UtcNow -lt $deadline) {
            Read-Available $st $sink $buf
            Start-Sleep -Milliseconds 50
        }
        $stats = Get-FrameStats $sink.ToArray()
        $known = @($MSG_JOIN_ZONE_ACK, $MSG_ZONE_MEMBERS_NTF)
        $other = ($stats.ById.Keys | Where-Object { $known -notcontains $_ }) -join ','
        $ackN = if ($stats.ById.ContainsKey($MSG_JOIN_ZONE_ACK)) { $stats.ById[$MSG_JOIN_ZONE_ACK] } else { 0 }
        $ntfN = if ($stats.ById.ContainsKey($MSG_ZONE_MEMBERS_NTF)) { $stats.ById[$MSG_ZONE_MEMBERS_NTF] } else { 0 }

        # 미지 id·파손만 보면 「같은 id 의 정상 형태 프레임이 중복·유실됐다」는
        #   못 잡는다 — 헤더도 msg_id 도 멀쩡해 스트림 자체는 안 깨지기 때문이다
        #   (뮤턴트 실증 — JoinZoneAck 를 2 회 발신해도 이 검사 하나로는 안 걸렸다).
        #   그래서 개수까지 하드 단언한다. 이 시나리오는 각 JoinZoneReq 가 한 번도
        #   재방문하지 않는 zone id 를 쓴다(위 send 안내 — i*8 로 계속 증가하는
        #   값) — 세션이 그 존의 유일한 멤버라 새 존 브로드캐스트 1건만 나가고,
        #   떠난 존은 그 즉시 비어 옛 존 브로드캐스트는 0건이다. 그래서 기대값은
        #   요청 수(2×Pairs)와 정확히 같다 — JoinZoneAck·ZoneMembersNtf 둘 다.
        $expectedAck = 2 * $Pairs
        $expectedNtf = 2 * $Pairs
        if ($other -or $stats.PartialBytes -gt 0) {
            Write-Host "판정2 : X 스트림 정합 깨짐 — other-ids=$other partial=$($stats.PartialBytes)B" -ForegroundColor Red
            $judgeOk = $false
        } elseif ($ackN -ne $expectedAck -or $ntfN -ne $expectedNtf) {
            Write-Host "판정2 : X 개수 불일치 — JoinZoneAck=$ackN(기대 $expectedAck) ZoneMembersNtf=$ntfN(기대 $expectedNtf)" -ForegroundColor Red
            $judgeOk = $false
        } else {
            Write-Host "판정2 : O 스트림 정합 — JoinZoneAck=$ackN(기대 $expectedAck) ZoneMembersNtf=$ntfN(기대 $expectedNtf), 그 외/파손 0" -ForegroundColor Green
        }
    }

    ""
    if ($judgeOk) {
        Write-Host "판정  : O 배정이 어긋나지 않았다(존 이동 폭탄이 락 계층·명부 정합을 안 깼다)" -ForegroundColor Green
    } else {
        Write-Host "판정  : X 위 실패 항목을 볼 것" -ForegroundColor Red
    }

    $sink.Dispose()
    $st.Close(); $cli.Close()
} finally {
    Stop-Harness $scratchRoot $listener $villageProc
}
