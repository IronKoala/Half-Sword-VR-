// The player's body solver: see body_solver.hpp. Ported in spirit from Chasm's VR Framework
// (crates/ik: solver.rs, spine.rs, torso.rs, legs.rs; MIT licence, (c) 2026 chasmlol), constants
// from its BodyParams, in centimetres and Unreal's axes.
#include "body_solver.hpp"

#include <algorithm>
#include <cmath>

namespace hsvr::body
{
    namespace
    {
        const Vec3 kUp{0, 0, 1};
        const double kMinPitch = -15.0 * kDegToRad, kMaxPitch = 115.0 * kDegToRad;
        const double kKneeMinFlex = 3.0 * kDegToRad;
        const double kLegEase = 0.01;   // last share of the leg's reach eased into straight
        const double kStepLiftOff = 0.15, kOverlap = 0.8, kLeftBias[2] = {0.9, 1.0};
        const double kStillSpeed = 10.0, kComAhead = 0.15, kComUpTheSpine = 0.3, kVelocityTau = 0.1;
        const double kHeelBack = 6.0, kToesAhead = 5.0;
        const double kNeckFollow = 0.5;
        // Spine segments from the pelvis up (pelvis->spine_02 is the pelvis bone with spine_01 riding on it).
        const double kPitchShare[5] = {0.85, 0.9, 1.0, 1.1, 1.15};
        const double kTwistShare[5] = {0.3, 0.5, 0.7, 0.85, 1.0}; // [0] is replaced by Params::twist_share_hips
        const double kShiftShare[5] = {1.0, 0.75, 0.5, 0.25, 0.0};

        Vec3 flat(const Vec3& v) { return {v.x, v.y, 0.0}; }
        double wrap(double a)
        {
            while (a > kPi) a -= 2.0 * kPi;
            while (a < -kPi) a += 2.0 * kPi;
            return a;
        }
        Vec3 yaw_dir(double yaw) { return {std::cos(yaw), std::sin(yaw), 0.0}; }
        Vec3 right_of(double yaw) { return {-std::sin(yaw), std::cos(yaw), 0.0}; }
        double yaw_of(const Vec3& v) { return std::atan2(v.y, v.x); }
        Quat yaw_rot(double yaw) { return quat_axis_angle({0, 0, 1}, yaw); }
        double smoothing(double dt, double tau) { return tau <= 1e-6 ? 1.0 : 1.0 - std::exp(-dt / tau); }
        double smoothstep(double a, double b, double x)
        {
            double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
            return t * t * (3.0 - 2.0 * t);
        }
        Vec3 normalize_or(const Vec3& v, const Vec3& fallback)
        {
            double n = length(v);
            return n > 1e-9 ? v * (1.0 / n) : fallback;
        }
        Vec3 lerp(const Vec3& a, const Vec3& b, double t) { return a + (b - a) * t; }
        // The bone's rotation from its T-pose (facing +X) frame (ref_along, ref_second) to (along, second).
        Quat frame_turn(const Vec3& along, const Vec3& second, const Vec3& ref_along, const Vec3& ref_second)
        {
            return (quat_from_two(along, second) * quat_from_two(ref_along, ref_second).inverse()).normalized();
        }
        double eased_reach(double d, double reach, double ease)
        {
            double knee = reach - ease;
            if (d <= knee) return d;
            return knee + ease * (1.0 - std::exp(-(d - knee) / ease));
        }
    } // namespace

    const char* joint_bone(int j)
    {
        static const char* const names[J_COUNT] = {"pelvis", "spine_02", "spine_03", "spine_04", "spine_05", "neck_01", "neck_02", "head",
                                                   "thigh_l", "calf_l", "foot_l", "ball_l", "thigh_r", "calf_r", "foot_r", "ball_r"};
        return j >= 0 && j < J_COUNT ? names[j] : "";
    }

    void Skeleton::build(double eye_forward_cm, double eye_up_cm)
    {
        thigh = 0.5 * (length(joint[J_CALF_L] - joint[J_THIGH_L]) + length(joint[J_CALF_R] - joint[J_THIGH_R]));
        shin = 0.5 * (length(joint[J_FOOT_L] - joint[J_CALF_L]) + length(joint[J_FOOT_R] - joint[J_CALF_R]));
        ankle_height = 0.5 * (joint[J_FOOT_L].z + joint[J_FOOT_R].z);
        ball_from_ankle = (joint[J_BALL_L] - joint[J_FOOT_L] + joint[J_BALL_R] - joint[J_FOOT_R]) * 0.5;
        ball_from_ankle.y = 0.0;
        hip_half = 0.5 * std::fabs(joint[J_THIGH_R].y - joint[J_THIGH_L].y);
        // The eyes, as the view places them from the head bone (eye_forward, eye_up), level.
        eye_height = joint[J_HEAD].z + eye_up_cm;
        (void)eye_forward_cm;
        valid = thigh > 1.0 && shin > 1.0 && eye_height > 50.0;
    }

    void Solver::Spring::step(double error, double dt, double tau)
    {
        // Critically damped (Chasm's Follow): value chases value + error with time constant tau.
        double target = value + error;
        double w = 1.0 / (std::max)(1e-3, tau);
        int n = (std::max)(1, (int)std::ceil(dt * w / 0.2));
        double h = dt / n;
        for (int i = 0; i < n; ++i)
        {
            double e = target - value;
            speed += (w * w * e - 2.0 * w * speed) * h;
            value += speed * h;
        }
    }

    double Solver::torso_update(const Input& in, double head_yaw, const Vec3& head_joint, double dt)
    {
        double target = head_yaw, tau = p.torso_yaw_tau_head_only;
        if (in.hand_valid[0] && in.hand_valid[1])
        {
            tau = p.torso_yaw_tau;
            const Vec3 L = in.hand[0], R = in.hand[1];
            Vec3 head_right = right_of(head_yaw);
            auto weight = [&](const Vec3& h) {
                double near = smoothstep(p.face_radius, 2.0 * p.face_radius, length(h - in.eyes));
                double above = 1.0 - (std::min)(1.0, (std::max)(0.0, h.z - in.eyes.z) / p.above_head_range);
                double horizontal = length(flat(h - head_joint));
                return near * above * smoothstep(5.0, 15.0, horizontal);
            };
            double crossing = (std::max)(0.0, dot(L - R, head_right));
            double uncrossed = 1.0 - (std::min)(1.0, crossing / p.hand_cross_range);
            Vec3 sum = normalize_or(flat(L - head_joint), {}) + normalize_or(flat(R - head_joint), {});
            Vec3 span = flat(R - L);
            double lateral = dot(span, head_right);
            double line_weight = 2.0 * smoothstep(10.0, 40.0, lateral) * (std::max)(0.0, 1.0 - 0.5 * length(sum));
            // The line between the hands, turned to face forward (its normal on the floor).
            sum = sum + normalize_or(Vec3{-span.y, span.x, 0.0}, {}) * line_weight;
            double magnitude = length(sum);
            double weights = (std::min)(weight(L), weight(R));
            double confidence = smoothstep(0.2, 0.8, magnitude) * weights * uncrossed;
            if (magnitude < 0.2)
                have_prev_raw_ = false;
            else
            {
                double raw = yaw_of(sum);
                bool steady = weights > 0.5;
                if (steady && have_prev_raw_)
                {
                    double jump = std::fmod(raw - prev_raw_hand_yaw_ + 4.0 * kPi, 2.0 * kPi) * kRadToDeg;
                    if (jump >= 150.0 && jump <= 210.0) flipped_ = !flipped_;
                }
                have_prev_raw_ = steady;
                prev_raw_hand_yaw_ = raw;
                double offset = wrap(raw + (flipped_ ? kPi : 0.0) - head_yaw);
                if (std::fabs(offset) > 120.0 * kDegToRad)
                {
                    if (steady) flipped_ = !flipped_;
                    offset = wrap(offset + kPi);
                }
                double clamp = p.torso_yaw_clamp_deg * kDegToRad;
                target = head_yaw + p.torso_yaw_blend * confidence * std::clamp(offset, -clamp, clamp);
            }
        }
        else
        {
            have_prev_raw_ = false;
            flipped_ = false;
        }
        torso_follow_.value = torso_yaw_;
        torso_follow_.step(wrap(target - torso_yaw_), dt, tau);
        torso_yaw_ = wrap(torso_follow_.value);
        double off = wrap(torso_yaw_ - head_yaw);
        if (std::fabs(off) > kPi * 0.5)
        {
            torso_yaw_ = wrap(head_yaw + std::copysign(kPi * 0.5, off));
            torso_follow_.speed = 0.0;
        }
        return torso_yaw_;
    }

    double Solver::foot_lift(const Foot& f) const
    {
        if (!f.stepping) return 0.0;
        double h = p.step_height * skel.scale * std::clamp(f.step.length / (25.0 * skel.scale), 0.5, 1.0);
        return 0.5 * (1.0 - std::cos(2.0 * kPi * (std::min)(1.0, f.step.progress / kOverlap))) * h;
    }

    Vec3 Solver::spot(const Vec3& center, double yaw, int side, double spacing) const
    {
        double sign = side == 0 ? -1.0 : 1.0;
        return flat(center) + right_of(yaw) * (sign * spacing * 0.5 * skel.scale);
    }

    Vec3 Solver::cross_guard(const Vec3& target, const Vec3& other, double yaw, int side) const
    {
        Vec3 outward = right_of(yaw) * (side == 0 ? -1.0 : 1.0);
        Vec3 rel = flat(target - other);
        double lateral = (std::max)(0.0, dot(rel, outward));
        Vec3 along = rel - outward * dot(rel, outward);
        double guard = p.leg_cross_guard * skel.scale;
        double needed = std::sqrt((std::max)(0.0, guard * guard - dot(along, along)));
        return flat(other) + along + outward * (std::max)(lateral, needed);
    }

    double Solver::outside_support(const Vec3& pt) const
    {
        Vec3 c[4];
        for (int i = 0; i < 2; ++i)
        {
            Vec3 fwd = yaw_dir(feet_[i].yaw);
            c[2 * i] = flat(feet_[i].pos) - fwd * (kHeelBack * skel.scale);
            c[2 * i + 1] = flat(feet_[i].pos) + fwd * (skel.ball_from_ankle.x + kToesAhead * skel.scale);
        }
        Vec3 q = flat(pt);
        // Convex outline (gift wrapping), then the distance to it.
        int start = 0;
        for (int k = 1; k < 4; ++k)
            if (c[k].x < c[start].x) start = k;
        int hull[4], n = 0;
        hull[n++] = start;
        for (;;)
        {
            int cur = hull[n - 1], next = (cur + 1) % 4;
            for (int k = 0; k < 4; ++k)
            {
                Vec3 a = c[next] - c[cur], b = c[k] - c[cur];
                if (a.x * b.y - a.y * b.x < 0.0) next = k; // (Unreal's floor is left-handed seen from above: turn the other way)
            }
            if (next == start || n == 4) break;
            hull[n++] = next;
        }
        bool inside = n >= 3;
        double nearest = 1e30;
        // Inside = on the same side of every edge.
        int sign = 0;
        for (int k = 0; k < n; ++k)
        {
            Vec3 a = c[hull[k]], b = c[hull[(k + 1) % n]], ab = b - a, ap = q - a;
            double cr = ab.x * ap.y - ab.y * ap.x;
            int s = cr > 0.0 ? 1 : (cr < 0.0 ? -1 : 0);
            if (s != 0)
            {
                if (sign == 0) sign = s;
                else if (s != sign) inside = false;
            }
            double t = std::clamp(dot(ap, ab) / (std::max)(1e-8, dot(ab, ab)), 0.0, 1.0);
            nearest = (std::min)(nearest, length(q - (a + ab * t)));
        }
        return inside ? 0.0 : nearest;
    }

    bool Solver::advance(Foot& f, const Vec3& target, double yaw)
    {
        if (!f.stepping) return false;
        Step& s = f.step;
        if (!s.landed)
        {
            double t = (std::min)(1.0, s.progress / kOverlap);
            double travel = std::clamp((t - kStepLiftOff) / (1.0 - 2.0 * kStepLiftOff), 0.0, 1.0);
            double ease = 0.5 - 0.5 * std::cos(kPi * travel);
            Vec3 to{target.x, target.y, f.pos.z};
            Vec3 from{s.from.x, s.from.y, f.pos.z};
            f.pos = lerp(from, to, ease);
            f.yaw = wrap(s.from_yaw + wrap(yaw - s.from_yaw) * ease);
            s.landed = travel >= 1.0;
        }
        if (s.progress >= 1.0)
        {
            f.stepping = false;
            ++steps_;
            return true;
        }
        return false;
    }

    void Solver::feet_update(const Input& in, double dt, const Vec3& com, const Vec3& com_home, double root_yaw, double hips_yaw, const Vec3 hip_joints[2])
    {
        for (int i = 0; i < 2; ++i) feet_[i].pos.z = in.floor_z[i];
        // Balance steps (VRIK's, as Chasm tuned them against motion capture).
        Vec3 ahead_vel = com_vel_;
        double av = length(ahead_vel);
        if (av > 40.0) ahead_vel = ahead_vel * (40.0 / av);
        Vec3 ground = flat(com + ahead_vel * kComAhead);
        still_time_ = length(flat(com_vel_)) < kStillSpeed ? still_time_ + dt : 0.0;
        Vec3 targets[2];
        // Where the feet belong: under the body as it would stand without the torso lean (com_home), so a
        // lean held still settles with a step under you after settle_delay, and a big lean steps there.
        Vec3 home = flat(com_home + ahead_vel * kComAhead);
        for (int i = 0; i < 2; ++i) targets[i] = cross_guard(spot(home, root_yaw, i, p.foot_spacing), feet_[1 - i].pos, root_yaw, i);
        for (int i = 0; i < 2; ++i)
            if (feet_[i].stepping)
            {
                feet_[i].step.progress += dt / feet_[i].step.duration;
                advance(feet_[i], targets[i], root_yaw);
            }
        for (int i = 0; i < 2; ++i)
            if (feet_[i].stepping && feet_[i].step.progress < kOverlap) return;
        double margin = p.support_margin * skel.scale;
        double outside = outside_support(ground) - margin;
        bool settled = still_time_ >= p.settle_delay;
        int best = -1;
        double best_need = 0.0;
        for (int i = 0; i < 2; ++i)
        {
            if (feet_[i].stepping) continue;
            double dist = length(flat(targets[i] - feet_[i].pos));
            double limit = p.step_threshold * skel.scale * kLeftBias[i];
            bool turned = std::fabs(wrap(hips_yaw - feet_[i].yaw)) > p.step_angle_deg * kDegToRad;
            double reach = (skel.thigh + skel.shin) * p.step_reach;
            double overreach = length(flat(hip_joints[i] - feet_[i].pos)) - reach;
            double need;
            if (outside > 0.0 || overreach > 0.0 || dist > limit || turned)
                need = 1.0 + (std::max)(0.0, outside / margin) + (std::max)(0.0, overreach / (10.0 * skel.scale)) + dist / limit;
            else if (settled && dist > p.settle_distance * skel.scale)
                need = dist / limit;
            else
                continue;
            if (best < 0 || need > best_need) best = i, best_need = need;
        }
        if (best >= 0)
        {
            Foot& f = feet_[best];
            double len = length(flat(targets[best] - f.pos));
            double turn = std::fabs(wrap(root_yaw - f.yaw));
            if (len < 3.0 * skel.scale && turn < 0.2) return;
            double urgency = std::clamp(best_need - 1.0, 0.0, 1.0);
            double duration = p.step_time_max + (p.step_time_min - p.step_time_max) * urgency;
            f.stepping = true;
            f.step = Step{f.pos, f.yaw, 0.0, duration, len, false};
            still_time_ = 0.0;
        }
    }

    Output Solver::solve(const Input& in)
    {
        Output out;
        if (!skel.valid) return out;
        const double s = skel.scale;
        double dt = std::clamp(in.dt, 0.0, 0.05);

        // ---- head ----
        Quat head_rot = in.head_rot.normalized();
        Vec3 head_joint = in.eyes - head_rot.rotate(in.eye_offset);
        Vec3 fwd = head_rot.rotate({1, 0, 0});
        Vec3 up_h = head_rot.rotate({0, 0, 1});
        // Facing: looking straight down, the top of the head still points the way you face.
        Vec3 facing = flat(fwd - up_h * fwd.z);
        double head_pitch = std::asin(std::clamp(-fwd.z, -1.0, 1.0));
        double head_yaw = length(facing) > 1e-6 ? yaw_of(facing) : in.heading_deg * kDegToRad;
        double floor = (std::min)(in.floor_z[0], in.floor_z[1]);

        if (!have_state_ || in.reset)
        {
            have_state_ = true;
            torso_yaw_ = root_yaw_ = head_yaw;
            torso_follow_ = Spring{head_yaw, 0.0};
            pitch_ = Spring{0.0, 0.0};
            Vec3 under = flat(head_joint);
            for (int i = 0; i < 2; ++i)
            {
                feet_[i] = Foot{};
                feet_[i].pos = spot(under, head_yaw, i, p.foot_spacing);
                feet_[i].pos.z = in.floor_z[i];
                feet_[i].yaw = head_yaw;
            }
            gait_ = -1;
            still_time_ = 0.0;
            have_com_ = false;
            com_vel_ = {};
            hip_drop_ = 0.0;
            have_hips_ = false;
            have_prev_raw_ = flipped_ = false;
            have_capsule_ = false;
            capsule_vel_ = {};
            lean_vec_ = {};
            last_weight_ = 1.0;
            stood_s_ = 1.0;
        }

        // ---- torso ----
        double torso_yaw = torso_update(in, head_yaw, head_joint, dt);
        {
            double off = wrap(torso_yaw - root_yaw_);
            double dz = p.root_dead_zone_deg * kDegToRad;
            if (std::fabs(off) > dz) root_yaw_ = wrap(root_yaw_ + off - std::copysign(dz, off));
        }
        double root_yaw = root_yaw_;
        double twist = wrap(torso_yaw - root_yaw);
        double seg_yaw[5];
        for (int i = 0; i < 5; ++i) seg_yaw[i] = root_yaw + twist * (i == 0 ? p.twist_share_hips : kTwistShare[i]);
        {
            // Moving on B&S's clips, his hips turn as the clip's do (its crouch walk stands side-on); the spine
            // turns back so the chest faces as before (17ab5a7's way). Last frame's clip turn, eased.
            static const double hips_share[5] = {1.0, 0.6, 0.25, 0.0, 0.0};
            double want = clip_hip_yaw_ * clip_weight_;
            clip_hip_shown_ += wrap(want - clip_hip_shown_) * smoothing(dt, 0.1);
            for (int i = 0; i < 5; ++i) seg_yaw[i] += clip_hip_shown_ * hips_share[i];
        }

        double height_ratio = 1.0; // B&S's Height: your level eye height over his standing eye height
        // ---- spine pitch: crouch versus bend (Parger, FRIK, Chasm) ----
        {
            double level_eyes = head_joint.z + in.eye_offset.z - floor;
            double h = (std::max)(level_eyes, p.min_head_height * s);
            double h0 = skel.eye_height;
            height_ratio = h / h0;
            double drop = std::clamp((h0 - h) / h0, 0.0, 1.0);
            double gaze = smoothstep(p.bend_gaze_start_deg, p.bend_gaze_full_deg, head_pitch * kRadToDeg);
            double bend = drop * p.spine_bend_deg * kDegToRad * gaze + p.spine_bend_head_pitch * head_pitch;
            double crouch = std::asin((std::min)(1.0, p.pelvis_back * drop));
            double target = std::clamp(bend + crouch * (1.0 - gaze), kMinPitch, kMaxPitch);
            pitch_.step(target - pitch_.value, dt, p.spine_tau);
        }

        // The chain hangs from the head joint: neck, then the five spine segments down to the pelvis.
        const Skeleton& K = skel;
        Vec3 ref_seg[5] = {K.joint[J_SPINE_02] - K.joint[J_PELVIS], K.joint[J_SPINE_03] - K.joint[J_SPINE_02], K.joint[J_SPINE_04] - K.joint[J_SPINE_03],
                           K.joint[J_SPINE_05] - K.joint[J_SPINE_04], K.joint[J_NECK_01] - K.joint[J_SPINE_05]};
        Vec3 ref_neck[2] = {K.joint[J_NECK_02] - K.joint[J_NECK_01], K.joint[J_HEAD] - K.joint[J_NECK_02]};
        struct Chain
        {
            Vec3 pos[8]; // pelvis, spine_02..05, neck_01, neck_02, head
            Quat seg[5]; // segment frames (yaw then pitch), body T -> world
            Vec3 neck_dir[2];
        };
        Quat lean_q[5]; // the torso lean (below), per segment
        auto chain_at = [&](double pitch) {
            Chain c;
            for (int i = 0; i < 5; ++i) c.seg[i] = (lean_q[i] * yaw_rot(seg_yaw[i]) * quat_axis_angle({0, 1, 0}, pitch * kPitchShare[i])).normalized();
            c.pos[7] = head_joint;
            for (int k = 1; k >= 0; --k)
            {
                Vec3 v = ref_neck[k];
                Vec3 d = normalize_or(c.seg[4].rotate(v) * (1.0 - kNeckFollow) + head_rot.rotate(v) * kNeckFollow, c.seg[4].rotate(normalized(v)));
                c.neck_dir[k] = d;
                c.pos[5 + k] = c.pos[6 + k] - d * length(v);
            }
            for (int i = 4; i >= 0; --i) c.pos[i] = c.pos[i + 1] - c.seg[i].rotate(ref_seg[i]);
            return c;
        };
        auto shift_hips = [&](Chain& c, double dz) { // down by dz (up when negative); the chest stays
            for (int i = 0; i < 5; ++i) c.pos[i].z -= dz * kShiftShare[i];
        };
        auto hip_joints = [&](const Chain& c, Vec3 out2[2]) {
            out2[0] = c.pos[0] + c.seg[0].rotate(K.joint[J_THIGH_L] - K.joint[J_PELVIS]);
            out2[1] = c.pos[0] + c.seg[0].rotate(K.joint[J_THIGH_R] - K.joint[J_PELVIS]);
        };
        Chain chain = chain_at(pitch_.value);
        Vec3 com_home = lerp(chain.pos[0], head_joint, kComUpTheSpine);

        // ---- torso lean (Matthew 2026-11-25: "keep the torso leaning, it's immersive") ----
        // Standing, his feet stay where they are and his torso bends to put his head at yours: the spine
        // tilts (forward, back or to a side) until his centre of mass is back over his feet, instead of
        // the whole body sliding under your head. Past body_lean_max_cm of head offset he steps instead
        // (the balance steps), and walking there is no lean.
        {
            Vec3 feet_mid = flat((feet_[0].pos + feet_[1].pos) * 0.5);
            Vec3 hips0 = chain.pos[0];
            com_home = lerp(hips0, head_joint, kComUpTheSpine);
            // centre of mass = hips + (head - hips) * k over the feet  ->  hips where it would be.
            Vec3 want_hips = (feet_mid - flat(head_joint) * kComUpTheSpine) * (1.0 / (1.0 - kComUpTheSpine));
            Vec3 d = flat(want_hips - hips0);
            double dl = length(d);
            double cap = p.lean_max_cm * s;
            if (dl > cap) d = d * (cap / dl);
            // Only standing, and only once he has stood a moment: stopping from a walk, the feet are where the
            // walk left them and the balance steps bring them under him (a lean there pulled him back over feet
            // left 30 cm behind, on his toes; test run 5).
            stood_s_ = last_weight_ > 0.98 ? stood_s_ + dt : 0.0;
            d = d * (std::clamp((stood_s_ - 0.3) / 0.5, 0.0, 1.0) * last_weight_);
            lean_vec_ = lerp(lean_vec_, d, smoothing(dt, p.lean_tau));
            double ls = length(chain.pos[5] - chain.pos[0]);
            double ll = length(lean_vec_);
            if (ll > 0.1 && ls > 1.0)
            {
                // Tilting the segments' tops away from where the hips must go moves the hips there.
                Vec3 away = lean_vec_ * (-1.0 / ll);
                Vec3 axis = normalized(cross(kUp, away));
                double theta = std::asin(std::clamp(ll / ls, 0.0, 0.8));
                for (int i = 0; i < 5; ++i) lean_q[i] = quat_axis_angle(axis, theta * kPitchShare[i]);
                chain = chain_at(pitch_.value);
            }
            out.lean_cm = ll;
        }

        // ---- feet ----
        Vec3 com = lerp(chain.pos[0], head_joint, kComUpTheSpine);
        if (have_com_ && dt > 0.0) com_vel_ = lerp(com_vel_, (com - com_) * (1.0 / dt), smoothing(dt, kVelocityTau));
        com_ = com;
        have_com_ = true;
        double hips_yaw = seg_yaw[0];
        Vec3 hips_now[2];
        hip_joints(chain, hips_now);
        feet_update(in, dt, com, com_home, root_yaw, hips_yaw, hips_now);
        double lifts[2] = {foot_lift(feet_[0]), foot_lift(feet_[1])};

        // ---- moving: a gait cycle (B&S: moving, the legs are the locomotion animation's) ----
        // Blade & Sorcery's locomotion weight: 1 standing (balance steps, feet planted in the world) .. 0
        // moving (each foot on a cycle under the hips: stance sliding back at the body's speed, swing
        // forward and up on eased curves). In between the two are blended, so nothing jumps.
        struct DrawFoot
        {
            Vec3 pos;
            double yaw = 0;
        } draw[2];
        double weight;
        {
            // B&S's weight is the locomotion's own speed (the capsule: stick or room walking), never the body's
            // sway: crouching or bending over moves the hips fast without going anywhere.
            if (have_capsule_ && dt > 1e-4)
            {
                Vec3 cv = flat(in.capsule - last_capsule_) * (1.0 / dt);
                if (length(cv) > 2000.0) cv = {}; // a teleport or respawn
                capsule_vel_ = lerp(capsule_vel_, cv, smoothing(dt, kVelocityTau));
            }
            last_capsule_ = in.capsule;
            have_capsule_ = true;
            Vec3 v = flat(capsule_vel_);
            double speed = length(v);
            double turn = 0.0;
            if (dt > 1e-4) turn = std::fabs(wrap(root_yaw - last_root_yaw_)) * kRadToDeg / dt;
            last_root_yaw_ = root_yaw;
            turn_rate_ += (turn - turn_rate_) * smoothing(dt, kVelocityTau);
            weight = 1.0 - std::clamp(speed / (p.loco_speed * s) + turn_rate_ / p.loco_turn_dps, 0.0, 1.0);
            if (speed > 1.0) gait_dir_ = normalize_or(lerp(gait_dir_, v * (1.0 / speed), smoothing(dt, 0.1)), gait_dir_);
            if (length(gait_dir_) < 0.5) gait_dir_ = yaw_dir(root_yaw);
            double h = (std::max)(0.0, (hips_now[0].z + hips_now[1].z) * 0.5 - (in.floor_z[0] + in.floor_z[1]) * 0.5 - skel.ankle_height);
            // The leg reaches its length plus what rising onto the toes adds (the trailing foot pushes off).
            double reach = skel.thigh + skel.shin + skel.ball_from_ankle.x * std::sin(p.heel_lift_max_deg * kDegToRad);
            double s_max = 2.0 * std::sqrt((std::max)(0.0, reach * reach - h * h)) * p.gait_stride_share;
            s_max = (std::max)(s_max, 0.3 * (skel.thigh + skel.shin));
            double duty = 0.62 + (0.45 - 0.62) * smoothstep(150.0 * s, 300.0 * s, speed);
            double T = std::clamp(1.0 - speed / (800.0 * s), 0.55, 1.0);
            double S = speed * T * duty;
            if (S > s_max && speed > 1e-3)
            {
                S = s_max;
                T = (std::max)(0.3, S / (duty * speed));
            }
            Vec3 mid = flat((hips_now[0] + hips_now[1]) * 0.5);
            bool use_clips = clips && clips->loaded;
            ClipPose cp;
            double stride = 1.0;
            Quat facing = yaw_rot(root_yaw);
            if (use_clips)
            {
                // B&S: the Unarmed (Moving) state's blend trees by Height and the local velocity; the clip plays
                // at your speed (half of a speed difference in cadence, half in stride, as 17ab5a7 did), so its
                // planted foot stays planted.
                Vec3 local = facing.inverse().rotate(v);
                double clip_len = 1.0, clip_speed = 0.0;
                cp = clips->sample(local, height_ratio, clip_phase_, clip_len, clip_speed);
                double k = clip_speed > 1.0 ? speed / clip_speed : 1.0;
                stride = std::clamp(std::sqrt(k), 0.6, 1.4);
                double cadence = k / stride;
                if (speed > 1.0) clip_phase_ = std::fmod(clip_phase_ + dt * cadence / (std::max)(0.1, clip_len), 1.0);
                cp = clips->sample(local, height_ratio, clip_phase_, clip_len, clip_speed);
                clip_hip_yaw_ = cp.pelvis_yaw;
            }
            else if (speed > 1.0)
                gait_phase_ = std::fmod(gait_phase_ + dt / T, 1.0);
            double lift_h = p.step_height * s * std::clamp(S / (25.0 * s), 0.0, 1.0);
            for (int i = 0; i < 2; ++i)
            {
                if (use_clips)
                {
                    // The clip's foot from its pelvis, stretched along the way you move by the stride share, put
                    // under his hips; its height above the clip's ground is the lift.
                    Vec3 vdir = speed > 1.0 ? facing.inverse().rotate(v * (1.0 / speed)) : Vec3{1, 0, 0};
                    Vec3 rel = cp.foot[i] - cp.pelvis;
                    rel.z = 0.0;
                    rel = rel + vdir * (dot(rel, vdir) * (stride - 1.0));
                    Vec3 foot = mid + facing.rotate(rel);
                    foot.z = in.floor_z[i];
                    draw[i].pos = lerp(foot, feet_[i].pos, weight);
                    draw[i].pos.z = in.floor_z[i];
                    Vec3 bf = facing.rotate(cp.ball[i] - cp.foot[i]);
                    double clip_yaw = length(flat(bf)) > 1.0 ? std::atan2(bf.y, bf.x) : root_yaw;
                    draw[i].yaw = feet_[i].yaw + wrap(clip_yaw - feet_[i].yaw) * (1.0 - weight);
                    double lift = (std::max)(0.0, cp.foot[i].z - skel.ankle_height);
                    lifts[i] = lifts[i] * weight + lift * (1.0 - weight);
                    // The knee bends where the clip's knee does (B&S's legs keep the animation's bend plane).
                    Vec3 kd = cp.calf[i] - (cp.thigh[i] + cp.foot[i]) * 0.5;
                    clip_knee_[i] = facing.rotate(kd);
                    continue;
                }
                double ph = std::fmod(gait_phase_ + 0.5 * i, 1.0), x, lift = 0.0;
                if (ph < duty) x = S * 0.5 - S * (ph / duty);
                else
                {
                    double q = (ph - duty) / (1.0 - duty);
                    x = -S * 0.5 + S * (0.5 - 0.5 * std::cos(kPi * q));
                    lift = lift_h * std::sin(kPi * q);
                }
                Vec3 side_dir = right_of(root_yaw) * (i == 0 ? -1.0 : 1.0);
                Vec3 cyc = mid + gait_dir_ * x + side_dir * (0.5 * p.foot_spacing * s);
                // Strafing, a foot never crosses to the other side of his midline (B&S strafes step out and back in).
                double out_by = dot(cyc - mid, side_dir);
                double min_out = 0.25 * p.foot_spacing * s;
                if (out_by < min_out) cyc = cyc + side_dir * (min_out - out_by);
                cyc.z = in.floor_z[i];
                draw[i].pos = lerp(cyc, feet_[i].pos, weight);
                draw[i].pos.z = in.floor_z[i];
                draw[i].yaw = feet_[i].yaw + wrap(root_yaw - feet_[i].yaw) * (1.0 - weight);
                lifts[i] = lifts[i] * weight + lift * (1.0 - weight);
            }
            // Fully moving, the planted feet are where the cycle has them: standing again starts from there.
            if (weight < 0.02)
                for (int i = 0; i < 2; ++i)
                {
                    feet_[i].pos = draw[i].pos;
                    feet_[i].yaw = draw[i].yaw;
                    feet_[i].stepping = false;
                }
            out.locomotion_weight = weight;
            last_weight_ = weight;
            clip_weight_ = use_clips ? 1.0 - weight : 0.0;
            out.clips = use_clips && weight < 0.98;
        }
        double thigh_len[2] = {length(K.joint[J_CALF_L] - K.joint[J_THIGH_L]), length(K.joint[J_CALF_R] - K.joint[J_THIGH_R])};
        double shin_len[2] = {length(K.joint[J_FOOT_L] - K.joint[J_CALF_L]), length(K.joint[J_FOOT_R] - K.joint[J_CALF_R])};

        auto ankle_at = [&](const DrawFoot& f, double lift, double heel, Vec3* ball_out) {
            Vec3 fw = yaw_dir(f.yaw);
            double ball_h = K.ankle_height + K.ball_from_ankle.z;
            Vec3 ball = f.pos + fw * K.ball_from_ankle.x + kUp * (ball_h + lift);
            Vec3 flat_ankle = f.pos + kUp * (K.ankle_height + lift);
            if (ball_out) *ball_out = ball;
            return ball + quat_axis_angle(right_of(f.yaw), heel).rotate(flat_ankle - ball);
        };
        auto hip_range = [&](int i, const Vec3& hip, double& lo, double& hi) {
            double a = thigh_len[i], b = shin_len[i];
            double fold = p.knee_max_flexion_deg * kDegToRad;
            double closest = std::sqrt(a * a + b * b + 2.0 * a * b * std::cos(fold));
            Vec3 fa = ankle_at(draw[i], lifts[i], 0.0, nullptr);
            double hor = length(flat(hip - fa));
            lo = fa.z + std::sqrt((std::max)(0.0, closest * closest - hor * hor));
            // Flat-footed reach: hips a little too high (your eyes over his) come down a few cm rather than
            // standing him on his toes; only past hips_pull_max_cm do the heels rise.
            Vec3 ra = ankle_at(draw[i], lifts[i], 0.0, nullptr);
            double reach = (a + b) * p.leg_stretch;
            double hor2 = length(flat(hip - ra));
            hi = ra.z + std::sqrt((std::max)(0.0, reach * reach - hor2 * hor2));
            if (hi < lo) hi = lo;
        };
        auto hips_range = [&](const Chain& c, double& lo, double& hi) {
            Vec3 hj[2];
            hip_joints(c, hj);
            lo = -1e30, hi = 1e30;
            for (int i = 0; i < 2; ++i)
            {
                if (in.kick[i]) continue;
                double l, h;
                hip_range(i, hj[i], l, h);
                double off = hj[i].z - c.pos[0].z;
                lo = (std::max)(lo, l - off);
                hi = (std::min)(hi, h - off);
            }
        };

        // ---- hips: never lower than the knees fold (the spine bends instead), never higher than the legs reach ----
        double low, high;
        hips_range(chain, low, high);
        if (chain.pos[0].z < low)
        {
            double lo = pitch_.value, hi = kMaxPitch;
            if (chain_at(hi).pos[0].z >= low)
            {
                for (int it = 0; it < 14; ++it)
                {
                    double mid = 0.5 * (lo + hi);
                    if (chain_at(mid).pos[0].z >= low) hi = mid;
                    else lo = mid;
                }
            }
            // Bend there quickly but not in one frame (a head dropping straight down past the knees' limit
            // asks for a large bend for a small drop): at most p.fold_bend_dps; the hips are squashed up
            // the spine for the frames in between.
            double most = p.fold_bend_dps * kDegToRad * dt;
            double was = pitch_.value;
            double now_pitch = (std::min)(hi, was + most);
            pitch_ = Spring{now_pitch, (now_pitch - was) / (std::max)(1e-4, dt)};
            chain = chain_at(now_pitch);
            double short_by = low - chain.pos[0].z;
            if (short_by > 0.0)
            {
                shift_hips(chain, -short_by);
                out.hips_short_cm = short_by;
            }
        }
        hips_range(chain, low, high);
        double excess = (std::max)(0.0, chain.pos[0].z - high);
        hip_drop_ = excess >= hip_drop_ ? excess : hip_drop_ + (excess - hip_drop_) * smoothing(dt, p.hip_release_tau);
        // At most hips_pull_max_cm: a foot further than that is left short of the floor (it lifts, as a
        // trailing foot does) instead of dragging the body down onto the other knee.
        double drop = (std::min)((std::min)(hip_drop_, p.hips_pull_max_cm * s), (std::max)(0.0, chain.pos[0].z - low));
        if (drop > 0.0)
        {
            shift_hips(chain, drop);
            out.hips_pulled_cm = drop;
        }

        // ---- legs ----
        Vec3 hips_fwd = flat(chain.seg[0].rotate({1, 0, 0}));
        Vec3 hj[2];
        hip_joints(chain, hj);
        Vec3 leg_pos[2][4];
        Quat leg_rot[2][3];
        for (int i = 0; i < 2; ++i)
        {
            const DrawFoot& f = draw[i];
            double a = thigh_len[i], b = shin_len[i];
            int jt = i == 0 ? J_THIGH_L : J_THIGH_R;
            Vec3 ref_thigh = K.joint[jt + 1] - K.joint[jt], ref_shin = K.joint[jt + 2] - K.joint[jt + 1], ref_foot = K.joint[jt + 3] - K.joint[jt + 2];
            Vec3 fw = yaw_dir(f.yaw);
            Vec3 knee_dir = normalize_or(fw + normalize_or(hips_fwd, fw), fw);
            if (clip_weight_ > 0.0 && length(clip_knee_[i]) > 1.0)
                knee_dir = normalize_or(knee_dir * (1.0 - clip_weight_) + normalized(clip_knee_[i]) * clip_weight_, knee_dir);
            Vec3 hip = hj[i], ankle, ball;
            double heel = 0.0;
            double reach = std::sqrt(a * a + b * b + 2.0 * a * b * std::cos(kKneeMinFlex));
            if (in.kick[i])
            {
                Vec3 way = normalize_or(in.kick_ankle[i] - hip, {0, 0, -1});
                ankle = hip + way * (std::min)(length(in.kick_ankle[i] - hip), a + b);
                ball = ankle + knee_dir * length(K.ball_from_ankle);
            }
            else
            {
                if (length(ankle_at(f, lifts[i], 0.0, nullptr) - hip) > reach)
                {
                    double lo = 0.0, hi = p.heel_lift_max_deg * kDegToRad;
                    for (int it = 0; it < 12; ++it)
                    {
                        double mid = 0.5 * (lo + hi);
                        if (length(ankle_at(f, lifts[i], mid, nullptr) - hip) > reach) lo = mid;
                        else hi = mid;
                    }
                    heel = hi;
                }
                ankle = ankle_at(f, lifts[i], heel, &ball);
            }
            // Two-bone: the knee in the plane of the leg and knee_dir; eased into straight; never past the fold limit.
            double d = length(ankle - hip);
            double fold = p.knee_max_flexion_deg * kDegToRad;
            double closest = std::sqrt(a * a + b * b + 2.0 * a * b * std::cos(fold));
            double deff = std::clamp(eased_reach(d, reach, kLegEase * reach), closest, reach);
            double k = d > deff && deff > 1e-6 ? d / deff : 1.0; // stretch the last bit (feet stay planted)
            if (k > p.leg_stretch)
            {
                // Out of reach even stretched: the foot comes off the floor toward the hip rather than the leg
                // growing (the hips were only pulled down hips_pull_max_cm for it).
                k = p.leg_stretch;
                Vec3 ball_off = ball - ankle;
                ankle = hip + normalize_or(ankle - hip, {0, 0, -1}) * (deff * k);
                ball = ankle + ball_off;
                d = deff * k;
            }
            Vec3 u = normalize_or(ankle - hip, {0, 0, -1});
            Vec3 n = knee_dir - u * dot(knee_dir, u);
            if (length(n) < 1e-4) n = hips_fwd - u * dot(hips_fwd, u);
            n = normalize_or(n, {1, 0, 0});
            double ca = std::clamp((a * a + deff * deff - b * b) / (2.0 * a * deff), -1.0, 1.0);
            double alpha = std::acos(ca);
            Vec3 knee = hip + (u * std::cos(alpha) + n * std::sin(alpha)) * (a * k);
            if (d < closest) ankle = hip + u * closest; // (cannot happen after the hips clamp; kept safe)
            double flex = kPi - std::acos(std::clamp(dot(normalize_or(hip - knee, u), normalize_or(ankle - knee, u)), -1.0, 1.0));
            out.knee_deg[i] = flex * kRadToDeg;
            out.heel_deg[i] = heel * kRadToDeg;
            out.lift_cm[i] = lifts[i];
            out.stepping[i] = feet_[i].stepping || (weight < 0.98 && lifts[i] > 0.05);
            Vec3 hinge = normalize_or(cross(n, u), right_of(f.yaw));
            Vec3 along_t = normalize_or(knee - hip, u), along_s = normalize_or(ankle - knee, u);
            leg_rot[i][0] = frame_turn(along_t, cross(along_t, hinge), ref_thigh, {1, 0, 0});
            leg_rot[i][1] = frame_turn(along_s, cross(along_s, hinge), ref_shin, {1, 0, 0});
            Vec3 foot_up = quat_axis_angle(right_of(f.yaw), heel).rotate(kUp);
            leg_rot[i][2] = frame_turn(normalize_or(ball - ankle, fw), foot_up, ref_foot, kUp);
            leg_pos[i][0] = hip, leg_pos[i][1] = knee, leg_pos[i][2] = ankle, leg_pos[i][3] = ball;
        }

        // ---- out ----
        for (int i = 0; i < 5; ++i)
        {
            out.pos[i] = chain.pos[i];
            Vec3 along = normalize_or(chain.pos[i + 1] - chain.pos[i], chain.seg[i].rotate(normalized(ref_seg[i])));
            out.rot[i] = frame_turn(along, chain.seg[i].rotate({0, 1, 0}), ref_seg[i], {0, 1, 0});
        }
        {
            // The neck's right: between the chest's and the head's.
            Vec3 chest_right = chain.seg[4].rotate({0, 1, 0});
            Vec3 neck_right = normalize_or(chest_right * (1.0 - kNeckFollow) + head_rot.rotate({0, 1, 0}) * kNeckFollow, chest_right);
            for (int k = 0; k < 2; ++k)
            {
                out.pos[J_NECK_01 + k] = chain.pos[5 + k];
                out.rot[J_NECK_01 + k] = frame_turn(chain.neck_dir[k], neck_right, ref_neck[k], {0, 1, 0});
            }
        }
        out.pos[J_HEAD] = head_joint;
        out.rot[J_HEAD] = head_rot;
        for (int i = 0; i < 2; ++i)
        {
            int jt = i == 0 ? J_THIGH_L : J_THIGH_R;
            for (int k = 0; k < 4; ++k) out.pos[jt + k] = leg_pos[i][k];
            for (int k = 0; k < 3; ++k) out.rot[jt + k] = leg_rot[i][k];
            out.rot[jt + 3] = leg_rot[i][2];
        }
        out.pitch_deg = pitch_.value * kRadToDeg;
        out.torso_yaw_deg = torso_yaw * kRadToDeg;
        out.walking = weight < 0.5;
        out.steps = steps_;
        out.valid = true;
        have_hips_ = true;
        last_hips_ = chain.pos[0];
        return out;
    }
} // namespace hsvr::body
