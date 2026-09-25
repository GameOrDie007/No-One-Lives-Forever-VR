"""Did a long run stay up, stay stereo, and keep drawing?

The M5 gate asks for "15 minutes continuous with no exceptions and no mono
fallback". Every capture this project takes is 24 seconds long, which cannot see
a leak, a slow stall, or a stereo path that gives up after ten minutes. This
reads the run's own log and answers the gate's three questions.

  python tools/soak-check.py game/logs/20260920-085500

WHAT IT CHECKS
  1. EXCEPTIONS          any exception/assert/fatal line.
  2. MONO FALLBACK       "world renders per frame -> N". Two is stereo. A later
                         line saying 1 is the fallback the gate forbids, and it
                         is the one failure a screenshot at the end would miss
                         entirely - a mono frame looks perfectly fine.
  3. STALLS              measured in FRAMES, not in log lines. A quiet log is
                         not a frozen game: the first version of this called the
                         3.7-second level load a stall and failed a healthy run,
                         which is the brightness-test mistake in a new coat. A
                         gap only counts when the frame counter stopped moving
                         through it.
Reported, not judged: elapsed, frames, mean fps.

A LOG IS NOT ONE SESSION. The engine frees and reloads the client DLL on every
focus loss, and the log's clock restarts at 0.000 each time - so a headset
session that ever lost focus is several runs concatenated. Taking the first
timestamp and the last gave 2,719,278 frames in 21 seconds, or 131,799 fps, on a
real 335 MB log, and said VERDICT with a straight face. This now splits on the
clock going backwards and judges the LONGEST segment, and refuses outright if the
rate it computes is impossible.
"""
import sys, os, re

LINE = re.compile(r"^\[\s*([0-9.]+)\]\s+F(\d+)\s+(.*)$")
BAD  = re.compile(r"exception|assert|fatal|access violation|unhandled", re.I)
RENDERS = re.compile(r"world renders per frame -> (\d+)")
# Lines that TALK about exceptions without being one. The client narrates its
# own exception handling, so a naive grep reports a healthy run as broken.
BENIGN = re.compile(r"restore camera|on exception|structured exception handler"
                    r"|exception-safe|no exception", re.I)


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else None
    if not d or not os.path.isdir(d):
        raise SystemExit("usage: soak-check.py <game/logs/RUNDIR>")
    p = os.path.join(d, "client.log")
    if not os.path.exists(p):
        raise SystemExit(f"no client.log in {d}")

    # Segments: a new one starts wherever the clock goes BACKWARDS, which is a
    # DLL reload, not time travel.
    segs = []          # [t0, t1, f0, f1]
    prev_t = None
    prev_f = None
    worst_fps = 0.0
    worst_gap = 0.0
    worst_at = 0.0
    bad = []
    renders = []

    for raw in open(p, encoding="latin-1", errors="replace"):
        m = LINE.match(raw.rstrip("\n"))
        if not m:
            continue
        t = float(m.group(1)); f = int(m.group(2)); msg = m.group(3)
        if prev_t is None or t < prev_t - 0.5:
            segs.append([t, t, f, f])       # first line, or the clock reset
            prev_t = prev_f = None
        else:
            segs[-1][1] = t; segs[-1][3] = f
        if prev_t is not None:
            gap = t - prev_t
            # A GAP IN THE LOG IS NOT A GAP IN THE GAME. Nothing logs during a
            # level load, so the quiet 3.7s while the quick save loads looks
            # identical to a freeze unless you ask whether FRAMES advanced.
            if gap > 1.0:
                advanced = f - prev_f
                fps_in_gap = advanced / gap
                if fps_in_gap < 5.0 and gap > worst_gap:
                    worst_gap, worst_at, worst_fps = gap, t, fps_in_gap
        prev_t, prev_f = t, f
        if BAD.search(msg) and not BENIGN.search(msg):
            bad.append((t, msg[:110]))
        r = RENDERS.search(msg)
        if r:
            renders.append((t, int(r.group(1))))

    if not segs:
        raise SystemExit("client.log has no timestamped lines")

    # The longest segment is the run worth judging; the rest are restarts.
    segs.sort(key=lambda s_: s_[1] - s_[0], reverse=True)
    t0, t1, f0, f1 = segs[0]
    elapsed = t1 - t0
    frames = f1 - f0
    fps = frames / elapsed if elapsed > 0 else 0.0

    print(f"run      {d}")
    if len(segs) > 1:
        print(f"         {len(segs)} sessions in this log (the client DLL reloads on focus"
              f" loss); judging the longest")
    print(f"elapsed  {elapsed/60:.1f} min ({elapsed:.0f}s)   frames {frames}   mean {fps:.1f} fps")

    ok = True
    # AN IMPOSSIBLE RATE MEANS THE PARSE IS WRONG, NOT THAT THE GAME WAS FAST.
    # Say so instead of printing a verdict on numbers that cannot be true.
    if fps > 500.0 or elapsed <= 0.0:
        print("\nPARSE LOOKS WRONG - that rate is not achievable. Not judging this log.")
        return 2
    if bad:
        ok = False
        print(f"\nEXCEPTIONS: {len(bad)}")
        for t, m in bad[:10]:
            print(f"   [{t:8.1f}] {m}")
    else:
        print("exceptions   none")

    if not renders:
        print("stereo       NO 'world renders per frame' line - cannot confirm stereo")
        ok = False
    else:
        mono = [(t, n) for t, n in renders if n < 2]
        first = renders[0][1]
        print(f"stereo       {first} world renders per frame at start, "
              f"{len(renders)} state line(s)")
        if mono:
            ok = False
            print(f"             MONO FALLBACK at t={mono[0][0]:.1f}s -> {mono[0][1]} render(s)")

    if worst_gap > 0:
        print(f"worst stall  {worst_gap:.2f}s at t={worst_at:.0f}s "
              f"({worst_fps:.1f} fps through it)", end="")
        if worst_gap > 2.0:
            print("   <== a freeze this long is not stable")
            ok = False
        else:
            print("")
    else:
        print("stalls       none (no gap where the frame counter stopped)")

    print("\nVERDICT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
