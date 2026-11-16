// Motion controls: the controllers move the character's arms. See arm_control.hpp for the idea.
//
// Order of events in one frame (game thread):
//  1. arm_game_tick (before the world ticks): read both controllers, put them into the game
//     world the way the eyes are put there, and feed the game's aim from the sword hand.
//  2. The driver skeleton animates as usual (ABP_Willie).
//  3. Just before each of the character's other skeletal meshes runs its update (where
//     CharacterMesh0's CopyPoseFromMesh reads the driver's bone transforms), our hook on that
//     mesh's tick function bends the driver's arm bones: two-bone IK from the shoulder to the
//     controller, elbow on its own hinge, hand turned like the controller.
//  4. The body copies the pose and its muscles pull the physical arm and sword towards it.
//
// Nothing here is engine source. The two engine layouts we rely on (a tick function's small
// virtual table, and the skinned mesh's pair of bone transform arrays) are found and checked at
// run time; if a check fails motion arms switch off and the game runs as before.

#include "arm_control.hpp"

#include "config.hpp"
#include "log.hpp"
#include "stereo_device.hpp"
#include "ue_abi.hpp"
#include "vr_present.hpp"
#include "vr_state.hpp"
#include "vrmath.hpp"

#include <Unreal/FProperty.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/Property/FStructProperty.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UScriptStruct.hpp>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <map>
#include <mutex>
#include <string>
#include <vector>

using namespace RC;
using namespace RC::Unreal;

namespace hsvr
{
    namespace
    {
        // ---- small maths -----------------------------------------------------------------------
        double dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
        double length(const Vec3& v) { return std::sqrt(dot(v, v)); }
        Vec3 normalized(const Vec3& v, const Vec3& fallback = {1, 0, 0})
        {
            double n = length(v);
            return n > 1e-9 ? v * (1.0 / n) : fallback;
        }
        Vec3 perpendicular_part(const Vec3& v, const Vec3& axis_unit) { return v - axis_unit * dot(v, axis_unit); }

        Quat quat_axis_angle(const Vec3& axis_unit, double radians)
        {
            double s = std::sin(radians * 0.5);
            return {axis_unit.x * s, axis_unit.y * s, axis_unit.z * s, std::cos(radians * 0.5)};
        }

        // Rotation whose local X goes to `x` and local Y (made square to x) goes to `y`.
        Quat quat_from_frame(const Vec3& x_in, const Vec3& y_in)
        {
            Vec3 x = normalized(x_in);
            Vec3 y = normalized(perpendicular_part(y_in, x), std::fabs(x.z) < 0.9 ? normalized(cross({0, 0, 1}, x)) : Vec3{0, 1, 0});
            Vec3 z = cross(x, y);
            // Columns x, y, z.
            double m00 = x.x, m01 = y.x, m02 = z.x;
            double m10 = x.y, m11 = y.y, m12 = z.y;
            double m20 = x.z, m21 = y.z, m22 = z.z;
            double trace = m00 + m11 + m22;
            Quat q;
            if (trace > 0.0)
            {
                double s = 0.5 / std::sqrt(trace + 1.0);
                q = {(m21 - m12) * s, (m02 - m20) * s, (m10 - m01) * s, 0.25 / s};
            }
            else if (m00 > m11 && m00 > m22)
            {
                double s = 2.0 * std::sqrt(1.0 + m00 - m11 - m22);
                q = {0.25 * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
            }
            else if (m11 > m22)
            {
                double s = 2.0 * std::sqrt(1.0 + m11 - m00 - m22);
                q = {(m01 + m10) / s, 0.25 * s, (m12 + m21) / s, (m02 - m20) / s};
            }
            else
            {
                double s = 2.0 * std::sqrt(1.0 + m22 - m00 - m11);
                q = {(m02 + m20) / s, (m12 + m21) / s, 0.25 * s, (m10 - m01) / s};
            }
            return q.normalized();
        }

        Quat quat_from_rot_deg(double pitch, double yaw, double roll)
        {
            // Same order as Unreal's FRotator::Quaternion(): yaw, then pitch, then roll (intrinsic).
            Quat qy = quat_axis_angle({0, 0, 1}, yaw * kDegToRad);
            Quat qp = quat_axis_angle({0, 1, 0}, -pitch * kDegToRad);
            Quat qr = quat_axis_angle({1, 0, 0}, -roll * kDegToRad);
            return (qy * qp * qr).normalized();
        }

        double quat_angle_deg(const Quat& a, const Quat& b)
        {
            double d = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
            return 2.0 * std::acos(std::min(1.0, d)) * kRadToDeg;
        }

        // A bone or component transform. The character's bones are not scaled, but the character
        // as a whole is (the game sizes a short or tall Willie with the actor's scale): `s` is the
        // size of a frame that things are placed in. It is 1 for everything except the body
        // mesh's place in the world and the frames tagged with the body's size on purpose; what
        // compose and relative return always has s = 1. (Until build 13 the size was left out, and
        // a short Willie's head sat below the view, a tall one's above it.)
        struct Xf
        {
            Quat q;
            Vec3 t;
            double s = 1.0;
        };
        // `local` placed in `parent`'s frame (Unreal's local * parent).
        Xf compose(const Xf& local, const Xf& parent) { return {(parent.q * local.q).normalized(), parent.q.rotate(local.t * parent.s) + parent.t}; }
        // `child` expressed in `parent`'s frame.
        Xf relative(const Xf& child, const Xf& parent)
        {
            Quat inv = parent.q.inverse();
            return {(inv * child.q).normalized(), inv.rotate(child.t - parent.t) * (1.0 / parent.s)};
        }

        // The engine's FTransform (double precision, vectorised): rotation x y z w, translation
        // x y z (+pad), scale x y z (+pad). 96 bytes; checked against a function's parameter size.
        struct alignas(16) RawXf
        {
            double q[4];
            double t[4];
            double s[4];
        };
        static_assert(sizeof(RawXf) == 96, "FTransform is 96 bytes in UE 5.4");
        Xf from_raw(const RawXf& r) { return {Quat{r.q[0], r.q[1], r.q[2], r.q[3]}.normalized(), Vec3{r.t[0], r.t[1], r.t[2]}}; }
        void to_raw(const Xf& x, RawXf& r)
        {
            r.q[0] = x.q.x;
            r.q[1] = x.q.y;
            r.q[2] = x.q.z;
            r.q[3] = x.q.w;
            r.t[0] = x.t.x;
            r.t[1] = x.t.y;
            r.t[2] = x.t.z;
        }

        // ---- reflection helpers (game thread) --------------------------------------------------
        UObject* object_property(UObject* owner, const TCHAR* name)
        {
            if (!owner) return nullptr;
            UObject** value = owner->GetValuePtrByPropertyNameInChain<UObject*>(name);
            return value ? *value : nullptr;
        }

        FName make_name(const wchar_t* text) { return FName(text, FNAME_Add); }

        struct alignas(16) SocketXfParams
        {
            FName InSocketName;
            uint8_t TransformSpace;
            uint8_t pad[7];
            RawXf ReturnValue;
        };
        static_assert(offsetof(SocketXfParams, ReturnValue) == 16, "layout");
        enum : uint8_t
        {
            RTS_World = 0,
            RTS_Component = 2
        };

        struct alignas(16) XfReturnParams
        {
            RawXf ReturnValue;
        };
        struct VectorReturnParams
        {
            ue::FVector ReturnValue;
        };
        struct RotatorReturnParams
        {
            ue::FRotator ReturnValue;
        };
        struct NameIntParams
        {
            FName BoneName;
            int32_t ReturnValue;
        };
        struct NameNameParams
        {
            FName BoneName;
            FName ReturnValue;
        };
        struct IntNameParams
        {
            int32_t BoneIndex;
            FName ReturnValue;
        };
        struct IntReturnParams
        {
            int32_t ReturnValue;
        };

        bool socket_transform(UObject* mesh, FName bone, uint8_t space, Xf& out)
        {
            if (!mesh) return false;
            UFunction* fn = mesh->GetFunctionByNameInChain(STR("GetSocketTransform"));
            if (!fn) return false;
            static bool size_checked = false, size_ok = false;
            if (!size_checked)
            {
                size_checked = true;
                size_ok = fn->GetParmsSize() == (int32_t)sizeof(SocketXfParams);
                if (!size_ok) HSVR_LOG("Arms: GetSocketTransform takes %d bytes, expected %d; motion arms stay off", fn->GetParmsSize(), (int)sizeof(SocketXfParams));
            }
            if (!size_ok) return false;
            SocketXfParams p{};
            p.InSocketName = bone;
            p.TransformSpace = space;
            mesh->ProcessEvent(fn, &p);
            out = from_raw(p.ReturnValue);
            return true;
        }

        bool component_location(UObject* component, Vec3& out)
        {
            if (!component) return false;
            UFunction* fn = component->GetFunctionByNameInChain(STR("K2_GetComponentLocation"));
            if (!fn) return false;
            VectorReturnParams p{};
            component->ProcessEvent(fn, &p);
            out = {p.ReturnValue.X, p.ReturnValue.Y, p.ReturnValue.Z};
            return true;
        }

        bool actor_location(UObject* actor, Vec3& out)
        {
            if (!actor) return false;
            UFunction* fn = actor->GetFunctionByNameInChain(STR("K2_GetActorLocation"));
            if (!fn) return false;
            VectorReturnParams p{};
            actor->ProcessEvent(fn, &p);
            out = {p.ReturnValue.X, p.ReturnValue.Y, p.ReturnValue.Z};
            return true;
        }

        bool actor_yaw(UObject* actor, double& yaw)
        {
            if (!actor) return false;
            UFunction* fn = actor->GetFunctionByNameInChain(STR("K2_GetActorRotation"));
            if (!fn) return false;
            RotatorReturnParams p{};
            actor->ProcessEvent(fn, &p);
            yaw = p.ReturnValue.Yaw;
            return true;
        }

        bool component_world(UObject* component, Xf& out)
        {
            if (!component) return false;
            UFunction* fn = component->GetFunctionByNameInChain(STR("K2_GetComponentToWorld"));
            if (!fn || fn->GetParmsSize() != (int32_t)sizeof(XfReturnParams)) return false;
            XfReturnParams p{};
            component->ProcessEvent(fn, &p);
            out = from_raw(p.ReturnValue);
            double size = p.ReturnValue.s[2];
            out.s = size > 0.3 && size < 3.0 ? size : 1.0;
            return true;
        }

        int32_t call_int(UObject* obj, const TCHAR* fn_name)
        {
            UFunction* fn = obj ? obj->GetFunctionByNameInChain(fn_name) : nullptr;
            if (!fn) return -1;
            IntReturnParams p{};
            obj->ProcessEvent(fn, &p);
            return p.ReturnValue;
        }

        // Reads memory that may not be there (used only while looking for engine layouts).
        bool safe_copy(void* dst, const void* src, size_t size)
        {
            __try
            {
                memcpy(dst, src, size);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // ---- the driver skeleton's bone transforms ---------------------------------------------
        // USkinnedMeshComponent keeps `TArray<FTransform> ComponentSpaceTransformsArray[2]`, then
        // two more arrays, a float, `int32 CurrentEditableComponentTransforms` and
        // `int32 CurrentReadComponentTransforms` (UE 5.4.4 SkinnedMeshComponent.h). None is
        // reflected, so the pair of arrays is found by its shape and checked against the bone
        // transforms the engine itself reports.
        struct RawArray
        {
            RawXf* data;
            int32_t num;
            int32_t max;
        };
        constexpr size_t kReadIndexAfterArrays = 72;

        struct DriverLayout
        {
            bool searched = false;
            size_t arrays_offset = 0; // 0 = not found
        };
        DriverLayout g_layout;

        bool transforms_close(const Xf& a, const Xf& b)
        {
            return length(a.t - b.t) < 0.05 && quat_angle_deg(a.q, b.q) < 0.5;
        }

        bool find_driver_layout(UObject* driver, int32_t bone_count, const std::vector<FName>& check_bones, const std::vector<int32_t>& check_index)
        {
            auto* base = reinterpret_cast<uint8_t*>(driver);
            UClass* cls = driver->GetClassPrivate();
            int32_t object_size = cls ? cls->GetPropertiesSize() : 0x1000;
            if (object_size <= 0 || object_size > 0x10000) object_size = 0x1000;
            std::vector<Xf> reported;
            for (size_t i = 0; i < check_bones.size(); ++i)
            {
                Xf x;
                if (!socket_transform(driver, check_bones[i], RTS_Component, x)) return false;
                reported.push_back(x);
            }
            for (size_t off = 0x200; off + kReadIndexAfterArrays + 8 <= (size_t)object_size; off += 8)
            {
                RawArray arrays[2];
                if (!safe_copy(arrays, base + off, sizeof(arrays))) continue;
                // The first array always holds the bones; the second too when the mesh keeps two
                // copies (the default), else it is empty.
                if (arrays[0].num != bone_count || arrays[0].max < bone_count || !arrays[0].data) continue;
                bool second_full = arrays[1].num == bone_count && arrays[1].max >= bone_count && arrays[1].data && arrays[1].data != arrays[0].data;
                bool second_empty = arrays[1].num == 0 && arrays[1].max >= 0;
                if (!second_full && !second_empty) continue;
                int32_t indices[2];
                if (!safe_copy(indices, base + off + kReadIndexAfterArrays - 4, sizeof(indices))) continue;
                int32_t read = indices[1];
                if (read != 0 && read != 1) continue;
                if (read == 1 && !second_full) continue;
                bool all_match = true;
                for (size_t i = 0; i < check_bones.size() && all_match; ++i)
                {
                    RawXf raw;
                    if (!safe_copy(&raw, arrays[read].data + check_index[i], sizeof(raw))) all_match = false;
                    else if (!transforms_close(from_raw(raw), reported[i])) all_match = false;
                }
                if (!all_match) continue;
                g_layout.arrays_offset = off;
                HSVR_LOG("Arms: driver skeleton bone transforms found at offset 0x%zX (read buffer %d, editable %d, %d bones)", off, read, indices[0], bone_count);
                return true;
            }
            return false;
        }

        RawXf* driver_read_buffer(UObject* driver, int32_t bone_count)
        {
            if (!g_layout.arrays_offset) return nullptr;
            auto* base = reinterpret_cast<uint8_t*>(driver) + g_layout.arrays_offset;
            auto* arrays = reinterpret_cast<RawArray*>(base);
            int32_t read = *reinterpret_cast<int32_t*>(base + kReadIndexAfterArrays);
            if (read != 0 && read != 1) return nullptr;
            if (arrays[read].num != bone_count || !arrays[read].data) return nullptr;
            return arrays[read].data;
        }

        // ---- per-side state ----------------------------------------------------------------------
        struct Side
        {
            const wchar_t* suffix;
            const char* label;
            double out_sign; // +1 right, -1 left
            int controller_index;
            // Driver bone indices.
            int32_t clavicle = -1, upper = -1, lower = -1, hand = -1, parent = -1;
            int32_t thigh = -1, calf = -1, foot = -1; // the leg on this side (-1 = not usable)
            // The mod's own stepping (own_legs). Learned from the standing pose: where the foot
            // bone rests relative to the character (its position and heading), and how it is turned.
            bool have_foot_home = false;
            Vec3 foot_home;
            Quat foot_rest_q;
            bool foot_planted = false, stepping = false;
            bool gait_step = false; // this step belongs to the walking rhythm (timed by it)
            Vec3 step_to;           // where the current step lands
            Vec3 foot_at, step_from; // world
            double foot_yaw = 0, step_from_yaw = 0, step_t = 0, step_duration = 0.2;
            // The game's grip joint for what this hand holds (rigid_grip): the component it was last
            // locked on, and its settings as the game made them, by engine function name.
            UObject* grip_component = nullptr;
            bool grip_locked = false;
            int grip_relocked = 0;
            std::map<std::wstring, std::vector<uint8_t>> grip_saved;
            // The wrist joint as the game made it, kept to put it back.
            bool wrist_freed = false;
            std::vector<uint8_t> wrist_linear, wrist_angular;
            FName parent_name, hand_name, middle_name, thumb_name, lower_name, upper_name;
            // Hand pull (see "pulling the hands" below).
            bool pull_active = false;
            bool have_last_world = false;
            Vec3 last_world_target;     // where the hand bone was last sent, world
            double pull_check_cm = -1;  // the engine's own target for the hand versus ours
            // A physical hand that is far from its controller for a moment (caught behind something)
            // is set at the controller for a few frames and then let go again.
            // Where the hand should be turned to (world), for the turning servo.
            bool have_turn_target = false;
            Quat turn_target;
            double turn_err_deg = 0;
            int lost_frames = 0;
            int behind_frames = 0; // frames in a row the hand has been far from where it is sent
            double rescue_until = 0;
            int rescues = 0;
            bool have_prev_wanted = false;
            Xf prev_wanted;             // where the hand was sent last frame, world
            // Learned from the game's own animation (bone-local).
            bool have_axes = false;
            Vec3 upper_axis, lower_axis; // bone direction in the bone's own frame
            bool have_hinge = false;
            Vec3 hinge_upper, hinge_lower; // elbow hinge axis in each bone's frame
            // How the hand holds things: hand-local vectors (from the physical body's bones).
            bool have_grip = false;
            bool grip_from_weapon = false;
            Vec3 grip_forward; // hand-local: the handle direction (towards the thumb / the blade)
            Vec3 grip_center;  // hand-local: from the hand bone to the middle of the fist / the grip
            // This frame's target.
            bool target_valid = false;
            Xf target_world; // where the hand bone should be
            Vec3 blade_world;
            Quat ctrl_world; // the controller's own turn in the world this frame (self-test)
            // Build 2: a shield the game strapped to the forearm, joined to the hand (shield_on_hand).
            UObject* shield_joint_seen = nullptr;
            int shield_moved = 0;
            bool shield_on_hand = false;
            // For the log.
            double last_error_cm = 0, last_error_deg = 0;
            double last_reach = 0;    // controller distance from the shoulder / arm length
            double elbow_angle = 0;   // how far the elbow is swung from its natural place, degrees
            double elbow_time = 0;
            double shoulder_shift = 0, shoulder_time = 0;
            double last_twist = 0;
            Vec3 assist_pos, assist_rot; // hold assist, in the upper body's frame (cm, rotation vector)
            double assist_time = 0;
            Vec3 assist_last_target;
            double arm_len = 0;         // the character's shoulder-to-wrist length, cm
            Vec3 shoulder_offset;       // character shoulder minus player shoulder, facing frame, cm
            bool have_offset = false;    // hand roll about the forearm last frame, radians (unwrapped)
        };

        struct State
        {
            bool enabled_by_key = true;
            bool failed = false;
            DWORD game_thread = 0;
            uint64_t frame = 0;
            UObject* pawn = nullptr;
            UObject* body = nullptr;   // CharacterMesh0
            UObject* driver = nullptr; // DriverSkeleton
            int32_t bone_count = 0;
            std::vector<int32_t> parents;
            bool ready = false; // bones known, layout found
            Side side[2] = {{L"_l", "left", -1.0, 0}, {L"_r", "right", 1.0, 1}};
            XrHandRaw hands[2];
            bool hands_valid = false;
            double actor_yaw = 0;
            bool menu_up = true;
            // What we last wrote, to tell whether the driver has animated since.
            uint64_t written_frame = 0;
            RawXf written_marker{};
            int32_t marker_bone = -1;
            std::vector<RawXf> animated; // the driver's own pose from its latest fresh frame
            bool have_animated = false;
            // Counters for the log.
            uint64_t fresh_count = 0, stale_count = 0, applied_count = 0;
            std::map<void*, void*> hooked; // tick function object -> its original vtable
            // Body follow (this frame, set in before_body_update).
            Xf body_world; // the body mesh's place in the world
            bool have_body_world = false;
            HeadWorld view;
            bool have_view = false;
            Vec3 actor_delta; // how far the character has moved since the view was taken
            int32_t head = -1, spine_base = -1;
            std::vector<char> below_spine, below_head, below_hand;
            bool have_head_axes = false;
            Quat head_look; // turns the look frame (X forward, Z up) into the head bone's own frame
            bool head_applied = false;
            double lean_cm = 0;
            // The upper body's own standing shape (own_upper_body): every bone from the lowest
            // spine bone up, relative to an upright frame at that bone facing the character's way.
            bool have_rest = false;
            int rest_wait = 0;
            std::vector<Xf> rest;
            bool have_chest_yaw = false;
            double chest_yaw = 0, chest_time = 0;
            bool head_pull_wanted = false, head_pull_active = false;
            bool pull_arms = false;     // what the last body update asked pull_update for
            int pull_applies_second = 0;
            // Placed body (kinematic_body): the body's physics bodies follow the pose exactly.
            bool hold_wanted = false, hold_active = false; // the held body (body_hold)
            bool placed = false;        // the pose is the whole body's truth this frame (held or kinematic)
            double forearm_off_cm = -1; // the right forearm's physics body against where it is drawn
            bool kin_active = false;
            bool kin_hands = false;     // the hands are placed too
            int kin_reattached = 0;     // times the body mesh had to be put back on the character
            bool wrist_failed = false;  // the wrists could not be freed on this character: rigid hands
            double kick_until = 0;      // (unused)
            bool level_ignored = false; // the body mesh ignores the level's geometry right now
            int fighters = 0;           // other characters in the level (for the log)
            double nearest_fighter_cm = -1;
            Vec3 actor_world;           // the character's position this frame
            bool have_actor_world = false;
            Vec3 step_last_actor, step_velocity;
            double gait_phase = 0;      // 0..1 over one full stride (left foot swings first, then right)
            bool gait_moving = false;
            // hold_ground: where the character may be this frame at the most, and what was taken back.
            double ground_time = 0;
            double foreign_cm = 0;      // distance gone beyond its own speed lately
            Vec3 room_step;             // what roomscale moved the character since the last look
            int ground_undone = 0;
            double ground_undone_cm = 0;
            bool actor_tick_hooked = false; // the character's own tick is wrapped (exact hold_ground)
            int drag_undone = 0;            // times the game's script moved the character in its tick
            double drag_undone_cm = 0;
            int fall_frames = 0;            // frames in which the game took the character for falling
            int bumps_muted = 0;            // body hits by props or the level kept from the game's damage script
            double trap_struck_time = -100; // when a loose weapon or trap last cost the character health
            bool have_max_dislocation = false;
            double game_max_dislocation = 10.0;
            double step_time = 0;
            int steps_taken = 0;
            double thrown_cm = 0;
            int steps_total = 0, ground_undone_total = 0;
            bool kin_attach_ok = false;
            int kin_retaken = 0;        // times the game switched simulation back on and we undid it
            double kin_blocked_until = 0;
            int32_t pelvis = -1;
            std::vector<char> below_pelvis, leg_below; // leg_below: strictly below a thigh
            Xf nominal;                 // where the body mesh belongs over the character's feet, world
            bool have_nominal = false;
            double pelvis_drop = 0, consciousness = -1, mesh_off_cm = 0;
            double rest_head_z = 0;         // the head bone's height above the mesh's origin, standing (size-free)
            double walked_cm = 0, stepped_cm = 0, input_most = 0, input_forward = 0; // this second: who moved the character
            bool fallen = false;
            bool have_head_target = false;
            Vec3 head_target_world; // where the head bone was last sent
            double head_err_cm = -1;
        };
        State g_state;
        std::mutex g_hook_mutex;

        // ---- hooking the meshes' tick functions ------------------------------------------------
        // FTickFunction has four virtual functions in UE 5.4.4 (EngineBaseTypes.h): destructor,
        // ExecuteTick, DiagnosticMessage, DiagnosticContext. Each hooked tick function gets its
        // vtable pointer swapped for a copy whose ExecuteTick runs our code first; every other
        // object of the type is left alone.
        using ExecuteTickFn = void (*)(void* self, float delta, int tick_type, int current_thread, const void* completion);
        // Four are copied plus a margin (copying a few words past the table is harmless; only the
        // four are ever called).
        constexpr int kTickVtableSlots = 8;
        constexpr int kExecuteTickSlot = 1;
        std::map<void**, void**> g_fake_vtables; // original -> our copy (never freed)
        ExecuteTickFn g_original_execute = nullptr;

        void before_body_update();
        void before_muscles_update();
        void placed_body_facts(bool count);
        void no_self_hits_update();
        bool body_set_simulate(const wchar_t* bone, bool simulate);
        bool g_self_hits_redo = false; // the collision switches have to be set again
        std::atomic<UObject*> g_held_item[2]{{nullptr}, {nullptr}}; // what the hands hold (actors)
        // The tick function of the character's physical animation component (its "muscles"). Our
        // settings for the hands are written immediately before it runs: the game's own script
        // writes its settings at some other point of the frame, and whoever writes last before
        // this tick is what the physics gets. (First self-test: written from a mesh's tick, the
        // hand drive never took effect and the freed hands lay on the floor.)
        void* g_muscle_tick = nullptr;

        void hooked_execute_tick(void* self, float delta, int tick_type, int current_thread, const void* completion)
        {
            if (GetCurrentThreadId() == g_state.game_thread)
            {
                if (self == g_muscle_tick) before_muscles_update();
                else before_body_update();
            }
            g_original_execute(self, delta, tick_type, current_thread, completion);
        }

        bool in_game_module(void* address)
        {
            HMODULE module = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)address, &module)) return false;
            return module == GetModuleHandleW(nullptr);
        }

        bool hook_component_tick(UObject* component)
        {
            FProperty* property = component->GetPropertyByNameInChain(STR("PrimaryComponentTick"));
            if (!property) return false;
            void* tick = reinterpret_cast<uint8_t*>(component) + property->GetOffset_Internal();
            void** vtable = *reinterpret_cast<void***>(tick);
            for (auto& entry : g_fake_vtables)
            {
                if (entry.second == vtable) return true; // already ours
            }
            if (!in_game_module(vtable[kExecuteTickSlot]))
            {
                HSVR_LOG("Arms: the tick function of %ls does not look like the engine's; not hooked", component->GetName().c_str());
                return false;
            }
            void** fake = nullptr;
            auto found = g_fake_vtables.find(vtable);
            if (found != g_fake_vtables.end())
            {
                fake = found->second;
            }
            else
            {
                auto original = reinterpret_cast<ExecuteTickFn>(vtable[kExecuteTickSlot]);
                if (g_original_execute && g_original_execute != original)
                {
                    HSVR_LOG("Arms: a second kind of tick function (%ls); not hooked", component->GetName().c_str());
                    return false;
                }
                g_original_execute = original;
                fake = new void*[kTickVtableSlots];
                for (int i = 0; i < kTickVtableSlots; ++i) fake[i] = vtable[i];
                fake[kExecuteTickSlot] = reinterpret_cast<void*>(&hooked_execute_tick);
                g_fake_vtables[vtable] = fake;
            }
            *reinterpret_cast<void***>(tick) = fake;
            return true;
        }

        // ---- the character's own tick -----------------------------------------------------------
        // The game's script runs in the character's tick. Each frame it sets the capsule's place
        // outright: to wherever the hips and chest are, plus the shoulders' lean, a stumble after a
        // kick or a blow, and a dodge, once that is further off than a reach it works out anew every
        // frame (10 cm standing still). For a ragdoll that is how the character follows its body.
        // For the placed body, which rides on the capsule, it means a kick, a duck or a lean drags
        // the player along. The movement component, which does the walking, has its own tick; so
        // whatever moves the character sideways inside this tick is that dragging, and is put back
        // the moment the tick returns, before anything is drawn or simulated.
        ExecuteTickFn g_original_actor_execute = nullptr;
        void** g_actor_real_vtable = nullptr;
        void** g_actor_fake_vtable = nullptr;
        void* g_actor_tick = nullptr;      // the tick function of the character we play
        UObject* g_actor_tick_pawn = nullptr;
        bool before_actor_tick(Vec3& at);
        void after_actor_tick(const Vec3& at);
        void hit_hook_setup(UObject* pawn);

        void hooked_actor_tick(void* self, float delta, int tick_type, int current_thread, const void* completion)
        {
            Vec3 at;
            bool ours = self == g_actor_tick && GetCurrentThreadId() == g_state.game_thread && before_actor_tick(at);
            g_original_actor_execute(self, delta, tick_type, current_thread, completion);
            if (ours && self == g_actor_tick) after_actor_tick(at);
        }

        bool hook_actor_tick(UObject* actor)
        {
            FProperty* property = actor->GetPropertyByNameInChain(STR("PrimaryActorTick"));
            if (!property) return false;
            void* tick = reinterpret_cast<uint8_t*>(actor) + property->GetOffset_Internal();
            void** vtable = *reinterpret_cast<void***>(tick);
            if (vtable != g_actor_fake_vtable)
            {
                if (!in_game_module(vtable[kExecuteTickSlot])) return false;
                if (g_actor_real_vtable && vtable != g_actor_real_vtable) return false;
                if (!g_actor_fake_vtable)
                {
                    g_actor_real_vtable = vtable;
                    g_original_actor_execute = reinterpret_cast<ExecuteTickFn>(vtable[kExecuteTickSlot]);
                    g_actor_fake_vtable = new void*[kTickVtableSlots];
                    for (int i = 0; i < kTickVtableSlots; ++i) g_actor_fake_vtable[i] = vtable[i];
                    g_actor_fake_vtable[kExecuteTickSlot] = reinterpret_cast<void*>(&hooked_actor_tick);
                }
                *reinterpret_cast<void***>(tick) = g_actor_fake_vtable;
            }
            g_actor_tick = tick;
            g_actor_tick_pawn = actor;
            return true;
        }

        // ---- finding the character's parts -------------------------------------------------------
        FName side_name(const wchar_t* stem, const Side& side)
        {
            std::wstring text = std::wstring(stem) + side.suffix;
            return make_name(text.c_str());
        }

        int32_t driver_bone_index(FName name)
        {
            UFunction* fn = g_state.driver->GetFunctionByNameInChain(STR("GetBoneIndex"));
            if (!fn) return -1;
            NameIntParams p{};
            p.BoneName = name;
            p.ReturnValue = -1;
            g_state.driver->ProcessEvent(fn, &p);
            return p.ReturnValue;
        }

        bool setup_pawn(UObject* pawn)
        {
            State& s = g_state;
            s.ready = false;
            s.have_animated = false;
            s.pawn = pawn;
            s.body = object_property(pawn, STR("Mesh"));
            s.driver = object_property(pawn, STR("DriverSkeleton"));
            if (!s.driver)
            {
                std::vector<UObject*> meshes;
                UObjectGlobals::FindAllOf(STR("SkeletalMeshComponent"), meshes);
                for (UObject* m : meshes)
                {
                    if (m && m->GetOuterPrivate() == pawn && m->GetName() == STR("DriverSkeleton")) s.driver = m;
                }
            }
            if (!s.body || !s.driver)
            {
                static UObject* told = nullptr;
                if (told == pawn) return false;
                told = pawn;
                HSVR_LOG("Arms: %ls has no %s; motion arms wait for a fighting character", pawn->GetName().c_str(), s.body ? "DriverSkeleton" : "body mesh");
                return false;
            }
            s.bone_count = call_int(s.driver, STR("GetNumBones"));
            if (s.bone_count <= 0 || s.bone_count > 1000) return false;
            // Parent of every driver bone (bones come parents first).
            s.parents.assign(s.bone_count, -1);
            UFunction* name_fn = s.driver->GetFunctionByNameInChain(STR("GetBoneName"));
            UFunction* parent_fn = s.driver->GetFunctionByNameInChain(STR("GetParentBone"));
            if (!name_fn || !parent_fn) return false;
            std::wstring names;
            for (int32_t i = 0; i < s.bone_count; ++i)
            {
                IntNameParams n{};
                n.BoneIndex = i;
                s.driver->ProcessEvent(name_fn, &n);
                NameNameParams p{};
                p.BoneName = n.ReturnValue;
                s.driver->ProcessEvent(parent_fn, &p);
                s.parents[i] = driver_bone_index(p.ReturnValue);
                names += n.ReturnValue.ToString() + L" ";
            }
            HSVR_LOG("Arms: driver skeleton %ls, %d bones: %ls", s.driver->GetFullName().c_str(), s.bone_count, names.c_str());
            for (int32_t i = 0; i < s.bone_count; ++i)
            {
                if (s.parents[i] >= i)
                {
                    HSVR_LOG("Arms: driver bone %d comes before its parent %d; motion arms stay off", i, s.parents[i]);
                    return false;
                }
            }
            std::vector<FName> check_names;
            std::vector<int32_t> check_index;
            for (Side& side : s.side)
            {
                side.clavicle = driver_bone_index(side_name(L"clavicle", side));
                side.upper = driver_bone_index(side_name(L"upperarm", side));
                side.lower = driver_bone_index(side_name(L"lowerarm", side));
                side.hand = driver_bone_index(side_name(L"hand", side));
                side.hand_name = side_name(L"hand", side);
                side.lower_name = side_name(L"lowerarm", side);
                side.upper_name = side_name(L"upperarm", side);
                side.pull_active = side.have_last_world = side.have_prev_wanted = false;
                side.have_foot_home = side.foot_planted = side.stepping = side.gait_step = false;
                side.wrist_freed = false;
                side.grip_component = nullptr;
                side.grip_locked = false;
                side.grip_saved.clear();
                side.middle_name = side_name(L"middle_01", side);
                side.thumb_name = side_name(L"thumb_01", side);
                side.have_axes = side.have_hinge = side.have_grip = false;
                if (side.upper < 0 || side.lower < 0 || side.hand < 0 || s.parents[side.lower] != side.upper || s.parents[side.hand] != side.lower)
                {
                    HSVR_LOG("Arms: the driver's %s arm is not upperarm -> lowerarm -> hand (%d %d %d); motion arms stay off", side.label, side.upper, side.lower,
                             side.hand);
                    return false;
                }
                // The frame the arm hangs from: the clavicle's parent (upper spine), or the
                // upper arm's parent when there is no clavicle.
                side.parent = side.clavicle >= 0 ? s.parents[side.clavicle] : s.parents[side.upper];
                if (side.parent < 0) return false;
                IntNameParams n{};
                n.BoneIndex = side.parent;
                s.driver->ProcessEvent(name_fn, &n);
                side.parent_name = n.ReturnValue;
                check_names.push_back(side.hand_name);
                check_index.push_back(side.hand);
                check_names.push_back(side_name(L"upperarm", side));
                check_index.push_back(side.upper);
            }
            s.marker_bone = s.side[1].hand;
            // Body follow: the head, and the lowest spine bone the upper body leans about.
            s.head = driver_bone_index(make_name(L"head"));
            s.spine_base = driver_bone_index(make_name(L"spine_01"));
            if (s.spine_base < 0) s.spine_base = driver_bone_index(make_name(L"spine_02"));
            s.below_spine.assign(s.bone_count, 0);
            s.below_head.assign(s.bone_count, 0);
            s.below_hand.assign(s.bone_count, 0);
            for (int32_t i = 0; i < s.bone_count; ++i)
            {
                int32_t parent = s.parents[i];
                s.below_hand[i] = parent >= 0 && (parent == s.side[0].hand || parent == s.side[1].hand || s.below_hand[parent]);
            }
            s.have_head_axes = false;
            s.have_rest = s.have_chest_yaw = s.have_head_target = false;
            s.rest_wait = 0;
            if (s.head >= 0 && s.spine_base >= 0)
            {
                for (int32_t i = 0; i < s.bone_count; ++i)
                {
                    int32_t parent = s.parents[i];
                    s.below_spine[i] = i == s.spine_base || (parent >= 0 && s.below_spine[parent]);
                    s.below_head[i] = i == s.head || (parent >= 0 && s.below_head[parent]);
                }
                if (!s.below_spine[s.head]) s.head = -1;
            }
            s.kin_active = false;
            s.kin_reattached = 0;
            s.kin_attach_ok = false;
            s.wrist_failed = false;
            s.level_ignored = false;
            s.have_actor_world = false;
            s.have_max_dislocation = false;
            s.ground_time = 0;
            s.room_step = {};
            s.hold_wanted = s.hold_active = s.placed = false;
            s.kin_retaken = 0;
            s.pelvis = s.spine_base >= 0 ? s.parents[s.spine_base] : -1;
            s.below_pelvis.assign(s.bone_count, 0);
            s.leg_below.assign(s.bone_count, 0);
            for (Side& side : s.side)
            {
                side.thigh = driver_bone_index(side_name(L"thigh", side));
                side.calf = driver_bone_index(side_name(L"calf", side));
                side.foot = driver_bone_index(side_name(L"foot", side));
                if (side.thigh < 0 || side.calf < 0 || side.foot < 0 || s.parents[side.calf] != side.thigh || s.parents[side.foot] != side.calf)
                    side.thigh = side.calf = side.foot = -1;
            }
            for (int32_t i = 0; i < s.bone_count && s.pelvis >= 0; ++i)
            {
                int32_t parent = s.parents[i];
                s.below_pelvis[i] = i == s.pelvis || (parent >= 0 && s.below_pelvis[parent]);
                s.leg_below[i] = parent >= 0 && (parent == s.side[0].thigh || parent == s.side[1].thigh || s.leg_below[parent]);
            }
            if (s.head < 0 || s.spine_base < 0)
                HSVR_LOG("Body: no head or spine bone found on the driver skeleton (%d %d); the upper body does not follow the headset", s.head, s.spine_base);
            if (!g_layout.arrays_offset && !find_driver_layout(s.driver, s.bone_count, check_names, check_index))
            {
                static int complaints = 0;
                if (++complaints <= 3) HSVR_LOG("Arms: could not find the driver skeleton's bone transforms (try %d); will try again", complaints);
                if (complaints >= 30)
                {
                    s.failed = true;
                    HSVR_LOG("Arms: giving up; motion arms stay off for this run");
                }
                return false;
            }
            // Hook the tick of every other skeletal mesh on the character, so that whichever
            // updates first after the driver has animated applies the arms.
            std::vector<UObject*> meshes;
            UObjectGlobals::FindAllOf(STR("SkeletalMeshComponent"), meshes);
            int hooked = 0;
            {
                std::lock_guard<std::mutex> lock(g_hook_mutex);
                for (UObject* m : meshes)
                {
                    if (!m || m == s.driver || m->GetOuterPrivate() != pawn) continue;
                    if (hook_component_tick(m)) ++hooked;
                }
            }
            if (hooked == 0 || !g_original_execute)
            {
                HSVR_LOG("Arms: no mesh of the character could be hooked; motion arms stay off");
                return false;
            }
            HSVR_LOG("Arms: ready on %ls (%d meshes follow the arms; arm frames %ls / %ls)", pawn->GetName().c_str(), hooked,
                     s.side[0].parent_name.ToString().c_str(), s.side[1].parent_name.ToString().c_str());
            {
                std::lock_guard<std::mutex> lock(g_hook_mutex);
                s.actor_tick_hooked = hook_actor_tick(pawn);
            }
            if (config().dev_character_scale > 0.3 && config().dev_character_scale < 3.0)
            {
                UFunction* size_fn = pawn->GetFunctionByNameInChain(STR("SetActorScale3D"));
                if (size_fn && size_fn->GetParmsSize() == (int32_t)sizeof(ue::FVector))
                {
                    double v = config().dev_character_scale;
                    ue::FVector size{v, v, v};
                    pawn->ProcessEvent(size_fn, &size);
                    HSVR_LOG("TEST MODE: the character was made %.2f of the standard size (dev_character_scale)", v);
                }
            }
            hit_hook_setup(pawn);
            HSVR_LOG(s.actor_tick_hooked ? "Room: the character's own tick is wrapped (the game's dragging of the character is undone on the spot)"
                                         : "Room: the character's own tick could not be wrapped; moves by the game are taken back by their size only");
            s.ready = true;
            return true;
        }

        // ---- learning from the game's animation ------------------------------------------------
        void learn_from_animation(Side& side, const RawXf* pose)
        {
            Xf su = from_raw(pose[side.upper]), sl = from_raw(pose[side.lower]), sh = from_raw(pose[side.hand]);
            Vec3 u = sl.t - su.t, f = sh.t - sl.t;
            if (length(u) < 1.0 || length(f) < 1.0) return;
            side.arm_len = length(u) + length(f);
            Vec3 upper_axis = normalized(su.q.inverse().rotate(u));
            Vec3 lower_axis = normalized(sl.q.inverse().rotate(f));
            if (!side.have_axes)
            {
                side.upper_axis = upper_axis;
                side.lower_axis = lower_axis;
                side.have_axes = true;
            }
            Vec3 bend = cross(normalized(u), normalized(f));
            double sin_bend = length(bend);
            if (sin_bend < 0.25) return; // nearly straight: the hinge cannot be told
            Vec3 hinge = bend * (1.0 / sin_bend);
            Vec3 hu = normalized(su.q.inverse().rotate(hinge));
            Vec3 hl = normalized(sl.q.inverse().rotate(hinge));
            if (!side.have_hinge)
            {
                side.hinge_upper = hu;
                side.hinge_lower = hl;
                side.have_hinge = true;
                HSVR_LOG("Arms: %s elbow hinge learned from the game's animation (bend %.0f deg): upper (%.2f %.2f %.2f) lower (%.2f %.2f %.2f), "
                         "bone axes upper (%.2f %.2f %.2f) lower (%.2f %.2f %.2f)",
                         side.label, std::asin(std::min(1.0, sin_bend)) * kRadToDeg, hu.x, hu.y, hu.z, hl.x, hl.y, hl.z, upper_axis.x, upper_axis.y,
                         upper_axis.z, lower_axis.x, lower_axis.y, lower_axis.z);
            }
            else
            {
                // A slow average; an elbow is a fixed hinge, so this only takes out noise.
                side.hinge_upper = normalized(side.hinge_upper * 0.98 + hu * 0.02);
                side.hinge_lower = normalized(side.hinge_lower * 0.98 + hl * 0.02);
            }
        }

        // How the hand holds a weapon (right hand) or a closed fist, measured on the physical body.
        void learn_grip(Side& side)
        {
            State& s = g_state;
            Xf hand;
            if (!socket_transform(s.body, side.hand_name, RTS_World, hand)) return;
            Quat inv = hand.q.inverse();
            Config cfg = config();
            bool from_weapon = false;
            Vec3 forward, center;
            // (grip_fixed: never from the weapon. Its root-to-head line is not its handle: a sickle's
            // head curves away, and the hand, and with it the weapon, was turned to put that line
            // along the controller; it differed by up to 40 degrees between weapons.)
            if (side.out_sign > 0.0 && !cfg.grip_fixed)
            {
                UObject* weapon = object_property(s.pawn, STR("Weapon R"));
                Vec3 root, head;
                if (weapon && actor_location(weapon, root) && component_location(object_property(weapon, STR("Head")), head) && length(head - root) > 3.0 &&
                    length(root - hand.t) < 40.0)
                {
                    forward = normalized(inv.rotate(head - root));
                    center = inv.rotate(root - hand.t) + forward * cfg.grip_shift_cm;
                    from_weapon = length(perpendicular_part(center, forward)) > 1.0;
                }
            }
            if (!from_weapon)
            {
                Xf middle, thumb;
                if (!socket_transform(s.body, side.middle_name, RTS_World, middle) || !socket_transform(s.body, side.thumb_name, RTS_World, thumb)) return;
                Vec3 knuckle = inv.rotate(middle.t - hand.t);
                Vec3 thumb_dir = inv.rotate(thumb.t - hand.t);
                if (length(knuckle) < 2.0) return;
                Vec3 k = normalized(knuckle);
                forward = normalized(perpendicular_part(thumb_dir, k));
                center = knuckle * 0.7 + forward * cfg.grip_shift_cm;
            }
            bool first = !side.have_grip || side.grip_from_weapon != from_weapon;
            if (first)
            {
                side.grip_forward = forward;
                side.grip_center = center;
                side.have_grip = true;
                side.grip_from_weapon = from_weapon;
                HSVR_LOG("Arms: %s hand grip measured from %s: handle (%.2f %.2f %.2f), grip centre (%.1f %.1f %.1f) cm in the hand's frame", side.label,
                         from_weapon ? "the weapon in it" : "its fingers", forward.x, forward.y, forward.z, center.x, center.y, center.z);
            }
            else
            {
                side.grip_forward = normalized(side.grip_forward * 0.9 + forward * 0.1);
                side.grip_center = side.grip_center * 0.9 + center * 0.1;
            }
        }

        // ---- arm length calibration ----------------------------------------------------------------
        // The player holds both arms straight out in front at shoulder height and holds both grip
        // buttons for 2 seconds. The hands are then in front of the shoulders: their height gives
        // the shoulder drop below the eyes, their spread the shoulder width, and their distance in
        // front of the eyes (plus 6 cm from the eyes back to the shoulder joint) the arm length.
        constexpr double kShoulderBackM = 0.06;

        void calibrate_tick()
        {
            State& s = g_state;
            static double hold_start = 0.0;
            static bool done = false;
            const XrHandRaw& l = s.hands[0];
            const XrHandRaw& r = s.hands[1];
            bool holding = s.hands_valid && l.valid && r.valid && l.squeeze > 0.8f && r.squeeze > 0.8f;
            if (!holding)
            {
                hold_start = 0.0;
                done = false;
                return;
            }
            double now = now_seconds();
            if (hold_start == 0.0) hold_start = now;
            if (done || now - hold_start < 2.0) return;
            done = true;
            Vec3 head, fwd;
            if (!stereo_head_tracking(head, fwd)) return;
            Vec3 right{-fwd.z, 0, fwd.x};
            Vec3 dl = Vec3{l.px, l.py, l.pz} - head, dr = Vec3{r.px, r.py, r.pz} - head;
            double side_cm = (dot(dr, right) - dot(dl, right)) * 0.5 * 100.0;
            double drop_cm = -(dl.y + dr.y) * 0.5 * 100.0;
            double arm_cm = ((dot(dl, fwd) + dot(dr, fwd)) * 0.5 + kShoulderBackM) * 100.0;
            if (arm_cm < 40.0 || arm_cm > 95.0 || side_cm < 8.0 || side_cm > 35.0 || drop_cm < 5.0 || drop_cm > 50.0)
            {
                HSVR_LOG("Arms: calibration ignored (arm %.0f cm, shoulders %.0f cm to each side, %.0f cm below the eyes): hold both arms straight out in front "
                         "at shoulder height", arm_cm, side_cm, drop_cm);
                return;
            }
            char a[32], b[32], c[32];
            snprintf(a, sizeof(a), "%.1f", arm_cm);
            snprintf(b, sizeof(b), "%.1f", drop_cm);
            snprintf(c, sizeof(c), "%.1f", side_cm);
            config_store({{"player_arm_cm", a}, {"player_shoulder_drop_cm", b}, {"player_shoulder_side_cm", c}});
            // How the controllers sit in the hands. In this pose the forearm runs from the shoulder to
            // the hand, the wrist is straight and the thumb is up: so the direction from the wrist
            // into the fist is the forearm's, and the handle (towards the thumb) points up. Both are
            // noted in each controller's own axes and used from now on in place of the assumed ones.
            // (Matthew: "i held my hand straight but my in game wrist was bent".)
            for (int k = 0; k < 2; ++k)
            {
                const XrHandRaw& hand = k ? r : l;
                double sign = k ? 1.0 : -1.0;
                Vec3 shoulder = head + right * (sign * side_cm / 100.0) + Vec3{0, -drop_cm / 100.0, 0} - fwd * kShoulderBackM;
                Vec3 forearm = normalized(Vec3{hand.px, hand.py, hand.pz} - shoulder);
                Vec3 up = normalized(perpendicular_part({0, 1, 0}, forearm), {0, 1, 0});
                Quat inv = Quat{hand.qx, hand.qy, hand.qz, hand.qw}.normalized().inverse();
                Vec3 fist_xr = inv.rotate(forearm), thumb_xr = inv.rotate(up);
                // The controller's own axes in the mod's convention (as xr_to_ue_position): forward -z, right x, up y.
                Vec3 handle{-thumb_xr.z, thumb_xr.x, thumb_xr.y}, fist{-fist_xr.z, fist_xr.x, fist_xr.y};
                char text[160];
                snprintf(text, sizeof(text), "%.4f %.4f %.4f %.4f %.4f %.4f", handle.x, handle.y, handle.z, fist.x, fist.y, fist.z);
                config_store({{k ? "ctrl_axes_r" : "ctrl_axes_l", text}});
                double handle_off = std::acos(std::max(-1.0, std::min(1.0, handle.x))) * kRadToDeg;
                double fist_off = std::acos(std::max(-1.0, std::min(1.0, -fist.z))) * kRadToDeg;
                HSVR_LOG("Arms: %s controller in the hand: handle (%.2f %.2f %.2f), wrist-to-fist (%.2f %.2f %.2f); that is %.0f and %.0f degrees from what was assumed; "
                         "as one tilt that is %.0f degrees (grip_tilt_deg is %.0f%s)",
                         k ? "right" : "left", handle.x, handle.y, handle.z, fist.x, fist.y, fist.z, handle_off, fist_off, std::atan2(handle.z, handle.x) * kRadToDeg,
                         config().grip_tilt_deg, config().grip_fixed ? "; the measurement is only reported, grip_fixed is on" : "");
            }
            {
                float origin = shared().local_origin_height_m.load();
                if (origin >= 0.0f)
                {
                    double eye_cm = (origin + head.y) * 100.0;
                    char text[32];
                    snprintf(text, sizeof(text), "%.1f", eye_cm);
                    config_store({{"player_eye_cm", text}});
                    HSVR_LOG("Arms: your eyes are %.0f cm above the floor standing (stored as player_eye_cm)", eye_cm);
                }
            }
            for (Side& side : s.side) side.have_offset = false;
            HSVR_LOG("Arms: calibrated: your arm %.0f cm (character %.0f / %.0f cm), shoulders %.0f cm below the eyes and %.0f cm to each side", arm_cm,
                     s.side[0].arm_len + length(s.side[0].grip_center), s.side[1].arm_len + length(s.side[1].grip_center), drop_cm, side_cm);
        }

        // Moves a controller's grip from the player's proportions to the character's: measured from
        // the player's shoulder, scaled by the ratio of arm lengths, and placed at the character's
        // shoulder. The hand's turn is not changed, so the blade angle stays the player's.
        void scale_reach(Side& side, const XrHandRaw& raw, Vec3& grip_pos, const Config& cfg)
        {
            State& s = g_state;
            if (!cfg.arm_scaling || cfg.player_arm_cm <= 0.0 || side.arm_len <= 0.0) return;
            Vec3 head, fwd;
            if (!stereo_head_tracking(head, fwd)) return;
            Vec3 right{-fwd.z, 0, fwd.x};
            Vec3 shoulder_xr = head + right * (side.out_sign * cfg.player_shoulder_side_cm / 100.0) + Vec3{0, -cfg.player_shoulder_drop_cm / 100.0, 0} -
                               fwd * kShoulderBackM;
            Vec3 player_shoulder;
            Quat unused;
            if (!stereo_tracking_to_world((float)shoulder_xr.x, (float)shoulder_xr.y, (float)shoulder_xr.z, 0, 0, 0, 1, player_shoulder, unused)) return;
            Xf character_shoulder;
            if (!socket_transform(s.body, side_name(L"upperarm", side), RTS_World, character_shoulder)) return;
            Quat facing = quat_from_yaw_deg(s.actor_yaw);
            Vec3 offset = facing.inverse().rotate(character_shoulder.t - player_shoulder);
            // Smoothed, so the character's sway does not shake the sword.
            side.shoulder_offset = side.have_offset ? side.shoulder_offset * 0.95 + offset * 0.05 : offset;
            side.have_offset = true;
            double k = (side.arm_len + length(side.grip_center)) / cfg.player_arm_cm;
            k = std::max(0.6, std::min(1.4, k));
            grip_pos = player_shoulder + facing.rotate(side.shoulder_offset) + (grip_pos - player_shoulder) * k;
            (void)raw;
        }

        // ---- where the hands should be (game thread, before the world ticks) -------------------
        void compute_targets()
        {
            State& s = g_state;
            Config cfg = config();
            Quat extra = quat_from_rot_deg(cfg.hand_pitch, cfg.hand_yaw, cfg.hand_roll);
            for (Side& side : s.side)
            {
                side.target_valid = false;
                const XrHandRaw& raw = s.hands[side.controller_index];
                bool wanted = side.out_sign > 0 ? cfg.arm_right : cfg.arm_left;
                if (!wanted || !raw.valid || !side.have_grip)
                {
                    side.assist_pos = {};
                    side.assist_rot = {};
                    side.assist_time = 0;
                    continue;
                }
                Vec3 grip_pos;
                Quat grip_rot;
                if (!stereo_tracking_to_world(raw.px, raw.py, raw.pz, raw.qx, raw.qy, raw.qz, raw.qw, grip_pos, grip_rot)) continue;
                grip_rot = (grip_rot * extra).normalized();
                side.ctrl_world = grip_rot;
                // With hand pull the hand goes where the controller really is; the older reach
                // scaling only applies without it.
                if (!cfg.hand_pull) scale_reach(side, raw, grip_pos, cfg);
                // The controller's handle (+X) and the direction from the wrist into the fist (-Z)
                // match the hand's handle and grip-centre directions.
                Quat hand_frame = quat_from_frame(side.grip_forward, side.grip_center);
                // ... as assumed, or as measured by the calibration (ctrl_axes_l / ctrl_axes_r).
                Vec3 handle_axis{1, 0, 0}, fist_axis{0, 0, -1};
                if (cfg.grip_fixed)
                {
                    // One fixed angle for every weapon and both hands: the handle leaves the
                    // controller's pointing direction by grip_tilt_deg towards its top.
                    double tilt = cfg.grip_tilt_deg * kDegToRad;
                    handle_axis = {std::cos(tilt), 0, std::sin(tilt)};
                    fist_axis = {std::sin(tilt), 0, -std::cos(tilt)};
                }
                else
                {
                    const std::string& text = side.out_sign > 0 ? cfg.ctrl_axes_r : cfg.ctrl_axes_l;
                    double v[6];
                    if (!text.empty() && sscanf(text.c_str(), "%lf %lf %lf %lf %lf %lf", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6)
                    {
                        Vec3 a{v[0], v[1], v[2]}, b{v[3], v[4], v[5]};
                        if (length(a) > 0.9 && length(a) < 1.1 && length(b) > 0.9 && length(b) < 1.1 && std::fabs(dot(a, b)) < 0.5)
                        {
                            handle_axis = normalized(a);
                            fist_axis = normalized(b);
                        }
                    }
                }
                Quat controller_frame = quat_from_frame(handle_axis, fist_axis);
                Quat hand_rot = (grip_rot * controller_frame * hand_frame.inverse()).normalized();
                side.target_world.q = hand_rot;
                side.target_world.t = grip_pos - hand_rot.rotate(side.grip_center);
                side.blade_world = grip_rot.rotate(handle_axis);
                side.target_valid = true;
            }
        }

        void sync_aim()
        {
            State& s = g_state;
            Config cfg = config();
            if (!cfg.aim_sync) return;
            const Side& right = s.side[1];
            if (!right.target_valid) return;
            static FProperty* property = nullptr;
            static UClass* property_class = nullptr;
            static bool ok = false;
            UClass* cls = s.pawn->GetClassPrivate();
            if (cls != property_class)
            {
                property_class = cls;
                property = s.pawn->GetPropertyByNameInChain(STR("Aim Vector"));
                ok = false;
                if (property)
                {
                    // Seen live as a 2-D point; a 3-D vector (X and Y used) is accepted too.
                    auto* sp = static_cast<FStructProperty*>(property);
                    UScriptStruct* st = property->GetClass().GetName() == STR("StructProperty") ? sp->GetStruct() : nullptr;
                    std::wstring type = st ? std::wstring(st->GetName()) : std::wstring(property->GetClass().GetName());
                    ok = st && ((type == L"Vector2D" && property->GetSize() == 16) || (type == L"Vector" && property->GetSize() == 24));
                    HSVR_LOG("Arms: the game's aim (Aim Vector) is a %ls of %d bytes at 0x%X; %s", type.c_str(), property->GetSize(), property->GetOffset_Internal(),
                             ok ? "it follows the sword" : "not a type the mod can write; aim_sync does nothing");
                }
                else
                {
                    std::wstring names;
                    for (UStruct* st = cls; st; st = st->GetSuperStruct())
                    {
                        for (FProperty* p : st->ForEachProperty())
                        {
                            std::wstring n = p->GetName();
                            if (n.find(L"Aim") != std::wstring::npos) names += n + L" (" + std::wstring(p->GetClass().GetName()) + L") ";
                        }
                    }
                    HSVR_LOG("Arms: the character has no property named Aim Vector; aim_sync does nothing. Aim properties: %ls", names.c_str());
                }
            }
            if (!ok) return;
            Vec3 b = right.blade_world;
            double yaw = normalize_axis(std::atan2(b.y, b.x) * kRadToDeg - s.actor_yaw);
            double pitch = std::asin(std::max(-1.0, std::min(1.0, b.z))) * kRadToDeg;
            auto* aim = reinterpret_cast<ue::FVector2D*>(reinterpret_cast<uint8_t*>(s.pawn) + property->GetOffset_Internal());
            aim->X = std::max(-90.0, std::min(90.0, yaw * cfg.aim_scale));
            double y = -pitch * cfg.aim_scale * (cfg.aim_invert_y ? -1.0 : 1.0);
            aim->Y = std::max(-90.0, std::min(90.0, y));
        }

        // ---- bending the driver's arms (game thread, inside a mesh's tick) ---------------------
        void solve_side(Side& side, const RawXf* anim, std::vector<Xf>& pose, std::vector<char>& changed, const Config& cfg)
        {
            State& s = g_state;
            if (!side.target_valid || !side.have_axes) return;
            Xf parent_world;
            if (!socket_transform(s.body, side.parent_name, RTS_World, parent_world)) return;
            Xf parent_cs = pose[side.parent];
            // The target in the frame of the physical upper body, placed on the driver's upper body.
            // The character may have moved since the view (and so the target) was taken.
            Xf wanted = side.target_world;
            wanted.t = wanted.t + s.actor_delta;
            // Hand pull: the hand body is pulled to a place in the world, so the pose is solved
            // in the world too, from where the physical shoulder really is.
            // The same holds for the placed body, whose shoulder is exactly where this frame's pose
            // has put it (the body's own shoulder would be last frame's).
            bool placed = s.placed && s.have_body_world;
            bool pull = (side.pull_active || placed) && s.have_body_world;
            Xf shoulder_world;
            if (placed) shoulder_world = compose(pose[side.upper], s.body_world);
            else if (pull && !socket_transform(s.body, side.upper_name, RTS_World, shoulder_world)) pull = false;
            Xf target_rel = relative(wanted, parent_world);
            // Hold assist: the muscles leave the real hand short of where it is sent by a steady
            // amount (sword weight, muscle strength). That steady part is measured in the upper
            // body's frame and added to what is sent, slowly (assist_seconds), within assist_max_cm
            // and assist_max_deg. Fast swings are not changed by it.
            Xf body_hand_now;
            if (!pull && cfg.assist_seconds > 0.0 && socket_transform(s.body, side.hand_name, RTS_World, body_hand_now))
            {
                Xf real_rel = relative(body_hand_now, parent_world);
                double now = now_seconds();
                double dt = side.assist_time > 0 ? std::min(0.1, now - side.assist_time) : 0.0;
                side.assist_time = now;
                double k = dt / cfg.assist_seconds;
                // Only while the hand moves slowly: fast swings keep the game's own lag.
                if (dt > 0)
                {
                    double speed = length(target_rel.t - side.assist_last_target) / dt;
                    double ratio = cfg.assist_speed_cms > 0 ? speed / cfg.assist_speed_cms : 0.0;
                    k /= 1.0 + ratio * ratio;
                }
                side.assist_last_target = target_rel.t;
                Vec3 pos_err = target_rel.t - real_rel.t;
                Quat rot_err = (target_rel.q * real_rel.q.inverse()).normalized();
                if (rot_err.w < 0) rot_err = {-rot_err.x, -rot_err.y, -rot_err.z, -rot_err.w};
                double half = std::acos(std::min(1.0, rot_err.w));
                double sn = std::sin(half);
                Vec3 rot_vec = sn > 1e-9 ? Vec3{rot_err.x, rot_err.y, rot_err.z} * (2.0 * half / sn) : Vec3{};
                side.assist_pos = side.assist_pos + pos_err * k;
                side.assist_rot = side.assist_rot + rot_vec * k;
                double max_pos = cfg.assist_max_cm, max_rot = cfg.assist_max_deg * kDegToRad;
                if (length(side.assist_pos) > max_pos) side.assist_pos = normalized(side.assist_pos) * max_pos;
                if (length(side.assist_rot) > max_rot) side.assist_rot = normalized(side.assist_rot) * max_rot;
                target_rel.t = target_rel.t + side.assist_pos;
                double ang = length(side.assist_rot);
                if (ang > 1e-6) target_rel.q = (quat_axis_angle(side.assist_rot * (1.0 / ang), ang) * target_rel.q).normalized();
            }
            else
            {
                side.assist_pos = {};
                side.assist_rot = {};
                side.assist_time = 0;
            }
            Xf target = compose(target_rel, parent_cs);
            Vec3 pull_shoulder;
            if (pull)
            {
                target = relative(wanted, s.body_world);
                pull_shoulder = relative(shoulder_world, s.body_world).t;
                // Never pull the hand further than the arm is long (it would drag the body).
                Vec3 from_shoulder = target.t - pull_shoulder;
                double far_cm = (side.arm_len > 10.0 ? side.arm_len : 60.0) * 1.05;
                if (length(from_shoulder) > far_cm) target.t = pull_shoulder + normalized(from_shoulder) * far_cm;
                side.last_world_target = compose(target, s.body_world).t;
                side.have_last_world = true;
            }
            else
            {
                side.have_last_world = false;
            }
            auto to_driver = [&](const Vec3& world_dir) {
                if (pull) return s.body_world.q.inverse().rotate(world_dir);
                return parent_cs.q.rotate(parent_world.q.inverse().rotate(world_dir));
            };

            Xf upper0 = pose[side.upper], lower0 = pose[side.lower], hand0 = pose[side.hand];
            double a = length(lower0.t - upper0.t), b = length(hand0.t - lower0.t);
            if (a < 1.0 || b < 1.0) return;
            Vec3 S = pull ? pull_shoulder : upper0.t;
            // Shoulder reach: like a real shoulder blade, the clavicle swings the shoulder towards a
            // hand that is beyond easy reach, by up to shoulder_reach_cm. The character's arms are
            // shorter than most players', so this lets the in-game hand get closer to the real one.
            if (!pull && side.clavicle >= 0 && cfg.shoulder_reach_cm > 0.0 && !(cfg.arm_scaling && cfg.player_arm_cm > 0.0))
            {
                Xf clav = pose[side.clavicle];
                Vec3 bone = S - clav.t;
                double len = length(bone);
                double excess = length(target.t - S) - cfg.reach_knee * (a + b);
                double want = std::max(0.0, std::min(cfg.shoulder_reach_cm, excess));
                double now = now_seconds();
                double dt = side.shoulder_time > 0 ? std::min(0.1, now - side.shoulder_time) : 0.0;
                side.shoulder_time = now;
                double alpha = dt > 0 ? 1.0 - std::exp(-dt / 0.08) : 1.0;
                side.shoulder_shift += (want - side.shoulder_shift) * alpha;
                if (len > 1.0 && side.shoulder_shift > 0.05)
                {
                    Vec3 moved = bone + normalized(target.t - S) * side.shoulder_shift;
                    Vec3 from = bone * (1.0 / len), to = normalized(moved);
                    Vec3 cr = cross(from, to);
                    double sn = length(cr), cs = dot(from, to);
                    if (sn > 1e-9)
                    {
                        Quat turn = quat_axis_angle(cr * (1.0 / sn), std::atan2(sn, cs));
                        clav.q = (turn * clav.q).normalized();
                        pose[side.clavicle] = clav;
                        changed[side.clavicle] = 1;
                        S = clav.t + turn.rotate(bone);
                    }
                }
            }
            // A physical hand (cut loose from the forearm, driven to the controller) leads: the arm
            // is drawn to where the hand body really is, while the hand bone of the pose carries the
            // controller's place for the drive to aim at.
            Vec3 reach_to = target.t;
            Xf hand_body;
            if (placed && !s.kin_hands && side.pull_active && side.wrist_freed && socket_transform(s.body, side.hand_name, RTS_World, hand_body))
                reach_to = relative(hand_body, s.body_world).t;
            Vec3 to_target = reach_to - S;
            double d = length(to_target);
            Vec3 dir = normalized(to_target, normalized(lower0.t - S));
            // Reach: a straight arm leaves the blade angle to the wrist alone, and the game's wrist
            // stops at about 78 degrees of bend. So the elbow is always kept a little bent: past
            // reach_knee of the full arm length the distance is eased towards reach_max.
            side.last_reach = d / (a + b);
            {
                double knee = std::max(0.3, std::min(0.98, cfg.reach_knee));
                double top = std::max(knee + 0.01, std::min(0.999, cfg.reach_max));
                double r = d / (a + b);
                if (r > knee) r = knee + (top - knee) * std::tanh((r - knee) / (top - knee));
                d = r * (a + b);
            }
            d = std::max(std::fabs(a - b) + 0.5, std::min(d, (a + b) * 0.999));

            // Elbow direction: down, out and back relative to the character's facing.
            Quat facing = quat_from_yaw_deg(s.actor_yaw);
            Vec3 pole_world = Vec3{0, 0, -cfg.elbow_down} + facing.rotate({-cfg.elbow_back, side.out_sign * cfg.elbow_out, 0});
            Vec3 pole = perpendicular_part(to_driver(pole_world), dir);
            if (length(pole) < 1e-3) pole = perpendicular_part(lower0.t - S, dir);
            Vec3 perp = normalized(pole, normalized(perpendicular_part({0, 0, -1}, dir)));
            double cos_a = std::max(-1.0, std::min(1.0, (a * a + d * d - b * b) / (2.0 * a * d)));
            double sin_a = std::sqrt(std::max(0.0, 1.0 - cos_a * cos_a));
            // The elbow's place around the shoulder-to-hand line decides which way the forearm
            // points and how it is rolled (the elbow is a hinge). Choose it so that the wrist needs
            // to bend less than wrist_bend_limit and roll less than twist_limit (the game's wrist
            // stops near 78 degrees of bend and 90 of roll), keeping it near its natural place and
            // near last frame's place otherwise.
            Vec3 side_axis = cross(dir, perp);
            Vec3 axis = side.lower_axis;
            Vec3 hand_dir = target.q.rotate(axis); // the hand's own lengthwise axis
            auto twist_of = [&](const Quat& lower_untwisted) {
                Quat rel = lower_untwisted.inverse() * target.q;
                double t = 2.0 * std::atan2(rel.x * axis.x + rel.y * axis.y + rel.z * axis.z, rel.w);
                if (t > kPi) t -= 2.0 * kPi;
                if (t < -kPi) t += 2.0 * kPi;
                return t;
            };
            auto arm_for = [&](double angle_deg, Vec3& elbow, Vec3& fore, Vec3& hinge_out, Quat& lower_untwisted) {
                double rad = angle_deg * kDegToRad;
                Vec3 p = normalized(perp * std::cos(rad) + side_axis * std::sin(rad));
                elbow = S + dir * (a * cos_a) + p * (a * sin_a);
                fore = normalized(S + dir * d - elbow);
                hinge_out = normalized(cross(p, dir));
                lower_untwisted = side.have_hinge ? (quat_from_frame(fore, hinge_out) * quat_from_frame(side.lower_axis, side.hinge_lower).inverse()).normalized() : Quat{};
            };
            if (side.have_hinge)
            {
                double best_cost = 1e30, best_angle = 0;
                double range = std::max(0.0, std::min(150.0, cfg.elbow_swing_max));
                for (double angle = -range; angle <= range + 1e-6; angle += 5.0)
                {
                    Vec3 e, fore, h;
                    Quat lu;
                    arm_for(angle, e, fore, h, lu);
                    double bend = std::acos(std::max(-1.0, std::min(1.0, dot(fore, hand_dir)))) * kRadToDeg;
                    double roll = std::fabs(twist_of(lu)) * kRadToDeg;
                    double over_bend = std::max(0.0, bend - cfg.wrist_bend_limit);
                    double over_roll = std::max(0.0, roll - cfg.twist_limit);
                    double moved = angle - side.elbow_angle;
                    double cost = over_bend * over_bend + over_roll * over_roll + cfg.elbow_stiffness * angle * angle + 0.01 * moved * moved;
                    if (cost < best_cost)
                    {
                        best_cost = cost;
                        best_angle = angle;
                    }
                }
                double now = now_seconds();
                double dt = side.elbow_time > 0 ? std::min(0.1, now - side.elbow_time) : 0.0;
                side.elbow_time = now;
                double alpha = dt > 0 ? 1.0 - std::exp(-dt / 0.06) : 1.0;
                side.elbow_angle += (best_angle - side.elbow_angle) * alpha;
            }
            Vec3 E, f, hinge;
            Quat lower_untwisted;
            arm_for(side.have_hinge ? side.elbow_angle : 0.0, E, f, hinge, lower_untwisted);
            Vec3 T = S + dir * d;
            Vec3 u = normalized(E - S);

            Quat upper_q, lower_q;
            Quat hand_q = target.q;
            if (side.have_hinge)
            {
                upper_q = quat_from_frame(u, hinge) * quat_from_frame(side.upper_axis, side.hinge_upper).inverse();
                // Hand roll about the forearm: kept continuous from frame to frame (no jump from
                // +180 to -180) and held within twist_clamp, so the arm never flips over.
                double twist = twist_of(lower_untwisted);
                while (twist - side.last_twist > kPi) twist -= 2.0 * kPi;
                while (twist - side.last_twist < -kPi) twist += 2.0 * kPi;
                double clamp = cfg.twist_clamp * kDegToRad;
                double sent_twist = std::max(-clamp, std::min(clamp, twist));
                side.last_twist = sent_twist;
                Quat rel = lower_untwisted.inverse() * target.q;
                double proj = rel.x * axis.x + rel.y * axis.y + rel.z * axis.z;
                Quat rel_twist = Quat{axis.x * proj, axis.y * proj, axis.z * proj, rel.w}.normalized();
                Quat rel_swing = rel * rel_twist.inverse();
                hand_q = (lower_untwisted * rel_swing * quat_axis_angle(axis, sent_twist)).normalized();
                double share = std::max(0.0, std::min(1.0, cfg.forearm_twist_share));
                lower_q = (lower_untwisted * quat_axis_angle(axis, sent_twist * share)).normalized();
            }
            else
            {
                // Hinge not learned yet: swing the animated bones the shortest way.
                auto swing = [](const Vec3& from, const Vec3& to) {
                    Vec3 cr = cross(from, to);
                    double sn = length(cr), c = dot(from, to);
                    if (sn < 1e-9) return Quat{};
                    return quat_axis_angle(cr * (1.0 / sn), std::atan2(sn, c));
                };
                upper_q = swing(normalized(lower0.t - S), u) * upper0.q;
                Vec3 lower_dir_now = (upper_q * upper0.q.inverse()).rotate(normalized(hand0.t - lower0.t));
                lower_q = swing(lower_dir_now, f) * (upper_q * upper0.q.inverse()) * lower0.q;
            }
            upper_q = upper_q.normalized();
            lower_q = lower_q.normalized();

            pose[side.upper] = {upper_q, S};
            pose[side.lower] = {lower_q, E};
            // The pose keeps the elbow a little bent (T); the pull takes the hand to the true place.
            // ... and, in the placed body, turned exactly like the controller: the wrist is free (or the
            // hand is simply set), so the roll limits that protect a simulated wrist do not apply.
            pose[side.hand] = {placed ? target.q : hand_q, pull ? target.t : T};
            changed[side.upper] = changed[side.lower] = changed[side.hand] = 1;

            // How far the physical hand is from where it is sent (for the log).
            Xf body_hand;
            if (socket_transform(s.body, side.hand_name, RTS_World, body_hand))
            {
                // Against last frame's target: that is what the physics has had time to reach.
                const Xf& asked = side.have_prev_wanted ? side.prev_wanted : wanted;
                side.last_error_cm = length(body_hand.t - asked.t);
                side.last_error_deg = quat_angle_deg(body_hand.q, asked.q);
            }
            side.turn_target = wanted.q;
            side.have_turn_target = placed && !s.kin_hands && side.wrist_freed && side.rescue_until == 0.0;
            if (placed && !s.kin_hands && side.wrist_freed)
            {
                double now = now_seconds();
                const wchar_t* bone = side.out_sign > 0 ? L"hand_r" : L"hand_l";
                if (side.rescue_until > 0.0)
                {
                    if (now >= side.rescue_until)
                    {
                        body_set_simulate(bone, true);
                        side.rescue_until = 0.0;
                        side.lost_frames = 0;
                    }
                }
                else
                {
                    // (For the log: the moment what the hand holds gets stuck, and in what.)
                    {
                        static int32_t stuck_was[2] = {0, 0};
                        int k = side.out_sign > 0 ? 1 : 0;
                        const TCHAR* held_name = k ? STR("Weapon R") : STR("Weapon L");
                        UObject* weapon = s.pawn->GetPropertyByNameInChain(held_name) ? object_property(s.pawn, held_name) : nullptr;
                        FProperty* list = weapon ? weapon->GetPropertyByNameInChain(STR("Stuck Constraints Array")) : nullptr;
                        struct ObjectArray { UObject** data; int32_t num; int32_t max; };
                        const ObjectArray* stuck = list ? reinterpret_cast<const ObjectArray*>(reinterpret_cast<uint8_t*>(weapon) + list->GetOffset_Internal()) : nullptr;
                        int32_t count = stuck && stuck->data ? stuck->num : 0;
                        if (count > 0 && stuck_was[k] == 0 && count < 32 && stuck->data[count - 1])
                        {
                            UObject* joint = stuck->data[count - 1];
                            UObject* in_actor = joint->GetPropertyByNameInChain(STR("Hit Actor")) ? object_property(joint, STR("Hit Actor")) : nullptr;
                            UObject* in_part = joint->GetPropertyByNameInChain(STR("Component 2 (Body)")) ? object_property(joint, STR("Component 2 (Body)")) : nullptr;
                            Xf joint_at;
                            bool have_at = false;
                            if (UObject* root = joint->GetPropertyByNameInChain(STR("RootComponent")) ? object_property(joint, STR("RootComponent")) : nullptr)
                                have_at = component_world(root, joint_at);
                            HSVR_LOG("Hands: what the %s hand holds got stuck in %ls (%ls, class %ls)%s | hand %.0f cm from the controller", side.label,
                                     in_actor ? in_actor->GetName().c_str() : L"?", in_part ? in_part->GetName().c_str() : L"?",
                                     in_actor ? in_actor->GetClassPrivate()->GetName().c_str() : L"?", in_actor == s.pawn ? " - the player's own character" : "", side.last_error_cm);
                            if (have_at)
                                HSVR_LOG("Hands:   the place it is stuck is %.0f cm from the hand, %.0f cm above the character's middle", length(joint_at.t - body_hand.t),
                                         joint_at.t.z - s.actor_world.z);
                        }
                        stuck_was[k] = count;
                    }
                    // A weapon stuck in something: pulled far enough, it comes free.
                    if (cfg.stuck_pull_free_cm > 0.0 && side.last_error_cm > cfg.stuck_pull_free_cm)
                    {
                        if (++side.behind_frames > 10) // (this runs twice a frame)
                        {
                            side.behind_frames = 0;
                            const TCHAR* stuck_property = side.out_sign > 0 ? STR("Weapon R") : STR("Weapon L");
                            UObject* weapon = s.pawn->GetPropertyByNameInChain(stuck_property) ? object_property(s.pawn, stuck_property) : nullptr;
                            FProperty* list = weapon ? weapon->GetPropertyByNameInChain(STR("Stuck Constraints Array")) : nullptr;
                            int32_t stuck = list ? *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(weapon) + list->GetOffset_Internal() + 8) : 0;
                            UFunction* release = stuck > 0 ? weapon->GetFunctionByNameInChain(STR("Release All Jammed Bones")) : nullptr;
                            if (release && release->GetParmsSize() == 0)
                            {
                                bool* in_body = weapon->GetValuePtrByPropertyNameInChain<bool>(STR("Stuck In Body"));
                                weapon->ProcessEvent(release, nullptr);
                                HSVR_LOG("Hands: what the %s hand holds was stuck in %s (%d places) and the hand had been pulled %.0f cm on; pulled free", side.label,
                                         in_body && *in_body ? "a body" : "something solid", stuck, side.last_error_cm);
                            }
                        }
                    }
                    else
                    {
                        side.behind_frames = 0;
                    }
                    // (For the log: where the hand is when it starts to fall behind.)
                    {
                        static double was[2] = {0, 0};
                        static Vec3 actor_was[2];
                        int k = side.out_sign > 0 ? 1 : 0;
                        if (side.have_prev_wanted && ((was[k] <= 30.0 && side.last_error_cm > 30.0) || (was[k] <= 100.0 && side.last_error_cm > 100.0)) &&
                            log_throttle(k ? "behind_r" : "behind_l", 2.0)) // (it searches the level: seldom)
                        {
                            Vec3 off = body_hand.t - side.prev_wanted.t, moved = s.actor_world - actor_was[k];
                            std::vector<UObject*> things;
                            UObjectGlobals::FindAllOf(STR("ModularWeaponBP_C"), things);
                            UObject* nearest = nullptr;
                            double nearest_cm = 1e9;
                            for (UObject* thing : things)
                            {
                                UObject* base = thing && thing->GetPropertyByNameInChain(STR("BaseMesh")) ? object_property(thing, STR("BaseMesh")) : nullptr;
                                bool* is_held = thing ? thing->GetValuePtrByPropertyNameInChain<bool>(STR("Is Held")) : nullptr;
                                Xf where;
                                if (!base || (is_held && *is_held) || !component_world(base, where)) continue;
                                double away = length(where.t - body_hand.t);
                                if (away < nearest_cm) nearest_cm = away, nearest = thing;
                            }
                            const TCHAR* held_property = side.out_sign > 0 ? STR("Weapon R") : STR("Weapon L");
                            UObject* held = s.pawn->GetPropertyByNameInChain(held_property) ? object_property(s.pawn, held_property) : nullptr;
                            UObject* held_base = held && held->GetPropertyByNameInChain(STR("BaseMesh")) ? object_property(held, STR("BaseMesh")) : nullptr;
                            float* held_mass = s.pawn->GetValuePtrByPropertyNameInChain<float>(side.out_sign > 0 ? STR("R Weapon Mass") : STR("L Weapon Mass"));
                            Xf held_at;
                            bool have_held = held_base && component_world(held_base, held_at);
                            FProperty* stuck_list = held ? held->GetPropertyByNameInChain(STR("Stuck Constraints Array")) : nullptr;
                            int32_t stuck_places = stuck_list ? *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(held) + stuck_list->GetOffset_Internal() + 8) : -1;
                            HSVR_LOG("Hands: the %s hand is falling behind: %.0f cm from the controller (%.0f, %.0f, %.0f in the world) | hand %.0f cm above the character's middle | the character moved (%.1f, %.1f, %.1f) this frame | nearest thing not held: %ls, %.0f cm from the hand | holds %ls (mass %.1f), its middle %.0f cm from the hand and %.0f cm above the character's middle, stuck in %d places",
                                     side.label, side.last_error_cm, off.x, off.y, off.z, body_hand.t.z - s.actor_world.z, moved.x, moved.y, moved.z,
                                     nearest ? nearest->GetName().c_str() : L"none", nearest ? nearest_cm : -1.0, held ? held->GetName().c_str() : L"nothing",
                                     held_mass ? (double)*held_mass : -1.0, have_held ? length(held_at.t - body_hand.t) : -1.0, have_held ? held_at.t.z - s.actor_world.z : 0.0, stuck_places);
                        }
                        was[k] = side.last_error_cm;
                        actor_was[k] = s.actor_world;
                    }
                    side.lost_frames = side.last_error_cm > 100.0 ? side.lost_frames + 1 : 0;
                    if (side.lost_frames > 20)
                    {
                        body_set_simulate(bone, false);
                        side.rescue_until = now + 0.15;
                        ++side.rescues;
                        HSVR_LOG("Hands: the %s hand was %.0f cm from its controller for a while; brought back to it", side.label, side.last_error_cm);
                    }
                }
            }
            side.prev_wanted = wanted;
            side.have_prev_wanted = true;
            (void)anim;
        }

        // ---- pulling the hands (game thread) ---------------------------------------------------
        // Half Sword's muscles are the engine's physical animation: for every body part a spring
        // from the simulated body to its place in the animated pose. The game only uses the part
        // of that spring that turns a body (orientation strength), so a hand is never pulled to a
        // place; it ends up wherever the chain of turned bones leaves it, and sags under the sword.
        // Here the two hand bodies also get the positional part of the same spring (in the world,
        // not relative to the forearm), with the pose's hand bone written at the controller's
        // place. The engine keeps its settings per body in an array that is not reflected
        // (UPhysicalAnimationComponent::DriveData); it is found by its shape, read to learn the
        // game's own values, and changed only through the engine's own function
        // ApplyPhysicalAnimationSettings. The game's values come back whenever the pull is off.
        struct RawBytesArray
        {
            uint8_t* data;
            int32_t num;
            int32_t max;
        };
        struct alignas(16) BodyTargetParams
        {
            FName BodyName;
            uint8_t pad[8];
            RawXf ReturnValue;
        };

        struct PullLayout
        {
            UObject* pawn = nullptr;
            UObject* component = nullptr;
            UFunction* apply = nullptr;
            UFunction* target_fn = nullptr;
            bool ok = false;
            int tries = 0;
            double next_try = 0;
            int32_t parms = 0, parm_body = -1, parm_data = -1, size = 0;
            int32_t f_body = -1, f_local = -1, f_orient = -1, f_angvel = -1, f_pos = -1, f_vel = -1, f_maxlin = -1, f_maxang = -1;
            size_t drive_offset = 0;
            uint64_t applies = 0;
        };
        PullLayout g_pull;
        // The bodies whose settings the mod changes: the two hands (held at the controllers), the
        // other bones of each arm (softened, so the arm follows the hand), and the head (held at
        // the headset).
        enum class PullKind
        {
            hand,
            arm,
            head,
            other
        };
        struct PullBody
        {
            FName name;
            PullKind kind;
            int side; // index into State::side, or -1
            bool have_written = false;
            bool active = false;
            std::vector<uint8_t> written, base;
        };
        std::vector<PullBody> g_pull_bodies;

        float pull_get(const uint8_t* entry, int32_t offset)
        {
            float v;
            memcpy(&v, entry + offset, sizeof(v));
            return v;
        }
        void pull_set(uint8_t* entry, int32_t offset, float v) { memcpy(entry + offset, &v, sizeof(v)); }
        bool pull_same(const uint8_t* a, const uint8_t* b)
        {
            const PullLayout& L = g_pull;
            if ((a[L.f_local] & 1) != (b[L.f_local] & 1)) return false;
            for (int32_t off : {L.f_orient, L.f_angvel, L.f_pos, L.f_vel, L.f_maxlin, L.f_maxang})
            {
                if (pull_get(a, off) != pull_get(b, off)) return false;
            }
            return true;
        }

        void pull_setup()
        {
            State& s = g_state;
            PullLayout& L = g_pull;
            int tries = L.pawn == s.pawn ? L.tries + 1 : 1;
            L = PullLayout{};
            L.pawn = s.pawn;
            L.tries = tries;
            L.next_try = now_seconds() + (tries <= 3 ? 0.3 : 2.0);
            g_pull_bodies.clear();
            bool last = tries >= 12;
            UObject* component = object_property(s.pawn, STR("PhysicalAnimation"));
            UFunction* apply = component ? component->GetFunctionByNameInChain(STR("ApplyPhysicalAnimationSettings")) : nullptr;
            if (!apply)
            {
                if (last) HSVR_LOG("Pull: the character has no physical animation component to ask; hand pull stays off");
                return;
            }
            for (FProperty* p : apply->ForEachProperty())
            {
                std::wstring name = p->GetName();
                if (name == L"BodyName") L.parm_body = p->GetOffset_Internal();
                if (name != L"PhysicalAnimationData") continue;
                L.parm_data = p->GetOffset_Internal();
                L.size = p->GetSize();
                if (p->GetClass().GetName() != STR("StructProperty")) continue;
                UScriptStruct* st = static_cast<FStructProperty*>(p)->GetStruct();
                if (!st) continue;
                for (FProperty* q : st->ForEachProperty())
                {
                    std::wstring field = q->GetName();
                    int32_t off = q->GetOffset_Internal();
                    if (field == L"BodyName") L.f_body = off;
                    else if (field == L"bIsLocalSimulation") L.f_local = off;
                    else if (field == L"OrientationStrength") L.f_orient = off;
                    else if (field == L"AngularVelocityStrength") L.f_angvel = off;
                    else if (field == L"PositionStrength") L.f_pos = off;
                    else if (field == L"VelocityStrength") L.f_vel = off;
                    else if (field == L"MaxLinearForce") L.f_maxlin = off;
                    else if (field == L"MaxAngularForce") L.f_maxang = off;
                }
            }
            L.parms = apply->GetParmsSize();
            bool fields = L.f_body >= 0 && L.f_local >= 0 && L.f_orient >= 0 && L.f_angvel >= 0 && L.f_pos >= 0 && L.f_vel >= 0 && L.f_maxlin >= 0 && L.f_maxang >= 0;
            if (!fields || L.parm_body < 0 || L.parm_data < 0 || L.size < 32 || L.size > 64 || L.parms > 200 || L.parm_data + L.size > L.parms ||
                L.f_maxang + 4 > L.size || L.f_body + (int32_t)sizeof(FName) > L.size)
            {
                if (last) HSVR_LOG("Pull: the engine's muscle settings are not laid out as expected (size %d, call %d bytes); hand pull stays off", L.size, L.parms);
                return;
            }
            FProperty* mesh_property = component->GetPropertyByNameInChain(STR("SkeletalMeshComponent"));
            if (!mesh_property) return;
            size_t after_mesh = (size_t)mesh_property->GetOffset_Internal() + sizeof(void*);
            UObject* driven = *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(component) + mesh_property->GetOffset_Internal());
            if (driven != s.body)
            {
                if (last) HSVR_LOG("Pull: the character's muscles do not work on its body mesh; hand pull stays off");
                return;
            }
            // The settings array: one entry per driven body, each starting with a bone's name.
            auto* base = reinterpret_cast<uint8_t*>(component);
            for (size_t off = after_mesh; off <= after_mesh + 80 && !L.drive_offset; off += 8)
            {
                RawBytesArray arr;
                if (!safe_copy(&arr, base + off, sizeof(arr))) continue;
                if (!arr.data || arr.num < 2 || arr.num > 400 || arr.max < arr.num || arr.max > 4096) continue;
                std::vector<uint8_t> copy((size_t)arr.num * L.size);
                if (!safe_copy(copy.data(), arr.data, copy.size())) continue;
                bool all_bones = true, has_hand = false;
                for (int32_t i = 0; i < arr.num && all_bones; ++i)
                {
                    FName name;
                    memcpy(&name, copy.data() + (size_t)i * L.size + L.f_body, sizeof(FName));
                    if (driver_bone_index(name) < 0) all_bones = false;
                    if (memcmp(&name, &s.side[1].hand_name, sizeof(FName)) == 0) has_hand = true;
                }
                if (!all_bones || !has_hand) continue;
                L.drive_offset = off;
                float multiplier = 1.0f;
                if (float* m = component->GetValuePtrByPropertyNameInChain<float>(STR("StrengthMultiplyer"))) multiplier = *m;
                HSVR_LOG("Pull: the game's muscle settings found (%d bodies, at 0x%zX, all strengths times %.2f):", arr.num, off, multiplier);
                for (int32_t i = 0; i < arr.num; ++i)
                {
                    const uint8_t* e = copy.data() + (size_t)i * L.size;
                    FName name;
                    memcpy(&name, e + L.f_body, sizeof(FName));
                    HSVR_LOG("Pull:   %-14ls %s turn %.0f / %.0f, place %.0f / %.0f, limits %.0f / %.0f", name.ToString().c_str(),
                             (e[L.f_local] & 1) ? "relative to its parent," : "in the world,         ", pull_get(e, L.f_orient), pull_get(e, L.f_angvel), pull_get(e, L.f_pos),
                             pull_get(e, L.f_vel), pull_get(e, L.f_maxlin), pull_get(e, L.f_maxang));
                }
            }
            if (!L.drive_offset)
            {
                if (last) HSVR_LOG("Pull: could not find the game's muscle settings; hand pull stays off");
                return;
            }
            {
                // Every body the game drives, sorted into hands, arm bones, the head and the rest.
                RawBytesArray arr = *reinterpret_cast<RawBytesArray*>(base + L.drive_offset);
                FName head_name = make_name(L"head");
                for (int32_t i = 0; i < arr.num; ++i)
                {
                    FName name;
                    memcpy(&name, arr.data + (size_t)i * L.size + L.f_body, sizeof(FName));
                    PullBody body{name, PullKind::other, -1};
                    for (int k = 0; k < 2; ++k)
                    {
                        Side& side = s.side[k];
                        FName clavicle = side_name(L"clavicle", side);
                        if (memcmp(&name, &side.hand_name, sizeof(FName)) == 0) body.kind = PullKind::hand, body.side = k;
                        else if (memcmp(&name, &side.lower_name, sizeof(FName)) == 0 || memcmp(&name, &side.upper_name, sizeof(FName)) == 0 ||
                                 memcmp(&name, &clavicle, sizeof(FName)) == 0)
                            body.kind = PullKind::arm, body.side = k;
                    }
                    if (memcmp(&name, &head_name, sizeof(FName)) == 0) body.kind = PullKind::head;
                    g_pull_bodies.push_back(body);
                }
            }
            {
                FProperty* tick_property = component->GetPropertyByNameInChain(STR("PrimaryComponentTick"));
                std::lock_guard<std::mutex> lock(g_hook_mutex);
                if (tick_property && hook_component_tick(component))
                {
                    g_muscle_tick = reinterpret_cast<uint8_t*>(component) + tick_property->GetOffset_Internal();
                    HSVR_LOG("Pull: the hand settings are written right before the muscles' own update");
                }
                else
                {
                    g_muscle_tick = nullptr;
                    HSVR_LOG("Pull: the muscles' update could not be hooked; hand settings are written from the body's update only");
                }
            }
            L.component = component;
            L.apply = apply;
            UFunction* target_fn = component->GetFunctionByNameInChain(STR("GetBodyTargetTransform"));
            if (target_fn && target_fn->GetParmsSize() == (int32_t)sizeof(BodyTargetParams)) L.target_fn = target_fn;
            L.ok = true;
        }

        // `arms` true from inside the mesh tick (after the game's own script has run this frame and
        // before the engine uses the settings); false gives the game's values back. The game
        // writes its own values again every frame, so ours are written after it every frame.
        void pull_update(bool arms)
        {
            State& s = g_state;
            PullLayout& L = g_pull;
            if (!s.pawn || !s.ready) return;
            Config cfg = config();
            if (L.pawn != s.pawn || (!L.ok && L.tries < 12 && now_seconds() >= L.next_try))
            {
                if (!cfg.hand_pull && !cfg.body_follow && L.pawn != s.pawn) return; // never looked, nothing to give back
                pull_setup();
            }
            for (Side& side : s.side) side.pull_active = false;
            s.head_pull_active = false;
            if (!L.ok)
            {
                s.hold_active = false;
                return;
            }
            RawBytesArray arr = *reinterpret_cast<RawBytesArray*>(reinterpret_cast<uint8_t*>(L.component) + L.drive_offset);
            if (!arr.data || arr.num <= 0 || arr.num > 400) return;
            auto hand_wanted = [&](int k) {
                const Side& side = s.side[k];
                bool wanted_side = side.out_sign > 0 ? cfg.arm_right : cfg.arm_left;
                return arms && cfg.hand_pull && cfg.hand_pull_strength > 0.0 && wanted_side && side.target_valid && s.have_body_world;
            };
            for (PullBody& body : g_pull_bodies)
            {
                uint8_t* entry = nullptr;
                for (int32_t i = 0; i < arr.num && !entry; ++i)
                {
                    uint8_t* e = arr.data + (size_t)i * L.size;
                    if (memcmp(e + L.f_body, &body.name, sizeof(FName)) == 0) entry = e;
                }
                if (!entry)
                {
                    body.active = false;
                    continue;
                }
                // Anything other than what we last wrote is the game's own (new) setting.
                if (!body.have_written || !pull_same(entry, body.written.data())) body.base.assign(entry, entry + L.size);
                const uint8_t* base = body.base.data();
                std::vector<uint8_t> desired = body.base;
                uint8_t* d = desired.data();
                bool want = false;
                auto scale_turn = [&](double scale) {
                    pull_set(d, L.f_orient, (float)(pull_get(base, L.f_orient) * scale));
                    pull_set(d, L.f_angvel, (float)(pull_get(base, L.f_angvel) * std::sqrt(scale)));
                    pull_set(d, L.f_maxang, (float)(pull_get(base, L.f_maxang) * scale));
                };
                if (s.hold_wanted)
                {
                    // The held body: every part is held at its place in the pose, in place and turn.
                    want = true;
                    d[L.f_local] &= 0xFE;
                    double scale = std::max(0.1, std::min(50.0, cfg.hold_turn_scale));
                    pull_set(d, L.f_orient, (float)(pull_get(base, L.f_orient) * scale));
                    pull_set(d, L.f_angvel, (float)(pull_get(base, L.f_angvel) * std::sqrt(scale)));
                    pull_set(d, L.f_maxang, 0.0f); // no torque limit
                    // The feet are not held in place, only turned: they stand on the ground, and a stiff
                    // spring pressing a foot into the floor would fight the floor. The shins carry them.
                    static const FName foot_r = make_name(L"foot_r"), foot_l = make_name(L"foot_l");
                    bool foot = memcmp(&body.name, &foot_r, sizeof(FName)) == 0 || memcmp(&body.name, &foot_l, sizeof(FName)) == 0;
                    pull_set(d, L.f_pos, foot ? 0.0f : (float)std::max(0.0, cfg.hold_strength));
                    pull_set(d, L.f_vel, foot ? 0.0f : (float)std::max(0.0, cfg.hold_damping));
                    pull_set(d, L.f_maxlin, (float)std::max(0.0, cfg.hold_max_force));
                }
                else if (body.kind == PullKind::hand)
                {
                    want = hand_wanted(body.side);
                    if (want)
                    {
                        d[L.f_local] &= 0xFE;
                        scale_turn(std::max(0.1, std::min(20.0, cfg.hand_turn_scale)));
                        pull_set(d, L.f_pos, (float)cfg.hand_pull_strength);
                        pull_set(d, L.f_vel, (float)std::max(0.0, cfg.hand_pull_damping));
                        pull_set(d, L.f_maxlin, (float)std::max(0.0, cfg.hand_pull_max_force));
                    }
                }
                else if (body.kind == PullKind::arm)
                {
                    want = !s.kin_active && hand_wanted(body.side) && cfg.arm_soft_scale != 1.0;
                    if (want) scale_turn(std::max(0.02, std::min(5.0, cfg.arm_soft_scale)));
                }
                else if (body.kind == PullKind::head)
                {
                    want = !s.kin_active && s.head_pull_wanted && cfg.head_pull_strength > 0.0;
                    if (want)
                    {
                        d[L.f_local] &= 0xFE;
                        pull_set(d, L.f_pos, (float)cfg.head_pull_strength);
                        pull_set(d, L.f_vel, (float)std::max(0.0, cfg.head_pull_damping));
                        pull_set(d, L.f_maxlin, (float)std::max(0.0, cfg.head_pull_max_force));
                    }
                }
                if (!pull_same(entry, d))
                {
                    alignas(16) uint8_t parms[208] = {};
                    memcpy(parms + L.parm_body, &body.name, sizeof(FName));
                    memcpy(parms + L.parm_data, d, L.size);
                    L.component->ProcessEvent(L.apply, parms);
                    ++L.applies;
                    ++s.pull_applies_second;
                }
                body.written = desired;
                body.have_written = true;
                body.active = want;
                if (body.kind == PullKind::hand) s.side[body.side].pull_active = want;
                if (body.kind == PullKind::head) s.head_pull_active = want;
            }
            if (s.hold_wanted != s.hold_active)
            {
                s.hold_active = s.hold_wanted;
                HSVR_LOG(s.hold_active ? "Body: every part of the body is now held at its place (still physical; %d bodies)" : "Body: the body's own muscles are the game's again (%d bodies)",
                         (int)g_pull_bodies.size());
            }
        }

        // Once a second: where the engine itself says the hand's spring is anchored, against where
        // we meant to put it (checks the whole chain from the written pose to the physics).
        void pull_check()
        {
            State& s = g_state;
            PullLayout& L = g_pull;
            for (Side& side : s.side)
            {
                side.pull_check_cm = -1;
                if (!L.ok || !L.target_fn || !side.pull_active || !side.have_last_world) continue;
                BodyTargetParams p{};
                p.BodyName = side.hand_name;
                L.component->ProcessEvent(L.target_fn, &p);
                side.pull_check_cm = length(from_raw(p.ReturnValue).t - side.last_world_target);
            }
        }

        // ---- the placed body ----------------------------------------------------------------------
        // Matthew's reference is Blade & Sorcery: there the player's body has no muscles or balance
        // of its own, it is put where the player is. Half Sword's player is a simulated ragdoll like
        // its enemies. Here the simulation is switched off for the player's body (its physics
        // bodies become kinematic and follow the pose exactly), except for the hands and what they
        // hold, so weapons keep their weight against the wrist and grip. Hits still land on the
        // body's shapes; it just is not thrown about by them. When the character is knocked out
        // or dies the body is given back to the game's simulation.
        struct BelowSimulateParams
        {
            FName InBoneName;
            bool bNewSimulate;
            bool bIncludeSelf;
            uint8_t pad[6];
        };
        struct BoneBoolParams
        {
            FName BoneName;
            bool ReturnValue;
            uint8_t pad[7];
        };

        bool body_simulating(const wchar_t* bone)
        {
            State& s = g_state;
            UFunction* fn = s.body ? s.body->GetFunctionByNameInChain(STR("IsSimulatingPhysics")) : nullptr;
            if (!fn || fn->GetParmsSize() > (int32_t)sizeof(BoneBoolParams)) return false;
            BoneBoolParams p{};
            p.BoneName = make_name(bone);
            s.body->ProcessEvent(fn, &p);
            return p.ReturnValue;
        }

        bool body_set_simulate(const wchar_t* bone, bool simulate)
        {
            State& s = g_state;
            UFunction* fn = s.body ? s.body->GetFunctionByNameInChain(STR("SetAllBodiesBelowSimulatePhysics")) : nullptr;
            if (!fn || fn->GetParmsSize() > (int32_t)sizeof(BelowSimulateParams)) return false;
            BelowSimulateParams p{};
            p.InBoneName = make_name(bone);
            p.bNewSimulate = simulate;
            p.bIncludeSelf = true;
            s.body->ProcessEvent(fn, &p);
            return true;
        }

        // Taking a body out of a ragdoll, the way engines normally do it: once its bodies no longer
        // simulate, the mesh is put back on the character (its capsule) at its standard place, so it
        // moves with the character like any animated body and its hit shapes follow the pose. When
        // simulation started, the engine had detached the mesh; left detached it stays where it was,
        // which is how the first attempt left the arms behind.
        int32_t param_offset(UFunction* fn, const wchar_t* name)
        {
            for (FProperty* p : fn->ForEachProperty())
            {
                if (p->GetName() == name) return p->GetOffset_Internal();
            }
            return -1;
        }

        bool body_attached_to_capsule(UObject** capsule_out = nullptr)
        {
            State& s = g_state;
            UObject* capsule = object_property(s.pawn, STR("CapsuleComponent"));
            if (capsule_out) *capsule_out = capsule;
            if (!capsule || !s.body->GetPropertyByNameInChain(STR("AttachParent"))) return false;
            return object_property(s.body, STR("AttachParent")) == capsule;
        }

        bool body_reattach()
        {
            State& s = g_state;
            UObject* capsule = nullptr;
            if (body_attached_to_capsule(&capsule)) return true;
            if (!capsule) return false;
            UFunction* attach = s.body->GetFunctionByNameInChain(STR("K2_AttachToComponent"));
            UFunction* place = s.body->GetFunctionByNameInChain(STR("K2_SetRelativeLocationAndRotation"));
            ue::FVector* offset = s.pawn->GetValuePtrByPropertyNameInChain<ue::FVector>(STR("BaseTranslationOffset"));
            double* turn = s.pawn->GetValuePtrByPropertyNameInChain<double>(STR("BaseRotationOffset")); // a quaternion: x y z w
            if (!attach || !place || !offset || !turn) return false;
            int32_t a_parent = param_offset(attach, L"Parent"), a_loc = param_offset(attach, L"LocationRule"), a_rot = param_offset(attach, L"RotationRule"),
                    a_scale = param_offset(attach, L"ScaleRule");
            int32_t p_loc = param_offset(place, L"NewLocation"), p_rot = param_offset(place, L"NewRotation"), p_teleport = param_offset(place, L"bTeleport");
            if (a_parent < 0 || a_loc < 0 || a_rot < 0 || a_scale < 0 || p_loc < 0 || p_rot < 0 || p_teleport < 0 || attach->GetParmsSize() > 128 ||
                place->GetParmsSize() > 1024)
                return false;
            {
                alignas(16) uint8_t parms[128] = {};
                memcpy(parms + a_parent, &capsule, sizeof(capsule));
                parms[a_loc] = 2; // snap to the capsule
                parms[a_rot] = 2;
                parms[a_scale] = 1; // keep the size
                s.body->ProcessEvent(attach, parms);
            }
            {
                alignas(16) uint8_t parms[1024] = {};
                Rot r = quat_to_rot(Quat{turn[0], turn[1], turn[2], turn[3]}.normalized());
                ue::FRotator rotation{r.pitch, r.yaw, r.roll};
                memcpy(parms + p_loc, offset, sizeof(ue::FVector));
                memcpy(parms + p_rot, &rotation, sizeof(rotation));
                parms[p_teleport] = 1;
                s.body->ProcessEvent(place, parms);
            }
            return body_attached_to_capsule();
        }

        // ---- physical hands: the wrist joint set free ---------------------------------------------
        // The engine's own constraint functions (ConstraintInstanceBlueprintLibrary) are used: the
        // joint between forearm and hand is read, kept, and set free in place and turn; put back
        // exactly as it was when the body is given back to the game.
        void copy_params_by_name(UFunction* from_fn, const uint8_t* from, UFunction* to_fn, uint8_t* to)
        {
            for (FProperty* p : to_fn->ForEachProperty())
            {
                std::wstring name = p->GetName();
                for (FProperty* q : from_fn->ForEachProperty())
                {
                    if (q->GetName() == name && q->GetSize() == p->GetSize()) memcpy(to + p->GetOffset_Internal(), from + q->GetOffset_Internal(), p->GetSize());
                }
            }
        }

        // Returns true when the wrist of `side` is in the wanted state afterwards.
        bool wrist_set_free(Side& side, bool free)
        {
            State& s = g_state;
            if (!free && !side.wrist_freed) return true;
            static UObject* library = nullptr;
            if (!library) library = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__ConstraintInstanceBlueprintLibrary"));
            UFunction* by_name = s.body ? s.body->GetFunctionByNameInChain(STR("GetConstraintByName")) : nullptr;
            if (!library || !by_name) return false;
            UFunction* get_lin = library->GetFunctionByNameInChain(STR("GetLinearLimits"));
            UFunction* set_lin = library->GetFunctionByNameInChain(STR("SetLinearLimits"));
            UFunction* get_ang = library->GetFunctionByNameInChain(STR("GetAngularLimits"));
            UFunction* set_ang = library->GetFunctionByNameInChain(STR("SetAngularLimits"));
            if (!get_lin || !set_lin || !get_ang || !set_ang) return false;
            constexpr int32_t kMax = 256;
            for (UFunction* fn : {by_name, get_lin, set_lin, get_ang, set_ang})
            {
                if (fn->GetParmsSize() > kMax) return false;
            }
            // The joint that ties the hand to its parent body, whatever it is called: the engine
            // lists a body's joints (GetConstraintsFromBody, "parent" joints = the body is the child).
            UFunction* from_body = s.body->GetFunctionByNameInChain(STR("GetConstraintsFromBody"));
            if (!from_body || from_body->GetParmsSize() > kMax) return false;
            int32_t accessor_size = 0;
            for (FProperty* p : get_lin->ForEachProperty())
            {
                if (p->GetName() == L"Accessor") accessor_size = p->GetSize();
            }
            int32_t b_name = param_offset(from_body, L"BodyName"), b_parent = param_offset(from_body, L"bParentConstraints"),
                    b_child = param_offset(from_body, L"bChildConstraints"), b_out = param_offset(from_body, L"OutConstraints");
            if (b_name < 0 || b_parent < 0 || b_child < 0 || b_out < 0 || accessor_size <= 0 || accessor_size > 64) return false;
            alignas(16) uint8_t found[64] = {};
            bool have_joint = false;
            {
                // By name: the game's physics asset calls the wrists UserConstraint_12 (hand_r to
                // lowerarm_r) and UserConstraint_16 (hand_l to lowerarm_l); see docs/game-internals.md.
                int32_t n_name = param_offset(by_name, L"ConstraintName"), n_ret = param_offset(by_name, L"ReturnValue");
                if (n_name >= 0 && n_ret >= 0)
                {
                    alignas(16) uint8_t parms[kMax] = {};
                    FName joint = make_name(side.out_sign > 0 ? L"UserConstraint_12" : L"UserConstraint_16");
                    memcpy(parms + n_name, &joint, sizeof(FName));
                    s.body->ProcessEvent(by_name, parms);
                    memcpy(found, parms + n_ret, accessor_size);
                    // A joint that exists is locked in place; the engine answers an unknown name with zeros.
                    alignas(16) uint8_t probe[kMax] = {};
                    int32_t at = param_offset(get_lin, L"Accessor");
                    if (at >= 0)
                    {
                        memcpy(probe + at, found, accessor_size);
                        library->ProcessEvent(get_lin, probe);
                        for (FProperty* p : get_lin->ForEachProperty())
                        {
                            if (p->GetName().find(L"Motion") != std::wstring::npos && probe[p->GetOffset_Internal()] != 0) have_joint = true;
                        }
                        if (side.wrist_freed) have_joint = true; // freed by us: it reads as free now
                    }
                }
            }
            if (!have_joint)
            {
                // Otherwise whatever joint the engine lists as tying the hand to its parent.
                alignas(16) uint8_t listing[kMax] = {};
                memcpy(listing + b_name, &side.hand_name, sizeof(FName));
                listing[b_parent] = 1;
                listing[b_child] = 0;
                s.body->ProcessEvent(from_body, listing);
                struct
                {
                    uint8_t* data;
                    int32_t num;
                    int32_t max;
                } joints;
                memcpy(&joints, listing + b_out, sizeof(joints)); // the engine's array; its few bytes are left to the engine
                if (!joints.data || joints.num < 1 || joints.num > 8)
                {
                    if (free) HSVR_LOG("Hands: the engine lists %d joints tying the %s hand to its arm; expected 1", joints.data ? joints.num : 0, side.label);
                    return false;
                }
                memcpy(found, joints.data, accessor_size);
            }
            const uint8_t* accessor = found;
            auto put_accessor = [&](UFunction* fn, uint8_t* parms) {
                int32_t at = param_offset(fn, L"Accessor");
                if (at < 0) return false;
                memcpy(parms + at, accessor, accessor_size);
                return true;
            };
            auto motions = [&](UFunction* fn, const uint8_t* parms) {
                std::string text;
                for (FProperty* p : fn->ForEachProperty())
                {
                    if (p->GetName().find(L"Motion") != std::wstring::npos) text += std::to_string((int)parms[p->GetOffset_Internal()]) + " ";
                }
                return text;
            };
            if (free && !side.wrist_freed)
            {
                alignas(16) uint8_t lin[kMax] = {}, ang[kMax] = {};
                if (!put_accessor(get_lin, lin) || !put_accessor(get_ang, ang)) return false;
                library->ProcessEvent(get_lin, lin);
                library->ProcessEvent(get_ang, ang);
                HSVR_LOG("Hands: %s wrist joint as the game made it: place %s(0 free, 1 limited, 2 locked), turn %s", side.label, motions(get_lin, lin).c_str(),
                         motions(get_ang, ang).c_str());
                // A real wrist is locked in place. All free means the joint was not found (the
                // engine then answers with zeros), and nothing would be freed.
                bool held = false;
                for (FProperty* p : get_lin->ForEachProperty())
                {
                    if (p->GetName().find(L"Motion") != std::wstring::npos && lin[p->GetOffset_Internal()] != 0) held = true;
                }
                if (!held)
                {
                    HSVR_LOG("Hands: no %s wrist joint found under the hand bone's name", side.label);
                    return false;
                }
                side.wrist_linear.assign(lin, lin + get_lin->GetParmsSize());
                side.wrist_angular.assign(ang, ang + get_ang->GetParmsSize());
            }
            if (side.wrist_linear.empty() || side.wrist_angular.empty()) return false;
            alignas(16) uint8_t lin[kMax] = {}, ang[kMax] = {};
            copy_params_by_name(get_lin, side.wrist_linear.data(), set_lin, lin);
            copy_params_by_name(get_ang, side.wrist_angular.data(), set_ang, ang);
            if (!put_accessor(set_lin, lin) || !put_accessor(set_ang, ang)) return false;
            if (free)
            {
                for (UFunction* fn : {set_lin, set_ang})
                {
                    uint8_t* parms = fn == set_lin ? lin : ang;
                    for (FProperty* p : fn->ForEachProperty())
                    {
                        if (p->GetName().find(L"Motion") != std::wstring::npos) parms[p->GetOffset_Internal()] = 0; // free
                    }
                }
            }
            library->ProcessEvent(set_lin, lin);
            library->ProcessEvent(set_ang, ang);
            // Read back: did it take?
            alignas(16) uint8_t check[kMax] = {};
            put_accessor(get_lin, check);
            library->ProcessEvent(get_lin, check);
            bool is_free = true;
            for (FProperty* p : get_lin->ForEachProperty())
            {
                if (p->GetName().find(L"Motion") != std::wstring::npos && check[p->GetOffset_Internal()] != 0) is_free = false;
            }
            if (free && !is_free) return false;
            side.wrist_freed = free;
            HSVR_LOG(free ? "Hands: %s hand cut loose from the forearm (wrist joint free)" : "Hands: %s wrist joint put back as the game made it", side.label);
            return true;
        }

        // ---- what a hand holds is fixed in it -------------------------------------------------------
        // The game hangs a weapon on the hand with its own joint (the pawn's "PhysicsConstraint R/L
        // Hand"): place locked, but swing and twist only softly limited (stiffness 50) and the joint
        // breakable. That is what let the weapon bounce about. With physical hands the joint is locked
        // hard in turn as well, its soft limits and breaking switched off, so hand and weapon move as
        // one object, stopped together by whatever the weapon hits. The game's values are kept and
        // put back.
        const wchar_t* const kGripSettings[][2] = {
            {L"GetLinearLimits", L"SetLinearLimits"},
            {L"GetAngularLimits", L"SetAngularLimits"},
            {L"GetAngularSoftSwingLimitParams", L"SetAngularSoftSwingLimitParams"},
            {L"GetAngularSoftTwistLimitParams", L"SetAngularSoftTwistLimitParams"},
            {L"GetLinearBreakable", L"SetLinearBreakable"},
            {L"GetAngularBreakable", L"SetAngularBreakable"},
        };

        // Matthew's point 2: "i cant raise and angle the shield". The game joins a weapon to the
        // hand bone, but for grip types 6 and 7 (strapped shields) to the FOREARM bone
        // (`Set Up Left Hand Weapon` @14076..@14301: lowerarm_l when `Grip L Hand Default` is 6
        // or 7). In the placed body the forearm is drawn by the arm's IK to the hand's place only:
        // the hand's turn never reaches it, and the strap sits a forearm's length below the fist.
        // So the shield is moved to sit on the hand as it sits on the forearm with a straight wrist
        // (the skeleton's reference pose), and its joint is bound to the hand bone (the game does
        // the same rebinding for grabs, @526273). It then goes where the hand goes and turns as the
        // hand turns; the weight stays the game's.
        bool call_ref_pose(UObject* mesh, FName bone, Xf& out)
        {
            UFunction* index_fn = mesh->GetFunctionByNameInChain(STR("GetBoneIndex"));
            UFunction* ref_fn = mesh->GetFunctionByNameInChain(STR("GetRefPoseTransform"));
            if (!index_fn || !ref_fn || ref_fn->GetParmsSize() > 160) return false;
            NameIntParams p{};
            p.BoneName = bone;
            p.ReturnValue = -1;
            mesh->ProcessEvent(index_fn, &p);
            if (p.ReturnValue < 0) return false;
            int32_t at_index = param_offset(ref_fn, L"BoneIndex"), at_ret = param_offset(ref_fn, L"ReturnValue");
            if (at_index < 0 || at_ret < 0 || at_ret % 16 != 0) return false;
            alignas(16) uint8_t parms[160] = {};
            memcpy(parms + at_index, &p.ReturnValue, sizeof(int32_t));
            mesh->ProcessEvent(ref_fn, parms);
            RawXf raw;
            memcpy(&raw, parms + at_ret, sizeof(raw));
            out = from_raw(raw);
            return true;
        }

        void shield_to_hand(Side& side, const Config& cfg)
        {
            State& s = g_state;
            side.shield_on_hand = false;
            if (!cfg.shield_on_hand || !s.kin_active || s.kin_hands || !side.wrist_freed || !s.body) return;
            const TCHAR* property = side.out_sign > 0 ? STR("PhysicsConstraint R Hand") : STR("PhysicsConstraint L Hand");
            const TCHAR* held_property = side.out_sign > 0 ? STR("Weapon R") : STR("Weapon L");
            UObject* joint = s.pawn->GetPropertyByNameInChain(property) ? object_property(s.pawn, property) : nullptr;
            UObject* weapon = s.pawn->GetPropertyByNameInChain(held_property) ? object_property(s.pawn, held_property) : nullptr;
            UObject* base = weapon && weapon->GetPropertyByNameInChain(STR("BaseMesh")) ? object_property(weapon, STR("BaseMesh")) : nullptr;
            if (!joint || !base) return;
            UFunction* get = joint->GetFunctionByNameInChain(STR("GetConstrainedComponents"));
            UFunction* set = joint->GetFunctionByNameInChain(STR("SetConstrainedComponents"));
            if (!get || !set || get->GetParmsSize() > 64 || set->GetParmsSize() > 64) return;
            int32_t g1 = param_offset(get, L"OutComponent1"), g2 = param_offset(get, L"OutComponent2"), gb2 = param_offset(get, L"OutBoneName2");
            int32_t s1 = param_offset(set, L"Component1"), sb1 = param_offset(set, L"BoneName1"), s2 = param_offset(set, L"Component2"), sb2 = param_offset(set, L"BoneName2");
            if (g1 < 0 || g2 < 0 || gb2 < 0 || s1 < 0 || sb1 < 0 || s2 < 0 || sb2 < 0) return;
            alignas(16) uint8_t got[64] = {};
            joint->ProcessEvent(get, got);
            UObject* c1 = *reinterpret_cast<UObject**>(got + g1);
            UObject* c2 = *reinterpret_cast<UObject**>(got + g2);
            FName bone2;
            memcpy(&bone2, got + gb2, sizeof(FName));
            bool on_hand = c1 == base && c2 == s.body && memcmp(&bone2, &side.hand_name, sizeof(FName)) == 0;
            bool on_forearm = c1 == base && c2 == s.body && memcmp(&bone2, &side.lower_name, sizeof(FName)) == 0;
            uint8_t* grip_type = weapon->GetValuePtrByPropertyNameInChain<uint8_t>(side.out_sign > 0 ? STR("Grip R Hand Default") : STR("Grip L Hand Default"));
            if (joint != side.shield_joint_seen || (on_forearm && side.shield_moved > 0 && side.shield_on_hand))
            {
                side.shield_joint_seen = joint;
                HSVR_LOG("Hands: the %s hand holds %ls (grip type %d), joined to the body at %ls", side.label, std::wstring(weapon->GetClassPrivate()->GetName()).c_str(),
                         grip_type ? (int)*grip_type : -1, c2 == s.body ? bone2.ToString().c_str() : L"(not the body)");
            }
            side.shield_on_hand = on_hand;
            if (!on_forearm || side.shield_moved < 0) return;
            // Where the shield is on the forearm now, and where the hand is.
            Xf lower, hand, shield, body_xf, straight;
            if (!socket_transform(s.body, side.lower_name, RTS_World, lower) || !socket_transform(s.body, side.hand_name, RTS_World, hand) || !component_world(base, shield) ||
                !component_world(s.body, body_xf))
                return;
            // The hand in the forearm's frame with a straight wrist: the reference pose, bone by bone
            // up to the forearm (twist bones may sit between), at the mesh's size.
            {
                Xf acc{};
                FName name = side.hand_name;
                UFunction* parent_fn = s.body->GetFunctionByNameInChain(STR("GetParentBone"));
                bool found = false;
                for (int step = 0; step < 4 && parent_fn; ++step)
                {
                    Xf ref;
                    if (!call_ref_pose(s.body, name, ref)) break;
                    acc = compose(acc, ref);
                    NameNameParams pp{};
                    pp.BoneName = name;
                    s.body->ProcessEvent(parent_fn, &pp);
                    name = pp.ReturnValue;
                    if (memcmp(&name, &side.lower_name, sizeof(FName)) == 0)
                    {
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    HSVR_LOG("Hands: the %s hand's shield stays on the forearm: the hand's reference pose against the forearm was not found", side.label);
                    side.shield_moved = -1000000; // say it once
                    return;
                }
                acc.t = acc.t * body_xf.s;
                straight = acc;
            }
            Xf on_lower = relative(shield, lower);
            Xf lower_if_straight = compose(relative(Xf{}, straight), hand); // the forearm a straight wrist would give this hand
            Xf target = compose(on_lower, lower_if_straight);
            UFunction* place = base->GetFunctionByNameInChain(STR("K2_SetWorldTransform"));
            if (!place || place->GetParmsSize() > 512) return;
            int32_t at_xf = param_offset(place, L"NewTransform"), at_tp = param_offset(place, L"bTeleport");
            if (at_xf < 0 || at_tp < 0 || at_xf % 16 != 0) return;
            alignas(16) uint8_t parms[512] = {};
            RawXf raw{};
            to_raw(target, raw);
            raw.s[0] = raw.s[1] = raw.s[2] = shield.s; // its size as it is (uniform in this game)
            memcpy(parms + at_xf, &raw, sizeof(raw));
            parms[at_tp] = 1;
            base->ProcessEvent(place, parms);
            alignas(16) uint8_t put[64] = {};
            memcpy(put + s1, &base, 8);
            memcpy(put + s2, &s.body, 8);
            memcpy(put + sb2, &side.hand_name, sizeof(FName));
            FName none_name = make_name(L"None");
            memcpy(put + sb1, &none_name, sizeof(FName));
            joint->ProcessEvent(set, put);
            ++side.shield_moved;
            side.grip_locked = false; // locked again on the hand (grip_lock_update, next)
            HSVR_LOG("Hands: the %s hand's %ls was strapped to the forearm; now joined to the hand as a straight wrist holds it (moved %.0f cm, turned %.0f degrees; time %d)",
                     side.label, std::wstring(weapon->GetClassPrivate()->GetName()).c_str(), length(target.t - shield.t), quat_angle_deg(target.q, shield.q), side.shield_moved);
        }

        void grip_lock_update(Side& side, bool want)
        {
            State& s = g_state;
            const TCHAR* property = side.out_sign > 0 ? STR("PhysicsConstraint R Hand") : STR("PhysicsConstraint L Hand");
            UObject* component = s.pawn && s.pawn->GetPropertyByNameInChain(property) ? object_property(s.pawn, property) : nullptr;
            if (component != side.grip_component)
            {
                // Another joint (a new weapon, a grab) or none: the old one is gone with its settings.
                side.grip_component = component;
                side.grip_locked = false;
                side.grip_saved.clear();
            }
            if (!component || (!want && !side.grip_locked)) return;
            static UObject* library = nullptr;
            if (!library) library = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__ConstraintInstanceBlueprintLibrary"));
            UFunction* get_constraint = component->GetFunctionByNameInChain(STR("GetConstraint"));
            if (!library || !get_constraint || get_constraint->GetParmsSize() > 64) return;
            int32_t ret = param_offset(get_constraint, L"ReturnValue");
            if (ret < 0) return;
            alignas(16) uint8_t accessor[64] = {};
            component->ProcessEvent(get_constraint, accessor);
            int32_t accessor_size = get_constraint->GetParmsSize() - ret;
            constexpr int32_t kMax = 256;
            auto call = [&](UFunction* fn, uint8_t* parms) {
                int32_t at = param_offset(fn, L"Accessor");
                if (at < 0) return false;
                memcpy(parms + at, accessor + ret, accessor_size);
                library->ProcessEvent(fn, parms);
                return true;
            };
            if (want && side.grip_locked)
            {
                // Still locked? The game rewrites this joint on grabs and grip changes.
                UFunction* get_ang = library->GetFunctionByNameInChain(STR("GetAngularLimits"));
                if (!get_ang || get_ang->GetParmsSize() > kMax) return;
                alignas(16) uint8_t parms[kMax] = {};
                if (!call(get_ang, parms)) return;
                bool locked = true;
                for (FProperty* p : get_ang->ForEachProperty())
                {
                    if (p->GetName().find(L"Motion") != std::wstring::npos && parms[p->GetOffset_Internal()] != 2) locked = false;
                }
                if (locked) return;
                ++side.grip_relocked;
            }
            int done = 0;
            for (const auto& pair : kGripSettings)
            {
                UFunction* get = library->GetFunctionByNameInChain(pair[0]);
                UFunction* set = library->GetFunctionByNameInChain(pair[1]);
                if (!get || !set || get->GetParmsSize() > kMax || set->GetParmsSize() > kMax) continue;
                std::vector<uint8_t>& saved = side.grip_saved[pair[0]];
                if (want && saved.empty())
                {
                    alignas(16) uint8_t parms[kMax] = {};
                    if (!call(get, parms)) continue;
                    saved.assign(parms, parms + get->GetParmsSize());
                }
                if (saved.empty()) continue;
                alignas(16) uint8_t parms[kMax] = {};
                copy_params_by_name(get, saved.data(), set, parms);
                if (want)
                {
                    for (FProperty* p : set->ForEachProperty())
                    {
                        std::wstring name = p->GetName();
                        uint8_t* at = parms + p->GetOffset_Internal();
                        if (name.find(L"Motion") != std::wstring::npos) *at = 2;                                    // locked
                        else if (name.rfind(L"bSoft", 0) == 0) *at = 0;                                             // a hard limit
                        else if (name.rfind(L"b", 0) == 0 && name.find(L"Breakable") != std::wstring::npos) *at = 0; // cannot break
                    }
                }
                if (call(set, parms)) ++done;
            }
            if (want != side.grip_locked)
                HSVR_LOG(want ? "Hands: what the %s hand holds is now fixed rigidly in it (%d of 6 joint settings changed)" : "Hands: the %s hand's grip joint is back as the game made it (%d settings)",
                         side.label, done);
            side.grip_locked = want && done > 0;
            if (!want) side.grip_saved.clear();
        }

        // ---- the mod's own legs: planted feet that step ---------------------------------------------
        // Without the ragdoll nothing moves the legs (the game's legs only stepped because its
        // physics made them). Each foot stays where it stands on the ground; when the body has moved
        // step_length_cm past it, or turned well away from it, the foot lifts and steps to its place
        // under the body, a little ahead in the direction of travel. One foot at a time, unless a
        // leg would be overstretched. `out` is each foot bone's place in the world.
        void step_feet(const Config& cfg, Xf out[2])
        {
            State& s = g_state;
            double now = now_seconds();
            double dt = s.step_time > 0 ? std::min(0.05, now - s.step_time) : 0.0;
            s.step_time = now;
            Vec3 moved = s.actor_world - s.step_last_actor;
            s.step_last_actor = s.actor_world;
            moved.z = 0;
            if (dt > 0 && length(moved) < 100.0)
            {
                double alpha = 1.0 - std::exp(-dt / 0.1);
                s.step_velocity = s.step_velocity + (moved * (1.0 / dt) - s.step_velocity) * alpha;
            }
            else
            {
                s.step_velocity = {};
            }
            double speed = length(s.step_velocity);
            Xf actor_frame{quat_from_yaw_deg(s.actor_yaw), s.actor_world, s.have_body_world ? s.body_world.s : 1.0};
            Vec3 home[2];
            double away[2] = {0, 0};
            for (int k = 0; k < 2; ++k)
            {
                Side& side = s.side[k];
                home[k] = compose(Xf{Quat{}, side.foot_home}, actor_frame).t + s.step_velocity * 0.12;
                Vec3 off = side.foot_at - home[k];
                off.z = 0;
                if (!side.foot_planted || length(off) > 130.0)
                {
                    side.foot_at = home[k];
                    side.foot_yaw = s.actor_yaw;
                    side.foot_planted = true;
                    side.stepping = false;
                    off = {};
                }
                away[k] = length(off);
            }
            // Two ways of stepping. Walking and running: a steady rhythm, like a real gait. The
            // step rate rises with speed (about 1.7 steps a second at a stroll to 4.5 at a sprint),
            // the feet take turns, and each lands ahead of the body by half the distance the body
            // covers while that foot stands. Standing or shuffling: a foot steps only when the
            // body has moved step_length_cm past it or turned well away. (The first version stepped
            // every 20 cm whatever the speed: "a lot of steps really quickly", Matthew.)
            bool moving = s.gait_moving ? speed > 18.0 : speed > 30.0;
            double rate = std::max(1.7, std::min(4.5, 1.5 + speed / 100.0 * 0.5)); // steps a second
            Vec3 heading = speed > 1.0 ? s.step_velocity * (1.0 / speed) : Vec3{};
            if (moving && !s.gait_moving)
            {
                // Start with the foot that is further behind.
                double behind[2] = {dot(home[0] - s.side[0].foot_at, heading), dot(home[1] - s.side[1].foot_at, heading)};
                s.gait_phase = behind[0] >= behind[1] ? 0.0 : 0.5;
            }
            s.gait_moving = moving;
            if (moving)
            {
                s.gait_phase += dt * rate * 0.5;
                s.gait_phase -= std::floor(s.gait_phase);
                double lead = std::min(45.0, speed / (2.0 * rate)); // half the ground covered during one stance
                for (int k = 0; k < 2; ++k)
                {
                    Side& side = s.side[k];
                    double begin = k == 0 ? 0.0 : 0.5;
                    bool swinging = s.gait_phase >= begin && s.gait_phase < begin + 0.5;
                    Vec3 plain_home = home[k] - s.step_velocity * 0.12; // without the standing-mode lead
                    if (swinging && !(side.stepping && side.gait_step))
                    {
                        side.stepping = true;
                        side.gait_step = true;
                        side.step_from = side.foot_at;
                        side.step_from_yaw = side.foot_yaw;
                        ++s.steps_taken;
                        ++s.steps_total;
                    }
                    if (side.stepping && side.gait_step)
                    {
                        if (swinging)
                        {
                            side.step_t = (s.gait_phase - begin) / 0.5;
                            side.step_to = plain_home + heading * lead;
                        }
                        else
                        {
                            // The swing is over: the foot stands where it was heading.
                            side.stepping = false;
                            side.gait_step = false;
                            side.foot_at = side.step_to;
                            side.foot_yaw = s.actor_yaw;
                        }
                    }
                    else if (!side.stepping)
                    {
                        // A standing foot left too far behind (a sudden change of direction) steps at once.
                        Vec3 off = side.foot_at - plain_home;
                        off.z = 0;
                        if (length(off) > 80.0)
                        {
                            side.foot_at = plain_home;
                            side.foot_yaw = s.actor_yaw;
                        }
                    }
                }
            }
            else
            {
                double trigger = std::max(5.0, cfg.step_length_cm);
                int order[2] = {away[0] >= away[1] ? 0 : 1, away[0] >= away[1] ? 1 : 0};
                for (int n = 0; n < 2; ++n)
                {
                    int k = order[n];
                    Side& side = s.side[k];
                    Side& other = s.side[1 - k];
                    if (side.stepping)
                    {
                        if (side.gait_step)
                        {
                            // Stopped in mid-stride: finish this step by the clock, under the body.
                            side.gait_step = false;
                            side.step_duration = 0.18;
                        }
                        continue;
                    }
                    bool other_busy = other.stepping && other.step_t < 0.6;
                    bool turned = std::fabs(normalize_axis(s.actor_yaw - side.foot_yaw)) > 35.0;
                    bool overstretched = away[k] > 45.0;
                    if ((!other_busy && (away[k] > trigger || turned)) || overstretched)
                    {
                        side.stepping = true;
                        side.gait_step = false;
                        side.step_from = side.foot_at;
                        side.step_from_yaw = side.foot_yaw;
                        side.step_t = 0;
                        side.step_duration = 0.22;
                        ++s.steps_taken;
                        ++s.steps_total;
                    }
                }
                for (Side& side : s.side)
                {
                    if (side.stepping && !side.gait_step)
                    {
                        side.step_t += dt / side.step_duration;
                        side.step_to = home[&side - s.side];
                        if (side.step_t >= 1.0)
                        {
                            side.stepping = false;
                            side.foot_at = side.step_to;
                            side.foot_yaw = s.actor_yaw;
                        }
                    }
                }
            }
            double lift = std::max(0.0, cfg.step_height_cm) * std::min(2.0, 1.0 + speed / 300.0);
            for (int k = 0; k < 2; ++k)
            {
                Side& side = s.side[k];
                double ground = home[k].z; // the ground under the character
                Vec3 at = side.foot_at;
                if (side.stepping)
                {
                    double t = std::max(0.0, std::min(1.0, side.step_t));
                    double ease = t * t * (3.0 - 2.0 * t);
                    at = side.step_from + (side.step_to - side.step_from) * ease;
                    at.z = ground + lift * std::sin(kPi * t);
                    side.foot_yaw = side.step_from_yaw + normalize_axis(s.actor_yaw - side.step_from_yaw) * ease;
                }
                else
                {
                    side.foot_at.z = ground;
                    at = side.foot_at;
                }
                out[k] = {(quat_from_yaw_deg(side.foot_yaw) * side.foot_rest_q).normalized(), at};
            }
        }

        // The body mesh's response to the level's own geometry (collision channel WorldStatic).
        void body_level_collision(bool collide)
        {
            State& s = g_state;
            UFunction* fn = s.body ? s.body->GetFunctionByNameInChain(STR("SetCollisionResponseToChannel")) : nullptr;
            if (!fn || fn->GetParmsSize() > 8) return;
            int32_t channel = param_offset(fn, L"Channel"), response = param_offset(fn, L"NewResponse");
            if (channel < 0 || response < 0) return;
            uint8_t parms[8] = {};
            parms[channel] = 0;                 // ECC_WorldStatic
            parms[response] = collide ? 2 : 0;  // ECR_Block / ECR_Ignore
            s.body->ProcessEvent(fn, parms);
        }

        void kinematic_update(bool want, const char* why_not)
        {
            State& s = g_state;
            if (!s.body) return;
            if (want)
            {
                Config cfg = config();
                // Physical hands need their wrists free; without that they are placed rigidly.
                // ... and a drive to the controllers; until the game's muscle settings have been found
                // (a moment after spawn) the hands stay placed, or they would drop to the floor.
                bool drive_ready = g_pull.ok && g_pull.pawn == s.pawn && cfg.hand_pull;
                bool want_rigid_hands = cfg.kinematic_hands || !cfg.free_wrists || s.wrist_failed || !drive_ready;
                bool hands = s.kin_active ? s.kin_hands : want_rigid_hands;
                if (s.kin_active && want_rigid_hands != s.kin_hands) hands = want_rigid_hands;
                // The game may switch simulation back on (its own script); look every frame.
                bool game_took_it = s.kin_active && (body_simulating(L"spine_03") || body_simulating(L"thigh_r") || body_simulating(L"lowerarm_r") ||
                                                     (s.kin_hands && (body_simulating(L"hand_r") || body_simulating(L"hand_l"))));
                if (!s.kin_active || game_took_it || hands != s.kin_hands)
                {
                    if (!body_set_simulate(L"pelvis", false)) return;
                    if (!hands)
                    {
                        bool freed = wrist_set_free(s.side[0], true);
                        freed = wrist_set_free(s.side[1], true) && freed;
                        if (!freed)
                        {
                            wrist_set_free(s.side[0], false);
                            wrist_set_free(s.side[1], false);
                            hands = true;
                            s.wrist_failed = true;
                            HSVR_LOG("Hands: the wrist joints could not be set free; the hands are placed rigidly at the controllers instead");
                        }
                    }
                    else
                    {
                        wrist_set_free(s.side[0], false);
                        wrist_set_free(s.side[1], false);
                    }
                    if (!hands)
                    {
                        body_set_simulate(L"hand_r", true);
                        body_set_simulate(L"hand_l", true);
                    }
                    if (game_took_it)
                    {
                        // The game ran its own refresh of the body (at the start of a life, and again
                        // when it applies muscle growth, half a minute or so in: everything below the
                        // hips simulated again, the mesh resized). The body's physics is as good as
                        // new after that: the collision switches are gone and the joints that hold
                        // the weapons no longer hold. Everything of ours is set up again.
                        ++s.kin_retaken;
                        g_self_hits_redo = true;
                        for (Side& side : s.side)
                        {
                            const TCHAR* property = side.out_sign > 0 ? STR("PhysicsConstraint R Hand") : STR("PhysicsConstraint L Hand");
                            UObject* joint = s.pawn->GetPropertyByNameInChain(property) ? object_property(s.pawn, property) : nullptr;
                            UFunction* get = joint ? joint->GetFunctionByNameInChain(STR("GetConstrainedComponents")) : nullptr;
                            UFunction* set = joint ? joint->GetFunctionByNameInChain(STR("SetConstrainedComponents")) : nullptr;
                            UFunction* broken = joint ? joint->GetFunctionByNameInChain(STR("IsBroken")) : nullptr;
                            bool was_broken = false;
                            if (broken && broken->GetParmsSize() == 1)
                            {
                                uint8_t out = 0;
                                joint->ProcessEvent(broken, &out);
                                was_broken = out != 0;
                            }
                            bool again = false;
                            if (get && set && get->GetParmsSize() <= 64 && set->GetParmsSize() <= 64)
                            {
                                int32_t g1 = param_offset(get, L"OutComponent1"), gb1 = param_offset(get, L"OutBoneName1"), g2 = param_offset(get, L"OutComponent2"),
                                        gb2 = param_offset(get, L"OutBoneName2");
                                int32_t s1 = param_offset(set, L"Component1"), sb1 = param_offset(set, L"BoneName1"), s2 = param_offset(set, L"Component2"),
                                        sb2 = param_offset(set, L"BoneName2");
                                if (g1 >= 0 && gb1 >= 0 && g2 >= 0 && gb2 >= 0 && s1 >= 0 && sb1 >= 0 && s2 >= 0 && sb2 >= 0)
                                {
                                    alignas(16) uint8_t got[64] = {}, put[64] = {};
                                    joint->ProcessEvent(get, got);
                                    UObject* c1 = *reinterpret_cast<UObject**>(got + g1);
                                    UObject* c2 = *reinterpret_cast<UObject**>(got + g2);
                                    if (c1 && c2 && !was_broken)
                                    {
                                        memcpy(put + s1, got + g1, 8);
                                        memcpy(put + sb1, got + gb1, sizeof(FName));
                                        memcpy(put + s2, got + g2, 8);
                                        memcpy(put + sb2, got + gb2, sizeof(FName));
                                        joint->ProcessEvent(set, put);
                                        again = true;
                                    }
                                }
                            }
                            side.grip_locked = false; // locked again below, whatever the game left
                            HSVR_LOG("Body: the game rebuilt the body (time %d): %s hand's grip joint %s%s | hand simulated: %s", s.kin_retaken, side.label,
                                     !joint ? "does not exist" : (again ? "joined again" : "left alone (nothing held)"), was_broken ? ", it was broken" : "",
                                     body_simulating(side.out_sign > 0 ? L"hand_r" : L"hand_l") ? "yes" : "no");
                        }
                    }
                    else HSVR_LOG(hands ? "Body: the body is now placed, not simulated, hands included (only what the hands hold stays physical)"
                                        : "Body: the body is now placed, not simulated; the hands are physical, loose from the arms and driven to the controllers");
                    s.kin_active = true;
                    s.kin_hands = hands;
                    if (cfg.body_ignores_level != s.level_ignored)
                    {
                        body_level_collision(!cfg.body_ignores_level);
                        s.level_ignored = cfg.body_ignores_level;
                        HSVR_LOG(s.level_ignored ? "Body: the placed body no longer collides with the level itself (the capsule does)" : "Body: the body collides with the level again");
                    }
                }
                // The game moves the character after its hips once they are further apart than this
                // (10 cm by default). The placed body's hips are where the player is; keep it out of reach.
                if (double* limit = s.pawn->GetValuePtrByPropertyNameInChain<double>(STR("Capsule Max Dislocation")))
                {
                    if (!s.have_max_dislocation && *limit < 100000.0)
                    {
                        s.game_max_dislocation = *limit;
                        s.have_max_dislocation = true;
                    }
                    *limit = 1000000.0;
                }
                // (The game works its leash out anew every frame while standing, so the value above
                // only counts while fallen; what the leash does is undone in the character's tick.)
                placed_body_facts(false);
                // The mesh rides on the character; if anything took it off again, put it back.
                if (!body_attached_to_capsule())
                {
                    bool ok = body_reattach();
                    ++s.kin_reattached;
                    if (ok != s.kin_attach_ok || s.kin_reattached == 1)
                        HSVR_LOG(ok ? "Body: the body mesh is back on the character (it had been detached for the ragdoll)" : "Body: could NOT put the body mesh back on the character");
                    s.kin_attach_ok = ok;
                }
            }
            else if (s.kin_active)
            {
                grip_lock_update(s.side[0], false);
                grip_lock_update(s.side[1], false);
                wrist_set_free(s.side[0], false);
                wrist_set_free(s.side[1], false);
                if (s.have_max_dislocation)
                {
                    if (double* limit = s.pawn->GetValuePtrByPropertyNameInChain<double>(STR("Capsule Max Dislocation"))) *limit = s.game_max_dislocation;
                }
                if (bool* blocked = s.pawn->GetValuePtrByPropertyNameInChain<bool>(STR("Dislocation Blocked"))) *blocked = false;
                if (s.level_ignored)
                {
                    body_level_collision(true);
                    s.level_ignored = false;
                }
                body_set_simulate(L"pelvis", true);
                s.kin_active = false;
                HSVR_LOG("Body: the body is given back to the game's simulation (%s)", why_not);
            }
        }

        Quat quat_between(const Vec3& from_unit, const Vec3& to_unit)
        {
            Vec3 cr = cross(from_unit, to_unit);
            double sn = length(cr), cs = dot(from_unit, to_unit);
            if (sn < 1e-9) return Quat{};
            return quat_axis_angle(cr * (1.0 / sn), std::atan2(sn, cs));
        }

        // The hips have just been moved by the pelvis drop; put this leg's foot back where the
        // game's animation had it by bending the knee (in the direction the animation bends it).
        // `foot_target` is where the foot bone goes (pose space); `knee_forward`, when given, is the
        // way the knee points (the foot's heading), else the animation's own knee direction is kept.
        void solve_leg(const Side& side, std::vector<Xf>& pose, const std::vector<Xf>& before, const Quat& to_pose, const Xf& foot_target, const Vec3* knee_forward)
        {
            State& s = g_state;
            if (side.thigh < 0) return;
            Vec3 hip0 = before[side.thigh].t, knee0 = before[side.calf].t, foot0 = before[side.foot].t;
            Vec3 hip = pose[side.thigh].t;
            double a = length(knee0 - hip0), b = length(foot0 - knee0);
            if (a < 1.0 || b < 1.0) return;
            Vec3 to_foot = foot_target.t - hip;
            double d = std::max(std::fabs(a - b) + 0.5, std::min(length(to_foot), (a + b) * 0.999));
            Vec3 dir = normalized(to_foot, {0, 0, -1});
            Vec3 pole = knee_forward ? *knee_forward : perpendicular_part(knee0 - hip0, normalized(foot0 - hip0, dir));
            if (length(pole) < 0.5) pole = to_pose.rotate(quat_from_yaw_deg(s.actor_yaw).rotate({1, 0, 0})); // a straight leg: knees go forward
            pole = perpendicular_part(pole, dir);
            if (length(pole) < 1e-3) return;
            pole = normalized(pole);
            double cos_a = std::max(-1.0, std::min(1.0, (a * a + d * d - b * b) / (2.0 * a * d)));
            double sin_a = std::sqrt(std::max(0.0, 1.0 - cos_a * cos_a));
            Vec3 knee = hip + dir * (a * cos_a) + pole * (a * sin_a);
            Vec3 foot = hip + dir * d;
            pose[side.thigh] = {(quat_between(normalized(knee0 - hip0), normalized(knee - hip)) * before[side.thigh].q).normalized(), hip};
            pose[side.calf] = {(quat_between(normalized(foot0 - knee0), normalized(foot - knee)) * before[side.calf].q).normalized(), knee};
            pose[side.foot] = {foot_target.q, foot};
        }

        // ---- the upper body and head follow the headset ----------------------------------------
        // Which of the head bone's own axes points up and which forward, from the game's own
        // animation while the character stands (bone axes line up with the body's directions).
        void learn_head_axes()
        {
            State& s = g_state;
            if (s.have_head_axes || s.head < 0 || !s.have_body_world || !s.have_animated) return;
            Xf head_world = compose(from_raw(s.animated[s.head]), s.body_world);
            Quat inv = head_world.q.inverse();
            auto snap = [](const Vec3& v, Vec3& axis) {
                double c[3] = {v.x, v.y, v.z};
                int best = 0;
                for (int i = 1; i < 3; ++i)
                {
                    if (std::fabs(c[i]) > std::fabs(c[best])) best = i;
                }
                double out[3] = {0, 0, 0};
                out[best] = c[best] < 0 ? -1.0 : 1.0;
                axis = {out[0], out[1], out[2]};
                return std::fabs(c[best]);
            };
            Vec3 up_axis, forward_axis;
            if (snap(inv.rotate({0, 0, 1}), up_axis) < 0.8) return;
            Vec3 forward = inv.rotate(quat_from_yaw_deg(s.actor_yaw).rotate({1, 0, 0}));
            forward = perpendicular_part(forward, up_axis);
            if (snap(forward, forward_axis) < 0.6) return;
            s.head_look = quat_from_frame(forward_axis, cross(up_axis, forward_axis));
            s.have_head_axes = true;
            HSVR_LOG("Body: head bone axes learned: forward (%.0f %.0f %.0f), up (%.0f %.0f %.0f)", forward_axis.x, forward_axis.y, forward_axis.z, up_axis.x,
                     up_axis.y, up_axis.z);
        }

        // The upper body's own standing shape, taken once from the game's animation a moment after
        // the character appears (it stands upright then).
        void learn_rest()
        {
            State& s = g_state;
            if (s.have_rest || s.head < 0 || s.spine_base < 0 || !s.have_body_world || !s.have_animated) return;
            if (++s.rest_wait < 30) return;
            Xf base_world = compose(from_raw(s.animated[s.spine_base]), s.body_world);
            Xf head_world = compose(from_raw(s.animated[s.head]), s.body_world);
            if (head_world.t.z - base_world.t.z < 25.0) return; // not standing upright; try again next frame
            Xf frame{quat_from_yaw_deg(s.actor_yaw), base_world.t, s.body_world.s}; // (in the body's own size: the rest shape is size-free)
            s.rest_head_z = from_raw(s.animated[s.head]).t.z;
            s.rest.assign(s.bone_count, Xf{});
            for (int32_t i = 0; i < s.bone_count; ++i)
            {
                if (s.below_spine[i]) s.rest[i] = relative(compose(from_raw(s.animated[i]), s.body_world), frame);
            }
            Vec3 actor;
            if (actor_location(s.pawn, actor))
            {
                Xf actor_frame{quat_from_yaw_deg(s.actor_yaw), actor, s.body_world.s};
                for (Side& side : s.side)
                {
                    if (side.foot < 0) continue;
                    Xf home = relative(compose(from_raw(s.animated[side.foot]), s.body_world), actor_frame);
                    side.foot_home = home.t;
                    side.foot_rest_q = home.q;
                    side.have_foot_home = true;
                    HSVR_LOG("Legs: %s foot rests %.0f cm forward, %.0f cm to the side, %.0f cm below the character's centre", side.label, home.t.x, home.t.y, -home.t.z);
                }
            }
            s.have_rest = true;
            Vec3 lean = s.rest[s.head].t;
            HSVR_LOG("Body: upper body shape taken from the game's standing pose (head %.0f cm above the lowest spine bone, %.0f cm forward, %.0f cm to the side)",
                     lean.z, lean.x, lean.y);
        }

        // Builds the driver's upper body from the player: a fixed standing shape on top of the
        // game's hips, turned to where the player's chest most likely faces, leaned so that the head
        // is at the headset, with the head turned like the headset. The arms are solved after
        // this. The legs and hips stay the game's.
        void apply_head(std::vector<Xf>& pose, std::vector<char>& moved, const Config& cfg)
        {
            State& s = g_state;
            s.head_applied = false;
            s.lean_cm = 0;
            if (!cfg.body_follow || s.head < 0 || s.spine_base < 0 || !s.have_body_world || !s.have_view || !s.view.head_valid) return;
            Quat to_pose = s.body_world.q.inverse();
            s.lean_cm = length(s.view.lean);
            Vec3 head_target_world = s.view.head_bone + s.actor_delta;
            s.pelvis_drop = 0;
            if (s.placed && s.have_nominal)
            {
                // The legs and hips: the game's own leg animation, set over the character's feet.
                // (The body mesh itself may sit elsewhere; it used to ride on the simulated hips.)
                // Every bone is taken over, the upper body too: it is rebuilt below on top of the
                // lowest spine bone, which must be in the same place as the hips it sits on.
                for (int32_t i = 0; i < s.bone_count; ++i)
                {
                    pose[i] = relative(compose(pose[i], s.nominal), s.body_world);
                    moved[i] = 1;
                }
                if (cfg.leg_ik && s.pelvis >= 0 && s.have_rest)
                {
                    // Lower (or raise a little) the hips so that the fixed-length upper body can put
                    // the head at the headset's height; the knees bend to keep the feet in place.
                    Vec3 up = to_pose.rotate({0, 0, 1});
                    double torso = length(s.rest[s.head].t);
                    Vec3 rel = to_pose.rotate(head_target_world - s.body_world.t) * (1.0 / s.body_world.s) - pose[s.spine_base].t;
                    double along = dot(rel, up);
                    double disc = along * along - (dot(rel, rel) - torso * torso);
                    double drop = disc > 0.0 ? along - std::sqrt(disc) : along;
                    // Up as well as down: the game's own crouch animation must not lower the body
                    // while the player's head stays up. Hip height comes from the player's head only.
                    drop = std::max(-75.0, std::min(80.0, drop));
                    bool own_legs = cfg.own_legs && s.have_actor_world && s.side[0].have_foot_home && s.side[1].have_foot_home;
                    if (std::fabs(drop) > 0.2 || own_legs)
                    {
                        std::vector<Xf> before(pose);
                        for (int32_t i = 0; i < s.bone_count; ++i)
                        {
                            if (s.below_pelvis[i]) pose[i].t = pose[i].t + up * drop;
                        }
                        Xf feet[2];
                        if (own_legs) step_feet(cfg, feet);
                        for (int k = 0; k < 2; ++k)
                        {
                            Side& side = s.side[k];
                            if (side.foot < 0) continue;
                            if (own_legs && cfg.kick_legs)
                            {
                                // A kicking leg is the game's own animation (already in the pose).
                                float* kick = s.pawn->GetValuePtrByPropertyNameInChain<float>(side.out_sign > 0 ? STR("Kick Rate R") : STR("Kick Rate L"));
                                if (kick && *kick > 0.02f)
                                {
                                    side.foot_planted = false; // stands down again where the body is afterwards
                                    continue;
                                }
                            }
                            if (own_legs)
                            {
                                Vec3 knee_forward = to_pose.rotate(quat_from_yaw_deg(side.foot_yaw).rotate({1, 0, 0}));
                                solve_leg(side, pose, before, to_pose, relative(feet[k], s.body_world), &knee_forward);
                            }
                            else
                            {
                                solve_leg(side, pose, before, to_pose, before[side.foot], nullptr);
                            }
                        }
                        for (int32_t i = 0; i < s.bone_count; ++i)
                        {
                            if (!s.leg_below[i]) continue;
                            bool solved = false;
                            for (const Side& side2 : s.side) solved = solved || (side2.thigh >= 0 && (i == side2.calf || i == side2.foot));
                            if (solved) continue;
                            int32_t parent = s.parents[i];
                            pose[i] = compose(relative(before[i], before[parent]), pose[parent]); // toes, twist bones
                        }
                        s.pelvis_drop = drop;
                    }
                }
            }
            if (cfg.own_upper_body && s.have_rest)
            {
                // Nothing tracks the chest, so it follows the head's heading loosely: freely
                // within 25 degrees, slowly towards it otherwise, and never further than
                // chest_turn_max_deg from the legs.
                double now = now_seconds();
                double dt = s.chest_time > 0 ? std::min(0.1, now - s.chest_time) : 0.0;
                s.chest_time = now;
                if (!s.have_chest_yaw)
                {
                    s.chest_yaw = s.actor_yaw;
                    s.have_chest_yaw = true;
                }
                Vec3 look = s.view.rotation.rotate({1, 0, 0});
                if (std::fabs(look.z) < 0.85) // looking steeply up or down says nothing about heading
                {
                    double head_yaw = std::atan2(look.y, look.x) * kRadToDeg;
                    double diff = normalize_axis(head_yaw - s.chest_yaw);
                    if (std::fabs(diff) > 25.0) s.chest_yaw += diff - (diff > 0 ? 25.0 : -25.0);
                    s.chest_yaw += normalize_axis(head_yaw - s.chest_yaw) * (1.0 - std::exp(-dt / 0.8));
                }
                double most = std::max(0.0, cfg.chest_turn_max_deg);
                double from_legs = std::max(-most, std::min(most, normalize_axis(s.chest_yaw - s.actor_yaw)));
                s.chest_yaw = s.actor_yaw + from_legs;
                Xf frame{quat_from_yaw_deg(s.chest_yaw), compose(pose[s.spine_base], s.body_world).t, s.body_world.s};
                for (int32_t i = 0; i < s.bone_count; ++i)
                {
                    if (!s.below_spine[i]) continue;
                    pose[i] = relative(compose(s.rest[i], frame), s.body_world);
                    moved[i] = 1;
                }
            }
            // Lean about the lowest spine bone so that the head is towards the headset's place.
            Vec3 pivot = pose[s.spine_base].t;
            Vec3 head_target = to_pose.rotate(head_target_world - s.body_world.t) * (1.0 / s.body_world.s);
            Vec3 before_lean = pose[s.head].t - pivot;
            Vec3 after_lean = before_lean + (head_target - pose[s.head].t) * std::max(0.0, std::min(1.0, cfg.lean_follow));
            if (length(before_lean) > 5.0 && length(after_lean) > 5.0)
            {
                Vec3 from = normalized(before_lean), to = normalized(after_lean);
                Vec3 cr = cross(from, to);
                double sn = length(cr), cs = dot(from, to);
                if (sn > 1e-6)
                {
                    double angle = std::min(std::atan2(sn, cs), std::max(0.0, cfg.lean_max_deg) * kDegToRad);
                    Quat swing = quat_axis_angle(cr * (1.0 / sn), angle);
                    for (int32_t i = 0; i < s.bone_count; ++i)
                    {
                        if (!s.below_spine[i]) continue;
                        pose[i].q = (swing * pose[i].q).normalized();
                        pose[i].t = pivot + swing.rotate(pose[i].t - pivot);
                        moved[i] = 1;
                    }
                }
            }
            // The head itself: at the headset's place (the head pull takes the body there) and
            // turned like the headset.
            std::vector<Xf> before(pose);
            Quat head_q = pose[s.head].q;
            if (cfg.head_turn && s.have_head_axes)
            {
                Quat want = (to_pose * s.view.rotation * s.head_look.inverse()).normalized();
                Quat delta = (want * head_q.inverse()).normalized();
                if (delta.w < 0) delta = {-delta.x, -delta.y, -delta.z, -delta.w};
                double angle = 2.0 * std::acos(std::min(1.0, delta.w));
                double most = std::max(0.0, cfg.head_turn_max_deg) * kDegToRad;
                if (angle > most && angle > 1e-6) delta = quat_axis_angle(normalized({delta.x, delta.y, delta.z}), most);
                head_q = (delta * head_q).normalized();
            }
            Vec3 head_t = pose[s.head].t;
            if (s.head_pull_active || s.placed)
            {
                // Not further from the spine than the neck can stretch, or the pull would hang
                // the body from its head.
                Vec3 reach = head_target - pivot;
                double longest = length(before_lean) * 1.08;
                head_t = length(reach) > longest ? pivot + normalized(reach) * longest : head_target;
            }
            pose[s.head] = {head_q, head_t};
            moved[s.head] = 1;
            for (int32_t i = s.head + 1; i < s.bone_count; ++i)
            {
                int32_t parent = s.parents[i];
                if (parent < 0 || !s.below_head[parent]) continue;
                pose[i] = compose(relative(before[i], before[parent]), pose[parent]);
                moved[i] = 1;
            }
            s.head_target_world = compose(pose[s.head], s.body_world).t;
            s.have_head_target = true;
            s.head_applied = true;
        }

        // ---- the character's own head is not drawn in first person -----------------------------
        // Only a drawing flag is used: each head piece (the head model, hair, anything named or
        // attached like headgear) is marked "not drawn for the player who owns it". Nothing the
        // game's own logic reads is changed. In particular the head BONE is never hidden: the game
        // uses that switch for a severed head and treated the character as beheaded when an earlier
        // version of this mod did it (Matthew, 2026-11-16).
        struct RawObjectArray
        {
            UObject** data;
            int32_t num;
            int32_t max;
        };
        struct OwnerNoSeeParams
        {
            bool NewOwnerNoSee;
        };
        struct NameReturnParams
        {
            FName ReturnValue;
        };

        bool is_head_piece(UObject* component, UObject* body, UObject* driver)
        {
            if (!component || component == body || component == driver) return false;
            if (!component->GetFunctionByNameInChain(STR("SetOwnerNoSee"))) return false; // not something that is drawn
            static const wchar_t* const words[] = {L"head", L"helm", L"hair", L"hood", L"coif", L"visor", L"beard", L"brow", L"lash"};
            std::wstring names = component->GetName();
            for (const TCHAR* asset_property : {STR("SkinnedAsset"), STR("StaticMesh")})
            {
                if (!component->GetPropertyByNameInChain(asset_property)) continue;
                if (UObject* asset = object_property(component, asset_property)) names += L" " + asset->GetName();
            }
            std::transform(names.begin(), names.end(), names.begin(), [](wchar_t ch) { return (wchar_t)towlower(ch); });
            for (const wchar_t* word : words)
            {
                if (names.find(word) != std::wstring::npos) return true;
            }
            // Anything fixed to the head bone.
            if (UFunction* fn = component->GetFunctionByNameInChain(STR("GetAttachSocketName")))
            {
                if (fn->GetParmsSize() == (int32_t)sizeof(NameReturnParams))
                {
                    NameReturnParams p{};
                    component->ProcessEvent(fn, &p);
                    static const FName head = make_name(L"head");
                    if (memcmp(&p.ReturnValue, &head, sizeof(FName)) == 0) return true;
                }
            }
            return false;
        }

        void hide_head_tick(const Config& cfg)
        {
            State& s = g_state;
            static UObject* hidden_pawn = nullptr;
            static double next = 0.0;
            static size_t last_count = 0;
            UObject* body = object_property(s.pawn, STR("Mesh"));
            UObject* driver = object_property(s.pawn, STR("DriverSkeleton"));
            bool want = cfg.hide_head && s.pawn && body && driver && cfg.anchor == "head" && !shared().third_person.load() && shared().xr_running.load();
            double now = now_seconds();
            if (hidden_pawn && hidden_pawn != s.pawn) hidden_pawn = nullptr; // the old character is gone or no longer ours
            bool was = hidden_pawn != nullptr;
            if (!want && !was) return;
            if (want && was && now < next) return; // again now and then: headgear put on later, or the game switching it back
            next = now + 2.0;
            if (!s.pawn || !body) return;
            std::vector<UObject*> pieces;
            for (const TCHAR* list : {STR("BlueprintCreatedComponents"), STR("InstanceComponents")})
            {
                RawObjectArray* arr = s.pawn->GetValuePtrByPropertyNameInChain<RawObjectArray>(list);
                if (!arr || !arr->data || arr->num <= 0 || arr->num > 2000) continue;
                for (int32_t i = 0; i < arr->num; ++i)
                {
                    if (is_head_piece(arr->data[i], body, driver)) pieces.push_back(arr->data[i]);
                }
            }
            std::wstring names;
            for (UObject* piece : pieces)
            {
                UFunction* fn = piece->GetFunctionByNameInChain(STR("SetOwnerNoSee"));
                if (!fn || fn->GetParmsSize() != (int32_t)sizeof(OwnerNoSeeParams)) continue;
                OwnerNoSeeParams p{want};
                piece->ProcessEvent(fn, &p);
                names += piece->GetName() + L" ";
            }
            if (want != was || pieces.size() != last_count)
                HSVR_LOG(want ? "Body: not drawn for the player (drawing flag only, the head bone is untouched): %ls" : "Body: drawn again: %ls", names.c_str());
            last_count = pieces.size();
            hidden_pawn = want ? s.pawn : nullptr;
        }

        // ---- roomscale: the character walks, turns and ducks under the player --------------------
        // In Blade & Sorcery (Matthew's reference) the body is always under the player's real
        // head. Here: when the headset has moved away from over the character's feet, the
        // character itself is moved there (its capsule, with the game's collision), and the
        // headset's neutral place is moved back by the same amount, so the view does not change.
        // The same for turning (the character's aim heading follows the head beyond a free range)
        // and ducking (the game's crouch).
        struct RoomState
        {
            UObject* pawn = nullptr;
            UFunction* offset_fn = nullptr;
            int32_t offset_parms = 0, o_delta = -1, o_sweep = -1, o_teleport = -1;
            UFunction* input_fn = nullptr;
            bool tried = false;
            double last_time = 0;
            bool have_last_actor = false;
            Vec3 last_actor;
            bool crouch_held = false;
            // For the log.
            double moved_cm = 0, turned_deg = 0, blocked_cm = 0;
        };
        RoomState g_room;
        struct MovementInputParams
        {
            ue::FVector WorldDirection;
            float ScaleValue;
            bool bForce;
        };

        void roomscale_tick(const Config& cfg)
        {
            State& s = g_state;
            RoomState& r = g_room;
            if (r.pawn != s.pawn)
            {
                r = RoomState{};
                r.pawn = s.pawn;
            }
            double now = now_seconds();
            double dt = r.last_time > 0 ? std::min(0.05, now - r.last_time) : 0.0;
            r.last_time = now;
            HeadWorld view;
            bool active = cfg.roomscale != 0 && cfg.body_follow && cfg.motion_arms && s.pawn && s.ready && s.enabled_by_key && !s.menu_up && stereo_head_world(view) && view.head_valid;
            {
                // The crouch button: the game's crouch state lowers the view (smoothly), so the
                // player crouches with the character instead of watching it crouch.
                // With own_crouch the button toggles our own flag (controller_input.cpp); otherwise the
                // game's crouch state is followed.
                bool* crouched = s.pawn ? s.pawn->GetValuePtrByPropertyNameInChain<bool>(STR("Is Crouched??")) : nullptr;
                bool crouching = cfg.own_crouch ? shared().own_crouch.load() : (crouched && *crouched && !r.crouch_held);
                if (!active || !s.kin_active) shared().own_crouch.store(false);
                double want = active && crouching ? std::max(0.0, std::min(70.0, cfg.crouch_view_drop_cm)) : 0.0;
                double have = (double)shared().view_drop_cm.load();
                double step = 160.0 * std::max(dt, 0.0);
                if (!active) have = 0.0;
                else if (have < want) have = std::min(want, have + step);
                else have = std::max(want, have - step);
                shared().view_drop_cm.store((float)have);
            }
            if (!active || dt <= 0.0)
            {
                r.have_last_actor = false;
                if (!active && r.crouch_held && s.pawn)
                {
                    // Do not leave the game crouching when the VR controls go off.
                    if (bool* holding = s.pawn->GetValuePtrByPropertyNameInChain<bool>(STR("Crouch Key Holding"))) *holding = false;
                    r.crouch_held = false;
                }
                return;
            }
            if (!r.tried)
            {
                r.tried = true;
                UFunction* fn = s.pawn->GetFunctionByNameInChain(STR("K2_AddActorWorldOffset"));
                if (fn)
                {
                    for (FProperty* p : fn->ForEachProperty())
                    {
                        std::wstring name = p->GetName();
                        if (name == L"DeltaLocation" && p->GetSize() == (int32_t)sizeof(ue::FVector)) r.o_delta = p->GetOffset_Internal();
                        if (name == L"bSweep") r.o_sweep = p->GetOffset_Internal();
                        if (name == L"bTeleport") r.o_teleport = p->GetOffset_Internal();
                    }
                    r.offset_parms = fn->GetParmsSize();
                    if (r.o_delta >= 0 && r.o_sweep >= 0 && r.o_teleport >= 0 && r.offset_parms <= 1024) r.offset_fn = fn;
                }
                UFunction* input = s.pawn->GetFunctionByNameInChain(STR("AddMovementInput"));
                if (input && input->GetParmsSize() <= (int32_t)sizeof(MovementInputParams) && param_offset(input, L"WorldDirection") == 0 &&
                    param_offset(input, L"ScaleValue") == (int32_t)offsetof(MovementInputParams, ScaleValue))
                    r.input_fn = input;
                HSVR_LOG("Room: moving the character directly %s, the game's walking %s", r.offset_fn ? "available" : "NOT available", r.input_fn ? "available" : "NOT available");
            }
            Vec3 actor;
            if (!actor_location(s.pawn, actor)) return;

            // ---- steps ----
            Vec3 away{view.lean.x, view.lean.y, 0}; // the head bone's place relative to over the feet
            double distance = length(away);
            double free_cm = std::max(0.0, cfg.roomscale_lean_cm);
            Vec3 direction = distance > 1e-3 ? away * (1.0 / distance) : Vec3{};
            if (cfg.roomscale == 2 && r.offset_fn && distance > free_cm)
            {
                double step = std::min(distance - free_cm, std::max(0.0, cfg.roomscale_max_speed) * dt);
                alignas(16) uint8_t parms[1024] = {};
                ue::FVector delta{direction.x * step, direction.y * step, 0.0};
                memcpy(parms + r.o_delta, &delta, sizeof(delta));
                parms[r.o_sweep] = 1; // stop at walls
                parms[r.o_teleport] = 0;
                s.pawn->ProcessEvent(r.offset_fn, parms);
                Vec3 after;
                if (actor_location(s.pawn, after))
                {
                    Vec3 went{after.x - actor.x, after.y - actor.y, 0};
                    stereo_consume_room(went, 0.0);
                    s.room_step = s.room_step + went;
                    r.moved_cm += length(went);
                    r.blocked_cm += std::max(0.0, step - length(went));
                    actor = after;
                }
            }
            else if (cfg.roomscale == 1 && r.input_fn)
            {
                // The game walks the character; whatever it has moved towards the head since last
                // frame is taken off the distance.
                if (r.have_last_actor && distance > 1e-3)
                {
                    Vec3 went = actor - r.last_actor;
                    double towards = std::max(0.0, std::min(dot(went, direction), std::max(0.0, distance - free_cm * 0.5)));
                    if (towards > 0.0)
                    {
                        stereo_consume_room(direction * towards, 0.0);
                        r.moved_cm += towards;
                    }
                }
                if (distance > free_cm)
                {
                    MovementInputParams p{};
                    p.WorldDirection = {direction.x, direction.y, 0.0};
                    p.ScaleValue = (float)std::max(0.25, std::min(1.0, (distance - free_cm) / 30.0));
                    s.pawn->ProcessEvent(r.input_fn, &p);
                }
            }
            r.last_actor = actor;
            r.have_last_actor = true;

            // ---- turns ----
            double base_yaw;
            Vec3 look = view.rotation.rotate({1, 0, 0});
            if (cfg.roomscale_turn_deg > 0.0 && cfg.heading == "aim" && std::fabs(look.z) < 0.85 && stereo_base_yaw(base_yaw))
            {
                double head_yaw = std::atan2(look.y, look.x) * kRadToDeg;
                double off = normalize_axis(head_yaw - base_yaw);
                double over = std::fabs(off) - cfg.roomscale_turn_deg;
                if (over > 0.0)
                {
                    double turn = std::min(over, std::max(0.0, cfg.roomscale_turn_speed) * dt) * (off > 0 ? 1.0 : -1.0);
                    UObject* controller = object_property(s.pawn, STR("Controller"));
                    ue::FRotator* rotation = controller ? controller->GetValuePtrByPropertyNameInChain<ue::FRotator>(STR("ControlRotation")) : nullptr;
                    if (rotation)
                    {
                        rotation->Yaw = normalize_axis(rotation->Yaw + turn);
                        stereo_consume_room({}, turn);
                        r.turned_deg += std::fabs(turn);
                    }
                }
            }

            // ---- ducking ----
            if (cfg.roomscale_crouch_cm > 0.0)
            {
                bool want = r.crouch_held ? view.lean.z < -(cfg.roomscale_crouch_cm - 8.0) : view.lean.z < -cfg.roomscale_crouch_cm;
                if (want != r.crouch_held)
                {
                    if (bool* holding = s.pawn->GetValuePtrByPropertyNameInChain<bool>(STR("Crouch Key Holding"))) *holding = want;
                    r.crouch_held = want;
                    HSVR_LOG(want ? "Room: ducked %.0f cm, asking the game to crouch" : "Room: standing again (%.0f cm), crouch released", -view.lean.z);
                }
            }

            // ---- once a second ----
            static double next_log = 0.0;
            if (cfg.log_arms && now >= next_log)
            {
                next_log = now + 1.0;
                bool* crouched = s.pawn->GetValuePtrByPropertyNameInChain<bool>(STR("Is Crouched??"));
                double* root_height = s.pawn->GetValuePtrByPropertyNameInChain<double>(STR("Root Height"));
                double* height_rate = s.pawn->GetValuePtrByPropertyNameInChain<double>(STR("Height Rate"));
                HSVR_LOG("Room: mode %d | head %.0f cm from over the feet, %.0f cm below standing | this second: feet moved %.0f cm (blocked %.0f), legs turned %.0f deg | "
                         "crouch asked %s, game says crouched %s, root height %.1f, height rate %.2f | crouch button lowers the view %.0f cm",
                         cfg.roomscale, distance, -view.lean.z, r.moved_cm, r.blocked_cm, r.turned_deg, r.crouch_held ? "yes" : "no", crouched ? (*crouched ? "yes" : "no") : "?",
                         root_height ? *root_height : -1.0, height_rate ? *height_rate : -1.0, (double)shared().view_drop_cm.load());
                r.moved_cm = r.turned_deg = r.blocked_cm = 0;
            }
        }

        // ---- self-test (dev_selftest) -----------------------------------------------------------------
        // Runs by itself once a character is being driven: the pretend hands (vr_present.cpp) hold
        // still, move slowly, swing fast and strike the floor while the pretend head ducks; then the
        // character walks forwards and back and turns. Each part's numbers are kept and judged at
        // the end; the verdicts go to the log ("Test:") and to selftest_result.txt.
        struct TestPart
        {
            const char* name;
            double seconds;
            int frames = 0;
            double err_sum[2] = {0, 0}, err_max[2] = {0, 0};
            double turn_sum[2] = {0, 0}, turn_max[2] = {0, 0};
            double weapon_dev_max = 0, travelled = 0, thrown = 0;
            double weapon_turn_max = 0, shield_turn_max = 0; // degrees against the controller (build 2)
            double other_min = 1e9; // how near another character came during this part, cm
            int undone = 0, steps = 0;
            bool weapon_held = true;
        };
        struct SelfTest
        {
            UObject* pawn = nullptr;
            bool finished = false;
            double start = 0, last = 0;
            int part = -1;
            TestPart parts[12] = {{"settle", 6}, {"hands still", 6}, {"hands move slowly", 8}, {"fast swings", 8}, {"striking the floor while ducked", 8},
                                  {"hands still again", 4}, {"twisting the wrists fast", 6}, {"walking", 8}, {"turning", 4},
                                  {"crouch button: crouched, swinging, kicking, walking, then up", 12},
                                  {"crouch-walking into the hanging kettle", 14}, {"left hand raised above the head and turned (shield up, point out)", 6}};
            // Build 2: what the hands hold against the controllers (the turn of each in the
            // controller's frame, taken while the hands are still), and how high the shield goes.
            Quat weapon_ref, shield_ref;
            bool have_weapon_ref = false, have_shield_ref = false, had_shield = false, shield_on_hand = true;
            double shield_z_ref = 0, hand_z_ref = 0, shield_rise = 0, hand_rise = 0;
            int shield_ref_n = 0;
            bool kicked[2] = {false, false};
            bool have_kettle = false, reached_kettle = false;
            Vec3 kettle_at;
            double kettle_near = 1e9;
            double awake_start = -1, awake_min = 1000, awake_ref = -1, awake_drop_max = 0, trap_drop_max = 0;
            double stand_hips_sum = 0;
            int stand_hips_n = 0;
            double opponent_next = 0, opponent_first = -1, opponent_nearest = 1e9; // another character within 3 m: when first, how near
            double crouch_drop_max = 0, crouch_hips_min = 0, crouch_view_end = -1, crouch_head_err_max = 0;
            bool game_crouched = false;
            double weapon_base_sum = 0;
            int weapon_base_n = 0;
            Vec3 last_actor;
            bool have_last_actor = false;
            double thrown_before = 0;
            int undone_before = 0, steps_before = 0;
            bool placed_all_the_time = true, physical_hands = true, mesh_on_character = true;
            double walk_yaw = 0;
            double draw_at = 0;
            bool walk_have_start = false, walk_moved = false;
            Vec3 walk_start;
            int walk_dir = -1;
            double walk_leg_start = 0;
        };
        SelfTest g_test;

        void selftest_tick(const Config& cfg)
        {
            State& s = g_state;
            SelfTest& t = g_test;
            if (!cfg.dev_selftest || t.finished || !s.pawn || !s.ready) return;
            if (t.pawn != s.pawn)
            {
                t = SelfTest{};
                t.pawn = s.pawn;
            }
            double now = now_seconds();
            if (t.start == 0.0)
            {
                // Starts when the body has been placed (or after 12 s without, to report that).
                static double waiting_since = 0.0;
                if (waiting_since == 0.0) waiting_since = now;
                if (!s.kin_active && now - waiting_since < 12.0) return;
                t.start = t.last = now;
                t.thrown_before = s.thrown_cm;
                t.undone_before = s.ground_undone_total;
                t.steps_before = s.steps_total;
                HSVR_LOG("Test: starting (%s)", s.kin_active ? "the body is placed" : "the body was NOT placed after 12 s");
                t.draw_at = now + 1.5;
            }
            if (t.draw_at > 0.0 && now >= t.draw_at)
            {
                t.draw_at = 0.0;
                bool has_weapon = s.pawn->GetPropertyByNameInChain(STR("Weapon R")) && object_property(s.pawn, STR("Weapon R"));
                if (!has_weapon)
                {
                    // An empty right hand: draw from the sheath with the game's own event, if there is one.
                    UFunction* draw = s.pawn->GetFunctionByNameInChain(STR("Unsheath R Event"));
                    if (draw && draw->GetParmsSize() == 0)
                    {
                        s.pawn->ProcessEvent(draw, nullptr);
                        HSVR_LOG("Test: the right hand was empty; asked the game to draw a weapon from its sheath");
                    }
                }
            }
            double dt = std::min(0.1, now - t.last);
            t.last = now;
            double elapsed = now - t.start, begin = 0.0;
            int part = -1;
            for (int i = 0; i < 12; ++i)
            {
                if (elapsed < begin + t.parts[i].seconds)
                {
                    part = i;
                    break;
                }
                begin += t.parts[i].seconds;
            }
            if (part != t.part && t.part >= 0)
            {
                TestPart& done = t.parts[t.part];
                done.thrown = s.thrown_cm - t.thrown_before;
                done.undone = s.ground_undone_total - t.undone_before;
                done.steps = s.steps_total - t.steps_before;
                t.thrown_before = s.thrown_cm;
                t.undone_before = s.ground_undone_total;
                t.steps_before = s.steps_total;
            }
            if (part < 0)
            {
                // ---- verdicts ----
                t.finished = true;
                shared().selftest_phase.store(-1);
                shared().selftest_head_y.store(0.0f);
                std::string report;
                int failures = 0;
                auto line = [&](bool ok, const char* format, ...) {
                    char text[400];
                    va_list args;
                    va_start(args, format);
                    vsnprintf(text, sizeof(text), format, args);
                    va_end(args);
                    // A check that fails in a part during which another character (the arena sends
                    // one in now and then; a hired companion walks beside the player) stood within
                    // arm's reach is noted, not failed: it may have blocked a hand or struck.
                    static const struct { const char* starts; int part; } kParts[] = {
                        {"hands still", 1}, {"slow movement", 2}, {"fast swings", 3}, {"striking the floor", 4}, {"after the floor", 5}, {"twisting", 6},
                        {"walking", 7}, {"turning", 8}, {"crouch", 9}, {"kettle", 10}, {"weapon angle", 2}, {"shield", 11}, {"no hand was lost", -1}, {"nothing struck", -1}};
                    double near_cm = 1e9;
                    for (const auto& entry : kParts)
                    {
                        if (strncmp(text, entry.starts, strlen(entry.starts)) != 0) continue;
                        near_cm = entry.part < 0 ? t.opponent_nearest : t.parts[entry.part].other_min;
                        break;
                    }
                    bool excused = !ok && near_cm < 150.0;
                    if (!ok && !excused) ++failures;
                    std::string full = std::string(ok ? "PASS  " : (excused ? "NOTE  (does not count: another character stood within reach) " : "FAIL  ")) + text;
                    HSVR_LOG("Test: %s", full.c_str());
                    report += full + "\n";
                };
                auto mean = [](const TestPart& p, int k) { return p.frames ? p.err_sum[k] / p.frames : -1.0; };
                const TestPart& still = t.parts[1];
                const TestPart& slow = t.parts[2];
                const TestPart& swing = t.parts[3];
                const TestPart& floor = t.parts[4];
                const TestPart& again = t.parts[5];
                const TestPart& twist = t.parts[6];
                const TestPart& walk = t.parts[7];
                const TestPart& turn = t.parts[8];
                // The character only has a weapon if the player's own save gives it one in the hand
                // or in a sheath (the test draws it); without one the weapon checks say nothing.
                bool armed = t.weapon_base_n > 0;
                if (!armed)
                {
                    HSVR_LOG("Test: the character had no weapon in this run; the weapon checks do not count");
                    report += "NOTE  no weapon in this run: the weapon checks do not count\n";
                }
                line(t.placed_all_the_time, "body placed (not simulated) for the whole test");
                line(t.mesh_on_character, "body mesh rides on the character");
                line(t.physical_hands, "hands physical (wrists free), not the rigid fallback");
                if (armed) line(s.side[1].grip_locked, "weapon fixed rigidly in the right hand (grip joint locked; re-locked %d times)", s.side[1].grip_relocked);
                line(mean(still, 1) >= 0 && mean(still, 1) < 2.5 && mean(still, 0) < 2.5, "hands still: right %.1f cm, left %.1f cm from the controllers on average (limit 2.5)", mean(still, 1), mean(still, 0));
                {
                    // A standing player's eyes are the character's eyes: the hips need not be moved to
                    // put the head at the headset, whatever size the character is.
                    double hips = t.stand_hips_n ? t.stand_hips_sum / t.stand_hips_n : 99.0;
                    line(hips < 4.0, "standing: the character's own height fits the player (hips moved %.1f cm to reach the headset, limit 4; character size %.2f, world scaled %.2f)", hips,
                         (double)shared().body_scale.load(), (double)shared().height_scale.load());
                }
                if (armed) line(still.weapon_dev_max < 2.0, "hands still: weapon moved %.1f cm in the hand at most (limit 2)", still.weapon_dev_max);
                line(slow.err_max[1] < 6.0 && slow.err_max[0] < 6.0, "slow movement: right %.1f cm, left %.1f cm from the controllers at most (limit 6)", slow.err_max[1], slow.err_max[0]);
                if (armed) line(slow.weapon_dev_max < 3.0, "slow movement: weapon moved %.1f cm in the hand at most (limit 3)", slow.weapon_dev_max);
                line(mean(swing, 1) < 12.0, "fast swings: right hand %.1f cm from the controller on average, %.1f at most (limit 12 average)", mean(swing, 1), swing.err_max[1]);
                if (armed) line(swing.weapon_dev_max < 6.0 && swing.weapon_held, "fast swings: weapon moved %.1f cm in the hand at most (limit 6), still held: %s", swing.weapon_dev_max, swing.weapon_held ? "yes" : "NO");
                line(swing.thrown < 5.0 && swing.undone == 0, "fast swings: character moved by something else %.0f cm, taken back %d times (limit 5 cm, 0 times)", swing.thrown, swing.undone);
                line(floor.thrown < 5.0 && floor.undone == 0, "striking the floor: character moved by something else %.0f cm, taken back %d times (limit 5 cm, 0 times)", floor.thrown, floor.undone);
                if (armed) line(floor.weapon_held, "striking the floor: weapon still held");
                line(floor.err_max[1] > 3.0, "striking the floor: the hand was stopped (%.1f cm short of the controller at most; a hand that passes through the floor reads under 3)", floor.err_max[1]);
                line(mean(again, 1) < 3.0 && again.weapon_dev_max < 3.0, "after the floor: right hand %.1f cm from the controller on average, weapon %.1f cm out of place (limits 3)", mean(again, 1), again.weapon_dev_max);
                {
                    double right = twist.frames ? twist.turn_sum[1] / twist.frames : -1.0, left = twist.frames ? twist.turn_sum[0] / twist.frames : -1.0;
                    line(right >= 0 && right < 12.0 && left < 12.0, "twisting the wrists (100 degrees, 2.5 times a second): hands %.0f / %.0f degrees behind on average, %.0f / %.0f at most (limit 12 average)",
                         right, left, twist.turn_max[1], twist.turn_max[0]);
                    line(twist.err_max[1] < 6.0 && twist.err_max[0] < 6.0, "twisting the wrists: hands stayed in place (%.1f / %.1f cm at most, limit 6)", twist.err_max[1], twist.err_max[0]);
                    double swing_turn = swing.frames ? swing.turn_sum[1] / swing.frames : -1.0;
                    line(swing_turn < 12.0, "fast swings: right hand %.0f degrees behind the controller on average, %.0f at most (limit 12 average)", swing_turn, swing.turn_max[1]);
                }
                line(walk.travelled > 250.0, "walking: went %.0f cm (at least 250)", walk.travelled);
                {
                    // A gait, not a patter: at the roughly 4 to 6 m/s the stick gives, 3 to 4.5 steps a second.
                    double seconds = walk.frames > 0 ? t.parts[7].seconds - 0.75 : 1.0;
                    double per_second = walk.steps / seconds;
                    // (The test reverses direction every 2 s, and each restart adds a step or two. The
                    // first stepper, a new step every 20 cm, made about 10 a second here.)
                    // (Since the twelfth cut the test reverses about every second instead of every two.)
                    line(per_second >= 1.5 && per_second <= 8.0, "walking: %.1f steps a second (%d steps; between 1.5 and 8)", per_second, walk.steps);
                }
                line(mean(walk, 1) < 6.0 && mean(walk, 0) < 6.0, "walking: hands %.1f / %.1f cm from the controllers on average (limit 6)", mean(walk, 1), mean(walk, 0));
                line(walk.thrown < 5.0 && walk.undone == 0, "walking: character moved by something else %.0f cm, taken back %d times", walk.thrown, walk.undone);
                line(turn.steps >= 2, "turning: %d steps taken (at least 2)", turn.steps);
                {
                    const TestPart& crouch = t.parts[9];
                    line(t.crouch_drop_max > 35.0 && t.crouch_drop_max < 45.0, "crouch button: the view went down %.0f cm (40 expected)", t.crouch_drop_max);
                    line(t.crouch_hips_min < -30.0, "crouch button: the hips went down %.0f cm with it (at least 30)", -t.crouch_hips_min);
                    line(t.crouch_head_err_max < 3.0, "crouch button: the head stayed at the headset's place (%.1f cm off at most, limit 3)", t.crouch_head_err_max);
                    line(t.crouch_view_end >= 0 && t.crouch_view_end < 1.0, "crouch button: back up afterwards (view %.0f cm down at the end)", t.crouch_view_end);
                    line(!t.game_crouched, "crouch button: the game's own crouch stayed out of it");
                    line(mean(crouch, 1) < 8.0 && mean(crouch, 0) < 8.0, "crouched, swinging and walking: hands %.1f / %.1f cm from the controllers on average (limit 8)", mean(crouch, 1), mean(crouch, 0));
                    line(crouch.thrown < 5.0 && crouch.undone == 0, "crouched: character moved by something else %.0f cm, taken back %d times", crouch.thrown, crouch.undone);
                    if (armed) line(crouch.weapon_held, "crouched: weapon still held");
                    line(t.awake_drop_max < 6.0, "nothing struck the character during the test (largest sudden drop in consciousness %.0f, limit 6; it drifted from %.0f to %.0f at its lowest by itself)",
                         t.awake_drop_max, t.awake_start, t.awake_min);
                }
                {
                    const TestPart& kettle = t.parts[10];
                    if (!t.have_kettle)
                    {
                        HSVR_LOG("Test: no hanging kettle in this level; that part does not count");
                        report += "NOTE  no hanging kettle in this level: that part does not count\n";
                    }
                    else
                    {
                        line(t.reached_kettle, "kettle: the crouched character walked into it (came within %.0f cm of where it hangs)", t.kettle_near);
                        line(kettle.thrown < 5.0 && kettle.undone == 0, "kettle: character moved by something else %.0f cm, taken back %d times", kettle.thrown, kettle.undone);
                        line(mean(kettle, 1) < 6.0 && mean(kettle, 0) < 6.0 && kettle.err_max[1] < 40.0 && kettle.err_max[0] < 40.0,
                             "kettle: hands %.1f / %.1f cm from the controllers on average, %.0f / %.0f at most (limits 6 and 40)", mean(kettle, 1), mean(kettle, 0), kettle.err_max[1],
                             kettle.err_max[0]);
                        if (armed) line(kettle.weapon_held, "kettle: weapon still held");
                        if (t.trap_drop_max >= 1.0)
                        {
                            char text[200];
                            snprintf(text, sizeof(text), "NOTE  the kettle (a trap) struck the character as it walked into it: consciousness down %.0f\n", t.trap_drop_max);
                            HSVR_LOG("Test: the kettle (a trap) struck the character as it walked into it: consciousness down %.0f (noted, not a fault)", t.trap_drop_max);
                            report += text;
                        }
                    }
                }
                {
                    // Build 2, Matthew's point 5: what the hand holds keeps one angle to the controller.
                    if (armed)
                        line(t.have_weapon_ref && t.parts[2].weapon_turn_max < 6.0 && t.parts[5].weapon_turn_max < 6.0,
                             "weapon angle: the weapon kept one angle to the controller (turned against it by %.1f degrees at most while the hands moved slowly, %.1f when still again; "
                             "limit 6; grip_fixed %d, grip_tilt_deg %.0f)",
                             t.parts[2].weapon_turn_max, t.parts[5].weapon_turn_max, (int)cfg.grip_fixed, cfg.grip_tilt_deg);
                    // Point 2: the shield goes where the hand goes and turns with it.
                    if (!t.had_shield)
                    {
                        HSVR_LOG("Test: nothing in the left hand in this run; the shield checks do not count");
                        report += "NOTE  nothing in the left hand in this run: the shield checks do not count\n";
                    }
                    else
                    {
                        const TestPart& up = t.parts[11];
                        line(t.shield_on_hand, "shield: joined to the hand, not the forearm, for the whole test (shield_on_hand %d)", (int)cfg.shield_on_hand);
                        line(t.hand_rise > 30.0 && t.shield_rise > 0.8 * t.hand_rise, "shield raised: it rose %.0f cm as the hand rose %.0f cm (at least 80%% of it)", t.shield_rise, t.hand_rise);
                        line(t.have_shield_ref && up.shield_turn_max < 10.0 && t.parts[2].shield_turn_max < 10.0,
                             "shield angle: it kept one angle to the controller (turned against it by %.1f degrees at most while raised and turned 35 degrees, %.1f while moving slowly; limit 10)",
                             up.shield_turn_max, t.parts[2].shield_turn_max);
                    }
                }
                line(s.side[0].rescues + s.side[1].rescues == 0, "no hand was lost and had to be brought back (%d times)", s.side[0].rescues + s.side[1].rescues);
                line(turn.err_max[1] < 8.0 && turn.err_max[0] < 8.0, "turning: hands %.1f / %.1f cm from the controllers at most (limit 8)", turn.err_max[1], turn.err_max[0]);
                if (t.opponent_first >= 0)
                {
                    char text[220];
                    snprintf(text, sizeof(text), "NOTE  another character came within 3 m from second %.0f of the test on (nearest %.0f cm): later hand and blow checks may be its doing\n",
                             t.opponent_first, t.opponent_nearest);
                    HSVR_LOG("Test: another character came within 3 m from second %.0f of the test on (nearest %.0f cm): later hand and blow checks may be its doing", t.opponent_first, t.opponent_nearest);
                    report += text;
                }
                const char* verdict = failures ? "FAILED" : (armed ? "PASSED" : "PASSED WITHOUT A WEAPON (weapon checks not run)");
                HSVR_LOG("Test: %s (%d failed)", verdict, failures);
                report += std::string("RESULT: ") + verdict + "\n";
                std::wstring path = mod_dir() + L"selftest_result.txt";
                if (FILE* file = _wfopen(path.c_str(), L"w"))
                {
                    fputs(report.c_str(), file);
                    fclose(file);
                }
                return;
            }
            if (part != t.part)
            {
                t.part = part;
                HSVR_LOG("Test: part %d of 12: %s", part + 1, t.parts[part].name);
            }
            shared().selftest_phase.store(part);
            shared().selftest_phase_time.store((float)(elapsed - begin));
            // The pretend head ducks half a metre for the floor strikes (eased over 0.4 s).
            {
                float want = part == 4 ? -0.5f : 0.0f;
                float have = shared().selftest_head_y.load();
                float step = (float)(1.25 * dt);
                have = have < want ? std::min(want, have + step) : std::max(want, have - step);
                shared().selftest_head_y.store(have);
            }
            // ---- what the character is made to do ----
            if (part == 7)
            {
                // Walking, through the virtual gamepad's left stick (controller_input.cpp): backwards
                // first, away from what the character faces at its start, then to and fro between
                // 1.2 and 3.2 m behind the start, at a walk. (In the cellar the character starts at arm's length
                // from a training dummy. Walking forward took the weapon hand through the dummy's
                // shield: a sickle hooked it, stuck, and the hand stayed behind, which is what a
                // physical hand should do and says nothing about walking.) A leg backwards also
                // ends after 1.6 s, for a wall.
                Vec3 here;
                if (actor_location(s.pawn, here))
                {
                    if (!t.walk_have_start)
                    {
                        t.walk_have_start = true;
                        t.walk_start = here;
                        t.walk_dir = -1;
                        t.walk_leg_start = elapsed;
                    }
                    double away = length(Vec3{here.x - t.walk_start.x, here.y - t.walk_start.y, 0});
                    if (t.walk_dir < 0 && away < 20.0 && !t.walk_moved) t.walk_leg_start = elapsed; // (the gamepad takes a moment to be noticed)
                    if (away >= 20.0) t.walk_moved = true;
                    double leg = elapsed - t.walk_leg_start;
                    if (t.walk_dir < 0 && (away > 320.0 || leg > 1.6))
                    {
                        t.walk_dir = 1;
                        t.walk_leg_start = elapsed;
                    }
                    else if (t.walk_dir > 0 && (away < 120.0 || leg > 1.6))
                    {
                        t.walk_dir = -1;
                        t.walk_leg_start = elapsed;
                    }
                    // (A big shield and a long weapon catch on the cellar's stairs and walls further out;
                    // that is the level stopping a physical thing, and no business of this part.)
                    shared().selftest_stick_y.store(0.6f * (float)t.walk_dir);
                }
            }
            else if (part == 9)
            {
                // The crouch button's own flag: down for 9 s (the pretend right hand swings low for
                // part of it and the stick walks, see vr_present.cpp and controller_input.cpp), then up.
                shared().own_crouch.store(elapsed - begin > 0.5 && elapsed - begin < 9.5);
                // Two kicks while crouched, by the game's own kick (Matthew: "kicking whilst crouching damages me").
                for (int n = 0; n < 2; ++n)
                {
                    if (!t.kicked[n] && elapsed - begin > (n ? 4.5 : 2.5))
                    {
                        t.kicked[n] = true;
                        UFunction* kick = s.pawn->GetFunctionByNameInChain(STR("Kick Event"));
                        if (kick && kick->GetParmsSize() == 0) s.pawn->ProcessEvent(kick, nullptr);
                    }
                }
                t.crouch_drop_max = std::max(t.crouch_drop_max, (double)shared().view_drop_cm.load());
                t.crouch_hips_min = std::min(t.crouch_hips_min, s.pelvis_drop);
                if (elapsed - begin > 11.5) t.crouch_view_end = (double)shared().view_drop_cm.load();
                if (bool* crouched = s.pawn->GetValuePtrByPropertyNameInChain<bool>(STR("Is Crouched??")))
                {
                    if (*crouched) t.game_crouched = true;
                }
                Xf head_now;
                if (s.have_head_target && elapsed - begin > 2.0 && elapsed - begin < 9.0 && socket_transform(s.body, make_name(L"head"), RTS_World, head_now))
                    t.crouch_head_err_max = std::max(t.crouch_head_err_max, length(head_now.t - s.head_target_world));
            }
            else if (part == 10)
            {
                // Crouched, walk at the hanging kettle (a trap: a weapon-type object on a chain), push
                // into it for a while, then back off. The stick goes through the virtual gamepad; the
                // heading is turned at the kettle.
                double pt = elapsed - begin;
                shared().own_crouch.store(pt > 0.3 && pt < 12.5);
                if (!t.have_kettle && pt < 0.5)
                {
                    std::vector<UObject*> items;
                    UObjectGlobals::FindAllOf(STR("ModularWeaponBP_C"), items);
                    double best = 1e9;
                    for (UObject* item : items)
                    {
                        Vec3 at;
                        if (!item || std::wstring(item->GetClassPrivate()->GetName()).find(L"Kettle") == std::wstring::npos || !actor_location(item, at)) continue;
                        double d = length(at - s.actor_world);
                        if (d < best)
                        {
                            best = d;
                            t.kettle_at = at;
                            t.have_kettle = true;
                        }
                    }
                    HSVR_LOG(t.have_kettle ? "Test: the hanging kettle is %.0f cm away" : "Test: no hanging kettle in this level (%.0f)", t.have_kettle ? best : 0.0);
                }
                float stick = 0.0f;
                bool stick_allowed = true;
                if (t.have_kettle)
                {
                    Vec3 to{t.kettle_at.x - s.actor_world.x, t.kettle_at.y - s.actor_world.y, 0};
                    double d = length(to);
                    t.kettle_near = std::min(t.kettle_near, d);
                    if (d < 45.0) t.reached_kettle = true;
                    UObject* controller = object_property(s.pawn, STR("Controller"));
                    ue::FRotator* rotation = controller ? controller->GetValuePtrByPropertyNameInChain<ue::FRotator>(STR("ControlRotation")) : nullptr;
                    if (rotation && d > 5.0 && pt < 10.0)
                    {
                        double off = normalize_axis(std::atan2(to.y, to.x) * kRadToDeg - rotation->Yaw);
                        double step = 120.0 * dt; // degrees a second, like a stick turn
                        rotation->Yaw = normalize_axis(rotation->Yaw + std::max(-step, std::min(step, off)));
                        if (std::fabs(off) > 30.0) stick_allowed = false; // turn first, then walk
                    }
                    stick = pt < 10.0 ? (stick_allowed ? 0.6f : 0.0f) : (pt < 12.5 ? -0.6f : 0.0f); // towards it and into it, then away
                }
                shared().selftest_stick_y.store(stick);
            }
            else if (part == 8)
            {
                UObject* controller = object_property(s.pawn, STR("Controller"));
                ue::FRotator* rotation = controller ? controller->GetValuePtrByPropertyNameInChain<ue::FRotator>(STR("ControlRotation")) : nullptr;
                if (rotation) rotation->Yaw = normalize_axis(rotation->Yaw + 60.0 * dt);
            }
            if (s.consciousness >= 0)
            {
                if (t.awake_start < 0) t.awake_start = t.awake_ref = s.consciousness;
                t.awake_min = std::min(t.awake_min, s.consciousness);
                // Consciousness drifts by itself (a point every second or two, down while the arms
                // work and back up while walking; it did so standing still at the start of every
                // run). A blow is a sudden drop: more than the drift allows within a moment.
                double change = s.consciousness - t.awake_ref;
                t.awake_ref += change > 0 ? change : std::max(change, -2.5 * dt);
                if (now_seconds() - s.trap_struck_time < 3.0)
                {
                    // The hanging kettle (a trap) or a loose weapon did this: noted, not a fault.
                    t.trap_drop_max = std::max(t.trap_drop_max, t.awake_ref - s.consciousness);
                    t.awake_ref = s.consciousness;
                }
                t.awake_drop_max = std::max(t.awake_drop_max, t.awake_ref - s.consciousness);
            }
            if (part == 5 && elapsed - begin < 0.5)
            {
                // After striking the floor: a weapon whose point stuck in it is tugged free, as a
                // player would (the mod frees it by itself only after a 25 cm pull).
                for (const TCHAR* name : {STR("Weapon R"), STR("Weapon L")})
                {
                    UObject* weapon = s.pawn->GetPropertyByNameInChain(name) ? object_property(s.pawn, name) : nullptr;
                    FProperty* list = weapon ? weapon->GetPropertyByNameInChain(STR("Stuck Constraints Array")) : nullptr;
                    int32_t stuck = list ? *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(weapon) + list->GetOffset_Internal() + 8) : 0;
                    UFunction* release = stuck > 0 ? weapon->GetFunctionByNameInChain(STR("Release All Jammed Bones")) : nullptr;
                    if (release && release->GetParmsSize() == 0)
                    {
                        weapon->ProcessEvent(release, nullptr);
                        HSVR_LOG("Test: a weapon stuck in the floor was tugged free");
                    }
                }
            }
            if (now >= t.opponent_next && s.have_actor_world)
            {
                // The arena sometimes sends somebody in. What an opponent does to the character is not
                // the mod's doing: the result says so.
                t.opponent_next = now + 0.5;
                std::vector<UObject*> others;
                UObjectGlobals::FindAllOf(STR("Willie_BP_C"), others);
                for (UObject* other : others)
                {
                    Vec3 at;
                    if (!other || other == s.pawn || !actor_location(other, at)) continue;
                    double away = length(at - s.actor_world);
                    if (away > 300.0) continue;
                    if (t.opponent_first < 0) t.opponent_first = elapsed;
                    t.opponent_nearest = std::min(t.opponent_nearest, away);
                    if (part >= 0 && part < (int)(sizeof(t.parts) / sizeof(t.parts[0]))) t.parts[part].other_min = std::min(t.parts[part].other_min, away);
                }
            }
            if (part == 1)
            {
                t.stand_hips_sum += std::fabs(s.pelvis_drop);
                ++t.stand_hips_n;
            }
            // ---- what is measured ----
            TestPart& p = t.parts[part];
            if (elapsed - begin < 0.75) return; // let each part settle in before measuring it
            ++p.frames;
            for (int k = 0; k < 2; ++k)
            {
                p.err_sum[k] += s.side[k].last_error_cm;
                p.err_max[k] = std::max(p.err_max[k], s.side[k].last_error_cm);
                p.turn_sum[k] += s.side[k].last_error_deg;
                p.turn_max[k] = std::max(p.turn_max[k], s.side[k].last_error_deg);
            }
            if (!s.kin_active) t.placed_all_the_time = false;
            if (part >= 1 && s.kin_active && s.kin_hands) t.physical_hands = false;
            if (s.kin_active && !body_attached_to_capsule()) t.mesh_on_character = false;
            Vec3 actor;
            if (actor_location(s.pawn, actor))
            {
                if (t.have_last_actor) p.travelled += length(Vec3{actor.x - t.last_actor.x, actor.y - t.last_actor.y, 0});
                t.last_actor = actor;
                t.have_last_actor = true;
            }
            // The weapon against the hand that holds it: its distance from the hand bone should not change.
            UObject* weapon = s.pawn->GetPropertyByNameInChain(STR("Weapon R")) ? object_property(s.pawn, STR("Weapon R")) : nullptr;
            Vec3 weapon_at;
            Xf hand;
            if (weapon && actor_location(weapon, weapon_at) && socket_transform(s.body, s.side[1].hand_name, RTS_World, hand))
            {
                // In the hand's own frame, so turning the hand does not count as the weapon moving.
                Vec3 local = hand.q.inverse().rotate(weapon_at - hand.t);
                static Vec3 base;
                if (part == 1)
                {
                    base = t.weapon_base_n ? (base * (double)t.weapon_base_n + local) * (1.0 / (t.weapon_base_n + 1)) : local;
                    ++t.weapon_base_n;
                }
                if (t.weapon_base_n > 0) p.weapon_dev_max = std::max(p.weapon_dev_max, length(local - base));
                if (length(weapon_at - hand.t) > 60.0) p.weapon_held = false;
            }
            else
            {
                p.weapon_held = false;
            }
            // Build 2: the held things' turn in each controller's frame. The reference is the
            // first steady frame of "hands still"; afterwards the largest difference from it per
            // part (in the raise, once the hand has arrived).
            for (int k = 0; k < 2; ++k)
            {
                const TCHAR* name = k ? STR("Weapon R") : STR("Weapon L");
                UObject* held = s.pawn->GetPropertyByNameInChain(name) ? object_property(s.pawn, name) : nullptr;
                UObject* base = held && held->GetPropertyByNameInChain(STR("BaseMesh")) ? object_property(held, STR("BaseMesh")) : nullptr;
                Xf thing;
                if (!base || !s.side[k].target_valid || !component_world(base, thing)) continue;
                Quat rel = (s.side[k].ctrl_world.inverse() * thing.q).normalized();
                Quat& ref = k ? t.weapon_ref : t.shield_ref;
                bool& have = k ? t.have_weapon_ref : t.have_shield_ref;
                if (part == 1 && !have)
                {
                    ref = rel;
                    have = true;
                }
                if (have && !(part == 11 && elapsed - begin < 3.0))
                {
                    double off = quat_angle_deg(rel, ref);
                    double& worst = k ? p.weapon_turn_max : p.shield_turn_max;
                    worst = std::max(worst, off);
                }
                if (k == 0)
                {
                    t.had_shield = true;
                    if (part >= 1 && s.kin_active && !s.side[0].shield_on_hand && cfg.shield_on_hand) t.shield_on_hand = false;
                    if (part == 1)
                    {
                        t.shield_z_ref = (t.shield_z_ref * t.shield_ref_n + thing.t.z) / (t.shield_ref_n + 1);
                        t.hand_z_ref = (t.hand_z_ref * t.shield_ref_n + s.side[0].target_world.t.z) / (t.shield_ref_n + 1);
                        ++t.shield_ref_n;
                    }
                    if (part == 11 && elapsed - begin > 3.0 && t.shield_ref_n > 0)
                    {
                        t.shield_rise = thing.t.z - t.shield_z_ref;
                        t.hand_rise = s.side[0].target_world.t.z - t.hand_z_ref;
                    }
                }
            }
        }

        // ---- recorder: every frame for a few seconds, started with the right trigger ----------
        // Writes Mods\HalfSwordVR\arms_record_<n>.csv. Per arm: how far the physical hand is from
        // where it was sent, and the wrist (hand relative to forearm) as sent and as it really is,
        // split into bend (swing) and roll (twist about the forearm), to see whether the wrist stops
        // at a limit or the muscles are too slow.
        struct Recorder
        {
            bool active = false;
            double start = 0, until = 0, cooldown_until = 0;
            int number = 0;
            std::string text;
            Quat last_target[2];
            double last_time = 0;
        };
        Recorder g_rec;

        void swing_twist_deg(const Quat& rel, const Vec3& axis, double& swing, double& twist)
        {
            double tw = 2.0 * std::atan2(rel.x * axis.x + rel.y * axis.y + rel.z * axis.z, rel.w);
            if (tw > kPi) tw -= 2.0 * kPi;
            if (tw < -kPi) tw += 2.0 * kPi;
            twist = tw * kRadToDeg;
            Vec3 bent = rel.rotate(axis);
            swing = std::acos(std::max(-1.0, std::min(1.0, dot(bent, axis)))) * kRadToDeg;
        }

        void recorder_tick()
        {
            State& s = g_state;
            double now = now_seconds();
            if (!g_rec.active)
            {
                if (config().arms_recorder && now >= g_rec.cooldown_until && s.hands[1].valid && s.hands[1].trigger > 0.7f && !s.menu_up && s.ready)
                {
                    g_rec.active = true;
                    g_rec.start = now;
                    g_rec.until = now + 8.0;
                    g_rec.number++;
                    g_rec.last_time = 0;
                    g_rec.text = "t,side,err_cm,err_deg,target_speed_dps,sent_wrist_bend,sent_wrist_roll,real_wrist_bend,real_wrist_roll,sent_elbow,real_elbow,reach,elbow_swing\n";
                    HSVR_LOG("Arms: recording 8 seconds (right trigger)");
                }
                return;
            }
            if (now < g_rec.until) return;
            g_rec.active = false;
            g_rec.cooldown_until = now + 2.0;
            char name[64];
            snprintf(name, sizeof(name), "arms_record_%d.csv", g_rec.number);
            std::wstring path = mod_dir() + std::wstring(name, name + strlen(name));
            if (FILE* f = _wfopen(path.c_str(), L"w"))
            {
                fputs(g_rec.text.c_str(), f);
                fclose(f);
            }
            HSVR_LOG("Arms: recording saved as %s", name);
        }

        void record_frame(const std::vector<Xf>& pose)
        {
            State& s = g_state;
            if (!g_rec.active) return;
            double now = now_seconds();
            double dt = g_rec.last_time > 0 ? now - g_rec.last_time : 0;
            for (int k = 0; k < 2; ++k)
            {
                Side& side = s.side[k];
                if (!side.target_valid || !side.have_axes) continue;
                Xf hand, lower, upper;
                if (!socket_transform(s.body, side.hand_name, RTS_World, hand) || !socket_transform(s.body, side.lower_name, RTS_World, lower) ||
                    !socket_transform(s.body, side_name(L"upperarm", side), RTS_World, upper))
                    continue;
                double speed = dt > 0 ? quat_angle_deg(g_rec.last_target[k], side.target_world.q) / dt : 0;
                g_rec.last_target[k] = side.target_world.q;
                double sb, sr, rb, rr, se, re, dummy;
                swing_twist_deg(pose[side.lower].q.inverse() * pose[side.hand].q, side.lower_axis, sb, sr);
                swing_twist_deg(lower.q.inverse() * hand.q, side.lower_axis, rb, rr);
                swing_twist_deg(pose[side.upper].q.inverse() * pose[side.lower].q, side.upper_axis, se, dummy);
                swing_twist_deg(upper.q.inverse() * lower.q, side.upper_axis, re, dummy);
                char line[256];
                snprintf(line, sizeof(line), "%.3f,%c,%.1f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.2f,%.0f\n", now - g_rec.start, k ? 'R' : 'L', length(hand.t - side.target_world.t),
                         quat_angle_deg(hand.q, side.target_world.q), speed, sb, sr, rb, rr, se, re, side.last_reach, side.elbow_angle);
                g_rec.text += line;
            }
            g_rec.last_time = now;
        }

        bool same_raw(const RawXf& a, const RawXf& b) { return memcmp(a.q, b.q, sizeof(a.q)) == 0 && memcmp(a.t, b.t, sizeof(a.t)) == 0; }

        void before_body_update()
        {
            State& s = g_state;
            if (!s.ready || s.failed || !s.enabled_by_key || s.menu_up) return;
            Config cfg = config();
            if (!cfg.motion_arms) return;
            bool arms = s.hands_valid && (s.side[0].target_valid || s.side[1].target_valid);
            bool body = cfg.body_follow && s.head >= 0 && s.spine_base >= 0;
            if (!arms && !body) return;
            RawXf* buffer = driver_read_buffer(s.driver, s.bone_count);
            if (!buffer) return;
            bool stale = s.have_animated && s.marker_bone >= 0 && same_raw(buffer[s.marker_bone], s.written_marker);
            if (stale && s.written_frame == s.frame) return; // done already this frame
            if (!stale)
            {
                // The driver has animated since we last wrote: keep its own pose and learn from it.
                s.animated.assign(buffer, buffer + s.bone_count);
                s.have_animated = true;
                ++s.fresh_count;
                for (Side& side : s.side) learn_from_animation(side, s.animated.data());
            }
            else
            {
                ++s.stale_count;
            }
            // Where the body and the view are right now (the character has already moved this frame).
            s.have_body_world = component_world(s.body, s.body_world);
            s.have_view = stereo_head_world(s.view);
            s.actor_delta = {};
            Vec3 actor_now;
            if (s.have_view && actor_location(s.pawn, actor_now))
            {
                s.actor_delta = actor_now - s.view.actor_position;
                {
                    double now = now_seconds();
                    double dt = s.ground_time > 0 ? std::min(0.1, now - s.ground_time) : 0.0;
                    s.ground_time = now;
                    UObject* movement = object_property(s.pawn, STR("CharacterMovement"));
                    ue::FVector* velocity = movement ? movement->GetValuePtrByPropertyNameInChain<ue::FVector>(STR("Velocity")) : nullptr;
                    if (s.have_actor_world && velocity && dt > 0.0)
                    {
                        // What walking (the movement component's own speed) and the player's own
                        // steps account for is the most the character can have gone by itself. Going
                        // less (a wall, a slope) is normal. Going more than 15 cm further in a single
                        // frame was done to it by something else (the game's capsule leash jumped it
                        // 20 cm and more at a time) and is taken back. Smaller differences are left
                        // alone: the game's own running moves the character faster than the movement
                        // component's speed says, and treating that as foreign made walking stutter
                        // (third self-test); a wall stopping the character must not count either
                        // (second self-test: "taken back" into the wall).
                        s.walked_cm += length(Vec3{velocity->X, velocity->Y, 0}) * dt;
                        s.stepped_cm += length(Vec3{s.room_step.x, s.room_step.y, 0});
                        if (ue::FVector* input = s.pawn->GetValuePtrByPropertyNameInChain<ue::FVector>(STR("Movement Input Vector")))
                        {
                            double size = std::sqrt(input->X * input->X + input->Y * input->Y);
                            if (size > s.input_most)
                            {
                                Vec3 ahead = quat_from_yaw_deg(s.actor_yaw).rotate({1, 0, 0});
                                s.input_most = size;
                                s.input_forward = input->X * ahead.x + input->Y * ahead.y;
                            }
                        }
                        Vec3 gone = actor_now - s.actor_world;
                        gone.z = 0; // slopes, steps and landing are the movement component's business
                        Vec3 own = Vec3{velocity->X, velocity->Y, 0} * dt + Vec3{s.room_step.x, s.room_step.y, 0};
                        double extra = length(gone) - length(own) - 1.0;
                        bool teleport = length(gone) > 250.0; // a real teleport (arena start, respawn)
                        s.foreign_cm = teleport ? 0.0 : std::max(0.0, extra);
                        // (A long frame explains more: the game's running outpaces the movement
                        // component's speed by up to a few metres a second.)
                        if (!teleport && s.foreign_cm > 15.0 + 350.0 * dt)
                        {
                            s.thrown_cm += s.foreign_cm;
                            double undo = std::min(length(gone), s.foreign_cm);
                            if (cfg.hold_ground && s.kin_active && length(gone) > 1e-3)
                            {
                                Vec3 back = actor_now - gone * (undo / length(gone));
                                UFunction* set_fn = s.pawn->GetFunctionByNameInChain(STR("K2_SetActorLocation"));
                                int32_t o_loc = set_fn ? param_offset(set_fn, L"NewLocation") : -1, o_sweep = set_fn ? param_offset(set_fn, L"bSweep") : -1,
                                        o_tp = set_fn ? param_offset(set_fn, L"bTeleport") : -1;
                                if (set_fn && o_loc >= 0 && o_sweep >= 0 && o_tp >= 0 && set_fn->GetParmsSize() <= 1024)
                                {
                                    alignas(16) uint8_t parms[1024] = {};
                                    ue::FVector to{back.x, back.y, back.z};
                                    memcpy(parms + o_loc, &to, sizeof(to));
                                    parms[o_sweep] = 1; // never through a wall
                                    parms[o_tp] = 1;
                                    s.pawn->ProcessEvent(set_fn, parms);
                                    actor_location(s.pawn, actor_now);
                                    ++s.ground_undone;
                                    ++s.ground_undone_total;
                                    s.ground_undone_cm += undo;
                                }
                            }
                            if (log_throttle("thrown", 0.5))
                                HSVR_LOG("Room: something other than walking moved the character %.0f cm further than its own speed (%.0f cm/s) explains%s", s.foreign_cm,
                                         length(Vec3{velocity->X, velocity->Y, 0}), cfg.hold_ground && s.kin_active ? "; taken back" : "");
                            s.foreign_cm = 0;
                        }
                    }
                    s.room_step = {};
                }
                s.actor_delta = actor_now - s.view.actor_position;
                s.actor_world = actor_now;
                s.have_actor_world = true;
                if (length(s.actor_delta) > 100.0) s.actor_delta = {}; // a teleport, not a step
            }
            s.head_pull_wanted = body && s.have_body_world && s.have_view && s.view.head_valid;
            // The placed body: where the mesh belongs over the feet, and whether the character is
            // in a state to be placed at all.
            s.have_nominal = false;
            {
                ue::FVector* offset = s.pawn->GetValuePtrByPropertyNameInChain<ue::FVector>(STR("BaseTranslationOffset"));
                double* turn = s.pawn->GetValuePtrByPropertyNameInChain<double>(STR("BaseRotationOffset")); // a quaternion: x y z w
                double yaw_now = s.actor_yaw;
                Vec3 at;
                if (offset && turn && actor_yaw(s.pawn, yaw_now) && actor_location(s.pawn, at))
                {
                    s.actor_yaw = yaw_now;
                    Xf local{Quat{turn[0], turn[1], turn[2], turn[3]}.normalized(), Vec3{offset->X, offset->Y, offset->Z}};
                    double size = s.have_body_world ? s.body_world.s : 1.0;
                    // The character's height is the actor's scale (capsule and all; the mesh hangs in
                    // it at its usual offset times that). The mesh's own size in the world is
                    // another number: the game lets it creep back towards 1 as the character lives.
                    double actor_size = 1.0;
                    if (UFunction* size_fn = s.pawn->GetFunctionByNameInChain(STR("GetActorScale3D")))
                    {
                        if (size_fn->GetParmsSize() == (int32_t)sizeof(ue::FVector))
                        {
                            ue::FVector out{1, 1, 1};
                            s.pawn->ProcessEvent(size_fn, &out);
                            if (out.Z > 0.3 && out.Z < 3.0) actor_size = out.Z;
                        }
                    }
                    s.nominal = compose(local, Xf{quat_from_yaw_deg(yaw_now), at, actor_size});
                    s.nominal.s = size;
                    s.have_nominal = true;
                    // For the view (stereo_device.cpp): how big the character is, where its feet
                    // are and how high its eyes are when it stands.
                    shared().body_scale.store((float)size);
                    shared().feet_below_actor_cm.store((float)(at.z - s.nominal.t.z));
                    if (s.have_rest && s.rest_head_z > 50.0) shared().willie_eye_cm.store((float)((s.rest_head_z + config().eye_up_cm) * size));
                    static double size_told = 0.0;
                    if (std::fabs(size - size_told) > 0.005)
                    {
                        size_told = size;
                        HSVR_LOG("Body: the character is %.2f of the standard height, its body mesh %.2f of the standard size (feet %.0f cm below its centre)", actor_size, size, at.z - s.nominal.t.z);
                    }
                    if (s.have_body_world) s.mesh_off_cm = length(s.body_world.t - s.nominal.t);
                }
            }
            {
                // A body part's health dropping: say at once which, and what the game's own hit
                // record names as the other thing (for the log; at most 40 lines a session).
                static UClass* watched_class = nullptr;
                static std::vector<std::pair<std::wstring, int32_t>> watched;
                static std::vector<double> last;
                static int lines = 0;
                if (watched_class != s.pawn->GetClassPrivate())
                {
                    watched_class = s.pawn->GetClassPrivate();
                    watched.clear();
                    last.clear();
                    for (UStruct* type = watched_class; type; type = type->GetSuperStruct())
                    {
                        if (std::wstring(type->GetName()) != L"Willie_BP_C") continue;
                        for (FProperty* p : type->ForEachProperty())
                        {
                            std::wstring name = p->GetName();
                            if (p->GetClass().GetName() != STR("DoubleProperty") || name.find(L"Health") == std::wstring::npos) continue;
                            watched.emplace_back(name, p->GetOffset_Internal());
                        }
                    }
                }
                bool first = last.size() != watched.size();
                if (first) last.assign(watched.size(), 0.0);
                for (size_t i = 0; i < watched.size(); ++i)
                {
                    double value = *reinterpret_cast<double*>(reinterpret_cast<uint8_t*>(s.pawn) + watched[i].second);
                    if (!first && value < last[i] - 3.0 && lines < 40)
                    {
                        ++lines;
                        UObject* other = s.pawn->GetPropertyByNameInChain(STR("Other Actor")) ? object_property(s.pawn, STR("Other Actor")) : nullptr;
                        UObject* part = s.pawn->GetPropertyByNameInChain(STR("Other Comp")) ? object_property(s.pawn, STR("Other Comp")) : nullptr;
                        FName* bone = s.pawn->GetValuePtrByPropertyNameInChain<FName>(STR("Body Hit Bone Name Self"));
                        for (UStruct* type = other ? other->GetClassPrivate() : nullptr; type; type = type->GetSuperStruct())
                        {
                            if (std::wstring(type->GetName()) != L"ModularWeaponBP_C") continue;
                            bool* is_held = other->GetValuePtrByPropertyNameInChain<bool>(STR("Is Held"));
                            if (is_held && !*is_held) s.trap_struck_time = now_seconds();
                            break;
                        }
                        double* impulse = s.pawn->GetValuePtrByPropertyNameInChain<double>(STR("Body Hit Normal Impulse"));
                        bool own_item = other && (other == g_held_item[0].load() || other == g_held_item[1].load());
                        HSVR_LOG("Hit: %ls fell from %.0f to %.0f | the game's last body hit: by %ls%s (%ls) of class %ls on %ls, impulse %.0f | placed %s, crouched %s",
                                 watched[i].first.c_str(), last[i], value, other ? other->GetName().c_str() : L"nothing", own_item ? L" [held by the player]" : L"",
                                 part ? part->GetName().c_str() : L"-",
                                 other ? other->GetClassPrivate()->GetName().c_str() : L"-", bone ? bone->ToString().c_str() : L"?",
                                 impulse ? std::sqrt(impulse[0] * impulse[0] + impulse[1] * impulse[1] + impulse[2] * impulse[2]) : -1.0,
                                 s.kin_active ? "yes" : "no", shared().own_crouch.load() ? "yes" : "no");
                    }
                    last[i] = value;
                }
            }
            if (double* awake = s.pawn->GetValuePtrByPropertyNameInChain<double>(STR("Consciousness")))
            {
                // A sudden drop: something struck the character. Say what hurts and who is near, so
                // that the log can tell an enemy's blow from the character hurting itself.
                static double reference = -1.0, reference_time = 0.0;
                static std::map<std::wstring, double> before;
                double now_hurt = now_seconds();
                bool struck = reference >= 0 && *awake < reference - 6.0;
                if (reference < 0 || now_hurt - reference_time > 1.0 || struck)
                {
                    static UClass* listed_class = nullptr;
                    static std::vector<std::pair<std::wstring, int32_t>> listed;
                    if (listed_class != s.pawn->GetClassPrivate())
                    {
                        listed_class = s.pawn->GetClassPrivate();
                        listed.clear();
                        for (UStruct* type = listed_class; type; type = type->GetSuperStruct())
                        {
                            if (std::wstring(type->GetName()) != L"Willie_BP_C") continue;
                            for (FProperty* p : type->ForEachProperty())
                            {
                                std::wstring name = p->GetName();
                                if (p->GetClass().GetName() != STR("DoubleProperty")) continue;
                                if (name.find(L"Health") == std::wstring::npos && name.find(L"Pain") == std::wstring::npos && name != L"Bleeding") continue;
                                listed.emplace_back(name, p->GetOffset_Internal());
                            }
                        }
                    }
                    std::map<std::wstring, double> values;
                    for (auto& entry : listed) values[entry.first] = *reinterpret_cast<double*>(reinterpret_cast<uint8_t*>(s.pawn) + entry.second);
                    if (struck)
                    {
                        std::wstring changed;
                        for (auto& entry : values)
                        {
                            auto was = before.find(entry.first);
                            if (was != before.end() && std::fabs(was->second - entry.second) > 0.5)
                            {
                                wchar_t text[96];
                                swprintf(text, 96, L"%ls %.0f -> %.0f; ", entry.first.c_str(), was->second, entry.second);
                                changed += text;
                            }
                        }
                        std::vector<UObject*> others;
                        UObjectGlobals::FindAllOf(STR("Willie_BP_C"), others);
                        double nearest = -1.0;
                        Vec3 me, at;
                        actor_location(s.pawn, me);
                        for (UObject* other : others)
                        {
                            if (!other || other == s.pawn || !actor_location(other, at)) continue;
                            double d = length(at - me);
                            if (nearest < 0 || d < nearest) nearest = d;
                        }
                        HSVR_LOG("Hurt: consciousness fell from %.0f to %.0f within a second | changed: %ls| other characters in the level %d, nearest %.0f cm | kicking: %s, crouched: %s",
                                 reference, *awake, changed.empty() ? L"(nothing in health or pain) " : changed.c_str(), (int)others.size() - 1, nearest,
                                 now_hurt < s.kick_until ? "yes" : "no", shared().own_crouch.load() ? "yes" : "no");
                    }
                    before.swap(values);
                    reference = *awake;
                    reference_time = now_hurt;
                }
                s.consciousness = *awake;
            }
            if (bool* fallen = s.pawn->GetValuePtrByPropertyNameInChain<bool>(STR("Fallen"))) s.fallen = *fallen;
            {
                double now = now_seconds();
                const char* why_not = nullptr;
                if (!cfg.kinematic_body) why_not = nullptr;
                else if (!s.head_pull_wanted || !s.have_nominal) why_not = "no first-person view of the character";
                else if (!s.have_rest) why_not = "the standing shape is not known yet";
                else if (s.consciousness >= 0 && s.consciousness < cfg.kinematic_min_consciousness) why_not = "knocked out";
                if (why_not && !s.have_rest && !s.kin_active) why_not = nullptr, s.kin_blocked_until = now; // not started yet: nothing to say
                if (why_not && cfg.kinematic_body) s.kin_blocked_until = now + 1.5;
                bool ready = s.head_pull_wanted && s.have_nominal && s.have_rest && now >= s.kin_blocked_until;
                bool knocked_out = s.consciousness >= 0 && s.consciousness < cfg.kinematic_min_consciousness;
                kinematic_update(cfg.kinematic_body && ready, why_not ? why_not : "waiting");
                s.hold_wanted = cfg.body_hold && !cfg.kinematic_body && s.head_pull_wanted && s.have_nominal && s.have_rest && !knocked_out;
            }
            for (Side& side : s.side) shield_to_hand(side, cfg);
            for (Side& side : s.side) grip_lock_update(side, cfg.rigid_grip && s.kin_active && !s.kin_hands && side.wrist_freed);
            if (s.kin_active)
            {
                // The placed body has no springs anywhere. The one driven thing is a physical hand
                // (as in Blade & Sorcery): its drive to the controller is the hand's entry here.
                s.head_pull_wanted = false;
                s.hold_wanted = false;
                s.pull_arms = arms && !s.kin_hands;
                pull_update(s.pull_arms);
            }
            else
            {
                s.pull_arms = arms;
                pull_update(arms);
            }
            s.placed = s.kin_active || s.hold_active;
            shared().body_placed.store(s.kin_active);
            if (s.kin_active && cfg.no_self_hits) no_self_hits_update();
            learn_head_axes();
            if (!stale) learn_rest();

            std::vector<Xf> pose(s.bone_count);
            for (int32_t i = 0; i < s.bone_count; ++i) pose[i] = from_raw(s.animated[i]);
            // First the upper body and head, then the arms on top of that.
            std::vector<char> moved(s.bone_count, 0);
            apply_head(pose, moved, cfg);
            std::vector<Xf> leaned(pose);
            std::vector<char> changed(s.bone_count, 0);
            if (arms)
            {
                for (Side& side : s.side) solve_side(side, s.animated.data(), pose, changed, cfg);
            }
            // Carry every bone below a moved one along with it (fingers, weapon bone).
            bool any = false;
            for (int32_t i = 0; i < s.bone_count; ++i)
            {
                if (changed[i] || moved[i]) any = true;
                if (changed[i]) continue;
                int32_t p = s.parents[i];
                if (p < 0 || !changed[p]) continue;
                // Fingers and the weapon bone keep the game's own animation (the grip closing).
                Xf local = s.below_hand[i] ? relative(from_raw(s.animated[i]), from_raw(s.animated[p])) : relative(leaned[i], leaned[p]);
                pose[i] = compose(local, pose[p]);
                changed[i] = 1;
            }
            if (!any) return;
            for (int32_t i = 0; i < s.bone_count; ++i)
            {
                if (changed[i] || moved[i]) to_raw(pose[i], buffer[i]);
            }
            s.written_marker = buffer[s.marker_bone];
            s.written_frame = s.frame;
            ++s.applied_count;
            record_frame(pose);
        }

        // Turns each physical hand towards its controller's rotation by giving the hand body a
        // turning speed: the remaining turn divided by hand_turn_seconds, less for a heavy weapon.
        // Runs right before the physics step. The engine's own turning drive stays on underneath
        // (it holds the hand between frames) but its torque limit no longer decides how fast a
        // wrist can turn a sword.
        // ---- carried along ----------------------------------------------------------------------
        // Walking moves the character, and the placed body with it, however fast the movement
        // component likes. The physical hands only follow through their pull to the controllers.
        // So each change of the character's own speed is given to the hands and to what they hold
        // as well, directly: seen from the body, walking then does nothing to them, and the pull
        // only ever has the player's own hand movements to deal with.
        void carry_hands(const Config& cfg)
        {
            State& s = g_state;
            static UObject* last_pawn = nullptr;
            static Vec3 last_velocity;
            if (!cfg.carry_hands || !s.kin_active || s.kin_hands || !s.body)
            {
                last_pawn = nullptr;
                return;
            }
            UObject* movement = object_property(s.pawn, STR("CharacterMovement"));
            ue::FVector* velocity = movement ? movement->GetValuePtrByPropertyNameInChain<ue::FVector>(STR("Velocity")) : nullptr;
            if (!velocity) return;
            Vec3 now_velocity{velocity->X, velocity->Y, velocity->Z};
            Vec3 change = now_velocity - last_velocity;
            bool usable = last_pawn == s.pawn;
            last_velocity = now_velocity;
            last_pawn = s.pawn;
            if (!usable || length(change) < 0.5 || length(change) > 1500.0) return;
            UFunction* set_fn = s.body->GetFunctionByNameInChain(STR("SetPhysicsLinearVelocity"));
            if (!set_fn || set_fn->GetParmsSize() > 64) return;
            int32_t o_vel = param_offset(set_fn, L"NewVel"), o_add = param_offset(set_fn, L"bAddToCurrent"), o_bone = param_offset(set_fn, L"BoneName");
            if (o_vel < 0 || o_add < 0 || o_bone < 0) return;
            ue::FVector add{change.x, change.y, change.z};
            UObject* carried[2] = {nullptr, nullptr};
            for (int k = 0; k < 2; ++k)
            {
                Side& side = s.side[k];
                if (!side.pull_active || !side.wrist_freed || side.rescue_until > 0.0) continue;
                alignas(16) uint8_t parms[64] = {};
                memcpy(parms + o_vel, &add, sizeof(add));
                parms[o_add] = 1;
                memcpy(parms + o_bone, &side.hand_name, sizeof(FName));
                s.body->ProcessEvent(set_fn, parms);
                const TCHAR* property = side.out_sign > 0 ? STR("Weapon R") : STR("Weapon L");
                UObject* weapon = s.pawn->GetPropertyByNameInChain(property) ? object_property(s.pawn, property) : nullptr;
                UObject* base = weapon && weapon->GetPropertyByNameInChain(STR("BaseMesh")) ? object_property(weapon, STR("BaseMesh")) : nullptr;
                if (!base || base == carried[0]) continue; // (one weapon in both hands is carried once)
                carried[k] = base;
                UFunction* weapon_fn = base->GetFunctionByNameInChain(STR("SetPhysicsLinearVelocity"));
                if (weapon_fn != set_fn) continue;
                alignas(16) uint8_t weapon_parms[64] = {};
                memcpy(weapon_parms + o_vel, &add, sizeof(add));
                weapon_parms[o_add] = 1;
                base->ProcessEvent(weapon_fn, weapon_parms);
            }
        }

        void turn_hands(const Config& cfg)
        {
            State& s = g_state;
            if (!cfg.hand_turn_servo || !s.kin_active || s.kin_hands || !s.body) return;
            UFunction* get_fn = s.body->GetFunctionByNameInChain(STR("GetPhysicsAngularVelocityInRadians"));
            UFunction* set_fn = s.body->GetFunctionByNameInChain(STR("SetPhysicsAngularVelocityInRadians"));
            if (!get_fn || !set_fn || get_fn->GetParmsSize() > 64 || set_fn->GetParmsSize() > 64) return;
            int32_t g_bone = param_offset(get_fn, L"BoneName"), g_ret = param_offset(get_fn, L"ReturnValue");
            int32_t s_vel = param_offset(set_fn, L"NewAngVel"), s_add = param_offset(set_fn, L"bAddToCurrent"), s_bone = param_offset(set_fn, L"BoneName");
            if (g_bone < 0 || g_ret < 0 || s_vel < 0 || s_add < 0 || s_bone < 0) return;
            static double last = 0.0;
            double now = now_seconds();
            double dt = last > 0 ? std::min(0.05, now - last) : 0.0;
            last = now;
            if (dt <= 0.0) return;
            for (Side& side : s.side)
            {
                if (!side.have_turn_target || !side.pull_active) continue;
                Xf hand;
                if (!socket_transform(s.body, side.hand_name, RTS_World, hand)) continue;
                Quat err = (side.turn_target * hand.q.inverse()).normalized();
                if (err.w < 0) err = {-err.x, -err.y, -err.z, -err.w};
                double angle = 2.0 * std::acos(std::min(1.0, err.w));
                side.turn_err_deg = angle * kRadToDeg;
                if (angle < 0.01) continue; // half a degree: leave it to the drive
                Vec3 axis = normalized({err.x, err.y, err.z});
                double seconds = std::max(dt, cfg.hand_turn_seconds);
                Vec3 want = axis * std::min(45.0, angle / seconds); // rad/s
                float* mass = s.pawn->GetValuePtrByPropertyNameInChain<float>(side.out_sign > 0 ? STR("R Weapon Mass") : STR("L Weapon Mass"));
                double held = mass ? std::max(0.0, (double)*mass) : 0.0;
                double share = 1.0 / (1.0 + held / std::max(0.2, cfg.hand_turn_mass_kg)); // of the way to that speed, this frame
                alignas(16) uint8_t get_parms[64] = {};
                memcpy(get_parms + g_bone, &side.hand_name, sizeof(FName));
                s.body->ProcessEvent(get_fn, get_parms);
                ue::FVector have;
                memcpy(&have, get_parms + g_ret, sizeof(have));
                Vec3 now_vel{have.X, have.Y, have.Z};
                Vec3 next = now_vel + (want - now_vel) * share;
                alignas(16) uint8_t set_parms[64] = {};
                ue::FVector out{next.x, next.y, next.z};
                memcpy(set_parms + s_vel, &out, sizeof(out));
                set_parms[s_add] = 0;
                memcpy(set_parms + s_bone, &side.hand_name, sizeof(FName));
                s.body->ProcessEvent(set_fn, set_parms);
                // What the hand holds is one piece with it (the grip is rigid) and turns with it:
                // it is given the same turning speed, and the speed its middle has on that turn
                // about the hand. Turning the hand alone left a long weapon to be dragged round by
                // the grip joint, and the wrist 40 degrees behind the player's. (The weight still
                // shows: `share` above is smaller the heavier the thing is.)
                const TCHAR* held_property = side.out_sign > 0 ? STR("Weapon R") : STR("Weapon L");
                UObject* weapon = s.pawn->GetPropertyByNameInChain(held_property) ? object_property(s.pawn, held_property) : nullptr;
                UObject* base = weapon && weapon->GetPropertyByNameInChain(STR("BaseMesh")) ? object_property(weapon, STR("BaseMesh")) : nullptr;
                if (!base || !side.grip_locked || base->GetFunctionByNameInChain(STR("SetPhysicsAngularVelocityInRadians")) != set_fn) continue;
                const Side& other = s.side[side.out_sign > 0 ? 0 : 1];
                const TCHAR* other_property = side.out_sign > 0 ? STR("Weapon L") : STR("Weapon R");
                if (side.out_sign < 0 && other.grip_locked && s.pawn->GetPropertyByNameInChain(other_property) && object_property(s.pawn, other_property) == weapon)
                    continue; // one thing in both hands: the right hand turns it
                alignas(16) uint8_t turn_parms[64] = {};
                memcpy(turn_parms + s_vel, &out, sizeof(out));
                base->ProcessEvent(set_fn, turn_parms);
                UFunction* lin_get = s.body->GetFunctionByNameInChain(STR("GetPhysicsLinearVelocity"));
                UFunction* lin_set = s.body->GetFunctionByNameInChain(STR("SetPhysicsLinearVelocity"));
                UFunction* com_fn = base->GetFunctionByNameInChain(STR("GetCenterOfMass"));
                if (!lin_get || !lin_set || !com_fn || lin_get->GetParmsSize() > 64 || lin_set->GetParmsSize() > 64 || com_fn->GetParmsSize() > 64) continue;
                int32_t lg_bone = param_offset(lin_get, L"BoneName"), lg_ret = param_offset(lin_get, L"ReturnValue");
                int32_t ls_vel = param_offset(lin_set, L"NewVel"), ls_add = param_offset(lin_set, L"bAddToCurrent"), ls_bone = param_offset(lin_set, L"BoneName");
                int32_t c_ret = param_offset(com_fn, L"ReturnValue");
                if (lg_bone < 0 || lg_ret < 0 || ls_vel < 0 || ls_add < 0 || ls_bone < 0 || c_ret < 0) continue;
                alignas(16) uint8_t a[64] = {}, b[64] = {}, c[64] = {};
                memcpy(a + lg_bone, &side.hand_name, sizeof(FName));
                s.body->ProcessEvent(lin_get, a);
                base->ProcessEvent(com_fn, b);
                ue::FVector hand_speed, middle;
                memcpy(&hand_speed, a + lg_ret, sizeof(hand_speed));
                memcpy(&middle, b + c_ret, sizeof(middle));
                Vec3 arm = Vec3{middle.X, middle.Y, middle.Z} - hand.t; // from the hand to the thing's middle, cm
                Vec3 round{next.y * arm.z - next.z * arm.y, next.z * arm.x - next.x * arm.z, next.x * arm.y - next.y * arm.x};
                if (length(arm) > 250.0) continue; // not in the hand after all
                ue::FVector speed{hand_speed.X + round.x, hand_speed.Y + round.y, hand_speed.Z + round.z};
                memcpy(c + ls_vel, &speed, sizeof(speed));
                base->ProcessEvent(lin_set, c);
            }
        }

        // Collisions the placed body should not have, switched off through the game's own function
        // library for it (CollisionDisablerFunctionLibrary; the game uses it for a weapon against
        // the hand and forearm that hold it):
        //   - the body against itself (a kick while crouched struck the player's own body and hurt);
        //   - what the player holds against the player's body;
        //   - loose weapons and traps, held by nobody, against the body other than the hands (a
        //     placed body cannot give way: the hanging kettle caught against it was thrown about).
        //     They collide again as soon as somebody holds them.
        struct NameArray
        {
            const FName* data;
            int32_t num;
            int32_t max;
        };

        bool collision_call(const wchar_t* name, std::initializer_list<std::pair<const void*, int32_t>> args)
        {
            static UObject* library = nullptr;
            if (!library) library = UObjectGlobals::FindObject(STR("CollisionDisablerFunctionLibrary"), STR("Default__CollisionDisablerFunctionLibrary"));
            UFunction* fn = library ? library->GetFunctionByNameInChain(name) : nullptr;
            if (!fn || fn->GetParmsSize() > 96) return false;
            alignas(16) uint8_t parms[96] = {};
            auto arg = args.begin();
            static std::map<UFunction*, bool> told;
            bool tell = !told[fn];
            told[fn] = true;
            for (FProperty* p : fn->ForEachProperty())
            {
                if (arg == args.end() || p->GetSize() != arg->second) return false;
                if (arg->second == 4)
                {
                    // The duration (-1 = for good). It is a number of seconds: a float. (Handed
                    // over as a whole number until the twelfth cut, the game read "not a number",
                    // and the switches it was meant to set for good did not hold.)
                    int32_t whole;
                    memcpy(&whole, arg->first, 4);
                    bool is_float = p->GetClass().GetName() == STR("FloatProperty");
                    float seconds = (float)whole;
                    memcpy(parms + p->GetOffset_Internal(), is_float ? (const void*)&seconds : (const void*)&whole, 4);
                    if (tell) HSVR_LOG("Hands: the game's collision switch %ls takes its duration as %ls", name, p->GetClass().GetName().c_str());
                }
                else
                {
                    memcpy(parms + p->GetOffset_Internal(), arg->first, arg->second);
                }
                ++arg;
            }
            if (arg != args.end()) return false;
            library->ProcessEvent(fn, parms);
            return true;
        }

        // The parts of a weapon (or trap) that collide: its base mesh and whatever else the game
        // lists in its "Collision Components Array" (the hanging kettle's pot is such a part).
        void weapon_parts(UObject* weapon, std::vector<UObject*>& parts)
        {
            parts.clear();
            if (!weapon) return;
            if (weapon->GetPropertyByNameInChain(STR("BaseMesh")))
                if (UObject* base = object_property(weapon, STR("BaseMesh"))) parts.push_back(base);
            if (FProperty* list = weapon->GetPropertyByNameInChain(STR("Collision Components Array")))
            {
                struct ObjectArray { UObject** data; int32_t num; int32_t max; };
                const ObjectArray* more = reinterpret_cast<const ObjectArray*>(reinterpret_cast<uint8_t*>(weapon) + list->GetOffset_Internal());
                if (more->data && more->num > 0 && more->num <= 32)
                    for (int32_t i = 0; i < more->num; ++i)
                        if (more->data[i] && std::find(parts.begin(), parts.end(), more->data[i]) == parts.end()) parts.push_back(more->data[i]);
            }
            // And every other part of it that can collide (a shield's board is none of the above).
            static UClass* primitive = nullptr;
            if (!primitive) primitive = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Engine.PrimitiveComponent"));
            UFunction* list_fn = weapon->GetFunctionByNameInChain(STR("K2_GetComponentsByClass"));
            if (primitive && list_fn && list_fn->GetParmsSize() == 24)
            {
                struct { UClass* type; UObject** data; int32_t num; int32_t max; } found{primitive, nullptr, 0, 0};
                weapon->ProcessEvent(list_fn, &found);
                // (The engine's array is not given back: a few dozen bytes each time something is picked up.)
                for (int32_t i = 0; found.data && i < found.num && i < 48; ++i)
                    if (found.data[i] && std::find(parts.begin(), parts.end(), found.data[i]) == parts.end()) parts.push_back(found.data[i]);
            }
        }

        void no_self_hits_update()
        {
            State& s = g_state;
            Config cfg = config();
            static UObject* done_pawn = nullptr;
            static bool self_done = false;
            static UObject* held_done[2] = {nullptr, nullptr};
            static std::map<UObject*, bool> loose; // a loose thing's body -> collision with our body is off
            static std::vector<FName> body_bones;  // every rigid bone but the hands
            static double next_scan = 0.0;
            if (done_pawn != s.pawn || g_self_hits_redo)
            {
                g_self_hits_redo = false;
                done_pawn = s.pawn;
                self_done = false;
                held_done[0] = held_done[1] = nullptr;
                loose.clear();
                body_bones.clear();
                next_scan = 0.0;
            }
            FProperty* bones_property = s.pawn->GetPropertyByNameInChain(STR("Rigid Bones"));
            if (!bones_property) return;
            const NameArray* all = reinterpret_cast<const NameArray*>(reinterpret_cast<uint8_t*>(s.pawn) + bones_property->GetOffset_Internal());
            if (!all->data || all->num <= 0 || all->num > 200) return;
            const int32_t forever = -1;
            if (!self_done)
            {
                self_done = true;
                if (cfg.body_passes_itself)
                {
                    bool ok = collision_call(L"DisableCollision_SkeletalVsSkeletal", {{&s.body, 8}, {all, 16}, {&s.body, 8}, {all, 16}, {&forever, 4}});
                    HSVR_LOG(ok ? "Hands: the player's body no longer collides with itself" : "Hands: the game's collision switch (body against itself) was not found or takes other parameters");
                }
                for (int32_t i = 0; i < all->num; ++i)
                {
                    if (memcmp(&all->data[i], &s.side[0].hand_name, sizeof(FName)) != 0 && memcmp(&all->data[i], &s.side[1].hand_name, sizeof(FName)) != 0)
                        body_bones.push_back(all->data[i]);
                }
            }
            UObject* held[2] = {nullptr, nullptr};
            for (int k = 0; k < 2; ++k)
            {
                const TCHAR* property = k ? STR("Weapon R") : STR("Weapon L");
                UObject* weapon = s.pawn->GetPropertyByNameInChain(property) ? object_property(s.pawn, property) : nullptr;
                held[k] = weapon;
                g_held_item[k].store(weapon);
                if (!weapon || weapon == held_done[k]) continue;
                held_done[k] = weapon;
                std::vector<UObject*> parts;
                weapon_parts(weapon, parts);
                bool ok = !parts.empty();
                for (UObject* part : parts)
                    ok = collision_call(L"DisableCollision_SkeletalVsSingleBody", {{&s.body, 8}, {all, 16}, {&part, 8}, {&forever, 4}}) && ok;
                HSVR_LOG(ok ? "Hands: what the %s hand holds (%ls, %d colliding parts) no longer collides with the player's own body"
                            : "Hands: the game's collision switch was not found; what the %s hand holds (%ls, %d parts) can still hit the player's own body",
                         k ? "right" : "left", weapon->GetClassPrivate()->GetName().c_str(), (int)parts.size());
            }
            double now = now_seconds();
            if (!cfg.loose_things_pass || now < next_scan || body_bones.empty() || !s.have_actor_world) return;
            next_scan = now + 2.0;
            NameArray body{body_bones.data(), (int32_t)body_bones.size(), (int32_t)body_bones.size()};
            std::vector<UObject*> items;
            UObjectGlobals::FindAllOf(STR("ModularWeaponBP_C"), items);
            std::map<UObject*, bool> seen;
            int switched_off = 0, switched_on = 0;
            for (UObject* item : items)
            {
                if (!item || item == held[0] || item == held[1] || !item->GetPropertyByNameInChain(STR("Is Held"))) continue;
                bool* is_held = item->GetValuePtrByPropertyNameInChain<bool>(STR("Is Held"));
                Vec3 at;
                if (!is_held || !actor_location(item, at) || length(at - s.actor_world) > 600.0) continue;
                std::vector<UObject*> parts;
                weapon_parts(item, parts);
                bool want_off = !*is_held;
                bool counted = false;
                for (UObject* part : parts)
                {
                    auto before = loose.find(part);
                    bool off = before != loose.end() && before->second;
                    if (want_off && !off)
                    {
                        if (collision_call(L"DisableCollision_SkeletalVsSingleBody", {{&s.body, 8}, {&body, 16}, {&part, 8}, {&forever, 4}}) && !counted) { ++switched_off; counted = true; }
                    }
                    else if (!want_off && off)
                    {
                        if (collision_call(L"EnableCollision_SkeletalVsSingleBody", {{&s.body, 8}, {&body, 16}, {&part, 8}}) && !counted) { ++switched_on; counted = true; }
                    }
                    seen[part] = want_off;
                }
            }
            loose.swap(seen);
            if (switched_off || switched_on)
                HSVR_LOG("Hands: %d loose weapon(s) or trap(s) nearby no longer collide with the player's body; %d picked up by somebody collide again", switched_off, switched_on);
        }

        // ---- bumps ------------------------------------------------------------------------------
        // The game's script hears of everything the body mesh touches (its hit event) and turns the
        // force of the contact into damage. A placed limb is moved, not pushed: it cannot give way,
        // so merely standing against a loose prop is contact after contact at full force. While the
        // body is placed, the force of a contact with anything that is neither a fighter nor a
        // weapon is handed to the script as zero; the script then does nothing with it. Blows by
        // fighters and weapons arrive untouched.
        std::atomic<UObject*> g_hit_pawn{nullptr};
        std::atomic<UFunction*> g_hit_event{nullptr};
        int32_t g_hit_other = -1, g_hit_impulse = -1, g_hit_part = -1, g_hit_result = -1, g_hit_size = 0;
        bool g_hit_registered = false;
        struct ItemHitEvent
        {
            bool is_hit = false;
            int32_t other = -1, part = -1, impulse = -1, result = -1, size = 0;
        };
        std::map<UFunction*, ItemHitEvent> g_item_hit_event; // a function -> whether it is an actor's "I was hit" event, and where its parameters are
        int g_own_item_hits = 0;                                    // (for the log)
        std::map<UClass*, bool> g_hit_harmful; // class of the other thing -> a fighter or a weapon
        UClass* g_bump_class = nullptr;         // what the body last bumped into (for the log)

        void on_any_event(UObject* context, UFunction* function, void* parms)
        {
            if (!parms) return;
            UObject* held_r = g_held_item[1].load(std::memory_order_relaxed);
            UObject* held_l = g_held_item[0].load(std::memory_order_relaxed);
            if (context && (context == held_r || context == held_l))
            {
                // Something the player holds reports that one of its parts was hit. If what it hit
                // is the player's own body, the event reaches the game's script as a hit on nothing:
                // what one holds does not wound oneself. (A big shield in the left hand cut the
                // player's own legs to pieces while the right arm swung.)
                State& held_state = g_state;
                if (GetCurrentThreadId() != held_state.game_thread || !held_state.kin_active) return;
                auto known = g_item_hit_event.find(function);
                if (known == g_item_hit_event.end())
                {
                    // A part's own hit event (bound in the blueprint) or the actor's (ReceiveHit).
                    ItemHitEvent event;
                    std::wstring name = function->GetName();
                    if (name.find(STR("Hit")) != std::wstring::npos)
                    {
                        event.other = param_offset(function, L"OtherActor");
                        if (event.other < 0) event.other = param_offset(function, L"Other");
                        event.part = param_offset(function, L"OtherComp");
                        event.impulse = param_offset(function, L"NormalImpulse");
                        event.result = param_offset(function, L"Hit");
                        event.size = function->GetParmsSize();
                        event.is_hit = event.other >= 0 && event.part >= 0 && event.impulse >= 0 && event.result > event.impulse && event.size > event.result;
                        HSVR_LOG("Hands: a held thing's event %ls (%d bytes of parameters) is %s", name.c_str(), event.size,
                                 event.is_hit ? "a hit event: hits on the player's own body are kept from it" : "not a hit event of a known shape");
                    }
                    known = g_item_hit_event.emplace(function, event).first;
                }
                const ItemHitEvent& event = known->second;
                if (!event.is_hit) return;
                uint8_t* raw_parms = reinterpret_cast<uint8_t*>(parms);
                if (*reinterpret_cast<UObject**>(raw_parms + event.other) != held_state.pawn) return;
                memset(raw_parms + event.other, 0, sizeof(UObject*));
                memset(raw_parms + event.part, 0, sizeof(UObject*));
                memset(raw_parms + event.impulse, 0, 3 * sizeof(double));
                memset(raw_parms + event.result, 0, event.size - event.result);
                ++g_own_item_hits;
                return;
            }
            if (function != g_hit_event.load(std::memory_order_relaxed) || context != g_hit_pawn.load(std::memory_order_relaxed)) return;
            State& s = g_state;
            if (GetCurrentThreadId() != s.game_thread || s.pawn != context || !s.kin_active || !config().bumps_harmless) return;
            UObject* other = *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(parms) + g_hit_other);
            if (other && (other == held_r || other == held_l))
            {
                // The player's body touched by what the player holds: nothing to the script either.
                double* own = reinterpret_cast<double*>(reinterpret_cast<uint8_t*>(parms) + g_hit_impulse);
                own[0] = own[1] = own[2] = 0.0;
                ++g_own_item_hits;
                return;
            }
            if (other)
            {
                UClass* type = other->GetClassPrivate();
                auto known = g_hit_harmful.find(type);
                if (known == g_hit_harmful.end())
                {
                    bool harmful = false;
                    for (UStruct* t = type; t && !harmful; t = t->GetSuperStruct())
                    {
                        std::wstring name = t->GetName();
                        harmful = name == L"Willie_BP_C" || name == L"ModularWeaponBP_C";
                    }
                    known = g_hit_harmful.emplace(type, harmful).first;
                }
                if (known->second) return;
            }
            double* impulse = reinterpret_cast<double*>(reinterpret_cast<uint8_t*>(parms) + g_hit_impulse);
            impulse[0] = impulse[1] = impulse[2] = 0.0;
            ++s.bumps_muted;
            g_bump_class = other ? other->GetClassPrivate() : nullptr;
        }

        void hit_hook_setup(UObject* pawn)
        {
            UFunction* fn = pawn->GetFunctionByNameInChain(STR("BndEvt__BP_ThirdPersonCharacter_Mesh_K2Node_ComponentBoundEvent_0_ComponentHitSignature__DelegateSignature"));
            int32_t other = fn ? param_offset(fn, L"OtherActor") : -1, impulse = fn ? param_offset(fn, L"NormalImpulse") : -1;
            int32_t part = fn ? param_offset(fn, L"OtherComp") : -1, result = fn ? param_offset(fn, L"Hit") : -1;
            if (fn && part >= 0 && result > impulse && fn->GetParmsSize() > result)
            {
                g_hit_part = part;
                g_hit_result = result;
                g_hit_size = fn->GetParmsSize();
            }
            if (!fn || other < 0 || impulse < 0)
            {
                g_hit_event = nullptr;
                HSVR_LOG("Body: the game's body-hit event was not found; bumping into props can still hurt the placed body");
                return;
            }
            g_hit_other = other;
            g_hit_impulse = impulse;
            g_hit_pawn = pawn;
            g_hit_event = fn;
            if (!g_hit_registered)
            {
                g_hit_registered = true;
#pragma warning(suppress : 4996)
                Hook::RegisterProcessEventPreCallback(&on_any_event);
            }
            HSVR_LOG("Body: listening to the game's body-hit event (bumps into props and the level do not hurt the placed body)");
        }

        // What the placed body tells the game's script about itself, again and again (the script
        // works these out anew as it runs).
        void placed_body_facts(bool count)
        {
            State& s = g_state;
            // It stands where the player stands: it is not stumbling after a kick ...
            if (double* stumble = s.pawn->GetValuePtrByPropertyNameInChain<double>(STR("Stumble Direction")))
                stumble[0] = stumble[1] = stumble[2] = 0.0;
            // ... and it is not falling. The game takes the character for falling while its feet's
            // ground checks fail, and then counts whatever a leg touches as the landing. (Falling
            // off a ledge is the movement component's business and still happens; only the landing
            // damage is lost.)
            if (double* falling = s.pawn->GetValuePtrByPropertyNameInChain<double>(STR("Free Fall Rate")))
            {
                if (count && *falling > 0.001) ++s.fall_frames;
                *falling = 0.0;
            }
            // Its joints cannot be pulled apart. The game judges a joint by how far the bone is
            // bent against its parent and wears the limb down frame by frame until it is
            // "dislocated"; the deep knee bend of the mod's own crouch read as a knee coming apart
            // (right leg from 94 to 0 in six frames, nobody near). The game has a switch for
            // exactly this, which it uses itself for a moment at a time.
            if (bool* blocked = s.pawn->GetValuePtrByPropertyNameInChain<bool>(STR("Dislocation Blocked"))) *blocked = true;
            // What the hands hold stays in them until the player lets go. The game measures the force
            // in the joint that holds a weapon and opens the hand when it is too much for the arm (a
            // disarm); with the weapon held rigidly that force is always large. The game has its own
            // switch against dropping, which it sets for a moment after each grab.
            if (config().hold_tight)
            {
                if (bool* keep = s.pawn->GetValuePtrByPropertyNameInChain<bool>(STR("R Hand Block Drop"))) *keep = true;
                if (bool* keep = s.pawn->GetValuePtrByPropertyNameInChain<bool>(STR("L Hand Block Drop"))) *keep = true;
            }
        }

        bool before_actor_tick(Vec3& at)
        {
            State& s = g_state;
            if (!s.ready || s.failed || !s.pawn || s.pawn != g_actor_tick_pawn || !s.kin_active) return false;
            placed_body_facts(true);
            if (config().dev_character_scale > 0.3 && config().dev_character_scale < 3.0)
            {
                // Self-test only: the game sets the character's height again a moment after it
                // appears; keep the one the test asked for.
                UFunction* get_fn = s.pawn->GetFunctionByNameInChain(STR("GetActorScale3D"));
                UFunction* set_fn = s.pawn->GetFunctionByNameInChain(STR("SetActorScale3D"));
                if (get_fn && set_fn && get_fn->GetParmsSize() == (int32_t)sizeof(ue::FVector) && set_fn->GetParmsSize() == (int32_t)sizeof(ue::FVector))
                {
                    double v = config().dev_character_scale;
                    ue::FVector have{1, 1, 1};
                    s.pawn->ProcessEvent(get_fn, &have);
                    if (std::fabs(have.Z - v) > 0.005)
                    {
                        ue::FVector size{v, v, v};
                        s.pawn->ProcessEvent(set_fn, &size);
                    }
                }
            }
            return config().hold_ground && actor_location(s.pawn, at);
        }

        void after_actor_tick(const Vec3& at)
        {
            State& s = g_state;
            Vec3 now;
            if (!s.pawn || s.pawn != g_actor_tick_pawn || !actor_location(s.pawn, now)) return;
            Vec3 gone = now - at;
            gone.z = 0; // height (the floor, a jump) is the game's
            double d = length(gone);
            if (d < 0.02) return;
            UFunction* set_fn = s.pawn->GetFunctionByNameInChain(STR("K2_SetActorLocation"));
            int32_t o_loc = set_fn ? param_offset(set_fn, L"NewLocation") : -1, o_sweep = set_fn ? param_offset(set_fn, L"bSweep") : -1,
                    o_tp = set_fn ? param_offset(set_fn, L"bTeleport") : -1;
            if (!set_fn || o_loc < 0 || o_sweep < 0 || o_tp < 0 || set_fn->GetParmsSize() > 1024) return;
            alignas(16) uint8_t parms[1024] = {};
            ue::FVector to{at.x, at.y, now.z};
            memcpy(parms + o_loc, &to, sizeof(to));
            parms[o_sweep] = 0; // straight back to where it was a moment ago
            parms[o_tp] = 0;    // (not a teleport: that would carry the physical hands along)
            s.pawn->ProcessEvent(set_fn, parms);
            ++s.drag_undone;
            s.drag_undone_cm += d;
        }

        void before_muscles_update()
        {
            State& s = g_state;
            if (!s.ready || s.failed || !s.pawn || g_pull.pawn != s.pawn || !g_pull.ok) return;
            if (!s.enabled_by_key || s.menu_up)
            {
                pull_update(false);
                return;
            }
            pull_update(s.pull_arms);
            turn_hands(config());
            carry_hands(config());
            {
                // The game's script sets the limits of the joints that hold the weapons again and
                // again as it runs (grip types, thrusts, sliding). It has run by now and the
                // physics has not: locked here, the joint is locked when it counts.
                Config cfg = config();
                for (Side& side : s.side) shield_to_hand(side, cfg);
                for (Side& side : s.side) grip_lock_update(side, cfg.rigid_grip && s.kin_active && !s.kin_hands && side.wrist_freed);
            }
            // The capsule leash's reach again, here as well as in the body's update: the game works
            // out its own value at the end of its script each frame, and that script may run before
            // or after the body's update. Whichever order, ours is then the one its next frame reads.
            if (s.kin_active)
            {
                placed_body_facts(false);
                if (double* limit = s.pawn->GetValuePtrByPropertyNameInChain<double>(STR("Capsule Max Dislocation")))
                {
                    if (*limit < 100000.0)
                    {
                        if (!s.have_max_dislocation)
                        {
                            s.game_max_dislocation = *limit;
                            s.have_max_dislocation = true;
                        }
                        *limit = 1000000.0;
                    }
                }
            }
        }

        void log_status(const Config& cfg)
        {
            State& s = g_state;
            if (!cfg.log_arms) return;
            static double last = 0.0;
            double now = now_seconds();
            if (now - last < 1.0) return;
            last = now;
            pull_check();
            if (cfg.body_follow || cfg.hand_pull)
            {
                HeadWorld view;
                bool have_view = stereo_head_world(view);
                s.head_err_cm = -1;
                Xf head_now;
                if (s.head_applied && s.have_head_target && s.body && socket_transform(s.body, make_name(L"head"), RTS_World, head_now))
                    s.head_err_cm = length(head_now.t - s.head_target_world);
                Vec3 up = s.have_body_world ? s.body_world.q.rotate({0, 0, 1}) : Vec3{0, 0, 1};
                s.forearm_off_cm = -1;
                {
                    // The right forearm's physics body (its centre of mass) against where the forearm
                    // is drawn: about half a forearm apart when the body is where the picture is.
                    UFunction* com_fn = s.body ? s.body->GetFunctionByNameInChain(STR("GetCenterOfMass")) : nullptr;
                    struct
                    {
                        FName BoneName;
                        ue::FVector ReturnValue;
                    } com{};
                    Xf drawn;
                    if (com_fn && com_fn->GetParmsSize() == (int32_t)sizeof(com) && socket_transform(s.body, s.side[1].lower_name, RTS_World, drawn))
                    {
                        com.BoneName = s.side[1].lower_name;
                        s.body->ProcessEvent(com_fn, &com);
                        s.forearm_off_cm = length(Vec3{com.ReturnValue.X, com.ReturnValue.Y, com.ReturnValue.Z} - drawn.t);
                    }
                }
                HSVR_LOG("Body: %s | right forearm's physics body %.0f cm from where it is drawn (about 13 is normal)", s.hold_active ? "held at its place" : "not held",
                         s.forearm_off_cm);
                HSVR_LOG("Room: moves by something other than walking taken back this second: %d (%.0f cm) | the game's script dragged the character %d times (%.0f cm), each undone on the spot | the game took the character for falling in %d frames | %d bumps into props or the level kept harmless (the last into a %ls) | %d touches between the body and what its own hands hold kept harmless",
                         s.ground_undone, s.ground_undone_cm, s.drag_undone, s.drag_undone_cm, s.fall_frames, s.bumps_muted,
                         s.bumps_muted && g_bump_class ? g_bump_class->GetName().c_str() : L"-", g_own_item_hits);
                g_own_item_hits = 0;
                s.bumps_muted = 0;
                // Whatever moves the character is named here: the game's movement input (the left
                // stick or keys; what it asked for at its strongest, and how much of that forward,
                // negative = backwards), or the player's own steps in the room.
                HSVR_LOG("Room: this second the character walked %.0f cm on movement input (strongest %.2f, %.2f of it forward) and %.0f cm with the player's own steps | heights: character size %.2f, world scaled %.2f for a player with eyes at %.0f cm",
                         s.walked_cm, s.input_most, s.input_forward, s.stepped_cm, (double)shared().body_scale.load(), (double)shared().height_scale.load(),
                         (double)shared().player_eye_cm.load());
                s.walked_cm = s.stepped_cm = s.input_most = s.input_forward = 0;
                s.ground_undone = 0;
                s.ground_undone_cm = 0;
                s.drag_undone = 0;
                s.drag_undone_cm = 0;
                s.fall_frames = 0;
                HSVR_LOG("Pull: hand and body settings written %d times this second (each time means the game had put its own back)", s.pull_applies_second);
                s.pull_applies_second = 0;
                HSVR_LOG("Legs: %d steps this second | feet %s | hands %s (wrists free: right %s, left %s)", s.steps_taken,
                         cfg.own_legs && s.side[0].have_foot_home ? "the mod's own" : "the game's animation", s.kin_active ? (s.kin_hands ? "placed rigidly" : "physical, driven to the controllers") : "the game's",
                         s.side[1].wrist_freed ? "yes" : "no", s.side[0].wrist_freed ? "yes" : "no");
                s.steps_taken = 0;
                HSVR_LOG("Body: mesh %s the character (put back %d times)", s.body && s.pawn && body_attached_to_capsule() ? "rides on" : "is NOT attached to", s.kin_reattached);
                HSVR_LOG("Body: %s (the game switched simulation back on %d times) | hips moved %.0f cm for the head's height | body mesh %.0f cm from its place over the feet | "
                         "consciousness %.0f, the game says fallen: %s",
                         s.kin_active ? "placed, not simulated" : "simulated by the game", s.kin_retaken, s.pelvis_drop, s.mesh_off_cm, s.consciousness, s.fallen ? "yes" : "no");
                HSVR_LOG("Body: follow %s (%s) | headset %.0f cm from its recentered place | real head %.1f cm from the headset's place (held: %s) | view pushed %.0f cm by the "
                         "leash | chest turned %.0f deg from the legs | body mesh tilted %.0f deg | hand pull right %s left %s (spring anchored %.1f / %.1f cm from the controller)",
                         !cfg.body_follow ? "off in settings" : (s.head_applied ? "on" : (have_view && view.head_valid ? "waiting" : "no first-person view")),
                         cfg.own_upper_body && s.have_rest ? "own upper body" : "game's upper body", s.lean_cm, s.head_err_cm, s.head_pull_active ? "yes" : "no",
                         have_view ? view.body_gap_cm : -1.0, s.have_chest_yaw ? normalize_axis(s.chest_yaw - s.actor_yaw) : 0.0,
                         std::acos(std::max(-1.0, std::min(1.0, up.z))) * kRadToDeg, s.side[1].pull_active ? "on" : "off", s.side[0].pull_active ? "on" : "off",
                         s.side[1].pull_check_cm, s.side[0].pull_check_cm);
            }
            const XrHandRaw& l = s.hands[0];
            const XrHandRaw& r = s.hands[1];
            HSVR_LOG("Arms: %s | controllers L %s R %s (trigger %.2f/%.2f grip %.2f/%.2f) | right hand %s err %.1f cm %.0f deg | left hand %s err %.1f cm %.0f deg | "
                     "driver frames fresh %llu late %llu applied %llu",
                     !cfg.motion_arms ? "off in settings" : (!s.enabled_by_key ? "off by key" : (s.menu_up ? "menu" : (s.ready ? "on" : "waiting"))),
                     l.valid ? "tracked" : "lost", r.valid ? "tracked" : "lost", l.trigger, r.trigger, l.squeeze, r.squeeze,
                     s.side[1].target_valid ? "sent" : "-", s.side[1].last_error_cm, s.side[1].last_error_deg, s.side[0].target_valid ? "sent" : "-",
                     s.side[0].last_error_cm, s.side[0].last_error_deg, (unsigned long long)s.fresh_count, (unsigned long long)s.stale_count,
                     (unsigned long long)s.applied_count);
        }
    } // namespace

    void arm_toggle()
    {
        g_state.enabled_by_key = !g_state.enabled_by_key;
        shared().vr_controls_off.store(!g_state.enabled_by_key);
        HSVR_LOG(g_state.enabled_by_key ? "Key: VR controls on (motion arms, body, roomscale, controller buttons and gestures)"
                                        : "Key: VR controls off: the game plays with mouse and keyboard as it does flat, seen through the headset");
    }

    void arm_game_tick(UObject* pawn, bool menu_up)
    {
        State& s = g_state;
        s.game_thread = GetCurrentThreadId();
        ++s.frame;
        s.menu_up = menu_up;
        Config cfg = config();
        // A new character first: nothing below may touch the old one again.
        if (pawn != s.pawn)
        {
            s.ready = false;
            s.pawn = pawn;
            s.body = s.driver = nullptr;
            for (Side& side : s.side) side.target_valid = side.pull_active = false;
            if (pawn) HSVR_LOG("Arms: new character %ls", pawn->GetFullName().c_str());
        }
        hide_head_tick(cfg);
        if (s.failed || !cfg.motion_arms)
        {
            s.head_pull_wanted = false;
            s.hold_wanted = false;
            kinematic_update(false, "motion controls off");
            pull_update(false);
            log_status(cfg);
            return;
        }
        if (!pawn) return;
        if (!s.ready)
        {
            // Try now and then (a character that is still being set up, or the tavern's pawn).
            static double next_try = 0.0;
            double now = now_seconds();
            if (now < next_try) return;
            next_try = now + 2.0;
            if (!setup_pawn(pawn)) return;
        }
        s.hands_valid = vr_locate_hands(s.hands);
        actor_yaw(pawn, s.actor_yaw);
        // The grip is measured a few times a second (it changes when a weapon is picked up).
        static double next_grip = 0.0;
        double now = now_seconds();
        if (now >= next_grip)
        {
            next_grip = now + 0.25;
            for (Side& side : s.side) learn_grip(side);
        }
        {
            // Every grip press and release, with the state of the hands at that moment.
            static bool down[2] = {false, false};
            for (int k = 0; k < 2; ++k)
            {
                bool now_down = s.hands_valid && s.hands[k].valid && (down[k] ? s.hands[k].squeeze > 0.4f : s.hands[k].squeeze > 0.6f);
                if (now_down == down[k]) continue;
                down[k] = now_down;
                HSVR_LOG("Grip: %s grip %s | hands off their targets right %.1f cm %.0f deg, left %.1f cm %.0f deg | body %s | hand pull right %s left %s", k ? "right" : "left",
                         now_down ? "pressed" : "released", s.side[1].last_error_cm, s.side[1].last_error_deg, s.side[0].last_error_cm, s.side[0].last_error_deg,
                         s.hold_active ? "held" : (s.kin_active ? "kinematic" : "the game's"), s.side[1].pull_active ? "on" : "off", s.side[0].pull_active ? "on" : "off");
            }
        }
        calibrate_tick();
        selftest_tick(cfg);
        roomscale_tick(cfg);
        compute_targets();
        recorder_tick();
        if (s.enabled_by_key && !menu_up && s.hands_valid) sync_aim();
        // The hook that pulls the hands only runs while the arms are driven; otherwise the
        // game's own muscle settings are put back here.
        if (!s.enabled_by_key || menu_up)
        {
            s.head_pull_wanted = false;
            s.head_applied = false;
            if (!s.enabled_by_key) s.hold_wanted = false; // a menu keeps the hold (the game is paused)
            pull_update(false);
            s.placed = s.kin_active || s.hold_active;
        }
        if (!s.enabled_by_key || !cfg.kinematic_body || !cfg.body_follow) kinematic_update(false, !s.enabled_by_key ? "VR controls off" : "switched off in the settings");
        if (!s.kin_active || !s.ready) shared().body_placed.store(false);
        log_status(cfg);
    }
} // namespace hsvr
