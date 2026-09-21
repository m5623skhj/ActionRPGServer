# TownServer 콘텐츠 및 패킷 개발 가이드

이 문서는 TownServer에 거래, 파티, 채팅 같은 마을 콘텐츠를 추가하고 해당 콘텐츠의
TCP 패킷을 클라이언트까지 연결하는 방법을 설명한다.

## 1. 현재 책임 구분

| 구성 요소 | 책임 | 콘텐츠 코드를 둘 수 있는가 |
| --- | --- | --- |
| `TcpSession` | 4바이트 길이 프레임, 비동기 읽기·쓰기, 송신 큐 | 아니요 |
| `PlayerSession` | 패킷 파싱, 형식·접속 상태 검증, 콘텐츠 라우팅 | 최소한의 검증만 |
| `Player` | 네트워크와 무관한 플레이어 상태 | 개인 상태에 적합 |
| `TownInstance` | 플레이어·마을 공유 상태와 authoritative 처리 | 콘텐츠 진입점에 적합 |
| 별도 도메인 클래스 | 거래, 파티 등 독립 규칙과 상태 | 복잡한 콘텐츠에 권장 |
| `TownMap` | 불변 맵 데이터와 이동 판정 | 맵 관련 규칙만 |
| `Protocol` | 바이트 직렬화·역직렬화 | 콘텐츠 규칙 금지 |

`PlayerSession`이 `Player`를 상속하지 않는 현재 has-a 구조를 유지한다. 네트워크 연결의
수명과 게임 플레이어의 상태·규칙은 서로 다른 책임이다.

## 2. 콘텐츠를 추가하는 순서

거래 요청을 예로 들면 다음 순서로 진행한다.

### 2.1 규칙과 authoritative 주체 정의

코드를 작성하기 전에 아래 항목을 먼저 결정한다.

- 거래를 시작할 수 있는 플레이어 상태
- 대상 플레이어가 같은 마을에 있고 보이는 상태여야 하는지
- 동시 거래 요청과 중복 수락을 어떻게 처리할지
- 취소, 연결 종료, 시간 초과 시 상태를 어떻게 정리할지
- 아이템·재화의 최종 소유권을 어느 서버가 판정할지
- 동일 요청이 재전송되었을 때 중복 처리되지 않도록 할 방법

### 2.2 상태의 위치 결정

- 플레이어 개인 상태: `Player`에 추가한다.
- 두 명 이상이 공유하는 짧은 상태: `TownInstance`가 소유한다.
- 규칙과 상태가 커지는 콘텐츠: `TradeManager`, `PartyManager` 같은 별도 클래스를 만들고
  `TownInstance`가 has-a 관계로 소유한다.
- DB나 외부 서비스 접근: 별도 비동기 서비스에 위임한다.

빠른 초기 구현에서는 `TownInstance`에 진입점을 추가할 수 있지만, 하나의 콘텐츠가 여러
상태와 명령을 가지기 시작하면 별도 도메인 클래스로 분리한다.

### 2.3 TownInstance 진입점 추가

공개 함수는 어느 I/O 스레드에서도 호출될 수 있으므로 반드시 town strand로 전달한다.

```cpp
void TownInstance::RequestTrade(const std::uint64_t inSessionId,
    const TownProtocol::TradeRequest inRequest)
{
    const std::shared_ptr<TownInstance> self = shared_from_this();
    asio::dispatch(strand, [self, inSessionId, inRequest]()
    {
        self->RequestTradeOnStrand(inSessionId, inRequest);
    });
}
```

`RequestTradeOnStrand()`에서 수행할 작업은 다음과 같다.

1. `sessionToPlayer`로 요청자를 찾는다.
2. 요청자와 대상 플레이어가 여전히 존재하는지 확인한다.
3. 거리, 현재 거래 상태, 차단 상태 같은 콘텐츠 조건을 검증한다.
4. 서버 상태를 변경한다.
5. 관련 플레이어에게 결과 패킷을 보낸다.

`players`, `sectors`, 콘텐츠 관리자 같은 town 공유 상태는 strand 밖에서 직접 접근하면 안 된다.

### 2.4 세션에서 검증하고 라우팅

`PlayerSession::HandlePacket()`에는 다음 역할만 둔다.

- 패킷 디코딩 성공 여부 확인
- 입장 완료 여부 확인
- 문자열 길이, 수량, 열거형 범위 같은 저비용 형식 검증
- `TownInstance`의 콘텐츠 진입점 호출

대상 플레이어 존재 여부, 거래 가능 여부처럼 공유 상태가 필요한 검증은 `TownInstance`의
strand 안에서 수행한다.

### 2.5 연결 종료 처리

`TownInstance::LeaveOnStrand()`에서 해당 플레이어가 참여 중인 거래·파티·초대 상태를
정리한다. 양쪽 플레이어가 공유하는 상태라면 상대에게 취소 또는 탈퇴 결과도 전송한다.

## 3. 스레드 안전성 규칙

- `TcpSession` 송신 큐는 각 소켓 executor에서 직렬화된다.
- 마을 공유 상태는 `TownInstance::strand`에서만 읽고 수정한다.
- 콘텐츠 관리자도 `TownInstance` strand에서만 호출한다면 별도 mutex가 필요 없다.
- DB, HTTP, 파일 I/O처럼 오래 걸리는 작업을 strand에서 동기 호출하지 않는다.
- 외부 비동기 작업의 결과는 다시 `asio::dispatch(strand, ...)`로 돌아온 뒤 적용한다.
- 비동기 작업 동안 `Player&`나 컨테이너 iterator를 보관하지 않는다. 완료 시 ID로 다시 찾는다.
- 클라이언트가 보낸 플레이어 ID, 가격, 수량, 상태를 신뢰하지 않는다.

strand에서 블로킹 작업을 실행하면 모든 플레이어의 이동과 콘텐츠 처리가 함께 멈춘다.

## 4. 현재 패킷 형식

TCP 스트림은 다음 구조를 사용한다.

```text
[uint32 bodySize, big-endian][packet body]
packet body = [uint16 packetType, big-endian][payload]
```

현재 패킷 ID는 다음과 같다.

| ID | 패킷 | 방향 |
| ---: | --- | --- |
| 1 | `EnterTownRequest` | Client → Server |
| 2 | `EnterTownResponse` | Server → Client |
| 3 | `MoveInput` | Client → Server |
| 4 | `PlayerAppear` | Server → Client |
| 5 | `PlayerMove` | Server → Client |
| 6 | `PlayerDisappear` | Server → Client |

패킷 body 상한은 1MiB다. 서버 송신 대기열 상한은 4MiB이며, 상한을 초과하는 세션은
정상적인 서비스가 불가능한 상태로 취급한다.

직렬화 규칙:

- 정수는 big-endian으로 기록한다.
- `bool`은 `uint8`의 0 또는 1로 기록한다.
- `float`은 IEEE 754 비트를 `uint32`로 변환한 뒤 big-endian으로 기록한다.
- 문자열은 `[uint16 byteLength][UTF-8 bytes]` 형식이다.
- C++ 구조체 메모리를 그대로 전송하지 않는다. 패딩과 ABI가 플랫폼마다 다르다.
- 디코더는 모든 필드를 읽은 뒤 `Finished()`로 여분 바이트가 없는지 검사한다.

## 5. 새 패킷 추가 절차

현재 서버와 클라이언트는 `Protocol`을 별도로 보유한다. 두 파일의 패킷 ID, 필드 순서,
자료형은 반드시 동일해야 한다. 일치하지 않으면 해당 연결은 malformed packet으로 종료된다.

거래 요청과 결과를 추가한다고 가정한다.

### 5.1 패킷 ID와 데이터 정의

기존 ID를 재사용하거나 중간 값을 바꾸지 말고 마지막 번호 뒤에 추가한다.

```cpp
enum class PacketType : std::uint16_t
{
    // 기존 값 유지
    TradeRequest = 7,
    TradeResult = 8
};

struct TradeRequest
{
    std::uint32_t requestId{};
    std::uint64_t targetPlayerId{};
};

struct TradeResult
{
    std::uint32_t requestId{};
    std::uint8_t resultCode{};
};
```

`requestId`는 클라이언트가 응답을 자신의 요청과 대응시키는 값이다. 재화 변경처럼 중복
실행이 위험한 명령에는 서버 측 중복 요청 방지도 함께 설계한다.

### 5.2 서버 Protocol 수정

`Protocol.h`에 구조체와 필요한 함수 선언을 추가한다.

```cpp
std::vector<std::uint8_t> Encode(const TradeResult& inPacket);
std::optional<TradeRequest> DecodeTradeRequest(
    const std::vector<std::uint8_t>& inPacket);
```

`Protocol.cpp`에서는 필드를 선언 순서대로 기록하고 정확히 같은 순서로 읽는다.

```cpp
std::vector<std::uint8_t> Encode(const TradeResult& inPacket)
{
    PacketWriter writer(PacketType::TradeResult);
    writer.WriteUInt32(inPacket.requestId);
    writer.WriteUInt8(inPacket.resultCode);
    return writer.Finish();
}
```

디코더는 예상 타입, 모든 필드, 값 범위, `Finished()`를 확인한다.

### 5.3 서버 수신 라우팅

`PlayerSession::HandlePacket()`의 switch에 C2S 패킷 case를 추가한다.

```cpp
case TownProtocol::PacketType::TradeRequest:
{
    const auto request = TownProtocol::DecodeTradeRequest(inPacket);
    if (!request.has_value() || !enterRequested)
    {
        tcpSession->Stop();
        return;
    }
    town->RequestTrade(GetSessionId(), *request);
    return;
}
```

패킷 형식 오류는 연결을 종료한다. 콘텐츠 조건 불충족은 연결 오류가 아니므로
`TradeResult`에 명시적인 실패 코드를 담아 응답한다.

### 5.4 클라이언트 Protocol 수정

클라이언트 `Network/TownProtocol.h/.cpp`에 서버와 동일한 enum 값과 구조체를 추가한다.

- C2S 요청: `Encode(const TradeRequest&)`
- S2C 결과: `DecodeTradeResult(...)`

서버와 클라이언트의 필드 순서가 동일한지 나란히 확인한다.

### 5.5 클라이언트 송수신 연결

`TownClient`에 `SendTradeRequest()`를 추가하고 `SendMovement()`와 동일하게 network strand에
post한 뒤 `QueuePacket()`을 호출한다.

S2C 결과는 다음 세 곳에 등록한다.

1. `TownClient::HandlePacket()` switch에서 디코딩한다.
2. `TownEvent` variant에 `TradeResult`를 추가한다.
3. `GameWorld::ProcessNetworkEvents()` 또는 별도 UI/controller가 이벤트를 처리한다.

네트워크 스레드에서 게임 오브젝트를 직접 수정하지 않는다. `TownClient`가 mutex로 보호된
이벤트 큐에 넣고 게임 스레드가 `ConsumeEvents()`로 가져가는 현재 흐름을 유지한다.

## 6. 호환성 정책

현재 프로토콜은 서버와 클라이언트를 함께 배포하는 lockstep 방식이다. 패킷 구조가 다른
구버전 클라이언트는 연결이 종료될 수 있다.

서버와 클라이언트를 독립 배포해야 한다면 콘텐츠 추가 전에 다음을 도입한다.

- 입장 요청의 `protocolVersion`
- 서버가 지원하는 최소·최대 버전
- 호환되지 않는 버전을 설명하는 입장 실패 패킷

필드를 기존 패킷 중간에 삽입하는 대신 새 패킷 ID 또는 새 버전 패킷을 추가하는 편이 안전하다.

## 7. 검증 체크리스트

### 패킷 단위 검증

- encode 후 decode한 값이 원본과 같은가
- 문자열 길이 0, 최대값, 초과값을 처리하는가
- 잘린 body와 여분 바이트를 거부하는가
- 잘못된 enum, bool, 수량, 플레이어 ID를 거부하는가
- 1MiB body 상한을 넘는 패킷을 거부하는가

### 콘텐츠 검증

- 입장 전 요청을 거부하는가
- 존재하지 않거나 퇴장한 플레이어를 안전하게 처리하는가
- 같은 요청이 동시에 들어와도 상태가 한 번만 변경되는가
- 요청 도중 연결이 끊어지면 공유 상태가 정리되는가
- 실패가 세션 종료가 아닌 명시적 결과 패킷으로 전달되는가
- DB 응답이 늦어져도 이동 tick이 멈추지 않는가

### 통합 검증

1. 서버와 클라이언트 Debug/Release를 빌드한다.
2. 클라이언트 두 개 이상을 연결한다.
3. 정상 요청과 거절 요청을 각각 보낸다.
4. 요청 중 한 클라이언트를 종료한다.
5. 재접속 후 이전 임시 상태가 남지 않았는지 확인한다.

## 8. 관련 파일

- `Protocol.h/.cpp`: 서버 패킷 정의와 직렬화
- `PlayerSession.h/.cpp`: C2S 검증과 라우팅
- `TownInstance.h/.cpp`: authoritative 콘텐츠 처리와 공유 상태
- `Player.h/.cpp`: 네트워크 비종속 플레이어 상태
- `TcpSession.h/.cpp`: TCP framing과 비동기 송수신
- `NetworkConstants.h`: body 및 송신 큐 상한
