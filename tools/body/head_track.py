#!/usr/bin/env python3
"""Cut a bug-marker replay down to a headset track for the test aid hsvr.head_replay (the pretend headset plays back
the recorded head relative to the capsule and its heading, looped), so the body solver is tested on Matthew's own
crouches and leans.

Usage: head_track.py <replay.csv> <out.csv> [--from S] [--to S]
  --from/--to  seconds from the (first) mark, as replay.py prints them (default: the whole capture)
Columns written: t, actor x y z yaw(deg), head x y z qx qy qz qw (world, cm).
"""
import argparse
import csv
import math


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("replay")
    ap.add_argument("out")
    ap.add_argument("--from", dest="t0", type=float, default=-1e9)
    ap.add_argument("--to", dest="t1", type=float, default=1e9)
    a = ap.parse_args()
    mark = 0.0
    header = None
    rows = []
    with open(a.replay, newline="", encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("# mark_t:"):
                mark = float(line.split(":", 1)[1])
                continue
            if line.startswith("#"):
                continue
            r = next(csv.reader([line]))
            if header is None:
                header = {n: i for i, n in enumerate(r)}
                continue
            rows.append(r)

    def g(r, n):
        i = header.get(n)
        if i is None or i >= len(r) or r[i] == "":
            return None
        return float(r[i])

    out = []
    for r in rows:
        t = g(r, "t")
        if t is None or not (a.t0 <= t - mark <= a.t1):
            continue
        hx = g(r, "head_x")
        if hx is None:
            continue
        qz, qw = g(r, "actor_qz"), g(r, "actor_qw")
        yaw = math.degrees(2.0 * math.atan2(qz, qw))
        out.append([t - mark, g(r, "actor_x"), g(r, "actor_y"), g(r, "actor_z"), yaw, hx, g(r, "head_y"), g(r, "head_z"),
                    g(r, "head_qx"), g(r, "head_qy"), g(r, "head_qz"), g(r, "head_qw")])
    with open(a.out, "w", newline="") as f:
        f.write("t,actor_x,actor_y,actor_z,yaw,head_x,head_y,head_z,head_qx,head_qy,head_qz,head_qw\n")
        for o in out:
            f.write(",".join("%.4f" % v for v in o) + "\n")
    print("%d rows, %.2f s" % (len(out), out[-1][0] - out[0][0] if out else 0.0))


if __name__ == "__main__":
    main()
