# Packet Generator

`PacketDefine.yml`을 기준으로 GameRoomServer와 ActionRPGClient가 공유할 RUDP 패킷 코드를 생성하고,
`TownPacketDefine.yml`을 기준으로 TownServer와 ActionRPGClient가 공유할 TCP 패킷 선언을 생성한다.
패킷 ID는 YAML의 선언 순서대로 1부터 자동 부여된다. 기존 패킷의 순서를 바꾸거나 중간에서 삭제하지 않고 새 패킷은 마지막에 추가한다.

## 준비

```bat
python -m pip install -r Tool\PacketGenerator\requirements.txt
```

## 생성

```bat
Tool\PacketGenerate.bat
```

생성 결과가 YAML과 일치하는지만 확인하려면 다음 명령을 사용한다.

```bat
Tool\PacketGenerate.bat --check
```

출력 위치와 네임스페이스는 각 YAML의 `Protocol` 항목에서 관리한다. 생성된 `DungeonProtocol.h`,
`DungeonProtocol.cpp`, `TownPacket.generated.h`는 직접 수정하지 않는다.
