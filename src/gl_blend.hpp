#pragma once

#include <SDL3/SDL.h>

#include <memory>

namespace th2 {

// The 2002 engine's compositing arithmetic, run through GLES.
//
// The SDL_GPU version of this (Display::ExactBlend, shaders/blend) can only
// do the source half of a blend: SDL 3.4.16 never populates
// SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER, so no second texture can be bound
// and the destination is left to the fixed-function blend unit, which rounds
// where the engine truncates.  GL hands out the texture name behind an
// SDL_Texture, so here the destination can be sampled and the whole of
//
//     dest = BlendTable[256 - a][dest] + BlendTable[a][src]
//
// done in integer, exactly.  It is also the stack that runs in the browser,
// where SDL_GPU does not exist at all.
//
// Best-effort by design, like GlPatternTransition: if SDL is not drawing
// through GLES, or the shader will not build, available() is false and the
// caller keeps whatever it was doing.
class GlExactBlend {
public:
    explicit GlExactBlend(SDL_Renderer* renderer);
    ~GlExactBlend();

    GlExactBlend(const GlExactBlend&) = delete;
    GlExactBlend& operator=(const GlExactBlend&) = delete;

    bool available() const;

    // Asks the next draw() to read the destination: it copies the part of
    // the current render target it covers into an internally held scratch
    // first.  Returns false if there is nothing to copy (drawing straight to
    // the window).
    bool capture_destination(SDL_Renderer* renderer);

    // Composites `source`'s `src` rectangle into the current render target's
    // `dst` rectangle.  `alpha` is DRW_BLD's parameter on the rasteriser's
    // 0..256 scale and the brightnesses are BrightTable indices, 128
    // neutral.  Requires a capture_destination() since the last draw into
    // the target.  Returns false if it could not draw, in which case nothing
    // was drawn and the caller should fall back.
    // `source2` is DSP_SetGraphBSet's second bitmap and `pair` the rate it
    // mixes at, 0..256 - the graph is then one sprite whose pixels are a mix
    // of the two, composited in a single pass the way DRW_DrawBMP_TTT_Bld
    // does it.  Both bitmaps are sampled at the same texel, so they must be
    // the same size or `src` must mean the same rectangle in each.
    bool draw(SDL_Renderer* renderer, SDL_Texture* source,
              const SDL_FRect& src, const SDL_FRect& dst,
              bool flip_x, bool flip_y, int mode,
              int alpha, int bright_r, int bright_g, int bright_b,
              SDL_Texture* source2 = nullptr, int pair = 256,
              const SDL_Rect* clip = nullptr,
              // DRW_DrawPOLY4_TT's per-line spans, 8 ints a row (see
              // Display::poly4_rows).  `dst` must then cover the target.
              const int* poly_rows = nullptr, int poly_row_count = 0);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace th2
