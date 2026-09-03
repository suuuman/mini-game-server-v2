//  bootstrap.h — 「서버를 조립해서 띄운다」에서 순서와 무관한 부분.
//
//  main() 에 남는 것은 「순서가 곧 계약」인 코드뿐이어야 한다 —
//    선언 순서(= 소멸 역순) · 기동 순서 · 종료 순서. 그 셋이 한 화면에 들어와야
//    「하나도 못 뒤집는다」가 눈에 보인다.
//    인자 파싱 · 설정 읽기 · 콜백 배선 · 종료 통계는 그 순서와 무관하다.
//
//  여기 있는 함수는 전부 「순서를 몰라도 되는」 것들이다.
//    순서가 걸린 것을 이 파일로 옮기면 이 파일의 존재 이유가 없어진다.
#pragma once

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "core/config.h"
#include "net/iocp_server.h"
#include "app/worker_pool.h"
#include "db/db_worker.h"
#include "app/entry_table.h"
#include "app/s2s_link.h"
#include "ops/crash_dump.h"

namespace app {

    //  명령줄 인자 — 「무엇을 하고 끝낼 것인가」
    struct CliArgs {
        bool bench        = false;   // --bench : 서버를 안 띄우고 측정만 하고 끝낸다
        int  auto_seconds = 0;       // --seconds N : N초 뒤 스스로 멈춘다
        int  crash_mode   = 0;       // 0 없음 · 1 --crash · 2 --crash-throw · 3 --crash-terminate
    };

    CliArgs parse_args(int argc, char** argv);

    // [village_entry] 절 — 마을의 입장 예약·스윕·FullSync 청크 정책.
    //   셋 다 실측 근거가 없다(세션 서버 왕복도, 예약 회수량도, 510명 동시
    //   입장도 잰 적이 없다) — server.ini 주석에 그대로 적는다.
    struct VillageEntrySettings {
        int reserve_expire_ms = 10000;
        int sweep_ms          = 30000;
        int fullsync_chunk_max = 0;    // 0 = proto::s2s::kFullSyncMaxPerChunk 그대로
    };

    //  설정에서 뽑아낸 값 전부. 「어디서 왔는가」는 안 남긴다 —
    //  ini 에서 온 값과 컴파일 상수 기본값이 여기서 이미 하나로 합쳐져 있다.
    //  쓰는 쪽이 그 둘을 구분할 이유가 없다.
    struct ServerSettings {
        unsigned short port       = 9000;
        int            io_workers = 4;

        // 직렬 큐 실행권을 나눠 먹는 워커 수(app::WorkerPool) — 옛 zone_threads 를
        //   대신한다. 배정이 고정이 아니라 유동적이라(§6-2), 코어 수 근처가
        //   상한이라는 성격은 같지만 배정 방식은 다르다.
        int            workers = 8;

        // 0 = 상한 없음. 상한에 걸린 접속은 거절 수로 세고 종료 통계에 나온다.
        size_t         max_connections = 0;

        // 프레임 버퍼 「개수」다. 크기가 아니다 —
        //   크기는 kRecvBufferSize(4100B) 이고 그건 kMaxBodySize 에서 파생된 규약이라
        //   설정으로 못 뺀다. 여기서 정하는 건 「그 버퍼를 몇 개 미리 잡아둘 것인가」다.
        //   load_settings 가 0 을 기본값으로 바꿔 놓는다 — 여기 0 이 들어오면
        //     버퍼가 하나도 없는 풀이 되어 모든 프레임이 실패한다.
        size_t         frame_pool_capacity = 0;

        // 접속 헬스체크. 0 = 끊지 않는다 (기본).
        //   기본이 꺼짐인 이유는 scripts\ 하네스가 ping 을 안 보내기 때문이다.
        //     켜 두면 유휴가 임계를 넘기는 회귀가 전부 실패하고, 원인이
        //     「하네스가 낡았다」인데 「서버가 이상하다」로 읽힌다.
        int            idle_timeout_sec  = 0;
        int            sweep_interval_sec = 10;

        // 송신 큐가 연속 몇 번 넘치면 그 세션을 끊을 것인가(§17-6).
        //   net::kDefaultSendOverflowLimit(iocp_server.h)은 0(꺼짐)인데 여기
        //   마을 Settings 의 기본은 8 이다 — 그 킥이 위험한 이유(락을 쥔 채
        //   송신하는 서버에서 session_gone_ 이 같은 락을 재획득해 자기
        //   데드락)가 마을에는 없다(불변식 2 — 마을은 어떤 락이든 쥔 채
        //   송신하지 않는다). 그래서 마을만 이 값을 8 로 옵트인해 넘긴다
        //   (main.cpp 의 set_send_overflow_policy 호출부 참조).
        int            send_overflow_limit = 8;

        db::DbConfig db;
        int          db_pool_size      = 4;    // db_pool.h 의 max_size — try_acquire 가 빌리는 연결 수

        int  tick_hz      = 30;
        bool hires_timer  = true;

        ops::CrashConfig crash;

        VillageEntrySettings entry;
    };

    ServerSettings load_settings(const core::Config& cfg);

    // 로그 정책만 따로 — 설정을 읽은 「뒤에」, 나머지를 만들기 「전에」 적용해야 한다.
    void apply_log_policy(const core::Config& cfg);

    // Windows 기본 타이머 해상도는 15.6ms 다. 30Hz 로 재워도 실제로는 15.6ms 배수에서만
    // 깨어나 간격이 31 과 47 을 오간다. 평균은 33.3 으로 맞고 건너뛴 틱도 0 이라, 그
    // 둘만 보면 잘 돌고 있는 것처럼 읽힌다 — 평균은 밀렸나에, 지터는 고른가에 답한다.
    // timeBeginPeriod(1) 이 해상도를 1ms 로 낮추는데, 타이머 인터럽트가 잦아져 CPU 가
    // 깊은 절전으로 못 내려가는 대가가 있다.
    //
    // RAII 인 것은 start 실패 경로가 있어서다. 손으로 끄면 그 경로 하나가 빠진다.
    // 그래서 이 가드는 main() 스코프에 있어야 한다 — 함수로 빼면 그 함수가 끝나는
    // 순간 해상도가 돌아온다.
    struct HiResTimer {
        bool on;
        explicit HiResTimer(bool b);
        ~HiResTimer();
        HiResTimer(const HiResTimer&) = delete;
        HiResTimer& operator=(const HiResTimer&) = delete;
    };

    // 예약 만료를 주기적으로 회수하는 전용 스레드 하나.
    //   EntryTable 에 못 두는 이유 — 그 클래스는 순수 자료구조라고 헤더 주석이
    //   역할을 좁혀 놨다("락은 이 클래스 안에서만 잡고 안에서만 푼다"). main.cpp
    //   에 못 두는 이유 — 그 파일은 "순서가 곧 계약"인 코드만 남기기로 했다.
    //   S2sLink 와 같은 모양(명시 start/stop + 소멸자 안전망)을 쓴다.
    //
    // sweep_ms 를 그대로 sleep_for 하지 않고 조건 변수로 기다리는 이유 — 그러면
    // 스윕 주기가 길수록(기본 30초) stop() 이 그만큼 지연된다. 정지 플래그를
    // 조건 변수와 같이 두면 stop() 이 즉시 깨운다.
    class EntrySweeper {
    public:
        EntrySweeper() = default;
        ~EntrySweeper() { stop(); }

        EntrySweeper(const EntrySweeper&) = delete;
        EntrySweeper& operator=(const EntrySweeper&) = delete;

        void start(EntryTable& entry, int sweep_ms);

        // 멱등 — 시작하지 않았으면 아무 일도 안 한다. server.stop() 보다 먼저
        // 명시 호출한다(main.cpp 참조) — 소멸자에 맡기면 정지 시점이 선언
        // 위치에 묶여 추적할 수 없다.
        void stop();

    private:
        void run(int sweep_ms);

        EntryTable* entry_ = nullptr;
        std::thread thread_;
        std::mutex mutex_;
        std::condition_variable cv_;
        bool stop_requested_ = false;
        bool started_ = false;
    };

    // net 이 되묻는 질문들에 답할 함수를 꽂는다. 순서와 무관하다 —
    // 꽂기만 하고 아무것도 시작하지 않는다.
    //   entry 는 recv_handler 람다가 on_frame 에 그대로 넘긴다 — kEnterReq 가
    //   예약을 소비하는 데 쓴다. s2s 는 같은 람다가 PlayerEnter/PlayerLeave 를
    //   보내는 데 쓴다(on_frame·on_session_gone 이 그 알림을 낸다).
    //   workers 가 job_sink(직렬 큐 제출)와 session_gone(정리 제출) 둘 다를 받는다 —
    //   존 스레드가 하던 라우팅을 이제 이 하나가 대신한다. tick 은 drain_hook_ 이
    //   workers 뒤에 이어 멈추는 자리를 갖는다(D5 — 세션·DB 어느 쪽도 안 만지므로
    //   순서 제약은 없지만, 종료 로그 순서를 이 하나에 모아 두면 보기 쉽다).
    void wire_server(net::IocpServer& server, WorkerPool& workers, TickThread& tick,
        db::DbWorkerPool& db, EntryTable& entry, S2sLink& s2s);

    // 정상 종료 뒤 통계. 회귀에서 눈으로 확인하는 값들이 전부 여기서 나온다.
    void report_shutdown_stats(net::IocpServer& server);

} // namespace app
