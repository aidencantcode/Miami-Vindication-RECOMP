"""Validate the renderer's shaders as GLSL ES 3.00 (Android) and GLSL 3.30 (desktop) with Khronos'
reference compiler, without a device.

    python -I tools/check_shaders.py [path/to/glslang.exe]

Shaders are the raw strings kVS / k...FS in src/gl/ffp.cpp and src/gl/post.cpp; the preambles match
the ones the code adds at run time (ffp.cpp / post.cpp prelude()).
"""
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
GLSLANG = sys.argv[1] if len(sys.argv) > 1 else r"C:\Users\aiden\devtools\glslang\bin\glslang.exe"

SOURCES = {
    "ffp": (ROOT / "src/gl/ffp.cpp", "#version 300 es\nprecision highp float;\nprecision highp int;\n",
            "#version 300 es\nprecision highp float;\nprecision highp int;\n"),
    "post": (ROOT / "src/gl/post.cpp", "#version 300 es\n", "#version 300 es\nprecision highp float;\n"),
}

failed = 0
with tempfile.TemporaryDirectory() as tmp:
    for name, (path, es_vs, es_fs) in SOURCES.items():
        text = path.read_text(encoding="utf-8")
        for m in re.finditer(r'const char\* (k\w+) = R"\((.*?)\)";', text, re.S):
            var, body = m.group(1), m.group(2)
            stage = "vert" if var == "kVS" else "frag"
            for profile, pre in (("es300", es_vs if stage == "vert" else es_fs), ("gl330", "#version 330\n")):
                f = pathlib.Path(tmp) / f"{name}_{var}_{profile}.{stage}"
                f.write_text(pre + body, encoding="utf-8")
                r = subprocess.run([GLSLANG, str(f)], capture_output=True, text=True)
                ok = r.returncode == 0
                failed += not ok
                print(f"{'ok  ' if ok else 'FAIL'} {name} {var} ({stage}, {profile})")
                if not ok:
                    print("    " + (r.stdout + r.stderr).strip().replace("\n", "\n    "))
print("all shaders valid" if not failed else f"{failed} shader(s) failed")
sys.exit(1 if failed else 0)
