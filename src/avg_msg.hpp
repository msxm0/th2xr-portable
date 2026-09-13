#pragma once

// GM_AvgMsg.cpp transcribed: the NovelMessage state machine and the half
// tone that goes with it.
//
// This is the piece we had never ported, and every flow bug came out of the
// substitute.  Ours ran the script with advance(), which walked instructions
// until something was pending and then had to be called again by whoever
// noticed the pending thing had finished - seven different update functions,
// each resuming the script from the middle of the frame.
//
// The engine does not work like that.  A waiting opcode leaves the program
// counter where it is and returns ESC_WAIT, so the *same instruction runs
// again next frame* and re-asks its own question:
//
//     if(!EOprFlag[ESC_SETMESSAGE2]){
//         EOprFlag[ESC_SETMESSAGE2] = 1;
//         AVG_SetNovelMessage( EscParam[0].str, EscParam[1].num );
//     }
//     if( AVG_WaitNovelMessage() ){          // step1 == MSG_NEXT
//         EOprFlag[ESC_SETMESSAGE2] = 0;
//         EXEC_AddPC( EscCnt );              // only now does the PC move
//     }
//
// So nothing ever resumes the script.  The script asks, every frame, and
// this state machine is what it is asking.  MSG_DISP runs the typewriter,
// MSG_WAIT and MSG_STOP hold for the reader, and MSG_NEXT is the one frame
// that lets the instruction retire.

#include "avg_key.hpp"
#include "avg_layout.hpp"
#include "dsp.hpp"

#include <functional>

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

// TONE_ from GM_AvgMsg.h.  TONE_FADEIN is in the enum but AVG_ControlHalfTone
// has its whole body commented out in the original, so it never runs.
enum ToneStep {
    tone_nodisp = 0,
    tone_disp,
    tone_fadein,
    tone_fadeout,
};

// NOVEL_MESSEGE, minus str[] and the history buffer: the text itself lives in
// Message and the log in the backlog, which are the two places the original
// keeps char arrays we already have better homes for.
struct NovelMessageState {
    int flag = 0;
    int add_flag = 0;
    int disp = 0;
    int step1 = msg_nodisp;
    int step2 = msg_nodisp;
    int count = 0;      // the typewriter cursor, in glyphs
    int kstep = 0;      // which \k step is being revealed
    int max = 0;        // glyph count this step stops at
};

// HALF_TONE
struct HalfToneState {
    int tstep = tone_nodisp;
    int tcount = 0;
};

class AvgMsg {
public:
    struct Hooks {
        // TXT_GetTextCount( DSP_GetTextStr(TXT_WINDOW), step ): glyphs up to
        // and including \k step `step`, or the whole string for -1.
        std::function<int(int)> text_count;
        // TXT_GetTextEndKeyWait: the text ends on a \k, so add_flag 1 stops
        // rather than running on into the next instruction.
        std::function<bool()> text_end_key_wait;
        std::function<void(int)> set_text_count;   // DSP_SetTextCount
        std::function<void(int)> set_text_step;    // DSP_SetTextStep
        std::function<void(bool)> set_text_disp;   // DSP_SetTextDisp
        // GRP_KEYWAIT: the click indicator.  `page` picks BMP_KEYWAIT+0, the
        // page-end mark, over BMP_KEYWAIT+1, the line-end one.
        std::function<void(bool page)> set_keywait;
        std::function<void()> reset_keywait;
        std::function<void()> set_half_tone;       // AVG_SetHalfTone
        std::function<void()> reset_half_tone;     // AVG_ResetHalfTone
        std::function<void(int, int)> play_se;     // AVG_PlaySE3
        std::function<bool()> wait_voice;          // AVG_WaitVoice(0)
        std::function<bool()> hit_key;             // AVG_GetHitKey
        std::function<bool()> mes_cut;             // AVG_GetMesCut
        std::function<int()> msg_cnt;              // AVG_MsgCnt
        // AVG_NovelLogStart: page up into the backlog.  Ours is a UI mode
        // rather than MSG_UP/MSG_LOG/MSG_DRAG, so the log states below are
        // the ones this replaces.
        std::function<void()> log_start;
        // GameKey.diswin hides the window until the next click - MSG_SYSTEM.
        std::function<void(bool)> window_disp;
        // Avg.auto_flag / Avg.auto_key / Avg.auto_page / Avg.msg_page.
        std::function<bool()> auto_flag;
        std::function<int()> auto_key;
        std::function<int()> auto_page;
        std::function<bool()> msg_page;
        // Message::reveal_next(), which is what DSP_SetTextStep's advance
        // means for us: the next \k segment joins the visible string.
        std::function<void(int)> reveal_step;
    };

    AvgMsg(Display& display, Hooks hooks)
        : display_(display), hooks_(std::move(hooks)) {}

    // AVG_SetNovelMessage / AVG_AddNovelMessage.  The text itself has already
    // been put in the Message by the caller, as DSP_SetText does here.
    void set_novel_message(int add_flag);
    void add_novel_message(int cr);
    // AVG_WaitNovelMessage: the one state that lets the opcode retire.
    bool wait_novel_message() const { return message_.step1 == msg_next; }
    void set_novel_message_disp(bool disp);
    // AVG_ControlNovelMessage, run from AVG_System between the script and the
    // draw.
    void control_novel_message(const GameKey& key);

    // AVG_WaitAutoMode / AVG_ResetAutoMode.
    bool wait_auto_mode();
    void reset_auto_mode() { auto_count_ = 0; }

    const NovelMessageState& state() const { return message_; }
    NovelMessageState& state() { return message_; }
    const HalfToneState& half_tone() const { return half_tone_; }
    HalfToneState& half_tone() { return half_tone_; }

    void init();  // InitNovelMessage

private:
    Display& display_;
    Hooks hooks_;
    NovelMessageState message_{};
    HalfToneState half_tone_{};
    int auto_count_ = 0;
    // The two statics inside AVG_ControlNovelMessage.  key_wait_count2
    // reaching thirty is what auto-advances a screenshot run; we keep them so
    // the transcription reads as itself.
    int key_wait_count_ = 0;
    int key_wait_count2_ = 0;

    // The MSG_WAIT / MSG_STOP body, which is one block in the original
    // guarded by `if( Avg.demo ) ... else ...`.
    void control_wait(const GameKey& key);
    // Shared tail of every "the reader is ready" branch.
    void advance_step();
};

}  // namespace th2
