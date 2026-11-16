#include "input_watch.hpp"

#include "config.hpp"
#include "log.hpp"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>

namespace hsvr
{
    namespace
    {
        // ---- gamepads (XInput, loaded by hand so the mod has no extra dependency) ----------------
        struct PadGamepad
        {
            WORD buttons;
            BYTE left_trigger;
            BYTE right_trigger;
            SHORT lx, ly, rx, ry;
        };
        struct PadState
        {
            DWORD packet;
            PadGamepad pad;
        };
        using GetStateFn = DWORD(WINAPI*)(DWORD, PadState*);

        GetStateFn pad_function()
        {
            static GetStateFn fn = nullptr;
            static bool tried = false;
            if (!tried)
            {
                tried = true;
                for (const wchar_t* name : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"})
                {
                    if (HMODULE module = LoadLibraryW(name))
                    {
                        fn = reinterpret_cast<GetStateFn>(reinterpret_cast<void*>(GetProcAddress(module, "XInputGetState")));
                        if (fn) break;
                    }
                }
                HSVR_LOG(fn ? "Input: watching for gamepads" : "Input: no gamepad library found, gamepads are not watched");
            }
            return fn;
        }

        // Stick position as -10..10 per axis, with the usual dead zone taken out, so that a
        // resting stick reads 0 and small wobbles do not look like changes.
        int stick_step(SHORT value)
        {
            double v = value / 32767.0;
            if (std::fabs(v) < 0.25) return 0;
            return (int)std::lround(v * 10.0);
        }

        // Text such as "pad 0: left stick (-10, 0) right stick (0, 0) buttons 0x0000".
        std::string describe_pads()
        {
            GetStateFn fn = pad_function();
            if (!fn) return "not watched";
            std::string text;
            for (DWORD i = 0; i < 4; ++i)
            {
                PadState state{};
                if (fn(i, &state) != ERROR_SUCCESS) continue;
                char line[160];
                snprintf(line, sizeof(line), "%spad %lu: left stick (%d, %d) right stick (%d, %d) triggers (%d, %d) buttons 0x%04X",
                         text.empty() ? "" : "; ", (unsigned long)i, stick_step(state.pad.lx), stick_step(state.pad.ly), stick_step(state.pad.rx),
                         stick_step(state.pad.ry), state.pad.left_trigger > 40 ? 1 : 0, state.pad.right_trigger > 40 ? 1 : 0,
                         (unsigned)state.pad.buttons);
                text += line;
            }
            return text.empty() ? "none" : text;
        }

        // ---- windows -----------------------------------------------------------------------------
        // "game" when one of this process's windows has the keyboard, otherwise what does.
        std::string describe_foreground(bool& is_game)
        {
            is_game = false;
            HWND window = GetForegroundWindow();
            if (!window) return "nothing";
            DWORD process = 0;
            GetWindowThreadProcessId(window, &process);
            if (process == GetCurrentProcessId())
            {
                is_game = true;
                return "game";
            }
            char class_name[64] = {};
            GetClassNameA(window, class_name, sizeof(class_name));
            char exe[MAX_PATH] = {};
            if (HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process))
            {
                DWORD size = sizeof(exe);
                if (!QueryFullProcessImageNameA(handle, 0, exe, &size)) exe[0] = 0;
                CloseHandle(handle);
            }
            const char* file = strrchr(exe, '\\');
            file = file ? file + 1 : exe;
            // The program name and window class are enough to tell what took the keyboard; the
            // window's title is left out because it can hold private text.
            char text[256];
            snprintf(text, sizeof(text), "another program (%s, window class %s)", file[0] ? file : "unknown", class_name);
            return text;
        }

        bool key_down(int vk)
        {
            return (GetAsyncKeyState(vk) & 0x8000) != 0;
        }

        double wrap_degrees(double d)
        {
            while (d > 180.0) d -= 360.0;
            while (d < -180.0) d += 360.0;
            return d;
        }
    } // namespace

    void input_watch_tick(const InputWatchGame& game)
    {
        if (!config().log_input) return;
        double now = now_seconds();

        bool front_is_game = false;
        std::string front = describe_foreground(front_is_game);
        CURSORINFO cursor{};
        cursor.cbSize = sizeof(cursor);
        bool pointer = GetCursorInfo(&cursor) && (cursor.flags & CURSOR_SHOWING) != 0;

        // Esc and mouse clicks, as they happen. Clicks only while a pointer shows or the game is paused (a menu),
        // otherwise every sword swing would be a line.
        static bool esc_was = false, click_was = false;
        bool esc = key_down(VK_ESCAPE);
        bool click = key_down(VK_LBUTTON);
        if (esc && !esc_was)
        {
            HSVR_LOG("Input: Esc pressed (keyboard goes to %s, game paused: %s, pointer showing: %s)", front.c_str(), game.paused ? "yes" : "no",
                     pointer ? "yes" : "no");
        }
        if (click && !click_was && (pointer || game.paused || game.wants_pointer))
        {
            POINT p = cursor.ptScreenPos;
            RECT client{};
            HWND window = GetForegroundWindow();
            if (window && front_is_game && GetClientRect(window, &client)) ScreenToClient(window, &p);
            HSVR_LOG("Input: click at (%ld, %ld) of %ld x %ld (keyboard goes to %s, game paused: %s, pointer showing: %s)", (long)p.x, (long)p.y,
                     (long)client.right, (long)client.bottom, front.c_str(), game.paused ? "yes" : "no", pointer ? "yes" : "no");
        }
        esc_was = esc;
        click_was = click;

        // Everything else four times a second; a line is written when something changed, and
        // once a second while the view is turning.
        static double last_poll = 0.0;
        if (now - last_poll < 0.25) return;
        double elapsed = now - last_poll;
        last_poll = now;

        static bool have_yaw = false;
        static double last_yaw = 0.0;
        double rate = 0.0;
        if (game.have_player && have_yaw && elapsed > 0.0 && elapsed < 1.0) rate = wrap_degrees(game.view_yaw - last_yaw) / elapsed;
        have_yaw = game.have_player;
        last_yaw = game.view_yaw;

        std::string pads = describe_pads();
        char state[768];
        snprintf(state, sizeof(state), "keyboard goes to %s, game paused: %s, pointer showing: %s, game wants a pointer: %s, gamepads: %s",
                 front.c_str(), game.paused ? "yes" : "no", pointer ? "yes" : "no", game.wants_pointer ? "yes" : "no", pads.c_str());

        static std::string last_state;
        static double last_line = 0.0;
        bool changed = last_state != state;
        bool turning = std::fabs(rate) > 15.0;
        if (changed || (turning && now - last_line >= 1.0))
        {
            last_state = state;
            last_line = now;
            if (game.have_player)
            {
                HSVR_LOG("Input: %s; view heading %.0f, turning %+.0f degrees a second", state, game.view_yaw, rate);
            }
            else
            {
                HSVR_LOG("Input: %s", state);
            }
        }
    }
} // namespace hsvr
