#!/usr/bin/env python3
"""Cut a replay capture down to a controller track for the test aid hsvr.ctl_replay (both pretend controllers play
back the recorded motion in the body's frame, looped). Made for two-hand jitter (playtest 2026-11-23), which the rig's
smooth made-up paths never showed.

Usage: ctl_track.py <replay.csv> <out.csv> [--from S] [--to S] [--two]
  --from/--to  seconds from the (first) mark, as replay.py prints them (default: the whole capture)
  --two        keep only the longest stretch where both hands held one weapon (inside from..to)
Prints the stretch it kept and how often the held weapon's spin reversed in it (B&S's jitter measure: a tick where a
spin component reverses with both ticks above 1 rad/s), for comparing a test run against the playtest.
Columns written: t, actor x y z yaw(deg), left controller x y z qx qy qz qw, right controller x y z qx qy qz qw (world, cm).
"""
import argparse
import csv
import math


def quat_mul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx, aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz)


def spin(q0, q1, dt):
    inv = (-q0[0], -q0[1], -q0[2], q0[3])
    d = quat_mul(q1, inv)
    if d[3] < 0:
        d = tuple(-c for c in d)
    s = math.sqrt(d[0] ** 2 + d[1] ** 2 + d[2] ** 2)
    if s < 1e-12:
        return (0.0, 0.0, 0.0)
    ang = 2 * math.atan2(s, d[3])
    return tuple(c / s * ang / dt for c in d[:3])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("replay")
    ap.add_argument("out")
    ap.add_argument("--from", dest="t0", type=float, default=-1e9)
    ap.add_argument("--to", dest="t1", type=float, default=1e9)
    ap.add_argument("--two", action="store_true")
    a = ap.parse_args()
    mark = None
    lines = []
    for line in open(a.replay, encoding="utf-8", errors="replace"):
        if line.startswith("# mark_t:") and mark is None:
            mark = float(line.split(":")[1])
        if not line.startswith("#"):
            lines.append(line)
    rd = list(csv.reader(lines))
    head, rows = rd[0], rd[1:]
    ix = {n: i for i, n in enumerate(head)}
    mark = mark if mark is not None else float(rows[-1][ix["t"]]) - 5.0

    def f(r, n):
        try:
            return float(r[ix[n]])
        except (ValueError, IndexError):
            return None

    keep = []
    for r in rows:
        t = f(r, "t")
        if t is None or not (a.t0 <= t - mark <= a.t1):
            continue
        vals = [f(r, n) for n in ("actor_x", "actor_y", "actor_z", "actor_qx", "actor_qy", "actor_qz", "actor_qw")]
        hands = [[f(r, s + "_ctl_" + c) for c in ("x", "y", "z", "qx", "qy", "qz", "qw")] for s in ("l", "r")]
        if None in vals or None in hands[0] or None in hands[1]:
            continue
        x, y, z, w = vals[3:]
        yaw = math.degrees(math.atan2(2 * (w * z + x * y), 1 - 2 * (y * y + z * z)))
        two = int(f(r, "flags") or 0) & 2
        keep.append((t - mark, vals[:3] + [yaw] + hands[0] + hands[1], two, r))
    if a.two:
        best, cur = [], []
        for k in keep:
            if k[2]:
                cur.append(k)
            else:
                best, cur = (cur if len(cur) > len(best) else best), []
        keep = cur if len(cur) > len(best) else best
    if len(keep) < 2:
        raise SystemExit("nothing to keep")
    # The held weapon's jitter in the kept stretch (the main hand's item: whichever hand holds an item).
    side = "r" if f(keep[0][3], "r_hold_item") not in (None, 0.0, -1.0) else "l"
    revs = ticks = 0
    pw = None
    for k0, k1 in zip(keep, keep[1:]):
        q0 = [f(k0[3], side + "_item_q" + c) for c in "xyzw"]
        q1 = [f(k1[3], side + "_item_q" + c) for c in "xyzw"]
        dt = k1[0] - k0[0]
        if None in q0 or None in q1 or dt <= 0:
            pw = None
            continue
        w = spin(q0, q1, dt)
        if pw is not None:
            ticks += 1
            revs += any(abs(w[i]) > 1 and abs(pw[i]) > 1 and w[i] * pw[i] < 0 for i in range(3))
        pw = w
    with open(a.out, "w", newline="\n") as o:
        o.write("# controller track from %s, %.2f..%.2f s from its mark; playtest jitter %.1f%% of %d ticks (%s hand's item)\n" %
                (a.replay.replace("\\", "/").split("/")[-1], keep[0][0], keep[-1][0], 100.0 * revs / max(1, ticks), ticks, side))
        o.write("t,ax,ay,az,yaw,lx,ly,lz,lqx,lqy,lqz,lqw,rx,ry,rz,rqx,rqy,rqz,rqw\n")
        for t, v, _, _ in keep:
            o.write(",".join(["%.4f" % (t - keep[0][0])] + ["%.4f" % c for c in v]) + "\n")
    print("kept %.2f..%.2f s (%d rows); playtest jitter %.1f%% of %d ticks" % (keep[0][0], keep[-1][0], len(keep), 100.0 * revs / max(1, ticks), ticks))


if __name__ == "__main__":
    main()
