// Gangstar: Miami Vindication native runtime (Windows x86-64).
//
//   gangstar --selftest-load [path/to/libGangster2.so]   load + relocate + verify (= probe 1a)
//   gangstar --window [--frames N]                       SDL2 window + GL context check
#include <SDL.h>
#include "gl/glapi.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#ifdef __ANDROID__
#include <pthread.h>
#endif
#include <string>
#include <vector>

#include "core/log.h"

#include "game.h"

int run_selftest_load(const std::string& so_path);
int run_selftest_fastpath(const rt::Paths& paths);

namespace fs = std::filesystem;

#ifndef __ANDROID__
static fs::path find_project_root() {
    // Walk up from the working directory and from the executable until we find
    // the extracted APK (work/apk/...), which marks the project root.
    std::vector<fs::path> starts = {fs::current_path()};
    if (char* base = SDL_GetBasePath()) {
        starts.emplace_back(base);
        SDL_free(base);
    }
    for (fs::path p : starts) {
        for (int i = 0; i < 8 && !p.empty(); ++i, p = p.parent_path()) {
            if (fs::exists(p / "work" / "apk" / "lib" / "armeabi" / "libGangster2.so")) return p;
            if (p == p.parent_path()) break;
        }
    }
    return {};
}
#endif

static int run_window(int frames) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) FATAL("SDL_Init: %s", SDL_GetError());
    // Compatibility profile: the game is GLES 1.1 fixed-function, which desktop GL
    // still provides outside the core profile.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_Window* win = SDL_CreateWindow("Gangstar: Miami Vindication (native)", SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED, 800, 480, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
    if (!win) FATAL("SDL_CreateWindow: %s", SDL_GetError());
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) FATAL("SDL_GL_CreateContext: %s", SDL_GetError());
    SDL_GL_SetSwapInterval(1);

    const char* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    LOG("GL_VENDOR   %s", glGetString(GL_VENDOR));
    LOG("GL_RENDERER %s", glGetString(GL_RENDERER));
    LOG("GL_VERSION  %s", glGetString(GL_VERSION));
    for (const char* e : {"GL_EXT_texture_compression_s3tc", "GL_ARB_framebuffer_object", "GL_ARB_vertex_buffer_object",
                          "GL_ARB_texture_env_combine", "GL_EXT_blend_equation_separate"})
        LOG("  %-34s %s", e, ext && std::strstr(ext, e) ? "yes" : "NO");

    bool running = true;
    for (int i = 0; running && (frames <= 0 || i < frames); ++i) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
            if (ev.type == SDL_QUIT) running = false;
        float t = (i % 120) / 120.0f;
        glClearColor(0.1f, 0.1f + 0.3f * t, 0.3f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        SDL_GL_SwapWindow(win);
    }
    LOG("window test done");
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

#ifdef __ANDROID__
// SDL runs main() on a Java thread with a small stack; recompiled code nests every guest call as a
// host C call (the Windows build links a 64 MB main-thread stack for the same reason). So the real
// main runs on a native thread with a 64 MB stack.
static int real_main(int argc, char** argv);
struct MainArgs { int argc; char** argv; int rc; };
static void* main_thread(void* p) {
    auto* a = static_cast<MainArgs*>(p);
    a->rc = real_main(a->argc, a->argv);
    return nullptr;
}
int main(int argc, char** argv) {
    MainArgs a{argc, argv, 1};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64u << 20);
    pthread_t t;
    if (pthread_create(&t, &attr, &main_thread, &a) != 0) return real_main(argc, argv);
    pthread_join(t, nullptr);
    return a.rc;
}
static int real_main(int argc, char** argv) {
#else
int main(int argc, char** argv) {
#endif
#ifdef __ANDROID__
    // Android: everything lives in the app's folder on shared storage
    // (/storage/emulated/0/Android/data/<package>/files), which the owner fills from a PC:
    //   libGangster2.so, sound_table.txt, gameloft/games/Gangstar2/ (the game data)
    // Saves (sandbox/), settings (user/) and runtime.log are written there too.
    const char* ext = SDL_AndroidGetExternalStoragePath();
    fs::path root = ext ? fs::path(ext) : fs::path();
    const fs::path home = root;
    const fs::path log_dir = root;
#else
    fs::path root = find_project_root();
    // GANGSTAR_HOME=<dir>: a separate home for test runs. Saves (sandbox), settings.ini and
    // runtime.log go under <dir>, so a test never touches the owner's save, settings or log.
    const char* home_env = std::getenv("GANGSTAR_HOME");
    const fs::path home = home_env && *home_env ? fs::absolute(home_env) : fs::path();
    const fs::path log_dir = !home.empty() ? home : (root.empty() ? fs::path("build") : root / "build");
#endif
    fs::create_directories(log_dir);
    core::log_open((log_dir / "runtime.log").string().c_str());

    std::string mode = argc > 1 ? argv[1] : "--run";
    int frames = 0;
    RunOptions opt;
#ifdef GANGSTAR_HAVE_RECOMP
    opt.recomp = true;  // the recompiled code is in this build: use it unless --cpu unicorn
#endif
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--profile-traps")) opt.profile_traps = true;
        else if (!std::strcmp(argv[i], "--no-vsync")) opt.no_vsync = true;
        else if (!std::strcmp(argv[i], "--no-mods")) opt.no_mods = true;
        else if (!std::strcmp(argv[i], "--no-fastpath")) opt.no_fastpath = true;
        else if (!std::strcmp(argv[i], "--ff")) opt.fast_forward = true;
    for (int i = 1; i + 1 < argc; ++i) {
        if (!std::strcmp(argv[i], "--frames")) frames = opt.frames = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--shot-every")) opt.shot_every = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--demo")) opt.demo = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--fps")) opt.fps = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--seconds")) opt.seconds = std::atof(argv[i + 1]);
        if (!std::strcmp(argv[i], "--dump-hud")) opt.dump_hud_at = std::atof(argv[i + 1]);
        if (!std::strcmp(argv[i], "--open-settings")) opt.open_settings_at = std::atof(argv[i + 1]);
        if (!std::strcmp(argv[i], "--set")) opt.set.push_back(argv[i + 1]);
        if (!std::strcmp(argv[i], "--cpu")) opt.recomp = !std::strcmp(argv[i + 1], "recomp");
        if (!std::strcmp(argv[i], "--audio-dump")) opt.audio_dump = argv[i + 1];
        if (!std::strcmp(argv[i], "--touch-controls")) opt.touch_controls = argv[i + 1];
        if (!std::strcmp(argv[i], "--quality")) opt.quality = argv[i + 1];
        if (!std::strcmp(argv[i], "--profile-guest")) opt.profile_guest_at = std::atof(argv[i + 1]);
        if (!std::strcmp(argv[i], "--input")) opt.input = argv[i + 1];
        if (!std::strcmp(argv[i], "--shots-at")) opt.shots_at = argv[i + 1];
        if (!std::strcmp(argv[i], "--game-size")) std::sscanf(argv[i + 1], "%dx%d", &opt.game_w, &opt.game_h);
    }

    int rc = 0;
#ifdef __ANDROID__
    rt::Paths p{root, root / "libGangster2.so", root / "gameloft" / "games" / "Gangstar2", root / "sandbox",
                root / "user", root / "sound_table.txt"};
    LOG("Android: files in %s", root.string().c_str());
    if (root.empty() || !fs::exists(p.so) || !fs::exists(p.data)) {
        const std::string msg = "Copy the game files to " + (root.empty() ? std::string("the app folder") : root.string()) +
                                ":\n  libGangster2.so\n  sound_table.txt (optional)\n  gameloft/games/Gangstar2/ (the game data)";
        LOG("%s", msg.c_str());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Gangstar: game files missing", msg.c_str(), nullptr);
        core::log_close();
        return 1;
    }
#else
    rt::Paths p{root, root / "work" / "apk" / "lib" / "armeabi" / "libGangster2.so",
                root / "original" / "gameloft" / "games" / "Gangstar2",
                !home.empty() ? home / "sandbox" : root / "work" / "sandbox",
                !home.empty() ? home / "user" : root / "work" / "user", root / "work" / "sound_table.txt"};
    if (!home.empty()) LOG("GANGSTAR_HOME: %s (saves, settings and log kept here)", home.string().c_str());
#endif
    if ((mode == "--run" || mode == "--selftest-fastpath") && root.empty())
        FATAL("project root not found (expected work/apk/lib/armeabi/libGangster2.so)");
    if (mode == "--run") {
        rc = run_game(p, opt);
    } else if (mode == "--selftest-fastpath") {
        rc = run_selftest_fastpath(p);
    } else if (mode == "--selftest-load") {
        std::string so = argc > 2 && argv[2][0] != '-' ? argv[2]
                                                       : (root / "work" / "apk" / "lib" / "armeabi" / "libGangster2.so").string();
        rc = run_selftest_load(so);
    } else if (mode == "--window") {
        rc = run_window(frames);
    } else {
        LOG("unknown mode %s", mode.c_str());
        rc = 2;
    }
    core::log_close();
    return rc;
}
