#include "game.hpp"

#include "icon.hpp"
#include "image.hpp"

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_system.h>
#include <imgui.h>
#include <zstd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <format>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numbers>
#include <ranges>
#include <stdexcept>
#include <unordered_set>

#ifdef _WIN32
#define localtime_r(timep, result) localtime_s(result, timep)
#endif

namespace th2app {

Game::CharacterTexture& Game::character_texture(int number)
{
    if (number < 0
        || static_cast<std::size_t>(number) >= character_textures_.size()) {
        throw std::out_of_range("unsupported character number");
    }
    return character_textures_[number];
}

Surface Game::capture_frame_pixels(bool art_only)
{
    (void)art_only;
    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer_);
    SDL_SetRenderTarget(renderer_, upscaler_->art_target());
    SDL_Surface* surface = SDL_RenderReadPixels(renderer_, nullptr);
    SDL_SetRenderTarget(renderer_, previous_target);
    if (!surface) {
        throw std::runtime_error(SDL_GetError());
    }
    SDL_Surface* converted = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(surface);
    if (!converted) {
        throw std::runtime_error(SDL_GetError());
    }
    return Surface(converted);
}

Texture Game::capture_frame_texture()
{
    SDL_Texture* source = upscaler_->art_target();
    float width = 0.0f;
    float height = 0.0f;
    if (!SDL_GetTextureSize(source, &width, &height)) {
        throw std::runtime_error(SDL_GetError());
    }
    Texture copy(SDL_CreateTexture(
        renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET,
        static_cast<int>(width), static_cast<int>(height)));
    if (!copy) {
        throw std::runtime_error(SDL_GetError());
    }

    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer_);
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    SDL_GetRenderScale(renderer_, &scale_x, &scale_y);
    SDL_SetRenderTarget(renderer_, copy.get());
    SDL_SetRenderScale(renderer_, 1.0f, 1.0f);
    // A straight copy, not a blend: the art target already holds the frame
    // exactly as it should be remembered.
    const SDL_BlendMode source_blend = [&] {
        SDL_BlendMode mode = SDL_BLENDMODE_BLEND;
        SDL_GetTextureBlendMode(source, &mode);
        return mode;
    }();
    SDL_SetTextureBlendMode(source, SDL_BLENDMODE_NONE);
    const bool drawn = SDL_RenderTexture(renderer_, source, nullptr, nullptr);
    SDL_SetTextureBlendMode(source, source_blend);
    SDL_SetRenderTarget(renderer_, previous_target);
    SDL_SetRenderScale(renderer_, scale_x, scale_y);
    if (!drawn) {
        throw std::runtime_error(SDL_GetError());
    }
    SDL_SetTextureBlendMode(copy.get(), SDL_BLENDMODE_BLEND);
    return copy;
}

Texture Game::texture_from_surface(SDL_Surface* surface)
{
    SDL_Texture* raw = SDL_CreateTextureFromSurface(renderer_, surface);
    if (!raw) {
        throw std::runtime_error(SDL_GetError());
    }
    SDL_SetTextureBlendMode(raw, SDL_BLENDMODE_BLEND);
    return Texture(raw);
}

void Game::retire_soak_gpu_work(bool force)
{
    constexpr std::size_t flush_interval = 64;
    if (!force && ++soak_renderer_ticks_ < flush_interval) {
        return;
    }
    soak_renderer_ticks_ = 0;
    // SDL_GPU retires transient uploads at present boundaries. Without
    // these, accelerated soak runs retain every scene's staging buffers.
    if (!SDL_SetRenderTarget(renderer_, nullptr)) {
        throw std::runtime_error(SDL_GetError());
    }
    SDL_SetRenderScale(renderer_, 1.0f, 1.0f);
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    if (!SDL_RenderClear(renderer_) || !SDL_RenderPresent(renderer_)) {
        throw std::runtime_error(SDL_GetError());
    }
}

bool Game::gl_transition_usable() const
{
    return !force_cpu_transitions && gl_transition_
        && gl_transition_->available();
}

// The mask only becomes a texture if the shader path is going to sample it;
// on the CPU path the bytes are all that is needed.
SDL_Texture* Game::transition_mask_texture(int type)
{
    auto& mask = const_cast<TransitionMask&>(transition_mask(type));
    if (mask.texture) {
        return mask.texture.get();
    }
    if (mask.width <= 0 || mask.height <= 0) {
        return nullptr;
    }
    Texture texture(SDL_CreateTexture(
        renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
        mask.width, mask.height));
    if (!texture) {
        return nullptr;
    }
    // The shader reads the red channel; the mask is a single value per pixel.
    std::vector<std::uint8_t> rgba(
        static_cast<std::size_t>(mask.width) * mask.height * 4);
    for (std::size_t i = 0; i < mask.pixels.size(); ++i) {
        const auto value = mask.pixels[i];
        rgba[i * 4 + 0] = value;
        rgba[i * 4 + 1] = value;
        rgba[i * 4 + 2] = value;
        rgba[i * 4 + 3] = 255;
    }
    if (!SDL_UpdateTexture(
            texture.get(), nullptr, rgba.data(), mask.width * 4)) {
        return nullptr;
    }
    // The CPU blend indexes the mask with integer division, so nearest keeps
    // the two paths producing the same edge.
    SDL_SetTextureScaleMode(texture.get(), SDL_SCALEMODE_NEAREST);
    mask.texture = std::move(texture);
    return mask.texture.get();
}

const Game::TransitionMask& Game::transition_mask(int type)
{
    if (const auto found = transition_masks_.find(type);
        found != transition_masks_.end()) {
        return found->second;
    }
    TransitionMask mask;
    mask.pixels = load_transition_mask(type, mask.width, mask.height);
    return transition_masks_.emplace(type, std::move(mask)).first->second;
}

// Runs on an idle frame only, and only once the mask's own bytes have
// arrived, so preparing it never turns into a blocking read.
void Game::prepare_pending_transition_mask()
{
    while (!pending_transition_masks_.empty()) {
        const int type = *pending_transition_masks_.begin();
        if (transition_masks_.contains(type)) {
            pending_transition_masks_.erase(pending_transition_masks_.begin());
            continue;
        }
        const auto name = std::format("f0{:03d}.bmp", type & 0x7f);
        const auto* entry = graphics_.find(name);
        if (!entry) {
            pending_transition_masks_.erase(pending_transition_masks_.begin());
            continue;  // Not in this release; nothing to prepare.
        }
        if (!graphics_.resident(*entry)) {
            return;  // Still in flight; try again on the next idle scan.
        }
        pending_transition_masks_.erase(pending_transition_masks_.begin());
        try {
            transition_mask(type);
        } catch (const std::exception&) {
            // A mask that will not load is not worth retrying every scan.
        }
        return;  // One per idle frame; the walk is not cheap.
    }
}

std::vector<std::uint8_t> Game::load_transition_mask(
    int type, int& width, int& height)
{
    const auto name = std::format("f0{:03d}.bmp", type & 0x7f);
    const auto* entry = graphics_.find(name);
    if (!entry) {
        throw std::runtime_error("transition mask not found: " + name);
    }
    Surface surface(th2::load_image(graphics_.read(*entry), entry->name));
    width = surface->w;
    height = surface->h;
    std::array<std::uint8_t, 256> curve{};
    for (int i = 0; i < 256; ++i) {
        curve[i] = static_cast<std::uint8_t>(i);
    }
    std::string_view curve_name;
    if (type & 0x100) {
        curve_name = type & 0x200 ? "rev_accel1.AMP"
            : type & 0x400 ? "rev_accel2.AMP" : "rev.AMP";
    } else if (type & 0x200) {
        curve_name = "accel1.AMP";
    } else if (type & 0x400) {
        curve_name = "accel2.AMP";
    }
    if (!curve_name.empty()) {
        const auto* curve_entry = graphics_.find(curve_name);
        if (!curve_entry) {
            throw std::runtime_error(
                "transition curve not found: " + std::string(curve_name));
        }
        const auto bytes = graphics_.read(*curve_entry);
        if (bytes.size() != curve.size()) {
            throw std::runtime_error(
                "invalid transition curve: " + std::string(curve_name));
        }
        std::copy(bytes.begin(), bytes.end(), curve.begin());
    }

    std::vector<std::uint8_t> mask(
        static_cast<std::size_t>(width) * height);
    const bool flip_x = type & 0x800;
    const bool flip_y = type & 0x1000;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            Uint8 r = 0;
            Uint8 g = 0;
            Uint8 b = 0;
            Uint8 a = 0;
            SDL_ReadSurfacePixel(
                surface.get(), flip_x ? width - x - 1 : x,
                flip_y ? height - y - 1 : y, &r, &g, &b, &a);
            mask[static_cast<std::size_t>(y) * width + x] = curve[b];
        }
    }
    return mask;
}

// The per-pixel sweeps always blend in C++.  Pattern wipes do too, unless the
// shader path is up, in which case nothing needs to come off the GPU at all.
bool Game::transition_needs_pixels(int type) const
{
    if (type >= 0x80) {
        return !gl_transition_usable();
    }
    return (type >= 2 && type <= 10) || type == 21;
}

void Game::begin_transition(
    int type, int frames, int vague, bool resume_script,
    EffectTiming timing)
{
    if (type < 0) {
        return;
    }
    // AVG_EffCnt4() is the same count in 30fps units with no Avg.wait in it,
    // so a menu fade lasts the same half second whatever the effect speed.
    const int effective_frames = timing == EffectTiming::menu
        ? frames * 2 : effect_frames(frames);
    Surface previous_pixels;
    Texture previous;
    if (transition_needs_pixels(type)) {
        previous_pixels = capture_frame_pixels(true);
        previous = texture_from_surface(previous_pixels.get());
    } else {
        previous = capture_frame_texture();
    }
    Transition transition{
        std::move(previous),
        std::move(previous_pixels),
        {},
        {},
        {},
        0,
        0,
        vague >= 0 ? vague : 128,
        effective_frames,
        type,
        resume_script,
        std::chrono::steady_clock::now(),
        next_transition_debug_id_++,
        -1,
        false,
    };
    if (type >= 0x80) {
        const auto& mask = transition_mask(type);
        transition.mask = mask.pixels;
        transition.mask_width = mask.width;
        transition.mask_height = mask.height;
    }
    transition_ = std::move(transition);
}

void Game::update_transition()
{
    if (!transition_) {
        return;
    }
    const auto elapsed = std::chrono::steady_clock::now()
        - transition_->started;
    const auto duration = std::chrono::duration<double>(
        static_cast<double>(transition_->frames) / 60.0);
    if (elapsed < duration) {
        return;
    }
    const bool resume_script = transition_->resume_script;
    transition_.reset();
    if (resume_script) {
        advance();
    }
}

// The shader form of draw_pattern_transition(): same blend, same mask, but
// both frames stay on the GPU, so the wipe costs no readback.  Returns false
// if anything is missing, and the caller falls back to the CPU blend.
bool Game::draw_pattern_transition_gpu(float progress)
{
    if (!gl_transition_usable()) {
        return false;
    }
    auto& transition = *transition_;
    if (!transition.previous) {
        return false;
    }
    if (!transition.composite) {
        // The new scene is on the art target; take a GPU-side copy of it the
        // same way the old one was taken.
        transition.composite = capture_frame_texture();
    }
    SDL_Texture* mask = transition_mask_texture(transition.type);
    if (!mask) {
        return false;
    }
    const int vague = std::clamp(transition.vague, 1, 256);
    const float offset = progress * static_cast<float>(256 + vague);
    return gl_transition_->draw(
        renderer_, transition.previous.get(), transition.composite.get(),
        mask, offset, static_cast<float>(vague));
}

void Game::draw_pattern_transition(float progress)
{
    auto& transition = *transition_;
    if (!transition.next_pixels) {
        transition.next_pixels = capture_frame_pixels();
        if (transition.next_pixels->w != transition.previous_pixels->w
            || transition.next_pixels->h != transition.previous_pixels->h) {
            throw std::runtime_error("transition frame size changed");
        }
        SDL_Texture* raw = SDL_CreateTexture(
            renderer_, SDL_PIXELFORMAT_RGBA32,
            SDL_TEXTUREACCESS_STREAMING,
            transition.next_pixels->w, transition.next_pixels->h);
        if (!raw) {
            throw std::runtime_error(SDL_GetError());
        }
        transition.composite.reset(raw);
    }

    const int width = transition.next_pixels->w;
    const int height = transition.next_pixels->h;
    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(width) * height * 4);
    const auto* previous =
        static_cast<const std::uint8_t*>(transition.previous_pixels->pixels);
    const auto* next =
        static_cast<const std::uint8_t*>(transition.next_pixels->pixels);
    const int vague = std::clamp(transition.vague, 1, 256);
    const int blend_offset = static_cast<int>(
        progress * static_cast<float>(256 + vague));

    for (int y = 0; y < height; ++y) {
        const int mask_y = y * transition.mask_height / height;
        for (int x = 0; x < width; ++x) {
            const int mask_x = x * transition.mask_width / width;
            const int mask = transition.mask[
                static_cast<std::size_t>(mask_y) * transition.mask_width
                + mask_x];
            const int alpha = std::clamp(
                (mask + blend_offset - 256) * 256 / vague, 0, 255);
            const auto source_offset =
                static_cast<std::size_t>(y) * transition.next_pixels->pitch
                + static_cast<std::size_t>(x) * 4;
            const auto previous_offset =
                static_cast<std::size_t>(y) * transition.previous_pixels->pitch
                + static_cast<std::size_t>(x) * 4;
            const auto output_offset =
                (static_cast<std::size_t>(y) * width + x) * 4;
            for (int channel = 0; channel < 3; ++channel) {
                pixels[output_offset + channel] = static_cast<std::uint8_t>(
                    (previous[previous_offset + channel] * (255 - alpha)
                     + next[source_offset + channel] * alpha)
                    / 255);
            }
            pixels[output_offset + 3] = 255;
        }
    }
    if (!SDL_UpdateTexture(
            transition.composite.get(), nullptr, pixels.data(), width * 4)) {
        throw std::runtime_error(SDL_GetError());
    }
    SDL_RenderTexture(
        renderer_, transition.composite.get(), nullptr, nullptr);
}

void Game::ensure_transition_target()
{
    auto& transition = *transition_;
    if (transition.composite) {
        return;
    }
    if (!transition_needs_pixels(transition.type)) {
        // The new scene is already on the art target; copying it there and
        // back through system memory would buy nothing.
        transition.composite = capture_frame_texture();
        return;
    }
    transition.next_pixels = capture_frame_pixels();
    if (transition.next_pixels->w != transition.previous_pixels->w
        || transition.next_pixels->h != transition.previous_pixels->h) {
        throw std::runtime_error("transition frame size changed");
    }
    transition.composite.reset(SDL_CreateTexture(
        renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
        transition.next_pixels->w, transition.next_pixels->h));
    if (!transition.composite) {
        throw std::runtime_error(SDL_GetError());
    }
}

void Game::draw_pixel_transition(float progress)
{
    ensure_transition_target();
    auto& transition = *transition_;
    const int width = transition.next_pixels->w;
    const int height = transition.next_pixels->h;
    const int rate = std::clamp(static_cast<int>(progress * 256.0f), 0, 256);
    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(width) * height * 4);
    const auto* previous =
        static_cast<const std::uint8_t*>(transition.previous_pixels->pixels);
    const auto* next =
        static_cast<const std::uint8_t*>(transition.next_pixels->pixels);

    auto source_alpha = [&](int x, int y) {
        switch (transition.type) {
        case 2: {
            const int edge = (height + 255) * (256 - rate) / 256;
            return std::clamp((y - edge + 255) * 256 / 255, 0, 256);
        }
        case 3: {
            const int edge = (height + 255) * rate / 256;
            return std::clamp((edge - y) * 256 / 255, 0, 256);
        }
        case 4: {
            const int edge = (width + 255) * (256 - rate) / 256;
            return std::clamp((x - edge + 255) * 256 / 255, 0, 256);
        }
        case 5: {
            const int edge = (width + 255) * rate / 256;
            return std::clamp((edge - x) * 256 / 255, 0, 256);
        }
        case 6: {
            const int edge = (width / 2 + 127) * (256 - rate) / 256;
            return std::clamp(
                (width / 2 - std::abs(x - width / 2) - edge + 127)
                    * 256 / 127,
                0, 256);
        }
        case 7: {
            const int edge = (width / 2 + 127) * rate / 256;
            return std::clamp(
                (std::abs(x - width / 2) - width / 2 + edge)
                    * 256 / 127,
                0, 256);
        }
        case 8:
        case 9:
        case 10: {
            const int shift = transition.type - 8;
            const int mask = 0x3f >> shift;
            const int half = 32 >> shift;
            const int extent = (96 >> shift) * rate / 256 - half;
            const bool inside = std::abs((x & mask) - half)
                < std::abs((y & mask) - half) + extent;
            return inside ? 16 + rate * rate / 512 : 0;
        }
        case 21: {
            std::uint32_t value = static_cast<std::uint32_t>(
                x + y * width + transition.debug_id * 0x9e3779b9U);
            value ^= value >> 16;
            value *= 0x7feb352dU;
            value ^= value >> 15;
            return static_cast<int>(value & 0xff) < rate ? 256 : 0;
        }
        default:
            return rate;
        }
    };

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int alpha = source_alpha(x, y);
            const auto old_offset =
                static_cast<std::size_t>(y) * transition.previous_pixels->pitch
                + static_cast<std::size_t>(x) * 4;
            const auto new_offset =
                static_cast<std::size_t>(y) * transition.next_pixels->pitch
                + static_cast<std::size_t>(x) * 4;
            const auto output_offset =
                (static_cast<std::size_t>(y) * width + x) * 4;
            for (int channel = 0; channel < 3; ++channel) {
                pixels[output_offset + channel] =
                    static_cast<std::uint8_t>(
                        (previous[old_offset + channel] * (256 - alpha)
                         + next[new_offset + channel] * alpha)
                        / 256);
            }
            pixels[output_offset + 3] = 255;
        }
    }
    if (!SDL_UpdateTexture(
            transition.composite.get(), nullptr, pixels.data(), width * 4)) {
        throw std::runtime_error(SDL_GetError());
    }
    SDL_RenderTexture(
        renderer_, transition.composite.get(), nullptr, nullptr);
}

void Game::draw_geometric_transition(float progress)
{
    ensure_transition_target();
    auto& transition = *transition_;
    if (transition.next_pixels
        && !SDL_UpdateTexture(
            transition.composite.get(), nullptr,
            transition.next_pixels->pixels,
            transition.next_pixels->pitch)) {
        throw std::runtime_error(SDL_GetError());
    }
    const int rate = std::clamp(static_cast<int>(progress * 256.0f), 0, 256);
    auto draw_old = [&](SDL_FRect destination, float alpha = 1.0f) {
        SDL_SetTextureAlphaModFloat(transition.previous.get(), alpha);
        SDL_RenderTexture(
            renderer_, transition.previous.get(), nullptr, &destination);
    };
    const SDL_FRect full{0.0f, 0.0f, 800.0f, 600.0f};

    switch (transition.type) {
    case 11: {
        int zoom = 256 - rate * rate / 256;
        const float scale = (zoom + 256.0f) / 256.0f;
        SDL_FRect rectangle{
            400.0f - 400.0f * scale, 300.0f - 300.0f * scale,
            800.0f * scale, 600.0f * scale};
        SDL_RenderTexture(renderer_, transition.previous.get(), nullptr, nullptr);
        SDL_SetTextureAlphaModFloat(
            transition.composite.get(), (128.0f - zoom / 2.0f) / 128.0f);
        SDL_RenderTexture(
            renderer_, transition.composite.get(), nullptr, &rectangle);
        break;
    }
    case 12: {
        const int inverse = 256 - rate;
        const int eased = 256 - inverse * inverse / 256;
        const float scale = (eased * 2.0f + 256.0f) / 256.0f;
        SDL_FRect rectangle{
            400.0f - 400.0f * scale, 300.0f - 300.0f * scale,
            800.0f * scale, 600.0f * scale};
        draw_old(rectangle);
        SDL_SetTextureAlphaModFloat(
            transition.composite.get(), eased / 256.0f);
        SDL_RenderTexture(
            renderer_, transition.composite.get(), nullptr, nullptr);
        break;
    }
    case 13: {
        const int inverse = 256 - rate;
        const int eased = inverse * inverse / 256;
        const float scale = 0.5f + eased / 512.0f;
        SDL_FRect rectangle{
            400.0f - 400.0f * scale, 300.0f - 300.0f * scale,
            800.0f * scale, 600.0f * scale};
        draw_old(rectangle, eased / 256.0f);
        break;
    }
    case 14: {
        const int inverse = 256 - rate;
        const int zoom = -(inverse * inverse / 256) / 2;
        const float scale = (zoom + 256.0f) / 256.0f;
        SDL_FRect rectangle{
            400.0f - 400.0f * scale, 300.0f - 300.0f * scale,
            800.0f * scale, 600.0f * scale};
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);
        SDL_SetTextureAlphaModFloat(
            transition.composite.get(), (32.0f - inverse / 8.0f) / 32.0f);
        SDL_RenderTexture(
            renderer_, transition.composite.get(), nullptr, &rectangle);
        break;
    }
    case 15: {
        const float x = 800.0f * rate / 256.0f;
        SDL_FRect left{-x, 0.0f, 800.0f, 600.0f};
        SDL_FRect right{x, 0.0f, 800.0f, 600.0f};
        draw_old(left, 0.5f);
        draw_old(right, 0.5f);
        break;
    }
    case 16:
    case 17:
    case 18:
    case 19: {
        const int inverse = 256 - rate;
        const float x = 800.0f
            - 800.0f * inverse * inverse / (256.0f * 256.0f);
        const float y = 600.0f
            - 600.0f * inverse * inverse / (256.0f * 256.0f);
        SDL_FRect rectangle = full;
        if (transition.type == 16) rectangle.y = y;
        if (transition.type == 17) rectangle.y = -y;
        if (transition.type == 18) rectangle.x = -x;
        if (transition.type == 19) rectangle.x = x;
        draw_old(rectangle, 1.0f - rate / 256.0f);
        break;
    }
    case 20: {
        const int amplitude = std::min(255, rate);
        for (int y = 0; y < 600; ++y) {
            const float offset = amplitude
                * std::sin((y * 480.0f / 600.0f + rate * 2.0f)
                           * std::numbers::pi_v<float> / 128.0f)
                / 10.0f;
            const SDL_FRect source{
                std::max(0.0f, offset), static_cast<float>(y),
                800.0f - std::abs(offset), 1.0f};
            SDL_FRect destination{
                std::max(0.0f, -offset), static_cast<float>(y),
                source.w, 1.0f};
            SDL_RenderTexture(
                renderer_, transition.previous.get(), &source, &destination);
        }
        break;
    }
    case 22: {
        for (int y = 0; y < 600; ++y) {
            const float offset = rate
                * std::sin((y + rate * 2.0f)
                           * std::numbers::pi_v<float> / 128.0f)
                / 10.0f;
            const SDL_FRect source{
                0.0f, static_cast<float>(
                    std::clamp(y + static_cast<int>(offset), 0, 599)),
                800.0f, 1.0f};
            const SDL_FRect destination{
                0.0f, static_cast<float>(y), 800.0f, 1.0f};
            SDL_SetTextureAlphaModFloat(
                transition.previous.get(), 32.0f / 256.0f);
            SDL_RenderTexture(
                renderer_, transition.previous.get(), &source, &destination);
        }
        break;
    }
    case 23: {
        int tv = 150 - 150 * rate / 256;
        tv = 150 - tv * tv * tv / (150 * 150);
        const float h = std::max(0.0f, 600.0f - 600.0f * tv / 120.0f);
        SDL_FRect rectangle{0.0f, (600.0f - h) / 2.0f, 800.0f, h};
        draw_old(rectangle);
        break;
    }
    case 24: {
        const float roll = 128.0f * rate / 256.0f;
        const float angle = roll * 360.0f / 256.0f;
        const float scale = (roll + 256.0f) / 256.0f;
        SDL_FRect rectangle{
            400.0f - 400.0f * scale, 300.0f - 300.0f * scale,
            800.0f * scale, 600.0f * scale};
        SDL_SetTextureAlphaModFloat(
            transition.previous.get(),
            std::clamp((128.0f - roll) / 128.0f, 0.0f, 1.0f));
        SDL_RenderTextureRotated(
            renderer_, transition.previous.get(), nullptr, &rectangle,
            angle, nullptr, SDL_FLIP_NONE);
        break;
    }
    default:
        break;
    }
}

void Game::dump_transition_frame(float progress)
{
    if (!config_.dump_transition_frames || !transition_) {
        return;
    }
    const int frame_count = transition_->frames;
    const int frame = std::clamp(
        static_cast<int>(progress * frame_count), 0, frame_count);
    if (frame == transition_->last_dumped_frame) {
        return;
    }
    transition_->last_dumped_frame = frame;

    const auto directory = std::filesystem::path("debug/transitions")
        / std::format("{:06}_{}_{}", transition_->debug_id,
                      runtime_.script_name(), runtime_.vm_pc());
    std::filesystem::create_directories(directory);
    auto surface = capture_frame_pixels();
    const auto path = directory / std::format("frame_{:04}.bmp", frame);
    if (!SDL_SaveBMP(surface.get(), path.string().c_str())) {
        std::cerr << "transition dump: " << SDL_GetError() << '\n';
    }
    if (!transition_->debug_metadata_written) {
        transition_->debug_metadata_written = true;
        std::ofstream metadata(directory / "transition.txt");
        metadata << "script=" << runtime_.script_name() << '\n'
                 << "vm_pc=" << runtime_.vm_pc() << '\n'
                 << "type=" << transition_->type << '\n'
                 << "vague=" << transition_->vague << '\n'
                 << "frames=" << transition_->frames << '\n'
                 << "mask_width=" << transition_->mask_width << '\n'
                 << "mask_height=" << transition_->mask_height << '\n';
    }
}

void Game::draw_active_transition()
{
    if (!transition_) {
        return;
    }
    if (transition_->frames <= 0) {
        // Instant: there is no frame of the old screen left to show.
        return;
    }
    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - transition_->started);
    const float progress = std::clamp(
        static_cast<float>(
            elapsed.count() * 60.0 / transition_->frames),
        0.0f, 1.0f);
    if (transition_->type >= 0x80) {
        if (!draw_pattern_transition_gpu(progress)) {
            draw_pattern_transition(progress);
        }
    } else if (transition_->type == 0) {
        if (progress < 0.5f) {
            SDL_SetTextureAlphaModFloat(
                transition_->previous.get(), 1.0f);
            SDL_RenderTexture(
                renderer_, transition_->previous.get(), nullptr, nullptr);
        }
        const float midpoint = progress < 0.5f
            ? progress * 2.0f : (1.0f - progress) * 2.0f;
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(
            renderer_, 0, 0, 0,
            static_cast<Uint8>(
                std::clamp(midpoint, 0.0f, 1.0f) * 255.0f));
        SDL_RenderFillRect(renderer_, nullptr);
    } else if ((transition_->type >= 2 && transition_->type <= 10)
               || transition_->type == 21) {
        draw_pixel_transition(progress);
    } else if (transition_->type >= 11
               && transition_->type <= 24) {
        draw_geometric_transition(progress);
    } else {
        SDL_SetTextureAlphaModFloat(
            transition_->previous.get(), 1.0f - progress);
        SDL_RenderTexture(
            renderer_, transition_->previous.get(), nullptr, nullptr);
    }
    dump_transition_frame(progress);
}

void Game::draw_script_position()
{
    if (!config_.show_script_position || ui_mode_ != UiMode::game) {
        return;
    }
    const auto position = std::format(
        "{}:{}  line={}", runtime_.script_name(), runtime_.vm_pc(),
        current_line_key_.empty() ? "-" : current_line_key_);
    ImGui::SetNextWindowPos(
        ImVec2(8.0f, 8.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.75f);
    ImGui::Begin(
        "Script position", nullptr,
        ImGuiWindowFlags_NoDecoration
            | ImGuiWindowFlags_AlwaysAutoResize
            | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted(position.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy")) {
        SDL_SetClipboardText(position.c_str());
    }
    ImGui::End();
}

void Game::begin_background_fade(int red, int green, int blue, int frames)
{
    const int effective_frames = effect_frames(frames);
    background_fade_ = BackgroundFade{
        background_brightness_,
        {
            static_cast<float>(std::clamp(red, 0, 256)),
            static_cast<float>(std::clamp(green, 0, 256)),
            static_cast<float>(std::clamp(blue, 0, 256)),
        },
        std::chrono::steady_clock::now(),
        std::chrono::milliseconds(effective_frames * 1000 / 60),
    };
}

void Game::update_background_fade()
{
    if (!background_fade_) {
        return;
    }
    const auto elapsed = std::chrono::steady_clock::now()
        - background_fade_->started;
    const auto duration =
        std::chrono::duration<float>(background_fade_->duration).count();
    const float progress = duration > 0.0f
        ? std::clamp(
              std::chrono::duration<float>(elapsed).count() / duration,
              0.0f, 1.0f)
        : 1.0f;
    for (std::size_t i = 0; i < background_brightness_.size(); ++i) {
        background_brightness_[i] = background_fade_->from[i]
            + (background_fade_->to[i] - background_fade_->from[i])
                * progress;
    }
    if (progress >= 1.0f) {
        background_brightness_ = background_fade_->to;
        background_fade_.reset();
        advance();
    }
}

void Game::update_screen_flash()
{
    if (!screen_flash_) {
        return;
    }
    const int total_frames =
        screen_flash_->fade_in_frames + screen_flash_->fade_out_frames;
    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - screen_flash_->started).count();
    if (elapsed * 60.0 >= total_frames) {
        screen_flash_.reset();
        advance();
    }
}

float Game::screen_flash_alpha() const
{
    if (!screen_flash_) {
        return 0.0f;
    }
    const float frame = static_cast<float>(
        std::chrono::duration<double>(
            std::chrono::steady_clock::now()
            - screen_flash_->started).count() * 60.0);
    if (frame < screen_flash_->fade_in_frames) {
        return std::clamp(
            frame / screen_flash_->fade_in_frames, 0.0f, 1.0f);
    }
    return std::clamp(
        1.0f - (frame - screen_flash_->fade_in_frames)
            / screen_flash_->fade_out_frames,
        0.0f, 1.0f);
}

void Game::update_shake()
{
    if (!shake_ || shake_->frames == 0) {
        return;
    }
    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - shake_->started).count();
    if (elapsed * 60.0 >= shake_->frames) {
        shake_.reset();
        advance();
    }
}

Game::ShakeSample Game::shake_sample()
{
    ShakeSample result;
    if (!shake_) {
        return result;
    }
    const float frame = static_cast<float>(
        std::chrono::duration<double>(
            std::chrono::steady_clock::now() - shake_->started).count()
        * 60.0);
    const int frame_index = std::max(1, static_cast<int>(frame) + 1);
    const float decay = shake_->frames > 0
        ? std::clamp(
            1.0f - static_cast<float>(frame_index) / shake_->frames,
            0.0f, 1.0f)
        : 1.0f;
    const int phase = frame_index * shake_->swing / 8;
    const float cosine = std::cos(
        static_cast<float>(phase % 256)
        * 2.0f * std::numbers::pi_v<float> / 256.0f);
    float amount = static_cast<float>(shake_->pitch);
    if (shake_->type == 0 || shake_->type == 3
        || shake_->type == 6 || shake_->type == 15
        || shake_->type == 16) {
        amount *= cosine;
        amount *= decay;
    } else if (shake_->type == 1 || shake_->type == 4
               || shake_->type == 7) {
        amount *= (frame_index & 1) ? 1.0f : -1.0f;
    } else if (shake_->type == 9 || shake_->type == 10
               || shake_->type == 11) {
        while (shake_->sampled_frame < frame_index) {
            int direction = std::rand() % 8;
            while (direction == shake_->direction) {
                direction = std::rand() % 8;
            }
            shake_->direction = direction;
            ++shake_->sampled_frame;
        }
    } else if (shake_->type == 2) {
        const float root = std::sqrt(std::max(0, shake_->pitch));
        const float cycle = std::fmod(
            frame_index * root * 2.0f / std::max(1, shake_->frames),
            std::max(1.0f, root));
        result.scale = 1.0f + cycle * cycle / 256.0f;
    } else if (shake_->type == 12) {
        const float inverse = std::clamp(
            1.0f - static_cast<float>(frame_index)
                / std::max(1, shake_->frames),
            0.0f, 1.0f);
        const float eased = 1.0f - inverse * inverse;
        float turn = std::fmod(eased * shake_->pitch / 2.0f, 256.0f);
        if ((shake_->direction & 1) == 0) {
            turn = 256.0f - turn;
        }
        result.angle = turn * 360.0 / 256.0;
    } else if (shake_->type == 13) {
        const float turn = -cosine * shake_->pitch * decay;
        result.angle = turn * 360.0 / 256.0;
    } else if (shake_->type == 14) {
        static constexpr std::array<int, 4> steps{-1, 0, 1, 0};
        result.angle = steps[frame_index & 3]
            * shake_->pitch * 360.0 / 256.0;
    }

    if (result.x == 0.0f && result.y == 0.0f
        && result.scale == 1.0f && result.angle == 0.0) {
        const bool left = shake_->direction == 1
            || shake_->direction == 2 || shake_->direction == 3;
        const bool right = shake_->direction == 5
            || shake_->direction == 6 || shake_->direction == 7;
        const bool up = shake_->direction == 3
            || shake_->direction == 4 || shake_->direction == 5;
        const bool down = shake_->direction == 0
            || shake_->direction == 1 || shake_->direction == 7;
        result.x = left ? -amount : right ? amount : 0.0f;
        result.y = up ? -amount : down ? amount : 0.0f;
    }
    result.text_only = shake_->type == 3 || shake_->type == 4
        || shake_->type == 10;
    result.includes_text = shake_->type == 6 || shake_->type == 7
        || shake_->type == 11 || shake_->type == 16;
    return result;
}

Game::BackgroundView Game::current_background_view() const
{
    if (!background_scroll_) {
        return background_view_;
    }
    const float raw = std::clamp(
        static_cast<float>(std::chrono::duration<double>(
            std::chrono::steady_clock::now()
            - background_scroll_->started).count()
            * 60.0 / background_scroll_->frames),
        0.0f, 1.0f);
    const float progress = background_scroll_->easing == 1
        ? raw * raw
        : background_scroll_->easing == 2
            ? 1.0f - (1.0f - raw) * (1.0f - raw)
            : raw;
    if (background_scroll_->zoom) {
        const auto zoom_axis = [progress](
            float from_position, float from_size,
            float to_position, float to_size) {
            const float inverse_size =
                (1.0f - progress) / from_size + progress / to_size;
            const float size = 1.0f / inverse_size;
            const float position =
                ((1.0f - progress) * from_position / from_size
                 + progress * to_position / to_size)
                * size;
            return std::pair{position, size};
        };
        const auto [x, width] = zoom_axis(
            background_scroll_->from.x,
            background_scroll_->from.width,
            background_scroll_->to.x,
            background_scroll_->to.width);
        const auto [y, height] = zoom_axis(
            background_scroll_->from.y,
            background_scroll_->from.height,
            background_scroll_->to.y,
            background_scroll_->to.height);
        return {x, y, width, height};
    }
    const auto mix = [progress](float from, float to) {
        return from + (to - from) * progress;
    };
    return {
        mix(background_scroll_->from.x, background_scroll_->to.x),
        mix(background_scroll_->from.y, background_scroll_->to.y),
        mix(background_scroll_->from.width, background_scroll_->to.width),
        mix(background_scroll_->from.height, background_scroll_->to.height),
    };
}

void Game::update_background_scroll()
{
    if (!background_scroll_) {
        return;
    }
    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now()
        - background_scroll_->started).count();
    if (elapsed * 60.0 >= background_scroll_->frames) {
        background_view_ = background_scroll_->to;
        background_scroll_.reset();
        advance();
    }
}


}  // namespace th2app
