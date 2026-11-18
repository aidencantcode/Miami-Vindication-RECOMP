"""Extract the Java sound-name table (GLMediaPlayer.cJ) for the native port.

The phone's Java layer loaded sound i from /sdcard/gameloft/games/Gangstar2/<cJ[i]>; the
native code only passes indices. The port needs the same table, so this pulls it out of the
decompiled APK (jadx output) into work/sound_table.txt (one name per line, line i = index i).
work/ is git-ignored: the table is derived from the owner's APK.

    python -I tools/extract_sound_table.py [analysis/jadx] [work/sound_table.txt]
"""
import pathlib
import re
import sys


def main() -> int:
    root = pathlib.Path(__file__).resolve().parent.parent
    jadx = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else root / "analysis" / "jadx"
    out = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else root / "work" / "sound_table.txt"
    src = next(jadx.rglob("GLMediaPlayer.java"), None)
    if src is None:
        print(f"GLMediaPlayer.java not found under {jadx} (run jadx on original/game.apk first)")
        return 1
    text = src.read_text(encoding="utf-8")
    m = re.search(r"String\[\]\s+cJ\s*=\s*\{(.*?)\};", text, re.S)
    if not m:
        print(f"no 'String[] cJ = {{...}}' table in {src}")
        return 1
    names = re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(names) + "\n", encoding="utf-8")
    print(f"{len(names)} sound names -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
