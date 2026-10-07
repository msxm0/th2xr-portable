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

template <typename Draw>
void Game::draw_save_layer(int alpha, Draw&& draw)
{
    if (alpha <= 0) {
        return;
    }
    if (alpha >= 256) {
        draw();
        return;
    }
    if (!save_layer_target_) {
        save_layer_target_.reset(SDL_CreateTexture(
            renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET,
            800, 600));
        if (!save_layer_target_) {
            draw();
            return;
        }
        // Drawn with the ordinary blend over transparent black, which leaves
        // the layer premultiplied.
        SDL_SetTextureBlendMode(save_layer_target_.get(),
                                SDL_BLENDMODE_BLEND_PREMULTIPLIED);
    }
    SDL_Texture* const held = SDL_GetRenderTarget(renderer_);
    SDL_SetRenderTarget(renderer_, save_layer_target_.get());
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
    SDL_RenderClear(renderer_);
    draw();
    SDL_SetRenderTarget(renderer_, held);
    const float a = static_cast<float>(alpha) / 256.0f;
    SDL_SetTextureColorModFloat(save_layer_target_.get(), a, a, a);
    SDL_SetTextureAlphaModFloat(save_layer_target_.get(), a);
    SDL_RenderTexture(renderer_, save_layer_target_.get(), nullptr, nullptr);
}

void Game::draw_save_load()
{
    // GWIN_ControlSaveLoadWindow's graphs: below half way the frame and the
    // slot numbers come up together at DRW_BLD(rate*2), the slots' contents
    // not yet there; above it the frame is solid and the contents come up
    // at DRW_BLD((rate-128)*2).  The question fades in over all of it at
    // rate2, and once it is fully up it is all that is left of the window
    // but its buttons.
    const int rate = save_window_.flag || save_window_.load_flag
        ? save_window_.rate : 256;
    const int rate2 = save_confirm_slot_ >= 0 ? save_window_.rate2 : 0;
    if (rate < 128) {
        draw_save_layer(rate * 2, [this] { draw_save_load_frame(); });
    } else if (rate2 < 256) {
        draw_save_load_frame();
        draw_save_layer(rate2 == 0 ? (rate - 128) * 2 : 256,
                        [this] { draw_save_load_slots(); });
    }
    if (rate2 >= 256) {
        draw_save_load_buttons();
    }
    if (rate2 > 0) {
        draw_save_layer(rate2, [this] { draw_save_load_prompt(); });
    }
    if (!load_error_.empty()) {
        font_.draw_save_menu(
            renderer_, 21.0f, 571.0f, load_error_, 0, 0, 0);
        font_.draw_save_menu(
            renderer_, 20.0f, 570.0f, load_error_, 255, 80, 80);
    }
}

void Game::draw_save_load_frame()
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
        draw_save_digit_number(x + 98.0f, y + 10.0f,
                               save_page_ * 10 + i + 1, 3);
    }

    constexpr int total_pages = 11;
    draw_save_digit_sheet_text(
        364.0f, 78.0f,
        std::format("{:02d}/{:02d}", save_page_ + 1, total_pages));
    draw_save_load_buttons();
}

void Game::draw_save_load_buttons()
{
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
}

void Game::draw_save_load_slots()
{
    for (int i = 0; i < 10; ++i) {
        const float x = 16.0f + 390.0f * (i / 5);
        const float y = 112.0f + 76.0f * (i % 5);
        if (save_thumbnails_[i]) {
            const SDL_FRect thumb{x + 15.0f, y + 5.0f, 80.0f, 60.0f};
            SDL_RenderTexture(
                renderer_, save_thumbnails_[i].get(), nullptr, &thumb);
        }

        const int slot = save_page_ * 10 + i;
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
}

void Game::draw_save_load_prompt()
{
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

namespace {

// SAVE_WINDOW.mode.
enum SaveWindowMode {
    sav_not,
    sav_open,
    sav_close,
    sav_load_open,
    sav_load_close,
    sav_load_check,
    sav_normal,
};

}  // namespace

void Game::gwin_renew_save_load_window()
{
    // void GWIN_RenewSaveLoadWindow( void ): the page's ten slots read again,
    // and a rect for each one that can be taken - every slot when saving,
    // only the ones with a file in them when loading.
    save_page_ = save_window_.page;
    refresh_save_page();
    auto& m = msg();
    for (int i = 0; i < 10; ++i) {
        if (visible_saves_[i].exists || !save_window_.load) {
            m.set_mouse_rect(save_window_.mouse_layer, i,
                             16 + 390 * (i / 5), 112 + 76 * (i % 5), 380, 74,
                             true);
        } else {
            m.set_mouse_rect(save_window_.mouse_layer, i, 0, 0, 0, 0, false);
        }
    }
}

void Game::gwin_set_save_load_window(bool load, int mouse_layer,
                                     int mouse_layer_back, bool title_load)
{
    // void GWIN_SetSaveLoadWindow( type, ..., mus_lay, mus_bak ).
    save_window_ = {};
    save_window_.flag = true;
    save_window_.load = load;
    save_window_.title_load = title_load;
    save_window_.cnt = 0;
    save_window_.mode = sav_open;
    save_window_.mouse_layer = mouse_layer;
    save_window_.mouse_layer_back = mouse_layer_back;
    // The newest save by its own time stamp, and the page it is on.  The
    // engine walks its hundred slots; ours has the autosaves' page past them.
    save_window_.new_no = 0;
    std::time_t newest = 0;
    for (int slot = 0; slot < 110; ++slot) {
        const auto metadata = read_save_metadata(slot);
        if (metadata.exists && metadata.timestamp > newest) {
            newest = metadata.timestamp;
            save_window_.new_no = slot;
        }
    }
    save_window_.page = save_window_.new_no >= 100
        ? 10 : save_window_.new_no / 10;
    auto& m = msg();
    m.set_mouse_layer(mouse_layer);
    m.set_mouse_rect(mouse_layer, 10, 190, 72, 130, 32, true);
    m.set_mouse_rect(mouse_layer, 11, 482, 72, 130, 32, true);
    m.set_mouse_rect(mouse_layer, 12, 306, 496, 188, 32, true);
    gwin_renew_save_load_window();
    save_confirm_slot_ = -1;
    save_hover_ = -1;
    load_error_.clear();
    th2::set_draw_flag_on();
}

void Game::gwin_reset_save_load_window()
{
    // void GWIN_ResetSaveLoadWindow( void ).
    auto& m = msg();
    if (save_window_.save_flag) {
        save_window_.save_flag = false;
        m.reset_mouse_rect_layer(save_window_.mouse_layer);
    } else {
        m.reset_mouse_rect_layer(save_window_.mouse_layer);
        m.set_mouse_layer(save_window_.mouse_layer_back);
    }
    save_window_.flag = false;
}

bool Game::gwin_set_save_load_check(int select)
{
    // int GWIN_SetSaveLoadCheck( int select ): an empty slot is saved into
    // straight away; anything else asks first.
    const int slot = select + save_window_.page * 10;
    if (!save_window_.load && !visible_saves_[select].exists) {
        save(slot);                                 // AVG_SetSave
        save_window_.new_no = slot;
        gwin_renew_save_load_window();
        return false;
    }
    if (save_window_.load && !save_loadable(slot)) {
        // The port's: a file from a newer or much older build.  The window
        // stays as it is.
        load_error_ = "Incompatible save version.";
        return true;
    }
    load_error_.clear();
    save_window_.select = slot;
    save_window_.cnt = 0;
    save_window_.mode = sav_load_open;
    auto& m = msg();
    m.set_mouse_rect(save_window_.mouse_layer + 1, 0, 461, 320, 130, 32,
                     true);
    m.set_mouse_rect(save_window_.mouse_layer + 1, 1, 606, 320, 130, 32,
                     true);
    m.set_mouse_layer(save_window_.mouse_layer + 1);
    return true;
}

void Game::gwin_reset_save_load_check()
{
    // void GWIN_ResetSaveLoadCheck( void ).
    auto& m = msg();
    m.reset_mouse_rect_layer(save_window_.mouse_layer + 1);
    m.set_mouse_layer(save_window_.mouse_layer);
}

int Game::gwin_control_save_load_window()
{
    // int GWIN_ControlSaveLoadWindow( void ).  -1 nothing, 0 closed, 1 the
    // window is less than half faded in, 2 more, -2 a load is under way.
    auto& w = save_window_;
    if (!w.flag) {
        return -1;
    }
    auto& m = msg();
    int cmax = effect_frames(-1);
    int select = m.mouse_no_ex(th2::mouse_any, w.mouse_layer);
    bool click = game_key_.click != 0;
    const bool cansel = game_key_.cansel != 0;
    int rate = 256;
    int rate2 = 0;
    switch (w.mode) {
    case sav_not:
        break;
    case sav_open:
        w.cnt++;
        if (w.cnt >= cmax) {
            w.mode = sav_normal;
            rate = 256;
        } else {
            rate = 256 * w.cnt / cmax;
        }
        break;
    case sav_close:
        w.cnt++;
        if (w.cnt >= cmax) {
            w.mode = sav_not;
            w.rate = 0;
            gwin_reset_save_load_window();
            return 0;
        }
        rate = 256 - 256 * w.cnt / cmax;
        break;
    case sav_load_open:
        cmax = effect_frames(8);
        w.cnt++;
        if (w.cnt >= cmax) {
            w.mode = sav_load_check;
        }
        rate2 = cmax ? 256 * w.cnt / cmax : 256;
        break;
    case sav_load_close:
        cmax = effect_frames(8);
        w.cnt++;
        if (w.cnt >= cmax) {
            w.mode = sav_normal;
            gwin_reset_save_load_check();
            gwin_renew_save_load_window();
        }
        rate2 = cmax ? 256 - 256 * w.cnt / cmax : 0;
        break;
    case sav_load_check:
        rate2 = 256;
        select = m.mouse_no_ex(th2::mouse_any, w.mouse_layer + 1);
        if (game_key_.l) {
            m.set_mouse_pos_rect(w.mouse_layer + 1, 0);
        }
        if (game_key_.r) {
            m.set_mouse_pos_rect(w.mouse_layer + 1, 1);
        }
        if (!w.load_flag) {
            if (click) {
                if (select == 0) {
                    play_system_se(9104, 255);
                    if (!w.load) {
                        w.new_no = w.select;
                        save(w.select);                   // AVG_SetSave
                        engine_reset_config_window();     // AVG_ResetConfigWindow
                        engine_config_open_mode_ = 1;     // ConfigOpenMode=1
                        w.save_flag = true;
                        w.mode = sav_close;
                        w.cnt = 0;
                    } else {
                        w.flag = false;
                        w.load_flag = true;
                        begin_load(w.select);             // AVG_SetLoad
                        stop_bgm(60);                     // AVG_StopBGM( FADE_MUS )
                        gwin_reset_save_load_window();
                        engine_reset_config_window();
                        w.rate = 256;
                        return -2;
                    }
                } else if (select == 1) {
                    play_system_se(9104, 255);
                    w.cnt = 0;
                    w.mode = sav_load_close;
                }
            } else if (cansel) {
                play_system_se(9107, 255);
                w.cnt = 0;
                w.mode = sav_load_close;
            }
        }
        break;
    case sav_normal:
        if (game_key_.pup) {
            select = 10;
            click = true;
        }
        if (game_key_.pdown) {
            select = 11;
            click = true;
        }
        if (select != -1 && click) {
            th2::set_draw_flag_on();
            switch (select) {
            default:
                play_system_se(9104, 255);
                if (!gwin_set_save_load_check(select)) {
                    engine_reset_config_window();
                    engine_config_open_mode_ = 1;
                    w.save_flag = true;
                    w.mode = sav_close;
                    w.cnt = 0;
                }
                break;
            case 10:
            case 11: {
                play_system_se(9104, 255);
                // Ten pages in the engine; ours has the autosaves' after.
                constexpr int page_count = 11;
                w.page = (w.page + (select == 10 ? page_count - 1 : 1))
                    % page_count;
                gwin_renew_save_load_window();
                break;
            }
            case 12:
                play_system_se(9104, 255);
                w.mode = sav_close;
                w.cnt = 0;
                break;
            }
        }
        if (cansel) {
            play_system_se(9107, 255);
            w.mode = sav_close;
            w.cnt = 0;
        }
        break;
    }
    w.rate = rate;
    w.rate2 = rate2;
    // What draw_save_load shows: the rect under the pointer, the slot being
    // asked about.
    if (w.mode == sav_load_open || w.mode == sav_load_check
        || w.mode == sav_load_close) {
        save_confirm_slot_ = w.select;
        save_hover_ = w.mode == sav_load_check && select >= 0 ? 13 + select
                                                              : -1;
    } else {
        save_confirm_slot_ = -1;
        save_hover_ = w.mode == sav_normal ? select : -1;
    }
    return rate < 128 ? 1 : 2;
}

void Game::engine_open_config_window()
{
    // void AVG_OpenConfigWindow( void ).
    const int g = th2::grp_system;
    for (int i = 0; i < 7; ++i) {
        display().set_graph_disp(g + i, true);
    }
    auto& m = msg();
    m.set_mouse_rect_flag(1, 0, !replay_mode_);
    m.set_mouse_rect_flag(1, 1, !replay_mode_);
    m.set_mouse_rect_flag(1, 2, !engine_config_from_map_);
    m.set_mouse_rect_flag(1, 3, true);
    m.set_mouse_rect_flag(1, 4, true);
}

void Game::control_system()
{
    // void AVG_ControlSystem( void ).
    if (engine_config_.flag) {
        control_engine_config();            // AVG_ControlConfigWindow
    }
    const int mode = gwin_control_save_load_window();
    if (mode == 0) {
        ui_mode_ = save_return_mode_;
        if (save_window_.title_load) {
            return;                         // the title's own, not the engine's
        }
    }
    switch (mode) {
    case 0:
        engine_port_screen_closed();
        break;
    case 1:
        if (save_window_.title_load) {
            break;
        }
        if (engine_config_open_mode_ == 0) {
            engine_open_config_window();
        } else {
            engine_open_back();
        }
        break;
    case 2:
        if (!save_window_.title_load && engine_config_open_mode_ != 0) {
            engine_close_back();
        }
        break;
    default:
        break;
    }
}

void Game::handle_save_load_input(const SDL_Event& event)
{
    // The window itself answers GameKey (see gwin_control_save_load_window),
    // which the sampler fills from the mouse, Enter, Escape and the page
    // keys like any other.  The arrows are the port's: they put the pointer
    // on the next rect, which the engine then reads as the pointer.
    if (event.type != SDL_EVENT_KEY_DOWN || !save_window_.flag
        || save_window_.mode != sav_normal) {
        return;
    }
    if (event.key.key == SDLK_UP || event.key.key == SDLK_DOWN
        || event.key.key == SDLK_LEFT || event.key.key == SDLK_RIGHT) {
        move_save_load_focus(event.key.key);
        if (save_hover_ >= 0) {
            msg().set_mouse_pos_rect(save_window_.mouse_layer, save_hover_);
        }
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

void Game::map_clock_done()
{
    // if( AVG_ViewClock( 19 ) ){ ...DSP_SetSprite...; AVG_PlayBGM( 10, 30,
    // ON, 255, 0 ); ... } - the characters start animating and the map's
    // music comes in with the map, on a 30 fade, not when the clock starts
    // counting.  AVG_PlayBGM leaves a track that is already playing alone.
    map_sprite_start_ = map_anim_frames_;
    const bool fresh = bgm_track_ != 10;
    play_bgm(10, true, 255, 30);
    if (fresh) {
        bgm_.set_gain(0.0f);
        bgm_.fade_to(bgm_gain(255), audio_fade_duration(30));
    }
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
                map_clock_done();
            }
            break;
        }
        // AVG_ViewClock( 19 ) with the clock already there - a map loaded
        // from a save - answers TRUE on its first call, the frame the clock
        // would have started on.
        if (map_clock_instant_ > 0) {
            if (--map_clock_instant_ == 0) {
                map_clock_done();
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

void Game::draw_sidebar()
{
    sidebar_layer_used_ = true;     // composited and cleared next frame
    if (!ui_sidebar_track_ || !ui_sidebar_btns_) return;
    // The engine's bar: ControlHistorySystem has already set up all eleven
    // planes and the fade this frame, from the machine's state, on DRW_BLD's
    // 0..256 scale.
    // Between ticks, on its way to the next tick's value.
    const int fade = presentation_phase_ > 0.0
        ? static_cast<int>(std::floor(msg().present_bar_fade(presentation_phase_)))
        : msg().history_bar().fade;
    if (fade <= 0) {
        return;
    }

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
}


}  // namespace th2app
