//  client/socket_set.h — 소켓 N개를 단일 스레드로 (fd_set 하나 · select 한 번)
//
//  ★★ 이 파일이 다중 소켓을 다루는 유일한 방법이다 — 열린 소켓을 전부
//  fd_set 하나에 담아 select 를 한 번 부르고, 준비된 소켓에만
//  recv_some(…, /*timeout_ms=*/0) 을 1회만 부른다.
//
//  ★ 스레드를 안 만드는 이유 — ADR-029 결정 3(단일 스레드 · 블로킹+select)을
//  승계한다. 동시 송수신 스레드는 ADR-029 가 버린 대안이다(§18-7 "지금
//  손해 없음" — docs/DECISIONS.md ADR-029 근거 절). TcpClient 는 락이
//  없어 여러 스레드가 같은 인스턴스를 만질 수 없고, 그 보호 장치를 새로
//  들이는 것이 곧 그 손해다. loopback 전용 CLI 라 소켓 N개를 한 스레드가
//  select 로 돌리는 데 관측된 손해가 없다(잰 값은 없다).
//
//  ★ recv_some(…, 0) 을 소켓마다 "1회만" 부르는 이유 — 헤드-오브-라인
//  차단. recv_exact/read_frame 은 n 바이트(또는 프레임 하나)가 채워질
//  때까지 그 소켓 하나를 붙들고 기다리는데, 그동안 다른 소켓에 이미 와
//  있는 데이터는 select 기회를 못 얻는다. 그래서 이 파일 안에서는
//  recv_exact/read_frame 을 부르지 않는다(지시서 §1 절대 금지) — 프레임
//  경계 복원은 FrameTally 가 소켓별 잔여 바이트(pending)를 들고 있는
//  방식으로 대신한다(parse_frames 의 "잔량은 호출자가 유지한다" 계약과
//  같다 — frame_codec.h 머리말).
//
//  ⚠️ FD_SETSIZE 를 여기서 #define 하지 않는다 — winsock2.h 는 이미
//  tcp_client.h 가 include 했고, 이 매크로는 그 include **이전**에 있어야
//  효과가 있다(MS Learn "Maximum Number of Sockets Supported" — "the implementor should define the manifest FD_SETSIZE in every source file before including the Winsock2.h header file" · 2026-09-06 열람).
//  그래서 값은 client.vcxproj 의 PreprocessorDefinitions(/D
//  FD_SETSIZE=1024)로 올린다 — 컴파일러 커맨드라인의 /D 는 첫 include
//  보다 먼저 처리된다. 아래 static_assert 는 FD_SETSIZE >= 1024 만
//  증명하고, vcxproj 가 정확히 1024 를 정의한다 — 값을 바꾸면 두 곳을
//  같이 본다.
//
//  select 의 첫 인자(nfds)를 0 으로 넘기는 이유 — Winsock 에서 무시된다
//  (MS Learn select — 이식성을 위해서만 존재하는 인자). tcp_client.cpp:
//  147-155 의 recv_some(tv 분해 147-150 · nfds 주석 152-154)과 같은 근거·방식을 그대로 쓴다.
//
//  ★ 순수 함수(zone_of·expect_count·tally_*)를 소켓 코드와 같은 헤더에
//  둔 이유 — selftest 가 소켓 연결 없이 이 함수들만 직접 두드릴 수
//  있어야 뮤턴트를 잡는다(frame_codec.h 머리말·T015-plan.md 결정 6과
//  같은 논리 — "정상 서버 앞에서 영구히 초록으로 남는 결함"을 막는다).

#pragma once

#include "tcp_client.h"      // winsock2.h 는 이 경로로만 들어온다(tcp_client.h:42)
#include "frame_codec.h"

static_assert(FD_SETSIZE >= 1024, "FD_SETSIZE=1024 가 client.vcxproj PreprocessorDefinitions 에서 이 TU 에 닿지 않았다 — ADR-030 결정 2");

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace client {

	// 열린 소켓만 모아 select 한 번 — INVALID_SOCKET 은 건너뛴다. ready 는
	//   준비된 소켓의 socks 인덱스를 오름차순으로 채운다(FD_ISSET 을 원본
	//   순서 그대로 훑으므로 자연히 오름차순이다).
	//
	//   넣을 소켓이 0개(전부 INVALID_SOCKET 이거나 socks 가 비었으면)면
	//   select 를 부르지 않고 0 을 돌려준다 — 세 fd_set 인자가 전부 비면
	//   Winsock select 는 WSAEINVAL 로 실패한다(MS Learn select 오류표).
	//   "부를 게 없다"와 "불렀는데 실패했다"를 여기서 미리 갈라 둔다.
	//
	//   반환값: 준비된 소켓 개수(0 = 타임아웃 또는 소켓 0개) · select 가
	//   SOCKET_ERROR 면 -1.
	inline int select_readable(const std::vector<SOCKET>& socks, int timeout_ms, std::vector<size_t>& ready) {
		ready.clear();

		fd_set read_set;
		FD_ZERO(&read_set);
		size_t valid_count = 0;
		for (const SOCKET s : socks) {
			if (s == INVALID_SOCKET) {
				continue;
			}
			FD_SET(s, &read_set);
			++valid_count;
		}
		if (valid_count == 0) {
			return 0;
		}

		// tv 분해는 tcp_client.cpp:147-150(recv_some)과 같은 식이다.
		const int wait_ms = timeout_ms < 0 ? 0 : timeout_ms;
		timeval tv{};
		tv.tv_sec = wait_ms / 1000;
		tv.tv_usec = (wait_ms % 1000) * 1000;

		const int sel = select(0, &read_set, nullptr, nullptr, &tv);
		if (sel == 0) {
			return 0;
		}
		if (sel == SOCKET_ERROR) {
			return -1;
		}

		for (size_t i = 0; i < socks.size(); ++i) {
			if (socks[i] != INVALID_SOCKET && FD_ISSET(socks[i], &read_set)) {
				ready.push_back(i);
			}
		}
		return sel;
	}

	// 소켓 하나의 수신 누적 — pending 은 아직 완성 안 된 잔여 바이트다.
	struct FrameTally {
		std::vector<uint8_t> pending;
		std::map<uint16_t, uint32_t> by_id;
		bool protocol_error = false;
	};

	// pending 뒤에 n 바이트를 붙이고 parse_frames 로 완성 프레임을 뽑아
	//   by_id 를 센 뒤, 소비한 바이트만큼 pending 앞을 지운다(잔여
	//   보존 — zone.ps1:73-79 Get-FrameStats 와 같은 [len:u16][id:u16]
	//   경계 규칙). protocol_error 가 이미 참이면 그 뒤로는 더 파싱하지
	//   않는다(parse_frames 의 "거기서 멈춘다" 계약을 다시 두드리지 않는다).
	inline void tally_feed(FrameTally& t, const uint8_t* p, size_t n) {
		t.pending.insert(t.pending.end(), p, p + n);
		if (t.protocol_error) {
			return;
		}

		std::vector<Frame> frames;
		bool protocol_error = false;
		const size_t consumed = parse_frames(t.pending.data(), t.pending.size(), frames, protocol_error);

		for (const Frame& f : frames) {
			++t.by_id[static_cast<uint16_t>(f.id)];
		}
		if (consumed > 0) {
			t.pending.erase(t.pending.begin(), t.pending.begin() + consumed);
		}
		if (protocol_error) {
			t.protocol_error = true;
		}
	}

	// PS 의 PartialBytes 대응.
	inline size_t tally_partial_bytes(const FrameTally& t) {
		return t.pending.size();
	}

	// by_id 만 비운다. ⚠️ PS 갈래는 Sink.SetLength(0) 으로 잔여 바이트까지
	//   버린다(zone.ps1:169) — 채팅 판정 시작 전 시점이라 지금은 그
	//   차이가 관측되지 않는다.
	inline void tally_reset_counts(FrameTally& t) {
		t.by_id.clear();
	}

	// zone.ps1:91,137 의 "zones<1 → 1" 대응 — zones==0 이면 나누지 않고
	//   zone_id 를 그대로 돌려준다.
	inline uint32_t zone_of(size_t index, uint32_t zone_id, uint32_t zones) {
		if (zones == 0) {
			return zone_id;
		}
		return zone_id + static_cast<uint32_t>(index % zones);
	}

	// zone.ps1:92-96 의 산식 — per_zone 은 정수 나눗셈([math]::Floor 와 같다).
	inline uint64_t expect_count(uint64_t clients, uint64_t zones, uint64_t chats) {
		if (zones == 0) {
			return 0;
		}
		const uint64_t per_zone = clients / zones;
		if (per_zone == 0) {
			return 0;
		}
		return per_zone * chats;
	}

	// 소켓 + 상태 하나. TcpClient 는 복사·이동이 없어(선언에 delete 됨)
	//   unique_ptr 로 든다(조사 A §2) — std::vector<Peer> 를 resize/push_back
	//   해도 TcpClient 인스턴스 자체는 옮겨 다니지 않는다.
	struct Peer {
		std::unique_ptr<TcpClient> client;
		FrameTally tally;
		uint32_t zone = 0;
		uint64_t player_id = 0;
		bool closed = false;
		bool error = false;
	};

	// ready(select_readable 이 돌려준, "열린 소켓만 모은 목록"의 인덱스)를
	//   peers 인덱스로 되돌린다 — open_index[slot] 로 매핑한다. 순수
	//   함수로 뺀 이유 — "일부 peer 가 닫힌" 입력 조합을 하네스가
	//   결정적으로 재현하기 어려워(타이밍에 좌우된다), selftest 가 소켓
	//   없이 이 매핑만 직접 두드릴 수 있어야 한다(7단계 코드 리뷰 test
	//   렌즈 MED — pump_once 안에 묻혀 있으면 이 경계를 못 잡는다).
	inline void remap_ready(const std::vector<size_t>& open_index, const std::vector<size_t>& ready, std::vector<size_t>& out) {
		out.clear();
		for (const size_t slot : ready) {
			out.push_back(open_index[slot]);
		}
	}

	// 열려 있는(!closed && client && native()!=INVALID_SOCKET) peer 의
	//   소켓만 모아 select_readable 을 한 번 부르고, 준비된 peer 마다
	//   recv_some(buf, cap, got, 0) 을 1회 부른다: kData → tally_feed ·
	//   kClosed → closed=true · kError → closed=true·error=true ·
	//   kTimeout → 무시.
	//
	//   ⚠️ select_readable 의 ready 는 "열린 소켓만 모은 목록"의 인덱스라
	//   peers 인덱스와 다르다 — remap_ready 로 되돌린다.
	//
	//   반환값: 이번 호출에서 데이터를 받은 peer 수(select 오류면 -1).
	inline int pump_once(std::vector<Peer>& peers, int timeout_ms, uint8_t* buf, size_t cap) {
		std::vector<SOCKET> socks;
		std::vector<size_t> open_index;	// socks[i] 에 대응하는 peers 인덱스
		socks.reserve(peers.size());
		open_index.reserve(peers.size());

		for (size_t i = 0; i < peers.size(); ++i) {
			const Peer& peer = peers[i];
			if (!peer.closed && peer.client && peer.client->native() != INVALID_SOCKET) {
				socks.push_back(peer.client->native());
				open_index.push_back(i);
			}
		}

		std::vector<size_t> ready;
		const int sel = select_readable(socks, timeout_ms, ready);
		if (sel < 0) {
			return -1;
		}

		std::vector<size_t> peer_idx;
		remap_ready(open_index, ready, peer_idx);

		int data_count = 0;
		for (const size_t pi : peer_idx) {
			Peer& peer = peers[pi];
			size_t got = 0;
			const RecvResult r = peer.client->recv_some(buf, cap, got, 0);
			switch (r) {
				case RecvResult::kData:
					tally_feed(peer.tally, buf, got);
					++data_count;
					break;
				case RecvResult::kClosed:
					peer.closed = true;
					break;
				case RecvResult::kError:
					peer.closed = true;
					peer.error = true;
					break;
				case RecvResult::kTimeout:
					break;
			}
		}
		return data_count;
	}

}	// namespace client
