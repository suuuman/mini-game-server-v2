-- ============================================================================
--  00_account.sql — 서버 전용 계정과 스키마를 만든다
--
--   Step 5 — 계정이 둘이다(ADR-014). 예전엔 단일 계정 전제로 읽혔지만,
--    이제는 DEFINER 분리가 최소 권한을 「완성하는」 표준 형태다:
--
--    | 계정 | 역할 | 권한 |
--    |---|---|---|
--    | `minigame` | 서버가 붙는 계정 | EXECUTE 만 |
--    | `sp_owner` | SP 정의자 전용 — 사람도 서버도 이 계정으로 안 붙는다 | 테이블 CRUD |
--
--    「테이블 권한을 가진 계정이 존재한다」와 「최소 권한」이 충돌하는 것처럼
--    보이면 그건 아래 서술이 낡았기 때문이다 — 원칙은 **서버가 실제로 붙는
--    계정**이 최소 권한이어야 한다는 것이고, sp_owner 는 그 계정이 아니다
--    (03_procedures.sql 이 SQL SECURITY DEFINER 로 두 SP 를 이 계정에 묶는다).
--
--  root 를 쓰지 않는 이유 —
--    설정 파일(config\server.ini)에 비밀번호가 들어간다. 거기에 root 를 적으면
--    그 파일 하나가 새는 순간 DB 전체가 넘어간다.
--    서버가 붙는 계정(minigame)은 EXECUTE 만 가지면 된다(최소 권한).
--
--  DDL 권한(CREATE/DROP)을 안 주는 이유 —
--    서버는 스키마를 바꿀 일이 없다. 스키마 변경은 사람이 하는 일이고,
--    그 경계를 권한으로 못박아 두면 「코드가 실수로 테이블을 지우는」 일이 원천 봉쇄된다.
--
--  실행 방법 (관리자 계정으로 한 번만)
--    mysql -u root -p < 00_account.sql
--    또는 MySQL Workbench 에서 열어 실행
--
--  아래 두 계정 모두 '바꿔주세요' 를 실제 비밀번호로 고치고 실행할 것.
--     minigame 의 값은 config\server.ini 의 [db] password 에도 적는다.
--     sp_owner 의 값은 03_procedures.sql 배포(DEFINER 지정)에만 쓰인다 —
--     서버는 이 계정으로 붙지 않으므로 server.ini 에는 안 들어간다.
--
--  sql\ 번호 규약이 「새 환경 부트스트랩 순서」다(00→01→02→03). sp_owner 가
--    여기(00)에 없으면, 03 이 DEFINER='sp_owner' 로 배포될 때 「정의자 없음」으로
--    깨진다 — 그래서 SP 정의자 계정도 이 파일에 함께 둔다.
-- ============================================================================

-- utf8mb4 인 이유 — utf8 은 MySQL 에서 3바이트까지만 담는다.
-- 이모지(4바이트)가 들어오면 잘리거나 에러가 난다. 채팅이 있는 서버라 필요하다.
CREATE DATABASE IF NOT EXISTS minigame
    CHARACTER SET utf8mb4
    COLLATE utf8mb4_unicode_ci;

-- 감사 로그는 스키마를 나눈다 (sql\02_log_schema.sql).
--   같은 인스턴스라 트랜잭션은 그대로 하나다 — 거래와 그 기록이 함께 확정된다.
--   나눠 두는 값어치는 백업·보존 정책을 따로 걸 수 있다는 것과,
--   나중에 물리 분리할 때 질의를 안 고쳐도 된다는 것이다.
CREATE DATABASE IF NOT EXISTS minigame_log
    CHARACTER SET utf8mb4
    COLLATE utf8mb4_unicode_ci;

-- ----------------------------------------------------------------------------
--  계정 1 — minigame (서버가 붙는 계정) — 최종적으로 EXECUTE 만 남는다
-- ----------------------------------------------------------------------------

-- localhost 로 제한한다. 외부에서 이 계정으로는 못 붙는다.
CREATE USER IF NOT EXISTS 'minigame'@'localhost'
    IDENTIFIED BY '바꿔주세요';

--  Step 5 — 아래 GRANT 둘은 SP 이전 시대의 권한이다. 새 환경을 이
--   스크립트 하나로 부트스트랩할 때도 「한때 테이블을 직접 만졌다가 SP 로
--   전부 옮기며 회수했다」는 실제 순서를 그대로 재생한다 — 그래야 이 파일만
--   읽어도 결정의 맥락이 보인다. 최종 상태는 바로 아래 REVOKE 로 정해진다.
GRANT SELECT, INSERT, UPDATE, DELETE
    ON minigame.* TO 'minigame'@'localhost';

-- 로그 스키마에는 INSERT 와 SELECT 만 준다. UPDATE 도 DELETE 도 안 준다 —
--   **감사 로그는 고쳐지면 안 되는 기록**이다. 서버 코드가 실수로든 아니든
--   과거 기록을 바꿀 수 있으면 그 순간 증거로서의 값어치가 사라진다.
--   ⇒ 보존 기간이 지난 것을 지우는 일(아카이빙)은 사람이나 별도 배치의 몫이고,
--     그 경계를 여기서 권한으로 못박는다. DDL 을 안 주는 것과 같은 생각이다.
GRANT SELECT, INSERT
    ON minigame_log.* TO 'minigame'@'localhost';

--  Step 5 — 위 둘을 회수한다(ADR-014 결정 1). 「모든 질의를 저장
--   프로시저로」가 끝나 서버 계정이 테이블을 직접 만질 일이 없어졌다.
--   SQL 인젝션이나 코드 실수가 나도 EXECUTE 뿐이라 테이블을 못 건드린다.
--   이 REVOKE 뒤에도 minigame_log 는 03_procedures.sql 의 sp_trade 가
--     sp_owner 권한으로 계속 쓴다 — 서버가 직접 쓰던 것을 SP 로 옮겼을 뿐이다.
REVOKE SELECT, INSERT, UPDATE, DELETE
    ON minigame.* FROM 'minigame'@'localhost';
REVOKE SELECT, INSERT
    ON minigame_log.* FROM 'minigame'@'localhost';

-- 저장 프로시저 호출 권한 (sql\03_procedures.sql 참조)
--   EXECUTE 만 준다. CREATE ROUTINE 은 주지 않는다 —
--     서버는 프로시저를 「부르기만」 하고, 만드는 것은 사람이다.
--     DDL 을 안 주는 것과 같은 이유이고, 그 비대칭이 의도된 것이다.
--
--    Step 4 이전에는 이 권한이 없으면 「준비는 통과, 실행에서 1370」으로
--     죽었고 서버가 문장 경로로 자가 강등했다. Step 4 가 그 폴백(자가 강등 포함)을
--     지웠다 — 지금은 이 권한이 없으면 거래·조회가 곧바로 kDbError 로 거절된다.
--     로그인·채팅은 영향받지 않는다 — prepare_trade_sp/prepare_inventory_sp
--     가 소프트 등급이라 connect 자체는 안 죽는다(ADR-014 결정 4).
GRANT EXECUTE
    ON minigame.* TO 'minigame'@'localhost';

-- ----------------------------------------------------------------------------
--  계정 2 — sp_owner (SP 정의자 전용) — 사람도 서버도 이 계정으로 안 붙는다
--
--  03_procedures.sql 이 두 SP 를 이 계정 DEFINER 로 배포한다
--    (SQL SECURITY DEFINER). 그러면 minigame 계정이 EXECUTE 만 가져도
--    SP 안의 SELECT/UPDATE/INSERT 는 sp_owner 권한으로 돈다 —
--    DEFINER 분리가 최소 권한을 완성하는 표준 형태다(ADR-014).
--
--  03 을 이 파일(00)보다 먼저 돌리면 부트스트랩 순서를 어기는 것이고,
--    sp_owner 가 아직 없어 「정의자 없음」으로 깨진다.
--
--   Step 5 — sp_owner 에도 EXECUTE 를 준다. **직접 겪은 함정**이다 —
--    처음엔 「EXECUTE 는 부르는 쪽(minigame)만 있으면 된다」고 생각했는데,
--    실측 결과 SQL SECURITY DEFINER 인 루틴은 **CALL 시점의 EXECUTE 검사를
--    DEFINER(sp_owner) 계정 기준으로 한다.** minigame 에만 EXECUTE 를 주고
--    sp_owner 에는 안 줬더니 minigame 으로 CALL 해도
--    `ERROR 1370: execute command denied to user 'sp_owner'@'localhost'
--    for routine ...` 가 났다 — **에러 메시지의 계정명이 minigame 이 아니라
--    sp_owner 인 것 자체가 단서였다.** sp_owner 에 EXECUTE 를 추가하고서야
--    minigame 의 CALL 이 통과했다(재현: 이 스크립트 없이 sp_owner 의 EXECUTE
--    만 뺀 상태로 `CALL sp_inventory_select(1)` 를 minigame 으로 실행해 보면
--    같은 1370 이 난다).
--    ⇒ **DEFINER 보안 루틴은 EXECUTE 도 두 계정 모두에 필요하다** —
--      minigame(호출자) 은 스키마 접근 자체(USE)와 라우팅을 위해,
--      sp_owner(정의자) 는 CALL 자체의 권한 검사 대상이라서.
-- ----------------------------------------------------------------------------
CREATE USER IF NOT EXISTS 'sp_owner'@'localhost'
    IDENTIFIED BY '바꿔주세요';

GRANT SELECT, INSERT, UPDATE, DELETE
    ON minigame.* TO 'sp_owner'@'localhost';
GRANT SELECT, INSERT
    ON minigame_log.* TO 'sp_owner'@'localhost';
GRANT EXECUTE
    ON minigame.* TO 'sp_owner'@'localhost';

FLUSH PRIVILEGES;

-- 확인
SELECT user, host FROM mysql.user WHERE user IN ('minigame', 'sp_owner');
SHOW GRANTS FOR 'minigame'@'localhost';
SHOW GRANTS FOR 'sp_owner'@'localhost';
