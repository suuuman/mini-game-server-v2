//  net/s2s_connector.h — 마을이 세션 서버에 "붙으러 가는" 커넥터
//
//  IocpServer 와 반대 방향이다. IocpServer 는 accept 만 할 줄 알고, 이 클래스는
//  connect 만 할 줄 안다. 자체 IOCP · 전용 스레드 하나가 연결·재연결·요청 매칭을
//  전부 그 스레드 위에서 혼자 한다 — 존과 같은 "락 없는 소유" 모델이다(ADR-019 결정 3).
//  세션 수명(acquire/release/io_count)에는 닿지 않는다 — 이 커넥터가 다루는 연결은
//  하나뿐이고 그 연결은 이 클래스 하나가 통째로 소유한다.
//
//  proto 를 모른다. 프레이밍·분류·직렬화는 전부 콜백으로 주입받는다 —
//  world → proto → net → core 의존 방향에서 net 이 proto 를 알면 방향이 뒤집힌다.
//  iocp_server.h 도 include 하지 않는다. 이 커넥터는 IocpServer 와 무관하게 서는
//  설계고, 그 의도를 "같은 시그니처를 자체 alias 로 재선언한다"는 형태로 지킨다.
#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace net {

    struct S2sConfig {
        std::string host;                  // 빈 값이면 start() 를 부르지 않는 것이 상위 계약이다
        uint16_t    port = 0;
        int backoff_initial_ms = 1000;
        int backoff_max_ms     = 30000;
        int request_timeout_ms = 10000;
    };

    // iocp_server.h::FrameSizer 와 시그니처가 같지만 별개 타입이다. include 를 안 하는
    // 것 자체가 "이 커넥터는 IocpServer 를 몰라도 선다"는 설계 의도라서, 같은 함수를
    // 다른 이름의 별개 alias 로 다시 선언한다.
    using S2sFrameSizer = std::function<int(const char* data, int len)>;

    using S2sHeaderEncode = std::function<void(char* dst, uint16_t msg_id, uint32_t seq, uint16_t body_size)>;

    // 1단계의 decode_header(const uint8_t*) 와 시그니처가 다르다 — 그쪽은 8바이트
    // 고정 헤더만 돌려주면 끝이지만, 여기서는 프레임 전체 안에서 헤더와 본문의
    // 경계까지 함께 잘라 내야 한다(커넥터는 프레임의 총 길이만 알지 그 안의 구조는
    // 모른다). 상위(s2s_link)가 1단계 함수들을 감싼 람다를 꽂는다.
    using S2sHeaderDecode = std::function<bool(const char* frame, int len,
        uint16_t& msg_id, uint32_t& seq, const char*& body, int& body_len)>;

    // 분류 결과 타입은 커넥터가 소유한다. proto 는 msg_id 열거만 알고, "그래서 이게
    // 응답인가 요청인가 모르는 것인가"의 최종 판정 타입은 프로토콜 무관이라 여기 둔다.
    enum class S2sFrameKind {
        kReply,
        kRequest,
        kUnknown,
    };

    using S2sClassify = std::function<S2sFrameKind(uint16_t msg_id)>;

    // 프레이밍에 관여하는 네 콜백을 한 벌로 묶는다 — 이 넷은 프로토콜이 바뀌면
    // 항상 같이 바뀐다. 연결 상태·요청 인입·tick 은 프로토콜과 무관하게 따로
    // 바뀔 수 있어 개별 setter 로 둔다(아래).
    struct S2sFrameCodec {
        S2sFrameSizer   frame_sizer;
        S2sHeaderEncode header_encode;
        S2sHeaderDecode header_decode;
        S2sClassify     classify;
    };

    using S2sOnConnected    = std::function<void()>;
    using S2sOnDisconnected = std::function<void()>;

    // 역방향 요청/알림 인입. 회신 여부·내용은 프로토콜 지식이라 상위가 정한다
    // (1단계는 전부 Unsupported 로 되돌린다 — respond() 를 그 자리에서 부른다).
    using S2sOnRequest = std::function<void(uint16_t msg_id, uint32_t seq, const char* body, int len)>;

    using S2sOnTick = std::function<void()>;

    // 완료 콜백 — 모든 request() 는 이걸 정확히 1회 받는다. 갈래는 넷뿐이다.
    //   kOk           정상 응답 (body/len 유효)
    //   kTimeout      request_timeout_ms 경과
    //   kDisconnected 응답 전에 연결이 끊겼다
    //   kStopped      stop() 시점에 아직 대기 중이었다
    // 뒤의 셋은 body == nullptr · len == 0 이다 — 호출자는 result 만 본다.
    enum class S2sResult {
        kOk,
        kTimeout,
        kDisconnected,
        kStopped,
    };

    using ResponseFn = std::function<void(S2sResult result, uint16_t msg_id, const char* body, int len)>;

    // 종료 시 한 줄 요약에 실리는 값 그대로다(§8-4 · D6 판정 근거).
    // stop() 이 join 을 마친 뒤에만 읽는다 — 그 전에는 S2S 스레드가 계속 갱신 중이라
    // 값이 흔들린다. join 이 happens-before 를 만들어 주므로 그 뒤로는 락 없이 안전하다.
    struct S2sStats {
        uint64_t connects      = 0;
        uint64_t reconnects    = 0;
        uint64_t sent          = 0;
        uint64_t recv          = 0;
        uint64_t timeouts      = 0;
        uint64_t failed        = 0;   // 끊김/stop 으로 실패 완료된 요청 수
        uint64_t unsupported_tx = 0;  // respond() 로 실제 전송된 횟수 — 이름과 달리 Unsupported
                                       // 전용이 아니다. Reserve→ReserveAck(3단계)도 respond() 를
                                       // 쓴다 — 필드명은 1단계(그때는 전부 Unsupported였다)의 흔적.
        uint64_t stray_seq     = 0;
        uint64_t unknown_msg   = 0;
    };

    // net 이 proto 를 모르므로 헤더 폭을 자체 상수로 못박는다(session.h::kRecvBufferSize
    // 가 proto::kMaxBodySize 를 모르는 것과 같은 이유). "둘 다 아는 층"인 app 이
    // static_assert 로 이 값과 proto::s2s::kHeaderSize 의 일치를 강제한다.
    constexpr int kS2sHeaderSize = 8;

    // 한 프레임 전체를 담을 수 있어야 한다 — 8(헤더) + 65535(본문 상한, proto::s2s::kMaxBodySize
    // 와 값으로만 맞춘다. include 하면 방향 위반이라 리터럴로 둔다).
    constexpr int kS2sRecvBufferSize = kS2sHeaderSize + 65535;

    class S2sConnector {
    public:
        S2sConnector();
        ~S2sConnector();

        // 복사·이동 금지. 스레드와 커널 핸들을 들고 있다.
        S2sConnector(const S2sConnector&) = delete;
        S2sConnector& operator=(const S2sConnector&) = delete;

        // start() 전에 꽂는다 — 전부 S2S 스레드에서만 불린다.
        void set_frame_codec(S2sFrameCodec codec) { codec_ = std::move(codec); }
        void set_on_connected(S2sOnConnected cb) { on_connected_ = std::move(cb); }
        void set_on_disconnected(S2sOnDisconnected cb) { on_disconnected_ = std::move(cb); }
        void set_on_request(S2sOnRequest cb) { on_request_ = std::move(cb); }
        void set_on_tick(S2sOnTick cb, int tick_interval_ms) {
            on_tick_ = std::move(cb);
            tick_interval_ms_ = tick_interval_ms;
        }

        // S2S 스레드를 기동한다. host 가 빈값이면 부르지 않는 것이 상위 계약이다
        // (S2sLink 가 [s2s] host 를 보고 거른다 — 여기서는 다시 검사하지 않는다).
        bool start(const S2sConfig& config);

        // 멱등 — IocpServer::stop 과 같은 running_.exchange(false) 가드. 명시 호출과
        // 소멸자 안전망이 겹쳐도 join 이 한 번만 돈다.
        // ① 명령 큐 닫기 ② PQCS 웨이크 ③ S2S 스레드가 대기 요청 전부 실패 완료 +
        // 소켓 close ④ join — 순서 그대로.
        void stop();

        // 셋 다 스레드 안전 — 명령 큐 + PQCS 웨이크로 S2S 스레드에 위임한다.
        // false = stop 이후(core::JobQueue::push 와 같은 bool 계약).
        bool request(std::vector<char> body, uint16_t msg_id, ResponseFn on_done);
        bool notify(std::vector<char> body, uint16_t msg_id);
        bool respond(uint32_t seq, uint16_t msg_id, std::vector<char> body);

        // 상위가 규약 위반이나 등록 거부를 판정했을 때 끊고 재연결을 스케줄한다.
        //   0    = 일반 백오프로 진행
        //   양수 = 다음 재시도 1회만 이 지연으로 덮어쓴다(그 뒤는 backoff_current 그대로
        //          정상 지수 진행 — 연결 성립 시 초기값으로 리셋되는 규칙도 그대로다)
        // S2S 스레드에서 불리면(1단계의 유일한 호출부 — 응답 콜백 안) 명령 큐를 거치지
        // 않고 그 자리에서 즉시 실행한다 — 인입 프레임 디스패치 루프를 그 자리에서
        // 중단시켜 같은 recv 배치의 후속 프레임이 처리되지 않게 하기 위해서다(위반
        // 감지 즉시 차단). 다른 스레드에서 불리면 명령 큐를 거친다.
        void force_disconnect(int reconnect_delay_ms = 0);

        // 스레드 계약 — stats_ 는 S2S 스레드만 쓴다. 이 호출은 S2S 스레드 위에서
        // (예: on_tick_ 콜백 안) 부르거나, stop() 이 join 을 마친 뒤에만 안전하다.
        // 그 밖의 시점에 다른 스레드에서 부르면 데이터 레이스다.
        S2sStats stats() const;

    private:
        enum class IoOp {
            kConnect,
            kRecv,
            kSend,
        };

        // OVERLAPPED 가 첫 멤버여야 한다 — 완료 시 커널이 주는 것은 OVERLAPPED* 뿐이고,
        // 첫 멤버면 그 주소가 곧 IoCtx 의 주소라 캐스팅 한 번으로 되돌릴 수 있다
        // (net::IoContext 와 같은 규칙 — session.h 를 include 하지 않으므로 따로 둔다).
        //
        // generation 은 "이 I/O 를 발행할 때의 세대"다. 연결마다 새 소켓을 만들지만
        // 이 구조체 자체는(connect_ctx_/recv_ctx_/send_ctx_ 각각 하나씩) 재사용한다.
        // closesocket 으로 끊은 뒤에도 그 소켓에 걸려 있던 I/O 의 완료는 IOCP 에
        // 나중에 도착하는데, generation 을 지금 값과 비교해 다르면 그 완료는 이미
        // 죽은 연결의 것이라 버린다(핸들 재사용 방지 — Step2 설계의 "세대 카운터").
        //
        // 재사용 시점(다음 begin_connect)이 옛 완료의 도착보다 먼저 오면 이 구조체를
        // 두 세대가 겹쳐 쓰는 것이라 generation 비교가 무의미해진다 — 그 안전은
        // "실측상 옛 완료가 항상 먼저 온다"가 아니라 outstanding_io_ 카운터가
        // 구조적으로 막는다(아래 outstanding_io_ 주석 참조).
        struct IoCtx {
            OVERLAPPED ov{};
            WSABUF     wsabuf{};
            IoOp       op = IoOp::kRecv;
            uint64_t   generation = 0;
        };

        // 매칭 테이블 엔트리 — S2S 스레드만 접근한다(락 없음).
        struct Pending {
            ResponseFn on_done;
            uint64_t   deadline_ms = 0;
        };

        enum class CommandKind {
            kRequest,
            kNotify,
            kRespond,
            kForceDisconnect,
        };

        // request/notify/respond/force_disconnect 넷을 한 모양으로 담는다 — 다른
        // 스레드는 이 모양으로만 S2S 스레드에 일을 맡긴다.
        struct Command {
            CommandKind kind = CommandKind::kRequest;
            std::vector<char> body;
            uint16_t msg_id = 0;
            uint32_t seq = 0;                   // kRespond 전용 — 응답 대상 seq
            ResponseFn on_done;                 // kRequest 전용
            int reconnect_override_ms = -1;     // kForceDisconnect 전용. -1 = 오버라이드 없음
        };

        void thread_main();
        void shutdown_cleanup();

        bool get_connect_ex();
        bool begin_connect();

        void handle_completion(BOOL ok, DWORD transferred, OVERLAPPED* ov);
        void on_connect_io_complete(BOOL ok);
        void on_recv_io_complete(BOOL ok, DWORD transferred);
        void on_send_io_complete(BOOL ok, DWORD transferred);

        void drain_recv_frames();
        void dispatch_frame(const char* frame, int len);
        void compact_recv_buffer();
        bool post_recv();

        bool post_send();
        void maybe_kick_send();
        void queue_send(const char* data, int len);
        void encode_and_queue(uint16_t msg_id, uint32_t seq, const std::vector<char>& body);

        // schedule_reconnect = false 는 stop 경로 전용이다 — 종료 중에는 재연결을
        // 스케줄하면 안 된다.
        void teardown_connection(S2sResult fail_reason, int reconnect_override_ms,
            bool schedule_reconnect = true);
        void do_force_disconnect(int reconnect_delay_ms);
        void schedule_reconnect_after_failure();
        void fail_all_pending(S2sResult reason);
        void sweep_timeouts(uint64_t now_ms);
        void maybe_reconnect(uint64_t now_ms);
        void maybe_tick(uint64_t now_ms);
        DWORD compute_wait_ms(uint64_t now_ms) const;

        void drain_commands();
        void handle_request_command(Command& cmd);
        void handle_notify_command(Command& cmd);
        void handle_respond_command(Command& cmd);
        bool submit(Command cmd);
        void wake();

        // 진짜 I/O 완료는 소켓과 연관 지을 때 넘긴 이 포인터가 키로 온다.
        // 웨이크는 kWakeKey — this 는 힙/스택 어디에 있든 1일 수 없으므로 항상 구분된다.
        static constexpr ULONG_PTR kWakeKey = 1;

        S2sConfig     config_;
        S2sFrameCodec codec_;
        S2sOnConnected    on_connected_;
        S2sOnDisconnected on_disconnected_;
        S2sOnRequest      on_request_;
        S2sOnTick         on_tick_;
        int tick_interval_ms_ = 0;

        HANDLE iocp_ = nullptr;
        std::thread thread_;
        std::atomic<bool> running_{ false };

        // ConnectEx 포인터 — 프로세스(정확히는 이 인스턴스) 당 1회 획득해 캐시한다.
        // 스파이크 SPIKE6 가 소켓 간 재사용을 실측으로 확인했다 — 재연결마다 다시
        // 물을 필요가 없다.
        LPFN_CONNECTEX connect_ex_ = nullptr;

        SOCKET sock_ = INVALID_SOCKET;
        bool   connected_ = false;
        bool   connecting_ = false;

        IoCtx connect_ctx_;
        IoCtx recv_ctx_;
        IoCtx send_ctx_;

        // 자체 보유 누적 수신 버퍼 — Session::RecvBuffer 를 쓰지 않는다. 그건 4100B
        // 고정이라 S2S 본문 상한(65,535B)을 못 담는다. 구조(두 커서로 처리분/수신분을
        // 가르는 것)는 참고했지만 크기 때문에 재사용이 안 된다.
        std::vector<char> recv_buf_;
        int recv_read_pos_ = 0;
        int recv_write_pos_ = 0;

        // 송신 이중 버퍼 — session.h::SendBuffer 와 같은 이유(같은 소켓에 WSASend 를
        // 두 개 걸면 순서가 안 보장된다)지만 뮤텍스가 없다. IocpServer 는 여러 I/O
        // 워커가 동시에 send_chunks 를 부를 수 있어 락이 필요했지만, 여기는 제출이
        // 전부 명령 큐를 거쳐 S2S 스레드 하나로 좁혀지므로 이 버퍼도 그 스레드
        // 혼자만 만진다 — 진짜 경쟁이 없는 곳에 락을 넣지 않는다.
        std::vector<char> send_front_;
        std::vector<char> send_back_;
        int  send_sent_ = 0;
        bool send_pending_ = false;

        // 연결마다 증가 — 재진입 멱등 가드(force_disconnect 가 진입 즉시 올린다)이자
        // 낡은 소켓의 완료를 걸러내는 태그(IoCtx::generation 과 비교)다.
        uint64_t generation_ = 0;

        // 지금 발행돼 있어 아직 완료를 못 받은 I/O 수(ConnectEx/WSARecv/WSASend
        // 전부 합쳐서). 발행 직전에 올리고(post_recv 와 같은 순서), 발행이 그
        // 자리에서 실패하면 되돌리고, 완료가 오면 handle_completion 진입 즉시
        // (세대 검사보다 먼저) 내린다.
        //
        // 이 값이 0 이라는 것은 "옛 소켓에 걸려 있던 I/O 의 완료가 전부 IOCP 에서
        // 소비됐다"는 뜻이다 — maybe_reconnect 가 이 값이 0 일 때만 begin_connect
        // 를 불러 connect_ctx_/recv_ctx_/send_ctx_ 를 재사용하므로, 옛 완료가
        // 아직 안 왔는데 같은 구조체를 새 세대가 덮어쓰는 경합이 "실측상 안 겹친다"
        // 가 아니라 이 카운터로 구조적으로 막힌다. S2S 스레드 혼자만 만지므로
        // atomic 이 아니다.
        int outstanding_io_ = 0;

        // 연결마다 1부터. 0 은 notify() 전용 예약값이라 순환 시 건너뛴다.
        uint32_t next_seq_ = 1;

        std::unordered_map<uint32_t, Pending> pending_;

        int backoff_current_ms_ = 0;
        int reconnect_override_ms_ = -1;      // -1 = 오버라이드 없음. 1회 쓰고 지운다
        uint64_t next_action_deadline_ms_ = 0; // 연결 안 된 상태일 때 다음 재시도 예정 시각
        uint64_t next_tick_ms_ = 0;

        std::mutex commands_mutex_;
        std::vector<Command> commands_;
        bool commands_closed_ = false;         // stop() 이후 — 새 제출을 거절한다

        S2sStats stats_;
    };

}   // namespace net
