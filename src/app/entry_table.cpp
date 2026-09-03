#include "app/entry_table.h"

#include "net/session.h"

#include <algorithm>

namespace {

    // 존 인덱스 버킷 하나에서 세션 포인터 하나를 뺀다. swap-and-pop 이라 순서가
    // 바뀐다 — world::Zone::remove 가 그랬던 것과 같은 이유로 상관없다(브로드캐스트는
    // 순서 무관이다). 버킷이 비면 지운다 — 존 id 는 실제로 쓰인 것만 늘어나야
    // 하고, 빈 벡터를 영구히 남기면 이동이 잦은 서버에서 지도만 계속 커진다.
    void erase_from_bucket(
        std::unordered_map<uint32_t, std::vector<net::Session*>>& zone_index,
        uint32_t zone, net::Session* s) {
        const auto it = zone_index.find(zone);
        if (it == zone_index.end()) {
            return;
        }
        auto& bucket = it->second;
        const auto vit = std::find(bucket.begin(), bucket.end(), s);
        if (vit == bucket.end()) {
            return;
        }
        *vit = bucket.back();
        bucket.pop_back();
        if (bucket.empty()) {
            zone_index.erase(it);
        }
    }

    // server.acquire_session() 과 같은 일(io_count++)이다. net::IocpServer 를
    // 끌어오지 않는 이유는 이 클래스가 net 계층의 그 큰 타입을 알 필요가 없어서다 —
    // io_count 는 Session 자신의 공개 필드고, 이미 Job·존·DB 요청 등 여러 소유자가
    // 「같은 사실」을 표시하려고 각자 이 카운터를 직접 올린다(world/zone.h 주석
    // 참조). 명부도 그 소유자 중 하나가 되는 것뿐이다.
    void acquire_hold(net::Session* s) {
        s->io_count.fetch_add(1);
    }

}   // namespace

namespace app {

    bool EntryTable::add_reservation(uint64_t player_id, uint64_t expire_at_ms) {
        core::LockRankGuard rank(core::LockRank::kRoster);
        std::lock_guard<RosterMutex> lock(mutex_);
        reservations_[player_id] = expire_at_ms;
        return true;
    }

    bool EntryTable::consume_reservation(uint64_t player_id, uint64_t now_ms) {
        core::LockRankGuard rank(core::LockRank::kRoster);
        std::lock_guard<RosterMutex> lock(mutex_);
        const auto it = reservations_.find(player_id);
        if (it == reservations_.end()) {
            return false;
        }
        const bool valid = it->second > now_ms;
        reservations_.erase(it);
        return valid;
    }

    bool EntryTable::enter(uint64_t player_id, net::Session& session) {
        core::LockRankGuard rank(core::LockRank::kRoster);
        std::lock_guard<RosterMutex> lock(mutex_);

        Entry e;
        e.session_id = session.id;
        e.session = &session;
        e.zone = 0;
        e.in_zone = false;

        const bool inserted = entered_.emplace(player_id, e).second;
        if (inserted && notifier_) {
            notifier_(player_id, true);
        }
        return inserted;
    }

    EntryTable::LeaveResult EntryTable::leave(uint64_t player_id, uint64_t session_id) {
        core::LockRankGuard rank(core::LockRank::kRoster);
        std::lock_guard<RosterMutex> lock(mutex_);

        LeaveResult result;
        const auto it = entered_.find(player_id);
        if (it == entered_.end() || it->second.session_id != session_id) {
            // 없거나(Enter 를 안 보냈거나 실패한 세션의 종료), 있어도 넣은
            // 세션이 다르다 — 어느 쪽이든 이 호출자는 지울 자격이 없다.
            return result;   // ok=false
        }

        result.ok = true;
        if (it->second.in_zone) {
            result.had_zone = true;
            result.zone = it->second.zone;
            erase_from_bucket(zone_index_, it->second.zone, it->second.session);
        }
        entered_.erase(it);

        if (notifier_) {
            notifier_(player_id, false);
        }
        return result;
    }

    EntryTable::MoveResult EntryTable::move_zone(net::Session& session, uint32_t to_zone) {
        core::LockRankGuard rank(core::LockRank::kRoster);
        std::lock_guard<RosterMutex> lock(mutex_);

        MoveResult result;
        const auto it = entered_.find(session.player_id);
        if (it == entered_.end()) {
            return result;   // Entry 부재 — no-op (ok=false)
        }

        Entry& e = it->second;
        result.ok = true;
        if (e.in_zone) {
            result.had_prev = true;
            result.prev_zone = e.zone;
            erase_from_bucket(zone_index_, e.zone, e.session);
        }

        zone_index_[to_zone].push_back(&session);
        e.zone = to_zone;
        e.in_zone = true;
        return result;
    }

    void EntryTable::snapshot_zone(uint32_t zone, std::vector<net::Session*>& out) const {
        core::LockRankGuard rank(core::LockRank::kRoster);
        std::lock_guard<RosterMutex> lock(mutex_);

        out.clear();
        const auto it = zone_index_.find(zone);
        if (it == zone_index_.end()) {
            return;
        }
        out.reserve(it->second.size());
        for (net::Session* s : it->second) {
            acquire_hold(s);
            out.push_back(s);
        }
    }

    void EntryTable::snapshot_all(std::vector<net::Session*>& out) const {
        core::LockRankGuard rank(core::LockRank::kRoster);
        std::lock_guard<RosterMutex> lock(mutex_);

        out.clear();
        out.reserve(entered_.size());
        for (auto& [player_id, e] : entered_) {
            acquire_hold(e.session);
            out.push_back(e.session);
        }
    }

    uint32_t EntryTable::zone_size(uint32_t zone) const {
        core::LockRankGuard rank(core::LockRank::kRoster);
        std::lock_guard<RosterMutex> lock(mutex_);

        const auto it = zone_index_.find(zone);
        return (it == zone_index_.end()) ? 0 : static_cast<uint32_t>(it->second.size());
    }

    net::Session* EntryTable::find_acquire_by_session_id(uint64_t session_id) const {
        core::LockRankGuard rank(core::LockRank::kRoster);
        std::lock_guard<RosterMutex> lock(mutex_);

        for (auto& [player_id, e] : entered_) {
            if (e.session_id == session_id) {
                acquire_hold(e.session);
                return e.session;
            }
        }
        return nullptr;
    }

    net::Session* EntryTable::find_acquire_by_player_id(uint64_t player_id) const {
        core::LockRankGuard rank(core::LockRank::kRoster);
        std::lock_guard<RosterMutex> lock(mutex_);

        const auto it = entered_.find(player_id);
        if (it == entered_.end()) {
            return nullptr;
        }
        acquire_hold(it->second.session);
        return it->second.session;
    }

    std::optional<uint64_t> EntryTable::find_session(uint64_t player_id) const {
        core::LockRankGuard rank(core::LockRank::kRoster);
        std::lock_guard<RosterMutex> lock(mutex_);
        const auto it = entered_.find(player_id);
        if (it == entered_.end()) {
            return std::nullopt;
        }
        return it->second.session_id;
    }

    uint32_t EntryTable::current() const {
        core::LockRankGuard rank(core::LockRank::kRoster);
        std::lock_guard<RosterMutex> lock(mutex_);
        return static_cast<uint32_t>(entered_.size());
    }

    std::vector<uint64_t> EntryTable::snapshot() const {
        core::LockRankGuard rank(core::LockRank::kRoster);
        std::lock_guard<RosterMutex> lock(mutex_);
        std::vector<uint64_t> ids;
        ids.reserve(entered_.size());
        for (const auto& [player_id, e] : entered_) {
            ids.push_back(player_id);
        }
        return ids;
    }

    void EntryTable::with_snapshot(const std::function<void(const std::vector<uint64_t>&)>& fn) const {
        core::LockRankGuard rank(core::LockRank::kRoster);
        std::lock_guard<RosterMutex> lock(mutex_);
        std::vector<uint64_t> ids;
        ids.reserve(entered_.size());
        for (const auto& [player_id, e] : entered_) {
            ids.push_back(player_id);
        }
        fn(ids);
    }

    void EntryTable::set_notifier(std::function<void(uint64_t, bool)> fn) {
        core::LockRankGuard rank(core::LockRank::kRoster);
        std::lock_guard<RosterMutex> lock(mutex_);
        notifier_ = std::move(fn);
    }

    size_t EntryTable::sweep_expired(uint64_t now_ms) {
        core::LockRankGuard rank(core::LockRank::kRoster);
        std::lock_guard<RosterMutex> lock(mutex_);
        size_t removed = 0;
        for (auto it = reservations_.begin(); it != reservations_.end(); ) {
            if (it->second <= now_ms) {
                it = reservations_.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
        return removed;
    }

    void EntryTable::set_draining(bool value) {
        draining_.store(value, std::memory_order_relaxed);
    }

    bool EntryTable::draining() const {
        return draining_.load(std::memory_order_relaxed);
    }

}   // namespace app
