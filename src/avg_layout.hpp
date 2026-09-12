#pragma once

// The numbering from GM_avg.h: which graph slot, bitmap slot and layer each
// part of the scene lives in.  Worked through from the chain of #defines
// rather than guessed, since every one is relative to the last.

#include "dsp.hpp"

namespace th2 {

inline constexpr int max_char = 8;  // MAX_CHAR

// GRP_/BMP_/LAY_ from GM_avg.h, worked through rather than guessed:
//   GRP_BACK2 0, GRP_BACK 1, GRP_SPBACK 51, GRP_SPBACKHALF 60,
//   GRP_CHIP 68, GRP_CHAR 76
inline constexpr int grp_back2 = 0;
inline constexpr int grp_back = grp_back2 + 1;
inline constexpr int grp_back_chip = 50;
inline constexpr int grp_char_chip = 8;
inline constexpr int grp_spback = grp_back + grp_back_chip;
inline constexpr int grp_spbackhalf = grp_spback + 1 + max_char;
inline constexpr int grp_chip = grp_spbackhalf + max_char;
inline constexpr int grp_char = grp_chip + grp_char_chip;

inline constexpr int bmp_back = 0;
inline constexpr int bmp_back2 = bmp_back + 8;
inline constexpr int bmp_backhalf = bmp_back2 + 1;
inline constexpr int bmp_spback = bmp_backhalf + 1;
inline constexpr int bmp_spbackhalf = bmp_spback + 1;
inline constexpr int bmp_chip = bmp_spbackhalf + 1;
inline constexpr int bmp_char = bmp_chip + 8;

inline constexpr int lay_back = 1;
inline constexpr int lay_spback = lay_back + 4;
inline constexpr int lay_chip = lay_spback + 4;
inline constexpr int lay_fore = lay_chip + 5;
inline constexpr int lay_char = lay_fore + 4;    // 18
inline constexpr int lay_weather = lay_char + 4;
inline constexpr int lay_window = lay_weather + 4;

// GRP_WINDOW onwards, for the parts that are not characters.  GRP_WORK is
// the scratch graph AVG_ControlShake parks a black rectangle in at layer 0,
// so that a shake which slides or rotates the background shows black in the
// strip it uncovers rather than the previous frame.
inline constexpr int grp_window = grp_char + max_char;      // 84
inline constexpr int grp_history = grp_window + 10;         // 94
inline constexpr int grp_keywait = grp_history + 15;        // 109
inline constexpr int grp_system = grp_keywait + 4;          // 113
inline constexpr int grp_system2 = grp_system + 20;         // 133
inline constexpr int grp_select = grp_system2 + 10;         // 143
inline constexpr int grp_save = grp_select + 10;            // 153
inline constexpr int grp_weather = grp_save + 70;           // 223
inline constexpr int grp_work = grp_weather + 200;          // 423
inline constexpr int grp_script = grp_work + 100;           // 523

}  // namespace th2
