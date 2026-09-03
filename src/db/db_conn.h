// db/db_conn.h — MySQL 연결 하나. 한 스레드가 독점한다.
//
// MYSQL* 는 스레드 안전하지 않다. 락으로 감쌀 수는 있지만 그러면 연결 하나를 두고
// 워커가 줄을 서서 워커를 여러 개 둔 의미가 없다. 연결은 스레드마다 빌려 쓴다(db_pool.h).
//
// 값은 전부 prepared statement 로 보낸다. escape 는 위험한 글자를 찾아 지우는 접근이라
// 빠뜨리면 뚫리지만, prepared 는 SQL 과 값의 통로가 달라 값이 SQL 로 해석될 자리가 없다.
//
// 연결이 끊기는 건 예외가 아니라 정상이다. wait_timeout(기본 8시간)이 지나면 MySQL 이
// 닫으므로, 새벽에 트래픽이 없다가 아침 첫 질의가 실패하는 건 흔한 사고다. 끊김으로
// 실패하면 재연결하고 한 번만 다시 시도한다. (MYSQL_OPT_RECONNECT 는 8.0 에서 deprecated)
#pragma once

#include <winsock2.h>       // mysql.h 보다 먼저. 소켓 타입이 여기서 정의된다
#include <mysql.h>

#include "core/log.h"       // 설정 값이 이상할 때 조용히 넘기지 않으려고

#include <algorithm>        // 락 순서 고정에 std::sort 를 쓴다
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace db {

    struct DbConfig {
        std::string host     = "127.0.0.1";
        unsigned    port     = 3306;
        std::string user;
        std::string password;
        std::string database;

        // 감사 로그가 들어갈 스키마. 비어 있으면 database 와 같은 곳에 쓴다.
        //
        // 같은 인스턴스의 다른 스키마이지 다른 서버가 아니다. 트랜잭션은 커넥션 단위라,
        // 같은 인스턴스면 한정자만 붙여도 거래와 같은 트랜잭션 안에 남는다 — 아이템이
        // 움직인 것과 그 기록이 함께 확정되거나 함께 없던 일이 된다. 서버를 물리적으로
        // 나누면 그 원자성이 사라지고 outbox 가 필요해지며, outbox 는 멱등성 키를 딸고 온다.
        //
        // 지금 나눠 두는 값은 오늘의 이득이 아니라, 나중에 물리 분리할 때 질의를 안 고치고
        // 커넥션 라우팅만 바꾸면 된다는 것이다.
        std::string log_database;

        // 이 연결이 감사 로그를 남길 것인가. 복제본은 false 다 — 쓸 일이 없고, 쓰지도
        // 않을 문장을 준비했다가 실패하면 읽기 경로까지 잃는다(main.cpp).
        // 「설정을 안 했다」와 「일부러 안 쓴다」를 가르는 값이라 하나로 뭉치면
        // 복제본 연결마다 설정 오류 경고가 찍힌다.
        bool trade_log_enabled = true;

        // 격리 수준 — 코드가 아니라 배포가 정할 값이라 설정으로 뺐다.
        //
        // 정합성은 여기에 기대지 않는다. 차감이 조건부 UPDATE(잠금 읽기)라 어느
        // 수준에서도 최신 커밋본을 본다. 그래서 이건 정합성 손잡이가 아니라 경합 손잡이고,
        // 어느 쪽이 유리한지는 규모가 정한다. 지금은 잠금이 전부 PK 점 접근이라 갭 락이
        // 거의 없고 트랜잭션도 짧아 차이가 안 난다. 동시 트랜잭션이 늘면 RR 의 오래된 읽기
        // 뷰가 undo 를 붙들어 purge 가 밀리고, 새 (player,item) 조합이 쏟아지면 갭 락이
        // 같은 갭의 삽입을 직렬화한다 — 그 구간에서는 READ COMMITTED 가 이긴다.
        //
        // READ UNCOMMITTED 는 bootstrap 이 거절한다. 거래는 잠금 읽기라 더티 리드의 영향을
        // 안 받지만 인벤토리 조회가 깨진다 — 롤백될 중간 상태를 보여주면 유저가 없는
        // 아이템을 보고 거래를 건다. 옛 값이 아니라 존재한 적 없는 값이라 복제 지연보다
        // 나쁘다. 얻는 것도 없다. InnoDB 는 MVCC 라 RC/RR 에서도 일반 SELECT 가 락을 안 잡는다.
        std::string isolation = "REPEATABLE READ";
    };

    struct InventoryRow {
        uint32_t item_id = 0;
        int32_t  count   = 0;
    };

    // 거래 한 건의 주문서. 「누가 무엇을 얼마나」만 담고 세션도 존도 모른다 — db 계층이
    // 게임 상태를 모르는 것이 이 층의 경계다. count 가 0 이면 그쪽은 아무것도 안 낸다.
    struct TradeOrder {
        uint64_t a_player = 0;
        uint32_t a_item = 0;
        uint32_t a_count = 0;
        uint64_t b_player = 0;
        uint32_t b_item = 0;
        uint32_t b_count = 0;

        // 아래 둘은 일하는 데 안 쓴다. 거래 로그에 남겨 텍스트 로그와 대조하는 표식이다.
        // trade_id 는 전역 카운터에서 나온다(frame_router.cpp) — 거래가 세션 소유가
        // 되며 존별 카운터(world/zone.h)가 사라졌다. 재시작하면 다시 1부터다.
        // 감사 로그의 키는 DB 의 AUTO_INCREMENT 가 맡는다.
        uint32_t zone_id  = 0;
        uint32_t trade_id = 0;
    };

    // bool 로 못 돌려주는 이유 — 실패가 한 종류가 아니다. 「잔량이 모자라 못 했다」는
    // 정상적인 게임 결과이고 「DB 가 죽어서 못 했다」는 장애다. 뭉치면 위층이 구분 못 한다.
    //
    // proto::ResultCode 를 그대로 안 쓰는 것은 db 가 proto 를 모르기 때문이다
    // (world → proto → net → core · db → core). 번역은 둘 다 아는 app/frame_router.cpp 가 한다.
    enum class TradeResult {
        kOk = 0,
        kNotEnough,     // 조건부 UPDATE 가 0행 — 잔량 부족
        kBusy,          // 데드락·락 대기로 재시도를 다 썼다
        kDbError,       // 연결 끊김 · 제약 위반 · 그 밖의 질의 실패
        kInvalidArg,    // 같은 플레이어끼리의 거래 — 아래 판단 참조
    };

    class DbConn {
    public:
        DbConn() = default;
        ~DbConn() { close(); }

        DbConn(const DbConn&) = delete;
        DbConn& operator=(const DbConn&) = delete;

        // false 여도 서버는 떠야 한다 — DB 가 죽었다고 로그인까지 못 받으면 장애가 커진다.
        bool open(const DbConfig& cfg) {
            cfg_ = cfg;
            return connect();
        }

        void close() {
            // 문장 폴백을 지울 때 준비 쪽(prepare_one)과 닫는 쪽을 같이 지웠다.
            // 한쪽만 남기면 이미 nullptr 인 것을 닫는 죽은 코드가 된다.
            close_stmt(stmt_take_);
            close_stmt(stmt_give_);
            close_stmt(stmt_trade_sp_);
            close_stmt(stmt_inventory_sp_);

            // 위에서 닫은 stmt 를 last_call_stmt_ 가 가리키고 있을 수 있으므로
            // mysql_close() 전에 리셋한다. 안 하면 재연결 뒤에도 옛 연결의 stmt 를
            // 가리키는 댕글링 포인터로 남는다. 지금은 do_sanitize() 의 첫 검사가
            // 역참조 전에 걸러 주지만 그건 우연한 안전이라, 검사 순서가 바뀌면 UAF 다.
            last_call_stmt_ = nullptr;

            if (mysql_ != nullptr) {
                mysql_close(mysql_);
                mysql_ = nullptr;
            }
        }

        bool is_open() const { return mysql_ != nullptr; }
        const std::string& last_error() const { return last_error_; }

        // 풀에 반납되기 직전에 호출된다 (DbPool::Lease::reset). 정의는 아래.
        void sanitize();

        // 인벤토리 조회. 연결이 끊겨 있으면 한 번만 재연결하고 다시 시도한다.
        // 계속 재시도하면 DB 가 죽었을 때 워커가 거기 붙들리므로, 실패는 위로 올린다.
        bool select_inventory(uint64_t player_id, std::vector<InventoryRow>& out) {
            if (select_inventory_dispatch(player_id, out)) {
                return true;
            }
            if (!connection_lost_) {
                return false;                  // 진짜 질의 실패다. 재연결해도 소용없다
            }
            // 연결이 끊겼던 것 — 한 번만 되살려 본다
            close();
            if (!connect()) {
                return false;
            }
            return select_inventory_dispatch(player_id, out);
        }

        // 일부러 틀리게 만든 거래. 아래 trade() 의 대조군이라 지우지 않는다.
        // 스위치는 app/frame_router.cpp 의 kBadTradeNoTx 이고 평시 false 다.
        //
        // 틀린 곳은 둘뿐이다. 트랜잭션이 없어서 중간에 실패하면 앞의 것만 커밋되고
        // (차감만 되고 지급이 실패하면 아이템이 사라진다), affected_rows 를 안 봐서
        // 조건부 UPDATE 가 0행이어도 지급을 그대로 보낸다 — 없는 아이템이 생긴다.
        //
        // prepared statement 와 조건부 UPDATE 는 그대로 뒀다. 바꾼 것이 적어야 무엇
        // 때문에 복사가 났는지 보인다. 그래서 이 버전도 엉성해 보이지 않는데, 실제
        // 사고도 대개 이렇게 거의 맞는 코드에서 난다.
        TradeResult trade_unsafe(const TradeOrder& o) {
            if (mysql_ == nullptr) {
                connection_lost_ = true;
                last_error_ = "not connected";
                return TradeResult::kDbError;
            }

            if (o.a_count > 0) {
                if (!exec_take(o.a_player, o.a_item, static_cast<int>(o.a_count))) {
                    return TradeResult::kDbError;
                }
                // 여기서 last_affected_ 를 「보지 않는다」. 그게 전부다.
                if (!exec_give(o.b_player, o.a_item, static_cast<int>(o.a_count))) {
                    return TradeResult::kDbError;
                }
            }
            if (o.b_count > 0) {
                if (!exec_take(o.b_player, o.b_item, static_cast<int>(o.b_count))) {
                    return TradeResult::kDbError;
                }
                if (!exec_give(o.a_player, o.b_item, static_cast<int>(o.b_count))) {
                    return TradeResult::kDbError;
                }
            }
            return TradeResult::kOk;
        }

        // 제대로 된 거래. 위 버전과 다른 곳은 넷이다.
        //   트랜잭션으로 묶는다         넷 다 되거나 넷 다 안 된다
        //   affected_rows 를 본다       0행이면 잔량 부족 — 복사를 막는 곳
        //   락 순서를 고정한다          순환 대기를 깬다
        //   1213/1205 만 재시도한다     재시도해도 되는 실패를 가려낸다
        // 앞의 셋은 지금 sp_trade 안에 있고(sql/03_procedures.sql), 여기 남은 것은 넷째다.
        static constexpr int kTradeRetry = 3;

        TradeResult trade(const TradeOrder& raw) {
            if (mysql_ == nullptr) {
                connection_lost_ = true;
                last_error_ = "not connected";
                return TradeResult::kDbError;
            }

            // 같은 플레이어끼리는 거래할 수 없다. 중복 로그인으로 세션이 둘이어도
            // player_id 가 같으면 같은 행을 두 번 만지고, 락 순서가 「같은 키」를 만나
            // 무의미해지며, 지급이 먼저 오면 늘어난 잔량으로 차감이 통과한다.
            // 세션 층(handle_trade_req)에서도 막지만 여기서도 막는다 — 층마다 자기 근거로.
            if (raw.a_player == raw.b_player) {
                last_error_ = "same player on both sides";
                return TradeResult::kInvalidArg;
            }

            // 아무도 아무것도 안 올린 거래는 DB 를 타지 않는다. 아래의 「상계해서 0」과는
            // 다르다 — 이쪽은 유저가 한 일이 없어 남길 것도 없고, 상계 0 은 서로 아이템을
            // 올렸으니 기록할 값이 있다. 안 가르면 빈 슬롯으로 요청·수락·확인을 반복하는
            // 것만으로 워커 한 자리와 트랜잭션 왕복이 나가고, 보존 정책도 없는 테이블에
            // 0만 든 행이 쌓인다.
            if (raw.a_count == 0 && raw.b_count == 0) {
                return TradeResult::kOk;      // 아무 일도 없었다. 그게 사실이다
            }

            const TradeOrder o = net_order(raw);

            // 상계해서 옮길 게 없어도 그냥 돌아가지 않는다. 한때 여기서 곧바로 kOk 를
            // 냈는데, 감사 로그가 생기고 나서 구멍이 됐다 — 유저는 거래를 한 것이라
            // 「거래했는데 아이템이 그대로예요」 문의가 오면 그 건이 어디에도 없다.
            // 그래서 옮길 게 없어도 트랜잭션을 열고 로그만 남긴다.

            for (int attempt = 0; attempt < kTradeRetry; ++attempt) {
                // 이번 시도의 결과만 본다.
                //   trade() 는 connection_lost_ 를 리셋하지 않던 유일한 진입점이었다 —
                //     try_select_inventory 는 첫 줄에서 리셋하는데 이쪽은 안 해서,
                //     앞선 조회가 세워 둔 값이 그대로 남아 있었다. 아래 판정이 그 값을
                //     보게 되므로 여기서 못 박는다.
                connection_lost_ = false;

                // 폴백을 없앴다(ADR-014 결정 3). SP 가 없으면 거래는 성립하지 않는다.
                //   그래도 「연결을 죽이지는 않는다」 — 로그인·채팅은 계속 돌아야 한다(결정 4와 같은 사상).
                //     이 저장소는 「부차적 기능의 실패가 주 기능을 죽이면 안 된다」를 이미 한 번
                //     사고로 배웠다(구 prepare_trade_log() 의 사고 기록 — 아래 log_schema() 참조).
                if (stmt_trade_sp_ == nullptr) {
                    // kBusy 가 아니다 — kBusy 는 「재시도하면 될 수도」인데 미배포는
                    //   재시도해도 안 된다(CODING_RULES §3). connect() 는 죽이지 않는다
                    //   (prepare_trade_sp() 는 소프트 유지 — ADR-014 결정 4).
                    last_error_ = "sp_trade 미배포 — sql\\03_procedures.sql 을 배포하라";
                    last_errno_ = 0;
                    return TradeResult::kDbError;
                }
                const TradeResult r = try_trade_sp(o, raw);
                if (r != TradeResult::kBusy) {
                    drop_if_connection_lost();
                    return r;                 // 성공했거나, 재시도해도 소용없는 실패다
                }
                // 롤백은 SP 안에서 이미 했다. 롤백된 트랜잭션은 아무 일도 안 일어난
                // 것이라 그대로 다시 해도 되고, kBusy 는 1213/1205 뿐이라 연결도 멀쩡하다.
            }
            return TradeResult::kBusy;        // 재시도를 다 썼다 — 위층이 "잠시 후 다시" 를 보낸다
        }

    private:
        // 거래 — 저장 프로시저 경로. 왕복 1회이고 문장 폴백은 없다.
        // 도입할 때 문장 경로와 같은 결과를 내는지 trade.ps1 19항목으로 대조했고,
        // 폴백을 지운 지금도 그 하네스가 유일한 판정 기준이다.
        //
        // 락 순서를 맞추는 정렬이 여기 없는 것은 SP 로 옮겨서가 아니라 필요가 없어져서다.
        // SP 가 두 플레이어 행을 id 순으로 먼저 잠그므로, 그 뒤 UPDATE 순서는 데드락과 무관하다.
        //
        // 재시도도 여기서 안 한다. SP 가 RESIGNAL 로 올려 errno 가 그대로 오고,
        // 바깥 trade() 의 루프가 판정한다.
        TradeResult try_trade_sp(const TradeOrder& o, const TradeOrder& raw) {
            unsigned long long a_player = o.a_player;
            unsigned long long a_item   = o.a_item;
            int                a_count  = static_cast<int>(o.a_count);
            unsigned long long b_player = o.b_player;
            unsigned long long b_item   = o.b_item;
            int                b_count  = static_cast<int>(o.b_count);
            unsigned long long zone_id  = raw.zone_id;
            unsigned long long trade_id = raw.trade_id;

            MYSQL_BIND p[8];
            std::memset(p, 0, sizeof(p));
            bind_u64(p[0], a_player);
            bind_u64(p[1], a_item);
            bind_i32(p[2], a_count);
            bind_u64(p[3], b_player);
            bind_u64(p[4], b_item);
            bind_i32(p[5], b_count);
            bind_u64(p[6], zone_id);
            bind_u64(p[7], trade_id);

            if (mysql_stmt_bind_named_param(stmt_trade_sp_, p, 8, nullptr) != 0) {
                last_error_ = mysql_stmt_error(stmt_trade_sp_);
                last_errno_ = mysql_stmt_errno(stmt_trade_sp_);
                connection_lost_ = is_connection_lost(last_errno_);
                return classify_failure();
            }

            // execute 직전에 세운다. 이 값의 규약은 「마지막으로 CALL 한 문장」인데,
            // 예전엔 성공한 뒤에만 세워서 실패 갈래의 do_sanitize() 가 이전 호출의
            // 포인터를 들고 도는 순간이 있었다. 실제로 깨지지는 않았다 — execute 실패는
            // 첫 응답이 ERR 이라는 뜻이고 ERR 에는 결과셋 헤더가 없어 more_results 가 0 이다.
            // 그래도 방어 코드의 전제가 맞아야 다음 사람이 그 위에 쌓을 수 있다.
            last_call_stmt_ = stmt_trade_sp_;

            if (mysql_stmt_execute(stmt_trade_sp_) != 0) {
                last_error_ = mysql_stmt_error(stmt_trade_sp_);
                last_errno_ = mysql_stmt_errno(stmt_trade_sp_);
                connection_lost_ = is_connection_lost(last_errno_);

                // 1305(프로시저 없음)·1370(권한 없음) 이어도 문장 경로로 내려가지 않는다.
                // 그 경로 자체가 없다. errno 는 last_errno_ 에 남아 위층 로그가 찍는다.
                do_sanitize();                     // 실패해도 커넥션은 깨끗하게 돌려준다
                return classify_failure();
            }

            // 결과셋 순회 — MySQL C API 공식 패턴(8.4 매뉴얼 3.6.5).
            //
            // CALL 은 프로시저가 낸 결과셋들에 마지막 상태값을 붙여서 낸다. 상태값은
            // 컬럼이 0개인 빈 결과셋이라 field_count 로 구분한다. 이 루프를 끝까지 돌지
            // 않으면 다음 질의가 Commands out of sync 로 죽는데(Bug #71044, Not a Bug),
            // 피해자는 이 요청이 아니라 다음에 이 커넥션을 빌린 요청이다.
            int  sp_result = static_cast<int>(TradeResult::kDbError);
            bool got       = false;
            int  status    = 0;
            // fetch 가 사유를 남겼는지 기억한다. 안 그러면 아래 !got 갈래가 그 사유를
            // "returned no result" 로 덮어써 진단이 사라진다. last_error_.empty() 로
            // 가르지 않는 이유는 거기 이전 호출의 무관한 메시지가 남아 있을 수 있어서다.
            bool fetch_failed = false;

            do {
                const unsigned num_fields = mysql_stmt_field_count(stmt_trade_sp_);
                if (num_fields > 0) {
                    // 우리가 기대하는 것은 SELECT <result> 하나뿐이다 (1열 1행).
                    if (!got && num_fields == 1) {
                        // 지금 SP 는 리터럴만 내므로 NULL 이 될 수 없지만, is_null 을
                        // 안 받으면 나중에 SP 가 NULL 을 낼 수 있게 바뀌었을 때 조용히 묻힌다.
                        bool is_null = false;

                        MYSQL_BIND col;
                        std::memset(&col, 0, sizeof(col));
                        col.buffer_type = MYSQL_TYPE_LONG;
                        col.buffer      = &sp_result;
                        col.is_null     = &is_null;

                        if (mysql_stmt_bind_result(stmt_trade_sp_, &col) == 0
                            && mysql_stmt_store_result(stmt_trade_sp_) == 0) {
                            // fetch 의 반환값은 셋이다 — 0 성공, 1 에러, 100 행 없음.
                            // 예전엔 == 0 하나로 뭉갰는데, 결과는 안전했지만(둘 다 kDbError)
                            // 실패 사유가 전부 "returned no result" 로 보고돼 원인을 못 찾았다.
                            const int fetch_rc = mysql_stmt_fetch(stmt_trade_sp_);
                            if (fetch_rc == 0) {
                                got = !is_null;    // NULL 이면 「받았다」로 치지 않는다
                                if (is_null) {
                                    last_error_  = "sp_trade: result column is NULL";
                                    fetch_failed = true;
                                }
                            }
                            else if (fetch_rc != MYSQL_NO_DATA) {
                                // 1(에러) 또는 101(MYSQL_DATA_TRUNCATED) — 사유를 남긴다.
                                last_error_ = mysql_stmt_error(stmt_trade_sp_);
                                last_errno_ = mysql_stmt_errno(stmt_trade_sp_);
                                connection_lost_ = is_connection_lost(last_errno_);
                                fetch_failed = true;
                            }
                            // MYSQL_NO_DATA 는 got=false 로 두고 아래 "no result" 가 받는다.
                        }
                        else {
                            // bind/store 실패 갈래. 한동안 비어 있어서 사유가 안 채워진 채
                            // "returned no result" 로 덮였다. 조회 경로에는 같은 자리에
                            // else 가 이미 있었고, 그 비대칭이 결함의 정체였다.
                            last_error_ = mysql_stmt_error(stmt_trade_sp_);
                            last_errno_ = mysql_stmt_errno(stmt_trade_sp_);
                            connection_lost_ = is_connection_lost(last_errno_);
                            fetch_failed = true;
                        }
                    }
                    else {
                        // 기대 밖의 결과셋 — 그래도 받아서 버려야 소켓이 비워진다.
                        mysql_stmt_store_result(stmt_trade_sp_);
                    }
                    mysql_stmt_free_result(stmt_trade_sp_);
                }
                status = mysql_stmt_next_result(stmt_trade_sp_);
            } while (status == 0);

            // status < 0 = 더 없음(정상) — 여기까지 왔으면 커넥션이 깨끗하다.
            // status > 0 = 에러
            if (status > 0) {
                last_error_ = mysql_stmt_error(stmt_trade_sp_);
                last_errno_ = mysql_stmt_errno(stmt_trade_sp_);
                connection_lost_ = is_connection_lost(last_errno_);
                return classify_failure();
            }

            if (!got) {
                // fetch 가 이미 진짜 사유를 남겼으면 덮지 않는다.
                if (!fetch_failed) {
                    last_error_ = "sp_trade returned no result";
                }
                return TradeResult::kDbError;
            }

            // 실패 사유를 채운다. 위층 로그가 이 문자열을 찍는데, 비워 두면
            // 「실패했다」만 남고 「왜」가 사라진다.
            const TradeResult r = to_trade_result(sp_result);
            switch (r) {
            case TradeResult::kOk:         break;
            case TradeResult::kNotEnough:  last_error_ = "not enough items"; break;
            case TradeResult::kInvalidArg: last_error_ = "sp_trade: player not found"; break;
            default:
                last_error_ = "sp_trade returned unknown result "
                            + std::to_string(sp_result);
                break;
            }
            return r;
        }

        // SP 가 돌려준 숫자를 enum 으로. sql\03_procedures.sql 과 값을 맞춰야 한다.
        // 정의가 두 곳에 생기는 것이 SP 도입의 대가다.
        static TradeResult to_trade_result(int v) {
            switch (v) {
            case 0:  return TradeResult::kOk;
            case 1:  return TradeResult::kNotEnough;
            case 4:  return TradeResult::kInvalidArg;
            default: return TradeResult::kDbError;
            }
        }

        // 커넥션을 풀에 돌려주기 전의 위생 검사. MSSQL 의 sp_reset_connection,
        // PostgreSQL 의 DISCARD ALL 에 해당하는 단일 명령이 MySQL 에는 없어 직접 한다.
        //
        // 호출 지점의 결과셋 루프가 정상 경로는 이미 비우므로, 이건 그 루프가 중간에
        // 빠져나간 경우를 위한 두 번째 그물이다 — 한 요청의 실수가 다음에 이 커넥션을
        // 빌린 요청을 죽이는 것을 막는다.
        void do_sanitize() {
            // 예전엔 거래용 문장 포인터를 여기 하드코딩했다가, 거래 SP 미배포 + 조회 SP
            // 배포 조합에서 이 검사가 통째로 스킵된 적이 있다. 조회 SP 가 커넥션을
            // 오염시켜도 아무도 못 잡는 상태였다. 지금은 마지막으로 CALL 한 문장을
            // 가리키므로 SP 종류가 늘어도 하나로 덮인다.
            if (mysql_ == nullptr || last_call_stmt_ == nullptr) {
                return;
            }

            // 플래그가 아니라 연결 상태를 직접 본다. more_results 는 「더 있는가」만
            // 답하고 상태를 바꾸지 않는다. 플래그로 하면 루프가 정상 종료했다고 스스로
            // 판단한 경우를 놓치는데, 실제로 그렇게 만들었다가 주입 검증이 통과해 버려
            // 그물이 없다는 걸 못 볼 뻔했다.
            if (mysql_more_results(mysql_) == 0) {
                return;
            }

            // reset 으로는 다음 결과셋이 안 지워진다. reset 이 치우는 것은 unbuffered
            // 결과와 에러 상태이지 next_result 로 넘어가야 하는 뒤쪽 결과셋이 아니다.
            // reset 만 두고 호출부 루프를 막아 봤더니 out of sync 가 5건 그대로 났다.
            int st = 0;
            do {
                if (mysql_stmt_field_count(last_call_stmt_) > 0) {
                    mysql_stmt_store_result(last_call_stmt_);
                }
                mysql_stmt_free_result(last_call_stmt_);
                st = mysql_stmt_next_result(last_call_stmt_);
            } while (st == 0);

            // drain 이 에러로 중단돼도 정리됐다고 조용히 넘기지 않는다. 반납 직전
            // 마지막 방어선이라, 못 비웠으면 로그라도 남겨야 다음에 이 연결을 빌린 쪽에서
            // 이상이 났을 때 첫 단서가 된다.
            if (st > 0) {
                core::logf("[WARN] do_sanitize: 결과셋 drain 중 에러(%u) — %s\n",
                    mysql_stmt_errno(last_call_stmt_), mysql_stmt_error(last_call_stmt_));
            }

            mysql_stmt_reset(last_call_stmt_);   // 바인딩 외 상태를 초기 상태로
        }

        //  같은 아이템을 서로 주고받으면 「같은 행」을 두 번 만지게 된다.
        //    정렬해도 키가 같아 순서가 정해지지 않고, 지급이 먼저 오면
        //    늘어난 잔량으로 차감이 통과한다 — 조건부 UPDATE 의 전제가 깨진다.
        //    → 미리 상계해서 차액만 옮긴다. 행 하나를 한 번만 만지게 만드는 것이다.
        //
        //  게임 규칙으로도 이게 맞다 — 200 을 5개 주고 3개 받는 거래는
        //    "2개를 준다" 와 결과가 같다. 두 번 만질 이유가 없다.
        static TradeOrder net_order(const TradeOrder& o) {
            TradeOrder r = o;
            if (o.a_count > 0 && o.b_count > 0 && o.a_item == o.b_item) {
                if (o.a_count >= o.b_count) {
                    r.a_count = o.a_count - o.b_count;
                    r.b_count = 0;
                }
                else {
                    r.b_count = o.b_count - o.a_count;
                    r.a_count = 0;
                }
            }
            return r;
        }

        // 1213 데드락과 1205 락 대기 초과만 「그대로 다시 하면 될 수도 있는」 실패다.
        // CHECK 위반이나 문법 오류는 백 번 해도 같고, 구분 없이 다 재시도하면
        // DB 가 아플 때 워커가 거기 붙들린다. (2006/2013 은 연결 끊김 — 재연결 대상)
        static bool is_retryable(unsigned err) {
            return err == 1213 || err == 1205;
        }

        TradeResult classify_failure() const {
            return is_retryable(last_errno_) ? TradeResult::kBusy : TradeResult::kDbError;
        }

        // 끊긴 연결을 버린다. 재연결도 재시도도 여기서 하지 않는다.
        //
        // 이게 없을 때 거래 경로에는 연결 끊김 처리가 통째로 없었다. 조회는 재연결하는데
        // 거래는 분류만 하고 kDbError 로 올렸고, close() 를 안 부르니 죽은 연결이
        // is_open()==true 인 채로 풀에 돌아가 다음에 빌린 워커의 거래까지 같은 자리에서
        // 실패했다. 되살아나는 경로가 「그 연결을 조회가 우연히 빌리는 것」뿐이었고,
        // 버린 수 통계에도 안 잡혔다.
        //
        // 조회처럼 「재연결 후 한 번 재시도」를 안 하는 것은 거래가 멱등이 아니어서다.
        // 커밋 전이면 서버가 롤백하니 다시 해도 되지만 COMMIT 은 불확정이다 — 서버에서
        // 커밋됐는지 알 방법이 없고, 재시도하면 거래가 두 번 일어난다. 단계별로 갈라
        // 한쪽만 재시도할 수도 있지만 「어디서 끊겼나」를 들고 다녀야 하고 그 상태가
        // 늘면 틀릴 자리도 는다. 거래 하나를 포기하는 대가로 그 위험을 안 만든다.
        //
        // close() 하나면 되는 것은 mysql_ 이 nullptr 이 되어 is_open() 이 false 가 되고,
        // 그러면 풀이 「죽은 연결은 넣지 않고 버린다」를 그대로 실행하기 때문이다.
        // 재연결은 풀의 책임이라 여기서 하지 않는다.
        void drop_if_connection_lost() {
            if (!connection_lost_) {
                return;
            }
            // close() 가 last_error_ 를 안 건드려도, 무엇 때문에 버렸는지는 남겨야 한다.
            core::logf("[WARN] 거래 중 연결이 끊겼다 — 이 연결을 버린다 (%s)\n",
                last_error_.c_str());
            close();
        }

        bool connect() {
            connection_lost_ = false;

            mysql_ = mysql_init(nullptr);
            if (mysql_ == nullptr) {
                last_error_ = "mysql_init failed";
                return false;
            }

            // 접속 타임아웃을 짧게 둔다. 기본값은 매우 길어서,
            //   DB 가 응답을 안 하면 워커가 그만큼 붙들린다.
            unsigned timeout = 3;
            mysql_options(mysql_, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
            mysql_options(mysql_, MYSQL_OPT_READ_TIMEOUT, &timeout);
            mysql_options(mysql_, MYSQL_OPT_WRITE_TIMEOUT, &timeout);

            // 연결 문자셋을 utf8mb4 로 맞춘다.
            //   스키마가 utf8mb4 인데 연결이 latin1 이면 한글·이모지가 조용히 깨진다.
            //   「저장은 됐는데 읽으면 이상한」 종류의 버그다.
            mysql_options(mysql_, MYSQL_SET_CHARSET_NAME, "utf8mb4");

            // 격리 수준을 연결이 정한다. 서버 기본값에 기대지 않는다.
            //
            // 성능 조정이 아니라 가정을 명시하는 것이다. 「autocommit 을 끄면 읽기
            // 스냅샷이 고정돼 오래된 값을 보게 된다」는 근거는 REPEATABLE READ 일 때만
            // 성립하는데, 정작 그 수준이 아무 데서도 못 박혀 있지 않았다. 서버 설정이
            // 바뀌면 그 근거가 조용히 거짓이 된다 — 위 문자셋과 같은 이유다.
            // 기본값은 서버 기본값과 같아서 동작은 안 바뀐다. 고치는 것은 수준이 아니라
            // 안 정해져 있던 것이다.
            //
            // INIT_COMMAND 로 거는 이유는 재연결 때 자동으로 다시 실행되기 때문이다.
            // connect() 뒤에 SET 을 손으로 보내면 재연결 경로에서 그 한 줄을 빠뜨리는
            // 날이 온다. 문자열은 mysql_options 가 복사해 가지만 멤버에 담아 두면
            // 누가 소유하는지 안 따져도 된다.
            init_command_ = "SET SESSION TRANSACTION ISOLATION LEVEL " + cfg_.isolation;
            mysql_options(mysql_, MYSQL_INIT_COMMAND, init_command_.c_str());

            // STRICT 모드도 연결이 정한다. 수량 방어의 마지막 층이라서다.
            //   proto::kMaxTradeCount 가 터무니없는 수량을 거르고,
            //   조건부 UPDATE 가 음수를 막고,
            //   DB 가 INT 범위를 넘는 값을 거절한다.
            // 마지막 층은 CHECK 제약이 아니라 sql_mode 가 한다. CHECK 는 음수만 막고,
            // INT 상한을 넘는 덧셈은 STRICT 가 없으면 에러가 아니라 경고 + 잘라 넣기다.
            // 누군가 sql_mode 에서 STRICT 를 빼는 날 아이템이 조용히 INT_MAX 에 눌러앉는다.
            //
            // 덮어쓰지 않고 더하는 것은 sql_mode 에 배포가 정한 다른 모드도 들어 있어서다.
            // NULLIF 는 sql_mode 가 빈 문자열일 때 CONCAT 결과가 ",STRICT_TRANS_TABLES" 가
            // 되어 SET 이 실패하는 것을 막는다.
            init_sqlmode_ =
                "SET SESSION sql_mode = "
                "CONCAT_WS(',', NULLIF(@@SESSION.sql_mode, ''), 'STRICT_TRANS_TABLES')";
            mysql_options(mysql_, MYSQL_INIT_COMMAND, init_sqlmode_.c_str());

            // CLIENT_MULTI_RESULTS 를 명시한다. CALL 은 결과셋 뒤에 상태 패킷을 하나 더
            // 내므로 결과가 하나여도 다중 결과셋을 다룰 줄 알아야 한다. libmysqlclient 가
            // 기본으로 켜 주지만, 기본값에 기대면 그게 바뀌는 날 조용히 틀린다.
            if (mysql_real_connect(mysql_,
                    cfg_.host.c_str(), cfg_.user.c_str(), cfg_.password.c_str(),
                    cfg_.database.c_str(), cfg_.port, nullptr,
                    CLIENT_MULTI_RESULTS) == nullptr) {
                last_error_ = mysql_error(mysql_);
                mysql_close(mysql_);
                mysql_ = nullptr;
                return false;
            }

            return prepare_all();
        }

        static void close_stmt(MYSQL_STMT*& s) {
            if (s != nullptr) {
                mysql_stmt_close(s);
                s = nullptr;
            }
        }

        bool prepare_one(MYSQL_STMT*& out, const char* sql) {
            out = mysql_stmt_init(mysql_);
            if (out == nullptr) {
                last_error_ = "mysql_stmt_init failed";
                return false;
            }
            if (mysql_stmt_prepare(out, sql,
                                   static_cast<unsigned long>(std::strlen(sql))) != 0) {
                last_error_ = mysql_stmt_error(out);
                return false;
            }
            return true;
        }

        // prepare 는 연결마다 한 번만 한다. 질의마다 하면 그 비용을 매번 낸다.
        // 재연결하면 statement 도 같이 죽으므로 여기서 다시 만든다.
        //
        // 셋 다 실패해도 연결을 죽이지 않는다. 서버 계정은 EXECUTE 권한만 갖고 있어서,
        // 테이블을 직접 만지는 문장의 PREPARE 는 1142 로 막힌다 — 그걸 하드 실패로 두면
        // connect() 자체가 죽어 로그인도 채팅도 못 받는다.
        bool prepare_all() {
            prepare_trade_sp();          // 없으면 거래가 kDbError
            prepare_inventory_sp();      // 없으면 조회가 kDbError
            prepare_unsafe_stmts();      // kBadTradeNoTx 대조군 전용
            return true;
        }

        // trade_unsafe() 전용 문장 준비. 실패해도 연결을 죽이지 않는다.
        //
        // 이 문장들은 테이블을 직접 만진다. EXECUTE 권한만 있는 계정으로 실측했더니
        // PREPARE 가 1142 로 막혀 연결 4개 중 하나도 못 만들었다. 하드 실패로 두면
        // 테이블 권한 회수 자체가 불가능해진다.
        //
        // 평시 영향은 없다. kBadTradeNoTx 가 false 라 trade_unsafe() 는 안 불리고,
        // 문장이 nullptr 로 남아도 exec_stmt() 가 "not prepared" 로 정직하게 실패한다.
        void prepare_unsafe_stmts() {
            // 차감. 「검사」와 「차감」이 한 문장 안에 있다.
            //   SELECT 로 잔량을 보고 코드에서 판단한 뒤 UPDATE 하면,
            //   보는 시점과 쓰는 시점 사이에 남이 끼어든다(TOCTOU).
            //   둘 다 "5개 있네" 를 읽고 둘 다 성공한다 — 그게 아이템 복사다.
            //   조건부 UPDATE 는 DB 가 행을 잠근 채로 판정하므로 그 틈이 없다.
            static const char kTakeItem[] =
                "UPDATE inventory SET item_count = item_count - ? "
                " WHERE player_id = ? AND item_id = ? AND item_count >= ?";

            // 지급. 받는 쪽에 그 아이템 줄이 「없을 수 있다」.
            //   UPDATE 만 쓰면 0행이 되어 아이템이 그냥 사라진다.
            //   SELECT 로 있는지 보고 갈라지면 또 TOCTOU 다 —
            //   복합 PK (player_id, item_id) 가 있으니 DB 가 원자적으로 판정하게 맡긴다.
            //
            //   VALUES(item_count) 가 아니라 행 별칭(AS new)을 쓰는 이유 —
            //     ON DUPLICATE KEY UPDATE 안의 VALUES() 는 MySQL 8.0.20 에서
            //     deprecated 됐다. 8.0.19 부터 들어온 별칭 문법이 대체재다.
            //
            //   inventory.item_count 로 한정자를 붙인 이유 — 실제로 겪었다.
            //     별칭을 도입하면 item_count 라는 이름이 「테이블」과 「새 행」 두 곳에서
            //     오므로 MySQL 이 ERROR 1052 (ambiguous) 로 거절한다.
            //    이 실패가 처음엔 「아이템 소멸」로 나타났다 — 차감은 커밋되고
            //       지급만 실패했기 때문이다. 트랜잭션이 없으면 문법 오류 하나가
            //       그대로 데이터 손실이 된다.
            static const char kGiveItem[] =
                "INSERT INTO inventory (player_id, item_id, item_count) VALUES (?, ?, ?) AS new "
                " ON DUPLICATE KEY UPDATE item_count = inventory.item_count + new.item_count";

            // 하드 && 사슬이 아니다 — 한쪽이 실패해도 나머지 준비를 계속 시도하고,
            //   어느 쪽이 실패해도 connect() 는 살려 둔다(prepare_trade_sp() 와 같은 등급).
            if (!prepare_one(stmt_take_, kTakeItem)) {
                // prepare_one 이 실패해도 mysql_stmt_init 은 성공했을 수 있다 —
                //   새지 않게 닫는다(prepare_trade_sp() 와 같은 이유).
                close_stmt(stmt_take_);
                stmt_take_ = nullptr;
                core::logf("[WARN] kBadTradeNoTx 대조군(kTakeItem) 준비 실패 — %s\n"
                           "        → 평시엔 무관하다(kBadTradeNoTx=false)\n",
                    last_error_.c_str());
            }
            if (!prepare_one(stmt_give_, kGiveItem)) {
                close_stmt(stmt_give_);
                stmt_give_ = nullptr;
                core::logf("[WARN] kBadTradeNoTx 대조군(kGiveItem) 준비 실패 — %s\n"
                           "        → 평시엔 무관하다(kBadTradeNoTx=false)\n",
                    last_error_.c_str());
            }
        }

        //  거래 프로시저 문장 준비 — 실패해도 연결을 죽이지 않는다.
        //
        // use_stored_proc 같은 손잡이는 두지 않았다. SP 를 쓸지는 운영자가 판단할 일이
        // 아니라 배포 상태가 정한다 — 있으면 쓰고 없으면 못 쓰는 것뿐이다.
        // 어느 경로로 도는지는 기동 로그 한 줄로 남긴다. 안 남기면 모르는 채 운영하게 되고
        // 회귀가 무엇을 검증했는지도 알 수 없다.
        void prepare_trade_sp() {
            stmt_trade_sp_ = nullptr;

            static const char kCallTrade[] =
                "CALL sp_trade(?, ?, ?, ?, ?, ?, ?, ?)";

            if (!prepare_one(stmt_trade_sp_, kCallTrade)) {
                close_stmt(stmt_trade_sp_);
                stmt_trade_sp_ = nullptr;
                core::logf("[ERROR] sp_trade 미배포 — 거래가 전부 kDbError 로 거절된다\n"
                           "        → sql\\03_procedures.sql 을 배포할 것. 로그인·채팅은 정상\n");
                return;
            }
            core::logf("[INFO] trade path = stored procedure (sp_trade)\n");
        }

        // 조회 SP 도 실패해도 연결을 안 죽인다. 이거 하나 때문에 연결 전체가 죽으면
        // 로그인·채팅까지 막히는데, 한때 감사 로그 문장을 필수 사슬에 넣었다가 정확히
        // 그 사고를 겪었다 — 테이블이 없다는 이유로 모든 DB 기능이 죽었다.
        void prepare_inventory_sp() {
            stmt_inventory_sp_ = nullptr;

            static const char kCallInventory[] =
                "CALL sp_inventory_select(?)";

            if (!prepare_one(stmt_inventory_sp_, kCallInventory)) {
                // prepare 가 실패해도 init 은 성공했을 수 있다. 새지 않게 닫는다.
                close_stmt(stmt_inventory_sp_);
                stmt_inventory_sp_ = nullptr;
                core::logf("[ERROR] sp_inventory_select 미배포 — 조회가 전부 kDbError 로 거절된다\n"
                           "        → sql\\03_procedures.sql 을 배포할 것. 로그인·채팅은 정상\n");
                return;
            }
            core::logf("[INFO] inventory path = stored procedure (sp_inventory_select)\n");
        }

        // 감사 로그가 들어갈 스키마 이름. 빈 문자열이면 쓸 수 없다는 뜻이다.
        //
        // 지금은 호출자가 없다. sp_trade 가 로그 테이블을 하드코딩해서 [db] log_database
        // 설정 자체가 무효가 됐기 때문인데, bootstrap.cpp 의 주석이 이 함수를 이름으로
        // 인용하고 있어 같이 정리하기 전까지는 남겨 둔다.
        //
        // 한때 빈 값을 「게임 DB 에 같이 쓴다」로 처리했다. 그게 틀렸다 — 게임 스키마에는
        // trade_log 가 없어서, 안전한 기본값이라고 적어 둔 경로가 실제로는 없는 테이블을
        // 가리키는 100% 실패 경로였다.
        //
        // 설정에서 온 값이라 형태를 한 번 본다. 못 쓸 글자가 있으면 거절하고 이유를
        // 남긴다 — 오타 하나로 서버가 안 뜨지는 않고 거래만 막힌다.
        std::string log_schema() const {
            const std::string& s = cfg_.log_database;
            if (s.empty()) {
                core::logf("[WARN] [db] log_database 가 비어 있다 —"
                           " 거래 감사 로그를 남길 수 없어 거래가 거절된다\n");
                return {};
            }
            for (const char c : s) {
                const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                             || (c >= '0' && c <= '9') || c == '_';
                if (!ok) {
                    core::logf("[WARN] [db] log_database=\"%s\" — 식별자로 쓸 수 없는"
                               " 글자가 있다. 거래가 거절된다\n", s.c_str());
                    return {};
                }
            }
            return s;
        }

        // 파라미터 채우기. memset 을 호출부마다 적으면 하나를 빠뜨리는 날이 온다.
        static void bind_u64(MYSQL_BIND& b, unsigned long long& v) {
            std::memset(&b, 0, sizeof(b));
            b.buffer_type = MYSQL_TYPE_LONGLONG;
            b.buffer      = &v;
            b.is_unsigned = true;
        }
        static void bind_i32(MYSQL_BIND& b, int& v) {
            std::memset(&b, 0, sizeof(b));
            b.buffer_type = MYSQL_TYPE_LONG;
            b.buffer      = &v;
            b.is_unsigned = false;      // item_count 는 부호 있는 INT 다 (스키마 참조)
        }

        // 차감·지급 — 실행에 성공했는가만 돌려준다. 몇 행이 바뀌었는지는
        // last_affected_ 에 남겨 호출자가 본다. trade_unsafe() 는 그걸 안 보고,
        // 그게 그 함수가 나쁜 이유의 절반이다. 지금 이 둘의 호출자는 그 대조군뿐이다.
        bool exec_take(uint64_t player, uint32_t item, int count) {
            unsigned long long pid = player;
            unsigned long long iid = item;
            int c1 = count;
            int c2 = count;

            MYSQL_BIND p[4];
            bind_i32(p[0], c1);         // SET item_count = item_count - ?
            bind_u64(p[1], pid);
            bind_u64(p[2], iid);
            bind_i32(p[3], c2);         // AND item_count >= ?
            return exec_stmt(stmt_take_, p, 4);
        }

        bool exec_give(uint64_t player, uint32_t item, int count) {
            unsigned long long pid = player;
            unsigned long long iid = item;
            int c1 = count;

            MYSQL_BIND p[3];
            bind_u64(p[0], pid);
            bind_u64(p[1], iid);
            bind_i32(p[2], c1);
            return exec_stmt(stmt_give_, p, 3);
        }

        bool exec_stmt(MYSQL_STMT* stmt, MYSQL_BIND* params, unsigned count) {
            last_affected_ = 0;
            if (stmt == nullptr) {
                connection_lost_ = true;
                last_error_ = "not prepared";
                return false;
            }
            if (mysql_stmt_bind_named_param(stmt, params, count, nullptr) != 0) {
                last_error_ = mysql_stmt_error(stmt);
                last_errno_ = mysql_stmt_errno(stmt);
                connection_lost_ = is_connection_lost(last_errno_);
                return false;
            }
            if (mysql_stmt_execute(stmt) != 0) {
                last_error_ = mysql_stmt_error(stmt);
                last_errno_ = mysql_stmt_errno(stmt);
                connection_lost_ = is_connection_lost(last_errno_);
                return false;
            }
            last_affected_ = mysql_stmt_affected_rows(stmt);
            return true;
        }

        // 2006 = CR_SERVER_GONE_ERROR / 2013 = CR_SERVER_LOST
        // 이 둘만 「재연결하면 살아날 수 있는」 실패다.
        //   문법 오류나 제약 위반은 재연결해도 그대로 실패한다 — 구분해야 한다.
        static bool is_connection_lost(unsigned err) {
            return err == 2006 || err == 2013;
        }

        //  조회 경로 갈래 — trade() 와 같은 규약이다(둘 다 「문장 폴백
        //  없음」이 됐다).
        //
        //  설계에 명시적으로 적힌 코드는 아니지만, 「SP 가 배포돼 있으면
        //    그걸 쓴다」가 실제로 성립하려면 select_inventory() 가 stmt_inventory_sp_
        //    유무로 갈라져야 했다 — trade() 가 stmt_trade_sp_ 로 가르는 것과 같은 이유다.
        //    재연결 뒤에도 다시 판정해야 하므로(재연결 시 SP 준비 여부가 바뀔 수 있다)
        //    select_inventory() 의 두 호출 지점 모두에서 이 함수를 통해 새로 판정한다.
        //
        // SP 가 없으면 조회는 성립하지 않는다. 거래와 같은 사상으로 kDbError 를 올리되
        // 연결은 죽이지 않는다.
        bool select_inventory_dispatch(uint64_t player_id, std::vector<InventoryRow>& out) {
            if (stmt_inventory_sp_ == nullptr) {
                // 재연결 대상이 아니다. connection_lost_ 를 세우면 위층이 재연결을 도는데,
                // SP 미배포는 재연결해도 그대로다.
                connection_lost_ = false;
                out.clear();
                last_error_ = "sp_inventory_select 미배포 — sql\\03_procedures.sql 을 배포하라";
                last_errno_ = 0;
                return false;
            }
            return try_select_inventory_sp(player_id, out);
        }

        // 인벤토리 조회 — CALL sp_inventory_select(?).
        //
        // 거래 쪽 결과셋 루프를 그대로 복사하면 안 된다. 그쪽은 1열 1행만 처리하고
        // 나머지는 else 로 빠지는데, 인벤토리는 2열 N행이라 항상 그 else 로 간다.
        // 게다가 else 가 소켓을 정상적으로 비워 주기 때문에 out of sync 조차 안 나고,
        // 증상은 조용한 빈 목록 하나뿐이다.
        //
        // 그래서 첫 결과셋의 열 개수를 직접 검사한다. 2가 아니면 즉시 실패로 올리고
        // 「행이 0개」와 같은 값으로 흘리지 않는다. 결과셋마다 bind_result 를 다시
        // 부르고(next_result 뒤의 상태는 execute 직후와 같다), 루프는 끝까지 돈다.
        bool try_select_inventory_sp(uint64_t player_id, std::vector<InventoryRow>& out) {
            connection_lost_ = false;
            out.clear();

            if (mysql_ == nullptr || stmt_inventory_sp_ == nullptr) {
                connection_lost_ = true;       // 아직 연결/준비가 없다 — 재연결 대상
                last_error_ = "not connected";
                return false;
            }

            unsigned long long pid = player_id;
            MYSQL_BIND param{};
            std::memset(&param, 0, sizeof(param));
            param.buffer_type = MYSQL_TYPE_LONGLONG;
            param.buffer      = &pid;
            param.is_unsigned = true;

            if (mysql_stmt_bind_named_param(stmt_inventory_sp_, &param, 1, nullptr) != 0) {
                last_error_ = mysql_stmt_error(stmt_inventory_sp_);
                last_errno_ = mysql_stmt_errno(stmt_inventory_sp_);
                connection_lost_ = is_connection_lost(last_errno_);
                return false;
            }

            // execute 직전에 세운다 — 거래 쪽과 같은 규약이다.
            last_call_stmt_ = stmt_inventory_sp_;

            if (mysql_stmt_execute(stmt_inventory_sp_) != 0) {
                last_error_ = mysql_stmt_error(stmt_inventory_sp_);
                last_errno_ = mysql_stmt_errno(stmt_inventory_sp_);
                connection_lost_ = is_connection_lost(last_errno_);

                // 여기에도 둔다. 예전엔 거래 쪽에만 있어서 두 CALL 경로가 비대칭이었다.
                // 동작은 안 바뀌지만(execute 실패면 more_results 가 0 이라 즉시 빠진다),
                // 근거 없는 비대칭을 두면 다음 사람이 한쪽이 빠진 것으로 읽는다.
                do_sanitize();

                core::logf("[WARN] sp_inventory_select 호출 실패(%u) — %s\n",
                    last_errno_, last_error_.c_str());
                return false;
            }

            // 결과셋 순회 — 거래 쪽과 같은 패턴이고, 다른 점은 1열 1행이 아니라 2열 N행뿐이다.
            bool first_seen  = false;   // 첫 실질 결과셋을 이미 봤는가
            bool rows_ok     = false;   // 그 결과셋이 2열이었고 읽기에 성공했다
            bool shape_error = false;   // 그 결과셋이 2열이 아니었다 — §3, 요구 #1
            int  status      = 0;

            do {
                const unsigned num_fields = mysql_stmt_field_count(stmt_inventory_sp_);
                if (num_fields > 0) {
                    if (!first_seen) {
                        first_seen = true;
                        if (num_fields == 2) {
                            unsigned int item_id       = 0;
                            int          count         = 0;
                            bool         is_null[2]    = { false, false };   // 요구 #6

                            MYSQL_BIND cols[2];
                            std::memset(cols, 0, sizeof(cols));
                            cols[0].buffer_type = MYSQL_TYPE_LONG;
                            cols[0].buffer      = &item_id;
                            cols[0].is_unsigned = true;
                            cols[0].is_null     = &is_null[0];
                            cols[1].buffer_type = MYSQL_TYPE_LONG;
                            cols[1].buffer      = &count;
                            cols[1].is_unsigned = false;   // item_count 는 부호 있는 INT (스키마 참조)
                            cols[1].is_null     = &is_null[1];

                            if (mysql_stmt_bind_result(stmt_inventory_sp_, cols) == 0
                                && mysql_stmt_store_result(stmt_inventory_sp_) == 0) {
                                // 루프 전에 true 로 둔다 — 행이 0개인 것도 정상이다.
                                // 에러가 나면 아래에서 따로 내린다.
                                rows_ok = true;

                                // fetch 의 반환값은 셋이다 — 0 성공, 1 에러, 100 더 없음.
                                // == 0 으로만 돌면 에러와 정상 종료가 안 갈려서, 중간에
                                // 실패해도 rows_ok 가 true 로 남아 부분 목록이 성공으로
                                // 반환된다. 101(잘림)도 자른 채 쓰지 않고 에러로 본다.
                                while (true) {
                                    const int fetch_rc = mysql_stmt_fetch(stmt_inventory_sp_);
                                    if (fetch_rc == MYSQL_NO_DATA) {
                                        break;      // 정상 종료 — 더 가져올 행이 없다
                                    }
                                    if (fetch_rc != 0) {
                                        // 1(에러) 또는 101(MYSQL_DATA_TRUNCATED) — 조용한 빈/부분
                                        // 목록으로 흘리지 않는다. 지금까지 담은 out 도 무효로 본다.
                                        rows_ok = false;
                                        last_error_ = mysql_stmt_error(stmt_inventory_sp_);
                                        last_errno_ = mysql_stmt_errno(stmt_inventory_sp_);
                                        connection_lost_ = is_connection_lost(last_errno_);
                                        break;
                                    }
                                    // fetch_rc == 0 — 정상 행.
                                    // 스키마상 NOT NULL 이지만, 이상한 행이 0 으로
                                    // 조용히 섞여 들어가는 것을 막는 방어적 검사다.
                                    if (is_null[0] || is_null[1]) {
                                        // 로그 없이 건너뛰지 않는다. 여기 타면 스키마의
                                        // NOT NULL 이 깨졌다는 뜻인데, 조용히 넘기면 유저에겐
                                        // 아이템이 하나 사라진 것으로 보이고 단서가 안 남는다.
                                        // 행만 버리고 목록 전체를 실패로 떨어뜨리진 않는다.
                                        core::logf("[WARN] sp_inventory_select: NULL 열이 섞였다 "
                                                   "— player=%llu 행 하나를 버린다\n",
                                            static_cast<unsigned long long>(player_id));
                                        continue;
                                    }
                                    InventoryRow row;
                                    row.item_id = item_id;
                                    row.count   = count;
                                    out.push_back(row);
                                }
                            }
                            else {
                                last_error_ = mysql_stmt_error(stmt_inventory_sp_);
                                last_errno_ = mysql_stmt_errno(stmt_inventory_sp_);
                                connection_lost_ = is_connection_lost(last_errno_);
                            }
                        }
                        else {
                            // 첫 결과셋이 2열이 아니다 — 조용한 빈 목록으로 흘리지 않는다.
                            //   그래도 받아서 버려야 소켓이 비워진다(try_trade_sp 의 else 와 같은 이유).
                            shape_error = true;
                            mysql_stmt_store_result(stmt_inventory_sp_);
                        }
                    }
                    else {
                        // 이미 판정이 끝난 뒤 온 기대 밖의 추가 결과셋 — 받아서 버린다.
                        mysql_stmt_store_result(stmt_inventory_sp_);
                    }
                    mysql_stmt_free_result(stmt_inventory_sp_);
                }
                status = mysql_stmt_next_result(stmt_inventory_sp_);
            } while (status == 0);   // 끝까지 돈다 — 중간 이탈 금지

            // status < 0 = 더 없음(정상) — 여기까지 왔으면 커넥션이 깨끗하다.
            // status > 0 = 에러
            if (status > 0) {
                last_error_ = mysql_stmt_error(stmt_inventory_sp_);
                last_errno_ = mysql_stmt_errno(stmt_inventory_sp_);
                connection_lost_ = is_connection_lost(last_errno_);
                out.clear();
                return false;
            }

            // 첫 결과셋이 2열이 아니었거나 2열인데도 bind/store 가 실패했으면
            // 빈 목록이 아니라 실패다.
            //
            // shape_error 는 last_error_ 가 비었든 말든 자기 사유를 덮어쓴다. 결함 주입
            // 때 SP 를 3열로 바꿔 이 검사를 발동시켰더니, 정작 로그에 찍힌 건
            // "INSERT, UPDATE command denied for table 'inventory'" 였다 — 기동 때
            // 대조군 문장의 prepare 가 실패하며 남긴 옛 메시지가 살아 있어서다. 그 실패는
            // 테이블 권한을 회수한 뒤로 매 기동마다 나니 오진이 상시 성립했다.
            // rows_ok 쪽은 그대로 둔다. 거기 담긴 것은 방금 채운 진짜 사유다.
            if (shape_error || !rows_ok) {
                if (shape_error || last_error_.empty()) {
                    last_error_ = "sp_inventory_select: unexpected result shape";
                }
                out.clear();
                return false;
            }

            return true;
        }

        DbConfig    cfg_;
        MYSQL*      mysql_          = nullptr;
        MYSQL_STMT* stmt_take_      = nullptr;   // 조건부 차감
        MYSQL_STMT* stmt_give_      = nullptr;   // UPSERT 지급

        // 둘 다 nullptr 이면 SP 가 배포되지 않았다는 뜻이고, 그때 그 기능은 kDbError 로
        // 거절된다. 설정이 아니라 이 포인터가 경로를 정한다.
        MYSQL_STMT* stmt_trade_sp_  = nullptr;      // CALL sp_trade(...)
        MYSQL_STMT* stmt_inventory_sp_ = nullptr;   // CALL sp_inventory_select(?)

        // 결과셋이 걸린 문장. 반납 위생 검사가 이것을 비운다. SP 가 둘이 되면서
        // 거래용 포인터 하드코딩으로는 못 덮게 됐다 — 거래 SP 미배포 + 조회 SP 배포
        // 조합에서 검사가 통째로 스킵됐다.
        //
        // 이건 「무엇을 비울 것인가」이지 「비울 것이 있는가」가 아니다. 그 판정은 계속
        // more_results 가 한다 — 이 포인터를 플래그처럼 믿는 소비자를 만들지 마라.
        MYSQL_STMT* last_call_stmt_ = nullptr;

        // 연결·재연결 때 서버가 실행할 문장. 둘 다 INIT_COMMAND 로 등록한다.
        // mysql_options 가 복사해 가지만 멤버로 들고 있으면 누가 소유하는지 안 따져도 된다.
        std::string init_command_;      // 격리 수준
        std::string init_sqlmode_;      // STRICT_TRANS_TABLES

        std::string last_error_;
        unsigned    last_errno_      = 0;        // 1213/1205 를 가려내려면 코드가 필요하다
        my_ulonglong last_affected_  = 0;        // 「몇 행이 바뀌었나」 — 좋은 버전만 본다
        bool        connection_lost_ = false;
    };


    // 선언은 클래스 안(public), 정의는 여기 — private 멤버를 쓰므로 클래스 정의 뒤에 둔다.
    inline void DbConn::sanitize() { do_sanitize(); }

}   // namespace db
