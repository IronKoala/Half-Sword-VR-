// The headset's two hand controllers: poses, buttons, sticks and vibration, through an OpenXR
// action set. Phase 2 only reads them and writes them to the log; nothing is sent to the game.
#pragma once
#include "vr_state.hpp"
#include "xr_loader.hpp"

#include <cstdint>

namespace hsvr
{
    struct HandState
    {
        bool grip_valid = false; // position and orientation of the grip are known this frame
        bool aim_valid = false;
        XrPoseRaw grip; // where the hand holds the controller (tracking space)
        XrPoseRaw aim;  // the pointing ray, from the front of the controller
        float trigger = 0.0f;
        float squeeze = 0.0f;
        float stick_x = 0.0f, stick_y = 0.0f;
        bool stick_click = false;
        bool primary = false;   // A on the right hand, X on the left (or the Index A buttons)
        bool secondary = false; // B / Y
        bool menu = false;
    };

    struct ControllerState
    {
        bool active = false;        // the action set is attached and has been read at least once
        bool focused = false;       // the session has input focus (otherwise all inputs read as idle)
        int64_t predicted_time = 0; // XrTime the poses were predicted for
        bool head_valid = false;
        XrPoseRaw head; // point between the eyes at the same time, for relative positions
        HandState hand[2]; // 0 = left, 1 = right
    };

    // After the instance exists: makes the action set and suggests bindings for the common
    // controller layouts. Failure is logged; VR goes on without controllers.
    bool controllers_create(const XrApi& xr, XrInstance instance);
    // After the session exists (once per session): attaches the action set and makes the pose spaces.
    bool controllers_attach(const XrApi& xr, XrSession session);
    // Game thread, once per frame while the session runs: reads every input, locating the poses
    // at `time` (the same time the eyes are predicted for) in `space`.
    void controllers_update(const XrApi& xr, XrSession session, XrSpace space, int64_t time, const XrPoseRaw* head);
    // The runtime says the controllers or their layout changed: writes down which layout is in use.
    void controllers_profile_changed(const XrApi& xr, XrInstance instance, XrSession session);

    // Latest state (copy).
    ControllerState controllers_state();
    // Asks for a buzz on a hand (0 left, 1 right), strength 0..1, sent with the next input update.
    // The strongest request since the last update wins.
    void controllers_buzz(int hand, float amplitude);
} // namespace hsvr
