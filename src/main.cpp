//  main.cpp — 서버를 조립해서 띄운다.
//
//  여기 남은 것은 「순서가 곧 계약」인 코드뿐이다.
//      선언 순서 (= 소멸 역순) · 기동 순서 · 종료 순서 — 셋이 한 화면에 들어와야
//      「하나도 못 뒤집는다」가 눈에 보인다.
//
//    · 「받은 바이트를 무엇으로 볼 것인가」  → app/frame_router.cpp
//    · 인자 파싱 · 설정 · 배선 · 종료 통계  → app/bootstrap.cpp
//
//  한때 이 파일이 셋을 다 지고 있었다. 그 셋은 바뀌는 이유가 서로 달라서,
//    한 파일에 두면 「무엇 때문에 이 파일이 커졌는가」를 알 수 없었다.
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdio>       // std::getchar
#include <chrono>
#include <thread>

#include "app/bootstrap.h"
#include "app/entry_table.h"
#include "app/s2s_link.h"
#include "app/worker_pool.h"
#include "net/iocp_server.h"
#include "core/config.h"
#include "core/log.h"
#include "core/alloc_counter.h"
#include "db/db_worker.h"
#include "bench/bench.h"
#include "ops/crash_dump.h"

int main(int argc, char** argv) {
    // 소스는 /utf-8 로 컴파일되는데 콘솔 기본 코드페이지는 CP949 다.
    // 맞춰 주지 않으면 로그의 비ASCII 문자가 ?? 로 깨진다.
    // 과제 제출물에서 로그가 깨져 보이는 건 그 자체로 감점 요인이다.
    SetConsoleOutputCP(CP_UTF8);

    // 로그를 파일에도 남긴다 (logs\server.log).
    //   콘솔만 있으면 서버를 백그라운드로 띄웠을 때 아무것도 못 본다.
    //   이 시점에는 아직 설정을 안 읽었다 — 기동 로그는 항상 콘솔에도 나간다.
    core::log_open_default();

    const app::CliArgs args = app::parse_args(argc, argv);

    // --bench 면 서버를 띄우지 않고 측정만 하고 끝낸다.
    //   소켓도 스레드도 안 만드는 게 중요하다 — 측정 중에 딴 게 돌면
    //   그것까지 같이 재게 된다.
    if (args.bench) {
        bench::run_all();
        core::log_close();
        return 0;
    }

    //  winsock 초기화 — 프로세스당 한 번.
    //  2.2 를 요청하는 이유: Overlapped I/O 가 2.x 부터다.
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        core::logf("[FAIL] WSAStartup failed\n");
        return 1;
    }

    {
        // 설정을 먼저 읽는다. 못 읽어도 기본값으로 뜬다 —
        //   설정 파일이 없다고 서버가 안 뜨면, 배포에서 파일 하나 빠뜨린 게
        //   장애가 된다. 「없으면 안전한 기본값」이 맞다.
        core::Config cfg;
        if (!cfg.load("config\\server.ini")) {
            core::logf("[WARN] config\\server.ini 를 못 읽었다 — 기본값으로 간다\n");
        }

        // 로그 정책은 설정을 읽은 「뒤에」 적용한다 —
        //   기동 로그(설정 읽기 실패 등)는 콘솔에 보여야 한다.
        app::apply_log_policy(cfg);

        const app::ServerSettings st = app::load_settings(cfg);

        //  여기부터 선언 순서가 소멸 순서를 정한다(지역변수는 역순 소멸) — 다만
        //  이번엔 그 규칙 하나로 전부를 설명할 수 없다. app::WorkerPool 이
        //  net::IocpServer& 를 생성자로 받으므로(session 홀드를 잡으려면 그
        //  API 가 있어야 한다) server 가 workers 보다 먼저 만들어져 있어야
        //  한다 — 그러면 소멸은 반대로 workers 가 server 보다 먼저 죽는다.
        //  옛 ZoneManager 는 이 참조를 안 들고 있어서(자기 완결적이었다)
        //  「drain_hook_ 이 부르는 것은 server 보다 먼저 선언한다」는 규칙이
        //  그대로 성립했지만, workers 는 그 규칙과 정면으로 충돌한다 —
        //  drain_hook_ 도 workers.stop() 을 부르고, workers 의 생성자도
        //  server 를 요구하기 때문이다. 이 순환은 선언 순서만으로는 못 푼다.
        //
        //  실제 안전의 근거는 선언 순서가 아니라 아래 종료 절차의 실행 순서다 —
        //  server.stop() 을 명시로 부르면 그 함수 「안에서」 drain_hook_ 이 돌며
        //  workers.stop() 까지 이미 끝난 채로 돌아온다(WorkerPool::stop() 은
        //  스레드를 전부 join 하고 나서야 반환한다). 그 시점엔 server 도 workers 도
        //  둘 다 아직 살아 있는 지역 변수다 — 소멸자가 아니라 이 명시 호출이
        //  workers 를 참조 안전한 채로 멈춰 세운다. s2s_link 를 server 보다
        //  먼저 멈추는 것과 같은 종류의 안전장치다(그것도 선언 순서가 아니라
        //  명시 호출로 순서를 만든다). WorkerPool::stop() 이 멱등(threads_.empty()
        //  로 가드)이라, 혹시 예외로 이 블록을 벗어나 소멸자만 도는 경로가
        //  와도 두 번째 stop() 은 no-op 이라 안전하다.
        //
        //  db_service(DbPool 소유)는 이제 drain_hook_ 밖이다 — DB 를 부르는
        //  스레드가 workers 자신이라, workers.stop() 이 끝난 뒤에야 db_service
        //  를 닫아도 안전하다(그 전에 닫으면 아직 도는 워커가 빈 Lease 를 받는다).
        //  그래서 db_service.stop() 은 아래에서 server.stop() 「다음 줄」에
        //  명시로 부른다. entry_table·s2s_link·entry_sweeper 를 server 보다
        //  먼저 선언하는 이유는 그대로다 — on_session_gone 이 그 셋을 쓰고,
        //  그 함수는 server 의 드레인 중에도(sweep·I/O 워커가 close_session 을
        //  부르는 동안) 불릴 수 있다.
        db::DbWorkerPool db_service(st.db, st.db_pool_size);

        const app::HiResTimer hires_guard(st.hires_timer);

        // 크래시 핸들러를 「설정을 읽은 직후, 아무것도 띄우기 전에」 건다.
        //   소켓·스레드·DB 를 띄우는 도중에 죽는 것이 가장 진단이 어려운 순간인데,
        //   나중에 걸면 그 구간이 통째로 안 잡힌다.
        //   로그보다는 뒤다 — 핸들러가 실패를 로그로 알려야 하기 때문이다.
        //   실패해도 계속 간다. 덤프는 진단 수단이지 서비스 조건이 아니다.
        ops::install_crash_handler(st.crash);

        // 입장 플레이어 집합과 예약 테이블 — server 보다 먼저 선언하는 이유는
        // 위 선언 순서 주석 참조.
        app::EntryTable entry_table;

        // s2s_link·entry_sweeper 를 server 보다 먼저 선언하는 이유도 위 참조.
        app::S2sLink s2s_link;
        app::EntrySweeper entry_sweeper;

        // 틱 전용 스레드(D5) — 세션도 DB 도 안 만지므로 server 와의 순서 제약이
        // 없다. drain_hook_ 이 workers 뒤에 이어 멈추는 자리로 쓸 뿐이다.
        app::TickThread tick(st.tick_hz);
        core::logf("[INFO] tick: hz=%d (%s) hires_timer=%d\n",
            st.tick_hz, st.tick_hz > 0 ? "on" : "off", st.hires_timer ? 1 : 0);

        net::IocpServer server(st.frame_pool_capacity);

        // workers 는 server 뒤에 선언한다 — 생성자가 net::IocpServer& 를
        // 요구한다(위 블록 주석 참조).
        app::WorkerPool workers(st.workers, server);

        app::wire_server(server, workers, tick, db_service, entry_table, s2s_link);

        // server.start() 와는 순서 제약이 없다 — 이 스레드는 자기 스레드를
        //   스스로 띄우므로 server 의 idle 스윕과 달리 「먼저 꽂아야 뜨는」
        //   관계가 아니다. entry_table 을 쓰는 조립을 한자리(wire_server 옆)에
        //   모아 두려고 여기 둔다.
        entry_sweeper.start(entry_table, st.entry.sweep_ms);

        // start() 「전」이어야 한다 — 스윕 스레드가 start() 에서 뜬다.
        //   wire_server 옆에 두는 이유도 같다: 둘 다 「기동 전에 꽂아 두는 것」이다.
        server.set_idle_policy(st.idle_timeout_sec, st.sweep_interval_sec);

        // 이것도 start() 「전」이어야 한다 — set_idle_policy 와 같은 규율이다.
        server.set_send_overflow_policy(st.send_overflow_limit);

        // 소비자를 먼저 띄운다 — 반대 순서면 첫 요청이 갈 데가 없다.
        db_service.start();
        workers.start();
        tick.start();

        if (!server.start(st.port, st.io_workers, st.max_connections)) {
            // 시작 실패 경로도 같은 순서를 지킨다. drain_hook 은 아직 안 걸렸으니
            //   여기서 직접 부른다. 순서를 두 군데 적어야 하는 건 위험 신호지만,
            //   시작 실패는 아직 아무 일도 안 벌어진 상태라 실제 위험은 낮다.
            //   s2s_link 는 여기서 아직 시작 전이라(아래) 멈출 것이 없다.
            entry_sweeper.stop();
            workers.stop();
            tick.stop();
            db_service.stop();
            WSACleanup();
            return 1;
        }

        // server 가 다 뜬 뒤 마지막으로 s2s_link 를 기동한다 — 세션 서버가 아직
        // 없거나 host 가 비어 있으면 내부에서 조용히 비활성으로 남는다. 실패해도
        // 서버 기동 자체를 막지 않는다(db_service.start() 와 같은 태도 — 이 연결은
        // 부가 기능이지 조건이 아니다). port·max_connections 는 이미 load_settings
        // 가 [server] 절에서 읽어 둔 st 값 그대로 싣는다(⛔ [io] 가 아니다).
        const app::S2sSettings s2s_settings = app::load_s2s_settings(cfg);
        if (!s2s_link.start(s2s_settings, st.port, static_cast<uint32_t>(st.max_connections),
                server, entry_table, static_cast<uint32_t>(st.entry.reserve_expire_ms),
                st.entry.fullsync_chunk_max)) {
            core::logf("[WARN] s2s_link 기동 실패 — s2s 없이 계속한다\n");
        }

        // 기동에 든 할당은 빼고 「처리 중에 나는 할당」만 센다.
        //   섞으면 프레임당 몇 회인지를 계산할 수 없다.
        core::alloc_reset();

        // 서버가 「다 뜬 뒤에」 죽인다 — 스레드가 다 도는 상태의 덤프여야
        // 「어느 스레드가 죽었나」를 볼 수 있다.
        //   여기서 죽으면 아래 stop() · 소멸자들이 하나도 안 돈다. 그게 정상이다 —
        //     크래시는 정상 종료가 아니고, 덤프는 그 사실을 남기는 것이다.
        if (args.crash_mode == 1) { ops::crash_now(ops::CrashKind::kAccessViolation); }
        if (args.crash_mode == 2) { ops::crash_now(ops::CrashKind::kCppThrow); }
        if (args.crash_mode == 3) { ops::crash_now(ops::CrashKind::kTerminate); }

        if (args.auto_seconds > 0) {
            core::logf("[INFO] auto-stop in %d s\n", args.auto_seconds);
            std::this_thread::sleep_for(std::chrono::seconds(args.auto_seconds));
        }
        else {
            core::logf("[INFO] press Enter to stop\n");
            std::getchar();
        }

        // server.stop() 보다 먼저 멈춘다 — 명시 호출이어야 정지 시점이 이 줄
        // 하나로 보인다(소멸자에 맡기면 선언 위치에 묶여 추적할 수 없다).
        entry_sweeper.stop();

        // server.stop() 보다 먼저 불러야 하는 진짜 이유 — 세션 서버에 배정을
        //   먼저 끊는 것이다. 드레인이 도는 동안에도 세션 서버가 계속 이
        //   마을에 새 유저를 배정하면, 죽어 가는 마을로 유저가 들어온다.
        //   (응답 또는 자체 타임아웃을 기다린 뒤 돌아오므로, 이 호출 자체는
        //   순서와 무관하게 안전하다 — s2s_link.h 참조. 하지만 안전한 것과
        //   순서가 실제로 뜻이 있는 것은 다른 얘기다.)
        s2s_link.unregister_and_wait();

        // server.stop() 보다 먼저다. server.stop() 말미의 sessions_.clear() 는
        // io_count 무관 무조건 삭제라, 그 실행 중에 S2S 스레드가 close_by_id
        // 를 부르면 이미 지워진 세션을 만지는 창이 생긴다. S2sConnector::stop()
        // 이 그 스레드를 동기 join 하므로(반환 시점엔 이미 죽어 있다) 이 순서로
        // 두면 server.stop() 이 도는 동안 close_by_id 를 부를 스레드 자체가
        // 없다 — 「세션을 만지는 스레드가 이제 없다」(iocp_server.cpp 의
        // sessions_.clear() 옆 주석)가 close_by_id 신설 뒤에도 유지된다. 이
        // 순서가 뒤집히면 그 창이 도로 열린다.
        //
        // 명시 호출이 필요한 이유는 그대로다 — workers/tick 은 server 의
        // drain_hook_ 경유로 멈추지만 s2s_link 는 그 훅 사슬 밖이다(위 선언부
        // 주석 참조). 잃는 것은 [S2S  ] 종료 요약이 다른 요약들보다 먼저
        // 찍힌다는 것뿐이다(내용은 그대로다).
        s2s_link.stop();

        // drain_hook_ 안에서 workers.stop() → tick.stop() 이 이미 돈다 — 이
        // 줄이 반환하면 그 세션들을 만질 수 있는 스레드가 전부 없다.
        server.stop();

        // 그래서 db_service 를 여기서 닫는다 — 더 이상 DB 를 부를 스레드가
        // 없다는 것이 이 줄 위치의 근거다. drain_hook_ 안에 두지 않는 이유는
        // db_worker.h 헤더 주석 참조.
        db_service.stop();

        app::report_shutdown_stats(server);
    }

    WSACleanup();

    // 마지막에 닫는다. 위 블록의 소멸자들이 남기는 로그까지 파일에 들어가야 한다.
    core::log_close();
    return 0;
}
