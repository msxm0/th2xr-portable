#include <functional>
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

void Game::reset_half_tone()
{
    half_tone_count_ = 0.0f;
    half_tone_fading_ = false;
    half_tone_armed_ = false;
}

void Game::raise_half_tone()
{

    // AVG_SetHalfTone().  From TONE_NODISP it copies the background and
    // starts the ramp with the copy still hidden; called again while the
    // ramp is running, or once it is shown, it goes straight to TONE_DISP
    // without copying again.
    const bool from_nothing =
        !half_tone_armed_ || (!half_tone_fading_ && half_tone_count_ <= 0.0f);
    half_tone_armed_ = true;
    if (from_nothing) {
        build_half_tone_background();
        half_tone_fading_ = false;
        half_tone_count_ = 0.0f;
        return;
    }
    half_tone_fading_ = false;
    half_tone_count_ = half_tone_steps;
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
    if (transition_) {
        // AVG_ControlBackChange drives the wipe through GRP_BACK and parks
        // the outgoing background in GRP_BACK+1, so the darkened copy has
        // nowhere to be drawn while one runs.
        half_tone_count_ = 0.0f;
        half_tone_fading_ = false;
        half_tone_armed_ = false;
        return;
    }
    if (!half_tone_armed_ || !message_visible_ || message_.empty()) {
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

float Game::half_tone_factor() const
{
    if (ui_mode_ != UiMode::game || transition_ || !half_tone_armed_
        || !message_visible_ || message_.empty()) {
        return 1.0f;
    }
    const int half_tone = std::clamp(
        config_.message_half_tone,
        th2::GameConfig::min_message_half_tone,
        th2::GameConfig::max_message_half_tone);
    const float ramp =
        std::clamp(half_tone_count_ / half_tone_steps, 0.0f, 1.0f);
    return 1.0f - (128.0f - static_cast<float>(half_tone)) / 128.0f * ramp;
}

bool Game::half_tone_settled() const
{
    return half_tone_factor() < 1.0f
        && half_tone_count_ >= half_tone_steps;
}

float Game::half_tone_target_factor() const
{
    const int half_tone = std::clamp(
        config_.message_half_tone,
        th2::GameConfig::min_message_half_tone,
        th2::GameConfig::max_message_half_tone);
    return static_cast<float>(half_tone) / 128.0f;
}

SDL_Texture* Game::half_tone_background() const
{
    return half_tone_background_.get();
}

void Game::build_half_tone_background()
{
    // DSP_CopyBmp2( BMP_BACKHALF, BMP_BACK, NULL, 256,
    //               BackStruct.r*Avg.half_tone/128, ... ) - the whole
    // darkness at once.  What ramps is the background's own brightness,
    // separately, until this copy takes over from it.
    if (!background_) {
        half_tone_background_.reset();
        return;
    }
    float width = 0.0f;
    float height = 0.0f;
    SDL_GetTextureSize(background_.get(), &width, &height);
    if (width <= 0.0f || height <= 0.0f) {
        half_tone_background_.reset();
        return;
    }
    float held_width = 0.0f;
    float held_height = 0.0f;
    if (half_tone_background_) {
        SDL_GetTextureSize(half_tone_background_.get(),
                           &held_width, &held_height);
    }
    if (!half_tone_background_ || held_width != width
        || held_height != height) {
        half_tone_background_.reset(SDL_CreateTexture(
            renderer_, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET,
            static_cast<int>(width), static_cast<int>(height)));
        if (!half_tone_background_) {
            return;
        }
        SDL_SetTextureBlendMode(
            half_tone_background_.get(), SDL_BLENDMODE_NONE);
    }
    // Built from raise_half_tone(), during event handling - the scale there
    // belongs to ImGui, not to the art.
    SDL_Texture* const previous_target = SDL_GetRenderTarget(renderer_);
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    SDL_GetRenderScale(renderer_, &scale_x, &scale_y);
    SDL_SetRenderTarget(renderer_, half_tone_background_.get());
    SDL_SetRenderScale(renderer_, 1.0f, 1.0f);
    const auto shade = static_cast<Uint8>(std::clamp(
        half_tone_target_factor() * 255.0f, 0.0f, 255.0f));
    // From BMP_BACK, not BMP_BACK2: the characters are already in it, which
    // is how they come to be darkened without anything darkening them.
    SDL_Texture* const plate =
        background_baked_ ? background_baked_.get() : background_.get();
    SDL_SetTextureColorMod(plate, shade, shade, shade);
    SDL_SetTextureBlendMode(plate, SDL_BLENDMODE_NONE);
    SDL_RenderTexture(renderer_, plate, nullptr, nullptr);
    SDL_SetTextureColorMod(plate, 255, 255, 255);
    SDL_SetTextureBlendMode(plate, SDL_BLENDMODE_BLEND);
    SDL_SetRenderScale(renderer_, scale_x, scale_y);
    SDL_SetRenderTarget(renderer_, previous_target);
}

Uint8 Game::apply_background_fade(SDL_Texture* texture, float extra) const
{
    // 128 is normal, below it darkens, above it brightens.  The darkening
    // half is a plain colour modulation; the brightening half has to be
    // added on afterwards, because modulation cannot exceed the source.
    const auto& fade = background_brightness_;
    float highest = 0.0f;
    for (const float channel : fade) {
        highest = std::max(highest, std::clamp(channel, 0.0f, 256.0f));
    }
    std::array<Uint8, 3> modulate{};
    Uint8 brighten = 0;
    for (std::size_t i = 0; i < fade.size(); ++i) {
        const float channel = std::clamp(fade[i], 0.0f, 256.0f);
        modulate[i] = static_cast<Uint8>(
            std::clamp(std::min(channel, 128.0f) * 255.0f / 128.0f * extra,
                       0.0f, 255.0f));
    }
    if (highest > 128.0f) {
        brighten = static_cast<Uint8>(
            std::clamp((highest - 128.0f) * 255.0f / 128.0f, 0.0f, 255.0f));
    }
    SDL_SetTextureColorMod(texture, modulate[0], modulate[1], modulate[2]);
    return brighten;
}

void Game::finish_background_fade(
    SDL_Texture* texture, Uint8 brighten, const SDL_FRect* source,
    const SDL_FRect* destination, double angle, SDL_FlipMode flip)
{
    if (brighten > 0) {
        // The same picture again, added on: additive keeps the object's own
        // shape, where a screen-blended rectangle over the destination would
        // light up everything transparent in it as well.
        const auto previous = SDL_BLENDMODE_BLEND;
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_ADD);
        SDL_SetTextureColorMod(texture, brighten, brighten, brighten);
        SDL_RenderTextureRotated(
            renderer_, texture, source, destination, angle, nullptr, flip);
        SDL_SetTextureBlendMode(texture, previous);
    }
    SDL_SetTextureColorMod(texture, 255, 255, 255);
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
    // Kept every frame, not only during a shake: the first frame of one has
    // to find the frame before it already there.  One 800x600 blit, entirely
    // on the GPU.
    //
    // The art layer only.  The engine's buffer holds the composited screen
    // including its text, but ours keeps text on a separate monitor-
    // resolution layer, and the uncovered region is at the screen edges
    // where the text is redrawn in place every frame regardless.
    capture_previous_frame(upscaler_->art_target());

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
    background_baked_.reset();
    background_baked_dirty_ = true;
    previous_frame_.reset();
    previous_frame_valid_ = false;
    half_tone_background_.reset();
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

void Game::ensure_previous_frame()
{
    // Sized from the art target rather than from 800x600: both upscalers
    // happen to author the scene at that size and magnify at present time,
    // so the two agree today - but a capture that did not match would
    // quietly resample the frame twice every frame, and that is not a
    // failure anyone would see coming.
    SDL_Texture* const art = upscaler_ ? upscaler_->art_target() : nullptr;
    float width = 800.0f;
    float height = 600.0f;
    if (art) {
        SDL_GetTextureSize(art, &width, &height);
    }
    if (previous_frame_) {
        float held_width = 0.0f;
        float held_height = 0.0f;
        SDL_GetTextureSize(previous_frame_.get(), &held_width, &held_height);
        if (held_width == width && held_height == height) {
            return;
        }
        previous_frame_valid_ = false;
    }
    previous_frame_.reset(SDL_CreateTexture(
        renderer_, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET,
        static_cast<int>(width), static_cast<int>(height)));
    if (previous_frame_) {
        SDL_SetTextureBlendMode(previous_frame_.get(), SDL_BLENDMODE_NONE);
        SDL_SetTextureScaleMode(previous_frame_.get(), SDL_SCALEMODE_NEAREST);
    }
    previous_frame_valid_ = false;
}

void Game::capture_previous_frame(SDL_Texture* art_target)
{
    ensure_previous_frame();
    if (!previous_frame_ || !art_target) {
        return;
    }
    // Called from present_frame(), where the scale is whatever the overlay
    // pass left behind - and a scaled copy would capture a magnified corner
    // of the frame rather than the frame.
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    SDL_GetRenderScale(renderer_, &scale_x, &scale_y);
    SDL_SetRenderTarget(renderer_, previous_frame_.get());
    SDL_SetRenderScale(renderer_, 1.0f, 1.0f);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
    SDL_RenderTexture(renderer_, art_target, nullptr, nullptr);
    SDL_SetRenderScale(renderer_, scale_x, scale_y);
    SDL_SetRenderTarget(renderer_, art_target);
    previous_frame_valid_ = true;
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

bool Game::copy_back_plate()
{
    // DSP_CopyBmp( BMP_BACK, BMP_BACK2 ): the clean plate back over the
    // baked one.  A bake cannot be undone in place, so this is how a
    // character comes out of the background again.
    if (!background_) {
        background_baked_.reset();
        publish_background_bitmaps();
        return false;
    }
    float width = 0.0f;
    float height = 0.0f;
    SDL_GetTextureSize(background_.get(), &width, &height);
    if (width <= 0.0f || height <= 0.0f) {
        background_baked_.reset();
        publish_background_bitmaps();
        return false;
    }
    float held_width = 0.0f;
    float held_height = 0.0f;
    if (background_baked_) {
        SDL_GetTextureSize(background_baked_.get(), &held_width, &held_height);
    }
    if (!background_baked_ || held_width != width || held_height != height) {
        background_baked_.reset(SDL_CreateTexture(
            renderer_, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET,
            static_cast<int>(width), static_cast<int>(height)));
        if (!background_baked_) {
            publish_background_bitmaps();
            return false;
        }
        SDL_SetTextureBlendMode(background_baked_.get(), SDL_BLENDMODE_NONE);
    }
    SDL_Texture* const previous_target = SDL_GetRenderTarget(renderer_);
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    SDL_GetRenderScale(renderer_, &scale_x, &scale_y);
    SDL_SetRenderTarget(renderer_, background_baked_.get());
    SDL_SetRenderScale(renderer_, 1.0f, 1.0f);
    SDL_SetTextureBlendMode(background_.get(), SDL_BLENDMODE_NONE);
    SDL_RenderTexture(renderer_, background_.get(), nullptr, nullptr);
    SDL_SetTextureBlendMode(background_.get(), SDL_BLENDMODE_BLEND);
    SDL_SetRenderScale(renderer_, scale_x, scale_y);
    SDL_SetRenderTarget(renderer_, previous_target);
    // The slots have to point at the plate before anything bakes into it.
    publish_background_bitmaps();
    return true;
}

void Game::rebuild_baked_background()
{
    // AVG_CopyBack(OFF) followed by AVG_SetBackChar: the clean plate, then
    // the settled characters composited back into it by layer and by slot.
    background_baked_dirty_ = false;
    if (!copy_back_plate()) {
        return;
    }
    chars().set_back_char(0, 0, 1);
    // AVG_SetBackChar() bakes, AVG_SetHalfTone() copies, in that order - so
    // BMP_BACKHALF is always of a plate that already has its characters.
    //
    // The engine gets that from the call sequence because AVG_ControlChar
    // bakes in the same update pass that runs the script.  Ours runs the
    // script first and renders after, so a copy taken while the script is
    // executing precedes the bake by a frame.  Re-copying here is the same
    // ordering expressed the only way this frame structure allows: the copy
    // is never older than the plate it is of.
    if (half_tone_armed_ && half_tone_background_) {
        build_half_tone_background();
    }
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
    // The types whose case in AVG_ControlShake transforms GRP_BACK itself,
    // rather than moving the text or the whole composited screen.
    // SHAKE_SIN_SET shares its case with SHAKE_SIN and so belongs here too -
    // it was in the character list but not this one, which left it moving
    // the characters while the background stood still.
    const bool shake_background = shake_
        && (shake_->type == 0 || shake_->type == 1
            || shake_->type == 2 || shake_->type == 9
            || shake_->type == 12 || shake_->type == 13
            || shake_->type == 14 || shake_->type == 15);
    // GRP_WORK: the cases that slide or roll the background park a black
    // PRM_FLAT rectangle at layer 0, below LAY_BACK, so the strip the
    // transform uncovers comes out black instead of showing the frame
    // before it.  SHAKE_ZOOM, SHAKE_ROLL_SIN and SHAKE_ROLL_2TI do not -
    // those three genuinely keep the previous frame in the corners.
    const bool shake_work_rect = shake_
        && (shake_->type == 0 || shake_->type == 1
            || shake_->type == 9 || shake_->type == 12
            || shake_->type == 15);
    // SetCharPosShake(x, y, ON) - only the SIN cases call it - sets
    // cut_mode 2, taking the characters out of the bitmap and moving them
    // itself, while GRP_BACK swaps to BMP_BACK2, the plate without them.
    // Every other shake leaves them baked, so they travel with the
    // background's source offset for free and nothing needs saying.
    const bool shake_characters = shake_
        && (shake_->type == 0 || shake_->type == 15);
    if (shake_characters) {
        chars().set_char_pos_shake(
            static_cast<int>(shake.x), static_cast<int>(shake.y), 1);
    }
    if (background_baked_dirty_) {
        rebuild_baked_background();
    }
    const bool shake_art = shake_
        && (shake_->type == 6 || shake_->type == 7
            || shake_->type == 11 || shake_->type == 16);
    if (shake_art) {
        ensure_shake_target();
        SDL_SetRenderTarget(renderer_, shake_target_.get());
    } else {
        SDL_SetRenderTarget(renderer_, art_target);
    }
    // The engine never clears: DSP_DrawGraph composites straight over the
    // framebuffer it drew last time, and GetGraph only restores a saved
    // image when DSP_GetDispBmp has armed it.  So whatever a transform
    // leaves uncovered keeps the previous frame, and that is visible in the
    // original.  Restoring it is the same cost as the clear it replaces and
    // covers every pixel, so there is nothing to clear first.
    //
    // Done for every frame rather than only for shakes, because that is what
    // the engine does - the background simply covers the screen the rest of
    // the time, which is why it gets away with it.
    if (previous_frame_valid_ && previous_frame_) {
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
        SDL_RenderTexture(renderer_, previous_frame_.get(), nullptr, nullptr);
    } else {
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);
    }
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
    // GRP_WORK, at layer 0:
    //     DSP_SetGraphPrim( GRP_WORK, PRM_FLAT, POL_RECT, 0, ON );
    //     DSP_SetGraphPosRect( GRP_WORK, 0, 0, DISP_X, DISP_Y );
    //     DSP_SetGraphFade( GRP_WORK, 0 );
    // A black rectangle under the background, for the shakes that move it.
    if (shake_work_rect) {
        display().set_graph_prim(
            th2::grp_work, th2::GraphType::flat, th2::Poly::rect, 0, true);
        display().set_graph_pos_rect(
            th2::grp_work, 0, 0, th2::display_width, th2::display_height);
        display().set_graph_fade(th2::grp_work, 0);
    } else {
        display().reset_graph(th2::grp_work);
    }
    // GRP_BACK at LAY_BACK, with the shake's transform on it, and the
    // darkened copy next door at LAY_BACK+2 with the same transform.
    setup_background_graphs(shake, shake_background, shake_characters);
    if (!background_ && bg_scene_ == 0) {
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        const SDL_FRect game_area{0.0f, 0.0f, 800.0f, 600.0f};
        SDL_RenderFillRect(renderer_, &game_area);
    }
    // DSP_DrawGraph: layers 0 to LAYER_MAX, every visible graph on each.
    // Everything in the art pass is a graph now - GRP_WORK at 0, GRP_BACK at
    // LAY_BACK, the script's overlays wherever SetBmpEx put them, GRP_BACK+1
    // at LAY_BACK+2 and the live characters at LAY_CHAR - so the ordering is
    // the layer numbers rather than a sequence of bands written out here.
    //
    // A wipe is the one thing that is not a graph yet.  It goes in at
    // LAY_BACK, where AVG_SetBack parks the outgoing snapshot: the
    // characters and overlays of the new moment draw over it rather than
    // fading in with it.
    display().draw(
        shake_art ? shake_target_.get() : art_target,
        [this](int layer) {
            if (layer == th2::lay_back) {
                draw_active_transition();
            }
        });
    if (ui_mode_ == UiMode::system_menu) {
        draw_system_menu();
    } else if (ui_mode_ == UiMode::save || ui_mode_ == UiMode::load) {
        draw_save_load();
    }
    if (shake_art) {
        SDL_SetRenderTarget(renderer_, art_target);
        SDL_FRect destination{
            shake.x + 400.0f * (1.0f - shake.scale),
            shake.y + 300.0f * (1.0f - shake.scale),
            800.0f * shake.scale, 600.0f * shake.scale};
        SDL_RenderTextureRotated(
            renderer_, shake_target_.get(), nullptr, &destination,
            shake.angle, nullptr, SDL_FLIP_NONE);
        // DSP_SetGraphGlobalPos shifts the whole composited screen, and
        // DSP_DrawGraph fills the strip it pulls away from with four black
        // rectangles - written with signed widths so the same four calls
        // cover an offset in any direction, two of them collapsing to
        // nothing each time:
        //
        //   (0, 0, x, DISP_Y)            (x, 0, DISP_X-x, y)
        //   (DISP_X+x, y, -x, DISP_Y-y)  (x, DISP_Y+y, DISP_X-x, -y)
        const float x = shake.x;
        const float y = shake.y;
        // DISP_X and DISP_Y, taken from the target rather than assumed.
        float span_x = 800.0f;
        float span_y = 600.0f;
        SDL_GetTextureSize(art_target, &span_x, &span_y);
        const std::array<SDL_FRect, 4> bands{{
            {0.0f, 0.0f, x, span_y},
            {x, 0.0f, span_x - x, y},
            {span_x + x, y, -x, span_y - y},
            {x, span_y + y, span_x - x, -y},
        }};
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        for (const auto& band : bands) {
            // A negative extent draws nothing, as it does in the original.
            if (band.w > 0.0f && band.h > 0.0f) {
                SDL_RenderFillRect(renderer_, &band);
            }
        }
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
        // The half tone is drawn with the art, above, so that it travels
        // with the background rather than with this text.
        // The width of the per-glyph fade, in the same units as
        // reveal_position: the original's alph2 = LIM(text_cnt-cnt2,0,16)*16.
        constexpr float fade_width = 16.0f;
        const auto visible = message_.visible();
        const auto reveal_start =
            std::min(text_reveal_start_, visible.size());
        const auto reveal_text = visible.substr(reveal_start);
        const auto reveal_character_count =
            utf8_character_count(reveal_text);
        const float fade_finished = static_cast<float>(reveal_character_count)
            + fade_width - 1.0f;
        float reveal_position = fade_finished;
        if (!text_fade_complete_ && config_.text_speed_ms > 0) {
            const auto elapsed =
                std::chrono::steady_clock::now() - text_reveal_started_;
            reveal_position =
                std::chrono::duration<float, std::milli>(elapsed).count()
                / config_.text_speed_ms;
            // NovelMessage.max: the original's counter stops here, and this
            // is what the click indicator and the wait for input key off.
            if (reveal_position >= reveal_character_count + 8.0f) {
                text_reveal_complete_ = true;
            }
            // The ramp runs on past that, because a glyph needs the full
            // sixteen counts of alph2 = LIM(text_cnt - cnt2, 0, 16) * 16 to
            // reach solid, and the last glyph only starts at count - 1.
            // Ending the fade at the counter's threshold instead made the
            // tail of every line jump from part-faded to solid.
            if (reveal_position >= fade_finished) {
                text_fade_complete_ = true;
                reveal_position = fade_finished;
            }
        }
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
            // Measured once for the line and reused for every glyph in it,
            // instead of asking for the width of a growing prefix twice per
            // glyph per frame.
            const auto& boundaries = font_.glyph_boundaries(line);
            std::size_t glyph_index_in_line = 0;
            while (glyph_offset < line.size()) {
                const auto glyph_bytes = utf8_prefix_bytes(
                    std::string_view(line).substr(glyph_offset), 1);
                const auto source_offset = line_start + glyph_offset;
                float glyph_alpha = 1.0f;
                if (!text_fade_complete_
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
                        ++glyph_index_in_line;
                        continue;
                    }
                    (void)glyph_end;
                    const float glyph_left =
                        x + boundaries[std::min(glyph_index_in_line,
                                                boundaries.size() - 1)];
                    const float glyph_right =
                        x + boundaries[std::min(glyph_index_in_line + 1,
                                                boundaries.size() - 1)];
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
                ++glyph_index_in_line;
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
