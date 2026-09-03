# scripts\build.ps1 — 여러 구성을 한 번에 빌드하고 경고·오류만 요약한다
#
#   Visual Studio 의 Ctrl+Shift+B 는 「현재 활성 구성」 하나만 빌드한다.
#   세 구성을 다 보려면 「빌드 > 일괄 빌드」를 열어야 하는데, 이게 그 대체다.
#
#   사용:
#     .\build.ps1                     # Debug · Release · ASan 전부
#     .\build.ps1 -Config Release     # 하나만
#     .\build.ps1 -Rebuild            # 전체 다시 빌드
#     .\build.ps1 -Config Release -Run --bench    # 빌드하고 바로 실행

param(
    [string[]]$Config  = @('Debug', 'Release', 'ASan'),
    [switch]  $Rebuild,
    [switch]  $Run,                   # 빌드 성공 시 실행 (Config 가 하나일 때만)
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$RunArgs                # 실행 인자 (예: --bench)
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$sln  = Join-Path $root 'mini-game-server.sln'
$msb  = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'

if (-not (Test-Path $msb)) { throw "MSBuild 를 못 찾음: $msb" }
if (-not (Test-Path $sln)) { throw "솔루션을 못 찾음: $sln" }

$target = if ($Rebuild) { 'Rebuild' } else { 'Build' }
$failed = 0

# 서버가 떠 있으면 링크가 LNK1104 로 막힌다 (exe 를 못 덮어쓴다).
#   빌드 실패를 코드 문제로 오해하기 딱 좋은 자리라 먼저 알려 준다.
$running = @(Get-Process -Name village,session -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
    "! 서버 exe 가 $($running.Count) 개 실행 중 (village/session) — 링크가 막힐 수 있다"
    $running | ForEach-Object { "    PID $($_.Id)  $($_.Path)" }
    "  종료: Get-Process -Name village,session | Stop-Process -Force"
    ""
}

foreach ($cfg in $Config) {
    $out = & $msb $sln /p:Configuration=$cfg /p:Platform=x64 /v:minimal /nologo /t:$target
    $code = $LASTEXITCODE

    # 경고·오류는 「개수」만 세고 본문은 문제가 있을 때만 보여준다.
    #   전체를 그대로 쏟으면 정작 중요한 줄이 묻힌다.
    $warn = @($out | Select-String -Pattern 'warning' -SimpleMatch)
    $err  = @($out | Select-String -Pattern 'error'   -SimpleMatch)

    $mark = if ($code -eq 0 -and $err.Count -eq 0 -and $warn.Count -eq 0) { 'OK' }
            elseif ($code -eq 0 -and $err.Count -eq 0)                    { '! ' }
            else                                                          { 'X ' }

    "{0} {1,-8} warnings={2}  errors={3}" -f $mark, $cfg, $warn.Count, $err.Count

    if ($warn.Count -gt 0) { $warn | Select-Object -First 8 | ForEach-Object { "    $($_.Line.Trim())" } }
    if ($err.Count  -gt 0) { $err  | Select-Object -First 8 | ForEach-Object { "    $($_.Line.Trim())" } }
    if ($code -ne 0 -or $err.Count -gt 0) { $failed++ }
}

if ($failed -gt 0) {
    ""
    "빌드 실패 $failed 개 — 실행하지 않는다"
    exit 1
}

if ($Run) {
    if ($Config.Count -ne 1) { throw "-Run 은 구성이 하나일 때만 쓴다 (-Config Release)" }
    $exe = Join-Path $root "build\x64\$($Config[0])\village.exe"
    if (-not (Test-Path $exe)) { throw "실행 파일이 없다: $exe" }
    ""
    "실행: $exe $($RunArgs -join ' ')"
    ""
    # 작업 디렉토리를 루트로 둔다 — logs\server.log 가 거기 생겨야 한다
    Push-Location $root
    try {
        if ($RunArgs) { & $exe @RunArgs } else { & $exe }
    } finally {
        Pop-Location
    }
}
