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


## Slide along the grip

- While a hand holds a weapon, hold that controller's trigger: the point held moves toward where your hand is along the weapon's axis, faster the harder you squeeze (a light squeeze drags, a full one slides freely, B&S-style). It works either way and one-handed; a loose grip lets the weapon slip through the hand. Let go of the trigger and the hand holds there.
- The hand stops at the ends of the weapon's grip zone and the controller buzzes: half a hand inside the pommel end ("Length to Bottom") and inside the top stop, which is the Head module's start on modular weapons (a sword's guard, a mace or pollaxe head) or the highest "base"-tagged scene on reforged weapons and tools (from hsvr.weapon_dump of 7 weapons). Hands can't slide through a guard.
- Half-swording (the free hand on the blade) comes with the two-handed grip.
- Log lines: "Slide: ... grip zone", "starts sliding", "holds", and twice a second while sliding.

| Setting | Default | What it does |
|---|---|---|
| `pose_slide` | 1 | The slide. |
| `pose_slide_hand_half_cm` | 4.5 | How far inside each end of the zone the hand's centre stops. |
| `pose_slide_rate_min`, `pose_slide_rate_max` | 3, 40 | How fast (1/s) the hand catches up along the weapon at a light and a full trigger squeeze. |
| `pose_slide_max_cm_s` | 250 | Fastest the hand runs along a weapon (a game hitch is not a slide). No slide in the first second a weapon is in the hand. |
| `pose_hold_let_go_grace_s` | 1.0 | No stuck let-go this long after a weapon comes into the hand, or while it is still closing in (long weapons swing into place). |
| `pose_hold_ignore_hand` | 1 | The held weapon never collides with the hand holding it (its forearm never does either). |
| `pose_hold_block_offhand_search` | 1 | Keeps the game's off-hand search off for a free other hand while the mod holds a weapon ("Block <other> OffHand Grab" kept on and "<other> Offhand Searching Grip" off, just before his tick; a hand holding or grabbing something is left alone). With a longsword in one hand and the other free, the game re-seated the weapon about once a second (teleport to the hand socket and back), which shoved it across the blade. 0 = leave the game's search alone. |
| `pose_hold_contact_cm` | 0.5 | The stuck rule only lets go of a weapon that is touching something: one that ended the last tick this far short of where the hold sent it (B&S also needs contact). A weapon that only lags is kept. |

## Grab button

- Squeeze a controller's grip with that hand empty: the nearest loose weapon whose body is within `pose_grab_radius_cm` of the hand (the weapon socket the game snaps weapons to) is picked up with the game's own call, Set Up Right/Left Hand Weapon (what its grab does once the hand reaches a weapon; grab_pickup.md). The game snaps the weapon's default grip into the hand; the hold then takes over. The controller buzzes.
- Let go of the grip: that hand drops what it holds (the game's own drop, no damage).
- A weapon you already hold without squeezing (the one you spawn with) stays until you squeeze and let go.
- Weapons are anything the game counts as a ModularWeaponBP (weapons, shields, tools, bottles). Loose means not sheathed and not in anyone's hand.
- With no weapon in reach, the nearest other simulating body within the same distance (a loose object, armour, food, or anyone's bone) is handed to the game's own grab the way its search does it: Grab Component/Bone set, GrabPoint put on the spot, Stuck Meter 0, Grab Target Found (grab_pickup.md, UF @523112). Its next tick builds the hand joint with its own handling. If the game hasn't grabbed it 0.3 s later, a plain object goes to Grab Specific Object instead; a body is left. Letting go calls the game's Release Grab. Log lines: "Grab: ... handed to the game's latch", "the game grabbed", "Grab Specific Object", "Release Grab called".
- Log lines: "Grab: ... took", "Grab: ... no loose weapon within", "Grab: ... let go: dropped".

| Setting | Default | What it does |
|---|---|---|
| `pose_grab_button` | 1 | The grab button. |
| `pose_grab_radius_cm` | 10 | How near the weapon's body your hand must be. |
| `pose_grab_release_drops` | 1 | Letting go of the grip drops the weapon. |
| `pose_grab_objects` | 1 | Objects and bodies can be grabbed too. |

## Menu controls

The game's menus take only the mouse and keyboard (no focus or gamepad navigation; HalfSwordDecomp defaults/menus_and_prompts.md), so the controllers stand in for them.

- **Pointer:** while a menu is up, point either controller at the floating panel. A dot shows where it lands and the Windows mouse pointer moves to that spot in the game window. The trigger is the left mouse button (held = drag, for sliders); the controller buzzes on each click. The right hand points unless only the left is on the panel. Log line: "Pointer: ... clicks the panel at".
- **Pause:** the left menu button is Escape, held while you hold it: it opens the pause menu, and holding it closes the pause menu like holding Escape does. Log lines: "Menu: left menu button down/up".
- **"Hold G to continue":** after a won fight, hold right A for 1 s. The mod calls the player's VO Deescalate, which is what the game's held Talk (G) calls after a win (UI_WIN, then the Next Fight menu, which you click with the pointer). It only does this while the GameMode's "Match Won" is set and his "Player Match End" is not, because G itself also surrenders (held when the fight is not won) and bites (double press). Holding A at any other time does nothing and logs why. Log line: "Menu: A held".

| Setting | Default | What it does |
|---|---|---|
| `ui_pointer` | 1 | The controller pointer and trigger click on the panel. |
| `menu_buttons` | 1 | Left menu button = Escape; right A held = continue after a win. |
| `menu_continue_hold_s` | 1.0 | How long right A is held to continue. |

## Unattended test tools (dev_script steps)

Live steps: write `HalfSwordVR_commands.txt` next to the ini while the game runs. It is checked once a second; each time it is written, its steps (`<seconds>: <command>`, separated by `;` or new lines, `#` lines skipped) are added with their times counted from that moment, so one launch can run many tests. A file already there at launch is not run.

A step's time can be written `M+<seconds>:` to count from when the first menu is on screen (logged as "Script: first menu on screen at") instead of from when the script loaded, so a run doesn't need a fixed wait that covers the slowest start.

The grip settings in the ini (pose_grip_*) are re-read every second during a run, so a test can change them live by editing the ini. Engine console commands in the dev_script (for example `shot`) take a screenshot at that step.

| Step | What it does |
|---|---|
| `hsvr.give_weapon [left] passport <inventory asset> <index>` | Equips a fully built weapon (right hand, or the left with `left`, through "Set Up Left Hand Weapon"); replaces the one in the hand |
| `hsvr.match_won [0]` | Sets (0 clears) what the game sets when the last enemy dies (GameMode "Match Won", his "Match Won (Player)"), to test continuing without a fight |
| `hsvr.mark` | Clears the test counters (let-goes, kicks, worst hold lag) |
| `hsvr.expect <what> <op> <value> [label]` | Logs "EXPECT PASS" or "EXPECT FAIL". what: held_r, held_l, grabbed_r, grabbed_l, letgo and kicks (counts since hsvr.mark), lag_r, lag_l (worst cm since hsvr.mark); op: < <= = >= > |
| `hsvr.joint_trace 0/1` | Logs the right hand joint every tick before and after our free, with his soft-attach variables ("Joint:") |
| `hsvr.watch_vars grip,offhand,stuck` | Every tick, logs each change ("Watch: <time> pawn.<name> old -> new") of the number, yes/no and enum variables on his pawn and on the weapon in each hand whose names contain one of the words (case ignored). No words = stop. For matching a game routine to a kick by its timing |
| `hsvr.passive_foe` | Makes every foe a training dummy: its AI controller stopped (DED, tick off, movement stopped) and its attack inputs cleared; its physics, balance and get-up keep working. Spawn one with Free Mode Foes Amount=1, Brawling off, and run this right after it spawns |
| `hsvr.force_watch 0/1` | Logs ("Force:") every physics write from game script (impulses, forces, velocities, teleports, collision and physics switches) on a part of a weapon in his hands or his own body, with the calling Blueprint function and bytecode offset |
| `hsvr.break_joint 0/1` | Breaks the game's hand joint on the held weapon (test: only the mod's hold links them) |
| `hsvr.pin_disabled 0/1` | Keeps his "Weapon R/L Collisions Disabled Temporarily" set (gates his back sweep) |
| `hsvr.stuck_check 0/1` | 0 pins his "Weapon Stuck In Static Mesh Check Rate" at 0 so his 1 s Check Stuck In Static Mesh loop never fires (the kick hunt). "Kick:" lines log every tick a held weapon's body (mass, centre of mass) or Unstuck flag changes |
| `hsvr.weapon_dump [0/1]` | Logs the weapon in the left (0) or right (1, default) hand: its components with their spans along the blade, BaseMesh sockets, the hand joint point and its Length variables |
| `hsvr.bring_object [left]` | Moves the nearest loose physics object (not a weapon or character) to the right hand (or the left) with its gravity off and keeps it there for 4 s or until grabbed, to test the object grab |
| `hsvr.trigger <0/1> <value>` | Fakes the left (0) or right (1) trigger (0..1); -1 gives it back to the controller |
| `hsvr.squeeze <0/1> <value>` | Fakes the left (0) or right (1) grip squeeze (0..1); -1 gives it back to the controller |
| `hsvr.press_a 0/1` | 1 holds right A for the menu buttons (continue after a win) |
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
| `hsvr.arm_move <mode>` | Fake-tracking arms: 0 circles, 1 still, 2 slow swing, 3 fast swing, 4 overhead, 5 pushed down through the floor (the weapon is stopped by the ground), 6 reaching 54-70 cm out in front (past his arm: the shoulder reaches) |
