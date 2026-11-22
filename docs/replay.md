# Bug-marker replay

While you play in VR the mod keeps the last few seconds of your headset, both controllers
(pose, trigger, grip, stick, buttons, finger curls), where it sends each hand, his posed hand
bones, both held items (name, pose, hold lag/force/torque, sliding, drawing), his capsule and
every physics-body bone of his visible body in memory. Nothing goes to disk until you ask.

**Hold the left Y button (left B on Valve Index) for about 0.8 s** the moment something looks
wrong. Both controllers give **three strong pulses** (nothing else in the mod buzzes like that):
the marker is in and it is recording. It keeps recording 5 more seconds, then writes the 10 s
before the press and the 5 s after it to

    Mods\HalfSwordVR\replays\replay-<date>_<time>.csv

with the mod's log lines of that window at the end. Pressing again within those 5 s adds a second
mark to the same file and records 5 s past it. A quick tap of Y still flips the left grip; holding
Y with right B (the arm measure) makes no marker.

Settings (`[Logging]` section): `replay_capture`, `replay_hold_s`, `replay_before_s`,
`replay_after_s`. Test scripts can press it with `hsvr.replay_mark`.

## Reading a capture

`tools/replay/replay.py` (Python 3, standard library only):

    python3 tools/replay/replay.py replay.csv            # summary: items per hand, buttons, hitches,
                                                          # jumps, state changes, log lines near the mark
    python3 tools/replay/replay.py replay.csv --cols "^l_(item_[xyz]|lag_cm)" --from -1 --to 0.5
    python3 tools/replay/replay.py replay.csv --at -0.25  # every value of one frame
    python3 tools/replay/replay.py replay.csv --log       # the log lines of the window

Times are seconds from the mark. Positions are cm in Unreal world axes, rotations quaternions.
The file starts with `# key: value` lines (map, VR or pretend headset, marks, item names by
index), then a CSV header and one row per tick, then `#log` lines.

Cost: one fixed ~1.5 KB frame per tick into a preallocated ring (4096 ticks, ~6 MB), one engine
call per recorded bone with reused call buffers; item names are read only when a hand's item
changes. The file is written on a helper thread.
