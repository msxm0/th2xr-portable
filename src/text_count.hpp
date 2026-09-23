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
    // TXT_GetMsgSpeed's fractional accumulator as the loop condition saw it
    // when this character's iteration was admitted.  Only matters for the
    // character sitting exactly on the count, which is entered when this is
    // zero and skipped when it is not.
    std::vector<int> glyph_amari;
    // Where each character lands, in 800x600 space.  Only as long as the
    // box had room for: TXT_DrawTextEx stops the walk outright when the
    // text overflows the height, so a message longer than the window simply
    // loses its tail.  Shorter than glyph_count whenever that happens.
    std::vector<int> glyph_x;
    std::vector<int> glyph_y;
    // Byte offset of each drawn character in the string that was walked.
    // The walk skips escapes and tags, so the Nth drawn character is not the
    // Nth character of the source - and it is not the Nth character of any
    // other rendering of that source either, which is what makes indexing a
    // separately-processed copy by glyph number go wrong.
    std::vector<int> glyph_off;
    // TXT_DrawTextEx's cursor as it stands once the iteration that drew this
    // character has finished - including the line break its tail may have
    // just applied.  DSP_GetTextDispPos hands this to the click indicator,
    // so it is where the mark goes.  px_bak comes with it because the
    // report has a special case for a cursor sitting at the start of a
    // fresh line, which points back at where the previous one ended.
    std::vector<int> cursor_x;
    std::vector<int> cursor_y;
    std::vector<int> cursor_bak;
};

// The box TXT_DrawTextEx lays text out in.  There are two of these in the
// engine and they are nothing alike, which is the point of having the
// parameter at all:
//
//   TXT_GetTextCount( str, step ) passes w=256 h=256 font=20 - so big that
//   nothing ever wraps or clips, because it only wants the character count.
//
//   DrawGraphText passes the text object's own ws/hs/pw/ph and font, which
//   for the message window is MES_POS_* - a 20x10 cell box at (48,50) in a
//   34px font.  That one wraps, and drops whatever does not fit.
struct TextBox {
    int sx = 0;         // MES_POS_X
    int sy = 0;         // MES_POS_Y
    int w = 256;        // MES_POS_W, in cells
    int h = 256;        // MES_POS_H, in cells
    int pich_w = 0;     // MES_PICH_W
    int pich_h = 0;     // MES_PICH_H
    int font = 20;      // fno
};

// GM_AvgMsg.cpp's MES_POS_X/Y/W/H, MES_PICH_W/H and SYS_FONT - as the
// English release sets them, not as the GPL source does.  That release is
// the same engine rebuilt with the message window widened, and these six
// values are the only constant it changed:
//
//     MES_POS_X  48 -> 30     MES_POS_H   10 -> 20
//     MES_POS_Y  50 -> 25     MES_PICH_H  18 ->  9
//     MES_POS_W  20 -> 31     SYS_FONT    34 -> 24
//
// 31 cells of a 24px font is 60 half-width characters a line, and the
// translated script's own hand-inserted breaks run to 57 - so it was
// written for this box.  The Japanese one fits 38, which is why English
// text laid out in it wraps mid-word and loses its tail to the height clip.
inline constexpr TextBox message_text_box{30, 25, 31, 20, 0, 9, 24};



// TXT_DrawTextEx( ..., cnt_flag = 1 ), walking the raw script string with
// its tags still in it.  The original walks CP932 bytes; ours walks UTF-8
// code points, which comes to the same cnt2 because every control code is
// ASCII and cnt2 advances once per drawn character either way.
TextCount txt_count_text(std::string_view str, const TextBox& box = {});

// Where TXT_DrawTextEx reports its cursor after drawing `shown` characters -
// the tail of the function, verbatim:
//
//     if( px==sx && py!=sy && (px_bak-sx <= w*font) ){
//         *px2 = px_bak;  *py2 = py - (fno2+pich_h);
//     }else{
//         *px2 = px;      *py2 = py;
//     }
//
// Returns false when there is nothing drawn to point at.
bool txt_cursor_after(const TextCount& counted, const TextBox& box,
                      std::size_t shown, int* x, int* y);

// TXT_GetTextCount( str, step_cnt ): -1 means the whole string.
int txt_get_text_count(const TextCount& counted, int step_cnt);

// TXT_GetTextEndKeyWait( str ): the text ends on a \k, so add_flag 1 stops
// for the reader instead of running on into the next instruction.
bool txt_get_text_end_key_wait(std::string_view str);

// How many drawn characters DSP_SetTextCount( text_cnt ) puts on screen.
// -1 is all of them.
std::size_t txt_visible_glyphs(const TextCount& counted, int text_cnt);

}  // namespace th2
