# =========================================================================
#  값의 사본을 추적 파일 전수에서 찾는다 — CLAUDE.md P1 의 실행 도구
#
#  이 스크립트가 있는 이유:
#     P1(고친 뒤 사본을 검색하는 것까지가 한 작업)은 문구로 세 번 승격됐는데
#     네 번째가 또 났다. 이번엔 검색을 안 한 것이 아니라 **어떻게 검색했는가**가
#     갈랐다 — 패턴에 형식이 섞여 있었다:
#        grep "trade.ps1 -All"   → 라벨이 다른 자리를 통과시킴
#        --include=scripts/*.ps1 → .py 를 통과시킴
#        grep Room               → RoomManager 의 대소문자 변형을 못 잡음
#     사람이 패턴을 짤 때마다 형식이 섞여 들어간다. 그래서 형식을 짤 여지를
#     없앤다 — 값만 받고, 범위는 항상 git ls-files 전수다.
#
#  ⛔ 여기서 나온 출력을 보고에 붙여야 P1 을 지킨 것이다. 돌렸다는 말은 근거가 아니다.
# =========================================================================

[CmdletBinding()]
param(
    # 찾을 값. 형식(따옴표·확장자·경로 구분자·라벨)을 빼고 값만 준다.
    [Parameter(Mandatory, Position = 0)]
    [string[]] $Value,

    # 이 경로들은 결과에서 뺀다 (예: 옛 이름을 일부러 남긴 마이그레이션 SQL)
    [string[]] $Exclude,

    # 히트 수만 낸다
    [switch] $Summary
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    # 범위를 좁히는 것이 이 실수의 원인이었으므로 여기서 좁힐 방법을 주지 않는다.
    #   확장자·폴더 필터가 없는 것은 의도다.
    # core.quotepath 기본값(true)이면 한글 파일명이 "docs/1-\353\204…" 로 이스케이프돼
    #   나온다. 그 이름은 실제 경로가 아니라서 Select-String 이 못 열고, 끝에 붙은
    #   따옴표 때문에 아래 확장자 필터도 빗나간다.
    #   --others --exclude-standard 를 함께 주는 것은 아직 커밋 안 한 신규 파일을
    #   놓치지 않기 위해서다 — 방금 만든 파일이 사본의 출처인 경우가 실제로 있다.
    $files = @(git -c core.quotepath=false ls-files --cached --others --exclude-standard)

    # ⛔ git 만으로는 부족하다. 이 저장소는 로컬 문서(.claude · CLAUDE.md)를
    #   통째로 .gitignore 하는데, 이 도구가 막으려는 실수의 상당수가 바로 그 문서들에서 났다
    #   (완료조건·배너·단계 상태). git ls-files 만 쓰면 그 축이 통째로 안 보인다.
    $untracked = @('.claude') | Where-Object { Test-Path $_ } | ForEach-Object {
        Get-ChildItem $_ -Recurse -File | ForEach-Object { Resolve-Path -Relative $_.FullName }
    }
    $files += @($untracked) + @('CLAUDE.md' | Where-Object { Test-Path $_ })
    $files = $files | ForEach-Object { $_ -replace '^\.\\', '' } | Sort-Object -Unique
    if (-not $files) { throw "대상 파일이 없다 — 저장소 루트가 맞는지 확인하라" }

    # 텍스트만 본다. PDF 같은 바이너리는 grep 결과가 의미 없다.
    $binary = '\.(pdf|png|jpg|jpeg|gif|ico|zip|dll|exe|pdb|lib|obj|ttf|woff2?)$'
    # 캐시·백업·인덱스 생성물은 사본이 아니라 파생물이다. 세면 히트 수만 부풀고
    #   「어디를 고쳐야 하는가」가 묻힌다.
    $derived = '(^|\\)(__pycache__|_index|_archive|worktrees|backups)(\\|$)|\.lock$'
    $files = $files | Where-Object { $_ -notmatch $binary -and $_ -notmatch $derived }
    if ($Exclude) {
        foreach ($e in $Exclude) { $files = $files | Where-Object { $_ -notlike $e } }
    }

    # ⛔ 삭제됐지만 아직 스테이지에 남은 경로는 ls-files 에는 있고 디스크에는 없다 —
    #   Select-String -Path 가 그 경로에서 죽으면 검색 전체가 「안 한」 상태로 끝난다
    #   (이 게이트가 조용히 무너지는 형태). 존재하는 파일만 남기되, 뺀 것은 반드시 알린다.
    $gone = @($files | Where-Object { -not (Test-Path -LiteralPath $_ -PathType Leaf) })
    if ($gone.Count -gt 0) {
        Write-Host "[경고] 디스크에 없는 경로 $($gone.Count)건 제외(삭제-미스테이지 추정) — $($gone -join ' · ')" -ForegroundColor Yellow
        $files = @($files | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf })
    }

    Write-Host "[대상] 추적 파일 $($files.Count)개 (바이너리 제외$(if ($Exclude) { ' · -Exclude 적용' }))" -ForegroundColor DarkGray

    $total = 0
    foreach ($v in $Value) {
        # 값을 리터럴로 다룬다. 사용자가 정규식을 넣어 범위를 비틀 여지를 없앤다.
        $pattern = [regex]::Escape($v)
        # -Path 로 준다. 파이프로 넘기면 Select-String 이 그 문자열 자체를 검색 대상으로
        #   삼아 **파일 경로만** 훑는다 — 내용은 한 줄도 안 보고 0건을 낸다.
        $hits = Select-String -Path $files -Pattern $pattern -CaseSensitive:$false -Encoding utf8

        Write-Host ""
        Write-Host "[값] $v  →  $($hits.Count)건" -ForegroundColor Cyan
        $total += $hits.Count

        if (-not $Summary) {
            foreach ($h in $hits) {
                $line = $h.Line.Trim()
                if ($line.Length -gt 110) { $line = $line.Substring(0, 110) + '…' }
                Write-Host ("  {0}:{1}: {2}" -f $h.RelativePath($root), $h.LineNumber, $line)
            }
        }

        # 파일 단위 분포. 「한 파일만 고쳤다」가 여기서 드러난다.
        #   @() 로 감싸는 것은 필수다 — 그룹이 하나면 PowerShell 이 배열을 벗겨
        #   단일 GroupInfo 를 주고, .Count 가 그룹 수가 아니라 그 그룹의 항목 수가 된다.
        $byFile = @($hits | Group-Object Path)
        if ($byFile.Count -gt 1) {
            Write-Host "  ── 파일 $($byFile.Count)개에 걸쳐 있다" -ForegroundColor Yellow
        }
    }

    Write-Host ""
    Write-Host "합계 $total 건" -ForegroundColor Green
    if ($total -eq 0) {
        Write-Host "0건이다. 값을 잘못 줬을 가능성을 먼저 의심하라 — 형식을 섞어 넣지 않았는지 본다." -ForegroundColor Yellow
    }
}
finally { Pop-Location }
