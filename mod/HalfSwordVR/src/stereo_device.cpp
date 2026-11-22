#include "stereo_device.hpp"

#include "config.hpp"
#include "engine_texture.hpp"
#include "log.hpp"
#include "ue_abi.hpp"
#include "vr_present.hpp"

#include <windows.h>
#include <d3d12.h>

#include <atomic>
#include <cmath>

namespace hsvr
{
    namespace
    {
        AnchorProvider g_anchor_provider = nullptr;
        LatchCallback g_latch_callback = nullptr;
        std::atomic<uint64_t> g_game_frame{1};
        double g_delta_seconds = 1.0 / 90.0;

        // True while the engine draws the eyes into their own full-size target instead of the
        // game window. Decided once per game frame.
        std::atomic<bool> g_separate{false};
        // Once the engine has been given our render target manager it must keep getting it.
        std::atomic<bool> g_manager_exposed{false};
        std::atomic<uint32_t> g_target_width{0}, g_target_height{0};

        // ---- per-frame view state (game thread only) -----------------------------------------
        struct FrameView
        {
            uint64_t game_frame = 0;
            bool valid = false;
            FramePose pose;
            Vec3 anchor;           // world position of the point between the eyes at recenter pose
            double base_yaw = 0.0; // world heading that "straight ahead" maps to
            Recenter recenter;
            double cm_per_metre = 100.0; // world centimetres per real metre (last seen)
            Vec3 eye_at[2];              // each eye as the engine last drew it (world), for the helmet probe
            Quat eye_rot[2];
            bool eye_valid[2] = {false, false};
        };
        FrameView g_view;
        bool g_have_smoothed = false;
        Vec3 g_smoothed_offset; // head position relative to the actor, smoothed

        void latch_frame(const ue::FRotator& camera_rotation, const ue::FVector& camera_location)
        {
            uint64_t frame = g_game_frame.load();
            if (g_view.game_frame == frame) return;
            g_view.game_frame = frame;
            Config cfg = config();

            FramePose pose;
            g_view.valid = vr_latch_frame_pose(pose);
            if (!g_view.valid) return;
            g_view.pose = pose;
            if (shared().recenter_requested.exchange(false)) vr_set_recenter_from(pose);
            g_view.recenter = vr_recenter();
            if (!g_view.recenter.valid)
            {
                vr_set_recenter_from(pose);
                g_view.recenter = vr_recenter();
            }

            g_view.base_yaw = camera_rotation.Yaw;
            Vec3 camera{camera_location.X, camera_location.Y, camera_location.Z};
            g_view.anchor = camera;
            PlayerAnchor player;
            bool have_player = g_anchor_provider && g_anchor_provider(player) && player.has_head;
            bool first_person = cfg.anchor == "head" && !shared().third_person.load();
            double camera_yaw = g_view.base_yaw;
            if (first_person && have_player)
            {
                // Straight ahead follows where the player aims rather than the game's camera:
                // the camera trails the aim and shakes on hits, which in a headset would swing
                // the whole world around.
                if (cfg.heading == "aim" && player.has_heading) g_view.base_yaw = player.heading_yaw;
                Vec3 exact = cfg.anchor_room_unsmoothed ? player.exact_offset : Vec3{};
                Vec3 offset = player.head_position - player.actor_position - exact;
                if (!g_have_smoothed || cfg.anchor_smoothing <= 0.0)
                {
                    g_smoothed_offset = offset;
                    g_have_smoothed = true;
                }
                else
                {
                    double alpha = 1.0 - std::exp(-g_delta_seconds / cfg.anchor_smoothing);
                    Vec3 delta = offset - g_smoothed_offset;
                    // A big jump (teleport, respawn) is taken at once.
                    if (delta.x * delta.x + delta.y * delta.y + delta.z * delta.z > 150.0 * 150.0) alpha = 1.0;
                    g_smoothed_offset = g_smoothed_offset + delta * alpha;
                }
                Quat base = quat_from_yaw_deg(g_view.base_yaw);
                g_view.anchor = player.actor_position + g_smoothed_offset + exact + base.rotate({cfg.eye_forward_cm, 0.0, 0.0}) + Vec3{0.0, 0.0, cfg.eye_up_cm};
                // Test aid: the view's own jumps (with the headset still it should stay put while his feet follow).
                static Vec3 last_anchor, last_actor;
                static bool have_last = false;
                if (have_last)
                {
                    Vec3 da = g_view.anchor - last_anchor, dc = player.actor_position - last_actor;
                    double a = std::sqrt(da.x * da.x + da.y * da.y), c = std::sqrt(dc.x * dc.x + dc.y * dc.y);
                    if (a > 1.0 && a < 150.0) HSVR_LOG_EVERY(0.25, "View: anchor moved %.1f cm this frame (capsule %.1f cm)", a, c);
                }
                last_anchor = g_view.anchor;
                last_actor = player.actor_position;
                have_last = true;
            }
            else
            {
                g_have_smoothed = false;
            }

            if (cfg.log_poses)
            {
                HSVR_LOG_EVERY(1.0, "Pose: anchor (%.0f, %.0f, %.0f) base yaw %.0f (camera yaw %.0f), camera (%.0f, %.0f, %.0f), head bone %s, %s",
                               g_view.anchor.x, g_view.anchor.y, g_view.anchor.z, g_view.base_yaw, camera_yaw, camera.x, camera.y, camera.z,
                               player.has_head ? "found" : "not found", first_person ? "first person" : "game camera");
            }
        }

        // The eye a view index stands for. Index -1 is the engine's single "both eyes" view used
        // for deciding what is visible: midway between the eyes, wide enough to cover both.
        XrEyeRaw eye_for_view(int32_t view_index)
        {
            if (view_index >= 0) return g_view.pose.eye[view_index > 0 ? 1 : 0];
            const XrEyeRaw& a = g_view.pose.eye[0];
            const XrEyeRaw& b = g_view.pose.eye[1];
            XrEyeRaw m = a;
            m.px = (a.px + b.px) * 0.5f;
            m.py = (a.py + b.py) * 0.5f;
            m.pz = (a.pz + b.pz) * 0.5f;
            float dot = a.qx * b.qx + a.qy * b.qy + a.qz * b.qz + a.qw * b.qw;
            float sign = dot < 0.0f ? -1.0f : 1.0f;
            float qx = a.qx + sign * b.qx, qy = a.qy + sign * b.qy, qz = a.qz + sign * b.qz, qw = a.qw + sign * b.qw;
            float n = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
            if (n > 1e-6f)
            {
                m.qx = qx / n;
                m.qy = qy / n;
                m.qz = qz / n;
                m.qw = qw / n;
            }
            // If the two displays are angled apart, each eye looks half that angle away from the middle.
            float half_cant = std::acos(std::fmin(1.0f, std::fabs(dot)));
            m.angle_left = std::fmin(a.angle_left, b.angle_left) - half_cant;
            m.angle_right = std::fmax(a.angle_right, b.angle_right) + half_cant;
            m.angle_up = std::fmax(a.angle_up, b.angle_up) + half_cant;
            m.angle_down = std::fmin(a.angle_down, b.angle_down) - half_cant;
            const float limit = 1.5f; // just under 90 degrees
            m.angle_left = std::fmax(m.angle_left, -limit);
            m.angle_right = std::fmin(m.angle_right, limit);
            m.angle_up = std::fmin(m.angle_up, limit);
            m.angle_down = std::fmax(m.angle_down, -limit);
            return m;
        }

        // ---- the render target manager ---------------------------------------------------------
        // Tells the engine to draw both eyes side by side into one texture of the headset's size
        // rather than into the game window. Everything else is left to the engine's defaults.
        class FHalfSwordRenderTargetManager final : public ue::IStereoRenderTargetManager
        {
          public:
            bool ShouldUseSeparateRenderTarget() const override { return g_separate.load(); }
            void UpdateViewport(bool, const ue::FViewport&, ue::SViewport*) override {}
            void CalculateRenderTargetSize(const ue::FViewport&, uint32_t& InOutSizeX, uint32_t& InOutSizeY) override
            {
                int w = shared().eye_width.load(), h = shared().eye_height.load();
                if (w <= 0 || h <= 0) return;
                InOutSizeX = (uint32_t)w * 2;
                InOutSizeY = (uint32_t)h;
                if (g_target_width.exchange(InOutSizeX) != InOutSizeX || g_target_height.exchange(InOutSizeY) != InOutSizeY)
                {
                    HSVR_LOG("Engine asked for the eye target size: %ux%u (both eyes side by side)", InOutSizeX, InOutSizeY);
                }
            }
            bool NeedReAllocateViewportRenderTarget(const ue::FViewport&) override
            {
                uint32_t w = g_target_width.load(), h = g_target_height.load();
                if (w == 0 || h == 0) return false;
                return w != (uint32_t)shared().eye_width.load() * 2 || h != (uint32_t)shared().eye_height.load();
            }
            bool NeedReAllocateDepthTexture(const void*) override { return false; }
            bool NeedReAllocateShadingRateTexture(const void*) override { return false; }
            uint32_t GetNumberOfBufferedFrames() const override { return 1; }
            bool AllocateRenderTargetTexture(uint32_t, uint32_t, uint32_t, uint8_t, uint32_t, uint64_t, uint64_t, void*, void*, uint32_t) override { return false; }
            bool AllocateRenderTargetTextures(uint32_t, uint32_t, uint8_t, uint32_t, uint64_t, uint64_t, void*, void*, uint32_t) override
            {
                // We let the engine make the texture itself; this call only tells us it is doing so now.
                vr_render_thread_target_reset();
                return false;
            }
            uint8_t GetActualColorSwapchainFormat() const override { return 0; }
            int32_t AcquireColorTexture() override { return -1; }
            bool AllocateDepthTexture(uint32_t, uint32_t, uint32_t, uint8_t, uint32_t, uint64_t, uint64_t, void*, void*, uint32_t) override { return false; }
            bool AllocateShadingRateTexture(uint32_t, uint32_t, uint32_t, uint8_t, uint32_t, uint64_t, uint64_t, void*, void*) override { return false; }
            bool HDRGetMetaDataForStereo(void*, void*, bool&) override { return false; }
            bool ReconfigureForShaderPlatform(uint32_t) override { return false; }
        };

        FHalfSwordRenderTargetManager* render_target_manager()
        {
            static FHalfSwordRenderTargetManager* instance = new FHalfSwordRenderTargetManager();
            return instance;
        }

        // ---- the device ------------------------------------------------------------------------
        class FHalfSwordStereoDevice final : public ue::IStereoRendering
        {
          public:
            ~FHalfSwordStereoDevice() override = default;

            bool IsStereoEnabled() const override { return true; }
            bool IsStereoEnabledOnNextFrame() const override { return true; }
            bool EnableStereo(bool) override { return true; }
            int32_t GetDesiredNumberOfViews(bool bStereoRequested) const override { return bStereoRequested ? 2 : 1; }
            ue::EStereoscopicPass GetViewPassForIndex(bool bStereoRequested, int32_t ViewIndex) const override
            {
                if (!bStereoRequested) return ue::EStereoscopicPass::eSSP_FULL;
                return ViewIndex == 0 ? ue::EStereoscopicPass::eSSP_PRIMARY : ue::EStereoscopicPass::eSSP_SECONDARY;
            }
            uint32_t GetLODViewIndex() const override { return 0; }
            bool IsStandaloneStereoOnlyDevice() const override { return false; }

            void AdjustViewRect(int32_t ViewIndex, int32_t& X, int32_t& Y, uint32_t& SizeX, uint32_t& SizeY) const override
            {
                int index = ViewIndex > 0 ? 1 : 0;
                if (g_separate.load())
                {
                    int w = shared().eye_width.load(), h = shared().eye_height.load();
                    X = index * w;
                    Y = 0;
                    SizeX = (uint32_t)w;
                    SizeY = (uint32_t)h;
                }
                else
                {
                    SizeX = SizeX / 2;
                    X += (int32_t)SizeX * index;
                }
            }

            void SetFinalViewRect(ue::FRHICommandListImmediate&, const int32_t ViewIndex, const ue::FIntRect& FinalViewRect) override
            {
                vr_render_thread_view_rect(ViewIndex, FinalViewRect.MinX, FinalViewRect.MinY, FinalViewRect.MaxX, FinalViewRect.MaxY);
            }

            ue::FVector2D GetTextSafeRegionBounds() const override { return {0.75, 0.75}; }

            void CalculateStereoViewOffset(const int32_t ViewIndex, ue::FRotator& ViewRotation, const float WorldToMeters, ue::FVector& ViewLocation) override
            {
                uint64_t latched_before = g_view.game_frame;
                latch_frame(ViewRotation, ViewLocation);
                if (!g_view.valid) return;
                if (g_view.game_frame != latched_before && g_latch_callback) g_latch_callback();
                Config cfg = config();
                const XrEyeRaw eye = eye_for_view(ViewIndex);
                double cm_per_metre = (WorldToMeters > 1.0f ? (double)WorldToMeters : 100.0) * cfg.world_scale;
                g_view.cm_per_metre = cm_per_metre;
                Quat recenter_inverse = quat_from_yaw_deg(g_view.recenter.yaw_deg).inverse();
                Vec3 eye_real_cm = xr_to_ue_position(eye.px, eye.py, eye.pz, 100.0);
                Vec3 relative = recenter_inverse.rotate(eye_real_cm - g_view.recenter.position_cm) * (cm_per_metre / 100.0);
                Quat relative_rotation = recenter_inverse * xr_to_ue_rotation(eye.qx, eye.qy, eye.qz, eye.qw);
                Quat base = quat_from_yaw_deg(g_view.base_yaw);
                Vec3 location = g_view.anchor + base.rotate(relative);
                Rot rotation = quat_to_rot((base * relative_rotation).normalized());
                if (shared().photo_on.load())
                {
                    // Test pictures: a fixed camera (hsvr.photo_cam), the eyes 3.2 cm either side of it.
                    double yaw = shared().photo_yaw.load(), pitch = shared().photo_pitch.load();
                    Quat q = quat_from_yaw_deg(yaw);
                    double side = ViewIndex == 0 ? -3.2 : (ViewIndex > 0 ? 3.2 : 0.0);
                    location = Vec3{shared().photo_x.load(), shared().photo_y.load(), shared().photo_z.load()} + q.rotate({0.0, side, 0.0});
                    rotation.pitch = pitch;
                    rotation.yaw = yaw;
                    rotation.roll = 0.0;
                }
                ViewLocation.X = location.x;
                ViewLocation.Y = location.y;
                ViewLocation.Z = location.z;
                ViewRotation.Pitch = rotation.pitch;
                ViewRotation.Yaw = rotation.yaw;
                ViewRotation.Roll = rotation.roll;
                if (ViewIndex >= 0 && ViewIndex < 2)
                {
                    g_view.eye_at[ViewIndex] = location;
                    g_view.eye_rot[ViewIndex] = (base * relative_rotation).normalized();
                    g_view.eye_valid[ViewIndex] = true;
                }
            }

            ue::FMatrix GetStereoProjectionMatrix(const int32_t ViewIndex) const override
            {
                // Off-centre perspective with reversed, infinite depth: the same form Unreal's own
                // OpenXR plugin produces.
                const XrEyeRaw eye = eye_for_view(ViewIndex);
                double left = std::tan((double)eye.angle_left);
                double right = std::tan((double)eye.angle_right);
                double up = std::tan((double)eye.angle_up);
                double down = std::tan((double)eye.angle_down);
                if (!g_view.valid || right - left < 1e-3 || up - down < 1e-3)
                {
                    left = down = -1.0;
                    right = up = 1.0;
                }
                double z_near = config().near_clip_cm;
                double window = shared().helmet_window_cm.load();
                if (window > z_near) z_near = window; // helmet_view 3: the helmet's walls right around your eyes not drawn
                double sum_rl = right + left, sum_tb = up + down;
                double inv_rl = 1.0 / (right - left), inv_tb = 1.0 / (up - down);
                ue::FMatrix m{};
                m.M[0][0] = 2.0 * inv_rl;
                m.M[1][1] = 2.0 * inv_tb;
                m.M[2][0] = sum_rl * -inv_rl;
                m.M[2][1] = sum_tb * -inv_tb;
                m.M[2][2] = 0.0;
                m.M[2][3] = 1.0;
                m.M[3][2] = z_near;
                return m;
            }

            void InitCanvasFromView(ue::FSceneView*, ue::UCanvas*) override {}

            void RenderTexture_RenderThread(ue::FRHICommandListImmediate&, ue::FRHITexture*, ue::FRHITexture* SrcTexture, ue::FVector2D) const override
            {
                // Called on the render thread once per frame while the engine draws the eyes into
                // their own target: the frame is finished, so pass its texture on to the headset.
                if (!g_separate.load()) return;
                ID3D12Resource* scene = engine_texture_resource(SrcTexture);
                if (scene) vr_render_thread_frame_done(scene);
            }

            // With no manager the engine draws both eyes side by side into the game window and the
            // presenter copies each half to the headset. With it, the eyes get their own target.
            ue::IStereoRenderTargetManager* GetRenderTargetManager() override
            {
                if (g_separate.load()) g_manager_exposed.store(true);
                return g_manager_exposed.load() ? render_target_manager() : nullptr;
            }
            ue::IStereoLayers* GetStereoLayers() override { return nullptr; }
            void StartFinalPostprocessSettings(ue::FPostProcessSettings*, const ue::EStereoscopicPass, const int32_t) override {}
            bool OverrideFinalPostprocessSettings(ue::FPostProcessSettings*, const ue::EStereoscopicPass, const int32_t, float&) override { return false; }
            void EndFinalPostprocessSettings(ue::FPostProcessSettings*, const ue::EStereoscopicPass, const int32_t) override {}
        };

        // TSharedPtr control block (ThreadSafe mode). The counts start huge so the engine never frees us.
        class FReferenceController
        {
          public:
            virtual void DestroyObject() {}
            virtual ~FReferenceController() = default;
            std::atomic<int32_t> SharedReferenceCount{0x40000000};
            std::atomic<int32_t> WeakReferenceCount{0x40000000};
        };

        FHalfSwordStereoDevice* device()
        {
            static FHalfSwordStereoDevice* instance = new FHalfSwordStereoDevice();
            return instance;
        }
        FReferenceController* controller()
        {
            static FReferenceController* instance = new FReferenceController();
            return instance;
        }
    } // namespace

    void stereo_set_anchor_provider(AnchorProvider provider)
    {
        g_anchor_provider = provider;
    }

    void stereo_set_latch_callback(LatchCallback callback)
    {
        g_latch_callback = callback;
    }

    void stereo_begin_game_frame(double delta_seconds)
    {
        g_game_frame.fetch_add(1);
        if (delta_seconds > 0.0 && delta_seconds < 0.5) g_delta_seconds = delta_seconds;

        int override_mode = shared().separate_override.load();
        bool asked = override_mode < 0 ? config().separate_render_target : override_mode != 0;
        if (config().dev_fake_headset && !config().dev_fake_separate) asked = false; // the pretend headset shows both eyes in the window
        bool want_separate = asked && shared().eye_width.load() > 0 && shared().eye_height.load() > 0 && !engine_texture_unavailable();
        if (want_separate != g_separate.load())
        {
            g_separate.store(want_separate);
            vr_set_separate_target_mode(want_separate);
            HSVR_LOG(want_separate ? "Eyes are drawn into their own target at headset resolution" : "Eyes are drawn side by side through the game window");
        }
    }

    // Where the view will be anchored this frame, worked out now (the same sum latch_frame makes when the
    // views are set up, late in the frame, without moving its smoothing on). Hands placed during the
    // frame from the anchor of the frame before were off by however far he moved since: while walking
    // his hands shook against the view by the capsule's own step-to-step jerk (playtest 11:31).
    bool current_anchor(Vec3& anchor, double& base_yaw)
    {
        anchor = g_view.anchor;
        base_yaw = g_view.base_yaw;
        Config cfg = config();
        if (!cfg.tracking_current_anchor || !g_have_smoothed || !g_anchor_provider) return true;
        if (cfg.anchor != "head" || shared().third_person.load()) return true;
        PlayerAnchor player;
        if (!g_anchor_provider(player) || !player.has_head) return true;
        if (cfg.heading == "aim" && player.has_heading) base_yaw = player.heading_yaw;
        Vec3 exact = cfg.anchor_room_unsmoothed ? player.exact_offset : Vec3{};
        anchor = player.actor_position + g_smoothed_offset + exact + quat_from_yaw_deg(base_yaw).rotate({cfg.eye_forward_cm, 0.0, 0.0}) + Vec3{0.0, 0.0, cfg.eye_up_cm};
        return true;
    }

    bool stereo_tracking_to_world(const XrPoseRaw& pose, Vec3& location, Quat& rotation)
    {
        if (!g_view.valid || !g_view.recenter.valid) return false;
        Vec3 anchor;
        double base_yaw = 0.0;
        current_anchor(anchor, base_yaw);
        // Exactly the placement the eyes get in CalculateStereoViewOffset.
        Quat recenter_inverse = quat_from_yaw_deg(g_view.recenter.yaw_deg).inverse();
        Vec3 real_cm = xr_to_ue_position(pose.px, pose.py, pose.pz, 100.0);
        Vec3 relative = recenter_inverse.rotate(real_cm - g_view.recenter.position_cm) * (g_view.cm_per_metre / 100.0);
        Quat relative_rotation = recenter_inverse * xr_to_ue_rotation(pose.qx, pose.qy, pose.qz, pose.qw);
        Quat base = quat_from_yaw_deg(base_yaw);
        location = anchor + base.rotate(relative);
        rotation = (base * relative_rotation).normalized();
        return true;
    }

    bool stereo_view_anchor(Vec3& anchor, double& base_yaw)
    {
        if (!g_view.valid) return false;
        return current_anchor(anchor, base_yaw); // the same anchor stereo_tracking_to_world places things from
    }

    bool stereo_last_eye(int index, Vec3& location, Quat& rotation)
    {
        if (index < 0 || index > 1 || !g_view.eye_valid[index]) return false;
        location = g_view.eye_at[index];
        rotation = g_view.eye_rot[index];
        return true;
    }

    bool stereo_separate_target()
    {
        return g_separate.load();
    }

    void* stereo_device_object()
    {
        return static_cast<ue::IStereoRendering*>(device());
    }

    void* stereo_device_reference_controller()
    {
        return controller();
    }
} // namespace hsvr
