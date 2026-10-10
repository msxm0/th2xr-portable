#include "engine_rand.hpp"
#include "game.hpp"

#include "gl_blend.hpp"
#include "image.hpp"

#include "icon.hpp"
#include "image.hpp"

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_system.h>
#include <imgui.h>
#include <zstd.h>

#include <algorithm>
#include <cstdlib>
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

std::optional<th2::ReadMarker> Game::current_read_marker() const
{
    if (current_line_key_.empty() || message_.empty()) {
        return std::nullopt;
    }
    return th2::parse_read_marker(
        current_line_key_ + ':' + std::to_string(message_.revealed_count()));
}

bool Game::current_text_is_read() const
{
    if (replay_mode_) {
        return true;
    }
    const auto marker = current_read_marker();
    return marker && persistent_state_.is_line_read(*marker);
}

void Game::mark_current_text_read()
{
    if (replay_mode_) {
        return;
    }
    const auto marker = current_read_marker();
    if (marker) {
        persistent_state_.mark_line_read(*marker);
    }
}

int Game::map_sakura_type() const
{
    const int month = runtime_.flag(0);
    const int day = runtime_.flag(1);
    if (month == 3) {
        return day <= 15 ? 4 : day <= 28 ? 2 : 0;
    }
    if (month == 4) {
        return day <= 15 ? 0 : day <= 27 ? 2 : 3;
    }
    return 3;
}

std::uint16_t Game::map_u16(
    std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return static_cast<std::uint16_t>(bytes[offset])
        | static_cast<std::uint16_t>(bytes[offset + 1]) << 8;
}

std::uint32_t Game::map_u32(
    std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return static_cast<std::uint32_t>(bytes[offset])
        | static_cast<std::uint32_t>(bytes[offset + 1]) << 8
        | static_cast<std::uint32_t>(bytes[offset + 2]) << 16
        | static_cast<std::uint32_t>(bytes[offset + 3]) << 24;
}

Game::MapCharacter Game::load_sprite_animation(const std::string& stem)
{
    const auto* animation_entry = graphics_.find(stem + ".ani");
    if (!animation_entry) {
        throw std::runtime_error("map animation not found: " + stem);
    }
    const auto bytes = graphics_.read(*animation_entry);
    if (bytes.size() < 36 || map_u32(bytes, 0) != 0x53414e49) {
        throw std::runtime_error("invalid map animation: " + stem);
    }

    // ANIME_STRUCT2 after the 16 byte SPANI_HEADER: flag, then frame - the
    // animation's own rate, which SPR_RenewSprite scales to Avg.frame.
    const int animation_rate = static_cast<int>(map_u32(bytes, 20));
    const auto frame_count = map_u32(bytes, 24);
    const auto sprite_count = map_u32(bytes, 28);
    std::size_t offset = 36;
    MapCharacter result;
    struct Operation {
        int code;
        int first;
        int second;
    };
    std::vector<Operation> operations;
    result.frames.resize(frame_count);
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        if (offset + 24 > bytes.size()) {
            throw std::runtime_error("truncated map animation frames");
        }
        const auto part_count = map_u32(bytes, offset + 20);
        offset += 24;
        for (std::size_t part = 0; part < part_count; ++part) {
            if (offset + 40 > bytes.size()) {
                throw std::runtime_error(
                    "truncated map animation parts");
            }
            if (bytes[offset] != 0) {
                result.frames[frame].push_back(MapSpritePart{
                    SDL_FRect{
                        static_cast<float>(map_u16(bytes, offset + 24)),
                        static_cast<float>(map_u16(bytes, offset + 26)),
                        static_cast<float>(map_u16(bytes, offset + 28)),
                        static_cast<float>(map_u16(bytes, offset + 30)),
                    },
                    static_cast<float>(
                        static_cast<std::int16_t>(
                            map_u16(bytes, offset + 32))),
                    static_cast<float>(
                        static_cast<std::int16_t>(
                            map_u16(bytes, offset + 34))),
                });
            }
            offset += 40;
        }
    }

    for (std::size_t sprite = 0; sprite < sprite_count; ++sprite) {
        if (offset + 8 > bytes.size()) {
            throw std::runtime_error("truncated map animation sprites");
        }
        const auto operation_count = map_u32(bytes, offset + 4);
        offset += 8;
        for (std::size_t operation = 0;
             operation <= operation_count; ++operation) {
            if (offset + 8 > bytes.size()) {
                throw std::runtime_error(
                    "truncated map animation operations");
            }
            const auto code = map_u16(bytes, offset);
            if (sprite == 0) {
                operations.push_back(Operation{
                    static_cast<int>(code),
                    static_cast<int>(map_u16(bytes, offset + 2)),
                    static_cast<int>(map_u16(bytes, offset + 4)),
                });
                result.program.push_back(SpriteOperation{
                    static_cast<int>(code),
                    static_cast<int>(map_u16(bytes, offset + 2)),
                    static_cast<int>(map_u16(bytes, offset + 4)),
                    static_cast<int>(map_u16(bytes, offset + 6)),
                });
            }
            offset += 8;
        }
    }
    struct Loop {
        std::size_t start;
        int remaining;
    };
    std::vector<Loop> loops;
    for (std::size_t pc = 0, guard = 0;
         pc < operations.size() && guard < 10000; ++guard) {
        const auto& operation = operations[pc];
        if (operation.code == 0) {
            break;
        }
        if (operation.code == 1) {
            result.steps.push_back(MapSpriteStep{
                operation.first, operation.second + 1});
            ++pc;
        } else if (operation.code == 2) {
            loops.push_back(Loop{
                pc + 1, operation.first + 1});
            ++pc;
        } else if (operation.code == 3 && !loops.empty()) {
            auto& loop = loops.back();
            if (--loop.remaining > 0) {
                pc = loop.start;
            } else {
                loops.pop_back();
                ++pc;
            }
        } else {
            ++pc;
        }
    }
    if (result.steps.empty()) {
        result.steps.push_back({0, 1});
    }
    result.rate = animation_rate > 0 ? animation_rate : 60;
    result.texture =
        load_texture(renderer_, graphics_, stem + ".tga");
    return result;
}

Game::MapCharacter Game::load_map_character(const Game::MapEvent& event)
{
    return load_sprite_animation(std::format(
        "mapc{:02d}{}", event.character, event.type));
}

int Game::weekday(int month, int day) const
{
    // Non-negative whatever the day: it indexes the calendar's weekday table,
    // and the day can come from a save's flags.
    const auto wrap = [](int value) { return ((value % 7) + 7) % 7; };
    if (month == 3) return wrap(day);
    if (month == 4) return wrap(day + 3);
    if (month == 5) return wrap(day + 5);
    return 0;
}

int Game::calendar_holiday(int month, int day) const
{
    struct Holiday {
        int month;
        int first;
        int last;
        int index;
    };
    static constexpr std::array holidays{
        Holiday{3, 12, 12, 0}, Holiday{3, 20, 20, 1},
        Holiday{3, 24, 24, 2}, Holiday{3, 25, 31, 3},
        Holiday{4, 1, 7, 3}, Holiday{4, 8, 8, 4},
        Holiday{4, 29, 29, 5}, Holiday{5, 3, 3, 6},
        Holiday{5, 4, 4, 7}, Holiday{5, 5, 5, 8},
    };
    for (const auto& holiday : holidays) {
        if (holiday.month == month
            && day >= holiday.first && day <= holiday.last) {
            return holiday.index;
        }
    }
    return -1;
}

// The clock and the calendar run on engine_now(), like everything else a
// trace can see.  On steady_clock::now() their frame counter was real time,
// so the hands turned at whatever rate the machine happened to manage and the
// animation took a different number of ticks on every run.
void Game::begin_clock(int requested, int first_frame)
{
    const int current = std::clamp(runtime_.flag(7), 0, 19);
    int target = std::clamp(requested, 0, 19);
    if (current == target) {
        return;
    }
    if (weekday(runtime_.flag(0), runtime_.flag(1)) == 6) {
        target = std::min(target, 11);
    }
    if (!clock_background_) {
        clock_background_ =
            load_texture(renderer_, graphics_, "clock98.tga");
        clock_animation_ = load_sprite_animation("clock99");
    }
    const int start_minutes = clock_minutes_[current];
    const int target_minutes = clock_minutes_[target];
    clock_state_ = ClockState{
        target, start_minutes, target_minutes,
        // Step 2 adds its six minutes before it compares, so it runs at
        // least once - also on a Saturday, where the target is capped at 11
        // and a clock already there still steps 12:10 -> 12:16 (04/10).
        std::max(1, (target_minutes - start_minutes + 5) / 6),
        // Frame 1, not 0.  AVG_ViewClock's case 0 sets the clock up and falls
        // through into case 1 without a break, so the call that starts the
        // animation is also its first frame:
        //     case 0: ...set up...; count = 0; step = 1;
        //     case 1: count = min(count+1,16); ...
        // Starting at zero put every clock in the game one tick long.
        first_frame,
    };
}

void Game::begin_calendar(int month, int day)
{
    if (skipped_month_ != 0) {
        runtime_.set_flag(0, skipped_month_);
        runtime_.set_flag(1, skipped_day_);
        runtime_.set_flag(2, -1);
        runtime_.set_flag(3, -1);
        runtime_.set_flag(4, 0);
        runtime_.set_flag(7, 0);
        skipped_month_ = 0;
        skipped_day_ = 0;
    }
    if (month < 0) {
        month = runtime_.flag(0);
        day = runtime_.flag(1);
    }
    calendar_background_ = load_texture(
        renderer_, graphics_, std::format("cal00{}.tga", month));
    if (!calendar_labels_) {
        calendar_labels_ =
            load_texture(renderer_, graphics_, "cal010.tga");
        calendar_days_ =
            load_texture(renderer_, graphics_, "cal011.tga");
    }
    display().release_bmp(th2::bmp_back);
    display().release_bmp(th2::bmp_back2);
    background_baked_dirty_ = true;
    clear_characters();
    calendar_state_ = CalendarState{
        month, day, weekday(month, day), calendar_holiday(month, day),
        false, 0,
    };
}

// AVG_ViewClock / AVG_SetCalender, stepped once a tick.
//
// Stepped here and not from the wait predicate.  The predicate looked like the
// faithful place - ESC_EOprViewClock calls AVG_ViewClock itself - but it is
// asked exactly once, when the instruction first parks, and never again: the
// clock reached frame 1 of 67 and the script sat on pc 3748 of 010301110.sdt
// for the rest of the run.  What the predicate wanted was the *ordering*, and
// that is had by running this before pump_script rather than after it, so the
// script sees this tick's frame instead of last tick's and leaves on the frame
// the animation ends, as the engine does.
// The map, driven from the trace script's pointer.
//
// AVG_ControlMapEvent answers a map from MUS_GetMouseNo and GameKey.click and
// has no number-key path, so unlike a choice this cannot be answered by a key.
// `pick` asks for the first selectable destination rather than a coordinate:
// the rects are per scene - MapEventCharPos[pos], offset again when several
// destinations share a spot - and only the ones on the displayed page count,
// so no fixed position answers every map in the game.  With none on this page
// it stands on the page arrow instead and lets the next click turn it.
void Game::trace_drive_map(bool pick, const std::vector<int>& prefer)
{
    if (ui_mode_ != UiMode::map) {
        return;
    }
    // th2ref_map_pick, every frame the map's mouse layer is up - fading in
    // and rolling included, since it asks MouseCheck and not the map's
    // step: the centre of the first flagged destination rect, or the
    // forward arrow when none is.  Before step 3 first narrows the flags
    // that is destination 0 wherever it is, which is why the first frame of
    // step 3 can hover something off the page.
    if (pick) {
        trace_map_pick_position(trace_mouse_x_, trace_mouse_y_, prefer);
    }
    // MUS_GetMouseNo reads the cursor wherever the harness left it, so the
    // pointer follows it on every frame, answered or not.
    trace_map_follow_pointer();
    // The map ignores input while it is fading in, sliding or fading out,
    // exactly as handle_map_input does.
    if (clock_state_ || map_enter_ticks_ != 0
        || map_enter_finished_this_frame_
        || map_slide_ticks_ != 0 || map_fade_ticks_ != 0) {
        return;
    }
    update_map_hover(map_pointer_x_, map_pointer_y_);
    if (game_key_.click) {
        if (map_hover_ == -2 || map_hover_ == -3) {
            // case 16/17: AVG_PlaySE3( 9015 ) and GameKey.pup/pdown = 1.
            // It is the key the arrow raises, not a page turn of its own:
            // the turn below answers it, and so does AVG_ControlSystem2 later
            // in the frame - GameKey.pup switches auto mode off, which is
            // how clicking a map arrow ends it.
            play_se(-1, 9015, false, 255);
            map_arrow_pressed_ = map_hover_;
            if (map_hover_ == -2) {
                game_key_.pup = 1;
            } else {
                game_key_.pdown = 1;
            }
        } else if (map_hover_ >= 0 && map_hover_on_page()) {
            finish_map_selection(map_hover_);
        }
    }
    //     if(GameKey.pup){   AVG_PlaySE3( 9015 ); ...+1... }
    //     if(GameKey.pdown){ AVG_PlaySE3( 9015 ); ...-1... }
    // PageUp and PageDown turn the page as well as the arrows do.
    if (game_key_.pup) {
        change_map_field(1);
    } else if (game_key_.pdown) {
        change_map_field(-1);
    }
}

void Game::trace_map_follow_pointer()
{
    map_pointer_x_ = static_cast<float>(trace_mouse_x_);
    map_pointer_y_ = static_cast<float>(trace_mouse_y_);
}

void Game::trace_peek_map_pointer()
{
    // MUS_RenewMouse runs at the top of the engine's frame, before anything
    // else in it - th2ref_input moves the cursor first, map pick included -
    // so AVG_ControlMapEvent's hover reads this tick's pointer.  Ours steps
    // the map (update_map) before the input pass, so the pointer is looked
    // up here, without consuming the tick's input: read a tick late, every
    // hover sound over the map came a tick after the reference's.
    if (ui_mode_ != UiMode::map) {
        return;
    }
    int x = 0;
    int y = 0;
    bool pick = false;
    std::vector<int> prefer;
    if (live_input()) {
        x = recorder_.x();
        y = recorder_.y();
    } else {
        const auto state = trace_script_.at(trace_tick_);
        x = state.mouse_x;
        y = state.mouse_y;
        pick = state.map_pick;
        prefer = state.map_prefer;
    }
    if (pick) {
        trace_map_pick_position(x, y, prefer);
    }
    map_pointer_x_ = static_cast<float>(x);
    map_pointer_y_ = static_cast<float>(y);
}

void Game::trace_map_pick_position(int& out_x, int& out_y,
                                   const std::vector<int>& prefer) const
{
    // th2ref_map_pick: the centre of a flagged destination rect, or the
    // forward arrow (748, 300) when there is none to take.  With a
    // preference, the first listed character that has a destination at all:
    // its rect if it is on this page, otherwise the arrow, until the page
    // with it comes round.  Without one present, the plain pick.  -N in the
    // list: never the destination whose script is N, unless nothing else is
    // on offer - a route's one event that would close it.
    std::vector<std::pair<int, int>> centres(map_events_.size(), {-1, -1});
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
        centres[i] = {cx + 20 + 65, cy - 118 + 59};
    }
    const auto on = [this](std::size_t i) {
        return i < map_rect_on_.size() && map_rect_on_[i];
    };
    const auto denied = [&](std::size_t i) {
        // atoi( MapEvent[n].script_fname ), as the reference shim reads it.
        const long script = std::strtol(map_events_[i].script.c_str(), nullptr, 10);
        return std::ranges::any_of(prefer, [script](int entry) {
            return entry < 0 && -static_cast<long>(entry) == script;
        });
    };
    for (const int character : prefer) {
        if (character < 0) {
            continue;
        }
        for (std::size_t i = 0; i < map_events_.size(); ++i) {
            if (map_events_[i].character != character || centres[i].first < 0
                || denied(i)) {
                continue;
            }
            if (on(i)) {
                out_x = centres[i].first;
                out_y = centres[i].second;
            } else {
                out_x = 748;
                out_y = 300;
            }
            return;
        }
    }
    out_x = 748;
    out_y = 300;
    for (const bool skip_denied : {true, false}) {
        for (std::size_t i = 0; i < map_events_.size(); ++i) {
            if (centres[i].first >= 0 && on(i)
                && !(skip_denied && denied(i))) {
                out_x = centres[i].first;
                out_y = centres[i].second;
                return;
            }
        }
    }
}

void Game::update_clock_calendar()
{
    // Not while the map owns it - see update_map, which steps it in the
    // engine's slot (after the script pass, not before).
    if (clock_state_ && ui_mode_ != UiMode::map) {
        clock_state_->frame += control_steps_;
        if (clock_state_->frame >= 32 + clock_state_->travel_frames) {
            runtime_.set_flag(7, clock_state_->target);
            clock_state_.reset();
            // Not under the map: there the clock is the map's own step 1,
            // not a ViewClock the script is parked on, so there is nothing
            // waiting to be resumed.
            if (ui_mode_ != UiMode::map) {
                advance();
            }
        }
    }
    if (calendar_state_) {
        calendar_state_->frame += control_steps_;
        // AVG_SetCalender's step 3 ends on count 16 (the page at fade 0) and
        // step 4 - the frame after - resets the graphs and returns TRUE, from
        // the control chain, after EXEC_ControlLang: the script goes on from
        // the frame after that.  So the page is released here, before the
        // script pass, on the frame after the one that saw count 17.
        if (calendar_state_->dismissing) {
            if (calendar_finish_pending_) {
                calendar_finish_pending_ = false;
                calendar_state_.reset();
                advance();
            } else if (calendar_state_->frame >= 17) {
                calendar_finish_pending_ = true;
            }
        }
    }
}

void Game::control_calendar_key()
{
    // AVG_SetCalender's step 2, in AVG_Main's slot - after MAIN_SystemControl
    // has sampled this tick's GameKey:
    //     if( AVG_GetMesCut() || AVG_GetHitKey() ){ step=3; count=0; }
    // reached only once step 1 has faded the page in over sixteen frames.
    // Read from GameKey rather than an SDL event, because a trace has no SDL
    // events - driven only by the event loop, a traced calendar was never
    // dismissed and the script would have parked on ViewCalender for good.
    // frame > 16, not >= 16: the frame the fade-in completes on is still a
    // case 1 frame - it sets step = 2 and breaks, so it asks for no key.
    // Checking on the completing frame took a click the reference could not
    // see, and with the harness clicking every thirty ticks that is thirty
    // ticks of difference: measured at the March 8th calendar, where ours
    // dismissed at tick 169111 and the reference waited to 169141.
    //
    // This used to run before the input pass, on last tick's GameKey, with
    // the release a tick early to make up for it.  The script came out on
    // the right tick, but every frame of the fade to black was drawn one tick
    // late (ticks 45361..45376 of the opening, 98% of the pixels off by up to
    // 19) - unseen by a comparison that samples every sixtieth tick.
    //
    // The skip key is not an edge: in AVG_CALENDER, Avg.msg_cut is the latch
    // AVG_ControlSystem2 left before the page went up, the same on every
    // frame of it.
    if (!calendar_state_ || calendar_state_->dismissing
        || calendar_state_->frame <= 16) {
        return;
    }
    if (game_key_.click || message_cut()) {
        calendar_state_->dismissing = true;
        calendar_state_->frame = 0;
    }
}

// One blit the way a graph goes to the rasteriser: through the integer
// blend when the exact path is there, at a DRW_BLD level on the engine's
// 0..256 scale, with a picture that carried an alpha channel treated as the
// premultiplied bitmap the engine stores.  At level 0 it draws nothing,
// which is DRW_DrawBMP_TT_Bld's `if( blnd==0 ) return 1;`.
void Game::draw_engine_blit(SDL_Texture* texture, const SDL_FRect& source,
                            const SDL_FRect& destination, int level,
                            int mode, int bright)
{
    level = std::clamp(level, 0, 256);
    if (level == 0 || !texture) {
        return;
    }
    if (mode < 0) {
        mode = th2::texture_source_folded(texture) ? 6
            : th2::texture_has_source_alpha(texture) ? 3 : 0;
    }
    auto* const exact = display_->gl_exact_blend();
    if (exact && exact->available()
        && exact->capture_destination(renderer_)
        && exact->draw(renderer_, texture, source, destination, false, false,
                       mode, level, bright, bright, bright)) {
        return;
    }
    SDL_SetTextureAlphaMod(
        texture, static_cast<std::uint8_t>(std::min(level, 255)));
    SDL_RenderTexture(renderer_, texture, &source, &destination);
    SDL_SetTextureAlphaMod(texture, 255);
}

// SPR_RenewSprite and RenewSprite, for an ANIME_CONTROL as DSP_SetSprite
// makes it (SP_PLAY, end ON, lnum 0 - loop for ever, speed 100), replayed
// from the start rather than carried, so nothing about it needs saving:
//
//     ac->draw_count += as->frame*ac->speed/frame;      // frame = Avg.frame
//     while( *draw_count > 0 ){
//         NULL:   if(code_no==0) draw_count -= 100;  code_no = 0;
//         DRAW:   count++;
//                 if( count > data2 ){ code_no++; count = 0; }
//                 else draw_count -= 100;
//         LOOP:   lcount[loop_no] = data1 ? data1+1 : 0; loop_no++; code_no++;
//         REPEAT: if(loop_no==0){ code_no++; break; }
//                 switch(lcount[loop_no-1]){
//                     default: lcount[loop_no-1]--;
//                     case 0:  back to the op after the LOOP;
//                     case 1:  lcount = 0; loop_no--; code_no++;
//                 }
//     }
//
// A DRAW holds for data2 counts, not data2+1, and the program runs at the
// animation's rate, not one count a frame - the two things the old (frame,
// ticks) cycle got wrong, which put a map character on the wrong chip.
int Game::sprite_frame_after(const MapCharacter& animation, int renews)
{
    const auto& code = animation.program;
    if (code.empty()) {
        return animation.steps.empty() ? 0 : animation.steps.front().frame;
    }
    const auto at = [&code](long i) -> const SpriteOperation& {
        static const SpriteOperation null_op{};
        return i >= 0 && static_cast<std::size_t>(i) < code.size()
            ? code[static_cast<std::size_t>(i)] : null_op;
    };
    long count = 0;
    long code_no = 0;
    long draw_count = 0;
    int loop_no = 0;
    int lcount[16] = {};
    const long step = static_cast<long>(animation.rate) * 100 / 60;
    for (int r = 0; r < renews; ++r) {
        draw_count += step;
        int guard = 0;
        while (draw_count > 0 && ++guard < 100000) {
            const auto& op = at(code_no);
            switch (op.code) {
            case 0:
                if (code_no == 0) {
                    draw_count -= 100;
                }
                code_no = 0;
                break;
            case 1:
                ++count;
                if (count > op.data2) {
                    ++code_no;
                    count = 0;
                } else {
                    draw_count -= 100;
                }
                break;
            case 2:
                if (loop_no < 16) {
                    lcount[loop_no] = op.data1 ? op.data1 + 1 : 0;
                }
                ++loop_no;
                ++code_no;
                break;
            case 3:
                if (loop_no == 0) {
                    ++code_no;
                    break;
                }
                switch (lcount[std::min(loop_no - 1, 15)]) {
                default:
                    --lcount[std::min(loop_no - 1, 15)];
                    [[fallthrough]];
                case 0: {
                    long i = code_no;
                    while (i >= 0 && at(i).code != 2) {
                        --i;
                    }
                    code_no = i + 1;
                    break;
                }
                case 1:
                    lcount[std::min(loop_no - 1, 15)] = 0;
                    --loop_no;
                    ++code_no;
                    break;
                }
                break;
            default:
                ++code_no;
                break;
            }
        }
    }
    const auto& shown = at(code_no);
    return shown.code == 1 ? shown.data1
        : (animation.steps.empty() ? 0 : animation.steps.front().frame);
}

// DSP_LoadSprite's pictures come through SPR_LoadBmpSet, which asks
// LoadBmpSet for BMP_256P: a 32 bit file keeps its alpha (the depth is the
// file's) but never gets DSP_LoadBmp's BMP_TRUE fold, so it is drawn raw as
// if folded - mode 6, exactly the map fields' case - and a paletted one is
// the ordinary straight blend.
int Game::sprite_blend_mode(SDL_Texture* texture)
{
    return th2::texture_has_source_alpha(texture) ? 6 : 0;
}

void Game::draw_sprite_frame(
    const MapCharacter& animation, int frame, float x, float y, int level)
{
    if (frame < 0
        || static_cast<std::size_t>(frame) >= animation.frames.size()) {
        return;
    }
    for (const auto& part : animation.frames[frame]) {
        const SDL_FRect destination{
            x + part.x, y + part.y, part.source.w, part.source.h};
        draw_engine_blit(animation.texture.get(), part.source, destination,
                         level, sprite_blend_mode(animation.texture.get()));
    }
}

void Game::draw_clock_calendar()
{
    if (clock_state_) {
        const int frame = clock_state_->frame;
        // AVG_ViewClock's DRW_BLD levels, in its own integers:
        //     step 1: count = min(count+1,16);  DRW_BLD( count*16 )
        //     step 2: DRW_NML                   (the hands travelling)
        //     step 3: count = min(count+1,16);  DRW_BLD( 256-count*16 )
        // A float alpha through SDL rounded the dial and the hands a level
        // or two brighter than that for the whole time the clock was up.
        int level = 256;
        if (frame < 16) {
            level = std::max(frame, 0) * 16;
        } else if (frame > 16 + clock_state_->travel_frames) {
            level = 256
                - std::min(frame - 16 - clock_state_->travel_frames, 16) * 16;
        }
        // clock_count += 6 a frame until ViewClockTimeTable[clock] <=
        // clock_count - so it stops on the first step past the target, not
        // on it, and the hands show the overshoot: 12:35 -> 14:50 in steps
        // of six lands on 14:53.
        const int minutes = clock_state_->start_minutes
            + std::min(std::max(0, frame - 16),
                       clock_state_->travel_frames) * 6;
        float width = 0.0f;
        float height = 0.0f;
        SDL_GetTextureSize(clock_background_.get(), &width, &height);
        const SDL_FRect whole{0.0f, 0.0f, width, height};
        draw_engine_blit(clock_background_.get(), whole,
                         SDL_FRect{0.0f, 0.0f, 800.0f, 600.0f}, level);
        draw_sprite_frame(
            *clock_animation_,
            (minutes / 60 % 12) * 10 + minutes % 60 / 6,
            400.0f, 300.0f, level);
        draw_sprite_frame(
            *clock_animation_, 120 + minutes % 60 * 2,
            400.0f, 300.0f, level);
        return;
    }
    if (!calendar_state_) {
        return;
    }
    // AVG_SetCalender, in its own integers:
    //     step 1: count++;  DRW_BLD( count*16 ), DRW_NML from count 16
    //     step 3: count++;  DSP_SetGraphFade( 128-count*8 ) - a brightness,
    //             not an alpha: the page goes to black, not to what is under
    // and each bitmap drawn as it was loaded - the page BMP_FULL (24 bit),
    // cal010 BMP_TRUE (folded), cal011 BMP_256P (paletted).
    const int frame = calendar_state_->frame;
    const int level = calendar_state_->dismissing
        ? 256 : std::clamp(frame, 0, 16) * 16;
    const int bright = calendar_state_->dismissing
        ? std::max(0, 128 - std::min(frame, 16) * 8) : 128;
    const auto blit = [&](SDL_Texture* texture, const SDL_FRect& source,
                          const SDL_FRect& destination) {
        draw_engine_blit(texture, source, destination, level, -1, bright);
    };
    blit(calendar_background_.get(), SDL_FRect{0.0f, 0.0f, 800.0f, 600.0f},
         SDL_FRect{0.0f, 0.0f, 800.0f, 600.0f});

    static constexpr std::array<int, 7> weekday_type{2, 0, 0, 0, 0, 0, 1};
    int day_type = weekday_type[calendar_state_->weekday];
    if (calendar_state_->holiday == 1
        || calendar_state_->holiday >= 5) {
        day_type = 2;
    }
    blit(calendar_days_.get(),
         SDL_FRect{static_cast<float>(day_type * 248),
                   static_cast<float>((calendar_state_->day - 1) * 144),
                   248.0f, 144.0f},
         SDL_FRect{256.0f, 240.0f, 248.0f, 144.0f});
    blit(calendar_labels_.get(),
         SDL_FRect{0.0f, static_cast<float>(calendar_state_->weekday * 32),
                   168.0f, 32.0f},
         SDL_FRect{88.0f, 352.0f, 168.0f, 32.0f});
    blit(calendar_labels_.get(),
         SDL_FRect{168.0f, static_cast<float>(calendar_state_->weekday * 34),
                   34.0f, 34.0f},
         SDL_FRect{504.0f, 347.0f, 34.0f, 34.0f});
    const int holiday = calendar_state_->holiday;
    blit(calendar_labels_.get(),
         SDL_FRect{
             static_cast<float>(202 + (holiday < 0 ? 0 : holiday / 3 * 164)),
             static_cast<float>((holiday < 0 ? 3 : holiday % 3) * 50),
             164.0f, 50.0f},
         SDL_FRect{538.0f, 331.0f, 164.0f, 50.0f});
}

Texture Game::load_sakura_texture(std::string_view name)
{
    const auto* entry = graphics_.find(name);
    if (!entry) {
        throw std::runtime_error(
            "image not found: " + std::string(name));
    }
    Surface surface(th2::load_image(
        graphics_.read(*entry), entry->name));
    const auto props = SDL_GetSurfaceProperties(surface.get());
    if (!SDL_GetBooleanProperty(props, th2::bmp_alpha_plane_property, false)) {
        SDL_SetSurfaceColorKey(
            surface.get(), true,
            SDL_MapSurfaceRGB(surface.get(), 0, 0, 0));
    }
    const SDL_Point pos{
        static_cast<int>(SDL_GetNumberProperty(props, th2::bmp_pos_x_property, 0)),
        static_cast<int>(SDL_GetNumberProperty(props, th2::bmp_pos_y_property, 0))};
    (name == "sakura.bmp" ? sakura_large_pos_ : sakura_small_pos_) = pos;
    return texture_from_surface(surface.get());
}

void Game::start_sakura(int amount, bool no_reset)
{
    if (!sakura_large_) {
        sakura_large_ = load_sakura_texture("sakura.bmp");
        sakura_small_ = load_sakura_texture("sakura2.bmp");
    }
    if (!sakura_) {
        sakura_ = SakuraState{};
    }
    sakura_->target_amount = std::clamp(amount, 0, 200);
    sakura_->wind = 1.0f;
    sakura_->speed = 10;
    sakura_->reset_frames = -1;
    sakura_->no_reset = no_reset;
}

void Game::stop_sakura(bool force)
{
    if (sakura_ && (force || !sakura_->no_reset)
        && sakura_->reset_frames < 0) {
        sakura_->no_reset = false;
        sakura_->reset_frames = 0;
    }
}

int Game::seasonal_background_scene(int scene) const
{
    const int variant = scene % 10;
    int base = scene / 10;
    if (base >= 10000) {
        return scene;
    }
    if ((base >= 1 && base <= 4) || base == 78) {
        base = 1;
    } else if ((base >= 5 && base <= 8) || base == 79) {
        base = 5;
    } else if ((base >= 34 && base <= 37) || base == 80) {
        base = 34;
    } else if ((base >= 48 && base <= 51) || base == 81) {
        base = 48;
    } else {
        return scene;
    }

    int type = 3;
    const int month = runtime_.flag(0);
    const int day = runtime_.flag(1);
    if (month == 3) {
        type = day <= 15 ? 4 : day <= 28 ? 2 : 0;
    } else if (month == 4) {
        type = day <= 15 ? 0 : day <= 27 ? 1 : 3;
    }
    if (type == 4) {
        base = base == 1 ? 78 : base == 5 ? 79
            : base == 34 ? 80 : 81;
    } else {
        base += type;
    }
    return base * 10 + variant;
}

void Game::update_background_sakura(int scene, bool background)
{
    if (background) {
        const int base = scene / 10;
        if (base == 1 || base == 5 || base == 34 || base == 48) {
            start_sakura(32, false);
            return;
        }
    }
    stop_sakura(false);
}

// The weather draws from the engine's one rand(), the same sequence the
// shakes and the script's RAND take from - see engine_rand.hpp.  Its own
// generator put every petal somewhere the reference's is not.
std::uint32_t Game::next_sakura_random()
{
    return static_cast<std::uint32_t>(th2::engine_rand());
}

void Game::spawn_sakura_petals()
{
    if (!sakura_ || sakura_->reset_frames >= 0) {
        return;
    }
    while (sakura_->amount < sakura_->target_amount
           && sakura_->amount < static_cast<int>(sakura_->petals.size())
           && sakura_->tick % 5 == 0) {
        auto& petal = sakura_->petals[sakura_->amount++];
        petal.active = true;
        petal.type = static_cast<int>(next_sakura_random() % 6);
        petal.x = static_cast<float>(next_sakura_random() % 800);
        petal.y = -static_cast<float>(next_sakura_random() % 100);
        // (float)(rand()%((6-type)*100)/100.0+1)/2 - the division is a
        // double one, rounded to float only after the +1.
        const int range = (6 - petal.type) * 100;
        const auto axis = [&] {
            const int r = static_cast<int>(next_sakura_random()) % range;
            return static_cast<float>(static_cast<double>(r) / 100.0 + 1.0)
                / 2.0f;
        };
        petal.axis_x = axis();
        petal.axis_y = axis();
        petal.counter = next_sakura_random() % 256;
        break;
    }
}

// The petals counted wall-clock sixtieths and clamped the result to 8, so
// on a slow frame they advanced by however long the frame took and on a
// stalled one they silently dropped whatever the clamp cut.  Everything else
// in AVG_System's chain - the background, the half tone, the characters -
// takes the frame count the top of the tick already worked out, and the
// petals are part of that same chain.  They take it too.
void Game::update_sakura(int steps)
{
    if (!sakura_ || steps <= 0) {
        return;
    }
    for (int step = 0; step < steps; ++step) {
        ++sakura_->tick;
        if (sakura_->reset_frames >= 0) {
            ++sakura_->reset_frames;
        }
        spawn_sakura_petals();
        for (int i = 0; i < sakura_->amount; ++i) {
            auto& petal = sakura_->petals[i];
            if (!petal.active) {
                continue;
            }
            // wos->x += (SIN(wos->cnt)*wos->ax/4096)+Weather.wind;
            // SIN is MM_std's integer table, SinTbl[(X+64)%256], not sin().
            // Evaluated the way the reference's 32 bit x87 build does:
            // the whole right-hand side at extended precision, rounded to
            // float once, when it is stored.  In float the three operations
            // round three times and the petals drift a pixel in a few
            // seconds.
            petal.x = static_cast<float>(
                static_cast<long double>(petal.x)
                + (static_cast<long double>(th2::SIN(
                       static_cast<int>(petal.counter % 256)))
                       * static_cast<long double>(petal.axis_x) / 4096.0L
                   + static_cast<long double>(sakura_->wind)));
            petal.y = static_cast<float>(
                static_cast<long double>(petal.y)
                + static_cast<long double>(petal.axis_y));
            if (petal.y > 600.0f) {
                if (sakura_->reset_frames >= 0) {
                    petal.active = false;
                } else {
                    petal.x =
                        static_cast<float>(next_sakura_random() % 800);
                    petal.y =
                        -static_cast<float>(next_sakura_random() % 100);
                }
            }
            if (petal.x >= 800.0f) {
                if (sakura_->reset_frames >= 0) {
                    petal.active = false;
                    continue;
                }
                petal.x -= 830.0f;
            } else if (petal.x < -30.0f) {
                if (sakura_->reset_frames >= 0) {
                    petal.active = false;
                    continue;
                }
                petal.x += 830.0f;
            }
            ++petal.counter;
        }
    }
    if (sakura_->reset_frames >= 16) {
        sakura_.reset();
    }
}

void Game::draw_sakura()
{
    // !weather_disp_: AVG_CloseBack has hidden every GRP_WEATHER graph.
    if (!sakura_ || !weather_disp_) {
        return;
    }
    {
        static std::FILE* wlog = [] {
            const char* path = std::getenv("TH2_WEATHER_LOG");
            return path && *path ? std::fopen(path, "w") : nullptr;
        }();
        if (wlog) {
            std::fprintf(wlog, "%llu n%d c%d",
                         static_cast<unsigned long long>(trace_tick_),
                         sakura_->amount, sakura_->tick);
            for (int k = 0; k < 3 && k < sakura_->amount; ++k) {
                const auto& o = sakura_->petals[k];
                unsigned int bx, by;
                std::memcpy(&bx, &o.x, 4);
                std::memcpy(&by, &o.y, 4);
                std::fprintf(wlog, " [t%d x%.4f y%.4f %08x %08x k%u]",
                             o.type, o.x, o.y, bx, by, o.counter);
            }
            std::fputc('\n', wlog);
            std::fflush(wlog);
        }
    }
    const float alpha = sakura_->reset_frames < 0
        ? 1.0f
        : std::clamp(
            1.0f - sakura_->reset_frames / 16.0f, 0.0f, 1.0f);
    // The same fade on the rasteriser's 0..256 scale, for the exact blend.
    const int alpha_256 = sakura_->reset_frames < 0
        ? 256
        : std::clamp(256 - sakura_->reset_frames * 16, 0, 256);
    // The engine draws weather as an ordinary graph out of BMP_WEATHER, so
    // it goes through Draw32's BlendTable like everything else.  Ours draws
    // it here rather than through Display::draw_graph, which meant it was
    // the one layer still compositing with SDL's rounding - and the petals
    // were the last pixels in the port more than two levels off the
    // reference.
    auto* const exact = display_->gl_exact_blend();
    const bool exact_ready = exact && exact->available();
    if (!exact_ready) {
        SDL_SetTextureAlphaModFloat(sakura_large_.get(), alpha);
        SDL_SetTextureAlphaModFloat(sakura_small_.get(), alpha);
    }
    // (int)(26.6666667f*(n)) as the reference's x87 build evaluates it: the
    // float constant times the int at extended precision, truncated - so
    // 13.333333f*15 is 199.99999 and gives 199, where rounding the product
    // to float first gives exactly 200 and a chip one column over.
    const auto chip_offset = [](float factor, int n) {
        return static_cast<float>(static_cast<int>(
            static_cast<long double>(factor) * static_cast<long double>(n)));
    };
    for (int i = 0; i < sakura_->amount; ++i) {
        const auto& petal = sakura_->petals[i];
        if (!petal.active) {
            continue;
        }
        // DSP_SetGraphSPos is set from wos->cnt before the wos->cnt++ at the
        // end of the same pass, so the chip on screen is one behind the
        // counter this has already advanced.
        const std::uint32_t shown_counter = petal.counter - 1;
        SDL_FRect source;
        Texture* texture = nullptr;
        if (petal.type == 0) {
            const int frame = shown_counter / 2 % 23;
            source = {
                static_cast<float>(40 * (frame % 10)),
                chip_offset(26.6666667f, (frame / 10)),
                40.0f, 27.0f};
            texture = &sakura_small_;
        } else if (petal.type == 1) {
            const int frame = shown_counter / 2 % 20;
            source = {
                chip_offset(26.6666667f, (frame % 15)),
                static_cast<float>(80 + 20 * (frame / 15)),
                27.0f, 20.0f};
            texture = &sakura_small_;
        } else if (petal.type == 2) {
            const int frame = shown_counter / 2 % 17;
            source = {
                chip_offset(13.3333333f, (frame % 30)),
                120.0f,
                13.0f, 13.0f};
            texture = &sakura_small_;
        } else if (petal.type == 3) {
            const int frame = shown_counter / 2 % 23;
            source = {
                static_cast<float>(30 * (frame % 10)),
                static_cast<float>(20 * (frame / 10)), 30.0f, 20.0f};
            texture = &sakura_large_;
        } else if (petal.type == 4) {
            const int frame = shown_counter / 2 % 20;
            source = {
                static_cast<float>(20 * (frame % 15)),
                static_cast<float>(60 + 15 * (frame / 15)),
                20.0f, 15.0f};
            texture = &sakura_large_;
        } else {
            const int frame = shown_counter / 2 % 17;
            source = {
                static_cast<float>(10 * (frame % 30)), 90.0f,
                10.0f, 10.0f};
            texture = &sakura_large_;
        }
        // DSP_SetGraphMove( GRP_WEATHER+i, (int)wos->x, (int)wos->y ):
        // whole pixels, truncated toward zero - the fraction only steers
        // where the next frame's petal lands.
        SDL_FRect destination{
            static_cast<float>(static_cast<int>(petal.x)),
            static_cast<float>(static_cast<int>(petal.y)),
            source.w, source.h};
        // DSP_DrawGraph: sx - BmpSet.pos.x, sy - BmpSet.pos.y, and the
        // rasteriser clips the source to the bitmap, moving the destination
        // with it.  The small sheet is 394x128 at (1,0) in a 400x160 frame,
        // so its third row of chips (y 120, 13 high) is mostly outside it.
        {
            const SDL_Point pos = texture == &sakura_large_
                ? sakura_large_pos_ : sakura_small_pos_;
            source.x -= static_cast<float>(pos.x);
            source.y -= static_cast<float>(pos.y);
            float tw = 0.0f;
            float th = 0.0f;
            SDL_GetTextureSize(texture->get(), &tw, &th);
            if (source.x < 0.0f) {
                destination.x -= source.x;
                source.w += source.x;
                source.x = 0.0f;
            }
            if (source.y < 0.0f) {
                destination.y -= source.y;
                source.h += source.y;
                source.y = 0.0f;
            }
            source.w = std::min(source.w, tw - source.x);
            source.h = std::min(source.h, th - source.y);
            if (source.w <= 0.0f || source.h <= 0.0f) {
                continue;
            }
            destination.w = source.w;
            destination.h = source.h;
        }
        bool drawn = false;
        if (exact_ready) {
            drawn = exact->capture_destination(renderer_)
                && exact->draw(
                    renderer_, texture->get(), source, destination,
                    false, false, 0, alpha_256, th2::bright_neutral,
                    th2::bright_neutral, th2::bright_neutral);
        }
        if (!drawn) {
            SDL_RenderTexture(
                renderer_, texture->get(), &source, &destination);
        }
    }
}

std::string Game::map_field_name(int field) const
{
    const int variant = field == 1 || field == 4
        ? map_sakura_type() : 0;
    return std::format("map1{}{}.tga", field, variant);
}

void Game::begin_map()
{
    if (std::ranges::none_of(
            map_events_, [](const Game::MapEvent& event) {
                return event.position == 0;
            })) {
        map_events_.push_back(MapEvent{});
    }
    map_frame_ = load_texture(renderer_, graphics_, "map000.tga");
    map_arrows_ = load_texture(renderer_, graphics_, "map010.tga");
    map_markers_ = load_texture(renderer_, graphics_, "map011.tga");
    std::array<bool, 5> present{};
    present[1] = true;
    for (const auto& event : map_events_) {
        if (event.position >= 0
            && event.position < static_cast<int>(map_positions_.size())) {
            present[map_positions_[event.position].field] = true;
        }
    }
    for (int field = 0; field < 5; ++field) {
        map_fields_[field].reset();
        if (present[field]) {
            map_fields_[field] =
                load_texture(renderer_, graphics_, map_field_name(field));
        }
    }
    map_characters_.clear();
    map_characters_.resize(map_events_.size());
    for (std::size_t i = 0; i < map_events_.size(); ++i) {
        const auto& event = map_events_[i];
        if (event.character == 0) {
            continue;
        }
        const auto name = std::format(
            "mapc{:02d}{}.ani", event.character, event.type);
        if (graphics_.find(name)) {
            map_characters_[i] = load_map_character(event);
        }
    }
    map_field_ = 1;
    map_previous_field_ = 1;
    map_hover_ = -1;
    map_slide_ticks_ = 0;
    map_fade_ticks_ = 0;
    map_arrow_pressed_ = 0;
    // case 0: MUS_SetMouseRect( 10, i, ..., 1 ) for every destination.
    map_rect_on_ = {};
    for (std::size_t i = 0; i < map_events_.size() && i < map_rect_on_.size();
         ++i) {
        map_rect_on_[i] = true;
    }
    map_anim_frames_ = 0;
    map_sprite_start_ = -1;
    // AVG_ControlMapEvent's steps 1 and 2, in order:
    //     case 1: if( AVG_ViewClock( 19 ) ){ MapStep = 2; scnt = 0; ... }
    //     case 2: ...DRW_BLD(scnt*16)...; if(scnt==16){ MapStep=3; }
    // so the clock runs the day on to 14:50 and only then does the map fade
    // in.  Ours went straight to the map, which cost it a whole click cycle
    // against the engine - 72 ticks on the screen where the engine spends
    // 103 - because a click that lands while the engine is still counting
    // the clock down is simply not there to be answered.
    // Two frames before the clock's first, which update_map then steps in
    // this same tick.  The engine enters the map in stages:
    //     frame E:   AVG_ToHertDaySinkou() == 1 -> AVG_ChangeSetp(AVG_MAP)
    //     frame E+1: AVG_ControlMapEvent case 0 - set up, MapStep = 1, break
    //     frame E+2: case 1 - AVG_ViewClock( 19 ), its own frame 1
    // Starting the clock at frame 1 here put the whole map - its music, its
    // fade-in and the first hover sound - three frames early.
    begin_clock(19, -2);
    // Three steps, as the clock's first frame is three steps from -2.
    map_clock_instant_ = clock_state_ ? 0 : 3;
    map_enter_ticks_ = 16;
    map_finish_pending_ = false;
    calendar_finish_pending_ = false;
    map_selected_ = -1;
    map_tick_ = std::chrono::steady_clock::now();
    map_started_ = map_tick_;
    // No music yet: AVG_ControlMapEvent starts it in step 1, on the frame
    // AVG_ViewClock(19) reports the clock done - see update_map.
    ui_mode_ = UiMode::map;
}

void Game::finish_map_selection(int selected)
{
    map_selected_ = selected;
    map_fade_ticks_ = 16;
    play_se(-1, 9014, false, 255);
}

void Game::complete_map_selection()
{
    const auto selected = map_events_.at(map_selected_);
    map_events_.clear();
    map_characters_.clear();
    map_frame_.reset();
    map_arrows_.reset();
    map_markers_.reset();
    for (auto& field : map_fields_) {
        field.reset();
    }
    runtime_.set_flag(4, 1);
    ui_mode_ = UiMode::game;
    if (selected.script.empty()) {
        // The engine's step 5 loads nothing for a destination with no script
        // of its own.  It only sets the flags -
        //     ESC_SetFlag(_EVENT_END,1); ESC_SetFlag(_EVENT_NEXT,6);
        // - and the script machine picks them up on a later frame, through
        // the same scheduled load that ends any other script.  Calling
        // load_scheduled_script() here instead put the next script on screen
        // a frame early, which is the tick the map exit was out by.
        runtime_.set_flag(3, 6);
        return;
    }
    load_script(selected.script);
    advance();
}


}  // namespace th2app
