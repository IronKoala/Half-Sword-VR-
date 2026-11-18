# Phase 2 tests: controller tracking and the player physics test

Phase 2 adds two things, neither of which changes how the game plays by default:

1. **Controller tracking (read only).** The mod now asks SteamVR for both controllers through an OpenXR action set: grip and pointer poses, trigger, grip button, stick, stick click, A/X, B/Y, menu and vibration. Nothing is sent to the game yet; everything goes to `HalfSwordVR.log`.
2. **Player physics test (off by default).** `dev_player_physics_off=1` switches physics off on your own character's body only (the enemies run the same `Willie_BP` and are not touched), to see what the game does without the ragdoll before Phase 3 is built on it.

Background for both: the roadmap and the decompile write-up (`HalfSwordDecomp\internals-map.md` on the test PC, not in this repo).

## New settings (`HalfSwordVR.ini`)

| Setting | Default | What it does |
| --- | --- | --- |
| `log_controllers` | 1 | Once a second, one log line per controller: offset from your head (forward, right, up in cm), position and rotation in the game world, distance to the character's hand bone, and what the buttons and stick read. |
| `hand_bone_left`, `hand_bone_right` | `hand_l`, `hand_r` | Bones the distance is measured to. `hand_r` is the weapon hand per the decompile; `hand_l` is assumed from the same naming and is checked by this test (the bone list is in the log at the start of each fight). |
| `haptics_test` | 0 | 1 = a short buzz on a controller each time its trigger is pulled all the way. |
| `log_body` | 0 | 1 = once a second, where your character's body, pelvis, head and hands are, and whether the pelvis is simulated. On automatically while the physics test runs. |
| `dev_player_arm_physics` | 0 | TEST ONLY, with `dev_player_physics_off=1`: keeps physics on for both arms (every body from `arm_bone_left` / `arm_bone_right`, default `clavicle_l` / `clavicle_r`, outwards), so the rest of the body is still and the arms move by the game's own muscles. Can be flipped while the test runs. |
| `dev_player_physics_off` | 0 | TEST ONLY. 1 = physics off on your character's body. Read while the game runs. Back to 0 turns physics on again, which may not match the game's own setup, so restart the fight afterwards. |

The ini is only written with these lines when it is first created. On an existing install, add the lines by hand.

## Test 1: controllers

1. Start SteamVR with both controllers on, then Half Sword. Get into a fight.
2. In the log, look for `Controllers: bindings for ... accepted` (four lines at start-up) and `Controllers: left hand uses layout ...` / `right hand uses layout ...`. The layout line says which controller layout SteamVR picked for your controllers; it is how we find out what the Steam Frame controllers report as.
3. Hold each controller still next to the character's hands as you see them, then move them around. Check in the `Controller left/right:` lines that the head offsets change the way your hands move (forward, right, up) and that the "cm from hand_l/hand_r" distance is small when your hand is where his is.
4. Press every button and push the sticks; each shows up in the same lines.
5. Optional: set `haptics_test=1`, pull each trigger fully, feel for a buzz and look for `Controllers: test buzz`.

**Passes when:** both hands track at 72 fps with no dropped frames, the layout lines are there, and every button and stick reads.

## Test 2: physics off on your character

1. In a fight (an easy opponent), set `dev_player_physics_off=1` and save the ini. The log says `Physics test: physics OFF for your character`.
2. Watch and note: does he stand, stay animated, fall through the floor, freeze, drop his weapon? Can you still walk, swing, block, get hit, kill the opponent, die?
3. The log checks every second whether the game turned physics back on (`Physics test: simulating now ...`) and switches it off again if so.
4. Set it back to 0, then restart the fight.

Unattended version: `dev_script` can switch the test with the mod's own steps `hsvr.physics_off 1` and `hsvr.physics_off 0`, next to engine console commands such as `open <map>`, so a timed run can load a fight, record a while with physics on (`log_body=1`), switch it off, and record again.

`open <arena>` on its own gives no character: the arena's level manager only keeps spawn points that match the mode the menus set on the game instance (GI_Settings). The step `hsvr.set <Name>=<value>` writes one of those variables first, as the Free Mode button and the Jester's Fight button do. The arena's game manager loads the save again when the map starts (GI_Settings "Load Game"), which would undo them, so the Fight button saves first; `hsvr.call Save Game` does the same (`hsvr.call` calls a game instance function without parameters). An easy one-on-one fight:

```
dev_script=40:hsvr.set Current Play Mode=1; 40:hsvr.set Free Mode Activated=true; 40:hsvr.set Current Game Mode Enum=1; 40:hsvr.set Free Mode Foes Amount=1; 40:hsvr.set FreeMode Foes Amount Random=false; 40:hsvr.set Free Mode Tier=0; 40:hsvr.set FreeMode Tier Random=false; 40:hsvr.set Free Mode Carnage=false; 40:hsvr.set Free Mode Brawling=false; 40:hsvr.set Free Mode Blossfechten=false; 40:hsvr.set FreeMode Multiplayer=false; 40:hsvr.set Current Combat Mode=1; 40:hsvr.set Current Lose Condition=0; 42:hsvr.call Save Game; 45:open /Game/Maps/Arenas/Map_Arena_Yard
```

This saves the game, so the save is left in Free Mode, as after pressing Free Mode and Fight by hand. Back the save up first if that matters.

**What this answers:** whether physics can be switched off for the player alone without the game breaking, and what Phase 3 has to replace.

### Result (unattended, 2026-11-17, game build 26737211)

- `SetSimulatePhysics(false)` on the character's mesh does nothing: the pelvis still simulates in the same frame. `SetAllBodiesSimulatePhysics(false)` switches every body off, so the test calls both and logs the pelvis after each.
- With every body off, the game never turned physics back on while he was alive (90 checks, one a second).
- The body freezes standing in the pose it had, while the collision capsule keeps moving under the opponent's pushes. Nothing fell, sank or slid. The opponent stood face to face with him and no damage showed (no red screen, no death).
- Physics back on recovered him at once: he moved and fought normally.
- An idle character at the easiest tier is killed in about 33 s, so the unattended script switches physics off about 3 s after the spawn.

### Result in the headset (2026-11-18)

- Test 1: every button, stick and trigger read and both controllers buzzed (Matthew; this run's log was lost to a restart, the inputs were already confirmed in the 2026-11-17 headset log). In the physics-test run both hands tracked the whole time (410 readings, none lost) on the Oculus Touch layout, about 70 fps.
- Test 2: as expected, no issues in the headset. The game turns physics back on once, about 2 s after each spawn, then leaves it off once the mod switches it off again; Phase 3 has to switch it off after that point or keep checking. Switching back on after about 90 s frozen made him collapse this time (yesterday he recovered), so restart the fight after the test.
