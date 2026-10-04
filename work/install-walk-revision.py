"""Copy the generated PNG unchanged and update only its existing animation data."""
import hashlib
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
client = Path(r"C:/Users/KimHyeongJin/source/repos/ActionRPGClient/ActionRPGClient")
output = root / "output/character-walk-20261004"
source_png = output / "player_walk.png"
source_ini = client / "Assets/Data/animations.ini"
data = source_ini.read_bytes().decode("utf-8")
match = re.search(r"(?ms)^\[PlayerWalk\]\r?\n.*?(?=^\[|\Z)", data)
assert match, "Missing PlayerWalk animation"
section = match.group()
assert "columns=5" in section and "rows=2" in section and "frame_count=10" in section
assert "render_height=202" in section and "anchor_xs=" not in section, "Animation changed during editing"
replacements = {"anchor_y": "0.958386", "second_row_anchor_y": "0.930643", "render_height": "198"}
for key, value in replacements.items():
    section, count = re.subn(r"(?m)^" + key + r"=[^\r\n]*", key + "=" + value, section)
    assert count == 1
newline = "\r\n" if "\r\n" in section else "\n"
anchors = "0.509586,0.512108,0.509586,0.484372,0.500761,0.507065,0.510847,0.505804,0.491936,0.503283"
section = section.replace("frame_seconds=0.088" + newline, "frame_seconds=0.088" + newline + "anchor_xs=" + anchors + newline)
updated = (data[:match.start()] + section + data[match.end():]).encode("utf-8")

def write_atomic(path, payload):
    assert path.resolve().is_relative_to(client.resolve())
    temporary = path.with_name(path.name + ".walk-revision.tmp")
    temporary.write_bytes(payload)
    temporary.replace(path)

png = source_png.read_bytes()
write_atomic(client / "Assets/Images/Characters/player_walk.png", png)
write_atomic(source_ini, updated)
(output / "animations.ini").write_bytes(updated)
runtime = client / "artifacts/bin/x64/Debug/Assets"
if runtime.is_dir():
    for relative, payload in (("Images/Characters/player_walk.png", png), ("Data/animations.ini", updated)):
        destination = runtime / relative
        if destination.exists():
            write_atomic(destination, payload)
            print("Updated:", destination)
print("Updated source walk sheet and PlayerWalk frame anchors; PNG SHA256:", hashlib.sha256(png).hexdigest())
