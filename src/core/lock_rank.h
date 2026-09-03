//  core/lock_rank.h — 락 계층 역순 검사기 (Debug 전용)
//
//  락을 두 개 이상 잡을 때 스레드마다 순서가 다르면 데드락인데, 컴파일러는
//  그 순서를 몰라 잡아 주지 않는다(DESIGN-server-split.md §7-6). 사람이
//  리뷰로 순서를 지키는 방식은 코드가 늘수록 무너지므로, 스레드마다 「지금
//  쥔 락의 계층」을 들고 있다가 그보다 낮거나 같은 계층을 다시 잡으려는
//  순간 assert 로 잡는다.
//
//  NDEBUG 에서 전부 no-op 인 이유 — 이건 개발 중 실수를 잡는 장치이지
//  런타임 방어가 아니다. 남겨 둬도 비용은 비교 한 번뿐이지만, 굳이 Release
//  까지 들고 갈 이유가 없다고 정했다.
#pragma once

#include <cassert>

namespace core {

    enum class LockRank {
        kRoster  = 1,
        kSession = 2,
    };

#ifndef NDEBUG

    inline thread_local int t_lock_rank = 0;

    //  scoped_lock(a, b) 로 같은 계층의 락 두 개를 한 번에 묶을 때는 가드를
    //  하나만 씌운다(그 계층 하나로). 가드를 두 개 쓰면 첫 번째가 이미
    //  t_lock_rank 를 그 계층으로 올려놔서 두 번째가 「같은 계층 재진입」을
    //  역순으로 오판한다 — scoped_lock 짝 획득이 가드 1개인 이유다.
    class LockRankGuard {
    public:
        explicit LockRankGuard(LockRank r) : prev_(t_lock_rank) {
            assert(static_cast<int>(r) > prev_ && "락 계층 역순");
            t_lock_rank = static_cast<int>(r);
        }
        ~LockRankGuard() { t_lock_rank = prev_; }

        LockRankGuard(const LockRankGuard&) = delete;
        LockRankGuard& operator=(const LockRankGuard&) = delete;

    private:
        int prev_;
    };

#else

    class LockRankGuard {
    public:
        explicit LockRankGuard(LockRank) {}
    };

#endif

}   // namespace core
