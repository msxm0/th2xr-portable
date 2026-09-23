#include "trace.hpp"

#include <zlib.h>

#include <cstring>

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace th2app {
namespace {

// The key names th2ref_input.cpp accepts.  Kept in step deliberately: a
// script that runs on one side has to run on the other, and a name only one
// of them knows would diverge silently.
constexpr std::array<std::string_view, 25> known_keys{
    "enter", "space", "esc", "bs", "ctrl", "shift", "alt",
    "up", "down", "left", "right", "pup", "pdown", "home", "end",
    // The number row.  GameKey.num[] in the original, and not a convenience:
    // AVG_ControlSelectWindow reads it to pick a choice, which is the only
    // way to answer one without knowing where its text landed on screen.
    "num0", "num1", "num2", "num3", "num4",
    "num5", "num6", "num7", "num8", "num9",
};

std::string lower(std::string value)
{
    std::ranges::transform(value, value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

}  // namespace

bool TraceInputState::is_pressed(std::string_view name) const
{
    return std::ranges::find(pressed, name) != pressed.end();
}

bool TraceInputState::is_held(std::string_view name) const
{
    return std::ranges::find(held, name) != held.end();
}

void TraceScript::load(const std::filesystem::path& path)
{
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("cannot open input script: " + path.string());
    }
    events_.clear();
    std::string line;
    int number = 0;
    while (std::getline(file, line)) {
        ++number;
        if (const auto hash = line.find('#'); hash != std::string::npos) {
            line.erase(hash);
        }
        std::istringstream parts(line);
        std::uint64_t tick = 0;
        std::string what;
        if (!(parts >> tick >> what)) {
            continue;
        }
        what = lower(std::move(what));

        TraceEvent event;
        event.tick = tick;
        // "<first> every <period> <what> [hold]": the same event, forever.
        if (what == "every") {
            std::uint64_t period = 0;
            if (!(parts >> period) || period == 0 || !(parts >> what)) {
                throw std::runtime_error(
                    path.string() + ":" + std::to_string(number)
                    + ": every needs a non-zero period and an event");
            }
            event.period = period;
            what = lower(std::move(what));
            if (what == "every" || what == "move") {
                throw std::runtime_error(
                    path.string() + ":" + std::to_string(number)
                    + ": '" + what + "' cannot repeat");
            }
        }
        if (what == "mappick") {
            event.kind = TraceEvent::Kind::mappick;
            event.hold = 1;
            if (event.period == 0) {
                event.period = 1;
            }
            events_.push_back(std::move(event));
            continue;
        }
        if (what == "move") {
            if (!(parts >> event.x >> event.y)) {
                throw std::runtime_error(
                    path.string() + ":" + std::to_string(number)
                    + ": move needs an x and a y");
            }
            event.kind = TraceEvent::Kind::move;
        } else if (what == "lclick" || what == "rclick") {
            event.kind = what == "lclick"
                ? TraceEvent::Kind::lclick : TraceEvent::Kind::rclick;
            std::uint64_t hold = 0;
            event.hold = (parts >> hold) && hold > 0 ? hold : 2;
        } else {
            if (std::ranges::find(known_keys, what) == known_keys.end()) {
                throw std::runtime_error(
                    path.string() + ":" + std::to_string(number)
                    + ": unknown key '" + what + "'");
            }
            event.kind = TraceEvent::Kind::key;
            event.key = what;
            std::uint64_t hold = 0;
            event.hold = (parts >> hold) && hold > 0 ? hold : 1;
        }
        if (event.period && event.hold > event.period) {
            throw std::runtime_error(
                path.string() + ":" + std::to_string(number)
                + ": a repeat holds longer than its period");
        }
        events_.push_back(std::move(event));
    }
}

TraceInputState TraceScript::at(std::uint64_t tick) const
{
    TraceInputState state;
    for (const auto& event : events_) {
        // A move is a state change, so it holds from its tick onwards.
        if (event.kind == TraceEvent::Kind::move) {
            if (tick >= event.tick) {
                state.mouse_x = event.x;
                state.mouse_y = event.y;
            }
            continue;
        }
        if (tick < event.tick) {
            continue;
        }
        // Where this tick falls inside the event: for a one-shot that is just
        // the distance from its tick, for a repeat the phase within a period.
        const std::uint64_t phase = event.period
            ? (tick - event.tick) % event.period : tick - event.tick;
        if (phase >= event.hold) {
            continue;
        }
        const bool first = phase == 0;
        switch (event.kind) {
        case TraceEvent::Kind::lclick:
            state.click_held = true;
            if (first) state.click = true;
            break;
        case TraceEvent::Kind::rclick:
            if (first) state.cancel = true;
            break;
        case TraceEvent::Kind::key:
            state.held.push_back(event.key);
            if (first) state.pressed.push_back(event.key);
            break;
        case TraceEvent::Kind::mappick:
            state.map_pick = true;
            break;
        default:
            break;
        }
    }
    return state;
}

namespace {

// The frame the next difference is taken against, and the tick it came from.
// Static rather than threaded through the caller: there is exactly one trace
// stream per process, and the reference's dump keeps its predecessor the
// same way.
constexpr std::uint64_t trace_key_period = 120;
std::vector<std::uint8_t> trace_previous;
std::uint64_t trace_previous_tick = 0;
bool trace_have_previous = false;

}  // namespace

bool write_trace_frame(
    const std::filesystem::path& path, std::uint64_t tick,
    const std::uint8_t* bgr, int width, int height)
{
    const auto bytes = static_cast<std::size_t>(width) * height * 3;
    const bool key = !trace_have_previous
        || trace_previous.size() != bytes
        || trace_previous_tick + 1 != tick
        || tick % trace_key_period == 0;

    std::vector<std::uint8_t> payload(bytes);
    if (key) {
        std::memcpy(payload.data(), bgr, bytes);
    } else {
        for (std::size_t i = 0; i < bytes; ++i) {
            payload[i] = static_cast<std::uint8_t>(bgr[i] - trace_previous[i]);
        }
    }

    uLongf packed_size = compressBound(static_cast<uLong>(bytes));
    std::vector<std::uint8_t> packed(packed_size);
    // Level 1: the few percent level 6 would add is paid for on every frame
    // of a twelve thousand tick run, against a file that is already a
    // twentieth of the budget.
    if (compress2(packed.data(), &packed_size, payload.data(),
                  static_cast<uLong>(bytes), 1) != Z_OK) {
        return false;
    }

    std::FILE* file = std::fopen(path.string().c_str(), "wb");
    if (!file) {
        return false;
    }
    std::fprintf(file, "TH2REFZ %d %d 24 %d %zu\n", width, height,
                 key ? 1 : 0, bytes);
    const bool ok = std::fwrite(packed.data(), 1, packed_size, file)
        == packed_size;
    std::fclose(file);

    trace_previous.assign(bgr, bgr + bytes);
    trace_previous_tick = tick;
    trace_have_previous = true;
    return ok;
}

}  // namespace th2app
