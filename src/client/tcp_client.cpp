//  client/tcp_client.cpp — WinsockScope · TcpClient 구현
//
//  recv_exact 의 누적 타임아웃은 GetTickCount64 로 잰다 — QueryPerformanceCounter
//  가 더 정밀하지만, 이 클라의 타임아웃 단위는 어차피 ms(--timeout 인자)라
//  1ms 미만의 정밀도가 필요 없다. GetTickCount64 를 쓰려면 <windows.h> 가
//  필요한데, tcp_client.h(공개 헤더)가 아니라 이 .cpp 에서만 include 한다
//  — 헤더가 끌고 다니는 매크로(min/max 등)를 줄이는 편이 낫다(common.props
//  가 NOMINMAX 를 정의하지만, 그건 이 파일에도 그대로 적용된다).
//  winsock2.h 를 먼저 include 한 뒤 windows.h 를 여는 순서를 지킨다 —
//  거꾸로 하면 windows.h 가 winsock1(winsock.h)을 먼저 끌어들여
//  "WinSock.h already included" 컴파일 에러가 난다.

#include "tcp_client.h"

#include <windows.h>

#include <cstdio>

namespace client {

	namespace {

		// 한 번의 send()/recv() 호출에 넘기는 상한. Winsock 의 send/recv 는
		// len 을 int 로 받으므로, size_t 값을 그대로 캐스팅하면 큰 값에서
		// 부호가 뒤집힐 수 있다 — 이 도구가 다루는 프레임은 kMaxBodySize
		// (4096) 근처라 이 상한에 절대 안 걸리지만, 방어적으로 잘라 둔다.
		constexpr size_t kMaxIoChunk = 65536;

	}	// namespace

	WinsockScope::WinsockScope() : ok_(false) {
		WSADATA wsa{};
		// 2.2 를 요청하는 이유 — src/main.cpp:54-55 선례와 같다(Overlapped
		// I/O 는 서버 쪽 근거라 이 클라와 무관하지만, 버전 요청 자체는
		// 저장소 관례를 그대로 따른다).
		ok_ = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
	}

	WinsockScope::~WinsockScope() {
		// WSAStartup 이 실패했으면 WSACleanup 을 부르지 않는다 — 시작하지
		// 않은 것을 정리하면 안 된다(src/main.cpp 의 실패 경로와 같은 결).
		if (ok_) {
			WSACleanup();
		}
	}

	TcpClient::TcpClient() : sock_(INVALID_SOCKET), last_send_error_(0) {
	}

	TcpClient::~TcpClient() {
		// 소멸자에서 열려 있으면 close_graceful — 모든 실패 경로가 반드시
		// 여기를 거치므로 cmd_send.cpp/cmd_flow.cpp 가 각 갈래마다 손으로
		// close 를 부르지 않아도 소켓이 샌다.
		if (sock_ != INVALID_SOCKET) {
			close_graceful();
		}
	}

	bool TcpClient::connect(const std::string& host, uint16_t port) {
		// 같은 인스턴스에 다시 connect 를 부르면 먼저 열린 소켓을 정리한다
		// — 지금 호출부(cmd_send.cpp·cmd_flow.cpp)는 인스턴스마다 connect
		// 를 한 번만 부르지만, 재호출을 막지 않는 이상 이건 클래스 계약이지
		// 호출부 사정이 아니다. 안 닫으면 이전 소켓 핸들이 샌다.
		if (sock_ != INVALID_SOCKET) {
			close_graceful();
		}

		addrinfo hints{};
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;

		char port_str[8]{};
		std::snprintf(port_str, sizeof(port_str), "%u", static_cast<unsigned>(port));

		addrinfo* resolved = nullptr;
		const int gai_err = getaddrinfo(host.c_str(), port_str, &hints, &resolved);
		if (gai_err != 0) {
			std::printf("connect: getaddrinfo 실패 (%d)\n", gai_err);
			return false;
		}

		SOCKET sock = ::socket(resolved->ai_family, resolved->ai_socktype, resolved->ai_protocol);
		if (sock == INVALID_SOCKET) {
			std::printf("connect: socket 실패 (WSAGetLastError=%d)\n", WSAGetLastError());
			freeaddrinfo(resolved);
			return false;
		}

		// 블로킹 connect — 타임아웃 인자를 두지 않은 이유는 tcp_client.h
		// 머리말 참조(loopback 전용, 실패는 이 환경에서 약 2~2.6초).
		const int rc = ::connect(sock, resolved->ai_addr, static_cast<int>(resolved->ai_addrlen));
		freeaddrinfo(resolved);
		if (rc == SOCKET_ERROR) {
			std::printf("connect: 실패 (WSAGetLastError=%d)\n", WSAGetLastError());
			closesocket(sock);
			return false;
		}

		sock_ = sock;
		return true;
	}

	bool TcpClient::set_nodelay(bool enable) {
		if (sock_ == INVALID_SOCKET) {
			return false;
		}
		// MS Learn IPPROTO_TCP 옵션 표 — TCP_NODELAY 의 optval 은 DWORD 다.
		DWORD value = enable ? 1u : 0u;
		const int rc = setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&value), sizeof(value));
		return rc == 0;
	}

	bool TcpClient::send_all(const uint8_t* p, size_t n) {
		last_send_error_ = 0;
		if (sock_ == INVALID_SOCKET) {
			return false;
		}
		size_t sent_total = 0;
		while (sent_total < n) {
			const size_t remaining = n - sent_total;
			const int chunk = static_cast<int>(remaining < kMaxIoChunk ? remaining : kMaxIoChunk);
			const int sent = ::send(sock_, reinterpret_cast<const char*>(p + sent_total), chunk, 0);
			if (sent == SOCKET_ERROR) {
				// tcp_client.h 머리말 참조 — WSAECONNRESET/WSAECONNABORTED 는
				// "상대가 이미 끊었다"로 읽는다. 호출자(cmd_send.cpp)가 raw
				// 모드에서 --repeat 를 여러 send_all 로 나눠 부를 때, 서버가
				// 첫 유닛만 보고 절단하면 이 값이 그 사실을 들고 나간다.
				last_send_error_ = WSAGetLastError();
				return false;
			}
			sent_total += static_cast<size_t>(sent);
		}
		return true;
	}

	RecvResult TcpClient::recv_some(uint8_t* buf, size_t cap, size_t& got, int timeout_ms) {
		got = 0;
		if (sock_ == INVALID_SOCKET) {
			return RecvResult::kError;
		}

		fd_set read_set;
		FD_ZERO(&read_set);
		FD_SET(sock_, &read_set);

		const int wait_ms = timeout_ms < 0 ? 0 : timeout_ms;
		timeval tv{};
		tv.tv_sec = wait_ms / 1000;
		tv.tv_usec = (wait_ms % 1000) * 1000;

		// select 의 첫 인자(nfds)는 Winsock 에서 무시된다 — fd_set 이
		// 소켓 핸들을 직접 들고 있어 리눅스처럼 "가장 큰 fd + 1" 이 필요
		// 없다(MS Learn select: 이 인자는 이식성을 위해서만 존재한다).
		const int sel = select(0, &read_set, nullptr, nullptr, &tv);
		if (sel == 0) {
			return RecvResult::kTimeout;
		}
		if (sel == SOCKET_ERROR) {
			return RecvResult::kError;
		}

		const int cap_i = static_cast<int>(cap < kMaxIoChunk ? cap : kMaxIoChunk);
		const int n = ::recv(sock_, reinterpret_cast<char*>(buf), cap_i, 0);
		if (n == 0) {
			// MS Learn recv: "If the connection has been gracefully closed,
			// the return value is zero."
			return RecvResult::kClosed;
		}
		if (n == SOCKET_ERROR) {
			const int err = WSAGetLastError();
			if (err == WSAECONNRESET || err == WSAECONNABORTED) {
				return RecvResult::kClosed;
			}
			return RecvResult::kError;
		}

		got = static_cast<size_t>(n);
		return RecvResult::kData;
	}

	RecvResult TcpClient::recv_exact(uint8_t* buf, size_t n, int timeout_ms) {
		// 누적 대기 상한 = timeout_ms — recv_some 을 반복할 때마다 새로
		// timeout_ms 를 주면 "매번 타임아웃 직전까지 기다리는" 조각들이
		// 이어져 실제 대기 시간이 timeout_ms 의 배수로 늘어난다. 그래서
		// 절대 마감(deadline)을 한 번만 잡고 매 호출에는 남은 시간만 준다.
		const ULONGLONG start = GetTickCount64();
		const ULONGLONG budget = static_cast<ULONGLONG>(timeout_ms < 0 ? 0 : timeout_ms);
		const ULONGLONG deadline = start + budget;

		// 마감에 닿았어도 poll 은 한 번 한다 — timeout_ms=0 이면 deadline==start
		//   라 첫 반복부터 "마감"인데, 그때 recv_some 을 한 번도 안 부르고
		//   kTimeout 을 내면 커널 버퍼에 이미 와 있는 데이터도 못 읽는다.
		//   recv_some 은 timeout 0 을 "select 를 tv={0,0} 으로 1회 poll" 로
		//   다루므로 이 함수도 같은 뜻이어야 한다(11단계 correctness MED —
		//   --timeout 은 0 을 허용하는 값 범위다). 마감 뒤 poll 은 정확히 1회.
		size_t got_total = 0;
		bool polled_at_deadline = false;
		while (got_total < n) {
			const ULONGLONG now = GetTickCount64();
			int remaining_ms = 0;
			if (now < deadline) {
				remaining_ms = static_cast<int>(deadline - now);
			} else {
				if (polled_at_deadline) {
					return RecvResult::kTimeout;
				}
				polled_at_deadline = true;
			}

			size_t got = 0;
			const RecvResult r = recv_some(buf + got_total, n - got_total, got, remaining_ms);
			if (r != RecvResult::kData) {
				return r;
			}
			got_total += got;
		}
		return RecvResult::kData;
	}

	void TcpClient::close_graceful() {
		if (sock_ != INVALID_SOCKET) {
			// 이쪽에서 더 보낼 것이 없다는 정상 종료 신호 — TIME_WAIT 은
			// 이 클라(짧게 살고 죽는 CLI 프로세스) 입장에서 문제될 게 없다.
			shutdown(sock_, SD_SEND);
			closesocket(sock_);
			sock_ = INVALID_SOCKET;
		}
	}

	void TcpClient::close_rst() {
		if (sock_ != INVALID_SOCKET) {
			// MS Learn closesocket — l_onoff!=0 && l_linger==0 이면 FIN 이
			// 아니라 RST 로 끊는다(TIME_WAIT 을 거치지 않는다).
			linger lin{};
			lin.l_onoff = 1;
			lin.l_linger = 0;
			setsockopt(sock_, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&lin), sizeof(lin));
			closesocket(sock_);
			sock_ = INVALID_SOCKET;
		}
	}

	RecvResult read_frame(TcpClient& client, Frame& out, int timeout_ms) {
		uint8_t header_buf[proto::kHeaderSize];
		RecvResult r = client.recv_exact(header_buf, proto::kHeaderSize, timeout_ms);
		if (r != RecvResult::kData) {
			return r;
		}

		const proto::PacketHeader header = proto::decode_header(header_buf);
		if (header.body_size > proto::kMaxBodySize) {
			return RecvResult::kError;
		}

		out.id = header.msg_id;
		out.body.assign(header.body_size, 0);
		if (header.body_size > 0) {
			r = client.recv_exact(out.body.data(), header.body_size, timeout_ms);
			if (r != RecvResult::kData) {
				return r;
			}
		}
		return RecvResult::kData;
	}

}	// namespace client
