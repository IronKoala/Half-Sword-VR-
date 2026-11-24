#!/usr/bin/env python3
"""Two-hand test cases with REAL-hand conditions (playtest 2026-11-23: two-handing worked on the rig and
failed in his hands). The old cases put the second fake hand exactly where the weapon's second grip was, with
its wrist turned as the first hand had it. Real hands are 15-33 cm further apart than the grips and the second
wrist is 30-90 deg off the first's; that is what made the two hands' pulls fight.

Each case: weapon in the right hand, the left hand takes it near its second spot, then
  static (hands on it), spread (left controller 20 cm further away + wrist rolled 70 deg),
  slow swing, fast swing (the left controller rigid to the right one), back to rest.
Checks (hsvr.expect on the mod's measures, worst since hsvr.two_reset):
  two_line_max  deg between the weapon's grip-to-grip line and the two-hand pose the mod asks for (the hands'
                line on wide grips and the crossbow; mostly the main wrist on close sword grips)
  two_off_max   cm either grip spot of that pose is off the weapon's line (across it)
  two_wrist     deg between the weapon and where the main wrist alone points it (swords' wrist phase)
  two_gap       cm between the two hands' points on the weapon (>= 9: never inside each other)
  two_hand      the second hand still holds it

Usage: make_steps.py weapons.txt [tracks folder on the PC]   (lines: name|asset class path|left hand cm along the weapon from the right|front for the crossbow)
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
12:hsvr.expect two_line_max < 6 static: follows the two-hand pose
12:hsvr.expect two_off_max < 3 static: grips on the pose
12.1:hsvr.hand_spread 0 20
12.1:hsvr.xbow_wrist 0 roll 70 1
13.5:hsvr.two_reset
15.5:hsvr.expect two_hand = 1 spread + wrist: still held
15.5:hsvr.expect two_line_max < 8 spread + wrist: follows the two-hand pose
15.5:hsvr.expect two_off_max < 5 spread + wrist: grips on the pose
15.6:hsvr.arm_move 2
15.7:hsvr.two_reset
20:hsvr.expect two_hand = 1 slow swing: still held
20:hsvr.expect two_line_max < 15 slow swing: follows the two-hand pose
20:hsvr.expect two_off_max < 8 slow swing: grips on the pose
20.1:hsvr.arm_move 3
20.2:hsvr.two_reset
24:hsvr.expect two_hand = 1 fast swing: still held
24:hsvr.expect two_line_max < 30 fast swing: follows the two-hand pose
24:hsvr.expect two_off_max < 15 fast swing: grips on the pose
24.1:hsvr.arm_move 1
24.1:hsvr.hand_spread 0 0
24.1:hsvr.xbow_wrist 0 roll 0 0.5
25.5:hsvr.two_reset
27.5:hsvr.expect two_hand = 1 rest: still held
27.5:hsvr.expect two_line_max < 6 rest: follows the two-hand pose
27.5:hsvr.expect two_off_max < 3 rest: grips on the pose
27.6:hsvr.expect two_gap >= 8.9 rest: hands apart
28:quit
"""
# B&S two hands (pose_two_hand_bs=1): the limits are B&S's own, measured in his greatsword session 2026-11-23
# (private\bs-2h-2026-11-23\an2h_bins.py, KingSword 6 kg + GreatSwordA, p90 of the larger, rest = both hands still >= 0.5 s p97):
# blade vs the line between his hands rest 43 / still 55 / slow 61 / fast 57 deg; second grip off its hand rest 11 / still 20 /
# slow 19 / fast 26 cm. B&S keeps the blade on the MAIN WRIST, not on the hands' line, so the line-mode limits (6-30 deg) do not
# apply (run 456cfc5: B&S-held swords sat 10-21 deg off the line at rest, 33-40 in swings; all failed the line limits).
# two_jitter_own: spin reversals not matched by the hands' own reversal; B&S 1.23% / 0.99% of two-hand ticks.
BS_LIMITS = [
    ("two_line_max < 6 static", "two_line_max < 44 static"),
    ("two_off_max < 3 static", "two_off_max < 11 static"),
    ("two_line_max < 8 spread", "two_line_max < 55 spread"),
    ("two_off_max < 5 spread", "two_off_max < 20 spread"),
    ("two_line_max < 15 slow", "two_line_max < 61 slow"),
    ("two_off_max < 8 slow", "two_off_max < 19 slow"),
    ("two_line_max < 30 fast", "two_line_max < 57 fast"),
    ("two_off_max < 15 fast", "two_off_max < 26 fast"),
    ("24:hsvr.expect two_off_max < 26 fast swing: grips on the pose\n", "24:hsvr.expect two_off_max < 26 fast swing: grips on the pose\n24:hsvr.expect two_jitter_own < 1.5 fast swing: spin reversals not the hands' own (B&S 1.2%)\n"),
    ("two_line_max < 6 rest", "two_line_max < 44 rest"),
    ("two_off_max < 3 rest", "two_off_max < 11 rest"),
]


def bs_limits(steps):
    for a, b in BS_LIMITS:
        assert a in steps, a
        steps = steps.replace(a, b)
    return steps.replace("follows the two-hand pose", "within B&S's own gap to the hands' line").replace("grips on the pose", "second grip within B&S's gap")


# Swords only (grips close together): the main wrist alone points the tip, the second hand stays put
# (playtest 2026-11-23: "I can't properly point the tip outward"). The left controller stops riding the
# right one and holds where it is; the right wrist pitches 35 deg, then a fast 50 deg cut and back.
WRIST = """28:hsvr.xbow_hand 0 off
28:hsvr.arm_move 7 {along}
28.1:hsvr.two_reset
29:hsvr.xbow_wrist 1 pitch 35 1
31:hsvr.expect two_hand = 1 wrist point: still held
31:hsvr.expect two_wrist < 12 wrist point: the tip goes where the main wrist points it
31.1:hsvr.xbow_wrist 1 pitch -15 0.15
31.2:hsvr.two_reset
32:hsvr.expect two_hand = 1 wrist cut: still held
32.5:hsvr.expect two_wrist < 12 wrist cut: settled where the wrist points
32.6:hsvr.xbow_wrist 1 pitch 0 0.5
34:quit
"""


# His own recorded controller motion (hsvr.ctl_replay, tracks/ cut from his playtest replays by tools/replay/ctl_track.py):
# playtest 2026-11-23 19:00Z, two-handing "very jittery ... very unintuitive" while the rig's smooth made-up paths passed.
# name | weapon (a weapons.txt name) | track file | left hand cm along | worst mean turn off the main wrist (deg) | worst mean grip lag (cm)
# The limits are B&S's own two-hand main-hand gaps (bs_hold_measured.md section 3: 1-3 m/s 16 deg / 10.1 cm, >3 m/s 34 / 15.8);
# the playtest (line mode, 457f284 + others) had waggle 33.0 / 12.6, polearm 23.7 / 6.8, mace 25.1 / 6.3 in the same windows.
REPLAYS = [
    # The waggle is a 5 Hz shake at 2-4 m/s and ~2000 deg/s: B&S's >3 m/s gaps (34 deg / 15.8 cm) are its yardstick.
    ("sword_waggle", "longsword", "sword2h-waggle.csv", "-12", 34, 15.8),
    ("polearm", "polehammer", "polearm.csv", "55", 16, 10),
    # (his 2H mace hands were 30 cm toward the pommel; the war staff's second spot is toward its far end, run 57719a0: -30 never took hold)
    ("mace2h", "warstaff", "mace2h.csv", "30", 16, 10),
]
# hold_rebase: the main hold keeps the weapon turned as it is once his wrists play (the rig's grab turn is not his:
# polehammer aimed 135 deg off his hands, runs 4750e14 / 6cec1f5).
REPLAY = """10:hsvr.expect two_hand = 1 second hand took it
10.2:hsvr.ctl_replay {track} 0.5
11.3:hsvr.hold_rebase 1
11.5:hsvr.two_reset
21.5:hsvr.expect two_hand = 1 his motion: still held in both hands
21.5:hsvr.expect two_turn_mean < {turn} his motion: weapon follows the main wrist (mean deg off it)
21.5:hsvr.expect two_hold_lag_mean < {lag} his motion: weapon follows the main hand (mean cm behind)
21.5:hsvr.expect two_wrist_mean < 90 his motion: (report) mean deg the blade points off where the main wrist points it
21.5:hsvr.expect two_lag_mean < 100 his motion: (report) mean cm the main grip is off its hand
21.5:hsvr.expect two_jitter < 100 his motion: (report) % of ticks the spin reversed (his playtest: see the track header)
21.5:hsvr.expect two_jitter_own < 1.5 his motion: spin reversals not his hands' own (B&S 1.23% / 0.99%)
21.6:hsvr.ctl_replay off
22:quit
"""


# Auto slide (pose_two_hand_auto_slide_cm, playtest 2026-11-23 4:59 PM polearm): two hands on a haft, then the left hand moves
# 18 cm along the shaft toward the right without the trigger: its spot must follow it (B&S rest gap 6-9 cm), not stay and stretch.
AUTO = [("polehammer", 30, 12), ("warstaff", 30, 12)]
AUTO_STEPS = """10:hsvr.expect two_hand = 1 second hand took it
10.5:hsvr.arm_move 7 {to}
13:hsvr.expect two_hand = 1 hand moved along the shaft: still held
13:hsvr.expect two_spot_off < 6 hand moved along the shaft: its spot followed it (B&S rest gap 6-9 cm)
13.5:hsvr.arm_move 7 {back}
16:hsvr.expect two_hand = 1 hand moved back: still held
16:hsvr.expect two_spot_off < 6 hand moved back: its spot followed it
16.5:quit
"""


# Still hands far from the grips (playtest 2026-11-23 17:09-17:10: a two-handed war hammer spun up to 3500-9000 deg/s with
# both controllers still and the second hand 19-31 cm from its spot): left controller 25 cm further than its spot, held still.
SPREAD = [("axe2h", -25), ("polehammer", 30), ("warstaff", 30), ("longsword", -12)]
SPREAD_STEPS = """10:hsvr.expect two_hand = 1 second hand took it
10.3:hsvr.hand_spread 0 25
11.8:hsvr.two_reset
16:hsvr.expect two_hand = 1 hands still, 25 cm wider than the grips: still held
16:hsvr.expect two_spin_max < 200 hands still, 25 cm wider than the grips: the weapon does not spin up (deg/s)
16:hsvr.expect two_jitter < 2 hands still, 25 cm wider than the grips: no shaking (% of ticks the spin reversed)
16.1:hsvr.hand_spread 0 0
16.5:quit
"""


# Spin brake (one hand; playtest 2026-11-23 17:01: a one-handed mace spun end over end at 1300-1700 deg/s for 10 s in a still
# hand): the held weapon is set spinning 1500 deg/s end over end, the hand still; it must come to rest with the hand.
SPIN = ["mace", "axe2h", "polehammer", "longsword"]
SPIN_STEPS = """6:hsvr.spin_kick 1 1500
6.5:hsvr.expect held_spin_max_r > 300 (check) the kick took: it spun
8.5:hsvr.expect held_r = 1 still in the hand
8.5:hsvr.expect held_spin_r < 120 2.5 s after: back at rest in the still hand (deg/s)
9:hsvr.spin_kick 1 -1500
11.5:hsvr.expect held_r = 1 the other way: still in the hand
11.5:hsvr.expect held_spin_r < 120 the other way: back at rest (deg/s)
12:quit
"""


# Reach (2026-11-24): his mace spin started with his controller ~40 cm past where the drawn hand could be (log 250.4:
# "42 cm from hand_r"), a swing that stopped, one hand. The kick cases never reproduced it (the kick died in a tick in every
# mode). arm_move 6 holds the hand 54-70 cm out (past his 53 cm arm); then a fast cut and a stop out there.
REACH = ["mace", "axe2h", "polehammer", "longsword"]
REACH_STEPS = """6:hsvr.arm_move 6
6.5:hsvr.spin_reset 1
9.5:hsvr.expect held_spin_max_r < 400 reaching past his arm: the weapon does not spin up on its own (deg/s)
9.6:hsvr.arm_move 3
12.6:hsvr.arm_move 6
12.6:hsvr.expect held_spin_max_r < 900 fast cut out past his arm: no runaway spin (the mace ran at 1300-1800 deg/s)
12.7:hsvr.spin_reset 1
15.5:hsvr.expect held_spin_max_r < 900 stopped out past his arm: no runaway spin
15.5:hsvr.expect held_spin_r < 120 stopped out past his arm: at rest 3 s later (deg/s)
15.5:hsvr.expect held_r = 1
16:quit
"""

# Switch (2026-11-24, his markers 2026-11-23: three runaway spins 0.82-1.06 s after a switch between one and two hands, controllers
# still, right hand main throughout; one same switch with no spin). A fast two-hand cut, the left lets go out there, then takes again.
SWITCH = [("axe2h", -25), ("polehammer", 30), ("warstaff", 30), ("longsword", -12)]
SWITCH_STEPS = """10:hsvr.expect two_hand = 1 second hand took it
10.1:hsvr.arm_move 3
12:hsvr.arm_move 7 {along}
12.1:hsvr.squeeze 0 0
12.4:hsvr.spin_reset 1
15:hsvr.expect held_r = 1
15:hsvr.expect held_spin_max_r < 300 from 0.3 s after the left let go (2 to 1 hands), hands still: no spin-up (deg/s; his ran 1300-2700)
15.1:hsvr.arm_move 7 {along}
16.5:hsvr.squeeze 0 1
16.85:hsvr.spin_reset 1
19.5:hsvr.expect two_hand = 1 took it again
19.5:hsvr.expect held_spin_max_r < 300 from 0.3 s after the left took it again (1 to 2 hands), hands still: no spin-up (deg/s)
19.6:quit
"""


def main():
    rows = []
    for line in Path(sys.argv[1]).read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        name, path, along = [p.strip() for p in line.split("|")]
        if along == "-":
            continue  # one hand only
        steps = HEAD.format(path=path) + (TAKE_FRONT if along == "front" else TAKE_ALONG.format(along=along)) + BODY
        if along != "front" and abs(float(along)) <= 15.0:
            steps = steps.replace("28:quit\n", "") + WRIST.format(along=along)
        # line mode (the crossbow always, the ctl_ cases): the line limits; B&S mode: B&S's own
        (HERE / ("steps-line-%s.txt" % name)).write_text(steps, newline="\n")
        (HERE / ("steps-%s.txt" % name)).write_text(steps if along == "front" else bs_limits(steps), newline="\n")
        rows.append(name)
    paths = {}
    for line in Path(sys.argv[1]).read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            name, path, along = [p.strip() for p in line.split("|")]
            paths[name] = path
    tracks = sys.argv[2] if len(sys.argv) > 2 else r"C:\dev\hsvr-2h0303\tools\pctest\lists\two-hand\tracks"
    rp = []
    for name, weapon, track, along, turn, lag in REPLAYS:
        steps = HEAD.format(path=paths[weapon]) + TAKE_ALONG.format(along=along) + REPLAY.format(track=tracks + "\\" + track, turn=turn, lag=lag)
        (HERE / ("steps-rp_%s.txt" % name)).write_text(steps, newline="\n")
        rp.append(name)
    auto = []
    for weapon, along, to in AUTO:
        steps = HEAD.format(path=paths[weapon]) + TAKE_ALONG.format(along=along) + AUTO_STEPS.format(to=to, back=along)
        (HERE / ("steps-auto_%s.txt" % weapon)).write_text(steps, newline="\n")
        auto.append(weapon)
    spread = []
    for weapon, along in SPREAD:
        steps = HEAD.format(path=paths[weapon]) + TAKE_ALONG.format(along=along) + SPREAD_STEPS
        (HERE / ("steps-spread_%s.txt" % weapon)).write_text(steps, newline="\n")
        spread.append(weapon)
    spin = []
    for weapon in SPIN:
        steps = HEAD.format(path=paths[weapon]) + SPIN_STEPS
        (HERE / ("steps-spin_%s.txt" % weapon)).write_text(steps, newline="\n")
        spin.append(weapon)
    reach = []
    for weapon in REACH:
        (HERE / ("steps-reach_%s.txt" % weapon)).write_text(HEAD.format(path=paths[weapon]) + REACH_STEPS, newline="\n")
        reach.append(weapon)
    switch = []
    for weapon, along in SWITCH:
        steps = HEAD.format(path=paths[weapon]) + TAKE_ALONG.format(along=along) + SWITCH_STEPS.format(along=along)
        (HERE / ("steps-switch_%s.txt" % weapon)).write_text(steps, newline="\n")
        switch.append(weapon)
    lst = ["# Two hands with real-hand conditions (playtest 2026-11-23); two hands are held as B&S holds them (pose_two_hand_bs=1).",
           "# rp_: his own recorded controller motion. ctl_: the same cases in the previous line mode (pose_two_hand_bs=0), for comparison."]
    lst += ["two_%s|steps-%s.txt" % (n, n) for n in rows]
    lst += ["rp_%s|steps-rp_%s.txt" % (n, n) for n in rp]
    lst += ["ctl_%s|steps-line-%s.txt|pose_two_hand_bs=0" % (n, n) for n in rows]
    lst += ["ctl_rp_%s|steps-rp_%s.txt|pose_two_hand_bs=0" % (n, n) for n in rp]
    lst += ["spread_%s|steps-spread_%s.txt" % (n, n) for n in spread]
    lst += ["ctl_spread_%s|steps-spread_%s.txt|pose_two_hand_bs=0" % (n, n) for n in spread]
    lst += ["spin_%s|steps-spin_%s.txt" % (n, n) for n in spin]
    lst += ["ctl_spin_%s|steps-spin_%s.txt|pose_hold_spin_brake_from=0" % (n, n) for n in spin]
    # (pull once, 2026-11-24: the one-hand spin the mod sets no longer carries the joint's pull a second time. old_: as installed
    # a3bde48, both off, expected to keep spinning (the mace bug); pull2_: the pull twice with the brake on)
    lst += ["old_spin_%s|steps-spin_%s.txt|pose_hold_spin_brake_from=0;pose_hold_joint_pull_once=0" % (n, n) for n in spin]
    lst += ["pull2_spin_%s|steps-spin_%s.txt|pose_hold_joint_pull_once=0" % (n, n) for n in spin]
    lst += ["reach_%s|steps-reach_%s.txt" % (n, n) for n in reach]
    lst += ["old_reach_%s|steps-reach_%s.txt|pose_hold_spin_brake_from=0;pose_hold_joint_pull_once=0" % (n, n) for n in reach]
    for pre, extra in (("switch", ""), ("switch2", ""), ("grip_switch", "|pose_hold_joint_turn_couple=0"), ("old_switch", "|pose_hold_spin_brake_from=0;pose_hold_joint_pull_once=0"),
                       ("old_switch2", "|pose_hold_spin_brake_from=0;pose_hold_joint_pull_once=0")):
        lst += ["%s_%s|steps-switch_%s.txt%s" % (pre, n, n, extra) for n in switch]
    # (a second launch of each: the one runaway seen so far came in only one of two launches)
    lst += ["reach2_%s|steps-reach_%s.txt" % (n, n) for n in reach]
    lst += ["old_reach2_%s|steps-reach_%s.txt|pose_hold_spin_brake_from=0;pose_hold_joint_pull_once=0" % (n, n) for n in reach]
    # (auto slide is off by default since Matthew 2026-11-24 00:08Z chose B&S's trigger-only slide; these turn it on)
    lst += ["auto_%s|steps-auto_%s.txt|pose_two_hand_auto_slide_cm=10" % (n, n) for n in auto]
    (HERE / "list.txt").write_text("\n".join(lst) + "\n", newline="\n")
    print("wrote %d cases (+%d control)" % (len(rows), len(rows)))


if __name__ == "__main__":
    main()
