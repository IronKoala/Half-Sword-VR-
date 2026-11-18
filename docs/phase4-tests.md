# Phase 4 tests: controls and weapons in your hands

## Stick walking and turning (first part of Phase 4)

- Left stick walks. It goes through the game's own movement (`AddMovementInput` and "Movement Input Vector", the same path as its gamepad and WASD handlers, Willie_BP @768801, @768447, @760861), so his walk speed, stamina, weight and injuries still apply, and walls still stop you. By default it walks toward where you look.
- Right stick turns you (view and body together), smoothly by default. It needs `pose_body_turn=1`.
- The log line "Walk:" shows the stick, the direction and his speed once a second.

| Setting | Default | What it does |
|---|---|---|
| `pose_stick_walk` | 1 | 1 = the left stick walks. |
| `pose_move_relative` | head | `head` = the stick walks toward where you look; `body` = toward where his body faces. |
| `pose_stick_turn` | smooth | `smooth`, `snap` or `off`. |
| `pose_stick_turn_dps`, `pose_snap_turn_deg` | 150, 30 | Smooth turn speed at full stick; snap size. |
| `pose_stick_dead_zone` | 0.15 | How far a stick moves before it counts. |

While you walk with the stick the capsule hold no longer clears his speed (it did, to stop the game's own pull, which also stopped walking); it clears it again half a second after you let go.

## Weapon in hand on a strength-limited spring (step 1)

The game holds a weapon with a physics joint between the weapon and the hand bone, locked once the weapon is seated (Willie_BP @454086-455803). With the hand posed from your controller a locked joint makes any weapon weightless. Right after his tick the mod turns that seated joint into a spring toward your hand with a force limit, so a weapon heavier than your strength lags and sags. The game's own grip slip and stamina drain read this joint's force, so they keep working. The controller buzzes when the weapon is more than 2 cm from your hand. The log line "Grip:" shows each hand's weapon mass, joint force, estimated gap and the game's slip rate twice a second.

| Setting | Default | What it does |
|---|---|---|
| `pose_grip_hold` | 1 | 1 = the hold from Matthew's Chasms VR Framework: the game's hand joint goes free and the mod sets the weapon's velocity and spin each tick so the point held follows the controller (implicit spring and damper, capped force and torque, the weapon's own weight and inertia about the grip). Replaces the spring below. 0 = the spring or the locked joint. |
| `pose_hold_strength` | 1 | Multiplies every hold value (dampers by half of it past 1). Game stats will drive this later. |
| `pose_hold_spring`, `pose_hold_damper`, `pose_hold_max_force` | 5000, 100, 3000 | The hold's spring (N/m), damper (N s/m) and most force (N), the framework's one-hand values. |
| `pose_hold_turn_spring`, `pose_hold_turn_damper`, `pose_hold_max_torque` | 1100, 45, 250 | Its turning spring (N m/rad), damper and most torque (N m), the framework's values with its handle multipliers (x1.1, x0.9). |
| `pose_hold_hand_on_weapon`, `pose_hold_blocked_from_cm` | 1, 3 | While held, his visible hand sits on the weapon when something stops it, instead of going on with your controller (Matthew's framework). It eases in as the weapon falls 1 to 3 cm behind where the hold sent it. |
| `pose_hold_let_go`, `pose_hold_let_go_s` | 1, 0.15 | The stuck rule: a stopped weapon whose grip is out of his reach from the shoulder, or more than an arm's length from your hand, for 0.15 s, is let go with the game's own drop (Set Up Right/Left Hand Weapon with no weapon, dropped with no damage). |
| `pose_ghost_hand`, `pose_ghost_from_cm` | 0, 4 | The ghost hand: while his hand stays on a stopped weapon more than 4 cm from your real hand, a see-through glove shows where your hand is. |
| `pose_ghost_mesh`, `pose_ghost_material` | a game glove, the engine's unlit translucent | Object paths in the cooked game (see HalfSwordDecomp ghost_hand_assets.md for others). |
| `pose_ghost_x_cm/y_cm/z_cm`, `pose_ghost_pitch/yaw/roll`, `pose_ghost_scale` | 0, pitch 180, scale 0.8 | Fitting the glove onto the hand bone. |
| `pose_shoulder_reach_cm`, `pose_shoulder_reach_start_cm`, `pose_shoulder_reach_max_deg` | 12, 6, 35 | The shoulder reaches with you: over the last 6 cm of his reach his clavicle turns toward your hand and brings the shoulder up to 12 cm closer (at most 35 deg). 0 = off. |
| `pose_user_arm_cm` | 0 | Your arm, shoulder to wrist, in cm. Measure it in the headset: arms straight out to the sides, hold B and Y together for 2 s; both controllers buzz and the value is written here. 0 = your hands go exactly where the controllers are; measured, your reach is scaled to his. |
| `pose_grip_spring` | 1 | With the hold off: 1 = spring, 0 = the game's locked joint. |
| `pose_grip_strength_kg` | 20 | The most weight the grip holds up, in kg. Turning strength is this times `pose_grip_radius_cm`. |
| `pose_grip_hz`, `pose_grip_damping` | 20, 0.3 | How briskly the weapon follows (natural frequency) and the damping ratio. Chaos runs drives in acceleration mode and solves their damping against the spring's own correction, so damping 1 makes the weapon sag ~16 cm; 0.3 holds as well as the game's locked grip (tests 25-26). |
| `pose_grip_radius_cm` | 30 | The lever used for turning stiffness and strength. |
| `pose_grip_buzz` | 0.6 | Buzz strength for the gap (0 = off). |
| `pose_grip_turn_scale`, `pose_grip_turn_damping`, `pose_grip_turn_max_scale` | 4, 0.3, 100 | The turning hold: stiffness relative to the position hold, its damping ratio, and a multiplier on its cap. At cap x1 the weapon broke loose and spun after a few seconds (test 27). |
| `pose_grip_buzz_from_cm` | 10 | The buzz starts past this stretch. The game's own locked grip reads ~4.5 cm on this measure. |

For unattended tests the dev_script step `hsvr.give_weapon <class path>` equips a weapon through the game's own "Set Up Right Hand Weapon" (the scripted Free Mode start has none).

A bare class gives an unbuilt weapon (no blade, guard, grip or pommel), because the modular weapon builds itself only from its passport. To get a complete one, use `hsvr.give_weapon passport <inventory data asset> <index>`, which copies `Inventory.WeaponPssports[index]` from that asset into the call. The arming sword is `hsvr.give_weapon passport /Game/Blueprints/DataAssets/Equipment/FreeMode/DA_FreeMode_Inventory_Tier_Beggar.DA_FreeMode_Inventory_Tier_Beggar 0`.

Next: your strength from the game's stats instead of a fixed number, the hand staying on the weapon with a ghost hand at the controller, two hands, grab button, slide grip.


## Unattended test tools (dev_script steps)

The grip settings in the ini (pose_grip_*) are re-read every second during a run, so a test can change them live by editing the ini. Engine console commands in the dev_script (for example `shot`) take a screenshot at that step.

| Step | What it does |
|---|---|
| `hsvr.give_weapon passport <inventory asset> <index>` | Equips a fully built weapon; replaces the one in the hand |
| `hsvr.ghost_show 0/1` | 1 shows the ghost hand at your hand all the time (for fitting it) |
| `hsvr.hold_reach 0/1` | 0 lets the hold aim past his reach (to push a held weapon into the floor with arm_move 5); 1 (default) clamps it to his reach |
| `hsvr.grip_hold 0/1` | Turns the framework hold off or on for the test, whatever the ini says |
| `hsvr.grip_spring 0/1/2` | 0 leaves the game's locked joint alone (still measured), 1 our spring every tick, 2 our spring written once |
| `hsvr.grip_k_scale <x>` | Multiplies the spring's stiffness, damping and force caps |
| `hsvr.grip_frame_offset x y z` | Moves the joint's hand anchor (Frame2) by that much, to check the joint is live |
| `hsvr.grip_target x y z` | Linear drive target |
| `hsvr.grip_ignore_arm 0/1` | Weapon ignores collision with the forearm bodies |
| `hsvr.grip_rejoin` | Re-creates the hand joint |
| `hsvr.weapon_gravity 0/1` | Weapon gravity |
| `hsvr.weapon_channel <channel> <response>` | Weapon's response to one collision channel (0 ignore, 1 overlap, 2 block) |
| `hsvr.weapon_collision <n>` | Switches the weapon's collision mode. This rebuilds the body and breaks the hand joint; don't use it to test the grip |
| `hsvr.contact_log 0/1` | Logs which of his bodies, and which other components, touch the weapon (on by default) |
| `hsvr.arm_move <mode>` | Fake-tracking arms: 0 circles, 1 still, 2 slow swing, 3 fast swing, 4 overhead, 5 pushed down through the floor (the weapon is stopped by the ground) |
