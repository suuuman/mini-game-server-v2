//  db/db_pool.h — DB 커넥션 풀
//
// 풀은 연결을 아끼는 장치가 아니라 요청 동시성과 DB 연결 수를 분리하는 장치다.
// 워커당 전용 연결이면 워커 수가 곧 연결 수라, 워커 수는 CPU 병렬성이 정하고 연결
// 수는 DB 부하가 정한다는 사실을 반영할 수 없다. 워커를 늘리면 연결도 따라 늘고,
// 읽기/쓰기 분리나 샤딩이 들어오면 워커당 하나라는 전제가 깨진다.
//
// 순서를 보장하는 것은 큐 분할이지 연결이 아니므로, 큐는 그대로 두고 연결만 풀로 뺐다.
//
// MYSQL* 는 스레드 안전하지 않은데 풀이 되는 이유는, 안전하지 않은 것이 동시 사용이지
// 다른 스레드가 나중에 쓰는 것이 아니어서다. 빌린 동안에는 그 워커만 쓴다. 다만
// mysql_thread_init 은 그 연결을 쓰는 스레드마다 필요하고, 워커가 전부 부르고 있다.
#pragma once

#include "db/db_conn.h"

#include <memory>
#include <mutex>
#include <vector>

namespace db {

    class DbPool {
    public:
        //  RAII 대여증. 소멸하면 자동으로 반납한다.
        //    반납을 손으로 적으면 early return 하나에 연결이 새고,
        //    그러면 풀이 조용히 말라붙는다 — 그때는 원인을 찾기가 아주 어렵다.
        //    반납 지점은 한 곳이어야 한다.
        //
        //  get() 이 nullptr 일 수 있다 — 풀이 비었거나 연결에 실패한 경우.
        //    호출부(app/frame_router.cpp 의 job)는 이미 nullptr 을 다루고 있어 그대로 산다.
        class Lease {
        public:
            Lease() = default;
            Lease(DbPool* pool, std::unique_ptr<DbConn> conn)
                : pool_(pool), conn_(std::move(conn)) {}

            ~Lease();

            Lease(Lease&& o) noexcept
                : pool_(o.pool_), conn_(std::move(o.conn_)) { o.pool_ = nullptr; }
            Lease& operator=(Lease&& o) noexcept {
                if (this != &o) {
                    reset();
                    pool_ = o.pool_;
                    conn_ = std::move(o.conn_);
                    o.pool_ = nullptr;
                }
                return *this;
            }
            Lease(const Lease&) = delete;
            Lease& operator=(const Lease&) = delete;

            DbConn* get() const { return conn_.get(); }
            explicit operator bool() const { return conn_ != nullptr; }

        private:
            void reset();

            DbPool*                 pool_ = nullptr;
            std::unique_ptr<DbConn> conn_;
        };

        struct Stats {
            uint64_t acquired    = 0;   // 빌려준 횟수
            uint64_t open_failed = 0;   // 연결 생성 실패 (DB 가 죽어 있다)
            uint64_t discarded   = 0;   // 죽어서 버린 연결
            size_t   created     = 0;   // 지금 살아 있는 연결 수
            size_t   peak_in_use = 0;   // 동시에 최대 몇 개를 썼나
            size_t   max_size    = 0;

            // 유휴가 없고 상한도 찼을 때 기다리지 않고 그 자리에서 물러난 횟수.
            //   0 이 아니면 풀이 작다.
            uint64_t try_failed = 0;
        };

        DbPool(const DbConfig& cfg, size_t max_size)
            : cfg_(cfg), max_size_(max_size > 0 ? max_size : 1) {}

        ~DbPool() { stop(); }

        DbPool(const DbPool&) = delete;
        DbPool& operator=(const DbPool&) = delete;

        // 유휴 연결이 있으면 그걸 주고, 없어도 상한 안이면 새로 만들어 준다.
        // 그마저 안 되면 기다리지 않고 그 자리에서 빈 Lease 를 돌려준다.
        // 못 빌린 요청을 재큐잉하면 같은 요청이
        // 실패-재시도를 반복하는 기아를 만든다 — 재큐잉 대신 위층이 그 자리에서
        // kBusy 로 답하게 한다(호출자에게는 손해가 바로 드러나고, 재시도는
        // 유저 손에 남는다).
        //
        // open_failed 는 실패의 두 갈래(풀 고갈 vs DB 다운)를 호출부가 가르게
        // 해 준다 — 고갈은 「지금 붐빈다」라 재시도가 의미 있고(kBusy), DB
        // 다운은 「연결 자체가 안 된다」라 운영이 알아야 하는 신호다(kDbError).
        // 둘 다 같은 빈 Lease 로 뭉개면 그 구분이 사라진다. nullptr 을 넘기면
        // (기본값) 그 구분이 필요 없는 호출부는 그대로 산다.
        Lease try_acquire(bool* open_failed = nullptr) {
            std::unique_lock<std::mutex> lock(mutex_);

            if (stopped_) {
                return Lease{};
            }

            // 1) 유휴 연결이 있으면 그걸 준다
            if (!idle_.empty()) {
                std::unique_ptr<DbConn> c = std::move(idle_.back());
                idle_.pop_back();
                on_lend();
                return Lease{ this, std::move(c) };
            }

            // 2) 아직 최대치가 아니면 새로 만든다
            if (created_ < max_size_) {
                ++created_;
                on_lend();

                // 연결 생성은 락 밖에서 한다.
                //   mysql_real_connect 는 타임아웃이 3초다. 락을 쥔 채로 하면
                //   그동안 다른 워커가 전부 멈춘다 — 풀을 둔 의미가 사라진다.
                lock.unlock();

                auto c = std::make_unique<DbConn>();
                if (c->open(cfg_)) {
                    return Lease{ this, std::move(c) };
                }

                lock.lock();
                --created_;
                --in_use_;
                // on_lend() 는 open() 시도 전에 acquired_ 를 먼저 올린다 —
                //   실패하면 대여가 성립하지 않은 것이므로 여기서 되돌린다.
                --acquired_;
                ++open_failed_;
                lock.unlock();
                if (open_failed != nullptr) {
                    *open_failed = true;
                }
                return Lease{};
            }

            // 3) 전부 빌려나갔다 — 여기서 기다리지 않는다.
            ++try_failed_;
            return Lease{};
        }

        //  기동할 때 미리 채운다. lazy 생성이 남긴 문제를 덮는다.
        //
        //  lazy 는 「DB 가 죽어 있어도 기동은 된다」를 위해 고른 것인데,
        //  대가로 첫 요청들이 mysql_real_connect(수 ms)를 물게 된다.
        //  실측 — 기동 직후 240건이 lazy 는 32~65ms, 미리 만든 쪽은 17~21ms.
        //  배포 직후에 그대로 나타나는 지연이라 그냥 두면 안 된다.
        //
        //  그래도 lazy 를 버리지는 않는다 — 여기서 실패해도 기동을 막지 않고,
        //    나중에 대여가 다시 시도한다 — 「DB 죽어도 서버는 뜬다」는 유지된다.
        size_t prewarm() {
            size_t made = 0;
            for (size_t i = 0; i < max_size_; ++i) {
                auto c = std::make_unique<DbConn>();
                if (!c->open(cfg_)) {
                    break;              // DB 가 죽어 있다. 여기서 그만두고 기동은 계속한다
                }
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (stopped_) {
                        break;
                    }
                    ++created_;
                    idle_.push_back(std::move(c));
                }
                ++made;
            }
            return made;
        }

        // 워커를 전부 join 한 「뒤에」 부른다.
        //   먼저 부르면 아직 도는 워커가 빈 Lease 를 받아 질의가 실패한다.
        void stop() {
            std::vector<std::unique_ptr<DbConn>> to_close;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stopped_) {
                    return;
                }
                stopped_ = true;
                to_close.swap(idle_);
                created_ -= to_close.size();
            }
            to_close.clear();       // 락 밖에서 닫는다. mysql_close 는 시간이 걸린다
        }

        Stats stats() const {
            std::lock_guard<std::mutex> lock(mutex_);
            Stats s;
            s.acquired    = acquired_;
            s.open_failed = open_failed_;
            s.discarded   = discarded_;
            s.created     = created_;
            s.peak_in_use = peak_in_use_;
            s.max_size    = max_size_;
            s.try_failed  = try_failed_;
            return s;
        }

    private:
        friend class Lease;

        // 락을 쥔 상태에서 부른다
        void on_lend() {
            ++acquired_;
            ++in_use_;
            if (in_use_ > peak_in_use_) {
                peak_in_use_ = in_use_;     // 이 숫자가 「연결이 몇 개 필요한가」의 답이다
            }
        }

        // 죽은 연결은 풀에 넣지 않고 버린다. 다음 대여가 새로 만든다. 죽은 걸
        // 돌려주면 그걸 빌린 워커가 또 실패하고, 그 연결이 돌아다니며 실패를 퍼뜨린다.
        //
        // 죽었는지는 is_open() 하나로 본다. 즉 DbConn 이 close() 를 불러 줬을 때만
        // 버릴 수 있어서, 두 진입점이 각자 그 책임을 진다. 조회는 멱등이라 close 후
        // 스스로 재연결하고, 거래는 COMMIT 중 끊김이 불확정이라 close 만 한다. 여기까지
        // 온 연결은 되살리기에 실패했거나 되살리지 않기로 한 것이고, 어느 쪽이든 풀이
        // 할 일은 같다.
        //
        // 한때 이 자리에 「DbConn 은 질의 실패 시 스스로 한 번 재연결한다」고 적혀
        // 있었다. 조회 경로에서만 참이었고 거래는 close 조차 안 해서 죽은 연결이
        // is_open()==true 로 그냥 통과했다 — 규칙이 아니라 그 전제가 반쪽이었다.
        void give_back(std::unique_ptr<DbConn> conn) {
            if (conn == nullptr) {
                return;
            }
            const bool alive = conn->is_open();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                --in_use_;

                if (!alive || stopped_) {
                    --created_;
                    if (!alive) {
                        ++discarded_;
                    }
                }
                else {
                    idle_.push_back(std::move(conn));
                }
            }
            conn.reset();           // 락 밖에서 닫는다 (버리는 경우)
        }

        DbConfig                             cfg_;
        size_t                               max_size_ = 1;

        mutable std::mutex                   mutex_;
        std::vector<std::unique_ptr<DbConn>> idle_;
        bool                                 stopped_ = false;

        size_t   created_     = 0;      // 만들어져 있는 연결 수 (유휴 + 대여 중)
        size_t   in_use_      = 0;      // 지금 빌려나가 있는 수
        size_t   peak_in_use_ = 0;
        uint64_t acquired_    = 0;
        uint64_t open_failed_ = 0;
        uint64_t discarded_   = 0;
        uint64_t try_failed_  = 0;
    };

    // DbPool 이 완전한 타입이 된 뒤에 정의한다 (중첩 클래스라 안에서는 못 부른다)
    inline void DbPool::Lease::reset() {
        if (pool_ != nullptr && conn_ != nullptr) {
            // 돌려주기 전에 깨끗하게 만든다.
            //   저장 프로시저(CALL)는 결과셋을 여러 개 내고, 하나라도 남은 채 반납하면
            //   다음에 이 커넥션을 빌린 요청이 Commands out of sync 로 죽는다.
            //   원인과 증상이 다른 요청에서 나타나 추적이 오래 걸린다.
            //   MSSQL 의 sp_reset_connection · PostgreSQL 의 DISCARD ALL 과 같은 자리다.
            conn_->sanitize();
            pool_->give_back(std::move(conn_));
        }
        conn_.reset();
    }

    inline DbPool::Lease::~Lease() {
        reset();
    }

}   // namespace db
