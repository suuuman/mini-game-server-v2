# 검증 절차 — mini-game-server

> **단위 테스트가 없다.** 검증은 통합 시나리오 하네스 · ASan · 크래시 덤프로 한다.
> 동시성·수명·순서는 함수 단위로 잘라내기 어려워 통합 쪽을 택했다.
> (상계 계산·락 순서 정렬처럼 **순수 함수인 것들은 넣을 수 있었고, 그건 넣지 않은 쪽이 맞다** — `docs/DNF-GAP-ANALYSIS.md` 참조)

---

## 0. 실행 전 — 부하 주입 스위치 확인 (생략 불가)

⛔ **켠 채로 돌리면 회귀가 거짓말을 한다. 실제로 겪은 사고다.**

| 파일 | 스위치 | 기대값 |
|---|---|---|
| `src/app/frame_router.cpp` | `kLogicDelayMs` · `kBadSyncDbMs` · `kBadTradeNoTx` | `0` · `0` · `false` |
| `src/app/worker_pool.cpp` | `kBadTickWorkMs` · `kBadTickSpikeEvery` · `kBadTickSpikeMs` | `0` |
| `config/server.ini` | `idle_timeout_sec` | **`90`**(의도값 — ADR-023. `0` 이 아니다) · 자체 스폰 11종(+`chat`·`client`)은 스크래치에서 `0` 고정 · `idle.ps1` 과 `client.ps1` 의 마을 B 만 축소값 `2` |

⚠️ **정정 — 위치 이동 1건 · 소멸 1건.** `kBadTick*` 는 틱 스레드가 `zone_manager.cpp`
(폐기됨)에서 `src/app/worker_pool.cpp` 로 옮겨 가면서 함께 옮겼다(`worker_pool.cpp:15-17` · `:301-307`
실측 확인). `kDedicatedConn`(옛 `src/db/db_worker.h`)은 **행 자체가 사라졌다** — `DbWorkerPool` 이
Step 4 에서 통째로 삭제돼 그 스위치가 걸릴 코드가 없다(§5 「소멸」 절 참조. `src` 전수 grep 으로
`kDedicatedConn` 이 0건임을 확인했다).

⚠️ **`kBadSyncDbMs` 의 의미가 5단계 재구성에서 바뀌었다.** 예전엔 「존 스레드에서 동기 DB 대기」였다 —
그런데 지금은 워커가 DB 를 직접 동기 호출하는 것 자체가 정상 경로라 그 의미가 성립하지 않는다.
지금은 `handle_inventory`(`frame_router.cpp:1073-1081`)가 **`try_acquire` 를 부르기도 전에** 그
값만큼 `Sleep` 하고 즉시 응답하는 **워커 점유 주입**이다 — 코드 주석 그대로:
*"워커 하나가 이만큼 묶였을 때 다른 세션(다른 워커)이 영향을 받는지가 이 스위치의 관측 대상"*.
잡는 하네스는 `zone_block.ps1`(재정의) — §5 표에서 다시 다룬다.

> ⚠️ **위 항목은 「끄는 스위치」가 아니다** — `90` 이 커밋 의도값이고, 부하 주입 스위치 목록(위 세 줄)과는
> 성격이 다르다. 자체 스폰 11종(`zone`·`members`·`inventory`·`zone_block`·`trade`·`zone_race`·`gate`·`drain`·`idle`·**`chat`**·**`client`**)은
> `New-HarnessHome` 이 스크래치 config 사본에 `0` 을 강제로 덮어써 이 값의 영향을 안 받는다 —
> ping 을 안 보내는 그 하네스들이 멀쩡한 세션을 유휴로 끊기는 문제는 그래서 발생하지 않는다.
> `idle.ps1` 은 반대로 **자기 스크래치에 `2`(축소값)로 재오버라이드**해 유휴 킥 자체를 시험한다.
> `client.ps1` 도 S4 전용 마을 B(9010)만 `2` 로 다시 덮는다 — 유일하게 ping 을 보내는 클라(`client send --hold --ping-ms`)가
> 그 값 앞에서 살아남고, ping 없는 대조군이 끊기는 것을 함께 본다(A15 — S4a~c).
> ⭐ **사전 기동 갈래(`send`·`churn`)는 커밋값 `90` 을 그대로 쓴다** — 실측:
> `send 3000/3000`·`churn ×3` 무누수(핸들 델타 0·비누적) 확인. 이 갈래의 시나리오 최장 유휴는
> 약 0.8초로 `90` 초에 비해 압도적으로 짧아(0.8s « 90s) 유휴 절단이 걸릴 여지가 없다.

```powershell
# 한 번에 확인 — kDedicatedConn 행은 소멸해 더 이상 확인 대상이 아니다(위 절 참조)
Select-String -Path src/app/frame_router.cpp -Pattern "kLogicDelayMs|kBadSyncDbMs|kBadTradeNoTx"
Select-String -Path src/app/worker_pool.cpp -Pattern "kBadTick"
Select-String -Path config/server.ini -Pattern "idle_timeout_sec"
```

---

## 1. 빌드 (하드 게이트)

```powershell
.\scripts\build.ps1        # Debug · Release · ASan 세 구성
```

| 항목 | 기준 |
|---|---|
| 컴파일 에러 | **0** |
| 경고 | **0** — 이것이 기준선이다. 경고를 남기고 넘어가지 않는다 |
| 산출물 | `build\x64\{Debug,Release,ASan}\village.exe` |

```powershell
.\scripts\build.ps1 -Config Release          # 하나만
.\scripts\build.ps1 -Config Release -Run --bench   # 빌드하고 바로 실행
```

⛔ **빌드 명령을 추측하지 마라.** `scripts\build.ps1` 이 정본이다.
직접 MSBuild 를 부를 일이 생기면 그 스크립트가 무엇을 하는지 먼저 읽는다.

⚠️ **MSBuild 경로가 스크립트에 하드코딩돼 있다** (`Visual Studio\18\Community\...`).
다른 에디션·버전이면 `못 찾음` 으로 즉시 throw 한다 — **빌드 실패가 아니라 환경 문제다.**
그 경우 스크립트의 `$msb` 를 고치고, 고쳤다는 사실을 보고에 남긴다.

---

## 2. 회귀 하네스 — 가벼운 것부터 무거운 것으로

⛔ **한 번에 다 돌릴 수 없다 — 하네스 묶음이 두 갈래로 갈렸다** (실측).

| 갈래 | 하네스 | 서버를 누가 띄우는가 |
|---|---|---|
| **A. 사전 기동** | `send` · `churn` | **사람이** 먼저 띄운다 — `.\build\x64\Release\village.exe --seconds 60` (**반드시 프로젝트 루트에서**) |
| **B. 자체 스폰** | `zone` · `members` · `inventory` · `zone_block` · `trade` · `zone_race` · `gate` · `s2s` · `session` · `drain` · `idle` · **`chat`** · **`client`** | **스크립트가** 스스로 띄운다(스크래치 config 사본 + 가짜 세션 서버 — `session`·`client` 는 실물 `session.exe`) |

⛔ **두 갈래가 같은 포트(클라 9000)를 쓴다.** 그래서 순서가 강제된다:

```
① 서버 기동 → A(send · churn) → ② 서버 종료 → ③ B(각자 스폰)
```

⛔ **서버를 띄운 채 B 를 돌리면** 포트를 점유당해 스폰이 실패하거나 **엉뚱한 서버에 붙는다.**
⚠️ **실제로 겪었다** — 8종을 순차 실행하려다 `send` 가 접속 거부로 즉시 죽었고,
반대로 서버를 띄우면 자체 스폰 쪽이 깨진다. **원인은 B 를 자체 스폰으로 바꾸면서
`send`·`churn` 두 종만 정당하게 제외한 것**이고, 그 운영 결과를 아무도 따지지 않았다.

⛔ **게이트는 한 번에 한 쪽만 돌린다** — 워크트리를 나눠도 포트(9000·9100·9200)는 공유된다.

| # | 명령 | 검증하는 성질 |
|---|---|---|
| 1 | `.\scripts\send.ps1 -Repeat 3000 -Size 8 -Framed -Seq` | 프레임 경계 복원 · 순서 |
| 2 | `.\scripts\zone.ps1 -Clients 8 -Zones 4 -Chats 100` | 존 경계 브로드캐스트 |
| 3 | `.\scripts\members.ps1` | 존 멤버 목록 통지 — 4개 변동 지점이 전부 통지를 내는가 |
| 4 | `.\scripts\inventory.ps1 -Mixed` | DB 왕복 |
| 5 | `.\scripts\zone_block.ps1`(기본 `-FloodClients 10 -Floods 30 -Config Release`) | **워커-풀 격리**(재정의 — 옛 「존 스레드 독점」은 존 스레드 소멸로 성립 안 함) — DB 폭탄이 커넥션 풀(`[db] pool_size=4`)을 다 잠가도 무관한 세션의 에코 왕복이 안 밀리는가. **판정 4축**(하드 3 + 소프트 1): ① B(에코) 왕복 max ≤ 기준선+50ms — 하드 ② `kTradeConfirmReq` 가 풀 고갈 중 `kBusy` 를 **관측**(⚠️ 확률적 — 프로브마다 풀을 두드리는 순간이 달라 busy/비-busy 혼재가 정상. 최대 3회 재시도, **미관측은 경고만·종합에 안 들어간다 — 소프트**) ③ 종료 로그 `[POOL2] try_failed>0` — 하드(**결정적 증거** — 판정2 가 미관측이어도 「풀이 실제로 꽉 찼다」는 이것이 증명한다) ④ **위반 축 — 하드**: confirm 응답을 받았는데 **kBusy 가 0건**이면 실패 — §7-4(커넥션을 락·검사보다 먼저)가 역전되면 kBusy 가 원천적으로 나올 수 없어 이것이 결정적 시그니처다(역전 뮤턴트 실측: 응답 15개·busy 0 → 판정4 사망 / 정상 3연속: busy 관측·위반 없음). ⚠️ **`FloodClients` 는 `pool_size`(4) 뿐 아니라 `[app] workers`(8)도 넘겨야 한다** — 6개로는 빈 워커가 남아 B 가 그리로 배정돼 판정1 이 거짓 통과한다(M4 뮤턴트 실측) |
| 6 | `.\scripts\trade.ps1 -All -DbUser sp_owner -DbPass '<비번>'` | 거래 **총 25항목 — 25 통과 · SKIP 0** · **아이템 총량 보존** (⛔ **계정 인자 필수** — 아래 참조. ⭐ **`DupTest` 를 재활성했다** — 이제 「거래 중 동기 Kick → 거래 정리 4/4(`kTradeCancelNtf` 직접 단언) → 재입장 후 실거래 confirm 완주 → DB 총량 보존」 5단언 시나리오다) |
| 7 | `.\scripts\zone_race.ps1 -Pairs 3000`(기본 **`-Config Debug`** — 옛 기본은 Release 였다) | **락 계층 검사기(`core/lock_rank.h`) 무발화 + 정합**(재정의 — 옛 「존 배정 어긋남」assert 는 존 배정 자체가 소멸) — 시나리오(존 이동 폭탄 + 교차 요청)는 유지. **판정 2종**: ① Debug/ASan 에서 `LockRankGuard` assert 무발화(L1→L2 역순 없음) ② 스트림 정합(`JoinZoneAck`/`ZoneMembersNtf` 개수·파손 0). ⛔ **기본값이 Debug 로 바뀌었지만 여전히 `-Config Release` 로 돌리면 검사기가 통째로 no-op 이다** — 아래 절 |
| 8 | `.\scripts\churn.ps1 -Count 1000 -Framed` | 접속·종료 반복 · 누수 |
| 9 | `.\scripts\s2s.ps1` | S2S 커넥터 시나리오 **총 29항목 — 28 통과 + SKIP 1** — 등록·매칭 테이블·백오프 수열·재연결·§8-4 검증 규칙·stop 정리 + 정원 거부(full) 재시도 + **Kick 코덱(⑯ NotFound 왕복 · ⑰ 길이 위반 절단 · ⑱ 이중 Kick 자극)**. ⚠️ **SKIP 1(⑦)은 정상** — 「등록됐지만 미구현인 요청 → Unsupported 폴백」의 마지막 사례(Kick)가 구현돼 와이어로 도달할 프로브가 없다. 재활성 조건: 새 세션→마을 요청 id 가 분류표에 먼저 등록되는 시기 |
| 10 | `.\scripts\session.ps1` | **세션 서버 통합 95항목(SKIP 0)** — Phase 1~6 + 실물 `session.exe`+`village.exe` 연동. 수용·레지스트리·헬스/orphan·배정·예약·홀드 수지·pending 항등식 + **실물 Enter·예약 소비·PlayerEnter/Leave·FullSync 청크·Unregister 유예** + **드레인 운영 명령(S1~S4)** + **동기 Kick(K1~K12 — Busy 즉시 거절·Kick 수행·재발신 억제 mid-check·NotFound 유령 정리·orphan 허용·미매칭 seq·타임아웃 재발신 재개·링크다운 정리·`[SESS ] kick` 종료 항등식 K11)** + **재연결 대조 Kick(R1~R6 · 별도 인스턴스 — 발화·pending 중 재전송 억제·비대상 무영향·NotFound 오삭제 방지·청크 2+ 발화·kick/connections 종료 요약 절대값)**. 한때 SKIP 이던 P6-H2a/H2b 는 전제(무인증 직행 로그인)가 예약 강제로 영구 소멸해 **K 계열이 대체**했다 |

| 11 | `.\scripts\gate.ps1` | **예약 강제 게이트 11항목** — 미인증 `kJoinZoneReq`·`kChatReq`·`kInventoryReq`·미정의 id 가 끊기는가(N1~N4a) · 입장한 세션의 관용은 남는가(N4b) · 예약 없는 `Enter` 는 `InvalidArg` 로 **끊지 않고** 거절하는가(N5) · 예약 경유 `Enter` 는 통과하는가(N6) · **같은 `player_id` 중복 `Enter` 는 두 번째만 거절·연결 유지(N7 — session.ps1 P6-7b 원형을 이식: 마을 단독 분기의 유일한 결정적 커버리지)** |
| 12 | `.\scripts\drain.ps1` | **드레인 시나리오 19판정(D1~D10)** — 운영 명령(SetMode)으로 드레인 진입 시 `Reserve` 거절(D2)·`Enter` 는 `kBusy` 로 거절되되 연결은 유지(D3)·기존 접속 무영향(D4)·`DrainComplete` 발신 1회(D5)·재발신 없음(D6)·복귀 시 재입장 성공(D7)·거절된 예약은 저장 안 됨(D7b)·재-drain 시 플래그 리셋(D8)·종료 지표 5종(D9)·**링크 절단→재-accept→재등록 시 SetMode 재전송 없이 재발신 1회(D10 — 재연결 리셋 실증)**. ⚠️ 옛 「17판정」 표기는 D3 보강(+1 · 18)이 이 표에 전파되지 않은 채 남아 있던 것 — 실측 기준(18+D10=19)으로 정정했다 |
| 13 | `.\scripts\idle.ps1` | **유휴 절단 시나리오 9판정(I1~I4)** — 무발신 소켓이 `idle_timeout_sec` 안에 끊기는가(I1)·주기 ping 소켓은 생존하는가(I2)·입장 세션도 idle kick 되고 같은 `player_id` 로 재입장 가능한가(I3, `entry.leave()` 수명 정리 실증)·종료 로그 `idle_kicked` 누적(I4) |
| 14 | `.\scripts\chat.ps1` | **채팅 3종 48판정(C1~C11)** — 전체(All) 존 무관 전달(C1·C1b)·지역(Zone) 교차 미수신(C2)·귓속말(대상만 수신·자기 자신·부재 시 `kChatAck`)(C3~C5)·경계값 절단(미정의 type·빈 body·whisper target 잘림)(C6~C8)·텍스트 상한 초과(-9 재계산 경로)(C11)·**§17-6 큐 넘침 킥**(C1~C5 클라 격리 뒤 `send_full_kicked==1` 수치 단언 + 3/4 수위 `[WARN]`)(C9)·종료 지표 5종(C10) |
| 15 | `.\scripts\client.ps1` | **C++ 클라이언트(`client.exe`) 회귀 35판정**(T015 · ADR-029 — S0 · S1 · S1b · S2×4 · S3a~f · S6 · S7 · S8~S21 · S5×3 · S4a~c) — `selftest` 순수 함수 12항목(S0) · `send` 이식(S1 순서 3000 · S1b **`send.ps1` 과 순서 판정 1종 동치** · S3b/S3e/S8/S9/S12/S16 판정 분기) · `flow`(S2 login/enter/echo/pong 정상 · S3a/S3c/S3d/S3f/S13/S14/S15/S17/S18/S19 부정 대조군) · usage/exit 2·3(S6/S7/S10/S11/S20/S21) · **A15 주기 ping**(S4a~c — `idle_timeout_sec=2` 마을 B 에서 ping 1s → 7s 생존 `pongs≥5` / ping 없음 → 절단 · `idle_kicked=1`) · 마을 A 종료 로그(S5 `[CONN ] rejected=0` · `[NET  ] idle_kicked=0` · `[ENTER]` 정확히 2). **종료 코드 = 실패 건수**(0 이면 35/35). ⛔ **단독 실행**(아래 절) · 서버 소스가 안 바뀐 클라 작업의 회귀는 이것 1종 + 자체 스폰 대표 1종(`zone.ps1`)으로 충분하다(서버 바이너리가 같은 소스에서 재빌드됐다는 전제 — T015 8단계 절차) |

⚠️ **15종이다.** 한때 이 표에 `members.ps1` 이 빠져 7종이었고 다른 문서와 어긋나 있었다(정정).
`s2s.ps1` 으로 9종, `session.ps1` 으로 10종, `gate.ps1` 으로 11종,
`drain.ps1`·`idle.ps1` 로 13종, `chat.ps1` 로 14종, **`client.ps1` 로 15종**이 됐다.
⚠️ **번호는 추가 순서다. 「가벼운 것부터」 순서와 다르다** — 기존 번호를 재배치하면
아래 「변경 유형별 최소 세트」의 참조가 조용히 어긋난다(실제로 겪었다).

⛔ **`session.ps1` 도 단독 실행이다** — `session.exe` 와 `village.exe` 를 **둘 다 스스로 스폰**한다(스크래치에 `config` 사본을 만들어 세션은 판정값을 축소하고, 마을은 `[s2s] host` 를 켠다). 위 1~8·11~14 중 자기 제외와 창을 공유하지 않는다.
⛔ 세션 수명(S2S 수용 연결의 영구 홀드)을 만지므로 **`session.ps1`·`s2s.ps1` 은 ASan 구성으로도 한 번씩** 돌려야 검증된 것으로 친다(ADR-020).

⛔ **`client.ps1` 도 단독 실행이다** — `session.ps1` 과 같은 두 서버(세션 클라 9200 · S2S 수용 9100 · 마을 A 9000)에 더해
S4 전용 마을 B 가 **9010**(`session.ps1` 내부 시나리오가 쓰는 포트)을 쓴다. 실행 전 `Get-Process village,session` 이 비어 있어야 한다.
ASan 회차는 `.\scripts\client.ps1 -Config ASan`(서버 3종 + 클라 자체 메모리 — 별도 프로세스라 클라 ASan 이 서버 결함을 더 잡아 주지는 않는다, ADR-029 결정 9).
세션 서버는 `config/session.ini` **미패치 사본**으로 스폰하고 `--seconds` 를 넘기지 않는다(넘기면 stdin 을 안 읽어 `Stop-Harness` 개행이 안 먹는다 — `src/session/main.cpp` 의 `auto_seconds` 갈래).

⛔ **`s2s.ps1` 은 다른 열셋과 실행 모양이 반대다** — 클라이언트를 흉내 내는 것이 아니라 **가짜 세션
서버(TcpListener 9100)를 흉내 내고, village.exe 를 스스로 스폰**한다(스크래치에 `config` 사본을 만들어
`[s2s]` 만 축소값으로 패치한 뒤 그 디렉터리를 cwd 로). 그래서 **서버를 미리 띄우지 않는다** — 위 1~8·11~14 중
자기 제외와 같은 창 절차에 끼워 넣지 말고 서버가 없는 상태에서 단독 실행한다.
⛔ **ASan 회차를 `run-asan.ps1` 로 돌리면 안 된다** — 그 스크립트는 커밋본 config(S2S 비활성)로 서버를
띄우므로 시나리오가 하나도 안 돌면서 통과처럼 보인다. `.\scripts\s2s.ps1 -Config ASan` 으로 돌린다
(ASan DLL PATH 처리가 스크립트 안에 복제돼 있다 — run-asan.ps1 과 동기화 주석 참조).

⛔ **ASan DLL PATH 처리는 이제 세 곳에 있다** — `run-asan.ps1` · `session.ps1` · **`harness_common.ps1`**
(`Start-Village()`). 앞의 둘은 각자 골격이라 공용 함수로 묶이지 않고, 셋째는 자체 스폰 11종
(`zone`·`members`·`inventory`·`zone_block`·`trade`·`zone_race`·`gate`·`drain`·`idle`·**`chat`**·**`client`**)이 공유한다
(`client.ps1` 은 `Start-Village` 가 `$env:PATH` 앞에 넣은 asan DLL 경로를 뒤에 뜨는 `session.exe`·`client.exe` 가 상속하는 데 기댄다).
⚠️ **리터럴이 같은 동안만 `find_copies.ps1` 이 동기화를 지킨다.** 글롭 경로를 고치면 세 곳을 함께 본다.
⛔ **한때 셋째가 없어 자체 스폰 종들이 `-Config ASan` 에서 전부 기동 실패했다** —
증상은 「가짜 세션 서버에 village 의 S2S 연결이 오지 않았다」로만 보여 **원인과 멀다.**

### ⛔⛔ `zone_race.ps1` 은 **`assert` 가 살아 있는 구성에서 돌려야 뜻이 있다** (실증 · 판정 대상이 교체됐다)

이 하네스의 판정은 여전히 **`assert` 발화 여부**다(`MainWindowTitle` 이 `Runtime Library` / `Assert`
인지 본다) — 다만 **5단계 재구성부터 죽는 대상이 바뀌었다.** 존 배정 assert(`zone_manager.cpp` — 폐기됨)가
아니라 **`core/lock_rank.h` 의 `LockRankGuard`**(락 계층 역순 검사기)다. ⭐ **기본값도 그때 바뀌었다** —
`-Config Debug` 가 기본이다(옛 기본은 Release). 그래도 성립 조건은 그대로다: **명시적으로
`-Config Release` 를 주면 검사기가 no-op 이라 아무것도 안 잡는다.**

| 구성 | 전처리기 (`server/common.props`) | 검사기(`LockRankGuard`) | 이 하네스의 뜻 |
|---|---|---|---|
| Debug | `_DEBUG` | 살아 있다 | ⭕ 락 계층 역순을 검증한다 |
| **ASan** | `_DEBUG` | 살아 있다 | ⭕ 검증한다 |
| **Release** | `NDEBUG` | ⛔ **컴파일아웃**(`lock_rank.h` `#else` 분기 — 전부 no-op) | ⛔ **역순으로 잡아도 통과한다** |

⇒ ⛔ **`-Config Release` 를 명시하면 락 계층 불변식은 검증되지 않는다.** 회귀는 기본값(Debug) 이나
ASan 회차를 반드시 포함한다.

⭐ **감지 자체는 동작한다 — 뮤턴트(M2: 계층 역순 획득 주입)로 재실증했다.** 옛 존 배정
assert 의 실화 확인(`zone_manager.cpp` 불변식을 깨서 `-Config Debug` 로 돌리자 `판정 : X assert
발화` 가 났다)은 **그 assert 자체가 폐기돼 더는 유효한 증거가 아니다** — 새 검사기로 같은
성질(즉시 킬)을 다시 실측했다(M2 — 계층 역순 주입 즉시 킬). ⚠️ **스폰 방식**(Win32 `CreateProcess`
직결 + stdin 파이프)과 **`MainWindowTitle` 판정 메커니즘**은 그때 확립한 그대로 재사용된다 — 바뀐
것은 **무엇을 깨서 죽이는가**뿐이다.
⛔ **ASan 구성에서도 성립하는 것은 전처리기 정의로 확인한 것이고, 발화를 실측으로 본 것은 Debug 뿐이다.**

**각 하네스는 하나의 성질만 검증한다.** 실패하면 어느 성질이 깨졌는지가 바로 나온다.

### ⛔ `trade.ps1` 은 `inventory` 테이블을 통째로 비운다

`Reset-Inventory` 가 `DELETE FROM inventory` 를 친다. 그래서:

- **`inventory.ps1` 을 `trade.ps1` 보다 먼저** 돌린다 (위 순서가 그렇게 돼 있다)
- **부하 측정을 하려면 `seed_load.ps1` 로 데이터를 다시 깔아야 한다**

```powershell
.\scripts\seed_load.ps1 -User sp_owner -Password '<비번>'    # 부하용 플레이어·인벤토리 시드

# ⭐ 응답 상한(kMaxRows) 경계를 재려면 — 한 플레이어에게 N 행을 몰아 심는다 (신설)
.\scripts\seed_load.ps1 -BulkPlayer 900 -Rows 512 -User sp_owner -Password '<비번>'
.\scripts\seed_load.ps1 -Clean -Base 900 -User sp_owner -Password '<비번>'   # 뒷정리
```

⛔ **`-User`/`-Password` 기본값(`minigame`)은 더 이상 통하지 않는다** — 테이블 권한이 없다.

> 전원 같은 행수로 까는 이유 — 행이 있는 플레이어와 없는 플레이어가 섞이면
> 조회 비용이 갈려서 측정값이 흔들린다.


### ⛔ 테이블을 직접 읽는 하네스는 **`sp_owner` 로 붙는다**

서버 계정 `minigame` 은 이제 **`EXECUTE` 만** 갖는다(ADR-014). 하네스가 결과를 **테이블로 대조**하려면
정의자 계정이 필요하다. 기본값으로 돌리면 **`1142` 로 죽는다**(의도된 동작이다).

| 하네스 | 필요한 인자 |
|---|---|
| `trade.ps1` | `-DbUser sp_owner -DbPass '<비번>'` |
| `seed_load.ps1` | `-User sp_owner -Password '<비번>'` |
| `inventory.ps1` | 없음 — 서버를 통해서만 본다 |

### ⭐ `inventory.ps1` 의 판정 스위치 3종 (신설)

| 스위치 | 무엇을 검사하나 | 쓰는 곳 |
|---|---|---|
| `-ExpectDbError` | 모든 응답이 **정확히 `kDbError`** 인가 | SP 미배포·권한 결핍 시나리오. ⛔ **「빈 목록」과 「못 읽었다」를 가르는 핵심** |
| `-ExpectCount <N>` | 모든 응답의 `count` 가 N 과 같은가 | `kMaxRows` 경계(510·511·512) |
| `-AfterPartialDeploy` | 부분 배포 상태 **표시용** | ⚠️ **판정은 이 스위치가 내리지 않는다** |

> ⭐ **`-ExpectDbError` 가 SP 전면화의 값어치를 지키는 스위치다.**
> 조회가 실패했는데 **빈 목록**으로 돌아오면 유저에겐 **아이템이 사라진 것**이다.
> 「응답이 왔다」와 「제대로 왔다」는 다르다.

### ⛔ `churn.ps1` 은 ASan 빌드에서 **스스로 거부한다**

ASan 은 `free` 한 메모리를 격리에 붙들어 둬서 **누수가 없어도 세션 수만큼 선형으로 늘어난다.**
그래서 하네스가 ASan 서버를 감지하면 실행을 막는다(설계된 가드).
⇒ **ASan 회차의 세션 수명 판정은 `churn` 이 아니라 ASan 리포트(use-after-free)로 한다.**
누수 수치가 필요하면 `Debug`/`Release` 로 다시 띄운다.

### 측정용 하네스 (회귀와 목적이 다르다)

위 15종이 **「깨졌는가」**를 보는 것이라면, 아래는 **「얼마나 걸리는가」**를 잰다.
(⚠️ 이 문장은 표가 7종이던 시절의 「7종」이 **두 번의 하네스 추가(members · s2s)를 지나도록**
낡은 채 남아 있었다 — 최종 리뷰가 잡았다. 표의 행 수를 바꾸면 이 문장도 함께 본다.
⚠️ `drain`·`idle` 로 11→13 으로 다시 늘었다 — 이 경고가 스스로 말한 그대로 함께 본다.
⚠️ `chat` 으로 13→14 로 다시 늘었다 — 이 경고가 스스로 말한 그대로 함께 본다.
⚠️ `client` 로 14→15 로 다시 늘었다 — 이 경고가 스스로 말한 그대로 함께 본다.)
회귀에는 안 넣고, 성능 판단이 필요할 때만 돌린다.

| 명령 | 재는 것 | 판정 |
|---|---|---|
| `.\scripts\tick.ps1` | **틱 지연·지터 전용 하네스** — 고장 스위치(`-WorkMs`·`-SpikeEvery`/`-SpikeMs`)와 `[tick]` 정책을 함께 다룬다 | `[TICK ]` `[TICK2]` |
| `.\scripts\load.ps1` | 동시 접속 N개를 붙잡은 채 왕복 지연 | 응답 시각 분포 |
| `.\scripts\dbload.ps1` | **워커 수 × DB 풀 크기 실측** — `-AppWorkers`·`-PoolSize` 로 스크래치 config 를 바꿔 스폰하고, 응답을 ok/busy/other 로 갈라 세며 종료 로그의 `[POOL2]`·`[WORK ]`·`[NET  ]` 를 스스로 파싱해 `RESULT` 한 줄로 낸다(T014). ~~서로 다른 `player_id` 로 워커를 가른다~~ — 그 축은 배정이 직렬 큐 유동이 되며 소멸했고 `[SKEW ]` 대조도 함께 지웠다 | `RESULT` 줄(`qps` 는 **ok 기준**) · `[POOL2] try_failed`(⚠️ DB 커넥션 풀 통계는 `[POOL ]`(프레임 풀)이 아니라 `[POOL2]` 다). ⛔ `-Traders` 를 주면 `ms` 에 거래 ACK 대기(연결당 최대 3s — `dbload.ps1:253`)가 섞여 **처리량 지표로 못 쓴다**(같은 조건 3회에서 26배 편차 실측 — ADR-028) |
| `.\scripts\drain_batch.ps1` | **`kSerialDrainBatch` 지연·비용 실측**(T014 신설) — 플러더 세션이 인벤토리 N건을 한 번의 Write 로 쏟아 직렬 큐를 깊게 만든 채, 예약 없는 Echo 프로브의 왕복 avg/p99/max 와 플러드 완료 시간을 잰다. `-Batch K` 는 `worker_pool.h` 의 상수를 패치해 Release 를 빌드한다(`tick.ps1` 의 3겹 안전장치 · `finally` 원복) · `-AppWorkers 1` 이 격리 측정 | `RESULT` 줄 · 유효성 3종(`cap_hits>0` · `kicked=0` · `lost=0`) → **`VALID=1` 회차만 채택** · `[WORK ]` |

> ⚠️ **`zone_block.ps1` 은 `player 1` 하나만 쓴다** — 워커 하나만 돌아서 풀 크기 측정에는 안 맞는다.
> 그 목적에는 `dbload.ps1` 을 쓴다.
>
> ⛔ **`drain_batch.ps1 -Batch K` 는 소스(`worker_pool.h`)와 exe 를 바꾼다.** `-KeepBuild` 로 스윕했으면 마지막에 **반드시 `-Restore`** —
> 잊으면 소스는 16 인데 exe 는 K 인 채로 다음 하네스가 돈다 — 그 상태는 exe 옆 마커 `build\x64\Release\village.exe.kSerialDrainBatch`(내용 `K=N`)가 알리고, `-Restore` 가 지운다. 확인은 `Select-String -Path src\app\worker_pool.h -Pattern 'constexpr size_t kSerialDrainBatch = \d+;'`(값 16)와
> `git diff -- src/app/worker_pool.h | Select-String '^[+-].*constexpr size_t kSerialDrainBatch'`(0줄).
> ⚠️ `Write-Host` 출력을 `*>` 로 파일에 받으면 **콘솔 폭(150)에서 하드 랩**돼 `RESULT` 줄이 토큰 중간에서 잘린다 — 스윕 스크립트에서 `$host.UI.RawUI.BufferSize` 폭을 넓히거나(1000 실측 OK) 콘솔로 받는다.
>
> ⚠️ `tick.ps1` 의 고장 스위치는 **`src/app/worker_pool.cpp` 의 `constexpr`**(`kBadTickWorkMs` ·
> `kBadTickSpikeEvery` · `kBadTickSpikeMs`)**를 패치하고 재빌드**해야 한다.
> ⛔ **정정** — 옛 `zone_manager.cpp` 는 5단계에서 삭제됐다. 이관 사실은 **§0 이 이미 적고 있었는데
> 이 줄만 낡은 채 남아 있었다** — 같은 문서 안의 모순이라 어느 쪽도 서로를 안 고쳤다.
> §0의 「부하 주입 스위치 꺼짐」과 직접 충돌하므로 **돌린 뒤 반드시 원복**한다.

### 변경 유형별 최소 세트

**● 필수 · ◐ 조건부(그 행의 조건 각주를 본다) · ✗ 그 구성에서 하네스가 스스로 거부**
구성 표기 — `D` Debug 필수 · `R` Release · `A` ASan 의무 · `3` Debug·Release·ASan 세 구성

| 변경한 것 | 1 send | 2 zone | 3 memb | 4 inv | 5 zblk | 6 trade | 7 race | 8 churn | 9 s2s | 10 sess | 11 gate | 12 drain | 13 idle | 14 chat | 15 client |
|---|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|
| 프로토콜 · 프레이밍 | ● | ● | | | | | | | | | | | | | ● |
| 락 · `EntryTable` · 직렬 큐 <sup>a</sup> | | ● | ● | | ◐ | ◐ | ●<sup>D</sup> | ◐<sup>DR</sup> | | ● | ● | | | ● | |
| 채팅 · 브로드캐스트 <sup>b</sup> | | ● | | | | | | | | | | | | ● | |
| DB · 트랜잭션 <sup>c</sup> | | | | ● | ● | ● | | | | | | | | | |
| 세션 수명(`acquire`/`release`) <sup>d</sup> | ●<sup>A</sup> | ●<sup>A</sup> | ●<sup>A</sup> | | | | | ●<sup>DR</sup>✗ | | | | | | | |
| 틱 · 시뮬레이션 <sup>e</sup> | | ● | | ● | | | | | | | | | | | |
| S2S 커넥터 · `s2s_link` · `s2s_packet` <sup>f</sup> | | | | | | | | | ●<sup>3</sup> | | | | | | |
| 세션 서버(`src/session/`) · 수용 경로 <sup>f</sup> | | | | | | | | | ●<sup>3</sup> | ●<sup>RA</sup> | | | | | ● |
| 예약 게이트 · 신원 확정 <sup>g</sup> | | | | | | ● | | | | ● | ● | | | | ● |
| 드레인 · 서비스 상태 | | | | | | | | | ● | ● | | ● | | | |
| idle · 유휴 정리 | | | | | | | | ●<sup>DR</sup> | | | | | ● | | ● |
| 동기 Kick <sup>h</sup> | | | | | | ● | | | ●<sup>A</sup> | ●<sup>A</sup> | ● | | | | |
| C++ 클라(`src/client/`) · `client.ps1` <sup>i</sup> | | ◐ | | | | | | | | | | | | | ● |

<sup>a</sup> **락(L1 명부·L2 세션)·`EntryTable`(존 인덱스·`move_zone`)·직렬 큐(`serial_queue`)** — 옛 「존·존 스레드·placement」 행을 대체한다(`placement.h`·`zone_manager.{h,cpp}` 는 삭제됐다). **◐ 조건**: 명부 갱신 API(`enter`/`leave`/`move_zone`)나 직렬 큐 push/drain 경로를 건드렸으면 5·6·8 도 포함한다. 7 은 락 계층 검사기라 **Debug/ASan 이어야 뜻이 있다**.
<sup>b</sup> `handle_chat`·`build_frame`·`snapshot_zone`/`snapshot_all`·`find_acquire_by_player_id`(신설). **⛔ 위 <sup>a</sup> 행의 필수 세트를 그대로 물려받는다** — 채팅이 명부 스냅샷·통짜 프레임 조립을 그대로 타기 때문이다.
<sup>c</sup> 5 는 **워커-풀 격리 재정의판**이다. ⛔ **SP 경로를 건드렸으면 `-ExpectDbError` 로 「미배포」·「권한 결핍」도 함께 본다.**
<sup>d</sup> ⛔ **`churn` 은 ASan 에서 하네스가 스스로 거부한다**(아래 절) — 그래서 ASan 축은 1·2·3 계열이 진다. `churn` 자체는 Debug·Release 만.
<sup>e</sup> `[TICK ]`/`[TICK2]` 를 기능 추가 전후로 비교한다. ⚠️ **정정** — 소유가 존 스레드에서 전용 틱 스레드(`app::worker_pool.cpp`)로 바뀌었다. 지표 의미·비교 방법은 그대로다.
<sup>f</sup> `main.cpp`/`config` 배선(S2S)이나 `proto` 공유 헤더(세션 서버)까지 건드렸으면 **전 세트**를 돌린다.
<sup>g</sup> `handle_enter`·`EntryTable`·`on_frame` 진입부. ⛔ **게이트는 `gate.ps1` 말고는 아무도 안 본다** — 뮤턴트 실측: 게이트를 무력화해도 죽는 것은 `gate.ps1` **N1~N4a 4건뿐**이고 `session.ps1` 73 항목은 **전부 통과했다**(그 73 개 중 미인증 상태로 게임 요청을 보내는 것이 0개다 — 그 시점 총량. 뒤에 78 로 늘었지만 이 결론을 재실측하지는 않았다).
<sup>h</sup> `find_connection`·`PendingKick`·`find_session`·`close_by_id`·Kick 코덱·`with_snapshot`. 10 은 **K 계열이 주 검증**, 9 는 코덱·NotFound·malformed, 6 은 거래 중 Kick 정리, 11 은 마을 중복 거절 잔존 분기다. ⛔ **세션 수명(`close_by_id`)을 건드리므로 session·s2s ASan 회차 의무.**
<sup>i</sup> 클라만 바뀌고 서버 소스가 0줄이면 15 하나로 충분하다(S1b 가 `send.ps1` 동치를 하네스 안에서 함께 돌린다). **◐ 2 는 `.sln`·`.vcxproj` 가 바뀌어 서버 exe 가 재빌드됐을 때** — 자체 스폰 대표 1종으로 기동·회귀 무이상을 본다(T015 8단계 절차). 클라 소스는 `proto/packet.h` 만 include 하므로 와이어 규약(`packet.h`)이 바뀌면 15 는 **프로토콜 행**에서 ● 로 걸린다.

> ⚠️ 이 표의 번호는 §2 표 기준이다. **3번(members)을 삽입했을 때 이 표의 번호가 갱신되지
> 않은 채 남아 있었다**(뒤에 발견·정정) — 그래서 이름을 병기한다. 행을 넣고 빼면 이 표도 같이 본다.

> ⛔ **"이 변경은 저기 안 닿는다"는 판단으로 하네스를 건너뛰지 마라.**
> 건너뛰었으면 **건너뛰었다고 보고에 명시한다.** 조용히 생략하지 않는다.

### 게이트 운영 규칙 (실측으로 확정)

**게이트는 한 번에 한 쪽만 돌린다.** 하네스가 고정 포트(9000 마을 클라 · 9100 S2S · 9200 세션 클라)를
쓰고 워크트리를 나눠도 포트는 공유된다. 게다가 하네스 대부분이 시작할 때
`Get-Process village | Stop-Process -Force` 를 실행하므로 **동시에 돌리면 서로의 서버를 죽인다.**
두 사람이 같은 시각에 돌려 `village exit = -1` 이 한 번 났고, 하네스는 전건
통과한 채 크래시 증거가 없어(덤프 0 · assert 0) 원인 규명에 한 단계를 더 썼다.

**ASan `village.exe` 를 직접 띄우면 무음 실패한다.** `clang_rt.asan_dynamic-x86_64.dll` 이 MSVC 의
`bin\Hostx64\x64` 에 있어서 PATH 에 없으면 **로그가 한 줄도 안 남고** 시작조차 못 한다.
하네스 스크립트(`s2s.ps1`·`session.ps1`)는 그 경로를 스스로 잡는다 — 직접 실행할 때만 사람이 잡아야 한다.

**하네스 간 상태 오염이 있다.** `members.ps1` 이 3/11 실패한 회차가 있었는데, 그 회차는 `churn` 이
ASan 에서 거부되며 중단된 직후였다. 단독 재실행하면 11/11 이다. **앞 하네스가 비정상 종료했으면
다음 하네스 결과를 그대로 믿지 말고 단독으로 다시 돌린다.**

**개행 검사는 바이트 단위로만 신뢰할 수 있다.** 같은 파일을 두고 PowerShell
`ReadAllBytes` 순회는 `CRLF=0 bareLF=2457`, Git Bash `grep -c` 는 `CR줄=2457` 로 **정반대**를 냈다.
Git Bash 의 텍스트 모드 변환이 결과를 오염시킨다. **python `b.count(b'\r\n')` 로 판정한다.**

⛔⛔ **도구를 고쳐도 부족하다 — 「어느 바이트를 재는가」를 먼저 정해야 한다** (실측).
`.gitattributes` 의 `text eol=crlf` 는 **저장은 LF · 체크아웃은 CRLF** 가 **정상 동작**이다.
⇒ **작업본과 커밋 blob 의 개행이 다른 것이 맞다.** 어느 쪽을 쟀는지 밝히지 않은 수치는 판정에 못 쓴다.

| 무엇을 재나 | 명령 | `.ps1` 의 정상값 |
|---|---|---|
| **워크트리 작업본** | `python` 으로 파일을 그대로 읽는다 | `crlf=N bare_lf=0` |
| **커밋 blob** | `git show <ref>:<경로>` 를 파이프로 받아 잰다 | `crlf=0 bare_lf=N` |

> **실측** — `find_copies.ps1` 이 *"LF 라서 `.gitattributes` 위반"* 이라는 **이월 항목으로 남아 있었는데
> 오진이었다.** blob `crlf=0 bare_lf=101` · 작업본 `crlf=101 bare_lf=0` 이고, 대조군 `build.ps1` 도
> `0/82` 대 `82/0` 으로 **똑같은 형태**였다. **모든 `.ps1` 의 blob 이 LF 다.**
> ⛔ **고쳤다면 오히려 정규화가 깨졌을 것이다.** 원인은 `main` 의 **낡은 작업본**을 잰 것이었다.

### 하네스가 구조적으로 못 덮는 것 (실측 — 사유가 서로 다르다)

⛔ **사유를 섞지 마라.** 지금 **네 가지**다 — 「창을 못 만든다」·「도구가 못 본다」·
**「전제가 사라져 재현 불가능해졌다」**·**「미배선이다」**. 같은 것으로 읽으면 대책도 틀린 것을 고른다.

⭐ **세 번째가 앞의 둘과 다른 점** — 앞의 둘은 **하네스의 한계**라 도구·시간이 생기면 덮을 수 있다.
세 번째는 **제품이 바뀌어 그 상황 자체가 성립하지 않는 것**이다. 방어 코드는 남아 있고 검증만 못 한다.
⇒ ⛔ **「지워도 되는 코드」로 오해되기 쉬우므로 재활성화 조건을 반드시 함께 적는다.**

⭐ **네 번째가 앞의 셋과 다른 점** — 앞의 셋은 재현이 안 되거나(창을 못 만든다) 도구의 구조적
한계이거나(도구가 못 본다) 상황 자체가 소멸한 것(전제가 사라졌다)인데, 네 번째는 **재현은 쉽고
결함을 넣으면 확실히 갈리는데 그 값을 읽어 판정하는 assert 가 하네스에 아직 없는 것**이다 —
하네스를 만들면 바로 닫힌다.

| 못 덮는 것 | 사유 | 실측 근거 |
|---|---|---|
| `session.ps1 #28` — **실물 드레인 중 Unregister 유예 창이 실제로 발동하는가** | **창이 좁아 못 만든다.** 이 시나리오의 `server.stop()` 드레인이 **3~4ms**(두 회차 측정 3ms·4ms)라 ms 해상도 타이머·스케줄링 지터보다 좁다 | 유예 로직 자체는 `#26`(가짜 연결·결정적)이 덮는다. 못 덮는 것은 **「실물 드레인이 그 분기를 타는가」라는 타이밍뿐**이다 |
| `EntryTable::mutex_` 의 **필요성** ⚠️ **성격이 바뀌었다** | ⛔ **도구가 그 결함 종류를 못 본다**는 사유 자체는 그대로다(ASan 은 데이터 레이스를 안 본다 — TSan 부재). 다만 **옛 「어느 항목도 안 죽었다」는 증거가 이제 약해졌다** — 그 시절엔 `P6-6k`(아래 참조)가 실증했듯 40 연결이 **존 스레드 0 하나로 FIFO 직렬 처리**돼 `mutex_` 를 다투는 스레드가 사실상 하나뿐이었다. **지금은 워커 8개가 명부를 실제로 동시에 두드리는 구조**라 「진짜 다중 접근」이 이번에 처음 생겼다 | 옛 「락 7곳 제거·3회 재현·안 죽음」 실측은 **그 전제(단일 접근자)가 성립하던 시절의 값**이라 지금 그대로 재인용하면 안 된다. **그 재검증은 하지 않았다** — 뮤텍스 전체 제거 재실험은 TSan 이 여전히 없어 결론이 안 나고, 8단계에서도 수행하지 않았다(미검증 전제로 남긴다) |
| ~~`session.ps1 P6-H2a/H2b`~~ · ~~`trade.ps1 DupTest`~~ | ✅ **닫혔다** — SKIP 3건의 재활성 조건(동기 Kick 도입)이 충족됐다. H2a/H2b 의 원형(무인증 직행 로그인)은 영구 소멸이라 **문자 그대로가 아니라 K 계열(정당한 두 번째 Login 의 소유권 충돌)로 대체**됐고, DupTest 는 「거래 중 Kick → 정리 4/4 → 총량 보존」 실판정이 됐다 | `session` 88(SKIP 0) · `trade` 25(SKIP 0) 이 그 시점 정상값(session 은 재연결 후속부터 **95** — 위 하네스 표가 정본. ⚠️ 그 뒤 게이트도 같은 항목 수를 그대로 기대값으로 유지했다) |
| `s2s.ps1 ⑦` — **등록됐지만 미구현인 요청 → Unsupported 폴백** | ⛔ **전제가 사라져 재현 불가능해졌다.** 분류표의 세 요청(Reserve·Kick·SetMode)이 전부 구현돼 와이어로 그 폴백에 닿을 살아있는 id 가 없다 | 폴백 코드는 **§8-4 순차 패치 대비 방어로 유지**된다. `28 통과 + SKIP 1(총 29)` 이 정상값. **재활성화 조건: 새 세션→마을 요청 id 가 `is_request_from_session` 에 등록되고 핸들러가 아직 없는 시기** |
| `close_by_id` 의 **already_closing 선점 갈래 발화** | **결정적 재현 불가** — closing 창이 정리 타이밍 의존이라 s2s ⑱(이중 Kick 배치)은 자극+무해 관측까지만 한다(이번 실측 응답 0,1 = NotFound 경로). ⚠️ **정정** — 「존 정리 타이밍」이던 원문은 이제 **직렬 큐 Job 정리 타이밍**으로 읽는다(zone 소멸 — 정리 Job 이 도는 큐가 바뀌었을 뿐 타이밍 의존이라는 성질은 그대로) | 갈래 자체는 코드 정독 + ASan 회차로 검증. 홀드 미상승·미반납 짝은 CODING_RULES §4-b 표 |
| `entry.leave` 와 `notify_player_leave` 제출 사이 창(증분↔증분 순서 비원자) | ✅ **닫혔다(D3 — 이월 1 흡수)** — enter/leave 성공과 notifier 제출이 이제 `mutex_` 같은 임계구역 안에서 원자로 묶인다(`entry_table.cpp:75-76`·`:101-103`). 이 행 자체는 소멸 — 대체 항목은 아래 「notifier 원자화」 행 | 정적 사실은 코드 정독으로 확인. 동적 검증은 아래 행 참조 |
| `EntryTable::leave` 의 **소유자 대조 갈래(②)** — 늦은 leave 가 새 주인의 항목을 지우는 것을 막는 방어 | **순서 역전을 하네스로 강제할 수 없다** — 재입장 성공(K4·K5) 자체가 옛 세션의 leave 완료를 함의하는 시나리오 구조라서다. 대조 무력화 뮤턴트가 session 88 전건을 통과했다(실측) | 방어는 유지·근거는 코드 정독. 재현하려면 직렬 큐 Job 지연 주입 같은 새 기구가 필요하다(⚠️ 옛 「존 Job」은 직렬 큐 Job 으로 대체됐다) |
| ⭐ **신설 — `move_zone` 완전 이전 단일 호출 지점 규칙** | ⛔ **검사기(`lock_rank.h`) 범위 밖이다** — lock_rank 는 락 획득 순서 역전만 잡지, 다단계 인덱스 갱신의 **중간 가시성**(두 스레드에 걸친 갱신 창)은 애초에 그 검사기의 대상이 아니다. 방어는 「호출 지점을 하나로 고정한다」는 **규칙 자체**뿐이고, 어긴 구현은 리뷰가 반려한다 — 기계적 게이트가 없다 | 실제 호출부가 `frame_router.cpp:470`(`handle_join_zone` — 목적지 존 Job 안) **단 한 곳**임을 코드 정독으로 확인. 두 지점으로 나누면(원존 CAS 직후 + 목적지 Job) 「인덱스만 먼저 새 존」 창이 생겨 §7-3 JoinZone 시퀀스가 막으려는 순서 역전이 재현된다 — 그 형태를 하네스가 결정적으로 못 잡는다는 뜻이다 |
| ⭐ **신설 — notifier 원자화(D3) 의 동적 검증** | 정적 사실(임계구역 안 호출)은 코드 정독으로 확인되지만, **동적 증상**(원자화가 깨졌을 때 실제로 무엇이 관측되는가)은 인위 지연 주입 없이 결정적으로 재현할 수 없다 — 생존 원인은 관찰 창의 협소함이지 결함 부재가 아니다 | M5 뮤턴트(notifier 호출을 락 밖으로 이동)를 `session.ps1` FullSync 계열로 3회 주입했으나 **3회 다 생존**(안 죽음) — `entry_table.cpp:75-76`·`:101-103` 이 지금 임계구역 안에서 호출한다는 것 자체는 코드 정독으로 확실하다 |
| ⭐ **신설 — §7-3-A 재검증**(trade 핸들러의 `a.trade.load()==t` 재검증 지점) | **뮤턴트 재현 불가로 판정** — M3(재검증 코드 제거 주입)을 넣고 돌려도 `trade.ps1 -All` 이 여전히 **25/25 그대로 통과**했다(결함이 있는데 하네스가 못 잡는다는 뜻) | 유지 근거는 **코드 정독뿐**이다. ⛔ 「정상 통과만으로 폐합 금지」(선례가 있다) — 재검증 코드 자체는 지우지 않는다. §7-8 위험 6(빠뜨리기 쉬운 지점)의 실증이 이것이다 |
| ⭐ **신설 — `move_zone` 의 `had_prev`/`prev_zone` 오분류 방향**(`in_zone` 판정이 실제 상태와 어긋나는 쪽) | 순서 역전과 달리 **이 오분류를 강제 재현할 뮤턴트가 없다**(8단계 뮤턴트 목록 M1~M5 어디에도 없음 — 미주입) | 방어는 `in_zone` 플래그 자체(`entry_table.cpp:119-123` — 조건부 `erase_from_bucket`)뿐. 재현하려면 `in_zone` 갱신 누락을 인위로 주입하는 새 뮤턴트가 필요하다 — 이번 8단계 범위 밖으로 남긴다 |
| ⭐ **신설 — DB 장애 중 동작**(MySQL 다운 상태에서 로그인·채팅 정상 / 인벤·거래 `kDbError`) | ✅ **2026-09-02 수동 실측으로 닫혔다**(코드 변경 없음). MySQL84 서비스를 내린 채 village(Release) 를 기동해 4B 프레임 클라로 실단말 왕복: 로그인(kEnterReq→OK)·채팅(kChatReq→상대 kChatNtf 수신, 텍스트 일치) 정상 / 인벤토리(kInventoryReq)=`result=1 kDbError`(body `01 00 00`) · 거래 커밋(양쪽 kTradeConfirmReq)=`A·B 모두 result=1 kDbError`. ⛔ **`kBusy`(4) 로 뭉개지지 않았다 — 최종 리뷰에서 회귀 없음 확인.** MySQL84 재시작 후 **서버 재기동 없이** 인벤이 `result=0` 으로 회복(1~2 왕복 내). 종료 로그 `[POOL2] open_failed=2 timeouts=0`(try_failed 미출력=0) | 실측 절차: MySQL84 `Stop-Service`→village.exe 기동(예약 경유 접속)→kInventoryReq·거래 커밋 응답 코드 수집→MySQL84 `Start-Service`→인벤 재시도→서버 정상 종료로 `[POOL2]` 수집. 서버 stdout 로그가 `inventory query failed — no connection`·`[DBACK] rows=0`·`[POOL2] open_failed=2` 로 코드 경로(`try_acquire(bool* open_failed)`)를 확증. ⚠️ **관측 1건 — 인과 정정**: DB 경로 응답이 요청 ~2s 뒤에 나갔다(결과 코드는 kDbError 로 정확). 그 지연을 `acquire(timeout)` 잔존 대기와 연관지었던 이전 서술은 **오진이었다**. 반증 ① 그 로그 문구(`db 연결을 못 빌렸다(풀 대기 100ms 초과)`) 자체가 코드에 없다 — `find_copies.ps1 -Value '풀 대기'` 전수 5건이 전부 문서이고 `src` 매치 0건, `grep -rn "못 빌\|연결을 못\|no connection" src/` 로도 그 형태의 로그가 없다(실제로 찍히는 것은 `frame_router.cpp:1109` 의 `"no connection"`) ② 100ms 대기가 일어날 경로 자체가 없다 — `acquire(timeout)` 호출부가 0건이라 `cv_.wait_until` 에 진입할 코드가 없다 ③ 관측 시점 바이너리도 지금과 같다 — `db_pool.h`·`db_worker.cpp` 의 마지막 변경은 5단계 재구성이고 2026-09-02 관측 이후 이 두 파일 변경 0건(`git log` 확인) — 즉 **「현재 코드에 없다」뿐 아니라 「그때도 없었다」**다. 실제 원인은 `mysql_real_connect` 의 3초 연결 타임아웃이다 — `try_acquire` 의 신규 생성 갈래가 락을 풀고(`db_pool.h:120` `lock.unlock()`) `c->open(cfg_)` 를 부르며(`db_pool.h:123`), 그 대기가 곧 응답 지연이다(코드 주석이 이미 그 값을 적고 있다 — `db_pool.h:118` "mysql_real_connect 는 타임아웃이 3초다") |
| ⭐ **신설 — 직렬화 1회(`build_frame`) 회귀** | ⛔ **도구가 못 본다** — 관측 가능한 와이어가 최적화 전후로 완전히 같다(헤더+본문 바이트가 그대로 나간다). `build_frame` 을 걷어내고 대상마다 재직렬화해도 프레임 내용은 안 바뀌어 하네스로 회귀를 못 잡는다 | 유지 근거는 코드 정독뿐 — 헤더 인코딩이 `proto::encode_header` 한 곳(§17-2 ①)이라는 사실만 직접 확인했다. §7 불변식 9(정정판) 참조 |
| ⭐ **신설 — 빈 body 방어**(`body_len < 1`, whisper 이전 공통 검증) | ⛔ **생존은 「1회 관측」일 뿐 결함 부재의 증거가 아니다** — `chat.ps1` 이 관측한 것은 스테일 바이트가 우연히 `type=0`(Zone)으로 읽혀 존 미가입 「조용히 버림」 갈래로 흡수된 경로 하나뿐이다. 그 바이트가 `type=1`(All)로 읽혔다면 `text_len = body_len - 1` 이 음수가 돼 `build_frame` 의 합산·복사가 어긋나는 힙 오버런 경로였다 — **11단계 A 반영(합산·복사 조건 통일)으로 지금은 그 경로 자체가 원천 차단됐다**(그 반영이 없었다면 이 방어의 진짜 값어치는 type=0 흡수가 아니라 이 오버런 차단이었다) | 뮤턴트 실측(R4 — 방어를 `false` 로 무력화) — `chat.ps1` C7(빈 body 절단 기대)이 생존했다(관측된 것은 type=0 갈래 1회뿐). 부수 관측: 생존한 클라가 이후 C9 격리 목록 밖에 남아 `send_full_kicked` 수치가 1 늘었다(같은 회차 실측) |
| ⭐ **신설 — `SendBuffer::overflow_streak` 리셋**(성공 큐잉 갈래) | ⛔ **창을 못 만든다** — 정상 트래픽은 streak 가 0 을 넘을 일이 없고, 한 번 넘치기 시작한 세션(victim)은 임계까지 연속으로만 넘치다 끊겨서 「넘친 뒤 다시 성공해 리셋이 실제로 도는」 왕복 자체가 이 하네스 시나리오에 없다 | 뮤턴트 실측(R5 — 리셋 문장 제거) — `chat.ps1` 48/48 전건 생존. 유지 근거는 "연속" 계약이라는 코드 정독뿐이다 |
| ⭐ **신설 — overflow 킥의 `==` 이중-가산 방어**(`streak == send_overflow_limit_`, `>=` 아님) | ⛔ **동치가 아니라 미검증이다** — `chat.ps1` 은 단일 blaster 구조라 `closing=true` 가 임계 도달 즉시 후속 `send_chunks` 재진입을 막아 `==`↔`>=` 차이가 관측되지 않는다(8단계에서 이 뮤턴트를 동치로 보고 미주입한 근거이기도 하다). 그런데 이 방어가 실제로 막는 것은 **다중 송신자가 같은 세션에 동시에 큐를 밀어 넣는 경합**이고, 그 경합을 만드는 하네스가 없다 — "동치라 안전하다"와 "구분할 시나리오가 없어 모른다"는 다른 결론이다 | `chat.ps1` C9 는 blaster 1명뿐이라 이 축을 만들지 않는다 — 다중 송신자 경합 하네스가 생기기 전까지는 코드 정독(§17-6 주석)이 유일한 근거다 |
| ⭐ **신설 — `try_acquire` open 실패 롤백의 `--acquired_` 보정** | ⛔ **미배선이다** — 값을 거꾸로 넣거나 빼도 빌드·회귀 14종(당시 — 뒤에 늘어난 `client.ps1` 도 `acquired` 를 읽지 않는다)이 전건 통과한다. `[POOL2] acquired=` 를 판정에 쓰는 코드가 저장소에 0건이기 때문이다(`grep -rn "acquired" scripts/` → `dbload.ps1:9` 주석뿐). 앞의 세 사유와 달리 **재현 자체는 쉽다** — DB `open` 을 실패시키면 그 자리서 확실히 갈리는 값인데, 그 값을 읽는 assert 가 하네스에 없을 뿐이다 | 검증하려면 스크래치 config 의 `[db] host`/`port` 를 무효값으로 주어 `open` 실패를 유도하고(자체 스폰 갈래와 같은 패턴 — MySQL 서비스 제어는 불필요하다), 종료 로그의 `acquired`/`open_failed` 항등식을 assert 하면 된다. 지금 만들지 않은 이유는 §18-7 — `acquired` 는 풀 크기 판단의 정본이 아니고(`server.ini` 주석이 그 답을 `try_failed` 로 지목한다) `try_failed` 는 이미 `zone_block.ps1` 이 검증한다. 비용은 `mysql_real_connect` 3초 대기 × N회의 실행 시간 증가다 |
| ⭐ **신설(T015) — 클라 `send` 의 「송신 도중 실패」 분기**(`FAIL send` — `send_all` 이 `WSAECONNRESET`/`WSAECONNABORTED` 외 오류로 실패) | ⛔ **창을 못 만든다** — loopback 에서 `send()` 가 실패하려면 상대가 그 순간 끊어야 하는데, 그 조합은 `--expect-close` 판정 직행(C5 해법 a)으로 흡수되고 나머지 오류 코드는 재현 수단이 없다 | 8단계 뮤턴트 17종(주입분) 어느 것도 이 분기를 타지 않았다 — 유지 근거는 코드 정독(`cmd_send.cpp` `last_send_error`) |
| ⭐ **신설(T015) — 클라 `send` 경로의 `protocol_error`**(응답 헤더 `body_size > 4096`) | ⛔ **전제가 없다** — 서버가 4096 을 넘는 응답을 내지 않으므로(불변식 6) 실서버 앞에서는 영구히 안 밟힌다. 대신 `client selftest` 의 `parse_frames_oversized` 항목이 같은 함수를 직접 검증한다(S0) | MUT2(`parse_frames` 마지막 프레임 누락)가 S0·S1·S4a 를 죽인 것으로 파서 판정력은 실증됐다 — 다만 그 항목은 오버사이즈 분기가 아니라 정상 분할을 죽인 것이다 |
| ⭐ **신설(T015) — 클라 `recv_exact` 의 「부분 수신 반복 시 마감 갱신」 결함** | ⛔ **창을 못 만든다** — S3f(`timeout at enter`) 는 세션 서버가 `kEnterReq` 를 조용히 버려 **완전 침묵**이라 루프가 1회로 끝난다(S3f 경과 1518ms ≤ 3000ms 단언은 「누적 마감이 배수로 늘지 않는다」의 1회 케이스만 본다). 부분 수신을 강제하려면 헤더만 보내고 멈추는 서버가 필요하다 | 유지 근거는 코드 정독(`tcp_client.cpp` `recv_exact` 의 마감 계산) |
| ⭐ **신설(T015) — 클라 `flow` 의 「단계 통째 생략」 뮤턴트**(예: Echo 단계를 건너뛰고 Ping 으로) | ⛔ **도구가 못 본다** — 판정 술어(`expect_frame`·`bytes_equal`)를 우회하는 형태라 selftest(술어 자체를 검증)도 하네스(출력 줄 대조)도 잡지 못한다 — 생략된 단계의 줄이 안 찍히는 것을 S2 의 「줄마다 1건」이 잡는 범위까지만 | S2-login/enter/echo/pong 4건은 정상 경로 판정이라 8단계 뮤턴트 대상이 아니었다(의도된 생존) — 단계 생략은 코드 리뷰 축 |
| ⭐ **신설(T015) — 클라 `flow` 의 login 단계 타임아웃 · 암묵 `login result≠0` FAIL 분기** | ⛔ **창을 못 만든다** — 두 서버가 로그인 요청에 즉시 응답하거나(세션) 즉시 끊는다(마을 게이트). `result≠0`(예: `NoServer`)은 마을이 등록되기 전에만 나는데 하네스는 등록 뒤에 시나리오를 시작한다 | S3a(끊김)·S14(`--expect-login-result` 불일치)만 실행된다 — 타임아웃·암묵 nonzero 갈래는 코드 정독 |
| ⭐ **신설(T015) — 클라 `WSACleanup` 정확히 1회** | ⛔ **기계 게이트가 없다** — `WinsockScope` RAII(`tcp_client.h`) 하나가 `main` 에서만 생성되는 구조지만 그것을 세는 하네스·검사기가 없다 | 7단계 코드 정독(`main.cpp` · `tcp_client.h` · `tcp_client.cpp` 의 `WSAStartup`/`WSACleanup` 짝)으로 확인 — 회귀는 없다 |
| ⭐ **신설(T015) — raw `--repeat>1` 의 「송신 도중 절단」 분기**(`peer closed during send`) | ⛔ **결정적 재현 불가** — 서버가 첫 4B 를 헤더로 읽어 끊는 시점과 클라의 2·3번째 `send_all` 이 loopback 에서 경합한다. S8(`--repeat 3 --size 4 --expect-close`) 20회 연속 실측은 **전부 수신 단계 절단**이었고, 8단계 MUT18 회차(recv 의 `kClosed` 분기 무력화)에서 **처음으로 송신 단계 절단(`WSAGetLastError=10053`)이 1회 관측**돼 두 경로가 모두 실재함은 확인됐다 — 어느 쪽을 타는지는 여전히 비결정적이라 S8 은 두 경로 중 하나만 죽인 뮤턴트에 생존할 수 있다(MUT18 에서 S3e 는 죽고 S8 은 생존) | 분기 자체는 C5(7단계 correctness HIGH — `send_all` 이 `last_send_error` 를 남기고 `expect_close` 면 `closed_seen` 으로 판정 직행)로 존재하고 코드 정독 + 위 1회 관측으로 확인 |
| ⭐ **신설(T015) — `client.ps1` 자체의 종료 코드 규약**(「실패 건수 = exit」) | ⚠️ **한 번 깨졌었다** — PowerShell 5.1 은 `Where-Object` 결과가 정확히 1개면 배열이 아닌 객체 하나를 돌려주고 `[pscustomobject]` 의 `.Count` 는 `$null` 이라, **실패가 정확히 1건일 때만** `exit $null`=0 이 됐다(콘솔 요약은 정확). 8단계 MUT1 회차(S0 1건 실패 · exit 0)가 드러냈고 `@(…).Count` 로 고쳤다 — 이후 1건 실패 회차 8종(MUT4·6·7·8·9·10·11·12)이 전부 exit 1 | 하네스 자체를 검증하는 하네스는 없다 — 같은 형태가 `inventory.ps1:248,266,267` 에도 있으나 그쪽은 정수 스칼라라 `.Count` 가 1 을 돌려줘 결함이 아니다(기록만) |
| ⭐ **신설(T015) — 클라 `bytes_equal` 의 `n==0` 분기**(`memcmp` 회피 갈래) | ⛔ **미배선이다** — `selftest` 의 `bytes_equal` 항목은 8B 만 시험하고, `flow` 의 `--echo-size` 는 35판정 전부 기본값 8 이라 그 분기가 한 번도 실행되지 않는다. 재현은 쉽다(`--echo-size 0` 또는 selftest 항목 1개) — 11단계 test 렌즈 LOW | 다음 조각에서 selftest 항목을 추가한다(12 → 13 이면 ADR-029·DESIGN §12·ARCHITECTURE §8·README·이 표 15행의 「12항목」 전파가 따라온다 — 그 비용 때문에 이번 커밋에는 넣지 않았다) |
| ⭐ **신설(T015) — 클라 `close_rst` 의 RST 의미론**(`SO_LINGER{1,0}` + `closesocket` 이 실제로 RST 를 내는가) | ⛔ **도구가 못 본다** — S9(`--drop-after-send`)는 `drop  : RST 로 즉시 종료` 출력 줄과 exit 0 만 보고, 소켓 레벨(RST vs FIN)은 관측하지 않는다. 뮤턴트 MUT20(`close_rst()` → `close_graceful()` 바꿔치기)이 **35/35 그대로 생존**했다(8단계 실측 — 11단계 test 렌즈 LOW 제안분) | 유지 근거는 코드 정독(`tcp_client.cpp` `close_rst` 의 MS Learn 인용)뿐. 소켓 레벨 관측(서버 쪽 `WSAECONNRESET` 수신 로그 또는 패킷 캡처)이 생기기 전까지 S9 는 「경로 실행 커버리지」다 |

⚠️ **`P6-6k`(옛 「존 스레드 내부 40 연결 경합」)는 완전히 역사적 기록이 됐다** — 원문은
「40 연결이 전부 존 스레드 0 으로 FIFO 순차 처리돼 경합이 아니었다」(`iocp_server.cpp:704-705` →
`bootstrap.cpp:352` `zones.post(placement_zone(placement), ...)` → `session.h:177` `placement{0}`)였는데,
**`placement.h`·`zone_manager.{h,cpp}` 가 삭제돼 이 경로 자체가 더 이상 존재하지
않는다.** ⛔ 이 기록이 남아 있는 이유는 「한때 `EntryTable::mutex_` 가 왜 미검증 상태였는가」의
근거이기 때문이다(바로 위 행 참조) — 지금 이 코드를 찾으려 하면 헛수고다.

---

## 3. 판정 지표 — 종료 로그

정상 종료(Enter 또는 `--seconds` 만료) 시 남는 요약이 판정 근거다.

| 줄 | 봐야 할 것 | 이상 신호 |
|---|---|---|
| `[ALLOC]` | **기준선 대비 추세** (절대값이 아니다) | 아래 ⛔ 참조 |
| `[POOL ]` | `failed` = 0 | 0이 아니면 **프레임 풀**(`bootstrap.cpp` `frame_pool_stats`)이 작다 — ⚠️ **DB 커넥션 풀이 아니다**(그건 아래 `[POOL2]`) |
| `[POOL2]` ⭐ **신설 등재**(`db_worker.cpp` — `try_acquire` 도입과 함께 생긴 DB 커넥션 풀 전용 요약) | `db conns` 줄의 `try_failed` | **`try_failed`** = `DbPool::try_acquire()` 가 그 자리서 즉시 실패한 횟수(`db_pool.h:142` `++try_failed_`) — §7-4 「락보다 먼저 빌리고, 못 빌리면 락 전에 `kBusy`」의 직접 증거이자 풀 부족 판단의 정본. **> 0 이 곧 결함은 아니다** — `zone_block.ps1` 판정3 처럼 「풀이 실제로 꽉 찼다」를 증명하는 용도로도 쓰인다. 줄에 남는 항목은 `peak`·`acquired`·`open_failed`·`discarded`·`try_failed` 다섯이다 |
| `[WORK ]` ⭐ **T014 신설**(`worker_pool.cpp` `stop()` — 직렬 큐 드레인 통계) | `drains`·`jobs`·`cap_hits`·`resubmits`·`batch`·`workers` | 회귀 판정 지표가 아니라 **측정 유효성 지표**다 — `drain_batch.ps1` 은 `cap_hits>0` 이어야 「상한이 실제로 걸린 회차」로 채택한다. `resubmits` 는 배치 뒤 잔량이 있어 큐 뒤에 다시 선 횟수(`cap_hits` 와 다를 수 있다 — 배치 도중 새 프레임이 와도 재제출된다). 종료 직전 `stop()` 경쟁으로 `resubmits` 가 1 과대일 수 있다 |
| `[CONN ]` | `peak` 과 `rejected` 의 관계 | peak 이 상한에 안 붙었는데 rejected 가 나면 **세션이 안 지워지고 있다** |
| `[NET  ]` | `idle_kicked` | peak 대비 비율이 뛰면 임계가 짧거나 클라 ping 이 안 나간다 |
| `[TICK ]` | `behind` 비율 | 틱이 밀리고 있다(⚠️ **정정** — 이 줄을 찍는 주체가 존 스레드에서 **전용 틱 스레드**(`app::worker_pool.cpp` — Step 4-f)로 바뀌었다. 의미·판정은 그대로) |
| `[TICK2]` | `interval` avg/p99/max · `jitter` | **평균은 「밀렸나」에, 지터는 「고른가」에 답한다. 다른 질문이다**(⚠️ 소유 스레드는 위 `[TICK ]` 과 동일하게 이전됐다) |
| `[WARN ]`/`[ERROR]` 로그 유실 | 0 | 0이 아니면 로그 설정을 고칠 신호 |

### ⛔ `[ALLOC]` 로 누수를 판정하지 마라 — **실측으로 확인한 것**

`allocs` 와 `frees` 는 **일치하지 않는 것이 정상이다.**

| | allocs | frees | 차이 |
|---|---|---|---|
| 기준선 `8f546dc` (room+send) | 2243 | 2025 | **218** |
| 같은 시나리오 | 2252 | 2149 | **103** |

`alloc_counter.h` 가 스스로 밝히듯 이 값은 **「프레임당 할당 횟수」를 보는 규모 지표**다 —
*"「여기서 할당이 날 것 같다」는 추측을 숫자로 바꾸는 용도"*.
`alloc_reset()` 이 기동 직후에 불리므로 **종료 시점에 아직 살아 있는 객체**(로거 큐 · 통계 · 스레드 로컬)가
그대로 차이로 남는다. 그 차이는 시나리오·타이밍마다 흔들린다.

**그래서 판정은 이렇게 한다**

| 무엇을 | 어떻게 |
|---|---|
| **누수** | ⛔ **`churn.ps1` 로 판정한다** — `delta: private=0 (0 B/session)` 이 기준이다 |
| **할당 규모** | `[ALLOC]` 을 **기준선과 같은 시나리오로 대조**한다. 절대값이 아니라 추세다 |
| ⚠️ ASan 구성 | `alloc_counting()` 이 **false** 다. 그때 `[ALLOC]` 숫자는 **믿으면 안 된다** |

**기능 추가 전후로 `[TICK ]` `[TICK2]` 를 비교한다.** 틱 예산을 먹었는지가 여기서만 보인다.

---

## 4. ASan (세션 수명·버퍼를 건드렸으면 필수)

⛔ **`run-asan.ps1` 은 「ASan 검증을 대신 해 주는」 스크립트가 아니다.**
**ASan 구성으로 서버를 띄우는** 스크립트다 — 하네스는 §2처럼 **별도 창에서 직접 돌려야 한다.**

```powershell
.\scripts\run-asan.ps1 -Build -Seconds 60    # ASan 빌드 + 기동, 60초 뒤 스스로 종료
# 다른 창에서 §2의 하네스를 그대로 실행
```

**이 스크립트가 있는 이유**: ASan 빌드는 `clang_rt.asan_dynamic-x86_64.dll` 에 의존하는데
그 DLL 이 MSVC 도구 폴더에만 있고 PATH 에 없다. 그냥 `village.exe` 를 실행하면
**「DLL 을 찾을 수 없습니다」로 죽는다.** 이 스크립트가 DLL 폴더를 PATH 앞에 붙여 준다.

### ⛔⛔ `-Seconds` 로 끝내라 — 강제 종료하면 리포트가 안 나온다

`Stop-Process` 로 죽이면 `stop()` 이 안 돌아 **종료 경로가 검증에서 빠지고, ASan 리포트도 안 나온다.**
그러면 「깨끗하게 통과했다」와 「검사 자체가 안 돌았다」가 **구분되지 않는다.**

| 잡는 것 | 전형적 원인 |
|---|---|
| use-after-free | `release_session` 을 두 번 했다 / 홀드 없이 포인터를 태웠다 |
| heap-buffer-overflow | body 길이 검증 누락 · 오프셋 실수 |

크래시가 나면 `dumps\` 에 미니덤프가 떨어진다 (`ops/crash_dump`).
덤프는 **파일 크기만 보지 말고** 내용을 확인한다 — 「파일은 있는데 열리지 않는」 상태가 제일 나쁘다.

```powershell
python scripts\dump_info.py dumps\<파일>.dmp
```

⛔ **세션 수명을 건드린 변경은 ASan 없이 「검증됐다」고 말할 수 없다.**

### ⭐ 자체 스폰 하네스도 이제 exit-time 검사를 받는다 (정상 종료를 복원했다)

⚠️ **한때는 그렇지 않았다** — 그때는 `harness_common.ps1` 의 `Stop-Harness()` 가 서버를
**`$Proc.Kill()`**(TerminateProcess)로만 끝냈고, `report_shutdown_stats`(`src/main.cpp:240`)는
**정상 종료 경로에서만** 불리므로 종료 지표도 exit-time 누수 리포트도 못 받았다(누수 판정은
`churn.ps1`·`run-asan.ps1 -Seconds N` 둘로만 제한됐다).

**이걸 되돌렸다** — `Stop-Harness()` 를 Win32 `CreateProcess` 직결 + stdin
파이프로 다시 짰다: 종료는 **개행(`WriteLine('')→Flush→Close→WaitForExit(5000)`)이 먼저**고,
5초 안에 안 죽으면 그때만 `Kill()` 폴백이다(U1 실증). 정상 종료 경로가
다시 돌므로 `report_shutdown_stats` 가 불리고, **exit-time 검사도 다시 실린다**:

| 예전에도 얻던 것 | **다시 얻는 것** |
|---|---|
| ⭕ 런타임 오류(use-after-free·heap-buffer-overflow) — 발생 즉시 리포트되므로 그대로 잡혔다 | ⭕ **종료 지표** `[POOL ] [CONN ] [NET  ] [TICK ]`(`[ALLOC]` 은 ASan 자체 설계로 여전히 안 찍힌다 — `alloc_counter.cpp:7-9`, §3 참조) |
| ⭕ 시나리오 자체의 판정 | ⭕ **ASan 의 exit-time 누수 리포트** |

⛔ **실측한 것과 이론상 되는 것을 가른다.** `-Config ASan` 으로 직접 재확인한 것은
`session.ps1`·`s2s.ps1`(원래도 정상 종료 경로였다)과 **새로 `drain.ps1`·`idle.ps1`** 넷뿐이다 —
D9/I4 가 종료 지표 4종을 전부 찍었고 ASan 리포트 0건이었다(그 4종의 존재 자체가 정상 종료 경로가
실제로 돌았다는 증거 — 안 돌았으면 지표 자체가 안 찍힌다). `zone`·`members`·`inventory`·`zone_block`·
`trade`·`zone_race`·`gate` 도 같은 `Stop-Harness()` 를 공유하므로 **이론상은** 같은 경로를 타지만,
그 넷을 `-Config ASan` 으로 재실행해 확인하지는 않았다 — 재는 사람이 값으로 확인하기 전에는
「된다」로 단정하지 않는다.

⇒ **누수 판정 수단이 늘었다** — `churn.ps1`(Debug·Release, ASan 은 자체 거부) · `run-asan.ps1 -Seconds N`
자연 만료 · **`-Config ASan` 정상 종료 회차의 exit-time 리포트**(위에서 실측 확인된 네 하네스는 즉시 유효,
나머지는 재실행해 확인한 뒤 유효).

---

## 5. 결함 주입으로 「하네스가 진짜 잡는가」를 증명한다

새 하네스를 짰거나, 기존 하네스가 새 결함을 잡는다고 **주장**한다면 — 실행으로 증명한다.

이 저장소에는 그 수단이 **이미 있다.** 부하 주입 스위치가 곧 뮤턴트다.

| 스위치 | 주입하는 결함 | 잡아야 할 하네스 |
|---|---|---|
| `kBadTradeNoTx` | 거래를 트랜잭션 없이 수행 | `trade.ps1 -All` (총량 보존 실패) |
| `kBadSyncDbMs` ⚠️ **재정의** | 예전 「존 스레드에서 동기 DB 대기」는 존 스레드 소멸로 의미 소멸 — 지금은 `handle_inventory` 가 `try_acquire` 를 부르기도 전에 `Sleep` 하고 즉시 응답하는 **워커 점유 주입**(§0 참조) | `zone_block.ps1`(재정의) **판정1**(비차단) — 워커 하나가 그만큼 묶여도 다른 세션(다른 워커)의 에코 왕복이 안 밀리는지를 이 스위치로 깨서 확인한다 |
| `kLogicDelayMs` | 로직 지연 | `zone_block.ps1` · `[TICK ]` |
| `kBadTickWorkMs` / `kBadTickSpike*` | 틱 지연 / 스파이크 | `[TICK2]` p99·max |
| ~~`kDedicatedConn`~~ | ⛔ **소멸** — `DbWorkerPool` 자체가 Step 4 에서 삭제돼 우회할 대상(전용 커넥션 경로)이 없다. 이 행은 재의 대상이 아니다 | — |

### 절차

| # | 행동 | 확인 |
|---|---|---|
| 1 | 스위치를 켠다 | `git diff -U0` 로 **주입 지점이 1곳인지** |
| 2 | 빌드 + 해당 하네스 실행 | **의도한 그 하네스만** 실패하는가 |
| 3 | 실패 **사유**를 본다 | 의도한 이유로 죽었는가 (다른 이유로 죽은 것이 아닌가) |
| 4 | 스위치를 **원복** | |
| 5 | ⛔ `git status` 로 **원복을 확인** | 주입이 커밋에 섞이지 않았나 |

⛔ **5번을 빠뜨리면 부하 주입 스위치가 켜진 채로 커밋된다.**
그러면 다음 사람의 회귀가 전부 거짓말을 한다.

> **하나도 안 죽었으면 하네스를 의심하기 전에 「주입이 실제로 반영됐는가」를 먼저 확인한다** —
> 빌드가 옛 산출물을 쓰고 있거나, 워크트리가 아닌 곳에 주입했을 수 있다.

### ⛔⛔ 판정 정본은 `logs\server.log` 다 — stdout 리다이렉트가 아니다 (실측)

`Start-Process -RedirectStandardOutput` 으로 받은 파일은 **버퍼링으로 잘린다.**
실측: stdout **4145B(줄 중간에서 절단)** vs `logs\server.log` **5539B**.
⛔ `[WARN] inventory truncated 512 -> 511` 이 **stdout 에는 없고** `logs\server.log` 에만 있었다 —
하마터면 「절단 로그가 안 남는다」는 **가짜 결함**을 보고할 뻔했다.
⚠️ `logs\server.log` 는 **기동마다 덮어쓴다** — 회차를 보존하려면 기동 전에 옮긴다.

### ⛔ `Stop-Process -Force` 로 죽이면 종료 지표가 **통째로 사라진다** (실측)

`[ALLOC]` · `[POOL ]` · `[CONN ]` · `[NET  ]` · `[TICK ]` 는 **정상 종료 경로에서만** 찍힌다.
⇒ 판정이 필요한 회차는 **`--seconds` 만료를 기다린다.** 상태 전환용 재기동에만 `-Force` 를 쓴다.

### ⚠️ `churn` 의 `handles` 델타는 **연속 3회**로 판정한다 (실측)

기동 직후 1회차에 `handles=+6` 이 나왔고 2·3회차는 **둘 다 0** 이었다.
⇒ **누적되지 않으면 누수가 아니다**(1회성 워밍업). 한 번 보고 판정하지 마라.

### ⛔ `trade.ps1` 의 **전건 통과는 DB 경로가 건강하다는 증거가 아니다** (실측)

⚠️ 아래 서술의 `19/19` 는 **옛 값**이다. `18 통과 + SKIP 1 (총 19)` 도 지나간 값이고,
**지금은 `DupTest` 가 「거래 중 동기 Kick」 시나리오로 재활성돼 `25 통과 (SKIP 0, 총 25)` 가 정상이다**
(§2 표와 정합). 논지(**DB 를 안 타는 항목이 대부분이다**)는 그대로다.

`REVOKE EXECUTE` 로 **DB 를 통째로 끊어도 17개가 통과한다.**
그것들은 프로토콜·상태머신 판정(`kTradeReqNtf` 수신 · confirm 지워짐 · 존 퇴장 취소 …)이라 DB 를 안 탄다.
⛔ **「총량 보존」조차 통과한다** — DB 가 안 붙어 아무것도 안 바뀌기 때문이다(**가짜 통과**).
⇒ DB 경로를 검증하려면 **실제로 수량이 옮겨졌는지**를 보는 2건을 보라.

### ⛔ DB 뮤턴트는 `git checkout` 으로 **안 사라진다**

저장 프로시저·권한을 건드린 주입은 **다른 세션·다음 작업까지 넘어간다.**
⛔ **주입 직후와 원복 직후 양쪽 다** 확인한다 — `SHOW CREATE PROCEDURE` · `SHOW GRANTS` · 실측 `CALL`.

---

## 6. 보고 형식

⛔ **실행하지 않은 하네스를 「통과 예상」으로 보고하지 않는다.**

```
## 빌드
Debug/Release/ASan — 경고 0

## 하네스
send.ps1      : 3000/3000 통과
zone.ps1      : 통과
trade.ps1     : 25/25 통과 (SKIP 0) · 총량 보존 확인
gate.ps1      : 11/11 통과
(inventory.ps1: 미실행 — DB 미기동. 사유 명시)

## 종료 로그
[ALLOC] allocs=N frees=M  (기준선 대비 추세 — 일치할 필요 없다)
churn.ps1: delta private=0  (누수 판정은 여기서)
[POOL ] failed=0
[TICK ] behind=0.0%
```
