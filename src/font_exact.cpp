#include "font.hpp"

#include "image.hpp"
#include "texture.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace th2 {

// The glyph masks, in one atlas, and the blend that composites them - in
// batches.
//
// Keyed by the address of the glyph's bitmap plus its size: the bitmaps live
// inside data_ / shadow_data_, which outlive the cache, so the address
// identifies the glyph.  Alpha is NOT part of the key - alph2 travels with
// each quad, so one mask serves every step of the typewriter's reveal.
//
// One draw per glyph was most of what a text frame cost the browser: a
// destination copy and a draw each, ~20000 GL calls for a full page.  So the
// masks share one texture and a run of glyphs goes to the GPU as one draw
// (GlExactBlend::draw_mask_batch).  That is only the same picture if no two
// glyphs in a run ink the same pixel, because every one of them reads the
// destination as it was before the run - so each mask also knows the box of
// its inked texels, and a glyph whose box would overlap one already queued
// starts a new run.  Shadows, which are wider than their advance, overlap
// their neighbours often; the glyphs over them rarely do.
struct GameFont::ExactGlyphs {
    GlExactBlend* blend = nullptr;

    // The atlas: shelves of masks, filled left to right and top to bottom.
    // Full, it is emptied and refilled - after the queue is flushed, since
    // queued quads point into it.
    static constexpr int atlas_size = 1024;
    th2app::Texture atlas;
    int shelf_x = 0;
    int shelf_y = 0;
    int shelf_h = 0;
    struct Slot {
        int x = 0;
        int y = 0;
        // The inked texels, [x0, x1) x [y0, y1) in the mask's own
        // coordinates; empty (x1 <= x0) for a mask with no coverage at all.
        int x0 = 0;
        int y0 = 0;
        int x1 = 0;
        int y1 = 0;
    };
    std::unordered_map<std::string, Slot> slots;

    // The run being built, and the renderer state it was built under.
    std::vector<GlExactBlend::MaskQuad> queue;
    std::vector<SDL_Rect> inked;    // target pixels, padded by one
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* target = nullptr;
    SDL_Rect viewport{};
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    int depth = 0;

    static std::string key(const std::uint8_t* bitmap, int w, int h)
    {
        char buffer[48];
        std::snprintf(buffer, sizeof buffer, "%p/%d/%d",
                      static_cast<const void*>(bitmap), w, h);
        return buffer;
    }

    bool ensure_atlas(SDL_Renderer* to)
    {
        if (atlas) {
            return true;
        }
        atlas.reset(SDL_CreateTexture(to, SDL_PIXELFORMAT_RGBA32,
                                      SDL_TEXTUREACCESS_STATIC, atlas_size,
                                      atlas_size));
        if (!atlas) {
            return false;
        }
        SDL_SetTextureScaleMode(atlas.get(), SDL_SCALEMODE_NEAREST);
        SDL_SetTextureBlendMode(atlas.get(), SDL_BLENDMODE_NONE);
        return true;
    }

    // The mask's slot, uploading it on first use.
    const Slot* slot(SDL_Renderer* to, const std::uint8_t* bitmap, int width,
                     int height)
    {
        const auto name = key(bitmap, width, height);
        if (const auto found = slots.find(name); found != slots.end()) {
            return &found->second;
        }
        if (width > atlas_size || height > atlas_size || !ensure_atlas(to)) {
            return nullptr;
        }
        if (shelf_x + width > atlas_size) {
            shelf_x = 0;
            shelf_y += shelf_h;
            shelf_h = 0;
        }
        if (shelf_y + height > atlas_size) {
            flush();
            slots.clear();
            shelf_x = shelf_y = shelf_h = 0;
        }
        Slot s;
        s.x = shelf_x;
        s.y = shelf_y;
        s.x0 = width;
        s.y0 = height;
        std::vector<std::uint8_t> pixels(
            static_cast<std::size_t>(width) * height * 4);
        const int stride = (width + 1) / 2;
        for (int row = 0; row < height; ++row) {
            for (int column = 0; column < width; ++column) {
                const auto packed = bitmap[row * stride + column / 2];
                const int coverage =
                    column % 2 == 0 ? (packed & 0x0f) : (packed >> 4);
                // 17*c, so a 0..15 coverage survives an 8 bit channel
                // exactly and the shader divides it back out.  In every
                // channel, not just alpha: this backend hands GL its
                // textures with the colour channels swizzled (see the
                // shader), and a mask that depends on picking the right one
                // would be a silent solid block wherever the guess was
                // wrong.  The colour is the ink's job.
                const auto value = static_cast<std::uint8_t>(coverage * 17);
                auto* out = pixels.data()
                    + (static_cast<std::size_t>(row) * width + column) * 4;
                out[0] = out[1] = out[2] = out[3] = value;
                if (coverage) {
                    s.x0 = std::min(s.x0, column);
                    s.y0 = std::min(s.y0, row);
                    s.x1 = std::max(s.x1, column + 1);
                    s.y1 = std::max(s.y1, row + 1);
                }
            }
        }
        const SDL_Rect where{s.x, s.y, width, height};
        if (!SDL_UpdateTexture(atlas.get(), &where, pixels.data(), width * 4)) {
            return nullptr;
        }
        shelf_x += width;
        shelf_h = std::max(shelf_h, height);
        return &slots.emplace(name, s).first->second;
    }

    // Composites the queued run.  Into the state it was built under, which
    // is the renderer's own unless something changed it mid-run.
    void flush()
    {
        if (queue.empty()) {
            return;
        }
        SDL_Texture* const held_target = SDL_GetRenderTarget(renderer);
        SDL_Rect held_viewport{};
        SDL_GetRenderViewport(renderer, &held_viewport);
        float held_x = 1.0f;
        float held_y = 1.0f;
        SDL_GetRenderScale(renderer, &held_x, &held_y);
        const bool moved = held_target != target
            || held_viewport.x != viewport.x || held_viewport.y != viewport.y
            || held_viewport.w != viewport.w || held_viewport.h != viewport.h
            || held_x != scale_x || held_y != scale_y;
        if (moved) {
            SDL_SetRenderTarget(renderer, target);
            SDL_SetRenderViewport(renderer, &viewport);
            SDL_SetRenderScale(renderer, scale_x, scale_y);
        }
        if (blend->capture_destination(renderer)) {
            blend->draw_mask_batch(renderer, atlas.get(), queue.data(),
                                   queue.size());
        }
        if (moved) {
            SDL_SetRenderTarget(renderer, held_target);
            SDL_SetRenderViewport(renderer, &held_viewport);
            SDL_SetRenderScale(renderer, held_x, held_y);
        }
        queue.clear();
        inked.clear();
    }
};

void GameFont::ExactGlyphsDelete::operator()(ExactGlyphs* glyphs) const
{
    delete glyphs;
}

void GameFont::set_exact_blend(GlExactBlend* blend)
{
    if (!blend) {
        exact_.reset();
        return;
    }
    if (!exact_) {
        exact_.reset(new ExactGlyphs);
    }
    exact_->blend = blend;
}

void GameFont::begin_batch() const
{
    if (exact_) {
        ++exact_->depth;
    }
}

void GameFont::end_batch() const
{
    if (exact_ && exact_->depth > 0 && --exact_->depth == 0) {
        exact_->flush();
    }
}

bool GameFont::draw_mask_exact(
    SDL_Renderer* renderer, float x, float y, int width, int height,
    const std::uint8_t* bitmap, int red, int green, int blue,
    int alpha_256, int alpha, Columns columns) const
{
    // A caller with no engine alpha - the backlog, the save screens - used
    // to plot the mask a point at a time instead: 16% of backlog frames
    // over 8 ms with the bitmap font, every glyph and shadow a few hundred
    // SDL_RenderPoint calls.  On a layer, which is normal play's text and
    // is never compared, its 0..255 alpha stands in on the engine's scale.
    // Into the picture nothing changes: that is what a trace reads.
    SDL_Texture* const target = SDL_GetRenderTarget(renderer);
    if (alpha_256 < 0 && alpha >= 0
        && th2::texture_is_premultiplied_layer(target)) {
        alpha_256 = (std::min(alpha, 255) * 256 + 127) / 255;
    }
    // The window has nothing to copy, so there is no exact path into it.
    if (!exact_ || !exact_->blend || !exact_->blend->batch_available(renderer)
        || alpha_256 < 0 || width <= 0 || height <= 0 || !target) {
        // The caller plots this one itself, now - after anything queued.
        if (exact_) {
            exact_->flush();
        }
        return false;
    }
    auto& e = *exact_;
    SDL_Rect viewport{};
    SDL_GetRenderViewport(renderer, &viewport);
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    SDL_GetRenderScale(renderer, &scale_x, &scale_y);
    if (!e.queue.empty()
        && (renderer != e.renderer || target != e.target
            || viewport.x != e.viewport.x || viewport.y != e.viewport.y
            || viewport.w != e.viewport.w || viewport.h != e.viewport.h
            || scale_x != e.scale_x || scale_y != e.scale_y)) {
        e.flush();
    }
    const auto* s = e.slot(renderer, bitmap, width, height);
    if (!s) {
        e.flush();
        return false;
    }
    if (s->x1 <= s->x0) {
        return true;    // no coverage anywhere: every texel would discard
    }
    const int first = std::max(0, columns.first);
    const int last = std::min(width, columns.last);
    if (last <= first) {
        return true;    // the engine clip leaves nothing of it
    }
    // The inked box in target pixels, as draw_mask_batch will place the
    // quad, padded a pixel each way so rounding cannot hide a shared pixel.
    const float tx = (x + static_cast<float>(viewport.x)) * scale_x;
    const float ty = (y + static_cast<float>(viewport.y)) * scale_y;
    const SDL_Rect ink{
        static_cast<int>(std::floor(tx + s->x0 * scale_x)) - 1,
        static_cast<int>(std::floor(ty + s->y0 * scale_y)) - 1,
        static_cast<int>(std::ceil((s->x1 - s->x0) * scale_x)) + 2,
        static_cast<int>(std::ceil((s->y1 - s->y0) * scale_y)) + 2};
    for (const auto& queued : e.inked) {
        if (SDL_HasRectIntersection(&ink, &queued)) {
            e.flush();
            break;
        }
    }
    e.renderer = renderer;
    e.target = target;
    e.viewport = viewport;
    e.scale_x = scale_x;
    e.scale_y = scale_y;
    e.queue.push_back({
        SDL_FRect{static_cast<float>(s->x + first), static_cast<float>(s->y),
                  static_cast<float>(last - first), static_cast<float>(height)},
        SDL_FRect{x + static_cast<float>(first), y,
                  static_cast<float>(last - first), static_cast<float>(height)},
        alpha_256, red, green, blue});
    e.inked.push_back(ink);
    if (e.depth == 0) {
        e.flush();
    }
    return true;
}

}  // namespace th2
