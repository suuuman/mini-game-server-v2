//  proto/packet.h — 패킷 헤더 규약과 바이트 순서 변환
//
//  이 파일이 클라이언트와 서버가 합의한 유일한 규약이다.
//  여기 있는 함수 밖에서 바이트를 직접 만들기 시작하면 프로토콜이 갈라진다.

#pragma once

#include <cstdint>
#include <cstddef>

namespace proto {
	//  메시지 ID
	//
	//  enum class 인 이유 — 일반 enum 은 int 로 암묵 변환돼서, 길이나 인덱스 같은
	//                       다른 숫자와 뒤섞여도 컴파일러가 잡아주지 않는다.
	//  기반형을 못박은 이유 — 이 값이 그대로 와이어에 나가므로 크기가 흔들리면 안 된다.
	//
	//  요청은 1번대, 응답·통지는 101번대. 로그에서 방향이 바로 보이고,
	//  「클라가 응답 ID 를 보내면 거른다」는 검사가 is_client_request 한 줄로 끝난다.
	enum class MsgId : uint16_t {
		// 요청 — 클라이언트가 보낸다
		kEchoReq = 1,
		kJoinZoneReq = 2,     // body: [ zone_id : u32 ]
		// type 마다 새 MsgId 를 만들지 않고 하나로 묶는다 — 타입별로 id 를
		// 쪼개면 채팅 종류가 늘 때마다 표 세 곳(is_client_request·
		// is_server_message·on_frame 스위치)에 매번 등재해야 하고, 그중
		// 하나라도 빠뜨리면 그 종류만 조용히 안 불린다(절대 규칙 7 의 함정).
		kChatReq = 3,     // body: [ type:u8 ][ target:u64 — type=2(Whisper) 만 ][ text... ]
				  //   type: 0=Zone · 1=All · 2=Whisper. 그 밖은 프로토콜
				  //   위반(절단)이다 — 클라 버전이 서버보다 앞선 것이
				  //   아니라 조립 결함으로 본다. text 는 빈 것도 유효하다.

		// body 가 없다. player_id 를 받으면 아무나 남의 인벤토리를 조회한다 —
		//   조회 대상은 클라이언트가 정하는 게 아니라 「이 세션이 누구인가」로 정해진다.
		kInventoryReq = 4,     // body: 없음 (세션의 player_id 를 쓴다)

		// 5 는 kLoginReq 폐지로 비었다 — 재사용하지 않는다. 폐기된 번호를 새 값에
		//   다시 매기면 조용한 이중 의미가 될 위험이 있다(ADR-020 —
		//   kSessionLoginAck 를 kZoneMembersNtf 와 같은 값으로 잘못 매길 뻔한
		//   사고와 같은 종류다).

		// 상대를 session_id 로 지목한다 (player_id 가 아니라).
		// 거래는 같은 존 안에서만 되고, 존이 아는 건 세션이기 때문이다.
		// 존 안 지목은 session_id · DB 는 player_id — 둘이 만나는 곳이 세션이다.
		kTradeReqReq = 6,     // body: [ to_session : u32 ]
		kTradeAnswerReq = 7,     // body: [ from_session : u32 ][ accept : u8 ]

		// 자기 아이템은 자기만 올린다 — 상대 것을 지정하는 필드가 아예 없다.
		//   "너의 100번 3개를 내놔"는 협상이 아니라 통보고, 상대는 통째로 거절하는 것
		//   말고 할 수 있는 게 없다. 권한 경계가 메시지 모양에 그대로 드러나야 한다.
		//   count = 0 이면 슬롯을 내린다. 한쪽이 아무것도 안 올리면 그게 일방 증여다.
		kTradeSetItemReq = 8,     // body: [ item_id : u32 ][ count : u32 ]

		// 토글이다 (확인 / 확인 취소). 누른 뒤에도 마음을 바꿀 수 있어야 한다.
		kTradeConfirmReq = 9,     // body: [ confirm : u8 ]

		// 거절과 취소를 한 메시지로 합친다. 양쪽 다 쓸 수 있고 하는 일이 같다 —
		// 거래를 지우고 상대에게 알린다. 방향이 다르다고 메시지를 나눌 이유가 없다.
		// body 가 빈 것은 세션당 거래가 하나라 「어느 거래인지」를 말할 필요가 없어서다.
		kTradeCancelReq = 10,    // body: 없음

		// kEchoReq 와 뜻이 다르다. 에코는 「왕복이 되는가」를 재는 개발용이고,
		//   이건 「이 연결이 아직 살아 있는가」를 서버에 알리는 운영용이다.
		//   body 를 안 싣는 이유가 그 차이다 — 에코는 실어 보낸 것을 되받아야
		//   의미가 있지만, ping 은 「왔다」는 사실만으로 할 일이 끝난다.
		//
		//  이 값이 하는 실제 일은 세션의 last_recv_ms 를 미는 것인데,
		//    그건 net 이 수신 완료 지점에서 이미 한다. 그래서 이 핸들러는
		//    「아무 일도 안 하고 답만 한다」가 맞다 — 여기서 뭔가 더 하고 싶어지면,
		//    그건 ping 이 아니라 다른 메시지다.
		kPingReq = 11,    // body: 없음

		// 세션 서버(로그인 분배)로 보내는 로그인이다. player_id 를 u64 로 받는
		// 이유 — 세션 서버 규약이 u64 로 시작했고, 마을 와이어의 나머지 u32
		// 필드와 폭을 맞추는 통일은 3단계로 미뤘다(ADR-020 결정 4). 이 값은
		// is_client_request 에 넣지 않는다 — 마을이 처리해 주는 요청이 아니다
		// (is_session_client_request 참조).
		kSessionLoginReq = 12,    // body: [ player_id : u64 ]

		// 세션 서버가 배정한 마을에 클라이언트가 직접 붙여 보내는 입장 요청이다.
		// player_id 가 u64 인 이유 — 이 값은 세션 서버가 Reserve 로 먼저 마을에
		// 넘겨 둔 예약과 같은 키로 맞아떨어져야 하는데, 그 예약이
		// kSessionLoginReq 규약(u64)으로 발급된다. 여기서 u32 로 낮추면 예약
		// 조회 지점에서 다시 넓혀야 해서 변환 지점만 하나 느는 것이라 얻는
		// 게 없다. 그렇다고 마을 와이어의 나머지 u32 필드까지 u64 로 통일하지는
		// 않는다(ADR-021 결정 8) — kZoneMembersNtf 를 넓히면 정원이 511 명에서
		// 341 명으로 줄고 기존 하네스의 고정 길이 파싱이 깨진다.
		//
		// is_client_request 에 넣고 is_session_client_request 에는 넣지 않는다 —
		// Enter 는 세션 서버가 아니라 마을이 처리하는 요청이다.
		kEnterReq = 13,    // body: [ player_id : u64 ]

		// 응답·통지 — 서버가 보낸다
		kEchoAck = 101,
		kJoinZoneAck = 102,   // body: [ zone_id : u32 ][ member_count : u32 ]
		// from 을 session_id 가 아니라 player_id 로 싣는다 — 상대를 특정해
		// 다시 말을 걸어야 하는 경로(거래 지목)는 이미 kZoneMembersNtf 가
		// session_id/player_id 쌍을 정본으로 내려주고 있어(kZoneMembersNtf
		// 주석 참조) 이 통지가 그 역할을 겸할 필요가 없고, 귓속말은 받은
		// from 을 그대로 target 에 되돌려 회신해야 해서 두 필드가 같은
		// 종류여야 한다.
		kChatNtf = 103,   // body: [ type:u8 ][ from:u64 = player_id ][ text... ]

		// item_count 는 부호 있는 값을 비트 그대로 실어 보내고 읽는 쪽이 int32 로 해석한다.
		// 스키마가 UNSIGNED 가 아닌 이유는 sql\01_schema.sql 주석 참조 —
		// 언더플로를 큰 수로 바꾸지 않기 위해서다.
		kInventoryAck = 104,  // body: [ result : u8 ][ count : u16 ]( [ item_id : u32 ][ item_count : i32 ] × count )

		// 105 는 kLoginAck 폐지로 비었다 — 재사용하지 않는다. 이유는 5 번과 같다.

		// from 은 서버가 채운다 — 요청자가 자기를 사칭할 수 없다.
		kTradeReqNtf = 106,     // body: [ from_session : u32 ]
		kTradeOpenNtf = 107,     // body: [ peer_session : u32 ]   거래창이 열렸다

		// 상태를 「전부」 보낸다. 바뀐 것만 보내면 패킷 하나만 어긋나도 양쪽 화면이
		//   갈라지고, 그 상태로 확인을 누르면 "내가 본 것과 다른 게 거래됐다"가 된다.
		//   전체를 보내면 매번 덮어쓰므로 언제나 일치한다.
		//   my / peer 로 보내면 받는 쪽이 자기 기준으로 바로 그린다 —
		//   a / b 로 보내면 클라이언트가 "내가 a 인가 b 인가"를 매번 따져야 한다.
		kTradeStateNtf = 108,     // body: [ my_item : u32 ][ my_count : u32 ][ my_confirm : u8 ]
								 //       [ peer_item : u32 ][ peer_count : u32 ][ peer_confirm : u8 ]

		// 결과는 양쪽에 같은 것을 보낸다. 제안자와 수락자가 서로 다른 결과를 보면
		//   그때부터 화면이 갈라진다 — 한쪽만 아이템이 사라진 것처럼 보인다.
		//
		// 잔량을 안 싣는 이유: 양방향이라 「누구의 잔량」인지 애매하고, 커밋이 끝난 뒤
		// 다시 읽으면 왕복이 는다. 트랜잭션 안에서 읽으면 잠근 행을 그만큼 더 붙든다.
		// → 성공하면 클라이언트가 kInventoryReq 로 다시 본다.
		//   실제 게임은 갱신을 푸시한다. 그건 한계다.
		kTradeAck = 109,      // body: [ result : u8 ][ peer_session : u32 ]

		// 상대가 취소했다는 통지. kTradeAck 로 뭉치지 않는 이유 —
		// 취소는 「거래가 실패한 것」이 아니라 「거래가 시작되지 않은 것」이고 DB 를 타지도
		// 않았다. 같은 코드로 묶으면 그 구분이 사라진다.
		kTradeCancelNtf = 110,    // body: [ peer_session : u32 ]

		// 답이 필요한 이유 — 서버는 ping 을 안 받아도 되지만 클라이언트는 답을
		//   받아야 한다. 「내가 보낸 것이 도착했는가」는 보낸 쪽에서 알 수 없다.
		//   TCP 의 ACK 는 커널이 받았다는 뜻이지 서버 프로세스가 살아 있다는 뜻이 아니다 —
		//   응답 큐가 막혀 아무 일도 못 하는 서버도 커널은 ACK 를 낸다.
		//   그래서 「응용 계층까지 돌았다」를 말해 주는 값이 따로 있어야 한다.
		kPongAck = 111,    // body: 없음

		// 이게 없으면 거래를 시작할 수 없었다. kTradeReqReq 가 상대를 session_id 로
		// 지목하는데 그 값을 아는 경로가 kChatNtf 하나뿐이라, 말을 걸지 않은 사람과는
		// 거래를 못 했다. 존이 아는 것이 세션이므로 존이 목록을 내려주는 것이 제자리다.
		//
		// 델타가 아니라 전체 목록을 보낸다. 하나만 놓쳐도 없는 사람에게 거래를 걸거나
		// 있는 사람이 안 보인다. 존 인원이 작아서 지금은 비용이 문제가 안 되지만,
		// 커지면 뒤집힌다 — N명 변동마다 N명에게 N개를 보내 N² 가 된다.
		//
		// player_id 를 같이 싣는 것은 session_id 는 지목에, player_id 는 표시에 쓰기
		// 때문이다. 로그인 전이면 0 이고 그 0 자체가 정보다. 와이어는 u32 인데,
		// 로그인 규약이 이미 u32 라 폭을 맞춘 것이다.
		//
		// 상한은 511 명. 넘으면 잘라 보내고 경고를 남긴다 — 인벤토리와 같은 방식이다.
		//
		// 한계 — 존에 있는 채로 로그인하면 그 사람의 player_id 가 낡은 채로 남는다.
		// 이 통지는 멤버가 드나들 때만 나가는데 로그인은 존 소속과 무관하게 처리되기
		// 때문이다. 다음 사람이 드나들 때 갱신되고, 거래는 session_id 로 지목하므로
		// 틀리는 것은 표시뿐이다. 로그인 지점에서 브로드캐스트하려면 그 핸들러가 존을
		// 알아야 하고 이동 중이면 아직 안 들어간 존에 보내게 되어 얻는 것보다 비싸다.
		kZoneMembersNtf = 112,   // body: [ count : u16 ]( [ session_id : u32 ][ player_id : u32 ] × count )

		// 112 가 아니라 113 인 이유 — 112 는 kZoneMembersNtf 가 이미 쓴다. enum 의
		// 중복값은 컴파일러가 잡아주지 않고, is_server_message 의 || 체인에서 두
		// 이름이 한 값으로 겹쳐 조용히 오판정된다. 응답 대역은 101~112 가 전부
		// 사용 중이라 113 이 다음 빈 값이다.
		//
		// host 는 u16 길이(바이트 수) 선행 + UTF-8 — s2s 의 encode_str16 과 같은
		// 규약이다(ADR-019 결정 7). 가변 필드라 마지막에 둔다.
		kSessionLoginAck = 113,   // body: [ result : u8 ][ port : u16 ][ host : str16 ]

		// session_id 는 성공·거절 어느 쪽이든 실값이다 — 거래처럼 상대를
		// session_id 로 지목하는 흐름이 이후에도 있어, 실패해도 자기 id 는 알아야
		// 한다. player_id 는 거절이면 0 이다 — kTradeAck 류처럼 「서버가 인정한
		// 값만 싣는다」는 관례를 따른다(클라가 자기 상태를 서버 기준으로 맞춘다).
		kEnterAck = 114,   // body: [ result : u8 ][ player_id : u64 ][ session_id : u32 ]

		// 성공은 무응답이다 — kTradeAck 선례("성공 → 상태 통지가 증명한다")와
		// 같은 이유로, 이 값은 실패했을 때만 나간다. 귓속말 대상 부재는
		// kNoPeer 를 그대로 쓴다 — "상대가 없다"는 뜻이 거래와 채팅에서 같다.
		kChatAck = 115,   // body: [ result : u8 ]
	};

	// 응답 결과 코드. 실패와 빈 결과는 반드시 구분해야 한다 — 질의가 실패했는데 빈
	// 목록을 돌려주면 클라이언트는 「아이템이 없다」로 받고, 유저 입장에선 아이템이
	// 사라진 것이 된다.
	//
	// 실패를 더 나누는 기준은 분류가 아니라 클라이언트의 반응이다.
	//   kNotEnough   정상적인 게임 결과다
	//   kInvalidArg  클라이언트가 잘못 보냈다. 재시도해도 같다
	//   kBusy        서버가 지금 못 받는다. 재시도가 의미 있다
	//   kDbError     장애다. 운영이 알아야 한다
	enum class ResultCode : uint8_t {
		kOk          = 0,
		kDbError     = 1,
		kNotEnough   = 2,   // 잔량 부족 — 조건부 UPDATE 가 0행이었다
		kInvalidArg  = 3,   // 자기 자신과 거래 · count 상한 초과 · body 길이 불일치
		kBusy        = 4,   // 이미 거래 중 · DB 큐가 가득 · 데드락 재시도를 다 썼다
		kNotLoggedIn = 5,   // 로그인 전에 조회·거래를 시도했다

		// 「상대가 없다」와 「거래가 없다」를 한 코드로 묶는다. 둘 다 정상 상황이고
		// (방금 나갔다 · 이미 취소됐다 · 아직 안 열렸다) 클라이언트가 할 일이 같다.
		kNoPeer      = 6,
	};

	// kSessionLoginAck 의 result. 마을 응답의 ResultCode 와 공간을 섞지 않는 이유 —
	// 둘 다 와이어 규약이라 한쪽의 개편이 다른 쪽 값을 흔들면 안 되고, 세션 서버의
	// 실패 사유(배정할 서버가 없다 등)는 마을 것과 겹치지 않는다.
	// busy 는 중복 로그인 차단(동기 Kick)의 즉시 거절 응답이다 — 먼저 붙은 세션에
	// Kick 을 보낸 뒤 이 값으로 거절하고, 클라 재시도가 정리 완료 후 통과한다.
	constexpr uint8_t kSessionLoginOk = 0;
	constexpr uint8_t kSessionLoginNoServer = 1;
	constexpr uint8_t kSessionLoginBusy = 2;

	// 한 번에 옮길 수 있는 수량 상한 — 방어의 1층이다. 게임 디자인이 정할 값인데 아직
	// 그 디자인이 없어서, 정확한 값이 아니라 터무니없는 값을 걸러내는 용도로만 둔다.
	// 여기서 거르는 것은 응답을 빨리 돌려주기 위해서고, 최종 방어는 DB 다.
	//   2층  조건부 UPDATE — 음수로 내려가는 것을 막는다
	//   3층  STRICT_TRANS_TABLES — INT 상한을 넘는 덧셈을 거절한다
	// 3층은 CHECK 제약이 아니라 sql_mode 가 하므로 db_conn.h 가 연결마다 못 박는다.
	constexpr uint32_t kMaxTradeCount = 1'000'000;

	//  ID 판정 — 「끊을 값인가」가 아니라 「무슨 값인가」만 답한다.
	//
	//  처분(무시할지 끊을지)은 여기서 정하지 않는다. 그건 정책이고, 정책은
	//    세션 상태를 아는 위층의 몫이다. 여기는 분류만 한다.
	//
	//  예전에는 이 함수가 프레임 자르기(frame_size) 안에서 불렸고,
	//    false 면 그 자리에서 연결을 끊었다. 그게 틀렸던 이유 —
	//    길이 선행 프레이밍이라 msg_id 를 몰라도 4+body_size 만큼 정확히
	//    건너뛸 수 있다. 즉 스트림 동기화가 안 깨지는데도 끊고 있었다.
	//    클라 버전 롤아웃 중 구버전 서버에 붙은 유저가 전부 튕기는 사고가 난다.

	// 클라가 보낼 수 있는 것 — 서버가 처리해 주는 요청들이다.
	//
	// 마을은 이 함수를 부르지 않는다. on_frame 이 msg_id 를 switch 로 직접
	//   가르고, 그 스위치에 없는 것을 handle_unhandled 가 받는다. 그래서 이
	//   표는 마을 동작에 영향을 주지 않는다 — 지금은 「마을이 무엇을 처리해
	//   주는가」를 한 곳에 모아 보여 주는 목록이고, 세션 서버 쪽 짝인
	//   is_session_client_request 가 session_router 에서 실제로 불린다.
	//   ⚠️ 그래서 여기에 새 요청을 넣는 것만으로는 아무 일도 일어나지 않는다.
	//   실제 처리는 on_frame 의 스위치에 넣어야 생긴다.
	constexpr bool is_client_request(MsgId id) {
		return id == MsgId::kEchoReq
			|| id == MsgId::kJoinZoneReq
			|| id == MsgId::kChatReq
			|| id == MsgId::kInventoryReq
			|| id == MsgId::kTradeReqReq
			|| id == MsgId::kTradeAnswerReq
			|| id == MsgId::kTradeSetItemReq
			|| id == MsgId::kTradeConfirmReq
			|| id == MsgId::kTradeCancelReq
			// ping 을 어느 표에서든 빠뜨리면 증상이 고약하다 —
			//   ping 이 「미정의 ID」로 떨어져 bad_msg_score 가 30초마다 1씩 오르고,
			//   kBadMsgLimit(32) 에 닿는 16분 뒤에 멀쩡한 세션이 끊긴다.
			//   「16분마다 끊긴다」에서 원인이 ping 판정 표라는 데까지 가는 길이 멀다.
			//   실제로 그 일이 나는 곳은 이 표가 아니라 session_router 가 부르는
			//   is_session_client_request 쪽이다.
			|| id == MsgId::kPingReq
			|| id == MsgId::kEnterReq;
	}

	// 세션 서버가 처리해 주는 클라 요청 — kSessionLoginReq 는 여기에만 있다.
	// 「새 요청 MsgId 는 is_client_request 에 반드시 넣는다」는 규칙의 승인된
	// 예외다(ADR-020 결정 4) — 마을에 kSessionLoginReq 가 올라오면 그건 잘못
	// 접속한 클라이언트지 정상 요청이 아니고, 마을이 이걸 「정상」으로 받아 주면
	// 두 서버의 요청 표가 한 방향으로 섞이기 시작한다(역방향 오염).
	// kPingReq 가 양쪽 표에 다 있는 것은 연결 생존 신호가 어느 서버에나
	// 필요하기 때문이다.
	constexpr bool is_session_client_request(MsgId id) {
		return id == MsgId::kSessionLoginReq
			|| id == MsgId::kPingReq;
	}

	// 서버만 보내는 것 — 이게 올라오면 정상 클라이언트가 아니다.
	//
	// is_client_request 의 부정이 아니라 셋으로 나뉜다. 클라 요청은 처리하고, 서버
	// 전용은 방향이 뒤집힌 값이고, 그 밖은 아직 모르는 ID 라 신버전 클라일 수 있다.
	// 뒤의 둘은 둘 다 처리 못 하지만 의심의 무게가 달라서, 위층이 가중치를 달리 줄 수
	// 있게 나눠 둔다.
	//
	// 나열이라 새 응답·통지를 넣을 때 여기도 넣어야 한다. 범위 검사로 하면 빠뜨릴 일은
	// 없지만 번호를 건너뛰거나 대역을 넓히는 순간 조용히 틀린다. 빠뜨려도 미정의로
	// 떨어져 덜 무거운 쪽으로 실패하므로 나열을 택했다.
	constexpr bool is_server_message(MsgId id) {
		return id == MsgId::kEchoAck
			|| id == MsgId::kJoinZoneAck
			|| id == MsgId::kChatNtf
			|| id == MsgId::kInventoryAck
			|| id == MsgId::kTradeReqNtf
			|| id == MsgId::kTradeOpenNtf
			|| id == MsgId::kTradeStateNtf
			|| id == MsgId::kTradeAck
			|| id == MsgId::kTradeCancelNtf
			|| id == MsgId::kPongAck
			|| id == MsgId::kZoneMembersNtf
			|| id == MsgId::kSessionLoginAck
			|| id == MsgId::kEnterAck
			|| id == MsgId::kChatAck;
	}

	//  헤더 — [ body_size : u16 ][ msg_id : u16 ][ body ... ]   빅엔디언
	//
	//  이 구조체를 그대로 send/recv 하지 않는다. 컴파일러가 패딩을 넣을 수 있고,
	//    x86 은 리틀엔디언이라 메모리 배치가 와이어 규약과 반대다.
	//    그래서 sizeof 대신 kHeaderSize 를 따로 둔다 —
	//    「메모리에서의 모습」과 「선을 타고 가는 모습」을 분리하는 것이 직렬화다.
	struct PacketHeader {
		uint16_t body_size;  // 본문 길이
		MsgId msg_id;     // 메시지 ID
	};

	constexpr size_t kHeaderSize = 4;	// sizeof 가 아니라 규약이 정한 값

	//  본문 상한
	//
	//  이건 규약이지 버퍼 사정이 아니다.
	//    "버퍼가 4096이니까 본문은 4092까지"로 정하면 구현 사정이 프로토콜을 정하게 된다.
	//    상한을 먼저 정하고 버퍼가 그걸 담도록 만드는 것이 순서다.
	//
	//  4096 인 근거는 약하다 — 실제로 필요한 최대치를 아직 모른다.
	//  「메모리로 감당되는, 넉넉한 쪽의 작은 값」으로 잡았다: 4100B × 동시접속 1만 = 41MB.
	//  실제 메시지 크기가 나오면 다시 정한다.
	constexpr uint16_t kMaxBodySize = 4096;

	//  바이트 순서 변환 — 빅엔디언(네트워크 바이트 오더)
	//
	//  htons/ntohs 를 안 쓴 이유 — winsock 헤더에 의존하지 않기 위해서다.
	//  proto 는 net 을 모른다 (의존 방향 world → proto → net → core).
	//  시프트는 호스트가 어느 엔디언이든 같은 결과를 낸다. 메모리 배치가 아니라
	//  「값의 상위 8비트」를 다루기 때문이다.
	inline void write_u16_be(uint8_t* dst, uint16_t value) {
		dst[0] = static_cast<uint8_t>((value >> 8) & 0xFF);		// 상위 바이트가 먼저
		dst[1] = static_cast<uint8_t>(value & 0xFF);
	}

	inline uint16_t read_u16_be(const uint8_t* src) {
		// uint8_t 는 산술 연산에서 int 로 정수 승격된다. 바깥 캐스팅을 빼면
		//   int → uint16_t 축소 변환 경고가 Level4 에서 뜬다.
		return (static_cast<uint16_t>(src[0]) << 8) | static_cast<uint16_t>(src[1]);
	}

	inline void write_u32_be(uint8_t* dst, uint32_t value) {
		dst[0] = static_cast<uint8_t>((value >> 24) & 0xFF);
		dst[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
		dst[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
		dst[3] = static_cast<uint8_t>(value & 0xFF);
	}

	inline uint32_t read_u32_be(const uint8_t* src) {
		return (static_cast<uint32_t>(src[0]) << 24)
			| (static_cast<uint32_t>(src[1]) << 16)
			| (static_cast<uint32_t>(src[2]) << 8)
			| static_cast<uint32_t>(src[3]);
	}

	inline void write_u64_be(uint8_t* dst, uint64_t value) {
		dst[0] = static_cast<uint8_t>((value >> 56) & 0xFF);
		dst[1] = static_cast<uint8_t>((value >> 48) & 0xFF);
		dst[2] = static_cast<uint8_t>((value >> 40) & 0xFF);
		dst[3] = static_cast<uint8_t>((value >> 32) & 0xFF);
		dst[4] = static_cast<uint8_t>((value >> 24) & 0xFF);
		dst[5] = static_cast<uint8_t>((value >> 16) & 0xFF);
		dst[6] = static_cast<uint8_t>((value >> 8) & 0xFF);
		dst[7] = static_cast<uint8_t>(value & 0xFF);
	}

	inline uint64_t read_u64_be(const uint8_t* src) {
		return (static_cast<uint64_t>(src[0]) << 56)
			| (static_cast<uint64_t>(src[1]) << 48)
			| (static_cast<uint64_t>(src[2]) << 40)
			| (static_cast<uint64_t>(src[3]) << 32)
			| (static_cast<uint64_t>(src[4]) << 24)
			| (static_cast<uint64_t>(src[5]) << 16)
			| (static_cast<uint64_t>(src[6]) << 8)
			| static_cast<uint64_t>(src[7]);
	}

	//  헤더 직렬화 / 역직렬화
	//
	//  오프셋(0, 2)을 이 두 함수 안에만 둔다. 호출부마다 +2 를 적으면
	//  헤더에 필드를 하나 추가하는 순간 전부 찾아 고쳐야 한다.
	//
	//  호출자 책임 — dst/src 는 최소 kHeaderSize 바이트가 확보돼 있어야 한다.
	//    길이 검사는 버퍼를 소유한 쪽에서 한 번만 하는 것이 맞다.
	inline void encode_header(uint8_t* dst, const PacketHeader& header) {
		write_u16_be(dst + 0, header.body_size);
		write_u16_be(dst + 2, static_cast<uint16_t>(header.msg_id));
	}

	inline PacketHeader decode_header(const uint8_t* src) {
		PacketHeader header{};
		header.body_size = read_u16_be(src + 0);
		header.msg_id = static_cast<MsgId>(read_u16_be(src + 2));
		return header;
	}

}	// namespace proto
