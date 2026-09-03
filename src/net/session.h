//  net/session.h — IOCP 가 다루는 두 가지 데이터
//
//  IOCP 의 완료 통지는 두 축으로 온다. 이 파일이 그 두 축의 실체다.
//
//      "누구의 완료인가"      → 완료 키(completion key) → Session*
//      "어느 I/O 의 완료인가"  → OVERLAPPED*            → IoContext*
//
//  net 계층은 proto 를 모른다. 여기엔 바이트와 소켓만 있고,
//  "이 바이트가 무슨 메시지인가"는 위층의 일이다.
#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>

#include "core/job_queue.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

// net 은 world 를 모른다(계층 규칙) — 그런데 거래는 세션이 직접 들고 있어야
// UAF 가 안 난다. raw 포인터의 atomic 은 load 와 역참조 사이에 상대가
// 거래를 지우면 해제된 메모리를 읽는다 — atomic<shared_ptr> 만이 load 시
// 강한 참조를 쥐어 그 창을 닫는다. 그래서 Session::trade 한 필드만 world::Trade 의
// "이름"을 예외로 안다 — 전방 선언이라 include 는 0 이고 world 의 정의가
// 완전할 것을 요구하지도 않으니 컴파일 결합은 생기지 않는다. 그래도 이
// 자리는 net 이 world 의 타입명을 아는 유일한 지점이라, 이후 이 필드를
// 발판 삼아 Trade 의 멤버를 net 쪽 코드가 직접 만지는 후속 코드가 들어오면
// 그건 이 한 줄이 아니라 그 코드가 계층을 어긴 것이다. 버린 대안은
// shared_ptr<void> + 캐스트다 — 계층은 지키지만 캐스팅 오류 위험을 세션
// 필드에 심는다.
namespace world { struct Trade; }

namespace net {
    // 한 프레임 전체를 담을 수 있어야 한다. 못 담으면 그 프레임은 영원히
    //   완성되지 않고 그 세션은 그대로 멈춘다 (커널은 계속 받는데 내가 자를 수가 없다).
    //
    //   여기서 proto::kMaxBodySize 를 쓰지 않는 것은 net 이 proto 를 모르기 때문이다.
    //   그래서 이 값은 net 이 독립적으로 정하고, 「둘 다 아는 층」인 app/frame_router.cpp 가
    //   static_assert 로 둘의 관계를 강제한다. 계층이 서로 모를 때 쓰는 방법이다.
    constexpr int kRecvBufferSize = 4100;      // 4(헤더) + 4096(본문 상한)

    // 송신 큐 상한. 상대가 안 읽으면 back 이 무한히 늘어난다 —
    // 느린 클라이언트 하나가 서버 메모리를 먹는 고전적인 형태다.
    //
    // 이 값은 「back 하나의」 상한이다. 세션 전체의 상한이 아니다 —
    //   send_chunks 가 back.size() + total 만 보고, front 는 안 센다.
    //   front 도 back 에서 swap 돼 온 것이라 같은 크기까지 갈 수 있으므로,
    //   실제 세션당 최대는 약 2배다 (front 1MB + back 1MB).
    //   front 를 같이 세지 않는 이유 — front 는 「이미 커널에 넘긴」 것이라
    //   내가 줄일 수 있는 양이 아니다. 역압은 「더 받지 않는다」로만 걸 수 있다.
    //   메모리 상한을 계산할 때는 이 값의 2배로 잡을 것.
    constexpr size_t kMaxSendQueue = 1 * 1024 * 1024;    // 1MB (세션당 실제 최대는 약 2MB)

    //  누적 수신 버퍼 — 완료로 온 덩어리를 이어 붙여 두고 완성된 프레임만 잘라 올린다.
    //
    //      [ 처리 끝 ][ 아직 못 자른 것 ][ 빈 공간 ]
    //      0        read_pos          write_pos   kRecvBufferSize
    //
    //  커서가 둘인 이유 — 「받은 것」과 「처리한 것」이 다르기 때문이다.
    //  하나면 절반만 온 프레임을 어디까지 봤는지 기억할 수 없다.
    struct RecvBuffer {
        char buffer[kRecvBufferSize]{};
        int  read_pos = 0;      // 다음에 파싱을 시작할 위치
        int  write_pos = 0;      // 다음에 커널이 쓸 위치

        int pending() const { return write_pos - read_pos; }        // 아직 못 자른 바이트
        int space()   const { return kRecvBufferSize - write_pos; } // 커널이 쓸 수 있는 공간

        char* tail() { return buffer + write_pos; }
        const char* head() const { return buffer + read_pos; }

        // 앞으로 당기기. 빈 공간이 앞쪽에만 남으면 커널이 쓸 자리가 없어서, 남은 조각을
        // 맨 앞으로 옮겨 뒤쪽을 되찾는다.
        //
        // 걸린 I/O 가 없을 때만 불러야 한다. 커널이 이 버퍼에 쓰는 중에 옮기면 커널은
        // 옮기기 전 주소에 계속 쓴다 — 완료를 수거한 뒤 다음 WSARecv 를 걸기 전에만
        // 부른다는 것이 불변식이다.
        //
        // 매번 안 당기는 것은 memmove 라서다. 프레임이 딱 떨어지면 남은 게 0 이라
        // 당길 것도 없다.
        void compact() {
            if (read_pos == 0) {
                return;                       // 이미 맨 앞이다
            }
            const int left = pending();
            if (left > 0) {
                std::memmove(buffer, buffer + read_pos, static_cast<size_t>(left));
            }
            read_pos = 0;
            write_pos = left;
        }
    };

    // 세션당 I/O 가 recv·send 둘이라 「어느 I/O 의 완료인가」가 필요하다.
    enum class IoOp : uint8_t {
        Recv,
        Send,
    };

    // per-I/O 데이터 — I/O 요청 하나마다 하나씩 필요하다.
    //
    // OVERLAPPED 가 반드시 첫 멤버여야 한다. 커널은 완료 시 OVERLAPPED* 만 돌려주는데,
    // 첫 멤버면 그 주소가 곧 IoContext 의 주소라 캐스팅 한 번으로 되돌릴 수 있다.
    // (일반해는 CONTAINING_RECORD 지만 여기선 첫 멤버 보장으로 대신한다)
    //
    // buffer 의 수명이 핵심이다. WSARecv 를 건 순간부터 완료가 돌아올 때까지 이 메모리는
    // 커널이 쓰는 중이고, 내가 안 읽었어도 그렇다. 그 사이에 해제하면 use-after-free 다 —
    // 논블로킹(준비 통지)과 비동기(완료 통지)의 실질적 차이가 이것이다.
    struct IoContext {
        OVERLAPPED ov{};        // 첫 멤버 유지
        WSABUF     wsabuf{};    // 버퍼를 소유하지 않는다 — Session 의 누적 버퍼를
        IoOp       op = IoOp::Recv;   // 가리키기만 한다. 커널이 거기 직접 쓰므로 복사가 0이다.
    };

    // 송신 이중 버퍼. back 은 새로 들어오는 것이 쌓이는 곳, front 는 지금 WSASend 가
    // 걸려 있는 것이다.
    //
    // 둘인 이유는 같은 소켓에 WSASend 를 두 개 걸면 도착 순서가 보장되지 않아서다.
    // TCP 가 보장하는 것은 바이트 스트림의 순서지 내가 낸 I/O 요청들의 순서가 아니다.
    // 발행은 항상 하나여야 하고, 그동안 들어온 것은 어딘가 쌓여야 한다.
    //
    // 부수 효과로 배치가 공짜로 생긴다 — 발행 중에 들어온 N개가 다음 한 번에 나가고,
    // 부하가 높을수록 더 많이 뭉친다.
    //
    // mutex 는 진짜 경쟁이라 있다. 송신은 recv 완료를 처리하는 워커가 걸고, 송신 완료는
    // 다른 워커가 처리할 수 있다.
    struct SendBuffer {
        std::vector<char> front;           // 발행 중인 바이트
        std::vector<char> back;            // 다음에 나갈 바이트
        int               sent = 0;     // front 에서 이미 나간 바이트 (부분 송신용)
        bool              sending = false; // front 로 WSASend 가 걸려 있나
        std::mutex        mutex;

        // 큐 넘침이 연속 몇 번째인가(§17-6) — mutex 를 쥔 곳에서만 읽고 쓴다.
        //   한 번의 넘침은 순간 폭주일 수 있어 봐주지만, 연속이면 상대가 계속
        //   안 읽는다는 뜻이다. 성공 큐잉 한 번이 이 값을 0으로 되돌린다 —
        //   "연속" 이라는 계약이 그것이다.
        int  overflow_streak = 0;

        // 상한의 3/4 를 넘었다는 경고를 세션당 한 번만 찍기 위한 에지 트리거 —
        //   mutex 를 쥔 곳에서만 만진다. 넘침 직전에 미리 알아채려는 관측용
        //   신호라 넘침 자체(overflow_streak)와는 별개다.
        bool queue_warned = false;
    };

    //  per-handle 데이터 — 접속 하나마다 하나씩. 완료 키로 이 포인터를 넘긴다.
	struct Session {
		uint64_t id{};	// 세션 고유 ID
		SOCKET socket = INVALID_SOCKET;	// 접속 소켓
		sockaddr_in peer{};	// 상대 주소
		IoContext recv_ctx;
        RecvBuffer recv_buf;    // 커널이 여기에 직접 쓴다
        IoContext   send_ctx;
        SendBuffer  send_buf;

        // "127.0.0.1:54321" — 로그마다 inet_ntop 을 다시 부르지 않으려고 한 번만 만든다
        char peer_text[32]{};

        // 세션 수명의 전부가 이 두 줄이다.
        //
        // io_count 는 지금 이 세션에 걸려 있는 미완료 I/O 수다. 소유 카운트와는 다른
        // 것을 센다 — 아무도 이 세션을 안 붙들고 있어도 커널은 recv 버퍼에 쓰고 있을
        // 수 있다. closing 은 더 이상 새 I/O 를 걸지 않는다는 표시다. 소켓을 닫는
        // 것만으로는 부족하다. 걸려 있던 완료가 아직 돌아오는 중이라 「끝났다」와
        // 「지워도 된다」가 다른 시점이기 때문이다.
        //
        // 삭제 조건은 둘 다다 — closing 이 서 있고 io_count 가 0. 하나만 보면 조기
        // 삭제이거나 영구 누수다.
        std::atomic<int>  io_count{ 0 };
        std::atomic<bool> closing{ false };

        // 마지막으로 이 세션에서 무언가를 받은 시각(ms).
        //
        // ping 을 받은 시각이 아니라 무엇이든 받은 시각이다. 채팅을 치는 사람은 ping 을
        // 안 보내도 살아 있는데, ping 만 세면 말이 많은 유저일수록 더 잘 끊기는 거꾸로
        // 된 정책이 된다. 그래서 프레임 파싱이 아니라 수신 완료 지점에서 민다 — net 은
        // 그 바이트가 무슨 메시지인지 몰라도 된다.
        //
        // atomic 인 것은 I/O 워커가 쓰고 스윕 스레드가 읽는 진짜 경쟁이라서다. 아래
        // player_id 가 atomic 이 아닌 것과 대비된다.
        //
        // steady_clock 이 아니라 GetTickCount64 인 것은 수신 완료마다 부르는 자리라
        // 값이 싸야 해서다. 공유 페이지를 읽을 뿐이라 수 ns 이고, steady_clock 은 QPC 를
        // 탄다. 해상도 15.6ms 가 걸리지만 재는 단위가 90초라 영향이 없다.
        //
        // 0 으로 두면 안 된다. 접속만 하고 아무것도 안 보낸 세션이 「1970년부터
        // 조용했다」로 읽혀 첫 스윕에 바로 끊긴다.
        std::atomic<uint64_t> last_recv_ms{ 0 };

        // 처리할 수 없는 메시지가 쌓인 정도. 위층이 매기고 위층이 읽는다 — 무엇이 나쁜
        // 메시지인지는 프로토콜 지식이고 net 에는 없다.
        //
        // 모르는 msg_id 하나는 버전 스큐일 수 있어 끊지 않지만, 무한히 봐주면 상대가
        // 쓰레기를 계속 보내도 파싱·복사 비용을 계속 문다. 「한 번은 실수, 계속이면
        // 상대가 이상한 것」을 세는 값이다.
        //
        // 줄어들지 않는다. 시간 창으로 감쇠시키면 느리게 계속 보내는 상대를 영원히 못
        // 끊는다. 정상 클라는 애초에 0 이라 누적이 문제되지 않는다.
        //
        // atomic 인 것은 이 값을 매기는 실행이 매번 같은 워커 스레드에서 돈다는
        // 보장이 없어서다(직렬 큐 실행권이 워커 사이를 옮겨 다닌다) — player_id 와
        // 달리 이 값은 그 순서 보장(happens-before)에 기대지 않고 그냥 원자로 둔다.
        std::atomic<int> bad_msg_score{ 0 };

        // 이 세션이 누구인가. 0 이면 아직 로그인 전이고, 그때는 인벤토리도 거래도 못 한다.
        //
        // 조회할 player_id 를 클라이언트가 body 로 보내면 아무나 남의 인벤토리를 보고,
        // 거래까지 붙으면 아무나 남의 아이템을 옮긴다. 인자 검증으로는 못 막는다 —
        // 그 값이 맞는지 판단할 근거가 서버에 없다. 믿을 값을 한 곳에서 한 번만 정한다.
        //
        // atomic 이 아닌 것은 이 값을 이 세션의 직렬 큐 실행권을 쥔 스레드만 읽고
        // 쓰기 때문이다 — 그 실행권은 세션당 하나뿐이라(session.h 의 sq_scheduled
        // 참조) 동시에 두 스레드가 이 필드를 만질 수 없다. DB 호출도 이제 그 실행
        // 안에서 동기로 이뤄지므로 값으로 복사해 별도 스레드에 넘길 필요가 없다.
        // 경쟁이 없는데 atomic 을 붙이면 여기 경쟁이 있다는 잘못된 신호를 남긴다 —
        // atomic 은 비용이 아니라 독자에게 주는 정보다.
        //
        // 인증이 아니다. 클라이언트가 보낸 값을 그대로 박는다. 실무에서는 로그인 서버가
        // 발급한 토큰을 여기서 검증한다.
        uint64_t player_id = 0;

        // L2 — 이 세션의 게임 상태(거래 등)를 지키는 락. 위 SendBuffer::mutex(송신
        // 전용)와 절대 겸용하지 않는다 — 송신은 recv 완료 워커와 송신 완료 워커가
        // 각자 걸 수 있는 별개의 경쟁이고, 이 락은 그것과 무관한 게임 로직 동시성을
        // 지킨다. 하나로 합치면 송신 완료 처리가 게임 로직 락에 물리고 그 반대도
        // 마찬가지라, 서로 상관없는 두 대기가 서로를 기다리게 된다.
        std::mutex game_mutex;

        // 존 라벨. 이 세션이 지금 어느 존 소속인가 — 직렬 큐 직렬화(아래) 안에서만
        // 읽고 쓴다. 그 세션의 실행권을 쥔 스레드가 유일해서 이 필드엔 락이 필요
        // 없다.
        uint32_t zone = 0;

        // 직렬 큐 — 이 세션 앞으로 온 실행 단위를 쌓아 두는 곳. sq_scheduled 는
        // 「이 직렬 큐를 비우는 워커가 이미 하나 떠 있는가」다 — push 하는 쪽과
        // 비우는 쪽이 serial_queue·sq_scheduled 를 항상 같은 sq_mutex 임계구역 안에서
        // 함께 본다. 덱과 플래그가 서로 다른 락 아래 갈리면 실행권이 유실되거나
        // 중복 발급되는 레이스가 생긴다.
        std::mutex             sq_mutex;
        std::deque<core::Job>  serial_queue;
        bool                   sq_scheduled = false;

        std::atomic<std::shared_ptr<world::Trade>> trade;
	};
}   // namespace net
