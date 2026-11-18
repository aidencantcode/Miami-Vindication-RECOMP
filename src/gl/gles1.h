#pragma once
// OpenGL ES 1.1 (as used by the game) -> desktop OpenGL compatibility profile.
// The probe's 2a trace defines what is needed: 64 of 95 imported GL functions during
// boot + first gameplay load, FLOAT/UBYTE vertex arrays (no GL_FIXED), ushort indices,
// DXT1/DXT5 compressed textures, point sprites.
#include <cstdint>

namespace rt { class Runtime; }

namespace gles1 {

bool load_gl();                     // after a GL context is current
void register_handlers(rt::Runtime& rt);
uint64_t draw_calls();              // statistics
void report();
// Read back the default framebuffer's back buffer and save it as a BMP.
bool screenshot(const char* path, int w, int h);

}  // namespace gles1
