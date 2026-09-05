# 아키텍처 분석 — 확장 개발용 정밀 지도

> ⚠️ **아래 서술은 「이 커밋이 `master` 에 머지된 시점」 기준으로 쓰였다.**
> 이 문서가 커밋에 포함되기 전(리뷰 중)에 읽으면 아직 참이 아니다.

> README 는 「왜 그렇게 했는가」를 설명한다. 이 문서는 「어디를 어떻게 만져야 하는가」다.
> 코드를 고치기 전에 이 문서의 **불변식** 절을 먼저 읽는다.

---

## 1. 실행 구조 — 스레드와 소유권

| 스레드 | 수 | 만져도 되는 것 | 절대 만지면 안 되는 것 |
|---|---|---|---|
| accept | 1 | `sessions_`(뮤텍스), 소켓 | DB |
| I/O 워커 | 4 | 세션의 recv/send 버퍼, 프레임 풀 | **게임 로직** — 직렬 큐에 넣기(`job_sink_`)만 하고 실행은 안 한다 |
| 직렬 큐 워커 | N(기본 8 — `app::WorkerPool`) | 그 순간 **실행권을 쥔 세션**의 `game_mutex`·`zone`·`trade`·`serial_queue`, DB 커넥션(동기 호출) | 실행권을 안 쥔 세션의 `zone`/`serial_queue`. **자기 자신의 실행 밖에서 다른 세션과 동시에** 만지려면 `game_mutex`(L2)로 |
| 틱 스레드 | 1 (`app::TickThread`) | (지금은 빈 `on_tick` 호출 + `[TICK ]`/`[TICK2]` 통계뿐) | Session, DB, EntryTable |
| 로거 | 1 | 로그 큐 | 전부 |
| 유휴 스윕 | 0~1 | `last_recv_ms` 읽기, 끊기 | Session 게임 상태 |
| 예약 스윕 스레드 (`app::EntrySweeper`) | 1 | `app::EntryTable` 의 예약 테이블 — **뮤텍스 아래에서만** | `EntryTable` 밖의 어떤 상태도, Session, DB |
| **S2S 스레드** | 0~1 (`[s2s].host` 빈값이면 0) | `S2sConnector` 상태 전부 — 자체 IOCP·매칭 테이블·백오프·송수신 버퍼 (락은 명령 큐 뮤텍스 하나) | Session, DB — 세션 수명(`acquire`/`release`/`io_count`)에 닿지 않는다(ADR-019). `EntryTable::find_session` 이 돌려주는 것도 **id 값뿐**이다 |

**소유 규칙 다섯 줄**

1. **L1**(`app::EntryTable::mutex_` · `LockRank::kRoster`)이 입장 집합(`entered_`)·존 인덱스(`zone_index_`)·예약 테이블(`reservations_`)을 한 뮤텍스로 지킨다. 락은 `EntryTable` 안에서만 잡고 안에서만 푼다 — 호출자는 락을 의식하지 않는다(`entry_table.h:40-43`).
2. **L2**(`net::Session::game_mutex` · `LockRank::kSession`)가 그 세션의 거래 정리 재검증을 지킨다. 두 세션을 동시에 잠글 때는 `std::scoped_lock(a.game_mutex, b.game_mutex)` **하나**로 묶는다 — 가드(`core::LockRankGuard`)도 하나만 씌운다(`core/lock_rank.h:27-30` — 두 개 쓰면 첫 가드가 이미 계층을 올려놔 두 번째가 "같은 계층 재진입"을 역순으로 오판한다).
3. `player_id`·`zone`·`serial_queue`는 그 세션의 **직렬 큐 실행권**을 쥔 스레드만 읽고 쓴다 — 락이 필요 없다. 실행권은 세션당 동시에 하나뿐이라(§3) 그 읽기가 다른 실행과 절대 겹치지 않는다(`session.h:216-221`·`234-237`).
4. `Trade`는 세션이 `std::atomic<std::shared_ptr<world::Trade>>`로 직접 소유한다(`session.h:248`). 락 없이 `load()`하고, 정리할 때만 `game_mutex` 아래 재검증한다(§7-3-A, §4).
5. 락 순서는 **L1 → L2**, 역순 금지. Debug/ASan 빌드는 `core::LockRankGuard`가 스레드마다 지금 쥔 계층을 들고 있다가 역순 획득을 `assert`로 잡는다(`core/lock_rank.h`). `NDEBUG`에서는 비교 한 번을 남기고 전부 no-op이다.

---

## 2. 한 프레임의 경로

```
recv 완료 (I/O 워커)
  └ drain_frames()                     net/iocp_server.cpp
      ├ frame_sizer_  → app::frame_size()          "어디서 자를까"
      ├ frame_pool_.acquire()                      프레임 1개 = 풀 버퍼 1개
      ├ session.io_count += 1                      수명 홀드 획득(1)
      └ job_sink_(session, job)  → workers.submit(session, job)      net/iocp_server.cpp
                                     app::WorkerPool::submit          app/worker_pool.cpp
          ├ sq_mutex 임계구역: serial_queue 에 push, sq_scheduled
          │   false→true 로 전이할 때만 실행권을 공용 큐(core::JobQueue)에 push
          └ 워커가 실행권을 뽑아 drain_serial_queue(session)
              ├ serial_queue 를 도착 순서(deque FIFO)대로 최대 kSerialDrainBatch(16)개 비운다
              ├ 각 Job 실행 → app::on_frame(server, db_pool, entry, session, frame, len)
              │   ├ 인증 불필요 3종(Echo·Ping·Enter) — 앞쪽 switch, 게이트 이전
              │   ├ 게이트: session.player_id == 0 이면 여기서 끊는다(return false)
              │   └ 뒤쪽 switch(msg_id) → 핸들러 (session.zone 을 직접 읽는다)
              ├ 잔량이 있으면 실행권을 큐 뒤에 재제출(공정성) · 없으면 스케줄 홀드 반납
              └ frame_pool_.release(buf) · release_io(sp)   net 이 항상 해 준다
```

**핵심**: `on_frame`은 세션의 존 소속을 인자로 받지 않고 `session.zone`을 직접 읽는다. 그 읽기가 안전한 것은 이 호출 자체가 이미 그 세션의 **직렬 큐 실행권 안**에서 돌기 때문이다 — 실행권은 세션당 동시에 하나뿐이라 이 읽기가 그 세션의 다른 실행과 절대 겹치지 않는다(`frame_router.cpp:1219-1223`). 예전 `placement` 인자와 "다시 읽지 않는다" 계약은 이 실행권 하나로 대체됐다 — 큐를 고른 근거와 일을 한 근거가 갈리는 문제 자체가, 세션마다 큐가 아니라 세션마다 실행권 1개인 구조에서는 성립하지 않는다.

---

## 3. 직렬 큐와 존 라벨

세션마다 직렬 큐(`net::Session::sq_mutex`·`serial_queue`·`sq_scheduled` — `session.h:239-246`)를 둔다. `WorkerPool::submit()`은 프레임을 직렬 큐에 쌓되, `sq_scheduled`가 `false→true`로 전이할 때만 실행권을 공용 큐에 올린다. 워커는 실행권을 뽑아 그 직렬 큐를 도착 순서대로 비운다 — "세션 프레임 순서 = 직렬 큐 순서"가 이렇게 성립한다. **mutex 는 배타이지 순서가 아니다**가 이 설계의 출발점이다 — 같은 세션의 두 프레임을 두 워커가 동시에 뽑으면 처리 순서는 스케줄러 몫이 되어 뒤집힐 수 있는데(존 스레드 시절에는 세션마다 큐가 갈려 있어 이 문제 자체가 없었다), 직렬 큐는 "세션당 in-flight 실행권 1개"로 그 축 자체를 없앤다(`worker_pool.h:7-19`).

push 측(`submit`)과 drain 측(`drain_serial_queue`)은 반드시 같은 `sq_mutex` 임계구역 안에서 `serial_queue`와 `sq_scheduled`를 함께 본다 — 플래그를 별도 atomic CAS로 빼면 덱과 플래그가 다른 락 아래 갈려 실행권이 유실(0 발급)되거나 중복 발급(2 발급)되는 레이스가 생긴다(`worker_pool.h:16-19`).

`kSerialDrainBatch`(16)는 공정성 상한이다. 한 세션이 직렬 큐에 수십 개를 들고 있어도(예: 순서 하네스가 짧은 간격으로 프레임을 밀어 넣는 경우) 워커 하나를 계속 독점하지 못하게, 상한만큼 비우고 잔량이 있으면 실행권 홀드를 쥔 채로 큐 뒤에 다시 선다(`worker_pool.h:21-25`). ✅ **쟀다 — 16 무변경**(ADR-028 · `drain_batch.ps1` · 2026-09-05). 격리 구성(워커 1 · 플러더 1 × 인벤토리 200건 버스트)에서 예약 없는 Echo 프로브의 p99 는 K=1/4/16/64 에 1.15/1.79/2.60/3.85ms 로 **K 에 단조 증가**한다 — 프로브 대기 상한 = K × 단위 작업 비용(로컬 DB 인벤토리 ~0.15ms)이라는 이 절의 설명과 부합한다. 플러드 완료 시간(42/58/40/34ms)의 차이는 같은 K 의 회차 간 편차(≥40%) 안이라 K 가 처리량에 주는 비용은 이 클라로는 못 가른다. 현실 구성(워커 8 · 플러더 10)에서는 다른 워커가 비어 있어 상한이 거의 안 걸린다(p99 0.3~0.7ms · K=64 만 꼬리 5~10ms). ⇒ 16 은 격리 p99 2.6ms · 재제출 12회(K=4 는 49회)의 중간값이라 유지한다. 상한이 실제로 걸렸는지는 신설 `[WORK ] cap_hits`(`worker_pool.cpp` `stop()`)로 본다.

정리 Job(`on_session_gone`)도 직렬 큐 맨 뒤로 들어간다 — 옛 존 FIFO 가 주던 "정리는 그 세션의 마지막 Job" 순서를 직렬 큐가 그대로 승계한다(`frame_router.cpp:1307-1312`). `player_id`·`zone` 비원자 유지의 근거가 이것이다.

**존은 이제 위치 라벨 한 필드다.** `net::Session::zone`(`uint32_t`)이 그 세션이 지금 어느 존에 있는지를 담고, 직렬 큐 직렬화 안에서만 읽고 쓴다 — 락이 필요 없다(`session.h:234-237`). 실제 멤버십의 정본은 `EntryTable`의 존 인덱스(`zone_index_`)다. `handle_join_zone`이 `EntryTable::move_zone()`으로 인덱스를 옮긴 **뒤에** 이 필드를 뒤따라 맞춘다(`frame_router.cpp:481-483`) — 순서가 반대면 다른 실행이 아직 안 옮겨진 인덱스를 보고 잘못된 존으로 취급한다.

---

## 4. 세션 수명 — `io_count` 참조 계수 + 홀드 4계열

`io_count` 메커니즘 자체는 그대로다(ADR-003 존속 — 락 밖 포인터 홀드의 기반). 세션 포인터를 **자기 실행 밖으로** 넘기는 순간 홀드가 필요하다는 규칙도 그대로다:

```
server.acquire_session(s);          // 넘기기 전
... 다른 실행(다른 세션의 실행권 · S2S 스레드)으로 s 를 건네준다 ...
server.release_session(&s);         // 그 일이 끝나면 정확히 1회
```

홀드가 걸리는 자리는 넷이다:

1. **프레임(net)** — `drain_frames`의 `io_count += 1` ~ `release_io`. net 계층이 모든 갈래에서 해 준다.
2. **직렬 큐 실행권**(`WorkerPool`) — `submit()`의 스케줄 전이(`false→true`) ~ `drain_serial_queue`가 그 직렬 큐를 마지막으로 비운 시점(잔량이 남으면 재제출하며 홀드 유지).
3. **명부 등록**(`EntryTable`) — `entry.enter()` 성공 ~ `entry.leave()` 성공. `on_session_gone`이 `leave().ok`일 때만 `release_session`을 부른다(`frame_router.cpp:1313-1315`) — 명부에 이미 없던 세션(Enter 미완료·중복 leave)의 정리는 이 홀드가 애초에 없었으므로 반납도 없다.
4. **거래 상대 일시**(`clear_trade`) — `server.acquire_session(*peer)` ~ 호출자가 통지를 보낸 뒤 `release_session(peer)`(정상 경로) 또는 재검증 실패 시 `clear_trade` 함수 안에서 즉시 `release_session(peer)`(`frame_router.cpp:320-322` — 둘 다 잠근 뒤 `trade` 가 바뀌어 있으면 그 자리에서 반납한다. `:307-315`는 상대가 이미 명부를 떠나 애초에 `acquire` 자체가 없었던 갈래라 release 콜이 없다 — 별개다). `handle_trade_confirm`·`handle_trade_answer`도 각자 상대를 `acquire`했다가 같은 함수 안에서 명시적으로 해제한다.

**모든 실패 경로에서도 정확히 1회** 해제해야 한다는 규칙은 네 계열 전부에 그대로 적용된다.

---

## 5. DB 계층

```
db_pool.try_acquire()   →   실패 시 즉시 kBusy(재큐잉 없음)
                        →   성공 시 DbConn* 를 그 실행(직렬 큐 워커) 안에서 동기로 그대로 쓴다
```

- DB 를 부르는 스레드가 이제 **직렬 큐 워커 자신**이다(`db/db_worker.h:1-8` — 예전 `DbWorkerPool`의 전용 스레드·`player_id % N` 큐 분할·`post_read`/`post_write`는 스케줄러가 직렬 큐 실행권으로 통합되며 통째로 사라졌다). "DB 잡을 다른 큐로 넘긴다"는 개념 자체가 없다. 순서 보장도 큐 분할이 아니라 그 세션의 직렬 큐가 진다 — 같은 세션의 확인·조회는 애초에 순차 실행이라 순서가 어긋날 자리가 없다.
- `db::DbPool::try_acquire()`(신설)는 커넥션을 **락보다 먼저** 빌린다. 유휴 연결을 반환하거나 상한 안이면 새로 만들어 주고, 그마저 안 되면 기다리지 않고 그 자리에서 빈 `Lease`를 돌려준다(`db_pool.h:97` `Lease try_acquire`). 실패한 요청은 재큐잉하지 않고 즉시 `kBusy`로 거절한다 — 재큐잉은 같은 요청이 실패-재시도를 반복하는 기아를 만들기 때문이다.
- **왜 락보다 먼저 빌리는가** — `handle_trade_confirm`이 그 자리에서 `try_acquire`부터 부른다(`frame_router.cpp:907-921`). 양쪽 확인이 완성되는 순간 그대로 DB 를 타야 하는데, 그때는 이미 `game_mutex`를 잡은 뒤라 커넥션을 구하러 가면 **락을 든 채 대기**하게 된다(불변식 5가 막으려는 바로 그 순서). 그래서 확인 여부와 무관하게 미리 빌리고, 안 쓰면 `Lease` 소멸자가 그대로 돌려준다 — 비용은 풀 왕복 하나뿐이다.
- 커넥션 대여 실패 빈도는 `[POOL2] try_failed`로 관측한다.
- `DbConn`의 공개 API·SP 경로(`sp_trade`·`sp_inventory_select`)·트랜잭션 구조·`DEFINER` 권한 모델(ADR-014)·`CALL` 결과셋 소진(`do_sanitize`)은 이번 재구성과 무관하게 그대로다.

### 스키마

`minigame.player` · `minigame.inventory(PK: player_id,item_id)` · `minigame_log.trade_log`

**저장 프로시저** (`sql\03_procedures.sql` · 둘 다 `DEFINER='sp_owner'@'localhost'`):
`minigame.sp_trade(...)` — 거래 1왕복 · `minigame.sp_inventory_select(player_id)` — 조회 1왕복
배포는 사람이 한다 — `mysql -u lapsix -p... minigame < sql\03_procedures.sql`. 서버 계정에는 DDL 권한이 없다.

---

## 6. 프로토콜 현황

`[ body_size:u16 ][ msg_id:u16 ][ body ]` 빅엔디언, 본문 상한 4096B. **와이어 변경은 0** — 이번 재구성은 스레드·락 구조만 바뀌고 메시지 표는 한 바이트도 안 바뀐다.

| 요청 (1~) | body | 로그인 필요 | 존 필요 |
|---|---|---|---|
| 1 `kEchoReq` | 임의 | X | X |
| 2 `kJoinZoneReq` | `zone_id:u32` | X | X |
| 3 `kChatReq` | `[type:u8][target:u64 — Whisper 만][text]` | X | O* |
| 4 `kInventoryReq` | 없음 | **O** | X |
| ~~5~~ | ⛔ `kLoginReq` 폐지 — 번호 5 / 105 는 비워 두고 재사용하지 않는다 | — | — |
| 6~10 `kTrade*Req` | 각각 다름 | **O**(Req 만 검사) | O |
| 11 `kPingReq` | 없음 | X | X |
| 13 `kEnterReq` | `player_id:u64` | — (로그인을 겸한다) | X |

「로그인 필요」 열은 게이트 하나로 결정된다 — `on_frame`의 배정 스위치 진입부가 `session.player_id != 0`을 보고 아닌 세션을 끊는다. 위 표의 `O`/`X`는 핸들러별 검사가 아니라 그 스위치 안에 있는가를 뜻한다. `kEchoReq`·`kPingReq`·`kEnterReq`는 스위치 앞이라 예외다.

\* `kChatReq`의 「존 필요」는 type 마다 갈린다 — Zone(0)만 O, All(1)·Whisper(2)는 X다(6단계 신설). All 은 명부 전원(zone 무관)을 스냅샷하고, Whisper 는 대상을 `player_id`로 직접 찾아 존 소속을 안 본다.

응답은 101~115. 미정의 ID 는 세션의 `bad_msg_score`를 올리고(미정의 +1 · 서버전용 +4), 32 를 넘으면 끊는다.

`kEnterReq = 13` / `kEnterAck = 114` — body 는 `player_id:u64` / `result:u8 · player_id:u64 · session_id:u32`. `is_client_request()`에 들어간다(`is_session_client_request()`가 아니다 — ADR-021 결정 5). `kZoneMembersNtf`는 u32 그대로다(ADR-021 결정 8).

`kChatNtf = 103` — body: `[type:u8][from:u64 = player_id][text]`. `kChatAck = 115`(6단계 신설) — body: `[result:u8]`, **실패 시에만**(성공은 무응답 — `kTradeAck` 선례와 동형). 귓속말 대상 부재는 `kNoPeer`를 재사용한다.

세션 서버 전용 클라 요청 — `kSessionLoginReq = 12`(body `player_id:u64`) · `kSessionLoginAck = 113`(body `result:u8 · port:u16 · host:str16`). `is_client_request()`에 넣지 않는다 — 세션 서버는 `is_session_client_request()`(Login + Ping)로 판정한다. 응답은 `is_server_message()`에 들어간다.

### S2S 프로토콜 (`proto/s2s_packet.h`)

`[ body_size:u16 ][ msg_id:u16 ][ seq:u32 ]` 빅엔디언 8B 헤더, 본문 상한 65,535B. `seq`는 요청–응답 매칭 키. 코덱이 구현된 메시지는 17종 — `Register`/`RegisterAck`/`Heartbeat`/`HeartbeatAck`/`Unsupported` · `Unregister`/`UnregisterAck`/`Reserve`/`ReserveAck` · `PlayerEnter`/`PlayerLeave`/`FullSync` · `SetMode`/`SetModeAck` · `DrainComplete` · `Kick`/`KickAck`. 커넥터는 `net::S2sConnector`(자체 IOCP·전용 스레드 1 — §1 표), 마을 쪽 절차는 `app::S2sLink`다. `[s2s].host`가 빈값(커밋 기본)이면 통째로 비활성이다.

`PlayerEnter`/`PlayerLeave` 발신은 `EntryTable::enter()`/`leave()`가 성공한 **바로 그 임계구역 안**에서 나간다 — `entry.set_notifier()`로 꽂은 콜백이 `S2sLink::notify_player_enter/leave`를 커넥터 명령 큐에 push_back 하는 것까지만 한다(`bootstrap.cpp:354-362`, `entry_table.h:45-48`). 알림 제출과 등록/제거 사이에 다른 실행이 끼어들 여지를 명부 자신의 락 안으로 옮겨 없앤 것 — 옛 `frame_router.cpp`가 직접 부르던 세 호출 지점(이월 1)이 이렇게 흡수됐다.

`Unregister`는 알림이 아니라 요청이다(`UnregisterAck=0x8202`가 실재). 마을은 `unregister_and_wait()`로 실제로 기다린다.
`FullSync`는 집합이 비어 있어도 `chunk_total=1, count=0`으로 한 번 보낸다. 청크당 최대 510개.

---

## 7. 불변식 — 어기면 조용히 깨진다

1. **락 순서는 `L1(EntryTable) → L2(Session::game_mutex)`. 역순 금지.** Debug/ASan 은 `core::LockRankGuard`가 감시한다(`core/lock_rank.h`) — 역순 획득은 assert.
2. **전역 락(L1)을 든 채 DB·송신 금지.** `EntryTable`의 임계구역 안에서 하는 일은 맵 갱신과 알림 콜백(커넥터 명령 큐 push_back)뿐이다. S2S 알림은 그 push_back 까지만 하고 실송신은 별도 스레드가 한다(`entry_table.h:189-201`).
3. **세션 포인터를 자기 실행 밖으로 들고 나가면 `acquire`, 모든 갈래에서 정확히 1회 `release`.**(§4의 홀드 4계열 그대로 — ADR-003 존속)
4. **같은 세션의 프레임·정리 Job 순서는 직렬 큐가 보장한다.** `player_id`·`zone`은 그 세션의 직렬 큐 실행권 안에서만 만진다 — 다른 스레드가 직접 읽거나 쓰지 않는다(§3).
5. **커넥션은 락보다 먼저 빌린다(`try_acquire`).** 못 빌리면 락을 잡기 전에 `kBusy`로 끝낸다(§5).
6. **body 길이를 먼저 검증한다.** 고정 길이면 `!=`로 정확히(`bad_body` 헬퍼 사용 → false 반환 시 세션 종료). **응답 본문이 4096B 를 넘지 않게 자른다.**
7. **`Trade`는 두 세션의 `atomic<shared_ptr<Trade>>`가 소유한다.** 정리 지점은 넷 그대로다 — 취소 / 거절 / 존 이동(`handle_join_zone`) / 접속 종료(`on_session_gone`) — 전부 `clear_trade`(또는 거절 전용 인라인 갈래)가 `std::scoped_lock(session.game_mutex, peer->game_mutex)` 아래에서 처리한다. §7-3-A 의 유일한 재검증: 락 없이 `load()`해 상대를 알아낸 뒤 상대를 `acquire`해 락 밖 반출을 정당화하고, 둘 다 잠근 뒤에야 그 사이 자기 `trade`가 바뀌지 않았는지 다시 본다(`frame_router.cpp:284-290`).
8. **부하 주입 스위치는 항상 0/false.** `frame_router.cpp`(`kLogicDelayMs`·`kBadSyncDbMs`·`kBadTradeNoTx`) · `worker_pool.cpp`(`kBadTickWorkMs`·`kBadTickSpikeEvery`·`kBadTickSpikeMs` — 존 스레드 시절 `zone_manager.cpp`에 있던 것을 그대로 승계).
9. **`send_msg` 밖에서 바이트를 직접 만들지 않는다. 헤더 인코딩은 `proto::encode_header`를 통해서만** — **마을** 기준 호출부는 `send_msg`와 브로드캐스트 프레임 조립 헬퍼(`build_frame`) 두 곳이다(6단계 신설 — §17-2 ①의 직렬화 1회 최적화. 손 조립 금지·인코딩 로직 한 곳이라는 취지는 그대로다). **세션 서버**는 이 두 곳과 별개로 자체 `send_client_msg`(`session_router.cpp`)가 세 번째 호출부다(§7 「세션 서버 판」 참조) — 두 서버가 각자의 인코딩 경로를 하나로 유지한다는 뜻이지, 저장소 전체에 정확히 두 곳뿐이라는 뜻이 아니다.
10. **SQL 은 prepared statement로만.** DB 잡·SP 규칙은 유지하되, 그 "잡"의 실행자가 이제 직렬 큐 워커 자신이다(§5).

> **S2S 판** — `S2sConnector` 상태는 S2S 스레드만 만진다. 다른 스레드는 `request`/`notify`/`respond`(명령 큐 + PQCS 웨이크)로만 접근한다. `force_disconnect` 만 S2S 스레드 위에서 동기 즉시다. 모든 `request()`는 완료 콜백을 정확히 1회 받는다(갈래 넷: 정상·타임아웃·끊김·stop). S2S 헤더 인코딩은 `proto::s2s::encode_header`/`decode_header` 한 곳.
>
> **세션 서버 판 (`session.exe`)** — 세션 서버의 테이블(레지스트리·예약·pending)은 Registry 뮤텍스 아래에서만 만진다. S2S 수용 연결의 `Session*`는 Registry 가 영구 홀드로 보관한다(획득 1곳·해제 4갈래). 종료 순서는 클라 서버 → S2S 서버 → router 정리. Registry 뮤텍스를 든 채 S2S 링크로 send 하는 것은 이 exe 에서는 허용 패턴이다(`registry.h`의 `link_of` 계약) — 마을의 "L1 을 든 채 DB·송신 금지"(불변식 2)는 **마을의 전역 락** 얘기라 세션 서버에는 그대로 적용되지 않는다.
>
> **마을 판 (`app::EntryTable`)** — 그 집합은 `player_id → {Session*, session_id, zone, in_zone}`을 담는다(ADR-021 결정 2 문언 정정 — "`Session*`를 담지 않는다" → "**S2S 스레드 경로의 API는 id 값만 노출한다**"). `Session*`를 돌려주는 API(`find_acquire_by_session_id`·`snapshot_zone`·`snapshot_all`)는 전부 직렬 큐 워커만 부른다는 계약이고, 반환 전에 그 세션의 `io_count`를 올려(acquire) 락 밖으로 들고 나가도 안전하게 만든다(`entry_table.h:9-13`). `leave(pid, sid)`는 소유자(`session_id`)가 일치할 때만 지운다 — 그러지 않으면 Kick 당한 옛 세션의 늦은 정리 Job 이 이미 재입장한 새 세션의 항목을 지울 수 있다(동기 Kick 이후 실전 경로 — `entry_table.h:123-130`).
> - `EntryTable::mutex_`가 이 저장소 최초의 "전역 테이블 + 자체 락"이라는 사실은 그대로다 — 절대 규칙("Zone 안에 락을 넣지 않는다")은 이제 `Zone` 이라는 클래스 자체가 없으므로 문언이 성립하지 않는다. 다중 진입은 여러 직렬 큐 워커·S2S 스레드·예약 스윕 스레드 셋이 동시에 이 테이블을 만질 수 있다는 사실에서 온다.
> - `EntryTable::find_session`(id 값 반환 · `Session*` 아님) → `IocpServer::close_by_id`(sweep 과 같은 「락 안 홀드 → 락 밖 close → release_io」 3단계 · S2S 스레드가 유일 호출자). 종료 안전은 `main.cpp`가 `s2s_link.stop()`(S2S 스레드 동기 join)을 `server.stop()` 앞에 두는 순서로 확보한다(`main.cpp:216-229`).
> - **`with_snapshot`** — FullSync 의 스냅샷 복사와 청크 제출을 `mutex_` 아래에서 함께 실행해 증분 알림과의 큐 순서 역전을 막는다(제출은 커넥터 명령 큐 push_back 뿐).
> - `PlayerEnter`/`PlayerLeave` 발신은 `started_` 가드 뒤에 있다. 링크가 비활성인 환경(`[s2s].host` 빈값)에서 프레임이 하나도 안 나가는 것이 회귀 8종 무회귀의 전제다.
>
> ⚠️ **`EntryTable::mutex_`의 필요성은 현재 하네스로 결정적 검증이 불가능하다** — ASan 은 데이터 레이스를 보지 않는다. 근거는 위 "다중 진입이 구조적이다"이지 실측이 아니다(`docs/TESTING.md` "하네스가 구조적으로 못 덮는 것").

## 8. 확장 여지 — 지금 비어 있는 자리

| 자리 | 위치 | 메모 |
|---|---|---|
| 틱 로직 | `app::TickThread::run()`(`worker_pool.cpp`) — 지금은 `[TICK ]`/`[TICK2]` 통계만 낸다 | 전용 틱 스레드 1개가 30Hz 로 돈다. 세션·DB 를 안 만지므로 상태 갱신을 넣으려면 EntryTable 스냅샷 경유(락 규율 §7 준수) |
| 인증 | `handle_enter`의 예약 확인 옆 | 토큰 검증이 들어갈 자리. 지금은 세션 서버의 `SessionLogin`이 `player_id`를 그대로 믿고, 마을은 그 결과인 예약만 확인한다 |
| ~~존 멤버 목록~~ | ✅ **채워졌다** — `kZoneMembersNtf`(112)가 멤버 `session_id` 목록을 내려준다 | 거래 상대 지목이 이것으로 성립한다(`packet.h` 의 그 주석이 이 메시지가 생긴 이유다). 4개 변동 지점이 전부 통지를 내는지는 `members.ps1` 11항목이 검증한다. ⚠️ 남은 한계는 「존에 있는 채로 로그인하면 `player_id` 가 낡는다」뿐이다(README 「알려진 한계」) |
| 인벤토리 푸시 | 거래 성공 후 클라가 다시 조회해야 함 | `kTradeAck` 뒤 자동 `kInventoryAck` 푸시 가능 |
| 존 인덱스 분리 | `EntryTable::zone_index_`가 L1 과 한 뮤텍스 | ✅ **6단계에서 재평가 완료(A11 폐합)** — 명부 스냅샷형 임계구역을 `std::mutex` vs `std::shared_mutex`로 실측(N=100/1,000/4,096), 최악 N 에서도 임계구역이 수백 μs 미만이고 `shared_mutex` 이득이 없어 **`std::mutex` 유지·인덱스 L1 안 유지로 확정**(근거·수치는 ADR-026) |
| 단위 테스트 | 없음 | 순수 함수(`frame_size`, `world::Trade` 순수 메서드, 락 순서 정렬)는 테스트 가능 |
| 비밀번호 평문 | `config/server.ini` | 환경변수 우선 읽기로 대체 가능 |

---

## 9. 빌드·검증 진입점

- 빌드: `scripts\build.ps1` (Debug · Release · ASan, 경고 0 기준선)
- 새 `.cpp`/`.h` 는 그 파일이 속한 프로젝트(`server\common.vcxproj` · `server\village.vcxproj` · `server\session.vcxproj`)의 `<ClCompile>`/`<ClInclude>` 항목에 **손으로 추가**해야 한다(와일드카드 아님). `.filters` 도 같이. `src/{core,net,db,ops,bench,proto}` → common · `src/{app,world}`와 `src/main.cpp` → village · `src/session/` → session. 빌드 플래그를 바꿀 때는 `server\common.props` 한 곳만 고친다. `.sln`의 `ProjectConfigurationPlatforms`는 프로젝트 x 3구성 x 2줄 — 빠지면 그 구성이 솔루션 빌드에서 조용히 빠진다.
- 회귀: `send.ps1` → `zone.ps1` → `members.ps1` → `inventory.ps1` → `zone_block.ps1`(워커-풀 격리로 재정의) → `trade.ps1` → `zone_race.ps1`(락 검사기 assert 관찰로 재목적) → `churn.ps1` → `s2s.ps1` → `session.ps1`(뒤 둘은 서버를 스스로 스폰하는 단독 실행)
- 종료 로그 `[TICK ]` `[POOL ]`(직렬 큐 워커가 동기로 문 `try_acquire` 실패/성공 집계는 `[POOL2]`) `[CONN ]` `[NET  ]` `[ALLOC]` 가 판정 지표 — S2S 를 켠 실행은 `[S2S  ]`, 세션 서버는 `[SESS ]` 요약이 추가된다.
