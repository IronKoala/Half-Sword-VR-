#!/usr/bin/env python3
"""Two-hand test cases with REAL-hand conditions (playtest 2026-11-23: two-handing worked on the rig and
failed in his hands). The old cases put the second fake hand exactly where the weapon's second grip was, with
its wrist turned as the first hand had it. Real hands are 15-33 cm further apart than the grips and the second
wrist is 30-90 deg off the first's; that is what made the two hands' pulls fight.

Each case: weapon in the right hand, the left hand takes it near its second spot, then
  static (hands on it), spread (left controller 20 cm further away + wrist rolled 70 deg),
  slow swing, fast swing (the left controller rigid to the right one), back to rest.
Checks (hsvr.expect on the mod's measures, worst since hsvr.two_reset):
  two_line_max  deg between the weapon's grip-to-grip line and the hands' line
  two_off_max   cm either hand is off the weapon's line (across it)
  two_gap       cm between the two hands' points on the weapon (>= 9: never inside each other)
  two_hand      the second hand still holds it

Usage: make_steps.py weapons.txt   (lines: name|asset class path|left hand cm along the weapon from the right|front for the crossbow)
Writes steps-<name>.txt and list.txt next to this file.
"""
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent

HEAD = """0.5:hsvr.fake_head_drop 0
0.6:hsvr.fake_look 0
1:hsvr.squeeze 0 1
1:hsvr.squeeze 1 1
1.5:hsvr.squeeze 0 0
1.5:hsvr.squeeze 1 0
2.4:hsvr.expect held_r = 0 right hand empty
2.5:hsvr.arm_move 1
3:hsvr.squeeze 1 1
3.5:hsvr.give_weapon {path}
5:hsvr.weapon_dump 1
5.5:hsvr.expect held_r = 1 weapon in the right hand
"""
TAKE_ALONG = """6:hsvr.arm_move 7 {along}
8:hsvr.squeeze 0 1
"""
TAKE_FRONT = """6:hsvr.xbow_hand 0 front
9:hsvr.squeeze 0 1
"""
BODY = """10:hsvr.expect two_hand = 1 second hand took it
10:hsvr.expect two_gap >= 8.9 hands a hand's width apart, never inside each other
10.1:hsvr.xbow_hand 0 keep
10.2:hsvr.two_reset
12:hsvr.expect two_line_max < 6 static: along the hands
12:hsvr.expect two_off_max < 3 static: hands on its line
12.1:hsvr.hand_spread 0 20
12.1:hsvr.xbow_wrist 0 roll 70 1
13.5:hsvr.two_reset
15.5:hsvr.expect two_hand = 1 spread + wrist: still held
15.5:hsvr.expect two_line_max < 8 spread + wrist: along the hands
15.5:hsvr.expect two_off_max < 5 spread + wrist: hands on its line
15.6:hsvr.arm_move 2
15.7:hsvr.two_reset
20:hsvr.expect two_hand = 1 slow swing: still held
20:hsvr.expect two_line_max < 15 slow swing: along the hands
20:hsvr.expect two_off_max < 8 slow swing: hands on its line
20.1:hsvr.arm_move 3
20.2:hsvr.two_reset
24:hsvr.expect two_hand = 1 fast swing: still held
24:hsvr.expect two_line_max < 30 fast swing: along the hands
24:hsvr.expect two_off_max < 15 fast swing: hands on its line
24.1:hsvr.arm_move 1
24.1:hsvr.hand_spread 0 0
24.1:hsvr.xbow_wrist 0 roll 0 0.5
25.5:hsvr.two_reset
27.5:hsvr.expect two_hand = 1 rest: still held
27.5:hsvr.expect two_line_max < 6 rest: along the hands
27.5:hsvr.expect two_off_max < 3 rest: hands on its line
27.6:hsvr.expect two_gap >= 8.9 rest: hands apart
28:quit
"""


def main():
    rows = []
    for line in Path(sys.argv[1]).read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        name, path, along = [p.strip() for p in line.split("|")]
        steps = HEAD.format(path=path) + (TAKE_FRONT if along == "front" else TAKE_ALONG.format(along=along)) + BODY
        (HERE / ("steps-%s.txt" % name)).write_text(steps, newline="\n")
        rows.append(name)
    lst = ["# Two hands with real-hand conditions (playtest 2026-11-23). Control run: the same with pose_two_hand_joint_line=0 must FAIL."]
    lst += ["two_%s|steps-%s.txt" % (n, n) for n in rows]
    lst += ["ctl_%s|steps-%s.txt|pose_two_hand_joint_line=0" % (n, n) for n in rows]
    (HERE / "list.txt").write_text("\n".join(lst) + "\n", newline="\n")
    print("wrote %d cases (+%d control)" % (len(rows), len(rows)))


if __name__ == "__main__":
    main()
