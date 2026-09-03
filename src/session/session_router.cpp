#include "session/session_router.h"

#include "net/iocp_server.h"
#include "proto/packet.h"
#include "proto/s2s_packet.h"
#include "core/log.h"

#include <optional>
#include <vector>

// net 도 proto 도 서로를 모른다. 둘 다 아는 이 층이 관계를 강제한다 — 어긋나면
// 링크도 실행도 아닌 컴파일 단계에서 잡힌다(마을의 s2s_link.cpp·frame_router.cpp 와
// 같은 자리). 캐스트를 명시하는 것도 그 선례와 같다 — signed/unsigned 비교 경고 봉쇄.
static_assert(proto::s2s::kServerRecvBodyCap + static_cast<int>(proto::s2s::kHeaderSize)
		<= net::kRecvBufferSize,
	"S2S 수용 상한(kServerRecvBodyCap)+헤더가 수신 버퍼를 넘는다 — frame_size_bounded 의 전제가 깨진다");
static_assert(static_cast<int>(proto::kHeaderSize) + static_cast<int>(proto::kMaxBodySize)
		<= net::kRecvBufferSize,
	"클라 프레임(헤더+최대 본문)이 수신 버퍼를 넘는다 — 그 프레임은 영원히 완성되지 않는다");

namespace {

	// 처리할 수 없는 클라 메시지 정책 — 마을 frame_router.cpp 의 상수와 같은 값이다.
	// 그 파일은 app 소속이라 include 할 수 없어(계층 — session 은 app 을 모른다)
	// 여기 재정의한다. 값·근거·레이트 리밋 전부 그쪽 주석이 정본이다.
	constexpr int kBadMsgLimit = 32;
	constexpr int kScoreUnknownId = 1;
	constexpr int kScoreServerOnlyId = 4;

	constexpr bool should_log_bad_msg(int score) {
		return (score & (score - 1)) == 0;
	}

	// 클라 쪽 프레임 자르기 — 마을 frame_router 의 판과 같은 규칙(4B 헤더 ·
	// body_size 상한 위반만 절단). 그 함수는 app 소속이라 재구현한다.
	int client_frame_size(const char* data, int len) {
		if (len < static_cast<int>(proto::kHeaderSize)) {
			return 0;
		}
		const proto::PacketHeader head =
			proto::decode_header(reinterpret_cast<const uint8_t*>(data));
		if (head.body_size > proto::kMaxBodySize) {
			return -1;
		}
		if (len - static_cast<int>(proto::kHeaderSize) < head.body_size) {
			return 0;
		}
		return static_cast<int>(proto::kHeaderSize) + head.body_size;
	}

	// 아는 메시지인데 모양이 다르다 — 버전 스큐와 달리 봐줄 근거가 없다. 로그를
	// 빠뜨릴 자리를 없애려고 함수로 뺀 것까지 마을 bad_body 와 같다.
	bool bad_body(const net::Session& session, const char* what, int got, int want) {
		core::logf("[WARN] #%llu %s body=%d (want %d) — closing\n",
			static_cast<unsigned long long>(session.id), what, got, want);
		return false;
	}

	bool send_client_msg(net::IocpServer& server, net::Session& to, proto::MsgId id,
			const char* body, int body_len) {
		uint8_t head[proto::kHeaderSize];
		const proto::PacketHeader h{ static_cast<uint16_t>(body_len), id };
		proto::encode_header(head, h);
		const net::SendChunk parts[2] = {
			{ reinterpret_cast<const char*>(head), static_cast<int>(proto::kHeaderSize) },
			{ body, body_len },
		};
		return server.send_chunks(to, parts, body_len > 0 ? 2 : 1);
	}

	bool send_s2s_msg(net::IocpServer& server, net::Session& to, proto::s2s::MsgId id,
			uint32_t seq, const char* body, int body_len) {
		uint8_t head[proto::s2s::kHeaderSize];
		const proto::s2s::Header h{ static_cast<uint16_t>(body_len),
			static_cast<uint16_t>(id), seq };
		proto::s2s::encode_header(head, h);
		const net::SendChunk parts[2] = {
			{ reinterpret_cast<const char*>(head), static_cast<int>(proto::s2s::kHeaderSize) },
			{ body, body_len },
		};
		return server.send_chunks(to, parts, body_len > 0 ? 2 : 1);
	}

}	// namespace

namespace session {

	Router::Router(Registry& registry, const RouterSettings& settings)
		: registry_(registry)
		, settings_(settings) {
	}

	void Router::wire(net::IocpServer& client_server, net::IocpServer& s2s_server) {
		client_ = &client_server;
		s2s_ = &s2s_server;

		// job_sink 는 「즉시 실행」이다 — 받은 Job 을 그 자리(I/O 워커)에서 동기
		// 호출한다. 없으면 동작 불능이다: net 은 job_sink 가 null 이면 버퍼를
		// 반납하고 false 를 돌려 모든 연결이 첫 프레임에서 절단되고, recv 핸들러는
		// job_sink 가 받은 Job 안에서만 불린다(iocp_server.cpp 의 drain_frames).
		// 존 스레드가 없는 이 exe 에서는 이 형태가 곧 「I/O 워커 인라인 실행」이다.
		const auto inline_sink = [](net::Session&, core::Job job) {
			job();
			return true;
		};
		client_server.set_job_sink(inline_sink);
		s2s_server.set_job_sink(inline_sink);

		client_server.set_frame_sizer(client_frame_size);
		s2s_server.set_frame_sizer(proto::s2s::frame_size_bounded);

		client_server.set_recv_handler(
			[this](net::Session& s, const char* frame, int len) {
				return on_client_frame(s, frame, len);
			});
		s2s_server.set_recv_handler(
			[this](net::Session& s, const char* frame, int len) {
				return on_s2s_frame(s, frame, len);
			});

		// 클라 쪽은 session_gone 을 안 꽂는다 — 클라 세션은 어떤 테이블에도 담기지
		// 않으므로(홀드 없음) 빼 줄 자료구조가 없다.
		s2s_server.set_session_gone(
			[this](net::Session& s) {
				return on_s2s_gone(s);
			});
	}

	bool Router::request_set_mode(uint32_t server_id, bool draining) {
		std::lock_guard<std::mutex> lock(mutex_);
		if (!registry_.set_draining(server_id, draining)) {
			core::logf("[WARN] setmode: unknown server_id=%u\n", server_id);
			return false;
		}
		net::Session* link = registry_.link_of(server_id);
		if (link == nullptr) {
			core::logf("[INFO] setmode server_id=%u draining=%d — 링크 없음(상태만 반영)\n",
				server_id, draining ? 1 : 0);
			return true;
		}
		// seq 는 소비하지만(발신 프레임에 실어야 하니) pending 에는 안 태운다 —
		// SetModeAck 를 그 자리에서 바로 로그로만 다루기 때문이다(on_s2s_frame
		// 의 is_reply_to_session 분기 참조). pending 에 넣으면 응답이 안 와도
		// 타임아웃 회수 대상이 되는데, 이 요청에는 실패해도 재시도할 대상
		// 카운터(reserve_* 류)가 없어 그 회수가 아무 항등식도 안 채운다.
		LinkPending& lp = pending_[link->id];
		const uint32_t seq = lp.next_seq++;
		const proto::s2s::SetMode msg{ static_cast<uint8_t>(draining ? 1 : 0) };
		const std::vector<char> mbody = proto::s2s::encode_set_mode(msg);
		if (!send_s2s_msg(*s2s_, *link, proto::s2s::MsgId::SetMode, seq,
				mbody.data(), static_cast<int>(mbody.size()))) {
			core::logf("[WARN] setmode send 실패 server_id=%u seq=%u\n", server_id, seq);
		}
		return true;
	}

	bool Router::on_client_frame(net::Session& s, const char* frame, int len) {
		// sizer 가 완성 프레임만 올리므로 여기서는 값 검증만 한다.
		const proto::PacketHeader head =
			proto::decode_header(reinterpret_cast<const uint8_t*>(frame));
		const char* body = frame + proto::kHeaderSize;
		const int body_len = len - static_cast<int>(proto::kHeaderSize);

		if (proto::is_session_client_request(head.msg_id)) {
			switch (head.msg_id) {
			case proto::MsgId::kSessionLoginReq:
				return handle_session_login(s, body, body_len);

			// 마을 판과 같은 이유로 아무 일도 안 하고 답만 한다 — last_recv_ms 는
			// net 이 수신 완료 지점에서 이미 밀었다.
			case proto::MsgId::kPingReq:
				return send_client_msg(*client_, s, proto::MsgId::kPongAck, nullptr, 0);

			default:
				break;
			}
		}

		// 처리할 수 없는 메시지 — 끊지 않고 이 프레임만 버리며 점수를 센다.
		// 마을 handle_unhandled 의 규율 준용(값·레이트 리밋의 정본은 그쪽 주석).
		// 마을 전용 요청(kJoinZoneReq 등)도 여기로 온다 — 이 서버가 처리해 주는
		// 요청이 아니라는 점에서 미정의와 같은 무게다.
		const int add = proto::is_server_message(head.msg_id)
			? kScoreServerOnlyId : kScoreUnknownId;
		const int score = s.bad_msg_score.fetch_add(add, std::memory_order_relaxed) + add;
		if (should_log_bad_msg(score)) {
			core::logf("[WARN] #%llu unhandled msg_id=%u (%s) score=%d/%d\n",
				static_cast<unsigned long long>(s.id),
				static_cast<unsigned>(head.msg_id),
				proto::is_server_message(head.msg_id) ? "server-only" : "unknown",
				score, kBadMsgLimit);
		}
		if (score > kBadMsgLimit) {
			core::logf("[WARN] #%llu bad msg score %d > %d — closing\n",
				static_cast<unsigned long long>(s.id), score, kBadMsgLimit);
			return false;
		}
		return true;
	}

	bool Router::has_pending_kick(const LinkPending& lp, uint64_t player_id) {
		// 선형 스캔으로 충분하다 — kick_by_seq 는 응답·타임아웃이 곧 회수하는
		// 소수 항목이고, player_id 역인덱스는 그 수명 관리에 실수 표면만 더한다.
		for (const auto& kv : lp.kick_by_seq) {
			if (kv.second.player_id == player_id) {
				return true;
			}
		}
		return false;
	}

	bool Router::handle_session_login(net::Session& s, const char* body, int body_len) {
		if (body_len != 8) {
			return bad_body(s, "session-login", body_len, 8);
		}
		const uint64_t player_id =
			proto::read_u64_be(reinterpret_cast<const uint8_t*>(body));
		const uint64_t now = GetTickCount64();

		// busy 는 「이미 다른 세션이 이 player_id 로 붙어 있다」다 — 테이블
		// 게이트가 공존 창 0 을 만든다(LoginAck(Ok) 는 connections_ 에 그
		// player 항목이 없을 때만 나가고, 항목 제거는 마을의 실제 정리 신호
		// PlayerLeave 로만 일어난다 — ADR-024 결정 2). 대기 없는 즉시 거절이라
		// 클라 세션을 붙드는 기구가 여전히 없다.
		bool busy = false;
		std::optional<Assignment> assigned;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			const std::optional<uint32_t> dup = registry_.find_connection(player_id);
			if (dup.has_value()) {
				net::Session* link = registry_.link_of(*dup);
				if (link == nullptr) {
					// orphan — 이 서버로는 Kick 을 보낼 수 없다(§5-3 그대로 —
					// 대상이 죽은 채 유예 중이면 다른 서버 배정을 허용한다).
					// busy 를 안 세우고 아래 기존 assign 흐름으로 그대로 진행한다.
				} else {
					busy = true;
					LinkPending& lp = pending_[link->id];
					if (!has_pending_kick(lp, player_id)) {
						// Reserve 선례와 같은 뮤텍스 안 발신(registry.h 의 link_of 계약 —
						// 뮤텍스 안에서 조회와 send 를 마쳐야 탈착·release 와 배타).
						const uint32_t seq = lp.next_seq++;
						const proto::s2s::Kick msg{ player_id, proto::s2s::kKickReasonDuplicate };
						const std::vector<char> kbody = proto::s2s::encode_kick(msg);
						if (!send_s2s_msg(*s2s_, *link, proto::s2s::MsgId::Kick, seq,
								kbody.data(), static_cast<int>(kbody.size()))) {
							core::logf("[WARN] kick send 실패 server_id=%u seq=%u\n", *dup, seq);
						}
						lp.kick_by_seq[seq] = PendingKick{ now, player_id, *dup };
						kick_sent_.fetch_add(1, std::memory_order_relaxed);
					}
					// 재발신 억제 여부와 무관하게 Busy 는 매번 낸다 — 이미 낸
					// Kick 의 결과를 기다리라는 뜻이지, 이번 시도 자체가 무시된
					// 것은 아니다.
					login_busy_.fetch_add(1, std::memory_order_relaxed);
				}
			}

			if (!busy) {
				assigned = registry_.assign(player_id, settings_.reserve_expire_ms, now);
				if (assigned.has_value()) {
					// Reserve 발신 — 뮤텍스 안이므로 send 도중 탈착·release 가 끼어들
					// 수 없다. 다른 스레드에서의 send 자체는 기존 패턴이다(Session
					// 송신 큐는 자체 뮤텍스).
					net::Session* link = registry_.link_of(assigned->server_id);
					if (link == nullptr) {
						// orphan 직후 등 링크 부재 상태 — 발신만 스킵하고 LoginAck 는
						// 그대로 발급한다(배정 자체는 유효하다 — 링크가 없을 뿐이다).
						reserve_skip_nolink_.fetch_add(1, std::memory_order_relaxed);
						core::logf("[INFO] reserve skip (no link) server_id=%u player=%llu\n",
							assigned->server_id,
							static_cast<unsigned long long>(player_id));
					} else {
						LinkPending& lp = pending_[link->id];
						const uint32_t seq = lp.next_seq++;
						const proto::s2s::Reserve msg{ player_id, settings_.reserve_expire_ms };
						const std::vector<char> rbody = proto::s2s::encode_reserve(msg);
						if (!send_s2s_msg(*s2s_, *link, proto::s2s::MsgId::Reserve, seq,
								rbody.data(), static_cast<int>(rbody.size()))) {
							// 송신 큐 상한 등 — 이 Reserve 는 pending 타임아웃이
							// 회수하고, 연결 정리는 net(session_gone 경유)의 몫이다.
							reserve_send_fail_.fetch_add(1, std::memory_order_relaxed);
							core::logf("[WARN] reserve send 실패 server_id=%u seq=%u\n",
								assigned->server_id, seq);
						}
						lp.by_seq[seq] = PendingReserve{ now, player_id,
							assigned->server_id, assigned->issue_id };
						reserve_sent_.fetch_add(1, std::memory_order_relaxed);
					}
				}
			}
		}

		// 응답 본문 상한(kMaxBodySize 4096) 검증 — host 는 Register 가 실어 온
		// 가변 문자열이다. 현 상한 조합에서는 도달 불가다: S2S 수용 상한이 body
		// 4092B 라 host 는 최대 4078B(Register 고정부 14B 제외), 5+4078=4083 ≤
		// 4096. 그래도 남기는 이유 — 두 상한은 서로 모르는 파일에서 다른 근거로
		// 정해져 있어 한쪽이 갈라지는 순간 이 경로가 살아난다. 잘라 보내면 접속
		// 불가능한 주소가 되므로 자르는 대신 배정 실패로 답한다 — 그 예약은
		// expire 회수가 치운다. 이 강등은 assign() 뒤라 [SESS ] assign ok 가 이
		// 건을 ok 로 세는 어긋남이 있다 — 도달 불가라 무해하고, 상한이 갈라져
		// 이 경로가 살아나면 집계도 같이 고친다.
		if (assigned.has_value()
			&& 5 + assigned->host.size() > static_cast<size_t>(proto::kMaxBodySize)) {
			core::logf("[WARN] login host 과대(%zuB) — no_server 로 응답\n",
				assigned->host.size());
			assigned.reset();
		}

		// 응답은 Reserve 결과를 기다리지 않고 즉시 낸다 — 마을이 Unsupported 로
		// 답하는 경우(구버전 등)까지 포함해 기다리면 그 로그인이 타임아웃까지
		// 걸린다. 응답 대기 설계는 ReserveAck 필수화(3단계) 몫이다.
		uint8_t result = proto::kSessionLoginNoServer;
		uint16_t port = 0;
		std::string host;
		if (assigned.has_value()) {
			result = proto::kSessionLoginOk;
			port = assigned->port;
			host = assigned->host;
		}
		// assigned 검사 뒤·회신 앞이다 — busy 갈래는 assign() 을 안 불러 assigned
		// 가 항상 비어 있으므로 대입 순서 자체는 결과에 안 걸리지만, 의도를
		// 코드 순서로 못박는다.
		if (busy) {
			result = proto::kSessionLoginBusy;
		}
		std::vector<char> ack(3);
		ack[0] = static_cast<char>(result);
		proto::write_u16_be(reinterpret_cast<uint8_t*>(ack.data() + 1), port);
		proto::s2s::encode_str16(ack, host);
		return send_client_msg(*client_, s, proto::MsgId::kSessionLoginAck,
			ack.data(), static_cast<int>(ack.size()));
	}

	bool Router::on_s2s_frame(net::Session& s, const char* frame, int len) {
		// sizer 가 완성 프레임만 올린다(8B 미만은 걸러졌다) — 여기서는 헤더 값을
		// 검증한다. decode 자체는 실패를 반환하지 않는다.
		const proto::s2s::Header head =
			proto::s2s::decode_header(reinterpret_cast<const uint8_t*>(frame));
		const char* body = frame + proto::s2s::kHeaderSize;
		const int body_len = len - static_cast<int>(proto::s2s::kHeaderSize);
		const auto id = static_cast<proto::s2s::MsgId>(head.msg_id);
		const uint64_t now = GetTickCount64();

		if (proto::s2s::is_request_from_village(id)) {
			switch (id) {
			case proto::s2s::MsgId::Register:
				return handle_register(s, head.seq, body, body_len);

			case proto::s2s::MsgId::Heartbeat: {
				// 고정 길이는 != 정확 검사 — 길이가 다르면 다른 규약이다(§8-4).
				if (body_len != 4) {
					return bad_body(s, "s2s heartbeat", body_len, 4);
				}
				proto::s2s::Heartbeat hb{};
				proto::s2s::decode_heartbeat(body, body_len, hb);
				bool known = false;
				bool grace = false;
				{
					std::lock_guard<std::mutex> lock(mutex_);
					known = registry_.heartbeat(s.id, hb.current, now);
					if (!known) {
						// 같은 임계구역·같은 now 안에서 이어 묻는다 —
						// registry.h 의 in_unregister_grace() 계약 참조.
						grace = registry_.in_unregister_grace(s.id, now);
					}
				}
				if (!known && !grace) {
					// Ack 를 주면 마을은 registered_ 인 채 영영 재등록을 안 한다 —
					// Register 재발신은 재연결 경로(on_connected)에만 있다. 끊어야
					// 커넥터가 재연결 → Register 로 자가 복구한다(§5-2 — 버전·정원
					// 거부가 스스로 끊고 재시도하는 것과 같은 논리다).
					core::logf("[WARN] s2s #%llu heartbeat 미등록 연결 — 끊는다\n",
						static_cast<unsigned long long>(s.id));
					return false;
				}
				if (!known && grace) {
					// grace 갈래는 Unregister 직후 drain 을 위해 연결을 유지하는
					// 마을의 하트비트다 — 항목이 없어도 Ack 하고 끊지 않는다.
					// Step4 의 unregister_and_wait() 가 이 갈래를 실제로 만든다 —
					// Unregister 로 항목을 지운 뒤에도 마을의 S2S 커넥터는
					// server.stop() 의 드레인이 끝날 때까지(s2s_link.stop() 전)
					// 계속 살아 있어 하트비트를 더 보낼 수 있다. 로그가 없으면
					// 이 갈래가 실제로 도는지 절단 여부로만 간접 추정해야 한다.
					core::logf("[INFO] s2s #%llu heartbeat 유예 중(unregister) — 안 끊는다\n",
						static_cast<unsigned long long>(s.id));
				}
				return send_s2s_msg(*s2s_, s, proto::s2s::MsgId::HeartbeatAck,
					head.seq, nullptr, 0);
			}

			case proto::s2s::MsgId::Unregister: {
				if (body_len != 0) {
					return bad_body(s, "s2s unregister", body_len, 0);
				}
				net::Session* link = nullptr;
				{
					std::lock_guard<std::mutex> lock(mutex_);
					link = registry_.unregister(s.id, now);
				}
				if (link != nullptr) {
					s2s_->release_session(link);
					holds_released_unregister_.fetch_add(1, std::memory_order_relaxed);
				}
				// 항목이 없어도 Ack — 「없음도 성공」이 멱등의 정의다(§8-2).
				core::logf("[INFO] s2s #%llu unregister%s\n",
					static_cast<unsigned long long>(s.id),
					link != nullptr ? "" : " (항목 없음 — 멱등 성공)");
				return send_s2s_msg(*s2s_, s, proto::s2s::MsgId::UnregisterAck,
					head.seq, nullptr, 0);
			}

			case proto::s2s::MsgId::PlayerEnter: {
				proto::s2s::PlayerEnter msg{};
				if (!proto::s2s::decode_player_enter(body, body_len, msg)) {
					return bad_body(s, "s2s player-enter", body_len, 8);
				}
				// 알림이다 — 대응 Ack 가 proto::s2s::MsgId 에 없다(Unregister 만
				// 있다). 응답을 안 보낸다.
				std::lock_guard<std::mutex> lock(mutex_);
				const uint32_t server_id = registry_.server_id_of(s.id);
				if (server_id != 0) {
					registry_.player_entered(msg.player_id, server_id);
				} else {
					// Heartbeat 와 달리 끊지 않는다 — 알림에는 재연결 -> Register
					// 자가 복구 논리가 없어 끊을 근거가 없다. 다만 미등록 연결이
					// 상태를 바꾸려 든 것은 규약 위반이라 로그는 남긴다.
					core::logf("[WARN] s2s #%llu player-enter 미등록 연결 — 무시(끊지 않는다)\n",
						static_cast<unsigned long long>(s.id));
				}
				return true;
			}

			case proto::s2s::MsgId::PlayerLeave: {
				proto::s2s::PlayerLeave msg{};
				if (!proto::s2s::decode_player_leave(body, body_len, msg)) {
					return bad_body(s, "s2s player-leave", body_len, 8);
				}
				std::lock_guard<std::mutex> lock(mutex_);
				const uint32_t server_id = registry_.server_id_of(s.id);
				if (server_id != 0) {
					registry_.player_left(msg.player_id, server_id);
				} else {
					core::logf("[WARN] s2s #%llu player-leave 미등록 연결 — 무시(끊지 않는다)\n",
						static_cast<unsigned long long>(s.id));
				}
				return true;
			}

			case proto::s2s::MsgId::FullSync: {
				proto::s2s::FullSync msg{};
				// decode 가 count 와 실제 크기의 일치까지 본다 — 6 은 고정
				// 헤더부다(chunk_idx·chunk_total·count 각 2B). 그 이상의 기대
				// 크기는 count 에 달려 있어 bad_body 의 단일 want 로는 못 담는다.
				if (!proto::s2s::decode_full_sync(body, body_len, msg)) {
					return bad_body(s, "s2s full-sync", body_len, 6);
				}
				std::lock_guard<std::mutex> lock(mutex_);
				const uint32_t server_id = registry_.server_id_of(s.id);
				if (server_id != 0) {
					const std::vector<uint64_t> conflicts =
						registry_.full_sync_replace(server_id, msg.chunk_idx == 0, msg.player_ids);
					for (uint64_t player_id : conflicts) {
						// 소유가 갈린 player — 어느 쪽이 진짜인지는 발신자(재연결한
						// 마을)에게 Kick 을 물어 판정한다. 실재하면 KickAck(Ok)로
						// 그쪽이 끊기고, NotFound 면 회수의 소유자 대조가 불일치로
						// 아무것도 안 지운다 — 충돌 항목은 정의상 타서버 소유라
						// 그 무동작이 의도다(현 소유자의 정상 항목을 보호한다).
						LinkPending& lp = pending_[s.id];
						if (has_pending_kick(lp, player_id)) {
							continue;
						}
						const uint32_t seq = lp.next_seq++;
						const proto::s2s::Kick kmsg{ player_id, proto::s2s::kKickReasonDuplicate };
						const std::vector<char> kbody = proto::s2s::encode_kick(kmsg);
						// 뮤텍스 안 send 는 세션 서버가 송신 큐 overflow 자기-절단을
						// 켜지 않는다는 전제 위에 있다 — 절단의 정리 경로가 이 뮤텍스로
						// 재진입하는 갈래를 연다.
						if (!send_s2s_msg(*s2s_, s, proto::s2s::MsgId::Kick, seq,
								kbody.data(), static_cast<int>(kbody.size()))) {
							core::logf("[WARN] kick send 실패 server_id=%u seq=%u\n",
								server_id, seq);
						}
						// server_id 는 발신 링크(이 마을)의 것이어야 한다 — connections_
						// 에 남은 소유자(타서버) id 를 넣으면 KickAck NotFound 정리의
						// 소유자 대조가 통과해 타서버의 정상 항목을 지운다.
						lp.kick_by_seq[seq] = PendingKick{ now, player_id, server_id };
						kick_sent_.fetch_add(1, std::memory_order_relaxed);
						core::logf("[INFO] s2s full-sync 대조 kick player=%llu server_id=%u seq=%u\n",
							static_cast<unsigned long long>(player_id), server_id, seq);
					}
					// 마을은 등록이 확인될 때마다(재등록 포함) 자동으로 이걸 보낸다
					// (마을 s2s_link.cpp::on_register_ack) — 응답이 없는 알림이라
					// 이 로그가 그 사실을 확인할 유일한 수단이다. kicked= 는 충돌로
					// 덮어쓰지 않은 원소 수다 — 재발신 억제와 무관하게 세므로
					// 발신 시도 수(kick_sent_)보다 클 수 있다.
					core::logf("[INFO] s2s #%llu full-sync server_id=%u chunk=%u/%u count=%zu kicked=%zu\n",
						static_cast<unsigned long long>(s.id), server_id,
						static_cast<unsigned>(msg.chunk_idx) + 1,
						static_cast<unsigned>(msg.chunk_total), msg.player_ids.size(),
						conflicts.size());
				} else {
					core::logf("[WARN] s2s #%llu full-sync 미등록 연결 — 무시(끊지 않는다)\n",
						static_cast<unsigned long long>(s.id));
				}
				return true;
			}

			case proto::s2s::MsgId::DrainComplete: {
				proto::s2s::DrainComplete msg{};
				if (!proto::s2s::decode_drain_complete(body, body_len, msg)) {
					return bad_body(s, "s2s drain-complete", body_len, 4);
				}
				// 알림이다(seq=0) — 운영자 판단 자료로 로그만 남긴다. 응답 없음.
				std::lock_guard<std::mutex> lock(mutex_);
				const uint32_t server_id = registry_.server_id_of(s.id);
				if (server_id != 0) {
					core::logf("[INFO] s2s #%llu drain-complete server_id=%u remaining=%u\n",
						static_cast<unsigned long long>(s.id), server_id, msg.remaining);
				} else {
					core::logf("[WARN] s2s #%llu drain-complete 미등록 연결 — 무시(끊지 않는다)\n",
						static_cast<unsigned long long>(s.id));
				}
				return true;
			}

			default: {
				// is_request_from_village 가 참인 값은 위 case 들이 전부 덮는다 —
				// 여기 오면 그 대응이 어긋난 것이다(방어적으로 남겨 둔다). 요청과
				// 알림의 최종 구분은 seq 다(알림은 seq=0·무응답 — §8-2 단방향 규칙).
				if (head.seq != 0) {
					// 안 보내면 상대가 타임아웃까지 기다린다(§8-2).
					core::logf("[INFO] s2s #%llu 미구현 요청 msg_id=0x%04x seq=%u — Unsupported 회신\n",
						static_cast<unsigned long long>(s.id),
						static_cast<unsigned>(head.msg_id), head.seq);
					return send_s2s_msg(*s2s_, s, proto::s2s::MsgId::Unsupported,
						head.seq, nullptr, 0);
				}
				core::logf("[INFO] s2s #%llu 알림 msg_id=0x%04x — 무시\n",
					static_cast<unsigned long long>(s.id),
					static_cast<unsigned>(head.msg_id));
				return true;
			}
			}
		}

		if (proto::s2s::is_reply_to_session(id)) {
			// KickAck·SetModeAck 는 둘 다 자체 완결 분기다 — 아래 ReserveAck/
			// Unsupported 전용 pending_ 매칭 블록(PendingReserve 전제)으로
			// 흘러들면 안 되므로, SetModeAck 선례처럼 여기서 매칭까지 끝내고
			// return 한다.
			if (id == proto::s2s::MsgId::KickAck) {
				if (body_len != 1) {
					return bad_body(s, "s2s kick-ack", body_len, 1);
				}
				proto::s2s::KickAck ack{};
				proto::s2s::decode_kick_ack(body, body_len, ack);

				bool matched = false;
				PendingKick pk{};
				{
					std::lock_guard<std::mutex> lock(mutex_);
					const auto lit = pending_.find(s.id);
					if (lit != pending_.end()) {
						const auto sit = lit->second.kick_by_seq.find(head.seq);
						if (sit != lit->second.kick_by_seq.end()) {
							pk = sit->second;
							lit->second.kick_by_seq.erase(sit);
							matched = true;
						}
					}
					if (matched && ack.result == proto::s2s::kKickResultNotFound) {
						// 유령 자가 회복 — player_left 의 소유자 대조(server_id
						// 불일치면 안 지운다)가 내장돼 있어, 이 KickAck 가 오는
						// 사이 다른 서버로 이미 옮겨 간 정상 항목은 안 지운다.
						// S2S 는 단일 소켓 + FIFO 명령 큐라, 이 KickAck 를 받은
						// 시점엔 그보다 먼저 제출된 PlayerEnter/Leave 가 전부
						// 도착해 있다 — NotFound 인데 항목이 남아 있으면 그것은
						// 유령이 맞다(ADR-024 결정 4).
						registry_.player_left(pk.player_id, pk.server_id);
					}
				}
				if (!matched) {
					// 타임아웃으로 이미 회수됐거나 애초에 안 보낸 seq — 순차
					// 패치 호환과 같은 이유로 끊지 않는다(§8-4).
					core::logf("[WARN] s2s #%llu kick-ack seq=%u 대기 없음 — 무시\n",
						static_cast<unsigned long long>(s.id), head.seq);
					return true;
				}
				if (ack.result == proto::s2s::kKickResultKicked) {
					kick_acked_.fetch_add(1, std::memory_order_relaxed);
				} else {
					kick_not_found_.fetch_add(1, std::memory_order_relaxed);
				}
				return true;
			}
			if (id == proto::s2s::MsgId::SetModeAck) {
				proto::s2s::SetModeAck ack{};
				if (!proto::s2s::decode_set_mode_ack(body, body_len, ack)) {
					return bad_body(s, "s2s setmode-ack", body_len, 4);
				}
				std::lock_guard<std::mutex> lock(mutex_);
				const uint32_t server_id = registry_.server_id_of(s.id);
				core::logf("[INFO] setmode ack server=%u current=%u\n",
					server_id, ack.current);
				return true;
			}
			if (id == proto::s2s::MsgId::ReserveAck && body_len != 1) {
				return bad_body(s, "s2s reserve-ack", body_len, 1);
			}
			if (id == proto::s2s::MsgId::Unsupported && body_len != 0) {
				return bad_body(s, "s2s unsupported", body_len, 0);
			}
			proto::s2s::ReserveAck ack{};
			if (id == proto::s2s::MsgId::ReserveAck) {
				proto::s2s::decode_reserve_ack(body, body_len, ack);
			}

			bool matched = false;
			bool revoked = false;
			PendingReserve pr{};
			{
				std::lock_guard<std::mutex> lock(mutex_);
				const auto lit = pending_.find(s.id);
				if (lit != pending_.end()) {
					const auto sit = lit->second.by_seq.find(head.seq);
					if (sit != lit->second.by_seq.end()) {
						pr = sit->second;
						lit->second.by_seq.erase(sit);
						matched = true;
					}
				}
				// 거절이면 예약도 같은 임계구역에서 회수한다 — expire 까지 남겨 두면
				// 그 서버의 배정 부하에 유령이 잡힌다. 대조 키가 발급 세대인 이유는
				// Registry 쪽 주석 참조(같은 서버 재로그인의 산 예약 보호).
				if (matched && id == proto::s2s::MsgId::ReserveAck && ack.result != 0) {
					revoked = registry_.revoke_reservation(pr.player_id, pr.issue_id);
					if (ack.result == proto::s2s::kResultDraining) {
						// 운영 명령(SetMode) 없이도 세션 서버가 스스로 배운다 —
						// 재기동 등으로 그 명령을 놓쳤어도 이 학습 경로가 자가
						// 회복시킨다. pr.server_id 는 위에서 이미 이 잠금 안에서
						// 확보됐다 — 락 밖(아래 거절 로그 갈래)에서 하면 세션
						// 서버 판 불변식(테이블은 Registry 뮤텍스 아래에서만)
						// 위반이다.
						registry_.set_draining(pr.server_id, true);
					}
				}
			}
			if (!matched) {
				// 타임아웃으로 이미 회수됐거나 애초에 안 보낸 seq — 순차 패치
				// 호환과 같은 이유로 끊지 않는다(§8-4).
				core::logf("[WARN] s2s #%llu reply seq=%u 대기 없음 — 무시\n",
					static_cast<unsigned long long>(s.id), head.seq);
				return true;
			}

			if (id == proto::s2s::MsgId::ReserveAck) {
				if (ack.result == 0) {
					reserve_ack_ok_.fetch_add(1, std::memory_order_relaxed);
				} else {
					reserve_ack_rejected_.fetch_add(1, std::memory_order_relaxed);
					core::logf("[WARN] s2s reserve 거절 result=%u player=%llu (예약 회수=%d)\n",
						static_cast<unsigned>(ack.result),
						static_cast<unsigned long long>(pr.player_id),
						revoked ? 1 : 0);
				}
			} else {
				// Unsupported 회신 — 상대 마을이 이 요청을 처리하지 못했다(구버전 등
				// 비호환). 예약은 세션 서버 쪽 테이블만으로 유효하고, 마을 강제는
				// 3단계 몫이다.
				reserve_unsupported_.fetch_add(1, std::memory_order_relaxed);
				core::logf("[WARN] s2s reserve unsupported player=%llu server_id=%u\n",
					static_cast<unsigned long long>(pr.player_id), pr.server_id);
			}
			return true;
		}

		// 미정의 msg_id — 순차 패치 호환을 위해 끊지 않는다(§8-4). 프레이밍은
		// sizer 가 이미 확정했으므로 이 프레임만 버려도 스트림은 멀쩡하다.
		core::logf("[WARN] s2s #%llu 미정의 msg_id=0x%04x — 무시\n",
			static_cast<unsigned long long>(s.id), static_cast<unsigned>(head.msg_id));
		return true;
	}

	bool Router::handle_register(net::Session& s, uint32_t seq, const char* body, int body_len) {
		proto::s2s::Register reg{};
		if (!proto::s2s::decode_register(body, body_len, reg)) {
			core::logf("[WARN] s2s #%llu register decode 실패 body=%d — closing\n",
				static_cast<unsigned long long>(s.id), body_len);
			return false;
		}

		// major 만 가른다(§8-5) — minor 는 호환 범위다. 거절해도 연결은 유지한다:
		// 붙긴 붙되 등록을 거절당하는 편이 소켓이 안 붙는 것보다 진단하기 쉽다는
		// 핸드셰이크 설계(s2s_packet.h 버전 절)의 수신측 반쪽이다.
		if ((reg.ver >> 8) != proto::s2s::kVerMajor) {
			{
				std::lock_guard<std::mutex> lock(mutex_);
				registry_.note_version_rejected();
			}
			core::logf("[WARN] s2s #%llu register ver=0x%04x major 불일치 (기대 %u) — 거절\n",
				static_cast<unsigned long long>(s.id),
				static_cast<unsigned>(reg.ver),
				static_cast<unsigned>(proto::s2s::kVerMajor));
			const proto::s2s::RegisterAck nack{ 0, proto::s2s::kResultVersionRejected };
			const std::vector<char> nb = proto::s2s::encode_register_ack(nack);
			return send_s2s_msg(*s2s_, s, proto::s2s::MsgId::RegisterAck, seq,
				nb.data(), static_cast<int>(nb.size()));
		}

		const uint64_t now = GetTickCount64();
		RegisterResult r{};
		net::Session* stale = nullptr;
		bool attach_failed = false;
		{
			// 한 임계구역에서 register(탈착 포함) → acquire(새 연결) → attach 를
			// 끝낸다 — acquire 는 카운터 증가뿐이라 뮤텍스 안 비용이 무시 가능하고,
			// 「link 가 비어 있는 부활 항목」 창 자체가 안 생긴다. 거부 갈래에서는
			// acquire 를 하지 않는다 — 항목이 없어 저장처가 없고, 하면 홀드 미아다.
			std::lock_guard<std::mutex> lock(mutex_);
			r = registry_.register_server(s.id, reg.host, reg.port,
				reg.capacity, reg.current, now);
			if (r.need_link) {
				s2s_->acquire_session(s);
				if (registry_.attach_link(s.id, &s)) {
					holds_acquired_.fetch_add(1, std::memory_order_relaxed);
				} else {
					attach_failed = true;
				}
			}
			stale = r.stale_link;
		}
		if (stale != nullptr) {
			// stale 과 새 link 는 다른 세션이라 release 가 attach 뒤여도 정확성과
			// 무관하다 — 행이 걸린 채 재기동한 마을의 옛 연결 홀드 회수 경로다.
			// attach 실패의 조기 반환보다 먼저 회수해야 한다 — stale 은 부활
			// 시점에 이미 탈착된 남의 홀드라, 여기서 놓치면 detach_all 감사에도
			// 수지 카운터에도 안 잡힌 채 그 Session 이 영구히 안 지워진다.
			s2s_->release_session(stale);
			holds_released_stale_revive_.fetch_add(1, std::memory_order_relaxed);
			core::logf("[INFO] s2s server_id=%u 부활 — 잔존 홀드 release\n", r.server_id);
		}
		if (attach_failed) {
			// need_link=true 인데 붙일 항목이 없거나 link 가 이미 차 있다 — 같은
			// 임계구역이라 정상 경로에서는 올 수 없는 계약 위반의 증거다. 방금 올린
			// 홀드를 되돌려 미아를 막고(쌍이 즉시 상쇄되므로 수지 카운터에는 안
			// 태운다), Ack 없이 끊는다 — ok 로 답하면 link 없는 항목이 남아 자가
			// 복구가 없고, 끊으면 커넥터가 재연결 → Register 로 처음부터 다시 간다.
			core::logf("[ERROR] s2s #%llu attach 실패 server_id=%u — 홀드 반납·절단\n",
				static_cast<unsigned long long>(s.id), r.server_id);
			s2s_->release_session(&s);
			return false;
		}

		if (r.result == proto::s2s::kResultOk) {
			core::logf("[INFO] s2s #%llu register -> server_id=%u %s:%u cap=%u cur=%u need_link=%d\n",
				static_cast<unsigned long long>(s.id), r.server_id,
				reg.host.c_str(), static_cast<unsigned>(reg.port),
				reg.capacity, reg.current, r.need_link ? 1 : 0);
		} else {
			core::logf("[WARN] s2s #%llu register 거절 result=%u (정원 %s)\n",
				static_cast<unsigned long long>(s.id),
				static_cast<unsigned>(r.result),
				r.result == proto::s2s::kResultFull ? "초과" : "외 사유");
		}

		// seq==0 인 Register 에도 seq=0 을 그대로 에코한다 — 「알림은 seq=0·무응답」
		// 규약과 경계가 겹치지만, 마을 커넥터는 항상 seq>=1 이라 실해가 없다.
		// 이 경계는 3단계 규약 확정 때 정리한다.
		const proto::s2s::RegisterAck ack{ r.server_id, r.result };
		const std::vector<char> ab = proto::s2s::encode_register_ack(ack);
		return send_s2s_msg(*s2s_, s, proto::s2s::MsgId::RegisterAck, seq,
			ab.data(), static_cast<int>(ab.size()));
	}

	bool Router::on_s2s_gone(net::Session& s) {
		const uint64_t now = GetTickCount64();
		net::Session* link = nullptr;
		bool known = false;
		std::map<uint32_t, PendingReserve> failed;
		std::map<uint32_t, PendingKick> failed_kicks;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			known = registry_.contains(s.id);
			if (known) {
				link = registry_.on_link_down(s.id, now);
			}
			const auto it = pending_.find(s.id);
			if (it != pending_.end()) {
				failed.swap(it->second.by_seq);
				failed_kicks.swap(it->second.kick_by_seq);
				pending_.erase(it);
			}
		}
		if (link != nullptr) {
			s2s_->release_session(link);
			holds_released_gone_.fetch_add(1, std::memory_order_relaxed);
		}
		if (known) {
			// 항목은 유예까지 남는다(§5-3) — 이 전이는 로그가 아니면 관측할 수단이
			// 없다(다음 이벤트가 부활이든 삭제든 이 시각이 그 기산점이다).
			core::logf("[INFO] s2s #%llu 링크 다운 — orphan (항목 유지)\n",
				static_cast<unsigned long long>(s.id));
		}
		if (!known) {
			// 이미 삭제(unregister·유예 초과)됐거나 등록한 적 없는 연결의 뒤늦은
			// 통지 — no-op 이 정상이고, 그 사실을 로그로 남겨야 수지 감사가 된다.
			core::logf("[INFO] s2s #%llu 링크 다운 — 항목 없음 no-op\n",
				static_cast<unsigned long long>(s.id));
		}
		if (!failed.empty()) {
			reserve_fail_linkdown_.fetch_add(failed.size(), std::memory_order_relaxed);
			core::logf("[WARN] s2s #%llu 링크 다운 — pending %zu 건 실패 처리\n",
				static_cast<unsigned long long>(s.id), failed.size());
		}
		if (!failed_kicks.empty()) {
			kick_link_down_.fetch_add(failed_kicks.size(), std::memory_order_relaxed);
			core::logf("[WARN] s2s #%llu 링크 다운 — kick pending %zu 건 실패 처리\n",
				static_cast<unsigned long long>(s.id), failed_kicks.size());
		}
		// false = 임시 홀드를 받지 않는다 — 위 처리는 이 콜백 안에서 동기로 끝났고,
		// net 이 미리 올린 홀드는 net 이 되돌린다. 이 콜백은 I/O 워커·net 스윕
		// 스레드 양쪽에서 불리는데 Registry 뮤텍스가 덮는다.
		return false;
	}

	void Router::on_sweep(uint64_t now_ms) {
		SweepResult sr;
		uint32_t timeouts = 0;
		uint32_t kick_timeouts = 0;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			sr = registry_.sweep(now_ms);
			for (auto lit = pending_.begin(); lit != pending_.end();) {
				auto& by_seq = lit->second.by_seq;
				for (auto it = by_seq.begin(); it != by_seq.end();) {
					// 덧셈 비교 — 시각을 락 밖에서 뜨는 스레드들과의 역전에서
					// 뺄셈은 언더플로로 조기 타임아웃이 된다(Registry 헬스 판정
					// 주석과 같은 이유).
					if (it->second.sent_ms + settings_.request_timeout_ms < now_ms) {
						it = by_seq.erase(it);
						++timeouts;
					} else {
						++it;
					}
				}
				// Kick 은 대기가 없어졌으니(ADR-024 결정 2) 이 타임아웃의 목적도
				// 대기가 아니라 정리다 — 그래서 Reserve 와 같은 해상도
				// (settings_.request_timeout_ms)를 그대로 재사용한다.
				auto& kick_seq = lit->second.kick_by_seq;
				for (auto it = kick_seq.begin(); it != kick_seq.end();) {
					if (it->second.sent_ms + settings_.request_timeout_ms < now_ms) {
						it = kick_seq.erase(it);
						++kick_timeouts;
					} else {
						++it;
					}
				}
				// pending 이 비어도 항목은 지우지 않는다 — next_seq 카운터가 같이
				// 살아야 한다. 지우면 다음 발신이 1부터 다시 세는데, 그 seq 로 방금
				// 타임아웃된 옛 요청의 늦은 응답이 새 요청에 오매칭될 수 있다.
				// 항목 정리는 링크 다운(연결 소멸 = seq 공간 소멸)이 한다.
				++lit;
			}
		}
		for (net::Session* stale : sr.stale_links) {
			s2s_->release_session(stale);
			holds_released_stale_sweep_.fetch_add(1, std::memory_order_relaxed);
		}
		if (timeouts > 0) {
			reserve_timeout_.fetch_add(timeouts, std::memory_order_relaxed);
		}
		if (kick_timeouts > 0) {
			kick_timeout_.fetch_add(kick_timeouts, std::memory_order_relaxed);
		}
		if (sr.expired_reservations > 0 || sr.removed_entries > 0
			|| !sr.stale_links.empty() || timeouts > 0 || kick_timeouts > 0) {
			core::logf("[INFO] sweep: 예약 만료=%u 항목 삭제=%u stale 홀드=%zu pending 타임아웃=%u kick 타임아웃=%u\n",
				sr.expired_reservations, sr.removed_entries,
				sr.stale_links.size(), timeouts, kick_timeouts);
		}
	}

	void Router::shutdown() {
		size_t leftover = 0;
		size_t failed = 0;
		size_t kick_failed = 0;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			const std::vector<net::Session*> links = registry_.detach_all();
			leftover = links.size();
			for (const auto& lp : pending_) {
				failed += lp.second.by_seq.size();
				kick_failed += lp.second.kick_by_seq.size();
			}
			pending_.clear();
			// 잔존 link 를 release 하지 않는다 — 서버 stop 의 세션 전량 삭제가
			// io_count 무관이라 이 포인터들은 이미 죽었을 수 있다(사후 release 는
			// UAF). 정상 경로에서는 stop 중 session_gone 전수 발화가 먼저 다
			// 회수해 leftover 가 0 이다.
		}
		if (failed > 0) {
			reserve_fail_stop_.fetch_add(failed, std::memory_order_relaxed);
			core::logf("[WARN] stop — pending %zu 건 실패 처리\n", failed);
		}
		if (kick_failed > 0) {
			kick_stop_.fetch_add(kick_failed, std::memory_order_relaxed);
			core::logf("[WARN] stop — kick pending %zu 건 실패 처리\n", kick_failed);
		}
		if (leftover > 0) {
			// 정상 종료에서도 나올 수 있다 — 서버 stop 은 워커 수만큼만 가짜 완료를
			// 넣고 워커는 그걸 만나면 즉시 이탈하므로, closesocket 이 만든 실패
			// 완료가 미처리로 남으면 session_gone 이 안 불린 세션이 남는다. 그래서
			// 오류 단정이 아니라 관찰 기록이고, release 는 여전히 안 한다(stop 의
			// 세션 전량 삭제가 io_count 무관이라 사후 release 는 UAF).
			holds_stop_leftover_.fetch_add(leftover, std::memory_order_relaxed);
			core::logf("[WARN] stop — 잔존 링크 홀드 %zu 건 (release 는 하지 않는다 — 감사 기록)\n",
				leftover);
		}
	}

	void Router::log_summary() {
		// Registry 접근은 전부 뮤텍스 아래라는 계약이라 stats 도 락 안에서 값으로
		// 복사한다 — 이 시점엔 실경합이 없지만 계약에 예외를 만들지 않는다.
		Stats st;
		size_t reservations = 0;
		size_t connections = 0;
		size_t pending_left = 0;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			st = registry_.stats();
			reservations = registry_.reservation_count();
			connections = registry_.connection_count();
			for (const auto& lp : pending_) {
				pending_left += lp.second.by_seq.size();
			}
		}
		const uint64_t released = holds_released_unregister_.load()
			+ holds_released_gone_.load() + holds_released_stale_revive_.load()
			+ holds_released_stale_sweep_.load();
		core::logf("[SESS ] register=%llu reject_full=%llu reject_ver=%llu orphan=%llu removed=%llu\n",
			static_cast<unsigned long long>(st.registered),
			static_cast<unsigned long long>(st.rejected_full),
			static_cast<unsigned long long>(st.rejected_version),
			static_cast<unsigned long long>(st.orphan_transitions),
			static_cast<unsigned long long>(st.entries_removed));
		core::logf("[SESS ] assign ok=%llu no_server=%llu | reserve issued=%llu overwrite=%llu expired=%llu revoked=%llu remain=%zu\n",
			static_cast<unsigned long long>(st.assigned),
			static_cast<unsigned long long>(st.assign_no_server),
			static_cast<unsigned long long>(st.reservations_issued),
			static_cast<unsigned long long>(st.reservations_overwritten),
			static_cast<unsigned long long>(st.reservations_expired),
			static_cast<unsigned long long>(st.reservations_revoked),
			reservations);
		core::logf("[SESS ] connections added=%llu removed=%llu fullsync_replaced=%llu remain=%zu\n",
			static_cast<unsigned long long>(st.connections_added),
			static_cast<unsigned long long>(st.connections_removed),
			static_cast<unsigned long long>(st.fullsync_replaced),
			connections);
		core::logf("[SESS ] link hold acquire=%llu release=%llu (unreg=%llu gone=%llu stale_revive=%llu stale_sweep=%llu) stop_leftover=%llu\n",
			static_cast<unsigned long long>(holds_acquired_.load()),
			static_cast<unsigned long long>(released),
			static_cast<unsigned long long>(holds_released_unregister_.load()),
			static_cast<unsigned long long>(holds_released_gone_.load()),
			static_cast<unsigned long long>(holds_released_stale_revive_.load()),
			static_cast<unsigned long long>(holds_released_stale_sweep_.load()),
			static_cast<unsigned long long>(holds_stop_leftover_.load()));
		core::logf("[SESS ] pending sent=%llu send_fail=%llu ack=%llu rejected=%llu unsupported=%llu timeout=%llu link_down=%llu stop=%llu skip_nolink=%llu remain=%zu\n",
			static_cast<unsigned long long>(reserve_sent_.load()),
			static_cast<unsigned long long>(reserve_send_fail_.load()),
			static_cast<unsigned long long>(reserve_ack_ok_.load()),
			static_cast<unsigned long long>(reserve_ack_rejected_.load()),
			static_cast<unsigned long long>(reserve_unsupported_.load()),
			static_cast<unsigned long long>(reserve_timeout_.load()),
			static_cast<unsigned long long>(reserve_fail_linkdown_.load()),
			static_cast<unsigned long long>(reserve_fail_stop_.load()),
			static_cast<unsigned long long>(reserve_skip_nolink_.load()),
			pending_left);
		core::logf("[SESS ] kick sent=%llu acked=%llu not_found=%llu timeout=%llu link_down=%llu stop=%llu | login_busy=%llu\n",
			static_cast<unsigned long long>(kick_sent_.load()),
			static_cast<unsigned long long>(kick_acked_.load()),
			static_cast<unsigned long long>(kick_not_found_.load()),
			static_cast<unsigned long long>(kick_timeout_.load()),
			static_cast<unsigned long long>(kick_link_down_.load()),
			static_cast<unsigned long long>(kick_stop_.load()),
			static_cast<unsigned long long>(login_busy_.load()));
	}

}	// namespace session
