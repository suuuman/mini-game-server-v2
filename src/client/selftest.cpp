//  client/selftest.cpp — frame_codec.h 순수 함수 단위 테스트 (T015-impl.md §6)
//
//  12개 이름 있는 검사로 §6 의 11개 항목을 전부 덮는다(≥11 요건). 항목
//  8("put_u64_be(...) · put_u32_be(...) — 두 함수 각각 항목으로")을 문서
//  지시대로 두 검사(put_u64_be·put_u32_be)로 쪼갰고, 그 빈 자리를 메우듯
//  echo_pattern 검사를 더했다 — 5단계 프롬프트 리뷰 M3가 확정한 분해다.
//  check_order·decode_str16·expect_frame/expect_frame_min·bytes_equal 은
//  §6 이 한 항목 안에 여러 경우를 나열한 그대로, 검사 하나 안에서 그
//  경우들을 순서대로 확인한다 — "각각 항목으로" 지시가 없는 항목까지
//  쪼개면 실패 시 어느 경우가 깨졌는지는 detail 문자열로 이미 드러나므로
//  얻는 게 없다.
//
//  이 파일이 하는 일은 cmd_flow.cpp 가 거치는 판정 술어(expect_frame·
//  expect_frame_min·bytes_equal)를 flow 없이 직접 두드리는 것이다 —
//  frame_codec.h 머리말의 "정상 서버 앞에서 영구히 초록으로 남는 결함"을
//  여기서 잡는다.

#include "commands.h"
#include "frame_codec.h"
#include "socket_set.h"

#include "proto/packet.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace client {

	namespace {

		struct TestResult {
			bool pass;
			std::string detail;	// pass==false 일 때만 쓴다 — "<기대> vs <실제>".
		};

		TestResult test_build_frame_basic() {
			const uint8_t body[] = {'A', 'B', 'C', 'D'};
			const std::vector<uint8_t> frame = build_frame(proto::MsgId::kEchoReq, body, 4);
			if (frame.size() != 8) {
				return {false, "size=8 vs " + std::to_string(frame.size())};
			}
			const proto::PacketHeader h = proto::decode_header(frame.data());
			if (h.body_size != 4 || h.msg_id != proto::MsgId::kEchoReq) {
				return {false, "body_size=4,msg_id=1 vs body_size=" + std::to_string(h.body_size)
					+ ",msg_id=" + std::to_string(static_cast<int>(h.msg_id))};
			}
			if (frame[0] != 0x00 || frame[1] != 0x04 || frame[2] != 0x00 || frame[3] != 0x01) {
				return {false, "헤더 바이트 00 04 00 01 vs 실제 불일치"};
			}
			return {true, ""};
		}

		TestResult test_build_frame_raw_size_override() {
			const uint8_t body[] = {'A', 'B', 'C', 'D'};
			const std::vector<uint8_t> frame = build_frame(proto::MsgId::kEchoReq, body, 4, 60000);
			const proto::PacketHeader h = proto::decode_header(frame.data());
			if (h.body_size != 60000) {
				return {false, "body_size=60000 vs " + std::to_string(h.body_size)};
			}
			if (frame.size() != 8) {
				return {false, "실제 길이 8(4+4) vs " + std::to_string(frame.size())};
			}
			return {true, ""};
		}

		TestResult test_parse_frames_two_and_partial() {
			const uint8_t b1[] = {'A', 'B'};
			const uint8_t b2[] = {'C', 'D'};
			const std::vector<uint8_t> f1 = build_frame(proto::MsgId::kEchoReq, b1, 2);
			const std::vector<uint8_t> f2 = build_frame(proto::MsgId::kEchoReq, b2, 2);

			std::vector<uint8_t> buf;
			buf.insert(buf.end(), f1.begin(), f1.end());
			buf.insert(buf.end(), f2.begin(), f2.end());

			// 세 번째 프레임의 헤더만(4B) 붙인다 — body_size=2 를 선언하되
			// 실제 몸통 바이트는 안 붙여서 "완성되지 않은 프레임"을 만든다.
			proto::PacketHeader partial{};
			partial.body_size = 2;
			partial.msg_id = proto::MsgId::kEchoReq;
			uint8_t partial_bytes[4];
			proto::encode_header(partial_bytes, partial);
			buf.insert(buf.end(), partial_bytes, partial_bytes + 4);

			std::vector<Frame> out;
			bool protocol_error = false;
			const size_t consumed = parse_frames(buf.data(), buf.size(), out, protocol_error);

			if (out.size() != 2 || consumed != f1.size() + f2.size() || protocol_error) {
				return {false, "2개·consumed=" + std::to_string(f1.size() + f2.size()) + "·protocol_error=false vs "
					+ std::to_string(out.size()) + "개·consumed=" + std::to_string(consumed)
					+ "·protocol_error=" + (protocol_error ? "true" : "false")};
			}
			const size_t leftover = buf.size() - consumed;
			if (leftover != 4) {
				return {false, "잔량=4 vs " + std::to_string(leftover)};
			}
			return {true, ""};
		}

		TestResult test_parse_frames_header_only() {
			const uint8_t buf[3] = {0x00, 0x02, 0x00};	// 4B 헤더에 1B 모자란다.
			std::vector<Frame> out;
			bool protocol_error = false;
			const size_t consumed = parse_frames(buf, 3, out, protocol_error);
			if (!out.empty() || consumed != 0 || protocol_error) {
				return {false, "0개·소비 0·protocol_error=false vs " + std::to_string(out.size())
					+ "개·소비 " + std::to_string(consumed) + "·protocol_error=" + (protocol_error ? "true" : "false")};
			}
			return {true, ""};
		}

		TestResult test_parse_frames_oversized() {
			proto::PacketHeader header{};
			header.body_size = 4097;
			header.msg_id = proto::MsgId::kEchoReq;
			uint8_t buf[4];
			proto::encode_header(buf, header);

			std::vector<Frame> out;
			bool protocol_error = false;
			const size_t consumed = parse_frames(buf, 4, out, protocol_error);
			if (!out.empty() || consumed != 0 || !protocol_error) {
				return {false, "0개·protocol_error=true vs " + std::to_string(out.size())
					+ "개·protocol_error=" + (protocol_error ? "true" : "false")};
			}
			return {true, ""};
		}

		TestResult test_check_order() {
			const OrderCheck a = check_order(std::vector<uint32_t>{0, 1, 2});
			if (a.mismatch != 0 || a.dup != 0) {
				return {false, "{0,1,2}->{0,0} vs {" + std::to_string(a.mismatch) + "," + std::to_string(a.dup) + "}"};
			}
			const OrderCheck b = check_order(std::vector<uint32_t>{0, 2, 1});
			if (b.mismatch != 2 || b.dup != 0) {
				return {false, "{0,2,1}->{2,0} vs {" + std::to_string(b.mismatch) + "," + std::to_string(b.dup) + "}"};
			}
			const OrderCheck c = check_order(std::vector<uint32_t>{0, 0, 1});
			if (c.mismatch != 2 || c.dup != 1) {
				return {false, "{0,0,1}->{2,1} vs {" + std::to_string(c.mismatch) + "," + std::to_string(c.dup) + "}"};
			}
			return {true, ""};
		}

		TestResult test_decode_str16() {
			const std::vector<uint8_t> buf1 = {0x00, 0x09, '1', '2', '7', '.', '0', '.', '0', '.', '1'};
			std::string out1;
			size_t consumed1 = 0;
			if (!decode_str16(buf1.data(), buf1.size(), out1, consumed1) || out1 != "127.0.0.1" || consumed1 != 11) {
				return {false, "\"127.0.0.1\"·consumed=11 vs out=\"" + out1 + "\"·consumed=" + std::to_string(consumed1)};
			}

			const uint8_t buf2[2] = {0x00, 0x00};
			std::string out2 = "(안 채워짐)";
			size_t consumed2 = 999;
			if (!decode_str16(buf2, 2, out2, consumed2) || !out2.empty() || consumed2 != 2) {
				return {false, "빈 문자열·consumed=2 vs out=\"" + out2 + "\"·consumed=" + std::to_string(consumed2)};
			}

			const uint8_t buf3[4] = {0x00, 0x05, 'a', 'b'};	// 길이 5 를 선언했는데 남은 바이트는 2.
			std::string out3;
			size_t consumed3 = 0;
			if (decode_str16(buf3, 4, out3, consumed3)) {
				return {false, "false(길이 초과) vs true"};
			}
			return {true, ""};
		}

		TestResult test_put_u64_be() {
			std::vector<uint8_t> out;
			put_u64_be(out, 0x0102030405060708ULL);
			const uint8_t expected[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
			if (out.size() != 8 || std::memcmp(out.data(), expected, 8) != 0) {
				return {false, "01 02 03 04 05 06 07 08 vs 길이=" + std::to_string(out.size()) + " 또는 바이트 불일치"};
			}
			return {true, ""};
		}

		TestResult test_put_u32_be() {
			std::vector<uint8_t> out;
			put_u32_be(out, 0x0A0B0C0DUL);
			const uint8_t expected[4] = {0x0A, 0x0B, 0x0C, 0x0D};
			if (out.size() != 4 || std::memcmp(out.data(), expected, 4) != 0) {
				return {false, "0A 0B 0C 0D vs 길이=" + std::to_string(out.size()) + " 또는 바이트 불일치"};
			}
			return {true, ""};
		}

		TestResult test_echo_pattern() {
			const std::vector<uint8_t> e0 = echo_pattern(0);
			if (!e0.empty()) {
				return {false, "빈 벡터 vs size=" + std::to_string(e0.size())};
			}
			const std::vector<uint8_t> e3 = echo_pattern(3);
			if (e3.size() != 3 || e3[0] != 'A' || e3[1] != 'A' || e3[2] != 'A') {
				return {false, "\"AAA\" vs 실제 불일치"};
			}
			return {true, ""};
		}

		TestResult test_expect_frame() {
			Frame f;
			f.id = proto::MsgId::kEchoAck;
			f.body = std::vector<uint8_t>(8, 0);

			if (!expect_frame(f, proto::MsgId::kEchoAck, 8)) {
				return {false, "true vs false (id·길이 일치)"};
			}
			if (expect_frame(f, proto::MsgId::kPongAck, 8)) {
				return {false, "false vs true (id 불일치인데 통과)"};
			}
			if (expect_frame(f, proto::MsgId::kEchoAck, 7)) {
				return {false, "false vs true (길이 불일치인데 통과)"};
			}

			Frame login;
			login.id = proto::MsgId::kSessionLoginAck;
			login.body = std::vector<uint8_t>(5, 0);
			if (!expect_frame_min(login, proto::MsgId::kSessionLoginAck, 5)) {
				return {false, "true vs false (5>=5)"};
			}
			login.body = std::vector<uint8_t>(4, 0);
			if (expect_frame_min(login, proto::MsgId::kSessionLoginAck, 5)) {
				return {false, "false vs true (4<5 인데 통과)"};
			}
			return {true, ""};
		}

		TestResult test_bytes_equal() {
			const uint8_t raw[8] = {1, 2, 3, 4, 5, 6, 7, 8};
			const std::vector<uint8_t> a(raw, raw + 8);
			if (!bytes_equal(a, raw, 8)) {
				return {false, "true vs false (동일한데 불일치 판정)"};
			}
			const uint8_t diff[8] = {1, 2, 3, 4, 5, 6, 7, 9};
			if (bytes_equal(a, diff, 8)) {
				return {false, "false vs true (마지막 1B 다른데 통과)"};
			}
			if (bytes_equal(a, raw, 7)) {
				return {false, "false vs true (길이 다른데 통과)"};
			}
			return {true, ""};
		}

		// T016 이식 — socket_set.h 순수 함수(§7). n==0 분기는 docs/TESTING.md
		// 「못 덮는 것」 T015 행이 지적한 구멍이다 — 기존 12항목 어디도
		// bytes_equal 의 n==0 memcmp 우회 분기를 두드리지 않았다.
		TestResult test_bytes_equal_n0() {
			if (!bytes_equal(std::vector<uint8_t>{}, nullptr, 0)) {
				return {false, "true vs false (빈 벡터·nullptr·n=0)"};
			}
			const uint8_t one[1] = {0x02};
			if (bytes_equal(std::vector<uint8_t>{1}, one, 1)) {
				return {false, "false vs true ({1} vs {2}인데 통과)"};
			}
			return {true, ""};
		}

		TestResult test_zone_of_round_robin() {
			const uint32_t zone_id = 5;
			const uint32_t expected[8] = {5, 6, 7, 8, 5, 6, 7, 8};
			for (size_t i = 0; i < 8; ++i) {
				const uint32_t z = zone_of(i, zone_id, 4);
				if (z != expected[i]) {
					return {false, "zones=4 i=" + std::to_string(i) + ": " + std::to_string(expected[i]) + " vs " + std::to_string(z)};
				}
			}
			for (size_t i = 0; i < 8; ++i) {
				const uint32_t z = zone_of(i, zone_id, 1);
				if (z != zone_id) {
					return {false, "zones=1 i=" + std::to_string(i) + ": 5 vs " + std::to_string(z)};
				}
			}
			const uint32_t z0 = zone_of(0, zone_id, 0);
			if (z0 != zone_id) {
				return {false, "zones=0: 5 vs " + std::to_string(z0)};
			}
			return {true, ""};
		}

		TestResult test_expect_count_floor() {
			struct Case {
				uint64_t clients;
				uint64_t zones;
				uint64_t chats;
				uint64_t expected;
			};
			const Case cases[] = {
				{8, 4, 100, 200},
				{7, 4, 100, 100},
				{3, 4, 100, 0},
				{8, 1, 100, 800},
				{8, 0, 100, 0},
			};
			for (const Case& c : cases) {
				const uint64_t got = expect_count(c.clients, c.zones, c.chats);
				if (got != c.expected) {
					return {false, "(" + std::to_string(c.clients) + "," + std::to_string(c.zones) + "," + std::to_string(c.chats)
						+ ")=" + std::to_string(c.expected) + " vs " + std::to_string(got)};
				}
			}
			return {true, ""};
		}

		// 24B(8B 프레임 3개 · id 103,103,102)를 [0,10) [10,21) [21,24) 세
		// 조각으로 나눠 먹인다 — 이 경계는 5단계 R1 개발 HIGH 가 잡은
		// 산술 오류(초안의 「30B · [10,25)[25,30)」는 3×8B=24B 를 안
		// 맞춘 값이었다)를 정정한 것이다.
		TestResult test_tally_feed_split() {
			const uint8_t body[4] = {0, 0, 0, 0};
			const std::vector<uint8_t> f1 = build_frame(proto::MsgId::kChatNtf, body, 4);
			const std::vector<uint8_t> f2 = build_frame(proto::MsgId::kChatNtf, body, 4);
			const std::vector<uint8_t> f3 = build_frame(proto::MsgId::kJoinZoneAck, body, 4);

			std::vector<uint8_t> stream;
			stream.insert(stream.end(), f1.begin(), f1.end());
			stream.insert(stream.end(), f2.begin(), f2.end());
			stream.insert(stream.end(), f3.begin(), f3.end());
			if (stream.size() != 24) {
				return {false, "24B vs " + std::to_string(stream.size())};
			}

			FrameTally t;

			tally_feed(t, stream.data() + 0, 10);
			uint32_t got103 = t.by_id.count(103) ? t.by_id[103] : 0;
			if (got103 != 1 || tally_partial_bytes(t) != 2) {
				return {false, "[0,10): by_id[103]=1,partial=2 vs by_id[103]=" + std::to_string(got103)
					+ ",partial=" + std::to_string(tally_partial_bytes(t))};
			}

			tally_feed(t, stream.data() + 10, 11);
			got103 = t.by_id.count(103) ? t.by_id[103] : 0;
			if (got103 != 2 || tally_partial_bytes(t) != 5) {
				return {false, "[10,21): by_id[103]=2,partial=5 vs by_id[103]=" + std::to_string(got103)
					+ ",partial=" + std::to_string(tally_partial_bytes(t))};
			}

			tally_feed(t, stream.data() + 21, 3);
			got103 = t.by_id.count(103) ? t.by_id[103] : 0;
			const uint32_t got102 = t.by_id.count(102) ? t.by_id[102] : 0;
			if (got103 != 2 || got102 != 1 || tally_partial_bytes(t) != 0 || t.protocol_error) {
				return {false, "[21,24): 103=2,102=1,partial=0,protocol_error=false vs 103=" + std::to_string(got103)
					+ ",102=" + std::to_string(got102) + ",partial=" + std::to_string(tally_partial_bytes(t))
					+ ",protocol_error=" + (t.protocol_error ? "true" : "false")};
			}
			return {true, ""};
		}

		// socket_set.h 순수 함수(§C, 7단계 코드 리뷰 test MED) — pump_once
		// 안에 묻혀 있던 인덱스 리매핑을 selftest 가 소켓 없이 직접
		// 두드린다. 「닫힌 peer 가 앞쪽에 섞인」 조합은 select 타이밍에
		// 좌우돼 하네스로 결정적으로 재현하기 어렵다.
		TestResult test_remap_ready_closed_peer() {
			std::vector<size_t> out;

			remap_ready(std::vector<size_t>{0, 2, 3}, std::vector<size_t>{1, 2}, out);
			if (out != std::vector<size_t>{2, 3}) {
				return {false, "{2,3} vs size=" + std::to_string(out.size())};
			}

			remap_ready(std::vector<size_t>{0, 2, 3}, std::vector<size_t>{}, out);
			if (!out.empty()) {
				return {false, "{}(빈 ready) vs size=" + std::to_string(out.size())};
			}

			remap_ready(std::vector<size_t>{5}, std::vector<size_t>{0}, out);
			if (out != std::vector<size_t>{5}) {
				return {false, "{5} vs size=" + std::to_string(out.size())};
			}

			return {true, ""};
		}

		struct NamedTest {
			const char* name;
			TestResult (*fn)();
		};

	}	// namespace

	int run_selftest() {
		const NamedTest tests[] = {
			{"build_frame_basic", test_build_frame_basic},
			{"build_frame_raw_size_override", test_build_frame_raw_size_override},
			{"parse_frames_two_and_partial", test_parse_frames_two_and_partial},
			{"parse_frames_header_only", test_parse_frames_header_only},
			{"parse_frames_oversized", test_parse_frames_oversized},
			{"check_order", test_check_order},
			{"decode_str16", test_decode_str16},
			{"put_u64_be", test_put_u64_be},
			{"put_u32_be", test_put_u32_be},
			{"echo_pattern", test_echo_pattern},
			{"expect_frame_and_expect_frame_min", test_expect_frame},
			{"bytes_equal", test_bytes_equal},
			{"bytes_equal_n0", test_bytes_equal_n0},
			{"zone_of_round_robin", test_zone_of_round_robin},
			{"expect_count_floor", test_expect_count_floor},
			{"tally_feed_split", test_tally_feed_split},
			{"remap_ready_closed_peer", test_remap_ready_closed_peer},
		};
		const int total = static_cast<int>(sizeof(tests) / sizeof(tests[0]));
		int pass = 0;

		for (const NamedTest& t : tests) {
			const TestResult r = t.fn();
			if (r.pass) {
				++pass;
				std::printf("  [ok] %s\n", t.name);
			} else {
				std::printf("  [X ] %s — %s\n", t.name, r.detail.c_str());
			}
		}

		std::printf("selftest: %d/%d\n", pass, total);
		result_line(pass == total, "selftest");
		return (pass == total) ? static_cast<int>(ExitCode::kPass) : static_cast<int>(ExitCode::kFail);
	}

}	// namespace client
