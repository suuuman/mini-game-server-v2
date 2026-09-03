-- ============================================================================
--  01_schema.sql — 플레이어 · 인벤토리
--
--  이 파일은 root(또는 DDL 권한이 있는 계정)로 실행한다.
--    서버가 쓰는 minigame 계정에는 CREATE 권한을 일부러 주지 않았다.
--    스키마 변경은 사람이 하는 일이고, 그 경계를 권한으로 못박아 두면
--    「코드가 실수로 테이블을 지우는」 일이 원천 봉쇄된다.
--
--  실행:
--    mysql -u root -p minigame < 01_schema.sql
-- ============================================================================

USE minigame;

-- ---------------------------------------------------------------------------
--  player
--
--  ENGINE=InnoDB 인 이유 — 트랜잭션이 필요하다.
--    MyISAM 은 트랜잭션도 외래키도 없다. L14(인벤토리 교환)의 전제가 여기서 정해진다.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS player (
    id          BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    name        VARCHAR(32)     NOT NULL,
    created_at  DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,

    PRIMARY KEY (id),

    -- 이름 중복을 「스키마에서」 막는다.
    --   애플리케이션에서 "SELECT 로 있는지 보고 없으면 INSERT" 하면,
    --   그 사이에 다른 요청이 끼어들어 둘 다 통과한다(TOCTOU).
    --   유니크 제약은 그 틈이 없다 — DB 가 원자적으로 판정한다.
    UNIQUE KEY uk_player_name (name)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------------
--  inventory
--
--  복합 기본키 (player_id, item_id) 인 이유 —
--    한 플레이어가 같은 아이템을 두 줄로 갖는 것 자체를 막는다.
--    "합쳐서 하나여야 한다"를 코드로 지키려 하면 언젠가 새는데,
--    키로 막으면 DB 가 거절한다.
--
--  item_count 에 CHECK 를 거는 이유 — L14 의 3중 방어 중 마지막 층이다.
--      1층 : 애플리케이션이 수량을 검사한다
--      2층 : 조건부 UPDATE (WHERE item_count >= ?) 로 원자적으로 깎는다
--      3층 : DB CHECK 가 음수 자체를 거절한다   ← 여기
--    앞의 두 층이 뚫려도 여기서 멈춘다. MySQL 8.0.16+ 부터 CHECK 가 실제로 강제된다.
--
--  UNSIGNED 를 안 쓴 이유 — 중요하다.
--    UNSIGNED 는 음수를 「막는」 게 아니라 언더플로를 「아주 큰 수」로 바꾼다.
--    수량 1개에서 2개를 빼면 -1 이 아니라 4294967295 가 된다.
--    아이템이 사라지는 것보다 무한히 생기는 쪽이 훨씬 나쁘다.
--    → 부호 있는 INT + CHECK 로 「음수는 거절」이 정확한 방어다.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS inventory (
    player_id   BIGINT UNSIGNED NOT NULL,
    item_id     INT UNSIGNED    NOT NULL,
    item_count  INT             NOT NULL,
    updated_at  DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP
                                         ON UPDATE CURRENT_TIMESTAMP,

    PRIMARY KEY (player_id, item_id),

    CONSTRAINT ck_inventory_count_nonneg CHECK (item_count >= 0),

    -- 외래키 — 없는 플레이어의 인벤토리가 생기는 걸 막는다.
    --   ON DELETE CASCADE 로 플레이어를 지우면 인벤토리도 같이 지운다.
    --   애플리케이션이 두 번 지우게 하면 하나를 빠뜨리는 날이 온다.
    CONSTRAINT fk_inventory_player
        FOREIGN KEY (player_id) REFERENCES player(id)
        ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------------
--  테스트 데이터 — L13 조회를 바로 확인할 수 있게
-- ---------------------------------------------------------------------------
INSERT IGNORE INTO player (id, name) VALUES
    (1, 'alice'),
    (2, 'bob');

INSERT IGNORE INTO inventory (player_id, item_id, item_count) VALUES
    (1, 100, 10),
    (1, 200,  5),
    (2, 100,  3);

-- 확인
SELECT p.id, p.name, i.item_id, i.item_count
  FROM player p
  LEFT JOIN inventory i ON i.player_id = p.id
  ORDER BY p.id, i.item_id;
