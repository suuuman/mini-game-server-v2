// core/buffer_pool.h — 고정 크기 버퍼를 미리 잡아 두고 돌려 쓴다.
//
// new/delete 는 락을 잡고, 자유 리스트를 뒤지고, 단편화를 남긴다. 같은 크기만 쓰면
// 뒤의 둘은 사라지지만 락은 안 사라진다 — acquire/release 도 mutex 를 잡는다.
// 바뀐 것은 범용 할당자의 락이 벡터 pop_back 하나만 감싸는 락이 된 것이지 없앤 게
// 아니다. 그 구분이 흐려지면 나중에 「여긴 락이 없으니까」로 잘못 판단하게 된다.
//
// 저장소는 한 덩어리로 잡는다. 생성 시 한 번 할당하고 그 뒤로는 힙을 안 만진다.
//
// 고갈되면 늘리지도 기다리지도 않고 거절한다. 늘리면 상한이 사라지고, 기다리면
// 빌리러 온 I/O 워커가 그 자리에서 막힌다. 풀 크기가 곧 큐 상한이다.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace core {

    class BufferPool {
    public:
        BufferPool(size_t buffer_size, size_t capacity)
            : buffer_size_(buffer_size), capacity_(capacity) {
            // 한 번의 할당으로 전부. 이후 서버가 도는 동안 여기서 new 가 안 난다.
            storage_ = std::make_unique<char[]>(buffer_size * capacity);

            free_.reserve(capacity);
            for (size_t i = 0; i < capacity; ++i) {
                free_.push_back(storage_.get() + i * buffer_size);
            }
        }

        BufferPool(const BufferPool&) = delete;
        BufferPool& operator=(const BufferPool&) = delete;

        // nullptr = 고갈. 호출자가 거절해야 한다. 여기서 기다리지 않는다.
        char* acquire() {
            std::lock_guard<std::mutex> lock(mutex_);
            if (free_.empty()) {
                ++failed_;
                return nullptr;
            }
            char* p = free_.back();      // 뒤에서 꺼낸다 — 방금 반납된 것이 캐시에 살아 있다
            free_.pop_back();
            ++acquired_;

            const uint64_t used = static_cast<uint64_t>(capacity_ - free_.size());
            if (used > peak_) { peak_ = used; }
            return p;
        }

        void release(char* p) {
            if (p == nullptr) { return; }
            std::lock_guard<std::mutex> lock(mutex_);
            free_.push_back(p);
        }

        size_t buffer_size() const { return buffer_size_; }

        struct Stats {
            uint64_t acquired = 0;   // 빌려준 횟수
            uint64_t failed = 0;   // 고갈로 거절한 횟수. 0 이 아니면 풀이 작다
            uint64_t peak = 0;   // 동시 사용 최대치
            uint64_t capacity = 0;
        };

        Stats stats() const {
            std::lock_guard<std::mutex> lock(mutex_);
            return Stats{ acquired_, failed_, peak_, static_cast<uint64_t>(capacity_) };
        }

    private:
        size_t                  buffer_size_;
        size_t                  capacity_;
        std::unique_ptr<char[]> storage_;
        std::vector<char*>      free_;

        mutable std::mutex mutex_;
        uint64_t           acquired_ = 0;
        uint64_t           failed_ = 0;
        uint64_t           peak_ = 0;
    };

}   // namespace core
