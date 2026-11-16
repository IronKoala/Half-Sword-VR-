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
        // Where the player is aiming (the controller's heading, set by the mouse), in degrees.
        bool has_heading = false;
        double heading_yaw = 0.0;
    };
    using AnchorProvider = bool (*)(PlayerAnchor& out);
    void stereo_set_anchor_provider(AnchorProvider provider);

    // Puts a controller into the game world the same way the eyes are put there (recentering,
    // head anchor, heading, world scale), using the most recent frame's view. Game thread only.
    // `rot` is in Unreal axes: its X axis is the controller's handle direction (OpenXR -Z).
    // The point between the eyes in tracking space (OpenXR axes, metres) and the head's level
    // forward direction, from the most recent frame's view. Game thread only.
    bool stereo_head_tracking(Vec3& pos, Vec3& forward);

    // What the most recent frame's view knows about the player's head and the character (game
    // thread only). False while there is no first-person view of a character.
    struct HeadWorld
    {
        Vec3 actor_position;      // the character's position when the view was taken, world, cm
        bool head_valid = false;  // the three below are set (settings key body_follow)
        Vec3 lean;                // where the head bone should be relative to its neutral place, world axes, cm
        Vec3 head_bone;           // where the character's head bone belongs for this view, world, cm
        Quat rotation;            // the headset's rotation in the world (X forward, Z up)
        double body_gap_cm = 0.0; // distance between the view and the character's real eyes
    };
    bool stereo_head_world(HeadWorld& out);

    bool stereo_tracking_to_world(float px, float py, float pz, float qx, float qy, float qz, float qw, Vec3& pos, Quat& rot);

    // Called once per engine tick on the game thread, before the frame is drawn.
    void stereo_begin_game_frame(double delta_seconds);

    // True while the engine draws the eyes into their own full-size target (settings key
    // separate_render_target) rather than side by side into the game window.
    bool stereo_separate_target();

    // The device object and its shared-pointer control block, laid out the way the engine expects
    // a TSharedPtr<IStereoRendering> to look.
    void* stereo_device_object();
    void* stereo_device_reference_controller();
} // namespace hsvr
