//  client/cmd_send.cpp — `client send` (send.ps1 이식, T015-impl.md §4)
//
//  단일 스레드인 이유 — 이 클라 전체가 그렇듯(main.cpp 머리말) 타이머
//  스레드·수신 스레드를 따로 두지 않는다. --hold 중의 주기 ping 도 hold
//  루프 안에서 시간을 조각내어 같은 스레드가 보내고 받는다.
//
//  60,000B 예산 규칙(send.ps1:11-12 원문 — "Repeat*Size 를 60000 이상으로
//  올리지 말 것 … 클라이언트가 다 쓸 때까지 안 읽는 구조라 양쪽 버퍼가
//  차면 에코 데드락") — 이 클라도 "다 보낸 뒤 읽는다" 구조를 그대로
//  옮겼다(단일 스레드라 송신 루프가 끝나기 전에는 recv 를 시작하지
//  않는다). 그래서 같은 문턱이 그대로 적용된다. 여기서 usage 오류로
//  막지 않고 경고만 찍는 이유 — 실제 안전 상한은 이 프로세스가 아니라
//  서버/커널 소켓 버퍼 크기에 달려 있어 클라가 "이 값 이상은 무조건
//  죽는다"고 단정할 근거가 없다(T015-impl.md §4-1).
//
//  판정은 §4-3 표 그대로: --expect-close 면 "어느 단계에서든 상대가
//  먼저 끊었는가"만 보고 프레임/순서는 안 본다. 그 외에는 framed 면
//  packets/잔량/순서, raw 면 총 바이트 수로 판정한다. closed_seen 하나로
//  "수신 루프든 hold 루프든 상대가 끊긴 적 있는가"를 추적해 두 경로의
//  판정에 공통으로 쓴다. hold 루프의 read_frame 이 규약 위반(kError)을
//  돌려주면 closed_seen 과는 별개로 즉시 FAIL "hold error" 로 끝낸다 —
//  "상대가 끊었다"와 "상대가 규약을 어긴 응답을 보냈다"는 다른 사고라
//  같은 사유로 뭉치지 않는다(7단계 코드 리뷰 correctness MED-2 대응 —
//  §4-3 표에 추가된 행).
//
//  ⚠️ raw(--framed 없음) 모드는 이 서버(모든 클라 바이트를 프레임으로
//  강제 해석 — frame_router.cpp:1206-1211) 앞에서는 항상 절단된다.
//  echo_pattern 이 만드는 'A'(0x41) 두 개가 헤더로 읽히면 body_size=
//  0x4141=16705 로 4096 상한을 넘어 서버가 그 자리에서 끊는다 —
//  send.ps1 원본도 같은 서버 앞에서 Write 예외로 죽는 것을 실측으로
//  확인했다(교차검증, 6단계 Step 2 보고). send.ps1:1-12 헤더의 "뭉침 :
//  send.ps1 -Repeat 20 -Size 4" 예시는 프레이밍이 강제되기 이전 문서다
//  — 지금은 raw 모드를 --expect-close 와 함께 "프레이밍 없는 바이트가
//  들어오면 서버가 끊는가"를 보는 경계 시험 용도로만 쓴다.

#include "commands.h"
#include "frame_codec.h"
#include "tcp_client.h"

#include "proto/packet.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace client {

	namespace {

		// PowerShell 의 부울 보간(예: $Framed.IsPresent)은 "True"/"False"
		// (대문자)로 찍힌다 — send.ps1 원문과 나란히 읽을 수 있도록 같은
		// 대소문자를 쓴다(T015-impl.md 결정 4 — 출력 문구는 send.ps1 재현).
		const char* ps_bool(bool b) {
			return b ? "True" : "False";
		}

	}	// namespace

	int run_send(const Args& args) {
		const std::string host = args.get("host", "127.0.0.1");
		const int64_t port_raw = args.get_int("port", 9000, 0, 65535);
		const int64_t repeat = args.get_int("repeat", 3, 1, 100000000);
		// 65535 상한 — build_frame 이 body_size 를 static_cast<uint16_t>(len)
		// 으로 헤더에 넣는다. 65536 이상을 통과시키면 헤더 값과 실제
		// 길이가 조용히 어긋난 프레임이 만들어진다(7단계 코드 리뷰
		// correctness MED-1 — u16 랩어라운드).
		const int64_t size = args.get_int("size", 4, 0, 65535);
		const int64_t msg_id_raw = args.get_int("msg-id", 1, 0, 65535);
		const int64_t timeout64 = args.get_int("timeout", 1000, 0, 600000);
		const int64_t hold = args.get_int("hold", 0, 0, 86400);
		const int64_t split = args.get_int("split", 0, 0, 1000000);
		const int64_t split_ms = args.get_int("split-ms", 60, 0, 600000);
		const int64_t raw_size = args.get_int("raw-size", -1, -1, 65535);
		const int64_t read_delay = args.get_int("read-delay", 0, 0, 600000);
		const bool seq = args.has("seq");
		const bool nodelay = args.has("nodelay");
		const bool framed = args.has("framed");
		const bool drop_after_send = args.has("drop-after-send");
		const int64_t ping_ms = args.get_int("ping-ms", 0, 0, 600000);
		const bool expect_close = args.has("expect-close");

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
		// --seq 는 --framed 이고 size>=4 일 때만 뜻이 있다(본문 앞 4바이트에
		// 순번을 박으므로) — send.ps1:70 의 조건과 같지만, 그 스크립트는
		// 조건이 안 맞으면 조용히 순번을 안 박을 뿐이다. 이 클라는 그
		// 조합을 usage 오류로 승격한다(T015-impl.md §4-1) — 조용히 무시하면
		// "order : 안 찍힘"과 "안 써서 안 찍힘"을 사용자가 구분할 수 없다.
		if (seq && (!framed || size < 4)) {
			print_usage();
			return static_cast<int>(ExitCode::kUsage);
		}
		if (raw_size >= 0 && !framed) {
			print_usage();
			return static_cast<int>(ExitCode::kUsage);
		}

		const uint16_t port = static_cast<uint16_t>(port_raw);
		const proto::MsgId msg_id = static_cast<proto::MsgId>(msg_id_raw);
		const int timeout_ms = static_cast<int>(timeout64);

		if (repeat * (4 + size) >= 60000) {
			std::printf(
				"! 경고: repeat*(4+size)=%lld — 60000 이상이면 송수신 양쪽 버퍼가 "
				"차서 에코 데드락이 날 수 있다 (send.ps1:11-12)\n",
				static_cast<long long>(repeat * (4 + size)));
		}

		TcpClient client;
		if (!client.connect(host, port)) {
			result_line(false, "connect");
			return static_cast<int>(ExitCode::kConnect);
		}
		if (nodelay) {
			client.set_nodelay(true);
		}

		// ── 유닛 조립 ──────────────────────────────────────────────
		const std::vector<uint8_t> body = echo_pattern(static_cast<size_t>(size));
		std::vector<uint8_t> unit;
		if (framed) {
			unit = build_frame(msg_id, body.data(), body.size(), static_cast<int>(raw_size));
		} else {
			unit = body;
		}

		if (raw_size >= 0) {
			std::printf("raw   : body_size 를 %lld 로 위조 (실제 본문 %lld B)\n",
				static_cast<long long>(raw_size), static_cast<long long>(size));
		}

		const size_t unit_len = unit.size();
		const size_t expected = static_cast<size_t>(repeat) * unit_len;
		std::printf("send  : %lld x %zuB = %zu B   framed=%s  nodelay=%s\n",
			static_cast<long long>(repeat), unit_len, expected, ps_bool(framed), ps_bool(nodelay));

		if (seq) {
			std::printf("seq   : 본문 앞 4바이트에 순번을 박고, 돌아온 순서를 검사한다\n");
		}

		// ── 송신 루프 ──────────────────────────────────────────────
		bool send_failed = false;
		for (int64_t i = 0; i < repeat && !send_failed; ++i) {
			if (seq) {
				// --seq 검증(위)이 framed && size>=4 를 보장하므로 unit 은
				// 최소 header(4B)+body(4B) 이상이다 — unit[4..7] 은 항상 유효.
				proto::write_u32_be(unit.data() + proto::kHeaderSize, static_cast<uint32_t>(i));
			}

			if (split > 1) {
				const size_t split_count = static_cast<size_t>(split);
				const size_t chunk = (unit_len + split_count - 1) / split_count;	// ceil
				size_t off = 0;
				while (off < unit_len) {
					const size_t n = std::min(chunk, unit_len - off);
					if (!client.send_all(unit.data() + off, n)) {
						send_failed = true;
						break;
					}
					off += n;
					if (off < unit_len) {
						std::this_thread::sleep_for(std::chrono::milliseconds(split_ms));
					}
				}
			} else {
				if (!client.send_all(unit.data(), unit_len)) {
					send_failed = true;
				}
			}
		}
		// 판정부(§4-3)까지 살아남는 상태 — 정상 경로든 송신 중 절단
		// 경로든 전부 이 선언들을 거친다. 기본값은 "아직 아무 일도
		// 없었다"에 해당하는 값들이다.
		bool closed_seen = false;
		int reads = 0;
		std::vector<uint8_t> recvd;
		std::vector<Frame> frames;
		bool protocol_error = false;
		size_t leftover = 0;
		OrderCheck order{0, 0};
		std::vector<uint32_t> seqs;
		bool order_ok = true;	// --seq 미사용/seqs 없음일 때의 기본값 — 판정에서 seq && 로 먼저 가린다.
		int pongs = 0;

		// ── 송신 도중 실패 — 경합: 상대가 이미 끊었을 수 있다 ──────────
		//
		// 증상: raw(--framed 없음) 모드에서 --repeat > 1 이면 송신 루프가
		//   유닛마다 send_all 을 따로 부른다(위 루프). 서버가 이 클라의 첫
		//   유닛만 보고 프레임 위반으로 절단(RST — frame_router.cpp:1206-
		//   1211)하면, 로컬 커널이 그 RST 를 먼저 받아 두 번째 이후의
		//   send_all 이 WSAECONNRESET 으로 실패할 수 있다(loopback 이라
		//   개연성이 충분하다 — 7단계 코드 리뷰 R2 correctness HIGH, 2회
		//   실측은 우연히 통과했을 뿐인 레이스였다).
		// 원인: send_failed 를 "연결 실패"로만 취급하면, --expect-close 로
		//   "서버가 먼저 끊는가"를 보려는 시나리오(S8)에서 정확히 그 일이
		//   일어났는데도 FAIL 로 뒤집힌다 — recv 쪽 kClosed 와 같은 사실이
		//   send 쪽에서는 사라지기 때문이다.
		// 지금 코드: TcpClient::last_send_error() 로 WSAGetLastError() 를
		//   돌려받아, WSAECONNRESET/WSAECONNABORTED(tcp_client.h 머리말의
		//   MS Learn 인용 — "상대가 이미 끊었다")면 --expect-close 일 때만
		//   closed_seen 을 세우고 판정부로 직행한다. 그 외(예: expect-close
		//   가 아닌데 송신이 실패한 경우)는 기존대로 FAIL "send".
		if (send_failed) {
			const int send_err = client.last_send_error();
			const bool peer_closed_on_send = (send_err == WSAECONNRESET || send_err == WSAECONNABORTED);
			if (expect_close && peer_closed_on_send) {
				std::printf("  (peer closed during send — WSAGetLastError=%d)\n", send_err);
				closed_seen = true;
				// 수신 루프·framed 복원·hold 는 건너뛴다 — 소켓은 이미
				// 죽었다. 아래 if (!send_failed) 블록이 전부 스킵된다.
			} else {
				result_line(false, "send");
				return static_cast<int>(ExitCode::kFail);
			}
		}

		if (!send_failed) {
			// ── read-delay ────────────────────────────────────────
			if (read_delay > 0) {
				std::printf("delay : %lld ms 동안 읽지 않는다 (서버 송신을 일부러 밀리게 한다)\n",
					static_cast<long long>(read_delay));
				std::this_thread::sleep_for(std::chrono::milliseconds(read_delay));
			}

			// ── drop-after-send ─────────────────────────────────────
			if (drop_after_send) {
				std::printf("drop  : RST 로 즉시 종료 — 응답을 받지 않는다\n");
				client.close_rst();
				result_line(true, nullptr);
				return static_cast<int>(ExitCode::kPass);
			}

			// ── 수신 루프 ────────────────────────────────────────────
			recvd.reserve(expected);
			uint8_t buf[65536];

			while (recvd.size() < expected) {
				size_t got = 0;
				const RecvResult r = client.recv_some(buf, sizeof(buf), got, timeout_ms);
				if (r == RecvResult::kData) {
					++reads;
					std::printf("  read #%d : %zu bytes\n", reads, got);
					recvd.insert(recvd.end(), buf, buf + got);
				} else if (r == RecvResult::kTimeout) {
					std::printf("  (read timeout — 더 올 게 없음)\n");
					break;
				} else if (r == RecvResult::kClosed) {
					std::printf("  (peer closed)\n");
					closed_seen = true;
					break;
				} else {
					std::printf("  (recv error)\n");
					break;
				}
			}
			std::printf("total : %zu B in %d reads   (expected %zu B)\n", recvd.size(), reads, expected);

			// ── framed 복원 ──────────────────────────────────────────
			if (framed) {
				const size_t consumed = parse_frames(recvd.data(), recvd.size(), frames, protocol_error);
				leftover = recvd.size() - consumed;

				for (size_t idx = 0; idx < frames.size() && idx < 3; ++idx) {
					std::printf("  frame #%zu : id=%u body=%zu\n",
						idx + 1, static_cast<unsigned>(frames[idx].id), frames[idx].body.size());
				}
				if (leftover > 0) {
					std::printf("  ! 불완전 프레임 — %zu B 남음 (다음 recv 를 기다려야 하는 상황)\n", leftover);
				}
				std::printf("frames: %zu packets   (expected %lld)\n", frames.size(), static_cast<long long>(repeat));

				if (seq) {
					for (const Frame& f : frames) {
						if (f.body.size() >= 4) {
							seqs.push_back(proto::read_u32_be(f.body.data()));
						}
					}
					if (!seqs.empty()) {
						order = check_order(seqs);
						// 출력용과 판정용(§4-3)이 같은 조건을 따로 계산하면 한쪽만
						// 고치는 리팩터 사고가 난다 — 한 번 계산해 둘 다 이 값을 쓴다
						// (7단계 코드 리뷰 test 추가 발견).
						order_ok = (order.mismatch == 0 && order.dup == 0 && seqs.size() == static_cast<size_t>(repeat));
						if (order_ok) {
							std::printf("order : OK — %zu 개가 0..%lld 순서대로\n",
								seqs.size(), static_cast<long long>(repeat - 1));
						} else {
							std::printf("order : X  어긋남 %zu 개 · 중복 %zu 개 · 받은 개수 %zu/%lld\n",
								order.mismatch, order.dup, seqs.size(), static_cast<long long>(repeat));
							std::string joined;
							for (size_t idx = 0; idx < seqs.size() && idx < 24; ++idx) {
								if (idx > 0) {
									joined += ",";
								}
								joined += std::to_string(seqs[idx]);
							}
							std::printf("        받은 순번(앞 24): %s\n", joined.c_str());
						}
					}
				}
			}

			// ── hold ─────────────────────────────────────────────────
			if (hold > 0) {
				std::printf("hold  : %lld 초 동안 연결 유지 — ping-ms=%lld\n",
					static_cast<long long>(hold), static_cast<long long>(ping_ms));

				const auto hold_start = std::chrono::steady_clock::now();
				const auto hold_end = hold_start + std::chrono::seconds(hold);
				auto next_ping = hold_start;
				bool hold_closed = false;
				bool hold_error = false;

				while (!hold_closed && !hold_error && std::chrono::steady_clock::now() < hold_end) {
					if (ping_ms > 0) {
						const auto now = std::chrono::steady_clock::now();
						if (now < next_ping) {
							std::this_thread::sleep_for(std::chrono::milliseconds(10));
							continue;
						}
						next_ping = now + std::chrono::milliseconds(ping_ms);

						const std::vector<uint8_t> ping_frame = build_frame(proto::MsgId::kPingReq, nullptr, 0);
						if (!client.send_all(ping_frame.data(), ping_frame.size())) {
							hold_closed = true;
							break;
						}

						// tcp_client.h 의 공용 read_frame 을 쓴다 — cmd_flow.cpp 와
						// 같은 함수라 body_size > kMaxBodySize 규약 위반도 여기서
						// 걸린다(예전엔 이 루프만 그 상한 검사가 빠져 있었다 —
						// 7단계 코드 리뷰 correctness MED-2).
						Frame pong_frame;
						const RecvResult pr = read_frame(client, pong_frame, timeout_ms);
						if (pr == RecvResult::kClosed) {
							hold_closed = true;
							break;
						}
						if (pr == RecvResult::kError) {
							// 규약 위반 등 — kClosed 와는 다른 사유("hold error")로
							// 끝낸다(§4-3 표에 추가된 행).
							hold_error = true;
							break;
						}
						if (pr != RecvResult::kData) {
							// kTimeout — 헤더가 늦게 온 것만으로 연결이 죽었다고
							// 단정하지 않고 다음 ping 주기로 넘어간다.
							continue;
						}
						if (pong_frame.id == proto::MsgId::kPongAck) {
							++pongs;
						}
						// 다른 id 는 무시하고 계속(§4-2).
					} else {
						const auto now = std::chrono::steady_clock::now();
						const auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(hold_end - now).count();
						if (remaining_ms <= 0) {
							break;
						}
						const int wait_ms = static_cast<int>(std::min<long long>(500, remaining_ms));
						uint8_t watch_buf[64];
						size_t got = 0;
						const RecvResult wr = client.recv_some(watch_buf, sizeof(watch_buf), got, wait_ms);
						if (wr == RecvResult::kClosed) {
							hold_closed = true;
							break;
						}
					}
				}

				const auto hold_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - hold_start).count();
				if (hold_closed) {
					std::printf("hold  : closed by server after %lld ms\n", static_cast<long long>(hold_elapsed_ms));
					closed_seen = true;
				} else if (hold_error) {
					std::printf("hold  : protocol error after %lld ms\n", static_cast<long long>(hold_elapsed_ms));
				}
				std::printf("hold  : done pongs=%d closed=%s\n", pongs, hold_closed ? "true" : "false");

				if (hold_error) {
					client.close_graceful();
					result_line(false, "hold error");
					return static_cast<int>(ExitCode::kFail);
				}
			}
		}

		client.close_graceful();

		// ── 판정 (§4-3) ───────────────────────────────────────────
		bool pass = false;
		std::string reason;

		if (expect_close) {
			if (closed_seen) {
				pass = true;
			} else {
				reason = "not closed";
			}
		} else if (framed) {
			const bool packets_ok = !protocol_error && leftover == 0 && frames.size() == static_cast<size_t>(repeat);
			if (!packets_ok) {
				reason = "frames " + std::to_string(frames.size()) + "/" + std::to_string(repeat);
			} else if (seq && !order_ok) {
				reason = "order mismatch=" + std::to_string(order.mismatch) + " dup=" + std::to_string(order.dup);
			} else if (closed_seen) {
				reason = "closed by server";
			} else {
				pass = true;
			}
		} else {
			// raw — "frames <n>/<m>" 과 같은 결로 "total <n>/<m>" 을 사유로
			// 쓴다(§4-3 표는 raw 실패 문구를 별도로 못박지 않는다).
			if (recvd.size() != expected) {
				reason = "total " + std::to_string(recvd.size()) + "/" + std::to_string(expected);
			} else if (closed_seen) {
				reason = "closed by server";
			} else {
				pass = true;
			}
		}

		result_line(pass, reason.empty() ? nullptr : reason.c_str());
		return pass ? static_cast<int>(ExitCode::kPass) : static_cast<int>(ExitCode::kFail);
	}

}	// namespace client
