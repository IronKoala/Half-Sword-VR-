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
        bool aim_sync = true;
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
