// `gangstar --run`: boot the game in a window. Same startup sequence as the probe's
// step 2a (tools/probe/probe.py), driven by a real frame loop.
//
//   GLSurfaceView lifecycle on the phone       what we call here
//   Gangster2.onCreate                         nativeSetPhone(width), nativeGetInfo(...)
//   GameRenderer.onSurfaceCreated              nativeGetJNIEnv, GLResLoader/GLMediaPlayer init,
//                                              Gangster2.nativeInit(isDemo), GameRenderer.nativeInit(1)
//   GameRenderer.onSurfaceChanged              nativeResize(width, 480)
//   first onDrawFrame                          Gangster2.nativeResume (queued focus-gained event)
//   every onDrawFrame                          nativeRender, then present + swap
//   onPause / focus regained                   nativePause / nativeResume
//
// The game is told a 480-high screen whose width follows the window's aspect ratio (it lays
// out correctly for any width at 480 high; taller sizes break its UI). It renders into a
// virtual framebuffer at N x that size (gles1::vfb_*), which is scaled into the window.
#include <SDL.h>
#include <SDL_opengl.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <unordered_map>
#include <vector>
#include <exception>
#include <filesystem>
#include <set>
#include <sstream>
#include <string>
#include <thread>

#include "core/layout.h"
#include "core/log.h"
#include "game.h"
#include "gl/gles1.h"
#include "hle/hle.h"
#include "input/input.h"
#include "jni/jni.h"
#include "rt/runtime.h"
#include "settings/settings.h"

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

gles1::Fit fit_of(const std::string& s) {
    return s == "stretch" ? gles1::Fit::Stretch : s == "integer" ? gles1::Fit::Integer : gles1::Fit::Aspect;
}

void apply_window_mode(SDL_Window* win, const settings::Settings& s) {
    if (s.window_mode == "borderless") SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP);
    else if (s.window_mode == "fullscreen") SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN);
    else {
        SDL_SetWindowFullscreen(win, 0);
        SDL_SetWindowSize(win, s.window_w, s.window_h);
        SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
}

}  // namespace

int run_game(const rt::Paths& paths, const RunOptions& opt) {
    // ---- settings ------------------------------------------------------------------------------
    const fs::path settings_file = paths.user / "settings.ini";
    settings::Settings cfg;
    if (!settings::load(settings_file, &cfg)) {
        settings::save(settings_file, cfg);
        LOG("settings: wrote defaults to %s", settings_file.string().c_str());
    } else {
        LOG("settings: loaded %s", settings_file.string().c_str());
    }
    if (opt.fps >= 0) cfg.fps_limit = opt.fps;  // command-line overrides (not saved)
    if (opt.no_vsync) cfg.vsync = false;

    // ---- window + GL ------------------------------------------------------------------------------
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) FATAL("SDL_Init: %s", SDL_GetError());
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);  // the game renders into our own framebuffer
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    SDL_Window* win = SDL_CreateWindow("Gangstar: Miami Vindication", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       cfg.window_w, cfg.window_h,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN);
    if (!win) FATAL("SDL_CreateWindow: %s", SDL_GetError());
    apply_window_mode(win, cfg);
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) FATAL("SDL_GL_CreateContext: %s", SDL_GetError());
    SDL_GL_SetSwapInterval(cfg.vsync ? 1 : 0);
    if (!gles1::load_gl()) FATAL("required OpenGL entry points are missing");
    LOG("GL: %s / %s", glGetString(GL_RENDERER), glGetString(GL_VERSION));

    int dw = 0, dh = 0;
    SDL_GL_GetDrawableSize(win, &dw, &dh);
    const int kW = opt.game_w > 0 ? opt.game_w : settings::logical_width(cfg, dw, dh);
    const int kH = 480;
    gles1::VfbConfig vc;
    vc.logical_w = kW;
    vc.logical_h = kH;
    vc.scale = cfg.render_scale > 0 ? cfg.render_scale : settings::auto_render_scale(dh);
    vc.msaa = cfg.msaa;
    if (!gles1::vfb_init(vc)) {
        LOG("GL: retrying the virtual framebuffer without MSAA");
        vc.msaa = 0;
        if (!gles1::vfb_init(vc)) FATAL("cannot create the virtual framebuffer");
    }
    jni::set_device_width(kW);
    LOG("display: window %dx%d (%s), game %dx%d rendered at x%d, msaa %d", dw, dh, cfg.window_mode.c_str(), kW, kH,
        vc.scale, vc.msaa);

    // ---- runtime ----------------------------------------------------------------------------------
    rt::Runtime rt;
    std::string err;
    if (!rt.init(paths, &err)) FATAL("runtime init failed: %s", err.c_str());
    hle::register_libc_core(rt);
    hle::register_libc_stdio(rt);
    gles1::register_handlers(rt);
    jni::install(rt);
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
        const uint32_t ia = rt.elf().dyn(25), ia_n = rt.elf().dyn(27) / 4;  // DT_INIT_ARRAY(SZ)
        auto t0 = Clock::now();
        for (uint32_t i = 0; i < ia_n; ++i) cpu.call(rt.mem().r32(layout::kLibBase + ia + 4 * i));
        LOG("%u static constructors ran (%.2f s)", ia_n, std::chrono::duration<double>(Clock::now() - t0).count());

        const uint32_t env = jni::env();
        const uint32_t c_g2 = jni::find_class(std::string(jni::kPkg) + "Gangster2");
        const uint32_t c_gr = jni::find_class(std::string(jni::kPkg) + "GameRenderer");
        const uint32_t c_mp = jni::find_class(std::string(jni::kPkg) + "GLMediaPlayer");
        const uint32_t c_rl = jni::find_class(std::string(jni::kPkg) + "GLResLoader");
        const uint32_t renderer = jni::new_instance(std::string(jni::kPkg) + "GameRenderer");
        auto native = [&](const char* cls_method, std::initializer_list<uint32_t> args,
                          std::initializer_list<uint32_t> stack = {}) {
            auto t = Clock::now();
            uint64_t r = cpu.call(rt.lib_func(std::string("Java_com_gameloft_android_GAND_GloftGMHP_ML_") + cls_method), args, stack);
            LOG("  ok  %s (%.2f s)", cls_method, std::chrono::duration<double>(Clock::now() - t).count());
            return uint32_t(r);
        };

        native("Gangster2_nativeSetPhone", {env, c_g2, uint32_t(kW)});
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
        native("GameRenderer_nativeResize", {env, c_gr, uint32_t(kW), uint32_t(kH)});

        // ---- frame loop ---------------------------------------------------------------------------
        const fs::path shot_dir = paths.root / "build" / "shots" /
                                  ("run_" + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                                                std::chrono::system_clock::now().time_since_epoch()).count()));
        std::set<int> shots_at;          // frame numbers
        std::vector<double> shots_at_t;  // seconds since the first frame ("t12.5")
        {
            std::stringstream ss(opt.shots_at);
            std::string tok;
            while (std::getline(ss, tok, ','))
                if (!tok.empty()) {
                    if (tok[0] == 't') shots_at_t.push_back(std::atof(tok.c_str() + 1));
                    else shots_at.insert(std::atoi(tok.c_str()));
                }
        }
        if (opt.shot_every || !shots_at.empty() || !shots_at_t.empty()) {
            fs::create_directories(shot_dir);
            LOG("screenshots -> %s", shot_dir.string().c_str());
        }

        input::Input in;
        in.init({&rt, env, jni::find_class(std::string(jni::kPkg) + "GameGLSurfaceView"), c_g2}, kW, kH);
        if (!opt.input.empty()) {
            std::string ierr;
            if (!in.parse_script(opt.input, &ierr)) FATAL("%s", ierr.c_str());
        }

        bool running = true, paused = false;
        const auto loop_start = Clock::now();
        bool profiling = false;
        auto last_report = Clock::now(), frame_start = Clock::now();
        int frames_since = 0;
        std::unordered_map<uint32_t, uint64_t> block_counts;
        double game_ms = 0;  // time inside nativeRender (emulated game code + GL calls) since last report
        for (; running && (opt.frames <= 0 || frame < opt.frames); ++frame) {
            frame_start = Clock::now();
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) { running = false; continue; }
                if (ev.type == SDL_KEYDOWN && !ev.key.repeat &&
                    (ev.key.keysym.sym == SDLK_F11 ||
                     (ev.key.keysym.sym == SDLK_RETURN && (ev.key.keysym.mod & KMOD_ALT)))) {
                    bool fs_now = SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN;
                    cfg.window_mode = fs_now ? "windowed" : "borderless";
                    apply_window_mode(win, cfg);
                    settings::save(settings_file, cfg);
                    continue;
                }
                if (ev.type == SDL_WINDOWEVENT && cfg.pause_when_unfocused && frame > 0) {
                    if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST && !paused) {
                        native("Gangster2_nativePause", {env, c_g2});
                        paused = true;
                    } else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED && paused) {
                        native("Gangster2_nativeResume", {env, c_g2});
                        paused = false;
                    }
                }
                in.handle(ev);
            }
            const double since_start = std::chrono::duration<double>(Clock::now() - loop_start).count();
            if (opt.seconds > 0 && since_start >= opt.seconds) break;
            if (opt.profile_guest_at >= 0 && since_start >= opt.profile_guest_at && !profiling) {
                LOG("guest profiler: on (frames run much slower while it is active)");
                cpu.enable_block_profile(&block_counts);
                profiling = true;
            }
            in.run_script(frame, since_start);
            if (frame == 0) native("Gangster2_nativeResume", {env, c_g2});
            const auto t_game = Clock::now();
            cpu.call(rt.lib_func("Java_com_gameloft_android_GAND_GloftGMHP_ML_GameRenderer_nativeRender"), {env, c_gr});
            game_ms += std::chrono::duration<double, std::milli>(Clock::now() - t_game).count();

            // present the game image into the window
            SDL_GL_GetDrawableSize(win, &dw, &dh);
            int rx, ry, rw, rh;
            gles1::vfb_present(dw, dh, fit_of(cfg.fit), cfg.smooth_scaling, &rx, &ry, &rw, &rh);
            in.set_viewport(rx, ry, rw, rh);
            bool shot_time = false;
            for (double& t : shots_at_t)
                if (t >= 0 && since_start >= t) { shot_time = true; t = -1; }
            if ((opt.shot_every && (frame % opt.shot_every == 0 || frame == opt.frames - 1)) || shots_at.count(frame) ||
                shot_time) {
                char name[64];
                std::snprintf(name, sizeof name, "frame_%05d.bmp", frame);
                gles1::screenshot((shot_dir / name).string().c_str(), dw, dh);
            }
            SDL_GL_SwapWindow(win);
            gles1::vfb_restore_binding();

            if (cfg.fps_limit > 0) {
                auto target = frame_start + std::chrono::microseconds(1000000 / cfg.fps_limit);
                std::this_thread::sleep_until(target);
            }
            ++frames_since;
            double dt = std::chrono::duration<double>(Clock::now() - last_report).count();
            if (dt >= 2.0) {
                LOG("frame %d: %.1f fps, game %.1f ms/frame, %llu draw calls total, heap %u KB", frame,
                    frames_since / dt, frames_since ? game_ms / frames_since : 0.0,
                    (unsigned long long)gles1::draw_calls(), (rt.brk() - layout::kSbrkBase) / 1024);
                game_ms = 0;
                if (opt.profile_traps) {
                    // busiest import traps since the last report (per second)
                    static std::vector<uint64_t> prev;
                    const auto& now_counts = rt.call_counts();
                    std::vector<std::pair<uint64_t, int>> d;
                    uint64_t total = 0;
                    for (size_t i = 0; i < now_counts.size(); ++i) {
                        uint64_t delta = now_counts[i] - (i < prev.size() ? prev[i] : 0);
                        total += delta;
                        if (delta) d.push_back({delta, int(i)});
                    }
                    prev = now_counts;
                    std::sort(d.rbegin(), d.rend());
                    std::string top;
                    for (size_t i = 0; i < d.size() && i < 10; ++i)
                        top += rt.imports().trap_name(d[i].second) + "=" + std::to_string(uint64_t(d[i].first / dt)) + " ";
                    LOG("  traps/s: %llu total; %s", (unsigned long long)(total / dt), top.c_str());
                }
                last_report = Clock::now();
                frames_since = 0;
            }
        }
        LOG("stopped after %d frames", frame);
        if (profiling) {
            // Aggregate block counts per game function (or per trap/fastpath region).
            std::unordered_map<std::string, uint64_t> per_fn;
            uint64_t total = 0;
            for (const auto& [addr, n] : block_counts) {
                total += n;
                std::string w = cpu.where(addr);
                size_t plus = w.rfind('+');
                per_fn[plus != std::string::npos && w.compare(0, 4, "lib+") != 0 ? w.substr(0, plus) : w] += n;
            }
            std::vector<std::pair<uint64_t, std::string>> top;
            for (auto& [k, v] : per_fn) top.push_back({v, k});
            std::sort(top.rbegin(), top.rend());
            LOG("guest profile: %llu instructions sampled; top functions:", (unsigned long long)total);
            for (size_t i = 0; i < top.size() && i < 40; ++i)
                LOG("  %5.1f%%  %s", 100.0 * top[i].first / total, top[i].second.c_str());
        }
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
