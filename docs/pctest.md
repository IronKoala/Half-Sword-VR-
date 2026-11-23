# Unattended test runs: many cases in one launch

**Every thread uses this runner for unattended tests from 2026-11-22 (Matthew).** Don't write a new
per-thread runner; fix or extend this one (PR to `tools/pctest`).

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

## Waiting for things to happen (hsvr.wait)

Fixed step times have to cover the slowest run, so most of a case was spent waiting (a hand that reached the
strap in 0.5 s, then 3 s more before the grip). Use a wait instead:

```
hsvr.wait <what> <op> <value> <timeout s> [hold s] [label]
```

The steps after it are held until `<what> <op> <value>` has been true for `<hold>` seconds (0 = at once), or
`<timeout>` passes. They then run as many seconds after that moment as they were after the wait in the file,
so a file stays readable with its old times. `<what>` is anything `hsvr.expect` reads, plus
`hand_speed_l` / `hand_speed_r` (cm/s of the hand bone: below ~5 means it has arrived). A timeout counts as a
failed check (and marks a replay); a wait that is met counts as nothing. The log says
`WAIT OK: ... after 0.48 s` or `WAIT TIMEOUT: ...`, so every case now also measures how long things took.

Before and after:

```
7.2:hsvr.hand_at 0 strap 0          7.2:hsvr.hand_at 0 strap 0
10.7:hsvr.squeeze 0 1               7.3:hsvr.wait hand_speed_l < 5 3.4 0.2 hand on the strap
12.7:hsvr.expect strap_l = 1 ...    7.4:hsvr.squeeze 0 1
                                    7.5:hsvr.wait strap_l = 1 2 0 in the hand by the strap
                                    7.6:hsvr.expect mount_l = 0 ...
```

Use `hand_speed` only for "arrived after a reach": an idle fake hand drifts at 4-6 cm/s, so a wait for a
still hand after `hsvr.arm_move` can time out; keep a fixed settle there (0.5 s).

Measured 2026-11-22 (same dll, shared launches): xbow_r 42 s -> 23 s, bastard_2h 45 s -> 38 s, results the
same. Reaches took 0.2-1.4 s where the files waited 3-4 s.

Keep a fixed gap where the test is about time passing (a settle, a hold that must last, a sample every
second): a wait only replaces "long enough for X to have happened". Use the old file's gap as the timeout,
so a slow run fails no sooner than before.

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

## Side effects to know

- The mod keeps its newest 40 `logs\HalfSwordVR-*.log`; the runner copies any not yet kept to
  `private\play-logs` before it launches, so test launches never lose his play logs.
- Every launch with the test room adds one save copy (~5 MB) to the mod's `save_backups` (3.3 GB on
  2026-11-22). Nothing deletes them.
- Worked examples (steps with waits): `HalfSwordDecomp\private\pctest-check\wait\`.

## Test copies: background and parallel runs (2026-11-23)

Matthew 2026-11-23: "When I'm at my computer, I only want one test running in the background. That way I
can also play Half Sword at the same time. ... when I'm away from my computer, you can run multiple tests
simultaneously." Tests run on the worst picture settings, and nothing may change what a test measures.

**Needs the mod with `game_saved_dir` (dllmain.cpp).** Older dlls build the save path from `%LOCALAPPDATA%`
and would copy and put back HIS save; the runner stops at once if the mod does not say where the game saves
or names a folder outside the copy.

- `tools/pctest/new-testcopy.ps1 -Name A` makes `C:\HSTest\A`:
  - `game\`: his install, files of 50 MB and more hard-linked (paks, exe; no disk used, read only), the rest
    copied, the `ue4ss` folder (mod dll, ini, log, commands, replays) always a real copy.
  - `user\`: the game's `-UserDir=` (checked in the shipping exe: an absolute path is used as is, and
    `Saved\SaveGames` and `Saved\Config` move with it). Its own save and settings, copied from his.
  - Picture: every `sg.*` group 0 except view distance and foliage (left as he has them), a 1280x720
    window, and `testcopy-Engine.ini` (render-only lines; the file lists what was left out and why).
  - `-Refresh` after an install or a game update (copies his mod folder, save and settings again, links
    the updated big files again).
- `run-suite.ps1 -Copy C:\HSTest\A -List ... -Dll ...` runs a list in that copy. No `-Master`: the copy's
  own files are the master backup. Only a game running from THIS copy stops it: his own game and other
  copies don't. The test game starts minimized; while his game runs it gets lower processor priority.
  Picture keys in the copy's mod ini: `mirror_window_res=1280x720`, `screen_percentage=50`,
  `extra_commands` + `t.MaxFPS 72` (the headset's rate; `-MaxFps 0` = no cap), `perf_log_seconds=2`.
  A case whose game logged a frame rate below `-MinFps` (65) is **SLOW**: neither pass nor fail.
- `run-parallel.ps1 -List ... -Dll ... [-Slots 2]` hands chunks of the list (`-Chunk 8`) to copies A, B, ...
  Keyboard or mouse used in the last `-IdleMinutes` (10), or his own Half Sword open = ONE test game.
  Otherwise up to `-Slots`. A running copy is never stopped when he comes back; its chunk is short.
  SLOW cases are queued again (up to `-SlowTries` 3). `<Out>\STOP` stops everything.
- His install, mod ini and save are never written by any of this.

**Incident 2026-11-23 19:03-19:14Z:** the first `new-testcopy.ps1` copied his whole mod folder (31.9 GB:
25.2 GB logs, 4.7 GB save backups, 1 GB replays) before deleting them. C: was swamped, and SteamVR's watchdog
killed vrserver/vrcompositor three times while he started VR ("Failed Watchdog timeout ... CVRLog::LogDirect").
Now: logs, save_backups and replays are never copied (robocopy /XD), the copy runs at Idle priority, it refuses
to run while SteamVR or his Half Sword runs, and it stops if the copied mod folder is over 2 GB. A test copy
loading a level still READS GBs from C:; while he plays, the runner keeps one test game at lower priority.
