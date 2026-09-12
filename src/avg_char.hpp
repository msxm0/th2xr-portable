#pragma once

// GM_AvgChar.cpp transcribed.  CharStruct[MAX_CHAR] is the whole character
// model in the original - state and animation counters in one array, with a
// fixed eight slots that are reused as characters come and go - and
// AVG_ControlChar is the state machine that turns it into graphs each frame.
//
// This replaces three things we had invented separately: a vector of
// CharacterState sorted by layer, a parallel array of animations, and a
// cut_mode array.  The slot walk matters: AVG_SetBackChar bakes by layer
// and then by slot, and a slot is whatever GetSpaceIndex found free, which
// is not the order a vector would keep.

#include "dsp.hpp"

#include <array>
#include <functional>

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

// CHAR_COND_*
enum CharCond {
    char_cond_nomal = 0,
    char_cond_in = 1,
    char_cond_out = 2,
    char_cond_pose = 3,
    char_cond_locate = 4,
    char_cond_bright = 5,
    char_cond_alpha = 6,
    char_cond_wait = 7,
};

// CHAR_TYPE_*
enum CharType {
    char_type_direct = -1,
    char_type_cfade = 0,
    char_type_move = 1,
    char_type_wait = 3,
    char_type_wait2 = 4,
    char_type_wave = 5,
};

// CHAR_STRUCT
struct CharState {
    int flag = 0;
    int grp = 0;       // which of the slot's two bitmaps holds the live pose
    int cond = char_cond_nomal;
    int type = char_type_cfade;
    int cno = 0;       // character number as the script knows it
    int disp = 0;      // forced to stay a layer this frame (occlusion)

    int cnt = 0;
    int max = 0;

    int pose = 0;
    int loc1 = 0;      // where it is going
    int loc2 = 0;      // where it came from
    int layer = 0;

    int fade1 = bright_neutral;
    int fade2 = bright_neutral;

    int alph1 = 256;
    int alph2 = 256;

    // 0 composite me into BMP_BACK next pass, 1 I am in it, 2 draw me live
    int cut_mode = 0;
};

// CharLocateTable[9], in the original's units.
int char_locate(int locate);

class AvgChar {
public:
    // The calls AVG_ControlChar makes into the rest of the engine.  Named
    // after what they are so a transcribed line still reads as itself.
    struct Hooks {
        // AVG_LoadChar( index, cno, pose, BackStruct.tone_no, in_type )
        std::function<void(int index, int cno, int pose, int in_type)>
            load_char;
        std::function<void()> reset_half_tone;          // AVG_ResetHalfTone
        std::function<void(bool)> novel_message_disp;   // AVG_SetNovelMessageDisp
        std::function<void()> copy_back;                // AVG_CopyBack(OFF)
        std::function<void()> create_back_cscope;       // AVG_CreateBackCScope
        std::function<bool()> window_cond;              // AVG_GetWindowCond
        std::function<void()> close_window;             // AVG_CloseWindow(OFF)
        std::function<void()> open_window;              // AVG_OpenWindow(OFF,OFF)
        // AVG_EffCnt: the configured effect speed scales every duration.
        std::function<int(int)> eff_cnt;
        std::function<bool()> level;                    // Avg.level
        std::function<bool()> ami;                      // Avg.ami
        // BackStruct.flag / sc_flag / zoom, read by AVG_ControlChar's guard.
        std::function<bool()> back_flag;
        std::function<bool()> back_scrolling;
        std::function<bool()> back_zooming;
        std::function<bool()> back_redraw_take;  // reads and clears .redraw
        // BackStruct.x / .y, added to a baked character's position.
        std::function<void(int*, int*)> back_pos;
    };

    AvgChar(Display& display, Hooks hooks)
        : display_(display), hooks_(std::move(hooks)) {}

    // GetCharIndex: the slot holding this character, or max_char.
    int char_index(int char_no) const;
    // GetSpaceIndex: the first free slot, or max_char.
    int space_index() const;

    void set_char(int char_no, int pose, int locate, int layer, int in_type,
                  int bright, int alph, int frame);        // AVG_SetChar
    void release_char(int index);                          // AVG_ReleaseChar
    void reset_char(int char_no, int out_type, int frame);  // AVG_ResetChar
    void set_char_pose(int char_no, int pose, int in_type, int frame);
    void set_char_locate(int char_no, int locate, int frame);
    void set_char_layer(int char_no, int layer);
    void set_char_bright(int char_no, int fade, int fade_count);
    void set_char_alph(int char_no, int alph, int fade_count);

    void open_char();                                      // AVG_OpenChar
    void close_char();                                     // AVG_CloseChar

    // SetCharPosShake( shx, shy, disp ): moves every character graph and,
    // when disp is not -1, takes them out of the plate (cut_mode 2) or puts
    // them back (0).  Only the sine shakes call it with ON.
    void set_char_pos_shake(int shx, int shy, int disp);
    void set_char_bright_all(int r, int g, int b);         // SetCharBright

    void set_back_release_char();                          // AVG_SetBackReleaseChar
    // AVG_SetBackChar: composites the settled characters into BMP_BACK, by
    // layer and then by slot.
    void set_back_char(int x, int y, int char_disp);

    // AVG_WaitChar: true while the script must stay on this instruction.
    bool wait_char(int char_no) const;

    // AVG_ControlChar, run once a frame between the script and the draw.
    void control_char();

    void init_char();                                      // AVG_InitChar

    const CharState& state(int index) const { return chars_.at(index); }
    CharState& state(int index) { return chars_.at(index); }

private:
    void set_char_pos(int index, int x);                   // SetCharPos
    int locate_offset(int index) const;                    // CharPosTable fold

    Display& display_;
    Hooks hooks_;
    std::array<CharState, max_char> chars_{};
    // The static win_flag inside AVG_ControlChar: whether the message window
    // was up when an animation started, so it can be put back afterwards.
    int win_flag_ = 0;
};

}  // namespace th2
