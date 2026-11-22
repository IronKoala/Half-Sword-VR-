// The headset side of the mod: OpenXR session, per-frame pose bookkeeping, and copying the
// engine's finished eye images to the headset from inside the game's Present call.
#pragma once
#include "vr_state.hpp"

struct ID3D12Resource;

namespace hsvr
{
    // Loads the OpenXR runtime, creates the instance and reads the headset's view sizes.
    // Returns false when VR is unavailable; the game then runs flat as usual.
    bool vr_start();

    // Hooks IDXGISwapChain::Present so we can find the game's D3D12 queue and submit frames.
    void vr_install_present_hook();

    // ---- Game thread ------------------------------------------------------------------------
    // Asks the headset where the eyes will be when the next frame is shown and records that pose
    // as belonging to the frame the game is about to draw. Returns false if no pose is known yet.
    bool vr_latch_frame_pose(FramePose& out);
    // Tests with the pretend headset and dev_fake_real_eyes: where its head is (the last latched pose's middle),
    // so the head lock and the helmet follow it; and a steady turn through straight ahead.
    struct XrPoseRaw;
    bool vr_fake_head(XrPoseRaw& out);
    void vr_fake_turn(double degrees_per_second);

    // Marks the current head position and heading as "straight ahead".
    struct Recenter
    {
        bool valid = false;
        Vec3 position_cm;     // Unreal axes, real-world centimetres
        double yaw_deg = 0.0; // Unreal heading of the head at recenter time
    };
    Recenter vr_recenter();
    void vr_set_recenter_from(const FramePose& pose);

    // ---- Render thread ----------------------------------------------------------------------
    // The scene for one eye has been drawn into this rectangle of the engine's render target.
    void vr_render_thread_view_rect(int view_index, int min_x, int min_y, int max_x, int max_y);
    // The frame is complete and about to be presented; `scene` is the D3D12 texture holding both
    // eyes (nullptr when the engine draws straight into the window).
    void vr_render_thread_frame_done(ID3D12Resource* scene);
    // The engine is (re)creating its eye target. It does that with all earlier frames fully
    // presented, so frames in flight are forgotten here and counting starts afresh.
    void vr_render_thread_target_reset();
    // Tells the presenter whether frames arrive through vr_render_thread_frame_done at all.
    void vr_set_separate_target_mode(bool separate);
} // namespace hsvr
