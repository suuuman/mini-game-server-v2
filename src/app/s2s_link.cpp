#include "app/s2s_link.h"

#include <algorithm>
#include <chrono>
#include <future>
#include <memory>
#include <optional>

#include "app/entry_table.h"
#include "net/iocp_server.h"   // close_by_id — Kick 이 전방선언 경계를 넘는 유일한 자리
#include "proto/s2s_packet.h"
#include "core/log.h"
#include "world/trade.h"     // world::now_ms() — Reserve 의 expire_ms 를 절대 시각으로 바꾼다

// net 도 proto 도 서로를 모른다. 둘 다 아는 이 층이 관계를 강제한다 — 어긋나면
// 링크도 실행도 아닌 컴파일 단계에서 잡힌다(frame_router.cpp 의 클라 프로토콜 판과
// 같은 자리).
static_assert(net::kS2sHeaderSize == static_cast<int>(proto::s2s::kHeaderSize),
    "net::S2sConnector 와 proto::s2s 의 헤더 폭이 어긋난다");
static_assert(net::kS2sRecvBufferSize >=
        static_cast<int>(proto::s2s::kHeaderSize) + static_cast<int>(proto::s2s::kMaxBodySize),
    "S2S 수신 버퍼가 한 프레임(헤더+최대 본문)을 못 담는다");

namespace app {

    namespace {

        // net 은 proto 를 모르므로 이 층이 감싼다. 1단계의 decode_header(const uint8_t*)
        // 와 시그니처가 다른 이유 — 그쪽은 8B 고정 헤더만 돌려주면 끝이지만, 여기서는
        // 프레임 전체 안에서 헤더/본문 경계까지 함께 잘라 내야 한다.
        bool decode_s2s_header(const char* frame, int len,
            uint16_t& msg_id, uint32_t& seq, const char*& body, int& body_len) {
            if (len < static_cast<int>(proto::s2s::kHeaderSize)) {
                return false;
            }
            const proto::s2s::Header h =
                proto::s2s::decode_header(reinterpret_cast<const uint8_t*>(frame));
            msg_id = h.msg_id;
            seq = h.seq;
            body = frame + proto::s2s::kHeaderSize;
            body_len = len - static_cast<int>(proto::s2s::kHeaderSize);
            return true;
        }

        void encode_s2s_header(char* dst, uint16_t msg_id, uint32_t seq, uint16_t body_size) {
            proto::s2s::Header h;
            h.body_size = body_size;
            h.msg_id = msg_id;
            h.seq = seq;
            proto::s2s::encode_header(reinterpret_cast<uint8_t*>(dst), h);
        }

        // unregister_and_wait() 의 대기 상한 — settings_.request_timeout_ms(기본
        // 10s)를 그대로 쓰지 않는다. 이 대기는 정상 종료 경로에 항상 놓이는데,
        // 세션 서버가 붙어 있으면서 응답을 못 주는 상황(과부하·행)에서 request_timeout_ms
        // 만큼을 그대로 기다리면 마을 종료가 그만큼 늦어진다. 못 알린 경우는 세션
        // 쪽 orphan 유예(§5-3)가 흡수하므로 Unregister 는 최선의 노력이지 성공
        // 조건이 아니다 — 짧게 포기해도 된다.
        // 실측(로컬호스트, 실물 session.exe·정상 응답 6회): 실왕복
        // min=0.0ms avg=0.3ms max=1.0ms — 1000 은 정상 경로에서 거의 전부
        // 여유다. 세션 서버가 응답을 아예 안 주는 최악의 경우는 이 값을
        // 그대로 다 쓴다(설계상 당연 — 그게 이 상수의 존재 이유다).
        constexpr int kUnregisterWaitMs = 1000;

        // 마을 쪽 분류기 — 세션 쪽(0x80xx 를 요청으로 보는) 분류기는 2단계 몫이다.
        net::S2sFrameKind classify_s2s(uint16_t msg_id) {
            const auto id = static_cast<proto::s2s::MsgId>(msg_id);
            if (proto::s2s::is_reply_to_village(id)) {
                return net::S2sFrameKind::kReply;
            }
            if (proto::s2s::is_request_from_session(id)) {
                return net::S2sFrameKind::kRequest;
            }
            return net::S2sFrameKind::kUnknown;
        }

    }   // namespace

    S2sSettings load_s2s_settings(const core::Config& cfg) {
        S2sSettings s;
        s.host = cfg.get("s2s", "host", "");
        s.port = static_cast<uint16_t>(cfg.get_int("s2s", "port", 9100, 1, 65535));
        s.advertise_host = cfg.get("s2s", "advertise_host", "127.0.0.1");
        s.backoff_initial_ms = cfg.get_int("s2s", "backoff_initial_ms", 1000, 1, 3'600'000);
        s.backoff_max_ms     = cfg.get_int("s2s", "backoff_max_ms", 30000, 1, 3'600'000);
        s.request_timeout_ms = cfg.get_int("s2s", "request_timeout_ms", 10000, 1, 3'600'000);
        s.heartbeat_ms       = cfg.get_int("s2s", "heartbeat_ms", 5000, 1, 3'600'000);
        return s;
    }

    S2sLink::~S2sLink() {
        stop();
    }

    bool S2sLink::start(const S2sSettings& settings, uint16_t server_port, uint32_t server_capacity,
        net::IocpServer& server, EntryTable& entry, uint32_t reserve_expire_max_ms,
        int fullsync_chunk_max) {
        settings_ = settings;
        server_port_ = server_port;
        server_capacity_ = server_capacity;
        server_ = &server;
        entry_ = &entry;
        reserve_expire_max_ms_ = reserve_expire_max_ms;
        fullsync_chunk_max_ = fullsync_chunk_max;

        if (settings_.host.empty()) {
            core::logf("[INFO] s2s disabled (no host)\n");
            return true;    // 비활성은 실패가 아니다 — 상위가 오류로 취급하면 안 된다
        }

        net::S2sFrameCodec codec;
        codec.frame_sizer = &proto::s2s::frame_size;
        codec.header_encode = &encode_s2s_header;
        codec.header_decode = &decode_s2s_header;
        codec.classify = &classify_s2s;
        connector_.set_frame_codec(std::move(codec));

        connector_.set_on_connected([this] { on_connected(); });
        connector_.set_on_disconnected([this] { on_disconnected(); });
        connector_.set_on_request(
            [this](uint16_t msg_id, uint32_t seq, const char* body, int len) {
                on_request(msg_id, seq, body, len);
            });
        connector_.set_on_tick([this] { on_tick(); }, settings_.heartbeat_ms);

        net::S2sConfig s2s_cfg;
        s2s_cfg.host = settings_.host;
        s2s_cfg.port = settings_.port;
        s2s_cfg.backoff_initial_ms = settings_.backoff_initial_ms;
        s2s_cfg.backoff_max_ms = settings_.backoff_max_ms;
        s2s_cfg.request_timeout_ms = settings_.request_timeout_ms;

        started_ = connector_.start(s2s_cfg);
        return started_;
    }

    void S2sLink::stop() {
        if (started_) {
            connector_.stop();
            started_ = false;
        }
    }

    void S2sLink::on_connected() {
        proto::s2s::Register msg;
        msg.ver = proto::s2s::ver();
        msg.port = server_port_;
        msg.capacity = server_capacity_;

        // IocpServer 공개 API 에는 "지금 세션이 몇 개인가"를 묻는 접근자가 없다 —
        // session_peak() 은 피크값이지 현재값이 아니고, iocp_server.h 는 불가침이라
        // 접근자를 새로 만들지 않는다. 대신 EntryTable::current() 가 "지금 이
        // 마을에 입장한 인원"이라는, 세션 서버 배정에 실제로 필요한 값을 준다.
        msg.current = entry_->current();
        msg.host = settings_.advertise_host;

        connector_.request(proto::s2s::encode_register(msg),
            static_cast<uint16_t>(proto::s2s::MsgId::Register),
            [this](net::S2sResult result, uint16_t msg_id, const char* body, int len) {
                on_register_ack(result, msg_id, body, len);
            });
    }

    void S2sLink::on_disconnected() {
        // 재연결 직후 새 RegisterAck 가 오기 전에 조기 Heartbeat 가 나가면 안 된다 —
        // on_tick 은 이 값을 보고 정한다.
        registered_ = false;
        // 재연결로 만나는 세션 서버는 이 마을의 완료 알림을 받은 적이 없다 —
        // per-링크 발신 상태는 링크 수명과 함께 죽어야 다음 링크에서 DrainComplete
        // 가 다시 나간다. registered_ 와 같은 이유·같은 자리다.
        drain_complete_sent_ = false;
    }

    void S2sLink::on_register_ack(net::S2sResult result, uint16_t /*msg_id*/,
        const char* body, int len) {
        if (result != net::S2sResult::kOk) {
            // 타임아웃/끊김 — 재연결 경로(on_connected)가 Register 를 다시 보낸다.
            core::logf("[WARN] s2s Register 실패(result=%d)\n", static_cast<int>(result));
            return;
        }

        proto::s2s::RegisterAck ack;
        if (!proto::s2s::decode_register_ack(body, len, ack)) {
            // 고정 5B 위반 — proto::s2s::decode_register_ack 의 bool 판정이 그대로
            // 검증이다(§8-4).
            core::logf("[WARN] s2s RegisterAck 길이 위반(%dB, 5B 기대) — 끊는다\n", len);
            connector_.force_disconnect();
            return;
        }

        if (ack.result == proto::s2s::kResultOk) {
            registered_ = true;
            core::logf("[S2S  ] registered server_id=%u\n",
                static_cast<unsigned>(ack.server_id));
            // 등록이 확인된 이 순간에만 보낸다 — on_connected() 에서 보내면
            // registered_ 가 아직 false 라 곧 끊길 연결(버전 거부·정원 거부)에도
            // 나가고, ① Register ② FullSync ③ 전량 교체라는 순서를 어긴다.
            send_full_sync();
        } else if (ack.result == proto::s2s::kResultVersionRejected) {
            registered_ = false;
            core::logf("[ERROR] s2s Register 버전 거부(result=%u)\n",
                static_cast<unsigned>(ack.result));
            // 연결을 유지한 채 기다리면 상대가 먼저 안 끊는 한 재시도가 영원히 안
            // 일어난다 — 마을이 스스로 끊고 최대 백오프 뒤 재시도한다
            // (ADR-019 결정 10 "최대 백오프로 재시도 지속"). backoff_max_ms 를
            // 1회성 오버라이드로 넘긴다 — 일반 백오프로 돌면 거부 직후 초기값(1s)
            // 간격의 거부 루프가 된다.
            connector_.force_disconnect(settings_.backoff_max_ms);
        } else if (ack.result == proto::s2s::kResultFull) {
            registered_ = false;
            // 정원 거부는 상대 상태라 기다리면 풀린다 — 버전 거부와 같은 이유로 스스로 끊고
            // 최대 백오프 뒤 재시도한다(ADR-020 결정 5). 30s 가 최적인지는 안 쟀다.
            core::logf("[WARN] s2s Register 정원 거부(full) — 최대 백오프 뒤 재시도\n");
            connector_.force_disconnect(settings_.backoff_max_ms);
        } else {
            registered_ = false;
            core::logf("[WARN] s2s RegisterAck result=%u (미정의값) — 등록 안 된 것으로 본다\n",
                static_cast<unsigned>(ack.result));
        }
    }

    void S2sLink::on_tick() {
        if (!registered_) {
            return;     // 등록 전에 하트비트를 보내면 상대가 순서를 의심할 이유를 준다
        }

        if (entry_->draining() && entry_->current() == 0 && !drain_complete_sent_) {
            // 알림이다(seq=0) — notify_player_enter/leave 와 같은 규약. 마지막
            // 한 명이 빠진 바로 다음 틱에 한 번만 나가야 하므로 플래그로 막는다
            // (안 막으면 current()==0 인 채로 도는 매 틱마다 다시 나간다).
            proto::s2s::DrainComplete drain_msg;
            drain_msg.remaining = 0;
            connector_.notify(proto::s2s::encode_drain_complete(drain_msg),
                static_cast<uint16_t>(proto::s2s::MsgId::DrainComplete));
            drain_complete_sent_ = true;
        }

        proto::s2s::Heartbeat msg;
        msg.current = entry_->current();   // Register.current 와 같은 값·같은 이유

        connector_.request(proto::s2s::encode_heartbeat(msg),
            static_cast<uint16_t>(proto::s2s::MsgId::Heartbeat),
            [this](net::S2sResult result, uint16_t msg_id, const char* body, int len) {
                on_heartbeat_ack(result, msg_id, body, len);
            });
    }

    void S2sLink::on_heartbeat_ack(net::S2sResult result, uint16_t /*msg_id*/,
        const char* /*body*/, int len) {
        if (result != net::S2sResult::kOk) {
            core::logf("[WARN] s2s Heartbeat 실패(result=%d)\n", static_cast<int>(result));
            return;
        }
        if (!proto::s2s::decode_heartbeat_ack(len)) {
            // 고정 0B 위반.
            core::logf("[WARN] s2s HeartbeatAck 길이 위반(%dB, 0B 기대) — 끊는다\n", len);
            connector_.force_disconnect();
        }
    }

    void S2sLink::on_request(uint16_t msg_id, uint32_t seq, const char* body, int len) {
        const auto id = static_cast<proto::s2s::MsgId>(msg_id);

        if (id == proto::s2s::MsgId::Reserve) {
            proto::s2s::Reserve msg;
            if (!proto::s2s::decode_reserve(body, len, msg)) {
                // 고정 12B 위반. on_request 의 반환형이 void 라 클라 핸들러의
                // bad_body 처럼 false 를 돌려주는 것으로 끊을 방법이 없다 —
                // on_register_ack 의 버전 거부 분기와 같은 방식으로
                // force_disconnect() 를 직접 부른다.
                core::logf("[WARN] s2s Reserve 길이 위반(%dB, 12B 기대) — 끊는다\n", len);
                connector_.force_disconnect();
                return;
            }

            if (entry_->draining()) {
                // 저장 자체를 안 한다 — add_reservation 을 부르면 거절 응답
                // 뒤에도 예약이 남아, 드레인이 풀린 뒤 그 예약으로 Enter 가
                // 성사되는 모순이 생긴다. 거절은 "이 요청을 안 받았다"여야 한다.
                proto::s2s::ReserveAck ack;
                ack.result = proto::s2s::kResultDraining;
                connector_.respond(seq, static_cast<uint16_t>(proto::s2s::MsgId::ReserveAck),
                    proto::s2s::encode_reserve_ack(ack));
                return;
            }

            // expire_ms 는 상대 시계를 안 믿는 상대적 유효 시간이다 — 수신 측인
            // 여기가 자기 시계 기준으로 절대 만료 시각을 만든다(s2s_packet.h 참조).
            // 상대가 부른 값을 그대로 믿지 않는다 — reserve_expire_max_ms_ 가
            // 마을이 인정하는 상한이다. 세션의 reserve.expire_ms 와 마을의 이
            // 상한을 같은 값으로 재지 않는 것이 회귀 하네스의 전제다(둘이 같으면
            // 「세션이 만료시켰다」와 「마을이 만료시켰다」를 구분하는 단언을 못 세운다).
            const uint32_t effective_ms = (std::min)(msg.expire_ms, reserve_expire_max_ms_);
            const uint64_t expire_at_ms = world::now_ms() + effective_ms;
            const bool saved = entry_->add_reservation(msg.player_id, expire_at_ms);

            proto::s2s::ReserveAck ack;
            // add_reservation 은 지금 실패 갈래가 없어 늘 true 다. kResultFull 은
            // "정원 거부"를 뜻하는 값이라 의미가 정확히 맞지는 않지만, Reserve 저장
            // 실패 전용 코드가 아직 없어 0(성공)과 구분되는 자리채움으로 재사용한다.
            ack.result = saved ? proto::s2s::kResultOk : proto::s2s::kResultFull;
            connector_.respond(seq, static_cast<uint16_t>(proto::s2s::MsgId::ReserveAck),
                proto::s2s::encode_reserve_ack(ack));
            return;
        }

        if (id == proto::s2s::MsgId::SetMode) {
            proto::s2s::SetMode msg;
            if (!proto::s2s::decode_set_mode(body, len, msg)) {
                core::logf("[WARN] s2s SetMode 길이 위반(%dB, 1B 기대) — 끊는다\n", len);
                connector_.force_disconnect();
                return;
            }

            entry_->set_draining(msg.mode != 0);
            if (msg.mode == 0) {
                // Running 복귀 — 다음 드레인에서 DrainComplete 를 다시 보낼 수
                // 있어야 한다. 플래그를 안 지우면 current()==0 인 채로 재-drain
                // 했을 때 on_tick 이 "이미 보냈다"고 보고 알림을 또 안 낸다.
                drain_complete_sent_ = false;
            }

            proto::s2s::SetModeAck ack;
            ack.current = entry_->current();
            connector_.respond(seq, static_cast<uint16_t>(proto::s2s::MsgId::SetModeAck),
                proto::s2s::encode_set_mode_ack(ack));
            return;
        }

        if (id == proto::s2s::MsgId::Kick) {
            proto::s2s::Kick msg;
            if (!proto::s2s::decode_kick(body, len, msg)) {
                core::logf("[WARN] s2s Kick 길이 위반(%dB, 9B 기대) — 끊는다\n", len);
                connector_.force_disconnect();
                return;
            }

            // 회신은 즉시 한다 — 정합의 실제 게이트는 이 응답이 아니라 뒤이어
            // 나가는 PlayerLeave 다(세션 서버 쪽 접속 테이블은 그 알림으로
            // 지워진다). 끊긴 세션의 entry.leave·notify_player_leave·거래
            // 정리는 여기서 하지 않는다 — 기존 on_session_gone 존 Job 경로가
            // 그 셋을 자동으로 수행한다(중복 로그인으로 「킥」당하는 경우가
            // 정확히 이 경로다).
            const std::optional<uint64_t> sid = entry_->find_session(msg.player_id);
            uint8_t result = proto::s2s::kKickResultNotFound;
            if (sid.has_value()) {
                // close_by_id 가 false 면 조회와 close 사이에 스스로 끊긴
                // 경우다 — 그때도 result 는 NotFound 다. 노리던 결말(그
                // 세션이 이제 없다)이 어느 경로로든 이루어져 있어서다.
                result = server_->close_by_id(*sid) ? proto::s2s::kKickResultKicked
                                                     : proto::s2s::kKickResultNotFound;
            }

            core::logf("[KICK ] player=%llu session=%llu result=%u\n",
                static_cast<unsigned long long>(msg.player_id),
                sid.has_value() ? static_cast<unsigned long long>(*sid) : 0ULL,
                static_cast<unsigned>(result));

            proto::s2s::KickAck ack;
            ack.result = result;
            connector_.respond(seq, static_cast<uint16_t>(proto::s2s::MsgId::KickAck),
                proto::s2s::encode_kick_ack(ack));
            return;
        }

        // 그 밖은 아직 처리하지 않는다. 안 보내면 상대가 타임아웃까지
        // 기다린다(§8-2) — 그래서 오면 즉시 Unsupported 로 답한다.
        connector_.respond(seq, static_cast<uint16_t>(proto::s2s::MsgId::Unsupported),
            proto::s2s::encode_unsupported());
    }

    void S2sLink::send_full_sync() {
        if (!started_) {
            return;     // notify_player_enter/leave() 와 같은 가드 — 비활성이면 no-op
        }

        // ids 와 무관한 고정값이라 락 밖에서 구해도 된다 — with_snapshot 안에서
        // 다시 계산해도 매번 같은 값이라, 그러면 락 보유 시간만 늘어난다.
        // 0 = 파생값 그대로. 그 외에는 파생값과의 min — 설정이 세션 수신 상한
        // (kServerRecvBodyCap 에서 나온 kFullSyncMaxPerChunk)을 못 넘게 한다.
        const size_t effective_max = (fullsync_chunk_max_ == 0)
            ? static_cast<size_t>(proto::s2s::kFullSyncMaxPerChunk)
            : (std::min)(static_cast<size_t>(fullsync_chunk_max_),
                         static_cast<size_t>(proto::s2s::kFullSyncMaxPerChunk));

        // 청크 조립과 notify() 제출을 entry_->mutex_ 를 쥔 채로 한다 — 존
        // 스레드의 enter/leave 가 같은 락을 타므로, 「스냅샷을 뜬 시점」과
        // 「그 스냅샷을 상대에게 다 보낸 시점」 사이에 다른 변경이 끼어들
        // 수 없다. 그 틈이 있으면 증분 알림(enter/leave)이 스냅샷보다 먼저
        // 명령 큐에 실려 세션 서버가 보는 순서가 뒤집히고, 전량 교체가 방금
        // 온 증분을 되돌려 유령이나 누락을 만든다 — Kick 이 붙은 지금은 그
        // 유령이 "멀쩡한 유저를 킥"으로 이어질 수 있어 더는 방치할 수 없다.
        // fn 이 하는 일은 connector_.notify() 로 명령 큐에 넣는 것뿐이라
        // (실송신은 S2S 스레드가 따로 한다 — §7-2 규칙 3) 락 아래 실행해도 된다.
        entry_->with_snapshot([this, effective_max](const std::vector<uint64_t>& ids) {
            // 집합이 비어 있어도 한 번은 보낸다 — 안 보내면 세션 서버가 "아직
            // 안 왔다"와 "비었다"를 구분할 방법이 없다.
            const size_t total = ids.empty()
                ? 1 : (ids.size() + effective_max - 1) / effective_max;

            for (size_t i = 0; i < total; ++i) {
                const size_t begin = i * effective_max;
                const size_t end = (std::min)(begin + effective_max, ids.size());

                proto::s2s::FullSync msg;
                msg.chunk_idx = static_cast<uint16_t>(i);
                msg.chunk_total = static_cast<uint16_t>(total);
                msg.player_ids.assign(ids.begin() + static_cast<ptrdiff_t>(begin),
                                       ids.begin() + static_cast<ptrdiff_t>(end));

                // 알림이다 — 세션 서버가 회신하지 않는다(is_request_from_village
                // 의 주석대로 seq=0 규약). request() 가 아니라 notify() 를 쓴다.
                connector_.notify(proto::s2s::encode_full_sync(msg),
                    static_cast<uint16_t>(proto::s2s::MsgId::FullSync));
            }
        });
    }

    void S2sLink::notify_player_enter(uint64_t player_id) {
        if (!started_) {
            return;
        }
        proto::s2s::PlayerEnter msg;
        msg.player_id = player_id;
        connector_.notify(proto::s2s::encode_player_enter(msg),
            static_cast<uint16_t>(proto::s2s::MsgId::PlayerEnter));
    }

    void S2sLink::notify_player_leave(uint64_t player_id) {
        if (!started_) {
            return;
        }
        proto::s2s::PlayerLeave msg;
        msg.player_id = player_id;
        connector_.notify(proto::s2s::encode_player_leave(msg),
            static_cast<uint16_t>(proto::s2s::MsgId::PlayerLeave));
    }

    void S2sLink::unregister_and_wait() {
        if (!started_) {
            return;
        }

        // done 을 shared_ptr 로 감싸는 이유 — 이 콜백은 S2S 스레드에서 나중에
        // (정상 응답·타임아웃·끊김·stop 갈래를 통틀어 정확히 1회) 불린다.
        // wait_for 가 시간 안에 못 받고 이 함수가 먼저 반환하면 지역 promise/
        // future 는 사라지지만, 콜백이 들고 있는 이 shared_ptr 사본이 공유
        // 상태를 그대로 살려 두므로 나중에 불려도 안전하다 — future 가 이미
        // 없어져도 promise 하나만 살아 있으면 set_value() 는 그대로 성립한다.
        auto done = std::make_shared<std::promise<void>>();
        std::future<void> waiting = done->get_future();

        const bool submitted = connector_.request(proto::s2s::encode_unregister(),
            static_cast<uint16_t>(proto::s2s::MsgId::Unregister),
            [done](net::S2sResult, uint16_t, const char*, int) {
                done->set_value();
            });

        // submit() 이 false 면 commands_closed_ 이후라 이 요청은 명령 큐에
        //   들어가지도 못했다 — 콜백은 영영 안 온다(CODING_RULES.md §4-b 짝 API
        //   표의 다섯 번째 경로). 여기서 기다리면 매번 kUnregisterWaitMs 를
        //   그대로 태우고서야 "응답 없음"을 찍는데, 실제로는 보내지도 못한
        //   것이라 그 문구가 원인을 잘못 말한다 — 기다리지 않고 바로 안다.
        if (!submitted) {
            core::logf("[WARN] s2s Unregister 전송 실패(stop 이후) — orphan 유예로 정리된다\n");
            return;
        }

        // 성공(응답 수신)이면 조용하다 — [S2S  ] 종료 요약이 이미 나간다. 실패
        // (타임아웃·끊김·stop)면 운영이 그 사실을 알 방법이 지금 이 한 줄뿐이다 —
        // 안 남기면 세션 쪽에 orphan 이 남았을 때 원인을 되짚을 수 없다.
        if (waiting.wait_for(std::chrono::milliseconds(kUnregisterWaitMs))
                != std::future_status::ready) {
            core::logf("[WARN] s2s Unregister 응답 없음 — orphan 유예로 정리된다\n");
        }
    }

}   // namespace app
