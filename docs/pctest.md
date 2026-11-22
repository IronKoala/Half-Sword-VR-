# Unattended test runs: many cases in one launch

`tools/pctest/run-suite.ps1` runs a list of test cases on the PC with the pretend headset. It replaces the
per-thread copies of `run-cross-*.ps1` (one launch per case).

Measured 2026-11-22 over 390 runs of the old way: a case took ~62 s, of which ~24 s was the launch and the
test room loading, ~35 s the steps and ~3 s closing and restoring. 11 % of runs were thrown away for the
bad spawn point (468, 1) in the Yard.

## How it works

- One launch per build (and per set of extra ini keys). The first case runs in the room the launch opened.
- Between cases the runner writes `0:hsvr.case <name>`. The mod then:
  - drops any steps left over from the case before,
  - puts every made-up input back to how a launch starts it (`case_reset_inputs` in dllmain.cpp: fake
    head, arms, triggers, grips, thumbs, sticks, buttons, test counters, `hsvr.player_immortal`),
  - writes the test room's game-instance values again and opens the test room map again (a new level:
    new Willie, foes and weapons; ~5 s).
- The runner waits for `TestRoom: <n> passive foe(s)` and 4 s more, exactly as after a launch, then writes the
  case's steps with `hsvr.case_here <name>` first and `hsvr.case_end` after the last step (`quit` lines are
  dropped). The mod logs `Case: end <name> (#n): <p> check(s) passed, <f> failed`.
- A bad spawn point now costs one more room opening (~5 s) instead of a whole launch.
- The first `hsvr.expect` that fails in a case marks a replay (replay_capture): the 10 s before and 5 s after
  are saved and copied into the case's folder. `python tools/replay/replay.py <csv>` reads it.
- Every case that fails or times out runs once more in a launch of its own (`fresh`). A failure that only
  happens when sharing a launch shows up as FAIL in `shared` and pass in `fresh`: that is a leak between
  cases (in the mod or in the test), not the feature.
- A case that never reaches its end within `-CaseSeconds` stops that launch; the cases after it go on in a
  new launch.
- Save and settings are put back after every launch; dll, ini, save and settings are restored and checked
  against the master backup at the end. It stops at once on a game it did not start or a `STOP` file in the
  results folder. SteamVR on its own doesn't stop it.

## Writing a list

One case a line, `name|steps file|extra ini keys`, `#` for comments. Steps files are the same as before
(`<seconds>:<command>`; no `M+` steps). Cases next to each other with the same extra keys share launches.

```
# pavise round
tw_b1_heavy_left|steps-cross-tw_b1_heavy_left.txt
tw_b3_light_right_arm|steps-cross-tw_b3_light_right_arm.txt|pose_hold_joint=1
```

A steps file with a line `# fresh` always gets a launch of its own. Use it for a case that needs a clean
launch, or one that uses a test aid that `case_reset_inputs` doesn't put back (a test copy's own commands:
add their state to `case_reset_inputs` instead when you can).

## Running

```
powershell -NoProfile -ExecutionPolicy Bypass -File tools\pctest\run-suite.ps1 -List <list.txt> -Dll <main.dll> -Master <backup tag>
```

`-Fresh` runs every case in its own launch (the old way, for comparisons). `-NoConfirm` skips the fresh
re-runs. `-PerLaunch` caps cases per launch (40). Results: `<Out>\results.csv` (case, run, launch, passed,
failed, result, seconds, spawn, replays), `<Out>\run.txt`, one folder per case with `steps.txt`, the case's own
slice of `HalfSwordVR.log` (the first case's slice starts at the launch), save probes and replays, and one
`launch-<n>` folder per launch with the whole log, UE4SS.log and the save after it.

Judge scripts that read a run folder's `HalfSwordVR.log` work on a case folder as before.
