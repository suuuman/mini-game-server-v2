//  core/alloc_counter.cpp — 전역 operator new/delete 를 갈아끼운다
//
//  헤더가 아니라 .cpp 인 이유 — operator new 는 교체 함수(replaceable function)라
//    표준이 「정확히 하나의 번역 단위」에만 정의하도록 요구한다.
//    헤더에 두면 정의가 여러 곳에 생겨 UB 다. inline 을 붙여도 마찬가지다.
//
//  ASan 빌드에서는 갈아끼우지 않는다. ASan 은 자기 할당자로 힙을 감시해
//    use-after-free 와 오버런을 잡는데, 여기서 가로채면 그 감시가 무력해진다.
//    크래시 탐지 수단을 측정 편의와 바꾸지 않는다.
#include "core/alloc_counter.h"

#include <atomic>
#include <cstdlib>
#include <new>

namespace {
    // atomic<uint64_t> 는 0 으로 상수 초기화된다 — 다른 전역의 생성자가
    //   먼저 new 를 불러도 안전하다. 여기에 std::mutex 같은 「생성자가 있는 것」을
    //   두면 초기화 순서에 걸린다.
    std::atomic<uint64_t> g_allocs{ 0 };
    std::atomic<uint64_t> g_frees{ 0 };
    std::atomic<uint64_t> g_bytes{ 0 };
}

#if !defined(__SANITIZE_ADDRESS__)

void* operator new(std::size_t n) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    g_bytes.fetch_add(n, std::memory_order_relaxed);

    // n == 0 도 유효한 요청이고, 서로 다른 포인터를 돌려줘야 한다.
    void* p = std::malloc(n != 0 ? n : 1);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}

void operator delete(void* p) noexcept {
    if (p != nullptr) {
        g_frees.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

// sized delete — C++14 부터 컴파일러가 이 쪽을 부를 수 있다.
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }

void* operator new[](std::size_t n) { return operator new(n); }
void  operator delete[](void* p) noexcept { operator delete(p); }
void  operator delete[](void* p, std::size_t) noexcept { operator delete(p); }

#endif  // !__SANITIZE_ADDRESS__

namespace core {

    AllocStats alloc_snapshot() {
        AllocStats s;
        s.allocs = g_allocs.load(std::memory_order_relaxed);
        s.frees = g_frees.load(std::memory_order_relaxed);
        s.bytes = g_bytes.load(std::memory_order_relaxed);
        return s;
    }

    void alloc_reset() {
        g_allocs.store(0, std::memory_order_relaxed);
        g_frees.store(0, std::memory_order_relaxed);
        g_bytes.store(0, std::memory_order_relaxed);
    }

    bool alloc_counting() {
#if defined(__SANITIZE_ADDRESS__)
        return false;
#else
        return true;
#endif
    }

}   // namespace core
