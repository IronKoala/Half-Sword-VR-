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
        // TEST ONLY. 1 = switch physics off on your own character's body (enemies are not touched)
        // to see what the game does without the ragdoll. Can be flipped while the game runs;
        // switching back to 0 turns physics on again, which may not match the game's own setup,
        // so restart the fight afterwards.
        bool dev_player_physics_off = false;
        // 1 = once a second, where your character's body, head and hands are (on automatically
        // while the physics test runs). Unattended tests can also switch the physics test with
        // the dev_script step "hsvr.physics_off 1" / "hsvr.physics_off 0".
        bool log_body = false;
        // TEST ONLY, with the physics test: 1 = keep physics on for both arms (every body from
        // these bones outwards), so the rest of the body is still and the arms hang and move by the
        // game's own muscles.
        bool dev_player_arm_physics = false;
        std::string arm_bone_left = "clavicle_l";
        std::string arm_bone_right = "clavicle_r";
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
        double pose_hold_spring = 5000.0;
        double pose_hold_damper = 100.0;
        double pose_hold_max_force = 3000.0;
        double pose_hold_turn_spring = 1100.0;
        double pose_hold_turn_damper = 45.0;
        double pose_hold_max_torque = 250.0;
        // While held, his visible hand sits on the weapon (1): when the weapon is stopped by something,
        // the hand stays on it instead of going on with your controller. It eases from following the
        // controller to sitting on the weapon as the weapon falls 1 to pose_hold_blocked_from_cm behind.
        bool pose_hold_hand_on_weapon = true;
        double pose_hold_blocked_from_cm = 3.0;
        // The stuck rule: a stopped weapon whose grip is out of his reach from the shoulder, or more
        // than an arm's length from your hand, for pose_hold_let_go_s, is let go (the game's own drop).
        bool pose_hold_let_go = true;
        double pose_hold_let_go_s = 0.15;
        // Your arm length (shoulder to wrist, cm), measured by holding both arms straight out to the
        // sides and holding B and Y for 2 s. 0 = not measured: your hands go exactly where the
        // controllers are. Measured, your reach is scaled to his so full reach is full reach.
        double pose_user_arm_cm = 0.0;
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
