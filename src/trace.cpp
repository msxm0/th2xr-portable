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
        // "... until <tick>" ends a repeat; taken off the line first so the
        // optional hold before it still parses as the last number.
        std::uint64_t until = 0;
        {
            std::istringstream words(line);
            std::vector<std::string> kept;
            std::string word;
            while (words >> word) {
                if (lower(word) == "until") {
                    if (!(words >> until) || until == 0) {
                        throw std::runtime_error(
                            path.string() + ":" + std::to_string(number)
                            + ": until needs a tick");
                    }
                    continue;
                }
                kept.push_back(word);
            }
            line.clear();
            for (const auto& w : kept) {
                line += w;
                line += ' ';
            }
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
        event.until = until;
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
        if (event.until && !event.period) {
            throw std::runtime_error(
                path.string() + ":" + std::to_string(number)
                + ": until only ends a repeat");
        }
        events_.push_back(std::move(event));
    }
    index();
}

void TraceScript::index()
{
    moves_.clear();
    one_shots_.clear();
    repeats_.clear();
    longest_hold_ = 1;
    for (const auto& event : events_) {
        if (event.kind == TraceEvent::Kind::move) {
            moves_.push_back(event);
        } else if (event.period) {
            repeats_.push_back(event);
        } else {
            one_shots_.push_back(event);
            longest_hold_ = std::max(longest_hold_, event.hold);
        }
    }
    const auto by_tick = [](const TraceEvent& a, const TraceEvent& b) {
        return a.tick < b.tick;
    };
    // Stable, so two moves on one tick keep their order in the file and the
    // later line wins, as it always has.
    std::ranges::stable_sort(moves_, by_tick);
    std::ranges::stable_sort(one_shots_, by_tick);
}

TraceInputState TraceScript::at(std::uint64_t tick) const
{
    TraceInputState state;
    const auto add = [&state](const TraceEvent& event, bool first) {
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
    };
    const auto after = [](std::uint64_t t, const TraceEvent& event) {
        return t < event.tick;
    };

    // A move is a state change, so the latest one at or before the tick
    // holds.  Ties go to the later line.
    if (const auto it = std::ranges::upper_bound(
            moves_, tick, {}, &TraceEvent::tick);
        it != moves_.begin()) {
        state.mouse_x = std::prev(it)->x;
        state.mouse_y = std::prev(it)->y;
    }

    // One-shots in effect started within the longest hold of this tick.
    const std::uint64_t earliest =
        tick + 1 > longest_hold_ ? tick + 1 - longest_hold_ : 0;
    auto first = std::ranges::lower_bound(
        one_shots_, earliest, {}, &TraceEvent::tick);
    const auto last = std::upper_bound(
        first, one_shots_.end(), tick, after);
    for (auto it = first; it != last; ++it) {
        if (tick - it->tick < it->hold) {
            add(*it, tick == it->tick);
        }
    }

    // Repeats: the phase within a period, from the first tick until `until`.
    for (const auto& event : repeats_) {
        if (tick < event.tick || (event.until && tick >= event.until)) {
            continue;
        }
        const std::uint64_t phase = (tick - event.tick) % event.period;
        if (phase < event.hold) {
            add(event, phase == 0);
        }
    }
    return state;
}

std::string format_trace_event(const TraceEvent& event)
{
    std::string what;
    switch (event.kind) {
    case TraceEvent::Kind::move:
        return std::to_string(event.tick) + " move " + std::to_string(event.x)
            + " " + std::to_string(event.y);
    case TraceEvent::Kind::lclick: what = "lclick"; break;
    case TraceEvent::Kind::rclick: what = "rclick"; break;
    case TraceEvent::Kind::mappick: what = "mappick"; break;
    case TraceEvent::Kind::key: what = event.key; break;
    }
    std::string line = std::to_string(event.tick);
    if (event.period) {
        line += " every " + std::to_string(event.period);
    }
    line += " " + what;
    if (event.kind != TraceEvent::Kind::mappick) {
        line += " " + std::to_string(event.hold);
    }
    if (event.until) {
        line += " until " + std::to_string(event.until);
    }
    return line;
}

void TraceRecorder::begin(const std::filesystem::path& path,
                          const TraceScript* base, std::uint64_t from)
{
    file_ = std::fopen(path.string().c_str(), "w");
    if (!file_) {
        throw std::runtime_error("cannot write recording: " + path.string());
    }
    std::fprintf(file_,
                 "# Recorded input, scenario ticks (the port's trace ticks).\n"
                 "# Turn into a comparison pair with\n"
                 "#     reference/scripts/make_pair.py <name> --recording %s\n",
                 path.filename().string().c_str());
    if (!base || from == 0) {
        std::fflush(file_);
        return;
    }
    std::fprintf(file_, "# Lead-in: the base script up to tick %llu.\n",
                 static_cast<unsigned long long>(from));
    // The pointer carries on from where the lead-in left it until the
    // player moves it - SDL reports no position before the first motion,
    // and jumping to the corner would be input nobody gave.
    const auto handed_over = base->at(from - 1);
    x_ = handed_over.mouse_x;
    y_ = handed_over.mouse_y;
    for (auto event : base->events()) {
        if (event.tick >= from) {
            continue;
        }
        if (event.period) {
            event.until = event.until ? std::min(event.until, from) : from;
        } else if (event.kind != TraceEvent::Kind::move) {
            event.hold = std::min(event.hold, from - event.tick);
        }
        write(event);
    }
    std::fprintf(file_, "# Live from tick %llu.\n",
                 static_cast<unsigned long long>(from));
    std::fflush(file_);
}

void TraceRecorder::pointer(int x, int y)
{
    x_ = std::clamp(x, 0, 799);
    y_ = std::clamp(y, 0, 599);
}

void TraceRecorder::key(std::string_view name, bool down)
{
    const auto in = [](const std::vector<std::string>& list,
                       std::string_view n) {
        return std::ranges::find(list, n) != list.end();
    };
    if (down) {
        if (!in(down_, name)) {
            down_.emplace_back(name);
            if (!in(went_down_, name)) {
                went_down_.emplace_back(name);
            }
        }
    } else {
        std::erase(down_, std::string(name));
    }
}

TraceInputState TraceRecorder::sample(std::uint64_t tick)
{
    const auto in = [](const std::vector<std::string>& list,
                       std::string_view n) {
        return std::ranges::find(list, n) != list.end();
    };
    if (!pointer_written_ || x_ != written_x_ || y_ != written_y_) {
        TraceEvent move;
        move.kind = TraceEvent::Kind::move;
        move.tick = tick;
        move.x = x_;
        move.y = y_;
        write(move);
        pointer_written_ = true;
        written_x_ = x_;
        written_y_ = y_;
    }
    const auto close = [&](const Held& held) {
        TraceEvent event;
        event.tick = held.since;
        event.hold = std::max<std::uint64_t>(1, tick - held.since);
        if (held.name == "lclick") {
            event.kind = TraceEvent::Kind::lclick;
        } else if (held.name == "rclick") {
            event.kind = TraceEvent::Kind::rclick;
        } else {
            event.kind = TraceEvent::Kind::key;
            event.key = held.name;
        }
        write(event);
    };
    // A mouse button pressed again before a tick saw it up is held back a
    // tick.  The reference derives a click from the button's *level* -
    // MUS_RenewMouse compares it with the last frame's - so two presses
    // back to back in the script are one long press over there, and the
    // second click would exist only on our side.  Keys carry their edge
    // explicitly (th2ref_input sets trg on the first tick) and need no gap.
    const auto button = [](std::string_view n) {
        return n == "lclick" || n == "rclick";
    };
    std::vector<std::string> deferred;
    for (auto it = went_down_.begin(); it != went_down_.end();) {
        const bool open = std::ranges::find(open_, *it, &Held::name)
            != open_.end();
        if (open && button(*it)) {
            deferred.push_back(*it);
        }
        ++it;
    }
    // Released since the last tick, or pressed again: the recorded press
    // ends here.  A new press opens on this tick.
    for (auto it = open_.begin(); it != open_.end();) {
        if (!in(down_, it->name) || in(went_down_, it->name)) {
            close(*it);
            it = open_.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto& name : deferred) {
        std::erase(went_down_, name);
    }
    for (const auto& name : went_down_) {
        open_.push_back({name, tick});
    }

    TraceInputState state;
    state.mouse_x = x_;
    state.mouse_y = y_;
    for (const auto& name : went_down_) {
        if (name == "lclick") {
            state.click = true;
        } else if (name == "rclick") {
            state.cancel = true;
        } else {
            state.pressed.push_back(name);
        }
    }
    for (const auto& held : open_) {
        if (held.name == "lclick") {
            state.click_held = true;
        } else if (held.name != "rclick") {
            state.held.push_back(held.name);
        }
    }
    went_down_ = std::move(deferred);
    return state;
}

void TraceRecorder::write(const TraceEvent& event)
{
    if (!file_) {
        return;
    }
    std::fprintf(file_, "%s\n", format_trace_event(event).c_str());
    std::fflush(file_);
    ++events_;
}

void TraceRecorder::finish(std::uint64_t tick)
{
    if (!file_) {
        return;
    }
    for (const auto& held : open_) {
        TraceEvent event;
        event.tick = held.since;
        event.hold = std::max<std::uint64_t>(1, tick - held.since);
        if (held.name == "lclick") {
            event.kind = TraceEvent::Kind::lclick;
        } else if (held.name == "rclick") {
            event.kind = TraceEvent::Kind::rclick;
        } else {
            event.kind = TraceEvent::Kind::key;
            event.key = held.name;
        }
        write(event);
    }
    open_.clear();
    std::fprintf(file_, "# End of recording at tick %llu.\n",
                 static_cast<unsigned long long>(tick));
    std::fclose(file_);
    file_ = nullptr;
}

TraceRecorder::~TraceRecorder()
{
    if (file_) {
        std::fclose(file_);
    }
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
