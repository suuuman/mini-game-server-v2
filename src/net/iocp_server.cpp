#include "net/iocp_server.h"

#include "core/log.h"

#include <chrono>
#include <cstdio>
#include <cassert>

namespace {
	void log_failure(const char* msg) {
		core::logf("[FAIL] %s (err=%d)\n", msg, WSAGetLastError());
	}
}	// namespace

namespace net {
	IocpServer::IocpServer(size_t frame_pool_capacity)
		: frame_pool_(kRecvBufferSize, frame_pool_capacity) {
	}

	IocpServer::~IocpServer() {
		stop();
	}

	bool IocpServer::start_rollback() {
		if (listen_sock_ != INVALID_SOCKET) {
			closesocket(listen_sock_);
			listen_sock_ = INVALID_SOCKET;
		}
		if (iocp_ != nullptr) {
			CloseHandle(iocp_);
			iocp_ = nullptr;
		}
		return false;
	}

	bool IocpServer::start(unsigned short port, int worker_count, size_t max_connections) {
		max_connections_ = max_connections;

		//  1. 완료 포트 생성
		//     마지막 인자 0 = "동시에 깨어 있을 스레드 수를 CPU 코어 수에 맡긴다".
		//    이 값과 「워커 스레드를 몇 개 만드느냐」는 다른 숫자다.
		//       스레드는 넉넉히 만들어 두고, 커널이 그중 몇 개만 깨우게 하는 것이
		//       IOCP 의 방식이다. 깨어 있던 워커가 블록되면 커널이 하나를 더 깨운다.
		iocp_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
		if (iocp_ == nullptr) {
			// 소켓 API 가 아니므로 WSAGetLastError 가 아니라 GetLastError 다
			core::logf("[FAIL] CreateIoCompletionPort (err=%lu)\n", GetLastError());
			return false;
		}

		//  2. 듣기 소켓
		//     WSA_FLAG_OVERLAPPED 를 명시한다. socket() 으로 만들어도 이 속성이
		//     기본으로 붙지만, 적어 두면 "이 소켓은 비동기로 쓴다"는 의도가 남는다.
		listen_sock_ = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
		if (listen_sock_ == INVALID_SOCKET) {
			log_failure("WSASocketW failed");
			return start_rollback();
		}

		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = INADDR_ANY;
		addr.sin_port = htons(port);

		if (bind(listen_sock_, reinterpret_cast<sockaddr*>(&addr), static_cast<int>(sizeof(addr))) == SOCKET_ERROR) {
			log_failure("bind failed");
			return start_rollback();
		}

		// SOMAXCONN — 커널이 정한 최대치. 숫자를 직접 고르는 건
		// 접속 폭주를 실측한 다음에 할 일이지, 지금 감으로 정할 값이 아니다.
		if (listen(listen_sock_, SOMAXCONN) == SOCKET_ERROR) {
			log_failure("listen failed");
			return start_rollback();
		}

		//  3. 스레드 기동
		//    running_ 을 스레드보다 먼저 세운다. 반대로 하면 갓 뜬 워커가
		//       running_ == false 를 보고 즉시 죽는다. 초기화 순서가 곧 버그다.
		running_ = true;

		workers_.reserve(static_cast<size_t>(worker_count));
		for (int i = 0; i < worker_count; ++i) {
			workers_.emplace_back(&IocpServer::worker_loop, this);
		}
		accept_thread_ = std::thread(&IocpServer::accept_loop, this);

		// 꺼져 있으면 스레드를 「안 만든다」. 만들어 놓고 아무것도 안 하게 두면
		//   스레드 목록에는 있는데 하는 일이 없어서, 나중에 보는 사람이
		//   「이게 도는데 왜 안 끊기지」를 먼저 의심하게 된다.
		if (idle_timeout_sec_ > 0) {
			sweep_thread_ = std::thread(&IocpServer::sweep_loop, this);
		}

		core::logf("[INFO] listening on %u (workers=%d idle_timeout=%ds sweep=%ds)\n",
			static_cast<unsigned>(port), worker_count,
			idle_timeout_sec_, sweep_interval_sec_);

		return true;
	}

	void IocpServer::stop() {
		if (!running_.exchange(false)) {
			return;		// 이미 멈췄다. 소멸자와 명시적 stop 이 겹쳐도 한 번만 돈다
		}

		// 순서가 전부다. 하나만 바꿔도 join 이 영원히 안 끝난다.
		//   1. 듣기 소켓을 닫는다   accept 가 에러로 깨어나 루프를 빠진다
		//   2. 세션 소켓을 닫는다   걸려 있던 WSARecv 가 실패 완료로 돌아온다.
		//                          안 하면 커널이 그 버퍼를 계속 붙들고 있다
		//   3. 가짜 완료를 워커 수만큼 넣는다
		//   4. 워커 join            이게 끝나야 큐에 Job 을 넣는 쪽이 사라진다
		//   5. 큐 stop → 로직 스레드 join
		//   6. 세션 삭제            로직 스레드가 죽은 뒤여야 한다
		// 뒤의 셋은 못 뒤집는다. 5를 4보다 먼저 하면 워커가 아직 살아서 push 하니 큐가
		// 영영 안 비고, 6을 5보다 먼저 하면 로직 스레드가 지워진 세션을 만진다.
		if (listen_sock_ != INVALID_SOCKET) {
			closesocket(listen_sock_);
			listen_sock_ = INVALID_SOCKET;
		}
		if (accept_thread_.joinable()) {
			accept_thread_.join();
		}

		// 스윕도 큐에 Job 을 넣는 쪽이다 — close_session 이 session_gone_ 을 부르고
		// 그게 존 스레드에 정리 Job 을 넣는다. 그래서 워커와 같은 규칙이 적용된다:
		// drain_hook_ 전에 반드시 죽어야 한다. join 만 하면 최대 한 주기를 기다리므로
		// 깨워서 보낸다. running_ 은 이 함수 첫 줄에서 이미 false 다.
		sweep_cv_.notify_all();
		if (sweep_thread_.joinable()) {
			sweep_thread_.join();
		}

		{
			std::lock_guard<std::mutex> lock(sessions_mutex_);
			for (const auto& entry : sessions_) {
				Session& s = *entry.second;
				if (s.socket != INVALID_SOCKET) {
					closesocket(s.socket);
					s.socket = INVALID_SOCKET;
				}
			}
			// 객체는 지우지 않는다. 걸려 있던 WSARecv 의 완료가 아직 큐에 남아
			// 이 메모리를 가리킬 수 있다. 지우는 것은 io_count 가 0 이 된 뒤다.
		}

		// PostQueuedCompletionStatus — 진짜 I/O 없이 완료 큐에 항목을 넣는 유일한 방법.
		// 완료 키 0 · OVERLAPPED nullptr 을 「종료하라」는 약속으로 쓴다.
		for (size_t i = 0; i < workers_.size(); ++i) {
			PostQueuedCompletionStatus(iocp_, 0, 0, nullptr);
		}
		for (auto& t : workers_) {
			if (t.joinable()) {
				t.join();
			}
		}
		workers_.clear();

		// 큐 stop 과 로직 스레드 join 자리다. 그 둘은 world 가 알고, net 은 언제 할지만
		// 안다. 훅이 없으면 아무것도 안 한다 — net 만으로도 돌아가야 하니까.
		if (drain_hook_) {
			drain_hook_();
		}

		// 워커도 존 스레드도 전부 끝난 뒤다. 세션을 만지는 스레드가 이제 없다.
		{
			std::lock_guard<std::mutex> lock(sessions_mutex_);
			sessions_.clear();
		}

		if (iocp_ != nullptr) {
			CloseHandle(iocp_);
			iocp_ = nullptr;
		}
		core::logf("[INFO] stopped\n");
	}

	void IocpServer::accept_loop() {
		for (;;) {
			sockaddr_in peer{};
			int peer_len = static_cast<int>(sizeof(peer));

			SOCKET client = accept(listen_sock_, reinterpret_cast<sockaddr*>(&peer), &peer_len);
			if (client == INVALID_SOCKET) {
				// stop() 이 듣기 소켓을 닫으면 여기로 온다 — 오류가 아니라 정상 경로다.
				if (!running_) {
					break;	// stop() 에서 닫아서 에러 난 거면 정상 종료
				}
				log_failure("accept failed");
				continue;
			}
			if (!running_) {
				closesocket(client);
				break;
			}

			//  Session 의 주소를 커널에 맡길 것이므로 절대 움직이면 안 된다.
			//    vector<Session> 이었다면 재할당 때 주소가 통째로 옮겨가고,
			//    커널이 들고 있던 완료 키는 그 순간 허공을 가리킨다.
			//    unique_ptr 은 객체 주소를 고정한다. 그래서 이 형태다.
			Session* s = add_session(client, peer);

			//  상한에 걸렸다 — 그냥 닫는다. 거절 사유를 보내지 않는 이유는
			//    아직 이 상대가 우리 규약을 쓰는지조차 모르기 때문이다.
			//    (규약을 모르는 상대에게 규약을 가르쳐 주는 일이 된다 — on_frame 의
			//     「모르는 메시지」와 같은 판단이다)
			//
			//  로그를 2의 거듭제곱 지점에서만 찍는다. 상한에 닿았다는 건
			//    접속이 몰리고 있다는 뜻이라, 한 줄씩 찍으면 그 순간 로그 큐가 터진다.
			//    누적 수는 종료 통계에서 정확히 나온다.
			if (s == nullptr) {
				closesocket(client);
				const uint64_t n = accept_rejected_.load(std::memory_order_relaxed);
				if ((n & (n - 1)) == 0) {
					core::logf("[WARN] max_connections=%zu 도달 — 접속 거절 (누적 %llu)\n",
						max_connections_, static_cast<unsigned long long>(n));
				}
				continue;
			}

			//  소켓을 완료 포트에 붙인다. 세 번째 인자가 완료 키다.
			//    여기서 넘긴 값이 GetQueuedCompletionStatus 로 그대로 돌아온다.
			//    "누구의 완료인가"를 이 한 줄이 정한다.
			if (CreateIoCompletionPort(reinterpret_cast<HANDLE>(client), iocp_,
				reinterpret_cast<ULONG_PTR>(s), 0) == nullptr) {
				core::logf("[FAIL] associate (err=%lu)\n", GetLastError());
				close_session(*s);
				// 이 세션엔 아직 건 I/O 가 하나도 없다(io_count == 0).
				//   그래서 release_io 가 아니라 try_remove_session 을 직접 부른다 —
				//   여기서 release_io 를 부르면 카운트가 음수로 내려간다.
				try_remove_session(s);
				continue;
			}

			core::logf("[INFO] #%llu accepted %s\n",
				static_cast<unsigned long long>(s->id), s->peer_text);

			if (!post_recv(*s)) {
				close_session(*s);
				// 이 세션엔 아직 건 I/O 가 하나도 없다(io_count == 0).
				//   그래서 release_io 가 아니라 try_remove_session 을 직접 부른다 —
				//   여기서 release_io 를 부르면 카운트가 음수로 내려간다.
				try_remove_session(s);
			}
		}
		core::logf("[INFO] accept loop out\n");
	}

	//  I/O 워커 — 완료를 꺼내서 처리하고, 다시 예약한다.
	void IocpServer::worker_loop() {
		for (;;) {
			DWORD       transferred = 0;
			ULONG_PTR   key = 0;
			OVERLAPPED* ov = nullptr;

			BOOL ok = GetQueuedCompletionStatus(iocp_, &transferred, &key, &ov, INFINITE);

			// 종료 신호. stop() 이 넣은 가짜 완료는 OVERLAPPED 가 nullptr 이다.
			//   진짜 I/O 완료는 절대 nullptr 일 수 없으므로 이걸 약속으로 쓴다.
			if (ov == nullptr) {
				break;
			}

			// 두 축. 완료 키 = 누구의 완료인가 / OVERLAPPED = 어느 I/O 의 완료인가
			Session* session = reinterpret_cast<Session*>(key);
			IoContext* ctx = reinterpret_cast<IoContext*>(ov);
			// ↑ OVERLAPPED 가 첫 멤버라서 가능

			// 홀드를 여기서 내리지 않는다. 완료를 꺼낸 시점에 커널은 이 I/O 에서 손을
			// 뗐으니 내려도 맞아 보이지만, 내리는 순간 0 이 될 수 있고 0 은 지워도 되는
			// 세션이라는 뜻이다 — 내가 아직 처리 중인데도. 커널의 홀드를 그대로
			// 이어받고 나갈 때 한 번만 반납한다. 아래 모든 경로가 release_io 로 끝난다.

			// ── I/O 자체가 실패 ─────────────────────────────────────────────
			if (!ok) {
				core::logf("[INFO] #%llu %s failed (err=%lu, holds=%d)\n",
					static_cast<unsigned long long>(session->id),
					(ctx->op == IoOp::Recv) ? "recv" : "send",
					GetLastError(), session->io_count.load());
				close_session(*session);
				release_io(session);
				continue;
			}

			// ── 송신 완료 ───────────────────────────────────────────────────
			if (ctx->op == IoOp::Send) {
				{
					std::lock_guard<std::mutex> lock(session->send_buf.mutex);
					session->send_buf.sent += static_cast<int>(transferred);
				}

				// post_send 하나가 두 가지를 다 한다 —
				//   front 가 남았으면(부분 송신) 이어서 발행하고,
				//   다 나갔으면 back 을 swap 해서 쌓인 것을 배치로 내보낸다.
				if (!post_send(*session)) {
					close_session(*session);
				}
				release_io(session);
				continue;
			}

			// ── 수신 완료 ───────────────────────────────────────────────────
			// transferred == 0 검사는 여기(Recv)에만 있어야 한다.
			//   "0바이트 = 상대가 닫았다"는 recv 의 규칙이지 send 의 규칙이 아니다.
			if (transferred == 0) {
				core::logf("[INFO] #%llu peer closed (holds=%d)\n",
					static_cast<unsigned long long>(session->id),
					session->io_count.load());
				close_session(*session);
				release_io(session);
				continue;
			}

			// 커널이 누적 버퍼에 직접 썼다. 나는 커서만 밀어 준다.
			session->recv_buf.write_pos += static_cast<int>(transferred);

			// 「살아 있다」의 판정을 여기서 한다 — 프레임 파싱 위가 아니라.
			//   여기는 net 이라 이 바이트가 ping 인지 채팅인지 모르는데, 그게 맞다.
			//   「무엇을 받았는가」는 살아 있음의 근거가 아니다. 받았다는 사실이 근거다.
			//   ping 만 세면 말 많은 유저가 더 잘 끊기고, 프레임 완성을 기다리면
			//     절반만 온 프레임을 계속 보내는 상대가 조용한 것으로 읽힌다.
			//
			//   relaxed 인 이유 — 이 값은 다른 것의 발행/획득을 지키지 않는다.
			//   스윕이 한 주기 늦게 새 값을 봐도 결과는 「10초 뒤에 다시 본다」뿐이다.
			session->last_recv_ms.store(GetTickCount64(), std::memory_order_relaxed);

			// 세 갈래가 한 모양이 됐다 — 「끊을지 판단 → 반납」.
			//   갈래마다 close + try_remove 를 따로 적으면,
			//   경로 하나를 빠뜨렸을 때 그 경로만 조용히 세션을 흘린다.
			//   반납 지점이 하나면 빠뜨릴 자리가 없다.
			if (!drain_frames(*session) || !running_ || !post_recv(*session)) {
				close_session(*session);
			}
			release_io(session);
			// 이 줄 아래에서 session 을 만지면 안 된다. 이미 지워졌을 수 있다.
		}
	}

	//  수신을 「예약」한다. 최초 1회와, 완료를 처리한 뒤 재발행에 둘 다 쓴다.
	//
	//  완료 통지는 한 번짜리 예약이다. 처리하고 나서 다시 걸지 않으면
	//    이 세션은 두 번 다시 통지가 오지 않는다.
	//    epoll(준비 통지)은 조건이 유지되면 계속 알려주지만, 완료 통지는 안 그렇다.
	//    이것이 두 모델의 실질적 차이 중 코드에 가장 크게 드러나는 부분이다.
	bool IocpServer::post_recv(Session& session) {
		// 닫히는 중이면 새 I/O 를 걸지 않는다.
		//   여기서 막지 않으면 io_count 가 계속 다시 올라가 영원히 0 이 안 된다.
		if (session.closing) {
			return false;
		}

		RecvBuffer& rb = session.recv_buf;

		// 당기기는 여기서만 한다.
		//    지금은 걸린 I/O 가 없는 유일한 순간이다. 커널이 쓰는 중에 memmove 하면
		//    커널은 옮기기 전 주소에 계속 쓴다 — ASan 도 못 잡는 사고다.
		//    (pending 이 0이면 memmove 없이 커서만 되돌리므로 흔한 경우엔 공짜다)
		rb.compact();

		if (rb.space() <= 0) {
			// 당겼는데도 공간이 없다 = 한 프레임이 버퍼보다 크다.
			// sizer 가 먼저 걸렀어야 하지만, net 도 자기 안전망을 갖는다. 이중 방어.
			core::logf("[WARN] #%llu recv buffer full — frame exceeds buffer\n",
				static_cast<unsigned long long>(session.id));
			return false;
		}

		IoContext& ctx = session.recv_ctx;
		ctx.ov = OVERLAPPED{};
		ctx.op = IoOp::Recv;	// 이 I/O 가 recv 용임을 표시
		ctx.wsabuf.buf = rb.tail();                          // 커널이 누적 버퍼에 직접 쓴다
		ctx.wsabuf.len = static_cast<ULONG>(rb.space());

		// 발행 「직전」에 올린다. 순서가 전부다.
		//    WSARecv 를 먼저 부르고 나서 올리면, 그 사이에 완료가 돌아와
		//    워커가 먼저 내려버린다. 카운트가 음수가 되고,
		//    「마지막 I/O 였다」는 판정이 영원히 안 온다.
		session.io_count.fetch_add(1);

		DWORD flags = 0;
		int rc = WSARecv(session.socket, &ctx.wsabuf, 1,
			nullptr, &flags, &ctx.ov, nullptr);

		if (rc == SOCKET_ERROR) {
			const int err = WSAGetLastError();

			// WSA_IO_PENDING 은 실패가 아니다.
			//   "접수했고 끝나면 알려주겠다"는 뜻이고, 비동기에서는 이게 정상 경로다.
			//   이걸 에러로 처리하면 서버가 한 발짝도 안 나간다.
			if (err != WSA_IO_PENDING) {
				// 발행에 실패했으면 올린 것을 되돌린다.
				//   이 I/O 의 완료는 절대 오지 않는다. 안 내리면 io_count 가
				//   영원히 0 이 안 되고, 세션이 영영 안 지워진다 — 누수다.
				session.io_count.fetch_sub(1);

				core::logf("[FAIL] WSARecv #%llu (err=%d)\n",
					static_cast<unsigned long long>(session.id), err);
				return false;
			}
		}
		// rc == 0 이면 이미 완료된 것이다. 그래도 완료 통지는 별도로 온다.
		// 그래서 여기서 따로 처리하지 않는다 — 처리 경로를 워커 한 곳으로 모은다.
		return true;
	}
	
	//  소켓만 닫는다. Session 객체는 지우지 않는다.
	void IocpServer::close_session(Session& session) {
		// closing 을 먼저 세운다. 이걸 본 스레드는 더 이상 새 I/O 를 걸지 않는다.
		//   exchange 로 「내가 처음 닫는 사람인가」를 원자적으로 판정한다.
		//   socket == INVALID_SOCKET 검사만으로는 읽고 쓰는 사이가 벌어져 있어서,
		//   워커 둘이 동시에 통과할 수 있었다. 그때는 세션당 I/O 가 하나라
		//   그 상황이 안 생겼을 뿐이고, 세션당 I/O 가 둘이 되면 진짜 경쟁이 된다.
		if (session.closing.exchange(true)) {
			return;                       // 이미 다른 스레드가 닫았다
		}

		closesocket(session.socket);
		session.socket = INVALID_SOCKET;

		core::logf("[INFO] #%llu closed %s\n",
			static_cast<unsigned long long>(session.id), session.peer_text);

		// 위층에 알린다. 위층은 이 세션을 존에서 빼야 하는데 그건 존 스레드의 일이라
		// Job 으로 보낸다. 그동안 세션이 지워지면 안 되니 홀드를 먼저 올린다 — 반대로
		// 하면 그 Job 이 아직 안 올라온 홀드를 반납하려 든다.
		//
		// 되돌릴 때 release_io 를 안 쓰는 것은 그게 세션을 지울 수 있어서다. 이 함수의
		// 호출자는 돌아간 뒤에도 세션을 만진다. 지우는 판단은 한 곳에서만 한다.
		if (session_gone_) {
			session.io_count.fetch_add(1);
			if (!session_gone_(session)) {
				session.io_count.fetch_sub(1);      // 못 받았다 — 되돌린다
			}
		}
	}

	// 구현은 sweep_loop 의 3단계 패턴 그대로다 — sessions_mutex_ 안 선형
	// 순회로 찾고, 락 밖에서 close_session → release_io.
	//
	// closing 이 이미 참이면 다른 스레드가 이미 닫는 중이다 — 홀드를 새로
	// 올리지 않고 true 만 돌려준다. 여기서 홀드를 올려도 close_session 의
	// closing.exchange 가 실제 작업을 1회로 수렴시키니 안전은 하지만,
	// 이미 닫히는 중인 세션에는 더 할 일이 없다.
	//
	// 이 함수의 유일한 호출자는 S2S 스레드다(app::S2sLink::on_request 의
	// Kick 분기). main.cpp 가 server.stop() 보다 s2s_link.stop() 을 먼저
	// 불러 그 스레드를 동기 join 하므로, stop() 말미의 sessions_.clear()
	// (io_count 무관 무조건 삭제)가 도는 시점엔 이 함수를 부를 스레드
	// 자체가 이미 없다 — 「세션을 만지는 스레드가 이제 없다」는 stop() 의
	// 전제가 close_by_id 신설 뒤에도 그대로 유지된다.
	bool IocpServer::close_by_id(uint64_t session_id) {
		Session* target = nullptr;
		bool already_closing = false;
		{
			std::lock_guard<std::mutex> lock(sessions_mutex_);
			for (const auto& entry : sessions_) {
				if (entry.first->id == session_id) {
					target = entry.first;
					break;
				}
			}
			if (target == nullptr) {
				return false;
			}
			if (target->closing.load()) {
				already_closing = true;
			} else {
				target->io_count.fetch_add(1);
			}
		}

		if (already_closing) {
			return true;
		}

		close_session(*target);
		release_io(target);
		return true;
	}

	// nullptr = 상한에 걸렸다. 호출자가 소켓을 닫는다.
	//   「거절」이지 「실패」가 아니다 — 서버는 멀쩡하고, 지금 자리가 없을 뿐이다.
	Session* IocpServer::add_session(SOCKET sock, const sockaddr_in& peer) {
		auto owned = std::make_unique<Session>();
		Session* s = owned.get();

		s->id = next_session_id_++;
		s->socket = sock;
		s->peer = peer;

		// 0 으로 두면 「접속만 하고 아직 아무것도 안 보낸」 세션이 첫 스윕에 끊긴다.
		//   붙는 것과 첫 메시지 사이에는 원래 시간이 걸린다 — 그 사이를 침묵으로 세면 안 된다.
		//   맵에 넣기 「전」이어야 한다. 넣은 뒤면 스윕이 0 인 상태를 볼 창이 생긴다.
		s->last_recv_ms.store(GetTickCount64(), std::memory_order_relaxed);

		char ip[INET_ADDRSTRLEN]{};
		inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
		std::snprintf(s->peer_text, sizeof(s->peer_text), "%s:%u",
			ip, static_cast<unsigned>(ntohs(peer.sin_port)));

		size_t now = 0;
		{
			std::lock_guard<std::mutex> lock(sessions_mutex_);

			// 검사와 삽입이 같은 락 안이다. 나누면 그 사이가 구멍이 된다.
			if (max_connections_ != 0 && sessions_.size() >= max_connections_) {
				accept_rejected_.fetch_add(1, std::memory_order_relaxed);
				return nullptr;         // owned 가 여기서 소멸한다 — 세션은 안 만들어졌다
			}

			sessions_.emplace(s, std::move(owned));
			now = sessions_.size();
		}

		// peak 을 남긴다. 「상한을 얼마로 잡을 것인가」를 감이 아니라 이 숫자로 정한다 —
		//   프레임 풀 peak 과 같은 이유다.
		size_t seen = session_peak_.load(std::memory_order_relaxed);
		while (now > seen &&
			!session_peak_.compare_exchange_weak(seen, now, std::memory_order_relaxed)) {
		}
		return s;
	}

	void IocpServer::try_remove_session(Session* session) {
		std::unique_ptr<Session> doomed;        // 락 밖에서 소멸시키려고 여기 받아 둔다
		{
			std::lock_guard<std::mutex> lock(sessions_mutex_);

			// 맵에서 「먼저 찾는다」. 조건을 먼저 보면 안 된다.
			//    다른 스레드가 이미 지웠다면 session 은 죽은 메모리다.
			//    session->closing 을 읽는 순간 그게 use-after-free 다.
			//    맵에 있느냐를 먼저 보는 것이 「살아 있느냐」를 확인하는 유일한 방법이다.
			//    → 그래서 이 함수는 참조가 아니라 포인터를 받는다. 참조는 「살아 있다」는
			//      약속을 이름부터 하고 들어가는데, 여기서는 그 약속을 못 하기 때문이다.
			auto it = sessions_.find(session);
			if (it == sessions_.end()) {
				return;		// 이미 다른 스레드가 지웠다
			}
			
			Session& s = *it->second;
			
			// 두 조건을 「같은 락 안에서」 함께 본다.
			//    따로 보면 이런 순서가 가능하다:
			//      워커A: io_count 를 0 으로 내림 → closing 을 읽음(아직 false) → 안 지움
			//      워커B: closing 을 true 로 씀   → io_count 를 읽음(아직 1)   → 안 지움
			//    둘 다 "내 차례가 아니네" 하고 지나가면 세션이 영원히 안 지워진다.
			//    락 안에서 함께 보면 그 틈이 없다.
			
			if (!s.closing || s.io_count.load() != 0) {
				return;		// 아직 지우면 안 된다
			}

			core::logf("[INFO] #%llu removed %s\n",
				static_cast<unsigned long long>(s.id), s.peer_text);

			doomed = std::move(it->second);   // unique_ptr 을 옮겨서 소유권을 가져온다
			sessions_.erase(it);                 // 맵에서 뺀다 (여기선 아직 delete 안 됨)
		}

		// doomed 가 여기서 소멸하며 Session 이 delete 된다. 락 밖이다.
		//   소멸자를 락 안에서 돌리면, 나중에 소멸자가 무거워질 때 그 시간만큼
		//   다른 워커가 맵에 못 들어온다. 락은 짧게 잡는 게 원칙이다.
	}

	// 홀드 하나를 반납하고, 지워도 되는 상태면 지운다.
	//
	// io_count 는 커널 I/O 만 세는 것이 아니라 이 세션을 아직 붙들고 있는 비동기 작업
	// 수다. 커널이 붙들든 큐에 든 Job 이 붙들든 「아직 지우면 안 된다」는 사실은 같아서
	// 같은 카운터로 센다. 이름은 hold_count 쪽이 맞다.
	//
	// 부르고 나면 session 을 만지면 안 된다. 여기서 지워졌을 수 있다.
	void IocpServer::release_io(Session* session) {
		const int left = session->io_count.fetch_sub(1) - 1;

		// 음수면 발행/반납의 짝이 깨진 것이다. 「터지기 전에」 여기서 잡힌다.
		//   단 assert 는 NDEBUG 에서 사라진다. 개발 중 자기검사지 라이브 방어가 아니다.
		assert(left >= 0 && "io_count 가 음수 — 발행/반납의 증감 짝이 깨졌다");

		// 0 이 아니면 여기서 끝낸다. 이 한 줄이 프레임당 전역 락을 없앤다.
		//
		// 한때 무조건 try_remove_session 을 불렀는데, 그 함수는 첫 줄에서 세션 맵 락을
		// 잡는다. release_io 는 수신·송신 완료, 프레임 Job, DB 응답, 존 이동 Job 에서
		// 전부 불리므로 프레임 하나당 전역 락을 두 번 넘게 잡고 있었다. 게다가 그 락의
		// 대부분은 헛수고였다 — 잡고 find 한 뒤 홀드가 남았다고 돌아섰다.
		//
		// 걸러도 되는 근거는 삭제 조건이 io_count == 0 이라는 것이다. 지울 자격이 있는
		// 스레드는 카운트를 0 으로 만든 하나뿐이고 fetch_sub 는 원자적이다. 0 이 됐지만
		// 아직 closing 이 아닌 경우는 try_remove_session 이 받아서 그냥 돌아온다.
		if (left != 0) {
			return;
		}

		try_remove_session(session);
	}

	// 유휴 스윕 — 말이 끊긴 지 오래된 세션을 끊는다.
	//
	// 2단이다. 락 안에서 후보만 모으고 락 밖에서 끊는다. 한 단으로 하면
	// close_session → session_gone_ → 위층 → try_remove_session 경로가 세션 맵 락을
	// 다시 잡아 재귀 데드락이 난다. recursive_mutex 는 답이 아니다 — 락 안에서 위층
	// 콜백을 부른다는 진짜 문제를 덮을 뿐이고, 위층이 무엇을 하는지 net 은 모른다.
	//
	// 락을 놓는 순간 후보 포인터가 죽을 수 있으므로(다른 워커가 지운다) 락 안에서
	// 홀드를 올려 둔다. 끊고 나서 반납하고, 지우는 판단은 거기 한 곳에서만 한다.
	void IocpServer::sweep_loop() {
		// 루프 밖에 둔다. 안에 두면 주기마다 vector 를 새로 할당하는데, 대부분의
		// 주기에 후보가 0 이라 그 할당이 전부 헛일이다.
		std::vector<Session*> doomed;

		const uint64_t limit_ms = static_cast<uint64_t>(idle_timeout_sec_) * 1000ULL;

		for (;;) {
			{
				std::unique_lock<std::mutex> lock(sweep_mutex_);
				sweep_cv_.wait_for(lock,
					std::chrono::seconds(sweep_interval_sec_),
					[this] { return !running_.load(); });
			}
			if (!running_) {
				break;
			}

			const uint64_t now = GetTickCount64();

			doomed.clear();
			{
				std::lock_guard<std::mutex> lock(sessions_mutex_);
				for (const auto& entry : sessions_) {
					Session& s = *entry.second;

					// 이미 닫히는 중이면 놔둔다 — 끊을 것을 또 끊는 일이고,
					// close_session 이 어차피 한 번만 통과시킨다.
					if (s.closing.load()) {
						continue;
					}

					// 부호 없는 뺄셈이지만 안전하다 — GetTickCount64 는 단조라
					//   last 가 now 보다 클 수 없다. (32비트 GetTickCount 였다면
					//   49.7일마다 한 바퀴 돌아서 이 뺄셈이 조용히 틀렸다)
					const uint64_t last =
						s.last_recv_ms.load(std::memory_order_relaxed);
					if (now - last < limit_ms) {
						continue;
					}

					s.io_count.fetch_add(1);   // 락 안에서 올린다. 이게 전부다
					doomed.push_back(&s);
				}
			}

			// ── 락 밖 ────────────────────────────────────────────────────────
			for (Session* s : doomed) {
				core::logf("[INFO] #%llu idle %llus — closing %s\n",
					static_cast<unsigned long long>(s->id),
					static_cast<unsigned long long>(
						(now - s->last_recv_ms.load(std::memory_order_relaxed)) / 1000ULL),
					s->peer_text);

				close_session(*s);
				idle_kicked_.fetch_add(1, std::memory_order_relaxed);
				release_io(s);
				// 이 줄 아래에서 s 를 만지면 안 된다. 이미 지워졌을 수 있다.
			}
		}
	}


	//  누적 버퍼에서 완성된 프레임을 「있는 대로 전부」 꺼낸다.
	//
	//  루프인 이유 — 한 번의 완료에 프레임이 여러 개 들어올 수 있다.
	//    TCP 에는 경계가 없으니 당연하다. 하나만 꺼내고 나가면 나머지는 다음 완료가
	//    올 때까지 묶여 있게 되고, 상대가 침묵하면 영영 처리되지 않는다.
	//
	//  false = 이 세션은 끊어야 한다
	bool IocpServer::drain_frames(Session& session) {
		RecvBuffer& rb = session.recv_buf;

		for (;;) {
			const int avail = rb.pending();
			if (avail <= 0) {
				break;
			}

			// 어디서 자를지는 net 이 모른다. 위층에 묻는다.
			//   sizer 가 없으면 통째로 한 덩어리로 올린다.
			const int frame_len = frame_sizer_ ? frame_sizer_(rb.head(), avail) : avail;

			if (frame_len == 0) {
				break;                       // 아직 부족하다. 다음 완료를 기다린다
			}
			if (frame_len < 0) {
				core::logf("[WARN] #%llu protocol violation — closing\n",
					static_cast<unsigned long long>(session.id));
				return false;
			}

			// 위층이 준 값도 검사한다.
			//   관통 주제는 「상대가 보낸 값을 믿지 않는다」인데, 실은 한 칸 더 넓다 —
			//   「다른 계층이 준 값도 믿지 않는다」. 위층 버그로 avail 보다 큰 값이 오면
			//   여기가 그대로 버퍼 밖 읽기가 된다.
			if (frame_len > avail) {
				core::logf("[WARN] #%llu sizer returned %d > avail %d\n",
					static_cast<unsigned long long>(session.id), frame_len, avail);
				return false;
			}

			// 부르지 않고 큐에 넣는다. 이 한 줄의 차이로 I/O 워커가 로직이 끝나기를
			// 기다리지 않는다.
			//
			// 프레임을 복사하는 것은 rb.head() 가 누적 버퍼 안이라서다. 이 함수가 끝나면
			// 워커는 곧바로 post_recv 를 걸고 커널이 그 자리를 덮어쓴다. 동기 호출일 땐
			// 공짜였던 것이 스레드 경계를 넘는 순간 복사가 된다 — 비동기라 빨라지는 게
			// 아니라 대가가 생기는 것이다.
			//
			// 버퍼는 풀에서 빌린다. vector 를 새로 만들면 프레임마다 힙 할당이 붙는데,
			// 재보니 프레임당 2회 중 절반이 이 줄이었다. 고갈이면 기다리지 않고 거절한다 —
			// 여기서 막히면 워커와 로직을 분리한 이유가 통째로 무너진다. 그래서 풀 크기가
			// 곧 Job 큐의 상한이고, 상한을 따로 셀 필요가 없다.
			char* buf = frame_pool_.acquire();
			if (buf == nullptr) {
				core::logf("[WARN] #%llu frame pool exhausted — closing\n",
					static_cast<unsigned long long>(session.id));
				return false;
			}
			std::memcpy(buf, rb.head(), static_cast<size_t>(frame_len));

			// 큐에 들어가는 순간부터 이 세션은 Job 이 붙들고 있다. 커널이 붙들든 Job 이
			// 붙들든 「아직 지우면 안 된다」는 같아서 같은 카운터를 쓴다. 올리는 시점은
			// push 전이어야 한다 — 뒤에 올리면 그 틈에 로직 스레드가 Job 을 먼저 끝낸다.
			session.io_count.fetch_add(1);

			Session* sp = &session;

			// 싱크가 없으면 이 Job 은 갈 데가 없다. 홀드를 되돌리고 끊는다.
			//
			// 캡처는 32B 다 — this + Session* + char* + int(패딩 포함). placement 를
			// 실어 보내던 시절엔 40B 였다(uint64 한 칸) — 이제 이 세션이 어느 존인지는
			// 실행 시점에 session.zone 을 직접 읽으므로 같이 실어 보낼 값이 없다.
			// sizeof(std::function)=64B 는 그릇 크기지 캡처 한계가 아니다. 타입 소거용
			// vptr 몫이 먼저 빠지므로, 둘을 같은 것으로 읽으면 남은 여유를 세 배로
			// 잘못 센다. 캡처를 늘릴 땐 다시 잴 것.
			bool queued = false;
			if (job_sink_) {
				queued = job_sink_(session,
					[this, sp, buf, frame_len]() {
					bool keep = true;
					if (recv_handler_) {
						keep = recv_handler_(*sp, buf, frame_len);
					}
					if (!keep) {
						close_session(*sp);
					}

					// 버퍼 반납이 먼저다. release_io 는 이 세션을 지울 수 있고,
					//   그 뒤로는 sp 를 만지면 안 된다.
					//   버퍼는 세션과 무관한 자원이라 순서만 지키면 된다.
					frame_pool_.release(buf);
					release_io(sp);
				});
			}

			if (!queued) {
				frame_pool_.release(buf);
				release_io(sp);
				return false;
			}

			rb.read_pos += frame_len;
		}
		return true;
	}

	bool IocpServer::send_frame(Session& session, const char* data, int len) {
		const SendChunk one{ data, len };
		return send_chunks(session, &one, 1);
	}

	bool IocpServer::send_chunks(Session& session, const SendChunk* chunks, int count) {
		if (count <= 0) {
			return true;
		}
		if (session.closing) {
			return false;
		}

		size_t total = 0;
		for (int i = 0; i < count; ++i) {
			total += static_cast<size_t>(chunks[i].len);
		}
		if (total == 0) {
			return true;
		}

		SendBuffer& sb = session.send_buf;
		bool need_post = false;
		bool overflowed = false;
		int  streak = 0;   // overflowed 일 때만 의미가 있다 — 락 밖에서 보려고 복사해 둔다

		{
			std::lock_guard<std::mutex> lock(sb.mutex);

			// 닫는 주체가 이제 둘이다 — 연속 send_overflow_limit_ 회에 닿으면
			// 이 함수가 스스로 닫는다(락 밖에서, 아래). 아직 임계 전이면 여기서는
			// 아무도 안 닫는다 — false 를 돌려주는 것까지고 그 다음은 호출자가
			// 정한다. 요청 처리 경로는 그 값을 받아 끊지만 브로드캐스트 경로는
			// 버린다.
			//
			// 한때 이 로그 줄이 「— closing」을 찍었는데 그때는 실제로 아무도
			// 안 닫고 있었다. 코드가 하는 일과 로그가 하는 말이 달랐고, 그건
			// 오타보다 나쁘다 — 운영자가 끊겼겠거니 하고 세션 누수를 다른 데서
			// 찾게 된다. 로그는 내가 방금 한 일만 말해야 한다 — 그래서 실제로
			// 닫을 때는 아래에서 그 사실만 따로 찍는다.
			if (sb.back.size() + total > kMaxSendQueue) {
				core::logf("[WARN] #%llu send queue full (%zu B) — 이 메시지를 버린다\n",
					static_cast<unsigned long long>(session.id), sb.back.size());
				sb.overflow_streak++;
				streak = sb.overflow_streak;
				overflowed = true;
			} else {
				// 수위 경고 — 넘치기 전에 미리 알아채려는 관측용 신호다. 세션당
				// 한 번만 찍는다(에지 트리거). 넘친 뒤에도 다시 리셋하지 않는
				// 이유는, 알고 싶은 사실이 "지금 이 순간의 수위"가 아니라
				// "이 세션이 한 번이라도 그 수위를 넘은 적이 있다"이기 때문이다.
				if (!sb.queue_warned && sb.back.size() + total > (kMaxSendQueue * 3 / 4)) {
					sb.queue_warned = true;
					core::logf("[WARN] #%llu send queue %zuB — 상한(%zuB)의 3/4 를 넘었다\n",
						static_cast<unsigned long long>(session.id),
						sb.back.size() + total, kMaxSendQueue);
				}

				sb.overflow_streak = 0;   // 성공 큐잉 — "연속" 계약의 리셋

				// 조각들을 「한 락 안에서」 이어 붙인다.
				//   나눠서 넣으면 그 사이에 다른 워커가 back 을 가져가 버려서,
				//   한 메시지가 두 번의 WSASend 로 쪼개진다. 시스템 콜이 두 배가 된다.
				for (int i = 0; i < count; ++i) {
					sb.back.insert(sb.back.end(), chunks[i].data, chunks[i].data + chunks[i].len);
				}

				if (!sb.sending) {
					sb.sending = true;      // 발행할 권리를 락 안에서 선점한다
					need_post = true;
				}
			}
		}

		if (overflowed) {
			// 락을 놓은 뒤에 닫는다 — close_session 이 session_gone_ 콜백으로
			// 위층까지 올라가는 동안 sb.mutex 를 쥐고 있을 이유가 없다
			// (sweep_loop 가 sessions_mutex_ 를 락 밖에서 닫는 것과 같은 이유).
			//
			// close_session 을 여기서 스스로 부르는 것이 sweep_loop·close_by_id
			// 에 이어 이 파일의 세 번째 호출부인데, 저 둘과 다르게 새 홀드를
			// 세우지도(io_count.fetch_add) 반납하지도(release_io) 않는다.
			// 안전한 이유는 둘이다. ① close_session 자신이 closing.exchange(true)
			// 로 멱등이라 이 세션이 sweep·S2S(close_by_id)·여기 중 어디서 먼저
			// 닫히든 실제 정리는 한 번만 돈다. ② 진짜 근거는 「send_chunks 를
			// 부르는 시점에 이 session 은 호출자가 이미 홀드를 쥐고 있다」는
			// 것이다 — 자기 응답 송신은 그 프레임을 처리 중인 Job 자신의
			// 홀드고, 브로드캐스트·귓속말 대상은 snapshot_zone/snapshot_all/
			// find_acquire_by_player_id 가 반환 전에 세운 acquire_hold 다. 그
			// 홀드가 이 함수가 도는 동안 session 을 살려 두므로 release_io
			// 없이 close_session 만 불러도 UAF 가 안 난다 — 반납은 호출자가
			// 원래 하던 대로 한다. 홀드 없이 send_chunks 를 부르는 새 경로가
			// 생기면 이 전제가 깨진다(CODING_RULES §4-b 짝 API 표에 이 계약을
			// 등재해 둔다).
			// == 다. >= 로 하면 임계 직후의 창(닫히기 전까지) 동안 이 세션에
			// 대해 두 워커가 겹쳐 넘침을 겪을 경우 둘 다 이 분기로 들어와
			// send_full_kicked_ 를 두 번 올릴 수 있다 — streak 는 락 안에서
			// 증가시킨 값의 복사본이라 두 워커가 경합해도 그 값이 정확히
			// limit 인 워커는 하나뿐이다(하나는 limit-1 이하 이전에, 다른
			// 하나는 limit+1 이상 이후에 본다). close_session 자체는 멱등이라
			// 실해는 없지만, 그러면 지표(send_full_kicked)가 실제보다 부풀어
			// 거짓말을 하게 된다.
			// send_overflow_limit_ <= 0 이면 이 갈래 자체를 안 탄다 — streak
			// 증가·수위 경고는 위에서 이미 끝났으니 관측은 그대로 공짜로
			// 남는다. 끄는 이유는 iocp_server.h 의 kDefaultSendOverflowLimit
			// 주석 참조 — 락을 쥔 채 송신하는 서버(세션 서버)에서 이 킥이
			// 발화하면 session_gone_ 이 같은 락을 재획득해 자기 데드락이 난다.
			if (send_overflow_limit_ > 0 && streak == send_overflow_limit_) {
				core::logf("[WARN] #%llu send queue overflow %d 회 연속 — closing\n",
					static_cast<unsigned long long>(session.id), streak);
				close_session(session);
				send_full_kicked_.fetch_add(1, std::memory_order_relaxed);
			}
			return false;
		}

		if (need_post) {
			return post_send(session);
		}
		return true;
	}

	bool IocpServer::post_send(Session& session) {
		SendBuffer& sb = session.send_buf;
		int batch = 0;

		{
			std::lock_guard<std::mutex> lock(sb.mutex);

			// front 를 다 보냈으면 back 을 통째로 가져온다. 여기가 배치가 생기는 지점이다.
			//    발행 중에 쌓인 것이 이 한 줄로 한 덩어리가 된다.
			//    swap 이라 복사가 없다 — 벡터 두 개의 내부 포인터만 맞바꾼다.
			if (sb.sent >= static_cast<int>(sb.front.size())) {
				sb.front.clear();
				sb.sent = 0;
				sb.front.swap(sb.back);
			}

			if (sb.front.empty()) {
				sb.sending = false;          // 보낼 게 없다. 발행 권리를 내려놓는다
				return true;
			}
			batch = static_cast<int>(sb.front.size()) - sb.sent;
		}

		//  불변식 — sending == true 인 동안 front 와 sent 는
		//     「발행 권리를 가진 스레드」만 만진다. back 만 mutex 로 보호한다.
		//     그래서 아래를 락 밖에서 만들어도 안전하다.
		//
		//  그리고 이 불변식이 지키는 게 하나 더 있다 —
		//    커널은 front.data() 에서 읽어 간다. 발행 중에 front 에 push_back 을 하면
		//    벡터가 재할당되며 주소가 옮겨가고, 커널은 옮기기 전 주소에서 계속 읽는다.
		//    세션 주소 · compact 에 이어 「커널에 맡긴 메모리는 못 움직인다」가
		//    세 번째로 나오는 자리다.
		IoContext& ctx = session.send_ctx;
		ctx.ov = OVERLAPPED{};
		ctx.op = IoOp::Send;                 // 완료 때 이걸 보고 분기한다
		ctx.wsabuf.buf = sb.front.data() + sb.sent;
		ctx.wsabuf.len = static_cast<ULONG>(batch);

		session.io_count.fetch_add(1);               // 발행 직전에 올린다

		// WSARecv 와 다르다 — 다섯 번째 인자(flags)가 포인터가 아니라 값이다.
		//   받는 쪽은 커널이 플래그를 돌려줄 게 있어서 포인터고, 보내는 쪽은 없다.
		int rc = WSASend(session.socket, &ctx.wsabuf, 1,
			nullptr, 0, &ctx.ov, nullptr);

		if (rc == SOCKET_ERROR) {
			const int err = WSAGetLastError();
			if (err != WSA_IO_PENDING) {
				session.io_count.fetch_sub(1);

				// 실패했으면 발행 권리를 반드시 돌려놓는다.
				//   안 그러면 sending 이 true 로 박혀서 이 세션은 두 번 다시
				//   아무것도 못 보낸다. 조용히 먹통이 되는 종류의 버그다.
				{
					std::lock_guard<std::mutex> lock(sb.mutex);
					sb.sending = false;
				}
				core::logf("[FAIL] WSASend #%llu (err=%d)\n",
					static_cast<unsigned long long>(session.id), err);
				return false;
			}
		}

		// 배치가 실제로 뭉치는지 눈으로 본다.
		if (core::log_trace_frames())
		core::logf("[SEND] #%llu batch=%dB\n",
			static_cast<unsigned long long>(session.id), batch);
		return true;
	}
		
}	// namespace net

