// Gangstar: Miami Vindication native runtime (Windows x86-64).
//
//   gangstar --selftest-load [path/to/libGangster2.so]   load + relocate + verify (= probe 1a)
//   gangstar --window [--frames N]                       SDL2 window + GL context check
#include <SDL.h>
#include <SDL_opengl.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "core/log.h"

int run_selftest_load(const std::string& so_path);

namespace fs = std::filesystem;

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

int main(int argc, char** argv) {
    fs::path root = find_project_root();
    fs::create_directories(root.empty() ? fs::path("build") : root / "build");
    core::log_open(((root.empty() ? fs::path(".") : root) / "build" / "runtime.log").string().c_str());

    std::string mode = argc > 1 ? argv[1] : "--window";
    int frames = 0;
    for (int i = 1; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--frames")) frames = std::atoi(argv[i + 1]);

    int rc = 0;
    if (mode == "--selftest-load") {
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
