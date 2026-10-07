#include "avg_msg.hpp"

#include <algorithm>
#include <cmath>
#include <istream>
#include <ostream>

namespace th2 {
namespace {

constexpr int DISP_X = 800;
constexpr int DISP_Y = 600;

// GM_AvgMsg.cpp's HistorySystemRect / HistorySystemSrc tables, verbatim.
// Rect 0 is the log track, 1 and 2 log up and down, 3 save, 4 load, 5 auto,
// 6 skip, 7 hide the window, 8 config, 9 the half-tone slider.
constexpr int HistorySystemRectX[10] = {
    776, 776, 776, 776, 776, 776, 776, 776, 776, 776};
constexpr int HistorySystemRectY[10] = {
    10, 271, 312, 353, 353 + 23 * 1, 353 + 23 * 2, 353 + 23 * 3,
    353 + 23 * 4, 353 + 23 * 5, 492};
constexpr int HistorySystemRectW[10] = {
    22, 22, 22, 22, 22, 22, 22, 22, 22, 22};
constexpr int HistorySystemRectH[10] = {
    255, 36, 36, 20, 20, 20, 20, 20, 20, 98};
constexpr int HistorySystemSrcX[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
constexpr int HistorySystemSrcY[10] = {
    0, 36, 77, 118, 118 + 23, 118 + 23 * 2, 118 + 23 * 3, 118 + 23 * 4,
    118 + 23 * 5, 257};
constexpr int HistorySystemSrcW[10] = {
    22, 22, 22, 22, 22, 22, 22, 22, 22, 22};
constexpr int HistorySystemSrcH[10] = {
    30, 36, 36, 20, 20, 20, 20, 20, 20, 6};

// DSP_SetTextColor's indices into text.cpp's FCT: 10 is the log's text and
// 11 the voiced line under the pointer.
constexpr int log_text_color = 10;
constexpr int log_voice_color = 11;

std::size_t utf8_length(unsigned char lead)
{
    if (lead < 0x80) return 1;
    if ((lead >> 5) == 0x6) return 2;
    if ((lead >> 4) == 0xE) return 3;
    if ((lead >> 3) == 0x1E) return 4;
    return 1;
}

// How many bytes the character at `pos` is in CP932, which is what
// SetHistoryVoiceMouseRect counts in: one for ASCII and half-width kana,
// two for everything else.
int cp932_width(const std::string& s, std::size_t pos, std::size_t length)
{
    if (length == 1) {
        return 1;
    }
    if (length == 3) {
        const auto b0 = static_cast<unsigned char>(s[pos]);
        const auto b1 = static_cast<unsigned char>(s[pos + 1]);
        const auto b2 = static_cast<unsigned char>(s[pos + 2]);
        const int cp = ((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
        if (cp >= 0xFF61 && cp <= 0xFF9F) {
            return 1;
        }
    }
    return 2;
}

bool starts_at(const std::string& s, std::size_t pos, std::string_view what)
{
    return s.compare(pos, what.size(), what) == 0;
}

constexpr std::string_view close_quote = "\xE3\x80\x8D";   // 」

}  // namespace

void AvgMsg::init()
{
    // InitNovelMessage: ZeroMemory over all three structs.
    message_ = {};
    half_tone_ = {};
    counted_ = {};
    layout_ = {};
    raw_.clear();
    end_key_wait_ = false;
    auto_count_ = 0;
    key_wait_count_ = 0;
    key_wait_count2_ = 0;
    demo_cnt_ = 0;
    // ZeroMemory( &NovelBuf, sizeof(NOVEL_BUF) ).
    novel_buf_ = {};
    rects_ = {};
    mouse_layer_ = 0;
    main_text_disp_ = false;
    log_text_ = {};
    log_voice_text_ = {};
}

AvgMsg::SaveData AvgMsg::save_data() const
{
    // void AVG_SetSaveDataNovelMessage( AVG_SAVE_DATA *sdata ):
    //
    //     sdata->ms_flag  = NovelMessage.flag;
    //     sdata->ms_add   = NovelMessage.add_flag;
    //     sdata->ms_disp  = ON;
    //     sdata->ms_step1 = NovelMessage.step2;
    //     sdata->ms_count = NovelMessage.count;
    //     sdata->ms_kstep = NovelMessage.kstep;
    //     sdata->ms_max   = NovelMessage.max;
    //     if( AVG_GetSelectMessageFlag() )
    //         strncpy( buf, NovelBuf.buf[ (NovelBuf.bpoint-1+NLOG_MAX)%NLOG_MAX ], NLOG_BUF );
    //     else
    //         strncpy( buf, NovelMessage.str, NLOG_BUF );
    //     ... then buf into ms_str with every "\k" dropped.
    //
    // ms_disp is not kept: the load always passes ON.
    SaveData data;
    data.flag = message_.flag;
    data.add_flag = message_.add_flag;
    data.step = message_.step2;
    data.count = message_.count;
    data.kstep = message_.kstep;
    data.max = message_.max;
    const bool select_up =
        hooks_.select_message_flag && hooks_.select_message_flag();
    const std::string& source = select_up
        ? novel_buf_.buf[static_cast<std::size_t>(
              (novel_buf_.bpoint - 1 + nlog_max) % nlog_max)]
        : message_.str;
    data.str.reserve(source.size());
    for (std::size_t i = 0; i < source.size(); ++i) {
        if (source[i] == '\\' && i + 1 < source.size()
            && source[i + 1] == 'k') {
            ++i;
            continue;
        }
        data.str += source[i];
    }
    return data;
}

void AvgMsg::load_save_data(const SaveData& data)
{
    // void AVG_SetLoadDataNovelMessage( AVG_SAVE_DATA *sdata ):
    //
    //     ZeroMemory( &NovelBuf, sizeof(NOVEL_BUF) );
    //     ZeroMemory( &NovelMessage, sizeof(NOVEL_MESSEGE) );
    //     NovelMessage.flag     = sdata->ms_flag;
    //     NovelMessage.add_flag = sdata->ms_add;
    //     AVG_SetNovelMessageDisp( sdata->ms_disp );       // ON
    //     NovelMessage.step2  = NovelMessage.step1  = sdata->ms_step1;
    //     NovelMessage.max    = TXT_GetTextCount( sdata->ms_str, 0 )+8;
    //     NovelMessage.count  = NovelMessage.max;
    //     DSP_SetText( TXT_WINDOW, ..., sdata->ms_str ); ...
    //     DSP_SetTextCount( TXT_WINDOW, -1 );
    //     DSP_SetTextStep( TXT_WINDOW, 0 );
    //     if(sdata->ms_str[0]!='\0') SetNovelMessageHistory(sdata->ms_str);
    //
    // So the line comes back whole, with no typewriter, in the step it was
    // saved in - waiting for the reader if it was.
    novel_buf_ = {};
    message_ = {};
    message_.flag = data.flag;
    message_.add_flag = data.add_flag;
    set_novel_message_disp(true);
    message_.step1 = message_.step2 = data.step;
    restore_raw(data.str);
    end_key_wait_ = txt_get_text_end_key_wait(raw_);
    message_.max = text_count(0) + 8;
    message_.count = message_.max;
    text_cnt_ = -1;
    text_step_ = 0;
    if (!data.str.empty()) {
        set_novel_message_history(data.str);
    }
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
    layout_ = txt_count_text(raw_, message_text_box);
    end_key_wait_ = txt_get_text_end_key_wait(raw_);

    message_ = {};
    message_.flag = 1;
    message_.add_flag = add_flag;
    message_.disp = 1;
    message_.step1 = msg_disp;
    message_.max = text_count(message_.kstep) + 8;
    //     DSP_SetTextCount( TXT_WINDOW, NovelMessage.count );
    //     DSP_SetTextStep(  TXT_WINDOW, NovelMessage.kstep );
    text_cnt_ = message_.count;
    text_step_ = message_.kstep;

    //     AVG_SetHalfTone();
    //     SetNovelMessageHistory(buf);
    //     AVG_SetNovelMessageDisp( ON );
    //     SetNovelMessageVoice2(1);
    set_draw_flag_on();   // MainWindow.draw_flag=ON;
    set_half_tone();
    set_novel_message_history(raw_);
    set_novel_message_disp(true);
    set_novel_message_voice2(true);
}

void AvgMsg::add_novel_message(const std::string& raw, int cr)
{
    // void AVG_AddNovelMessage( char *str, int cr ).  No ZeroMemory: the
    // counters carry over, because the new text is appended to the same
    // window and the typewriter picks up where it stopped.
    //
    //     wsprintf( buf2, "\\k%s", buf1 );
    //     DSP_AddText( TXT_WINDOW, buf2 );
    //     NovelMessage.kstep++;
    //     NovelMessage.count = NovelMessage.max;
    //     NovelMessage.max = TXT_GetTextCount( ..., NovelMessage.kstep ) + 8;
    //
    // The text object *accumulates*, with a \k in front of each addition, and
    // every TXT_GetTextCount after this measures the whole of it.  Keeping
    // only the latest chunk here left NovelMessage.count - which carries over
    // - running off the end of the table: the line was cut short, the glyph
    // alphas were read at the wrong indices so already-shown text faded in
    // again, and the machine reached MSG_STOP early and put the click
    // indicator up in the middle of nowhere.
    //     SetNovelMessageVoice2(0);
    // first, while the log entry still ends where the new text will start:
    // that end is where the voice about to be logged is drawn from.
    set_novel_message_voice2(false);
    set_draw_flag_on();   // MainWindow.draw_flag=ON;
    //     sp = (NovelBuf.bpoint-1+NLOG_MAX)%NLOG_MAX;
    //     strcpy( NovelMessage.str, NovelBuf.buf[sp] );
    //     strcat( NovelBuf.buf[sp], buf2 );
    auto& log_entry = novel_buf_.buf[static_cast<std::size_t>(
        (novel_buf_.bpoint - 1 + nlog_max) % nlog_max)];
    message_.str = log_entry;
    log_entry += "\\k" + raw;
    raw_ += "\\k";
    raw_ += raw;
    counted_ = txt_count_text(raw_);
    layout_ = txt_count_text(raw_, message_text_box);
    end_key_wait_ = txt_get_text_end_key_wait(raw_);

    message_.flag = 1;
    message_.disp = 1;
    message_.step1 = msg_disp;
    message_.add_flag = cr;

    message_.kstep++;
    message_.count = message_.max;
    message_.max = text_count(message_.kstep) + 8;
    text_step_ = message_.kstep;
    text_cnt_ = message_.count;

    set_half_tone();
    set_novel_message_disp(true);
}

void AvgMsg::set_novel_message_disp(bool disp)
{
    // void AVG_SetNovelMessageDisp( int disp ).  It hides the *text*, not
    // the window frame - AVG_GetWindowCond reads a different state machine -
    // which is what lets AVG_ControlChar hide the text for the length of an
    // animation and put it back afterwards.
    //
    //     NovelMessage.disp  = disp;
    //     DSP_SetTextDisp( TXT_WINDOW, disp );
    //     if(disp){ SetHistorySystemMouseRect(); }
    //     else    { DSP_ResetGraph( GRP_KEYWAIT ); ResetHistorySystemMouseRect(); }
    message_.disp = disp ? 1 : 0;
    main_text_disp_ = disp;
    if (hooks_.set_text_disp) {
        hooks_.set_text_disp(disp);
    }
    if (disp) {
        set_history_system_mouse_rect();
    } else {
        if (hooks_.reset_keywait) {
            hooks_.reset_keywait();
        }
        reset_history_system_mouse_rect();
    }
}

void AvgMsg::open_window(bool tdisp)
{
    // void AVG_OpenWindow( int tdisp, int flag ):
    //
    //     AVG_SetNovelMessageDisp( ON );
    //     NovelMessage.step1 = NovelMessage.step2;
    //     for(i=0;i<10;i++) DSP_SetGraphDisp( GRP_WINDOW+i, ON );
    //     if(tdisp) for(i=0;i<5;i++) DSP_SetTextDisp( TXT_WINDOW+i, ON );
    //     for( i=0; i<MAX_SCRIPT_OBJ ; i++ )
    //         if(SpriteBmp[i].disp) DSP_SetGraphDisp( GRP_SCRIPT+i, ON );
    wstep_ = 1;
    if (tdisp) {
        set_novel_message_disp(true);
        log_text_.disp = true;
        log_voice_text_.disp = true;
    }
    message_.step1 = message_.step2;
    if (hooks_.script_objects_disp) {
        hooks_.script_objects_disp(true);
    }
}

void AvgMsg::close_window()
{
    // void AVG_CloseWindow( int flag ):
    //
    //     AVG_SetNovelMessageDisp( OFF );
    //     NovelMessage.step2 = NovelMessage.step1;
    //     NovelMessage.step1 = MSG_NODISP;
    //     ...hide GRP_WINDOW, GRP_SCRIPT and TXT_WINDOW...
    //
    // Parking step1 is the part that matters: the typewriter stops where it
    // is for the length of whatever closed the window, and open_window puts
    // it back on exactly the character it had reached.
    set_novel_message_disp(false);
    log_text_.disp = false;
    log_voice_text_.disp = false;
    message_.step2 = message_.step1;
    message_.step1 = msg_nodisp;
    wstep_ = 0;
    if (hooks_.script_objects_disp) {
        hooks_.script_objects_disp(false);
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
        text_step_ = message_.kstep;
        text_cnt_ = message_.count;
    } else {
        message_.step1 = msg_next;
        message_.kstep++;
        if (hooks_.page_end) {
            hooks_.page_end();
        }
    }
}

void AvgMsg::control_wait(const GameKey& key)
{
    // The MSG_WAIT / MSG_STOP body.  The whole thing is wrapped in
    //     if( Avg.demo ){ ...counter... }else{ ...the reader... }
    // so the attract loop never shows a click indicator and never reads the
    // keyboard - it just counts Avg.demo_max frames and moves on.
    if (hooks_.demo && hooks_.demo()) {
        demo_cnt_++;
        const int demo_max = hooks_.demo_max ? hooks_.demo_max() : 60;
        if (demo_cnt_ >= demo_max) {
            demo_cnt_ = 0;
            advance_step();
        }
        return;
    }
    // GRP_KEYWAIT gets BMP_KEYWAIT+1 at a line end and BMP_KEYWAIT+0 at a
    // page end:
    //
    //     if(NovelMessage.step1==MSG_WAIT || NovelMessage.add_flag!=2)
    //         DSP_SetGraph( GRP_KEYWAIT, BMP_KEYWAIT+1, ... );
    //     else
    //         DSP_SetGraph( GRP_KEYWAIT, BMP_KEYWAIT+0, ... );
    //
    // All of it, and the two counters, inside if( !AVG_GetLaodFlag() ): the
    // line a load brings back waits under the load's fade with no mark up.
    if (!(hooks_.load_flag && hooks_.load_flag())) {
        if (hooks_.set_keywait) {
            const bool page =
                !(message_.step1 == msg_wait || message_.add_flag != 2);
            hooks_.set_keywait(page);
        }
        //     DSP_GetTextDispPos( TXT_WINDOW, &px, &py );
        if (hooks_.measure_text) {
            hooks_.measure_text();
        }
        key_wait_count_ = std::min(key_wait_count_ + 1, 10);
        key_wait_count2_ = std::min(key_wait_count2_ + 1, 30);
    }

    // Read once, at the top, as the original's two locals are:
    //     command_btrg = MUS_GetMouseNoEx(MOUSE_LBTRIGGER,0);
    //     command_trg  = MUS_GetMouseNoEx(MOUSE_LBUTTON,0);
    const int command_trg = mouse_no_trg();
    const int command_btrg = mouse_no_btrg();
    const bool hit = hit_key();
    const bool cut = hooks_.mes_cut && hooks_.mes_cut();

    if (key.diswin || command_trg == 7) {
        if (hooks_.play_se) hooks_.play_se(9104, 255);
        message_.step2 = message_.step1;
        message_.step1 = msg_system;
        reset_half_tone();
        set_novel_message_disp(false);
    } else if (key.pup || command_btrg == 1) {
        if (hooks_.play_se) hooks_.play_se(9012, 140);
        if (novel_buf_.bmax - 1 > 0) {
            key_wait_count_ = 0;
            key_wait_count2_ = 0;
            if (hooks_.reset_keywait) hooks_.reset_keywait();
            novel_log_start();
        }
    } else if ((hit && mouse_.x < DISP_X - 32) || cut || wait_auto_mode()) {
        // The original's condition has four more terms, all off here:
        //     ( GameKey.pdown && Avg.tx_pdwon )
        //     ( (KeyCond.btrg.space || KeyCond.btrg.enter) && Avg.tx_lbtrg )
        //     ( MUS_GetMouseBTrigger(MOUSE_LBUTTON) && Avg.tx_lbtrg && ... )
        //     ( MainWindow.save_disp && key_wait_count2==30 )
        // tx_pdwon and tx_lbtrg are menu options that default to off and
        // CONFIG.ini does not set, and save_disp is the window's screenshot
        // mode.  The x<768 guard is what stops a click on the bar from also
        // turning the page.
        advance_step();
    } else if (command_btrg == 0) {
        // A press on the log track: MSG_DRAG, with the entry before this one
        // loaded into TXT_WINDOW+1 but not yet shown - the drag decides.
        message_.step2 = message_.step1;
        message_.step1 = msg_drag;
        key_wait_count_ = 0;
        key_wait_count2_ = 0;
        if (hooks_.reset_keywait) hooks_.reset_keywait();
        set_log_text(false, novel_buf_.buf[static_cast<std::size_t>(
            (novel_buf_.bpoint - 2 + nlog_max) % nlog_max)]);
    } else {
        bar_buttons(command_trg, command_btrg);
    }
}

void AvgMsg::bar_buttons(int command_trg, int command_btrg)
{
    // The tail of the else-if chain in MSG_NEXT and MSG_WAIT / MSG_STOP.
    const bool omake = hooks_.omake && hooks_.omake();
    if (command_trg == 3 && !omake) {
        if (hooks_.play_se) hooks_.play_se(9104, 255);
        if (hooks_.go_config) hooks_.go_config(1);
    } else if (command_trg == 4 && !omake) {
        if (hooks_.play_se) hooks_.play_se(9104, 255);
        if (hooks_.go_config) hooks_.go_config(2);
    } else if (command_trg == 5) {
        if (hooks_.play_se) hooks_.play_se(9104, 255);
        if (hooks_.toggle_auto_flag) hooks_.toggle_auto_flag();
    } else if (command_trg == 6) {
        if (hooks_.play_se) hooks_.play_se(9104, 255);
        if (hooks_.set_msg_cut_mode) {
            if (hooks_.msg_cut_offered && hooks_.msg_cut_offered()) {
                hooks_.set_msg_cut_mode(
                    !(hooks_.msg_cut_mode && hooks_.msg_cut_mode()));
            } else {
                hooks_.set_msg_cut_mode(false);
            }
        }
    } else if (command_trg == 8) {
        if (hooks_.play_se) hooks_.play_se(9104, 255);
        if (hooks_.go_config) hooks_.go_config(3);
    } else if (command_btrg == 9) {
        set_half_tone_from_slider();
    }
}

void AvgMsg::set_half_tone_from_slider()
{
    //     my = MUS_GetMousePosY()-HistorySystemRectY[9];
    //     Avg.half_tone = 128-LIM(my-HistorySystemSrcH[9]/2,0,
    //                            HistorySystemRectH[9]-HistorySystemSrcH[9]);
    //     AVG_SetHalfToneDirect();
    const int my = mouse_.y - HistorySystemRectY[9];
    const int half_tone = 128
        - std::clamp(my - HistorySystemSrcH[9] / 2, 0,
                     HistorySystemRectH[9] - HistorySystemSrcH[9]);
    if (hooks_.set_half_tone_depth) {
        hooks_.set_half_tone_depth(half_tone);
    }
    set_half_tone_direct();
}

// ------------------------------------------------------------ the mouse --

void AvgMsg::renew_mouse(int x, int y, bool left)
{
    // MUS_RenewMouse, the parts a trace can reach.  MouseStruct.active is
    // taken as always on: the reference's window never loses focus.
    bar_press_ = -1;
    mouse_.x = x;
    mouse_.y = y;
    // rect_err = !(0 < mx && 0 < my && disp_w >= mx && disp_h >= my);
    const bool rect_err = !(0 < x && 0 < y && x <= DISP_X && y <= DISP_Y);
    mouse_.no = -1;
    if (!rect_err && mouse_layer_ >= 0 && mouse_layer_ < mouse_layers) {
        for (const auto& rect :
             rects_[static_cast<std::size_t>(mouse_layer_)]) {
            if (rect.flag && rect.sx <= x && rect.sy <= y
                && rect.sx + rect.w > x && rect.sy + rect.h > y) {
                mouse_.no = rect.rect_no;
                break;
            }
        }
    }
    //     lcnt = (MouseStruct.bl) ? min(15,lcnt+1) : 0;
    //     MouseStruct.btl = (lcnt==15) || (lcnt==1);
    //     if(bl==OFF && MouseStruct.bl==ON ) MouseStruct.tl = ON; ...
    mouse_.lcnt = left ? std::min(15, mouse_.lcnt + 1) : 0;
    mouse_.btl = mouse_.lcnt == 15 || mouse_.lcnt == 1;
    mouse_.tl = !mouse_.bl && left;
    mouse_.bl = left;
}

bool AvgMsg::mouse_in_window() const
{
    // The guard MUS_GetMouseButton / Trigger / BTrigger all share - note
    // the half-open bounds, which are not rect_err's.
    return 0 <= mouse_.x && mouse_.x < DISP_X
        && 0 <= mouse_.y && mouse_.y < DISP_Y;
}

int AvgMsg::mouse_no_ex(int button, int lno) const
{
    // MUS_GetMouseNoEx, and MUS_GetMouseNo inside it:
    //     if( MOUSE_LBTRIGGER<=button ){ if( MUS_GetMouseBTrigger(button-8) ) ... }
    //     else{ if( MUS_GetMouseTrigger( button ) ) ... }
    if (lno != mouse_layer_) {
        return -1;
    }
    if (button == mouse_any) {
        return mouse_.no;
    }
    const bool edge = button >= mouse_lbtrigger ? mouse_.btl : mouse_.tl;
    return mouse_in_window() && edge ? mouse_.no : -1;
}

int AvgMsg::mouse_no_btrg() const
{
    return mouse_no_ex(mouse_lbtrigger, 0);
}

int AvgMsg::mouse_no_trg() const
{
    if (bar_press_ >= 0) {
        return bar_press_;
    }
    return mouse_no_ex(mouse_lbutton, 0);
}

void AvgMsg::set_mouse_pos_rect(int lno, int no)
{
    //     if( no < 0) return ;
    //     if(!MouseCheck[lno][no].flag)return ;
    //     x = MouseCheck[lno][no].sx;
    //     y = MouseCheck[lno][no].sy + MouseCheck[lno][no].h-1;
    //     MUS_SetMousePos( (HWND)hwnd, x, y );
    if (lno < 0 || lno >= mouse_layers || no < 0 || no >= mouse_rest_max) {
        return;
    }
    const auto& rect =
        rects_[static_cast<std::size_t>(lno)][static_cast<std::size_t>(no)];
    if (!rect.flag) {
        return;
    }
    if (hooks_.set_mouse_pos) {
        hooks_.set_mouse_pos(rect.sx, rect.sy + rect.h - 1);
    }
}

void AvgMsg::set_mouse_rect(int lno, int no, int sx, int sy, int w, int h,
                            bool flag)
{
    if (lno < 0 || lno >= mouse_layers || no < 0 || no >= mouse_rest_max) {
        return;
    }
    rects_[static_cast<std::size_t>(lno)][static_cast<std::size_t>(no)] =
        {flag, sx, sy, w, h, no};
}

void AvgMsg::set_mouse_rect_flag(int lno, int no, bool flag)
{
    if (lno < 0 || lno >= mouse_layers || no < 0 || no >= mouse_rest_max) {
        return;
    }
    rects_[static_cast<std::size_t>(lno)][static_cast<std::size_t>(no)]
        .flag = flag;
}

void AvgMsg::reset_mouse_rect_layer(int lno)
{
    if (lno < 0 || lno >= mouse_layers) {
        return;
    }
    rects_[static_cast<std::size_t>(lno)] = {};
}

void AvgMsg::close_window_for_config()
{
    // void AVG_CloseWindow( int flag ): the text goes, the machine parks at
    // MSG_NODISP remembering where it was, and the script's overlays hide.
    set_novel_message_disp(false);
    message_.step2 = message_.step1;
    message_.step1 = msg_nodisp;
    if (hooks_.script_objects_disp) {
        hooks_.script_objects_disp(false);
    }
    main_text_disp_ = false;
    log_text_.disp = false;
    log_voice_text_.disp = false;
}

void AvgMsg::open_window_for_config()
{
    // void AVG_OpenWindow( int tdisp, int flag ), with tdisp ON.
    set_novel_message_disp(true);
    message_.step1 = message_.step2;
    main_text_disp_ = true;
    log_text_.disp = true;
    log_voice_text_.disp = true;
    if (hooks_.script_objects_disp) {
        hooks_.script_objects_disp(true);
    }
}

bool AvgMsg::hit_key() const
{
    // BOOL AVG_GetHitKey( void ):
    //     return GameKey.click && (MUS_GetMouseNo(-1)==-1);
    // A click on any registered rect - the bar's buttons, a voiced line in
    // the log - is that rect's, not the page's.
    return hooks_.hit_key && hooks_.hit_key() && mouse_.no == -1;
}

void AvgMsg::set_mouse_rect(int no, int sx, int sy, int w, int h, bool flag,
                            int rect_no)
{
    // MUS_SetMouseRect / MUS_SetMouseRectAdd on layer 0.
    if (no < 0 || no >= mouse_rest_max) {
        return;
    }
    rects_[0][static_cast<std::size_t>(no)] = {flag, sx, sy, w, h, rect_no};
}

void AvgMsg::set_history_system_mouse_rect()
{
    // void SetHistorySystemMouseRect( void ), verbatim.
    if (hooks_.side_option && hooks_.side_option() == 3) {
        return;
    }
    const bool omake = hooks_.omake && hooks_.omake();
    for (int i = 0; i < 10; ++i) {
        const bool flag = (i == 3 || i == 4) ? !omake : true;
        set_mouse_rect(i, HistorySystemRectX[i], HistorySystemRectY[i],
                       HistorySystemRectW[i], HistorySystemRectH[i], flag, i);
    }
}

void AvgMsg::reset_history_system_mouse_rect()
{
    for (int i = 0; i < 10; ++i) {
        reset_mouse_rect(i);
    }
}

// -------------------------------------------------------------- the log --

void AvgMsg::set_novel_message_history(const std::string& str)
{
    // void SetNovelMessageHistory( char *str ), verbatim.
    auto& nb = novel_buf_;
    nb.nv[static_cast<std::size_t>(nb.bpoint)].clear();
    nb.buf[static_cast<std::size_t>(nb.bpoint)] = str;
    nb.bpoint = (nb.bpoint + 1) % nlog_max;
    nb.bmax = std::min(nlog_max, nb.bmax + 1);
}

void AvgMsg::set_novel_message_voice1(int sno, int vno, int cno, int a_cut)
{
    novel_buf_.sno = sno;
    novel_buf_.vno = vno;
    novel_buf_.cno = cno;
    novel_buf_.a_cut = a_cut;
}

void AvgMsg::set_novel_message_voice2(bool top)
{
    // void SetNovelMessageVoice2( int top ).  The voice last played is
    // logged against the newest entry, at the place its text starts: the top
    // of the window for a fresh message, or wherever the entry's text ends
    // for an AddMessage2 - measured by drawing it, as the original does.
    auto& nb = novel_buf_;
    const auto cnt =
        static_cast<std::size_t>((nb.bpoint - 1 + nlog_max) % nlog_max);
    if (!nb.sno) {
        nb.sno = 0;
        nb.vno = 0;
        nb.cno = 0;
        return;
    }
    NovelVoice voice{nb.sno, nb.vno, nb.cno, nb.a_cut, 0, 0, 0};
    nb.sno = 0;
    nb.vno = 0;
    nb.cno = 0;
    if (top) {
        voice.px = message_text_box.sx;
        voice.py = message_text_box.sy;
        voice.vstcount = 0;
    } else {
        //     TXT_DrawText( NULL, 0, MES_POS_X, MES_POS_Y, MES_POS_W,
        //                   MES_POS_H, ..., &px, &py, ..., NovelBuf.buf[cnt],
        //                   0, -1, -1, ... );
        const auto layout = txt_count_text(nb.buf[cnt], message_text_box);
        int px = message_text_box.sx;
        int py = message_text_box.sy;
        txt_cursor_after(layout, message_text_box, layout.glyph_x.size(),
                         &px, &py);
        voice.px = px;
        voice.py = py;
        voice.vstcount = static_cast<int>(nb.buf[cnt].size());
    }
    if (nb.nv[cnt].size() < static_cast<std::size_t>(nlog_v_max)) {
        nb.nv[cnt].push_back(voice);
    }
}

void AvgMsg::reset_history_voice_mouse_rect()
{
    for (int i = 0; i < nlog_v_max; ++i) {
        reset_mouse_rect(32 + i);
    }
}

void AvgMsg::set_history_voice_mouse_rect()
{
    // void SetHistoryVoiceMouseRect( void ), for the entry bcount back.
    //
    // Each voiced line gets rects 32+m with rect_no 32+i, sized in the
    // original's own units: seventeen pixels a CP932 byte and 34+18 tall,
    // which is the Japanese release's 34px font.  The English release kept
    // them when it shrank the font to 24, and so does this - it is what the
    // reference answers a click with.
    auto& nb = novel_buf_;
    const auto cnt = static_cast<std::size_t>(
        (nb.bpoint - 1 - nb.bcount + nlog_max) % nlog_max);
    const std::string& text = nb.buf[cnt];
    const auto& voices = nb.nv[cnt];
    const int mes_x = message_text_box.sx;
    const int pich_h = message_text_box.pich_h;

    int m = 0;
    std::size_t i = 0;
    for (; i < voices.size(); ++i) {
        const auto& voice = voices[i];
        const auto start = static_cast<std::size_t>(voice.vstcount);
        // j counts CP932 bytes up to the closing bracket, the next \k or
        // \n; n counts the escape bytes among them, which take no room.
        int j = 0;
        int n = 0;
        std::size_t end = text.size();
        for (std::size_t pos = start; pos < text.size();) {
            if (starts_at(text, pos, close_quote)) {
                end = pos + close_quote.size();
                break;
            }
            if (starts_at(text, pos, "\\k") || starts_at(text, pos, "\\n")) {
                n += 2;
                if (j != 0) {
                    end = pos;
                    break;
                }
            }
            const auto length = std::min(
                utf8_length(static_cast<unsigned char>(text[pos])),
                text.size() - pos);
            j += cp932_width(text, pos, length);
            pos += length;
        }
        if (i < nb.vv_span.size()) {
            nb.vv_span[i] = {start, end};
        }
        std::vector<std::array<int, 4>> measured;
        if (hooks_.log_voice_rects
            && hooks_.log_voice_rects(text, start, end, measured)) {
            for (const auto& r : measured) {
                set_mouse_rect(32 + m++, r[0], r[1], r[2], r[3], true,
                               32 + static_cast<int>(i));
            }
        } else {
            int vwork = voice.px + 17 * (j - n);
            int vpx = voice.px;
            int vpy = voice.py;
            set_mouse_rect(32 + m++, vpx, vpy - 9, 17 * j, 34 + 18, true,
                           32 + static_cast<int>(i));
            vwork -= 34 * 20;
            while (vwork > 0) {
                vpx = mes_x;
                vpy += pich_h + 34;
                set_mouse_rect(32 + m++, vpx, vpy - 9, vwork, 34 + 18, true,
                               32 + static_cast<int>(i));
                vwork -= 34 * 20;
            }
        }

        // vv_mes: as many spaces as the line starts cells in, then the
        // voiced text up to and including its closing bracket.  Unlike the
        // measuring loop this one does not stop at \n.
        std::string mes(static_cast<std::size_t>(
                            std::max(0, (voice.px - mes_x) / 17)), ' ');
        std::size_t copied = 0;
        for (std::size_t pos = start; pos < text.size();) {
            const auto length = std::min(
                utf8_length(static_cast<unsigned char>(text[pos])),
                text.size() - pos);
            if (starts_at(text, pos, close_quote)) {
                mes.append(text, pos, length);
                break;
            }
            if (starts_at(text, pos, "\\k") && copied != 0) {
                break;
            }
            mes.append(text, pos, length);
            copied += length;
            pos += length;
        }
        if (i < nb.vv_mes.size()) {
            nb.vv_mes[i] = std::move(mes);
        }
    }
    for (; i < nb.vv_mes.size(); ++i) {
        nb.vv_mes[i].clear();
        nb.vv_span[i] = {0, 0};
    }
    for (; m < nlog_v_max; ++m) {
        reset_mouse_rect(32 + m);
    }
}

void AvgMsg::set_log_text(bool disp, const std::string& str)
{
    //     DSP_SetText( TXT_WINDOW+1, LAY_WINDOW+1, SYS_FONT, disp, str );
    //     DSP_SetTextPos( TXT_WINDOW+1, MES_POS_X, MES_POS_Y, ... );
    //     DSP_SetTextColor( TXT_WINDOW+1, 10 );
    log_text_ = {true, disp, str, message_text_box.sx, message_text_box.sy,
                 log_text_color};
}

void AvgMsg::novel_log_start()
{
    // BOOL AVG_NovelLogStart( void ), verbatim.
    if (message_.step1 == msg_next || message_.step1 == msg_wait
        || message_.step1 == msg_stop) {
        message_.step2 = message_.step1;
        message_.step1 = msg_up;
        main_text_disp_ = false;
        set_log_text(true, novel_buf_.buf[static_cast<std::size_t>(
            (novel_buf_.bpoint - 2 + nlog_max) % nlog_max)]);
    }
}

void AvgMsg::leave_log()
{
    // The way out of the log that every state shares:
    //     ResetHistoryVoiceMouseRect();
    //     DSP_ResetText( TXT_WINDOW+1 );
    //     DSP_SetTextDisp( TXT_WINDOW, ON );
    //     AVG_OpenSelectWindow();
    //     NovelBuf.bcount=0;
    //     NovelMessage.step1 = NovelMessage.step2;
    reset_history_voice_mouse_rect();
    log_text_ = {};
    main_text_disp_ = true;
    if (hooks_.open_select_window) hooks_.open_select_window();
    novel_buf_.bcount = 0;
    message_.step1 = message_.step2;
}

void AvgMsg::control_log(const GameKey& key)
{
    // case MSG_LOG.
    auto& nb = novel_buf_;
    const int command_btrg = mouse_no_btrg();
    if (mouse_.no >= 32) {
        const auto mno = static_cast<std::size_t>(mouse_.no - 32);
        const auto cnt = static_cast<std::size_t>(
            (nb.bpoint - 1 - nb.bcount + nlog_max) % nlog_max);
        const auto& voices = nb.nv[cnt];
        const int py = mno < voices.size() ? voices[mno].py : 0;
        log_voice_text_ = {true, true,
                           mno < nb.vv_mes.size() ? nb.vv_mes[mno] : "",
                           message_text_box.sx, py, log_voice_color};
        if (key.click && mno < voices.size() && hooks_.log_voice) {
            const auto& voice = voices[mno];
            hooks_.log_voice(voice.cno, voice.sno, voice.vno, voice.a_cut);
        }
    } else {
        log_voice_text_ = {};
    }
    if (hit_key() || key.cansel) {
        log_voice_text_ = {};
        leave_log();
    } else if (key.pup || command_btrg == 1) {
        log_voice_text_ = {};
        if (hooks_.play_se) hooks_.play_se(9012, 140);
        message_.step1 = msg_up;
    } else if (key.pdown || command_btrg == 2) {
        log_voice_text_ = {};
        if (hooks_.play_se) hooks_.play_se(9012, 140);
        message_.step1 = msg_down;
    } else if (command_btrg == 0) {
        key_wait_count_ = 0;
        if (hooks_.reset_keywait) hooks_.reset_keywait();
        message_.step1 = msg_drag;
    } else if (command_btrg == 9) {
        set_half_tone_from_slider();
    }
}

void AvgMsg::control_novel_message(const GameKey& key)
{
    switch (message_.step1) {
    case msg_nodisp:
        break;

    case msg_next:
        // The instruction retires this frame - unless a choice is up under
        // the message, when this is where the reader sits and the bar is
        // answered from here.
        if (hooks_.select_message_flag && hooks_.select_message_flag()) {
            const int command_trg = mouse_no_trg();
            const int command_btrg = mouse_no_btrg();
            if (key.diswin || command_trg == 7) {
                if (hooks_.play_se) hooks_.play_se(9104, 255);
                message_.step2 = message_.step1;
                message_.step1 = msg_system;
                reset_half_tone();
                set_novel_message_disp(false);
                if (hooks_.close_select_window) hooks_.close_select_window();
            } else if (key.pup || command_btrg == 1) {
                if (hooks_.play_se) hooks_.play_se(9012, 140);
                if (hooks_.close_select_window) hooks_.close_select_window();
                if (novel_buf_.bmax - 1 > 0) {
                    if (hooks_.reset_keywait) hooks_.reset_keywait();
                    novel_log_start();
                }
            } else if (command_btrg == 0) {
                if (hooks_.close_select_window) hooks_.close_select_window();
                message_.step2 = message_.step1;
                message_.step1 = msg_drag;
                if (hooks_.reset_keywait) hooks_.reset_keywait();
                set_log_text(false, novel_buf_.buf[static_cast<std::size_t>(
                    (novel_buf_.bpoint - 2 + nlog_max) % nlog_max)]);
            } else {
                bar_buttons(command_trg, command_btrg);
            }
        }
        break;

    case msg_disp: {
        // The typewriter.  count walks to max; max is the count of the
        // current \k step plus eight, so it overshoots the step slightly and
        // the test below is against the whole string's count plus the same
        // eight.
        const bool hit = hit_key();
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
                        text_cnt_ = -1;
                    } else if (page) {
                        if ((hooks_.wait_voice && hooks_.wait_voice())
                            || hit || cut) {
                            message_.step1 = msg_next;
                            text_cnt_ = -1;
                        } else {
                            message_.step1 = msg_disp;
                            text_cnt_ = -1;
                        }
                    } else {
                        // No keywait at the end: the instruction retires
                        // immediately and an AddMessage2 follows without the
                        // reader having to click.
                        message_.step1 = msg_next;
                        text_cnt_ = -1;
                    }
                    break;
                case 3:
                    message_.step1 = msg_stop;
                    text_cnt_ = -1;
                    break;
                default:
                case 2:
                    message_.step1 = msg_stop;
                    message_.kstep = -1;
                    text_cnt_ = -1;
                    text_step_ = -1;
                    break;
                }
            } else if (page) {
                message_.step1 = msg_disp;
                if (message_.kstep != -1) {
                    message_.kstep++;
                }
                if (hooks_.reveal_step) hooks_.reveal_step(message_.kstep);
                message_.max = text_count(message_.kstep) + 8;
                text_step_ = message_.kstep;
                text_cnt_ = message_.count;
            } else {
                // A \k in the middle: hold here until the reader clicks.
                // The text object goes to -1, so everything up to the \k is
                // drawn solid while the reader is waited on - the stop at
                // the \k itself is text_step_, which stays where it was.
                message_.step1 = msg_wait;
                message_.count = text_count(message_.kstep);
                text_cnt_ = -1;
            }
        } else {
            //     if( Avg.demo ) NovelMessage.count+=(short)(2*30/Avg.frame);
            //     else           NovelMessage.count+=AVG_MsgCnt();
            // The attract loop reads at a fixed rate rather than the
            // reader's text-speed setting.
            if (hooks_.demo && hooks_.demo()) {
                message_.count += 2 * 30 / 60;
            } else {
                message_.count += hooks_.msg_cnt ? hooks_.msg_cnt() : 1;
            }
            //     DSP_SetTextCount( TXT_WINDOW, NovelMessage.count );
            // The typing path is the one place the two are kept in step.
            text_cnt_ = message_.count;
        }
        break;
    }

    case msg_system:
        // The window is hidden.  Any of the three keys puts it back, and the
        // state it goes back to is the one it left.
        if (key.cansel || key.diswin || key.click) {
            set_half_tone();
            set_novel_message_disp(true);
            if (hooks_.open_select_window) hooks_.open_select_window();
            message_.step1 = message_.step2;
        }
        break;

    case msg_wait:
    case msg_stop:
        control_wait(key);
        break;

    // The log.  Nothing in these states drives the script.
    case msg_log:
        control_log(key);
        break;

    case msg_up: {
        auto& nb = novel_buf_;
        nb.bcount = std::min(nb.bcount + 1, nb.bmax - 1);
        if (nb.bcount == 0) {
            leave_log();
        } else {
            set_history_voice_mouse_rect();
            log_text_.str = nb.buf[static_cast<std::size_t>(
                (nb.bpoint - 1 - nb.bcount + nlog_max) % nlog_max)];
            message_.step1 = msg_log;
        }
        break;
    }

    case msg_down: {
        auto& nb = novel_buf_;
        if (nb.bcount <= 1) {
            leave_log();
        } else {
            nb.bcount--;
            set_history_voice_mouse_rect();
            log_text_.str = nb.buf[static_cast<std::size_t>(
                (nb.bpoint - 1 - nb.bcount + nlog_max) % nlog_max)];
            message_.step1 = msg_log;
        }
        break;
    }

    case msg_drag: {
        // The handle follows the pointer, and the entry it lands on is shown
        // for as long as the button is held:
        //     bcount = (bmax-1)-(my-RectY[0]+((RectH[0]-40)/((bmax-1)*2)))
        //                      *(bmax-1)/(RectH[0]-40);
        auto& nb = novel_buf_;
        const int my = mouse_.y;
        if (nb.bmax > 1) {
            const int span = HistorySystemRectH[0] - 40;
            nb.bcount = (nb.bmax - 1)
                - (my - HistorySystemRectY[0]
                   + (span / ((nb.bmax - 1) * 2)))
                    * (nb.bmax - 1) / span;
            nb.bcount = std::clamp(nb.bcount, 0, nb.bmax - 1);
        } else {
            nb.bcount = 0;
        }
        if (nb.bcount > 0) {
            main_text_disp_ = false;
            log_text_.disp = true;
            set_history_voice_mouse_rect();
            log_text_.str = nb.buf[static_cast<std::size_t>(
                (nb.bpoint - 1 - nb.bcount + nlog_max) % nlog_max)];
        } else {
            main_text_disp_ = true;
            log_text_.disp = false;
        }
        // MUS_GetMouseButton2: the level, with no window test.
        if (!mouse_.bl) {
            if (nb.bcount > 0) {
                message_.step1 = msg_log;
            } else {
                leave_log();
            }
        }
        break;
    }

    case msg_wait2:
    default:
        break;
    }
    control_history_system();
}

void AvgMsg::control_history_system()
{
    // void ControlHistorySystem( void ): the eleven GRP_HISTORY planes, set
    // up again every frame from the machine's state and the pointer.
    auto& bar = history_bar_;
    const bool demo = hooks_.demo && hooks_.demo();
    if (!message_.disp || demo) {
        bar.shown = false;   // DSP_ResetGraph, all eleven
        return;
    }
    bar.shown = true;
    const auto& nb = novel_buf_;
    const int step1 = message_.step1;
    const int command = mouse_no_ex(mouse_any, 0);
    const bool click = mouse_in_window() && mouse_.tl; // MUS_GetMouseTrigger

    const int drag_bar_y = nb.bmax >= 2
        ? HistorySystemRectY[0]
            + (nb.bmax - 1 - nb.bcount)
                * (HistorySystemRectH[0] - (HistorySystemSrcH[0] + 1))
                / (nb.bmax - 1)
        : HistorySystemRectY[0] + HistorySystemRectH[0]
            - (HistorySystemSrcH[0] + 1);

    // DSP_SetGraphPos( GRP_HISTORY+n, RectX, RectY, SrcX+SrcW*column,
    // SrcY, RectW, RectH ), and DSP_SetGraphSMove only moves the source.
    auto button = [](int i, int column) {
        return HistoryGraph{HistorySystemRectX[i], HistorySystemRectY[i],
                            HistorySystemSrcX[i] + HistorySystemSrcW[i] * column,
                            HistorySystemSrcY[i], HistorySystemRectW[i],
                            HistorySystemRectH[i]};
    };
    auto column = [](int i, int c) {
        return HistorySystemSrcX[i] + HistorySystemSrcW[i] * c;
    };

    // GRP_HISTORY+0, BMP_HISTORY+0 whole, moved to (DISP_X-30, 0).
    bar.g[0] = {DISP_X - 30, 0, 0, 0, 30, DISP_Y};
    const int handle_column = step1 == msg_drag ? 3 : nb.bmax > 1 ? 1 : 0;
    bar.g[1] = {HistorySystemRectX[0], drag_bar_y, column(0, handle_column),
                HistorySystemSrcY[0], HistorySystemRectW[0],
                HistorySystemSrcH[0]};
    bar.g[2] = button(1, 1);
    bar.g[3] = button(2, 1);

    const bool parked = step1 == msg_wait || step1 == msg_stop
        || step1 == msg_disp || step1 == msg_next;
    const bool omake = hooks_.omake && hooks_.omake();
    const bool cut_offered = hooks_.msg_cut_offered && hooks_.msg_cut_offered();
    if (parked) {
        bar.g[4] = button(3, omake ? 0 : 1);
        bar.g[5] = button(4, omake ? 0 : 1);
        for (int i = 5; i <= 8; ++i) {
            bar.g[static_cast<std::size_t>(i + 1)] = button(i, 1);
        }
        const bool auto_flag = hooks_.auto_flag && hooks_.auto_flag();
        bar.g[6].sx = column(5, auto_flag ? 3 : 1);
        if (cut_offered) {
            const bool mode = hooks_.msg_cut_mode && hooks_.msg_cut_mode();
            bar.g[7].sx = column(6, mode ? 3 : 1);
        } else {
            bar.g[7].sx = column(6, 0);
        }
    } else {
        for (int i = 3; i <= 8; ++i) {
            bar.g[static_cast<std::size_t>(i + 1)] = button(i, 0);
        }
    }
    bar.g[10] = {HistorySystemRectX[9],
                 HistorySystemRectY[9] + (128 - half_tone_depth()),
                 column(9, 1), HistorySystemSrcY[9], HistorySystemSrcW[9],
                 HistorySystemSrcH[9]};

    switch (command) {
    case 0:
        if (step1 != msg_drag && nb.bmax > 1) {
            bar.g[1].sx = column(0, 2);
        }
        break;
    case 1:
        bar.g[2].sx = column(1, click ? 3 : 2);
        break;
    case 2:
        bar.g[3].sx = column(2, click ? 3 : 2);
        break;
    case 3:
    case 4:
        if (omake) break;
        [[fallthrough]];
    case 7:
    case 8:
        if (parked) {
            bar.g[static_cast<std::size_t>(command + 1)].sx =
                column(command, click ? 3 : 2);
        }
        break;
    case 5:
        if (parked) {
            bar.g[6].sx = column(5, click ? 3 : 2);
        }
        break;
    case 6:
        if (cut_offered && parked) {
            bar.g[7].sx = column(6, click ? 3 : 2);
        }
        break;
    case 9:
        // MUS_GetMouseButton: the level, inside the window.
        bar.g[10].sx = column(9, mouse_in_window() && mouse_.bl ? 3 : 2);
        break;
    default:
        break;
    }
    if (nb.bmax - 1 <= nb.bcount) {
        bar.g[2].sx = column(1, 0);
    }
    if (0 >= nb.bcount) {
        bar.g[3].sx = column(2, 0);
    }

    // The fade, a function static in the original: it only moves while the
    // bar is up, and picks up where it left off when it comes back.
    const bool away = mouse_.x < DISP_X - 24 && step1 != msg_drag;
    switch (hooks_.side_option ? hooks_.side_option() : 0) {
    case 0:
        bar.fade = std::clamp(bar.fade + (away ? -24 : 24), 64, 256);
        break;
    case 1:
        bar.fade = 256;
        break;
    case 2:
        bar.fade = std::clamp(bar.fade + (away ? -32 : 32), 0, 256);
        break;
    case 3:
        bar.fade = 0;
        break;
    default:
        break;
    }
}

namespace {

void write_i32(std::ostream& out, int value)
{
    out.write(reinterpret_cast<const char*>(&value), sizeof value);
}

int read_i32(std::istream& in)
{
    int value = 0;
    in.read(reinterpret_cast<char*>(&value), sizeof value);
    return value;
}

void write_string(std::ostream& out, const std::string& value)
{
    write_i32(out, static_cast<int>(value.size()));
    out.write(value.data(), static_cast<std::streamsize>(value.size()));
}

std::string read_string(std::istream& in)
{
    const int size = read_i32(in);
    if (!in || size < 0 || size > (1 << 20)) {
        in.setstate(std::ios::failbit);
        return {};
    }
    std::string value(static_cast<std::size_t>(size), '\0');
    in.read(value.data(), size);
    return value;
}

constexpr int history_magic = 0x54534948;   // "HIST"
constexpr int log_magic = 0x474f4c4e;       // "NLOG"

void write_text(std::ostream& out, const EngineText& text)
{
    write_i32(out, text.flag ? 1 : 0);
    write_i32(out, text.disp ? 1 : 0);
    write_string(out, text.str);
    write_i32(out, text.x);
    write_i32(out, text.y);
    write_i32(out, text.color);
}

EngineText read_text(std::istream& in)
{
    EngineText text;
    text.flag = read_i32(in) != 0;
    text.disp = read_i32(in) != 0;
    text.str = read_string(in);
    text.x = read_i32(in);
    text.y = read_i32(in);
    text.color = read_i32(in);
    return text;
}

}  // namespace

void AvgMsg::write_history(std::ostream& out) const
{
    write_i32(out, history_magic);
    const auto& nb = novel_buf_;
    for (int i = 0; i < nlog_max; ++i) {
        write_string(out, nb.buf[static_cast<std::size_t>(i)]);
        const auto& voices = nb.nv[static_cast<std::size_t>(i)];
        write_i32(out, static_cast<int>(voices.size()));
        for (const auto& v : voices) {
            for (const int field : {v.sno, v.vno, v.cno, v.a_cut, v.vstcount,
                                    v.px, v.py}) {
                write_i32(out, field);
            }
        }
    }
    for (const auto& mes : nb.vv_mes) {
        write_string(out, mes);
    }
    for (const int field : {nb.sno, nb.vno, nb.cno, nb.a_cut, nb.bmax,
                            nb.bpoint, nb.bcount}) {
        write_i32(out, field);
    }
    for (int lno = 0; lno < mouse_layers_saved; ++lno) {
        for (const auto& rect : rects_[static_cast<std::size_t>(lno)]) {
            for (const int field : {rect.flag ? 1 : 0, rect.sx, rect.sy,
                                    rect.w, rect.h, rect.rect_no}) {
                write_i32(out, field);
            }
        }
    }
    write_i32(out, mouse_layer_);
    for (const int field : {mouse_.x, mouse_.y, mouse_.bl ? 1 : 0,
                            mouse_.lcnt}) {
        write_i32(out, field);
    }
    write_i32(out, main_text_disp_ ? 1 : 0);
    write_text(out, log_text_);
    write_text(out, log_voice_text_);
    write_i32(out, history_bar_.fade);
}

void AvgMsg::clear_log()
{
    // A new game starts with nothing to page back to.
    novel_buf_ = {};
    log_text_ = {};
    log_voice_text_ = {};
}

void AvgMsg::write_log(std::ostream& out) const
{
    write_i32(out, log_magic);
    const auto& nb = novel_buf_;
    for (int i = 0; i < nlog_max; ++i) {
        write_string(out, nb.buf[static_cast<std::size_t>(i)]);
        const auto& voices = nb.nv[static_cast<std::size_t>(i)];
        write_i32(out, static_cast<int>(voices.size()));
        for (const auto& v : voices) {
            for (const int field : {v.sno, v.vno, v.cno, v.a_cut, v.vstcount,
                                    v.px, v.py}) {
                write_i32(out, field);
            }
        }
    }
    for (const int field : {nb.sno, nb.vno, nb.cno, nb.a_cut, nb.bmax,
                            nb.bpoint}) {
        write_i32(out, field);
    }
}

bool AvgMsg::read_log(std::istream& in, bool apply)
{
    if (read_i32(in) != log_magic || !in) {
        return false;
    }
    NovelBufState nb;
    for (int i = 0; i < nlog_max; ++i) {
        nb.buf[static_cast<std::size_t>(i)] = read_string(in);
        const int voices = read_i32(in);
        if (!in || voices < 0 || voices > nlog_v_max) {
            return false;
        }
        for (int k = 0; k < voices; ++k) {
            NovelVoice v;
            v.sno = read_i32(in);
            v.vno = read_i32(in);
            v.cno = read_i32(in);
            v.a_cut = read_i32(in);
            v.vstcount = read_i32(in);
            v.px = read_i32(in);
            v.py = read_i32(in);
            nb.nv[static_cast<std::size_t>(i)].push_back(v);
        }
    }
    nb.sno = read_i32(in);
    nb.vno = read_i32(in);
    nb.cno = read_i32(in);
    nb.a_cut = read_i32(in);
    nb.bmax = std::clamp(read_i32(in), 0, nlog_max);
    nb.bpoint = read_i32(in);
    if (!in) {
        return false;
    }
    // Loaded at the current line, not paging back through the log.
    nb.bcount = 0;
    if (apply) {
        novel_buf_ = std::move(nb);
    }
    return true;
}

bool AvgMsg::read_history(std::istream& in)
{
    if (read_i32(in) != history_magic || !in) {
        return false;
    }
    NovelBufState nb;
    for (int i = 0; i < nlog_max; ++i) {
        nb.buf[static_cast<std::size_t>(i)] = read_string(in);
        const int voices = read_i32(in);
        if (!in || voices < 0 || voices > nlog_v_max) {
            return false;
        }
        for (int k = 0; k < voices; ++k) {
            NovelVoice v;
            v.sno = read_i32(in);
            v.vno = read_i32(in);
            v.cno = read_i32(in);
            v.a_cut = read_i32(in);
            v.vstcount = read_i32(in);
            v.px = read_i32(in);
            v.py = read_i32(in);
            nb.nv[static_cast<std::size_t>(i)].push_back(v);
        }
    }
    for (auto& mes : nb.vv_mes) {
        mes = read_string(in);
    }
    nb.sno = read_i32(in);
    nb.vno = read_i32(in);
    nb.cno = read_i32(in);
    nb.a_cut = read_i32(in);
    nb.bmax = read_i32(in);
    nb.bpoint = read_i32(in);
    nb.bcount = read_i32(in);
    std::array<std::array<MouseRect, mouse_rest_max>, mouse_layers> rects{};
    for (int lno = 0; lno < mouse_layers_saved; ++lno) {
        for (auto& rect : rects[static_cast<std::size_t>(lno)]) {
            rect.flag = read_i32(in) != 0;
            rect.sx = read_i32(in);
            rect.sy = read_i32(in);
            rect.w = read_i32(in);
            rect.h = read_i32(in);
            rect.rect_no = read_i32(in);
        }
    }
    const int mouse_layer = read_i32(in);
    EngineMouse mouse;
    mouse.x = read_i32(in);
    mouse.y = read_i32(in);
    mouse.bl = read_i32(in) != 0;
    mouse.lcnt = read_i32(in);
    const bool main_disp = read_i32(in) != 0;
    const auto log_text = read_text(in);
    const auto log_voice_text = read_text(in);
    const int fade = read_i32(in);
    if (!in) {
        return false;
    }
    novel_buf_ = std::move(nb);
    rects_ = rects;
    mouse_layer_ = mouse_layer;
    mouse_ = mouse;
    main_text_disp_ = main_disp;
    log_text_ = log_text;
    log_voice_text_ = log_voice_text;
    history_bar_.fade = fade;
    return true;
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
    set_draw_flag_on();   // MainWindow.draw_flag=1;
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
    // Everything here reads the text object, not NovelMessage: ts->cnt and
    // ts->step are what TXT_DrawTextEx is actually handed.
    auto shown = txt_visible_glyphs(
        counted_,
        text_cnt_ < 0 ? text_cnt_
                      : text_cnt_ + static_cast<int>(std::floor(present_extra_)));

    // TXT_DrawTextEx stops the draw outright at the current \k:
    //
    //     case 'k':
    //         if(step>=step_cnt && step_cnt!=-1){ end_flag=1; }
    //         step++;
    //
    // NovelMessage.max is the count at the \k *plus eight*, so the count
    // deliberately runs past the end of the segment and this stop is the
    // only thing holding the reveal at the boundary.  Without it the tail
    // kept revealing into the next segment - eight characters of the
    // following sentence before the reader had clicked.
    if (text_step_ >= 0) {
        const auto limit = static_cast<std::size_t>(
            std::ranges::upper_bound(counted_.glyph_step, text_step_)
            - counted_.glyph_step.begin());
        shown = std::min(shown, limit);
    }
    // ...and at what the window had room for, but only with the original
    // bitmap font.  TXT_DrawTextEx stops the walk dead when the text
    // overflows the box height, so vanilla silently loses the tail of a long
    // message.  Our own font is a different size and our window scrolls
    // instead, which is the better behaviour and the reason the outline font
    // exists - so the vanilla clip applies only where we are trying to be
    // vanilla.
    if (vanilla_layout_) {
        shown = std::min(shown, layout_.glyph_x.size());
    }
    return shown;
}

int AvgMsg::glyph_alpha(std::size_t index) const
{
    // TXT_DrawTextEx, both lines of it:
    //
    //     if(text_cnt==-1){ alph2 = 256; }
    //     else            { alph2 = LIM(text_cnt-cnt2, 0, 16)*16; }
    //     if( step < step_cnt ){ alph2 = 256; }
    //
    // The step guard is the one that matters at a \k.  NovelMessage.count
    // stops at the end of the step, so every character near that boundary
    // has text_cnt - cnt2 close to zero - and when the reader clicks on and
    // the next step starts revealing, the characters they just finished
    // reading fade out again.  The guard pins anything from an earlier step
    // at fully opaque, which is why the original never does that.
    if (index >= counted_.glyph_count.size()) {
        return 0;
    }
    if (text_cnt_ < 0) {
        return 256;
    }
    if (index < counted_.glyph_step.size()
        && counted_.glyph_step[index] < text_step_) {
        return 256;
    }
    if (present_extra_ > 0.0) {
        const double delta =
            text_cnt_ + present_extra_ - counted_.glyph_count[index];
        return static_cast<int>(std::floor(std::clamp(delta, 0.0, 16.0) * 16));
    }
    const int delta = text_cnt_ - counted_.glyph_count[index];
    return std::clamp(delta, 0, 16) * 16;
}

void AvgMsg::set_presentation_phase(double phase)
{
    present_extra_ = 0.0;
    if (phase <= 0.0 || message_.step1 != msg_disp || text_cnt_ < 0
        || message_.count >= message_.max) {
        return;
    }
    //     if( Avg.demo ) NovelMessage.count+=(short)(2*30/Avg.frame);
    //     else           NovelMessage.count+=AVG_MsgCnt();
    const int step = hooks_.demo && hooks_.demo()
        ? 2 * 30 / 60
        : (hooks_.msg_cnt ? hooks_.msg_cnt() : 1);
    if (step <= 0 || step >= 9999) {
        return;   // the skip key: the whole page arrives on the next tick
    }
    present_extra_ = std::min(phase * step,
                              static_cast<double>(message_.max - text_cnt_));
}

bool AvgMsg::present_half_tone(double phase, int& r, int& g, int& b) const
{
    if (half_tone_.tstep != tone_fadeout
        || (hooks_.wav_effect && hooks_.wav_effect())) {
        return false;
    }
    const int pulse = hooks_.eff_cnt_puls ? hooks_.eff_cnt_puls() : 1;
    if (pulse <= 0 || pulse >= 9999 || half_tone_.tcount >= 16) {
        return false;
    }
    // tcount was stepped before the brightness was worked out, so the
    // screen shows tcount and the next tick tcount+pulse.
    const double c = std::min(16.0, half_tone_.tcount + phase * pulse);
    const int tone = half_tone_depth();
    const auto at = [&](int channel) {
        return static_cast<int>(std::floor(
            (c * channel * tone / 128.0 + (16.0 - c) * channel) / 16.0));
    };
    r = at(back_->r);
    g = at(back_->g);
    b = at(back_->b);
    return true;
}

double AvgMsg::present_bar_fade(double phase) const
{
    const auto& bar = history_bar_;
    const double now = bar.fade;
    if (!bar.shown || phase <= 0.0) {
        return now;
    }
    const bool away = mouse_.x < DISP_X - 24 && message_.step1 != msg_drag;
    int next = bar.fade;
    switch (hooks_.side_option ? hooks_.side_option() : 0) {
    case 0: next = std::clamp(bar.fade + (away ? -24 : 24), 64, 256); break;
    case 1: next = 256; break;
    case 2: next = std::clamp(bar.fade + (away ? -32 : 32), 0, 256); break;
    case 3: next = 0; break;
    default: break;
    }
    return now + (next - now) * phase;
}

void AvgMsg::restore_raw(std::string raw)
{
    raw_ = std::move(raw);
    // Both tables, as set_text/add_text build them - not just counted_.
    // layout_ is what the vanilla clip in visible_glyphs measures against,
    // so restoring the raw text without it left every glyph clamped to zero
    // the moment vanilla_layout_ was itself restored: a resumed run showed
    // no text at all while every counter agreed with the reference.
    counted_ = txt_count_text(raw_);
    layout_ = txt_count_text(raw_, message_text_box);
}

}  // namespace th2
