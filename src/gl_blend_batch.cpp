#include "gl_blend.hpp"
#include "gl_blend_impl.hpp"

#include "image.hpp"

// A run of glyphs in one draw.  draw() is one quad per call, and for text
// that is one call per glyph and per shadow - in WebGL each of them a
// destination copy, a dozen state changes and a draw, every one a command
// the GPU process validates and forwards: a full message page was ~20000 GL
// calls a frame.  Here a whole pass of the typewriter is one copy and one
// draw, with the per-glyph values carried per vertex.
//
// The fragment shader is gl_blend.cpp's, built with BATCH defined, so what
// a pixel comes out as is the same code whichever path drew it.  What makes
// one draw equivalent to many is the caller's promise that no two quads ink
// the same pixel: each then reads the destination exactly as the separate
// draw would have, and the texels with no coverage are discarded, so a
// quad's empty corner never overwrites a neighbour's ink.

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace th2 {

#ifdef TH2_GL_BLEND

using namespace gl_blend_detail;

namespace {

constexpr char batch_vertex_source[] = R"(#version 300 es
precision highp float;
precision highp int;
in vec2 a_position;      // clip space
in ivec4 a_src_rect;
in ivec4 a_dst_rect;
in ivec4 a_alpha_ink;    // alpha, then the ink's r, g, b
out vec2 v_uv;
flat out ivec4 v_src_rect;
flat out ivec4 v_dst_rect;
flat out ivec4 v_alpha_ink;
void main()
{
    v_uv = vec2(0.0);    // the mask path indexes texels, not v_uv
    v_src_rect = a_src_rect;
    v_dst_rect = a_dst_rect;
    v_alpha_ink = a_alpha_ink;
    gl_Position = vec4(a_position, 0.0, 1.0);
}
)";

}  // namespace

bool GlExactBlend::Impl::build_batch()
{
    if (batch_tried) {
        return batch_ready;
    }
    batch_tried = true;
    // The shared fragment source with BATCH defined, which has to come
    // after #version and before anything else.
    std::string fragment_text(fragment_source);
    const auto line_end = fragment_text.find('\n');
    if (line_end == std::string::npos) {
        return false;
    }
    fragment_text.insert(line_end + 1, "#define BATCH 1\n");
    const GLuint vertex = compile(GL_VERTEX_SHADER, batch_vertex_source);
    const GLuint fragment = compile(GL_FRAGMENT_SHADER, fragment_text.c_str());
    if (!vertex || !fragment) {
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        return false;
    }
    batch_program = glCreateProgram();
    glAttachShader(batch_program, vertex);
    glAttachShader(batch_program, fragment);
    glBindAttribLocation(batch_program, 0, "a_position");
    glBindAttribLocation(batch_program, 1, "a_src_rect");
    glBindAttribLocation(batch_program, 2, "a_dst_rect");
    glBindAttribLocation(batch_program, 3, "a_alpha_ink");
    glLinkProgram(batch_program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint linked = 0;
    glGetProgramiv(batch_program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512]{};
        glGetProgramInfoLog(batch_program, sizeof(log) - 1, nullptr, log);
        SDL_Log("exact blend batch shader failed to link: %s", log);
        glDeleteProgram(batch_program);
        batch_program = 0;
        return false;
    }
    batch_source_location = glGetUniformLocation(batch_program, "u_source");
    batch_dest_location = glGetUniformLocation(batch_program, "u_dest");
    batch_source2_location = glGetUniformLocation(batch_program, "u_source2");
    batch_rows_location = glGetUniformLocation(batch_program, "u_rows");
    batch_target_location = glGetUniformLocation(batch_program, "u_target");
    batch_mode_location = glGetUniformLocation(batch_program, "u_mode");
    batch_layer_location = glGetUniformLocation(batch_program, "u_layer");
    batch_flip_location = glGetUniformLocation(batch_program, "u_flip");

    glGenVertexArrays(1, &batch_vertex_array);
    glGenBuffers(1, &batch_vertex_buffer);
    glBindVertexArray(batch_vertex_array);
    glBindBuffer(GL_ARRAY_BUFFER, batch_vertex_buffer);
    constexpr auto stride = static_cast<GLsizei>(sizeof(BatchVertex));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<const void*>(
                              offsetof(BatchVertex, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribIPointer(1, 4, GL_INT, stride,
                           reinterpret_cast<const void*>(
                               offsetof(BatchVertex, src)));
    glEnableVertexAttribArray(2);
    glVertexAttribIPointer(2, 4, GL_INT, stride,
                           reinterpret_cast<const void*>(
                               offsetof(BatchVertex, dst)));
    glEnableVertexAttribArray(3);
    glVertexAttribIPointer(3, 4, GL_INT, stride,
                           reinterpret_cast<const void*>(
                               offsetof(BatchVertex, alpha_ink)));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    batch_ready = glGetError() == GL_NO_ERROR;
    return batch_ready;
}

bool GlExactBlend::batch_available(SDL_Renderer* renderer)
{
    if (!available()) {
        return false;
    }
    if (!impl_->batch_tried && !SDL_FlushRenderer(renderer)) {
        return false;
    }
    return impl_->build_batch();
}

bool GlExactBlend::draw_mask_batch(
    SDL_Renderer* renderer, SDL_Texture* atlas, const MaskQuad* quads,
    std::size_t count)
{
    if (count == 0) {
        return true;
    }
    if (!available() || !impl_->captured || !impl_->build_batch()) {
        return false;
    }
    // Viewport and render scale exactly as draw() applies them; see there.
    SDL_Rect sdl_viewport{};
    SDL_GetRenderViewport(renderer, &sdl_viewport);
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    SDL_GetRenderScale(renderer, &scale_x, &scale_y);
    // Flush first, then touch GL - draw() says why.
    if (!SDL_FlushRenderer(renderer)) {
        return false;
    }
    const GLuint atlas_name = texture_name(atlas);
    if (!atlas_name) {
        return false;
    }
    SDL_Texture* const target = SDL_GetRenderTarget(renderer);
    float target_w = 0.0f;
    float target_h = 0.0f;
    if (!target || !SDL_GetTextureSize(target, &target_w, &target_h)
        || target_w <= 0.0f || target_h <= 0.0f) {
        return false;
    }
    const bool layer = th2::texture_is_premultiplied_layer(target);

    const auto to_int = [](float value) {
        return static_cast<GLint>(std::lround(value));
    };
    auto& vertices = impl_->batch_vertices;
    vertices.clear();
    vertices.reserve(count * 6);
    int box_x0 = static_cast<int>(target_w);
    int box_y0 = static_cast<int>(target_h);
    int box_x1 = 0;
    int box_y1 = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const MaskQuad& q = quads[i];
        const SDL_FRect dst{
            (q.dst.x + static_cast<float>(sdl_viewport.x)) * scale_x,
            (q.dst.y + static_cast<float>(sdl_viewport.y)) * scale_y,
            q.dst.w * scale_x, q.dst.h * scale_y};
        box_x0 = std::min(box_x0, static_cast<int>(std::floor(dst.x)));
        box_y0 = std::min(box_y0, static_cast<int>(std::floor(dst.y)));
        box_x1 = std::max(box_x1, static_cast<int>(std::ceil(dst.x + dst.w)));
        box_y1 = std::max(box_y1, static_cast<int>(std::ceil(dst.y + dst.h)));
        // Clip space, y not inverted - draw()'s first coordinate fact.
        const float x0 = dst.x / target_w * 2.0f - 1.0f;
        const float x1 = (dst.x + dst.w) / target_w * 2.0f - 1.0f;
        const float y0 = dst.y / target_h * 2.0f - 1.0f;
        const float y1 = (dst.y + dst.h) / target_h * 2.0f - 1.0f;
        Impl::BatchVertex v{};
        v.src[0] = to_int(q.src.x);
        v.src[1] = to_int(q.src.y);
        v.src[2] = to_int(q.src.w);
        v.src[3] = to_int(q.src.h);
        v.dst[0] = to_int(dst.x);
        v.dst[1] = to_int(dst.y);
        v.dst[2] = to_int(dst.w);
        v.dst[3] = to_int(dst.h);
        v.alpha_ink[0] = std::clamp(q.alpha, 0, 256);
        v.alpha_ink[1] = q.red;
        v.alpha_ink[2] = q.green;
        v.alpha_ink[3] = q.blue;
        for (const auto [px, py] : {std::pair{x0, y0}, std::pair{x1, y0},
                                    std::pair{x0, y1}, std::pair{x1, y0},
                                    std::pair{x1, y1}, std::pair{x0, y1}}) {
            v.x = px;
            v.y = py;
            vertices.push_back(v);
        }
    }
    impl_->captured = false;
    if (box_x1 <= box_x0 || box_y1 <= box_y0) {
        return true;
    }
    // One copy of the whole run's box: every quad reads the destination as
    // it stood before the batch, which is what the separate draws read too
    // when no two of them ink the same pixel.
    if (!layer
        && !impl_->copy_destination(static_cast<int>(target_w),
                                    static_cast<int>(target_h), box_x0,
                                    box_y0, box_x1, box_y1)) {
        return true;
    }
    const GLuint dest_name = impl_->scratch_name;

    impl_->use(impl_->batch_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, atlas_name);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, dest_name);
    if (!impl_->batch_samplers_set) {
        // All four, though only two are read: samplers left at their
        // default all point at unit 0, and u_rows is an isampler2D - two
        // sampler types on one unit fail every draw with INVALID_OPERATION
        // in WebGL (desktop Mesa lets it pass).
        glUniform1i(impl_->batch_source_location, 0);
        glUniform1i(impl_->batch_dest_location, 1);
        glUniform1i(impl_->batch_source2_location, 2);
        glUniform1i(impl_->batch_rows_location, 3);
        impl_->batch_samplers_set = true;
    }
    impl_->uniform_f(impl_->batch_target_location, target_w, target_h);
    impl_->uniform_i(impl_->batch_mode_location, 2);
    impl_->uniform_i(impl_->batch_layer_location, layer ? 1 : 0);
    impl_->uniform_i(impl_->batch_flip_location, 0, 0);
    if (layer) {
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glDisable(GL_BLEND);
    }
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, static_cast<GLsizei>(target_w),
               static_cast<GLsizei>(target_h));
    glBindVertexArray(impl_->batch_vertex_array);
    glBindBuffer(GL_ARRAY_BUFFER, impl_->batch_vertex_buffer);
    const auto bytes = vertices.size() * sizeof(Impl::BatchVertex);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(bytes),
                 vertices.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices.size()));
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE0);
    glUseProgram(0);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    strict_check("the glyph batch");
    return true;
}

#else   // no GLES headers: never available, so never asked

bool GlExactBlend::draw_mask_batch(SDL_Renderer*, SDL_Texture*,
                                   const MaskQuad*, std::size_t)
{
    return false;
}

bool GlExactBlend::batch_available(SDL_Renderer*)
{
    return false;
}

#endif

}  // namespace th2
