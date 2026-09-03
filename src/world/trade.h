//  world/trade.h — 거래창 하나
//
//  Zone 에서 뗀 것은 거래의 수명이 존과 갈라지기 때문이다 — 존은 위치
//  라벨로 강등되고 거래는 두 세션이 직접 소유하는 구조
//  (atomic<shared_ptr<Trade>>)로 간다. Zone 이 사라진 뒤에도 이 구조체는
//  남아야 해서 별도 파일로 옮겼다. 내용은 world/zone.h 에 있던 것 그대로다.
#pragma once

#include "net/session.h"

#include <chrono>
#include <cstdint>

namespace world {

    //  단조 밀리초 — 「지금이 언제인가」를 묻는 유일한 창구. world/zone.h 에 있던
    //  것을 그대로 옮겼다(Zone 자체는 4단계에서 소멸 — 존이 사라져도 이 유틸은
    //  남는다. Reserve 만료·예약 스윕이 여전히 쓴다).
    //
    //  steady_clock 인 이유 — system_clock 은 NTP 보정이나 사람 손으로 되돌아갈
    //    수 있다. 만료 판정이 그 시계에 걸리면 시계를 되돌리는 순간 되살아나거나,
    //    앞으로 당기면 한꺼번에 풀린다.
    inline uint64_t now_ms() {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    //  거래창 하나
    //
    //  여기 담긴 Session* 에 별도 홀드를 잡지 않는다. 생성 시점에 양쪽 다
    //  명부(app::EntryTable)에 올라 있어 그 등록 홀드로 살아 있지만, 그
    //  뒤로 어느 한쪽이 먼저 사라질 수 있다는 사실 자체는 없어지지 않는다 —
    //  raw a/b 를 락 밖에서 그대로 반출(acquire_session(*peer))하면, 상대의
    //  정리(gone-Job)가 이미 끝나 소멸한 세션을 다시 사서 도는 UAF 창이
    //  생긴다. 그래서 상대를 확보하는 유일한 통로는 a_sid/b_sid 값으로
    //  app::EntryTable::find_acquire_by_session_id 를 거치는 것이다 — 그
    //  조회+acquire 가 EntryTable 자신의 락 한 임계구역이라, 상대가 이미
    //  명부를 떠났으면 nullptr 을 받고 살아 있으면 유효한 홀드를 동반한
    //  포인터를 받는다. raw a/b 포인터는 두 세션의 game_mutex 를 이미 둘 다
    //  쥔 구간에서만 동일성 비교·상태 갱신에 쓴다 — 그 구간에서는 access 로
    //  이미 확보된 상대이므로 역참조가 안전하다.
    //
    //  open == false 는 「요청은 갔지만 아직 수락 전」(pending) 이다.
    //  이 상태에서는 아이템을 올릴 수도, 확인할 수도 없다.
    struct Trade {
        uint32_t      id = 0;
        net::Session* a = nullptr;         // 요청한 쪽
        net::Session* b = nullptr;         // 요청받은 쪽
        uint64_t      a_sid = 0, b_sid = 0;   // a·b 의 세션 id — 상대 확보는 이 값으로만 한다
        uint32_t a_item = 0, a_count = 0;
        uint32_t b_item = 0, b_count = 0;
        bool     a_confirm = false;
        bool     b_confirm = false;
        bool     open = false;

        bool has(const net::Session* s) const { return s == a || s == b; }

        // s 가 이 거래의 당사자가 아니면 nullptr 이다.
        //   한때 `(s == a) ? b : a` 였다 — 당사자가 아닌 세션에 대해 조용히 a 를
        //   돌려줬다. 「모르는 값을 받았는데 그럴듯한 답을 준다」가 가장 나쁜 형태다.
        //   호출부가 그걸 상대로 알고 통지를 보내면 엉뚱한 사람이 취소 통지를 받는다.
        //   지금은 trade_of·clear_trade_of 가 has() 로 걸러 주므로 여기서
        //     nullptr 이 나올 경로가 없다. 그래도 「없으면 없다」고 답하게 둔다 —
        //     방어는 걸러 주는 쪽이 사라져도 남아 있어야 방어다.
        //   양쪽 game_mutex 를 이미 쥔 구간에서 동일성 비교·상태 갱신에만
        //     쓴다 — 그 밖에서 이 반환값을 락 없이 반출하지 않는다(아래
        //     peer_sid_of 참고).
        net::Session* peer_of(const net::Session* s) const {
            if (s == a) { return b; }
            if (s == b) { return a; }
            return nullptr;
        }

        // s 가 당사자면 상대의 세션 id, 아니면 0(무효 id — session id 는
        //   0 을 안 쓴다). peer_of 와 달리 이 값은 역참조가 아니라 정수
        //   비교라서 상대의 생존 여부와 무관하게 항상 안전하게 읽을 수
        //   있다 — 락 밖에서 상대를 찾는 유일한 진입점은 이 값을 들고
        //   app::EntryTable::find_acquire_by_session_id 를 거치는 것이다.
        uint64_t peer_sid_of(const net::Session* s) const {
            if (s == a) { return b_sid; }
            if (s == b) { return a_sid; }
            return 0;
        }

        // 슬롯이 바뀌면 「양쪽」 확인을 지운다 — 막판 바꿔치기 방어.
        //   상대가 확인을 누른 뒤 내가 슬롯을 바꿔치기하면, 상대는 자기가 본 적 없는
        //   거래에 동의한 것이 된다.
        //   한쪽만 지우면 안 된다. 바꾼 쪽의 확인이 남아도 같은 사고다.
        void reset_confirm() { a_confirm = false; b_confirm = false; }

        bool both_confirmed() const { return a_confirm && b_confirm; }
    };

}   // namespace world
