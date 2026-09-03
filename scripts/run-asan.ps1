# scripts\run-asan.ps1 — ASan 빌드를 실행한다
#
#   ASan 빌드는 clang_rt.asan_dynamic-x86_64.dll 에 의존한다.
#   그 DLL 은 MSVC 도구 폴더에만 있고 PATH 에 없어서, 탐색기나 일반 터미널에서
#   village.exe 를 그냥 실행하면 "DLL 을 찾을 수 없습니다" 로 죽는다.
#   이 스크립트가 DLL 폴더를 찾아 PATH 앞에 붙이고 실행한다.
#
#   사용:
#     .\run-asan.ps1              # ASan 빌드 실행
#     .\run-asan.ps1 -Build       # 빌드부터 하고 실행

param(
    [switch]$Build,
    [string]$Config = 'ASan',

    # N초 뒤 스스로 종료한다. 0 이면 안 넘긴다(수동 종료).
    #   강제 종료(Stop-Process)하면 stop 이 안 돌아 종료 경로가 검증에서 빠지고,
    #     ASan 리포트도 안 나온다. 자동 검증에서는 반드시 이 옵션으로 끝낼 것.
    [int]$Seconds = 0
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$sln  = Join-Path $root 'mini-game-server.sln'
$exe  = Join-Path $root "build\x64\$Config\village.exe"

# ── MSBuild 찾기 ──────────────────────────────────────────────────────
if ($Build) {
    $msbuild = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\*\*\MSBuild\Current\Bin\MSBuild.exe' |
               Select-Object -First 1
    if (-not $msbuild) { throw 'MSBuild 를 찾지 못했습니다.' }

    "build : $Config|x64"
    & $msbuild.FullName $sln /p:Configuration=$Config /p:Platform=x64 /v:minimal /nologo
    if ($LASTEXITCODE -ne 0) { throw "빌드 실패 (exit $LASTEXITCODE)" }
    ""
}

if (-not (Test-Path $exe)) {
    throw "$exe 가 없습니다. -Build 를 붙여 빌드하세요."
}

# ── ASan 런타임 DLL 폴더 찾기 ─────────────────────────────────────────
$dll = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\clang_rt.asan_dynamic-x86_64.dll' -ErrorAction SilentlyContinue |
       Sort-Object FullName -Descending | Select-Object -First 1

if ($dll) {
    $env:PATH = "$($dll.DirectoryName);$env:PATH"
    "asan  : $($dll.DirectoryName)"
} else {
    "asan  : DLL 을 못 찾음 — 그냥 실행해 봅니다 (이미 PATH 에 있을 수도 있음)"
}

# ── ASan 옵션 ────────────────────────────────────────────────────────
#   abort_on_error=0  : 리포트를 찍고 끝낸다 (1 이면 디버거를 부르며 죽는다)
#   그 외 기본값이면 heap-use-after-free 를 잡는 데 충분하다.
$env:ASAN_OPTIONS = 'abort_on_error=0'

"exe   : $exe"
"------------------------------------------------------------"

# 작업 디렉토리를 프로젝트 루트로 옮긴다.
#   서버가 config\server.ini 와 logs\ 를 「상대 경로」로 찾기 때문이다.
#   scripts\ 에서 그냥 실행하면 설정을 못 읽어 DB 접속이 조용히 실패한다 —
#   실제로 ASan 검증에서 인벤토리 요청이 전부 DB 오류로 나와서 알았다.
#   설정 경로가 실행 위치에 의존하는 것 자체가 한계다 (NOTES 참조).
Push-Location $root
try {
    if ($Seconds -gt 0) { & $exe --seconds $Seconds } else { & $exe }
} finally {
    Pop-Location
}

"------------------------------------------------------------"
"exit  : $LASTEXITCODE"
