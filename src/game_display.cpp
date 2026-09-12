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
    display().set_graph(
        th2::grp_back, back_bmp, th2::lay_back, !copy_shown,
        th2::check_none);
    display().set_graph(
        th2::grp_back + 1, half_bmp, th2::lay_back + 2, copy_shown,
        th2::check_none);

    const auto view = current_background_view();
    for (const int graph : {th2::grp_back, th2::grp_back + 1}) {
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
        for (const int graph : {th2::grp_back, th2::grp_back + 1}) {
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
