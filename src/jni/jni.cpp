#include "jni/jni.h"

#include <array>
#include <cstring>
#include <map>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/layout.h"
#include "core/log.h"
#include "rt/runtime.h"

namespace jni {

using cpu::Args;
using cpu::Cpu;

namespace {

// ---- the standard JNINativeInterface layout (233 slots) -----------------------------------------
std::vector<std::string> build_names() {
    std::vector<std::string> n = {"reserved0", "reserved1", "reserved2", "reserved3", "GetVersion", "DefineClass",
                                  "FindClass", "FromReflectedMethod", "FromReflectedField", "ToReflectedMethod",
                                  "GetSuperclass", "IsAssignableFrom", "ToReflectedField", "Throw", "ThrowNew",
                                  "ExceptionOccurred", "ExceptionDescribe", "ExceptionClear", "FatalError",
                                  "PushLocalFrame", "PopLocalFrame", "NewGlobalRef", "DeleteGlobalRef",
                                  "DeleteLocalRef", "IsSameObject", "NewLocalRef", "EnsureLocalCapacity",
                                  "AllocObject", "NewObject", "NewObjectV", "NewObjectA", "GetObjectClass",
                                  "IsInstanceOf", "GetMethodID"};
    const char* types[] = {"Object", "Boolean", "Byte", "Char", "Short", "Int", "Long", "Float", "Double", "Void"};
    const char* prims[] = {"Boolean", "Byte", "Char", "Short", "Int", "Long", "Float", "Double"};
    const char* fields[] = {"Object", "Boolean", "Byte", "Char", "Short", "Int", "Long", "Float", "Double"};
    for (const char* t : types) for (const char* v : {"", "V", "A"}) n.push_back(std::string("Call") + t + "Method" + v);
    for (const char* t : types) for (const char* v : {"", "V", "A"}) n.push_back(std::string("CallNonvirtual") + t + "Method" + v);
    n.push_back("GetFieldID");
    for (const char* t : fields) n.push_back(std::string("Get") + t + "Field");
    for (const char* t : fields) n.push_back(std::string("Set") + t + "Field");
    n.push_back("GetStaticMethodID");
    for (const char* t : types) for (const char* v : {"", "V", "A"}) n.push_back(std::string("CallStatic") + t + "Method" + v);
    n.push_back("GetStaticFieldID");
    for (const char* t : fields) n.push_back(std::string("GetStatic") + t + "Field");
    for (const char* t : fields) n.push_back(std::string("SetStatic") + t + "Field");
    for (const char* s : {"NewString", "GetStringLength", "GetStringChars", "ReleaseStringChars", "NewStringUTF",
                          "GetStringUTFLength", "GetStringUTFChars", "ReleaseStringUTFChars", "GetArrayLength",
                          "NewObjectArray", "GetObjectArrayElement", "SetObjectArrayElement"})
        n.push_back(s);
    for (const char* p : prims) n.push_back(std::string("New") + p + "Array");
    for (const char* p : prims) n.push_back(std::string("Get") + p + "ArrayElements");
    for (const char* p : prims) n.push_back(std::string("Release") + p + "ArrayElements");
    for (const char* p : prims) n.push_back(std::string("Get") + p + "ArrayRegion");
    for (const char* p : prims) n.push_back(std::string("Set") + p + "ArrayRegion");
    for (const char* s : {"RegisterNatives", "UnregisterNatives", "MonitorEnter", "MonitorExit", "GetJavaVM",
                          "GetStringRegion", "GetStringUTFRegion", "GetPrimitiveArrayCritical",
                          "ReleasePrimitiveArrayCritical", "GetStringCritical", "ReleaseStringCritical",
                          "NewWeakGlobalRef", "DeleteWeakGlobalRef", "ExceptionCheck", "NewDirectByteBuffer",
                          "GetDirectBufferAddress", "GetDirectBufferCapacity", "GetObjectRefType"})
        n.push_back(s);
    return n;
}
const std::vector<std::string> kNames = build_names();
constexpr size_t kSlots = 233;
const char* kVmNames[] = {"reserved0", "reserved1", "reserved2", "DestroyJavaVM", "AttachCurrentThread",
                          "DetachCurrentThread", "GetEnv", "AttachCurrentThreadAsDaemon"};

// ---- Java objects ---------------------------------------------------------------------------
struct JObj {
    enum Kind { Class, Object, String, ByteArray, Method, Field } kind;
    std::string cls;              // class name (Class/Object), or method/field owner
    std::string text;             // String value; Method/Field name
    std::string sig;              // Method/Field signature
    std::vector<uint8_t> bytes;   // ByteArray
};

rt::Runtime* g_rt = nullptr;
uint32_t g_env = 0, g_vm = 0;
uint32_t g_next_handle = layout::kHandleBase + 0x100;
std::unordered_map<uint32_t, JObj> g_objs;
std::unordered_map<std::string, uint32_t> g_classes;
std::map<std::string, uint64_t> g_jni_calls;
std::map<std::string, uint64_t> g_java_calls;
std::unordered_map<uint32_t, uint32_t> g_pinned;  // guest buffer -> array handle

uint32_t new_obj(JObj o) {
    uint32_t h = g_next_handle;
    g_next_handle += 4;
    g_objs.emplace(h, std::move(o));
    return h;
}

JObj& obj(uint32_t h) {
    auto it = g_objs.find(h);
    if (it == g_objs.end()) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "JNI: unknown object handle %08x", h);
        throw std::runtime_error(buf);
    }
    return it->second;
}

std::string short_name(const std::string& cls) {
    size_t p = cls.rfind('/');
    return p == std::string::npos ? cls : cls.substr(p + 1);
}

// ---- the Java side ------------------------------------------------------------------------------
struct JVal {
    int64_t i = 0;
    double d = 0;
    bool is_fp = false;
};
using JavaFn = JVal (*)(const std::vector<JVal>& args);
JVal ret_i(int64_t v) { JVal r; r.i = v; return r; }

std::string resource_path(const std::string& s) {
    std::string p = s.rfind(".//", 0) == 0 ? s.substr(3) : s.rfind("./", 0) == 0 ? s.substr(2) : s;
    return "/sdcard/gameloft/games/gangster2/" + p;
}

struct JavaMethod {
    JavaFn fn;  // nullptr = documented no-op in this port so far
};

const std::map<std::tuple<std::string, std::string, std::string>, JavaMethod>& java_methods() {
    static const auto* m = [] {
        auto* t = new std::map<std::tuple<std::string, std::string, std::string>, JavaMethod>();
        auto add = [&](const char* c, const char* n, const char* s, JavaFn f = nullptr) { (*t)[{c, n, s}] = {f}; };
        // Gangster2: lifecycle / store / trial state (values = decompiled Java on a fresh install)
        for (auto [n, s] : std::initializer_list<std::pair<const char*, const char*>>{
                 {"Exit", "()V"}, {"sendAppToBackground", "()V"}, {"openBrowser", "(Ljava/lang/String;)V"},
                 {"launchIGP", "(I)V"}, {"OpenGLive", "(I)V"}, {"NotifyTrophy", "(I)V"}, {"lockDemo", "()V"},
                 {"IncreaseLaunchTimes", "()V"}, {"VZInitMobileNetwork", "()V"}, {"VZRestoreNetworkState", "()V"},
                 {"VZRequestLogin", "()V"}, {"VZRequestPurchaseGame", "()V"}, {"VZGetGamePrice", "()[B"},
                 {"VZGetGameName", "()[B"}, {"VZGetLastServerMsg", "()[B"}})
            add("Gangster2", n, s);
        add("Gangster2", "isWifiAlive", "()I", [](auto&) { return ret_i(0); });
        add("Gangster2", "unlockDemo", "()I", [](auto&) { return ret_i(0); });
        add("Gangster2", "DisableLaunchGame", "()I", [](auto&) { return ret_i(0); });
        add("Gangster2", "VZIsInProgress", "()I", [](auto&) { return ret_i(0); });
        add("Gangster2", "VZIsErrorOcurred", "()I", [](auto&) { return ret_i(1); });
        add("Gangster2", "VZIsMobileNetworkReady", "()I", [](auto&) { return ret_i(0); });
        // GLMediaPlayer: device info (audio is a no-op until Phase 3)
        add("GLMediaPlayer", "GetDeviceType", "()I", [](auto&) { return ret_i(-1); });   // non-Motorola/Samsung/HTC
        add("GLMediaPlayer", "detectPhoneLang", "()I", [](auto&) { return ret_i(0); });  // English
        add("GLMediaPlayer", "getDeviceWidth", "()I", [](auto&) { return ret_i(800); });
        add("GLMediaPlayer", "loadMovie", "(Ljava/lang/String;)I", [](auto&) { return ret_i(1); });
        add("GLMediaPlayer", "isMediaPlaying", "(I)I", [](auto&) { return ret_i(0); });
        add("GLMediaPlayer", "isSoundLoaded", "(II)I", [](auto&) { return ret_i(0); });
        add("GLMediaPlayer", "isSoundLoadedBig", "(I)I", [](auto&) { return ret_i(0); });
        for (auto [n, s] : std::initializer_list<std::pair<const char*, const char*>>{
                 {"loadSound", "(II)V"}, {"loadSoundBig", "(I)V"}, {"unloadSound", "(II)V"}, {"unloadSoundBig", "(I)V"},
                 {"playSound", "(IIFF)V"}, {"playSoundBig", "(IFI)V"}, {"pauseSound", "(II)V"}, {"pauseSoundBig", "(I)V"},
                 {"resumeSound", "(II)V"}, {"resumeSoundBig", "(I)V"}, {"stopSound", "(II)V"}, {"stopSoundBig", "(I)V"},
                 {"setVolume", "(IIF)V"}, {"setVolumeBig", "(IF)V"}, {"setPitch", "(IIF)V"}, {"resetSound", "(I)V"},
                 {"stopAllSounds", "()V"}, {"pauseAllSounds", "()V"}, {"resumeAllSounds", "()V"},
                 {"stopAllPool", "(I)V"}, {"stopAllBig", "(I)V"}, {"destroySoundPool", "()V"},
                 {"initSoundPoolArray", "()V"}, {"setMusicGain", "(F)V"}, {"setSfxGain", "(F)V"}, {"setVfxGain", "(F)V"}})
            add("GLMediaPlayer", n, s);
        // GLResLoader: never used during boot (the game reads files itself); implemented in Phase 3 if needed
        add("GLResLoader", "getResourceFull", "(Ljava/lang/String;)[B");
        add("GLResLoader", "getResourceBytes", "(Ljava/lang/String;II)[B");
        add("GLResLoader", "getResourceLength", "(Ljava/lang/String;)I");
        return t;
    }();
    return *m;
}

std::vector<std::string> parse_sig(const std::string& sig, std::string* ret) {
    std::vector<std::string> args;
    size_t i = 1;
    while (i < sig.size() && sig[i] != ')') {
        size_t s = i;
        while (sig[i] == '[') ++i;
        if (sig[i] == 'L') i = sig.find(';', i);
        ++i;
        args.push_back(sig.substr(s, i - s));
    }
    *ret = sig.substr(i + 1);
    return args;
}

// ---- JNI functions ------------------------------------------------------------------------------
void call_static(Cpu& c, Args& a, const std::string& name) {
    const char variant = name.back() == 'V' ? 'V' : name.back() == 'A' ? 'A' : 0;
    a.u32();  // env
    a.u32();  // clazz
    uint32_t mid = a.u32();
    JObj& m = obj(mid);
    std::string ret;
    std::vector<std::string> types = parse_sig(m.sig, &ret);
    std::vector<JVal> vals;
    auto& mem = c.rt().mem();
    if (variant == 'A') {
        uint32_t p = a.u32();
        for (size_t i = 0; i < types.size(); ++i) {
            JVal v;
            uint32_t slot = p + 8 * uint32_t(i);
            if (types[i] == "J") v.i = int64_t(mem.r32(slot) | (uint64_t(mem.r32(slot + 4)) << 32));
            else if (types[i] == "D") { std::memcpy(&v.d, mem.ptr(slot), 8); v.is_fp = true; }
            else if (types[i] == "F") { float f; std::memcpy(&f, mem.ptr(slot), 4); v.d = f; v.is_fp = true; }
            else v.i = int32_t(mem.r32(slot));
            vals.push_back(v);
        }
    } else {
        Args va = variant == 'V' ? Args::from_va_list(c, a.u32()) : a;
        Args& src = variant == 'V' ? va : a;
        for (const std::string& t : types) {
            JVal v;
            if (t == "J") v.i = src.s64();
            else if (t == "F" || t == "D") { v.d = src.f64(); v.is_fp = true; }  // float promoted to double
            else if (t.size() == 1) v.i = src.s32();
            else v.i = src.u32();  // object reference
            vals.push_back(v);
        }
    }
    const std::string key = short_name(m.cls) + "." + m.text + m.sig;
    uint64_t n = ++g_java_calls[key];
    auto it = java_methods().find({short_name(m.cls), m.text, m.sig});
    JVal r;
    if (it == java_methods().end()) {
        LOG("JAVA-STUB: %s is not a known Java method -> 0", key.c_str());
    } else if (it->second.fn) {
        r = it->second.fn(vals);
        if (n <= 3) LOG("[java] %s -> %lld", key.c_str(), (long long)r.i);
    } else if (n <= 3) {
        LOG("[java] %s (no-op in this port so far)", key.c_str());
    }
    if (ret == "V") return c.ret(0);
    if (ret == "J") return c.ret64(uint64_t(r.i));
    if (ret == "D") return c.ret_d(r.is_fp ? r.d : double(r.i));
    if (ret == "F") return c.ret_f(float(r.is_fp ? r.d : double(r.i)));
    c.ret(uint32_t(r.i));
}

uint32_t lookup(Args& a, bool is_static) {
    a.u32();
    JObj& cls = obj(a.u32());
    auto& mem = g_rt->mem();
    std::string name = mem.cstr(a.u32()), sig = mem.cstr(a.u32());
    bool known = java_methods().count({short_name(cls.cls), name, sig}) != 0;
    if (!known) LOG("[jni] Get%sMethodID(%s, %s, %s): NOT in the decompiled Java", is_static ? "Static" : "",
                    cls.cls.c_str(), name.c_str(), sig.c_str());
    return new_obj({JObj::Method, cls.cls, name, sig, {}});
}

void jni_slot(size_t slot, Cpu& c, Args& a) {
    const std::string& name = kNames[slot];
    ++g_jni_calls[name];
    auto& mem = c.rt().mem();
    if (name.rfind("CallStatic", 0) == 0) return call_static(c, a, name);
    if (name == "GetVersion") return c.ret(0x00010006);
    if (name == "FindClass") { a.u32(); return c.ret(find_class(mem.cstr(a.u32()))); }
    if (name == "GetObjectClass") { a.u32(); JObj& o = obj(a.u32()); return c.ret(find_class(o.cls)); }
    if (name == "GetStaticMethodID") return c.ret(lookup(a, true));
    if (name == "GetMethodID") return c.ret(lookup(a, false));
    if (name == "NewStringUTF") { a.u32(); uint32_t p = a.u32(); return c.ret(p ? new_string(mem.cstr(p)) : 0); }
    if (name == "GetStringUTFChars") {
        a.u32();
        uint32_t h = a.u32(), iscopy = a.u32();
        const std::string& s = obj(h).text;
        uint32_t p = c.rt().heap().alloc(uint32_t(s.size()) + 1);
        mem.write(p, s.c_str(), s.size() + 1);
        if (iscopy) mem.w8(iscopy, 1);
        return c.ret(p);
    }
    if (name == "ReleaseStringUTFChars" || name == "DeleteGlobalRef" || name == "DeleteLocalRef" ||
        name == "DeleteWeakGlobalRef" || name == "ExceptionClear" || name == "ExceptionDescribe" ||
        name == "ExceptionCheck" || name == "ExceptionOccurred")
        return c.ret(0);
    if (name == "GetStringUTFLength") { a.u32(); return c.ret(uint32_t(obj(a.u32()).text.size())); }
    if (name == "NewGlobalRef" || name == "NewLocalRef" || name == "NewWeakGlobalRef") { a.u32(); return c.ret(a.u32()); }
    if (name == "GetArrayLength") { a.u32(); return c.ret(uint32_t(obj(a.u32()).bytes.size())); }
    if (name == "NewByteArray") { a.u32(); JObj o{JObj::ByteArray}; o.bytes.resize(a.u32()); return c.ret(new_obj(std::move(o))); }
    if (name == "GetByteArrayElements") {
        a.u32();
        uint32_t h = a.u32(), iscopy = a.u32();
        auto& b = obj(h).bytes;
        uint32_t p = c.rt().heap().alloc(uint32_t(b.size()) + 1);
        if (!b.empty()) mem.write(p, b.data(), b.size());
        g_pinned[p] = h;
        if (iscopy) mem.w8(iscopy, 1);
        return c.ret(p);
    }
    if (name == "ReleaseByteArrayElements") {
        a.u32();
        uint32_t h = a.u32(), p = a.u32(), mode = a.u32();
        auto& b = obj(h).bytes;
        if (mode != 2 && !b.empty()) mem.read(p, b.data(), b.size());  // JNI_ABORT = 2
        return c.ret(0);
    }
    if (name == "GetByteArrayRegion" || name == "SetByteArrayRegion") {
        a.u32();
        uint32_t h = a.u32();
        int32_t start = a.s32(), len = a.s32();
        uint32_t buf = a.u32();
        auto& b = obj(h).bytes;
        if (name[0] == 'G') mem.write(buf, b.data() + start, size_t(len));
        else mem.read(buf, b.data() + start, size_t(len));
        return c.ret(0);
    }
    if (name == "GetJavaVM") { a.u32(); mem.w32(a.u32(), g_vm); return c.ret(0); }
    LOG("JNI-STUB: %s(%#x, %#x, %#x, %#x) -> 0 [from %s]", name.c_str(), a.raw(0), a.raw(1), a.raw(2), a.raw(3),
        c.where(c.reg(cpu::LR)).c_str());
    c.ret(0);
}

void vm_slot(size_t slot, Cpu& c, Args& a) {
    std::string name = std::string("JVM::") + kVmNames[slot];
    ++g_jni_calls[name];
    if (slot == 4 || slot == 6 || slot == 7) {  // AttachCurrentThread, GetEnv, AttachCurrentThreadAsDaemon
        a.u32();
        c.rt().mem().w32(a.u32(), g_env);
        return c.ret(0);
    }
    if (slot == 5) return c.ret(0);  // DetachCurrentThread
    LOG("JNI-STUB: %s", name.c_str());
    c.ret(0);
}

template <size_t N> void jni_entry(Cpu& c, Args& a) { jni_slot(N, c, a); }
template <size_t N> void vm_entry(Cpu& c, Args& a) { vm_slot(N, c, a); }
template <size_t... I>
constexpr std::array<cpu::Handler, sizeof...(I)> jni_table(std::index_sequence<I...>) { return {&jni_entry<I>...}; }
template <size_t... I>
constexpr std::array<cpu::Handler, sizeof...(I)> vm_table(std::index_sequence<I...>) { return {&vm_entry<I>...}; }

}  // namespace

void install(rt::Runtime& rt) {
    g_rt = &rt;
    if (kNames.size() != kSlots) FATAL("JNI table has %zu slots, expected %zu", kNames.size(), kSlots);
    auto& mem = rt.mem();
    auto& da = rt.imports().data_alloc();
    static constexpr auto kJni = jni_table(std::make_index_sequence<kSlots>{});
    static constexpr auto kVm = vm_table(std::make_index_sequence<8>{});
    uint32_t table = da.alloc(uint32_t(kSlots) * 4, 4);
    for (size_t i = 0; i < kSlots; ++i) mem.w32(table + 4 * uint32_t(i), rt.add_trap("JNI::" + kNames[i], kJni[i]));
    g_env = da.alloc(4, 4);
    mem.w32(g_env, table);
    uint32_t vt = da.alloc(8 * 4, 4);
    for (size_t i = 0; i < 8; ++i) mem.w32(vt + 4 * uint32_t(i), rt.add_trap(std::string("JVM::") + kVmNames[i], kVm[i]));
    g_vm = da.alloc(4, 4);
    mem.w32(g_vm, vt);
}

uint32_t env() { return g_env; }

uint32_t find_class(const std::string& name) {
    auto it = g_classes.find(name);
    if (it != g_classes.end()) return it->second;
    uint32_t h = new_obj({JObj::Class, name, {}, {}, {}});
    g_classes.emplace(name, h);
    return h;
}

uint32_t new_instance(const std::string& cls) { return new_obj({JObj::Object, cls, {}, {}, {}}); }
uint32_t new_string(const std::string& s) { return new_obj({JObj::String, "java/lang/String", s, {}, {}}); }

std::string string_of(uint32_t h) {
    auto it = g_objs.find(h);
    return it != g_objs.end() && it->second.kind == JObj::String ? it->second.text : std::string();
}

void report() {
    std::string line;
    for (auto& [k, v] : g_jni_calls) line += k + "x" + std::to_string(v) + " ";
    LOG("JNI functions used: %s", line.c_str());
    line.clear();
    for (auto& [k, v] : g_java_calls) line += k + "x" + std::to_string(v) + " ";
    LOG("Java callbacks invoked: %s", line.c_str());
}

}  // namespace jni
