// B&S locomotion clips for the body solver: see body_clips.hpp.
#include "body_clips.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace hsvr::body
{
    namespace
    {
        // His component space (x his left, y forward, z up) -> the body's frame (X forward, Y right, Z up).
        Vec3 from_cs(double x, double y, double z) { return {y, -x, z}; }

        // Blade & Sorcery's "Unarmed (Moving)" state (decompiled Human controller, tools/bs-crouch): the crouch
        // walk tree (Height 0.4) and the walk/run tree (Height 0.9). Points come from each clip's own root speed.
        const char* const kCrouch[] = {"Crouch_WalkFwd_new", "Crouch_WalkLt_new", "Crouch_WalkRt_new", "Crouch_WalkBwd_new",
                                       "Crouch_WalkLt45_new", "Crouch_WalkLt135_new", "Crouch_WalkRt45_new", "Crouch_WalkRt135_new"};
        const char* const kStand[] = {"RunFwdLoop", "WalkFwdLoop", "WalkBwdLoop", "StrafeRightLoop", "StrafeLeftLoop", "RunLtLoop",
                                      "RunRtLoop", "RunBwdLoop", "RunStrafeLeft45Loop", "RunStrafeLeft135Loop", "RunStrafeRight45Loop",
                                      "RunStrafeRight135Loop", "SprintFwdLoop", "StrafeRight135Loop", "StrafeLeft45Loop",
                                      "StrafeLeft135Loop", "StrafeRight45Loop"};
        const char* const kBones[10] = {"pelvis", "spine_01", "thigh_l", "calf_l", "foot_l", "ball_l", "thigh_r", "calf_r", "foot_r", "ball_r"};

        ClipPose mix(const ClipPose& a, const ClipPose& b, double t)
        {
            ClipPose o;
            auto l = [t](const Vec3& x, const Vec3& y) { return x + (y - x) * t; };
            for (int s = 0; s < 2; ++s)
            {
                o.foot[s] = l(a.foot[s], b.foot[s]);
                o.ball[s] = l(a.ball[s], b.ball[s]);
                o.calf[s] = l(a.calf[s], b.calf[s]);
                o.thigh[s] = l(a.thigh[s], b.thigh[s]);
            }
            o.pelvis = l(a.pelvis, b.pelvis);
            double d = b.pelvis_yaw - a.pelvis_yaw;
            while (d > kPi) d -= 2.0 * kPi;
            while (d < -kPi) d += 2.0 * kPi;
            o.pelvis_yaw = a.pelvis_yaw + d * t;
            return o;
        }

        void accumulate(ClipPose& acc, const ClipPose& p, double w)
        {
            for (int s = 0; s < 2; ++s)
            {
                acc.foot[s] = acc.foot[s] + p.foot[s] * w;
                acc.ball[s] = acc.ball[s] + p.ball[s] * w;
                acc.calf[s] = acc.calf[s] + p.calf[s] * w;
                acc.thigh[s] = acc.thigh[s] + p.thigh[s] * w;
            }
            acc.pelvis = acc.pelvis + p.pelvis * w;
            acc.pelvis_yaw += p.pelvis_yaw * w; // (the clips' hip turns are all within +-90 deg of each other)
        }
    } // namespace

    bool Clip::load(const std::string& path, std::string& why)
    {
        std::ifstream f(path);
        if (!f)
        {
            why = "missing";
            return false;
        }
        std::string line;
        std::getline(f, line);
        std::vector<std::string> head;
        {
            std::stringstream ss(line);
            std::string c;
            while (std::getline(ss, c, ',')) head.push_back(c);
        }
        auto col = [&](const std::string& n) {
            for (size_t i = 0; i < head.size(); ++i)
                if (head[i] == n) return (int)i;
            return -1;
        };
        int it = col("t"), irx = col("root_x"), iry = col("root_y");
        int ib[10];
        for (int k = 0; k < 10; ++k)
        {
            ib[k] = col(std::string(kBones[k]) + "_x");
            if (ib[k] < 0)
            {
                why = std::string("no ") + kBones[k];
                return false;
            }
        }
        if (it < 0 || irx < 0 || iry < 0)
        {
            why = "no root";
            return false;
        }
        double t0 = 0, t1 = 0, rx0 = 0, ry0 = 0, rx1 = 0, ry1 = 0;
        bool first = true;
        while (std::getline(f, line))
        {
            std::vector<double> v;
            v.reserve(head.size());
            const char* p = line.c_str();
            while (*p)
            {
                char* e = nullptr;
                double d = std::strtod(p, &e);
                if (e == p) break;
                v.push_back(d);
                p = e;
                if (*p == ',') ++p;
            }
            if (v.size() < head.size()) continue;
            auto at = [&](int i) { return from_cs(v[i], v[i + 1], v[i + 2]); };
            ClipPose s;
            s.pelvis = at(ib[0]);
            for (int side = 0; side < 2; ++side)
            {
                s.thigh[side] = at(ib[2 + side * 4]);
                s.calf[side] = at(ib[3 + side * 4]);
                s.foot[side] = at(ib[4 + side * 4]);
                s.ball[side] = at(ib[5 + side * 4]);
            }
            Quat q{v[ib[0] + 3], v[ib[0] + 4], v[ib[0] + 5], v[ib[0] + 6]};
            Vec3 fwd_cs = q.rotate({0, 1, 0});
            Vec3 fwd = from_cs(fwd_cs.x, fwd_cs.y, fwd_cs.z);
            s.pelvis_yaw = std::atan2(fwd.y, fwd.x);
            samples.push_back(s);
            if (first) t0 = v[it], rx0 = v[irx], ry0 = v[iry], first = false;
            t1 = v[it], rx1 = v[irx], ry1 = v[iry];
        }
        if (samples.size() < 4 || t1 <= t0)
        {
            why = "too short";
            return false;
        }
        length = t1 - t0;
        velocity = from_cs((rx1 - rx0) / length, (ry1 - ry0) / length, 0.0);
        return true;
    }

    ClipPose Clip::at(double u) const
    {
        u = u - std::floor(u);
        double x = u * (double)(samples.size() - 1);
        size_t i = (size_t)x;
        if (i + 1 >= samples.size()) return samples.back();
        return mix(samples[i], samples[i + 1], x - (double)i);
    }

    void BlendTree::weights(const Vec3& v, std::vector<double>& w) const
    {
        // Gradient band interpolation (Johansen 2009, Unity's freeform 2D blend): each clip's weight is the
        // smallest of its influences against every other clip, then normalized.
        size_t n = clips.size();
        w.assign(n, 0.0);
        double sum = 0.0;
        for (size_t i = 0; i < n; ++i)
        {
            Vec3 pi = clips[i].velocity;
            double wi = 1.0;
            for (size_t j = 0; j < n && wi > 0.0; ++j)
            {
                if (j == i) continue;
                Vec3 pij = clips[j].velocity - pi;
                double d2 = pij.x * pij.x + pij.y * pij.y;
                if (d2 < 1e-6) continue;
                double h = 1.0 - ((v.x - pi.x) * pij.x + (v.y - pi.y) * pij.y) / d2;
                wi = (std::min)(wi, (std::max)(0.0, h));
            }
            w[i] = wi;
            sum += wi;
        }
        if (sum <= 1e-9)
        {
            // Outside every band (faster than any clip): the clip nearest in direction.
            size_t best = 0;
            double bd = -2.0, vl = std::sqrt(v.x * v.x + v.y * v.y);
            for (size_t i = 0; i < n; ++i)
            {
                double cl = std::sqrt(clips[i].velocity.x * clips[i].velocity.x + clips[i].velocity.y * clips[i].velocity.y);
                double d = vl > 1e-6 && cl > 1e-6 ? (v.x * clips[i].velocity.x + v.y * clips[i].velocity.y) / (vl * cl) : 0.0;
                if (d > bd) bd = d, best = i;
            }
            w[best] = 1.0;
            return;
        }
        for (double& x : w) x /= sum;
    }

    bool LocomotionClips::load(const std::string& folder)
    {
        loaded = false;
        report.clear();
        crouch.clips.clear();
        stand.clips.clear();
        std::string missing;
        for (const char* n : kCrouch)
        {
            Clip c;
            std::string why;
            if (c.load(folder + n + ".csv", why)) c.name = n, crouch.clips.push_back(c);
            else missing += std::string(" ") + n + " (" + why + ")";
        }
        for (const char* n : kStand)
        {
            Clip c;
            std::string why;
            if (c.load(folder + n + ".csv", why)) c.name = n, stand.clips.push_back(c);
            else missing += std::string(" ") + n + " (" + why + ")";
        }
        loaded = crouch.clips.size() >= 4 && stand.clips.size() >= 4;
        report = std::to_string(crouch.clips.size()) + " crouch and " + std::to_string(stand.clips.size()) + " walk/run clips read from " + folder +
                 (missing.empty() ? std::string() : ";" + std::string(" missing:") + missing);
        return loaded;
    }

    ClipPose LocomotionClips::sample(const Vec3& v, double height_ratio, double u, double& length, double& speed) const
    {
        // B&S's 1D Height blend: 0.4 crouch walk .. 0.9 walk/run (below 0.4 stays the crouch walk: no crawling here).
        double t = std::clamp((height_ratio - 0.4) / 0.5, 0.0, 1.0);
        ClipPose out;
        length = 0.0;
        Vec3 vel;
        const BlendTree* trees[2] = {&crouch, &stand};
        double tw[2] = {1.0 - t, t};
        std::vector<double> w;
        for (int k = 0; k < 2; ++k)
        {
            if (tw[k] <= 1e-6) continue;
            trees[k]->weights(v, w);
            for (size_t i = 0; i < w.size(); ++i)
            {
                double ww = w[i] * tw[k];
                if (ww <= 1e-6) continue;
                const Clip& c = trees[k]->clips[i];
                accumulate(out, c.at(u), ww);
                length += c.length * ww;
                vel = vel + c.velocity * ww;
            }
        }
        speed = std::sqrt(vel.x * vel.x + vel.y * vel.y);
        return out;
    }
} // namespace hsvr::body
