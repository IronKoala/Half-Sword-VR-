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
| `pose_hold_spring`, `pose_hold_damper`, `pose_hold_max_force` | 2500, 150, 3000 (B&S: 5000, 100) | The hold's spring (N/m), damper (N s/m) and most force (N), the framework's one-hand values. |
| `pose_hold_turn_spring`, `pose_hold_turn_damper`, `pose_hold_max_torque` | 1100, 75, 30 (B&S: 45, 250; 30 N m is a wrist: a war staff held by its end droops) | Its turning spring (N m/rad), damper and most torque (N m), the framework's values with its handle multipliers (x1.1, x0.9). |
| `pose_hold_hand_on_weapon`, `pose_hold_blocked_from_cm` | 1, 3 | While held, his visible hand sits on the weapon when something stops it, instead of going on with your controller (Matthew's framework). It eases in as the weapon falls 1 to 3 cm behind where the hold sent it. |
| `pose_hold_let_go`, `pose_hold_let_go_s` | 1, 0.15 | The stuck rule: a stopped weapon whose grip is out of his reach from the shoulder, or more than an arm's length from your hand, for 0.15 s, is let go with the game's own drop (Set Up Right/Left Hand Weapon with no weapon, dropped with no damage). |
| `pose_hold_let_go_ignore_self` | 1 | A weapon stopped by his own body (any of his physics bodies within 3 cm of its axis) is kept in the hand, not let go. Vanilla weapons collide with his body too, so the collision stays. |
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
| `pose_slide_zone_measured`, `pose_slide_wide_cm` | 1, 4 | The grip zone comes from the weapon's real parts: walking its axis from the grip, it ends where the weapon ends or gets wider than 4 cm from the axis (a guard, a polearm's hook or head, a wide pommel). The log line "measured along its axis" gives both ends next to the game's own lengths. 0 = the game's lengths and its Head module (wrong on polearms: the billhook's zone was 21 cm of a 231 cm weapon). |
| `pose_slide_rate_min`, `pose_slide_rate_max` | 3, 40 | How fast (1/s) the hand catches up along the weapon at a light and a full trigger squeeze. |
| `pose_slide_max_cm_s` | 250 | Fastest the hand runs along a weapon (a game hitch is not a slide). No slide in the first second a weapon is in the hand. |
| `pose_hold_let_go_grace_s` | 1.0 | No stuck let-go this long after a weapon comes into the hand, or while it is still closing in (long weapons swing into place). |
| `pose_hold_ignore_hand` | 1 | The held weapon never collides with the hand holding it (its forearm never does either). |
| `pose_body_strength` | 1 | His stats scale the hold's force and torque caps: strength (Muscle Rate, as the game's grip uses it: 0..1 -> 1..1.5, at least 0.5, over its usual value 1.25), that arm's tonus (injury, exhaustion) times consciousness/100, and stamina (100..0 -> 1..`pose_body_stamina_floor`); the weaker of tonus and stamina counts, since the game's tonus already falls with exhaustion. 1 = a fresh, healthy Willie. Two hands: the second adds its own. Logged once as "Strength: ...". |
| `pose_body_stamina_floor` | 0.5 | What is left of the hold at stamina 0. |
| `pose_body_strength_min` | 0.1 | The hold never goes weaker than this. |
| `pose_stamina_scale`, `pose_stamina_swing_lo_cm_s`, `pose_stamina_swing_hi_cm_s`, `pose_stamina_speed_smooth_s`, `pose_stamina_force_cap_n`, `pose_stamina_torque_cap_nm` | 1, 200, 700, 0.7, 150, 15 | Holding and swinging cost stamina by the game's own formula (stamina_calibration.md), run on the hold per hand: burn = (force N, capped at 150, + 100 x torque N m, capped at 15) / 60 x MRC(Muscle Rate, 0..3 -> 1..0.1), only from 10 up; swing = the controller's speed at the grip smoothed over 0.7 s, 200..700 cm/s -> x10..x85; per second burn x swing x 0.0125 x MRC(Fat Rate, -1..10 -> 0.5..2.25) x scale. The game regenerates about 26 a second when healthy, so normal swinging never empties the bar (as in the game); only flailing nonstop with both hands does, over 10 s or more. Scale 3-5 makes continuous swinging tiring. Logged per hand each second as "Stamina: right hand: ..." with Muscle Rate and Fat Rate. |
| `pose_two_hand` | 1 | Two hands on one weapon: squeeze the free hand on a weapon the other hand holds, on the grip or anywhere along the blade (half-swording). The weapon lies along the line between the hands, the first hand sets its roll, and the hold is `pose_two_hand_strength` (2) times as strong. The second hand's trigger slides it along the zone it is on; the guard stops it. Letting go with the first hand hands the weapon to the second, where it holds it (B&S). |
| `pose_two_hand_blade_gap_cm` | 3 | The blade zone starts this far past the guard (plus half a hand). |
| `pose_two_hand_reach_cm` | 6 | A squeeze this close to a zone (along the weapon) still lands on it. |
| `pose_two_hand_let_go_cm` | 25 | The second hand lets go when its controller is pulled this far off its spot for 0.3 s. |
| `pose_two_hand_curl` | 0 | Only with `pose_fingers_anim_only` 0. 1 = the second hand's fingers close (its grip type set to the sword grip while it holds). Off by default: the game then drops the first hand's two-handed grip type (11 -> 2) and with it its two-handed rules (Master Stroke, the two-hand stamina factor and more; fists_eating.md). |
| `pose_fingers_anim_only` | 1 | VR owns the fingers: each hand's finger shape (the handle's: sword grip on a grip, hammer grip on a blade or a spot, the strap shape on a shield; the second hand's too) is written only into ABP_Willie's own copy ('R/L Grip Type') right after its BlueprintUpdateAnimation, for the player only. The pawn's R/L_GripType_Current stay the game's, so its two-handed rules (11 -> 2 when the left's is non-zero), Master Stroke and Set Up never see them (grip_type_flow.md). Log: "Fingers: <hand> shows N". 0 = the old way (pawn values). |
| `pose_feed_swing_speed`, `pose_swing_speed_lo`, `pose_swing_speed_hi`, `pose_swing_speed_fall_s` | 1, 1.5, 7.0, 0.15 | The game's swing speed (Aim Swing Speed 0..75, mouse-only) from the controllers: the faster hand that holds something (weapon, item or fist), its speed relative to the body, lo..hi m/s -> 0..75, exactly 0 below lo; rises at once, falls over `fall_s`. Written as the game's own input ("Swing Velocity proxy") before his tick, so weapon skill growth, combat stance, wrist locks, the AI's threat timing, weapons live when fallen, unsheath length etc. see it (aim_swing_speed_flow.md). Log "Swing: hands .. m/s, swing speed wanted N, game's now N" each second. Calibration aim: one normal swing ~20, a frantic one 60+ (a mouse swing peaks ~22). |
| `pose_grab_seat` | 1 | B&S's fixed orientations: a weapon taken by its grip or blade (either hand, first or second) sits at the game's own seat (weapon_rSocket / weapon_lSocket in the hand bone, the same for every ordinary weapon; seated_orientation.md) or that seat with the edge turned, the blade reversed, or both, whichever is nearest to how it was; its handle runs through the seat's point, where the fingers close. Log "Grab: <hand> seats it <how> (N deg from how it was)". 0 = turned the least that lays the handle along the hand (old). |
| `pose_grab_seat_reverse_bias_deg` | 45 | A reversed seat (pommel up) is chosen only when it is this many degrees nearer than a blade-up one (test 737-T: all three fake grabs chose reversed, one at 81 deg). |
| `pose_throw`, `pose_throw_speed`, `pose_throw_multiplier` | 1, 2.0, 2.0 | Throwing (B&S: let go faster than 2 m/s, velocity x2). A weapon let go while that hand moves at least `pose_throw_speed` m/s (world) is dropped as a throw (Set Up Hand Weapon with "Dropped With no Damage" false, so the game sets Just Thrown and counts its hits; Last Parent = you, kept while it flies past the game's 3 m cut-off) and its velocity multiplied. When it lands (slower than 1.5 m/s after 0.3 s) or after 4 s the mod clears Just Thrown (the game clears it only after a hit). Log "Throw: ...". Slower = a plain drop, as before. |
| `pose_draw`, `pose_draw_touch_cm`, `pose_draw_settle_s` | 1, 8, 0.15 | Drawing from a scabbard (Matthew 2026-11-19: grab the handle and pull it out along the scabbard; the game's 85% release kept). A squeeze within `touch_cm` of a sheathed weapon's grip (any belt/back slot) calls the game's Draw Weapon and does its attach at once (Set Up Hand Weapon in place, Weapon UnSheathing, Set Scabbard Constraint Free); the slot's slide joint keeps the blade on the scabbard's line and the hold moves it only along that line. The game frees it at 0.85 x its length (swing speed held at 0 while drawing) and the hold eases it into the hand over `settle_s`. Let go mid-draw and the game seats it again. Log "Draw: ...". Test path: hsvr.arm_move 11 (right hand to the first sheathed grip, then pulls out). hs_draw_sheathe_calls.md. |
| `pose_draw_step_cm` | 6 | The most a drawn or sheathed blade moves along the scabbard per tick (draw-test2: a hand starting far off yanked it 20 cm/frame). A draw also puts the slide's reference back at (0,0,L), so the blade cannot go in past the seat (draw-test2 reached -33 cm). |
| `pose_draw_force_scale`, `pose_draw_scabbard_free` | 0.3, 0 | While drawing or sheathing, the hold's force and torque caps are scaled by `force_scale`, and the scabbard's belt pin is the game's Locked one (2 cm / 5 deg) instead of its Free one (15 cm / 30 deg, meant for its own off hand): draw-test3 dragged the scabbard 34-71 cm off the belt. 1 = the game's Free pin. |
| `pose_hold_stuck_force_scale` | 0.15 | While the held weapon is Stuck In Body, the hold's force and torque caps are scaled by this, so the hold catching up does not tear the blade out (stab-test8: every lodge released 10-20 ms after it stuck, with the hold pulling 356-2860 N). A first value, to be tuned with the stab tests. Log "Hold: ... stuck in a body: pulling at N% strength". |
| `pose_sheathe`, `pose_sheathe_mouth_cm`, `pose_sheathe_angle_deg` | 1, 10, 30 | Sheathing, the reverse of a draw: a held weapon with its own scabbard, its tip within `mouth_cm` of that hand's side's empty belt scabbard mouth (slot 1, else 2; the mouth is the slot socket) and its blade within `angle_deg` of the scabbard's line, goes in: the mod calls the game's Assign Weapon Slot (bookkeeping), undoes its arm reach, makes the slide joint (Create Sheath Constraint) at once; the hand then pushes it in along the line and the game seats it and lets go at 20 cm. Pull back out instead and it is a draw. Log "Sheathe: ...". Tests: hsvr.sheathe_test (the right hand's weapon put at the right mouth, sheathe started), then hsvr.arm_move 12 (the right hand pushes in along the line); then arm_move 11 draws it again. |
| `pose_two_hand_lock_rotation` | 1 | While both hands hold, each hand sits on the weapon turned as it holds it (the second hand mirrored from the first), instead of turning with its controller (playtest 2026-11-18: the off hand twisted and clipped through the handle). |
| `pose_two_hand_mirror` | 1 | Which weapon axis the second hand mirrors across when locked (0 = X, 1 = Y). |
| `pose_fists` | 1 | Squeezing an empty grip with nothing in reach makes that hand a fist: the game's own punching weapon (Weapon_Fists_C), put there the game's way; letting go opens it. Damage is the game's blunt hit from the fist's speed. |
| `pose_fist_class` | /Game/Assets/Weapons/Blueprints/Built_Weapons/Weapon_Fists.Weapon_Fists_C | The fist's class (loaded through the asset registry if the game has not loaded it yet; empty = this path). |
| `pose_fist_mark_spawned` | 0 | 1 = a fist the mod makes sets the game's "Fist R/L Spawned"; its one-shot removal then takes the first fist at once ("R Hand Active" is 0 in VR). At 0 the mod opens the fist on release, when knocked out or when falling. |
| `pose_eat` | 1 | Grabbed food within `pose_eat_cm` (12) of your mouth is bitten (the game's Bite Event); a held bottle at your mouth with its neck tipped below `pose_drink_tip` (0 = level) is drunk from (Drink From Bottle, or Event Gulp once 0.5 is in the mouth), one sip every `pose_drink_sip_s` (1) s, like the game (a bottle holds 0.5 and each sip takes 0.1); taking the bottle away swallows what is in the mouth. |
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
| `pose_grab_radius_cm` | 18 | How near the weapon's body (its real shape) your hand must be. Was 10: in the headset 8 floor pickups missed by 10-17 cm (playtest 2026-11-18). |
| `pose_shield_free_grip` | 1 | Shields: every strap and handle measured on the shield's mesh is a grab point. The hand closing within `pose_shield_point_cm` of one the game itself uses for that hand takes the game's grip; any other fitting is held in the hand, turned so the strap or handle lies along the hand's grip line, and anywhere else (the rim, an edge) the hand holds it where it touched. 0 = always the game's grip. |
| `pose_shield_point_cm` | 8 | How near a strap or handle the hand must close to take the shield by it. |
| `pose_shield_in_hand`, `pose_shield_hand_grip_type` | 1, 2 | Strap shields the game puts on the forearm (the round's own, or taken by its forearm strap) are held in the hand instead, by the fitting nearest the hand (its line along the hand's grip line). Fingers of a hand holding a shield by the mod's grip take grip type 2 (the game's curled sword grip); -1 leaves the game's. |
| `pose_weapon_free_grip` | 1 | A loose weapon taken more than `pose_weapon_free_cm` (6) from its grip (by the blade, the haft, the tip) is held where the hand closed, turned as it was, instead of snapped onto its grip. The other hand cannot join it two-handed yet. |
| `pose_weapon_free_cm` | 6 | How far from the weapon's grip point counts as taking it elsewhere. |
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
| `hsvr.expect <what> <op> <value> [label]` | Logs "EXPECT PASS" or "EXPECT FAIL". what: held_r, held_l, grabbed_r, grabbed_l, letgo and kicks (counts since hsvr.mark), lag_r, lag_l (worst cm since hsvr.mark), stamina, strength_r, strength_l (the hold's stat factor), two_hand (1 while two hands hold), fist_r, fist_l, food (Nutrients Due), liquid (Body Liquid); op: < <= = >= > |
| `hsvr.joint_trace 0/1` | Logs the right hand joint every tick before and after our free, with his soft-attach variables ("Joint:") |
| `hsvr.arm_move 7 <cm>` | Fake arms for two-hand tests: the right hand still in front, the left this many cm from it along the weapon the right hand holds (+ toward the tip, - toward the pommel) |
| `hsvr.pawn_pin <variable> <value>` / `hsvr.pawn_pin off` | Holds a number variable of his pawn at a value every tick, after his tick (e.g. `Stamina 10`, `Arm R Tonus 0.25`), to test the strength scaling |
| `hsvr.spawn <class path> [left]` | Spawns an actor of that class just above that hand (e.g. food: /Game/Assets/Food/Blueprints/BP_Food_Apple_Red.BP_Food_Apple_Red_C). `hsvr.give_weapon` now refuses anything that is not a ModularWeaponBP (an apple through Set Up Hand Weapon is a fatal engine error). |
| `hsvr.arm_move 8` | Fake arms: both hands at his mouth (eating and drinking tests); `9` the same with the hands turning over (a full turn in 6 s) so a bottle tips |
| `hsvr.bring_object [left] food` | As bring_object, food items only |
| `hsvr.bring_object [left] shield (or weapon) [rim] [at x y z]` | The nearest loose shield (or non-shield weapon) kept at that hand (up to 8 s, until the hand holds it): its middle, with `rim` a point on its edge, or with `at x y z` that point of it (BaseMesh frame, cm) |
| `hsvr.hide <text> 0/1` | Every component of his whose name or tags contain `<text>` (any case; `all` = every one) hidden from your view (1) or shown again (0) |
| `hsvr.camera_probe` | Logs your eye against his head, neck and spine bones, and every visible part of him: its mesh, tags, the eye's distance to its collision and bounds, and whether the eye is inside its bounds |
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

## Crouch (2026-11-19, playtest: body sank through the floor at a deep crouch)

| Setting | Default | What it does |
|---|---|---|
| `pose_crouch_keep_leg_ik` | 1 | The animation's kneel and falling leg branches (every Kneel flag and Free Fall Rate in ABP_Willie's own copy) are kept off each frame, so the legs stay on the leg IK. The log line "Crouch: kept off in the animation" lists what was found; "the animation wanted canned legs" shows when the game tried. The pawn's own values are not touched. |
| `pose_player_no_kneel` | 1 | No knockdowns for your player. His own R/L Kneel and R/L Kneel Falling are kept off and Legs Tonus Far Step kept at 1, just before and after his tick. The game read a bent, leaning VR pose as kneel-falling, fired its Kneel Event, and while kneeling it cut leg tonus (and the pelvis drive) to 0, so the body sank about 60 cm and stayed down. "Knees: kept off" is logged once; "Knees: the game wanted your player down" (at most once a second) shows each time it tried. Enemies are not touched; stamina and leg injuries work as before. |
| `pose_capsule_floor_clamp` | 1 | When the game drops the capsule more than 20 cm in a tick (its foot-trace miss drop is ~77 cm), it is kept no more than 15 cm below its standing height over the floor under it. Only on such a drop, since the floor trace can land on a loose shield. Logged as "Capsule: the game put it ... into the floor". |
| `pose_hold_wrist_free_deg` / `pose_hold_wrist_stop_deg` / `pose_hold_wrist_stop_torque` | 25 / 50 / 250 | The wrist's end stop: within 25° of the hand the item is turned only with the wrist's strength (`pose_hold_max_torque`), so a long weapon held at its end droops; from 25° to 50° the limit rises to 250 N m (times his strength), so a stopped swing doesn't carry a short sword on past the hand. |
| `pose_capsule_no_step_offset` | 1 | The game sets the capsule height from the lower foot plus `clamp(Step Height 2, ±10)`; that term sinks to about −4 cm during each step and snaps to 0 when a foot lands, so walking bobbed and hopped the view and his body ~2×/s. On: the step term (read before his tick) is taken back out, except while climbing, jumping or fallen. The "Capsule:" line shows "step offset taken out up to …". |

Unattended: with `dev_pose_fake_tracking`, `hsvr.fake_head_drop <cm>` holds a crouch depth (-1 = the made-up path). In the headset: crouch slowly to the deepest you can and hold 10 s, a few times. Pass: feet on the floor, head where your headset is, no "into the floor" lines after the first second.

## Exact hold (2026-11-19, grab rebuild step 1)

| Setting | Default | What it does |
|---|---|---|
| `pose_hold_exact` | 1 | Each frame the held weapon gets the velocity that puts it exactly where the hand is drawn at the end of this frame's physics (this frame's own delta time, GetWorldDeltaSeconds; gravity during the frame taken off ahead). Only the hand's strength (`pose_hold_max_force`, `pose_hold_max_torque` x his strength) limits the change, so a blocked or too-heavy weapon falls behind. 0 = the old spring. Why: the spring trailed the hand by its lag and used the last frame's time; Willie's kinematic hand also moves in one jump per frame inside the physics substeps (grab_rebuild_research.md section 3), so nothing is driven through the hand body. Logged as "Hold: right exact hold, frame ... ms (measured ...), ... cm and ... deg to go". |

## Handle system (2026-11-19, grab rebuild step 2)

| Setting | Default | What it does |
|---|---|---|
| `pose_grab_handles` | 1 | One grab for everything, B&S-style. Each item's handles are found once and logged ("Handles: <item>: grip (..)..(..); blade ..."): a weapon's grip measured along its axis from its real parts (a polearm's whole haft), the blade past the guard, and every measured shield strap and handle. A squeeze takes only an item the palm touches (`pose_grab_touch_cm` 5), held on the nearest handle within `pose_grab_handle_cm` 6 (its line laid along the hand's grip line), else where it was touched. A weapon the other hand holds is taken the same way (two hands). The game's own grip (weapons he spawns with) becomes the same kind of grip on its handle. 0 = the old grab. |
| `pose_slide_keep_light`, `pose_slide_keep_full` | 0.6, 0.97 | Holding the trigger, either hand slides along its handle: the item keeps this much of its own motion along the handle (light .. full squeeze) while staying in the hand across it; the hand's spot is where your hand is on the handle. Stops at the handle's ends. Needs `pose_hold_exact`. |
| `pose_slide_damper_light`, `pose_slide_damper_full` | 300, 20 | B&S's slide: along its handle the item's own motion is kept by a damper of this many N s/m (light .. full trigger), i.e. keep = exp(-c·dt/m) each frame, as B&S's axis drive (Handle.cs). 0 = use `pose_slide_keep_*` instead. |
| `pose_grab_settle_s` | 0.08 | B&S's grab: the item stays where it lay (held there while the game sets it up in the hand) and the hold then pulls it into the hand over this many seconds, eased, still capped by strength. 0 = it is placed in the hand at once (the old way). |
| `hit_buzz`, `hit_buzz_scrape`, `hit_buzz_blade_on_plate`, `hit_buzz_ignore_self`, `hit_buzz_impulse_full`, `hit_buzz_log` | 1, 1, 1, 1, 3000, 1 | Hit vibration as Blade & Sorcery does it: the buzz is B&S's hit-sound envelopes (sword on sword, slash on flesh, mace, punch) at the volume their curves give for the hit's intensity, InverseLerp of the contact speed (0.25-15 m/s; blunt 6-16; fists, open hands and forearms 2-9), summed per hand every frame as 1/30 s pulses. Weapon/shield hits buzz every hand holding either item, punches only the punching hand, hits taken on the body nothing; world hits only scrape ticks (0.8 per 2 cm of slide). A blow on a held item also takes intensity from the game's impulse (15..`hit_buzz_impulse_full`). B&S is silent for a blade on plate; `hit_buzz_blade_on_plate` 1 gives it the metal hard layer. `hit_buzz_ignore_self` 1: a weapon's own parts touching (a flail head on its haft; same actor, a component it owns, or an actor attached to or owned by it) never buzz. "Hit buzz:" log lines. |

Finger shapes: grips keep the game's own (the second hand's sword grip 2); blades, shield straps and spots touched take the hammer grip (5). A left hand never gets grip types 10-15 (no curl).
