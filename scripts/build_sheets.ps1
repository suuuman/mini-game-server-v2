# =========================================================================
#  도면 시트 인쇄 — docs\src\*.html  →  docs\*.pdf
#
#  이 스크립트가 있는 이유:
#      스레드/존」) 때 docs\*.pdf 4장이 옛 용어를 담고
#     있는데도 고치지 못했다. 소스 HTML 이 저장소에 없어 「재생성 불가능」
#     이었기 때문이다(D4 에서 「방치」로 승인). 그래서 소스를 docs\src\ 에
#     넣고, 인쇄 절차를 여기 고정한다.
#
#  원본 PDF 의 판형은 842×1684pt = 297mm × 594mm 다.
#     그 값은 docs\src\_sheet.css 의 @page 에 있다. 여기서 지정하지 않는다 —
#     두 군데에 적으면 반드시 갈린다.
#
#  원본은 HeadlessChrome/151 · Skia/PDF m151 로 뽑혔다(PDF 메타데이터에
#     남아 있다). 그래서 같은 엔진(Edge)으로 인쇄한다. 다른 엔진은 판형이
#     같아도 줄바꿈 위치가 달라져 쪽수가 바뀐다.
#
#  폰트는 Google Fonts 에서 받는다 — 인터넷이 없으면 폴백으로 조판이
#     무너진다. 인쇄 후 반드시 -Verify 로 쪽수를 확인하라.
# =========================================================================

[CmdletBinding()]
param(
    # 인쇄할 시트 번호. 생략하면 docs\src 의 모든 .html
    [int[]] $Sheet,

    # 인쇄 후 쪽수·판형·옛 용어 잔재·조판을 확인한다 (python + pymupdf 필요)
    [switch] $Verify,

    # docs\*.pdf 를 덮어쓰지 않고 build\sheets\ 에만 만든다
    [switch] $DryRun
)

$ErrorActionPreference = 'Stop'

$root    = Split-Path -Parent $PSScriptRoot
$srcDir  = Join-Path $root 'docs\src'
$outDir  = Join-Path $root 'docs'
$tmpDir  = Join-Path $root 'build\sheets'

if (-not (Test-Path $srcDir)) { throw "소스 폴더가 없다: $srcDir" }
if (-not (Test-Path $tmpDir)) { New-Item -ItemType Directory -Force $tmpDir | Out-Null }

# ── 인쇄 엔진 찾기 ──────────────────────────────────────────────────────
#   원본과 같은 Edge 를 먼저 본다. 없으면 Chrome 으로 떨어진다.
$engines = @(
    "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe",
    "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe",
    "$env:ProgramFiles\Google\Chrome\Application\chrome.exe",
    "${env:ProgramFiles(x86)}\Google\Chrome\Application\chrome.exe"
)
$engine = $engines | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $engine) { throw "Edge 도 Chrome 도 없다. 둘 중 하나가 있어야 인쇄된다." }
Write-Host "[엔진] $engine" -ForegroundColor DarkGray

# ── 대상 목록 ───────────────────────────────────────────────────────────
$files = Get-ChildItem -Path $srcDir -Filter '*.html' | Sort-Object Name
if ($Sheet) {
    $files = $files | Where-Object {
        $n = 0
        if ([int]::TryParse($_.BaseName.Split('-')[0], [ref]$n)) { $Sheet -contains $n } else { $false }
    }
}
if (-not $files) { throw "인쇄할 .html 이 없다 (-Sheet 값을 확인하라)" }

$failed = @()

foreach ($f in $files) {
    $pdf = Join-Path $tmpDir ($f.BaseName + '.pdf')

    # file:// URL 은 퍼센트 인코딩이 필요하다 — 파일명이 한글이다.
    $url = ([uri]$f.FullName).AbsoluteUri

    Write-Host "[인쇄] $($f.Name)" -ForegroundColor Cyan

    # --virtual-time-budget: 웹폰트가 도착할 때까지 기다린다.
    #   이 값이 작으면 폰트가 폴백으로 굳은 채 인쇄된다 — 조용히 깨진다.
    #   --log-level=3 을 빼면 Edge 가 stderr 로 경고를 쏟고, PowerShell 5.1 은
    #     네이티브 stderr 를 ErrorRecord 로 감싸 $? 를 거짓으로 만든다.
    #     그래서 여기서 stderr 를 리다이렉트하지 않는다(`2>&1` 금지).
    $engineArgs = @(
        '--headless=new'
        '--disable-gpu'
        '--log-level=3'
        '--no-pdf-header-footer'
        '--virtual-time-budget=20000'
        "--print-to-pdf=$pdf"
        $url
    )

    if (Test-Path $pdf) { Remove-Item $pdf -Force }

    #   Edge 는 완료 메시지("N bytes written to file")를 stderr 로 낸다.
    #   PowerShell 5.1 은 네이티브 stderr 를 ErrorRecord 로 감싸므로, 위의
    #   ErrorActionPreference='Stop' 이 그 정상 메시지를 실패로 잡아 버린다.
    #   성공 판정은 아래에서 파일 존재로 하므로, 이 호출만 예외를 흘려보낸다.
    $prevEAP = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try   { & $engine @engineArgs 2>&1 | Out-Null }
    catch { }
    finally { $ErrorActionPreference = $prevEAP }

    if (-not (Test-Path $pdf)) {
        Write-Host "  ✗ 인쇄 실패" -ForegroundColor Red
        $failed += $f.Name
        continue
    }

    $kb = [math]::Round((Get-Item $pdf).Length / 1KB)
    Write-Host "  ✓ $pdf  ($kb KB)" -ForegroundColor Green

    if (-not $DryRun) {
        Copy-Item $pdf (Join-Path $outDir ($f.BaseName + '.pdf')) -Force
        Write-Host "  → docs\$($f.BaseName).pdf 갱신" -ForegroundColor DarkGray
    }
}

if ($failed.Count -gt 0) { throw "인쇄 실패: $($failed -join ', ')" }

# ── 검증 ────────────────────────────────────────────────────────────────
if ($Verify) {
    Write-Host ""
    Write-Host "[검증] 판형 · 쪽수 · 옛 용어 잔재 · 조판" -ForegroundColor Cyan

    $py = @'
import glob, os, re, sys, io, html
try:
    import pymupdf
except ImportError:
    print("  ! pymupdf 가 없다: python -m pip install pymupdf"); sys.exit(0)

# 그물을 좁게 짜면 「검색은 했는데 사본을 놓친다」. 대소문자 무시하고
#   room/lane 을 통째로 본다 — 한 번 실제로 소문자 room 하나가 빠져나갔다.
STALE = ["레인", "룸", "room", "lane", "RoomManager", "t_lane", "placement_room"]

# 조판 검사가 여기 있는 이유: 판형·쪽수·용어를 전부 통과한 PDF 에서 조판 결함
#   두 개가 그대로 인쇄된 적이 있다. 한글 어절이 줄 경계에서 갈라진 것(117건)과
#   라벨의 한글이 굴림체로 떨어지며 낱글자로 뜯어진 것(자간 324건)이다.
#   둘 다 눈으로 봐야 보이고, 위 세 검사로는 잡히지 않는다.
HAN = re.compile(r'^[가-힣]$')
# 자간 상한. 굴림체 글리프는 em box 보다 좁아 실제 letter-spacing 에 0.09em 쯤이
#   더해져 측정된다. 그래서 CSS 0.13em 이 0.22em 으로 나오는 칸이 있다.
#   현재 기준선은 네 장 합쳐 46건(11·6·14·15)이고 전부 SVG 안이거나 짧은 영문
#   라벨이다. 예산은 파일당이라 가장 많은 장(15)에 여유를 두어 잡았다.
GAP_MAX, GAP_BUDGET = 0.15, 25

def words(src, stem):
    # 어절 사전. PDF 의 줄 끝 토큰이 여기 없고 다음 줄 머리를 붙이면 있으면,
    #   그 어절은 줄 경계에서 갈라진 것이다.
    f = os.path.join(src, stem + ".html")
    if not os.path.exists(f): return None
    s = io.open(f, encoding="utf-8").read()
    s = re.sub(r'<svg[\s\S]*?</svg>|<pre[\s\S]*?</pre>|<!--[\s\S]*?-->', ' ', s)
    # 태그를 빈 문자열로 지우면 <b>1.1</b><span>완료 가 「1.1완료」 한 어절이 되어
    #   실재하지 않는 어절이 사전에 들어간다. 공백으로 바꿔야 한다.
    s = html.unescape(re.sub(r'<[^>]+>', ' ', s))
    return set(w for w in s.split() if re.search(r'[가-힣]', w))

def typeset(doc, vocab):
    split, wide = 0, 0
    for pg in doc:
        lines = [l for b in pg.get_text("rawdict")["blocks"] for l in b.get("lines", [])]
        txts = []
        for l in lines:
            for sp in l["spans"]:
                ch = sp["chars"]
                gaps = [(ch[i+1]["bbox"][0] - ch[i]["bbox"][2]) / sp["size"]
                        for i in range(len(ch) - 1)
                        if HAN.match(ch[i]["c"]) and HAN.match(ch[i+1]["c"])]
                if gaps and sum(gaps) / len(gaps) > GAP_MAX: wide += 1
            txts.append("".join(c["c"] for sp in l["spans"] for c in sp["chars"]).strip())
        if vocab is None: continue
        for i in range(len(txts) - 1):
            a, b = txts[i].split(), txts[i+1].split()
            if not a or not b: continue
            # 양쪽 끝이 모두 한글일 때만 본다. 이 조건이 없으면 「1.1」+「완료」나
            #   「검사」+「APP/FRAME_ROUTER.CPP」 같은 칸 경계를 분리로 읽는다.
            if not (HAN.match(a[-1][-1]) and HAN.match(b[0][0])): continue
            if a[-1] not in vocab and (a[-1] + b[0]) in vocab: split += 1
    return split, wide

bad = 0
src = sys.argv[2] if len(sys.argv) > 2 else ""
for p in sorted(glob.glob(sys.argv[1] + os.sep + "*.pdf")):
    d = pymupdf.open(p)
    r = d[0].rect
    t = "".join(pg.get_text() for pg in d)
    hits = {w: len(re.findall(w, t, re.IGNORECASE)) for w in STALE}
    hits = {k: v for k, v in hits.items() if v}
    stem = os.path.basename(p)[:-4]
    split, wide = typeset(d, words(src, stem))
    ok = (abs(r.width - 841.92) < 1 and abs(r.height - 1684.08) < 1
          and not hits and split == 0 and wide <= GAP_BUDGET)
    if not ok: bad += 1
    print("  %s%-22s %d쪽  %.0fx%.0fpt  %s  어절분리 %d  자간초과 %d" % (
        "OK " if ok else "!! ", os.path.basename(p), len(d), r.width, r.height,
        ("잔재 " + str(hits)) if hits else "잔재 0", split, wide))
sys.exit(1 if bad else 0)
'@
    $pyFile = Join-Path $tmpDir '_verify.py'
    Set-Content -Path $pyFile -Value $py -Encoding utf8
    & python $pyFile $tmpDir $srcDir
    if ($LASTEXITCODE -ne 0) { throw "검증 실패 — 위 !! 줄을 보라" }
}

Write-Host ""
Write-Host "완료." -ForegroundColor Green
