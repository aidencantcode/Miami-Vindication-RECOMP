#pragma once
// GL declarations for the platform.
//  - Desktop (Windows): SDL's OpenGL headers (compatibility profile: fixed function + GL 3.x/4.x).
//  - GANGSTAR_GLES (Android): GLES 3.0 plus the GLES 1.1 header for the fixed-function enums and
//    prototypes the bridge names. The GLES build always uses the shader renderer (gl/ffp), so those
//    fixed-function calls are never made there; libGLESv1_CM only satisfies the linker.
//
// GLAPI_LOAD(type, name) fills `p_name`: through SDL_GL_GetProcAddress on desktop (entry points
// beyond GL 1.1 are not exported there), straight from the GLES libraries on Android.
#ifdef GANGSTAR_GLES
#include <GLES/gl.h>
#include <GLES/glext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

// Desktop names the code uses, mapped to GLES.
#ifndef APIENTRY
#define APIENTRY GL_APIENTRY
#endif
#define GL_SOURCE0_RGB GL_SRC0_RGB
#define GL_SOURCE1_RGB GL_SRC1_RGB
#define GL_SOURCE2_RGB GL_SRC2_RGB
#define GL_SOURCE0_ALPHA GL_SRC0_ALPHA
#define GL_SOURCE1_ALPHA GL_SRC1_ALPHA
#define GL_SOURCE2_ALPHA GL_SRC2_ALPHA
typedef void(GL_APIENTRYP PFNGLPOINTPARAMETERFPROC)(GLenum pname, GLfloat param);
typedef void(GL_APIENTRYP PFNGLPOINTPARAMETERFVPROC)(GLenum pname, const GLfloat* params);
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
inline void glDepthRange(double n, double f) { glDepthRangef(float(n), float(f)); }
inline void glClearDepth(double d) { glClearDepthf(float(d)); }

#define GLAPI_LOAD(type, name, ok)                                     \
    p_##name = reinterpret_cast<type>(&::name);                        \
    (void)ok;
#else
#include <SDL.h>
#include <SDL_opengl.h>
#include <SDL_opengl_glext.h>

#define GLAPI_LOAD(type, name, ok)                                                  \
    p_##name = reinterpret_cast<type>(SDL_GL_GetProcAddress(#name));               \
    if (!p_##name) { LOG("GL: missing entry point %s", #name); ok = false; }
#endif
