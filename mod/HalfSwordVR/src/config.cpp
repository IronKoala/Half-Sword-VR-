#include "config.hpp"

#include "log.hpp"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstddef>
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
                "; 1 = VR and the mod only when SteamVR is already running as the game starts; otherwise the game\n"
                "; plays flat as if the mod weren't there. 0 = always start VR (opens SteamVR if it is closed).\n"
                "vr_auto=%d\n"
                "vr_auto_processes=%s\n"
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
                d.enabled, d.vr_auto, d.vr_auto_processes.c_str(), d.render_scale, d.world_scale, d.anchor.c_str(), d.head_bone.c_str(), d.heading.c_str(),
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

    // The key on an ini line ("" for comments, section headers and blank lines).
    static std::string line_key(const std::string& line)
    {
        std::string t = trim(line);
        if (t.empty() || t[0] == ';' || t[0] == '#' || t[0] == '[') return {};
        size_t eq = t.find('=');
        return eq == std::string::npos ? std::string() : trim(t.substr(0, eq));
    }

    struct MissingSetting
    {
        std::string key;
        std::string line;  // key=default
        std::string after; // the setting it goes under ("" = none in the file)
    };

    // Puts each missing setting on the line after the setting it follows in the reading order (its
    // last line, as the last one counts), so it lands in that setting's section. One with nothing
    // to follow goes at the end, under a note, as before.
    static void add_missing_settings(const std::vector<MissingSetting>& missing)
    {
        std::vector<std::string> lines;
        if (FILE* f = _wfopen(ini_path().c_str(), L"r"))
        {
            char chunk[512];
            std::string line;
            while (fgets(chunk, sizeof(chunk), f))
            {
                line += chunk;
                if (line.back() != '\n') continue;
                lines.push_back(line);
                line.clear();
            }
            if (!line.empty()) lines.push_back(line + "\n");
            fclose(f);
        }
        else
            return;
        std::vector<std::string> tail;
        int placed = 0;
        for (const MissingSetting& m : missing)
        {
            bool found = false;
            size_t at = 0;
            if (!m.after.empty())
                for (size_t i = lines.size(); i-- > 0;)
                    if (line_key(lines[i]) == m.after)
                    {
                        found = true;
                        at = i + 1;
                        break;
                    }
            if (!found)
            {
                tail.push_back(m.line + "\n");
                continue;
            }
            lines.insert(lines.begin() + (std::ptrdiff_t)at, m.line + "\n");
            ++placed;
        }
        if (!tail.empty())
        {
            lines.push_back("\n; Added by HalfSwordVR: settings from a newer version, at their defaults (see the docs for what each does).\n");
            lines.insert(lines.end(), tail.begin(), tail.end());
        }
        if (FILE* out = _wfopen(ini_path().c_str(), L"w"))
        {
            for (const std::string& l : lines) std::fputs(l.c_str(), out);
            fclose(out);
            HSVR_LOG("Settings: added %d new settings to the ini at their defaults (%d next to their section, %d at the end)",
                     (int)missing.size(), placed, (int)tail.size());
        }
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
        // Each goes in right under the setting read just before it below (the one that is in the file),
        // so it lands in that setting's section of the organized ini: read a new setting next to the
        // ones it belongs with.
        std::vector<MissingSetting> missing;
        std::string last_in_file;
        auto note_missing = [&](const char* key, const std::string& value) {
            if (kv.find(key) != kv.end())
            {
                last_in_file = key;
                return;
            }
            if (std::strncmp(key, "dev_", 4) == 0) return;
            for (const MissingSetting& m : missing)
                if (m.key == key) return;
            missing.push_back({key, std::string(key) + "=" + value, last_in_file});
            last_in_file = key;
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
        integer("vr_auto", c.vr_auto);
        text("vr_auto_processes", c.vr_auto_processes);
        {
            // Older name: only read when it is in the file, never added.
            auto it = kv.find("require_steamvr_running");
            if (it != kv.end() && !it->second.empty()) c.require_steamvr_running = strtol(it->second.c_str(), nullptr, 0) != 0;
        }
        num("render_scale", c.render_scale);
        num("world_scale", c.world_scale);
        text("anchor", c.anchor);
        text("head_bone", c.head_bone);
        text("heading", c.heading);
        num("eye_forward_cm", c.eye_forward_cm);
        num("eye_up_cm", c.eye_up_cm);
        num("anchor_smoothing", c.anchor_smoothing);
        flag("tracking_current_anchor", c.tracking_current_anchor);
        flag("anchor_room_unsmoothed", c.anchor_room_unsmoothed);
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
        flag("test_room", c.test_room);
        flag("test_room_button", c.test_room_button);
        text("test_room_map", c.test_room_map);
        flag("test_room_blackout", c.test_room_blackout);
        flag("test_room_books", c.test_room_books);
        num("test_room_book_light", c.test_room_book_light);
        num("test_room_fight_x", c.test_room_fight_x);
        num("test_room_fight_y", c.test_room_fight_y);
        integer("test_room_foes", c.test_room_foes);
        integer("test_room_tier", c.test_room_tier);
        integer("test_room_weapon_tier", c.test_room_weapon_tier);
        flag("test_room_respawn", c.test_room_respawn);
        num("test_room_respawn_seconds", c.test_room_respawn_seconds);
        num("test_room_despawn_seconds", c.test_room_despawn_seconds);
        integer("test_room_crossbow", c.test_room_crossbow);
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
        flag("pose_lean", c.pose_lean);
        flag("pose_lean_head_angle", c.pose_lean_head_angle);
        num("pose_lean_look_down_back_cm", c.pose_lean_look_down_back_cm);
        num("pose_lean_pitch_dead_deg", c.pose_lean_pitch_dead_deg);
        num("pose_lean_pitch_gain", c.pose_lean_pitch_gain);
        num("pose_lean_max_deg", c.pose_lean_max_deg);
        num("pose_lean_roll_dead_deg", c.pose_lean_roll_dead_deg);
        num("pose_lean_roll_gain", c.pose_lean_roll_gain);
        num("pose_lean_side_max_deg", c.pose_lean_side_max_deg);
        num("pose_lean_smooth_s", c.pose_lean_smooth_s);
        num("pose_lean_slack_cm", c.pose_lean_slack_cm);
        num("pose_lean_step_radius_cm", c.pose_lean_step_radius_cm);
        num("pose_room_neck_base_cm", c.pose_room_neck_base_cm);
        num("pose_lean_walk_catchup_s", c.pose_lean_walk_catchup_s);
        flag("pose_lean_follow_past_radius", c.pose_lean_follow_past_radius);
        num("pose_lean_follow_speed_cms", c.pose_lean_follow_speed_cms);
        num("pose_lean_settle_s", c.pose_lean_settle_s);
        num("pose_lean_back_radius_cm", c.pose_lean_back_radius_cm);
        num("pose_lean_settle_speed_cms", c.pose_lean_settle_speed_cms);
        flag("pose_lean_cancel_game_bend", c.pose_lean_cancel_game_bend);
        flag("pose_no_uro", c.pose_no_uro);
        flag("pose_helmet_late", c.pose_helmet_late);
        flag("pose_mesh_after_driver", c.pose_mesh_after_driver);
        flag("pose_arms", c.pose_arms);
        flag("pose_follow_room", c.pose_follow_room);
        flag("pose_hold_capsule", c.pose_hold_capsule);
        flag("pose_body_turn", c.pose_body_turn);
        flag("pose_mouse_off_with_controllers", c.pose_mouse_off_with_controllers);
        num("pose_turn_dead_zone_deg", c.pose_turn_dead_zone_deg);
        num("pose_turn_speed_dps", c.pose_turn_speed_dps);
        num("pose_turn_max_deg", c.pose_turn_max_deg);
        flag("pose_turn_spawn_guard", c.pose_turn_spawn_guard);
        flag("pose_calib_body_facing", c.pose_calib_body_facing);
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
        num("pose_stamina_gate", c.pose_stamina_gate);
        num("pose_stamina_fist_scale", c.pose_stamina_fist_scale);
        flag("pose_no_lawnmower", c.pose_no_lawnmower);
        num("pose_stamina_force_cap_n", c.pose_stamina_force_cap_n);
        num("pose_stamina_torque_cap_nm", c.pose_stamina_torque_cap_nm);
        flag("pose_hold_exact", c.pose_hold_exact);
        num("pose_hold_spring", c.pose_hold_spring);
        num("pose_hold_damper", c.pose_hold_damper);
        num("pose_hold_max_force", c.pose_hold_max_force);
        num("pose_hold_turn_spring", c.pose_hold_turn_spring);
        num("pose_hold_turn_damper", c.pose_hold_turn_damper);
        num("pose_hold_max_torque", c.pose_hold_max_torque);
        num("pose_hold_wrist_free_deg", c.pose_hold_wrist_free_deg);
        num("pose_hold_wrist_stop_deg", c.pose_hold_wrist_stop_deg);
        num("pose_hold_wrist_stop_torque", c.pose_hold_wrist_stop_torque);
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
        flag("pose_hold_ignore_own_helmet", c.pose_hold_ignore_own_helmet);
        flag("hold_blocker_log", c.hold_blocker_log);
        num("pose_hold_weight_spin_extra", c.pose_hold_weight_spin_extra);
        flag("pose_hold_hand_on_weapon", c.pose_hold_hand_on_weapon);
        flag("pose_arm_solve_drawn_hand", c.pose_arm_solve_drawn_hand);
        num("pose_hold_blocked_from_cm", c.pose_hold_blocked_from_cm);
        flag("pose_hold_hand_follows", c.pose_hold_hand_follows);
        integer("pose_hold_hand_follows_predict", c.pose_hold_hand_follows_predict);
        num("pose_hold_follow_predict_max_cm", c.pose_hold_follow_predict_max_cm);
        num("pose_hold_follow_predict_smooth_s", c.pose_hold_follow_predict_smooth_s);
        flag("pose_hold_hand_follows_draw", c.pose_hold_hand_follows_draw);
        num("pose_draw_force_scale_follows", c.pose_draw_force_scale_follows);
        flag("pose_hold_let_go", c.pose_hold_let_go);
        num("pose_hold_let_go_s", c.pose_hold_let_go_s);
        flag("pose_hold_let_go_ignore_self", c.pose_hold_let_go_ignore_self);
        num("pose_hold_let_go_grace_s", c.pose_hold_let_go_grace_s);
        flag("pose_hold_let_go_dead", c.pose_hold_let_go_dead);
        flag("pose_hold_measured_v", c.pose_hold_measured_v);
        flag("pose_hold_joint", c.pose_hold_joint);
        num("pose_hold_joint_stiffness", c.pose_hold_joint_stiffness);
        num("pose_hold_joint_damping", c.pose_hold_joint_damping);
        num("pose_hold_joint_max", c.pose_hold_joint_max);
        flag("pose_hold_joint_shoulder", c.pose_hold_joint_shoulder);
        flag("pose_hold_joint_carry_weight", c.pose_hold_joint_carry_weight);
        num("pose_hold_joint_fade_min_speed", c.pose_hold_joint_fade_min_speed);
        num("pose_hold_joint_fade_keep_s", c.pose_hold_joint_fade_keep_s);
        num("pose_hold_joint_fade_after_s", c.pose_hold_joint_fade_after_s);
        flag("pose_hold_joint_slerp", c.pose_hold_joint_slerp);
        flag("pose_hold_joint_mod_turn", c.pose_hold_joint_mod_turn);
        num("pose_hold_joint_target_max_speed", c.pose_hold_joint_target_max_speed);
        flag("pose_hold_joint_draw_wound", c.pose_hold_joint_draw_wound);
        flag("pose_hold_joint_turn_by_grip", c.pose_hold_joint_turn_by_grip);
        flag("pose_hold_joint_speed_limit", c.pose_hold_joint_speed_limit);
        num("pose_hold_contact_cm", c.pose_hold_contact_cm);
        flag("pose_hold_ignore_hand", c.pose_hold_ignore_hand);
        flag("pose_hold_block_offhand_search", c.pose_hold_block_offhand_search);
        flag("pose_slide", c.pose_slide);
        num("pose_slide_hand_half_cm", c.pose_slide_hand_half_cm);
        flag("pose_slide_zone_measured", c.pose_slide_zone_measured);
        num("pose_slide_wide_cm", c.pose_slide_wide_cm);
        flag("pose_grab_from_controller", c.pose_grab_from_controller);
        num("pose_grab_controller_gap_cm", c.pose_grab_controller_gap_cm);
        num("pose_grab_controller_max_cm", c.pose_grab_controller_max_cm);
        flag("pose_scabbard_handle", c.pose_scabbard_handle);
        flag("pose_grab_weapon_handles_only", c.pose_grab_weapon_handles_only);
        num("pose_hold_damping_ratio", c.pose_hold_damping_ratio);
        num("pose_hold_target_vel_smooth_s", c.pose_hold_target_vel_smooth_s);
        flag("pose_body_size_strength", c.pose_body_size_strength);
        flag("pose_hold_arm_lag", c.pose_hold_arm_lag);
        num("pose_hold_arm_lag_s", c.pose_hold_arm_lag_s);
        num("pose_hold_arm_lag_max_s", c.pose_hold_arm_lag_max_s);
        num("pose_hold_heft_lag_s", c.pose_hold_heft_lag_s);
        num("pose_hold_arm_reach_cm", c.pose_hold_arm_reach_cm);
        num("pose_hold_shoulder_torque", c.pose_hold_shoulder_torque);
        num("pose_hold_shoulder_fade_from_cm", c.pose_hold_shoulder_fade_from_cm);
        num("pose_hold_shoulder_fade_to_cm", c.pose_hold_shoulder_fade_to_cm);
        num("pose_hold_let_go_reach_slack_cm", c.pose_hold_let_go_reach_slack_cm);
        num("pose_hold_wrist_stop_spin", c.pose_hold_wrist_stop_spin);
        num("pose_hold_torque_strength_power", c.pose_hold_torque_strength_power);
        num("pose_slide_ring_max_len_cm", c.pose_slide_ring_max_len_cm);
        num("pose_slide_ring_max_width_cm", c.pose_slide_ring_max_width_cm);
        num("pose_slide_gap_max_cm", c.pose_slide_gap_max_cm);
        flag("pose_slide_rondel_stops", c.pose_slide_rondel_stops);
        flag("pose_slide_butt_is_haft", c.pose_slide_butt_is_haft);
        flag("pose_slide_fixed_1h_sword", c.pose_slide_fixed_1h_sword);
        integer("pose_grip_two_zones_2h_sword", c.pose_grip_two_zones_2h_sword);
        num("pose_grip_pommel_top_cm", c.pose_grip_pommel_top_cm);
        num("pose_grip_hands_overlap_cm", c.pose_grip_hands_overlap_cm);
        flag("pose_grip_dagger_fixed", c.pose_grip_dagger_fixed);
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
        flag("pose_slide_bs_damper", c.pose_slide_bs_damper);
        flag("draw_seat_ref_fix", c.draw_seat_ref_fix);
        num("pose_grab_settle_s", c.pose_grab_settle_s);
        flag("pose_grab_joint", c.pose_grab_joint);
        num("pose_grab_joint_hz", c.pose_grab_joint_hz);
        flag("pose_shield_free_grip", c.pose_shield_free_grip);
        flag("pose_weapon_free_grip", c.pose_weapon_free_grip);
        num("pose_weapon_free_cm", c.pose_weapon_free_cm);
        num("pose_shield_point_cm", c.pose_shield_point_cm);
        flag("pose_shield_in_hand", c.pose_shield_in_hand);
        num("pose_shield_hand_grip_type", c.pose_shield_hand_grip_type);
        flag("pose_shield_fixed_grip", c.pose_shield_fixed_grip);
        flag("pose_shield_grab_rim", c.pose_shield_grab_rim);
        num("pose_shield_rim_cm", c.pose_shield_rim_cm);
        flag("pose_shield_grab_face", c.pose_shield_grab_face);
        flag("pose_pavise_forearm", c.pose_pavise_forearm);
        flag("pose_pavise_strap", c.pose_pavise_strap);
        num("pose_pavise_mount_cm", c.pose_pavise_mount_cm);
        flag("pose_two_hand", c.pose_two_hand);
        num("pose_two_hand_strength", c.pose_two_hand_strength);
        num("pose_two_hand_blade_gap_cm", c.pose_two_hand_blade_gap_cm);
        num("pose_two_hand_reach_cm", c.pose_two_hand_reach_cm);
        num("pose_two_hand_let_go_cm", c.pose_two_hand_let_go_cm);
        flag("pose_two_hand_curl", c.pose_two_hand_curl);
        flag("pose_fingers_anim_only", c.pose_fingers_anim_only);
        flag("pose_in_anim_update", c.pose_in_anim_update);
        flag("pose_post_physics", c.pose_post_physics);
        num("pose_actor_z_smooth_s", c.pose_actor_z_smooth_s);
        flag("pose_feed_swing_speed", c.pose_feed_swing_speed);
        flag("pose_grab_seat", c.pose_grab_seat);
        flag("pose_throw", c.pose_throw);
        num("pose_throw_speed", c.pose_throw_speed);
        flag("pose_holster_snap", c.pose_holster_snap);
        flag("log_frames", c.log_frames);
        flag("replay_capture", c.replay_capture);
        num("replay_hold_s", c.replay_hold_s);
        num("replay_before_s", c.replay_before_s);
        num("replay_after_s", c.replay_after_s);
        flag("pose_keep_scabbard_on_drop", c.pose_keep_scabbard_on_drop);
        flag("crossbow", c.crossbow);
        num("crossbow_string_reach_cm", c.crossbow_string_reach_cm);
        num("crossbow_quiver_reach_cm", c.crossbow_quiver_reach_cm);
        num("crossbow_bolt_seat_cm", c.crossbow_bolt_seat_cm);
        num("crossbow_trigger_fire", c.crossbow_trigger_fire);
        num("crossbow_trigger_release", c.crossbow_trigger_release);
        num("crossbow_trigger_grip_cm", c.crossbow_trigger_grip_cm);
        num("pose_holster_zone_cm", c.pose_holster_zone_cm);
        flag("pose_holster_by_item", c.pose_holster_by_item);
        num("pose_holster_max_speed", c.pose_holster_max_speed);
        num("pose_throw_multiplier", c.pose_throw_multiplier);
        flag("pose_draw", c.pose_draw);
        num("pose_draw_touch_cm", c.pose_draw_touch_cm);
        num("pose_draw_zone_cm", c.pose_draw_zone_cm);
        num("pose_draw_settle_s", c.pose_draw_settle_s);
        num("pose_draw_step_cm", c.pose_draw_step_cm);
        num("pose_draw_force_scale", c.pose_draw_force_scale);
        flag("pose_draw_scabbard_free", c.pose_draw_scabbard_free);
        num("pose_hold_stuck_force_scale", c.pose_hold_stuck_force_scale);
        flag("stab_wound_hold", c.stab_wound_hold);
        flag("stab_no_rubber_band", c.stab_no_rubber_band);
        num("stab_pickup_grace_s", c.stab_pickup_grace_s);
        flag("stab_drag", c.stab_drag);
        num("stab_drag_force_scale", c.stab_drag_force_scale);
        num("stab_drag_grip_s", c.stab_drag_grip_s);
        num("stab_drag_reach_slack_cm", c.stab_drag_reach_slack_cm);
        num("stab_drag_reach_s", c.stab_drag_reach_s);
        flag("hit_assist", c.hit_assist);
        num("hit_arm_mass_kg", c.hit_arm_mass_kg);
        num("hit_assist_min_cms", c.hit_assist_min_cms);
        flag("cut_follow_through", c.cut_follow_through);
        num("cut_release_cm", c.cut_release_cm);
        num("cut_release_after_s", c.cut_release_after_s);
        flag("grip_flip", c.grip_flip);
        flag("grip_flip_short_only", c.grip_flip_short_only);
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
        num("stab_wound_head_in_factor", c.stab_wound_head_in_factor);
        flag("pose_sheathe", c.pose_sheathe);
        num("pose_sheathe_mouth_cm", c.pose_sheathe_mouth_cm);
        num("pose_sheathe_angle_deg", c.pose_sheathe_angle_deg);
        flag("pose_sheathe_two_hand", c.pose_sheathe_two_hand);
        flag("pose_scabbard_on_pickup", c.pose_scabbard_on_pickup);
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
        flag("pose_two_hand_twist_only", c.pose_two_hand_twist_only);
        flag("pose_two_hand_joint_both_turn", c.pose_two_hand_joint_both_turn);
        flag("pose_two_hand_fixed_rebase", c.pose_two_hand_fixed_rebase);
        num("pose_two_hand_swing_share", c.pose_two_hand_swing_share);
        flag("pose_fists", c.pose_fists);
        text("pose_fist_class", c.pose_fist_class);
        flag("pose_fist_mark_spawned", c.pose_fist_mark_spawned);
        flag("pose_fist_from_fingers", c.pose_fist_from_fingers);
        num("pose_fist_index_on", c.pose_fist_index_on);
        num("pose_fist_index_off", c.pose_fist_index_off);
        num("pose_fist_grip_on", c.pose_fist_grip_on);
        num("pose_fist_grip_off", c.pose_fist_grip_off);
        num("pose_fist_thumb_on", c.pose_fist_thumb_on);
        num("pose_fist_thumb_off", c.pose_fist_thumb_off);
        flag("pose_fist_slap", c.pose_fist_slap);
        num("pose_fist_slap_ass", c.pose_fist_slap_ass);
        num("pose_fist_slap_keep_s", c.pose_fist_slap_keep_s);
        flag("pose_fist_clear", c.pose_fist_clear);
        num("pose_fist_clear_margin_cm", c.pose_fist_clear_margin_cm);
        num("pose_fist_clear_retry_s", c.pose_fist_clear_retry_s);
        flag("pose_fist_clear_dropped", c.pose_fist_clear_dropped);
        flag("pose_fist_full_impulse", c.pose_fist_full_impulse);
        flag("disarm", c.disarm);
        num("disarm_grip_scale", c.disarm_grip_scale);
        num("disarm_hold_s", c.disarm_hold_s);
        integer("pose_fist_shape", c.pose_fist_shape);
        flag("pose_fist_game_flags", c.pose_fist_game_flags);
        flag("pose_fingers_tracked", c.pose_fingers_tracked);
        flag("pose_elbow_strikes", c.pose_elbow_strikes);
        num("pose_elbow_radius_cm", c.pose_elbow_radius_cm);
        num("pose_elbow_min_ass", c.pose_elbow_min_ass);
        integer("pose_elbow_learn_min_hits", c.pose_elbow_learn_min_hits);
        num("pose_elbow_rigidity", c.pose_elbow_rigidity);
        num("pose_elbow_cut", c.pose_elbow_cut);
        num("pose_elbow_impulse_per_v", c.pose_elbow_impulse_per_v);
        num("pose_elbow_velocity_scale", c.pose_elbow_velocity_scale);
        num("pose_fingers_tau_s", c.pose_fingers_tau_s);
        flag("pose_finger_schedule", c.pose_finger_schedule);
        flag("pose_fingers_per_joint", c.pose_fingers_per_joint);
        num("pose_finger_dip_coupling", c.pose_finger_dip_coupling);
        num("pose_finger_scale_mcp", c.pose_finger_scale_mcp);
        num("pose_finger_scale_pip", c.pose_finger_scale_pip);
        num("pose_finger_scale_dip", c.pose_finger_scale_dip);
        flag("pose_finger_pivot_fix", c.pose_finger_pivot_fix);
        flag("pose_hand_world_stop", c.pose_hand_world_stop);
        num("pose_hand_world_radius_cm", c.pose_hand_world_radius_cm);
        num("pose_hand_world_reach_cm", c.pose_hand_world_reach_cm);
        flag("pose_arm_fresh_shoulder", c.pose_arm_fresh_shoulder);
        flag("pose_arm_fresh_shoulder_held", c.pose_arm_fresh_shoulder_held);
        flag("pose_no_fall_arms", c.pose_no_fall_arms);
        flag("pose_body_wall_stop", c.pose_body_wall_stop);
        num("pose_body_wall_step_cm", c.pose_body_wall_step_cm);
        num("pose_body_wall_radius_share", c.pose_body_wall_radius_share);
        num("pose_finger_mcp_lag", c.pose_finger_mcp_lag);
        num("pose_finger_pip_full", c.pose_finger_pip_full);
        flag("pose_wrap", c.pose_wrap);
        num("pose_wrap_ease_s", c.pose_wrap_ease_s);
        num("pose_wrap_refresh_s", c.pose_wrap_refresh_s);
        num("pose_wrap_finger_radius_cm", c.pose_wrap_finger_radius_cm);
        flag("pose_wrap_body_pad", c.pose_wrap_body_pad);
        flag("pose_wrap_all_bodies", c.pose_wrap_all_bodies);
        num("pose_wrap_untouched_curl", c.pose_wrap_untouched_curl);
        num("pose_wrap_gauntlet_pad_cm", c.pose_wrap_gauntlet_pad_cm);
        num("pose_wrap_halfgauntlet_pad_cm", c.pose_wrap_halfgauntlet_pad_cm);
        flag("pose_wrap_open_base", c.pose_wrap_open_base);
        flag("pose_grab_seat_palm", c.pose_grab_seat_palm);
        num("pose_grab_palm_cm", c.pose_grab_palm_cm);
        num("pose_grab_scenery_gap_cm", c.pose_grab_scenery_gap_cm);
        num("pose_grab_palm_along", c.pose_grab_palm_along);
        flag("pose_grab_drive", c.pose_grab_drive);
        num("pose_grab_drive_heft", c.pose_grab_drive_heft);
        num("pose_grab_drive_accel", c.pose_grab_drive_accel);
        num("pose_grab_drive_ang_accel", c.pose_grab_drive_ang_accel);
        num("pose_grab_body_pull_n", c.pose_grab_body_pull_n);
        num("pose_grab_body_pull_hz", c.pose_grab_body_pull_hz);
        num("pose_grab_body_pull_slack_cm", c.pose_grab_body_pull_slack_cm);
        num("pose_grab_body_pull_min_kg", c.pose_grab_body_pull_min_kg);
        flag("pose_grab_ride", c.pose_grab_ride);
        flag("pose_grab_ride_objects", c.pose_grab_ride_objects);
        num("pose_grab_ride_from_cm", c.pose_grab_ride_from_cm);
        num("pose_grab_ride_full_cm", c.pose_grab_ride_full_cm);
        num("pose_grab_ride_max_cm", c.pose_grab_ride_max_cm);
        num("pose_grab_slip_s", c.pose_grab_slip_s);
        flag("log_wrap", c.log_wrap);
        flag("log_fingers", c.log_fingers);
        flag("log_body_grab", c.log_body_grab);
        flag("log_joint_strain", c.log_joint_strain);
        flag("joint_break", c.joint_break);
        num("joint_break_twist_free_neck_deg", c.joint_break_twist_free_neck_deg);
        num("joint_break_twist_free_arm_deg", c.joint_break_twist_free_arm_deg);
        num("joint_break_twist_free_back_deg", c.joint_break_twist_free_back_deg);
        num("joint_break_twist_gain", c.joint_break_twist_gain);
        num("joint_break_free_min", c.joint_break_free_min);
        flag("joint_break_legs", c.joint_break_legs);
        num("joint_break_scale", c.joint_break_scale);
        flag("log_body_hits", c.log_body_hits);
        integer("pose_grab_body_hold", c.pose_grab_body_hold);
        num("pose_grab_body_spring", c.pose_grab_body_spring);
        num("pose_grab_body_damper", c.pose_grab_body_damper);
        num("pose_grab_body_max", c.pose_grab_body_max);
        num("pose_grab_body_cap_per_t", c.pose_grab_body_cap_per_t);
        flag("pose_grab_body_no_slip", c.pose_grab_body_no_slip);
        num("pose_grab_body_slip_below_t", c.pose_grab_body_slip_below_t);
        num("pose_grab_body_let_go_cm", c.pose_grab_body_let_go_cm);
        flag("input_hand_tracking", c.input_hand_tracking);
        flag("input_frame_layout", c.input_frame_layout);
        flag("pose_eat", c.pose_eat);
        num("pose_eat_cm", c.pose_eat_cm);
        num("pose_drink_tip", c.pose_drink_tip);
        num("pose_drink_sip_s", c.pose_drink_sip_s);
        flag("pose_grab_release_drops", c.pose_grab_release_drops);
        flag("pose_grab_objects", c.pose_grab_objects);
        flag("pose_grab_world", c.pose_grab_world);
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
        num("pose_crouch_max_cm", c.pose_crouch_max_cm);
        flag("pose_crouch_bend_for_depth", c.pose_crouch_bend_for_depth);
        num("pose_crouch_button_cm", c.pose_crouch_button_cm);
        num("pose_crouch_button_cms", c.pose_crouch_button_cms);
        flag("kick_on_a", c.kick_on_a);
        num("kick_tap_s", c.kick_tap_s);
        flag("jump_on_x", c.jump_on_x);
        flag("slowmo_on_right_stick_click", c.slowmo_on_right_stick_click);
        flag("pose_crouch_keep_leg_ik", c.pose_crouch_keep_leg_ik);
        flag("pose_player_no_kneel", c.pose_player_no_kneel);
        flag("pose_capsule_floor_clamp", c.pose_capsule_floor_clamp);
        flag("pose_capsule_keep_on_floor", c.pose_capsule_keep_on_floor);
        flag("pose_capsule_no_step_offset", c.pose_capsule_no_step_offset);
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
        flag("pose_feet_free_on_kick", c.pose_feet_free_on_kick);
        num("pose_feet_kick_free_s", c.pose_feet_kick_free_s);
        num("pose_step_distance_cm", c.pose_step_distance_cm);
        num("pose_step_angle_deg", c.pose_step_angle_deg);
        num("pose_step_time_s", c.pose_step_time_s);
        num("pose_step_lift_cm", c.pose_step_lift_cm);
        num("pose_step_lead_s", c.pose_step_lead_s);
        flag("dev_pose_fake_tracking", c.dev_pose_fake_tracking);
        flag("dev_grip_trace", c.dev_grip_trace);
        flag("stab_log", c.stab_log);
        num("hit_buzz", c.hit_buzz);
        num("hit_buzz_scrape", c.hit_buzz_scrape);
        flag("hit_buzz_blade_on_plate", c.hit_buzz_blade_on_plate);
        flag("hit_buzz_ignore_self", c.hit_buzz_ignore_self);
        num("hit_buzz_impulse_full", c.hit_buzz_impulse_full);
        flag("hit_buzz_log", c.hit_buzz_log);
        num("perf_log_seconds", c.perf_log_seconds);
        num("strength_log_seconds", c.strength_log_seconds);
        flag("perf_events", c.perf_events);
        flag("perf_sample", c.perf_sample);
        flag("perf_bp_segments", c.perf_bp_segments);
        num("hit_buzz_min_intensity", c.hit_buzz_min_intensity);
        num("hit_buzz_limb_min_ms", c.hit_buzz_limb_min_ms);
        num("whoosh_vr_threshold_scale", c.whoosh_vr_threshold_scale);
        num("whoosh_walk_speed_scale", c.whoosh_walk_speed_scale);
        flag("whoosh_log", c.whoosh_log);
        num("whoosh_log_min_speed", c.whoosh_log_min_speed);
        flag("stab_assist", c.stab_assist);
        num("stab_min_speed_cms", c.stab_min_speed_cms);
        num("stab_min_into_cos", c.stab_min_into_cos);
        num("stab_arm_mass_kg", c.stab_arm_mass_kg);
        num("stab_push_force_n", c.stab_push_force_n);
        num("stab_push_time_s", c.stab_push_time_s);
        flag("hide_own_head", c.hide_own_head);
        integer("helmet_view", c.helmet_view);
        num("helmet_window_cm", c.helmet_window_cm);
        num("helmet_see_through", c.helmet_see_through);
        integer("helmet_see_through_mode", c.helmet_see_through_mode);
        text("helmet_see_through_param", c.helmet_see_through_param);
        integer("helmet_see_through_test", c.helmet_see_through_test);
        num("helmet_fade_see_through", c.helmet_fade_see_through);
        text("helmet_fade_always", c.helmet_fade_always);
        text("helmet_fade_never", c.helmet_fade_never);
        text("helmet_fade_tint", c.helmet_fade_tint);
        num("helmet_fade_metallic", c.helmet_fade_metallic);
        num("helmet_fade_roughness", c.helmet_fade_roughness);
        text("helmet_fade_material", c.helmet_fade_material);
        text("hide_own_gore_from", c.hide_own_gore_from);
        flag("player_takes_damage", c.player_takes_damage);
        flag("player_body_contact_speed", c.player_body_contact_speed);
        num("player_body_contact_mass_scale", c.player_body_contact_mass_scale);
        flag("player_body_contact_log", c.player_body_contact_log);
        flag("player_item_contact_speed", c.player_item_contact_speed);
        flag("player_stump_blood", c.player_stump_blood);
        flag("injury_limbs", c.injury_limbs);
        num("injury_limb_health", c.injury_limb_health);
        num("injury_one_leg_speed", c.injury_one_leg_speed);
        flag("injury_two_legs_fall", c.injury_two_legs_fall);
        flag("injury_back_broken_falls", c.injury_back_broken_falls);
        num("injury_dislocated_follow_s", c.injury_dislocated_follow_s);
        num("injury_dislocated_sag_cm", c.injury_dislocated_sag_cm);
        flag("injury_swing_relocate", c.injury_swing_relocate);
        flag("death_ragdoll", c.death_ragdoll);
        if (c.render_scale < 0.2) c.render_scale = 0.2;
        if (c.render_scale > 2.0) c.render_scale = 2.0;
        if (c.world_scale < 0.1) c.world_scale = 0.1;
        if (c.near_clip_cm < 0.5) c.near_clip_cm = 0.5;
        {
            std::lock_guard<std::mutex> lock(g_cfg_mutex);
            g_cfg = c;
        }
        if (!missing.empty()) add_missing_settings(missing);
        HSVR_LOG("Settings loaded: render_scale=%.2f world_scale=%.2f anchor=%s head_bone=%s eye_fwd=%.1f eye_up=%.1f smoothing=%.3f "
                 "near=%.1f gamma=%d ui_panel=%d separate_rt=%d",
                 c.render_scale, c.world_scale, c.anchor.c_str(), c.head_bone.c_str(), c.eye_forward_cm, c.eye_up_cm,
                 c.anchor_smoothing, c.near_clip_cm, c.gamma_mode, c.ui_panel, c.separate_render_target);
        return true;
    }
} // namespace hsvr
