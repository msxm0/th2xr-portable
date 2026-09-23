#!/usr/bin/env python3
"""Compare the two sides' state traces field by field.

Both runs write state.txt with the same named columns (reference:
shim/th2ref_state.cpp, ours: Game::trace_dump_state).  This lines them up on
the scenario tick and reports, per field, the first tick they disagree on -
which is the question a pixel diff can never answer, because most of this
game is a black screen where everything and nothing matches.

    statediff.py <ours/state.txt> <ref/state.txt> --offset N [--from N]
                 [--to N] [--fields a,b,c] [--context N]
"""
import argparse
import sys


def read_header(path):
    with open(path) as f:
        return f.readline().split()


def rows(path):
    """Stream (tick, row) in file order.

    Streamed rather than loaded because a route is not a scene: at six hundred
    thousand ticks a side, two dicts of thirty-six string fields per tick is
    several gigabytes, and the comparison died before it could report
    anything.  Both files are written a line at a time in ascending tick
    order, so they can be walked in step instead.
    """
    with open(path) as f:
        header = f.readline().split()
        for line in f:
            parts = line.split()
            if len(parts) != len(header):
                continue
            yield int(parts[0]), dict(zip(header, parts))


def walk(ours_path, ref_path, offset, first, last):
    """Yield (tick, ours_row, ref_row) for ticks both sides have, in order."""
    theirs = rows(ref_path)
    their_tick, their_row = next(theirs, (None, None))
    for tick, row in rows(ours_path):
        if tick < first:
            continue
        if tick > last:
            break
        want = tick + offset
        while their_tick is not None and their_tick < want:
            their_tick, their_row = next(theirs, (None, None))
        if their_tick is None:
            return
        if their_tick == want:
            yield tick, row, their_row


def main():
    p = argparse.ArgumentParser()
    p.add_argument("ours")
    p.add_argument("ref")
    p.add_argument("--offset", type=int, default=0,
                   help="add to our tick to get the reference's")
    p.add_argument("--from", dest="first", type=int, default=0)
    p.add_argument("--to", dest="last", type=int, default=10**9)
    p.add_argument("--fields", default=None,
                   help="comma-separated subset to check")
    p.add_argument("--context", type=int, default=3,
                   help="ticks of both sides to print around a first divergence")
    p.add_argument("--max-idle", type=int, default=5000,
                   help="fail if the script sits on one instruction this long "
                        "(0 disables)")
    args = p.parse_args()

    header = read_header(args.ours)
    fields = args.fields.split(",") if args.fields else [
        f for f in header if f != "tick"]

    first_bad = {}
    counts = {}
    shared_count = 0
    shared_first = shared_last = None
    # Two engines parked on the same instruction agree about everything, and
    # that is not a pass - it is the harness proving nothing over however many
    # ticks nobody moved.  A reference wedged in the map screen sat on
    # 020000100.sdt@0 for a quarter of a million ticks, and every counter
    # would have matched for all of them.
    idle_at = None
    idle_from = 0
    worst_idle = 0
    worst_idle_at = None
    for tick, a, b in walk(args.ours, args.ref, args.offset,
                           args.first, args.last):
        shared_count += 1
        here = (a.get("script"), a.get("pc"))
        if here != idle_at:
            idle_at, idle_from = here, tick
        elif tick - idle_from > worst_idle:
            worst_idle = tick - idle_from
            worst_idle_at = here
        if shared_first is None:
            shared_first = tick
        shared_last = tick
        for field in fields:
            mine, theirs = a.get(field), b.get(field)
            if field == "script":
                # The two engines store the running script's name with
                # different case.  That is how they spell it, not what they
                # are doing, so compare it case-insensitively.
                mine = (mine or "").lower()
                theirs = (theirs or "").lower()
            if mine != theirs:
                counts[field] = counts.get(field, 0) + 1
                first_bad.setdefault(field, (tick, a.get(field), b.get(field)))

    if not shared_count:
        sys.exit("no overlapping ticks (check --offset)")
    print(f"{shared_count} ticks in common "
          f"(ours {shared_first}..{shared_last}, offset {args.offset:+d})")

    # A range that stops short is a failure, not a pass.  If one side's trace
    # ends early - a run killed by its wall-clock budget, a reference stopped
    # before it reached the end - the ticks past that point are simply absent
    # from `shared`, every tick that IS there agrees, and the sweep reports
    # success over a range it never looked at.  That is the worst thing this
    # harness can do, so it is checked rather than assumed.
    if args.last < 10**9 and shared_last < args.last:
        sys.exit(f"traces stop at our tick {shared_last}, short of the "
                 f"requested {args.last} - one side ended early, so this "
                 f"run proves nothing about the rest")

    if args.max_idle and worst_idle >= args.max_idle:
        where = f"{worst_idle_at[0]}@{worst_idle_at[1]}" if worst_idle_at else "?"
        sys.exit(
            f"the script sat on {where} for {worst_idle} ticks - both sides "
            f"were parked, so whatever they agreed about here they agreed "
            f"about while doing nothing")

    if not first_bad:
        print(f"all {len(fields)} fields agree on all {shared_count} ticks")
        return 0

    width = max(len(f) for f in first_bad)
    print(f"\n{'field':{width}}  {'first':>7}  {'ours':>14}  {'ref':>14}  ticks")
    for field, (tick, mine, theirs) in sorted(
            first_bad.items(), key=lambda kv: kv[1][0]):
        print(f"{field:{width}}  {tick:7d}  {mine:>14}  {theirs:>14}  "
              f"{counts[field]}")

    earliest = min(t for t, _, _ in first_bad.values())
    print(f"\nfirst divergence at our tick {earliest} "
          f"(reference tick {earliest + args.offset}):")
    show = [f for f in fields if first_bad.get(f, (None,))[0] == earliest]
    print("  fields:", ", ".join(show))
    lo, hi = earliest - args.context, earliest + args.context
    cols = ["tick"] + show
    print("  " + "  ".join(f"{c:>10}" for c in cols))
    # A second walk rather than a remembered one: the window is a handful of
    # ticks and the file may be a million.
    for tick, a, b in walk(args.ours, args.ref, args.offset, lo, hi):
        for side, row, key in (("ours", a, tick),
                               ("ref ", b, tick + args.offset)):
            values = [str(key)] + [row.get(f, "?") for f in show]
            print(f"  {side} " + "  ".join(f"{v:>10}" for v in values))
    return 1


if __name__ == "__main__":
    sys.exit(main())
