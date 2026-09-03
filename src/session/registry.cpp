#include "session/registry.h"

#include "proto/s2s_packet.h"

namespace session {

	Registry::Registry(uint32_t registry_capacity, uint32_t health_period_ms,
			uint32_t health_fail_count, uint64_t orphan_grace_ms, uint64_t unregister_grace_ms)
		: registry_capacity_(registry_capacity)
		, health_period_ms_(health_period_ms)
		, health_fail_count_(health_fail_count)
		, orphan_grace_ms_(orphan_grace_ms)
		, unregister_grace_ms_(unregister_grace_ms)
		, next_server_id_(1)     // 0 은 무효값 — 거부 응답의 server_id 로 쓴다
		, next_issue_id_(1) {
	}

	RegisterResult Registry::register_server(uint64_t net_session_id, const std::string& host,
			uint16_t port, uint32_t capacity, uint32_t current, uint64_t now_ms) {
		// 매칭 ① — 같은 연결의 재등록. 갱신만 하는 멱등 처리다(§8-2). 행에
		// 걸렸다 풀린 연결이 Register 부터 다시 보낼 수도 있으므로 orphan 이면
		// 함께 복원한다 — heartbeat 의 복원과 같은 이유다. 이미 홀드를 보관 중이라
		// need_link 는 false 다.
		if (ServerEntry* entry = find_by_net_session(net_session_id)) {
			entry->host = host;
			entry->port = port;
			entry->capacity = capacity;
			entry->current = current;
			entry->last_heartbeat_ms = now_ms;
			entry->orphaned_at_ms = 0;
			++stats_.registered;
			return {entry->server_id, proto::s2s::kResultOk, false, nullptr};
		}

		// 매칭 ② — 같은 host:port 의 orphan 부활(마을 재기동 — 새 연결이라
		// net_session_id 가 다르다). 복수면 orphaned_at 최신 항목을 잡는다 —
		// 더 최근까지 살아 있던 쪽이 같은 서버의 직전 화신일 가능성이 높다.
		ServerEntry* revive = nullptr;
		for (ServerEntry& entry : entries_) {
			if (entry.orphaned_at_ms == 0 || entry.host != host || entry.port != port) {
				continue;
			}
			if (revive == nullptr || entry.orphaned_at_ms > revive->orphaned_at_ms) {
				revive = &entry;
			}
		}
		if (revive != nullptr) {
			// lazy orphan(연결이 산 채 판정만 초과한 뒤 재기동)이었다면 옛 연결의
			// 홀드가 남아 있다 — 탈착해 돌려주고 release 는 router 가 뮤텍스
			// 밖에서 한다. server_id 는 유지한다 — 같은 서버의 부활이지 새 서버가
			// 아니고, 배정·로그의 연속성이 그 값에 걸려 있다.
			net::Session* stale = revive->link;
			revive->link = nullptr;
			revive->net_session_id = net_session_id;
			revive->host = host;
			revive->port = port;
			revive->capacity = capacity;
			revive->current = current;
			revive->last_heartbeat_ms = now_ms;
			revive->orphaned_at_ms = 0;
			++stats_.registered;
			return {revive->server_id, proto::s2s::kResultOk, true, stale};
		}

		// 정원 검사가 매칭 ①② 뒤인 이유 — 둘은 항목을 늘리지 않으므로 가득 찬
		// 테이블에서도 재등록·부활은 성공해야 한다(멱등). orphan 도 유예가 끝날
		// 때까지 자리를 차지한다 — 부활의 매칭 대상이므로 정원에서 빼면 유예가
		// 정원을 조용히 늘리는 셈이 된다.
		if (entries_.size() >= registry_capacity_) {
			++stats_.rejected_full;
			return {0, proto::s2s::kResultFull, false, nullptr};
		}

		// 매칭 ③ — 신규. lazy 판정 전의 급재기동이면 비-orphan 동일 host:port
		// 항목과 일시 공존한다 — 옛 항목은 헬스 판정 → 유예 삭제로 스스로
		// 사라지므로 막는 장치를 두지 않는다(§18-7 — 손해가 없으면 장치도 없다).
		ServerEntry entry{};
		entry.server_id = next_server_id_++;
		entry.net_session_id = net_session_id;
		entry.link = nullptr;
		entry.host = host;
		entry.port = port;
		entry.capacity = capacity;
		entry.current = current;
		entry.last_heartbeat_ms = now_ms;
		entry.orphaned_at_ms = 0;
		entries_.push_back(entry);
		++stats_.registered;
		return {entry.server_id, proto::s2s::kResultOk, true, nullptr};
	}

	bool Registry::heartbeat(uint64_t net_session_id, uint32_t current, uint64_t now_ms) {
		ServerEntry* entry = find_by_net_session(net_session_id);
		if (entry == nullptr) {
			return false;
		}
		entry->current = current;
		entry->last_heartbeat_ms = now_ms;
		// lazy orphan 뒤 같은 연결의 하트비트는 행이 풀렸다는 뜻이다 — 살아 있는
		// 서버가 유예 삭제되면 안 되므로 되살린다(§5-3 복구 정신).
		entry->orphaned_at_ms = 0;
		return true;
	}

	net::Session* Registry::unregister(uint64_t net_session_id, uint64_t now_ms) {
		// 항목이 있든 없든 유예를 건다 — 이 연결은 정상적으로 나가겠다고
		// 알렸고, drain 중에도 계속 heartbeat 를 보낼 수 있다는 사실은 항목의
		// 존재와 무관하다.
		note_unregister(net_session_id, now_ms);
		for (auto it = entries_.begin(); it != entries_.end(); ++it) {
			if (it->net_session_id != net_session_id) {
				continue;
			}
			net::Session* link = it->link;
			const uint32_t server_id = it->server_id;
			entries_.erase(it);
			++stats_.entries_removed;
			drop_server_connections(server_id);
			return link;
		}
		return nullptr;
	}

	void Registry::note_unregister(uint64_t net_session_id, uint64_t now_ms) {
		unregister_grace_until_[net_session_id] = now_ms + unregister_grace_ms_;
	}

	bool Registry::in_unregister_grace(uint64_t net_session_id, uint64_t now_ms) const {
		const auto it = unregister_grace_until_.find(net_session_id);
		return it != unregister_grace_until_.end() && it->second > now_ms;
	}

	void Registry::player_entered(uint64_t player_id, uint32_t server_id) {
		connections_[player_id] = server_id;
		++stats_.connections_added;
	}

	bool Registry::player_left(uint64_t player_id, uint32_t server_id) {
		const auto it = connections_.find(player_id);
		if (it == connections_.end() || it->second != server_id) {
			return false;
		}
		connections_.erase(it);
		++stats_.connections_removed;
		return true;
	}

	std::optional<uint32_t> Registry::find_connection(uint64_t player_id) const {
		const auto it = connections_.find(player_id);
		if (it == connections_.end()) {
			return std::nullopt;
		}
		return it->second;
	}

	std::vector<uint64_t> Registry::full_sync_replace(uint32_t server_id, bool first_chunk,
			const std::vector<uint64_t>& ids) {
		if (first_chunk) {
			for (auto it = connections_.begin(); it != connections_.end(); ) {
				if (it->second == server_id) {
					it = connections_.erase(it);
					++stats_.connections_removed;
				} else {
					++it;
				}
			}
			++stats_.fullsync_replaced;
		}
		std::vector<uint64_t> conflicts;
		for (uint64_t player_id : ids) {
			const auto it = connections_.find(player_id);
			if (it != connections_.end() && it->second != server_id) {
				// 타서버 소유 항목은 덮어쓰지 않는다 — 덮어쓰면 그 서버의 PlayerLeave 가
				// 소유자 대조(player_left)에 막혀 항목이 영구 유령으로 남는다. 어느 쪽이
				// 진짜인지는 호출부의 Kick 왕복이 판정한다. connections_added 도 안
				// 센다 — 삽입하지 않은 원소를 세면 추가 = 제거 + 잔여 검산이 깨진다.
				conflicts.push_back(player_id);
				continue;
			}
			connections_[player_id] = server_id;
			++stats_.connections_added;
		}
		return conflicts;
	}

	size_t Registry::drop_server_connections(uint32_t server_id) {
		size_t removed = 0;
		for (auto it = connections_.begin(); it != connections_.end(); ) {
			if (it->second == server_id) {
				it = connections_.erase(it);
				++removed;
			} else {
				++it;
			}
		}
		stats_.connections_removed += removed;
		return removed;
	}

	net::Session* Registry::on_link_down(uint64_t net_session_id, uint64_t now_ms) {
		ServerEntry* entry = find_by_net_session(net_session_id);
		if (entry == nullptr) {
			// 이미 unregister·부활로 이 연결과의 매칭이 끊긴 항목의 뒤늦은 통지 —
			// no-op 이어야 이중 처리가 없다(로그는 호출부가 남긴다).
			return nullptr;
		}
		// lazy 로 이미 orphan 이었다면 시각을 덮지 않는다 — 유예는 「링크를 잃은
		// 시점」이 아니라 「죽었다고 처음 판정한 시점」부터 세는 것이 맞다.
		if (entry->orphaned_at_ms == 0) {
			entry->orphaned_at_ms = now_ms;
			++stats_.orphan_transitions;
		}
		net::Session* link = entry->link;
		entry->link = nullptr;
		return link;
	}

	std::optional<Assignment> Registry::assign(uint64_t player_id, uint32_t expire_ms, uint64_t now_ms) {
		// 헬스는 여기서 lazy 로 판정한다(§18-3 「정확성은 lazy」) — 죽은 서버로
		// 배정이 나가면 안 되는 순간은 배정 직전뿐이라 주기 검사가 필요 없다.
		// 이 경로의 orphan 전이는 연결이 살아 있으므로 link 를 탈착하지 않는다 —
		// 그 홀드는 부활·sweep 삭제·session_gone 중 먼저 오는 쪽이 회수한다.
		// 경과 판정을 뺄셈이 아니라 덧셈 비교로 한다 — 호출부가 시각을 락 밖에서
		// 뜨므로 「먼저 시각을 뜬 스레드가 나중에 락을 잡는」 역전이 가능하고,
		// 그때 저장값이 now 보다 커진다. 뺄셈은 uint64 언더플로로 2^64 근처가 되어
		// 즉시 초과 판정(방금 하트비트한 서버가 orphan)이 되고, 덧셈은 그냥 false
		// 로 끝난다. 이 크기대(임계값 ≤ 시간 축소값 몇 초)는 덧셈 오버플로가 없다.
		const uint64_t health_timeout_ms = health_period_ms_ * health_fail_count_;
		for (ServerEntry& entry : entries_) {
			if (entry.orphaned_at_ms == 0
				&& entry.last_heartbeat_ms + health_timeout_ms < now_ms) {
				entry.orphaned_at_ms = now_ms;
				++stats_.orphan_transitions;
			}
		}

		// load = (current + 유효 예약 수) / capacity 최소 선택. 나눗셈 대신
		// 곱셈 교차 비교 — 정수로 정확하고 부동소수 오차가 동률 판정을 흔들지
		// 않는다. 동률이면 server_id 최소(등록 순) — 하네스 기대값이 결정적이
		// 되게 한다.
		const ServerEntry* best = nullptr;
		uint64_t best_load_num = 0;
		for (const ServerEntry& entry : entries_) {
			if (entry.orphaned_at_ms != 0) {
				continue;
			}
			if (entry.capacity == 0) {
				// 정원 0 은 아무도 못 받는다 — 비교식의 분모이기도 해서 값이 아니라
				// 의미로 먼저 거른다.
				continue;
			}
			if (entry.draining) {
				// 드레인 중인 서버는 정원·부하와 무관하게 배정 후보에서 뺀다 —
				// 새 유저가 곧 내릴 서버로 들어가면 안 된다.
				continue;
			}
			const uint64_t load_num = entry.current + valid_reservations(entry.server_id, now_ms);
			if (best == nullptr) {
				best = &entry;
				best_load_num = load_num;
				continue;
			}
			const uint64_t lhs = load_num * best->capacity;
			const uint64_t rhs = best_load_num * entry.capacity;
			if (lhs < rhs || (lhs == rhs && entry.server_id < best->server_id)) {
				best = &entry;
				best_load_num = load_num;
			}
		}
		if (best == nullptr) {
			++stats_.assign_no_server;
			return std::nullopt;
		}

		// 같은 player_id 의 예약은 덮어쓴다 — 중복 로그인 잠정 정책(§8-2 멱등,
		// ADR-020 결정 9). 옛 예약의 만료 여부와 무관하게 덮어쓰기로 센다 —
		// 예약 하나가 끝나는 경로(덮어쓰기·만료·잔여)가 정확히 하나여야 종료
		// 요약의 수지가 맞는다.
		if (reservations_.find(player_id) != reservations_.end()) {
			++stats_.reservations_overwritten;
		}
		const uint64_t issue_id = next_issue_id_++;
		reservations_[player_id] = Reservation{best->server_id, now_ms + expire_ms, issue_id};
		++stats_.reservations_issued;
		++stats_.assigned;
		return Assignment{best->server_id, best->host, best->port, issue_id};
	}

	SweepResult Registry::sweep(uint64_t now_ms) {
		SweepResult result{};
		for (auto it = reservations_.begin(); it != reservations_.end();) {
			if (it->second.expire_at_ms <= now_ms) {
				it = reservations_.erase(it);
				++result.expired_reservations;
				++stats_.reservations_expired;
			} else {
				++it;
			}
		}
		// 유예를 넘긴 orphan 만 지운다 — 삭제는 정확성이 아니라 메모리 회수다
		// (§18-3. 배정 제외는 orphan 표시가 이미 한다). lazy orphan(연결 생존)
		// 항목엔 홀드가 남아 있을 수 있다 — 탈착해 돌려주고 release 는 router 가
		// 뮤텍스 밖에서 한다.
		for (auto it = entries_.begin(); it != entries_.end();) {
			// 덧셈 비교인 이유는 assign 의 헬스 판정 주석 참조(시각 역전 언더플로).
			if (it->orphaned_at_ms != 0 && it->orphaned_at_ms + orphan_grace_ms_ < now_ms) {
				if (it->link != nullptr) {
					result.stale_links.push_back(it->link);
				}
				drop_server_connections(it->server_id);
				it = entries_.erase(it);
				++result.removed_entries;
				++stats_.entries_removed;
			} else {
				++it;
			}
		}

		// heartbeat 유예 청소 — 링크가 끊긴 채로도 이 맵은 남을 수 있어서
		// (on_link_down 은 이 맵을 모른다), 시각 기준 청소가 유일한 회수
		// 경로다. 그래야 새는 자리가 없다.
		for (auto it = unregister_grace_until_.begin(); it != unregister_grace_until_.end(); ) {
			if (it->second <= now_ms) {
				it = unregister_grace_until_.erase(it);
			} else {
				++it;
			}
		}
		return result;
	}

	std::vector<net::Session*> Registry::detach_all() {
		std::vector<net::Session*> links;
		for (ServerEntry& entry : entries_) {
			if (entry.link != nullptr) {
				links.push_back(entry.link);
				entry.link = nullptr;
			}
		}
		return links;
	}

	bool Registry::attach_link(uint64_t net_session_id, net::Session* link) {
		ServerEntry* entry = find_by_net_session(net_session_id);
		if (entry == nullptr) {
			return false;
		}
		// 비-null link 를 덮지 않는다 — 덮인 옛 홀드는 회수 경로를 잃는다.
		// need_link=true 갈래는 전부 탈착 뒤라 정상적으로 올 수 없는 상태고,
		// false 를 돌려 호출부가 증거를 남기게 한다.
		if (entry->link != nullptr) {
			return false;
		}
		entry->link = link;
		return true;
	}

	bool Registry::revoke_reservation(uint64_t player_id, uint64_t issue_id) {
		const auto it = reservations_.find(player_id);
		if (it == reservations_.end() || it->second.issue_id != issue_id) {
			return false;
		}
		reservations_.erase(it);
		++stats_.reservations_revoked;
		return true;
	}

	net::Session* Registry::link_of(uint32_t server_id) {
		for (ServerEntry& entry : entries_) {
			if (entry.server_id == server_id) {
				return entry.link;
			}
		}
		return nullptr;
	}

	bool Registry::contains(uint64_t net_session_id) const {
		for (const ServerEntry& entry : entries_) {
			if (entry.net_session_id == net_session_id) {
				return true;
			}
		}
		return false;
	}

	uint32_t Registry::server_id_of(uint64_t net_session_id) const {
		for (const ServerEntry& entry : entries_) {
			if (entry.net_session_id == net_session_id) {
				return entry.server_id;
			}
		}
		return 0;
	}

	void Registry::note_version_rejected() {
		++stats_.rejected_version;
	}

	bool Registry::set_draining(uint32_t server_id, bool draining) {
		for (ServerEntry& entry : entries_) {
			if (entry.server_id == server_id) {
				entry.draining = draining;
				return true;
			}
		}
		return false;
	}

	ServerEntry* Registry::find_by_net_session(uint64_t net_session_id) {
		for (ServerEntry& entry : entries_) {
			if (entry.net_session_id == net_session_id) {
				return &entry;
			}
		}
		return nullptr;
	}

	uint64_t Registry::valid_reservations(uint32_t server_id, uint64_t now_ms) const {
		// 서버별 카운터로 들고 다니지 않고 매번 센다 — 만료가 lazy 라 카운터는
		// 「지금 유효한 수」를 모르고, 테이블 규모(정원 두 자릿수·예약도 그 수준)
		// 에서 순회 비용은 문제가 안 된다.
		uint64_t count = 0;
		for (const auto& pair : reservations_) {
			if (pair.second.server_id == server_id && pair.second.expire_at_ms > now_ms) {
				++count;
			}
		}
		return count;
	}

}	// namespace session
