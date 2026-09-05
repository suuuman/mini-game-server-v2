# 코딩 규칙 — mini-game-server

> `docs/ARCHITECTURE.md` §7 불변식이 **「어기면 깨지는 것」**이라면,
> 이 문서는 **「이 저장소가 코드를 쓰는 방식」**이다.

---

## 0. 절대 규칙 — 어기면 조용히 깨진다

⛔ 아래 열 가지는 **Release 구성에서 증상이 안 나타나는 종류**다. Debug 에서 assert 로 잡히거나,
아무 데서도 안 잡히고 며칠 뒤 운영에서 터진다. 고치기 전에 이 절을 먼저 읽는다.

1. 락 순서는 **`L1(app::EntryTable::mutex_) → L2(net::Session::game_mutex)`**. 역순 금지 —
   Debug/ASan 구성은 `core::LockRankGuard`(`core/lock_rank.h`)가 스레드마다 지금 쥔 계층을 들고
   있다가 역순 획득을 assert 로 잡는다. 두 세션을 동시에 잠글 때는
   `std::scoped_lock(a.game_mutex, b.game_mutex)` 하나로 묶고 가드도 하나만 씌운다.

2. **L1(전역 락)을 든 채 DB·송신을 하지 않는다.** `EntryTable` 의 임계구역 안에서 하는 일은
   맵 갱신과 알림 콜백(커넥터 명령 큐 `push_back`)뿐이다 — S2S 실송신은 별도 스레드가 한다.

3. 세션 포인터를 자기 실행 밖으로(다른 세션의 실행권 · S2S 스레드) 태우기 전
   `server.acquire_session()`, **모든 갈래(정상 · 재검증 실패 · 큐/스케줄 실패)에서 정확히 1회**
   `server.release_session()`. 홀드는 프레임(net) · 직렬 큐 실행권(`app::WorkerPool`) ·
   명부 등록(`EntryTable::enter`~`leave`) · 거래 상대 일시(`clear_trade`) 네 계열이다.

4. 같은 세션의 프레임·정리 Job 순서는 **직렬 큐**(`net::Session::serial_queue`·`sq_mutex`·
   `sq_scheduled`)가 보장한다. `player_id`·`zone` 은 **그 세션의 직렬 큐 실행권을 쥔 스레드만**
   읽고 쓴다 — 다른 스레드가 직접 만지지 않는다.

5. 직렬 큐 워커 안에서 동기 DB 호출은 **정상 경로다** — 다만 커넥션은 **락보다 먼저**
   (`db::DbPool::try_acquire()`) 빌린다. 못 빌리면 락을 잡기 전에 `kBusy` 로 끝낸다.
   재큐잉은 하지 않는다(재큐잉은 기아 위험을 데려온다 — ADR-025).

6. `app::EntryTable` 안에만 락을 둔다. 이 저장소 유일의 「전역 테이블 + 자체 락」이고,
   다중 직렬 큐 워커 · S2S 스레드 · 예약 스윕 스레드가 동시에 진입하는 것이 전제다 —
   호출자는 락을 의식하지 않는다.

7. 새 요청 `MsgId` 는 `proto::is_client_request()` 에 추가한다.
   ⭐ 단 **세션 서버 전용 요청**은 `proto::is_session_client_request()` 다(ADR-020 — 두 서버의
   요청 표를 섞지 않는다). 응답은 어느 쪽이든 `is_server_message()` 에 넣는다.
   ⚠️ **효과가 나는 곳을 정확히 알고 써라** — `is_client_request()` 는 **호출부가 0건**이고,
   마을은 `on_frame` 의 switch 로 직접 가른다. ⛔ **그 표에만 넣고 스위치에 안 넣으면 아무 일도
   일어나지 않는다.** 실제로 불리는 것은 `is_session_client_request()`(`session_router.cpp`)와
   `is_server_message()`(`frame_router.cpp` · `session_router.cpp`) 둘이다.

8. 핸들러는 `body_len` 을 먼저 검증한다(`bad_body` 헬퍼). 응답 본문은 `kMaxBodySize`(4096B)를
   넘지 않게 자른다.

9. SQL 은 prepared statement 로만 추가한다. 문자열 연결로 만들지 않는다.

10. **부하 주입 스위치는 항상 꺼진 상태로 커밋한다** —
    `frame_router.cpp`(`kLogicDelayMs`·`kBadSyncDbMs`·`kBadTradeNoTx`) ·
    `worker_pool.cpp`(`kBadTickWorkMs`·`kBadTickSpikeEvery`·`kBadTickSpikeMs`).
    ⚠️ `server.ini` 의 `idle_timeout_sec` 는 이 목록이 **아니다** — **`90` 이 커밋 의도값**이고
    `0` 이 아니다(ADR-023). 자체 스폰 하네스는 스크래치 config 에서 `0` 으로 오버라이드해 이
    값의 영향을 안 받는다(`docs/TESTING.md` §0).

### 계층과 파일 등록

- 의존 방향은 한쪽이다 — `world → proto → net → core` · `db → core` · `ops → core` ·
  **`session → proto → net → core`(`db` 는 링크하지 않는다 — ADR-020 결정 7)**.
  `app` 만 전부를 안다. 역방향 include 금지.
  ⚠️ **`session` 줄이 오래 빠져 있었다** — 세션 서버는 레지스트리·예약·접속 테이블을 전부 프로세스 메모리에 두므로 DB 계층이 아예 안 붙는다는 것이 이 줄의 요지다.
- 새 `.cpp`/`.h` 는 **그 파일이 속한 프로젝트에** 손으로 등록한다(와일드카드가 없다) —
  `src/{core,net,db,ops,bench,proto}` → `server\common.vcxproj` ·
  `src/{app,world}` 와 `src/main.cpp` → `server\village.vcxproj` ·
  `src/session/` → `server\session.vcxproj`. `.filters` 도 같이,
  ⚠️ **`.sln` 구성 6줄까지** — 빠지면 그 구성이 조용히 빠진다.
  ⛔ **헤더 전용 추가가 가장 위험하다** — `<ClInclude>` 에 안 넣어도 `#include` 로 끌려와
  빌드가 성공하므로 아무 증상이 없다.
- 빌드 플래그는 `server\common.props` 한 곳만 고친다.

### 새 장치를 만들기 전에 통과시키는 질문 (`docs/DESIGN-server-split.md` §18-7)

> **그게 실제로 누구에게 어떤 손해를 주나?**
> 손해가 없으면 · 유저가 스스로 벗어날 수 있으면 → **장치도 없다.**
> 자원이면 **정량으로** 따진다 · 정합성이면 그때 만든다.

이 설계에서 **다섯 번** 이 판단을 빠뜨려 없는 문제에 장치를 만들었다.
**장치는 공짜가 아니다** — 새 실패 경로 · 새 미정 항목 · 새 위험을 데려온다.
그리고 **안 잰 것을 잰 것처럼 쓰지 않는다.** 문서의 `⚠️ 안 쟀다` 표시는 잰 뒤에 지운다.

---

## 1. 주석 — 이 저장소의 정체성

**주석이 코드의 절반이고, 그게 의도된 것이다.**

### 무엇을 적는가

| 적는다 | 적지 않는다 |
|---|---|
| **왜** 그렇게 했는가 | 무엇을 하는가 (코드가 이미 말한다) |
| 대안을 왜 버렸는가 | 자명한 동작 설명 |
| **실제로 겪은 사고** | 가상의 위험 |
| 어겼을 때 나타나는 **증상** | 일반론 |
| 잰 값 / 안 잰 값의 구분 | 근거 없는 성능 주장 |

### 표기 관례

| 기호 | 뜻 |
|---|---|
| `★` | 설계 판단의 핵심. 여기를 잘못 읽으면 전체를 잘못 읽는다 |
| `★★` | 그중에서도 이 파일의 존재 이유에 해당하는 것 |
| `⚠️` | 함정. 실제로 걸렸거나 걸릴 수 있는 것 |
| `⚠️⚠️` | 걸리면 증상과 원인이 멀어서 추적이 오래 걸리는 것 |

### 사고 기록의 형식

실제로 겪은 것은 **증상 → 원인 → 그래서 지금 코드가 어떻게 생겼는지** 순으로 적는다.

> 예 (`packet.h`) — ping 을 `is_client_request` 에 빠뜨렸을 때:
> 「ping 이 미정의 ID 로 떨어져 `bad_msg_score` 가 30초마다 1씩 오르고,
> `kBadMsgLimit`(32)에 닿는 16분 뒤에 멀쩡한 세션이 끊긴다.」

**⛔ 「최적화했다」 「빠르다」 같은 서술은 잰 값 없이 쓰지 않는다.**
안 쟀으면 안 쟀다고 적는다 — `config/server.ini` 의 *"20코어에서 4가 맞는지는 잰 적이 없다"* 가 그 예다.

---

## 2. 명명

| 대상 | 규칙 | 예 |
|---|---|---|
| 상수 | `k` + PascalCase | `kMaxBodySize` · `kHeaderSize` |
| 부하 주입 스위치 | `kBad*` 또는 `k*DelayMs` | `kBadTradeNoTx` · `kLogicDelayMs` |
| 멤버 변수 | snake_case + `_` | `frame_pool_` · `worker_count_` |
| 지역/인자 | snake_case | `body_len` · `player_id` |
| 함수 | snake_case | `send_msg` · `placement_zone` |
| 타입 | PascalCase | `IocpServer` · `TradeOrder` |
| 네임스페이스 | 소문자 | `net` · `proto` · `world` · `db` · `core` · `ops` · `app` |
| 메시지 ID | `k` + 이름 + `Req`/`Ack`/`Ntf` | `kTradeSetItemReq` · `kTradeStateNtf` |

**메시지 ID 대역**: 요청 1번대 · 응답/통지 101번대. 로그에서 방향이 바로 보인다.

---

## 3. 오류 처리

### 반환값의 뜻을 고정한다

| 문맥 | `true` / 양수 | `false` / 음수 |
|---|---|---|
| 핸들러 (`handle_*`) | 세션 유지 | **이 세션을 끊어라** |
| `frame_size` | 프레임 길이 | `0`=더 받아야 함 · `<0`=규약 위반 |
| `post` 계열 | 큐에 들어감 | 큐가 닫혔거나 가득 참 |

**⛔ 핸들러가 `false` 를 반환하는 것은 「프로토콜 위반」일 때만이다.**
게임 로직상의 실패(잔량 부족·상대 없음)는 `ResultCode` 로 답하고 `true` 를 반환한다.

### 결과 코드는 「분류」가 아니라 「클라이언트의 반응」으로 나눈다

`proto::ResultCode` 참조 — `kNotEnough`(정상 게임 결과) · `kInvalidArg`(재시도 무의미) ·
`kBusy`(재시도 유의미) · `kDbError`(운영이 알아야 함)를 한 코드로 뭉치지 않는다.

### 실패와 빈 결과를 반드시 구분한다

DB 질의가 실패했는데 빈 목록을 돌려주면 클라이언트는 「아이템이 없다」로 받는다.
유저에겐 아이템이 사라진 것이다.

---

## 4. 자원 수명

### RAII 를 쓴다

수명이 걸린 것을 손으로 정리하게 두지 않는다 (`HiResTimer` 가 그 예 — `server.start` 실패 경로가 있어서).

### 세션 홀드는 「짝」이다

```cpp
server.acquire_session(s);      // 넘기기 전
... 다른 큐로 태워 보냄 ...
server.release_session(&s);     // 모든 갈래에서 정확히 1회
```

**⛔ 새 실패 경로를 만들 때마다 `release` 를 빠뜨리지 않았는지 센다.**
`handle_trade_confirm` 이 네 갈래에서 해제하는 이유가 이것이다.

---

## 4-b. 짝 API — 한쪽만 고치면 반드시 깨진다

아래는 **반드시 쌍으로 존재해야 하는** 연산들이다.
한쪽을 고치거나 새 경로를 만들면 **다른 쪽도 같은 수의 갈래를 가져야 한다.**

⚠️ **표를 주제별로 아홉으로 나눴다 — 행 41개는 그대로다.**
한 표에 41행이 있으면 「내 변경이 어느 짝을 건드리나」를 찾는 데만 시간이 든다. 나눈 축은 **누가 그 짝을 깨뜨릴 수 있는가**다.

### ① 세션 수명 홀드 — `acquire` ↔ `release`

⛔ **개수가 같다는 것은 근거가 못 된다.** 한 갈래에서 두 번 해제하고 다른 갈래에서 안 하면 개수는 맞는다 — **분기마다** 센다.

| A | B | 대조 방법 | 비대칭이 허용되는 경우 |
|---|---|---|---|
| `acquire_session()` | `release_session()` | **갈래마다** 센다. 개수만 세면 안 된다 | 없다 — 정확히 1:1 |
| ⭐ 명부 등록 홀드 — `EntryTable::enter` 성공 직후 **호출자**(`handle_enter` 의 `server.acquire_session` — `frame_router.cpp:399`)가 잡는다(`entry_table.cpp` 의 `acquire_hold` 는 snapshot·find 계열 전용 — enter 는 그것을 부르지 않는다) | `leave` 가 `true` 를 낼 때(`result.ok`) `server.release_session()` | enter 성공 1 : leave 성공(`ok=true`) 1 — 이관 후에도 **모든 갈래에서 정확히 1:1**(옛 `Zone::add`/`remove` 관례의 이전처) | 없다 — `leave` 가 `ok=false`(Enter 미도달·소유자 불일치)면 애초에 홀드가 없었으므로 release 도 없다 |
| ⭐ `EntryTable::find_acquire_by_player_id()`(락 안에서 acquire — 직렬 큐 워커 전용, S2S 스레드 금지) | 호출자의 `server.release_session()` 정확히 1회 | 갈래마다 센다 — 없으면(부재) 0:0, 있으면(성공·송신 실패 갈래 포함, 반환값과 무관) 1:1 | 없다 — `find_acquire_by_session_id` 와 같은 계약(로직 워커 전용·acquire 후 반출·호출자가 정확히 1회 release — `entry_table.h` 주석 동형) |
| `send_chunks` 의 킥 `close_session()` 호출(새 홀드를 세우지도 반납하지도 않는다) | **호출자가 이미 쥔 홀드**가 이 함수 실행 동안 세션을 살려 둔다는 전제 — 자기 응답 송신은 프레임 Job 홀드, 브로드캐스트·귓속말 대상은 `snapshot_zone`/`snapshot_all`/`find_acquire_by_player_id` 의 acquire_hold | 홀드 없이 `send_chunks` 를 부르는 새 경로가 생기면 이 전제가 깨져 UAF | ⚠️ 이 킥은 기본 꺼짐(`send_overflow_limit=0`)이고 마을만 config 로 옵트인한다 — 락을 쥔 채 송신하는 것이 허용 패턴인 서버(세션 서버)에서 켜면 `session_gone_` 이 같은 락을 재획득해 자기 데드락이 난다 |
| ⭐ 세션 서버 `acquire_session()`(S2S 수용 link 보관) | `release_session()` — **4갈래**(unregister 탈착분 · session_gone 탈착분 · stale 회수(부활/sweep) · attach 실패 즉시 상쇄) | 갈래마다 센다. **탈착은 Registry 뮤텍스 안 · release 는 밖**이 배타의 근거다 | ⚠️ **stop 잔존은 release 하지 않는다**(사후 release 는 UAF) — 감사 로그만. 이 다섯 번째 경로를 「해제」로 세지 않는다 |
| ⭐ `IocpServer::close_by_id` 의 `io_count.fetch_add`(락 안 · closing 아닐 때만) | `release_io`(같은 함수 락 밖) | 갈래 셋(발견 · closing 선점 · 부재) — **closing 선점·부재 갈래는 홀드를 안 올리므로 반납도 없다** | 없다 |

### ② 직렬 큐와 명부(L1)

이 저장소 유일의 「전역 테이블 + 자체 락」이 명부다. 인덱스를 만지는 API 는 셋뿐이고, 그 밖에서 만지는 코드가 없어야 한다.

| A | B | 대조 방법 | 비대칭이 허용되는 경우 |
|---|---|---|---|
| ⭐ `WorkerPool::submit()` 의 `serial_queue.push_back` + `sq_scheduled(false→true)` 전이(그 전이가 일어난 push 만 실행권을 큐에 올린다) | `WorkerPool::drain_serial_queue()` 의 잔량 확인 + `sq_scheduled=false` 해제(잔량 있으면 재제출로 홀드 유지) | **push 측·drain 측 둘 다 같은 `sq_mutex` 임계구역**인가 — 재확인은 **배치를 전부 실행한 뒤**여야 한다(`worker_pool.cpp:156-172` — 팝 직후에 내리면 다른 워커의 push 가 실행권을 중복 발급한다) | 없다 — 플래그를 별도 atomic CAS 로 빼면 덱과 플래그가 다른 락 아래 갈려 실행권 0(유실)·2(중복) 레이스가 재현된다(M1 뮤턴트가 `send -Seq 3000` 순서 어긋남으로 실증) |
| ⭐ `EntryTable::enter`/`leave`/`move_zone`(`zone_index_` 를 만지는 유일한 API 셋) | `zone_index_` 갱신 그 자체 — 이 세 API **밖에서** 인덱스를 만지는 코드는 없어야 한다 | 전수표(`entry_table.cpp`) — `enter`: `entered_` 삽입만(`in_zone=false`, 인덱스 안 만짐) / `leave`: `entered_` 제거 + `in_zone` 이면 `zone_index_` 도 같은 임계구역에서 제거 / `move_zone`: 옛 버킷 제거 + 새 버킷 등록 + `Entry::zone`·`in_zone` 갱신을 **한 임계구역**에서 | 없다 — `move_zone` 을 두 지점에 나눠 부르면(원존 CAS 직후 + 목적지 Job) 「인덱스만 먼저 새 존」 창이 생긴다(단일 호출 지점 규칙 — `TESTING.md` 「하네스가 구조적으로 못 덮는 것」 참조) |
| ⭐ `app::EntryTable::enter(pid, sid)` | `leave(pid, sid)` | 갈래마다 센다. **`Enter` 실패 갈래에서는 삽입이 없어야** 한다. ⛔ **둘 다 `session.id` 를 인자로 받는 것이 계약이다** — 소유자가 일치할 때만 지운다 | ⚠️ **허용된다 — 다만 사유가 4단계에서 바뀌었다.** 예전에는 「`kLoginReq` 직행 세션이 집합에 없어서」였는데 그 경로가 폐지됐다. 지금 `leave()` 가 false 를 내는 경우는 **`Enter` 에 도달하지 못한 채 끊긴 세션**(예약 없음·만료·중복 거절·body 오류)과 **소유자 불일치**다. ⛔ 어느 쪽이든 **그때 `PlayerLeave` 를 보내지 않는 것**이 계약이고, 그 계약은 그대로다 |
| ⭐ `app::EntryTable::add_reservation()` (Reserve 수신 1곳 — **단, 드레인 중이면 그 1곳조차 안 불린다**: `s2s_link.cpp` 의 Reserve 처리가 `entry_->draining()` 을 먼저 보고 `kResultDraining` 으로 즉시 거절하며 `add_reservation` 을 건너뛴다) | 소멸 **3경로** — `consume_reservation` 소비 · `sweep_expired` 만료 · 덮어쓰기 | 삽입 1(조건부) · 소멸 3 이 상호 배타인가 | 없다 — 드레인 거부 갈래는 **애초에 삽입이 없으므로** 소멸 3경로 어디에도 안 걸린다 |
| ⭐ `EntryTable::with_snapshot`(스냅샷+청크 제출을 `mutex_` 아래에서) | ⚠️ **정정** — 이제 존 스레드가 아니라 **워커 직렬 큐 실행권을 쥔 스레드**가 부르는 `enter`/`leave`/`move_zone`(전부 같은 `mutex_`) | 역순 획득 경로 0(`commands_mutex_` 3곳 전부 벡터 조작만 — 데드락 축 없음)이 전제 계약 | 없다 — fn 이 「커넥터 큐 push_back」 이상을 하면 이 전제가 깨진다(entry_table.h 주석) |

### ③ 거래 · 버퍼 · 락 가드

전부 RAII 또는 원자 소유다. 정리 지점의 **개수**가 계약이다.

| A | B | 대조 방법 | 비대칭이 허용되는 경우 |
|---|---|---|---|
| ⭐ `make_shared<world::Trade>()` 를 **양쪽 세션**의 `trade`(`atomic<shared_ptr>`)에 `store`(생성 1곳 — `frame_router.cpp:723-730`) | 정리 **5갈래** — `clear_trade()` 헬퍼 경유 3곳(JoinZone·연결 종료·`on_session_gone`) + Answer 거절 인라인 + Confirm 인라인 | 5갈래 전부 **`scoped_lock(a.game_mutex, b.game_mutex)` 아래 + 재검증**(`trade.load()==t`) 뒤에만 clear 하는가(§7-3-A) | 없다 — 재검증 없이 clear 하면 상대가 이미 새 거래를 만든 뒤 지우는 사고가 난다(§7-8 위험 6. M3 뮤턴트가 이 재검증을 지워도 `trade.ps1` 로는 재현 불가함을 실측 — 유지 근거는 코드 정독) |
| `SendBuffer::overflow_streak` 증가(넘침 갈래 · `sb.mutex` 안) | 리셋(성공 큐잉 갈래 · 같은 락) | "연속" 계약 — 리셋 없이 두면 순간 폭주 한 번이 영구히 누적돼 언젠가 킥으로 이어진다 | ⚠️ 리셋 제거는 하네스가 못 잡는다(뮤턴트 생존 실측 — `TESTING.md` 「하네스가 구조적으로 못 덮는 것」의 `overflow_streak` 리셋 행 참조) |
| ⭐ `DbPool::try_acquire()`의 `Lease` 반환 | `Lease` 소멸자(RAII — `db_pool.h:280` `Lease::~Lease` — 커넥션을 풀로 반환) | 호출부가 `Lease` 를 락 스코프 밖으로 들고 나가 수명을 늘리지 않는가 | 없다 — 빈 `Lease`(실패)는 `bool` 변환이 `false` 라 소멸자가 아무 것도 안 한다. RAII 계약이 실패 갈래에도 그대로 적용된다 |
| `BufferPool::acquire()` | `release()` | 프레임 잡의 마지막에서 net 이 해제 | 없다 |
| ⭐ `core::LockRankGuard(kRoster)`/`(kSession)` 생성(락 획득 직전) | 가드 소멸(스코프 이탈 — 락 해제와 함께) | `scoped_lock(a.game_mutex, b.game_mutex)` 로 두 세션을 함께 잠글 때 **가드는 1개**(kSession)뿐인가 — 2개를 씌우면 두 번째가 「같은 계층 재진입」을 역순으로 오판한다(`lock_rank.h:27-30`) | 없다 — `NDEBUG` 에서는 전부 no-op 이라 이 행 자체가 Debug/ASan 전용이다 |

### ④ DB

| A | B | 대조 방법 | 비대칭이 허용되는 경우 |
|---|---|---|---|
| ⭐ `CALL` (SP 실행) | **결과셋 drain** (`next_result` 루프 → `do_sanitize()`) | 커넥션을 **반납하기 전에** 비웠는가 | 없다 — 결함 주입이 실측으로 증명 |
| `prepare_one()` | `close_stmt()` | 연결마다 전부 준비, 닫을 때 전부 해제 | 없다 |

### ⑤ S2S 커넥터 내부

커넥터 상태는 S2S 스레드 단독 소유다 — 그래서 짝이 깨지면 그 스레드 안에서만 조용히 깨진다.

| A | B | 대조 방법 | 비대칭이 허용되는 경우 |
|---|---|---|---|
| ⭐ `S2sConnector::request()` | **완료 콜백 정확히 1회** — 갈래 넷(정상 응답 · 타임아웃 · 끊김 · stop) | 갈래마다 센다. kReply 는 **erase 후 콜백**(move-out) 순서까지 본다 — 순서가 뒤집히면 재진입 시 이중 완료 | ⚠️ **반환 `false`(stop 이후)면 콜백이 없다** — 호출자가 반환값을 본다(`JobQueue::push` 동형). 이 다섯 번째 경로를 빠뜨리고 세지 않는다 |
| `S2sConnector` 매칭 테이블 insert | erase (kOk · 타임아웃 스윕 · `fail_all_pending` 스왑) | 삽입 1곳 · 제거 경로 셋이 상호 배타인가 | 없다 |
| `S2sConnector` 소켓 생성(`WSASocketW`) | `closesocket` (begin_connect 실패 4갈래 · connect 완료 실패 · teardown — **shutdown 은 별개 지점이 아니라 teardown 을 재사용한다**) | 매번 `INVALID_SOCKET` 재설정과 짝인가 — 이중 close 봉쇄 | 없다 |
| `S2sConnector` `outstanding_io_` `++` (발행 3종) | `--` (동기 실패 즉시 롤백 · `handle_completion` 진입 — **세대 검사보다 먼저**) | 발행/완료 증감이 전 갈래에서 짝인가. **0 일 때만 재연결**이 IoCtx 재사용 안전의 근거다 | 없다 |

### ⑥ 세션 서버 테이블 · 운영 상태

⭐ **이 그룹의 짝은 종료 요약의 항등식으로 관측된다** — `[SESS ] pending` · `reserve` · `connections` · `kick` 네 줄이 그 계약서다.

| A | B | 대조 방법 | 비대칭이 허용되는 경우 |
|---|---|---|---|
| 세션 서버 pending 삽입(Reserve 발신 1곳) | 제거 **4경로**(응답 매칭 · 타임아웃 · 링크 다운 · stop 정리) | 삽입 1 · 제거 4 가 상호 배타인가. `[SESS ] pending` 의 항등식(`sent == ack+rejected+unsupported+timeout+link_down+stop`)이 그 계약이다 | 없다 |
| 세션 서버 예약 발급(`assign`) | 소멸 **4경로**(덮어쓰기 · 만료 sweep · **회수**(ReserveAck 거절) · 잔여) | `[SESS ] reserve` 항등식 `issued == overwrite+expired+revoked+remain` | 없다 — 회수는 **발급 세대(issue_id)** 로 대조해야 같은 서버 재로그인에서 산 예약을 안 지운다 |
| ⭐ 세션 접속 테이블 삽입 **2곳** — `player_entered()` · **`full_sync_replace()` 의 적용 루프** | 제거 **4경로** — `player_left()` · `full_sync_replace()` 의 first_chunk clear · `drop_server_connections()`(**unregister** · **orphan sweep** 두 호출자) | `[SESS ] connections` 의 `added / removed / fullsync_replaced / remain` 이 서로 맞는가 | 없다 — ⛔ **삽입을 1곳으로 세면 안 된다.** 코드 리뷰가 이 오기를 잡았다 |
| ⭐ 세션 서버 `PendingKick` 삽입 **2곳**(login 중복 감지 · FullSync 재연결 대조 — 억제 스캔은 `has_pending_kick` 헬퍼 공용) | 제거 **4경로**(KickAck 매칭 · `on_sweep` 타임아웃 · 링크 다운 · stop) | `[SESS ] kick` 항등식 `sent == acked+not_found+timeout+link_down+stop` 이 계약이다(하네스 K11·R5 가 파싱 단언). ⛔ **`server_id` 필드는 두 곳 다 「Kick 을 받는 마을」이다** — FullSync 경로에서 소유자(타서버) id 를 넣으면 NotFound 정리가 타서버 정상 항목을 오삭제한다(R3 이 게이트) | 없다 — 억제 갈래는 애초에 삽입이 없으므로 제거 어디에도 안 걸린다 |
| ⭐ 세션 서버 Kick 발신 | KickAck 처리 **2갈래**(Kicked / NotFound — 둘 다 성공, §8-2) | NotFound 만 유령 정리(`player_left` — 소유자 대조 내장) 부가 동작 | 없다 — 미매칭 seq 는 무시+로그(§8-4) |
| ⭐ `SessionRouter::request_set_mode(server_id, true)`(드레인 진입) | `request_set_mode(server_id, false)`(드레인 해제) | **같은 함수의 `bool draining` 분기다** — 세션 서버가 `SetMode` S2S 요청을 fire-and-log 로 보낸다(응답 대기 안 함). Running 복귀 시 **`s2s_link.cpp` 의 `drain_complete_sent_` 를 리셋하는 것이 계약** — 안 하면 다음 재-drain 에서 `DrainComplete` 가 다시 안 나간다(§ 아래 행 참조) | 없다 — 리셋을 빠뜨리면 D8 류 재-drain 시나리오가 조용히 깨진다 |
| ⭐ `s2s_link.cpp` 의 `DrainComplete` 발신(드레인 중 `current()==0` 최초 1회 — 플래그로 중복 억제) | `drain_complete_sent_` 리셋 **2갈래** — Running 복귀(SetMode 분기) · 링크 절단(`on_disconnected` — 재연결한 새 세션 서버는 알림을 받은 적이 없다) | 발신 갈래마다 대응 리셋이 있는가 — **Running 복귀 리셋이 없으면 재-drain 에서, 절단 리셋이 없으면 재연결 후에 두 번째 `DrainComplete` 가 영영 안 나간다**(후자는 drain.ps1 D10 이 단언) | 없다 |
| ⭐ 마을 `PlayerEnter` 발신 | 마을 `PlayerLeave` 발신 | 한 `player_id` 에 대해 Enter 1회 : Leave 최대 1회. ⛔ **둘 다 `started_` 가드 뒤에 있어야 한다** — 링크 비활성일 때 나가면 회귀 8종이 깨진다 | ⚠️ 마을이 죽으면 Leave 가 안 나간다 — 세션 쪽 orphan 이 그것을 흡수한다 |
| ⭐ 마을 Kick 분기(`find_session` → `close_by_id` → 즉시 KickAck) | 기존 `on_session_gone` 존 Job 정리(`entry.leave`+`notify_player_leave`+거래 정리 4/4) | **Kick 분기 자체는 아무 정리도 하지 않는 것**이 계약 — 정리는 전부 기존 경로 | 없다 |

### ⑦ 코덱 · 분류표 — 오프셋은 한 곳에만

⚠️ 하네스는 `proto` 를 include 못 해 **리터럴 사본**을 갖는다. 값이 바뀌면 `find_copies.ps1` 로 훑는다.

| A | B | 대조 방법 | 비대칭이 허용되는 경우 |
|---|---|---|---|
| `is_client_request()` | `is_server_message()` | 새 MsgId 는 **둘 중 하나에** 들어간다 | 미정의 대역은 의도적으로 비움. ⭐ **예외 — 세션 서버 전용 요청**(`kSessionLoginReq`)은 `is_client_request()` 가 아니라 `is_session_client_request()` 에 넣는다(ADR-020 결정 4 — 마을이 그 요청을 「정상」으로 받으면 두 서버의 요청 표가 섞인다). 응답은 그대로 `is_server_message()` 다 |
| `encode_header()` | `decode_header()` | 오프셋이 한 곳에만 | 없다 |
| `proto::s2s::encode_header()` | `proto::s2s::decode_header()` **+ `scripts/s2s.ps1`·`scripts/session.ps1` 의 PS 재현본** | 오프셋 한 곳 + 하네스 쪽 바이트 레이아웃 대조 | 없다 — ⚠️ 하네스는 proto 를 include 못 해 리터럴 사본이다. 값이 바뀌면 `find_copies.ps1` 로 훑는다 |
| ⭐ `proto::s2s::encode_full_sync()` | `decode_full_sync()` **+ `scripts/session.ps1` 의 PS 재현본** | 오프셋 한 곳 + 하네스 쪽 바이트 레이아웃 대조 | 없다 — 하네스는 proto 를 include 못 해 **리터럴 사본**이다 |
| ⭐ `proto::s2s::encode_kick`/`encode_kick_ack` | `decode_kick`/`decode_kick_ack` **+ `scripts/harness_common.ps1` 의 PS 재현본**(`New-KickBody`·`ConvertFrom-KickAckBody`) | 오프셋 한 곳 + 하네스 리터럴 사본 대조 | 없다 |

### ⑧ 빌드 설정

런타임 코드가 아니지만 성격은 같다 — **한쪽만 고치면 조용히 깨지고 하네스가 못 잡는다.**

| A | B | 대조 방법 | 비대칭이 허용되는 경우 |
|---|---|---|---|
| ⭐ `common.props` 의 `Label="Configuration"` 3블록 | 각 `.vcxproj` 의 같은 3블록 | 구성 이름(Debug·Release·ASan)이 양쪽에 다 있는가 | 없다 — 빠지면 **조용히 기본값으로 떨어진다** |
| ⭐ `.vcxproj` 의 `ProjectConfigurations` 3개 | `.sln` 의 `ProjectConfigurationPlatforms` 6줄 | 프로젝트 x 구성 x (ActiveCfg+Build.0) | 없다 — 빠지면 **그 구성이 솔루션 빌드에서 조용히 빠진다** |

### ⑨ ⛔ 무효가 된 행 — 아래 절이 이유를 적는다

지우지 않는 이유는 **검증 위치가 옮겨간 것과 통째로 소멸한 것이 다르기 때문**이다. 그 구분을 아래 두 절이 적는다.

| A | B | 대조 방법 | 비대칭이 허용되는 경우 |
|---|---|---|---|
| ~~`Zone::add()`~~ | ~~`Zone::remove()`~~ | ⛔ **무효 — `Zone` 자체가 삭제됐다**(§ 아래 「Zone 짝 두 행이 무효가 된 이유」 참조). 존 멤버십 홀드는 **명부 등록 홀드**로 이전됐다 | 아래 「명부 등록 홀드」 행이 대신한다 |
| ~~`new_trade()`~~ | ~~`clear_trade_of()`~~ | ⛔ **무효 — `Zone::trades` 맵이 삭제됐다.** 거래는 `Session::trade`(`atomic<shared_ptr<world::Trade>>`)가 직접 진다 | 아래 「trade 생성 1 : 정리 5갈래」 행이 대신한다 |
| ~~`START TRANSACTION`~~ | ~~`COMMIT` / `rollback_quiet()`~~ | ⛔ **무효 — 아래 참조** | — |


#### ⭐ 빌드 설정에도 짝이 있다

위 **⑧ 빌드 설정** 표의 두 행은 **런타임 코드가 아니라 빌드 설정**이다. 성격은 같다 —
**한쪽만 고치면 조용히 깨지고, 하네스가 그것을 못 잡는다.**

⛔ 실측 — `common.props` 에서 `<EnableASAN>` 한 줄을 빼면 ASan 계측이 통째로 사라지는데
**빌드는 `warnings=0 errors=0` 으로 통과한다.** 확인 수단은 `dumpbin` 뿐이다 (ADR-018 참조).

#### ⛔ `START TRANSACTION` 행이 무효가 된 이유

**트랜잭션이 C++ 에서 사라지고 SP 안으로 갔다.** `rollback_quiet()`·`exec_simple()` 은
문장 경로 전용이라 Step 4 에서 **함께 삭제**됐다. 이제 그 짝은 `sql\03_procedures.sql`
안에서 닫힌다(`START TRANSACTION` … `ROLLBACK`/`COMMIT`).

⚠️ **그렇다고 짝이 없어진 게 아니다 — 검증 위치가 옮겨간 것이다.**
`src/` 를 `grep` 해서 「롤백이 없네」라고 판단하면 틀린다. **SQL 파일을 봐라.**

#### ⛔ `Zone::add/remove`·`new_trade/clear_trade_of` 두 행이 무효가 된 이유

**`world::Zone` 자체가 사라졌다.** Step 2 가 `Zone::members`를, Step 3 이 `Zone::trades`(및
`new_trade`/`clear_trade_of`)를, Step 4 가 `world/zone.h` 잔여물 전체를 지웠다 — 존 멤버십은
`EntryTable` 의 「명부 등록 홀드」로, 거래 수명은 `Session::trade`(`atomic<shared_ptr<world::Trade>>`)로
각각 이전됐다(대체 행은 **① 세션 수명 홀드** 의 「명부 등록 홀드」와 **③ 거래 · 버퍼 · 락 가드** 의 `Trade` 행이다).

같은 삭제로 **DB 결과를 `zones.post(zone_id, ...)` 로 되돌려 존 스레드에서 응답하던 3홉 경로**(옛
절대 규칙 4 · 「`zones.post` 회귀·acquire/release 3홉 갈래 전부 삭제」)도
함께 소멸했다. DB 호출이 이제 **워커 자신의 동기 1홉**(`handle_trade_confirm`·`handle_inventory` —
`db_pool.try_acquire()` → `scoped_lock` → 동기 호출 → 락 밖 응답)이라 결과를 다른 큐로 되돌릴 필요
자체가 없다. ⚠️ **이 짝은 `START TRANSACTION` 행과 달리 검증 위치가 옮겨간 것이 아니라 통째로
소멸이다** — 대응하는 새 짝은 없다(1홉 안에서 락과 커넥션 수명이 한 함수 스코프로 닫히기 때문).

#### ⭐ 새 짝 `CALL` ↔ **결과셋 drain** — 결함 주입이 값어치를 실측했다

`CALL` 은 결과셋을 **둘** 낸다(데이터 + 상태). 하나라도 안 비우고 커넥션을 반납하면
**다음에 그 커넥션을 빌린 요청이** `2014 Commands out of sync` 로 죽는다 — 범인이 아닌 쪽이 죽는다.

**방어는 두 겹이다. 둘 다 있어야 한다:**

| 겹 | 무엇 | 없으면 |
|---|---|---|
| 1차 | 호출부의 `do { … } while (status == 0)` **끝까지 도는 루프** | 커넥션이 더러운 채 반납된다 |
| 2차 | 반납 직전 `do_sanitize()` — `last_call_stmt_` 로 **SP 종류와 무관하게** 덮는다 | 1차가 깨졌을 때 아무도 못 잡는다 |

> **실측.** 같은 오염에서 `do_sanitize` **OFF → out-of-sync 7건** · **ON → 0건**.
> 그물을 `stmt_trade_sp_` 단수로 **좁히면 4건** · `last_call_stmt_` 로 **확장하면 0건**.
> ⛔ **2차만 있고 1차가 멀쩡하면 2차의 존재가 관측되지 않는다** — 그래서 *"이거 왜 있지"* 소리가 나온다.
> 지우기 전에 **1차를 깨고 다시 재봐라.**

⛔ **SP 를 새로 추가하면 `last_call_stmt_` 를 세우는 지점도 함께 늘려야 한다.**
안 늘리면 그 SP 만 그물 밖에 남는다 — 실제로 그 사고가 났고 그것을 고친 변경이 이 규칙의 출처다.

### 대조 절차

```bash
# 갈래 수를 센다 — 숫자가 아니라 조건 분기를 본다
grep -rn "acquire_session\|release_session" src/
```

⛔ **「개수가 같다」는 근거가 못 된다.** 한 갈래에서 두 번 해제하고 다른 갈래에서 안 하면 개수는 맞는다.
**분기마다** 확인한다 — `handle_trade_confirm` 이 네 갈래(정상 · 큐 만원 · DB 후 post 실패 · 완료)에서
각각 해제하는 것이 기준 형태다.

---

## 5. 설정값

- 기동 시에만 읽는다. 도는 중에 바뀌는 값을 만들지 않는다.
- `cfg.get_int(section, key, 기본값, lo, hi)` — **범위를 반드시 준다.**
- 범위 밖이면 `[WARN]` 을 남기고 기본값으로 간다. **기동을 죽이지 않는다** —
  배포에서 오타 하나가 장애가 되면 안 된다.
- `0` 을 「기본값 사용」으로 쓰는 관례가 있다.
- `config/server.ini` 에는 **값마다 「왜 이 값인가」**를 적는다.

---

## 6. 로그

| 태그 | 용도 |
|---|---|
| `[INFO]` `[WARN]` `[ERROR]` | 일반 |
| `[RECV]` `[SEND]` `[DB  ]` `[DBACK]` | 프레임 추적 (`trace_frames` 로 끔) |
| `[TICK ]` `[POOL ]` `[CONN ]` `[NET  ]` `[ALLOC]` | 종료 요약 = **회귀 판정 지표** |

- 태그는 **5칸 폭으로 맞춘다** (`[DB  ]` 처럼 공백 채움). 로그를 세로로 훑을 수 있어야 한다.
- 핫패스 로그는 `core::log_trace_frames()` 로 감싼다.
- **로그 한 줄이 곧 증거다.** `[RECV]` 와 `[DB  ]` 의 tid 가 다른 것이 「I/O 워커와 로직이 분리돼 있다」의 증거다.

---

## 7. 컴파일러

- **경고 수준 4 · 경고 0 이 기준선이다.** 경고를 남긴 채 커밋하지 않는다.
- 경고를 끄는 `#pragma warning(disable)` 은 **이유를 주석으로 적고 범위를 최소로** 한다
  (`alignas(64)` 의 4324 가 그 예).
- `static_assert` 로 규약과 구현을 묶는다
  (예: 수신 버퍼가 한 프레임을 담는지 — 못 담으면 그 프레임은 영원히 완성되지 않는다).

---

## 7-b. 파일 규약 — 인코딩과 줄바꿈

`.gitattributes` 가 줄바꿈을 고정하고 있다. 그 짝으로 **인코딩 규약**이 있다.

| 확장자 | 줄바꿈 | 인코딩 | 이유 |
|---|---|---|---|
| `.ps1` | CRLF | **UTF-8 + BOM (필수)** | ⛔ Windows PowerShell 5.1 은 **BOM 없는 UTF-8 을 시스템 ANSI 로 읽는다** |
| `.sln`                           | CRLF | UTF-8 (**BOM 없음** — VS 가 그렇게 만든다) | Windows 도구가 읽는다 |
| `.vcxproj` `.vcxproj.*` `.props` | CRLF | **UTF-8 + BOM**                            | Windows 도구가 읽는다 |
| `.h` `.cpp` | LF | UTF-8 | |
| `.ini` `.sql` `.md` | LF | UTF-8 (BOM 허용 — `config.h` 가 `strip_bom` 한다) | |

### ⛔ `.ps1` 에 BOM 이 없으면 — **파서 에러가 엉뚱한 줄을 가리킨다**

한글이 든 문자열이 ANSI 로 잘못 해석되면서 따옴표 짝이 깨지고,
PowerShell 은 **파일 끝의 `}`** 를 *"예기치 않은 토큰"* 이라고 보고한다.
**실제 원인(인코딩)과 보고된 위치(중괄호)가 완전히 다르다.**

```bash
# BOM 확인 — efbbbf 가 나와야 한다
head -c 3 <파일>.ps1 | od -An -tx1

# BOM 추가
python -c "import pathlib,sys; p=pathlib.Path(sys.argv[1]); b=p.read_bytes(); \
p.write_bytes(b'\xef\xbb\xbf'+b) if not b.startswith(b'\xef\xbb\xbf') else None" <파일>.ps1
```

> **기존 `.ps1` 은 전부 BOM 을 갖고 있다**(`scripts\*.ps1` · 스킬 헬퍼).
> 새로 만들 때만 빠뜨리기 쉽다 — 대부분의 편집 도구가 BOM 없이 저장하기 때문이다.

### ⚠️ PowerShell 5.1 문법 함정

- `if (...) { ... }` **다음 줄**에 `elseif` 를 쓰면 파서 에러다. `} elseif {` 로 같은 줄에 둔다
- `$arr | Sort-Object` 가 **원소 1개면 스칼라를 반환**한다. 거기에 `[-1]` 을 쓰면
  배열의 마지막이 아니라 **문자열의 마지막 문자**가 나온다 → `@($arr | Sort-Object)[-1]`
- `&&` `||` `?:` `??` 없음 · `$args` 는 예약 변수라 쓰지 않는다

---

## 8. 하지 말 것

- ⛔ 요청받지 않은 파일 수정 · 무관한 포매팅 변경
- ⛔ 「나중에 필요할 것 같아서」 만드는 추상화
- ⛔ 커스텀 컨테이너·할당자 (재는 것이 먼저다)
- ⛔ 승인 없는 외부 의존 추가
- ⛔ 부하 주입 스위치를 켠 채 커밋
- ⛔ 문서를 **라인 번호로 인용** — 코드가 한 줄만 밀려도 거짓이 된다. 심볼명으로 인용한다
