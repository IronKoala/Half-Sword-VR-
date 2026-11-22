// Settings read from HalfSwordVR.ini next to the mod. The file is re-read when it changes,
// so values can be tuned while the game runs.
#pragma once
#include <string>

namespace hsvr
{
    struct Config
    {
        bool enabled = true;
        // vr_auto=1: VR (and everything else the mod does, test room included) only when SteamVR is
        // already running as the game starts (one of vr_auto_processes). Without it the mod stays out
        // of the way: no OpenXR (so SteamVR is never opened), no hooks, the game plays flat as if the
        // mod weren't installed. vr_auto=0: always start VR (OpenXR opens SteamVR if it is closed).
        int vr_auto = 1;
        // Comma-separated process names that count as "SteamVR is running".
        std::string vr_auto_processes = "vrserver.exe,vrmonitor.exe";
        // Older name of the same switch: require_steamvr_running=0 is the same as vr_auto=0.
        bool require_steamvr_running = true;
        // Fraction of the headset's recommended per-eye resolution the engine renders at.
        double render_scale = 0.75;
        // Size of the world relative to you. 1 = life size.
        double world_scale = 1.0;
        // Where the view sits: "head" (character's head bone), "camera" (the game's own camera).
        std::string anchor = "head";
        std::string head_bone = "head";
        // What "straight ahead" follows in first person: "aim" (where the player aims with the
        // mouse; steady) or "camera" (the game's own camera, which trails the aim and shakes).
        std::string heading = "aim";
        // Offset from the head bone to between the eyes, in cm, along the body's forward and up.
        double eye_forward_cm = 9.0;
        double eye_up_cm = 7.0;
        // Seconds of smoothing on the head anchor (0 = none). Takes the physics jitter out.
        double anchor_smoothing = 0.06;
        // Controllers and headset placed in the world from the view anchor as it will be this frame
        // (where he is now), not as it was set up for the frame before (hands shook while walking).
        bool tracking_current_anchor = true;
        // 1 = the room walking handed to his capsule is taken into the view at once, not smoothed with
        // anchor_smoothing: the capsule moves by it the same frame, so smoothing it swung the world by
        // each step (and hands placed earlier in the frame were off by the smoothing's step).
        bool anchor_room_unsmoothed = true;
        // Near clip plane of the eyes, cm. Helmet walls sit 2.3-4.6 cm from your eyes (measured on every
        // helmet mesh, helmet_see_through.md), and off to the side their depth along the view is even
        // less: at 4 cm the sides of closed helmets were cut away and you saw the world through them.
        double near_clip_cm = 1.0;
        // 0 = auto (decode sRGB unless the engine target is a float format), 1 = always decode, 2 = never.
        int gamma_mode = 0;
        // Show the game's flat picture (menus, HUD) on a floating panel.
        bool ui_panel = true;
        // 0 = always shown (the menu-panel key hides it). 1 = the panel only shows while a menu is
        // up (game paused, intro, main menu, or a mouse pointer showing); the key forces it on.
        bool ui_panel_auto = false;
        double ui_distance_m = 1.1;
        double ui_width_m = 1.25;
        double ui_height_offset_m = -0.05;
        // How solid the panel's menus and HUD are drawn: opacity = brightness x this.
        double ui_opacity_gain = 2.0;
        // Darkness of the sheet behind the panel while a menu is up (0 = none, 1 = solid).
        double ui_menu_backdrop = 0.92;
        // While a menu is up, point a controller at the panel to move the mouse pointer there; the
        // trigger clicks (held drags).
        bool ui_pointer = true;
        // Menu buttons on the controllers: left menu button = Escape (pause / close the pause menu,
        // held while held); right A held menu_continue_hold_s after a won fight = the game's "Hold G
        // to continue" (only then, so it can never surrender or bite).
        bool menu_buttons = true;
        double menu_continue_hold_s = 1.0;
        // Also use the coverage the game's UI pass leaves in the window's alpha channel. Off by
        // default: the window only has four levels of alpha, which shows as rings on soft overlays,
        // and the dark sheet behind menus already keeps dark menu parts visible.
        bool ui_use_alpha = false;
        // Show the left eye's picture (with menus) in the game window on the monitor.
        bool mirror_window = true;
        // Virtual key codes. 0x24 = Home, 0x23 = End, 0x2D = Insert.
        int recenter_key = 0x24;
        int toggle_ui_key = 0x23;
        // Switches between the view from the character's head and the game's own camera.
        int toggle_view_key = 0x2D;
        // Flips between full-resolution mode and the fallback while playing. 0x22 = Page Down.
        int toggle_mode_key = 0x22;
        // Frames of prediction added when asking the headset for a pose (-1 = adapt automatically).
        double predict_frames = -1.0;
        // A head position is matched to its picture by order. One that was taken more than this
        // many finished Presents before its picture is presented cannot belong to that picture
        // (the engine never runs further ahead) and is skipped. -1 = no check.
        int pose_max_age = 2;
        // 1 = the eyes are drawn at the headset's own resolution and menus float on a panel.
        // 0 = fallback: both eyes are squeezed through the game window (lower resolution).
        bool separate_render_target = true;
        // Picture tuning, passed on to the engine when set (-1 = leave the game's own value).
        // sharpen: 0 = none, 0.5 to 1 = crisper. screen_percentage: the share of the eye size the
        // engine draws before its upscaler brings it up to full size (100 = no upscaling).
        double sharpen = -1.0;
        double screen_percentage = -1.0;
        // 1 = keep the engine's clouds in the one mode that places them correctly for the second
        // eye (see keep_clouds_correct in dllmain.cpp). 0 = leave the game's cloud mode alone.
        bool fix_clouds = true;
        // The cloud mode used in place of a faulty one: 2 or 0 (r.VolumetricRenderTarget.Mode).
        // 2 lays the clouds over the scene with a test per full-size pixel. 0 does it from a
        // half-size picture, which lets cloud land on thin far things (a steeple, leaves) and
        // makes them flicker.
        int cloud_mode = 2;
        // How densely the game's bounce lighting (Lumen) is sampled: one light sample per this many
        // pixels (r.Lumen.ScreenProbeGather.DownsampleFactor). Half Sword uses 64, which in the
        // headset is only about 16 samples across an eye and makes far thin things flicker and
        // plants look blotchy. The mod lowers it to this value; it never raises a smaller value
        // the game chose. 32 = twice as dense, 16 = four times (costs more), 8 = eight times.
        // 0 = leave alone. The player compared 32, 16 and 8 in the headset and chose 16.
        int light_samples = 16;
        // 1 = also wipe the stand-in pictures that the game's built-in frame generation add-on
        // makes the game draw its menus on (see "stand-in window pictures" in vr_present.cpp).
        // Without it closed menus and old mouse pointers stay visible on the menu panel.
        bool wipe_standins = true;
        // 1 = write down what each menu slider is doing (value, holding the mouse, pointer over
        // it) whenever it changes. For troubleshooting; can be switched while the game runs.
        bool log_sliders = false;
        // Extra engine console commands to run, separated by semicolons (for tuning; empty = none).
        std::string extra_commands;
        // 1 = while the headset is asleep or showing something else, the game keeps running at
        // normal speed (a helper thread keeps the headset link alive). 0 = the game always waits
        // for the headset, which can make it crawl while the headset sleeps.
        bool idle_helper = true;
        // Developer switches.
        bool log_poses = false;
        // Frame-by-frame recording (like the B&S logger): every tick of his, one CSV row with the
        // headset, both controllers and their buttons, his drawn hands, head, pelvis and feet, each held
        // item and its hold, crouch and the game's own state; logs\frames-<time>.csv (Matthew 2026-11-19).
        bool log_frames = true;
        // Write "Input:" lines to the log: which window has the keyboard, Esc and menu clicks,
        // gamepad sticks, and how fast the view is turning.
        bool log_input = true;
        // Save troubleshooting pictures of the menu layer and the monitor every this many seconds
        // into Mods\HalfSwordVR\captures (0 = off).
        double capture_seconds = 0.0;
        // 1 = take those pictures only while the game shows a mouse pointer (menus), and only the
        // menu layer, so that a short visit to one menu screen is not overwritten by a fight.
        bool capture_menus_only = false;
        // Test without a headset: a pretend headset that slowly looks left and right. The two
        // eye views show side by side in the game window.
        bool dev_fake_headset = false;
        // The pretend headset holds still, looking dev_fake_yaw degrees to the left and
        // dev_fake_pitch degrees up of where it looked when the game started.
        bool dev_fake_still = false;
        double dev_fake_yaw = 0.0;
        double dev_fake_pitch = 0.0;
        // How much the pretend headset moves when it is not held still: 1 = the full slow look
        // left and right, 0.04 = about as much as a head that is trying to keep still. The
        // movement is added to dev_fake_yaw and dev_fake_pitch.
        double dev_fake_scale = 1.0;
        // Unattended tests: "seconds:command" entries separated by semicolons, counted from the
        // moment the mod is ready. A command is an engine console command. Read once at the start.
        // Example: 20:open /Game/Maps/Arenas/Map_Arena_Slums;50:r.Fog 0;56:r.Fog 1
        std::string dev_script;
        // Test room: 1 = from the first menu straight into a Free Mode arena (test_room_map) with
        // test_room_foes foes (1-10) whose AI is off - they never fight back, keep full physics,
        // fall and get up - stood behind you, and two weapon tables in front of you with one weapon
        // of every kind. Your save is copied first (save_backups\ in the mod folder) and every
        // write the game makes to it in the room is undone; leaving the room puts it all back.
        // Read at launch. 0 = normal game.
        bool test_room = false;
        // 1 = in Free Mode the fencer's dialog (the tavern's "Train") gets a Train button that takes
        // you into the test room instead of the cellar (progression's Train is not changed).
        bool test_room_button = true;
        std::string test_room_map = "/Game/Maps/Arenas/Map_Arena_Yard";
        int test_room_foes = 4;
        // The foes' tier (armour and weapons): -1 = a random tier each, 0 Beggar .. 8 Lord.
        int test_room_tier = -1;
        // The table weapons' tier: -1 = random 0-7 each, 0 Beggar .. 8 Lord.
        int test_room_weapon_tier = -1;
        // A foe out of the fight (killed, or beaten and given up) gets a new one, passive, in his place
        // test_room_respawn_seconds later, so the room keeps test_room_foes of them. 0 = no new foes.
        bool test_room_respawn = true;
        double test_room_respawn_seconds = 10.0;
        // His body is removed this many seconds after he was out (never while you hold or grab him,
        // or a weapon in your hand is stuck in him). 0 = bodies stay.
        double test_room_despawn_seconds = 7.0;
        // A crossbow on the second table and a quiver of 20 bolts on your hip: 1 = when you come in by
        // the Train button, 2 = always (also test_room=1 boots), 0 = never.
        int test_room_crossbow = 1;
        int stereo_slot_offset = 0; // 0 = find automatically

        // ---- Phase 2: controllers (read only) and the player physics test ----
        // Write the controllers' positions, buttons and sticks to the log once a second, with the
        // distance from each controller to the character's hand bone.
        bool log_controllers = true;
        std::string hand_bone_left = "hand_l";
        std::string hand_bone_right = "hand_r";
        // 1 = a short buzz on a controller each time its trigger is pulled all the way (checks vibration).
        // Read SteamVR's hand skeleton (XR_EXT_hand_tracking) for finger curls when it offers one.
        bool input_hand_tracking = true;
        bool haptics_test = false;
        // 1 = once a second, where your character's body, head and hands are.
        bool log_body = false;
        // TEST ONLY (Phase 3 probe). 1 = on your character's animation (ABP_Willie on the hidden
        // DriverSkeleton), set the end-of-graph "Modify Bone" node for dev_pose_bone to this
        // rotation (degrees, component space, replacing the animation), to check that the mod can
        // pose bones that way. The node table is logged once.
        bool dev_pose_probe = false;
        std::string dev_pose_bone = "head";
        double dev_pose_pitch = 0.0, dev_pose_yaw = 0.0, dev_pose_roll = 0.0;

        // ---- Phase 3: your body posed by you (off until tested) ----
        // 1 = your character's physics off; his head turns with the headset, his arms reach for
        // the controllers, and his body moves with you around the room. Can be flipped while
        // playing; switching back restarts nothing, so restart the fight afterwards.
        bool pose_body = false;
        bool pose_head = true;
        // His head bone is put where your headset says it is (not only turned), so the helmet keeps
        // still in front of your eyes: the animation's breathing and crouch lean no longer move it in
        // and out of view (Matthew 2026-11-19). At most pose_head_lock_max_cm from where the animation
        // has his neck reach (beyond that the neck would stretch visibly).
        bool pose_head_lock = true;
        double pose_head_lock_max_cm = 12.0;
        // ---- Torso lean (Matthew 2026-11-19: armour blocked a third of the view looking down) ----
        // The torso hangs from your neck and bends like Half Sword's own lean (LeftAlt + mouse,
        // Anim_Add_Bob_AO): spine_02..spine_05 take equal shares, neck_01/neck_02 bend back 3/8 each
        // (torso_lean_and_helmet.md). Your hips go back by as much as the bend carries the head
        // forward, so your head stays at the headset and the chest moves in, out of view.
        // Forward bend = (head pitch below the horizon - pose_lean_pitch_dead_deg) x pose_lean_pitch_gain,
        // at most pose_lean_max_deg (the game's own lean reaches 60). Sideways = head roll likewise.
        bool pose_lean = true;
        // 1 = also bend him from your head's pitch and roll (the bend above); 0 = his torso only leans to
        // where your neck actually is, as closed PR #8/#10 did (Matthew 2026-11-21: that leaning was
        // "basically perfect"; with the head-angle bend his spine was never in line with his own).
        bool pose_lean_head_angle = false;
        // Looking down, his chest leans back out of the view by up to this many cm (reached at 60 degrees
        // down, from 5), from closed PR #8/#10. 0 = off.
        double pose_lean_look_down_back_cm = 16.0;
        double pose_lean_pitch_dead_deg = 10.0;
        double pose_lean_pitch_gain = 0.7;
        double pose_lean_max_deg = 60.0;
        double pose_lean_roll_dead_deg = 5.0;
        double pose_lean_roll_gain = 0.8;
        double pose_lean_side_max_deg = 25.0;
        double pose_lean_smooth_s = 0.08;
        // A bend lowers his head; his hips can't rise above standing, so the bend is held to what your
        // headset's drop (plus this much neck slack) allows.
        double pose_lean_slack_cm = 4.0;
        // Lean first, step after (Matthew 2026-11-20): when your neck moves away from where his hips
        // stand, his torso leans to put his head there; only once it is more than pose_lean_step_radius_cm
        // away does he step: his capsule (and so his feet) comes back under your neck over
        // pose_lean_walk_catchup_s and stays with you while you walk, until it is under your neck again.
        // 0 = the old way (his hips always under your neck).
        double pose_lean_step_radius_cm = 8.0;
        // Your lean and room steps are measured this far (cm) below your head joint, turning with your head
        // (about the base of your neck), so a nod or head tilt is neither a lean nor a step. 0 = at the head joint.
        double pose_room_neck_base_cm = 12.0;
        double pose_lean_walk_catchup_s = 0.15;
        // 1 = past the radius his hips follow your neck at once (at most pose_lean_follow_speed_cms), so
        // your neck is never more than the radius from over his hips (closed PR #8/#10: 8 cm, 400 cm/s).
        // 0 = past the radius he steps all the way under you (pose_lean_walk_catchup_s).
        bool pose_lean_follow_past_radius = true;
        double pose_lean_follow_speed_cms = 400.0;
        // A lean you hold still is a new stance: once your neck has moved less than
        // pose_lean_settle_speed_cms for pose_lean_settle_s, he steps under you (0 = never; stays leaned).
        double pose_lean_settle_s = 0.8;
        // Behind his hips he leans back at most this far (cm); further back, his hips follow at once.
        double pose_lean_back_radius_cm = 6.0;
        double pose_lean_settle_speed_cms = 10.0;
        // 1 = the game's own forward bend from crouching (Root Height) and reaching for grabs (Lean
        // Forward (Grabbing)) is taken out of his animation, so only your lean bends him.
        bool pose_lean_cancel_game_bend = true;
        // 1 = his body, head and worn armour animate every frame (no update-rate skipping).
        bool pose_no_uro = true;
        // 1 = your helmet (and his head) are moved to this frame's headset pose right when the view takes
        // it, just before drawing. The body is posed from the headset pose of the frame before (the pose
        // is read when the views are set up), so a helmet following his head trailed your view by one
        // frame of head motion and slid around in front of your eyes.
        bool pose_helmet_late = true;
        // 1 = his visible body updates after his posed skeleton every frame (it copies its pose), never before.
        bool pose_mesh_after_driver = true;
        bool pose_arms = true;
        bool pose_follow_room = true;
        // 1 = while posed, your character's capsule only moves when you walk: the game's own pull
        // toward the body (and hit stumbles, dodges and leans) is undone after each of his ticks.
        bool pose_hold_capsule = true;
        // 1 = his body faces where you face: it follows your headset once you look more than
        // pose_turn_dead_zone_deg away, turning at pose_turn_speed_dps. The mouse or stick still
        // turns you as well.
        bool pose_body_turn = true;
        // 1 = while the controllers are tracked, the mouse neither turns him nor tilts his look (the
        // control pitch is held at 0); with mouse and keyboard play the mouse works as before.
        bool pose_mouse_off_with_controllers = true;
        double pose_turn_dead_zone_deg = 60.0;
        double pose_turn_speed_dps = 150.0;
        // The most your head can be turned from his hips (a neck's reach): past it the hips come
        // round with you at once.
        double pose_turn_max_deg = 85.0;
        // Spawn yaw (playtest 2026-11-22 10:04Z: helmet, chest, hips and lean 13 deg off for a whole life).
        bool pose_turn_spawn_guard = true;  // body turning never writes the last body's yaw onto a new one
        bool pose_calib_body_facing = true; // calibrate against the way the body really faces, not the control yaw
        // Controller sticks: the left stick walks (toward where you look with "head", or where your
        // body faces with "body"), the right stick turns you ("smooth", "snap" or "off").
        bool pose_stick_walk = true;
        std::string pose_move_relative = "head";
        std::string pose_stick_turn = "smooth";
        double pose_stick_turn_dps = 150.0;
        double pose_snap_turn_deg = 30.0;
        double pose_stick_dead_zone = 0.15;
        // Weapon in hand: the game's hand joint becomes a spring toward your hand (1) instead of
        // locked (0). pose_grip_strength_kg is the most it can hold up (kg weight), pose_grip_hz how
        // briskly it follows (natural frequency), pose_grip_damping the damping ratio (Chaos solves
        // drive damping against the spring's own correction each step, so 1 makes it sag ~16 cm; 0.3
        // holds as well as the game's locked grip, tests 25-26), pose_grip_radius_cm
        // the lever used for the turning strength, pose_grip_buzz the buzz strength for the gap (0 off).
        bool pose_grip_spring = true;
        // The hold from Matthew's Chasms VR Framework (replaces the spring when on): the game's hand
        // joint goes free and the mod sets the weapon's velocity and spin each tick so the point held
        // follows the controller. Its spring (N/m), damper (N s/m), most force (N), turning spring
        // (N m/rad), turning damper (N m s/rad) and most torque (N m), the framework's one-hand values
        // (turning x1.1 / x0.9 for a handle), all times pose_hold_strength (dampers by half of it past 1).
        bool pose_grip_hold = true;
        double pose_hold_strength = 1.0;
        // His stats on the hold: strength (Muscle Rate), that arm's tonus (injury, exhaustion) times
        // consciousness, and stamina scale its force and torque caps (1 = a fresh, healthy Willie).
        // Stamina at 0 leaves pose_body_stamina_floor of it; never below pose_body_strength_min.
        bool pose_body_strength = true;
        double pose_body_stamina_floor = 0.5;
        double pose_body_strength_min = 0.1;
        // Holding and swinging cost stamina through the game's own rules: the hold's force and torque
        // (capped) go into the game's per-hand burn (as its hand joint would), x Muscle Rate's factor x
        // this scale, kept from pose_stamina_gate up (the game's 10); the game then applies its swing
        // speed, fat, temperature and armour factors and regenerates as usual.
        double pose_stamina_scale = 4.0; // Matthew 2026-11-21: 12 was far too much in play (one swing emptied the bar), back to 4
        // The game keeps a hand burn only from 10 up. Its hand joint is spiky (flat runs 2026-11-21: 30%
        // of fast-swing ticks over 10), the mod's spring hold smooth (never over 10 at the same
        // swings), so with the gate VR never paid; with none its mean matches the flat game's.
        double pose_stamina_gate = 0.0;
        // A fist's burn x this, on top of the game's own fist factor (Hands Rigidity x Fisting Rate).
        double pose_stamina_fist_scale = 1.0;
        // 1 = the game's 'Lawnmower Rate' (mouse waggle spam penalty) kept at 0 for your player: with
        // Aim Vector always 0 in VR it read every fast real swing as spam (x10 stamina, x0.5 grip).
        bool pose_no_lawnmower = true;
        double pose_stamina_force_cap_n = 250.0;  // playtest 2026-11-21: clean holds p90 60 N, real swings 36-216 N, snags up to 1019 N
        double pose_stamina_torque_cap_nm = 30.0; // clean holds p90 26.5 N m, swings 8-32, snags up to 290
        // Exact hold (B&S-style): each frame the weapon gets the velocity that puts it exactly where
        // the hand is drawn at the end of this frame's physics, limited only by the hand's strength
        // (pose_hold_max_force / max_torque). 0 = the spring below.
        bool pose_hold_exact = true;
        // Tuned in the headset 2026-11-19 (flat_hs_weight_vs_hold.md): half B&S's spring so a weapon trails
        // the hand ~0.04-0.06 s (flat Half Sword's arm trails its aim by 0.1 s), and the dampers raised so a
        // heavy, long weapon (war staff 2.38 kg, balance 108 cm up) settles without bouncing (damping ratio ~1).
        double pose_hold_spring = 2500.0;
        double pose_hold_damper = 150.0;
        double pose_hold_max_force = 3000.0;
        // While the held weapon is stuck in a body, the hold's force and torque caps are scaled by this.
        double pose_hold_stuck_force_scale = 0.15;
        // In a wound (the weapon Stuck In Body): the blade slides along it at most this fast in and
        // out (cm/s; B&S's held dampers with the hand's strength), x stab_wound_head_factor in the
        // head and neck; sideways and in turn it goes with the body.
        bool stab_wound_hold = true;
        // A weapon the mod holds is never left "Being Rubber Banded" by the game (it then can't stick and hits at x0.1).
        bool stab_no_rubber_band = true;
        double stab_pickup_grace_s = 0.3; // the game's 2 s "Just Picked Up" (no sticking) cut to this; < 0 = the game's
        // A body with the weapon stuck in it is dragged by the hand (all its strength x
        // stab_drag_force_scale across the wound) instead of the hand letting go; the weapon slips
        // only after pulling at full strength with the grip past reach for stab_drag_grip_s x his
        // strength. A yank (pull-out) counts only the hand's distance out along the wound.
        bool stab_drag = true;
        double stab_drag_force_scale = 1.0;
        double stab_drag_grip_s = 2.0; // (unused since the arm's-length pull-out)
        double stab_drag_reach_slack_cm = 8.0; // a stuck thrust past arm's length by this much ...
        double stab_drag_reach_s = 0.15;       // ... this long comes out of the body (stays in the hand)
        // Every other hit of a held weapon on a person gets his arm's mass behind it (kg, times his
        // strength), at the speed the struck point closes on the surface (from hit_assist_min_cms).
        bool hit_assist = true;
        double hit_arm_mass_kg = 3.0;
        double hit_assist_min_cms = 150.0;
        // A lodged cut (not a thrust) is let go once the hand is cut_release_cm past it.
        bool cut_follow_through = true;
        double cut_release_cm = 10.0;
        double cut_release_after_s = 0.03;
        // Reverse grip flip (S&S): a tap (shorter than grip_flip_tap_s) of the holding hand's B / Y
        // turns a one-handed weapon end for end at the same spot on its handle, gliding over grip_flip_s.
        bool grip_flip = true;
        bool grip_flip_short_only = true; // only daggers, short swords and one-handed swords flip (Matthew 2026-11-21)
        // A blade stuck in someone is torn out when your hand is more than stab_yank_cm from its grip
        // for stab_yank_s (0 = never).
        double stab_yank_cm = 35.0;
        double stab_yank_s = 0.25;
        double grip_flip_s = 0.4; // Matthew 2026-11-21: 0.3 flipped too fast, 25% slower
        double grip_flip_tap_s = 0.4;
        double stab_wound_settle_s = 0.2; // no pulling out for this long after it goes in
        double stab_wound_settle_max_s = 0.8; // a young stick is held open until the game's own keep trace passes, at most this long
        double stab_wound_entry_cms = 70.0; // ...and it is carried in at this speed meanwhile (8 cm over 0.2 s)
        // A blade the game has just stuck is set this far into the wound along its tip (cm), plus
        // stab_start_depth_per_ms cm for each cm/s of the controller along it, at most _max_cm
        // (the game's stuck joint lets the blade slide 5 cm along a thrust).
        double stab_start_depth_cm = 0.0; // 0 = off (stab-test11: a teleport into the wound made it let go)
        double stab_start_depth_per_ms = 0.01;
        double stab_start_depth_max_cm = 4.5;
        double stab_wound_in_cms = 120.0; // (was 50: daggers needed pushing and pushing, playtest 2026-11-20)
        double stab_wound_head_in_factor = 0.6;
        double stab_wound_out_cms = 60.0;
        double stab_wound_head_factor = 0.6;
        double pose_hold_turn_spring = 1100.0;
        double pose_hold_turn_damper = 75.0;
        // One hand's most turning force at the grip (N m, x his strength): a real wrist, not B&S's 250. A war
        // staff held by its end (29 N m of weight) droops; one-handed swords (2-4 N m, hard swings ~20) stay free.
        double pose_hold_max_torque = 30.0;
        // The wrist's end stop: once the held item is bent more than pose_hold_wrist_free_deg from where
        // the hand points it, the turning limit rises to pose_hold_wrist_stop_torque (N m, times his
        // strength) by pose_hold_wrist_stop_deg, so a stopped swing does not carry the item on past the
        // hand while a long weapon held at its end still droops. 0 = no stop.
        double pose_hold_wrist_free_deg = 25.0;
        double pose_hold_wrist_stop_deg = 50.0;
        double pose_hold_wrist_stop_torque = 250.0;
        // The wrist stop pushes back only against the weapon turning on past the hand, fully from this spin
        // (rad/s) away, so it stops a swing's overshoot but a held weapon's own weight still sags it slowly
        // down to where the wrist's turning limit carries it (Matthew 2026-11-20: a pole hammer held out by
        // its end barely sagged for a weak Willie). 0 = it also holds still weight, as before.
        double pose_hold_wrist_stop_spin = 2.0;
        // His strength on the turning limits (wrist and stop) is raised to this power: a wrist is weaker in
        // proportion than the whole arm (min/min Willie x0.58 -> x0.34, biggest x1.27 -> x1.6). 1 = as the force.
        double pose_hold_torque_strength_power = 2.0; // below a default body only: above it, x strength as the force
        // B&S hold dampers computed per weapon for this damping ratio (1 = critical: settles without
        // bouncing, the least lag that does): from its mass and its inertia about the grip. 0 = the fixed
        // pose_hold_damper / turn_damper / pose_bs_two_*_c values.
        double pose_hold_damping_ratio = 1.0;
        // The hand's velocity the hold's dampers follow, smoothed over this time (s): tracking noise
        // differentiated frame to frame shook a held weapon ~1 cm at ~10 Hz. 0 = raw.
        double pose_hold_target_vel_smooth_s = 0.05;
        // Weight: the hold's turn is limited by torque about the grip, not the centre of mass (heavy and
        // long weapons trail fast turns, B&S-style). torque_scale multiplies pose_hold_max_torque for it.
        bool pose_hold_weight = true;
        double pose_hold_weight_torque_scale = 1.0;
        // Two hands turn a weapon by pushing the hands opposite ways: that couple (the second hand's
        // force x their distance apart on it) adds to the torque cap (playtest 09:38: a 2.2 kg axe in
        // two hands needed 550-930 N m against a 486 N m wrist cap and shook).
        bool pose_hold_weight_two_hand_couple = true;
        // B&S's hold (bs_hold_measured.md, from Matthew's B&S session 2026-11-19): soft drives instead
        // of the exact hold. One hand: pose_hold_spring/damper/max_force and turn_spring/damper/
        // max_torque (5000/100/3000, 1100/45/250). Two hands: both hands' points 10000/150/6000, only
        // the first hand turns it (2200/90/500), the second has no turning. Solved per frame as soft
        // constraints (pose_bs_iterations), all hands together.
        bool pose_hold_bs = true;
        double pose_bs_two_pos_k = 4000.0, pose_bs_two_pos_c = 200.0, pose_bs_two_pos_max = 6000.0;
        double pose_bs_two_rot_k = 2200.0, pose_bs_two_rot_c = 120.0, pose_bs_two_rot_max = 500.0;
        int pose_bs_iterations = 8;
        double pose_bs_two_let_go_cm = 45.0; // B&S gaps reach 18 cm (p95) on fast two-hand swings
        // A worn helmet's collision is not part of his body mesh: the game welds it onto CM_Helmet, which
        // pose_hold_ignore_body doesn't cover. On: the held weapon ignores that too (his own only).
        bool pose_hold_ignore_own_helmet = true;
        bool hold_blocker_log = false; // test: name what a held weapon hits, and what is along it while it is held off
        bool pose_hold_ignore_body = true;   // the held weapon never collides with his own body (B&S)
        double pose_bs_max_spin = 60.0; // rad/s
        // The weight's own swing never spins it faster than the turn wanted plus this (rad/s): one tick
        // of a deadbeat grip pull fed back into the lever spun the axe to the 60 rad/s cap (playtest 09:38).
        double pose_hold_weight_spin_extra = 5.0;
        // While held, his visible hand sits on the weapon (1): when the weapon is stopped by something,
        // the hand stays on it instead of going on with your controller. It eases from following the
        // controller to sitting on the weapon as the weapon falls 1 to pose_hold_blocked_from_cm behind.
        bool pose_hold_hand_on_weapon = true;
        // The elbow is chosen for his drawn hand (on a stopped weapon: the weapon's turn), as B&S's IK does.
        bool pose_arm_solve_drawn_hand = true;
        double pose_hold_blocked_from_cm = 3.0;
        // B&S (bs_hold_measured.md section 5): while a hand holds something, his drawn hand always rides
        // the item at its grip (where the item will be after this frame's physics), never your controller;
        // the hold's springs pull the item toward the controller. So the item never slips, sits to the
        // side of the hand or jitters in it (Matthew 2026-11-19). 0 = the hand follows the controller
        // and only goes onto a stopped weapon (pose_hold_blocked_from_cm).
        bool pose_hold_hand_follows = true;
        // How the drawn hand predicts where the item will be after this frame's physics: 1 = from how it
        // actually moved last frame (at most pose_hold_follow_predict_max_cm ahead), 2 = from the
        // physics velocity (jumped when the item was blocked), 0 = none (a frame behind the item).
        int pose_hold_hand_follows_predict = 1;
        double pose_hold_follow_predict_max_cm = 12.0;
        double pose_hold_follow_predict_smooth_s = 0.03; // smoothing of that measured velocity (0 = none)
        // The stuck rule: a stopped weapon whose grip is out of his reach from the shoulder, or more
        // than an arm's length from your hand, for pose_hold_let_go_s, is let go (the game's own drop).
        // A weapon stopped by his own body is never let go by the stuck rule (vanilla weapons collide with him too).
        bool pose_hold_let_go_ignore_self = true;
        bool pose_hold_let_go = true;
        double pose_hold_let_go_s = 0.15;
        double pose_hold_let_go_reach_slack_cm = 15.0; // with the arm-lag hold: how far past his reach the grip may trail
        // No let-go for this long after a weapon comes into the hand (long weapons swing into place).
        double pose_hold_let_go_grace_s = 1.0;
        // Stuck let-go drops the weapon dead (no velocity) instead of with the hold's last chase
        // velocity (playtest 2026-11-21: stopped weapons left at 10-20 m/s and flew at him). 0 = old.
        bool pose_hold_let_go_dead = true;
        // While a held weapon is stopped, the hold uses its measured motion, not the velocity it last
        // set (which never drops while it is blocked, so the hold wound up to 20 m/s). 0 = old.
        bool pose_hold_measured_v = true;
        // Experimental (hold rebuild, Matthew 2026-11-21): the held weapon is held by engine joints, B&S
        // style. Each hand gets an invisible kinematic body that follows the controller; a Chaos joint
        // with spring drives ties it to the weapon and the physics solves it with the collisions. One
        // hand: pose_hold_spring/damper/max_force and turn_spring/damper/max_torque; two hands: each
        // hand pulls (pose_bs_two_pos_*), only the main hand turns (pose_bs_two_rot_*). Caps scale with
        // strength/stamina/injury. Not used while drawing, sliding or stuck in a wound. 0 = the old hold.
        bool pose_hold_joint = true;
        double pose_hold_joint_stiffness = 1.0; // multiplies the springs
        double pose_hold_joint_damping = 1.0;   // multiplies the dampers (the lag in a steady swing is about damper / spring x speed)
        double pose_hold_joint_max = 1.0;       // multiplies the force and torque caps
        // The shoulder's limit on each hand's pull (the arm-lag decision 2026-11-20: weight shows as the arm
        // being slow), as the old hold; it eases off to B&S's cap when the grip is far behind. 0 = B&S caps only.
        bool pose_hold_joint_shoulder = true;
        // The arm carries the weapon's weight at the grip outside that cap (as the old hold). 0 = the cap pays it.
        bool pose_hold_joint_carry_weight = true;
        // One hand: the joint's turn no stronger than the hand's pull can hold the grip still against.
        bool pose_hold_joint_turn_by_grip = true;
        // A joint-held weapon never goes faster than the old hold sets (2000 cm/s, pose_bs_max_spin).
        bool pose_hold_joint_speed_limit = true;
        double pose_hold_joint_fade_min_speed = 50.0; // cm/s: below it a grip far behind is stopped by something, no ease to B&S's cap
        double pose_hold_joint_fade_keep_s = 0.4; // s: once allowed, the ease to B&S's cap stays allowed this long (so it brakes what it started)
        double pose_hold_joint_fade_after_s = 0.0;  // s: no ease to B&S's cap this soon after a joint is made (was 0.3 before the grab's speed was zeroed)
        bool pose_hold_joint_slerp = true; // 1 = the joint turns the weapon by one SLERP drive (0 = twist and swing drives)
        bool pose_hold_joint_mod_turn = true; // one hand: 1 = the old hold's turn (spin set each tick), the joint only pulls; 0 = the joint's turn drive
        // Two hands: the turning hand only twists the weapon about the line between the hands; where it
        // points is the two hands' positions (a real stick). pose_two_hand_swing_share = how much of the
        // turning hand's pull on its direction is kept (0 = none, 1 = old behaviour).
        bool pose_two_hand_twist_only = true;
        bool pose_two_hand_joint_both_turn = false; // joint hold, both hands on fixed grips: both wrists turn it (test)
        double pose_two_hand_swing_share = 0.0;
        // Whoosh judged on the weapon's speed against your body: the game's whoosh threshold for a weapon in
        // a tracked hand is raised by your walking speed x this (0 = off).
        double whoosh_walk_speed_scale = 1.0;
        // Something is in the held weapon's way when it ends a tick this far (cm) short of where the
        // hold sent it; the stuck rule only lets go of a weapon that is in contact like that.
        double pose_hold_contact_cm = 0.5;
        // The held weapon never collides with the hand holding it (as with the forearm).
        bool pose_hold_ignore_hand = true;
        // Keeps the game's off-hand search ("R/L Offhand Searching Grip") off while the mod holds a
        // weapon; its re-seat shoved longswords about once a second.
        bool pose_hold_block_offhand_search = true;
        // The slide: hold the trigger while holding a weapon and that hand slides along it, within its
        // grip zone (stops half a hand inside the guard and the pommel end). Rate (1/s) at a light and
        // a full trigger squeeze.
        bool pose_slide = true;
        double pose_slide_hand_half_cm = 4.5;
        // The zone is measured from the weapon's real parts: walking along its axis from the grip, it
        // ends where the weapon ends or gets wider (radius) than a hand slides past (a guard, a
        // polearm's hook or head, a wide pommel). 0 = the game's lengths and its Head module only.
        bool pose_slide_zone_measured = true;
        double pose_slide_wide_cm = 4.0;
        // When his arm can't reach where your hand is (more than grab_controller_gap_cm short: reaching to the
        // floor), a squeeze looks for items at your controller, and what it touches is pulled into his hand.
        bool pose_grab_from_controller = true;
        double pose_grab_controller_gap_cm = 4.0;
        double pose_grab_controller_max_cm = 60.0; // not from farther than this (a jump, a teleport)
        // A belt scabbard is a handle (B&S): taken only within pose_grab_handle_cm of its length, lined up
        // with the hand's grip line, the trigger slides the hand along it. 0 = held where touched, as before.
        bool pose_scabbard_handle = true;
        // B&S: a weapon or tool is taken only by a handle (its grip, any haft stretch, a sword's blade), not
        // anywhere the hand touched it (shields and bottles still are). 0 = anywhere, as before.
        bool pose_grab_weapon_handles_only = true;
        // The hold's force and torque caps x his arm mass against a default creator body's (height and
        // weight/muscle; 0.78 smallest .. 1.27 biggest), as flat Half Sword's weapon-to-arm mass ratio.
        bool pose_body_size_strength = true;
        // Arm-inertia lag, like flat Half Sword's arm (Matthew 2026-11-20, option A: a weak Willie should
        // feel weight and lag): the hold drives his arm and the weapon together, as flat Half Sword's arm
        // motors do (same pull for every body, so the weapon's share of arm + weapon inertia about the
        // shoulder sets how much it slows him). Empty-arm follow delay pose_hold_arm_lag_s (flat: 0.1 s),
        // x sqrt((arm + weapon) / arm), x 1/sqrt(the weaker of injury and stamina), never past
        // pose_hold_arm_lag_max_s. The weapon's weight at the grip is carried (no sag of the hand), its
        // lever is not. Replaces the B&S springs and dampers; the caps stay. pose_hold_arm_reach_cm: the
        // grip's distance from the shoulder for a default body (scaled with his size).
        bool pose_hold_arm_lag = true;
        double pose_hold_arm_lag_s = 0.05;
        double pose_hold_arm_lag_max_s = 0.25;
        // A heavy weapon's own heft, whoever holds it (Matthew 2026-11-21: a max-strength "GigaChad" Willie swung a
        // greatsword like a dagger): the lag is never below this x sqrt(weapon inertia about the shoulder per arm,
        // in kg m^2). 0.11: greatsword ~0.10 s two-handed / 0.14 one-handed, arming sword ~0.08, dagger ~0.05. 0 = off.
        double pose_hold_heft_lag_s = 0.11;
        double pose_hold_arm_reach_cm = 55.0;
        // His shoulder's strength (N m, for a default fresh body; x his strength, injury and stamina; both arms
        // add): the arm-lag hold can't speed arm + weapon about the shoulder faster than this allows, so the
        // grip's pull on the weapon is capped at its mass x this torque x reach / (arm + weapon inertia).
        // A two-hander in one hand trails and won't whip round like a one-hander (Matthew 2026-11-20). 0 = off.
        double pose_hold_shoulder_torque = 110.0;
        // The shoulder limit fades out as the grip trails the hand from shoulder_fade_from_cm to shoulder_fade_to_cm
        // (the hand is on the weapon: it can't trail far), so a big jump or a hard swing never trails it far
        // enough for the stuck rule to let go (unattended test: a longsword 53 cm behind was dropped).
        double pose_hold_shoulder_fade_from_cm = 15.0;
        double pose_hold_shoulder_fade_to_cm = 35.0;
        // Thin bits on a haft the hand slides over: a wide stretch no longer than ring_max_len_cm along the
        // haft and reaching no more than ring_max_width_cm from its axis, or a gap between parts no longer
        // than gap_max_cm, with narrow haft past it (ring_max_len_cm 0 = every wide bit stops the hand).
        // rondel_stops: a Guard part on a haft (a pollaxe's rondel) stops the hand. butt_is_haft: below the
        // grip, parts that carry on toward the game's own bottom length (a pollaxe's butt) count as haft.
        double pose_slide_ring_max_len_cm = 2.5; // measured: war staff lug 2 cm; rondel and sword guards 6 cm
        double pose_slide_ring_max_width_cm = 10.5; // war staff lug 10 cm; sword guards 11-12 cm
        double pose_slide_gap_max_cm = 4.0;
        bool pose_slide_rondel_stops = true;
        bool pose_slide_butt_is_haft = true;
        // A one-handed sword (its grip module's game grip type, "Grip R Hand Default", is not one of the
        // game's two-hand types 10-16) is held at one fixed spot on its grip, the game's own grip point:
        // no trigger slide along the grip (Matthew 2026-11-20). Hand-and-a-half and two-handed swords, and
        // every non-sword, keep the slide. The blade (half-swording) is unchanged.
        bool pose_slide_fixed_1h_sword = true;
        // Two fixed grip zones, under the guard and above the pommel, no slide (Matthew 2026-11-21):
        // 0 = off (grip slides), 1 = hand-and-a-half (bastard sword, Grip_15) only, 2 = every two-handed
        // sword (also longswords and greatswords).
        int pose_grip_two_zones_2h_sword = 1; // Matthew 2026-11-21: bastard swords only
        // The lower zone sits just above the pommel (Matthew 2026-11-22: "you grab just above the pommel"): the
        // pommel's top is this far above its origin (measured 4.7 on a Grip_15 pommel, 8 cm long).
        double pose_grip_pommel_top_cm = 4.7;
        // Daggers (no grip module, dagger or rondel icon): one fixed grip zone, no slide.
        bool pose_grip_dagger_fixed = true;
        double pose_slide_rate_min = 3.0;
        double pose_slide_rate_max = 40.0;
        double pose_slide_max_cm_s = 250.0; // fastest the hand runs along a weapon
        // The grab button: squeeze a grip near a loose weapon to pick it up; let go of the grip to
        // drop what that hand holds (pose_grab_release_drops).
        bool pose_grab_button = true;
        double pose_grab_radius_cm = 18.0;
        // The handle system (B&S-style grab rebuild): a hand takes only what it touches (its palm
        // within pose_grab_touch_cm of the item's real collision), on the nearest handle within
        // pose_grab_handle_cm (lined up with the hand's grip line), else where it touched. Both hands
        // work the same: the other hand's weapon is taken the same way. Sliding along a handle keeps
        // pose_slide_keep_light (light trigger) .. pose_slide_keep_full (full) of the item's own
        // motion along it. 0 = the old grab (radius, game grips, zones).
        bool pose_grab_handles = true;
        double pose_grab_touch_cm = 10.0; // B&S fight session: ~6 cm typical, up to 25 cm from the handle surface (bs_full_measured.md)
        double pose_grab_handle_cm = 10.0;
        double pose_slide_keep_light = 0.6;
        double pose_slide_keep_full = 0.97;
        // B&S slide damper along the handle (N s/m) at a light .. full trigger: the item keeps
        // exp(-c dt / mass) of its motion along it each frame (replaces keep_light/full; 0 = use those).
        double pose_slide_damper_light = 300.0;
        double pose_slide_damper_full = 20.0;
        // B&S's slide along the handle (Handle.cs UpdateSliding): spring 0, damper 300 -> 20 N s/m over trigger
        // 0.2 -> 1, so gravity pulls an upright weapon through the hand and a level one is dragged along with it.
        // 0 = the old free axis (no damper, and the arm also carried its weight along the handle, so it never fell).
        bool pose_slide_bs_damper = true;
        // Draw: the scabbard seat's SetConstraintReferencePosition call with parameter names that match the
        // engine (the old names may write nothing). Off by default: draws feel as they do now.
        bool draw_seat_ref_fix = false;
        // A grab leaves the item where it was and the hold pulls it into the hand over this long (B&S);
        // also a hand-over. 0 = put it in the hand at once.
        double pose_grab_settle_s = 0.08;
        // Shields: every measured strap and handle is a grab point; anywhere else (the rim, an edge)
        // the hand holds it where it touched (pose_shield_free_grip; 0 = always the game's grip).
        bool pose_shield_free_grip = true;
        // Weapons: taken farther than pose_weapon_free_cm from their grip, held where the hand closed
        // (pose_weapon_free_grip; 0 = snapped onto the grip, the game's way).
        bool pose_weapon_free_grip = true;
        double pose_weapon_free_cm = 6.0;
        // How near a strap or handle the hand must close to take the shield by it (cm).
        double pose_shield_point_cm = 8.0;
        // A strap shield the game puts on the forearm (the round's own, or taken by its forearm strap)
        // is held in the hand instead, by the fitting nearest the hand (Matthew 2026-11-19: "In the
        // hand, like ones you pick up"). Fittings held in the hand are turned so the strap or handle
        // runs along the hand's grip line, and that hand's fingers take this grip type (2 = the
        // game's sword grip, curled; -1 = leave the game's).
        bool pose_shield_in_hand = true;
        double pose_shield_hand_grip_type = 2.0;
        // Shield straps and handles are fixed grips (Matthew 2026-11-20: "the grips, they should be
        // fixed... I shouldn't be able to slide up and down"): the hand always holds one at its middle,
        // at whatever angle round it the hand came in, and the trigger never slides it. 0 = slide.
        bool pose_shield_fixed_grip = true;
        // Away from its straps and handles a shield is taken only by its rim (the hand within
        // pose_shield_rim_cm of an outer edge), never by the flat front or back (Matthew 2026-11-20:
        // "I can also grab the shields anywhere... the flat front... I shouldn't be able to").
        // pose_shield_grab_rim 0 = straps and handles only; pose_shield_grab_face 1 = anywhere, as before.
        bool pose_shield_grab_rim = true;
        double pose_shield_rim_cm = 4.0;
        bool pose_shield_grab_face = false;
        // Pavises start locked to the forearm like a real pavise (Matthew 2026-11-20): the game's own
        // forearm strap grip is kept (not moved into the hand) where the game has one for that hand
        // (light pavise both hands, heavy pavise left hand). Overrides pose_shield_in_hand for pavises.
        bool pose_pavise_forearm = true;
        bool pose_grab_release_drops = true;
        // Squeezing near a loose object or a body (no weapon in reach) grabs it through the game's own grab.
        bool pose_grab_objects = true;
        // Fixed scenery (a fence, a table, a wall: anything that blocks and does not move) can be held:
        // his drawn hand stays put on it with the fingers wrapped round it until you let go or pull your
        // hand pose_grab_ride_max_cm away from it.
        bool pose_grab_world = true;
        // Two hands on one weapon (B&S rules): squeeze the free hand on a weapon the other hand holds,
        // on its grip or anywhere along its blade (half-swording, Matthew 2026-11-18). The weapon then
        // lies along the line between the hands, the first hand sets its roll, and the hold is
        // pose_two_hand_strength times as strong. The blade zone starts pose_two_hand_blade_gap_cm
        // past the guard. A controller pulled more than pose_two_hand_let_go_cm off its spot lets go.
        bool pose_two_hand = true;
        double pose_two_hand_strength = 2.0;
        double pose_two_hand_blade_gap_cm = 3.0;
        double pose_two_hand_reach_cm = 6.0; // how far from the zones a squeeze still lands on them
        double pose_two_hand_let_go_cm = 25.0;
        // The second hand's fingers close (its grip type set to the sword grip while it holds). Off:
        // the game then drops the first hand's two-handed grip type (11 -> 2) and with it its two-handed
        // rules (Master Stroke, the two-hand stamina factor, ...; fists_eating.md).
        bool pose_two_hand_curl = false;
        // Finger shapes go only to the animation's copy of each hand's grip type (written right after
        // ABP_Willie's update), never to the pawn's, so the game's two-handed rules and Set Up never see them.
        bool pose_fingers_anim_only = true;
        // Write the body pose inside his animation update (right before it is evaluated) rather than at the
        // start of the engine tick (the drawn hand on a held item showed the previous frame's solve).
        bool pose_in_anim_update = true;
        // His posed skeleton and visible body tick after the physics step (with pose_in_anim_update), so a
        // drawn hand riding a held item sits on it where it is drawn this frame (no guess ahead).
        bool pose_post_physics = true;
        // The capsule's height as his body and the view use it, smoothed over this time (s): walking, the
        // game's capsule hops ~2 cm about once a second. 0 = the capsule's own height.
        double pose_actor_z_smooth_s = 0.08;
        // The game's swing speed (Aim Swing Speed) fed from the controllers: the faster hand that holds
        // something, lo..hi m/s relative to the body -> 0..75; falls with this time constant.
        bool pose_feed_swing_speed = true;
        // A weapon grabbed by its grip or blade sits at the game's own seat in the hand (the nearest of
        // it, edge turned, blade reversed), its handle through the seat's point (B&S fixed orientations).
        bool pose_grab_seat = true;
        // Throwing (B&S): a weapon let go while the hand moves at least pose_throw_speed m/s is thrown the
        // game's way (its hits count) and its velocity multiplied by pose_throw_multiplier (B&S 2.0).
        bool pose_throw = true;
        // Holster snap (B&S Holder.cs:683-695), only for weapons with no scabbard of their own (Matthew
        // 2026-11-19; scabbard weapons keep the physical sheathe): let go of one while your hand is within
        // pose_holster_zone_cm of the free belt slot it goes in (the one the game's Assign picks) and moving
        // slower than pose_holster_max_speed m/s (faster is a throw), and the game hangs it there.
        bool pose_holster_snap = true;
        // A drawn weapon dropped, thrown or passed to the other hand leaves its scabbard on the belt (B&S);
        // the game's own release takes it off (Matthew 2026-11-19: "the scabbard despawns").
        bool pose_keep_scabbard_on_drop = true;
        // ---- Crossbow (Matthew 2026-11-22: two grips, pull the string to cock, a bolt into the
        // cradle, trigger fires). The game's own string joint, bolt loading and Event Shoot do the work.
        bool crossbow = true;
        // A free hand closing this near the string's nut (cm) takes the string (when nearer it than a grip).
        // The free hand's trigger pulled this near the string's nut (cm) takes the string (grip takes the
        // fore end; Matthew 2026-11-22).
        double crossbow_string_reach_cm = 10.0;
        // A hand closing this near the quiver at his hip (cm, from its collision) takes a bolt out.
        double crossbow_quiver_reach_cm = 12.0;
        // A held bolt this near the cradle's loaded spot (cm) is loaded (the game's own rule is 10).
        double crossbow_bolt_seat_cm = 10.0;
        // The trigger fires at this pull (0-1) and must come back under crossbow_trigger_release first.
        double crossbow_trigger_fire = 0.75;
        double crossbow_trigger_release = 0.3;
        // The hand on the rear grip (by the trigger) is the one that fires: within this of it (cm).
        double crossbow_trigger_grip_cm = 8.0;
        double pose_holster_zone_cm = 15.0;
        double pose_holster_max_speed = 3.0;
        double pose_throw_speed = 2.0;
        double pose_throw_multiplier = 2.0;
        // Drawing from a scabbard: a squeeze within this distance of a sheathed weapon's grip draws it
        // (pulled out along the scabbard; the game releases it at 85%), eased into the hand over settle_s.
        bool pose_draw = true;
        double pose_draw_touch_cm = 10.0;
        // A weapon WITHOUT a scabbard (axe, mace, hammer on the belt) or a shield on the back is taken off
        // the body B&S-style (its HolderHips/HolderBack: playerGrabFromHandle false, the holder's touch
        // zone, Interactable.touchRadius 0.1 m): a squeeze within this distance of any part of it takes it,
        // and the hand lands on its nearest handle (Matthew 2026-11-21: "I can't grab weapons off my hip").
        double pose_draw_zone_cm = 10.0;
        double pose_draw_settle_s = 0.15;
        double pose_draw_step_cm = 6.0; // the most it moves along the scabbard per tick
        double pose_draw_force_scale = 0.3; // the hold's force/torque caps while drawing or sheathing
        // With the drawn hand on the weapon during a draw/sheathe too (pose_hold_hand_follows_draw), the
        // game's hand joint no longer pulls the blade along (it follows the hand, which is on the blade),
        // so the hold pulls with this share of its strength instead (test b3: 0.3 alone took 6.5 s).
        bool pose_hold_hand_follows_draw = true;
        double pose_draw_force_scale_follows = 1.0;
        bool pose_draw_scabbard_free = false; // the game's loose scabbard pin while drawing (else locked)
        // Sheathing: a held (scabbarded) weapon's tip within mouth_cm of its side's empty belt scabbard
        // mouth, its blade within angle_deg of the scabbard's line, goes in; the game seats it and lets go.
        bool pose_sheathe = true;
        double pose_sheathe_mouth_cm = 10.0;
        double pose_sheathe_angle_deg = 30.0;
        bool pose_sheathe_cross = true; // a weapon whose scabbard hangs on the other hip goes back across (the game's cross path)
        // The off hand on a belt scabbard: a squeeze within touch_cm of it takes it; it turns with the hand
        // up to angle_deg on the belt (the game's own loose limit is 30) and stays pinned there; let go and it
        // turns back. follow_s: how fast it catches up with the hand's angle. let_go_cm: the hand pulled this
        // far off it for 0.3 s lets go. block_auto: keep the game's own teleporting off-hand grab from firing.
        bool pose_scabbard_grab = true;
        double pose_scabbard_touch_cm = 8.0;
        double pose_scabbard_angle_deg = 30.0;
        double pose_scabbard_follow_s = 0.015; // ~1 tick: it carries a sheathed sword and the soft cone (0.05 left 11-18 deg)
        double pose_scabbard_let_go_cm = 25.0;
        double pose_scabbard_grip_type = 2.0;
        bool pose_scabbard_block_auto = true;
        double pose_grab_seat_reverse_bias_deg = 45.0; // a reversed seat must be this much nearer to win
        double pose_swing_speed_lo = 1.5;
        double pose_swing_speed_hi = 7.0;
        double pose_swing_speed_fall_s = 0.15;
        // The second hand turns with the weapon (locked onto it as the first hand holds it, mirrored)
        // instead of with your controller.
        bool pose_two_hand_lock_rotation = true;
        int pose_two_hand_mirror = 1; // which weapon axis the second hand mirrors across (0 = X, 1 = Y)
        // Fists: squeezing an empty grip with nothing in reach makes that hand a fist (the game's
        // Weapon_Fists_C); letting go opens it. pose_fist_class = its object path if it is not loaded.
        bool pose_fists = true;
        std::string pose_fist_class = "/Game/Assets/Weapons/Blueprints/Built_Weapons/Weapon_Fists.Weapon_Fists_C";
        // Set the game's "Fist R/L Spawned" on a fist the mod makes (so the game's own removal rules
        // apply to it). 0 = the mod alone opens it (test switch while the first-fist loss is hunted).
        bool pose_fist_mark_spawned = false;
        // Fists from your real hand (Matthew 2026-11-19): the hand is a fist while the grip, the trigger
        // (and the thumb, only if pose_fist_thumb_on > 0) are closed, and opens
        // when any of them opens. Finger curls (0 open .. 1 closed) come from SteamVR's hand skeleton
        // (input_hand_tracking, XR_EXT_hand_tracking) when it gives one, else from the trigger, grip and
        // touch sensors. index/grip/thumb_on: curls that close it; *_off: below these it opens.
        // 0 = the old way (a squeeze with nothing in reach).
        bool pose_fist_from_fingers = true;
        double pose_fist_index_on = 0.65;
        double pose_fist_index_off = 0.4;
        double pose_fist_grip_on = 0.65;
        double pose_fist_grip_off = 0.4;
        // The thumb does not gate the fist (Matthew 2026-11-19: stick and button touch are unreliable);
        // raise thumb_on above 0 to require it.
        double pose_fist_thumb_on = 0.0;
        double pose_fist_thumb_off = -1.0;
        // Slaps: an open hand swung at the game's fist speed (Aim Swing Speed >= slap_ass, the game's own
        // 30) carries the fist (fingers open) until keep_s after it slows.
        bool pose_fist_slap = true;
        double pose_fist_slap_ass = 30.0;
        double pose_fist_slap_keep_s = 0.3;
        // No fist while the hand is inside another Willie (playtest 2026-11-20 588 s: a fist made at his
        // neck as a throat grab let go threw his head and neck at 37-87 m/s). The fist waits until its
        // ball (radius measured from the live fist, 13 cm until then) plus margin_cm is clear of every
        // other Willie's physics bodies, checked every pose_fist_clear_retry_s; the fingers still close.
        bool pose_fist_clear = true;
        double pose_fist_clear_margin_cm = 2.0;
        double pose_fist_clear_retry_s = 0.03;
        // A fist blow's assisted impulse (hit_assist) goes into the victim's damage as is. The game
        // scales a sliding contact by the weapon's cutting rate, ~0 for a fist ball, so without this
        // a hard punch reached his damage as impulse ~1 (playtest 2026-11-22).
        bool pose_fist_full_impulse = true;
        // Grabbing a weapon someone else holds: a tug of war under the game's own grip rule; his hand
        // lets go when its joint is pulled harder than his grip strength T x disarm_grip_scale (pain
        // weakens it, out cold he has none) for disarm_hold_s, and it is yours.
        bool disarm = true;
        double disarm_grip_scale = 3.0;
        double disarm_hold_s = 0.25;
        // The fist's finger shape in his animation: -2 = the game's own, -1 = the animation's grab
        // shape (its 'Grabbed' switch), 0..17 = that grip type, 100 = its guard fist ('Guarding' for
        // that hand, with the swing speed its fist blend reads held at 75 in the animation only).
        // Unattended test 2026-11-19 (fingertips to the hand bone, four-finger mean): open/-2/0 14.8 cm,
        // -1 10.7, 1 9.0 (tightest), 5 and 9 9.1; 100 did not hold (one sample, then open).
        int pose_fist_shape = 1;
        // The game's own fist flags: 'Fist R/L Spawned' is hidden from Willie's tick (whose removal
        // rule needs 'Hand Active', always 0 in VR) and set after it while the mod's fist exists (his
        // punch rules read it on hits), and 'R/L Fisting Rate' (the punch stamina) is fed by the
        // game's own formula from the swing speed.
        bool pose_fist_game_flags = true;
        // Each finger of a free hand (empty, or making a fist) follows yours: SteamVR's hand skeleton when it
        // gives one, else the trigger, grip and touch sensors (the VR framework's empty-hand curls), with
        // the framework's per-joint bends and spread, smoothed over tau_s. Written into the visible
        // body's copy of the animated pose (ABP_CopyPose1), so gauntlets follow. 0 = the game's grip shapes.
        // Elbow strikes hit like a punch (Matthew 2026-11-20): the elbow (and 7 cm either side along the
        // arm) meeting a Willie within radius_cm at the fist's speed (min_ass, the game's Aim Swing Speed
        // for its fist) makes the fist's own damage call on him with the elbow's velocity. Rigidity, cut,
        // impulse per velocity and velocity scale are the running mean of your last fist hits once
        // learn_min_hits have landed; until then these defaults.
        bool pose_elbow_strikes = true;
        double pose_elbow_radius_cm = 6.0;
        double pose_elbow_min_ass = 30.0;
        int pose_elbow_learn_min_hits = 3;
        double pose_elbow_rigidity = 0.66;
        double pose_elbow_cut = 0.75;
        double pose_elbow_impulse_per_v = 0.1;
        double pose_elbow_velocity_scale = 0.9375;
        bool pose_fingers_tracked = true;
        double pose_fingers_tau_s = 0.05;
        // A finger's joints from one curl: the middle joint is fully bent at curl pip_full, the end joint
        // just behind it, the knuckle starts at curl mcp_lag (a half curl is a hook, not a circle).
        // 0 = every joint the same fraction (the framework's way).
        bool pose_finger_schedule = true;
        // With SteamVR's hand skeleton: each finger joint follows your own joint (not one curl per finger).
        bool pose_fingers_per_joint = true;
        // With per-joint fingers: the end joint bends this much of the middle joint's angle (the tendon
        // couples them, ~0.67); 0 = as the hand skeleton reports it.
        double pose_finger_dip_coupling = 0.67;
        // The fingers' full bend at each joint x this (knuckle / middle / end; thumb not included).
        double pose_finger_scale_mcp = 1.0;
        double pose_finger_scale_pip = 1.0;
        double pose_finger_scale_dip = 1.0;
        // 1 = finger joints turn about the finger's middle, not about Willie's bones (they sit on the
        // palm-side skin, so closing the hand rolled the fingers into loops).
        bool pose_finger_pivot_fix = true;
        // A free hand (nothing held) stops at walls and scenery: a sphere of this radius at the palm's
        // middle (reach_cm past the wrist along the fingers), swept from the shoulder.
        bool pose_hand_world_stop = true;
        double pose_hand_world_radius_cm = 3.5;
        double pose_hand_world_reach_cm = 6.0;
        // 1 = an empty hand's arm is corrected after his body is evaluated, from this frame's shoulder
        // (the solve only has last frame's, so the hand bobbed with each step while walking).
        bool pose_arm_fresh_shoulder = true;
        // 1 = walking fast doesn't swap his arms into the game's falling-backwards swing (it opened the weapon hand).
        bool pose_no_fall_arms = true;
        bool pose_arm_fresh_shoulder_held = false; // test 2026-11-22: with the joint off it shook held weapons 1.4-2.6x more // ... and a hand holding a weapon, two-handing or on a scabbard
        // He stops at walls and scenery when walking (stick or room): his capsule, from this far above his
        // feet (so steps and the floor don't stop him) to his head, radius = his capsule's x share.
        bool pose_body_wall_stop = true;
        double pose_body_wall_step_cm = 35.0;
        double pose_body_wall_radius_share = 1.0;
        double pose_finger_mcp_lag = 0.25;
        double pose_finger_pip_full = 0.75;
        // Procedural finger wrap (his framework's grasp): fingers of a hand the game's grab holds a body or
        // loose object with curl until they touch it. Ease in/out time, how often all five are re-solved,
        // finger radius (0 = 9.5% of the hand's length, the framework's), and the open hand as its base.
        bool pose_wrap = true;
        double pose_wrap_ease_s = 0.12;
        double pose_wrap_refresh_s = 0.5;
        double pose_wrap_finger_radius_cm = 0.0;
        // Fingers stop at a Willie's skin, not his collision (measured per body: neck, clavicles, upper
        // spine stand out of it); hand armour makes the fingers this much thicker on the palm side (full gauntlets/gloves:
        // every segment; half gauntlets: the first).
        bool pose_wrap_body_pad = true;
        // 1 = on a Willie, fingers feel all his collision bodies (jaw, collarbones), not only the grabbed one.
        bool pose_wrap_all_bodies = true;
        // A finger joint that touched nothing while wrapping (past the edge): curled like its nearest
        // neighbour that touched, else your own curl but at most this.
        double pose_wrap_untouched_curl = 0.45;
        double pose_wrap_gauntlet_pad_cm = 0.85;
        double pose_wrap_halfgauntlet_pad_cm = 1.0;
        bool pose_wrap_open_base = true;
        // What the game's grab holds is pulled onto the palm (the spot on it nearest the palm), not into
        // the fist: palm = hand bone + along x (middle knuckle - hand bone) + palm_cm toward the palm.
        bool pose_grab_seat_palm = true;
        double pose_grab_palm_cm = 2.0;
        // Scenery: the palm point stays this far off the surface (the hand sat half in the fence).
        double pose_grab_scenery_gap_cm = 1.5;
        double pose_grab_palm_along = 0.5;
        // The mod holds what the game's grab holds (its hand joint lets things hang off the hand). Loose
        // objects: driven to their place in the hand (framework physics.track): accel cap accel/heft m/s^2,
        // turn ang_accel/heft rad/s^2, heft = (1 + 2 x heft x kg)^2. Bodies: the held bone is also pulled
        // to the palm by a spring of pull_hz, capped at pull_n N x his grip strength / 121.
        bool pose_grab_drive = true;
        double pose_grab_drive_heft = 0.1;
        double pose_grab_drive_accel = 600.0;
        double pose_grab_drive_ang_accel = 1200.0;
        double pose_grab_body_pull_n = 600.0;
        double pose_grab_body_pull_hz = 4.0;
        // The pull only takes the part of your hand's distance from the held spot past this (slack):
        // holding a throat with your hand still 10-13 cm from it pulled at the cap (900+ N with his
        // trained grip) and the game's strangle knocked them out in 0.45 s (13:50 playtest).
        double pose_grab_body_pull_slack_cm = 12.0;
        // The body pull's spring is sized for at least this many kg: a throat or chest bone is light
        // (0.9 kg), but pulling it means moving his whole upper body.
        double pose_grab_body_pull_min_kg = 12.0;
        // His drawn hand rides a held body (his framework): it stays with the palm on the held spot, fully
        // once the body is ride_full_cm from where your hand wants it (from ride_from_cm). If the body gets
        // ride_max_cm further away than its closest since the grab, for pose_grab_slip_s, the grip slips.
        bool pose_grab_ride = true;
        bool pose_grab_ride_objects = false;
        double pose_grab_ride_from_cm = 0.5;
        double pose_grab_ride_full_cm = 2.0;
        // (20 dropped ~25 body grabs in 0.4-1.8 s in a real fight, 2026-11-21 11:28-11:34: fighting Willies
        // move 35-92 cm away; the 'pushed away' reaching was his hat's collision, not the ride.)
        double pose_grab_ride_max_cm = 40.0;
        double pose_grab_slip_s = 0.5; // held past ride_max_cm this long: the grip slips (let go)
        bool log_wrap = true;
        // Once a second while fingers are posed: each finger's bends before/after and where its tip went.
        bool log_fingers = true;
        // Body grabs (a Willie's throat, arm...): what the game's grab joint feels, logged every 0.1 s.
        bool log_body_grab = true;
        // While you hold a Willie: his joints as the game's Dislocate Bone reads them (each bone's forward
        // against its parent's, the bend; and its twist), with Strain / Dislocation State / part health.
        bool log_joint_strain = true;
        // Joint breaking by grappling. The game's own joint strain (Dislocate Bone) was emptied out in
        // the 11-17 update, so the mod runs the old game's rules for a Willie you hold: his neck_01,
        // upperarms, spine_03 (and calves with joint_break_legs) bent against their parent bone, or
        // twisted past joint_break_twist_free_*_deg from how they sat when you took hold, build his
        // Strain and a Dislocation State the mod keeps per part (easing off at 0.05/s); at 1 the neck snaps (the game's Snap Neck: he dies), a shoulder
        // or knee dislocates (the game relocates it by its own rules), or the back breaks (Break Back).
        // A twist's excess counts as a bend of excess x joint_break_twist_gain degrees.
        bool joint_break = true;
        // A twist counts only for a limb a hand of yours is on (his head, neck, arm or back), and the
        // free range shrinks to joint_break_free_min of itself (and the strain rate doubles) as his
        // consciousness and pain take his resistance away. His neck stops turning at about 70 deg from
        // where it sat (playtest 2026-11-22), so the neck's free range is well short of that.
        double joint_break_twist_free_neck_deg = 40.0;
        double joint_break_twist_free_arm_deg = 45.0;
        double joint_break_twist_free_back_deg = 30.0;
        double joint_break_twist_gain = 3.0;
        double joint_break_free_min = 0.4;
        // Knees too (the old rule read the calf's bend against the thigh).
        bool joint_break_legs = false;
        // Every part's strain rate x this (1 = the old game).
        double joint_break_scale = 1.0;
        // Every other Willie's body hits (impulse, health, consciousness, what hit him), when they change.
        bool log_body_hits = true;
        // 1 = the game's grab joint is made soft (a capped spring, B&S-like) 0.25 s after it latches, so
        // the tracked hand no longer pulls on it without limit (the game lets go when the pull beats
        // his grip strength). 0 = the game's own joint.
        // Test 4 (2026-11-19): 2000/100 let a standing Willie go in 2.8 s; 20000/400 held like the game's
        // joint. cap_per_t: the drive cap is this x his grip strength T (the game's slip threshold), so
        // the force stays just under T (it levels off at about cap/500): his strength sets how hard he
        // can drag a body, and the stretch rule lets go when it gets away. 0 = the fixed pose_grab_body_max.
        // Test 5: the game's own joint (0) held an active Willie 15 s and choked him out; the capped spring
        // let a standing one drift off (its force levels at ~cap/400 whatever the stretch). Every grab the
        // game broke had HIS arm at tonus 0 / health 0 after hits (T = 0: the game's broken-arm rule).
        int pose_grab_body_hold = 0;
        double pose_grab_body_spring = 20000.0; // drive stiffness (acceleration mode)
        double pose_grab_body_damper = 400.0;
        double pose_grab_body_cap_per_t = 450.0;
        double pose_grab_body_max = 250000.0;    // drive cap when cap_per_t is 0
        double pose_grab_body_let_go_cm = 30.0; // hand this far from the grab point for 0.3 s lets go
        // The game's grip-slip (joint force > his grip strength T for ~0.1 s) is made for a physical hand;
        // a tracked hand's own motion trips it (Matthew's playtest 2026-11-19: all three throat grabs
        // released by it in 0.6-0.7 s with a healthy arm). 1 = while a body is held, the game's own
        // 'Hand Block Drop' (which it sets itself for the first 0.5 s of a grab) is kept on, and the hand
        // lets go instead when pulled pose_grab_body_let_go_cm off its hold (B&S). His grip strength T
        // still counts: below pose_grab_body_slip_below_t (a broken or numb arm) the game's slip applies.
        bool pose_grab_body_no_slip = true;
        double pose_grab_body_slip_below_t = 20.0;
        // Eating and drinking: grabbed food within pose_eat_cm of your mouth is bitten (the game's
        // Bite Event); a held bottle at your mouth with its neck below pose_drink_tip (the dot of its
        // axis with up) is drunk from, at most once a second.
        bool pose_eat = true;
        double pose_eat_cm = 12.0;
        double pose_drink_tip = 0.0;
        // Seconds between sips while the bottle stays tipped at your mouth.
        double pose_drink_sip_s = 1.0;
        // The ghost hand: while his hand stays on a stopped weapon more than pose_ghost_from_cm from
        // your real hand, a see-through glove shows where your hand is. Mesh and material are object
        // paths in the cooked game; offset (cm) and turn (deg) from the hand bone, and scale, for fitting.
        // The glove is turned so its fingers and thumb follow the hand bone's; the turn is extra, in the glove's frame.
        bool pose_ghost_hand = false; // off: Matthew 2026-11-18, it popped up on every strike
        double pose_ghost_from_cm = 4.0;
        std::string pose_ghost_mesh = "/Game/Assets/Armor/Hand/Gauntlet/StaticMeshes/WM_Armor_Hand_Gauntlet_Glove_003.WM_Armor_Hand_Gauntlet_Glove_003";
        std::string pose_ghost_material = "/Engine/EngineDebugMaterials/M_SimpleUnlitTranslucent.M_SimpleUnlitTranslucent";
        double pose_ghost_x_cm = 0.0, pose_ghost_y_cm = 0.0, pose_ghost_z_cm = 0.0;
        double pose_ghost_pitch = 180.0, pose_ghost_yaw = 0.0, pose_ghost_roll = 0.0; // 180: run 33b, fingertips along his fingers
        double pose_ghost_scale = 0.8;
        // Your arm length (shoulder to wrist, cm), measured by holding both arms straight out to the
        // sides and holding B and Y for 2 s. 0 = not measured: your hands go exactly where the
        // controllers are. Measured, your reach is scaled to his so full reach is full reach.
        double pose_user_arm_cm = 0.0;
        // The shoulder reaches with you: as your hand nears the end of his arm (the last
        // pose_shoulder_reach_start_cm), his clavicle turns toward it and brings the shoulder up to
        // pose_shoulder_reach_cm closer (at most pose_shoulder_reach_max_deg of clavicle turn). 0 = off.
        double pose_shoulder_reach_cm = 12.0;
        double pose_shoulder_reach_start_cm = 6.0;
        double pose_shoulder_reach_max_deg = 35.0;
        double pose_grip_strength_kg = 20.0;
        double pose_grip_hz = 20.0;
        double pose_grip_damping = 0.3;
        double pose_grip_radius_cm = 30.0;
        double pose_grip_buzz = 0.6;
        // The buzz starts when the joint stretches past this (cm). Even the game's locked grip
        // reads ~4.5-5.5 cm on this measure (headset test 1: buzzing at 6 fired a third of the time).
        double pose_grip_buzz_from_cm = 10.0;
        // The turning hold, relative to the position hold: stiffness multiplier, its own damping
        // ratio, and a multiplier on the turning cap. Test 27: at cap x1 the weapon held ~1 deg for a
        // few seconds, then broke loose and spun (up to 150 deg); x10 and up held; 4 / 0.3 / 100 held
        // 0.8 deg still and ~4.5 deg in fast swings.
        double pose_grip_turn_scale = 4.0;
        double pose_grip_turn_damping = 0.3;
        double pose_grip_turn_max_scale = 100.0;
        // Where on the weapon the hand holds it: 0 = the game's joint anchor; 1 = the weapon's own
        // "Grip R/L Hand Default Pos" marker (the handle), when it has one.
        bool pose_grip_anchor_marker = false;
        // Move his body each frame without teleporting its physics bodies (1), so held weapons follow
        // smoothly; jumps over pose_teleport_above_cm per frame still teleport.
        bool pose_move_bodies = true;
        double pose_teleport_above_cm = 30.0;
        // 1 = he crouches as low as you do (your headset below where you stood when you recentred,
        // less pose_crouch_dead_zone_cm).
        bool pose_crouch = true;
        double pose_crouch_dead_zone_cm = 3.0;
        // How far his legs crouch him (cm, times his size). Deeper than that (your view lower still), he
        // bends forward until his head is down with your view, so his hands reach as low as yours
        // (pose_crouch_bend_for_depth; playtest 2026-11-21: he could not reach the ground).
        double pose_crouch_max_cm = 62.0; // was a fixed 55; playtest 2026-11-21: view 114 cm down, his head bone 35 cm above it
        bool pose_crouch_bend_for_depth = true;
        // Left stick click toggles a button crouch this deep (cm, eased at _cms); 0 = off.
        double pose_crouch_button_cm = 40.0;
        double pose_crouch_button_cms = 120.0;
        // A short tap (under kick_tap_s) of right A kicks: the game's Kick Event.
        bool kick_on_a = true;
        double kick_tap_s = 0.35;
        // Left X is the game's Jump key (Space): hold to gather, let go to leap (the game jumps only
        // once its Jump Rate has reached 0.5; a quick tap is the game's own tap-kick). 0 = off.
        bool jump_on_x = true;
        // Right stick click is the game's SloMo key (F): toggles its slow motion (Slomo Timeline:
        // time slows to 0.75..0.25 by his Body Skill, stamina drains faster, it ends by itself at
        // stamina 0.5 or consciousness 50). 0 = off.
        bool slowmo_on_right_stick_click = true;
        // Crouching keeps his legs on the leg IK: the animation's kneel and falling leg poses are kept
        // off (they sank the whole body 50-100 cm at a deep crouch). And the capsule is never put more
        // than 15 cm below its standing height over the floor (the game drops it ~77 cm when its foot
        // traces miss).
        bool pose_crouch_keep_leg_ik = true;
        // No knockdowns for your player: the game's kneel never takes his legs. While the body is posed,
        // his own R/L Kneel and R/L Kneel Falling are kept off and Legs Tonus Far Step kept at 1, just
        // before and just after his tick. In VR the game read a bent, leaning pose as kneel-falling, fired
        // its Kneel Event, and while kneeling it cuts leg tonus to 0 once the feet are 1 m from its last
        // step spot (always, with the feet posed): the pelvis drive went to 0 and the body sank 60 cm for
        // good (playtest 2026-11-20 11:20Z). Enemies are not touched; leg injuries still cut leg tonus.
        bool pose_player_no_kneel = true;
        bool pose_capsule_floor_clamp = true;
        // Every tick: the capsule never more than 15 cm under standing height over the floor (one trace),
        // and above it he falls with gravity (pits, ledges). Stops the running sink-into-the-floor collapse.
        bool pose_capsule_keep_on_floor = true;
        // The game sets the capsule's height to (lower foot's step end) + half height + clamp(Step Height 2,
        // +-10). Step Height 2 sinks to about -4 cm through each step and snaps back to 0 as a foot lands,
        // so walking the capsule (and the view and his body on it) dropped and hopped ~2 times a second
        // (walking_floor_loss.md). On: the mod takes that step term (read before his tick) back out, except
        // while he climbs, jumps or is fallen.
        bool pose_capsule_no_step_offset = true;
        // Where the wrist sits relative to the controller's grip, in the controller's frame
        // (forward, right, up, cm), and how the hand is turned relative to the controller (degrees).
        // Defaults: the wrist 8 cm from the palm centre back along the fingers and 2 cm toward the
        // back of the hand, with the fingers tilted pose_knuckle_deg from the handle (right is
        // mirrored for the left hand, so + is toward the back of either hand).
        double pose_wrist_forward_cm = -2.7, pose_wrist_right_cm = 2.0, pose_wrist_up_cm = 7.5;
        double pose_hand_pitch = 0.0, pose_hand_yaw = 0.0, pose_hand_roll = 0.0;
        double pose_knuckle_deg = 20.0;
        // How much of the hand's turn about the forearm the forearm bone takes (0 to 1); the rest
        // shows at the wrist. Willie's forearm has no twist bones.
        double pose_forearm_twist_share = 0.5;
        // How much of your head's turn away from the hips the spine and neck take together (0 to 1;
        // the head is always exactly the headset).
        double pose_torso_twist = 0.7;
        // Feet stay where they were put down and step (lift and place) when the hips have moved or
        // turned away from them: 1 = on, 0 = the game's own feet.
        bool pose_feet = true;
        // 1 = while the game's kick has a leg (Kicking L/R), the mod stops planting that foot and holding
        // it flat; it is planted again where the kick put it down (playtest 2026-11-21: foot turned backwards).
        bool pose_feet_free_on_kick = true;
        double pose_feet_kick_free_s = 1.0; // and this long after right A / the left-X tap fires a kick (kicks rise 0.2-0.3 s after the press and last ~0.5 s)
        double pose_step_distance_cm = 10.0, pose_step_angle_deg = 30.0; // when a foot steps
        double pose_step_time_s = 0.3;  // how long a step takes standing (shorter when moving)
        double pose_step_lift_cm = 10.0; // how high the foot lifts
        double pose_step_lead_s = 0.2;   // how far ahead (in seconds of movement) a foot lands when walking
        // How quickly the elbow's swing around the arm follows (seconds; the hand is never smoothed).
        double pose_swivel_smooth_s = 0.06;
        // TEST ONLY: the head and hands follow a slow made-up path instead of the headset and
        // controllers, so an unattended run (no one wearing the headset) can check the posing.
        bool dev_pose_fake_tracking = false;
        // Log the right-hand grip every tick (hand bone, hand physics body, weapon anchor, wrist target).
        bool dev_grip_trace = false;
        // Log every hit of a held weapon on a person ("Stab:" lines): the game's own impulse, tip
        // alignment, cutting rate, rubber-band and stick state, and the controller's speed along the
        // blade. Read only; changes nothing in the game.
        bool stab_log = true;
        // Hit vibration, Blade & Sorcery's way (defaults\bs_haptics.md): every weapon, shield or fist
        // hit plays B&S's hit-sound envelopes on the controllers, sized by the contact speed (and,
        // for a blow taken on a held item, the game's own impulse). This is the overall strength
        // (0 off, 1 as B&S).
        double hit_buzz = 1.0;
        // B&S's scrape ticks while a contact slides (0.8 per 2 cm), x this (0 off).
        double hit_buzz_scrape = 1.0;
        // B&S makes no buzz for a blade on plate armour; 1 = give it the metal-on-metal layer.
        bool hit_buzz_blade_on_plate = true;
        // 1 = a weapon's own parts hitting each other (flail head on its haft) never buzz.
        bool hit_buzz_ignore_self = true;
        // The game's impulse (kg cm/s) that counts as a full-strength blow on a held item.
        double hit_buzz_impulse_full = 3000.0;
        // Hits under this intensity (0..1 of B&S's speed range) are touch, not hits: no buzz.
        double hit_buzz_min_intensity = 0.05;
        // An open hand, forearm or arm on a person buzzes only when it strikes this fast (m/s,
        // against him at the contact).
        double hit_buzz_limb_min_ms = 3.0;
        // Swing whoosh (hs_whoosh_sound.md). The game whooshes only when the weapon's centre moves faster than
        // 1.5 x its "Sound Woosh Threshold" (7.5 m/s for most weapons, 5.25 for an arming sword): flat
        // Willie's mouse swings reach 10-20 m/s, a human VR cut often doesn't (Matthew 2026-11-21: most VR
        // swings were silent). A weapon in a tracked hand has its threshold x this during its tick
        // (1 = the game's own; lower = slower swings whoosh; the one-shot is also that much quieter).
        double whoosh_vr_threshold_scale = 0.6;
        // "Whoosh:" lines every 0.5 s while a held weapon moves faster than whoosh_log_min_speed (cm/s).
        bool whoosh_log = true;
        double whoosh_log_min_speed = 300.0;
        // Log every hit buzz ("Hit buzz:" lines).
        bool hit_buzz_log = true;
        // Every this many seconds a "Perf:" line: frame rate, the mod's own time per frame by part, and
        // how many objects, actors and simulating bodies the game has (0 = off).
        double perf_log_seconds = 10.0;
        // Every this many seconds a "Strength: training:" line: his strength (Muscle Rate), the growth the
        // game has banked (Muscle Growth Due) and every input to it (0 = off).
        double strength_log_seconds = 10.0;
        // Troubleshooting: time every Blueprint call the game makes on its main thread, per class and
        // function ("Perf events" lines with each Perf line). Costs about 1 ms a frame in big fights.
        bool perf_events = false;
        // Troubleshooting: sample the game thread's stack ~1000 times a second ("Perf sample" lines,
        // logs\perf_samples.tsv). Read live; the sampling thread starts on first use.
        bool perf_sample = false;
        // Troubleshooting: time Willie's event graph (his tick) by bytecode segment ("Perf segments"
        // lines, logs\perf_segments.tsv), to map onto the decompile listing.
        bool perf_bp_segments = false;
        // Stab assist: the VR hold moves the weapon on its own (the game's hand joint is free), so a
        // tip meets flesh with only the weapon's mass behind it and the game's stick rule
        // (cutting rate x impulse >= 10000) rarely passes. On a hit of a piercing tip on a person's
        // flesh that points into him (cosine >= stab_min_into_cos, tip within 15 cm of the impact)
        // and moves along the thrust at stab_min_speed_cms or more, or keeps being pushed in with
        // at least stab_push_force_n for stab_push_time_s, the impulse the game is given is raised
        // to stab_arm_mass_kg x that speed, along the thrust. The game then judges the stab itself
        // (alignment, armour, sticking, damage).
        bool stab_assist = true;
        double stab_min_speed_cms = 70.0;  // (120 until 2026-11-20: "too much force, too much effort")
        double stab_min_into_cos = 0.5;
        double stab_arm_mass_kg = 4.0;
        double stab_push_force_n = 60.0;
        double stab_push_time_s = 0.1;
        // 1 = your character's head (and hair) is not drawn in your own view while you look out of
        // it; it still casts its shadow and is there for the game (hits, other characters). Helmets
        // and other headwear stay visible.
        bool hide_own_head = true;
        // Your worn helmet in your own view: 0 drawn (real, blocks the view), 1 the helmet hidden from
        // you, 2 everything on your head hidden from you (helmet and hood/coif/cap), 3 a clear window:
        // the parts within helmet_window_cm of your eyes aren't drawn. Others still see it and it
        // still protects you.
        int helmet_view = 0;
        double helmet_window_cm = 7.0;
        // helmet_view 4 (experiment): the helmet half see-through for you. helmet_see_through is how
        // much you see through (0..1). helmet_see_through_mode 0 = screen door through the helmet
        // material's texture parameter helmet_see_through_param (our own render target, dithered;
        // helmet_see_through_test 1 fills it white, 2 black, to find what drives the opacity mask),
        // 1 = the helmet hidden from you on alternate frames.
        double helmet_see_through = 0.5;
        int helmet_see_through_mode = 0;
        std::string helmet_see_through_param = "RT_Damage";
        int helmet_see_through_test = 0;
        // helmet_view 5: your helmet faded in your view (an only-you-see copy drawn with a translucent
        // material the game ships). helmet_fade_see_through 0 = solid, 1 = invisible. The material has
        // no colour texture: helmet_fade_tint is the metal colour "r,g,b" (0-1).
        double helmet_fade_see_through = 0.5;
        // Which head pieces helmet_view 5 fades: those in front of your eyes (built-in list measured
        // from the meshes: helmet_fade_face.md). Comma lists of mesh names to always / never fade.
        std::string helmet_fade_always = "";
        std::string helmet_fade_never = "";
        std::string helmet_fade_tint = "0.55,0.55,0.58";
        double helmet_fade_metallic = 1.0;
        double helmet_fade_roughness = 0.35;
        std::string helmet_fade_material = "/DatasmithContent/Materials/StdTranslucent/M_StdTranslucent.M_StdTranslucent";
        // The gore skeleton under his skin (skull, jaw, teeth, throat) would show from inside the
        // hidden head: it is hidden from this bone up. Empty = not hidden. Wounds elsewhere still
        // show their bones.
        std::string hide_own_gore_from = "neck_01";
        // 1 = hits on your posed body hurt you as in the flat game. The game's Get Damage only counts
        // Health, Consciousness, Bleeding and Force Death when the victim's head body simulates
        // physics (Root Mesh, Get Damage @540-@584); the posed body does not, so without this the
        // player only lost limb health and could die only by losing his head.
        bool player_takes_damage = true;
        // 1 = your posed body bumping into a person counts by how fast it moves into him, as a
        // simulated body would. The posed body is kinematic: when it sinks into a Willie the physics
        // pushes him out with an impulse set by how deep it went, not how fast (the game caps it at
        // 1500 standing), and his body-hit block (Willie_BP UG @741800-@746500) passes any contact of
        // your arm, hand, calf or foot ('Beig Hit By Limbs') to Deal Complex Damage with that impulse:
        // walking slowly into him knocked his consciousness down (Matthew 2026-11-21). With this the
        // contact's impulse is at most reduced mass x closing speed (each side's body mass, your
        // contact body's x player_body_contact_mass_scale), the impulse a free body of that mass
        // would give; it is never raised. Both ways: him hitting your body too.
        bool player_body_contact_speed = true;
        double player_body_contact_mass_scale = 1.0;
        bool player_body_contact_log = true;
        // 1 = an item touching you (someone else's weapon, tool, a thrown thing) hurts by how fast it
        // comes into your body part, not by how hard your posed body squeezes it out (a passive
        // Willie's tongs pinned between you took your consciousness 95 -> 55 in 2 s). Real blows unchanged.
        bool player_item_contact_speed = true;
        // 1 = blood from your own cut-off limb stays at the stump. The game sticks the fountain to a
        // bone of the cut-off limb, which it only hides; the posed body keeps moving that hidden
        // bone (hand to controller, head to headset), so the blood floated in mid-air.
        bool player_stump_blood = true;
        // Your own injuries, with the game's own limb values (Willie_BP; injuries.md). A limb counts as
        // badly hurt when its health is below injury_limb_health (the game's own bone-break
        // threshold, Get Damage: bones only break at limb health <= 25; a bone put back is set to 25),
        // or it is broken, dislocated or cut off (the game holds that limb's health at 0 then). 0 = off.
        bool injury_limbs = true;
        double injury_limb_health = 25.0;
        // One leg badly hurt: you walk at this share of your normal speed (stick walking).
        double injury_one_leg_speed = 0.5;
        // Both legs badly hurt (or the back broken, which drops both legs' tonus in the game): no
        // walking and he falls over (physics on, the view stays on his head) until a leg recovers.
        bool injury_two_legs_fall = true;
        bool injury_back_broken_falls = true;
        // A broken, cut-off or badly hurt arm: the controller no longer moves it; it hangs and swings
        // from the shoulder and its hand lets go. (Dislocated without broken: it flops after your
        // controller, this lag, s, and this much sag, cm; normal Willies never get that flag.)
        double injury_dislocated_follow_s = 0.15;
        double injury_dislocated_sag_cm = 12.0;
        // Swing a broken arm back in, the game's own rule (his tick, UG @358184..358825): while that
        // hand swings at Aim Swing Speed > 60 it calls Restore Bone(arm, MRC(speed, 60..90 -> 0..50)
        // x dt); at Dislocation State 0 the game clears Broken, sets health 25 and the joint "Motor".
        // The game gates it on "R/L Hand Active" (mouse buttons, always 0 in VR), so the mod makes the
        // call with your controller's speed (pose_swing_speed_lo..hi m/s -> 0..75, as the swing feed).
        bool injury_swing_relocate = true;
        // You die: the mod stops posing you, physics goes on and he falls as a ragdoll; the view stays
        // on his head (no fade).
        bool death_ragdoll = true;
    };

    // Current settings (copy). Cheap enough to call once per frame.
    Config config();
    // Loads the file, writing a default one if it is missing. Returns true if values changed.
    bool config_reload_if_changed(bool force = false);
    void config_set(const std::string& key, const std::string& value);
} // namespace hsvr
