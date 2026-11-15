#include "config.hpp"

#include "log.hpp"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>

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
                "eye_forward_cm=%.1f\n"
                "eye_up_cm=%.1f\n"
                "anchor_smoothing=%.3f\n"
                "hide_head=%d\n"
                "near_clip_cm=%.1f\n"
                "gamma_mode=%d\n"
                "ui_panel=%d\n"
                "ui_distance_m=%.2f\n"
                "ui_width_m=%.2f\n"
                "ui_height_offset_m=%.2f\n"
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
                "; 1 = full headset resolution with menus on a floating panel, 0 = fallback through the game window.\n"
                "separate_render_target=%d\n"
                "; 1 = keep the game at normal speed while the headset is asleep.\n"
                "idle_helper=%d\n"
                "log_poses=%d\n"
                "stereo_slot_offset=%d\n"
                "dev_fake_headset=%d\n",
                d.enabled, d.require_steamvr_running, d.render_scale, d.world_scale, d.anchor.c_str(), d.head_bone.c_str(),
                d.eye_forward_cm, d.eye_up_cm, d.anchor_smoothing, d.hide_head, d.near_clip_cm, d.gamma_mode, d.ui_panel,
                d.ui_distance_m, d.ui_width_m, d.ui_height_offset_m, d.mirror_window, (unsigned)d.recenter_key, (unsigned)d.toggle_ui_key, (unsigned)d.toggle_view_key, (unsigned)d.toggle_mode_key,
                d.predict_frames, d.separate_render_target, d.idle_helper, d.log_poses, d.stereo_slot_offset, d.dev_fake_headset);
        fclose(f);
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
        char line[512];
        while (fgets(line, sizeof(line), f))
        {
            std::string s = trim(line);
            if (s.empty() || s[0] == ';' || s[0] == '#' || s[0] == '[') continue;
            size_t eq = s.find('=');
            if (eq == std::string::npos) continue;
            kv[trim(s.substr(0, eq))] = trim(s.substr(eq + 1));
        }
        fclose(f);

        Config c;
        auto num = [&](const char* key, double& out) {
            auto it = kv.find(key);
            if (it != kv.end() && !it->second.empty()) out = strtod(it->second.c_str(), nullptr);
        };
        auto integer = [&](const char* key, int& out) {
            auto it = kv.find(key);
            if (it != kv.end() && !it->second.empty()) out = (int)strtol(it->second.c_str(), nullptr, 0);
        };
        auto flag = [&](const char* key, bool& out) {
            int v = out ? 1 : 0;
            integer(key, v);
            out = v != 0;
        };
        auto text = [&](const char* key, std::string& out) {
            auto it = kv.find(key);
            if (it != kv.end() && !it->second.empty()) out = it->second;
        };
        flag("enabled", c.enabled);
        flag("require_steamvr_running", c.require_steamvr_running);
        num("render_scale", c.render_scale);
        num("world_scale", c.world_scale);
        text("anchor", c.anchor);
        text("head_bone", c.head_bone);
        num("eye_forward_cm", c.eye_forward_cm);
        num("eye_up_cm", c.eye_up_cm);
        num("anchor_smoothing", c.anchor_smoothing);
        flag("hide_head", c.hide_head);
        num("near_clip_cm", c.near_clip_cm);
        integer("gamma_mode", c.gamma_mode);
        flag("ui_panel", c.ui_panel);
        num("ui_distance_m", c.ui_distance_m);
        num("ui_width_m", c.ui_width_m);
        num("ui_height_offset_m", c.ui_height_offset_m);
        flag("mirror_window", c.mirror_window);
        integer("recenter_key", c.recenter_key);
        integer("toggle_ui_key", c.toggle_ui_key);
        integer("toggle_view_key", c.toggle_view_key);
        integer("toggle_mode_key", c.toggle_mode_key);
        num("predict_frames", c.predict_frames);
        flag("separate_render_target", c.separate_render_target);
        flag("idle_helper", c.idle_helper);
        flag("log_poses", c.log_poses);
        integer("stereo_slot_offset", c.stereo_slot_offset);
        flag("dev_fake_headset", c.dev_fake_headset);
        if (c.render_scale < 0.2) c.render_scale = 0.2;
        if (c.render_scale > 2.0) c.render_scale = 2.0;
        if (c.world_scale < 0.1) c.world_scale = 0.1;
        if (c.near_clip_cm < 0.5) c.near_clip_cm = 0.5;
        {
            std::lock_guard<std::mutex> lock(g_cfg_mutex);
            g_cfg = c;
        }
        HSVR_LOG("Settings loaded: render_scale=%.2f world_scale=%.2f anchor=%s head_bone=%s eye_fwd=%.1f eye_up=%.1f smoothing=%.3f "
                 "hide_head=%d near=%.1f gamma=%d ui_panel=%d separate_rt=%d",
                 c.render_scale, c.world_scale, c.anchor.c_str(), c.head_bone.c_str(), c.eye_forward_cm, c.eye_up_cm,
                 c.anchor_smoothing, c.hide_head, c.near_clip_cm, c.gamma_mode, c.ui_panel, c.separate_render_target);
        return true;
    }
} // namespace hsvr
