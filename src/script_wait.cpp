#include "game.hpp"

#include <algorithm>
#include <array>
#include <string_view>

namespace th2app {
namespace {

// Escr.h's EScroptOpr[].ret, for the opcodes the retail scripts use.
//
//     #define ESC_WAIT   0
//     #define ESC_NOWAIT 1
//     ...
//     return EScroptOpr[opr-64].ret;
//
// and EXEC_ControlTask runs `while( EXEC_CallOprControl( mode ) );` - so a
// zero stops the virtual machine for the rest of the frame and a one lets
// it run straight on to the next instruction.
//
// Every opcode that touches the screen is ESC_WAIT, and that is not a
// detail: it is what guarantees AVG_ControlChar gets a pass between one
// graphics instruction and the next.  A C that has just finished its
// animation yields, the character is composited into BMP_BACK, and only
// then does the following frame reach SetMessage2 and copy the plate.  A
// virtual machine that ran both in one frame would copy a plate the
// character is not in yet.
//
// The twelve arithmetic and flow opcodes the scripts use - AddV, Dec, End,
// Goto, IfR, IfV, Inc, Loop, MovV, Rand, Run and Wait - are not in this
// table at all: the engine dispatches them through EXEC_OprControl rather
// than ESC_OprControl, and they never yield.
constexpr std::array<std::string_view, 34> wait_opcodes{
    "AddMessage2", "B", "BC", "BCT", "BT", "C", "CL", "CP", "CR", "F",
    "FB", "H", "HT", "K", "LoadScript", "MW", "Q", "S", "SEW",
    "SetEnding", "SetMessage2", "SetMovie", "SetSelect", "SetShake",
    "SetTitle", "StopSakura", "V", "VT", "VW", "ViewCalender",
    "ViewClock", "W", "WaitTime", "Z"
};

}  // namespace

bool Game::opcode_yields_frame(std::string_view name)
{
    return std::find(wait_opcodes.begin(), wait_opcodes.end(), name)
        != wait_opcodes.end();
}

}  // namespace th2app
