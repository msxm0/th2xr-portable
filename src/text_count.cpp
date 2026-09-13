#include "text_count.hpp"

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

TextCount txt_count_text(std::string_view str)
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

    msg_speed.reset();   // TXT_GetMsgSpeed( -1 )

    // while( cnt2 < text_cnt || text_cnt==-1 || ( cnt2==text_cnt && !amari) )
    // with text_cnt == -1, so it runs to the end of the string.
    while (cnt < str.size()) {
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
            // TH2_Flag is ON in this game, so the switch that restores the
            // saved colour, font and speed is skipped: a <s> tag holds to
            // the end of the line rather than to its closing bracket.
            break;
        default:
            draw_flag = true;
            break;
        }

        if (draw_flag) {
            draw_flag = false;
            const auto lead = static_cast<unsigned char>(str[cnt]);
            const auto width = utf8_length(lead);
            out.glyph_count.push_back(cnt2);
            out.glyph_step.push_back(step);
            cnt2 += msg_speed.advance(speed);
            // The original advances cnt by one extra byte for a two-byte
            // character here and then once more at the bottom of the loop.
            cnt += width - 1;
            // What the entry really records is the count *after* this
            // character, because DSP_SetTextCount(n) draws while cnt2 < n.
            out.glyph_count.back() = cnt2;
        }
        if (end_flag) {
            break;
        }
        ++cnt;
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

std::size_t txt_visible_glyphs(const TextCount& counted, int text_cnt)
{
    if (text_cnt < 0) {
        return counted.glyph_count.size();
    }
    // Every character whose count has been reached is on screen.  The
    // original draws while cnt2 < text_cnt and fades the last sixteen counts
    // in, which is what alph2 = LIM(text_cnt-cnt2, 0, 16)*16 does.
    const auto it = std::ranges::upper_bound(counted.glyph_count, text_cnt);
    return static_cast<std::size_t>(it - counted.glyph_count.begin());
}

}  // namespace th2
