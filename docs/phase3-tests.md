# Phase 3 tests: your body posed by you

Phase 3 switches physics off on your own character and poses him from the headset and controllers. Off by default (`pose_body=0`) until it passes in the headset.

How it works (from the decompile, `HalfSwordDecomp\internals-map.md`, "Posing for Phase 3"): Willie's animation (ABP_Willie on the hidden DriverSkeleton) ends in one "Modify Bone" node per main bone, which the game only uses for muscle size. The mod writes their rotation every frame (component space, replace). The body you see copies the DriverSkeleton, so it takes the same pose. With physics off nothing moves the visible body, so the mod places it every frame with its pelvis on the game's capsule (the game pulls the capsule toward the pelvis, so anywhere else drags both away: the first unattended run flung him out of the arena that way). Walking in your room moves the capsule itself, with collision, and the view takes the same amount off its anchor.

- Head: turns with the headset.
- Arms: a two-bone IK from the shoulder to each controller (wrist offset and hand rotation adjustable).
- Legs, spine and stepping: the game's own animation.
- The view is anchored at the head height taken when posing starts, so it no longer follows the head bone (which now follows the view).

## Settings (`HalfSwordVR.ini`, add by hand on an existing install)

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
| `pose_turn_dead_zone_deg`, `pose_turn_speed_dps` | 35, 150 | How far you look away before the body turns, and how fast it turns. |
| `pose_crouch`, `pose_crouch_dead_zone_cm` | 1, 3 | He crouches as low as you do, measured from where you stood when you recentred. |
| `pose_swivel_smooth_s` | 0.06 | How quickly the elbow follows as it swings around the arm. The hand itself is never smoothed. Where the elbow points is chosen each frame by the swivel search ported from the Chasms VR Framework (`crates/ik/src/arm.rs`, MIT). The search keeps the wrist bend and forearm twist in comfortable ranges and the elbow out of the ribcage. |
| `hide_own_head` | 1 | Your character's head and hair are not drawn in your own view while you look out of it (first person through the mod). They still cast their shadow and stay in the game. Helmets and other headwear stay visible. |
| `hide_own_gore_from` | neck_01 | The gore skeleton under his skin (SK_Skeleton) is hidden from this bone up, so its skull, jaw, teeth and throat don't show from inside your hidden head. The bones in wounds elsewhere on your body still show. Leave it empty to turn this off. |
| `dev_pose_fake_tracking` | 0 | TEST ONLY: the head and hands follow a slow made-up path instead of the headset and controllers, for unattended runs. |

## Known limits of this first version

- Crouching lowers the body by as much as your headset drops, and his leg IK bends the knees with the feet planted. The game's own crouch values follow (kept at 61 or more, so the game's canned kneel never starts). There's no voluntary kneel or prone yet.
- The body faces where you face: it follows the headset once you look more than 35 degrees away (`pose_turn_dead_zone_deg`) at 150 degrees a second (`pose_turn_speed_dps`). The mouse or stick still turns you too. Turn the game's "Camera Lock On" off: lock-on fights this. Stick movement is relative to your body. Turning in place steps the feet only after a large turn.
- Wrist offset and hand rotation come from the grip geometry, to be tuned in the headset.

## Arm style decision (2026-11-18)

Matthew tried the game's own muscles on the arms ("muscle" style) in the headset. Even with the strength multiplier set 500 times higher, the visible hands trailed his real hands by 15 to 35 cm whenever he moved. The game's arm muscles are rotation springs toward an animated pose. A spring only pulls once the arm has fallen behind, and the game re-applies every arm bone's settings each tick. So he chose **exact arms**: the arms are posed exactly onto the controllers, with no lag. Weight, strength and stamina act through the grip joint on whatever the hand holds (Phase 4). The muscle style is removed.
