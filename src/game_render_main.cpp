#include <zlib.h>
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
    // The wash is BMP_BACKHALF, a darkened copy of the plate drawn by
    // GRP_BACK+1, rather than black composited over the top.  What is left
    // here is the save and load screens, which darken behind their own
    // panels and are ours.
    const int half_tone = std::clamp(
        config_.message_half_tone,
        th2::GameConfig::min_message_half_tone,
        th2::GameConfig::max_message_half_tone);
    return static_cast<Uint8>(255.0f * (128 - half_tone) / 128.0f);
}

float Game::half_tone_pulse() const
{
    // AVG_EffCntPuls(): how far the sixteen-step counter moves each frame.
    // The original computes k*30/Avg.frame at 60 fps, so the slowest setting
    // truncates to zero and is clamped back up to one.
    // AVG_EffCntPuls opens with
    //     if( AVG_GetMesCut() ) ret = 9999;
    // so the wash arrives whole rather than ramping while skipping.
    if (message_cut()) {
        return half_tone_steps;
    }
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
    // AVG_ResetHalfTone(), in avg_msg.cpp.
    msg().reset_half_tone();
}

void Game::draw_engine_text(const th2::EngineText& text)
{
    // A TEXT_STRUCT the way DrawGraphText draws one: laid out in the message
    // box, every character at once (cnt and step are -1), and TXT_DrawText's
    // two passes - every shadow, then every glyph - in FCT[color].
    if (!text.flag || !text.disp || text.str.empty()) {
        return;
    }
    // text.cpp's FCT, the colours DSP_SetTextColor indexes - RGB32, which
    // is {b, g, r, a}: FCT[10], the log, is orange and FCT[11] a sea green.
    static constexpr std::array<std::array<std::uint8_t, 3>, 18> fct{{
        {255, 255, 255}, {192, 192, 192}, {80, 80, 80}, {0, 0, 0},
        {0, 0, 255}, {0, 255, 0}, {255, 0, 0}, {0, 255, 255},
        {255, 255, 0}, {255, 0, 255}, {0, 128, 255}, {128, 255, 0},
        {255, 0, 128}, {0, 255, 128}, {255, 128, 0}, {128, 0, 255},
        {223, 230, 172}, {255, 225, 197},
    }};
    const auto& rgb = fct[static_cast<std::size_t>(
        std::clamp(text.color, 0, static_cast<int>(fct.size()) - 1))];
    auto box = th2::message_text_box;
    box.sx = text.x;
    box.sy = text.y;
    const auto layout = th2::txt_count_text(text.str, box);
    if (font_.authentic()) {
        begin_authentic_text();
    }
    const std::string_view source = text.str;
    for (int pass = 0; pass < 2; ++pass) {
        for (std::size_t i = 0;
             i < layout.glyph_x.size() && i < layout.glyph_off.size(); ++i) {
            const auto begin = static_cast<std::size_t>(layout.glyph_off[i]);
            if (begin >= source.size()) {
                break;
            }
            const auto glyph =
                source.substr(begin, utf8_prefix_bytes(source.substr(begin), 1));
            const auto gx = static_cast<float>(layout.glyph_x[i]);
            const auto gy = static_cast<float>(layout.glyph_y[i]);
            if (pass == 0) {
                font_.draw_authentic_shadow(renderer_, gx, gy, glyph, 255, 256);
            } else {
                font_.draw(renderer_, gx, gy, glyph, rgb[2], rgb[1], rgb[0],
                           255, 256);
            }
        }
    }
    if (font_.authentic()) {
        select_overlay();
    }
}

void Game::raise_half_tone()
{
    // AVG_SetHalfTone(), in avg_msg.cpp.
    msg().set_half_tone();
}

void Game::update_half_tone()
{
    // AVG_ControlHalfTone() runs in AVG_System's chain, not here.  AVG_Main
    // calls it from AVG_GAME and AVG_CONFIG and from nowhere else, so the
    // save and load menus freeze the wash where it is rather than resetting
    // it - which is why opening the save menu used to take the wash away.
    if (ui_mode_ != UiMode::game && ui_mode_ != UiMode::system_menu
        && ui_mode_ != UiMode::backlog) {
        return;
    }
}

bool Game::handle_message_scroll_press(float x, float y)
{
    if (ui_mode_ != UiMode::game || !message_shown() || message_.empty()
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


// The click indicator, composited the way the engine composites GRP_KEYWAIT:
// as an ordinary graph through Draw32's BlendTable, not through SDL's
// rounding blend.  It is a 40x40 sakura petal with soft alpha edges sitting
// directly on the message plate, so every one of those edge pixels was a
// level or two bright - and because it lands just past the end of the text
// it looked for a long time like a glyph problem.
std::vector<std::size_t> Game::engine_glyph_map(std::string_view visible) const
{
    const auto& counted = msg().counted();
    const std::string_view raw = msg().raw();
    const std::size_t glyphs = counted.glyph_off.size();
    const auto glyph_text = [&](std::size_t k) {
        const auto off = static_cast<std::size_t>(counted.glyph_off[k]);
        if (off >= raw.size()) {
            return std::string_view{};
        }
        return raw.substr(off, utf8_prefix_bytes(raw.substr(off), 1));
    };
    std::vector<std::size_t> map(visible.size(), glyphs);
    std::size_t next = 0;
    for (std::size_t at = 0; at < visible.size();) {
        const auto bytes = std::max<std::size_t>(
            1, utf8_prefix_bytes(visible.substr(at), 1));
        const auto character = visible.substr(at, bytes);
        // The engine's next glyph, or one a little further on if the two
        // renderings disagree about a character or two in between.
        std::size_t match = glyphs;
        for (std::size_t k = next; k < std::min(glyphs, next + 4); ++k) {
            if (glyph_text(k) == character) {
                match = k;
                break;
            }
        }
        if (match < glyphs) {
            map[at] = match;
            next = match + 1;
        } else {
            // Not a glyph to the engine - a line break, say: it goes with
            // the next one that is.
            map[at] = next;
        }
        at += bytes;
    }
    return map;
}

bool Game::draw_keywait_sprite(
    SDL_Texture* texture, const SDL_FRect& source, const SDL_FRect& destination)
{
    auto* const exact = display_->gl_exact_blend();
    if (exact && exact->available()
        && exact->capture_destination(renderer_)
        && exact->draw(renderer_, texture, source, destination, false, false,
                       0, 256, th2::bright_neutral, th2::bright_neutral,
                       th2::bright_neutral)) {
        return true;
    }
    SDL_RenderTexture(renderer_, texture, &source, &destination);
    return true;
}

int Game::keywait_frame() const
{
    //     DSP_SetGraphSPos( GRP_KEYWAIT, KEYWAIT_SIZE*(GlobalCount/2%30), ...
    //
    // Thirty frames of a spinning petal, stepped every second tick - but the
    // count it reads is the one from *before* this tick.  MAIN_Loop runs
    //
    //     MAIN_SystemControl();   // AVG_ControlNovelMessage picks the frame
    //     GlobalCount++;
    //     MAIN_DrawControl();     // and only then draws it
    //
    // while ours takes the tick at the top of the frame, so at the moment
    // the frame is chosen we are one ahead.  Both engines agree on
    // GlobalCount itself - the state trace says so - and disagreed only on
    // which value the sprite was allowed to see.
    const int count = global_count_ > 0 ? global_count_ - 1 : 0;
    return (count / 2) % 30;
}

void Game::draw_click_indicator()
{
    // GRP_KEYWAIT.  AVG_ControlNovelMessage puts it up in MSG_WAIT and
    // MSG_STOP and takes it down everywhere else, so this only has to say
    // where it goes.
    if (!keywait_visible_ || !message_shown() || message_.empty()) {
        return;
    }
    const bool end_of_block = keywait_page_end_;
    auto& tex = end_of_block ? ui_keywait_ : ui_pageend_;
    if (!tex) return;

    if (msg().vanilla_layout()) {
        //     DSP_GetTextDispPos( TXT_WINDOW, &px, &py );
        //     DSP_SetGraphMove( GRP_KEYWAIT, px-2, py-2 );
        //
        // px/py are TXT_DrawTextEx's cursor after the revealed text, so the
        // mark sits in the cell the next character would have taken.  At
        // KEYWAIT_SIZE, undersized and placed from our own font metrics it
        // drifted away from the text.
        const auto& layout = msg().layout();
        const auto shown = std::min(msg().visible_glyphs(),
                                    layout.glyph_x.size());
        int cx = 0, cy = 0;
        if (!th2::txt_cursor_after(
                layout, th2::message_text_box, shown, &cx, &cy)) {
            return;
        }
        const auto px = static_cast<float>(cx);
        const auto py = static_cast<float>(cy);
        const int frame = keywait_frame();
        const SDL_FRect src{frame * 40.0f, 0.0f, 40.0f, 40.0f};
        const SDL_FRect dst{px - 2.0f, py - 2.0f, 40.0f, 40.0f};
        draw_keywait_sprite(tex.get(), src, dst);
        return;
    }

    // DSP_GetTextDispPos( TXT_WINDOW, &px, &py ): where the *next* character
    // would be drawn, which is the end of what has actually been revealed -
    // not the end of the string.  Taking it from the whole string left the
    // indicator floating past the text whenever the two disagreed.
    const std::string_view visible_text = message_.visible();
    const auto glyph_of = engine_glyph_map(visible_text);
    const auto shown_glyphs = msg().visible_glyphs();
    std::size_t revealed = 0;
    for (std::size_t at = 0; at < visible_text.size();) {
        const auto bytes = std::max<std::size_t>(
            1, utf8_prefix_bytes(visible_text.substr(at), 1));
        if (glyph_of[at] < shown_glyphs) {
            revealed = at + bytes;
        }
        at += bytes;
    }
    const auto lines = display_lines(visible_text.substr(0, revealed));
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

    //     DSP_SetGraphSPos( GRP_KEYWAIT,
    //                       KEYWAIT_SIZE*(GlobalCount/2%30), 0,
    //                       KEYWAIT_SIZE, KEYWAIT_SIZE );
    //
    // Thirty frames off a sprite sheet, stepped every second tick.  This was
    // a wall clock reading - the last one left in the drawing path - which
    // runs the petal at whatever rate the machine happens to manage rather
    // than at the engine's, and in a trace run made it land on a different
    // frame every time.
    const int frame = keywait_frame();
    const SDL_FRect src{frame * 40.0f, 0.0f, 40.0f, 40.0f};
    const SDL_FRect dst{x, y, size, size};
    draw_keywait_sprite(tex.get(), src, dst);
}

void Game::select_overlay()
{
    // In a trace run the overlay *is* the art layer.  The engine composites
    // its text into the same 800x600 buffer it draws the picture in, and the
    // whole point of a pixel comparison is to see what it drew - so ours has
    // to land in the same buffer at the same resolution, with the bitmap
    // font enable_trace() turns on.  Keeping it on its own high-resolution
    // layer is what made the harness blind to every text bug there is.
    if (trace_mode_) {
        SDL_SetRenderTarget(renderer_, upscaler_->art_target());
        SDL_SetRenderScale(renderer_, 1.0f, 1.0f);
        return;
    }
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
    if (trace_mode_) {
        // No clear: the target already holds this frame's picture, and the
        // text goes on top of it exactly as DSP_DrawGraph puts it there.
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        return;
    }
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
    SDL_RenderClear(renderer_);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
}

void Game::select_sidebar()
{
    // Same reasoning as select_overlay: in a trace the system bar belongs in
    // the art buffer, because that is where the engine draws it and that is
    // the buffer being compared.  On its own layer it was invisible to the
    // pixel harness - a 30x600 strip of difference at x 770 that looked like
    // a rendering bug and was only the comparison missing a layer.
    if (trace_mode_) {
        SDL_SetRenderTarget(renderer_, upscaler_->art_target());
        SDL_SetRenderScale(renderer_, 1.0f, 1.0f);
        return;
    }
    auto* sidebar = upscaler_->sidebar_target();
    SDL_SetRenderTarget(renderer_, sidebar);
    float width = 0.0f;
    float height = 0.0f;
    SDL_GetTextureSize(sidebar, &width, &height);
    SDL_SetRenderScale(renderer_, width / 800.0f, height / 600.0f);
}

void Game::clear_sidebar()
{
    // In a trace the bar is drawn into the art buffer, which must keep the
    // last frame like the engine's framebuffer; clearing it here was only
    // ever hidden by the previous-frame copy that used to follow.
    if (trace_mode_ || !sidebar_layer_used_) {
        return;     // nothing there to clear
    }
    sidebar_layer_used_ = false;
    select_sidebar();
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
    SDL_RenderClear(renderer_);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
}

void Game::clear_authentic_text()
{
    if (!text_layer_used_) {
        return;     // nothing there to clear
    }
    text_layer_used_ = false;
    SDL_SetRenderTarget(renderer_, upscaler_->authentic_text_target());
    SDL_SetRenderScale(renderer_, 1.0f, 1.0f);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
    SDL_RenderClear(renderer_);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
}

void Game::begin_authentic_text()
{
    // In a trace run the bitmap text goes into the art layer with everything
    // else, because that is where the engine puts it and the comparison is
    // of that buffer.  Its own target exists so the upscaler can treat crisp
    // bitmap glyphs differently from the upscaled picture, which is a choice
    // about presentation and not about what was drawn.
    if (trace_mode_) {
        SDL_SetRenderTarget(renderer_, upscaler_->art_target());
        SDL_SetRenderScale(renderer_, 1.0f, 1.0f);
        apply_text_shake();
        return;
    }
    text_layer_used_ = true;
    SDL_SetRenderTarget(renderer_, upscaler_->authentic_text_target());
    SDL_SetRenderScale(renderer_, 1.0f, 1.0f);
    apply_text_shake();
}

// DSP_SetTextMove( TXT_WINDOW, ShakeDx+x, ShakeDy+y ): the text shakes carry
// the message with them.  Kept as a member, not only as the viewport,
// because changing render target resets the viewport - and the message goes
// through begin_authentic_text, which changes target, so the offset was set
// and then thrown away before a single glyph was drawn.
void Game::apply_text_shake()
{
    if (text_shake_.x == 0 && text_shake_.y == 0) {
        return;
    }
    const SDL_Rect viewport{text_shake_.x, text_shake_.y, 800, 600};
    SDL_SetRenderViewport(renderer_, &viewport);
}

float Game::half_tone_factor() const
{
    // AVG_ControlHalfTone drives GRP_BACK's brightness directly now, so
    // there is no separate shade to fold in anywhere.
    return 1.0f;
}

bool Game::half_tone_settled() const
{
    // HalfTone.tstep == TONE_DISP: the ramp is over and BMP_BACKHALF is
    // what is on screen.
    return msg().check_half_tone_step() == th2::tone_disp;
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
    return display().bmp_texture(th2::bmp_backhalf);
}

void Game::build_half_tone_background()
{
    // AVG_SetHalfTone's TONE_NODISP branch:
    //     DSP_CopyBmp2( BMP_BACKHALF, BMP_BACK, NULL, 256,
    //                   BackStruct.r*Avg.half_tone/128, ... );
    // From BMP_BACK, not BMP_BACK2: the characters are already in it, which
    // is how they come to be darkened without anything darkening them.  The
    // whole darkness at once, because the copy is only ever shown once the
    // ramp has finished; what ramps is the background's own brightness.
    if (!display().bmp_flag(th2::bmp_back)) {
        display().release_bmp(th2::bmp_backhalf);
        return;
    }
    // BackStruct.r*Avg.half_tone/128 per channel, not the half tone alone:
    // the background's own fade is baked into the copy.  In practice it is
    // always neutral here, because AVG_SetBackFade tears the wash down
    // before it starts - but the copy is now a DSP_CopyBmp2 like the
    // engine's, so there is no reason to leave it out.
    const auto shade = [this](std::size_t channel) {
        return static_cast<int>(
            background_brightness_[channel] * half_tone_target_factor());
    };
    display().copy_bmp2(
        th2::bmp_backhalf, th2::bmp_back, shade(0), shade(1), shade(2));
}

float Game::imgui_display_scale() const
{
    // Cap the scale so ImGui doesn't become enormous on high-DPI phones.
    // 2.5x is plenty for crisp text on 5K+ desktop displays and keeps
    // phone UIs usable.
    return std::clamp(SDL_GetWindowDisplayScale(window_), 1.0f, 2.5f);
}

void Game::trace_dump_frame()
{
    // Temporary: TH2_DEBUG_TICK=N makes the draw paths report themselves for
    // that one tick, which is how you find out which of them owns a region
    // of the screen instead of guessing.
    if (const char* at = SDL_getenv("TH2_DEBUG_TICK")) {
        // A short range, not one tick: what a frame shows is often built
        // several ticks earlier, so the draw that is wrong is rarely in the
        // tick that displays it.
        const auto first = std::strtoull(at, nullptr, 10);
        const char* dash = std::strchr(at, '-');
        const auto last = dash ? std::strtoull(dash + 1, nullptr, 10) : first;
        th2::debug_draws = trace_tick_ >= first && trace_tick_ <= last;
        if (th2::debug_draws) {
            SDL_Log("-- tick %llu",
                    static_cast<unsigned long long>(trace_tick_));
        }
    }
    SDL_Texture* art = upscaler_->art_target();
    // trace_resume_pending_: the ticks a resumed run spends getting the
    // engine as far as the scenario are bookkeeping, not part of the run,
    // and they carry tick numbers from the title screen.
    if (!art || trace_dir_.empty() || trace_resume_pending_) {
        return;
    }
    // One CRC per frame, a picture only inside --trace-from's range.  Storing
    // every frame of a route is well over a hundred gigabytes a side; a
    // checksum is eight bytes, so the whole run can be compared and only the
    // neighbourhood of a mismatch needs real images.  The reference does the
    // same in th2ref_dump_frame, over the same tightly packed BGR24.
    static const long frame_step = [] {
        const char* v = SDL_getenv("TH2_FRAME_STEP");
        const long n = v ? std::strtol(v, nullptr, 10) : 1;
        return n < 1 ? 1 : n;
    }();
    // Sampled: the readback below is a GPU stall, so it happens only on the
    // ticks that are actually compared.  Both the picture and the checksum
    // come from the same sample, and the reference samples identically.
    if ((static_cast<long>(trace_tick_) % frame_step) != 0) {
        return;
    }
    const bool store_frame = trace_tick_ >= trace_first_tick_;
    const char* hash_path = SDL_getenv("TH2_FRAME_HASH");
    if (!store_frame && !hash_path) {
        return;
    }
    SDL_SetRenderTarget(renderer_, art);
    const SDL_Rect rect{0, 0, 800, 600};
    Surface pixels(SDL_RenderReadPixels(renderer_, &rect));
    SDL_SetRenderTarget(renderer_, nullptr);
    if (!pixels) {
        return;
    }
    Surface bgr(SDL_ConvertSurface(pixels.get(), SDL_PIXELFORMAT_BGR24));
    if (!bgr) {
        return;
    }
    // SDL pads rows to its own pitch; the dump is tightly packed.
    std::vector<std::uint8_t> packed(
        static_cast<std::size_t>(800) * 600 * 3);
    const auto* src = static_cast<const std::uint8_t*>(bgr->pixels);
    for (int y = 0; y < 600; ++y) {
        std::memcpy(packed.data() + static_cast<std::size_t>(y) * 800 * 3,
                    src + static_cast<std::size_t>(y) * bgr->pitch,
                    static_cast<std::size_t>(800) * 3);
    }
    if (hash_path) {
        static std::FILE* hash_file = std::fopen(hash_path, "a");
        if (hash_file) {
            const auto sum = crc32(
                crc32(0L, Z_NULL, 0), packed.data(),
                static_cast<uInt>(packed.size()));
            std::fprintf(hash_file, "%llu %08lx 800 600\n",
                         static_cast<unsigned long long>(trace_tick_),
                         static_cast<unsigned long>(sum));
            std::fflush(hash_file);
        }
    }
    if (store_frame) {
        char name[32];
        std::snprintf(name, sizeof name, "f%06llu.binz",
                      static_cast<unsigned long long>(trace_tick_));
        write_trace_frame(
            trace_dir_ / name, trace_tick_, packed.data(), 800, 600);
    }
}

void Game::present_frame()
{
    // The art layer is 800x600, which is exactly what the reference build
    // dumps out of MAIN_DrawGraph - so the two are directly comparable with
    // no scaling in between.  Text is deliberately left out: ours renders at
    // monitor resolution and theirs inside the plate, and the typewriter's
    // correctness is carried by NovelMessage.count rather than by glyphs.
    if (trace_mode_) {
        trace_dump_frame();
        // and then fall through and present like any other frame.
        //
        // Returning here instead looked free - nothing watches the window in
        // a trace run, and it saved about forty percent of a tick.  What it
        // actually did was stop SDL's GPU renderer ever flushing: without
        // the present there is no submit, command buffers and their staging
        // allocations pile up, and a long replay dies somewhere past a
        // thousand ticks with `execbuf ioctl keeps returning ENOMEM` and a
        // lost device.  Seconds saved per run against a ceiling on how far a
        // run can go is not a trade worth making.
    }

    upscaler_->set_layer_content(text_layer_used_, sidebar_layer_used_);
    upscaler_->present();

    // ImGui is rendered directly to the window backbuffer using a capped
    // display scale, so the debug/config UI stays crisp but does not
    // balloon to the full screen magnification used for the 800x600 art.
    const float display_scale = imgui_display_scale();
    SDL_SetRenderTarget(renderer_, nullptr);
    SDL_SetRenderScale(renderer_, display_scale, display_scale);
    imgui_->render();

#ifdef __EMSCRIPTEN__
    // SDL's Emscripten swap yields to the browser with emscripten_sleep(0)
    // on every present, "for screen refresh" - but this loop yields already,
    // at the requestAnimationFrame wait straight after drawing, so that was a
    // second hop per frame: a setTimeout task and a promise between one
    // frame's present and the next one's wait.  Off for the present only:
    // the same hint is what makes SDL_Delay sleep cooperatively, and off for
    // good it turned SDL's own waits - the audio device's shutdown among
    // them - into busy loops on the main thread.
    SDL_SetHint(SDL_HINT_EMSCRIPTEN_ASYNCIFY, "0");
#endif
    SDL_RenderPresent(renderer_);
#ifdef __EMSCRIPTEN__
    SDL_SetHint(SDL_HINT_EMSCRIPTEN_ASYNCIFY, "1");
#endif
}

void Game::reset_render_state()
{
    SDL_Log("Resetting render state after resume/reset");
    if (upscaler_) {
        upscaler_->reset();
    }
    shake_target_.reset();
    display().release_bmp(th2::bmp_back);
    background_baked_dirty_ = true;
    art_cleared_for_ = nullptr;
    // New targets: whatever was on them is gone, so they get a first clear.
    text_layer_used_ = true;
    sidebar_layer_used_ = true;
    display().release_bmp(th2::bmp_backhalf);
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

bool Game::copy_back_plate()
{
    // AVG_CopyBack(OFF): DSP_CopyBmp( BMP_BACK, BMP_BACK2 ).  The clean
    // plate back over the baked one, because a bake cannot be undone in
    // place - this is how a character comes out of the background again.
    if (!display().bmp_flag(th2::bmp_back2)) {
        display().release_bmp(th2::bmp_back);
        return false;
    }
    display().copy_bmp(th2::bmp_back, th2::bmp_back2);
    return display().bmp_flag(th2::bmp_back);
}

void Game::rebuild_baked_background()
{
    // AVG_CopyBack(OFF) followed by AVG_SetBackChar: the clean plate, then
    // the settled characters composited back into it by layer and by slot.
    background_baked_dirty_ = false;
    if (!copy_back_plate()) {
        return;
    }
    // AVG_SetBackChar( BackStruct.x, BackStruct.y, ON ) - the plate is the
    // background *bitmap*, and the background is shown through a window at
    // (x, y), so a character standing at screen X has to be composited at
    // bitmap X + x.  Passing zero here only looked right because the window
    // sits at the origin until something scrolls it.
    {
        const auto view = current_background_view();
        chars().set_back_char(
            static_cast<int>(view.x), static_cast<int>(view.y), 1);
    }
    // AVG_SetBackChar() bakes, AVG_SetHalfTone() copies, in that order - so
    // BMP_BACKHALF is always of a plate that already has its characters.
    //
    // The engine gets that from the call sequence because AVG_ControlChar
    // bakes in the same update pass that runs the script.  Ours runs the
    // script first and renders after, so a copy taken while the script is
    // executing precedes the bake by a frame.  Re-copying here is the same
    // ordering expressed the only way this frame structure allows: the copy
    // is never older than the plate it is of.
    if (half_tone_armed_ && display().bmp_flag(th2::bmp_backhalf)) {
        build_half_tone_background();
    }
    // Loaded with the wash up.  HalfTone comes back from the save but its
    // bitmap does not - BMP_BACKHALF is a copy of a plate that has only just
    // been rebuilt - so the step said "shown" with nothing to show, GRP_BACK
    // stayed hidden in its favour, and the screen went black under the text.
    // Taken now, from the plate with its characters in, as AVG_SetHalfTone
    // takes it; the graphs as that function leaves them for the step.
    //
    // Not under a calendar, though.  AVG_SetCalender's case 0 is
    // AVG_ResetBack( 0 ): every background graph reset and every bitmap from
    // BMP_BACK up released, while HalfTone.tstep is left saying "shown".
    // Rebuilt from a plate that is not there, the wash came back as a black
    // screen under the page - and the page fades in with DRW_BLD over
    // whatever the last frame left, so over black it came up at 3/4
    // brightness on its twelfth frame where the reference's was all but
    // solid.
    const int tone_step = msg().check_half_tone_step();
    if ((tone_step == th2::tone_disp || tone_step == th2::tone_fadeout)
        && !display().bmp_flag(th2::bmp_backhalf) && !calendar_state_) {
        build_half_tone_background();
        if (display().bmp_flag(th2::bmp_backhalf)) {
            const bool shown = tone_step == th2::tone_disp;
            display().set_graph(
                th2::grp_back + 1, th2::bmp_backhalf, th2::lay_back + 2,
                shown, th2::check_none);
            display().set_graph_pos(
                th2::grp_back + 1, 0, 0, back().x, back().y,
                th2::display_width, th2::display_height);
            display().set_graph_disp(th2::grp_back, !shown);
        }
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
    SDL_Texture* art_target = upscaler_->art_target();
    const auto shake = shake_sample();
    // The types whose case in AVG_ControlShake transforms GRP_BACK itself,
    // rather than moving the text or the whole composited screen.
    // SHAKE_SIN_SET shares its case with SHAKE_SIN and so belongs here too -
    // it was in the character list but not this one, which left it moving
    // the characters while the background stood still.
    const bool shake_background = back().sk_flag
        && (back().sk_type == 0 || back().sk_type == 1
            || back().sk_type == 2 || back().sk_type == 9
            || back().sk_type == 12 || back().sk_type == 13
            || back().sk_type == 14 || back().sk_type == 15);
    // GRP_WORK: the cases that slide or roll the background park a black
    // PRM_FLAT rectangle at layer 0, below LAY_BACK, so the strip the
    // transform uncovers comes out black instead of showing the frame
    // before it.  SHAKE_ZOOM, SHAKE_ROLL_SIN and SHAKE_ROLL_2TI do not -
    // those three genuinely keep the previous frame in the corners.
    const bool shake_work_rect = back().sk_flag
        && (back().sk_type == 0 || back().sk_type == 1
            || back().sk_type == 9 || back().sk_type == 12
            || back().sk_type == 15);
    // SetCharPosShake(x, y, ON) - only the SIN cases call it - sets
    // cut_mode 2, taking the characters out of the bitmap and moving them
    // itself, while GRP_BACK swaps to BMP_BACK2, the plate without them.
    // Every other shake leaves them baked, so they travel with the
    // background's source offset for free and nothing needs saying.
    const bool shake_characters = back().sk_flag
        && (back().sk_type == 0 || back().sk_type == 15);
    if (shake_characters) {
        chars().set_char_pos_shake(
            static_cast<int>(shake.x), static_cast<int>(shake.y), 1);
    }
    if (background_baked_dirty_) {
        rebuild_baked_background();
    }
    // SHAKE_ALL_* is DSP_SetGraphGlobalPos( x, y ) in AVG_ControlShake and
    // nothing else: DSP_DrawGraph shifts every graph by the global offset
    // and paints the four black bands it pulls away from.  AvgBack sets it,
    // Display honours it, and the separate compositing target this used to
    // need is gone with them.
    SDL_SetRenderTarget(renderer_, art_target);
    // After the switch, never before it: the scale is per-target state that
    // SDL restores here, so a reset written above would land on whichever
    // target clear_sidebar() left current and leave this one as it was.
    SDL_SetRenderScale(renderer_, 1.0f, 1.0f);
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
    //
    // The art target already is that framebuffer.  A render target keeps
    // its contents from one frame to the next, and nothing draws into it
    // between one present and the next frame's drawing - everything else
    // that touches it in between only reads - so it is simply drawn over.
    // Copying it out at present and back here, as this once did, was two
    // full-screen passes a frame for an identical picture: the largest GPU
    // cost of a frame, which a phone pays in fill rate.  Only a target that
    // has never been drawn is cleared, to the engine's black.
    if (art_cleared_for_ != art_target) {
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);
        art_cleared_for_ = art_target;
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
    // The map's step 1 is AVG_ViewClock( 19 ), and until it reports done
    // nothing of the map is up and the scene is not yet reset - that is
    // step 2's AVG_ResetBack( 0 ).  So the clock goes over the scene, by the
    // ordinary path below.  Drawn over the map's empty frame instead, each
    // frame's DRW_BLD landed on the last one's clock and the fade-out never
    // faded.
    if (ui_mode_ == UiMode::map && !clock_state_) {
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
    // Not under a calendar: AVG_SetCalender's AVG_ResetBack( 0 ) leaves no
    // background at all, and the engine draws nothing where there is none -
    // its framebuffer keeps the last frame, which is what the page's
    // DRW_BLD( count*16 ) fade-in builds up on.  Painted black here instead,
    // every frame of the fade was the page over black: 3/4 brightness on its
    // twelfth frame, where the reference's was all but solid.
    if (!display().bmp_flag(th2::bmp_back2) && bg_scene_ == 0
        && !calendar_state_) {
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
        art_target,
        [this](int layer) {
            // The wipes the display layer can express are already on screen
            // by now - the snapshot is a graph at LAY_BACK and the incoming
            // background one at LAY_BACK+1.  What is left here is the
            // pattern wipes, which need their mask, and the handful of
            // types no script uses.
            if (layer == th2::lay_back && !transition_drives_graphs()) {
                draw_active_transition();
            }
        });
    if (ui_mode_ == UiMode::system_menu) {
        draw_system_menu();
    } else if (ui_mode_ == UiMode::save || ui_mode_ == UiMode::load) {
        draw_save_load();
    }
    begin_overlay();
    if (clock_state_ || calendar_state_) {
        draw_clock_calendar();
        draw_script_position();

        present_frame();
        return;
    }
    text_shake_ = {};
    if (back().sk_flag && (shake.text_only || shake.includes_text)) {
        text_shake_ = {static_cast<int>(shake.x), static_cast<int>(shake.y)};
        apply_text_shake();
    }
    if (ui_mode_ == UiMode::game
        && message_shown() && !message_.empty()
        // DSP_SetTextDisp( TXT_WINDOW, ... ), which the engine's log turns
        // off while an older entry is up in its place.
        && (!msg().engine_bar() || msg().main_text_disp())) {
        // The engine's layout goes with the engine's font: the box, the
        // wrap and the overflow clip are all in units of SYS_FONT, so they
        // only mean anything at that size.
        msg().set_vanilla_layout(font_.authentic());
        if (font_.authentic()) {
            begin_authentic_text();
        }
        // The half tone is drawn with the art, above, so that it travels
        // with the background rather than with this text.
        // The typewriter is NovelMessage.count, advanced once per frame by
        // AVG_MsgCnt() in AVG_ControlNovelMessage, and each character fades
        // in over the sixteen counts after its own:
        //     alph2 = LIM( text_cnt - cnt2, 0, 16 ) * 16
        // AvgMsg::glyph_alpha is that line; nothing here measures a clock.
        const auto visible = message_.visible();
        const std::size_t shown = msg().visible_glyphs();

        // With the original bitmap font, the engine's own layout: every
        // character goes in the cell TXT_DrawTextEx put it in, wrapped where
        // the engine wraps and dropped where the engine drops it.  Our
        // wrapping and the scrolling window are for the outline font, which
        // is a different size and deliberately does better than vanilla.
        if (msg().vanilla_layout()) {
            const auto& layout = msg().layout();
            const auto limit = std::min(shown, layout.glyph_x.size());
            // Two passes, because TXT_DrawText is two calls:
            //
            //     TXT_DrawTextEx( ..., kage, ... );   // every shadow
            //     TXT_DrawTextEx( ..., 0,    ... );   // every glyph
            //
            // Every shadow in the line goes down before any glyph does.
            // Interleaved, a glyph's shadow darkens the *previous* glyph's
            // antialiased edge where the two cells meet - one column per
            // character, which is where the last of the text difference was.
            for (int pass = 0; pass < 2; ++pass) {
            for (std::size_t i = 0; i < limit; ++i) {
                const auto alpha_256 = msg().glyph_alpha(i);
                if (alpha_256 <= 0) {
                    continue;
                }
                const auto alpha = static_cast<std::uint8_t>(
                    std::min(255, alpha_256 * 255 / 256));
                // From the string the layout actually walked, at the offset
                // it recorded - not the i'th character of message_.visible(),
                // which is a different rendering of the same source and
                // drops a character wherever the two disagree about escapes.
                const std::string_view source = msg().raw();
                if (i >= layout.glyph_off.size()) {
                    break;
                }
                const auto begin =
                    static_cast<std::size_t>(layout.glyph_off[i]);
                if (begin >= source.size()) {
                    break;
                }
                const auto bytes = utf8_prefix_bytes(
                    source.substr(begin), 1);
                const auto glyph = source.substr(begin, bytes);
                const auto gx = static_cast<float>(layout.glyph_x[i]);
                const auto gy = static_cast<float>(layout.glyph_y[i]);
                // alpha_256 is TXT_DrawTextEx's alph2 itself, not the
                // 0..255 squeeze of it: handing the font the engine's own
                // number is what lets it reproduce BlendTable16 exactly.
                if (pass == 0) {
                    font_.draw_authentic_shadow(
                        renderer_, gx, gy, glyph, alpha, alpha_256);
                } else {
                    font_.draw(
                        renderer_, gx, gy, glyph, 255, 255, 255, alpha,
                        alpha_256);
                }
            }
            }
            // Back to the overlay, as the outline path does at its end;
            // no scrollbar, because vanilla has nothing to scroll - it drops
            // the overflow instead.
            select_overlay();
        } else {

        const float x = message_text_x();
        float y = message_text_y();
        std::size_t source_cursor = 0;
        const auto lines = display_lines(visible);
        const auto glyph_of = engine_glyph_map(visible);
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
            struct AuthenticGlyph {
                float x;
                float y;
                std::string text;
                std::uint8_t alpha;
            };
            std::vector<AuthenticGlyph> authentic_glyphs;
            // Measured once for the line and reused for every glyph in it,
            // instead of asking for the width of a growing prefix twice per
            // glyph per frame.
            const auto& boundaries = font_.glyph_boundaries(line);
            std::size_t glyph_index_in_line = 0;
            // Neighbouring glyphs at the same alpha share one clip and one
            // pair of draws.  A clip per glyph made a message 130-160 draw
            // calls a frame long after it had finished revealing - each one
            // breaking SDL's batch, which on a phone's WebGL is most of a
            // frame's CPU.  Every pixel still gets the same shadow-then-text
            // pair; the runs only stop a pixel column between two rounded
            // glyph boxes being blended twice.
            struct Run {
                float left = 0.0f;
                float right = 0.0f;
                std::uint8_t alpha = 0;
                bool open = false;
            } run;
            const auto flush_run = [&] {
                if (!run.open) {
                    return;
                }
                const SDL_Rect clip{
                    static_cast<int>(std::floor(run.left)),
                    static_cast<int>(std::floor(y)),
                    std::max(1, static_cast<int>(
                                    std::ceil(run.right - run.left))),
                    // Tall enough for the glyph and its shadow: a fixed
                    // height clipped the bottom off large fonts.
                    static_cast<int>(std::ceil(text_line_height())) + 4};
                SDL_SetRenderClipRect(renderer_, &clip);
                font_.draw(renderer_, x + 2.0f, y + 2.0f, line, 0, 0, 0,
                           run.alpha);
                font_.draw(renderer_, x, y, line, 255, 255, 255, run.alpha);
                SDL_SetRenderClipRect(renderer_, nullptr);
                run.open = false;
            };
            while (glyph_offset < line.size()) {
                const auto glyph_bytes = utf8_prefix_bytes(
                    std::string_view(line).substr(glyph_offset), 1);
                const auto source_offset = line_start + glyph_offset;
                const auto glyph_index = source_offset < glyph_of.size()
                    ? glyph_of[source_offset]
                    : std::numeric_limits<std::size_t>::max();
                float glyph_alpha = glyph_index < shown
                    ? static_cast<float>(msg().glyph_alpha(glyph_index))
                          / 256.0f
                    : 0.0f;
                glyph_alpha = std::clamp(glyph_alpha, 0.0f, 1.0f);
                if (glyph_alpha > 0.0f) {
                    const auto glyph_end = glyph_offset + glyph_bytes;
                    const auto glyph = std::string_view(line).substr(
                        glyph_offset, glyph_bytes);
                    const auto alpha = static_cast<std::uint8_t>(
                        glyph_alpha * 255.0f);
                    if (font_.authentic()) {
                        // Held back and drawn after the line's shadows, the
                        // way TXT_DrawText's two passes do it - see the
                        // vanilla branch above.
                        authentic_glyphs.push_back(
                            {authentic_x, y, std::string(glyph), alpha});
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
                    if (run.open && run.alpha == alpha) {
                        run.right = glyph_right;
                    } else {
                        flush_run();
                        run = {glyph_left, glyph_right, alpha, true};
                    }
                } else {
                    flush_run();    // a gap: nothing drawn here
                }
                glyph_offset += glyph_bytes;
                ++glyph_index_in_line;
            }
            flush_run();
            // Every shadow in the line, then every glyph - TXT_DrawText's
            // two calls.  A glyph drawn before the next one's shadow keeps
            // its edge; one drawn after loses a level of it.
            for (const auto& held : authentic_glyphs) {
                font_.draw_authentic_shadow(
                    renderer_, held.x, held.y, held.text, held.alpha);
            }
            for (const auto& held : authentic_glyphs) {
                font_.draw(
                    renderer_, held.x, held.y, held.text,
                    255, 255, 255, held.alpha);
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
    }
    if (ui_mode_ == UiMode::game && msg().engine_bar()) {
        // The engine's log: TXT_WINDOW+1 at LAY_WINDOW+1, the entry being
        // read, and TXT_WINDOW+2 at LAY_WINDOW+2, the voiced line under the
        // pointer drawn over it in another colour.
        draw_engine_text(msg().log_text());
        draw_engine_text(msg().log_voice_text());
    }
    if (ui_mode_ == UiMode::game
        && message_shown() && choosing_ && !choices_.empty()) {
        float y = choice_y_start();
        const bool typed = choice_reveal_finished();
        for (int i = 0; font_.authentic()
             && i < static_cast<int>(choices_.size()); ++i) {
            // TXT_SELECT+i as AVG_ControlSelectWindow sets it: SYS_FONT,
            // DSP_SetTextKage 2, FCT_GLAY (192) - FCT_NORMAL (255) only for
            // the option under the pointer, and only once cond 0 asks - and
            // while typing, DSP_SetTextAlph -1 with
            // DSP_SetTextCount( cnt - j*4 ), which TXT_DrawTextEx turns into
            //     alph2 = LIM( text_cnt - cnt2, 0, 16 ) * 16
            // per character.  Then its two passes: every shadow of the
            // object, then every glyph.
            const int count = typed
                ? -1 : std::clamp(choice_reveal_count(i), 0, 1024);
            const std::uint8_t shade =
                typed && i == choice_highlight_ ? 255 : 192;
            struct Held {
                float x;
                float y;
                std::string text;
                int alpha;
            };
            std::vector<Held> held;
            const auto counted = choice_layout(i);
            const std::string text = choice_engine_text(i);
            for (std::size_t k = 0; k < counted.glyph_x.size()
                 && k < counted.glyph_off.size(); ++k) {
                const int alpha = count < 0 ? 256
                    : std::clamp(count - counted.glyph_count[k], 0, 16) * 16;
                if (alpha <= 0) {
                    continue;
                }
                const auto begin =
                    static_cast<std::size_t>(counted.glyph_off[k]);
                const auto bytes = utf8_prefix_bytes(
                    std::string_view(text).substr(begin), 1);
                held.push_back({static_cast<float>(counted.glyph_x[k]),
                                static_cast<float>(counted.glyph_y[k]),
                                text.substr(begin, bytes), alpha});
            }
            for (const auto& g : held) {
                font_.draw_authentic_shadow(
                    renderer_, g.x, g.y, g.text,
                    static_cast<std::uint8_t>(std::min(255, g.alpha * 255 / 256)),
                    g.alpha);
            }
            for (const auto& g : held) {
                font_.draw(
                    renderer_, g.x, g.y, g.text, shade, shade, shade,
                    static_cast<std::uint8_t>(std::min(255, g.alpha * 255 / 256)),
                    g.alpha);
            }
        }
        for (int i = 0; !font_.authentic()
             && i < static_cast<int>(choices_.size()); ++i) {
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
    text_shake_ = {};
    SDL_SetRenderViewport(renderer_, nullptr);
    if (ui_mode_ == UiMode::game) {
        draw_click_indicator();
    }
    if (ui_mode_ == UiMode::backlog) {
        draw_backlog();
    }
    select_sidebar();
    // ControlHistorySystem: if( NovelMessage.disp && !Avg.demo ) - the bar
    // is not put up while a demo (SetDemoFlag) is running the scene.
    if (msg().engine_bar()) {
        if (ui_mode_ == UiMode::game && msg().history_bar().shown) {
            draw_sidebar();
        }
    } else if ((ui_mode_ == UiMode::game || ui_mode_ == UiMode::backlog)
               && message_shown() && !demo_mode_) {
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
