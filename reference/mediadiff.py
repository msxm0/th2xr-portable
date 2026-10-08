#!/usr/bin/env python3
"""Compare the frame checksums and the audio events of the two runs.

Neither is a waveform or a picture: the reference decodes no audio at all, and
storing every frame of a route is over a hundred gigabytes a side.  So the
frame comparison is one CRC per tick, and the audio comparison is the sequence
of sounds the engine asked for.  Both find the tick where the two runs part;
pictures are then dumped for a short window around it.

The reference's clock leads ours by the title-screen lead-in, hence --offset.
"""
import argparse
import collections
import sys


def load_hashes(path, offset):
    out = {}
    with open(path) as handle:
        for line in handle:
            parts = line.split()
            if len(parts) >= 2:
                out[int(parts[0]) - offset] = parts[1]
    return out


def load_audio(path, offset):
    events = collections.defaultdict(list)
    with open(path) as handle:
        for line in handle:
            parts = line.split()
            if len(parts) >= 6:
                events[int(parts[0]) - offset].append(tuple(parts[1:]))
    return events


def canonical(events):
    """Fold away differences that are in the probes, not in the engines.

    Three of them, all from where the probe sits rather than what the engine
    did.  AVG_PlaySE2 calls AVG_PlaySE and AVG_PlayVoice calls AVG_StopVoice,
    so the reference logs a second line our single call site cannot produce.
    The voice scenario is the raw parameter there (-1 meaning "this one") and
    already resolved here.  And the reference's filename has no extension.

    Everything else is compared as logged: folding away a real difference
    here would hide exactly what this trace exists to find.
    """
    for tick, rows in events.items():
        kinds = {row[0] for row in rows}
        out = []
        for row in rows:
            kind = row[0]
            if kind == "voicestop" and "voice" in kinds:
                continue          # AVG_PlayVoice stops the channel first
            if kind == "voice":
                row = (kind, row[1], row[2], row[4])   # drop the scenario
            elif kind == "voicefile":
                name = row[5].rsplit(".", 1)[0]
                row = (kind, name)                     # the file, not the args
            out.append(row)
        events[tick] = out
    return events


def drop_duplicate_se(events):
    """AVG_PlaySE2 calls AVG_PlaySE, so the reference logs both for one sound.

    Our play_se takes one arm or the other and logs once.  The duplicate is an
    artefact of where the probes sit, not a difference between the engines.
    """
    for tick, rows in events.items():
        channelled = {row[2] for row in rows if row[0] == "se2"}
        events[tick] = [row for row in rows
                        if not (row[0] == "se" and row[2] in channelled)]
    return events


def report(name, ours, theirs, limit, lo, hi):
    # Ticks below zero are the reference's title-screen lead-in, which the
    # port has no counterpart for - that is what the offset exists for.
    ticks = sorted(t for t in (set(ours) & set(theirs))
                   if t >= 0 and lo <= t <= hi)
    if not ticks:
        print(f"{name}: no overlapping ticks - nothing compared")
        return 1
    bad = [t for t in ticks if ours[t] != theirs[t]]
    span = f"{ticks[0]}..{ticks[-1]}"
    if not bad:
        print(f"{name}: {len(ticks)} ticks in common ({span}) - all agree")
        return 0
    print(f"{name}: {len(bad)} of {len(ticks)} ticks differ ({span});"
          f" first at our tick {bad[0]}")
    for tick in bad[:limit]:
        print(f"   tick {tick}:")
        print(f"      ours = {ours[tick]}")
        print(f"      ref  = {theirs[tick]}")
    return 1


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--our-hash"), p.add_argument("--ref-hash")
    p.add_argument("--our-audio"), p.add_argument("--ref-audio")
    p.add_argument("--offset", type=int, default=235)
    p.add_argument("--show", type=int, default=5)
    p.add_argument("--from", dest="frm", type=int, default=0)
    p.add_argument("--to", dest="to", type=int, default=10**12)
    args = p.parse_args()

    status = 0
    if args.our_hash and args.ref_hash:
        status |= report("frames",
                         load_hashes(args.our_hash, 0),
                         load_hashes(args.ref_hash, args.offset), args.show,
                         args.frm, args.to)
    if args.our_audio and args.ref_audio:
        ours = canonical(drop_duplicate_se(load_audio(args.our_audio, 0)))
        theirs = canonical(
            drop_duplicate_se(load_audio(args.ref_audio, args.offset)))
        # Only ticks either side thought something happened on.
        keys = set(ours) | set(theirs)
        ours = {t: ours.get(t, []) for t in keys}
        theirs = {t: theirs.get(t, []) for t in keys}
        status |= report("audio", ours, theirs, args.show, args.frm, args.to)
    sys.exit(status)


if __name__ == "__main__":
    main()
