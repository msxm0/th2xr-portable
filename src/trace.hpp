#pragma once

// Trace mode: run the engine as a pure function of a tick counter and an
// input script, and write out what it draws.
//
// The reference build does exactly the same thing (reference/shim), reading
// the same script format and writing the same frame format, so the two can be
// compared tick by tick.  The point of both is that a tick is a unit of
// engine progress rather than of real time: a tick taking three milliseconds
// here and four hundred under Wine changes nothing about what is computed,
// because neither side is allowed to look at a clock.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace th2app {

// One line of an input script.  The format is the reference's:
//
//     150 move 400 403     pointer to (400,403) in 800x600 client coordinates
//     200 lclick 3         left button held three ticks
//     320 enter            a single-tick key press
//     420 ctrl 90          a key held ninety ticks
//     30 every 30 lclick 3 that click, repeated every thirty ticks forever
//
//     30 every 30 lclick 3 until 9000   ...but only on ticks before 9000
//
// `every` exists because a route is not a window.  Clicking through one takes
// hundreds of thousands of ticks, and spelling that out one line per click
// costs the reference shim a fixed-size table it silently overflows and both
// sides a scan of every event on every tick - quadratic in the length of the
// run, on the one run long enough to care.  A repeat is one event whose
// firing is arithmetic.
struct TraceEvent {
    enum class Kind { key, move, lclick, rclick, mappick };
    std::uint64_t tick = 0;
    Kind kind = Kind::key;
    std::string key;          // for Kind::key
    int x = 0, y = 0;         // for Kind::move
    std::uint64_t hold = 1;
    // 0 is a one-shot.  Otherwise the event repeats every `period` ticks from
    // `tick` onwards, holding for `hold` out of every `period`.
    std::uint64_t period = 0;
    // A repeat stops being in effect on this tick (0: never).  What a
    // recording made on top of a scripted lead-in needs: the lead-in's
    // clicking has to end where the player's hands take over.
    std::uint64_t until = 0;
    // For Kind::mappick: characters to steer to, in order of preference
    // ("mappick 8,9").  Empty is the plain first-destination pick.
    std::vector<int> prefer;
};

// What the script says is true on a given tick.
struct TraceInputState {
    bool click = false;       // left button edge
    bool cancel = false;      // right button edge
    bool click_held = false;
    int mouse_x = 0, mouse_y = 0;
    // Keys held this tick, by the reference's names: enter, space, esc, bs,
    // ctrl, shift, alt, up, down, left, right, pup, pdown, home, end, and
    // num0..num9 for the number row.
    std::vector<std::string> held;
    std::vector<std::string> pressed;   // only on the first tick of a hold

    // "mappick": stand on a map destination, whichever page it is on.  The
    // map is answered by the pointer being inside a destination's rect when a
    // click lands, and those rects are per scene, so the script asks for the
    // first selectable one rather than naming a coordinate.
    bool map_pick = false;
    std::vector<int> map_prefer;   // see TraceEvent::prefer

    bool is_pressed(std::string_view name) const;
    bool is_held(std::string_view name) const;
};

// "at <script>@<pc> <key>": hold a key while the script is parked on that
// instruction - an answer for one particular choice, on top of whatever the
// repeats press everywhere.  The engine takes the highest number key held,
// so "at 080306000@755 num2" overrides the route's "every 1 num1" there.
struct TraceRule {
    std::string script;       // lower case, no extension
    std::uint32_t pc = 0;
    std::string key;
};

class TraceScript {
public:
    // Throws if the file cannot be read or names a key the reference does not
    // know - a typo should fail the run, not silently do nothing.
    void load(const std::filesystem::path& path);
    TraceInputState at(std::uint64_t tick) const;
    std::size_t size() const { return events_.size(); }
    const std::vector<TraceEvent>& events() const { return events_; }
    const std::vector<TraceRule>& rules() const { return rules_; }

private:
    // Sorted and split once, so a tick costs a binary search and the events
    // actually in effect rather than a scan of every line.  A recorded run
    // is thousands of pointer moves, and scanning all of them on every tick
    // of a long replay is quadratic in its length.
    void index();
    std::vector<TraceEvent> events_;
    std::vector<TraceRule> rules_;
    std::vector<TraceEvent> moves_;      // by tick
    std::vector<TraceEvent> one_shots_;  // by tick
    std::vector<TraceEvent> repeats_;
    std::uint64_t longest_hold_ = 1;
};

// The script line an event is written as - the inverse of TraceScript::load.
std::string format_trace_event(const TraceEvent& event);

// Records live input as a script.  The game asks it, once a tick, what the
// player's hands are doing (sample), and plays exactly that - so replaying
// the file it writes gives the engine the same input on the same ticks.
//
// Per tick, a key or button is "pressed" if it went down since the last
// tick and "held" if it is down now or went down since - the same two things
// TraceScript::at reports for a "<tick> <key> <hold>" line, which is what a
// press becomes once it is released.  The pointer is written as a move on
// every tick it has changed since the last one.
class TraceRecorder {
public:
    // Opens `path` and writes the events of `base` that fall before `from`,
    // clipped there: the scripted lead-in the recording is made on top of.
    // `from` 0 means no lead-in.
    void begin(const std::filesystem::path& path, const TraceScript* base,
               std::uint64_t from);
    // Live play: the same sampling with nothing written.  Normal play is a
    // recording nobody keeps - the engine gets the player's hands through
    // sample() exactly as a recorded run does, so what a recording verifies
    // against the reference is what everyone plays.
    void begin_live() { live_ = true; }
    bool active() const { return live_ || file_ != nullptr; }
    bool recording() const { return file_ != nullptr; }

    // Device changes between samples, in the 800x600 game space.
    void pointer(int x, int y);
    void key(std::string_view name, bool down);

    TraceInputState sample(std::uint64_t tick);
    // The pointer as the device events have left it, without sampling.
    int x() const { return x_; }
    int y() const { return y_; }
    // Closes whatever is still held and the file.
    void finish(std::uint64_t tick);
    ~TraceRecorder();

private:
    void write(const TraceEvent& event);
    struct Held {
        std::string name;
        std::uint64_t since = 0;
    };
    std::FILE* file_ = nullptr;
    bool live_ = false;
    int x_ = 0, y_ = 0;
    // A pointer move that came after a button went down, held until that
    // press has been sampled.  A tap arrives as press, release and a move
    // off the screen all at once, and without this the tick that sees the
    // click would see it land wherever the finger was moved away to.
    bool pointer_deferred_ = false;
    int deferred_x_ = 0, deferred_y_ = 0;
    bool pointer_written_ = false;
    int written_x_ = 0, written_y_ = 0;
    std::vector<std::string> down_;      // physically down now
    std::vector<std::string> went_down_; // pressed since the last sample
    std::vector<Held> open_;             // recorded presses not yet released
    std::uint64_t events_ = 0;
};

// Writes one frame in the reference's format: a text header line
// "TH2REFZ <w> <h> 24 <key> <bytes>\n" followed by a deflate stream of
// either the picture (key) or its per-byte difference from the previous
// tick's (not key).  The difference is mod 256 and exactly reversible - this
// is a storage format, not a codec, and nothing about a pixel is
// approximated, because the comparison downstream tolerates two levels of
// difference and must never be handed one it created itself.
//
// Raw, a frame is 800*600*3 = 1.37MB, and that is what used to cap a
// comparison window at 300 ticks and force the harness to replay the whole
// game to reach each one.  Successive frames differ in the glyphs the
// typewriter has just drawn, so the difference deflates about eight to one
// and a twelve thousand tick window fits in about 2GB a side.
//
// A key frame is written when there is no predecessor to subtract - the
// first tick of a window, or after a gap - and every 120 ticks after that,
// so reading a single tick costs at most that many decodes.  The caller
// passes the tick for exactly that reason; passing them out of order or with
// gaps is safe, it only costs key frames.
bool write_trace_frame(
    const std::filesystem::path& path, std::uint64_t tick,
    const std::uint8_t* bgr, int width, int height);

}  // namespace th2app
