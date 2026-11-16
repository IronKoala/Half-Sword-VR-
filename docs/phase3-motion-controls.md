# Phase 3: motion controls (the controllers move the arms)

Status 2026-11-16: in use in the headset. Matthew: figure 8s work and it is "better", but the sword is still not exactly where his hand is; a larger rework is expected later (see Open). Parked to move on to the next step.
Background: `docs/phase2-arm-research.md`.

## How it works

Half Sword animates an invisible driver skeleton (`DriverSkeleton`, anim `ABP_Willie`). The visible,
physically simulated body (`CharacterMesh0`, anim `ABP_CopyPose1`) copies the driver's bone transforms
each frame in its `CopyPoseFromMesh` node, and the body's muscles pull the real arm bodies towards that
pose. The mod bends the driver's arm bones between those two steps, so the game's strength, stamina,
injuries and weapon weight still decide how well the real arm follows.

1. **Controllers** (`vr_present.cpp`): an OpenXR action set with a grip pose, trigger and squeeze per
   hand. Bindings are suggested for the Index, Touch, Vive and simple controller profiles; SteamVR maps
   the Steam Frame controllers onto one of them. Poses are located at the same predicted time as the
   eyes. The log names the profile in use ("controller uses the bindings for ...").
2. **Into the world** (`stereo_device.cpp`, `stereo_tracking_to_world`): exactly the mapping used for the
   eyes (recenter, head anchor, heading, world scale), so the hands sit where the player sees them.
3. **Hand target** (`arm_control.cpp`, `compute_targets`): the controller's handle direction is matched
   to the direction of the handle in the character's hand, and the controller's "wrist to fist" direction
   to the hand bone's "wrist to grip" direction. Both are measured on the physical body a few times a
   second: from the weapon in the right hand (`Weapon R` actor location to its `Head` component), or,
   with no weapon, from the finger bones (`middle_01`, `thumb_01`). So the blade points where the
   controller's handle points.
4. **Hook point**: every other skeletal mesh of the character gets its `PrimaryComponentTick`'s
   vtable pointer replaced by a copy whose `ExecuteTick` first runs the arm solver (per object; other
   objects of the type are untouched). `FTickFunction` has four virtuals in 5.4.4: destructor,
   `ExecuteTick`, `DiagnosticMessage`, `DiagnosticContext`. The first mesh to update after the driver
   has animated applies the arms; later ones in the same frame see it done.
5. **Driver bone transforms**: `USkinnedMeshComponent::ComponentSpaceTransformsArray[2]` followed by two
   arrays, a float, `CurrentEditableComponentTransforms` and `CurrentReadComponentTransforms` (+68 and +72
   from the arrays). Not reflected, so found by shape and confirmed against `GetSocketTransform` in
   component space for both hands and upper arms. The read buffer is the one `CopyPoseFromMesh` reads.
6. **Solver** (`solve_side`): the hand target is taken relative to the physical upper body (the
   clavicle's parent bone) and placed on the driver's upper body. Two-bone IK from the animated shoulder:
   elbow towards a pole (down, out, back of the character's facing; `elbow_*`), the elbow bends about its
   own hinge axis, which is learned from the game's own animation (logged once per arm). The forearm takes
   `forearm_twist_share` of the hand's roll. Bones below the hand (fingers, `weapon_r`) are carried along.
   Out-of-reach targets are clamped to 99.9 % of the arm length.
7. **Aim** (`sync_aim`): the pawn's `Aim Vector` (the 2-D aim the mouse normally moves, Â±90) is set from
   the blade direction relative to the character's facing, so the game's swing speed, stamina and body
   turning follow along (`aim_sync`).

If any check fails (layout not found, unexpected parameter sizes, arm chain not as expected) the log says
so and motion arms stay off; the game runs as before.

## Use

- Controllers must be on and tracked. Hold the sword controller like a sword handle.
- **Page Up** switches motion arms on and off while playing (the game's own arm animation comes back).
- Home recenters as before; recenter while standing as you will play.
- Settings in `HalfSwordVR.ini` (defaults apply when a key is missing):

| Key | Default | Meaning |
| --- | --- | --- |
| `motion_arms` | 1 | Controllers move the hands |
| `arm_right`, `arm_left` | 1 | Per arm |
| `toggle_arms_key` | 0x21 | Page Up |
| `aim_sync` | 1 | Game's aim follows the sword |
| `aim_scale`, `aim_invert_y` | 1, 0 | Aim mapping |
| `elbow_down`, `elbow_out`, `elbow_back` | 1, 0.6, 0.2 | Where the elbow points |
| `forearm_twist_share` | 0.5 | Hand roll taken by the forearm |
| `grip_shift_cm` | 0 | Slide the sword along the handle |
| `hand_pitch`, `hand_yaw`, `hand_roll` | 0 | Extra turn of the hands |
| `log_arms` | 1 | "Arms:" status line once a second |

Developer switches: `dev_fake_hands` (pretend controllers circling in front of the chest),
`dev_arms_any_character` (drive the first `Willie_BP_C` in the level when nobody plays one).

## Log lines to read after a test

- `Arms: driver skeleton bone transforms found at offset ...`, `Arms: ready on ...`: setup worked.
- `Arms: ... elbow hinge learned ...`, `Arms: ... hand grip measured from the weapon in it / its fingers`.
- `Arms: on | controllers L tracked R tracked | right hand sent err X cm Y deg | ... driver frames fresh
  N late M applied K`: err is the distance between where the physical hand is and where it was sent
  (the muscles' lag and weakness). "late" counts frames where the body updated before the driver had
  animated (the previous pose was reused), which adds one frame of delay.

## Unattended test notes

- `open /Game/Maps/Arenas/Map_Arena_Cellar` (and Slums) from the splash screen loaded the arena but
  spawned no character on 2026-11-15, so the arm code could not be exercised without someone playing.

## Tuning history (headset, 2026-11-15/16)

- First fight: hands within 5 to 10 cm, no crash; fast swings lag 20 to 50 cm.
- Figure 8 failed: the arm was sent almost straight (targets past the character's reach), so all blade turning fell to the wrist, which stops near 78 degrees of bend. Fix: reach easing (reach_knee, reach_max) and an elbow that swings about the shoulder-hand line to keep wrist bend and roll in range (wrist_bend_limit, twist_limit, elbow_swing_max).
- Wonky elbow: the hand's roll about the forearm crossed 180 degrees and the forearm flipped in one frame. Fix: roll unwrapped frame to frame and clamped (twist_clamp). Matthew: "WAY better".
- In-game hand 4 to 6 inches short: shoulder reach (clavicle swings forward, shoulder_reach_cm), then an arm-length calibration (both grips held 2 s, arms straight out; saved as player_arm_cm etc.) that scales reach from the player's shoulder to the character's.
- Steady 13 cm / 15 to 25 degree gap even at rest (muscles under sword weight): hold assist (assist_*), made gentler after it felt springy.
- Matthew's goal: keep the game's physics feel; heavy weapons may lag, one-handers should swing about as fast as the controller.

## Open

- The sword is still not where the real hand is. Candidates for the rework: a lead on fast moves scaled by weapon weight; a direct physics assist on the hand or weapon (option C in the research notes); changing the physical arm itself (bone lengths, constraint drive strength), which affects game balance.
- efore_body_update runs late on about half the frames (the body updates before the driver has animated), which adds a frame of delay; worth a fix in the rework.

- Not yet tried in the headset: accuracy, feel, whether the muscles are strong enough, elbow look.
- Buttons are read (trigger, grip) but do nothing yet; grabbing, guard, thrust, two-handed grip and
  walking still use mouse and keyboard.
- Two-handed grip: the left hand follows its own controller even when the game puts it on the hilt.
- Physics assist for tip accuracy (option C in the research notes) only if the muscles prove too weak.

## Sticks and buttons (virtual gamepad), 2026-11-16

Half Sword already plays with a gamepad, so the mod hands the game one made from the controllers
(`controller_input.cpp`). The game exe delay-loads `XINPUT1_4.dll` and imports `XInputGetState` by
ordinal 2; the mod points that import slot at its own function, which answers pad 0 with the
controllers mapped by `pad_map` (a real pad on pad 0 is merged in). Nothing is sent to Windows.
If the engine stops asking about pad 0 while the controllers are on, the mod pulses
`XInput.ForceControllerStateUpdate` for one frame.

The game's own gamepad bindings (read from `Default__InputSettings`, logged as "Bindings:" lines):
left stick move; right stick X turn, Y look up/down; LT/RT guard left/right (also "Left/Right Arm
Axis"); LB grab left (also Slomo), RB grab right; A jump/talk; X swap hands; B camera lock;
Y undress; left stick press crouch; right stick press thrust; Start pause; D-pad up inventory,
down change camera. Run has no gamepad binding (Left Shift only).

Default `pad_map` (controller -> gamepad):

| Controller | Gamepad | Game action |
| --- | --- | --- |
| Left stick | Left stick | Walk |
| Right stick left/right | Right stick X | Turn (smooth) |
| Left / right trigger | LT / RT | Guard left / right |
| Left / right grip | LB / RB | Grab left / right (LB also slow motion) |
| A | A | Jump, talk |
| B, right stick press | Right stick press | Thrust |
| X | X | Swap hands |
| Y | D-pad up | Inventory |
| Left stick press | Left stick press | Crouch |
| Left menu button | Start | Pause |

Sources: `l_stick`, `r_stick`, `l_stick_x/y`, `r_stick_x/y`, `l_trigger`, `r_trigger`, `l_grip`,
`r_grip`, `l_lower` (X), `l_upper` (Y), `r_lower` (A), `r_upper` (B), `l_click`, `r_click`, `l_menu`.
Targets: `LSTICK`, `RSTICK`, `LSTICK_X/Y`, `RSTICK_X/Y`, `LT`, `RT`, `A`, `B`, `X`, `Y`, `LB`, `RB`,
`LS`, `RS`, `START`, `BACK`, `DUP`, `DDOWN`, `DLEFT`, `DRIGHT`. Other keys: `controller_pad` (on/off),
`pad_deadzone`, `pad_game_deadzone`, `log_bindings`.

Not tried in the headset yet. Known overlap: the arm-length calibration (both grips held 2 s) also
grabs with both hands.
