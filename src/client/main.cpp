//  client/main.cpp — 진입점: WinsockScope 스코프 → 서브커맨드 분기 → 종료 코드
//
//  이 파일이 하는 일은 딱 하나 — "어느 서브커맨드인가"를 가르는 것뿐이다.
//  실제 판정 절차는 각각 cmd_send.cpp·cmd_flow.cpp·selftest.cpp 로 쪼개
//  둔다 — 그래야 그 절차가 바뀌어도 이 파일은 안 바뀐다(진입점을 "순서가
//  곧 계약"인 코드만 남기는 것은 src/main.cpp:1-11 의 관례를 따른 것이다).
//
//  단일 스레드 · 블로킹 순차 처리다(T015-plan.md 결정 3) — 타이머 스레드도
//  수신 스레드도 만들지 않는다. --ping-ms 의 주기 송신(Step 2)도 hold
//  루프 안에서 select 대기를 조각내어 같은 스레드가 한다.
//
//  ⚠️ WinsockScope 를 main() 의 최상위 스코프에 정확히 1개만 두는 이유 —
//  아래로 내려가는 모든 반환 경로가 이 스코프를 벗어날 때 소멸자가
//  WSACleanup 을 1회 부른다(RAII — docs/CODING_RULES.md §4). 손으로 모든
//  return 문 앞에 WSACleanup 을 넣으면 새 반환 경로를 추가할 때마다
//  하나씩 빠뜨릴 위험이 생긴다.

#include "commands.h"
#include "args.h"
#include "tcp_client.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
	client::WinsockScope winsock;
	if (!winsock.ok()) {
		std::printf("WSAStartup failed\n");
		return static_cast<int>(client::ExitCode::kConnect);
	}

	if (argc < 2) {
		client::print_usage();
		return static_cast<int>(client::ExitCode::kUsage);
	}

	const std::string sub = argv[1];

	if (sub == "help") {
		client::print_usage();
		return static_cast<int>(client::ExitCode::kPass);
	}

	if (sub == "selftest") {
		return client::run_selftest();
	}

	if (sub == "send") {
		const client::Args args(argc, argv, 2);
		return client::run_send(args);
	}

	if (sub == "flow") {
		const client::Args args(argc, argv, 2);
		return client::run_flow(args);
	}

	// 알 수 없는 서브커맨드 — usage + kUsage(2).
	client::print_usage();
	return static_cast<int>(client::ExitCode::kUsage);
}
