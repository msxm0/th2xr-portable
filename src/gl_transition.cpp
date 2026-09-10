#include "gl_transition.hpp"

// Built where SDL draws through GLES and the headers are known to exist.
// Nothing here depends on the canvas size: the quad is in clip space and the
// textures are sampled with normalised coordinates, so it behaves the same
// on a phone screen as on a desktop one.
#ifdef __EMSCRIPTEN__
#define TH2_GL_TRANSITION 1
#include <GLES3/gl3.h>
#endif

#include <SDL3/SDL_log.h>

#include <string>
#include <string_view>

namespace th2 {

#ifdef TH2_GL_TRANSITION

namespace {

// The two frames and the destination are all render targets, whose GL row 0
// is the bottom, so they share one mapping.  The mask is an ordinary uploaded
// texture with row 0 at the top of the image, so it needs the opposite one.
// Sampling all three the same way renders the scene upside down.
constexpr char vertex_source[] = R"(#version 300 es
precision highp float;
in vec2 a_position;
out vec2 v_uv;
out vec2 v_mask_uv;
void main() {
    vec2 clip = a_position * 0.5 + 0.5;
    v_uv = clip;
    v_mask_uv = vec2(clip.x, 1.0 - clip.y);
    gl_Position = vec4(a_position, 0.0, 1.0);
}
)";

// highp, not mediump: a desktop GPU quietly promotes mediump to 32 bits, but
// on a phone it really is 16, whose guaranteed range is only +-16384.  The
// blend's intermediate reaches about 98000 with a wide mask, overflows, and
// the whole wipe comes out black.  The arithmetic is also folded so it stays
// in 0..1 rather than scaling up to 0..65280 and dividing back down.
constexpr char fragment_source[] = R"(#version 300 es
precision highp float;
precision highp sampler2D;
in vec2 v_uv;
in vec2 v_mask_uv;
out vec4 fragment;
uniform sampler2D u_previous;
uniform sampler2D u_next;
uniform sampler2D u_mask;
uniform float u_offset;
uniform float u_vague;
void main() {
    float mask = texture(u_mask, v_mask_uv).r * 255.0;
    float alpha = clamp(
        (mask + u_offset - 256.0) * (256.0 / 255.0) / u_vague, 0.0, 1.0);
    vec3 blended = mix(
        texture(u_previous, v_uv).rgb, texture(u_next, v_uv).rgb, alpha);
    fragment = vec4(blended, 1.0);
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
        SDL_Log("transition shader failed to compile: %s", log);
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

}  // namespace

struct GlPatternTransition::Impl {
    GLuint program = 0;
    GLuint vertex_buffer = 0;
    GLuint vertex_array = 0;
    GLint previous_location = -1;
    GLint next_location = -1;
    GLint mask_location = -1;
    GLint offset_location = -1;
    GLint vague_location = -1;
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
        glLinkProgram(program);
        glDeleteShader(vertex);
        glDeleteShader(fragment);

        GLint linked = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (!linked) {
            char log[512]{};
            glGetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
            SDL_Log("transition shader failed to link: %s", log);
            glDeleteProgram(program);
            program = 0;
            return;
        }

        previous_location = glGetUniformLocation(program, "u_previous");
        next_location = glGetUniformLocation(program, "u_next");
        mask_location = glGetUniformLocation(program, "u_mask");
        offset_location = glGetUniformLocation(program, "u_offset");
        vague_location = glGetUniformLocation(program, "u_vague");

        static constexpr GLfloat quad[] = {
            -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f,
        };
        glGenVertexArrays(1, &vertex_array);
        glGenBuffers(1, &vertex_buffer);
        glBindVertexArray(vertex_array);
        glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        ready = glGetError() == GL_NO_ERROR;
        if (ready) {
            report_self_test();
        }
    }

    // Renders a known blend and says so loudly if the result is wrong, but
    // does not act on it.  A shader can compile, link, draw, and still be
    // wrong - mediump on a phone is genuinely 16-bit, so arithmetic that is
    // fine on a desktop GPU overflows there and the wipe comes out black.
    // Worth knowing about; not worth silently demoting a path that may well
    // be working, since a wrong picture is easier to notice and report than
    // an unexplained slow one.
    void report_self_test()
    {
        const auto solid = [](GLubyte r, GLubyte g, GLubyte b) {
            GLuint name = 0;
            const GLubyte pixel[4] = {r, g, b, 255};
            glGenTextures(1, &name);
            glBindTexture(GL_TEXTURE_2D, name);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, pixel);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            return name;
        };
        const GLuint previous = solid(255, 0, 0);
        const GLuint next = solid(0, 255, 0);
        const GLuint mask = solid(255, 255, 255);
        GLuint target = 0;
        glGenTextures(1, &target);
        glBindTexture(GL_TEXTURE_2D, target);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        GLint previous_fbo = 0;
        GLint viewport[4] = {0, 0, 0, 0};
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_fbo);
        glGetIntegerv(GL_VIEWPORT, viewport);
        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, target, 0);

        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
            glViewport(0, 0, 2, 2);
            glDisable(GL_BLEND);
            glDisable(GL_SCISSOR_TEST);
            glUseProgram(program);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, previous);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, next);
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, mask);
            glUniform1i(previous_location, 0);
            glUniform1i(next_location, 1);
            glUniform1i(mask_location, 2);
            glUniform1f(vague_location, 128.0f);
            glBindVertexArray(vertex_array);

            GLubyte pixel[4] = {0, 0, 0, 0};
            // Past the end of the ramp the result must be the incoming frame.
            glUniform1f(offset_location, 384.0f);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
            if (!(pixel[1] > 200 && pixel[0] < 64)) {
                SDL_Log("pattern wipe shader looks wrong: expected the "
                        "incoming frame, got rgba(%d,%d,%d,%d) - wipes will "
                        "render incorrectly on this device",
                        pixel[0], pixel[1], pixel[2], pixel[3]);
            }
            // Before it starts, the outgoing one.
            glUniform1f(offset_location, 0.0f);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
            if (!(pixel[0] > 200 && pixel[1] < 64)) {
                SDL_Log("pattern wipe shader looks wrong: expected the "
                        "outgoing frame, got rgba(%d,%d,%d,%d) - wipes will "
                        "render incorrectly on this device",
                        pixel[0], pixel[1], pixel[2], pixel[3]);
            }
            glBindVertexArray(0);
            glUseProgram(0);
        }

        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previous_fbo));
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        glDeleteFramebuffers(1, &fbo);
        const GLuint scratch[] = {previous, next, mask, target};
        glDeleteTextures(4, scratch);
        glActiveTexture(GL_TEXTURE0);
        // Whatever it found, the shader still runs: this reports, it does not
        // decide.  Clear any error it raised so it cannot affect readiness.
        while (glGetError() != GL_NO_ERROR) {
        }
    }

    ~Impl()
    {
        if (vertex_array) {
            glDeleteVertexArrays(1, &vertex_array);
        }
        if (vertex_buffer) {
            glDeleteBuffers(1, &vertex_buffer);
        }
        if (program) {
            glDeleteProgram(program);
        }
    }
};

namespace {

// SDL only guarantees these calls are meaningful when it is itself drawing
// through GL or GLES.  With the GPU renderer (Vulkan, D3D12, Metal) the
// context does not exist and the texture handles mean nothing.
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

GlPatternTransition::GlPatternTransition(SDL_Renderer* renderer)
    : impl_(renderer_uses_gl(renderer) ? std::make_unique<Impl>() : nullptr)
{
    if (!impl_) {
        SDL_Log("pattern wipe shader: renderer is not GL, keeping the CPU blend");
    }
}

GlPatternTransition::~GlPatternTransition() = default;

bool GlPatternTransition::available() const
{
    return impl_ && impl_->ready;
}

bool GlPatternTransition::draw(
    SDL_Renderer* renderer, SDL_Texture* previous, SDL_Texture* next,
    SDL_Texture* mask, float offset, float vague)
{
    if (!available()) {
        return false;
    }
    const GLuint previous_name = texture_name(previous);
    const GLuint next_name = texture_name(next);
    const GLuint mask_name = texture_name(mask);
    if (!previous_name || !next_name || !mask_name) {
        return false;
    }
    // SDL batches; anything still queued has to land before the framebuffer
    // this draws into is the one SDL thinks it is.
    if (!SDL_FlushRenderer(renderer)) {
        return false;
    }

    // Sampling state travels with the texture, not the sampler uniform, and
    // these textures belong to SDL - whatever it last set is what applies.
    // A texture whose filter wants mipmaps it does not have is incomplete,
    // and an incomplete texture samples as solid black rather than failing,
    // so this has to be set explicitly on every one of them.
    const auto bind = [](GLenum unit, GLuint name, GLint filter) {
        glActiveTexture(unit);
        glBindTexture(GL_TEXTURE_2D, name);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };
    glUseProgram(impl_->program);
    bind(GL_TEXTURE0, previous_name, GL_LINEAR);
    bind(GL_TEXTURE1, next_name, GL_LINEAR);
    // The CPU blend indexes the mask with integer division, so this one stays
    // nearest or the two paths disagree along the wipe's edge.
    bind(GL_TEXTURE2, mask_name, GL_NEAREST);
    glUniform1i(impl_->previous_location, 0);
    glUniform1i(impl_->next_location, 1);
    glUniform1i(impl_->mask_location, 2);
    glUniform1f(impl_->offset_location, offset);
    glUniform1f(impl_->vague_location, vague);

    // Put these back afterwards rather than trusting SDL to reset them: it
    // caches its own idea of the GL state and only sets what it thinks has
    // changed.
    const GLboolean blend_was_on = glIsEnabled(GL_BLEND);
    const GLboolean scissor_was_on = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glBindVertexArray(impl_->vertex_array);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    if (blend_was_on) {
        glEnable(GL_BLEND);
    }
    if (scissor_was_on) {
        glEnable(GL_SCISSOR_TEST);
    }

    // Put back what SDL expects to find; it caches this state rather than
    // setting it per draw.
    glActiveTexture(GL_TEXTURE0);
    glUseProgram(0);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    return glGetError() == GL_NO_ERROR;
}

#else  // No GL headers here; SDL_GPU is the route to a shader instead.

struct GlPatternTransition::Impl {};

GlPatternTransition::GlPatternTransition(SDL_Renderer*) {}
GlPatternTransition::~GlPatternTransition() = default;
bool GlPatternTransition::available() const { return false; }

bool GlPatternTransition::draw(
    SDL_Renderer*, SDL_Texture*, SDL_Texture*, SDL_Texture*, float, float)
{
    return false;
}

#endif

}  // namespace th2
