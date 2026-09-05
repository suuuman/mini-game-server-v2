//  client/frame_codec.h — 프레임 조립·분해·판정의 순수 함수 (헤더 전용)
//
//  전부 순수 함수인 이유 — 소켓도 전역 상태도 없어야 `client selftest` 가
//  이 파일을 직접 두드릴 수 있다(T015-plan.md 결정 6). cmd_flow.cpp 의
//  echo/pong 판정이 여기 술어(expect_frame·expect_frame_min·bytes_equal)를
//  거치지 않고 id·길이·바이트를 직접 비교하면, selftest 는 그 비교를 대신
//  시험할 방법이 없어져 판정 로직 자체의 결함(뮤턴트 MUT6/7 — echo 비교
//  무력화·pong id 검사 무력화)이 정상 서버 앞에서 영구히 초록으로 남는다.
//  그래서 flow 의 각 단계 판정은 반드시 이 술어들을 거친다.
//
//  헤더는 proto::encode_header/decode_header 로만 만든다 — ARCHITECTURE.md
//  §7 불변식 9("헤더 인코딩은 proto::encode_header 한 곳")의 정신을 클라도
//  지킨다. 여기서 바이트를 손으로 조립하지 않는다.
//
//  ⚠️ build_frame 은 본문 길이(len)의 kMaxBodySize(4096) 상한을 의도적으로
//  검사하지 않는다 — 이 도구는 프로토콜 경계를 두드리는 용도라, 상한을
//  넘겨 보내고 서버가 그 프레임을 절단하는 것(frame_router.cpp:1206-1211)을
//  보는 것이 목적이다. 상한을 여기서 막으면 그 시나리오(S3e — `--raw-size
//  60000 --expect-close`)를 아예 못 만든다.
//
//  parse_frames 가 check_order 와 분리된 이유 — parse_frames 는 "스트림에서
//  프레임 경계를 복원한다"만 하고, 순번이 옳은지는 모른다. 두 일을 한
//  함수에 합치면 "복원은 맞았는데 순번 판정이 결함"인 경우를 selftest 가
//  독립적으로 재현할 수 없다(항목 3~5 는 parse_frames 만, 항목 6 은
//  check_order 만 시험한다).

#pragma once

#include "proto/packet.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace client {

	// build_frame — [ body_size:u16 ][ msg_id:u16 ][ body... ] 프레임 하나를
	//   만든다. raw_size_override >= 0 이면 body_size 필드에 그 값을 강제로
	//   넣는다(send.ps1:50-58 의 -RawSize 동치 — 헤더 위조로 서버 절단을
	//   시험한다). 실제로 담기는 바이트 수는 len 그대로다 — raw_size_override
	//   는 "헤더가 주장하는 길이"만 위조하고 진짜 버퍼 크기는 바꾸지 않는다.
	//   ⚠️ 호출자가 len ≤ 0xFFFF 를 보장한다 — body_size 필드가 u16 이라
	//   그 이상은 static_cast<uint16_t> 에서 조용히 랩어라운드된다(7단계
	//   코드 리뷰 correctness MED-1). cmd_send.cpp 는 --size 의 get_int
	//   상한(65535)으로 이걸 지킨다.
	inline std::vector<uint8_t> build_frame(proto::MsgId id, const uint8_t* body, size_t len, int raw_size_override = -1) {
		std::vector<uint8_t> out(proto::kHeaderSize + len);

		proto::PacketHeader header{};
		header.body_size = (raw_size_override >= 0)
			? static_cast<uint16_t>(raw_size_override)
			: static_cast<uint16_t>(len);
		header.msg_id = id;
		proto::encode_header(out.data(), header);

		if (len > 0 && body != nullptr) {
			std::memcpy(out.data() + proto::kHeaderSize, body, len);
		}
		return out;
	}

	// 분해된 프레임 하나.
	struct Frame {
		proto::MsgId id;
		std::vector<uint8_t> body;
	};

	// parse_frames — buf 에서 완성된 프레임을 전부 뽑아 out 에 채우고, 소비한
	//   바이트 수를 반환한다. 잔량(소비 안 된 꼬리)은 호출자가 유지한다 —
	//   이 함수는 buf 를 복사하지도, 잔량을 어딘가에 들고 있지도 않는다
	//   (순수 함수 원칙 — 파일 첫머리 주석).
	//
	//   body_size 가 proto::kMaxBodySize 를 넘으면 protocol_error=true 로
	//   세팅하고 그 프레임은 out 에 넣지 않은 채 거기서 멈춘다. 헤더는
	//   읽었지만 몸통이 아직 덜 온 경우("불완전 프레임")도 그 프레임을 넣지
	//   않고 멈추지만, 이건 protocol_error 가 아니다 — 다음 recv 로 채워질
	//   수 있는 정상 상황이다. 두 "멈춤"을 protocol_error 로 구분한다.
	inline size_t parse_frames(const uint8_t* buf, size_t len, std::vector<Frame>& out, bool& protocol_error) {
		protocol_error = false;
		size_t offset = 0;

		while (offset + proto::kHeaderSize <= len) {
			const proto::PacketHeader header = proto::decode_header(buf + offset);

			if (header.body_size > proto::kMaxBodySize) {
				protocol_error = true;
				break;
			}
			if (offset + proto::kHeaderSize + header.body_size > len) {
				// 헤더는 왔지만 몸통이 덜 왔다 — 다음 recv 를 기다린다.
				break;
			}

			Frame frame;
			frame.id = header.msg_id;
			const uint8_t* body_begin = buf + offset + proto::kHeaderSize;
			frame.body.assign(body_begin, body_begin + header.body_size);
			out.push_back(std::move(frame));

			offset += proto::kHeaderSize + header.body_size;
		}

		return offset;
	}

	// decode_str16 — [ len:u16 BE ][ bytes... ] 를 읽는다(session_router.cpp:
	//   327-331 조립의 역연산). len 이 남은 바이트보다 크면 false — 그 자리에서
	//   out/consumed 를 건드리지 않는다(호출자가 실패를 실패로만 보게 한다).
	inline bool decode_str16(const uint8_t* p, size_t len, std::string& out, size_t& consumed) {
		if (len < 2) {
			return false;
		}
		const uint16_t str_len = proto::read_u16_be(p);
		if (static_cast<size_t>(2) + str_len > len) {
			return false;
		}
		out.assign(reinterpret_cast<const char*>(p + 2), str_len);
		consumed = static_cast<size_t>(2) + str_len;
		return true;
	}

	// check_order — send.ps1:170-171 의 순번 검사 로직. mismatch 는
	//   seqs[k] != k 인 개수, dup 은 (전체 개수 - 유일 개수).
	struct OrderCheck {
		size_t mismatch;
		size_t dup;
	};

	inline OrderCheck check_order(const std::vector<uint32_t>& seqs) {
		OrderCheck result{0, 0};
		for (size_t k = 0; k < seqs.size(); ++k) {
			if (seqs[k] != static_cast<uint32_t>(k)) {
				++result.mismatch;
			}
		}

		// 유일 개수는 정렬한 사본에서 std::unique 로 센다 — std::set 을
		// 새로 include 하지 않고 <algorithm> 하나로 끝낸다.
		std::vector<uint32_t> sorted_seqs(seqs);
		std::sort(sorted_seqs.begin(), sorted_seqs.end());
		const size_t unique_count = static_cast<size_t>(std::unique(sorted_seqs.begin(), sorted_seqs.end()) - sorted_seqs.begin());
		result.dup = seqs.size() - unique_count;

		return result;
	}

	// 판정 술어 — flow 의 각 단계 판정은 전부 이 둘을 거친다(파일 첫머리
	//   주석). expect_frame_min 은 LoginAck 처럼 가변 길이(host str16)인
	//   응답에 쓴다.
	inline bool expect_frame(const Frame& f, proto::MsgId id, size_t exact_body_len) {
		return f.id == id && f.body.size() == exact_body_len;
	}

	inline bool expect_frame_min(const Frame& f, proto::MsgId id, size_t min_body_len) {
		return f.id == id && f.body.size() >= min_body_len;
	}

	inline bool bytes_equal(const std::vector<uint8_t>& a, const uint8_t* b, size_t n) {
		if (a.size() != n) {
			return false;
		}
		if (n == 0) {
			// memcmp(nullptr, nullptr, 0) 은 표준상 정의되지 않은 인자
			// 조합이 될 수 있다 — a.data()/b 가 둘 다 비어 있을 때 굳이
			// memcmp 를 부르지 않고 "길이가 같은 빈 것은 같다"로 바로 끝낸다.
			return true;
		}
		return std::memcmp(a.data(), b, n) == 0;
	}

	// put_u64_be / put_u32_be — proto::write_*_be 를 감싸 out 뒤에 이어
	//   붙인다(끝에 덧붙이는 용도라 offset 인자가 없다).
	inline void put_u64_be(std::vector<uint8_t>& out, uint64_t v) {
		const size_t offset = out.size();
		out.resize(offset + 8);
		proto::write_u64_be(out.data() + offset, v);
	}

	inline void put_u32_be(std::vector<uint8_t>& out, uint32_t v) {
		const size_t offset = out.size();
		out.resize(offset + 4);
		proto::write_u32_be(out.data() + offset, v);
	}

	// echo_pattern — 'A' * n (send.ps1:47 동치).
	inline std::vector<uint8_t> echo_pattern(size_t n) {
		return std::vector<uint8_t>(n, static_cast<uint8_t>('A'));
	}

}	// namespace client
