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
from pathlib import Path

SRC = Path(__file__).resolve().parents[2] / "mod" / "HalfSwordVR" / "src"

# (section title, section note, [(key, comment or "")]). Quick settings first.
LAYOUT = [
    ("Quick settings", "The ones you are most likely to change. Each is in only this place.", [
        ("test_room", "1 = boot straight into the test arena (passive foes, two weapon tables); 0 = normal game. Read at launch."),
        ("test_room_button", "1 = in Free Mode the tavern's Train (the fencer) has a Train button that opens the test room."),
        ("test_room_foes", "Foes in the test room (1-10)."),
        ("render_scale", "Per-eye resolution as a share of what the headset asks for (1.0 = full)."),
        ("screen_percentage", "Share of the eye the engine draws before upscaling (50-100; -1 = game's own)."),
        ("sharpen", "0 = none, 0.5-1 = crisper (-1 = game's own)."),
        ("light_samples", "Bounce-light density: one sample per this many pixels (game 64 flickers; 32/16/8 denser and slower; 0 = game's)."),
        ("pose_stick_turn", "Right stick turning: smooth, snap or off."),
        ("pose_stick_turn_dps", "Smooth turn speed (degrees per second)."),
        ("pose_snap_turn_deg", "Snap turn step (degrees)."),
        ("pose_move_relative", "Left stick walks toward where you look (head) or where your body faces (body)."),
        ("pose_stick_dead_zone", "Stick dead zone (0-1)."),
        ("pose_crouch_button_cm", "Left stick click toggles a crouch this deep (cm; 0 = off)."),
        ("pose_crouch_button_cms", "How fast that crouch goes down and up (cm/s)."),
        ("kick_on_a", "1 = a short tap of right A kicks."),
        ("kick_tap_s", "A press shorter than this (s) is a kick tap."),
        ("jump_on_x", "1 = left X is the game's jump: hold, then let go to leap (a quick tap is the game's tap-kick)."),
        ("slowmo_on_right_stick_click", "1 = right stick click toggles the game's slow motion."),
        ("pose_hold_arm_lag", "1 = held weapons trail your hand like flat Half Sword's arm (heavier = more lag)."),
        ("pose_hold_arm_lag_s", "Empty-arm follow delay (s); weapons add to it by their weight. Flat game ~0.1."),
        ("pose_hold_arm_lag_max_s", "Most lag any weapon gets (s)."),
        ("pose_hold_heft_lag_s", "Heft floor: a weapon lags at least this x sqrt(its inertia about the shoulder) (s), so a greatsword never swings like a dagger."),
        ("pose_hold_strength", "Multiplier on the whole hold (force and turning)."),
        ("pose_hold_max_force", "Most force one hand puts on a held item (N, x his strength)."),
        ("pose_hold_max_torque", "Wrist strength: most turning force one hand has at the grip (N m, x his strength)."),
        ("pose_hold_wrist_stop_torque", "Wrist end stop: turning limit once the item is bent past the free angle (N m)."),
        ("pose_hold_shoulder_torque", "Shoulder strength against swinging arm + weapon (N m; 0 = off). Lower = heavy weapons trail more."),
        ("pose_two_hand_strength", "Two hands on one weapon hold this many times as strong."),
        ("pose_stamina_scale", "Stamina cost of holding and swinging (1 = exactly the flat game, which barely drains; 4 = chosen 2026-11-21)."),
        ("pose_throw_multiplier", "Thrown weapon speed x this (B&S 2)."),
        ("pose_grab_touch_cm", "How far from an item your palm can be and still grab it (cm)."),
        ("hit_buzz", "Hit vibration strength (0 = off, 1 = like Blade & Sorcery)."),
        ("hit_buzz_scrape", "Scrape ticks while a blade slides along something (0 = off)."),
        ("hit_buzz_blade_on_plate", "1 = blade on plate armour buzzes too (B&S is silent there)."),
        ("hit_buzz_ignore_self", "1 = a weapon's own parts hitting each other (flail head on its haft) never buzz."),
        ("hit_buzz_min_intensity", "Below this (0-1) a contact is a touch, not a hit: no buzz."),
        ("whoosh_vr_threshold_scale", "Swing whoosh in VR: the game's whoosh speed threshold x this (1 = game's own, needs 7.5 m/s swings; lower = slower swings whoosh)."),
        ("whoosh_walk_speed_scale", "Whoosh threshold raised by your walking speed x this, so walking with a weapon does not whoosh (0 = off)."),
        ("pose_lean", "1 = his torso leans to put his head where yours is."),
        ("pose_lean_head_angle", "1 = he also bends forward/sideways when you tilt your head (pitch/roll); 0 = he only leans to where your neck is (closed PR #8/#10's way)."),
        ("pose_lean_look_down_back_cm", "Looking down, his chest leans back out of your view by up to this many cm (0 = off)."),
        ("pose_lean_pitch_gain", "pose_lean_head_angle 1: how much of your head's downward pitch the torso takes."),
        ("pose_lean_step_radius_cm", "Lean first, step after: how far your head can move from his hips (cm) before he steps (0 = always step)."),
        ("pose_lean_settle_s", "Hold your head still this long (s) away from his hips and he steps under you (0 = stays leaned)."),
        ("hide_own_head", "1 = your own head isn't drawn in your view (shadow and helmet stay)."),
        ("helmet_view", "Your worn helmet in your view: 0 = drawn (blocks the view like a real one), 1 = helmet hidden from you, 2 = everything on your head hidden from you, 3 = clear window (parts near your eyes not drawn), 5 = see-through (faded, see helmet_fade_see_through). Others still see it; it still protects you."),
        ("helmet_window_cm", "helmet_view 3: helmet parts closer than this to your eyes aren't drawn (cm; 5-9). Food at your mouth that close is cut too."),
        ("helmet_fade_see_through", "helmet_view 5: how see-through your helmet is (0 = solid, 0.5 = half, 1 = invisible)."),
        ("helmet_fade_always", "helmet_view 5 fades only pieces in front of your eyes (visors, barbutes, armets, closed sallets); hats and caps stay solid. Mesh names to fade anyway, comma separated (the log names them)."),
        ("helmet_fade_never", "helmet_view 5: mesh names never to fade, comma separated."),
        ("helmet_fade_tint", "helmet_view 5: the faded helmet's metal colour, r,g,b (0-1)."),
        ("helmet_fade_metallic", "helmet_view 5: how metallic the faded helmet looks (0-1)."),
        ("helmet_fade_roughness", "helmet_view 5: how rough/dull the faded helmet looks (0 = mirror, 1 = matte)."),
        ("helmet_fade_material", "helmet_view 5: the translucent material used (the game's own Datasmith glass material)."),
        ("helmet_see_through", "helmet_view 4 (experiment, does NOT work yet: test 2026-11-20 found no material input that makes the helmet see-through): how much you see through, 0..1."),
        ("helmet_see_through_mode", "helmet_view 4: 0 = screen door through the helmet material, 1 = helmet hidden on alternate frames."),
        ("helmet_see_through_param", "helmet_view 4 mode 0: the helmet material's texture parameter we replace (test)."),
        ("helmet_see_through_test", "helmet_view 4 mode 0: 0 = dither, 1 = all white, 2 = all black (finding what drives the opacity mask)."),
        ("pose_wrist_forward_cm", "Hand placement on the controller: wrist forward (cm, controller frame)."),
        ("pose_wrist_right_cm", "...toward the back of the hand (cm)."),
        ("pose_wrist_up_cm", "...up (cm)."),
        ("pose_hand_pitch", "Hand turn relative to the controller (degrees)."),
        ("pose_hand_yaw", ""),
        ("pose_hand_roll", ""),
        ("pose_knuckle_deg", "Fingers tilted this much from the handle (degrees)."),
        ("pose_user_arm_cm", "Your arm length (shoulder to wrist, cm). Measured by holding both arms out and holding B+Y 2 s; 0 = not measured."),
    ]),
    ("Startup", "Whether the mod and VR start.", [
        ("enabled", "0 = mod off."),
        ("vr_auto", "1 = VR only when SteamVR is already running as the game starts (else plays flat); 0 = always start VR."),
        ("vr_auto_processes", "Process names that count as SteamVR running."),
        ("require_steamvr_running", "Older name of vr_auto (only read when present; 0 = same as vr_auto=0)."),
    ]),
    ("Test room", "test_room, test_room_button and test_room_foes are in Quick settings.", [
        ("test_room_map", ""),
        ("test_room_tier", "Foes' tier: -1 random, 0 Beggar .. 8 Lord."),
        ("test_room_weapon_tier", "Table weapons' tier: -1 random 0-7, 0 Beggar .. 8 Lord."),
        ("test_room_respawn", "1 = a foe out of the fight is replaced."),
        ("test_room_respawn_seconds", ""),
        ("test_room_despawn_seconds", "His body is removed this long after he is out (0 = bodies stay)."),
    ]),
    ("Picture and view", "Where your eyes sit and how the picture is made.", [
        ("world_scale", "1 = life size."),
        ("anchor", "head = view from his head bone, camera = the game's camera."),
        ("head_bone", ""),
        ("heading", "Straight ahead follows aim (mouse) or camera."),
        ("eye_forward_cm", "Eyes from the head bone, forward (cm)."),
        ("eye_up_cm", "...and up (cm)."),
        ("anchor_smoothing", "Smoothing on the head anchor (s; takes physics jitter out)."),
        ("tracking_current_anchor", ""),
        ("anchor_room_unsmoothed", "1 = your room steps move the view at once (not smoothed), so the world and your hands don't swing when his feet follow you."),
        ("near_clip_cm", "Near clip of the eyes, cm. Keep it at 1: at 4 the sides of closed helmets were cut away (see-through)."),
        ("gamma_mode", "0 auto, 1 always decode sRGB, 2 never."),
        ("separate_render_target", "1 = full headset resolution with menus on a panel; 0 = fallback through the game window."),
        ("predict_frames", "Pose prediction frames (-1 = automatic)."),
        ("pose_max_age", ""),
        ("fix_clouds", "1 = keep clouds correct in both eyes."),
        ("cloud_mode", "2 or 0."),
        ("extra_commands", "Extra engine console commands, separated by semicolons."),
        ("idle_helper", "1 = game keeps normal speed while the headset sleeps."),
        ("mirror_window", "Show the left eye in the game window."),
        ("stereo_slot_offset", "0 = find automatically."),
    ]),
    ("Menus, panel and keys", "Key codes are Windows virtual keys: 36 Home (recenter), 35 End (panel), 45 Insert (view), 34 Page Down (mode).", [
        ("ui_panel", "Show menus/HUD on a floating panel."),
        ("ui_panel_auto", "1 = panel only while a menu is up."),
        ("ui_distance_m", ""),
        ("ui_width_m", ""),
        ("ui_height_offset_m", ""),
        ("ui_opacity_gain", ""),
        ("ui_menu_backdrop", "Dark sheet behind menus (0-1)."),
        ("ui_use_alpha", ""),
        ("ui_pointer", "Point a controller at the panel to move the mouse; trigger clicks."),
        ("menu_buttons", "Left menu = Escape; right A held after a won fight = continue."),
        ("menu_continue_hold_s", ""),
        ("wipe_standins", ""),
        ("recenter_key", ""),
        ("toggle_ui_key", ""),
        ("toggle_view_key", ""),
        ("toggle_mode_key", ""),
    ]),
    ("Moving: sticks, turning, crouch", "Stick turn, dead zone, crouch button and kick are in Quick settings.", [
        ("pose_stick_walk", ""),
        ("pose_body_turn", "1 = his body turns to face where you look."),
        ("pose_mouse_off_with_controllers", "1 = with controllers tracked, the mouse does not turn or tilt him."),
        ("pose_turn_dead_zone_deg", "...once you look this far away."),
        ("pose_turn_speed_dps", ""),
        ("pose_turn_max_deg", "Past this the hips come round at once."),
        ("pose_crouch", "1 = he crouches as low as you do."),
        ("pose_crouch_dead_zone_cm", ""),
        ("pose_crouch_max_cm", "How far his legs crouch him (cm); deeper, he bends forward so his hands still reach your floor."),
        ("pose_crouch_bend_for_depth", ""),
        ("pose_crouch_keep_leg_ik", ""),
        ("pose_player_no_kneel", "1 = the game's kneel never takes your legs."),
    ]),
    ("Body posing", "Your character posed from the headset and controllers.", [
        ("pose_body", "Master switch for the posed body."),
        ("pose_head", ""),
        ("pose_head_lock", "1 = head held at the headset so the helmet stays still in view."),
        ("pose_head_lock_max_cm", ""),
        ("pose_lean_pitch_dead_deg", ""),
        ("pose_lean_max_deg", ""),
        ("pose_lean_roll_dead_deg", ""),
        ("pose_lean_roll_gain", ""),
        ("pose_lean_side_max_deg", ""),
        ("pose_lean_smooth_s", ""),
        ("pose_lean_slack_cm", ""),
        ("pose_lean_settle_speed_cms", ""),
        ("pose_lean_back_radius_cm", "He leans back at most this far (cm); further back his hips follow."),
        ("pose_lean_walk_catchup_s", "Once your neck passes the step radius he steps: hips back under you over this long (s)."),
        ("pose_room_neck_base_cm", "Leans and steps are measured this far below your head joint (base of the neck), so nodding or tilting your head doesn't move him (0 = at the head joint)."),
        ("pose_lean_follow_past_radius", "1 = past the step radius his hips follow your neck at once, so it never gets further than the radius; 0 = he steps all the way under you."),
        ("pose_lean_follow_speed_cms", "pose_lean_follow_past_radius: fastest his hips follow (cm/s)."),
        ("pose_lean_cancel_game_bend", ""),
        ("pose_torso_twist", "Share of your head's turn the spine and neck take."),
        ("hide_own_gore_from", "Gore skeleton hidden from this bone up (empty = not hidden)."),
        ("player_takes_damage", "1 = hits on your body hurt you as in the flat game (health, consciousness, bleeding)."),
        ("player_body_contact_speed", "1 = bumping into people counts by how fast your body moves into them, not how deep it sinks in (walking into someone no longer hurts him)."),
        ("player_body_contact_mass_scale", "Your body part's mass in those bumps is x this (1 = its own mass; higher = harder bumps and kicks)."),
        ("player_body_contact_log", "1 = log each changed bump (\"Body contact:\" lines)."),
        ("player_item_contact_speed", "1 = an item pinned or squeezed against you hurts only by how fast it comes into you (real blows unchanged)."),
        ("player_stump_blood", "1 = blood from your own cut-off limb stays at the stump (not floating where the limb was)."),
        ("injury_limbs", "1 = your bad injuries cripple you: a limb below injury_limb_health, broken, dislocated or cut off."),
        ("injury_limb_health", "Limb health below this counts as badly hurt (25 = the game's own bone-break threshold; a bone put back gets 25)."),
        ("injury_one_leg_speed", "One bad leg: walk at this share of normal speed."),
        ("injury_two_legs_fall", "1 = both legs bad: no walking and you fall over until a leg recovers."),
        ("injury_back_broken_falls", "1 = a broken back counts as both legs (the game drops both legs' strength)."),
        ("injury_dislocated_follow_s", "Dislocated (not broken) arm: how far it lags your controller (s)."),
        ("injury_dislocated_sag_cm", "Dislocated arm: how much it sags below your controller (cm)."),
        ("injury_swing_relocate", "1 = swing your controller hard (the game's swing speed over 60) to put a broken arm back, the game's own rule."),
        ("death_ragdoll", "1 = when you die he ragdolls and falls; the view stays on his head."),
        ("pose_arms", ""),
        ("pose_follow_room", ""),
        ("pose_hold_capsule", ""),
        ("pose_feet", "1 = feet stay planted and step."),
        ("pose_feet_free_on_kick", "1 = the game's kick moves the leg freely; the foot is planted again where it lands."),
        ("pose_feet_kick_free_s", ""),
        ("pose_step_distance_cm", ""),
        ("pose_step_angle_deg", ""),
        ("pose_step_time_s", ""),
        ("pose_step_lift_cm", ""),
        ("pose_step_lead_s", ""),
        ("pose_no_uro", ""),
        ("pose_helmet_late", ""),
        ("pose_mesh_after_driver", ""),
        ("pose_in_anim_update", ""),
        ("pose_post_physics", ""),
        ("pose_actor_z_smooth_s", ""),
        ("pose_move_bodies", ""),
        ("pose_teleport_above_cm", ""),
        ("pose_capsule_floor_clamp", ""),
        ("pose_capsule_keep_on_floor", ""),
        ("pose_capsule_no_step_offset", ""),
    ]),
    ("Arms, hands and fingers", "Hand placement and arm length are in Quick settings.", [
        ("pose_forearm_twist_share", ""),
        ("pose_swivel_smooth_s", "Elbow swing smoothing (s)."),
        ("pose_arm_solve_drawn_hand", ""),
        ("pose_shoulder_reach_cm", "Shoulder comes forward this much at full reach (0 = off)."),
        ("pose_shoulder_reach_start_cm", ""),
        ("pose_shoulder_reach_max_deg", ""),
        ("hand_bone_left", ""),
        ("hand_bone_right", ""),
        ("input_hand_tracking", "Read SteamVR's finger tracking when offered."),
        ("pose_fingers_tracked", "1 = free fingers follow yours."),
        ("pose_fingers_tau_s", ""),
        ("pose_fingers_per_joint", "1 = with finger tracking, each finger joint follows your own."),
        ("pose_finger_dip_coupling", "End joint = this x the middle joint's bend (0 = as tracked)."),
        ("pose_finger_scale_mcp", "Finger full bend x this at the knuckle."),
        ("pose_finger_scale_pip", "... at the middle joint."),
        ("pose_finger_scale_dip", "... at the end joint."),
        ("pose_finger_schedule", "1 = finger joints close in a natural order (no round 'circle' fingers)."),
        ("pose_finger_mcp_lag", ""),
        ("pose_finger_pip_full", ""),
        ("pose_wrap", "1 = fingers wrap bodies and loose objects you grab (procedural grab)."),
        ("pose_wrap_ease_s", ""),
        ("pose_wrap_refresh_s", ""),
        ("pose_wrap_finger_radius_cm", "0 = auto."),
        ("pose_wrap_open_base", ""),
        ("pose_grab_seat_palm", "1 = what you grab sits on your palm, not inside the fist."),
        ("pose_grab_palm_cm", ""),
        ("pose_grab_scenery_gap_cm", ""),
        ("pose_grab_palm_along", ""),
        ("pose_grab_drive", "1 = the mod holds grabbed objects/bodies to your palm (B&S-style)."),
        ("pose_grab_drive_heft", ""),
        ("pose_grab_drive_accel", ""),
        ("pose_grab_drive_ang_accel", ""),
        ("pose_grab_body_pull_n", ""),
        ("pose_grab_body_pull_hz", ""),
        ("pose_grab_body_pull_slack_cm", "Hand this far from a held body before it pulls (stops instant chokes)."),
        ("pose_grab_body_pull_min_kg", "The body pull pulls as if the held part weighed at least this (kg): a neck moves his whole upper body."),
        ("pose_grab_ride", "1 = your hand stays on a grabbed NPC (neck, arm) instead of floating off it."),
        ("pose_grab_ride_objects", ""),
        ("pose_grab_ride_from_cm", ""),
        ("pose_grab_ride_full_cm", ""),
        ("pose_grab_ride_max_cm", ""),
        ("pose_grab_slip_s", ""),
        ("pose_fingers_anim_only", ""),
    ]),
    ("Holding: weight, strength, stamina", "The main feel values are in Quick settings.", [
        ("pose_grip_hold", ""),
        ("pose_hold_exact", ""),
        ("pose_hold_bs", "1 = Blade & Sorcery style spring hold."),
        ("pose_hold_spring", ""),
        ("pose_hold_damper", ""),
        ("pose_hold_turn_spring", ""),
        ("pose_hold_turn_damper", ""),
        ("pose_hold_damping_ratio", "Dampers per weapon for this damping ratio (0 = fixed dampers)."),
        ("pose_hold_target_vel_smooth_s", ""),
        ("pose_hold_wrist_free_deg", "Wrist bends freely up to this angle..."),
        ("pose_hold_wrist_stop_deg", "...and reaches the full end stop here."),
        ("pose_hold_wrist_stop_spin", ""),
        ("pose_hold_torque_strength_power", ""),
        ("pose_hold_arm_reach_cm", ""),
        ("pose_hold_shoulder_fade_from_cm", ""),
        ("pose_hold_shoulder_fade_to_cm", ""),
        ("pose_hold_let_go_reach_slack_cm", ""),
        ("pose_hold_weight", ""),
        ("pose_hold_weight_torque_scale", ""),
        ("pose_hold_weight_two_hand_couple", ""),
        ("pose_hold_weight_spin_extra", ""),
        ("pose_bs_two_pos_k", "Two-hand hold springs (B&S)."),
        ("pose_bs_two_pos_c", ""),
        ("pose_bs_two_pos_max", ""),
        ("pose_bs_two_rot_k", ""),
        ("pose_bs_two_rot_c", ""),
        ("pose_bs_two_rot_max", ""),
        ("pose_bs_max_spin", ""),
        ("pose_bs_iterations", ""),
        ("pose_bs_two_let_go_cm", ""),
        ("pose_body_strength", "1 = his strength, injuries and stamina scale the hold."),
        ("pose_body_stamina_floor", ""),
        ("pose_body_strength_min", ""),
        ("pose_body_size_strength", ""),
        ("pose_stamina_gate", ""),
        ("pose_stamina_fist_scale", ""),
        ("pose_no_lawnmower", "1 = no mouse-waggle spam penalty for your VR swings (it multiplied stamina cost up to 10x)."),
        ("pose_stamina_force_cap_n", ""),
        ("pose_stamina_torque_cap_nm", ""),
        ("pose_hold_hand_on_weapon", ""),
        ("pose_hold_blocked_from_cm", ""),
        ("pose_hold_hand_follows", "1 = his drawn hand rides the held item."),
        ("pose_hold_hand_follows_predict", ""),
        ("pose_hold_follow_predict_max_cm", ""),
        ("pose_hold_follow_predict_smooth_s", ""),
        ("pose_hold_let_go", "Stuck rule: drop a weapon stuck out of reach."),
        ("pose_hold_let_go_s", ""),
        ("pose_hold_let_go_grace_s", ""),
        ("pose_hold_let_go_dead", "1 = a stuck weapon is let go with no speed (never flies off)."),
        ("pose_hold_measured_v", "1 = a stopped weapon is driven from how it really moves (no wind-up)."),
        ("pose_hold_joint", "Experimental: 1 = weapons held by engine joints (B&S style), 0 = old hold."),
        ("pose_hold_joint_stiffness", ""),
        ("pose_hold_joint_damping", ""),
        ("pose_hold_joint_max", ""),
        ("pose_hold_joint_shoulder", ""),
        ("pose_hold_joint_carry_weight", ""),
        ("pose_hold_joint_fade_min_speed", ""),
        ("pose_hold_let_go_ignore_self", ""),
        ("pose_hold_contact_cm", ""),
        ("pose_hold_ignore_hand", ""),
        ("pose_hold_ignore_body", ""),
        ("pose_hold_block_offhand_search", ""),
    ]),
    ("Grabbing, sliding, two hands, shields", "", [
        ("pose_grab_button", ""),
        ("pose_grab_release_drops", ""),
        ("pose_grab_objects", ""),
        ("pose_grab_world", ""),
        ("pose_grab_radius_cm", ""),
        ("pose_grab_handles", "1 = B&S-style handle grabbing."),
        ("pose_grab_handle_cm", ""),
        ("pose_grab_weapon_handles_only", ""),
        ("pose_grab_settle_s", "Grabbed item eases into the hand over this long (s)."),
        ("pose_grab_seat", ""),
        ("pose_grab_seat_reverse_bias_deg", ""),
        ("pose_grab_from_controller", ""),
        ("pose_grab_controller_gap_cm", ""),
        ("pose_grab_controller_max_cm", ""),
        ("pose_weapon_free_grip", ""),
        ("pose_weapon_free_cm", ""),
        ("pose_slide", "Hold trigger to slide the hand along a handle."),
        ("pose_slide_hand_half_cm", ""),
        ("pose_slide_rate_min", ""),
        ("pose_slide_rate_max", ""),
        ("pose_slide_max_cm_s", ""),
        ("pose_slide_keep_light", ""),
        ("pose_slide_keep_full", ""),
        ("pose_slide_damper_light", ""),
        ("pose_slide_damper_full", ""),
        ("pose_slide_bs_damper", "B&S slide: gravity pulls an upright weapon through a loose hand, a level one follows the hand (0 = old free slide)"),
        ("draw_seat_ref_fix", "1 = scabbard seat reference set with the engine's real parameter names (changes draw feel; 0 = as before)"),
        ("pose_slide_zone_measured", ""),
        ("pose_slide_wide_cm", ""),
        ("pose_slide_ring_max_len_cm", ""),
        ("pose_slide_ring_max_width_cm", ""),
        ("pose_slide_gap_max_cm", ""),
        ("pose_slide_rondel_stops", ""),
        ("pose_slide_butt_is_haft", ""),
        ("pose_slide_fixed_1h_sword", ""),
        ("pose_grip_two_zones_2h_sword", "Two fixed grip zones (under guard, above pommel): 0 off, 1 bastard swords only, 2 all two-handed swords."),
        ("pose_grip_dagger_fixed", ""),
        ("pose_two_hand", ""),
        ("pose_two_hand_blade_gap_cm", ""),
        ("pose_two_hand_reach_cm", ""),
        ("pose_two_hand_let_go_cm", ""),
        ("pose_two_hand_curl", ""),
        ("pose_two_hand_lock_rotation", ""),
        ("pose_two_hand_mirror", ""),
        ("pose_two_hand_twist_only", "1 = with two hands the turning hand only rolls the weapon; your two hands point it."),
        ("pose_two_hand_swing_share", "0..1: how much the turning hand still points a two-handed weapon (1 = old)."),
        ("pose_shield_free_grip", ""),
        ("pose_shield_point_cm", ""),
        ("pose_shield_in_hand", "1 = strap shields held in the hand, not on the forearm."),
        ("pose_shield_hand_grip_type", ""),
        ("pose_shield_fixed_grip", "1 = shield straps/handles are fixed grips (held at their middle, no trigger slide)."),
        ("pose_shield_grab_rim", "1 = a shield can also be taken by its rim; 0 = straps and handles only."),
        ("pose_shield_rim_cm", "How near an outer edge the hand must be for a rim grab (cm)."),
        ("pose_shield_grab_face", "1 = a shield can be taken anywhere, flat faces too (old way)."),
        ("pose_pavise_forearm", "1 = pavises strap onto the forearm (light: either arm; heavy: left arm), not held in the hand."),
    ]),
    ("Drawing, sheathing, belt", "", [
        ("pose_draw", ""),
        ("pose_draw_touch_cm", "A squeeze this near a scabbarded weapon's grip on your belt draws it (cm)."),
        ("pose_draw_zone_cm", "A squeeze this near any part of a belt weapon without a scabbard, or a shield on your back, takes it off you (cm)."),
        ("pose_draw_settle_s", ""),
        ("pose_draw_step_cm", ""),
        ("pose_draw_force_scale", ""),
        ("pose_hold_hand_follows_draw", ""),
        ("pose_draw_force_scale_follows", ""),
        ("pose_draw_scabbard_free", ""),
        ("pose_sheathe", ""),
        ("pose_sheathe_mouth_cm", ""),
        ("pose_sheathe_angle_deg", ""),
        ("pose_sheathe_cross", ""),
        ("pose_scabbard_grab", ""),
        ("pose_scabbard_handle", ""),
        ("pose_scabbard_touch_cm", ""),
        ("pose_scabbard_angle_deg", ""),
        ("pose_scabbard_follow_s", ""),
        ("pose_scabbard_let_go_cm", ""),
        ("pose_scabbard_grip_type", ""),
        ("pose_scabbard_block_auto", ""),
        ("pose_holster_snap", "Holster snap for weapons without a scabbard."),
        ("pose_holster_zone_cm", ""),
        ("pose_holster_max_speed", ""),
        ("pose_keep_scabbard_on_drop", ""),
    ]),
    ("Swinging and throwing", "", [
        ("pose_feed_swing_speed", ""),
        ("pose_swing_speed_lo", ""),
        ("pose_swing_speed_hi", ""),
        ("pose_swing_speed_fall_s", ""),
        ("pose_throw", ""),
        ("pose_throw_speed", "Let go faster than this (m/s) = a throw."),
    ]),
    ("Stabbing, cutting, hits", "", [
        ("stab_assist", ""),
        ("stab_min_speed_cms", ""),
        ("stab_min_into_cos", ""),
        ("stab_arm_mass_kg", ""),
        ("stab_push_force_n", ""),
        ("stab_push_time_s", ""),
        ("stab_no_rubber_band", ""),
        ("stab_pickup_grace_s", ""),
        ("stab_wound_hold", ""),
        ("stab_wound_in_cms", ""),
        ("stab_wound_out_cms", ""),
        ("stab_wound_head_factor", ""),
        ("stab_wound_head_in_factor", ""),
        ("stab_wound_settle_s", ""),
        ("stab_wound_settle_max_s", ""),
        ("stab_wound_entry_cms", ""),
        ("stab_start_depth_cm", ""),
        ("stab_start_depth_per_ms", ""),
        ("stab_start_depth_max_cm", ""),
        ("stab_yank_cm", ""),
        ("stab_yank_s", ""),
        ("stab_drag", "A stuck weapon drags the body instead of letting go."),
        ("stab_drag_force_scale", ""),
        ("stab_drag_grip_s", ""),
        ("stab_drag_reach_slack_cm", ""),
        ("stab_drag_reach_s", ""),
        ("pose_hold_stuck_force_scale", ""),
        ("hit_assist", "Your arm's mass behind every other hit."),
        ("hit_arm_mass_kg", ""),
        ("hit_assist_min_cms", ""),
        ("cut_follow_through", ""),
        ("cut_release_cm", ""),
        ("cut_release_after_s", ""),
        ("grip_flip", "Tap B/Y to flip a one-handed weapon end for end."),
        ("grip_flip_short_only", ""),
        ("grip_flip_s", ""),
        ("grip_flip_tap_s", ""),
    ]),
    ("Fists, elbows, body grabs", "", [
        ("pose_fists", ""),
        ("pose_fist_class", ""),
        ("pose_fist_mark_spawned", ""),
        ("pose_fist_from_fingers", "Fist from your real fingers."),
        ("pose_fist_index_on", ""),
        ("pose_fist_index_off", ""),
        ("pose_fist_grip_on", ""),
        ("pose_fist_grip_off", ""),
        ("pose_fist_thumb_on", ""),
        ("pose_fist_thumb_off", ""),
        ("pose_fist_slap", ""),
        ("pose_fist_slap_ass", ""),
        ("pose_fist_slap_keep_s", ""),
        ("pose_fist_clear", "No fist while the hand is inside another Willie."),
        ("pose_fist_clear_margin_cm", ""),
        ("pose_fist_clear_retry_s", ""),
        ("pose_fist_shape", ""),
        ("pose_fist_game_flags", ""),
        ("pose_elbow_strikes", "Elbows hit like a punch."),
        ("pose_elbow_radius_cm", ""),
        ("pose_elbow_min_ass", ""),
        ("pose_elbow_learn_min_hits", ""),
        ("pose_elbow_rigidity", ""),
        ("pose_elbow_cut", ""),
        ("pose_elbow_impulse_per_v", ""),
        ("pose_elbow_velocity_scale", ""),
        ("pose_grab_body_hold", ""),
        ("pose_grab_body_spring", ""),
        ("pose_grab_body_damper", ""),
        ("pose_grab_body_cap_per_t", ""),
        ("pose_grab_body_max", ""),
        ("pose_grab_body_let_go_cm", ""),
        ("pose_grab_body_no_slip", ""),
        ("pose_grab_body_slip_below_t", ""),
    ]),
    ("Eating and drinking", "", [
        ("pose_eat", ""),
        ("pose_eat_cm", "Food this close to your mouth is bitten (cm)."),
        ("pose_drink_tip", ""),
        ("pose_drink_sip_s", ""),
    ]),
    ("Hit vibration (more)", "Main strength values are in Quick settings.", [
        ("hit_buzz_impulse_full", ""),
        ("hit_buzz_limb_min_ms", ""),
    ]),
    ("Old grip spring", "Only used when pose_grip_hold=0.", [
        ("pose_grip_spring", ""),
        ("pose_grip_strength_kg", ""),
        ("pose_grip_hz", ""),
        ("pose_grip_damping", ""),
        ("pose_grip_radius_cm", ""),
        ("pose_grip_buzz", ""),
        ("pose_grip_buzz_from_cm", ""),
        ("pose_grip_turn_scale", ""),
        ("pose_grip_turn_damping", ""),
        ("pose_grip_turn_max_scale", ""),
        ("pose_grip_anchor_marker", ""),
    ]),
    ("Ghost hand", "Off (you didn't like it).", [
        ("pose_ghost_hand", ""),
        ("pose_ghost_from_cm", ""),
        ("pose_ghost_mesh", ""),
        ("pose_ghost_material", ""),
        ("pose_ghost_x_cm", ""),
        ("pose_ghost_y_cm", ""),
        ("pose_ghost_z_cm", ""),
        ("pose_ghost_pitch", ""),
        ("pose_ghost_yaw", ""),
        ("pose_ghost_roll", ""),
        ("pose_ghost_scale", ""),
    ]),
    ("Logging and troubleshooting", "Change nothing in the game.", [
        ("log_poses", ""),
        ("log_frames", ""),
        ("log_input", ""),
        ("log_sliders", ""),
        ("log_controllers", ""),
        ("log_body", ""),
        ("log_fingers", ""),
        ("log_wrap", ""),
        ("log_body_grab", ""),
        ("log_joint_strain", "Log a held Willie's joint bends, twists and dislocation state."),
        ("joint_break", "Bending or twisting a held Willie's neck, shoulders or back past their range strains them (the old game's rules): the neck snaps, a shoulder dislocates, the back breaks."),
        ("joint_break_twist_free_neck_deg", "How far his head turns freely (degrees from where it was when you took hold) before a twist strains the neck."),
        ("joint_break_twist_free_arm_deg", "Same for a twist of his upper arm in the shoulder."),
        ("joint_break_twist_free_back_deg", "Same for a twist of his back."),
        ("joint_break_twist_gain", "A twist past the free range counts as a bend of that many times the extra degrees."),
        ("joint_break_legs", "Knees can be strained and dislocated too (calf bent against the thigh)."),
        ("joint_break_scale", "How fast joints strain (1 = the old game)."),
        ("log_body_hits", ""),
        ("stab_log", ""),
        ("hit_buzz_log", ""),
        ("whoosh_log", ""),
        ("whoosh_log_min_speed", ""),
        ("perf_log_seconds", ""),
        ("strength_log_seconds", ""),
        ("perf_events", "1 = with each Perf line, time the game's Blueprint calls per class (troubleshooting, ~1 ms a frame)."),
        ("perf_sample", "1 = with each Perf line, sample where the game thread spends its time (troubleshooting)."),
        ("perf_bp_segments", "1 = with each Perf line, time Willie's tick graph by bytecode segment (troubleshooting)."),
        ("capture_seconds", "Troubleshooting pictures every this many seconds (0 = off)."),
        ("capture_menus_only", ""),
        ("haptics_test", ""),
    ]),
    ("Developer test switches", "For unattended tests; leave alone.", [
        ("dev_fake_headset", ""),
        ("dev_fake_still", ""),
        ("dev_fake_yaw", ""),
        ("dev_fake_pitch", ""),
        ("dev_fake_scale", ""),
        ("dev_script", ""),
        ("dev_pose_probe", ""),
        ("dev_pose_bone", ""),
        ("dev_pose_pitch", ""),
        ("dev_pose_yaw", ""),
        ("dev_pose_roll", ""),
        ("dev_pose_fake_tracking", ""),
        ("dev_grip_trace", ""),
    ]),
]
UNUSED_TITLE = "No longer read by the mod"
UNUSED_NOTE = "Old settings; changing them does nothing. Safe to delete."

HEADER = """; Half Sword VR settings. Edit and save: the mod re-reads this file about once a second while the game runs.
; Format: key=value, one per line. Lines starting with ; are notes. [Section] lines are only headings.
; The most-changed settings are in [Quick settings] at the top; each setting appears once in the file.
; New settings from a newer mod version are added under the setting they belong with.
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


def build(values, keys_in_code):
    placed = [k for _, _, items in LAYOUT for k, _ in items]
    dupes = {k for k in placed if placed.count(k) > 1}
    if dupes:
        sys.exit("placed twice in LAYOUT: " + ", ".join(sorted(dupes)))
    unplaced = [k for k in keys_in_code if k not in placed]
    if unplaced:
        sys.exit("read by the mod but not in LAYOUT (add them to a section): " + ", ".join(unplaced))
    lines = [HEADER]
    for title, note, items in LAYOUT:
        present = [(k, c) for k, c in items if k in values]
        if not present:
            continue
        lines.append("\n[%s]\n" % title)
        if note:
            lines.append("; %s\n" % note)
        for k, c in present:
            if c:
                lines.append("; %s\n" % c)
            lines.append("%s=%s\n" % (k, values[k]))
    extra = [k for k in values if k not in placed]
    if extra:
        lines.append("\n[%s]\n; %s\n" % (UNUSED_TITLE, UNUSED_NOTE))
        for k in extra:
            lines.append("%s=%s\n" % (k, values[k]))
    return "".join(lines)


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
