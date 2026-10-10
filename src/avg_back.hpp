#pragma once

// GM_AvgBack.h and GM_AvgBack.cpp transcribed: BACK_STRUCT and the
// AVG_ControlBack* chain.
//
// BackStruct is the centre of the engine's world.  The half tone reads its
// r/g/b and x/y, AVG_ControlChar reads its flag/sc_flag/zoom to decide
// whether a character may be baked into the plate, and every background
// effect keeps its counter here.  We had it scattered across six optional
// structs with wall-clock start times, which is why AVG_EffCnt could not be
// re-evaluated the way the original re-evaluates it every single frame:
//
//     int back_max = AVG_EffCnt(BackStruct.fd_max);
//     if(BackStruct.fd_flag){
//         BackStruct.fd_cnt++;
//         if( BackStruct.fd_cnt>=back_max ){ ...done... }
//
// fd_max is the raw number the script wrote.  back_max is recomputed on
// every pass, so pressing the skip key mid-wipe takes it to zero and the
// wipe finishes on that same pass.  Nothing has to be rewound.

#include "avg_layout.hpp"
#include "dsp.hpp"

#include <functional>

namespace th2 {

// The chg_type argument of AVG_SetBack, and BackStruct.fd_type.
enum {
    bak_direct = -1,
    bak_fade = 0,
    bak_cfade = 1,
    bak_cfade_up = 2,
    bak_cfade_do = 3,
    bak_cfade_ri = 4,
    bak_cfade_le = 5,
    bak_cfade_ce = 6,
    bak_cfade_ou = 7,
    bak_dia1 = 8,
    bak_dia2 = 9,
    bak_dia3 = 10,
    bak_cfzoom1 = 11,
    bak_cfzoom2 = 12,
    bak_cfzoom3 = 13,
    bak_cfzoom4 = 14,
    bak_kami = 15,
    bak_slide_up = 16,
    bak_slide_do = 17,
    bak_slide_ri = 18,
    bak_slide_le = 19,
    bak_lasterin = 20,
    bak_noise = 21,
    bak_piple = 22,
    bak_tv = 23,
    bak_roll = 24,

    bak_pattern = 0x80,
    bak_ptf_rev = 0x100,
    bak_ptf_accl1 = 0x200,
    bak_ptf_accl2 = 0x400,
    bak_ptf_rev_w = 0x800,
    bak_ptf_rev_h = 0x1000,
};

// SHAKE_ types, the #defines above AVG_SetShake in GM_AvgBack.cpp.  The
// numbering has gaps - 5 and 8 are unused, and the random ones are 9 to 11,
// not the 2/5/8 their position after SIN and 2TI suggests.  That guess is
// what this table used to hold, which ran SHAKE_ZOOM as a random shake and
// never ran the real random shakes at all.
enum {
    shake_sin = 0,
    shake_2ti = 1,
    shake_zoom = 2,
    shake_txt_sin = 3,
    shake_txt_2ti = 4,
    shake_all_sin = 6,
    shake_all_2ti = 7,
    shake_rand = 9,
    shake_txt_rand = 10,
    shake_all_rand = 11,
    shake_roll = 12,
    shake_roll_sin = 13,
    shake_roll_2ti = 14,
    shake_sin_set = 15,
    shake_all_sin_set = 16,
};

// TONE_ (time of day), from GM_AvgBack.h.
enum { tone_normal = 0, tone_evening = 1, tone_night = 2, tone_nt_room = 3 };

// BACK_STRUCT, field for field and in order.  The names are the original's
// so a transcribed line still reads as itself; the ones we do not drive yet
// are kept rather than dropped, because AVG_ControlChar and the half tone
// read several of them and a missing field there reads as dead code.
struct BackStruct {
    int flag = 0;

    // BACK_STRUCT is a zero-initialised global in the original, so every
    // field starts at 0 - including this one.  Nothing here reads bno as a
    // "no background" sentinel; that question is has_background().
    int bno = 0;

    int x = 0, y = 0;
    int x2 = 0, y2 = 0;
    int x3 = 0, y3 = 0;
    int x4 = 0, y4 = 0;
    int w = 0, h = 0;
    int zoom = 0;
    int effect = 0;
    int cg_flag = 0;

    int ns_flag = 0;
    int ns_rate = 0;
    int ns_fade = 0;
    int ns_count = 0;

    int cscop = 0;
    int cs_flag = 0;
    int cs_lock = 0;
    int cs_type = 0;
    int cs_cnt = 0;
    int cs_fade = 0;

    // The background change.
    int fd_flag = 0;
    int fd_type = 0;
    int fd_vague = 128;
    int fd_cnt = 0;
    int fd_max = 0;

    // The scroll / pan / zoom.
    int sc_flag = 0;
    int sx = 0, sy = 0;
    int sx2 = 0, sy2 = 0;
    int sx3 = 0, sy3 = 0;
    int sx4 = 0, sy4 = 0;
    int sw = 0, sh = 0;
    int sc_type = 0;
    int sc_cnt = 0;
    int sc_max = 0;

    // The colour fade.
    int br_flag = 0;
    int br_fade = 0;
    int br_cnt = 0;
    int br_pat = -1;
    int br_type = 0;
    int r = bright_neutral, g = bright_neutral, b = bright_neutral;
    int er = bright_neutral, eg = bright_neutral, eb = bright_neutral;
    int rr = bright_neutral, gg = bright_neutral, bb = bright_neutral;
    int rx = 0, ry = 0;

    // The shake.
    int sk_flag = 0;
    int sk_dir = 0;
    int sk_pich = 0;
    int sk_cnt = 0;
    int sk_type = 0;
    int sk_speed = 0;
    int sk_swing = 256;
    // Unused now - AVG_ControlShake rolls the random shake's direction itself
    // (see AvgBack::ShakeOut).  Kept because harness checkpoints write this
    // struct as raw bytes (Game::trace_checkpoint_resume), and dropping a
    // field shifts every one after it: removing it once made every saved
    // checkpoint resume with the message and the background scrambled.
    int sk_cnt2 = 0;

    int tone_no2 = 0;
    int tone_no = 0;
    int tone_char = 0;
    int tone_back = 0;

    int wether = 0;

    int sp_flag = 0;

    int redraw = 0;
};

// FADE_STRUCT, from GM_avg.h.  It lives in GM_Avg.cpp rather than
// GM_AvgBack.cpp, but it is the same shape as everything here - a flag, a
// counter and a raw max that AVG_EffCnt is re-asked about every frame - and
// it needs the same Display, so it keeps them company.
struct FadeStruct {
    int flag = 0;
    int cnt = 0;
    int fade = 0;
    int disp = 0;
    // The second leg.  AVG_SetFlash is AVG_SetFade followed by a flash
    // count, and AVG_ColtrolFade starts the return leg itself when the
    // first one ends.
    int flash = 0;
    int sr = bright_neutral, sg = bright_neutral, sb = bright_neutral;
    int er = bright_neutral, eg = bright_neutral, eb = bright_neutral;
    int r = bright_neutral, g = bright_neutral, b = bright_neutral;
    // grp_flag[i] / txt_flag[TXT_WINDOW]: what AVG_SetFade turned off and
    // the end of the fade turns back on.
    std::array<bool, grp_disp> grp_flag{};
    bool txt_flag = false;
};

class AvgBack {
public:
    // What AVG_ControlBack* calls out to that is not BackStruct or a graph.
    struct Hooks {
        // AVG_EffCnt / AVG_EffCnt3 / AVG_EffCnt4, re-asked every frame.
        std::function<int(int)> eff_cnt;
        std::function<int(int)> eff_cnt3;
        std::function<int(int)> eff_cnt4;
        // Avg.level: effects on, so a fade is a real blend rather than a
        // dither mesh.  Avg.half_tone: the message wash depth, 0..128.
        std::function<bool()> level;
        std::function<int()> half_tone;
        // AVG_CopyBack( sc ) and AVG_SetBackChar( x, y, disp ), which
        // AVG_ControlBackScroll runs when it finishes.
        std::function<void(bool)> copy_back;
        std::function<void(int, int, bool)> set_back_char;
        // SetCharPosShake( shx, shy, disp ) and SetCharBright, in AvgChar.
        std::function<void(int, int, int)> set_char_pos_shake;
        std::function<void(int, int, int)> set_char_bright;
        // AVG_SetNovelMessageDisp( OFF ) and AVG_ResetHalfTone().
        std::function<void(bool)> novel_message_disp;
        std::function<void()> reset_half_tone;
        // DSP_SetTextDisp( TXT_WINDOW, disp ) alone - not NovelMessage.disp.
        std::function<void(bool)> text_disp;
        // The screen the wipe is blending away from has to exist as a
        // bitmap before GRP_BACK+1 can draw it.  The engine gets it from
        // AVG_SetBack's DSP_CopyBmp; ours captures the framebuffer.
        std::function<void()> capture_outgoing;
        // GlobalCount / GlobalCount2, the free-running frame counters the
        // laster and roll wipes phase off.
        std::function<int()> global_count;
    };

    AvgBack(Display& display, Hooks hooks)
        : display_(display), hooks_(std::move(hooks)) {}

    BackStruct& back() { return back_; }
    const BackStruct& back() const { return back_; }

    // AVG_ControlBack: the whole chain, in order.  The chip branch is gone -
    // no retail script reaches it - and so are ControlNoise, ControlRipple
    // and ControlBackCScope, which are measurably never used.
    void control_back();

    void control_back_change();   // AVG_ControlBackChange
    void control_back_fade();     // AVG_ControlBackFade
    void control_back_scroll();   // AVG_ControlBackScroll
    void control_shake();         // AVG_ControlShake

    // The setters the opcodes call.
    void reset_back_half_tone(int bak_no, int chg_type);  // AVG_ResetBackHalfTone
    // AVG_SetBack's bookkeeping half.  The decode and the DSP_SetBmp that go
    // with it stay with the caller, which owns the archive and the cache.
    void begin_back(int bak_no, int x, int y, int chg_type, int cg_flag,
                    int fd_max, int vague);
    bool wait_back() const { return back_.fd_flag != 0; }        // AVG_WaitBack
    void copy_back(bool sc);                                     // AVG_CopyBack

    void set_back_fade(int r, int g, int b, int fade);           // AVG_SetBackFade
    void set_back_fade_direct(int r, int g, int b);              // AVG_SetBackFadeDirect
    bool wait_back_fade() const { return back_.br_flag != 0; }   // AVG_WaitBackFade

    void set_back_pos(int x, int y);                             // AVG_SetBackPos
    void set_back_pos_zoom(int x, int y, int w, int h);          // AVG_SetBackPosZoom
    void set_back_scroll(int x, int y, int w, int h,
                         int frame, int type);                   // AVG_SetBackScroll
    void set_back_scroll_poly(int x1, int y1, int x2, int y2,
                              int x3, int y3, int x4, int y4,
                              int frame, int type);              // AVG_SetBackScrollPoly
    bool wait_back_scroll() const { return back_.sc_flag != 0; } // AVG_WaitBackScroll

    bool set_shake(int type, int pich, int speed, int dir, int swing);
    bool wait_shake() const;                                     // AVG_WaitShake
    void stop_shake();                                           // AVG_StopShake

    void open_back();    // AVG_OpenBack
    void close_back();   // AVG_CloseBack

    // AVG_SetFade / AVG_SetFlash / AVG_ColtrolFade / AVG_WaitFade.
    void set_fade(int r, int g, int b, int disp, int fade);
    void set_bright(int r, int g, int b);                        // AVG_SetBright
    void set_flash(int r, int g, int b, int fade1, int fade2);
    void control_fade();
    // AVG_Init's share of FadeStruct and the graphs: GRP_DISP and BMP_DISP
    // gone, and nothing left for the end of the next fade to turn back on.
    void reset_fade_freeze();
    bool wait_fade() const { return fade_.flag != 0; }
    const FadeStruct& fade() const { return fade_; }

    void init();

    int shake_text_dx() const { return shake_text_dx_; }
    int shake_text_dy() const { return shake_text_dy_; }
    // What AVG_ControlShake worked out this frame, in its own integer
    // arithmetic, for the draw to apply.  Deriving these a second time in
    // floating point at draw time is not the same number: the engine
    // truncates after each step (COS*pich/512/8, then the taper), and one
    // truncation at the end lands elsewhere - on a rolling shake a whole
    // 1/256 of a turn elsewhere, which is every pixel of the frame.
    struct ShakeOut {
        int x = 0;
        int y = 0;
        int roll = -1;   // DSP_SetGraphRoll's rate, or -1 for no roll
        int zoom = -1;   // DSP_SetGraphZoom2's 256ths, or -1 for no zoom
    };
    const ShakeOut& shake_out() const { return shake_out_; }

    // A frame drawn `phase` (0..1) of a tick after the last one.  Each of
    // these computes what the control function computes, at the count the
    // next tick is heading for, and says false when nothing is running or
    // the next tick ends it anyway.  None of them writes anything.
    //
    // AVG_ControlBackFade's brightness, for GRP_BACK, the script graphs and
    // the characters' back planes.
    bool present_back_fade(double phase, int& r, int& g, int& b) const;
    // AVG_ColtrolFade's FadeStruct colour (the screen flash).
    bool present_fade(double phase, int& r, int& g, int& b) const;
    // AVG_ControlBackChange, for the changes drawn through graphs: the plain
    // cross-fade, the slides and the second and third zooms.  The wipes are
    // drawn from fd_cnt directly (Game::draw_active_transition), and the
    // rest stay stepped.  Changes `graph`, a copy of graph gno.
    bool present_back_change(int gno, double phase, Graph& graph) const;
    // fd_cnt where the next tick is heading, for the wipes; -1 when no
    // change is running or the next tick ends it.
    double present_fd_cnt(double phase) const;

private:
    Display& display_;
    Hooks hooks_;
    BackStruct back_{};
    FadeStruct fade_{};

    // DSP_SetTextMove( TXT_WINDOW, ShakeDx+x, ShakeDy+y ): the text shake's
    // offset, which the renderer adds to the message position.
    int shake_text_dx_ = 0;
    int shake_text_dy_ = 0;
    ShakeOut shake_out_;

    int eff_cnt(int n) const { return hooks_.eff_cnt ? hooks_.eff_cnt(n) : n; }
    int eff_cnt4(int n) const { return hooks_.eff_cnt4 ? hooks_.eff_cnt4(n) : n; }
    bool level() const { return hooks_.level ? hooks_.level() : true; }
    int global_count() const {
        return hooks_.global_count ? hooks_.global_count() : 0;
    }
};

}  // namespace th2
