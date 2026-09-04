# 확장 개발 가이드 — 작업 유형별 체크리스트

> 먼저 `docs/ARCHITECTURE.md` 의 **7절 불변식**을 읽는다.
> 아래는 「무엇을 어느 순서로 고치는가」만 적는다.

---

## A. 새 메시지 하나 추가하기

가장 흔한 확장. **다섯 곳**을 고친다. 하나라도 빠지면 증상이 엉뚱한 곳에서 난다.

### 1) `src/proto/packet.h`
```cpp
kFooReq = 12,          // 요청은 1번대
kFooAck = 112,         // 응답·통지는 101번대
```
- `is_client_request()` 에 `kFooReq` 추가 — **빠뜨리면** 미정의 ID 로 떨어져 `bad_msg_score` 가 쌓이고 한참 뒤에 멀쩡한 세션이 끊긴다.
  ⭐ **세션 서버 전용 요청이면 `is_session_client_request()`** 다 (ADR-020 결정 4 — 두 서버의 요청 표를 섞지 않는다).
- `is_server_message()` 에 `kFooAck` 추가 — 빠뜨려도 안전하게 실패하지만(미정의로 분류) 맞춰 둔다.
- body 규약을 **주석으로** 옆에 적는다 (`// body: [ x:u32 ][ y:u8 ]`).

### 2) `src/app/frame_router.cpp` — 익명 네임스페이스에 핸들러
```cpp
bool handle_foo(net::IocpServer& server, app::EntryTable& entry,
    net::Session& session, const char* body, int body_len) {
    if (body_len != 5) { return bad_body(session, "foo", body_len, 5); }   // ① 길이 검증
    if (session.player_id == 0) {                                          // ② 로그인 검증(필요시)
        return send_msg(server, session, proto::MsgId::kFooAck, err, n);
    }
    // ③ 존이 필요하면 session.zone 을 직접 읽는다 — 이 호출 자체가 그 세션의
    //    직렬 큐 실행권 안에서 돌므로 그 필드를 읽어도 안전하다(ARCHITECTURE.md §2).
    std::vector<net::Session*> members;
    entry.snapshot_zone(session.zone, members);                           // ④ 소속 확인은 명부(L1) 스냅샷으로
    ...
    return send_msg(server, session, proto::MsgId::kFooAck, out, len);
}
```
반환값 의미: `true` = 세션 유지 · `false` = **이 세션을 끊어라**. 프로토콜 위반에만 `false`.
`snapshot_zone`/`snapshot_all` 은 원소마다 `acquire`를 걸어 돌려준다 — 반환된 개수만큼 정확히 `release_session`해야 한다(ARCHITECTURE.md §4).

### 3) `on_frame` 의 switch 에 case 추가
- 로그인·존과 무관한 것(Echo/Ping/Enter 류)이면 **앞쪽 switch**(로그인 게이트 앞).
- 로그인이 필요한 것은 **뒤쪽 switch**(`session.player_id != 0` 게이트 뒤).

### 4) 클라이언트 쪽 하네스 — `scripts\` 에 검증 스크립트
기존 것(`send.ps1`, `trade.ps1`)의 프레임 조립부를 복사해서 쓴다.

### 5) 문서
`README.md` 의 프로토콜 표 · `docs/ARCHITECTURE.md` §6 의 요청 표 — 이 파일 아님.
⚠️ **정정 — 도면(`docs\*.pdf`)은 이 저장소에 없다**(추적 0건). 옛 문구가 *"소스 발췌를 담고 있으니 같이 본다"* 고 했으나 볼 대상이 없다. 경위는 `README.md` 「문서」 절 끝에 있다.

---

## B. DB 를 타는 기능 추가하기

DB 를 부르는 스레드는 이제 **직렬 큐 워커 자신**이다 — 그 실행권 안에서 **동기 호출이 정상 경로**다(옛 "존 스레드에서 동기 DB 질의 금지"는 존 스레드 자체가 없어져 성립하지 않는다). 다만 지켜야 할 순서는 그대로 남는다 — **커넥션은 락보다 먼저 빌린다.**

```cpp
bool handle_foo(net::IocpServer& server, db::DbPool& db_pool, net::Session& session, ...) {
    db::DbPool::Lease lease = db_pool.try_acquire();      // ① 락을 잡기 전에 먼저 빌린다
    if (!lease) {
        return send_msg(server, session, proto::MsgId::kFooAck, kBusyBody, n);  // ② 못 빌리면 즉시 kBusy — 재큐잉하지 않는다
    }

    db::DbConn* conn = lease.get();
    ... 질의 ...(재시도가 필요하면 handle_trade_confirm 의 kTradeRetry 패턴 참고)

    // ③ 세션 상태를 만져야 하면 그 다음에 game_mutex(L2) 를 잡는다 — 순서는 항상 L1(필요 시) → L2.
    //    커넥션을 든 채로도 game_mutex 를 잡는 것은 문제 없다(§7-4 가 막는 것은 "락을 먼저 잡고
    //    커넥션을 나중에 구하는" 순서다).
    return send_msg(server, session, proto::MsgId::kFooAck, out, len);
    // lease 가 스코프를 벗어나며 자동 반납된다 — 반납 지점은 이 하나뿐이다.
}
```

- `try_acquire()`는 유휴 연결을 반환하거나 상한 안이면 새로 만들어 주고, 그마저 안 되면 **기다리지 않고** 그 자리에서 빈 `Lease`를 돌려준다. 실패한 요청을 재큐잉하지 않는다 — 같은 요청이 실패-재시도를 반복하는 기아를 만들기 때문이다.
- 새 질의문은 `db_conn.h` 에 **prepared statement 로** 추가한다: `stmt_xxx_` 멤버 + `prepare_all()` 에 `prepare_one()` 한 줄 + 실행 함수. 문자열 연결로 SQL 을 만들지 않는다.
- 다중 행 응답은 `(kMaxBodySize - 헤더분) / 행크기` 로 상한을 계산해 `resize` 한다.
- 트랜잭션이 필요하면 `DbConn::trade()` 의 구조를 그대로 따른다 — 락 순서 정렬 → BEGIN → ops → 로그 INSERT → COMMIT, 재시도는 `is_retryable()` 로 판정. 재시도 판단은 트랜잭션 **바깥**에서 한다(SP 안에서 하면 롤백된 트랜잭션을 다시 못 연다).

---

## C. 틱 기반 기능 추가하기

`app::TickThread::run()`(`worker_pool.cpp`)이 비어 있는 자리를 30Hz 로 이미 돌리고 있다 — 옛 `world::Zone::on_tick`이 있던 자리다. 이제 존이라는 객체 자체가 없다(마을은 세션의 `zone` 필드와 `EntryTable`의 존 인덱스만 안다). 틱에서 상태를 만지려면 이 둘 중 하나를 거쳐야 한다:

- **세션별 상태**라면 그 세션의 `game_mutex`(L2)를 잡는다 — 틱 스레드가 직렬 큐 실행권 밖에서 세션을 직접 만지는 유일한 합법적 경로이므로, 잡는 순서는 항상 L1(필요하면 먼저) → L2 다.
- **존 단위로 순회**해야 한다면 `EntryTable::snapshot_zone`/`snapshot_all`로 스냅샷을 떠서(원소마다 `acquire`된 채로 온다 — 반드시 그만큼 `release_session`) 락 밖에서 순회한다. 틱 스레드가 `EntryTable::mutex_`(L1)를 직접 오래 들고 있으면 그동안 직렬 큐 워커의 Enter/Leave/JoinZone 이 전부 막힌다.
- 틱은 세션·DB·`EntryTable` 중 **어느 것도 원래 안 만진다**(§1 표) — 이 자리에 새 접근을 넣는 순간 "틱 지연"과 "락 경합"이 같은 스레드에서 섞여 원인 분석 축이 무너진다는 것이 전용 스레드를 둔 이유(D5) 자체를 갉아먹는다는 점을 감안한다.
- 틱에서 송신해도 된다(`send_msg`는 스레드 안전). 다만 브로드캐스트는 멤버 수만큼 늘어난다.
- 틱 품질은 종료 로그 `[TICK ]` `[TICK2]`(interval avg/p99/max, jitter, behind%)로 판정한다. 틱 스레드가 이제 하나뿐이라 이 통계도 스레드마다가 아니라 값 하나다. 기능 추가 전후 값을 비교한다.
- 존 자체를 지우는 개념은 없다 — 남는 것은 `EntryTable::zone_index_`의 빈 버킷뿐이고, 이건 존 이동(`move_zone`)이 옛 버킷을 항상 지우므로 자연히 비게 된다(별도 GC 가 필요 없다).

---

## D. 새 소스 파일 추가하기

1. `src/<layer>/` 에 만든다. **의존 방향**: `world → proto → net → core`, `db → core`, `ops → core`, `app` 만 전부를 안다. 역방향 include 금지.
2. 그 파일이 속한 프로젝트(`src/{core,net,db,ops,bench,proto}` → `server\common.vcxproj` · `src/{app,world}`와 `src/main.cpp` → `server\village.vcxproj`)의 `<ItemGroup>` 에 `<ClCompile Include="..\src\...\x.cpp" />` / `<ClInclude Include="..\src\...\x.h" />` 추가 (와일드카드 없음).
3. 같은 프로젝트의 `.filters` 에 같은 항목 + `<Filter>` 지정.
4. `scripts\build.ps1` 로 세 구성 빌드 — **경고 0** 이 기준선이다 (`/W4`).

---

## E. 설정값 추가하기

1. `config\server.ini` 에 키 추가 + **왜 이 값인가**를 주석으로 (기존 스타일 유지).
2. `app::ServerSettings` 에 필드 추가.
3. `bootstrap.cpp` 의 `load_settings()` 에서 `cfg.get_int(section, key, 기본값, lo, hi)` 로 읽는다 — **범위를 반드시 준다.** 범위 밖이면 `[WARN]` 남기고 기본값으로 간다(기동 실패 아님).
4. 0 을 「기본값 사용」으로 쓰는 관례가 있다 (`[io] worker_threads`, `[app] workers` 등 — 옛 `[world] zone_threads`는 폐지됐다, `bootstrap.cpp`가 그 키를 만나면 경고를 남긴다).

---

## F. 작업 전후 회귀 절차 (고정)

```powershell
# 0. 부하 주입 스위치 전부 꺼짐 확인
#    frame_router.cpp : kLogicDelayMs · kBadSyncDbMs · kBadTradeNoTx
#    worker_pool.cpp  : kBadTickWorkMs · kBadTickSpikeEvery · kBadTickSpikeMs
#    (idle_timeout_sec 는 이 목록이 아니다 — 커밋 의도값이 90 이다(ADR-023).
#     자체 스폰 하네스는 스크래치에서 0 으로 오버라이드한다 — TESTING.md §0)

.\scripts\build.ps1                                          # 경고 0

.\scripts\send.ps1       -Repeat 3000 -Size 8 -Framed -Seq   # 프레임 경계·순서(직렬 큐 순서 회귀도 겸한다)
.\scripts\zone.ps1       -Clients 8 -Zones 4 -Chats 100      # 존 경계 브로드캐스트
.\scripts\inventory.ps1  -Mixed                              # DB 왕복
.\scripts\zone_block.ps1 -Floods 20                          # 워커-풀 격리(DB 폭탄이 풀을 잠가도 에코는 안 밀림)
.\scripts\trade.ps1      -All                                # 거래 25항목·총량 보존
.\scripts\zone_race.ps1  -Pairs 3000                         # 락 계층 역순 검사기(Debug/ASan) 무발화
.\scripts\churn.ps1      -Count 1000 -Framed                 # 접속 반복·누수
```

**판정 지표(종료 로그)**

| 줄 | 봐야 할 것 |
|---|---|
| `[ALLOC]` | 할당 **규모** (기준선 대비 추세). ⛔ 누수 판정은 `churn.ps1` 이 한다 |
| `[POOL ]` / `[POOL2]` | failed = 0 (프레임 풀 고갈) / `try_failed` (커넥션 즉시 실패 횟수) |
| `[CONN ]` | rejected 와 peak 의 관계 (세션이 안 지워지는가) |
| `[NET  ]` | idle_kicked |
| `[TICK ]` `[TICK2]` | behind% · jitter (전용 틱 스레드 1개가 밀리는가) |

같은 시나리오를 **ASan 구성으로 한 번 더** 돌린다. 세션 수명(`acquire`/`release`)을 건드린 변경은 ASan 없이는 검증됐다고 할 수 없다.

---

## G. 흔한 실수 — 증상과 원인

| 증상 | 원인 |
|---|---|
| 16분쯤 뒤 멀쩡한 세션이 끊긴다 | 새 요청 ID 를 `is_client_request()` 에 안 넣었다 |
| Debug/ASan 에서만 assert 사망 | 락 순서를 역순(L2 를 든 채 L1 재진입 등)으로 잡았다 — `core::LockRankGuard` 가 잡는다 |
| 두 세션 잠그는 코드에서 assert 사망 | `scoped_lock(a,b)` 를 안 쓰고 `LockRankGuard` 를 두 번 씌웠다 — 첫 가드가 이미 계층을 올려놔 두 번째가 역순으로 오판한다 |
| 접속 반복 후 메모리가 는다 | 실패 경로 하나에서 `release_session` 이 빠졌다(홀드 4계열 중 하나 — ARCHITECTURE.md §4) |
| ASan 에서 use-after-free | `release` 를 두 번 했다 / 홀드 없이 포인터를 태웠다 |
| 같은 세션의 요청 두 개가 순서가 뒤바뀐 것처럼 처리된다 | 직렬 큐를 우회해 그 세션을 직접 다른 큐로 태웠다 — `player_id`·`zone` 은 직렬 큐 실행권 밖에서 안전하지 않다 |
| DB 가 느려지면 무관한 요청까지 밀린다 | 커넥션을 빌리기 전에 락을 먼저 잡았다(§7-4 순서 위반) / `Lease` 를 락 스코프 밖으로 들고 나가 반납이 늦어졌다(`CODING_RULES.md` §4-b) |
| 회귀가 이유 없이 전부 실패 | 부하 주입 스위치가 켜져 있다. ⚠️ `idle_timeout_sec` 는 커밋값이 이제 `90` 이라(ADR-023) 이 자체는 원인이 아니다 — 자체 스폰 하네스가 스크래치에서 `0` 으로 오버라이드하는지가 깨졌을 때만 의심한다 |
| 거래 후 아이템 총량이 안 맞는다 | 트랜잭션 밖에서 take/give 를 나눴다 |
| 응답이 잘려서 온다 | 본문이 4096B 를 넘었다 (상한 계산 누락) |
