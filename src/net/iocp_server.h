//  net/iocp_server.h — IOCP 수거 루프를 가진 서버
//
//  이 클래스가 하는 일은 세 가지뿐이다.
//    1. 접속을 받아 Session 을 만들고 IOCP 에 붙인다
//    2. WSARecv 를 걸어 두고, 완료를 워커 스레드에서 수거한다
//    3. 수거한 바이트 덩어리를 위층에 넘긴다
//
//  "무슨 메시지인가"는 이 클래스가 모른다. 그건 위층이 정한다.
#pragma once

#include "net/session.h"
#include "core/job_queue.h"
#include "core/buffer_pool.h"

#include <cstdint>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <atomic>
#include <thread>
#include <vector>
#include <unordered_map>

namespace net {
    // false = 이 세션은 끊어야 한다.
    // 핸들러 안에서 세션을 직접 지우면 안 된다 — drain_frames 가 아직 그 세션의
    //   버퍼를 들고 루프를 돌고 있다. 「죽여야 한다」는 알리고 죽이는 건 호출자가 한다.
    // 배정(placement) 값을 넘기지 않는다 — 이 프레임은 이제 그 세션 자신의
    //   직렬 큐(session.h) 안에서, 그 세션의 실행권을 쥔 워커가 도착 순서대로
    //   처리한다. 「어느 큐로 갈지」를 net 이 정할 필요가 없어졌으니 위층에
    //   실어 보낼 라우팅 값도 없다 — 존을 알아야 하면 session.zone 을 그
    //   실행 안에서 직접 읽는다(직렬 큐 직렬화가 그 읽기를 안전하게 만든다).
    using RecvHandler = std::function<bool(Session&, const char* frame, int len)>;

    // 위층이 제공한다. 버퍼 앞부분을 보고 「완성된 메시지 하나의 전체 길이」를 돌려준다.
    //     > 0 : 이만큼이 한 메시지다 (헤더 포함)
    //     = 0 : 아직 부족하다. 더 받아야 한다
    //     < 0 : 규약 위반이다. 끊어라
    // net 은 이 함수가 무엇을 보고 판단하는지 모른다 — 그래서 proto 를 몰라도 된다.
    using FrameSizer = std::function<int(const char* data, int len)>;

    // net 은 「이 세션의 실행 단위를 어떻게 스케줄할지」를 모른다. 넘기기만 한다 —
    // 위층(app::WorkerPool)이 세션의 직렬 큐에 쌓고 스케줄 여부를 판단한다.
    // false = 못 받았다 → 호출자가 홀드를 되돌린다.
    using JobSink = std::function<bool(Session&, core::Job)>;

    // 종료 순서의 가운데 칸을 world 가 채운다.
    // net 은 「언제」 멈춰야 하는지 알고, world 는 「무엇을」 멈춰야 하는지 안다.
    // 훅은 워커 join 직후 · 세션 삭제 직전에 불린다.
    using DrainHook = std::function<void()>;

    // 세션이 닫힐 때 위층에 알린다 — 「닫혔다」와 「지워졌다」 사이의 한 순간이다.
    // 위층은 이 세션을 자기 자료구조(존)에서 빼야 하는데 그건 다른 스레드의 일이라
    // 즉시 못 한다 — Job 으로 보내야 한다. 그동안의 홀드는 net 이 올려 준다.
    // false = 위층이 못 받았다 → net 이 홀드를 되돌린다
    using SessionGone = std::function<bool(Session&)>;

    // Job 하나가 프레임 버퍼 하나를 물고 큐에 들어간다.
    //   그래서 이 값이 곧 「큐에 동시에 들어갈 수 있는 Job 수」의 상한이다 —
    //   Job 큐에 따로 상한을 세는 코드를 안 짜도 된다.
    //   4100B × 4096 ≈ 16.8MB. 세션 1만 개의 수신 버퍼(41MB)에 비하면 작다.
    constexpr size_t kFramePoolCapacity = 4096;

    // 흩어져 있는 조각들을 「한 번의 락, 한 번의 발행 판단」으로 큐에 넣는다.
    struct SendChunk {
        const char* data;
        int         len;
    };

    // 유휴 세션 정리 — 말이 끊긴 연결을 언제 치울 것인가.
    //
    // TCP 는 선이 뽑힌 것을 알려주지 않는다. 정상 종료면 0바이트 완료로 알 수 있지만
    // 랜선이 뽑히거나 전원이 나가면 아무 일도 안 일어나고, 서버 입장에서는 조용한
    // 정상 유저와 구분이 안 된다. 조용함에 상한을 두는 것 말고 방법이 없다.
    //
    // 임계는 클라 주기의 3배가 권장값이다 — 3회 연속 놓쳐야 끊는다는 뜻이다. 1배면
    // 지연 한 번에 멀쩡한 유저가 끊기는데, 그건 좀비를 하나 더 두는 것보다 나쁘다.
    // 좀비의 비용은 세션 하나(약 4.2KB)뿐이고 대기 중인 Job 이 없어 프레임 풀도 안
    // 먹는다. 4096개가 전부 좀비여도 16.8MB 라 임계를 당겨 아끼는 것이 없다.
    //
    // 스윕 주기가 임계보다 훨씬 짧을 필요는 없다. 실제로 끊기는 시각은 임계와
    // 임계+주기 사이 어딘가인데, 90초에 10초 주기면 오차가 11% 고 이 판단에 영향이 없다.
    //
    // 위 권장값은 켤 때의 이야기고 기본값은 0(끄기)이다. scripts\ 하네스가 ping 을
    // 안 보내서, 켜 두면 회귀 여러 개가 「서버가 멀쩡한 접속을 끊는다」로 실패한다.
    constexpr int kDefaultIdleTimeoutSec  = 0;    // 0 = 끊지 않는다 ← 기본
    constexpr int kDefaultSweepIntervalSec = 10;

    // 송신 큐 넘침이 연속 몇 번째면 그 세션을 끊을 것인가(§17-6).
    //   0 이 기본이다(끄기) — kDefaultIdleTimeoutSec 의 "0=꺼짐" 관례와 같다.
    //   여기서는 관례를 넘어 안전과 직결된다: 이 킥은 send_chunks 가 호출자
    //   스택에서 close_session 을 동기로 부르고, close_session 은 session_gone_
    //   콜백까지 같은 스택에서 곧장 실행한다(iocp_server.cpp). 「어떤 락이든
    //   쥔 채 송신 금지」(마을 불변식 2)가 지켜지는 서버라면 안전하지만,
    //   락을 쥔 채 송신하는 것이 허용 패턴인 서버(세션 서버 —
    //   ARCHITECTURE.md §7 "세션 서버 판": Router::request_set_mode 류가
    //   Router::mutex_ 를 쥔 채 송신한다)에서 켜면, 그 송신이 넘쳐 킥이
    //   발화하는 순간 session_gone_ 이 같은 mutex_ 를 재획득하려 들어
    //   자기 데드락이 난다(비재진입 뮤텍스). 그래서 기본은 끄기고, 이
    //   전제가 실제로 성립하는 서버(마을)만 config 로 옵트인한다.
    constexpr int kDefaultSendOverflowLimit = 0;

    class IocpServer {
    public:
        // 프레임 풀 크기를 생성자로 받는다 — app::WorkerPool 의 워커 수와 같은 자리다.
        //   「기동 전에 정해지고 도는 중에는 안 바뀌는 값」이다.
        //   풀은 여기서 통째로 할당된다(4100B × capacity). start() 가 아니라
        //     생성자인 이유 — 이 크기로 못 잡으면 서버를 띄울 이유가 없다.
        explicit IocpServer(size_t frame_pool_capacity = kFramePoolCapacity);
		~IocpServer();

        // 복사·이동 금지. 스레드와 커널 핸들을 들고 있어서 복사되면 이중 해제가 난다.
		IocpServer(const IocpServer&) = delete;
		IocpServer& operator=(const IocpServer&) = delete;

		// max_connections = 0 은 「상한 없음」이다. io.worker_threads = 0 이
		//   「자동」인 것과 같은 규칙 — 0 은 값이 아니라 「정하지 않았다」를 뜻한다.
		bool start(unsigned short port, int worker_count, size_t max_connections = 0);
		void stop();

        void set_recv_handler(RecvHandler handler) { recv_handler_ = std::move(handler); }
        void set_frame_sizer(FrameSizer sizer) { frame_sizer_ = std::move(sizer); }
        void set_job_sink(JobSink sink) { job_sink_ = std::move(sink); }
        void set_drain_hook(DrainHook hook) { drain_hook_ = std::move(hook); }
        void set_session_gone(SessionGone cb) { session_gone_ = std::move(cb); }

        //  유휴 정리 정책. start() 「전에」 불러야 한다 — 스윕 스레드가 start()
        //  에서 뜨고, 그 뒤에 바꾸면 이미 뜬 스레드가 옛 값으로 돈다.
        //
        //  start() 의 인자로 안 넣은 이유 — start 는 이미 「듣기·워커」를 받는데
        //    여기에 정책 값 둘을 더 얹으면 인자 다섯 개가 전부 다른 종류가 된다.
        //    콜백들과 같은 「기동 전에 꽂아 두는 것」 자리에 두는 편이 읽힌다.
        //
        //  timeout_sec = 0 이면 스윕 스레드를 아예 안 만든다.
        //  「만들고 아무것도 안 하기」가 아니라 「안 만들기」다 — 꺼져 있을 때
        //    스레드 목록에 없어야 「이 기능은 지금 안 돈다」가 눈에 보인다.
        void set_idle_policy(int timeout_sec, int sweep_interval_sec) {
            idle_timeout_sec_ = timeout_sec;
            sweep_interval_sec_ =
                (sweep_interval_sec > 0) ? sweep_interval_sec : kDefaultSweepIntervalSec;
        }

        //  송신 큐 넘침 정책(§17-6). set_idle_policy 와 같은 규율이다 —
        //  start() 「전에」 불러야 한다. send_chunks 를 부르는 모든 워커가
        //  이 값을 참조하는데, 기동 뒤에 바꾸면 그 순간 이후 워커들만 새
        //  값을 본다는 보장이 없다 — 스레드 생성이 만드는 happens-before 에
        //  기대는 것은 기동 전 설정에서만 성립한다. atomic 이 아닌 이유도
        //  같다.
        void set_send_overflow_policy(int overflow_limit) {
            send_overflow_limit_ = overflow_limit;
        }

        // 위층이 세션을 자기 자료구조에 담을 때 부른다 — 담고 있는 동안 지워지면 안 되니까.
        //
        // 커널이 붙들든 · Job 이 붙들든 · 존이 붙들든 전부 「아직 지우면 안 된다」는
        // 같은 사실이라 같은 카운터로 센다.
        // release_session 을 부른 뒤에는 그 포인터를 만지면 안 된다.
        void acquire_session(Session& s) { s.io_count.fetch_add(1); }
        void release_session(Session* s) { release_io(s); }

        // 동기 Kick 이 id 로 세션을 찾아 끊는 자리 — 어느 스레드에서든 불러도
        //   된다(지금은 S2S 스레드 하나). 못 찾으면 false. sessions_ 가 Session*
        //   를 키로 두므로 id 조회는 선형이지만, 이 호출은 로그인 이벤트당
        //   최대 1회라 sweep_loop 와 같은 비용 계산이 선다.
        bool close_by_id(uint64_t session_id);

        // 이 세션으로 바이트를 보낸다. 큐에 넣고 바로 돌아온다 — 블록하지 않는다.
        // false = 이 세션은 끊어야 한다
        bool send_frame(Session& session, const char* data, int len);
        bool send_chunks(Session& session, const SendChunk* chunks, int count);

        // 종료 시 풀이 실제로 얼마나 쓰였나. failed 가 0 이 아니면 풀이 작다는 뜻이다.
        core::BufferPool::Stats frame_pool_stats() const { return frame_pool_.stats(); }

        // 상한에 걸려 돌려보낸 접속 수. 0 이 아니면 max_connections 가 작거나
        //   세션이 안 지워지고 있다는 뜻이다 — 둘은 다른 문제라 peak 과 같이 본다.
        uint64_t accept_rejected() const {
            return accept_rejected_.load(std::memory_order_relaxed);
        }
        size_t session_peak() const {
            return session_peak_.load(std::memory_order_relaxed);
        }

        // 유휴로 끊은 세션 수. 이 값만으로는 좋은지 나쁜지 알 수 없다 —
        //   진짜 끊긴 연결을 치운 것이 대부분이지만, 임계가 짧으면 멀쩡한 유저도
        //   여기 섞인다. 둘을 서버가 구분할 방법은 없다.
        //   그래서 「0인가 아닌가」가 아니라 「접속 수 대비 얼마인가」로 읽는다.
        //     평소 대비 갑자기 늘면 임계가 짧거나 클라 ping 이 안 나가는 것이다.
        uint64_t idle_kicked() const {
            return idle_kicked_.load(std::memory_order_relaxed);
        }

        // 송신 큐가 연속으로 넘쳐 끊긴 세션 수(§17-6). idle_kicked 와 같은
        //   읽는 법이다 — 이것만으로 좋고 나쁨을 못 가른다. 평소 대비 갑자기
        //   늘면 그 세션(들)이 정말 안 읽고 있었거나, overflow_limit 이 너무
        //   낮은 것이다.
        uint64_t send_full_kicked() const {
            return send_full_kicked_.load(std::memory_order_relaxed);
        }

    private:
        // 기동 도중 실패했을 때 잡아 둔 것을 되돌린다. 언제나 false 를 준다.
        //
        // start() 는 running_ 을 스레드를 띄우기 직전에야 세운다. 그 전에 실패하면
        // running_ 이 false 인 채로 돌아가는데, stop() 은 첫 줄에서 그 값을 보고 즉시
        // 빠져나가므로 완료 포트 핸들과 듣기 소켓이 안 닫힌 채 남았다. 프로세스가 곧
        // 끝나 실피해는 없었지만 「시작 실패 경로도 같은 순서를 지킨다」와 어긋났다.
        //
        // stop() 의 조기 탈출을 푸는 대신 여기서 되돌린다. 그걸 풀면 멈추는 절차와
        // 자원을 닫는 절차가 서로 다른 조건 위에 놓인다.
        bool start_rollback();

        void accept_loop();
        void worker_loop();
        void sweep_loop();
        bool post_recv(Session& session);
		void close_session(Session& session);
        Session* add_session(SOCKET sock, const sockaddr_in& peer);
        void     try_remove_session(Session* session);   // 실제 delete. 조건 확인은 호출자 책임
        void     release_io(Session* session);           // 홀드 반납 + 지워도 되면 지운다
        bool drain_frames(Session& session);    // 완성된 프레임을 다 꺼낸다. false = 끊어야 함
        bool post_send(Session& session);

        HANDLE iocp_ = nullptr;
        SOCKET listen_sock_ = INVALID_SOCKET;

        std::vector<std::thread> workers_;
        std::thread accept_thread_;

        // 왜 「전용 스레드 하나」인가 —
        //   ① 존 스레드에 얹으면: 존 0 을 맡은 존 스레드만 일이 늘고, 그 존 스레드가
        //      sessions_mutex_ 를 잡는다. world 가 net 의 락을 잡는 순간
        //      「존 락 → 세션 락」이라는 새 순서가 생기고, 반대 방향이 어딘가에
        //      이미 있으면 그게 데드락이다. 계층을 섞지 않는 편이 싸다.
        //   ② I/O 워커에 얹으면: GQCS 에 INFINITE 로 막혀 있어서 깨울 방법이
        //      「가짜 완료를 주기적으로 넣기」밖에 없다. 타이머 스레드가 또 필요해진다.
        //   10초에 한 번 깨서 4096개를 훑고(≈82μs) 다시 자는 스레드 하나가
        //     제일 단순하고 제일 싸다. 점유율로 0.001% 미만이다.
        std::thread sweep_thread_;

        std::atomic<bool> running_{ false };

        // 스윕을 「자는 중에」 깨우는 짝. stop() 이 최대 sweep_interval_sec_ 만큼
        // 기다리지 않게 하려는 것이다 — 10초짜리 종료 지연은 회귀 스크립트에서
        // 「멈추다 만 것」처럼 보인다.
        std::mutex              sweep_mutex_;
        std::condition_variable sweep_cv_;

        // atomic 이 아니다 — accept 스레드 하나만 만진다.
        // 경쟁이 없는 곳에 atomic 을 쓰면 비용만 내고 "여기 경쟁이 있다"는
        // 잘못된 신호를 코드에 남긴다.
        uint64_t next_session_id_ = 1;

        RecvHandler recv_handler_;
        FrameSizer  frame_sizer_;
        JobSink   job_sink_;
        DrainHook drain_hook_;
        SessionGone session_gone_;

        // 프레임 하나마다 여기서 빌리고, Job 이 끝나면 반납한다.
        // 프레임마다 vector 를 새로 만들면 그것만으로 프레임당 힙 할당이 하나 는다.
        core::BufferPool frame_pool_;

        // 키를 Session* 로 두는 이유 — 완료 통지가 주는 게 그 포인터라서,
        // id 로 찾으면 한 번 더 들고 다녀야 한다. 찾는 비용도 O(1) 이다.
        // 값이 unique_ptr 이므로 erase 하는 순간이 곧 delete 되는 순간이다.
        std::unordered_map<Session*, std::unique_ptr<Session>> sessions_;
        std::mutex                                            sessions_mutex_;

        // 상한 검사는 sessions_mutex_ 「안에서」 한다 — 밖에서 세고 안에서 넣으면
        //   그 사이에 다른 스레드가 넣을 수 있다. 지금은 accept 스레드가 하나뿐이라
        //   드러나지 않지만, 불변식을 스레드 수에 기대게 두지 않는다.
        size_t                max_connections_ = 0;    // 0 = 상한 없음
        std::atomic<uint64_t> accept_rejected_{ 0 };
        std::atomic<size_t>   session_peak_{ 0 };

        // atomic 이 아니다 — set_idle_policy 가 start() 전에 쓰고, 그 뒤로는
        // 스윕 스레드가 읽기만 한다. 스레드 생성이 happens-before 를 만들어 준다.
        int                   idle_timeout_sec_ = kDefaultIdleTimeoutSec;
        int                   sweep_interval_sec_ = kDefaultSweepIntervalSec;

        std::atomic<uint64_t> idle_kicked_{ 0 };

        // atomic 이 아니다 — set_send_overflow_policy 가 start() 전에 쓰고,
        // 그 뒤로는 send_chunks 를 부르는 워커들이 읽기만 한다(위 idle_timeout_sec_
        // 과 같은 이유).
        int                   send_overflow_limit_ = kDefaultSendOverflowLimit;
        std::atomic<uint64_t> send_full_kicked_{ 0 };
    };
}
