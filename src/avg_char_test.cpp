// Checks on AVG_ControlChar's state machine, against GM_AvgChar.cpp.  These
// are the transitions that produced this session's bugs: a character marked
// baked while not being in the plate, a fade-out that never invalidated it,
// and the layer-then-slot order that decides who occludes whom.

#include "avg_char.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition) {
        std::printf("FAIL: %s\n", what.c_str());
        ++failures;
    }
}

// A rig that records the calls AVG_ControlChar makes outside itself, so the
// ordering can be asserted rather than eyeballed.
struct Rig {
    th2::Display display{nullptr};
    std::vector<std::string> calls;
    bool window_up = true;
    int loaded = 0;

    th2::AvgChar::Hooks hooks()
    {
        th2::AvgChar::Hooks h;
        h.load_char_bitmap = [this](int bmp_slot, int, int, int) {
            ++loaded;
            calls.push_back("load " + std::to_string(bmp_slot));
        };
        h.reset_half_tone = [this] { calls.push_back("reset_half_tone"); };
        h.novel_message_disp = [this](bool on) {
            calls.push_back(on ? "message_on" : "message_off");
        };
        h.copy_back = [this] { calls.push_back("copy_back"); };
        h.window_cond = [this] { return window_up; };
        h.close_window = [this] {
            window_up = false;
            calls.push_back("close_window");
        };
        h.open_window = [this] {
            window_up = true;
            calls.push_back("open_window");
        };
        h.eff_cnt = [](int frames) { return frames; };
        h.level = [] { return true; };
        h.ami = [] { return false; };
        h.back_flag = [] { return true; };
        h.back_scrolling = [] { return false; };
        h.back_zooming = [] { return false; };
        h.back_redraw_take = [] { return false; };
        h.back_pos = [](int* x, int* y) { *x = 0; *y = 0; };
        return h;
    }

    bool said(const std::string& what) const
    {
        for (const auto& call : calls) {
            if (call == what) {
                return true;
            }
        }
        return false;
    }
};

// GetCharIndex / GetSpaceIndex work on eight fixed slots, and a freed slot
// is reused - which is why the bake order is by slot and not by arrival.
void test_slots_are_reused()
{
    Rig rig;
    th2::AvgChar chars(rig.display, rig.hooks());
    chars.set_char(1, 0, 1, 0, th2::char_type_direct, 128, 256, 0);
    chars.set_char(2, 0, 1, 0, th2::char_type_direct, 128, 256, 0);
    check(chars.char_index(1) == 0, "first character takes slot 0");
    check(chars.char_index(2) == 1, "second takes slot 1");

    chars.release_char(0);
    chars.set_char(3, 0, 1, 0, th2::char_type_direct, 128, 256, 0);
    check(chars.char_index(3) == 0,
          "a freed slot is reused before a fresh one");
    check(chars.char_index(99) == th2::max_char,
          "an unknown character reports no slot");
}

// A call that changes nothing returns before AVG_ResetHalfTone.
void test_identical_call_changes_nothing()
{
    Rig rig;
    th2::AvgChar chars(rig.display, rig.hooks());
    chars.set_char(1, 0, 1, 0, th2::char_type_direct, 128, 256, 0);
    rig.calls.clear();
    chars.set_char(1, 0, 1, 0, th2::char_type_direct, 128, 256, 0);
    check(rig.calls.empty(),
          "an identical set_char touches neither the wash nor the message");

    chars.set_char(1, 5, 1, 0, th2::char_type_direct, 128, 256, 0);
    check(rig.said("reset_half_tone"),
          "a real change does tear the wash down");
}

// cut_mode 0 -> bake -> 1, and 1 means the graph stops drawing as a layer.
void test_bake_cycle()
{
    Rig rig;
    th2::AvgChar chars(rig.display, rig.hooks());
    chars.set_char(1, 0, 1, 0, th2::char_type_direct, 128, 256, 0);
    check(chars.state(0).cut_mode == 0, "a settled character starts unbaked");
    check(chars.state(0).cond == th2::char_cond_nomal, "and settled");

    chars.control_char();
    check(chars.state(0).cut_mode == 1, "one pass bakes it into the plate");
    // DSP_SetGraphTarget switches the graph off; DSP_DrawGraph turns it back
    // on at the end of the frame, and the next pass turns it off by cut_mode.
    rig.display.draw(nullptr);
    chars.control_char();
    check(!rig.display.graph_disp(th2::grp_char + 0),
          "a baked character stops being drawn as a layer");
}

// The invalidation that a fade-out needs: a character animating while still
// baked sends the whole plate back to the clean copy.
void test_animating_while_baked_invalidates_the_plate()
{
    Rig rig;
    th2::AvgChar chars(rig.display, rig.hooks());
    chars.set_char(1, 0, 1, 0, th2::char_type_direct, 128, 256, 0);
    chars.set_char(2, 0, 1, 0, th2::char_type_direct, 128, 256, 0);
    chars.control_char();
    check(chars.state(0).cut_mode == 1 && chars.state(1).cut_mode == 1,
          "both baked");

    rig.calls.clear();
    chars.reset_char(1, th2::char_type_cfade, 10);  // CR: fade her out
    check(chars.state(0).cond == th2::char_cond_out, "she is leaving");
    chars.control_char();
    check(rig.said("copy_back"),
          "animating while baked restores the clean plate");
    check(chars.state(0).cut_mode == 0 && chars.state(1).cut_mode == 0,
          "and resets every cut_mode, not just hers");
}

// Anyone ordered above an animating character stays a layer, so the plate
// cannot draw over the one that is moving.  CharStruct::disp is a per-frame
// flag: the occlusion loop sets it and the layer loop below consumes it, so
// what is observable afterwards is whether the character got baked.
void test_occlusion_keeps_upper_characters_live()
{
    {
        Rig rig;
        th2::AvgChar chars(rig.display, rig.hooks());
        chars.set_char(1, 0, 1, 0, th2::char_type_direct, 128, 256, 0);  // slot 0, layer 0
        chars.set_char(2, 0, 1, 1, th2::char_type_direct, 128, 256, 0);  // slot 1, layer 1
        chars.control_char();
        chars.reset_char(1, th2::char_type_cfade, 10);
        chars.control_char();
        // layer*MAX_CHAR+j > layer*MAX_CHAR+i: 1*8+1 beats 0*8+0, so slot 1
        // is held out of the plate while slot 0 animates.
        check(chars.state(1).cut_mode == 0,
              "a character above an animating one is not baked");
        check(rig.display.graph_disp(th2::grp_char + 1),
              "and is drawn as a layer instead");
    }
    {
        // The other way round: below the animating character, so it bakes
        // as usual.
        Rig rig;
        th2::AvgChar chars(rig.display, rig.hooks());
        chars.set_char(1, 0, 1, 1, th2::char_type_direct, 128, 256, 0);  // slot 0, layer 1
        chars.set_char(2, 0, 1, 0, th2::char_type_direct, 128, 256, 0);  // slot 1, layer 0
        chars.control_char();
        chars.reset_char(1, th2::char_type_cfade, 10);
        chars.control_char();
        check(chars.state(1).cut_mode == 1,
              "a character below an animating one still bakes");
    }
}

// AVG_WaitChar is what holds the program counter on a character opcode, and
// it is the reason the script cannot reach a message before the animation
// has finished.
void test_wait_char_blocks_until_settled()
{
    Rig rig;
    th2::AvgChar chars(rig.display, rig.hooks());
    chars.set_char(1, 0, 1, 0, th2::char_type_cfade, 128, 256, 4);
    check(chars.wait_char(1), "an entering character holds the script");
    for (int i = 0; i < 6; ++i) {
        chars.control_char();
    }
    check(!chars.wait_char(1), "and lets go once it settles");
    check(chars.state(0).cond == th2::char_cond_nomal, "settled");

    // CHAR_TYPE_WAIT never holds it - that is the CW form.
    chars.set_char(2, 0, 1, 0, th2::char_type_wait, 128, 256, 4);
    check(!chars.wait_char(2), "the wait form does not hold the script");
}

// The message window closes for the length of an animation and is put back
// afterwards.  This is a real mechanism in the original, not an invention.
void test_window_closes_and_reopens_around_an_animation()
{
    Rig rig;
    th2::AvgChar chars(rig.display, rig.hooks());
    chars.set_char(1, 0, 1, 0, th2::char_type_cfade, 128, 256, 3);
    rig.calls.clear();
    chars.control_char();
    check(rig.said("close_window"), "the window closes while it animates");
    for (int i = 0; i < 5; ++i) {
        chars.control_char();
    }
    check(rig.said("open_window"), "and reopens when it settles");
}

// SetCharPosShake(x, y, ON) is the one thing that takes a settled character
// out of the plate and makes it move on its own.
void test_shake_takes_characters_out_of_the_plate()
{
    Rig rig;
    th2::AvgChar chars(rig.display, rig.hooks());
    chars.set_char(1, 0, 1, 0, th2::char_type_direct, 128, 256, 0);
    chars.control_char();
    check(chars.state(0).cut_mode == 1, "baked to start with");

    chars.set_char_pos_shake(8, 4, 1);
    check(chars.state(0).cut_mode == 2, "a sine shake makes it live");
    chars.set_char_pos_shake(0, 0, 0);
    check(chars.state(0).cut_mode == 0, "and stopping puts it back");
    chars.set_char_pos_shake(0, 0, -1);
    check(chars.state(0).cut_mode == 0,
          "disp -1 moves it without changing cut_mode");
}

// A pose change alternates the slot's two bitmaps so the dissolve has
// something to blend between.
void test_pose_change_alternates_bitmaps()
{
    Rig rig;
    th2::AvgChar chars(rig.display, rig.hooks());
    chars.set_char(1, 0, 1, 0, th2::char_type_direct, 128, 256, 0);
    check(chars.state(0).grp == 0, "the first pose is in bitmap 0");
    chars.set_char_pose(1, 7, th2::char_type_cfade, 4);
    check(chars.state(0).grp == 1, "the next goes in the other one");
    check(chars.state(0).cond == th2::char_cond_pose, "and dissolves");
    chars.set_char_pose(1, 7, th2::char_type_cfade, 4);
    check(chars.state(0).grp == 1, "the same pose again changes nothing");
}

}  // namespace

int main()
{
    test_slots_are_reused();
    test_identical_call_changes_nothing();
    test_bake_cycle();
    test_animating_while_baked_invalidates_the_plate();
    test_occlusion_keeps_upper_characters_live();
    test_wait_char_blocks_until_settled();
    test_window_closes_and_reopens_around_an_animation();
    test_shake_takes_characters_out_of_the_plate();
    test_pose_change_alternates_bitmaps();
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("character state machine ok\n");
    return 0;
}
