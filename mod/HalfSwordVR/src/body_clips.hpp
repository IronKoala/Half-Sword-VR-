// Blade & Sorcery's locomotion animations for the body solver's moving legs (Matthew 2026-11-24: "just copy
// Blade and Sorcery's animation"). B&S plays its 'Human' controller's "Unarmed (Moving)" state: a 1D blend by
// Height (head height / standing head height; thresholds 0.2 crawl, 0.4 crouch walk, 0.9 walk/run) of
// directional 2D blend trees over walk/run/strafe clips by the local velocity (Strafe, Speed). The clips are
// baked onto Willie with Unity's humanoid retargeting (tools/bs-crouch; B&S-derived files, own use only, read
// from the mod's bs-crouch folder). Here they give the feet (and the knees' bend plane and the hips' turn)
// relative to the clip's root; the solver plants them on the floor and bends the legs to them.
//
// Units: centimetres, the body's frame (X forward, Y right, Z up), relative to the clip's root on the ground.
#pragma once
#include <string>
#include <vector>

#include "vrmath.hpp"

namespace hsvr::body
{
    struct ClipPose
    {
        Vec3 foot[2], ball[2], calf[2], thigh[2], pelvis;
        double pelvis_yaw = 0.0; // radians, right +
    };

    struct Clip
    {
        std::string name;
        double length = 1.0;   // seconds
        Vec3 velocity;         // the root's mean velocity, body frame, cm/s
        std::vector<ClipPose> samples;
        bool load(const std::string& path, std::string& why);
        ClipPose at(double u) const; // u = normalized time 0..1
    };

    struct BlendTree
    {
        std::vector<Clip> clips;
        // Gradient-band weights of the clips for a velocity (body frame, cm/s), as Unity's 2D blend trees.
        void weights(const Vec3& v, std::vector<double>& w) const;
    };

    struct LocomotionClips
    {
        bool loaded = false;
        BlendTree crouch, stand; // B&S Height 0.4 and 0.9
        std::string report;      // what was read, for the log
        bool load(const std::string& folder);
        // The blended pose for a velocity, a head height ratio and a normalized time; also the blended clip's
        // length and speed (for the phase rate).
        ClipPose sample(const Vec3& v, double height_ratio, double u, double& length, double& speed) const;
    };
} // namespace hsvr::body
