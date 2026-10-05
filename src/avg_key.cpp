#include "avg_key.hpp"

#include <cstddef>

namespace th2 {

void KeyCond::clear_triggers()
{
    trg_enter = trg_esc = trg_bs = trg_space = trg_shift = false;
    trg_home = trg_end = btrg_pup = btrg_pdown = false;
    btrg_up = btrg_down = btrg_left = btrg_right = false;
    mouse_trg_left = mouse_trg_right = mouse_trg_middle = false;
    for (bool& digit : trg_num) {
        digit = false;
    }
    wheel = 0;
}

void get_game_key(GameKey& key, const KeyCond& cond, int wheel_mode,
                  bool demo)
{
    const int wclick = cond.btn_enter || cond.mouse_btn_left;
    const int wheel = cond.wheel;

    // Alt+Enter is the fullscreen toggle, so it must not also advance.
    bool trg_enter = cond.trg_enter;
    if (trg_enter && cond.btn_alt) {
        trg_enter = false;
    }

    key.click = trg_enter || cond.mouse_trg_left;
    key.cansel = cond.trg_esc || cond.trg_bs || cond.mouse_trg_right;

    //     GameKey.num[j] = KeyCond.trg.kJ || KeyCond.trg.nJ;
    // ten lines of it in AVG_GetGameKey, the keypad folded onto the number
    // row.  Ours arrives already folded, one flag per digit.
    for (std::size_t digit = 0; digit < 10; ++digit) {
        key.num[digit] = cond.trg_num[digit] ? 1 : 0;
    }

    switch (wheel_mode) {
    case 1:
        key.diswin = cond.trg_space;
        key.mes_cut = cond.btn_ctrl;
        key.mes_cut_mode = cond.trg_shift;
        break;
    default:
        key.diswin = cond.trg_space || cond.mouse_trg_middle;
        key.mes_cut = cond.btn_ctrl;
        key.mes_cut_mode = cond.trg_shift;
        break;
    }

    key.end = cond.trg_end && !wclick;
    key.home = cond.trg_home && !wclick;
    if (wheel > 0) {
        key.pup = !wclick;
        key.pdown = 0;
    } else if (wheel < 0) {
        key.pup = 0;
        key.pdown = !wclick;
    } else {
        key.pup = cond.btrg_pup && !wclick;
        key.pdown = cond.btrg_pdown && !wclick;
    }

    // Held combinations resolve to one thing, and the skip key always loses.
    // This is why tapping control while clicking never half-skips in the
    // original: the click wins outright and mes_cut is cleared for the frame.
    if (key.click && key.cansel) {
        key.click = 0;
        key.cansel = 1;
    }
    if (key.click && key.mes_cut) {
        key.mes_cut = 0;
        key.mes_cut_mode = 0;
        key.click = 1;
    }
    if (key.cansel && key.mes_cut) {
        key.mes_cut = 0;
        key.mes_cut_mode = 0;
        key.cansel = 1;
    }
    if (key.end && key.home) {
        key.home = 1;
        key.end = 0;
    }
    if (key.pup && key.pdown) {
        key.pup = 1;
        key.pdown = 0;
    }

    //     if(Avg.demo || Flag_MenuSelected){
    //         GameKey.click = 0;  GameKey.cansel = 0;  GameKey.diswin = 0;
    //         GameKey.mes_cut = 0;
    //         GameKey.end = 0;  GameKey.home = 0;
    //         GameKey.pup = 0;  GameKey.pdown = 0;
    //     }
    // mes_cut_mode, the arrows and the number row are left alone.
    // Flag_MenuSelected is the Windows menu bar's, which the port has not.
    if (demo) {
        key.click = 0;
        key.cansel = 0;
        key.diswin = 0;
        key.mes_cut = 0;
        key.end = 0;
        key.home = 0;
        key.pup = 0;
        key.pdown = 0;
    }

    key.u = cond.btrg_up;
    key.d = cond.btrg_down;
    key.l = cond.btrg_left;
    key.r = cond.btrg_right;
}

}  // namespace th2
