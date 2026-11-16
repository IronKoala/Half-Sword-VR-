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
#include "config.hpp"
#include "log.hpp"
#include "vr_present.hpp"

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
        PadOut pad;
        if (have) mod_keys_on_controllers(hands, cfg);
        if (have)
        {
            pad.active = true;
            float value[kSourceCount] = {};
            value[kLStickX] = shape_stick(hands[0].stick_x, cfg);
            value[kLStickY] = shape_stick(hands[0].stick_y, cfg);
            value[kRStickX] = shape_stick(hands[1].stick_x, cfg);
            value[kRStickY] = shape_stick(hands[1].stick_y, cfg);
            value[kLTrigger] = hands[0].trigger;
            value[kRTrigger] = hands[1].trigger;
            value[kLGrip] = hands[0].squeeze;
            value[kRGrip] = hands[1].squeeze;
            value[kLLower] = hands[0].button_lower ? 1.0f : 0.0f;
            value[kLUpper] = hands[0].button_upper ? 1.0f : 0.0f;
            value[kRLower] = hands[1].button_lower ? 1.0f : 0.0f;
            value[kRUpper] = hands[1].button_upper ? 1.0f : 0.0f;
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
