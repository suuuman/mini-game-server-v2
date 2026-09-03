//  session/session_router.h — 두 IocpServer(클라 수용 · S2S 수용)의 콜백 소유자
//
//  이 exe 에는 존 스레드가 없다 — 받은 Job 을 I/O 워커에서 그 자리에서 동기
//  호출한다(ADR-020 결정 3). 그래서 recv 핸들러는 여러 스레드에서 동시에 들어오고,
//  동시성은 router 가 소유한 단일 뮤텍스(Registry 뮤텍스)가 전담한다 — 세션 서버
//  테이블(Registry 의 항목·예약, 그리고 여기의 pending·seq)은 전부 그 아래에서만
//  만진다.
//
//  net 함수(acquire/release/send)의 수행 주체는 전부 이 클래스다 — Registry 는
//  보관·탈착만 한다. 락 경계: 조회·보관·탈착·send 는 뮤텍스 안(탈착·send 가 다른
//  스레드의 release 와 배타된다) · release 는 탈착으로 받은 포인터에 대해 뮤텍스
//  밖(임계구역 최소 — 탈착이 뮤텍스 안이므로 이중 해제가 구조적으로 불가능하다).

#pragma once

#include "session/registry.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>

namespace net {
	class IocpServer;
}

namespace session {

	struct RouterSettings {
		uint32_t reserve_expire_ms;
		uint32_t request_timeout_ms;
	};

	class Router {
	public:
		// 서버 참조를 생성자가 아니라 wire() 로 받는 이유 — 콜백 타깃인 이 객체가
		// 서버들보다 먼저 선언돼야(= 나중에 소멸해야) 서버 stop 중의 콜백이 산
		// 객체를 부른다. 생성자로 받으면 선언 순서가 뒤집힌다.
		Router(Registry& registry, const RouterSettings& settings);

		Router(const Router&) = delete;
		Router& operator=(const Router&) = delete;

		// 두 서버에 sizer·job_sink·recv·session_gone 을 꽂는다. start() 전에 부른다.
		void wire(net::IocpServer& client_server, net::IocpServer& s2s_server);

		// 운영 진입점 — main.cpp 의 stdin 콘솔이 부른다. server_id 가 없으면
		// false + 로그(호출부가 그대로 대조한다). 있으면 registry 갱신은 항상
		// 되고, 링크가 없거나 발신이 실패해도 true 다 — 상태 반영 자체는
		// 성공했고 통지는 최선의 노력이기 때문이다(아래 .cpp 의 fire-and-log
		// 주석 참조).
		bool request_set_mode(uint32_t server_id, bool draining);

		// 스윕 스레드가 주기마다 부른다 — 만료 예약·유예 초과 orphan 회수 +
		// pending 타임아웃 판정(lazy — 다음 틱에서 잡는다).
		void on_sweep(uint64_t now_ms);

		// 두 서버 stop 뒤에 부른다. 잔존 link 는 release 하지 않는다 — 서버 stop 의
		// 세션 전량 삭제가 io_count 무관이라 사후 release 는 UAF 다. stop 중의
		// session_gone 발화는 전수 보장이 아니다 — 워커는 stop 의 가짜 완료를
		// 만나면 즉시 이탈하므로 closesocket 의 실패 완료가 미처리로 남을 수 있다
		// (iocp_server.cpp 워커 루프). 그래서 잔존은 오류 단정이 아니라 감사
		// 기록이고, 로그와 카운터로만 남긴다. 잔존 pending 도 여기서 전량 실패
		// 처리한다.
		void shutdown();

		// [SESS ] 종료 요약 — Registry 카운터 + 홀드 수지(acquire/release 주체인
		// 이 클래스가 자체 집계) 합산.
		void log_summary();

	private:
		struct PendingReserve {
			uint64_t sent_ms;
			uint64_t player_id;
			uint32_t server_id;
			uint64_t issue_id;     // 이 요청이 실은 예약의 세대 — 거절 회수의 대조 키
		};

		// Kick 은 예약 세대가 없다 — PendingReserve 의 issue_id 자리가 필요 없어
		// 별도 구조체다(PendingReserve 재사용은 그 필드가 강제하는 의미가 안 맞아
		// 기각 — ADR-023 결정 5 의 이유와 같다).
		struct PendingKick {
			uint64_t sent_ms;
			uint64_t player_id;
			uint32_t server_id;
		};

		// 연결마다 seq 1부터 단조 증가(§8-6 — 마을과 독립 공간). 클라 세션 정보는
		// 담지 않는다 — 클라 쪽 세션 수명 경로를 만들지 않는다(응답 대기 설계는
		// 3단계 ReserveAck 필수화 몫).
		struct LinkPending {
			uint32_t next_seq = 1;
			std::map<uint32_t, PendingReserve> by_seq;
			// seq 공간은 위 next_seq 를 Reserve 와 함께 쓴다 — 응답 매칭이
			// msg_id(ReserveAck vs KickAck)로 먼저 갈리므로 같은 seq 값이
			// 서로 다른 요청에서 나와도 충돌하지 않는다.
			std::map<uint32_t, PendingKick> kick_by_seq;
		};

		bool on_client_frame(net::Session& s, const char* frame, int len);
		bool on_s2s_frame(net::Session& s, const char* frame, int len);
		bool on_s2s_gone(net::Session& s);

		bool handle_session_login(net::Session& s, const char* body, int body_len);
		bool handle_register(net::Session& s, uint32_t seq, const char* body, int body_len);

		// 같은 player 의 Kick pending 이 이미 있는가 — 재발신 억제 판정. login 경로와
		// full-sync 대조 경로가 같은 스캔을 써야 억제 기준이 두 곳에서 따로 갈리지
		// 않는다. pending_ 아래 자료라 뮤텍스 안에서만 부른다.
		static bool has_pending_kick(const LinkPending& lp, uint64_t player_id);

		Registry& registry_;
		RouterSettings settings_;

		// wire() 전에는 null. 콜백은 wire() 가 꽂으므로 null 인 채 불릴 일이 없다.
		net::IocpServer* client_ = nullptr;
		net::IocpServer* s2s_ = nullptr;

		// Registry 뮤텍스 — 신설 불변식의 그 뮤텍스다. pending_ 도 이 아래다:
		// 삽입(클라 프레임 워커)·매칭(S2S 프레임 워커)·타임아웃(스윕 스레드)·링크
		// 다운(session_gone 콜백) 최소 3계열 스레드가 만진다.
		std::mutex mutex_;
		std::map<uint64_t, LinkPending> pending_;   // key: S2S 수용 연결의 net 세션 id

		// 홀드 수지: acquired == released 합 + stop 잔존 0 이 정상이다. 뮤텍스 밖
		// (release 직후)에서도 올리므로 atomic 으로 둔다.
		std::atomic<uint64_t> holds_acquired_{ 0 };
		std::atomic<uint64_t> holds_released_unregister_{ 0 };
		std::atomic<uint64_t> holds_released_gone_{ 0 };
		// stale 회수 두 원인(부활 · sweep 삭제)을 나눠 센다 — 종료 요약만 보고
		// 어느 경로가 홀드를 거둬 갔는지 가릴 수 있어야 로그가 판별력을 가진다.
		std::atomic<uint64_t> holds_released_stale_revive_{ 0 };
		std::atomic<uint64_t> holds_released_stale_sweep_{ 0 };
		std::atomic<uint64_t> holds_stop_leftover_{ 0 };

		// sent 는 「pending 등재」 기준이다 — 송신 큐 거절(send_frame false)도
		// pending 은 등재되고 타임아웃이 회수하므로 항등식의 좌변에 남는다.
		// 실패 자체는 send_fail 로 따로 센다 — 이름이 「보냈다」만 말하면 큐
		// 거절이 회계에 묻힌다.
		std::atomic<uint64_t> reserve_sent_{ 0 };
		std::atomic<uint64_t> reserve_send_fail_{ 0 };
		std::atomic<uint64_t> reserve_skip_nolink_{ 0 };
		std::atomic<uint64_t> reserve_ack_ok_{ 0 };
		std::atomic<uint64_t> reserve_ack_rejected_{ 0 };
		std::atomic<uint64_t> reserve_unsupported_{ 0 };
		std::atomic<uint64_t> reserve_timeout_{ 0 };
		std::atomic<uint64_t> reserve_fail_linkdown_{ 0 };
		std::atomic<uint64_t> reserve_fail_stop_{ 0 };

		// kick_sent 는 발신 1회당 정확히 1 이다 — 억제된 재발신(같은 player 의
		// pending 이 이미 있을 때)은 안 센다. 나머지 다섯은 그 pending 이 끝나는
		// 다섯 경로고, sent == acked+not_found+timeout+link_down+stop 이 짝
		// 계약이다([SESS ] kick 요약).
		std::atomic<uint64_t> kick_sent_{ 0 };
		std::atomic<uint64_t> kick_acked_{ 0 };
		std::atomic<uint64_t> kick_not_found_{ 0 };
		std::atomic<uint64_t> kick_timeout_{ 0 };
		std::atomic<uint64_t> kick_link_down_{ 0 };
		std::atomic<uint64_t> kick_stop_{ 0 };
		// Busy 회신 수 — 대상 링크가 있어 Kick 을 발신(또는 재발신 억제)한
		// 갈래에서만 오른다. orphan(링크 없음)은 Kick 을 보낼 수 없어 다른
		// 서버 배정을 그대로 허용하므로(§5-3) 여기 안 낀다. kick_sent_ 와
		// 다르다 — 억제된 재발신도 Busy 는 낸다.
		std::atomic<uint64_t> login_busy_{ 0 };
	};

}	// namespace session
