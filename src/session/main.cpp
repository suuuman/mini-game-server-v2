//  session/main.cpp — 세션 서버를 조립해서 띄운다.
//
//  마을 main.cpp 의 골격을 세션 전용으로 복제했다(ADR-020 결정 7 — 이 exe 는
//  app/world/db 를 모른다). 그쪽과 마찬가지로 여기 남은 것은 「순서가 곧 계약」인
//  코드뿐이다 — 선언 순서(= 소멸 역순) · 기동 순서 · 종료 순서.
//
//  종료 순서가 마을과 다른 곳: 두 서버 stop(클라 → S2S 순)이 router 정리보다
//  먼저다. 클라가 먼저 서야 하는 이유 — 클라 워커가 Registry 뮤텍스 아래에서
//  S2S 세션 포인터로 Reserve 를 보내는 단방향 의존이 있어, S2S 를 먼저 내리면
//  stop 의 세션 전량 삭제(io_count 무관)와 그 send 가 겹칠 수 있다. router
//  정리가 마지막인 이유 — net stop 이 session_gone 을 발화해 홀드 대부분을
//  회수한 뒤라야 잔존 감사가 뜻을 갖는다. 다만 그 발화는 전수 보장이 아니다 —
//  워커가 stop 의 가짜 완료를 만나면 즉시 이탈해 closesocket 의 실패 완료가
//  미처리로 남을 수 있다. 그래서 잔존은 오류 단정이 아니라 감사 기록이다.
//  마을 골격을 그대로 복제하면 이 순서에서 틀린다.
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mmsystem.h>   // timeBeginPeriod — windows.h 가 lean 구성이라 따로 온다

#include <cstdlib>      // strtol — atoi 가 아니다 (마을 bootstrap 의 근거와 같다)
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <iostream>     // std::cin, std::getline — 콘솔 명령 루프
#include <mutex>
#include <sstream>      // std::istringstream — 명령 토큰 분리
#include <string>
#include <thread>

#include "session/registry.h"
#include "session/session_router.h"
#include "net/iocp_server.h"
#include "core/config.h"
#include "core/log.h"
#include "ops/crash_dump.h"

namespace {

	// I/O-bound 워커는 GQCS 에서 대부분 자고 있어 코어 수에 매일 이유가 없다 —
	// 마을 bootstrap 의 kIoWorkerCount 와 같은 판단·같은 값이다.
	constexpr int kIoWorkerCount = 4;

	// S2S 수용 쪽 프레임 풀 축소 — 상대는 마을 수 대(레지스트리 정원 두 자릿수)라
	// 기본 4096개(×4100B ≈ 16.8MB)는 낭비다. 256개 ≈ 1MB.
	constexpr size_t kS2sFramePoolCapacity = 256;

	struct CliArgs {
		int auto_seconds = 0;    // --seconds N : N초 뒤 스스로 멈춘다 (하네스용)
		int crash_mode = 0;      // 0 없음 · 1 --crash · 2 --crash-throw · 3 --crash-terminate
	};

	CliArgs parse_args(int argc, char** argv) {
		CliArgs a;
		for (int i = 1; i < argc; ++i) {
			if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
				char* end = nullptr;
				const long v = std::strtol(argv[i + 1], &end, 10);
				if (end != argv[i + 1] && *end == '\0' && v > 0) {
					a.auto_seconds = static_cast<int>(v);
				}
				++i;
			} else if (std::strcmp(argv[i], "--crash") == 0) {
				a.crash_mode = 1;
			} else if (std::strcmp(argv[i], "--crash-throw") == 0) {
				a.crash_mode = 2;
			} else if (std::strcmp(argv[i], "--crash-terminate") == 0) {
				a.crash_mode = 3;
			}
		}
		return a;
	}

	// 마을 HiResTimer 의 복제(app 소속이라 include 불가). 타이머 해상도를 1ms 로
	// 올린다 — 하네스가 헬스 주기·스윕을 수백 ms 로 축소해 돌리는데, 기본 해상도
	// (15.6ms)의 sleep 오차가 그 스케일에서는 판정 오차가 된다.
	struct HiResGuard {
		HiResGuard() { timeBeginPeriod(1); }
		~HiResGuard() { timeEndPeriod(1); }
		HiResGuard(const HiResGuard&) = delete;
		HiResGuard& operator=(const HiResGuard&) = delete;
	};

}	// namespace

int main(int argc, char** argv) {
	// 소스는 /utf-8 로 컴파일되는데 콘솔 기본 코드페이지는 CP949 다 — 마을과 같다.
	SetConsoleOutputCP(CP_UTF8);
	core::log_open_default();

	const CliArgs args = parse_args(argc, argv);

	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		core::logf("[FAIL] WSAStartup failed\n");
		return 1;
	}

	{
		// 못 읽어도 기본값으로 뜬다 — 파일 하나 빠뜨린 배포가 장애가 되면 안 된다
		// (마을과 같은 판단, ADR-007).
		core::Config cfg;
		if (!cfg.load("config\\session.ini")) {
			core::logf("[WARN] config\\session.ini 를 못 읽었다 — 기본값으로 간다\n");
		}

		const int client_port = cfg.get_int("server", "client_port", 9200, 1, 65535);
		const int max_connections = cfg.get_int("server", "max_connections", 4096, 0, 1000000);
		const int idle_timeout_sec = cfg.get_int("server", "idle_timeout_sec", 0, 0, 86400);
		const int s2s_port = cfg.get_int("s2s_accept", "port", 9100, 1, 65535);
		const int registry_capacity = cfg.get_int("registry", "capacity", 16, 1, 100000);
		const int health_period_ms = cfg.get_int("registry", "health_period_ms", 5000, 1, 3600000);
		const int health_fail_count = cfg.get_int("registry", "health_fail_count", 3, 1, 1000);
		const int orphan_grace_ms = cfg.get_int("registry", "orphan_grace_ms", 30000, 0, 86400000);
		const int unregister_grace_ms = cfg.get_int("registry", "unregister_grace_ms", 5000, 0, 3600000);
		const int reserve_expire_ms = cfg.get_int("reserve", "expire_ms", 10000, 1, 3600000);
		const int sweep_ms = cfg.get_int("reserve", "sweep_ms", 30000, 1, 3600000);
		const int request_timeout_ms = cfg.get_int("reserve", "request_timeout_ms", 10000, 1, 3600000);

		// 설정을 읽은 직후, 아무것도 띄우기 전에 건다 — 마을과 같은 이유(기동
		// 도중의 죽음이 가장 진단이 어렵다). 실패해도 계속 간다.
		ops::CrashConfig crash;
		ops::install_crash_handler(crash);

		//  여기부터 선언 순서가 소멸 순서를 정한다 (지역변수는 역순 소멸).
		//
		//    선언 :  hires_guard → registry/router → 클라 서버 → S2S 서버
		//    소멸 :  S2S 서버 → 클라 서버 → router/registry → hires_guard
		//
		//  콜백 타깃(registry/router)이 서버들보다 먼저 선언돼야 서버가 먼저
		//  죽는다 — 뒤집으면 start 실패·예외 경로의 소멸자 순서에서 살아 있는
		//  서버가 죽은 router 를 콜백한다(마을의 zones·db_pool 이 server 보다 먼저인
		//  것과 같은 이유).
		const HiResGuard hires_guard;

		session::Registry registry(
			static_cast<uint32_t>(registry_capacity),
			static_cast<uint32_t>(health_period_ms),
			static_cast<uint32_t>(health_fail_count),
			static_cast<uint64_t>(orphan_grace_ms),
			static_cast<uint64_t>(unregister_grace_ms));

		const session::RouterSettings router_settings{
			static_cast<uint32_t>(reserve_expire_ms),
			static_cast<uint32_t>(request_timeout_ms),
		};
		session::Router router(registry, router_settings);

		net::IocpServer client_server(net::kFramePoolCapacity);
		net::IocpServer s2s_server(kS2sFramePoolCapacity);

		router.wire(client_server, s2s_server);

		// start() 전에 꽂는다 — 스윕 스레드가 start() 에서 뜬다(마을과 같다).
		// S2S 쪽은 유휴 정리를 끈다 — 마을 하트비트(5s 주기)가 유휴를 만들지 않고,
		// 죽은 마을의 정리는 orphan 유예가 담당한다(두 장치가 같은 연결을 다투면
		// 어느 쪽이 끊었는지 로그로 가릴 수 없게 된다).
		client_server.set_idle_policy(idle_timeout_sec, 0);
		s2s_server.set_idle_policy(0, 0);

		if (!client_server.start(static_cast<unsigned short>(client_port),
				kIoWorkerCount, static_cast<size_t>(max_connections))) {
			core::logf("[FAIL] 클라 수용 서버 start 실패 (port=%d)\n", client_port);
			WSACleanup();
			core::log_close();
			return 1;
		}
		if (!s2s_server.start(static_cast<unsigned short>(s2s_port), kIoWorkerCount, 0)) {
			core::logf("[FAIL] S2S 수용 서버 start 실패 (port=%d)\n", s2s_port);
			client_server.stop();
			WSACleanup();
			core::log_close();
			return 1;
		}
		core::logf("[INFO] session server up — client=%d s2s=%d registry_cap=%d\n",
			client_port, s2s_port, registry_capacity);

		// 레지스트리 스윕 스레드 — 만료 예약·유예 초과 orphan·pending 타임아웃의
		// lazy 판정 주기다. net 의 idle 스윕과 별개인 이유 — 그쪽은 net 소유 정책
		// 이고 이쪽은 router 소유 테이블이라, 계층을 섞으면 net 이 위층 뮤텍스를
		// 잡게 된다(마을이 존 스레드에 net 스윕을 안 얹은 것과 같은 판단).
		std::mutex sweep_mutex;
		std::condition_variable sweep_cv;
		bool stopping = false;
		std::thread sweeper([&]() {
			std::unique_lock<std::mutex> lk(sweep_mutex);
			while (!stopping) {
				sweep_cv.wait_for(lk, std::chrono::milliseconds(sweep_ms),
					[&]() { return stopping; });
				if (stopping) {
					break;
				}
				lk.unlock();
				router.on_sweep(GetTickCount64());
				lk.lock();
			}
		});

		// 서버가 다 뜬 뒤에 죽인다 — 스레드가 다 도는 상태의 덤프여야 한다(마을과 같다).
		if (args.crash_mode == 1) { ops::crash_now(ops::CrashKind::kAccessViolation); }
		if (args.crash_mode == 2) { ops::crash_now(ops::CrashKind::kCppThrow); }
		if (args.crash_mode == 3) { ops::crash_now(ops::CrashKind::kTerminate); }

		if (args.auto_seconds > 0) {
			core::logf("[INFO] auto-stop in %d s\n", args.auto_seconds);
			std::this_thread::sleep_for(std::chrono::seconds(args.auto_seconds));
		} else {
			core::logf("[INFO] drain <id> / undrain <id> — 빈 줄 또는 EOF 로 정지\n");
			std::string line;
			while (std::getline(std::cin, line)) {
				// 파이프로 보낸 개행은 "\r\n" 일 수 있다(harness_common.ps1 의
				// StreamWriter.WriteLine 이 그렇다) — std::getline 은 '\n' 만
				// 자르므로 남는 '\r' 을 여기서 벗긴다. 실제 콘솔 입력은 애초에
				// '\r' 이 안 실려 이 줄이 no-op 이다.
				if (!line.empty() && line.back() == '\r') {
					line.pop_back();
				}
				if (line.empty()) {
					break;   // 마을과 같은 종료 규약 — 빈 줄(Enter 하나) 또는 EOF
				}
				std::istringstream iss(line);
				std::string cmd;
				iss >> cmd;
				if (cmd == "drain" || cmd == "undrain") {
					uint32_t server_id = 0;
					if (iss >> server_id) {
						router.request_set_mode(server_id, cmd == "drain");
					} else {
						core::logf("[INFO] 사용법: drain <id> | undrain <id>\n");
					}
				} else {
					core::logf("[INFO] 사용법: drain <id> | undrain <id>\n");
				}
			}
		}

		//  명시 stop 순서 — 스윕 join → 클라 stop → S2S 서버 stop → router 정리.
		//  스윕이 먼저 서야 stop 중의 테이블에 주기 작업이 끼어들지 않는다.
		//  클라가 S2S 보다 먼저인 이유 — 클라 워커가 Registry 뮤텍스 아래에서
		//  S2S 세션 포인터로 send 하는 경로가 있어(Reserve 발신), S2S 를 먼저
		//  내리면 그 send 가 stop 의 세션 전량 삭제와 겹칠 수 있다. 역방향 의존은
		//  없다 — S2S 쪽 처리는 클라 세션을 보관하지 않는다. router 정리는 두
		//  stop 의 session_gone 발화(전수 보장은 아니다 — 파일 머리 주석)가 끝난
		//  뒤라야 잔존 감사가 뜻을 갖는다.
		{
			std::lock_guard<std::mutex> lk(sweep_mutex);
			stopping = true;
		}
		sweep_cv.notify_all();
		sweeper.join();

		client_server.stop();
		s2s_server.stop();
		router.shutdown();

		router.log_summary();
	}

	WSACleanup();

	// 마지막에 닫는다. 위 블록의 소멸자들이 남기는 로그까지 파일에 들어가야 한다.
	core::log_close();
	return 0;
}
