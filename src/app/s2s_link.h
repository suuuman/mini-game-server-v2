//  app/s2s_link.h — 마을이 세션 서버에 붙는 절차(등록·재등록·하트비트)
//
//  net::S2sConnector 는 연결·재연결·요청 매칭만 안다 — 프로토콜을 모른다.
//  이 파일이 그 위에서 "Register 를 보낸다 · result 를 본다 · registered 일 때만
//  Heartbeat 를 보낸다"는 S2S 프로토콜의 실제 절차를 안다. frame_router.cpp 가
//  net 과 proto 를 잇는 자리인 것과 같은 역할을 마을→세션 서버 방향에서 한다.
#pragma once

#include "core/config.h"
#include "net/s2s_connector.h"

#include <cstdint>
#include <string>

namespace net { class IocpServer; }

namespace app {

    // entry_table.h 를 안 끌어오는 이유 — start() 시그니처와 포인터 멤버만
    // 있으면 되고, 그 둘은 완전한 타입을 요구하지 않는다. .cpp 가 실제로
    // add_reservation() 을 부를 때만 완전한 정의가 필요하다.
    class EntryTable;

    //  [s2s] 설정에서 뽑아낸 값. host 가 비어 있으면 비활성이다 — 그 뜻을
    //  S2sLink::start() 가 해석한다(여기는 값만 담는다).
    struct S2sSettings {
        std::string host;
        uint16_t    port = 9100;
        std::string advertise_host = "127.0.0.1";
        int backoff_initial_ms = 1000;
        int backoff_max_ms     = 30000;
        int request_timeout_ms = 10000;
        int heartbeat_ms       = 5000;
    };

    S2sSettings load_s2s_settings(const core::Config& cfg);

    class S2sLink {
    public:
        S2sLink() = default;
        ~S2sLink();

        // 복사·이동 금지 — net::S2sConnector 를 값으로 물고 있고, 그 안에 스레드와
        // 커널 핸들이 있다.
        S2sLink(const S2sLink&) = delete;
        S2sLink& operator=(const S2sLink&) = delete;

        // settings.host 가 비어 있으면 커넥터를 만들지 않고 로그만 남긴 뒤 true 를
        // 돌려준다 — "host 없음"은 실패가 아니라 정상적인 비활성 상태다(기존
        // 하네스 8종 환경이 이 값으로 무변경임을 보장하는 지점).
        // server_port/server_capacity 는 Register 에 실을 [server] 절 값 그대로다
        // (⛔ [io] 가 아니다 — bootstrap.cpp::load_settings 와 server.ini 원문 참조).
        // entry 는 main.cpp 가 소유한다 — Reserve 요청이 들어올 때 on_request 가
        // 예약을 저장하는 데 쓴다. main.cpp 의 선언 순서(zones 와 server 사이)가
        // 이 링크보다 entry 를 먼저 만들고 나중에 지워, 포인터로 들고 있어도
        // entry 가 이 링크보다 먼저 죽을 일이 없다.
        // server 는 Kick 이 close_by_id 를 부르는 대상이다 — net::IocpServer
        // 는 이 파일이 몰라도 되던 타입이었는데(전방선언 참조), Kick 이 그
        // 경계를 넘어야 해서 여기서만 포인터로 받는다.
        // reserve_expire_max_ms/fullsync_chunk_max 를 구조체가 아니라 값으로 받는
        // 이유 — 그 값의 출처(bootstrap.h::VillageEntrySettings)를 구조체로 넘기면
        // 이 헤더가 bootstrap.h 를 include 해야 하는데, bootstrap.h 는 배선을 위해
        // 이미 이 헤더를 include 한다(순환). server_port/server_capacity 를 개별
        // 인자로 받는 지금 관례와도 맞다.
        bool start(const S2sSettings& settings, uint16_t server_port, uint32_t server_capacity,
            net::IocpServer& server, EntryTable& entry, uint32_t reserve_expire_max_ms,
            int fullsync_chunk_max);

        // 멱등 — 시작하지 않았으면(비활성) 아무 일도 안 한다.
        // 짝 비대칭 — 소멸자(~S2sLink)는 이 함수만 부르고 unregister_and_wait()
        //   는 안 부른다. 그래서 정상 종료가 아닌 경로(예외로 인한 조기 소멸)에서는
        //   Unregister 가 안 나간다 — 의도다. 그 경로는 세션 쪽 orphan 유예(§5-3)가
        //   흡수한다. 정상 종료 경로는 main.cpp 가 unregister_and_wait() 를 먼저
        //   불러 명시적으로 알린다.
        void stop();

        // 알림 둘 — 비활성([s2s] host 없음)이면 조용히 no-op 한다. 기존 하네스
        // 8종 환경이 이 값 하나로 무변경을 보장받는 지점이 여기다(§6 참조).
        // notify() 의 반환값(false = stop 이후)은 무시한다 — 알림이라 실패해도
        // 재시도할 응답 콜백이 없고, 종료 중이면 어차피 상대가 없다.
        void notify_player_enter(uint64_t player_id);
        void notify_player_leave(uint64_t player_id);

        // Unregister 는 위 둘과 다르다 — 세션 서버는 이걸 진짜 요청으로 보고
        // seq 를 그대로 돌려 UnregisterAck 로 답한다(session_router.cpp 의
        // 대응 분기). notify() 로 보내면 이 함수가 반환한 직후 호출부가
        // 커넥터를 곧 닫을 때 실제 전송이 됐는지가 레이스가 된다 — 그래서
        // request() 로 보내고 응답(또는 자체 상한 타임아웃 — settings_.request_timeout_ms
        // 가 아니다. s2s_link.cpp::kUnregisterWaitMs 주석 참조)을 기다린 뒤 돌아온다.
        // main.cpp 가 server.stop() 보다 먼저 이 함수를 부른다 — stop() 은 이
        // 함수를 부르지 않으므로(위 stop() 의 짝 비대칭 주석 참조) 정상 종료
        // 경로에서만 호출부가 명시적으로 불러야 한다.
        void unregister_and_wait();

    private:
        void on_connected();
        void on_disconnected();
        void on_request(uint16_t msg_id, uint32_t seq, const char* body, int len);
        void on_tick();
        void on_register_ack(net::S2sResult result, uint16_t msg_id, const char* body, int len);
        void on_heartbeat_ack(net::S2sResult result, uint16_t msg_id, const char* body, int len);

        // 등록이 확인된 순간(on_register_ack 의 kResultOk 분기)에만 부른다 —
        // §5-2 순서(① Register ② FullSync ③ 전량 교체)의 근거는 s2s_link.cpp 참조.
        void send_full_sync();

        net::S2sConnector connector_;
        S2sSettings settings_;
        uint16_t server_port_ = 0;
        uint32_t server_capacity_ = 0;

        // start() 가 채운다. Reserve 를 처리하는 on_request 가 이 포인터로만
        // EntryTable 을 만진다 — 참조 멤버로 두면 S2sLink() = default 가 안 되므로
        // 포인터로 든다(entry 의 수명은 위 start() 주석 참조).
        EntryTable* entry_ = nullptr;

        // Kick 이 close_by_id 를 부르는 대상. entry_ 와 같은 이유로 포인터다 —
        // 수명은 entry 와 같다(main.cpp 선언 순서 참조).
        net::IocpServer* server_ = nullptr;

        // Reserve 의 expire_ms 는 상대(세션 서버)가 부르는 값이라 그대로 믿지
        // 않는다 — 마을이 인정하는 예약 유효시간의 상한이다(min 클램프,
        // config/server.ini [village_entry] reserve_expire_ms 참조).
        uint32_t reserve_expire_max_ms_ = 10000;

        // 0 = proto::s2s::kFullSyncMaxPerChunk 그대로. 그 외에는 그 값과의 min —
        // 설정이 파생 상한을 못 넘게 한다(config/server.ini 의 같은 이름 키 참조).
        int fullsync_chunk_max_ = 0;

        // Ack(result=0) 수신 → true. on_disconnected → false. result=1 수신 →
        // false 유지. on_tick 이 이 값을 보고 Heartbeat 를 보낼지 정한다 — 재연결
        // 직후 새 RegisterAck 가 오기 전에 조기 Heartbeat 가 나가는 것을 이 전이가 막는다.
        bool registered_ = false;

        // connector_.start() 를 실제로 불렀는가 — host 가 비어 비활성으로 끝났으면
        // false 로 남는다. stop() 이 이 값을 보고 커넥터를 건드릴지 정한다.
        bool started_ = false;

        // DrainComplete 를 한 번만 보내려는 플래그다. entry_->draining_ 과 달리
        // S2S 스레드만 읽고 쓴다(on_request 의 SetMode 분기 · on_tick ·
        // on_disconnected 전부 이 스레드 위에서 돈다) — atomic 이 아니라 plain
        // bool 인 이유다.
        bool drain_complete_sent_ = false;
    };

}   // namespace app
