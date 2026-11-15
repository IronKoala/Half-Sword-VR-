// Our implementation of the engine's stereo rendering device. Once installed in the running
// engine, the engine draws a left and a right view every frame using the headset's pose and
// lens shape, which we then hand to the headset.
#pragma once
#include "vrmath.hpp"

namespace hsvr
{
    // What the game glue knows about the player this frame (game thread only).
    struct PlayerAnchor
    {
        bool has_head = false;
        Vec3 head_position;  // world, cm
        Vec3 actor_position; // world, cm
    };
    using AnchorProvider = bool (*)(PlayerAnchor& out);
    void stereo_set_anchor_provider(AnchorProvider provider);

    // Called once per engine tick on the game thread, before the frame is drawn.
    void stereo_begin_game_frame(double delta_seconds);

    // The device object and its shared-pointer control block, laid out the way the engine expects
    // a TSharedPtr<IStereoRendering> to look.
    void* stereo_device_object();
    void* stereo_device_reference_controller();
} // namespace hsvr
