// The player's body from the headset and controllers: one self-contained full-body solver
// (Matthew 2026-11-24: "the entire body posing and the animation for VR ... needs to be completely
// redone ... create our own. Look to Blade and Sorcery and Chasm's VR framework").
//
// The solve follows Chasm's VR Framework (crates/ik, MIT licence, (c) 2026 chasmlol), itself after
// VRIK/FRIK and Parger et al. 2018: the head joint hangs from the eyes; the torso turns from the
// head and both hands; the spine pitches by how far and how the head dropped (crouching with a
// level gaze bends the knees, looking down bends the spine); the hips never go lower than the knees
// can fold (past that the spine bends instead), nor higher than the legs reach; feet are planted in
// the world and step only when balance needs it, lifting, travelling and landing on eased curves;
// the knee always points between the foot and the hips' facing, so it can never flip.
//
// Blade & Sorcery's split decides the legs (ThunderRoad Player.cs: VRIK's locomotion weight is
// 1 - clamp(speed / 1 m/s + turn / 30 deg/s)): standing still, balance steps; moving, a gait.
//
// Everything here is plain maths in Unreal's world axes (X forward at yaw 0, Y right, Z up,
// centimetres), with no engine calls, so it runs the same in the game and in the offline replay
// test (tools/body/replay_solve.cpp).
#pragma once
#include "vrmath.hpp"

namespace hsvr::body
{
    // Joints the solver poses.
    enum Joint
    {
        J_PELVIS, J_SPINE_02, J_SPINE_03, J_SPINE_04, J_SPINE_05, J_NECK_01, J_NECK_02, J_HEAD,
        J_THIGH_L, J_CALF_L, J_FOOT_L, J_BALL_L,
        J_THIGH_R, J_CALF_R, J_FOOT_R, J_BALL_R,
        J_COUNT
    };
    const char* joint_bone(int j); // the Unreal bone name ("pelvis", "spine_02", ..., "head", "thigh_l", ...)

    // The body standing in his reference pose (Willie_Lopoly's T-pose: every bone's rotation is the
    // identity), turned to face +X: each joint relative to the floor under the pelvis, centimetres,
    // already scaled to his size.
    struct Skeleton
    {
        bool valid = false;
        Vec3 joint[J_COUNT];
        double scale = 1.0;
        // Derived.
        double thigh = 0, shin = 0;      // hip joint to knee, knee to ankle (mean of both legs)
        double ankle_height = 0;          // foot bone above the floor standing
        Vec3 ball_from_ankle;             // the ball of the foot from the ankle (+X forward, Z up)
        double hip_half = 0;              // hip joints either side of the pelvis
        double eye_height = 0;            // eyes above the floor standing (set by build from eye offsets)
        void build(double eye_forward_cm, double eye_up_cm);
    };

    struct Params
    {
        // Torso (Chasm BodyParams, metres there; centimetres here).
        double torso_yaw_clamp_deg = 80.0, torso_yaw_blend = 0.7, torso_yaw_tau = 0.08, torso_yaw_tau_head_only = 0.5;
        double face_radius = 15.0, above_head_range = 29.0, hand_cross_range = 70.0;
        // Spine.
        double spine_bend_deg = 120.0, spine_bend_head_pitch = 0.2, bend_gaze_start_deg = 10.0, bend_gaze_full_deg = 40.0;
        double pelvis_back = 0.5, min_head_height = 80.0, spine_tau = 0.05;
        double twist_share_hips = 0.3; // of the chest's twist over the feet, the hips take this much
        // Feet and legs.
        double foot_spacing = 24.0, step_threshold = 40.0, step_reach = 0.55, step_angle_deg = 50.0;
        double step_time_min = 0.22, step_time_max = 0.33, leg_cross_guard = 20.0, step_height = 8.0;
        double support_margin = 3.0, settle_distance = 10.0, settle_delay = 1.0;
        double gait_start_speed = 50.0, gait_stop_speed = 29.0, gait_step_time_min = 0.28, gait_step_time_max = 0.5, gait_spacing = 0.5;
        double knee_max_flexion_deg = 150.0, fold_bend_dps = 240.0, heel_lift_max_deg = 35.0, leg_stretch = 1.02, hip_release_tau = 0.15;
        double root_dead_zone_deg = 25.0;
        double gait_stride_share = 0.8, gait_step_time_floor = 0.16; // stride held to the legs' reach (see feet_update)
        double hips_pull_max_cm = 12.0;
        double lean_max_cm = 35.0, lean_tau = 0.1; // torso lean: most hip offset it takes back, smoothing
        double loco_speed = 100.0, loco_turn_dps = 30.0; // B&S ikLocomotionSpeedThreshold (1 m/s), ...AngularSpeedThreshold // the hips come down at most this far to keep a foot on the floor
        // Chasm: the hips (the button crouch, a recentre) never move faster than this, cm/s.
        double hips_max_speed = 250.0;
    };

    struct Input
    {
        double dt = 1.0 / 60.0;
        Vec3 eyes;                 // the view's eye point, world
        Quat head_rot;             // the headset's rotation, world
        Vec3 eye_offset;           // the eyes from the head bone, in the head's frame (eye_forward, 0, eye_up)
        bool hand_valid[2] = {false, false};
        Vec3 hand[2];              // controller grips, world
        double heading_deg = 0.0;  // the capsule's facing (the game's control yaw)
        Vec3 capsule;              // the capsule's middle, world
        Vec3 velocity;             // the capsule's velocity, world cm/s (stick walking and room walking)
        double floor_z[2] = {0.0, 0.0}; // the floor under each foot (world z)
        bool reset = false;        // start over (new pawn, recentre, teleport)
        bool kick[2] = {false, false};
        Vec3 kick_ankle[2];        // where a kick puts the ankle, world
    };

    struct Output
    {
        bool valid = false;
        Vec3 pos[J_COUNT];         // world
        Quat rot[J_COUNT];         // world rotation of the bone, relative to its T-pose facing +X
        // Diagnostics.
        double knee_deg[2] = {0, 0};   // knee flexion
        double pitch_deg = 0;          // spine pitch
        double torso_yaw_deg = 0;      // chest's yaw
        double hips_short_cm = 0;      // how far the hips were held up by the knees (spine bent instead)
        double hips_pulled_cm = 0;     // how far the hips were pulled down to reach the feet
        double lean_cm = 0;            // the torso lean: how far it carries the hips back over the feet
        double heel_deg[2] = {0, 0};
        double lift_cm[2] = {0, 0};
        bool stepping[2] = {false, false};
        bool walking = false;
        double locomotion_weight = 1.0; // B&S: 1 still (balance steps) .. 0 moving
        int steps = 0;
    };

    class Solver
    {
    public:
        Params p;
        Skeleton skel;
        Output solve(const Input& in);
        void reset() { have_state_ = false; }
        // The feet's floor spots (world, z on the floor) for the glue's ground traces.
        Vec3 foot_spot(int side) const { return feet_[side].pos; }

    private:
        struct Step
        {
            Vec3 from;
            double from_yaw = 0, progress = 0, duration = 0.3, length = 0;
            bool landed = false;
        };
        struct Foot
        {
            Vec3 pos; // floor point under the ankle (world; z = floor)
            double yaw = 0;
            bool stepping = false;
            Step step;
        };
        struct Spring
        {
            double value = 0, speed = 0;
            void step(double error, double dt, double tau);
        };
        bool have_state_ = false;
        double torso_yaw_ = 0, root_yaw_ = 0;
        Spring torso_follow_, pitch_;
        Foot feet_[2];
        int gait_ = -1; // (unused: the moving legs are the gait cycle)
        double gait_phase_ = 0.0, last_root_yaw_ = 0.0, turn_rate_ = 0.0;
        Vec3 gait_dir_{1, 0, 0};
        Vec3 last_capsule_, capsule_vel_, lean_vec_;
        double last_weight_ = 1.0, stood_s_ = 1.0;
        bool have_capsule_ = false;
        double still_time_ = 0;
        Vec3 com_, com_vel_;
        bool have_com_ = false;
        double hip_drop_ = 0;
        Vec3 last_hips_;
        bool have_hips_ = false;
        int steps_ = 0;
        double prev_raw_hand_yaw_ = 0;
        bool have_prev_raw_ = false, flipped_ = false;

        double torso_update(const Input& in, double head_yaw, const Vec3& head_joint, double dt);
        double foot_lift(const Foot& f) const;
        void feet_update(const Input& in, double dt, const Vec3& com, const Vec3& com_home, double root_yaw, double hips_yaw, const Vec3 hip_joints[2]);
        bool advance(Foot& f, const Vec3& target, double yaw);
        Vec3 spot(const Vec3& center, double yaw, int side, double spacing) const;
        Vec3 cross_guard(const Vec3& target, const Vec3& other, double yaw, int side) const;
        double outside_support(const Vec3& p) const;
    };
} // namespace hsvr::body
