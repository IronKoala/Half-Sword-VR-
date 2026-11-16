// Controller sticks and buttons drive the game: walking, turning, grabbing, guarding and so on.
//
// Half Sword already plays with a gamepad (its input settings bind gamepad keys to every action).
// The mod hands the game a virtual gamepad: it takes over the game's own XInput call
// (XInputGetState, imported by the game exe from XINPUT1_4.dll) and answers it for pad 0 with the
// VR controllers' sticks and buttons, mapped by the `pad_map` setting. A real gamepad on pad 0
// still works; its sticks and buttons are merged in. Nothing is sent to Windows; only the game's
// own question "what is gamepad 0 doing?" gets a different answer.
#pragma once

namespace RC::Unreal
{
    class UObject;
}

namespace hsvr
{
    // Patches the game's import of XInputGetState. Call once at start-up (any thread).
    void controller_input_install();

    // Once per engine tick on the game thread. `in_menu` is true while a menu or pause screen shows.
    // `pawn` is the player's character (may be null). Returns an engine console command to run now,
    // or nullptr.
    const char* controller_input_tick(RC::Unreal::UObject* pawn, bool in_menu);

    // Writes the game's input bindings (action and axis mappings) to the log, once.
    void controller_input_log_bindings();
} // namespace hsvr
