//  core/config.h — ini 설정 파일
//
//  포트나 DB 접속 정보를 코드에 박으면 바꾸려고 빌드를 다시 해야 하고,
//  운영에서 그건 배포다.
//
//  ini 라이브러리를 안 붙였다 — 읽는 게 파일 하나고, 의존성이 하나 늘면
//  빌드·배포가 그만큼 는다. 섹션이 수십 개가 되고 타입 검증이 필요해지면 그때 간다.
#pragma once

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>

#include "core/log.h"   // 틀린 설정을 조용히 넘기지 않으려고 — 경고를 여기서 낸다

namespace core {

    class Config {
    public:
        // false = 파일을 못 열었다. 「기본값으로 갈지 멈출지」는 호출자가 정한다.
        bool load(const std::string& path) {
            std::ifstream in(path);
            if (!in) {
                return false;
            }

            std::string section;
            std::string line;
            while (std::getline(in, line)) {
                strip_bom(line);
                strip_comment(line);
                trim(line);
                if (line.empty()) {
                    continue;
                }

                if (line.front() == '[' && line.back() == ']') {
                    section = line.substr(1, line.size() - 2);
                    trim(section);
                    continue;
                }

                const size_t eq = line.find('=');
                if (eq == std::string::npos) {
                    continue;                    // key=value 모양이 아니면 조용히 넘긴다
                }

                std::string key = line.substr(0, eq);
                std::string val = line.substr(eq + 1);
                trim(key);
                trim(val);
                if (key.empty()) {
                    continue;
                }

                // "섹션.키" 하나로 합쳐 담는다. 중첩 맵보다 조회가 단순하다.
                values_[section + "." + key] = val;
            }
            return true;
        }

        std::string get(const std::string& section, const std::string& key,
                        const std::string& def = "") const {
            const auto it = values_.find(section + "." + key);
            return (it != values_.end()) ? it->second : def;
        }

        // 못 읽거나 범위 밖이면 기본값을 쓰고 경고를 남긴다.
        //
        // 한때 atoi 였다. atoi 는 실패를 0 으로 돌려주는데, 하필 이 서버에서 0 은 키마다
        // 뜻이 다르다(자동 / 무제한 / 끔). 4O96 같은 오타 하나가 키에 따라 전혀 다른
        // 증상으로 나타나서 원인을 찾기 유난히 나쁘다. 음수도 문제다 — 받는 쪽이
        // size_t 면 -1 이 SIZE_MAX 가 된다. 실제로 frame_pool_capacity = -1 이 곱셈
        // 오버플로로 기동을 죽였고, max_connections = -1 은 상한을 조용히 없앴다.
        //
        // 범위를 키마다 받는 것은 「음수는 무조건 거절」로 뭉갤 수 없어서다. 지금은
        // 음수를 받는 키가 없지만, 무엇이 정상인지는 그 값을 쓰는 쪽만 안다. 한 키의
        // 사정이 사라졌다고 그 판단을 이 층으로 끌어오면 다음에 다시 뜯어야 한다.
        //
        // 틀린 값에 멈추지 않고 기본값으로 가는 것은, 설정 한 줄 때문에 서버가 안 뜨면
        // 배포에서 오타 하나가 장애가 되기 때문이다. 대신 조용히 넘어가지는 않는다.
        int get_int(const std::string& section, const std::string& key, int def = 0,
                    int lo = INT_MIN, int hi = INT_MAX) const {
            const auto it = values_.find(section + "." + key);
            if (it == values_.end() || it->second.empty()) {
                return def;
            }

            const std::string& raw = it->second;
            errno = 0;
            char* end = nullptr;
            const long v = std::strtol(raw.c_str(), &end, 10);

            // 앞이 숫자가 아니거나(end 가 안 움직임), 뒤에 뭐가 더 붙어 있다
            if (end == raw.c_str() || *end != '\0') {
                logf("[WARN] config [%s] %s = \"%s\" — 숫자가 아니다. 기본값 %d 로 간다\n",
                    section.c_str(), key.c_str(), raw.c_str(), def);
                return def;
            }
            if (errno == ERANGE || v < static_cast<long>(lo) || v > static_cast<long>(hi)) {
                logf("[WARN] config [%s] %s = %ld — 허용 범위 [%d..%d] 밖이다."
                     " 기본값 %d 로 간다\n",
                    section.c_str(), key.c_str(), v, lo, hi, def);
                return def;
            }
            return static_cast<int>(v);
        }

        bool has(const std::string& section, const std::string& key) const {
            return values_.find(section + "." + key) != values_.end();
        }

    private:
        // UTF-8 BOM. 첫 줄에만 붙는데, 안 떼면 첫 섹션 이름이 "\xEF\xBB\xBF[server]" 가
        //   되어 조회가 전부 빗나간다. 파일은 멀쩡해 보이는데 값만 안 읽히는,
        //   원인을 찾기 유난히 성가신 종류의 버그다.
        static void strip_bom(std::string& s) {
            if (s.size() >= 3 &&
                static_cast<unsigned char>(s[0]) == 0xEF &&
                static_cast<unsigned char>(s[1]) == 0xBB &&
                static_cast<unsigned char>(s[2]) == 0xBF) {
                s.erase(0, 3);
            }
        }

        static void strip_comment(std::string& s) {
            // 1) 줄 전체가 주석인 경우
            const size_t p = s.find_first_not_of(" \t");
            if (p != std::string::npos && (s[p] == ';' || s[p] == '#')) {
                s.clear();
                return;
            }

            // 2) 값 뒤에 붙은 주석
            //    「공백 뒤의 ; 또는 #」만 주석으로 본다.
            //      그냥 ; 를 다 자르면 password = a;b 같은 값이 잘린다.
            //      비밀번호에 특수문자가 들어가는 건 흔한 일이라 실제로 물릴 수 있다.
            for (size_t i = 0; i + 1 < s.size(); ++i) {
                if ((s[i] == ' ' || s[i] == '\t') && (s[i + 1] == ';' || s[i + 1] == '#')) {
                    s.erase(i);
                    return;
                }
            }
        }

        static void trim(std::string& s) {
            const size_t b = s.find_first_not_of(" \t\r\n");
            if (b == std::string::npos) {
                s.clear();
                return;
            }
            const size_t e = s.find_last_not_of(" \t\r\n");
            s = s.substr(b, e - b + 1);
        }

        std::map<std::string, std::string> values_;
    };

}   // namespace core
