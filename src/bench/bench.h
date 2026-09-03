// bench/bench.h — 측정 하네스. 반드시 Release 로 잰다.
//
// Debug 는 최적화가 없어 메모리 접근 비용이 잡비용에 묻히고, false sharing 처럼
// 캐시 라인 때문에 느려지는 현상은 거의 안 보인다. ASan 은 더 안 된다 — 모든 접근에
// 검사가 붙어 측정 대상 자체가 바뀐다.
//
// 이 프로젝트에서 두 번 겪었다. ASan 빌드로 누수를 재서 오진했고, 부하 주입 스위치를
// 켠 채로 속도를 재서 또 오진했다. 무엇으로 재는가가 무엇을 재는가만큼 중요하다.
//
// 별도 exe 를 안 만든 것은 구성이 하나 늘면 빌드 설정을 두 벌 관리해야 해서다.
#pragma once

#include "core/log.h"
#include "core/mpsc_queue.h"
#include "core/alloc_counter.h"

#include <functional>
#include <chrono>
#include <condition_variable>   // FakePool
#include <cstdint>
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>
#include <queue>
#include <shared_mutex>
#include <unordered_map>

namespace bench {

    // steady_clock 이다. system_clock 은 시스템 시각이 조정되면 뒤로 갈 수 있어서
    //   경과 시간 측정에 쓰면 안 된다. 「시각」과 「경과」는 다른 시계를 쓴다.
    using Clock = std::chrono::steady_clock;

    struct Result {
        const char* name = "";
        double      ms = 0.0;
        uint64_t    ops = 0;

        double ops_per_sec() const {
            return (ms > 0.0) ? (static_cast<double>(ops) / (ms / 1000.0)) : 0.0;
        }
    };

    //  컴파일러가 루프를 통째로 지우는 것을 막는다.
    //    결과를 아무도 안 쓰면 「그 계산은 없어도 된다」고 판단해서 삭제한다.
    //    그러면 0ms 가 나오고, 그걸 「엄청 빠르다」로 오독하게 된다.
    //    여기에 값을 흘려 두면 컴파일러가 지우지 못한다.
    inline volatile uint64_t g_sink = 0;

    inline void keep(uint64_t v) {
        g_sink = g_sink + v;
    }

    //  한 번 재기.
    //
    //  측정 구간 안에서는 로그를 찍지 않는다.
    //    우리 로거는 매 줄 락 + fflush 라, 찍는 순간 그게 측정 대상이 되어 버린다.
    template <typename Fn>
    Result once(const char* name, uint64_t ops, Fn&& fn) {
        const auto t0 = Clock::now();
        fn();
        const auto t1 = Clock::now();

        Result r;
        r.name = name;
        r.ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        r.ops = ops;
        return r;
    }

    //  워밍업 1회 + 본 측정 N회 중 「최솟값」.
    //
    //  왜 평균이 아니라 최솟값인가 —
    //    측정을 방해하는 요인(다른 프로세스, 인터럽트, 페이지 폴트)은 전부
    //    「느려지는」 쪽으로만 작용한다. 빨라지는 노이즈는 없다.
    //    그래서 최솟값이 「방해가 가장 적었던 실행」이고 실제 성능에 가장 가깝다.
    //    평균을 쓰면 우연히 낀 한 번의 스파이크가 결과를 끌고 간다.
    //
    //  워밍업이 필요한 이유 — 첫 실행은 페이지 폴트와 차가운 캐시를 함께 잰다.
    //    그건 이 코드의 성능이 아니라 「처음 실행되는 비용」이다.
    template <typename Fn>
    Result best_of(const char* name, uint64_t ops, int rounds, Fn&& fn) {
        once(name, ops, fn);                       // 워밍업 — 결과를 버린다

        Result best = once(name, ops, fn);
        for (int i = 1; i < rounds; ++i) {
            const Result r = once(name, ops, fn);
            if (r.ms < best.ms) {
                best = r;
            }
        }
        return best;
    }

    inline void report(const Result& r) {
        core::logf("  %-28s %8.2f ms   %10.2f M ops/s\n",
            r.name, r.ms, r.ops_per_sec() / 1'000'000.0);
    }

    // 줄 수가 서로 다른 측정을 비교하려면 「건당 비용」이어야 한다.
    //   로거 분해 측정은 콘솔 단계만 줄 수를 줄여야 해서(수만 줄을 쏟을 수 없다)
    //   ms 끼리 비교하면 안 된다.
    inline void report_per_op(const Result& r) {
        const double ns = (r.ops > 0)
            ? (r.ms * 1'000'000.0 / static_cast<double>(r.ops)) : 0.0;
        core::logf("  %-30s %8.2f ms  %9.0f ns/line   (%llu 줄)\n",
            r.name, r.ms, ns, static_cast<unsigned long long>(r.ops));
    }

    // 두 측정의 「건당 비용」 차이 — 그게 그 단계가 새로 추가한 몫이다
    inline void report_delta(const Result& base, const Result& with, const char* what) {
        if (base.ops == 0 || with.ops == 0) { return; }
        const double b = base.ms * 1'000'000.0 / static_cast<double>(base.ops);
        const double w = with.ms * 1'000'000.0 / static_cast<double>(with.ops);
        core::logf("     → %-22s +%8.0f ns/line   (%.2f 배)\n",
            what, w - b, (b > 0.0) ? (w / b) : 0.0);
    }

    // 어느 쪽이 느리든 자연스럽게 읽히도록 방향을 맞춰 출력한다
    inline void report_ratio(const Result& a, const Result& b) {
        if (a.ms <= 0.0 || b.ms <= 0.0) { return; }
        if (a.ms >= b.ms) {
            core::logf("  → %s 가 %s 보다 %.2f 배 느리다\n", a.name, b.name, a.ms / b.ms);
        }
        else {
            core::logf("  → %s 가 %s 보다 %.2f 배 느리다\n", b.name, a.name, b.ms / a.ms);
        }
    }

    //  False Sharing
    //
    //  스레드마다 「자기 카운터」만 두드린다. 공유하는 변수가 하나도 없다.
    //  그런데도 느리다 — 캐시 일관성은 「변수 단위」가 아니라 「캐시 라인 단위」라서다.
    //
    //  한 코어가 자기 카운터에 쓰면, 같은 라인에 있는 남의 카운터까지
    //  다른 코어의 캐시에서 무효화된다. 논리적으로는 독립인데 하드웨어에서는 공유다.
    //
    //  이름이 「거짓 공유」인 이유가 이것이다. 공유한 적이 없는데 공유 비용을 낸다.
    inline constexpr size_t kCacheLine = 64;      // x86-64 의 캐시 라인 크기

    // 나쁜 배치 — 8B 짜리 8개가 64B 한 라인에 통째로 들어간다
    struct PackedCounters {
        std::atomic<uint64_t> c[8];
    };

    // 좋은 배치 — 하나씩 자기 라인을 차지한다
    //
    // C4324 는 「alignas 때문에 구조체에 패딩이 들어갔다」는 정보성 경고다.
    //   그 패딩이 바로 우리가 원한 것이라 여기서만 끈다.
    //   전역으로 끄지 않는 이유 — 그러면 「의도치 않은 패딩」까지 같이 숨는다.
    //     경고를 끌 때는 범위를 최대한 좁히고, 왜 껐는지를 옆에 적어 둔다.
#pragma warning(push)
#pragma warning(disable: 4324)
    struct alignas(kCacheLine) PaddedCell {
        std::atomic<uint64_t> c;
        // 패딩을 직접 안 써도 alignas 가 구조체 크기를 64 의 배수로 올린다.
        //   배열로 만들면 원소마다 64B 간격이 생긴다 — 그게 목적이다.
    };
#pragma warning(pop)

    template <typename Access>
    void hammer(unsigned threads, uint64_t iters, Access&& access) {
        std::vector<std::thread> ts;
        ts.reserve(threads);
        for (unsigned t = 0; t < threads; ++t) {
            ts.emplace_back([t, iters, &access] {
                for (uint64_t i = 0; i < iters; ++i) {
                    // relaxed 다. 순서를 보장할 필요가 없고,
                    //   여기서 재려는 건 「메모리 순서 비용」이 아니라 「캐시 라인 경합」이다.
                    //   seq_cst 로 두면 그 비용이 섞여서 무엇을 쟀는지 흐려진다.
                    access(t).fetch_add(1, std::memory_order_relaxed);
                }
                });
        }
        for (auto& th : ts) { th.join(); }
    }

    inline void bench_false_sharing() {
        const unsigned threads = 8;
        const uint64_t iters = 5'000'000;
        const uint64_t ops = static_cast<uint64_t>(threads) * iters;

        core::logf("[False Sharing]  스레드 %u개 × %llu회\n",
            threads, static_cast<unsigned long long>(iters));

        PackedCounters packed{};
        std::vector<PaddedCell> padded(threads);

        const Result bad = best_of("packed (한 라인에)", ops, 3, [&] {
            hammer(threads, iters, [&](unsigned t) -> std::atomic<uint64_t>&{
                return packed.c[t];
                });
            });

        const Result good = best_of("padded (라인마다 하나)", ops, 3, [&] {
            hammer(threads, iters, [&](unsigned t) -> std::atomic<uint64_t>&{
                return padded[t].c;
                });
            });

        // 결과를 흘려 둬야 컴파일러가 루프를 안 지운다
        keep(packed.c[0].load() + padded[0].c.load());

        report(bad);
        report(good);
        report_ratio(bad, good);
        core::logf("  (sizeof PackedCounters=%zu  PaddedCell=%zu)\n\n",
            sizeof(PackedCounters), sizeof(PaddedCell));
    }

    //  락 큐 vs Lock-Free 큐
    //
    //  JobQueue 를 직접 쓰지 않고 같은 구조의 최소 큐를 따로 만든 이유 —
    //    JobQueue 는 std::function 을 담는다. 그 힙 할당 비용이 섞이면
    //    「락 때문에 느린 건지 function 때문에 느린 건지」를 구별할 수 없다.
    //    비교에서는 다른 조건을 전부 같게 만들어야 한다.
    //
    //  조건변수를 안 쓰고 스핀으로 받는 것도 같은 이유다.
    //    블로킹 대기 비용이 섞이면 「큐 자체의 비용」이 흐려진다.
    struct LockedQueue {
        std::mutex           m;
        std::queue<uint64_t> q;

        void push(uint64_t v) {
            std::lock_guard<std::mutex> lock(m);
            q.push(v);
        }
        bool pop(uint64_t& out) {
            std::lock_guard<std::mutex> lock(m);
            if (q.empty()) { return false; }
            out = q.front();
            q.pop();
            return true;
        }
    };

    template <typename Q>
    void drain_test(Q& q, unsigned producers, uint64_t per_producer) {
        const uint64_t total = static_cast<uint64_t>(producers) * per_producer;

        std::vector<std::thread> ts;
        ts.reserve(producers);
        for (unsigned p = 0; p < producers; ++p) {
            ts.emplace_back([&q, per_producer] {
                for (uint64_t i = 0; i < per_producer; ++i) {
                    q.push(i);
                }
                });
        }

        // 소비자는 이 스레드 하나. MPSC 의 전제이기도 하다.
        uint64_t got = 0;
        uint64_t v = 0;
        while (got < total) {
            if (q.pop(v)) {
                ++got;
                keep(v);
            }
            else {
                std::this_thread::yield();   // 비었으면 양보. 바쁜 대기로 코어를 태우지 않는다
            }
        }

        for (auto& t : ts) { t.join(); }
    }

    inline void bench_queue() {
        const unsigned producers = 4;
        const uint64_t per = 200'000;
        const uint64_t ops = static_cast<uint64_t>(producers) * per;

        core::logf("[Queue]  생산자 %u개 × %llu개  (소비자 1)\n",
            producers, static_cast<unsigned long long>(per));

        const Result locked = best_of("mutex + std::queue", ops, 3, [&] {
            LockedQueue q;
            drain_test(q, producers, per);
            });

        const Result lockfree = best_of("lock-free MPSC", ops, 3, [&] {
            core::MpscQueue<uint64_t> q;
            drain_test(q, producers, per);
            });

        report(locked);
        report(lockfree);
        report_ratio(locked, lockfree);
        core::logf("\n");
    }

    // op(is_read) 를 스레드 N개가 iters 번씩 부른다.
    // 난수를 안 쓰고 i % 100 으로 나누는 이유 — 매번 같은 순서여야 재현이 된다.
    //   측정에서 재현 안 되는 요소는 하나라도 줄이는 게 낫다.
    template <typename Fn>
    void mixed_run(unsigned threads, uint64_t iters, int read_pct, Fn&& op) {
        std::vector<std::thread> ts;
        ts.reserve(threads);
        for (unsigned t = 0; t < threads; ++t) {
            ts.emplace_back([&op, iters, read_pct] {
                for (uint64_t i = 0; i < iters; ++i) {
                    op((i % 100) < static_cast<uint64_t>(read_pct));
                }
                });
        }
        for (auto& th : ts) { th.join(); }
    }

    //  mutex vs shared_mutex — 「읽기 비율이 얼마나 돼야 이득인가」
    //
    //  shared_mutex 가 주는 것은 하나뿐이다 — 읽기끼리는 서로 안 막는다.
    //    대신 자료구조가 더 무겁다(읽는 사람 수를 따로 관리해야 하니까).
    //    그래서 쓰기가 섞이면 오히려 손해가 날 수 있다.
    //
    //  우리 세션 목록에 안 쓴 이유가 여기서 숫자로 나온다 —
    //    거기엔 읽기 전용 경로가 「하나도」 없다. 추가·삭제·종료뿐이고,
    //    삭제는 조건을 확인한 뒤 지우는 거라 어차피 배타 락이 필요하다.
    //    즉 우리는 이 그래프의 왼쪽 끝(읽기 0%)에 있다.
    inline void bench_rwlock() {
        const unsigned threads = 8;
        const uint64_t iters = 100'000;
        const uint64_t ops = static_cast<uint64_t>(threads) * iters;

        // 읽기 「비율」이 아니라 읽기 「구간 길이」를 바꿔가며 본다.
        //   앞선 측정에서 읽기 비율을 99%까지 올려도 shared_mutex 가 더 느렸다.
        //   읽기 구간이 짧으면 락을 잡고 푸는 비용이 실제 일보다 크고,
        //   shared_mutex 는 읽는 사람 수를 원자적으로 세느라 그 비용이 더 크기 때문이다.
        //   → 「읽기가 많을 때」가 아니라 「읽기가 오래 걸릴 때」 이득이라는 가설을 확인한다.
        const int read_pct = 90;

        for (int span : { 64, 4096 }) {
            std::vector<int> data(static_cast<size_t>(span), 1);

            core::logf("[RWLock]  읽기 %d%%  · 읽기 구간 %d개  · 스레드 %u개 × %llu회\n",
                read_pct, span, threads, static_cast<unsigned long long>(iters));

            std::mutex m;
            const Result ex = best_of("std::mutex", ops, 3, [&] {
                mixed_run(threads, iters, read_pct, [&](bool is_read) {
                    std::lock_guard<std::mutex> lock(m);
                    if (is_read) {
                        uint64_t s = 0;
                        for (int v : data) { s += static_cast<uint64_t>(v); }
                        keep(s);
                    }
                    else {
                        data[0] += 1;
                    }
                    });
                });

            std::shared_mutex sm;
            const Result sh = best_of("std::shared_mutex", ops, 3, [&] {
                mixed_run(threads, iters, read_pct, [&](bool is_read) {
                    if (is_read) {
                        // 읽기 락 — 여러 스레드가 동시에 들어온다
                        std::shared_lock<std::shared_mutex> lock(sm);
                        uint64_t s = 0;
                        for (int v : data) { s += static_cast<uint64_t>(v); }
                        keep(s);
                    }
                    else {
                        // 쓰기 락 — 혼자만 들어온다. 읽는 사람이 다 나갈 때까지 기다린다
                        std::unique_lock<std::shared_mutex> lock(sm);
                        data[0] += 1;
                    }
                    });
                });

            report(ex);
            report(sh);
            report_ratio(ex, sh);
            core::logf("\n");
        }
    }

    //  명부 스냅샷형 임계구역 — app::EntryTable::snapshot_zone/snapshot_all 의
    //  재현이다. 「락 → 전체 순회 + 원소당 atomic 증가(acquire_hold 흉내) +
    //  vector push_back(반출 흉내) → 해제」를 한 임계구역 안에서 한다.
    //
    //  위 bench_rwlock() 과 조건이 다르다 — 거기는 읽기 구간 길이를 64/4096
    //  「원소」로 바꿔가며 봤지만 임계구역 자체는 "찾고 즉시 놓기"(그때의
    //  U3 실측 0.93~1.01x 가 그 조건이다)였다. 여기는 원소 「전부」를 훑는
    //  것 자체가 임계구역이라 읽기 구간이 N 에 정비례로 늘어난다 — U3 값을
    //  여기 다시 쓰면 「안 잰 것을 잰 것처럼」이 된다.
    //
    //  N 은 접속 상한(kZoneMembersNtf 주석의 511명)을 감싸는 100/1,000/4,096
    //  세 점을 본다. 스레드를 여럿 띄우지 않는다 — 여기서 보려는 것은
    //  「동시 경합이 있을 때 얼마나 이득인가」가 아니라 「이 임계구역 자체가
    //  얼마나 오래 걸리는가」다(D5 판정 기준이 임계구역 길이 자체를 본다).
    //  회당 μs 로 출력한다 — 채팅 한 번마다 도는 비용이라 ms 단위로는 안 보인다.
    inline void bench_roster_snapshot() {
        const uint64_t rounds = 2000;

        for (uint64_t n : { 100ull, 1000ull, 4096ull }) {
            std::unordered_map<uint64_t, uint64_t> table;
            table.reserve(static_cast<size_t>(n));
            for (uint64_t i = 0; i < n; ++i) {
                table[i] = i;
            }

            core::logf("[Roster Snapshot]  N=%llu  회 %llu\n",
                static_cast<unsigned long long>(n),
                static_cast<unsigned long long>(rounds));

            // acquire_hold 흉내 — 실제로는 세션마다의 io_count 지만, 여기서는
            //   임계구역 안에서 도는 원자 연산 자체의 비용만 보면 되므로 카운터
            //   하나로 합친다(결과를 sink 로도 흘려 컴파일러가 안 지운다).
            std::atomic<uint64_t> holds{ 0 };

            std::mutex m;
            const Result ex = best_of("std::mutex", rounds, 3, [&] {
                for (uint64_t r = 0; r < rounds; ++r) {
                    std::lock_guard<std::mutex> lock(m);
                    std::vector<uint64_t> out;
                    out.reserve(table.size());
                    for (const auto& [k, v] : table) {
                        holds.fetch_add(1, std::memory_order_relaxed);
                        out.push_back(v);
                    }
                    keep(out.size());
                }
                });

            std::shared_mutex sm;
            const Result sh = best_of("std::shared_mutex(shared)", rounds, 3, [&] {
                for (uint64_t r = 0; r < rounds; ++r) {
                    // 스냅샷은 자료구조를 안 바꾸는 「읽기」다 — 실제로 A11 후보였던
                    //   이유가 이것이다. shared_lock 으로 재서 그 가정에서도
                    //   이득이 없는지를 본다.
                    std::shared_lock<std::shared_mutex> lock(sm);
                    std::vector<uint64_t> out;
                    out.reserve(table.size());
                    for (const auto& [k, v] : table) {
                        holds.fetch_add(1, std::memory_order_relaxed);
                        out.push_back(v);
                    }
                    keep(out.size());
                }
                });

            keep(holds.load(std::memory_order_relaxed));

            // report()/report_per_op() 은 ms·ns 단위라 이 임계구역의 자릿수(μs)에
            //   안 맞는다 — 여기서 직접 회당 μs 로 바꿔 찍는다.
            const double ex_us = (ex.ops > 0)
                ? (ex.ms * 1000.0 / static_cast<double>(ex.ops)) : 0.0;
            const double sh_us = (sh.ops > 0)
                ? (sh.ms * 1000.0 / static_cast<double>(sh.ops)) : 0.0;
            core::logf("  %-28s %10.3f us/round\n", "std::mutex", ex_us);
            core::logf("  %-28s %10.3f us/round\n", "std::shared_mutex(shared)", sh_us);
            report_ratio(ex, sh);
            core::logf("\n");
        }
    }

    // std::function 이 정말 힙을 쓰는가. 「붙을 수 있다」는 흔한 통념인데, 있을 수
    // 있다와 있다는 다르다. 대부분의 구현은 작은 캡처를 객체 내부 버퍼에 담고(SBO)
    // 그 경계를 넘어야 힙으로 간다. 그 경계가 어디인지 여기서 센다.
    //
    // 우리 Job 의 캡처는 40B 다. 한때 주석에 「vector(24) 포함 40B」로 적혀 있었는데,
    // vector 는 이미 풀 버퍼로 바뀌었고 그 뒤에 placement 가 들어와 우연히 다시 40B 가
    // 됐다. 숫자만 같고 구성은 전부 달랐다 — 맞아 보이는 주석이 제일 오래 안 고쳐진다.
    //
    // 한때 이 표에 40B 와 64B 사이가 비어 있는데도 「경계가 64B」라고 적어 뒀다.
    // 잰 것보다 강한 말이었다. 그래서 48B 를 넣어 다시 쟀고, 48B 0회 64B 1회로
    // 우리 Job 의 여유가 8B 임을 확인했다.
    //
    // sizeof(std::function)=64B 는 그릇 크기지 담기는 캡처 한계가 아니다. 타입 소거용
    // vptr 몫이 먼저 빠지므로, 둘을 같은 것으로 읽으면 여유를 세 배로 잘못 센다.
    template <typename Cap>
    void probe_capture(const char* label) {
        Cap c{};

        // 리셋과 스냅샷 사이에는 아무것도 하지 않는다.
        //   로그 한 줄만 끼어도 그 할당이 같이 세어진다.
        core::alloc_reset();
        {
            std::function<void()> f = [c]() { keep(static_cast<uint64_t>(c.b[0])); };
            f();
        }
        const core::AllocStats s = core::alloc_snapshot();

        core::logf("  %-16s 캡처 %3zu B → 할당 %llu 회\n",
            label, sizeof(Cap), static_cast<unsigned long long>(s.allocs));
    }

    inline void bench_alloc_probe() {
        core::logf("[Alloc]  카운터 %s\n",
            core::alloc_counting() ? "활성" : "★비활성 (ASan 빌드 — 숫자를 믿지 말 것)");

        if (!core::alloc_counting()) {
            core::logf("\n");
            return;
        }

        core::logf("  sizeof(std::function<void()>) = %zu B\n",
            sizeof(std::function<void()>));

        struct Cap8 { char b[8]; };
        struct Cap16 { char b[16]; };
        struct Cap24 { char b[24]; };
        struct Cap40 { char b[40]; };   // 우리 Job 의 캡처 크기 (drain_frames)
        struct Cap48 { char b[48]; };   // 40 과 64 사이의 유일한 점 — 여유를 보려면 필요하다
        struct Cap64 { char b[64]; };
        struct Cap128 { char b[128]; };

        probe_capture<Cap8>("Cap8");
        probe_capture<Cap16>("Cap16");
        probe_capture<Cap24>("Cap24");
        probe_capture<Cap40>("Cap40  ← 우리 Job");
        probe_capture<Cap48>("Cap48");
        probe_capture<Cap64>("Cap64");
        probe_capture<Cap128>("Cap128");

        // vector 는 어떤가 — 프레임 복사가 여기에 해당한다
        core::alloc_reset();
        {
            std::vector<char> v(12);      // 12바이트 프레임 하나
            keep(v.size());
        }
        const core::AllocStats vs = core::alloc_snapshot();
        core::logf("  %-16s              → 할당 %llu 회\n",
            "vector<char>(12)", static_cast<unsigned long long>(vs.allocs));

        core::logf("\n");
    }

    // 고치기 전에 무엇이 느린지 분해한다. emit() 한 줄에 후보가 넷이라, 하나씩 쌓아
    // 가며 재면 각자의 몫이 나온다 — 포맷, 락, 파일 쓰기, flush, 콘솔.
    //
    // 고치고 나서 재면 「좋아졌다」밖에 못 말한다. 이 프로젝트에서 추측이 틀린 적이
    // 이미 있어서, 재기 전에는 원인을 말하지 않는다.
    //
    // 콘솔 단계만 줄 수를 크게 줄인다. 같은 줄 수로 재면 수만 줄이 쏟아져 벤치 결과가
    // 화면에서 밀려난다. 그래서 비교는 ns/line 으로 한다.
    enum class LogStage {
        kFormatOnly = 0,
        kLock,
        kFile,
        kFileFlush,
        kFileFlushConsole,
    };

    inline void log_line_sim(LogStage stage, std::mutex& m, std::FILE* f, uint64_t seq) {
        // 실제 emit() 과 같은 모양으로 만든다. GetLocalTime 도 매 줄 부르고 있으니
        //   그 비용까지 포함해야 「지금 로거」를 잰 것이 된다.
        SYSTEMTIME t;
        GetLocalTime(&t);

        char line[1200];
        const int n = std::snprintf(line, sizeof(line),
            "%02u:%02u:%02u.%03u [t%-5lu] bench line %llu\n",
            static_cast<unsigned>(t.wHour), static_cast<unsigned>(t.wMinute),
            static_cast<unsigned>(t.wSecond), static_cast<unsigned>(t.wMilliseconds),
            GetCurrentThreadId(), static_cast<unsigned long long>(seq));
        keep(static_cast<uint64_t>(n));

        if (stage == LogStage::kFormatOnly) {
            return;
        }

        std::lock_guard<std::mutex> lock(m);
        if (stage == LogStage::kLock) {
            return;
        }

        if (f != nullptr) {
            std::fputs(line, f);
            if (stage >= LogStage::kFileFlush) {
                std::fflush(f);
            }
        }
        if (stage == LogStage::kFileFlushConsole) {
            std::fputs(line, stdout);
        }
    }

    inline Result bench_log_stage(const char* name, LogStage stage,
        unsigned threads, uint64_t per_thread, std::FILE* f) {
        std::mutex m;
        return best_of(name, static_cast<uint64_t>(threads) * per_thread, 3, [&] {
            std::vector<std::thread> ts;
            ts.reserve(threads);
            for (unsigned i = 0; i < threads; ++i) {
                ts.emplace_back([&] {
                    for (uint64_t k = 0; k < per_thread; ++k) {
                        log_line_sim(stage, m, f, k);
                    }
                    });
            }
            for (std::thread& t : ts) {
                t.join();
            }
            });
    }

    inline void bench_logger() {
        core::logf("── 동기 로거 분해 — 지금 emit() 의 비용은 어디서 나는가 ──\n");

        // 실제 서버에서 로그를 쓰는 스레드 수에 맞춘다:
        //   I/O 워커 4 + 존 스레드 4 + DB 워커 4. 락 경합을 그 조건에서 봐야 한다.
        constexpr unsigned kThreads = 12;
        constexpr uint64_t kPerThread = 3000;
        constexpr uint64_t kPerThreadConsole = 150;   // 콘솔은 1/20 로 줄인다

        CreateDirectoryA("logs", nullptr);
        std::FILE* f = std::fopen("logs\\bench_sync.log", "w");
        if (f == nullptr) {
            core::logf("  ! logs\\bench_sync.log 를 못 열었다 — 파일 단계를 건너뛴다\n\n");
            return;
        }

        const Result r1 = bench_log_stage("[1] 포맷만", LogStage::kFormatOnly, kThreads, kPerThread, f);
        const Result r2 = bench_log_stage("[2] + 락", LogStage::kLock, kThreads, kPerThread, f);
        const Result r3 = bench_log_stage("[3] + 파일", LogStage::kFile, kThreads, kPerThread, f);
        const Result r4 = bench_log_stage("[4] + flush", LogStage::kFileFlush, kThreads, kPerThread, f);
        const Result r5 = bench_log_stage("[5] + 콘솔 = 지금 로거", LogStage::kFileFlushConsole,
            kThreads, kPerThreadConsole, f);

        std::fclose(f);
        DeleteFileA("logs\\bench_sync.log");

        core::logf("\n");
        report_per_op(r1);
        report_per_op(r2);  report_delta(r1, r2, "락");
        report_per_op(r3);  report_delta(r2, r3, "파일 쓰기");
        report_per_op(r4);  report_delta(r3, r4, "매 줄 flush ★");
        report_per_op(r5);  report_delta(r4, r5, "콘솔 ★");

        core::logf("\n  ★ 스레드 %u개 · [1]~[4] 는 %llu줄/스레드 · [5] 는 %llu줄/스레드\n",
            kThreads,
            static_cast<unsigned long long>(kPerThread),
            static_cast<unsigned long long>(kPerThreadConsole));
        core::logf("  ⚠️ 콘솔 수치는 리다이렉트 여부에 크게 좌우된다 — 조건을 함께 적을 것\n");
        core::logf("\n");
    }

    // 지금 로거를 그대로 잰다. 위 bench_logger() 는 동기 로거를 흉내 낸 것이고 이건
    // 실제 core::logf 다. 보는 것은 둘이다.
    //   게임 스레드 관점의 비용. 큐에 넣기만 하므로 수십 ns 여야 한다
    //   드롭 경로가 실제로 도는가. 생산이 소비를 앞지르면 유실이 나야 한다
    //
    // 두 번째는 실제 네트워크 부하로 재현이 안 됐다 — 프레임 풀이 먼저 고갈됐고 로그
    // 유실은 0이었다. 방어는 되지만 일상은 아니라는 뜻이라, 여기서 인위적으로
    // 앞지르게 만들어 경로가 살아 있는지 확인한다.
    //
    // 측정이 끝나고 log_close() 가 남은 줄을 쓰는 시간은 여기 안 들어간다. 게임
    // 스레드가 문 비용만 재는 것이 목적이다.
    inline Result logf_burst(const char* name, unsigned threads, uint64_t per_thread) {
        return once(name, static_cast<uint64_t>(threads) * per_thread, [&] {
            std::vector<std::thread> ts;
            ts.reserve(threads);
            for (unsigned i = 0; i < threads; ++i) {
                ts.emplace_back([&] {
                    for (uint64_t k = 0; k < per_thread; ++k) {
                        core::logf("[BENCH] async logger line %llu\n",
                            static_cast<unsigned long long>(k));
                    }
                    });
            }
            for (std::thread& t : ts) {
                t.join();
            }
            });
    }

    //  드롭 정책이 레벨별로 지켜지는가
    //
    //  기본은 「아무것도 안 버린다」다. 로그 때문에 큐가 가득 찬다면 그건 로거가
    //  아니라 설정의 문제이고, 조용히 버려서 덮으면 원인이 안 보이기 때문이다.
    //  → 여기서는 일부러 상한을 작게 잡아 정책이 실제로 도는지만 확인한다.
    //
    //  무엇을 보는가 — drop_level=info 로 두고 INFO 와 WARN 을 섞어 넣는다.
    //    INFO 는 줄고 WARN 은 「한 줄도 안 줄어야」 한다.
    //    세는 것은 이 함수가 아니라 로그 파일이다 (scripts 에서 grep).
    inline void bench_log_drop_policy() {
        constexpr unsigned kThreads = 8;
        constexpr uint64_t kInfoPer = 2000;      // 8 x 2000 = 16,000 줄
        constexpr uint64_t kWarnPer = 50;        // 8 x   50 =    400 줄
        constexpr size_t   kSmallLimit = 512;    // 일부러 작게 — 드롭을 유도한다

        core::log_flush();
        core::log_set_policy(kSmallLimit, core::DropLevel::kInfo);

        const uint64_t d0 = core::log_dropped_total();
        const uint64_t f0 = core::log_dropped_forced();

        {
            std::vector<std::thread> ts;
            ts.reserve(kThreads);
            for (unsigned i = 0; i < kThreads; ++i) {
                ts.emplace_back([&] {
                    for (uint64_t k = 0; k < kInfoPer; ++k) {
                        core::logf("[DROPTEST-INFO] line %llu\n",
                            static_cast<unsigned long long>(k));
                        // INFO 40줄마다 WARN 하나 — 큐가 가득 찬 구간에 걸치게 한다
                        if ((k % (kInfoPer / kWarnPer)) == 0) {
                            core::logf("[WARN ] DROPTEST-WARN line %llu\n",
                                static_cast<unsigned long long>(k));
                        }
                    }
                    });
            }
            for (std::thread& t : ts) {
                t.join();
            }
        }

        const uint64_t dropped = core::log_dropped_total() - d0;
        const uint64_t forced = core::log_dropped_forced() - f0;

        // 정책을 되돌린 뒤에 결과를 찍는다 — 안 그러면 결과 줄이 버려진다
        core::log_set_policy(core::Log::kDefaultQueueLimit, core::DropLevel::kNone);
        core::log_flush();

        core::logf("\n── 드롭 정책 — 레벨별로 지켜지는가 ──\n\n");
        core::logf("  설정      drop_level=info · queue_limit=%zu (일부러 작게)\n", kSmallLimit);
        core::logf("  넣은 것   INFO %llu 줄 · WARN %llu 줄\n",
            static_cast<unsigned long long>(kThreads * kInfoPer),
            static_cast<unsigned long long>(kThreads * kWarnPer));
        core::logf("  버린 것   %llu 줄  (그중 하드 상한 강제 %llu 줄)\n",
            static_cast<unsigned long long>(dropped),
            static_cast<unsigned long long>(forced));
        core::logf("  ★ 판정은 로그 파일에서 한다 —\n");
        core::logf("    DROPTEST-WARN 이 %llu 줄 전부 남아 있으면 정책이 지켜진 것이다\n",
            static_cast<unsigned long long>(kThreads * kWarnPer));
        core::logf("  ⚠️ 강제 유실이 0이 아니면 WARN 도 일부 잃었다는 뜻이다\n\n");
    }

    inline void bench_async_logger() {
        constexpr unsigned kThreads = 12;
        constexpr uint64_t kPerThread = 4000;
        constexpr uint64_t kTotal = static_cast<uint64_t>(kThreads) * kPerThread;

        // 기본 정책(아무것도 안 버림)으로 잰다. 그래야 「게임 스레드가 무는 비용」이
        //   버리기로 절약된 값이 아니라 실제로 다 넣은 값이 된다.
        core::log_set_policy(core::Log::kDefaultQueueLimit, core::DropLevel::kNone);

        // ── 콘솔 켠 채로 ────────────────────────────────────────────
        core::log_flush();                       // 앞 벤치가 남긴 것을 비우고 시작
        const uint64_t d0 = core::log_dropped_total();
        const Result on = logf_burst("core::logf  콘솔 켬", kThreads, kPerThread);
        const uint64_t drop_on = core::log_dropped_total() - d0;

        // 여기서 flush 하는 이유 — 방금 4만 8천 줄을 밀어 넣어 큐가 가득 차 있다.
        //   그대로 결과를 찍으면 결과 줄 자체가 드롭된다. 실제로 처음에 그렇게 잃었다.
        //   「측정 결과를 잃는 측정」은 의미가 없다.
        //   결과를 찍기 전에 log_flush() — 큐가 가득 찬 상태면 결과 줄이 유실된다.
        core::log_flush();

        // ── 콘솔 끄고 ──────────────────────────────────────────────
        core::log_set_console(false);
        const uint64_t d1 = core::log_dropped_total();
        const Result off = logf_burst("core::logf  콘솔 끔", kThreads, kPerThread);
        const uint64_t drop_off = core::log_dropped_total() - d1;
        core::log_flush();
        core::log_set_console(true);

        core::logf("\n── 비동기 로거 — 게임 스레드가 무는 비용 ──\n\n");
        report_per_op(on);
        core::logf("     → 유실 %llu / %llu 줄  (%.1f%%)\n",
            static_cast<unsigned long long>(drop_on),
            static_cast<unsigned long long>(kTotal),
            100.0 * static_cast<double>(drop_on) / static_cast<double>(kTotal));
        report_per_op(off);
        core::logf("     → 유실 %llu / %llu 줄  (%.1f%%)\n",
            static_cast<unsigned long long>(drop_off),
            static_cast<unsigned long long>(kTotal),
            100.0 * static_cast<double>(drop_off) / static_cast<double>(kTotal));

        core::logf("\n  ★ 동기 로거였다면 이 자리가 30,600~50,300 ns/line 이었다 (콘솔 포함)\n");
        core::logf("  ★ 게임 스레드는 「넣기만」 하므로 빨라졌지만, 로거 스레드는 여전히\n");
        core::logf("    콘솔에 묶인다 — 그 차이가 위 두 유실률로 나온다\n");
        core::logf("  ⚠️ 유실은 「방어가 동작한다」는 뜻이다. 실제 네트워크 부하에서는\n");
        core::logf("    프레임 풀이 먼저 고갈됐고 로그 유실은 0이었다\n\n");
    }

    // 커넥션 풀의 관리 비용만 잰다. 실부하로는 못 잰다 — 오버헤드가 수백 ns 인데
    // 질의 하나가 90μs 라 0.2% 도 안 되어 노이즈에 묻힌다.
    //
    // 실제 DbPool 이 아니라 같은 연산 구조를 흉내 낸 것이다. 진짜를 재려면 DB 연결이
    // 필요하고 그러면 연결 비용이 지배해 acquire/release 가 안 보인다. 절대값보다
    // 자릿수를 본다 — 질의 90μs 와 비교할 수 있으면 충분하다.
    //
    // 비교 대상인 전용 연결은 job 당 비용이 0 이다. 워커가 시작할 때 한 번 잡고 끝이라,
    // 여기서 나온 값이 곧 풀로 바꿔서 늘어난 비용이다.
    struct FakePool {
        std::mutex                m;
        std::condition_variable   cv;
        std::vector<int>          idle;

        int acquire() {
            std::unique_lock<std::mutex> lk(m);
            cv.wait(lk, [this] { return !idle.empty(); });
            const int v = idle.back();
            idle.pop_back();
            return v;
        }
        void release(int v) {
            {
                std::lock_guard<std::mutex> lk(m);
                idle.push_back(v);
            }
            cv.notify_one();
        }
    };

    inline void bench_db_pool() {
        constexpr unsigned kThreads = 4;        // 워커 수와 맞춘다
        constexpr uint64_t kPerThread = 200000;
        constexpr int      kConns = 4;          // 풀 크기 = 워커 수 (실측으로 정한 값)

        FakePool pool;
        for (int i = 0; i < kConns; ++i) {
            pool.idle.push_back(i);
        }

        const Result r = best_of("풀 acquire+release",
            static_cast<uint64_t>(kThreads) * kPerThread, 3, [&] {
                std::vector<std::thread> ts;
                ts.reserve(kThreads);
                for (unsigned t = 0; t < kThreads; ++t) {
                    ts.emplace_back([&] {
                        for (uint64_t k = 0; k < kPerThread; ++k) {
                            const int c = pool.acquire();
                            keep(static_cast<uint64_t>(c));
                            pool.release(c);
                        }
                        });
                }
                for (std::thread& th : ts) {
                    th.join();
                }
            });

        core::logf("\n── 커넥션 풀 관리 비용 (전용 연결 대비 순수 증가분) ──\n\n");
        report_per_op(r);

        const double ns = (r.ops > 0)
            ? (r.ms * 1'000'000.0 / static_cast<double>(r.ops)) : 0.0;
        core::logf("  ★ 스레드 %u개가 연결 %d개를 두고 경쟁 (워커4 / 풀4 배치와 같음)\n",
            kThreads, kConns);
        core::logf("  ★ 전용 연결이면 이 비용이 0 이다 — 시작할 때 한 번 잡고 끝이라서\n");
        core::logf("  ★★ 실측한 질의 하나는 약 92,000 ns 였다 → 풀 비용은 그 %.3f%%\n",
            100.0 * ns / 92000.0);
        core::logf("\n");
    }

    //  전체 실행
    inline void run_all() {
        const unsigned hw = std::thread::hardware_concurrency();
        core::logf("\n=== bench ===  하드웨어 스레드 %u개\n", hw);
        core::logf("(Release 로 재야 한다. Debug/ASan 수치는 의미 없다)\n\n");

        // 지금 할당이 어디서 나는가  다른 측정보다 먼저
        bench_alloc_probe();
        // False Sharing
        bench_false_sharing();
        // 락 큐 vs Lock-Free
        bench_queue();
        // mutex vs shared_mutex
        bench_rwlock();
        // 명부 스냅샷형 임계구역 — mutex vs shared_mutex(A11 재평가 · U1 폐합)
        bench_roster_snapshot();
        // 로깅은 무엇 때문에 느린가  고치기 전에 잰다
        bench_logger();
        // 지금 로거(비동기)를 그대로 잰다 + 드롭 경로 확인
        bench_async_logger();
        // 드롭 정책이 레벨별로 지켜지는가 (판정은 로그 파일에서)
        bench_log_drop_policy();
        // 커넥션 풀 관리 비용 (전용 연결 대비 순수 증가분)
        bench_db_pool();

        core::logf("\n=== bench 끝 ===\n");
    }

}   // namespace bench
