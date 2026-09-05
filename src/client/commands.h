//  client/commands.h — 서브커맨드 진입점과 종료 코드 규약
//
//  서브커맨드마다 함수 하나(run_send/run_flow/run_selftest)로 쪼갠 이유 —
//  main.cpp 는 "어느 서브커맨드인가"를 가르는 것 말고 아무 로직도 갖지
//  않는다(src/main.cpp:1-11 의 "진입점은 순서가 곧 계약인 코드만" 관례).
//  판정 절차(T015-impl.md §4~§6)가 나중에 바뀌어도 main.cpp 는 안 바뀐다.
//
//  ExitCode 를 enum class 로 못박은 이유 — 이 값이 그대로 프로세스 종료
//  코드로 나가므로(설계 결정 4 — 하네스가 콘솔 문자열이 아니라 이 값을
//  읽는다) 일반 enum 처럼 int 로 암묵 변환되면 usage 오류(2)와 판정
//  실패(1)를 실수로 바꿔 반환해도 컴파일러가 못 잡는다.
//
//  commands.cpp 가 없는 이유 — print_usage/result_line 은 cmd_send.cpp·
//  cmd_flow.cpp·selftest.cpp·main.cpp 네 번역 단위 전부에서 부른다. 별도
//  .cpp 를 만들면 client.vcxproj 에 다섯 번째 ClCompile 이 늘어날 뿐이고,
//  본문이 몇 줄 안 되는 출력 헬퍼라 inline 으로 헤더에 두는 쪽이 짝(선언
//  파일=정의 파일)이 하나 준다. frame_codec.h·args.h 도 같은 이유로 헤더
//  전용이다(T015-impl.md §3).
//
//  ⚠️ result_line 은 매번 fflush(stdout) 한다 — 하네스(scripts\client.ps1)
//  가 `& $exe @ArgList 2>&1` 로 줄 단위 캡처를 하므로, 마지막 판정 줄이
//  버퍼에 남은 채 프로세스가 죽으면 하네스가 그 줄을 못 본다
//  (docs\TESTING.md §5 실측 — stdout 리다이렉트 버퍼링 절단 사고).

#pragma once

#include "args.h"

#include <cstdio>

namespace client {

	// 종료 코드 — T015-plan.md 결정 4.
	//   kPass    0  판정 전건 통과
	//   kFail    1  판정 불일치(프레임 수 · 순서 · result 코드 · 상대 종료 등)
	//   kUsage   2  인자 오류(알 수 없는 서브커맨드/키 · 범위 밖 값)
	//   kConnect 3  접속·초기화 실패(connect 실패 · WSAStartup 실패)
	enum class ExitCode : int {
		kPass = 0,
		kFail = 1,
		kUsage = 2,
		kConnect = 3,
	};

	// 서브커맨드 본체 — Step 2(send) · Step 3(flow) 에서 채운다.
	int run_send(const Args& args);
	int run_flow(const Args& args);

	// frame_codec.h 순수 함수 단위 테스트 — Step 2 에서 항목을 채운다.
	int run_selftest();

	// 알 수 없는 서브커맨드/인자 · `client help` 공용 usage 출력.
	inline void print_usage() {
		std::printf(
			"client <subcommand> [--key value] [--flag]\n"
			"\n"
			"subcommands:\n"
			"  send      raw/framed TCP 왕복 (send.ps1 이식)\n"
			"  flow      로그인 -> 배정 -> Enter -> Echo -> Ping\n"
			"  selftest  frame_codec.h 순수 함수 단위 테스트\n"
			"  help      이 화면\n"
		);
	}

	// 판정 줄 — 항상 마지막 줄이다. reason 은 pass==true 면 무시된다(nullptr 허용).
	inline void result_line(bool pass, const char* reason) {
		if (pass) {
			std::printf("RESULT: PASS\n");
		} else {
			std::printf("RESULT: FAIL %s\n", reason != nullptr ? reason : "");
		}
		// fflush 를 여기 한 곳에 모아 두는 이유 — 호출부마다 흩어 두면
		// 하나 빠뜨리는 순간 그 경로만 버퍼링 절단에 노출된다.
		std::fflush(stdout);
	}

}	// namespace client
