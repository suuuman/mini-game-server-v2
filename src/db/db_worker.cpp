#include "db/db_worker.h"

#include "core/log.h"

namespace db {

    DbWorkerPool::DbWorkerPool(const DbConfig& cfg, int pool_size)
        : cfg_(cfg), pool_(cfg, static_cast<size_t>(pool_size > 0 ? pool_size : 1)) {
    }

    DbWorkerPool::~DbWorkerPool() {
        stop();
    }

    bool DbWorkerPool::start() {
        // 멀티스레드에서 MySQL 을 쓰기 전에 프로세스당 한 번 부른다. mysql_init 이
        //   자동으로 하기도 하지만, 그건 「첫 호출이 어느 스레드인지」에 의존하는
        //   초기화라 경쟁이 생길 수 있다. 명시적으로 먼저 해 두는 게 맞다.
        if (mysql_library_init(0, nullptr, nullptr) != 0) {
            core::logf("[FAIL] mysql_library_init 실패\n");
            return false;
        }

        // 연결을 미리 만들어 둔다. 안 하면 첫 요청들이 mysql_real_connect 를 물어
        //   기동 직후가 느리다(db_pool.h prewarm() 주석의 실측 참고). 실패해도
        //   기동은 계속한다 — DB 가 죽어도 서버는 떠야 한다.
        //   여기는 메인 스레드다. 연결을 만드므로 mysql_thread_init 짝을 맞춘다.
        mysql_thread_init();
        const size_t warmed = pool_.prewarm();
        mysql_thread_end();

        core::logf("[INFO] db pool started (conns=%zu/%zu prewarmed, %s@%s:%u/%s)\n",
            warmed, pool_.stats().max_size,
            cfg_.user.c_str(), cfg_.host.c_str(),
            cfg_.port, cfg_.database.c_str());

        if (warmed < pool_.stats().max_size) {
            core::logf("[WARN] db 연결을 %zu/%zu 만 미리 만들었다 — 나머지는 요청 시 시도\n",
                warmed, pool_.stats().max_size);
        }
        return true;
    }

    void DbWorkerPool::stop() {
        if (stopped_) {
            return;     // 명시 호출 뒤 소멸자가 다시 부르는 경우의 가드
        }
        stopped_ = true;

        // 이 스레드(메인)가 남은 유휴 연결을 닫는다 — mysql_close 를 부르는
        //   스레드도 mysql_thread_init/end 짝이 있어야 한다.
        mysql_thread_init();
        pool_.stop();
        mysql_thread_end();

        const DbPool::Stats p = pool_.stats();
        // 「모자란가」는 peak 가 아니라 try_failed 가 답한다.
        //   0이면 유휴가 없어 즉시 거절한 적이 없다는 뜻 — 풀이 넉넉하다.
        //   0이 아니면 그만큼 요청이 kBusy 로 돌아갔고, 풀을 키울 근거가 된다.
        core::logf("[POOL2] db conns peak=%zu / %zu  acquired=%llu"
                   "  open_failed=%llu  discarded=%llu  try_failed=%llu\n",
            p.peak_in_use, p.max_size,
            static_cast<unsigned long long>(p.acquired),
            static_cast<unsigned long long>(p.open_failed),
            static_cast<unsigned long long>(p.discarded),
            static_cast<unsigned long long>(p.try_failed));

        mysql_library_end();
    }

}   // namespace db
