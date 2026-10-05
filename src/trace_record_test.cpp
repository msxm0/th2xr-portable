// TraceRecorder round trip: whatever the game was handed while recording has
// to be exactly what TraceScript::at hands it when the file is replayed - on
// every tick, for presses of any length, presses that start and end between
// two ticks, and a pointer that moves when it likes.  Plus the lead-in: a
// recording made on top of a script replays that script, clipped, before
// the hand-over.

#include "trace.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace {

bool same(th2app::TraceInputState a, th2app::TraceInputState b)
{
    std::ranges::sort(a.held);
    std::ranges::sort(a.pressed);
    std::ranges::sort(b.held);
    std::ranges::sort(b.pressed);
    return a.click == b.click && a.cancel == b.cancel
        && a.click_held == b.click_held && a.mouse_x == b.mouse_x
        && a.mouse_y == b.mouse_y && a.held == b.held
        && a.pressed == b.pressed && a.map_pick == b.map_pick;
}

int fail(const char* what, std::uint64_t tick)
{
    std::fprintf(stderr, "trace-record: %s differs at tick %llu\n", what,
                 static_cast<unsigned long long>(tick));
    return 1;
}

// Drives a recorder with random device activity for `ticks` ticks from
// `first`, returning what it handed out each tick.
std::vector<th2app::TraceInputState> play(
    th2app::TraceRecorder& recorder, std::uint64_t first, std::uint64_t ticks,
    unsigned seed)
{
    static const char* names[] = {"lclick", "rclick", "enter", "ctrl",
                                  "space", "num1", "esc"};
    std::mt19937 random(seed);
    std::vector<std::string> down;
    std::vector<th2app::TraceInputState> handed;
    for (std::uint64_t tick = first; tick < first + ticks; ++tick) {
        // Zero to three device changes between this sample and the last.
        const int changes = static_cast<int>(random() % 4);
        for (int i = 0; i < changes; ++i) {
            switch (random() % 3) {
            case 0:
                recorder.pointer(static_cast<int>(random() % 900) - 50,
                                 static_cast<int>(random() % 700) - 50);
                break;
            default: {
                const std::string name = names[random() % 7];
                const bool is_down = std::ranges::find(down, name) != down.end();
                recorder.key(name, !is_down);
                if (is_down) {
                    std::erase(down, name);
                } else {
                    down.push_back(name);
                }
                break;
            }
            }
        }
        handed.push_back(recorder.sample(tick));
    }
    recorder.finish(first + ticks);
    return handed;
}

}  // namespace

int main()
{
    const auto dir = std::filesystem::temp_directory_path()
        / "th2-trace-record-test";
    std::filesystem::create_directories(dir);

    // 1. Plain recording.
    {
        const auto path = dir / "plain.txt";
        th2app::TraceRecorder recorder;
        recorder.begin(path, nullptr, 0);
        const auto handed = play(recorder, 1, 6000, 12345);
        th2app::TraceScript script;
        script.load(path);
        for (std::uint64_t i = 0; i < handed.size(); ++i) {
            if (!same(handed[i], script.at(i + 1))) {
                return fail("plain recording", i + 1);
            }
        }
        // No mouse button event may start on the tick the previous one of
        // the same button ends: the reference sees the level, not the lines,
        // and would merge the two into one press.
        for (const auto kind : {th2app::TraceEvent::Kind::lclick,
                                th2app::TraceEvent::Kind::rclick}) {
            std::vector<std::pair<std::uint64_t, std::uint64_t>> spans;
            for (const auto& event : script.events()) {
                if (event.kind == kind) {
                    spans.emplace_back(event.tick, event.tick + event.hold);
                }
            }
            std::ranges::sort(spans);
            for (std::size_t i = 1; i < spans.size(); ++i) {
                if (spans[i].first <= spans[i - 1].second) {
                    return fail("button gap", spans[i].first);
                }
            }
        }
    }

    // 2. On top of a lead-in: repeats cut off by `until`, a one-shot that
    // straddles the hand-over clipped to it, a move carried into it.
    {
        const auto base_path = dir / "base.txt";
        {
            std::ofstream base(base_path);
            base << "1 move 400 403\n"
                    "30 every 30 lclick 3\n"
                    "30 every 1 num1\n"
                    "990 ctrl 40\n"
                    "5000 enter 5\n";
        }
        th2app::TraceScript base;
        base.load(base_path);
        const std::uint64_t from = 1000;
        const auto path = dir / "leadin.txt";
        th2app::TraceRecorder recorder;
        recorder.begin(path, &base, from);
        const auto handed = play(recorder, from, 3000, 777);
        th2app::TraceScript script;
        script.load(path);
        for (std::uint64_t tick = 0; tick < from; ++tick) {
            if (!same(base.at(tick), script.at(tick))) {
                return fail("lead-in", tick);
            }
        }
        for (std::uint64_t i = 0; i < handed.size(); ++i) {
            if (!same(handed[i], script.at(from + i))) {
                return fail("recording after lead-in", from + i);
            }
        }
    }

    // 3. The format round-trips through format_trace_event.
    {
        th2app::TraceEvent event;
        event.tick = 30;
        event.kind = th2app::TraceEvent::Kind::lclick;
        event.hold = 3;
        event.period = 30;
        event.until = 900;
        if (th2app::format_trace_event(event) != "30 every 30 lclick 3 until 900") {
            std::fprintf(stderr, "trace-record: format %s\n",
                         th2app::format_trace_event(event).c_str());
            return 1;
        }
    }

    // 4. Normal play is a recording nobody keeps: the live sampler hands the
    //    engine exactly what a recording sampler does for the same hands.
    {
        th2app::TraceRecorder recorded;
        recorded.begin(dir / "live-twin.txt", nullptr, 0);
        th2app::TraceRecorder live;
        live.begin_live();
        const auto a = play(recorded, 0, 2000, 99);
        const auto b = play(live, 0, 2000, 99);
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (!same(a[i], b[i])) {
                return fail("live sampler against a recording", i);
            }
        }
    }

    // 5. A tap: press, release and the pointer moved off the screen, all
    //    before one sample.  The click is where the finger was; the move
    //    takes effect on the tick after.
    {
        th2app::TraceRecorder live;
        live.begin_live();
        live.pointer(300, 120);
        live.sample(0);
        live.key("lclick", true);
        live.key("lclick", false);
        live.pointer(0, 0);
        const auto tap = live.sample(1);
        if (!tap.click || tap.mouse_x != 300 || tap.mouse_y != 120) {
            return fail("tap position", 1);
        }
        const auto after = live.sample(2);
        if (after.click || after.mouse_x != 0 || after.mouse_y != 0) {
            return fail("pointer after a tap", 2);
        }
    }

    std::filesystem::remove_all(dir);
    return 0;
}
