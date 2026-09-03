-- ============================================================================
--  04_migrate_zone_id.sql — trade_log.room_id → zone_id  ( · ADR-015 결정 4)
--
--  왜 ALTER 인가 — 셋 중 골랐다.
--      A. 테이블 재생성  : 쌓인 거래 로그가 날아간다. 비가역이다
--      B. 컬럼은 그대로  : 코드는 zone_id 인데 스키마는 room_id 로 영영 갈린다
--      C. RENAME COLUMN  : 데이터 보존 + 용어 일치           ← 이것
--
--  이 파일과 03_procedures.sql 재실행은 **한 배포 단위**다. 떼어 놓지 마라.
--    sp_trade 가 INSERT 문에서 컬럼명 zone_id 를 **리터럴로** 참조하기 때문이다.
--
--  순서를 틀리면 언제 터지는가 — 실측으로 확인했다 (6단계 Step 0 스파이크):
--        존재하지 않는 컬럼을 참조하는 프로시저를 만들어 봤더니
--          CREATE PROCEDURE  → 성공한다 (exit 0. 목록에도 올라온다)
--          CALL              → ERROR 1054 Unknown column ... in 'field list'
--    ⇒ MySQL 은 CREATE 시점에 컬럼을 해석하지 않는다. **실행 시점에 푼다.**
--      그래서 ALTER 만 하고 SP 재배포를 빠뜨리면 **배포는 조용히 성공하고
--      첫 거래 요청에서 kDbError 가 난다.** 배포한 사람은 이미 자리를 떴을 시간이다.
--      ( 이후 문장 폴백이 없다 — ADR-014 결정 3. 그 기능은 그냥 죽는다)
--
--  실행 계정: lapsix (관리자). 서버 계정 minigame 에는 DDL 권한이 없다.
--
--      mysql -u lapsix -p... < sql\04_migrate_zone_id.sql
--      mysql -u lapsix -p... minigame < sql\03_procedures.sql     ← 이어서 즉시
--
--  되돌리기 — git reset 으로는 안 된다. 이 축만 비대칭이다.
--      ALTER TABLE minigame_log.trade_log RENAME COLUMN zone_id TO room_id;
--      + 03_procedures.sql 을 옛 버전으로 재배포
-- ============================================================================

-- **이 문장은 멱등이 아니다.** 이미 적용된 뒤 다시 돌리면
--   `ERROR 1054 Unknown column 'room_id' in 'trade_log'` 로 **여기서 멈춘다** —
--   그러면 아래 확인 질의까지 도달하지 못한다. **그 에러 자체가 「이미 적용됨」의 신호다.**
--   다시 확인하고 싶으면 아래 「확인용 ②」 질의만 따로 실행하라.
ALTER TABLE minigame_log.trade_log
    RENAME COLUMN room_id TO zone_id;

-- 확인용 ① — 컬럼이 실제로 바뀌었는가
SELECT COLUMN_NAME, COLUMN_TYPE, IS_NULLABLE
  FROM information_schema.COLUMNS
 WHERE TABLE_SCHEMA = 'minigame_log'
   AND TABLE_NAME   = 'trade_log'
   AND COLUMN_NAME IN ('zone_id', 'room_id');
--  ⇒ zone_id 한 줄만 나와야 한다. room_id 가 함께 보이면 ALTER 가 안 먹은 것이다.

-- ============================================================================
--  확인용 ② — **SP 재배포를 빠뜨렸는지 여기서 바로 보인다**
--
--  왜 이 질의가 필요한가 — 위 주석만으로는 「한 배포 단위」가 **강제되지 않는다.**
--    사람이 04 만 돌리고 03 을 잊으면, ALTER 는 성공하고 **아무 에러도 안 난다.**
--    그 상태는 **첫 거래 요청**에서야 ERROR 1054 로 드러난다(위 실측 참조) —
--    배포한 사람은 이미 자리를 떴을 시간이다.
--
--  ⇒ **SP 본문이 아직 옛 컬럼을 참조하는지 지금 읽어서 보여 준다.**
--    **LIKE 를 쓰지 않는다.** `room_id` 의 밑줄이 「임의의 한 글자」로 먹히는데,
--      그것을 막으려고 쓴 `ESCAPE '\'` 는 **백슬래시가 이스케이프 문자라 문법 에러가 난다**
--      — 실측 `ERROR 1064`. ⇒ INSTR 은 순수 부분문자열 검색이라 그 함정이 아예 없다.
--    ROUTINE_DEFINITION 은 **정의자 권한이 있어야 보인다** — **`lapsix` 로 돌려라.**
--      실측: 서버 계정 `minigame` 으로는 두 SP 모두 `ROUTINE_DEFINITION IS NULL` 이다
--        (EXECUTE 만 갖기 때문 — ADR-014). ⇒ 그 계정으로 돌리면 **늘 통과로 보여 무용지물이다.**
--        그래서 NULL 을 「OK」가 아니라 **UNKNOWN** 으로 따로 가른다.
-- ============================================================================
SELECT ROUTINE_NAME,
       CASE WHEN ROUTINE_DEFINITION IS NULL
                 THEN 'UNKNOWN - lapsix 로 다시 돌려라 (권한 부족)'
            WHEN INSTR(ROUTINE_DEFINITION, 'room_id') > 0
                 THEN 'STALE - 03_procedures.sql 을 지금 재배포하라'
            ELSE 'OK' END AS deploy_status
  FROM information_schema.ROUTINES
 WHERE ROUTINE_SCHEMA = 'minigame'
 ORDER BY ROUTINE_NAME;
--  ⇒ 두 SP 모두 OK 여야 한다. 하나라도 STALE 이면 **아직 배포가 끝나지 않은 것이다.**
--    STALE 인 채로 서버를 띄우면 거래가 kDbError 로 죽는다.
