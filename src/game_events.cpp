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

bool Game::handle(const th2::Event& event)
{
    const auto name = event.instruction.name;
    if (name == "B" || name == "BT" || name == "BC" || name == "BCT") {
        const int scene = number(event, 1) < 0 ? -1
            : number(event, 1) * 10
                + std::max<std::int32_t>(0, number(event, 2));
        const bool unchanged_direct =
            number(event, 0) == -1
            && has_background()
            && bg_scene_ == scene;
        // ESC_EOprB is a two-phase opcode:
        //
        //     if(EOprFlag[ESC_B]==0){ EOprFlag[ESC_B]=1;
        //         AVG_ResetBackHalfTone( bak_no, EscParam[0].num ); }
        //     else if(EOprFlag[ESC_B]==1){ EOprFlag[ESC_B]=2;
        //         AVG_SetBack( ... ); }
        //
        // so the wash comes off the plate on one frame and the new picture is
        // copied from it on the next.  Copying in the same frame took the
        // darkened plate into the new background.  BT and BCT have no such
        // split and run both halves at once.
        const bool split = name == "B" || name == "BC";
        if (!split || opcode_phase_ == 1) {
            // AVG_ResetBackHalfTone: nothing at all when the background is
            // not really changing.
            if (unchanged_direct) {
                return true;
            }
            // void AVG_ResetBackHalfTone( int bak_no, int chg_type ):
            //
            //     if(BackStruct.flag && bak_no==BackStruct.bno
            //        && chg_type==BAK_DIRECT) return;
            //     AVG_ResetHalfTone();
            //     AVG_SetNovelMessageDisp(OFF);
            //     MainWindow.draw_flag = 1;
            //
            // Through AVG_SetNovelMessageDisp rather than setting our own
            // visibility flag: that also clears NovelMessage.disp, which is
            // the field the rest of the message machine reads, and resets
            // GRP_KEYWAIT - so the click indicator does not sit on screen
            // through a background change.
            reset_half_tone();
            msg().set_novel_message_disp(false);
            th2::set_draw_flag_on();   // AVG_ResetBackHalfTone: MainWindow.draw_flag = 1;
        }
        if (!split || opcode_phase_ == 2) {
            if (unchanged_direct) {
                return true;
            }
            // ESC_EOprB fills in what the script left at -1 before using
            // any of it:
            //     if(EscParam[3].num==-1) EscParam[3].num = -2;
            //     if(EscParam[6].num==-1) EscParam[6].num = 128;
            // Same trap as ESC_EOprV: -1 and -2 are not "unset" to
            // AVG_EffCnt, they are fifteen frames and thirty, so passing the
            // script's -1 straight through ran every defaulted B at half its
            // length.  Measured at pc 52 of 020000100.sdt, where the engine
            // holds the transition a frame longer than ours did.
            begin_transition(
                number(event, 0),
                number(event, 3) == -1 ? -2 : number(event, 3),
                number(event, 6) == -1 ? 128 : number(event, 6), true);
            set_background(
                event, name == "BC" || name == "BCT");
        }
    } else if (name == "H" || name == "HT") {
        if (number(event, 1) >= 0) {
            const int visual = number(event, 1) * 10
                + std::max<std::int32_t>(0, number(event, 2));
            const bool unchanged_direct =
                number(event, 0) == -1
                && has_background()
                && bg_scene_ == visual;
            const bool split = name == "H";
            if (!split || opcode_phase_ == 1) {
                if (unchanged_direct) {
                    return true;
                }
                reset_half_tone();
                msg().set_novel_message_disp(false);
                th2::set_draw_flag_on();   // AVG_ResetBackHalfTone: MainWindow.draw_flag = 1;
            }
            if (!split || opcode_phase_ == 2) {
                if (unchanged_direct) {
                    return true;
                }
                // ESC_EOprH defaults the same way, with vague at index 7.
                begin_transition(
                    number(event, 0),
                    number(event, 3) == -1 ? -2 : number(event, 3),
                    number(event, 7) == -1 ? 128 : number(event, 7), true);
                set_cg(event, BackgroundKind::hcg, 'h');
            }
        }
    } else if (name == "V" || name == "VT") {
        // ESC_EOprV fills in the arguments the script left at -1 before it
        // uses any of them:
        //
        //     bak_no = EscParam[1].num*10;
        //     if(EscParam[2].num!=-1) bak_no += EscParam[2].num;
        //     if(EscParam[3].num==-1) EscParam[3].num = -2;
        //     if(EscParam[7].num==-1) EscParam[7].num = 128;
        //
        // -1 and -2 are not "unset" to AVG_EffCnt - they are fifteen frames
        // and thirty - so passing the script's -1 straight through ran every
        // defaulted V at half its length.  ESC_EOprVT does the same except
        // that it never adds EscParam[2], so a VT ignores the sub-number.
        const bool split = name == "V";
        const int visual = split
            ? number(event, 1) * 10
                + (number(event, 2) != -1 ? number(event, 2) : 0)
            : number(event, 1) * 10;
        const int fade_frames =
            number(event, 3) == -1 ? -2 : number(event, 3);
        const int vague = number(event, 7) == -1 ? 128 : number(event, 7);
        const bool unchanged_direct =
            number(event, 0) == -1
            && has_background()
            && bg_scene_ == visual;
        if (!split || opcode_phase_ == 1) {
            if (unchanged_direct) {
                return true;
            }
            reset_half_tone();
            msg().set_novel_message_disp(false);
            th2::set_draw_flag_on();   // AVG_ResetBackHalfTone: MainWindow.draw_flag = 1;
        }
        if (!split || opcode_phase_ == 2) {
            if (unchanged_direct) {
                return true;
            }
            begin_transition(
                number(event, 0), fade_frames, vague, true);
            set_cg(event, BackgroundKind::visual, 'v');
        }
    } else if (name == "FB") {
        msg().set_novel_message_disp(false);
        begin_background_fade(
            number(event, 0), number(event, 1), number(event, 2),
            number(event, 3));
    } else if (name == "F") {
        // AVG_SetFlash runs two AVG_ColtrolFade legs, both counted by
        // AVG_EffCnt, so the effect-speed setting scales them like any other
        // effect.
        // AVG_SetFlash( r, g, b, fade1, fade2 ), whose counters stay raw so
        // AVG_EffCnt is re-asked on every pass of AVG_ColtrolFade.
        avgback().set_flash(
            std::clamp(number(event, 0), 0, 255),
            std::clamp(number(event, 1), 0, 255),
            std::clamp(number(event, 2), 0, 255),
            number(event, 3), number(event, 4));
        screen_flash_ = ScreenFlash{
            std::clamp(number(event, 0), 0, 255),
            std::clamp(number(event, 1), 0, 255),
            std::clamp(number(event, 2), 0, 255),
            number(event, 3), number(event, 4),
            engine_now(),
        };
    } else if (name == "Q" || name == "SetShake") {
        // AVG_SetShake(): SHAKE_SIN (0) and SHAKE_ALL_SIN (6) put the
        // message away before shaking.  Three is SHAKE_TXT_SIN, which shakes
        // the *text* - hiding it there removed the only thing the effect
        // moves - and six was missing, which is why a shake-everything took
        // the message and its backdrop with it and showed the backdrop's
        // rectangular edge sliding about.
        //
        // The original also calls AVG_ResetHalfTone() here; update_half_tone()
        // already clears the wash as soon as the message is hidden, and lets
        // it ramp back up from nothing when the message returns, which is the
        // same thing a frame later.
        //
        // The count guard stands in for the original's type rewrite: with a
        // zero count SHAKE_SIN becomes SHAKE_SIN_SET and SHAKE_ALL_SIN
        // becomes SHAKE_ALL_SIN_SET, neither of which is in the list, so
        // neither hides anything.
        //
        // Only once the shake has started, though: AVG_SetShake opens with
        //     if(Avg.msg_cut) return FALSE;
        // so under a held skip key there is no shake and the message stays
        // up.  Hidden first regardless, it went for a frame mid-skip.
        //
        // BOOL AVG_SetShake( type, pich, speed, dir, swing ).  The speed
        // stays the raw number the script wrote; AVG_EffCnt4 is re-asked on
        // every pass of AVG_ControlShake, so zero is an endless shake and a
        // held skip key ends any of them on the next pass.
        const bool shaking = avgback().set_shake(
            number(event, 0), number(event, 1), std::max(0, number(event, 2)),
            number(event, 3),
            event.arguments.size() > 4 && number(event, 4) >= 0
                ? number(event, 4) : 256);
        if (shaking && (number(event, 0) == 0 || number(event, 0) == 6)
            && number(event, 2) != 0) {
            msg().set_novel_message_disp(false);
        }
    } else if (name == "S") {
        begin_background_scroll(
            number(event, 0), number(event, 1), 800.0f, 600.0f,
            number(event, 2), number(event, 3));
    } else if (name == "Z") {
        begin_background_scroll(
            number(event, 0), number(event, 1),
            number(event, 2), number(event, 3),
            number(event, 4), number(event, 5) + 3);
    } else if (name == "WaitFrame") {
        // void AVG_SetWaitFrame( int type, int wait ), verbatim:
        //
        //     WaitStruct.flag  = 1;
        //     WaitStruct.type  = type;
        //     WaitStruct.count = 0;
        //     WaitStruct.max   = wait;
        //
        // ESC_EOprWaitFrame passes -1 for the type, which is the
        // AVG_EffCnt4 arm.  The count is compared against a maximum that is
        // recomputed every frame, so a skip key taken mid-wait ends it on
        // the next frame rather than on the one it was armed for.
        wait_frame_.flag = 1;
        wait_frame_.type = -1;
        wait_frame_.count = 0;
        wait_frame_.max = number(event, 0);
    } else if (name == "WaitTime") {
        const auto now = static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                engine_now().time_since_epoch()).count());
        const auto deadline = static_cast<std::uint32_t>(number(event, 0));
        const auto remaining = static_cast<std::int32_t>(deadline - now);
        if (remaining > 0) {
            wake_time_ = engine_now()
                + std::chrono::milliseconds(remaining);
        }
    } else if (name == "SetMovie") {
        start_movie(number(event, 0), 0, true);
    } else if (name == "SetEnding") {
        const int ending = number(event, 1) == 1 || number(event, 0) == 10
            ? 0 : number(event, 0);
        start_movie(1, ending, true);
    } else if (name == "SetTitle") {
        // void AVG_SetGotoTitle( void ):
        //     GotoTitle = 1; Avg.demo = OFF; Avg.msg_cut = OFF;
        //     Avg.msg_cut_mode = OFF; Avg.auto_flag = OFF;
        //     AVG_StopBGM( 15 ); AVG_FadeSeAll( 15 );
        //     MAIN_SetScriptFlag( OFF );
        //     AVG_SetFade( 0, 0, 0, ON, -1 );
        // The fade is what holds the instruction: -1 is AVG_EffCnt(-1), and
        // with Avg.wait 2 at 60fps that is thirty frames.  Measured at pc
        // 1648 of 500000000.sdt, where the reference sits from tick 841121
        // and releases the script at 841153.
        demo_mode_ = false;
        auto_mode_ = false;
        skip_mode_ = false;
        skip_held_ = false;
        stop_bgm(15);
        stop_all_se(15);
        avgback().set_fade(0, 0, 0, 1, -1);
        goto_title_pending_ = true;
    } else if (name == "SetDemoFlag") {
        demo_mode_ = number(event, 0) != 0;
        demo_delay_frames_ = std::max(0, number(event, 1));
        auto_next_time_.reset();
    } else if (name == "SetReplayNo") {
        const int replay = number(event, 0);
        if (unlocked_replays_.emplace(replay).second) {
            persistent_state_.unlock(
                th2::PersistentState::UnlockKind::replay, replay);
        }
    } else if (name == "ViewClock") {
        begin_clock(number(event, 0));
    } else if (name == "ViewCalender") {
        begin_calendar(number(event, 0), number(event, 1));
    } else if (name == "SkipDate") {
        skipped_month_ = number(event, 0);
        skipped_day_ = number(event, 1);
    } else if (name == "SetSakura") {
        start_sakura(number(event, 0), true);
    } else if (name == "StopSakura") {
        stop_sakura(true);
    } else if (name == "SetTimeMode") {
        const int tone = number(event, 0) < 0 ? 0 : number(event, 0);
        const int effect = number(event, 1) < 0 ? 0 : number(event, 1);
        tone_ = tone + effect * 4;
        tone_back_ = -1;
        tone_char_ = -1;
    } else if (name == "SetWeatherMode") {
        weather_ = std::max<std::int32_t>(0, number(event, 0));
    } else if (name == "SetBmpEx") {
        // AVG_LoadBmp ends with MainWindow.draw_flag=ON.
        th2::set_draw_flag_on();
        if (const auto slot = overlay_index(number(event, 0))) {
            load_overlay(
                *slot, text(event, 2), text(event, 6),
                number(event, 5) < 0 ? 1 : number(event, 5),
                number(event, 3), number(event, 4));
        }
    } else if (name == "ResetBmp") {
        if (const auto slot = overlay_index(number(event, 0))) {
            display().reset_graph(
                th2::grp_script + static_cast<int>(*slot));
            display().release_bmp(
                th2::bmp_script + static_cast<int>(*slot));
            overlay_states_[*slot] = {};
        }
    } else if (name == "SetBmpDisp") {
        // AVG_SetBmpDisp: DSP_SetGraphDisp( GRP_SCRIPT+s_bno, disp )
        if (const auto slot = overlay_index(number(event, 0))) {
            const bool on = number(event, 1) != 0;
            display().set_graph_disp(
                th2::grp_script + static_cast<int>(*slot), on);
            overlay_states_[*slot].visible = on;
        }
    } else if (name == "SetBmpLayer") {
        // AVG_SetBmpLayer: DSP_SetGraphLayer( GRP_SCRIPT+s_bno, layer )
        if (const auto slot = overlay_index(number(event, 0))) {
            display().set_graph_layer(
                th2::grp_script + static_cast<int>(*slot), number(event, 1));
            overlay_states_[*slot].layer = number(event, 1);
        }
    } else if (name == "SetBmpParam") {
        // AVG_SetBmpParam: DSP_SetGraphParam( GRP_SCRIPT+s_bno, param ).
        // The script gives the mode and its parameter separately, where the
        // engine has them packed into one DWORD.
        if (const auto slot = overlay_index(number(event, 0))) {
            auto& state = overlay_states_[*slot];
            state.parameter = number(event, 1);
            state.parameter_value =
                number(event, 2) < 0 ? 0 : number(event, 2);
            display().set_graph_param(
                th2::grp_script + static_cast<int>(*slot),
                static_cast<std::uint32_t>(state.parameter)
                    | (static_cast<std::uint32_t>(state.parameter_value)
                       << 16));
        }
    } else if (name == "SetBmpRevParam") {
        if (const auto slot = overlay_index(number(event, 0))) {
            overlay_states_[*slot].reverse = number(event, 1);
            display().set_graph_rev_param(
                th2::grp_script + static_cast<int>(*slot),
                static_cast<std::uint32_t>(number(event, 1)));
        }
    } else if (name == "SetBmpBright") {
        // AVG_SetBmpBright: DSP_SetGraphBright( GRP_SCRIPT+s_bno, r, g, b ).
        // A graph field, not a walk over the picture - and the brightening
        // half above 128 is the additive pass the display layer does.
        if (const auto slot = overlay_index(number(event, 0))) {
            auto& state = overlay_states_[*slot];
            state.red = std::clamp(number(event, 1), 0, 255);
            state.green = number(event, 2) < 0
                ? state.red : std::clamp(number(event, 2), 0, 255);
            state.blue = number(event, 3) < 0
                ? state.red : std::clamp(number(event, 3), 0, 255);
            display().set_graph_bright(
                th2::grp_script + static_cast<int>(*slot),
                state.red, state.green, state.blue);
        }
    } else if (name == "SetBmpMove") {
        // AVG_SetBmpMove: DSP_SetGraphMove( g, x*800/640, y*600/448 )
        if (const auto slot = overlay_index(number(event, 0))) {
            auto& state = overlay_states_[*slot];
            state.destination_x = number(event, 1);
            state.destination_y = number(event, 2);
            display().set_graph_move(
                th2::grp_script + static_cast<int>(*slot),
                state.destination_x * 800 / 640,
                state.destination_y * 600 / 448);
        }
    } else if (name == "SetBmpPos") {
        // AVG_SetBmpPos: DSP_SetGraphPos with every coordinate scaled.
        if (const auto slot = overlay_index(number(event, 0))) {
            auto& state = overlay_states_[*slot];
            state.destination_x = number(event, 1);
            state.destination_y = number(event, 2);
            state.source_x = number(event, 3);
            state.source_y = number(event, 4);
            state.destination_width = state.source_width = number(event, 5);
            state.destination_height = state.source_height = number(event, 6);
            display().set_graph_pos(
                th2::grp_script + static_cast<int>(*slot),
                state.destination_x * 800 / 640,
                state.destination_y * 600 / 448,
                state.source_x * 800 / 640, state.source_y * 600 / 448,
                state.destination_width * 800 / 640,
                state.destination_height * 600 / 448);
        }
    } else if (name == "SetBmpZoom") {
        // AVG_SetBmpZoom: DSP_SetGraphZoom( g, dx, dy, dw, dh ), which puts
        // the source back to the whole bitmap - it is a scale of the picture,
        // not a window onto it.
        if (const auto slot = overlay_index(number(event, 0))) {
            auto& state = overlay_states_[*slot];
            state.destination_x = number(event, 1);
            state.destination_y = number(event, 2);
            state.destination_width = number(event, 3);
            state.destination_height = number(event, 4);
            display().set_graph_zoom(
                th2::grp_script + static_cast<int>(*slot),
                state.destination_x * 800 / 640,
                state.destination_y * 600 / 448,
                state.destination_width * 800 / 640,
                state.destination_height * 600 / 448);
        }
    } else if (name == "SetBmpZoom2") {
        // AVG_SetBmpZoom2: DSP_SetGraphZoom2( g, cx, cy, zoom ) - a scale
        // about a point, which leaves the rectangle alone.
        if (const auto slot = overlay_index(number(event, 0))) {
            auto& state = overlay_states_[*slot];
            state.zoom_center_x = number(event, 1);
            state.zoom_center_y = number(event, 2);
            state.zoom = number(event, 3);
            display().set_graph_zoom2(
                th2::grp_script + static_cast<int>(*slot),
                state.zoom_center_x * 800 / 640,
                state.zoom_center_y * 600 / 448, state.zoom);
        }
    } else if (name == "C") {
        // ESC_EOprC: the defaults the opcode fills in before AVG_SetChar
        // ever sees them, then
        //   AVG_SetChar( P0, P1, P2, P4, P3, P5, P6, P7 )
        // - note layer and in_type arrive swapped round.
        int locate = number(event, 2);
        if (locate == -1) {
            const int held = chars().check_char_locate(number(event, 0));
            locate = held == -1 ? 1 : held;
        }
        int in_type = number(event, 3);
        if (in_type == -1) {
            in_type = th2::char_type_cfade;
        } else if (in_type == -2) {
            in_type = th2::char_type_direct;
        }
        const int layer = number(event, 4) == -1 ? 0 : number(event, 4);
        const int bright = number(event, 5) == -1
            ? th2::bright_neutral : number(event, 5);
        const int alph = number(event, 6) == -1 ? 256 : number(event, 6);
        const int frame = number(event, 7);
        chars().set_char(
            number(event, 0), number(event, 1), locate, layer, in_type,
            bright, alph, frame);
    } else if (name == "CW") {
        // ESC_EOprCW: the wait form, which does not hold the script and
        // passes CHAR_TYPE_WAIT with no frame count.
        int locate = number(event, 2);
        if (locate == -1) {
            const int held = chars().check_char_locate(number(event, 0));
            locate = held == -1 ? 1 : held;
        }
        const int layer = number(event, 3) == -1 ? 0 : number(event, 3);
        const int bright = number(event, 4) == -1
            ? th2::bright_neutral : number(event, 4);
        const int alph = number(event, 5) == -1 ? 256 : number(event, 5);
        chars().set_char(
            number(event, 0), number(event, 1), locate, layer,
            th2::char_type_wait, bright, alph, -1);
    } else if (name == "CR") {
        const int out_type = number(event, 1) == -1 ? 0 : number(event, 1);
        chars().reset_char(number(event, 0), out_type, number(event, 2));
    } else if (name == "CRW") {
        chars().reset_char(number(event, 0), th2::char_type_wait, -1);
    } else if (name == "CP") {
        const int in_type = number(event, 2) == -1 ? 0 : number(event, 2);
        chars().set_char_pose(
            number(event, 0), number(event, 1), in_type, -1);
    } else if (name == "CL") {
        chars().set_char_locate(
            number(event, 0), number(event, 1), number(event, 2));
    } else if (name == "CY") {
        chars().set_char_layer(number(event, 0), number(event, 1));
    } else if (name == "CB") {
        chars().set_char_bright(
            number(event, 0), number(event, 1), number(event, 2));
    } else if (name == "CA") {
        chars().set_char_alph(
            number(event, 0), number(event, 1), number(event, 2));
    } else if (name == "SetMessage2") {
        push_backlog();
        message_scroll_follow_ = true;
        message_.set(th2::substitute_player_name(
            text(event, 0), player_name_,
            runtime_.flag(213) != 0));
        current_backlog_voices_.clear();
        if (pending_backlog_voice_) {
            pending_backlog_voice_->start = 0;
            pending_backlog_voice_->end = message_.visible().size();
            current_backlog_voices_.push_back(*pending_backlog_voice_);
            pending_backlog_voice_.reset();
        }
        message_window_open_ = true;
        // void AVG_SetScenarioFlag( int block_no ), which ESC_EOprSetMessage2
        // calls right after AVG_SetNovelMessage:
        //
        //     if(BlockNo!=-1){ STD_SetBit( ScenarioFlag[i], 1, BlockNo, ON ); }
        //     BlockNo = block_no;
        //
        // It marks the block the reader has just finished, not the one about
        // to start - reaching the next message is what proves the last one
        // was read.  AVG_CheckScenarioFlag then asks about the *current*
        // BlockNo, which is how AVG_GetMesCut knows whether this line may be
        // skipped.  Marking on the click instead left almost nothing marked,
        // and the skip key stopped working on a second read-through.
        mark_current_text_read();
        current_line_key_ = runtime_.script_name() + ':'
            + std::to_string(runtime_.vm_pc());
        message_ends_block_ = number(event, 1) == 2;
        // AVG_SetNovelMessage( EscParam[0].str, EscParam[1].num ).  It sets
        // MSG_DISP, takes the half tone and shows the text; everything after
        // that is AVG_ControlNovelMessage's, one frame at a time.
        // The substituted text, not the script's.  AVG_SetNovelMessage
        // stores one string and TXT_GetTextCount walks that same string, so
        // the name has to be in it before anything is counted: *nnk is four
        // ASCII characters and the name it stands for is two full-width
        // ones, which is two counts of difference in every message that
        // mentions the player.
        msg().set_novel_message(
            th2::substitute_player_name(
                text(event, 0), player_name_, runtime_.flag(213) != 0),
            number(event, 1));
    } else if (name == "AddMessage2") {
        const auto reveal_start = message_.visible().size();
        message_.append(th2::substitute_player_name(
            text(event, 0), player_name_,
            runtime_.flag(213) != 0));
        if (pending_backlog_voice_) {
            pending_backlog_voice_->start = reveal_start;
            pending_backlog_voice_->end = message_.visible().size();
            current_backlog_voices_.push_back(*pending_backlog_voice_);
            pending_backlog_voice_.reset();
        }
        message_window_open_ = true;
        current_line_key_ = runtime_.script_name() + ':'
            + std::to_string(runtime_.vm_pc());
        message_ends_block_ = number(event, 1) == 2;
        static_cast<void>(reveal_start);
        // AVG_AddNovelMessage( EscParam[0].str, EscParam[1].num ).
        msg().add_novel_message(
            th2::substitute_player_name(
                text(event, 0), player_name_, runtime_.flag(213) != 0),
            number(event, 1));
    } else if (name == "T") {
        // ESC_EOprT, verbatim:
        //
        //     if(EscParam[1].num==-1) EscParam[1].num = OFF;
        //     if(EscParam[0].num){
        //         if(EscParam[1].num){ AVG_SetHalfTone(); }
        //         AVG_SetNovelMessageDisp(ON);
        //     }else{
        //         if(EscParam[1].num){ AVG_ResetHalfTone(); }
        //         AVG_SetNovelMessageDisp(OFF);
        //     }
        //
        // The second parameter is the half tone, and we had no half of it:
        // T was only hiding and showing the text, so a script that took the
        // text away for a moment left the wash sitting on the background
        // with nothing on top of it.
        const int tone = number(event, 1) == -1 ? 0 : number(event, 1);
        if (number(event, 0)) {
            if (tone) {
                msg().set_half_tone();
            }
            msg().set_novel_message_disp(true);
        } else {
            if (tone) {
                msg().reset_half_tone();
            }
            msg().set_novel_message_disp(false);
        }
    } else if (name == "K") {
        waiting_for_input_ = true;
        message_ends_block_ = true;
        auto_next_time_.reset();
    } else if (name == "Run") {
        // EXEC_OprRun: ESC_SetDrawFlag(), MainWindow.draw_flag=ON.
        th2::set_draw_flag_on();
    } else if (name == "W") {
        // Explicit no-op in the original.
    } else if (name == "M") {
        // ESC_EOprM defaults each parameter and then hands every case, the
        // negative one included, to AVG_PlayBGM:
        //     if(EscParam[1].num==-1) EscParam[1].num = 0;
        //     if(EscParam[2].num==-1) EscParam[2].num = ON;
        //     if(EscParam[3].num==-1) EscParam[3].num = 0xff;
        //     AVG_PlayBGM( EscParam[0].num, fade, loop, vol, 0 );
        const int music = number(event, 0);
        const int fade = number(event, 1) < 0 ? 0 : number(event, 1);
        const int loop = number(event, 2) < 0 ? 1 : number(event, 2);
        const int volume = number(event, 3) < 0 ? 255 : number(event, 3);
        play_bgm(music, loop != 0, volume, fade);
        if (music >= 0 && fade > 0) {
            bgm_.set_gain(0.0f);
            bgm_.fade_to(
                bgm_gain(volume), audio_fade_duration(fade));
        }
    } else if (name == "MS") {
        // ESC_EOprMS: the same -1 default, then AVG_StopBGM( fade ).
        stop_bgm(number(event, 0) < 0 ? 0 : number(event, 0));
    } else if (name == "MV") {
        bgm_volume_ = number(event, 0);
        const int fade = number(event, 1) < 0 ? 0 : number(event, 1);
        bgm_.fade_to(
            bgm_gain(bgm_volume_),
            audio_fade_duration(fade));
    } else if (name == "MW") {
        // AVG_WaitBGM (GM_Avg.cpp:2048):
        //     return PlayMusic[DB_No].mode==MUSIC_STOP
        //         || PlayMusic[DB_No].mode==MUSIC_PLAY;
        // It is done when the music is stopped OR steadily playing, and
        // waits only through an actual fade.  Nothing playing is MUSIC_STOP,
        // so there is nothing to wait for - where ours reported a fade on a
        // channel with no music in it and held the script an extra frame.
        // Not in a trace, where the fade is ours alone.  The reference's
        // music is stubbed (reference/shim, th2ref_pcm_status), so
        // PlayMusic[].mode is only ever MUSIC_STOP or MUSIC_PLAY and
        // AVG_WaitBGM is true the moment it is asked - the opcode asks it
        // itself inside EXEC_ControlLang, so the reference spends no tick on
        // MW at all.  Ours runs a real decoder on the injected clock, so it
        // really is mid-fade here and parked for a tick the other side never
        // spent: measured at pc 10191 of 010301100.sdt, the MW after MS 60,
        // where the reference goes straight from 10184 to 10193.
        if (!trace_mode_ && bgm_.fading() && bgm_.playing()) {
            audio_wait_ = AudioWait{AudioWaitKind::bgm, 0};
        }
    } else if (name == "SE") {
        play_se(-1, number(event, 0), false,
                number(event, 1) < 0 ? 255 : number(event, 1));
    } else if (name == "SEP") {
        play_se(
            number(event, 0), number(event, 1), number(event, 3) != 0,
            number(event, 4) < 0 ? 255 : number(event, 4),
            number(event, 2) < 0 ? 0 : number(event, 2));
    } else if (name == "SES") {
        const auto channel = number(event, 0);
        if (channel >= 0 && static_cast<std::size_t>(channel) < se_channels_.size()) {
            const int fade = number(event, 1) < 0 ? 0 : number(event, 1);
            trace_note_audio("sestop", channel, -1, number(event, 1), 0);
            se_channels_[channel].fade_to(
                0.0f, audio_fade_duration(fade), true);
            se_sound_[channel] = -1;
        }
    } else if (name == "SEV") {
        const auto channel = number(event, 0);
        if (channel >= 0 && static_cast<std::size_t>(channel) < se_channels_.size()) {
            se_volume_[channel] = number(event, 1);
            const int fade = number(event, 2) < 0 ? 0 : number(event, 2);
            se_channels_[channel].fade_to(
                se_gain(se_volume_[channel]),
                audio_fade_duration(fade));
        }
    } else if (name == "SEW") {
        const auto channel = number(event, 0);
        // In a trace the other side is not decoding anything: its effect
        // "plays" for a fixed count of ticks (th2ref_pcm_status), and
        // SeStruct[sno].flag is set for exactly that long.  Our decoder's
        // own idea of whether it is still playing is a different question -
        // it answered no at pc 4944 of 010302000.sdt, where the engine
        // waited, so the script ran straight through a SEW the engine
        // honoured.
        const bool wait_for_playback = channel >= 0
            && static_cast<std::size_t>(channel) < se_channels_.size()
            && !se_loop_[channel]
            && (trace_mode_
                    ? trace_se_playing(static_cast<std::size_t>(channel))
                    : se_channels_[channel].playing());
        if (wait_for_playback) {
            audio_wait_ = AudioWait{
                AudioWaitKind::sound_effect, static_cast<std::size_t>(channel)};
        }
    } else if (name == "VV" || name == "VA" || name == "VB"
               || name == "VC") {
        play_voice(event);
    } else if (name == "VI") {
        if (number(event, 2) != -1) {
            vi_event_voice_no_all_ = number(event, 2);
        } else if (number(event, 1) != -1) {
            vi_event_voice_no_ = number(event, 1);
        }
    } else if (name == "VS") {
        const auto channel = number(event, 1) < 0 ? 0 : number(event, 1);
        if (channel >= 0 && static_cast<std::size_t>(channel) < voice_channels_.size()) {
            const int fade = number(event, 0) < 0 ? 0 : number(event, 0);
            trace_note_audio("voicestop", channel, -1, -1, -1);
            voice_channels_[channel].fade_to(
                0.0f, audio_fade_duration(fade), true);
            voice_sound_[channel] = -1;
        }
    } else if (name == "VW") {
        const auto channel = number(event, 0) < 0 ? 0 : number(event, 0);
        if (channel >= 0 && static_cast<std::size_t>(channel) < voice_channels_.size()
            && voice_channels_[channel].playing()) {
            audio_wait_ = AudioWait{
                AudioWaitKind::voice, static_cast<std::size_t>(channel)};
        }
    } else if (name == "SetSelectMes") {
        choices_.push_back(Choice{
            interpret_newlines(th2::substitute_player_name(
                text(event, 0), player_name_,
                runtime_.flag(213) != 0)),
            number(event, 1),
            number(event, 2),
        });
    } else if (name == "SetSelect") {
        choice_result_register_ =
            std::get<th2::RegisterTarget>(event.arguments.at(0)).index;
        choosing_ = true;
        choice_reveal_cnt_ = 0;
        choice_highlight_ = 0;
        choice_selected_ = -1;
    } else if (name == "SetMapEvent") {
        map_events_.push_back(MapEvent{
            number(event, 0), number(event, 1), number(event, 2),
            text(event, 3)});
    } else if (name == "LoadScript") {
        reset_overlays();
        choices_.clear();
        choosing_ = false;
        choice_highlight_ = 0;
        choice_selected_ = -1;
        choice_result_register_ = -1;
        choice_ex_ = false;
        waiting_for_input_ = false;
        message_ends_block_ = false;
        load_script(text(event, 0));
    } else if (name == "SetSelectMes") {
        choices_.push_back(Choice{
            interpret_newlines(th2::substitute_player_name(
                text(event, 0), player_name_,
                runtime_.flag(213) != 0)),
            number(event, 1),
            number(event, 2),
        });
    } else if (name == "SetSelect") {
        choice_result_register_ =
            std::get<th2::RegisterTarget>(event.arguments.at(0)).index;
        choosing_ = true;
        choice_highlight_ = 0;
        choice_selected_ = -1;
    } else {
        return false;
    }
    return true;
}

std::filesystem::path Game::dump_engine_error(
    const th2::ScriptStep& step, std::string_view error)
{
    const auto logs_dir = writable_directory() / "logs";
    std::filesystem::create_directories(logs_dir);
    const auto now = std::chrono::system_clock::now();
    const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    const auto path = logs_dir / std::format("engine-error-{}.log", stamp);
    std::ofstream output(path);
    output << "error=" << error << '\n'
           << "script=" << step.script_name << '\n'
           << "pc=" << step.event.instruction.offset << '\n'
           << "next_pc=" << runtime_.vm_pc() << '\n'
           << "opcode=" << step.event.instruction.opcode << '\n'
           << "instruction=" << step.event.instruction.name << '\n'
           << "arguments=";
    for (std::size_t i = 0; i < step.event.arguments.size(); ++i) {
        if (i != 0) {
            output << ", ";
        }
        std::visit([&](const auto& argument) {
            using T = std::decay_t<decltype(argument)>;
            if constexpr (std::is_same_v<T, std::int32_t>) {
                output << argument;
            } else if constexpr (std::is_same_v<T, std::string>) {
                output << std::quoted(argument);
            } else if constexpr (std::is_same_v<T, th2::RegisterTarget>) {
                output << "register[" << static_cast<int>(argument.index) << ']';
            } else {
                output << "compare(register["
                       << static_cast<int>(argument.register_index)
                       << "], op=" << static_cast<int>(argument.operation)
                       << ", value=" << argument.value << ')';
            }
        }, step.event.arguments[i]);
    }
    output << "\nregisters=";
    for (const auto value : runtime_.vm_registers()) {
        output << value << ' ';
    }
    output << "\nstack=";
    for (const auto value : runtime_.vm_stack()) {
        output << value << ' ';
    }
    output << "\nbackground=" << bg_scene_
           << "\nbackground_kind="
           << static_cast<std::int32_t>(background_kind_)
           << "\ntone=" << tone_
           << "\ntone_back=" << tone_back_
           << "\ntone_char=" << tone_char_
           << "\nweather=" << weather_
           << "\nbgm=" << bgm_track_
           << "\nvoice_event=" << vi_event_voice_no_
           << "\nvoice_event_all=" << vi_event_voice_no_all_
           << "\nmessage=" << std::quoted(message_.visible()) << '\n';
    return path;
}

std::filesystem::path Game::dump_runtime_error(std::string_view error)
{
    const auto logs_dir = writable_directory() / "logs";
    std::filesystem::create_directories(logs_dir);
    const auto now = std::chrono::system_clock::now();
    const auto stamp = std::chrono::duration_cast<
        std::chrono::milliseconds>(now.time_since_epoch()).count();
    const auto path = logs_dir / std::format("engine-error-{}.log", stamp);
    std::ofstream output(path);
    const auto pc = runtime_.vm_pc();
    const auto bytecode = runtime_.vm_bytecode();
    output << "error=" << error << '\n'
           << "script=" << runtime_.script_name() << '\n'
           << "pc=" << pc << "\nbytecode=";
    const auto first = pc > 16 ? pc - 16 : 0;
    const auto last = std::min(bytecode.size(), pc + 32);
    output << std::hex << std::setfill('0');
    for (std::size_t offset = first; offset < last; ++offset) {
        if (offset == pc) {
            output << '[';
        }
        output << std::setw(2)
               << static_cast<unsigned>(bytecode[offset]);
        if (offset == pc + 1) {
            output << ']';
        }
        output << ' ';
    }
    output << std::dec << "\nregisters=";
    for (const auto value : runtime_.vm_registers()) {
        output << value << ' ';
    }
    output << "\nstack=";
    for (const auto value : runtime_.vm_stack()) {
        output << value << ' ';
    }
    output << "\nbackground=" << bg_scene_
           << "\nbackground_kind="
           << static_cast<std::int32_t>(background_kind_)
           << "\ntone=" << tone_
           << "\ntone_back=" << tone_back_
           << "\ntone_char=" << tone_char_
           << "\nweather=" << weather_
           << "\nbgm=" << bgm_track_
           << "\nvoice_event=" << vi_event_voice_no_
           << "\nvoice_event_all=" << vi_event_voice_no_all_
           << "\nmessage=" << std::quoted(message_.visible()) << '\n';
    return path;
}

void Game::pump_script()
{
    // main.cpp:
    //     if(ScriptFlag){ script = EXEC_ControlLang( &ScriptData ); }
    //     NextMainStep = MAIN_GameControl( script );
    // and only then MAIN_DrawGraph.  Every frame, unconditionally: a parked
    // instruction runs again and re-asks its wait, which is why nothing in
    // the engine ever has to resume the script.
    if (!script_flag_ || !running_) {
        return;
    }
    // EXEC_ControlTask checks the two wait forms before it runs anything:
    //     if( BusyFlg == SCCODE_WAIT_TWAIT ) EXEC_LangTWait();
    //     if( BusyFlg == SCCODE_WAIT_WAIT )  EXEC_LangWait();
    //     if( BusyFlg == SCCODE_RUN ) while( EXEC_CallOprControl( mode ) );
    if (wake_frames_ > 0) {
        wake_frames_ -= control_steps_;
        if (wake_frames_ > 0) {
            return;
        }
        wake_frames_ = 0;
    }
    if (wake_time_) {
        if (engine_now() < *wake_time_) {
            return;
        }
        wake_time_.reset();
    }
    if (ui_mode_ != UiMode::game || movie_ || clock_state_ || calendar_state_) {
        return;
    }
    exec_control_lang();
}

// The AVG_Wait* predicates, one per ESC_WAIT opcode.  True means the
// instruction stays where it is; the polarity is the original's, which is not
// uniform - AVG_WaitBack is true while busy, AVG_WaitNovelMessage is true when
// the message is ready.
bool Game::avg_wait_frame()
{
    // BOOL AVG_WaitFrame( void ), the type -1 arm, verbatim:
    //
    //     wait_max = AVG_EffCnt4(WaitStruct.max);
    //     WaitStruct.count++;
    //     if( WaitStruct.count>=wait_max ){ WaitStruct.flag = 0; ret = TRUE; }
    //
    // A frame counter.  We had this as a wall-clock deadline of
    // frames*1000/60 milliseconds, which is the same thing only while the
    // loop happens to run at exactly sixty frames a second - and is off by
    // the rounding even then: a WaitFrame 90 took 188 ticks against the
    // engine's 180.
    // Advanced by the frame's tick count rather than once per call: the
    // engine asks this from EXEC_ControlLang, which runs exactly once per
    // sixtieth, while our pump runs once per drawn frame.  At sixty frames a
    // second - and in a trace run, where a tick is the frame - the two are
    // the same thing and the count is identical.
    for (int i = 0; i < control_steps_ && wait_frame_.flag; ++i) {
        // Recomputed every frame, not cached at set-up: that is what lets a
        // skip key taken mid-wait collapse AVG_EffCnt4 to zero and end it.
        const int wait_max = effect_frames4(wait_frame_.max);
        //     if( WaitStruct.max==1 && AVG_GetMesCut() ){
        //         if(MainWindow.draw_flag<0){ }else{ MainWindow.draw_flag=-10; }
        //     }
        // A one-frame wait skipped over stops the screen for ten frames.
        if (wait_frame_.max == 1 && message_cut()
            && th2::engine_draw_flag >= 0) {
            th2::engine_draw_flag = -10;
        }
        ++wait_frame_.count;
        if (wait_frame_.count >= wait_max) {
            wait_frame_.flag = 0;
        }
    }
    return !wait_frame_.flag;
}

bool Game::opcode_waiting(WaitKind kind, const th2::Event& event)
{

    switch (kind) {
    case WaitKind::none:
    case WaitKind::frame:
        // No predicate: one frame of ESC_WAIT and the machine moves on.
        return false;
    case WaitKind::clock:
        // A pure query.  update_clock_calendar does the stepping, before the
        // script pass so that this sees the frame the animation is actually
        // on.  AVG_ViewClock returns TRUE at once when the clock is already at
        // the time asked for, and begin_clock leaves clock_state_ unset in
        // exactly that case, so this is false and nothing waits.
        return clock_state_.has_value() || calendar_state_.has_value();
    case WaitKind::character:
        // !AVG_WaitChar( EscParam[0].num ).  Its own character, and nothing
        // else - a background fade running at the same time cannot hold it.
        return avg_char_ && chars().wait_char(number(event, 0));
    case WaitKind::novel_message:
        // AVG_WaitNovelMessage(): NovelMessage.step1 == MSG_NEXT.  MSG_DISP
        // and MSG_WAIT both leave on AVG_GetMesCut(), so a held skip key
        // takes the message to MSG_NEXT on its own and nothing here has to
        // know about skipping.
        return !msg().wait_novel_message();
    case WaitKind::back:
        // !AVG_WaitBack(): BackStruct.fd_flag, the background change.
        return avgback().wait_back();
    case WaitKind::title:
        // The screen fades out with the script switched off, and the script
        // is let go on the frame *after* the fade lands - the reference sits
        // on pc 1648 of 500000000.sdt for thirty-two ticks, which is the
        // thirty frame fade plus the frame that starts it and the frame that
        // releases it.  Ours let go one frame early and then showed End for
        // a frame, which the reference never does: MAIN_SetScriptFlag(OFF)
        // means End is never reached at all.
        if (avgback().wait_fade()) {
            return true;
        }
        if (!goto_title_released_) {
            goto_title_released_ = true;
            return true;
        }
        return false;
    case WaitKind::fade:
        // !AVG_WaitFade(): FadeStruct.flag, the screen flash.
        return avgback().wait_fade();
    case WaitKind::back_fade:
        // !AVG_WaitBackFade(): BackStruct.br_flag.
        return avgback().wait_back_fade();
    case WaitKind::shake:
        // !AVG_WaitShake(): sk_speed and sk_flag both non-zero.
        return avgback().wait_shake();
    case WaitKind::back_scroll:
        // !AVG_WaitBackScroll(): BackStruct.sc_flag.
        return avgback().wait_back_scroll();
    case WaitKind::key:
        // AVG_WaitKey(): AVG_GetHitKey() || AVG_GetMesCut() || Avg.demo.
        return waiting_for_input_ && !message_cut() && !demo_mode_;
    case WaitKind::bgm:
        // AVG_WaitBGM(): the track is stopped or playing - that is, not
        // still fading - so MW holds only for the length of a fade.
        return audio_wait_.has_value();
    case WaitKind::se:
        // int AVG_WaitSe( int sno ), verbatim:
        //
        //     if(!Avg.se) return 0;
        //     if(SeStruct[sno].flag && SeStruct[sno].dno!=-1){
        //         if( AVG_GetMesCut() ){ AVG_StopSE2( sno, 0 ); return FALSE; }
        //         else return !SeStruct[sno].loop;
        //     }
        //     return FALSE;
        //
        // Two things we did not have.  With sound effects off it never
        // waits at all, and a held skip key *stops the sound* rather than
        // waiting it out - which is why skipping past a long effect in the
        // original is silent instead of dragging its tail along.
        if (config_.se_volume <= 0) {
            return false;
        }
        if (message_cut()) {
            // AVG_StopSE2( sno, 0 ) - the sound is cut, not waited out.
            // The stop itself is done by the caller, which is not const.
            se_cut_pending_ = true;
            return false;
        }
        return audio_wait_.has_value();
    case WaitKind::voice:
        // int AVG_WaitVoice( int vc_no ): with voices off it returns 1 -
        // ready - so VW never holds the script when the reader has turned
        // them off.
        if (config_.voice_volume <= 0) {
            return false;
        }
        return audio_wait_.has_value();
    case WaitKind::movie:
        // !AVG_WaitMovie(): movPlayerFrm && !bEnd.
        // trace_movie_live_ as well, and not as a nicety: a traced run never
        // builds a VideoPlayer (see Game::start_movie), so movie_ is null from
        // the start and this predicate said "not waiting" on the very first
        // evaluation.  The script then walked straight past SetMovie no matter
        // what the stub did, which is why giving the stub a length changed
        // nothing at all.  The stub is the movie in a trace, so it is what the
        // wait has to ask about.
        return movie_ != nullptr || trace_movie_live_;
    case WaitKind::wait_frame:
        // !AVG_WaitFrame().  Asking is what advances it, so this must be
        // reached exactly once a frame - which is what the latch above
        // guarantees.
        return !avg_wait_frame();
    case WaitKind::select:
        // AVG_WaitSelect() != -1: SelectWindow.res.
        return choosing_;
    }
    return false;
}

void Game::advance(bool skipping)
{
    if (wake_time_ || audio_wait_ || transition_ || back().br_flag
        || screen_flash_
        || (back().sk_flag && back().sk_speed > 0)
        || background_scroll_ || character_animation_active()
        || clock_state_ || calendar_state_
        || movie_) {
        return;
    }
    // A waiting instruction has already run its set-up, so re-entering the
    // machine here would run it again.  The loop below picks it up from the
    // latch instead, which is what EOprFlag is for.

    // The reveal, the page turn and the read mark are AVG_ControlNovelMessage's
    // now.  A click is an edge in GameKey, which it reads on the same frame;
    // this is left with the two things that are not the message machine's -
    // the choice the player picked, and the autosave at a page end.
    just_advanced_past_block_end_ = false;
    if (msg().state().step1 == th2::msg_stop && message_ends_block_) {
        just_advanced_past_block_end_ = true;
    }
    if (choosing_) {
        if (choice_selected_ < 0) {
            return;
        }
        if (choice_ex_) {
            load_script(choices_.at(choice_selected_).sno);
        } else if (choice_result_register_ >= 0) {
            runtime_.set_reg(
                static_cast<std::size_t>(choice_result_register_),
                choice_selected_);
        }
        choices_.clear();
        choosing_ = false;
        choice_highlight_ = 0;
        choice_selected_ = -1;
        choice_result_register_ = -1;
        choice_ex_ = false;
    }
    // The click does not run the machine.  AVG_GetGameKey only fills in
    // GameKey; EXEC_ControlLang runs at the top of the frame and
    // AVG_ControlNovelMessage reacts after it, so what a click does is change
    // the state the parked instruction is about to ask about.  pump_script()
    // is a few lines further down the same frame, which is where it lands.
    static_cast<void>(skipping);
}

// EXEC_ControlLang / EXEC_ControlTask.  main.cpp runs this once at the top of
// every frame while ScriptFlag is on, and the machine runs instructions until
// one of them is ESC_WAIT:
//
//     if(ScriptFlag){ script = EXEC_ControlLang( &ScriptData ); }
//     NextMainStep = MAIN_GameControl( script );
//
// It is not a resumption and there is no callback: a parked instruction is
// simply run again, and re-asks its own wait.
void Game::exec_control_lang(bool skipping)
{
    while (running_) {
        th2::ScriptStep step;
        try {
            step = runtime_.run();
        } catch (const std::exception& error) {
            const auto dump = dump_runtime_error(error.what());
            throw std::runtime_error(std::format(
                "{}:{}: {} (state dumped to {})",
                runtime_.script_name(), runtime_.vm_pc(),
                error.what(), dump.string()));
        }
        sync_game_flags();
        if (step.reason == th2::VmYield::ended) {
            // Nothing is running until something else is loaded.  Cleared by
            // load_script; a scheduled load clears it on the way through, and
            // a map or calendar leaves it set, which is the engine's null
            // EXEC_LangInfo.
            script_ended_ = true;
            if (goto_title_pending_) {
                goto_title_pending_ = false;
                return_to_title();
                break;
            }
            if (replay_mode_ || direct_scenario_) {
                return_to_title();
                break;
            }
            if (!load_scheduled_script()) {
                return_to_title();
                break;
            }
            // The tick ends here, whatever was loaded.  A script arriving and
            // its first instruction running are two different frames in the
            // engine - which is what the LoadScript opcode's ESC_WAIT buys,
            // and the scheduled load has to cost the same.  Running straight
            // on made each of the four EV_0301* interval scripts one tick
            // instead of two, so the day's scenario started four ticks early
            // and everything after it was measured against the wrong frame.
            break;
        }
        if (step.reason == th2::VmYield::wait_frames
            || step.reason == th2::VmYield::wait_time) {
            if (skipping) {
                continue;
            }
            if (step.reason == th2::VmYield::wait_frames) {
                // Plus the frame it is issued on.  The decrement below runs
                // at the top of pump_script, so a wait set during tick T is
                // first counted at T+1 and would resume at T+N - one frame
                // before the engine, which spends T itself waiting.  Measured
                // at pc 148 of 070000200.sdt: N left us a tick ahead, N+1
                // lands on it.
                wake_frames_ = step.wait_value + 1;
            } else {
                wake_time_ = engine_now()
                    + std::chrono::milliseconds(step.wait_value);
            }
            break;
        }
        if (step.reason == th2::VmYield::frame) {
            wake_time_ = engine_now();
            break;
        }
        if (step.reason == th2::VmYield::event) {
            // EXEC_CallOprControl, which is where ESC_WAIT lives.  An
            // ESC_NOWAIT opcode runs and the machine goes straight on; an
            // ESC_WAIT one runs its set-up once, re-asks its own wait every
            // frame after that, and stops the machine for the frame either
            // way.  The program counter only moves when the wait clears, so
            // a waiting instruction executes again next frame rather than
            // needing anything to resume it.
            const auto name = std::string_view(step.event.instruction.name);
            const auto kind = opcode_wait_kind(name);
            const auto opcode = step.event.instruction.opcode;
            const int phases = opcode_phases(name);
            auto& latch = eopr_flag_[opcode];

            if (kind == WaitKind::none || latch < phases) {
                if (kind != WaitKind::none) {
                    // EOprFlag[ESC_x] = 1 - or 2, for the four background
                    // opcodes that clear the old wash a frame before they
                    // set the new picture.
                    ++latch;
                    opcode_phase_ = latch;
                } else {
                    opcode_phase_ = 1;
                }
                try {
                    if (!handle(step.event)) {
                        throw std::runtime_error(std::format(
                            "unimplemented event opcode: {}",
                            step.event.instruction.name));
                    }
                } catch (const std::exception& error) {
                    const auto dump = dump_engine_error(step, error.what());
                    throw std::runtime_error(std::format(
                        "{}:{}: {} (state dumped to {})",
                        step.script_name, step.event.instruction.offset,
                        error.what(), dump.string()));
                }
            }

            if (kind == WaitKind::none) {
                // ESC_NOWAIT: `while( EXEC_CallOprControl( mode ) )` keeps
                // going, so the next instruction runs in this same frame.
                if (clock_state_ || calendar_state_ || choosing_
                    || wake_time_ || ui_mode_ != UiMode::game) {
                    break;
                }
                continue;
            }

            const bool waiting =
                latch < phases || opcode_waiting(kind, step.event);
            // The cut belongs to the wait that asked for it, on this frame:
            // AVG_WaitSe stops the sound from inside the check.  Applied on
            // the next pass instead, the stop landed a tick after the
            // reference's.
            if (se_cut_pending_) {
                se_cut_pending_ = false;
                if (audio_wait_) {
                    // AVG_StopSE2( sno, 0 ), all of it: the stop is logged
                    // like any other, and the channel is free again - the
                    // engine clears SeStruct[sno].flag, so a later SEW on it
                    // finds nothing playing.
                    const auto channel = audio_wait_->channel;
                    if (audio_wait_->kind == AudioWaitKind::sound_effect
                        && channel < se_channels_.size()) {
                        trace_note_audio(
                            "sestop", static_cast<int>(channel), -1, 0, 0);
                        se_sound_[channel] = -1;
                        trace_sound_started_.erase(channel);
                    }
                    waited_audio_channel().stop();
                    audio_wait_.reset();
                }
            }
            if (!waiting) {
                latch = 0;            // EXEC_AddPC: run() already moved it
                if (goto_title_pending_) {
                    // The engine never runs the End after a SetTitle - the
                    // script is off - so the machine stops here rather than
                    // stepping onto it for a frame.
                    goto_title_pending_ = false;
                    goto_title_released_ = false;
                    script_ended_ = true;
                    // AVG_ControlGotoTitle, GOTILE_END:
                    //     AVG_StopBGM( FADE_MUS );
                    //     for(i=0;i<WAVE_SOUND_NUM;i++) AVG_StopSE2( i,0 );
                    stop_bgm(60);
                    stop_all_se(0);
                    return_to_title();
                    break;
                }
            } else {
                runtime_.vm_rewind_to(step.event.instruction.offset);
            }
            break;                    // ESC_WAIT: the frame is over
        }
    }
    // The interpreter has stopped for now, which is exactly when there is
    // time to fetch what it will ask for next.  The scan itself happens on
    // the next frame (see iterate()); this only asks for it.
    prefetch_scan_pending_ = true;
    // Autosave after advancing past a block end, if enabled and enough time has passed.
    if (just_advanced_past_block_end_) {
        just_advanced_past_block_end_ = false;
        if (config_.autosave_enabled && ui_mode_ == UiMode::game
            && !replay_mode_ && !demo_mode_) {
            const auto now = engine_now();
            constexpr auto minimum_interval = std::chrono::minutes(2);
            if (last_save_time_.time_since_epoch().count() == 0
                || now - last_save_time_ >= minimum_interval) {
                if (!save_snapshot_) {
                    save_snapshot_ = capture_frame_thumbnail(
    save_thumbnail_width, save_thumbnail_height);
                }
                perform_autosave();
                // Refresh the save snapshot so the next autosave has an
                // up-to-date thumbnail.
                save_snapshot_.reset();
            }
        }
    }
}


}  // namespace th2app
