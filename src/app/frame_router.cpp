// frame_router.cpp — 프로토콜. 「받은 바이트를 무엇으로 볼 것인가」가 전부다.
//
// 한때 main.cpp 안에 있었다. 그 파일이 「무엇으로 볼 것인가」와 「서버를 조립해 띄운다」를
// 같이 지고 있었는데, 둘은 바뀌는 이유가 다르다 — 프로토콜이 늘면 이쪽이 커지고
// 서브시스템이 늘면 저쪽이 커진다.
//
// 이 파일이 net·proto·world·db 를 전부 안다. 역방향은 없다 — net 은 존을 모르고
// db 는 게임 상태를 모른다.
#include "app/frame_router.h"
#include "app/worker_pool.h"

#include <windows.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <vector>

#include <mutex>

#include "proto/packet.h"
#include "core/log.h"
#include "core/lock_rank.h"
#include "db/db_conn.h"
#include "world/trade.h"

// net 도 proto 도 서로를 모른다. 둘 다 아는 이 층이 관계를 강제한다.
//   깨지면 링크도 실행도 아닌 「컴파일」 단계에서 잡힌다.
static_assert(proto::kHeaderSize + proto::kMaxBodySize <= net::kRecvBufferSize,
	"수신 버퍼가 한 프레임을 못 담는다 — 그 프레임은 영원히 완성되지 않는다");
namespace {
    // 로직을 일부러 느리게 만든다 — 「이 세션의 실행」만 막히고 I/O 워커는 계속
    // Job 을 쌓는지 보는 용도다. 분리가 안 됐다면 워커가 같이 막힌다.
    constexpr DWORD kLogicDelayMs = 0;

    // 인벤토리 요청을 「이 실행 안에서 동기로」 처리한다.
    //   이제 정상 경로 자체가 동기라, 이 스위치는 「비동기 경로가 사라진 뒤에도
    //   유의미한 부하 주입」으로 재정의됐다 — try_acquire 조차 건너뛰고 즉시
    //   응답한다. 그 자리가 원래 노리던 「같은 워커의 다른 세션까지 밀린다」는
    //   워커 풀 격리(zone_block.ps1 재정의)가 대신 검증한다.
    constexpr DWORD kBadSyncDbMs = 0;

    // 거래를 트랜잭션 없이 처리한다. true 면 trade_unsafe() 를 부르는데, 중간 실패가
    // 그대로 데이터 손실이 되고 잔량이 모자라도 지급이 나가 아이템이 복사된다.
    // 평시 false — 켜진 채로 회귀를 돌리면 결과가 거짓말을 한다.
    constexpr bool kBadTradeNoTx = false;

    // 처리할 수 없는 메시지 정책 — 한 번은 실수, 계속이면 상대가 이상한 것.
    //
    // 끊지 않고 세는 것은 모르는 msg_id 가 클라 버전 스큐로도 생기기 때문이다. 끊으면
    // 롤아웃 중에 정상 유저가 튕긴다. 그렇다고 무한히 봐주면 쓰레기를 계속 보내도
    // 파싱·복사 비용을 계속 문다 — 상한이 있어야 역압이 성립한다.
    //
    // 가중치가 다른 것은 의심의 무게가 달라서다. 미정의 ID 는 신버전 클라일 수 있어
    // 가볍게 세고, 서버 전용 ID 는 정상 클라가 낼 수 없는 값이라 무겁게 센다. 그래도
    // 즉시 끊지는 않는다 — 응답 ID 를 되돌려 보내는 클라 버그 하나에 접속이 끊기면
    // 원인 찾기가 어려워진다. 무겁게 세면 8회에 끊기므로 오래 봐주지는 않는 셈이다.
    //
    // 숫자의 근거는 약하다. 정상 클라는 애초에 0 이고 비정상은 대개 폭주라 금방 넘는다.
    constexpr int kBadMsgLimit      = 32;   // 누적 점수가 이걸 넘으면 끊는다
    constexpr int kScoreUnknownId   = 1;    // 표에 없는 ID
    constexpr int kScoreServerOnlyId = 4;   // 서버만 보내는 ID 가 올라왔다

    // 로그 레이트 리밋 — 초당 수천 개가 오면 로그 큐가 그대로 터진다.
    //   무시로 바꾸면서 새로 생긴 위험이다. 끊을 때는 세션당 한 줄로 끝났다.
    //   2의 거듭제곱 지점에서만 찍는다: 1, 2, 4, 8, 16, 32 …
    //   → 처음 몇 번은 다 보이고, 폭주하면 로그가 로그(log) 스케일로 준다.
    constexpr bool should_log_bad_msg(int score) {
        return (score & (score - 1)) == 0;      // score 가 2의 거듭제곱인가
    }

    //  응답 하나 보내기 — 헤더를 만들어 본문 앞에 붙인다.
    //
    //  head 가 스택 배열인데 안전한 이유 —
    //    send_chunks 가 락 안에서 큐로 「복사」한다. 이 함수가 끝나도 큐엔 값이 남는다.
    //    그게 send_chunks 의 계약이고, 덕분에 호출부마다 버퍼 수명을 신경 쓰지 않는다.
    bool send_msg(net::IocpServer& server, net::Session& to, proto::MsgId id,
        const char* body, int body_len) {
        uint8_t head[proto::kHeaderSize];
        const proto::PacketHeader h{ static_cast<uint16_t>(body_len), id };
        proto::encode_header(head, h);

        const net::SendChunk parts[2] = {
            { reinterpret_cast<const char*>(head), static_cast<int>(proto::kHeaderSize) },
            { body, body_len },
        };
        return server.send_chunks(to, parts, (body_len > 0) ? 2 : 1);
    }

    //  통짜 프레임 — 여러 body 조각을 [header][body] 연속 버퍼 하나로 합친다.
    //  proto::encode_header 를 여기서 한 번만 부른다. send_msg 는 대상 하나마다
    //  send_chunks 로 header/body 두 조각을 넘기는데, 채팅 브로드캐스트처럼
    //  같은 body 를 N 명에게 그대로 반복해서 보낼 때는 그 조립을 한 번만 해도
    //  된다 — 이 헬퍼가 그 한 번을 만들고, 호출자가 대상마다 send_frame 으로
    //  같은 버퍼를 큐잉한다.
    //
    //  이게 하는 일은 §17-2 의 ①(직렬화 1회)뿐이다. ②(대상마다 send_chunks
    //  가 큐로 복사하는 것 자체를 없애는 것)는 범위 밖이다 — 관측 가능한
    //  와이어가 최적화 전후로 완전히 같아서, 이 함수를 걷어내고 예전처럼
    //  대상마다 다시 조립해도 하네스로는 못 잡는다(TESTING 「못 덮는 것」
    //  대상 — 유지 근거는 코드 정독뿐이다).
    //  합산과 복사가 같은 조건(len > 0 만 가산)이어야 한다 — 다르면 호출부가
    //  음수 길이 조각을 넘겼을 때(예: body_len 하한 검증이 뚫려 text_len 이
    //  음수가 되는 경우) 합산은 그 음수만큼 줄어든 값으로 버퍼를 할당하는데
    //  복사는 그 조각을 건너뛰지 않아 실제로 쓴 바이트 수가 할당량을 넘는
    //  힙 오버런이 된다. 두 루프를 같은 규칙으로 묶으면 그 클래스 자체가
    //  성립하지 않는다 — 계산한 만큼만 쓴다.
    std::vector<char> build_frame(proto::MsgId id, const net::SendChunk* parts, int count) {
        int body_len = 0;
        for (int i = 0; i < count; ++i) {
            if (parts[i].len > 0) { body_len += parts[i].len; }
        }

        // 호출 전 상한 검사(kMaxBodySize)는 호출자의 계약이다 — 여기는 그
        // 계약이 지켜졌는지를 Debug 에서만 잡는다. u16 로 좁히는 캐스트는
        // Release 에서도 항상 도니 절단 자체는 막히지만, 계약이 깨진 새
        // 호출자를 조용히 통과시키지 않으려는 것이다.
        assert(body_len <= static_cast<int>(proto::kMaxBodySize));

        std::vector<char> buf(static_cast<size_t>(proto::kHeaderSize) + body_len);
        const proto::PacketHeader h{ static_cast<uint16_t>(body_len), id };
        proto::encode_header(reinterpret_cast<uint8_t*>(buf.data()), h);

        char* p = buf.data() + proto::kHeaderSize;
        for (int i = 0; i < count; ++i) {
            if (parts[i].len > 0) {
                std::memcpy(p, parts[i].data, parts[i].len);
                p += parts[i].len;
            }
        }
        return buf;
    }

    //  거래 통지 세 벌
    //
    //  성공과 실패를 다른 메시지로 나눈다.
    //      성공  → 상태 통지(Ntf)가 증명한다. "됐다" 를 따로 안 보낸다
    //      실패  → kTradeAck 에 result 를 실어 보낸다
    //      성사  → kTradeAck[kOk] . 이 조합만 「거래가 끝났다」를 뜻한다
    //    성공마다 ack 를 보내면 kOk 가 「요청 접수」와 「거래 완료」 둘 다를
    //    뜻하게 되고, 클라이언트가 그 둘을 구분할 방법이 사라진다.
    bool send_trade_result(net::IocpServer& server, net::Session& to,
        proto::ResultCode rc, uint32_t peer_session) {
        char b[5];
        b[0] = static_cast<char>(rc);
        proto::write_u32_be(reinterpret_cast<uint8_t*>(b) + 1, peer_session);
        return send_msg(server, to, proto::MsgId::kTradeAck, b, 5);
    }

    // kTradeOpenNtf · kTradeCancelNtf · kTradeReqNtf — 셋 다 본문이 세션 id 하나다
    bool send_peer_ntf(net::IocpServer& server, net::Session& to,
        proto::MsgId id, uint32_t peer_session) {
        char b[4];
        proto::write_u32_be(reinterpret_cast<uint8_t*>(b), peer_session);
        return send_msg(server, to, id, b, 4);
    }

    //  상태를 「전부」 보낸다. 바뀐 것만 보내면 패킷 하나가 어긋날 때
    //    양쪽 화면이 갈라지고, 그 상태로 확인을 누르면
    //    "내가 본 것과 다른 게 거래됐다" 가 된다. 거래에서 가장 나쁜 사고다.
    //
    //  my / peer 로 보내는 이유 — 받는 쪽이 자기 기준으로 바로 그린다.
    //    a / b 로 보내면 클라이언트가 매번 "내가 a 인가 b 인가" 를 따져야 하고,
    //    그 판단을 클라이언트마다 다시 짜게 된다.
    bool send_trade_state(net::IocpServer& server, const world::Trade& t, net::Session& to) {
        const bool is_a = (&to == t.a);

        char b[18];
        uint8_t* p = reinterpret_cast<uint8_t*>(b);
        proto::write_u32_be(p + 0, is_a ? t.a_item : t.b_item);
        proto::write_u32_be(p + 4, is_a ? t.a_count : t.b_count);
        b[8] = static_cast<char>((is_a ? t.a_confirm : t.b_confirm) ? 1 : 0);
        proto::write_u32_be(p + 9, is_a ? t.b_item : t.a_item);
        proto::write_u32_be(p + 13, is_a ? t.b_count : t.a_count);
        b[17] = static_cast<char>((is_a ? t.b_confirm : t.a_confirm) ? 1 : 0);

        return send_msg(server, to, proto::MsgId::kTradeStateNtf, b, 18);
    }

    // 양쪽에 같은 상태를 보낸다 (각자 기준으로 my/peer 가 뒤바뀐다)
    void broadcast_trade_state(net::IocpServer& server, const world::Trade& t) {
        send_trade_state(server, t, *t.a);
        send_trade_state(server, t, *t.b);
    }

    // 존 멤버 목록을 존 전체에 보낸다.
    //
    // 멤버가 바뀌는 지점에서 부른다 — 입장 확정, 존 이동(옛 존·새 존 둘 다), 접속 종료.
    // 하나라도 빠지면 증상이 조용하다. 유령 멤버가 남아 없는 사람에게 거래를 걸거나,
    // 새로 들어온 사람이 안 보여 거래를 못 한다.
    //
    // 명부(EntryTable)의 존 인덱스에서 스냅샷을 뜬다 — 원소마다 acquire 되므로
    // 이 함수 안에서 전부 소비하고 마지막에 일괄 release 한다(조기 return 없이
    // 한 번에 끝까지 돈다 — acquire 개수와 release 개수가 항상 같다).
    void broadcast_zone_members(net::IocpServer& server, app::EntryTable& entry, uint32_t zone_id) {
        std::vector<net::Session*> members;
        entry.snapshot_zone(zone_id, members);

        // 상한은 규약에서 파생된다. kInventoryAck 이 rows 를 자르는 것과 같은 계산이다.
        constexpr size_t kMaxMembers = (static_cast<size_t>(proto::kMaxBodySize) - 2) / 8;

        size_t n = members.size();
        if (n > kMaxMembers) {
            core::logf("[WARN] zone=%u members truncated %zu -> %zu\n", zone_id, n, kMaxMembers);
            n = kMaxMembers;
        }

        std::vector<char> body;
        body.reserve(2 + n * 8);

        uint8_t head[2];
        proto::write_u16_be(head, static_cast<uint16_t>(n));
        body.insert(body.end(), reinterpret_cast<char*>(head),
                                reinterpret_cast<char*>(head) + 2);

        for (size_t i = 0; i < n; ++i) {
            const net::Session* m = members[i];
            uint8_t rec[8];
            proto::write_u32_be(rec + 0, static_cast<uint32_t>(m->id));
            // player_id 는 uint64_t 지만 와이어는 u32 다 — Enter 만 u64 로 넓혔고
            //   나머지 와이어는 u32 를 유지하기로 했다(ADR-021 결정 8).
            proto::write_u32_be(rec + 4, static_cast<uint32_t>(m->player_id));
            body.insert(body.end(), reinterpret_cast<char*>(rec),
                                    reinterpret_cast<char*>(rec) + 8);
        }

        // 잘렸어도 「전원에게」 같은 것을 보낸다. 받는 사람마다 다른 목록을 주면
        //   거래 지목이 한쪽에서만 성립하는 상태가 생긴다.
        for (net::Session* m : members) {
            send_msg(server, *m, proto::MsgId::kZoneMembersNtf,
                body.data(), static_cast<int>(body.size()));
        }
        for (net::Session* m : members) {
            server.release_session(m);
        }
    }

    // db 의 어휘를 proto 의 어휘로 옮긴다. 둘 다 아는 곳은 app 층뿐이다 —
    // db 가 proto 를 알면 「게임 상태를 모른다」는 경계가 무너진다.
    proto::ResultCode to_result(db::TradeResult r) {
        switch (r) {
        case db::TradeResult::kOk:        return proto::ResultCode::kOk;
        case db::TradeResult::kNotEnough: return proto::ResultCode::kNotEnough;
        case db::TradeResult::kBusy:      return proto::ResultCode::kBusy;
        case db::TradeResult::kInvalidArg: return proto::ResultCode::kInvalidArg;
        default:                          return proto::ResultCode::kDbError;
        }
    }

    // 세션 id 는 u64 인데 와이어는 u32 다. kChatNtf 가 이미 그렇게 하고 있어 맞춘다.
    //   접속이 40억 번을 넘으면 아래 32비트가 겹친다 — 지금 범위 밖이라 두지만,
    //     실무라면 프로토콜을 u64 로 넓히거나 세션 id 에 세대 비트를 넣는다.
    inline uint32_t sid32(const net::Session& s) {
        return static_cast<uint32_t>(s.id);
    }

    // body 길이가 규약과 다르면 끊는다. 언제나 false 를 돌려준다.
    //
    // 길이를 「이상」이 아니라 「같음」으로 본다. 남는 바이트가 있다는 건 이 클라이언트가
    // 다른 규약을 쓴다는 뜻이고, 조용히 무시하면 어긋난 채로 계속 간다.
    //
    // 함수로 뺀 것은 중복 제거가 아니라 로그를 빠뜨릴 자리를 없애기 위해서다. 한때
    // 거래 핸들러 다섯이 전부 로그 없이 false 만 돌려줘서, 운영에서 왜 끊겼는지가
    // 아무 데도 안 남았다. 끊는 것보다 끊었는데 이유가 없는 것이 나쁘다.
    //
    // 모르는 msg_id 와는 다른 판단이다. 저쪽은 프레이밍이 멀쩡하고 버전 스큐일 수 있어
    // 봐주지만, 이쪽은 아는 메시지인데 모양이 다른 것이라 봐줄 근거가 없다.
    bool bad_body(const net::Session& session, const char* what, int got, int want) {
        core::logf("[WARN] #%llu %s body=%d (want %d) — closing\n",
            static_cast<unsigned long long>(session.id), what, got, want);
        return false;
    }

    // 거래 번호 발급 — 한때 Zone::next_trade_id 가 존마다 따로 셌다. 거래가
    // 세션 소유로 바뀌며 그 카운터가 존과 함께 사라져, 감사 로그 대조용
    // 표식을 전역 atomic 하나로 대신한다. 유일하지만(예전엔 존마다 겹쳤다)
    // 여전히 키가 아니라 표식이다 — 재시작하면 다시 1부터다.
    std::atomic<uint32_t> g_next_trade_id{ 1 };

    // 거래 정리 공통 흐름 — 취소·존 이동·접속 종료 세 갈래가 그대로 쓴다
    // (거절은 수락/거절 분기가 같은 락 구간을 나눠 써야 해서 handle_trade_answer
    // 안에 인라인으로 남는다 — 그 함수 주석 참고).
    //
    // §7-3-A 의 유일한 재검증이 여기 있다 — 내 trade 를 락 없이 읽어 상대의
    // 세션 id 를 알아낸 다음(peer_sid_of), entry.find_acquire_by_session_id 로
    // 명부 경유로만 상대를 확보하고(락 밖 raw 포인터 반출·역참조는 하지
    // 않는다 — trade.h 참고), 둘 다 잠근 뒤에야 그 사이 내 trade 가 바뀌지
    // 않았는지 다시 본다. 바뀌었으면(상대가 먼저 지웠거나 다른 거래로
    // 넘어갔다) 포기하고 상대 홀드를 돌려준다. 성공하면 상대를 쥔 채로
    // 돌려준다 — 호출자가 락 밖에서 통지를 보내고 나서 release 한다.
    //
    // 상대가 이미 명부를 떠난 경우(find_acquire_by_session_id 가 nullptr) —
    // 상대의 gone-Job 정리가 먼저 도는 중이라는 뜻이다. 이때는 내 game_mutex
    // 만 잡고 재검증한 뒤 내 trade 만 지운다. 상대 쪽 필드는 소멸하는
    // 세션의 atomic<shared_ptr> 소멸자가 스스로 놓으므로 여기서 손댈 이유가
    // 없고, 통지도 필요 없다(상대는 죽는 중이라 받을 사람이 없다).
    net::Session* clear_trade(net::IocpServer& server, app::EntryTable& entry, net::Session& session) {
        std::shared_ptr<world::Trade> t = session.trade.load();
        if (t == nullptr) {
            return nullptr;
        }
        const uint64_t peer_sid = t->peer_sid_of(&session);
        if (peer_sid == 0) {
            return nullptr;   // has() 가 원래 보장하므로 도달하지 않는다 — trade.h 의 방어 승계
        }

        net::Session* peer = entry.find_acquire_by_session_id(peer_sid);
        if (peer == nullptr) {
            core::LockRankGuard rank(core::LockRank::kSession);
            std::lock_guard<std::mutex> lk(session.game_mutex);
            if (session.trade.load() == t) {
                session.trade.store(nullptr);
            }
            return nullptr;
        }

        core::LockRankGuard rank(core::LockRank::kSession);
        std::scoped_lock lk(session.game_mutex, peer->game_mutex);

        if (session.trade.load() != t) {
            server.release_session(peer);
            return nullptr;
        }
        session.trade.store(nullptr);
        peer->trade.store(nullptr);
        return peer;
    }

    // 메시지 핸들러 — 한 메시지가 한 함수다. 전부 on_frame 과 같은 계약을 쓴다:
    // true 면 처리했으니 세션은 계속 가고, false 면 끊는다.
    //
    // 전부 이 세션의 직렬 큐 실행권 안에서 돈다(app::WorkerPool) — 그래서 존을
    // 알아야 하는 핸들러는 session.zone 을 직접 읽는다. 실행권이 세션당 하나뿐
    // 이라 그 읽기가 다른 실행과 절대 겹치지 않는다.
    //
    // 인자는 필요한 것만 받는다. 서명이 곧 그 메시지의 의존 범위다 — 에코는 존도 DB 도
    // 모르고 거래 확정만 DB 를 안다. 한 덩어리였을 때는 그 차이가 안 보였다.

    // ── 입장 = 세션 서버가 넘겨준 예약을 소비해 신원을 확정한다 ──────────
    //
    //   세션에 신원을 한 번 박고 그 뒤로는 세션 것만 쓴다. 예약 없이 이
    //   신원을 세션에 박을 방법은 없다 — Reserve 로 미리 넘겨 둔 값과
    //   대조해야만 여기를 통과한다.
    bool handle_enter(net::IocpServer& server, app::EntryTable& entry,
        net::Session& session, const char* body, int body_len) {
        if (body_len != 8) {
            return bad_body(session, "enter", body_len, 8);
        }
        const uint64_t player_id =
            proto::read_u64_be(reinterpret_cast<const uint8_t*>(body));

        proto::ResultCode result = proto::ResultCode::kOk;

        if (player_id == 0) {
            // 0 은 「입장 안 함」의 표식이라 실제 id 로 쓸 수 없다 —
            //   표식과 값이 같은 공간을 쓰면 언젠가 헷갈린다.
            result = proto::ResultCode::kInvalidArg;
        }
        else if (session.player_id != 0) {
            // 이미 입장했다. 갈아끼우지 않는다 — 거래 중에 신원이 바뀌면
            //   그 거래가 누구 것인지 알 수 없게 된다. 「한 번 정해진 것은
            //   세션이 끝날 때까지 안 바뀐다」가 더 지키기 쉽다.
            core::logf("[WARN] #%llu already entered as %llu\n",
                static_cast<unsigned long long>(session.id),
                static_cast<unsigned long long>(session.player_id));
            result = proto::ResultCode::kInvalidArg;
        }
        else if (entry.draining()) {
            // kBusy 를 재사용한다 — 일시 상태·재시도 계열의 값이고(packet.h
            // 「kBusy 서버가 지금 못 받는다. 재시도가 의미 있다」), 드레인
            // 거절에 대한 클라의 반응(세션 서버로 돌아가 재배정받는다)이 정확히
            // 그것이다. 예약을 소비하지 않는다 — 드레인이 풀리면 같은 예약으로
            // 다시 Enter 할 수 있어야 하기 때문이다.
            result = proto::ResultCode::kBusy;
        }
        else if (!entry.consume_reservation(player_id, world::now_ms())) {
            // 예약이 없거나 만료됐다. 잘못 보낸 것이지 공격이 아니므로 끊지 않는다.
            core::logf("[WARN] #%llu enter %llu — 예약 없음/만료\n",
                static_cast<unsigned long long>(session.id),
                static_cast<unsigned long long>(player_id));
            result = proto::ResultCode::kInvalidArg;
        }
        else if (!entry.enter(player_id, session)) {
            // 이미 그 player_id 로 입장한 세션이 있다. 중복 감지는 여기까지고
            // 기존 세션을 끊는 것(Kick)은 이 단계의 범위가 아니다 — 새로 온
            // 쪽을 거절한다. 예약은 이미 소비됐다 — 예약이 1회용이라 그게 맞다.
            core::logf("[WARN] #%llu enter %llu — 이미 입장 중\n",
                static_cast<unsigned long long>(session.id),
                static_cast<unsigned long long>(player_id));
            result = proto::ResultCode::kInvalidArg;
        }
        else {
            session.player_id = player_id;

            // 명부 등록 = 존 멤버십 홀드 하나 — 예전엔 첫 JoinZone 에서 잡았지만,
            //   존 이동과 무관하게 "이 세션이 명부에 있다"는 사실 하나로
            //   통합했다. leave() 가 성공할 때(아래 또는 on_session_gone)
            //   정확히 한 번 반납된다.
            server.acquire_session(session);

            // PlayerEnter 통지는 이제 이 자리에서 직접 안 보낸다 — entry.enter()
            //   가 자신의 임계구역 안에서 notifier 를 이미 불렀다.
            core::logf("[ENTER] #%llu -> player=%llu\n",
                static_cast<unsigned long long>(session.id),
                static_cast<unsigned long long>(player_id));

            // idle 스윕이 이 Enter 실행보다 먼저 이 세션의 직렬 큐에 정리 Job 을
            //   넣었을 수 있다 — close_session 이 closing 을 세우는 것은
            //   session_gone_ 콜백(정리 Job 을 이 세션의 직렬 큐에 제출)보다
            //   코드 순서상 먼저다. 정리 Job 은 그때 player_id==0 이라
            //   entry.leave() 를 건너뛰고 지나갔으므로, 뒤이어 도착한 이 Enter 가
            //   방금 넣은 항목을 스스로 되돌리지 않으면 entered_ 에 영구히 남아
            //   그 player_id 는 서버 재시작까지 재입장이 막힌다. 둘 다 같은
            //   세션의 직렬 큐에 들어가고 직렬 큐가 FIFO 로 순차 실행하므로,
            //   정리 Job 이 이미 지나간 뒤라면 closing 은 반드시 true 로 보인다.
            // 반대 방향(closing 만 먼저 서고 정리 Job 은 아직 직렬 큐에 없음)도
            //   실재한다 — exchange 와 제출 사이의 창이다. 그때 여기서
            //   되돌려도 안전하다. 뒤늦게 도착한 정리 Job 은 이미 지워진 것을
            //   보고 leave() 가 false 를 내므로 반납·통지가 두 번 나가지 않는다.
            //   반대로 closing 이 false 면 이 세션은 아직 살아 있고, 나중에
            //   진짜로 닫힐 때 정리 Job 이 player_id 를 실값으로 읽어 정상
            //   경로로 치운다.
            if (session.closing.load()) {
                if (entry.leave(player_id, session.id).ok) {
                    server.release_session(&session);
                }
            }
        }

        // 실패 갈래에서는 상태를 하나도 안 남긴다 — session.player_id 는 0
        //   그대로고 입장 집합에도 안 들어간다. 그래서 세션도 끊지 않는다.
        // 되돌려 주는 player_id 는 「서버가 인정한 값」이라 실패면 0 이다.
        //   session_id 는 성공·실패 어느 쪽이든 실값이다 — 실패해도 자기
        //   id 는 알아야 거래에서 상대를 지목할 수 있다.
        char ack[13];
        ack[0] = static_cast<char>(result);
        const uint64_t out_player_id =
            (result == proto::ResultCode::kOk) ? session.player_id : 0;
        proto::write_u64_be(reinterpret_cast<uint8_t*>(ack) + 1, out_player_id);
        proto::write_u32_be(reinterpret_cast<uint8_t*>(ack) + 9, sid32(session));
        return send_msg(server, session, proto::MsgId::kEnterAck, ack, 13);
    }

    // ── 존 입장 = 명부(L1) 갱신 하나로 끝난다 ─────────────────────────
    //
    //  예전엔 두 존 스레드에 걸친 CAS + Job 인계로 상호 배제를 만들었다.
    //  지금은 이 세션의 직렬 큐 실행권 자체가 유일한 동시 접근자라
    //  (app::WorkerPool) 그 상호 배제가 구조적으로 공짜다 — 이 프레임이
    //  도는 동안 같은 세션의 다른 프레임이 동시에 도는 일이 없다.
    bool handle_join_zone(net::IocpServer& server, app::EntryTable& entry,
        net::Session& session, const char* body, int body_len) {
        if (body_len != 4) {
            return bad_body(session, "join_zone", body_len, 4);
        }
        const uint32_t want =
            proto::read_u32_be(reinterpret_cast<const uint8_t*>(body));

        // 존을 떠나면 거래도 끝난다. 거래는 세션이 직접 소유하므로(net::Session::trade)
        // 존 번호와 무관하게 clear_trade 하나로 정리한다(정리 지점 3/4).
        if (net::Session* tp = clear_trade(server, entry, session)) {
            send_peer_ntf(server, *tp, proto::MsgId::kTradeCancelNtf, sid32(session));
            server.release_session(tp);
        }

        // L1(unique) — 옛 버킷 제거(있었으면) + 새 버킷 등록 + zone/in_zone 갱신을
        // EntryTable::move_zone 안에서 한 임계구역으로 끝낸다(완전 이전 1회). 두
        // 단계로 나누면 「인덱스만 먼저 새 존」인 중간 창이 생겨, snapshot_zone 을
        // 쓰는 다른 세션의 실행이 JoinZoneAck 전에 새 존 Chat 을 이 세션에 보낼
        // 수 있다.
        const auto move_result = entry.move_zone(session, want);
        if (!move_result.ok) {
            // 명부에 없다 — Enter 를 안 했거나, 이 세션의 정리 Job(직렬 큐 맨 뒤에
            // 서도록 보장된다)이 이미 돌아 leave() 가 먼저 끝난 경우다. 어느
            // 쪽이든 옮길 대상이 없다.
            core::logf("[WARN] #%llu join %u 무시 — 명부에 없다\n",
                static_cast<unsigned long long>(session.id),
                static_cast<unsigned>(want));
            return true;
        }

        // move_zone 이 이미 인덱스를 새 존으로 옮긴 뒤다 — 이 세션 자신의 라벨을
        // 뒤따라 맞춘다. 직렬 큐 직렬화 안이라 락이 필요 없다(session.h 참조).
        session.zone = want;

        uint8_t ack[8];
        proto::write_u32_be(ack + 0, want);
        proto::write_u32_be(ack + 4, entry.zone_size(want));
        send_msg(server, session, proto::MsgId::kJoinZoneAck,
            reinterpret_cast<const char*>(ack), 8);

        // ack 「뒤에」 보낸다 — 먼저 보내면 클라이언트가 「어느 존의 목록인지」를
        //   모르는 상태로 받는다. ack 의 zone_id 가 그 문맥을 준다.
        //   본인도 이 브로드캐스트로 목록을 처음 받는다 (따로 보내지 않는다).
        broadcast_zone_members(server, entry, want);
        if (move_result.had_prev) {
            // 옛 존에 남은 사람들도 갱신을 받는다 — move_zone 이 이미 옛
            // 버킷에서 이 세션을 뺀 뒤라 목록에서 정확히 빠져 있다.
            broadcast_zone_members(server, entry, move_result.prev_zone);
        }
        return true;
    }

    // ── 채팅 = Zone·All·Whisper 세 갈래 ───────────────────────────────
    bool handle_chat(net::IocpServer& server, app::EntryTable& entry,
        net::Session& session, const char* body, int body_len) {
        // 길이 검증이 type 읽기보다 먼저다 — 순서를 바꾸면 빈 body 로 온
        // 신형 kChatReq 의 body[0] 읽기가 OOB 다. 막는 것은 두 갈래다 —
        // ① body[0] 읽기 자체의 OOB ② All 경로에서 text_len = body_len - 1
        // 이 음수가 돼 build_frame 의 합산·복사가 어긋나며 나는 1바이트 힙
        // 오버런(합산·복사를 같은 조건으로 통일해 지금은 그 클래스 자체가
        // 안 나지만, 이 검사가 애초에 body_len 을 양수로 묶어 둔다).
        if (body_len < 1) {
            core::logf("[WARN] #%llu chat body=%d (want >=1) — closing\n",
                static_cast<unsigned long long>(session.id), body_len);
            return false;
        }
        const uint8_t type = static_cast<uint8_t>(body[0]);
        if (type > 2) {
            // 미정의 type 은 프로토콜 위반으로 본다 — 클라 버전이 서버보다
            // 앞선 것으로 봐주지 않는다. 프로토타입 단계라 전방 호환보다
            // 엄격성을 택했다(D1). 절단은 「위반일 때만」이라는 CODING_RULES
            // §3 의 정방향 적용이다.
            core::logf("[WARN] #%llu chat type=%u undefined — closing\n",
                static_cast<unsigned long long>(session.id),
                static_cast<unsigned>(type));
            return false;
        }

        if (type == 2) {
            if (body_len < 9) {
                core::logf("[WARN] #%llu chat whisper body=%d (want >=9) — closing\n",
                    static_cast<unsigned long long>(session.id), body_len);
                return false;
            }
            const uint64_t target =
                proto::read_u64_be(reinterpret_cast<const uint8_t*>(body) + 1);

            net::Session* peer = entry.find_acquire_by_player_id(target);
            if (peer == nullptr) {
                // 상대가 없다 — 게임 로직 실패지 프로토콜 위반이 아니다.
                // 연결은 유지하고 실패만 알린다(D3 — kTradeReq 의 kNoPeer 와
                // 같은 이유로 재사용한다).
                const char rc = static_cast<char>(proto::ResultCode::kNoPeer);
                return send_msg(server, session, proto::MsgId::kChatAck, &rc, 1);
            }

            // 텍스트 상한 재계산이 여기 없다 — 요청 오버헤드(type 1B +
            // target 8B = 9B)와 통지 오버헤드(type 1B + from 8B = 9B)가
            // 폭이 같아서다. 수신 시점에 kMaxBodySize 를 통과한 입력은
            // 이 그대로도 상한 이하다 — 아래 Zone/All 처럼 오버헤드가
            // 늘어나는 쪽만 다시 계산할 이유가 있다.
            const int text_len = body_len - 9;
            uint8_t head[9];
            head[0] = type;
            proto::write_u64_be(head + 1, session.player_id);

            const net::SendChunk parts[2] = {
                { reinterpret_cast<const char*>(head), 9 },
                { body + 9, text_len },
            };
            const std::vector<char> frame = build_frame(proto::MsgId::kChatNtf, parts, 2);

            // 자기 자신을 target 으로 허용한다 — 막을 손해가 없다(§18-7).
            // 송신 성공·실패 어느 쪽이든 대상 홀드는 정확히 1회 반납한다 —
            // 반환값을 안 본다.
            server.send_frame(*peer, frame.data(), static_cast<int>(frame.size()));
            server.release_session(peer);
            return true;
        }

        // Zone(0)·All(1) — 스냅샷 대상만 다르고 나머지는 같다. Zone::has 를
        // 대신하던 자리다 — 명부의 인덱스에서 스냅샷을 뜨고, 그 안에 자기
        // 자신이 있는지로 「아직 입장 전」을 가린다. 이 스냅샷을 그대로
        // 브로드캐스트에도 쓴다 — 두 번 뜰 이유가 없다.
        std::vector<net::Session*> members;
        if (type == 0) {
            entry.snapshot_zone(session.zone, members);
        } else {
            entry.snapshot_all(members);
        }

        const bool is_member =
            std::find(members.begin(), members.end(), &session) != members.end();
        if (!is_member) {
            // session.zone 이 아직 어느 존에도 등록 안 된 상태(Enter 직후,
            // 존 미가입)에서 온 Zone 패킷 — 조용히 버린다. All 은 이 핸들러에
            // 오는 시점에 이미 게이트(player_id!=0)를 지났으니 입장 자체는
            // 끝나 있어, 이 갈래에 사실상 도달하지 않는다.
            for (net::Session* m : members) { server.release_session(m); }
            return true;
        }

        // 받은 것을 그대로 되보내는 게 아니라 type+from(u64) 를 덧붙이므로
        // 상한을 「다시」 계산해야 한다 — 받을 때 통과했다고 보낼 때도
        // 통과하는 게 아니다. 오버헤드가 요청의 1B(type)에서 통지의 9B
        // (type+from)로 늘어난 만큼을 뺀다(Whisper 는 이 재계산이 필요
        // 없는 이유가 위에 있다).
        const int text_len = body_len - 1;
        if (text_len > static_cast<int>(proto::kMaxBodySize) - 9) {
            core::logf("[WARN] #%llu chat too long to relay\n",
                static_cast<unsigned long long>(session.id));
            for (net::Session* m : members) { server.release_session(m); }
            return false;
        }

        uint8_t head[9];
        head[0] = type;
        proto::write_u64_be(head + 1, session.player_id);

        const net::SendChunk parts[2] = {
            { reinterpret_cast<const char*>(head), 9 },
            { body + 1, text_len },
        };
        const std::vector<char> frame = build_frame(proto::MsgId::kChatNtf, parts, 2);

        // 실패를 무시한다. 남의 세션을 여기서 끊으면 「누가 정리하는가」가
        // 흩어진다. 다만 실패에 두 종류가 있고 한쪽만 정리된다 — 소켓이
        // 죽은 경우는 걸려 있던 I/O 가 실패로 돌아와 정리되지만, 송신
        // 큐가 넘친 경우는 I/O 실패가 아니라 아무도 안 끊는다. 상대가 안
        // 읽어 큐가 상한까지 차면 그 세션은 아무것도 못 받으면서 존
        // 멤버로 남는다 — idle_timeout_sec 커밋값은 90(ADR-023)이라
        // "기본이 꺼짐"은 이제 사실이 아니지만, 그 타임아웃은 무응답
        // 연결을 잡는 것이지 큐 자체가 찬 세션을 잡는 것이 아니다. 큐
        // 넘침 쪽은 §17-6 킥이 따로 맡는다.
        for (net::Session* m : members) {
            server.send_frame(*m, frame.data(), static_cast<int>(frame.size()));
        }
        for (net::Session* m : members) {
            server.release_session(m);
        }
        return true;
    }

    // ── 거래 요청 거래창의 입구 ────────────────────────────────
    bool handle_trade_req(net::IocpServer& server,
        app::EntryTable& entry, net::Session& session, const char* body, int body_len) {
        if (body_len != 4) {
            return bad_body(session, "trade_req", body_len, 4);
        }
        const uint32_t to_id =
            proto::read_u32_be(reinterpret_cast<const uint8_t*>(body));

        // on_frame 의 게이트가 이미 player_id == 0 을 끊으므로 클라 경로로는
        //   이 분기에 못 온다. 그래도 지우지 않는다 — 게이트 배선이 실수로
        //   빠지면 이 검사가 그 자리를 대신 지켜야 한다.
        if (session.player_id == 0) {
            return send_trade_result(server, session,
                proto::ResultCode::kNotLoggedIn, to_id);
        }

        // 이 스냅샷은 아래 상대 탐색의 「같은 존인가」 검증에도 재사용한다.
        std::vector<net::Session*> zone_members;
        entry.snapshot_zone(session.zone, zone_members);
        const bool self_in_zone =
            std::find(zone_members.begin(), zone_members.end(), &session) != zone_members.end();
        if (!self_in_zone) {
            for (net::Session* m : zone_members) { server.release_session(m); }
            return true;                // 존 미가입 상태다. 조용히 버린다 (kChatReq 와 같은 판단)
        }

        // 세션당 거래는 하나 — 락 밖에서 보는 빠른 선점검이다. 진짜 판정은
        //   아래 scoped_lock 아래 양쪽 trade 를 다시 본다(§7-3 TradeReq 시퀀스).
        if (session.trade.load() != nullptr) {
            for (net::Session* m : zone_members) { server.release_session(m); }
            return send_trade_result(server, session,
                proto::ResultCode::kBusy, to_id);
        }

        // member_by_id 가 명부로 옮겨가며 존 무관 선형 스캔이 됐다(find_acquire_by_session_id —
        //   6단계 인덱스 후보). 「이 존의 멤버 중에서만」 이라는 원래 검증은
        //   위에서 이미 뜬 zone_members 로 다시 확인한다 — 거래 로직 자체는
        //   지금은 안 바꾼다(요청자와 상대가 같은 존이어야 한다는 규칙은 유지한다).
        //   명부의 락 모델 자체는 존 무관 거래를 이미 허용한다 — 존을 넘어
        //   거래를 트는 것은 락이 막아서가 아니라 아직 그렇게 하기로 정하지
        //   않아서다. 그 결정은 이 재확인과 별개다.
        net::Session* peer = entry.find_acquire_by_session_id(to_id);
        const bool peer_in_zone = (peer != nullptr) &&
            (std::find(zone_members.begin(), zone_members.end(), peer) != zone_members.end());
        for (net::Session* m : zone_members) { server.release_session(m); }

        if (peer == &session) {
            // 자기 자신은 kNoPeer 가 아니라 kInvalidArg 다 —
            //   「상대가 없다」가 아니라 「클라이언트가 잘못 보냈다」이고,
            //   재시도해도 결과가 같다. 반응이 다르면 코드도 달라야 한다.
            server.release_session(peer);
            return send_trade_result(server, session,
                proto::ResultCode::kInvalidArg, to_id);
        }
        // 앞 절반(nullptr 이거나 다른 존)은 살아 있다 — 다른 존 사람을 지목하면
        //   여기 걸린다. 뒤 절반(peer->player_id == 0)은 게이트가 생긴 뒤로
        //   클라 경로에서 도달 불가다. 존에 들어오려면 kJoinZoneReq 를 통과해야
        //   하고 그것이 게이트 뒤에 있으므로, 존 멤버는 전부 신원이 선 세션이다.
        //   같은 이유로 지우지 않는다.
        if (peer == nullptr || !peer_in_zone || peer->player_id == 0) {
            if (peer != nullptr) { server.release_session(peer); }
            return send_trade_result(server, session,
                proto::ResultCode::kNoPeer, to_id);
        }
        // 세션이 달라도 같은 계정이면 거래할 수 없다 (중복 로그인).
        //   같은 player_id 는 DB 에서 「같은 행」이라, 차감과 지급이 한 행에
        //   겹쳐 락 순서 정렬이 무의미해진다. db 층에서도 다시 막지만,
        //   여기서 걸러야 사용자에게 빨리 알려줄 수 있다.
        if (peer->player_id == session.player_id) {
            server.release_session(peer);
            return send_trade_result(server, session,
                proto::ResultCode::kInvalidArg, to_id);
        }
        if (peer->trade.load() != nullptr) {
            server.release_session(peer);
            return send_trade_result(server, session,
                proto::ResultCode::kBusy, to_id);
        }

        // L1(상대 acquire)은 이미 위 find_acquire_by_session_id 에서 끝났다 —
        // 그 홀드만 들고 L1 은 놓은 채로 L2 시퀀스로 들어간다(§7-3 TradeReq 행).
        // 양쪽 trade 를 여기서 다시 보는 것이 진짜 판정이다 — 위 선점검과 이
        // 지점 사이에 다른 스레드가 둘 중 하나를 먼저 거래에 물렸을 수 있다.
        bool ok = false;
        {
            core::LockRankGuard rank(core::LockRank::kSession);
            std::scoped_lock lk(session.game_mutex, peer->game_mutex);
            if (session.trade.load() == nullptr && peer->trade.load() == nullptr) {
                auto t = std::make_shared<world::Trade>();
                t->id = g_next_trade_id.fetch_add(1, std::memory_order_relaxed);
                t->a = &session;
                t->b = peer;
                t->a_sid = session.id;
                t->b_sid = peer->id;
                session.trade.store(t);
                peer->trade.store(t);
                ok = true;
            }
        }
        if (!ok) {
            server.release_session(peer);
            return send_trade_result(server, session,
                proto::ResultCode::kBusy, to_id);
        }

        // from 은 서버가 채운다 — 요청자가 자기를 사칭할 수 없다.
        // 요청자에게는 아무것도 안 보낸다. 성공은 상대의 다음 응답이 증명한다.
        const bool sent = send_peer_ntf(server, *peer,
            proto::MsgId::kTradeReqNtf, sid32(session));
        server.release_session(peer);
        return sent;
    }

    // ── 요청에 답한다 (수락 · 거절) ────────────────────────────────
    bool handle_trade_answer(net::IocpServer& server, app::EntryTable& entry,
        net::Session& session, const char* body, int body_len) {
        if (body_len != 5) {
            return bad_body(session, "trade_answer", body_len, 5);
        }
        const uint32_t from_id =
            proto::read_u32_be(reinterpret_cast<const uint8_t*>(body));
        const bool accept = (body[4] != 0);

        std::shared_ptr<world::Trade> t = session.trade.load();
        if (t == nullptr) {
            return send_trade_result(server, session, proto::ResultCode::kNoPeer, from_id);
        }
        const uint64_t peer_sid = t->peer_sid_of(&session);
        if (peer_sid == 0) {
            return send_trade_result(server, session, proto::ResultCode::kNoPeer, from_id);
        }

        net::Session* peer = entry.find_acquire_by_session_id(peer_sid);
        if (peer == nullptr) {
            // 상대가 이미 명부를 떠났다(gone-Job 정리 진행 중) — clear_trade 의
            //   단독 갈래와 같은 재검증으로 내 trade 만 지운다. kBusy — 정리가
            //   끝나는 대로 다시 시도하면 되는 일시 상태다.
            core::LockRankGuard rank(core::LockRank::kSession);
            std::lock_guard<std::mutex> lk(session.game_mutex);
            if (session.trade.load() == t) {
                session.trade.store(nullptr);
            }
            return send_trade_result(server, session, proto::ResultCode::kBusy, from_id);
        }

        // 수락/거절이 같은 락 구간에서 갈리므로 clear_trade 를 그대로 못 쓴다 —
        // 거절은 지우고, 수락은 연다. 아래 재검증·조건은 둘이 공유한다.
        world::Trade snap{};
        bool ok = false;
        bool closed = false;
        {
            core::LockRankGuard rank(core::LockRank::kSession);
            std::scoped_lock lk(session.game_mutex, peer->game_mutex);

            // §7-3-A 유일 재검증 — 그 사이 t 가 바뀌었으면(상대가 먼저
            //   취소했거나 다른 거래로 넘어갔다) 포기한다. 「내가 받은
            //   요청인가」도 여기서 같이 본다 — t->b != &session 이면
            //   요청한 쪽이 자기 요청을 자기가 수락하는 것이고, from_id
            //   불일치는 「내가 아는 그 요청」이 아니다. open 을 락 밖에서
            //   읽으면 다른 스레드의 accept 와 경합하므로 반드시 락 안에서 본다.
            if (session.trade.load() == t && !t->open
                && t->b == &session && sid32(*t->a) == from_id) {
                ok = true;
                if (accept) {
                    t->open = true;
                } else {
                    session.trade.store(nullptr);
                    peer->trade.store(nullptr);
                    closed = true;
                }
                snap = *t;
            }
        }

        if (!ok) {
            server.release_session(peer);
            return send_trade_result(server, session, proto::ResultCode::kNoPeer, from_id);
        }
        if (closed) {
            send_peer_ntf(server, *peer, proto::MsgId::kTradeCancelNtf, sid32(session));
            server.release_session(peer);
            return true;
        }

        // 창이 열렸다는 사실과 초기 상태를 양쪽에 함께 보낸다.
        //   상태를 안 보내면 클라이언트가 「빈 창」을 스스로 가정해야 한다.
        send_peer_ntf(server, *snap.a, proto::MsgId::kTradeOpenNtf, sid32(*snap.b));
        send_peer_ntf(server, *snap.b, proto::MsgId::kTradeOpenNtf, sid32(*snap.a));
        broadcast_trade_state(server, snap);
        server.release_session(peer);
        return true;
    }

    // ── 슬롯에 올린다 / 내린다 막판 바꿔치기 방어가 여기 있다 ──
    bool handle_trade_set_item(net::IocpServer& server, app::EntryTable& entry,
        net::Session& session, const char* body, int body_len) {
        if (body_len != 8) {
            return bad_body(session, "trade_set_item", body_len, 8);
        }
        const uint8_t* p = reinterpret_cast<const uint8_t*>(body);
        uint32_t item = proto::read_u32_be(p);
        uint32_t count = proto::read_u32_be(p + 4);

        std::shared_ptr<world::Trade> t = session.trade.load();
        if (t == nullptr) {
            return send_trade_result(server, session, proto::ResultCode::kNoPeer, 0);
        }
        // peer_sid_of 는 정수 비교라 상대의 생존 여부와 무관하게 항상 안전하다 —
        //   실제 확보(find_acquire_by_session_id)는 아래 검증 뒤로 미룬다.
        const uint64_t peer_sid = t->peer_sid_of(&session);
        if (peer_sid == 0) {
            return send_trade_result(server, session, proto::ResultCode::kNoPeer, 0);
        }

        // 3중 방어의 1층 — 터무니없는 값을 여기서 거른다.
        //   실제로 막는 건 DB 의 조건부 UPDATE(2층)이고,
        //   이 층은 「응답을 빨리 돌려주기」 위한 것이다. 여기서만 막으면 안 된다.
        //   peer_sid 는 sid32 가 하는 것과 같은 하위 32비트 절단이다 — 아직
        //   상대를 확보하지 않았으니 역참조 없이 응답만 채운다.
        if (count > proto::kMaxTradeCount) {
            return send_trade_result(server, session, proto::ResultCode::kInvalidArg,
                static_cast<uint32_t>(peer_sid));
        }
        if (count == 0) {
            item = 0;                   // 「내림」을 한 가지 모양으로 정규화한다
        }

        net::Session* peer = entry.find_acquire_by_session_id(peer_sid);
        if (peer == nullptr) {
            // 상대가 이미 명부를 떠났다 — clear_trade 의 단독 갈래와 같다.
            core::LockRankGuard rank(core::LockRank::kSession);
            std::lock_guard<std::mutex> lk(session.game_mutex);
            if (session.trade.load() == t) {
                session.trade.store(nullptr);
            }
            return send_trade_result(server, session, proto::ResultCode::kBusy, 0);
        }

        world::Trade snap{};
        bool ok = false;
        {
            core::LockRankGuard rank(core::LockRank::kSession);
            std::scoped_lock lk(session.game_mutex, peer->game_mutex);

            // §7-3-A 유일 재검증 — open 도 여기서 같이 본다(락 밖에서 읽으면
            //   accept 와 경합한다).
            if (session.trade.load() == t && t->open) {
                if (&session == t->a) { t->a_item = item; t->a_count = count; }
                else { t->b_item = item; t->b_count = count; }

                // 여기가 막판 바꿔치기 방어다.
                //   상대가 확인을 누른 뒤 내가 슬롯을 바꿔치기하면,
                //   상대는 자기가 본 적 없는 거래에 동의한 것이 된다.
                //   「양쪽」을 지운다. 바꾼 쪽의 확인이 남아도 같은 사고다.
                t->reset_confirm();
                snap = *t;
                ok = true;
            }
        }

        if (!ok) {
            server.release_session(peer);
            return send_trade_result(server, session, proto::ResultCode::kNoPeer, 0);
        }

        broadcast_trade_state(server, snap);
        server.release_session(peer);
        return true;
    }

    // ── 확인 토글 둘 다여야 DB 로 간다 ──────────────────────────
    //
    // §7-4 — 커넥션은 락보다 먼저 빌린다. try_acquire 를 이 함수 맨 앞에서
    // 부르는 것은 「이 토글이 실제로 양쪽 확인을 완성시킬지」를 아직 몰라서다 —
    // 완성되는 순간 그대로 DB 를 타야 하는데, 그때는 이미 game_mutex 를 잡은
    // 뒤라 커넥션을 구하러 가면 락을 든 채 대기하게 된다(§7-4 가 막으려는
    // 바로 그 순서). 그래서 확인 여부와 무관하게 미리 빌리고, 안 쓰면 Lease
    // 소멸자가 그대로 돌려준다(풀 왕복 하나뿐이라 비용은 작다).
    bool handle_trade_confirm(net::IocpServer& server, db::DbPool& db_pool,
        app::EntryTable& entry, net::Session& session, const char* body, int body_len) {
        if (body_len != 1) {
            return bad_body(session, "trade_confirm", body_len, 1);
        }
        const bool on = (body[0] != 0);

        bool open_failed = false;
        db::DbPool::Lease lease = db_pool.try_acquire(&open_failed);
        if (!lease) {
            // 실패 즉시 응답 — 통지는 없다(재검증도 스냅샷도 아직 안 만들었으니
            // 상대에게 알릴 「상태 변화」자체가 없다). 관측은 [POOL2] try_failed·
            // open_failed. 풀 고갈(재시도 유의미)과 DB 다운(운영 신호)을 가른다 —
            // db_pool.h 의 try_acquire 주석 참고.
            return send_trade_result(server, session,
                open_failed ? proto::ResultCode::kDbError : proto::ResultCode::kBusy, 0);
        }

        std::shared_ptr<world::Trade> t = session.trade.load();
        if (t == nullptr) {
            return send_trade_result(server, session, proto::ResultCode::kNoPeer, 0);
        }
        const uint64_t peer_sid = t->peer_sid_of(&session);
        if (peer_sid == 0) {
            return send_trade_result(server, session, proto::ResultCode::kNoPeer, 0);
        }

        net::Session* peer = entry.find_acquire_by_session_id(peer_sid);
        if (peer == nullptr) {
            // 상대가 이미 명부를 떠났다 — clear_trade 의 단독 갈래와 같다.
            core::LockRankGuard rank(core::LockRank::kSession);
            std::lock_guard<std::mutex> lk(session.game_mutex);
            if (session.trade.load() == t) {
                session.trade.store(nullptr);
            }
            return send_trade_result(server, session, proto::ResultCode::kBusy, 0);
        }

        world::Trade snap{};
        bool ok = false;
        bool both = false;
        {
            core::LockRankGuard rank(core::LockRank::kSession);
            std::scoped_lock lk(session.game_mutex, peer->game_mutex);

            // §7-3-A 유일 재검증 — open 도 락 안에서 본다(Answer 의 accept 와 경합).
            if (session.trade.load() == t && t->open) {
                ok = true;
                // 토글인 이유 — 눌렀다가 마음을 바꿀 수 있어야 한다.
                //   되돌릴 수 없는 확인은 사용자가 무서워서 못 누른다.
                if (&session == t->a) { t->a_confirm = on; } else { t->b_confirm = on; }
                both = t->both_confirmed();
                if (both) {
                    // 정리 지점 (2/4) — 거래는 여기서 끝난다.
                    //   이 뒤로 t 는 무효라, 필요한 값은 아래 스냅샷에 다 복사해 둔다.
                    session.trade.store(nullptr);
                    peer->trade.store(nullptr);
                }
                snap = *t;
            }
        }

        if (!ok) {
            server.release_session(peer);
            return send_trade_result(server, session, proto::ResultCode::kNoPeer, 0);
        }
        if (!both) {
            broadcast_trade_state(server, snap);
            server.release_session(peer);
            return true;
        }

        // ── 둘 다 확인됐다 → 이 실행 안에서 동기로 DB(1홉) ──────────
        //   예전엔 존 스레드가 db.post_write 로 DB 워커에 넘기고 DB 워커가
        //   zones.post 로 되돌아왔다(3홉). 지금은 이 직렬 큐 실행 자체가
        //   워커 스레드 위에서 돌고 있으므로 그 왕복이 통째로 사라진다 —
        //   빌려 둔 lease 를 여기서 그대로 쓴다.
        net::Session* sa = snap.a;
        net::Session* sb = snap.b;

        db::TradeOrder order;
        order.a_player = sa->player_id;
        order.a_item = snap.a_item;
        order.a_count = snap.a_count;
        order.b_player = sb->player_id;
        order.b_item = snap.b_item;
        order.b_count = snap.b_count;

        // 감사 로그에 남길 대조용 표식. session.zone 은 「이 확인을 처리한 세션이
        //   그 시점에 있던 존」이다 — 라우팅이 사라진 지금도 텍스트 로그와
        //   DB 기록을 사람이 맞춰 보는 용도는 그대로 남아 있다.
        order.zone_id  = session.zone;
        order.trade_id = snap.id;

        db::TradeResult tr = db::TradeResult::kDbError;
        db::DbConn* conn = lease.get();
        if (conn != nullptr) {
            // 일부러 틀리게 만든 대조군. 스위치는 평시 false — kBadTradeNoTx 가
            //   켜진 채로 회귀를 돌리면 결과가 거짓말을 한다.
            tr = kBadTradeNoTx ? conn->trade_unsafe(order) : conn->trade(order);
        }

        // 실패 이유를 반드시 남긴다. 결과 코드만으로는 「무엇이 왜」를 알 수 없다.
        if (tr != db::TradeResult::kOk && conn != nullptr) {
            core::logf("[WARN] trade failed — %s\n", conn->last_error().c_str());
        }

        core::logf("[TRADE] zone=%u trade=%u p%llu[%u x%u] <-> p%llu[%u x%u] result=%d\n",
            static_cast<unsigned>(order.zone_id),
            static_cast<unsigned>(order.trade_id),
            static_cast<unsigned long long>(order.a_player),
            static_cast<unsigned>(order.a_item),
            static_cast<unsigned>(order.a_count),
            static_cast<unsigned long long>(order.b_player),
            static_cast<unsigned>(order.b_item),
            static_cast<unsigned>(order.b_count),
            static_cast<int>(tr));

        const proto::ResultCode rc = to_result(tr);
        send_trade_result(server, *sa, rc, sid32(*sb));
        send_trade_result(server, *sb, rc, sid32(*sa));
        server.release_session(peer);   // peer 는 sa·sb 중 하나 — 그 홀드 하나만 반납한다.
        return true;
    }

    // ── 취소 = 거절과 같은 일을 한다 ──────────────────────────────
    bool handle_trade_cancel(net::IocpServer& server, app::EntryTable& entry,
        net::Session& session, int body_len) {
        if (body_len != 0) {
            return bad_body(session, "trade_cancel", body_len, 0);
        }

        // 거래가 없어도 실패로 보지 않는다. 「없는 상태로 만든다」가 목적이고,
        //   이미 없으면 그 목적은 달성돼 있다 — 멱등이다.
        net::Session* peer = clear_trade(server, entry, session);
        if (peer != nullptr) {
            send_peer_ntf(server, *peer,
                proto::MsgId::kTradeCancelNtf, sid32(session));
            server.release_session(peer);
        }
        return true;
    }

    // ── 인벤토리 조회 = DB 왕복(1홉, 동기) ─────────────────────────
    //
    //  body 를 인자로 받지 않는다. body 의 player_id 를 읽으면 아무나 남의
    //    인벤토리를 들여다본다 — 조회 대상은 클라이언트가 정하는 게 아니라
    //    「이 세션이 누구인가」로 정해진다. 서명에서 아예 뺐다.
    bool handle_inventory(net::IocpServer& server, db::DbPool& db_pool,
        net::Session& session) {
        // on_frame 의 게이트가 이미 player_id == 0 을 끊으므로 클라 경로로는
        //   이 분기에 못 온다. 그래도 지우지 않는다 — 게이트 배선이 실수로
        //   빠지면 이 검사가 그 자리를 대신 지켜야 한다.
        if (session.player_id == 0) {
            const char err[3] = {
                static_cast<char>(proto::ResultCode::kNotLoggedIn), 0, 0
            };
            return send_msg(server, session, proto::MsgId::kInventoryAck, err, 3);
        }

        // 이 실행 자체를 통째로 지연시켜 「동기 DB 질의를 흉내」낸다(부하 주입).
        //   0 이 아니면 try_acquire 조차 건너뛰고 그 자리에서 응답한다 —
        //   워커 하나가 이만큼 묶였을 때 다른 세션(다른 워커)이 영향을 받는지가
        //   이 스위치의 관측 대상이다(zone_block.ps1 재정의가 그 격리를 잰다).
        if constexpr (kBadSyncDbMs != 0) {
            Sleep(kBadSyncDbMs);
            const char empty[3] = { 0, 0, 0 };   // result=OK, count=0
            return send_msg(server, session, proto::MsgId::kInventoryAck, empty, 3);
        }

        // §7-4 — 커넥션은 락보다 먼저 빌린다. 못 빌리면 락을 잡기 전에 kBusy 로
        // 끝낸다(D7 — 실패는 즉시 실패, 재큐잉하지 않는다).
        bool open_failed = false;
        db::DbPool::Lease lease = db_pool.try_acquire(&open_failed);
        if (!lease) {
            const proto::ResultCode rc =
                open_failed ? proto::ResultCode::kDbError : proto::ResultCode::kBusy;
            const char busy[3] = { static_cast<char>(rc), 0, 0 };
            return send_msg(server, session, proto::MsgId::kInventoryAck, busy, 3);
        }

        std::vector<db::InventoryRow> rows;
        proto::ResultCode result = proto::ResultCode::kOk;
        {
            // L2(자기) — 이 세션의 게임 상태 락을 DB 왕복 동안 쥔다(§7-1 「작업
            // 단위 전체 보유 · DB 까지 들고 감」). 지금은 이 세션 혼자 만지는
            // 조회라 실제 경합 상대가 없지만, L2 의 계약은 「게임 상태를 만지는
            // 작업은 전부 이 락 아래」이지 「경합이 있을 때만」이 아니다.
            core::LockRankGuard rank(core::LockRank::kSession);
            std::lock_guard<std::mutex> lock(session.game_mutex);

            db::DbConn* conn = lease.get();
            if (conn == nullptr || !conn->select_inventory(session.player_id, rows)) {
                result = proto::ResultCode::kDbError;
                rows.clear();
                core::logf("[WARN] inventory query failed — %s\n",
                    (conn != nullptr) ? conn->last_error().c_str() : "no connection");
            }
        }

        // 응답 상한. 한 프레임에 못 담을 만큼 많으면 자른다.
        //   페이지네이션이 정답이지만 지금 그 무대가 없다.
        const size_t kMaxRows = (static_cast<size_t>(proto::kMaxBodySize) - 3) / 8;
        if (rows.size() > kMaxRows) {
            core::logf("[WARN] inventory truncated %zu -> %zu\n", rows.size(), kMaxRows);
            rows.resize(kMaxRows);
        }

        std::vector<char> out;
        out.reserve(3 + rows.size() * 8);

        // result 를 맨 앞에 둔다. 실패면 클라이언트가 「아이템이 없다」가 아니라
        //   「못 읽었다」로 안다.
        out.push_back(static_cast<char>(result));

        uint8_t head[2];
        proto::write_u16_be(head, static_cast<uint16_t>(rows.size()));
        out.insert(out.end(), reinterpret_cast<char*>(head),
                              reinterpret_cast<char*>(head) + 2);

        for (const db::InventoryRow& r : rows) {
            uint8_t rec[8];
            proto::write_u32_be(rec + 0, r.item_id);
            // 부호 있는 값을 비트 그대로 싣는다. 읽는 쪽이 int32 로 본다.
            proto::write_u32_be(rec + 4, static_cast<uint32_t>(r.count));
            out.insert(out.end(), reinterpret_cast<char*>(rec),
                                  reinterpret_cast<char*>(rec) + 8);
        }

        return send_msg(server, session, proto::MsgId::kInventoryAck,
            out.data(), static_cast<int>(out.size()));
    }

    // ── 처리할 수 없는 메시지 ────────────────────────────────────────
    //
    // 끊지 않고 이 프레임만 버린다. frame_size 가 길이를 이미 확정했으므로 read_pos 는
    // 정확히 다음 프레임 시작에 가 있고, 스트림은 멀쩡하다.
    //
    // DB 큐가 가득 찼을 때와 같은 판단이다 — 요청 하나를 못 받은 것이지 그 클라이언트가
    // 잘못한 게 아니다. 다만 저쪽은 우리 사정이고 이쪽은 상대 사정이라 점수를 센다.
    //
    // 무엇이 왜 거부됐는지는 알려주지 않는다. 규약을 모르는 상대에게 규약을 가르쳐
    // 주는 일이 된다. 정상 클라의 버전 스큐라면 서버 로그로 잡는 편이 맞다.
    bool handle_unhandled(net::Session& session, proto::MsgId msg_id) {
        const int add = proto::is_server_message(msg_id)
            ? kScoreServerOnlyId : kScoreUnknownId;

        // fetch_add 는 「더하기 전」 값을 준다. 더한 뒤 값이 필요하다.
        const int score = session.bad_msg_score.fetch_add(
            add, std::memory_order_relaxed) + add;

        if (should_log_bad_msg(score)) {
            core::logf("[WARN] #%llu unhandled msg_id=%u (%s) score=%d/%d\n",
                static_cast<unsigned long long>(session.id),
                static_cast<unsigned>(msg_id),
                proto::is_server_message(msg_id) ? "server-only" : "unknown",
                score, kBadMsgLimit);
        }

        if (score > kBadMsgLimit) {
            core::logf("[WARN] #%llu bad msg score %d > %d — closing\n",
                static_cast<unsigned long long>(session.id),
                score, kBadMsgLimit);
            return false;           // 여기서만 끊는다
        }
        return true;                // 이 프레임만 버리고 계속 읽는다
    }
} // namespace

namespace app {
    // 프레임 자르기 — net 이 「어디서 자를까요」 하고 묻는 함수. net 이 부르는 것 중
    // proto 를 아는 유일한 곳이라, net 은 proto 를 안 봐도 프레임을 자를 수 있다.
    //
    //   > 0  이만큼이 한 메시지다 (헤더 포함)
    //   = 0  아직 부족하다. 더 받아야 한다
    //   < 0  규약 위반이다. 끊어라
    //
    // 「어디서 자를지」만 답하고 「아는 메시지인가」는 안 본다. 한때 여기서 msg_id 까지
    // 검사하고 모르면 끊었는데 그게 틀렸다. 끊어야 하는 건 프레이밍이 깨진 경우뿐이다 —
    // body_size 가 상한을 넘으면 그 값을 믿을 수 없고, 믿을 수 없는 값 말고는 다음
    // 프레임이 어디서 시작하는지 알 방법이 없다. 반면 모르는 msg_id 는 프레이밍과
    // 무관하다. body_size 가 유효 범위 안이라 정확히 건너뛸 수 있고, 끊으면 클라 버전
    // 롤아웃 중에 유저가 전부 튕긴다. 그 처분은 세션 상태를 아는 on_frame 이 정한다.
    int frame_size(const char* data, int len) {
        // 헤더부터 확보한다. 헤더가 안 왔으면 body_size 를 읽을 수조차 없다.
        if (len < static_cast<int>(proto::kHeaderSize)) {
            return 0;
        }

        const proto::PacketHeader head =
            proto::decode_header(reinterpret_cast<const uint8_t*>(data));

        // 여기만 -1 이다 — 프레이밍이 깨진 유일한 경우.
        if (head.body_size > proto::kMaxBodySize) {
            core::logf("[WARN] body_size=%u > max=%u — closing\n",
                static_cast<unsigned>(head.body_size),
                static_cast<unsigned>(proto::kMaxBodySize));
            return -1;
        }

        if (len - static_cast<int>(proto::kHeaderSize) < head.body_size) {
            return 0;
        }
        return static_cast<int>(proto::kHeaderSize) + head.body_size;
    }

    // 이 호출 자체가 이미 그 세션의 직렬 큐 실행권 안에서 돈다(app::WorkerPool) —
    // 그래서 존을 아는 핸들러는 session.zone 을 직접 읽는다. 실행권이 세션당
    // 하나뿐이라 그 읽기가 이 세션의 다른 실행과 절대 겹치지 않는다. 예전
    // placement 인자와 「다시 읽지 않는다」계약(그 시절의 버그였다 — 큐를 고른
    // 근거와 일을 한 근거가 갈리는 것)은 이 실행권 하나로 대체됐다.
    bool on_frame(net::IocpServer& server, db::DbPool& db_pool, EntryTable& entry,
        net::Session& session, const char* frame, int len) {
        if constexpr (kLogicDelayMs != 0) {
            Sleep(kLogicDelayMs);
        }

        const proto::PacketHeader req =
            proto::decode_header(reinterpret_cast<const uint8_t*>(frame));

        const char* body = frame + proto::kHeaderSize;
        const int      body_len = req.body_size;

        if (core::log_trace_frames())
        core::logf("[RECV] tid=%lu #%llu id=%u zone=%u body=%dB (frame=%dB)\n",
            GetCurrentThreadId(),
            static_cast<unsigned long long>(session.id),
            static_cast<unsigned>(req.msg_id),
            static_cast<unsigned>(session.zone), body_len, len);

        // 로그인 여부를 안 가리는 메시지 — 존도 DB 도 안 만진다.
        switch (req.msg_id) {
        case proto::MsgId::kEchoReq:
            return send_msg(server, session, proto::MsgId::kEchoAck, body, body_len);

        // 아무 일도 안 하는 핸들러다. ping 이 해야 할 일(last_recv_ms 를 미는 것)은
        // net 이 수신 완료 지점에서 이미 했고, 이 프레임이 여기까지 온 것 자체가 그
        // 증거다. 여기서 또 만지면 두 곳이 같은 값을 밀게 되고, 언젠가 한쪽만 고쳐지면
        // 어느 쪽이 진짜인지 알 수 없어진다. body 도 안 보낸다 — 클라가 알아야 하는
        // 것은 응용 계층까지 답이 왔다는 사실뿐이고 헤더만으로 다 말해진다.
        case proto::MsgId::kPingReq:
            return send_msg(server, session, proto::MsgId::kPongAck, nullptr, 0);

        // 존 개념 이전의 메시지다 — 예약을 소비해 신원을 확정할 뿐 존을 만지지 않는다.
        case proto::MsgId::kEnterReq:
            return handle_enter(server, entry, session, body, body_len);

        default:
            break;                        // 나머지는 로그인 게이트 뒤에서만 뜻이 있다
        }

        // 게이트 — 예약을 소비해 신원을 확정하지 않은 세션은 여기서 끊는다.
        //   조건 없이 건다 — 미정의 msg_id 도 예외를 두지 않는다. 모르는
        //   메시지를 봐주는 관용(handle_unhandled 의 근거)은 이미 자리를 얻은
        //   세션에게 주는 것이다. 예약도 없이 붙은 소켓은 그 시나리오의
        //   당사자가 아니다.
        if (session.player_id == 0) {
            core::logf("[WARN] #%llu msg_id=%u 미인증 상태에서 게임 요청 — closing\n",
                static_cast<unsigned long long>(session.id),
                static_cast<unsigned>(req.msg_id));
            return false;
        }

        switch (req.msg_id) {

        case proto::MsgId::kJoinZoneReq:
            return handle_join_zone(server, entry, session, body, body_len);

        case proto::MsgId::kChatReq:
            return handle_chat(server, entry, session, body, body_len);

        case proto::MsgId::kTradeReqReq:
            return handle_trade_req(server, entry, session, body, body_len);

        case proto::MsgId::kTradeAnswerReq:
            return handle_trade_answer(server, entry, session, body, body_len);

        case proto::MsgId::kTradeSetItemReq:
            return handle_trade_set_item(server, entry, session, body, body_len);

        case proto::MsgId::kTradeConfirmReq:
            return handle_trade_confirm(server, db_pool, entry, session, body, body_len);

        case proto::MsgId::kTradeCancelReq:
            return handle_trade_cancel(server, entry, session, body_len);

        case proto::MsgId::kInventoryReq:
            return handle_inventory(server, db_pool, session);

        default:
            return handle_unhandled(session, req.msg_id);
        }
    }

    //  끊긴 세션의 뒷정리 — 그 세션 자신의 직렬 큐 맨 뒤에 넣는다.
    //
    //  세션이 닫히면 명부에서 빼야 한다. 예전엔 그게 「존 스레드의 일」이라 Job 으로
    //  보냈다. 지금은 「그 세션의 실행권」의 일이라 직렬 큐로 보낸다 — 도착 순서로
    //  실행되는 직렬 큐가 이 정리를 이 세션의 다른 실행(로그인·입장·이동)보다 뒤에
    //  세워 준다(예전 존 큐의 FIFO 가 서던 그 자리를 승계한다).
    //  홀드가 두 개다 —
    //      (1) 명부 등록이 들고 있던 것 : entry.leave() 가 성공했을 때만 반납
    //      (2) close_session 이 올려 준 것 : 이 정리 자신의 홀드. 항상 반납
    //    순서가 중요하다. (1)을 먼저 놓아도 (2)가 남아 있어 0 이 안 된다.
    //    마지막 반납에서 지워지고, 그 뒤로 sp 를 만지지 않는다.
    bool on_session_gone(net::IocpServer& server, WorkerPool& workers,
        EntryTable& entry, net::Session& session) {
        net::Session* sp = &session;

        return workers.submit(session, [&server, &entry, sp] {
            // player_id 는 이 세션의 실행권을 쥔 코드만 읽고 쓴다(session.h 주석) —
            // 이 정리는 그 실행권 자체이고, 직렬 큐 FIFO 가 이 세션의 이전 실행
            // (로그인·입장)보다 뒤에 서도록 보장하므로, 그 값을 쓰는 마지막 실행이
            // 이미 끝난 뒤에 읽는다는 것이 이 순서로 보장된다.
            const uint64_t pid = sp->player_id;   // 0 이면 Enter 를 안 한 세션이다

            // entry.leave() 가 성공하면 그 임계구역 안에서 PlayerLeave 통지도
            //   notifier 로 자동으로 나간다. 여기서 따로 부르지 않는다.
            if (pid != 0) {
                const app::EntryTable::LeaveResult left = entry.leave(pid, sp->id);
                if (left.ok) {
                    server.release_session(sp);   // (1) 명부 홀드

                    // 멤버 변동 — 접속이 끊겼다. had_zone 일 때만, 그리고 명부가
                    //   기억하는 그 존에만 남은 사람들의 목록을 갱신한다.
                    //   session.zone 을 그대로 쓰지 않는 이유는, 존에 한 번도
                    //   들어간 적 없는 세션도 그 필드는 기본값(로비 0)이라서다.
                    //   통지 대상 존의 정본은 명부지 세션 자신의 라벨이 아니다.
                    if (left.had_zone) {
                        broadcast_zone_members(server, entry, left.zone);
                    }
                }
            }

            // 거래 정리 지점 (4/4) — 끊긴 세션의 거래를 지우고 상대에게 알린다.
            //   실무에서 중복 로그인으로 「킥」당하는 경우가 정확히 이 경로다.
            //   여기를 빠뜨리면 상대는 죽은 세션과 거래 중인 상태로 남는다.
            //   sp 자신은 이 정리의 홀드로 살아 있고, 상대는 clear_trade 안에서
            //   entry.find_acquire_by_session_id 로 명부 경유로만 확보한다 —
            //   상대가 이미 gone-Job 을 먼저 마쳤으면 그 조회 자체가 nullptr
            //   을 돌려주므로 raw 포인터를 반출하는 창이 없다(world/trade.h 참고).
            if (net::Session* tp = clear_trade(server, entry, *sp)) {
                send_peer_ntf(server, *tp, proto::MsgId::kTradeCancelNtf, sid32(*sp));
                server.release_session(tp);
            }

            server.release_session(sp);       // (2) 이 정리 자신의 홀드
            });
    }

} // namespace app
