#include "game.hpp"

#include <SDL3/SDL_log.h>

#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace th2app {

std::chrono::steady_clock::time_point Game::menu_now() const
{
    // The port's title and bonus screens count their animations from a time
    // point.  In a trace that is the engine's clock, so the menus play out on
    // the same ticks every run and a script can click through them.
    return trace_mode_ ? engine_now() : std::chrono::steady_clock::now();
}

void Game::set_trace_replay_flags(const std::string& spec)
{
    // The reference's TH2REF_REPLAY_FLAGS: AVG_SetReplayFlag for each, which
    // is what the replay menu's items are unlocked by.
    std::istringstream list(spec);
    std::string item;
    while (std::getline(list, item, ',')) {
        if (item.empty()) {
            continue;
        }
        const int flag = std::stoi(item);
        if (flag < 0 || flag >= 32) {
            throw std::runtime_error(
                "--trace-replay-flags: no replay flag " + item);
        }
        unlocked_replays_.insert(flag);
        SDL_Log("trace: replay flag %d", flag);
    }
}

void Game::dispatch_port_screen_event(const SDL_Event& event)
{
    if (event.type == SDL_EVENT_MOUSE_MOTION) {
        port_pointer_x_ = event.motion.x;
        port_pointer_y_ = event.motion.y;
    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        port_pointer_x_ = event.button.x;
        port_pointer_y_ = event.button.y;
    }
    if (ui_mode_ == UiMode::title) {
        handle_title_input(event);
    } else if (ui_mode_ == UiMode::cg_gallery) {
        handle_cg_gallery_input(event);
    } else if (ui_mode_ == UiMode::music_room) {
        handle_music_room_input(event);
    } else if (ui_mode_ == UiMode::replay_gallery) {
        handle_replay_gallery_input(event);
    }
}

void Game::trace_title_input()
{
    // --trace-title: the script's hands on the port's own screens, as the
    // SDL events the player's would have been.  Before the tick, where live
    // play handles its events; the tick's sample then sees none of it, as a
    // press over one of these screens never reaches the sampler.
    trace_port_input_ = false;
    if (trace_title_ && name_input_open_) {
        // Nobody is at the name dialog: it goes on with the names it opened
        // on, as the reference's DialogBox does - built without its
        // resources, it returns at once.  Here, between ticks, where a
        // player's click on Start Game lands, and not inside the tick that
        // opened it: a scenario started mid-tick has its AVG_INIT frame
        // split across two.
        accept_name_input();
        return;
    }
    if (!trace_title_ || movie_ || name_input_open_
        || ui_mode_ == UiMode::game || ui_mode_ == UiMode::map
        || ui_mode_ == UiMode::save || ui_mode_ == UiMode::load) {
        return;
    }
    trace_port_input_ = true;
    const auto state = trace_script_.at(trace_tick_);
    const auto x = static_cast<float>(state.mouse_x);
    const auto y = static_cast<float>(state.mouse_y);
    if (state.mouse_x != trace_title_mouse_x_
        || state.mouse_y != trace_title_mouse_y_) {
        trace_title_mouse_x_ = state.mouse_x;
        trace_title_mouse_y_ = state.mouse_y;
        SDL_Event motion{};
        motion.type = SDL_EVENT_MOUSE_MOTION;
        motion.motion.x = x;
        motion.motion.y = y;
        dispatch_port_screen_event(motion);
    }
    // The buttons are edges of their own in the script's state, the keys
    // names in its pressed list.
    std::vector<std::string> pressed;
    if (state.click) pressed.emplace_back("lclick");
    if (state.cancel) pressed.emplace_back("rclick");
    pressed.insert(pressed.end(), state.pressed.begin(), state.pressed.end());
    for (const auto& name : pressed) {
        SDL_Event event{};
        if (name == "lclick" || name == "rclick") {
            event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
            event.button.button =
                name == "lclick" ? SDL_BUTTON_LEFT : SDL_BUTTON_RIGHT;
            event.button.down = true;
            event.button.x = x;
            event.button.y = y;
        } else {
            SDL_Keycode key = SDLK_UNKNOWN;
            if (name == "enter") key = SDLK_RETURN;
            else if (name == "esc") key = SDLK_ESCAPE;
            else if (name == "up") key = SDLK_UP;
            else if (name == "down") key = SDLK_DOWN;
            else if (name == "left") key = SDLK_LEFT;
            else if (name == "right") key = SDLK_RIGHT;
            if (key == SDLK_UNKNOWN) {
                continue;
            }
            event.type = SDL_EVENT_KEY_DOWN;
            event.key.key = key;
            event.key.down = true;
        }
        dispatch_port_screen_event(event);
        // A click that started the scenario is the last thing this screen
        // takes; the rest of the tick's keys belong to nobody.
        if (ui_mode_ == UiMode::game) {
            break;
        }
    }
}

}  // namespace th2app
