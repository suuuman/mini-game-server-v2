#include "app/worker_pool.h"

#include "net/iocp_server.h"
#include "db/db_conn.h"     // mysql_thread_init/end — 이 스레드가 DB 를 직접 부르므로 짝을 맞춘다
#include "core/log.h"

#include <windows.h>

#include <array>

namespace {

    // 부하 주입 스위치 — 평시 0. world/zone_manager.cpp 의 kBadTick* 를 그대로
    // 승계한다(존 틱 루프가 사라지며 이관). 회귀 전에 전부 0 인지 확인할 것.
    constexpr DWORD kBadTickWorkMs = 0;
    constexpr uint64_t kBadTickSpikeEvery = 0;   // 0 = 끔
    constexpr DWORD    kBadTickSpikeMs = 0;

    double hist_percentile_ms(const uint32_t* hist, uint64_t total, double q) {
        if (total == 0) { return 0.0; }
        const uint64_t want = static_cast<uint64_t>(static_cast<double>(total) * q);
        uint64_t acc = 0;
        for (size_t i = 0; i < app::kTickHistBuckets; ++i) {
            acc += hist[i];
            if (acc > want) {
                return static_cast<double>((i + 1) * app::kTickHistBucketNs) / 1e6;
            }
        }
        return static_cast<double>(app::kTickHistBuckets * app::kTickHistBucketNs) / 1e6;
    }

}   // namespace

namespace app {

    void TickStat::add_interval(uint64_t ns, uint64_t period_ns) {
        ++samples;
        interval_sum_ns += ns;
        if (ns > interval_max_ns) { interval_max_ns = ns; }

        const uint64_t d = (ns > period_ns) ? (ns - period_ns) : (period_ns - ns);
        jitter_sum_ns += d;
        if (d > jitter_max_ns) { jitter_max_ns = d; }

        size_t b = static_cast<size_t>(ns / kTickHistBucketNs);
        if (b >= kTickHistBuckets) { b = kTickHistBuckets - 1; }
        ++hist[b];
    }

    // ── WorkerPool ──────────────────────────────────────────────────────

    WorkerPool::WorkerPool(int worker_count, net::IocpServer& server)
        : worker_count_(worker_count > 0 ? worker_count : 1), server_(server) {
    }

    WorkerPool::~WorkerPool() {
        stop();
    }

    void WorkerPool::start() {
        threads_.reserve(static_cast<size_t>(worker_count_));
        for (int i = 0; i < worker_count_; ++i) {
            threads_.emplace_back(&WorkerPool::worker_loop, this);
        }
        core::logf("[INFO] worker pool started (workers=%d)\n", worker_count_);
    }

    void WorkerPool::stop() {
        if (threads_.empty()) {
            return;
        }
        queue_.stop();
        for (auto& t : threads_) {
            if (t.joinable()) {
                t.join();
            }
        }
        threads_.clear();

        core::logf("[WORK ] drains=%llu jobs=%llu cap_hits=%llu resubmits=%llu  batch=%zu workers=%d\n",
            static_cast<unsigned long long>(stat_drains_.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(stat_jobs_.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(stat_cap_hits_.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(stat_resubmits_.load(std::memory_order_relaxed)),
            kSerialDrainBatch,
            worker_count_);
        core::logf("[INFO] worker pool stopped\n");
    }

    bool WorkerPool::submit(net::Session& session, core::Job job) {
        bool need_schedule = false;
        {
            std::lock_guard<std::mutex> lock(session.sq_mutex);
            session.serial_queue.push_back(std::move(job));
            if (!session.sq_scheduled) {
                session.sq_scheduled = true;
                need_schedule = true;
            }
        }
        if (!need_schedule) {
            // 이미 이 세션의 실행권이 큐에 있거나 워커가 드레인 중이다 — 방금 넣은
            // job 은 그 드레인이 직렬 큐를 다시 보는 순간(이번 배치 또는 재제출된
            // 다음 배치) 자연히 처리된다. 새 실행권을 또 올릴 필요가 없다.
            return true;
        }

        // 직렬 큐 스케줄 홀드 — 이 실행권이 큐에서 뽑혀 드레인을 마칠 때까지(또는
        // 재제출을 거쳐 최종적으로 sq_scheduled=false 가 될 때까지) 세션이 살아
        // 있어야 한다. 프레임 하나하나의 홀드(drain_frames 가 이미 잡아 둔 것)와는
        // 별개로 센다 — 직렬 큐에 여러 프레임이 있어도 실행권 자체의 홀드는 하나다.
        server_.acquire_session(session);

        if (!queue_.push([this, sp = &session] { drain_serial_queue(*sp); })) {
            // 큐가 이미 멈췄다(stop 이후). 이 세션의 직렬 큐는 다시는 드레인되지
            // 않으므로 스케줄 홀드와 플래그를 여기서 직접 되돌린다 — stop() 시점엔
            // 이 세션으로 새 프레임을 보낼 스레드가 이미 없다는 것이 net 쪽 종료
            // 순서의 전제이므로(iocp_server.cpp 의 stop() 참고), 직렬 큐에 남는 것이
            // 있어도 그 세션 객체가 곧 정리된다.
            std::lock_guard<std::mutex> lock(session.sq_mutex);
            session.sq_scheduled = false;
            server_.release_session(&session);
            return false;
        }
        return true;
    }

    void WorkerPool::drain_serial_queue(net::Session& session) {
        // [WORK ] 넷 다 relaxed — 통계일 뿐 순서를 만들지 않는다. sq_mutex 가
        // 이미 이 경로의 직렬화를 담당한다. 캐시라인 패딩도 안 넣는다 —
        // 드레인(배치)마다 1~2회라 절대 빈도가 낮고, 같은 경로에서 바로 앞에
        // 잡는 sq_mutex 비용이 지배적이다.
        stat_drains_.fetch_add(1, std::memory_order_relaxed);

        std::array<core::Job, kSerialDrainBatch> batch;
        size_t n = 0;
        {
            std::lock_guard<std::mutex> lock(session.sq_mutex);
            while (n < kSerialDrainBatch && !session.serial_queue.empty()) {
                batch[n++] = std::move(session.serial_queue.front());
                session.serial_queue.pop_front();
            }
        }

        stat_jobs_.fetch_add(n, std::memory_order_relaxed);
        if (n == kSerialDrainBatch) {
            // 잔량이 정확히 K 개라 상한에 「닿기만」 한 경우도 세지만, 그
            // 경계는 resubmits 와의 차이로 읽는다.
            stat_cap_hits_.fetch_add(1, std::memory_order_relaxed);
        }

        for (size_t i = 0; i < n; ++i) {
            batch[i]();
            batch[i] = nullptr;    // 다음 job 까지 캡처를 끌지 않는다
        }

        // sq_scheduled 를 여기서, 즉 배치를 「다 실행한 뒤에」 내린다 — 팝(꺼냄)과
        // 실행 사이에는 실측으로 이 값이 계속 true 로 남아 있어야 한다. 한때
        // 팝 직후(아직 batch[] 를 실행하기 전) 직렬 큐가 비었다는 이유만으로
        // 여기서 false 를 세웠는데, 그러면 그 창에서 다른 워커가 push 한 새
        // 프레임이 「아무도 안 비우고 있다」고 보고 새 실행권을 또 발급했다 —
        // 그 실행권이 지금 이 배치보다 먼저 실행되면 도착 순서가 뒤집힌다
        // (send.ps1 -Seq 3000 이 그 어긋남 8건을 그렇게 잡았다). 이 재확인
        // 임계구역까지 마쳐야 「이 세션 몫의 실행이 전부 끝났다」와 「다음 push 가
        // 새 실행권을 받아도 된다」가 같은 순간에 확정된다.
        bool more = false;
        {
            std::lock_guard<std::mutex> lock(session.sq_mutex);
            more = !session.serial_queue.empty();
            if (!more) {
                session.sq_scheduled = false;
            }
        }

        if (!more) {
            server_.release_session(&session);   // 직렬 큐 스케줄 홀드 반납
            return;
        }

        // 잔량이 있다 — 홀드를 쥔 채로 같은 큐 뒤에 다시 선다. 워커를 놓고
        // 큐로 돌아가는 것이 공정성의 전부다: 다른 세션의 실행권이 먼저 서 있으면
        // 그쪽이 이번에 뽑힌다.
        //
        // push 성공 여부와 무관하게 여기서 먼저 센다 — stop() 과 경쟁해 push 가
        // 실패하는 종료 시점 레이스에서는 1 과대 집계된다. 측정 중에는 안
        // 일어나고 종료 직전에만 가능하므로 그대로 둔다(성공 분기 안으로
        // 옮기면 재제출 블록의 문장 구조를 건드리게 된다).
        stat_resubmits_.fetch_add(1, std::memory_order_relaxed);
        if (!queue_.push([this, sp = &session] { drain_serial_queue(*sp); })) {
            // stop() 이 그 사이 불렸다 — submit() 의 같은 갈래와 동일하게 정리한다.
            std::lock_guard<std::mutex> lock(session.sq_mutex);
            session.sq_scheduled = false;
            server_.release_session(&session);
        }
    }

    void WorkerPool::worker_loop() {
        // 이 스레드가 DB 를 직접 부른다(handle_trade_confirm·handle_inventory 가
        // 직렬 큐 안에서 동기 호출한다) — 옛 db_worker.cpp 의 워커 스레드가 지키던
        // 짝을 그대로 옮긴다. 안 하면 내부 TLS 가 없어 첫 질의부터 실패하거나
        // (mysql_thread_init 누락) 스레드 종료마다 메모리가 샌다(mysql_thread_end 누락).
        mysql_thread_init();

        core::Job job;
        while (queue_.pop(job)) {
            job();
            job = nullptr;
        }

        mysql_thread_end();
    }

    // ── TickThread ──────────────────────────────────────────────────────

    TickThread::TickThread(int tick_hz) : tick_hz_(tick_hz) {
        if (tick_hz_ > 0) {
            period_ = std::chrono::nanoseconds(1'000'000'000LL / tick_hz_);
        }
    }

    TickThread::~TickThread() {
        stop();
    }

    void TickThread::start() {
        if (started_ || tick_hz_ <= 0) {
            // 0 이면 스레드를 아예 안 만든다 — iocp_server.h 의 idle 스윕과 같은
            // 판단이다: 꺼져 있으면 스레드 목록에 없어야 「이 기능은 안 돈다」가 보인다.
            return;
        }
        started_ = true;
        thread_ = std::thread(&TickThread::run, this);
        core::logf("[INFO] tick thread started (hz=%d)\n", tick_hz_);
    }

    void TickThread::stop() {
        if (!started_) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_requested_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
        started_ = false;

        // 존 스레드가 여럿이던 시절엔 스레드마다 편중(skew)을 같이 남겼다. 지금은
        // 틱 스레드가 하나뿐이라 그 비교가 성립하지 않는다 — [TICK ]/[TICK2] 자체가
        // 이미 유일한 스레드의 값이다.
        core::logf("[INFO] tick thread stopped\n");
        core::logf("[TICK ] hz=%d target=%.2fms ticks=%llu behind=%llu (%.2f%%)\n",
            tick_hz_,
            static_cast<double>(period_.count()) / 1e6,
            static_cast<unsigned long long>(stat_.ticks),
            static_cast<unsigned long long>(stat_.behind),
            (stat_.ticks + stat_.behind) > 0
                ? 100.0 * static_cast<double>(stat_.behind)
                    / static_cast<double>(stat_.ticks + stat_.behind)
                : 0.0);

        if (stat_.samples > 0) {
            core::logf("[TICK2] interval avg=%.2fms p99=%.2fms max=%.2fms  "
                       "jitter avg=%.2fms max=%.2fms\n",
                static_cast<double>(stat_.interval_sum_ns) / static_cast<double>(stat_.samples) / 1e6,
                hist_percentile_ms(stat_.hist, stat_.samples, 0.99),
                static_cast<double>(stat_.interval_max_ns) / 1e6,
                static_cast<double>(stat_.jitter_sum_ns) / static_cast<double>(stat_.samples) / 1e6,
                static_cast<double>(stat_.jitter_max_ns) / 1e6);
        }
    }

    void TickThread::run() {
        const auto loop_start = std::chrono::steady_clock::now();
        auto next_tick = loop_start + period_;
        auto last_tick_at = loop_start;

        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            const bool stopped = cv_.wait_until(lock, next_tick,
                [this] { return stop_requested_; });
            if (stopped) {
                break;
            }

            const auto now = std::chrono::steady_clock::now();
            if (now < next_tick) {
                continue;      // 조기 깨움(spurious) — 다시 잔다
            }

            lock.unlock();
            stat_.add_interval(static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    now - last_tick_at).count()),
                static_cast<uint64_t>(period_.count()));
            last_tick_at = now;

            // 게임 틱 훅 자리(DNF Phase 1.2) — 지금은 zone_manager.cpp 시절과
            // 동일하게 아무 일도 안 한다. Zone::on_tick 이 실제로 빈 함수였다.
            if constexpr (kBadTickWorkMs != 0) {
                Sleep(kBadTickWorkMs);
            }
            if constexpr (kBadTickSpikeEvery != 0) {
                static uint64_t spike_n = 0;
                if (++spike_n % kBadTickSpikeEvery == 0) {
                    Sleep(kBadTickSpikeMs);
                }
            }
            stat_.ticks += 1;

            uint64_t advanced = 0;
            do {
                next_tick += period_;
                ++advanced;
            } while (next_tick <= now);
            const uint64_t missed = (advanced > 1) ? (advanced - 1) : 0;
            if (missed > 0) {
                stat_.behind += missed;
            }
            lock.lock();
        }
    }

}   // namespace app
