// Settings read from HalfSwordVR.ini next to the mod. The file is re-read when it changes,
// so values can be tuned while the game runs.
#pragma once
#include <string>

namespace hsvr
{
    struct Config
    {
        bool enabled = true;
        // Only start VR when SteamVR is already running, so the game still starts flat without it.
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
        double near_clip_cm = 4.0;
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
        int stereo_slot_offset = 0; // 0 = find automatically

        // ---- Phase 2: controllers (read only) and the player physics test ----
        // Write the controllers' positions, buttons and sticks to the log once a second, with the
        // distance from each controller to the character's hand bone.
        bool log_controllers = true;
        std::string hand_bone_left = "hand_l";
        std::string hand_bone_right = "hand_r";
        // 1 = a short buzz on a controller each time its trigger is pulled all the way (checks vibration).
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
        bool pose_arms = true;
        bool pose_follow_room = true;
        // 1 = while posed, your character's capsule only moves when you walk: the game's own pull
        // toward the body (and hit stumbles, dodges and leans) is undone after each of his ticks.
        bool pose_hold_capsule = true;
        // 1 = his body faces where you face: it follows your headset once you look more than
        // pose_turn_dead_zone_deg away, turning at pose_turn_speed_dps. The mouse or stick still
        // turns you as well.
        bool pose_body_turn = true;
        double pose_turn_dead_zone_deg = 60.0;
        double pose_turn_speed_dps = 150.0;
        // The most your head can be turned from his hips (a neck's reach): past it the hips come
        // round with you at once.
        double pose_turn_max_deg = 85.0;
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
        // Holding and swinging cost stamina by the game's own formula, run on the hold's force and
        // torque (capped) and the controller's speed at the grip, smoothed (lo..hi cm/s -> x10..x85),
        // times this scale (3-5 if continuous swinging should empty the bar in 10-20 s).
        double pose_stamina_scale = 1.0;
        double pose_stamina_swing_lo_cm_s = 200.0;
        double pose_stamina_swing_hi_cm_s = 700.0;
        double pose_stamina_speed_smooth_s = 0.7;
        double pose_stamina_force_cap_n = 150.0;
        double pose_stamina_torque_cap_nm = 15.0;
        // Exact hold (B&S-style): each frame the weapon gets the velocity that puts it exactly where
        // the hand is drawn at the end of this frame's physics, limited only by the hand's strength
        // (pose_hold_max_force / max_torque). 0 = the spring below.
        bool pose_hold_exact = true;
        double pose_hold_spring = 5000.0;
        double pose_hold_damper = 100.0;
        double pose_hold_max_force = 3000.0;
        // While the held weapon is stuck in a body, the hold's force and torque caps are scaled by this.
        double pose_hold_stuck_force_scale = 0.15;
        // In a wound (the weapon Stuck In Body): the blade slides along it at most this fast in and
        // out (cm/s; B&S's held dampers with the hand's strength), x stab_wound_head_factor in the
        // head and neck; sideways and in turn it goes with the body.
        bool stab_wound_hold = true;
        // Reverse grip flip (S&S): a tap (shorter than grip_flip_tap_s) of the holding hand's B / Y
        // turns a one-handed weapon end for end at the same spot on its handle, gliding over grip_flip_s.
        bool grip_flip = true;
        // A blade stuck in someone is torn out when your hand is more than stab_yank_cm from its grip
        // for stab_yank_s (0 = never).
        double stab_yank_cm = 35.0;
        double stab_yank_s = 0.25;
        double grip_flip_s = 0.3;
        double grip_flip_tap_s = 0.4;
        double stab_wound_settle_s = 0.2; // no pulling out for this long after it goes in
        double stab_wound_settle_max_s = 0.8; // a young stick is held open until the game's own keep trace passes, at most this long
        double stab_wound_entry_cms = 40.0; // ...and it is carried in at this speed meanwhile (8 cm over 0.2 s)
        // A blade the game has just stuck is set this far into the wound along its tip (cm), plus
        // stab_start_depth_per_ms cm for each cm/s of the controller along it, at most _max_cm
        // (the game's stuck joint lets the blade slide 5 cm along a thrust).
        double stab_start_depth_cm = 0.0; // 0 = off (stab-test11: a teleport into the wound made it let go)
        double stab_start_depth_per_ms = 0.01;
        double stab_start_depth_max_cm = 4.5;
        double stab_wound_in_cms = 50.0;
        double stab_wound_out_cms = 40.0;
        double stab_wound_head_factor = 0.35;
        double pose_hold_turn_spring = 1100.0;
        double pose_hold_turn_damper = 45.0;
        double pose_hold_max_torque = 250.0;
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
        double pose_bs_two_pos_k = 10000.0, pose_bs_two_pos_c = 150.0, pose_bs_two_pos_max = 6000.0;
        double pose_bs_two_rot_k = 2200.0, pose_bs_two_rot_c = 90.0, pose_bs_two_rot_max = 500.0;
        int pose_bs_iterations = 8;
        double pose_bs_two_let_go_cm = 45.0; // B&S gaps reach 18 cm (p95) on fast two-hand swings
        bool pose_hold_ignore_body = true;   // the held weapon never collides with his own body (B&S)
        double pose_bs_max_spin = 60.0; // rad/s
        // The weight's own swing never spins it faster than the turn wanted plus this (rad/s): one tick
        // of a deadbeat grip pull fed back into the lever spun the axe to the 60 rad/s cap (playtest 09:38).
        double pose_hold_weight_spin_extra = 5.0;
        // While held, his visible hand sits on the weapon (1): when the weapon is stopped by something,
        // the hand stays on it instead of going on with your controller. It eases from following the
        // controller to sitting on the weapon as the weapon falls 1 to pose_hold_blocked_from_cm behind.
        bool pose_hold_hand_on_weapon = true;
        double pose_hold_blocked_from_cm = 3.0;
        // The stuck rule: a stopped weapon whose grip is out of his reach from the shoulder, or more
        // than an arm's length from your hand, for pose_hold_let_go_s, is let go (the game's own drop).
        // A weapon stopped by his own body is never let go by the stuck rule (vanilla weapons collide with him too).
        bool pose_hold_let_go_ignore_self = true;
        bool pose_hold_let_go = true;
        double pose_hold_let_go_s = 0.15;
        // No let-go for this long after a weapon comes into the hand (long weapons swing into place).
        double pose_hold_let_go_grace_s = 1.0;
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
        double pose_grab_touch_cm = 5.0;
        double pose_grab_handle_cm = 6.0;
        double pose_slide_keep_light = 0.6;
        double pose_slide_keep_full = 0.97;
        // B&S slide damper along the handle (N s/m) at a light .. full trigger: the item keeps
        // exp(-c dt / mass) of its motion along it each frame (replaces keep_light/full; 0 = use those).
        double pose_slide_damper_light = 300.0;
        double pose_slide_damper_full = 20.0;
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
        bool pose_grab_release_drops = true;
        // Squeezing near a loose object or a body (no weapon in reach) grabs it through the game's own grab.
        bool pose_grab_objects = true;
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
        // The game's swing speed (Aim Swing Speed) fed from the controllers: the faster hand that holds
        // something, lo..hi m/s relative to the body -> 0..75; falls with this time constant.
        bool pose_feed_swing_speed = true;
        // A weapon grabbed by its grip or blade sits at the game's own seat in the hand (the nearest of
        // it, edge turned, blade reversed), its handle through the seat's point (B&S fixed orientations).
        bool pose_grab_seat = true;
        // Throwing (B&S): a weapon let go while the hand moves at least pose_throw_speed m/s is thrown the
        // game's way (its hits count) and its velocity multiplied by pose_throw_multiplier (B&S 2.0).
        bool pose_throw = true;
        double pose_throw_speed = 2.0;
        double pose_throw_multiplier = 2.0;
        // Drawing from a scabbard: a squeeze within this distance of a sheathed weapon's grip draws it
        // (pulled out along the scabbard; the game releases it at 85%), eased into the hand over settle_s.
        bool pose_draw = true;
        double pose_draw_touch_cm = 8.0;
        double pose_draw_settle_s = 0.15;
        double pose_draw_step_cm = 6.0; // the most it moves along the scabbard per tick
        double pose_draw_force_scale = 0.3; // the hold's force/torque caps while drawing or sheathing
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
        // Crouching keeps his legs on the leg IK: the animation's kneel and falling leg poses are kept
        // off (they sank the whole body 50-100 cm at a deep crouch). And the capsule is never put more
        // than 15 cm below its standing height over the floor (the game drops it ~77 cm when its foot
        // traces miss).
        bool pose_crouch_keep_leg_ik = true;
        bool pose_capsule_floor_clamp = true;
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
        // Stab assist: the VR hold moves the weapon on its own (the game's hand joint is free), so a
        // tip meets flesh with only the weapon's mass behind it and the game's stick rule
        // (cutting rate x impulse >= 10000) rarely passes. On a hit of a piercing tip on a person's
        // flesh that points into him (cosine >= stab_min_into_cos, tip within 15 cm of the impact)
        // and moves along the thrust at stab_min_speed_cms or more, or keeps being pushed in with
        // at least stab_push_force_n for stab_push_time_s, the impulse the game is given is raised
        // to stab_arm_mass_kg x that speed, along the thrust. The game then judges the stab itself
        // (alignment, armour, sticking, damage).
        bool stab_assist = true;
        double stab_min_speed_cms = 120.0;
        double stab_min_into_cos = 0.5;
        double stab_arm_mass_kg = 4.0;
        double stab_push_force_n = 150.0;
        double stab_push_time_s = 0.15;
        // 1 = your character's head (and hair) is not drawn in your own view while you look out of
        // it; it still casts its shadow and is there for the game (hits, other characters). Helmets
        // and other headwear stay visible.
        bool hide_own_head = true;
        // The gore skeleton under his skin (skull, jaw, teeth, throat) would show from inside the
        // hidden head: it is hidden from this bone up. Empty = not hidden. Wounds elsewhere still
        // show their bones.
        std::string hide_own_gore_from = "neck_01";
    };

    // Current settings (copy). Cheap enough to call once per frame.
    Config config();
    // Loads the file, writing a default one if it is missing. Returns true if values changed.
    bool config_reload_if_changed(bool force = false);
    void config_set(const std::string& key, const std::string& value);
} // namespace hsvr
