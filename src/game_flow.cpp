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

void Game::load_script(std::string name)
{
    runtime_.load(std::move(name));
    vi_event_voice_no_ = -1;
    vi_event_voice_no_all_ = -1;
}

bool Game::load_scheduled_script()
{
    constexpr std::size_t month_flag = 0;
    constexpr std::size_t day_flag = 1;
    constexpr std::size_t time_flag = 2;
    constexpr std::size_t event_next_flag = 3;
    constexpr std::size_t event_end_flag = 4;
    constexpr std::size_t calendar_skip_flag = 6;
    constexpr std::size_t clock_time_flag = 7;

    int month = runtime_.flag(month_flag);
    int day = runtime_.flag(day_flag);
    int time = runtime_.flag(time_flag);
    const int event_next = runtime_.flag(event_next_flag);
    bool show_calendar = false;

    if (runtime_.flag(event_end_flag) != 0) {
        time = event_next == -1 ? time + 1 : event_next;
        if (time >= 7) {
            map_events_.clear();
            if (skipped_month_ != 0) {
                month = skipped_month_;
                day = skipped_day_;
                skipped_month_ = 0;
                skipped_day_ = 0;
            } else {
                ++day;
            }
            time = 0;
            runtime_.set_flag(clock_time_flag, 0);
            if (runtime_.flag(calendar_skip_flag) != 0) {
                runtime_.set_flag(calendar_skip_flag, 0);
            } else {
                show_calendar = true;
            }
            if ((month == 3 && day >= 32)
                || (month == 4 && day >= 31)) {
                ++month;
                day = 1;
            }
        }
    }

    const int weekday = month == 3 ? day % 7
        : month == 4 ? (day + 3) % 7
        : (day + 5) % 7;
    const bool holiday =
        (month == 3 && (day == 20 || day >= 25))
        || (month == 4 && (day <= 7 || day == 29))
        || (month == 5 && (day == 3 || day == 4 || day == 5));
    if ((weekday == 0 || holiday) && time == 5) {
        time = 6;
    }

    if (time == 5) {
        runtime_.set_flag(month_flag, month);
        runtime_.set_flag(day_flag, day);
        runtime_.set_flag(time_flag, time);
        begin_map();
        return true;
    }
    map_events_.clear();

    static constexpr std::array periods{
        "MORNING", "INTERVAL", "LUNCH_BREAK", "SCHOOL_HOURS",
        "AFTER_SCHOOL", "", "NIGHT",
    };
    if (time < 0 || time >= static_cast<int>(periods.size())
        || periods[time][0] == '\0') {
        return false;
    }

    runtime_.set_flag(month_flag, month);
    runtime_.set_flag(day_flag, day);
    runtime_.set_flag(time_flag, time);
    runtime_.set_flag(event_end_flag, 0);
    runtime_.set_flag(event_next_flag, -1);
    load_script(std::format(
        "EV_{:02d}{:02d}{}.SDT", month, day, periods[time]));
    if (show_calendar) {
        begin_calendar(-1, -1);
    }
    return true;
}

bool Game::voice_playing() const
{
    return std::any_of(
        voice_channels_.begin(), voice_channels_.end(),
        [](const th2::AudioChannel& channel) {
            return channel.playing();
        });
}

void Game::update_playback_modes()
{
    if (config_open_ || ui_mode_ != UiMode::game || choosing_) {
        auto_next_time_.reset();
        return;
    }
    // Skipping and auto mode are both AVG_ControlNovelMessage's, and both
    // are one line of it:
    //     }else if( ( AVG_GetHitKey() ... ) || ( AVG_GetMesCut() )
    //               || ... || AVG_WaitAutoMode() ){
    // AVG_GetMesCut() reads Avg.msg_cut_mode, which AVG_ControlSystem2 has
    // already set from the key, and AVG_WaitAutoMode counts its own frames
    // against Avg.auto_key or Avg.auto_page.  There is nothing left to do
    // here, and no timer of ours to keep in step with either of them.
}

float Game::choice_y_start() const
{
    if (!message_.empty()) {
        return message_text_y()
            + static_cast<float>(display_lines(message_.visible()).size())
                * text_line_height()
            + 1.0f;
    }
    return 468.0f;
}

float Game::choice_height(const Choice& choice) const
{
    return std::max<std::size_t>(
        1, display_lines(choice.text).size()) * text_line_height();
}

std::vector<std::string> Game::choice_lines(
    const Choice& choice, int index) const
{
    auto lines = display_lines(choice.text);
    if (!lines.empty()) {
        lines.front() = std::format("{}. {}", index + 1, lines.front());
    }
    return lines;
}

bool Game::message_cut() const
{
    // AVG_GetMesCut():
    //
    //     if(Avg.msg_cut_optin&1){
    //         return Avg.msg_cut || Avg.msg_cut_mode;
    //     }else{
    //         return (Avg.msg_cut || Avg.msg_cut_mode) && AVG_CheckScenarioFlag();
    //     }
    //
    // Avg.msg_cut is the key held, Avg.msg_cut_mode the toggle, and
    // msg_cut_optin&1 is the "skip unread text" setting: with it off, the cut
    // only applies to a line that has been read before.  Reading it here
    // rather than in the skip handler is the whole of skipping - every
    // AVG_EffCnt goes to zero, every AVG_Wait clears, and the machine runs an
    // instruction a frame with nothing forcing anything.
    if (!skip_mode_ && !skip_held_) {
        return false;
    }
    if (config_.skip_unread) {
        return true;
    }
    return current_text_is_read();
}

int Game::effect_frames4(int frames) const
{
    // AVG_EffCnt4( cnt ): 30fps units with no Avg.wait, and zero while the
    // message is being cut.  The scroll, the waits and the audio fades all
    // measure themselves with this rather than AVG_EffCnt.
    if (message_cut()) {
        return 0;
    }
    const int count = frames == -1 ? 15
        : frames == -2 ? 30
        : std::max(0, frames);
    return count * 2;  // Avg.frame / 30, and Avg.frame is 60
}

int Game::message_wait_setting() const
{
    // Avg.msg_wait is one of four settings; ours is milliseconds per
    // character.  0 is instant in both.
    if (config_.text_speed_ms <= 0) return 0;
    if (config_.text_speed_ms <= 12) return 1;
    if (config_.text_speed_ms <= 28) return 2;
    return 3;
}

int Game::message_count_step() const
{
    // int AVG_MsgCnt( void ), verbatim:
    //
    //     if( AVG_GetMesCut() ) ret = 9999;
    //     else switch(Avg.msg_wait){
    //         case 0: ret = 9999;            break;
    //         case 1: ret = 4*30/Avg.frame;  break;
    //         case 2: ret = 2*30/Avg.frame;  break;
    //         case 3: ret = 1*30/Avg.frame;  break;
    //     }
    //     if(ret==0) ret=1;
    //
    // Avg.frame is 60, so the divisions halve each rate.  Zero is "no
    // typewriter at all", which is the fastest text setting rather than the
    // slowest - and 9999 while skipping is what puts a whole line up at once.
    if (message_cut()) {
        return 9999;
    }
    int ret = 0;
    switch (message_wait_setting()) {
    default:
    case 0: ret = 9999; break;
    case 1: ret = 4 * 30 / 60; break;
    case 2: ret = 2 * 30 / 60; break;
    case 3: ret = 1 * 30 / 60; break;
    }
    if (ret == 0) {
        ret = 1;
    }
    return ret;
}

int Game::effect_count_pulse() const
{
    // int AVG_EffCntPuls( void ), verbatim - the same shape as AVG_MsgCnt
    // but keyed off Avg.wait, the effect speed, rather than Avg.msg_wait.
    if (message_cut()) {
        return 9999;
    }
    int ret = 0;
    switch (std::clamp(config_.effect_speed, 0, 4)) {
    default:
    case 0: ret = 9999; break;
    case 1: ret = 4 * 30 / 60; break;
    case 2: ret = 2 * 30 / 60; break;
    case 4: ret = 1 * 30 / 60; break;
    }
    if (ret == 0) {
        ret = 1;
    }
    return ret;
}

int Game::effect_frames3(int frames) const
{
    // AVG_EffCnt3( cnt ): 30fps units like AVG_EffCnt4, and the only one of
    // the family that does not consult AVG_GetMesCut() - so an effect timed
    // with it runs at full length even while the skip key is down.
    const int count = frames == -1 ? 15
        : frames == -2 ? 30
        : std::max(0, frames);
    return count * 2;  // Avg.frame / 30, and Avg.frame is 60
}

int Game::effect_frames(int frames) const
{
    // AVG_EffCnt() reads a script's frame count the same way everywhere: -1
    // means fifteen frames, -2 thirty, and anything else is the count
    // itself, so a zero really is instant.  The effect-speed setting
    // (Avg.wait in the original) scales all of them.
    // AVG_EffCnt's first question is whether the message is being cut:
    //     if( cut ) ret = 0;
    //     else      ret = Avg.wait*cnt*Avg.frame/60;
    // Skipping makes every effect instant, not merely fast.  Without that a
    // wipe kept running while the script raced past it, and the next line
    // of text went up over the top of it.
    if (message_cut()) {
        return 0;
    }
    const int count = frames == -1 ? 15
        : frames == -2 ? 30
        : std::max(0, frames);
    return count * std::clamp(config_.effect_speed, 0, 4);
}

// Every frame count a script passes is authored in 30fps units: AVG_EffCnt2,
// AVG_EffCnt3 and AVG_EffCnt4 all return cnt * Avg.frame / 30, and AVG_EffCnt
// lands on the same duration at the default effect speed.  Audio fades go
// through AVG_EffCnt3 and AVG_EffCnt4 (GM_Avg.cpp:2335, 2371, 2410), which
// the effect-speed setting does not scale.
std::chrono::milliseconds Game::audio_fade_duration(int frames)
{
    return std::chrono::milliseconds(std::max(0, frames) * 1000 / 30);
}

// Text runs from message_text_x() to the same distance short of the sidebar,
// so the gap on the right matches the one on the left.
float Game::message_text_width() const
{
    return sidebar_left_x - 2.0f * message_text_x();
}

float Game::text_line_height() const
{
    if (font_.authentic()) {
        return 31.0f;
    }
    // font_size + 7 is fine until the face's own line spacing outgrows it,
    // which it does at large sizes and would overlap the next line.
    return std::max(
        static_cast<float>(std::max(31, config_.font_size + 7)),
        font_.line_height());
}

std::vector<std::string> Game::display_lines(std::string_view source) const
{
    // The script's own line breaks are left exactly as written; this only
    // decides where a line too long for the area has to be broken.
    return th2app::display_lines(
        source, message_text_width(),
        [this](std::string_view text) { return font_.text_width(text); });
}

float Game::message_text_x() const
{
    return 26.0f;
}

float Game::message_text_y() const
{
    return 36.0f;
}

std::size_t Game::utf8_prefix_bytes(
    std::string_view text, std::size_t characters)
{
    std::size_t position = 0;
    while (position < text.size() && characters > 0) {
        const auto byte = static_cast<unsigned char>(text[position]);
        position += byte < 0x80 ? 1
            : byte < 0xe0 ? 2
            : byte < 0xf0 ? 3
            : 4;
        --characters;
    }
    return std::min(position, text.size());
}

std::size_t Game::utf8_character_count(std::string_view text)
{
    return std::count_if(
        text.begin(), text.end(), [](unsigned char byte) {
            return (byte & 0xc0) != 0x80;
        });
}

// AVG_ControlSelectWindow types the options out at the message speed
// (SelectWindow.cnt += AVG_MsgCnt()) and clips each one with
// DSP_SetTextCount(cnt - j*4), so each option starts four characters after
// the one above it.  Returns how many characters of this option to show.
int Game::choice_reveal_count(int index) const
{
    if (config_.text_speed_ms <= 0 || !choice_reveal_started_) {
        return std::numeric_limits<int>::max();
    }
    const auto elapsed = std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - *choice_reveal_started_).count();
    return static_cast<int>(elapsed / static_cast<float>(config_.text_speed_ms))
        - index * 4;
}

void Game::start_text_reveal(std::size_t start)
{
    text_reveal_start_ = start;
    text_reveal_started_ = std::chrono::steady_clock::now();
    text_reveal_complete_ = config_.text_speed_ms == 0;
    text_fade_complete_ = text_reveal_complete_;
}

bool Game::finish_text_reveal()
{
    if (text_reveal_complete_) {
        return false;
    }
    // Skipping ends both: the reader asked for the whole line now.
    text_reveal_complete_ = true;
    text_fade_complete_ = true;
    return true;
}

void Game::get_game_key()
{
    // void AVG_GetGameKey(void).  The device state is SDL's rather than
    // KeyCond's, but the fold is the original's: click and cansel are edges,
    // mes_cut is a level, and a click always beats a held skip key.
    const bool blocked = config_open_ || name_input_open_;
    key_cond_.btn_ctrl = !blocked
        && ((SDL_GetModState() & SDL_KMOD_CTRL) != 0
            || touch_input_.skip_held()
            || gamepad_input_.ctrl_skip_held());
    key_cond_.btn_alt = (SDL_GetModState() & SDL_KMOD_ALT) != 0;
    th2::get_game_key(game_key_, key_cond_, 0);
    // The edges have now been consumed by exactly one control pass, so they
    // are cleared - KEY_RenewKeybord does this at the top of
    // MAIN_SystemControl, which in the original is every frame.
    key_cond_.clear_triggers();
}

void Game::control_system2()
{
    // void AVG_ControlSystem2( void ), the part that matters:
    //
    //     Avg.msg_cut = GameKey.mes_cut;
    //     if( GameKey.mes_cut_mode ){ Avg.msg_cut_mode = !Avg.msg_cut_mode; }
    //     if( GameKey.cansel || GameKey.click || GameKey.diswin || ... ){
    //         Avg.msg_cut = OFF; Avg.msg_cut_mode = OFF; }
    //     if(GameKey.mes_cut){ Avg.msg_cut_mode = OFF; Avg.auto_flag=OFF; }
    //     if( GameKey.cansel || GameKey.diswin || GameKey.pup ){
    //         Avg.auto_flag=OFF; }
    skip_held_ = game_key_.mes_cut != 0;
    if (game_key_.mes_cut_mode) {
        skip_mode_ = !skip_mode_;
    }
    if (game_key_.cansel || game_key_.click || game_key_.diswin
        || game_key_.home || game_key_.end || game_key_.pdown
        || game_key_.pup || demo_mode_) {
        skip_held_ = false;
        skip_mode_ = false;
    }
    if (game_key_.mes_cut) {
        skip_mode_ = false;
        auto_mode_ = false;
    }
    if (game_key_.cansel || game_key_.diswin || game_key_.pup) {
        auto_mode_ = false;
    }
}

void Game::play_system_se(int number, int volume)
{
    // AVG_PlaySE3( sno, volume ): the two sounds AVG_ControlNovelMessage
    // makes - 9104 for a button and 9012 for the log.
    play_se(0, number, false, volume);
}

void Game::skip(bool force_unread)
{
    // The engine has no skip function.  Holding the key sets Avg.msg_cut and
    // that is all it does: AVG_GetMesCut() then makes every AVG_EffCnt return
    // zero, so each effect finishes itself on its next control pass, and the
    // message state machine leaves MSG_WAIT on the same flag.  Nothing is
    // rewound and nothing is forced - which is why the original cannot get
    // stuck part-way through an animation the way ours did.
    //
    // What is left here is the half a key press really does do: turn the
    // page.  It is the same thing a click does, and it goes through the same
    // guard, so it cannot fire in the middle of an effect.
    static_cast<void>(force_unread);
    if (clock_state_ || calendar_state_ || choosing_ || movie_) {
        return;
    }
    if (waiting_for_input_) {
        mark_current_text_read();
        if (finish_text_reveal()) {
            auto_next_time_.reset();
            return;
        }
        const auto reveal_start = message_.visible().size();
        if (message_.reveal_next() && message_.has_hidden_segments()) {
            start_text_reveal(reveal_start);
            auto_next_time_.reset();
            return;
        }
        waiting_for_input_ = false;
    }
}


}  // namespace th2app
