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

bool Game::steady_for_save() const
{
    // The port's own saves - the autosave and F5 - come from the middle of
    // play.  The engine's save screen only opens through AVG_GoConfig, which
    // AVG_ConfigCheck allows only on a settled scene; these are taken at
    // moments nothing has vetted, so they ask for the same and for none of
    // the port's own animations to be part-way through.
    return !(replay_mode_ || wake_time_ || audio_wait_
             || (transition_ && !transition_->menu)
             || back().br_flag || screen_flash_
             || (shake_ && shake_->frames > 0)
             || background_scroll_ || character_animation_active()
             || clock_state_ || calendar_state_ || movie_);
}

void Game::save(int slot)
{
    // SAV_Save: whatever the save screen asks for is written.  It used to
    // refuse whenever something looked mid-way, and the save screen's own
    // fade-in - driven by the back-change counter, which stands still while
    // the engine sits in its config step underneath - always did, so no
    // manual save was ever made.
    const auto save_dir = save_directory();
    std::filesystem::create_directories(save_dir);
    const auto path = save_path(slot);
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return;
    }
    save_body(file);
    file.close();
    save_preview(slot);
    last_save_time_ = std::chrono::steady_clock::now();
}

bool Game::load(int slot)
{
    const auto path = save_path(slot);
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    if (!load_body(file)) {
        return false;
    }
    engine_config_loaded();
    return true;
}

bool Game::save_loadable(int slot) const
{
    std::ifstream file(save_path(slot), std::ios::binary);
    if (!file) {
        return false;
    }
    const auto version = read_u32(file);
    return file && version >= oldest_supported_save_version_
        && version <= save_version_;
}

void Game::begin_load(int slot)
{
    // void AVG_SetLoad( int save_no ):
    //     LoadNo = save_no;
    //     AVG_FadeSeAll( 15 );
    //     MAIN_SetScriptFlag( OFF );
    //     AVG_SetFade( 0, 0, 0, ON, -1 );
    //     AVG_ChangeSetp( 1, LOAD_FADEOUT );
    //     MUS_ResetMouseRect_All( ); MUS_SetMouseLayer( 0 );
    load_slot_ = slot;
    stop_all_se(15);
    script_flag_ = false;
    avgback().set_fade(0, 0, 0, 1, -1);
    // FadeStruct only counts; the darkening is drawn by the overlay that
    // reads it (Game::screen_flash_alpha), in black.
    screen_flash_ = ScreenFlash{0, 0, 0, 1, 1, engine_now()};
    load_step_next_ = LoadStep::fadeout;
    for (int layer = 0; layer < th2::mouse_layers; ++layer) {
        msg().reset_mouse_rect_layer(layer);    // MUS_ResetMouseRect_All
    }
    msg().set_mouse_layer(0);
}

void Game::control_load()
{
    // void AVG_ControlLoad( void ).
    switch (load_step_) {
    case LoadStep::none:
        break;
    case LoadStep::fadeout:
        // if( !AVG_WaitFade() ){ AVG_Init(); AVG_SetBackFadeDirect(...);
        //     SAV_Load( LoadNo ); ... MAIN_SetScriptFlag( OFF );
        //     AVG_ChangeSetp( 0, MapStep ? AVG_MAP : AVG_GAME );
        //     AVG_ChangeSetp( 1, LOAD_WORK ); }
        if (!avgback().wait_fade()) {
            if (!load(load_slot_)) {
                // Checked before the fade began (save_loadable), so only a
                // file that changed under us lands here: back to the light,
                // with the script as it was.
                avgback().set_fade(th2::bright_neutral, th2::bright_neutral,
                                   th2::bright_neutral, 0, -2);
                load_step_ = LoadStep::none;
                break;
            }
            script_flag_ = false;
            ui_mode_ = UiMode::game;
            //     if(MapStep){ MapStep=0; DSP_SetTextDisp( TXT_WINDOW, OFF );
            //                  AVG_ChangeSetp( 0, AVG_MAP ); }
            // The map from its step 0: set up again from the saved events,
            // the clock, then the fade-in.
            if (loaded_on_map_) {
                msg().set_novel_message_disp(false);
                begin_map();
            }
            // AVG_Init zeroes FadeStruct - black - so the frame between the
            // two legs stays dark.  The load dropped our overlay with the
            // rest of the play state; it is what draws FadeStruct.
            avgback().set_bright(0, 0, 0);
            screen_flash_ = ScreenFlash{0, 0, 0, 1, 1, engine_now()};
            load_step_ = LoadStep::work;
        }
        break;
    case LoadStep::work:
        load_work_tick_ = true;
        // AVG_SetBright( 0, 0, 0 ); AVG_SetFade( 128, 128, 128, OFF, -2 );
        avgback().set_bright(0, 0, 0);
        avgback().set_fade(th2::bright_neutral, th2::bright_neutral,
                           th2::bright_neutral, 0, -2);
        screen_flash_ = ScreenFlash{0, 0, 0, 1, 1, engine_now()};
        load_step_ = LoadStep::fadein;
        break;
    case LoadStep::fadein:
        // if( !AVG_WaitFade() ){ ... MAIN_SetScriptFlag( ON ); LOAD_NOT }
        // A load made from the system menu came in with the script stopped
        // by AVG_GoConfig; this is what starts it again.
        if (!avgback().wait_fade()) {
            script_flag_ = true;
            load_step_ = LoadStep::none;
        }
        break;
    }
}

std::filesystem::path Game::save_directory() const
{
    // A trace keeps its saves with its output: it starts with none, as the
    // reference's run does once its save_*.sav are cleared, and leaves the
    // player's own alone.
    if (trace_mode_) {
        return trace_dir_ / "save";
    }
    return writable_directory() / "save";
}

std::filesystem::path Game::save_path(int slot) const
{
    return save_directory()
        / std::format("save_{:02d}.sav", slot);
}

std::filesystem::path Game::thumbnail_path(int slot) const
{
    return save_directory()
        / std::format("save_{:02d}.bmp", slot);
}

std::filesystem::path Game::metadata_path(int slot) const
{
    return save_directory()
        / std::format("save_{:02d}.meta", slot);
}

void Game::save_preview(int slot)
{
    if (save_snapshot_) {
        // Captured straight to thumbnail size on the GPU.  That size comes
        // from the art target, which is fixed, so it does not vary with the
        // window or the display's pixel ratio - and the save screen scales
        // whatever it loads into its slot anyway.
        SDL_SaveBMP(
            save_snapshot_.get(), thumbnail_path(slot).string().c_str());
    } else if ((thumbnail_readback_ && thumbnail_readback_->pending())
               || snapshot_after_draw_) {
        // Still to be taken, or on its way from the GPU: written when it
        // arrives.
        snapshot_slots_.push_back(slot);
    }
    std::ofstream metadata(metadata_path(slot));
    if (metadata) {
        // SAV_CreateSaveHead: strncpy( buf, DSP_GetTextDispStr(TXT_WINDOW),
        // 18 ), cut at the first '\n' - the start of the line on screen,
        // eighteen bytes of Shift-JIS: eighteen half-width characters or
        // nine full-width ones.  Measured that way here, so a Japanese line
        // is not cut to six characters by UTF-8's three bytes apiece.  The
        // port's own Message, which this used to read, no longer plays.
        const auto raw = msg().save_data().str;
        std::string excerpt;
        //     if(MapStep){ strcpy( buf, "　ＭＡＰ選択" ); }
        const bool on_map = engine_config_from_map_ || ui_mode_ == UiMode::map;
        if (on_map) {
            excerpt = "\u3000\uFF2D\uFF21\uFF30\u9078\u629E";
        }
        int sjis_bytes = 0;
        for (std::size_t i = 0; !on_map && i < raw.size();) {
            const auto c = static_cast<unsigned char>(raw[i]);
            if (c == '\n' || (c == '\\' && i + 1 < raw.size()
                                && raw[i + 1] == 'n')) {
                break;
            }
            if (c == '\\') {
                i += 2;   // any other tag: not shown
                continue;
            }
            const std::size_t length = c < 0x80 ? 1 : c >= 0xF0 ? 4
                : c >= 0xE0 ? 3 : 2;
            // Half-width katakana (U+FF61..FF9F) is one byte in Shift-JIS.
            const bool half_kana = length == 3 && c == 0xEF
                && i + 2 < raw.size()
                && ((static_cast<unsigned char>(raw[i + 1]) == 0xBD
                     && static_cast<unsigned char>(raw[i + 2]) >= 0xA1)
                    || static_cast<unsigned char>(raw[i + 1]) == 0xBE);
            const int width = c < 0x80 || half_kana ? 1 : 2;
            if (sjis_bytes + width > 18) {
                break;
            }
            sjis_bytes += width;
            excerpt.append(raw, i, length);
            i += length;
        }
        const std::size_t cut = excerpt.size();
        metadata << std::time(nullptr) << '\n'
                 << runtime_.flag(0) << ' ' << runtime_.flag(1) << ' '
                 << runtime_.flag(2) << '\n'
                 << excerpt.substr(0, cut) << '\n';
    }
}

Game::SaveMetadata Game::read_save_metadata(int slot) const
{
    SaveMetadata result;
    result.exists = std::filesystem::exists(save_path(slot));
    if (!result.exists) {
        return result;
    }
    std::ifstream metadata(metadata_path(slot));
    if (metadata) {
        long long timestamp = 0;
        metadata >> timestamp;
        metadata.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        auto after_timestamp = metadata.tellg();
        if (metadata >> result.game_month >> result.game_day
                     >> result.game_time) {
            metadata.ignore(
                std::numeric_limits<std::streamsize>::max(), '\n');
        } else {
            metadata.clear();
            metadata.seekg(after_timestamp);
        }
        std::getline(metadata, result.message);
        result.timestamp = static_cast<std::time_t>(timestamp);
    }
    if (result.timestamp == 0) {
        const auto written = std::filesystem::last_write_time(save_path(slot));
        const auto sys_time = std::chrono::file_clock::to_sys(written);
        result.timestamp = std::chrono::system_clock::to_time_t(
            std::chrono::time_point_cast<std::chrono::system_clock::duration>(sys_time));
    }
    return result;
}

void Game::perform_autosave()
{
    // Autosave slots are 100-109, reusing the same save/load infrastructure.
    // Priority: lowest-numbered empty slot, then oldest occupied slot.
    constexpr int autosave_base = 100;
    int target_slot = -1;
    std::time_t oldest_time = std::numeric_limits<std::time_t>::max();
    for (int i = 0; i < 10; ++i) {
        const auto metadata = read_save_metadata(autosave_base + i);
        if (!metadata.exists) {
            target_slot = autosave_base + i;
            break;
        }
        if (metadata.timestamp < oldest_time) {
            oldest_time = metadata.timestamp;
            target_slot = autosave_base + i;
        }
    }
    if (target_slot < 0) {
        return;
    }
    save(target_slot);
}

void Game::refresh_save_page()
{
    constexpr int autosave_base = 100;
    // Page 10 (displayed as 11/11) shows autosave slots 100-109.
    if (save_page_ == 10) {
        newest_save_slot_ = -1;
        std::time_t newest_time = 0;
        for (int slot = 0; slot < 100; ++slot) {
            const auto metadata = read_save_metadata(slot);
            if (metadata.exists && metadata.timestamp >= newest_time) {
                newest_time = metadata.timestamp;
                newest_save_slot_ = slot;
            }
        }
        for (int i = 0; i < 10; ++i) {
            const auto metadata = read_save_metadata(autosave_base + i);
            if (metadata.exists && metadata.timestamp >= newest_time) {
                newest_time = metadata.timestamp;
                newest_save_slot_ = autosave_base + i;
            }
        }
        for (int i = 0; i < 10; ++i) {
            const int slot = autosave_base + i;
            visible_saves_[i] = read_save_metadata(slot);
            save_thumbnails_[i].reset();
            if (!visible_saves_[i].exists) {
                continue;
            }
            SDL_Surface* surface =
                SDL_LoadBMP(thumbnail_path(slot).string().c_str());
            if (surface) {
                save_thumbnails_[i] = texture_from_surface(surface);
                SDL_DestroySurface(surface);
            }
        }
        return;
    }
    newest_save_slot_ = -1;
    std::time_t newest_time = 0;
    for (int slot = 0; slot < 100; ++slot) {
        const auto metadata = read_save_metadata(slot);
        if (metadata.exists && metadata.timestamp >= newest_time) {
            newest_time = metadata.timestamp;
            newest_save_slot_ = slot;
        }
    }
    for (int i = 0; i < 10; ++i) {
        const auto metadata = read_save_metadata(autosave_base + i);
        if (metadata.exists && metadata.timestamp >= newest_time) {
            newest_time = metadata.timestamp;
            newest_save_slot_ = autosave_base + i;
        }
    }
    for (int i = 0; i < 10; ++i) {
        const int slot = save_page_ * 10 + i;
        visible_saves_[i] = read_save_metadata(slot);
        save_thumbnails_[i].reset();
        if (!visible_saves_[i].exists) {
            continue;
        }
        SDL_Surface* surface =
            SDL_LoadBMP(thumbnail_path(slot).string().c_str());
        if (surface) {
            save_thumbnails_[i] = texture_from_surface(surface);
            SDL_DestroySurface(surface);
        }
    }
}

void Game::save_body(std::ostream& out) const
{
    write_u32(out, save_version_);  // native version

    // Script identity
    write_str(out, runtime_.script_name(), 64);
    write_i32(out, tone_);
    write_i32(out, tone_back_);
    write_i32(out, tone_char_);
    write_i32(out, weather_);
    write_i32(out, vi_event_voice_no_);
    write_i32(out, vi_event_voice_no_all_);
    write_i32(out, demo_mode_ ? 1 : 0);
    write_i32(out, demo_delay_frames_);
    write_i32(out, skipped_month_);
    write_i32(out, skipped_day_);
    write_i32(out, shake_.has_value() ? 1 : 0);
    if (shake_) {
        write_i32(out, shake_->type);
        write_i32(out, shake_->pitch);
        write_i32(out, shake_->frames);
        write_i32(out, shake_->direction);
        write_i32(out, shake_->swing);
        write_i32(out, shake_->sampled_frame);
        const auto elapsed = std::chrono::duration_cast<
            std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - shake_->started).count();
        write_i64(out, elapsed);
    }
    write_i32(out, sakura_.has_value() ? 1 : 0);
    write_u32(out, sakura_random_);
    if (sakura_) {
        write_i32(out, sakura_->amount);
        write_i32(out, sakura_->target_amount);
        write_i32(out, static_cast<std::int32_t>(sakura_->wind * 1000.0f));
        write_i32(out, sakura_->speed);
        write_i32(out, sakura_->tick);
        write_i32(out, sakura_->reset_frames);
        write_i32(out, sakura_->no_reset ? 1 : 0);
        for (int i = 0; i < sakura_->amount; ++i) {
            const auto& petal = sakura_->petals[i];
            write_i32(out, petal.active ? 1 : 0);
            write_i32(out, petal.type);
            write_i32(out, static_cast<std::int32_t>(petal.x * 1000.0f));
            write_i32(out, static_cast<std::int32_t>(petal.y * 1000.0f));
            write_i32(
                out, static_cast<std::int32_t>(petal.axis_x * 1000.0f));
            write_i32(
                out, static_cast<std::int32_t>(petal.axis_y * 1000.0f));
            write_u32(out, petal.counter);
        }
    }

    // VM state
    write_u32(out, static_cast<std::uint32_t>(runtime_.vm_pc()));
    const auto registers = runtime_.vm_registers();
    for (const auto r : registers) {
        write_i32(out, r);
    }
    const auto stack = runtime_.vm_stack();
    write_u32(out, static_cast<std::uint32_t>(stack.size()));
    for (const auto s : stack) {
        write_i32(out, s);
    }

    // Flags
    for (const auto f : runtime_.all_flags()) {
        write_i32(out, f);
    }

    // Background
    write_i32(out, bg_scene_);
    write_i32(out, static_cast<std::int32_t>(background_kind_));
    write_i32(out, static_cast<std::int32_t>(background_view_.x));
    write_i32(out, static_cast<std::int32_t>(background_view_.y));
    write_i32(out, static_cast<std::int32_t>(background_view_.width));
    write_i32(out, static_cast<std::int32_t>(background_view_.height));
    for (const float brightness : background_brightness_) {
        write_i32(out, static_cast<std::int32_t>(brightness));
    }
    // AVG_SetSaveData's shake: only one that never ends (sk_speed 0) - any
    // other is over before AVG_ConfigCheck lets a save screen open.
    const auto& bk = back();
    const bool endless_shake = bk.sk_flag && bk.sk_speed == 0;
    write_i32(out, endless_shake ? 1 : 0);
    write_i32(out, endless_shake ? bk.sk_dir : 0);
    write_i32(out, endless_shake ? bk.sk_pich : 0);
    write_i32(out, endless_shake ? bk.sk_type : 0);

    // Characters
    // CharStruct[MAX_CHAR], slot order - which is the order AVG_SetBackChar
    // bakes in, so it has to survive a round trip.  The two flags are
    // CHAR_COND_WAIT (registered but not shown yet) and CHAR_TYPE_WAIT2
    // (released at the next AVG_SetBackReleaseChar), which is what the CW
    // and CRW forms leave behind.
    std::uint32_t live = 0;
    for (int i = 0; i < th2::max_char; ++i) {
        if (chars().state(i).flag) {
            ++live;
        }
    }
    write_u32(out, live);
    for (int i = 0; i < th2::max_char; ++i) {
        const auto& ch = chars().state(i);
        if (!ch.flag) {
            continue;
        }
        write_i32(out, ch.cno);
        write_i32(out, ch.pose);
        write_i32(out, ch.loc1);
        write_i32(out, ch.layer);
        write_i32(out, ch.fade1);
        write_i32(out, ch.alph1);
        write_i32(out, ch.cond == th2::char_cond_wait ? 1 : 0);
        write_i32(out, ch.type == th2::char_type_wait2 ? 1 : 0);
    }

    // Overlays
    // SpriteBmp[i], with the slot it lives in - a loaded overlay is one
    // whose bitmap slot is filled.
    const auto overlay_live = [this](std::size_t i) {
        return display().bmp_flag(th2::bmp_script + static_cast<int>(i));
    };
    std::uint32_t overlay_count = 0;
    for (std::size_t i = 0; i < overlay_states_.size(); ++i) {
        if (overlay_live(i)) {
            ++overlay_count;
        }
    }
    write_u32(out, overlay_count);
    for (std::size_t i = 0; i < overlay_states_.size(); ++i) {
        if (!overlay_live(i)) {
            continue;
        }
        const auto& state = overlay_states_[i];
        write_u32(out, static_cast<std::uint32_t>(i));
        write_str(out, state.name, 64);
        write_str(out, state.archive, 16);
        write_i32(out, state.visible ? 1 : 0);
        write_i32(out, state.layer);
        write_i32(out, state.tone_type);
        write_i32(out, state.parameter);
        write_i32(out, state.parameter_value);
        write_i32(out, state.reverse);
        write_i32(out, state.red);
        write_i32(out, state.green);
        write_i32(out, state.blue);
        write_i32(out, state.destination_x);
        write_i32(out, state.destination_y);
        write_i32(out, state.destination_width);
        write_i32(out, state.destination_height);
        write_i32(out, state.source_x);
        write_i32(out, state.source_y);
        write_i32(out, state.source_width);
        write_i32(out, state.source_height);
        write_i32(out, state.zoom_center_x);
        write_i32(out, state.zoom_center_y);
        write_i32(out, state.zoom);
    }

    // BGM
    write_i32(out, bgm_track_);
    write_i32(out, bgm_loop_ ? 1 : 0);
    write_i32(out, play_music_vol_);

    // The original restores only looping numbered SE channels.
    std::uint32_t se_ch_count = 0;
    for (std::size_t i = 0; i < se_channels_.size(); ++i) {
        if (se_channels_[i].playing() && se_loop_[i]) {
            ++se_ch_count;
        }
    }
    write_u32(out, se_ch_count);
    for (std::size_t i = 0; i < se_channels_.size(); ++i) {
        if (se_channels_[i].playing() && se_loop_[i]) {
            write_u32(out, static_cast<std::uint32_t>(i));
            write_i32(out, se_sound_[i]);
            write_i32(out, se_volume_[i]);
        }
    }

    // The message, as AVG_SetSaveDataNovelMessage keeps it.
    const auto message = msg().save_data();
    write_i32(out, message.flag);
    write_i32(out, message.add_flag);
    write_i32(out, message.step);
    write_i32(out, message.count);
    write_i32(out, message.kstep);
    write_i32(out, message.max);
    write_u32(out, static_cast<std::uint32_t>(message.str.size()));
    out.write(message.str.data(),
              static_cast<std::streamsize>(message.str.size()));

    // Choice state
    write_i32(out, choosing_ ? 1 : 0);
    write_u32(out, static_cast<std::uint32_t>(choices_.size()));
    for (const auto& c : choices_) {
        write_str(out, c.text, 256);
        write_i32(out, c.flag_no);
        write_i32(out, c.flag_value);
        write_str(out, c.sno, 8);
    }
    write_i32(out, choice_highlight_);
    write_i32(out, choice_selected_);
    write_i32(out, choice_result_register_);
    write_i32(out, choice_ex_ ? 1 : 0);

    // Pending after-school destinations survive saves made in a mandatory
    // scene that runs before the map opens.
    write_u32(out, static_cast<std::uint32_t>(map_events_.size()));
    for (const auto& event : map_events_) {
        write_i32(out, event.character);
        write_i32(out, event.position);
        write_i32(out, event.type);
        write_str(out, event.script, 32);
    }

    // AVG_SAVE_DATA's MapStep: the save was made with the map up (from the
    // menu opened over it).
    write_i32(out, engine_config_from_map_ || ui_mode_ == UiMode::map ? 1 : 0);

    // The engine's log: NovelBuf, which is what the log pages through.
    msg().write_log(out);
    write_i32(out, message_ends_block_ ? 1 : 0);

    // Playback and read state
    write_u32(out, static_cast<std::uint32_t>(current_line_key_.size()));
    out.write(current_line_key_.data(),
              static_cast<std::streamsize>(current_line_key_.size()));
    const std::array saved_names{
        &player_name_.family,
        &player_name_.given,
        &player_name_.family_reading,
        &player_name_.given_reading,
        &player_name_.nickname,
        &player_name_.nickname_reading,
    };
    for (const auto* value : saved_names) {
        write_u32(out, static_cast<std::uint32_t>(value->size()));
        out.write(value->data(), static_cast<std::streamsize>(value->size()));
    }
}

bool Game::load_body(std::istream& in)
{
    const auto version = read_u32(in);
    if (version < oldest_supported_save_version_
        || version > save_version_) {
        return false;
    }

    reset_play_state();
    ui_mode_ = UiMode::game;
    message_visible_ = true;
    // AVG_ControlLoad's AVG_Init: InitNovelMessage among the rest (the
    // message machine is rebuilt from the save below), and SAV_LoadScript's
    // ESC_InitEOprFlag - the instruction the save stopped on runs its set-up
    // again rather than resuming a wait it never set up.
    msg().init();
    eopr_flag_.fill(0);
    opcode_phase_ = 1;
    // And the rest of AVG_Init that the scene below is rebuilt on:
    //     ZeroMemory( &BackStruct ); AVG_SetBackFadeDirect( 128, 128, 128 );
    //     AVG_ResetHalfTone();
    // BackStruct otherwise kept the scene the load replaced - its brightness,
    // fade and shake carried straight into the loaded one.
    avgback().init();
    avgback().set_back_fade_direct(th2::bright_neutral, th2::bright_neutral,
                                   th2::bright_neutral);
    reset_half_tone();
    // AVG_CloseBack, from the save/load window the load was asked from, is
    // a one-off DSP_SetGraphDisp in the engine; ours holds it as a flag,
    // which the scene the load builds must not inherit.
    engine_back_closed_ = false;
    // GRP_KEYWAIT goes with the rest of the graphs; AVG_ControlNovelMessage
    // puts it back when the restored line next waits.
    keywait_visible_ = false;
    // Waits and flags of the script that was running, which a load ends.
    script_ended_ = false;
    goto_title_pending_ = false;
    goto_title_released_ = false;
    wake_frames_ = 0;
    wait_frame_ = {};
    skip_held_ = false;

    // Script identity
    const auto script_name = read_str(in, 64);
    runtime_.load(script_name);
    tone_ = read_i32(in);
    tone_back_ = read_i32(in);
    tone_char_ = read_i32(in);
    weather_ = read_i32(in);
    vi_event_voice_no_ = read_i32(in);
    vi_event_voice_no_all_ = read_i32(in);
    demo_mode_ = read_i32(in) != 0;
    demo_delay_frames_ = read_i32(in);
    skipped_month_ = read_i32(in);
    skipped_day_ = read_i32(in);
    if (read_i32(in)) {
        ShakeState state;
        state.type = read_i32(in);
        state.pitch = read_i32(in);
        state.frames = read_i32(in);
        state.direction = read_i32(in);
        state.swing = read_i32(in);
        state.sampled_frame = read_i32(in);
        state.started = std::chrono::steady_clock::now()
            - std::chrono::milliseconds(read_i64(in));
        shake_ = state;
    } else {
        shake_.reset();
    }
    const bool has_sakura = read_i32(in) != 0;
    sakura_random_ = read_u32(in);
    if (has_sakura) {
        SakuraState state;
        state.amount = read_i32(in);
        state.target_amount = read_i32(in);
        state.wind = read_i32(in) / 1000.0f;
        state.speed = read_i32(in);
        state.tick = read_i32(in);
        state.reset_frames = read_i32(in);
        state.no_reset = read_i32(in) != 0;
        if (state.amount < 0
            || state.amount > static_cast<int>(state.petals.size())) {
            throw std::runtime_error("invalid sakura save state");
        }
        for (int i = 0; i < state.amount; ++i) {
            auto& petal = state.petals[i];
            petal.active = read_i32(in) != 0;
            petal.type = read_i32(in);
            petal.x = read_i32(in) / 1000.0f;
            petal.y = read_i32(in) / 1000.0f;
            petal.axis_x = read_i32(in) / 1000.0f;
            petal.axis_y = read_i32(in) / 1000.0f;
            petal.counter = read_u32(in);
        }
        sakura_large_ = load_sakura_texture("sakura.bmp");
        sakura_small_ = load_sakura_texture("sakura2.bmp");
        // AVG_Init zeroes Weather and SAV_Load only calls AVG_SetWeather( wno,
        // twind, tspeed, tamount ) again - with Weather.flag clear, so the
        // amount starts from nothing and the petals fall in afresh.  The
        // saved petals are read past, not put back.
        SakuraState fresh;
        fresh.target_amount = state.target_amount;
        fresh.wind = state.wind;
        fresh.speed = state.speed;
        sakura_ = std::move(fresh);
    } else {
        sakura_.reset();
    }

    // VM state
    const auto pc = read_u32(in);
    std::array<std::int32_t, 50> regs{};
    for (auto& r : regs) {
        r = read_i32(in);
    }
    const auto stack_size = read_u32(in);
    std::vector<std::int32_t> stack_data;
    stack_data.reserve(stack_size);
    for (std::uint32_t i = 0; i < stack_size; ++i) {
        stack_data.push_back(read_i32(in));
    }
    // Flags
    for (std::size_t i = 0; i < 1024; ++i) {
        runtime_.set_flag(i, read_i32(in));
    }
    runtime_.vm_restore(regs, stack_data, pc);

    // Background
    set_bg_scene(read_i32(in));
    background_kind_ =
        static_cast<BackgroundKind>(read_i32(in));
    background_view_.x = static_cast<float>(read_i32(in));
    background_view_.y = static_cast<float>(read_i32(in));
    background_view_.width = static_cast<float>(read_i32(in));
    background_view_.height = static_cast<float>(read_i32(in));
    for (auto& brightness : background_brightness_) {
        brightness = static_cast<float>(read_i32(in));
    }
    struct { int flag = 0, dir = 0, pich = 0, type = 0; } shake;
    if (version >= 29) {
        shake.flag = read_i32(in);
        shake.dir = read_i32(in);
        shake.pich = read_i32(in);
        shake.type = read_i32(in);
    }
    background_scroll_.reset();
    // AVG_SetLoadData: BackStruct.r/g/b from the save.  The port reads its
    // brightness back out of BackStruct every tick (update_background_fade),
    // so it has to be there - left to the BackStruct the load started from,
    // the scene came up at the brightness of whatever was on screen before.
    avgback().set_back_fade_direct(
        static_cast<int>(background_brightness_[0]),
        static_cast<int>(background_brightness_[1]),
        static_cast<int>(background_brightness_[2]));
    restore_background();
    // if( sdata.sk_flag ) AVG_SetShake( sk_type, sk_pich, 0, sk_dir );
    if (shake.flag) {
        avgback().set_shake(shake.type, shake.pich, 0, shake.dir, 256);
    }

    // Characters
    chars().init_char();
    const auto char_count = read_u32(in);
    for (std::uint32_t i = 0; i < char_count; ++i) {
        const auto number = read_i32(in);
        const auto pose = read_i32(in);
        const auto locate = read_i32(in);
        const auto layer = read_i32(in);
        const auto brightness = read_i32(in);
        const auto alpha = read_i32(in);
        const bool staged = read_i32(in) != 0;
        const bool pending_removal = read_i32(in) != 0;
        // Back into the slot it came out of, so the bake order is the same.
        chars().restore(
            static_cast<int>(i), number, pose, locate, layer, brightness,
            alpha, staged, pending_removal);
    }
    background_baked_dirty_ = true;

    // Overlays
    reset_overlays();
    const auto overlay_count = read_u32(in);
    for (std::uint32_t i = 0; i < overlay_count; ++i) {
        const auto slot = static_cast<std::size_t>(read_u32(in));
        const auto name = read_str(in, 64);
        const auto archive = read_str(in, 16);
        OverlayState state;
        state.name = name;
        state.archive = archive;
        state.visible = read_i32(in) != 0;
        state.layer = read_i32(in);
        state.tone_type = read_i32(in);
        state.parameter = read_i32(in);
        state.parameter_value = read_i32(in);
        state.reverse = read_i32(in);
        state.red = read_i32(in);
        state.green = read_i32(in);
        state.blue = read_i32(in);
        state.destination_x = read_i32(in);
        state.destination_y = read_i32(in);
        state.destination_width = read_i32(in);
        state.destination_height = read_i32(in);
        state.source_x = read_i32(in);
        state.source_y = read_i32(in);
        state.source_width = read_i32(in);
        state.source_height = read_i32(in);
        state.zoom_center_x = read_i32(in);
        state.zoom_center_y = read_i32(in);
        state.zoom = read_i32(in);
        if (slot < overlay_states_.size()) {
            restore_overlay(slot, state);
        }
    }

    // BGM
    const auto loaded_bgm_track = read_i32(in);
    bgm_loop_ = read_i32(in) != 0;
    bgm_volume_ = read_i32(in);
    if (loaded_bgm_track >= 0) {
        play_bgm(loaded_bgm_track, bgm_loop_, bgm_volume_);
    }

    // SE channels - restore looping channels, after AVG_ReleaseSeAll:
    //     for(i=0;i<WAVE_SOUND_NUM;i++) AVG_StopSE2( i, 0 );
    stop_all_se(0);
    const auto se_ch_count = read_u32(in);
    for (std::uint32_t i = 0; i < se_ch_count; ++i) {
        const auto channel = static_cast<std::size_t>(read_u32(in));
        const auto sound = read_i32(in);
        const auto volume = read_i32(in);
        if (channel < se_channels_.size() && sound >= 0) {
            play_se(static_cast<int>(channel), sound, true, volume);
        }
    }

    if (version >= 28) {
        // AVG_SetLoadDataNovelMessage: the line back whole, in the step it
        // was saved in.  The port's own Message is not used by the engine's
        // play and stays empty.
        th2::AvgMsg::SaveData message;
        message.flag = read_i32(in);
        message.add_flag = read_i32(in);
        message.step = read_i32(in);
        message.count = read_i32(in);
        message.kstep = read_i32(in);
        message.max = read_i32(in);
        const auto size = read_u32(in);
        message.str.resize(size);
        in.read(message.str.data(), static_cast<std::streamsize>(size));
        msg().load_save_data(message);
        message_ = th2::Message{};
        waiting_for_input_ = false;
    } else if (read_i32(in)) {
        const auto seg_count = read_u32(in);
        std::vector<std::string> segments;
        segments.reserve(seg_count);
        for (std::uint32_t i = 0; i < seg_count; ++i) {
            const auto seg_size = read_u32(in);
            std::string seg(seg_size, '\0');
            if (seg_size > 0) {
                in.read(seg.data(),
                        static_cast<std::streamsize>(seg_size));
            }
            segments.push_back(std::move(seg));
        }
        const auto revealed = read_u32(in);
        const auto visible_size = read_u32(in);
        std::string visible(visible_size, '\0');
        if (visible_size > 0) {
            in.read(visible.data(),
                    static_cast<std::streamsize>(visible_size));
        }
        // A save from before version 28 kept only the port's Message: the
        // engine's machine gets the line it showed, waiting for the reader.
        th2::AvgMsg::SaveData message;
        message.flag = 1;
        message.step = th2::msg_wait;
        message.str = visible;
        msg().load_save_data(message);
        message_ = th2::Message{};
        waiting_for_input_ = false;
    } else {
        message_ = th2::Message{};
        waiting_for_input_ = false;
    }

    // Choice state
    choosing_ = read_i32(in) != 0;
    choices_.clear();
    const auto choices_count = read_u32(in);
    for (std::uint32_t i = 0; i < choices_count; ++i) {
        choices_.push_back(Choice{
            read_str(in, 256),
            read_i32(in),
            read_i32(in),
            read_str(in, 8),
        });
    }
    choice_highlight_ = read_i32(in);
    choice_selected_ = read_i32(in);
    choice_result_register_ = read_i32(in);
    choice_ex_ = read_i32(in) != 0;

    map_events_.clear();
    const auto map_event_count = read_u32(in);
    map_events_.reserve(map_event_count);
    for (std::uint32_t i = 0; i < map_event_count; ++i) {
        map_events_.push_back(MapEvent{
            read_i32(in), read_i32(in), read_i32(in),
            read_str(in, 32),
        });
    }

    loaded_on_map_ = version >= 30 && read_i32(in) != 0;
    // The engine's log is not part of a save: AVG_SetLoadDataNovelMessage
    // zeroes NovelBuf and logs the one line it brings back, so after a load
    // the log starts there (and its up button is dim until the next line).
    // A trace does exactly that.  In play the port keeps its copy of the log
    // and puts it back, so the reader does not lose the backlog to a load;
    // it replaces the one-line log wholesale, the restored line included.
    // A save from before version 28 has no engine message, so its log is
    // the only history there is.
    if (!msg().read_log(in, version < 28 || !trace_mode_)) {
        return false;
    }
    message_ends_block_ = read_i32(in) != 0;
    const auto key_size = read_u32(in);
    current_line_key_.resize(key_size);
    in.read(current_line_key_.data(),
            static_cast<std::streamsize>(key_size));
    if (version < 29) {
        read_i32(in);   // auto mode, no longer restored
        read_i32(in);   // skip mode
    }
    // Off, as reset_play_state left them: the original keeps neither.
    auto_mode_ = false;
    skip_mode_ = false;
    const auto read_name = [&]() {
        const auto size = read_u32(in);
        std::string value(size, '\0');
        in.read(value.data(), static_cast<std::streamsize>(size));
        return value;
    };
    player_name_.family = read_name();
    player_name_.given = read_name();
    player_name_.family_reading = read_name();
    player_name_.given_reading = read_name();
    player_name_.nickname = read_name();
    player_name_.nickname_reading = read_name();
    if (player_name_.family.empty()) {
        player_name_ = default_player_name_;
    }
    // DefaultCharName = ESC_GetFlag( _DEFAULT_NAME ): the save carries it in
    // flag 5 already, so the load restores it from there rather than
    // recomputing it from the names.
    default_char_name_ = runtime_.flag(5);
    return true;
}

void Game::write_u32(std::ostream& out, std::uint32_t value) const
{
    out.put(static_cast<char>(value & 0xFF));
    out.put(static_cast<char>((value >> 8) & 0xFF));
    out.put(static_cast<char>((value >> 16) & 0xFF));
    out.put(static_cast<char>((value >> 24) & 0xFF));
}

void Game::write_i32(std::ostream& out, std::int32_t value) const
{
    write_u32(out, static_cast<std::uint32_t>(value));
}

void Game::write_i64(std::ostream& out, std::int64_t value) const
{
    write_u32(out, static_cast<std::uint32_t>(value & 0xFFFFFFFF));
    write_u32(out, static_cast<std::uint32_t>((value >> 32) & 0xFFFFFFFF));
}

void Game::write_str(std::ostream& out, std::string_view str,
               std::size_t padded_size) const
{
    const auto len = std::min(str.size(), padded_size);
    out.write(str.data(), static_cast<std::streamsize>(len));
    for (std::size_t i = len; i < padded_size; ++i) {
        out.put('\0');
    }
}

std::uint32_t Game::read_u32(std::istream& in) const
{
    std::uint32_t value = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        value |= static_cast<std::uint32_t>(
            static_cast<unsigned char>(in.get())) << shift;
    }
    return value;
}

std::int32_t Game::read_i32(std::istream& in) const
{
    return static_cast<std::int32_t>(read_u32(in));
}

std::int64_t Game::read_i64(std::istream& in) const
{
    const auto low = static_cast<std::int64_t>(read_u32(in));
    const auto high = static_cast<std::int64_t>(read_u32(in));
    return low | (high << 32);
}

std::string Game::read_str(std::istream& in, std::size_t size) const
{
    std::string result(size, '\0');
    in.read(result.data(), static_cast<std::streamsize>(size));
    const auto null_pos = result.find('\0');
    if (null_pos != std::string::npos) {
        result.resize(null_pos);
    }
    return result;
}


}  // namespace th2app
