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

void Game::open_backlog()
{
    if (choosing_) return;
    play_se(-1, 9012, false, 140);
    ui_mode_ = UiMode::backlog;
    backlog_voice_hover_ = -1;
    backlog_scroll_ = 0;
    backlog_depth_ = std::min(
        1, static_cast<int>(backlog_.size()));
}

void Game::close_backlog()
{
    backlog_depth_ = 0;
    backlog_voice_hover_ = -1;
    ui_mode_ = UiMode::game;
}

bool Game::begin_touch_drag(float logical_x, float logical_y)
{
    // Only the widgets you drag take the press.  Everything else waits for
    // the release, so pressing a button and sliding off it does nothing,
    // which is how a touch UI is expected to behave.
    if (config_open_ || name_input_open_ || imgui_->wants_mouse()) {
        return false;  // the panel on top owns this touch.
    }
    if (ui_mode_ == UiMode::game || ui_mode_ == UiMode::backlog) {
        handle_sidebar_click(logical_x, logical_y, false);
        if (backlog_handle_dragging_ || opacity_handle_dragging_) {
            return true;
        }
    }
    if (ui_mode_ == UiMode::game
        && handle_message_scroll_press(logical_x, logical_y)) {
        return true;
    }
    if (ui_mode_ == UiMode::backlog
        && handle_backlog_scroll_press(logical_x, logical_y)) {
        return true;
    }
    return false;
}

void Game::push_touch_mouse_event(
    const SDL_Event& event, int window_width, int window_height)
{
    // The game's own UI is written against a mouse: sliders and scroll
    // handles drag, buttons and choices highlight under the cursor, and
    // clicks act.  SDL can synthesize that from touches, but its synthesis
    // fires for gestures too and ran alongside our own tap handling, which
    // counted every tap twice - so it is off (see the
    // SDL_HINT_TOUCH_MOUSE_EVENTS hint in main()) and we synthesize here
    // instead, with touch manners rather than mouse ones: the press only
    // reaches things you can drag, and a click happens on release, and only
    // if the finger stayed put.
    if (SDL_GetHintBoolean(SDL_HINT_TOUCH_MOUSE_EVENTS, true)) {
        return;  // SDL is synthesizing them already; ours would be a second.
    }
    const auto finger = event.tfinger.fingerID;
    if (event.type == SDL_EVENT_FINGER_DOWN) {
        if (touch_mouse_active_) {
            return;  // a second finger; gestures own it, not the cursor.
        }
        touch_mouse_active_ = true;
        touch_mouse_finger_ = finger;
        touch_mouse_dragging_ = false;
    } else if (!touch_mouse_active_ || finger != touch_mouse_finger_) {
        return;
    }

    const float x = event.tfinger.x * static_cast<float>(window_width);
    const float y = event.tfinger.y * static_cast<float>(window_height);
    const bool ending = event.type == SDL_EVENT_FINGER_UP
        || event.type == SDL_EVENT_FINGER_CANCELED;

    if (event.type != SDL_EVENT_FINGER_CANCELED) {
        // Motion first: it drives a drag in progress, and otherwise moves
        // the highlight under the finger.
        SDL_Event motion{};
        motion.type = SDL_EVENT_MOUSE_MOTION;
        motion.motion.which = SDL_TOUCH_MOUSEID;
        motion.motion.state = ending ? 0 : SDL_BUTTON_LMASK;
        motion.motion.x = x;
        motion.motion.y = y;
        motion.motion.xrel = x - touch_mouse_x_;
        motion.motion.yrel = y - touch_mouse_y_;
        SDL_PushEvent(&motion);
    }
    touch_mouse_x_ = x;
    touch_mouse_y_ = y;

    const auto [logical_x, logical_y] =
        logical_coordinates(x, y, window_width, window_height);
    if (event.type == SDL_EVENT_FINGER_DOWN) {
        touch_mouse_dragging_ = begin_touch_drag(logical_x, logical_y);
        if (touch_mouse_dragging_) {
            // The handle owns the finger now; without this the same drag
            // would still read as a backlog swipe and page the log while
            // the slider moved.
            touch_input_.claim_touch();
        }
        return;
    }
    if (!ending) {
        return;
    }

    touch_mouse_active_ = false;
    if (touch_mouse_dragging_) {
        // The drag was started here rather than by a press event, so end it
        // here too; a drag never counts as a click.
        touch_mouse_dragging_ = false;
        finish_sidebar_drag();
        suppress_sidebar_mouse_up_ = false;
    } else if (event.type != SDL_EVENT_FINGER_CANCELED
               && !touch_input_.last_touch_was_gesture()
               && !touch_input_.last_touch_moved()) {
        // A tap: a still finger lifted from where it landed, and not part of
        // a swipe.  Deliver it as a whole click where it was released.
        for (const bool down : {true, false}) {
            SDL_Event button{};
            button.type = down
                ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
            button.button.button = SDL_BUTTON_LEFT;
            button.button.clicks = 1;
            button.button.down = down;
            button.button.which = SDL_TOUCH_MOUSEID;
            button.button.x = x;
            button.button.y = y;
            SDL_PushEvent(&button);
        }
    }

    // A touch that ends on the sidebar leaves it up: it only fades once a
    // finger lands somewhere else.  Everywhere else the cursor goes away, so
    // the bar starts fading and whatever the finger was over stops being
    // hovered.
    if (logical_x < sidebar_left_x) {
        SDL_Event away{};
        away.type = SDL_EVENT_MOUSE_MOTION;
        away.motion.which = SDL_TOUCH_MOUSEID;
        away.motion.x = -1.0f;
        away.motion.y = -1.0f;
        away.motion.xrel = -1.0f - touch_mouse_x_;
        away.motion.yrel = -1.0f - touch_mouse_y_;
        SDL_PushEvent(&away);
        touch_mouse_x_ = -1.0f;
        touch_mouse_y_ = -1.0f;
    }
    // The highlights themselves always let go, but not before the click
    // above has been handled: the map and the backlog answer a click from
    // whatever was last hovered.  Queueing the clear behind them keeps that
    // order no matter which frame the events come out in.
    if (touch_clear_event_ == 0) {
        touch_clear_event_ = SDL_RegisterEvents(1);
    }
    if (touch_clear_event_ != 0) {
        SDL_Event clear{};
        clear.type = touch_clear_event_;
        SDL_PushEvent(&clear);
    } else {
        clear_pointer_highlights();
    }
}

void Game::clear_pointer_highlights()
{
    // A finger that has lifted is not hovering anything, so nothing should
    // stay lit under it.  The sidebar's fade is deliberately not part of
    // this: the bar itself stays up until a touch lands off it.
    sidebar_hover_ = -1;
    backlog_handle_hover_ = false;
    opacity_handle_hover_ = false;
    backlog_voice_hover_ = -1;
    title_highlight_ = -1;
    menu_highlight_ = -1;
    save_hover_ = -1;
    map_hover_ = -1;
}

void Game::handle_touch_actions()
{
    using Action = th2::TouchAction;
    const auto action = touch_input_.poll_action();
    if (action == Action::None) {
        return;
    }
    // While an ImGui panel is up it owns the touch: a drag over it scrolls
    // the panel rather than paging the backlog, and the swipe shortcuts
    // belong to the game underneath.  Taps still go through so ImGui sees
    // the click, and the back button still closes the panel.
    if ((config_open_ || name_input_open_)
        && action != Action::Tap && action != Action::MenuToggle) {
        return;
    }

    switch (action) {
    case Action::BacklogOlder:
        if (ui_mode_ == UiMode::backlog) {
            backlog_older();
        } else if (ui_mode_ == UiMode::game) {
            open_backlog();
        }
        break;
    case Action::BacklogNewer:
        if (ui_mode_ == UiMode::backlog) {
            backlog_newer();
        }
        break;
    case Action::BacklogOrHideTextbox:
        if (ui_mode_ == UiMode::backlog) {
            close_backlog();
        } else if (ui_mode_ == UiMode::game) {
            message_visible_ = false;
        }
        break;
    case Action::MenuToggle:
        if (ui_mode_ == UiMode::save || ui_mode_ == UiMode::load) {
            close_save_load();
        } else if (ui_mode_ == UiMode::system_menu) {
            close_system_menu();
        } else if (ui_mode_ == UiMode::game || ui_mode_ == UiMode::backlog) {
            open_system_menu();
        } else if (config_open_) {
            close_config();
        }
        break;
    case Action::SkipToggle:
        if (ui_mode_ == UiMode::game) {
            skip_mode_ = !skip_mode_;
            if (skip_mode_) {
                auto_mode_ = false;
            }
            play_se(-1, 9104, false, 255);
        }
        break;
    case Action::AutoModeToggle:
        if (ui_mode_ == UiMode::game) {
            auto_mode_ = !auto_mode_;
            if (auto_mode_) {
                skip_mode_ = false;
            }
            play_se(-1, 9104, false, 255);
        }
        break;
    case Action::Tap:
        // Nothing to do: taps reach the game as a synthesized press and
        // release (see push_touch_mouse_event), the same way a mouse click
        // would, so the sidebar, the backlog and the text handlers have all
        // already seen this one.
        break;
    default:
        break;
    }
}

bool Game::backlog_older()
{
    if (backlog_depth_ >= static_cast<int>(backlog_.size())) {
        return false;
    }
    play_se(-1, 9012, false, 140);
    ++backlog_depth_;
    backlog_scroll_ = 0;
    ui_mode_ = UiMode::backlog;
    return true;
}

bool Game::backlog_newer()
{
    if (backlog_depth_ <= 0) {
        return false;
    }
    play_se(-1, 9012, false, 140);
    backlog_scroll_ = 0;
    if (backlog_depth_ > 1) {
        --backlog_depth_;
    } else {
        close_backlog();
    }
    return true;
}

void Game::execute_menu_item(int index)
{
    switch (index) {
    case 0:
        if (!replay_mode_) open_save_load(UiMode::save);
        break;
    case 1:
        if (!replay_mode_) open_save_load(UiMode::load);
        break;
    case 2: message_visible_ = !message_visible_; break;
    case 3: open_config(); break;
    case 4: break;
    }
}

void Game::open_save_load(UiMode mode)
{
    if (!save_snapshot_) {
        save_snapshot_ = capture_frame_pixels();
    }
    save_return_mode_ =
        ui_mode_ == UiMode::title ? UiMode::title : UiMode::game;
    begin_transition(1, 12, 128, false);
    ui_mode_ = mode;
    save_confirm_slot_ = -1;
    save_hover_ = -1;
    load_error_.clear();
    refresh_save_page();
    if (newest_save_slot_ >= 0) {
        save_page_ = newest_save_slot_ >= 100
            ? 10 : newest_save_slot_ / 10;
        refresh_save_page();
    }
    ensure_save_load_focus();
}

void Game::close_save_load()
{
    save_confirm_slot_ = -1;
    load_error_.clear();
    begin_transition(1, 12, 128, false);
    ui_mode_ = save_return_mode_;
}

void Game::draw_save_digit_sheet_text(
    float x, float y, std::string_view text,
    std::uint8_t red, std::uint8_t green,
    std::uint8_t blue)
{
    if (!ui_save_digits_) {
        return;
    }
    float texture_width = 0.0f;
    float texture_height = 0.0f;
    SDL_GetTextureSize(ui_save_digits_.get(), &texture_width, &texture_height);
    const float glyph_width = texture_width / 16.0f;
    const float glyph_height = texture_height / 4.0f;
    SDL_SetTextureColorMod(ui_save_digits_.get(), red, green, blue);
    for (const unsigned char character : text) {
        if (character >= '!' && character <= '_') {
            const int index = static_cast<int>(character) - ('!' - 1);
            const SDL_FRect src{
                glyph_width * static_cast<float>(index % 16),
                glyph_height * static_cast<float>(index / 16),
                glyph_width, glyph_height};
            const SDL_FRect dst{x, y, glyph_width, glyph_height};
            SDL_RenderTexture(
                renderer_, ui_save_digits_.get(), &src, &dst);
        }
        x += glyph_width;
    }
    SDL_SetTextureColorMod(ui_save_digits_.get(), 255, 255, 255);
}

void Game::draw_save_digit_number(float x, float y, int number, int digits)
{
    if (!ui_save_digits_) {
        return;
    }
    float texture_width = 0.0f;
    float texture_height = 0.0f;
    SDL_GetTextureSize(ui_save_digits_.get(), &texture_width, &texture_height);
    const float glyph_width = texture_width / 16.0f;
    const float glyph_height = texture_height / 4.0f;
    const auto text = std::format("{:0{}d}", number, digits);
    for (const unsigned char character : text) {
        if (character >= '0' && character <= '9') {
            const SDL_FRect src{
                glyph_width * static_cast<float>(character - '0'),
                glyph_height, glyph_width, glyph_height};
            const SDL_FRect dst{x, y, glyph_width, glyph_height};
            SDL_RenderTexture(
                renderer_, ui_save_digits_.get(), &src, &dst);
        }
        x += glyph_width;
    }
}

void Game::draw_system_menu()
{
    // Background
    if (ui_sys_menu_bg_) {
        SDL_RenderTexture(renderer_, ui_sys_menu_bg_.get(),
                          nullptr, nullptr);
    } else {
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 180);
        SDL_RenderFillRect(renderer_, nullptr);
    }

    draw_save_digit_sheet_text(
        138.0f, 12.0f,
        std::format("{}A{}B", runtime_.flag(0), runtime_.flag(1)));

    // 4 main buttons from sys0110.tga
    // Layout: Save(0,0) Load(400,0) Hide(0,246) Settings(400,246)
    // Each: w=400, h=82, 3 states stacked vertically (0,82,164)
    const int btn_x[4] = {0, 400, 0, 400};
    const int btn_y[4] = {0, 0, 246, 246};
    const int dst_x[4] = {200, 200, 200, 200};
    const int dst_y[4] = {112, 200, 288, 376};

    for (int i = 0; i < 4; ++i) {
        const bool disabled = replay_mode_ && (i == 0 || i == 1);
        const int state = (i == menu_highlight_) ? 82 : 0;
        const SDL_FRect src{
            static_cast<float>(btn_x[i]),
            static_cast<float>(btn_y[i] + state), 400.0f, 82.0f};
        const SDL_FRect dst{
            static_cast<float>(dst_x[i]),
            static_cast<float>(dst_y[i]), 400.0f, 82.0f};
        if (ui_sys_menu_btns_) {
            SDL_SetTextureAlphaMod(
                ui_sys_menu_btns_.get(), disabled ? 64 : 255);
            SDL_RenderTexture(renderer_, ui_sys_menu_btns_.get(),
                              &src, &dst);
            SDL_SetTextureAlphaMod(ui_sys_menu_btns_.get(), 255);
        } else {
            // Fallback: draw text
            const char* labels[4] = {"Save", "Load", "Hide Text", "Settings"};
            const float tw = std::strlen(labels[i]) * 12.0f;
            const float tx = dst_x[i] + (400.0f - tw) / 2.0f;
            const float ty = dst_y[i] + (82.0f - 24.0f) / 2.0f;
            font_.draw(renderer_, tx + 2, ty + 2, labels[i], 0, 0, 0);
            if (i == menu_highlight_) {
                SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 40);
                SDL_RenderFillRect(renderer_, &dst);
                font_.draw(renderer_, tx, ty, labels[i], 255, 255, 255);
            } else {
                font_.draw(renderer_, tx, ty, labels[i], 128, 128, 128);
            }
        }
    }

    // sys0111.tga stores normal, hover and pressed states horizontally.
    const float cs = menu_highlight_ == 4 ? 188.0f : 0.0f;
    const SDL_FRect csrc{cs, 0.0f, 188.0f, 32.0f};
    const SDL_FRect cdst{306.0f, 480.0f, 188.0f, 32.0f};
    if (ui_sys_cancel_) {
        SDL_RenderTexture(renderer_, ui_sys_cancel_.get(), &csrc, &cdst);
    } else {
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        if (menu_highlight_ == 4) {
            SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 40);
            SDL_RenderFillRect(renderer_, &cdst);
        }
        font_.draw(renderer_, 356.0f, 484.0f, "Close", 0, 0, 0);
        font_.draw(renderer_, 354.0f, 482.0f, "Close",
                   menu_highlight_ == 4 ? 255 : 128,
                   menu_highlight_ == 4 ? 255 : 128,
                   menu_highlight_ == 4 ? 255 : 128);
    }
}

void Game::draw_map_layer(
    int field, float x, float alpha,
    bool draw_field, bool draw_events)
{
    if (field < 0 || field >= 5 || !map_fields_[field]) {
        return;
    }
    if (draw_field) {
        SDL_SetTextureAlphaModFloat(map_fields_[field].get(), alpha);
        const SDL_FRect field_dst{x + 80.0f, 76.0f, 640.0f, 480.0f};
        SDL_RenderTexture(
            renderer_, map_fields_[field].get(), nullptr, &field_dst);
        SDL_SetTextureAlphaModFloat(map_fields_[field].get(), 1.0f);
    }
    if (!draw_events) {
        return;
    }

    std::array<int, 10> overlaps{};
    for (std::size_t i = 0; i < map_events_.size(); ++i) {
        const auto& event = map_events_[i];
        if (event.position < 0
            || event.position >= static_cast<int>(map_positions_.size())) {
            continue;
        }
        const auto& position = map_positions_[event.position];
        const int overlap = ++overlaps[position.overlap];
        int cx = position.x;
        int cy = position.y;
        if (overlap == 2) cx -= 200;
        else if (overlap == 3) cx += 200;
        else if (overlap == 4) { cx -= 100; cy += 160; }
        if (position.field != field) {
            continue;
        }

        if (map_markers_) {
            int state = static_cast<int>(i) == map_hover_ ? 1 : 0;
            if (static_cast<int>(i) == map_selected_) state = 2;
            const SDL_FRect src{
                static_cast<float>(state * 130),
                static_cast<float>(event.position * 118),
                130.0f, 118.0f};
            const SDL_FRect dst{
                x + cx + 20.0f, static_cast<float>(cy - 118),
                130.0f, 118.0f};
            SDL_SetTextureAlphaModFloat(map_markers_.get(), alpha);
            SDL_RenderTexture(
                renderer_, map_markers_.get(), &src, &dst);
            SDL_SetTextureAlphaModFloat(map_markers_.get(), 1.0f);
        }
        if (i < map_characters_.size()
            && map_characters_[i].texture) {
            const auto& character = map_characters_[i];
            const auto elapsed_ticks =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - map_started_)
                    .count() * 60 / 1000;
            int cycle_ticks = 0;
            for (const auto& step : character.steps) {
                cycle_ticks += step.ticks;
            }
            int tick = cycle_ticks > 0
                ? static_cast<int>(elapsed_ticks % cycle_ticks) : 0;
            int frame = character.steps.front().frame;
            for (const auto& step : character.steps) {
                if (tick < step.ticks) {
                    frame = step.frame;
                    break;
                }
                tick -= step.ticks;
            }
            if (frame < 0
                || frame >= static_cast<int>(character.frames.size())) {
                continue;
            }
            SDL_SetTextureAlphaModFloat(
                character.texture.get(), alpha);
            for (const auto& part : character.frames[frame]) {
                const SDL_FRect destination{
                    x + cx + part.x, cy + part.y,
                    part.source.w, part.source.h};
                SDL_RenderTexture(
                    renderer_, character.texture.get(),
                    &part.source, &destination);
            }
            SDL_SetTextureAlphaModFloat(
                character.texture.get(), 1.0f);
        }
    }
}

void Game::draw_map(bool ui)
{
    const float fade = map_fade_ticks_ > 0
        ? map_fade_ticks_ / 16.0f
        : std::min(1.0f,
                std::chrono::duration<float>(
                std::chrono::steady_clock::now() - map_started_).count()
                * 60.0f / 16.0f);
    if (!ui && map_frame_) {
        SDL_SetTextureAlphaModFloat(map_frame_.get(), fade);
        SDL_RenderTexture(renderer_, map_frame_.get(), nullptr, nullptr);
        SDL_SetTextureAlphaModFloat(map_frame_.get(), 1.0f);
    }

    if (map_slide_ticks_ == 0) {
        draw_map_layer(map_field_, 0.0f, fade, !ui, ui);
    } else {
        const int ticks = std::abs(map_slide_ticks_);
        const float square = static_cast<float>(ticks * ticks);
        const float direction = map_slide_ticks_ > 0 ? 1.0f : -1.0f;
        const float next_x = direction * 800.0f * square / 256.0f;
        const float previous_x =
            direction * 800.0f * (square - 256.0f) / 256.0f;
        draw_map_layer(map_field_, next_x, fade, !ui, ui);
        draw_map_layer(
            map_previous_field_, previous_x, fade, !ui, ui);
    }

    if (ui && map_arrows_) {
        const int left_state = map_hover_ == -2 ? 1 : 0;
        const int right_state = map_hover_ == -3 ? 1 : 0;
        const SDL_FRect left_src{
            static_cast<float>(left_state * 112), 0.0f, 56.0f, 122.0f};
        const SDL_FRect right_src{
            static_cast<float>(right_state * 112 + 56), 0.0f,
            56.0f, 122.0f};
        const SDL_FRect left_dst{24.0f, 239.0f, 56.0f, 122.0f};
        const SDL_FRect right_dst{720.0f, 239.0f, 56.0f, 122.0f};
        SDL_SetTextureAlphaModFloat(map_arrows_.get(), fade);
        SDL_RenderTexture(
            renderer_, map_arrows_.get(), &left_src, &left_dst);
        SDL_RenderTexture(
            renderer_, map_arrows_.get(), &right_src, &right_dst);
        SDL_SetTextureAlphaModFloat(map_arrows_.get(), 1.0f);
    }
}


}  // namespace th2app
