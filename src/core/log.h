// core/log.h — 콘솔과 파일에 함께 남기는 비동기 로그.
//
// 게임 스레드는 큐에 넣기만 한다(수십 ns). 파일과 콘솔은 로거 스레드가 독점하므로
// 쓰기 경로에 락이 없고, fflush 는 줄당이 아니라 배치당 한 번이다.
//
// 로깅 비용의 대부분은 flush 가 아니라 콘솔 출력이다. 비동기로 바꿔도 그 비용은
// 사라지지 않고 로거 스레드로 옮겨갈 뿐이라, 콘솔이 켜져 있으면 로거 하나가 초당
// 2만 줄쯤에서 막힌다. 상한·드롭 정책과 콘솔 끄기가 함께 필요한 이유다.
//
// 콘솔과 파일을 한 스레드가 쓰는 것은 따로 쓰면 두 출력의 줄 순서가 달라지기
// 때문이다. 로그 두 벌을 대조하다 없는 버그를 쫓게 된다.
#pragma once

#include <windows.h>

#include <share.h>      // _SH_DENYWR — 읽기는 허용하고 열기 위해

#include <atomic>
#include <chrono>           // flush_try 의 시한 — steady_clock
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>          // level_of 의 strncmp
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace core {

    //  로그 레벨 — 「무엇을 버려도 되는가」를 정하기 위한 것이다.
    //
    //  드롭이 무차별적이면 [WARN] 같은 진단의 핵심까지 같이 사라진다.
    //  버려도 되는 것과 안 되는 것을 나누지 않으면, 유실은 그 자체로 사고가 된다.
    //
    //  세 단계면 충분하다. 드롭 정책에서 필요한 구분은 「지킬 것 / 버릴 것」뿐이고,
    //    debug 와 info 를 더 나눠도 정책이 달라지지 않는다.
    enum class LogLevel : uint8_t {
        kInfo = 0,
        kWarn = 1,
        kError = 2,
    };

    // 큐가 가득 찼을 때 「어느 레벨까지 버릴 것인가」
    //   kNone → 아무것도 안 버린다 (기본)
    //   kInfo → 일반 로그만 버리고 WARN·ERROR 는 지킨다
    //   kWarn → ERROR 만 지킨다
    enum class DropLevel : uint8_t {
        kNone = 0,
        kInfo = 1,
        kWarn = 2,
    };

    inline DropLevel drop_level_from(const std::string& s) {
        if (s == "info") { return DropLevel::kInfo; }
        if (s == "warn") { return DropLevel::kWarn; }
        return DropLevel::kNone;                 // 알 수 없는 값 = 안 버린다 (안전한 쪽)
    }

    //  drop_level 과 다른 것이다. 헷갈리면 둘 다 잘못 잡는다 —
    //      level      : 「애초에 안 남긴다」. 큐에 넣지도 않는다
    //      drop_level : 「남기려 했는데 큐가 찼다」. 그때 무엇을 버릴 것인가
    //    앞은 평시 로그량을 줄이는 것이고, 뒤는 사고 시 무엇을 지킬 것인가다.
    //
    //  알 수 없는 값은 kInfo(다 남긴다)로 간다 — 오타 하나로 로그가
    //    조용히 사라지는 쪽보다, 많이 남는 쪽이 낫다.
    inline LogLevel log_level_from(const std::string& s) {
        if (s == "warn")  { return LogLevel::kWarn; }
        if (s == "error") { return LogLevel::kError; }
        return LogLevel::kInfo;
    }

    class Log {
    public:
        static Log& get() {
            // 함수 지역 static 이라 첫 호출 때 한 번만 만들어진다(C++11 부터 스레드 안전).
            //   전역 객체로 두면 다른 전역과의 초기화 순서가 정해지지 않는다.
            static Log instance;
            return instance;
        }

        Log(const Log&) = delete;
        Log& operator=(const Log&) = delete;

        // 큐 상한. 넘으면 버린다. 절대 막지 않는다 — 로그가 밀렸다고 게임 스레드를
        // 세우는 건 로거의 권한이 아니다. 무한히 쌓으면 메모리가 터지므로, 버리되
        // 버렸다는 사실은 세어 둔다.
        //
        // 8192 는 한 줄 100B 남짓으로 약 800KB 이고, 콘솔이 켜진 상태의 처리량으로
        // 0.4초치다. 그보다 오래 밀린다면 큐를 키울 문제가 아니라 로그가 너무 많은 것이다.
        //
        // 다만 기본값은 아무것도 안 버린다. 큐가 가득 찬다면 로거가 아니라 설정의
        // 문제고(로그가 많거나 콘솔이 켜졌거나 상한이 작거나), 조용히 버려 덮으면
        // 원인이 안 보인다. 버릴지 말지는 [log] drop_level 로 운영이 정한다.
        static constexpr size_t kDefaultQueueLimit = 8192;

        // 하드 상한 = 소프트 상한 × 8. 안 버리기로 했다는 게 메모리가 터져도 좋다는
        // 뜻은 아니다. 여기 닿으면 레벨 무관하게 버리고 따로 센다 — 그 숫자가 0 이
        // 아니면 설정이 틀렸다는 뜻이다. 배수인 것은 상한을 키우면 같이 커져야 해서다.
        static constexpr size_t kHardLimitFactor = 8;

        // 이 레벨 미만은 큐에 넣지도 않는다. 설정을 읽은 뒤 부른다.
        void set_min_level(LogLevel lv) {
            min_level_.store(lv, std::memory_order_relaxed);
        }

        // 큐 상한과 드롭 정책. 설정을 읽은 뒤 main 이 부른다.
        void set_policy(size_t queue_limit, DropLevel drop) {
            if (queue_limit > 0) {
                queue_limit_.store(queue_limit, std::memory_order_relaxed);
            }
            drop_level_.store(drop, std::memory_order_relaxed);
        }

        // 콘솔을 끌 수 있어야 한다 — 측정에서 콘솔이 전체 비용의 88% 였다.
        //   비동기로 옮겨도 그 비용은 사라지지 않고 로거 스레드로 옮겨갈 뿐이라,
        //   콘솔이 켜져 있으면 로거 스레드가 초당 2만 줄에서 막힌다.
        //   → 콘솔 출력은 개발 편의이지 운영 기능이 아니다. 실서비스는 파일에만 남긴다.
        //
        //   atomic 인 이유 — 설정을 읽는 시점(main)과 쓰는 시점(로거 스레드)이 다르다.
        //     배치 시작 때 한 번 읽어서 그 배치 내내 같은 값을 쓴다.
        void set_console(bool on) { console_.store(on, std::memory_order_relaxed); }

        // 누적 유실. 보고용 dropped_ 는 한 줄 남길 때마다 0으로 리셋되지만
        //   이건 안 한다 — 「이번 배치에서 몇 줄」과 「뜬 뒤로 총 몇 줄」은 다른 질문이다.
        //   운영에서는 뒤쪽을 본다: 0이 아니면 로그 설계를 다시 볼 때다.
        uint64_t dropped_total() const {
            return dropped_total_.load(std::memory_order_relaxed);
        }

        // 정책대로 버린 것과 「하드 상한에 닿아 어쩔 수 없이」 버린 것은
        //   심각도가 전혀 다르다. 뒤쪽이 0이 아니면 설정이 틀린 것이다.
        uint64_t dropped_forced() const {
            return dropped_forced_.load(std::memory_order_relaxed);
        }

        // 실패해도 서버는 뜬다. 로그를 못 남기는 것이 서비스를 멈출 이유는 아니다.
        //
        // fopen 이 아니라 _fsopen(_SH_DENYWR) 인 것은 fopen 이 파일을 배타적으로 잠가
        // 서버가 도는 동안 아무도 그 로그를 못 열기 때문이다. 로그를 남기는 목적 자체가
        // 돌고 있는 서버를 들여다보는 것인데 그게 막힌다. 실제로 fopen 으로 만들었다가
        // 바로 걸렸다. _SH_DENYWR 은 남의 쓰기만 막고 읽기는 허용한다.
        //
        // 파일을 연 뒤에 스레드를 띄운다. 그래야 file_ 을 로거 스레드가 독점하고 쓰기
        // 경로에서 락이 사라진다. 닫을 때는 반대로 join 한 뒤에 닫는다.
        void open(const char* path) {
            if (running_) {
                return;
            }
            file_ = _fsopen(path, "w", _SH_DENYWR);
            if (file_ == nullptr) {
                std::fputs("[WARN] 로그 파일을 열 수 없다 — 콘솔로만 남긴다\n", stdout);
            }
            running_ = true;
            stop_ = false;
            thread_ = std::thread(&Log::worker_loop, this);
        }

        //  남은 줄을 마저 쓰고 끝낸다.
        //    stop 만 세우고 join 하면 버퍼에 있던 줄이 사라진다. 정상 종료인데도
        //    마지막 로그가 없어서 「서버가 어디까지 갔는지」를 못 보게 된다.
        //    → worker_loop 는 「stop_ 이면서 비었을 때」만 빠져나온다.
        //
        //  main 의 종료 경로에서 로거를 「가장 마지막에」 닫아야 한다.
        //    다른 정리 코드가 로그를 남기기 때문이다.
        void close() {
            if (running_) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    stop_ = true;
                }
                cv_.notify_one();
                if (thread_.joinable()) {
                    thread_.join();
                }
                running_ = false;
            }
            // join 뒤에 닫는다. 도는 동안에는 아무도 file_ 을 안 만진다.
            if (file_ != nullptr) {
                std::fclose(file_);
                file_ = nullptr;
            }
        }

        //  게임 스레드가 하는 일은 여기까지다 — 만들어서 넣기만 한다.
        //
        //  타임스탬프와 tid 를 「여기서」 만든다. 로거 스레드에서 만들면
        //    「발생 시각」이 아니라 「기록 시각」이 찍힌다 — 큐가 밀린 만큼 어긋나고,
        //    그러면 로그로 시간을 추적할 수 없게 된다.
        //    비동기 로거에서 가장 흔한 함정이고, 성능을 위해 정확도를 잃는 거래다.
        //    이건 하면 안 되는 거래다. 로그의 존재 이유가 시간 추적이기 때문이다.
        void emit(const char* msg) {
            // 레벨 필터가 맨 앞이다 — 걸러질 줄에는 타임스탬프 조립 비용조차 안 낸다.
            //   이건 드롭이 아니라서 세지 않는다. 「안 남기기로 한 것」은 유실이 아니다.
            //     여기서 세면 [WARN] 로그 유실 보고가 설정대로 동작한 것까지 세게 된다.
            const LogLevel lv = level_of(msg);
            if (lv < min_level_.load(std::memory_order_relaxed)) {
                return;
            }

            SYSTEMTIME t;
            GetLocalTime(&t);

            // 스레드 ID 를 매 줄에 박는다. 스레드가 여럿이면 「어느 스레드가 했나」가
            //   로그의 핵심 정보다 — I/O 워커 N개 + 존 스레드 N개가 섞여 찍히기 때문이다.
            char line[1200];
            std::snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u [t%-5lu] %s",
                static_cast<unsigned>(t.wHour), static_cast<unsigned>(t.wMinute),
                static_cast<unsigned>(t.wSecond), static_cast<unsigned>(t.wMilliseconds),
                GetCurrentThreadId(), msg);

            const size_t    soft = queue_limit_.load(std::memory_order_relaxed);
            const DropLevel dl = drop_level_.load(std::memory_order_relaxed);

            {
                std::lock_guard<std::mutex> lock(mutex_);

                if (front_.size() >= soft) {
                    // 여기서 기다리면 게임이 멈춘다. 버리거나, 더 쌓거나 둘 중 하나다.
                    if (droppable(lv, dl)) {
                        ++dropped_;
                        dropped_total_.fetch_add(1, std::memory_order_relaxed);
                        return;         // notify 도 안 한다. 이미 로거는 일하는 중이다
                    }

                    // 정책상 지켜야 할 줄이다 — 하드 상한까지는 더 쌓는다.
                    //   그마저 넘으면 레벨 무관하게 버린다. 「안 버리기로 했다」가
                    //   「메모리가 터져도 좋다」는 아니기 때문이다.
                    //   이 카운터가 0이 아니면 설정이 틀렸다는 뜻이다.
                    if (front_.size() >= soft * kHardLimitFactor) {
                        ++dropped_;
                        dropped_total_.fetch_add(1, std::memory_order_relaxed);
                        dropped_forced_.fetch_add(1, std::memory_order_relaxed);
                        return;
                    }
                }
                front_.emplace_back(line);
            }
            // 락을 놓고 나서 깨운다. 락 안에서 깨우면 깬 스레드가 바로 다시 막힌다.
            cv_.notify_one();
        }

        // 지금 큐에 있는 것을 즉시 내보낸다. 비동기로 바꾸면서 잃은 것이 이것이다 —
        // 매 줄 fflush 하던 동기 로거는 크래시 직전 줄이 파일에 남았는데, 비동기는
        // 버퍼에 있던 줄이 통째로 날아간다. 크래시 핸들러가 그 구멍을 메운다.
        //
        // 한계가 있다. 크래시 컨텍스트에서 뮤텍스를 잡는 건 안전하지 않고, 그 락을
        // 이미 죽은 스레드가 쥐고 있으면 여기서 멈춘다. 크래시가 로거 스레드 자체를
        // 죽였다면 이것도 못 살린다.
        void flush() {
            std::vector<std::string> local;
            uint64_t lost = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                local.swap(front_);
                lost = dropped_;
                dropped_ = 0;
            }
            write_batch(local, lost);
        }

        // 크래시 핸들러 전용 flush. 절대 무한 대기하지 않는다.
        //
        // 위 flush() 는 락을 그냥 기다리는데, 평시에는 옳지만 크래시 핸들러에서는
        // 그 락을 죽은 스레드가 쥐고 있을 수 있다. 데드락으로 죽었다면 특히 그렇다.
        // 거기서 기다리면 프로세스가 안 죽고 매달리는데, 그게 죽는 것보다 나쁘다 —
        // 감시 프로세스가 재기동을 못 걸고 서버는 접속만 받고 응답을 안 한다.
        //
        // 그래서 시한 안에서 try_lock 을 반복하고, 못 잡으면 포기한다. 비동기 로거는
        // 애초에 잃을 수 있다는 전제 위에 있어서 여기서 몇 줄 더 잃는 것은 설계 안에
        // 있는 손실이다. 평시 경로인 flush() 는 한 줄도 안 고쳤다.
        //
        // false 는 시한 안에 못 밀어냈다는 뜻이고, 그때 무엇을 할지는 호출자가 정한다.
        bool flush_try(unsigned timeout_ms) {
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(timeout_ms);

            std::vector<std::string> local;
            uint64_t lost = 0;
            if (!lock_until(mutex_, deadline)) {
                return false;
            }
            {
                std::lock_guard<std::mutex> adopt(mutex_, std::adopt_lock);
                local.swap(front_);
                lost = dropped_;
                dropped_ = 0;
            }

            // mutex_ 를 놓고 io_mutex_ 를 잡는다 — 둘을 동시에 안 쥔다.
            //   로거 스레드도 같은 규칙이라(위 loop) 잠금 순서 고리가 안 생긴다.
            //   그 사이에 로거 스레드가 먼저 쓸 수 있다. 순서가 살짝 섞이는데,
            //     죽는 중에 몇 줄 순서가 섞이는 것보다 매달리는 게 훨씬 나쁘다.
            if (!lock_until(io_mutex_, deadline)) {
                return false;             // 이때 local 의 줄들은 잃는다
            }
            {
                std::lock_guard<std::mutex> io_adopt(io_mutex_, std::adopt_lock);
                for (const std::string& s : local) {
                    if (file_ != nullptr) {
                        std::fputs(s.c_str(), file_);
                    }
                    if (console_.load(std::memory_order_relaxed)) {
                        std::fputs(s.c_str(), stdout);
                    }
                }
                if (lost > 0) {
                    char note[200];
                    std::snprintf(note, sizeof(note),
                        "[WARN ] 로그 %llu 줄 유실 (큐 상한)\n",
                        static_cast<unsigned long long>(lost));
                    if (file_ != nullptr) { std::fputs(note, file_); }
                }
                if (file_ != nullptr) { std::fflush(file_); }
            }
            return true;
        }

    private:
        Log() = default;

        // std::mutex 에는 try_lock_for 가 없다(그건 timed_mutex 다).
        //   타입을 timed_mutex 로 바꾸면 평시 emit 경로의 락이 바뀐다.
        //     크래시 경로 하나 때문에 평시 경로를 흔들지 않는다.
        //   → try_lock 을 짧게 재우며 반복한다. 시한이 200ms 라 최악 200번이다.
        static bool lock_until(std::mutex& m,
                               std::chrono::steady_clock::time_point deadline) {
            for (;;) {
                if (m.try_lock()) {
                    return true;
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        ~Log() { close(); }

        // 태그로 레벨을 판정한다. 호출부를 안 고치려는 절충이다 — 로그가 이미
        // [WARN]·[INFO] 태그 규약을 지키고 있어 그걸 재사용한다. 정석은 레벨을 인자로
        // 받는 것이고, 태그를 잘못 쓰면 레벨이 틀려도 컴파일러가 못 잡는다.
        //
        // 실제로 [FAIL] 이 이 표에서 빠져 있었다. bind·listen·WSARecv·WSAStartup·
        // mysql_library_init 이 전부 그 태그를 쓰는데 표에 없어 kInfo 로 떨어졌고,
        // level=warn 이면 그 실패들이 통째로 사라지고 drop_level=info 면 큐가 찰 때
        // 제일 먼저 버려졌다. 무엇을 지킬지 정하는 표에서 정작 지켜야 할 것이 빠져
        // 있었던 셈이다.
        //
        // [FATAL] 은 지금 안 쓰지만 남긴다. 안 쓰여서 지운다는 게 위 버그를 만든
        // 사고방식이다.
        static LogLevel level_of(const char* msg) {
            if (msg == nullptr || msg[0] != '[') {
                return LogLevel::kInfo;
            }
            if (std::strncmp(msg + 1, "WARN", 4) == 0)  { return LogLevel::kWarn; }
            if (std::strncmp(msg + 1, "ERROR", 5) == 0) { return LogLevel::kError; }
            if (std::strncmp(msg + 1, "FATAL", 5) == 0) { return LogLevel::kError; }
            if (std::strncmp(msg + 1, "FAIL", 4) == 0)  { return LogLevel::kError; }
            return LogLevel::kInfo;
        }

        static bool droppable(LogLevel lv, DropLevel dl) {
            switch (dl) {
            case DropLevel::kNone: return false;                     // 아무것도 안 버린다
            case DropLevel::kInfo: return lv == LogLevel::kInfo;     // WARN·ERROR 는 지킨다
            case DropLevel::kWarn: return lv != LogLevel::kError;    // ERROR 만 지킨다
            }
            return false;
        }

        //  이중 버퍼 — 송신 큐와 같은 구조다.
        //
        //      생산자들 → [ front_ ]        로거 스레드 → [ local ] → 파일·콘솔
        //                     ↑ 락은 swap 순간에만
        //
        //    「파일에 쓰는 중에도 로그를 남길 수 있어야 한다」가 요구인데,
        //    「소켓으로 보내는 중에도 큐에 쌓을 수 있어야 한다」와 문제의 모양이 같다.
        //
        //  swap 이라 두 벡터의 capacity 가 서로 오간다 — 몇 번 돌고 나면
        //    양쪽 다 필요한 만큼 잡고 있어서 재할당이 거의 안 난다.
        void worker_loop() {
            std::vector<std::string> local;

            for (;;) {
                uint64_t lost = 0;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    cv_.wait(lock, [this] { return stop_ || !front_.empty(); });

                    // 「stop_ 이면서 비었을 때」만 나간다.
                    //   stop_ 만 보고 나가면 남은 줄이 사라진다.
                    if (front_.empty()) {
                        break;
                    }
                    local.swap(front_);

                    // 유실 개수도 여기서 가져간다. 락 밖에서 읽으면 그 사이에 또 는다.
                    lost = dropped_;
                    dropped_ = 0;
                }

                write_batch(local, lost);
                local.clear();          // capacity 는 남는다 — 다음 배치가 재할당을 안 한다
            }
        }

        // 락 밖에서 돈다. 여기가 느려도(콘솔 50μs/line) 게임 스레드는 안 막힌다.
        //   그게 이 단계의 전부다.
        //
        //  io_mutex_ 는 「로거 스레드와 flush() 호출자」 사이의 것이다.
        //    게임 스레드는 이 락을 잡지 않는다 — emit 은 mutex_ 만 쓴다.
        //    평시에는 로거 스레드만 잡으므로 경합이 0이다.
        void write_batch(const std::vector<std::string>& lines, uint64_t lost) {
            if (lines.empty() && lost == 0) {
                return;
            }

            std::lock_guard<std::mutex> io_lock(io_mutex_);
            const bool to_console = console_.load(std::memory_order_relaxed);

            for (const std::string& s : lines) {
                if (to_console) {
                    std::fputs(s.c_str(), stdout);
                }
                if (file_ != nullptr) {
                    std::fputs(s.c_str(), file_);
                }
            }

            // 유실을 「한 줄로」 보고한다. 잃은 줄은 못 살리지만
            //   「몇 줄을 잃었는지」는 잃지 않는다. 그게 드롭 정책의 조건이다.
            if (lost > 0) {
                char note[200];
                std::snprintf(note, sizeof(note),
                    "[WARN ] 로그 %llu 줄 유실 — 큐 상한 %zu 초과 (로그가 많거나 출력이 느리다)\n",
                    static_cast<unsigned long long>(lost),
                    queue_limit_.load(std::memory_order_relaxed));
                if (to_console) {
                    std::fputs(note, stdout);
                }
                if (file_ != nullptr) {
                    std::fputs(note, file_);
                }
            }

            // flush 를 「줄당」이 아니라 「배치당」 한 번으로.
            //   측정에서 매 줄 flush 가 3,500~6,300 ns/line 이었다.
            //   배치가 100줄이면 그 비용이 100분의 1로 나뉜다.
            //   그래도 배치마다는 한다 — 안 하면 크래시 때 잃는 양이 통제 불능이 된다.
            if (file_ != nullptr) {
                std::fflush(file_);
            }
        }

        // file_ 은 open 에서 스레드 시작 「전에」 정해지고 close 에서 join 「후에」 치운다.
        //   그래서 로거 스레드가 도는 동안 아무도 이 값을 안 바꾼다 — 락이 필요 없다.
        //   여기서도 해법은 「만질 사람을 하나로 정한다」다.
        std::FILE* file_ = nullptr;

        std::mutex               mutex_;      // 큐 — 게임 스레드가 잡는 유일한 락
        std::mutex               io_mutex_;   // 파일·콘솔 — 로거 스레드와 flush() 사이
        std::condition_variable  cv_;
        std::vector<std::string> front_;      // 생산자들이 쌓는다
        std::thread              thread_;
        uint64_t                 dropped_ = 0;        // 보고할 때마다 0으로 (mutex_ 아래)
        std::atomic<uint64_t>    dropped_total_{ 0 }; // 누적 — 리셋 안 한다
        std::atomic<uint64_t>    dropped_forced_{ 0 };// 하드 상한에 닿아 버린 것
        std::atomic<size_t>      queue_limit_{ kDefaultQueueLimit };
        std::atomic<DropLevel>   drop_level_{ DropLevel::kNone };  // 기본은 안 버린다
        std::atomic<LogLevel>    min_level_{ LogLevel::kInfo };    // 기본은 다 남긴다
        std::atomic<bool>        console_{ true };
        bool                     stop_ = false;
        bool                     running_ = false;
    };

    // printf 와 같은 모양으로 쓴다. 기존 호출부를 그대로 옮길 수 있어야
    //   「로그를 넣느라 코드가 바뀌는」 일이 안 생긴다.
    inline void logf(const char* fmt, ...) {
        char msg[1024];

        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);

        Log::get().emit(msg);
    }

    // logs\ 폴더를 만들고 그 안에 연다. 이미 있으면 그냥 쓴다.
    inline void log_open_default() {
        CreateDirectoryA("logs", nullptr);
        Log::get().open("logs\\server.log");
    }

    inline void log_close() {
        Log::get().close();
    }

    // 크래시 핸들러가 부르는 고리.
    inline void log_flush() {
        Log::get().flush();
    }

    // 크래시 핸들러 전용. 시한 안에 못 밀어내면 false 를 주고 포기한다.
    //   평시 코드에서 부르지 말 것. 이건 「죽는 중」을 위한 것이다.
    inline bool log_flush_try(unsigned timeout_ms) {
        return Log::get().flush_try(timeout_ms);
    }

    // 콘솔 출력 on/off — 설정을 읽은 뒤 main 이 부른다.
    //   기동 로그는 콘솔에 보이고, 그 뒤로는 설정을 따른다.
    inline void log_set_console(bool on) {
        Log::get().set_console(on);
    }

    inline uint64_t log_dropped_total() {
        return Log::get().dropped_total();
    }

    inline uint64_t log_dropped_forced() {
        return Log::get().dropped_forced();
    }

    // 큐 상한과 드롭 정책 — 설정을 읽은 뒤 main 이 부른다.
    inline void log_set_policy(size_t queue_limit, DropLevel drop) {
        Log::get().set_policy(queue_limit, drop);
    }

    // 최소 레벨 — 이 아래는 아예 안 남긴다. (config [log] level)
    inline void log_set_level(LogLevel lv) {
        Log::get().set_min_level(lv);
    }

    // 프레임 추적 로그 — [RECV]·[SEND]·[DB  ]·[DBACK]. [DB  ] 의 공백 둘은 태그 폭을
    // 맞춘 것이고, scripts\ 가 이 문자열로 grep 하므로 한 칸만 쓰면 안 걸린다.
    //
    // 넷 다 프레임 하나·송신 하나마다 나간다. 3,000 프레임 회귀 한 번에 약 12,000줄이
    // 났다. 그런데 emit 은 큐 뮤텍스를 잡으므로, 동시성을 올릴수록 로그가 프레임 경로의
    // 직렬화 지점이 된다 — release_io 가 세션 맵 락으로 그랬던 것과 같은 모양이다.
    //
    // level 로는 못 끈다. 넷 다 INFO 급이라 warn 으로 올리면 기동·종료 로그와 통계까지
    // 사라진다. 「무엇을 남길 것인가」와 「얼마나 자세히 볼 것인가」는 다른 질문이다.
    //
    // 기본은 켬이다. 이 로그가 곧 I/O 워커와 로직 스레드가 다르다는 것과 DB 워커가
    // 게임 상태를 안 만진다는 것의 증거다 — tid 를 비교해서 본다. 대규모로 갈 때 먼저
    // 끄는 손잡이라는 뜻이지 평시에 꺼 두라는 뜻이 아니다.
    //
    // 검사는 호출부에서 한다. emit 안에서 걸러도 되지만 그러면 걸러질 줄의 인자 계산과
    // vsnprintf 비용은 이미 낸 뒤다. 프레임당 도는 자리라 그 비용이 곧 없애려는 것이다.
    inline std::atomic<bool>& log_trace_flag() {
        static std::atomic<bool> on{ true };
        return on;
    }
    inline bool log_trace_frames() {
        return log_trace_flag().load(std::memory_order_relaxed);
    }
    inline void log_set_trace_frames(bool on) {
        log_trace_flag().store(on, std::memory_order_relaxed);
    }

}   // namespace core
