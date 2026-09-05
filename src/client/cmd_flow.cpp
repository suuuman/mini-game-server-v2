//  client/cmd_flow.cpp — `client flow` (로그인→Enter→Echo→Ping, T015-impl.md §5)
//
//  정본 시퀀스는 session.ps1:1537-1550 의 로그인→Enter 체인과 조사 A
//  §7 계약표다 — 여기서 새로 설계한 것은 없다, 기존 PS 하네스가 손으로
//  하던 절차를 그대로 옮긴다.
//
//  각 단계의 판정은 frame_codec.h 의 술어(expect_frame·expect_frame_min·
//  bytes_equal)를 반드시 거친다 — 이 파일 안에서 id·길이·바이트를 직접
//  비교하지 않는다. 술어를 우회해 여기서 직접 비교하면, `client selftest`
//  가 그 비교의 결함(echo 비교를 무력화하거나 pong id 검사를 생략하는
//  뮤턴트)을 대신 잡을 방법이 없어진다 — 정상 서버 앞에서는 flow 가 항상
//  통과하므로 그런 결함이 영구히 초록으로 남는다(frame_codec.h 머리말과
//  같은 근거, 8단계 뮤턴트 MUT6/MUT7 이 이 축을 시험한다).
//
//  ⚠️ Enter 성공 뒤 서버가 먼저 보내는 메시지는 없다(계약 #9 —
//  kZoneMembersNtf 는 kJoinZoneReq 를 보내야만 온다) — 그래서 Enter 응답을
//  받은 즉시 Echo 로 넘어가고, 그 사이에 뭔가 더 오는지 기다리지 않는다.
//
//  read_frame(tcp_client.h/.cpp 공용 함수 — cmd_send.cpp 의 hold/ping
//  루프와 같이 쓴다)이 실패하면(kClosed/kTimeout) 그 단계 이름을 사유에
//  실어 "closed by server at <step>"/"timeout at <step>" 으로 보고한다 —
//  판정 자체가 실패한 경우(expect_frame 계열이 거짓)와 수신 자체가 실패한
//  경우를 사유 문자열로 구분해야 로그만 보고도 어느 쪽인지 알 수 있다.

#include "commands.h"
#include "frame_codec.h"
#include "tcp_client.h"

#include "proto/packet.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace client {

	namespace {

		// read_frame 이 실패했을 때의 공통 판정 — kClosed/kTimeout 은
		// "<step>" 이름을 실어 보고하고, 그 외(규약 위반 등)는 "<step>
		// frame" 으로 묶는다(login/enter 의 expect_frame 계열 실패와 같은
		// 결의 사유 문자열이 되게 한다).
		int fail_at(const char* step, RecvResult why) {
			std::string reason;
			if (why == RecvResult::kClosed) {
				reason = std::string("closed by server at ") + step;
			} else if (why == RecvResult::kTimeout) {
				reason = std::string("timeout at ") + step;
			} else {
				reason = std::string(step) + " frame";
			}
			result_line(false, reason.c_str());
			return static_cast<int>(ExitCode::kFail);
		}

	}	// namespace

	int run_flow(const Args& args) {
		const std::string session_host = args.get("session-host", "127.0.0.1");
		const int64_t session_port_raw = args.get_int("session-port", 9200, 0, 65535);
		const bool has_player = args.has_value("player");
		const uint64_t player_id = args.get_u64("player", 0);
		const uint64_t enter_player = args.get_u64("enter-player", player_id);
		const int64_t timeout64 = args.get_int("timeout", 3000, 0, 600000);
		const int64_t echo_size = args.get_int("echo-size", 8, 0, static_cast<int64_t>(proto::kMaxBodySize));
		const bool no_login = args.has("no-login");
		const std::string village_host = args.get("village-host", "127.0.0.1");
		const int64_t village_port_raw = args.get_int("village-port", 9000, 0, 65535);
		const bool has_expect_login = args.has_value("expect-login-result");
		const int64_t expect_login_result = args.get_int("expect-login-result", 0, 0, 255);
		const bool has_expect_enter = args.has_value("expect-enter-result");
		const int64_t expect_enter_result = args.get_int("expect-enter-result", 0, 0, 255);

		// --player 는 필수 u64 >= 1 이다 — 없거나(has_value==false) 0 이면
		// (기본값 0 과 "명시적으로 0 을 줬다"를 여기서는 구분하지 않는다,
		// 0 은 어차피 유효한 player_id 가 아니다) usage 오류다.
		if (!args.ok() || !has_player || player_id < 1) {
			print_usage();
			return static_cast<int>(ExitCode::kUsage);
		}
		const std::vector<std::string> unknown = args.unknown();
		if (!unknown.empty()) {
			print_usage();
			return static_cast<int>(ExitCode::kUsage);
		}

		const uint16_t session_port = static_cast<uint16_t>(session_port_raw);
		const uint16_t village_port_default = static_cast<uint16_t>(village_port_raw);
		const int timeout_ms = static_cast<int>(timeout64);

		std::string enter_host;
		uint16_t enter_port = 0;

		// ── 1. 로그인 (--no-login 이면 건너뛴다) ──────────────────────
		if (!no_login) {
			TcpClient session_client;
			if (!session_client.connect(session_host, session_port)) {
				result_line(false, "connect");
				return static_cast<int>(ExitCode::kConnect);
			}

			std::vector<uint8_t> login_body;
			put_u64_be(login_body, player_id);
			const std::vector<uint8_t> login_frame_bytes =
				build_frame(proto::MsgId::kSessionLoginReq, login_body.data(), login_body.size());
			if (!session_client.send_all(login_frame_bytes.data(), login_frame_bytes.size())) {
				result_line(false, "send");
				return static_cast<int>(ExitCode::kFail);
			}

			Frame login_frame;
			const RecvResult login_why = read_frame(session_client, login_frame, timeout_ms);
			if (login_why != RecvResult::kData) {
				return fail_at("login", login_why);
			}
			if (!expect_frame_min(login_frame, proto::MsgId::kSessionLoginAck, 5)) {
				result_line(false, "login frame");
				return static_cast<int>(ExitCode::kFail);
			}

			// [result:u8][port:u16 BE][host:str16] — session_router.cpp:327-331 조립의 역.
			const uint8_t login_result = login_frame.body[0];
			const uint16_t login_port = proto::read_u16_be(login_frame.body.data() + 1);
			std::string login_host_str;
			size_t host_consumed = 0;
			if (!decode_str16(login_frame.body.data() + 3, login_frame.body.size() - 3, login_host_str, host_consumed)) {
				result_line(false, "login frame");
				return static_cast<int>(ExitCode::kFail);
			}

			std::printf("login : result=%u host=%s port=%u\n",
				static_cast<unsigned>(login_result), login_host_str.c_str(), static_cast<unsigned>(login_port));

			// 세션 서버는 먼저 끊지 않는다(session_router.cpp:212-333 에
			// close 없음) — 이쪽에서 정상 종료한다.
			session_client.close_graceful();

			if (has_expect_login) {
				if (login_result == static_cast<uint8_t>(expect_login_result)) {
					result_line(true, nullptr);
					return static_cast<int>(ExitCode::kPass);
				}
				result_line(false, ("login result=" + std::to_string(login_result)).c_str());
				return static_cast<int>(ExitCode::kFail);
			}
			if (login_result != proto::kSessionLoginOk) {
				result_line(false, ("login result=" + std::to_string(login_result)).c_str());
				return static_cast<int>(ExitCode::kFail);
			}

			enter_host = login_host_str;
			enter_port = login_port;
		} else {
			enter_host = village_host;
			enter_port = village_port_default;
		}

		// ── 2. Enter ──────────────────────────────────────────────
		TcpClient village_client;
		if (!village_client.connect(enter_host, enter_port)) {
			result_line(false, "connect");
			return static_cast<int>(ExitCode::kConnect);
		}

		std::vector<uint8_t> enter_body;
		put_u64_be(enter_body, enter_player);
		const std::vector<uint8_t> enter_frame_bytes =
			build_frame(proto::MsgId::kEnterReq, enter_body.data(), enter_body.size());
		if (!village_client.send_all(enter_frame_bytes.data(), enter_frame_bytes.size())) {
			result_line(false, "send");
			return static_cast<int>(ExitCode::kFail);
		}

		Frame enter_frame;
		const RecvResult enter_why = read_frame(village_client, enter_frame, timeout_ms);
		if (enter_why != RecvResult::kData) {
			return fail_at("enter", enter_why);
		}
		// [result:u8][player:u64][session:u32] — frame_router.cpp:435-441.
		if (!expect_frame(enter_frame, proto::MsgId::kEnterAck, 13)) {
			result_line(false, "enter frame");
			return static_cast<int>(ExitCode::kFail);
		}

		const uint8_t enter_result = enter_frame.body[0];
		const uint64_t enter_result_player = proto::read_u64_be(enter_frame.body.data() + 1);
		const uint32_t enter_session = proto::read_u32_be(enter_frame.body.data() + 9);
		std::printf("enter : result=%u player=%llu session=%u\n",
			static_cast<unsigned>(enter_result),
			static_cast<unsigned long long>(enter_result_player),
			static_cast<unsigned>(enter_session));

		if (has_expect_enter) {
			if (enter_result == static_cast<uint8_t>(expect_enter_result)) {
				result_line(true, nullptr);
				return static_cast<int>(ExitCode::kPass);
			}
			result_line(false, ("enter result=" + std::to_string(enter_result)).c_str());
			return static_cast<int>(ExitCode::kFail);
		}
		if (enter_result != static_cast<uint8_t>(proto::ResultCode::kOk)) {
			result_line(false, ("enter result=" + std::to_string(enter_result)).c_str());
			return static_cast<int>(ExitCode::kFail);
		}

		// Enter 성공 뒤 서버가 먼저 보내는 메시지는 없다(파일 머리말) —
		// 대기하지 않고 바로 Echo 로 넘어간다.

		// ── 3. Echo ───────────────────────────────────────────────
		const std::vector<uint8_t> echo_body = echo_pattern(static_cast<size_t>(echo_size));
		const std::vector<uint8_t> echo_frame_bytes = build_frame(proto::MsgId::kEchoReq, echo_body.data(), echo_body.size());
		if (!village_client.send_all(echo_frame_bytes.data(), echo_frame_bytes.size())) {
			result_line(false, "send");
			return static_cast<int>(ExitCode::kFail);
		}

		Frame echo_frame;
		const RecvResult echo_why = read_frame(village_client, echo_frame, timeout_ms);
		if (echo_why != RecvResult::kData) {
			return fail_at("echo", echo_why);
		}
		if (!expect_frame(echo_frame, proto::MsgId::kEchoAck, echo_body.size())
			|| !bytes_equal(echo_frame.body, echo_body.data(), echo_body.size())) {
			result_line(false, "echo mismatch");
			return static_cast<int>(ExitCode::kFail);
		}
		std::printf("echo  : OK %lldB\n", static_cast<long long>(echo_size));

		// ── 4. Ping ───────────────────────────────────────────────
		const std::vector<uint8_t> ping_frame_bytes = build_frame(proto::MsgId::kPingReq, nullptr, 0);
		if (!village_client.send_all(ping_frame_bytes.data(), ping_frame_bytes.size())) {
			result_line(false, "send");
			return static_cast<int>(ExitCode::kFail);
		}

		Frame pong_frame;
		const RecvResult pong_why = read_frame(village_client, pong_frame, timeout_ms);
		if (pong_why != RecvResult::kData) {
			return fail_at("pong", pong_why);
		}
		if (!expect_frame(pong_frame, proto::MsgId::kPongAck, 0)) {
			result_line(false, "pong");
			return static_cast<int>(ExitCode::kFail);
		}
		std::printf("pong  : OK\n");

		// ── 5. 종료 ───────────────────────────────────────────────
		village_client.close_graceful();
		result_line(true, nullptr);
		return static_cast<int>(ExitCode::kPass);
	}

}	// namespace client
