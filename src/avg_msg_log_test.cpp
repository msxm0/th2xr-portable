// The engine's log in a save: AvgMsg::write_log / read_log carry NovelBuf -
// every entry, its voices and where the ring stands - and nothing of the
// mouse layer or rects, and a loaded log is at the current line rather than
// paging back.  clear_log is what a new game starts from.

#include "avg_msg.hpp"

#include <cstdio>
#include <sstream>
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

}  // namespace

int main()
{
    th2::Display display{nullptr};
    th2::BackStruct back{};
    th2::AvgMsg source(display, back, {});
    source.set_novel_message_voice1(10, 20, 3, 0);
    source.set_novel_message("\xE3\x80\x8C" "First line" "\xE3\x80\x8D", 0);
    source.set_novel_message("Second\\nline", 0);
    source.set_novel_message("Third", 0);

    std::stringstream saved;
    source.write_log(saved);

    th2::AvgMsg loaded(display, back, {});
    check(loaded.read_log(saved), "read_log accepts what write_log wrote");
    const auto& a = source.novel_buf();
    const auto& b = loaded.novel_buf();
    check(a.bmax == b.bmax, "bmax");
    check(a.bpoint == b.bpoint, "bpoint");
    check(b.bcount == 0, "a loaded log is at the current line");
    bool entries = true;
    bool voices = true;
    for (std::size_t i = 0; i < a.buf.size(); ++i) {
        entries = entries && a.buf[i] == b.buf[i];
        voices = voices && a.nv[i].size() == b.nv[i].size();
        for (std::size_t k = 0; voices && k < a.nv[i].size(); ++k) {
            const auto& x = a.nv[i][k];
            const auto& y = b.nv[i][k];
            voices = x.sno == y.sno && x.vno == y.vno && x.cno == y.cno
                && x.a_cut == y.a_cut && x.vstcount == y.vstcount
                && x.px == y.px && x.py == y.py;
        }
    }
    check(entries, "every entry's text");
    check(voices, "every entry's voices");
    check(a.bmax >= 3, "three lines were logged");

    std::stringstream garbage("not a log");
    th2::AvgMsg untouched(display, back, {});
    check(!untouched.read_log(garbage), "read_log refuses what is not a log");

    loaded.clear_log();
    check(loaded.novel_buf().bmax == 0 && loaded.novel_buf().bpoint == 0,
          "clear_log empties the ring");

    if (failures == 0) {
        std::printf("avg-msg-log: ok\n");
    }
    return failures == 0 ? 0 : 1;
}
