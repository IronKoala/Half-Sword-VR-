// A record of what could be steering the game besides the player's mouse and keyboard: which
// window has the keyboard, whether a mouse pointer is showing, whether a gamepad (real, or one
// Steam makes out of VR controllers) is pushing a stick, and how fast the view is turning.
// It only reads state and writes "Input:" lines to the log; it never sends input.
#pragma once

namespace hsvr
{
    struct InputWatchGame
    {
        bool have_player = false; // a player controller exists
        bool paused = false;      // the game reports itself paused
        double view_yaw = 0.0;    // the player's control heading, degrees
    };

    // Call once per game tick (game thread).
    void input_watch_tick(const InputWatchGame& game);
} // namespace hsvr
