#pragma once

// GM_AvgMsg.cpp transcribed: the NovelMessage state machine and the half
// tone that goes with it.
//
// This is the piece we had never ported, and most of this session's flow
// bugs came out of the substitute - waiting_for_input_, a wall-clock reveal
// timer and an auto-mode timer, all invented.
//
// The engine has one state machine instead, and the script asks it a single
// question every frame:
//
//     if(!EOprFlag[ESC_SETMESSAGE2]){
//         EOprFlag[ESC_SETMESSAGE2] = 1;
//         AVG_SetNovelMessage( EscParam[0].str, EscParam[1].num );
//     }
//     if( AVG_WaitNovelMessage() ){          // step1 == MSG_NEXT
//         EOprFlag[ESC_SETMESSAGE2] = 0;
//         EXEC_AddPC( EscCnt );
//     }
//
// MSG_DISP runs the typewriter, MSG_WAIT holds at a \k in the middle of a
// line, MSG_STOP holds at the end of one, and MSG_NEXT is the single frame
// that lets the instruction retire.

#include "avg_back.hpp"
#include "avg_key.hpp"
#include "avg_layout.hpp"
#include "dsp.hpp"
#include "text_count.hpp"

#include <functional>
#include <string>

namespace th2 {

// The MSG_ enum from GM_AvgMsg.h, in its original order.
enum MsgStep {
    msg_nodisp = 0,
    msg_disp,
    msg_system,
    msg_wait,
    msg_stop,
    msg_next,
    msg_up,
    msg_down,
    msg_drag,
    msg_log,
    msg_wait2,
};

// TONE_ from GM_AvgMsg.h.  TONE_FADEIN is in the enum but its whole body in
// AVG_ControlHalfTone is commented out, so it never runs.
enum ToneStep {
    tone_nodisp = 0,
    tone_disp,
    tone_fadein,
    tone_fadeout,
};

// NOVEL_MESSEGE, minus str[] and the history buffer: the text itself lives
// in Message and the log in the backlog, which are the two places the
// original keeps char arrays we already have better homes for.
struct NovelMessageState {
    int flag = 0;
    int add_flag = 0;
    int disp = 0;
    int step1 = msg_nodisp;
    int step2 = msg_nodisp;
    int count = 0;      // the typewriter cursor, in TXT_GetTextCount units
    int kstep = 0;      // which \k step is being revealed
    int max = 0;        // the count this step stops at
};

// HALF_TONE
struct HalfToneState {
    int tstep = tone_nodisp;
    int tcount = 0;
};

class AvgMsg {
public:
    struct Hooks {
        // GRP_KEYWAIT: the click indicator.  `page` picks BMP_KEYWAIT+0, the
        // page-end mark, over BMP_KEYWAIT+1, the line-end one.
        std::function<void(bool page)> set_keywait;
        std::function<void()> reset_keywait;
        std::function<void(int, int)> play_se;     // AVG_PlaySE3
        std::function<bool()> wait_voice;          // AVG_WaitVoice(0)
        std::function<bool()> hit_key;             // AVG_GetHitKey
        std::function<bool()> mes_cut;             // AVG_GetMesCut
        std::function<int()> msg_cnt;              // AVG_MsgCnt
        std::function<int()> eff_cnt_puls;         // AVG_EffCntPuls
        // AVG_NovelLogStart: page up into the backlog.  Ours is a UI mode
        // rather than MSG_UP/MSG_LOG/MSG_DRAG, so this is what replaces
        // those four states of the machine.
        std::function<void()> log_start;
        // DSP_SetTextDisp( TXT_WINDOW, disp ) - hides the text without
        // closing the window, which is what lets AVG_ControlChar put it back
        // after an animation.
        std::function<void(bool)> set_text_disp;
        // Message::reveal_next(): the next \k segment joins the visible
        // string, which is what DSP_SetTextStep's advance means for us.
        std::function<void(int)> reveal_step;
        // Avg.auto_flag / Avg.auto_key / Avg.auto_page / Avg.msg_page.
        std::function<bool()> auto_flag;
        std::function<int()> auto_key;
        std::function<int()> auto_page;
        std::function<bool()> msg_page;
        // Avg.half_tone: how dark the wash goes, 0..128.
        std::function<int()> half_tone_depth;
        // SetCharHalfTone / SetCharBright, in AvgChar.
        std::function<void(bool)> set_char_half_tone;
        std::function<void(int, int, int)> set_char_bright;
        // AVG_GetWavEffectFlag(): the wave effect cuts the ramp short.  No
        // retail script starts one, so this is always false.
        std::function<bool()> wav_effect;
        // AVG_CloseWindow hides every script overlay that was showing and
        // AVG_OpenWindow puts them back:
        //     for( i=0; i<MAX_SCRIPT_OBJ ; i++ )
        //         if(SpriteBmp[i].disp) DSP_SetGraphDisp( GRP_SCRIPT+i, x );
        std::function<void(bool)> script_objects_disp;
        // Avg.demo / AVG_EffCnt3(Avg.demo_max): the attract loop, which
        // reads at a fixed rate and turns its own pages.
        std::function<bool()> demo;
        std::function<int()> demo_max;
    };

    AvgMsg(Display& display, BackStruct& back, Hooks hooks)
        : display_(display), back_(&back), hooks_(std::move(hooks)) {}

    // AVG_SetNovelMessage / AVG_AddNovelMessage.  `raw` is the script's
    // string with its tags still in it, which is what DSP_GetTextStr returns
    // and what TXT_GetTextCount is measured over.
    void set_novel_message(const std::string& raw, int add_flag);
    void add_novel_message(const std::string& raw, int cr);
    // AVG_WaitNovelMessage: the one state that lets the opcode retire.
    bool wait_novel_message() const { return message_.step1 == msg_next; }
    void set_novel_message_disp(bool disp);      // AVG_SetNovelMessageDisp
    // AVG_OpenWindow( tdisp, flag ) / AVG_CloseWindow( flag ).  Closing the
    // window parks the message machine at MSG_NODISP and remembers where it
    // was; opening puts it back.  That is what stops the typewriter for the
    // length of a character animation and starts it again afterwards.
    void open_window(bool tdisp);
    void close_window();
    // AVG_GetWindowCond(): 1 while the window is up, -1 while it is moving,
    // 0 when it is not there.
    int window_cond() const { return wstep_ == 0 ? 0 : 1; }
    // AVG_ControlNovelMessage, run from AVG_System between the script and
    // the draw.  One call per sixtieth of a second.
    void control_novel_message(const GameKey& key);

    // AVG_WaitAutoMode / AVG_ResetAutoMode.
    bool wait_auto_mode();
    void reset_auto_mode() { auto_count_ = 0; }

    // --- the half tone, also GM_AvgMsg.cpp ------------------------------
    void set_half_tone();          // AVG_SetHalfTone
    void set_half_tone_direct();   // AVG_SetHalfToneDirect
    void reset_half_tone();        // AVG_ResetHalfTone
    void control_half_tone();      // AVG_ControlHalfTone
    int check_half_tone_step() const { return half_tone_.tstep; }
    void set_half_tone_step(int step) { half_tone_.tstep = step; }

    const NovelMessageState& state() const { return message_; }
    NovelMessageState& state() { return message_; }
    const HalfToneState& half_tone() const { return half_tone_; }
    HalfToneState& half_tone() { return half_tone_; }
    // Restoring a trace checkpoint.  The machine's state is not only
    // NovelMessage: ts->cnt for TXT_WINDOW says how much of the text is
    // uncovered, and -1 means all of it.  Restoring the message without it
    // left count at 205 of 213 beside a slot showing nothing, and the machine
    // resolved that by abandoning the message and starting the next one.
    void restore_text_slot(int cnt, int step)
    {
        text_cnt_ = cnt;
        text_step_ = step;
    }
    // Restoring the source is not enough: counted_ is derived from it
    // (txt_count_text) and holds the per-glyph tables the typewriter draws
    // from, so without recomputing it a resumed run has the counters but no
    // glyphs - the state trace showed "-" where a straight-through run had a
    // full line, and the first click after the resume then advanced a
    // message that had nothing in it.
    void restore_raw(std::string raw);
    // ts->cnt and ts->step for TXT_WINDOW; see text_cnt_.
    int text_cnt() const { return text_cnt_; }
    int text_step() const { return text_step_; }

    // The rest of the machine's own state.  NovelMessage's eight fields are
    // not all of it: these decide what happens when a message finishes, and
    // a checkpoint that leaves them behind resumes into the wrong arm.
    // end_key_wait_ is the one that was measured - resuming mid-message at
    // pc 22370 of 040425000.sdt, ours took the no-keywait arm and retired
    // the instruction on the frame the text completed, where a run that had
    // been there all along waited for the reader.
    struct ResumeState {
        int vanilla_layout = 0;
        int end_key_wait = 0;
        int auto_count = 0;
        int key_wait_count = 0;
        int key_wait_count2 = 0;
        int wstep = 0;
        int demo_cnt = 0;
    };
    ResumeState resume_state() const
    {
        return {vanilla_layout_ ? 1 : 0, end_key_wait_ ? 1 : 0, auto_count_,
                key_wait_count_, key_wait_count2_, wstep_, demo_cnt_};
    }
    void restore_resume_state(const ResumeState& state)
    {
        vanilla_layout_ = state.vanilla_layout != 0;
        end_key_wait_ = state.end_key_wait != 0;
        auto_count_ = state.auto_count;
        key_wait_count_ = state.key_wait_count;
        key_wait_count2_ = state.key_wait_count2;
        wstep_ = state.wstep;
        demo_cnt_ = state.demo_cnt;
    }
    // The accumulated message source, tags and all.
    const std::string& raw() const { return raw_; }
    // The engine's own layout of the message, in the message window's box.
    // Used for drawing and for the overflow clip when the original bitmap
    // font is selected; ignored otherwise, where our own wrapping and the
    // scrolling window take over.
    const TextCount& layout() const { return layout_; }
    void set_vanilla_layout(bool on) { vanilla_layout_ = on; }
    bool vanilla_layout() const { return vanilla_layout_; }

    // What the renderer needs to draw the typewriter.
    const TextCount& counted() const { return counted_; }
    // How many characters of the visible string are on screen.
    std::size_t visible_glyphs() const;
    // alph2 = LIM( text_cnt - cnt2, 0, 16 ) * 16, the per-character fade
    // TXT_DrawTextEx applies while a line is still arriving.
    int glyph_alpha(std::size_t index) const;
    // TXT_GetTextEndKeyWait over the current text.
    bool text_end_key_wait() const { return end_key_wait_; }

    void init();  // InitNovelMessage

private:
    Display& display_;
    BackStruct* back_;
    Hooks hooks_;
    NovelMessageState message_{};
    // TEXT_STRUCT's own cnt and step for TXT_WINDOW, which the engine keeps
    // *separately* from NovelMessage.count and .kstep and writes through
    // DSP_SetTextCount / DSP_SetTextStep.  They are not the same thing and
    // collapsing them was wrong twice over: -1 in either is a distinct
    // state, meaning "draw the lot" and "no \k stop", and the engine parks
    // them there at every wait while the NovelMessage fields carry on
    // holding live values.
    int text_cnt_ = 0;
    int text_step_ = 0;
    HalfToneState half_tone_{};
    TextCount counted_{};
    std::string raw_;
    TextCount layout_{};
    bool vanilla_layout_ = false;
    bool end_key_wait_ = false;
    int auto_count_ = 0;
    // The two statics inside AVG_ControlNovelMessage.
    int key_wait_count_ = 0;
    int key_wait_count2_ = 0;
    // Message.wstep, reduced to the two states our window has: it is drawn
    // at monitor resolution rather than as GRP_WINDOW graphs, so MWIN_OPEN
    // and MWIN_CLOSE - the slide, which SetMessageWindowEffect drives - have
    // no counterpart here.  MWIN_NODISP is 0, MWIN_STOP is 1.
    int wstep_ = 0;
    int demo_cnt_ = 0;   // Avg.demo_cnt

    // TXT_GetTextCount( DSP_GetTextStr(TXT_WINDOW), step ).
    int text_count(int step) const { return txt_get_text_count(counted_, step); }
    // The MSG_WAIT / MSG_STOP body.
    void control_wait(const GameKey& key);
    // Shared tail of every "the reader is ready" branch.
    void advance_step();
    int half_tone_depth() const {
        return hooks_.half_tone_depth ? hooks_.half_tone_depth() : 128;
    }
};

}  // namespace th2
