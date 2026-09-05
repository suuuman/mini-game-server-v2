//  client/args.h — `--key value` / `--flag` 최소 인자 파서 (헤더 전용)
//
//  위치 인자를 아예 안 받는 이유 — send.ps1 도 param 블록의 이름 있는
//  스위치만 쓰고(T015-impl.md §3), 위치 인자를 허용하면 "다음 토큰이 값인가
//  다음 옵션인가"를 구분하는 규칙이 하나 더 필요해진다. "--" 로 시작하지
//  않는 토큰은 전부 미정의(extra_)로 몰아 unknown() 에 실어 kUsage 로
//  떨어뜨린다.
//
//  값/플래그 판정 규칙 — "--key" 다음 토큰이 "--" 로 시작하지 않으면 그
//  토큰을 값으로 삼는다. "--raw-size -1" 처럼 값이 음수라도 단일 "-" 는
//  "--" 가 아니므로 값으로 인식된다. 다음 토큰이 없거나 "--" 로 시작하면
//  이 키는 플래그(값 없음)로 등록한다.
//
//  seen_/unknown() — get()·get_int()·get_u64()·has() 는 부를 때마다 그
//  키를 seen_ 에 적어 둔다. unknown() 은 (파싱된 전체 키) - (seen_) 을
//  돌려준다 — 그래서 호출자가 자기 서브커맨드가 아는 옵션을 전부 조회한
//  뒤에 unknown() 을 불러야 실제로 정의된 키 목록과 맞는다. ⚠️ 이 계약을
//  지키지 않으면(정의한 옵션을 안 읽고 넘어가면) 그 옵션이 "미정의"로
//  오판정된다 — cmd_send.cpp·cmd_flow.cpp 는 자기 옵션을 전부 읽은 뒤에만
//  unknown() 을 부른다.
//
//  ⚠️ get_int/get_u64 는 숫자 변환·범위 실패를 예외가 아니라 error_ 에
//  쌓는다(첫 실패만 기록하고 이후 실패는 버린다). 소켓도 안 연 시점의
//  인자 오류로 예외를 던지면 main 이 그걸 잡는 이유가 하나 늘 뿐이다 —
//  ok()/error() 로 조회하는 쪽이 이 클라의 다른 실패 경로(RESULT: FAIL)와
//  결이 같다.

#pragma once

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace client {

	class Args {
	public:
		Args(int argc, char** argv, int start) {
			for (int i = start; i < argc; ++i) {
				const std::string tok(argv[i]);
				if (tok.size() <= 2 || tok[0] != '-' || tok[1] != '-') {
					// "--" 로 시작하지 않는다 — 이 클라는 위치 인자를 받지
					// 않으므로 전부 미정의 취급하고 unknown() 에 실어
					// kUsage 로 떨어뜨린다.
					extra_.push_back(tok);
					continue;
				}
				const std::string key = tok.substr(2);
				bool consumed_value = false;
				if (i + 1 < argc) {
					const std::string next(argv[i + 1]);
					const bool next_is_option = next.size() >= 2 && next[0] == '-' && next[1] == '-';
					if (!next_is_option) {
						values_[key] = next;
						++i;
						consumed_value = true;
					}
				}
				if (!consumed_value) {
					flags_.insert(key);
				}
			}
		}

		std::string get(const std::string& key, const std::string& def) const {
			seen_.insert(key);
			const auto it = values_.find(key);
			return it != values_.end() ? it->second : def;
		}

		int64_t get_int(const std::string& key, int64_t def, int64_t lo, int64_t hi) const {
			seen_.insert(key);
			const auto it = values_.find(key);
			if (it == values_.end()) {
				return def;
			}
			const std::string& s = it->second;
			char* end = nullptr;
			errno = 0;
			const long long v = std::strtoll(s.c_str(), &end, 10);
			if (s.empty() || *end != '\0' || errno == ERANGE || v < lo || v > hi) {
				if (error_.empty()) {
					error_ = "--" + key + " 값이 잘못됐다: " + s;
				}
				return def;
			}
			return static_cast<int64_t>(v);
		}

		uint64_t get_u64(const std::string& key, uint64_t def) const {
			seen_.insert(key);
			const auto it = values_.find(key);
			if (it == values_.end()) {
				return def;
			}
			const std::string& s = it->second;
			if (s.empty() || s[0] == '-') {
				// strtoull 은 음수 문자열을 감싸서 큰 양수로 읽어버린다 —
				// 부호를 여기서 먼저 거른다.
				if (error_.empty()) {
					error_ = "--" + key + " 값이 잘못됐다: " + s;
				}
				return def;
			}
			char* end = nullptr;
			errno = 0;
			const unsigned long long v = std::strtoull(s.c_str(), &end, 10);
			if (*end != '\0' || errno == ERANGE) {
				if (error_.empty()) {
					error_ = "--" + key + " 값이 잘못됐다: " + s;
				}
				return def;
			}
			return static_cast<uint64_t>(v);
		}

		bool has(const std::string& flag) const {
			seen_.insert(flag);
			return flags_.find(flag) != flags_.end();
		}

		// has_value — "--key" 가 값과 함께 실제로 주어졌는가. get_int/get_u64
		// 는 키가 없을 때 def 를 돌려주는데, def 자체가 유효한 값(예:
		// --expect-enter-result 0)일 수 있어 "미지정"과 "0 을 지정"을
		// get_* 의 반환값만으로는 구분할 수 없다 — cmd_flow.cpp 가 이
		// 둘을 다르게 처리해야 하는 자리(§5-1)에서 쓴다.
		bool has_value(const std::string& key) const {
			seen_.insert(key);
			return values_.find(key) != values_.end();
		}

		// 숫자 변환·범위 실패가 하나도 없었는가 — unknown() 과는 다른 축이다.
		// unknown() 은 "모르는 키", ok() 는 "아는 키인데 값이 잘못됐다".
		bool ok() const { return error_.empty(); }
		const std::string& error() const { return error_; }

		// 정의되지 않은 키 — get/get_int/get_u64/has 로 한 번도 조회되지
		// 않은 --key 전부 + 위치 인자 전부. 하나라도 있으면 호출자는 kUsage.
		std::vector<std::string> unknown() const {
			std::vector<std::string> result;
			for (const auto& kv : values_) {
				if (seen_.find(kv.first) == seen_.end()) {
					result.push_back("--" + kv.first);
				}
			}
			for (const auto& f : flags_) {
				if (seen_.find(f) == seen_.end()) {
					result.push_back("--" + f);
				}
			}
			for (const auto& e : extra_) {
				result.push_back(e);
			}
			return result;
		}

	private:
		std::map<std::string, std::string> values_;
		std::set<std::string> flags_;
		std::vector<std::string> extra_;
		mutable std::set<std::string> seen_;
		// error_ 도 seen_ 과 같은 이유로 mutable 이다 — get_int/get_u64 는
		// 인자 파싱 실패를 "값을 만들어 낸 부작용"으로 기록할 뿐 Args 의
		// 파싱 결과 자체(values_/flags_/extra_)를 바꾸지 않으므로 논리적
		// const 다. main.cpp 가 `const Args&` 로 넘기므로 이 메서드들이
		// const 가 아니면 send/flow 구현이 컴파일되지 않는다.
		mutable std::string error_;
	};

}	// namespace client
