#pragma once

#include <SDL3/SDL.h>

#include <memory>

namespace th2 {

// Anime4K's upscale pass, run through GLES.
//
// The SDL_GPU build of this (src/anime4k.cpp) cannot serve the browser -
// SDL_GPU has no WebGL backend - so it left the web target with no upscaler
// at all, and the desktop lost its own when the renderer moved to GLES.
// This is the same fused shader against the same art texture, drawn with the
// GL the browser and the desktop both have.
//
// It replaces exactly one draw: the art layer, magnified into the letterbox
// rectangle.  The text, overlay and sidebar layers stay with SDL, because
// they are composited 1:1 and there is nothing for an upscaler to do to
// them.  Nothing here touches the 800x600 art target itself, which is what
// the accuracy harness compares - this is presentation, and it is the one
// part of the pipeline that is deliberately not the 2002 engine's output.
//
// Best-effort, like GlPatternTransition: if SDL is not drawing through GLES,
// or the shader will not build, available() is false and the caller falls
// back to a plain magnified blit.
class GlAnime4K {
public:
    explicit GlAnime4K(SDL_Renderer* renderer);
    ~GlAnime4K();

    GlAnime4K(const GlAnime4K&) = delete;
    GlAnime4K& operator=(const GlAnime4K&) = delete;

    bool available() const;

    // Draws `art` magnified into `destination` on the current target, which
    // is the window.  Returns false if it could not draw, in which case
    // nothing was drawn and the caller should blit normally.
    bool draw(SDL_Renderer* renderer, SDL_Texture* art,
              const SDL_FRect& destination);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace th2
