// ops/crash_dump.h — 프로세스가 죽을 때 왜 죽었는지를 남긴다.
//
// 라이브 서버가 죽으면 재현이 안 된다. 로그는 무슨 일이 있었나를 말하지만 어느 줄에서
// 죽었나는 말하지 않고, 덤프가 그걸 말한다.
//
// 의존은 core 뿐이다. 크래시 핸들러가 게임 상태를 알면, 그 상태가 망가져서 죽었을 때
// 같이 죽는다.
//
// 이 파일의 코드는 이미 망가진 프로세스 안에서 돈다. 힙이 손상됐거나 락이 잠긴 채로
// 죽었을 수 있어서, 힙 할당을 안 하고(스택 버퍼만), 락을 무한 대기하지 않고(시한 뒤
// 포기), 두 번째 크래시가 나면 즉시 손을 뗀다.
#pragma once

#include <string>

namespace ops {

    struct CrashConfig {
        // 덤프를 떨어뜨릴 폴더. 없으면 만든다.
        std::string dump_dir = "dumps";

        // 기본은 「콜스택만」(MiniDumpNormal · 수백 KB).
        //   변수까지 담으려면 전체 메모리를 떠야 하는데 그건 수십~수백 MB 다.
        //   라이브에서 죽을 때마다 그걸 남기면 디스크가 먼저 죽는다.
        //   기본은 끄고, 재현이 어려운 버그를 쫓을 때만 켠다.
        //   켜면 덤프에 플레이어 데이터와 접속 정보가 통째로 들어간다.
        //     반출·보관 규정이 있는 곳에서는 그 자체가 문제다.
        bool full_memory = false;

        // 로그 flush 를 몇 ms 까지 기다려 볼 것인가. 0 이면 아예 안 한다.
        unsigned log_flush_ms = 200;
    };

    // true = 핸들러를 걸었다. 프로세스당 한 번만 부른다.
    //   실패해도 서버는 뜬다.
    //     크래시 덤프는 진단 수단이지 서비스 조건이 아니다.
    bool install_crash_handler(const CrashConfig& cfg);

    // 핸들러 검증용. 일부러 죽인다 (--crash 인자).
    //   kAccessViolation : null 참조     → SetUnhandledExceptionFilter 가 잡는다
    //   kCppThrow        : 안 잡힌 throw → 재보니 이것도 SEH 로 잡힌다 (0xE06D7363)
    //   kTerminate       : std::terminate() 직접 호출 → set_terminate 백스톱 검증용
    enum class CrashKind { kAccessViolation, kCppThrow, kTerminate };
    void crash_now(CrashKind kind);

}   // namespace ops
