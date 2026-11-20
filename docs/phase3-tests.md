# Phase 3 tests: your body posed by you

Phase 3 switches physics off on your own character and poses him from the headset and controllers. Off by default (`pose_body=0`) until it passes in the headset.

How it works (from the decompile, `HalfSwordDecomp\internals-map.md`, "Posing for Phase 3"): Willie's animation (ABP_Willie on the hidden DriverSkeleton) ends in one "Modify Bone" node per main bone, which the game only uses for muscle size. The mod writes their rotation every frame (component space, replace). The body you see copies the DriverSkeleton, so it takes the same pose. With physics off nothing moves the visible body, so the mod places it every frame with its pelvis on the game's capsule (the game pulls the capsule toward the pelvis, so anywhere else drags both away: the first unattended run flung him out of the arena that way). Walking in your room moves the capsule itself, with collision, and the view takes the same amount off its anchor.

- Head: turns with the headset.
- Arms: a two-bone IK from the shoulder to each controller (wrist offset and hand rotation adjustable).
- Legs, spine and stepping: the game's own animation.
- The view is anchored at the head height taken when posing starts, so it no longer follows the head bone (which now follows the view).

## Settings (`HalfSwordVR.ini`; new ones are added to an existing file at their defaults)

| Setting | Default | What it does |
| --- | --- | --- |
| `pose_body` | 0 | 1 = posing on (physics off on your character). Can be flipped while playing; restart the fight after switching it off. |
| `pose_head`, `pose_arms`, `pose_follow_room` | 1 | Turn each part off on its own. |
| `pose_hold_capsule` | 1 | While posed, the capsule moves only when you walk. Willie's own tick pulls the capsule toward his body and adds the hit stumble, dodge and lean offsets. With the body posed on the capsule, that pull never settled, so hits slid him across the arena. The mod puts the capsule back sideways after each of his ticks and clears those offsets. Its height stays the game's own, which follows his feet, except a rise of more than 30 cm in one tick (the game's climb over an opponent), which is undone. It also means the game's dodge does not move you. |
| `pose_wrist_forward_cm`, `pose_wrist_right_cm`, `pose_wrist_up_cm` | -2.7, 2, 7.5 | Where the wrist sits relative to the controller grip (forward through the fist, toward the back of the hand, up). The defaults put it 8 cm from the palm centre back along the fingers and 2 cm toward the back of the hand. |
| `pose_hand_pitch`, `pose_hand_yaw`, `pose_hand_roll` | 0 | Turns the hand relative to the controller. |
| `pose_knuckle_deg` | 20 | How far the fingers tilt from straight down the controller's handle toward its front. |

The hand's direction comes from Willie's own finger bones, measured when posing starts. The log line "hand frame from finger bones" shows it. The grip geometry follows the hand model in Matthew's Chasms VR Framework (MIT).
| `pose_body_turn` | 1 | His body faces where you face (see below). |
| `pose_turn_dead_zone_deg`, `pose_turn_speed_dps`, `pose_turn_max_deg` | 60, 150, 85 | How far you look away before the body turns, how fast it turns, and the furthest your head can be from his hips (past it the hips come round with you at once, so a quick spin doesn't leave his body behind). |
| `pose_torso_twist` | 0.7 | How much of your head's turn away from the hips the spine and neck take, more toward the top (spine_02 to neck_02), on top of the game's own animation. Your hips only turn with the body turning above. 0 = off. |
| `pose_lean` | 1 | Torso lean (Half Sword's own lean shape: spine_02-05 equal shares, neck_01/02 back 3/8 each). The torso hangs from your neck: looking down bends him forward and his hips go back, so the chest moves in out of your view; tilting your head sideways leans him sideways. Bend = (pitch below the horizon - `pose_lean_pitch_dead_deg` 10) x `pose_lean_pitch_gain` 0.7, at most `pose_lean_max_deg` 60; sideways = (roll - `pose_lean_roll_dead_deg` 5) x `pose_lean_roll_gain` 0.8, at most `pose_lean_side_max_deg` 25; smoothed over `pose_lean_smooth_s` 0.08. Held to what your head height allows (hips never above standing; `pose_lean_slack_cm` 4 of neck slack). |
| `pose_lean_step_radius_cm` | 30 | Lean first, step after: when your neck moves away from where his hips stand, his torso leans to put his head there (forward, sideways, up to 20 deg back); his capsule and feet follow only the part beyond this radius. Walking (neck faster than `pose_lean_walk_speed_cms` 50) brings his hips back under you within `pose_lean_walk_catchup_s` 0.25. 0 = hips always under your neck (the old way). |
| `pose_lean_cancel_game_bend` | 1 | With the lean, the game's own forward bend from crouching (Root Height) and grab reaching (Lean Forward (Grabbing)) is taken out of his animation, so only your lean bends him. |
| `pose_no_uro` | 1 | His body, head, driver skeleton and worn armour animate every frame (update-rate optimisation off; his body refreshes its bones every frame). The log line "Helmet:" measures the worn helmet's head bone against the headset each frame. |
| `pose_feet` | 1 | His feet stay where they were put down and step (lift and place, one at a time) instead of sliding with the body. 0 = the game's own feet. |
| `pose_step_distance_cm`, `pose_step_angle_deg` | 10, 30 | A foot steps once it is this far from where it would stand under the hips, or turned this far from them. |
| `pose_step_time_s`, `pose_step_lift_cm`, `pose_step_lead_s` | 0.3, 10, 0.2 | How long a step takes standing (quicker when moving), how high the foot lifts, and how far ahead a foot lands when walking (in seconds of movement). The foot height is the floor under the foot (a trace straight down) plus his ankle height measured when posing starts. |
| `pose_crouch`, `pose_crouch_dead_zone_cm` | 1, 3 | He crouches as low as you do, measured from where you stood when you recentred. |
| `pose_swivel_smooth_s` | 0.06 | How quickly the elbow follows as it swings around the arm. The hand itself is never smoothed. Where the elbow points is chosen each frame by the swivel search ported from the Chasms VR Framework (`crates/ik/src/arm.rs`, MIT). The search keeps the wrist bend and forearm twist in comfortable ranges and the elbow out of the ribcage. |
| `hide_own_head` | 1 | Your character's head and hair are not drawn in your own view while you look out of it (first person through the mod). They still cast their shadow and stay in the game. Helmets and other headwear stay visible. |
| `hide_own_gore_from` | neck_01 | The gore skeleton under his skin (SK_Skeleton) is hidden from this bone up, so its skull, jaw, teeth and throat don't show from inside your hidden head. The bones in wounds elsewhere on your body still show. Leave it empty to turn this off. |
| `dev_pose_fake_tracking` | 0 | TEST ONLY: the head and hands follow a slow made-up path instead of the headset and controllers, for unattended runs. |

## Known limits of this first version

- Crouching lowers the body by as much as your headset drops, and his leg IK bends the knees with the feet planted. The game's own crouch values follow (kept at 61 or more, so the game's canned kneel never starts). There's no voluntary kneel or prone yet.
- The body faces where you face: it follows the headset once you look more than 60 degrees away (`pose_turn_dead_zone_deg`) at 150 degrees a second (`pose_turn_speed_dps`). The mouse or stick still turns you too. Turn the game's "Camera Lock On" off: lock-on fights this. Stick movement is relative to your body. Turning in place steps the feet only after a large turn.
- Wrist offset and hand rotation come from the grip geometry, to be tuned in the headset.

## Arm style decision (2026-11-18)

Matthew tried the game's own muscles on the arms ("muscle" style) in the headset. Even with the strength multiplier set 500 times higher, the visible hands trailed his real hands by 15 to 35 cm whenever he moved. The game's arm muscles are rotation springs toward an animated pose. A spring only pulls once the arm has fallen behind, and the game re-applies every arm bone's settings each tick. So he chose **exact arms**: the arms are posed exactly onto the controllers, with no lag. Weight, strength and stamina act through the grip joint on whatever the hand holds (Phase 4). The muscle style is removed.
