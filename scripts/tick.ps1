# ============================================================================
#  scripts\tick.ps1 - L11 틱 측정 하네스
#
#  두 종류를 다룬다.
#    · 고장 스위치 (zone_manager.cpp 의 constexpr)  → 패치 + 빌드가 필요하다
#        -WorkMs      : 매 틱 이만큼 걸리게 한다      (지속적 밀림)
#        -SpikeEvery / -SpikeMs : N 틱마다 한 번만 무겁게 (일시적 밀림)
#    · 정책 (config\server.ini 의 [tick])          → 빌드 없이 바뀐다
#        -Hz / -HiRes
#
#  -CatchUpMax 는 없어졌다. 틱이 dt 대신 「지금 몇 시인가」를 받게 되면서
#     밀린 칸을 몰아 돌 이유가 사라졌기 때문이다.
#     밀린 정도는 이제 [TICK ] behind 로 본다 — 손실이 아니라 지연 지표다.
#
#  실행이 끝나도 고장 스위치를 되돌리지 않는다 (케이스마다 빌드가 두 번 되는 걸 피하려고).
#     측정이 다 끝나면 반드시  .\tick.ps1 -Restore  를 부를 것.
#     켜진 채로 회귀를 돌리면 회귀가 거짓말을 한다 (6차 함정).
#
#  사용례
#     .\tick.ps1 -Seconds 15 -Repeat 3                            # 기준선
#     .\tick.ps1 -WorkMs 50 -Seconds 12 -Repeat 3                 # [1] 지속적 밀림
#     .\tick.ps1 -SpikeEvery 100 -SpikeMs 200 -Seconds 12         # [2] 일시적 밀림
#     .\tick.ps1 -Restore                                         # 전부 원복 + 빌드
# ============================================================================
param(
    [int]$WorkMs      = 0,
    [int]$SpikeEvery  = 0,
    [int]$SpikeMs     = 0,
    [int]$Hz          = -1,      # -1 = server.ini 그대로
    [int]$HiRes       = -1,      # -1 = server.ini 그대로 · 0/1 = 강제
    [int]$Seconds     = 10,
    [int]$Repeat      = 1,
    [switch]$Restore,
    [switch]$NoBuild
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$cpp  = Join-Path $root 'src\world\zone_manager.cpp'
$ini  = Join-Path $root 'config\server.ini'
$exe  = Join-Path $root 'build\x64\Release\village.exe'
$log  = Join-Path $root 'logs\server.log'

if ($Restore) { $WorkMs = 0; $SpikeEvery = 0; $SpikeMs = 0; $HiRes = 1 }

# ---------------------------------------------------------------------------
#  1) 고장 스위치 패치 (빌드 필요)
#  WARN 읽을 때도 -Encoding UTF8. 빼면 PS 5.1 이 CP949 로 읽어 한글 주석을 깨뜨린다 (12차)
# ---------------------------------------------------------------------------
# WARN 이 스크립트가 zone_manager.cpp 를 0 바이트로 날린 적이 있다 (13차).
#   Set-Content 는 「먼저 비우고 쓴다」. 그래서 쓸 내용이 비었거나 도중에 죽으면
#   원본이 사라진다. 아래 세 겹으로 막는다 —
#     1) 읽은 내용이 비면 즉시 throw (파일을 안 건드린다)
#     2) 치환 결과가 비거나 앵커 3개를 못 찾으면 throw
#     3) 임시 파일에 먼저 쓰고 Move-Item 으로 갈아끼운다 (원자적 교체)
$body = Get-Content $cpp -Encoding UTF8 -Raw
if ([string]::IsNullOrWhiteSpace($body)) { throw "zone_manager.cpp 를 못 읽었다 (비어 있음). 아무것도 안 썼다." }
$before = $body

$body = [regex]::Replace($body, 'constexpr DWORD kBadTickWorkMs = \d+;', "constexpr DWORD kBadTickWorkMs = $WorkMs;")
$body = [regex]::Replace($body, 'constexpr uint64_t kBadTickSpikeEvery = \d+;', "constexpr uint64_t kBadTickSpikeEvery = $SpikeEvery;")
$body = [regex]::Replace($body, 'constexpr DWORD    kBadTickSpikeMs = \d+;', "constexpr DWORD    kBadTickSpikeMs = $SpikeMs;")

$hit = ([regex]::Matches($body, 'kBadTickWorkMs = \d+;|kBadTickSpikeEvery = \d+;|kBadTickSpikeMs = \d+;')).Count
if ($hit -ne 3) { throw "고장 스위치 앵커를 $hit/3 개만 찾았다. 아무것도 안 썼다." }
if ($body.Length -lt 1000) { throw "치환 결과가 너무 짧다 ($($body.Length)B). 아무것도 안 썼다." }

if ($body -ne $before) {
    $tmp = "$cpp.tmp"
    Set-Content $tmp -Value $body -Encoding UTF8 -NoNewline
    if ((Get-Item $tmp).Length -lt 1000) { Remove-Item $tmp; throw '임시 파일이 비었다. 원본은 그대로다.' }
    Move-Item $tmp $cpp -Force
}

# ---------------------------------------------------------------------------
#  2) 정책 / 설정 (빌드 불필요)
# ---------------------------------------------------------------------------
$cfg = Get-Content $ini -Encoding UTF8 -Raw
if ([string]::IsNullOrWhiteSpace($cfg)) { throw 'server.ini 를 못 읽었다. 아무것도 안 썼다.' }
if ($Hz    -ge 0) { $cfg = [regex]::Replace($cfg, '(?m)^hz\s*=\s*\d+',          "hz                   = $Hz") }
if ($HiRes -ge 0) { $cfg = [regex]::Replace($cfg, '(?m)^hires_timer\s*=\s*\d+', "hires_timer          = $HiRes") }
if ($cfg.Length -lt 500) { throw "server.ini 치환 결과가 너무 짧다 ($($cfg.Length)B). 아무것도 안 썼다." }
Set-Content "$ini.tmp" -Value $cfg -Encoding UTF8 -NoNewline
Move-Item "$ini.tmp" $ini -Force

Write-Host ("switches: work={0}ms spike={1}/{2}ms" -f $WorkMs, $SpikeEvery, $SpikeMs) -ForegroundColor Cyan

# ---------------------------------------------------------------------------
#  3) 빌드 — Release 만. 측정은 Release 로만 의미가 있다.
# ---------------------------------------------------------------------------
if (-not $NoBuild) {
    Get-Process village -EA SilentlyContinue | Stop-Process -Force
    $msb = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
    $out = & $msb (Join-Path $root 'mini-game-server.sln') /p:Configuration=Release /p:Platform=x64 /v:m /nologo
    if ($LASTEXITCODE -ne 0) { $out | Select-Object -Last 20; throw 'build failed' }
    $w = ($out | Select-String 'warning').Count
    Write-Host ("build OK  warnings={0}" -f $w) -ForegroundColor Green
}

if ($Restore) {
    Write-Host 'restored: 고장 스위치 전부 끔 · hires_timer=1' -ForegroundColor Green
    return
}

# ---------------------------------------------------------------------------
#  4) 실행 · [TICK] 줄 추출
#  서버는 프로젝트 루트에서 띄운다 — config 를 상대경로로 읽는다.
#  --seconds 로 스스로 멈춰야 stop 이 돌고 통계가 나온다. 강제 종료 금지 (6차).
#  wall 시간을 같이 찍는 이유 — 나선이면 --seconds 보다 한참 오래 걸린다.
#    그 자체가 「서버를 멈추게 할 수 있다」의 증거다.
# ---------------------------------------------------------------------------
for ($i = 1; $i -le $Repeat; $i++) {
    Remove-Item $log -EA SilentlyContinue
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process $exe -WorkingDirectory $root -ArgumentList '--seconds', $Seconds -PassThru
    $p.WaitForExit()
    $sw.Stop()

    $lines = Get-Content $log -Encoding UTF8 | Select-String '\[TICK'
    $txt = ($lines | ForEach-Object { ($_.Line -split '\] ', 2)[-1].Trim() }) -join '  ||  '
    Write-Host ("run {0}  (wall {1:N1}s / asked {2}s)  {3}" -f $i, $sw.Elapsed.TotalSeconds, $Seconds, $txt)
}

if ($WorkMs -ne 0 -or $SpikeEvery -ne 0) {
    Write-Host ''
    Write-Host 'WARN 고장 스위치가 켜진 채로 남아 있다. 측정이 끝나면 .\tick.ps1 -Restore' -ForegroundColor Yellow
}
