#pragma once

#include "archive.hpp"

#include "gl_blend.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace th2 {

class GameFont {
public:
    explicit GameFont(const Archive& archive);
    ~GameFont();

    int glyph_width(unsigned char character) const;
    bool authentic() const { return authentic_; }
    void configure(bool authentic, std::string_view family,
                   int font_size, float framebuffer_scale);
    static const std::vector<std::string>& system_families();

    // A font that ships with the app, for the platforms without fontconfig.
    struct BundledFont {
        std::string_view family;
        std::string_view path;
    };
    static std::span<const BundledFont> bundled_fonts();
    float text_width(std::string_view text) const;
    // Changes whenever configure() changes the face or size, so a caller
    // can remember measurements and know when they went stale.
    std::uint64_t generation() const { return generation_; }
    // Cumulative width at every glyph boundary of a line, measured once and
    // remembered.  Drawing a partially revealed line needs the left and right
    // edge of each glyph, and asking text_width() for a prefix per glyph
    // re-measures the whole line from the start every time - quadratic in the
    // line, repeated every frame, which put FreeType's kerning lookup among
    // the busiest functions in a playthrough.  Entry k is the width of the
    // first k glyphs, so entry 0 is 0 and the last entry is the whole line.
    const std::vector<float>& glyph_boundaries(std::string_view line) const;
    // Line advance the face itself asks for, in logical units; 0 when the
    // modern font is unavailable.
    float line_height() const;
    // Distance from the top of a line to the text baseline, in logical
    // units; 0 for the bitmap font, which has no metrics to ask.
    float ascent() const;
    // `alpha_256` is the rasteriser's own alph2, 0..256, and when it is
    // given the glyph is composited with the engine's integer arithmetic
    // instead of SDL's - see set_glyph_colour in font.cpp.  -1 keeps the
    // straight-alpha path, which is what every caller outside the vanilla
    // message layout wants.
    void draw(
        SDL_Renderer* renderer, float x, float y, std::string_view text,
        std::uint8_t red = 255, std::uint8_t green = 255,
        std::uint8_t blue = 255, std::uint8_t alpha = 255,
        int alpha_256 = -1) const;
    void draw_original(
        SDL_Renderer* renderer, float x, float y, std::string_view text,
        std::uint8_t red = 255, std::uint8_t green = 255,
        std::uint8_t blue = 255, std::uint8_t alpha = 255) const;
    void draw_save_menu(
        SDL_Renderer* renderer, float x, float y, std::string_view text,
        std::uint8_t red = 255, std::uint8_t green = 255,
        std::uint8_t blue = 255, std::uint8_t alpha = 255) const;
    void draw_authentic_shadow(
        SDL_Renderer* renderer, float x, float y, std::string_view text,
        std::uint8_t alpha = 255, int alpha_256 = -1) const;

    // Composite glyphs through the engine's own arithmetic instead of
    // plotting them point by point.
    //
    // A point draw leaves the destination half of the blend to the blend
    // unit, which rounds where FNT_Draw truncates, and a glyph over its own
    // shadow stacks two of those - the last pixels in the port that were
    // more than a level off the reference.  Handing the mask to the shader
    // fixes that and is far cheaper besides: one composite per glyph instead
    // of a RenderPoint per covered pixel.
    //
    // Null disables it and the point path stands in unchanged.
    void set_exact_blend(GlExactBlend* blend);

private:
    std::uint64_t generation_ = 0;
    struct Modern;
    struct ExactGlyphs;
    std::unique_ptr<ExactGlyphs> exact_;
    // Returns false when the mask could not be composited, in which case the
    // caller plots it.  `alpha` (0..255) stands in for a missing alpha_256
    // on a layer target - see the definition.
    bool draw_mask_exact(
        SDL_Renderer* renderer, float x, float y, int width, int height,
        const std::uint8_t* bitmap, int red, int green, int blue,
        int alpha_256, int alpha = -1) const;
    static constexpr int size = 24;
    static constexpr int width = 12;
    std::vector<std::uint8_t> data_;
    std::vector<std::uint8_t> save_menu_data_;
    std::vector<std::uint8_t> shadow_data_;
    int shadow_width_ = 0;
    std::unique_ptr<Modern> modern_;
    mutable std::unordered_map<std::string, std::vector<float>>
        boundary_cache_;
    bool authentic_ = false;
    std::string family_;
    int font_size_ = size;
    float framebuffer_scale_ = 1.0f;

    const std::uint8_t* glyph(unsigned char character) const;
    void draw_bitmap(
        SDL_Renderer* renderer, float x, float y, std::string_view text,
        std::uint8_t red, std::uint8_t green, std::uint8_t blue,
        std::uint8_t alpha, int alpha_256 = -1) const;
    void draw_bitmap_face(
        SDL_Renderer* renderer, const std::vector<std::uint8_t>& data,
        int font_size, int half_width, float x, float y,
        std::string_view text, std::uint8_t red, std::uint8_t green,
        std::uint8_t blue, std::uint8_t alpha, int alpha_256 = -1) const;
};

}  // namespace th2
