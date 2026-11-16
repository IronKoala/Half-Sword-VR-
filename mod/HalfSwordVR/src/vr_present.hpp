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

    // Marks the current head position and heading as "straight ahead".
    struct Recenter
    {
        bool valid = false;
        Vec3 position_cm;     // Unreal axes, real-world centimetres
        double yaw_deg = 0.0; // Unreal heading of the head at recenter time
    };
    Recenter vr_recenter();
    void vr_set_recenter_from(const FramePose& pose);
    // Moves the recentered place by `position_delta_cm` (Unreal axes, real centimetres) and turns
    // its heading by `yaw_delta_deg`. Used by roomscale: when the character has walked or turned
    // under the player, the "neutral" place of the headset moves along.
    void vr_shift_recenter(const Vec3& position_delta_cm, double yaw_delta_deg);

    // Where the two controllers will be when the frame the game is about to make is shown
    // (index 0 = left hand, 1 = right). Reads the controller buttons too. Returns false when no
    // controller is known (no session yet, or none tracked).
    bool vr_locate_hands(XrHandRaw out[2]);

    // The controllers' sticks and buttons for this game tick (reuses vr_locate_hands' reading
    // when that already ran this tick). Returns false when there are no real controllers to read.
    bool vr_controllers_this_tick(XrHandRaw out[2]);

    // The menu panel as last shown, and the controller pointer's spot on it (u, v in 0..1 from the
    // top left; negative = no pointer drawn).
    PanelGeometry vr_panel_geometry();

    // Where the head was at the last latched frame, in tracking space (metres), and its heading
    // about +Y (radians, OpenXR sense: positive turns to the left). False before the first pose.
    bool vr_head_in_tracking(float pos[3], float& yaw);
    void vr_set_panel_pointer(float u, float v);

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
