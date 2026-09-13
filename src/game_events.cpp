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
            reset_half_tone();
            message_visible_ = false;
        }
        if (!split || opcode_phase_ == 2) {
            if (unchanged_direct) {
                return true;
            }
            begin_transition(
                number(event, 0), number(event, 3), number(event, 6), true);
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
                message_visible_ = false;
            }
            if (!split || opcode_phase_ == 2) {
                if (unchanged_direct) {
                    return true;
                }
                begin_transition(
                    number(event, 0), number(event, 3), number(event, 7), true);
                set_cg(event, BackgroundKind::hcg, 'h');
            }
        }
    } else if (name == "V" || name == "VT") {
        const int visual = number(event, 1) * 10
            + std::max<std::int32_t>(0, number(event, 2));
        const bool unchanged_direct =
            number(event, 0) == -1
            && has_background()
            && bg_scene_ == visual;
        const bool split = name == "V";
        if (!split || opcode_phase_ == 1) {
            if (unchanged_direct) {
                return true;
            }
            reset_half_tone();
            message_visible_ = false;
        }
        if (!split || opcode_phase_ == 2) {
            if (unchanged_direct) {
                return true;
            }
            begin_transition(
                number(event, 0), number(event, 3), number(event, 7), true);
            set_cg(event, BackgroundKind::visual, 'v');
        }
    } else if (name == "FB") {
        message_visible_ = false;
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
            std::chrono::steady_clock::now(),
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
        if ((number(event, 0) == 0 || number(event, 0) == 6)
            && number(event, 2) != 0) {
            message_visible_ = false;
        }
        // BOOL AVG_SetShake( type, pich, speed, dir, swing ).  The speed
        // stays the raw number the script wrote; AVG_EffCnt4 is re-asked on
        // every pass of AVG_ControlShake, so zero is an endless shake and a
        // held skip key ends any of them on the next pass.
        avgback().set_shake(
            number(event, 0), number(event, 1), std::max(0, number(event, 2)),
            number(event, 3),
            event.arguments.size() > 4 && number(event, 4) >= 0
                ? number(event, 4) : 256);
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
        // AVG_EffCnt4 counts these in 30fps units (cnt * Avg.frame / 30
        // frames, i.e. cnt/30 seconds at any frame rate), unlike the effect
        // durations, which are 60fps based.  The values in the scripts agree:
        // 15, 30, 60, 90, 120 are half a second through five seconds.
        // ...and AVG_EffCnt4 is zero while the message is being cut, so a
        // skipped wait is no wait.
        const int frames = effect_frames4(number(event, 0));
        wake_time_ = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(frames * 1000 / 60);
    } else if (name == "WaitTime") {
        const auto now = static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        const auto deadline = static_cast<std::uint32_t>(number(event, 0));
        const auto remaining = static_cast<std::int32_t>(deadline - now);
        if (remaining > 0) {
            wake_time_ = std::chrono::steady_clock::now()
                + std::chrono::milliseconds(remaining);
        }
    } else if (name == "SetMovie") {
        start_movie(number(event, 0), 0, true);
    } else if (name == "SetEnding") {
        const int ending = number(event, 1) == 1 || number(event, 0) == 10
            ? 0 : number(event, 0);
        start_movie(1, ending, true);
    } else if (name == "SetTitle") {
        return_to_title();
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
        msg().set_novel_message(text(event, 0), number(event, 1));
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
        msg().add_novel_message(text(event, 0), number(event, 1));
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
    } else if (name == "W") {
        // Explicit no-op in the original.
    } else if (name == "M") {
        const int music = number(event, 0);
        const int fade = number(event, 1) < 0 ? 0 : number(event, 1);
        if (music < 0) {
            bgm_.fade_to(
                0.0f, audio_fade_duration(fade), true);
            bgm_track_ = -1;
        } else {
            const int loop = number(event, 2) < 0 ? 1 : number(event, 2);
            const int volume = number(event, 3) < 0 ? 255 : number(event, 3);
            play_bgm(music, loop != 0, volume);
            if (fade > 0) {
                bgm_.set_gain(0.0f);
                bgm_.fade_to(
                    bgm_gain(volume),
                    audio_fade_duration(fade));
            }
        }
    } else if (name == "MS") {
        const int fade = number(event, 0) < 0 ? 0 : number(event, 0);
        bgm_.fade_to(
            0.0f, audio_fade_duration(fade), true);
        bgm_track_ = -1;
    } else if (name == "MV") {
        bgm_volume_ = number(event, 0);
        const int fade = number(event, 1) < 0 ? 0 : number(event, 1);
        bgm_.fade_to(
            bgm_gain(bgm_volume_),
            audio_fade_duration(fade));
    } else if (name == "MW") {
        if (bgm_.fading()) {
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
        const bool wait_for_playback = channel >= 0
            && static_cast<std::size_t>(channel) < se_channels_.size()
            && !se_loop_[channel] && se_channels_[channel].playing();
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
        choice_reveal_started_ = std::chrono::steady_clock::now();
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
    if (wake_time_) {
        if (std::chrono::steady_clock::now() < *wake_time_) {
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
bool Game::opcode_waiting(WaitKind kind, const th2::Event& event) const
{
    switch (kind) {
    case WaitKind::none:
    case WaitKind::frame:
        // No predicate: one frame of ESC_WAIT and the machine moves on.
        return false;
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
        return movie_ != nullptr;
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
            if (replay_mode_ || direct_scenario_) {
                return_to_title();
                break;
            }
            if (!load_scheduled_script()) {
                return_to_title();
                break;
            }
            if (ui_mode_ == UiMode::map || calendar_state_) {
                break;
            }
            continue;
        }
        if (step.reason == th2::VmYield::wait_frames
            || step.reason == th2::VmYield::wait_time) {
            if (skipping) {
                continue;
            }
            const auto milliseconds = step.reason == th2::VmYield::wait_frames
                ? step.wait_value * 1000 / 60
                : step.wait_value;
            wake_time_ = std::chrono::steady_clock::now()
                + std::chrono::milliseconds(milliseconds);
            break;
        }
        if (step.reason == th2::VmYield::frame) {
            wake_time_ = std::chrono::steady_clock::now();
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

            if (se_cut_pending_) {
                se_cut_pending_ = false;
                if (audio_wait_) {
                    waited_audio_channel().stop();
                    audio_wait_.reset();
                }
            }
            if (latch >= phases && !opcode_waiting(kind, step.event)) {
                latch = 0;            // EXEC_AddPC: run() already moved it
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
            const auto now = std::chrono::steady_clock::now();
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
