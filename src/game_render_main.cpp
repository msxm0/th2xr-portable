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

std::size_t Game::message_visible_lines() const
{
    const float height = message_bottom_y - message_text_y();
    return static_cast<std::size_t>(std::max(
        1, static_cast<int>(height / text_line_height())));
}

int Game::message_scroll_limit(std::size_t total_lines) const
{
    return std::max(
        0,
        static_cast<int>(total_lines)
            - static_cast<int>(message_visible_lines()));
}

namespace {

float scroll_thumb_height(std::size_t total_lines, std::size_t visible_lines,
                          float track_height)
{
    return std::max(
        24.0f,
        track_height * static_cast<float>(visible_lines)
            / static_cast<float>(std::max<std::size_t>(total_lines, 1)));
}

}  // namespace

void Game::draw_scrollbar(
    std::size_t total_lines, int scroll, bool dragging)
{
    const int limit = message_scroll_limit(total_lines);
    if (limit <= 0) {
        return;
    }
    const float top = message_text_y();
    const float height = message_bottom_y - top;
    const float thumb_height = scroll_thumb_height(
        total_lines, message_visible_lines(), height);
    const float thumb_y = top
        + (height - thumb_height) * static_cast<float>(scroll)
            / static_cast<float>(limit);

    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    const SDL_FRect track{message_scroll_x, top, message_scroll_width, height};
    SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 40);
    SDL_RenderFillRect(renderer_, &track);
    const SDL_FRect thumb{
        message_scroll_x, thumb_y, message_scroll_width, thumb_height};
    SDL_SetRenderDrawColor(renderer_, 255, 255, 255, dragging ? 220 : 150);
    SDL_RenderFillRect(renderer_, &thumb);
}

int Game::scroll_from_y(float y, std::size_t total_lines) const
{
    const int limit = message_scroll_limit(total_lines);
    if (limit <= 0) {
        return 0;
    }
    const float top = message_text_y();
    const float height = message_bottom_y - top;
    const float thumb_height = scroll_thumb_height(
        total_lines, message_visible_lines(), height);
    const float travel = std::max(1.0f, height - thumb_height);
    const float position = std::clamp(
        y - top - thumb_height / 2.0f, 0.0f, travel);
    return static_cast<int>(std::lround(
        position / travel * static_cast<float>(limit)));
}

bool Game::on_scrollbar(float x, float y) const
{
    return x >= message_scroll_x
        && x < message_scroll_x + message_scroll_width
        && y >= message_text_y() && y < message_bottom_y;
}

void Game::set_message_scroll_from_y(float y)
{
    message_scroll_follow_ = false;
    message_scroll_ =
        scroll_from_y(y, display_lines(message_.visible()).size());
}

Uint8 Game::message_backdrop_alpha() const
{
    // The original multiplies the background by half_tone/128 while the
    // message window is up - in the log too, which shows its text in the
    // same window over the same darkened background - and compositing black
    // at the complementary alpha is the same thing.  It never reaches solid
    // because half_tone stops at 36.
    const int half_tone = std::clamp(
        config_.message_half_tone,
        th2::GameConfig::min_message_half_tone,
        th2::GameConfig::max_message_half_tone);
    // AVG_ControlHalfTone() walks the background from its own brightness to
    // the dimmed one over sixteen steps when the window comes up, so the
    // wash arrives with the text rather than snapping in behind it.
    const float ramp = std::clamp(half_tone_count_ / half_tone_steps, 0.0f, 1.0f);
    return static_cast<Uint8>(255.0f * (128 - half_tone) / 128.0f * ramp);
}

float Game::half_tone_pulse() const
{
    // AVG_EffCntPuls(): how far the sixteen-step counter moves each frame.
    // The original computes k*30/Avg.frame at 60 fps, so the slowest setting
    // truncates to zero and is clamped back up to one.
    switch (std::clamp(config_.effect_speed, 0, 4)) {
    case 0:
        return half_tone_steps;  // effects off: no ramp at all
    case 1:
        return 2.0f;             // eight frames
    default:
        return 1.0f;             // sixteen frames
    }
}

void Game::raise_half_tone()
{
    // AVG_SetHalfTone(): from nothing it starts the ramp, but called again
    // while the ramp is running it goes straight to the dimmed state.
    if (half_tone_fading_) {
        half_tone_fading_ = false;
        half_tone_count_ = half_tone_steps;
    }
}

void Game::update_half_tone()
{
    const auto now = std::chrono::steady_clock::now();
    const float frames = std::clamp(
        static_cast<float>(
            std::chrono::duration<double>(now - half_tone_updated_).count()
            * 60.0),
        0.0f, 8.0f);
    half_tone_updated_ = now;
    if (!message_visible_ || message_.empty()) {
        // AVG_ResetHalfTone() drops it in one go; the fade back in is
        // commented out in the original and never runs.
        half_tone_count_ = 0.0f;
        half_tone_fading_ = false;
        return;
    }
    if (half_tone_count_ >= half_tone_steps) {
        return;
    }
    if (!half_tone_fading_) {
        half_tone_fading_ = true;
        half_tone_count_ = 0.0f;
        return;
    }
    half_tone_count_ =
        std::min(half_tone_steps, half_tone_count_ + half_tone_pulse() * frames);
    if (half_tone_count_ >= half_tone_steps) {
        half_tone_fading_ = false;
    }
}

bool Game::handle_message_scroll_press(float x, float y)
{
    if (ui_mode_ != UiMode::game || !message_visible_ || message_.empty()
        || !on_scrollbar(x, y)
        || message_scroll_limit(
               display_lines(message_.visible()).size()) <= 0) {
        return false;
    }
    message_scroll_dragging_ = true;
    set_message_scroll_from_y(y);
    return true;
}

void Game::set_backlog_scroll_from_y(float y)
{
    backlog_scroll_ = scroll_from_y(y, backlog_view_lines().size());
}

bool Game::handle_backlog_scroll_press(float x, float y)
{
    if (!on_scrollbar(x, y)
        || message_scroll_limit(backlog_view_lines().size()) <= 0) {
        return false;
    }
    backlog_scroll_dragging_ = true;
    set_backlog_scroll_from_y(y);
    return true;
}

void Game::draw_click_indicator()
{
    if (!waiting_for_input_ || !message_visible_ || message_.empty()
        || !text_reveal_complete_) {
        return;
    }

    const bool end_of_block =
        !message_.has_hidden_segments() && message_ends_block_;
    auto& tex = end_of_block ? ui_keywait_ : ui_pageend_;
    if (!tex) return;

    const auto lines = display_lines(message_.visible());
    if (lines.empty()) return;

    // Sits on the row the last line actually occupies, which is not the last
    // row of the message once the page is scrolled.  If that line is out of
    // the visible window there is nothing to point at.
    const int row = static_cast<int>(lines.size()) - 1 - message_scroll_;
    if (row < 0 || row >= static_cast<int>(message_visible_lines())) {
        return;
    }

    // Fixed size, placed from the text baseline so it keeps sitting on the
    // line as the font grows instead of hanging from the top of the row.
    // The bitmap font reports no metrics, and keeps the original's offset.
    constexpr float size = 36.0f;
    const float row_top = message_text_y()
        + static_cast<float>(row) * text_line_height();
    const float ascent = font_.ascent();
    const float width = font_.text_width(lines.back());
    const float x = message_text_x() + width + 4.0f;
    const float y = ascent > 0.0f
        ? row_top + ascent - 5.0f - size / 2.0f
        : row_top - 2.0f;

    // Time-based 30fps animation matching original GlobalCount/2%30 (1s cycle)
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const int frame = (ms / 33) % 30;
    const SDL_FRect src{frame * 40.0f, 0.0f, 40.0f, 40.0f};
    const SDL_FRect dst{x, y, size, size};
    SDL_RenderTexture(renderer_, tex.get(), &src, &dst);
}

void Game::select_overlay()
{
    auto* overlay = upscaler_->overlay_target();
    SDL_SetRenderTarget(renderer_, overlay);
    float width = 0.0f;
    float height = 0.0f;
    SDL_GetTextureSize(overlay, &width, &height);
    SDL_SetRenderScale(renderer_, width / 800.0f, height / 600.0f);
}

void Game::begin_overlay()
{
    select_overlay();
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
    SDL_RenderClear(renderer_);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
}

void Game::select_sidebar()
{
    auto* sidebar = upscaler_->sidebar_target();
    SDL_SetRenderTarget(renderer_, sidebar);
    float width = 0.0f;
    float height = 0.0f;
    SDL_GetTextureSize(sidebar, &width, &height);
    SDL_SetRenderScale(renderer_, width / 800.0f, height / 600.0f);
}

void Game::clear_sidebar()
{
    select_sidebar();
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
    SDL_RenderClear(renderer_);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
}

void Game::clear_authentic_text()
{
    SDL_SetRenderTarget(renderer_, upscaler_->authentic_text_target());
    SDL_SetRenderScale(renderer_, 1.0f, 1.0f);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
    SDL_RenderClear(renderer_);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
}

void Game::begin_authentic_text()
{
    SDL_SetRenderTarget(renderer_, upscaler_->authentic_text_target());
    SDL_SetRenderScale(renderer_, 1.0f, 1.0f);
}

void Game::draw_overlay(std::size_t slot)
{
    if (!overlays_[slot] || !overlay_states_[slot].visible) {
        return;
    }
    const auto& state = overlay_states_[slot];
    auto* texture = overlays_[slot].get();
    const int alpha = state.parameter == 11
        ? std::clamp(state.parameter_value, 0, 256) * 255 / 256
        : 255;
    SDL_SetTextureAlphaMod(texture, static_cast<Uint8>(alpha));
    if (state.parameter == 1) {
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_ADD);
    } else if (state.parameter == 5) {
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_MOD);
    } else {
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    }

    const auto scale_x = [](int value) {
        return value * 800.0f / 640.0f;
    };
    const auto scale_y = [](int value) {
        return value * 600.0f / 448.0f;
    };
    SDL_FRect source{
        scale_x(state.source_x), scale_y(state.source_y),
        scale_x(state.source_width), scale_y(state.source_height)};
    SDL_FRect destination{
        scale_x(state.destination_x), scale_y(state.destination_y),
        scale_x(state.destination_width),
        scale_y(state.destination_height)};
    if (state.zoom != 0) {
        const float scale = (256.0f + state.zoom) / 256.0f;
        const float center_x = scale_x(state.zoom_center_x);
        const float center_y = scale_y(state.zoom_center_y);
        destination.x = center_x + (destination.x - center_x) * scale;
        destination.y = center_y + (destination.y - center_y) * scale;
        destination.w *= scale;
        destination.h *= scale;
    }
    SDL_FlipMode flip = SDL_FLIP_NONE;
    if ((state.reverse & 0x10) != 0) {
        flip = static_cast<SDL_FlipMode>(flip | SDL_FLIP_HORIZONTAL);
    }
    if ((state.reverse & 0x20) != 0) {
        flip = static_cast<SDL_FlipMode>(flip | SDL_FLIP_VERTICAL);
    }
    SDL_RenderTextureRotated(
        renderer_, texture, &source, &destination, 0.0, nullptr, flip);
}

float Game::imgui_display_scale() const
{
    // Cap the scale so ImGui doesn't become enormous on high-DPI phones.
    // 2.5x is plenty for crisp text on 5K+ desktop displays and keeps
    // phone UIs usable.
    return std::clamp(SDL_GetWindowDisplayScale(window_), 1.0f, 2.5f);
}

void Game::present_frame()
{
    upscaler_->present();

    // ImGui is rendered directly to the window backbuffer using a capped
    // display scale, so the debug/config UI stays crisp but does not
    // balloon to the full screen magnification used for the 800x600 art.
    const float display_scale = imgui_display_scale();
    SDL_SetRenderTarget(renderer_, nullptr);
    SDL_SetRenderScale(renderer_, display_scale, display_scale);
    imgui_->render();

    SDL_RenderPresent(renderer_);
}

void Game::reset_render_state()
{
    SDL_Log("Resetting render state after resume/reset");
    if (upscaler_) {
        upscaler_->reset();
    }
    shake_target_.reset();
    title_masked_.reset();
    if (imgui_) {
        imgui_->rebuild_font_atlas(imgui_display_scale());
    }
}

bool Game::draw_pose_dissolve(
    SDL_Texture* next, SDL_Texture* previous, float progress,
    Uint8 brightness, int alpha, const SDL_FRect& destination)
{
    if (!ensure_pose_blend_target()) {
        return false;
    }
    // Both poses are added in weighted by their share of the dissolve, with
    // the alpha channel weighted the same way, which is what makes the two
    // of them add up to one solid sprite.
    const auto accumulate = SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_SRC_ALPHA, SDL_BLENDFACTOR_ONE,
        SDL_BLENDOPERATION_ADD,
        SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE,
        SDL_BLENDOPERATION_ADD);
    if (!SDL_SetTextureBlendMode(next, accumulate)
        || !SDL_SetTextureBlendMode(previous, accumulate)) {
        SDL_SetTextureBlendMode(next, SDL_BLENDMODE_BLEND);
        SDL_SetTextureBlendMode(previous, SDL_BLENDMODE_BLEND);
        return false;  // No custom blending here; the plain path still works.
    }
    auto* const scene = SDL_GetRenderTarget(renderer_);
    SDL_SetRenderTarget(renderer_, pose_blend_target_.get());
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
    SDL_RenderClear(renderer_);
    const SDL_FRect whole{0.0f, 0.0f, 800.0f, 600.0f};
    for (const auto& [texture, weight] : {
             std::pair{previous, 1.0f - progress},
             std::pair{next, progress}}) {
        SDL_SetTextureColorMod(texture, 255, 255, 255);
        SDL_SetTextureAlphaMod(
            texture,
            static_cast<Uint8>(std::clamp(weight * 255.0f, 0.0f, 255.0f)));
        SDL_RenderTexture(renderer_, texture, nullptr, &whole);
    }
    SDL_SetRenderTarget(renderer_, scene);
    SDL_SetTextureBlendMode(next, SDL_BLENDMODE_BLEND);
    SDL_SetTextureBlendMode(previous, SDL_BLENDMODE_BLEND);
    SDL_SetTextureColorMod(
        pose_blend_target_.get(), brightness, brightness, brightness);
    SDL_SetTextureAlphaMod(
        pose_blend_target_.get(),
        static_cast<Uint8>(std::clamp(alpha, 0, 255)));
    SDL_RenderTexture(
        renderer_, pose_blend_target_.get(), nullptr, &destination);
    return true;
}

bool Game::ensure_pose_blend_target()
{
    if (pose_blend_target_) {
        return true;
    }
    pose_blend_target_.reset(SDL_CreateTexture(
        renderer_, SDL_PIXELFORMAT_RGBA32,
        SDL_TEXTUREACCESS_TARGET, 800, 600));
    if (!pose_blend_target_) {
        return false;
    }
    // The dissolve accumulates both poses premultiplied, so the result is
    // composited as premultiplied too.
    return SDL_SetTextureBlendMode(
        pose_blend_target_.get(), SDL_BLENDMODE_BLEND_PREMULTIPLIED);
}

void Game::ensure_shake_target()
{
    if (shake_target_) {
        return;
    }
    shake_target_.reset(SDL_CreateTexture(
        renderer_, SDL_PIXELFORMAT_RGBA32,
        SDL_TEXTUREACCESS_TARGET, 800, 600));
    if (!shake_target_) {
        throw std::runtime_error(SDL_GetError());
    }
    SDL_SetTextureBlendMode(shake_target_.get(), SDL_BLENDMODE_NONE);
}

void Game::draw_frame()
{
    // present() composites these two layers over the art on every path, so
    // they have to start empty here rather than in the game-mode branch
    // below: otherwise the last message and sidebar stay on screen after
    // returning to the title, or during a movie or a gallery.
    clear_authentic_text();
    clear_sidebar();
    SDL_SetRenderScale(renderer_, 1.0f, 1.0f);
    SDL_Texture* art_target = upscaler_->art_target();
    const auto shake = shake_sample();
    const bool shake_background = shake_
        && (shake_->type == 0 || shake_->type == 1
            || shake_->type == 2 || shake_->type == 9
            || shake_->type == 12 || shake_->type == 13
            || shake_->type == 14);
    const bool shake_characters = shake_ && shake_->type == 0;
    const bool shake_art = shake_
        && (shake_->type == 6 || shake_->type == 7
            || shake_->type == 11 || shake_->type == 16);
    if (shake_art) {
        ensure_shake_target();
        SDL_SetRenderTarget(renderer_, shake_target_.get());
    } else {
        SDL_SetRenderTarget(renderer_, art_target);
    }
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
    if (movie_) {
        movie_->draw();
        begin_overlay();

        present_frame();
        return;
    }
    if (name_input_open_) {
        begin_overlay();

        present_frame();
        return;
    }
    if (ui_mode_ == UiMode::title) {
        draw_title();
        draw_active_transition();
        begin_overlay();
        present_frame();
        return;
    }
    if (ui_mode_ == UiMode::cg_gallery) {
        draw_cg_gallery();
        draw_active_transition();
        begin_overlay();
        present_frame();
        return;
    }
    if (ui_mode_ == UiMode::music_room) {
        draw_music_room();
        draw_active_transition();
        begin_overlay();
        present_frame();
        return;
    }
    if (ui_mode_ == UiMode::replay_gallery) {
        draw_replay_gallery();
        draw_active_transition();
        begin_overlay();
        present_frame();
        return;
    }
    if (ui_mode_ == UiMode::map) {
        draw_map(false);
        begin_overlay();
        draw_map(true);
        draw_script_position();

        present_frame();
        return;
    }
    for (std::size_t i = 0; i < overlays_.size(); ++i) {
        if (overlay_states_[i].layer < 1) {
            draw_overlay(i);
        }
    }
    if (background_) {
        const auto view = current_background_view();
        SDL_FRect source{
            view.x, view.y, view.width, view.height};
        SDL_FRect destination{0.0f, 0.0f, 800.0f, 600.0f};
        double angle = 0.0;
        if (shake_background) {
            destination = {
                -shake.x + 400.0f * (1.0f - shake.scale),
                -shake.y + 300.0f * (1.0f - shake.scale),
                800.0f * shake.scale,
                600.0f * shake.scale,
            };
            angle = shake.angle;
        }
        if (clip_texture_source(
                background_.get(), source, destination)) {
            SDL_RenderTextureRotated(
                renderer_, background_.get(), &source, &destination,
                angle, nullptr, SDL_FLIP_NONE);
        }
    } else if (bg_scene_ == 0) {
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        const SDL_FRect game_area{0.0f, 0.0f, 800.0f, 600.0f};
        SDL_RenderFillRect(renderer_, &game_area);
    }
    for (std::size_t i = 0; i < overlays_.size(); ++i) {
        if (overlay_states_[i].layer >= 1
            && overlay_states_[i].layer < 5) {
            draw_overlay(i);
        }
    }
    if (background_brightness_ != std::array<float, 3>{
            128.0f, 128.0f, 128.0f}) {
        const SDL_FRect game_area{0.0f, 0.0f, 800.0f, 600.0f};
        std::array<Uint8, 3> multiply{};
        std::array<Uint8, 3> screen{};
        bool needs_multiply = false;
        bool needs_screen = false;
        for (std::size_t i = 0; i < background_brightness_.size(); ++i) {
            const float value =
                std::clamp(background_brightness_[i], 0.0f, 256.0f);
            multiply[i] = static_cast<Uint8>(
                value < 128.0f ? value * 255.0f / 128.0f : 255.0f);
            screen[i] = static_cast<Uint8>(
                value > 128.0f
                    ? (value - 128.0f) * 255.0f / 128.0f
                    : 0.0f);
            needs_multiply |= value < 128.0f;
            needs_screen |= value > 128.0f;
        }
        if (needs_multiply) {
            SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_MOD);
            SDL_SetRenderDrawColor(
                renderer_, multiply[0], multiply[1], multiply[2], 255);
            SDL_RenderFillRect(renderer_, &game_area);
        }
        if (needs_screen) {
            const auto screen_blend = SDL_ComposeCustomBlendMode(
                SDL_BLENDFACTOR_ONE,
                SDL_BLENDFACTOR_ONE_MINUS_SRC_COLOR,
                SDL_BLENDOPERATION_ADD,
                SDL_BLENDFACTOR_ZERO,
                SDL_BLENDFACTOR_ONE,
                SDL_BLENDOPERATION_ADD);
            SDL_SetRenderDrawBlendMode(renderer_, screen_blend);
            SDL_SetRenderDrawColor(
                renderer_, screen[0], screen[1], screen[2], 0);
            SDL_RenderFillRect(renderer_, &game_area);
        }
    }
    for (std::size_t i = 0; i < overlays_.size(); ++i) {
        if (overlay_states_[i].layer >= 5
            && overlay_states_[i].layer < 18) {
            draw_overlay(i);
        }
    }
    for (const auto& character : characters_.ordered()) {
        if (character_staged_.at(character.number)) {
            continue;
        }
        auto& loaded = character_texture(character.number);
        if (!loaded.texture) {
            continue;
        }
        auto& animation = character_animations_.at(character.number);
        float progress = 1.0f;
        if (animation.kind != CharacterAnimationKind::none) {
            progress = std::clamp(
                static_cast<float>(std::chrono::duration<double>(
                    std::chrono::steady_clock::now()
                    - animation.started).count() * 60.0
                    / animation.frames),
                0.0f, 1.0f);
        }
        const float eased = 1.0f
            - (1.0f - progress) * (1.0f - progress) * (1.0f - progress);
        float x = static_cast<float>(
            th2::character_offset(character.locate));
        int brightness_value = character.brightness;
        int alpha_value = character.alpha;
        if (animation.kind == CharacterAnimationKind::enter) {
            if (animation.type == 1 || animation.type == 2) {
                const float start = animation.type == 1 ? -600.0f : 600.0f;
                x = start + (x - start) * eased;
            } else {
                alpha_value = static_cast<int>(
                    animation.to_alpha * progress);
            }
        } else if (animation.kind == CharacterAnimationKind::leave) {
            if (animation.type == 1 || animation.type == 2) {
                const float destination =
                    animation.type == 1 ? -600.0f : 600.0f;
                // CHAR_COND_OUT cubes the remaining count, not the elapsed
                // one: the walk off screen starts fast and eases into the
                // wings, the mirror of the entrance.  Cubing progress
                // instead left the character standing still for most of the
                // animation and then snapping away.
                const float remaining = 1.0f - eased;  // (1 - progress)^3
                x = destination + (x - destination) * remaining;
            } else {
                alpha_value = static_cast<int>(
                    animation.from_alpha * (1.0f - progress));
            }
        } else if (animation.kind == CharacterAnimationKind::locate) {
            const float from = static_cast<float>(
                th2::character_offset(animation.from_locate));
            const float to = static_cast<float>(
                th2::character_offset(animation.to_locate));
            x = from + (to - from) * eased;
        } else if (animation.kind
                   == CharacterAnimationKind::brightness) {
            brightness_value = static_cast<int>(
                animation.from_brightness
                + (animation.to_brightness
                   - animation.from_brightness) * progress);
        } else if (animation.kind == CharacterAnimationKind::alpha) {
            alpha_value = static_cast<int>(
                animation.from_alpha
                + (animation.to_alpha - animation.from_alpha) * progress);
        } else if (animation.kind == CharacterAnimationKind::pose) {
            alpha_value = static_cast<int>(
                animation.to_alpha * progress);
        }
        const auto brightness = static_cast<Uint8>(
            std::clamp(brightness_value * 2, 0, 255));
        SDL_SetTextureColorMod(
            loaded.texture.get(), brightness, brightness, brightness);
        SDL_SetTextureAlphaMod(
            loaded.texture.get(),
            static_cast<Uint8>(
                std::clamp(alpha_value, 0, 256) * 255 / 256));
        SDL_FRect destination{
            x + (shake_characters ? shake.x : 0.0f),
            shake_characters ? shake.y : 0.0f,
            800.0f, 600.0f};
        if (animation.kind == CharacterAnimationKind::pose
            && animation.previous) {
            // GM_AvgChar.cpp hangs the old pose off the same graphic as the
            // new one (DSP_SetGraphBSet) and slides a single blend factor
            // across the pair, so the character dissolves from one pose to
            // the other without ever becoming see-through.  Blending the two
            // in a scratch target reproduces that; drawing them one over the
            // other straight onto the scene would let the background show
            // through in the middle, which reads as the old pose leaving
            // before the new one arrives.
            if (draw_pose_dissolve(
                    loaded.texture.get(), animation.previous.get(), progress,
                    brightness,
                    std::clamp(animation.to_alpha, 0, 256) * 255 / 256,
                    destination)) {
                continue;
            }
            SDL_SetTextureColorMod(
                animation.previous.get(),
                brightness, brightness, brightness);
            SDL_SetTextureAlphaMod(
                animation.previous.get(),
                static_cast<Uint8>(
                    std::clamp(static_cast<int>(
                        animation.from_alpha * (1.0f - progress)),
                        0, 256) * 255 / 256));
            SDL_RenderTexture(
                renderer_, animation.previous.get(), nullptr, &destination);
        }
        SDL_RenderTexture(renderer_, loaded.texture.get(), nullptr, &destination);
    }
    for (std::size_t i = 0; i < overlays_.size(); ++i) {
        if (overlay_states_[i].layer >= 18) {
            draw_overlay(i);
        }
    }
    if (ui_mode_ == UiMode::system_menu) {
        draw_system_menu();
    } else if (ui_mode_ == UiMode::save || ui_mode_ == UiMode::load) {
        draw_save_load();
    }
    draw_active_transition();
    if (shake_art) {
        SDL_SetRenderTarget(renderer_, art_target);
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);
        SDL_FRect destination{
            shake.x + 400.0f * (1.0f - shake.scale),
            shake.y + 300.0f * (1.0f - shake.scale),
            800.0f * shake.scale, 600.0f * shake.scale};
        SDL_RenderTextureRotated(
            renderer_, shake_target_.get(), nullptr, &destination,
            shake.angle, nullptr, SDL_FLIP_NONE);
    }
    begin_overlay();
    if (clock_state_ || calendar_state_) {
        draw_clock_calendar();
        draw_script_position();

        present_frame();
        return;
    }
    if (shake_ && (shake.text_only || shake.includes_text)) {
        const SDL_Rect viewport{
            static_cast<int>(shake.x), static_cast<int>(shake.y),
            800, 600};
        SDL_SetRenderViewport(renderer_, &viewport);
    }
    if (ui_mode_ == UiMode::game
        && message_visible_ && !message_.empty()) {
        if (font_.authentic()) {
            begin_authentic_text();
        }
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, message_backdrop_alpha());
        SDL_RenderFillRect(renderer_, nullptr);
        const auto visible = message_.visible();
        const auto reveal_start =
            std::min(text_reveal_start_, visible.size());
        const auto reveal_text = visible.substr(reveal_start);
        const auto reveal_character_count =
            utf8_character_count(reveal_text);
        float reveal_position =
            static_cast<float>(reveal_character_count);
        if (!text_reveal_complete_ && config_.text_speed_ms > 0) {
            const auto elapsed =
                std::chrono::steady_clock::now() - text_reveal_started_;
            reveal_position =
                std::chrono::duration<float, std::milli>(elapsed).count()
                / config_.text_speed_ms;
            if (reveal_position >= reveal_character_count + 8.0f) {
                text_reveal_complete_ = true;
                reveal_position =
                    static_cast<float>(reveal_character_count);
            }
        }
        constexpr float fade_width = 16.0f;
        const float x = message_text_x();
        float y = message_text_y();
        std::size_t source_cursor = 0;
        const auto lines = display_lines(visible);
        // A page taller than the screen scrolls; it follows the newest text
        // unless the reader has dragged the bar away from the bottom.
        const int scroll_limit = message_scroll_limit(lines.size());
        if (message_scroll_follow_) {
            message_scroll_ = scroll_limit;
        } else {
            message_scroll_ = std::clamp(message_scroll_, 0, scroll_limit);
        }
        std::size_t line_index = 0;
        for (const auto& line : lines) {
            auto line_start = visible.find(line, source_cursor);
            if (line_start == std::string_view::npos) {
                line_start = source_cursor;
            }
            if (static_cast<int>(line_index++) < message_scroll_) {
                source_cursor = line_start + line.size();
                continue;
            }
            std::size_t glyph_offset = 0;
            float authentic_x = x;
            while (glyph_offset < line.size()) {
                const auto glyph_bytes = utf8_prefix_bytes(
                    std::string_view(line).substr(glyph_offset), 1);
                const auto source_offset = line_start + glyph_offset;
                float glyph_alpha = 1.0f;
                if (!text_reveal_complete_
                    && source_offset >= reveal_start) {
                    const auto glyph_index = utf8_character_count(
                        visible.substr(
                            reveal_start,
                            source_offset - reveal_start));
                    glyph_alpha = std::clamp(
                        reveal_position
                            - static_cast<float>(glyph_index),
                        0.0f, fade_width) / fade_width;
                }
                if (glyph_alpha > 0.0f) {
                    const auto glyph_end = glyph_offset + glyph_bytes;
                    const auto glyph = std::string_view(line).substr(
                        glyph_offset, glyph_bytes);
                    const auto alpha = static_cast<std::uint8_t>(
                        glyph_alpha * 255.0f);
                    if (font_.authentic()) {
                        font_.draw_authentic_shadow(
                            renderer_, authentic_x, y, glyph, alpha);
                        font_.draw(
                            renderer_, authentic_x, y, glyph,
                            255, 255, 255, alpha);
                        authentic_x += font_.text_width(glyph);
                        glyph_offset += glyph_bytes;
                        continue;
                    }
                    const auto prefix =
                        std::string_view(line).substr(0, glyph_offset);
                    const auto through_glyph =
                        std::string_view(line).substr(0, glyph_end);
                    const float glyph_left =
                        x + font_.text_width(prefix);
                    const float glyph_right =
                        x + font_.text_width(through_glyph);
                    const SDL_Rect clip{
                        static_cast<int>(std::floor(glyph_left)),
                        static_cast<int>(std::floor(y)),
                        std::max(
                            1, static_cast<int>(
                                std::ceil(glyph_right - glyph_left))),
                        // Tall enough for the glyph and its shadow: a fixed
                        // height clipped the bottom off large fonts.
                        static_cast<int>(
                            std::ceil(text_line_height())) + 4};
                    SDL_SetRenderClipRect(renderer_, &clip);
                    font_.draw(
                        renderer_, x + 2.0f, y + 2.0f,
                        line, 0, 0, 0, alpha);
                    font_.draw(
                        renderer_, x, y, line,
                        255, 255, 255, alpha);
                    SDL_SetRenderClipRect(renderer_, nullptr);
                }
                glyph_offset += glyph_bytes;
            }
            source_cursor = line_start + line.size();
            y += text_line_height();
            if (y > message_bottom_y) {
                break;
            }
        }
        if (font_.authentic()) {
            select_overlay();
        }
        draw_scrollbar(
            lines.size(), message_scroll_, message_scroll_dragging_);
    }
    if (ui_mode_ == UiMode::game
        && message_visible_ && choosing_ && !choices_.empty()) {
        float y = choice_y_start();
        for (int i = 0; i < static_cast<int>(choices_.size()); ++i) {
            const auto highlighted = i == choice_highlight_;
            int budget = choice_reveal_count(i);
            for (const auto& line : choice_lines(choices_[i], i)) {
                const auto count = static_cast<int>(
                    utf8_character_count(line));
                // The line keeps its slot whether or not it has arrived yet,
                // so the options do not slide up the screen as they type.
                if (budget > 0) {
                    const std::string_view shown = budget >= count
                        ? std::string_view(line)
                        : std::string_view(line).substr(
                            0, utf8_prefix_bytes(
                                line, static_cast<std::size_t>(budget)));
                    font_.draw(
                        renderer_, 34.0f, y + 2.0f, shown, 0, 0, 0);
                    font_.draw(
                        renderer_, 32.0f, y, shown,
                        highlighted ? 255 : 128,
                        highlighted ? 255 : 128,
                        highlighted ? 255 : 128);
                }
                budget -= count;
                y += text_line_height();
            }
        }
    }
    SDL_SetRenderViewport(renderer_, nullptr);
    if (ui_mode_ == UiMode::game) {
        draw_click_indicator();
    }
    if (ui_mode_ == UiMode::backlog) {
        draw_backlog();
    }
    select_sidebar();
    if ((ui_mode_ == UiMode::game || ui_mode_ == UiMode::backlog) && message_visible_) {
        draw_sidebar();
    }
    select_overlay();
    // The petals sit at LAY_WINDOW+10 in the original, above the characters
    // and the message window alike, so they are drawn after the text rather
    // than back with the scene.
    draw_sakura();
    if (screen_flash_) {
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(
            renderer_,
            static_cast<Uint8>(screen_flash_->red),
            static_cast<Uint8>(screen_flash_->green),
            static_cast<Uint8>(screen_flash_->blue),
            static_cast<Uint8>(screen_flash_alpha() * 255.0f));
        SDL_RenderFillRect(renderer_, nullptr);
    }
    draw_script_position();
    present_frame();
}


}  // namespace th2app
