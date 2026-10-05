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

#include <array>
#include <functional>
#include <utility>
#include <iosfwd>
#include <string>
#include <vector>

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

// NOVEL_MESSEGE, minus str[]: the text itself lives in raw_ below.  The
// history buffer is NovelBufState.
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

// GM_AvgMsg.h's NLOG_MAX and NLOG_V_MAX, and mouse.h's MOUSE_REST_MAX.
inline constexpr int nlog_max = 256;
inline constexpr int nlog_v_max = 32;
inline constexpr int mouse_rest_max = 64;
// The mouse layers in use: 0 for the history bar and the log, 1 for the
// system menu (MOUSE_LAYER_MAX is 48; nothing a trace reaches uses more).
inline constexpr int mouse_layers = 2;
// MUS_GetMouseNo's button argument.
inline constexpr int mouse_any = -1;
inline constexpr int mouse_lbutton = 0;
inline constexpr int mouse_lbtrigger = 8;

// MOUSE_STRUCT, the part the message machine reads: my_inc2/mouse.cpp's
// MUS_RenewMouse, run once at the top of a frame.
struct EngineMouse {
    int x = 0;
    int y = 0;
    bool bl = false;    // the button, as a level
    bool tl = false;    // bl's rising edge
    bool btl = false;   // (lcnt==15) || (lcnt==1): the edge, then every
                        // frame from the fifteenth held one on
    int lcnt = 0;
    int no = -1;        // the rect under the pointer, or -1
};

// MOUSE_CHECK, layer 0.
struct MouseRect {
    bool flag = false;
    int sx = 0;
    int sy = 0;
    int w = 0;
    int h = 0;
    int rect_no = 0;
};

// NOVEL_VOICE, one voice of one log entry.  vstcount is where in the entry's
// text the voiced line starts - a byte offset into our UTF-8 copy rather than
// the engine's CP932 one, which is only ever used to index that same string.
struct NovelVoice {
    int sno = 0;
    int vno = 0;
    int cno = 0;
    int a_cut = 0;
    int vstcount = 0;
    int px = 0;
    int py = 0;
};

// NOVEL_BUF: the engine's own log, a ring of NLOG_MAX entries.  Every
// AVG_SetNovelMessage adds one and AVG_AddNovelMessage extends the newest, so
// the line on screen is always already in it - bmax counts it, and bcount
// is how many entries back the reader is looking.
struct NovelBufState {
    std::array<std::string, nlog_max> buf{};
    std::array<std::vector<NovelVoice>, nlog_max> nv{};
    std::array<std::string, nlog_v_max> vv_mes{};
    // Where each voiced line of the entry on screen runs in its raw text,
    // [first, second) - for a presentation that measures its own rects.
    std::array<std::pair<std::size_t, std::size_t>, nlog_v_max> vv_span{};
    int sno = 0;        // SetNovelMessageVoice1's pending voice
    int vno = 0;
    int cno = 0;
    int a_cut = 0;
    int bmax = 0;
    int bpoint = 0;
    int bcount = 0;
};

// A TEXT_STRUCT the log puts up: TXT_WINDOW+1, the entry being read, and
// TXT_WINDOW+2, the voiced line under the pointer drawn over it.
struct EngineText {
    bool flag = false;
    bool disp = false;
    std::string str;
    int x = 0;
    int y = 0;
    int color = 0;      // the FCT index
};

// GRP_HISTORY+0..10 as ControlHistorySystem leaves them: +0 the backing,
// +1 the log handle, +2..+9 the buttons, +10 the half-tone handle.
struct HistoryGraph {
    int dx = 0;
    int dy = 0;
    int sx = 0;
    int sy = 0;
    int w = 0;
    int h = 0;
};
struct HistoryBar {
    bool shown = false;
    int fade = 0;       // DRW_BLD(fade), all eleven planes
    std::array<HistoryGraph, 11> g{};
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

        // --- the history bar, used only with set_engine_bar(true) --------
        // AVG_GetSelectMessageFlag / AVG_OpenSelectWindow /
        // AVG_CloseSelectWindow: a choice is up under the message.
        std::function<bool()> select_message_flag;
        std::function<void()> open_select_window;
        std::function<void()> close_select_window;
        // AVG_GoConfig( 1 save, 2 load, 3 config ).
        std::function<void(int)> go_config;
        // The voice rects of a log entry in a presentation's own units: the
        // rects covering raw text [start, end) of `text` as it is drawn.
        // Unset, or answering false, leaves the original's units, which is
        // what the reference answers a click with.
        std::function<bool(const std::string& text, std::size_t start,
                           std::size_t end,
                           std::vector<std::array<int, 4>>& rects)>
            log_voice_rects;
        // MSG_STOP left for MSG_NEXT: the reader has finished a page.
        std::function<void()> page_end;
        // MUS_SetMousePos: put the pointer at (x, y) in the 800x600 space,
        // where the next MUS_RenewMouse finds it.
        std::function<void(int, int)> set_mouse_pos;
        // Avg.auto_flag = !Avg.auto_flag, and Avg.msg_cut_mode.
        std::function<void()> toggle_auto_flag;
        std::function<bool()> msg_cut_mode;
        std::function<void(bool)> set_msg_cut_mode;
        // (Avg.msg_cut_optin&1) || AVG_CheckScenarioFlag(): whether skipping
        // is offered on this line at all.
        std::function<bool()> msg_cut_offered;
        // Avg.side_option: 0 fades to 64, 1 always up, 2 fades out, 3 off.
        std::function<int()> side_option;
        std::function<bool()> omake;
        // Avg.half_tone = ..., from the slider.
        std::function<void(int)> set_half_tone_depth;
        // AVG_PlayVoice( 0, cno, sno, vno, 255, 0, a_cut, 1 ): a voice
        // clicked in the log.
        std::function<void(int cno, int sno, int vno, int a_cut)> log_voice;
        // DSP_GetTextDispPos( TXT_WINDOW, ... ): where the click indicator
        // goes, measured by running the text through DrawGraphText without a
        // destination.  Only the trace cares - the reference's glyph probe
        // sees that pass as a draw.
        std::function<void()> measure_text;
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

    // --- the history bar and the log ------------------------------------
    //
    // The engine's side bar is part of this machine: its buttons are mouse
    // rects on layer 0, AVG_ControlNovelMessage answers them from MSG_WAIT,
    // MSG_STOP and MSG_NEXT, and paging back through the log is four more
    // states of step1 (MSG_UP, MSG_LOG, MSG_DOWN, MSG_DRAG) with the text
    // drawn from NovelBuf.  Every run uses it, so a recorded run that works
    // the bar replays against the reference, and what that verifies is what
    // is played.
    // MUS_RenewMouse: the pointer and the left button for this frame.
    void renew_mouse(int x, int y, bool left);
    // A press of bar button `no` (1..9) this tick, as if its rect had been
    // clicked, for an input with no pointer behind it - the gamepad's auto
    // button, the touch swipe.  The bar's own answer to it runs unchanged,
    // only in the states that answer the bar at all.  Lasts until the next
    // renew_mouse.
    void press_bar_button(int no) { bar_press_ = no; }
    const EngineMouse& mouse() const { return mouse_; }
    // MUS_GetMouseNoEx( button, lno ): the rect under the pointer on layer
    // lno, if that is the current layer and `button` has its edge this
    // frame (mouse_any asks for no edge at all).
    int mouse_no_ex(int button, int lno) const;
    // MUS_SetMousePosRect( hwnd, lno, no ): the pointer onto the bottom-left
    // of rect `no` of layer `lno`, if it is flagged - how the arrow keys
    // walk the menu and the choices.
    void set_mouse_pos_rect(int lno, int no);
    // MUS_GetMouseNo( -1 ): the rect under the pointer on the current layer.
    int mouse_no() const { return mouse_.no; }
    // MUS_SetMouseLayer / MUS_GetMouseLayer / MUS_SetMouseRect /
    // MUS_SetMouseRectFlag / MUS_ResetMouseRect_Layer.
    void set_mouse_layer(int lno) { mouse_layer_ = lno; }
    int mouse_layer() const { return mouse_layer_; }
    void set_mouse_rect(int lno, int no, int sx, int sy, int w, int h,
                        bool flag);
    void set_mouse_rect_flag(int lno, int no, bool flag);
    void reset_mouse_rect_layer(int lno);
    // AVG_CloseWindow( 1 ) / AVG_OpenWindow( ON, 1 ) as AVG_GoConfig and
    // AVG_EndConfig call them - verbatim, which unlike close_window() and
    // open_window() leaves the window's own state (Message.wstep) alone.
    void close_window_for_config();
    void open_window_for_config();
    // SetNovelMessageVoice1, from AVG_PlayVoice when test==0: the voice the
    // next message will be logged with.
    void set_novel_message_voice1(int sno, int vno, int cno, int a_cut);
    // What the renderer draws.  TXT_WINDOW's display flag is separate from
    // NovelMessage.disp: the log hides the line without taking the bar down.
    bool main_text_disp() const { return main_text_disp_; }
    const EngineText& log_text() const { return log_text_; }
    // The log entry on screen: its voiced spans, the one under the pointer
    // (-1 for none), and its rects again - after the presentation's own
    // layout of it has moved, say by scrolling.
    const std::array<std::pair<std::size_t, std::size_t>, nlog_v_max>&
    log_voice_spans() const { return novel_buf_.vv_span; }
    int log_voice_hover() const
    {
        return log_text_.flag && mouse_.no >= 32 ? mouse_.no - 32 : -1;
    }
    void refresh_log_voice_rects()
    {
        if (log_text_.flag) {
            set_history_voice_mouse_rect();
        }
    }
    const EngineText& log_voice_text() const { return log_voice_text_; }
    const HistoryBar& history_bar() const { return history_bar_; }
    const NovelBufState& novel_buf() const { return novel_buf_; }
    // A trace checkpoint carries all of it: bmax alone moves the handle.
    void write_history(std::ostream& out) const;
    bool read_history(std::istream& in);
    // NovelBuf alone - the log's entries and their voices - for a save.  A
    // save is made from the engine's config, whose mouse layer and rects
    // are no part of the scene it goes back to.
    void write_log(std::ostream& out) const;
    bool read_log(std::istream& in);
    // A new game: an empty log.
    void clear_log();

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

    // A frame drawn `phase` (0..1) of a tick after the last one.  The
    // typewriter's count moves on by msg_cnt every tick while it types, so
    // between ticks visible_glyphs and glyph_alpha read it that far along -
    // never past NovelMessage.max, where the next tick stops it - until the
    // phase is set back to 0.  Nothing the machine keeps is changed.
    void set_presentation_phase(double phase);
    // AVG_ControlHalfTone's brightness for GRP_BACK at the next tick's
    // count; false when the wash is not fading in.
    bool present_half_tone(double phase, int& r, int& g, int& b) const;
    // ControlHistorySystem's fade on its way to the next tick's value, from
    // the pointer as it stands.
    double present_bar_fade(double phase) const;
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
    double present_extra_ = 0.0;   // see set_presentation_phase
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

    int bar_press_ = -1;
    EngineMouse mouse_{};
    // MouseCheck[lno][no], and MouseStruct.lno.
    std::array<std::array<MouseRect, mouse_rest_max>, mouse_layers> rects_{};
    int mouse_layer_ = 0;
    NovelBufState novel_buf_{};
    bool main_text_disp_ = false;   // DSP_SetTextDisp( TXT_WINDOW, ... )
    EngineText log_text_{};         // TXT_WINDOW+1
    EngineText log_voice_text_{};   // TXT_WINDOW+2
    HistoryBar history_bar_{};      // GRP_HISTORY, and its static fade

    // TXT_GetTextCount( DSP_GetTextStr(TXT_WINDOW), step ).
    int text_count(int step) const { return txt_get_text_count(counted_, step); }
    // The MSG_WAIT / MSG_STOP body.
    void control_wait(const GameKey& key);
    // AVG_GetHitKey(): GameKey.click && (MUS_GetMouseNo(-1)==-1).
    bool hit_key() const;
    // MUS_GetMouseNoEx( MOUSE_LBTRIGGER / MOUSE_LBUTTON / -1, 0 ).
    int mouse_no_btrg() const;
    int mouse_no_trg() const;
    bool mouse_in_window() const;
    // The bar's buttons below the log ones, shared by MSG_NEXT (under a
    // choice) and MSG_WAIT / MSG_STOP: 3/4 save and load, 5 auto, 6 skip,
    // 8 config, 9 the half-tone slider.
    void bar_buttons(int command_trg, int command_btrg);
    void set_half_tone_from_slider();
    // The log itself.
    void novel_log_start();                 // AVG_NovelLogStart
    void set_novel_message_history(const std::string& str);
    void set_novel_message_voice2(bool top);
    void set_history_system_mouse_rect();   // SetHistorySystemMouseRect
    void reset_history_system_mouse_rect();
    void set_history_voice_mouse_rect();    // SetHistoryVoiceMouseRect
    void reset_history_voice_mouse_rect();
    // Layer 0, by slot and rect number (MUS_SetMouseRectAdd).
    void set_mouse_rect(int no, int sx, int sy, int w, int h, bool flag,
                        int rect_no);
    void reset_mouse_rect(int no) { rects_[0][static_cast<std::size_t>(no)] = {}; }
    // DSP_SetText( TXT_WINDOW+1, ..., buf ) with the log's settings.
    void set_log_text(bool disp, const std::string& str);
    void leave_log();                       // back to step2, bcount 0
    void control_log(const GameKey& key);   // MSG_LOG
    void control_history_system();          // ControlHistorySystem
    // Shared tail of every "the reader is ready" branch.
    void advance_step();
    int half_tone_depth() const {
        return hooks_.half_tone_depth ? hooks_.half_tone_depth() : 128;
    }
};

}  // namespace th2
