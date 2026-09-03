//  session/registry.h — 세션 서버가 아는 마을 서버 테이블과 로그인 예약
//
//  순수 자료구조다 — net 함수를 부르지 않는다. net::Session* 는 불투명 포인터로
//  「보관·탈착」만 하고, acquire/release/send 는 전부 router 몫이다. 그래서 net 의
//  헤더를 include 하지 않는다 — 전방 선언이면 충분하고, 계층 접촉면이 좁을수록
//  이 파일이 net 을 만질 수 없다는 사실이 컴파일로 강제된다.
//
//  락이 없고 스레드 안전하지 않다. 세션 서버 테이블은 Registry 뮤텍스 아래에서만
//  만진다. 뮤텍스는 router 가 소유한다 — 마을의 Zone 락 0 모델(절대 규칙 6)은
//  마을 존 스레드 계약이고, 이 exe 는 I/O 워커 인라인 처리라 다중 스레드 진입이
//  구조적이다(ADR-020 결정 3).
//
//  모든 메서드가 now_ms 를 인자로 받는 이유 — 시계를 밖에서 주입해야 하네스가
//  헬스 주기·유예를 축소한 설정값으로 판정 경로를 실시간 대기 없이 검증할 수 있고,
//  한 임계구역 안의 연쇄 호출이 같은 「지금」을 보게 된다.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace net {
	struct Session;
}

namespace session {

	struct ServerEntry {
		uint32_t server_id;         // 단조 증가 채번. 0 은 무효 — 거부 응답이 server_id=0 을 쓴다
		uint64_t net_session_id;    // S2S 수용 연결의 net 세션 id — 링크 다운을 항목에 되찾아 주는 매칭 키
		net::Session* link;         // 영구 홀드의 보관처. acquire/release 는 router 가 한다 — 여기는 보관·탈착만
		std::string host;
		uint16_t port;
		uint32_t capacity;
		uint32_t current;           // 마을이 보고하는 현재 입장 인원 — assign() 의 부하 계산에 쓴다
		uint64_t last_heartbeat_ms;
		uint64_t orphaned_at_ms;    // 0 = 정상. 링크를 잃었거나 헬스 판정을 초과한 시각
		bool draining = false;      // SetMode(운영 명령) 또는 ReserveAck(kResultDraining) 학습으로 켜진다
	};

	// result 는 proto::s2s::kResult* 값이다 — 와이어에 그대로 나가는 규약 값이라
	// 여기서 다른 코드 공간을 만들면 router 가 번역을 한 겹 더 해야 한다.
	//
	// need_link 는 「router 가 이 연결을 acquire 해 항목에 붙여야 하는가」다 —
	// 거부(result != 0)면 붙일 항목 자체가 없으므로 항상 false 다. 이 계약이
	// 깨지면 acquire 만 되고 보관처가 없는 홀드 미아가 생긴다.
	//
	// stale_link 는 부활한 항목에 남아 있던 옛 연결의 홀드다(행이 걸린 채 재기동한
	// 경우) — 탈착만 해서 돌려주므로 release 는 router 가 뮤텍스 밖에서 한다.
	struct RegisterResult {
		uint32_t server_id;
		uint8_t result;
		bool need_link;
		net::Session* stale_link;
	};

	struct Assignment {
		uint32_t server_id;
		std::string host;
		uint16_t port;
		uint64_t issue_id;     // 이 배정이 발급한 예약의 세대 — 거절 회수의 대조 키
	};

	// stale_links 는 삭제된 항목에 남아 있던 홀드들 — release 는 router 가 뮤텍스
	// 밖에서 한다. 숫자 둘은 이번 호출분이다(누계는 stats 가 든다) — 스윕 로그가
	// 「이번에 몇 개」를 말할 수 있어야 조용한 주기 동작을 관측할 수 있다.
	struct SweepResult {
		uint32_t expired_reservations;
		uint32_t removed_entries;
		std::vector<net::Session*> stale_links;
	};

	// [SESS ] 종료 요약의 Registry 몫 카운터. 홀드 수지(acquire/release 횟수)는
	// 수행 주체인 router 가 자체 집계해 합산한다 — 여기 없다.
	// 예약의 정합 — 발급 = 덮어쓰기 + 만료 + 회수 + 종료 시점 잔여. 예약 하나는
	// 넷 중 정확히 한 경로로 끝난다(덮어쓰기는 옛 예약의 만료 여부와 무관하게 센다).
	struct Stats {
		uint64_t registered = 0;               // 성공한 Register — 신규·부활·재등록 갱신 포함
		uint64_t rejected_full = 0;
		uint64_t rejected_version = 0;         // 판정은 router 몫(§8-5) — 종료 요약 카운터를 한곳에 모을 뿐
		uint64_t orphan_transitions = 0;
		uint64_t entries_removed = 0;          // unregister 즉시 삭제 + sweep 유예 초과 삭제
		uint64_t assigned = 0;
		uint64_t assign_no_server = 0;
		uint64_t reservations_issued = 0;
		uint64_t reservations_overwritten = 0;
		uint64_t reservations_expired = 0;
		uint64_t reservations_revoked = 0;     // ReserveAck 거절이 회수한 것

		// 접속 테이블의 정합 — 추가 = 제거 + 종료 시점 잔여. 삽입(player_entered ·
		// full_sync_replace 의 개별 원소 — 충돌(타서버 소유)로 덮어쓰지 않은 원소는
		// 세지 않는다)과 제거(player_left · full_sync_replace 의 전량 교체 ·
		// drop_server_connections)가 각각 정확히 그 자리에서 센다.
		uint64_t connections_added = 0;
		uint64_t connections_removed = 0;
		uint64_t fullsync_replaced = 0;        // 전량 교체가 실제로 일어난 횟수(첫 청크만)
	};

	class Registry {
	public:
		// health_period_ms × health_fail_count 를 넘겨 하트비트가 끊긴 항목을
		// 배정에서 거르는 기준으로 쓴다. 곱을 미리 하지 않고 둘을 따로 받는 이유 —
		// 설정 파일(§4)이 둘을 따로 말하므로 여기서 합치면 로그·설정 대조가 어긋난다.
		Registry(uint32_t registry_capacity, uint32_t health_period_ms,
			uint32_t health_fail_count, uint64_t orphan_grace_ms, uint64_t unregister_grace_ms);

		RegisterResult register_server(uint64_t net_session_id, const std::string& host,
			uint16_t port, uint32_t capacity, uint32_t current, uint64_t now_ms);

		// 항목 갱신. orphan 이면 복원한다 — lazy orphan 뒤 같은 연결이 회복(행
		// 해제)했다는 뜻이라, 살아 있는 서버가 유예 삭제되면 안 된다(§5-3 복구
		// 정신). 미등록 연결이면 false — 로그는 호출부가 남긴다.
		bool heartbeat(uint64_t net_session_id, uint32_t current, uint64_t now_ms);

		// unregister() 안에서 부른다. 항목 존재 여부와 무관하게 유예를 건다 —
		// 유예가 지키는 것은 "이 연결이 방금 정상적으로 나가겠다고 알렸다"는
		// 사실이지 서버 항목의 존재가 아니다. public 인 이유는 호출부가
		// unregister() 하나로 고정돼 있어도, 그 계약을 이 클래스 밖에서
		// 읽을 수 있어야(단위 시험 등) 하기 때문이다.
		void note_unregister(uint64_t net_session_id, uint64_t now_ms);

		// heartbeat() 의 반환형을 3상태로 바꾸지 않는다 — 기존 호출부의 뜻이
		// 흔들린다. 대신 조회를 따로 둔다. 두 메서드는 반드시 router 의 같은
		// 임계구역·같은 now_ms 안에서 이어 불러야 한다(registry.h 머리 주석의
		// "한 임계구역 안의 연쇄 호출이 같은 「지금」을 보게 된다" 그 이유다) —
		// heartbeat() 가 false 를 낸 바로 그 순간의 유예 여부를 물어야지, 따로
		// now_ms 를 다시 떠서 물으면 그 사이 유예가 방금 끝났을 수 있다.
		bool in_unregister_grace(uint64_t net_session_id, uint64_t now_ms) const;

		// 접속 테이블 — player_id -> server_id. PlayerEnter/PlayerLeave/FullSync
		// 수신이 만지는 자리다. now_ms 를 안 받는 이유 — 이 테이블에는 만료
		// 개념이 없다(entries_/reservations_ 와 다르다).
		void player_entered(uint64_t player_id, uint32_t server_id);

		// server_id 가 일치하는 항목일 때만 지운다 — 다른 서버가 이미 같은
		// player_id 로 되잡은 항목을 늦게 도착한 PlayerLeave 가 지우면 안 된다.
		bool player_left(uint64_t player_id, uint32_t server_id);

		// 동기 Kick 의 중복 감지가 쓰는 조회 — player_id 가 지금 어느 서버에
		// 붙어 있는지를 본다. 뮤텍스는 router 소유(기존 계약 — 락 없이 부르면
		// 안 된다). 없으면 nullopt.
		std::optional<uint32_t> find_connection(uint64_t player_id) const;

		// FullSync 수신. first_chunk(=chunk_idx==0) 에서 그 서버 소유 항목을
		// 통째로 비우고, 매 호출마다 ids 를 싣는다 — 청크가 여러 개면 이 함수가
		// 여러 번 불려 누적된다. 중간에 링크가 끊겨 청크가 덜 온 채 남는 경우를
		// 따로 다루지 않는다 — 그 서버 항목은 orphan → 유예 삭제 경로로 결국
		// drop_server_connections 되고, 마을이 재연결하면 on_register_ack 이
		// FullSync 를 처음부터 다시 보낸다(마을 s2s_link.cpp). 재조립 타임아웃·
		// 부분 상태 추적 같은 장치는 자연 복구되는 문제에 만드는 것이다.
		// 반환은 타서버 소유라 덮어쓰지 않은 id 들 — 첫 청크 삭제가 자기 항목만
		// 지우므로 타서버 항목이 청크 내내 남아 이 대조가 성립한다. 진위 판정
		// (Kick 왕복)은 호출부 몫이다.
		std::vector<uint64_t> full_sync_replace(uint32_t server_id, bool first_chunk,
			const std::vector<uint64_t>& ids);

		// 그 서버 소유 접속 항목을 전부 지운다. 반환은 지운 개수(로그·검산용).
		// unregister()·orphan 유예 초과 삭제(sweep) 양쪽이 이 메서드로 접속
		// 테이블을 정리한다 — 「서버 항목이 없어지면 그 접속 항목도 없어진다」는
		// 불변식을 한 곳에서 지킨다.
		size_t drop_server_connections(uint32_t server_id);

		size_t connection_count() const { return connections_.size(); }

		// 항목 즉시 삭제 — 명시 보고라 orphan 유예가 필요 없다(재연결로 복구될
		// 상황이 아니다). 탈착한 link 를 반환한다(없으면 null) — release 는
		// router 가 뮤텍스 밖에서 한다. 그 서버의 접속 테이블 항목도 함께
		// 지운다(drop_server_connections). now_ms 는 heartbeat 유예의 기산점을
		// 남기는 데 쓴다(note_unregister) — 항목이 있든 없든(한 번도 등록한
		// 적 없는 연결이어도) 유예는 건다. unregister() 자체가 멱등 계약
		// (§8-2 「없음도 성공」)이라 그 부수효과도 조건 없이 맞춘다 — S2S 는
		// 신뢰 도메인이고 유예는 정해진 시간 뒤 스스로 만료하므로, 등록 이력
		// 없는 연결이 잠깐 heartbeat 면제를 얻어도 실해가 없다.
		net::Session* unregister(uint64_t net_session_id, uint64_t now_ms);

		// orphan 표시 + link 탈착·반환. 항목은 남긴다 — 마을은 정상 종료도
		// Unregister 없이 끊으므로(F10) 끊김이 기본 경로고, 재연결(부활)로 복구될
		// 수 있다(§5-3). 남는 주소 정보가 그 부활의 매칭 키다.
		net::Session* on_link_down(uint64_t net_session_id, uint64_t now_ms);

		std::optional<Assignment> assign(uint64_t player_id, uint32_t expire_ms, uint64_t now_ms);

		// 배정 제외 표시를 켜고 끈다. server_id 로 찾는 이유 — 운영자가 다루는
		// 단위가 server_id 지 net_session_id(연결)가 아니다. 항목이 없으면
		// false — 호출부(router)가 그 사실을 로그로 남긴다.
		bool set_draining(uint32_t server_id, bool draining);

		// ReserveAck 거절의 회수 전용 — expire 까지 남겨 두면 거절된 예약이 그
		// 서버의 배정 부하에 유령으로 잡힌다. 대조 키가 발급 세대(issue_id)인
		// 이유: server_id 만 보면 「같은 서버로의 재로그인」이 만든 새 예약을 옛
		// 거절이 지운다 — 세대는 발급마다 유일해 늦게 온 거절이 정확히 자기
		// 발급분만 지운다.
		bool revoke_reservation(uint64_t player_id, uint64_t issue_id);

		// 만료 예약 회수 + 유예(orphan_grace_ms)를 넘긴 orphan 항목 삭제.
		SweepResult sweep(uint64_t now_ms);

		// stop 절차 전용 — 전 항목의 잔존 link 를 탈착해 돌려준다. net stop 의
		// session_gone 발화는 전수 보장이 아니라(워커 조기 이탈 — router 쪽 주석
		// 참조) 잔존이 정상 종료에서도 있을 수 있다. 처분(감사 기록)은 router 몫.
		std::vector<net::Session*> detach_all();

		// register_server 가 need_link=true 를 돌려준 그 임계구역 안에서 router 가
		// acquire 한 포인터를 되붙이는 짝이다. Registry 가 acquire 를 못 하므로
		// (net 불호출) 저장만 분리됐다. 항목이 없으면 false — 같은 임계구역이라
		// 정상 경로에선 있을 수 없고, 호출부가 로그로 남길 일이다.
		bool attach_link(uint64_t net_session_id, net::Session* link);

		// Reserve 발신 직전 대상 항목의 link 조회 — 뮤텍스 안에서 조회와 send 를
		// 마쳐야 다른 스레드의 탈착·release 와 배타된다(경계는 router 가 지킨다).
		// null 이면 링크 부재(orphan 직후 등) — 발신을 스킵할지는 호출부 정책이다.
		net::Session* link_of(uint32_t server_id);

		// 링크 다운 통지가 「아는 연결이었나」를 구분해야 해서 있다 — on_link_down 의
		// null 반환은 「항목이 없다」와 「항목은 있는데 link 가 이미 탈착됐다」를
		// 가리지 못하는데, 삭제된 항목의 뒤늦은 통지는 no-op 이 정상임을 로그로
		// 남겨야 감사가 된다.
		bool contains(uint64_t net_session_id) const;

		// PlayerEnter/PlayerLeave/FullSync 가 실려 온 S2S 연결이 어느 서버인지
		// 찾는다 — 그 셋은 프레임에 server_id 를 안 싣고 net_session_id(연결
		// 자체)로만 신원을 밝히므로, 접속 테이블을 만지기 전에 이 변환이 필요하다.
		// 0 은 무효값이다(등록 거부 응답과 같은 관례) — 항목이 없으면 0.
		uint32_t server_id_of(uint64_t net_session_id) const;

		// 종료 요약의 「잔여 예약」 — 발급 = 덮어쓰기 + 만료 + 회수 + 잔여 검산용.
		size_t reservation_count() const { return reservations_.size(); }

		void note_version_rejected();
		const Stats& stats() const { return stats_; }

	private:
		struct Reservation {
			uint32_t server_id;
			uint64_t expire_at_ms;
			uint64_t issue_id;
		};

		ServerEntry* find_by_net_session(uint64_t net_session_id);
		uint64_t valid_reservations(uint32_t server_id, uint64_t now_ms) const;

		uint32_t registry_capacity_;
		uint64_t health_period_ms_;
		uint64_t health_fail_count_;
		uint64_t orphan_grace_ms_;
		uint64_t unregister_grace_ms_;
		uint32_t next_server_id_;
		uint64_t next_issue_id_;

		// 정원이 두 자릿수(설정 기본 16)라 선형 탐색이 보조 인덱스보다 단순하고
		// 충분하다 — 부활이 net_session_id 를 바꾸므로 그 키로 맵을 잡으면
		// 삭제·재삽입이 오히려 실수 표면이 된다.
		std::vector<ServerEntry> entries_;
		std::map<uint64_t, Reservation> reservations_;

		// player_id -> server_id. entries_/reservations_ 와 달리 만료가 없어
		// 절대 시각을 안 든다 — 제거는 명시 이벤트(PlayerLeave·FullSync 교체·
		// 서버 항목 삭제)로만 일어난다.
		std::map<uint64_t, uint32_t> connections_;

		// net_session_id -> 유예 만료 절대 시각. sweep() 이 지난 항목을 청소한다
		// (링크가 끊겨도 on_link_down 은 이 맵을 모를 수 있으니, 시각 기준 청소가
		// 유일한 회수 경로여야 새는 자리가 없다).
		std::map<uint64_t, uint64_t> unregister_grace_until_;

		Stats stats_;
	};

}	// namespace session
