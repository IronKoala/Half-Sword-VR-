// Settings read from HalfSwordVR.ini next to the mod. The file is re-read when it changes,
// so values can be tuned while the game runs.
#pragma once
#include <map>
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
        // ---- motion controls (arm_control.cpp) ----
        // 1 = the controllers move the character's hands (when they are tracked).
        bool motion_arms = true;
        bool arm_right = true;
        bool arm_left = true;
        // Key that switches motion arms on and off while playing. 0x21 = Page Up.
        int toggle_arms_key = 0x21;
        // 1 = also feed the game's own aim (where the mouse would point the sword) from the
        // controller, so that the game's swing speed, stamina and body turning follow along.
        bool aim_sync = false;
        double aim_scale = 1.0;   // aim degrees per degree of blade direction
        bool aim_invert_y = false;
        // Where the elbow points while the hand is placed: a mix of down, out to the side and back.
        double elbow_down = 1.0;
        double elbow_out = 0.6;
        double elbow_back = 0.2;
        // Reach: beyond reach_knee of the character's arm length the hand is eased in towards
        // reach_max, so the elbow stays a little bent (0.94 is about 40 degrees of bend).
        double reach_knee = 0.85;
        double reach_max = 0.95;
        // How far the shoulder may move towards a hand beyond easy reach, cm (0 = never).
        double shoulder_reach_cm = 10.0;
        // The elbow swings out of its natural place when the wrist would have to bend more than
        // this (the game's wrist stops near 78 degrees). elbow_stiffness: cost per degree squared
        // of moving the elbow; larger keeps it more in place.
        double wrist_bend_limit = 55.0;
        double elbow_stiffness = 0.02;
        // Hand roll about the forearm the elbow tries to keep under (degrees), the most roll ever sent
        // (stops the arm flipping over), and how far the elbow may swing from its natural place.
        double twist_limit = 70.0;
        double twist_clamp = 150.0;
        double elbow_swing_max = 90.0;
        // Hold assist: the steady gap between where a hand is sent and where the muscles hold it
        // is made up for over this many seconds (0 = off), within these limits.
        // It only works while the hand is slow (assist_speed_cms: speed at which it is half as strong).
        double assist_seconds = 1.0;
        double assist_max_cm = 10.0;
        double assist_max_deg = 25.0;
        double assist_speed_cms = 40.0;
        // Arm-length calibration (hold both grip buttons for 2 seconds with both arms straight out
        // in front at shoulder height). Written here by the mod; player_arm_cm 0 = not calibrated.
        // The player's shoulders are placed this far below the eyes and to each side; reach is
        // then scaled from the player's shoulder to the character's, and the shoulders lined up.
        double player_arm_cm = 0.0;
        double player_shoulder_drop_cm = 22.0;
        double player_shoulder_side_cm = 18.0;
        // 1 = use the calibration (when there is one).
        bool arm_scaling = true;
        // Share of the hand's roll that the forearm takes (0 = all in the wrist, 1 = all in the forearm).
        double forearm_twist_share = 0.5;
        // Moves the sword's grip along the handle relative to the controller, in cm (+ = towards the blade).
        double grip_shift_cm = 0.0;
        // Extra turn of the hands relative to the controllers, in degrees (pitch, yaw, roll).
        double hand_pitch = 0.0;
        double hand_yaw = 0.0;
        double hand_roll = 0.0;
        // ---- body follow: the character moves with the player (docs/phase4-body-follow.md) ----
        // 1 = the view is the player's own head and the character's head and upper body are sent
        // after it; 0 = the view rides on the character's head bone as before.
        bool body_follow = true;
        // The view is the headset, placed on the character when recentering. It only gives way
        // when the character's real head is further from it than view_leash_cm (a knock-down).
        // view_follow_seconds > 0 also lets it drift back to the real head over that time.
        double view_follow_seconds = 0.0;
        double view_leash_cm = 40.0;
        // 1 = the upper body is built from the headset and hands alone (a fixed standing shape,
        // turned and leaned to put the head at the headset); the game's own upper-body animation
        // is not used. 0 = the game's animation, leaned after the headset.
        bool own_upper_body = true;
        // How far the chest may turn away from the legs, and the most the upper body leans.
        double chest_turn_max_deg = 70.0;
        double lean_follow = 1.0;
        double lean_max_deg = 45.0;
        // 1 = the player's body is placed, not simulated: head, torso and legs are put exactly
        // where the pose says (head at the headset, legs from the game's leg animation) with no
        // muscles or balance in between, as in Blade & Sorcery. Only the hands and what they hold
        // stay physical. The body is given back to the game's simulation while the character's
        // consciousness is below kinematic_min_consciousness (knocked out, dead).
        bool kinematic_body = true;
        // 1 = the held body: every part of the player's body stays physical but is held at its
        // place in the pose by a very stiff spring in place and turn (the engine's own physical
        // animation spring, which the game only uses for turning). The pose is the player: head
        // at the headset, hands at the controllers, legs from the game's leg animation over the
        // feet. This replaced kinematic_body, whose rigid parts did not move with the character in
        // the physics (first test: "locked the arms into place even when i walked away").
        bool body_hold = false;
        double hold_strength = 20000.0;
        double hold_damping = 200.0;
        double hold_max_force = 0.0; // 0 = no limit
        double hold_turn_scale = 5.0; // the game's own turning strength times this
        // 1 = the hands are placed too (exactly at the controllers); only what they hold is physical.
        // 0 = the hands stay simulated on the wrist. That broke in the first test: once the player
        // walked, the hands ended up a metre from the controllers and turned backwards.
        bool kinematic_hands = false;
        // Physical hands as in Blade & Sorcery: each hand body is cut loose from the forearm (its
        // wrist joint is set free) and driven hard towards the controller (hand_pull_* below); the
        // arm is drawn to follow the hand. So a hand can be stopped: when the sword hits something
        // the hand stops with it. If the wrist cannot be freed the hands are placed rigidly instead.
        bool free_wrists = true;
        // 1 = the legs are the mod's own: feet stay planted on the ground and step when the body
        // has moved step_length_cm past them (or turned), lifting step_height_cm.
        bool own_legs = true;
        double step_length_cm = 20.0;
        double step_height_cm = 9.0;
        double kinematic_min_consciousness = 15.0;
        // 1 = the hips are lowered and the knees bent so the head can be at the headset's height.
        bool leg_ik = true;
        // Roomscale: the character walks, turns and ducks under the player's real body.
        // 0 = off (the body only leans). 2 = the character is moved directly under the headset
        // every frame. 1 = the character is told to walk there with the game's own walking.
        int roomscale = 2;
        // The headset may be this far from over the character's feet before the feet follow
        // (the upper body leans that far), and the fastest the feet follow, cm per second.
        double roomscale_lean_cm = 8.0;
        double roomscale_max_speed = 400.0;
        // The head may look this far to either side before the legs turn after it (0 = no turning),
        // and the fastest they turn, degrees per second.
        double roomscale_turn_deg = 25.0;
        double roomscale_turn_speed = 360.0;
        // The character crouches while the headset is this far below its recentered height (0 = never).
        double roomscale_crouch_cm = 0.0;
        // The crouch button crouches the player: while the game says the character is crouched,
        // the view is lowered this far (and the body with it, since hip height follows the head).
        double crouch_view_drop_cm = 40.0;
        // 1 = while the body is placed, the character may only be moved by walking (stick), by the
        // player's own steps (roomscale) or by a real teleport. The game's own balancing shifts the
        // character after its hips and feet; with the placed body that dragged the player sideways
        // 20 cm at a time and threw him across the room. Such moves are taken back.
        bool hold_ground = true;
        // The head body is held at the headset's place (0 strength = only its pose is sent).
        double head_pull_strength = 3000.0;
        double head_pull_damping = 80.0;
        double head_pull_max_force = 20000.0;
        bool head_turn = true;
        double head_turn_max_deg = 75.0;
        // 1 = the character's own head pieces (head model, hair, headgear) are not drawn for the
        // player in first person. Only a drawing flag is set; the head bone is never hidden, because
        // the game takes a hidden head bone for a severed head.
        bool hide_head = true;
        // Hand pull: besides turning each arm bone towards its pose (all the game's muscles do),
        // each hand is pulled to the place of its controller. Strength is the spring, damping
        // the brake, max_force 0 = no limit. The sword's weight still slows the hand, because the
        // pull works on the hand and the sword hangs from it.
        bool hand_pull = true;
        double hand_pull_strength = 12000.0;
        double hand_pull_damping = 150.0;
        double hand_pull_max_force = 30000.0;
        // The hand's own turning strength (wrist) times this, so the blade angle holds better.
        double hand_turn_scale = 5.0;
        // While a hand is pulled, the turning strength of that arm's other bones times this: the
        // arm follows the hand instead of carrying it (1 = the game's own arm muscles).
        double arm_soft_scale = 0.3;
        // Controller sticks and buttons play the game through a virtual gamepad (see
        // controller_input.hpp). pad_map lists "controller input=gamepad input" pairs.
        bool controller_pad = true;
        std::string pad_map = "l_stick=LSTICK r_stick_x=RSTICK_X l_grip=LB r_grip=RB r_lower=A r_click=RS l_click=LS l_menu=START";
        // Stick movement below this is ignored; above it the stick is scaled so that the game's own
        // gamepad dead zone (pad_game_deadzone) does not swallow small pushes.
        double pad_deadzone = 0.12;
        double pad_game_deadzone = 0.25;
        // Write the game's input bindings to the log once (to know what each gamepad input does).
        bool log_bindings = true;
        // Keep the game's guard ("R/L Hand Toggle") raised for a hand that holds a weapon, so no
        // button is needed to fight.
        bool auto_guard = true;
        // The mod's keys on the left controller: Y = recenter, Y held = menu panel, X held = motion arms.
        bool controller_mod_keys = true;
        // While a menu is up, point a controller at the menu panel and pull the trigger to click.
        bool controller_pointer = true;
        // Body holsters and hand-to-hand swaps: squeeze the grip with a hand at the hip (below
        // holster_hip_below_m under the eyes) or over the shoulder behind the head to draw or put
        // away; squeeze an empty hand next to the other hand's weapon to take it over.
        bool controller_gestures = true;
        double holster_hip_below_m = 0.45;
        double holster_back_behind_m = 0.08;
        double swap_distance_m = 0.15;
        // B on the right controller kicks.
        bool kick_on_b = true;
        // An empty hand squeezing near a weapon or food picks it up (within this distance of the hand).
        bool pickup = true;
        double pickup_radius_m = 0.30;
        // Food held up to the mouth is eaten.
        bool eat_at_mouth = true;
        // A grip squeeze on a hand that holds a weapon is not passed to the game as a grab (which drops it).
        bool keep_held_weapon = true;
        // Two grip squeezes within this many seconds on a hand holding a weapon drop it (0 = never).
        double drop_double_squeeze_s = 0.4;
        // The right trigger starts an 8-second arm recording (arms_record_N.csv). Off by default now
        // that the trigger guards.
        bool arms_recorder = false;
        // Write "Arms:" lines to the log (once a second): where each hand should be and where it is.
        bool log_arms = true;
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
        // Test without anyone holding the controllers: pretend hands in front of the chest.
        bool dev_fake_hands = false;
        // Test without anyone playing: drive the arms of the first fighter in the level.
        bool dev_arms_any_character = false;
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
    };

    // Current settings (copy). Cheap enough to call once per frame.
    Config config();
    // Loads the file, writing a default one if it is missing. Returns true if values changed.
    bool config_reload_if_changed(bool force = false);
    // Writes these keys into the settings file (replacing or adding lines) and reloads it.
    void config_store(const std::map<std::string, std::string>& values);
} // namespace hsvr
