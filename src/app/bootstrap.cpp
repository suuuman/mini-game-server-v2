//  bootstrap.cpp — 조립의 재료를 만든다. 순서는 main() 이 정한다.
#include "app/bootstrap.h"
#include "app/frame_router.h"

#include <windows.h>
#include <timeapi.h>    // timeBeginPeriod. WIN32_LEAN_AND_MEAN 이라 따로 넣는다

#include <chrono>
#include <cstring>
#include <cstdlib>      // strtol — atoi 가 아니다 (parse_args 의 근거 참조)

#include "core/log.h"
#include "core/alloc_counter.h"
#include "world/trade.h"     // world::now_ms() — EntrySweeper 가 만료 판정 시각에 쓴다
#include "proto/s2s_packet.h"   // kFullSyncMaxPerChunk — fullsync_chunk_max 범위 상한

namespace {

    // I/O 워커 수 — 무엇을 하는 스레드인가로 정한다. CPU-bound 는 코어 수 근처가
    // 맞고 더 늘리면 컨텍스트 스위칭 비용만 늘지만, I/O-bound 는 대기 중에 CPU 를
    // 안 쓰므로 코어 수보다 많아도 된다. 이 워커는 GQCS 에서 대부분 자고 있다.
    //
    // IOCP 의 concurrency value 와는 다른 값이다. 그건 동시에 깨어 있을 수이고,
    // 스레드는 내가 만들어 넣는다. 블록되는 워커가 생기면 커널이 하나를 더 깨운다.
    constexpr int kIoWorkerCount = 4;

    // 직렬 큐 워커 수(app::WorkerPool) — 옛 존 스레드 수를 대신한다. 8 은 안 쟀다 —
    //   §6-2 예시값 그대로다. dbload·zone_block(재정의) 실측으로 확정할 몫이다.
    constexpr int kAppWorkerCount = 8;

    constexpr unsigned short kPort = 9000;

} // namespace

namespace app {

    //  명령줄 인자 — 읽기만 한다. 아무것도 시작하지 않는다.
    //
    //  --seconds N : 테스트 자동화용이다 — 백그라운드로 띄우면 Enter 를 넣어 줄
    //    방법이 없고, 강제 종료로 죽이면 stop() 이 안 돌아서 「종료 경로」와
    //    「종료 시 통계」가 통째로 검증에서 빠진다.
    //    정상 종료를 사람 손에만 의존하면 결국 안 재게 된다.
    //
    //  --crash* : 크래시 핸들러 검증용 — 일부러 죽인다.
    //    「기동 직후」가 아니라 「서버가 다 뜬 뒤」에 죽여야 의미가 있다 —
    //      스레드가 다 도는 상태의 덤프여야 「어느 스레드가 죽었나」를 볼 수 있다.
    //      그래서 여기서는 플래그만 읽고, 실제로 죽이는 건 main() 이 서버 기동 후에 한다.
    CliArgs parse_args(int argc, char** argv) {
        CliArgs a;

        // --bench 는 서버를 아예 안 띄운다.
        //   소켓도 스레드도 안 만드는 게 중요하다 — 측정 중에 딴 게 돌면
        //   그것까지 같이 재게 된다.
        if (argc > 1 && std::strcmp(argv[1], "--bench") == 0) {
            a.bench = true;
            return a;
        }

        // atoi 를 안 쓴다 — core/config.h 가 「atoi 는 실패를 0 으로 돌려준다」는
        //   이유로 버린 그 함수다. 여기서 0 은 「Enter 를 기다린다」라, 백그라운드로
        //   띄웠으면 getchar 가 EOF 로 즉시 돌아와 서버가 바로 꺼진다.
        //   `--seconds 6O`(영문자 O) 하나에 회귀가 통째로 헛돌고, 증상은
        //   「서버가 안 뜬다」로 보인다. 설정에서 고친 함정을 인자에서 다시 밟지 않는다.
        for (int i = 1; i + 1 < argc; ++i) {
            if (std::strcmp(argv[i], "--seconds") == 0) {
                const char* raw = argv[i + 1];
                char* end = nullptr;
                const long v = std::strtol(raw, &end, 10);
                if (end == raw || *end != '\0' || v <= 0 || v > 86'400) {
                    core::logf("[WARN] --seconds \"%s\" — 1..86400 의 숫자가 아니다."
                               " 자동 종료를 끄고 Enter 를 기다린다\n", raw);
                }
                else {
                    a.auto_seconds = static_cast<int>(v);
                }
            }
        }

        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--crash") == 0)           { a.crash_mode = 1; }
            if (std::strcmp(argv[i], "--crash-throw") == 0)     { a.crash_mode = 2; }
            if (std::strcmp(argv[i], "--crash-terminate") == 0) { a.crash_mode = 3; }
        }

        return a;
    }

    //  로그 정책 — 설정을 읽은 「뒤에」 적용한다.
    //  기동 로그(설정 읽기 실패 등)는 콘솔에 보여야 하므로 순서가 이렇다.
    void apply_log_policy(const core::Config& cfg) {
        // 최소 레벨 — 이 아래는 큐에 넣지도 않는다.
        //   drop_level 과 다른 것이다. 이건 「애초에 안 남긴다」이고,
        //     drop_level 은 「남기려 했는데 큐가 찼을 때 무엇을 버리나」다.
        core::log_set_level(core::log_level_from(cfg.get("log", "level", "info")));

        // 프레임 추적 로그([RECV]·[SEND]·[DB  ]·[DBACK])만 따로 끄는 손잡이.
        //   [DB  ] 는 공백이 둘이다 — 태그 폭을 나머지와 맞춘 것이다.
        //   level 로는 못 끈다 — 넷 다 INFO 급이라 level=warn 으로 올리면
        //   기동·종료 로그와 통계까지 통째로 사라진다. 굵기가 안 맞는다.
        //   이 넷은 프레임/송신마다 나가므로, 동시성을 올리면 로거의 큐 뮤텍스가
        //     프레임 경로의 직렬화 지점이 된다. 대규모로 갈 때 먼저 끄는 것이 이것이다.
        core::log_set_trace_frames(cfg.get_int("log", "trace_frames", 1, 0, 1) != 0);

        // 콘솔 출력을 끌 수 있다. 로깅 비용의 대부분이 콘솔이라, 켜 두면
        //   로거 스레드가 초당 약 2만 줄에서 막히고 그만큼 드롭이 는다.
        const bool log_console = (cfg.get_int("log", "console", 1, 0, 1) != 0);
        core::log_set_console(log_console);
        if (!log_console) {
            core::logf("[INFO] 콘솔 로그 끔 — logs\\server.log 만 남는다\n");
        }

        // 드롭 정책도 설정으로 뺀다 — 기본은 「아무것도 안 버린다」.
        //   로그 때문에 큐가 가득 찬다면 그건 로거가 아니라 설정의 문제다.
        //   조용히 버려서 덮으면 원인이 안 보인다 → 버릴지 말지는 운영이 정한다.
        const int drop_q = cfg.get_int("log", "queue_limit",
            static_cast<int>(core::Log::kDefaultQueueLimit), 1, 10'000'000);
        const core::DropLevel drop_lv =
            core::drop_level_from(cfg.get("log", "drop_level", "none"));
        core::log_set_policy(static_cast<size_t>(drop_q), drop_lv);
    }

    //  설정 → 값. 부작용이 없다 — 읽고 합치기만 한다.
    ServerSettings load_settings(const core::Config& cfg) {
        ServerSettings s;

        s.port = static_cast<unsigned short>(cfg.get_int("server", "port", kPort, 1, 65535));

        // 0 = 자동. 설정이 「자동」을 표현할 수 있어야 한다 —
        //   숫자를 강제하면 배포하는 사람이 그 장비의 코어 수를 알아야 한다.
        s.io_workers = cfg.get_int("io", "worker_threads", 0, 0, 1024);
        if (s.io_workers <= 0) {
            s.io_workers = kIoWorkerCount;
        }
        // 직렬 큐 워커 수(app::WorkerPool) — 옛 [world] zone_threads 를 대신한다.
        //   존 스레드 시절엔 배정이 zone_id % N 고정이라 이 값이 「배정」을 바꿨지만,
        //   지금은 실행권이 유동적으로 큐를 나눠 먹으므로 그 의미가 없다 —
        //   그냥 「동시에 몇 개의 직렬 큐를 드레인할 수 있는가」다.
        // 옛 절([world] zone_threads·room_lanes)을 쓴 ini 를 만나면 알려 준다 —
        //   room_lanes 가 zone_threads 로 바뀌었고, 지금은 zone_threads 자체가
        //   폐지됐다. get_int 는 「키가 없으면」 조용히 기본값을 돌려주므로(config.h),
        //   말하지 않으면 옛 값으로 튜닝해 쓰던 사람이 경고 없이 기본값으로 돌아간다.
        if (cfg.has("world", "zone_threads") || cfg.has("world", "room_lanes")) {
            core::logf("[WARN] config [world] zone_threads(옛 room_lanes) 는 폐지됐다 —"
                       " [app] workers 로 바꿔라. 지금 이 값은 무시된다\n");
        }
        s.workers = cfg.get_int("app", "workers", 0, 0, 1024);
        if (s.workers <= 0) {
            s.workers = kAppWorkerCount;
        }

        // 0 = 상한 없음.
        //   상한이 없어도 프레임 풀(kFramePoolCapacity)이 사실상의 역압이 된다.
        //     다만 그건 「프레임을 못 빌려 끊긴다」라 증상이 접속 거절과 다르다.
        s.max_connections =
            static_cast<size_t>(cfg.get_int("server", "max_connections", 0, 0, 1000000));

        // 「개수」다. 프레임 하나가 버퍼 하나를 물고 Job 으로 큐에 들어가므로,
        //   이 값이 곧 「큐에 동시에 들어갈 수 있는 Job 수」의 상한이기도 하다.
        //   버퍼 「크기」는 여기서 못 정한다 — kRecvBufferSize 는 kMaxBodySize 에서
        //     파생된 규약이고, 클라이언트와 합의된 값이라 컴파일 타임이어야 한다.
        s.frame_pool_capacity =
            static_cast<size_t>(cfg.get_int("net", "frame_pool_capacity", 0, 0, 65536));
        if (s.frame_pool_capacity == 0) {
            s.frame_pool_capacity = net::kFramePoolCapacity;
        }

        // 기본이 0(꺼짐)이다 — 「안 정했으면 안 끊는다」.
        //   max_connections 의 0 과 성격이 같다: 0 은 값이 아니라 「상한 없음」이다.
        //   켜는 쪽이 기본이면, ping 을 안 보내는 옛 클라이언트가 서버를
        //     올리는 것만으로 전부 끊긴다. 기능을 켜는 것은 명시적이어야 한다.
        //   음수를 막는 것은 여기가 아니라 범위 검사다 — 음수를 그냥 통과시키면
        //     limit_ms 계산에서 u64 로 넘어가며 「사실상 무한」이 되어, 켜 놓고도
        //     아무도 안 끊기는 상태가 조용히 만들어진다.
        //   상한 86400(하루) 은 「이 이상이면 껐다는 뜻이다」로 읽으라는 선이다.
        s.idle_timeout_sec = cfg.get_int("net", "idle_timeout_sec", 0, 0, 86'400);

        // 0/음수는 「안 정했다」로 보고 기본값으로 되돌린다.
        // 여기서 0 을 그대로 두면 스윕이 안 자고 도는 바쁜 대기가 된다.
        s.sweep_interval_sec = cfg.get_int("net", "sweep_interval_sec", 0, 0, 3'600);
        if (s.sweep_interval_sec <= 0) {
            s.sweep_interval_sec = net::kDefaultSweepIntervalSec;
        }

        // 임계가 스윕 주기보다 짧으면 「임계를 정한 의미」가 흐려진다 —
        //   실제로 끊기는 시각이 임계가 아니라 주기가 정하게 된다.
        //   끄지는 않는다. 설정한 사람의 의도일 수도 있어서, 말만 해 준다.
        if (s.idle_timeout_sec > 0 && s.idle_timeout_sec < s.sweep_interval_sec) {
            core::logf("[WARN] idle_timeout_sec(%d) < sweep_interval_sec(%d)"
                       " — 실제 끊는 시각은 주기가 정하게 된다\n",
                s.idle_timeout_sec, s.sweep_interval_sec);
        }

        // 범위 0~1024 — 0 은 idle_timeout_sec 과 같은 "꺼짐" 관례다(net 쪽
        //   기본값도 0 — iocp_server.h 의 kDefaultSendOverflowLimit 참조).
        //   이 값을 켜는 것 자체가 위험한 서버(락을 쥔 채 송신하는 것이
        //   허용 패턴인 세션 서버 — send_chunks 의 동기 close_session 이
        //   같은 락을 재획득해 자기 데드락을 낸다)가 있어서, 끄기가 안전한
        //   기본값이다. 마을은 그 위험 전제가 없어 기본 인자로 8 을 넘긴다
        //   (main.cpp 호출부 참조) — 여기 마을 Settings 의 기본값은
        //   bootstrap.h 에 그대로 8 로 남아 있다.
        s.send_overflow_limit =
            cfg.get_int("net", "send_overflow_limit", 8, 0, 1024);

        core::logf("[INFO] config: port=%u io_workers=%d workers=%d"
                   " max_conn=%zu frame_pool=%zu idle_timeout=%ds sweep=%ds"
                   " send_overflow_limit=%d\n",
            static_cast<unsigned>(s.port), s.io_workers, s.workers,
            s.max_connections, s.frame_pool_capacity,
            s.idle_timeout_sec, s.sweep_interval_sec, s.send_overflow_limit);

        s.db.host     = cfg.get("db", "host", "127.0.0.1");
        s.db.port     = static_cast<unsigned>(cfg.get_int("db", "port", 3306, 1, 65535));
        s.db.user     = cfg.get("db", "user", "");
        s.db.password = cfg.get("db", "password", "");
        s.db.database = cfg.get("db", "database", "");

        // 감사 로그 스키마. 비어 있으면 게임 DB 에 같이 쓴다 —
        //   「안 정했으면 나누지 않는다」가 안전한 쪽이다. 없는 스키마를 가리키면
        //   거래가 통째로 실패하는데, 그건 로그 하나 때문에 게임이 멈추는 것이다.
        //   값의 형태 검사는 DbConn::log_schema() 가 한다 — 스키마명이 질의 문자열에
        //     들어가는 유일한 자리라 검사도 거기 두는 게 맞다.
        s.db.log_database = cfg.get("db", "log_database", "");

        // 격리 수준 — 「코드가 아니라 배포가 정할 값」이다 (db_conn.h 의 근거 참조).
        //   read-uncommitted 만 거절한다. 이 서버에서는 이득이 0 이고
        //     인벤토리 조회가 「존재한 적 없는 값」을 보여주게 된다.
        //    조용히 기본값으로 넘기지 않고 왜 거절했는지 남긴다 —
        //       설정한 사람은 의도가 있었을 것이고, 그 의도가 틀렸다는 걸 알려야 한다.
        {
            const std::string want = cfg.get("db", "isolation", "repeatable-read");
            if (want == "read-committed")        { s.db.isolation = "READ COMMITTED"; }
            else if (want == "repeatable-read")  { s.db.isolation = "REPEATABLE READ"; }
            else if (want == "serializable")     { s.db.isolation = "SERIALIZABLE"; }
            else if (want == "read-uncommitted") {
                core::logf("[WARN] config [db] isolation = read-uncommitted 는 받지 않는다"
                           " — 인벤토리 조회가 롤백될 값을 보여준다. repeatable-read 로 간다\n");
            }
            else {
                core::logf("[WARN] config [db] isolation = \"%s\" — 모르는 값이다."
                           " repeatable-read 로 간다\n", want.c_str());
            }
        }
        // 옛 [db] workers/read_workers 는 폐지됐다 — DB 를 부르는 스레드가 이제
        //   app::WorkerPool 자신이라 「DB 전용 워커 수」라는 개념이 없다. 남기고
        //   조용히 무시하면 튜닝하던 사람이 값을 계속 올려도 아무 효과가 없는 걸
        //   모른다 — room_lanes 선례와 같은 이유로 말한다.
        if (cfg.has("db", "workers") || cfg.has("db", "read_workers")) {
            core::logf("[WARN] config [db] workers/read_workers 는 폐지됐다 —"
                       " DB 를 직접 부르는 것은 이제 [app] workers 자신이다."
                       " 지금 이 값은 무시된다\n");
        }

        // 연결 수는 그대로 설정으로 남는다 — try_acquire 가 빌리는 실제 연결
        //   수(db_pool.h 의 max_size)를 정할 뿐, 「몇 스레드가 DB 를 부르는가」와는
        //   더 이상 묶이지 않는다(그게 workers/read_workers 를 없앤 이유다).
        s.db_pool_size = cfg.get_int("db", "pool_size", 4, 1, 1024);

        // 틱 주기. 30Hz 인 근거는 「흔한 값」이고, 그 이상은 없다 —
        // 게임 디자인이 정할 값이지 서버가 정할 값이 아니다. 0 이면 틱을 끈다.
        s.tick_hz     = cfg.get_int("tick", "hz", 30, 0, 1000);
        s.hires_timer = (cfg.get_int("tick", "hires_timer", 1, 0, 1) != 0);


        s.crash.dump_dir     = cfg.get("ops", "dump_dir", "dumps");
        s.crash.full_memory  = (cfg.get_int("ops", "full_memory", 0, 0, 1) != 0);
        s.crash.log_flush_ms =
            static_cast<unsigned>(cfg.get_int("ops", "log_flush_ms", 200, 0, 60000));

        // 세 값 다 실측 근거가 없다(server.ini 주석 참조) — 범위만 상식선에서 막는다.
        s.entry.reserve_expire_ms =
            cfg.get_int("village_entry", "reserve_expire_ms", 10000, 1, 3'600'000);
        s.entry.sweep_ms = cfg.get_int("village_entry", "sweep_ms", 30000, 100, 3'600'000);
        s.entry.fullsync_chunk_max =
            cfg.get_int("village_entry", "fullsync_chunk_max", 0, 0,
                static_cast<int>(proto::s2s::kFullSyncMaxPerChunk));

        return s;
    }

    HiResTimer::HiResTimer(bool b) : on(b) {
        if (on) { timeBeginPeriod(1); }
    }

    HiResTimer::~HiResTimer() {
        if (on) { timeEndPeriod(1); }
    }

    void EntrySweeper::start(EntryTable& entry, int sweep_ms) {
        if (started_) {
            return;     // 이미 돈다 — stop() 의 멱등 가드와 짝을 맞춘다
        }
        entry_ = &entry;
        started_ = true;
        thread_ = std::thread([this, sweep_ms] { run(sweep_ms); });
    }

    void EntrySweeper::stop() {
        if (!started_) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_requested_ = true;
        }
        cv_.notify_one();
        if (thread_.joinable()) {
            thread_.join();
        }
        started_ = false;
    }

    void EntrySweeper::run(int sweep_ms) {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!cv_.wait_for(lock, std::chrono::milliseconds(sweep_ms),
                              [this] { return stop_requested_; })) {
            lock.unlock();
            const size_t removed = entry_->sweep_expired(world::now_ms());
            // 0 건은 안 찍는다 — 기본 주기(30초)로 계속 도는 스레드라 0 건이
            // 정상이고, 매번 찍으면 그 자체가 노이즈다. 회수 건수는 이 스레드가
            // 실제로 도는지를 보는 관측용이다(entry_table.h 주석 참조).
            if (removed > 0) {
                core::logf("[INFO] entry sweep: removed=%zu\n", removed);
            }
            lock.lock();
        }
    }

    //  콜백 배선 — net 이 되묻는 질문들에 답할 함수를 꽂는다.
    //
    //  이 함수는 프로토콜을 모른다. frame_router 가 노출한 세 함수를 꽂을 뿐이다.
    //    한때 여기(main.cpp)에 send_peer_ntf 호출이 섞여 있었는데,
    //    그러면 「조립하는 코드」가 「거래 규칙」을 알아야 한다.
    void wire_server(net::IocpServer& server, WorkerPool& workers, TickThread& tick,
        db::DbWorkerPool& db, EntryTable& entry, S2sLink& s2s) {
        server.set_frame_sizer(frame_size);

        server.set_recv_handler(
            [&server, &db, &entry](net::Session& s, const char* f, int n) {
                return on_frame(server, db.pool(), entry, s, f, n);
            });

        // 프레임을 이 세션의 직렬 큐에 넣는다 — 어느 워커가 언제 이걸 드레인할지는
        // net 이 모른다(WorkerPool::submit 이 스케줄 여부까지 알아서 정한다).
        server.set_job_sink(
            [&workers](net::Session& s, core::Job job) {
                return workers.submit(s, std::move(job));
            });

        server.set_session_gone(
            [&server, &workers, &entry](net::Session& s) -> bool {
                return on_session_gone(server, workers, entry, s);
            });

        // 알림 원자화 — entry.enter()/leave() 가 성공한 바로 그 임계구역
        //   안에서 이 콜백이 불린다. frame_router.cpp 가 직접 notify_player_enter/
        //   leave() 를 부르던 세 자리는 그래서 사라졌다 — "성공했다"와 "알린다"
        //   사이에 다른 실행이 끼어들 여지를 명부 자신의 락 안으로 옮겨 아예
        //   없앤 것이다.
        entry.set_notifier([&s2s](uint64_t player_id, bool entered) {
            if (entered) { s2s.notify_player_enter(player_id); }
            else { s2s.notify_player_leave(player_id); }
            });

        // db.stop() 은 여기 없다. DB 를 부르는 스레드가 이제 워커 자신이라(직렬 큐
        //   실행 안에서 동기 호출한다), workers.stop() 이 그 워커를 전부 join 한
        //   뒤에야 「DB 를 만질 스레드가 이제 없다」가 성립한다. 그래서 db_pool 을
        //   닫는 것은 main() 이 server.stop() 이 반환한 뒤에 명시로 한다 — 여기서
        //   같이 부르면 아직 도는 워커가 빈 Lease 를 받는 순서가 될 수 있다.
        //   틱 스레드는 세션도 DB 도 안 만지므로 순서 제약이 없다 — workers 뒤에
        //   두는 것은 종료 로그를 한 자리에 모으는 관례일 뿐이다.
        server.set_drain_hook([&workers, &tick] {
            workers.stop();
            tick.stop();
            });
    }

    //  종료 통계 — 회귀에서 눈으로 확인하는 값이 전부 여기서 나온다.
    void report_shutdown_stats(net::IocpServer& server) {
        // [TICK ]/[TICK2] 는 drain_hook_ 안 tick.stop() 이 이미 찍었다(더 앞선
        // 시점) — 그 합계로 나누면 「프레임 하나당 힙 할당 몇 회」가 나온다.
        if (core::alloc_counting()) {
            const core::AllocStats s = core::alloc_snapshot();
            core::logf("[ALLOC] allocs=%llu  frees=%llu  bytes=%llu\n",
                static_cast<unsigned long long>(s.allocs),
                static_cast<unsigned long long>(s.frees),
                static_cast<unsigned long long>(s.bytes));
        }

        // failed 가 0 이 아니면 풀이 작다는 뜻이다 (그만큼 세션을 끊었다).
        //   peak 은 「동시에 최대 몇 개까지 썼나」 — 풀 크기를 감이 아니라
        //   이 숫자로 정하기 위해 센다.
        const auto ps = server.frame_pool_stats();
        core::logf("[POOL ] acquired=%llu  failed=%llu  peak=%llu / %llu\n",
            static_cast<unsigned long long>(ps.acquired),
            static_cast<unsigned long long>(ps.failed),
            static_cast<unsigned long long>(ps.peak),
            static_cast<unsigned long long>(ps.capacity));

        // 세션 상한이 실제로 어디까지 갔나. rejected 가 0 이 아니면 상한이 작거나
        //   세션이 안 지워지고 있다 — peak 과 같이 봐야 어느 쪽인지 갈린다.
        core::logf("[CONN ] peak=%zu  rejected=%llu\n",
            server.session_peak(),
            static_cast<unsigned long long>(server.accept_rejected()));

        // 유휴로 끊은 수. 좋고 나쁨이 이 숫자 하나로는 안 갈린다 —
        //   진짜 끊긴 연결을 치운 것과 멀쩡한 유저를 잘못 끊은 것이 같이 세진다.
        //   서버는 그 둘을 구분할 방법이 없다(구분할 수 있으면 임계가 필요 없다).
        //   [CONN ] peak 과 같이 본다. peak 대비 비율이 갑자기 뛰면
        //     임계가 짧거나 클라이언트 ping 이 안 나가고 있는 것이다.
        // send_full_kicked 를 같은 줄에 얹는다 — 둘 다 "이 세션을 서버가
        //   먼저 끊었다"는 같은 종류의 사실이라 관측 지점을 나눌 이유가 없다.
        //   0 이 정상이다 — 넘치는 것 자체가 상대가 안 읽고 있다는 이상 신호다.
        core::logf("[NET  ] idle_kicked=%llu  send_full_kicked=%llu\n",
            static_cast<unsigned long long>(server.idle_kicked()),
            static_cast<unsigned long long>(server.send_full_kicked()));

        // 로그를 몇 줄 잃었나. 0이 아니면 그 자체가 신호다:
        //   로그가 너무 많거나, 출력(특히 콘솔)이 느리거나, 큐 상한이 작다.
        //   유실을 「조용히」 두면 나중에 로그를 믿고 진단하다 틀린다.
        const uint64_t log_lost   = core::log_dropped_total();
        const uint64_t log_forced = core::log_dropped_forced();
        if (log_lost > 0) {
            core::logf("[WARN ] 로그 누적 유실 %llu 줄 (정책에 따라 버림)\n",
                static_cast<unsigned long long>(log_lost));
        }
        // 이쪽이 훨씬 심각하다 — 「버리지 않기로 한 로그」까지 버린 것이다.
        //   0이 아니면 큐 상한·콘솔·로그량 중 하나가 잘못 잡혀 있다는 뜻이다.
        if (log_forced > 0) {
            core::logf("[ERROR] 로그 %llu 줄이 하드 상한에서 강제 유실됨 — 설정을 고칠 것\n"
                       "        (queue_limit 를 올리거나, 콘솔을 끄거나, 로그를 줄인다)\n",
                static_cast<unsigned long long>(log_forced));
        }
    }

} // namespace app
