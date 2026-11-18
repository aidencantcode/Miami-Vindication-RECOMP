"""A fake JNIEnv / JavaVM for the probe.

JNIEnv* -> [pointer to function table]; the table has the standard 233 slots
(JNI 1.6, `JNINativeInterface`), each pointing at a trap. Implemented functions
are methods named jni_<Name>; every other slot logs `JNI-STUB:` and returns 0.

Java objects are opaque handles in an UNMAPPED range (emu.HANDLE_BASE), so a
native dereference of a jobject faults with a clear message.

Java-side behaviour (the static callbacks the game makes) lives in JAVA below,
keyed by (class short name, method name, signature). Values follow the decompiled
Java (analysis/jni_surface.md); unknown methods are logged as JAVA-STUB.
"""
import collections
import os
import struct

import emu as E
from hle_libc import b32, b64, f32, split64, s32

JNI_NAMES = (
    ["reserved0", "reserved1", "reserved2", "reserved3", "GetVersion", "DefineClass", "FindClass",
     "FromReflectedMethod", "FromReflectedField", "ToReflectedMethod", "GetSuperclass",
     "IsAssignableFrom", "ToReflectedField", "Throw", "ThrowNew", "ExceptionOccurred",
     "ExceptionDescribe", "ExceptionClear", "FatalError", "PushLocalFrame", "PopLocalFrame",
     "NewGlobalRef", "DeleteGlobalRef", "DeleteLocalRef", "IsSameObject", "NewLocalRef",
     "EnsureLocalCapacity", "AllocObject", "NewObject", "NewObjectV", "NewObjectA",
     "GetObjectClass", "IsInstanceOf", "GetMethodID"]
    + [f"Call{t}Method{v}" for t in ("Object", "Boolean", "Byte", "Char", "Short", "Int", "Long",
                                      "Float", "Double", "Void") for v in ("", "V", "A")]
    + [f"CallNonvirtual{t}Method{v}" for t in ("Object", "Boolean", "Byte", "Char", "Short", "Int",
                                                "Long", "Float", "Double", "Void") for v in ("", "V", "A")]
    + ["GetFieldID"]
    + [f"Get{t}Field" for t in ("Object", "Boolean", "Byte", "Char", "Short", "Int", "Long", "Float", "Double")]
    + [f"Set{t}Field" for t in ("Object", "Boolean", "Byte", "Char", "Short", "Int", "Long", "Float", "Double")]
    + ["GetStaticMethodID"]
    + [f"CallStatic{t}Method{v}" for t in ("Object", "Boolean", "Byte", "Char", "Short", "Int", "Long",
                                            "Float", "Double", "Void") for v in ("", "V", "A")]
    + ["GetStaticFieldID"]
    + [f"GetStatic{t}Field" for t in ("Object", "Boolean", "Byte", "Char", "Short", "Int", "Long", "Float", "Double")]
    + [f"SetStatic{t}Field" for t in ("Object", "Boolean", "Byte", "Char", "Short", "Int", "Long", "Float", "Double")]
    + ["NewString", "GetStringLength", "GetStringChars", "ReleaseStringChars", "NewStringUTF",
       "GetStringUTFLength", "GetStringUTFChars", "ReleaseStringUTFChars", "GetArrayLength",
       "NewObjectArray", "GetObjectArrayElement", "SetObjectArrayElement"]
    + [f"New{t}Array" for t in ("Boolean", "Byte", "Char", "Short", "Int", "Long", "Float", "Double")]
    + [f"Get{t}ArrayElements" for t in ("Boolean", "Byte", "Char", "Short", "Int", "Long", "Float", "Double")]
    + [f"Release{t}ArrayElements" for t in ("Boolean", "Byte", "Char", "Short", "Int", "Long", "Float", "Double")]
    + [f"Get{t}ArrayRegion" for t in ("Boolean", "Byte", "Char", "Short", "Int", "Long", "Float", "Double")]
    + [f"Set{t}ArrayRegion" for t in ("Boolean", "Byte", "Char", "Short", "Int", "Long", "Float", "Double")]
    + ["RegisterNatives", "UnregisterNatives", "MonitorEnter", "MonitorExit", "GetJavaVM",
       "GetStringRegion", "GetStringUTFRegion", "GetPrimitiveArrayCritical",
       "ReleasePrimitiveArrayCritical", "GetStringCritical", "ReleaseStringCritical",
       "NewWeakGlobalRef", "DeleteWeakGlobalRef", "ExceptionCheck", "NewDirectByteBuffer",
       "GetDirectBufferAddress", "GetDirectBufferCapacity", "GetObjectRefType"])
assert len(JNI_NAMES) == 233, len(JNI_NAMES)
assert JNI_NAMES.index("GetStaticMethodID") * 4 == 0x1C4
assert JNI_NAMES.index("NewStringUTF") * 4 == 0x29C

VM_NAMES = ["reserved0", "reserved1", "reserved2", "DestroyJavaVM", "AttachCurrentThread",
            "DetachCurrentThread", "GetEnv", "AttachCurrentThreadAsDaemon"]

PKG = "com/gameloft/android/GAND/GloftGMHP/ML/"


class JObj:
    def __init__(self, kind, cls=None, value=None):
        self.kind, self.cls, self.value = kind, cls, value

    def __repr__(self):
        if self.kind == "class":
            return f"<class {self.value}>"
        if self.kind == "string":
            return repr(self.value)
        if self.kind == "array":
            return f"<{self.cls}[{len(self.value)}]>"
        return f"<{self.cls} instance>"


def parse_sig(sig):
    assert sig.startswith("("), sig
    i, args = 1, []
    while sig[i] != ")":
        start = i
        while sig[i] == "[":
            i += 1
        if sig[i] == "L":
            i = sig.index(";", i)
        i += 1
        args.append(sig[start:i])
    return args, sig[i + 1:]


# --- Java side --------------------------------------------------------------
def _java_resource_path(jni, s):
    s = s[3:] if s.startswith(".//") else s[2:] if s.startswith("./") else s
    return "/sdcard/gameloft/games/gangster2/" + s.strip()


def _getResourceFull(jni, args):
    path = _java_resource_path(jni, jni.str_of(args[0]))
    host = jni.emu.fs.resolve_read(path)
    jni.emu.log(f"[java] GLResLoader.getResourceFull({path!r}) -> {'%d bytes' % os.path.getsize(host) if host else 'null'}")
    if not host:
        return 0
    with open(host, "rb") as fh:
        return jni.new(JObj("array", "byte", bytearray(fh.read())))


def _getResourceLength(jni, args):
    path = _java_resource_path(jni, jni.str_of(args[0]))
    host = jni.emu.fs.resolve_read(path)
    return os.path.getsize(host) if host else 0


def _getResourceBytes(jni, args):
    path = _java_resource_path(jni, jni.str_of(args[0]))
    off, ln = args[1], args[2]
    host = jni.emu.fs.resolve_read(path)
    if not host:
        return 0
    with open(host, "rb") as fh:
        fh.seek(off)
        return jni.new(JObj("array", "byte", bytearray(fh.read(ln).ljust(ln, b"\0"))))


def _const(v):
    return lambda jni, args: v


# (class, name, sig) -> implementation; the 57 callbacks from analysis/jni_surface.md
JAVA = {}
for _n, _s in [("Exit", "()V"), ("sendAppToBackground", "()V"), ("openBrowser", "(Ljava/lang/String;)V"),
               ("launchIGP", "(I)V"), ("OpenGLive", "(I)V"), ("NotifyTrophy", "(I)V"), ("lockDemo", "()V"),
               ("IncreaseLaunchTimes", "()V"), ("VZInitMobileNetwork", "()V"), ("VZRestoreNetworkState", "()V"),
               ("VZRequestLogin", "()V"), ("VZRequestPurchaseGame", "()V")]:
    JAVA[("Gangster2", _n, _s)] = None
JAVA.update({
    ("Gangster2", "isWifiAlive", "()I"): _const(0),
    ("Gangster2", "unlockDemo", "()I"): _const(0),
    ("Gangster2", "DisableLaunchGame", "()I"): _const(0),
    ("Gangster2", "VZIsInProgress", "()I"): _const(0),
    ("Gangster2", "VZIsErrorOcurred", "()I"): _const(1),
    ("Gangster2", "VZIsMobileNetworkReady", "()I"): _const(0),
    ("Gangster2", "VZGetGamePrice", "()[B"): None,
    ("Gangster2", "VZGetGameName", "()[B"): None,
    ("Gangster2", "VZGetLastServerMsg", "()[B"): None,
    ("GLMediaPlayer", "GetDeviceType", "()I"): _const(-1),
    ("GLMediaPlayer", "detectPhoneLang", "()I"): _const(0),
    ("GLMediaPlayer", "getDeviceWidth", "()I"): _const(800),
    ("GLMediaPlayer", "loadMovie", "(Ljava/lang/String;)I"): _const(1),
    ("GLMediaPlayer", "isMediaPlaying", "(I)I"): _const(0),
    ("GLMediaPlayer", "isSoundLoaded", "(II)I"): _const(0),
    ("GLMediaPlayer", "isSoundLoadedBig", "(I)I"): _const(0),
    ("GLResLoader", "getResourceFull", "(Ljava/lang/String;)[B"): _getResourceFull,
    ("GLResLoader", "getResourceBytes", "(Ljava/lang/String;II)[B"): _getResourceBytes,
    ("GLResLoader", "getResourceLength", "(Ljava/lang/String;)I"): _getResourceLength,
})
for _n, _s in [("loadSound", "(II)V"), ("loadSoundBig", "(I)V"), ("unloadSound", "(II)V"),
               ("unloadSoundBig", "(I)V"), ("playSound", "(IIFF)V"), ("playSoundBig", "(IFI)V"),
               ("pauseSound", "(II)V"), ("pauseSoundBig", "(I)V"), ("resumeSound", "(II)V"),
               ("resumeSoundBig", "(I)V"), ("stopSound", "(II)V"), ("stopSoundBig", "(I)V"),
               ("setVolume", "(IIF)V"), ("setVolumeBig", "(IF)V"), ("setPitch", "(IIF)V"),
               ("resetSound", "(I)V"), ("stopAllSounds", "()V"), ("pauseAllSounds", "()V"),
               ("resumeAllSounds", "()V"), ("stopAllPool", "(I)V"), ("stopAllBig", "(I)V"),
               ("destroySoundPool", "()V"), ("initSoundPoolArray", "()V"), ("setMusicGain", "(F)V"),
               ("setSfxGain", "(F)V"), ("setVfxGain", "(F)V")]:
    JAVA[("GLMediaPlayer", _n, _s)] = None


class FakeJNI:
    def __init__(self, emu):
        self.emu = emu
        self.objs = {}
        self._next = E.HANDLE_BASE + 0x100
        self.methods = {}        # method id -> (class name, name, sig, static)
        self.fields = {}
        self.class_by_name = {}
        self.trace = collections.Counter()
        self.trace_order = []
        self.lookups = []        # (kind, class, name, sig, known)
        self.java_calls = collections.Counter()
        self.pinned = {}         # guest ptr -> array handle
        self.strings_made = []

        self.table = emu.data_alloc(len(JNI_NAMES) * 4, 4)
        for i, name in enumerate(JNI_NAMES):
            emu.w32(self.table + 4 * i, emu.add_trap("JNI::" + name, self._dispatch))
        self.env = emu.data_alloc(4, 4)
        emu.w32(self.env, self.table)

        self.vm_table = emu.data_alloc(len(VM_NAMES) * 4, 4)
        for i, name in enumerate(VM_NAMES):
            emu.w32(self.vm_table + 4 * i, emu.add_trap("JVM::" + name, self._dispatch))
        self.vm = emu.data_alloc(4, 4)
        emu.w32(self.vm, self.vm_table)

    # -- handles ---------------------------------------------------------------
    def new(self, obj):
        h = self._next
        self._next += 4
        self.objs[h] = obj
        return h

    def cls(self, name):
        if name not in self.class_by_name:
            self.class_by_name[name] = self.new(JObj("class", value=name))
        return self.class_by_name[name]

    def instance(self, clsname):
        return self.new(JObj("object", cls=clsname))

    def get(self, h):
        o = self.objs.get(h)
        if o is None and h:
            raise E.ProbeError(f"JNI: unknown object handle {h:#x}")
        return o

    def str_of(self, h):
        o = self.get(h)
        return None if o is None else o.value

    # -- dispatch -----------------------------------------------------------------
    def _dispatch(self, emu, trapname, a):
        name = trapname.split("::", 1)[1]
        if trapname not in self.trace:
            self.trace_order.append(trapname)
        self.trace[trapname] += 1
        fn = getattr(self, ("jvm_" if trapname.startswith("JVM") else "jni_") + name, None)
        if fn is None:
            if name.startswith("CallStatic"):
                return self._call_static(name, a)
            r = a.raw()
            emu.log(f"JNI-STUB: {trapname}({r[0]:#x}, {r[1]:#x}, {r[2]:#x}, {r[3]:#x}) -> 0 "
                    f"[from {emu.where(emu.uc.reg_read(E.A.UC_ARM_REG_LR))}]")
            return 0
        return fn(a)

    # -- JNIEnv functions --------------------------------------------------------------
    def jni_GetVersion(self, a):
        return 0x00010006

    def jni_FindClass(self, a):
        a.u32()
        name = self.emu.cstr(a.u32()).decode("latin1")
        self.emu.log(f"[jni] FindClass({name!r})")
        return self.cls(name)

    def jni_GetObjectClass(self, a):
        a.u32()
        o = self.get(a.u32())
        return self.cls(o.cls if o.kind == "object" else "java/lang/Object")

    def _lookup(self, a, kind):
        a.u32()
        clsobj = self.get(a.u32())
        name = self.emu.cstr(a.u32()).decode("latin1")
        sig = self.emu.cstr(a.u32()).decode("latin1")
        cname = clsobj.value if clsobj else "?"
        short = cname.rsplit("/", 1)[-1]
        known = (short, name, sig) in JAVA if kind == "static" else None
        self.lookups.append((kind, cname, name, sig, known))
        mid = self.new(JObj("method", value=(short, name, sig)))
        self.methods[mid] = (short, name, sig, kind == "static")
        return mid

    def jni_GetStaticMethodID(self, a):
        return self._lookup(a, "static")

    def jni_GetMethodID(self, a):
        return self._lookup(a, "virtual")

    def jni_GetStaticFieldID(self, a):
        a.u32()
        clsobj = self.get(a.u32())
        name, sig = self.emu.cstr(a.u32()).decode(), self.emu.cstr(a.u32()).decode()
        self.lookups.append(("static-field", clsobj.value, name, sig, None))
        fid = self.new(JObj("field", value=(clsobj.value, name, sig)))
        self.fields[fid] = (clsobj.value, name, sig)
        return fid

    def jni_NewStringUTF(self, a):
        a.u32()
        p = a.u32()
        s = self.emu.cstr(p)
        if s is None:
            return 0
        text = s.decode("utf-8", "replace")
        self.strings_made.append(text)
        return self.new(JObj("string", "java/lang/String", text))

    def jni_GetStringUTFChars(self, a):
        a.u32()
        h, iscopy = a.u32(), a.u32()
        data = self.str_of(h).encode("utf-8") + b"\0"
        p = self.emu.heap_alloc(len(data))
        self.emu.write(p, data)
        if iscopy:
            self.emu.write(iscopy, b"\1")
        return p

    def jni_ReleaseStringUTFChars(self, a):
        return 0

    def jni_GetStringUTFLength(self, a):
        a.u32()
        return len(self.str_of(a.u32()).encode("utf-8"))

    def jni_GetStringLength(self, a):
        a.u32()
        return len(self.str_of(a.u32()).encode("utf-16-le")) // 2

    def jni_NewGlobalRef(self, a):
        a.u32()
        return a.u32()

    jni_NewLocalRef = jni_NewGlobalRef

    def jni_DeleteGlobalRef(self, a):
        return 0

    jni_DeleteLocalRef = jni_DeleteGlobalRef
    jni_DeleteWeakGlobalRef = jni_DeleteGlobalRef

    def jni_ExceptionCheck(self, a):
        return 0

    jni_ExceptionOccurred = jni_ExceptionCheck

    def jni_ExceptionClear(self, a):
        return 0

    jni_ExceptionDescribe = jni_ExceptionClear

    def jni_GetArrayLength(self, a):
        a.u32()
        return len(self.get(a.u32()).value)

    def jni_NewByteArray(self, a):
        a.u32()
        return self.new(JObj("array", "byte", bytearray(a.s32())))

    def jni_GetByteArrayElements(self, a):
        a.u32()
        h, iscopy = a.u32(), a.u32()
        arr = self.get(h).value
        p = self.emu.heap_alloc(len(arr) + 1)
        self.emu.write(p, bytes(arr))
        self.pinned[p] = h
        if iscopy:
            self.emu.write(iscopy, b"\1")
        return p

    def jni_ReleaseByteArrayElements(self, a):
        a.u32()
        h, p, mode = a.u32(), a.u32(), a.u32()
        arr = self.get(h).value
        if mode != 2:  # JNI_ABORT = 2
            arr[:] = self.emu.read(p, len(arr))
        if mode != 1:  # JNI_COMMIT = 1 keeps the buffer
            self.pinned.pop(p, None)
        return 0

    def jni_GetByteArrayRegion(self, a):
        a.u32()
        h, start, ln, buf = a.u32(), a.s32(), a.s32(), a.u32()
        self.emu.write(buf, bytes(self.get(h).value[start:start + ln]))
        return 0

    def jni_SetByteArrayRegion(self, a):
        a.u32()
        h, start, ln, buf = a.u32(), a.s32(), a.s32(), a.u32()
        self.get(h).value[start:start + ln] = self.emu.read(buf, ln)
        return 0

    def jni_GetJavaVM(self, a):
        a.u32()
        self.emu.w32(a.u32(), self.vm)
        return 0

    # -- JavaVM functions -------------------------------------------------------------
    def jvm_GetEnv(self, a):
        a.u32()
        self.emu.w32(a.u32(), self.env)
        return 0

    def jvm_AttachCurrentThread(self, a):
        a.u32()
        self.emu.w32(a.u32(), self.env)
        return 0

    jvm_AttachCurrentThreadAsDaemon = jvm_AttachCurrentThread

    def jvm_DetachCurrentThread(self, a):
        return 0

    # -- CallStatic<Type>Method[V|A] ------------------------------------------------------
    def _read_args(self, argtypes, a, variant):
        vals = []
        if variant == "A":
            p = a.u32()
            for i, t in enumerate(argtypes):
                slot = p + 8 * i
                if t == "J":
                    vals.append(self.emu.r32(slot) | (self.emu.r32(slot + 4) << 32))
                elif t == "D":
                    vals.append(struct.unpack("<d", self.emu.read(slot, 8))[0])
                elif t == "F":
                    vals.append(f32(self.emu.r32(slot)))
                else:
                    vals.append(s32(self.emu.r32(slot)) if t in "IBSZC" else self.emu.r32(slot))
            return vals
        src = E.VaList(self.emu, a.u32()) if variant == "V" else a
        for t in argtypes:
            if t == "J":
                vals.append(src.s64())
            elif t in ("F", "D"):           # float is promoted to double through varargs
                vals.append(src.f64())
            elif t in "IBSZC":
                vals.append(src.s32())
            else:
                vals.append(src.u32())
        return vals

    def _call_static(self, name, a):
        variant = name[-1] if name[-1] in "VA" and not name.endswith("Method") else ""
        a.u32()                     # env
        clazz, mid = a.u32(), a.u32()
        if mid not in self.methods:
            raise E.ProbeError(f"JNI {name}: unknown jmethodID {mid:#x}")
        short, mname, sig, _ = self.methods[mid]
        argtypes, ret = parse_sig(sig)
        vals = self._read_args(argtypes, a, variant)
        key = (short, mname, sig)
        self.java_calls[key] += 1
        impl = JAVA.get(key, "missing")
        shown = ", ".join(repr(self.objs[v]) if isinstance(v, int) and v in self.objs else repr(v) for v in vals)
        if impl == "missing":
            self.emu.log(f"JAVA-STUB: {short}.{mname}{sig}({shown}) is not a known Java method -> 0")
            res = 0
        elif impl is None:
            if self.java_calls[key] <= 3:
                self.emu.log(f"[java] {short}.{mname}({shown}) (no-op in probe)")
            res = 0
        else:
            res = impl(self, vals)
            if self.java_calls[key] <= 3:
                self.emu.log(f"[java] {short}.{mname}({shown}) -> {res!r}")
        if ret == "V":
            return 0
        if ret == "J":
            return split64(res)
        if ret == "D":
            return b64(float(res))
        if ret == "F":
            return b32(float(res))
        return res & 0xFFFFFFFF
