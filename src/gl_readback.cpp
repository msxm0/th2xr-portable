#include "gl_readback.hpp"


#include <cstring>
#include <string_view>

#if defined(__EMSCRIPTEN__) || defined(TH2_HAVE_GLES3)
#define TH2_GL_READBACK 1
#include <GLES3/gl3.h>
#ifdef __EMSCRIPTEN__
// WebGL 2's read of a buffer's contents, which GLES 3 does not have (it maps
// the buffer instead, and WebGL cannot).  Declared here rather than through
// <webgl/webgl2.h>, which redeclares the whole of GLES 3 next to gl3.h.
extern "C" void glGetBufferSubData(GLenum target, GLintptr offset,
                                   GLsizeiptr size, void* data);
#endif
#endif

namespace th2 {

#ifdef TH2_GL_READBACK

namespace {

// Only SDL's GLES renderer: its targets are framebuffer objects in the
// context this code calls into, and it reads them back as plain RGBA rows,
// top row first (GLES2_RenderReadPixels), which is what this does too.
bool renderer_is_gles(SDL_Renderer* renderer)
{
    const char* name = SDL_GetRendererName(renderer);
    return name && std::string_view(name) == "opengles2";
}

}  // namespace

struct GlAsyncReadback::Impl {
    GLuint buffer = 0;
    GLsizeiptr capacity = 0;
    GLsync fence = nullptr;
    int width = 0;
    int height = 0;

    ~Impl()
    {
        if (fence) {
            glDeleteSync(fence);
        }
        if (buffer) {
            glDeleteBuffers(1, &buffer);
        }
    }

    void drop()
    {
        if (fence) {
            glDeleteSync(fence);
            fence = nullptr;
        }
    }
};

GlAsyncReadback::GlAsyncReadback(SDL_Renderer* renderer)
    : impl_(renderer_is_gles(renderer) ? std::make_unique<Impl>() : nullptr)
{
}

GlAsyncReadback::~GlAsyncReadback() = default;

bool GlAsyncReadback::available() const { return impl_ != nullptr; }

bool GlAsyncReadback::pending() const { return impl_ && impl_->fence; }

bool GlAsyncReadback::start(SDL_Renderer* renderer, SDL_Texture* target,
                            int width, int height)
{
    if (!impl_ || !target || width <= 0 || height <= 0) {
        return false;
    }
    impl_->drop();
    // Through SDL, so its idea of the bound framebuffer stays true: setting
    // the target and flushing leaves the target's framebuffer bound, with
    // everything queued for it already submitted.
    SDL_Texture* const held = SDL_GetRenderTarget(renderer);
    if (!SDL_SetRenderTarget(renderer, target) || !SDL_FlushRenderer(renderer)) {
        SDL_SetRenderTarget(renderer, held);
        return false;
    }
    const GLsizeiptr size = static_cast<GLsizeiptr>(width) * height * 4;
    if (!impl_->buffer) {
        glGenBuffers(1, &impl_->buffer);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, impl_->buffer);
    if (impl_->capacity != size) {
        glBufferData(GL_PIXEL_PACK_BUFFER, size, nullptr, GL_STREAM_READ);
        impl_->capacity = size;
    }
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    impl_->fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    // The fence has to reach the GPU for it ever to signal.
    glFlush();
    SDL_SetRenderTarget(renderer, held);
#ifdef __EMSCRIPTEN__
    // glGetError is a round trip to the GPU process in WebGL, the very wait
    // this exists to avoid; a failed read shows up as a black thumbnail.
    const bool failed = !impl_->fence;
#else
    const bool failed = glGetError() != GL_NO_ERROR || !impl_->fence;
#endif
    if (failed) {
        impl_->drop();
        return false;
    }
    impl_->width = width;
    impl_->height = height;
    return true;
}

SDL_Surface* GlAsyncReadback::take(bool wait)
{
    if (!pending()) {
        return nullptr;
    }
    if (!wait) {
        // A zero timeout only asks; WebGL allows no other from script.
        const GLenum status = glClientWaitSync(impl_->fence, 0, 0);
        if (status != GL_ALREADY_SIGNALED
            && status != GL_CONDITION_SATISFIED) {
            return nullptr;
        }
    }
    // Waiting needs no call of its own: reading the buffer waits for the
    // copy into it.
    impl_->drop();
    SDL_Surface* surface = SDL_CreateSurface(impl_->width, impl_->height,
                                             SDL_PIXELFORMAT_RGBA32);
    if (!surface) {
        return nullptr;
    }
    const GLsizeiptr row = static_cast<GLsizeiptr>(impl_->width) * 4;
    const GLsizeiptr size = row * impl_->height;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, impl_->buffer);
    bool copied = false;
    auto* const out = static_cast<unsigned char*>(surface->pixels);
#ifdef __EMSCRIPTEN__
    if (surface->pitch == row) {
        glGetBufferSubData(GL_PIXEL_PACK_BUFFER, 0, size, out);
    } else {
        for (int y = 0; y < impl_->height; ++y) {
            glGetBufferSubData(GL_PIXEL_PACK_BUFFER, row * y, row,
                               out + static_cast<std::size_t>(y) * surface->pitch);
        }
    }
    copied = true;
#else
    if (const void* mapped = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, size,
                                              GL_MAP_READ_BIT)) {
        const auto* in = static_cast<const unsigned char*>(mapped);
        for (int y = 0; y < impl_->height; ++y) {
            std::memcpy(out + static_cast<std::size_t>(y) * surface->pitch,
                        in + row * y, static_cast<std::size_t>(row));
        }
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        copied = true;
    }
#endif
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
#ifndef __EMSCRIPTEN__
    copied = copied && glGetError() == GL_NO_ERROR;
#endif
    if (!copied) {
        SDL_DestroySurface(surface);
        return nullptr;
    }
    return surface;
}

#else

struct GlAsyncReadback::Impl {};
GlAsyncReadback::GlAsyncReadback(SDL_Renderer*) {}
GlAsyncReadback::~GlAsyncReadback() = default;
bool GlAsyncReadback::available() const { return false; }
bool GlAsyncReadback::pending() const { return false; }
bool GlAsyncReadback::start(SDL_Renderer*, SDL_Texture*, int, int)
{
    return false;
}
SDL_Surface* GlAsyncReadback::take(bool) { return nullptr; }

#endif

}  // namespace th2
