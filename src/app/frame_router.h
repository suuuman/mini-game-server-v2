//  frame_router.h — 「받은 바이트를 무엇으로 볼 것인가」
//
//  net 은 존을 모르고, db 는 게임 상태를 모른다. 그 셋을 다 아는 층이 app 이고,
//    그중 「프로토콜을 아는 쪽」이 이 파일이다. 조립은 bootstrap 이 한다.
//
//  노출하는 것은 셋뿐이다 — net 이 되묻는 세 가지 질문에 답하는 함수들이다.
//      frame_size      : "어디서 자를까요"
//      on_frame        : "이 한 덩어리를 어떻게 할까요"
//      on_session_gone : "이 세션이 끊겼는데 정리할 게 있나요"
//    나머지(핸들러 · 송신 헬퍼 · 부하 주입 스위치)는 전부 .cpp 안에 있다.
#pragma once

#include "net/iocp_server.h"
#include "db/db_pool.h"
#include "app/entry_table.h"

namespace app {

    class WorkerPool;

    // 프레임 자르기. > 0 한 메시지 길이 · = 0 더 받아야 함 · < 0 규약 위반(끊기)
    int frame_size(const char* data, int len);

    // 한 프레임을 처리한다. false = 이 세션을 끊어라
    //   이 호출 자체가 이미 그 세션의 직렬 큐 실행권 안에서 돈다(app::WorkerPool) —
    //     그래서 존을 알아야 하는 핸들러는 session.zone 을 직접 읽는다. 예전의
    //     placement 인자·「다시 읽지 않는다」계약은 그 실행권 하나가 대신한다.
    //   entry 는 kEnterReq 가 예약을 소비하고 입장 집합에 넣을 때, 그리고 존
    //     이동·채팅·거래 상대 탐색이 명부(L1)를 조회할 때 쓴다. S2S 통지는 더
    //     이상 이 함수의 일이 아니다 — EntryTable::set_notifier 로 옮겨
    //     enter/leave 성공과 같은 임계구역 안에서 자동으로 나간다.
    //   db_pool 은 거래 확정·인벤토리 조회가 그 자리에서 동기로 커넥션을 빌리는
    //     자리다(§7-4 — 락보다 먼저 빌린다). DB 워커 스레드로 넘기지 않는다.
    bool on_frame(net::IocpServer& server, db::DbPool& db_pool, EntryTable& entry,
        net::Session& session, const char* frame, int len);

    // 끊긴 세션의 뒷정리를 그 세션 자신의 직렬 큐 맨 뒤에 넣는다 (거래 정리 지점
    // 4/4). 직렬 큐가 그 세션의 다른 실행(로그인·입장·이동)보다 이 정리가 뒤에
    // 서도록 순서를 보장한다 — 예전 존 큐의 FIFO 가 서던 그 자리를 승계한다.
    //   여기에 프로토콜 지식이 있다 — 상대에게 kTradeCancelNtf 를 보내야 한다.
    //     그래서 배선(bootstrap)이 아니라 이 파일에 있다.
    //   entry 는 입장 집합에서 빼는 데(EntryTable::leave) 쓴다 — 그 성공을
    //     세션 서버에 PlayerLeave 로 알리는 것은 이 함수가 아니라 명부 자신의
    //     notifier(set_notifier 로 꽂은 것)가 한다.
    bool on_session_gone(net::IocpServer& server, WorkerPool& workers,
        EntryTable& entry, net::Session& session);

} // namespace app
