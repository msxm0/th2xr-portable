#include "game.hpp"

#include "gl_blend.hpp"
#include "icon.hpp"
#include "image.hpp"

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_system.h>
#include <imgui.h>
#include <zstd.h>

#include <algorithm>
#include <chrono>
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

void Game::draw_save_load()
{
    auto& background = ui_mode_ == UiMode::save ? ui_save_bg_ : ui_load_bg_;
    if (background) {
        SDL_RenderTexture(renderer_, background.get(), nullptr, nullptr);
    }
    const SDL_FRect rows_dst{15.0f, 111.0f, 770.0f, 378.0f};
    if (ui_save_rows_) {
        SDL_RenderTexture(
            renderer_, ui_save_rows_.get(), nullptr, &rows_dst);
    }

    for (int i = 0; i < 10; ++i) {
        const float x = 16.0f + 390.0f * (i / 5);
        const float y = 112.0f + 76.0f * (i % 5);
        if (i == save_hover_ && ui_save_rows_hover_) {
            const SDL_FRect src{
                390.0f * (i / 5), 76.0f * (i % 5), 380.0f, 74.0f};
            const SDL_FRect dst{x, y, 380.0f, 74.0f};
            SDL_RenderTexture(
                renderer_, ui_save_rows_hover_.get(), &src, &dst);
        }
        if (save_thumbnails_[i]) {
            const SDL_FRect thumb{x + 15.0f, y + 5.0f, 80.0f, 60.0f};
            SDL_RenderTexture(
                renderer_, save_thumbnails_[i].get(), nullptr, &thumb);
        }

        const int slot = save_page_ * 10 + i;
        draw_save_digit_number(x + 98.0f, y + 10.0f, slot + 1, 3);
        if (!visible_saves_[i].exists) {
            continue;
        }

        std::tm local{};
        localtime_r(&visible_saves_[i].timestamp, &local);
        const auto game_date = visible_saves_[i].game_month == 0
            ? std::string("?A?B")
            : std::format(
                "{}A{}B", visible_saves_[i].game_month,
                visible_saves_[i].game_day);
        draw_save_digit_sheet_text(x + 152.0f, y + 10.0f, game_date);
        draw_save_digit_sheet_text(
            x + 98.0f, y + 43.0f,
            std::format("{:04d}", local.tm_year + 1900), 120, 43, 56);
        draw_save_digit_sheet_text(
            x + 164.0f, y + 43.0f,
            std::format("{:02d}/{:02d}", local.tm_mon + 1, local.tm_mday),
            120, 43, 56);
        draw_save_digit_sheet_text(
            x + 244.0f, y + 43.0f,
            std::format("{:02d}:{:02d}", local.tm_hour, local.tm_min),
            120, 43, 56);
        font_.draw_save_menu(
            renderer_, x + 222.0f, y + 10.0f,
            visible_saves_[i].message.substr(0, 18), 255, 245, 225);
        if (slot == newest_save_slot_ && ui_save_new_) {
            const SDL_FRect badge{x + 316.0f, y + 37.0f, 56.0f, 29.0f};
            SDL_RenderTexture(
                renderer_, ui_save_new_.get(), nullptr, &badge);
        }
    }

    constexpr int total_pages = 11;
    draw_save_digit_sheet_text(
        364.0f, 78.0f,
        std::format("{:02d}/{:02d}", save_page_ + 1, total_pages));
    if (ui_save_controls_) {
        const SDL_FRect prev_src{
            0.0f, save_hover_ == 10 ? 64.0f : 0.0f, 130.0f, 32.0f};
        const SDL_FRect next_src{
            0.0f, 32.0f + (save_hover_ == 11 ? 64.0f : 0.0f),
            130.0f, 32.0f};
        const SDL_FRect prev_dst{190.0f, 72.0f, 130.0f, 32.0f};
        const SDL_FRect next_dst{482.0f, 72.0f, 130.0f, 32.0f};
        SDL_RenderTexture(
            renderer_, ui_save_controls_.get(), &prev_src,
            &prev_dst);
        SDL_RenderTexture(
            renderer_, ui_save_controls_.get(), &next_src,
            &next_dst);
    }
    if (ui_sys_cancel_) {
        const SDL_FRect src{
            save_hover_ == 12 ? 188.0f : 0.0f, 0.0f, 188.0f, 32.0f};
        const SDL_FRect dst{306.0f, 496.0f, 188.0f, 32.0f};
        SDL_RenderTexture(renderer_, ui_sys_cancel_.get(), &src, &dst);
    }

    if (save_confirm_slot_ >= 0) {
        auto& prompt =
            ui_mode_ == UiMode::save ? ui_save_prompt_ : ui_load_prompt_;
        if (prompt) {
            const SDL_FRect dst{0.0f, 246.0f, 800.0f, 142.0f};
            SDL_RenderTexture(renderer_, prompt.get(), nullptr, &dst);
        }
        const int selected = save_confirm_slot_ - save_page_ * 10;
        if (selected >= 0 && selected < 10) {
            const float x = 25.0f;
            const float y = 271.0f;
            if (save_thumbnails_[selected]) {
                const SDL_FRect thumb{x, y + 2.0f, 80.0f, 60.0f};
                SDL_RenderTexture(
                    renderer_, save_thumbnails_[selected].get(),
                    nullptr, &thumb);
            }
            draw_save_digit_number(
                x + 98.0f, y + 4.0f, save_confirm_slot_ + 1, 3);
            const auto game_date =
                visible_saves_[selected].game_month == 0
                ? std::string("?A?B")
                : std::format(
                    "{}A{}B", visible_saves_[selected].game_month,
                    visible_saves_[selected].game_day);
            draw_save_digit_sheet_text(x + 152.0f, y + 4.0f, game_date);
            font_.draw_save_menu(
                renderer_, x + 222.0f, y + 4.0f,
                visible_saves_[selected].message.substr(0, 18),
                255, 245, 225);

            std::tm local{};
            localtime_r(
                &visible_saves_[selected].timestamp, &local);
            draw_save_digit_sheet_text(
                x + 98.0f, y + 37.0f,
                std::format("{:04d}", local.tm_year + 1900),
                120, 43, 56);
            draw_save_digit_sheet_text(
                x + 164.0f, y + 37.0f,
                std::format(
                    "{:02d}/{:02d}", local.tm_mon + 1, local.tm_mday),
                120, 43, 56);
            draw_save_digit_sheet_text(
                x + 244.0f, y + 37.0f,
                std::format("{:02d}:{:02d}", local.tm_hour, local.tm_min),
                120, 43, 56);
        }
        if (ui_confirm_buttons_) {
            const SDL_FRect yes_src{
                0.0f, save_hover_ == 13 ? 32.0f : 0.0f, 130.0f, 32.0f};
            const SDL_FRect no_src{
                130.0f, save_hover_ == 14 ? 32.0f : 0.0f,
                130.0f, 32.0f};
            const SDL_FRect yes_dst{461.0f, 320.0f, 130.0f, 32.0f};
            const SDL_FRect no_dst{606.0f, 320.0f, 130.0f, 32.0f};
            SDL_RenderTexture(
                renderer_, ui_confirm_buttons_.get(), &yes_src,
                &yes_dst);
            SDL_RenderTexture(
                renderer_, ui_confirm_buttons_.get(), &no_src,
                &no_dst);
        }
    }

    if (!load_error_.empty()) {
        font_.draw_save_menu(
            renderer_, 21.0f, 571.0f, load_error_, 0, 0, 0);
        font_.draw_save_menu(
            renderer_, 20.0f, 570.0f, load_error_, 255, 80, 80);
    }
}

int Game::save_load_hit(float x, float y) const
{
    if (save_confirm_slot_ >= 0) {
        if (x >= 461 && x < 591 && y >= 320 && y < 352) return 13;
        if (x >= 606 && x < 736 && y >= 320 && y < 352) return 14;
        return -1;
    }
    for (int i = 0; i < 10; ++i) {
        const float left = 16.0f + 390.0f * (i / 5);
        const float top = 112.0f + 76.0f * (i % 5);
        if (x >= left && x < left + 380 && y >= top && y < top + 74) {
            return i;
        }
    }
    if (x >= 190 && x < 320 && y >= 72 && y < 104) return 10;
    if (x >= 482 && x < 612 && y >= 72 && y < 104) return 11;
    if (x >= 306 && x < 494 && y >= 496 && y < 528) return 12;
    return -1;
}

bool Game::save_load_item_enabled(int item) const
{
    if (save_confirm_slot_ >= 0) {
        return item == 13 || item == 14;
    }
    if (item >= 0 && item < 10) {
        return ui_mode_ == UiMode::save || visible_saves_[item].exists;
    }
    return item == 10 || item == 11 || item == 12;
}

void Game::ensure_save_load_focus()
{
    if (save_load_item_enabled(save_hover_)) {
        return;
    }
    if (save_confirm_slot_ >= 0) {
        save_hover_ = 14;
        return;
    }
    if (newest_save_slot_ >= 0) {
        const int visible_slot = save_page_ == 10
            ? newest_save_slot_ - 100
            : newest_save_slot_ - save_page_ * 10;
        if (visible_slot >= 0 && visible_slot < 10
            && save_load_item_enabled(visible_slot)) {
            save_hover_ = visible_slot;
            return;
        }
    }
    for (int item = 0; item < 13; ++item) {
        if (save_load_item_enabled(item)) {
            save_hover_ = item;
            return;
        }
    }
    save_hover_ = -1;
}

void Game::move_save_load_focus(SDL_Keycode key)
{
    const int previous = save_hover_;
    ensure_save_load_focus();

    if (save_confirm_slot_ >= 0) {
        if (key == SDLK_LEFT) {
            save_hover_ = 13;
        } else if (key == SDLK_RIGHT) {
            save_hover_ = 14;
        }
    } else if (save_hover_ >= 0 && save_hover_ < 10) {
        const int column = save_hover_ / 5;
        const int row = save_hover_ % 5;
        auto focus_slot = [&](int candidate) {
            if (candidate >= 0 && candidate < 10
                && save_load_item_enabled(candidate)) {
                save_hover_ = candidate;
                return true;
            }
            return false;
        };
        if (key == SDLK_UP) {
            for (int r = row - 1; r >= 0; --r) {
                if (focus_slot(column * 5 + r)) break;
            }
            if (save_hover_ == previous) {
                save_hover_ = column == 0 ? 10 : 11;
            }
        } else if (key == SDLK_DOWN) {
            for (int r = row + 1; r < 5; ++r) {
                if (focus_slot(column * 5 + r)) break;
            }
            if (save_hover_ == previous) {
                save_hover_ = 12;
            }
        } else if (key == SDLK_LEFT) {
            if (column == 1 && !focus_slot(row)) {
                save_hover_ = 10;
            } else if (column == 0) {
                save_hover_ = 10;
            }
        } else if (key == SDLK_RIGHT) {
            if (column == 0 && !focus_slot(5 + row)) {
                save_hover_ = 11;
            } else if (column == 1) {
                save_hover_ = 11;
            }
        }
    } else {
        if (key == SDLK_LEFT) {
            save_hover_ = 10;
        } else if (key == SDLK_RIGHT) {
            save_hover_ = 11;
        } else if (key == SDLK_DOWN) {
            const int column = save_hover_ == 11 ? 1 : 0;
            for (int r = 0; r < 5; ++r) {
                const int candidate = column * 5 + r;
                if (save_load_item_enabled(candidate)) {
                    save_hover_ = candidate;
                    break;
                }
            }
        } else if (key == SDLK_UP) {
            for (int r = 4; r >= 0; --r) {
                const int left = r;
                const int right = 5 + r;
                if (save_load_item_enabled(left)) {
                    save_hover_ = left;
                    break;
                }
                if (save_load_item_enabled(right)) {
                    save_hover_ = right;
                    break;
                }
            }
        }
    }

    if (save_hover_ != previous && save_hover_ >= 0) {
        play_se(-1, 9108, false, 255);
    }
}

void Game::activate_save_load_item(int item)
{
    load_error_.clear();
    if (item >= 0 && item < 10) {
        const int slot = save_page_ * 10 + item;
        if (ui_mode_ == UiMode::load && !visible_saves_[item].exists) {
            return;
        }
        play_se(-1, 9104, false, 255);
        if (ui_mode_ == UiMode::save && !visible_saves_[item].exists) {
            save(slot);
            close_save_load();
        } else {
            save_confirm_slot_ = slot;
            save_hover_ = 14;
        }
    } else if (item == 10 || item == 11) {
        play_se(-1, 9104, false, 255);
        constexpr int page_count = 11;
        save_page_ = (save_page_ + (item == 10 ? page_count - 1 : 1)) % page_count;
        refresh_save_page();
    } else if (item == 12) {
        play_se(-1, 9104, false, 255);
        close_save_load();
    } else if (item == 13 && save_confirm_slot_ >= 0) {
        play_se(-1, 9104, false, 255);
        const int slot = save_confirm_slot_;
        if (ui_mode_ == UiMode::save) {
            save(slot);
            close_save_load();
        } else if (load(slot)) {
            save_confirm_slot_ = -1;
            ui_mode_ = UiMode::game;
        } else {
            load_error_ = "Incompatible save version.";
        }
    } else if (item == 14) {
        play_se(-1, 9104, false, 255);
        save_confirm_slot_ = -1;
        save_hover_ = -1;
    }
}

void Game::handle_save_load_input(const SDL_Event& event)
{
    if (event.type == SDL_EVENT_MOUSE_MOTION) {
        const int hovered = save_load_hit(event.motion.x, event.motion.y);
        if (hovered != save_hover_) {
            save_hover_ = hovered;
            if (save_hover_ >= 0) {
                play_se(-1, 9108, false, 255);
            }
        }
    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        if (event.button.button == SDL_BUTTON_RIGHT) {
            play_se(-1, 9107, false, 255);
            if (save_confirm_slot_ >= 0) {
                save_confirm_slot_ = -1;
            } else {
                close_save_load();
            }
        } else if (event.button.button == SDL_BUTTON_LEFT) {
            activate_save_load_item(
                save_load_hit(event.button.x, event.button.y));
        }
    } else if (event.type == SDL_EVENT_KEY_DOWN) {
        if (event.key.key == SDLK_ESCAPE) {
            play_se(-1, 9107, false, 255);
            if (save_confirm_slot_ >= 0) {
                save_confirm_slot_ = -1;
                ensure_save_load_focus();
            } else {
                close_save_load();
            }
        } else if (event.key.key == SDLK_UP
                   || event.key.key == SDLK_DOWN
                   || event.key.key == SDLK_LEFT
                   || event.key.key == SDLK_RIGHT) {
            move_save_load_focus(event.key.key);
        } else if (event.key.key == SDLK_PAGEUP) {
            activate_save_load_item(10);
            ensure_save_load_focus();
        } else if (event.key.key == SDLK_PAGEDOWN) {
            activate_save_load_item(11);
            ensure_save_load_focus();
        } else if (is_confirm_key(event.key.key)) {
            ensure_save_load_focus();
            activate_save_load_item(save_hover_);
        }
    }
}

void Game::handle_system_menu_input(const SDL_Event& event)
{
    const int dst_x[5] = {200, 200, 200, 200, 306};
    const int dst_y[5] = {112, 200, 288, 376, 480};
    const int dst_w[5] = {400, 400, 400, 400, 188};
    const int dst_h[5] = {82, 82, 82, 82, 32};
    const auto enabled = [&](int item) {
        return !replay_mode_ || (item != 0 && item != 1);
    };
    const auto move_highlight = [&](int direction) {
        do {
            menu_highlight_ =
                (menu_highlight_ + direction + 5) % 5;
        } while (!enabled(menu_highlight_));
    };

    const int previous_highlight = menu_highlight_;
    if (event.type == SDL_EVENT_KEY_DOWN) {
        if (event.key.key == SDLK_ESCAPE) {
            play_se(-1, 9107, false, 255);
            close_system_menu();
        } else if (is_confirm_key(event.key.key)) {
            const int item = menu_highlight_;
            if (item >= 0 && enabled(item)) {
                play_se(-1, 9014, false, 255);
                close_system_menu();
                execute_menu_item(item);
            }
        } else if (event.key.key == SDLK_UP) {
            move_highlight(-1);
        } else if (event.key.key == SDLK_DOWN) {
            move_highlight(1);
        }
    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        if (event.button.button == SDL_BUTTON_RIGHT) {
            play_se(-1, 9107, false, 255);
            close_system_menu();
        } else if (event.button.button == SDL_BUTTON_LEFT) {
            const float mx = event.button.x; const float my = event.button.y;
            for (int i = 0; i < 5; ++i) {
                if (mx >= dst_x[i] && mx < dst_x[i] + dst_w[i]
                    && my >= dst_y[i] && my < dst_y[i] + dst_h[i]
                    && enabled(i)) {
                    play_se(-1, 9014, false, 255);
                    close_system_menu();
                    execute_menu_item(i);
                    break;
                }
            }
        }
    } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
        const float mx = event.motion.x; const float my = event.motion.y;
        // Same rule as the title menu: the highlight is whatever the cursor
        // is over, and nothing when it is over none of them.
        menu_highlight_ = -1;
        for (int i = 0; i < 5; ++i) {
            if (mx >= dst_x[i] && mx < dst_x[i] + dst_w[i]
                && my >= dst_y[i] && my < dst_y[i] + dst_h[i]
                && enabled(i)) {
                menu_highlight_ = i;
                break;
            }
        }
    }
    if (menu_highlight_ != previous_highlight && menu_highlight_ >= 0) {
        play_se(-1, 9108, false, 255);
    }
}

void Game::change_map_field(int direction)
{
    if (map_slide_ticks_ != 0 || map_fade_ticks_ != 0) {
        return;
    }
    map_previous_field_ = map_field_;
    do {
        map_field_ = (map_field_ + direction + 5) % 5;
    } while (!map_fields_[map_field_]);
    map_slide_ticks_ = direction > 0 ? 16 : -16;
    map_hover_ = -1;
    play_se(-1, 9015, false, 255);
}

// The engine's default: case - a destination is only taken, or lit, when
// it is on the page shown, whatever MUS_GetMouseNo answered.
bool Game::map_hover_on_page() const
{
    if (map_hover_ < 0
        || static_cast<std::size_t>(map_hover_) >= map_events_.size()) {
        return false;
    }
    const int position = map_events_[map_hover_].position;
    return position >= 0
        && position < static_cast<int>(map_positions_.size())
        && map_positions_[position].field == map_field_;
}

void Game::update_map_hover(float x, float y)
{
    // MUS_RenewMouse: the first flagged rect under the cursor, in slot
    // order - the destinations 0..15, then the arrows 16 and 17.  The
    // destination rects are the page's only once step 3 has narrowed them;
    // until then every destination on every page answers.
    map_hover_ = -1;
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
        if (i < map_rect_on_.size() && map_rect_on_[i]
            && x >= cx + 20 && x < cx + 150
            && y >= cy - 118 && y < cy) {
            map_hover_ = static_cast<int>(i);
            return;
        }
    }
    if (x >= 24.0f && x < 80.0f && y >= 239.0f && y < 361.0f) {
        map_hover_ = -2;
        return;
    }
    if (x >= 720.0f && x < 776.0f && y >= 239.0f && y < 361.0f) {
        map_hover_ = -3;
        return;
    }
}

void Game::handle_map_input(const SDL_Event& event)
{
    if (clock_state_ || map_enter_ticks_ != 0
        || map_enter_finished_this_frame_
        || map_slide_ticks_ != 0 || map_fade_ticks_ != 0) {
        return;
    }
    const int previous = map_hover_;
    if (event.type == SDL_EVENT_MOUSE_MOTION) {
        map_pointer_x_ = event.motion.x;
        map_pointer_y_ = event.motion.y;
        update_map_hover(event.motion.x, event.motion.y);
    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
               && event.button.button == SDL_BUTTON_LEFT) {
        update_map_hover(event.button.x, event.button.y);
        if (map_hover_ == -2 || map_hover_ == -3) {
            // The arrow's own AVG_PlaySE3( 9015 ), then the page turn's.
            play_se(-1, 9015, false, 255);
            map_arrow_pressed_ = map_hover_;
            change_map_field(map_hover_ == -2 ? 1 : -1);
        } else if (map_hover_ >= 0 && map_hover_on_page()) {
            finish_map_selection(map_hover_);
        }
    } else if (event.type == SDL_EVENT_KEY_DOWN) {
        if (event.key.key == SDLK_PAGEUP
            || event.key.key == SDLK_RIGHT) {
            change_map_field(1);
        } else if (event.key.key == SDLK_PAGEDOWN
                   || event.key.key == SDLK_LEFT) {
            change_map_field(-1);
        } else if (is_confirm_key(event.key.key) && map_hover_ >= 0
                   && map_hover_on_page()) {
            finish_map_selection(map_hover_);
        }
    }
    (void)previous;
}

void Game::update_map()
{
    if (ui_mode_ != UiMode::map) {
        return;
    }
    map_enter_finished_this_frame_ = false;
    // Frames, not elapsed time.  The slide and the fade were counted off
    // steady_clock, so their length in *ticks* depended on how fast the
    // engine happened to be running: at a trace's ~400 ticks a second one
    // sixtieth of a second is nearly seven ticks, and a sixteen frame
    // animation became a hundred and seven.  The map ate 263 of the 313
    // ticks it was open, where the engine spends 103 on the whole screen.
    // Fourth time this has turned up - after the audio fades, the choice
    // reveal and the clock - so: nothing a trace can see counts in seconds.
    for (int step = 0; step < control_steps_; ++step) {
        // The characters' sprites were set up in case 0 and animate from
        // there, through the clock and the fade-in, not from step 3.
        ++map_anim_frames_;
        map_marker_roll_ = 0;
        // Step 1 before step 2, and stepped here rather than in
        // update_clock_calendar: AVG_ControlMapEvent calls AVG_ViewClock
        // itself, from after EXEC_ControlLang, where update_clock_calendar
        // runs before the script pass.  A frame's difference in when the
        // clock finishes is a frame's difference in when the map appears.
        if (clock_state_) {
            clock_state_->frame += 1;
            if (clock_state_->frame >= 32 + clock_state_->travel_frames) {
                runtime_.set_flag(7, clock_state_->target);
                clock_state_.reset();
                // if( AVG_ViewClock( 19 ) ){ ... AVG_PlayBGM( 10, 30, ON,
                // 255, 0 ); ... } - the map's music comes in with the map,
                // on a 30 fade, not when the clock starts counting.
                map_sprite_start_ = map_anim_frames_;
                play_bgm(10, true, 255, 30);
                bgm_.set_gain(0.0f);
                bgm_.fade_to(bgm_gain(255), audio_fade_duration(30));
            }
            break;
        }
        if (map_enter_ticks_ > 0) {
            --map_enter_ticks_;
            if (map_enter_ticks_ == 0) {
                map_enter_finished_this_frame_ = true;
            }
            continue;
        }
        // Step 3, not rolling: select = MUS_GetMouseNo(-1) every frame, and
        //     if(select!=-1 && select != select_back ) AVG_PlaySE3( 9108 );
        //     ...
        //     select_back=select;
        // Asked of where the pointer IS, each frame - not only when it moves:
        // a pointer already resting on something when the map finishes
        // fading in is a change of select all the same.  select_back is a
        // function static in the engine, so it carries over between maps.
        if (map_slide_ticks_ == 0 && map_fade_ticks_ == 0
            && !map_finish_pending_) {
            // The non-roll branch resets every graph's source offset before
            // setting them from select: the pressed arrow is released here.
            map_arrow_pressed_ = 0;
            update_map_hover(map_pointer_x_, map_pointer_y_);
            if (map_hover_ != -1 && map_hover_ != map_select_back_) {
                play_se(-1, 9108, false, 255);
            }
            map_select_back_ = map_hover_;
            for (std::size_t i = 0; i < map_events_.size()
                 && i < map_rect_on_.size(); ++i) {
                const int position = map_events_[i].position;
                map_rect_on_[i] = position >= 0
                    && position < static_cast<int>(map_positions_.size())
                    && map_positions_[position].field == map_field_;
            }
        }
        // The markers' cross-fade reads EventFieldRoolCount before this
        // step takes one off it, the fields' slide after:
        //     cnt = 8-abs(EventFieldRoolCount); ...DRW_BLD(cnt*32)...
        //     EventFieldRoolCount--; cnt = EventFieldRoolCount*EventFieldRoolCount;
        map_marker_roll_ = std::abs(map_slide_ticks_);
        if (map_slide_ticks_ > 0) {
            --map_slide_ticks_;
        } else if (map_slide_ticks_ < 0) {
            ++map_slide_ticks_;
        }
        if (map_finish_pending_) {
            map_finish_pending_ = false;
            complete_map_selection();
            return;
        }
        if (map_fade_ticks_ > 0) {
            --map_fade_ticks_;
            if (map_fade_ticks_ == 0) {
                // Not this frame: the engine spends step 4 fading and step 5
                // releasing the map and loading the choice, so the load lands
                // one frame after the fade ends.
                map_finish_pending_ = true;
                // case 4, scnt==16: AVG_ResetBack(0); MainWindow.draw_flag=1;
                th2::set_draw_flag_on();
            }
        }
    }
}

void Game::draw_backlog()
{
    const bool authentic = font_.authentic();
    if (authentic) {
        begin_authentic_text();
    }

    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, message_backdrop_alpha());
    const SDL_FRect game_area{0.0f, 0.0f, 800.0f, 600.0f};
    SDL_RenderFillRect(renderer_, &game_area);

    std::string_view selected = message_.visible();
    const BacklogEntry* entry = nullptr;
    if (backlog_depth_ > 0 && backlog_depth_ <= static_cast<int>(backlog_.size())) {
        entry = &backlog_[
            backlog_.size() - static_cast<std::size_t>(backlog_depth_)];
        selected = entry->text;
    }

    const float x = message_text_x();
    float y = message_text_y();
    const auto lines = display_lines(selected);
    backlog_scroll_ = std::clamp(
        backlog_scroll_, 0, message_scroll_limit(lines.size()));
    for (std::size_t index = static_cast<std::size_t>(backlog_scroll_);
         index < lines.size(); ++index) {
        const auto& line = lines[index];
        if (authentic) {
            font_.draw_authentic_shadow(renderer_, x, y, line);
        } else {
            font_.draw(renderer_, x + 2.0f, y + 2.0f, line, 0, 0, 0);
        }
        font_.draw(renderer_, x, y, line, 255, 144, 32);
        y += text_line_height();
        if (y > message_bottom_y) {
            break;
        }
    }
    draw_scrollbar(lines.size(), backlog_scroll_, backlog_scroll_dragging_);
    if (entry && backlog_voice_hover_ >= 0
        && backlog_voice_hover_
            < static_cast<int>(entry->voices.size())) {
        const auto lines = display_lines(entry->text);
        for (const auto& rect :
             backlog_voice_rects(*entry, backlog_voice_hover_)) {
            const auto line = static_cast<std::size_t>(
                (rect.y - message_text_y()) / text_line_height())
                + static_cast<std::size_t>(backlog_scroll_);
            if (line >= lines.size()) {
                continue;
            }
            const SDL_Rect clip{
                static_cast<int>(std::floor(rect.x)),
                static_cast<int>(std::floor(rect.y)),
                static_cast<int>(std::ceil(rect.w)),
                static_cast<int>(std::ceil(rect.h))};
            SDL_SetRenderClipRect(renderer_, &clip);
            if (authentic) {
                font_.draw_authentic_shadow(
                    renderer_, x, rect.y, lines[line]);
            } else {
                font_.draw(
                    renderer_, x + 2.0f, rect.y + 2.0f,
                    lines[line], 0, 0, 0);
            }
            font_.draw(
                renderer_, x, rect.y, lines[line], 255, 255, 255);
            SDL_SetRenderClipRect(renderer_, nullptr);
        }
    }

    if (authentic) {
        select_overlay();
    }
}

std::string_view Game::backlog_view_text() const
{
    if (backlog_depth_ > 0
        && backlog_depth_ <= static_cast<int>(backlog_.size())) {
        return backlog_[
            backlog_.size() - static_cast<std::size_t>(backlog_depth_)].text;
    }
    return message_.visible();
}

std::vector<std::string> Game::backlog_view_lines() const
{
    return display_lines(backlog_view_text());
}

std::vector<SDL_FRect> Game::backlog_voice_rects(
    const Game::BacklogEntry& entry, int voice_index) const
{
    std::vector<SDL_FRect> result;
    if (voice_index < 0
        || voice_index >= static_cast<int>(entry.voices.size())) {
        return result;
    }
    const auto& voice = entry.voices[voice_index];
    const auto start = std::min(voice.start, entry.text.size());
    const auto end = std::clamp(voice.end, start, entry.text.size());
    std::size_t source_cursor = 0;
    float y = message_text_y()
        - static_cast<float>(backlog_scroll_) * text_line_height();
    for (const auto& line : display_lines(entry.text)) {
        auto line_start =
            std::string_view(entry.text).find(line, source_cursor);
        if (line_start == std::string_view::npos) {
            line_start = source_cursor;
        }
        const auto line_end = line_start + line.size();
        const auto overlap_start = std::max(start, line_start);
        const auto overlap_end = std::min(end, line_end);
        if (overlap_start < overlap_end) {
            const auto prefix = std::string_view(line).substr(
                0, overlap_start - line_start);
            const auto voiced = std::string_view(line).substr(
                0, overlap_end - line_start);
            const float left =
                message_text_x() + font_.text_width(prefix);
            const float right =
                message_text_x() + font_.text_width(voiced);
            result.push_back(
                {left, y, right - left, text_line_height()});
        }
        source_cursor = line_end;
        y += text_line_height();
    }
    return result;
}

// The lower track in sys0000.tga sets how opaque the message backdrop is.
// Its artwork is a preview of the effect: transparent at the top, solid at
// the bottom, so the handle position maps straight onto the alpha.
namespace {
constexpr float opacity_track_top = 492.0f;
constexpr float opacity_track_height = 98.0f;
// sys0001.tga row 9 (y=257, 6 px tall) is the flat handle this bar uses;
// the 30 px capsule at the top of the sheet belongs to the scrollbar.
constexpr float opacity_handle_source_y = 257.0f;
constexpr float opacity_handle_height = 6.0f;

// GM_AvgMsg.cpp positions this handle at RectY[9] + (128 - half_tone), so
// the top of the track is the untouched background and the bottom is the
// darkest setting.
float opacity_handle_y(int half_tone)
{
    const int offset = std::clamp(
        th2::GameConfig::max_message_half_tone - half_tone, 0,
        static_cast<int>(opacity_track_height - opacity_handle_height));
    return opacity_track_top + static_cast<float>(offset);
}
}  // namespace

// NovelBuf.bmax.  SetNovelMessageHistory runs inside AVG_SetNovelMessage, so
// the engine's log gains its entry the moment a line is set and the line on
// screen is always already counted; AVG_AddNovelMessage appends to that same
// entry rather than making a new one.  Ours pushes the finished text instead,
// on the way into the *next* SetMessage2, so the line being read is the one
// entry the vector does not hold yet.
int Game::novel_log_depth() const
{
    return static_cast<int>(backlog_.size()) + (message_.empty() ? 0 : 1);
}

void Game::draw_sidebar()
{
    sidebar_layer_used_ = true;     // composited and cleared next frame
    if (!ui_sidebar_track_ || !ui_sidebar_btns_) return;

    // AVG_ControlHistorySystem's fade, on DRW_BLD's 0..256 scale rather than
    // SDL's 0..255 - the whole bar is composited through the rasteriser's
    // blend table below, and that table is indexed by the engine's number.
    //
    //     case 0: if( MUS_GetMousePosX()<DISP_X-24 && step1!=MSG_DRAG )
    //                 fade = LIM(fade-24,64,256);
    //             else    fade = LIM(fade+24,64,256);
    //
    // Fade by elapsed time rather than by frame, the same way the sakura
    // petals step: the browser build renders at the display refresh rate,
    // which is often not 60 Hz.  A stall is clamped so the fade cannot jump.
    const auto now = engine_now();
    // In a trace run the engine's own count: once per control pass, and
    // MUS_GetMousePosX() is the scripted pointer.  Elapsed time there is
    // 16 ms a tick - 0.96 of a sixtieth - so the fade moved 23.04 a frame,
    // and the pointer it asked about was SDL's, which a trace never moves.
    // The steady route parks at x=400 with the bar pinned at 64, so neither
    // showed until a recorded run put the pointer on the bar.
    const float steps = trace_mode_
        ? static_cast<float>(control_steps_)
        : std::clamp(
              static_cast<float>(
                  std::chrono::duration<double>(now - sidebar_alpha_updated_)
                      .count()
                  * 60.0),
              0.0f, 8.0f);
    sidebar_alpha_updated_ = now;
    // MSG_DRAG holds the bar up while the log handle is being pulled, which
    // is the one way the pointer can be over it and still want it opaque.
    const bool near = trace_mode_
        ? trace_mouse_x_ >= th2::display_width - 24
        : sidebar_mouse_near_;
    const bool rising = near || backlog_handle_dragging_;
    switch (config_.sidebar_mode) {
    case 0:
        sidebar_alpha_ = std::clamp(
            sidebar_alpha_ + steps * (rising ? 24.0f : -24.0f),
            64.0f, 256.0f);
        break;
    case 1:
        sidebar_alpha_ = 256.0f;
        break;
    case 2:
        sidebar_alpha_ = std::clamp(
            sidebar_alpha_ + steps * (rising ? 32.0f : -32.0f),
            0.0f, 256.0f);
        break;
    default:
        sidebar_alpha_ = 0.0f;
        break;
    }
    if (msg().engine_bar()) {
        // The engine's own bar: ControlHistorySystem has already set up all
        // eleven planes and the fade this frame, from the machine's state.
        sidebar_alpha_ = static_cast<float>(msg().history_bar().fade);
    }
    if (sidebar_alpha_ <= 0.0f) {
        return;
    }
    const int fade = static_cast<int>(sidebar_alpha_);

    // Every one of the eleven GRP_HISTORY planes carries DRW_BLD(fade), so
    // the bar is composited the way the click indicator is: through Draw32's
    // blend table, which truncates at /256, and not through SDL's blend,
    // which rounds at /255.  Over a 30x600 strip that difference was a level
    // or two on every pixel of it.
    auto blit = [&](SDL_Texture* texture, const SDL_FRect& src,
                    const SDL_FRect& dst) {
        auto* const exact = display_->gl_exact_blend();
        if (exact && exact->available()
            && exact->capture_destination(renderer_)
            && exact->draw(renderer_, texture, src, dst, false, false, 3,
                           fade, th2::bright_neutral, th2::bright_neutral,
                           th2::bright_neutral)) {
            return;
        }
        SDL_SetTextureAlphaMod(
            texture, static_cast<std::uint8_t>(std::min(fade, 255)));
        SDL_RenderTexture(renderer_, texture, &src, &dst);
        SDL_SetTextureAlphaMod(texture, 255);
    };

    if (msg().engine_bar()) {
        // GRP_HISTORY+0 is BMP_HISTORY+0 (sys0000), the rest BMP_HISTORY+1
        // (sys0001).
        const auto& graphs = msg().history_bar().g;
        for (std::size_t i = 0; i < graphs.size(); ++i) {
            const auto& g = graphs[i];
            const auto to_rect = [](int x, int y, int w, int h) {
                return SDL_FRect{static_cast<float>(x), static_cast<float>(y),
                                 static_cast<float>(w), static_cast<float>(h)};
            };
            blit(i == 0 ? ui_sidebar_track_.get() : ui_sidebar_btns_.get(),
                 to_rect(g.sx, g.sy, g.w, g.h), to_rect(g.dx, g.dy, g.w, g.h));
        }
        return;
    }

    // sys0000.tga is the complete 30x600 sidebar backing.
    const SDL_FRect sidebar_dst{770.0f, 0.0f, 30.0f, 600.0f};
    blit(ui_sidebar_track_.get(), SDL_FRect{0.0f, 0.0f, 30.0f, 600.0f},
         sidebar_dst);

    // sys0001.tga stores disabled, normal, hover and pressed states
    // in four 22-pixel-wide columns.
    {
        // AVG_ControlHistorySystem's DragBarY, integer division and all:
        //
        //     if(NovelBuf.bmax>=2)
        //         DragBarY = RectY[0]
        //                  + (bmax-1-bcount)*(RectH[0]-(SrcH[0]+1))/(bmax-1);
        //     else
        //         DragBarY = RectY[0]+RectH[0]-(SrcH[0]+1);
        //
        // The handle sits at the *bottom* on the newest line and climbs as
        // the log is paged back, so the numerator counts down from bmax-1.
        const int depth = backlog_depth_;
        const int bmax = novel_log_depth();
        const int handle_y = bmax >= 2
            ? 10 + (bmax - 1 - depth) * 224 / (bmax - 1)
            : 10 + 224;
        const float handle_state = backlog_handle_dragging_ ? 3.0f
            : (backlog_handle_hover_ && bmax > 1) ? 2.0f
            : bmax > 1 ? 1.0f : 0.0f;
        const SDL_FRect hdl_src{
            handle_state * 22.0f, 0.0f, 22.0f, 30.0f};
        const SDL_FRect hdl_dst{
            776.0f, static_cast<float>(handle_y), 22.0f, 30.0f};
        blit(ui_sidebar_btns_.get(), hdl_src, hdl_dst);
    }

    {
        const float state = opacity_handle_dragging_ ? 3.0f
            : opacity_handle_hover_ ? 2.0f : 1.0f;
        const SDL_FRect src{
            state * 22.0f, opacity_handle_source_y,
            22.0f, opacity_handle_height};
        const SDL_FRect dst{
            776.0f, opacity_handle_y(config_.message_half_tone),
            22.0f, opacity_handle_height};
        blit(ui_sidebar_btns_.get(), src, dst);
    }

    struct SBBtn { int y; int source_y; int h; };
    const SBBtn btns[] = {
        {271, 36, 36},   // PageUp
        {312, 77, 36},   // PageDown
        {353, 118, 20},  // Save
        {376, 141, 20},  // Load
        {399, 164, 20},  // Auto
        {422, 187, 20},  // Skip
        {445, 210, 20},  // Settings
        {468, 233, 20},  // QuickSave
    };

    // Which column each button shows, from AVG_ControlHistorySystem.  Only
    // PageUp and PageDown are lit unconditionally; everything below them is
    // dead unless the message machine is parked (MSG_WAIT / MSG_STOP /
    // MSG_DISP / MSG_NEXT), because that is the only time a click on them
    // would be answered.  Getting this wrong is invisible at a glance - the
    // disabled column is the same icon in a duller ink - and it was a 20-odd
    // level difference over two buttons in the pixel harness.
    const int step1 = msg().state().step1;
    const bool parked = step1 == th2::msg_wait || step1 == th2::msg_stop
        || step1 == th2::msg_disp || step1 == th2::msg_next;
    // AVG_GetMesCut's own gate, minus the key and the toggle: with "skip
    // unread" off, skipping is only offered on a line already read.
    const bool cut_offered = config_.skip_unread || current_text_is_read();
    const int bmax = novel_log_depth();
    for (int i = 0; i < static_cast<int>(std::size(btns)); ++i) {
        const auto& button = btns[i];
        const bool disabled =
            i == 0 ? bmax - 1 <= backlog_depth_     // nothing older to show
            : i == 1 ? backlog_depth_ <= 0          // already at the newest
            : !parked || (replay_mode_ && (i == 2 || i == 3))
                || (i == 5 && !cut_offered);
        const bool active =
            (i == 4 && auto_mode_) || (i == 5 && skip_mode_);
        const float state_x = disabled ? 0.0f : active ? 66.0f
            : (i == sidebar_hover_ ? 44.0f : 22.0f);
        const SDL_FRect src{
            state_x, static_cast<float>(button.source_y),
            22.0f, static_cast<float>(button.h)};
        const SDL_FRect dst{
            776.0f, static_cast<float>(button.y),
            22.0f, static_cast<float>(button.h)};
        blit(ui_sidebar_btns_.get(), src, dst);
    }
}

void Game::update_sidebar_hover(float x, float y)
{
    sidebar_mouse_near_ = x >= 776.0f;
    backlog_handle_hover_ = false;
    opacity_handle_hover_ = false;
    const int previous_hover = sidebar_hover_;
    sidebar_hover_ = -1;
    if (config_.sidebar_mode == 3) {
        return;
    }
    if (x < 776.0f || x >= 798.0f) {
        return;
    }
    // MUS_GetMouseNoEx( -1, 0 ) is the history bar's rect under the pointer,
    // and rect 0 is the whole track, (776, 10, 22, 255) - not the handle.
    // ControlHistorySystem's `case 0` lights the handle for any of it, so the
    // handle comes up the moment the pointer is on the track, wherever the
    // handle happens to sit.
    if (!backlog_.empty()) {
        backlog_handle_hover_ = y >= 10.0f && y < 10.0f + 255.0f;
    }
    opacity_handle_hover_ =
        y >= opacity_track_top
        && y < opacity_track_top + opacity_track_height;
    static constexpr std::array button_y{
        271, 312, 353, 376, 399, 422, 445, 468,
    };
    static constexpr std::array button_h{
        36, 36, 20, 20, 20, 20, 20, 20,
    };
    for (int i = 0; i < static_cast<int>(button_y.size()); ++i) {
        if (y >= button_y[i] && y < button_y[i] + button_h[i]) {
            if (replay_mode_ && (i == 2 || i == 3)) {
                return;
            }
            sidebar_hover_ = i;
            if (sidebar_hover_ != previous_hover) {
                play_se(-1, 9108, false, 255);
            }
            return;
        }
    }
}

bool Game::handle_sidebar_click(float x, float y, bool activate_buttons)
{
    if (config_.sidebar_mode == 3) {
        return false;
    }
    if (x < 776.0f || x >= 798.0f) {
        return false;
    }
    if (y >= 10.0f && y < 265.0f && !backlog_.empty()) {
        backlog_handle_dragging_ = true;
        set_backlog_from_sidebar_y(y);
        return true;
    }
    if (y >= opacity_track_top
        && y < opacity_track_top + opacity_track_height) {
        opacity_handle_dragging_ = true;
        set_message_alpha_from_sidebar_y(y);
        return true;
    }

    struct Hitbox { int y; int h; };
    static constexpr Hitbox buttons[] = {
        {271, 36}, {312, 36}, {353, 20}, {376, 20},
        {399, 20}, {422, 20}, {445, 20}, {468, 20},
    };
    for (int i = 0; i < static_cast<int>(std::size(buttons)); ++i) {
        if (y < buttons[i].y || y >= buttons[i].y + buttons[i].h) {
            continue;
        }
        if (!activate_buttons) {
            return true;
        }
        switch (i) {
        case 0:
            backlog_older();
            break;
        case 1:
            if (!backlog_newer()) {
                advance();
            }
            break;
        case 2:
            if (replay_mode_) break;
            play_se(-1, 9104, false, 255);
            save_snapshot_ = capture_frame_thumbnail(
    save_thumbnail_width, save_thumbnail_height);
            open_save_load(UiMode::save);
            break;
        case 3:
            if (replay_mode_) break;
            play_se(-1, 9104, false, 255);
            save_snapshot_ = capture_frame_thumbnail(
    save_thumbnail_width, save_thumbnail_height);
            open_save_load(UiMode::load);
            break;
        case 4:
            play_se(-1, 9104, false, 255);
            auto_mode_ = !auto_mode_;
            if (auto_mode_) skip_mode_ = false;
            break;
        case 5:
            play_se(-1, 9104, false, 255);
            skip_mode_ = !skip_mode_;
            if (skip_mode_) auto_mode_ = false;
            break;
        case 6:
            play_se(-1, 9104, false, 255);
            window_hidden_ = !window_hidden_;
            break;
        case 7:
            play_se(-1, 9104, false, 255);
            open_config();
            break;
        }
        return true;
    }
    return true;
}

void Game::set_message_alpha_from_sidebar_y(float y)
{
    // Dragging the handle counts as being on the bar: without this the fade
    // keeps running while the finger works the slider, because the motion
    // handler goes straight to the drag and never updates the hover.
    sidebar_mouse_near_ = true;
    const int travel = static_cast<int>(
        opacity_track_height - opacity_handle_height);
    const int offset = std::clamp(
        static_cast<int>(std::lround(
            y - opacity_track_top - opacity_handle_height / 2.0f)),
        0, travel);
    config_.message_half_tone =
        th2::GameConfig::max_message_half_tone - offset;
}

void Game::finish_sidebar_drag()
{
    backlog_handle_dragging_ = false;
    message_scroll_dragging_ = false;
    backlog_scroll_dragging_ = false;
    if (opacity_handle_dragging_) {
        opacity_handle_dragging_ = false;
        th2::save_config(config_path_, config_);
    }
}

void Game::set_backlog_from_sidebar_y(float y)
{
    sidebar_mouse_near_ = true;  // see set_message_alpha_from_sidebar_y()
    if (backlog_.empty()) {
        backlog_depth_ = 0;
        ui_mode_ = UiMode::game;
        return;
    }
    constexpr float track_top = 10.0f;
    constexpr float handle_height = 30.0f;
    constexpr float track_height = 255.0f;
    const float handle_y = std::clamp(
        y - handle_height / 2.0f,
        track_top, track_top + track_height - handle_height);
    const float ratio =
        (handle_y - track_top) / (track_height - handle_height);
    backlog_depth_ = std::clamp(
        static_cast<int>(std::lround(
            (1.0f - ratio) * static_cast<float>(backlog_.size()))),
        0, static_cast<int>(backlog_.size()));
    backlog_voice_hover_ = -1;
    backlog_scroll_ = 0;
    ui_mode_ = backlog_depth_ == 0 ? UiMode::game : UiMode::backlog;
}

void Game::handle_backlog_input(const SDL_Event& event)
{
    if (event.type == SDL_EVENT_KEY_DOWN) {
        if (event.key.key == SDLK_ESCAPE) {
            close_backlog();
        } else if (event.key.key == SDLK_UP) {
            backlog_older();
        } else if (event.key.key == SDLK_DOWN) {
            backlog_newer();
        } else if (event.key.key == SDLK_PAGEUP) {
            backlog_older();
        } else if (event.key.key == SDLK_PAGEDOWN) {
            backlog_newer();
        } else if (event.key.key == SDLK_HOME) {
            if (backlog_depth_ != static_cast<int>(backlog_.size())) {
                play_se(-1, 9012, false, 140);
                backlog_depth_ = static_cast<int>(backlog_.size());
            }
        } else if (event.key.key == SDLK_END) {
            close_backlog();
        }
    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        if (event.button.button == SDL_BUTTON_RIGHT) {
            close_backlog();
            return;
        }
        if (handle_sidebar_click(event.button.x, event.button.y)) {
            suppress_sidebar_mouse_up_ = true;
            return;
        }
        if (handle_backlog_scroll_press(event.button.x, event.button.y)) {
            suppress_sidebar_mouse_up_ = true;
            return;
        }
    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        if (event.button.button == SDL_BUTTON_LEFT) {
            // A press on the sidebar scroll handle or one of its buttons
            // switches to backlog mode, so its release arrives here rather
            // than in the game-mode handler.  Releasing a scroll drag or a
            // scroll button must not count as a click on the backlog.
            if (suppress_sidebar_mouse_up_) {
                suppress_sidebar_mouse_up_ = false;
                finish_sidebar_drag();
                return;
            }
            if (backlog_voice_hover_ >= 0
                && backlog_depth_ > 0
                && backlog_depth_
                    <= static_cast<int>(backlog_.size())) {
                const auto& entry = backlog_[
                    backlog_.size()
                    - static_cast<std::size_t>(backlog_depth_)];
                if (backlog_voice_hover_
                    < static_cast<int>(entry.voices.size())) {
                    replay_backlog_voice(
                        entry.voices[backlog_voice_hover_]);
                    return;
                }
            }
            close_backlog();
        }
        finish_sidebar_drag();
    } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
        if (event.wheel.y > 0) {
            backlog_older();
        } else if (event.wheel.y < 0) {
            backlog_newer();
        }
    } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
        if (backlog_handle_dragging_) {
            set_backlog_from_sidebar_y(event.motion.y);
            return;
        }
        if (opacity_handle_dragging_) {
            set_message_alpha_from_sidebar_y(event.motion.y);
            return;
        }
        if (backlog_scroll_dragging_) {
            set_backlog_scroll_from_y(event.motion.y);
            return;
        }
        update_sidebar_hover(event.motion.x, event.motion.y);
        backlog_voice_hover_ = -1;
        if (event.motion.x < 770.0f && backlog_depth_ > 0
            && backlog_depth_
                <= static_cast<int>(backlog_.size())) {
            const auto& entry = backlog_[
                backlog_.size()
                - static_cast<std::size_t>(backlog_depth_)];
            for (int i = 0;
                 i < static_cast<int>(entry.voices.size()); ++i) {
                for (const auto& rect :
                     backlog_voice_rects(entry, i)) {
                    if (event.motion.x >= rect.x
                        && event.motion.x < rect.x + rect.w
                        && event.motion.y >= rect.y
                        && event.motion.y < rect.y + rect.h) {
                        backlog_voice_hover_ = i;
                        break;
                    }
                }
                if (backlog_voice_hover_ >= 0) {
                    break;
                }
            }
        }
    }
}


}  // namespace th2app
