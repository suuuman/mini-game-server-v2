# mini-game-server-v2

Windows · C++20 · IOCP 기반 게임 서버를 **세션 서버 + 마을 서버**로 나눈 프로젝트.
`mini-game-server-portfolio` 에서 파생했다.

산출물은 셋이다 — `common.lib`(공용 계층) · `village.exe`(마을 서버) · `session.exe`(세션 서버).
분리 로드맵 0~6단계가 끝나 있고, 이 문서 아래의 서술은 **지금 소스 그대로**다.

| | |
|---|---|
| 설계 | **`docs/DESIGN-server-split.md`** — 18개 절. **왜 이 구조인가**는 여기 있다 |
| 구조 | `docs/ARCHITECTURE.md` — 계층 · 불변식 10개 |
| 결정 기록 | `docs/DECISIONS.md` — ADR. 되돌리기 전에 여기서 이유를 확인한다 |
| 검증 절차 | `docs/TESTING.md` — 회귀 하네스 14종 · 부하 주입 스위치 · 판정 지표 |
| 코드 규약 | `docs/CODING_RULES.md` — 주석 문화 · 명명 · 자원 수명 · 짝 API |

---

## 왜 나누는가

`Zone` 이라는 개념 하나가 **스레드 배정 키 · 거래 소유자 · 채팅 대상** 세 역할을 겸했다.

| 역할 | 판정 |
|---|---|
| 스레드 배정 키 (`zone_id % N`) | ⛔ 게임 개념이 라우팅 키가 되면 안 된다 |
| 거래 소유자 | ⛔ 같은 존끼리만 거래된다 |
| 채팅 대상 | ✅ 문제 아님 — 지역 채팅의 대상은 원래 공간이다 |

목표 구조 —

```
클라 ──> 세션 서버      로그인 · 서버 목록 · 배정 · 중복 차단 · 운영 명령
클라 ──> 마을 서버      이동 · 거래 · 강화 · 인벤토리 · 채팅 전부

마을 ──> 세션           등록 · 헬스체크 · 접속 보고
```

존은 **마을 서버가 소유한 위치 라벨 한 필드**로 강등된다.

진행 — 0단계(공통 라이브러리 분리 · `common.lib`+`village.exe`) · 1단계(S2S 커넥터 —
`net::S2sConnector`·`proto::s2s` 8B 프레이밍·`app::S2sLink`) · 2단계(`session.exe` — 레지스트리 ·
헬스체크 · 배정 · 예약) · 3단계(마을의 등록·보고·예약 확인 — `app::EntryTable`·`kEnterReq`) ·
4단계(**예약 강제** — `kLoginReq` 폐지 · **드레인+운영 명령+idle** — 세션 stdin 콘솔
`drain <id>`/`undrain <id>` · S2S `SetMode`/`DrainComplete` · `idle_timeout_sec=90` · **동기 Kick**) ·
5단계(**마을 내부 재구성** — 락 2계층 `L1 명부/L2 세션`·세션 직렬 큐 워커·DB 동기 1홉·틱 스레드 —
와이어 무변) · 6단계(**채팅 3종** — 전체·지역·귓속말 · 송신 큐 넘침 킥 · **S2S 링크 재연결**)
까지 끝나 있다. 세션 서버는 클라 포트와 S2S 수용 포트를
따로 열고(`config\session.ini`), 클라는 `kSessionLoginReq`(u64) → `kSessionLoginAck{result·port·host}`
로 마을 주소를 받는다.
`config\server.ini` 의 `[s2s] host` 가 빈값이면 마을의 커넥터는 통째로 비활성이라,
마을 서버 단독으로도 그대로 돌아간다. 세부는 `docs/DECISIONS.md` ADR-018·019·020.

---

## 흐름

한 프레임이 지나가는 길이 곧 이 서버의 구조입니다.

```
             ┌──────────┐
   TCP ─────>│ accept ×1│──> Session 생성 · IOCP 에 붙임
             └──────────┘
                  │
             ┌────▼──────────┐   완료 수거 · 프레임 경계 복원 · 송신 이중 버퍼
             │ I/O 워커 ×4   │   ★ 여기서 게임 로직을 돌리지 않는다 — 세션 직렬 큐에 밀어 넣기만
             └────┬──────────┘
                  │  세션마다 직렬 큐(serial_queue)에 push
                  │  ← sq_scheduled=false→true 로 전이할 때만 실행권을 공용 큐에 올린다
             ┌────▼──────────┐   존 라벨(session.zone) · 거래(atomic<shared_ptr<Trade>>) · DB 동기 호출
             │ 직렬 큐 워커   │   ★ 세션당 실행권 1개 — 「프레임 순서 = 직렬 큐 순서」가 이렇게 성립한다
             │  ×N (기본 8)  │   락은 L1(명부)→L2(세션) 순서만 지키면 자유
             └───────────────┘
             ┌───────────────┐   고정 30Hz — 지금은 빈 on_tick 호출 + 통계뿐
             │ 틱 스레드 ×1  │   세션·DB 를 안 만져 직렬 큐 워커와 간섭이 없다
             └───────────────┘
```

---

## 계층

```
src/
  main.cpp                                 순서가 곧 계약인 것만 — 선언 · 기동 · 종료
  app/    frame_router · bootstrap · worker_pool · entry_table   프로토콜 · 배선 · 직렬 큐 스케줄러 · 명부(L1)
  core/   log · config · job_queue · buffer_pool · alloc_counter · mpsc_queue · lock_rank
  net/    session · iocp_server            소켓 · IOCP · 수신 누적 · 송신 이중 버퍼 · 세션 직렬 큐(session.h)
  proto/  packet                           프레임 규약 · 직렬화 · MsgId
  world/  trade                            거래창 — 세션이 atomic<shared_ptr> 로 직접 소유
  db/     db_conn · db_pool · db_worker    커넥션 풀(try_acquire) · DbPool 소유(전용 워커 스레드 없음)
  ops/    crash_dump                       크래시 덤프
  bench/  bench                            내장 측정 하네스
```

**의존 방향은 한쪽입니다** — `world → proto → net → core` · `db → core` · `ops → core`.
역방향 include 는 금지이고, `#include` 그래프를 훑어 위반을 잡는 검사기를 따로 두고 돌렸습니다.
`app/` 만 전부를 압니다.

설계의 축은 셋이고 전부 **「한 X 의 일은 한 곳에서만」** 입니다.

| 축 | 규칙 | 그래서 |
|---|---|---|
| 세션 직렬 큐 | 세션당 실행권 **1개**(`serial_queue`) | 프레임 순서가 직렬 큐 순서로 보장되고, `player_id`·`zone` 은 락 없이 안전 |
| 명부(L1) | `EntryTable::mutex_` 하나가 입장 집합·존 인덱스·예약을 같이 지킨다 | 어긋나면 안 되는 한 쌍이 항상 같은 락 아래서 갱신된다 |
| 락 순서 | `L1(명부) → L2(세션 game_mutex)`, 역순 금지 | Debug/ASan 은 `core::LockRankGuard` 로 역순 획득을 assert 로 잡는다 |

---

## 프로토콜

길이 선행 프레이밍 · 빅엔디언. 본문 상한 4,096B.

```
[ body_size : u16 ][ msg_id : u16 ][ body ... ]
```

| 방향 | 메시지 |
|---|---|
| 클라 → 마을 | `Echo` · `Ping` · **`Enter`**(`player_id:u64`) · `JoinZone` · **`Chat`**(`Zone`/`All`/`Whisper` — `type:u8`) · `Inventory` · `Trade{Req,Answer,SetItem,Confirm,Cancel}` |
| 마을 → 클라 | `*Ack`(`Pong` · **`EnterAck`** 포함) · `ChatNtf` · **`ChatAck`**(실패 시에만) · **`ZoneMembersNtf`** · `Trade{Req,Open,State,Cancel}Ntf` |
| 클라 → **세션 서버** | **`SessionLogin`**(`player_id:u64`) · `Ping` |
| **세션 서버** → 클라 | **`SessionLoginAck`**(`result:u8 · port:u16 · host:str16`) · `PongAck` |

`Enter` 는 세션 서버가 발급한 **예약을 확인하는 유일한 지점**이고 로그인을 겸합니다
(`EnterAck` body = `result:u8 · player_id:u64 · session_id:u32`). **`Login` 직행 경로는 폐지했습니다** —
신원을 얻는 길이 `Enter` 하나뿐이라, 예약 없이 붙은 소켓은 게임 요청에 닿지 못합니다.
게이트는 `on_frame` 의 배정 스위치 진입부 한 곳에 있고, 조건은 `session.player_id != 0` 입니다.

S2S(마을 ↔ 세션)는 헤더에 `seq:u32` 가 붙은 별도 8B 프레이밍이고, 코덱이 구현된 메시지는 17종입니다:
`Register`·`Heartbeat`·`Unregister`(+각 `Ack`) · `Reserve`/`ReserveAck` ·
`PlayerEnter`·`PlayerLeave`·`FullSync`(알림 — `seq=0`, 응답 없음) · `Unsupported` ·
**`SetMode`/`SetModeAck`**(운영 명령 — 드레인 진입/해제) ·
**`DrainComplete`**(알림 — 드레인 중 인원 0 을 세션 서버에 보고) ·
**`Kick`/`KickAck`**(중복 로그인 시 세션 서버가 먼저 붙은 세션의 절단을 지시 — 「이미 없음」도
성공이고 그 응답은 접속 테이블의 유령 정리 신호를 겸합니다).

---

## 빌드 · 실행

`mini-game-server.sln` 을 Visual Studio 에서 열거나:

```powershell
.\scripts\build.ps1          # Debug · Release · ASan 세 구성을 한 번에
```

| | |
|---|---|
| 툴셋 | v145 (Visual Studio 2026) · C++20 · 경고 수준 4 |
| 의존 | MySQL 8.4 (`libmysql.lib` · `libmysql.dll`) |
| 출력 | `build\x64\{Debug,Release,ASan}\village.exe` · `session.exe` |

MySQL 경로는 `.vcxproj` 의 `MySqlDir` 하나로 관리합니다.

```powershell
mysql -u root -p < sql\00_account.sql     # 계정 생성 (최소 권한 · DDL 없음)
mysql -u root -p < sql\01_schema.sql      # 게임 스키마 + 시드 데이터
mysql -u root -p < sql\02_log_schema.sql  # 감사 로그 스키마 (거래 기록)
```

⚠️ `00_account.sql` 의 비밀번호를 바꾼 뒤 `config\server.ini` 의 `[db] password` 를 맞춰 주세요.

⚠️ **세 번째를 건너뛰면 거래만 실패합니다.** 로그인·채팅·조회는 정상이고,
기동 로그에 `[ERROR] 감사 로그 문장 준비 실패` 가 남습니다 — 거래 기록을 못 남기면
거래도 없던 일로 하는 것이 이 서버의 선택입니다. 로그는 거래 트랜잭션 **안에서** 남기므로
「아이템은 옮겨졌는데 기록이 없는」 상태가 생기지 않습니다.

설정은 `config\server.ini` 하나이고, 값마다 「왜 이 값인가」가 주석으로 붙어 있습니다.

```powershell
.\build\x64\Release\village.exe --seconds 60      # ★ 반드시 프로젝트 루트에서
```

DB 가 죽어 있어도 서버는 뜹니다 — 인벤토리·거래만 실패하고 나머지는 정상 동작합니다.
종료 시 로그에 `[TICK ]` · `[POOL ]` · `[CONN ]` · `[NET  ]` · `[ALLOC]` 요약이 남습니다.

---

## 동작 확인

`scripts\` 하네스 **14종**은 각각 **하나의 성질**을 검증합니다.
⛔ **묶음이 두 갈래고, 둘 다 포트 9000 을 쓰므로 순서가 강제됩니다.**

**① 서버를 미리 띄우고** 다른 창에서 두 종:

```powershell
.\build\x64\Release\village.exe --seconds 60                 # 먼저 이것
.\scripts\send.ps1  -Repeat 3000 -Size 8 -Framed -Seq        # 프레임 경계 복원 · 순서
.\scripts\churn.ps1 -Count 1000 -Framed                      # 접속·종료 반복 · 누수
```

**② 서버를 내린 뒤** 나머지 열두 종 — 각자 서버를 스스로 스폰합니다:

```powershell
.\scripts\zone.ps1       -Clients 8 -Zones 4 -Chats 100      # 존 경계 브로드캐스트
.\scripts\members.ps1                                        # 존 멤버 목록 통지 — 4개 변동 지점
.\scripts\inventory.ps1  -Mixed                              # DB 왕복
.\scripts\zone_block.ps1                                     # 워커-풀 격리 — DB 폭탄이 풀을 잠가도 에코는 안 밀림
.\scripts\trade.ps1      -All -DbUser sp_owner -DbPass '<비번>'   # 거래 25항목 · 아이템 총량 보존
.\scripts\zone_race.ps1  -Pairs 3000                         # 락 계층 역순 검사기 무발화 (기본 -Config Debug)
.\scripts\gate.ps1                                           # 예약 강제 게이트 11항목
.\scripts\drain.ps1                                          # 드레인 시나리오 19판정
.\scripts\idle.ps1                                           # 유휴 절단 9판정
.\scripts\chat.ps1                                           # 채팅 3종 48판정
```

**③ 아래 둘은 단독 실행입니다** — 다른 종과 창을 공유하지 않습니다:

```powershell
.\scripts\s2s.ps1       # S2S 커넥터 29항목 (28 통과 + SKIP 1) — 가짜 세션 서버 역할을 스스로 한다
.\scripts\session.ps1   # 세션 서버 통합 95항목 — session.exe·village.exe 를 둘 다 스폰한다
```

⚠️ `zone_race.ps1` 은 **`assert` 가 살아 있는 구성에서만 뜻이 있습니다** — `-Config Release` 를
명시하면 검사기가 no-op 이라 아무것도 잡지 않습니다.
⚠️ `s2s.ps1` 의 ASan 회차는 `run-asan.ps1` 이 아니라 `.\scripts\s2s.ps1 -Config ASan` 으로 돌립니다
(전자는 커밋본 config 로 서버를 띄워 S2S 가 비활성이라, 시나리오가 하나도 안 돌면서 통과처럼 보입니다).

**회귀를 돌리는 순서**

1. `build.ps1` 로 Debug · Release · ASan 세 구성을 빌드합니다 (**경고 0** 이 기준선입니다).
2. 소스의 부하 주입 스위치가 전부 꺼져 있는지 확인합니다 —
   `frame_router.cpp` 의 `kLogicDelayMs` · `kBadSyncDbMs` · `kBadTradeNoTx`,
   `worker_pool.cpp` 의 `kBadTickWorkMs` · `kBadTickSpikeEvery` · `kBadTickSpikeMs`.
   켠 채로 돌리면 **회귀가 거짓말을 합니다** (실제로 겪은 사고입니다).
   `config\server.ini` 의 `idle_timeout_sec` 는 **90 이 커밋 의도값**입니다(ADR-023) —
   자체 스폰 하네스는 스크래치 config 에서 0 으로 고정하고, 사전 기동 갈래(`send`·`churn`)는
   90 을 그대로 읽어도 충분히 짧아 안전합니다(최장 유휴 0.8s — 실측).
3. 위 순서대로 하네스를 돌립니다. 가벼운 것(프레임 경계)에서 무거운 것(거래 · 접속 반복)으로 갑니다.
4. 종료 로그의 `[TICK ]` · `[POOL ]` · `[CONN ]` · `[NET  ]` · `[ALLOC]` 요약을 봅니다 —
   틱 지연 · 풀 대기 · 누수 여부가 여기서 드러납니다.
5. 같은 시나리오를 **ASan 구성으로 한 번 더** 돌립니다 (UAF · 오버런).
   크래시가 나면 `dumps\` 에 미니덤프가 떨어집니다.

---

## 문서

`docs/` 안에 있습니다. **처음 읽는 순서는 이렇습니다.**

| | |
|---|---|
| `docs/DESIGN-server-split.md` | **여기서 시작합니다** — 왜 나누는가 · 18개 절. 설계가 여러 번 갈아엎힌 이유가 여기 있습니다 |
| `docs/ARCHITECTURE.md` | 계층 · 경계 · **불변식 10개**. 코드를 고치기 전에 봅니다 |
| `docs/DECISIONS.md` | ADR — 각 결정의 대안과 버린 이유. **되돌리고 싶을 때 먼저 봅니다** |
| `docs/CODING_RULES.md` | 주석 문화 · 명명 · 오류 처리 · 자원 수명 · 짝 API 표 |
| `docs/TESTING.md` | 하네스 14종의 판정 지표 · 부하 주입 스위치 · 실행 순서 |
| `docs/EXTENSION-GUIDE.md` | 기능을 더할 때의 유형별 체크리스트 |
| `docs/DNF-GAP-ANALYSIS.md` | 상용 MMO 와의 격차 분석 · 로드맵 |

⚠️ **도면(`docs\*.pdf`)은 이 저장소에 없습니다.** 파생 원본인 `mini-game-server-portfolio` 에는
네 장이 있는데, 그 도면들은 분리 **이전** 구조(존 스레드 · 전용 DB 워커)를 그리고 있어
지금 소스와 어긋납니다. 재생성 절차만 `scripts\build_sheets.ps1` 에 남겨 뒀고
(소스 HTML 은 `docs\src\` 에 두는 것이 전제입니다 — 아직 없습니다), 지금의 정본은 위 문서 표입니다.

---

## 알려진 한계

**토큰 검증이 없습니다** — 세션 서버의 `SessionLogin` 이 `player_id` 를 그대로 믿습니다(검증 자리를 비워 둔 것).
마을은 세션 서버가 미리 걸어 둔 **예약을 소비한 세션에게만 신원을 줍니다** — 예약을 못 얻으면 게임 요청이
통째로 막힙니다. 다만 **인가되는 것은 소켓이 아니라 값입니다.** 예약 테이블이 `player_id → 만료시각` 뿐이라
**누가 그 예약을 쓰는지는 확인하지 않습니다** — `player_id` 를 아는 상대가 마을 포트에 직접 붙어
그 예약을 가로챌 수 있습니다(진짜 주인은 그다음에 거절됩니다). 막으려면 예약에 주인을 묶는 토큰이
있어야 하고, 그것이 곧 인증이라 이번 범위 밖입니다.

**같은 계정의 중복 접속은 끊고 교체합니다** — 세션 서버가 로그인 시 접속 테이블을 조회해 중복을
발견하면 먼저 붙은 마을 세션에 `Kick` 을 보내고 새 로그인은 즉시 「잠시 후 다시 시도」로
거절합니다(대기 0초 — 클라 재시도가 정리 완료 후 통과). 마을 쪽 `Enter` 의 중복 거절
(`player_id → session.id` 전역 표)은 2차 방어선으로 그대로 있습니다. 이 변경으로 한때 `SKIP`
이던 중복 세션 회귀 3건이 Kick 시나리오로 재활성됐고, 대신 「등록됐지만 미구현인 S2S 요청」
프로브 1건이 전제 소멸로 `SKIP` 이 됐습니다.

**단위 테스트가 없습니다.** 검증은 위의 통합 시나리오 하네스 · ASan · 크래시 덤프로 했습니다 —
동시성 · 수명 · 순서는 함수 단위로 잘라내기 어려워 통합 쪽을 택했습니다.
다만 「안 넣은 것」과 「못 넣는 것」은 다릅니다. 상계 계산이나 락 순서 정렬처럼
순수 함수인 것들은 넣을 수 있었고, 그건 넣지 않은 쪽이 맞습니다.

빈 존을 삭제하지 않습니다 — 실서비스가 목적이 아니라 방치가 더 단순하다고 봤습니다.
핫패스에 로그가 남아 있습니다 — 성능상 빼는 게 맞지만, 동작을 눈으로 확인하는 증거물이라 뒀습니다.
`config\server.ini` 의 DB 비밀번호가 평문입니다 — 실서비스라면 여기서부터 고쳐야 합니다.

존 멤버 목록(`kZoneMembersNtf`)은 **멤버가 드나들 때만** 나갑니다 — 존에 있는 채로 로그인하면
그 사람의 `player_id` 가 낡은 채로 남습니다. 거래는 `session_id` 로 지목하므로 기능에는 영향이
없고 표시만 틀립니다. 실제 게임은 「로그인 → 캐릭터 선택 → 존 입장」 순서라 안 생기는 상태입니다.
