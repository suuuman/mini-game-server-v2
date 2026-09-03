//  app/entry_table.h — 마을의 명부(L1). 입장 플레이어 집합·존 인덱스·로그인
//  예약을 한 뮤텍스 아래 같이 둔다.
//
//  전에는 player_id -> session_id 값 하나만 담았다. 이제는 net::Session* 와
//  그 세션이 지금 어느 존에 있는지(zone·in_zone)까지 같이 담는다 — Zone 이
//  들고 있던 members 벡터가 여기로 흡수됐기 때문이다(ADR-021 결정 2 문언
//  정정: "Session* 를 담지 않는다" → "S2S 스레드 경로의 API 는 id 값만
//  노출한다"). find_session() 은 여전히 값(session_id)만 돌려준다 — 동기
//  Kick 이 부르는 경로가 S2S 스레드이기 때문이다. Session* 를 돌려주는
//  API(find_acquire_by_session_id·snapshot_zone·snapshot_all)는
//  전부 직렬 큐 워커(로직)만 부른다는 계약이고, 반환 전에 그 세션의 io_count 를
//  올려(acquire) 락 밖으로 들고 나가도 안전하게 만든다 — 찾은 순간과 반환한
//  순간 사이에 그 세션이 사라지면 안 되기 때문이다(§7-2 규칙 4).
//
//  session_id 를 넣게 된 이유는 그대로다 — player_id 만으로는 "이미 들어와
//  있는 player_id 를 다른 세션이 지울 수 있다"는 구멍이 있었다. leave() 가
//  "넣은 그 세션"인지 대조하는 것이 그 구멍을 막는 유일한 방어다(자세한
//  근거는 leave() 주석 참조).
//
//  존 인덱스(zone_index_)를 entered_ 와 같은 뮤텍스 아래 두는 이유 — 둘이
//  어긋나면 안 되는 한 쌍이다. 어떤 세션이 "입장 중"이면서 "존 인덱스엔
//  없다"거나 그 반대인 순간이 있으면 채팅/브로드캐스트가 유령 멤버를
//  만들거나 실재 멤버를 빠뜨린다. 락을 둘로 쪼개면 그 틈이 생긴다.
//
//  zone_index_ 를 만지는 자리는 enter·leave·move_zone 셋뿐이다. 이 규약이
//  깨지면 "존 스레드가 이 존은 자기만 만진다"는 전제와 별개로 인덱스
//  자체의 정합성이 깨진다 — 다른 자리에서 새로 손대지 않는다.
//
//  Entry::in_zone 이 따로 있는 이유 — 존 미가입(Enter 직후·이동 창)과 존 0
//  소속을 구분해야 한다. 존 id 0(로비)은 유효한 값이라, zone 필드가 0 이라는
//  사실만으로는 "아직 어디에도 없다"와 "로비에 있다"를 가릴 수 없다.
//
//  입장 집합과 예약 테이블을 한 뮤텍스 아래 같이 두는 이유 — Enter 처리가 예약을
//  소비하고 집합에 넣는 것을 한 흐름에서 해야 하는데, 락을 둘로 쪼개면 그 사이에
//  다른 존 스레드가 같은 player_id 의 Reserve/Enter 를 끼워 넣을 수 있어 락 순서
//  문제가 새로 생긴다. 한 락 안에서 둘을 다 처리하면 그 문제 자체가 없다.
//
//  이 저장소 최초의 "Zone 이 아닌 전역 테이블 + 자체 락" 이다 — 절대 규칙 6번
//  ("Zone 안에 락을 넣지 않는다")은 world::Zone 객체 내부가 대상이고 이 클래스는
//  그 밖에 있다(ADR-021 결정 4). 락은 이 클래스 안에서만 잡고 안에서만 푼다 —
//  호출자는 락을 의식하지 않는다. snapshot() 은 복사본을 돌려주고 송신은 항상
//  락 밖에서 한다 — 세션 서버가 "탈착은 뮤텍스 안 · release 는 밖"으로 배타를
//  만든 것과 같은 규율이다.
//
//  set_notifier() 로 꽂은 콜백은 enter/leave 가 성공한 바로 그 임계구역 안에서
//  불린다 — 알림 제출과 등록/제거 사이에 다른 존 스레드가 끼어들 여지를
//  없애려는 것이다(이월 1 흡수). 콜백은 커넥터 명령 큐에 push_back 하는 것
//  이상을 하면 안 된다 — with_snapshot() 이 이미 지키는 것과 같은 계약이다.
//
//  예약의 정확성은 스윕이 아니라 소비 시점의 lazy 확인에 있다 — sweep_expired 는
//  회수 건수를 로그에 남기기 위한 청소일 뿐, consume_reservation 이 만료를 직접
//  확인하므로 스윕 주기와 무관하게 항상 옳다. 스윕이 한 바퀴 안 돌아도 만료된
//  예약으로는 Enter 할 수 없다.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "core/lock_rank.h"

namespace net { struct Session; }

namespace app {

    // 이 별칭이 mutex_ 의 실제 타입을 가리키는 유일한 지점이다 — 종류를 바꿀
    // 일이 생기면(A11 재평가가 shared_mutex 쪽 이득을 실측으로 보일 때) 여기
    // 한 줄만 고치면 되도록 교체 지점을 하나로 고정한다(ADR-025 결정 1).
    using RosterMutex = std::mutex;

    class EntryTable {
    public:
        EntryTable() = default;

        EntryTable(const EntryTable&) = delete;
        EntryTable& operator=(const EntryTable&) = delete;

        // move_zone() 의 반환 — 호출자가 "이전 존"을 아는 유일한 통로다.
        // had_prev 가 false 면 prev_zone 은 안 쓴다(존 미가입 상태에서 첫 이동).
        struct MoveResult {
            bool ok = false;
            bool had_prev = false;
            uint32_t prev_zone = 0;
        };

        // Reserve 수신 시. 같은 player_id 로 다시 오면 덮어쓴다(§8-2 멱등) — 옛
        // 예약의 만료 여부와 무관하게 최신 expire_at_ms 로 대체된다. 반환은
        // "새로 넣었나"가 아니라 "저장에 성공했나"다 — 지금은 실패 갈래가 없어
        // 항상 true 지만, 시그니처를 미리 bool 로 둔 것은 add_reservation 이
        // 유일하게 "쓰기 실패"라는 계약을 가질 수 있는 자리이기 때문이다.
        bool add_reservation(uint64_t player_id, uint64_t expire_at_ms);

        // Enter 처리 시. lazy 만료 확인 — 예약이 없으면 false, 만료됐으면 지우고
        // false, 유효하면 지우고 true. 세 갈래 모두 호출 한 번으로 소비까지
        // 끝난다 — 재사용을 막으려면 확인과 삭제가 같은 임계구역 안이어야 한다.
        bool consume_reservation(uint64_t player_id, uint64_t now_ms);

        // 입장 집합에 넣는다. 이미 있으면 false(중복 Enter) — 그 세션은 이미
        // 확정된 player_id 를 갖고 있어야 여기까지 오므로, false 는 같은
        // player_id 로 두 번째 Enter 가 온 것이지 재시도로 취급할 상황이 아니다.
        // 성공하면 in_zone=false 로 등록되고(아직 어느 존에도 없다), 이 임계구역
        // 안에서 notifier(pid, true) 가 불린다.
        bool enter(uint64_t player_id, net::Session& session);

        // leave() 의 반환 — 명부에서 실제로 빠졌는가(ok)와, 빠지기 전에 그
        // 세션이 어느 존 소속이었는가(had_zone·zone)를 함께 담는다. 존에
        // 한 번도 들어간 적 없는 세션의 leave 는 had_zone=false 다.
        // 호출자(on_session_gone)가 멤버 변동을 알릴 존을 고르는 유일한
        // 근거가 이 값이다 — 세션의 placement(라우팅 키)가 아니라 명부가
        // 기억하는 존이 정본이다. placement 는 존에 한 번도 안 들어간
        // 세션도 로비(0)를 가리키므로, 그 값으로 대상을 고르면 실제로는
        // 무관한 존의 멤버들에게 통지가 나간다.
        struct LeaveResult {
            bool ok = false;
            bool had_zone = false;
            uint32_t zone = 0;
        };

        // 입장 집합에서 뺀다. ok=false 인 두 갈래 — ① 아예 없다(Enter 를 안
        // 보냈거나 Enter 가 실패한 세션의 종료, 또는 Enter 와 idle 스윕이
        // 겹치는 레이스의 롤백. 정상 상황이라 no-op 이고, 그때는
        // PlayerLeave 를 보내지 않는 것이 짝의 계약이다) ② 있지만 넣은
        // session_id 가 다르다. 동기 Kick 이 이 갈래를 도달 가능하게 만들었다 —
        // Kick 당한 옛 세션의 정리 Job 이 늦게 돌 때, 같은 player_id 로 이미
        // 재입장한 새 세션의 항목을 이 대조가 지키지 않으면 늦은 leave 가
        // 새 주인의 자리를 지운다.
        // 성공하면 in_zone 이었을 때 그 존 인덱스 버킷에서도 같은 임계구역
        // 안에서 제거되고, notifier(pid, false) 가 불린다.
        LeaveResult leave(uint64_t player_id, uint64_t session_id);

        // 존 이동. session.player_id 로 Entry 를 찾는다 — 이 호출은 언제나
        // 그 세션 자신의 실행권을 쥔 스레드(존 스레드)에서만 오므로 그 필드를
        // 읽어도 안전하다(session.h 주석 참조). Entry 가 없으면 ok=false 로
        // no-op — 이미 명부에서 빠진 세션(예: on_session_gone 의 leave() 가
        // 이 호출보다 먼저 같은 존 스레드에서 돌았다)을 옮기려 하지 않는다.
        // 옛 버킷 제거(있었으면)·새 버킷 등록·zone/in_zone 갱신을 전부 한
        // 임계구역 안에서 끝낸다 — 완전 이전 1회. 두 단계로 나누면 "인덱스만
        // 먼저 새 존"인 중간 창이 생겨 다른 스레드가 그 틈을 통해 아직 오지
        // 않은 멤버에게 존 전용 메시지를 보낼 수 있다.
        MoveResult move_zone(net::Session& session, uint32_t to_zone);

        // 그 존 인덱스 버킷을 통째로 복사해 돌려준다. 원소마다 acquire 한다 —
        // 호출자는 반환된 개수만큼 정확히 release 해야 한다(수집 후 일괄 release
        // 형태로 쓸 것 — 중간에 조기 return 해도 N:N 이 깨지지 않게).
        void snapshot_zone(uint32_t zone, std::vector<net::Session*>& out) const;

        // 입장 집합 전원의 스냅샷(zone 무관) — 위와 같은 acquire 계약.
        void snapshot_all(std::vector<net::Session*>& out) const;

        // 그 존 인덱스 버킷의 크기만 본다. acquire 를 안 하는 이유는 개수만
        // 필요한 자리(JoinZoneAck 본문)에서 세션 포인터를 반출할 일이 없어서다.
        uint32_t zone_size(uint32_t zone) const;

        // session_id 로 찾아 acquire 하고 돌려준다 — 거래 상대 탐색이 쓴다.
        // 선형 스캔이다(entered_ 는 player_id 로 색인돼 있어 session_id 로는
        // 못 찾는다). 거래는 드물어 지금은 이 비용이 안 보인다 — 잦아지면
        // session_id -> player_id 역인덱스가 6단계 후보다.
        net::Session* find_acquire_by_session_id(uint64_t session_id) const;

        // player_id 로 찾아 acquire 하고 돌려준다 — 귓속말 대상 탐색이 쓴다.
        // entered_ 가 player_id 로 색인돼 있어 O(1)이다(위 find_acquire_by_session_id
        // 의 선형 스캔과 다른 이유가 이것). find_session 과 반환하는 것이
        // 다르다 — find_session 은 S2S 스레드(동기 Kick)가 부르는 값 전용
        // API 라 Session* 를 아예 안 돌려주고, 이쪽은 직렬 큐 워커(로직) 전용
        // 이라 acquire 한 Session* 를 락 밖으로 반출해도 안전하다. 계약은
        // find_acquire_by_session_id 와 같다 — 없으면 nullptr, 있으면 호출자가
        // 정확히 1회 release.
        net::Session* find_acquire_by_player_id(uint64_t player_id) const;

        // 동기 Kick 이 대상을 찾는 자리 — player_id 로 그것을 넣은
        // net::Session::id 를 값으로 돌려준다. Session* 는 여전히 담지도
        // 반환하지도 않는다(S2S 경로 API 는 id 값만 노출한다) — 돌려준 id 를
        // 들고 net::IocpServer 쪽 공개 API 로 넘기는 몫은 호출자가 진다.
        std::optional<uint64_t> find_session(uint64_t player_id) const;

        // 입장 집합 크기 — Register/Heartbeat 의 current 필드가 이 값이다.
        uint32_t current() const;

        // FullSync 발신용 전량 복사 — player_id(키)만 뽑는다. 세션 서버가 보는
        // 접속 테이블은 player_id 단위라 소유자 session_id 는 이 마을 안에서만
        // 쓰는 값이다. 락을 잡은 채 반환하지 않는다 — 호출부가 이 복사본을
        // 청크로 잘라 락 밖에서 보낸다.
        std::vector<uint64_t> snapshot() const;

        // snapshot() 과 달리 fn 을 mutex_ 를 쥔 채로 부른다 — 존 스레드의
        // enter/leave 가 같은 락을 타므로, 「스냅샷을 뜬 시점」과 「그
        // 스냅샷을 상대에게 다 보낸 시점」 사이에 끼어들 수 없다. 그 틈이
        // 있으면 증분 알림이 스냅샷보다 먼저 도착해 전량 교체가 방금 온
        // 증분을 되돌리는 순서 역전이 생긴다. fn 이 하는 일이 커넥터
        // 명령 큐에 넣는 것뿐이라면(실송신은 별도 스레드가 한다) 락 아래
        // 실행해도 안전하다 — 그 이상을 하는 fn 을 넘기지 않는다.
        void with_snapshot(const std::function<void(const std::vector<uint64_t>&)>& fn) const;

        // enter/leave 성공 시 그 임계구역 안에서 부를 콜백을 꽂는다. 배선은
        // bootstrap 이 한다(S2sLink::notify_player_enter/leave 를 여기 태운다) —
        // 이 클래스는 S2sLink 를 모른다. fn 은 커넥터 명령 큐 push_back 이상을
        // 하면 안 된다(with_snapshot 과 같은 제약).
        void set_notifier(std::function<void(uint64_t player_id, bool entered)> fn);

        // 만료된 예약을 회수한다. 반환은 이번 호출에서 지운 개수(로그용) —
        // 정확성은 이미 consume_reservation 의 lazy 확인이 보장하므로, 이 값은
        // 스레드가 실제로 도는지를 관측하는 용도지 정합성의 근거가 아니다.
        size_t sweep_expired(uint64_t now_ms);

        // 드레인 모드 — 세션 서버가 SetMode 로 켜고 끈다. 존 스레드(handle_enter)
        // 는 읽기만, S2S 스레드(SetMode 수신)는 쓰기만 한다 — 값 하나를 주고받는
        // 것뿐이라 위 mutex_ 를 같이 타지 않는다. 같이 타면 Enter 임계구역과
        // S2S 스레드가 서로를 기다리게 된다.
        void set_draining(bool value);
        bool draining() const;

    private:
        struct Entry {
            uint64_t session_id = 0;
            net::Session* session = nullptr;
            uint32_t zone = 0;
            bool in_zone = false;
        };

        mutable RosterMutex mutex_;
        std::unordered_map<uint64_t, Entry> entered_;   // player_id -> Entry
        std::unordered_map<uint32_t, std::vector<net::Session*>> zone_index_;   // zone -> 그 존 멤버
        std::unordered_map<uint64_t, uint64_t> reservations_;   // player_id -> expire_at_ms
        std::atomic<bool> draining_{false};
        std::function<void(uint64_t, bool)> notifier_;
    };

}   // namespace app
