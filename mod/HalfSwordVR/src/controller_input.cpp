// Controller sticks and buttons drive the game through a virtual gamepad. See controller_input.hpp.

#include "controller_input.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FProperty.hpp>
#include <Unreal/Property/FBoolProperty.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "arm_control.hpp"
#include "bs_hands.hpp"
#include "config.hpp"
#include "log.hpp"
#include "vr_present.hpp"
#include "vr_state.hpp"

#include <windows.h>
#include <Xinput.h>

using namespace RC;
using namespace RC::Unreal;

namespace hsvr
{
    namespace
    {
        // ---- the pad the game is told about ---------------------------------------------------
        struct PadOut
        {
            bool active = false; // answer pad 0 with this
            float lx = 0, ly = 0, rx = 0, ry = 0;
            float lt = 0, rt = 0;
            uint16_t buttons = 0;
        };

        using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
        GetStateFn g_real_get_state = nullptr;
        std::mutex g_pad_mutex;
        PadOut g_pad;
        DWORD g_packet = 1;
        XINPUT_GAMEPAD g_last_sent{};
        // Polls of pad 0 by the game (any thread), for the log and to notice a game that stopped asking.
        volatile LONG g_polls = 0;
        // The real pad 0's buttons (and triggers in the high bytes) as last read, for the log.
        volatile LONG g_real_buttons = 0;
        bool g_installed = false;

        SHORT to_short(float v)
        {
            v = std::clamp(v, -1.0f, 1.0f);
            return (SHORT)std::lround(v >= 0 ? v * 32767.0f : v * 32768.0f);
        }

        float from_short(SHORT v) { return v >= 0 ? v / 32767.0f : v / 32768.0f; }

        DWORD WINAPI hooked_get_state(DWORD index, XINPUT_STATE* state)
        {
            DWORD result = g_real_get_state ? g_real_get_state(index, state) : ERROR_DEVICE_NOT_CONNECTED;
            if (index != 0 || !state) return result;
            InterlockedIncrement(&g_polls);
            std::lock_guard<std::mutex> lock(g_pad_mutex);
            if (!g_pad.active) return result;
            XINPUT_GAMEPAD real{};
            if (result == ERROR_SUCCESS) real = state->Gamepad;
            g_real_buttons = (LONG)real.wButtons | ((LONG)real.bLeftTrigger << 16) | ((LONG)real.bRightTrigger << 24);
            XINPUT_GAMEPAD out{};
            // A real pad's sticks win where they are pushed further than the controllers'.
            auto pick = [](SHORT real_value, float ours) {
                float r = from_short(real_value);
                return std::fabs(r) > std::fabs(ours) ? real_value : to_short(ours);
            };
            out.sThumbLX = pick(real.sThumbLX, g_pad.lx);
            out.sThumbLY = pick(real.sThumbLY, g_pad.ly);
            out.sThumbRX = pick(real.sThumbRX, g_pad.rx);
            out.sThumbRY = pick(real.sThumbRY, g_pad.ry);
            out.bLeftTrigger = (BYTE)std::max<int>(real.bLeftTrigger, (int)std::lround(std::clamp(g_pad.lt, 0.0f, 1.0f) * 255.0f));
            out.bRightTrigger = (BYTE)std::max<int>(real.bRightTrigger, (int)std::lround(std::clamp(g_pad.rt, 0.0f, 1.0f) * 255.0f));
            out.wButtons = (WORD)(real.wButtons | g_pad.buttons);
            if (std::memcmp(&out, &g_last_sent, sizeof(out)) != 0)
            {
                g_last_sent = out;
                ++g_packet;
            }
            state->dwPacketNumber = g_packet;
            state->Gamepad = out;
            return ERROR_SUCCESS;
        }

        // Finds the import slot the game calls XInputGetState through: first in the delay-load
        // imports (this build: XINPUT1_4.dll by ordinal 2), then in the ordinary imports.
        void** find_get_state_slot(const char** how)
        {
            auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
            auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
            auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
            auto is_xinput = [](const char* name) { return _strnicmp(name, "xinput", 6) == 0; };
            auto matches = [&](const IMAGE_THUNK_DATA64& entry) {
                if (IMAGE_SNAP_BY_ORDINAL64(entry.u1.Ordinal)) return IMAGE_ORDINAL64(entry.u1.Ordinal) == 2;
                auto* by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + entry.u1.AddressOfData);
                return std::strcmp(reinterpret_cast<const char*>(by_name->Name), "XInputGetState") == 0;
            };
            const IMAGE_DATA_DIRECTORY& delay = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
            if (delay.VirtualAddress)
            {
                for (auto* d = reinterpret_cast<IMAGE_DELAYLOAD_DESCRIPTOR*>(base + delay.VirtualAddress); d->DllNameRVA; ++d)
                {
                    if (!d->Attributes.RvaBased) continue;
                    if (!is_xinput(reinterpret_cast<const char*>(base + d->DllNameRVA))) continue;
                    auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + d->ImportNameTableRVA);
                    auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + d->ImportAddressTableRVA);
                    for (int i = 0; names[i].u1.AddressOfData; ++i)
                    {
                        if (!matches(names[i])) continue;
                        *how = "delay-load import";
                        return reinterpret_cast<void**>(&slots[i].u1.Function);
                    }
                }
            }
            const IMAGE_DATA_DIRECTORY& normal = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
            if (normal.VirtualAddress)
            {
                for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + normal.VirtualAddress); d->Name; ++d)
                {
                    if (!is_xinput(reinterpret_cast<const char*>(base + d->Name))) continue;
                    auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
                    auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + d->FirstThunk);
                    for (int i = 0; names[i].u1.AddressOfData; ++i)
                    {
                        if (!matches(names[i])) continue;
                        *how = "import";
                        return reinterpret_cast<void**>(&slots[i].u1.Function);
                    }
                }
            }
            return nullptr;
        }

        // ---- the mapping from controllers to the pad -------------------------------------------
        enum Source
        {
            kLStickX, kLStickY, kRStickX, kRStickY,
            kLTrigger, kRTrigger, kLGrip, kRGrip,
            kLLower, kLUpper, kRLower, kRUpper, kLClick, kRClick, kLMenu,
            kSourceCount
        };
        enum TargetKind
        {
            kAxis,    // a stick component
            kTrigger, // an analog trigger
            kButton,
        };
        struct Mapping
        {
            Source source;
            TargetKind kind;
            int axis = 0;         // 0 LX, 1 LY, 2 RX, 3 RY / 0 LT, 1 RT
            uint16_t button = 0;
            bool down = false;    // a button's latch (with hysteresis for analog sources)
        };

        bool parse_source(const std::string& name, std::vector<Source>& out)
        {
            static const struct
            {
                const char* name;
                Source a, b;
                bool pair;
            } table[] = {
                    {"l_stick", kLStickX, kLStickY, true}, {"r_stick", kRStickX, kRStickY, true},
                    {"l_stick_x", kLStickX, kLStickX, false}, {"l_stick_y", kLStickY, kLStickY, false},
                    {"r_stick_x", kRStickX, kRStickX, false}, {"r_stick_y", kRStickY, kRStickY, false},
                    {"l_trigger", kLTrigger, kLTrigger, false}, {"r_trigger", kRTrigger, kRTrigger, false},
                    {"l_grip", kLGrip, kLGrip, false}, {"r_grip", kRGrip, kRGrip, false},
                    {"l_lower", kLLower, kLLower, false}, {"l_upper", kLUpper, kLUpper, false},
                    {"r_lower", kRLower, kRLower, false}, {"r_upper", kRUpper, kRUpper, false},
                    {"l_click", kLClick, kLClick, false}, {"r_click", kRClick, kRClick, false},
                    {"l_menu", kLMenu, kLMenu, false},
            };
            for (const auto& entry : table)
            {
                if (name != entry.name) continue;
                out.push_back(entry.a);
                if (entry.pair) out.push_back(entry.b);
                return true;
            }
            return false;
        }

        // Fills `out` with one mapping per source; a stick pair maps onto both components.
        bool parse_target(const std::string& name, const std::vector<Source>& sources, std::vector<Mapping>& out)
        {
            static const struct
            {
                const char* name;
                uint16_t button;
            } buttons[] = {
                    {"A", XINPUT_GAMEPAD_A}, {"B", XINPUT_GAMEPAD_B}, {"X", XINPUT_GAMEPAD_X}, {"Y", XINPUT_GAMEPAD_Y},
                    {"LB", XINPUT_GAMEPAD_LEFT_SHOULDER}, {"RB", XINPUT_GAMEPAD_RIGHT_SHOULDER},
                    {"LS", XINPUT_GAMEPAD_LEFT_THUMB}, {"RS", XINPUT_GAMEPAD_RIGHT_THUMB},
                    {"START", XINPUT_GAMEPAD_START}, {"BACK", XINPUT_GAMEPAD_BACK},
                    {"DUP", XINPUT_GAMEPAD_DPAD_UP}, {"DDOWN", XINPUT_GAMEPAD_DPAD_DOWN},
                    {"DLEFT", XINPUT_GAMEPAD_DPAD_LEFT}, {"DRIGHT", XINPUT_GAMEPAD_DPAD_RIGHT},
            };
            if (sources.size() == 2)
            {
                int first = name == "LSTICK" ? 0 : name == "RSTICK" ? 2 : -1;
                if (first < 0) return false;
                out.push_back({sources[0], kAxis, first});
                out.push_back({sources[1], kAxis, first + 1});
                return true;
            }
            Source source = sources[0];
            if (name == "LSTICK_X") out.push_back({source, kAxis, 0});
            else if (name == "LSTICK_Y") out.push_back({source, kAxis, 1});
            else if (name == "RSTICK_X") out.push_back({source, kAxis, 2});
            else if (name == "RSTICK_Y") out.push_back({source, kAxis, 3});
            else if (name == "LT") out.push_back({source, kTrigger, 0});
            else if (name == "RT") out.push_back({source, kTrigger, 1});
            else
            {
                for (const auto& entry : buttons)
                {
                    if (name != entry.name) continue;
                    Mapping m{source, kButton};
                    m.button = entry.button;
                    out.push_back(m);
                    return true;
                }
                return false;
            }
            return true;
        }

        std::vector<Mapping> parse_map(const std::string& text)
        {
            std::vector<Mapping> result;
            std::string token;
            auto flush = [&]() {
                if (token.empty()) return;
                size_t eq = token.find('=');
                std::vector<Source> sources;
                bool ok = eq != std::string::npos && parse_source(token.substr(0, eq), sources);
                if (ok)
                {
                    std::string target = token.substr(eq + 1);
                    for (char& c : target) c = (char)toupper((unsigned char)c);
                    ok = parse_target(target, sources, result);
                }
                if (!ok) HSVR_LOG("Pad: pad_map entry \"%s\" not understood; skipped", token.c_str());
                token.clear();
            };
            for (char c : text)
            {
                if (c == ' ' || c == ',' || c == ';' || c == '\t') flush();
                else token += c;
            }
            flush();
            return result;
        }

        // Stick values from the controller, with our own small dead zone, stretched past the game's
        // gamepad dead zone so that a light push still moves a little.
        float shape_stick(float v, const Config& cfg)
        {
            float a = std::fabs(v);
            float own = (float)std::clamp(cfg.pad_deadzone, 0.0, 0.9);
            float game = (float)std::clamp(cfg.pad_game_deadzone, 0.0, 0.9);
            if (a <= own) return 0.0f;
            float t = std::min(1.0f, (a - own) / (1.0f - own));
            return std::copysign(game + (1.0f - game) * t, v);
        }

        struct TickState
        {
            std::string parsed_text;
            std::vector<Mapping> map;
            bool was_active = false;
            int refresh_step = 0; // asking the engine to look for pads again: 1 = turned on, 2 = turn off
            double last_poll_check = 0.0;
            LONG polls_at_check = 0;
            PadOut last_logged;
        };
        TickState g_tick;

        // ---- guard kept up for the player --------------------------------------------------------
        // On mouse and keyboard the mouse buttons flip "R Hand Toggle" / "L Hand Toggle" on the
        // character (the hand is raised to fight; the game then refreshes the arm muscles,
        // "Force Refresh Pa Strength"). With motion arms the player raises the hand themselves, so
        // the mod keeps the toggle on for a hand that holds a weapon (auto_guard), and the triggers
        // are free.
        bool read_bool(UObject* owner, const TCHAR* name, bool& value)
        {
            FProperty* property = owner->GetPropertyByNameInChain(name);
            if (!property) return false;
            auto* flag = static_cast<FBoolProperty*>(property);
            const uint8_t* byte = reinterpret_cast<const uint8_t*>(owner) + property->GetOffset_Internal() + flag->GetByteOffset();
            value = (*byte & flag->GetFieldMask()) != 0;
            return true;
        }

        bool write_bool(UObject* owner, const TCHAR* name, bool value)
        {
            FProperty* property = owner->GetPropertyByNameInChain(name);
            if (!property) return false;
            auto* flag = static_cast<FBoolProperty*>(property);
            uint8_t* byte = reinterpret_cast<uint8_t*>(owner) + property->GetOffset_Internal() + flag->GetByteOffset();
            if (value) *byte |= flag->GetFieldMask();
            else *byte &= (uint8_t)~flag->GetFieldMask();
            return true;
        }

        void keep_guard(UObject* pawn, bool in_menu, const Config& cfg)
        {
            static UObject* warned_pawn = nullptr;
            if (!pawn || in_menu || !cfg.auto_guard) return;
            const struct
            {
                const TCHAR* weapon;
                const TCHAR* toggle;
                const char* label;
            } hands[] = {{STR("Weapon R"), STR("R Hand Toggle"), "right"}, {STR("Weapon L"), STR("L Hand Toggle"), "left"}};
            bool changed = false;
            for (const auto& hand : hands)
            {
                UObject** weapon = pawn->GetValuePtrByPropertyNameInChain<UObject*>(hand.weapon);
                bool up = false;
                if (!weapon || !read_bool(pawn, hand.toggle, up))
                {
                    if (warned_pawn != pawn) HSVR_LOG("Guard: %ls has no %ls / %ls; guard is left to the game", pawn->GetFullName().c_str(), hand.weapon, hand.toggle);
                    warned_pawn = pawn;
                    return;
                }
                if (!*weapon || up) continue;
                write_bool(pawn, hand.toggle, true);
                changed = true;
                HSVR_LOG("Guard: %s hand holds a weapon; guard raised for it", hand.label);
            }
            if (changed) write_bool(pawn, STR("Force Refresh Pa Strength"), true);
        }

        // ---- groundwork for physical swapping, holsters and kicks --------------------------------
        // Writes the parameters of the character's weapon-handling events once per character class,
        // so they can later be called directly (not through gamepad buttons).
        void log_weapon_functions(UObject* pawn)
        {
            static std::vector<UObject*> done_classes;
            if (!pawn || !config().log_bindings) return;
            UObject* cls = reinterpret_cast<UObject*>(pawn->GetClassPrivate());
            if (std::find(done_classes.begin(), done_classes.end(), cls) != done_classes.end()) return;
            done_classes.push_back(cls);
            static const TCHAR* names[] = {
                    STR("Kick Event"), STR("Swap Weapons"), STR("Draw Weapon"), STR("Sheath R"), STR("Sheath L"), STR("Unsheath R"), STR("Unsheath L"),
                    STR("Unsheath R Event"), STR("Unsheath L Event"), STR("R Hand Sheath Start"), STR("L Hand Sheath Start"),
                    STR("Grab Specific Object R H"), STR("Grab Specific Object L H"), STR("Release Grab R"), STR("Release Grab L"),
                    STR("Pick Up Right Event"), STR("Pick Up Left Event"), STR("Find Grab Location Right Event"), STR("Find Grab Location Left Event"),
                    STR("Check if R Holding Weapon"), STR("Check if L Holding Weapon"), STR("Event Add Sheathed Weapon"),
                    STR("Assign R Weapon Slot"), STR("Assign L Weapon Slot"), STR("Check Available Weapon R in All Slots"),
            };
            HSVR_LOG("Functions: weapon handling on %ls", pawn->GetFullName().c_str());
            for (const TCHAR* name : names)
            {
                UFunction* fn = pawn->GetFunctionByNameInChain(name);
                if (!fn)
                {
                    HSVR_LOG("Functions:   %ls: not found", name);
                    continue;
                }
                std::wstring params;
                for (FProperty* param : fn->ForEachProperty())
                {
                    params += param->GetName();
                    params += L" (";
                    params += param->GetClass().GetName();
                    params += L", @";
                    params += std::to_wstring(param->GetOffset_Internal());
                    params += L", ";
                    params += std::to_wstring(param->GetElementSize());
                    params += L" bytes) ";
                }
                HSVR_LOG("Functions:   %ls: %d bytes of parameters: %ls", name, (int)fn->GetParmsSize(), params.empty() ? L"none" : params.c_str());
            }
            // Where the character carries its weapons (slots and sheaths), for the holsters.
            for (FProperty* property : static_cast<UStruct*>(pawn->GetClassPrivate())->ForEachProperty())
            {
                std::wstring n = property->GetName();
                std::wstring lower = n;
                for (wchar_t& c : lower) c = (wchar_t)towlower(c);
                if (lower.find(L"sheath") == std::wstring::npos && lower.find(L"slot") == std::wstring::npos && lower.find(L"scabbard") == std::wstring::npos &&
                    lower.find(L"holster") == std::wstring::npos && lower.find(L"belt") == std::wstring::npos)
                    continue;
                HSVR_LOG("Functions:   property %ls (%ls)", n.c_str(), property->GetClass().GetName().c_str());
            }
        }

        // ---- the mod's own keys on the controllers ----------------------------------------------
        // Y: press = recenter (Home); hold 1 s = show or hide the menu panel (End).
        // X: hold 1 s = motion arms on or off (Page Up).
        // A press acts on release, so that a hold does not also do the short action.
        struct HoldButton
        {
            bool down = false;
            bool long_fired = false;
            double since = 0.0;
            // Returns 1 for a short press (on release), 2 once when held for `hold_s`, else 0.
            int update(bool now_down, double now, double hold_s)
            {
                int result = 0;
                if (now_down && !down)
                {
                    since = now;
                    long_fired = false;
                }
                if (now_down && !long_fired && now - since >= hold_s)
                {
                    long_fired = true;
                    result = 2;
                }
                if (!now_down && down && !long_fired) result = 1;
                down = now_down;
                return result;
            }
        };
        HoldButton g_y_button, g_x_button;
        bool g_x_tapped = false; // X pressed and let go within the hold time: slow motion (stick_layout)

        void mod_keys_on_controllers(const XrHandRaw hands[2], const Config& cfg)
        {
            if (!cfg.controller_mod_keys) return;
            double now = now_seconds();
            auto& sh = shared();
            int y = g_y_button.update(hands[0].button_upper, now, 1.0);
            int x = g_x_button.update(hands[0].button_lower, now, 1.0);
            if (y == 1)
            {
                sh.recenter_requested.store(true);
                HSVR_LOG("Controller: recenter (Y)");
            }
            else if (y == 2)
            {
                bool toggled = !sh.ui_toggled.load();
                sh.ui_toggled.store(toggled);
                HSVR_LOG("Controller: menu panel %s (Y held)", toggled == cfg.ui_panel_auto ? "always shown" : "hidden during play");
            }
            if (x == 2) arm_toggle();
        }

        // ---- pointing at the menu panel --------------------------------------------------------
        // While a menu is up, the right controller (or the left, if the right is not tracked)
        // points at the panel; the spot is drawn on the panel, the game window's mouse pointer is
        // moved to the same place, and the trigger clicks there. Only the game's own window gets
        // the click, and only while it is the window in front.
        struct V3
        {
            float x, y, z;
        };
        V3 rotate(float qx, float qy, float qz, float qw, V3 v)
        {
            // v + 2w(q x v) + 2 q x (q x v)
            V3 t{2 * (qy * v.z - qz * v.y), 2 * (qz * v.x - qx * v.z), 2 * (qx * v.y - qy * v.x)};
            return {v.x + qw * t.x + (qy * t.z - qz * t.y), v.y + qw * t.y + (qz * t.x - qx * t.z), v.z + qw * t.z + (qx * t.y - qy * t.x)};
        }

        struct WindowFind
        {
            HWND best = nullptr;
            LONG best_area = 0;
        };
        BOOL CALLBACK find_game_window(HWND hwnd, LPARAM param)
        {
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd)) return TRUE;
            RECT r{};
            GetClientRect(hwnd, &r);
            LONG area = (r.right - r.left) * (r.bottom - r.top);
            auto* find = reinterpret_cast<WindowFind*>(param);
            if (area > find->best_area)
            {
                find->best_area = area;
                find->best = hwnd;
            }
            return TRUE;
        }

        struct PointerState
        {
            HWND window = nullptr;
            double window_checked = -10.0;
            bool clicking = false;
            int last_x = -1, last_y = -1;
            bool was_pointing = false;
        };
        PointerState g_pointer;

        // Returns true while the controller is pointing at the panel during a menu.
        bool point_at_panel(const XrHandRaw hands[2], const Config& cfg, bool in_menu)
        {
            PointerState& ps = g_pointer;
            auto& sh = shared();
            bool menu = in_menu || sh.game_wants_pointer.load() || sh.game_paused.load();
            PanelGeometry panel = vr_panel_geometry();
            double now = now_seconds();
            bool panel_up = panel.shown && now - panel.shown_at < 0.25;
            int hand = hands[1].aim_valid ? 1 : hands[0].aim_valid ? 0 : -1;
            float u = -1, v = -1;
            if (cfg.controller_pointer && menu && panel_up && hand >= 0)
            {
                const XrHandRaw& hr = hands[hand];
                V3 dir = rotate(hr.aqx, hr.aqy, hr.aqz, hr.aqw, {0, 0, -1});
                V3 normal{std::sin(panel.yaw), 0, std::cos(panel.yaw)};
                V3 right{std::cos(panel.yaw), 0, -std::sin(panel.yaw)};
                float facing = dir.x * normal.x + dir.y * normal.y + dir.z * normal.z;
                if (facing < -0.05f)
                {
                    V3 to{panel.cx - hr.ax, panel.cy - hr.ay, panel.cz - hr.az};
                    float t = (to.x * normal.x + to.y * normal.y + to.z * normal.z) / facing;
                    if (t > 0.0f && t < 10.0f)
                    {
                        V3 hit{hr.ax + dir.x * t - panel.cx, hr.ay + dir.y * t - panel.cy, hr.az + dir.z * t - panel.cz};
                        float lx = hit.x * right.x + hit.y * right.y + hit.z * right.z;
                        float ly = hit.y;
                        float cu = lx / panel.width + 0.5f, cv = 0.5f - ly / panel.height;
                        if (cu >= 0.0f && cu <= 1.0f && cv >= 0.0f && cv <= 1.0f)
                        {
                            u = cu;
                            v = cv;
                        }
                    }
                }
            }
            vr_set_panel_pointer(u, v);
            bool pointing = u >= 0.0f;
            if (pointing != ps.was_pointing)
            {
                ps.was_pointing = pointing;
                HSVR_LOG(pointing ? "Controller: pointing at the menu panel (%s hand)" : "Controller: no longer pointing at the menu panel",
                         hand == 1 ? "right" : "left");
            }
            if (!pointing)
            {
                if (ps.clicking && ps.window) PostMessageW(ps.window, WM_LBUTTONUP, 0, MAKELPARAM(ps.last_x, ps.last_y));
                ps.clicking = false;
                return false;
            }
            if (!ps.window || now - ps.window_checked > 2.0 || !IsWindow(ps.window))
            {
                ps.window_checked = now;
                WindowFind find;
                EnumWindows(&find_game_window, reinterpret_cast<LPARAM>(&find));
                ps.window = find.best;
            }
            if (!ps.window || GetForegroundWindow() != ps.window) return true;
            RECT client{};
            GetClientRect(ps.window, &client);
            int x = (int)std::lround(u * (float)(client.right - 1));
            int y = (int)std::lround(v * (float)(client.bottom - 1));
            if (ps.clicking)
            {
                // Hold the spot still during a click: pulling the trigger moves the controller.
                x = ps.last_x;
                y = ps.last_y;
            }
            else if (x != ps.last_x || y != ps.last_y)
            {
                ps.last_x = x;
                ps.last_y = y;
                POINT screen{x, y};
                ClientToScreen(ps.window, &screen);
                SetCursorPos(screen.x, screen.y);
            }
            float trigger = hands[hand].trigger;
            bool down = ps.clicking ? trigger > 0.4f : trigger > 0.6f;
            if (down != ps.clicking)
            {
                ps.clicking = down;
                PostMessageW(ps.window, down ? WM_LBUTTONDOWN : WM_LBUTTONUP, down ? MK_LBUTTON : 0, MAKELPARAM(x, y));
                HSVR_LOG("Controller: %s at (%d, %d) on the menu panel", down ? "click" : "release", x, y);
            }
            return true;
        }

        // ---- body holsters, hand-to-hand swaps and kick -----------------------------------------
        // Calls the character's own events, so the game's sheathing, drawing and swapping run as
        // they do on mouse and keyboard (found in the log: all take no parameters).
        bool call_event(UObject* pawn, const TCHAR* name)
        {
            UFunction* fn = pawn ? pawn->GetFunctionByNameInChain(name) : nullptr;
            if (!fn)
            {
                HSVR_LOG("Gesture: %ls not found on the character", name);
                return false;
            }
            alignas(16) uint8_t params[64] = {};
            if (fn->GetParmsSize() > (int)sizeof(params))
            {
                HSVR_LOG("Gesture: %ls takes %d bytes of parameters; not called", name, (int)fn->GetParmsSize());
                return false;
            }
            pawn->ProcessEvent(fn, params);
            HSVR_LOG("Gesture: called %ls", name);
            return true;
        }

        bool holds_weapon(UObject* pawn, int hand)
        {
            UObject** weapon = pawn->GetValuePtrByPropertyNameInChain<UObject*>(hand ? STR("Weapon R") : STR("Weapon L"));
            return weapon && *weapon;
        }

        // ---- picking things up --------------------------------------------------------------------
        // Everything that can be held (weapons, and food such as bread or blutwine, which are the
        // same kind of object with "Consumable" set) is a ModularWeaponBP_C. The game's own grab
        // reaches with its animated arm towards a spot it searches for; with motion arms the arm is
        // the player's, so that reach never lands. Instead, an empty hand that squeezes near such an
        // object asks the game to grab exactly that object (Grab Specific Object R H / L H, the
        // game's own grab with a given target) and then to pick it up (Pick Up Right / Left Event).
        struct Vec
        {
            double X, Y, Z;
        };
        struct SocketParams
        {
            FName InSocketName;
            Vec ReturnValue;
        };
        struct LocationParams
        {
            Vec ReturnValue;
        };
        struct GrabParams
        {
            UObject* GrabComponent;
            Vec NewLocation;
        };

        bool hand_world(UObject* pawn, int hand, Vec& out)
        {
            UObject** mesh = pawn->GetValuePtrByPropertyNameInChain<UObject*>(STR("Mesh"));
            if (!mesh || !*mesh) return false;
            UFunction* fn = (*mesh)->GetFunctionByNameInChain(STR("GetSocketLocation"));
            if (!fn) return false;
            SocketParams p{};
            p.InSocketName = FName(hand ? STR("hand_r") : STR("hand_l"), FNAME_Add);
            (*mesh)->ProcessEvent(fn, &p);
            out = p.ReturnValue;
            return true;
        }

        bool component_world(UObject* component, Vec& out)
        {
            UFunction* fn = component ? component->GetFunctionByNameInChain(STR("K2_GetComponentLocation")) : nullptr;
            if (!fn) return false;
            LocationParams p{};
            component->ProcessEvent(fn, &p);
            out = p.ReturnValue;
            return true;
        }

        UObject* object_at(UObject* owner, const TCHAR* name)
        {
            UObject** value = owner ? owner->GetValuePtrByPropertyNameInChain<UObject*>(name) : nullptr;
            return value ? *value : nullptr;
        }

        // True when a hand picked something up.
        bool pick_up_near(UObject* pawn, int hand, const Config& cfg)
        {
            Vec at{};
            if (!hand_world(pawn, hand, at)) return false;
            // Things that are already in someone's hands or sheaths are not on offer.
            std::vector<UObject*> taken;
            std::vector<UObject*> fighters;
            UObjectGlobals::FindAllOf(STR("Willie_BP_C"), fighters);
            static const TCHAR* held[] = {STR("Weapon R"), STR("Weapon L"), STR("Weapon Slot R 1"), STR("Weapon Slot R 2"),
                                          STR("Weapon Slot L 1"), STR("Weapon Slot L 2"), STR("Weapon Slot Back")};
            for (UObject* f : fighters)
                for (const TCHAR* n : held)
                    if (UObject* w = object_at(f, n)) taken.push_back(w);
            std::vector<UObject*> items;
            UObjectGlobals::FindAllOf(STR("ModularWeaponBP_C"), items);
            UObject* best = nullptr;
            UObject* best_component = nullptr;
            Vec best_at{};
            double best_d = cfg.pickup_radius_m * 100.0;
            for (UObject* item : items)
            {
                if (std::find(taken.begin(), taken.end(), item) != taken.end()) continue;
                UObject* grip = object_at(item, STR("Grip"));
                UObject* base = object_at(item, STR("BaseMesh"));
                if (!base) continue;
                Vec where{};
                if (!component_world(grip ? grip : base, where)) continue;
                double dx = where.X - at.X, dy = where.Y - at.Y, dz = where.Z - at.Z;
                double d = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (d < best_d)
                {
                    best_d = d;
                    best = item;
                    best_component = base;
                    best_at = where;
                }
            }
            if (!best)
            {
                HSVR_LOG("Gesture: %s hand squeezed; nothing to pick up within %.0f cm (%d things in the level)", hand ? "right" : "left",
                         cfg.pickup_radius_m * 100.0, (int)items.size());
                return false;
            }
            HSVR_LOG("Gesture: %s hand picks up %ls (%.0f cm away)", hand ? "right" : "left", best->GetFullName().c_str(), best_d);
            // The game's own pick-up, finished by the game in its tick (bs_hands.cpp). The old way
            // (Grab Specific Object, then Pick Up Right Event) let go again at once: that event is
            // the grab key's release code (self-test, build 3).
            return bs_pick_up(pawn, hand, best_component, best_at.X, best_at.Y, best_at.Z);
        }

        struct GestureState
        {
            bool grip_down[2]{false, false};
            bool consumed[2]{false, false}; // this grip press was used by a gesture; keep it from the game
            bool kick_down = false;
            const char* last_zone[2]{"", ""};
        };
        GestureState g_gesture;

        // Which holster zone a hand is in, relative to the head: "hip" (either side, below the
        // belt line), "back" (over the shoulder, behind the head), or nullptr.
        const char* holster_zone(const XrHandRaw& hand, const float head[3], float yaw, const Config& cfg)
        {
            float dx = hand.px - head[0], dy = hand.py - head[1], dz = hand.pz - head[2];
            // Into the head's frame: x right, z backwards (OpenXR), heading `yaw` about +Y.
            float c = std::cos(yaw), s = std::sin(yaw);
            float x = c * dx - s * dz;
            float z = s * dx + c * dz;
            float side = std::fabs(x);
            if (dy < -(float)cfg.holster_hip_below_m && dy > -1.0f && side > 0.05f && side < 0.45f && std::fabs(z) < 0.30f) return "hip";
            if (dy > -0.30f && dy < 0.25f && z > (float)cfg.holster_back_behind_m && side < 0.35f) return "back";
            return nullptr;
        }

        // ---- hands like Blade & Sorcery (bs_hands) ---------------------------------------------------
        // Squeeze = the hand closes on what it touches: a weapon or food (picked up), the handle of
        // a sheathed weapon at the hip or over the shoulder (drawn), the other hand's weapon (taken
        // over), or anything else the game lets a hand grab, people included (held by the bone
        // touched). Let go = the hand opens: a grab is let go, a weapon at the hip or behind the
        // shoulder goes into its sheath, a weapon anywhere else falls.
        struct BsHandState
        {
            bool closed_on_weapon[2]{false, false}; // the grip was squeezed while this hand held a weapon
            bool offhand = false;                    // the left hand holds the right hand's weapon as its second hand
            double offhand_since = 0.0;
        };
        BsHandState g_bs;

        void bs_grip(UObject* pawn, int h, const XrHandRaw hands[2], bool pressed, const char* zone, const Config& cfg)
        {
            const char* side = h ? "right" : "left";
            const wchar_t letter = h ? L'R' : L'L';
            bool full = holds_weapon(pawn, h);
            if (pressed)
            {
                g_bs.closed_on_weapon[h] = false;
                if (full)
                {
                    g_bs.closed_on_weapon[h] = true;
                    HSVR_LOG("Hands: %s grip closed on the weapon it holds%s", side, zone ? " (at a sheath place)" : "");
                    return;
                }
                // The second hand on a weapon: the empty left hand closes on the right hand's weapon
                // anywhere along its handle (within offhand_radius_m of the line from its pommel to
                // its right-hand grip point, as the game made the weapon), and holds it just where it
                // took it (B&S and the Chasms VR Framework: the hand neither snaps nor moves the
                // weapon); the weapon pull gives that hand a joint of its own there (arm_control.cpp).
                if (h == 0)
                {
                    UObject* weapon = object_at(pawn, STR("Weapon R"));
                    UObject* pommel = weapon ? object_at(weapon, STR("Pommel")) : nullptr;
                    UObject* grip = weapon ? object_at(weapon, STR("Grip R Hand Default Pos")) : nullptr;
                    if (!grip && weapon) grip = object_at(weapon, STR("Grip"));
                    Vec palm{}, a{}, b{};
                    if (pommel && grip && hand_world(pawn, 0, palm) && component_world(pommel, a) && component_world(grip, b))
                    {
                        // Out past the right hand too (towards the blade: a half-sword hold), by half a handle.
                        double ux = b.X - a.X, uy = b.Y - a.Y, uz = b.Z - a.Z;
                        double len = std::sqrt(ux * ux + uy * uy + uz * uz);
                        if (len > 2.0)
                        {
                            ux /= len, uy /= len, uz /= len;
                            double along = (palm.X - a.X) * ux + (palm.Y - a.Y) * uy + (palm.Z - a.Z) * uz;
                            double clamped = std::max(-5.0, std::min(len * 1.5, along));
                            double px = a.X + ux * clamped - palm.X, py = a.Y + uy * clamped - palm.Y, pz = a.Z + uz * clamped - palm.Z;
                            double d = std::sqrt(px * px + py * py + pz * pz);
                            if (d <= cfg.offhand_radius_m * 100.0)
                            {
                                g_bs.offhand = true;
                                g_bs.offhand_since = now_seconds();
                                shared().left_second_hand.store(true);
                                HSVR_LOG("Hands: left hand takes the weapon's handle %.0f cm from it, %.0f cm along it from the pommel (handle %.0f cm to the right hand's grip); held where it took it",
                                         d, along, len);
                                return;
                            }
                        }
                    }
                }
                const XrHandRaw& hand = hands[h];
                const XrHandRaw& other = hands[1 - h];
                float ddx = hand.px - other.px, ddy = hand.py - other.py, ddz = hand.pz - other.pz;
                float apart = std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz);
                if (other.valid && apart < (float)cfg.swap_distance_m && holds_weapon(pawn, 1 - h))
                {
                    HSVR_LOG("Hands: %s hand takes the weapon from the other hand (hands %.0f cm apart)", side, apart * 100.0f);
                    if (call_event(pawn, STR("Swap Weapons"))) g_bs.closed_on_weapon[h] = true;
                    return;
                }
                if (zone)
                {
                    // Something to grab right in the palm (a person low down, a prop) comes before
                    // the sheath (self-test: a knocked-down opponent's neck at the player's hip).
                    if (bs_grab(pawn, h, cfg.grab_radius_m * 100.0)) return;
                    HSVR_LOG("Hands: %s hand closes at the %s; drawing a weapon", side, zone);
                    if (call_event(pawn, letter == L'R' ? STR("Unsheath R Event") : STR("Unsheath L Event"))) g_bs.closed_on_weapon[h] = true;
                    return;
                }
                if (cfg.pickup && pick_up_near(pawn, h, cfg))
                {
                    g_bs.closed_on_weapon[h] = true;
                    return;
                }
                bs_grab(pawn, h, cfg.grab_radius_m * 100.0);
                return;
            }
            // Released.
            if (h == 0 && g_bs.offhand)
            {
                g_bs.offhand = false;
                shared().left_second_hand.store(false);
                HSVR_LOG("Hands: left hand lets go of the weapon's handle");
                return;
            }
            if (bs_grabbing(pawn, h))
            {
                bs_release(pawn, h);
                return;
            }
            if (!full)
            {
                // A weapon that has not arrived yet (drawn or picked up a moment ago) is not dropped.
                g_bs.closed_on_weapon[h] = false;
                return;
            }
            if (!g_bs.closed_on_weapon[h])
            {
                // Holding a weapon the grip never closed on (one the character started with, say):
                // opening a hand that was not closed lets nothing go.
                return;
            }
            g_bs.closed_on_weapon[h] = false;
            if (zone)
            {
                HSVR_LOG("Hands: %s hand lets go at the %s; putting the weapon away", side, zone);
                call_event(pawn, letter == L'R' ? STR("R Hand Sheath Start") : STR("L Hand Sheath Start"));
                return;
            }
            UObject* weapon = object_at(pawn, h ? STR("Weapon R") : STR("Weapon L"));
            bool asked = bs_drop(pawn, h);
            UObject* after = object_at(pawn, h ? STR("Weapon R") : STR("Weapon L"));
            HSVR_LOG("Hands: %s hand lets go; %ls %s", side, weapon ? weapon->GetName().c_str() : L"the weapon",
                     !asked ? "could not be dropped" : (after ? "is STILL in the hand" : "dropped"));
        }

        // The game puts the left hand on the right hand's weapon by itself whenever the left hand
        // comes near the place for it (its tick; so at spawn, where its own pose has the hands
        // together). With the left hand at the player's own hand that joint and the hand fight
        // (Matthew: two-handers "freaking out" from spawn; self-test: the left hand torn 120 cm off
        // two seconds in). With these hands the second hand is only on when the left grip put it there.
        void undo_auto_offhand(UObject* pawn)
        {
            // The game's own switch that stops its tick from attaching the second hand (one of the
            // conditions before "Attach L Hand To Offhand Position", @507795), held on every frame.
            // (Self-test run 10: taking the hand off after the fact had the game put it back 175
            // times.) The left grip's own attach calls that function directly, past the switch.
            if (bool* block = pawn->GetValuePtrByPropertyNameInChain<bool>(STR("Block Offhand Attach"))) *block = true;
            bool* attached = pawn->GetValuePtrByPropertyNameInChain<bool>(STR("L Hand In Offhand Attached"));
            static double next_release = 0.0;
            if (attached && *attached && !g_bs.offhand && now_seconds() >= next_release)
            {
                next_release = now_seconds() + 0.5;
                call_event(pawn, STR("Release Offhand"));
                HSVR_LOG("Hands: the game put the left hand on the weapon by itself; taken off (squeeze the left grip there to hold it two-handed)");
            }
            // (The left hand's own hold is the mod's, not the game's offhand joint: every game attach is taken off.)
            if (attached && *attached && g_bs.offhand && now_seconds() >= next_release)
            {
                next_release = now_seconds() + 0.5;
                call_event(pawn, STR("Release Offhand"));
            }
            if (!object_at(pawn, STR("Weapon R")) && g_bs.offhand)
            {
                g_bs.offhand = false;
                shared().left_second_hand.store(false);
            }
        }

        void gestures(UObject* pawn, const XrHandRaw hands[2], bool in_menu, const Config& cfg)
        {
            GestureState& g = g_gesture;
            bool active = cfg.controller_gestures && pawn && !in_menu && pawn->GetFunctionByNameInChain(STR("Kick Event"));
            float head[3];
            float yaw = 0;
            bool have_head = vr_head_in_tracking(head, yaw);
            for (int h = 0; h < 2; ++h)
            {
                const XrHandRaw& hand = hands[h];
                bool down = g.grip_down[h] ? hand.squeeze > (float)cfg.grip_release : hand.squeeze > (float)cfg.grip_press;
                bool pressed = down && !g.grip_down[h];
                bool released = !down && g.grip_down[h];
                {
                    // The grip's own reading over the last moment, written down at each let-go, so a
                    // session shows whether a let-go was the player's hand opening or the reading dipping.
                    static float history[2][48] = {};
                    static int at[2] = {0, 0};
                    history[h][at[h]] = hand.squeeze;
                    at[h] = (at[h] + 1) % 48;
                    if (released)
                    {
                        std::string text;
                        char v[8];
                        for (int i = 0; i < 48; ++i)
                        {
                            snprintf(v, sizeof(v), " %.2f", history[h][(at[h] + i) % 48]);
                            text += v;
                        }
                        HSVR_LOG("Grip: %s let go; the grip read over the last 48 frames (oldest first):%s", h ? "right" : "left", text.c_str());
                    }
                }
                g.grip_down[h] = down;
                if (!down) g.consumed[h] = false;
                if (cfg.bs_hands && active)
                {
                    // Hands like Blade & Sorcery: the grip is the hand closing. It never reaches the
                    // game as its grab key (a tap that toggles); what it means is done here.
                    if (down) g.consumed[h] = true;
                    if (hand.valid && (pressed || released)) bs_grip(pawn, h, hands, pressed, have_head ? holster_zone(hand, head, yaw, cfg) : nullptr, cfg);
                    continue;
                }
                if (!active || !pressed || !hand.valid) continue;
                const wchar_t side = h ? L'R' : L'L';
                bool full = holds_weapon(pawn, h);
                // Swap: an empty hand closes on the other hand while that one holds a weapon.
                const XrHandRaw& other = hands[1 - h];
                float ddx = hand.px - other.px, ddy = hand.py - other.py, ddz = hand.pz - other.pz;
                float apart = std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz);
                if (!full && other.valid && apart < (float)cfg.swap_distance_m && holds_weapon(pawn, 1 - h))
                {
                    HSVR_LOG("Gesture: %s hand takes the weapon from the other hand (hands %.0f cm apart)", h ? "right" : "left", apart * 100.0f);
                    g.consumed[h] = call_event(pawn, STR("Swap Weapons"));
                    continue;
                }
                const char* zone = have_head ? holster_zone(hand, head, yaw, cfg) : nullptr;
                if (!zone)
                {
                    if (!full && cfg.pickup) g.consumed[h] = pick_up_near(pawn, h, cfg);
                    // A hand that holds a weapon: the game's grab on that hand lets go of the
                    // weapon (seen in a test: a grip squeeze with a sword in hand, and the sword
                    // was gone). Squeezing a held weapon is natural in VR, so keep it from the game.
                    if (full && cfg.keep_held_weapon)
                    {
                        // A second squeeze soon after the first is a deliberate drop: that one
                        // goes to the game as its grab, which lets go of the weapon.
                        static double last_squeeze[2] = {-10.0, -10.0};
                        double now = now_seconds();
                        if (now - last_squeeze[h] <= cfg.drop_double_squeeze_s)
                        {
                            last_squeeze[h] = -10.0;
                            HSVR_LOG("Gesture: %s hand double-squeezes its weapon; dropping it (the game's grab)", h ? "right" : "left");
                        }
                        else
                        {
                            last_squeeze[h] = now;
                            g.consumed[h] = true;
                            HSVR_LOG("Gesture: %s hand squeezes its weapon; not passed on as the game's grab (it would drop it)", h ? "right" : "left");
                        }
                    }
                    continue;
                }
                if (full)
                {
                    HSVR_LOG("Gesture: %s hand puts its weapon away (%s)", h ? "right" : "left", zone);
                    g.consumed[h] = call_event(pawn, side == L'R' ? STR("R Hand Sheath Start") : STR("L Hand Sheath Start"));
                }
                else
                {
                    HSVR_LOG("Gesture: %s hand draws a weapon (%s)", h ? "right" : "left", zone);
                    g.consumed[h] = call_event(pawn, side == L'R' ? STR("Unsheath R Event") : STR("Unsheath L Event"));
                }
            }
            // Eating: food held up to the mouth (within 15 cm of the eyes) is eaten, once every 2 s.
            static double next_bite = 0.0;
            if (active && have_head && cfg.eat_at_mouth && now_seconds() >= next_bite)
            {
                for (int h = 0; h < 2; ++h)
                {
                    const XrHandRaw& hand = hands[h];
                    UObject* item = object_at(pawn, h ? STR("Weapon R") : STR("Weapon L"));
                    if (!hand.valid || !item) continue;
                    float dx = hand.px - head[0], dy = hand.py - head[1], dz = hand.pz - head[2];
                    if (dx * dx + dy * dy + dz * dz > 0.15f * 0.15f) continue;
                    FProperty* consumable = item->GetPropertyByNameInChain(STR("Consumable"));
                    bool food = false;
                    if (consumable)
                    {
                        auto* flag = static_cast<FBoolProperty*>(consumable);
                        food = (*(reinterpret_cast<uint8_t*>(item) + consumable->GetOffset_Internal() + flag->GetByteOffset()) & flag->GetFieldMask()) != 0;
                    }
                    if (!food) continue;
                    next_bite = now_seconds() + 2.0;
                    UFunction* eat = pawn->GetFunctionByNameInChain(STR("Eat Held Food"));
                    if (eat && eat->GetParmsSize() == 0)
                    {
                        HSVR_LOG("Gesture: %s hand holds food to the mouth; eating", h ? "right" : "left");
                        call_event(pawn, STR("Eat Held Food"));
                    }
                    else
                    {
                        HSVR_LOG("Gesture: food at the mouth, but Eat Held Food %s; not called", eat ? "takes parameters" : "is missing");
                    }
                    break;
                }
            }
            // Kick on the right controller's B.
            bool kick = hands[1].button_upper;
            if (active && cfg.kick_on_b && kick && !g.kick_down) call_event(pawn, STR("Kick Event"));
            g.kick_down = kick;
        }

        // ---- Matthew's stick layout (stick_layout) -------------------------------------------------
        // Calls a no-parameter-that-matters event of the character (the game's input events take
        // the key that fired them, which they only store).
        bool call_input_event(UObject* pawn, const TCHAR* name)
        {
            UFunction* fn = pawn->GetFunctionByNameInChain(name);
            if (!fn || fn->GetParmsSize() > 64) return false;
            alignas(16) uint8_t params[64] = {};
            pawn->ProcessEvent(fn, params);
            return true;
        }

        void stick_layout_tick(UObject* pawn, const XrHandRaw hands[2], PadOut& pad, const Config& cfg)
        {
            static bool sprint_down = false, kick_down = false, up_held = false, down_held = false, prone_sent = false, a_down = false;
            // Taunt: A is the game's Talk key alone (held, the game's own give-up hold as before).
            bool a = hands[1].button_lower;
            if (a != a_down)
            {
                a_down = a;
                call_input_event(pawn, a ? STR("InpActEvt_Talk_K2Node_InputActionEvent_33") : STR("InpActEvt_Talk_K2Node_InputActionEvent_34"));
                if (a) HSVR_LOG("Pad: A: taunt");
            }
            // Slow motion: the right controller's own X (Steam Frame), on pressing it.
            static bool x_was = false;
            bool x_now = hands[1].button_x;
            bool x_pressed = x_now && !x_was;
            x_was = x_now;
            if (x_pressed)
            {
                bool ok = call_input_event(pawn, STR("InpActEvt_SloMo_K2Node_InputActionEvent_8"));
                bool* on = pawn->GetValuePtrByPropertyNameInChain<bool>(STR("Slomo Active"));
                HSVR_LOG("Pad: X tapped: slow motion %s", !ok ? "event NOT found" : (on && *on ? "on" : "off (or not allowed: the game needs consciousness over 50 and stamina)"));
            }
            static double down_since = 0.0;
            double now = now_seconds();
            // Sprint: the left stick held in, as the game's Run key (Left Shift) held.
            bool sprint = hands[0].stick_click;
            if (sprint != sprint_down)
            {
                sprint_down = sprint;
                // (Its two input events: _4 is the press, _5 the release, as with the other keys.)
                bool ok = call_input_event(pawn, sprint ? STR("InpActEvt_Run_K2Node_InputActionEvent_4") : STR("InpActEvt_Run_K2Node_InputActionEvent_5"));
                HSVR_LOG("Pad: left stick click: sprint %s%s", sprint ? "on" : "off", ok ? "" : " (the game's Run event was NOT found)");
            }
            // Kick: the right stick pressed in.
            bool kick = hands[1].stick_click;
            if (kick && !kick_down) call_event(pawn, STR("Kick Event"));
            kick_down = kick;
            // The right stick up and down (left and right turn; only a push that is mostly up or
            // down counts, so turning never jumps or crouches).
            float x = hands[1].stick_x, y = hands[1].stick_y;
            bool up = up_held ? y > 0.5f && y > std::fabs(x) : y > 0.7f && y > std::fabs(x);
            bool down = down_held ? y < -0.5f && -y > std::fabs(x) : y < -0.7f && -y > std::fabs(x);
            // Jump: the game's own Jump key events (pressed while the stick is up, let go when it
            // comes back; the game jumps on the let-go). Not the gamepad's A, which is also Talk.
            if (up != up_held)
            {
                call_input_event(pawn, up ? STR("InpActEvt_Jump_K2Node_InputActionEvent_14") : STR("InpActEvt_Jump_K2Node_InputActionEvent_15"));
                HSVR_LOG(up ? "Pad: right stick up: jump (held)" : "Pad: right stick back from up: jump");
            }
            up_held = up;
            // Crouch and prone, by how long the stick is held down: under the game's own 0.2 s it is a
            // tap (crouch), held that long the game's own hold (prone).
            if (down && !down_held)
            {
                down_since = now;
                prone_sent = false;
            }
            bool placed = cfg.own_crouch && shared().body_placed.load();
            if (down && !prone_sent && now - down_since >= 0.2)
            {
                prone_sent = true;
                // What the game does when its crouch key is still held after 0.2 s (its delayed
                // callback, entered here after checking the script there starts as read: "return
                // unless Crouch Key Holding"): standing still it lies down ("Rest"), moving it dives.
                bool* holding = pawn->GetValuePtrByPropertyNameInChain<bool>(STR("Crouch Key Holding"));
                UFunction* graph = pawn->GetFunctionByNameInChain(STR("ExecuteUbergraph_Willie_BP"));
                const int32_t entry = 118040;
                bool ok = false;
                if (holding && graph && graph->GetParmsSize() == 4)
                {
                    auto& script = graph->GetScript();
                    ok = entry < script.Num() && script.GetData()[entry] == 0x4F; // EX_PopExecutionFlowIfNot
                }
                if (ok)
                {
                    if (placed) shared().own_crouch.store(false);
                    *holding = true;
                    int32_t param = entry;
                    pawn->ProcessEvent(graph, &param);
                    bool* rest = pawn->GetValuePtrByPropertyNameInChain<bool>(STR("Rest"));
                    bool* dive = pawn->GetValuePtrByPropertyNameInChain<bool>(STR("Dolphin Dive"));
                    HSVR_LOG("Pad: right stick held down: the game's prone (lying %s, diving %s)", rest && *rest ? "yes" : "no", dive && *dive ? "yes" : "no");
                }
                else
                {
                    HSVR_LOG("Pad: right stick held down: the game's prone code is not where it was (game updated?); nothing done");
                }
            }
            if (!down && down_held && !prone_sent)
            {
                if (placed)
                {
                    bool now_crouched = !shared().own_crouch.load();
                    shared().own_crouch.store(now_crouched);
                    HSVR_LOG(now_crouched ? "Pad: right stick down: crouching (the view goes down with the body)" : "Pad: right stick down: standing up");
                }
                else
                {
                    // Not placed (VR arms off, knocked out): the game's own crouch toggle.
                    if (bool* crouched = pawn->GetValuePtrByPropertyNameInChain<bool>(STR("Is Crouched??"))) *crouched = !*crouched;
                    HSVR_LOG("Pad: right stick down: the game's own crouch toggled");
                }
            }
            down_held = down;
        }

        // ---- switching between mouse and keyboard and the controllers (auto_input_switch) --------
        // A keyboard key (never the mouse: Matthew works menus with it while on VR controls) newly
        // pressed while the game's window is in front, other than the
        // mod's own keys, hands play to mouse and keyboard. A controller button (not X or Y),
        // trigger or grip pressed past 60 %, or a stick pushed past 60 %, hands it back. After a
        // switch the other side is not listened to for a second, so the hand that just let go of
        // one cannot flip it straight back.
        void auto_input_switch(const XrHandRaw* hands, bool have, const Config& cfg)
        {
            static bool key_was[256] = {};
            static double quiet_until = 0.0;
            double now = now_seconds();
            bool vr_off = shared().vr_controls_off.load();
            bool keyboard = false;
            int which = 0;
            HWND front = GetForegroundWindow();
            DWORD front_process = 0;
            if (front) GetWindowThreadProcessId(front, &front_process);
            bool ours = front_process == GetCurrentProcessId();
            for (int vk = 1; vk < 255; ++vk)
            {
                if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2 /* the mouse never switches (Matthew: menus) */ || vk == VK_ESCAPE || vk == VK_TAB /* Matthew: Escape, Alt and Tab stay out */ || vk == VK_PACKET || vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN /* Alt-Tab and Windows */ || vk == 0x21 /* Page Up */ || vk == cfg.recenter_key || vk == cfg.toggle_ui_key || vk == cfg.toggle_view_key || vk == cfg.toggle_mode_key) continue;
                bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
                if (down && !key_was[vk] && ours)
                {
                    keyboard = true;
                    which = vk;
                }
                key_was[vk] = down;
            }
            bool controller = false;
            const char* what = "";
            if (have && hands)
            {
                for (int h = 0; h < 2 && !controller; ++h)
                {
                    const XrHandRaw& c = hands[h];
                    if (c.trigger > 0.6f) what = "a trigger";
                    else if (c.squeeze > 0.6f) what = "a grip";
                    else if (std::fabs(c.stick_x) > 0.6f || std::fabs(c.stick_y) > 0.6f) what = "a stick";
                    else if (c.stick_click) what = "a stick click";
                    else if (h == 1 && (c.button_lower || c.button_upper || c.button_x || c.button_y)) what = "a button on the right controller";
                    else continue;
                    controller = true;
                }
            }
            if (now < quiet_until) return;
            if (keyboard && !vr_off)
            {
                arm_toggle();
                quiet_until = now + 1.0;
                HSVR_LOG("Input: key 0x%02X pressed: playing with mouse and keyboard (the controllers take over again when used)", which);
            }
            else if (controller && vr_off)
            {
                arm_toggle();
                quiet_until = now + 1.0;
                HSVR_LOG("Input: %s used on a controller: playing with the 6DoF controllers again", what);
            }
        }

        const char* source_name(Source s)
        {
            static const char* names[] = {"l_stick_x", "l_stick_y", "r_stick_x", "r_stick_y", "l_trigger", "r_trigger", "l_grip", "r_grip",
                                          "l_lower", "l_upper", "r_lower", "r_upper", "l_click", "r_click", "l_menu"};
            return s < kSourceCount ? names[s] : "?";
        }
    } // namespace

    void controller_input_install()
    {
        if (g_installed) return;
        g_installed = true;
        HMODULE xinput = LoadLibraryW(L"xinput1_4.dll");
        g_real_get_state = xinput ? reinterpret_cast<GetStateFn>(GetProcAddress(xinput, "XInputGetState")) : nullptr;
        if (!g_real_get_state)
        {
            HSVR_LOG("Pad: XInput (xinput1_4.dll) not found; controller sticks and buttons stay off");
            return;
        }
        const char* how = "";
        void** slot = find_get_state_slot(&how);
        if (!slot)
        {
            HSVR_LOG("Pad: the game's XInputGetState import was not found; controller sticks and buttons stay off");
            g_real_get_state = nullptr;
            return;
        }
        DWORD old = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old))
        {
            HSVR_LOG("Pad: could not unlock the game's XInput import (%lu); controller sticks and buttons stay off", GetLastError());
            g_real_get_state = nullptr;
            return;
        }
        *slot = reinterpret_cast<void*>(&hooked_get_state);
        VirtualProtect(slot, sizeof(void*), old, &old);
        HSVR_LOG("Pad: the game's gamepad reading (XInputGetState, %s) now goes through the mod", how);
    }

    const char* controller_input_tick(UObject* pawn, bool in_menu)
    {
        TickState& t = g_tick;
        Config cfg = config();
        // Mouse-and-keyboard mode: nothing from the controllers reaches the game. The mod's own
        // controller keys (X held switches back) and the menu pointer keep working.
        bool vr_off = shared().vr_controls_off.load();
        if (vr_off) cfg.auto_guard = cfg.controller_gestures = cfg.kick_on_b = cfg.pickup = cfg.eat_at_mouth = false;
        keep_guard(pawn, in_menu, cfg);
        log_weapon_functions(pawn);
        if (cfg.pad_map != t.parsed_text)
        {
            t.parsed_text = cfg.pad_map;
            t.map = parse_map(cfg.pad_map);
            std::string summary;
            for (const Mapping& m : t.map)
            {
                summary += source_name(m.source);
                summary += m.kind == kAxis ? "->axis" : m.kind == kTrigger ? "->trigger" : "->button";
                summary += " ";
            }
            HSVR_LOG("Pad: %d controller input(s) mapped: %s", (int)t.map.size(), summary.c_str());
        }

        XrHandRaw hands[2];
        bool have = g_real_get_state && cfg.controller_pad && vr_controllers_this_tick(hands);
        if (cfg.auto_input_switch && !cfg.dev_fake_hands)
        {
            auto_input_switch(have ? hands : nullptr, have, cfg);
            if (shared().vr_controls_off.load() != vr_off)
            {
                vr_off = shared().vr_controls_off.load();
                cfg = config();
                if (vr_off) cfg.auto_guard = cfg.controller_gestures = cfg.kick_on_b = cfg.pickup = cfg.eat_at_mouth = false;
            }
        }
        PadOut pad;
        if (have) mod_keys_on_controllers(hands, cfg);
        if (have) point_at_panel(hands, cfg, in_menu);
        if (have) gestures(pawn, hands, in_menu, cfg);
        else vr_set_panel_pointer(-1.0f, -1.0f);
        if (!have && cfg.dev_selftest && cfg.dev_fake_hands && shared().selftest_phase.load() == 12)
        {
            // Self-test, the hands part: the pretend controllers squeeze and let go as the test
            // says (vr_present.cpp), through the same hand logic as real ones.
            XrHandRaw fake[2];
            if (vr_locate_hands(fake)) gestures(pawn, fake, in_menu, cfg);
        }
        if (have && !vr_off)
        {
            pad.active = true;
            float value[kSourceCount] = {};
            value[kLStickX] = shape_stick(hands[0].stick_x, cfg);
            value[kLStickY] = shape_stick(hands[0].stick_y, cfg);
            value[kRStickX] = shape_stick(hands[1].stick_x, cfg);
            value[kRStickY] = shape_stick(hands[1].stick_y, cfg);
            value[kLTrigger] = hands[0].trigger;
            value[kRTrigger] = hands[1].trigger;
            value[kLGrip] = g_gesture.consumed[0] ? 0.0f : hands[0].squeeze;
            value[kRGrip] = g_gesture.consumed[1] ? 0.0f : hands[1].squeeze;
            value[kLLower] = hands[0].button_lower ? 1.0f : 0.0f;
            value[kLUpper] = hands[0].button_upper ? 1.0f : 0.0f;
            value[kRLower] = hands[1].button_lower ? 1.0f : 0.0f;
            value[kRUpper] = (hands[1].button_upper && !cfg.kick_on_b) ? 1.0f : 0.0f;
            value[kLClick] = hands[0].stick_click ? 1.0f : 0.0f;
            value[kRClick] = hands[1].stick_click ? 1.0f : 0.0f;
            value[kLMenu] = (hands[0].menu || hands[1].menu) ? 1.0f : 0.0f;
            float axes[4] = {};
            for (Mapping& m : t.map)
            {
                float v = value[m.source];
                switch (m.kind)
                {
                case kAxis: axes[m.axis] = std::clamp(axes[m.axis] + v, -1.0f, 1.0f); break;
                case kTrigger:
                    if (m.axis == 0) pad.lt = std::max(pad.lt, v);
                    else pad.rt = std::max(pad.rt, v);
                    break;
                case kButton:
                    // Analog sources (triggers, grips) press at 60 % and let go at 40 %.
                    m.down = m.down ? v > 0.4f : v > 0.6f;
                    if (m.down) pad.buttons |= m.button;
                    break;
                }
            }
            pad.lx = axes[0];
            pad.ly = axes[1];
            pad.rx = axes[2];
            pad.ry = axes[3];
        }
        if (cfg.bs_hands && cfg.controller_gestures && !vr_off && pawn && !in_menu && shared().body_placed.load()) undo_auto_offhand(pawn);
        if (have && !vr_off && cfg.stick_layout && pawn && !in_menu) stick_layout_tick(pawn, hands, pad, cfg);
        else g_x_tapped = false;
        if (cfg.own_crouch && shared().body_placed.load() && !vr_off)
        {
            // The crouch button (left stick press, LS) toggles the player's own crouch and is not
            // passed on: the game's crouch would move the character's hips by itself.
            static bool was_down = false;
            bool down = (pad.buttons & XINPUT_GAMEPAD_LEFT_THUMB) != 0;
            if (down && !was_down)
            {
                bool now_crouched = !shared().own_crouch.load();
                shared().own_crouch.store(now_crouched);
                HSVR_LOG(now_crouched ? "Pad: crouch button: crouching (the view goes down with the body)" : "Pad: crouch button: standing up");
            }
            was_down = down;
            pad.buttons &= (WORD)~XINPUT_GAMEPAD_LEFT_THUMB;
        }
        if (cfg.dev_selftest && shared().selftest_phase.load() == 7)
        {
            // Self-test, "walking": the left stick back and forward, as the test says, through the
            // same virtual gamepad the real controllers use.
            pad.active = true;
            pad.ly = shared().selftest_stick_y.load();
        }
        if (cfg.dev_selftest && (shared().selftest_phase.load() == 10 || shared().selftest_phase.load() == 12))
        {
            // Self-test, the kettle: the test steers by the heading and pushes the stick itself.
            pad.active = true;
            pad.ly = shared().selftest_stick_y.load();
        }
        if (cfg.dev_selftest && shared().selftest_phase.load() == 9)
        {
            // Self-test, crouched: a short walk forward and back at half stick.
            float pt = shared().selftest_phase_time.load();
            pad.active = true;
            pad.ly = pt > 6.0f && pt < 7.5f ? 0.5f : (pt >= 7.5f && pt < 9.0f ? -0.5f : 0.0f);
        }
        {
            std::lock_guard<std::mutex> lock(g_pad_mutex);
            g_pad = pad;
        }

        const char* command = nullptr;
        if (pad.active != t.was_active)
        {
            t.was_active = pad.active;
            HSVR_LOG(pad.active ? "Pad: controllers now play as gamepad 0" : "Pad: controllers no longer play as a gamepad");
            t.last_poll_check = now_seconds();
            t.polls_at_check = g_polls;
        }
        // The engine only asks about a pad it believes is plugged in, and looks for new pads when
        // Windows reports a device change. If it has not asked about pad 0 for a second while the
        // controllers are on, have it look once (one frame of XInput.ForceControllerStateUpdate).
        if (t.refresh_step == 1)
        {
            t.refresh_step = 2;
            command = "XInput.ForceControllerStateUpdate 0";
        }
        else if (pad.active && now_seconds() - t.last_poll_check >= 1.0)
        {
            LONG polls = g_polls;
            if (polls == t.polls_at_check)
            {
                HSVR_LOG("Pad: the game is not reading gamepad 0; asking it to look for gamepads");
                t.refresh_step = 1;
                command = "XInput.ForceControllerStateUpdate 1";
            }
            t.last_poll_check = now_seconds();
            t.polls_at_check = polls;
        }

        {
            // Anything the real pad 0 (Steam's own virtual pad, or a plugged-in one) presses is
            // merged into what the game sees; log every change so it can be told apart from ours.
            static LONG last_real = 0;
            LONG real = g_real_buttons;
            if (real != last_real)
            {
                last_real = real;
                HSVR_LOG("Pad: the real gamepad 0 now reads buttons 0x%04X triggers (%d, %d)%s", (unsigned)(real & 0xFFFF), (int)((real >> 16) & 0xFF),
                         (int)((real >> 24) & 0xFF), real ? " (passed on to the game with ours)" : "");
            }
        }
        if (cfg.log_input && pad.active)
        {
            const PadOut& l = t.last_logged;
            bool changed = pad.buttons != l.buttons || std::fabs(pad.lx - l.lx) > 0.25f || std::fabs(pad.ly - l.ly) > 0.25f ||
                           std::fabs(pad.rx - l.rx) > 0.25f || std::fabs(pad.ry - l.ry) > 0.25f || std::fabs(pad.lt - l.lt) > 0.3f ||
                           std::fabs(pad.rt - l.rt) > 0.3f;
            if (changed && log_throttle("pad-change", 0.2))
            {
                t.last_logged = pad;
                HSVR_LOG("Pad: sent left stick (%.2f, %.2f) right stick (%.2f, %.2f) triggers (%.2f, %.2f) buttons 0x%04X%s", pad.lx, pad.ly, pad.rx,
                         pad.ry, pad.lt, pad.rt, (unsigned)pad.buttons, in_menu ? " (menu up)" : "");
            }
        }
        return command;
    }

    void controller_input_log_bindings()
    {
        static bool done = false;
        if (done || !config().log_bindings) return;
        UObject* settings = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__InputSettings"));
        if (!settings) return;
        done = true;
        struct RawArray
        {
            uint8_t* data;
            int32_t num;
            int32_t max;
        };
        // FInputActionKeyMapping: FName ActionName; 4 modifier bits; FKey Key (its FName first).
        // FInputAxisKeyMapping: FName AxisName; float Scale; FKey Key.
        const struct
        {
            const TCHAR* property;
            bool axis;
        } lists[] = {{STR("ActionMappings"), false}, {STR("AxisMappings"), true}};
        for (const auto& list : lists)
        {
            FProperty* property = settings->GetPropertyByNameInChain(list.property);
            auto* array_property = property ? CastField<FArrayProperty>(property) : nullptr;
            FProperty* inner = array_property ? array_property->GetInner() : nullptr;
            auto* array = settings->GetValuePtrByPropertyNameInChain<RawArray>(list.property);
            if (!inner || !array)
            {
                HSVR_LOG("Bindings: %ls not found", list.property);
                continue;
            }
            int32_t stride = inner->GetElementSize();
            if (stride < 24 || stride > 128 || array->num < 0 || array->num > 2000)
            {
                HSVR_LOG("Bindings: %ls has an unexpected layout (element %d bytes, %d entries)", list.property, stride, array->num);
                continue;
            }
            HSVR_LOG("Bindings: %d %ls", array->num, list.property);
            for (int32_t i = 0; i < array->num; ++i)
            {
                uint8_t* entry = array->data + (size_t)i * stride;
                FName name = *reinterpret_cast<FName*>(entry);
                FName key = *reinterpret_cast<FName*>(entry + 16);
                if (list.axis)
                {
                    float scale = *reinterpret_cast<float*>(entry + 8);
                    HSVR_LOG("Bindings:   axis \"%ls\" <- %ls x %.2f", name.ToString().c_str(), key.ToString().c_str(), scale);
                }
                else
                {
                    uint8_t mods = entry[8];
                    HSVR_LOG("Bindings:   action \"%ls\" <- %ls%s%s%s", name.ToString().c_str(), key.ToString().c_str(), (mods & 1) ? " +shift" : "",
                             (mods & 2) ? " +ctrl" : "", (mods & 4) ? " +alt" : "");
                }
            }
        }
    }
} // namespace hsvr
