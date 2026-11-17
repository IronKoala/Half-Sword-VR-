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

`open <arena>` on its own gives no character: the arena's level manager only keeps spawn points that match the mode the menus set on the game instance (GI_Settings). The step `hsvr.set <Name>=<value>` writes one of those variables first, as the Free Mode button and the Jester's Fight button do. An easy one-on-one fight:

```
dev_script=40:hsvr.set Current Play Mode=1; 40:hsvr.set Free Mode Activated=true; 40:hsvr.set Current Game Mode Enum=1; 40:hsvr.set Free Mode Foes Amount=1; 40:hsvr.set FreeMode Foes Amount Random=false; 40:hsvr.set Free Mode Tier=0; 40:hsvr.set FreeMode Tier Random=false; 40:hsvr.set Free Mode Carnage=false; 40:hsvr.set Free Mode Brawling=false; 40:hsvr.set Free Mode Blossfechten=false; 40:hsvr.set FreeMode Multiplayer=false; 45:open /Game/Maps/Arenas/Map_Arena_Yard
```

The game saves by itself when the fight starts, so the save is left in Free Mode, as after pressing Free Mode and Fight by hand. Back the save up first if that matters.

**What this answers:** whether physics can be switched off for the player alone without the game breaking, and what Phase 3 has to replace.
