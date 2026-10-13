#pragma once

#include "cunny_weights.hpp"

#include <SDL3/SDL.h>

#include <memory>

namespace th2 {

// CuNNy, a small convolutional upscaler trained on visual novel screenshots
// and CG (its NVL models), run through GLES on the art layer.
//
// Like GlAnime4K this replaces one draw only - the art, magnified into the
// letterbox - and never touches the 800x600 art target, which is what the
// accuracy harness compares.  Unlike it, the network is too heavy to run on
// every frame for a picture that has not changed: it runs into a 2x texture
// when the art changes, GlLanczos takes that to the letterbox's size, and
// every frame in between draws the same result 1:1.
//
// Best-effort: if SDL is not drawing through GLES or a shader will not
// build, available() is false and the caller blits the art as before - as
// it also does while the shaders are still building.
class GlCunny {
public:
    GlCunny(SDL_Renderer* renderer, CunnyModel model);
    ~GlCunny();

    GlCunny(const GlCunny&) = delete;
    GlCunny& operator=(const GlCunny&) = delete;

    // Ready to run.  False while the shaders are still being built
    // (building()) and for good if they failed to build.
    bool available() const;
    // The build is not over yet: still compiling, or compiled and waiting
    // for upscale() to set the programs up - which changes GL state SDL
    // caches, so it happens only there, after SDL has been flushed.
    bool building() const;

    // `art` upscaled to width x height, to be drawn 1:1 in its place.  Null
    // until the build is over (see building()).  The
    // network runs when `art_changed` says the art was drawn since the last
    // call, or when `art` is not the texture it last ran on, and the
    // resampling when the network ran or the size changed; otherwise the
    // last result is returned as it is.  Null if it could not run - draw
    // `art` instead.
    SDL_Texture* upscale(SDL_Renderer* renderer, SDL_Texture* art,
                         bool art_changed, int width, int height);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace th2
