#!/usr/bin/env python3
"""Read a Half Sword VR bug-marker replay (Mods\\HalfSwordVR\\replays\\replay-<time>.csv).

Usage:
  replay.py <file>                       summary: what was held in which hand, button presses,
                                         hitches, jumps, state changes, log lines around the mark
  replay.py <file> --cols REGEX [--from S] [--to S] [--every N]
                                         table of the matching columns, time in seconds from the
                                         (first) mark; e.g. --cols "^r_(item_[xyz]|lag_cm)" --from -1 --to 0.5
  replay.py <file> --at S                every value of the frame nearest S seconds from the mark
  replay.py <file> --log [--from S] [--to S]
                                         the mod's log lines of the window
  replay.py <file> --columns             list the column names

Only the standard library. Positions are cm in Unreal world axes (X forward, Y right, Z up),
rotations quaternions (x y z w). Column groups: head_*, l_*/r_* (left/right hand: ctl_* controller,
target_* where the mod sends the hand, hand_* his posed hand bone, item_* the held item's BaseMesh,
slot_item/hold_item name indexes, lag/turn/force/torque of the hold), actor_*, then one group per
body bone (<bone>_x .. <bone>_qw).
"""
import csv
import math
import re
import sys

FLAG_NAMES = {1: "head tracked", 2: "two hands", 4: "slow-mo", 8: "fallen", 16: "kicking L", 32: "kicking R", 64: "button crouch",
              128: "headset lost focus"}
BUTTON_NAMES = {1: "A/X", 2: "B/Y", 4: "stick click", 8: "menu"}
HOLD_NAMES = {1: "force capped", 2: "torque capped", 4: "sliding", 8: "drawing", 16: "sheathing", 32: "scabbard held", 64: "free grip",
              128: "flipped"}
SIDE = {"l": "left", "r": "right"}


def load(path):
    meta, logs, rows, header = {}, [], [], None
    with open(path, newline="", encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("#log "):
                logs.append(line[5:].rstrip("\n"))
            elif line.startswith("# "):
                k, _, v = line[2:].partition(": ")
                meta.setdefault(k.strip(), []).append(v.strip())
            elif header is None:
                header = next(csv.reader([line]))
            else:
                rows.append(next(csv.reader([line])))
    items = {}
    for part in (meta.get("items", [""])[0]).split("|"):
        if "=" in part:
            i, _, n = part.partition("=")
            items[int(i)] = n
    marks = [float(m) for m in meta.get("mark_t", [])]
    col = {name: i for i, name in enumerate(header or [])}
    return meta, logs, header or [], col, rows, items, marks


def num(row, col, name):
    i = col.get(name)
    if i is None or i >= len(row) or row[i] == "":
        return None
    try:
        return float(row[i])
    except ValueError:
        return None


def item_name(items, idx):
    if idx is None or idx < 0:
        return "nothing"
    return items.get(int(idx), "#%d" % idx)


def bits(value, names):
    v = int(value or 0)
    return [n for b, n in names.items() if v & b]


def dist(a, b):
    if None in a or None in b:
        return None
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def vec(row, col, prefix):
    return tuple(num(row, col, prefix + c) for c in ("x", "y", "z"))


def summary(path):
    meta, logs, header, col, rows, items, marks = load(path)
    if not rows:
        print("no frames")
        return
    t0 = marks[0] if marks else num(rows[0], col, "t")
    rel = lambda r: num(r, col, "t") - t0
    print("Replay %s" % path)
    for k in ("saved_local", "trigger", "map", "vr", "mod_built", "frames", "before_s"):
        if k in meta:
            print("  %-12s %s" % (k, "; ".join(meta[k])))
    if len(marks) > 1:
        print("  marks at   %s s from the first" % ", ".join("%+.2f" % (m - t0) for m in marks))
    dts = [num(r, col, "dt") for r in rows[1:] if num(r, col, "dt")]
    if dts:
        dts_sorted = sorted(dts)
        print("  ticks      %d, %.1f a second mean, worst %.0f ms, 99th pct %.0f ms" % (
            len(rows), len(dts) / sum(dts), max(dts) * 1000, dts_sorted[int(len(dts_sorted) * 0.99) - 1] * 1000))
    print("\nTimes are seconds from the mark (negative = before the press).")

    print("\nHeld items (what the mod's hold had / the game's weapon slot):")
    for s in ("l", "r"):
        last, start = None, None
        segs = []
        for r in rows:
            key = (num(r, col, s + "_hold_item"), num(r, col, s + "_slot_item"))
            if key != last:
                if last is not None:
                    segs.append((start, rel(r), last))
                last, start = key, rel(r)
        segs.append((start, rel(rows[-1]), last))
        for a, b, (h, g) in segs:
            print("  %-5s %+7.2f .. %+7.2f  hold %s, slot %s" % (SIDE[s], a, b, item_name(items, h), item_name(items, g)))

    print("\nEvents:")
    events = []
    prev = None
    for r in rows:
        t = rel(r)
        if prev is not None:
            fl, pfl = int(num(r, col, "flags") or 0), int(num(prev, col, "flags") or 0)
            for b, n in FLAG_NAMES.items():
                if (fl ^ pfl) & b:
                    events.append((t, "%s %s" % (n, "on" if fl & b else "off")))
            for s in ("l", "r"):
                bt, pbt = int(num(r, col, s + "_buttons") or 0), int(num(prev, col, s + "_buttons") or 0)
                for b, n in BUTTON_NAMES.items():
                    if (bt ^ pbt) & b:
                        events.append((t, "%s %s %s" % (SIDE[s], n, "down" if bt & b else "up")))
                hd, phd = int(num(r, col, s + "_hold") or 0), int(num(prev, col, s + "_hold") or 0)
                for b, n in HOLD_NAMES.items():
                    if b in (1, 2):
                        continue  # caps flicker every tick; see --cols
                    if (hd ^ phd) & b:
                        events.append((t, "%s hand: %s %s" % (SIDE[s], n, "on" if hd & b else "off")))
                for g, name in (("trigger", "trigger"), ("squeeze", "grip")):
                    v, pv = num(r, col, s + "_" + g), num(prev, col, s + "_" + g)
                    if v is not None and pv is not None and (v > 0.7) != (pv > 0.7):
                        events.append((t, "%s %s %s" % (SIDE[s], name, "pulled" if v > 0.7 else "let go")))
                # A held item or the posed hand jumping more than 25 cm in one tick.
                for part, what in (("item_", "item"), ("hand_", "hand bone")):
                    d = dist(vec(r, col, s + "_" + part), vec(prev, col, s + "_" + part))
                    if d is not None and d > 25 and num(r, col, s + "_hold_item") == num(prev, col, s + "_hold_item"):
                        events.append((t, "%s %s jumped %.0f cm in one tick" % (SIDE[s], what, d)))
            dt = num(r, col, "dt")
            if dt and dt > 0.05:
                events.append((t, "hitch: tick took %.0f ms" % (dt * 1000)))
        prev = r
    for s in ("l", "r"):
        worst = max(rows, key=lambda r: num(r, col, s + "_lag_cm") or 0)
        lag = num(worst, col, s + "_lag_cm") or 0
        if lag > 15:
            events.append((rel(worst), "%s hold's worst lag %.0f cm behind the controller" % (SIDE[s], lag)))
    for m in marks:
        events.append((m - t0, "*** MARK (button pressed) ***"))
    for t, e in sorted(events, key=lambda x: x[0]):
        print("  %+7.2f  %s" % (t, e))

    near = [l for l in logs if _log_t(l) is not None and -2.0 <= _log_t(l) - t0 <= 0.5]
    if near:
        print("\nLog lines from 2 s before to 0.5 s after the mark (all of them: --log):")
        for l in near[-60:]:
            print("  %+7.2f  %s" % (_log_t(l) - t0, l.split("] ", 2)[-1]))


def _log_t(line):
    m = re.match(r"\[\s*([0-9.]+)\]", line)
    return float(m.group(1)) if m else None


def window(args):
    a = {"from": -1e9, "to": 1e9}
    for k in ("from", "to"):
        if "--" + k in args:
            a[k] = float(args[args.index("--" + k) + 1])
    return a["from"], a["to"]


def main(argv):
    if len(argv) < 2 or argv[1] in ("-h", "--help"):
        print(__doc__)
        return 1
    path, args = argv[1], argv[2:]
    if not args:
        summary(path)
        return 0
    meta, logs, header, col, rows, items, marks = load(path)
    t0 = marks[0] if marks else num(rows[0], col, "t")
    lo, hi = window(args)
    if "--columns" in args:
        print("\n".join(header))
    elif "--log" in args:
        for l in logs:
            t = _log_t(l)
            if t is not None and lo <= t - t0 <= hi:
                print("%+7.3f  %s" % (t - t0, l))
    elif "--at" in args:
        s = float(args[args.index("--at") + 1])
        r = min(rows, key=lambda r: abs(num(r, col, "t") - t0 - s))
        for name, v in zip(header, r):
            extra = ""
            if name.endswith("_item") and v not in ("", None):
                extra = "  (%s)" % item_name(items, int(float(v)))
            print("%-24s %s%s" % (name, v, extra))
    elif "--cols" in args:
        rx = re.compile(args[args.index("--cols") + 1])
        every = int(args[args.index("--every") + 1]) if "--every" in args else 1
        names = [n for n in header if rx.search(n) and n != "t"]
        print("\t".join(["t_rel"] + names))
        for i, r in enumerate(rows):
            t = num(r, col, "t") - t0
            if lo <= t <= hi and i % every == 0:
                print("\t".join(["%+.4f" % t] + [r[col[n]] if col[n] < len(r) else "" for n in names]))
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
