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
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>
#include <exception>
#include <filesystem>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>

#include "audio/media_player.h"
#include "audio/mixer.h"
#include "core/layout.h"
#include "core/log.h"
#include "game.h"
#include "game/mods.h"
#include "game/night_textures.h"
#include "gl/gles1.h"
#include "hle/hle.h"
#include "hle/overrides.h"
#include "input/input.h"
#include "jni/jni.h"
#include "rt/runtime.h"
#include "recomp/rc_runtime.h"
#include "settings/settings.h"
#include "ui/overlay.h"

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

gles1::TextureFilter filter_of(const std::string& s) {
    return s == "nearest" ? gles1::TextureFilter::Nearest : s == "bilinear" ? gles1::TextureFilter::Bilinear
         : s == "trilinear" ? gles1::TextureFilter::Trilinear : gles1::TextureFilter::Game;
}

gles1::Fit fit_of(const std::string& s) {
    return s == "stretch" ? gles1::Fit::Stretch : s == "integer" ? gles1::Fit::Integer : gles1::Fit::Aspect;
}

// Internal resolution: render_scale N = N x the game's 480p screen (supersampling when larger than
// the window); 0 = native: exactly the size the image is shown at in the window, so presenting it
// is a 1:1 copy with no resampling (never below the game's own 480p).
void internal_size(const settings::Settings& s, int kW, int kH, int dw, int dh, int* w, int* h) {
    if (s.render_scale > 0) {
        *w = kW * s.render_scale;
        *h = kH * s.render_scale;
        return;
    }
    int x, y;
    gles1::fit_rect(kW, kH, dw, dh, fit_of(s.fit), &x, &y, w, h);
    *w = std::max(*w, kW);
    *h = std::max(*h, kH);
}

void apply_window_mode(SDL_Window* win, const settings::Settings& s) {
    if (s.window_mode == "borderless") SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP);
    else if (s.window_mode == "fullscreen") {
        // Exclusive fullscreen at the monitor's own resolution and refresh rate. Without this SDL
        // switches the display to the windowed size (e.g. 1280x720) and the monitor upscales it.
        SDL_DisplayMode dm;
        if (SDL_GetDesktopDisplayMode(std::max(0, SDL_GetWindowDisplayIndex(win)), &dm) == 0)
            SDL_SetWindowDisplayMode(win, &dm);
        SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN);
    } else {
        SDL_SetWindowFullscreen(win, 0);
        SDL_SetWindowSize(win, s.window_w, s.window_h);
        SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
}

void apply_audio_volumes(const settings::Settings& s) {
    audio::set_master_volume(s.master_volume);
    audio::set_group_volume(audio::kMusic, s.music_volume);
    audio::set_group_volume(audio::kSfx, s.sfx_volume);
    audio::set_group_volume(audio::kVoice, s.voice_volume);
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
    if (!opt.touch_controls.empty()) cfg.touch_controls = opt.touch_controls;
    if (!opt.quality.empty()) cfg.quality = opt.quality;
    for (const std::string& o : opt.set)
        if (!settings::apply_override(&cfg, o)) FATAL("--set %s: unknown setting (expected section.key=value)", o.c_str());

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
    internal_size(cfg, kW, kH, dw, dh, &vc.width, &vc.height);
    vc.msaa = cfg.msaa;
    if (!gles1::vfb_init(vc)) {
        LOG("GL: retrying the virtual framebuffer without MSAA");
        vc.msaa = 0;
        if (!gles1::vfb_init(vc)) FATAL("cannot create the virtual framebuffer");
    }
    jni::set_device_width(kW);
    LOG("display: window %dx%d (%s), game %dx%d rendered at %dx%d (%s), msaa %d", dw, dh, cfg.window_mode.c_str(), kW,
        kH, vc.width, vc.height, cfg.render_scale > 0 ? (std::to_string(cfg.render_scale) + "x").c_str() : "native",
        vc.msaa);
    gles1::set_anisotropy(cfg.anisotropy);
    gles1::set_texture_filter(filter_of(cfg.texture_filter));
    ui::init(win, ctx);
    nighttex::init(paths.data);  // the game's unused night city textures (game/night_textures.h)

    // ---- runtime ----------------------------------------------------------------------------------
    rt::Runtime rt;
    std::string err;
    if (!rt.init(paths, &err, /*fastpath=*/!opt.recomp && !opt.no_fastpath)) FATAL("runtime init failed: %s", err.c_str());
    hle::register_libc_core(rt);
    hle::register_libc_stdio(rt);
    gles1::register_handlers(rt);
    hle::overrides::set_dir(paths.sandbox / "overrides");
    jni::install(rt);
    if (!opt.no_mods) mods::install(rt, cfg);
    else LOG("mods: disabled (--no-mods)");
    if (const char* probe = std::getenv("GANGSTAR_PROBE")) {
        // Backend comparison (tools/recomp/mem_diff.py): dump guest memory + registers at the N-th
        // entry of a game function. Recomp mode needs that function generated with --probe.
        const char* hit_env = std::getenv("GANGSTAR_PROBE_HIT");
        const char* out_env = std::getenv("GANGSTAR_PROBE_OUT");
        const uint64_t hit = hit_env ? std::strtoull(hit_env, nullptr, 10) : 0;
        const std::string out = out_env ? out_env : (paths.root / "build" / "probe.mem").string();
        const std::string name = probe;
        const bool at_exit = std::getenv("GANGSTAR_PROBE_EXIT") != nullptr;  // dump when hit N returns
        auto count = std::make_shared<uint64_t>(0);
        rt::Runtime::FunctionHook h;
        h.pre = [&rt, hit, out, name, count, at_exit](cpu::Cpu& c) {
            const uint64_t n = ++*count;
            if (n <= 20 || (n & (n - 1)) == 0) LOG("probe: %s hit %llu", name.c_str(), (unsigned long long)n);
            if (n == hit && !at_exit) rt.dump_state(c, out, name + " hit " + std::to_string(n));
            return false;
        };
        if (at_exit)
            h.post = [&rt, hit, out, name, count](cpu::Cpu& c) {
                if (*count == hit) rt.dump_state(c, out, name + " hit " + std::to_string(hit) + " (exit)");
            };
        rt.hook_function(name, std::move(h));
        LOG("probe: %s armed (dump at hit %llu -> %s)", probe, (unsigned long long)hit, out.c_str());
    }
    {  // audio: the Java GLMediaPlayer class on our mixer
        media::Config mc;
        mc.data_dir = paths.data;
        mc.table_file = paths.root / "work" / "sound_table.txt";
        mc.restore_missing = cfg.restore_missing_sounds;
        mc.native_name = [&rt](int i) -> std::string {  // the game's own name for sound i (FileManager)
            const uint32_t p = uint32_t(rt.main_cpu().call(rt.lib_func("_Z23AndroidGetSoundFielNamei"), {uint32_t(i)}));
            if (!p) return {};
            const char* s = reinterpret_cast<const char*>(rt.mem().ptr(p));
            return std::string(s, strnlen(s, 256));
        };
        if (!media::init(mc)) LOG("audio: disabled (no output device)");
        apply_audio_volumes(cfg);
        if (!opt.audio_dump.empty()) audio::start_capture();
    }
    {
        std::string missing;
        int n = 0;
        for (const auto& s : rt.elf().dynsyms())
            if (s.undefined() && !s.name.empty() && rt.imports().trap_addr(s.name) && !rt.has_handler(s.name))
                missing += s.name + " ", ++n;
        LOG("%d function imports without a host implementation%s%s", n, n ? ": " : "", missing.c_str());
    }

    cpu::Cpu& cpu = rt.main_cpu();
    const uint32_t daytime_obj = rt.lib_object("_ZN7DayTime9s_dayTimeE");  // DayTime::s_dayTime, state at +4
    if (opt.recomp) {
#ifdef GANGSTAR_HAVE_RECOMP
        if (!recomp::bind(rt, cpu, &err)) FATAL("recomp: %s", err.c_str());
#else
        FATAL("--cpu recomp: this build has no recompiled code (configure with -DGANGSTAR_RECOMP=ON)");
#endif
    }
    LOG("cpu backend: %s", opt.recomp ? "recompiled code (--cpu unicorn: emulator)" : "Unicorn emulator");
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
        in.init({&rt, env, jni::find_class(std::string(jni::kPkg) + "GameGLSurfaceView"), c_g2}, kW, kH, cfg);
        if (!opt.input.empty()) {
            std::string ierr;
            if (!in.parse_script(opt.input, &ierr)) FATAL("%s", ierr.c_str());
        }

        bool running = true, paused = false;
        const auto loop_start = Clock::now();
        bool profiling = false;
        double dump_hud_at = opt.dump_hud_at;
        auto last_report = Clock::now(), frame_start = Clock::now(), prev_frame_start = Clock::now();
        int frames_since = 0;
        auto next_deadline = Clock::now();  // frame limiter
        Uint32 input_ticks = 0;             // SDL time of the newest input event this frame
        double lat_sum = 0;                 // input latency samples since the last stats update
        int lat_n = 0;
        auto stat_start = Clock::now();  // overlay statistics, refreshed every 0.5 s
        int stat_frames = 0;
        double stat_game_ms = 0;
        ui::FrameStats stats;
        stats.game_w = kW;
        stats.game_h = kH;
        bool menu_was_open = false;
        bool fast_forward = false, l3_held = false, r3_held = false, r3_chord = false;
        auto toggle_ff = [&] {
            fast_forward = !fast_forward;
            const int speed = std::clamp(cfg.fast_forward_speed, 2, 8);
            hle::set_time_scale(fast_forward ? speed : 1.0);
            stats.fast_forward = fast_forward ? speed : 0;
            ui::toast(fast_forward ? "Fast forward " + std::to_string(speed) + "x" : std::string("Normal speed"));
        };
        if (opt.fast_forward) toggle_ff();
        double open_settings_at = opt.open_settings_at;
        std::unordered_map<uint32_t, uint64_t> block_counts;
        double game_ms = 0;  // time inside nativeRender (emulated game code + GL calls) since last report
        for (; running && (opt.frames <= 0 || frame < opt.frames); ++frame) {
            prev_frame_start = frame_start;
            frame_start = Clock::now();
            const double frame_dt = std::min(0.1, std::chrono::duration<double>(frame_start - prev_frame_start).count());
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
                        audio::set_global_pause(true);
                    } else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED && paused) {
                        native("Gangster2_nativeResume", {env, c_g2});
                        paused = false;
                        audio::set_global_pause(ui::is_open());
                    }
                }
                if (ev.type == SDL_CONTROLLERDEVICEADDED) {
                    const char* name = SDL_GameControllerNameForIndex(ev.cdevice.which);
                    ui::toast(std::string("Controller connected: ") + (name ? name : "unknown"));
                } else if (ev.type == SDL_CONTROLLERDEVICEREMOVED) {
                    ui::toast("Controller disconnected");
                }
                // Fast forward: F2, or a click of the right stick (on release, and not when it was part
                // of the L3+R3 chord that opens the settings menu).
                if (ev.type == SDL_KEYDOWN && !ev.key.repeat && ev.key.keysym.sym == SDLK_F2 && !ui::is_open()) {
                    toggle_ff();
                    continue;
                }
                if (ev.type == SDL_CONTROLLERBUTTONDOWN || ev.type == SDL_CONTROLLERBUTTONUP) {
                    const bool down = ev.type == SDL_CONTROLLERBUTTONDOWN;
                    if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSTICK) {
                        l3_held = down;
                        if (down && r3_held) r3_chord = true;
                    } else if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSTICK) {
                        if (down) {
                            r3_held = true;
                            r3_chord = l3_held;
                        } else {
                            if (r3_held && !r3_chord && !ui::is_open()) toggle_ff();
                            r3_held = false;
                        }
                    }
                }
                if (ui::handle_event(ev)) continue;  // the settings menu has the input
                if (ev.type == SDL_KEYDOWN || ev.type == SDL_MOUSEMOTION || ev.type == SDL_MOUSEBUTTONDOWN ||
                    ev.type == SDL_CONTROLLERBUTTONDOWN || ev.type == SDL_CONTROLLERAXISMOTION)
                    input_ticks = std::max(input_ticks, ev.common.timestamp);
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
            if (dump_hud_at >= 0 && since_start >= dump_hud_at) {
                mods::log_controls(rt, kW, kH);
                dump_hud_at = -1;
            }
            if (frame == 0) native("Gangster2_nativeResume", {env, c_g2});
            if (const uint32_t cam = mods::take_camera_to_watch()) {  // GS_WATCH_CAMERA debugging
                cpu.watch_writes(cam + 0x2c, 0x14);
                cpu.flush_translations();
            }
            switch (jni::take_app_request()) {  // Gangster2.Exit / sendAppToBackground from the last frame
                case jni::AppRequest::Quit:
                    LOG("game asked to exit (Gangster2.Exit)");
                    running = false;
                    break;
                case jni::AppRequest::Minimize:
                    SDL_MinimizeWindow(win);
                    break;
                default: break;
            }
            if (!running) break;
            if (const char* msg = jni::take_activity_bounce()) {  // the other activity "closes" at once
                LOG("Java opened another activity (not available in the port): pause + resume");
                native("Gangster2_nativePause", {env, c_g2});
                native("Gangster2_nativeResume", {env, c_g2});
                ui::toast(msg);
            }
            if (open_settings_at >= 0 && since_start >= open_settings_at) {  // (automated tests)
                ui::set_open(true);
                open_settings_at = -1;
            }
            // Native render resolution follows the window (resize, fullscreen switch): rebuild the
            // framebuffer before the game draws this frame.
            SDL_GL_GetDrawableSize(win, &dw, &dh);
            if (cfg.render_scale <= 0) {
                int iw, ih;
                internal_size(cfg, kW, kH, dw, dh, &iw, &ih);
                if (iw != gles1::vfb_config().width || ih != gles1::vfb_config().height)
                    gles1::vfb_reconfigure(iw, ih, gles1::vfb_config().msaa);
            }
            // The settings menu pauses the game: the last frame stays on screen underneath.
            const bool menu_open = ui::is_open();
            if (menu_open != menu_was_open) {
                audio::set_global_pause(menu_open || paused);
                if (menu_open) in.release_all();
                else in.suppress_held();  // the button that closed the menu must not reach the game
                menu_was_open = menu_open;
            }
            if (!menu_open) {
                // Keyboard / controller -> virtual touches on the current HUD.
                in.update(frame_dt);
                if (cfg.touch_controls == "auto")  // hide the touch HUD while a pad or the keyboard drives
                    mods::set_touch_controls_hidden(in.last_device() != input::Device::Touch);
                else
                    mods::set_touch_controls_hidden(cfg.touch_controls == "hide");
                const auto t_game = Clock::now();
                nighttex::update(cfg.night_textures, int(rt.mem().r32(daytime_obj + 4)),  // DayTime state, ms of day
                                int(rt.mem().r32(daytime_obj + 8) / 60000));
                cpu.call(rt.lib_func("Java_com_gameloft_android_GAND_GloftGMHP_ML_GameRenderer_nativeRender"), {env, c_gr});
                const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t_game).count();
                game_ms += ms;
                stat_game_ms += ms;
            }
            // Mouse-look: cursor locked only while playing (not in the settings menu or unfocused).
            in.update_mouse_lock(!ui::is_open() && (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS) != 0);

            // present the game image into the window
            SDL_GL_GetDrawableSize(win, &dw, &dh);
            int rx, ry, rw, rh;
            gles1::vfb_present(dw, dh, fit_of(cfg.fit), cfg.smooth_scaling, &rx, &ry, &rw, &rh);
            in.set_viewport(rx, ry, rw, rh);
            if (in.cursor_visible()) {  // controller menu cursor
                const int cx = rx + int(in.cursor_x() * rw / kW), cy = ry + int(in.cursor_y() * rh / kH);
                const int s = std::max(3, rh / 160);  // arm thickness
                const int l = s * 5;                  // arm length
                const gles1::OverlayRect cross[] = {
                    {cx - l - 1, cy - s / 2 - 1, 2 * l + 2, s + 2, 0, 0, 0}, {cx - s / 2 - 1, cy - l - 1, s + 2, 2 * l + 2, 0, 0, 0},
                    {cx - l, cy - s / 2, 2 * l, s, 1, 0.85f, 0.2f},          {cx - s / 2, cy - l, s, 2 * l, 1, 0.85f, 0.2f},
                };
                gles1::draw_overlay_rects(dw, dh, cross, 4);
            }
            {  // settings menu / FPS counter / toasts; apply what changed
                const settings::Settings before = cfg;
                stats.render_w = gles1::vfb_config().width;
                stats.render_h = gles1::vfb_config().height;
                if (ui::draw(cfg, stats)) {
                    if (cfg.window_mode != before.window_mode) apply_window_mode(win, cfg);
                    if (cfg.vsync != before.vsync) SDL_GL_SetSwapInterval(cfg.vsync ? 1 : 0);
                    if (cfg.render_scale != before.render_scale || cfg.msaa != before.msaa || cfg.fit != before.fit) {
                        int iw, ih;
                        internal_size(cfg, kW, kH, dw, dh, &iw, &ih);
                        if (!gles1::vfb_reconfigure(iw, ih, cfg.msaa)) ui::toast("That anti-aliasing level is not supported");
                    }
                    if (cfg.anisotropy != before.anisotropy) gles1::set_anisotropy(cfg.anisotropy);
                    if (cfg.texture_filter != before.texture_filter) gles1::set_texture_filter(filter_of(cfg.texture_filter));
                    apply_audio_volumes(cfg);
                    if (cfg.camera_mode != before.camera_mode) mods::set_camera_mode(cfg.camera_mode);
                    if (fast_forward) {  // speed changed in the menu
                        stats.fast_forward = std::clamp(cfg.fast_forward_speed, 2, 8);
                        hle::set_time_scale(stats.fast_forward);
                    }
                    in.apply_settings(cfg);
                    settings::save(settings_file, cfg);
                }
            }
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
            if (cfg.low_latency) glFinish();  // no queued frames: what was just drawn is what is shown
            if (input_ticks) {  // input-to-present latency of the newest input this frame
                lat_sum += double(SDL_GetTicks() - input_ticks);
                ++lat_n;
                input_ticks = 0;
            }
            gles1::vfb_restore_binding();

            int fps_limit = cfg.fps_limit;
            if (fps_limit < 0) {  // follow the display the window is on
                SDL_DisplayMode dm;
                fps_limit = SDL_GetCurrentDisplayMode(std::max(0, SDL_GetWindowDisplayIndex(win)), &dm) == 0 &&
                                    dm.refresh_rate > 0 ? dm.refresh_rate : 60;
            }
            if (fps_limit > 0) {
                // Fixed cadence: each deadline is the previous one + one period (so per-frame overhead
                // doesn't drift the rate below the limit); after a slow frame, start over from now.
                // Sleep most of the way, then spin: Windows sleeps are only ~1 ms accurate, too coarse
                // for 144-240 fps frame pacing.
                const auto period = std::chrono::nanoseconds(1000000000LL / fps_limit);
                next_deadline += period;
                const auto now = Clock::now();
                if (next_deadline < now - period || next_deadline > now + 2 * period) next_deadline = now;
                const auto coarse = next_deadline - std::chrono::microseconds(1500);
                if (now < coarse) std::this_thread::sleep_until(coarse);
                while (Clock::now() < next_deadline) std::this_thread::yield();
            } else {
                next_deadline = Clock::now();
            }
            ++frames_since;
            ++stat_frames;
            if (const double sdt = std::chrono::duration<double>(Clock::now() - stat_start).count(); sdt >= 0.5) {
                stats.fps = stat_frames / sdt;
                stats.game_ms = stat_frames ? stat_game_ms / stat_frames : 0.0;
                stats.input_latency_ms = lat_n ? lat_sum / lat_n : 0.0;
                lat_sum = 0;
                lat_n = 0;
                stat_frames = 0;
                stat_game_ms = 0;
                stat_start = Clock::now();
            }
            double dt = std::chrono::duration<double>(Clock::now() - last_report).count();
            if (dt >= 2.0) {
                double step_ms = 0;
                int steps = 0;
                mods::take_step_stats(&step_ms, &steps);
                LOG("frame %d: %.1f fps, game %.1f ms/frame, game step %.1f ms (x%d), %llu draw calls total, heap %u KB",
                    frame, frames_since / dt, frames_since ? game_ms / frames_since : 0.0, step_ms, steps,
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
                    static std::vector<uint64_t> prev_hooks;
                    const auto hc = rt.hook_counts();
                    std::string hooks;
                    for (size_t i = 0; i < hc.size(); ++i) {
                        const uint64_t delta = hc[i].second - (i < prev_hooks.size() ? prev_hooks[i] : 0);
                        if (delta) hooks += hc[i].first + "=" + std::to_string(uint64_t(delta / dt)) + " ";
                    }
                    prev_hooks.resize(hc.size());
                    for (size_t i = 0; i < hc.size(); ++i) prev_hooks[i] = hc[i].second;
                    if (!hooks.empty()) LOG("  hooks/s: %s", hooks.c_str());
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
    if (!opt.audio_dump.empty()) audio::write_capture(opt.audio_dump);
    media::report();
    audio::shutdown();
    jni::report();
    gles1::report();
    std::string stubs;
    for (const auto& s : rt.stubbed()) stubs += s + " ";
    LOG("imports that hit a plain STUB: %s", stubs.empty() ? "none" : stubs.c_str());

    ui::shutdown();
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return rc;
}
