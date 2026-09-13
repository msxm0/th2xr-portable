// Where the game meets the transcribed display layer.  AVG_ControlChar
// calls outward into the background, the message window and the config; in
// the original those are all file-static globals in the same binary, so the
// calls are direct.  Here they are hooks, named after the functions they
// stand for so a transcribed line still reads as itself.

#include "game.hpp"

namespace th2app {

void Game::build_display()
{
    display_.emplace(renderer_);
    avg_char_.emplace(*display_, character_hooks());
    avg_back_.emplace(*display_, background_hooks());
    avg_msg_.emplace(*display_, avg_back_->back(), message_hooks());
}

bool Game::has_background() const
{
    return display().bmp_flag(th2::bmp_back2);
}

bool Game::transition_drives_graphs() const
{
    if (!transition_ || !back().fd_flag) {
        return false;
    }
    // A menu cross-fade is of the whole composited screen, not of GRP_BACK:
    // it has to go through draw_active_transition() instead, or setting the
    // background graph up for it undoes whatever AVG_SetHalfTone had left
    // there - which took the wash away every time the menu closed.
    if (menu_transition_frames_ > 0) {
        return false;
    }
    // The types the scripts actually use, minus BAK_PATTERN which needs its
    // mask: BAK_CFADE, the four BAK_CFZOOM* and the four BAK_SLIDE_*.
    const int type = transition_->type;
    return type == 1 || (type >= 11 && type <= 14)
        || (type >= 16 && type <= 19);
}

float Game::transition_progress() const
{
    // AVG_ControlBackChange's `rate`:
    //     int back_max = AVG_EffCnt(BackStruct.fd_max);
    //     rate = 256*BackStruct.fd_cnt/back_max;
    // recomputed from the raw fd_max on every pass rather than measured
    // against a start time, which is what lets the skip key end a wipe that
    // is already running - back_max goes to zero and fd_cnt is past it.
    if (!transition_) {
        return 0.0f;
    }
    int back_max = effect_frames(back().fd_max);
    if (back().fd_type == th2::bak_fade
        && (back().r != th2::bright_neutral || back().g != th2::bright_neutral
            || back().b != th2::bright_neutral)) {
        back_max *= 2;
    }
    if (back_max <= 0) {
        return 1.0f;
    }
    return std::clamp(
        static_cast<float>(back().fd_cnt) / static_cast<float>(back_max),
        0.0f, 1.0f);
}

void Game::control_back_change()
{
    // Superseded by AVG_ControlBackChange in avg_back.cpp, which does every
    // fd_type rather than the nine that were only graph parameters.
    avgback().control_back_change();
}
void Game::setup_background_graphs(
    const ShakeSample& shake, bool shake_background, bool shake_characters)
{
    if (!has_background()) {
        display().reset_graph(th2::grp_back);
        display().reset_graph(th2::grp_back + 1);
        return;
    }

    // DSP_SetGraphBmp( GRP_BACK, BMP_BACK2 ) - a sine shake points both the
    // background and the half tone at the clean plate, which is why the
    // characters come out of it and move on their own for the duration.
    const bool have_plate = display().bmp_flag(th2::bmp_back);
    const int back_bmp = (shake_characters || !have_plate)
        ? th2::bmp_back2 : th2::bmp_back;

    // GRP_BACK at LAY_BACK and GRP_BACK+1 at LAY_BACK+2.  Once the ramp has
    // finished the engine turns GRP_BACK off and shows the darkened copy in
    // its place, rather than tinting anything.
    //
    // A wipe takes GRP_BACK+1 away for the duration:
    //     DSP_GetDispBmp( BMP_BACK+1, ... );
    //     DSP_SetGraph( GRP_BACK+1, BMP_BACK+1, LAY_BACK, ON, CHK_NO );
    //     DSP_SetGraphLayer( GRP_BACK, LAY_BACK+1 );
    // so the outgoing snapshot sits at the bottom with the incoming picture
    // above it - and the darkened copy simply has nowhere to be, which is
    // why the half tone vanishes for a wipe without anyone suppressing it.
    // Which bitmap each of the two graphs holds, whether it is shown and
    // what layer it sits on are AVG_SetHalfTone's, AVG_ResetHalfTone's and
    // AVG_ControlShake's - they set them once, when something changes, and
    // leave them.  The only one left here is the wipe, whose outgoing
    // snapshot is a texture of ours rather than a bitmap the engine made.
    const bool wiping = transition_drives_graphs();
    if (wiping) {
        display().set_graph(
            th2::grp_back, back_bmp, th2::lay_back + 1, true, th2::check_none);
        display().borrow_bmp(
            th2::bmp_back + 1, transition_->previous.get(),
            th2::display_width, th2::display_height, false);
        display().set_graph(
            th2::grp_back + 1, th2::bmp_back + 1, th2::lay_back, true,
            th2::check_none);
    } else {
        // Not wiping.  GRP_BACK+1 is shared between the wipe's outgoing
        // snapshot and the half tone's darkened plate - AVG_SetBack parks
        // one there and AVG_SetHalfTone the other - and they never overlap
        // in the engine because AVG_ResetBackHalfTone runs first.  Ours can
        // leave the snapshot behind, because our transition texture outlives
        // BackStruct.fd_flag, so HalfTone.tstep is re-asserted here: it is
        // the one that knows what should be in the slot.
        const bool washed = msg().check_half_tone_step() == th2::tone_disp
            && display().bmp_flag(th2::bmp_backhalf);
        if (display().graph(th2::grp_back).bno != back_bmp
            || display().graph(th2::grp_back).layer != th2::lay_back) {
            display().set_graph(
                th2::grp_back, back_bmp, th2::lay_back, !washed,
                th2::check_none);
        }
        if (washed) {
            display().set_graph(
                th2::grp_back + 1, th2::bmp_backhalf, th2::lay_back + 2,
                true, th2::check_none);
            display().set_graph_pos(
                th2::grp_back + 1, 0, 0,
                back().x, back().y,
                th2::display_width, th2::display_height);
            display().set_graph_disp(th2::grp_back, false);
        } else if (display().graph(th2::grp_back + 1).bno == th2::bmp_back + 1
                   || msg().check_half_tone_step() == th2::tone_nodisp) {
            display().reset_graph(th2::grp_back + 1);
            display().set_graph_disp(th2::grp_back, true);
        }
    }

    const auto view = current_background_view();
    if (wiping) {
        // The snapshot is a picture of the whole screen, so it goes back
        // one to one rather than through BackStruct's window.
        display().set_graph_pos(
            th2::grp_back + 1, 0, 0, 0, 0, th2::display_width,
            th2::display_height);
    }
    // The darkened copy is the background's twin and takes the same window;
    // the snapshot, when it has the slot instead, does not.
    const int geometry_last = wiping ? th2::grp_back : th2::grp_back + 1;
    for (int graph = th2::grp_back; graph <= geometry_last; ++graph) {
        // AVG_SetBackPos:      DSP_SetGraphPos( g, 0,0, x,y, DISP_X,DISP_Y )
        // AVG_SetBackPosZoom:  DSP_SetGraphPosZoom( g, 0,0,DISP_X,DISP_Y,
        //                                           x,y,w,h )
        if (view.width != static_cast<float>(th2::display_width)
            || view.height != static_cast<float>(th2::display_height)) {
            display().set_graph_pos_zoom(
                graph, 0, 0, th2::display_width, th2::display_height,
                static_cast<int>(view.x), static_cast<int>(view.y),
                static_cast<int>(view.width), static_cast<int>(view.height));
        } else {
            display().set_graph_pos(
                graph, 0, 0, static_cast<int>(view.x),
                static_cast<int>(view.y), th2::display_width,
                th2::display_height);
        }
    }

    if (shake_background) {
        for (int graph = th2::grp_back; graph <= geometry_last; ++graph) {
            if (shake.roll_rate >= 0) {
                // DSP_GetGraphBmpSize then
                // DSP_SetGraphRoll( g, DISP_X/2, DISP_Y/2, 0, cnt, 0,0,w,h )
                int width = 0;
                int height = 0;
                display().get_graph_bmp_size(graph, &width, &height);
                display().set_graph_roll(
                    graph, th2::display_width / 2, th2::display_height / 2,
                    0, shake.roll_rate, 0, 0, width, height);
            } else if (shake.half_blend) {
                // SHAKE_ZOOM:
                //     DSP_SetGraphZoom2( g, DISP_X/2, DISP_Y/2, cnt2 );
                //     DSP_SetGraphParam( g, DRW_BLD(128) );
                display().set_graph_zoom2(
                    graph, th2::display_width / 2, th2::display_height / 2,
                    shake.zoom_256);
                display().set_graph_param(
                    graph, th2::drw_bld | (128u << 16));
            } else {
                // DSP_SetGraphSMove( g, BackStruct.x-x, BackStruct.y-y ).
                // The source moves, not the destination: lowering the offset
                // starts the sample earlier and slides the picture the same
                // way the characters go.
                display().set_graph_smove(
                    graph, static_cast<int>(view.x - shake.x),
                    static_cast<int>(view.y - shake.y));
            }
        }
    }

    // GRP_BACK's brightness belongs to AVG_ControlBackFade and, while the
    // wash is ramping, to AVG_ControlHalfTone.  They cannot fight:
    // AVG_SetBackFade calls AVG_ResetHalfTone before it starts.
}

th2::AvgChar::Hooks Game::character_hooks()
{
    th2::AvgChar::Hooks hooks;

    // The decode half of AVG_LoadChar.  The graph setup that follows it in
    // the original stays inside AvgChar, so this only has to fill the slot.
    hooks.load_char_bitmap =
        [this](int bmp_slot, int cno, int pose, int) {
            const auto asset = th2::character_asset_name(cno, pose);
            Texture texture = load_toned_texture(
                renderer_, graphics_, asset, graphics_,
                character_tone_curves(), nullptr,
                take_predecoded_image(false, asset));
            if (!texture) {
                display_->release_bmp(bmp_slot);
                return;
            }
            float width = 0.0f;
            float height = 0.0f;
            SDL_GetTextureSize(texture.get(), &width, &height);
            display_->set_bmp(
                bmp_slot, std::move(texture), static_cast<int>(width),
                static_cast<int>(height));
        };

    hooks.reset_half_tone = [this] { reset_half_tone(); };

    // AVG_SetNovelMessageDisp hides the *text*.  The window frame it sits in
    // is a separate state machine (Message.wstep), which is what
    // AVG_GetWindowCond reads - so setting a character can hide the text
    // while the window is still up, and that is what lets AVG_ControlChar
    // notice it and put it back afterwards.
    hooks.novel_message_disp = [this](bool on) {
        msg().set_novel_message_disp(on);
    };
    hooks.window_cond = [this] { return msg().window_cond() != 0; };
    // AVG_CloseWindow(OFF) / AVG_OpenWindow(OFF,OFF).  Closing parks the
    // message machine at MSG_NODISP and remembers where it was, so the
    // typewriter stops for the length of the animation and comes back on
    // the character it had reached.
    hooks.close_window = [this] {
        message_window_open_ = false;
        msg().close_window();
    };
    hooks.open_window = [this] {
        message_window_open_ = true;
        msg().open_window(false);
    };

    // AVG_CopyBack(OFF): BMP_BACK <- BMP_BACK2, the clean plate back over
    // the baked one.  A bake cannot be undone in place, so this is how a
    // character comes out of the background.
    hooks.copy_back = [this] {
        background_baked_dirty_ = true;
        copy_back_plate();
    };
    // BMP_BACKHALF is a copy of BMP_BACK, so a bake makes it out of date.
    // The engine never has to say this: AVG_SetHalfTone is only ever called
    // from a message, and the script cannot reach one while a character is
    // still animating because every character opcode holds the program
    // counter.  Ours has the same hold, but the bake lands a frame after the
    // script resumes, so the copy has to be told.
    hooks.plate_baked = [this] { half_tone_copy_stale_ = true; };
    // AVG_CreateBackCScope draws the cinemascope bars.  Its opcode exists
    // (Escript.cpp calls AVG_SetBackCScope) but no script in the game uses
    // it - it does not appear once in the 123,413 instructions of SDT.PAK -
    // so there is nothing to create.  The same goes for AVG_ControlNoise,
    // whose DRW_FLT has no SDL equivalent and would need a shader written
    // for an effect nothing asks for.
    hooks.create_back_cscope = [] {};

    // AVG_EffCnt( cnt ): Avg.wait * cnt * Avg.frame / 60, with -1 and -2
    // standing for fifteen and thirty frames, and nothing at all while the
    // message is being skipped.  Avg.frame is 60, so the rate cancels.
    hooks.eff_cnt = [this](int cnt) {
        if (message_cut()) {
            return 0;
        }
        const int wait = std::clamp(config_.effect_speed, 0, 4);
        if (cnt == -1) {
            return wait * 15;
        }
        if (cnt == -2) {
            return wait * 30;
        }
        return wait * cnt;
    };
    // Avg.level / Avg.ami: with effects on a fade is a plain blend, with
    // them off it is a dither mesh or a pattern wipe.
    hooks.level = [this] { return config_.effect_speed != 0; };
    hooks.ami = [] { return false; };

    hooks.back_flag = [this] { return has_background(); };
    hooks.back_scrolling = [this] { return background_scroll_.has_value(); };
    hooks.back_zooming = [this] {
        const auto view = current_background_view();
        return view.width != 800.0f || view.height != 600.0f;
    };
    // BackStruct.redraw is only ever set by a savegame load, and our load
    // path rebuilds the plate itself, so there is nothing to take here.
    hooks.back_redraw_take = [] { return false; };
    hooks.back_pos = [this](int* x, int* y) {
        const auto view = current_background_view();
        if (x) {
            *x = static_cast<int>(view.x);
        }
        if (y) {
            *y = static_cast<int>(view.y);
        }
    };
    return hooks;
}

th2::AvgBack::Hooks Game::background_hooks()
{
    th2::AvgBack::Hooks hooks;
    // AVG_EffCnt / AVG_EffCnt3 / AVG_EffCnt4.  These are the whole of
    // skipping: AVG_GetMesCut() makes each of them return zero, so every
    // counter in BackStruct is already past its max on the next pass.
    hooks.eff_cnt = [this](int n) { return effect_frames(n); };
    hooks.eff_cnt3 = [this](int n) { return effect_frames3(n); };
    hooks.eff_cnt4 = [this](int n) { return effect_frames4(n); };
    hooks.level = [this] { return config_.effect_speed != 0; };
    hooks.half_tone = [this] {
        return std::clamp(
            config_.message_half_tone,
            th2::GameConfig::min_message_half_tone,
            th2::GameConfig::max_message_half_tone);
    };
    hooks.copy_back = [this](bool) { background_baked_dirty_ = true; };
    hooks.set_back_char = [this](int x, int y, bool disp) {
        background_baked_dirty_ = true;
        chars().set_back_char(x, y, disp);
    };
    hooks.set_char_pos_shake = [this](int x, int y, int disp) {
        chars().set_char_pos_shake(x, y, disp);
    };
    hooks.set_char_bright = [this](int r, int g, int b) {
        chars().set_char_bright_all(r, g, b);
    };
    hooks.novel_message_disp = [this](bool on) { message_visible_ = on; };
    hooks.reset_half_tone = [this] { reset_half_tone(); };
    hooks.global_count = [this] { return global_count_; };
    return hooks;
}

th2::AvgMsg::Hooks Game::message_hooks()
{
    th2::AvgMsg::Hooks hooks;
    // GRP_KEYWAIT.  Ours is drawn by draw_click_indicator() at monitor
    // resolution, so this only says whether it is up and which of the two
    // marks it is.
    hooks.set_keywait = [this](bool page) {
        keywait_visible_ = true;
        keywait_page_end_ = page;
    };
    hooks.reset_keywait = [this] { keywait_visible_ = false; };
    hooks.play_se = [this](int number, int volume) {
        play_system_se(number, volume);
    };
    hooks.wait_voice = [this] { return !voice_playing(); };
    // AVG_GetHitKey(): GameKey.click && the pointer is not over a button.
    hooks.hit_key = [this] { return game_key_.click != 0; };
    hooks.mes_cut = [this] { return message_cut(); };
    hooks.msg_cnt = [this] { return message_count_step(); };
    hooks.eff_cnt_puls = [this] { return effect_count_pulse(); };
    hooks.log_start = [this] { open_backlog(); };
    hooks.set_text_disp = [this](bool on) { message_visible_ = on; };
    hooks.reveal_step = [this](int) { message_.reveal_next(); };
    hooks.auto_flag = [this] { return auto_mode_ || demo_mode_; };
    // Avg.auto_key / Avg.auto_page, in frames.
    hooks.auto_key = [this] {
        return th2::auto_delay_ms(
                   config_, current_text_is_read(), true, false) * 60 / 1000;
    };
    hooks.auto_page = [this] {
        return th2::auto_delay_ms(
                   config_, current_text_is_read(), false, true) * 60 / 1000;
    };
    // Avg.msg_page: the "show the whole page at once" reading mode.
    hooks.msg_page = [] { return false; };
    hooks.half_tone_depth = [this] {
        return std::clamp(
            config_.message_half_tone,
            th2::GameConfig::min_message_half_tone,
            th2::GameConfig::max_message_half_tone);
    };
    hooks.set_char_half_tone = [](bool) {};
    hooks.set_char_bright = [this](int r, int g, int b) {
        chars().set_char_bright_all(r, g, b);
    };
    hooks.wav_effect = [] { return false; };
    hooks.demo = [this] { return demo_mode_; };
    // AVG_EffCnt3( Avg.demo_max ): 30fps units, and the one AVG_EffCnt that
    // ignores the skip key.
    hooks.demo_max = [this] {
        return effect_frames3(std::max(0, demo_delay_frames_));
    };
    hooks.script_objects_disp = [this](bool on) {
        for (int i = 0; i < th2::max_script_obj; ++i) {
            if (overlay_states_[static_cast<std::size_t>(i)].visible) {
                display().set_graph_disp(th2::grp_script + i, on);
            }
        }
    };
    return hooks;
}

}  // namespace th2app

