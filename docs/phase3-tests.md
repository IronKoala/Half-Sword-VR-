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
| `pose_arm_style` | exact | `exact` = arms posed exactly; `muscle` = arms on physics, pulled to the pose by the game's muscles. |
| `pose_wrist_forward_cm`, `pose_wrist_right_cm`, `pose_wrist_up_cm` | -6, 0, -2 | Where the wrist sits relative to the controller grip. |
| `pose_hand_pitch`, `pose_hand_yaw`, `pose_hand_roll` | 0 | Turns the hand relative to the controller. |
| `pose_elbow_back`, `pose_elbow_out`, `pose_elbow_down` | 0.3, 0.6, 1 | Which way the elbows point. |
| `hide_own_head` | 1 | Your character's head and hair are not drawn in your own view while you look out of it (first person, headset running). They still cast their shadow and stay in the game. Helmets and other headwear stay visible. |
| `dev_pose_fake_tracking` | 0 | TEST ONLY: the head and hands follow a slow made-up path instead of the headset and controllers, for unattended runs. |

## Known limits of this first version

- Crouching in the room doesn't crouch the body yet (the view goes down, the body stays standing).
- The body turns with the mouse heading, not your hips.
- Wrist offset and hand rotation are first guesses, to be tuned in the headset.
