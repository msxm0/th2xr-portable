#include "game.hpp"

#include <SDL3/SDL_log.h>

#include <algorithm>
#include <cmath>
#include <format>

// GM_Avg.cpp's system menu - AVG_GoConfig(0), AVG_SetConfigWindow,
// AVG_ControlConfigWindow, AVG_ResetConfigWindow and AVG_EndConfig -
// transcribed for the engine bar (a trace run), where ours would not do.
//
// Ours is a UI of its own: a cross-fade over a snapshot, and the scene left
// running underneath.  The engine's is part of the frame: the script stops,
// the message window, the characters and the background's half-tone plate
// are put away, and sys0100 comes up over what is left as a graph at
// LAY_SYSTEM, fading in over AVG_EffCnt(-1) frames with its buttons
// following it in one after another.  A recorded run that right-clicks is
// compared against exactly that, so it is exactly that.
//
// The three buttons that open another screen - save, load and the side bar
// settings - lead to GWIN_SetSaveLoadWindow and GWIN_SetSideBarWindow, which
// are not transcribed.  The port's own save and load screens and its
// settings panel stand in for them, in the engine's AVG_SAVE / AVG_LOAD /
// AVG_SETTING step: the scene stays frozen exactly as the engine leaves it,
// and the way back is AVG_ControlSystem's.

namespace th2app {
namespace {

// CONFIG_STRUCT's modes, in the original's order.
enum {
    cnf_not,
    cnf_open,
    cnf_close,
    cnf_normal,
    cnf_dis_text,
    cnf_another,
    cnf_next_open,
    cnf_next_close,
    cnf_check,
};

// RECT2 CnfCons[5]: save, load, hide the text, settings, and the cancel bar.
struct CnfRect {
    int x;
    int y;
    int w;
    int h;
};
constexpr CnfRect cnf_cons[5] = {
    {200, 112 + 88 * 0, 400, 82},
    {200, 112 + 88 * 1, 400, 82},
    {200, 112 + 88 * 2, 400, 82},
    {200, 112 + 88 * 3, 400, 82},
    {306, 480, 188, 32},
};

// LAY_MAP = LAY_WINDOW+10, LAY_SYSTEM = LAY_MAP+10.
constexpr int lay_system = th2::lay_window + 10 + 10;
// CHK_ANTI: the graphs' colour key.  Ours are straight-alpha textures, so the
// key has nothing to do and the value is only carried for the record.
constexpr int chk_anti = th2::check_none;

}  // namespace

bool Game::engine_config_check() const
{
    // BOOL AVG_ConfigCheck( void ): only while nothing is moving and the
    // message is waiting on the reader.
    const auto& b = back();
    const bool bk_flag = !(b.cs_flag || b.sc_flag || b.fd_flag || b.br_flag
                           || (b.sk_speed != 0 && b.sk_flag != 0));
    const bool char_flag = chars().config_check();
    // AVG_ConfigCheckSelect: !SelectWindow.cond, which is 0 once the
    // options have finished typing.
    const bool sl_flag = !choosing_ || choice_reveal_finished();
    const bool fd_flag = !(avg_back_ && avg_back_->wait_fade());
    // AVG_ConfigCheckMessage: the window is up and still, or not there.
    const bool wf_flag = true;
    // (AVG_GetSelectMessageFlag() || AVG_GetNovelMessageFlag())
    //     && AVG_GetNovelMessageConfig()
    const auto& m = msg().state();
    const bool select = choosing_;
    const bool novel_config = m.step1 == th2::msg_wait
        || m.step1 == th2::msg_stop
        || (m.step1 == th2::msg_next && select);
    const bool go_flag = (select || m.flag != 0) && novel_config;
    return bk_flag && char_flag && sl_flag && fd_flag && wf_flag && go_flag;
}

void Game::engine_go_config(int mode)
{
    // void AVG_GoConfig( int mode ), from AVG_GAME.
    script_flag_ = false;                       // MAIN_SetScriptFlag( OFF )
    msg().close_window_for_config();            // AVG_CloseWindow(1)
    chars().close_char();                       // AVG_CloseChar()
    msg().set_half_tone();                      // AVG_SetHalfTone()
    engine_close_back();                        // AVG_CloseBack()
    // AVG_FadeOutVoiceAll( 30 ): every voice that is playing, down to
    // nothing over thirty frames.
    for (std::size_t i = 0; i < voice_channels_.size(); ++i) {
        if (voice_channels_[i].playing()) {
            voice_channels_[i].fade_to(0.0f, audio_fade_duration(30), true);
        }
    }
    // ConfigOpenMode: where AVG_ControlSystem goes back to - the menu, or
    // straight to the scene.
    engine_config_open_mode_ = mode;
    // DSP_GetDispBmp( BMP_CAP, ... ): the screen as it was asked from,
    // which is what a save made from here shows as its thumbnail.
    save_snapshot_ = capture_frame_thumbnail(
        save_thumbnail_width, save_thumbnail_height);
    if (mode == 0) {
        play_system_se(9002, 150);
        engine_set_config_window();
    } else {
        // GWIN_SetSaveLoadWindow / GWIN_SetSideBarWindow, then
        // AVG_ChangeSetp( 0, AVG_SAVE / AVG_LOAD / AVG_SETTING ).
        engine_open_port_screen(mode);
    }
    engine_config_step_next_ = true;            // AVG_ChangeSetp( AVG_CONFIG )
}

void Game::engine_open_port_screen(int page)
{
    // 1 save, 2 load, 3 the side bar settings: AVG_GoConfig's numbering.
    if (page == 1) {
        open_save_load(UiMode::save);
    } else if (page == 2) {
        open_save_load(UiMode::load);
    } else {
        open_config();
    }
}

void Game::engine_port_screen_closed()
{
    // AVG_ControlSystem, when GWIN_ControlSaveLoadWindow or
    // GWIN_ControlSideBarWindow reports itself done:
    //     if(ConfigOpenMode==0){ AVG_ChangeSetp( 0, AVG_CONFIG );
    //                            Config.mode = CNF_NEXT_CLOSE; }
    //     else                 { AVG_EndConfig(); }
    // Only for a screen the engine opened; the title's load screen has no
    // scene under it.
    const bool engine_step = engine_config_step_next_.value_or(
        engine_config_step_);
    if (!engine_step) {
        return;
    }
    if (engine_config_open_mode_ == 0 && engine_config_.flag) {
        engine_config_.mode = cnf_next_close;
        engine_config_.next_cnt = 0;
    } else {
        engine_reset_config_window();
        engine_end_config();
    }
}

void Game::engine_config_loaded()
{
    // A load from inside the engine's config replaces the scene it was
    // frozen over; nothing of the menu survives it.
    if (engine_config_.flag) {
        engine_reset_config_window();
    }
    engine_config_ = {};
    engine_config_step_ = false;
    engine_config_step_next_.reset();
}

void Game::engine_close_back()
{
    // void AVG_CloseBack( void ): the half-tone plate and every weather
    // graph go.  The petals come back when AVG_ControlWeather next runs,
    // which is on the next AVG_GAME frame.
    display().set_graph_disp(th2::grp_back + 1, false);
    engine_back_closed_ = true;
    weather_disp_ = false;
}

void Game::engine_open_back()
{
    // void AVG_OpenBack( void ).
    display().set_graph_disp(th2::grp_back + 1, true);
    engine_back_closed_ = false;
}

void Game::engine_set_config_window()
{
    // void AVG_SetConfigWindow( int open_mode ).
    th2::set_draw_flag_on();   // MainWindow.draw_flag=ON;
    engine_config_ = {};
    engine_config_.flag = 1;
    engine_config_.mode = cnf_open;

    // DSP_LoadBmp( BMP_SYSTEM+0..3, "sys0100", "sys0110", "sys0111",
    // "sys0230" ) - already loaded, so lent to the display rather than read
    // again.
    const auto lend = [this](int bno, const Texture& texture) {
        if (!texture) {
            return;
        }
        float w = 0.0f;
        float h = 0.0f;
        SDL_GetTextureSize(texture.get(), &w, &h);
        display().borrow_bmp(bno, texture.get(), static_cast<int>(w),
                             static_cast<int>(h), false);
    };
    lend(th2::bmp_system + 0, ui_sys_menu_bg_);
    lend(th2::bmp_system + 1, ui_sys_menu_btns_);
    lend(th2::bmp_system + 2, ui_sys_cancel_);
    lend(th2::bmp_system + 3, ui_save_digits_);

    const int g = th2::grp_system;
    display().set_graph(g + 0, th2::bmp_system + 0, lay_system + 0, false,
                        th2::check_none);
    for (int i = 1; i <= 4; ++i) {
        display().set_graph(g + i, th2::bmp_system + 1, lay_system + 1, false,
                            chk_anti);
    }
    display().set_graph(g + 5, th2::bmp_system + 2, lay_system + 1, false,
                        chk_anti);

    const int month = runtime_.flag(0);   // ESC_GetFlag( _MONTH )
    const int day = runtime_.flag(1);     // ESC_GetFlag( _DAY )
    const auto date = month == 0 || replay_mode_
        ? std::string("?A?B")
        : std::format("{}A{}B", month, day);
    display().set_graph_str(g + 6, th2::bmp_system + 3, lay_system + 1,
                            false, chk_anti, date);
    display().set_graph_move(g + 6, 138, 12);

    display().set_graph_pos(g + 0, 0, 0, 0, 0, th2::display_width,
                            th2::display_height);
    for (int i = 0; i < 4; ++i) {
        display().set_graph_pos(g + 1 + i, cnf_cons[i].x, cnf_cons[i].y,
                                400 * (i % 2), 82 * 3 * (i / 2),
                                cnf_cons[i].w, cnf_cons[i].h);
    }
    display().set_graph_pos(g + 5, cnf_cons[4].x, cnf_cons[4].y, 0, 0,
                            cnf_cons[4].w, cnf_cons[4].h);
    engine_config_mouse_ = msg().mouse_layer();   // Config.mouse

    auto& m = msg();
    for (int i = 0; i < 2; ++i) {
        m.set_mouse_rect(1, i, cnf_cons[i].x, cnf_cons[i].y, cnf_cons[i].w,
                         cnf_cons[i].h, !replay_mode_);
        if (replay_mode_) {
            display().set_graph_fade(g + 1 + i, 64);
        }
    }
    // MapStep: the menu was opened from the map, where hiding the text
    // means nothing.  A trace only opens it from a message.
    m.set_mouse_rect(1, 2, cnf_cons[2].x, cnf_cons[2].y, cnf_cons[2].w,
                     cnf_cons[2].h, true);
    for (int i = 3; i < 5; ++i) {
        m.set_mouse_rect(1, i, cnf_cons[i].x, cnf_cons[i].y, cnf_cons[i].w,
                         cnf_cons[i].h, true);
    }
    m.set_mouse_layer(1);
}

void Game::engine_reset_config_window()
{
    // void AVG_ResetConfigWindow( void ).
    msg().reset_mouse_rect_layer(1);
    msg().set_mouse_layer(engine_config_mouse_);
    engine_config_ = {};
    for (int i = 0; i < 20; ++i) {
        display().reset_graph(th2::grp_system + i);
    }
    for (int i = 0; i < 10; ++i) {
        display().release_bmp(th2::bmp_system + i);
    }
}

void Game::engine_end_config()
{
    // void AVG_EndConfig( void ), BackStep == AVG_GAME.
    script_flag_ = true;
    msg().open_window_for_config();   // AVG_OpenWindow( ON, 1 )
    engine_open_back();
    engine_config_step_next_ = false;  // AVG_ChangeSetp( 0, BackStep )
}

bool Game::present_engine_config_graph(
    int gno, double phase, th2::Graph& graph) const
{
    // The menu's fade in or out, between ticks: control_engine_config's
    // rate at the count the next tick is heading for.  The count is stepped
    // before the rate is worked out, so the screen shows cnt.
    const auto& c = engine_config_;
    const int g = th2::grp_system;
    if (!c.flag || (c.mode != cnf_open && c.mode != cnf_close)
        || gno < g || gno > g + 6) {
        return false;
    }
    const int cmax = effect_frames(-1);
    if (cmax <= 0 || c.cnt >= cmax
        || (c.mode == cnf_close && c.cnt + 1 >= cmax)) {
        return false;   // the next tick finishes it; closing, it resets
    }
    const double at = std::min(static_cast<double>(cmax), c.cnt + phase);
    const double rate = c.mode == cnf_open
        ? 256.0 * at / cmax : 256.0 - 256.0 * at / cmax;
    if (rate >= 256.0) {
        return false;
    }
    static constexpr int start[7] = {0, 64, 80, 96, 112, 128, 128};
    const int i = gno - g;
    const double alpha = i == 0
        ? rate : std::clamp((rate - start[i]) * 2.0, 0.0, 255.0);
    graph.param = th2::DRW_BLD(static_cast<int>(std::floor(alpha)));
    return true;
}

void Game::engine_close_start_config_window(int cmax)
{
    // void AVG_CloseStartConfigWindow( int cmax ): a close started while
    // still opening runs back from where the opening had got to.
    auto& c = engine_config_;
    if (!(c.mode == cnf_open && c.cnt == 0) && c.mode != cnf_close) {
        c.cnt = c.mode == cnf_open ? cmax - c.cnt : 0;
        c.mode = cnf_close;
    }
}

void Game::control_engine_config()
{
    // int AVG_ControlConfigWindow( void ).
    auto& c = engine_config_;
    if (!c.flag) {
        return;
    }
    const int cmax = effect_frames(-1);
    const int select = msg().mouse_no_ex(th2::mouse_any, 1);
    const bool click = game_key_.click != 0;
    const bool cansel = game_key_.cansel != 0;
    int rate = 256;
    const int g = th2::grp_system;

    switch (c.mode) {
    case cnf_not:
        break;
    case cnf_another:
        break;
    case cnf_next_close: {
        // Back from the port's screen: the menu is up again at once, and
        // takes clicks after AVG_EffCnt(10) frames.
        const int next_max = effect_frames(10);
        c.next_cnt++;
        if (c.next_cnt >= next_max) {
            c.mode = cnf_normal;
            c.next_cnt = 0;
        }
        break;
    }
    case cnf_next_open: {
        // The pressed button stays on screen for AVG_EffCnt(10) frames, and
        // then the other screen takes over.
        const int next_max = effect_frames(10);
        c.next_cnt++;
        if (c.next_cnt >= next_max) {
            engine_open_port_screen(c.next_mode);
            c.mode = cnf_another;
            display().set_graph_disp(g + 3, false);
            c.next_cnt = 0;
        }
        break;
    }
    case cnf_open:
        c.cnt++;
        if (c.cnt >= cmax) {
            c.mode = cnf_normal;
            rate = 256;
        } else {
            rate = 256 * c.cnt / cmax;
        }
        break;
    case cnf_close:
        c.cnt++;
        if (c.cnt >= cmax) {
            c.mode = cnf_not;
            rate = 0;
            engine_reset_config_window();
            engine_end_config();
        } else {
            rate = 256 - 256 * c.cnt / cmax;
        }
        break;
    case cnf_normal:
        //     if(GameKey.u) MUS_SetMousePosRect( hwnd, 1, (select<=0)? 0 : select-1 );
        //     if(GameKey.d) MUS_SetMousePosRect( hwnd, 1, (select<0)? 0 : select+1 );
        if (game_key_.u) {
            msg().set_mouse_pos_rect(1, select <= 0 ? 0 : select - 1);
        }
        if (game_key_.d) {
            msg().set_mouse_pos_rect(1, select < 0 ? 0 : select + 1);
        }
        for (int i = 0; i < 4; ++i) {
            display().set_graph_smove(g + 1 + i, 400 * (i % 2),
                                      82 * 3 * (i / 2) + 0 * 82);
        }
        display().set_graph_smove(g + 5, 188 * 0, 0);
        if (select != -1) {
            if (select != engine_config_select_back_) {
                play_system_se(9108, 255);
            }
            if (select < 4) {
                display().set_graph_smove(g + 1 + select, 400 * (select % 2),
                                          82 * 3 * (select / 2) + 1 * 82);
            } else {
                display().set_graph_smove(g + 5, 188 * 1, 0);
            }
            if (click) {
                th2::set_draw_flag_on();   // MainWindow.draw_flag=1;
                if (select < 4) {
                    display().set_graph_smove(
                        g + 1 + select, 400 * (select % 2),
                        82 * 3 * (select / 2) + 2 * 82);
                } else {
                    display().set_graph_smove(g + 5, 188 * 2, 0);
                }
                switch (select) {
                case 0:
                case 1:
                case 3:
                    // CNF_NEXT_SAVE / CNF_NEXT_LOAD / CNF_NEXT_SIDE, kept as
                    // AVG_GoConfig's page numbers.
                    play_system_se(9014, 255);
                    if (trace_mode_) {
                        // See hooks.go_config: a replay could never leave
                        // the port's screen, and the reference's is not
                        // ours.
                        SDL_Log("engine config: button %d opens a screen "
                                "that is not transcribed", select);
                        break;
                    }
                    c.next_mode = select == 3 ? 3 : select + 1;
                    c.mode = cnf_next_open;
                    break;
                case 2:
                    play_system_se(9014, 255);
                    msg().reset_half_tone();
                    c.mode = cnf_dis_text;
                    break;
                case 4:
                    play_system_se(9014, 255);
                    engine_close_start_config_window(cmax);
                    break;
                default:
                    break;
                }
            }
        }
        if (cansel) {
            play_system_se(9107, 255);
            engine_close_start_config_window(cmax);
        }
        engine_config_select_back_ = select;
        break;
    case cnf_dis_text:
        // "Hide the text": the menu goes and the bare scene is shown until
        // the next click.
        for (int i = 0; i < 6; ++i) {
            display().set_graph_disp(g + i, false);
        }
        engine_open_back();
        rate = 0;
        if (click || cansel) {
            rate = 256;
            for (int i = 0; i < 6; ++i) {
                display().set_graph_disp(g + i, true);
            }
            msg().set_half_tone();
            c.mode = cnf_normal;
        }
        break;
    default:
        break;
    }

    if (c.mode != cnf_another) {
        if (rate < 256) {
            // The backing fades in first, and each button a little behind
            // the one above it.
            display().set_graph_param(g + 0, th2::DRW_BLD(rate));
            display().set_graph_disp(g + 0, true);
            for (int i = 1; i <= 6; ++i) {
                display().set_graph_disp(g + i, true);
            }
            const int start[7] = {0, 64, 80, 96, 112, 128, 128};
            for (int i = 1; i <= 6; ++i) {
                display().set_graph_param(
                    g + i,
                    th2::DRW_BLD(std::clamp((rate - start[i]) * 2, 0, 255)));
            }
            engine_open_back();
        } else {
            for (int i = 0; i < 7; ++i) {
                display().set_graph_disp(g + i, true);
                display().set_graph_param(g + i, th2::drw_nml);
            }
            engine_close_back();
        }
    } else {
        for (int i = 0; i < 7; ++i) {
            display().set_graph_disp(g + i, false);
        }
    }
}

}  // namespace th2app
