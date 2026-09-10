#pragma once

#include <SDL3/SDL.h>

#include <memory>

namespace th2 {

// The pattern wipes blend two frames through a mask, per pixel, per frame.
// Doing that in C++ means reading both frames back off the GPU, and
// SDL_RenderReadPixels is a pipeline sync - it waits for everything queued to
// finish, which at high DPI cost tens of milliseconds twice a wipe.
//
// SDL_Renderer has no way to hand it a fragment shader: that lives in SDL_GPU,
// whose backends are Vulkan, D3D12 and Metal, so the browser cannot use it.
// The browser can, however, reach the same WebGL context SDL is drawing
// through, and SDL hands out the GL name behind a texture.  This runs the
// blend there instead, with no readback at all.
//
// It is best-effort by design: if the renderer is not a GL one, or the
// shader will not build, or the texture handles are missing, available() is
// false and the caller keeps the CPU path.  Those are the cases where the
// shader cannot run at all - it deliberately does not judge what the shader
// draws, because silently demoting a working GPU path is worse than showing
// that something is wrong.
class GlPatternTransition {
public:
    explicit GlPatternTransition(SDL_Renderer* renderer);
    ~GlPatternTransition();

    GlPatternTransition(const GlPatternTransition&) = delete;
    GlPatternTransition& operator=(const GlPatternTransition&) = delete;

    bool available() const;

    // Draws mix(previous, next, ramp(mask)) over the current render target,
    // matching the CPU blend:
    //     alpha = clamp((mask + offset - 256) * 256 / vague, 0, 255)
    // Returns false if it could not draw, in which case nothing was drawn and
    // the caller should fall back.
    bool draw(SDL_Renderer* renderer, SDL_Texture* previous, SDL_Texture* next,
              SDL_Texture* mask, float offset, float vague);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace th2
