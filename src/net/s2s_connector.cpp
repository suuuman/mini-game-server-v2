#include "net/s2s_connector.h"

#include "core/log.h"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace net {

    S2sConnector::S2sConnector() {
        recv_buf_.resize(static_cast<size_t>(kS2sRecvBufferSize));
    }

    S2sConnector::~S2sConnector() {
        stop();
    }

    bool S2sConnector::start(const S2sConfig& config) {
        config_ = config;
        backoff_current_ms_ = config_.backoff_initial_ms;

        // 소켓 API 가 아니므로 WSAGetLastError 가 아니라 GetLastError 다
        // (iocp_server.cpp 의 같은 호출과 같은 이유).
        iocp_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
        if (iocp_ == nullptr) {
            core::logf("[FAIL] s2s CreateIoCompletionPort (err=%lu)\n", GetLastError());
            return false;
        }

        if (on_tick_ && tick_interval_ms_ > 0) {
            next_tick_ms_ = GetTickCount64() + static_cast<uint64_t>(tick_interval_ms_);
        }

        running_ = true;
        thread_ = std::thread(&S2sConnector::thread_main, this);
        return true;
    }

    void S2sConnector::stop() {
        if (!running_.exchange(false)) {
            return;     // 이미 멈췄다 — 소멸자와 명시적 stop 이 겹쳐도 한 번만 돈다
        }

        {
            std::lock_guard<std::mutex> lock(commands_mutex_);
            commands_closed_ = true;
        }
        if (iocp_ != nullptr) {
            PostQueuedCompletionStatus(iocp_, 0, kWakeKey, nullptr);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        if (iocp_ != nullptr) {
            CloseHandle(iocp_);
            iocp_ = nullptr;
        }
    }

    // ── 외부 제출 API — 전부 명령 큐를 거친다 ──────────────────────────

    bool S2sConnector::submit(Command cmd) {
        {
            std::lock_guard<std::mutex> lock(commands_mutex_);
            if (commands_closed_) {
                return false;       // stop() 이후 — JobQueue::push 와 같은 bool 계약
            }
            commands_.push_back(std::move(cmd));
        }
        wake();
        return true;
    }

    void S2sConnector::wake() {
        PostQueuedCompletionStatus(iocp_, 0, kWakeKey, nullptr);
    }

    bool S2sConnector::request(std::vector<char> body, uint16_t msg_id, ResponseFn on_done) {
        Command cmd;
        cmd.kind = CommandKind::kRequest;
        cmd.body = std::move(body);
        cmd.msg_id = msg_id;
        cmd.on_done = std::move(on_done);
        return submit(std::move(cmd));
    }

    bool S2sConnector::notify(std::vector<char> body, uint16_t msg_id) {
        Command cmd;
        cmd.kind = CommandKind::kNotify;
        cmd.body = std::move(body);
        cmd.msg_id = msg_id;
        return submit(std::move(cmd));
    }

    bool S2sConnector::respond(uint32_t seq, uint16_t msg_id, std::vector<char> body) {
        Command cmd;
        cmd.kind = CommandKind::kRespond;
        cmd.body = std::move(body);
        cmd.msg_id = msg_id;
        cmd.seq = seq;
        return submit(std::move(cmd));
    }

    void S2sConnector::force_disconnect(int reconnect_delay_ms) {
        // S2S 스레드 위에서 불렸으면(1단계의 유일한 호출부 — 응답 콜백 안) 명령
        // 큐를 거치지 않고 그 자리에서 즉시 처리한다 — 인입 프레임 디스패치 루프를
        // 그 자리에서 중단시켜야 같은 recv 배치의 후속 프레임이 안 돈다.
        if (std::this_thread::get_id() == thread_.get_id()) {
            do_force_disconnect(reconnect_delay_ms);
            return;
        }
        Command cmd;
        cmd.kind = CommandKind::kForceDisconnect;
        cmd.reconnect_override_ms = reconnect_delay_ms;
        submit(std::move(cmd));    // 실패(stop 이후)해도 무시 — 멈춘 커넥터에 끊으라는 요청은 의미가 없다
    }

    S2sStats S2sConnector::stats() const {
        return stats_;
    }

    // ── S2S 스레드 본체 ─────────────────────────────────────────────

    void S2sConnector::thread_main() {
        if (on_tick_ && tick_interval_ms_ > 0) {
            next_tick_ms_ = GetTickCount64() + static_cast<uint64_t>(tick_interval_ms_);
        }

        begin_connect();

        while (running_.load(std::memory_order_relaxed)) {
            const uint64_t now = GetTickCount64();
            const DWORD wait_ms = compute_wait_ms(now);

            DWORD transferred = 0;
            ULONG_PTR key = 0;
            OVERLAPPED* ov = nullptr;
            const BOOL ok = GetQueuedCompletionStatus(iocp_, &transferred, &key, &ov, wait_ms);

            if (ov != nullptr) {
                handle_completion(ok, transferred, ov);
            }
            // 웨이크든 완료든 타임아웃이든 — 매 회전 끝에 항상 명령 큐를 드레인한다.
            drain_commands();

            if (!running_.load(std::memory_order_relaxed)) {
                break;      // stop() 이 그사이 신호를 보냈다 — 재연결 시도 없이 바로 정리한다
            }

            const uint64_t now2 = GetTickCount64();
            sweep_timeouts(now2);
            maybe_reconnect(now2);
            maybe_tick(now2);
        }

        shutdown_cleanup();
    }

    DWORD S2sConnector::compute_wait_ms(uint64_t now_ms) const {
        uint64_t deadline = UINT64_MAX;

        // outstanding_io_ > 0 이면 재연결 마감을 아예 안 본다 — maybe_reconnect
        // 가 어차피 그 값이 0이 아닌 한 begin_connect 를 안 부르므로(outstanding_io_
        // 주석 참조), 이 마감으로 짧은 wait_ms 를 돌려줘 봤자 매 회전 헛돈다.
        // 기다리는 완료의 "도착" 자체가 GQCS 를 깨우므로 그걸 기다리면 된다.
        if (!connected_ && !connecting_ && outstanding_io_ == 0) {
            deadline = (std::min)(deadline, next_action_deadline_ms_);
        }
        if (on_tick_ && tick_interval_ms_ > 0) {
            deadline = (std::min)(deadline, next_tick_ms_);
        }
        for (const auto& entry : pending_) {
            deadline = (std::min)(deadline, entry.second.deadline_ms);
        }

        if (deadline == UINT64_MAX) {
            return INFINITE;    // 기다릴 마감이 없다 — 다음 웨이크나 완료가 깨운다
        }
        if (deadline <= now_ms) {
            return 0;
        }
        return static_cast<DWORD>(deadline - now_ms);
    }

    void S2sConnector::shutdown_cleanup() {
        // teardown_connection 을 재사용한다(schedule_reconnect=false — 종료 중에는
        // 재연결을 스케줄하면 안 된다). 연결/핸드셰이크 중이 아니었으면 pending_ 은
        // 애초에 비어 있다(handle_request_command 가 !connected_ 일 때 테이블에
        // 넣지 않고 즉시 실패시키므로) — 그래서 left 를 teardown 호출 전에 재도
        // "얼마나 실패했는가"의 정답이다.
        const size_t left = pending_.size();
        teardown_connection(S2sResult::kStopped, -1, /*schedule_reconnect=*/false);

        // teardown_connection 이 closesocket 을 부른 뒤에도, 그 시점에 커널이
        // 이미 걸려 있던 overlapped(recv/send/connect)의 완료는 비동기로
        // 계속 마무리된다 — 그리고 그 완료는 이 객체의 멤버(recv_ctx_ 등)
        // 메모리에 쓴다. iocp_server.cpp::stop() 이 "객체는 지우지 않는다...
        // io_count 가 0 이 된 뒤다" 라고 규율하는 것과 같은 문제의 커넥터 판이다
        // — outstanding_io_ 가 이 객체 버전의 io_count 다. 지금은 S2sLink 가
        // main() 스택 수명이라 이 객체가 완료보다 먼저 죽을 일이 우연히 없어서
        // 가려질 뿐, 그 가정이 깨지면(예: 커넥터를 힙에 만들고 stop() 직후
        // delete 하면) use-after-free 다. ⑩ 시나리오(연결된 채 stop)가 이
        // 경로를 매번 밟는다 — GQCS 를 짧게 반복 호출해 남은 완료를
        // handle_completion 경로(감소 → 세대 검사로 드롭)로 그대로 소비시킨다.
        // 상한을 둔다 — 커널이 끝내 안 주면(있어선 안 되지만) 영원히 못
        // 기다리는 것을 막는다.
        {
            constexpr int kDrainMaxIterations = 10;
            constexpr DWORD kDrainWaitMs = 100;   // 총 상한 ≈ 10 × 100ms = 1초
            int drain_iterations = 0;
            while (outstanding_io_ > 0 && drain_iterations < kDrainMaxIterations) {
                DWORD transferred = 0;
                ULONG_PTR key = 0;
                OVERLAPPED* ov = nullptr;
                const BOOL ok = GetQueuedCompletionStatus(iocp_, &transferred, &key, &ov, kDrainWaitMs);
                ++drain_iterations;
                if (ov != nullptr) {
                    handle_completion(ok, transferred, ov);
                }
                // ov == nullptr(웨이크 또는 타임아웃)이면 그냥 다음 회전으로 —
                // stop() 이후 새 웨이크가 들어올 일은 없다(commands_closed_ 가
                // 이미 서 있어 submit() 이 막힌다), 상한이 결국 멈춰 준다.
            }
            if (outstanding_io_ > 0) {
                core::logf("[WARN] s2s %d outstanding completions not drained\n", outstanding_io_);
            }
        }

        // stop() 신호와 마지막 정상 드레인 사이에 들어온 명령이 있을 수 있다 —
        // 이 시점엔 connected_ 가 이미 false 라 즉시 kDisconnected 로 실패한다
        // (그 자체가 "끊긴 상태의 request 는 접수하지 않는다"는 정상 규칙이다).
        drain_commands();

        core::logf("[S2S  ] connects=%llu reconnects=%llu sent=%llu recv=%llu timeouts=%llu "
            "failed=%llu unsupported_tx=%llu stray_seq=%llu unknown_msg=%llu\n",
            static_cast<unsigned long long>(stats_.connects),
            static_cast<unsigned long long>(stats_.reconnects),
            static_cast<unsigned long long>(stats_.sent),
            static_cast<unsigned long long>(stats_.recv),
            static_cast<unsigned long long>(stats_.timeouts),
            static_cast<unsigned long long>(stats_.failed),
            static_cast<unsigned long long>(stats_.unsupported_tx),
            static_cast<unsigned long long>(stats_.stray_seq),
            static_cast<unsigned long long>(stats_.unknown_msg));

        if (left > 0) {
            // stop 갈래의 실패 완료가 실행으로 관측돼야 한다 — 주석으로 세는 것과
            // 별개로 하네스가 이 줄을 본다(Step2 설계 문서 「카운터·종료 요약」).
            core::logf("[S2S  ] stopped, %zu pending failed\n", left);
        }
    }

    // ── 명령 큐 드레인 ──────────────────────────────────────────────

    void S2sConnector::drain_commands() {
        std::vector<Command> batch;
        {
            std::lock_guard<std::mutex> lock(commands_mutex_);
            batch.swap(commands_);
        }
        if (batch.empty()) {
            return;
        }

        for (auto& cmd : batch) {
            switch (cmd.kind) {
            case CommandKind::kRequest:
                handle_request_command(cmd);
                break;
            case CommandKind::kNotify:
                handle_notify_command(cmd);
                break;
            case CommandKind::kRespond:
                handle_respond_command(cmd);
                break;
            case CommandKind::kForceDisconnect:
                do_force_disconnect(cmd.reconnect_override_ms);
                break;
            }
        }
        // 배치를 다 처리한 뒤 한 번만 판단한다 — 명령마다 발행 여부를 보면
        // 배치로 뭉치는 이점이 사라진다(post_send 자신의 배치화와 같은 이유).
        maybe_kick_send();
    }

    void S2sConnector::handle_request_command(Command& cmd) {
        if (!connected_) {
            // 끊김 상태의 request() — 접수하지 않고 즉시 실패 완료한다. 큐잉해 두면
            // "대기 중 요청"의 정의가 흐려진다(§8-6).
            if (cmd.on_done) {
                cmd.on_done(S2sResult::kDisconnected, cmd.msg_id, nullptr, 0);
            }
            return;
        }

        const uint32_t seq = next_seq_++;
        if (next_seq_ == 0) {
            next_seq_ = 1;      // u32 순환 시 0 은 notify 전용 예약값이라 건너뛴다
        }

        Pending p;
        p.on_done = std::move(cmd.on_done);
        p.deadline_ms = GetTickCount64() + static_cast<uint64_t>(config_.request_timeout_ms);
        pending_.emplace(seq, std::move(p));

        encode_and_queue(cmd.msg_id, seq, cmd.body);
    }

    void S2sConnector::handle_notify_command(Command& cmd) {
        if (!connected_) {
            return;     // 알림은 응답이 없어 실패를 알릴 대상도 없다 — 그냥 버린다
        }
        encode_and_queue(cmd.msg_id, 0, cmd.body);     // seq=0 예약
    }

    void S2sConnector::handle_respond_command(Command& cmd) {
        if (!connected_) {
            return;     // 상대가 이미 없다 — 보낼 곳이 없다
        }
        encode_and_queue(cmd.msg_id, cmd.seq, cmd.body);
        ++stats_.unsupported_tx;    // 이름과 달리 respond() 전량을 센다 — Reserve→ReserveAck
                                    // (3단계)도 이 경로를 타서 지금은 Unsupported 전용이 아니다.
    }

    void S2sConnector::encode_and_queue(uint16_t msg_id, uint32_t seq, const std::vector<char>& body) {
        if (!codec_.header_encode) {
            return;     // 시작 전 설정 실수 — start() 이전에 코덱을 꽂는 것은 호출자 책임이다
        }
        // 호출자 계약 — body 는 0xFFFF 바이트를 넘지 않는다. 넘으면 아래
        // static_cast<uint16_t> 에서 길이 필드가 잘리는데 memcpy 는 원본 크기
        // 그대로 복사해, 헤더가 말하는 길이와 실제로 실린 바이트 수가 어긋난다 —
        // 상대는 뒤에 남는 바이트를 다음 프레임의 헤더로 오독한다.
        // proto::s2s::encode_str16 과 같은 함정이라 같은 방어(assert)를 건다.
        assert(body.size() <= 0xFFFF);
        std::vector<char> frame(static_cast<size_t>(kS2sHeaderSize) + body.size());
        codec_.header_encode(frame.data(), msg_id, seq, static_cast<uint16_t>(body.size()));
        if (!body.empty()) {
            std::memcpy(frame.data() + kS2sHeaderSize, body.data(), body.size());
        }
        queue_send(frame.data(), static_cast<int>(frame.size()));
        ++stats_.sent;
    }

    // ── 연결 ────────────────────────────────────────────────────────

    bool S2sConnector::get_connect_ex() {
        GUID guid = WSAID_CONNECTEX;
        DWORD bytes = 0;
        const int rc = WSAIoctl(sock_, SIO_GET_EXTENSION_FUNCTION_POINTER,
            &guid, sizeof(guid), &connect_ex_, sizeof(connect_ex_), &bytes, nullptr, nullptr);
        if (rc == SOCKET_ERROR) {
            core::logf("[FAIL] s2s WSAIoctl(ConnectEx) (err=%d)\n", WSAGetLastError());
            return false;
        }
        return true;
    }

    bool S2sConnector::begin_connect() {
        sock_ = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
        if (sock_ == INVALID_SOCKET) {
            core::logf("[FAIL] s2s socket (err=%d)\n", WSAGetLastError());
            schedule_reconnect_after_failure();
            return false;
        }

        // ConnectEx 는 미리 bind 된 소켓을 요구한다 — 포트는 아무거나 상관없어 ANY:0.
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = INADDR_ANY;
        local.sin_port = 0;
        if (bind(sock_, reinterpret_cast<sockaddr*>(&local), static_cast<int>(sizeof(local))) == SOCKET_ERROR) {
            core::logf("[FAIL] s2s bind (err=%d)\n", WSAGetLastError());
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
            schedule_reconnect_after_failure();
            return false;
        }

        // bind → IOCP 연관 → ConnectEx 순서(스파이크 SPIKE2 로 검증됨) — 연관을
        // 먼저 해야 완료 통지가 이 IOCP 로 온다.
        if (CreateIoCompletionPort(reinterpret_cast<HANDLE>(sock_), iocp_,
            reinterpret_cast<ULONG_PTR>(this), 0) == nullptr) {
            core::logf("[FAIL] s2s associate (err=%lu)\n", GetLastError());
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
            schedule_reconnect_after_failure();
            return false;
        }

        if (connect_ex_ == nullptr && !get_connect_ex()) {
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
            schedule_reconnect_after_failure();
            return false;
        }

        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_port = htons(config_.port);
        if (inet_pton(AF_INET, config_.host.c_str(), &target.sin_addr) != 1) {
            core::logf("[FAIL] s2s bad host '%s'\n", config_.host.c_str());
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
            schedule_reconnect_after_failure();
            return false;
        }

        connect_ctx_.ov = OVERLAPPED{};
        connect_ctx_.op = IoOp::kConnect;
        connect_ctx_.generation = generation_;

        connecting_ = true;

        // 발행 직전에 올린다(post_recv 와 같은 순서) — 발행이 pending 으로 성공하면
        // 동기 완료든 비동기 완료든 IOCP 로 완료 통지가 오므로 "발행 성공 = 완료를
        // 하나 빚졌다"다. outstanding_io_ 가 0 이 되는 것이 곧 "옛 소켓의 완료가
        // 전부 소비됐다"는 구조적 보장이라 maybe_reconnect 가 이 값을 본다.
        ++outstanding_io_;

        DWORD bytes_sent = 0;
        const BOOL ok = connect_ex_(sock_, reinterpret_cast<sockaddr*>(&target),
            static_cast<int>(sizeof(target)), nullptr, 0, &bytes_sent, &connect_ctx_.ov);
        if (!ok) {
            const int err = WSAGetLastError();
            if (err != ERROR_IO_PENDING) {
                --outstanding_io_;      // 발행 자체가 실패했다 — 완료가 안 온다. 되돌린다
                core::logf("[FAIL] s2s ConnectEx (err=%d)\n", err);
                connecting_ = false;
                closesocket(sock_);
                sock_ = INVALID_SOCKET;
                schedule_reconnect_after_failure();
                return false;
            }
        }
        return true;
    }

    void S2sConnector::on_connect_io_complete(BOOL ok) {
        connecting_ = false;

        if (!ok) {
            core::logf("[FAIL] s2s connect (err=%lu)\n", GetLastError());
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
            schedule_reconnect_after_failure();
            return;
        }

        // ConnectEx 완료 뒤에는 이 옵션을 세워야 getpeername/getsockname 류가 먹는다
        // (스파이크 SPIKE3 로 검증됨).
        setsockopt(sock_, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0);

        connected_ = true;
        if (stats_.connects > 0) {
            ++stats_.reconnects;
        }
        ++stats_.connects;

        backoff_current_ms_ = config_.backoff_initial_ms;   // 연결 성립 시 초기화
        reconnect_override_ms_ = -1;

        core::logf("[S2S  ] connected %s:%u\n", config_.host.c_str(),
            static_cast<unsigned>(config_.port));

        if (!post_recv()) {
            teardown_connection(S2sResult::kDisconnected, -1);
            return;
        }
        if (on_connected_) {
            on_connected_();
        }
    }

    // ── 완료 디스패치 ───────────────────────────────────────────────

    void S2sConnector::handle_completion(BOOL ok, DWORD transferred, OVERLAPPED* ov) {
        // 세대 검사보다 먼저 내린다 — 이 완료가 낡은 세대의 것이어도 "발행했던
        // I/O 하나의 완료를 받았다"는 사실 자체는 똑같다. outstanding_io_ 가
        // 이 값으로 0 에 닿아야 maybe_reconnect 가 다음 연결을 시작한다
        // (outstanding_io_ 주석 참조 — IoCtx 재사용 안전의 구조적 근거).
        --outstanding_io_;
        assert(outstanding_io_ >= 0 && "outstanding_io_ 가 음수 — 발행/완료의 증감 짝이 깨졌다");

        IoCtx* ctx = reinterpret_cast<IoCtx*>(ov);     // 첫 멤버 보장으로 캐스팅

        if (ctx->generation != generation_) {
            // 이미 닫힌 옛 연결의 완료다 — 지금 세대와 무관하므로 조용히 버린다
            // (IoCtx::generation 주석 참조).
            return;
        }

        switch (ctx->op) {
        case IoOp::kConnect:
            on_connect_io_complete(ok);
            break;
        case IoOp::kRecv:
            on_recv_io_complete(ok, transferred);
            break;
        case IoOp::kSend:
            on_send_io_complete(ok, transferred);
            break;
        }
    }

    // ── 수신 ────────────────────────────────────────────────────────

    void S2sConnector::compact_recv_buffer() {
        // 걸린 WSARecv 가 없는 지금(다음 recv 발행 직전)만 안전하다 — session.h
        // RecvBuffer::compact 와 같은 불변식이다.
        if (recv_read_pos_ == 0) {
            return;
        }
        const int left = recv_write_pos_ - recv_read_pos_;
        if (left > 0) {
            std::memmove(recv_buf_.data(), recv_buf_.data() + recv_read_pos_, static_cast<size_t>(left));
        }
        recv_read_pos_ = 0;
        recv_write_pos_ = left;
    }

    bool S2sConnector::post_recv() {
        compact_recv_buffer();

        const int space = static_cast<int>(recv_buf_.size()) - recv_write_pos_;
        if (space <= 0) {
            core::logf("[WARN] s2s recv buffer full — frame exceeds buffer\n");
            return false;
        }

        recv_ctx_.ov = OVERLAPPED{};
        recv_ctx_.op = IoOp::kRecv;
        recv_ctx_.generation = generation_;
        recv_ctx_.wsabuf.buf = recv_buf_.data() + recv_write_pos_;
        recv_ctx_.wsabuf.len = static_cast<ULONG>(space);

        ++outstanding_io_;      // 발행 직전 — outstanding_io_ 주석 참조

        DWORD flags = 0;
        const int rc = WSARecv(sock_, &recv_ctx_.wsabuf, 1, nullptr, &flags, &recv_ctx_.ov, nullptr);
        if (rc == SOCKET_ERROR) {
            const int err = WSAGetLastError();
            if (err != WSA_IO_PENDING) {
                --outstanding_io_;  // 발행 실패 — 완료가 안 온다. 되돌린다
                core::logf("[FAIL] s2s WSARecv (err=%d)\n", err);
                return false;
            }
        }
        return true;
    }

    void S2sConnector::on_recv_io_complete(BOOL ok, DWORD transferred) {
        if (!ok || transferred == 0) {
            teardown_connection(S2sResult::kDisconnected, -1);
            return;
        }
        recv_write_pos_ += static_cast<int>(transferred);

        const uint64_t gen_before = generation_;
        drain_recv_frames();
        if (generation_ != gen_before) {
            // 드레인 도중 응답 콜백이 force_disconnect 를 불렀다 — 디스패치 중단.
            // 소켓이 이미 닫혔고 재연결이 스케줄됐다. 다음 WSARecv 재발행을 걸면
            // 방금 만든 새 연결(또는 재시도 대기 상태)을 오염시킨다.
            return;
        }

        if (!post_recv()) {
            teardown_connection(S2sResult::kDisconnected, -1);
        }
    }

    void S2sConnector::drain_recv_frames() {
        const uint64_t my_generation = generation_;

        for (;;) {
            const int avail = recv_write_pos_ - recv_read_pos_;
            if (avail <= 0) {
                break;
            }
            if (!codec_.frame_sizer) {
                break;      // 코덱 미설정 — start() 이전 설정 실수에 대한 방어적 정지
            }

            const int frame_len = codec_.frame_sizer(recv_buf_.data() + recv_read_pos_, avail);
            if (frame_len == 0) {
                break;      // 아직 부족하다. 다음 완료를 기다린다
            }
            if (frame_len < 0 || frame_len > avail) {
                core::logf("[WARN] s2s protocol violation — closing\n");
                teardown_connection(S2sResult::kDisconnected, -1);
                return;
            }

            dispatch_frame(recv_buf_.data() + recv_read_pos_, frame_len);

            // 콜백(응답 콜백) 안에서 force_disconnect 가 돌았으면 세대가 바뀌어
            // 있다 — 남은 버퍼도 다음 재발행도 전부 건너뛴다(Step2 설계 문서
            // 「디스패치 루프의 중단 메커니즘」).
            if (generation_ != my_generation) {
                return;
            }
            recv_read_pos_ += frame_len;
        }
    }

    void S2sConnector::dispatch_frame(const char* frame, int len) {
        uint16_t msg_id = 0;
        uint32_t seq = 0;
        const char* body = nullptr;
        int body_len = 0;

        if (!codec_.header_decode || !codec_.header_decode(frame, len, msg_id, seq, body, body_len)) {
            // frame_sizer 가 이미 프레임 경계를 확정했으니, 디코드 자체의 실패는
            // 코덱 쪽 버그다 — 프레이밍은 안 깨졌으므로 끊지 않고 이 프레임만 버린다.
            core::logf("[WARN] s2s header decode failed\n");
            return;
        }
        ++stats_.recv;

        const S2sFrameKind kind = codec_.classify ? codec_.classify(msg_id) : S2sFrameKind::kUnknown;
        switch (kind) {
        case S2sFrameKind::kReply: {
            auto it = pending_.find(seq);
            if (it == pending_.end()) {
                core::logf("[WARN] s2s stray seq=%u\n", static_cast<unsigned>(seq));
                ++stats_.stray_seq;
                return;
            }
            // 엔트리를 먼저 move-out + erase 한 뒤 콜백을 부른다 — 콜백 안에서
            // force_disconnect 가 재진입해 테이블을 스왑해도 이미 지워진 이 엔트리는
            // 사본에 없다. "정확 1회" 계약이 이 순서에 걸려 있다.
            Pending p = std::move(it->second);
            pending_.erase(it);
            // 캐너리 — move-out→erase→콜백 순서가 뒤집히면(콜백을 먼저 부르면)
            // 콜백 안 재진입 force_disconnect 의 테이블 스왑과 만나 반복자가
            // 죽은 채로 남는 반복자 UB 다(위 주석의 "정확 1회" 계약이 이 순서에
            // 걸려 있다는 말 그대로). 하네스 항목 중 이 순서 자체를 결정적으로
            // 잡는 판정은 없어서, 이 순서가 회귀로 뒤집히면 Debug 실행에서
            // 여기가 걸리게 캐너리를 둔다 — erase 가 이미 끝났으니 seq 는 더
            // 이상 테이블에 없어야 한다.
            assert(pending_.find(seq) == pending_.end());
            if (p.on_done) {
                p.on_done(S2sResult::kOk, msg_id, body, body_len);
            }
            break;
        }
        case S2sFrameKind::kRequest:
            if (on_request_) {
                on_request_(msg_id, seq, body, body_len);
            }
            break;
        case S2sFrameKind::kUnknown:
        default:
            core::logf("[WARN] s2s unknown msg_id=0x%04x\n", static_cast<unsigned>(msg_id));
            ++stats_.unknown_msg;
            break;
        }
    }

    // ── 송신 ────────────────────────────────────────────────────────

    void S2sConnector::queue_send(const char* data, int len) {
        send_back_.insert(send_back_.end(), data, data + len);
    }

    void S2sConnector::maybe_kick_send() {
        if (!connected_ || send_pending_) {
            return;
        }
        if (send_back_.empty() && send_front_.empty()) {
            return;
        }
        send_pending_ = true;
        if (!post_send()) {
            send_pending_ = false;
            teardown_connection(S2sResult::kDisconnected, -1);
        }
    }

    bool S2sConnector::post_send() {
        // front 를 다 보냈으면 back 을 통째로 가져온다 — 배치가 생기는 지점이다
        // (iocp_server.cpp::post_send 와 같은 swap).
        if (send_sent_ >= static_cast<int>(send_front_.size())) {
            send_front_.clear();
            send_sent_ = 0;
            send_front_.swap(send_back_);
        }

        if (send_front_.empty()) {
            send_pending_ = false;
            return true;
        }

        send_ctx_.ov = OVERLAPPED{};
        send_ctx_.op = IoOp::kSend;
        send_ctx_.generation = generation_;
        send_ctx_.wsabuf.buf = send_front_.data() + send_sent_;
        send_ctx_.wsabuf.len = static_cast<ULONG>(send_front_.size() - static_cast<size_t>(send_sent_));

        ++outstanding_io_;      // 발행 직전 — outstanding_io_ 주석 참조

        const int rc = WSASend(sock_, &send_ctx_.wsabuf, 1, nullptr, 0, &send_ctx_.ov, nullptr);
        if (rc == SOCKET_ERROR) {
            const int err = WSAGetLastError();
            if (err != WSA_IO_PENDING) {
                --outstanding_io_;  // 발행 실패 — 완료가 안 온다. 되돌린다
                core::logf("[FAIL] s2s WSASend (err=%d)\n", err);
                send_pending_ = false;
                return false;
            }
        }
        return true;
    }

    void S2sConnector::on_send_io_complete(BOOL ok, DWORD transferred) {
        if (!ok) {
            teardown_connection(S2sResult::kDisconnected, -1);
            return;
        }
        send_sent_ += static_cast<int>(transferred);
        if (!post_send()) {
            teardown_connection(S2sResult::kDisconnected, -1);
        }
    }

    // ── 끊김 · 재연결 · 타임아웃 ────────────────────────────────────

    void S2sConnector::do_force_disconnect(int reconnect_delay_ms) {
        teardown_connection(S2sResult::kDisconnected,
            (reconnect_delay_ms > 0) ? reconnect_delay_ms : -1);
    }

    void S2sConnector::teardown_connection(S2sResult fail_reason, int reconnect_override_ms,
        bool schedule_reconnect) {
        // 진입 즉시, 아래에서 테이블을 스왑하기 전에 세대를 올린다 — 이것이
        // 멱등 가드다. 콜백 재진입(예: 실패 콜백이 다시 force_disconnect 를 부른다)
        // 이 여기로 다시 들어와도, connected_/connecting_ 이 이미 false 라 아래에서
        // 조용히 반환한다 — 두 번째 teardown(closesocket 재호출 · on_disconnected_
        // 재호출 · 재연결 스케줄 덮어쓰기)을 막는 것이 이 순서의 목적이다.
        ++generation_;

        if (!connected_ && !connecting_) {
            return;     // 이미 끊긴 상태 — 할 일이 없다
        }

        if (sock_ != INVALID_SOCKET) {
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
        }
        const bool was_connected = connected_;
        connected_ = false;
        connecting_ = false;

        // 수신 버퍼에는 끊기는 순간 거의 항상 잔여 바이트가 있다 — ① frame_sizer 가
        // 아직 0(부족)을 돌려주던 부분 프레임 ② force_disconnect 로 중단된 배치의
        // 미처리 후속 프레임(drain_recv_frames 가 세대 변화를 보고 read_pos 를 안
        // 올린 채 이탈한 바이트들). 다음 연결은 이전 스트림과 완전히 무관한데,
        // 리셋하지 않으면 post_recv→compact_recv_buffer 가 이 잔여를 앞으로 밀고
        // 새 연결의 바이트를 그 뒤에 이어 붙인다 — 옛 연결의 바이트가 새 스트림의
        // 머리가 되어 프레이밍이 통째로 어긋난다.
        recv_read_pos_ = 0;
        recv_write_pos_ = 0;

        fail_all_pending(fail_reason);
        next_seq_ = 1;

        // 송신 버퍼도 이 연결의 것이다 — 다음 연결로 새지 않게 비운다.
        send_front_.clear();
        send_back_.clear();
        send_sent_ = 0;
        send_pending_ = false;

        if (was_connected && on_disconnected_) {
            on_disconnected_();
        }

        if (!schedule_reconnect) {
            return;     // stop 경로 — 종료 중이라 재연결을 스케줄하지 않는다
        }
        if (reconnect_override_ms >= 0) {
            reconnect_override_ms_ = reconnect_override_ms;
        }
        schedule_reconnect_after_failure();
    }

    void S2sConnector::schedule_reconnect_after_failure() {
        const int delay = (reconnect_override_ms_ >= 0) ? reconnect_override_ms_ : backoff_current_ms_;
        reconnect_override_ms_ = -1;    // 1회성 — 쓰고 나면 지운다

        next_action_deadline_ms_ = GetTickCount64() + static_cast<uint64_t>(delay);

        // 다음 실패를 위해 지수 증가 — 이번 delay 는 위에서 이미 확정했으므로
        // 오버라이드가 있었어도 정상 진행분은 오염되지 않는다. 무한 반복(§5-2) —
        // backoff_max_ms 에 닿으면 거기서 멈추고 계속 그 값으로 재시도한다.
        backoff_current_ms_ = (backoff_current_ms_ * 2 < config_.backoff_max_ms)
            ? backoff_current_ms_ * 2 : config_.backoff_max_ms;

        core::logf("[S2S  ] disconnected, retry in %dms\n", delay);
    }

    void S2sConnector::fail_all_pending(S2sResult reason) {
        // 스왑한 사본으로 순회한다 — 콜백 안에서 force_disconnect 가 재진입해
        // pending_ 을 다시 건드려도(예: 실패를 본 상위가 즉시 새 요청을 낸다) 이
        // 순회는 이미 떼어 낸 사본 위에서 돌고 있어 영향받지 않는다(ADR-019 결정 8).
        std::unordered_map<uint32_t, Pending> doomed;
        doomed.swap(pending_);
        for (auto& entry : doomed) {
            Pending p = std::move(entry.second);
            if (p.on_done) {
                p.on_done(reason, 0, nullptr, 0);
            }
            ++stats_.failed;
        }
    }

    void S2sConnector::sweep_timeouts(uint64_t now_ms) {
        // 2단계로 나눈다 — 1차 패스는 마감 지난 엔트리를 콜백 호출 없이 move-out +
        // erase 만 해서 지역 vector 에 모은다. 콜백을 pending_ 순회 도중 바로
        // 부르면, 콜백 안에서 force_disconnect 가 재진입해(동기 즉시 — S2S 스레드)
        // teardown_connection→fail_all_pending 이 pending_ 을 통째로 swap 하는데,
        // 그 순간 이 함수가 들고 있던 반복자는 이제 다른 컨테이너(스왑된 사본)
        // 소속인 원소를 가리키게 되어 다음 pending_.erase(it) 가 정의되지 않은
        // 동작이다. 2차 패스는 이미 pending_ 을 떠난 사본 위에서만 콜백을 부르므로
        // 그 문제가 없다 — erase-before-invoke 규율의 일괄판이다.
        std::vector<Pending> expired;
        for (auto it = pending_.begin(); it != pending_.end(); ) {
            if (it->second.deadline_ms <= now_ms) {
                expired.push_back(std::move(it->second));
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }

        for (auto& p : expired) {
            ++stats_.timeouts;
            if (p.on_done) {
                p.on_done(S2sResult::kTimeout, 0, nullptr, 0);
            }
        }
    }

    void S2sConnector::maybe_reconnect(uint64_t now_ms) {
        if (connected_ || connecting_) {
            return;
        }
        if (now_ms < next_action_deadline_ms_) {
            return;
        }
        if (outstanding_io_ != 0) {
            // 옛 소켓에 걸려 있던 I/O 의 완료가 아직 IOCP 에서 다 소비되지 않았다.
            // 지금 begin_connect 를 부르면 connect_ctx_/recv_ctx_/send_ctx_ 를 그
            // 완료가 도착하기 전에 재사용하게 되어 generation 비교가 무의미해진다
            // (outstanding_io_ 주석 참조). 다음 GQCS 회전에서 그 완료가 처리되며
            // 이 값이 내려가면 이 함수가 다시 불릴 때 통과한다 — 별도 타이머 없이
            // thread_main 의 매 회전이 재시도 지점이다.
            return;
        }
        begin_connect();
    }

    void S2sConnector::maybe_tick(uint64_t now_ms) {
        if (!on_tick_ || tick_interval_ms_ <= 0) {
            return;
        }
        if (now_ms < next_tick_ms_) {
            return;
        }
        next_tick_ms_ = now_ms + static_cast<uint64_t>(tick_interval_ms_);
        on_tick_();
    }

}   // namespace net
