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
#include <cstdint>
#include <cstring>
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

        // A bone or component transform, scale left out (the character's bones are not scaled).
        struct Xf
        {
            Quat q;
            Vec3 t;
        };
        // `local` placed in `parent`'s frame (Unreal's local * parent).
        Xf compose(const Xf& local, const Xf& parent) { return {(parent.q * local.q).normalized(), parent.q.rotate(local.t) + parent.t}; }
        // `child` expressed in `parent`'s frame.
        Xf relative(const Xf& child, const Xf& parent)
        {
            Quat inv = parent.q.inverse();
            return {(inv * child.q).normalized(), inv.rotate(child.t - parent.t)};
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
            FName parent_name, hand_name, middle_name, thumb_name, lower_name, upper_name;
            // Hand pull (see "pulling the hands" below).
            bool pull_active = false;
            bool have_last_world = false;
            Vec3 last_world_target;     // where the hand bone was last sent, world
            double pull_check_cm = -1;  // the engine's own target for the hand versus ours
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

        void hooked_execute_tick(void* self, float delta, int tick_type, int current_thread, const void* completion)
        {
            if (GetCurrentThreadId() == g_state.game_thread) before_body_update();
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
            if (side.out_sign > 0.0)
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
                // With hand pull the hand goes where the controller really is; the older reach
                // scaling only applies without it.
                if (!cfg.hand_pull) scale_reach(side, raw, grip_pos, cfg);
                // The controller's handle (+X) and the direction from the wrist into the fist (-Z)
                // match the hand's handle and grip-centre directions.
                Quat hand_frame = quat_from_frame(side.grip_forward, side.grip_center);
                Quat controller_frame = quat_from_frame({1, 0, 0}, {0, 0, -1});
                Quat hand_rot = (grip_rot * controller_frame * hand_frame.inverse()).normalized();
                side.target_world.q = hand_rot;
                side.target_world.t = grip_pos - hand_rot.rotate(side.grip_center);
                side.blade_world = grip_rot.rotate({1, 0, 0});
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
            bool pull = side.pull_active && s.have_body_world;
            Xf shoulder_world;
            if (pull && !socket_transform(s.body, side.upper_name, RTS_World, shoulder_world)) pull = false;
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
            Vec3 to_target = target.t - S;
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
            pose[side.hand] = {hand_q, pull ? target.t : T};
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
            head
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
            L.next_try = now_seconds() + 3.0;
            g_pull_bodies.clear();
            bool last = tries >= 5;
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
            for (int k = 0; k < 2; ++k)
            {
                Side& side = s.side[k];
                g_pull_bodies.push_back({side.hand_name, PullKind::hand, k});
                g_pull_bodies.push_back({side.lower_name, PullKind::arm, k});
                g_pull_bodies.push_back({side.upper_name, PullKind::arm, k});
                g_pull_bodies.push_back({side_name(L"clavicle", side), PullKind::arm, k});
            }
            g_pull_bodies.push_back({make_name(L"head"), PullKind::head, -1});
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
            if (L.pawn != s.pawn || (!L.ok && L.tries < 5 && now_seconds() >= L.next_try))
            {
                if (!cfg.hand_pull && !cfg.body_follow && L.pawn != s.pawn) return; // never looked, nothing to give back
                pull_setup();
            }
            for (Side& side : s.side) side.pull_active = false;
            s.head_pull_active = false;
            if (!L.ok) return;
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
                if (body.kind == PullKind::hand)
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
                    want = hand_wanted(body.side) && cfg.arm_soft_scale != 1.0;
                    if (want) scale_turn(std::max(0.02, std::min(5.0, cfg.arm_soft_scale)));
                }
                else
                {
                    want = s.head_pull_wanted && cfg.head_pull_strength > 0.0;
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
                }
                body.written = desired;
                body.have_written = true;
                body.active = want;
                if (body.kind == PullKind::hand) s.side[body.side].pull_active = want;
                if (body.kind == PullKind::head) s.head_pull_active = want;
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
            Xf frame{quat_from_yaw_deg(s.actor_yaw), base_world.t};
            s.rest.assign(s.bone_count, Xf{});
            for (int32_t i = 0; i < s.bone_count; ++i)
            {
                if (s.below_spine[i]) s.rest[i] = relative(compose(from_raw(s.animated[i]), s.body_world), frame);
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
                Xf frame{quat_from_yaw_deg(s.chest_yaw), compose(pose[s.spine_base], s.body_world).t};
                for (int32_t i = 0; i < s.bone_count; ++i)
                {
                    if (!s.below_spine[i]) continue;
                    pose[i] = relative(compose(s.rest[i], frame), s.body_world);
                    moved[i] = 1;
                }
            }
            // Lean about the lowest spine bone so that the head is towards the headset's place.
            Vec3 pivot = pose[s.spine_base].t;
            Vec3 head_target = to_pose.rotate(head_target_world - s.body_world.t);
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
            if (s.head_pull_active)
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
        struct RawObjectArray
        {
            UObject** data;
            int32_t num;
            int32_t max;
        };
        struct HideBoneParams
        {
            FName BoneName;
            uint8_t PhysBodyOption; // 0 = leave the physics alone
            uint8_t pad[7];
        };
        struct HiddenInGameParams
        {
            bool NewHidden;
            bool PropagateToChildren;
        };

        void hide_head_tick(const Config& cfg)
        {
            State& s = g_state;
            static UObject* hidden_pawn = nullptr;
            static double next = 0.0;
            UObject* body = object_property(s.pawn, STR("Mesh"));
            UObject* driver = object_property(s.pawn, STR("DriverSkeleton"));
            bool want = cfg.hide_head && s.pawn && body && driver && cfg.anchor == "head" && !shared().third_person.load() && shared().xr_running.load();
            double now = now_seconds();
            bool was = hidden_pawn != nullptr && hidden_pawn == s.pawn;
            if (hidden_pawn && hidden_pawn != s.pawn) hidden_pawn = nullptr; // the old character is gone or no longer ours
            if (!want && !was) return;
            if (want && was && now < next) return; // again now and then: armour put on later
            next = now + 2.0;
            if (!s.pawn || !body) return;
            std::wstring bone_text(cfg.hide_head_bone.begin(), cfg.hide_head_bone.end());
            FName bone = make_name(bone_text.c_str());
            std::vector<UObject*> components{body};
            for (const TCHAR* list : {STR("BlueprintCreatedComponents"), STR("InstanceComponents")})
            {
                RawObjectArray* arr = s.pawn->GetValuePtrByPropertyNameInChain<RawObjectArray>(list);
                if (!arr || !arr->data || arr->num <= 0 || arr->num > 2000) continue;
                for (int32_t i = 0; i < arr->num; ++i)
                {
                    if (arr->data[i] && arr->data[i] != driver && arr->data[i] != body) components.push_back(arr->data[i]);
                }
            }
            int count = 0;
            for (UObject* component : components)
            {
                UFunction* fn = component->GetFunctionByNameInChain(want ? STR("HideBoneByName") : STR("UnHideBoneByName"));
                if (!fn || fn->GetParmsSize() > (int32_t)sizeof(HideBoneParams)) continue;
                HideBoneParams p{};
                p.BoneName = bone;
                component->ProcessEvent(fn, &p);
                ++count;
            }
            if (UObject* hair = object_property(s.pawn, STR("Hair")))
            {
                UFunction* fn = hair->GetFunctionByNameInChain(STR("SetHiddenInGame"));
                if (fn && fn->GetParmsSize() == (int32_t)sizeof(HiddenInGameParams))
                {
                    HiddenInGameParams p{want, false};
                    hair->ProcessEvent(fn, &p);
                }
            }
            if (want != was) HSVR_LOG(want ? "Body: the character's own head is hidden in first person (%d meshes)" : "Body: the character's head is shown again (%d meshes)", count);
            hidden_pawn = want ? s.pawn : nullptr;
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
                if (length(s.actor_delta) > 100.0) s.actor_delta = {}; // a teleport, not a step
            }
            s.head_pull_wanted = body && s.have_body_world && s.have_view && s.view.head_valid;
            pull_update(arms);
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
        HSVR_LOG(g_state.enabled_by_key ? "Key: motion arms on" : "Key: motion arms off (the game's own arm animation)");
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
        calibrate_tick();
        compute_targets();
        recorder_tick();
        if (s.enabled_by_key && !menu_up && s.hands_valid) sync_aim();
        // The hook that pulls the hands only runs while the arms are driven; otherwise the
        // game's own muscle settings are put back here.
        if (!s.enabled_by_key || menu_up)
        {
            s.head_pull_wanted = false;
            s.head_applied = false;
            pull_update(false);
        }
        log_status(cfg);
    }
} // namespace hsvr
