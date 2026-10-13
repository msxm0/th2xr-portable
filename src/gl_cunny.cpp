#include "gl_cunny.hpp"

// Same build rule as gl_anime4k: wherever SDL draws through GLES and the
// headers exist.
#if defined(__EMSCRIPTEN__) || defined(TH2_HAVE_GLES3)
#define TH2_GL_CUNNY 1
#include <GLES3/gl3.h>
#endif

#include "cunny_model.hpp"
#include "gl_lanczos.hpp"

#include <SDL3/SDL_log.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace th2 {

#ifdef TH2_GL_CUNNY

namespace {

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

// GL_COMPLETION_STATUS_KHR, from KHR_parallel_shader_compile; not in the
// GLES 3.0 headers.
constexpr GLenum completion_status = 0x91B1;

// Four channels written per pass at most: what GLES 3.0 and WebGL2
// guarantee for GL_MAX_DRAW_BUFFERS, so every device runs the same passes.
constexpr int max_outputs = 4;

}  // namespace

struct GlCunny::Impl {
    std::vector<CunnyPass> passes;
    std::vector<GLuint> programs;
    GLuint vertex_array = 0;
    // Every pass's weights in one buffer: pass i's block b is the range
    // weight_ranges[i][b] (offset, size), bound to binding point b.
    GLuint weights = 0;
    std::vector<std::vector<std::pair<GLintptr, GLsizeiptr>>> weight_ranges;
    // The hidden layers' four-channel textures, two sets used in turn: a
    // layer reads the set the one before it wrote.
    int set_blocks = 0;
    std::array<std::vector<GLuint>, 2> blocks;
    std::vector<GLuint> framebuffers;   // one per pass
    int width = 0;
    int height = 0;
    GLuint upscaled = 0;                // the network's 2x picture
    GLuint last_art = 0;
    bool fresh = false;
    // The 2x picture resampled to the letterbox, as an SDL texture so SDL
    // draws it; rebuilt when the network ran or the window size changed.
    GlLanczos lanczos;
    SDL_Texture* resampled = nullptr;
    GLuint resampled_name = 0;
    int resampled_w = 0;
    int resampled_h = 0;
    bool resampled_fresh = false;
    // The programs' build: see the constructor.
    std::chrono::steady_clock::time_point started;
    GLuint vertex = 0;
    std::vector<GLuint> fragments;
    bool parallel = false;
    bool building = false;
    bool ready = false;

    explicit Impl(CunnyModel model)
    {
        const auto network = parse_cunny(cunny_weights(model));
        if (!network) {
            SDL_Log("cunny: the embedded weights do not parse");
            return;
        }
        for (std::size_t i = 0; i + 1 < network->convs.size(); ++i) {
            set_blocks = std::max(set_blocks, network->convs[i].outputs / 4);
        }
        passes = cunny_passes(*network, max_outputs);

        // Started here, finished in finish_build: with
        // KHR_parallel_shader_compile the driver builds the programs in the
        // background and poll() asks whether it is done, so choosing a
        // model does not stall the game.  Without the extension the first
        // status query below simply waits, as any GL compile does.
        started = std::chrono::steady_clock::now();
        parallel = SDL_GL_ExtensionSupported("GL_KHR_parallel_shader_compile");
        const char* vertex_source = cunny_vertex_shader();
        vertex = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vertex, 1, &vertex_source, nullptr);
        glCompileShader(vertex);
        for (const auto& pass : passes) {
            const char* source = pass.fragment.c_str();
            const GLuint fragment = glCreateShader(GL_FRAGMENT_SHADER);
            glShaderSource(fragment, 1, &source, nullptr);
            glCompileShader(fragment);
            const GLuint program = glCreateProgram();
            glAttachShader(program, vertex);
            glAttachShader(program, fragment);
            glLinkProgram(program);
            fragments.push_back(fragment);
            programs.push_back(program);
        }
        building = true;
    }

    // Whether the driver has finished compiling - never waits with the
    // extension (GL_COMPLETION_STATUS_KHR); without it, the link status
    // query in finish_build is what waits.  Changes no GL state.
    bool compiled() const
    {
        if (!parallel) {
            return true;
        }
        for (const GLuint program : programs) {
            GLint done = GL_FALSE;
            glGetProgramiv(program, completion_status, &done);
            if (!done) {
                return false;
            }
        }
        return true;
    }

    void finish_build()
    {
        building = false;
        bool linked_all = true;
        for (std::size_t i = 0; i < programs.size(); ++i) {
            GLint linked = 0;
            glGetProgramiv(programs[i], GL_LINK_STATUS, &linked);
            if (!linked) {
                char log[512]{};
                glGetShaderInfoLog(fragments[i], sizeof(log) - 1, nullptr, log);
                if (!log[0]) {
                    glGetProgramInfoLog(programs[i], sizeof(log) - 1, nullptr,
                                        log);
                }
                SDL_Log("cunny pass %zu failed to build: %s", i, log);
                linked_all = false;
                break;
            }
        }
        for (const GLuint fragment : fragments) {
            glDeleteShader(fragment);
        }
        fragments.clear();
        glDeleteShader(vertex);
        vertex = 0;
        if (!linked_all) {
            return;
        }
        // Texture units: the art on 0 when the pass reads it, then the input
        // blocks in order.  Fixed per program, so set once.
        for (std::size_t i = 0; i < programs.size(); ++i) {
            const auto& pass = passes[i];
            glUseProgram(programs[i]);
            int unit = 0;
            if (pass.reads_art) {
                glUniform1i(glGetUniformLocation(programs[i], "u_art"), unit++);
            }
            for (int b = 0; b < pass.input_blocks; ++b) {
                const std::string name = "u_in" + std::to_string(b);
                glUniform1i(glGetUniformLocation(programs[i], name.c_str()),
                            unit++);
            }
        }
        glUseProgram(0);
        if (!upload_weights()) {
            return;
        }
        glGenVertexArrays(1, &vertex_array);
        ready = lanczos.ready() && glGetError() == GL_NO_ERROR;
        SDL_Log("cunny: %zu passes built in %lld ms%s", passes.size(),
                static_cast<long long>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started).count()),
                parallel ? " (in the background)" : "");
    }

    // The uniform blocks into one buffer, each range aligned as the driver
    // asks, and each program's block W<b> tied to binding point b.
    bool upload_weights()
    {
        GLint align = 256;
        glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &align);
        align = std::max(align, 16);
        std::vector<unsigned char> data;
        weight_ranges.assign(passes.size(), {});
        for (std::size_t i = 0; i < passes.size(); ++i) {
            const auto& blocks = passes[i].uniform_blocks;
            for (std::size_t b = 0; b < blocks.size(); ++b) {
                const std::size_t offset =
                    (data.size() + static_cast<std::size_t>(align) - 1)
                    / static_cast<std::size_t>(align)
                    * static_cast<std::size_t>(align);
                const std::size_t size = blocks[b].size() * sizeof(float);
                data.resize(offset + size);
                std::memcpy(data.data() + offset, blocks[b].data(), size);
                weight_ranges[i].emplace_back(
                    static_cast<GLintptr>(offset),
                    static_cast<GLsizeiptr>(size));
                const std::string name = "W" + std::to_string(b);
                const GLuint index =
                    glGetUniformBlockIndex(programs[i], name.c_str());
                if (index == GL_INVALID_INDEX) {
                    SDL_Log("cunny pass %zu has no block %s", i, name.c_str());
                    return false;
                }
                glUniformBlockBinding(programs[i], index,
                                      static_cast<GLuint>(b));
            }
        }
        glGenBuffers(1, &weights);
        glBindBuffer(GL_UNIFORM_BUFFER, weights);
        glBufferData(GL_UNIFORM_BUFFER, static_cast<GLsizeiptr>(data.size()),
                     data.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
        return true;
    }

    ~Impl()
    {
        if (weights) {
            glDeleteBuffers(1, &weights);
        }
        release_targets();
        release_resampled();
        for (const GLuint program : programs) {
            glDeleteProgram(program);
        }
        for (const GLuint fragment : fragments) {
            glDeleteShader(fragment);
        }
        if (vertex) {
            glDeleteShader(vertex);
        }
        if (vertex_array) {
            glDeleteVertexArrays(1, &vertex_array);
        }
    }

    void release_targets()
    {
        if (!framebuffers.empty()) {
            glDeleteFramebuffers(static_cast<GLsizei>(framebuffers.size()),
                                 framebuffers.data());
            framebuffers.clear();
        }
        for (auto& set : blocks) {
            if (!set.empty()) {
                glDeleteTextures(static_cast<GLsizei>(set.size()), set.data());
                set.clear();
            }
        }
        if (upscaled) {
            glDeleteTextures(1, &upscaled);
            upscaled = 0;
        }
        width = 0;
        height = 0;
        fresh = false;
        resampled_fresh = false;
    }

    void release_resampled()
    {
        if (resampled) {
            lanczos.forget_target();
            SDL_DestroyTexture(resampled);
            resampled = nullptr;
        }
        resampled_name = 0;
        resampled_w = 0;
        resampled_h = 0;
        resampled_fresh = false;
    }

    GLuint make_texture(int w, int h)
    {
        GLuint texture = 0;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        return texture;
    }

    // The SDL texture the resampled picture lands in, w x h.
    bool ensure_resampled(SDL_Renderer* renderer, int w, int h, SDL_Texture* art)
    {
        if (resampled && w == resampled_w && h == resampled_h) {
            return true;
        }
        release_resampled();
        resampled = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                  SDL_TEXTUREACCESS_TARGET, w, h);
        if (!resampled) {
            return false;
        }
        // Drawn 1:1; nearest keeps a half-texel rounding from blurring it.
        SDL_SetTextureScaleMode(resampled, SDL_SCALEMODE_NEAREST);
        SDL_BlendMode blend = SDL_BLENDMODE_BLEND;
        SDL_GetTextureBlendMode(art, &blend);
        SDL_SetTextureBlendMode(resampled, blend);
        // SDL may create the GL texture only once its queue runs.
        if (!SDL_FlushRenderer(renderer)) {
            return false;
        }
        resampled_name = texture_name(resampled);
        resampled_w = w;
        resampled_h = h;
        return resampled_name != 0;
    }

    // Textures and framebuffers for an art of w x h; false if the driver
    // will not give them.
    bool ensure_targets(int w, int h)
    {
        if (w == width && h == height && upscaled) {
            return true;
        }
        release_targets();
        upscaled = make_texture(w * 2, h * 2);
        for (auto& set : blocks) {
            set.resize(static_cast<std::size_t>(set_blocks));
            for (auto& texture : set) {
                texture = make_texture(w, h);
            }
        }
        glBindTexture(GL_TEXTURE_2D, 0);

        GLint previous = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
        framebuffers.resize(passes.size());
        glGenFramebuffers(static_cast<GLsizei>(framebuffers.size()),
                          framebuffers.data());
        bool complete = true;
        for (std::size_t i = 0; i < passes.size() && complete; ++i) {
            const auto& pass = passes[i];
            glBindFramebuffer(GL_FRAMEBUFFER, framebuffers[i]);
            std::array<GLenum, max_outputs> attachments{};
            for (int b = 0; b < pass.blocks; ++b) {
                const GLuint texture = pass.output
                    ? upscaled
                    : blocks[static_cast<std::size_t>(pass.layer % 2)]
                            [static_cast<std::size_t>(pass.first_block + b)];
                attachments[static_cast<std::size_t>(b)] =
                    GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(b);
                glFramebufferTexture2D(GL_FRAMEBUFFER,
                                       GL_COLOR_ATTACHMENT0 + b,
                                       GL_TEXTURE_2D, texture, 0);
            }
            glDrawBuffers(pass.blocks, attachments.data());
            complete = glCheckFramebufferStatus(GL_FRAMEBUFFER)
                == GL_FRAMEBUFFER_COMPLETE;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previous));
        if (!complete) {
            SDL_Log("cunny: a pass's framebuffer is incomplete");
            release_targets();
            return false;
        }
        width = w;
        height = h;
        return true;
    }

    // State SDL's next draw sets again: SDL_FlushRenderer, before these,
    // marks its cached GL state invalid (see gl_blend.cpp).  The framebuffer
    // binding is the caller's to put back, because SDL keeps drawing to the
    // window it thinks is still bound.
    void run(GLuint art)
    {
        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);
        glBindVertexArray(vertex_array);
        // Linear for the output pass's bilinear base; texelFetch, which
        // every other read is, ignores it.  The texture is SDL's, so its
        // sampling state is set every time rather than assumed.
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, art);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        for (std::size_t i = 0; i < passes.size(); ++i) {
            const auto& pass = passes[i];
            glBindFramebuffer(GL_FRAMEBUFFER, framebuffers[i]);
            if (pass.output) {
                glViewport(0, 0, width * 2, height * 2);
            } else {
                glViewport(0, 0, width, height);
            }
            glUseProgram(programs[i]);
            for (std::size_t b = 0; b < weight_ranges[i].size(); ++b) {
                glBindBufferRange(GL_UNIFORM_BUFFER, static_cast<GLuint>(b),
                                  weights, weight_ranges[i][b].first,
                                  weight_ranges[i][b].second);
            }
            GLenum unit = GL_TEXTURE0;
            if (pass.reads_art) {
                glActiveTexture(unit++);
                glBindTexture(GL_TEXTURE_2D, art);
            }
            for (int b = 0; b < pass.input_blocks; ++b) {
                glActiveTexture(unit++);
                glBindTexture(
                    GL_TEXTURE_2D,
                    blocks[static_cast<std::size_t>(pass.input_layer % 2)]
                          [static_cast<std::size_t>(b)]);
            }
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        glBindVertexArray(0);
        glUseProgram(0);
        glActiveTexture(GL_TEXTURE0);
    }
};

GlCunny::GlCunny(SDL_Renderer* renderer, CunnyModel model)
{
    // GlLanczos sets its program up as it is built, so SDL's queue has to be
    // run and its cached state dropped first (see upscale()).
    if (renderer_uses_gl(renderer) && SDL_FlushRenderer(renderer)) {
        impl_ = std::make_unique<Impl>(model);
    }
}

GlCunny::~GlCunny() = default;

bool GlCunny::available() const { return impl_ && impl_->ready; }

bool GlCunny::building() const { return impl_ && impl_->building; }

SDL_Texture* GlCunny::upscale(SDL_Renderer* renderer, SDL_Texture* art,
                              bool art_changed, int width, int height)
{
    if (!impl_ || !art || width <= 0 || height <= 0
        || (impl_->building && !impl_->compiled())) {
        return nullptr;
    }
    // Everything SDL has queued - the art's own drawing above all - has to
    // reach the driver before the network reads it, and the flush marks
    // SDL's cached GL state invalid, so what follows may change it.
    if (!SDL_FlushRenderer(renderer)) {
        return nullptr;
    }
    if (impl_->building) {
        impl_->finish_build();
    }
    if (!available()) {
        return nullptr;
    }
    const GLuint art_name = texture_name(art);
    float w = 0.0f;
    float h = 0.0f;
    if (!art_name || !SDL_GetTextureSize(art, &w, &h)
        || !impl_->ensure_resampled(renderer, width, height, art)) {
        return nullptr;
    }
    const int art_w = static_cast<int>(w);
    const int art_h = static_cast<int>(h);
    const bool targets_ready = impl_->upscaled && art_w == impl_->width
        && art_h == impl_->height;
    const bool run_network = art_changed || !targets_ready || !impl_->fresh
        || art_name != impl_->last_art;
    if (!run_network && impl_->resampled_fresh) {
        return impl_->resampled;
    }
    GLint previous = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
    bool ok = impl_->ensure_targets(art_w, art_h);
    if (ok && run_network) {
        impl_->run(art_name);
        impl_->last_art = art_name;
        impl_->fresh = true;
    }
    ok = ok && impl_->lanczos.run(impl_->upscaled, impl_->width * 2,
                                  impl_->height * 2, impl_->resampled_name,
                                  width, height);
    impl_->resampled_fresh = ok;
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previous));
    glActiveTexture(GL_TEXTURE0);
    return ok ? impl_->resampled : nullptr;
}

#else   // no GLES headers: a stub that is never available

struct GlCunny::Impl {};
GlCunny::GlCunny(SDL_Renderer*, CunnyModel) : impl_(nullptr) {}
GlCunny::~GlCunny() = default;
bool GlCunny::available() const { return false; }
bool GlCunny::building() const { return false; }
SDL_Texture* GlCunny::upscale(SDL_Renderer*, SDL_Texture*, bool, int, int)
{
    return nullptr;
}

#endif

}  // namespace th2
