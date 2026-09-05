//  client/tcp_client.h — 블로킹 Winsock 소켓 한 겹
//
//  블로킹 connect/send/recv 위에 select() 타임아웃만 얹는다(T015-plan.md
//  결정 3). SO_RCVTIMEO 를 쓰지 않는 이유 — MS Learn SOL_SOCKET 옵션 표:
//  "If a blocking receive call times out, the connection is in an
//  indeterminate state and should be closed." cmd_send.cpp 의 hold/ping
//  루프는 타임아웃 뒤에도 같은 소켓을 계속 쓰므로 그 문구와 양립하지 않는다.
//  그래서 수신 대기는 select(readfds, timeval) 로 하고 — MS Learn select:
//  "zero if the time limit expired" — 준비된 뒤에만 recv 를 부른다.
//
//  connect 에 타임아웃 인자를 두지 않은 이유 — 이 도구는 loopback
//  (127.0.0.1) 전용이다. 접속은 즉시 성공하거나 RST 로 즉시 실패하고,
//  리스너가 없는 포트로의 실패는 이 환경에서 약 2~2.6초 걸린다
//  (session.ps1:1-22 헤더의 실측 — 원인 불명·방화벽 추정). 상한은 OS 의
//  connect 재시도 한도(수십 초)이고 무한 대기가 아니므로, 원격 지연을
//  다루는 비차단+select(writefds) 기법을 지금 들이는 것은 §18-7 "지금
//  손해 없음"이다.
//
//  close_rst() 의 SO_LINGER{l_onoff=1, l_linger=0} — MS Learn closesocket:
//  "If the l_onoff member of the linger structure is nonzero and the
//  l_linger member is a zero timeout interval on a blocking socket, then a
//  call to closesocket will reset the connection. The socket will not go to
//  the TIME_WAIT state." --drop-after-send 시나리오(cmd_send.cpp)가 이
//  경로를 쓴다.
//
//  send_all 이 실패하면 WSAGetLastError() 를 last_send_error_ 에 남긴다 —
//  MS Learn send() 오류표: WSAECONNRESET "The virtual circuit was reset by
//  the remote side executing a hard or abortive close." · WSAECONNABORTED
//  "The virtual circuit was terminated due to a time-out or other failure.
//  The application should close the socket as it is no longer usable."
//  (2026-09-05 lead 열람 원문) — 둘 다 "상대가 이미 끊었다"로 읽는다.
//  cmd_send.cpp 가 --repeat 를 여러 번의 send_all 로 나눠 부르는 raw 모드
//  에서, 서버가 첫 유닛만 보고 절단(RST)하면 두 번째 이후의 send_all 이
//  이 값으로 실패할 수 있다 — recv 쪽 kClosed 와 같은 "상대가 끊었다"는
//  사실인데 send 쪽에서는 bool 하나(성공/실패)만 돌려주면 그 사실이
//  사라진다. last_send_error() 가 그 구분을 호출자에게 되돌려준다.

#pragma once

#include "frame_codec.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace client {

	// Winsock 수명 — 프로세스에 정확히 1개, main() 의 최상위 스코프.
	//   ⚠️ 이 "정확히 1개·정확히 1회 WSACleanup" 은 기계 게이트가 없다 —
	//   main.cpp 코드 정독으로 지킨다(T015-impl.md §5 끝 문단).
	class WinsockScope {
	public:
		WinsockScope();
		~WinsockScope();

		WinsockScope(const WinsockScope&) = delete;
		WinsockScope& operator=(const WinsockScope&) = delete;

		bool ok() const { return ok_; }

	private:
		bool ok_;
	};

	// recv_some/recv_exact 의 결과 — kTimeout 과 kClosed 를 반드시 구분한다.
	//   kTimeout: select 가 시간 안에 아무것도 못 봤다(상대는 아직 살아있을 수 있다).
	//   kClosed : recv()==0(정상 종료) 또는 WSAECONNRESET/WSAECONNABORTED(강제 종료).
	enum class RecvResult {
		kData,
		kTimeout,
		kClosed,
		kError,
	};

	class TcpClient {
	public:
		TcpClient();
		~TcpClient();

		TcpClient(const TcpClient&) = delete;
		TcpClient& operator=(const TcpClient&) = delete;

		// 블로킹 getaddrinfo(AF_INET) + connect — 타임아웃 인자 없음
		// (위 파일 첫머리 주석 참조).
		bool connect(const std::string& host, uint16_t port);

		// TCP_NODELAY — optval 은 DWORD(MS Learn IPPROTO_TCP 옵션 표).
		bool set_nodelay(bool enable);

		// 보낸 합이 n 이 될 때까지 반복한다. 실패하면 last_send_error() 로
		// WSAGetLastError() 값을 조회할 수 있다(성공한 직전 호출은 0).
		bool send_all(const uint8_t* p, size_t n);

		// send_all 의 마지막 실패 사유 — WSAECONNRESET/WSAECONNABORTED 면
		// "상대가 이미 끊었다"(위 파일 첫머리 주석 참조).
		int last_send_error() const { return last_send_error_; }

		// select(readfds, timeval) → 0 이면 kTimeout, 준비되면 recv 한 번.
		RecvResult recv_some(uint8_t* buf, size_t cap, size_t& got, int timeout_ms);

		// recv_some 을 반복해 n 바이트를 다 채운다 — 누적 대기 상한은
		// timeout_ms(각 recv_some 호출마다 새로 주는 것이 아니다).
		RecvResult recv_exact(uint8_t* buf, size_t n, int timeout_ms);

		// shutdown(SD_SEND) → closesocket. 정상 종료 경로.
		void close_graceful();

		// SO_LINGER{1,0} → closesocket. RST 로 즉시 끊는다.
		void close_rst();

	private:
		SOCKET sock_;
		int last_send_error_;
	};

	// read_frame — 4B 헤더 recv_exact → decode_header → body_size 가
	//   proto::kMaxBodySize 를 넘으면 규약 위반으로 kError → body recv_exact.
	//   cmd_flow.cpp 의 로그인/Enter/Echo/Ping 네 단계와 cmd_send.cpp 의
	//   hold/ping 루프가 공유한다 — 두 호출부가 각자 헤더를 읽고 상한을
	//   검사하면 한쪽만 검사를 깜빡했을 때(실제로 hold 루프가 그랬다) 그
	//   결함이 오래 안 보인다. kClosed/kTimeout 은 recv_exact 그대로,
	//   규약 위반은 RecvResult 에 전용 값이 없어 kError 로 묶는다.
	RecvResult read_frame(TcpClient& client, Frame& out, int timeout_ms);

}	// namespace client
