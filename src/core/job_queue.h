//  core/job_queue.h — I/O 워커와 로직 스레드 사이를 끊는 한 지점
//
//      I/O 워커    : Job 을 넣고 즉시 다음 완료로 돌아간다   (생산자 N)
//      로직 스레드 : 꺼내서 실행한다                        (소비자 1)
//
//  이 큐가 없으면 완료를 꺼낸 워커가 게임 로직까지 다 하게 되고,
//  로직이 길어지는 동안 그 워커는 다음 완료를 못 꺼낸다.
//
//  소비자를 1개로 둔 것이 락을 없애는 근거다 —
//  「한 스레드만 게임 상태를 만진다」가 성립하면 상태에 락이 필요 없다.
#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>

namespace core {

    using Job = std::function<void()>;

    //  pop_until 의 결과는 셋이어야 한다.
    //
    //  pop 은 bool 로 충분했다 — 「Job 을 받았다 / 끝났다」 둘뿐이었으니까.
    //  시한을 두는 순간 세 번째가 생긴다: 아무 일도 없었는데 시간이 됐다.
    //
    //  이걸 bool 로 뭉개면 호출자가 「틱할 때」와 「멈출 때」를 구분 못 한다.
    //    false 를 종료로 보면 타임아웃마다 루프가 죽고,
    //    false 를 타임아웃으로 보면 큐가 멈춰도 루프가 안 죽어 join 이 안 끝난다.
    enum class PopResult {
        kJob,        // out 에 Job 을 넣었다
        kTimeout,    // 시한이 됐다. 큐는 살아 있다
        kStopped     // 멈췄고 남은 것도 없다 → 루프를 끝내도 된다
    };

    class JobQueue {
    public:
        //  false = 큐가 이미 멈췄다. 이 Job 은 영원히 실행되지 않는다.
        //
        //  void 가 아니라 bool 인 것이 이 큐의 핵심 계약이다.
        //    Job 은 세션을 붙들고 있다(io_count 를 올려 둔 채로 큐에 들어간다).
        //    실행돼야 그 홀드가 풀린다. 조용히 버리면 홀드가 영원히 안 풀려서
        //    그 세션은 영원히 안 지워진다 — 누수다.
        //    「못 받았다」를 알려야 호출자가 홀드를 되돌릴 수 있다.
        bool push(Job job) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stopped_) {
                    return false;
                }
                jobs_.push(std::move(job));
            }
            // 락 밖에서 깨운다. 락을 쥔 채로 깨우면 깨어난 스레드가
            // 곧바로 그 락에 다시 막힌다 — 깨운 의미가 없다.
            cv_.notify_one();
            return true;
        }

        //  꺼낼 것이 생길 때까지 블록한다.
        //
        //  true  = out 에 Job 을 넣었다
        //  false = 멈췄고 남은 것도 없다
        //
        //  stopped_ 여도 남은 것이 있으면 먼저 꺼내 준다. 「멈춤」은 새 Job 을
        //    안 받는다는 뜻이지 있던 걸 버린다는 뜻이 아니다 — 버리면 위 push 주석의
        //    홀드 누수가 그대로 종료 경로에서 재현된다.
        //
        //  wait 에 조건식을 주는 이유: condition_variable 은 아무도 안 깨웠는데
        //  깨어날 수 있다(spurious wakeup). 조건식을 주면 다시 자는 처리가 알아서 감긴다.
        bool pop(Job& out) {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return !jobs_.empty() || stopped_; });

            if (jobs_.empty()) {
                return false;                 // 멈췄고 비었다
            }
            out = std::move(jobs_.front());
            jobs_.pop();
            return true;
        }

        //  deadline 까지만 기다린다. 그때까지 아무것도 안 오면 kTimeout.
        //
        //  이 함수가 「소비자가 스스로 틱한다」를 성립시킨다 —
        //  별도 타이머 스레드를 두는 대신 큐 대기 자체에 시한을 붙였다.
        //  틱이 그 스레드의 일이 되므로 틱에서 상태를 만져도 락이 필요 없다.
        //
        //  wait_until 에 조건식을 주면 반환값이 조건식의 최종 값이다 —
        //  false 는 시한이 다 됐는데도 조건이 거짓, 즉 진짜 타임아웃이다.
        //
        //  deadline 이 이미 지났으면 조건식을 한 번 보고 즉시 돌아온다.
        //  밀렸을 때도 큐를 한 번은 보게 되어, 밀림이 곧 굶주림이 되지 않는다.
        PopResult pop_until(std::chrono::steady_clock::time_point deadline, Job& out) {
            std::unique_lock<std::mutex> lock(mutex_);

            if (!cv_.wait_until(lock, deadline,
                                [this] { return !jobs_.empty() || stopped_; })) {
                return PopResult::kTimeout;   // 시한이 됐다. 큐는 살아 있다
            }

            if (jobs_.empty()) {
                return PopResult::kStopped;   // 멈췄고 비었다
            }
            out = std::move(jobs_.front());
            jobs_.pop();
            return PopResult::kJob;
        }

        //  이걸 안 부르면 소비자는 pop 안에서 영원히 잔다 → join 이 안 끝난다.
        //    「스스로는 절대 안 깨어난다」가 블로킹 큐의 계약이다.
        //    stop() 이 안 멈추는 서버의 가장 흔한 원인이 이 한 줄의 누락이다.
        void stop() {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stopped_ = true;
            }
            cv_.notify_all();                 // 소비자가 늘어나도 전부 깨우도록
        }

        size_t size() const {
            std::lock_guard<std::mutex> lock(mutex_);
            return jobs_.size();
        }

    private:
        std::queue<Job>         jobs_;
        mutable std::mutex      mutex_;    // const 인 size() 안에서도 잠가야 해서 mutable
        std::condition_variable cv_;
        bool                    stopped_ = false;
    };

}   // namespace core
