#include "touch_input.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ranges>

namespace th2 {
namespace {

constexpr float SwipeDistance = 0.12f;       // fraction of screen dimension
constexpr float SwipeMaxDurationMs = 500.0f;
constexpr float SwipeMinRatio = 1.5f;        // dominant axis must exceed other
constexpr float TapMaxMovement = 0.03f;      // fraction of screen dimension
constexpr float TapMaxDurationMs = 250.0f;
constexpr float TwoFingerMaxDelayMs = 150.0f;
constexpr float BacklogLineThreshold = 0.1f; // fraction of screen height per line
constexpr float SkipHoldDelayMs = 150.0f;    // before a right swipe becomes hold

float distance(float ax, float ay, float bx, float by)
{
    const float dx = ax - bx;
    const float dy = ay - by;
    return std::sqrt(dx * dx + dy * dy);
}

float elapsed_ms(std::chrono::steady_clock::time_point start,
                 std::chrono::steady_clock::time_point end)
{
    return std::chrono::duration<float, std::milli>(end - start).count();
}

}  // namespace

TouchInput::TouchInput() = default;

int TouchInput::find_finger(SDL_FingerID id) const
{
    for (std::size_t i = 0; i < fingers_.size(); ++i) {
        if (fingers_[i].active && fingers_[i].id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int TouchInput::add_finger(SDL_FingerID id, float x, float y)
{
    for (std::size_t i = 0; i < fingers_.size(); ++i) {
        if (!fingers_[i].active) {
            fingers_[i].id = id;
            fingers_[i].start_x = x;
            fingers_[i].start_y = y;
            fingers_[i].x = x;
            fingers_[i].y = y;
            fingers_[i].start_time = std::chrono::steady_clock::now();
            fingers_[i].travel = 0.0f;
            fingers_[i].active = true;
            if (scroll_anchor_y_ < 0.0f) {
                scroll_anchor_y_ = y;
            }
            return static_cast<int>(i);
        }
    }
    return -1;
}

void TouchInput::remove_finger(int index)
{
    Finger& finger = fingers_[index];
    finger.active = false;
    finger.up_time = std::chrono::steady_clock::now();
    scroll_anchor_y_ = -1.0f;

    const bool had_scroll = emitted_scroll_;
    emitted_scroll_ = false;
    last_gesture_ = false;
    last_moved_ = std::max(
        finger.travel,
        distance(finger.start_x, finger.start_y, finger.x, finger.y))
        >= TapMaxMovement;

    const int other = 1 - index;
    if (fingers_[other].active) {
        // Wait for the second finger to lift before deciding if this was a
        // two-finger tap.  Whatever it turns out to be, it is a gesture and
        // not a click.
        last_gesture_ = true;
        return;
    }

    // If we already emitted scroll events during this drag, don't also treat
    // the lift as a swipe.
    if (had_scroll) {
        active_gesture_ = false;
        last_gesture_ = true;
        return;
    }

    // Rightward swipe: quick release toggles skip mode; holding for longer
    // activates an unconditional skip while the finger is down.
    if (horizontal_state_ != HorizontalState::none) {
        if (horizontal_state_ == HorizontalState::right && !skip_held_) {
            pending_ = TouchAction::SkipToggle;
        } else if (horizontal_state_ == HorizontalState::left) {
            pending_ = TouchAction::AutoModeToggle;
        }
        skip_held_ = false;
        horizontal_state_ = HorizontalState::none;
        active_gesture_ = false;
        last_gesture_ = true;
        return;
    }

    // If both fingers lifted close together and both moved very little,
    // treat it as a two-finger tap.
    if (fingers_[other].up_time > finger.start_time
        && elapsed_ms(fingers_[other].up_time, finger.up_time)
               < TwoFingerMaxDelayMs) {
        const float movement = std::max(
            distance(finger.start_x, finger.start_y, finger.x, finger.y),
            distance(
                fingers_[other].start_x, fingers_[other].start_y,
                fingers_[other].x, fingers_[other].y));
        const float duration = std::max(
            elapsed_ms(finger.start_time, finger.up_time),
            elapsed_ms(fingers_[other].start_time, fingers_[other].up_time));
        if (movement < TapMaxMovement && duration < TapMaxDurationMs) {
            pending_ = TouchAction::BacklogOrHideTextbox;
            active_gesture_ = false;
            last_gesture_ = true;
            return;
        }
    }

    evaluate_single_swipe(finger);
    if (pending_ != TouchAction::None) {
        last_gesture_ = true;
    }

    // If it wasn't a swipe, treat a short, still lift as a tap.
    if (pending_ == TouchAction::None) {
        const auto now = std::chrono::steady_clock::now();
        const float duration = elapsed_ms(finger.start_time, now);
        const float movement =
            distance(finger.start_x, finger.start_y, finger.x, finger.y);
        if (movement < TapMaxMovement && duration < TapMaxDurationMs) {
            pending_ = TouchAction::Tap;
            tap_x_ = finger.x;
            tap_y_ = finger.y;
            active_gesture_ = false;
        }
    }
}

void TouchInput::maybe_emit_scroll(const Finger& finger)
{
    if (scroll_anchor_y_ < 0.0f) {
        return;
    }
    const int other = 1 - find_finger(finger.id);
    if (other >= 0 && fingers_[other].active) {
        return;
    }

    const float delta = finger.y - scroll_anchor_y_;
    if (delta >= BacklogLineThreshold) {
        pending_ = TouchAction::BacklogOlder;
        scroll_anchor_y_ += BacklogLineThreshold;
        emitted_scroll_ = true;
    } else if (delta <= -BacklogLineThreshold) {
        pending_ = TouchAction::BacklogNewer;
        scroll_anchor_y_ -= BacklogLineThreshold;
        emitted_scroll_ = true;
    }
}

void TouchInput::evaluate_single_swipe(const Finger& finger)
{
    const auto now = std::chrono::steady_clock::now();
    const float duration = elapsed_ms(finger.start_time, now);
    if (duration > SwipeMaxDurationMs) {
        active_gesture_ = false;
        return;
    }

    const float dx = finger.x - finger.start_x;
    const float dy = finger.y - finger.start_y;
    if (std::abs(dy) < SwipeDistance) {
        active_gesture_ = false;
        return;
    }
    if (std::abs(dy) < SwipeMinRatio * std::abs(dx)) {
        active_gesture_ = false;
        return;
    }

    active_gesture_ = false;
    if (dy > 0.0f) {
        pending_ = TouchAction::BacklogOlder;
    } else {
        pending_ = TouchAction::BacklogNewer;
    }
}

void TouchInput::evaluate_horizontal_swipe(const Finger& finger)
{
    const float dx = finger.x - finger.start_x;
    const float dy = finger.y - finger.start_y;

    if (horizontal_state_ == HorizontalState::none) {
        if (emitted_scroll_ || std::abs(dx) <= SwipeDistance) {
            return;
        }
        if (std::abs(dx) < SwipeMinRatio * std::abs(dy)) {
            return;
        }
        if (dx > 0.0f) {
            horizontal_state_ = HorizontalState::right;
            horizontal_cross_time_ = std::chrono::steady_clock::now();
        } else {
            horizontal_state_ = HorizontalState::left;
        }
        return;
    }

    if (horizontal_state_ == HorizontalState::right) {
        if (!skip_held_) {
            const auto now = std::chrono::steady_clock::now();
            if (elapsed_ms(horizontal_cross_time_, now) >= SkipHoldDelayMs) {
                skip_held_ = true;
            }
        }
        if (dx <= -SwipeDistance) {
            // Pulled back to the left: stop the held skip.
            horizontal_state_ = HorizontalState::cancelled;
            skip_held_ = false;
        }
    }
}

void TouchInput::process_event(const SDL_Event& event)
{
    switch (event.type) {
    case SDL_EVENT_FINGER_DOWN: {
        const int index = add_finger(
            event.tfinger.fingerID,
            event.tfinger.x, event.tfinger.y);
        if (index >= 0) {
            active_gesture_ = true;
        }
        break;
    }
    case SDL_EVENT_FINGER_MOTION: {
        const int index = find_finger(event.tfinger.fingerID);
        if (index >= 0) {
            fingers_[index].x = event.tfinger.x;
            fingers_[index].y = event.tfinger.y;
            fingers_[index].travel = std::max(
                fingers_[index].travel,
                distance(
                    fingers_[index].start_x, fingers_[index].start_y,
                    fingers_[index].x, fingers_[index].y));
            maybe_emit_scroll(fingers_[index]);
            evaluate_horizontal_swipe(fingers_[index]);
        }
        break;
    }
    case SDL_EVENT_FINGER_UP: {
        const int index = find_finger(event.tfinger.fingerID);
        if (index >= 0) {
            fingers_[index].x = event.tfinger.x;
            fingers_[index].y = event.tfinger.y;
            remove_finger(index);
        }
        break;
    }
    case SDL_EVENT_FINGER_CANCELED: {
        // The browser or the OS took the touch over - an edge-swipe back
        // gesture, a fullscreen change, a notification shade.  There is no
        // lift to interpret, so drop the gesture rather than leave a finger
        // marked active for good.
        const int index = find_finger(event.tfinger.fingerID);
        if (index >= 0) {
            fingers_[index].active = false;
            fingers_[index].up_time = std::chrono::steady_clock::now();
        }
        const bool still_down = std::ranges::any_of(
            fingers_, [](const Finger& finger) { return finger.active; });
        if (!still_down) {
            reset();
        }
        break;
    }
    case SDL_EVENT_KEY_DOWN:
        if (event.key.key == SDLK_AC_BACK) {
            pending_ = TouchAction::MenuToggle;
        }
        break;
    default:
        break;
    }
}

void TouchInput::claim_touch()
{
    pending_ = TouchAction::None;
    emitted_scroll_ = false;
    // No anchor means maybe_emit_scroll() stops stepping the backlog, and a
    // cancelled horizontal state means the lift is neither a swipe nor a tap.
    scroll_anchor_y_ = -1.0f;
    horizontal_state_ = HorizontalState::cancelled;
    skip_held_ = false;
    active_gesture_ = false;
}

void TouchInput::reset()
{
    fingers_ = {};
    pending_ = TouchAction::None;
    last_gesture_ = false;
    last_moved_ = false;
    active_gesture_ = false;
    emitted_scroll_ = false;
    scroll_anchor_y_ = -1.0f;
    horizontal_state_ = HorizontalState::none;
    skip_held_ = false;
}

TouchAction TouchInput::poll_action()
{
    const auto action = pending_;
    pending_ = TouchAction::None;
    return action;
}

bool TouchInput::active_gesture() const
{
    return active_gesture_;
}

}  // namespace th2
