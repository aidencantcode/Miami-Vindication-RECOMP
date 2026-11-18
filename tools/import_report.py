"""Write analysis reports for an Android ARM .so from the JSON produced by
elf_report.py:  imports grouped by providing library, JNI exports, and
relocation facts relevant to loading.

ELF imports do not record which library provides them (no symbol versioning
in this old binary); the Android linker searches the DT_NEEDED list.  So the
grouping below is by API family, using bionic / NDK knowledge, and resolved
exactly where we can (e.g. against a sibling .so's exports).

Usage:
  python tools/import_report.py analysis/raw/libGangster2.elf.json \
      --sibling analysis/raw/libStormGLOFT.elf.json --out analysis/libGangster2
"""
import argparse
import json
import re

from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection

LIBM = set("""
acos acosf asin asinf atan atanf atan2 atan2f ceil ceilf cos cosf cosh coshf exp expf exp2
floor floorf fmod fmodf frexp ldexp ldexpf log logf log10 log10f pow powf sin sinf sinh
sqrt sqrtf tan tanf tanh truncf trunc round roundf lround modf fabs fabsf __isfinitef
__isfinite __isnanf __isinff hypot hypotf
""".split())

# Symbols bionic's libc.so exported for ARM (libgcc compat + ARM EABI helpers).
AEABI_RUNTIME = re.compile(r"^__aeabi_(?!atexit|unwind_cpp)")
LIBC_CXX_ABI = {"__aeabi_atexit", "__cxa_atexit", "__cxa_finalize"}
CXX_RUNTIME = re.compile(r"^(_Z|__cxa_(guard|pure_virtual|begin_catch|end_catch|throw|rethrow|allocate)|__gxx_personality)")
UNWIND = re.compile(r"^(__aeabi_unwind_cpp_pr\d|_Unwind_|__gnu_Unwind)")

# Data objects bionic exports whose LAYOUT the game depends on.
BIONIC_DATA = {"__sF", "_ctype_", "_tolower_tab_", "_toupper_tab_", "__stack_chk_guard",
               "__dso_handle", "_ZSt7nothrow"}

GLES1_ONLY = set("""
glAlphaFunc glClientActiveTexture glClipPlanef glColor4f glColor4ub glColorPointer
glDisableClientState glEnableClientState glFogf glFogfv glLightModelf glLightModelfv glLightf
glLightfv glLoadIdentity glLoadMatrixf glMaterialf glMaterialfv glMatrixMode glMultMatrixf
glMultiTexCoord4f glNormal3f glNormalPointer glPointParameterf glPointParameterfv glPointSize
glPopMatrix glPushMatrix glShadeModel glTexCoordPointer glTexEnvf glTexEnvfv glTexEnvi
glVertexPointer
""".split())


def classify(name, sibling_exports):
    if name in sibling_exports:
        return "libStormGLOFT.so (sibling, exact)"
    if name.startswith("__android_log"):
        return "liblog"
    if name.startswith("gl"):
        if name.endswith("OES"):
            return "GLES 1.1 (OES extension)"
        if name in GLES1_ONLY:
            return "GLES 1.1 (fixed-function, ES1-only)"
        return "GLES 1.1 (common to ES1/ES2)"
    if name.startswith("egl"):
        return "libEGL"
    if name == "slCreateEngine" or name.startswith("SL_IID"):
        return "libOpenSLES"
    if name.startswith(("ANativeWindow", "AAsset", "ALooper", "AInput", "AConfiguration")):
        return "libandroid"
    if name.startswith(("dlopen", "dlsym", "dlclose", "dlerror", "dladdr")):
        return "libdl"
    if name in LIBM:
        return "libm"
    if UNWIND.match(name):
        return "C++ unwinder (libgcc/libstdc++)"
    if name in LIBC_CXX_ABI:
        return "libc (C++ ABI support)"
    if AEABI_RUNTIME.match(name):
        return "libc (ARM EABI runtime helpers: soft-float / int division)"
    if CXX_RUNTIME.match(name):
        return "libstdc++ (Android minimal C++ runtime)"
    if name.startswith("pthread_") or name.startswith("sched_"):
        return "libc (pthreads / scheduling)"
    return "libc"


def text_relocations(so_path):
    """Count relocations whose target lies inside a non-writable PT_LOAD."""
    elf = ELFFile(open(so_path, "rb"))
    ro = [(s["p_vaddr"], s["p_vaddr"] + s["p_memsz"]) for s in elf.iter_segments()
          if s["p_type"] == "PT_LOAD" and not (s["p_flags"] & 2)]
    hits = {}
    for sec in elf.iter_sections():
        if isinstance(sec, RelocationSection):
            for r in sec.iter_relocations():
                off = r["r_offset"]
                if any(a <= off < b for a, b in ro):
                    hits[r["r_info_type"]] = hits.get(r["r_info_type"], 0) + 1
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("json")
    ap.add_argument("--sibling", help="elf_report JSON of another .so in the APK")
    ap.add_argument("--out", required=True, help="output path prefix")
    a = ap.parse_args()
    rep = json.load(open(a.json, encoding="utf-8"))
    sib = set()
    if a.sibling:
        srep = json.load(open(a.sibling, encoding="utf-8"))
        # Only a sibling that is actually in DT_NEEDED can satisfy our imports.
        if srep["dynamic"]["soname"] and srep["dynamic"]["soname"] in rep["dynamic"]["needed"]:
            sib = {e["name"] for e in srep["exports"]}

    groups = {}
    for imp in rep["imports"]:
        groups.setdefault(classify(imp["name"], sib), []).append(imp)

    lines = [f"# Imports of {rep['file']}",
             "",
             f"DT_NEEDED (search order): {', '.join(rep['dynamic']['needed'])}",
             f"Total imported symbols: {len(rep['imports'])}",
             "",
             "Grouping is by API family (old ELF imports carry no library name).",
             "Objects marked [DATA] are variables, not functions: our shim must provide",
             "memory with bionic's exact layout for them.",
             ""]
    for g in sorted(groups, key=lambda k: (-len(groups[k]), k)):
        lines.append(f"## {g}  ({len(groups[g])})")
        for imp in sorted(groups[g], key=lambda i: i["name"]):
            tag = " [DATA]" if imp["type"] == "STT_OBJECT" or imp["name"] in BIONIC_DATA else ""
            lines.append(f"- `{imp['name']}`{tag}")
        lines.append("")
    with open(a.out + ".imports.md", "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))

    jni = sorted((e for e in rep["exports"] if e["name"].startswith(("Java_", "JNI_"))),
                 key=lambda e: e["name"])
    jl = [f"# JNI exports of {rep['file']}", "",
          "Address has bit 0 set when the function is Thumb code.", "",
          "| address | mode | Java class | method | symbol |", "|---|---|---|---|---|"]
    for e in jni:
        v = int(e["value"], 16)
        parts = e["name"].split("_")
        if e["name"].startswith("Java_"):
            # Java_<pkg parts>_<Class>_<method>; package here has no '_' escapes
            cls, meth = parts[-2], parts[-1]
            pkg = ".".join(parts[1:-2])
        else:
            pkg, cls, meth = "", "", e["name"]
        jl.append(f"| {hex(v & ~1)} | {'Thumb' if v & 1 else 'ARM'} | {pkg}.{cls} | {meth} | `{e['name']}` |")
    if not any(e["name"] == "JNI_OnLoad" for e in jni):
        jl += ["", "No `JNI_OnLoad`: Java resolves each native method by its mangled name."]
    with open(a.out + ".jni_exports.md", "w", encoding="utf-8") as fh:
        fh.write("\n".join(jl) + "\n")

    tr = text_relocations(rep["file"])
    summary = {g: len(v) for g, v in groups.items()}
    print("groups:", json.dumps(summary, indent=1))
    print("JNI exports:", len(jni))
    print("relocations into read-only segments (type: count):", tr or "none")


if __name__ == "__main__":
    main()
