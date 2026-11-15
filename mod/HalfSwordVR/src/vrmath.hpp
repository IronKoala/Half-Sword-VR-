// Small double-precision math in Unreal's conventions (X forward, Y right, Z up, centimetres,
// left-handed), plus conversion from OpenXR (X right, Y up, -Z forward, metres, right-handed).
#pragma once
#include <cmath>

namespace hsvr
{
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kRadToDeg = 180.0 / kPi;
    constexpr double kDegToRad = kPi / 180.0;

    struct Vec3
    {
        double x = 0, y = 0, z = 0;
        Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
        Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
        Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
    };

    inline Vec3 cross(const Vec3& a, const Vec3& b)
    {
        return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    }

    struct Quat
    {
        double x = 0, y = 0, z = 0, w = 1;

        Quat operator*(const Quat& q) const
        {
            // Same composition as Unreal's FQuat::operator*: (A * B) rotates by B first, then A.
            return {w * q.x + x * q.w + y * q.z - z * q.y,
                    w * q.y - x * q.z + y * q.w + z * q.x,
                    w * q.z + x * q.y - y * q.x + z * q.w,
                    w * q.w - x * q.x - y * q.y - z * q.z};
        }
        Quat inverse() const { return {-x, -y, -z, w}; }
        Vec3 rotate(const Vec3& v) const
        {
            Vec3 q{x, y, z};
            Vec3 t = cross(q, v) * 2.0;
            return v + t * w + cross(q, t);
        }
        Quat normalized() const
        {
            double n = std::sqrt(x * x + y * y + z * z + w * w);
            if (n < 1e-12) return {};
            return {x / n, y / n, z / n, w / n};
        }
    };

    // Rotation about Unreal's up axis. Positive yaw turns right (clockwise seen from above).
    inline Quat quat_from_yaw_deg(double yaw_deg)
    {
        double h = yaw_deg * kDegToRad * 0.5;
        return {0, 0, std::sin(h), std::cos(h)};
    }

    inline double normalize_axis(double a)
    {
        a = std::fmod(a, 360.0);
        if (a > 180.0) a -= 360.0;
        if (a < -180.0) a += 360.0;
        return a;
    }

    struct Rot
    {
        double pitch = 0, yaw = 0, roll = 0; // degrees, Unreal FRotator order
    };

    // Same maths as Unreal's FQuat::Rotator().
    inline Rot quat_to_rot(const Quat& q)
    {
        const double singularity = q.z * q.x - q.w * q.y;
        const double yaw_y = 2.0 * (q.w * q.z + q.x * q.y);
        const double yaw_x = 1.0 - 2.0 * (q.y * q.y + q.z * q.z);
        const double threshold = 0.4999995;
        Rot r;
        if (singularity < -threshold)
        {
            r.pitch = -90.0;
            r.yaw = std::atan2(yaw_y, yaw_x) * kRadToDeg;
            r.roll = normalize_axis(-r.yaw - (2.0 * std::atan2(q.x, q.w) * kRadToDeg));
        }
        else if (singularity > threshold)
        {
            r.pitch = 90.0;
            r.yaw = std::atan2(yaw_y, yaw_x) * kRadToDeg;
            r.roll = normalize_axis(r.yaw - (2.0 * std::atan2(q.x, q.w) * kRadToDeg));
        }
        else
        {
            r.pitch = std::asin(2.0 * singularity) * kRadToDeg;
            r.yaw = std::atan2(yaw_y, yaw_x) * kRadToDeg;
            r.roll = std::atan2(-2.0 * (q.w * q.x + q.y * q.z), 1.0 - 2.0 * (q.x * q.x + q.y * q.y)) * kRadToDeg;
        }
        return r;
    }

    // Heading of a rotation in degrees (where its forward axis points on the ground plane).
    inline double quat_yaw_deg(const Quat& q)
    {
        Vec3 f = q.rotate({1, 0, 0});
        return std::atan2(f.y, f.x) * kRadToDeg;
    }

    // OpenXR -> Unreal, the same mapping Unreal's own OpenXR plugin uses.
    inline Vec3 xr_to_ue_position(float x, float y, float z, double cm_per_metre)
    {
        return {-double(z) * cm_per_metre, double(x) * cm_per_metre, double(y) * cm_per_metre};
    }
    inline Quat xr_to_ue_rotation(float x, float y, float z, float w)
    {
        return {-double(z), double(x), double(y), -double(w)};
    }
} // namespace hsvr
