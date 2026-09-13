#include "game.hpp"

#include <algorithm>
#include <array>
#include <string_view>

namespace th2app {

// Escr.h's EScroptOpr[] table and the ESC_EOpr* handlers in Escript.cpp, for
// the 87 opcodes the retail scripts use.
//
//     #define ESC_WAIT   0
//     #define ESC_NOWAIT 1
//     ...
//     return EScroptOpr[opr-64].ret;
//
// and EXEC_ControlTask runs `while( EXEC_CallOprControl( mode ) );` - so a
// zero stops the virtual machine for the rest of the frame and a one lets it
// run straight on to the next instruction.
//
// The important half is what an ESC_WAIT opcode does about it.  It does not
// hand the machine off to anything: it leaves the program counter where it is
// and is *re-executed next frame*, where it asks its own question again.
//
//     if(!EOprFlag[ESC_C]){                  // once, on the first frame
//         EOprFlag[ESC_C] = 1;
//         AVG_SetChar( ... );
//     }
//     if( !AVG_WaitChar( EscParam[0].num ) ){ // every frame after that
//         EOprFlag[ESC_C] = 0;
//         EXEC_AddPC( EscCnt );               // only now does the PC move
//     }
//
// Two things fall out of that and both matter.  Each opcode waits on exactly
// its own thing - a C waits for its character and nothing else, so a
// background fade running at the same time cannot hold it - and nothing ever
// has to resume the script, because the script never stopped asking.
//
// The twelve arithmetic and flow opcodes the scripts use - AddV, Dec, End,
// Goto, IfR, IfV, Inc, Loop, MovV, Rand, Run and Wait - are not in this table
// at all: the engine dispatches them through EXEC_OprControl rather than
// ESC_OprControl, and they never yield.
namespace {

struct WaitOpcode {
    std::string_view name;
    WaitKind kind;
    // EOprFlag counts up rather than latching for four of them: B, BC, H and
    // V clear the old background's half tone on their first frame and only
    // set the new background on their second, so the wash is gone from the
    // plate the new one is copied from.
    int phases;
};

constexpr std::array<WaitOpcode, 34> wait_opcodes{{
    {"AddMessage2", WaitKind::novel_message, 1},
    {"B",           WaitKind::back,          2},
    {"BC",          WaitKind::back,          2},
    {"BCT",         WaitKind::back,          1},
    {"BT",          WaitKind::back,          1},
    {"C",           WaitKind::character,     1},
    {"CL",          WaitKind::character,     1},
    {"CP",          WaitKind::character,     1},
    {"CR",          WaitKind::character,     1},
    {"F",           WaitKind::fade,          1},
    {"FB",          WaitKind::back_fade,     1},
    {"H",           WaitKind::back,          2},
    {"HT",          WaitKind::back,          1},
    {"K",           WaitKind::key,           1},
    {"LoadScript",  WaitKind::frame,         1},
    {"MW",          WaitKind::bgm,           1},
    {"Q",           WaitKind::shake,         1},
    {"S",           WaitKind::back_scroll,   1},
    {"SEW",         WaitKind::se,            1},
    {"SetEnding",   WaitKind::movie,         1},
    {"SetMessage2", WaitKind::novel_message, 1},
    {"SetMovie",    WaitKind::movie,         1},
    {"SetSelect",   WaitKind::select,        1},
    {"SetShake",    WaitKind::shake,         1},
    {"SetTitle",    WaitKind::frame,         1},
    {"StopSakura",  WaitKind::frame,         1},
    {"V",           WaitKind::back,          2},
    {"VT",          WaitKind::back,          1},
    {"VW",          WaitKind::voice,         1},
    {"ViewCalender",WaitKind::frame,         1},
    {"ViewClock",   WaitKind::frame,         1},
    {"W",           WaitKind::frame,         1},
    {"WaitTime",    WaitKind::frame,         1},
    {"Z",           WaitKind::back_scroll,   1},
}};

const WaitOpcode* find(std::string_view name)
{
    const auto it = std::ranges::find(wait_opcodes, name, &WaitOpcode::name);
    return it == wait_opcodes.end() ? nullptr : &*it;
}

}  // namespace

bool Game::opcode_yields_frame(std::string_view name)
{
    return find(name) != nullptr;
}

WaitKind Game::opcode_wait_kind(std::string_view name)
{
    const auto* entry = find(name);
    return entry ? entry->kind : WaitKind::none;
}

int Game::opcode_phases(std::string_view name)
{
    const auto* entry = find(name);
    return entry ? entry->phases : 1;
}

}  // namespace th2app
