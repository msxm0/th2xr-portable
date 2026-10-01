#include "gl_anime4k.hpp"

// Same build rule as gl_transition and gl_blend: wherever SDL draws through
// GLES and the headers exist.
#if defined(__EMSCRIPTEN__) || defined(TH2_HAVE_GLES3)
#define TH2_GL_ANIME4K 1
#include <GLES3/gl3.h>
#endif

#include <SDL3/SDL_log.h>

#include <string_view>

namespace th2 {

#ifdef TH2_GL_ANIME4K

namespace {

constexpr char vertex_source[] = R"(#version 300 es
precision highp float;
in vec2 a_position;      // clip space
in vec2 a_uv;            // art texture, normalised
out vec2 v_uv;
void main()
{
    v_uv = a_uv;
    gl_Position = vec4(a_position, 0.0, 1.0);
}
)";

// Fused Anime4K_Upscale_DoG_x2. Copyright (c) 2019-2021 bloc97, MIT.
// Algebraically the same as the release's luma, Gaussian X/Y and apply
// passes, folded into one so it needs a single sampler.
//
// Ported from the SPIRV version in shaders/anime4k/apply.frag: SDL's own
// vertex colour is gone (this draws its own quad, so there is nothing to
// modulate by) and textureSize returns an ivec2 that has to be converted
// before it can divide a vec2.
constexpr char fragment_source[] = R"(#version 300 es
precision highp float;
precision highp sampler2D;
uniform sampler2D u_source;
in vec2 v_uv;
out vec4 fragment;

float luma(vec2 position)
{
    return dot(texture(u_source, position).rgb, vec3(0.299, 0.587, 0.114));
}

void main()
{
    const float strength = 0.8;
    const float w0 = 0.38774;
    const float w1 = 0.24477;
    const float w2 = 0.06136;
    vec2 pixel = 1.0 / vec2(textureSize(u_source, 0));
    float center = luma(v_uv);
    float minimum = 1.0;
    float maximum = 0.0;
    float gaussian = 0.0;
    for (int y = -2; y <= 2; ++y) {
        float wy = y == 0 ? w0 : (abs(y) == 1 ? w1 : w2);
        for (int x = -2; x <= 2; ++x) {
            float wx = x == 0 ? w0 : (abs(x) == 1 ? w1 : w2);
            float sample_luma = luma(v_uv + vec2(float(x), float(y)) * pixel);
            gaussian += sample_luma * wx * wy;
            if (abs(x) <= 1 && abs(y) <= 1) {
                minimum = min(minimum, sample_luma);
                maximum = max(maximum, sample_luma);
            }
        }
    }
    float correction = (center - gaussian) * strength;
    correction = clamp(correction + center, minimum, maximum) - center;
    fragment = vec4(texture(u_source, v_uv).rgb + vec3(correction), 1.0);
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
        SDL_Log("anime4k shader failed to compile: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

// SDL exposes the GL name under a different property per backend.
GLuint texture_name(SDL_Texture* texture)
{
    if (!texture) {
        return 0;
    }
    const SDL_PropertiesID properties = SDL_GetTextureProperties(texture);
    if (!properties) {
        return 0;
    }
    auto name = static_cast<GLuint>(SDL_GetNumberProperty(
        properties, SDL_PROP_TEXTURE_OPENGLES2_TEXTURE_NUMBER, 0));
    if (!name) {
        name = static_cast<GLuint>(SDL_GetNumberProperty(
            properties, SDL_PROP_TEXTURE_OPENGL_TEXTURE_NUMBER, 0));
    }
    return name;
}

bool renderer_uses_gl(SDL_Renderer* renderer)
{
    const char* name = SDL_GetRendererName(renderer);
    if (!name) {
        return false;
    }
    const std::string_view driver(name);
    return driver == "opengl" || driver == "opengles2"
        || driver == "opengles";
}

}  // namespace

struct GlAnime4K::Impl {
    GLuint program = 0;
    GLuint vertex_buffer = 0;
    GLuint vertex_array = 0;
    GLint source_location = -1;
    bool ready = false;

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
        glBindAttribLocation(program, 0, "a_position");
        glBindAttribLocation(program, 1, "a_uv");
        glLinkProgram(program);
        glDeleteShader(vertex);
        glDeleteShader(fragment);

        GLint linked = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (!linked) {
            char log[512]{};
            glGetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
            SDL_Log("anime4k shader failed to link: %s", log);
            glDeleteProgram(program);
            program = 0;
            return;
        }
        source_location = glGetUniformLocation(program, "u_source");

        glGenVertexArrays(1, &vertex_array);
        glGenBuffers(1, &vertex_buffer);
        glBindVertexArray(vertex_array);
        glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
        glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * 16, nullptr,
                     GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE,
                              sizeof(GLfloat) * 4, nullptr);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(GLfloat) * 4,
                              reinterpret_cast<const void*>(
                                  sizeof(GLfloat) * 2));
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        ready = glGetError() == GL_NO_ERROR;
    }

    ~Impl()
    {
        if (vertex_array) glDeleteVertexArrays(1, &vertex_array);
        if (vertex_buffer) glDeleteBuffers(1, &vertex_buffer);
        if (program) glDeleteProgram(program);
    }
};

GlAnime4K::GlAnime4K(SDL_Renderer* renderer)
    : impl_(renderer_uses_gl(renderer) ? std::make_unique<Impl>() : nullptr)
{
}

GlAnime4K::~GlAnime4K() = default;

bool GlAnime4K::available() const { return impl_ && impl_->ready; }

bool GlAnime4K::draw(
    SDL_Renderer* renderer, SDL_Texture* art, const SDL_FRect& destination)
{
    if (!available()) {
        return false;
    }
    // Everything SDL has queued - the clear this draws on top of above all -
    // has to reach the driver before raw GL touches the same framebuffer.
    if (!SDL_FlushRenderer(renderer)) {
        return false;
    }
    const GLuint name = texture_name(art);
    if (!name) {
        return false;
    }
    int output_width = 0;
    int output_height = 0;
    if (!SDL_GetRenderOutputSize(renderer, &output_width, &output_height)
        || output_width <= 0 || output_height <= 0) {
        return false;
    }
    const auto w = static_cast<float>(output_width);
    const auto h = static_cast<float>(output_height);

    // The window, not a render target: here GL's y really does run opposite
    // to SDL's, so the vertical axis is inverted.  (A render target does not
    // need this - SDL's projection already lines those up, which is what
    // gl_blend.cpp and gl_transition.cpp record.)
    const float x0 = destination.x / w * 2.0f - 1.0f;
    const float x1 = (destination.x + destination.w) / w * 2.0f - 1.0f;
    const float y0 = 1.0f - destination.y / h * 2.0f;
    const float y1 = 1.0f - (destination.y + destination.h) / h * 2.0f;

    const GLfloat quad[16] = {
        x0, y0, 0.0f, 0.0f,
        x1, y0, 1.0f, 0.0f,
        x0, y1, 0.0f, 1.0f,
        x1, y1, 1.0f, 1.0f,
    };

    glUseProgram(impl_->program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, name);
    // Linear: this pass magnifies, and the shader's own samples are meant to
    // be filtered.  Sampling state travels with the texture and this one
    // belongs to SDL, so it is set every time rather than assumed.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glUniform1i(impl_->source_location, 0);

    // Nothing is saved to put back.  The SDL_FlushRenderer above marks SDL's
    // cached GL state invalid, so it sets blend, scissor, viewport, program
    // and textures again before its next draw - and a query per frame here,
    // or a glGetError, is a round trip to the GPU process in WebGL.
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, output_width, output_height);
    glBindVertexArray(impl_->vertex_array);
    glBindBuffer(GL_ARRAY_BUFFER, impl_->vertex_buffer);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(quad), quad);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE0);
    glUseProgram(0);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    return true;
}

#else   // no GLES headers: a stub that is never available

struct GlAnime4K::Impl {};
GlAnime4K::GlAnime4K(SDL_Renderer*) : impl_(nullptr) {}
GlAnime4K::~GlAnime4K() = default;
bool GlAnime4K::available() const { return false; }
bool GlAnime4K::draw(SDL_Renderer*, SDL_Texture*, const SDL_FRect&)
{
    return false;
}

#endif

}  // namespace th2
