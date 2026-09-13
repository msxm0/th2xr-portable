#include "avg_msg.hpp"

#include <algorithm>

namespace th2 {
namespace {

constexpr int DISP_X = 800;
constexpr int DISP_Y = 600;

}  // namespace

void AvgMsg::init()
{
    // InitNovelMessage: ZeroMemory over all three structs.
    message_ = {};
    half_tone_ = {};
    counted_ = {};
    raw_.clear();
    end_key_wait_ = false;
    auto_count_ = 0;
    key_wait_count_ = 0;
    key_wait_count2_ = 0;
}

// ---------------------------------------------------------- the messages --

void AvgMsg::set_novel_message(const std::string& raw, int add_flag)
{
    // void AVG_SetNovelMessage( char *str, int add_flag ).
    //
    //     ZeroMemory( &NovelMessage, sizeof(NOVEL_MESSEGE) );
    //     NovelMessage.flag = ON; ... step1 = MSG_DISP;
    //     NovelMessage.max  = TXT_GetTextCount( buf, NovelMessage.kstep )+8;
    //
    // The ZeroMemory is why count and kstep go back to zero and the
    // typewriter starts from the left.
    raw_ = raw;
    counted_ = txt_count_text(raw_);
    end_key_wait_ = txt_get_text_end_key_wait(raw_);

    message_ = {};
    message_.flag = 1;
    message_.add_flag = add_flag;
    message_.disp = 1;
    message_.step1 = msg_disp;
    message_.max = text_count(message_.kstep) + 8;

    set_half_tone();
    set_novel_message_disp(true);
}

void AvgMsg::add_novel_message(const std::string& raw, int cr)
{
    // void AVG_AddNovelMessage( char *str, int cr ).  No ZeroMemory: the
    // counters carry over, because the new text is appended to the same
    // window and the typewriter picks up where it stopped.
    //
    //     NovelMessage.kstep++;
    //     NovelMessage.count = NovelMessage.max;
    //     NovelMessage.max = TXT_GetTextCount( ..., NovelMessage.kstep ) + 8;
    raw_ = raw;
    counted_ = txt_count_text(raw_);
    end_key_wait_ = txt_get_text_end_key_wait(raw_);

    message_.flag = 1;
    message_.disp = 1;
    message_.step1 = msg_disp;
    message_.add_flag = cr;

    message_.kstep++;
    message_.count = message_.max;
    message_.max = text_count(message_.kstep) + 8;

    set_half_tone();
    set_novel_message_disp(true);
}

void AvgMsg::set_novel_message_disp(bool disp)
{
    // void AVG_SetNovelMessageDisp( int disp ).  It hides the *text*, not
    // the window frame - AVG_GetWindowCond reads a different state machine -
    // which is what lets AVG_ControlChar hide the text for the length of an
    // animation and put it back afterwards.
    message_.disp = disp ? 1 : 0;
    if (hooks_.set_text_disp) {
        hooks_.set_text_disp(disp);
    }
    if (!disp && hooks_.reset_keywait) {
        hooks_.reset_keywait();
    }
}

bool AvgMsg::wait_auto_mode()
{
    // BOOL AVG_WaitAutoMode( void ), verbatim.  The delay is the page one at
    // a page end and the line one everywhere else, and AutoCount is held
    // down for as long as a voice is playing, so auto mode waits the line
    // out rather than cutting it off.
    const bool wv = hooks_.wait_voice ? hooks_.wait_voice() : true;
    const int wtime = (message_.step1 == msg_wait || message_.add_flag != 2)
        ? (hooks_.auto_key ? hooks_.auto_key() : 60)
        : (hooks_.auto_page ? hooks_.auto_page() : 60);

    if (!(hooks_.auto_flag && hooks_.auto_flag())) {
        return false;
    }
    if (!wv) {
        auto_count_ = wtime;
    }
    auto_count_++;
    if (auto_count_ > wtime) {
        auto_count_ = 0;
        return wv;
    }
    return false;
}

void AvgMsg::advance_step()
{
    // The tail every "the reader is ready" branch shares.  MSG_WAIT is a \k
    // in the middle of a message, so it goes back to the typewriter with the
    // next step revealed; MSG_STOP is the end of it, so it goes to MSG_NEXT
    // and the instruction retires.
    reset_auto_mode();
    key_wait_count_ = 0;
    key_wait_count2_ = 0;
    if (hooks_.reset_keywait) {
        hooks_.reset_keywait();
    }
    if (message_.step1 == msg_wait) {
        message_.step1 = msg_disp;
        message_.kstep++;
        if (hooks_.reveal_step) {
            hooks_.reveal_step(message_.kstep);
        }
        message_.max = text_count(message_.kstep) + 8;
    } else {
        message_.step1 = msg_next;
        message_.kstep++;
    }
}

void AvgMsg::control_wait(const GameKey& key)
{
    // The MSG_WAIT / MSG_STOP body: the click indicator is up and the reader
    // decides.  GRP_KEYWAIT gets BMP_KEYWAIT+1 at a line end and
    // BMP_KEYWAIT+0 at a page end:
    //
    //     if(NovelMessage.step1==MSG_WAIT || NovelMessage.add_flag!=2)
    //         DSP_SetGraph( GRP_KEYWAIT, BMP_KEYWAIT+1, ... );
    //     else
    //         DSP_SetGraph( GRP_KEYWAIT, BMP_KEYWAIT+0, ... );
    if (hooks_.set_keywait) {
        const bool page =
            !(message_.step1 == msg_wait || message_.add_flag != 2);
        hooks_.set_keywait(page);
    }
    key_wait_count_ = std::min(key_wait_count_ + 1, 10);
    key_wait_count2_ = std::min(key_wait_count2_ + 1, 30);

    const bool hit = hooks_.hit_key && hooks_.hit_key();
    const bool cut = hooks_.mes_cut && hooks_.mes_cut();

    if (key.diswin) {
        if (hooks_.play_se) hooks_.play_se(9104, 255);
        message_.step2 = message_.step1;
        message_.step1 = msg_system;
        reset_half_tone();
        set_novel_message_disp(false);
    } else if (key.pup) {
        if (hooks_.play_se) hooks_.play_se(9012, 140);
        key_wait_count_ = 0;
        key_wait_count2_ = 0;
        if (hooks_.reset_keywait) hooks_.reset_keywait();
        if (hooks_.log_start) hooks_.log_start();
    } else if (hit || cut || wait_auto_mode()) {
        advance_step();
    }
}

void AvgMsg::control_novel_message(const GameKey& key)
{
    switch (message_.step1) {
    case msg_nodisp:
        break;

    case msg_next:
        // The instruction retires this frame.  The original runs the system
        // bar here; ours is a UI of its own.
        break;

    case msg_disp: {
        // The typewriter.  count walks to max; max is the count of the
        // current \k step plus eight, so it overshoots the step slightly and
        // the test below is against the whole string's count plus the same
        // eight.
        const bool hit = hooks_.hit_key && hooks_.hit_key();
        const bool cut = hooks_.mes_cut && hooks_.mes_cut();
        const bool page = hooks_.msg_page && hooks_.msg_page();

        if (message_.count >= message_.max || hit || cut || page) {
            message_.count = message_.max;
            if (message_.count >= text_count(-1) + 8) {
                // The whole message is on screen.  What happens now is the
                // add_flag the script gave SetMessage2.
                switch (message_.add_flag) {
                case 1:
                    if (end_key_wait_ && !page) {
                        message_.step1 = msg_stop;
                    } else if (page) {
                        if ((hooks_.wait_voice && hooks_.wait_voice())
                            || hit || cut) {
                            message_.step1 = msg_next;
                        } else {
                            message_.step1 = msg_disp;
                        }
                    } else {
                        // No keywait at the end: the instruction retires
                        // immediately and an AddMessage2 follows without the
                        // reader having to click.
                        message_.step1 = msg_next;
                    }
                    break;
                case 3:
                    message_.step1 = msg_stop;
                    break;
                default:
                case 2:
                    message_.step1 = msg_stop;
                    message_.kstep = -1;
                    break;
                }
            } else if (page) {
                message_.step1 = msg_disp;
                if (message_.kstep != -1) {
                    message_.kstep++;
                }
                if (hooks_.reveal_step) hooks_.reveal_step(message_.kstep);
                message_.max = text_count(message_.kstep) + 8;
            } else {
                // A \k in the middle: hold here until the reader clicks.
                message_.step1 = msg_wait;
                message_.count = text_count(message_.kstep);
            }
        } else {
            //     NovelMessage.count += AVG_MsgCnt();
            message_.count += hooks_.msg_cnt ? hooks_.msg_cnt() : 1;
        }
        break;
    }

    case msg_system:
        // The window is hidden.  Any of the three keys puts it back, and the
        // state it goes back to is the one it left.
        if (key.cansel || key.diswin || key.click) {
            set_half_tone();
            set_novel_message_disp(true);
            message_.step1 = message_.step2;
        }
        break;

    case msg_wait:
    case msg_stop:
        control_wait(key);
        break;

    case msg_up:
    case msg_down:
    case msg_drag:
    case msg_log:
        // The backlog.  The original drives it through these four states
        // with a drag bar and mouse rects; ours is a UI mode with its own
        // input, entered by hooks_.log_start() above and leaving the message
        // machine parked where it was.  Nothing here drives the script.
        break;

    case msg_wait2:
    default:
        break;
    }
}

// ----------------------------------------------------------- the half tone -

void AvgMsg::set_half_tone()
{
    // void AVG_SetHalfTone(void), verbatim.
    //
    // From TONE_NODISP it takes the darkened copy of the plate and starts
    // the ramp with the copy still hidden; called again while the ramp is
    // running, or once it is shown, it goes straight to TONE_DISP without
    // copying again.  That second path is why a second line of dialogue does
    // not re-darken an already-darkened background.
    const int tone = half_tone_depth();
    switch (half_tone_.tstep) {
    default:
    case tone_nodisp:
        half_tone_.tstep = tone_fadeout;
        half_tone_.tcount = 0;
        display_.copy_bmp2(
            bmp_backhalf, bmp_back,
            back_->r * tone / 128, back_->g * tone / 128,
            back_->b * tone / 128);
        display_.set_graph(
            grp_back + 1, bmp_backhalf, lay_back + 2, false, check_none);
        display_.set_graph_pos(
            grp_back + 1, 0, 0, back_->x, back_->y, DISP_X, DISP_Y);
        display_.set_graph_disp(grp_back, true);
        break;
    case tone_fadeout:
    case tone_disp:
        half_tone_.tstep = tone_disp;
        display_.set_graph(
            grp_back + 1, bmp_backhalf, lay_back + 2, true, check_none);
        display_.set_graph_pos(
            grp_back + 1, 0, 0, back_->x, back_->y, DISP_X, DISP_Y);
        display_.set_graph_disp(grp_back, false);
        break;
    }
}

void AvgMsg::set_half_tone_direct()
{
    // void AVG_SetHalfToneDirect(void): the config slider, which jumps
    // straight to the end of the ramp so the reader sees the depth they are
    // dragging.
    const int tone = half_tone_depth();
    half_tone_.tstep = tone_disp;
    half_tone_.tcount = 16;
    if (hooks_.set_char_half_tone) {
        hooks_.set_char_half_tone(true);
    }
    display_.copy_bmp2(
        bmp_backhalf, bmp_back,
        back_->r * tone / 128, back_->g * tone / 128, back_->b * tone / 128);
    display_.set_graph(
        grp_back + 1, bmp_backhalf, lay_back + 2, true, check_none);
    display_.set_graph_pos(
        grp_back + 1, 0, 0, back_->x, back_->y, DISP_X, DISP_Y);
    display_.set_graph_disp(grp_back, false);
}

void AvgMsg::reset_half_tone()
{
    // void AVG_ResetHalfTone(void), verbatim.  It drops the wash in one go -
    // the fade back in is commented out in the original and never runs.
    half_tone_.tstep = tone_nodisp;
    half_tone_.tcount = 0;
    display_.release_bmp(bmp_backhalf);
    display_.reset_graph(grp_back + 1);
    display_.set_graph_disp(grp_back, true);
    display_.set_graph_bright(grp_back, back_->r, back_->g, back_->b);
}

void AvgMsg::control_half_tone()
{
    // void AVG_ControlHalfTone(void), verbatim.  TONE_NODISP and TONE_DISP
    // are holds; TONE_FADEIN's body is commented out in the original.
    switch (half_tone_.tstep) {
    default:
    case tone_nodisp:
        half_tone_.tstep = tone_nodisp;
        break;
    case tone_disp:
        half_tone_.tstep = tone_disp;
        break;
    case tone_fadein:
        break;
    case tone_fadeout: {
        half_tone_.tcount += hooks_.eff_cnt_puls ? hooks_.eff_cnt_puls() : 1;
        if (half_tone_.tcount >= 16
            || (hooks_.wav_effect && hooks_.wav_effect())) {
            half_tone_.tstep = tone_disp;
            half_tone_.tcount = 16;
            // The ramp is over: show the pre-darkened copy and hide the
            // plate it was made from, rather than keep tinting.
            display_.set_graph_disp(grp_back + 1, true);
            display_.set_graph_disp(grp_back, false);
        } else {
            // Sixteen steps from the background's own brightness to the
            // dimmed one, so the wash arrives with the text rather than
            // snapping in behind it.
            const int tone = half_tone_depth();
            const int c = half_tone_.tcount;
            const int r = (c * back_->r * tone / 128 + (16 - c) * back_->r) / 16;
            const int g = (c * back_->g * tone / 128 + (16 - c) * back_->g) / 16;
            const int b = (c * back_->b * tone / 128 + (16 - c) * back_->b) / 16;
            display_.set_graph_bright(grp_back, r, g, b);
        }
        break;
    }
    }
}

// -------------------------------------------------------- for the renderer -

std::size_t AvgMsg::visible_glyphs() const
{
    if (message_.step1 == msg_nodisp) {
        return 0;
    }
    // DSP_SetTextCount( TXT_WINDOW, -1 ) draws the lot; otherwise it draws
    // up to NovelMessage.count.
    if (message_.step1 != msg_disp) {
        return counted_.glyph_count.size();
    }
    return txt_visible_glyphs(counted_, message_.count);
}

int AvgMsg::glyph_alpha(std::size_t index) const
{
    // alph2 = LIM( text_cnt - cnt2, 0, 16 ) * 16, so a character fades in
    // over the sixteen counts after its own.
    if (message_.step1 != msg_disp) {
        return 256;
    }
    if (index >= counted_.glyph_count.size()) {
        return 0;
    }
    const int delta = message_.count - counted_.glyph_count[index];
    return std::clamp(delta, 0, 16) * 16;
}

}  // namespace th2
