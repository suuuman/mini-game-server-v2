//  app/worker_pool.h — 세션 실행권을 나눠 먹는 워커 N + 틱 전용 스레드 1
//
//  존 스레드 모델(zone_id % N 고정 배정)을 대신한다. 배정을 없앤 이유는
//  §6-2 가 요구하는 「워커 N 을 유동적으로」다 — 고정 배정이면 동기 DB 가
//  물린 워커의 다른 세션까지 전부 막힌다.
//
//  큐에 들어가는 것은 프레임 Job 이 아니라 「세션 실행권」이다. 세션마다
//  직렬 큐(net::Session::sq_mutex·serial_queue·sq_scheduled, session.h)를 두고,
//  submit() 은 프레임을 직렬 큐에 쌓되 sq_scheduled 가 false→true 로 전이할
//  때만 실행권을 공용 큐에 올린다. 워커는 실행권을 뽑아 그 직렬 큐를 도착
//  순서대로(deque FIFO) 비운다 — 그래서 「세션 프레임 순서 = 직렬 큐 순서」가
//  성립한다. mutex 는 배타이지 순서가 아니다 — 같은 세션의 두 프레임을 두
//  워커가 동시에 뽑으면 처리 순서가 스케줄러 몫이 되어 뒤집힐 수 있는데,
//  직렬 큐는 「세션당 in-flight 실행권 1개」로 그 축 자체를 없앤다.
//
//  push 측(submit)과 drain 측(drain_serial_queue)은 둘 다 같은 sq_mutex 임계구역
//  안에서 serial_queue·sq_scheduled 를 함께 본다. 플래그를 별도 atomic CAS 로
//  빼면 덱과 플래그가 다른 락 아래 갈려 실행권이 유실(0 발급)되거나
//  중복 발급(2 발급)되는 레이스가 생긴다 — 그래서 반드시 한 임계구역이다.
//
//  K(kSerialDrainBatch) 는 공정성 상한이다. 한 세션이 직렬 큐에 수십 개를
//  들고 있어도(예: 순서 하네스가 짧은 간격으로 3000 프레임을 밀어 넣는
//  경우) 워커 하나를 계속 독점하면 다른 세션이 굶는다. 상한만큼 비우고
//  잔량이 있으면 실행권 홀드를 쥔 채로 큐 뒤에 다시 선다 — 다른 세션에게
//  차례를 양보하는 것이 이 재제출의 유일한 목적이다.
#pragma once

#include "core/job_queue.h"
#include "net/session.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace net { class IocpServer; }

namespace app {

    // 직렬 큐 드레인 한 번의 상한(공정성). 이 숫자 자체는 안 쟀다 —
    // send -Seq · zone_block 지연 실측으로 재평가할 몫이다.
    constexpr size_t kSerialDrainBatch = 16;

    class WorkerPool {
    public:
        // server 는 acquire_session/release_session 짝을 위해서만 쓴다 — 이 클래스는
        // 그 세션이 「무엇인가」(프로토콜)를 모른다. 스케줄만 안다.
        WorkerPool(int worker_count, net::IocpServer& server);
        ~WorkerPool();

        WorkerPool(const WorkerPool&) = delete;
        WorkerPool& operator=(const WorkerPool&) = delete;

        void start();
        void stop();

        // 이 세션 앞으로 실행 단위 하나(프레임 처리 · 정리 Job)를 직렬 큐에 넣는다.
        // false = 큐가 이미 멈췄다 — 이 job 은 영원히 실행되지 않으므로 호출자가
        // 자신이 들고 있던 홀드를 되돌려야 한다(core::JobQueue::push 와 같은 계약).
        bool submit(net::Session& session, core::Job job);

    private:
        void worker_loop();

        // 직렬 큐를 최대 kSerialDrainBatch 개 비운다. 잔량이 있으면 실행권을
        // 큐 뒤로 재제출하고(홀드 유지), 없으면 스케줄 홀드를 반납한다.
        void drain_serial_queue(net::Session& session);

        core::JobQueue            queue_;
        std::vector<std::thread>  threads_;
        int                       worker_count_;
        net::IocpServer&          server_;
    };

    // 틱 히스토그램 — world/zone_manager.h 의 ZoneThreadStat 이 재던 것과 같은
    // 값이다(0.1ms 버킷 × 4096 = 0~409.6ms). 존 스레드가 여럿이라 스레드마다
    // 따로 세던 것과 달리, 틱 스레드는 이제 하나뿐이라 배열이 아니라 값 하나다.
    constexpr uint64_t kTickHistBucketNs = 100'000;
    constexpr size_t   kTickHistBuckets  = 4096;

    struct TickStat {
        uint64_t ticks   = 0;
        uint64_t behind  = 0;    // 「잃은 틱」이 아니라 지연 지표 — zone_manager.cpp 주석 그대로
        uint64_t samples = 0;
        uint64_t interval_sum_ns = 0;
        uint64_t interval_max_ns = 0;
        uint64_t jitter_sum_ns   = 0;
        uint64_t jitter_max_ns   = 0;
        uint32_t hist[kTickHistBuckets] = {};

        void add_interval(uint64_t ns, uint64_t period_ns);
    };

    // 전용 틱 스레드 1개(D5) — 존 틱 루프가 실제로 하던 일은 빈 on_tick 호출과
    // [TICK ]/[TICK2] 통계뿐이었다. 워커에
    // 얹지 않는 이유는 DB 동기 호출이 워커를 막을 수 있는데, 그 지연이 틱에까지
    // 번지면 「틱이 밀렸다」와 「DB 가 느리다」가 같은 스레드에서 섞여 원인 분석
    // 축이 무너지기 때문이다(전용 스레드가 그 간섭을 원천적으로 차단한다).
    class TickThread {
    public:
        explicit TickThread(int tick_hz);
        ~TickThread();

        TickThread(const TickThread&) = delete;
        TickThread& operator=(const TickThread&) = delete;

        void start();
        void stop();

    private:
        void run();

        int                       tick_hz_;
        std::chrono::nanoseconds period_{ 0 };
        std::thread              thread_;
        std::mutex               mutex_;
        std::condition_variable  cv_;
        bool                     stop_requested_ = false;
        bool                     started_ = false;
        TickStat                 stat_;
    };

}   // namespace app
