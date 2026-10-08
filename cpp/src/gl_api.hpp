#pragma once

// Desktop presentation uses OpenGL 2.1 with fixed-function quads. Android
// (EB_GLES) uses OpenGL ES 3.0, which has no immediate mode, matrix stack,
// alpha test or EXT framebuffer entry points. Renderers draw through these
// helpers so both paths produce the same pixels from the same call sites.

#ifdef EB_GLES
#include <GLES3/gl3.h>
// Core ES 3.0 provides the framebuffer objects that desktop GL 2.1 loads
// from EXT_framebuffer_object. Map the names so call sites stay shared.
#define glGenFramebuffersEXT glGenFramebuffers
#define glDeleteFramebuffersEXT glDeleteFramebuffers
#define glBindFramebufferEXT glBindFramebuffer
#define glCheckFramebufferStatusEXT glCheckFramebufferStatus
#define glFramebufferTexture2DEXT glFramebufferTexture2D
#define glGenRenderbuffersEXT glGenRenderbuffers
#define glDeleteRenderbuffersEXT glDeleteRenderbuffers
#define glBindRenderbufferEXT glBindRenderbuffer
#define glRenderbufferStorageEXT glRenderbufferStorage
#define glFramebufferRenderbufferEXT glFramebufferRenderbuffer
#define GL_FRAMEBUFFER_EXT GL_FRAMEBUFFER
#define GL_RENDERBUFFER_EXT GL_RENDERBUFFER
#define GL_DEPTH24_STENCIL8_EXT GL_DEPTH24_STENCIL8
#define GL_DEPTH_ATTACHMENT_EXT GL_DEPTH_ATTACHMENT
#define GL_STENCIL_ATTACHMENT_EXT GL_STENCIL_ATTACHMENT
#define GL_COLOR_ATTACHMENT0_EXT GL_COLOR_ATTACHMENT0
#define GL_FRAMEBUFFER_COMPLETE_EXT GL_FRAMEBUFFER_COMPLETE
// ES functions are linked directly instead of loaded through SDL.
#define EB_GL_PROC(type, name) decltype(&gl##name) name = &gl##name
#else
#include <SDL_opengl.h>
#define EB_GL_PROC(type, name) type name = proc<type>("gl" #name)
#endif

#include <array>
#include <string>

namespace eb::gl {
// One triangle-strip corner: clip-space position and texture coordinate.
struct Vertex {
    float x, y, z, u, v;
};

// Draw four corners in strip order with the currently bound program/texture.
// Desktop uses ftransform()/gl_MultiTexCoord0; ES binds attributes 0 and 1.
void draw_strip(const std::array<Vertex, 4> &corners);

// Fixed-function replacement for the shaderless picture paths. Desktop keeps
// GL_TEXTURE_2D/GL_ALPHA_TEST state; ES binds an equivalent texture program.
void begin_textured(bool alpha_test);
void end_textured();

// Desktop-only fixed-function toggles that are invalid enums under ES.
void fixed_function_texture(bool enabled);
void disable_alpha_test();
void clear_depth(double depth);
// Select the default framebuffer's back buffer for drawing and reading.
void use_back_buffer();
// Draw an offscreen framebuffer's first color attachment.
void use_color_attachment();

// Vertex shader matching draw_strip, and a fragment source adapted from the
// GLSL 1.20 source used by desktop builds.
const char *vertex_source();
std::string fragment_source(const std::string &desktop_source);
} // namespace eb::gl
