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
            window_hidden_ = true;
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
    case 2: window_hidden_ = !window_hidden_; break;
    case 3: open_config(); break;
    case 4: break;
    }
}

void Game::open_save_load(UiMode mode)
{
    if (!save_snapshot_) {
        save_snapshot_ = capture_frame_thumbnail(
    save_thumbnail_width, save_thumbnail_height);
    }
    save_return_mode_ =
        ui_mode_ == UiMode::title ? UiMode::title : UiMode::game;
    begin_transition(1, 12, 128, false, EffectTiming::menu);
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
    begin_transition(1, 12, 128, false, EffectTiming::menu);
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

// AVG_ControlMapEvent's graphs, drawn as the rasteriser draws them: by layer
// (the frame at LAY_MAP, fields +1, markers +2, characters +3, arrows +4),
// through the integer blend, at the levels and source offsets its steps set.
//
//   step 2 (fading in, map_enter_ticks_): DRW_BLD(scnt*16) on the frame,
//          field 1 and the arrows only - no markers or characters yet.
//   step 3: everything DRW_NML.  Not rolling, the arrows' and markers'
//          source offsets are reset and set again from `select` each frame;
//          rolling, they are left alone, so a pressed arrow stays pressed.
//          The markers do not travel with the field - they cross-fade in
//          place at DRW_BLD(cnt*32).
//   step 4 (map_fade_ticks_): DSP_SetGraphFade(128 - scnt*8), a brightness.
namespace {
struct MapLevels {
    int frame = 0;          // GRP_MAP, the fields, the arrows
    int bright = 128;       // step 4
    bool markers = false;   // step 3 onward
};
}

int Game::map_marker_level(int field) const
{
    // The roll as the engine's marker pass saw it: before this frame's step
    // took one off (map_marker_roll_), or, on a frame that ran no roll step,
    // as it stands - 16 on the frame a page turn starts, 0 at rest.
    const int roll = map_marker_roll_ != 0 ? map_marker_roll_
                                           : std::abs(map_slide_ticks_);
    if (roll == 0) {
        return field == map_field_ ? 256 : 0;
    }
    const int cnt = field == map_field_ ? 8 - roll
        : field == map_previous_field_ ? roll - 8 : -1;
    return std::clamp(cnt * 32, 0, 256);   // DRW_BLD clamps to 0..256
}

void Game::draw_map(bool ui)
{
    MapLevels levels;
    if (map_enter_ticks_ > 0 || map_enter_finished_this_frame_) {
        // k = frames into step 2: scnt is read before it is incremented, so
        // the first frame is BLD(0) and the sixteenth has already been set
        // back to DRW_NML.  Frame 0 is the clock's last, with nothing up.
        const int k = 16 - map_enter_ticks_;
        levels.frame = k <= 0 ? 0 : k >= 16 ? 256 : (k - 1) * 16;
    } else {
        levels.frame = 256;
        levels.markers = true;
    }
    if (map_fade_ticks_ > 0 || map_finish_pending_) {
        // The click lands after the frame's update, so the click frame
        // draws with map_fade_ticks_ still 16: scnt 0, as the engine's
        // click frame is still step 3's.  Then 1..16, the last at 0.
        const int scnt = map_finish_pending_ ? 16 : 16 - map_fade_ticks_;
        levels.bright = std::max(0, 128 - scnt * 8);
    }

    if (!ui) {
        if (map_frame_) {
            draw_engine_blit(map_frame_.get(), SDL_FRect{0, 0, 800, 600},
                             SDL_FRect{0, 0, 800, 600}, levels.frame, 0,
                             levels.bright);
        }
        // Fields, in graph order GRP_MAP+1+i.  Step 2 shows field 1 only.
        for (int field = 0; field < 5; ++field) {
            if (!map_fields_[field]) {
                continue;
            }
            int x = 0;
            bool shown = field == map_field_;
            if (map_slide_ticks_ != 0) {
                // cnt = R*R; x1 = +-800*cnt/256; x2 = +-800*(cnt-256)/256
                const int roll = std::abs(map_slide_ticks_);
                const int sign = map_slide_ticks_ > 0 ? 1 : -1;
                const int cnt = roll * roll;
                if (field == map_field_) {
                    x = sign * 800 * cnt / 256;
                } else if (field == map_previous_field_) {
                    x = sign * 800 * (cnt - 256) / 256;
                    shown = true;
                }
            }
            if (!levels.markers) {
                shown = field == 1;
            }
            if (!shown) {
                continue;
            }
            float w = 0.0f;
            float h = 0.0f;
            SDL_GetTextureSize(map_fields_[field].get(), &w, &h);
            draw_engine_blit(
                map_fields_[field].get(), SDL_FRect{0, 0, w, h},
                SDL_FRect{static_cast<float>(80 + x), 76.0f, w, h},
                levels.frame, 6, levels.bright);
        }
        return;
    }

    // Marker and character positions: MapEventCharPos, pushed apart where
    // several share a spot (EventFieldKaburi).
    std::vector<std::pair<int, int>> spots(map_events_.size(), {0, 0});
    std::vector<int> fields(map_events_.size(), -1);
    {
        std::array<int, 10> overlaps{};
        for (std::size_t i = 0; i < map_events_.size(); ++i) {
            const auto& event = map_events_[i];
            if (event.position < 0
                || event.position
                    >= static_cast<int>(map_positions_.size())) {
                continue;
            }
            const auto& position = map_positions_[event.position];
            const int overlap = ++overlaps[position.overlap];
            int cx = position.x;
            int cy = position.y;
            if (overlap == 2) cx -= 200;
            else if (overlap == 3) cx += 200;
            else if (overlap == 4) { cx -= 100; cy += 160; }
            spots[i] = {cx, cy};
            fields[i] = position.field;
        }
    }
    if (levels.markers && map_markers_) {
        for (std::size_t i = 0; i < map_events_.size(); ++i) {
            if (fields[i] < 0) continue;
            const int level = map_marker_level(fields[i]);
            // 0 normal, 130 under the pointer, 260 clicked - as the last
            // frame that was not rolling left it.
            int state = static_cast<int>(i) == map_select_back_ ? 1 : 0;
            if (static_cast<int>(i) == map_selected_) state = 2;
            draw_engine_blit(
                map_markers_.get(),
                SDL_FRect{static_cast<float>(state * 130),
                          static_cast<float>(map_events_[i].position * 118),
                          130.0f, 118.0f},
                SDL_FRect{static_cast<float>(spots[i].first + 20),
                          static_cast<float>(spots[i].second - 118),
                          130.0f, 118.0f},
                level, 0, levels.bright);
        }
        for (std::size_t i = 0; i < map_events_.size(); ++i) {
            if (fields[i] < 0 || i >= map_characters_.size()
                || !map_characters_[i].texture) {
                continue;
            }
            const auto& character = map_characters_[i];
            // DSP_SetSprite runs in case 1, on the frame AVG_ViewClock( 19 )
            // reports the clock done - it is in the same block as the map's
            // BGM and mouse rects, not in case 0 - and SPR_RenewSprite at the
            // top of every frame after that.  Measured on the reference with
            // AnimeControl logged: fresh on the BGM frame, one count behind
            // every frame from the next.
            const int frame = sprite_frame_after(
                character, map_sprite_start_ < 0
                    ? 0 : map_anim_frames_ - map_sprite_start_);
            if (frame < 0
                || frame >= static_cast<int>(character.frames.size())) {
                continue;
            }
            const int level = map_marker_level(fields[i]);
            for (const auto& part : character.frames[frame]) {
                draw_engine_blit(
                    character.texture.get(), part.source,
                    SDL_FRect{spots[i].first + part.x,
                              spots[i].second + part.y,
                              part.source.w, part.source.h},
                    level, sprite_blend_mode(character.texture.get()),
                    levels.bright);
            }
        }
    }
    if (map_arrows_) {
        // GRP_MAP+6 (left) and +7 (right): 112*state, +56 for the right.
        const auto state_of = [&](int arrow) {
            if (!levels.markers) return 0;   // SetGraphPos's own offset
            if (map_arrow_pressed_ == arrow) return 2;
            return map_select_back_ == arrow ? 1 : 0;
        };
        draw_engine_blit(
            map_arrows_.get(),
            SDL_FRect{static_cast<float>(state_of(-2) * 112), 0, 56, 122},
            SDL_FRect{24, 239, 56, 122}, levels.frame, 0, levels.bright);
        draw_engine_blit(
            map_arrows_.get(),
            SDL_FRect{static_cast<float>(state_of(-3) * 112 + 56), 0, 56, 122},
            SDL_FRect{720, 239, 56, 122}, levels.frame, 0, levels.bright);
    }
}

}  // namespace th2app
