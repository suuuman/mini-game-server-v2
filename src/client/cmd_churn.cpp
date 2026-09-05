//  client/cmd_churn.cpp — `client churn` (churn.ps1 이식, T016-impl.md §5)
//
//  접속 → (선택) 프레임 교환 → 종료를 --count 번 반복한다. 서버
//  메모리·핸들 통계(before/after, churn.ps1 의 Get-ServerStat)는 이 exe
//  가 아니라 scripts/churn.ps1 -Cxx 래퍼의 몫이다(ADR-029 결정 2 — 이
//  클라는 client → proto 하나만 의존하고 서버 프로세스를 관측할 방법이
//  없다). 그래서 --report 중간 보고 줄도 서버 통계 없이 "회차 : ok"
//  만 찍는다 — -Cxx 에서는 그 줄 대신 래퍼의 before/after 통계가 판정을
//  대신한다.
//
//  ⚠️ 첫 회차(i==1) 접속 실패만 kConnect(3)로 즉시 끝낸다 — 그 뒤의
//  실패(i>1)는 failed++ 로만 센다. churn.ps1:112-131 은 모든 회차의
//  실패(첫 회차 포함)를 하나의 catch { $failed++ } 로 센다 — 이 exe 는
//  "서버가 처음부터 없다"를 그것과 구분해 별도로 승격한다(send/flow 의
//  첫 connect 실패 관례를 churn 에도 맞춘다 — ADR-029 결정 4). 그래서
//  이 갈래(서버 없음)는 PS 와 동치가 아니다 — client.ps1 의 S22/S23
//  동치 확인은 "서버가 있는 정상 경로"에 한정된다(5단계 R1 아키텍트
//  LOW 지적, 정정 완료).
//
//  ⚠️ Echo(msg-id 기본값 1)는 로그인 게이트 앞 case 다(frame_router.cpp:
//  1245-1261 — kEchoReq/kPingReq/kEnterReq 는 예약 게이트보다 먼저
//  switch 가 가로챈다) — 그래서 예약 없이 접속만 해도 통과한다. --msg-id
//  를 게이트 뒤 값(예: 2·3·13)으로 주면 서버가 예약 없는 세션을 끊어
//  failed 가 오른다 — 이건 결함이 아니라 게이트가 의도대로 작동한
//  것이다(churn.ps1 도 같은 서버 앞에서 같은 값들에 대해 같다).

#include "commands.h"
#include "frame_codec.h"
#include "tcp_client.h"

#include "proto/packet.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace client {

	int run_churn(const Args& args) {
		// --host 는 churn.ps1 param 에 없는 신규 인자다(churn.ps1:114 는
		// '127.0.0.1' 하드코딩) — send/flow 의 --host 관례를 따를 뿐이고
		// loopback 전용이라는 점은 같다(T016-impl.md §5-1).
		const std::string host = args.get("host", "127.0.0.1");
		const int64_t port_raw = args.get_int("port", 9000, 1, 65535);
		const int64_t count = args.get_int("count", 1000, 1, 10000000);
		const int64_t size = args.get_int("size", 8, 1, 65535);
		const int64_t msg_id_raw = args.get_int("msg-id", 1, 0, 65535);
		const int64_t report = args.get_int("report", 100, 0, 10000000);
		const bool framed = args.has("framed");
		const bool no_exchange = args.has("no-exchange");

		if (!args.ok()) {
			print_usage();
			std::printf("%s\n", args.error().c_str());
			return static_cast<int>(ExitCode::kUsage);
		}
		const std::vector<std::string> unknown = args.unknown();
		if (!unknown.empty()) {
			print_usage();
			return static_cast<int>(ExitCode::kUsage);
		}

		const uint16_t port = static_cast<uint16_t>(port_raw);
		const proto::MsgId msg_id = static_cast<proto::MsgId>(msg_id_raw);

		// ── 유닛 조립 (churn.ps1:72-73 New-Frame / 'A'*Size 대응) ────────
		const std::vector<uint8_t> body = echo_pattern(static_cast<size_t>(size));
		const std::vector<uint8_t> unit = framed
			? build_frame(msg_id, body.data(), body.size())
			: body;

		std::printf("churn : %lld 회  exchange=%s  unit=%zuB\n",
			static_cast<long long>(count), ps_bool(!no_exchange), unit.size());

		std::vector<uint8_t> recv_buf(unit.size());
		const auto start = std::chrono::steady_clock::now();
		int64_t failed = 0;

		for (int64_t i = 1; i <= count; ++i) {
			bool iter_failed = false;

			TcpClient c;
			if (!c.connect(host, port)) {
				if (i == 1) {
					// 서버가 처음부터 없다 — 위 머리말 참조(PS 와 동치가
					// 아닌 갈래).
					result_line(false, "connect");
					return static_cast<int>(ExitCode::kConnect);
				}
				iter_failed = true;
			} else {
				if (!no_exchange) {
					bool ok = c.send_all(unit.data(), unit.size());
					if (ok) {
						// churn.ps1:118 의 $s.ReadTimeout = 2000 대응.
						ok = (c.recv_exact(recv_buf.data(), unit.size(), 2000) == RecvResult::kData);
					}
					if (!ok) {
						iter_failed = true;
					}
				}
				c.close_graceful();	// churn.ps1:127 의 $c.Close() 대응.
			}

			if (iter_failed) {
				++failed;
			}

			// report 는 성공/실패와 무관하게 매 회차 끝에 검사한다
			// (churn.ps1 의 try/catch 밖에 있는 것과 같은 위치).
			if (report > 0 && (i % report) == 0) {
				std::printf("%6lld : ok\n", static_cast<long long>(i));
			}
		}

		const double elapsed_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		std::printf("time  : %.1fs   실패 %lld 회\n", elapsed_s, static_cast<long long>(failed));

		// ASCII 요약 — 래퍼·client.ps1 이 읽는 줄(한글·기호 금지, §1).
		std::printf("churn : count=%lld failed=%lld\n", static_cast<long long>(count), static_cast<long long>(failed));

		const bool pass = (failed == 0);
		const std::string reason = "failed=" + std::to_string(failed);
		result_line(pass, pass ? nullptr : reason.c_str());
		return pass ? static_cast<int>(ExitCode::kPass) : static_cast<int>(ExitCode::kFail);
	}

}	// namespace client
