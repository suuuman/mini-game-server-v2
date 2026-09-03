# scripts\trade.ps1 — 거래창 상태 머신 검증
#
#   무엇이 깨지는가를 먼저 정한다 (7차 함정)
#     이 단계에서 깨질 수 있는 것은 「상태 머신」이다. 아이템은 아직 안 움직인다.
#       · 요청 → 수락 → 등록 → 확인 순서가 지켜지는가
#       · 슬롯이 바뀌면 「양쪽」 confirm 이 지워지는가  (막판 바꿔치기 방어)
#       · 세션당 거래가 하나로 제한되는가
#       · 취소·퇴장에서 상대가 통지를 받는가
#     아이템 총량 검증은 DB 배선 단계에서 붙인다. 지금은 DB 를 타지 않는다.
#
#   사용:
#     .\trade.ps1                # 전체 흐름 한 번
#     .\trade.ps1 -SwapTest      # 막판 바꿔치기 — 확인 뒤 슬롯을 바꾼다
#     .\trade.ps1 -BusyTest      # 이미 거래 중인데 또 요청
#     .\trade.ps1 -LeaveTest     # 거래 중에 존을 떠난다
#     .\trade.ps1 -All           # 전부
#
#   ) 이후 — DB 계정을 오버라이드해야 할 수 있다
#      이후 minigame 계정은 EXECUTE 만 갖는다. 이 하네스의 총량 검증·
#     픽스처 리셋은 테이블을 직접 읽고 쓰므로 그 계정으로는 ERROR 1142 다.
#     ⇒ 테이블 권한이 있는 계정(sp_owner 또는 관리자)을 -DbUser/-DbPass 로 준다.
#     서버가 쓰는 계정과 하네스가 쓰는 계정이 갈린 것이고, 그게 권한 분리의
#       자연스러운 결과다 — 서버는 SP 를 통해서만 만지고, 하네스는 「진실을
#       직접 본다」(7차 함정 주석)는 성격상 테이블 권한이 필요하다.
#     비밀번호를 스크립트에 박지 마라(커밋 훅이 차단한다).
#
#     .\trade.ps1 -All -DbUser sp_owner -DbPass '<비번>'

param(
    [int]$Port    = 9000,
    [int]$ZoneId  = 4,
    [int]$Timeout = 3000,
    [switch]$SwapTest,
    [switch]$BusyTest,
    [switch]$LeaveTest,
    [switch]$DupTest,       # 아이템 복사 재현
    [switch]$All,
    [string]$DbUser = '',   # 비우면 config\server.ini 의 [db] 를 그대로 쓴다(기존 동작)
    [string]$DbPass = '',
    [string]$Config  = 'Release',   # 예약 경유 접속을 위해 스스로 스폰할 village.exe 구성
    [int]$Seconds    = 120          # 스폰한 village.exe 의 수명(초)
)

$ErrorActionPreference = 'Stop'

# kLoginReq/직행 접속이 폐지돼서 이 하네스도 예약을 거쳐야 한다.
#   거래 상태 머신만 재는 게 목적이라 실물 session.exe 는 안 띄운다 — 이
#   파일이 가짜 세션 서버 노릇을 해서 스스로 띄운 village.exe 에 직접
#   Reserve 를 찔러 넣는다. harness_common.ps1 참조.
. (Join-Path $PSScriptRoot 'harness_common.ps1')

# ── DB 직접 조회 ─────────────────────────────────────────────────
#   서버 응답을 믿고 재면 안 된다. 서버가 거짓말할 때 못 잡는다.
#     「깨지는 것은 아이템 총량」이고, 그 진실은 DB 에만 있다. (7차 함정)
$MYSQL = (Get-ChildItem 'C:\Program Files\MySQL\*\bin\mysql.exe' -EA SilentlyContinue |
          Select-Object -First 1).FullName

# 접속 정보는 서버가 읽는 것과 같은 파일에서 가져온다.
#   스크립트에 따로 적으면 둘이 어긋나는 날이 온다.
$iniPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'config\server.ini'
# 이 둘의 이름을 $DbUser/$DbPass(파라미터)와 다르게 둔다 — PowerShell 은
#   변수명을 **대소문자 구분 없이** 찾는다. $dbUser 와 $DbUser 는 「같은 변수」다.
#   처음엔 이름만 대소문자로 구분해 뒀다가, 바로 아래 ini 파싱이 그 「같은 변수」를
#   'minigame' 으로 덮어써서 -DbUser 로 넘긴 값이 통째로 사라지는 사고를 냈다 —
#   재현: -DbUser 뒤에 즉시 찍은 디버그 출력이 이미 'minigame' 이었다. ⇒ 접두사가
#   아니라 **어간 자체**를 다르게 짓는다(sqlUser/sqlPass).
$sqlUser = 'minigame'; $sqlPass = 'minigame'; $dbName = 'minigame'
if (Test-Path $iniPath) {
    foreach ($line in Get-Content $iniPath) {
        if ($line -match '^\s*user\s*=\s*(.+?)\s*$')     { $sqlUser = $Matches[1] }
        if ($line -match '^\s*password\s*=\s*(.+?)\s*$') { $sqlPass = $Matches[1] }
        if ($line -match '^\s*database\s*=\s*(.+?)\s*$') { $dbName = $Matches[1] }
    }
}

# ) 이후 — server.ini 를 파싱한 뒤 오버라이드한다(순서가 중요하다).
#   minigame 계정은 이제 EXECUTE 만 갖는다. 이 하네스의 총량 검증·픽스처 리셋은
#   테이블을 직접 만지므로 그 계정으로는 ERROR 1142 다 — 서버가 쓰는 계정과
#   하네스가 쓰는 계정이 갈린 것이고, 그게 권한 분리(DEFINER 분리)의 자연스러운
#   결과다. 값을 안 주면(기본값 '') 지금과 똑같이 server.ini 를 그대로 쓴다.
if ($DbUser -ne '') { $sqlUser = $DbUser }
if ($DbPass -ne '') { $sqlPass = $DbPass }

function Invoke-Sql([string]$Query) {
    if (-not $MYSQL) { throw 'mysql.exe 를 찾지 못했습니다.' }

    # 비밀번호를 명령줄에 적지 않고 MYSQL_PWD 로 넘긴다. 이유가 둘이다 —
    #   1) -p 로 주면 mysql 이 "insecure" 경고를 stderr 로 뱉고,
    #      PowerShell 5.1 은 네이티브 exe 의 stderr 를 ErrorRecord 로 승격시킨다.
    #      $ErrorActionPreference='Stop' 과 만나면 정상 실행인데도 스크립트가 죽는다.
    #   2) 명령줄 인자는 다른 프로세스에서 그대로 보인다. 경고가 맞는 말이다.
    $env:MYSQL_PWD = $sqlPass
    $prev = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $result = & $MYSQL "-u$sqlUser" "-D$dbName" -N -B -e $Query

        # $LASTEXITCODE 를 반드시 본다.
        #    이전엔 minigame 계정이 테이블 SELECT 를 가져서 기본 경로가 항상
        #   성공했다 — 이 검사가 없어도 무해했다. ) 이후 minigame 은
        #   EXECUTE 만 갖는다. -DbUser 없이 기본값(server.ini 의 minigame)으로
        #   돌리면 이제 항상 ERROR 1142 이고, 그 실패를 검사 없이 [int] 로 캐스팅
        #   하면 [int]$null → 0 이 된다. 총량 검증이 0 == 0 으로 「보존됐다」고
        #   거짓 통과하고, 아이템 복사 재현(DupTest)조차 delta=0-0=0 으로
        #   통과해 버린다 — 이 저장소에서 가장 중요한 회귀가 무력화된다.
        #   ⇒ seed_load.ps1 은 이미 이 검사를 갖고 있다 — 짝이 맞아야 한다.
        if ($LASTEXITCODE -ne 0) {
            throw "mysql 실패 (exit $LASTEXITCODE) — 계정 '$sqlUser' 에 테이블 권한이" +
                  " 있는지 확인할 것 (minigame 은 EXECUTE 만 갖는다." +
                  " -DbUser/-DbPass 로 sp_owner 등 테이블 권한 있는 계정을 지정하라)"
        }
        return $result
    } finally {
        $ErrorActionPreference = $prev
        Remove-Item Env:MYSQL_PWD -ErrorAction SilentlyContinue
    }
}

function Get-TotalSum {
    return [int](Invoke-Sql 'SELECT IFNULL(SUM(item_count),0) FROM inventory')
}
function Get-ItemSum([int]$ItemId) {
    return [int](Invoke-Sql "SELECT IFNULL(SUM(item_count),0) FROM inventory WHERE item_id = $ItemId")
}
function Show-Inventory {
    foreach ($r in (Invoke-Sql 'SELECT player_id, item_id, item_count FROM inventory ORDER BY player_id, item_id')) {
        Write-Host "        $($r -replace "`t", '  item ' )"
    }
}
# 테스트 데이터를 되돌린다 (반복 실행 가능하게)
function Reset-Inventory {
    $null = Invoke-Sql @"
INSERT IGNORE INTO player (id, name) VALUES (3, 'carol');
DELETE FROM inventory;
INSERT INTO inventory (player_id, item_id, item_count) VALUES (1,100,10),(1,200,5),(2,100,3);
"@
}

$MSG_JOIN_REQ    = 2
$MSG_INV_REQ     = 4
$MSG_TRADE_REQ   = 6
$MSG_TRADE_ANS   = 7
$MSG_TRADE_ITEM  = 8
$MSG_TRADE_CONF  = 9
$MSG_TRADE_CANCEL= 10

$MSG_JOIN_ACK    = 102
$MSG_TRADE_REQ_N = 106
$MSG_TRADE_OPEN  = 107
$MSG_TRADE_STATE = 108
$MSG_TRADE_ACK   = 109
$MSG_TRADE_CAN_N = 110

$RESULT_NAME = @('OK','DB오류','잔량부족','인자오류','바쁨','로그인안함','상대없음')
function Get-ResultName([int]$c) {
    if ($c -ge 0 -and $c -lt $RESULT_NAME.Count) { return $RESULT_NAME[$c] }
    return "알수없음($c)"
}

function New-Frame([byte[]]$Body, [int]$Id) {
    $len = if ($null -eq $Body) { 0 } else { $Body.Length }
    $h = [byte[]]::new(4)
    $h[0] = [byte](($len -shr 8) -band 0xFF); $h[1] = [byte]($len -band 0xFF)
    $h[2] = [byte](($Id  -shr 8) -band 0xFF); $h[3] = [byte]($Id  -band 0xFF)
    if ($len -eq 0) { return $h }
    return $h + $Body
}
function New-U32BE([int]$v) {
    $b = [byte[]]::new(4)
    $b[0] = [byte](($v -shr 24) -band 0xFF); $b[1] = [byte](($v -shr 16) -band 0xFF)
    $b[2] = [byte](($v -shr  8) -band 0xFF); $b[3] = [byte]( $v          -band 0xFF)
    return $b
}
# [int] 캐스팅 필수 — PowerShell 의 -shl 은 왼쪽이 byte 면 결과도 byte 로 자른다 (8차 함정)
function Get-U16([byte[]]$d,[int]$o) { return ([int]$d[$o] -shl 8) -bor [int]$d[$o+1] }
function Get-U32([byte[]]$d,[int]$o) {
    return ([int]$d[$o] -shl 24) -bor ([int]$d[$o+1] -shl 16) -bor `
           ([int]$d[$o+2] -shl 8) -bor  [int]$d[$o+3]
}

# 들어온 프레임을 전부 꺼낸다 (없으면 빈 배열)
function Receive-Frames($c, [int]$WaitMs = 250) {
    $out = @()
    $deadline = [datetime]::UtcNow.AddMilliseconds($WaitMs)
    $buf = [byte[]]::new(65536)
    while ([datetime]::UtcNow -lt $deadline) {
        if ($c.Stream.DataAvailable) {
            $n = $c.Stream.Read($buf, 0, $buf.Length)
            if ($n -gt 0) { $c.Pending.Write($buf, 0, $n) }
        } else {
            Start-Sleep -Milliseconds 20
        }
    }
    $data = $c.Pending.ToArray()
    $off = 0
    while ($off + 4 -le $data.Length) {
        $len = Get-U16 $data $off
        if ($off + 4 + $len -gt $data.Length) { break }
        $body = if ($len -gt 0) { $data[($off+4)..($off+3+$len)] } else { @() }
        $out += @{ Id = (Get-U16 $data ($off+2)); Body = $body }
        $off += 4 + $len
    }
    # 소비한 만큼 버린다
    $c.Pending.SetLength(0)
    if ($off -lt $data.Length) {
        $rest = $data[$off..($data.Length-1)]
        $c.Pending.Write($rest, 0, $rest.Length)
    }
    return ,$out
}

function Send-Frame($c, [byte[]]$Body, [int]$Id) {
    $f = New-Frame $Body $Id
    $c.Stream.Write($f, 0, $f.Length); $c.Stream.Flush()
}

function Connect-Player([int]$PlayerId, [int]$Zone) {
    # 블록마다 같은 player_id 를 다시 쓰는데, Close-Player 는 소켓만 닫고 서버
    #   쪽 정리(on_session_gone -> entry.leave)가 끝나기를 기다리지 않는다.
    #   예약을 강제하기 전에는 그 정리가 늦어도 무해했다 — 입장 집합을 안 거치는
    #   경로로 붙었기 때문이다. 지금은 정리가 늦으면 Enter 가 「이미 입장 중」으로
    #   거절되고 Connect-Reserved 가 throw 해 하네스가 통째로 죽는다. 개별 항목
    #   실패가 아니라 전체 중단이라 원인을 찾기도 어렵다.
    # 로컬 IOCP 는 보통 이 창보다 훨씬 빠르지만 보장이 아니다. 느린 머신이나
    #   부하 중에는 벌어질 수 있어 재시도로 흡수한다. 재시도가 실제로 걸렸다면
    #   그 자체가 관측값이므로 조용히 넘기지 않고 찍는다.
    $reserved = $null
    for ($attempt = 1; $attempt -le 10; $attempt++) {
        try {
            $reserved = Connect-Reserved $link $Port ([uint64]$PlayerId) $Timeout
            if ($attempt -gt 1) {
                Write-Host "   (재시도 $attempt 회만에 player=$PlayerId 입장 — 앞 세션 정리가 늦었다)" -ForegroundColor DarkYellow
            }
            break
        }
        catch {
            if ($attempt -eq 10) { throw }
            Start-Sleep -Milliseconds 100
        }
    }
    $c = @{ Client = $reserved.Client; Stream = $reserved.Stream; Pending = [System.IO.MemoryStream]::new();
            PlayerId = $PlayerId; SessionId = $reserved.SessionId }

    Send-Frame $c (New-U32BE $Zone) $MSG_JOIN_REQ
    [void](Receive-Frames $c 400)
    return $c
}

function Close-Player($c) { $c.Pending.Dispose(); $c.Client.Close() }

# 상태 통지에서 confirm 두 개를 꺼낸다
# body: [my_item:4][my_count:4][my_confirm:1][peer_item:4][peer_count:4][peer_confirm:1]
function Read-State($frames) {
    $last = $null
    foreach ($f in $frames) { if ($f.Id -eq $MSG_TRADE_STATE) { $last = $f } }
    if ($null -eq $last) { return $null }
    $b = $last.Body
    return @{
        MyItem    = Get-U32 $b 0;  MyCount    = Get-U32 $b 4;  MyConfirm   = [int]$b[8]
        PeerItem  = Get-U32 $b 9;  PeerCount  = Get-U32 $b 13; PeerConfirm = [int]$b[17]
    }
}

function Test-Name([string]$n) { Write-Host ""; Write-Host "== $n ==" }
function Check([string]$what, [bool]$ok) {
    $mark = if ($ok) { 'O' } else { 'X' }
    Write-Host "   [$mark] $what"
    return $ok
}

$pass = 0; $fail = 0; $skip = 0
function Note([bool]$ok) { if ($ok) { $script:pass++ } else { $script:fail++ } }
# 재현 불가로 판정된 시나리오용 — 실패로도 통과로도 세지 않는다. 조용히
#   지우면 「전부 통과」의 개수가 줄어 무회귀 판정이 흐려지므로 SKIP 으로
#   따로 센다(session.ps1 의 같은 신설과 짝이다).
function Note-Skip([string]$what) {
    Write-Host "   [SKIP] $what"
    $script:skip++
}

$runAll = $All -or (-not ($SwapTest -or $BusyTest -or $LeaveTest -or $DupTest))

$scratchRoot = Join-Path $env:TEMP ("trade_harness_" + [guid]::NewGuid().ToString('N'))
$listener = $null
$villageProc = $null
try {
$vhome = New-HarnessHome $scratchRoot ([string]$Port)
$listener = Start-FakeSession 9100
$villageProc = Start-Village $Config $vhome $Seconds
$link = Accept-FakeSessionLink $listener

# ── 1. 정상 흐름 ─────────────────────────────────────────────────────
if ($runAll -or $All) {
    Test-Name "정상 흐름 — 요청 → 수락 → 등록 → 확인"
    Reset-Inventory
    $sumBefore  = Get-TotalSum
    $item200Bef = Get-ItemSum 200
    $item100Bef = Get-ItemSum 100
    Write-Host "   거래 전 : 총량 $sumBefore  (item200=$item200Bef  item100=$item100Bef)"

    $A = Connect-Player 1 $ZoneId
    $B = Connect-Player 2 $ZoneId
    Write-Host "   A session=$($A.SessionId)  B session=$($B.SessionId)"

    Send-Frame $A (New-U32BE $B.SessionId) $MSG_TRADE_REQ
    $bf = Receive-Frames $B
    Note (Check "B 가 kTradeReqNtf 를 받았다" ([bool]($bf | Where-Object { $_.Id -eq $MSG_TRADE_REQ_N })))

    Send-Frame $B ((New-U32BE $A.SessionId) + [byte[]]@(1)) $MSG_TRADE_ANS
    $af = Receive-Frames $A; $bf = Receive-Frames $B
    Note (Check "양쪽이 kTradeOpenNtf 를 받았다" (
        ([bool]($af | Where-Object { $_.Id -eq $MSG_TRADE_OPEN })) -and
        ([bool]($bf | Where-Object { $_.Id -eq $MSG_TRADE_OPEN }))))

    Send-Frame $A ((New-U32BE 200) + (New-U32BE 5)) $MSG_TRADE_ITEM
    $null = Receive-Frames $A; $null = Receive-Frames $B
    Send-Frame $B ((New-U32BE 100) + (New-U32BE 3)) $MSG_TRADE_ITEM
    $st = Read-State (Receive-Frames $A)
    Note (Check "A 가 본 상태 = 내것 200x5 / 상대것 100x3" (
        $null -ne $st -and $st.MyItem -eq 200 -and $st.MyCount -eq 5 -and
        $st.PeerItem -eq 100 -and $st.PeerCount -eq 3))
    $null = Receive-Frames $B

    Send-Frame $A ([byte[]]@(1)) $MSG_TRADE_CONF
    $st = Read-State (Receive-Frames $B)
    Note (Check "B 가 본 상태 = 상대 confirm 만 1" (
        $null -ne $st -and $st.PeerConfirm -eq 1 -and $st.MyConfirm -eq 0))
    $null = Receive-Frames $A

    Send-Frame $B ([byte[]]@(1)) $MSG_TRADE_CONF
    $af = Receive-Frames $A; $bf = Receive-Frames $B
    $aack = $af | Where-Object { $_.Id -eq $MSG_TRADE_ACK }
    $back = $bf | Where-Object { $_.Id -eq $MSG_TRADE_ACK }
    Note (Check "양쪽이 kTradeAck 를 받았다" (($null -ne $aack) -and ($null -ne $back)))
    if ($null -ne $aack) {
        $rc = [int]$aack.Body[0]
        Note (Check "결과 = OK  (받은 값: $(Get-ResultName $rc))" ($rc -eq 0))
    } else { Note $false }

    # ── DB 를 직접 본다. 서버 응답이 아니라 이쪽이 진실이다 ──────
    Start-Sleep -Milliseconds 400
    $sumAfter  = Get-TotalSum
    $item200Af = Get-ItemSum 200
    $item100Af = Get-ItemSum 100
    Write-Host "   거래 후 : 총량 $sumAfter  (item200=$item200Af  item100=$item100Af)"
    Note (Check "★ 총량이 보존됐다 — 거래는 「옮기는 것」이다" ($sumAfter -eq $sumBefore))
    Note (Check "종류별 수량도 보존됐다" (
        $item200Af -eq $item200Bef -and $item100Af -eq $item100Bef))

    # alice 가 200 을 5개 내주고 100 을 3개 받았어야 한다
    $aliceHas200 = [int](Invoke-Sql 'SELECT IFNULL(SUM(item_count),0) FROM inventory WHERE player_id=1 AND item_id=200')
    $bobHas200   = [int](Invoke-Sql 'SELECT IFNULL(SUM(item_count),0) FROM inventory WHERE player_id=2 AND item_id=200')
    Note (Check "alice 의 200 이 5 → $aliceHas200 · bob 이 0 → $bobHas200" (
        $aliceHas200 -eq 0 -and $bobHas200 -eq 5))
    Show-Inventory

    Close-Player $A; Close-Player $B
}

# ── 2. 막판 바꿔치기 ───────────────────────────────────────────────
if ($SwapTest -or $All) {
    Test-Name "막판 바꿔치기 — 상대 확인 뒤 슬롯을 바꾼다"
    $A = Connect-Player 1 $ZoneId
    $B = Connect-Player 2 $ZoneId

    Send-Frame $A (New-U32BE $B.SessionId) $MSG_TRADE_REQ
    $null = Receive-Frames $B
    Send-Frame $B ((New-U32BE $A.SessionId) + [byte[]]@(1)) $MSG_TRADE_ANS
    $null = Receive-Frames $A; $null = Receive-Frames $B

    Send-Frame $A ((New-U32BE 200) + (New-U32BE 5)) $MSG_TRADE_ITEM
    $null = Receive-Frames $A; $null = Receive-Frames $B

    # B 가 먼저 확인한다
    Send-Frame $B ([byte[]]@(1)) $MSG_TRADE_CONF
    $st = Read-State (Receive-Frames $B)
    Note (Check "B 의 confirm 이 켜졌다" ($null -ne $st -and $st.MyConfirm -eq 1))
    $null = Receive-Frames $A

    # A 가 슬롯을 바꿔치기한다
    Send-Frame $A ((New-U32BE 200) + (New-U32BE 1)) $MSG_TRADE_ITEM
    $stb = Read-State (Receive-Frames $B)
    $sta = Read-State (Receive-Frames $A)
    Note (Check "★ B 의 confirm 이 지워졌다 (막판 바꿔치기 방어)" (
        $null -ne $stb -and $stb.MyConfirm -eq 0))
    Note (Check "★ A 의 confirm 도 함께 0 이다 (한쪽만 지우면 실패)" (
        $null -ne $sta -and $sta.MyConfirm -eq 0))
    Note (Check "B 가 바뀐 수량 1 을 본다" ($null -ne $stb -and $stb.PeerCount -eq 1))

    Close-Player $A; Close-Player $B
}

# ── 3. 세션당 거래는 하나 ────────────────────────────────────────────
if ($BusyTest -or $All) {
    Test-Name "세션당 거래는 하나 — 이미 거래 중인데 또 요청"
    $A = Connect-Player 1 $ZoneId
    $B = Connect-Player 2 $ZoneId
    # entry_table::enter() 는 A 가 아직 물고 있는 player_id=1 로의 두 번째
    #   Enter 를 거절한다 — 이 블록의 두 단언은 세션 단위 거래 슬롯만 보고
    #   player_id 값 자체는 안 보므로(아래 Check 참조), 겹치지 않는 새
    #   id 를 쓴다. 겹치는 id로 "같은 player_id 로 두 세션이 동시에 산다"를
    #   보이려던 것이 DupTest 였으나, 그 전제 자체가 Enter 경유로는 재현
    #   불가능해졌다(DupTest 블록의 skip 사유 참조).
    $C = Connect-Player 3 $ZoneId

    Send-Frame $A (New-U32BE $B.SessionId) $MSG_TRADE_REQ
    $null = Receive-Frames $B

    Send-Frame $A (New-U32BE $C.SessionId) $MSG_TRADE_REQ
    $af = Receive-Frames $A
    $ack = $af | Where-Object { $_.Id -eq $MSG_TRADE_ACK }
    $rc  = if ($null -ne $ack) { [int]$ack.Body[0] } else { -1 }
    Note (Check "두 번째 요청이 거절됐다 — 결과: $(Get-ResultName $rc)" ($rc -eq 4))

    Send-Frame $C (New-U32BE $B.SessionId) $MSG_TRADE_REQ
    $cf = Receive-Frames $C
    $ack = $cf | Where-Object { $_.Id -eq $MSG_TRADE_ACK }
    $rc  = if ($null -ne $ack) { [int]$ack.Body[0] } else { -1 }
    Note (Check "이미 거래 중인 상대에게 건 요청도 거절 — 결과: $(Get-ResultName $rc)" ($rc -eq 4))

    Close-Player $A; Close-Player $B; Close-Player $C
}

# ── 4. 존을 떠나면 거래가 정리된다 ───────────────────────────────────
if ($LeaveTest -or $All) {
    Test-Name "정리 — 거래 중에 존을 떠난다 / 접속을 끊는다"
    $A = Connect-Player 1 $ZoneId
    $B = Connect-Player 2 $ZoneId

    Send-Frame $A (New-U32BE $B.SessionId) $MSG_TRADE_REQ
    $null = Receive-Frames $B
    Send-Frame $B ((New-U32BE $A.SessionId) + [byte[]]@(1)) $MSG_TRADE_ANS
    $null = Receive-Frames $A; $null = Receive-Frames $B

    Send-Frame $A (New-U32BE ($ZoneId + 1)) $MSG_JOIN_REQ    # A 가 다른 존으로
    $bf = Receive-Frames $B
    Note (Check "B 가 kTradeCancelNtf 를 받았다 (존 퇴장)" (
        [bool]($bf | Where-Object { $_.Id -eq $MSG_TRADE_CAN_N })))

    # 다시 거래를 열 수 있어야 한다 — trade_id 가 양쪽 다 0 으로 돌아갔다는 뜻.
    #   A 는 아직 연결이 살아 있어 player_id=1 을 그대로 물고 있으므로(존만
    #   옮겼을 뿐 접속을 안 끊었다), entry_table::enter() 가 겹치는 id 의
    #   재입장을 거절한다 — 아래 단언은 "새 거래가 열리는가"만 보고
    #   player_id 값은 안 보므로 겹치지 않는 새 id 를 쓴다.
    $D = Connect-Player 4 $ZoneId
    Send-Frame $D (New-U32BE $B.SessionId) $MSG_TRADE_REQ
    $bf = Receive-Frames $B
    Note (Check "★ B 가 새 거래 요청을 받는다 (양쪽 trade_id 가 함께 풀렸다)" (
        [bool]($bf | Where-Object { $_.Id -eq $MSG_TRADE_REQ_N })))

    Close-Player $D                                          # 거래 중에 끊는다
    Start-Sleep -Milliseconds 200
    $bf = Receive-Frames $B
    Note (Check "B 가 kTradeCancelNtf 를 받았다 (세션 종료)" (
        [bool]($bf | Where-Object { $_.Id -eq $MSG_TRADE_CAN_N })))

    Close-Player $A; Close-Player $B
}

# ── 5. 동기 Kick 이 거래 정리 지점 4/4(on_session_gone)를 타는가 ──────
#    옛 DupTest 의 전제("같은 player_id 로 두 세션이 동시에 산다")는
#    예약 강제 도입으로 영구 재현 불가다(kLoginReq 직행 경로 폐지 —
#    entry_table::enter() 가 중복 Enter 를 거절하고, 그 경로를 우회하던
#    직행 로그인 자체가 와이어에서 사라졌다). 동기 Kick 이 여는 것은
#    그것과 다른 형태의 소유권 충돌("정당한 두 번째 Login")이고, 그
#    Kick 이 거래 중인 세션을 끊을 때 거래 정리 4경로 중 마지막
#    (on_session_gone)이 실제로 도는지가 이 재활성의 원 목적(DB 총량
#    보존)과 맞물린 새 커버리지다.
if ($DupTest -or $All) {
    Test-Name "동기 Kick 이 거래 중인 세션을 끊는다 — 정리 4/4 + DB 총량 보존"
    Reset-Inventory
    $before = Get-TotalSum

    $alice = Connect-Player 1 $ZoneId
    $bob   = Connect-Player 2 $ZoneId

    Send-Frame $alice (New-U32BE $bob.SessionId) $MSG_TRADE_REQ
    $null = Receive-Frames $bob
    Send-Frame $bob ((New-U32BE $alice.SessionId) + [byte[]]@(1)) $MSG_TRADE_ANS
    $null = Receive-Frames $alice; $null = Receive-Frames $bob

    # (a) 실제 아이템을 적재한 채로 끊는다 — 거래를 열기만 하면 DB 무접촉·
    #   clear_trade_of 비의존이라 이 항목이 아무것도 증명 못 한다(뮤턴트
    #   실측으로 확인됐다). 확인만 하고 confirm 은 안 한다 — 그게 이
    #   시나리오의 전제(거래가 「진행 중」에 끊긴다)다.
    Send-Frame $alice ((New-U32BE 200) + (New-U32BE 5)) $MSG_TRADE_ITEM
    $null = Receive-Frames $alice; $null = Receive-Frames $bob

    $kickAck = Send-VillageKick $link ([uint64]1)
    Note (Check "KickAck result=$($kickAck.Result)(기대 0=Kicked)" ($kickAck.Result -eq 0))

    # alice 소켓이 실제로 끊기는지 — gate.ps1 Wait-ConnectionOutcome ·
    # session.ps1 K3 와 같은 Poll(SelectRead)+Available==0 기법이다.
    $aliceClosed = $false
    $ksw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($ksw.ElapsedMilliseconds -lt 5000) {
        if ($alice.Client.Client.Poll(50000, [System.Net.Sockets.SelectMode]::SelectRead)) {
            if ($alice.Client.Client.Available -eq 0) { $aliceClosed = $true; break }
            [void](Receive-Frames $alice 50)
        }
    }
    Note (Check "Kick 뒤 alice 소켓이 끊겼다" $aliceClosed)

    # (b) bob 이 kTradeCancelNtf 를 받는다 — clear_trade_of(정리 4/4)가
    #   실제로 돌았다는 직접 증거다(총량 보존만으론 그 경로를 안 타도
    #   우연히 통과할 수 있다 — 뮤턴트 실측으로 확인됐다).
    $bf = Receive-Frames $bob 1000
    Note (Check "bob 이 kTradeCancelNtf 를 받았다(거래 정리 4/4 — clear_trade_of 확인)" (
        [bool]($bf | Where-Object { $_.Id -eq $MSG_TRADE_CAN_N })))

    # (d) 플레이크 방어 — Send-VillageKick 안의
    #   Wait-S2sFrame(KickAck 대기) 이 그 사이 먼저 도착한 PlayerLeave 를
    #   조용히 버렸을 수 있다. 버린 목록(Skipped)을 먼저 보고, 거기 없을
    #   때만 새로 기다린다 — 그래야 도착 순서(KickAck 먼저 vs PlayerLeave
    #   먼저)에 따라 결과가 갈리는 플레이크가 없다.
    $leaveSeen = $kickAck.Skipped -contains $Harness_MsgPlayerLeave
    if (-not $leaveSeen) {
        $skippedLeave = New-Object System.Collections.Generic.List[int]
        $leaveFrame = Wait-S2sFrame $link.Stream $Harness_MsgPlayerLeave 5000 $skippedLeave
        $leaveSeen = $null -ne $leaveFrame
    }
    Note (Check "가짜 세션 서버가 PlayerLeave 를 받았다(거래 정리 4/4 경로 확인)" $leaveSeen)

    Close-Player $bob

    # alice 는 여기서 안 닫는다 — 하네스가 스스로 닫으면 서버가 실제로
    # 안 끊었어도 재입장이 통과해버려(자기-마스킹) 아래 4단언이 close_by_id
    # 미수행을 못 잡는다. 판정이 전부 끝난 뒤(DupTest 블록 맨 끝)에 닫는다.

    # 재입장 — 재Reserve·재Enter. Connect-Player 의 재시도(최대 10회)가
    # 앞 세션 정리 지연을 흡수한다(§ Connect-Player 주석 참조 — Kick 이
    # 여는 창도 같은 종류다).
    $alice2 = Connect-Player 1 $ZoneId
    Note (Check "재입장 성공(player=1)" ($null -ne $alice2))

    # (c) 재입장 뒤 실거래를 confirm 까지 완주시킨다 — DB 에 실제로 쓰기가
    #   나야 총량 보존 판정이 유의미하다(그냥 재입장만 확인하면 DB 를
    #   전혀 안 건드려 이 판정이 가짜 통과였다 — 뮤턴트 실측으로 확인됐다).
    #   item 200 이 아니라 100 을 쓰는 이유 — Kick 으로 끊긴 앞 거래가
    #   item 200 을 적재만 하고 confirm 전에 끊겨 DB 에 안 쓰였는지까지
    #   같은 김에 교차 확인한다(다른 아이템으로 겹치지 않게 분리).
    $bob2 = Connect-Player 2 $ZoneId
    Send-Frame $alice2 (New-U32BE $bob2.SessionId) $MSG_TRADE_REQ
    $null = Receive-Frames $bob2
    Send-Frame $bob2 ((New-U32BE $alice2.SessionId) + [byte[]]@(1)) $MSG_TRADE_ANS
    $null = Receive-Frames $alice2; $null = Receive-Frames $bob2
    Send-Frame $alice2 ((New-U32BE 100) + (New-U32BE 2)) $MSG_TRADE_ITEM
    $null = Receive-Frames $alice2; $null = Receive-Frames $bob2
    Send-Frame $alice2 ([byte[]]@(1)) $MSG_TRADE_CONF
    $null = Receive-Frames $bob2; $null = Receive-Frames $alice2
    Send-Frame $bob2 ([byte[]]@(1)) $MSG_TRADE_CONF
    $af2 = Receive-Frames $alice2; $null = Receive-Frames $bob2
    $aack2 = $af2 | Where-Object { $_.Id -eq $MSG_TRADE_ACK }
    Note (Check "재입장 뒤 실거래 confirm 완주 — result=$(if ($aack2) { Get-ResultName ([int]$aack2.Body[0]) } else { '(무응답)' })" (
        ($null -ne $aack2) -and ([int]$aack2.Body[0] -eq 0)))

    Start-Sleep -Milliseconds 400
    $after = Get-TotalSum
    Note (Check "DB 총량 보존 — before=$before after=$after" ($before -eq $after))

    Close-Player $alice
    Close-Player $alice2; Close-Player $bob2
}

Write-Host ""
if ($fail -eq 0) {
    # SKIP 은 통과에 안 들어간다($pass 를 안 건드린다) — 「그중」의 모집단은
    #   통과 수가 아니라 총 항목 수다. session.ps1 이 쓰는 형태와 맞춘다.
    Write-Host "판정  : O $pass 개 통과 (총 $($pass + $skip) 개 · 그중 SKIP $skip 개)"
} else {
    Write-Host "판정  : X $fail 개 실패 (총 $($pass + $fail + $skip) 개 · 그중 SKIP $skip 개) — 서버 로그의 [WARN] 을 볼 것"
}
} finally {
    Stop-Harness $scratchRoot $listener $villageProc
}
