#include "avg_msg.hpp"

namespace th2 {

void AvgMsg::init()
{
    // InitNovelMessage: ZeroMemory over all three structs.
    message_ = {};
    half_tone_ = {};
    auto_count_ = 0;
    key_wait_count_ = 0;
    key_wait_count2_ = 0;
}

void AvgMsg::set_novel_message(int add_flag)
{
    // AVG_SetNovelMessage.  ZeroMemory( &NovelMessage, ... ) first, so count
    // and kstep go back to zero and the typewriter starts from the left.
    message_ = {};
    message_.flag = 1;
    message_.add_flag = add_flag;
    message_.disp = 1;
    message_.step1 = msg_disp;
    message_.max = hooks_.text_count(message_.kstep) + 8;

    hooks_.set_text_count(message_.count);
    hooks_.set_text_step(message_.kstep);

    hooks_.set_half_tone();
    set_novel_message_disp(true);
}

void AvgMsg::add_novel_message(int cr)
{
    // AVG_AddNovelMessage.  No ZeroMemory: the counters carry over, because
    // the new text is appended to the same window and the typewriter has to
    // pick up where it stopped.
    message_.flag = 1;
    message_.disp = 1;
    message_.step1 = msg_disp;
    message_.add_flag = cr;

    message_.kstep++;
    message_.count = message_.max;
    message_.max = hooks_.text_count(message_.kstep) + 8;
    hooks_.set_text_step(message_.kstep);
    hooks_.set_text_count(message_.count);
    hooks_.set_text_disp(true);

    hooks_.set_half_tone();
    set_novel_message_disp(true);
}

void AvgMsg::set_novel_message_disp(bool disp)
{
    // AVG_SetNovelMessageDisp.  It hides the *text*, not the window frame -
    // AVG_GetWindowCond reads a different state machine - which is what lets
    // AVG_ControlChar hide the text for the length of an animation and put it
    // back afterwards.
    message_.disp = disp;
    hooks_.set_text_disp(disp);
    if (!disp) {
        hooks_.reset_keywait();
    }
}

bool AvgMsg::wait_auto_mode()
{
    // AVG_WaitAutoMode.  The delay is the page one at a page end and the line
    // one everywhere else, and the counter is held down for as long as a
    // voice is playing, so auto mode waits out the line rather than cutting
    // it off.
    const bool wv = hooks_.wait_voice();
    const int wtime = (message_.step1 == msg_wait || message_.add_flag != 2)
        ? hooks_.auto_key()
        : hooks_.auto_page();

    if (!hooks_.auto_flag()) {
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
    hooks_.reset_keywait();
    if (message_.step1 == msg_wait) {
        message_.step1 = msg_disp;
        message_.kstep++;
        hooks_.reveal_step(message_.kstep);
        message_.max = hooks_.text_count(message_.kstep) + 8;
        hooks_.set_text_step(message_.kstep);
        hooks_.set_text_count(message_.count);
    } else {
        message_.step1 = msg_next;
        message_.kstep++;
    }
}

void AvgMsg::control_wait(const GameKey& key)
{
    // MSG_WAIT / MSG_STOP: the click indicator is up and the reader decides.
    if (message_.step1 == msg_wait || message_.add_flag != 2) {
        hooks_.set_keywait(false);
    } else {
        hooks_.set_keywait(true);
    }
    key_wait_count_ = key_wait_count_ + 1 > 10 ? 10 : key_wait_count_ + 1;
    key_wait_count2_ = key_wait_count2_ + 1 > 30 ? 30 : key_wait_count2_ + 1;

    if (key.diswin) {
        hooks_.play_se(9104, 255);
        message_.step2 = message_.step1;
        message_.step1 = msg_system;
        hooks_.reset_half_tone();
        set_novel_message_disp(false);
        hooks_.window_disp(false);
    } else if (key.pup) {
        hooks_.play_se(9012, 140);
        key_wait_count_ = 0;
        key_wait_count2_ = 0;
        hooks_.reset_keywait();
        hooks_.log_start();
    } else if (hooks_.hit_key() || hooks_.mes_cut() || wait_auto_mode()) {
        advance_step();
    }
}

void AvgMsg::control_novel_message(const GameKey& key)
{
    switch (message_.step1) {
    case msg_nodisp:
        break;

    case msg_next:
        // The instruction retires this frame; the script decides what is
        // next.  The original also runs the system bar here, which is ours.
        break;

    case msg_disp:
        // The typewriter.  count walks to max; max is the glyph count of the
        // current \k step plus eight, so it overshoots the step slightly and
        // the comparison below is against the whole string's count plus the
        // same eight.
        if (message_.count >= message_.max || hooks_.hit_key()
            || hooks_.mes_cut() || hooks_.msg_page()) {
            message_.count = message_.max;
            if (message_.count >= hooks_.text_count(-1) + 8) {
                // The whole message is on screen.  What happens now is the
                // add_flag the script gave SetMessage2.
                switch (message_.add_flag) {
                case 1:
                    if (hooks_.text_end_key_wait() && !hooks_.msg_page()) {
                        message_.step1 = msg_stop;
                        hooks_.set_text_count(-1);
                    } else if (hooks_.msg_page()) {
                        if (hooks_.wait_voice() || hooks_.hit_key()
                            || hooks_.mes_cut()) {
                            message_.step1 = msg_next;
                        } else {
                            message_.step1 = msg_disp;
                        }
                        hooks_.set_text_count(-1);
                    } else {
                        // No keywait at the end: the instruction retires
                        // immediately and an AddMessage2 follows without the
                        // reader having to click.
                        message_.step1 = msg_next;
                        hooks_.set_text_count(-1);
                    }
                    break;
                case 3:
                    message_.step1 = msg_stop;
                    hooks_.set_text_count(-1);
                    break;
                default:
                case 2:
                    message_.step1 = msg_stop;
                    message_.kstep = -1;
                    hooks_.set_text_count(-1);
                    hooks_.set_text_step(-1);
                    break;
                }
            } else if (hooks_.msg_page()) {
                message_.step1 = msg_disp;
                if (message_.kstep != -1) {
                    message_.kstep++;
                }
                hooks_.reveal_step(message_.kstep);
                message_.max = hooks_.text_count(message_.kstep) + 8;
                hooks_.set_text_step(message_.kstep);
            } else {
                // A \k in the middle: hold here until the reader clicks.
                message_.step1 = msg_wait;
                message_.count = hooks_.text_count(message_.kstep);
                hooks_.set_text_count(-1);
            }
        } else {
            message_.count += hooks_.msg_cnt();
            hooks_.set_text_count(message_.count);
        }
        break;

    case msg_system:
        // The window is hidden.  Any of the three keys puts it back, and the
        // state it goes back to is the one it left.
        if (key.cansel || key.diswin || key.click) {
            hooks_.set_half_tone();
            set_novel_message_disp(true);
            hooks_.window_disp(true);
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
        // The backlog.  The original drives it through these four states with
        // a drag bar and mouse rects; ours is a UI mode with its own input,
        // entered by hooks_.log_start() above and leaving the message state
        // machine parked where it was.  Nothing here drives the script, so
        // the transcription stops at the boundary rather than replacing a
        // working scrollback with a pixel-addressed one.
        break;

    case msg_wait2:
        break;

    default:
        break;
    }
}

}  // namespace th2
