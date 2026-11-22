#!/usr/bin/env python3
"""Rewrites HalfSwordVR.ini into the organized layout below, keeping every value.

  organize_ini.py <in.ini> <out.ini>        your ini, reorganized (values kept exactly)
  organize_ini.py --defaults <out.ini>      the layout with the mod's own defaults (config.hpp)

Every setting the mod reads (config.cpp) must be placed in LAYOUT exactly once, or the script
stops and names it. Keys in the input the mod no longer reads go to the last section, values kept.
After writing, the key=value pairs of input and output are compared and must match exactly.

The mod reads the file as plain key=value lines; lines starting with ; # or [ are skipped, so the
section headers and comments here change nothing. A key written twice: the last one counts.
"""
import re
import sys
import textwrap
from pathlib import Path

SRC = Path(__file__).resolve().parents[2] / "mod" / "HalfSwordVR" / "src"

# (section title, section note, [(key, comment)]), grouped by feature. Every key has a comment.
LAYOUT = [
    ('Startup', 'Whether the mod and VR start.', [
        ('enabled', '0 = mod off.'),
        ('vr_auto', '1 = VR only when SteamVR is already running as the game starts (else plays flat); 0 = always start VR.'),
        ('vr_auto_processes', 'Process names that count as SteamVR running.'),
        ('require_steamvr_running', 'Older name of vr_auto (only read when present; 0 = same as vr_auto=0).'),
    ]),
    ('Test room', 'The practice arena (Free Mode tavern: Train button).', [
        ('test_room', '1 = boot straight into the test arena (passive foes, two weapon tables); 0 = normal game. Read at launch.'),
        ('test_room_button', "1 = in Free Mode the tavern's Train (the fencer) has a Train button that opens the test room."),
        ('test_room_foes', 'Foes in the test room (1-10).'),
        ('test_room_map', 'The Free Mode arena the test room opens.'),
        ('test_room_tier', "Foes' tier: -1 random, 0 Beggar .. 8 Lord."),
        ('test_room_weapon_tier', "Table weapons' tier: -1 random 0-7, 0 Beggar .. 8 Lord."),
        ('test_room_respawn', '1 = a foe out of the fight is replaced.'),
        ('test_room_respawn_seconds', 'Seconds after a foe is out before his replacement appears.'),
        ('test_room_despawn_seconds', 'His body is removed this long after he is out (0 = bodies stay).'),
        ('test_room_crossbow', 'A crossbow on the table and 20 bolts on your hip: 1 = via the Train button, 2 = always, 0 = never.'),
    ]),
    ('Picture quality and performance', 'Resolution, sharpness, lighting.', [
        ('render_scale', 'Per-eye resolution as a share of what the headset asks for (1.0 = full).'),
        ('screen_percentage', "Share of the eye the engine draws before upscaling (50-100; -1 = game's own)."),
        ('sharpen', "0 = none, 0.5-1 = crisper (-1 = game's own)."),
        ('light_samples', "Bounce-light density: one sample per this many pixels (game 64 flickers; 32/16/8 denser and slower; 0 = game's)."),
        ('gamma_mode', '0 auto, 1 always decode sRGB, 2 never.'),
        ('fix_clouds', '1 = keep clouds correct in both eyes.'),
        ('cloud_mode', '2 or 0.'),
        ('separate_render_target', '1 = full headset resolution with menus on a panel; 0 = fallback through the game window.'),
        ('predict_frames', 'Pose prediction frames (-1 = automatic).'),
        ('pose_max_age', 'A head pose older than this many presented frames is skipped for its picture (-1 = no check).'),
        ('extra_commands', 'Extra engine console commands, separated by semicolons.'),
        ('idle_helper', '1 = game keeps normal speed while the headset sleeps.'),
        ('mirror_window', 'Show the left eye in the game window.'),
        ('stereo_slot_offset', '0 = find automatically.'),
    ]),
    ('Eyes and world scale', 'Where your eyes sit in his head and how big the world feels.', [
        ('world_scale', '1 = life size.'),
        ('anchor', "head = view from his head bone, camera = the game's camera."),
        ('head_bone', 'The bone the view sits on with anchor=head.'),
        ('heading', 'Straight ahead follows aim (mouse) or camera.'),
        ('eye_forward_cm', 'Eyes from the head bone, forward (cm).'),
        ('eye_up_cm', '...and up (cm).'),
        ('anchor_smoothing', 'Smoothing on the head anchor (s; takes physics jitter out).'),
        ('tracking_current_anchor', '1 = controllers and headset placed from where he is this frame, not last frame (hands shook while walking).'),
        ('anchor_room_unsmoothed', "1 = your room steps move the view at once (not smoothed), so the world and your hands don't swing when his feet follow you."),
        ('near_clip_cm', 'Near clip of the eyes, cm. Keep it at 1: at 4 the sides of closed helmets were cut away (see-through).'),
    ]),
    ('Helmet and your own head', 'What you see of your own head and helmet. Others always see it; it always protects you.', [
        ('hide_own_head', "1 = your own head isn't drawn in your view (shadow and helmet stay)."),
        ('hide_own_gore_from', 'Gore skeleton hidden from this bone up (empty = not hidden).'),
        ('helmet_view', 'Your worn helmet in your view: 0 = drawn (blocks the view like a real one), 1 = helmet hidden from you, 2 = everything on your head hidden from you, 3 = clear window (parts near your eyes not drawn), 5 = see-through (faded, see helmet_fade_see_through). Others still see it; it still protects you.'),
        ('helmet_window_cm', "helmet_view 3: helmet parts closer than this to your eyes aren't drawn (cm; 5-9). Food at your mouth that close is cut too."),
        ('helmet_fade_see_through', 'helmet_view 5: how see-through your helmet is (0 = solid, 0.5 = half, 1 = invisible).'),
        ('helmet_fade_always', 'helmet_view 5 fades only pieces in front of your eyes (visors, barbutes, armets, closed sallets); hats and caps stay solid. Mesh names to fade anyway, comma separated (the log names them).'),
        ('helmet_fade_never', 'helmet_view 5: mesh names never to fade, comma separated.'),
        ('helmet_fade_tint', "helmet_view 5: the faded helmet's metal colour, r,g,b (0-1)."),
        ('helmet_fade_metallic', 'helmet_view 5: how metallic the faded helmet looks (0-1).'),
        ('helmet_fade_roughness', 'helmet_view 5: how rough/dull the faded helmet looks (0 = mirror, 1 = matte).'),
        ('helmet_fade_material', "helmet_view 5: the translucent material used (the game's own Datasmith glass material)."),
        ('helmet_see_through', 'helmet_view 4 (experiment, does NOT work yet: test 2026-11-20 found no material input that makes the helmet see-through): how much you see through, 0..1.'),
        ('helmet_see_through_mode', 'helmet_view 4: 0 = screen door through the helmet material, 1 = helmet hidden on alternate frames.'),
        ('helmet_see_through_param', "helmet_view 4 mode 0: the helmet material's texture parameter we replace (test)."),
        ('helmet_see_through_test', 'helmet_view 4 mode 0: 0 = dither, 1 = all white, 2 = all black (finding what drives the opacity mask).'),
    ]),
    ('Menus, panel and keys', 'Key codes are Windows virtual keys: 36 Home (recenter), 35 End (panel), 45 Insert (view), 34 Page Down (mode).', [
        ('ui_panel', 'Show menus/HUD on a floating panel.'),
        ('ui_panel_auto', '1 = panel only while a menu is up.'),
        ('ui_distance_m', 'Menu panel distance in front of you (m).'),
        ('ui_width_m', 'Menu panel width (m).'),
        ('ui_height_offset_m', 'Menu panel height offset from your eyes (m; negative = lower).'),
        ('ui_opacity_gain', 'How solid menus and HUD on the panel are drawn (opacity = brightness x this).'),
        ('ui_menu_backdrop', 'Dark sheet behind menus (0-1).'),
        ('ui_use_alpha', "1 = also use the window's alpha for the panel (only 4 levels: shows rings). Off by default."),
        ('ui_pointer', 'Point a controller at the panel to move the mouse; trigger clicks.'),
        ('menu_buttons', 'Left menu = Escape; right A held after a won fight = continue.'),
        ('menu_continue_hold_s', "Hold right A this long after a won fight = the game's 'Hold G to continue' (s)."),
        ('wipe_standins', "1 = wipe the frame-generation stand-in pictures so closed menus and old mouse pointers don't stay on the panel."),
        ('recenter_key', 'Keyboard key that recentres the view (Windows key code; 36 = Home).'),
        ('toggle_ui_key', 'Key that shows/hides the menu panel (35 = End).'),
        ('toggle_view_key', "Key that switches between his head view and the game's own camera (45 = Insert)."),
        ('toggle_mode_key', 'Key that flips between full-resolution mode and the fallback (34 = Page Down).'),
    ]),
    ('Sticks, turning and walking', '', [
        ('pose_stick_turn', 'Right stick turning: smooth, snap or off.'),
        ('pose_stick_turn_dps', 'Smooth turn speed (degrees per second).'),
        ('pose_snap_turn_deg', 'Snap turn step (degrees).'),
        ('pose_move_relative', 'Left stick walks toward where you look (head) or where your body faces (body).'),
        ('pose_stick_dead_zone', 'Stick dead zone (0-1).'),
        ('pose_stick_walk', '1 = the left stick walks.'),
        ('pose_body_turn', '1 = his body turns to face where you look.'),
        ('pose_mouse_off_with_controllers', '1 = with controllers tracked, the mouse does not turn or tilt him.'),
        ('pose_turn_dead_zone_deg', '...once you look this far away.'),
        ('pose_turn_speed_dps', 'How fast his body turns to catch up with your head (degrees per second).'),
        ('pose_turn_max_deg', 'Past this the hips come round at once.'),
        ('pose_turn_spawn_guard', '1 = after a respawn the new body keeps its own facing (0 = the yaw of the last body can be written onto it: helmet and body off-centre).'),
        ('pose_calib_body_facing', '1 = at spawn your head and body are measured against the way the body really faces.'),
    ]),
    ('Crouch, kick, jump, slow motion', '', [
        ('pose_crouch_button_cm', 'Left stick click toggles a crouch this deep (cm; 0 = off).'),
        ('pose_crouch_button_cms', 'How fast that crouch goes down and up (cm/s).'),
        ('pose_crouch', '1 = he crouches as low as you do.'),
        ('pose_crouch_dead_zone_cm', 'Headset drop below standing that is ignored before he crouches (cm).'),
        ('pose_crouch_max_cm', 'How far his legs crouch him (cm); deeper, he bends forward so his hands still reach your floor.'),
        ('pose_crouch_bend_for_depth', '1 = deeper than his legs crouch, he bends forward so his hands reach as low as yours.'),
        ('pose_crouch_keep_leg_ik', "1 = crouching keeps his legs on the leg IK (the game's kneel poses sank him 50-100 cm)."),
        ('pose_player_no_kneel', "1 = the game's kneel never takes your legs."),
        ('kick_on_a', '1 = a short tap of right A kicks.'),
        ('kick_tap_s', 'A press shorter than this (s) is a kick tap.'),
        ('jump_on_x', "1 = left X is the game's jump: hold, then let go to leap (a quick tap is the game's tap-kick)."),
        ('slowmo_on_right_stick_click', "1 = right stick click toggles the game's slow motion."),
    ]),
    ('Body lean and torso', 'His torso follows your head.', [
        ('pose_lean', '1 = his torso leans to put his head where yours is.'),
        ('pose_lean_head_angle', "1 = he also bends forward/sideways when you tilt your head (pitch/roll); 0 = he only leans to where your neck is (closed PR #8/#10's way)."),
        ('pose_lean_look_down_back_cm', 'Looking down, his chest leans back out of your view by up to this many cm (0 = off).'),
        ('pose_lean_pitch_gain', "pose_lean_head_angle 1: how much of your head's downward pitch the torso takes."),
        ('pose_lean_step_radius_cm', 'Lean first, step after: how far your head can move from his hips (cm) before he steps (0 = always step).'),
        ('pose_lean_settle_s', 'Hold your head still this long (s) away from his hips and he steps under you (0 = stays leaned).'),
        ('pose_lean_pitch_dead_deg', 'Head pitch below the horizon ignored before he bends forward (degrees).'),
        ('pose_lean_max_deg', "Most forward bend (degrees; the game's own lean reaches 60)."),
        ('pose_lean_roll_dead_deg', 'Head roll ignored before he bends sideways (degrees).'),
        ('pose_lean_roll_gain', "How much of your head's roll his torso takes."),
        ('pose_lean_side_max_deg', 'Most sideways bend (degrees).'),
        ('pose_lean_smooth_s', 'Smoothing of the lean (s; 0 = none).'),
        ('pose_lean_slack_cm', "Neck slack allowed on top of your headset's drop when he bends (cm)."),
        ('pose_lean_settle_speed_cms', 'Your neck moving slower than this (cm/s) for pose_lean_settle_s counts as holding still.'),
        ('pose_lean_back_radius_cm', 'He leans back at most this far (cm); further back his hips follow.'),
        ('pose_lean_walk_catchup_s', 'Once your neck passes the step radius he steps: hips back under you over this long (s).'),
        ('pose_room_neck_base_cm', "Leans and steps are measured this far below your head joint (base of the neck), so nodding or tilting your head doesn't move him (0 = at the head joint)."),
        ('pose_lean_follow_past_radius', '1 = past the step radius his hips follow your neck at once, so it never gets further than the radius; 0 = he steps all the way under you.'),
        ('pose_lean_follow_speed_cms', 'pose_lean_follow_past_radius: fastest his hips follow (cm/s).'),
        ('pose_lean_cancel_game_bend', "1 = the game's own forward bend (crouching, reaching for grabs) is removed, so only your lean bends him."),
        ('pose_torso_twist', "Share of your head's turn the spine and neck take."),
    ]),
    ('Body posing, feet and steps', 'Your character posed from the headset and controllers. Mostly internals.', [
        ('pose_body', 'Master switch for the posed body.'),
        ('pose_head', '1 = his head turns with your headset.'),
        ('pose_head_lock', '1 = head held at the headset so the helmet stays still in view.'),
        ('pose_head_lock_max_cm', 'Head lock: most his head moves from where the animation has it (cm; more would stretch the neck).'),
        ('pose_arms', '1 = his arms reach for your controllers.'),
        ('pose_follow_room', '1 = walking around your room moves him (and his capsule) with you.'),
        ('pose_hold_capsule', "1 = his capsule only moves when you walk (the game's own pulls, stumbles and dodges are undone)."),
        ('pose_feet', '1 = feet stay planted and step.'),
        ('pose_feet_free_on_kick', "1 = the game's kick moves the leg freely; the foot is planted again where it lands."),
        ('pose_feet_kick_free_s', 'Feet stay free this long after a kick fires (s).'),
        ('pose_step_distance_cm', 'A foot steps once it is this far from where it should be (cm).'),
        ('pose_step_angle_deg', 'A foot steps once it is turned this far from where it should be (degrees).'),
        ('pose_step_time_s', 'How long a step takes standing (s; shorter when moving).'),
        ('pose_step_lift_cm', 'How high the foot lifts in a step (cm).'),
        ('pose_step_lead_s', 'How far ahead a walking foot lands (seconds of movement).'),
        ('pose_no_uro', '1 = his body, head and worn armour animate every frame (no update-rate skipping).'),
        ('pose_helmet_late', "1 = helmet and head moved to this frame's headset pose just before drawing (stops the helmet sliding in front of your eyes)."),
        ('pose_mesh_after_driver', '1 = his visible body always updates after his posed skeleton.'),
        ('pose_in_anim_update', "1 = body pose written inside his animation update (else the drawn hand showed last frame's solve)."),
        ('pose_post_physics', '1 = his skeleton and body tick after physics, so a hand on a held item sits where the item is drawn.'),
        ('pose_actor_z_smooth_s', "Smoothing of the capsule's height for his body and the view (s; walking hops ~2 cm)."),
        ('pose_move_bodies', '1 = his body moves each frame without teleporting its physics bodies, so held weapons follow smoothly.'),
        ('pose_teleport_above_cm', 'A jump bigger than this per frame still teleports (cm).'),
        ('pose_capsule_floor_clamp', 'Old floor clamp; only used when pose_capsule_keep_on_floor=0.'),
        ('pose_capsule_keep_on_floor', '1 = his capsule never sinks more than 15 cm under standing height (stops the sink-into-the-floor collapse).'),
        ('pose_capsule_no_step_offset', "1 = the game's per-step capsule dip is taken out (walking no longer bobs the view ~2 cm twice a second)."),
    ]),
    ('Your damage and injuries', 'What hurts you and what injuries do.', [
        ('player_takes_damage', '1 = hits on your body hurt you as in the flat game (health, consciousness, bleeding).'),
        ('player_body_contact_speed', '1 = bumping into people counts by how fast your body moves into them, not how deep it sinks in (walking into someone no longer hurts him).'),
        ('player_body_contact_mass_scale', "Your body part's mass in those bumps is x this (1 = its own mass; higher = harder bumps and kicks)."),
        ('player_body_contact_log', '1 = log each changed bump ("Body contact:" lines).'),
        ('player_item_contact_speed', '1 = an item pinned or squeezed against you hurts only by how fast it comes into you (real blows unchanged).'),
        ('player_stump_blood', '1 = blood from your own cut-off limb stays at the stump (not floating where the limb was).'),
        ('injury_limbs', '1 = your bad injuries cripple you: a limb below injury_limb_health, broken, dislocated or cut off.'),
        ('injury_limb_health', "Limb health below this counts as badly hurt (25 = the game's own bone-break threshold; a bone put back gets 25)."),
        ('injury_one_leg_speed', 'One bad leg: walk at this share of normal speed.'),
        ('injury_two_legs_fall', '1 = both legs bad: no walking and you fall over until a leg recovers.'),
        ('injury_back_broken_falls', "1 = a broken back counts as both legs (the game drops both legs' strength)."),
        ('injury_dislocated_follow_s', 'Dislocated (not broken) arm: how far it lags your controller (s).'),
        ('injury_dislocated_sag_cm', 'Dislocated arm: how much it sags below your controller (cm).'),
        ('injury_swing_relocate', "1 = swing your controller hard (the game's swing speed over 60) to put a broken arm back, the game's own rule."),
        ('death_ragdoll', '1 = when you die he ragdolls and falls; the view stays on his head.'),
    ]),
    ('Hands on the controllers and arms', 'Where his hand sits on your controller, arm length, shoulder reach.', [
        ('pose_wrist_forward_cm', 'Hand placement on the controller: wrist forward (cm, controller frame).'),
        ('pose_wrist_right_cm', '...toward the back of the hand (cm).'),
        ('pose_wrist_up_cm', '...up (cm).'),
        ('pose_hand_pitch', 'Hand turn relative to the controller, pitch (degrees).'),
        ('pose_hand_yaw', 'Hand turn relative to the controller, yaw (degrees).'),
        ('pose_hand_roll', 'Hand turn relative to the controller, roll (degrees).'),
        ('pose_knuckle_deg', 'Fingers tilted this much from the handle (degrees).'),
        ('pose_user_arm_cm', 'Your arm length (shoulder to wrist, cm). Measured by holding both arms out and holding B+Y 2 s; 0 = not measured.'),
        ('pose_forearm_twist_share', "Share of the hand's twist the forearm takes (0-1); the rest shows at the wrist."),
        ('pose_swivel_smooth_s', 'Elbow swing smoothing (s).'),
        ('pose_arm_solve_drawn_hand', '1 = the elbow is chosen for his drawn hand (on a stopped weapon), as B&S does.'),
        ('pose_shoulder_reach_cm', 'Shoulder comes forward this much at full reach (0 = off).'),
        ('pose_shoulder_reach_start_cm', "Shoulder reach starts in the last this many cm of his arm's reach."),
        ('pose_shoulder_reach_max_deg', 'Most collarbone turn for shoulder reach (degrees).'),
        ('hand_bone_left', "His left hand bone's name."),
        ('hand_bone_right', "His right hand bone's name."),
    ]),
    ('Fingers and grip wrap', 'Finger tracking and how fingers close around what you hold.', [
        ('input_hand_tracking', "Read SteamVR's finger tracking when offered."),
        ('pose_fingers_tracked', '1 = free fingers follow yours.'),
        ('pose_fingers_tau_s', 'Smoothing of the tracked fingers (s).'),
        ('pose_fingers_per_joint', '1 = with finger tracking, each finger joint follows your own.'),
        ('pose_finger_dip_coupling', "End joint = this x the middle joint's bend (0 = as tracked)."),
        ('pose_finger_scale_mcp', 'Finger full bend x this at the knuckle.'),
        ('pose_finger_scale_pip', '... at the middle joint.'),
        ('pose_finger_scale_dip', '... at the end joint.'),
        ('pose_hand_world_stop', '1 = an empty hand stops at walls and scenery instead of going into them.'),
        ('pose_hand_world_radius_cm', 'pose_hand_world_stop: radius of the sphere at the middle of the palm that stops on walls (cm).'),
        ('pose_hand_world_reach_cm', 'pose_hand_world_stop: that sphere sits this far past the wrist along the fingers (cm).'),
        ('pose_arm_fresh_shoulder', '1 = an empty hand does not bob with your steps while walking (its arm is re-aimed from this frame\'s shoulder after his body is evaluated).'),
        ('pose_arm_fresh_shoulder_held', "pose_arm_fresh_shoulder for a hand holding a weapon too (two hands, scabbard): 0 = empty hands only."),
        ('pose_no_fall_arms', "1 = walking fast doesn't swap his arms into the game's falling-backwards swing (that opened the weapon hand)."),
        ('pose_body_wall_stop', '1 = you stop at walls, posts and scenery instead of walking through them (stick and room walking).'),
        ('pose_body_wall_step_cm', 'pose_body_wall_stop: things lower than this above your feet (steps, the floor) do not stop you (cm).'),
        ('pose_body_wall_radius_share', "pose_body_wall_stop: how wide you are, as a share of the game's capsule (1 = the capsule)."),
        ('pose_finger_pivot_fix', "1 = finger joints bend at the finger's middle (0 = about the game's bones: fingers roll into circles)."),
        ('pose_finger_schedule', "1 = finger joints close in a natural order (no round 'circle' fingers)."),
        ('pose_finger_mcp_lag', 'Finger schedule: the knuckle starts bending at this curl (0-1).'),
        ('pose_finger_pip_full', 'Finger schedule: the middle joint is fully bent at this curl (0-1).'),
        ('pose_wrap', '1 = fingers wrap bodies and loose objects you grab (procedural grab).'),
        ('pose_wrap_ease_s', 'Grip wrap: fingers ease in/out over this long (s).'),
        ('pose_wrap_refresh_s', 'Grip wrap: how often all five fingers are solved again (s).'),
        ('pose_wrap_finger_radius_cm', '0 = the measured finger shape (set a number for one plain radius).'),
        ('pose_wrap_all_bodies', '1 = on a Willie, fingers feel all of him (jaw, collarbones), not only the part you grabbed.'),
        ('pose_wrap_untouched_curl', 'A finger that misses what you hold curls like its neighbour, else at most this (0-1).'),
        ('pose_wrap_body_pad', "1 = fingers stop at a Willie's skin where it stands outside his collision (neck, collarbones, upper back)."),
        ('pose_wrap_gauntlet_pad_cm', 'Extra palm-side finger thickness with full gauntlets or gloves on.'),
        ('pose_wrap_halfgauntlet_pad_cm', 'The same for half gauntlets (first finger segment only).'),
        ('pose_wrap_open_base', "1 = a wrapping hand starts from the open hand (not the game's grab shape)."),
        ('pose_fingers_anim_only', "1 = finger shapes go only to the animation's copy of the grip type, so the game's two-hand rules never see them."),
    ]),
    ('Grabbing', 'Picking things up: reach, handles, how an item seats in the hand.', [
        ('pose_grab_button', '1 = squeeze grip near a loose weapon to pick it up.'),
        ('pose_grab_release_drops', '1 = letting go of grip drops what that hand holds.'),
        ('pose_grab_objects', "1 = squeezing near a loose object or a body grabs it (the game's own grab)."),
        ('pose_grab_world', '1 = fixed scenery (fence, table, wall) can be held.'),
        ('pose_grab_radius_cm', 'Old grab: search radius around the hand (cm; used when pose_grab_handles=0).'),
        ('pose_grab_touch_cm', 'How far from an item your palm can be and still grab it (cm).'),
        ('pose_grab_handles', '1 = B&S-style handle grabbing.'),
        ('pose_grab_handle_cm', 'A grab lands on the nearest handle within this distance (cm).'),
        ('pose_grab_weapon_handles_only', '1 = a weapon is taken only by a handle (grip, haft, blade), not anywhere you touch it.'),
        ('pose_grab_settle_s', 'Grabbed item eases into the hand over this long (s).'),
        ('pose_grab_seat', "1 = a weapon taken by grip or blade sits at the game's own seat in the hand."),
        ('pose_grab_seat_reverse_bias_deg', 'A reversed seat must be this much nearer to win (degrees).'),
        ('pose_grab_seat_palm', '1 = what you grab sits on your palm, not inside the fist.'),
        ('pose_grab_palm_cm', 'Palm point: this far from the hand bone toward the palm (cm).'),
        ('pose_grab_scenery_gap_cm', 'On scenery the palm stays this far off the surface (cm; the hand sat half in the fence).'),
        ('pose_grab_palm_along', 'Palm point: this share of the way from the hand bone to the middle knuckle.'),
        ('pose_grab_from_controller', "1 = when his arm can't reach your hand (floor), a squeeze grabs at your controller and pulls it to him."),
        ('pose_grab_controller_gap_cm', 'His hand this far short of yours (cm) before grabbing at the controller.'),
        ('pose_grab_controller_max_cm', 'Never grab at the controller from farther than this (cm).'),
        ('pose_weapon_free_grip', '1 = a weapon taken away from its grip is held where the hand closed. 0 = snapped onto the grip.'),
        ('pose_weapon_free_cm', 'Farther than this from the grip (cm) counts as away from it.'),
        ('pose_grab_drive', '1 = the mod holds grabbed objects/bodies to your palm (B&S-style).'),
        ('pose_grab_drive_heft', "Loose objects: how much an object's weight slows its pull into the hand."),
        ('pose_grab_drive_accel', 'Loose objects: most acceleration pulling one into the hand (m/s^2, divided by its heft).'),
        ('pose_grab_drive_ang_accel', 'Loose objects: most turning acceleration pulling one into the hand.'),
        ('pose_grab_body_pull_n', 'Held body part pulled to your palm with up to this force (N, x his grip strength).'),
        ('pose_grab_body_pull_hz', 'Stiffness of that pull (Hz).'),
        ('pose_grab_body_pull_slack_cm', 'Hand this far from a held body before it pulls (stops instant chokes).'),
        ('pose_grab_body_pull_min_kg', 'The body pull pulls as if the held part weighed at least this (kg): a neck moves his whole upper body.'),
        ('pose_grab_ride', '1 = your hand stays on a grabbed NPC (neck, arm) instead of floating off it.'),
        ('pose_grab_ride_objects', '1 = loose objects also hold his hand on them (bodies always do).'),
        ('pose_grab_ride_from_cm', 'His hand stays on a held thing from this far off your hand (cm)...'),
        ('pose_grab_ride_full_cm', '...fully at this far (cm).'),
        ('pose_grab_ride_max_cm', 'Your hand pulled this far off a held thing (cm) for pose_grab_slip_s lets go.'),
        ('pose_grab_slip_s', 'Seconds past pose_grab_ride_max_cm before the grip slips.'),
    ]),
    ('Sliding the hand along a handle', 'Hold trigger to slide; fixed grips for swords and daggers.', [
        ('pose_slide', 'Hold trigger to slide the hand along a handle.'),
        ('pose_slide_hand_half_cm', "Half your hand's width along a handle (cm), for where the hand stops."),
        ('pose_slide_rate_min', 'Slide speed at a light trigger (how fast the hand catches up).'),
        ('pose_slide_rate_max', 'Slide speed at a full trigger.'),
        ('pose_slide_max_cm_s', 'Fastest the hand runs along a weapon (cm/s).'),
        ('pose_slide_keep_light', "Share of the item's own motion along the handle kept at a light trigger (only if the dampers are 0)."),
        ('pose_slide_keep_full', 'Same at a full trigger.'),
        ('pose_slide_damper_light', 'B&S slide damper at a light trigger (N s/m; lower = slides more freely).'),
        ('pose_slide_damper_full', 'B&S slide damper at a full trigger (N s/m).'),
        ('pose_slide_bs_damper', 'B&S slide: gravity pulls an upright weapon through a loose hand, a level one follows the hand (0 = old free slide)'),
        ('pose_slide_zone_measured', "1 = the slide zone is measured from the weapon's real parts (stops at guards, heads, wide pommels)."),
        ('pose_slide_wide_cm', 'A part wider than this from the axis stops the hand (cm).'),
        ('pose_slide_ring_max_len_cm', 'A wide bit on a haft this short or shorter is slid over (cm; 0 = every wide bit stops).'),
        ('pose_slide_ring_max_width_cm', '...if it reaches no farther than this from the axis (cm).'),
        ('pose_slide_gap_max_cm', 'A gap between parts this short or shorter is slid over (cm).'),
        ('pose_slide_rondel_stops', '1 = a rondel (guard part on a haft) stops the hand.'),
        ('pose_slide_butt_is_haft', "1 = parts below the grip (a pollaxe's butt) count as haft to slide on."),
        ('pose_slide_fixed_1h_sword', '1 = one-handed swords are held at one fixed spot on the grip (no slide).'),
        ('pose_grip_two_zones_2h_sword', 'Two fixed grip zones (under guard, above pommel): 0 off, 1 bastard swords only, 2 all two-handed swords.'),
        ('pose_grip_pommel_top_cm', 'The lower hand on a two-zone sword stays this far above the centre of the pommel (cm), so it never grabs the pommel.'),
        ('pose_grip_dagger_fixed', '1 = daggers have one fixed grip spot (no slide).'),
    ]),
    ('Two hands on one weapon', '', [
        ('pose_two_hand', '1 = squeeze the free hand on a weapon the other hand holds to hold it with both (grip or blade).'),
        ('pose_two_hand_strength', 'Two hands on one weapon hold this many times as strong.'),
        ('pose_two_hand_twist_only', '1 = with two hands the turning hand only rolls the weapon; your two hands point it.'),
        ('pose_two_hand_joint_both_turn', 'Engine-joint hold, both hands on fixed grips (crossbow, sword grip zones): 1 = both wrists turn the weapon, each toward its own hand; 0 = the first hand only rolls it (test switch).'),
        ('pose_two_hand_swing_share', '0..1: how much the turning hand still points a two-handed weapon (1 = old).'),
        ('pose_two_hand_blade_gap_cm', 'The blade grip zone starts this far past the guard (cm).'),
        ('pose_two_hand_reach_cm', 'How far from a grip zone a squeeze still lands on it (cm).'),
        ('pose_two_hand_let_go_cm', 'A controller pulled this far off its spot lets go (cm).'),
        ('pose_two_hand_curl', "1 = the second hand's fingers close with the sword grip type (0 keeps the game's two-hand rules working). Only with pose_fingers_anim_only=0."),
        ('pose_two_hand_lock_rotation', '1 = the second hand turns with the weapon, not with your controller.'),
        ('pose_two_hand_mirror', 'Which weapon axis the second hand mirrors across (0 = X, 1 = Y).'),
    ]),
    ('Holding: weight, arm lag, wrist and shoulder', 'How a held weapon follows your hand.', [
        ('pose_hold_arm_lag', "1 = held weapons trail your hand like flat Half Sword's arm (heavier = more lag)."),
        ('pose_hold_arm_lag_s', 'Empty-arm follow delay (s); weapons add to it by their weight. Flat game ~0.1.'),
        ('pose_hold_arm_lag_max_s', 'Most lag any weapon gets (s).'),
        ('pose_hold_heft_lag_s', 'Heft floor: a weapon lags at least this x sqrt(its inertia about the shoulder) (s), so a greatsword never swings like a dagger.'),
        ('pose_hold_strength', 'Multiplier on the whole hold (force and turning).'),
        ('pose_hold_max_force', 'Most force one hand puts on a held item (N, x his strength).'),
        ('pose_hold_max_torque', 'Wrist strength: most turning force one hand has at the grip (N m, x his strength).'),
        ('pose_hold_wrist_stop_torque', 'Wrist end stop: turning limit once the item is bent past the free angle (N m).'),
        ('pose_hold_shoulder_torque', 'Shoulder strength against swinging arm + weapon (N m; 0 = off). Lower = heavy weapons trail more.'),
        ('pose_grip_hold', "1 = the mod's own hold (the game's hand joint goes free). 0 = the old grip spring section below."),
        ('pose_hold_exact', '1 = exact hold: the weapon goes exactly where the hand is, limited by hand strength. 0 = the springs below.'),
        ('pose_hold_bs', '1 = Blade & Sorcery style spring hold.'),
        ('pose_hold_spring', 'One-hand hold spring (N/m).'),
        ('pose_hold_damper', 'One-hand hold damper (N s/m).'),
        ('pose_hold_turn_spring', 'One-hand turning spring (N m/rad).'),
        ('pose_hold_turn_damper', 'One-hand turning damper (N m s/rad).'),
        ('pose_hold_damping_ratio', 'Dampers per weapon for this damping ratio (0 = fixed dampers).'),
        ('pose_hold_target_vel_smooth_s', "Smoothing of your hand's speed fed to the hold (s; stops ~1 cm tracking shake)."),
        ('pose_hold_wrist_free_deg', 'Wrist bends freely up to this angle...'),
        ('pose_hold_wrist_stop_deg', '...and reaches the full end stop here.'),
        ('pose_hold_wrist_stop_spin', "Wrist stop pushes back fully from this spin (rad/s), so a weapon's weight still sags it slowly."),
        ('pose_hold_torque_strength_power', 'His strength on wrist turning is raised to this power (a wrist is weaker in proportion).'),
        ('pose_hold_arm_reach_cm', 'Grip distance from the shoulder for a default body (cm, scaled with his size).'),
        ('pose_hold_shoulder_fade_from_cm', 'Shoulder limit starts fading once the grip trails your hand this far (cm)...'),
        ('pose_hold_shoulder_fade_to_cm', '...and is gone at this far (cm).'),
        ('pose_hold_let_go_reach_slack_cm', 'With arm lag: how far past his reach the grip may trail (cm).'),
        ('pose_hold_weight', '1 = heavy, long weapons trail fast turns (turn limited by torque about the grip).'),
        ('pose_hold_weight_torque_scale', 'Multiplier on pose_hold_max_torque for the weight rule.'),
        ('pose_hold_weight_two_hand_couple', '1 = two hands pushing opposite ways add to the turning strength.'),
        ('pose_hold_weight_spin_extra', "A weapon's own weight never spins it faster than wanted plus this (rad/s)."),
        ('pose_bs_two_pos_k', 'Two-hand hold springs (B&S).'),
        ('pose_bs_two_pos_c', "B&S two-hand hold: each hand's pull damper."),
        ('pose_bs_two_pos_max', "B&S two-hand hold: each hand's most pull force."),
        ('pose_bs_two_rot_k', "B&S two-hand hold: the main hand's turning spring."),
        ('pose_bs_two_rot_c', "B&S two-hand hold: the main hand's turning damper."),
        ('pose_bs_two_rot_max', "B&S two-hand hold: the main hand's most turning force."),
        ('pose_bs_max_spin', 'B&S hold: fastest a held weapon may spin (rad/s).'),
        ('pose_bs_iterations', 'B&S hold: solver passes per frame.'),
        ('pose_bs_two_let_go_cm', 'B&S hold: a second hand this far off its spot lets go (cm).'),
        ('pose_hold_hand_on_weapon', '1 = his drawn hand stays on a held weapon that is stopped by something.'),
        ('pose_hold_blocked_from_cm', 'The hand is fully on the stopped weapon once it is this far behind (cm).'),
        ('pose_hold_hand_follows', '1 = his drawn hand rides the held item.'),
        ('pose_hold_hand_follows_predict', "How the drawn hand predicts the item: 1 = from last frame's motion, 2 = physics velocity, 0 = none."),
        ('pose_hold_follow_predict_max_cm', 'Most that prediction looks ahead (cm).'),
        ('pose_hold_follow_predict_smooth_s', 'Smoothing of the measured motion for the prediction (s).'),
        ('pose_hold_let_go', 'Stuck rule: drop a weapon stuck out of reach.'),
        ('pose_hold_let_go_s', 'A stuck weapon out of his reach this long (s) is let go.'),
        ('pose_hold_let_go_grace_s', 'No let-go for this long after a weapon comes into the hand (s).'),
        ('pose_hold_let_go_dead', '1 = a stuck weapon is let go with no speed (never flies off).'),
        ('pose_hold_measured_v', '1 = a stopped weapon is driven from how it really moves (no wind-up).'),
        ('pose_hold_joint', '1 = weapons held by engine joints (B&S style); 0 = the old hold. Drawing, wounded arms and fists still use the old hold.'),
        ('pose_hold_joint_stiffness', 'Engine-joint hold: multiplier on the springs.'),
        ('pose_hold_joint_damping', 'Engine-joint hold: multiplier on the dampers.'),
        ('pose_hold_joint_max', 'Engine-joint hold: multiplier on the force and torque caps.'),
        ('pose_hold_joint_shoulder', 'Engine-joint hold: 1 = the grip force is capped by your arm strength from the shoulder (the old arm-lag model); 0 = B&S fixed cap.'),
        ('pose_hold_joint_carry_weight', "Engine-joint hold: 1 = your hand carries the weapon's weight at the grip (split between two hands by leverage), so it doesn't sag."),
        ('pose_hold_joint_fade_min_speed', "Engine-joint hold: your hand must move at least this fast (cm/s) before a grip far behind eases toward B&S's cap (stops pressing a propped weapon)."),
        ('pose_hold_joint_fade_keep_s', "Engine-joint hold: once the hand (or the weapon against it) moves that fast, the ease to B&S's cap stays allowed this long (s), so it can brake what it started."),
        ('pose_hold_joint_fade_after_s', "Engine-joint hold: no ease to B&S's cap this soon after a joint is made (s)."),
        ('pose_hold_joint_slerp', 'Engine-joint hold: 1 = turns the weapon with one SLERP drive (handles flips and end-for-end grabs); 0 = twist and swing drives.'),
        ('pose_hold_joint_mod_turn', "Engine-joint hold, one hand: 1 = the weapon turns as with the old hold (wrist strength and wrist stop), the joint only holds the grip in your hand; 0 = the joint turns it too (weak on weapons with lopsided inertia, e.g. polehammers)."),
        ('pose_hold_joint_turn_by_grip', "Engine-joint hold, one hand: the weapon's turn is no stronger than your hand's pull can keep the grip in place against (1 = long weapons aren't swung out of your hand on a grab)."),
        ('pose_hold_joint_speed_limit', 'Engine-joint hold: 1 = a held weapon never goes faster than 20 m/s or pose_bs_max_spin (a safety net against throws).'),
        ('pose_hold_let_go_ignore_self', '1 = a weapon stopped by his own body is never let go by the stuck rule.'),
        ('pose_hold_contact_cm', 'A held weapon this far short of where the hold sent it counts as blocked (cm).'),
        ('pose_hold_ignore_hand', '1 = the held weapon never collides with the hand holding it.'),
        ('pose_hold_ignore_body', '1 = the held weapon never collides with his own body (B&S).'),
        ('pose_hold_ignore_own_helmet', "1 = your held weapon also passes through your own worn helmet's collision. Others' helmets are still hit."),
        ('pose_hold_block_offhand_search', "1 = the game's off-hand search grip stays off while the mod holds a weapon (it shoved longswords)."),
    ]),
    ('Stamina and strength', '', [
        ('pose_stamina_scale', 'Stamina cost of holding and swinging (1 = exactly the flat game, which barely drains; 4 = chosen 2026-11-21).'),
        ('pose_body_strength', '1 = his strength, injuries and stamina scale the hold.'),
        ('pose_body_stamina_floor', 'At 0 stamina, this share of his strength is left.'),
        ('pose_body_strength_min', 'His hold strength never drops below this share.'),
        ('pose_body_size_strength', '1 = bigger Willies hold stronger (arm mass vs a default body).'),
        ('pose_stamina_gate', 'Hand burn smaller than this per tick is ignored (the game uses 10; 0 = all counts).'),
        ('pose_stamina_fist_scale', 'Fist stamina burn x this.'),
        ('pose_no_lawnmower', '1 = no mouse-waggle spam penalty for your VR swings (it multiplied stamina cost up to 10x).'),
        ('pose_stamina_force_cap_n', "Hold force counted for stamina is capped here (N; snags don't drain you)."),
        ('pose_stamina_torque_cap_nm', 'Hold torque counted for stamina is capped here (N m).'),
    ]),
    ('Shields and pavise', '', [
        ('pose_shield_free_grip', '1 = a shield taken away from a strap or handle is held where it touched.'),
        ('pose_shield_point_cm', 'How near a strap or handle the hand must close to take the shield by it (cm).'),
        ('pose_shield_in_hand', '1 = strap shields held in the hand, not on the forearm.'),
        ('pose_shield_hand_grip_type', 'Finger shape (game grip type) for a hand holding a shield (-1 = none).'),
        ('pose_shield_fixed_grip', '1 = shield straps/handles are fixed grips (held at their middle, no trigger slide).'),
        ('pose_shield_grab_rim', '1 = a shield can also be taken by its rim; 0 = straps and handles only.'),
        ('pose_shield_rim_cm', 'How near an outer edge the hand must be for a rim grab (cm).'),
        ('pose_shield_grab_face', '1 = a shield can be taken anywhere, flat faces too (old way).'),
        ('pose_pavise_forearm', '1 = pavises strap onto the forearm (light: either arm; heavy: left arm), not held in the hand.'),
    ]),
    ('Drawing, sheathing, belt', '', [
        ('pose_draw', "1 = squeeze near a sheathed weapon's grip to draw it."),
        ('pose_draw_touch_cm', "A squeeze this near a scabbarded weapon's grip on your belt draws it (cm)."),
        ('pose_draw_zone_cm', 'A squeeze this near any part of a belt weapon without a scabbard, or a shield on your back, takes it off you (cm).'),
        ('pose_draw_settle_s', 'A drawn weapon eases into the hand over this long (s).'),
        ('pose_draw_step_cm', 'Most it moves along the scabbard per tick (cm).'),
        ('pose_draw_force_scale', 'Hold strength while drawing or sheathing (share).'),
        ('pose_hold_hand_follows_draw', '1 = his drawn hand stays on the weapon while drawing/sheathing too.'),
        ('pose_draw_force_scale_follows', 'Hold strength while drawing with the hand on the blade (share).'),
        ('pose_draw_scabbard_free', '1 = the scabbard swings loose while drawing (else locked).'),
        ('draw_seat_ref_fix', "1 = scabbard seat reference set with the engine's real parameter names (changes draw feel; 0 = as before)"),
        ('pose_sheathe', "1 = put a held weapon's tip into its belt scabbard's mouth to sheathe it."),
        ('pose_sheathe_mouth_cm', 'Tip within this of the scabbard mouth (cm).'),
        ('pose_sheathe_angle_deg', "Blade within this angle of the scabbard's line (degrees)."),
        ('pose_sheathe_cross', '1 = a weapon whose scabbard is on the other hip goes back across.'),
        ('pose_scabbard_grab', '1 = the off hand can take a belt scabbard and angle it.'),
        ('pose_scabbard_handle', '1 = a belt scabbard is a handle (taken along its length, trigger slides).'),
        ('pose_scabbard_touch_cm', 'Squeeze within this of the scabbard to take it (cm).'),
        ('pose_scabbard_angle_deg', 'Most the scabbard turns on the belt (degrees).'),
        ('pose_scabbard_follow_s', "How fast the scabbard catches up with your hand's angle (s)."),
        ('pose_scabbard_let_go_cm', 'Hand pulled this far off the scabbard for 0.3 s lets go (cm).'),
        ('pose_scabbard_grip_type', 'Finger shape (game grip type) for a hand holding a scabbard.'),
        ('pose_scabbard_block_auto', "1 = the game's own teleporting off-hand scabbard grab can't fire."),
        ('pose_holster_snap', 'Holster snap for weapons without a scabbard.'),
        ('pose_holster_zone_cm', 'Let go of a scabbard-less weapon within this of its free belt slot (cm) to hang it there.'),
        ('pose_holster_max_speed', '...moving slower than this (m/s; faster is a throw).'),
        ('pose_keep_scabbard_on_drop', '1 = a drawn weapon dropped or thrown leaves its scabbard on the belt.'),
    ]),
    ('Crossbow', 'Two fixed grips; a free hand pulls the string back to cock it; a bolt from the hip quiver goes into the cradle; the trigger fires.', [
        ('crossbow', '1 = crossbow handling on.'),
        ('crossbow_string_reach_cm', 'How near the string your hand must close to take it (cm).'),
        ('crossbow_string_margin_cm', '...and at least this much nearer the string than a grip (the front grip is 6 cm from it).'),
        ('crossbow_quiver_reach_cm', 'How near the quiver your hand must close to take a bolt (cm).'),
        ('crossbow_bolt_seat_cm', "A bolt this near the cradle is loaded (cm; the game's own is 10)."),
        ('crossbow_trigger_fire', 'Trigger pull that fires (0-1).'),
        ('crossbow_trigger_release', 'Trigger must come back under this before it fires again.'),
        ('crossbow_trigger_grip_cm', 'Only the hand on the rear grip fires: within this of it (cm).'),
    ]),
    ('Swinging, throwing, whoosh', '', [
        ('pose_throw_multiplier', 'Thrown weapon speed x this (B&S 2).'),
        ('pose_throw', '1 = letting go of a weapon fast enough throws it (its hits count).'),
        ('pose_throw_speed', 'Let go faster than this (m/s) = a throw.'),
        ('whoosh_vr_threshold_scale', "Swing whoosh in VR: the game's whoosh speed threshold x this (1 = game's own, needs 7.5 m/s swings; lower = slower swings whoosh)."),
        ('whoosh_walk_speed_scale', 'Whoosh threshold raised by your walking speed x this, so walking with a weapon does not whoosh (0 = off).'),
        ('pose_feed_swing_speed', "1 = the game's swing speed is fed from your controllers."),
        ('pose_swing_speed_lo', 'Hand speed that counts as no swing (m/s).'),
        ('pose_swing_speed_hi', 'Hand speed that counts as a full swing (m/s).'),
        ('pose_swing_speed_fall_s', 'How fast the swing speed falls off (s).'),
    ]),
    ('Stabbing, cutting, hits', '', [
        ('stab_assist', "1 = a thrust gets his arm's mass behind it so the game's stab rule can pass."),
        ('stab_min_speed_cms', 'Thrust speed needed for stab assist (cm/s).'),
        ('stab_min_into_cos', 'How straight into him the tip must point (cosine; 1 = dead straight).'),
        ('stab_arm_mass_kg', 'Arm mass put behind a stab (kg).'),
        ('stab_push_force_n', 'Or: keep pushing in with at least this force (N)...'),
        ('stab_push_time_s', '...for this long (s).'),
        ('stab_no_rubber_band', "1 = a held weapon is never left 'rubber banded' by the game (it can't stick then)."),
        ('stab_pickup_grace_s', "No sticking for this long after a pickup (s; the game's 2 s cut to this; < 0 = game's)."),
        ('stab_wound_hold', '1 = a stuck blade slides in and out of the wound at limited speed.'),
        ('stab_wound_in_cms', 'Fastest the blade goes deeper in a wound (cm/s).'),
        ('stab_wound_out_cms', 'Fastest it comes out (cm/s).'),
        ('stab_wound_head_factor', 'Coming-out speed x this in the head and neck.'),
        ('stab_wound_head_in_factor', 'Going-in speed x this in the head and neck.'),
        ('stab_wound_settle_s', 'No pulling out for this long after it goes in (s).'),
        ('stab_wound_settle_max_s', 'A fresh stick held open until the game confirms it, at most this long (s).'),
        ('stab_wound_entry_cms', 'Speed it is carried in meanwhile (cm/s).'),
        ('stab_start_depth_cm', 'A freshly stuck blade is set this deep (cm; 0 = off).'),
        ('stab_start_depth_per_ms', 'Plus this many cm per cm/s of thrust speed.'),
        ('stab_start_depth_max_cm', 'At most this deep (cm).'),
        ('stab_yank_cm', 'A stuck blade is torn out when your hand is this far from its grip (cm; 0 = never)...'),
        ('stab_yank_s', '...for this long (s).'),
        ('stab_drag', 'A stuck weapon drags the body instead of letting go.'),
        ('stab_drag_force_scale', 'Drag strength x this.'),
        ('stab_drag_grip_s', "(Unused since the arm's-length pull-out.)"),
        ('stab_drag_reach_slack_cm', "A stuck thrust past arm's length by this much (cm)..."),
        ('stab_drag_reach_s', '...for this long (s) comes out of the body (stays in your hand).'),
        ('pose_hold_stuck_force_scale', 'Hold strength while the weapon is stuck in a body (share).'),
        ('hit_assist', "Your arm's mass behind every other hit."),
        ('hit_arm_mass_kg', 'Arm mass behind a hit (kg, x his strength).'),
        ('hit_assist_min_cms', 'Hit assist from this closing speed (cm/s).'),
        ('cut_follow_through', '1 = a lodged cut is let go once your hand carries on past it.'),
        ('cut_release_cm', 'Hand this far past the lodged cut (cm)...'),
        ('cut_release_after_s', '...for this long (s).'),
        ('grip_flip', 'Tap B/Y to flip a one-handed weapon end for end.'),
        ('grip_flip_short_only', '1 = only daggers, short swords and one-handed swords flip.'),
        ('grip_flip_s', 'How long the flip takes (s).'),
        ('grip_flip_tap_s', 'A press shorter than this is a flip tap (s).'),
    ]),
    ('Punches, elbows, grabbing bodies', '', [
        ('pose_fists', '1 = squeezing an empty hand with nothing in reach makes a fist.'),
        ('pose_fist_class', "The game's fist object path."),
        ('pose_fist_mark_spawned', "Test switch: set the game's 'Fist Spawned' flag on the mod's fist."),
        ('pose_fist_from_fingers', 'Fist from your real fingers.'),
        ('pose_fist_index_on', 'Trigger at least this (0-1) to make a fist...'),
        ('pose_fist_index_off', '...and below this the fist opens.'),
        ('pose_fist_grip_on', 'Grip at least this (0-1) to make a fist...'),
        ('pose_fist_grip_off', '...and below this the fist opens.'),
        ('pose_fist_thumb_on', 'Thumb touch needed for a fist (0 = not needed).'),
        ('pose_fist_thumb_off', 'Thumb below this opens the fist (-1 = never).'),
        ('pose_fist_slap', '1 = an open hand swung fast enough slaps (hits like a fist, fingers open).'),
        ('pose_fist_slap_ass', 'Swing speed for a slap (game units; its fist speed is 30).'),
        ('pose_fist_slap_keep_s', 'Slap stays on this long after the hand slows (s).'),
        ('pose_fist_clear', 'No fist while the hand is inside another Willie.'),
        ('pose_fist_clear_margin_cm', 'Extra clearance the fist needs from him (cm).'),
        ('pose_fist_clear_retry_s', 'How often it checks again (s).'),
        ('pose_fist_full_impulse', "1 = a punch's full force reaches his damage (the game cut a sliding fist contact to almost nothing)."),
        ('disarm', "1 = grab a weapon out of an enemy's hand: pull harder than his grip and he lets go."),
        ('disarm_grip_scale', 'His grip against your pull: his grip strength x this (higher = harder to disarm).'),
        ('disarm_hold_s', 'How long your pull must beat his grip before he lets go (seconds).'),
        ('pose_fist_shape', "Fist finger shape: -2 game's own, -1 grab shape, 0-17 grip type, 100 guard fist (1 = tightest)."),
        ('pose_fist_game_flags', "1 = the game's own fist flags and punch stamina work with the mod's fist."),
        ('pose_elbow_strikes', 'Elbows hit like a punch.'),
        ('pose_elbow_radius_cm', 'Elbow hit reach (cm).'),
        ('pose_elbow_min_ass', 'Elbow speed needed (game swing speed units; fist is 30).'),
        ('pose_elbow_learn_min_hits', "After this many fist hits, elbows use your fists' learnt numbers."),
        ('pose_elbow_rigidity', 'Elbow hit rigidity until learnt.'),
        ('pose_elbow_cut', 'Elbow hit cut value until learnt.'),
        ('pose_elbow_impulse_per_v', 'Elbow impulse per velocity until learnt.'),
        ('pose_elbow_velocity_scale', 'Elbow velocity scale until learnt.'),
        ('pose_grab_body_hold', "1 = the game's body-grab joint is made a soft capped spring; 0 = the game's own joint."),
        ('pose_grab_body_spring', 'Body grab spring (with pose_grab_body_hold=1).'),
        ('pose_grab_body_damper', 'Body grab damper.'),
        ('pose_grab_body_cap_per_t', 'Body grab force cap x his grip strength (0 = use pose_grab_body_max).'),
        ('pose_grab_body_max', 'Fixed body grab force cap.'),
        ('pose_grab_body_let_go_cm', "Hand this far from a grabbed body's spot for 0.3 s lets go (cm)."),
        ('pose_grab_body_no_slip', "1 = the game's grip slip is off while you hold a body (your tracked hand tripped it); pulling away lets go instead."),
        ('pose_grab_body_slip_below_t', "Below this grip strength (broken or numb arm) the game's slip still applies."),
    ]),
    ('Hit vibration', 'Controller buzz on hits.', [
        ('hit_buzz', 'Hit vibration strength (0 = off, 1 = like Blade & Sorcery).'),
        ('hit_buzz_scrape', 'Scrape ticks while a blade slides along something (0 = off).'),
        ('hit_buzz_blade_on_plate', '1 = blade on plate armour buzzes too (B&S is silent there).'),
        ('hit_buzz_ignore_self', "1 = a weapon's own parts hitting each other (flail head on its haft) never buzz."),
        ('hit_buzz_min_intensity', 'Below this (0-1) a contact is a touch, not a hit: no buzz.'),
        ('hit_buzz_impulse_full', 'Game impulse that counts as a full-strength blow on a held item.'),
        ('hit_buzz_limb_min_ms', 'An open hand or arm on a person buzzes only above this speed (m/s).'),
    ]),
    ('Eating and drinking', '', [
        ('pose_eat', '1 = food at your mouth is bitten; a tipped bottle at your mouth is drunk from.'),
        ('pose_eat_cm', 'Food this close to your mouth is bitten (cm).'),
        ('pose_drink_tip', 'Bottle neck must point lower than this to drink (-1 to 1; 0 = level).'),
        ('pose_drink_sip_s', 'Seconds between sips.'),
    ]),
    ('Old grip spring', 'Only used when pose_grip_hold=0.', [
        ('pose_grip_spring', 'Old grip spring on (only with pose_grip_hold=0).'),
        ('pose_grip_strength_kg', 'Most it holds up (kg).'),
        ('pose_grip_hz', 'How briskly it follows (Hz).'),
        ('pose_grip_damping', 'Damping ratio.'),
        ('pose_grip_radius_cm', 'Lever used for turning strength (cm).'),
        ('pose_grip_buzz', 'Buzz strength for the gap (0 = off).'),
        ('pose_grip_buzz_from_cm', 'Buzz starts past this stretch (cm).'),
        ('pose_grip_turn_scale', 'Turning stiffness multiplier.'),
        ('pose_grip_turn_damping', 'Turning damping ratio.'),
        ('pose_grip_turn_max_scale', 'Turning cap multiplier.'),
        ('pose_grip_anchor_marker', "1 = hold at the weapon's handle marker; 0 = the game's joint anchor."),
    ]),
    ('Ghost hand', "Off (you didn't like it).", [
        ('pose_ghost_hand', '1 = a see-through glove shows your real hand when his is stuck on a weapon (off: you disliked it).'),
        ('pose_ghost_from_cm', 'Shows when the hands are this far apart (cm).'),
        ('pose_ghost_mesh', 'Glove mesh.'),
        ('pose_ghost_material', 'Glove material.'),
        ('pose_ghost_x_cm', 'Glove offset X (cm).'),
        ('pose_ghost_y_cm', 'Glove offset Y (cm).'),
        ('pose_ghost_z_cm', 'Glove offset Z (cm).'),
        ('pose_ghost_pitch', 'Glove turn pitch (degrees).'),
        ('pose_ghost_yaw', 'Glove turn yaw (degrees).'),
        ('pose_ghost_roll', 'Glove turn roll (degrees).'),
        ('pose_ghost_scale', 'Glove scale.'),
    ]),
    ('Logging and troubleshooting', 'These change nothing in the game.', [
        ('log_poses', '1 = log the view anchor and camera once a second.'),
        ('log_frames', '1 = record every tick to logs\\frames-<time>.csv (headset, controllers, hands, held items).'),
        ('log_input', '1 = log input: keyboard focus, Esc and menu clicks, sticks, turn speed.'),
        ('log_sliders', '1 = log menu sliders when they change.'),
        ('log_controllers', '1 = log controller positions and buttons once a second.'),
        ('log_body', '1 = log his body, head and hands once a second.'),
        ('log_fingers', '1 = log finger bends once a second while posed.'),
        ('hold_blocker_log', 'Test: log which component a held weapon hits or is held off by (writes a lot).'),
        ('log_wrap', '1 = log the grip wrap and grab pull.'),
        ('log_body_grab', '1 = log body grabs every 0.1 s.'),
        ('log_joint_strain', "Log a held Willie's joint bends, twists and dislocation state."),
        ('joint_break', "Bending or twisting a held Willie's neck, shoulders or back past their range strains them (the old game's rules): the neck snaps, a shoulder dislocates, the back breaks."),
        ('joint_break_twist_free_neck_deg', 'How far his head turns freely (degrees from where it was when you took hold) before a twist strains the neck.'),
        ('joint_break_twist_free_arm_deg', 'Same for a twist of his upper arm in the shoulder.'),
        ('joint_break_twist_free_back_deg', 'Same for a twist of his back.'),
        ('joint_break_twist_gain', 'A twist past the free range counts as a bend of that many times the extra degrees.'),
        ('joint_break_free_min', 'With no resistance left (out cold, in great pain) the free twist range shrinks to this much of itself.'),
        ('joint_break_legs', 'Knees can be strained and dislocated too (calf bent against the thigh).'),
        ('joint_break_scale', 'How fast joints strain (1 = the old game).'),
        ('log_body_hits', "1 = log other Willies' body hits."),
        ('stab_log', "1 = log every held-weapon hit on a person ('Stab:' lines)."),
        ('hit_buzz_log', '1 = log every hit buzz.'),
        ('whoosh_log', "1 = log 'Whoosh:' lines while a held weapon moves fast."),
        ('whoosh_log_min_speed', 'Whoosh log from this speed (cm/s).'),
        ('perf_log_seconds', "A 'Perf:' line every this many seconds (0 = off)."),
        ('strength_log_seconds', "A 'Strength:' training line every this many seconds (0 = off)."),
        ('perf_events', "1 = with each Perf line, time the game's Blueprint calls per class (troubleshooting, ~1 ms a frame)."),
        ('perf_sample', '1 = with each Perf line, sample where the game thread spends its time (troubleshooting).'),
        ('perf_bp_segments', "1 = with each Perf line, time Willie's tick graph by bytecode segment (troubleshooting)."),
        ('capture_seconds', 'Troubleshooting pictures every this many seconds (0 = off).'),
        ('capture_menus_only', '1 = screen captures only while a menu is up.'),
        ('haptics_test', '1 = a short buzz each time a trigger is pulled all the way.'),
    ]),
    ('Developer test switches', 'For unattended tests; leave alone.', [
        ('dev_fake_headset', 'Test without a headset: a pretend headset.'),
        ('dev_fake_still', 'Pretend headset holds still.'),
        ('dev_fake_yaw', 'Pretend headset look left (degrees).'),
        ('dev_fake_pitch', 'Pretend headset look up (degrees).'),
        ('dev_fake_scale', 'Pretend headset movement amount (1 = full).'),
        ('dev_script', 'Unattended test script: seconds:command;...'),
        ('dev_pose_probe', 'Test only: pose dev_pose_bone directly.'),
        ('dev_pose_bone', 'Test only: bone to pose.'),
        ('dev_pose_pitch', 'Test only: pitch (degrees).'),
        ('dev_pose_yaw', 'Test only: yaw (degrees).'),
        ('dev_pose_roll', 'Test only: roll (degrees).'),
        ('dev_pose_fake_tracking', 'Test only: head and hands follow a made-up path.'),
        ('dev_grip_trace', 'Log the right-hand grip every tick.'),
    ]),
]
UNUSED_TITLE = "No longer read by the mod"
UNUSED_NOTE = "Old settings; changing them does nothing. Safe to delete."

HEADER = """; ======================================================================
;  HALF SWORD VR SETTINGS
; ======================================================================
; Edit and save: the mod re-reads this file about once a second while the game runs.
; Each setting is one line, key=value. The line above it (starting with ;) says what it does.
; Lines starting with ; or [ are only notes and headings; never put a note on a value line.
; Each setting appears once. To find one: Ctrl+F its name, or the section number below, e.g. [16.
; Settings from a newer mod version are added under the setting they belong with.
"""


def code_keys():
    text = (SRC / "config.cpp").read_text()
    keys = re.findall(r'^\s*(?:num|flag|integer|text)\("([^"]+)"', text, re.M)
    return keys


def code_defaults():
    """Defaults from config.hpp, formatted the way the mod writes them."""
    hpp = (SRC / "config.hpp").read_text()
    out = {}
    for line in hpp.splitlines():
        line = line.split("//")[0].strip()
        m = re.match(r"(bool|int|double|std::string)\s+(.*);$", line)
        if not m:
            continue
        kind, rest = m.groups()
        if kind == "std::string":
            for name, val in re.findall(r'(\w+)(?:\s*=\s*"([^"]*)")?', rest):
                out[name] = val
            continue
        for part in rest.split(","):
            name, _, val = part.partition("=")
            name, val = name.strip(), val.strip() or "0"
            if kind == "bool":
                out[name] = "1" if val == "true" else "0"
            elif kind == "int":
                out[name] = str(int(val, 0))
            else:
                out[name] = "%g" % float(val)
    return out


def read_ini(path):
    pairs = {}
    for raw in Path(path).read_text(encoding="utf-8").splitlines():
        s = raw.strip()
        if not s or s[0] in ";#[" or "=" not in s:
            continue
        k, _, v = s.partition("=")
        pairs[k.strip()] = v.strip()  # last one counts, as in the mod
    return pairs


WRAP = 96
RULE = "; " + "=" * 70 + "\n"


def note_lines(text):
    """A ; note, wrapped so no line runs off the screen."""
    return "".join("; %s\n" % line for line in textwrap.wrap(text, WRAP, break_on_hyphens=False))


def build(values, keys_in_code):
    placed = [k for _, _, items in LAYOUT for k, _ in items]
    dupes = {k for k in placed if placed.count(k) > 1}
    if dupes:
        sys.exit("placed twice in LAYOUT: " + ", ".join(sorted(dupes)))
    unplaced = [k for k in keys_in_code if k not in placed]
    if unplaced:
        sys.exit("read by the mod but not in LAYOUT (add them to a section): " + ", ".join(unplaced))
    uncommented = [k for _, _, items in LAYOUT for k, c in items if not c]
    if uncommented:
        sys.exit("every setting needs a comment in LAYOUT: " + ", ".join(uncommented))
    sections = [(t, n, [(k, c) for k, c in items if k in values]) for t, n, items in LAYOUT]
    sections = [s for s in sections if s[2]]
    extra = [k for k in values if k not in placed]
    if extra:
        sections.append((UNUSED_TITLE, UNUSED_NOTE, [(k, "") for k in extra]))
    lines = [HEADER, ";\n; SECTIONS\n"]
    for i, (title, _, _) in enumerate(sections, 1):
        lines.append(";   %2d. %s\n" % (i, title))
    for i, (title, note, items) in enumerate(sections, 1):
        lines.append("\n\n" + RULE + ";  %d. %s\n" % (i, title.upper()) + RULE)
        lines.append("[%d. %s]\n" % (i, title))
        if note:
            lines.append(note_lines(note))
        lines.append("\n")
        for k, c in items:
            if c:
                lines.append(note_lines(c))
            lines.append("%s=%s\n\n" % (k, values[k]))
    return "".join(lines).rstrip("\n") + "\n"


def main():
    args = sys.argv[1:]
    keys = code_keys()
    if args and args[0] == "--defaults":
        d = code_defaults()
        values = {k: d[k] for k in keys}
        out = args[1]
    else:
        values = read_ini(args[0])
        out = args[1]
    text = build(values, keys)
    Path(out).write_text(text, encoding="utf-8", newline="\r\n")
    after = read_ini(out)
    if after != values:
        diff = set(values.items()) ^ set(after.items())
        sys.exit("MISMATCH: " + repr(sorted(diff)))
    print("ok: %d settings, identical key=value set" % len(after))


if __name__ == "__main__":
    main()
