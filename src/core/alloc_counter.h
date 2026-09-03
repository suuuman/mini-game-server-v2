//  core/alloc_counter.h — 힙 할당 횟수를 센다
//
//  operator new / delete 를 전역에서 가로채 프레임당 할당 횟수를 본다.
//  "여기서 할당이 날 것 같다"는 추측을 숫자로 바꾸는 용도다.
//
//  세는 것은 「횟수」다. delete 는 크기를 모르는 경우가 있어 바이트는
//    new 쪽에서만 센다 — 정확히 셀 수 없는 것을 센 척하지 않는다.
#pragma once

#include <cstdint>

namespace core {

    struct AllocStats {
        uint64_t allocs = 0;    // operator new 호출 횟수
        uint64_t frees = 0;    // operator delete 호출 횟수
        uint64_t bytes = 0;    // new 로 요청된 총 바이트
    };

    AllocStats alloc_snapshot();
    void       alloc_reset();

    // ASan 빌드에서는 false. 가로채면 ASan 감시가 무력해지므로 끈다.
    //   그때는 위 숫자를 믿으면 안 된다.
    bool       alloc_counting();

}   // namespace core
