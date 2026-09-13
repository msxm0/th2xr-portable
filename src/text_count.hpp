#pragma once

// my_inc2/text.cpp's counting half, transcribed.
//
// TXT_GetTextCount( str, step_cnt ) is the only thing the message state
// machine knows about text:
//
//     int TXT_GetTextCount( char *str, int step_cnt )
//     {
//         return TXT_DrawTextEx( NULL, 0, 0, 0, 256, 256, 0, 0, NULL, NULL,
//                                NULL, 20, str, 0, -1, step_cnt,
//                                128, 128, 128, 256, 0, 1 );
//     }
//
// - cnt_flag 1, so nothing is drawn and the return is cnt2.  We port that
// walk and leave the drawing to our own layout engine, which is why keeping
// high-resolution text costs nothing here: the state machine wants counts,
// not glyphs.
//
// cnt2 is not a character count.  It is advanced by TXT_GetMsgSpeed(speed),
// which carries a remainder between characters, so a <s> tag makes one
// character worth ten counts or two characters worth one.  At the default
// speed of ten it is one per character, which is why it looks like a
// character count in every line that has no speed tag - and most do not.

#include <cstddef>
#include <string_view>
#include <vector>

namespace th2 {

// One walk of TXT_DrawTextEx in counting mode, keeping everything the
// message machine and the renderer need to ask about afterwards.
struct TextCount {
    // cnt2 at the end, which is TXT_GetTextCount( str, -1 ).
    int total = 0;
    // cnt2 at each \k, so TXT_GetTextCount( str, step ) is a lookup.
    std::vector<int> step_total;
    // For each character that was drawn, the value cnt2 had reached once it
    // had been.  DSP_SetTextCount( n ) shows every character whose entry is
    // at most n, which is how a count turns back into a prefix of the text.
    std::vector<int> glyph_count;
    // Which \k step each drawn character belongs to.
    std::vector<int> glyph_step;
};

// TXT_DrawTextEx( ..., cnt_flag = 1 ), walking the raw script string with
// its tags still in it.  The original walks CP932 bytes; ours walks UTF-8
// code points, which comes to the same cnt2 because every control code is
// ASCII and cnt2 advances once per drawn character either way.
TextCount txt_count_text(std::string_view str);

// TXT_GetTextCount( str, step_cnt ): -1 means the whole string.
int txt_get_text_count(const TextCount& counted, int step_cnt);

// TXT_GetTextEndKeyWait( str ): the text ends on a \k, so add_flag 1 stops
// for the reader instead of running on into the next instruction.
bool txt_get_text_end_key_wait(std::string_view str);

// How many drawn characters DSP_SetTextCount( text_cnt ) puts on screen.
// -1 is all of them.
std::size_t txt_visible_glyphs(const TextCount& counted, int text_cnt);

}  // namespace th2
