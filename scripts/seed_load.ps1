# scripts\seed_load.ps1 — 부하 측정용 플레이어 · 인벤토리 시드
#
#   왜 필요한가
#     trade.ps1 의 Reset-Inventory 가 `DELETE FROM inventory` 로 테이블을 통째로 비운다.
#     회귀를 한 번 돌릴 때마다 부하용 데이터가 날아가므로, 측정 직전에 다시 깐다.
#
#   왜 「전원 같은 행수」인가
#     행이 있는 플레이어와 없는 플레이어가 섞이면 조회 비용이 갈리고,
#     그게 워커 busy 편중으로 보인다 — 배정이 치우친 게 아니라 데이터가 치우친 건데도.
#     측정하려는 변수 하나만 남기려면 나머지는 같아야 한다.
#
#   player_id 대역을 1000 이상으로 띄운 이유
#     alice(1) · bob(2) 는 trade.ps1 의 검증 대상이다. 섞이면 총량 검증이 흔들린다.
#
#   사용:
#     .\seed_load.ps1                 # 256명 삽입
#     .\seed_load.ps1 -Count 512
#     .\seed_load.ps1 -Clean          # 부하용 데이터만 지운다 (1,2 는 안 건드린다)
#
#   ──  N2(kMaxRows 경계) 전용 벌크 픽스처 ──────────────────────
#   kMaxRows = (kMaxBodySize-3)/8 = 511 이다. 고정 픽스처(1·2·99)에는 이 근방 행 수를
#     가진 플레이어가 없어서 신설한다. player_id 는 900~902(900번대) — 1·2·99·1000+ 를
#     전부 피한 자리다.
#
#     .\seed_load.ps1 -BulkPlayer 900 -Rows 510    # 경계 -1 (안 잘림)
#     .\seed_load.ps1 -BulkPlayer 901 -Rows 511    # 경계   (안 잘림 — 511 <= kMaxRows)
#     .\seed_load.ps1 -BulkPlayer 902 -Rows 512    # 경계 +1 (511 로 잘려야 한다)
#     .\seed_load.ps1 -Clean -Base 900             # -Base 를 반드시 준다 — 기본값(1000)으론 900대가 안 지워진다
#                                                   #   (id >= 900 이라 1000+ 부하 데이터도 함께 지워진다. 부하 측정 중이면 다시 시드할 것)

param(
    [int]$Count = 256,
    [int]$Base  = 1000,
    [switch]$Clean,
    [string]$User     = 'minigame',
    [string]$Password = 'minigame',
    [string]$Database = 'minigame',
    [int]$BulkPlayer  = 0,      # N2 — 이 player_id 하나에 -Rows 개의 인벤토리 행을 심는다 (0 = 미사용)
    [int]$Rows        = 0       # BulkPlayer 와 함께 쓴다
)

$ErrorActionPreference = 'Stop'

# 비밀번호를 -p 로 넘기면 mysql 이 "insecure" 경고를 stderr 로 뱉고,
#   PowerShell 5.1 이 그걸 오류로 승격시켜 정상인데도 스크립트가 죽는다 (11차 함정).
#   환경변수로 넘기면 경고 자체가 안 난다.
$env:MYSQL_PWD = $Password

function Invoke-Sql([string]$Sql) {
    $out = $Sql | & mysql -u $User -D $Database -N -B
    if ($LASTEXITCODE -ne 0) { throw "mysql 실패 (exit $LASTEXITCODE)" }
    return $out
}

if ($Clean) {
    # inventory 를 먼저 지울 필요가 없다 — FK 가 ON DELETE CASCADE 다.
    #   「애플리케이션이 두 번 지우게 하면 하나를 빠뜨리는 날이 온다」(01_schema.sql)
    Invoke-Sql "DELETE FROM player WHERE id >= $Base;" | Out-Null
    $left = Invoke-Sql "SELECT COUNT(*) FROM player WHERE id >= $Base;"
    Write-Host "clean : 부하용 플레이어 삭제 완료 (남은 수 $left)"
    Write-Host "        alice(1) / bob(2) 는 건드리지 않았다"
    return
}

if ($BulkPlayer -gt 0) {
    # -------------------------------------------------------------------
    #   N2 (kMaxRows 경계) 전용 픽스처.
    #
    #  player 를 인벤토리보다 먼저 넣는다 — FK 다(01_schema.sql 의
    #    fk_inventory_player, ON DELETE CASCADE). player 가 없으면 인벤토리
    #    INSERT 는 FK 위반인데, INSERT IGNORE 라 그게 경고로 낮춰져
    #    **조용히 0행이 들어가고 스크립트는 성공한 것처럼 보인다** — 그래서
    #    삽입 뒤 실제 COUNT 를 반드시 확인한다(아래).
    #
    #  숫자 테이블 트릭은 위 -Count 경로와 같은 것을 재사용한다(3자리라 -Rows 999 까지 문제없다).
    #    item_id 를 10000+n 으로 띄워 기존 아이템(100·200)과 안 겹치게 한다.
    # -------------------------------------------------------------------
    if ($Rows -le 0) {
        throw "-BulkPlayer 는 -Rows 와 함께 써야 한다 (예: -BulkPlayer 900 -Rows 511)"
    }

    # 재실행 시 잔존 행을 먼저 지운다.
    #   INSERT IGNORE 만 쓰면 같은 player 를 더 작은 -Rows 로 재실행했을 때
    #   이전 행이 그대로 남아 COUNT 검증이 불일치로 throw 하는데, 에러 메시지는
    #   「FK 위반으로 스킵됐는지 확인하라」라 엉뚱한 원인을 가리킨다.
    #   player 행은 지우지 않는다 — INSERT IGNORE 라 그대로 둬도 무해하고,
    #   지우면 CASCADE 로 방금 넣을 inventory 까지 같이 날아가는 순서 문제가 생긴다.
    $bulkSql = @"
DELETE FROM inventory WHERE player_id = $BulkPlayer;
INSERT IGNORE INTO player (id, name) VALUES ($BulkPlayer, CONCAT('bulk', $BulkPlayer));
INSERT IGNORE INTO inventory (player_id, item_id, item_count)
SELECT $BulkPlayer, 10000 + n, 1 FROM (
  SELECT a.N + b.N*10 + c.N*100 AS n FROM
   (SELECT 0 N UNION SELECT 1 UNION SELECT 2 UNION SELECT 3 UNION SELECT 4
    UNION SELECT 5 UNION SELECT 6 UNION SELECT 7 UNION SELECT 8 UNION SELECT 9) a,
   (SELECT 0 N UNION SELECT 1 UNION SELECT 2 UNION SELECT 3 UNION SELECT 4
    UNION SELECT 5 UNION SELECT 6 UNION SELECT 7 UNION SELECT 8 UNION SELECT 9) b,
   (SELECT 0 N UNION SELECT 1 UNION SELECT 2 UNION SELECT 3 UNION SELECT 4
    UNION SELECT 5 UNION SELECT 6 UNION SELECT 7 UNION SELECT 8 UNION SELECT 9) c
) t WHERE n < $Rows;
"@
    Invoke-Sql $bulkSql | Out-Null

    # INSERT IGNORE 는 실패를 숨긴다 — 성공했다고 믿지 말고 실제 행 수를 센다.
    $actual = [int](Invoke-Sql "SELECT COUNT(*) FROM inventory WHERE player_id = $BulkPlayer;")
    Write-Host "bulk  : player $BulkPlayer 에 인벤토리 $actual / $Rows 행 삽입"
    if ($actual -ne $Rows) {
        throw "픽스처 삽입 실패 — 기대 $Rows 행, 실제 $actual 행. player($BulkPlayer) 가 이미 있었는지," +
              " 또는 FK 위반으로 INSERT IGNORE 가 조용히 스킵했는지 확인할 것"
    }
    return
}

$last = $Base + $Count - 1

# 숫자 테이블을 UNION 으로 만든다.
#   프로시저나 임시 테이블은 minigame 계정 권한 밖이다 — DDL 을 일부러 안 줬다.
$sql = @"
INSERT IGNORE INTO player (id, name)
SELECT $Base + n, CONCAT('load', n) FROM (
  SELECT a.N + b.N*10 + c.N*100 AS n FROM
   (SELECT 0 N UNION SELECT 1 UNION SELECT 2 UNION SELECT 3 UNION SELECT 4
    UNION SELECT 5 UNION SELECT 6 UNION SELECT 7 UNION SELECT 8 UNION SELECT 9) a,
   (SELECT 0 N UNION SELECT 1 UNION SELECT 2 UNION SELECT 3 UNION SELECT 4
    UNION SELECT 5 UNION SELECT 6 UNION SELECT 7 UNION SELECT 8 UNION SELECT 9) b,
   (SELECT 0 N UNION SELECT 1 UNION SELECT 2 UNION SELECT 3 UNION SELECT 4
    UNION SELECT 5 UNION SELECT 6 UNION SELECT 7 UNION SELECT 8 UNION SELECT 9) c
) t WHERE n < $Count;
INSERT IGNORE INTO inventory (player_id, item_id, item_count)
SELECT id, 100, 1000 FROM player WHERE id >= $Base;
INSERT IGNORE INTO inventory (player_id, item_id, item_count)
SELECT id, 200, 1000 FROM player WHERE id >= $Base;
"@

# item_count 를 1000 으로 넉넉히 준 이유
#   부하 중에 수량이 바닥나면 거래가 kNotEnough 로 떨어지는데,
#   실패한 거래는 DB 일을 덜 한다 — 그걸 「쓰기가 빨라졌다」로 오독하게 된다.
#   (11차 교훈 : 「빠른 것」과 「일을 안 한 것」은 다르다)

Invoke-Sql $sql | Out-Null

$p = Invoke-Sql "SELECT COUNT(*) FROM player WHERE id >= $Base;"
$r = Invoke-Sql "SELECT COUNT(*) FROM inventory WHERE player_id >= $Base;"
$s = Invoke-Sql "SELECT IFNULL(SUM(item_count),0) FROM inventory WHERE player_id >= $Base;"

Write-Host "seed  : player $Base..$last"
Write-Host "        플레이어 $p 명 / 인벤토리 $r 행 / 총량 $s"
Write-Host ""
Write-Host "참고  : 거래 부하는 총량을 보존한다. 측정 뒤에도 이 총량이 같아야 한다."
Write-Host "        다르면 부하가 아니라 정합성이 깨진 것이다 - 그건 측정보다 큰 사고다."
