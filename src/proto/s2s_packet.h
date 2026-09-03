//  proto/s2s_packet.h — 마을 서버 ↔ 세션 서버 간 S2S 프로토콜 규약
//
//  packet.h 와 네임스페이스를 분리한 이유 — 헤더 폭이 다르다(4B vs 8B, seq 유무).
//  같은 이름 공간에 섞으면 상수 하나만 봐서는 어느 프로토콜의 것인지 알 수 없다.
//  바이트 순서 변환 헬퍼(write_u16_be 류)는 값을 다루는 함수라 프로토콜에 무관하므로
//  packet.h 것을 그대로 재사용한다 — 같은 계층(proto) 안이라 방향 위반이 아니다.

#pragma once

#include "packet.h"

#include <cassert>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace proto {
namespace s2s {

	//  버전
	//
	//  마을이 세션 서버에 자기 프로토콜 버전을 Register 로 알린다. 세션 서버가
	//  감당 못 하는 버전이면 RegisterAck(result=version_rejected) 로 거절한다 —
	//  버전 불일치를 연결 자체가 아니라 핸드셰이크에서 가리는 것이 배포 중 롤아웃을
	//  덜 위험하게 만든다(붙긴 붙되 등록을 거절당하는 편이 소켓이 안 붙는 것보다
	//  진단하기 쉽다).
	constexpr uint16_t kVerMajor = 1;
	constexpr uint16_t kVerMinor = 0;

	// (kVerMajor << 8) | kVerMinor 의 산술 결과 타입은 int 다 (정수 승격). 캐스트 없이
	// uint16_t 를 반환하면 축소 변환 경고(C4244)가 /W4 에서 뜬다 — packet.h 의
	// read_u16_be 가 같은 이유로 바깥 캐스팅을 명시한 선례다.
	constexpr uint16_t ver() {
		return static_cast<uint16_t>((kVerMajor << 8) | kVerMinor);
	}

	//  헤더 — [ body_size : u16 ][ msg_id : u16 ][ seq : u32 ]   빅엔디언
	//
	//  msg_id 를 MsgId 로 바로 들고 있지 않고 원시 u16 인 이유 — 모르는 msg_id 가
	//  와도(신버전 상대) 헤더 디코드 자체는 성공해야 한다. "아는 값인가"의 판정은
	//  classify 콜백 몫이다(packet.h::is_client_request 와 같은 이유 — 분류와
	//  프레이밍을 섞지 않는다).
	//
	//  seq 가 있는 이유 — 클라-서버 프로토콜과 달리 S2S 는 양방향 요청-응답이다.
	//  마을도 세션 서버에 요청을 보내고 응답을 기다린다. 인플라이트 요청이 여러 건
	//  동시에 있을 수 있어 "어느 응답이 어느 요청 것인가"를 몸통이 아니라 헤더가
	//  실어야 한다 — 그래야 body 를 열어보지 않고도 매칭 테이블을 찾을 수 있다.
	struct Header {
		uint16_t body_size;
		uint16_t msg_id;
		uint32_t seq;
	};

	constexpr size_t kHeaderSize = 8;      // sizeof 가 아니라 규약이 정한 값
	constexpr uint16_t kMaxBodySize = 65535;

	//  헤더 직렬화 / 역직렬화 — 오프셋(0, 2, 4)을 이 두 함수 안에만 둔다.
	//  packet.h::encode_header/decode_header 와 같은 이유 — 필드를 추가하는 순간
	//  호출부마다 오프셋을 고치게 만들지 않기 위해서다.
	//
	//  호출자 책임 — dst/src 는 최소 kHeaderSize 바이트가 확보돼 있어야 한다.
	inline void encode_header(uint8_t* dst, const Header& header) {
		write_u16_be(dst + 0, header.body_size);
		write_u16_be(dst + 2, header.msg_id);
		write_u32_be(dst + 4, header.seq);
	}

	inline Header decode_header(const uint8_t* src) {
		Header header{};
		header.body_size = read_u16_be(src + 0);
		header.msg_id = read_u16_be(src + 2);
		header.seq = read_u32_be(src + 4);
		return header;
	}

	//  프레임 자르기 — net::FrameSizer 계약과 동일 (>0 완성 · 0 부족 · <0 위반).
	//  body_size 는 u16 이라 kMaxBodySize(65535) 를 넘는 값 자체를 표현할 수 없다 —
	//  즉 아래 초과 검사는 지금 상수로는 항상 거짓이다. 그래도 지우지 않는 이유는
	//  kMaxBodySize 를 나중에(운영상 더 좁게) 낮출 가능성에 대비하기 위해서다 —
	//  그때 이 검사가 없으면 상수만 바꾸고 프레이밍은 안 바뀌는 조용한 결함이 된다.
	inline int frame_size(const char* data, int len) {
		if (len < static_cast<int>(kHeaderSize)) {
			return 0;
		}
		const Header header = decode_header(reinterpret_cast<const uint8_t*>(data));
		if (header.body_size > kMaxBodySize) {
			return -1;
		}
		if (len - static_cast<int>(kHeaderSize) < header.body_size) {
			return 0;
		}
		return static_cast<int>(kHeaderSize) + header.body_size;
	}

	//  세션 서버가 S2S 수용 소켓에서 받아 주는 body 상한. 규약 상한(kMaxBodySize
	//  65535)과 별도인 이유 — net 의 수신 버퍼는 4100B 라, 그보다 큰 완성 프레임은
	//  sizer 가 0(부족)만 반복해 연결이 끊기지도 않고 영구히 멈춘다(net session.h
	//  주석의 함정). 담을 수 없는 크기는 조용한 멈춤 대신 규약 위반으로 명시
	//  절단한다. 4092 = 4100 - kHeaderSize(8) 이고, FullSync 청크 상한 4092B 는
	//  이 값에서 나온 확정값이다(ADR-020 결정 2). 버퍼와의 관계는 proto 가 net 을
	//  모르므로 둘 다 아는 층이 static_assert 로 강제한다.
	constexpr int kServerRecvBodyCap = 4092;

	//  frame_size 와 같되 body_size > kServerRecvBodyCap 을 위반(-1)으로 판정한다 —
	//  세션 서버 수용 소켓 전용 sizer 다. 마을 쪽(발신자·연결자)은 기존 frame_size
	//  를 그대로 쓴다.
	inline int frame_size_bounded(const char* data, int len) {
		if (len < static_cast<int>(kHeaderSize)) {
			return 0;
		}
		const Header header = decode_header(reinterpret_cast<const uint8_t*>(data));
		if (header.body_size > kServerRecvBodyCap) {
			return -1;
		}
		if (len - static_cast<int>(kHeaderSize) < header.body_size) {
			return 0;
		}
		return static_cast<int>(kHeaderSize) + header.body_size;
	}

	//  메시지 ID — 네 개 대역으로 나눈다.
	//    0x80xx  마을 → 세션 서버 (요청/통지)
	//    0x81xx  세션 서버 → 마을 (요청/통지 — 역방향)
	//    0x82xx  응답, 세션 서버가 보낸다 (마을이 보낸 0x80xx 요청에 대한 응답)
	//    0x83xx  응답, 마을이 보낸다 (세션 서버가 보낸 0x81xx 요청에 대한 응답)
	//
	//  응답 id 를 양방향에 재사용하지 않고 대역을 나눈 이유 — 예를 들어 하나의
	//  RegisterAck 값을 양쪽이 같이 쓰면, 그 프레임을 받은 쪽이 "이건 내가 보낸
	//  요청의 응답인가 아니면 상대가 보낸 요청인가"를 msg_id 만으로 가를 수 없다.
	//  대역을 나누면 classify 가 헤더만 보고 즉시 답한다.
	enum class MsgId : uint16_t {
		// 마을 → 세션 서버
		Register        = 0x8001,
		Unregister      = 0x8002,
		Heartbeat       = 0x8003,
		PlayerEnter     = 0x8004,
		PlayerLeave     = 0x8005,
		FullSync        = 0x8006,
		DrainComplete   = 0x8007,

		// 세션 서버 → 마을
		Reserve         = 0x8101,
		Kick            = 0x8102,
		SetMode         = 0x8103,

		// 응답 — 세션 서버가 보낸다 (마을의 0x80xx 요청에 대한 응답)
		RegisterAck     = 0x8201,
		UnregisterAck   = 0x8202,
		HeartbeatAck    = 0x8203,

		// 응답 — 마을이 보낸다 (세션 서버의 0x81xx 요청에 대한 응답)
		ReserveAck      = 0x8301,
		KickAck         = 0x8302,
		SetModeAck      = 0x8303,

		// 공통 — 모르는 msg_id 에 대한 회신. 방향 무관.
		Unsupported     = 0x8FFF,
	};

	//  분류 — 마을 쪽 분류기다. 0x82xx/0x8FFF 를 응답으로, 0x81xx 를 (역방향)
	//  요청으로 본다.
	inline bool is_reply_to_village(MsgId id) {
		return id == MsgId::RegisterAck
			|| id == MsgId::UnregisterAck
			|| id == MsgId::HeartbeatAck
			|| id == MsgId::Unsupported;
	}

	inline bool is_request_from_session(MsgId id) {
		return id == MsgId::Reserve
			|| id == MsgId::Kick
			|| id == MsgId::SetMode;
	}

	//  세션 쪽 분류기 — 위 둘의 대칭이다. 다만 is_request_from_village 의 의미
	//  폭은 is_request_from_session(순수 요청 3종)과 다르다 — 알림 대역
	//  (0x8004~0x8007, seq=0·무응답 규약)까지 포함한다. 요청/알림의 최종 구분을
	//  msg_id 나열로 여기서 가르지 않는 이유 — 그 구분은 헤더의 seq 가 이미 실어
	//  오므로(알림은 seq=0), 라우터가 seq==0 으로 재분류한다. 여기서는 「마을이
	//  보내오는 것 전부」만 가른다.
	inline bool is_request_from_village(MsgId id) {
		return id == MsgId::Register
			|| id == MsgId::Unregister
			|| id == MsgId::Heartbeat
			|| id == MsgId::PlayerEnter
			|| id == MsgId::PlayerLeave
			|| id == MsgId::FullSync
			|| id == MsgId::DrainComplete;
	}

	inline bool is_reply_to_session(MsgId id) {
		return id == MsgId::ReserveAck
			|| id == MsgId::KickAck
			|| id == MsgId::SetModeAck
			|| id == MsgId::Unsupported;
	}

	//  문자열 — u16 길이(바이트 수) 선행 + UTF-8 그대로 (ADR-019 결정 7).
	//  길이를 "문자 수"가 아니라 "바이트 수"로 잡은 이유 — UTF-8 은 가변 바이트라
	//  문자 수는 셀 필요가 없고, 프레이밍이 원하는 건 애초에 몇 바이트를
	//  건너뛰면 되는가이지 몇 글자인가가 아니다.
	// 호출자 계약 — value 는 0xFFFF 바이트를 넘지 않는다. 넘으면 길이 필드가
	// 잘려서(uint16_t 캐스트) 뒤에 붙는 바이트 수와 필드가 말하는 길이가 어긋나는데,
	// 이 조용한 절단은 이 저장소가 반복해서 겪은 함정 유형이다 — 여기서는 호출 자체를
	// assert 로 막는다(1단계의 host 는 짧은 설정값이라 실측 위험은 낮다).
	inline void encode_str16(std::vector<char>& out, const std::string& value) {
		assert(value.size() <= 0xFFFF);
		const uint16_t len = static_cast<uint16_t>(value.size());
		const size_t offset = out.size();
		out.resize(offset + 2 + len);
		write_u16_be(reinterpret_cast<uint8_t*>(out.data() + offset), len);
		if (len > 0) {
			std::memcpy(out.data() + offset + 2, value.data(), len);
		}
	}

	// data/len 은 문자열이 시작하는 지점부터 남은 바이트 수 — 이미 frame_size 로
	// 프레임 전체 길이는 확정된 뒤지만, 그 안에서 길이 필드가 가리키는 값이 남은
	// 공간을 넘는지는 여기서 다시 봐야 한다(body 안 필드 하나의 자체 무결성이라
	// frame_size 가 대신 봐주지 않는다).
	inline bool decode_str16(const char* data, int len, std::string& out, int& consumed) {
		if (len < 2) {
			return false;
		}
		const uint16_t str_len = read_u16_be(reinterpret_cast<const uint8_t*>(data));
		if (len - 2 < str_len) {
			return false;
		}
		out.assign(data + 2, str_len);
		consumed = 2 + str_len;
		return true;
	}

	//  body — 1단계에서 오가는 4.5종 (HeartbeatAck·Unsupported 는 0B 라 반쪽으로 센다)

	// ver 는 상대가 보낸 프로토콜 버전 그대로를 담아 되돌아볼 수 있게 한다(로그 등) —
	// 검증은 수신측이 proto::s2s::ver() 와 비교해서 한다(이 파일은 비교를 강제하지
	// 않는다 — 그건 정책이고 app 계층 몫).
	struct Register {
		uint16_t ver;
		uint16_t port;
		uint32_t capacity;
		uint32_t current;
		std::string host;
	};

	inline std::vector<char> encode_register(const Register& msg) {
		std::vector<char> body(12);
		uint8_t* p = reinterpret_cast<uint8_t*>(body.data());
		write_u16_be(p + 0, msg.ver);
		write_u16_be(p + 2, msg.port);
		write_u32_be(p + 4, msg.capacity);
		write_u32_be(p + 8, msg.current);
		encode_str16(body, msg.host);
		return body;
	}

	inline bool decode_register(const char* data, int len, Register& out) {
		if (len < 12) {
			return false;
		}
		const uint8_t* p = reinterpret_cast<const uint8_t*>(data);
		out.ver = read_u16_be(p + 0);
		out.port = read_u16_be(p + 2);
		out.capacity = read_u32_be(p + 4);
		out.current = read_u32_be(p + 8);
		int consumed = 0;
		if (!decode_str16(data + 12, len - 12, out.host, consumed)) {
			return false;
		}
		// host 뒤에 남는 바이트가 있으면 body_size 가 실제 필드 합보다 크다는
		// 뜻이다 — §8-4 「가변 길이는 count 와 실제 본문 크기가 일치해야 한다」의
		// Register 판. 꼬리 바이트를 무시하고 통과시키면 프레이밍은 안 깨지지만
		// 그 여분이 손상인지 의도인지 아무도 모른 채 조용히 버려진다.
		return 12 + consumed == len;
	}

	// result 값 — 0=ok · 1=version_rejected · 2=full · 3=draining.
	// full 은 2단계(세션 서버가 실제로 정원을 판단)의 소관이라 여기서는 값만
	// 예약해 둔다. RegisterAck·ReserveAck 가 이 값들을 공유한다(둘 다 uint8_t
	// result 하나뿐이다) — draining 은 4단계(SetMode)가 드레인 중 Reserve 를
	// 거절할 때 ReserveAck 가 쓴다.
	constexpr uint8_t kResultOk = 0;
	constexpr uint8_t kResultVersionRejected = 1;
	constexpr uint8_t kResultFull = 2;
	constexpr uint8_t kResultDraining = 3;

	struct RegisterAck {
		uint32_t server_id;
		uint8_t result;
	};

	inline std::vector<char> encode_register_ack(const RegisterAck& msg) {
		std::vector<char> body(5);
		write_u32_be(reinterpret_cast<uint8_t*>(body.data()), msg.server_id);
		body[4] = static_cast<char>(msg.result);
		return body;
	}

	// 고정 5B — 길이가 다르면 규약 위반이다(app 계층이 != 로 검사해 끊는다. 여기는
	// 판정만 하고 처분은 안 한다 — packet.h 의 관례와 같다).
	inline bool decode_register_ack(const char* data, int len, RegisterAck& out) {
		if (len != 5) {
			return false;
		}
		const uint8_t* p = reinterpret_cast<const uint8_t*>(data);
		out.server_id = read_u32_be(p + 0);
		out.result = p[4];
		return true;
	}

	struct Heartbeat {
		uint32_t current;
	};

	inline std::vector<char> encode_heartbeat(const Heartbeat& msg) {
		std::vector<char> body(4);
		write_u32_be(reinterpret_cast<uint8_t*>(body.data()), msg.current);
		return body;
	}

	inline bool decode_heartbeat(const char* data, int len, Heartbeat& out) {
		if (len != 4) {
			return false;
		}
		out.current = read_u32_be(reinterpret_cast<const uint8_t*>(data));
		return true;
	}

	// HeartbeatAck / Unsupported — 둘 다 0B. seq 가 헤더에 이미 있어서 body 로
	// 옮길 정보가 없다(누구의 무엇에 대한 응답인지는 seq 매칭 테이블이 안다).
	// encode 가 인자를 받지 않는 것도 같은 이유 — 실을 값이 없다.
	inline std::vector<char> encode_heartbeat_ack() {
		return {};
	}

	inline bool decode_heartbeat_ack(int len) {
		return len == 0;
	}

	inline std::vector<char> encode_unsupported() {
		return {};
	}

	inline bool decode_unsupported(int len) {
		return len == 0;
	}

	//  body — 2단계(세션 서버)부터 오가는 것들

	// Unregister / UnregisterAck — 둘 다 0B 로 확정한다. §8-3 표가 body 를 "—" 로
	// 비워 둔 것의 명문화다 — 「누가」는 연결 자체가 식별하므로 실을 값이 없다.
	// HeartbeatAck 와 같은 이유·같은 형태다.
	inline std::vector<char> encode_unregister() {
		return {};
	}

	inline bool decode_unregister(int len) {
		return len == 0;
	}

	inline std::vector<char> encode_unregister_ack() {
		return {};
	}

	inline bool decode_unregister_ack(int len) {
		return len == 0;
	}

	// player_id 가 u64 인 이유 — 세션 서버의 클라 로그인 규약(kSessionLoginReq)이
	// u64 로 시작했고 이 값은 그대로 마을에 전달돼야 한다. 마을 내부의 u32 와의
	// 폭 통일은 3단계 몫이다(ADR-020 결정 4).
	// expire_ms 는 예약의 유효 시간이다 — 절대 시각을 싣지 않는 이유는 두 서버의
	// 시계가 같다는 보장이 없어서다(수신 측이 자기 시계 기준으로 만료를 잰다).
	struct Reserve {
		uint64_t player_id;
		uint32_t expire_ms;
	};

	inline std::vector<char> encode_reserve(const Reserve& msg) {
		std::vector<char> body(12);
		uint8_t* p = reinterpret_cast<uint8_t*>(body.data());
		write_u64_be(p + 0, msg.player_id);
		write_u32_be(p + 8, msg.expire_ms);
		return body;
	}

	// 고정 12B — 길이가 다르면 규약 위반이다(register_ack 와 같은 관례 — 판정만
	// 하고 처분은 위층 몫).
	inline bool decode_reserve(const char* data, int len, Reserve& out) {
		if (len != 12) {
			return false;
		}
		const uint8_t* p = reinterpret_cast<const uint8_t*>(data);
		out.player_id = read_u64_be(p + 0);
		out.expire_ms = read_u32_be(p + 8);
		return true;
	}

	struct ReserveAck {
		uint8_t result;
	};

	inline std::vector<char> encode_reserve_ack(const ReserveAck& msg) {
		std::vector<char> body(1);
		body[0] = static_cast<char>(msg.result);
		return body;
	}

	inline bool decode_reserve_ack(const char* data, int len, ReserveAck& out) {
		if (len != 1) {
			return false;
		}
		out.result = static_cast<uint8_t>(data[0]);
		return true;
	}

	//  body — 3단계(마을 연동)부터 오가는 것들

	// player_id 만 싣는 이유 — 이 알림은 「누가 들어왔다」는 사실 전달이 전부이고,
	// 세션 서버가 자기 접속 테이블을 갱신하는 데 필요한 값은 이것뿐이다. 알림이라
	// seq=0 으로 보낸다 — Reserve 류의 요청-응답 흐름과 다르다.
	struct PlayerEnter {
		uint64_t player_id;
	};

	inline std::vector<char> encode_player_enter(const PlayerEnter& msg) {
		std::vector<char> body(8);
		write_u64_be(reinterpret_cast<uint8_t*>(body.data()), msg.player_id);
		return body;
	}

	// 고정 8B — 길이가 다르면 규약 위반이다(register_ack 와 같은 관례 — 판정만
	// 하고 처분은 위층 몫).
	inline bool decode_player_enter(const char* data, int len, PlayerEnter& out) {
		if (len != 8) {
			return false;
		}
		out.player_id = read_u64_be(reinterpret_cast<const uint8_t*>(data));
		return true;
	}

	// PlayerEnter 와 body 모양이 같지만 별도 구조체·별도 함수다 — 이름 자체가
	// 방향(입장/퇴장)을 말해야 호출부가 msg_id 분기 없이도 실수로 반대 함수를
	// 부르는 것을 컴파일 타임에 걸러낸다.
	struct PlayerLeave {
		uint64_t player_id;
	};

	inline std::vector<char> encode_player_leave(const PlayerLeave& msg) {
		std::vector<char> body(8);
		write_u64_be(reinterpret_cast<uint8_t*>(body.data()), msg.player_id);
		return body;
	}

	inline bool decode_player_leave(const char* data, int len, PlayerLeave& out) {
		if (len != 8) {
			return false;
		}
		out.player_id = read_u64_be(reinterpret_cast<const uint8_t*>(data));
		return true;
	}

	// 한 FullSync 청크에 담을 수 있는 인원 상한 — kServerRecvBodyCap 에서 역산한
	// 파생값이다. 고정 6B(chunk_idx 2 + chunk_total 2 + count 2) 를 뺀 나머지를
	// player_id 8B 로 나눈 몫이 그 프레임에 담을 수 있는 최대치다. 매직 넘버로
	// 박지 않는 이유 — kServerRecvBodyCap 이 나중에 바뀌면 이 값도 같이
	// 움직여야 하는데, 상수로 따로 적으면 그 연결이 조용히 끊긴다.
	constexpr int kFullSyncMaxPerChunk = (kServerRecvBodyCap - 6) / 8;

	// player_id 만 담는 이유 — 마을이 세션 서버에 넘기는 것은 「지금 이 서버에
	// 있는 입장 인원의 식별자 목록」이 전부다. 인원이 kFullSyncMaxPerChunk 를
	// 넘으면 한 프레임에 다 못 담아 여러 프레임으로 나눠 보내야 하는데,
	// chunk_idx/chunk_total 이 있어야 수신측이 마지막 조각이 왔는지 판단하고
	// 조각들을 이어붙일 수 있다.
	//
	// count 를 따로 싣는 이유 — body_size 만으로도 (body_size-6)/8 로 인원수를
	// 역산할 수는 있지만, 손상된 프레임이 8의 배수가 아닌 잉여 바이트를 달고
	// 오면 그 역산이 조용히 그럴듯한 값을 낸다. count 를 명시하고 decode 에서
	// 그 값과 실제 크기의 일치를 검사하면 그 손상이 규약 위반으로 즉시 걸린다.
	struct FullSync {
		uint16_t chunk_idx;
		uint16_t chunk_total;
		std::vector<uint64_t> player_ids;
	};

	inline std::vector<char> encode_full_sync(const FullSync& msg) {
		// 0xFFFF(count 필드 폭)가 아니라 청크 상한으로 건다 — 그보다 많이 담으면
		// 프레임이 kServerRecvBodyCap 을 넘어 받는 쪽이 규약 위반으로 끊는데,
		// 그 증상은 「연결이 끊겼다」로 나타나 나눠 보내지 않은 호출부까지
		// 되짚어 가기 멀다. 나누는 책임은 호출부에 있고 여기서 그것을 감시한다.
		assert(msg.player_ids.size() <= static_cast<size_t>(kFullSyncMaxPerChunk));
		const uint16_t count = static_cast<uint16_t>(msg.player_ids.size());
		std::vector<char> body(6 + static_cast<size_t>(count) * 8);
		uint8_t* p = reinterpret_cast<uint8_t*>(body.data());
		write_u16_be(p + 0, msg.chunk_idx);
		write_u16_be(p + 2, msg.chunk_total);
		write_u16_be(p + 4, count);
		for (uint16_t i = 0; i < count; ++i) {
			write_u64_be(p + 6 + static_cast<size_t>(i) * 8, msg.player_ids[i]);
		}
		return body;
	}

	// count 와 실제 본문 크기의 일치를 검사한다 — 손상된 프레임이 count 만
	// 그럴듯하게 달고 오는 것을 여기서 걸러야, 청크를 이어붙이는 위층이 그
	// 손상을 「적은 인원」으로 오인하지 않는다.
	inline bool decode_full_sync(const char* data, int len, FullSync& out) {
		if (len < 6) {
			return false;
		}
		const uint8_t* p = reinterpret_cast<const uint8_t*>(data);
		out.chunk_idx = read_u16_be(p + 0);
		out.chunk_total = read_u16_be(p + 2);
		const uint16_t count = read_u16_be(p + 4);
		if (6 + static_cast<size_t>(count) * 8 != static_cast<size_t>(len)) {
			return false;
		}
		out.player_ids.resize(count);
		for (uint16_t i = 0; i < count; ++i) {
			out.player_ids[i] = read_u64_be(p + 6 + static_cast<size_t>(i) * 8);
		}
		return true;
	}

	//  body — 4단계(드레인)부터 오가는 것들

	// mode — 0=Running · 1=Draining. 세션 서버가 SetMode 로 보내고 마을이 같은
	// seq 로 SetModeAck 를 돌려준다 — Reserve/ReserveAck 왕복과 같은 모양이다.
	struct SetMode {
		uint8_t mode;
	};

	inline std::vector<char> encode_set_mode(const SetMode& msg) {
		std::vector<char> body(1);
		body[0] = static_cast<char>(msg.mode);
		return body;
	}

	// 고정 1B — 길이가 다르면 규약 위반이다(register_ack 와 같은 관례).
	inline bool decode_set_mode(const char* data, int len, SetMode& out) {
		if (len != 1) {
			return false;
		}
		out.mode = static_cast<uint8_t>(data[0]);
		return true;
	}

	// current — SetMode 수신 시점의 입장 인원. 세션 서버가 드레인 진행 상황을
	// 이 값 하나로 가늠한다(별도 조회 왕복이 없다).
	struct SetModeAck {
		uint32_t current;
	};

	inline std::vector<char> encode_set_mode_ack(const SetModeAck& msg) {
		std::vector<char> body(4);
		write_u32_be(reinterpret_cast<uint8_t*>(body.data()), msg.current);
		return body;
	}

	inline bool decode_set_mode_ack(const char* data, int len, SetModeAck& out) {
		if (len != 4) {
			return false;
		}
		out.current = read_u32_be(reinterpret_cast<const uint8_t*>(data));
		return true;
	}

	// remaining — 드레인 완료 시점의 남은 인원. 마을은 current()==0 일 때만
	// 이 알림을 보내므로(§app::S2sLink::on_tick) 지금은 항상 0 으로 나간다.
	// PlayerEnter/PlayerLeave 와 같은 알림이라 seq=0 규약이고 응답이 없다.
	struct DrainComplete {
		uint32_t remaining;
	};

	inline std::vector<char> encode_drain_complete(const DrainComplete& msg) {
		std::vector<char> body(4);
		write_u32_be(reinterpret_cast<uint8_t*>(body.data()), msg.remaining);
		return body;
	}

	inline bool decode_drain_complete(const char* data, int len, DrainComplete& out) {
		if (len != 4) {
			return false;
		}
		out.remaining = read_u32_be(reinterpret_cast<const uint8_t*>(data));
		return true;
	}

	// player_id 는 세션 서버가 지목한 대상. reason 을 지금 하나뿐인데도 값으로
	// 남겨 두는 이유는 SetMode 의 mode 와 같다 — 사유가 늘어도 이 구조체의 폭이
	// 안 바뀐다.
	struct Kick {
		uint64_t player_id;
		uint8_t reason;
	};

	constexpr uint8_t kKickReasonDuplicate = 0;

	inline std::vector<char> encode_kick(const Kick& msg) {
		std::vector<char> body(9);
		uint8_t* p = reinterpret_cast<uint8_t*>(body.data());
		write_u64_be(p + 0, msg.player_id);
		p[8] = static_cast<uint8_t>(msg.reason);
		return body;
	}

	// 고정 9B — 길이가 다르면 규약 위반이다(register_ack 와 같은 관례).
	inline bool decode_kick(const char* data, int len, Kick& out) {
		if (len != 9) {
			return false;
		}
		const uint8_t* p = reinterpret_cast<const uint8_t*>(data);
		out.player_id = read_u64_be(p + 0);
		out.reason = p[8];
		return true;
	}

	// result 는 「끊음을 개시했다」와 「이미 없었다」 둘 다 성공이다(§8-2 「없음도
	// 성공」이 멱등의 정의) — 노리던 결과(그 세션이 이제 없다)가 어느 쪽이든
	// 이루어져 있어서다. NotFound 는 그래서 실패 신호가 아니라, 세션 쪽 접속
	// 테이블에 남은 유령 항목을 지우라는 신호로 쓰인다(session_router.cpp 참조).
	struct KickAck {
		uint8_t result;
	};

	constexpr uint8_t kKickResultKicked = 0;
	constexpr uint8_t kKickResultNotFound = 1;

	inline std::vector<char> encode_kick_ack(const KickAck& msg) {
		std::vector<char> body(1);
		body[0] = static_cast<char>(msg.result);
		return body;
	}

	inline bool decode_kick_ack(const char* data, int len, KickAck& out) {
		if (len != 1) {
			return false;
		}
		out.result = static_cast<uint8_t>(data[0]);
		return true;
	}

	static_assert(kHeaderSize == 8,
		"s2s 헤더는 8바이트로 고정이다 — encode_header/decode_header 의 오프셋(0,2,4)이 이 값을 전제한다");
	static_assert(kMaxBodySize <= 0xFFFF,
		"body_size 필드가 u16 이라 0xFFFF 를 넘는 값은 애초에 표현이 안 된다");

	// 축소값(예: fullsync_chunk_max=3)으로 돌리는 하네스는 청크 분할 루프만
	// 재고, kFullSyncMaxPerChunk 라는 파생값 자체가 kServerRecvBodyCap 안에
	// 들어가는 산술은 재지 않는다 — 효과값이 항상 축소값이라 510 이 실제로
	// 쓰이는 경로가 하네스에서 한 번도 실행되지 않기 때문이다. static_assert
	// 로 그 마진을 컴파일 타임에 대신 잡는다.
	static_assert(kFullSyncMaxPerChunk * 8 + 6 <= kServerRecvBodyCap,
		"FullSync 청크가 세션 수신 한도를 넘는다");

}	// namespace s2s
}	// namespace proto
