#include "game.hpp"
#include "text_count.hpp"
#include "engine_rand.hpp"

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

namespace {
// 'EOPR', ahead of the EOprFlag bytes in a trace checkpoint.
constexpr int eopr_checkpoint_marker = 0x52504f45;
}  // namespace

void Game::load_script(std::string name)
{
    script_ended_ = false;
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
    // Temporary: TH2_SCHEDULE_FLAGS=30,206,750 logs those script flags as
    // each schedule script starts - what its route gates are about to read.
    if (const char* list = SDL_getenv("TH2_SCHEDULE_FLAGS")) {
        std::string line = std::format("schedule {} EV_{:02d}{:02d}{}",
                                       trace_tick_, month, day, periods[time]);
        for (const char* p = list; *p;) {
            char* end = nullptr;
            const long index = std::strtol(p, &end, 10);
            if (end == p) break;
            line += std::format(" {}={}", index,
                                runtime_.flag(static_cast<std::size_t>(index)));
            p = *end == ',' ? end + 1 : end;
        }
        SDL_Log("%s", line.c_str());
    }
    if (show_calendar) {
        begin_calendar(-1, -1);
        calendar_state_->step = true;
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
    if (msg().vanilla_layout()) {
        // DSP_GetTextDispPos( TXT_WINDOW, &px, &py ); ... py + SYS_FONT*2:
        // from the message's own cursor, which is the top of the row its
        // last character is on - not from the bottom of the lines we count.
        const auto& layout = msg().layout();
        int cx = th2::message_text_box.sx;
        int cy = th2::message_text_box.sy;
        th2::txt_cursor_after(layout, th2::message_text_box,
                              layout.glyph_x.size(), &cx, &cy);
        return static_cast<float>(cy + th2::message_text_box.font * 2);
    }
    if (!message_.empty()) {
        return message_text_y()
            + static_cast<float>(display_lines(message_.visible()).size())
                * text_line_height()
            + 1.0f;
    }
    return 468.0f;
}

// TXT_SELECT+i as the engine lays it out:
//     DSP_SetText( TXT_SELECT+j, LAY_WINDOW+1, SYS_FONT, ON, mes[j] );
//     DSP_SetTextPos( TXT_SELECT+j, 32, py + SYS_FONT*2 + 13*j+h, 20, 4 );
//     h += DSP_GetTextDispH( TXT_SELECT+j );
// with mes[j] = "１．" + the option (AVG_SetSelectMessage's "%c%c．%s").
// The walk is TXT_DrawTextEx's own, so the wrap at 20 cells, the 24px line
// pitch (a text object has no pich of its own) and each character's count
// all come from the same place the engine takes them.
std::string Game::choice_engine_text(int index) const
{
    // "１" + index, "．": U+FF11.. and U+FF0E, three UTF-8 bytes each.
    const char32_t digit = U'\uFF11' + static_cast<char32_t>(index);
    std::string text;
    for (const char32_t c : {digit, U'\uFF0E'}) {
        text += static_cast<char>(0xE0 | (c >> 12));
        text += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        text += static_cast<char>(0x80 | (c & 0x3F));
    }
    return text + choices_.at(static_cast<std::size_t>(index)).text;
}

th2::TextCount Game::choice_layout(int index) const
{
    th2::TextBox box{32, static_cast<int>(choice_row_y(index)), 20, 4, 0, 0,
                     th2::message_text_box.font};
    return th2::txt_count_text(choice_engine_text(index), box);
}

// DSP_GetTextDispH: TXT_DrawText's return, (px==sx) ? py-sy : py-sy+fno.
float Game::choice_row_height(int index) const
{
    if (!font_.authentic()) {
        return choice_height(choices_.at(static_cast<std::size_t>(index)));
    }
    th2::TextBox box{32, 0, 20, 4, 0, 0, th2::message_text_box.font};
    const auto counted = th2::txt_count_text(choice_engine_text(index), box);
    if (counted.cursor_x.empty()) {
        return 0.0f;
    }
    const int px = counted.cursor_x.back();
    const int py = counted.cursor_y.back();
    return static_cast<float>(px == box.sx ? py : py + box.font);
}

float Game::choice_row_y(int index) const
{
    float y = choice_y_start();
    for (int k = 0; k < index; ++k) {
        y += choice_row_height(k) + (font_.authentic() ? 13.0f : 0.0f);
    }
    return y;
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
        if (font_.authentic() && index >= 0 && index < 9) {
            // AVG_SetSelectMessage: "%c%c．%s" with '１'+mnum - the full
            // width digit and stop, two characters of SYS_FONT, where "1. "
            // was three half-width ones: every glyph after it sat a column
            // off and typed in a count late.
            const char32_t digit = U'\uFF11' + static_cast<char32_t>(index);
            std::string prefix;
            const auto put = [&prefix](char32_t c) {
                prefix += static_cast<char>(0xE0 | (c >> 12));
                prefix += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                prefix += static_cast<char>(0x80 | (c & 0x3F));
            };
            put(digit);
            put(U'\uFF0E');
            lines.front() = prefix + lines.front();
        } else {
            lines.front() = std::format("{}. {}", index + 1, lines.front());
        }
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
    const float width = message_text_width();
    const auto generation = font_.generation();
    for (std::size_t i = 0; i < wrapped_text_.size(); ++i) {
        auto& held = wrapped_text_[i];
        if (held.generation == generation && held.width == width
            && held.source == source) {
            // Most recent first, so the message being typed out stays put.
            std::rotate(wrapped_text_.begin(), wrapped_text_.begin() + i,
                        wrapped_text_.begin() + i + 1);
            return wrapped_text_.front().lines;
        }
    }
    auto lines = th2app::display_lines(
        source, width,
        [this](std::string_view text) { return font_.text_width(text); });
    constexpr std::size_t remembered = 8;
    if (wrapped_text_.size() >= remembered) {
        wrapped_text_.pop_back();
    }
    wrapped_text_.insert(wrapped_text_.begin(),
                         WrappedText{std::string(source), width, generation,
                                     lines});
    return lines;
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
    // SelectWindow.cnt, less this option's stagger.  The count is stepped
    // in control_select_window by AVG_MsgCnt(), the same per-frame integer
    // the message machine uses - so a choice types out at exactly the rate
    // a line of dialogue does.  Deriving it from elapsed milliseconds
    // instead ran it slow: measured at the SetSelect at pc 5857 of
    // 070000400.sdt, where the reference answers after 34 ticks and ours
    // took 44.
    return choice_reveal_cnt_ - index * 4;
}

// SelectWindow.cond flipping from 1 to 0: the options have finished typing.
//
//     max_cnt = max( max_cnt, TXT_GetTextCount(SelectWindow.mes[j],-1)+8+j*4 );
//     ...
//     if(SelectWindow.cnt>=max_cnt){ SelectWindow.cond=0; ... }
//
// choice_reveal_count(i) is that cnt less the option's own i*4 stagger, so
// the test below is the original's with the stagger put back.  The +8 is the
// engine's and not a margin of ours: an option is not answerable for eight
// counts after its last character lands.
bool Game::choice_reveal_finished() const
{
    if (choices_.empty()) {
        return false;
    }
    int max_count = 0;
    for (int i = 0; i < static_cast<int>(choices_.size()); ++i) {
        int count = 0;
        if (font_.authentic()) {
            // TXT_GetTextCount( SelectWindow.mes[j], -1 )
            count = th2::txt_count_text(choice_engine_text(i)).total;
        } else {
            for (const auto& line : choice_lines(choices_[i], i)) {
                count += static_cast<int>(utf8_character_count(line));
            }
        }
        max_count = std::max(max_count, count + 8 + i * 4);
    }
    return choice_reveal_count(0) >= max_count;
}

// AVG_ControlSelectWindow, the number row half of it.
//
//     for(j=0;j<SelectWindow.mnum+1;j++){
//         if(GameKey.num[j]){ select = j-1; click = 1; }
//     }
//     if( click && select>=0 ){ SelectWindow.select = select; ... }
//
// The other half answers a choice from the cursor's mouse rect, and that
// already reaches choices_ through the SDL event path.  This is the half
// nothing here had: num[j] names option j-1 outright, so num[1] is the first
// option and num[0] selects -1 and is then discarded by the `select>=0`
// guard.  It is read in cond 0 only - a choice cannot be answered while it is
// still typing itself out.
// The body of AVG_ControlSelectWindow's `if( click && select>=0 )`:
//
//     SelectWindow.select = select;
//     ...
//     AVG_SetNovelMessageDisp(OFF);
//
// The message goes off the moment the choice is answered and stays off until
// whatever the branch lands on puts a new one up - two frames later at the
// SetSelect at pc 5857 of 070000400.sdt, where the reference blanks the
// window for exactly those two ticks and ours never blanked it at all.
void Game::answer_choice(int select)
{
    choice_highlight_ = select;
    choice_selected_ = select;
    trace_choice_answered_ = true;
    msg().set_novel_message_disp(false);
    // The click that answered has ended auto and skip mode as any click does
    // (AVG_ControlSystem2); advance() resolves the branch.
    auto_mode_ = false;
    skip_mode_ = false;
    advance();
}

void Game::control_select_window()
{
    if (!choosing_ || choices_.empty() || choice_selected_ >= 0) {
        return;
    }
    // case 1 types (SelectWindow.cnt += AVG_MsgCnt(), once per control
    // pass) and, on the frame the count reaches max_cnt, only sets cond = 0;
    // case 0 is where a key is read, and that is the next frame's switch.
    // Typing and answering in one pass took the number key a frame early -
    // hidden for as long as the options were one character too long.
    const int options = static_cast<int>(choices_.size());
    if (!choice_reveal_finished()) {
        th2::set_draw_flag_on();   // case 1: MainWindow.draw_flag=1;
        for (int step = 0; step < control_steps_; ++step) {
            choice_reveal_cnt_ += message_count_step();
        }
        if (choice_reveal_finished()) {
            // ...and on that same frame each option becomes a mouse rect on
            // layer 0, slot 16+i.
            register_choice_rects(false);
        }
        return;
    }
    // A finger is not a mouse: the engine's rects are one line of text
    // tall, a few millimetres on a phone.  Under touch they widen to the
    // whole row and meet halfway across the gaps, and go back to the
    // engine's own the moment the mouse is used.  Never in a trace.
    if (!trace_mode_ && pointer_from_touch_ != choice_rects_for_touch_) {
        register_choice_rects(pointer_from_touch_);
    }
    // case 0.  The option under the pointer is the selection - lit in
    // FCT_NORMAL, the rest FCT_GLAY - and a click answers it; a number key
    // answers outright:
    //     select = MUS_GetMouseNoEx( -1, 0 )-16;  click = GameKey.click;
    //     for(j=0;j<mnum+1;j++) if(GameKey.num[j]){ select = j-1; click = 1; }
    int select = msg().mouse_no_ex(th2::mouse_any, 0) - 16;
    if (select >= options) {
        select = -1;
    }
    bool click = game_key_.click != 0;
    //     if(GameKey.u) MUS_SetMousePosRect( hwnd, 0, (select<0)? 16 : select+16-1 );
    //     if(GameKey.d) MUS_SetMousePosRect( hwnd, 0, (select<0)? 16 : select+16+1 );
    // The pointer moves; the highlight follows it on the next tick.
    if (game_key_.u) {
        msg().set_mouse_pos_rect(0, select < 0 ? 16 : select + 16 - 1);
    }
    if (game_key_.d) {
        msg().set_mouse_pos_rect(0, select < 0 ? 16 : select + 16 + 1);
    }
    for (int j = 0; j <= options && j < 10; ++j) {
        if (game_key_.num[j]) {
            select = j - 1;
            click = true;
        }
    }
    choice_highlight_ = select;
    if (click && select >= 0 && select < options) {
        // AVG_ResetSelectWindow's MUS_ResetMouseRect( 0, 16+i ), and
        //     Avg.msg_cut = OFF;
        for (int i = 0; i < 10; ++i) {
            msg().set_mouse_rect(0, 16 + i, 0, 0, 0, 0, false);
        }
        skip_held_ = false;
        answer_choice(select);
    }
}

void Game::register_choice_rects(bool touch)
{
    //     len = min( 26, TXT_GetTextCount(SelectWindow.mes[i],0) );
    //     MUS_SetMouseRect( 0, 16+i, 32, py + SYS_FONT*2 + 13*i+h,
    //                       len*SYS_FONT, DSP_GetTextDispH(..), ON );
    // For touch, each band runs from the screen's edge to the bar and down
    // to halfway to the next option, at least `touch_min` tall at the ends.
    constexpr int touch_min = 56;
    const int options = static_cast<int>(choices_.size());
    choice_rects_for_touch_ = touch;
    for (int i = 0; i < options; ++i) {
        const int top = static_cast<int>(choice_row_y(i));
        const int height = static_cast<int>(choice_row_height(i));
        if (!touch) {
            const auto counted = th2::txt_count_text(choice_engine_text(i));
            const int len = std::min(26, th2::txt_get_text_count(counted, 0));
            msg().set_mouse_rect(0, 16 + i, 32, top,
                                 len * th2::message_text_box.font, height,
                                 true);
            continue;
        }
        const int pad = std::max(0, (touch_min - height) / 2);
        const int band_top = i == 0
            ? top - pad
            : (static_cast<int>(choice_row_y(i - 1) + choice_row_height(i - 1))
               + top) / 2;
        const int band_bottom = i + 1 == options
            ? top + height + pad
            : (top + height + static_cast<int>(choice_row_y(i + 1))) / 2;
        msg().set_mouse_rect(0, 16 + i, 0, band_top,
                             static_cast<int>(sidebar_left_x),
                             band_bottom - band_top, true);
    }
}

void Game::start_text_reveal(std::size_t start)
{
    text_reveal_start_ = start;
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

void Game::set_trace_game_flags(const std::string& spec)
{
    std::istringstream list(spec);
    std::string item;
    while (std::getline(list, item, ',')) {
        const auto equals = item.find('=');
        if (item.empty() || equals == std::string::npos) {
            throw std::runtime_error("--trace-game-flags: expected N=V, got '"
                                     + item + "'");
        }
        const int index = std::stoi(item.substr(0, equals));
        const int value = std::stoi(item.substr(equals + 1));
        if (index < 0
            || static_cast<std::size_t>(index) >= persistent_game_flags_.size()) {
            throw std::runtime_error("--trace-game-flags: no game flag "
                                     + std::to_string(index));
        }
        persistent_game_flags_[static_cast<std::size_t>(index)] = value;
        runtime_.set_game_flag(static_cast<std::size_t>(index), value);
        SDL_Log("trace: game flag %d = %d", index, value);
    }
}

void Game::enable_trace(
    const std::filesystem::path& dir,
    const std::optional<std::filesystem::path>& input,
    std::uint64_t ticks, std::uint64_t first, std::uint64_t lead)
{
    trace_mode_ = true;
    // The audio channels' own clock, so a fade advances with the tick
    // counter rather than with however fast this machine replays.  See
    // AudioChannel::set_clock: AVG_WaitBGM asks whether a fade has finished,
    // so on the wall clock the script's program counter picked up the host's
    // speed and the same tick disagreed with itself between runs.
    th2::AudioChannel::set_clock([this] { return engine_now(); });
    // See the note on the declaration: GlobalCount is not reset when the
    // scenario starts, so it carries the reference's lead-in with it.
    global_count_ = static_cast<int>(lead);
    trace_dir_ = dir;
    trace_last_tick_ = ticks;
    trace_first_tick_ = first;
    // A trace tick is a unit of engine progress, not of time, so there is
    // nothing for the run to be in step with.  Left on, the swapchain's
    // vsync paces the whole replay at the monitor's refresh - which made a
    // 695 tick run take 12 seconds of wall clock for about a second of work,
    // and would make replaying to a divergence deep in a scenario the
    // dominant cost of every fix-and-retest cycle.
    if (renderer_ && !SDL_SetRenderVSync(renderer_, SDL_RENDERER_VSYNC_DISABLED)) {
        SDL_Log("trace: could not disable vsync: %s", SDL_GetError());
    }
    vsync_paced_ = false;
    // Every frame of a trace is recorded as the engine's, so a GL call that
    // failed must stop the run rather than leave a wrong frame in the file.
    th2::GlExactBlend::set_strict(true);
    // And nothing the host's speed decides may reach the frame: background
    // decoding is done whole in the tick it is queued in.
    background_budget_.set_unlimited(true);
    std::filesystem::create_directories(trace_dir_);
    if (input) {
        trace_script_.load(*input);
        SDL_Log("trace: %zu input events from %s",
                trace_script_.size(), input->string().c_str());
    }
    // A trace run gets a pinned config rather than the player's, and the
    // pinned values are the ones reference/run/CONFIG.ini gives the other
    // side.  Otherwise the comparison measures two option screens: whatever
    // this profile last saved in the menu would set Avg.wait, which
    // AVG_EffCnt multiplies every effect length by, and every fade would
    // diverge for a reason that has nothing to do with the engine.
    config_.effect_speed = 2;       // Avg.wait
    config_.text_speed_ms = 20;     // Avg.msg_wait == 2
    config_.message_half_tone = 64; // Avg.half_tone
    // The original's bitmap font out of FNT.PAK, at the size the English
    // release draws it - font24, not the Japanese build's font34.  The high-resolution outline font is the better one to read
    // and the reason our text layer exists at all - but it cannot be
    // compared to a 16x16 bitmap glyph, and a comparison that leaves the
    // text out cannot see the typewriter at all.
    config_.authentic_font = true;
    config_.font_size = 24;   // SYS_FONT in the English release
    // Matching run/CONFIG.ini, which is the point: AVG_WaitSe opens with
    // `if(!Avg.se) return 0`, so sound on one side and off the other means
    // one engine waits at an SEW and the other walks straight past it.
    // Effects and voices are on, so those waits are exercised; BGM is off
    // on both sides because MW waits on a *fade*, which the reference's
    // stubbed decoder drives from a worker thread rather than from ticks.
    // The player's name, pinned to GM_AvgMsg.h's compiled-in defaults.
    //
    // The reference is built from the GPL source, so it can only ever have
    // those; ours reads the shipped executable, which the English patch
    // romanised.  That is a difference between two binaries rather than
    // between two engines, but it is not cosmetic: *nnk stands for two
    // full-width characters in one and four half-width ones in the other,
    // which is two counts of difference in the typewriter and a different
    // line break wherever the name appears.
    player_name_ = th2::PlayerName{
        "\u6cb3\u91ce",                    // DEF_NAME_L    河野
        "\u8cb4\u660e",                    // DEF_NAME_F    貴明
        "\u3053\u3046\u306e",             // DEF_NAME_LK   こうの
        "\u305f\u304b\u3042\u304d",      // DEF_NAME_FK   たかあき
        "\u305f\u304b",                    // DEF_NAME_NN   たか
        "\u30bf\u30ab",                    // DEF_NAME_NNK  タカ
    };
    // The same difference one token further out: *h2 is a character's name,
    // not the player's, and AVG_SetName substitutes the original two-
    // character one.  Ours ships the romanised English name, which is six
    // counts rather than two - measured at pc 129 of 110010000.sdt, where
    // the message came to 76 counts here and 72 there.
    th2::set_h2_character_name("\u5c0f\u7267", "\u611b\u4f73");
    config_.se_volume = 256;
    config_.voice_volume = 256;
    config_.bgm_volume = 0;
    // Avg.msg_cut_optin 1, "skip unread text", which run/CONFIG.ini gives
    // the reference too.  A trace starts from a fresh profile where nothing
    // has been read, so with it off holding Ctrl did nothing at all - and a
    // hand-recorded run wants to hurry through the scenes it is not about.
    // The setting is not only a key gate: it also picks the column the
    // sidebar's skip button is drawn from (lit rather than dead on an unread
    // line), so the two sides have to agree on it, not merely both allow it.
    config_.skip_unread = true;
    // Avg.side_option 0, which is what the reference runs: the bar fades by
    // 24 a frame toward 64 while the pointer is left of DISP_X-24 and back up
    // to 256 when it is not (GM_AvgMsg.cpp's GRP_HISTORY block).  Ours
    // defaults to hidden, so without this the bar is simply absent from the
    // frame; forcing it fully opaque instead is just as wrong, because the
    // traced pointer sits at x=400 and the engine settles it at 64.
    config_.sidebar_mode = 0;

    const auto state = trace_dir_ / "state.txt";
    trace_state_ = std::fopen(state.string().c_str(), "w");
    if (trace_state_) {
        // Same columns, same order, same names as the reference's
        // th2ref_state.cpp, so the two files diff directly.
        std::fprintf(trace_state_,
                     "tick script pc "
                     "msg_flag msg_disp msg_step1 msg_step2 msg_count "
                     "msg_kstep msg_max "
                     "tone_tstep tone_tcount "
                     "bk_bno bk_fd_flag bk_fd_type bk_fd_cnt bk_fd_max "
                     "bk_sc_flag bk_sc_cnt bk_sc_max "
                     "bk_sk_flag bk_sk_cnt "
                     "bk_br_flag bk_br_cnt "
                     "avg_msg_cut avg_auto avg_frame avg_wait avg_level "
                     "avg_msg_wait avg_msg_page avg_half_tone "
                     "txt_cnt txt_step global_count text\n");
    }
}

std::string Game::trace_glyph_alpha() const
{
    // One character per revealed glyph: alph2/16, which is 0..16, in base 36
    // - so a fully faded-in glyph is 'g' and an invisible one '0'.  The same
    // encoding the reference writes from inside TXT_DrawTextEx.
    //
    // This is the typewriter itself rather than a summary of it.  No scalar
    // can stand in: NovelMessage.count and .kstep can agree on both sides
    // while the formula that turns them into per-glyph alpha disagrees, and
    // a 'g' that goes back to '0' is precisely the bug that hid from a trace
    // carrying only the counters.
    // Nothing when the window is not on screen, which is what the
    // reference records: with DSP_SetTextDisp off, DrawGraphText is never
    // reached and no glyph goes by.  Ours has to agree about *when* there is
    // text as well as about what it looks like.
    if (!avg_msg_ || !message_visible_ || msg().state().disp == 0
        || ui_mode_ != UiMode::game || message_.empty()
        // The engine's log hides TXT_WINDOW while an older entry is up.
        || !msg().main_text_disp()) {
        return "-";
    }
    return trace_glyph_string();
}

std::string Game::trace_glyph_string() const
{
    // TXT_WINDOW's glyphs whether or not it is on screen: what a pass
    // through DrawGraphText would report of it.
    if (!avg_msg_ || message_.empty()) {
        return "-";
    }
    const auto shown = msg().visible_glyphs();
    if (shown == 0) {
        return "-";
    }
    std::string out;
    out.reserve(shown);
    for (std::size_t i = 0; i < shown; ++i) {
        int v = msg().glyph_alpha(i) / 16;
        v = std::clamp(v, 0, 16);
        out.push_back(static_cast<char>(v < 10 ? '0' + v : 'a' + (v - 10)));
    }
    return out;
}

void Game::enable_recording(const std::filesystem::path& path,
                            std::uint64_t from)
{
    if (!trace_mode_) {
        throw std::runtime_error("--record needs trace mode");
    }
    if (from > 0 && trace_script_.size() == 0) {
        throw std::runtime_error(
            "--record-from needs a --trace-input to play up to it");
    }
    record_from_ = from;
    recorder_.begin(path, from > 0 ? &trace_script_ : nullptr, from);
    SDL_Log("record: writing %s, live from tick %llu", path.string().c_str(),
            static_cast<unsigned long long>(from));
}

void Game::set_trace_hold(std::uint64_t tick, int seconds)
{
    trace_hold_tick_ = tick;
    trace_hold_seconds_ = seconds;
}

void Game::set_trace_checkpoint(const std::filesystem::path& save_file,
                                std::uint64_t save_tick,
                                const std::filesystem::path& resume_file,
                                std::uint64_t resume_trigger)
{
    trace_save_file_ = save_file;
    trace_save_tick_ = save_tick;
    trace_resume_file_ = resume_file;
    trace_resume_trigger_ = resume_trigger;
    trace_resume_pending_ = !resume_file.empty();
}

// The tick and the free-running counters the save body does not carry, in a
// text sidecar next to it.  Text so a truncated or stale one fails to parse
// rather than resuming at a plausible-looking wrong tick, which would put
// the two sides a few ticks out of phase and look like an engine bug.
namespace {

std::filesystem::path checkpoint_meta(const std::filesystem::path& save)
{
    return std::filesystem::path(save.string() + ".meta");
}

// The message machine, appended to the trace checkpoint.
//
// save_body() is the player-facing save, and the engine's is not a snapshot:
// it carries ms_step1/ms_count/ms_kstep and rebuilds the rest, so a load lands
// near where the save was taken rather than on it.  Ours did not carry the
// machine at all, so a resumed run came back with no message where a linear
// run had one mid-reveal (count 205 of 213 at tick 3000, against 0 of 0), and
// the two sides then drifted apart and re-synced for the rest of the window.
// Appended here rather than added to save_body so the player save's format is
// left alone - matching the engine's *lossy* load is a separate question from
// resuming a trace exactly, and only the second one is this file's business.
void write_state_i32(std::ostream& out, int value)
{
    out.write(reinterpret_cast<const char*>(&value), sizeof value);
}

int read_state_i32(std::istream& in)
{
    int value = 0;
    in.read(reinterpret_cast<char*>(&value), sizeof value);
    return value;
}

}  // namespace

void Game::trace_checkpoint_save()
{
    if (trace_save_file_.empty() || trace_tick_ != trace_save_tick_) {
        return;
    }
    std::ofstream file(trace_save_file_, std::ios::binary);
    if (!file) {
        SDL_Log("trace: cannot write checkpoint %s",
                trace_save_file_.string().c_str());
        return;
    }
    save_body(file);
    {
        const auto& m = msg().state();
        for (const int field : {m.flag, m.add_flag, m.disp, m.step1, m.step2,
                                m.count, m.kstep, m.max}) {
            write_state_i32(file, field);
        }
        const auto& tone = msg().half_tone();
        write_state_i32(file, tone.tstep);
        write_state_i32(file, tone.tcount);
        // The text slot, back again.  It was dropped when the reference was
        // still being resumed too, because its load does not restore
        // TextStruct and carrying ours made the two sides differ at the
        // resume tick by construction.  window.sh stopped resuming the
        // reference, so symmetry with its losses no longer applies and the
        // only thing that counts is matching a straight-through run - which
        // needs it.  ts->cnt of -1 means the whole line is uncovered; 0
        // means none of it, and the machine reconciles a full count against
        // an empty slot by throwing the message away.
        write_state_i32(file, msg().text_cnt());
        write_state_i32(file, msg().text_step());
        const auto& raw = msg().raw();
        write_state_i32(file, static_cast<int>(raw.size()));
        if (!raw.empty()) {
            file.write(raw.data(),
                       static_cast<std::streamsize>(raw.size()));
        }
        // The background machine.  Left out until now because both sides
        // lost it identically and restoring ours alone would have *created*
        // a divergence - but the reference is no longer resumed at all
        // (reference/window.sh), so symmetry with its losses has stopped
        // mattering and only fidelity to a straight-through run counts.
        // BackStruct is plain data, so it round-trips as itself.
        {
            const auto& b = back();
            file.write(reinterpret_cast<const char*>(&b), sizeof b);
        }
        // EOprFlag.  A waiting opcode has already run its set-up, and the
        // latch is how the machine knows not to run it twice:
        //     if(!EOprFlag[ESC_SETMESSAGE2]){ EOprFlag[..]=1; ...set up... }
        // Without it a resumed run re-entered the instruction it was parked
        // on and set the message up again, so at the same pc it threw away a
        // message 205 characters into 213 and started the next one - which
        // reads as the two engines disagreeing when it is only ours
        // forgetting where it was.
        // Marked with its size: it grew from 256 to 512 entries (event
        // opcodes run to 297), and a checkpoint from before that would
        // otherwise be read misaligned without a word.
        write_state_i32(file, eopr_checkpoint_marker);
        write_state_i32(file, static_cast<int>(eopr_flag_.size()));
        file.write(reinterpret_cast<const char*>(eopr_flag_.data()),
                   static_cast<std::streamsize>(eopr_flag_.size()));
        // The waits.  A checkpoint that lands mid-wait used to resume with
        // none of this and the script simply carried on: taken 30 frames
        // into the WaitFrame at pc 5635 of 010319100.sdt, the resumed run
        // left one tick after the resume where a straight-through run - and
        // the reference - stood still.  That is invisible until the drift
        // reaches the compared part of the window, so it reads as an engine
        // divergence hundreds of ticks later.
        write_state_i32(file, wake_frames_);
        write_state_i32(file, wake_time_ ? 1 : 0);
        write_state_i32(
            file,
            wake_time_
                ? static_cast<int>(
                      std::chrono::duration_cast<std::chrono::milliseconds>(
                          *wake_time_ - engine_now()).count())
                : 0);
        write_state_i32(file, audio_wait_ ? 1 : 0);
        write_state_i32(
            file, audio_wait_ ? static_cast<int>(audio_wait_->kind) : 0);
        write_state_i32(
            file, audio_wait_ ? static_cast<int>(audio_wait_->channel) : 0);
        // trace_se_playing answers from this, so a resumed run with an empty
        // map calls every effect finished and walks past an SEW the engine
        // honours.  Stored as "how long ago", because the tick restarts.
        write_state_i32(file, static_cast<int>(trace_sound_started_.size()));
        for (const auto& [channel, started] : trace_sound_started_) {
            write_state_i32(file, static_cast<int>(channel));
            write_state_i32(file, static_cast<int>(trace_tick_ - started));
        }
        // AVG_SetWaitFrame's latch, which is what a parked WaitFrame really
        // sits on - not wake_frames_, which belongs to the VM's own Wait and
        // was measured at zero here.  This is the one that was lost.
        write_state_i32(file, wait_frame_.flag);
        write_state_i32(file, wait_frame_.type);
        write_state_i32(file, wait_frame_.count);
        write_state_i32(file, wait_frame_.max);
        const auto resume = msg().resume_state();
        for (const int field : {resume.vanilla_layout, resume.end_key_wait,
                                resume.auto_count, resume.key_wait_count,
                                resume.key_wait_count2, resume.wstep,
                                resume.demo_cnt}) {
            write_state_i32(file, field);
        }
        // Game::message_, the laid-out message the glyphs are drawn from.
        // AvgMsg's raw source was already here, but this is a separate
        // object and load_body resets it to empty - so a checkpoint taken
        // mid-message resumed with every counter right and nothing on
        // screen.  Measured at tick 759000 of 040426300.sdt, where the
        // reference had 295 glyphs up and ours had none.
        const auto& segments = message_.segments();
        write_state_i32(file, static_cast<int>(segments.size()));
        for (const auto& segment : segments) {
            write_state_i32(file, static_cast<int>(segment.size()));
            file.write(segment.data(),
                       static_cast<std::streamsize>(segment.size()));
        }
        write_state_i32(file, static_cast<int>(message_.revealed_count()));
        const auto& visible = message_.visible();
        write_state_i32(file, static_cast<int>(visible.size()));
        file.write(visible.data(),
                   static_cast<std::streamsize>(visible.size()));
        // The engine's log, its mouse rects and the bar's fade.  bmax alone
        // decides where the log handle is drawn, so a resume without it put
        // the handle at the bottom of an empty track.
        msg().write_history(file);
        // The map, the clock and the calendar.  load_body brings back the
        // map's destinations but not the screen: a checkpoint that landed on
        // a map resumed with the script let go past it, into the next scene.
        // The steady route never hit this - it leaves every map within a few
        // ticks - but a held skip key sits on one until something clicks.
        write_state_i32(file, 0x5350414d);   // "MAPS"
        write_state_i32(file, ui_mode_ == UiMode::map ? 1 : 0);
        write_state_i32(file, script_ended_ ? 1 : 0);
        write_state_i32(file, clock_state_ ? 1 : 0);
        if (clock_state_) {
            for (const int field : {clock_state_->target,
                                    clock_state_->start_minutes,
                                    clock_state_->target_minutes,
                                    clock_state_->travel_frames,
                                    clock_state_->frame}) {
                write_state_i32(file, field);
            }
        }
        write_state_i32(file, calendar_state_ ? 1 : 0);
        if (calendar_state_) {
            for (const int field : {calendar_state_->month,
                                    calendar_state_->day,
                                    calendar_state_->weekday,
                                    calendar_state_->holiday,
                                    calendar_state_->dismissing ? 1 : 0,
                                    calendar_state_->frame,
                                    calendar_state_->step ? 1 : 0}) {
                write_state_i32(file, field);
            }
        }
        for (const int field : {map_field_, map_previous_field_, map_hover_,
                                map_slide_ticks_, map_arrow_pressed_,
                                map_anim_frames_, map_sprite_start_,
                                static_cast<int>(map_pointer_x_),
                                static_cast<int>(map_pointer_y_),
                                map_fade_ticks_, map_selected_,
                                map_finish_pending_ ? 1 : 0,
                                map_enter_ticks_,
                                map_enter_finished_this_frame_ ? 1 : 0}) {
            write_state_i32(file, field);
        }
        for (const bool on : map_rect_on_) {
            write_state_i32(file, on ? 1 : 0);
        }
        // The settings the engine bar can change mid-run.  The rest of the
        // config is pinned by enable_trace and the same on every run, but
        // Avg.half_tone moves with the slider, and a resume that put it back
        // to 64 darkened every scene after it by the wrong amount.
        write_state_i32(file, 0x464e4f43);   // "CONF"
        write_state_i32(file, config_.message_half_tone);
        write_state_i32(file, th2::engine_draw_flag);
    }
    file.close();
    std::ofstream meta(checkpoint_meta(trace_save_file_));
    // AVG_ControlMapEvent's select_back is a function static: it outlives
    // every map and every save, and a resumed run has to be handed it.
    meta << trace_tick_ << ' ' << global_count_ << ' ' << map_select_back_
         << ' ' << th2::engine_rand_state << '\n';
    SDL_Log("trace: checkpoint at tick %llu -> %s",
            static_cast<unsigned long long>(trace_tick_),
            trace_save_file_.string().c_str());
}

void Game::trace_checkpoint_resume()
{
    if (!trace_resume_pending_ || trace_tick_ < trace_resume_trigger_) {
        return;
    }
    trace_resume_pending_ = false;

    std::uint64_t tick = 0;
    int global = 0;
    {
        std::ifstream meta(checkpoint_meta(trace_resume_file_));
        if (!meta || !(meta >> tick >> global)) {
            throw std::runtime_error(
                "no checkpoint sidecar for "
                + trace_resume_file_.string());
        }
        int select_back = 0;
        if (meta >> select_back) {
            map_select_back_ = select_back;
        }
        // The engine's rand() sequence, which no save carries either.
        std::uint32_t rand_state = 0;
        if (meta >> rand_state) {
            th2::engine_rand_state = rand_state;
        }
    }
    std::ifstream file(trace_resume_file_, std::ios::binary);
    if (!file || !load_body(file)) {
        throw std::runtime_error(
            "cannot read checkpoint " + trace_resume_file_.string());
    }
    {
        auto& m = msg().state();
        m.flag = read_state_i32(file);
        m.add_flag = read_state_i32(file);
        m.disp = read_state_i32(file);
        m.step1 = read_state_i32(file);
        m.step2 = read_state_i32(file);
        m.count = read_state_i32(file);
        m.kstep = read_state_i32(file);
        m.max = read_state_i32(file);
        auto& tone = msg().half_tone();
        tone.tstep = read_state_i32(file);
        tone.tcount = read_state_i32(file);
        const int text_cnt = read_state_i32(file);
        const int text_step = read_state_i32(file);
        msg().restore_text_slot(text_cnt, text_step);
        const int raw_size = read_state_i32(file);
        std::string raw(static_cast<std::size_t>(std::max(0, raw_size)), '\0');
        if (raw_size > 0) {
            file.read(raw.data(), static_cast<std::streamsize>(raw_size));
        }
        msg().restore_raw(std::move(raw));
        file.read(reinterpret_cast<char*>(&back()), sizeof(th2::BackStruct));
        if (read_state_i32(file) != eopr_checkpoint_marker
            || read_state_i32(file) != static_cast<int>(eopr_flag_.size())) {
            throw std::runtime_error(
                "checkpoint from an older build (EOprFlag layout): "
                + trace_resume_file_.string());
        }
        file.read(reinterpret_cast<char*>(eopr_flag_.data()),
                  static_cast<std::streamsize>(eopr_flag_.size()));
        if (!file) {
            throw std::runtime_error(
                "checkpoint has no message machine: "
                + trace_resume_file_.string());
        }
        // The waits, appended later than the rest: a checkpoint written
        // before they were carried simply ends here, and such a file still
        // loads - it just resumes without them, which is what it always did.
        const int wake_frames = read_state_i32(file);
        const int has_wake_time = read_state_i32(file);
        const int wake_ms = read_state_i32(file);
        const int has_audio_wait = read_state_i32(file);
        const int audio_kind = read_state_i32(file);
        const int audio_channel = read_state_i32(file);
        const int sounds = read_state_i32(file);
        std::vector<std::pair<int, int>> sound_ages;
        if (file && sounds >= 0 && sounds < 4096) {
            for (int i = 0; i < sounds && file; ++i) {
                const int channel = read_state_i32(file);
                const int age = read_state_i32(file);
                sound_ages.emplace_back(channel, age);
            }
        }
        const int wf_flag = read_state_i32(file);
        const int wf_type = read_state_i32(file);
        const int wf_count = read_state_i32(file);
        const int wf_max = read_state_i32(file);
        th2::AvgMsg::ResumeState resume;
        resume.vanilla_layout = read_state_i32(file);
        resume.end_key_wait = read_state_i32(file);
        resume.auto_count = read_state_i32(file);
        resume.key_wait_count = read_state_i32(file);
        resume.key_wait_count2 = read_state_i32(file);
        resume.wstep = read_state_i32(file);
        resume.demo_cnt = read_state_i32(file);
        const auto read_blob = [&file]() {
            const int size = read_state_i32(file);
            std::string value(
                static_cast<std::size_t>(std::max(0, size)), '\0');
            if (size > 0 && file) {
                file.read(value.data(), static_cast<std::streamsize>(size));
            }
            return value;
        };
        std::vector<std::string> segments;
        const int segment_count = read_state_i32(file);
        if (file && segment_count >= 0 && segment_count < 4096) {
            for (int i = 0; i < segment_count && file; ++i) {
                segments.push_back(read_blob());
            }
        }
        const int revealed = read_state_i32(file);
        const std::string visible = read_blob();
        if (file) {
            message_.restore_state(
                segments, static_cast<std::size_t>(std::max(0, revealed)),
                visible);
            msg().restore_resume_state(resume);
            wait_frame_ = WaitFrameState{wf_flag, wf_type, wf_count, wf_max};
            wake_frames_ = wake_frames;
            wake_time_ = has_wake_time
                ? std::optional{engine_now()
                                + std::chrono::milliseconds(wake_ms)}
                : std::nullopt;
            audio_wait_ = has_audio_wait
                ? std::optional{AudioWait{
                      static_cast<AudioWaitKind>(audio_kind),
                      static_cast<std::size_t>(audio_channel)}}
                : std::nullopt;
            pending_sound_ages_ = std::move(sound_ages);
        }
        // Appended last: the engine's log, its mouse rects and the bar.
        if (file && !msg().read_history(file)) {
            SDL_Log("trace: checkpoint has no engine history");
        }
        if (file && read_state_i32(file) == 0x5350414d) {
            const bool in_map = read_state_i32(file) != 0;
            const bool ended = read_state_i32(file) != 0;
            std::optional<ClockState> clock;
            if (read_state_i32(file) != 0) {
                ClockState c;
                c.target = read_state_i32(file);
                c.start_minutes = read_state_i32(file);
                c.target_minutes = read_state_i32(file);
                c.travel_frames = read_state_i32(file);
                c.frame = read_state_i32(file);
                clock = c;
            }
            std::optional<CalendarState> calendar;
            if (read_state_i32(file) != 0) {
                CalendarState c;
                c.month = read_state_i32(file);
                c.day = read_state_i32(file);
                c.weekday = std::clamp(read_state_i32(file), 0, 6);
                c.holiday = read_state_i32(file);
                c.dismissing = read_state_i32(file) != 0;
                c.frame = read_state_i32(file);
                c.step = read_state_i32(file) != 0;
                calendar = c;
            }
            std::array<int, 14> m{};
            for (auto& field : m) {
                field = read_state_i32(file);
            }
            std::array<bool, 16> rects{};
            for (auto& on : rects) {
                on = read_state_i32(file) != 0;
            }
            if (file) {
                if (in_map) {
                    // Textures and the screen, from the saved destinations;
                    // then everything begin_map starts afresh, as it was.
                    begin_map();
                    map_field_ = std::clamp(m[0], 0, 4);
                    map_previous_field_ = std::clamp(m[1], 0, 4);
                    map_hover_ = m[2];
                    map_slide_ticks_ = m[3];
                    map_arrow_pressed_ = m[4];
                    map_anim_frames_ = m[5];
                    map_sprite_start_ = m[6];
                    map_pointer_x_ = static_cast<float>(m[7]);
                    map_pointer_y_ = static_cast<float>(m[8]);
                    map_fade_ticks_ = m[9];
                    map_selected_ = m[10];
                    map_finish_pending_ = m[11] != 0;
                    map_enter_ticks_ = m[12];
                    map_enter_finished_this_frame_ = m[13] != 0;
                    map_rect_on_ = rects;
                }
                script_ended_ = ended;
                clock_state_ = clock;
                calendar_state_ = calendar;
            }
        }
        if (file && read_state_i32(file) == 0x464e4f43) {
            const int half_tone = read_state_i32(file);
            const int draw_flag = read_state_i32(file);
            if (file) {
                config_.message_half_tone = half_tone;
                th2::engine_draw_flag = draw_flag;
            }
        }
    }
    global_count_ = global;
    trace_tick_ = tick;
    trace_sound_started_.clear();
    for (const auto& [channel, age] : pending_sound_ages_) {
        if (channel >= 0 && age >= 0
            && static_cast<std::uint64_t>(age) <= trace_tick_) {
            trace_sound_started_[static_cast<std::size_t>(channel)] =
                trace_tick_ - static_cast<std::uint64_t>(age);
        }
    }
    pending_sound_ages_.clear();
    SDL_Log("trace: resumed at tick %llu from %s",
            static_cast<unsigned long long>(trace_tick_),
            trace_resume_file_.string().c_str());
}

std::chrono::steady_clock::time_point Game::engine_now() const
{
    // 1000/60 == 16, integer divided, exactly as MAIN_Loop steps next_time
    // and as th2ref_time() reports it.  Deriving it from the tick rather
    // than accumulating keeps the two sides' arithmetic identical - and in
    // normal play keeps every wait, fade and flash counted in the same
    // ticks the rest of the engine is, rather than in whatever the host's
    // clock did between them.
    const auto ticks = trace_mode_
        ? trace_tick_
        : static_cast<std::uint64_t>(std::max(global_count_, 0));
    return std::chrono::steady_clock::time_point{}
        + std::chrono::milliseconds(ticks * engine_tick_ms);
}

void Game::trace_dump_state()
{
    // A resumed run's lead-in ticks would write lines numbered from the
    // title screen, leaving the trace with two disjoint tick ranges in one
    // file for statediff to trip over.
    if (trace_resume_pending_) {
        return;
    }
    // One line per tick of the integers the AVG machine turns on.  Pixels
    // say two runs disagree; this says which counter disagreed first, and
    // for a scene that is a black screen for two hundred frames it is the
    // only thing that says anything at all.
    if (!trace_state_) {
        return;
    }
    const auto& message = avg_msg_ ? msg().state() : th2::NovelMessageState{};
    const auto& tone = avg_msg_ ? msg().half_tone() : th2::HalfToneState{};
    const auto& bk = avg_back_ ? back() : th2::BackStruct{};
    const auto name = std::string(runtime_.script_name());
    // On the frame a choice is answered the glyphs were drawn and only then
    // hidden, which is what the reference records.  Every other frame
    // computes the string here as usual.
    std::string glyphs = frame_undrawn_ ? std::string("-")
                                        : trace_glyph_alpha();
    // Not drawn, but measured: MSG_WAIT runs DSP_GetTextDispPos every frame
    // for the click indicator, and the reference's probe sees that pass as
    // a draw.  On the frame a click on the bar hides the window the measure
    // has already happened, so the reference records the line anyway.
    if (glyphs == "-" && !trace_glyph_measured_.empty()) {
        glyphs = trace_glyph_measured_;
    }
    trace_glyph_measured_.clear();
    if (trace_choice_answered_) {
        if (glyphs == "-" && !trace_glyph_drawn_.empty()) {
            glyphs = trace_glyph_drawn_;
        }
        trace_choice_answered_ = false;
    }
    std::fprintf(trace_state_,
                 "%llu %s %lu "
                 "%d %d %d %d %d %d %d "
                 "%d %d "
                 "%d %d %d %d %d "
                 "%d %d %d "
                 "%d %d "
                 "%d %d "
                 "%d %d %d %d %d %d %d %d %d %d %d %s\n",
                 static_cast<unsigned long long>(trace_tick_),
                 name.empty() ? "-" : name.c_str(),
                 static_cast<unsigned long>(
                     script_ended_ ? 0 : runtime_.vm_pc()),
                 message.flag, message.disp, message.step1, message.step2,
                 message.count, message.kstep, message.max,
                 tone.tstep, tone.tcount,
                 bk.bno, bk.fd_flag, bk.fd_type, bk.fd_cnt, bk.fd_max,
                 bk.sc_flag, bk.sc_cnt, bk.sc_max,
                 bk.sk_flag, bk.sk_cnt,
                 bk.br_flag, bk.br_cnt,
                 skip_held_ ? 1 : 0, auto_mode_ ? 1 : 0, 60,
                 std::clamp(config_.effect_speed, 0, 4),
                 config_.effect_speed != 0 ? 1 : 0,
                 message_wait_setting(),
                 0,   // Avg.msg_page: hooks.msg_page is a constant false
                 config_.message_half_tone,
                 avg_msg_ ? msg().text_cnt() : 0,
                 avg_msg_ ? msg().text_step() : 0,
                 global_count_,
                 glyphs.c_str());
    std::fflush(trace_state_);

    // A one-shot dump of the message source, for when the two sides agree
    // about every counter and still draw a different number of glyphs - at
    // which point the question is no longer the reveal rules but what text
    // each of them thinks it is revealing.
    if (const char* at = std::getenv("TH2_STRTICK");
        at && avg_msg_ && trace_tick_ == std::strtoull(at, nullptr, 10)) {
        if (std::FILE* f = std::fopen(
                (trace_dir_ / "string.txt").string().c_str(), "wb")) {
            std::fwrite(msg().raw().data(), 1, msg().raw().size(), f);
            std::fclose(f);
        }
    }
}

void Game::trace_apply_input()
{
    // The only way input reaches the engine.  Same semantics as the
    // reference's th2ref_input.cpp: trg on the first tick of a press, btn
    // for its whole duration.
    //
    // Live - normal play, and a recording past its lead-in - the player's
    // hands, sampled for this tick (and written out when recording, so the
    // file replays exactly what was played).  Otherwise the script.
    const bool live = live_input();
    auto state = live
        ? recorder_.sample(trace_mode_
                               ? trace_tick_
                               : static_cast<std::uint64_t>(global_count_))
        : trace_script_.at(trace_tick_);
    if (!live) {
        for (const auto& rule : trace_script_.rules()) {
            if (rule.pc == trace_rule_pc_ && rule.script == trace_rule_script_) {
                state.held.push_back(rule.key);
                state.pressed.push_back(rule.key);
            }
        }
    }
    if (!live && pointer_warp_) {
        // The reference's SetCursorPos stands until the script moves the
        // pointer somewhere else.
        if (state.mouse_x == pointer_warp_->from_x
            && state.mouse_y == pointer_warp_->from_y) {
            state.mouse_x = pointer_warp_->x;
            state.mouse_y = pointer_warp_->y;
        } else {
            pointer_warp_.reset();
        }
    }
    script_mouse_x_ = state.mouse_x;
    script_mouse_y_ = state.mouse_y;
    const bool open = !live || engine_input_open();
    if (!open) {
        // One of the port's own screens is up over the scene.  Its presses
        // never reached the sampler; what is left is the pointer, which the
        // engine may as well keep following.
        const int x = state.mouse_x;
        const int y = state.mouse_y;
        state = {};
        state.mouse_x = x;
        state.mouse_y = y;
    }
    key_cond_ = {};
    key_cond_.trg_enter   = state.is_pressed("enter");
    key_cond_.btn_enter   = state.is_held("enter");
    key_cond_.trg_space   = state.is_pressed("space");
    key_cond_.trg_esc     = state.is_pressed("esc");
    key_cond_.trg_bs      = state.is_pressed("bs");
    key_cond_.btn_bs      = state.is_held("bs");
    key_cond_.trg_shift   = state.is_pressed("shift");
    key_cond_.btn_ctrl    = state.is_held("ctrl");
    key_cond_.btn_alt     = state.is_held("alt");
    key_cond_.trg_home    = state.is_pressed("home");
    key_cond_.trg_end     = state.is_pressed("end");
    key_cond_.btrg_pup    = state.is_pressed("pup");
    key_cond_.btrg_pdown  = state.is_pressed("pdown");
    key_cond_.btrg_up     = state.is_pressed("up");
    key_cond_.btrg_down   = state.is_pressed("down");
    key_cond_.btrg_left   = state.is_pressed("left");
    key_cond_.btrg_right  = state.is_pressed("right");
    // num0..num9, which is how a traced run answers a choice at all.
    for (int digit = 0; digit < 10; ++digit) {
        const std::string name = "num" + std::to_string(digit);
        key_cond_.trg_num[digit] = state.is_pressed(name);
    }
    if (live && open) {
        // The port's devices that have no name in a script: the wheel, the
        // middle button, and the touch and gamepad gestures standing in for
        // the skip key.  A recording that uses them does not replay.
        key_cond_.wheel = std::exchange(live_wheel_, 0);
        key_cond_.mouse_trg_middle = std::exchange(live_middle_, false);
        key_cond_.btn_ctrl = key_cond_.btn_ctrl
            || touch_input_.skip_held() || gamepad_input_.ctrl_skip_held();
    }
    live_wheel_ = 0;
    live_middle_ = false;
    trace_mouse_x_ = state.mouse_x;
    trace_mouse_y_ = state.mouse_y;
    // MUS_RenewMouse, for the history bar and AVG_GetHitKey's rect test:
    // the engine bar takes its rects, edges and repeats from this.
    msg().renew_mouse(trace_mouse_x_, trace_mouse_y_, state.click_held);
    // A gesture standing in for one of the bar's buttons (the left swipe,
    // the gamepad's auto button): pressed as the bar would be, so it goes
    // through the engine's own answer to it.
    if (live && open && live_bar_button_ >= 0) {
        msg().press_bar_button(live_bar_button_);
    }
    live_bar_button_ = -1;
    // The half-tone slider is the player's setting as well as the engine's,
    // and is written out when the button that dragged it comes up.
    if (half_tone_unsaved_ && !state.click_held && !trace_mode_) {
        half_tone_unsaved_ = false;
        th2::save_config(config_path_, config_);
    }
    key_cond_.mouse_trg_left  = state.click;
    key_cond_.mouse_btn_left  = state.click_held;
    key_cond_.mouse_trg_right = state.cancel;
    th2::get_game_key(game_key_, key_cond_, 0, demo_mode_);
    // The map's step does not run while the menu is up over it.
    if (!engine_config_step_) {
        trace_drive_map(state.map_pick, state.map_prefer);
    }
    key_cond_.clear_triggers();
}

void Game::warp_pointer(int x, int y)
{
    // MUS_SetMousePos: the engine moves the cursor, and the next tick's
    // MUS_RenewMouse finds it there.  Live, the sampler's pointer moves and
    // stays until the mouse does; the system cursor is left where it is,
    // since a warp rounded back through the window's scale can land a pixel
    // off the one-pixel row MUS_SetMousePosRect aims at.
    if (live_input()) {
        recorder_.pointer(x, y);
        return;
    }
    // A replay: see trace_apply_input.  The script's own position is what
    // the warp stands in front of.
    const int from_x = pointer_warp_ ? pointer_warp_->from_x : script_mouse_x_;
    const int from_y = pointer_warp_ ? pointer_warp_->from_y : script_mouse_y_;
    pointer_warp_ = PointerWarp{x, y, from_x, from_y};
}

void Game::get_game_key()
{
    // void AVG_GetGameKey(void), for every run: the script's input in a
    // replay, the player's otherwise.
    //
    // Avg.demo swallows the click edge, and with it the cancel, hide, skip
    // and paging keys - the tail of AVG_GetGameKey, now in th2::get_game_key.
    // Measured before it was read: while SetDemoFlag is on, the reference
    // converts *no* scripted click into GameKey.click - 28 consecutive
    // 30-tick bursts at pc 18540..18884 of 040426300.sdt.  It matters
    // because demo mode also drives auto-advance (hooks.auto_flag), so the
    // engine walks a demo scene on the auto timer alone; ours took the auto
    // timer *and* the clicks, and finished a line at tick 764340 that the
    // reference was still typing.
    trace_apply_input();
}

void Game::control_system2()
{
    // Not in AVG_CALENDER: AVG_Main runs only AVG_SetCalender there, and
    // not on the frame the page goes up either (AVG_GAME's map==2 arm).  The
    // skip key's latch stays as it was - measured over a day change
    // mid-skip, where the reference kept Avg.msg_cut set for the ten ticks
    // of the page after the key came up.  The frame ours releases the page
    // on is already the engine's first AVG_GAME frame again, and by then
    // calendar_state_ is gone.
    if (calendar_state_ && calendar_state_->step) {
        return;
    }
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
    //     if( cansel && AVG_ConfigCheck() ){ ... AVG_GoConfig(0); }
    if (game_key_.cansel && !engine_config_step_
        && (ui_mode_ == UiMode::game || ui_mode_ == UiMode::map)
        && engine_config_check()) {
        engine_go_config(0);
    }
}

void Game::play_system_se(int number, int volume)
{
    // AVG_PlaySE3( sno, volume ): the sounds AVG_ControlNovelMessage and the
    // system menu make - 9104 for a button and 9012 for the log.  It is
    // AVG_PlaySE underneath, the unchannelled one, not AVG_PlaySE2's slot 0.
    play_se(-1, number, false, volume);
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
            return;
        }
        const auto reveal_start = message_.visible().size();
        if (message_.reveal_next() && message_.has_hidden_segments()) {
            start_text_reveal(reveal_start);
            return;
        }
        waiting_for_input_ = false;
    }
}


}  // namespace th2app
