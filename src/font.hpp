#pragma once

#include "archive.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
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
    // Line advance the face itself asks for, in logical units; 0 when the
    // modern font is unavailable.
    float line_height() const;
    // Distance from the top of a line to the text baseline, in logical
    // units; 0 for the bitmap font, which has no metrics to ask.
    float ascent() const;
    void draw(
        SDL_Renderer* renderer, float x, float y, std::string_view text,
        std::uint8_t red = 255, std::uint8_t green = 255,
        std::uint8_t blue = 255, std::uint8_t alpha = 255) const;
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
        std::uint8_t alpha = 255) const;

private:
    struct Modern;
    static constexpr int size = 24;
    static constexpr int width = 12;
    std::vector<std::uint8_t> data_;
    std::vector<std::uint8_t> save_menu_data_;
    std::vector<std::uint8_t> shadow_data_;
    int shadow_width_ = 0;
    std::unique_ptr<Modern> modern_;
    bool authentic_ = false;
    std::string family_;
    int font_size_ = size;
    float framebuffer_scale_ = 1.0f;

    const std::uint8_t* glyph(unsigned char character) const;
    void draw_bitmap(
        SDL_Renderer* renderer, float x, float y, std::string_view text,
        std::uint8_t red, std::uint8_t green, std::uint8_t blue,
        std::uint8_t alpha) const;
    void draw_bitmap_face(
        SDL_Renderer* renderer, const std::vector<std::uint8_t>& data,
        int font_size, int half_width, float x, float y,
        std::string_view text, std::uint8_t red, std::uint8_t green,
        std::uint8_t blue, std::uint8_t alpha) const;
};

}  // namespace th2
