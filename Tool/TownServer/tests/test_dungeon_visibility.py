"""Exercise the real TownServer on private ports with two party members and an observer."""
import json
import os
from pathlib import Path
import secrets
import select
import shutil
import socket
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[3]
EXE = ROOT / "ActionRPGServer/x64/Debug/TownServer.exe"

def pack(fmt, *values):
    return struct.pack(">" + fmt, *values)

def string(value):
    data = value.encode()
    return pack("H", len(data)) + data

class Peer:
    def __init__(self, port):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=3)
        self.buffer = b""
        self.events = []

    def send(self, kind, body=b""):
        data = pack("H", kind) + body
        self.socket.sendall(pack("I", len(data)) + data)

    def read(self, seconds=0.2):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            ready, _, _ = select.select([self.socket], [], [], max(0, deadline - time.monotonic()))
            if not ready:
                break
            data = self.socket.recv(65536)
            if not data:
                raise RuntimeError("Test connection closed")
            self.buffer += data
            while len(self.buffer) >= 4:
                size = struct.unpack_from(">I", self.buffer)[0]
                if len(self.buffer) < size + 4:
                    break
                frame, self.buffer = self.buffer[4:size + 4], self.buffer[size + 4:]
                self.events.append((struct.unpack_from(">H", frame)[0], frame[2:]))
        return self.events

    def take(self, kind):
        self.read()
        for index, (event_kind, data) in enumerate(self.events):
            if event_kind == kind:
                self.events.pop(index)
                return data
        raise AssertionError(f"Missing packet type {kind}; received {[k for k, _ in self.events]}")

    def clear(self):
        self.read()
        self.events.clear()

def port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]

def scenario(start_only=False):
    with tempfile.TemporaryDirectory(prefix="actionrpg-town-visibility-") as directory:
        fixture = Path(directory)
        shutil.copy2(EXE, fixture / "TownServer.exe")
        (fixture / "Data").mkdir()
        document = json.loads((ROOT / "ActionRPGServer/TownServer/Data/TownMap.json").read_text(encoding="utf-8-sig"))
        gate = next(z for z in document["transitionZones"] if z["action"]["type"] == "DungeonSelection")
        document["transitionZones"] = [gate]
        document["spawn"] = {axis: sum(p[axis] for p in gate["polygon"]) / len(gate["polygon"]) for axis in ("x", "y")}
        (fixture / "Data/TownMap.json").write_text(json.dumps(document), encoding="utf-8")
        shutil.copy2(ROOT / "ActionRPGServer/TownServer/Data/DungeonCatalog.json", fixture / "Data/DungeonCatalog.json")
        client_port, control_port = port(), port()
        while client_port == control_port:
            control_port = port()
        key = secrets.token_hex(32)
        environment = dict(os.environ, ACTIONRPG_ROOM_CONTROL_KEY=key)
        peers = []
        with (fixture / "server.log").open("w") as log:
            process = subprocess.Popen([str(fixture / "TownServer.exe"), str(client_port), "4", str(control_port)],
                                       env=environment, stdout=log, stderr=log, creationflags=subprocess.CREATE_NO_WINDOW)
            try:
                deadline = time.monotonic() + 5
                while True:
                    try:
                        control = Peer(control_port)
                        peers.append(control)
                        break
                    except OSError:
                        if time.monotonic() >= deadline:
                            raise RuntimeError("Test server failed to start")
                        time.sleep(0.05)
                control.send(1, pack("QI", 99, 10) + string(key))
                players = []
                for index in range(3):
                    peer = Peer(client_port)
                    peers.append(peer)
                    peer.send(1, string(f"VisibilityTest{index}") + pack("I", index + 1))
                    player_id = struct.unpack_from(">Q", peer.take(2))[0]
                    players.append((peer, player_id))
                leader, member, observer = [p for p, _ in players]
                ids = [pid for _, pid in players]
                observer.read()
                assert {struct.unpack_from(">Q", b)[0] for k, b in observer.events if k == 4} == set(ids[:2])
                leader.send(12, pack("Q", ids[1]))
                invitation = struct.unpack_from(">Q", member.take(16))[0]
                member.send(13, pack("QB", invitation, 1))
                leader.read()
                assert any(k == 17 and b[16] == 2 for k, b in leader.events), "Party join failed"
                for peer in peers:
                    peer.clear()
                # A rejected room creation must leave both party members visible and retryable.
                leader.send(8, string(gate["id"]) + pack("I", 6))
                rejected_id = struct.unpack_from(">Q", control.take(2))[0]
                control.send(3, pack("QBQQ", rejected_id, 0, 0, 0) + string("") + pack("H", 0))
                assert leader.take(9)[0] == member.take(9)[0] == 0
                assert not any(k == 6 for k, _ in observer.read()), "Failed entry hid town players"
                for peer in peers:
                    peer.clear()
                leader.send(8, string(gate["id"]) + pack("I", 6))
                request = control.take(2)
                request_id = struct.unpack_from(">Q", request)[0]
                count = struct.unpack_from(">H", request, 12)[0]
                participants = list(struct.unpack_from(">" + "Q" * count, request, 14))
                assert set(participants) == set(ids[:2])
                room = 100
                control.send(3, pack("QBQQ", request_id, 1, room, 123) + string("127.0.0.1") + pack("H", 17777))
                assert leader.take(9)[0] == member.take(9)[0] == 1
                observer.clear()
                if not start_only:
                    for pid in participants:
                        control.send(5, pack("QQ", room, pid))
                control.send(10, pack("QH", room, count) + pack("Q" * count, *participants))
                observer.read(0.5)
                disappeared = {struct.unpack_from(">Q", b)[0] for k, b in observer.events if k == 6}
                print(json.dumps({"startOnly": start_only, "expected": ids[:2], "disappeared": sorted(disappeared)}), flush=True)
                assert disappeared == set(ids[:2]), "Dungeon participants remained visible in town"
                for peer in peers:
                    peer.clear()
                for peer, _ in players[:2]:
                    peer.send(3, pack("IbbB", 1, 1, 0, 0))
                observer.read(0.5)
                assert not any(k in (4, 5) and struct.unpack_from(">Q", b)[0] in participants
                               for k, b in observer.events), "Dungeon players reappeared or sent town movement"
                # Delayed individual confirmations must not eject already admitted participants.
                for pid in participants:
                    control.send(5, pack("QQ", room, pid))
                control.read()
                assert not any(k == 6 for k, _ in control.events), "Duplicate entry ejected a participant"
                # Fresh observers must not receive the dungeon party in their town snapshot.
                late = Peer(client_port)
                peers.append(late)
                late.send(1, string("LateObserver") + pack("I", 1))
                late.take(2)
                assert not ({struct.unpack_from(">Q", b)[0] for k, b in late.read() if k == 4} & set(participants))
                observer.clear()
                for pid in participants:
                    control.send(6, pack("QQ", room, pid))
                observer.read(0.5)
                returned = {struct.unpack_from(">Q", b)[0] for k, b in observer.events if k == 4}
                assert returned == set(participants), "Town return did not restore visibility"
                print("PASS: failed entry, disappearance, movement exclusion, duplicate entry, fresh observer, town return", flush=True)
            finally:
                for peer in peers:
                    peer.socket.close()
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=5)

if __name__ == "__main__":
    import sys
    scenario(start_only="--start-only" in sys.argv)
