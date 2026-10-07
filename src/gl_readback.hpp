#pragma once

#include <SDL3/SDL.h>

#include <memory>

namespace th2 {

// Reads a render target back without stopping for the GPU.
//
// SDL_RenderReadPixels waits for every command before it to finish and then
// copies the pixels out - in a browser a synchronous round trip to the GPU
// process, 4 ms on a quiet machine and 16 ms on a busy one, in whatever frame
// asked.  This queues the copy into a pixel buffer behind a fence instead,
// and hands the pixels over once the fence has passed, a frame or two later.
//
// Only where SDL draws through GLES 3 (the browser's WebGL 2, desktop GLES);
// elsewhere available() is false and the caller reads synchronously.
class GlAsyncReadback {
public:
    explicit GlAsyncReadback(SDL_Renderer* renderer);
    ~GlAsyncReadback();
    GlAsyncReadback(const GlAsyncReadback&) = delete;
    GlAsyncReadback& operator=(const GlAsyncReadback&) = delete;

    bool available() const;
    bool pending() const;

    // Queues a read of the top-left width x height of `target`, which must
    // be an SDL target texture the renderer has finished drawing into (this
    // flushes it).  False if the read could not be queued; nothing is
    // pending then.  A read already pending is dropped.
    bool start(SDL_Renderer* renderer, SDL_Texture* target, int width,
               int height);

    // The pixels as an RGBA32 surface once the GPU has them, otherwise
    // null.  With `wait`, blocks until they are there.  Either way a surface
    // is handed over once; after that nothing is pending.
    SDL_Surface* take(bool wait);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace th2
