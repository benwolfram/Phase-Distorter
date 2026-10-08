#include "gl_api.hpp"
#include <SDL.h>
#include <stdexcept>
#include <vector>

namespace eb::gl {
#ifdef EB_GLES
namespace {
constexpr char es_vertex[] = R"GLSL(#version 300 es
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
out vec2 UV;
void main() { gl_Position = Position; UV = TexCoord; }
)GLSL";
// Matches GL_MODULATE with a white color plus glAlphaFunc(GL_GREATER, 0).
constexpr char es_textured[] = R"GLSL(#version 300 es
precision highp float;
uniform sampler2D Image;
uniform bool AlphaTest;
in vec2 UV;
out vec4 FragColor;
void main() {
    vec4 pixel = texture(Image, UV);
    if (AlphaTest && pixel.a <= 0.0) discard;
    FragColor = pixel;
}
)GLSL";

// Objects belong to one context. Recreate them if SDL hands us a new one.
struct Resources {
    SDL_GLContext context{};
    GLuint array{}, buffer{}, program{};
    GLint alpha_test{};
} resources;

GLuint compile(GLenum type, const char *source) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok{};
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint length{};
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
        std::vector<char> log(length + 1);
        glGetShaderInfoLog(shader, length, nullptr, log.data());
        glDeleteShader(shader);
        throw std::runtime_error(std::string("GLES shader compilation failed: ") + log.data());
    }
    return shader;
}

Resources &current() {
    auto &r = resources;
    const auto context = SDL_GL_GetCurrentContext();
    if (r.context == context && r.program)
        return r;
    r = {};
    r.context = context;
    glGenVertexArrays(1, &r.array);
    glGenBuffers(1, &r.buffer);
    glBindVertexArray(r.array);
    glBindBuffer(GL_ARRAY_BUFFER, r.buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(Vertex) * 4, nullptr, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<const void *>(3 * sizeof(float)));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    const auto vert = compile(GL_VERTEX_SHADER, es_vertex);
    GLuint frag{};
    try { frag = compile(GL_FRAGMENT_SHADER, es_textured); }
    catch (...) { glDeleteShader(vert); throw; }
    r.program = glCreateProgram();
    glAttachShader(r.program, vert); glAttachShader(r.program, frag);
    glLinkProgram(r.program); glDeleteShader(vert); glDeleteShader(frag);
    GLint ok{};
    glGetProgramiv(r.program, GL_LINK_STATUS, &ok);
    if (!ok) {
        glDeleteProgram(r.program);
        r.program = 0;
        throw std::runtime_error("GLES picture program link failed");
    }
    // Created lazily, possibly mid-draw: restore the caller's program.
    GLint previous{};
    glGetIntegerv(GL_CURRENT_PROGRAM, &previous);
    glUseProgram(r.program);
    glUniform1i(glGetUniformLocation(r.program, "Image"), 0);
    glUseProgram(GLuint(previous));
    r.alpha_test = glGetUniformLocation(r.program, "AlphaTest");
    return r;
}

void replace_all(std::string &text, const std::string &from, const std::string &to) {
    for (auto at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
        text.replace(at, from.size(), to);
}
} // namespace

void draw_strip(const std::array<Vertex, 4> &corners) {
    auto &r = current();
    glBindVertexArray(r.array);
    glBindBuffer(GL_ARRAY_BUFFER, r.buffer);
    // Orphan the previous quad so the driver need not wait for it to draw.
    glBufferData(GL_ARRAY_BUFFER, sizeof corners, corners.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}
void begin_textured(bool alpha_test) {
    auto &r = current();
    glUseProgram(r.program);
    glUniform1i(r.alpha_test, alpha_test);
}
void end_textured() { glUseProgram(0); }
void fixed_function_texture(bool) {}
void disable_alpha_test() {}
void clear_depth(double depth) { glClearDepthf(float(depth)); }
void use_back_buffer() {
    const GLenum back = GL_BACK;
    glDrawBuffers(1, &back);
    glReadBuffer(GL_BACK);
}
void use_color_attachment() {
    const GLenum color = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &color);
}
const char *vertex_source() { return es_vertex; }
std::string fragment_source(const std::string &desktop_source) {
    // The desktop shaders are written against GLSL 1.20 with no ES-only
    // syntax, so a header swap plus renamed built-ins is a complete port.
    auto source = desktop_source;
    const auto line_end = source.find('\n');
    if (source.rfind("#version", 0) != 0 || line_end == std::string::npos)
        throw std::runtime_error("Shader source must begin with a #version line");
    source.replace(0, line_end + 1,
                   "#version 300 es\nprecision highp float;\nprecision highp int;\n"
                   "out vec4 eb_FragColor;\n");
    replace_all(source, "varying ", "in ");
    replace_all(source, "texture2D(", "texture(");
    replace_all(source, "gl_FragColor", "eb_FragColor");
    return source;
}
#else
void draw_strip(const std::array<Vertex, 4> &corners) {
    glBegin(GL_TRIANGLE_STRIP);
    for (const auto &corner : corners) {
        glTexCoord2f(corner.u, corner.v);
        glVertex3f(corner.x, corner.y, corner.z);
    }
    glEnd();
}
void begin_textured(bool alpha_test) {
    if (!alpha_test) return;
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0);
}
void end_textured() { glDisable(GL_ALPHA_TEST); }
void fixed_function_texture(bool enabled) {
    if (enabled) glEnable(GL_TEXTURE_2D);
    else glDisable(GL_TEXTURE_2D);
}
void disable_alpha_test() { glDisable(GL_ALPHA_TEST); }
void clear_depth(double depth) { glClearDepth(depth); }
void use_back_buffer() {
    glDrawBuffer(GL_BACK);
    glReadBuffer(GL_BACK);
}
void use_color_attachment() { glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT); }
const char *vertex_source() {
    return "#version 120\nvarying vec2 UV; void main(){gl_Position=ftransform(); UV=gl_MultiTexCoord0.xy;}";
}
std::string fragment_source(const std::string &desktop_source) { return desktop_source; }
#endif
} // namespace eb::gl
