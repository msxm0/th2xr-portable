#include "gl_lanczos.hpp"

// Same build rule as gl_cunny: wherever SDL draws through GLES.
#if defined(__EMSCRIPTEN__) || defined(TH2_HAVE_GLES3)
#define TH2_GL_LANCZOS 1
#include <GLES3/gl3.h>
#endif

#include <SDL3/SDL_log.h>

#include <algorithm>
#include <cmath>

namespace th2 {

#ifdef TH2_GL_LANCZOS

namespace {

constexpr const char* vertex_source = R"(#version 300 es
void main()
{
    vec2 corner = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)";

// One axis of the filter; u_axis picks which.  Integer texel reads
// throughout, so the source's own filtering state does not matter.
constexpr const char* fragment_source = R"(#version 300 es
precision highp float;
precision highp int;
precision highp sampler2D;
uniform sampler2D u_source;
uniform bool u_vertical;
uniform float u_ratio;     // source texels per target texel on this axis
uniform float u_stretch;   // kernel width, in source texels: max(1, ratio)
uniform int u_taps;        // samples: 2 * ceil(2 * stretch)
out vec4 o_color;

float lanczos2(float x)
{
    x = abs(x);
    if (x >= 2.0) {
        return 0.0;
    }
    if (x < 1e-5) {
        return 1.0;
    }
    float k = 3.14159265358979 * x;
    return sin(k) * sin(0.5 * k) / (x * x);
}

void main()
{
    ivec2 o = ivec2(gl_FragCoord.xy);
    ivec2 size = textureSize(u_source, 0);
    int along = u_vertical ? o.y : o.x;
    int last = (u_vertical ? size.y : size.x) - 1;
    float centre = (float(along) + 0.5) * u_ratio - 0.5;
    int first = int(floor(centre)) - u_taps / 2 + 1;
    vec4 sum = vec4(0.0);
    float total = 0.0;
    vec3 low = vec3(1e6);
    vec3 high = vec3(-1e6);
    for (int i = 0; i < 16; ++i) {
        if (i >= u_taps) {
            break;
        }
        int s = first + i;
        int at = clamp(s, 0, last);
        vec4 c = texelFetch(u_source, u_vertical ? ivec2(o.x, at)
                                                 : ivec2(at, o.y), 0);
        c.rgb *= c.rgb;
        float w = lanczos2((float(s) - centre) / u_stretch);
        sum += c * w;
        total += w;
        low = min(low, c.rgb);
        high = max(high, c.rgb);
    }
    vec4 v = sum / total;
    o_color = vec4(sqrt(clamp(v.rgb, low, high)), clamp(v.a, 0.0, 1.0));
}
)";

GLuint compile(GLenum type, const char* source)
{
    const GLuint shader = glCreateShader(type);
    if (!shader) {
        return 0;
    }
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[512]{};
        glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
        SDL_Log("lanczos shader failed to compile: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

}  // namespace

struct GlLanczos::Impl {
    GLuint program = 0;
    GLuint vertex_array = 0;
    GLint vertical = -1;
    GLint ratio = -1;
    GLint stretch = -1;
    GLint taps = -1;
    // The horizontal pass's result: target width, source height.
    GLuint middle = 0;
    GLuint middle_fbo = 0;
    GLuint target_fbo = 0;
    GLuint attached_target = 0;
    int middle_w = 0;
    int middle_h = 0;
    bool ok = false;

    Impl()
    {
        const GLuint vertex = compile(GL_VERTEX_SHADER, vertex_source);
        const GLuint fragment = compile(GL_FRAGMENT_SHADER, fragment_source);
        if (!vertex || !fragment) {
            glDeleteShader(vertex);
            glDeleteShader(fragment);
            return;
        }
        program = glCreateProgram();
        glAttachShader(program, vertex);
        glAttachShader(program, fragment);
        glLinkProgram(program);
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        GLint linked = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (!linked) {
            SDL_Log("lanczos shader failed to link");
            glDeleteProgram(program);
            program = 0;
            return;
        }
        glUseProgram(program);
        glUniform1i(glGetUniformLocation(program, "u_source"), 0);
        glUseProgram(0);
        vertical = glGetUniformLocation(program, "u_vertical");
        ratio = glGetUniformLocation(program, "u_ratio");
        stretch = glGetUniformLocation(program, "u_stretch");
        taps = glGetUniformLocation(program, "u_taps");
        glGenVertexArrays(1, &vertex_array);
        glGenFramebuffers(1, &middle_fbo);
        glGenFramebuffers(1, &target_fbo);
        ok = true;
    }

    ~Impl()
    {
        if (middle) glDeleteTextures(1, &middle);
        if (middle_fbo) glDeleteFramebuffers(1, &middle_fbo);
        if (target_fbo) glDeleteFramebuffers(1, &target_fbo);
        if (vertex_array) glDeleteVertexArrays(1, &vertex_array);
        if (program) glDeleteProgram(program);
    }

    bool ensure_middle(int w, int h)
    {
        if (middle && w == middle_w && h == middle_h) {
            return true;
        }
        if (middle) {
            glDeleteTextures(1, &middle);
        }
        glGenTextures(1, &middle);
        glBindTexture(GL_TEXTURE_2D, middle);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, middle_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, middle, 0);
        middle_w = w;
        middle_h = h;
        return glCheckFramebufferStatus(GL_FRAMEBUFFER)
            == GL_FRAMEBUFFER_COMPLETE;
    }

    void axis(bool is_vertical, int from, int to)
    {
        const float r = static_cast<float>(from) / static_cast<float>(to);
        const float s = std::max(1.0f, r);
        glUniform1i(vertical, is_vertical ? 1 : 0);
        glUniform1f(ratio, r);
        glUniform1f(stretch, s);
        glUniform1i(taps, std::min(16, 2 * static_cast<int>(std::ceil(2.0f * s))));
    }
};

GlLanczos::GlLanczos() : impl_(std::make_unique<Impl>()) {}

GlLanczos::~GlLanczos() = default;

bool GlLanczos::ready() const { return impl_->ok; }

bool GlLanczos::run(unsigned source, int sw, int sh, unsigned target, int tw,
                    int th)
{
    if (!impl_->ok || sw <= 0 || sh <= 0 || tw <= 0 || th <= 0
        || !impl_->ensure_middle(tw, sh)) {
        return false;
    }
    if (impl_->attached_target != target) {
        glBindFramebuffer(GL_FRAMEBUFFER, impl_->target_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, target, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER)
            != GL_FRAMEBUFFER_COMPLETE) {
            return false;
        }
        impl_->attached_target = target;
    }
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(impl_->program);
    glBindVertexArray(impl_->vertex_array);
    glActiveTexture(GL_TEXTURE0);

    glBindFramebuffer(GL_FRAMEBUFFER, impl_->middle_fbo);
    glViewport(0, 0, tw, sh);
    glBindTexture(GL_TEXTURE_2D, source);
    impl_->axis(false, sw, tw);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindFramebuffer(GL_FRAMEBUFFER, impl_->target_fbo);
    glViewport(0, 0, tw, th);
    glBindTexture(GL_TEXTURE_2D, impl_->middle);
    impl_->axis(true, sh, th);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindVertexArray(0);
    glUseProgram(0);
    return true;
}

void GlLanczos::forget_target() { impl_->attached_target = 0; }

#else   // no GLES headers: never ready

struct GlLanczos::Impl {};
GlLanczos::GlLanczos() = default;
GlLanczos::~GlLanczos() = default;
bool GlLanczos::ready() const { return false; }
bool GlLanczos::run(unsigned, int, int, unsigned, int, int) { return false; }
void GlLanczos::forget_target() {}

#endif

}  // namespace th2
