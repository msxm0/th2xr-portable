// TXT_GetTextCount and the accumulation AVG_AddNovelMessage depends on.
//
// This is the arithmetic NovelMessage.count is measured against, and getting
// it wrong is invisible until it is very visible: a line cut short, glyph
// alphas read at the wrong indices so text that was already up fades in
// again, and MSG_STOP reached early with the click indicator left floating
// past the end of the text.

#include "text_count.hpp"

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const char* what)
{
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

void check_equal(int actual, int expected, const char* what)
{
    if (actual != expected) {
        std::printf("FAIL: %s (got %d, expected %d)\n", what, actual, expected);
        ++failures;
    }
}

// At the default speed of ten, TXT_GetMsgSpeed returns one per character, so
// cnt2 is a plain character count.  That is what makes the whole thing look
// like a character count in every line that has no <s> tag - and most do not.
void test_plain_count()
{
    const auto counted = th2::txt_count_text("abcde");
    check_equal(counted.total, 5, "five characters count five");
    check_equal(th2::txt_get_text_count(counted, -1), 5,
                "step -1 is the whole string");
    check_equal(static_cast<int>(counted.glyph_count.size()), 5,
                "one entry per drawn character");
}

// Control codes draw nothing and cost nothing.
void test_control_codes()
{
    check_equal(th2::txt_count_text("ab\\ncd").total, 4,
                "a line break is not a character");
    check_equal(th2::txt_count_text("ab<c3>cd>").total, 4,
                "a colour tag is not a character");
    // ^ is a space and ~ a comma, because the compiler eats the real ones.
    check_equal(th2::txt_count_text("a^b~c").total, 5,
                "^ and ~ are drawn characters");
}

// The \k steps, which are what NovelMessage.kstep indexes.
void test_steps()
{
    const auto counted = th2::txt_count_text("abc\\kde\\kfghi");
    check_equal(static_cast<int>(counted.step_total.size()), 2,
                "two \\k markers");
    check_equal(th2::txt_get_text_count(counted, 0), 3, "step 0 ends at 3");
    check_equal(th2::txt_get_text_count(counted, 1), 5, "step 1 ends at 5");
    check_equal(th2::txt_get_text_count(counted, 2), 9,
                "past the last step is the whole string");
    check_equal(th2::txt_get_text_count(counted, -1), 9, "and so is -1");
}

// AVG_AddNovelMessage appends "\k" + the new text to the same text object and
// every TXT_GetTextCount after it measures the whole of it, with
// NovelMessage.count carrying over from the previous line.  Counting only the
// latest chunk leaves count running off the end of the table.
void test_accumulation()
{
    std::string raw = "first";
    auto counted = th2::txt_count_text(raw);
    const int first_max = th2::txt_get_text_count(counted, 0) + 8;
    check_equal(first_max, 13, "the first line stops at its count plus eight");

    raw += "\\k";
    raw += "second";
    counted = th2::txt_count_text(raw);
    // kstep has moved to 1 and count carries over as the old max.
    check_equal(th2::txt_get_text_count(counted, 1), 11,
                "the accumulated count covers both chunks");
    check_equal(th2::txt_get_text_count(counted, 0), 5,
                "and the first step still ends where it did");
    check_equal(static_cast<int>(counted.glyph_count.size()), 11,
                "eleven drawn characters across the two chunks");
    // The count that carries over must land inside the new table, not past
    // its end - that was the bug.
    check(first_max - 8 < static_cast<int>(counted.glyph_count.size()),
          "the carried-over count indexes into the accumulated table");
}

// DSP_SetTextCount( n ) draws every character whose count has been reached,
// and alph2 = LIM( text_cnt - cnt2, 0, 16 ) * 16 fades in the last sixteen.
void test_visible_prefix()
{
    const auto counted = th2::txt_count_text("abcdef");
    check_equal(static_cast<int>(th2::txt_visible_glyphs(counted, 0)), 0,
                "nothing is shown at count zero");
    check_equal(static_cast<int>(th2::txt_visible_glyphs(counted, 3)), 3,
                "three characters at count three");
    check_equal(static_cast<int>(th2::txt_visible_glyphs(counted, 99)), 6,
                "everything once the count is past the end");
    check_equal(static_cast<int>(th2::txt_visible_glyphs(counted, -1)), 6,
                "-1 shows the lot");
}

// TXT_GetTextEndKeyWait walks back over the trailing tag run looking for \k.
void test_end_key_wait()
{
    check(th2::txt_get_text_end_key_wait("hello\\k"),
          "a trailing \\k is a key wait");
    check(!th2::txt_get_text_end_key_wait("hello"),
          "plain text is not");
    check(!th2::txt_get_text_end_key_wait("hello\\n"),
          "a trailing line break is not");
}

// Multi-byte characters cost one count each, the same as the original's
// two-byte CP932 pairs do.
void test_multibyte()
{
    check_equal(th2::txt_count_text("\xe3\x81\x82\xe3\x81\x84").total, 2,
                "two kana are two counts");
}

}  // namespace

int main()
{
    test_plain_count();
    test_control_codes();
    test_steps();
    test_accumulation();
    test_visible_prefix();
    test_end_key_wait();
    test_multibyte();
    if (failures != 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("text count: all checks passed\n");
    return 0;
}
