#include "config.hpp"

#include "log.hpp"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace hsvr
{
    static std::mutex g_cfg_mutex;
    static Config g_cfg;
    static FILETIME g_cfg_time{};
    static double g_last_check = -100.0;

    static std::wstring ini_path()
    {
        return mod_dir() + L"HalfSwordVR.ini";
    }

    static std::string trim(const std::string& s)
    {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return {};
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    static void write_default_file()
    {
        FILE* f = _wfopen(ini_path().c_str(), L"w");
        if (!f) return;
        Config d;
        fprintf(f,
                "; Half Sword VR settings. Edit and save; the mod re-reads this file while the game runs.\n"
                "enabled=%d\n"
                "; Only start VR if SteamVR is already running when the game starts.\n"
                "require_steamvr_running=%d\n"
                "; Per-eye resolution as a fraction of what the headset asks for (1.0 = full).\n"
                "render_scale=%.2f\n"
                "world_scale=%.2f\n"
                "; head = view from the character's head, camera = the game's own camera position.\n"
                "anchor=%s\n"
                "head_bone=%s\n"
                "; Straight ahead follows: aim (where you aim with the mouse) or camera (the game's camera).\n"
                "heading=%s\n"
                "eye_forward_cm=%.1f\n"
                "eye_up_cm=%.1f\n"
                "anchor_smoothing=%.3f\n"
                "near_clip_cm=%.1f\n"
                "gamma_mode=%d\n"
                "ui_panel=%d\n"
                "; 0 = panel always shown (the menu-panel key hides it); 1 = only while a menu is up.\n"
                "ui_panel_auto=%d\n"
                "ui_distance_m=%.2f\n"
                "ui_width_m=%.2f\n"
                "ui_height_offset_m=%.2f\n"
                "; Opacity of menus/HUD on the panel = brightness x ui_opacity_gain.\n"
                "ui_opacity_gain=%.1f\n"
                "; Dark sheet behind the panel while a menu is up: 0 = none, 1 = solid.\n"
                "ui_menu_backdrop=%.2f\n"
                "; 1 = also use the window's own alpha for the panel (coarse; off by default).\n"
                "ui_use_alpha=%d\n"
                "; Show the left eye's picture in the game window on the monitor.\n"
                "mirror_window=%d\n"
                "; Keys are Windows virtual key codes: 0x24 = Home (recenter), 0x23 = End (show/hide menu panel),\n"
                "; 0x2D = Insert (switch between first-person and the game's own third-person camera).\n"
                "recenter_key=0x%X\n"
                "toggle_ui_key=0x%X\n"
                "toggle_view_key=0x%X\n"
                "; 0x22 = Page Down: flip between full-resolution mode and the fallback while playing.\n"
                "toggle_mode_key=0x%X\n"
                "predict_frames=%.1f\n"
                "; Head positions older than this many finished pictures are not used (2 is right for this game; -1 = off).\n"
                "pose_max_age=%d\n"
                "; 1 = full headset resolution with menus on a floating panel, 0 = fallback through the game window.\n"
                "separate_render_target=%d\n"
                "; Picture tuning (-1 = leave the game's own value): sharpen 0 to 1, screen_percentage 50 to 100.\n"
                "sharpen=%.2f\n"
                "screen_percentage=%.0f\n"
                "; Extra engine console commands, separated by semicolons (for tuning; normally empty).\n"
                "extra_commands=%s\n"
                "; 1 = keep the clouds behind buildings in both eyes (works around an engine fault).\n"
                "fix_clouds=%d\n"
                "; Which cloud mode replaces the faulty one: 2 (thin far things stay clean) or 0.\n"
                "cloud_mode=%d\n"
                "; Bounce lighting: one light sample per this many pixels. The game uses 64, which flickers\n"
                "; in the headset. 32 = twice as dense, 16 = four times, 8 = eight times (each slower), 0 = leave the game's value.\n"
                "light_samples=%d\n"
                "; 1 = keep the game at normal speed while the headset is asleep.\n"
                "idle_helper=%d\n"
                "log_poses=%d\n"
                "; Record in the log what the keyboard, mouse pointer and any gamepad are doing.\n"
                "log_input=%d\n"
                "; Save troubleshooting pictures every this many seconds into the captures folder (0 = off).\n"
                "capture_seconds=%.0f\n"
                "stereo_slot_offset=%d\n"
                "dev_fake_headset=%d\n",
                d.enabled, d.require_steamvr_running, d.render_scale, d.world_scale, d.anchor.c_str(), d.head_bone.c_str(), d.heading.c_str(),
                d.eye_forward_cm, d.eye_up_cm, d.anchor_smoothing, d.near_clip_cm, d.gamma_mode, d.ui_panel, d.ui_panel_auto,
                d.ui_distance_m, d.ui_width_m, d.ui_height_offset_m, d.ui_opacity_gain, d.ui_menu_backdrop, d.ui_use_alpha, d.mirror_window, (unsigned)d.recenter_key, (unsigned)d.toggle_ui_key, (unsigned)d.toggle_view_key, (unsigned)d.toggle_mode_key,
                d.predict_frames, d.pose_max_age, d.separate_render_target, d.sharpen, d.screen_percentage, d.extra_commands.c_str(), d.fix_clouds, d.cloud_mode, d.light_samples, d.idle_helper, d.log_poses, d.log_input, d.capture_seconds, d.stereo_slot_offset, d.dev_fake_headset);
        fprintf(f,
                "; Controllers: write their positions and buttons to the log once a second.\n"
                "log_controllers=%d\n"
                "hand_bone_left=%s\n"
                "hand_bone_right=%s\n"
                "; 1 = short buzz when a trigger is pulled all the way (vibration check).\n"
                "haptics_test=%d\n",
                d.log_controllers, d.hand_bone_left.c_str(), d.hand_bone_right.c_str(), d.haptics_test);
        fclose(f);
    }

    // Writes one setting into the ini (replacing its line, or adding it), for values the mod measures
    // itself (your arm length). The 1 s reload then picks it up.
    void config_set(const std::string& key, const std::string& value)
    {
        std::vector<std::string> lines;
        bool found = false;
        if (FILE* f = _wfopen(ini_path().c_str(), L"r"))
        {
            char chunk[512];
            std::string line;
            while (fgets(chunk, sizeof(chunk), f))
            {
                line += chunk;
                if (line.back() != '\n') continue;
                std::string t = trim(line);
                size_t eq = t.find('=');
                if (!found && eq != std::string::npos && trim(t.substr(0, eq)) == key)
                {
                    line = key + "=" + value + "\n";
                    found = true;
                }
                lines.push_back(line);
                line.clear();
            }
            if (!line.empty()) lines.push_back(line + "\n");
            fclose(f);
        }
        if (!found) lines.push_back(key + "=" + value + "\n");
        if (FILE* out = _wfopen(ini_path().c_str(), L"w"))
        {
            for (const std::string& l : lines) std::fputs(l.c_str(), out);
            fclose(out);
        }
        HSVR_LOG("Settings: %s set to %s", key.c_str(), value.c_str());
    }

    Config config()
    {
        std::lock_guard<std::mutex> lock(g_cfg_mutex);
        return g_cfg;
    }

    bool config_reload_if_changed(bool force)
    {
        double t = now_seconds();
        {
            std::lock_guard<std::mutex> lock(g_cfg_mutex);
            if (!force && t - g_last_check < 1.0) return false;
            g_last_check = t;
        }
        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (!GetFileAttributesExW(ini_path().c_str(), GetFileExInfoStandard, &info))
        {
            write_default_file();
            if (!GetFileAttributesExW(ini_path().c_str(), GetFileExInfoStandard, &info)) return false;
        }
        {
            std::lock_guard<std::mutex> lock(g_cfg_mutex);
            if (!force && CompareFileTime(&info.ftLastWriteTime, &g_cfg_time) == 0) return false;
            g_cfg_time = info.ftLastWriteTime;
        }
        FILE* f = _wfopen(ini_path().c_str(), L"r");
        if (!f) return false;
        std::map<std::string, std::string> kv;
        // Lines of any length (a dev_script can be long): read in pieces up to the end of the line.
        char chunk[512];
        std::string line;
        bool more = true;
        while (more)
        {
            line.clear();
            more = false;
            while (fgets(chunk, sizeof(chunk), f))
            {
                more = true;
                line += chunk;
                if (!line.empty() && line.back() == '\n') break;
            }
            if (!more) break;
            std::string s = trim(line);
            if (s.empty() || s[0] == ';' || s[0] == '#' || s[0] == '[') continue;
            size_t eq = s.find('=');
            if (eq == std::string::npos) continue;
            kv[trim(s.substr(0, eq))] = trim(s.substr(eq + 1));
        }
        fclose(f);

        Config c;
        // Settings added in newer versions are missing from an older file: they get added at their
        // defaults (test switches, dev_*, are left out), so every setting can be found in the file.
        std::vector<std::string> missing;
        auto note_missing = [&](const char* key, const std::string& value) {
            if (kv.find(key) != kv.end() || std::strncmp(key, "dev_", 4) == 0) return;
            for (const std::string& m : missing)
                if (m.compare(0, std::strlen(key) + 1, std::string(key) + "=") == 0) return;
            missing.push_back(std::string(key) + "=" + value);
        };
        auto num = [&](const char* key, double& out) {
            auto it = kv.find(key);
            if (it != kv.end() && !it->second.empty()) out = strtod(it->second.c_str(), nullptr);
            char v[64];
            std::snprintf(v, sizeof(v), "%g", out);
            note_missing(key, v);
        };
        auto integer = [&](const char* key, int& out) {
            auto it = kv.find(key);
            if (it != kv.end() && !it->second.empty()) out = (int)strtol(it->second.c_str(), nullptr, 0);
            note_missing(key, std::to_string(out));
        };
        auto flag = [&](const char* key, bool& out) {
            int v = out ? 1 : 0;
            integer(key, v);
            out = v != 0;
        };
        auto text = [&](const char* key, std::string& out) {
            auto it = kv.find(key);
            if (it != kv.end() && !it->second.empty()) out = it->second;
            note_missing(key, out);
        };
        flag("enabled", c.enabled);
        flag("require_steamvr_running", c.require_steamvr_running);
        num("render_scale", c.render_scale);
        num("world_scale", c.world_scale);
        text("anchor", c.anchor);
        text("head_bone", c.head_bone);
        text("heading", c.heading);
        num("eye_forward_cm", c.eye_forward_cm);
        num("eye_up_cm", c.eye_up_cm);
        num("anchor_smoothing", c.anchor_smoothing);
        num("near_clip_cm", c.near_clip_cm);
        integer("gamma_mode", c.gamma_mode);
        flag("ui_panel", c.ui_panel);
        flag("ui_panel_auto", c.ui_panel_auto);
        flag("ui_pointer", c.ui_pointer);
        flag("menu_buttons", c.menu_buttons);
        num("menu_continue_hold_s", c.menu_continue_hold_s);
        num("ui_distance_m", c.ui_distance_m);
        num("ui_width_m", c.ui_width_m);
        num("ui_height_offset_m", c.ui_height_offset_m);
        num("ui_opacity_gain", c.ui_opacity_gain);
        num("ui_menu_backdrop", c.ui_menu_backdrop);
        flag("ui_use_alpha", c.ui_use_alpha);
        flag("mirror_window", c.mirror_window);
        integer("recenter_key", c.recenter_key);
        integer("toggle_ui_key", c.toggle_ui_key);
        integer("toggle_view_key", c.toggle_view_key);
        integer("toggle_mode_key", c.toggle_mode_key);
        num("predict_frames", c.predict_frames);
        integer("pose_max_age", c.pose_max_age);
        flag("separate_render_target", c.separate_render_target);
        num("sharpen", c.sharpen);
        num("screen_percentage", c.screen_percentage);
        text("extra_commands", c.extra_commands);
        flag("fix_clouds", c.fix_clouds);
        integer("cloud_mode", c.cloud_mode);
        integer("light_samples", c.light_samples);
        flag("wipe_standins", c.wipe_standins);
        flag("log_sliders", c.log_sliders);
        flag("idle_helper", c.idle_helper);
        flag("log_poses", c.log_poses);
        flag("log_input", c.log_input);
        num("capture_seconds", c.capture_seconds);
        flag("capture_menus_only", c.capture_menus_only);
        integer("stereo_slot_offset", c.stereo_slot_offset);
        flag("dev_fake_headset", c.dev_fake_headset);
        flag("dev_fake_still", c.dev_fake_still);
        num("dev_fake_yaw", c.dev_fake_yaw);
        num("dev_fake_pitch", c.dev_fake_pitch);
        num("dev_fake_scale", c.dev_fake_scale);
        text("dev_script", c.dev_script);
        flag("log_controllers", c.log_controllers);
        text("hand_bone_left", c.hand_bone_left);
        text("hand_bone_right", c.hand_bone_right);
        flag("haptics_test", c.haptics_test);
        flag("log_body", c.log_body);
        flag("dev_pose_probe", c.dev_pose_probe);
        text("dev_pose_bone", c.dev_pose_bone);
        num("dev_pose_pitch", c.dev_pose_pitch);
        num("dev_pose_yaw", c.dev_pose_yaw);
        num("dev_pose_roll", c.dev_pose_roll);
        flag("pose_body", c.pose_body);
        flag("pose_head", c.pose_head);
        flag("pose_head_lock", c.pose_head_lock);
        num("pose_head_lock_max_cm", c.pose_head_lock_max_cm);
        flag("pose_arms", c.pose_arms);
        flag("pose_follow_room", c.pose_follow_room);
        flag("pose_hold_capsule", c.pose_hold_capsule);
        flag("pose_body_turn", c.pose_body_turn);
        num("pose_turn_dead_zone_deg", c.pose_turn_dead_zone_deg);
        num("pose_turn_speed_dps", c.pose_turn_speed_dps);
        num("pose_turn_max_deg", c.pose_turn_max_deg);
        flag("pose_stick_walk", c.pose_stick_walk);
        text("pose_move_relative", c.pose_move_relative);
        text("pose_stick_turn", c.pose_stick_turn);
        num("pose_stick_turn_dps", c.pose_stick_turn_dps);
        num("pose_snap_turn_deg", c.pose_snap_turn_deg);
        num("pose_stick_dead_zone", c.pose_stick_dead_zone);
        flag("pose_grip_spring", c.pose_grip_spring);
        flag("pose_grip_hold", c.pose_grip_hold);
        num("pose_hold_strength", c.pose_hold_strength);
        flag("pose_body_strength", c.pose_body_strength);
        num("pose_body_stamina_floor", c.pose_body_stamina_floor);
        num("pose_body_strength_min", c.pose_body_strength_min);
        num("pose_stamina_scale", c.pose_stamina_scale);
        num("pose_stamina_swing_lo_cm_s", c.pose_stamina_swing_lo_cm_s);
        num("pose_stamina_swing_hi_cm_s", c.pose_stamina_swing_hi_cm_s);
        num("pose_stamina_speed_smooth_s", c.pose_stamina_speed_smooth_s);
        num("pose_stamina_force_cap_n", c.pose_stamina_force_cap_n);
        num("pose_stamina_torque_cap_nm", c.pose_stamina_torque_cap_nm);
        flag("pose_hold_exact", c.pose_hold_exact);
        num("pose_hold_spring", c.pose_hold_spring);
        num("pose_hold_damper", c.pose_hold_damper);
        num("pose_hold_max_force", c.pose_hold_max_force);
        num("pose_hold_turn_spring", c.pose_hold_turn_spring);
        num("pose_hold_turn_damper", c.pose_hold_turn_damper);
        num("pose_hold_max_torque", c.pose_hold_max_torque);
        flag("pose_hold_weight", c.pose_hold_weight);
        num("pose_hold_weight_torque_scale", c.pose_hold_weight_torque_scale);
        flag("pose_hold_weight_two_hand_couple", c.pose_hold_weight_two_hand_couple);
        flag("pose_hold_bs", c.pose_hold_bs);
        num("pose_bs_two_pos_k", c.pose_bs_two_pos_k);
        num("pose_bs_two_pos_c", c.pose_bs_two_pos_c);
        num("pose_bs_two_pos_max", c.pose_bs_two_pos_max);
        num("pose_bs_two_rot_k", c.pose_bs_two_rot_k);
        num("pose_bs_two_rot_c", c.pose_bs_two_rot_c);
        num("pose_bs_two_rot_max", c.pose_bs_two_rot_max);
        num("pose_bs_max_spin", c.pose_bs_max_spin);
        integer("pose_bs_iterations", c.pose_bs_iterations);
        num("pose_bs_two_let_go_cm", c.pose_bs_two_let_go_cm);
        flag("pose_hold_ignore_body", c.pose_hold_ignore_body);
        num("pose_hold_weight_spin_extra", c.pose_hold_weight_spin_extra);
        flag("pose_hold_hand_on_weapon", c.pose_hold_hand_on_weapon);
        num("pose_hold_blocked_from_cm", c.pose_hold_blocked_from_cm);
        flag("pose_hold_hand_follows", c.pose_hold_hand_follows);
        flag("pose_hold_hand_follows_draw", c.pose_hold_hand_follows_draw);
        num("pose_draw_force_scale_follows", c.pose_draw_force_scale_follows);
        flag("pose_hold_let_go", c.pose_hold_let_go);
        num("pose_hold_let_go_s", c.pose_hold_let_go_s);
        flag("pose_hold_let_go_ignore_self", c.pose_hold_let_go_ignore_self);
        num("pose_hold_let_go_grace_s", c.pose_hold_let_go_grace_s);
        num("pose_hold_contact_cm", c.pose_hold_contact_cm);
        flag("pose_hold_ignore_hand", c.pose_hold_ignore_hand);
        flag("pose_hold_block_offhand_search", c.pose_hold_block_offhand_search);
        flag("pose_slide", c.pose_slide);
        num("pose_slide_hand_half_cm", c.pose_slide_hand_half_cm);
        flag("pose_slide_zone_measured", c.pose_slide_zone_measured);
        num("pose_slide_wide_cm", c.pose_slide_wide_cm);
        num("pose_slide_rate_min", c.pose_slide_rate_min);
        num("pose_slide_rate_max", c.pose_slide_rate_max);
        num("pose_slide_max_cm_s", c.pose_slide_max_cm_s);
        flag("pose_grab_button", c.pose_grab_button);
        num("pose_grab_radius_cm", c.pose_grab_radius_cm);
        flag("pose_grab_handles", c.pose_grab_handles);
        num("pose_grab_touch_cm", c.pose_grab_touch_cm);
        num("pose_grab_handle_cm", c.pose_grab_handle_cm);
        num("pose_slide_keep_light", c.pose_slide_keep_light);
        num("pose_slide_keep_full", c.pose_slide_keep_full);
        num("pose_slide_damper_light", c.pose_slide_damper_light);
        num("pose_slide_damper_full", c.pose_slide_damper_full);
        num("pose_grab_settle_s", c.pose_grab_settle_s);
        flag("pose_shield_free_grip", c.pose_shield_free_grip);
        flag("pose_weapon_free_grip", c.pose_weapon_free_grip);
        num("pose_weapon_free_cm", c.pose_weapon_free_cm);
        num("pose_shield_point_cm", c.pose_shield_point_cm);
        flag("pose_shield_in_hand", c.pose_shield_in_hand);
        num("pose_shield_hand_grip_type", c.pose_shield_hand_grip_type);
        flag("pose_two_hand", c.pose_two_hand);
        num("pose_two_hand_strength", c.pose_two_hand_strength);
        num("pose_two_hand_blade_gap_cm", c.pose_two_hand_blade_gap_cm);
        num("pose_two_hand_reach_cm", c.pose_two_hand_reach_cm);
        num("pose_two_hand_let_go_cm", c.pose_two_hand_let_go_cm);
        flag("pose_two_hand_curl", c.pose_two_hand_curl);
        flag("pose_fingers_anim_only", c.pose_fingers_anim_only);
        flag("pose_feed_swing_speed", c.pose_feed_swing_speed);
        flag("pose_grab_seat", c.pose_grab_seat);
        flag("pose_throw", c.pose_throw);
        num("pose_throw_speed", c.pose_throw_speed);
        flag("pose_holster_snap", c.pose_holster_snap);
        flag("log_frames", c.log_frames);
        flag("pose_keep_scabbard_on_drop", c.pose_keep_scabbard_on_drop);
        num("pose_holster_zone_cm", c.pose_holster_zone_cm);
        num("pose_holster_max_speed", c.pose_holster_max_speed);
        num("pose_throw_multiplier", c.pose_throw_multiplier);
        flag("pose_draw", c.pose_draw);
        num("pose_draw_touch_cm", c.pose_draw_touch_cm);
        num("pose_draw_settle_s", c.pose_draw_settle_s);
        num("pose_draw_step_cm", c.pose_draw_step_cm);
        num("pose_draw_force_scale", c.pose_draw_force_scale);
        flag("pose_draw_scabbard_free", c.pose_draw_scabbard_free);
        num("pose_hold_stuck_force_scale", c.pose_hold_stuck_force_scale);
        flag("stab_wound_hold", c.stab_wound_hold);
        flag("grip_flip", c.grip_flip);
        num("stab_yank_cm", c.stab_yank_cm);
        num("stab_yank_s", c.stab_yank_s);
        num("grip_flip_s", c.grip_flip_s);
        num("grip_flip_tap_s", c.grip_flip_tap_s);
        num("stab_wound_settle_s", c.stab_wound_settle_s);
        num("stab_wound_settle_max_s", c.stab_wound_settle_max_s);
        num("stab_wound_entry_cms", c.stab_wound_entry_cms);
        num("stab_start_depth_cm", c.stab_start_depth_cm);
        num("stab_start_depth_per_ms", c.stab_start_depth_per_ms);
        num("stab_start_depth_max_cm", c.stab_start_depth_max_cm);
        num("stab_wound_in_cms", c.stab_wound_in_cms);
        num("stab_wound_out_cms", c.stab_wound_out_cms);
        num("stab_wound_head_factor", c.stab_wound_head_factor);
        flag("pose_sheathe", c.pose_sheathe);
        num("pose_sheathe_mouth_cm", c.pose_sheathe_mouth_cm);
        num("pose_sheathe_angle_deg", c.pose_sheathe_angle_deg);
        flag("pose_sheathe_cross", c.pose_sheathe_cross);
        flag("pose_scabbard_grab", c.pose_scabbard_grab);
        num("pose_scabbard_touch_cm", c.pose_scabbard_touch_cm);
        num("pose_scabbard_angle_deg", c.pose_scabbard_angle_deg);
        num("pose_scabbard_follow_s", c.pose_scabbard_follow_s);
        num("pose_scabbard_let_go_cm", c.pose_scabbard_let_go_cm);
        num("pose_scabbard_grip_type", c.pose_scabbard_grip_type);
        flag("pose_scabbard_block_auto", c.pose_scabbard_block_auto);
        num("pose_grab_seat_reverse_bias_deg", c.pose_grab_seat_reverse_bias_deg);
        num("pose_swing_speed_lo", c.pose_swing_speed_lo);
        num("pose_swing_speed_hi", c.pose_swing_speed_hi);
        num("pose_swing_speed_fall_s", c.pose_swing_speed_fall_s);
        flag("pose_two_hand_lock_rotation", c.pose_two_hand_lock_rotation);
        {
            double m = c.pose_two_hand_mirror;
            num("pose_two_hand_mirror", m);
            c.pose_two_hand_mirror = (int)m;
        }
        flag("pose_fists", c.pose_fists);
        text("pose_fist_class", c.pose_fist_class);
        flag("pose_fist_mark_spawned", c.pose_fist_mark_spawned);
        flag("pose_eat", c.pose_eat);
        num("pose_eat_cm", c.pose_eat_cm);
        num("pose_drink_tip", c.pose_drink_tip);
        num("pose_drink_sip_s", c.pose_drink_sip_s);
        flag("pose_grab_release_drops", c.pose_grab_release_drops);
        flag("pose_grab_objects", c.pose_grab_objects);
        flag("pose_ghost_hand", c.pose_ghost_hand);
        num("pose_ghost_from_cm", c.pose_ghost_from_cm);
        text("pose_ghost_mesh", c.pose_ghost_mesh);
        text("pose_ghost_material", c.pose_ghost_material);
        num("pose_ghost_x_cm", c.pose_ghost_x_cm);
        num("pose_ghost_y_cm", c.pose_ghost_y_cm);
        num("pose_ghost_z_cm", c.pose_ghost_z_cm);
        num("pose_ghost_pitch", c.pose_ghost_pitch);
        num("pose_ghost_yaw", c.pose_ghost_yaw);
        num("pose_ghost_roll", c.pose_ghost_roll);
        num("pose_ghost_scale", c.pose_ghost_scale);
        num("pose_user_arm_cm", c.pose_user_arm_cm);
        num("pose_shoulder_reach_cm", c.pose_shoulder_reach_cm);
        num("pose_shoulder_reach_start_cm", c.pose_shoulder_reach_start_cm);
        num("pose_shoulder_reach_max_deg", c.pose_shoulder_reach_max_deg);
        num("pose_grip_strength_kg", c.pose_grip_strength_kg);
        num("pose_grip_hz", c.pose_grip_hz);
        num("pose_grip_damping", c.pose_grip_damping);
        num("pose_grip_radius_cm", c.pose_grip_radius_cm);
        num("pose_grip_buzz", c.pose_grip_buzz);
        num("pose_grip_buzz_from_cm", c.pose_grip_buzz_from_cm);
        num("pose_grip_turn_scale", c.pose_grip_turn_scale);
        num("pose_grip_turn_damping", c.pose_grip_turn_damping);
        num("pose_grip_turn_max_scale", c.pose_grip_turn_max_scale);
        flag("pose_grip_anchor_marker", c.pose_grip_anchor_marker);
        flag("pose_move_bodies", c.pose_move_bodies);
        num("pose_teleport_above_cm", c.pose_teleport_above_cm);
        flag("pose_crouch", c.pose_crouch);
        num("pose_crouch_dead_zone_cm", c.pose_crouch_dead_zone_cm);
        num("pose_crouch_button_cm", c.pose_crouch_button_cm);
        num("pose_crouch_button_cms", c.pose_crouch_button_cms);
        flag("kick_on_a", c.kick_on_a);
        num("kick_tap_s", c.kick_tap_s);
        flag("pose_crouch_keep_leg_ik", c.pose_crouch_keep_leg_ik);
        flag("pose_capsule_floor_clamp", c.pose_capsule_floor_clamp);
        num("pose_wrist_forward_cm", c.pose_wrist_forward_cm);
        num("pose_wrist_right_cm", c.pose_wrist_right_cm);
        num("pose_wrist_up_cm", c.pose_wrist_up_cm);
        num("pose_hand_pitch", c.pose_hand_pitch);
        num("pose_hand_yaw", c.pose_hand_yaw);
        num("pose_hand_roll", c.pose_hand_roll);
        num("pose_knuckle_deg", c.pose_knuckle_deg);
        num("pose_swivel_smooth_s", c.pose_swivel_smooth_s);
        num("pose_forearm_twist_share", c.pose_forearm_twist_share);
        num("pose_torso_twist", c.pose_torso_twist);
        flag("pose_feet", c.pose_feet);
        num("pose_step_distance_cm", c.pose_step_distance_cm);
        num("pose_step_angle_deg", c.pose_step_angle_deg);
        num("pose_step_time_s", c.pose_step_time_s);
        num("pose_step_lift_cm", c.pose_step_lift_cm);
        num("pose_step_lead_s", c.pose_step_lead_s);
        flag("dev_pose_fake_tracking", c.dev_pose_fake_tracking);
        flag("dev_grip_trace", c.dev_grip_trace);
        flag("stab_log", c.stab_log);
        flag("stab_assist", c.stab_assist);
        num("stab_min_speed_cms", c.stab_min_speed_cms);
        num("stab_min_into_cos", c.stab_min_into_cos);
        num("stab_arm_mass_kg", c.stab_arm_mass_kg);
        num("stab_push_force_n", c.stab_push_force_n);
        num("stab_push_time_s", c.stab_push_time_s);
        flag("hide_own_head", c.hide_own_head);
        text("hide_own_gore_from", c.hide_own_gore_from);
        if (c.render_scale < 0.2) c.render_scale = 0.2;
        if (c.render_scale > 2.0) c.render_scale = 2.0;
        if (c.world_scale < 0.1) c.world_scale = 0.1;
        if (c.near_clip_cm < 0.5) c.near_clip_cm = 0.5;
        {
            std::lock_guard<std::mutex> lock(g_cfg_mutex);
            g_cfg = c;
        }
        if (!missing.empty())
        {
            if (FILE* out = _wfopen(ini_path().c_str(), L"a"))
            {
                std::fputs("\n; Added by HalfSwordVR: settings from a newer version, at their defaults (see the docs for what each does).\n", out);
                for (const std::string& m : missing) std::fprintf(out, "%s\n", m.c_str());
                fclose(out);
                HSVR_LOG("Settings: added %d new settings to the ini at their defaults", (int)missing.size());
            }
        }
        HSVR_LOG("Settings loaded: render_scale=%.2f world_scale=%.2f anchor=%s head_bone=%s eye_fwd=%.1f eye_up=%.1f smoothing=%.3f "
                 "near=%.1f gamma=%d ui_panel=%d separate_rt=%d",
                 c.render_scale, c.world_scale, c.anchor.c_str(), c.head_bone.c_str(), c.eye_forward_cm, c.eye_up_cm,
                 c.anchor_smoothing, c.near_clip_cm, c.gamma_mode, c.ui_panel, c.separate_render_target);
        return true;
    }
} // namespace hsvr
