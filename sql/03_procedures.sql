-- ============================================================================
--  03_procedures.sql — 저장 프로시저
--
--  왜 SP 인가 — 거래 한 건이 왕복 7회였고, 그중 6회가 「행을 잠근 채」였다.
--    START TRANSACTION → UPDATE ×4 → 로그 INSERT → COMMIT
--    첫 UPDATE 가 행을 잠그고 COMMIT 이 풀 때까지가 락 보유 시간이다.
--    로컬 DB 는 왕복 0.1ms 라 안 보이지만, DB 가 다른 머신이면 왕복당 0.3~0.5ms —
--    핫 아이템 행 하나를 2~3ms 씩 붙든다.
--    ⇒ SP 로 묶으면 왕복 1회. 락 보유가 「네트워크 시간」에서 「DB 안의 시간」이 된다.
--
--  배포는 사람이 한다. 서버 계정에 DDL(CREATE ROUTINE) 권한이 없다 —
--    「서버는 데이터만 만진다. 스키마 변경은 사람이 한다」(00_account.sql 참조).
--    관리자 계정으로:
--      mysql -u <admin> -p < sql\03_procedures.sql
--    00_account.sql 을 먼저 돌려야 한다 — sp_owner 가 없으면 아래 DEFINER
--      지정이 「정의자 없음」으로 실패한다(sql\ 번호 규약이 그 순서다).
--
--   Step 5 — 이 파일을 안 돌려도 서버는 뜬다(소프트 등급, ADR-014
--    결정 4). 그런데 **Step 4 로 문장 폴백이 사라졌으므로** 예전과 뜻이 다르다 —
--    더 이상 「다른 경로로 내려간다」가 아니라 **「거래·조회가 kDbError 로
--    거절된다」**다. 로그인·채팅만 정상이다. 기동 로그의 `[ERROR] sp_trade
--    미배포 ...` / `[ERROR] sp_inventory_select 미배포 ...` 가 그 신호다.
--
--   Step 5 — 두 SP 모두 `SQL SECURITY DEFINER` + `DEFINER='sp_owner'`
--    로 배포한다(ADR-014). `minigame`(서버 접속 계정)은 이제 테이블 권한이
--    없고 `EXECUTE` 만 가지므로, SP 안의 SELECT/UPDATE/INSERT 가 정의자
--    (`sp_owner`, 테이블 CRUD 보유)의 권한으로 실행돼야 한다.
-- ============================================================================

DROP PROCEDURE IF EXISTS minigame.sp_trade;

DELIMITER $$

CREATE DEFINER = 'sp_owner'@'localhost' PROCEDURE minigame.sp_trade(
    IN p_a_player BIGINT UNSIGNED, IN p_a_item INT UNSIGNED, IN p_a_count INT,
    IN p_b_player BIGINT UNSIGNED, IN p_b_item INT UNSIGNED, IN p_b_count INT,
    IN p_zone_id  INT UNSIGNED,    IN p_trade_id INT UNSIGNED
)
    --  Step 5 — DEFINER 로 바꿨다(ADR-014). 예전엔 INVOKER 를 명시했다 —
    --   그때는 서버 계정(minigame)이 테이블 CRUD 를 직접 갖고 있어서 INVOKER 로
    --   둬도 SP 안의 SELECT/UPDATE/INSERT 가 그대로 통했다. 실측: INVOKER 일 때
    --   SP 안의 CURRENT_USER 가 minigame@localhost 였다.
    --
    --   이제는 반대다 — minigame 계정에서 테이블 권한을 회수했으므로(00_account.sql
    --   Step 5), INVOKER 로 두면 SP 안의 테이블 접근이 전부 권한 거부로 죽는다.
    --   DEFINER='sp_owner'(테이블 CRUD 보유)로 두어야 SP 가 sp_owner 권한으로 돈다 —
    --   이것이 DEFINER 분리가 최소 권한을 완성하는 지점이다.
    SQL SECURITY DEFINER
proc: BEGIN
    DECLARE v_dummy INT;
    DECLARE v_lo BIGINT UNSIGNED;
    DECLARE v_hi BIGINT UNSIGNED;

    -- 잠글 플레이어가 없다 = 있을 수 없는 요청이다 (세션에 박힌 player_id 로 오므로).
    -- 조용히 넘기지 않고 kInvalidArg 로 돌려준다.
    DECLARE EXIT HANDLER FOR NOT FOUND
    BEGIN
        ROLLBACK;
        SELECT 4 AS result;          -- db::TradeResult::kInvalidArg
    END;

    -- 에러를 삼키지 않는다 — RESIGNAL 로 그대로 올린다.
    --   앱의 is_retryable(1213 데드락 · 1205 락 타임아웃)이 재시도를 판정해야 하는데,
    --   여기서 결과 코드로 바꿔 버리면 그 판정 근거(errno)가 사라진다.
    --   ⇒ 재시도 루프는 앱에 그대로 두고, SP 는 사실만 전달한다.
    DECLARE EXIT HANDLER FOR SQLEXCEPTION
    BEGIN
        ROLLBACK;
        RESIGNAL;
    END;

    SET v_lo = LEAST(p_a_player, p_b_player);
    SET v_hi = GREATEST(p_a_player, p_b_player);

    START TRANSACTION;

    -- ------------------------------------------------------------------
    --  ① 플레이어 잠금 — 작은 id 먼저. 이 두 줄이 데드락 회피의 전부다.
    --
    --  왜 inventory 가 아니라 player 인가 — 실측으로 골랐다.
    --    격리 수준 4종(RU·RC·RR·SERIALIZABLE) × 방식 3종을 재 본 결과:
    --      player_id 범위 잠금 : 인벤 102행일 때 104~105개, RR/SER 에서만 갭 락
    --      (player,item) 정밀  : 4개
    --      player 2문장        : 네 수준 전부 2개, 모드까지 동일
    --    player 는 플레이어당 정확히 1행이라 「없는 행」 문제가 아예 없고,
    --    인벤이 수천 행이 돼도 잠금은 2개 그대로다.
    --
    --  먼저 상호배제를 걸어 두면 아래 UPDATE 순서는 아무래도 상관없다.
    --    그래서 앱이 하던 (player_id,item_id) 정렬이 여기 없다 —
    --    옮긴 것이 아니라 필요가 없어진 것이다.
    --     에서 그 정렬을 하던 앱 코드(`build_ops`)는 문장 폴백과 함께 삭제됐다.
    --      ⇒ **데드락 회피는 이제 이 두 줄(LEAST/GREATEST)이 유일한 방어다.**
    --        여기를 지우면 앱에 대체재가 없다 — ADR-013 결정 5 참조.
    --
    --  INTO 를 쓰는 이유가 둘이다.
    --    ① 결과셋을 클라이언트로 내보내지 않는다 (CALL 이 결과셋 하나만 내야 한다)
    --    ② SERIALIZABLE 에서 일반 SELECT 는 공유 잠금을 건다 — 실측 102행 SELECT 에 S락 103개.
    --      INTO + FOR UPDATE 면 의도한 배타 잠금만 생긴다.
    -- ------------------------------------------------------------------
    SELECT 1 INTO v_dummy FROM minigame.player WHERE id = v_lo FOR UPDATE;
    SELECT 1 INTO v_dummy FROM minigame.player WHERE id = v_hi FOR UPDATE;

    -- ------------------------------------------------------------------
    --  ② a → b
    --
    --  count = 0 이면 건너뛴다. 0 을 차감하면 값이 안 바뀌어 ROW_COUNT 가 0 이 되고,
    --    「잔량 부족」으로 오판한다 (스파이크에서 실제로 재현했다).
    -- ------------------------------------------------------------------
    IF p_a_count > 0 THEN
        -- 조건부 UPDATE — SELECT 로 보고 판단한 뒤 UPDATE 하면 그 사이가 TOCTOU 다.
        --   WHERE item_count >= ? 는 DB 가 행을 잠근 채로 판정하므로 틈이 없다.
        UPDATE minigame.inventory
           SET item_count = item_count - p_a_count
         WHERE player_id = p_a_player AND item_id = p_a_item
           AND item_count >= p_a_count;

        IF ROW_COUNT() = 0 THEN
            ROLLBACK;
            SELECT 1 AS result;      -- db::TradeResult::kNotEnough
            LEAVE proc;
        END IF;

        -- 받는 쪽은 행이 없을 수 있다 (그 아이템을 처음 받는 경우).
        --   UPDATE 만 쓰면 0행이 되어 아이템이 그냥 사라진다.
        INSERT INTO minigame.inventory (player_id, item_id, item_count)
        VALUES (p_b_player, p_a_item, p_a_count) AS new
        ON DUPLICATE KEY UPDATE item_count = inventory.item_count + new.item_count;
    END IF;

    -- ------------------------------------------------------------------
    --  ③ b → a  (②와 대칭)
    -- ------------------------------------------------------------------
    IF p_b_count > 0 THEN
        UPDATE minigame.inventory
           SET item_count = item_count - p_b_count
         WHERE player_id = p_b_player AND item_id = p_b_item
           AND item_count >= p_b_count;

        IF ROW_COUNT() = 0 THEN
            ROLLBACK;
            SELECT 1 AS result;      -- kNotEnough
            LEAVE proc;
        END IF;

        INSERT INTO minigame.inventory (player_id, item_id, item_count)
        VALUES (p_a_player, p_b_item, p_b_count) AS new
        ON DUPLICATE KEY UPDATE item_count = inventory.item_count + new.item_count;
    END IF;

    -- ------------------------------------------------------------------
    --  ④ 감사 로그 — 같은 트랜잭션 안에서 남긴다 (ADR-005).
    --    아이템이 움직인 것과 그 기록이 함께 확정되거나 함께 없던 일이 된다.
    --    minigame_log 스키마가 없으면 여기서 예외 → RESIGNAL → 거래 전체가 롤백된다.
    --      「기록을 못 남기면 거래도 없던 일로」가 이 서버의 선택이다.
    -- ------------------------------------------------------------------
    INSERT INTO minigame_log.trade_log
        (a_player, a_item, a_count, b_player, b_item, b_count, zone_id, trade_id)
    VALUES (p_a_player, p_a_item, p_a_count,
            p_b_player, p_b_item, p_b_count, p_zone_id, p_trade_id);

    COMMIT;

    -- 마지막 문장이 결과를 돌려준다. OUT 파라미터를 쓰지 않는 이유 —
    --   실측: CALL sp(?, @r) 는 결과셋을 내지 않아서 SELECT @r 왕복이 하나 더 든다.
    --   왕복 1회를 목표로 하는데 그러면 2회가 된다.
    SELECT 0 AS result;              -- db::TradeResult::kOk
END proc$$

DELIMITER ;


-- ============================================================================
--  sp_inventory_select — 인벤토리 조회
--
--  왜 SP 인가 — **성능이 아니다.** 진짜 단일 SELECT 라 왕복도 1회고 트랜잭션도 없다.
--    SP 로 감싸도 **한 번도 안 빨라진다.**
--
--    얻는 것은 **권한 모델** 하나다 (ADR-014):
--    모든 접근이 SP 를 타면 서버 계정에서 테이블 권한을 회수하고 `EXECUTE` 만 남길 수 있다.
--    그러면 SQL 인젝션이나 코드 실수로도 테이블을 직접 건드릴 수 없다.
--
--    이 근거를 지우지 마라. 없으면 다음 사람이 「이건 왜 SP 지?」 하고 되돌린다.
--
--  ORDER BY item_id 를 명시한 이유 — 기존 문장 경로에는 **없었다.**
--    PRIMARY KEY (player_id, item_id) 라 InnoDB 가 PK 프리픽스 스캔으로 읽어
--    **사실상 item_id 순으로 나오지만, 그건 보장이 아니라 관찰이다.**
--    ⇒ 응답이 kMaxRows(=(4096-3)/8=511)로 잘릴 때 **매번 다른 행이 잘리면**
--      클라이언트에겐 아이템이 오락가락하는 것으로 보인다.
--      옵티마이저가 어차피 PK 순서를 쓰므로 **비용은 0이고 보장만 생긴다.**
--
--  결과셋은 정확히 1개다. OUT 파라미터를 쓰지 않는다 —
--    쓰면 결과셋이 하나 더 늘어(SERVER_PS_OUT_PARAMS) C API 파싱이 복잡해진다.
--    sp_trade 가 SELECT 로 결과를 돌려주는 것과 같은 이유다.
-- ============================================================================

DROP PROCEDURE IF EXISTS minigame.sp_inventory_select;

DELIMITER $$

CREATE DEFINER = 'sp_owner'@'localhost' PROCEDURE minigame.sp_inventory_select(
    IN p_player_id BIGINT UNSIGNED      -- inventory.player_id 와 같은 타입이다
)
    --  Step 5 — DEFINER 로 바꿨다. sp_trade 와 같은 이유다(위 주석 참조) —
    --   minigame 계정이 테이블 권한을 잃었으므로 SP 는 sp_owner 권한으로 돈다.
    SQL SECURITY DEFINER
proc: BEGIN
    -- 7단계 리뷰 L3 — minigame. 으로 한정한다. sp_trade 는 처음부터
    --   minigame.inventory 로 한정했는데 여긴 미한정으로 남아 있었다(Step 1 실수).
    --   기능상 안전은 하다(루틴이 속한 스키마로 해석된다) — 같은 파일 안 스타일을 맞춘다.
    SELECT item_id, item_count
      FROM minigame.inventory
     WHERE player_id = p_player_id
     ORDER BY item_id;
END proc$$

DELIMITER ;

-- 배포 확인
SELECT ROUTINE_NAME, SECURITY_TYPE
  FROM information_schema.ROUTINES
 WHERE ROUTINE_SCHEMA = 'minigame'
   AND ROUTINE_NAME IN ('sp_trade', 'sp_inventory_select');
