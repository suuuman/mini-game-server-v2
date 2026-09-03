# scripts\churn.ps1 — 접속/종료를 N회 반복하고 서버 메모리·핸들을 본다
#
#   Phase 1 통과 기준의 나머지 절반:
#     "클라이언트 강제 종료해도 안 죽음" + "접속/종료 1,000회에 메모리 안 늘어남"
#
#   반드시 Debug 빌드로 잴 것. ASan 빌드는 free 한 메모리를 격리(quarantine)에
#     붙들어 두기 때문에, 누수가 없어도 메모리가 늘어난다. 누수 측정에 못 쓴다.
#
#   핸들 수도 같이 본다. 소켓을 안 닫으면 메모리보다 핸들이 먼저 드러난다.
#
#   사용:
#     .\churn.ps1 -Count 1000 -Framed
#     .\churn.ps1 -Count 1000 -NoExchange     # 연결만 하고 바로 끊는다

param(
    [int]$Port        = 9000,
    [int]$Count       = 1000,
    [int]$Size        = 8,
    [int]$MsgId       = 1,
    [int]$Report      = 100,          # 몇 회마다 중간 보고
    [string]$Process  = 'village',
    [switch]$Framed,
    [switch]$NoExchange               # 주고받지 않고 연결/종료만 반복
)

$ErrorActionPreference = 'Stop'

function New-Frame([byte[]]$Body, [int]$Id) {
    $h = [byte[]]::new(4)
    $h[0] = [byte](($Body.Length -shr 8) -band 0xFF)
    $h[1] = [byte]( $Body.Length         -band 0xFF)
    $h[2] = [byte](($Id          -shr 8) -band 0xFF)
    $h[3] = [byte]( $Id                  -band 0xFF)
    return $h + $Body
}

function Get-ServerStat([string]$Name) {
    $p = Get-Process -Name $Name -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $p) { return $null }
    $p.Refresh()
    [pscustomobject]@{
        PrivateMB = [math]::Round($p.PrivateMemorySize64 / 1MB, 2)
        PrivateB  = $p.PrivateMemorySize64
        WsMB      = [math]::Round($p.WorkingSet64 / 1MB, 2)
        Handles   = $p.HandleCount
        Threads   = $p.Threads.Count
    }
}

# ASan 빌드로 재면 누수가 없어도 세션 크기만큼 선형으로 는다.
#   free 한 메모리를 격리(quarantine)에 붙들어 두기 때문이다.
#   측정 전에 프로세스가 ASan 런타임을 물고 있는지 본다.
function Test-Asan([string]$Name) {
    $p = Get-Process -Name $Name -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $p) { return $false }
    try {
        return [bool]($p.Modules | Where-Object { $_.ModuleName -like 'clang_rt.asan*' })
    } catch {
        return $false      # 모듈 목록을 못 읽으면 판단 보류
    }
}

function Show-Stat([string]$Label, $S) {
    if ($null -eq $S) { "$Label : (서버 프로세스를 못 찾음)"; return }
    "{0} : private={1}MB  ws={2}MB  handles={3}  threads={4}" -f `
        $Label, $S.PrivateMB, $S.WsMB, $S.Handles, $S.Threads
}

$body = [System.Text.Encoding]::ASCII.GetBytes('A' * $Size)
$unit = if ($Framed) { New-Frame $body $MsgId } else { $body }
$buf  = [byte[]]::new(65536)

if (Test-Asan $Process) {
    Write-Host ""
    Write-Host "  ✕ 이 서버는 ASan 빌드입니다. 누수 측정에 쓸 수 없습니다." -ForegroundColor Red
    Write-Host "    ASan 은 free 한 메모리를 격리에 붙들어 두기 때문에," -ForegroundColor Red
    Write-Host "    누수가 없어도 세션 크기만큼 선형으로 늘어납니다." -ForegroundColor Red
    Write-Host ""
    Write-Host "    build\x64\Debug\village.exe 로 다시 띄운 뒤 재세요." -ForegroundColor Yellow
    Write-Host ""
    return
}

$before = Get-ServerStat $Process
Show-Stat 'before' $before
"churn : $Count 회  exchange=$(-not $NoExchange.IsPresent)  unit=$($unit.Length)B"
""

$sw     = [System.Diagnostics.Stopwatch]::StartNew()
$failed = 0

for ($i = 1; $i -le $Count; $i++) {
    try {
        $c = [System.Net.Sockets.TcpClient]::new('127.0.0.1', $Port)
        $s = $c.GetStream()

        if (-not $NoExchange) {
            $s.ReadTimeout = 2000
            $s.Write($unit, 0, $unit.Length)
            $got = 0
            while ($got -lt $unit.Length) {
                $n = $s.Read($buf, 0, $buf.Length)
                if ($n -le 0) { break }
                $got += $n
            }
        }
        $c.Close()
    }
    catch {
        $failed++
    }

    if ($Report -gt 0 -and ($i % $Report) -eq 0) {
        $now = Get-ServerStat $Process
        if ($null -ne $now) {
            "{0,6} : private={1}MB  handles={2}" -f $i, $now.PrivateMB, $now.Handles
        }
    }
}
$sw.Stop()

# 서버가 마지막 세션들을 정리할 시간을 준다
Start-Sleep -Milliseconds 800
$after = Get-ServerStat $Process

""
Show-Stat 'after ' $after
"time  : $([math]::Round($sw.Elapsed.TotalSeconds,1))s   실패 $failed 회"

if ($null -ne $before -and $null -ne $after) {
    $dMem = $after.PrivateB - $before.PrivateB
    $dHnd = $after.Handles  - $before.Handles
    $per  = [math]::Round($dMem / $Count, 1)

    ""
    "delta : private={0}  ({1} B/session)   handles={2}   threads={3}" -f `
        ("{0:+#,0;-#,0;0}" -f $dMem), $per, ("{0:+#;-#;0}" -f $dHnd),
        ("{0:+#;-#;0}" -f ($after.Threads - $before.Threads))
    ""
    # Session 은 약 4KB(IoContext 의 버퍼) 다. 세션을 안 지우면 여기가 4000 근처로 뜬다.
    if ($per -gt 1000) {
        "판정  : ✕ 세션이 새고 있다. B/session 이 Session 크기(~4KB)에 가깝다."
    } elseif ($dHnd -gt ($Count / 10)) {
        "판정  : ✕ 핸들이 샌다. 소켓을 안 닫는 경로가 있다."
    } else {
        "판정  : ○ 누수 없음. (할당자 단편화·로그 버퍼로 약간의 증가는 정상)"
    }
}

""
"참고  : 연속으로 여러 번 돌리면 클라이언트 쪽 TIME_WAIT 가 쌓인다."
"        netstat -an | Select-String TIME_WAIT | Measure-Object | % Count"
