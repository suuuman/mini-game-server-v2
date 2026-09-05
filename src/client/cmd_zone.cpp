//  client/cmd_zone.cpp — `client zone` (zone.ps1 이식, T016-impl.md §6)
//
//  PS 절차와 이 파일의 대응(zone.ps1:125-279):
//    ① 접속·Enter          zone.ps1:125-140 (Connect-Reserved 를 connect+
//                          kEnterReq/kEnterAck 로 직접 푼다)
//    ② 존 입장             zone.ps1:142-166
//    ③ 수신 버퍼 비움       zone.ps1:169
//    ④ 채팅(라운드 로빈)    zone.ps1:171-214
//    ⑤ 남은 것 수거         zone.ps1:216-230
//    ⑥ 판정                 zone.ps1:232-279 (known 집합만 예외 — 아래 참조)
//    ⑦ 종료                 zone.ps1:281-284
//
//  ⚠️ Enter 단계(①)만 소켓 1개씩 순차로 `read_frame` 을 쓴다 — 이 시점엔
//  아직 다른 peer 가 없어(peers 벡터를 채우는 중) 헤드-오브-라인 차단이
//  성립하지 않는다. ②부터는 peers 가 모두 갖춰지므로 socket_set.h 의
//  규약대로 `pump_once`/`select_readable` 만 쓰고 `recv_exact`/`read_frame`
//  은 부르지 않는다(지시서 §1 절대 금지).
//
//  「소켓 생존 : %s (closed=%zu)」가 PS 의 「서버 생존 : $alive」를 대체한
//  이유 — `Get-Process` 는 클라이언트 프로세스 밖의 관측이라 이 exe 에서는
//  할 수 없다. 대신 이 프로세스가 직접 아는 사실(자기 소켓이 닫혔는가)로
//  같은 질문("부하 중 서버/연결이 죽었는가")에 답한다 — `closed==0` 이면
//  PS 의 `$alive=True` 와 같은 뜻으로 읽는다.
//
//  ⚠️⚠️ known 집합에 112(kZoneMembersNtf)를 넣은 이유 — `zone.ps1:244`
//  의 `$known = @($MSG_CHAT_NTF, $MSG_JOIN_ACK)`(103·102)는 ADR-026
//  이전(존 이동마다 kZoneMembersNtf 가 오게 되기 전)의 집합이다. 그
//  낡은 집합으로 `-Churn` 모드를 재면 정상적으로 오는 kZoneMembersNtf
//  통지를 "깨진 스트림"으로 오판한다 — Step 0 스파이크 실측
//  (`T016-step0-zone-churn10.txt`): 8클라 전부 `ChatNtf 200/200` 인데
//  `other-ids=112` 가 4건 찍혀 `판정 ✕`. 이 exe 는 known 에 112 를 넣어
//  그 오판을 없앤다 — **의도된 비동치**(lead 자체 승인 A12)다. PS 갈래
//  자체를 고치는 것은 이번 게이트 밖(다음 소정리)이라 `zone.ps1` 의
//  `$known` 은 그대로 둔다.
//
//  기계 계약(ASCII: `zone  : ...` 요약 줄 · `RESULT:`)과 사람용(한글 —
//  판정 설명·클라별 줄)을 분리한 이유는 다른 커맨드와 같다 — 콘솔 코드
//  페이지 949 에서 한글·○·✕ 가 깨지므로, 하네스가 실제로 파싱하는 줄에는
//  넣지 않는다(§1 절대 금지).
//
//  ②④ 의 `send_all` 은 전부 `send_or_mark`(익명 네임스페이스)를 거친다 —
//  PS 는 `Stream.Write` 가 예외를 던지면 스크립트 전체가 죽지만, 이 exe
//  는 그 peer 하나만 `closed`(및 `error`)로 표시하고 나머지 peer 는
//  계속 돈다. 판정에는 `closed>0` 으로 나타나고 클라별 줄에는 `  error`
//  조각이 붙는다 — **의도된 비동치**다(7단계 코드 리뷰 correctness LOW).

#include "commands.h"
#include "frame_codec.h"
#include "socket_set.h"
#include "tcp_client.h"

#include "proto/packet.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace client {

	namespace {

		// pump_once 를 부르고 select 오류(-1)면 그 자리에서 판정 실패로
		// 끝낸다 — §6-2 말미: "select 가 SOCKET_ERROR 를 돌려주면
		// RESULT: FAIL select WSAGetLastError=N + 1(접속 실패가 아니라
		// 판정 실패로 분류)". WSAGetLastError() 는 select_readable 안의
		// select 호출 직후 값을 그대로 물려받는다 — 그 사이 다른 Winsock
		// 호출이 없어야 유효한데, pump_once 호출 지점들은 전부 이 값을
		// 즉시 확인하므로 그 전제가 지켜진다.
		bool pump_or_fail(std::vector<Peer>& peers, int timeout_ms, uint8_t* buf, size_t cap) {
			if (pump_once(peers, timeout_ms, buf, cap) < 0) {
				const int err = WSAGetLastError();
				result_line(false, ("select WSAGetLastError=" + std::to_string(err)).c_str());
				return false;
			}
			return true;
		}

		// ②④ 의 send_all 호출을 이걸 거치게 한다 — 이미 closed 인 peer 에는
		// 아예 보내지 않고, 보내다 실패하면 그 peer 를 closed·error 로
		// 표시하고 계속 진행한다. PS 는 Stream.Write 가 예외를 던지면
		// 스크립트 전체가 죽지만(zone.ps1 은 이 실패 경로를 시험한 적이
		// 없다), 이 exe 는 그 peer 하나만 죽은 것으로 보고 나머지는 계속
		// 돈다 — 의도된 비동치다(7단계 코드 리뷰 correctness LOW). 판정엔
		// closed>0 로 나타나고, 클라별 줄에는 "  error" 조각이 붙는다.
		void send_or_mark(Peer& p, const std::vector<uint8_t>& frame) {
			if (p.closed) {
				return;
			}
			if (!p.client->send_all(frame.data(), frame.size())) {
				p.closed = true;
				p.error = true;
			}
		}

	}	// namespace

	int run_zone(const Args& args) {
		const std::string host = args.get("host", "127.0.0.1");
		const int64_t port_raw = args.get_int("port", 9000, 1, 65535);
		const int64_t clients = args.get_int("clients", 8, 1, FD_SETSIZE);
		const int64_t zone_id_raw = args.get_int("zone-id", 5, 0, 4294967295);
		const int64_t zones = args.get_int("zones", 1, 1, FD_SETSIZE);
		const int64_t chats = args.get_int("chats", 100, 1, 1000000);
		// 1+size <= kMaxBodySize(4096) — kChatReq body 는 [type:u8][text...].
		const int64_t size = args.get_int("size", 8, 1, 4095);
		const int64_t churn = args.get_int("churn", 0, 0, 1000000);
		const int64_t timeout = args.get_int("timeout", 5000, 0, 600000);
		const int64_t settle = args.get_int("settle", 1500, 0, 600000);
		// player-base 는 PS param 에 없는 신규 인자다 — 래퍼가
		// Grant-Reservation 한 첫 player_id(zone.ps1 -Cxx 는 1부터 발급).
		const uint64_t player_base = args.get_u64("player-base", 1);

		if (!args.ok() || player_base < 1) {
			print_usage();
			if (!args.error().empty()) {
				std::printf("%s\n", args.error().c_str());
			}
			return static_cast<int>(ExitCode::kUsage);
		}
		const std::vector<std::string> unknown = args.unknown();
		if (!unknown.empty()) {
			print_usage();
			return static_cast<int>(ExitCode::kUsage);
		}

		const uint16_t port = static_cast<uint16_t>(port_raw);
		const uint32_t zone_id = static_cast<uint32_t>(zone_id_raw);
		const uint64_t expect = expect_count(static_cast<uint64_t>(clients), static_cast<uint64_t>(zones), static_cast<uint64_t>(chats));
		if (expect == 0) {
			// zone.ps1:93 의 throw 대응 — per_zone(=clients/zones, 정수
			// 나눗셈) < 1. 기계 계약 줄이라 한글·기호를 넣지 않는다(§1).
			std::printf("zone: clients(%lld) < zones(%lld)\n", static_cast<long long>(clients), static_cast<long long>(zones));
			return static_cast<int>(ExitCode::kUsage);
		}

		std::vector<Peer> peers;
		peers.reserve(static_cast<size_t>(clients));

		// ── 1. 접속·Enter (순차 · 이 단계만 read_frame 허용) ────────────
		for (int64_t i = 0; i < clients; ++i) {
			Peer p;
			p.client = std::make_unique<TcpClient>();
			if (!p.client->connect(host, port)) {
				if (i == 0) {
					result_line(false, "connect");
					return static_cast<int>(ExitCode::kConnect);
				}
				const std::string reason = "connect client=" + std::to_string(i);
				result_line(false, reason.c_str());
				return static_cast<int>(ExitCode::kFail);
			}

			std::vector<uint8_t> enter_body;
			put_u64_be(enter_body, player_base + static_cast<uint64_t>(i));
			const std::vector<uint8_t> enter_frame = build_frame(proto::MsgId::kEnterReq, enter_body.data(), enter_body.size());

			Frame f;
			RecvResult r = RecvResult::kError;
			if (p.client->send_all(enter_frame.data(), enter_frame.size())) {
				r = read_frame(*p.client, f, static_cast<int>(timeout));
			}

			const bool got_ok = (r == RecvResult::kData) && expect_frame_min(f, proto::MsgId::kEnterAck, 1) && f.body[0] == 0;
			if (!got_ok) {
				// result=R — 정상 응답인데 kOk 가 아니면 그 결과 코드,
				// 응답 모양 자체가 틀렸으면(id/길이) "badframe", 아예 못
				// 받았으면 recv 실패 종류를 적는다(설계 결정 — 지시서는
				// "result=R" 형식만 못박고 실패 갈래별 R 값은 정하지
				// 않았다).
				std::string result_str;
				if (r == RecvResult::kTimeout) {
					result_str = "timeout";
				} else if (r == RecvResult::kClosed) {
					result_str = "closed";
				} else if (r == RecvResult::kError) {
					result_str = "error";
				} else if (!expect_frame_min(f, proto::MsgId::kEnterAck, 1)) {
					result_str = "badframe";
				} else {
					result_str = std::to_string(static_cast<int>(f.body[0]));
				}
				const std::string reason = "enter client=" + std::to_string(i) + " result=" + result_str;
				result_line(false, reason.c_str());
				return static_cast<int>(ExitCode::kFail);
			}

			p.zone = zone_of(static_cast<size_t>(i), zone_id, static_cast<uint32_t>(zones));
			p.player_id = player_base + static_cast<uint64_t>(i);
			peers.push_back(std::move(p));
		}
		std::printf("connect: %zu 개 완료(예약 경유)\n", peers.size());

		uint8_t buf[65536];

		// ── 2. 존 입장 ───────────────────────────────────────────────
		for (Peer& p : peers) {
			std::vector<uint8_t> join_body;
			put_u32_be(join_body, p.zone);
			const std::vector<uint8_t> join_frame = build_frame(proto::MsgId::kJoinZoneReq, join_body.data(), join_body.size());
			send_or_mark(p, join_frame);
		}

		const auto join_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
		size_t joined = 0;
		for (;;) {
			if (!pump_or_fail(peers, 50, buf, sizeof(buf))) {
				return static_cast<int>(ExitCode::kFail);
			}
			joined = 0;
			for (const Peer& p : peers) {
				const auto it = p.tally.by_id.find(static_cast<uint16_t>(proto::MsgId::kJoinZoneAck));
				if (it != p.tally.by_id.end() && it->second > 0) {
					++joined;
				}
			}
			if (joined >= peers.size()) {
				break;
			}
			if (std::chrono::steady_clock::now() >= join_deadline) {
				break;
			}
		}
		std::printf("join   : %zu / %zu 입장 확인\n", joined, peers.size());
		if (joined < peers.size()) {
			std::printf("  ! 입장이 다 안 됐다. 서버 로그를 확인할 것\n");
		}

		// ── 3. 입장 단계에서 받은 것은 버린다 ────────────────────────────
		for (Peer& p : peers) {
			tally_reset_counts(p.tally);
		}

		// ── 4. 채팅 — 라운드 로빈으로 인터리브 ───────────────────────────
		std::vector<uint8_t> chat_body;
		chat_body.push_back(0x00);	// type=Zone(0) 선행(zone.ps1:174-178).
		{
			const std::vector<uint8_t> text = echo_pattern(static_cast<size_t>(size));
			chat_body.insert(chat_body.end(), text.begin(), text.end());
		}
		const std::vector<uint8_t> chat_frame = build_frame(proto::MsgId::kChatReq, chat_body.data(), chat_body.size());

		const auto chat_start = std::chrono::steady_clock::now();
		for (int64_t m = 0; m < chats; ++m) {
			for (Peer& p : peers) {
				send_or_mark(p, chat_frame);
			}

			// -Churn K>0 — K 회마다 홀수 인덱스 peer 를 옆 존으로 보냈다
			// 되돌린다(zone.ps1:196-205, +1001 은 관찰 중인 존과 안 겹치게).
			if (churn > 0 && (m % churn) == (churn - 1)) {
				for (size_t j = 1; j < peers.size(); j += 2) {
					Peer& p = peers[j];
					std::vector<uint8_t> away_body;
					put_u32_be(away_body, p.zone + 1001);
					std::vector<uint8_t> back_body;
					put_u32_be(back_body, p.zone);
					const std::vector<uint8_t> away_frame = build_frame(proto::MsgId::kJoinZoneReq, away_body.data(), away_body.size());
					const std::vector<uint8_t> back_frame = build_frame(proto::MsgId::kJoinZoneReq, back_body.data(), back_body.size());
					send_or_mark(p, away_frame);
					send_or_mark(p, back_frame);
				}
			}

			// 10 회마다 빨아들인다 — 안 그러면 수신 버퍼가 차서 서버 송신이
			// 막힌다(zone.ps1:206-210 과 같은 이유).
			if ((m % 10) == 9) {
				if (!pump_or_fail(peers, 0, buf, sizeof(buf))) {
					return static_cast<int>(ExitCode::kFail);
				}
			}
		}
		const auto chat_end = std::chrono::steady_clock::now();
		const long long chat_ms = std::chrono::duration_cast<std::chrono::milliseconds>(chat_end - chat_start).count();
		const uint64_t sent_count = static_cast<uint64_t>(clients) * static_cast<uint64_t>(chats);
		std::printf("send   : %llu 개 전송 완료 (%lld ms)\n", static_cast<unsigned long long>(sent_count), chat_ms);

		// ── 5. 남은 것 수거 ──────────────────────────────────────────────
		const auto drain_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout + settle);
		for (;;) {
			if (!pump_or_fail(peers, 100, buf, sizeof(buf))) {
				return static_cast<int>(ExitCode::kFail);
			}
			bool all_done = true;
			for (const Peer& p : peers) {
				const auto it = p.tally.by_id.find(static_cast<uint16_t>(proto::MsgId::kChatNtf));
				const uint32_t got = (it != p.tally.by_id.end()) ? it->second : 0;
				if (got < expect) {
					all_done = false;
					break;
				}
			}
			if (all_done) {
				break;
			}
			if (std::chrono::steady_clock::now() >= drain_deadline) {
				break;
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(settle));
		if (!pump_or_fail(peers, 0, buf, sizeof(buf))) {
			return static_cast<int>(ExitCode::kFail);
		}

		// ── 6. 판정(zone.ps1:232-278 — known 집합만 예외, 머리말 참조) ───
		const bool churn_mode = (churn > 0);
		size_t bad = 0;
		size_t broken = 0;

		std::printf("\n");
		for (size_t idx = 0; idx < peers.size(); ++idx) {
			const Peer& p = peers[idx];
			const auto it_got = p.tally.by_id.find(static_cast<uint16_t>(proto::MsgId::kChatNtf));
			const uint32_t got = (it_got != p.tally.by_id.end()) ? it_got->second : 0;

			std::string other;
			for (const auto& kv : p.tally.by_id) {
				const uint16_t id = kv.first;
				if (id == static_cast<uint16_t>(proto::MsgId::kChatNtf)
					|| id == static_cast<uint16_t>(proto::MsgId::kJoinZoneAck)
					|| id == static_cast<uint16_t>(proto::MsgId::kZoneMembersNtf)) {
					continue;
				}
				if (!other.empty()) {
					other += ",";
				}
				other += std::to_string(static_cast<unsigned>(id));
			}
			const size_t partial = tally_partial_bytes(p.tally);
			if (!other.empty() || partial > 0) {
				++broken;
			}

			const char* mark = churn_mode ? "--" : (static_cast<uint64_t>(got) == expect ? "OK" : "X ");
			if (!churn_mode && static_cast<uint64_t>(got) != expect) {
				++bad;
			}

			std::string extra;
			if (partial > 0) {
				extra += "  partial=" + std::to_string(partial) + "B";
			}
			if (!other.empty()) {
				extra += "  other-ids=" + other;
			}
			if (p.error) {
				extra += "  error";
			}

			std::printf("  client %zu (zone %u) : %s  ChatNtf %u / %llu%s\n",
				idx, p.zone, mark, got, static_cast<unsigned long long>(expect), extra.c_str());
		}

		size_t closed = 0;
		for (const Peer& p : peers) {
			if (p.closed) {
				++closed;
			}
		}

		std::printf("\n");
		if (churn_mode) {
			std::printf("churn : 채팅 중 입·퇴장을 섞었다 (절반이 %lld 회마다 옆 존 왕복)\n", static_cast<long long>(churn));
			std::printf("        소켓 생존   : %s (closed=%zu)\n", ps_bool(closed == 0), closed);
			std::printf("        깨진 스트림 : %zu / %zu\n", broken, peers.size());
			if (broken == 0 && closed == 0) {
				std::printf("판정  : ○ 부하 중 멤버 목록이 흔들려도 서버가 멀쩡하다 (closed=0)\n");
			} else {
				std::printf("판정  : ✕ 상태가 깨졌다 — 같은 존을 여러 스레드가 만졌을 때 나오는 증상이다\n");
			}
		} else if (bad == 0) {
			if (zones > 1) {
				std::printf("판정  : ○ 브로드캐스트가 「자기 존에만」 갔다. 존 경계가 지켜졌다\n");
				std::printf("        (다른 존 것까지 왔다면 %llu 개가 됐을 것)\n", static_cast<unsigned long long>(sent_count));
			} else {
				std::printf("판정  : ○ 브로드캐스트 정확. 모든 클라가 %llu 개를 받았다\n", static_cast<unsigned long long>(expect));
			}
		} else {
			std::printf("판정  : ✕ %zu / %zu 개 클라의 수신 개수가 어긋남\n", bad, peers.size());
			std::printf("        기대보다 많으면 존 경계가 샌 것이고,\n");
			std::printf("        적거나 들쭉날쭉하면 같은 존을 여러 스레드가 만진 것이다.\n");
			std::printf("        Debug 빌드라면 서버 쪽 assert 로그도 함께 확인할 것.\n");
		}

		// ASCII 요약 — 래퍼·client.ps1 이 읽는 줄(한글·기호 금지, §1).
		std::printf("zone  : clients=%zu zones=%u expect=%llu joined=%zu bad=%zu broken=%zu closed=%zu\n",
			peers.size(), static_cast<unsigned>(zones), static_cast<unsigned long long>(expect), joined, bad, broken, closed);

		// ── 7. 종료 ──────────────────────────────────────────────────────
		for (Peer& p : peers) {
			if (p.client) {
				p.client->close_graceful();
			}
		}

		const bool pass = churn_mode ? (broken == 0 && closed == 0) : (bad == 0);
		const std::string reason = churn_mode
			? ("broken=" + std::to_string(broken) + " closed=" + std::to_string(closed))
			: ("bad=" + std::to_string(bad));
		result_line(pass, pass ? nullptr : reason.c_str());
		return pass ? static_cast<int>(ExitCode::kPass) : static_cast<int>(ExitCode::kFail);
	}

}	// namespace client
