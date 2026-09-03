// db/db_worker.h — DB 서비스: mysql_library_init/end 프로세스 수명 + 기본(primary) 풀 소유.
//
// 예전엔 이 파일에 DbWorkerPool(전용 워커 스레드 · player_id % N 큐 분할 ·
// post_read/post_write)이 있었다. 스케줄러가 직렬 큐 실행권(app::WorkerPool)으로
// 통합되며 그 구조가 통째로 사라졌다 — DB 를 부르는 스레드가 이제 그 실행권을
// 처리하는 워커 자신이라, 「DB 잡을 다른 큐로 넘긴다」는 개념 자체가 없다.
// 순서 보장도 큐 분할이 아니라 그 세션의 직렬 큐가 진다(같은 세션의 확인·조회는
// 애초에 순차 실행이라 순서가 어긋날 자리가 없다).
//
// 남은 것은 셋뿐이다 — DbPool 소유, mysql_library_init/end 짝, 종료 시 [POOL2] 요약.
// 클래스 이름을 그대로 둔 것은 신설 파일을 늘리지 않으려는 것뿐이고, 내용은
// 완전히 다른 것으로 봐야 한다.
#pragma once

#include "db/db_conn.h"
#include "db/db_pool.h"

namespace db {

    class DbWorkerPool {
    public:
        DbWorkerPool(const DbConfig& cfg, int pool_size);
        ~DbWorkerPool();

        DbWorkerPool(const DbWorkerPool&) = delete;
        DbWorkerPool& operator=(const DbWorkerPool&) = delete;

        // mysql_library_init + 미리 연결 채우기(prewarm). 실패해도 true 를 돌려준다 —
        //   DB 가 죽어 있어도 서버는 떠야 한다(로그인·채팅은 DB 와 무관하다).
        bool start();

        // pool_.stop() + [POOL2] 요약 로그 + mysql_library_end.
        //   app::WorkerPool(직렬 큐 워커) 을 전부 join 한 「뒤에」 불러야 한다 —
        //   그 전에 닫으면 아직 도는 워커가 빈 Lease 를 받아 질의가 실패한다.
        //   멱등 — main() 이 명시로 부른 뒤에도 소멸자가 다시 부른다(스코프 종료).
        //   옛 워커 스레드 버전은 threads_.empty() 가 그 가드였다 — 워커 자체가
        //   없어진 지금은 stopped_ 로 직접 든다. 안 두면 mysql_library_end() 가
        //   두 번 불려 정의되지 않은 동작이 된다(MySQL C API 는 init/end 짝을
        //   프로세스당 정확히 한 번으로 규정한다).
        void stop();

        // 트레이드·인벤토리 핸들러가 그 자리에서 동기로 빌리는 자리.
        DbPool& pool() { return pool_; }

    private:
        DbConfig cfg_;
        DbPool   pool_;
        bool     stopped_ = false;
    };

}   // namespace db
