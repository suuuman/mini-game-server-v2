# scripts\drain.ps1 — 마을 드레인(SetMode·DrainComplete·Reserve 거부) 검증
#
#   무엇을 보는가
#     세션 서버 역할은 harness_common.ps1 의 가짜 세션 서버가 대신한다(8종과 같은 자리).
#     그 가짜 세션 서버가 SetMode 로 드레인을 켜고 끄면서, 그동안 Reserve·Enter·
#     DrainComplete 가 설계가 규정한 그대로 도는지를 프로토콜 레벨에서 직접 잰다.
#
#   D1~D8·D10 은 한 서버 인스턴스 위의 단일 순차 시나리오다(D9 는 종료 지표) —
#   표 순서와 실행 순서가 다르다. 준비 단계(D1 이전)에 ① D4 용 세션을 예약→Enter 로
#   입장시키고 ② D3 용 예약을 미리 발급해 둔다. D3 의 "drain 전 발급된
#   예약"은 drain 상태에서는 만들 수 없어 이 준비 단계의 산출물이다.
#   D2·D3 가 쓰는 예약의 expire 는 시나리오 전체 길이보다 길게 잡는다 —
#   짧으면 "저장됐다가 만료"가 D7b 에서 "애초에 미저장"으로 위장한다.
#
#   D3 와 D7 은 같은 예약(같은 player_id)을 쓴다 — D3 가 드레인 중 거절당한
#   그 연결을 D7 이 undrain 뒤에 그대로 재사용해 재시도한다. 그래서 D3 의
#   예약 expire 는 D7 시점까지 살아 있어야 한다.
#
#   사용:
#     .\scripts\drain.ps1
#     .\scripts\drain.ps1 -Config Debug

param(
    [int]$Port           = 9000,
    [string]$Config      = 'Release',   # 예약 경유 접속을 위해 스스로 스폰할 village.exe 구성
    [int]$Seconds        = 120,         # Start-Village 의 죽은 인자(harness_common.ps1 참조) — 자리만 맞춘다
    [int]$ReserveExpireMs = 20000       # D2·D3 예약의 유효 시간 — 이 시나리오 전체보다 길게
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'harness_common.ps1')

$PlayerD4 = [uint64]41001
$PlayerD3 = [uint64]31001
$PlayerD2 = [uint64]21001

function Test-Name([string]$n) { Write-Host ''; Write-Host "== $n ==" }
function Check([string]$what, [bool]$ok) {
    $mark = if ($ok) { 'O' } else { 'X' }
    Write-Host "   [$mark] $what"
    return $ok
}
$script:pass = 0; $script:fail = 0
function Note([bool]$ok) { if ($ok) { $script:pass++ } else { $script:fail++ } }

# gate.ps1 과 같은 헬퍼다(사본 — find_copies.ps1 이 동기화를 지킨다) — 서버가
#   쓰는 중인 로그를 FileShare.ReadWrite 로 열어야 공유 충돌 없이 읽는다.
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

$scratchRoot = Join-Path $env:TEMP ("drain_harness_" + [guid]::NewGuid().ToString('N'))
$listener = $null
$villageProc = $null
$vhome = $null
try {
    $vhome = New-HarnessHome $scratchRoot ([string]$Port)
    $listener = Start-FakeSession 9100
    $villageProc = Start-Village $Config $vhome $Seconds
    $link = Accept-FakeSessionLink $listener
    $serverLog = Join-Path $vhome 'logs\server.log'

    # ── 준비 단계 — D4 입장 + D3 예약 선발급 ────────────────────────────
    Test-Name '준비 — D4 입장 + D3 예약 선발급'
    $connD4 = Connect-Reserved $link $Port $PlayerD4
    Grant-Reservation $link $PlayerD3 $ReserveExpireMs
    Note (Check "D4 입장 완료(session=$($connD4.SessionId)) · D3 예약 발급 완료" $true)

    # ── D1 — SetMode(drain) → SetModeAck{current} ───────────────────────
    Test-Name 'D1 — SetMode(drain) → SetModeAck{current}'
    $ack1 = Set-VillageMode $link $true
    Note (Check "current=$($ack1.Current)(기대 1 — 준비 단계의 입장 1명)" ($ack1.Current -eq 1))

    # ── D2 — drain 중 Reserve → ReserveAck result=3(kResultDraining) ────
    Test-Name 'D2 — drain 중 Reserve → ReserveAck result=3'
    $link.Seq = $link.Seq + [uint32]1
    Send-S2sFrame $link.Stream $Harness_MsgReserve $link.Seq (New-ReserveBody $PlayerD2 $ReserveExpireMs)
    $skippedD2 = New-Object System.Collections.Generic.List[int]
    $ack2 = Wait-S2sFrame $link.Stream $Harness_MsgReserveAck 4000 $skippedD2
    $ack2Ok = ($null -ne $ack2)
    $body2 = if ($ack2Ok) { ConvertFrom-ReserveAckBody $ack2.Body } else { $null }
    Note (Check "ReserveAck result=$(if ($body2) { $body2.Result } else { '(무응답)' })(기대 3)" `
        ($ack2Ok -and ($body2.Result -eq 3)))

    # ── D3 — 준비 예약으로 drain 중 Enter → kBusy 거절 · 연결 유지 ──────
    Test-Name 'D3 — 준비 예약으로 drain 중 Enter → kEnterAck kBusy · 연결 유지'
    $skippedD3 = New-Object System.Collections.Generic.List[int]
    $d3 = Send-VilEnter $Port $PlayerD3
    Note (Check "kEnterAck result=$($d3.Result)(기대 $Harness_ResultBusy — kBusy)" ($d3.Result -eq $Harness_ResultBusy))
    Note (Check "연결 유지(kPingReq/kPongAck 왕복)" (Test-VilAlive $d3.Stream))
    # kBusy 거절이 EntryTable::enter() 이전 분기라 PlayerEnter 알림 자체가
    #   안 나가야 한다 — draining() 검사가 consume_reservation 보다 앞이라는
    #   순서 그 자체를 이 프레임 부재로 실증한다.
    $peD3 = Wait-S2sFrame $link.Stream $Harness_MsgPlayerEnter 800 $skippedD3
    Note (Check "kBusy 거절 후 PlayerEnter 미발신(무응답=$($null -eq $peD3), 기대 True)" ($null -eq $peD3))

    # ── D4 — drain 전 입장한 기존 세션 — 에코 정상 ──────────────────────
    Test-Name 'D4 — drain 전 입장한 기존 세션 — 에코 정상(기존 접속 영향 없음)'
    Note (Check "kPingReq/kPongAck 왕복 정상" (Test-VilAlive $connD4.Stream))

    # ── D5 — 기존 세션 퇴장(소켓 close) → 다음 하트비트 틱에 DrainComplete ──
    Test-Name 'D5 — 기존 세션 퇴장(소켓 close) → 다음 하트비트 틱에 DrainComplete{remaining=0} 1회'
    $connD4.Client.Close()
    $skippedD5 = New-Object System.Collections.Generic.List[int]
    $dc1 = Wait-S2sFrame $link.Stream $Harness_MsgDrainComplete 3000 $skippedD5
    $dc1Ok = ($null -ne $dc1)
    $dc1Body = if ($dc1Ok) { ConvertFrom-DrainCompleteBody $dc1.Body } else { $null }
    Note (Check "DrainComplete remaining=$(if ($dc1Body) { $dc1Body.Remaining } else { '(무응답)' })(기대 0)" `
        ($dc1Ok -and ($dc1Body.Remaining -eq 0)))

    # ── D6 — 추가 틱 대기 — DrainComplete 재발신 없음(플래그) ───────────
    Test-Name 'D6 — 추가 틱 대기 — DrainComplete 재발신 없음(플래그)'
    $skippedD6 = New-Object System.Collections.Generic.List[int]
    $dc2 = Wait-S2sFrame $link.Stream $Harness_MsgDrainComplete 1500 $skippedD6
    Note (Check "재발신 없음(1.5s 무응답이 정상)" ($null -eq $dc2))

    # ── D7 — SetMode(running) → D3 player 가 같은 연결·같은 예약으로 재시도 ──
    Test-Name 'D7 — SetMode(running) → D3 player 재시도 Enter 성공(재허용 + 예약 무소비 실증)'
    $ack7 = Set-VillageMode $link $false
    $d7 = Send-VilEnterAgain $d3.Stream $PlayerD3
    Note (Check "kEnterAck result=$($d7.Result)(기대 $Harness_ResultOk) player=$($d7.PlayerId)(기대 $PlayerD3)" `
        (($d7.Result -eq $Harness_ResultOk) -and ($d7.PlayerId -eq $PlayerD3)))

    # ── D7b — D2 player 로 Enter → kInvalidArg 거절 + 로그 대조(미저장 실증) ──
    Test-Name 'D7b — D2 player 로 Enter → kInvalidArg 거절 + 로그 「예약 없음/만료」 대조'
    $logBefore7b = (Read-ServerLog $serverLog).Length
    $d7b = Send-VilEnter $Port $PlayerD2
    $log7bText = Read-ServerLog $serverLog
    $log7bNew = if ($log7bText.Length -gt $logBefore7b) { $log7bText.Substring($logBefore7b) } else { '' }
    $sawNoReservation7b = $log7bNew -match '예약 없음/만료'
    Note (Check "kEnterAck result=$($d7b.Result)(기대 $Harness_ResultInvalidArg — kInvalidArg)" `
        ($d7b.Result -eq $Harness_ResultInvalidArg))
    Note (Check "서버 로그에 '예약 없음/만료' 기록 — drain 중 거부된 Reserve 가 저장되지 않았다" $sawNoReservation7b)
    try { $d7b.Client.Close() } catch {}

    # ── D8 — 재-drain → DrainComplete 다시 발신(해제 시 플래그 리셋 실증) ──
    Test-Name 'D8 — 재-drain → DrainComplete 다시 발신(undrain 시 플래그 리셋 실증)'
    $ack8 = Set-VillageMode $link $true
    $d3.Client.Close()   # D7 로 재입장한 D3 player 도 퇴장시켜 current=0 을 만든다
    $skippedD8 = New-Object System.Collections.Generic.List[int]
    $dc3 = Wait-S2sFrame $link.Stream $Harness_MsgDrainComplete 3000 $skippedD8
    $dc3Ok = ($null -ne $dc3)
    $dc3Body = if ($dc3Ok) { ConvertFrom-DrainCompleteBody $dc3.Body } else { $null }
    Note (Check "재-drain 후 DrainComplete remaining=$(if ($dc3Body) { $dc3Body.Remaining } else { '(무응답)' })(기대 0)" `
        ($dc3Ok -and ($dc3Body.Remaining -eq 0)))

    # ── D10 — 링크 절단→재-accept→Register 재응대(⛔ SetMode 재전송 없이) →
    #    on_tick 주기 안 DrainComplete 재발신 1회 — per-링크 발신 상태 리셋의
    #    직접 실증. draining 은 마을 로컬 상태라 절단에도 유지되고, 재등록 뒤
    #    on_tick 이 current()==0 + 리셋된 플래그로 자동 재발신한다. ─────────
    #    Accept-FakeSessionLink 는 여기서 안 쓴다 — 그 선폐기가 「MsgId 무확인
    #    첫 프레임 1개 폐기」라 재등록 직후 순서(FullSync 먼저)에 기대는데,
    #    만에 하나 DrainComplete 를 삼키면 이 판정이 거짓 실패로 갈린다. 같은
    #    원시 함수(Wait-Accept·Read/Send-S2sFrame)로 Register 만 응대하고, 이후
    #    프레임은 Wait-S2sFrame 의 일반 필터로 받아 걸러낸 msg_id 를 전부
    #    판정 상세에 남긴다(무확인 폐기 0).
    Test-Name 'D10 — 절단→재-accept→재등록(SetMode 재전송 없이) → DrainComplete 재발신 1회'
    $link.Client.Close()
    $client10 = Wait-Accept $listener 8000 'D10 — village S2S 재연결'
    $d10Ok = $false
    $d10Detail = '(재연결이 오지 않았다)'
    if ($null -ne $client10) {
        $stream10 = $client10.GetStream()
        $reg10 = Read-S2sFrame $stream10 3000
        if ($null -ne $reg10 -and $reg10.MsgId -eq $Harness_MsgRegister) {
            Send-S2sFrame $stream10 $Harness_MsgRegisterAck $reg10.Seq (New-RegisterAckBody 1 0)
            $skippedD10 = New-Object System.Collections.Generic.List[int]
            $dc10 = Wait-S2sFrame $stream10 $Harness_MsgDrainComplete 4000 $skippedD10
            $dc10Body = if ($null -ne $dc10) { ConvertFrom-DrainCompleteBody $dc10.Body } else { $null }
            # 「1회」의 나머지 절반 — 첫 수신 뒤 추가 재발신이 없어야 한다(D6 과 같은 창).
            $skippedD10b = New-Object System.Collections.Generic.List[int]
            $dc10Again = Wait-S2sFrame $stream10 $Harness_MsgDrainComplete 1500 $skippedD10b
            $skipText10 = if ($skippedD10.Count -gt 0) { ($skippedD10 | ForEach-Object { '0x{0:X4}' -f $_ }) -join ',' } else { '(없음)' }
            $d10Ok = ($null -ne $dc10Body) -and ($dc10Body.Remaining -eq 0) -and ($null -eq $dc10Again)
            $d10Detail = "DrainComplete remaining=$(if ($dc10Body) { $dc10Body.Remaining } else { '(무응답)' })(기대 0) · 추가 재발신 없음=$($null -eq $dc10Again)(기대 True) · 그사이 걸러낸 프레임=$skipText10 (FullSync 0x8006 이 정상 — DrainComplete 를 안 삼켰다는 관측 기록)"
        } else {
            $d10Detail = '(재연결은 왔으나 Register 를 못 받았다)'
        }
    }
    Note (Check "절단→재-accept→재등록 후 재발신: $d10Detail" $d10Ok)

    Write-Host ''
    if ($fail -eq 0) {
        Write-Host "판정  : O D1~D8·D10 $pass 개 전부 통과(D9 는 종료 지표 절에서 별도 확인)"
    } else {
        Write-Host "판정  : X D1~D8·D10 중 $fail 개 실패 / $($pass + $fail) 개"
    }
} finally {
    # D9 — 정상 종료 뒤 종료 지표 5종을 읽어야 해서 Stop-Harness 를 그대로 안
    #   쓴다(그 함수는 종료와 스크래치 정리를 한 번에 해 로그를 건질 틈이
    #   없다) — 같은 절차(개행→WaitForExit→Kill 폴백, harness_common.ps1
    #   Stop-Harness 사본)를 여기서 반복한 뒤에 읽고 나서 정리한다.
    try { if ($listener) { $listener.Stop() } } catch {}
    try {
        if ($villageProc -and -not $villageProc.HasExited) {
            $villageProc.HarnessStdin.WriteLine('')
            $villageProc.HarnessStdin.Flush()
            $villageProc.HarnessStdin.Close()
            if (-not $villageProc.WaitForExit(5000)) {
                $villageProc.Kill()
            }
        }
    } catch {
        try { if ($villageProc -and -not $villageProc.HasExited) { $villageProc.Kill() } } catch {}
    }
    Start-Sleep -Milliseconds 300

    Test-Name 'D9 — 정상 종료 후 종료 지표 5종'
    $finalLog = if ($vhome -and (Test-Path -LiteralPath (Join-Path $vhome 'logs\server.log'))) {
        Read-ServerLog (Join-Path $vhome 'logs\server.log')
    } else { '' }
    # ASan 구성은 [ALLOC] 을 안 찍는다 — alloc_counter.cpp 가 ASan 빌드에서는
    #   전역 operator new/delete 를 안 갈아끼운다(ASan 자신의 힙 감시와 충돌
    #   하지 않기 위해서다. docs/TESTING.md 에 이미 기록된
    #   사실이고, 이 하네스가 정상 종료로 바뀌어 ASan exit-time 검사를 처음
    #   받으면서 실측으로 걸렸다). 그 한 지표만 존재 여부 기대를 뒤집는다.
    $indicators = [ordered]@{
        'POOL ' = '\[POOL \]'
        'CONN ' = '\[CONN \]'
        'NET  ' = '\[NET  \]'
        'TICK ' = '\[TICK \]'
    }
    if ($Config -eq 'ASan') {
        Note (Check '종료 지표 [ALLOC] 부재 확인(ASan 은 안 찍는다)' (-not ($finalLog -match '\[ALLOC\]')))
    } else {
        Note (Check '종료 지표 [ALLOC] 확인' ($finalLog -match '\[ALLOC\]'))
    }
    foreach ($k in $indicators.Keys) {
        Note (Check "종료 지표 [$k] 확인" ($finalLog -match $indicators[$k]))
    }
    $poolLine = [regex]::Match($finalLog, '\[POOL \][^\r\n]*')
    if ($poolLine.Success) { Write-Host "   $($poolLine.Value)" }

    if ($fail -eq 0) {
        Write-Host ''
        Write-Host "판정(전체) : O D1~D10 $pass 개 전부 통과"
    } else {
        Write-Host ''
        Write-Host "판정(전체) : X $fail 개 실패 / $($pass + $fail) 개"
    }

    try { Remove-Item -Recurse -Force -LiteralPath $scratchRoot -ErrorAction SilentlyContinue } catch {}
}

if ($fail -gt 0) { exit 1 }
exit 0
