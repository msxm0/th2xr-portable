#!/usr/bin/env python3
"""Find glyphs whose alpha goes *down*.

The typewriter only ever brightens a character: alph2 = LIM(text_cnt-cnt2,
0, 16)*16 is clamped, so once a glyph is sixteen counts behind the cursor it
stays at 256 forever, and `if(step<step_cnt) alph2 = 256` pins every glyph of
an earlier \\k step there regardless.  A glyph that dims from one tick to the
next is therefore a bug by construction - no comparison needed, though the
reference is run through the same check to prove the rule.

    refade.py <state.txt> [--label NAME]
"""
import argparse, sys

def main():
    p = argparse.ArgumentParser()
    p.add_argument("state")
    p.add_argument("--label", default=None)
    args = p.parse_args()
    label = args.label or args.state

    rows = []
    with open(args.state, errors="replace") as f:
        header = f.readline().split()
        idx = header.index("text")
        for line in f:
            parts = line.split()
            if len(parts) != len(header):
                continue
            rows.append((int(parts[0]), parts[idx]))

    events = 0
    shrank = 0
    first = None
    prev_tick, prev = None, None
    for tick, text in rows:
        if text == "-":
            prev_tick, prev = tick, None
            continue
        # Every dim is reported, and a shrinking string is flagged rather
        # than skipped.  An earlier version filtered those out as "probably a
        # different text object" - and that filter threw away the one real
        # re-fade in the trace, because the bug shortened the string at the
        # same moment it dimmed the glyphs.  A detector that hides what it
        # cannot classify is worse than one that reports too much.
        if prev is not None:
            for i in range(min(len(prev), len(text))):
                if int(text[i], 36) < int(prev[i], 36):
                    events += 1
                    if first is None:
                        first = (prev_tick, tick, i, prev[i], text[i],
                                 prev, text)
                    break
        if prev is not None and len(text) < len(prev):
            shrank += 1
        prev_tick, prev = tick, text

    if first is None:
        print(f"{label}: no glyph ever dims across {len(rows)} ticks"
              + (f" ({shrank} shrink events)" if shrank else ""))
        return 0
    a, b, i, was, now, ps, ts = first
    print(f"{label}: {events} ticks where a glyph dims; first at tick {b}")
    print(f"  glyph {i} went {was} -> {now} between tick {a} and {b}")
    print(f"  tick {a}: {ps}")
    print(f"  tick {b}: {ts}")
    if shrank:
        print(f"  ({shrank} ticks where the string also got shorter - either a"
              f" different text object, or a reveal that lost glyphs)")
    return 1

if __name__ == "__main__":
    sys.exit(main())
