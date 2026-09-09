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

constexpr char fragment_source[] = R"(#version 300 es
precision mediump float;
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
        (mask + u_offset - 256.0) * 256.0 / u_vague, 0.0, 255.0) / 255.0;
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

    glUseProgram(impl_->program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, previous_name);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, next_name);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, mask_name);
    glUniform1i(impl_->previous_location, 0);
    glUniform1i(impl_->next_location, 1);
    glUniform1i(impl_->mask_location, 2);
    glUniform1f(impl_->offset_location, offset);
    glUniform1f(impl_->vague_location, vague);

    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glBindVertexArray(impl_->vertex_array);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

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
