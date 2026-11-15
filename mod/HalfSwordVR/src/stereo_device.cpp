#include "stereo_device.hpp"

#include "config.hpp"
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
        std::atomic<uint64_t> g_game_frame{1};
        double g_delta_seconds = 1.0 / 90.0;

        // ---- per-frame view state (game thread only) -----------------------------------------
        struct FrameView
        {
            uint64_t game_frame = 0;
            bool valid = false;
            FramePose pose;
            Vec3 anchor;           // world position of the point between the eyes at recenter pose
            double base_yaw = 0.0; // world heading that "straight ahead" maps to
            Recenter recenter;
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
            if (cfg.anchor == "head" && g_anchor_provider && g_anchor_provider(player) && player.has_head)
            {
                Vec3 offset = player.head_position - player.actor_position;
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
                g_view.anchor = player.actor_position + g_smoothed_offset + base.rotate({cfg.eye_forward_cm, 0.0, 0.0}) + Vec3{0.0, 0.0, cfg.eye_up_cm};
            }
            else
            {
                g_have_smoothed = false;
            }

            if (cfg.log_poses)
            {
                HSVR_LOG_EVERY(1.0, "Pose: anchor (%.0f, %.0f, %.0f) base yaw %.0f, camera (%.0f, %.0f, %.0f), head bone %s",
                               g_view.anchor.x, g_view.anchor.y, g_view.anchor.z, g_view.base_yaw, camera.x, camera.y, camera.z,
                               player.has_head ? "found" : "not found");
            }
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
                if (!bStereoRequested) return ue::eSSP_FULL;
                return ViewIndex == 0 ? ue::eSSP_PRIMARY : ue::eSSP_SECONDARY;
            }
            uint32_t GetLODViewIndex() const override { return 0; }
            bool IsStandaloneStereoOnlyDevice() const override { return false; }

            void AdjustViewRect(int32_t ViewIndex, int32_t& X, int32_t& Y, uint32_t& SizeX, uint32_t& SizeY) const override
            {
                int index = ViewIndex > 0 ? 1 : 0;
                if (config().separate_render_target)
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
                latch_frame(ViewRotation, ViewLocation);
                if (!g_view.valid) return;
                Config cfg = config();
                const XrEyeRaw& eye = g_view.pose.eye[ViewIndex > 0 ? 1 : 0];
                double cm_per_metre = (WorldToMeters > 1.0f ? (double)WorldToMeters : 100.0) * cfg.world_scale;
                Quat recenter_inverse = quat_from_yaw_deg(g_view.recenter.yaw_deg).inverse();
                Vec3 eye_real_cm = xr_to_ue_position(eye.px, eye.py, eye.pz, 100.0);
                Vec3 relative = recenter_inverse.rotate(eye_real_cm - g_view.recenter.position_cm) * (cm_per_metre / 100.0);
                Quat relative_rotation = recenter_inverse * xr_to_ue_rotation(eye.qx, eye.qy, eye.qz, eye.qw);
                Quat base = quat_from_yaw_deg(g_view.base_yaw);
                Vec3 location = g_view.anchor + base.rotate(relative);
                Rot rotation = quat_to_rot((base * relative_rotation).normalized());
                ViewLocation.X = location.x;
                ViewLocation.Y = location.y;
                ViewLocation.Z = location.z;
                ViewRotation.Pitch = rotation.pitch;
                ViewRotation.Yaw = rotation.yaw;
                ViewRotation.Roll = rotation.roll;
            }

            ue::FMatrix GetStereoProjectionMatrix(const int32_t ViewIndex) const override
            {
                // Off-centre perspective with reversed, infinite depth: the same form Unreal's own
                // OpenXR plugin produces.
                const XrEyeRaw& eye = g_view.pose.eye[ViewIndex > 0 ? 1 : 0];
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

            void RenderTexture_RenderThread(ue::FRHICommandListImmediate&, ue::FRHITexture* BackBuffer, ue::FRHITexture* SrcTexture, ue::FVector2D) const override;

            ue::IStereoRenderTargetManager* GetRenderTargetManager() override;
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

    void stereo_begin_game_frame(double delta_seconds)
    {
        g_game_frame.fetch_add(1);
        if (delta_seconds > 0.0 && delta_seconds < 0.5) g_delta_seconds = delta_seconds;
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
