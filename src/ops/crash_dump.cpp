#include "ops/crash_dump.h"

#include "core/log.h"

#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <cstdlib>      // _exit — on_terminate 가 abort 대신 쓰는 것
#include <cstring>      // 크래시 경로의 strlen — 전이 include 에 기대지 않는다
#include <exception>    // std::set_terminate
#include <stdexcept>    // std::runtime_error — crash_now(kCppThrow)

namespace {

    // 핸들러가 쓸 값은 「미리」 담아 둔다.
    //   크래시 시점에 std::string 을 읽으면 그 문자열이 힙에 있고,
    //   힙이 손상돼서 죽은 거라면 거기서 또 죽는다. → 고정 배열에 복사해 둔다.
    char     g_dump_dir[MAX_PATH] = "dumps";
    bool     g_full_memory = false;
    unsigned g_flush_ms = 200;

    // 재진입 차단. 핸들러 안에서 또 죽으면 무한 루프가 된다 —
    //   그러면 프로세스가 안 죽고 CPU 만 태우면서 매달려 있다.
    //   「죽는 것」보다 「안 죽고 이상하게 살아 있는 것」이 운영에는 더 나쁘다.
    LONG g_in_handler = 0;

    //  파일 이름 — dumps\server_20260823_041530_12345.dmp
    //  시각과 pid 를 둘 다 넣는다. 시각만 넣으면 같은 초에 두 프로세스가
    //    죽었을 때 하나가 덮인다. 여러 대를 띄우는 서버에서 실제로 일어난다.
    //  snprintf 는 이 인자들로 힙을 안 쓴다. std::string 을 만들면 쓴다.
    void build_dump_path(char* out, size_t cap) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        _snprintf_s(out, cap, _TRUNCATE,
            "%s\\server_%04u%02u%02u_%02u%02u%02u_%lu.dmp",
            g_dump_dir, t.wYear, t.wMonth, t.wDay,
            t.wHour, t.wMinute, t.wSecond,
            GetCurrentProcessId());
    }

    //  덤프를 쓴다. 여기가 이 단계의 전부다.
    //  반환 = 남긴 파일 크기(바이트). 0 이면 실패.
    //
    //  실패 코드를 인자로 「같이」 돌려준다. 호출부에서 GetLastError() 를 부르면
    //    안 되기 때문이다 — 그 사이에 CloseHandle 과 DeleteFileA 가 이미 돌아서
    //    마지막 오류가 덮인다. 한때 그렇게 적혀 있었고, 덤프가 안 남은 이유를 쫓을 때
    //    거짓 단서를 주고 있었다. 「파일이 있는데 안 열린다」만큼이나 나쁘다 —
    //    있지도 않은 원인을 몇 시간 쫓게 만든다.
    //    오류는 그것을 낸 API 바로 뒤에서 집는다. 그게 유일하게 맞는 자리다.
    DWORD write_dump(EXCEPTION_POINTERS* ep, char* path_out, size_t cap, DWORD& err_out) {
        err_out = 0;
        CreateDirectoryA(g_dump_dir, nullptr);
        build_dump_path(path_out, cap);

        const HANDLE file = CreateFileA(path_out, GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            err_out = GetLastError();       // CreateFileA 직후
            return 0;
        }

        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        // FALSE = 이 구조체가 「덤프를 뜨는 프로세스」의 주소공간에 있다는 뜻.
        //   우리는 자기 자신을 뜨므로 FALSE 다. 별도 프로세스에서 뜨면 TRUE 다.
        mei.ClientPointers = FALSE;

        // 기본은 콜스택만이다. MiniDumpWithThreadInfo 를 얹으면 스레드 시작 주소와
        // TEB 가 들어가 어느 스레드가 무엇이었는지(I/O 워커인지 존 스레드인지 DB
        // 워커인지) 구분할 수 있다. 기본 설정에서 우리가 만드는 스레드가 15개라 그
        // 구분이 없으면 콜스택만으로 헷갈린다.
        //
        // 덤프에 찍히는 수는 그보다 많다(실측 18). CRT 와 OS 가 만든 스레드가 섞이므로
        // 몇 개인가로 세지 말고 시작 주소로 무엇인가를 봐야 한다. 두 옵션 다 수백 KB
        // 수준이고 전체 메모리 덤프는 그 100배다.
        MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
            MiniDumpNormal | MiniDumpWithThreadInfo);
        if (g_full_memory) {
            type = static_cast<MINIDUMP_TYPE>(
                MiniDumpWithFullMemory | MiniDumpWithHandleData |
                MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
        }

        const BOOL ok = MiniDumpWriteDump(
            GetCurrentProcess(), GetCurrentProcessId(), file,
            type, (ep != nullptr) ? &mei : nullptr, nullptr, nullptr);

        DWORD size = 0;
        if (ok) {
            size = GetFileSize(file, nullptr);
        }
        else {
            // 여기서 집는다. 아래 CloseHandle · DeleteFileA 가 덮기 전이다.
            //   MiniDumpWriteDump 는 HRESULT 를 GetLastError 에 남기는 경우가 있다
            //     (0x8007xxxx). 그대로 찍어야 그게 보인다 — 해석은 사람이 한다.
            err_out = GetLastError();
        }
        CloseHandle(file);

        // 실패했으면 0바이트 파일이 남는다. 그건 「덤프가 있다」는 거짓말이라 지운다.
        //   「파일이 있는데 열리지 않는다」가 운영에서 제일 시간을 잡아먹는 상태다.
        if (!ok || size == 0) {
            DeleteFileA(path_out);
            return 0;
        }
        return size;
    }

    // 핸들러 본체 — 순서가 곧 판단이다. 덤프를 먼저 쓰고 그다음 로그를 flush 한다.
    //
    // 원래 계획은 flush 가 먼저였는데 뒤집었다. flush 는 락을 두 개 잡고 힙에 vector 를
    // 만드는데, 크래시 원인이 힙 손상이나 데드락이면 거기서 또 죽는다 — 로그를 지키려다
    // 덤프를 잃는 것이다. 덤프가 더 중요하다는 판단을 순서로 표현했다.
    //
    // flush 는 무한 대기하지 않고, 못 잡으면 포기한다. 비동기 로거는 잃을 수 있다는
    // 전제 위에 있어서 몇 줄 더 잃는 건 설계 안의 손실이다. 덤프는 그렇지 않다 —
    // 못 남기면 그 사고는 영원히 재현 못 한다.
    LONG handle_crash(EXCEPTION_POINTERS* ep, const char* what) {
        if (InterlockedExchange(&g_in_handler, 1) != 0) {
            return EXCEPTION_EXECUTE_HANDLER;    // 두 번째다. 손 뗀다.
        }

        char path[MAX_PATH] = { 0 };
        DWORD       err  = 0;
        const DWORD size = write_dump(ep, path, sizeof(path), err);

        // 이 줄들은 「비동기 로거의 큐」에 들어간다. 아래 flush 가 그걸 밀어낸다.
        //   flush 가 실패하면 이 줄들도 같이 잃는다 — 그래서 덤프를 먼저 쓴 것이다.
        if (size > 0) {
            core::logf("[ERROR] 크래시(%s) — 덤프 %s (%lu bytes)\n",
                what, path, static_cast<unsigned long>(size));
        }
        else {
            core::logf("[ERROR] 크래시(%s) — 덤프 쓰기 실패 (err=%lu / 0x%08lX)\n",
                what, static_cast<unsigned long>(err),
                static_cast<unsigned long>(err));
        }

        if (g_flush_ms > 0) {
            const bool flushed = core::log_flush_try(g_flush_ms);
            if (!flushed && size > 0) {
                // 로그가 안 나갔으니 콘솔에 직접 찍는다. 락을 안 쓰는 유일한 경로다.
                //   이것도 실패할 수 있다. 그때는 덤프 파일만 남는다 — 그걸로 충분하다.
                char line[MAX_PATH + 64];
                _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "[CRASH] dump: %s (%lu bytes) - log flush timed out\n",
                    path, static_cast<unsigned long>(size));
                DWORD written = 0;
                WriteFile(GetStdHandle(STD_ERROR_HANDLE), line,
                    static_cast<DWORD>(strlen(line)), &written, nullptr);
            }
        }

        // EXECUTE_HANDLER = 「내가 처리했다, 프로세스를 정상 종료 절차로 보내라」.
        //   CONTINUE_SEARCH 를 돌려주면 Windows 오류 보고(WER)가 다시 뜬다 —
        //     서버에서는 그 대화상자가 프로세스를 몇 분씩 붙들어 둔다.
        //    「죽을 때는 빨리 죽어야」 감시 프로세스가 재기동을 건다.
        return EXCEPTION_EXECUTE_HANDLER;
    }

    LONG WINAPI unhandled_filter(EXCEPTION_POINTERS* ep) {
        return handle_crash(ep, "SEH");
    }

    // 백스톱 — std::terminate() 가 직접 불리는 경로.
    //
    // 처음에는 「C++ 의 안 잡힌 throw 는 위 필터로 안 잡힌다」고 적었는데 틀렸다.
    // MSVC 는 C++ 예외를 SEH 로 구현해서, 안 잡힌 throw 는 예외 코드 0xE06D7363 짜리
    // SEH 예외로 올라와 필터가 먼저 잡는다. --crash-throw 의 종료 코드로 확인했다.
    //
    // 그래도 남기는 이유는 terminate 가 throw 말고도 불리기 때문이다 — noexcept 함수가
    // 던졌을 때, 소멸자에서 던졌을 때, 그리고 joinable 한 std::thread 를 그냥
    // 소멸시켰을 때. 마지막은 실재하는 위험이다. app::WorkerPool 이나 app::TickThread 가
    // stop() 에서 join 을 빠뜨리면 정확히 그 경우가 된다.
    //
    // --crash-terminate 로 실제로 불러서 확인했다. 안 부르는 핸들러는 걸어 둔 게
    // 아니라 검증 안 된 코드다.
    void on_terminate() {
        handle_crash(nullptr, "terminate");
        // ep 가 nullptr 이라 「예외 정보」가 없다. 콜스택은 여전히 나온다 —
        //   덤프는 현재 스레드들의 스택을 뜨는 것이지 예외 기록이 아니다.
        //   다만 terminate 시점의 스택은 「원인 지점」이 아니라 그 위다. 한계다.
        // _exit 인 이유 — abort() 로 가면 CRT 가 다시 예외를 던져 재진입한다.
        _exit(3);
    }

}   // namespace

namespace ops {

    bool install_crash_handler(const CrashConfig& cfg) {
        _snprintf_s(g_dump_dir, sizeof(g_dump_dir), _TRUNCATE, "%s",
            cfg.dump_dir.empty() ? "dumps" : cfg.dump_dir.c_str());
        g_full_memory = cfg.full_memory;
        g_flush_ms = cfg.log_flush_ms;

        // 오류는 그것을 낸 API 「바로 뒤에서」 집는다 — write_dump 가 실제로 데이고
        //   세운 원칙인데(위 55행), 정작 같은 파일의 이 자리에서 안 지켜지고 있었다.
        //   한때 여기는 조건문과 logf 인자에서 GetLastError 를 두 번 불렀다.
        //   지금은 그 사이에 Win32 호출이 없어 값이 같다. 즉 맞는 것이 아니라
        //     「사이에 아무것도 없다」는 우연에 기대고 있었다 — 줄 하나만 끼어들면
        //     조용히 다른 오류 코드를 찍고, 그건 덤프가 안 남는 이유를 쫓을 때
        //     거짓 단서가 된다. write_dump 가 겪은 것이 정확히 그것이다.
        const BOOL  made   = CreateDirectoryA(g_dump_dir, nullptr);
        const DWORD mk_err = made ? 0 : GetLastError();

        if (!made && mk_err != ERROR_ALREADY_EXISTS) {
            core::logf("[WARN] 덤프 폴더 '%s' 를 못 만들었다 (err=%lu) — "
                       "크래시 때 덤프가 안 남는다\n",
                g_dump_dir, mk_err);
            return false;
        }

        SetUnhandledExceptionFilter(unhandled_filter);
        std::set_terminate(on_terminate);

        core::logf("[INFO] crash handler: dir=%s type=%s flush=%ums\n",
            g_dump_dir, g_full_memory ? "full-memory" : "normal", g_flush_ms);
        return true;
    }

    void crash_now(CrashKind kind) {
        if (kind == CrashKind::kTerminate) {
            // SEH 예외를 만들지 않고 곧장 terminate 로 간다.
            //   joinable 한 std::thread 를 그냥 소멸시켰을 때와 같은 경로다.
            core::logf("[WARN] --crash-terminate — std::terminate() 직접 호출\n");
            std::terminate();
        }
        if (kind == CrashKind::kCppThrow) {
            core::logf("[WARN] --crash throw — 일부러 죽인다 (안 잡힌 C++ 예외)\n");
            throw std::runtime_error("intentional crash for handler test");
        }

        core::logf("[WARN] --crash — 일부러 죽인다 (null 역참조)\n");
        // volatile 이 아니면 최적화가 이 줄을 지운다. Release 에서 안 죽는다 —
        //   일부러 낸 결함이 Release 에서만 재현이 안 되는 가장 흔한 이유가 이것이다.
        volatile int* p = nullptr;
        *p = 42;
    }

}   // namespace ops
