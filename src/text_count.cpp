#include "text_count.hpp"

#include <ranges>

#include <algorithm>
#include <cctype>

namespace th2 {
namespace {

// static int TXT_GetMsgSpeed( int speed ), verbatim.  The static speed_cnt
// is the carry: at speed 10 it contributes nothing and every character is
// worth one, at speed 1 it takes ten characters to be worth one.
class MsgSpeed {
public:
    void reset() { speed_cnt_ = 0; }          // TXT_GetMsgSpeed( -1 )
    int remainder() const { return speed_cnt_; }  // TXT_GetMsgSpeed( -2 )
    int advance(int speed)
    {
        speed_cnt_ += speed % 10;
        const int ret = speed / 10 + speed_cnt_ / 10;
        speed_cnt_ = speed_cnt_ % 10;
        return ret;
    }

private:
    int speed_cnt_ = 0;
};

// static int GetDigit( char *str, int *cnt, int *digit ), verbatim - right
// down to the decrement, which leaves the caller's cnt on the last digit so
// that the loop's own cnt++ steps past it.
int get_digit(std::string_view str, std::size_t& cnt, int& digit)
{
    int dcnt = 0;
    digit = 0;
    while (cnt < str.size()
           && std::isdigit(static_cast<unsigned char>(str[cnt]))) {
        digit *= 10;
        digit += str[cnt] - '0';
        ++cnt;
        ++dcnt;
    }
    if (cnt >= str.size() || str[cnt] != ':') {
        --cnt;
    }
    return dcnt;
}

// How many bytes the code point starting here occupies.  The original tests
// the CP932 lead byte instead: (0x00<=c && c<0x80) || (0xa0<=c && c<0xe0) is
// one byte, anything else is two.  Either way one character is one count.
std::size_t utf8_length(unsigned char lead)
{
    if (lead < 0x80) return 1;
    if (lead < 0xc2) return 1;   // stray continuation byte: step over it
    if (lead <= 0xdf) return 2;
    if (lead <= 0xef) return 3;
    if (lead <= 0xf4) return 4;
    return 1;
}

enum Tag {
    tag_digit,
    tag_color,
    tag_font,
    tag_rubi,
    tag_accent,
    tag_speed,
    tag_wait,
};

}  // namespace

namespace {

// The characters TXT_DrawTextEx refuses to start a line with - the engine
// spells the test out as a wall of strncmp against CP932 literals.  Ours are
// UTF-8, so they are listed as such.
bool starts_no_line(std::string_view rest)
{
    static constexpr std::string_view forbidden[] = {
        "\u3002", "\u3001", "\uff0c", "\uff0e", "\u30fb", "\u2026",
        "\u30fc", "\uff1a", "\uff1b", "\uff1f", "\uff01", "\uff3d",
        "\u201d", "\u3000", "\uff09", "\u300d", "\u300f",
    };
    return std::ranges::any_of(forbidden, [&](std::string_view c) {
        return rest.starts_with(c);
    });
}

}  // namespace

TextCount txt_count_text(std::string_view str, const TextBox& box)
{
    TextCount out;

    // The locals TXT_DrawTextEx keeps that matter when nothing is drawn.
    // Everything to do with position - px, py, fno, the line-break table -
    // is layout, which is ours, so it is not carried here.
    std::size_t cnt = 0;
    int cnt2 = 0;
    int tag_cnt = 0;
    int tag_param[16] = {};
    int tag_back[16] = {};
    bool draw_flag = false;
    int wait = 0;
    int speed = 10;
    int step = 0;
    bool end_flag = false;
    MsgSpeed msg_speed;

    // TXT_DrawTextEx's cursor.  With the counting box - 256x256 cells in a
    // 20px font, which is what TXT_GetTextCount passes - nothing ever
    // reaches the wrap or the clip, so this is inert and the walk is purely
    // a character count.  With the message window's box it is the layout.
    int px = box.sx;
    int py = box.sy;
    const int fno = box.font;
    int fno2 = fno;
    int kaig = 0;    // an automatic break just happened; swallow the next \n
    int kflag = 0;   // holding a line open for a character that may not start one
    int px_bak = 0;  // where the last line ended, for the cursor report

    msg_speed.reset();   // TXT_GetMsgSpeed( -1 )

    // while( cnt2 < text_cnt || text_cnt==-1 || ( cnt2==text_cnt && !amari) )
    // with text_cnt == -1, so it runs to the end of the string.
    //
    // `amari` is TXT_GetMsgSpeed(-2), the fractional accumulator, and the
    // original reads it at the *top of the body* - after the condition has
    // already been evaluated.  So the condition that admits an iteration
    // sees the value read one iteration earlier, and that lag is not a
    // detail: it decides whether the character sitting exactly on the count
    // is entered at all.  Recorded per glyph so txt_visible_glyphs can ask
    // the same question later.
    int amari = 0;
    while (cnt < str.size()) {
        const int entry_amari = amari;
        amari = msg_speed.remainder();
        int drew = -1;
        if (wait) {
            // A <w> tag spends counts without drawing anything, which is how
            // a pause in the middle of a line is timed.
            --wait;
            ++cnt2;
            continue;
        }

        const char c = str[cnt];
        switch (c) {
        case '\0':
            end_flag = true;
            break;
        case '\n':
            //     if(!kaig){ px_bak=px; px=sx; py+=fno2+pich_h; fno2=fno; }
            //     kaig=0;
            if (!kaig) {
                px_bak = px;
                px = box.sx;
                py += fno2 + box.pich_h;
                fno2 = fno;
            }
            kaig = 0;
            break;
        case '^':
            // The script writes ^ for a space and ~ for a comma, because the
            // compiler eats the real ones.
            draw_flag = true;
            break;
        case '~':
            draw_flag = true;
            break;
        case '\\':
            ++cnt;
            if (cnt >= str.size()) {
                end_flag = true;
                break;
            }
            switch (str[cnt]) {
            case 'n':
                if (!kaig) {
                    px_bak = px;
                    px = box.sx;
                    py += fno2 + box.pich_h;
                    fno2 = fno;
                }
                kaig = 0;
                break;
            case 'k':
                // The step boundary.  step_cnt is -1 here so the walk never
                // stops early; the count reached at each one is recorded
                // instead, which is the whole table the caller wants.
                out.step_total.push_back(cnt2);
                ++step;
                break;
            case '^': case '~':
            case '<': case '>':
            case '|': case '\\':
                draw_flag = true;
                break;
            default:
                break;
            }
            break;
        case '<': {
            ++cnt;
            if (cnt >= str.size()) {
                end_flag = true;
                break;
            }
            if (tag_cnt >= 15) {
                break;
            }
            switch (str[cnt]) {
            case 'd': case 'D': {
                tag_param[tag_cnt] = tag_digit;
                tag_back[tag_cnt] = 0;
                ++tag_cnt;
                ++cnt;
                int digit = 0;
                get_digit(str, cnt, digit);
                break;
            }
            case 'c': case 'C': {
                tag_param[tag_cnt] = tag_color;
                tag_back[tag_cnt] = 0;
                ++tag_cnt;
                ++cnt;
                int digit = 0;
                get_digit(str, cnt, digit);
                break;
            }
            case 'f': case 'F': {
                tag_param[tag_cnt] = tag_font;
                tag_back[tag_cnt] = 0;
                ++tag_cnt;
                ++cnt;
                int digit = 0;
                get_digit(str, cnt, digit);
                break;
            }
            case 'r': case 'R':
                tag_param[tag_cnt] = tag_rubi;
                tag_back[tag_cnt] = 0;
                ++tag_cnt;
                break;
            case 'a': case 'A':
                tag_param[tag_cnt] = tag_accent;
                ++tag_cnt;
                break;
            case 's': case 'S': {
                tag_param[tag_cnt] = tag_speed;
                tag_back[tag_cnt] = speed;
                ++tag_cnt;
                ++cnt;
                int digit = 0;
                if (get_digit(str, cnt, digit)) {
                    switch (digit) {
                    case 0:  speed = 100; break;
                    case 1:  speed = 80;  break;
                    case 2:  speed = 60;  break;
                    case 3:  speed = 40;  break;
                    case 4:  speed = 20;  break;
                    case 5:  speed = 10;  break;
                    case 6:  speed = 6;   break;
                    case 7:  speed = 4;   break;
                    case 8:  speed = 2;   break;
                    case 9:  speed = 1;   break;
                    case 10: speed = 0;   break;
                    default: break;
                    }
                }
                break;
            }
            case 'w': case 'W': {
                tag_param[tag_cnt] = tag_wait;
                tag_back[tag_cnt] = 0;
                ++tag_cnt;
                ++cnt;
                int digit = 0;
                if (get_digit(str, cnt, digit)) {
                    wait = digit * 2;
                }
                break;
            }
            default:
                break;
            }
            break;
        }
        case '|':
            // The ruby text between | and > is drawn above the line and does
            // not advance cnt2, so the walk skips it outright.
            ++cnt;
            if (tag_cnt > 0 && tag_param[tag_cnt - 1] == tag_rubi) {
                while (cnt < str.size() && str[cnt] != '>') {
                    ++cnt;
                }
                --cnt;
            }
            break;
        case '>':
            if (tag_cnt <= 0) {
                break;
            }
            --tag_cnt;
            // Closing a tag restores what it pushed.  This used to skip the
            // restore on the grounds that "TH2_Flag is ON in this game", so
            // a <S> held to the end of the line - but TH2_Flag is OFF
            // (my_inc2/text.cpp:158), which is what makes TXT_DrawTextEx run
            // its restore switch.  A <S0> therefore changes the speed for no
            // characters at all, where ours charged ten counts each for the
            // rest of the message: at pc 18629 of 040426300.sdt ours made
            // NovelMessage.max 1268 against the reference's 134.
            //
            // Speed is the only one of them this counter varies - fno is
            // box.font throughout and the colour is not tracked - so it is
            // the only one restored here, and it is the only one whose
            // previous value the stack actually saves.
            if (tag_param[tag_cnt] == tag_speed) {
                speed = tag_back[tag_cnt];
            }
            break;
        default:
            draw_flag = true;
            break;
        }

        if (draw_flag) {
            draw_flag = false;
            const auto lead = static_cast<unsigned char>(str[cnt]);
            const auto width = utf8_length(lead);
            // The counter as it stands *before* this character, which is
            // the value TXT_DrawTextEx tests and the value it measures the
            // fade from:
            //
            //     while( cnt2 < text_cnt || ... )
            //         alph2 = LIM(text_cnt - cnt2, 0, 16) * 16;
            //         ... draw ...
            //         cnt2 += TXT_GetMsgSpeed( speed );
            //
            // This used to be overwritten with the count *after* the
            // character, on the reasoning that the loop draws while
            // cnt2 < n - but the cnt2 it compares is this one, taken before
            // the character is drawn, so storing the later value put every
            // glyph one step behind and started the reveal a tick late.
            out.glyph_count.push_back(cnt2);
            out.glyph_step.push_back(step);
            out.glyph_amari.push_back(entry_amari);
            out.glyph_x.push_back(px);
            out.glyph_y.push_back(py);
            out.glyph_off.push_back(static_cast<int>(cnt));
            drew = static_cast<int>(out.glyph_x.size()) - 1;
            cnt2 += msg_speed.advance(speed);
            //     px += fno/2+pixh_w;   (half width)
            //     px += fno  +pixh_w;   (full width)
            px += (width == 1 ? fno / 2 : fno) + box.pich_w;
            // kaig is cleared by drawing, not only by a newline:
            //
            //     px += fno/2+pixh_w;
            //     kaig=0;
            //
            // so it swallows a \n only when one lands immediately after an
            // automatic break, with no character between them.  Left set
            // until the next newline, it ate the line breaks the translator
            // put in and the message came out several lines short - which
            // then hid the overflow clip, because the text fitted the box.
            kaig = 0;
            // The original advances cnt by one extra byte for a two-byte
            // character here and then once more at the bottom of the loop.
            cnt += width - 1;
        }
        if (end_flag) {
            if (drew >= 0) {
                out.cursor_x.push_back(px);
                out.cursor_y.push_back(py);
                out.cursor_bak.push_back(px_bak);
            }
            break;
        }
        ++cnt;

        // The line break, which the engine makes *after* stepping to the
        // next character so it can refuse to start a line with one of the
        // closing marks:
        //
        //     if( px-sx >= w*font-fno+1 ){ ... px=sx; py+=fno2+pich_h; ... }
        //
        // There is no word wrap in it at all - the break lands wherever the
        // cell runs out, mid-word if that is where it falls.
        const auto rest = str.substr(std::min(cnt, str.size()));
        if (px - box.sx >= box.w * fno - fno + 1) {
            if (kflag == 1) {
                if (!rest.starts_with("\u3000")) {
                    kflag = 0;
                    px_bak = px;
                    px = box.sx;
                    py += fno2 + box.pich_h;
                    fno2 = fno;
                    kaig = 1;
                }
            } else if (!starts_no_line(rest)) {
                px_bak = px;
                px = box.sx;
                py += fno2 + box.pich_h;
                fno2 = fno;
                kaig = 1;
            } else {
                kflag = 1;
            }
        }

        // And the clip.  Text that overflows the box height is not wrapped,
        // not scrolled and not shortened - the walk simply stops, and the
        // rest of the message is never drawn at all:
        //
        //     if( py-sy >= h*(font+pich_h)-(fno+pich_h)+1 ){
        //         if( py-sy==h*(font+pich_h) && px==sx ){ }else{ break; }
        //     }
        if (drew >= 0) {
            out.cursor_x.push_back(px);
            out.cursor_y.push_back(py);
            out.cursor_bak.push_back(px_bak);
        }

        if (py - box.sy >= box.h * (fno + box.pich_h) - (fno + box.pich_h) + 1) {
            if (!(py - box.sy == box.h * (fno + box.pich_h) && px == box.sx)) {
                break;
            }
        }
    }

    out.total = cnt2;
    return out;
}

int txt_get_text_count(const TextCount& counted, int step_cnt)
{
    if (step_cnt < 0
        || step_cnt >= static_cast<int>(counted.step_total.size())) {
        return counted.total;
    }
    return counted.step_total[static_cast<std::size_t>(step_cnt)];
}

bool txt_get_text_end_key_wait(std::string_view str)
{
    // int TXT_GetTextEndKeyWait( char *str ), verbatim.  It walks backwards
    // from the second-to-last byte over the run of k, n, backslash and '>'
    // that a trailing tag makes, and reports a backslash-k inside it.
    const auto len = static_cast<int>(str.size());
    int i = 2;
    while (len - i > 0) {
        const char c = str[static_cast<std::size_t>(len - i)];
        if (c == 'k' || c == 'n' || c == '\\' || c == '>') {
            if (c == '\\') {
                if (str[static_cast<std::size_t>(len - i + 1)] == 'k') {
                    return true;
                }
            }
        } else {
            break;
        }
        ++i;
    }
    return false;
}

bool txt_cursor_after(const TextCount& counted, const TextBox& box,
                      std::size_t shown, int* x, int* y)
{
    if (shown == 0) {
        // Nothing drawn: TXT_DrawTextEx's px/py never left (sx, sy), and
        // its kaigyou test needs py != sy, so the cursor is the origin.
        *x = box.sx;
        *y = box.sy;
        return true;
    }
    if (shown > counted.cursor_x.size()) {
        return false;
    }
    const auto i = shown - 1;
    const int px = counted.cursor_x[i];
    const int py = counted.cursor_y[i];
    const int px_bak = counted.cursor_bak[i];
    if (px == box.sx && py != box.sy
        && px_bak - box.sx <= box.w * box.font) {
        *x = px_bak;
        *y = py - (box.font + box.pich_h);
    } else {
        *x = px;
        *y = py;
    }
    return true;
}

std::size_t txt_visible_glyphs(const TextCount& counted, int text_cnt)
{
    if (text_cnt < 0) {
        return counted.glyph_count.size();
    }
    // while( cnt2 < text_cnt || ( cnt2==text_cnt && !amari) )
    //
    // Every character whose count is strictly behind is on screen, and the
    // one sitting exactly on the count joins it only when the fractional
    // accumulator is empty - drawn at LIM(0,0,16)*16, which is invisible but
    // counted.  Taking it unconditionally put one blank glyph too many on
    // the end for most of a message.
    const auto begin = counted.glyph_count.begin();
    const auto it = std::ranges::lower_bound(counted.glyph_count, text_cnt);
    auto shown = static_cast<std::size_t>(it - begin);
    if (shown < counted.glyph_count.size()
        && counted.glyph_count[shown] == text_cnt
        && shown < counted.glyph_amari.size()
        && counted.glyph_amari[shown] == 0) {
        ++shown;
    }
    return shown;
}

}  // namespace th2
