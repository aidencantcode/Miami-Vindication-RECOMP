// `gangstar --run`: boot the game in a window. Same startup sequence as the probe's
// step 2a (tools/probe/probe.py), driven by a real frame loop.
//
//   GLSurfaceView lifecycle on the phone       what we call here
//   Gangster2.onCreate                         nativeSetPhone(800), nativeGetInfo(...)
//   GameRenderer.onSurfaceCreated              nativeGetJNIEnv, GLResLoader/GLMediaPlayer init,
//                                              Gangster2.nativeInit(isDemo), GameRenderer.nativeInit(1)
//   GameRenderer.onSurfaceChanged              nativeResize(800, 480)
//   first onDrawFrame                          Gangster2.nativeResume (queued focus-gained event)
//   every onDrawFrame                          nativeRender, then swap
#include <SDL.h>
#include <SDL_opengl.h>

#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>

#include "core/layout.h"
#include "core/log.h"
#include "game.h"
#include "gl/gles1.h"
#include "hle/hle.h"
#include "jni/jni.h"
#include "rt/runtime.h"

namespace fs = std::filesystem;

int run_game(const rt::Paths& paths, const RunOptions& opt) {
    constexpr int kW = 800, kH = 480;  // LG P990 panel, landscape
    if (SDL_Init(SDL_INIT_VIDEO) != 0) FATAL("SDL_Init: %s", SDL_GetError());
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_Window* win = SDL_CreateWindow("Gangstar: Miami Vindication (native)", SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED, kW, kH, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
    if (!win) FATAL("SDL_CreateWindow: %s", SDL_GetError());
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) FATAL("SDL_GL_CreateContext: %s", SDL_GetError());
    SDL_GL_SetSwapInterval(1);
    if (!gles1::load_gl()) FATAL("required OpenGL entry points are missing");
    LOG("GL: %s / %s", glGetString(GL_RENDERER), glGetString(GL_VERSION));

    rt::Runtime rt;
    std::string err;
    if (!rt.init(paths, &err)) FATAL("runtime init failed: %s", err.c_str());
    hle::register_libc_core(rt);
    hle::register_libc_stdio(rt);
    gles1::register_handlers(rt);
    jni::install(rt);

    // Imports that still have no host implementation (they log STUB: if ever called).
    {
        std::string missing;
        int n = 0;
        for (const auto& s : rt.elf().dynsyms())
            if (s.undefined() && !s.name.empty() && rt.imports().trap_addr(s.name) && !rt.has_handler(s.name))
                missing += s.name + " ", ++n;
        LOG("%d function imports without a host implementation%s%s", n, n ? ": " : "", missing.c_str());
    }

    cpu::Cpu& cpu = rt.main_cpu();
    int rc = 0;
    int frame = 0;
    try {
        // Static constructors (DT_INIT_ARRAY)
        const uint32_t ia = rt.elf().dyn(25), ia_n = rt.elf().dyn(27) / 4;
        auto t0 = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < ia_n; ++i) cpu.call(rt.mem().r32(layout::kLibBase + ia + 4 * i));
        LOG("%u static constructors ran (%.2f s)", ia_n,
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());

        const uint32_t env = jni::env();
        const uint32_t c_g2 = jni::find_class(std::string(jni::kPkg) + "Gangster2");
        const uint32_t c_gr = jni::find_class(std::string(jni::kPkg) + "GameRenderer");
        const uint32_t c_mp = jni::find_class(std::string(jni::kPkg) + "GLMediaPlayer");
        const uint32_t c_rl = jni::find_class(std::string(jni::kPkg) + "GLResLoader");
        const uint32_t renderer = jni::new_instance(std::string(jni::kPkg) + "GameRenderer");
        auto native = [&](const char* cls_method, std::initializer_list<uint32_t> args,
                          std::initializer_list<uint32_t> stack = {}) {
            auto t = std::chrono::steady_clock::now();
            uint64_t r = cpu.call(rt.lib_func(std::string("Java_com_gameloft_android_GAND_GloftGMHP_ML_") + cls_method), args, stack);
            LOG("  ok  %s (%.2f s)", cls_method, std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count());
            return uint32_t(r);
        };

        native("Gangster2_nativeSetPhone", {env, c_g2, kW});
        native("Gangster2_nativeGetInfo", {env, c_g2, jni::new_string("US"), 0},
               {jni::new_string("LGE_LG-P990"), jni::new_string("2.2.2")});
        native("GameRenderer_nativeGetJNIEnv", {env, renderer});
        native("GLResLoader_nativeInit", {env, c_rl, 0});
        native("GLMediaPlayer_nativeInit", {env, c_mp, 0});
        uint32_t sounds = native("GLMediaPlayer_nativeGetTotalSounds", {env, c_mp});
        LOG("  nativeGetTotalSounds() = %u", sounds);
        native("GLMediaPlayer_nativeGetTotalSoundsOfSameInstance", {env, c_mp});
        native("Gangster2_nativeInit", {env, c_g2, uint32_t(opt.demo)});
        native("GameRenderer_nativeInit", {env, c_gr, 1});
        native("GameRenderer_nativeResize", {env, c_gr, kW, kH});

        // Each run gets its own screenshot folder: build/shots/run_<unix time>/
        const fs::path shot_dir = paths.root / "build" / "shots" /
                                  ("run_" + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                                                std::chrono::system_clock::now().time_since_epoch()).count()));
        if (opt.shot_every) {
            fs::create_directories(shot_dir);
            LOG("screenshots -> %s", shot_dir.string().c_str());
        }
        bool running = true;
        auto last_report = std::chrono::steady_clock::now();
        int frames_since = 0;
        for (; running && (opt.frames <= 0 || frame < opt.frames); ++frame) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev))
                if (ev.type == SDL_QUIT) running = false;
            if (frame == 0) native("Gangster2_nativeResume", {env, c_g2});
            cpu.call(rt.lib_func("Java_com_gameloft_android_GAND_GloftGMHP_ML_GameRenderer_nativeRender"), {env, c_gr});
            if (opt.shot_every && (frame % opt.shot_every == 0 || frame == opt.frames - 1)) {
                char path[64];
                std::snprintf(path, sizeof path, "frame_%05d.bmp", frame);
                gles1::screenshot((shot_dir / path).string().c_str(), kW, kH);
            }
            SDL_GL_SwapWindow(win);
            ++frames_since;
            auto now = std::chrono::steady_clock::now();
            double dt = std::chrono::duration<double>(now - last_report).count();
            if (dt >= 2.0) {
                LOG("frame %d: %.1f fps, %llu draw calls total, heap %u KB", frame, frames_since / dt,
                    (unsigned long long)gles1::draw_calls(), (rt.brk() - layout::kSbrkBase) / 1024);
                last_report = now;
                frames_since = 0;
            }
        }
        LOG("stopped after %d frames", frame);
    } catch (const std::exception& e) {
        LOG("FATAL at frame %d: %s", frame, e.what());
        rc = 1;
    }
    jni::report();
    gles1::report();
    std::string stubs;
    for (const auto& s : rt.stubbed()) stubs += s + " ";
    LOG("imports that hit a plain STUB: %s", stubs.empty() ? "none" : stubs.c_str());

    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return rc;
}
