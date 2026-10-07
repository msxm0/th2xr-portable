#pragma once

// The exact blend's private state, shared by gl_blend.cpp and
// gl_blend_batch.cpp.  Not for anyone else: everything here assumes SDL is
// drawing through GLES and that the caller has flushed it first.

#include "gl_blend.hpp"

#if defined(__EMSCRIPTEN__) || defined(TH2_HAVE_GLES3)
#define TH2_GL_BLEND 1
#include <GLES3/gl3.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace th2 {

namespace gl_blend_detail {
// See GlExactBlend::set_strict.
void strict_check(const char* what);
GLuint compile(GLenum type, const char* source);
// SDL exposes the GL name under a different property per backend.
GLuint texture_name(SDL_Texture* texture);
extern const char vertex_source[];
extern const char fragment_source[];
}  // namespace gl_blend_detail

struct GlExactBlend::Impl {
    GLuint program = 0;
    GLuint vertex_buffer = 0;
    GLuint vertex_array = 0;
    GLint source_location = -1;
    GLint source2_location = -1;
    GLint source2_scale_location = -1;
    GLint rows_location = -1;
    GLint row_count_location = -1;
    GLuint rows_name = 0;
    GLint src_rect_location = -1;
    GLint dst_rect_location = -1;
    GLint src2_origin_location = -1;
    GLint flip_location = -1;
    GLint pair_location = -1;
    GLint folded_location = -1;
    GLint dest_location = -1;
    GLint target_location = -1;
    GLint alpha_location = -1;
    GLint bright_locations[3] = {-1, -1, -1};
    GLint mode_location = -1;
    GLint ink_location = -1;
    GLint layer_location = -1;
    bool ready = false;

    // A copy of the render target.  Sampling the texture being drawn into is
    // a feedback loop and undefined, so the destination is read from here
    // instead.
    //
    // A raw GL texture rather than an SDL one: SDL's GLES backend does not
    // publish SDL_PROP_TEXTURE_OPENGLES2_TEXTURE_NUMBER for textures created
    // with SDL_TEXTUREACCESS_TARGET (an ordinary texture gets a name, a
    // target gets 0), so an SDL scratch could never be bound as a sampler.
    // glCopyTexSubImage2D reads the bound framebuffer directly, which is
    // both simpler and one less full render pass than copying through SDL.
    GLuint scratch_name = 0;
    // The program's uniforms as last set.  Nothing but this class uses the
    // program, so a value that has not changed since the last draw does not
    // need sending again - and in WebGL every call is a command the GPU
    // process validates and forwards, ~45 of them per glyph before this.
    // Per program: the batch program's locations are its own.
    bool samplers_set = false;
    GLuint active_program = 0;
    std::unordered_map<std::uint64_t, std::array<GLint, 4>> uniform_cache;
    void use(GLuint which)
    {
        glUseProgram(which);
        active_program = which;
    }
    bool changed(GLint location, std::array<GLint, 4> value)
    {
        const auto key = (static_cast<std::uint64_t>(active_program) << 32)
            | static_cast<std::uint32_t>(location);
        auto [at, inserted] = uniform_cache.try_emplace(key, value);
        if (!inserted && at->second == value) {
            return false;
        }
        at->second = value;
        return true;
    }
    void uniform_i(GLint location, GLint a)
    {
        if (changed(location, {a, 0, 0, 0})) glUniform1i(location, a);
    }
    void uniform_i(GLint location, GLint a, GLint b)
    {
        if (changed(location, {a, b, 0, 0})) glUniform2i(location, a, b);
    }
    void uniform_i(GLint location, GLint a, GLint b, GLint c)
    {
        if (changed(location, {a, b, c, 0})) glUniform3i(location, a, b, c);
    }
    void uniform_i(GLint location, GLint a, GLint b, GLint c, GLint d)
    {
        if (changed(location, {a, b, c, d})) glUniform4i(location, a, b, c, d);
    }
    void uniform_f(GLint location, float a, float b)
    {
        if (changed(location, {std::bit_cast<GLint>(a), std::bit_cast<GLint>(b), 0, 0})) {
            glUniform2f(location, a, b);
        }
    }
    int scratch_width = 0;
    int scratch_height = 0;
    bool captured = false;

    // Copies [x0,x1)x[y0,y1) of the bound framebuffer, a width x height
    // render target, into the scratch.  False if the box is empty.
    bool copy_destination(int width, int height, int x0, int y0, int x1,
                          int y1);

    // The batched glyph path (gl_blend_batch.cpp): the same fragment shader
    // built with BATCH defined, so the rectangles, alpha and ink come from
    // per-vertex attributes instead of uniforms, and a whole run of glyphs
    // is one draw.  Built on first use.
    struct BatchVertex {
        GLfloat x, y;
        GLint src[4];
        GLint dst[4];
        GLint alpha_ink[4];
    };
    GLuint batch_program = 0;
    GLuint batch_vertex_array = 0;
    GLuint batch_vertex_buffer = 0;
    std::size_t batch_buffer_bytes = 0;
    GLint batch_source_location = -1;
    GLint batch_dest_location = -1;
    GLint batch_source2_location = -1;
    GLint batch_rows_location = -1;
    GLint batch_target_location = -1;
    GLint batch_mode_location = -1;
    GLint batch_layer_location = -1;
    GLint batch_flip_location = -1;
    bool batch_samplers_set = false;
    bool batch_tried = false;
    bool batch_ready = false;
    std::vector<BatchVertex> batch_vertices;
    bool build_batch();

    Impl();
    ~Impl();
};

}  // namespace th2

#endif
