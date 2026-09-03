// core/mpsc_queue.h — Lock-Free MPSC 큐 (Vyukov). 생산자 여럿, 소비자 하나.
//
// MPMC 를 안 쓴 것은 소비자가 존 스레드당 하나라 필요가 없는데, 쓰면 ABA 와 메모리
// 회수(hazard pointer / epoch)가 통째로 따라오기 때문이다. 여기에 ABA 가 없는 것도
// 푼 게 아니라 생길 자리가 없어서다 — push 가 exchange 하나로 끝나 CAS 루프가 없다.
//
// 공짜는 아니다. pop 의 거짓 empty 를 반드시 같이 볼 것.
//
// 본체는 이 파일을 안 쓰고 bench.h 만 쓴다. 죽은 코드가 아니라 대조군이다. 락 큐
// 대비 1.5배가 나왔지만 거짓 empty 때문에 블로킹 소비자를 못 붙이고, Job 큐는 소비자가
// 자야 하므로 못 쓴다. 빠른데 못 쓰는 이유를 코드로 남겨 둔 것이다.
#pragma once

#include <atomic>
#include <utility>

namespace core {

    template <typename T>
    class MpscQueue {
    public:
        MpscQueue() {
            // 더미 노드 하나로 시작한다. 큐가 비어도 head/tail 이 null 이 안 되므로
            // push 와 pop 이 서로의 null 검사를 신경 쓰지 않아도 된다.
            Node* stub = new Node();
            head_.store(stub, std::memory_order_relaxed);
            tail_ = stub;
        }

        ~MpscQueue() {
            T tmp;
            while (pop(tmp)) {}
            delete tail_;
        }

        MpscQueue(const MpscQueue&) = delete;
        MpscQueue& operator=(const MpscQueue&) = delete;

        // 여러 스레드가 동시에 불러도 된다
        void push(T v) {
            Node* n = new Node();
            n->value = std::move(v);

            // exchange 는 무조건 성공한다. 재시도 루프가 없으므로
            // 어느 생산자도 다른 생산자를 기다리지 않는다 (wait-free push).
            Node* prev = head_.exchange(n, std::memory_order_acq_rel);

            // 여기가 「끊긴 순간」이다. prev 는 이미 새 head 를 가리키지 않는데
            //   아직 next 로 이어지지 않았다. 이 찰나에 소비자가 보면 큐가 비어 보인다.
            prev->next.store(n, std::memory_order_release);
        }

        // 소비자는 하나여야 한다. 둘이 부르면 깨진다.
        //   그래서 tail_ 은 atomic 이 아니다 — 만지는 스레드가 하나뿐이다.
        //
        // false 가 「비었다」를 뜻하지 않을 수 있다. 위 push 의 두 줄 사이에
        //   걸린 항목은 여기서 안 보인다. 락 큐라면 "비었다"가 확정인데 이 큐는 아니다.
        //   블로킹 소비자를 붙이려면 그 처리가 따로 필요하다.
        bool pop(T& out) {
            Node* t = tail_;
            Node* next = t->next.load(std::memory_order_acquire);
            if (next == nullptr) {
                return false;
            }
            out = std::move(next->value);
            tail_ = next;
            delete t;                  // 소비자만 지운다 → 회수 문제가 안 생긴다
            return true;
        }

    private:
        struct Node {
            std::atomic<Node*> next{ nullptr };
            T                  value{};
        };

        std::atomic<Node*> head_;      // 생산자들이 밀어 넣는 쪽
        Node* tail_;      // 소비자만 만진다
    };

}   // namespace core
