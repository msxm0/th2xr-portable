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
}

void Game::publish_background_bitmaps()
{
    // BMP_BACK is the plate with the characters baked in and BMP_BACK2 the
    // clean copy.  The game still owns both - the background is not a graph
    // yet - so the slots borrow them rather than taking them over.  A
    // character bakes into BMP_BACK through DSP_SetGraphTarget, which needs
    // the slot to point at a texture that can be rendered into, and both of
    // ours are created with target access.
    const auto publish = [this](int slot, SDL_Texture* texture) {
        if (!texture) {
            display_->release_bmp(slot);
            return;
        }
        float width = 0.0f;
        float height = 0.0f;
        SDL_GetTextureSize(texture, &width, &height);
        display_->borrow_bmp(
            slot, texture, static_cast<int>(width),
            static_cast<int>(height), true);
    };
    publish(th2::bmp_back, background_baked_.get());
    publish(th2::bmp_back2, background_.get());
    publish(th2::bmp_backhalf, half_tone_background_.get());
}

bool Game::transition_drives_graphs() const
{
    if (!transition_ || transition_->frames <= 0) {
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
    if (!transition_ || transition_->frames <= 0) {
        return 0.0f;
    }
    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - transition_->started);
    return std::clamp(
        static_cast<float>(elapsed.count() * 60.0 / transition_->frames),
        0.0f, 1.0f);
}

void Game::control_back_change()
{
    // AVG_ControlBackChange, for the types that are only graph parameters.
    // GRP_BACK+1 holds the outgoing snapshot at LAY_BACK and GRP_BACK the
    // incoming background at LAY_BACK+1, so "the wipe" is what these calls
    // do to the two of them between now and the end.
    if (!transition_drives_graphs()) {
        return;
    }
    const int back = th2::grp_back;
    const int snap = th2::grp_back + 1;
    const auto bld = [](int alpha) {
        return th2::drw_bld
            | (static_cast<std::uint32_t>(std::clamp(alpha, 0, 256)) << 16);
    };
    const int rate = std::clamp(
        static_cast<int>(transition_progress() * 256.0f), 0, 256);
    const int centre_x = th2::display_width / 2;
    const int centre_y = th2::display_height / 2;

    switch (transition_->type) {
    case 1:  // BAK_CFADE
        display().set_graph_param(back, bld(rate));
        break;
    case 11: {  // BAK_CFZOOM1
        // Two rates: the first is the alpha, the second the zoom.  They
        // agree only at the ends of the wipe.
        const int faded = 256 - rate * rate / 256;
        const int inverse = 256 - rate;
        display().set_graph_param(back, bld(128 - faded / 2));
        display().set_graph_zoom2(
            back, centre_x, centre_y, inverse * inverse / 256);
        break;
    }
    case 12: {  // BAK_CFZOOM2 - the snapshot is the one that zooms
        const int inverse = 256 - rate;
        const int eased = 256 - inverse * inverse / 256;
        display().set_graph_param(back, bld(eased));
        display().set_graph_zoom2(snap, centre_x, centre_y, eased * 2);
        break;
    }
    case 13: {  // BAK_CFZOOM3 - the snapshot shrinks away and the incoming
                // picture is left plain underneath it
        const int inverse = 256 - rate;
        const int eased = inverse * inverse / 256;
        display().set_graph_param(snap, bld(eased));
        display().set_graph_zoom2(snap, centre_x, centre_y, eased / 2 - 128);
        break;
    }
    case 14: {  // BAK_CFZOOM4
        const int inverse = 256 - rate;
        display().set_graph_param(back, bld(32 - inverse / 8));
        display().set_graph_zoom2(
            back, centre_x, centre_y, -(inverse * inverse / 256) / 2);
        break;
    }
    default: {  // BAK_SLIDE_UP / DO / RI / LE
        const int inverse = 256 - rate;
        const int x = th2::display_width
            - th2::display_width * inverse * inverse / (256 * 256);
        const int y = th2::display_height
            - th2::display_height * inverse * inverse / (256 * 256);
        display().set_graph_param(back, bld(rate));
        display().set_graph_param(snap, bld(rate));
        switch (transition_->type) {
        case 16:
            display().set_graph_move(back, 0, y - th2::display_height);
            display().set_graph_move(snap, 0, y);
            break;
        case 17:
            display().set_graph_move(back, 0, th2::display_height - y);
            display().set_graph_move(snap, 0, -y);
            break;
        case 18:
            display().set_graph_move(back, th2::display_width - x, 0);
            display().set_graph_move(snap, -x, 0);
            break;
        default:  // 19
            display().set_graph_move(back, x - th2::display_width, 0);
            display().set_graph_move(snap, x, 0);
            break;
        }
        break;
    }
    }
}

void Game::setup_background_graphs(
    const ShakeSample& shake, bool shake_background, bool shake_characters)
{
    if (!background_) {
        display().reset_graph(th2::grp_back);
        display().reset_graph(th2::grp_back + 1);
        return;
    }
    publish_background_bitmaps();

    // DSP_SetGraphBmp( GRP_BACK, BMP_BACK2 ) - a sine shake points both the
    // background and the half tone at the clean plate, which is why the
    // characters come out of it and move on their own for the duration.
    const bool have_plate = display().bmp_flag(th2::bmp_back);
    const int back_bmp = (shake_characters || !have_plate)
        ? th2::bmp_back2 : th2::bmp_back;
    const bool copy_shown = !shake_characters && half_tone_settled()
        && display().bmp_flag(th2::bmp_backhalf);
    const int half_bmp = shake_characters
        ? th2::bmp_back2 : th2::bmp_backhalf;

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
    const bool wiping = transition_drives_graphs();
    display().set_graph(
        th2::grp_back, back_bmp,
        wiping ? th2::lay_back + 1 : th2::lay_back,
        wiping || !copy_shown, th2::check_none);
    if (wiping) {
        display().borrow_bmp(
            th2::bmp_back + 1, transition_->previous.get(),
            th2::display_width, th2::display_height, false);
        display().set_graph(
            th2::grp_back + 1, th2::bmp_back + 1, th2::lay_back, true,
            th2::check_none);
    } else {
        display().release_bmp(th2::bmp_back + 1);
        display().set_graph(
            th2::grp_back + 1, half_bmp, th2::lay_back + 2, copy_shown,
            th2::check_none);
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

    // AVG_ControlBackFade: DSP_SetGraphBright( GRP_BACK, rr, gg, bb ), with
    // the half tone's own ramp multiplied in while it is still running.
    // GRP_BACK+1 keeps a neutral brightness, as it does in the original -
    // the darkness is already in the bitmap, and AVG_SetBackFade tears the
    // wash down before it starts anyway.
    const float shade = half_tone_factor();
    display().set_graph_bright(
        th2::grp_back,
        static_cast<int>(background_brightness_[0] * shade),
        static_cast<int>(background_brightness_[1] * shade),
        static_cast<int>(background_brightness_[2] * shade));

    // Last, because every one of its calls lands on the geometry above.
    control_back_change();
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
    hooks.novel_message_disp = [this](bool on) { message_visible_ = on; };
    hooks.window_cond = [this] { return message_window_open_; };
    hooks.close_window = [this] {
        message_window_open_ = false;
        message_visible_ = false;
    };
    hooks.open_window = [this] {
        message_window_open_ = true;
        message_visible_ = true;
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
    hooks.create_back_cscope = [] {};  // AVG_CreateBackCScope: no cscope yet

    // AVG_EffCnt( cnt ): Avg.wait * cnt * Avg.frame / 60, with -1 and -2
    // standing for fifteen and thirty frames, and nothing at all while the
    // message is being skipped.  Avg.frame is 60, so the rate cancels.
    hooks.eff_cnt = [this](int cnt) {
        if (skip_mode_) {
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

    hooks.back_flag = [this] { return background_ != nullptr; };
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

}  // namespace th2app
