// HalfSwordVR: native VR for Half Sword, loaded by UE4SS.
// This file is the glue to the game: it starts the headset link, installs our stereo device in
// the running engine, and tells it where the player's head is each frame.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <initializer_list>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <map>
#include <unordered_map>
#include <string>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Mod/CppUserModBase.hpp>
#include <Unreal/FProperty.hpp>
#include <Unreal/Property/FBoolProperty.hpp>
#include <Unreal/Property/FStructProperty.hpp>
#include <Unreal/UScriptStruct.hpp>
#include <Unreal/FString.hpp>
#include <Unreal/Hooks.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UAssetRegistry.hpp>
#include <Unreal/UAssetRegistryHelpers.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "config.hpp"
#include "controllers.hpp"
#include "engine_slot.hpp"
#include "input_watch.hpp"
#include "log.hpp"
#include "stereo_device.hpp"
#include "ue_abi.hpp"
#include "vr_present.hpp"

#include <windows.h>

using namespace RC;
using namespace RC::Unreal;

namespace
{
    // ---- reflection helpers (game thread) ------------------------------------------------------
    UObject* object_property(UObject* owner, const TCHAR* name)
    {
        if (!owner) return nullptr;
        UObject** value = owner->GetValuePtrByPropertyNameInChain<UObject*>(name);
        return value ? *value : nullptr;
    }

    // Reads a yes/no property that the engine packs into a single bit.
    bool bool_property(UObject* owner, const TCHAR* name)
    {
        if (!owner) return false;
        FProperty* property = owner->GetPropertyByNameInChain(name);
        if (!property) return false;
        auto* flag = static_cast<FBoolProperty*>(property);
        const uint8_t* byte = reinterpret_cast<const uint8_t*>(owner) + property->GetOffset_Internal() + flag->GetByteOffset();
        return (*byte & flag->GetFieldMask()) != 0;
    }

    // What a widget with a "Brush" (an Image) draws: a texture, a material or a render target.
    UObject* brush_resource(UObject* widget)
    {
        if (!widget) return nullptr;
        FProperty* property = widget->GetPropertyByNameInChain(STR("Brush"));
        if (!property) return nullptr;
        UScriptStruct* brush_struct = static_cast<FStructProperty*>(property)->GetStruct();
        if (!brush_struct) return nullptr;
        for (FProperty* member : brush_struct->ForEachProperty())
        {
            if (member->GetName() != STR("ResourceObject")) continue;
            const uint8_t* brush = reinterpret_cast<const uint8_t*>(widget) + property->GetOffset_Internal();
            return *reinterpret_cast<UObject* const*>(brush + member->GetOffset_Internal());
        }
        return nullptr;
    }

    struct RawArray
    {
        void** Data;
        int32_t Num;
        int32_t Max;
    };

    UObject* g_engine = nullptr;
    bool g_vr_started = false;
    bool g_install_failed = false;
    bool g_console_done = false;
    UObject* g_logged_pawn = nullptr;

    UObject* find_player_controller()
    {
        UObject* viewport = object_property(g_engine, STR("GameViewport"));
        UObject* game_instance = object_property(viewport, STR("GameInstance"));
        if (!game_instance) return nullptr;
        RawArray* players = game_instance->GetValuePtrByPropertyNameInChain<RawArray>(STR("LocalPlayers"));
        if (!players || players->Num < 1 || !players->Data) return nullptr;
        UObject* local_player = static_cast<UObject*>(players->Data[0]);
        return object_property(local_player, STR("PlayerController"));
    }

    UObject* find_pawn_mesh(UObject** out_pawn = nullptr)
    {
        UObject* controller = find_player_controller();
        UObject* pawn = object_property(controller, STR("Pawn"));
        if (out_pawn) *out_pawn = pawn;
        if (!pawn) return nullptr;
        return object_property(pawn, STR("Mesh"));
    }

    struct SocketLocationParams
    {
        FName InSocketName;
        ue::FVector ReturnValue;
    };

    struct ActorLocationParams
    {
        ue::FVector ReturnValue;
    };

    struct BoneNameParams
    {
        int32_t BoneIndex;
        FName ReturnValue;
    };

    struct NumBonesParams
    {
        int32_t ReturnValue;
    };

    FName head_bone_name()
    {
        static std::string cached_text;
        static FName cached{};
        std::string wanted = hsvr::config().head_bone;
        if (wanted != cached_text)
        {
            cached_text = wanted;
            std::wstring wide(wanted.begin(), wanted.end());
            cached = FName(wide.c_str(), FNAME_Add);
        }
        return cached;
    }

    void log_bones_once(UObject* pawn, UObject* mesh)
    {
        if (pawn == g_logged_pawn) return;
        g_logged_pawn = pawn;
        HSVR_LOG("Player pawn: %ls", pawn->GetFullName().c_str());
        HSVR_LOG("Player mesh: %ls", mesh->GetFullName().c_str());
        UFunction* num_fn = mesh->GetFunctionByNameInChain(STR("GetNumBones"));
        UFunction* name_fn = mesh->GetFunctionByNameInChain(STR("GetBoneName"));
        if (!num_fn || !name_fn) return;
        NumBonesParams count{};
        mesh->ProcessEvent(num_fn, &count);
        std::wstring line;
        for (int32_t i = 0; i < count.ReturnValue && i < 400; ++i)
        {
            BoneNameParams p{};
            p.BoneIndex = i;
            mesh->ProcessEvent(name_fn, &p);
            line += p.ReturnValue.ToString();
            line += L" ";
            if (line.size() > 900)
            {
                HSVR_LOG("Bones: %ls", line.c_str());
                line.clear();
            }
        }
        if (!line.empty()) HSVR_LOG("Bones (%d total): %ls", count.ReturnValue, line.c_str());
    }

    // Phase 3: while the body is posed from the headset, the view must not follow the head bone
    // (the bone follows the view). The head's offset from the actor, in the actor's heading frame,
    // is taken once when posing starts and used instead.
    bool g_pose_anchor_fixed = false;
    hsvr::Vec3 g_pose_anchor_local;
    // How far your walking in the room has already been handed to the game's capsule (in the
    // view's heading frame), so it is not counted twice.
    hsvr::Vec3 g_pose_room_moved_local;
    // Body turning (see "Your body faces where you face"): the turn you make with the mouse or
    // stick (the view's straight ahead), and the body's yaw relative to it.
    bool g_turn_valid = false;
    double g_turn_yaw = 0.0, g_body_rel = 0.0, g_last_written_yaw = 0.0, g_turn_time = 0.0;

    // Called by the stereo device while the frame's views are being set up (game thread).
    bool provide_anchor(hsvr::PlayerAnchor& out)
    {
        if (!g_engine) return false;
        UObject* pawn = nullptr;
        UObject* mesh = find_pawn_mesh(&pawn);
        if (!pawn || !mesh) return false;
        log_bones_once(pawn, mesh);

        UFunction* socket_fn = mesh->GetFunctionByNameInChain(STR("GetSocketLocation"));
        UFunction* actor_fn = pawn->GetFunctionByNameInChain(STR("K2_GetActorLocation"));
        if (!socket_fn || !actor_fn) return false;
        SocketLocationParams socket{};
        socket.InSocketName = head_bone_name();
        mesh->ProcessEvent(socket_fn, &socket);
        ActorLocationParams actor{};
        pawn->ProcessEvent(actor_fn, &actor);
        out.head_position = {socket.ReturnValue.X, socket.ReturnValue.Y, socket.ReturnValue.Z};
        out.actor_position = {actor.ReturnValue.X, actor.ReturnValue.Y, actor.ReturnValue.Z};
        out.has_head = true;
        double body_yaw = 0.0;
        if (UObject* controller = find_player_controller())
        {
            if (ue::FRotator* rotation = controller->GetValuePtrByPropertyNameInChain<ue::FRotator>(STR("ControlRotation")))
            {
                out.has_heading = true;
                body_yaw = rotation->Yaw; // with body turning on, the mod writes the body's yaw here
                out.heading_yaw = (g_pose_anchor_fixed && g_turn_valid) ? g_turn_yaw : rotation->Yaw;
            }
        }
        // The view sits where his head is: the head's spot on his body turns with his body, while the
        // distance walked in the room is kept in the view's own frame (straight ahead = mouse/stick turn).
        // Turning both with the view's frame put the eyes in front of or behind his neck once the body
        // faced elsewhere (180 degrees round: looking down at his own shoulders from behind).
        if (g_pose_anchor_fixed)
            out.head_position = out.actor_position + hsvr::quat_from_yaw_deg(body_yaw).rotate(g_pose_anchor_local) -
                                hsvr::quat_from_yaw_deg(out.has_heading ? out.heading_yaw : 0.0).rotate(g_pose_room_moved_local);
        return true;
    }

    // ---- Phase 2: controllers in the log, and the player physics test -------------------------
    FName bone_fname(const std::string& text)
    {
        static std::map<std::string, FName> cache;
        auto it = cache.find(text);
        if (it != cache.end()) return it->second;
        std::wstring wide(text.begin(), text.end());
        FName name(wide.c_str(), FNAME_Add);
        cache.emplace(text, name);
        return name;
    }

    bool bone_location(UObject* mesh, const std::string& bone, hsvr::Vec3& out)
    {
        if (!mesh) return false;
        UFunction* socket_fn = mesh->GetFunctionByNameInChain(STR("GetSocketLocation"));
        if (!socket_fn) return false;
        SocketLocationParams socket{};
        socket.InSocketName = bone_fname(bone);
        mesh->ProcessEvent(socket_fn, &socket);
        out = {socket.ReturnValue.X, socket.ReturnValue.Y, socket.ReturnValue.Z};
        return true;
    }

    // Once a second: where each controller is (relative to the headset, and in the game world),
    // how far that is from the character's hand bone, and what the buttons and sticks read.
    void log_controllers(const hsvr::Config& cfg)
    {
        if (!cfg.log_controllers) return;
        static double last = 0.0;
        double now = hsvr::now_seconds();
        if (now - last < 1.0) return;
        last = now;
        hsvr::ControllerState s = hsvr::controllers_state();
        if (!s.active)
        {
            HSVR_LOG_EVERY(10.0, "Controllers: not read yet (headset session not running, or no action set)");
            return;
        }
        UObject* mesh = find_pawn_mesh();
        const char* names[2] = {"left", "right"};
        const std::string* bones[2] = {&cfg.hand_bone_left, &cfg.hand_bone_right};
        for (int h = 0; h < 2; ++h)
        {
            const hsvr::HandState& hand = s.hand[h];
            char where[256] = "not tracked";
            if (hand.grip_valid)
            {
                int n = 0;
                if (s.head_valid)
                {
                    // Offset from the point between the eyes, in the head's heading: forward, right, up (cm).
                    hsvr::Quat head_rotation = hsvr::xr_to_ue_rotation(s.head.qx, s.head.qy, s.head.qz, s.head.qw);
                    hsvr::Quat heading = hsvr::quat_from_yaw_deg(hsvr::quat_yaw_deg(head_rotation));
                    hsvr::Vec3 rel = heading.inverse().rotate(hsvr::xr_to_ue_position(hand.grip.px, hand.grip.py, hand.grip.pz, 100.0) -
                                                              hsvr::xr_to_ue_position(s.head.px, s.head.py, s.head.pz, 100.0));
                    n += snprintf(where + n, sizeof(where) - n, "from head fwd %.0f right %.0f up %.0f cm", rel.x, rel.y, rel.z);
                }
                hsvr::Vec3 world;
                hsvr::Quat world_rotation;
                if (hsvr::stereo_tracking_to_world(hand.grip, world, world_rotation))
                {
                    hsvr::Rot r = hsvr::quat_to_rot(world_rotation);
                    n += snprintf(where + n, sizeof(where) - n, ", world (%.0f, %.0f, %.0f) rot p%.0f y%.0f r%.0f", world.x, world.y, world.z, r.pitch, r.yaw, r.roll);
                    hsvr::Vec3 bone;
                    if (bone_location(mesh, *bones[h], bone))
                    {
                        hsvr::Vec3 d = world - bone;
                        n += snprintf(where + n, sizeof(where) - n, ", %.0f cm from %s", std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z), bones[h]->c_str());
                    }
                }
            }
            HSVR_LOG("Controller %s%s: %s | trigger %.2f grip %.2f stick %.2f %.2f%s%s%s%s", names[h], s.focused ? "" : " (no input focus)", where,
                     hand.trigger, hand.squeeze, hand.stick_x, hand.stick_y, hand.stick_click ? " [stick]" : "", hand.primary ? " [A/X]" : "",
                     hand.secondary ? " [B/Y]" : "", hand.menu ? " [menu]" : "");
        }
    }

    struct SimulatePhysicsParams
    {
        bool bSimulate;
    };

    struct IsSimulatingParams
    {
        FName BoneName;
        bool ReturnValue;
    };

    int is_simulating(UObject* mesh, const std::string& bone)
    {
        UFunction* fn = mesh->GetFunctionByNameInChain(STR("IsSimulatingPhysics"));
        if (!fn) return -1;
        IsSimulatingParams p{};
        p.BoneName = bone_fname(bone);
        mesh->ProcessEvent(fn, &p);
        return p.ReturnValue ? 1 : 0;
    }

    // SetSimulatePhysics on a skeletal mesh only changes the bodies whose physics asset entry is
    // "Default". Willie's mesh uses Willie_PhysicsAsset_Capsules, whose 22 bodies are all marked
    // Kinematic (decoded 2026-11-17), and the game makes them simulate itself, so SetSimulatePhysics
    // leaves them alone (the pelvis and hand_r still simulated in the same tick). SetAllBodiesSimulatePhysics changes every
    // body regardless, so it is called after it. Each step is logged with what the pelvis reads after.
    void set_simulate(UObject* mesh, bool simulate, bool report = false)
    {
        UFunction* whole = mesh->GetFunctionByNameInChain(STR("SetSimulatePhysics"));
        UFunction* every_body = mesh->GetFunctionByNameInChain(STR("SetAllBodiesSimulatePhysics"));
        if (whole)
        {
            SimulatePhysicsParams p{simulate};
            mesh->ProcessEvent(whole, &p);
        }
        int after_whole = report ? is_simulating(mesh, "pelvis") : -1;
        if (every_body)
        {
            SimulatePhysicsParams p{simulate};
            mesh->ProcessEvent(every_body, &p);
        }
        if (report)
            HSVR_LOG("Physics test: set %d: SetSimulatePhysics %s, pelvis then %d; SetAllBodiesSimulatePhysics %s, pelvis then %d", (int)simulate,
                     whole ? "called" : "not found", after_whole, every_body ? "called" : "not found", is_simulating(mesh, "pelvis"));
    }

    std::atomic<bool> g_joint_trace{false}; // hsvr.joint_trace 1: the right hand joint every tick, before and after our free

    // Once a second while log_body is on: where the character is.
    void log_body(UObject* pawn, UObject* mesh, const hsvr::Config& cfg)
    {
        static double last = 0.0;
        double now = hsvr::now_seconds();
        if (now - last < 1.0) return;
        last = now;
        UFunction* actor_fn = pawn->GetFunctionByNameInChain(STR("K2_GetActorLocation"));
        ActorLocationParams actor{};
        if (actor_fn) pawn->ProcessEvent(actor_fn, &actor);
        hsvr::Vec3 pelvis, head, hand_l, hand_r;
        bone_location(mesh, "pelvis", pelvis);
        bone_location(mesh, "Head", head);
        bone_location(mesh, cfg.hand_bone_left, hand_l);
        bone_location(mesh, cfg.hand_bone_right, hand_r);
        HSVR_LOG("Body: actor (%.0f, %.0f, %.0f) pelvis (%.0f, %.0f, %.0f) head (%.0f, %.0f, %.0f) hand_l (%.0f, %.0f, %.0f) hand_r (%.0f, %.0f, %.0f) | head above actor %.0f cm, physics pelvis %d",
                 actor.ReturnValue.X, actor.ReturnValue.Y, actor.ReturnValue.Z, pelvis.x, pelvis.y, pelvis.z, head.x, head.y, head.z, hand_l.x, hand_l.y, hand_l.z,
                 hand_r.x, hand_r.y, hand_r.z, head.z - actor.ReturnValue.Z, is_simulating(mesh, "pelvis"));
    }

    void body_log_tick(const hsvr::Config& cfg)
    {
        if (!cfg.log_body) return;
        UObject* pawn = nullptr;
        UObject* mesh = find_pawn_mesh(&pawn);
        if (mesh && pawn && pawn->GetFullName().find(STR("Willie_BP")) != std::wstring::npos) log_body(pawn, mesh, cfg);
    }

    // ---- Phase 3 probe: posing bones through ABP_Willie's Modify Bone nodes ----------------------
    // ABP_Willie ends in one FAnimNode_ModifyBone per main bone (decompile, internals-map "Posing for
    // Phase 3"); the game only writes their Scale. Every field is found by name through reflection.
    FProperty* struct_member(UScriptStruct* type, const TCHAR* name)
    {
        if (!type) return nullptr;
        for (UStruct* level = type; level; level = level->GetSuperStruct())
            for (FProperty* member : level->ForEachProperty())
                if (member->GetName() == name) return member;
        return nullptr;
    }

    UScriptStruct* struct_type(FProperty* property)
    {
        if (!property || property->GetClass().GetName() != STR("StructProperty")) return nullptr;
        return static_cast<FStructProperty*>(property)->GetStruct();
    }

    // Writes three numbers into a vector or rotator field (float or double members, in order).
    bool write_three(uint8_t* field, FProperty* property, double a, double b, double c)
    {
        UScriptStruct* type = struct_type(property);
        if (!type) return false;
        double values[3] = {a, b, c};
        int i = 0;
        for (FProperty* member : type->ForEachProperty())
        {
            if (i >= 3) break;
            auto kind = member->GetClass().GetName();
            uint8_t* at = field + member->GetOffset_Internal();
            if (kind == STR("DoubleProperty")) *reinterpret_cast<double*>(at) = values[i];
            else if (kind == STR("FloatProperty")) *reinterpret_cast<float*>(at) = (float)values[i];
            else return false;
            ++i;
        }
        return i == 3;
    }

    void read_three(const uint8_t* field, FProperty* property, double out[3])
    {
        out[0] = out[1] = out[2] = 0.0;
        UScriptStruct* type = struct_type(property);
        if (!type) return;
        int i = 0;
        for (FProperty* member : type->ForEachProperty())
        {
            if (i >= 3) break;
            auto kind = member->GetClass().GetName();
            const uint8_t* at = field + member->GetOffset_Internal();
            if (kind == STR("DoubleProperty")) out[i] = *reinterpret_cast<const double*>(at);
            else if (kind == STR("FloatProperty")) out[i] = *reinterpret_cast<const float*>(at);
            ++i;
        }
    }

    struct SocketRotationParams
    {
        FName InSocketName;
        ue::FRotator ReturnValue;
    };

    struct ModifyBoneNode
    {
        std::wstring bone;
        uint8_t* node = nullptr; // inside the anim instance
        FProperty* rotation = nullptr;
        FProperty* translation = nullptr;
        FProperty* rotation_mode = nullptr;
        FProperty* rotation_space = nullptr;
        FProperty* translation_mode = nullptr;
        FProperty* translation_space = nullptr;
    };

    // The player's DriverSkeleton anim instance (ABP_Willie). Enemies run the same ABP; this is
    // reached through the player's own pawn, so only the player's is touched.
    UObject* player_pose_instance(UObject* pawn)
    {
        UObject* driver = object_property(pawn, STR("DriverSkeleton"));
        static UObject* reported = nullptr;
        if (!driver && reported != pawn)
        {
            reported = pawn;
            HSVR_LOG("Pose probe: %ls has no DriverSkeleton variable", pawn->GetFullName().c_str());
        }
        return object_property(driver, STR("AnimScriptInstance"));
    }

    std::vector<ModifyBoneNode> find_modify_bone_nodes(UObject* instance)
    {
        std::vector<ModifyBoneNode> nodes;
        for (UStruct* level = instance->GetClassPrivate(); level; level = level->GetSuperStruct())
        {
            for (FProperty* property : level->ForEachProperty())
            {
                UScriptStruct* type = struct_type(property);
                if (!type || type->GetName() != STR("AnimNode_ModifyBone")) continue;
                ModifyBoneNode n;
                n.node = reinterpret_cast<uint8_t*>(instance) + property->GetOffset_Internal();
                FProperty* bone_ref = struct_member(type, STR("BoneToModify"));
                FProperty* bone_name = struct_member(struct_type(bone_ref), STR("BoneName"));
                if (bone_ref && bone_name)
                    n.bone = reinterpret_cast<const FName*>(n.node + bone_ref->GetOffset_Internal() + bone_name->GetOffset_Internal())->ToString();
                n.rotation = struct_member(type, STR("Rotation"));
                n.translation = struct_member(type, STR("Translation"));
                n.rotation_mode = struct_member(type, STR("RotationMode"));
                n.rotation_space = struct_member(type, STR("RotationSpace"));
                n.translation_mode = struct_member(type, STR("TranslationMode"));
                n.translation_space = struct_member(type, STR("TranslationSpace"));
                nodes.push_back(n);
            }
        }
        return nodes;
    }

    void pose_probe(const hsvr::Config& cfg)
    {
        static UObject* logged_for = nullptr;
        static bool was_on = false;
        static std::vector<ModifyBoneNode> nodes;
        static uint8_t saved_mode = 0, saved_space = 0;
        static ModifyBoneNode* active = nullptr;
        UObject* pawn = nullptr;
        find_pawn_mesh(&pawn);
        UObject* instance = pawn ? player_pose_instance(pawn) : nullptr;
        if (!cfg.dev_pose_probe || !instance)
        {
            if (was_on && active && instance == logged_for)
            {
                *(active->node + active->rotation_mode->GetOffset_Internal()) = saved_mode;
                *(active->node + active->rotation_space->GetOffset_Internal()) = saved_space;
                HSVR_LOG("Pose probe: off, %ls back to the game's own setting", active->bone.c_str());
            }
            was_on = false;
            active = nullptr;
            return;
        }
        if (instance != logged_for)
        {
            logged_for = instance;
            active = nullptr;
            was_on = false;
            nodes = find_modify_bone_nodes(instance);
            HSVR_LOG("Pose probe: %ls has %d Modify Bone nodes", instance->GetFullName().c_str(), (int)nodes.size());
            for (const ModifyBoneNode& n : nodes)
            {
                double r[3], t[3];
                if (n.rotation) read_three(n.node + n.rotation->GetOffset_Internal(), n.rotation, r);
                if (n.translation) read_three(n.node + n.translation->GetOffset_Internal(), n.translation, t);
                HSVR_LOG("  %ls: rotation mode %d space %d (%.1f, %.1f, %.1f), translation mode %d space %d (%.1f, %.1f, %.1f)", n.bone.c_str(),
                         n.rotation_mode ? (int)*(n.node + n.rotation_mode->GetOffset_Internal()) : -1,
                         n.rotation_space ? (int)*(n.node + n.rotation_space->GetOffset_Internal()) : -1, r[0], r[1], r[2],
                         n.translation_mode ? (int)*(n.node + n.translation_mode->GetOffset_Internal()) : -1,
                         n.translation_space ? (int)*(n.node + n.translation_space->GetOffset_Internal()) : -1, t[0], t[1], t[2]);
            }
        }
        std::wstring wanted(cfg.dev_pose_bone.begin(), cfg.dev_pose_bone.end());
        if (!active || active->bone != wanted)
        {
            active = nullptr;
            // A bone can have several nodes (Head: the game's world-space head lock, a roll, and the
            // end-of-graph one). Take the end-of-graph one: the game leaves it at Ignore, component space.
            int index = -1;
            for (size_t i = 0; i < nodes.size(); ++i)
            {
                ModifyBoneNode& n = nodes[i];
                if (_wcsicmp(n.bone.c_str(), wanted.c_str()) != 0 || !n.rotation || !n.rotation_mode || !n.rotation_space) continue;
                bool final_chain = *(n.node + n.rotation_mode->GetOffset_Internal()) == 0 && *(n.node + n.rotation_space->GetOffset_Internal()) == 1;
                if (final_chain || !active)
                {
                    active = &n;
                    index = (int)i;
                }
            }
            if (active) HSVR_LOG("Pose probe: using node %d of the table for %ls", index, active->bone.c_str());
            if (!active)
            {
                static std::string missing;
                if (missing != cfg.dev_pose_bone) HSVR_LOG("Pose probe: no Modify Bone node for \"%s\"", cfg.dev_pose_bone.c_str());
                missing = cfg.dev_pose_bone;
                return;
            }
            saved_mode = *(active->node + active->rotation_mode->GetOffset_Internal());
            saved_space = *(active->node + active->rotation_space->GetOffset_Internal());
        }
        // EBoneModificationMode: 0 ignore, 1 replace, 2 additive. EBoneControlSpace: 0 world, 1 component, 2 parent, 3 bone.
        *(active->node + active->rotation_mode->GetOffset_Internal()) = 1;
        *(active->node + active->rotation_space->GetOffset_Internal()) = 1;
        write_three(active->node + active->rotation->GetOffset_Internal(), active->rotation, cfg.dev_pose_pitch, cfg.dev_pose_yaw, cfg.dev_pose_roll);
        if (!was_on)
            HSVR_LOG("Pose probe: %ls rotation set to (%.1f, %.1f, %.1f) in component space, replacing the animation", active->bone.c_str(),
                     cfg.dev_pose_pitch, cfg.dev_pose_yaw, cfg.dev_pose_roll);
        was_on = true;
        // Once a second: where the bone really is, to see whether the write took.
        static double last = 0.0;
        double now = hsvr::now_seconds();
        if (now - last >= 1.0)
        {
            last = now;
            hsvr::Vec3 at;
            UObject* mesh = find_pawn_mesh();
            std::string bone(cfg.dev_pose_bone);
            UFunction* rotation_fn = mesh ? mesh->GetFunctionByNameInChain(STR("GetSocketRotation")) : nullptr;
            SocketRotationParams r{};
            r.InSocketName = bone_fname(bone);
            if (rotation_fn) mesh->ProcessEvent(rotation_fn, &r);
            if (mesh && bone_location(mesh, bone, at))
                HSVR_LOG("Pose probe: %s now at (%.0f, %.0f, %.0f), world rotation pitch %.0f yaw %.0f roll %.0f", bone.c_str(), at.x, at.y, at.z,
                         r.ReturnValue.Pitch, r.ReturnValue.Yaw, r.ReturnValue.Roll);
        }
    }

    // ---- Phase 3: the body posed from the headset and controllers ------------------------------
    // Calls a Blueprint-callable function with named parameters, laid out by reflection (so no
    // parameter struct has to match the engine by hand).
    // A zeroed parameter block big enough for every parameter of the function.
    std::vector<uint8_t> param_block(UFunction* fn)
    {
        size_t size = 16;
        if (fn)
        {
            for (FProperty* p : fn->ForEachProperty()) size = (std::max)(size, (size_t)p->GetOffset_Internal() + (size_t)p->GetSize());
        }
        return std::vector<uint8_t>(size + 64, 0);
    }

    struct Call
    {
        UObject* target = nullptr;
        UFunction* fn = nullptr;
        std::vector<uint8_t> buffer;

        Call(UObject* object, const TCHAR* name) : target(object)
        {
            fn = object ? object->GetFunctionByNameInChain(name) : nullptr;
            size_t size = 16;
            if (fn)
                for (FProperty* p : fn->ForEachProperty()) size = (std::max)(size, (size_t)p->GetOffset_Internal() + (size_t)p->GetSize());
            buffer.assign(size + 16, 0);
        }
        FProperty* param(const TCHAR* name)
        {
            if (!fn) return nullptr;
            for (FProperty* p : fn->ForEachProperty())
                if (p->GetName() == name) return p;
            return nullptr;
        }
        void three(const TCHAR* name, double a, double b, double c)
        {
            if (FProperty* p = param(name)) write_three(buffer.data() + p->GetOffset_Internal(), p, a, b, c);
        }
        void flag(const TCHAR* name, bool value)
        {
            FProperty* p = param(name);
            if (!p) return;
            auto* b = static_cast<FBoolProperty*>(p);
            uint8_t* at = buffer.data() + p->GetOffset_Internal() + b->GetByteOffset();
            if (value) *at |= b->GetFieldMask();
            else *at &= ~b->GetFieldMask();
        }
        // The function's first yes/no parameter, whatever it is called.
        void first_flag(bool value)
        {
            if (!fn) return;
            for (FProperty* p : fn->ForEachProperty())
            {
                if (p->GetClass().GetName() != STR("BoolProperty")) continue;
                auto* b = static_cast<FBoolProperty*>(p);
                uint8_t* at = buffer.data() + p->GetOffset_Internal() + b->GetByteOffset();
                if (value) *at |= b->GetFieldMask();
                else *at &= ~b->GetFieldMask();
                return;
            }
        }
        void byte(const TCHAR* param_name, uint8_t value)
        {
            if (FProperty* p = param(param_name)) *(buffer.data() + p->GetOffset_Internal()) = value;
        }
        void name(const TCHAR* param_name, FName value)
        {
            if (FProperty* p = param(param_name)) *reinterpret_cast<FName*>(buffer.data() + p->GetOffset_Internal()) = value;
        }
        bool run()
        {
            if (!fn || !target) return false;
            target->ProcessEvent(fn, buffer.data());
            return true;
        }
        hsvr::Vec3 vec(const TCHAR* name = STR("ReturnValue"))
        {
            double v[3] = {0, 0, 0};
            if (FProperty* p = param(name)) read_three(buffer.data() + p->GetOffset_Internal(), p, v);
            return {v[0], v[1], v[2]};
        }
        void number(const TCHAR* name, double value)
        {
            FProperty* p = param(name);
            if (!p) return;
            uint8_t* at = buffer.data() + p->GetOffset_Internal();
            auto kind = p->GetClass().GetName();
            if (kind == STR("FloatProperty")) *reinterpret_cast<float*>(at) = (float)value;
            else if (kind == STR("DoubleProperty")) *reinterpret_cast<double*>(at) = value;
        }
        double number(const TCHAR* name = STR("ReturnValue"))
        {
            FProperty* p = param(name);
            if (!p) return -1.0;
            uint8_t* at = buffer.data() + p->GetOffset_Internal();
            auto kind = p->GetClass().GetName();
            if (kind == STR("FloatProperty")) return *reinterpret_cast<float*>(at);
            if (kind == STR("DoubleProperty")) return *reinterpret_cast<double*>(at);
            return -1.0;
        }
        void object(const TCHAR* name, UObject* value)
        {
            if (FProperty* p = param(name)) *reinterpret_cast<UObject**>(buffer.data() + p->GetOffset_Internal()) = value;
        }
        UObject* object_result(const TCHAR* name = STR("ReturnValue"))
        {
            FProperty* p = param(name);
            return p ? *reinterpret_cast<UObject**>(buffer.data() + p->GetOffset_Internal()) : nullptr;
        }
        int32_t integer(const TCHAR* name = STR("ReturnValue"))
        {
            FProperty* p = param(name);
            return p ? *reinterpret_cast<int32_t*>(buffer.data() + p->GetOffset_Internal()) : -1;
        }
        hsvr::Quat rot(const TCHAR* name = STR("ReturnValue"))
        {
            hsvr::Vec3 r = vec(name);
            return hsvr::rot_to_quat(r.x, r.y, r.z);
        }
    };

    hsvr::Vec3 socket_location(UObject* mesh, FName bone)
    {
        Call c(mesh, STR("GetSocketLocation"));
        c.name(STR("InSocketName"), bone);
        c.run();
        return c.vec();
    }

    hsvr::Quat socket_rotation(UObject* mesh, FName bone)
    {
        Call c(mesh, STR("GetSocketRotation"));
        c.name(STR("InSocketName"), bone);
        c.run();
        return c.rot();
    }

    hsvr::Quat component_rotation(UObject* component)
    {
        Call c(component, STR("K2_GetComponentRotation"));
        c.run();
        return c.rot();
    }

    hsvr::Vec3 component_location(UObject* component)
    {
        Call c(component, STR("K2_GetComponentLocation"));
        c.run();
        return c.vec();
    }

    void set_component_transform(UObject* component, const hsvr::Vec3& location, const hsvr::Quat& rotation, bool teleport = true)
    {
        Call c(component, STR("K2_SetWorldLocationAndRotation"));
        hsvr::Rot r = hsvr::quat_to_rot(rotation);
        c.three(STR("NewLocation"), location.x, location.y, location.z);
        c.three(STR("NewRotation"), r.pitch, r.yaw, r.roll);
        c.flag(STR("bSweep"), false);
        c.flag(STR("bTeleport"), teleport);
        c.run();
    }

    double component_scale_z(UObject* component)
    {
        Call c(component, STR("K2_GetComponentScale"));
        c.run();
        double z = c.vec().z;
        return z > 0.01 ? z : 1.0;
    }

    // The end-of-graph Modify Bone node of each bone (the game leaves them at Ignore, component space).
    std::map<std::wstring, ModifyBoneNode> final_chain_nodes(UObject* instance)
    {
        std::map<std::wstring, ModifyBoneNode> chain;
        std::vector<ModifyBoneNode> nodes = find_modify_bone_nodes(instance);
        for (size_t i = 0; i < nodes.size(); ++i)
        {
            ModifyBoneNode& n = nodes[i];
            if (!n.rotation || !n.rotation_mode || !n.rotation_space) continue;
            if (*(n.node + n.rotation_mode->GetOffset_Internal()) != 0 || *(n.node + n.rotation_space->GetOffset_Internal()) != 1) continue;
            std::wstring key = n.bone;
            for (auto& ch : key) ch = (wchar_t)towlower(ch);
            chain[key] = n; // the last one found wins
            HSVR_LOG("Pose: bone %ls -> node %d", n.bone.c_str(), (int)i);
        }
        return chain;
    }

    void node_set_rotation(ModifyBoneNode& n, const hsvr::Quat& component_space)
    {
        hsvr::Rot r = hsvr::quat_to_rot(component_space);
        *(n.node + n.rotation_space->GetOffset_Internal()) = 1; // component space
        write_three(n.node + n.rotation->GetOffset_Internal(), n.rotation, r.pitch, r.yaw, r.roll);
        *(n.node + n.rotation_mode->GetOffset_Internal()) = 1; // replace
    }

    // Adds a turn on top of whatever the game's animation gives the bone (component space), so his
    // own breathing, leaning and crouch bend stay underneath.
    void node_add_rotation(ModifyBoneNode& n, const hsvr::Quat& component_space)
    {
        hsvr::Rot r = hsvr::quat_to_rot(component_space);
        *(n.node + n.rotation_space->GetOffset_Internal()) = 1; // component space
        write_three(n.node + n.rotation->GetOffset_Internal(), n.rotation, r.pitch, r.yaw, r.roll);
        *(n.node + n.rotation_mode->GetOffset_Internal()) = 2; // additive
    }

    void node_release(ModifyBoneNode& n)
    {
        *(n.node + n.rotation_mode->GetOffset_Internal()) = 0;
    }

    struct ArmCalibration
    {
        bool valid = false;
        double upper_length = 0, lower_length = 0;
        hsvr::Vec3 upper_dir_local, upper_normal_local; // in the upperarm bone's frame
        hsvr::Vec3 lower_dir_local, lower_normal_local; // in the lowerarm bone's frame
        hsvr::Vec3 fingers_local, thumb_local;           // in the hand bone's frame: toward the fingers, toward the thumb side
        hsvr::Quat clavicle_in_chest;  // clavicle rotation in spine_05's frame, as the animation has it at calibration
        hsvr::Vec3 shoulder_in_clavicle; // the upperarm joint in the clavicle's frame
        bool clavicle_valid = false;
        double twist_deg = 0, bend_deg = 0;              // last frame, for the log
    };

    struct PoseState
    {
        UObject* pawn = nullptr;
        UObject* mesh = nullptr;
        UObject* driver = nullptr;
        UObject* instance = nullptr;
        std::map<std::wstring, ModifyBoneNode> chain;
        bool calibrated = false;
        hsvr::Vec3 mesh_offset_local; // visible mesh relative to the actor, in the actor's heading frame
        hsvr::Quat mesh_rotation_local;
        hsvr::Quat head_offset;       // body frame -> head bone
        ArmCalibration arm[2];
        double last_check = 0.0;
        hsvr::Vec3 target[2]; // last hand targets (where the hand bone goes, from the controller)
        hsvr::Quat hand_rot[2]; // and the hand bone's world rotation there
        hsvr::Vec3 raw_target[2]; // the same before scaling your reach to his (for measuring your arm)
        bool hand_valid[2] = {false, false};
        hsvr::Vec3 swivel_local[2]; // smoothed elbow direction, in the body's frame
        bool swivel_valid[2] = {false, false};
        double twist_deg = 0.0; // how far the spine and neck together turned this frame, for the log
        // Feet (see "Feet that stand and step" below).
        hsvr::Vec3 foot_home_local[2];   // where each foot stands relative to the actor, in the body's frame
        hsvr::Quat foot_rot_local[2];    // body frame -> foot bone, standing
        double ankle_height[2] = {10.0, 10.0}; // foot bone above the floor, standing
        bool ankle_measured[2] = {false, false};
        double calibrated_at = 0.0;
        hsvr::Quat chest_rot_local, pelvis_rot_local; // body frame -> spine_05 / pelvis at calibration, for the twist log
        bool feet_calibrated = false;
        double swivel_time[2] = {0.0, 0.0};
    };
    PoseState g_pose;
    std::atomic<bool> g_hold_capsule{false}; // see "The capsule stays where you are" below
    // Posed and calibrated this frame (crouch and body turning run around his tick).
    std::atomic<bool> g_pose_active{false};
    bool g_body_turning = false;
    // Crouch: how far your headset is below where you stood at recentering (cm, 0 standing).
    double g_crouch_drop = 0.0;
    bool g_pose_cfg_turn = true, g_pose_cfg_crouch = true, g_pose_fake = false;
    double g_turn_dead_deg = 60.0, g_turn_speed_dps = 150.0, g_turn_max_deg = 85.0;

    const char* side_suffix(int side) { return side == 0 ? "_l" : "_r"; }

    ModifyBoneNode* chain_node(const std::string& bone)
    {
        std::wstring key(bone.begin(), bone.end());
        auto it = g_pose.chain.find(key);
        return it == g_pose.chain.end() ? nullptr : &it->second;
    }

    void feet_reset();
    bool ground_below(UObject* pawn, double x, double y, double from_z, double& ground_z);

    void pose_release()
    {
        for (auto& [bone, n] : g_pose.chain) node_release(n);
        if (g_pose.mesh && g_pose.calibrated)
        {
            set_simulate(g_pose.mesh, true, true);
            HSVR_LOG("Pose: off, physics back on for your character (restart the fight to be sure)");
        }
        g_pose = PoseState{};
        g_pose_anchor_fixed = false;
        g_pose_room_moved_local = {};
        feet_reset();
    }

    double actor_heading(UObject* pawn)
    {
        if (UObject* controller = find_player_controller())
            if (ue::FRotator* rotation = controller->GetValuePtrByPropertyNameInChain<ue::FRotator>(STR("ControlRotation"))) return rotation->Yaw;
        Call c(pawn, STR("K2_GetActorRotation"));
        c.run();
        return c.vec().y;
    }

    hsvr::Vec3 actor_location(UObject* pawn)
    {
        Call c(pawn, STR("K2_GetActorLocation"));
        c.run();
        return c.vec();
    }

    bool bone_exists(UObject* mesh, const std::string& bone)
    {
        Call c(mesh, STR("GetBoneIndex"));
        c.name(STR("BoneName"), bone_fname(bone));
        return c.run() && c.integer() >= 0;
    }

    void apply_arm_physics(const hsvr::Config&)
    {
        set_simulate(g_pose.mesh, false);
    }

    bool pose_calibrate(const hsvr::Config& cfg)
    {
        using namespace hsvr;
        UObject* d = g_pose.driver;
        double heading = actor_heading(g_pose.pawn);
        Quat body = quat_from_yaw_deg(heading);
        Quat body_inv = body.inverse();
        Vec3 actor = actor_location(g_pose.pawn);

        apply_arm_physics(cfg);
        Vec3 mesh_location = component_location(g_pose.mesh);
        g_pose.mesh_offset_local = body_inv.rotate(mesh_location - actor);
        g_pose.mesh_rotation_local = (body_inv * component_rotation(g_pose.mesh)).normalized();

        Vec3 head = socket_location(d, bone_fname("Head"));
        g_pose.head_offset = (body_inv * socket_rotation(d, bone_fname("Head"))).normalized();
        g_pose_anchor_local = body_inv.rotate(head - actor);
        g_pose_anchor_fixed = true;
        g_pose_room_moved_local = {};

        g_pose.chest_rot_local = (body_inv * socket_rotation(g_pose.mesh, bone_fname("spine_05"))).normalized();
        g_pose.pelvis_rot_local = (body_inv * socket_rotation(g_pose.mesh, bone_fname("pelvis"))).normalized();
        g_pose.feet_calibrated = true;
        for (int side = 0; side < 2; ++side)
        {
            FName foot = bone_fname(side == 0 ? "foot_l" : "foot_r");
            if (!bone_exists(g_pose.mesh, side == 0 ? "foot_l" : "foot_r"))
            {
                g_pose.feet_calibrated = false;
                continue;
            }
            g_pose.foot_home_local[side] = body_inv.rotate(socket_location(g_pose.mesh, foot) - actor);
            g_pose.foot_rot_local[side] = (body_inv * socket_rotation(d, foot)).normalized();
            Vec3 foot_at = socket_location(g_pose.mesh, foot);
            double floor_z = 0.0;
            bool hit = ground_below(g_pose.pawn, foot_at.x, foot_at.y, actor.z, floor_z);
            bool used = hit && foot_at.z - floor_z > 2.0 && foot_at.z - floor_z < 30.0;
            if (used)
            {
                g_pose.ankle_height[side] = foot_at.z - floor_z;
                g_pose.ankle_measured[side] = true;
            }
            g_pose.calibrated_at = now_seconds();
            if (hit)
                HSVR_LOG("Pose: %s ankle %.1f cm above the floor (foot bone at %.1f, floor at %.1f%s)", side == 0 ? "left" : "right",
                         g_pose.ankle_height[side], foot_at.z, floor_z, used ? "" : ", out of range: measured again once he stands");
            else
                HSVR_LOG("Pose: %s ankle: no floor found under the foot (trace from %.0f down), 10 cm used", side == 0 ? "left" : "right", actor.z);
            HSVR_LOG("Pose: %s foot stands at (%.0f, %.0f, %.0f) from the actor", side == 0 ? "left" : "right", g_pose.foot_home_local[side].x,
                     g_pose.foot_home_local[side].y, g_pose.foot_home_local[side].z);
        }

        for (int side = 0; side < 2; ++side)
        {
            ArmCalibration& a = g_pose.arm[side];
            std::string sx = side_suffix(side);
            FName upper = bone_fname("upperarm" + sx), lower = bone_fname("lowerarm" + sx), hand = bone_fname("hand" + sx);
            Vec3 S = socket_location(d, upper), E = socket_location(d, lower), H = socket_location(d, hand);
            {
                FName clav = bone_fname("clavicle" + sx);
                Quat Wch = socket_rotation(d, bone_fname("spine_05")), Wcl = socket_rotation(d, clav);
                a.clavicle_in_chest = (Wch.inverse() * Wcl).normalized();
                a.shoulder_in_clavicle = Wcl.inverse().rotate(S - socket_location(d, clav));
                a.clavicle_valid = length(a.shoulder_in_clavicle) > 1.0;
            }
            a.upper_length = length(E - S);
            a.lower_length = length(H - E);
            Vec3 n = cross(E - S, H - E);
            if (length(n) < 1e-3 * a.upper_length * a.lower_length) n = cross(body.rotate({1, 0, 0}), E - S); // straight arm: elbow bends forward
            Quat Wu = socket_rotation(d, upper), Wl = socket_rotation(d, lower), Wh = socket_rotation(d, hand);
            a.upper_dir_local = Wu.inverse().rotate(normalized(E - S));
            a.upper_normal_local = Wu.inverse().rotate(normalized(n));
            a.lower_dir_local = Wl.inverse().rotate(normalized(H - E));
            a.lower_normal_local = Wl.inverse().rotate(normalized(n));
            // Which way his hand points, in the hand bone's own frame: from the hand to the middle
            // finger's first bone, and from the little finger across to the index finger (the
            // thumb side). Without finger bones, along the forearm, with the elbow's bend as the side.
            Vec3 fingers = H - E, across = n;
            const char* how = "forearm (no finger bones)";
            if (bone_exists(d, "middle_01" + sx) && bone_exists(d, "index_01" + sx) && bone_exists(d, "pinky_01" + sx))
            {
                fingers = socket_location(d, bone_fname("middle_01" + sx)) - H;
                across = socket_location(d, bone_fname("index_01" + sx)) - socket_location(d, bone_fname("pinky_01" + sx));
                how = "finger bones";
            }
            a.fingers_local = normalized(Wh.inverse().rotate(fingers));
            Vec3 t = Wh.inverse().rotate(across);
            a.thumb_local = normalized(t - a.fingers_local * dot(t, a.fingers_local));
            HSVR_LOG("Pose: %s hand frame from %s: fingers (%.2f, %.2f, %.2f), thumb side (%.2f, %.2f, %.2f) in the hand bone", side == 0 ? "left" : "right",
                     how, a.fingers_local.x, a.fingers_local.y, a.fingers_local.z, a.thumb_local.x, a.thumb_local.y, a.thumb_local.z);
            a.valid = a.upper_length > 5.0 && a.lower_length > 5.0 && chain_node("upperarm" + sx) && chain_node("lowerarm" + sx) && chain_node("hand" + sx);
            HSVR_LOG("Pose: %s arm upper %.1f cm, lower %.1f cm%s", side == 0 ? "left" : "right", a.upper_length, a.lower_length, a.valid ? "" : " (not usable)");
        }
        HSVR_LOG("Pose: on. Head %.0f cm above the actor, mesh offset (%.0f, %.0f, %.0f), arms %s", g_pose_anchor_local.z, g_pose.mesh_offset_local.x,
                 g_pose.mesh_offset_local.y, g_pose.mesh_offset_local.z, "exact");
        return true;
    }

    // The bone rotation that points its calibrated direction along `dir` with its calibrated
    // bend-plane normal along `normal`.
    hsvr::Quat aim_bone(const hsvr::Vec3& dir_local, const hsvr::Vec3& normal_local, const hsvr::Vec3& dir, const hsvr::Vec3& normal)
    {
        return (hsvr::quat_from_two(dir, normal) * hsvr::quat_from_two(dir_local, normal_local).inverse()).normalized();
    }

    // ---- Feet that stand and step ---------------------------------------------------------------
    // The game re-measures where each foot stands from the body every tick, so when the body turns
    // or slides the feet go with it, and it lifts a foot only once it is about 25 cm or 60-100
    // degrees off (decompile: Willie_BP @596869-598708). While posed, the mod keeps each foot where
    // it was put down, and steps it (lift and place, one foot at a time) once it is more than
    // pose_step_distance_cm from where it would stand under the hips, or turned more than
    // pose_step_angle_deg from them. The foot positions go into ABP_Willie's two foot IK effectors
    // (in the pelvis's frame) right after the game's animation update writes its own; the leg IK
    // bends the knees. The height is the floor under the foot (a trace straight down) plus his ankle
    // height when standing; the game's own foot height drifts 20-45 cm off in a deep crouch.
    // The floor's height under a point: a straight-down trace on the Visibility channel that ignores
    // your own character. False if nothing is hit within 2 m below `from_z`.
    bool ground_below(UObject* pawn, double x, double y, double from_z, double& ground_z)
    {
        static UObject* cdo = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetSystemLibrary"));
        if (!cdo || !pawn) return false;
        Call trace(cdo, STR("LineTraceSingle"));
        if (!trace.fn) return false;
        trace.object(STR("WorldContextObject"), pawn);
        trace.three(STR("Start"), x, y, from_z);
        trace.three(STR("End"), x, y, from_z - 200.0);
        trace.flag(STR("bIgnoreSelf"), true);
        if (!trace.run()) return false;
        FProperty* ret = trace.param(STR("ReturnValue"));
        if (!ret || !(*(trace.buffer.data() + ret->GetOffset_Internal()) & 1)) return false;
        FProperty* hit = trace.param(STR("OutHit"));
        FProperty* point = hit ? struct_member(struct_type(hit), STR("ImpactPoint")) : nullptr;
        UScriptStruct* vec_type = struct_type(point);
        FProperty* z = vec_type ? struct_member(vec_type, STR("Z")) : nullptr;
        if (!z) return false;
        const uint8_t* at = trace.buffer.data() + hit->GetOffset_Internal() + point->GetOffset_Internal() + z->GetOffset_Internal();
        ground_z = z->GetClass().GetName() == STR("DoubleProperty") ? *reinterpret_cast<const double*>(at) : *reinterpret_cast<const float*>(at);
        return true;
    }

    struct FootState
    {
        bool valid = false;
        hsvr::Vec3 at;          // where the foot stands (x, y; z unused)
        double yaw = 0.0;
        bool stepping = false;
        double t = 0.0, duration = 0.3;
        hsvr::Vec3 from;
        double yaw_from = 0.0;
        double lift = 0.0;      // this frame's height above the ground (cm)
        double ground_z = 0.0;  // where the foot bone sits when the foot is down: the floor plus the ankle height
        bool have_ground = false;
        int steps = 0;
    };
    FootState g_feet[2];
    std::atomic<bool> g_feet_active{false};
    int g_last_foot = -1;
    double g_feet_time = 0.0;
    FProperty* g_effector[2] = {nullptr, nullptr};
    bool g_effector_looked = false;

    double wrap_deg(double a)
    {
        while (a > 180.0) a -= 360.0;
        while (a < -180.0) a += 360.0;
        return a;
    }

    void feet_reset()
    {
        for (FootState& f : g_feet) f = FootState{};
        g_feet_active = false;
        g_last_foot = -1;
    }

    // Called each frame from pose_body, after the body is placed for this frame.
    void pose_feet(const hsvr::Config& cfg, UObject* pawn, const hsvr::Vec3& actor, double heading, const hsvr::Quat& component_inv)
    {
        using namespace hsvr;
        if (!cfg.pose_feet || !g_pose.feet_calibrated)
        {
            if (g_feet_active.load())
            {
                for (const char* b : {"foot_l", "foot_r"})
                    if (ModifyBoneNode* n = chain_node(b)) node_release(*n);
                feet_reset();
            }
            return;
        }
        double now = now_seconds();
        double dt = (std::min)(0.1, (std::max)(0.0, now - g_feet_time));
        g_feet_time = now;
        Quat body = quat_from_yaw_deg(heading);
        // Where your feet would stand under your hips, a little ahead in the direction you move.
        Vec3 velocity{0, 0, 0};
        if (UObject* movement = object_property(pawn, STR("CharacterMovement")))
            if (FProperty* vp = movement->GetPropertyByNameInChain(STR("Velocity")))
            {
                double v[3];
                read_three(reinterpret_cast<uint8_t*>(movement) + vp->GetOffset_Internal(), vp, v);
                velocity = {v[0], v[1], 0.0};
            }
        double speed = length(velocity);
        Vec3 want[2];
        double need[2];
        for (int side = 0; side < 2; ++side)
        {
            Vec3 home = g_pose.foot_home_local[side];
            want[side] = actor + body.rotate(Vec3{home.x, home.y, 0.0}) + velocity * cfg.pose_step_lead_s;
            want[side].z = 0.0;
            FootState& f = g_feet[side];
            if (!f.valid)
            {
                f.valid = true;
                f.at = want[side];
                f.yaw = heading;
            }
            Vec3 off = f.at - want[side];
            off.z = 0.0;
            double d = length(off);
            if (d > 120.0) // teleported or respawned: put both feet down at once
            {
                f.at = want[side];
                f.yaw = heading;
                f.stepping = false;
                d = 0.0;
            }
            need[side] = (std::max)(d / (std::max)(1.0, cfg.pose_step_distance_cm), std::fabs(wrap_deg(heading - f.yaw)) / (std::max)(1.0, cfg.pose_step_angle_deg));
        }
        bool any_stepping = g_feet[0].stepping || g_feet[1].stepping;
        if (!any_stepping)
        {
            // The foot that needs it most; after a step the other foot goes next if it needs it at all.
            int pick = need[0] >= need[1] ? 0 : 1;
            if (pick == g_last_foot && need[1 - pick] >= 0.5 && need[pick] < 2.0 * need[1 - pick]) pick = 1 - pick;
            if (need[pick] >= 1.0)
            {
                FootState& f = g_feet[pick];
                f.stepping = true;
                f.t = 0.0;
                f.from = f.at;
                f.yaw_from = f.yaw;
                // Quicker steps when moving faster.
                f.duration = cfg.pose_step_time_s / (1.0 + speed / 150.0);
                f.duration = (std::max)(0.12, f.duration);
                g_last_foot = pick;
                ++f.steps;
            }
        }
        for (int side = 0; side < 2; ++side)
        {
            FootState& f = g_feet[side];
            f.lift = 0.0;
            if (f.stepping)
            {
                f.t += dt;
                double u = (std::min)(1.0, f.t / f.duration);
                double s = u * u * (3.0 - 2.0 * u);
                // It aims for where the foot should land now, so it keeps up with a moving body.
                f.at = f.from + (want[side] - f.from) * s;
                f.at.z = 0.0;
                f.yaw = wrap_deg(f.yaw_from + wrap_deg(heading - f.yaw_from) * s);
                f.lift = std::sin(u * 3.14159265358979) * cfg.pose_step_lift_cm;
                if (u >= 1.0) f.stepping = false;
            }
            double floor_z = 0.0;
            if (!g_pose.ankle_measured[side])
            {
                // Posing can start while he is still dropping onto the floor after spawning: measure
                // the ankle once the game's own foot is down (the game keeps the feet until then).
                Vec3 foot_at = socket_location(g_pose.mesh, bone_fname(side == 0 ? "foot_l" : "foot_r"));
                if (ground_below(pawn, foot_at.x, foot_at.y, actor.z, floor_z) && foot_at.z - floor_z > 2.0 && foot_at.z - floor_z < 30.0)
                {
                    g_pose.ankle_height[side] = foot_at.z - floor_z;
                    g_pose.ankle_measured[side] = true;
                    HSVR_LOG("Pose: %s ankle %.1f cm above the floor (measured once he stood)", side == 0 ? "left" : "right", g_pose.ankle_height[side]);
                }
                else if (now - g_pose.calibrated_at > 3.0)
                {
                    g_pose.ankle_measured[side] = true;
                    HSVR_LOG("Pose: %s ankle could not be measured in 3 s; 10 cm used", side == 0 ? "left" : "right");
                }
                if (!g_pose.ankle_measured[side]) continue; // the game's own foot until then
            }
            if (ground_below(pawn, f.at.x, f.at.y, actor.z, floor_z))
            {
                f.ground_z = floor_z + g_pose.ankle_height[side];
                f.have_ground = true;
            }
            // The foot points where it was put down, flat (the leg IK takes its turn from the hips otherwise).
            if (ModifyBoneNode* n = chain_node(side == 0 ? "foot_l" : "foot_r"))
                node_set_rotation(*n, component_inv * (quat_from_yaw_deg(f.yaw) * g_pose.foot_rot_local[side]));
        }
        g_feet_active = true;
    }

    // Right after ABP_Willie's animation update (which copies the game's foot targets in): our feet.
    UFunction* g_anim_update = nullptr;
    void feet_after_anim_update(Hook::TCallbackIterationData<void>&, UObject* context, UFunction* function, void*)
    {
        using namespace hsvr;
        if (!g_feet_active.load() || !context || context != g_pose.instance || !function) return;
        if (g_anim_update) { if (function != g_anim_update) return; }
        else
        {
            if (function->GetName() != STR("BlueprintUpdateAnimation")) return;
            g_anim_update = function;
        }
        if (!g_effector_looked)
        {
            g_effector_looked = true;
            g_effector[0] = context->GetPropertyByNameInChain(STR("Left Foot Effector Location"));
            g_effector[1] = context->GetPropertyByNameInChain(STR("Right Foot Effector Location"));
            std::string seen;
            for (UStruct* level = static_cast<UStruct*>(context->GetClassPrivate()); level; level = level->GetSuperStruct())
                for (FProperty* p : level->ForEachProperty())
                {
                    std::wstring n = p->GetName();
                    if (n.find(STR("Effector")) == std::wstring::npos && n.find(STR("Foot")) == std::wstring::npos) continue;
                    if (!seen.empty()) seen += ", ";
                    for (wchar_t ch : n) seen += (char)(ch < 128 ? ch : '?');
                }
            HSVR_LOG("Feet: animation foot targets %s/%s; foot properties in the animation: %s", g_effector[0] ? "found" : "MISSING",
                     g_effector[1] ? "found" : "MISSING", seen.c_str());
        }
        UObject* mesh = g_pose.mesh;
        UObject* pawn = g_pose.pawn;
        if (!mesh || !pawn) return;
        static const FName pelvis = bone_fname("pelvis");
        for (int side = 0; side < 2; ++side)
        {
            FProperty* p = g_effector[side];
            FootState& f = g_feet[side];
            if (!p || !f.valid) continue;
            uint8_t* field = reinterpret_cast<uint8_t*>(context) + p->GetOffset_Internal();
            if (!f.have_ground) continue; // no floor found yet: the game's own target this frame
            Call to(mesh, STR("TransformToBoneSpace"));
            to.name(STR("BoneName"), pelvis);
            to.three(STR("InPosition"), f.at.x, f.at.y, f.ground_z + f.lift);
            if (!to.run()) continue;
            Vec3 out = to.vec(STR("OutPosition"));
            write_three(field, p, out.x, out.y, out.z);
        }
    }

    // ---- Torso twist ----------------------------------------------------------------------------
    // Your head turns first, then the neck and chest, and the hips last (they follow with the body
    // turning, outside its dead zone). The spine and neck bones share pose_torso_twist of how far
    // your head is turned from the hips, more toward the top, on top of the game's own animation.
    // The head itself is always exactly the headset; the arms are placed in the world, so they stay
    // on your hands whatever the chest does.
    void pose_torso_twist(const hsvr::Config& cfg, bool have_look, const hsvr::Quat& look_rot, double hips_yaw, const hsvr::Quat& component_inv)
    {
        using namespace hsvr;
        static const struct
        {
            const char* bone;
            double weight;
        } chain[] = {{"spine_02", 0.10}, {"spine_03", 0.12}, {"spine_04", 0.14}, {"spine_05", 0.16}, {"neck_01", 0.22}, {"neck_02", 0.26}};
        double rel = 0.0;
        if (have_look && cfg.pose_torso_twist > 0.0)
        {
            rel = quat_yaw_deg(look_rot) - hips_yaw;
            while (rel > 180.0) rel -= 360.0;
            while (rel < -180.0) rel += 360.0;
            rel = (std::max)(-90.0, (std::min)(90.0, rel));
        }
        Vec3 up = component_inv.rotate({0.0, 0.0, 1.0});
        double total = rel * (std::min)(1.0, cfg.pose_torso_twist);
        for (const auto& link : chain)
        {
            ModifyBoneNode* n = chain_node(link.bone);
            if (!n) continue;
            if (total == 0.0) node_release(*n);
            else node_add_rotation(*n, quat_axis_angle(up, total * link.weight * 3.14159265358979 / 180.0));
        }
        g_pose.twist_deg = total;
    }

    std::atomic<int> g_fake_arm_mode{0};
    std::atomic<double> g_fake_two_up{20.0};
    bool mouth_point(UObject* pawn, hsvr::Vec3& out); // hsvr.arm_move 7 <cm>: the left hand this far above the right
    std::atomic<bool> g_contact_log{true};

    bool hand_on_weapon(int side, const hsvr::Config& cfg, hsvr::Vec3& target, hsvr::Quat& hand_rot);
    bool two_hand_pose(int side, hsvr::Vec3& target, hsvr::Quat& hand_rot);
    std::atomic<bool> g_hold_reach_clamp{true};
    // Test counters since the last hsvr.mark (for hsvr.expect): stuck let-goes, kicks (a held weapon
    // ending a tick more than 8 cm from where it was sent) and the worst hold lag (cm) per hand.
    std::atomic<int> g_count_letgo{0}, g_count_kicks{0};
    std::atomic<double> g_worst_lag[2] = {0.0, 0.0};
    std::atomic<bool> g_fake_continue{false}; // test: hsvr.press_a 1 holds right A for menu_buttons
    std::atomic<float> g_fake_trigger[2] = {-1.0f, -1.0f}; // test: hsvr.trigger <0|1> <value>, -1 = the controller
    std::atomic<float> g_fake_squeeze[2] = {-1.0f, -1.0f}; // test: hsvr.squeeze <0|1> <value>, -1 = the controller

    // Sets a yes/no property that the engine packs into a single bit (tests only).
    bool set_bool_property(UObject* owner, const TCHAR* name, bool value)
    {
        if (!owner) return false;
        FProperty* property = owner->GetPropertyByNameInChain(name);
        if (!property || property->GetClass().GetName() != STR("BoolProperty")) return false;
        auto* flag = static_cast<FBoolProperty*>(property);
        uint8_t* byte = reinterpret_cast<uint8_t*>(owner) + property->GetOffset_Internal() + flag->GetByteOffset();
        if (value) *byte |= flag->GetFieldMask();
        else *byte &= (uint8_t)~flag->GetFieldMask();
        return true;
    }
    void ghost_hand(int side, const hsvr::Config& cfg, bool show, const hsvr::Vec3& wrist, const hsvr::Quat& hand_rot);
    double hand_on_weapon_amount(int side);
    UObject* load_asset_object(const std::wstring& object_path);

    // Measuring your arm length: arms straight out to the sides, B and Y held together for 2 s. Your
    // wrist-to-wrist span (from the controllers), less his shoulder width (yours is taken as his),
    // halved, is your arm from shoulder to wrist; it goes into the ini (pose_user_arm_cm) and both
    // controllers buzz. Holding B and Y with the arms not spread (under 100 cm) does nothing.
    void measure_arms(const hsvr::ControllerState& state)
    {
        using namespace hsvr;
        static double held_since = -1.0;
        static double done_at = -100.0;
        static bool measured = false;
        double now = now_seconds();
        if (measured && now - done_at < 0.4)
        {
            controllers_buzz(0, 0.8f);
            controllers_buzz(1, 0.8f);
        }
        bool both = state.hand[0].grip_valid && state.hand[1].grip_valid && state.hand[0].secondary && state.hand[1].secondary &&
                    g_pose.hand_valid[0] && g_pose.hand_valid[1];
        if (!both)
        {
            held_since = -1.0;
            return;
        }
        if (held_since < 0.0) held_since = now;
        if (now - held_since < 2.0 || now - done_at < 3.0) return;
        double span = length(g_pose.raw_target[0] - g_pose.raw_target[1]);
        Vec3 sl = socket_location(g_pose.driver, bone_fname("upperarm_l")), sr = socket_location(g_pose.driver, bone_fname("upperarm_r"));
        double shoulders = length(sl - sr);
        double arm = (span - shoulders) * 0.5;
        double his = (g_pose.arm[0].upper_length + g_pose.arm[0].lower_length + g_pose.arm[1].upper_length + g_pose.arm[1].lower_length) * 0.5;
        done_at = now;
        held_since = -1.0;
        measured = false;
        if (span < 100.0 || arm < 35.0 || arm > 110.0)
        {
            HSVR_LOG("Arms: not measured: wrist-to-wrist %.0f cm (his shoulders %.0f apart) gives an arm of %.0f cm, out of range", span, shoulders, arm);
            return;
        }
        char v[32];
        std::snprintf(v, sizeof(v), "%.1f", arm);
        config_set("pose_user_arm_cm", v);
        HSVR_LOG("Arms: measured: wrist-to-wrist %.0f cm, his shoulders %.0f apart, so your arm is %.1f cm (his %.1f): your reach is scaled by %.2f",
                 span, shoulders, arm, his, his / arm);
        measured = true;
    }

    // The shoulder reaches with you (Matthew's choice 2026-11-18, B&S-style): your real reach is
    // longer than your arm (the shoulder blade slides forward and the shoulder lifts when you reach,
    // ~88 cm controller-to-shoulder against a 56 cm arm in his log), so as the hand target nears the
    // end of his arm, his clavicle turns toward it and carries the shoulder up to pose_shoulder_reach_cm
    // closer. It eases in over the last pose_shoulder_reach_start_cm of his reach. Returns where the
    // shoulder (upperarm joint) will be this frame.
    hsvr::Vec3 shoulder_reach(int side, const hsvr::Config& cfg, const hsvr::Vec3& S, const hsvr::Vec3& target, const hsvr::Quat& component_inv)
    {
        using namespace hsvr;
        std::string sx = side_suffix(side);
        ModifyBoneNode* n = chain_node("clavicle" + sx);
        if (!n)
        {
            static bool told[2] = {false, false};
            if (!told[side])
            {
                told[side] = true;
                HSVR_LOG("Pose: %s clavicle has no bone node in his animation, so the shoulder can't reach forward", side == 0 ? "left" : "right");
            }
            return S;
        }
        const ArmCalibration& a = g_pose.arm[side];
        if (!a.clavicle_valid) return S;
        double reach = a.upper_length + a.lower_length;
        // From the clavicle as the chest carries it, not as posed last frame (its socket already
        // carries last frame's turn, which made the shoulder go only part of the way: run 34).
        Vec3 C = socket_location(g_pose.driver, bone_fname("clavicle" + sx));
        Quat Wc = (socket_rotation(g_pose.driver, bone_fname("spine_05")) * a.clavicle_in_chest).normalized();
        Vec3 arm = Wc.rotate(a.shoulder_in_clavicle);
        Vec3 S0 = C + arm;
        double c = length(arm);
        double D = length(target - S0);
        double from = reach - (std::max)(0.0, cfg.pose_shoulder_reach_start_cm), to = reach + (std::max)(0.0, cfg.pose_shoulder_reach_cm);
        double t = cfg.pose_shoulder_reach_cm > 0.0 && to > from ? (std::min)(1.0, (std::max)(0.0, (D - from) / (to - from))) : 0.0;
        t = t * t * (3.0 - 2.0 * t);
        if (t <= 0.0 || c < 1.0)
        {
            node_release(*n);
            return S;
        }
        // Turn the clavicle about its own root so the shoulder moves toward the target.
        Vec3 want = target - C;
        Vec3 axis = cross(arm, want);
        double al = length(axis);
        if (al < 1e-6)
        {
            node_release(*n);
            return S;
        }
        axis = axis * (1.0 / al);
        double angle = (std::min)(cfg.pose_shoulder_reach_cm * t / c, cfg.pose_shoulder_reach_max_deg * kDegToRad);
        Quat dq = quat_axis_angle(axis, angle);
        node_set_rotation(*n, component_inv * (dq * Wc).normalized());
        Vec3 moved = C + dq.rotate(arm);
        // Check: where we said the shoulder would be last frame against where it is now.
        static Vec3 said[2];
        static double last_log[2] = {0.0, 0.0};
        double now = now_seconds();
        if (now - last_log[side] >= 1.0)
        {
            last_log[side] = now;
            HSVR_LOG("Pose: %s shoulder reaches %.0f%%: %.1f cm forward of where the chest has it; last frame's prediction off by %.1f cm", side == 0 ? "left" : "right",
                     t * 100.0, length(moved - S0), length(said[side] - S));
        }
        said[side] = moved;
        return moved;
    }

    void pose_arm(int side, const hsvr::Config& cfg, const hsvr::ControllerState& state, const hsvr::Quat& body, const hsvr::Quat& component_inv)
    {
        using namespace hsvr;
        ArmCalibration& a = g_pose.arm[side];
        const HandState& hs = state.hand[side];
        if (!a.valid) return;
        std::string sx = side_suffix(side);
        double out = side == 0 ? -1.0 : 1.0;
        Vec3 grip;
        Quat grip_rot;
        bool tracked = !cfg.dev_pose_fake_tracking && hs.grip_valid && stereo_tracking_to_world(hs.grip, grip, grip_rot);
        if (!tracked)
        {
            if (!cfg.dev_pose_fake_tracking)
            {
                // No controller (playing with mouse and keyboard, or tracking lost): the game's own arms.
                for (const char* b : {"clavicle", "upperarm", "lowerarm", "hand"})
                    if (ModifyBoneNode* n = chain_node(std::string(b) + sx)) node_release(*n);
                g_pose.hand_valid[side] = false;
                ghost_hand(side, cfg, false, Vec3{}, Quat{});
                return;
            }
            // Made-up path for unattended tests (dev_script "hsvr.arm_move <mode>"): 0 each hand circles
            // slowly in front of its shoulder; 1 holds still; 2 slow swing and 3 fast swing (side to
            // side and down, a cut); 4 raised overhead; 5 pushed down through the floor; 6 reaching out
            // 54-70 cm in front of the shoulder (past his 53 cm arm).
            Vec3 shoulder = socket_location(g_pose.driver, bone_fname("upperarm" + sx));
            int mode = g_fake_arm_mode.load();
            double now = now_seconds();
            Vec3 local;
            if (mode == 1) local = Vec3{40.0, out * 15.0, -10.0};
            else if (mode == 2 || mode == 3)
            {
                double t = now * (mode == 2 ? 1.2 : 6.0) + side * 0.3;
                local = Vec3{45.0, out * 10.0 + 35.0 * std::sin(t), 5.0 - 30.0 * std::fabs(std::sin(t))};
            }
            else if (mode == 4) local = Vec3{15.0, out * 15.0, 45.0};
            else if (mode == 5) local = Vec3{45.0, out * 15.0, -175.0}; // under the floor: the weapon is stopped by the ground
            else if (mode == 6) local = Vec3{62.0 + 8.0 * std::sin(now * 1.0), out * 10.0, 5.0}; // reaching out past his arm: the shoulder reaches
            else if ((mode == 8 || mode == 9) && g_pose.pawn && mouth_point(g_pose.pawn, grip))
            {
                // At his mouth (eating and drinking tests): the hand a little in front of and below it.
                local = body.inverse().rotate(grip - shoulder) + Vec3{6.0, 0.0, -4.0};
            }
            else if (mode == 7)
            {
                // Two hands on one weapon: the right still (as mode 1); the left hsvr.arm_move 7 <cm>
                // from it along the weapon the right hand holds (+ toward the tip, - toward the pommel),
                // or straight up when it holds none.
                shoulder = socket_location(g_pose.driver, bone_fname("upperarm_r"));
                local = Vec3{40.0, 15.0, -10.0};
                if (side == 0)
                {
                    Vec3 along{0, 0, 1};
                    UObject* held = g_pose.pawn ? object_property(g_pose.pawn, STR("Weapon R")) : nullptr;
                    if (UObject* wb = held ? object_property(held, STR("BaseMesh")) : nullptr) along = body.inverse().rotate(component_rotation(wb).rotate(Vec3{0, 0, 1}));
                    local = local + along * g_fake_two_up.load();
                }
            }
            else
            {
                double t = now * 1.5 + side * 1.5;
                local = Vec3{35.0 + 10.0 * std::sin(t), out * 15.0, -15.0 + 20.0 * std::cos(t)};
            }
            grip = shoulder + body.rotate(local);
            grip_rot = (body * rot_to_quat(60.0, 0.0, 0.0)).normalized(); // controller handle tilted forward, as held
            // Mode 9: at the mouth, the hand turning over (a full turn about his right in 6 s), so a
            // held bottle tips its neck down at some point (drinking tests).
            if (mode == 9) grip_rot = (body * quat_axis_angle(Vec3{0, 1, 0}, std::fmod(now, 6.0) / 6.0 * 2.0 * 3.14159265358979) * rot_to_quat(60.0, 0.0, 0.0)).normalized();
        }
        Vec3 target = grip + grip_rot.rotate({cfg.pose_wrist_forward_cm, cfg.pose_wrist_right_cm * out, cfg.pose_wrist_up_cm});
        g_pose.raw_target[side] = target;
        Vec3 S = socket_location(g_pose.driver, bone_fname("upperarm" + sx));
        // Your reach scaled to his (once your arm length is measured): from his shoulder, your hand is
        // as far out along his arm as it is along yours.
        if (cfg.pose_user_arm_cm > 1.0 && tracked) target = S + (target - S) * ((a.upper_length + a.lower_length) / cfg.pose_user_arm_cm);
        g_pose.target[side] = target;
        S = shoulder_reach(side, cfg, S, target, component_inv);

        Vec3 to = target - S;
        double D = length(to);
        double L1 = a.upper_length, L2 = a.lower_length;
        double lo = std::fabs(L1 - L2) + 0.5, hi = L1 + L2 - 0.5;
        if (D < 1e-3) return;
        Vec3 dir = to * (1.0 / D);
        D = (std::min)((std::max)(D, lo), hi);
        // A held weapon goes where his hand can be: past his reach, as far as his arm goes (the test
        // switch hsvr.hold_reach 0 leaves the target unclamped, to push a weapon into the floor).
        if (g_hold_reach_clamp.load()) g_pose.target[side] = S + dir * D;
        // The hand in the controller's grip (OpenXR grip pose, here X forward through the fist from
        // the little finger to the thumb, Y right, Z up): the fingers run down the handle, tilted
        // pose_knuckle_deg toward the front; the thumb side is the front. The same for both hands
        // (each palm faces the other hand). Geometry as in Matthew's Chasms VR Framework (MIT).
        Quat offset = rot_to_quat(cfg.pose_hand_pitch, cfg.pose_hand_yaw * out, cfg.pose_hand_roll * out);
        Quat hand_grip = (grip_rot * offset).normalized();
        double k = cfg.pose_knuckle_deg * 3.14159265358979323846 / 180.0;
        Vec3 fingers_world = hand_grip.rotate(Vec3{std::sin(k), 0.0, -std::cos(k)});
        Vec3 thumb_world = hand_grip.rotate(Vec3{std::cos(k), 0.0, std::sin(k)});
        Quat Wh = (quat_from_two(fingers_world, thumb_world) * quat_from_two(a.fingers_local, a.thumb_local).inverse()).normalized();
        g_pose.hand_rot[side] = Wh;
        g_pose.hand_valid[side] = true;
        // Holding a weapon that something has stopped: his hand stays on the weapon (Matthew's framework).
        Vec3 controller_wrist = target;
        Quat controller_rot = Wh;
        bool on_weapon = hand_on_weapon(side, cfg, target, Wh);
        if (two_hand_pose(side, target, Wh)) on_weapon = true;
        // Where your real hand is, while his stays on a stopped weapon.
        ghost_hand(side, cfg, on_weapon && hand_on_weapon_amount(side) > 0.5 && length(controller_wrist - target) > cfg.pose_ghost_from_cm, controller_wrist,
                   controller_rot);
        if (on_weapon)
        {
            to = target - S;
            double d = length(to);
            if (d < 1e-3) return;
            dir = to * (1.0 / d);
            D = (std::min)((std::max)(d, lo), hi);
        }

        // Where the elbow points around the shoulder-to-wrist line (the swivel): ported from the
        // arm solver in Matthew's Chasms VR Framework (crates/ik/src/arm.rs, MIT, Copyright (c) 2026
        // chasmlol). Start from VRArmIK's tuned version of Parger's position model, follow the
        // hand's twist when the arm is nearly straight, then take the cheapest angle in a short
        // search that keeps the wrist and forearm twist in comfortable ranges, the elbow out of the
        // ribcage and close to last frame. The hand itself is never moved.
        const double PI = 3.14159265358979323846, RAD = PI / 180.0;
        Vec3 up{0, 0, 1}, forward = body.rotate({1, 0, 0}), outward = body.rotate({0, out, 0});
        auto reject = [](const Vec3& v, const Vec3& from) { return v - from * dot(v, from); };
        auto smoothstep = [](double e0, double e1, double x) {
            double t = (std::min)(1.0, (std::max)(0.0, (x - e0) / (e1 - e0)));
            return t * t * (3.0 - 2.0 * t);
        };
        auto wrap = [PI](double x) {
            while (x > PI) x -= 2.0 * PI;
            while (x < -PI) x += 2.0 * PI;
            return x;
        };
        // The rotation taking unit `from` onto unit `to`, applied to `v`.
        auto arc = [](const Vec3& from, const Vec3& to, const Vec3& v) {
            Vec3 c = cross(from, to);
            double d = dot(from, to);
            if (d < -0.999999) return v * -1.0;
            Quat q{c.x, c.y, c.z, 1.0 + d};
            return q.normalized().rotate(v);
        };
        auto signed_angle = [](Vec3 a2, Vec3 b2, Vec3 axis2) {
            axis2 = normalized(axis2);
            a2 = a2 - axis2 * dot(a2, axis2);
            b2 = b2 - axis2 * dot(b2, axis2);
            return std::atan2(dot(cross(a2, b2), axis2), dot(a2, b2));
        };
        // Body frame in the framework's chest space: x right, y up, z forward.
        Vec3 chest = socket_location(g_pose.driver, bone_fname("spine_05"));
        auto local = [&](const Vec3& v) { Vec3 l = body.inverse().rotate(v - chest); return Vec3{l.y, l.z, l.x}; };

        Vec3 axis_sw = dir;
        double lean = -dot(axis_sw, up) * 0.2;
        Vec3 u = reject(up, axis_sw) + reject(forward, axis_sw) * lean;
        if (length(u) < 1e-6) u = reject(forward, axis_sw);
        u = normalized(u);
        Vec3 o = reject(reject(outward, axis_sw) - reject(forward, axis_sw) * 0.3, u);
        if (length(o) < 1e-6)
        {
            o = cross(u, axis_sw);
            if (dot(o, outward) < 0) o = o * -1.0;
        }
        o = normalized(o);
        Vec3 swivel_axis = cross(u, o);
        auto swivel_dir = [&](double al) { return u * std::cos(al) + o * std::sin(al); };
        auto swivel_angle = [&](const Vec3& e) { return std::atan2(dot(e, o), dot(e, u)); };
        double along = (L1 * L1 - L2 * L2 + D * D) / (2.0 * D);
        double h = std::sqrt((std::max)(0.0, L1 * L1 - along * along));
        auto elbow_for = [&](const Vec3& e) { return S + dir * along + normalized(reject(e, dir)) * h; };
        Vec3 H = S + dir * D;

        // 1. The position model, from the wrist relative to the shoulder in arm lengths.
        Vec3 rel = local(target) - local(S);
        // The framework's lengths are metres for a reference body whose arm is 56.5 cm.
        double scale = (L1 + L2) / 56.5;
        Vec3 hn = rel * (1.0 / (L1 + L2));
        double alpha = 135.0 - 60.0 * hn.y;
        alpha += hn.y > 0.0 ? 260.0 * (std::max)(0.0, 0.6 - hn.z) * hn.y : -100.0 * (std::max)(0.0, 0.6 - hn.z) * -hn.y;
        alpha -= 50.0 * (std::max)(0.0, -hn.x * out + 0.1);
        {
            double lo2 = 13.0, hi2 = 175.0, knee = 10.0;
            auto soft = [knee](double over) { return knee * (1.0 - std::exp(-over / knee)); };
            if (alpha > hi2 - knee) alpha = hi2 - knee + soft(alpha - (hi2 - knee));
            else if (alpha < lo2 + knee) alpha = lo2 + knee - soft(lo2 + knee - alpha);
        }
        Vec3 prior_dir = swivel_dir(alpha * RAD);
        // 2. Parger's singularity correction near the shoulder's vertical axis and behind it.
        double radius_m = std::sqrt(rel.x * rel.x + rel.z * rel.z) / 100.0 / scale;
        double blend = (std::max)(1.0 - radius_m / 0.5, (std::min)(1.0, (std::max)(0.0, -rel.z / 100.0 / scale / 0.1)));
        if (blend > 0.0)
        {
            Vec3 fixed_local{0.133 * out, -0.443, -0.886}; // x right, y up, z forward
            Vec3 fixed = body.rotate(Vec3{fixed_local.z, fixed_local.x, fixed_local.y});
            fixed = reject(fixed, swivel_axis);
            if (length(fixed) > 1e-6)
            {
                Vec3 b = prior_dir + (normalized(fixed) - prior_dir) * (std::min)(1.0, blend);
                if (length(b) > 1e-6) prior_dir = normalized(b);
            }
        }
        double prior = swivel_angle(prior_dir);
        Vec3 hand_y = normalized(fingers_world), hand_z = normalized(thumb_world), hand_x = cross(hand_y, hand_z);
        // A nearly straight arm follows the hand's twist: the inside of the elbow toward the thumb.
        double dd = (std::min)(D, L1 + L2);
        double flexion = std::acos((std::max)(-1.0, (std::min)(1.0, (dd * dd - L1 * L1 - L2 * L2) / (2.0 * L1 * L2))));
        double straight = smoothstep(30.0 * RAD, 8.0 * RAD, flexion);
        {
            Vec3 f = normalized(H - elbow_for(swivel_dir(prior)));
            Vec3 thumb = arc(hand_y, f, hand_z);
            Vec3 r = reject(thumb * -1.0, swivel_axis);
            if (length(r) > 1e-6) prior += wrap(swivel_angle(r) - prior) * straight;
        }
        // 3. The cheapest swivel near the prior.
        Vec3 shoulder_l = local(S);
        auto excess = [RAD](double angle, double limit_deg) { return (std::max)(0.0, angle - limit_deg * RAD); };
        bool have_prev = g_pose.swivel_valid[side];
        double prev = have_prev ? swivel_angle(body.rotate(g_pose.swivel_local[side])) : 0.0;
        auto cost = [&](double al) {
            Vec3 e = swivel_dir(al);
            Vec3 Ee = elbow_for(e);
            Vec3 f = normalized(H - Ee);
            double alg = dot(f, hand_y);
            double flex = std::fabs(std::atan2(dot(f, hand_x), alg));
            double dev = std::atan2(dot(f, hand_z), alg);
            dev = dev > 0.0 ? excess(dev, 30.0) : excess(-dev, 15.0);
            Vec3 thumb = arc(hand_y, f, hand_z);
            double tw = std::fabs(signed_angle(e * -1.0, thumb, f));
            double c = 1.0 - std::cos(al - prior) + 8.0 * (std::pow(excess(flex, 50.0), 2) + dev * dev) + 4.0 * std::pow(excess(tw, 70.0), 2);
            if (have_prev) c += 0.3 * (1.0 - std::cos(al - prev));
            Vec3 el = local(Ee);
            if (std::fabs(el.x) < 14.0 * scale && el.z > -12.0 * scale && el.z < 11.0 * scale && el.y > -55.0 * scale && el.y < shoulder_l.y) c += 20.0;
            double tucked = (std::min)(shoulder_l.x * out, local(H).x * out) - el.x * out;
            c += 2.0 * std::pow((std::max)(0.0, tucked) / (10.0 * scale), 2);
            return c;
        };
        double lo_a = 30.0 * RAD, hi_a = 215.0 * RAD, step = 10.0 * RAD;
        double best = lo_a, best_cost = 1e30;
        for (int i = 0; i <= 18; ++i)
        {
            double al = lo_a + step * i, c = cost(al);
            if (c < best_cost) best = al, best_cost = c;
        }
        {
            double before = cost(best - step), after = cost(best + step), curve = before - 2.0 * best_cost + after;
            if (curve > 1e-6)
            {
                double refined = (std::min)(hi_a, (std::max)(lo_a, best + 0.5 * step * (before - after) / curve));
                if (cost(refined) < best_cost) best = refined;
            }
        }
        // Smooth the swivel (never the hand), in the body's frame.
        Vec3 want_local = body.inverse().rotate(swivel_dir(best));
        double dt = now_seconds() - g_pose.swivel_time[side];
        g_pose.swivel_time[side] = now_seconds();
        if (have_prev && dt > 0.0 && dt < 0.5)
        {
            double k2 = 1.0 - std::exp(-dt / (std::max)(0.001, cfg.pose_swivel_smooth_s));
            Vec3 m = g_pose.swivel_local[side] + (want_local - g_pose.swivel_local[side]) * k2;
            if (length(m) > 1e-6) want_local = normalized(m);
        }
        g_pose.swivel_local[side] = want_local;
        g_pose.swivel_valid[side] = true;
        Vec3 pole = body.rotate(want_local);
        if (length(reject(pole, dir)) < 1e-3) pole = swivel_dir(best);
        Vec3 E = elbow_for(pole);
        // The arm's roll is set from Willie's reference pose (T-pose, palms down, thumbs forward),
        // where upper arm, forearm and hand share their axes (Willie_Lopoly, HalfSwordDecomp
        // defaults/hand_bones.md): along the arm is the bone's own direction, and +Y (the thumb
        // side) is the way the forearm folds when the elbow bends. So the elbow is a pure hinge,
        // and a hand in that same frame (a handshake grip) has no twist. The hand's turn about the
        // forearm away from that is the twist; the forearm bone takes pose_forearm_twist_share of it,
        // since Willie's forearm has no twist bones, and the hand keeps its exact rotation.
        Vec3 fold_axis_local{0.0, 1.0, 0.0};
        Vec3 ud = normalized(E - S), fd = normalized(H - E);
        Vec3 fold = reject(fd, ud);
        if (length(fold) < 1e-3) fold = reject(pole, ud) * -1.0; // straight arm: away from the elbow's side
        fold = normalized(fold);
        Quat Wu = (quat_from_two(ud, fold) * quat_from_two(a.upper_dir_local, fold_axis_local).inverse()).normalized();
        Quat Wl = (quat_from_two(fd, arc(ud, fd, fold)) * quat_from_two(a.lower_dir_local, fold_axis_local).inverse()).normalized();
        Vec3 axis_local = normalized(a.lower_dir_local);
        Quat rel_hand = (Wl.inverse() * Wh).normalized();
        double proj = rel_hand.x * axis_local.x + rel_hand.y * axis_local.y + rel_hand.z * axis_local.z;
        double twist = wrap(2.0 * std::atan2(proj, rel_hand.w));
        Wl = (Wl * quat_axis_angle(axis_local, twist * cfg.pose_forearm_twist_share)).normalized();
        Vec3 axis = fd;
        a.twist_deg = twist * 180.0 / 3.14159265358979323846;
        double c = dot(normalized(Wh.rotate(a.fingers_local)), axis);
        a.bend_deg = std::acos((std::max)(-1.0, (std::min)(1.0, c))) * 180.0 / 3.14159265358979323846;
        node_set_rotation(*chain_node("upperarm" + sx), component_inv * Wu);
        node_set_rotation(*chain_node("lowerarm" + sx), component_inv * Wl);
        node_set_rotation(*chain_node("hand" + sx), component_inv * Wh);
    }

    void pose_body(const hsvr::Config& cfg)
    {
        using namespace hsvr;
        UObject* pawn = nullptr;
        UObject* mesh = find_pawn_mesh(&pawn);
        bool willie = pawn && pawn->GetFullName().find(STR("Willie_BP")) != std::wstring::npos;
        UObject* driver = willie ? object_property(pawn, STR("DriverSkeleton")) : nullptr;
        UObject* instance = object_property(driver, STR("AnimScriptInstance"));
        bool abp = instance && instance->GetClassPrivate()->GetName().find(STR("ABP_Willie")) != std::wstring::npos;
        g_hold_capsule.store(cfg.pose_body && cfg.pose_hold_capsule && abp && mesh && g_pose.calibrated && g_pose.pawn == pawn);
        g_pose_active.store(cfg.pose_body && abp && mesh && g_pose.calibrated && g_pose.pawn == pawn);
        g_pose_cfg_turn = cfg.pose_body_turn;
        g_pose_cfg_crouch = cfg.pose_crouch;
        g_pose_fake = cfg.dev_pose_fake_tracking;
        g_turn_dead_deg = cfg.pose_turn_dead_zone_deg;
        g_turn_speed_dps = cfg.pose_turn_speed_dps;
        g_turn_max_deg = (std::max)(g_turn_dead_deg, cfg.pose_turn_max_deg);
        if (!g_pose_active.load()) g_turn_valid = false;
        if (!cfg.pose_body || !abp || !mesh)
        {
            if (g_pose.pawn && (pawn == g_pose.pawn)) pose_release();
            else if (g_pose.pawn)
            {
                g_pose = PoseState{}; // character gone: nothing to restore
                feet_reset();
                g_pose_anchor_fixed = false;
                g_pose_room_moved_local = {};
            }
            return;
        }
        if (g_pose.pawn != pawn || g_pose.instance != instance)
        {
            g_pose = PoseState{};
            feet_reset();
            g_effector_looked = false;
            g_anim_update = nullptr;
            g_pose.pawn = pawn;
            g_pose.mesh = mesh;
            g_pose.driver = driver;
            g_pose.instance = instance;
            g_pose.chain = final_chain_nodes(instance);
            g_pose.calibrated = pose_calibrate(cfg);
        }
        // Keep physics off: the game turns it on again after a spawn and when it rescales the body.
        double now = now_seconds();
        if (now - g_pose.last_check >= 0.5)
        {
            g_pose.last_check = now;
            if (is_simulating(mesh, "pelvis") == 1)
            {
                HSVR_LOG_EVERY(5.0, "Pose: the game switched physics on again; switching it off");
                if (g_joint_trace.load()) HSVR_LOG("Joint: physics found on at the pelvis and switched off again (this tick)");
                apply_arm_physics(cfg);
            }
        }

        ControllerState state = controllers_state();
        double heading = actor_heading(pawn);
        Quat body = quat_from_yaw_deg(heading);
        Vec3 actor = actor_location(pawn);

        // Walking in the room moves the game's capsule (with collision), so the game, the camera
        // and the body agree on where you are; the view takes the same amount off its anchor.
        Vec3 room{0, 0, 0};
        Vec3 anchor;
        double base_yaw = 0.0;
        if (cfg.pose_follow_room && state.head_valid && stereo_view_anchor(anchor, base_yaw))
        {
            Vec3 head_world;
            Quat head_rot;
            if (stereo_tracking_to_world(state.head, head_world, head_rot))
            {
                // Walking is measured at your neck, not your eyes: your eyes swing round your neck when
                // you turn or nod, and that is not a step (a 180 degree turn moved the eyes ~18 cm and
                // walked his body out in front of you). The view's anchor is the eye point when you
                // recentred, so the neck then was eye_forward/eye_up behind and below it.
                Quat base = quat_from_yaw_deg(base_yaw);
                Vec3 eye_offset{cfg.eye_forward_cm, 0.0, cfg.eye_up_cm};
                Vec3 neck_now = head_world - head_rot.rotate(eye_offset);
                Vec3 neck_then = anchor - base.rotate(eye_offset);
                Vec3 total = base.inverse().rotate(Vec3{neck_now.x - neck_then.x, neck_now.y - neck_then.y, 0.0});
                total.z = 0.0;
                Vec3 delta = total - g_pose_room_moved_local;
                if (length(delta) > 50.0)
                {
                    g_pose_room_moved_local = total; // recentred or tracking jumped: take it as the new start
                }
                else if (length(delta) > 0.3)
                {
                    Vec3 want = actor + base.rotate(delta);
                    Call move(pawn, STR("K2_SetActorLocation"));
                    move.three(STR("NewLocation"), want.x, want.y, want.z);
                    move.flag(STR("bSweep"), true);
                    move.flag(STR("bTeleport"), false);
                    move.run();
                    Vec3 moved = actor_location(pawn) - actor;
                    moved.z = 0.0;
                    g_pose_room_moved_local = g_pose_room_moved_local + base.inverse().rotate(moved);
                    g_pose_room_moved_local.z = 0.0;
                    actor = actor + moved;
                    room = moved;
                }
            }
        }
        // Put the body so its pelvis stands on the capsule. The game pulls the capsule toward the
        // pelvis; a pelvis anywhere else would drag the capsule along and the body with it.
        Vec3 mesh_now = component_location(mesh);
        Vec3 pelvis_off = socket_location(driver, bone_fname("pelvis")) - mesh_now;
        Vec3 place = actor + body.rotate(g_pose.mesh_offset_local);
        place.x = actor.x - pelvis_off.x;
        place.y = actor.y - pelvis_off.y;
        // Crouching: the body goes down by as much as your headset is below where you stood when
        // you recentred. His leg IK keeps the feet planted, so the knees bend. Never raised above
        // standing (the leg IK only stretches 1.1x), and at most 55 cm (times his size) down.
        g_crouch_drop = 0.0;
        if (cfg.pose_crouch && cfg.dev_pose_fake_tracking)
        {
            // Made-up path: slowly down to 40 cm and back up.
            g_crouch_drop = 20.0 - 20.0 * std::cos(now_seconds() * 0.4);
        }
        else if (cfg.pose_crouch && state.head_valid)
        {
            Vec3 view_anchor, head_world;
            Quat head_rot;
            double unused_yaw = 0.0;
            if (stereo_view_anchor(view_anchor, unused_yaw) && stereo_tracking_to_world(state.head, head_world, head_rot))
            {
                double scale = component_scale_z(mesh);
                // Measured at the neck too, so looking down is not crouching.
                Vec3 eye_offset{cfg.eye_forward_cm, 0.0, cfg.eye_up_cm};
                double neck_now_z = (head_world - head_rot.rotate(eye_offset)).z;
                double neck_then_z = view_anchor.z - cfg.eye_up_cm;
                double drop = (neck_then_z - neck_now_z) - cfg.pose_crouch_dead_zone_cm;
                g_crouch_drop = (std::min)(55.0 * scale, (std::max)(0.0, drop));
            }
        }
        place.z -= g_crouch_drop;
        // Moved, not teleported: a teleport snaps his kinematic physics bodies (the hand holding a
        // weapon) to the new spot with no velocity in the first physics substep, and the held weapon
        // jolts every frame (headset tests 2-3: even the game's locked grip wobbled ~14 deg). A plain
        // move gives the bodies kinematic targets that the substeps interpolate (UE 5.4
        // SkeletalMeshComponentPhysics OnUpdateTransform -> UpdateKinematicBonesToAnim). Large jumps
        // (first placement, recentre) still teleport.
        {
            static Vec3 last_place;
            static bool have_last = false;
            bool jump = !have_last || length(place - last_place) > cfg.pose_teleport_above_cm;
            set_component_transform(mesh, place, (body * g_pose.mesh_rotation_local).normalized(), jump || !cfg.pose_move_bodies);
            last_place = place;
            have_last = true;
        }

        Quat component_inv = component_rotation(driver).inverse();
        bool have_look = false;
        Quat look_rot;
        if (cfg.pose_head)
        {
            Vec3 head_world;
            Quat head_rot;
            bool have = !cfg.dev_pose_fake_tracking && state.head_valid && stereo_tracking_to_world(state.head, head_world, head_rot);
            if (!have && cfg.dev_pose_fake_tracking)
            {
                // Made-up path: looks left and right, and up and down.
                double t = now_seconds();
                head_rot = (body * rot_to_quat(20.0 * std::sin(t * 0.7), 40.0 * std::sin(t * 0.5), 0.0)).normalized();
                if (g_turn_valid && cfg.pose_body_turn) // the same made-up look the body turning follows
                    head_rot = (quat_from_yaw_deg(g_turn_yaw + 80.0 * std::sin(t * 0.25)) * rot_to_quat(20.0 * std::sin(t * 0.7), 0.0, 0.0)).normalized();
                have = true;
            }
            if (ModifyBoneNode* n = chain_node("head"))
            {
                if (have) node_set_rotation(*n, component_inv * (head_rot * g_pose.head_offset));
                else node_release(*n);
            }
            have_look = have;
            look_rot = head_rot;
        }
        else if (ModifyBoneNode* n = chain_node("head"))
        {
            node_release(*n);
        }
        pose_torso_twist(cfg, have_look, look_rot, heading, component_inv);
        pose_feet(cfg, pawn, actor, heading, component_inv);
        for (int side = 0; side < 2; ++side)
        {
            if (cfg.pose_arms)
            {
                pose_arm(side, cfg, state, body, component_inv);
                if (side == 1) measure_arms(state);
            }
            else
                for (const char* b : {"clavicle", "upperarm", "lowerarm", "hand"})
                    if (ModifyBoneNode* n = chain_node(std::string(b) + side_suffix(side))) node_release(*n);
        }
        static double last_line = 0.0;
        if (now - last_line >= 0.25)
        {
            last_line = now;
            Vec3 pelvis = socket_location(driver, bone_fname("pelvis"));
            Vec3 hl = socket_location(driver, bone_fname("hand_l")), hr = socket_location(driver, bone_fname("hand_r"));
            UObject* movement = object_property(pawn, STR("CharacterMovement"));
            double v[3] = {0, 0, 0};
            int mode = -1;
            if (movement)
            {
                if (FProperty* vp = movement->GetPropertyByNameInChain(STR("Velocity")))
                    read_three(reinterpret_cast<uint8_t*>(movement) + vp->GetOffset_Internal(), vp, v);
                if (FProperty* mp = movement->GetPropertyByNameInChain(STR("MovementMode")))
                    mode = *(reinterpret_cast<uint8_t*>(movement) + mp->GetOffset_Internal());
            }
            // What his body really does on screen: chest and hips turned from where they were at calibration.
            double chest_yaw = quat_yaw_deg((socket_rotation(mesh, bone_fname("spine_05")) * g_pose.chest_rot_local.inverse()).normalized());
            double hips_yaw = quat_yaw_deg((socket_rotation(mesh, bone_fname("pelvis")) * g_pose.pelvis_rot_local.inverse()).normalized());
            double look_yaw = have_look ? quat_yaw_deg(look_rot) : 0.0;
            HSVR_LOG("Pose: capsule velocity (%.0f, %.0f, %.0f) mode %d, torso twist %.0f deg asked; on screen: head %.0f, chest %.0f, hips %.0f (chest %+.0f from hips)",
                     v[0], v[1], v[2], mode, g_pose.twist_deg, look_yaw, chest_yaw, hips_yaw, wrap_deg(chest_yaw - hips_yaw));
            if (g_feet_active.load())
            {
                Vec3 fl = socket_location(mesh, bone_fname("foot_l")), fr = socket_location(mesh, bone_fname("foot_r"));
                HSVR_LOG("Feet: left at (%.0f, %.0f) foot bone (%.0f, %.0f, %.0f) ground %.0f%s, %d steps; right at (%.0f, %.0f) foot bone (%.0f, %.0f, %.0f) ground %.0f%s, %d steps",
                         g_feet[0].at.x, g_feet[0].at.y, fl.x, fl.y, fl.z, g_feet[0].ground_z, g_feet[0].stepping ? " stepping" : "", g_feet[0].steps,
                         g_feet[1].at.x, g_feet[1].at.y, fr.x, fr.y, fr.z, g_feet[1].ground_z, g_feet[1].stepping ? " stepping" : "", g_feet[1].steps);
            }
            HSVR_LOG("Pose: wrists: left twist %.0f deg, bend %.0f deg; right twist %.0f deg, bend %.0f deg (twist before the forearm's share)",
                     g_pose.arm[0].twist_deg, g_pose.arm[0].bend_deg, g_pose.arm[1].twist_deg, g_pose.arm[1].bend_deg);
            HSVR_LOG("Pose: actor (%.0f, %.0f, %.0f) heading %.0f, pelvis gap %.0f cm, room moved (%.0f, %.0f) this frame (%.1f, %.1f), head %s, hands %s/%s, "
                     "hand_l (%.0f, %.0f, %.0f) target (%.0f, %.0f, %.0f), hand_r (%.0f, %.0f, %.0f) target (%.0f, %.0f, %.0f)",
                     actor.x, actor.y, actor.z, heading, length(Vec3{pelvis.x - actor.x, pelvis.y - actor.y, 0.0}), g_pose_room_moved_local.x,
                     g_pose_room_moved_local.y, room.x, room.y, state.head_valid ? "tracked" : "not tracked",
                     state.hand[0].grip_valid ? "tracked" : (cfg.dev_pose_fake_tracking ? "fake" : "-"),
                     state.hand[1].grip_valid ? "tracked" : (cfg.dev_pose_fake_tracking ? "fake" : "-"), hl.x, hl.y, hl.z, g_pose.target[0].x,
                     g_pose.target[0].y, g_pose.target[0].z, hr.x, hr.y, hr.z, g_pose.target[1].x, g_pose.target[1].y, g_pose.target[1].z);
        }
    }

    // ---- The capsule stays where you are --------------------------------------------------------
    // Willie's tick (ReceiveTick) pulls his capsule toward a point worked out from his pelvis and
    // spine plus the game's stumble, pain, dodge and lean offsets, and sets its height from his
    // steps. With the body posed on the capsule that pull never closes, so a hit (pain stumble)
    // slid him across the arena. While the body is posed, the mod notes where the capsule is just
    // before his tick and puts it back just after, so only your walking moves it. The stumble and
    // dodge offsets are cleared after each tick so they don't build up.
    UFunction* g_receive_tick = nullptr;
    bool g_hold_have = false;
    hsvr::Vec3 g_hold_location;

    double number_property(UObject* owner, FProperty* p)
    {
        uint8_t* at = reinterpret_cast<uint8_t*>(owner) + p->GetOffset_Internal();
        auto kind = p->GetClass().GetName();
        if (kind == STR("DoubleProperty")) return *reinterpret_cast<double*>(at);
        if (kind == STR("FloatProperty")) return *reinterpret_cast<float*>(at);
        if (kind == STR("BoolProperty"))
        {
            auto* flag = static_cast<FBoolProperty*>(p);
            return (at[flag->GetByteOffset()] & flag->GetFieldMask()) != 0 ? 1.0 : 0.0;
        }
        if (kind == STR("StructProperty"))
        {
            double v[3];
            read_three(at, p, v);
            return hsvr::length(hsvr::Vec3{v[0], v[1], v[2]});
        }
        return -1.0;
    }

    bool is_receive_tick(UObject* context, UFunction* function)
    {
        if (!(g_hold_capsule.load() || g_pose_active.load()) || !context || context != g_pose.pawn || !function) return false;
        if (g_receive_tick) return function == g_receive_tick;
        if (function->GetName() != STR("ReceiveTick")) return false;
        g_receive_tick = function;
        HSVR_LOG("Capsule: found your character's tick (%ls)", function->GetFullName().c_str());
        return true;
    }

    // ---- Your body faces where you face --------------------------------------------------------
    // The game turns Willie's capsule toward the control rotation's yaw (and nothing else turns
    // him); turning that way is not a swing or aim input (the aim reads only the mouse and pitch).
    // So just before his tick the mod sets the control yaw to the body's yaw: your mouse or stick
    // turn plus your real facing, which follows the headset once you look more than
    // pose_turn_dead_zone_deg away, at pose_turn_speed_dps, until it is lined up again. The view's
    // straight ahead keeps only the mouse or stick turn, so turning your head never turns the view
    // twice. Pitch is left as the game has it, so mouse up and down still aims.
    // ---- The left stick walks you ---------------------------------------------------------------
    // The game walks him through Unreal's character movement: its own stick and WASD handlers call
    // AddMovementInput and set "Movement Input Vector", whose length scales his walk speed (decompile:
    // Willie_BP @768801, @768447, @760861, @563757). Just before his tick the mod does the same with
    // the left stick, pointed where you look (pose_move_relative=head) or where your body faces
    // (=body). The capsule hold keeps what the character movement did (it runs before his tick).
    bool g_stick_walking = false;
    double g_stick_walk_until = 0.0;

    void stick_walk_before_tick(UObject* pawn)
    {
        using namespace hsvr;
        Config cfg = config();
        ControllerState state = controllers_state();
        double x = 0.0, y = 0.0;
        if (cfg.pose_stick_walk && state.active && state.focused)
        {
            x = state.hand[0].stick_x;
            y = state.hand[0].stick_y;
        }
        double m = std::sqrt(x * x + y * y);
        double dz = cfg.pose_stick_dead_zone;
        g_stick_walking = m > dz;
        if (!g_stick_walking) return;
        double scale = (std::min)(1.0, (m - dz) / (1.0 - dz));
        double yaw = g_turn_valid ? wrap_deg(g_turn_yaw + g_body_rel) : actor_heading(pawn);
        if (cfg.pose_move_relative == "head")
        {
            Vec3 head_world;
            Quat head_rot;
            if (state.head_valid && stereo_tracking_to_world(state.head, head_world, head_rot)) yaw = quat_yaw_deg(head_rot);
        }
        // Stick up = forward (+X), right = right (+Y).
        Vec3 dir = quat_from_yaw_deg(yaw).rotate(Vec3{y / m, x / m, 0.0});
        Call add(pawn, STR("AddMovementInput"));
        add.three(STR("WorldDirection"), dir.x, dir.y, 0.0);
        add.number(STR("ScaleValue"), scale);
        add.run();
        if (FProperty* p = pawn->GetPropertyByNameInChain(STR("Movement Input Vector")))
            write_three(reinterpret_cast<uint8_t*>(pawn) + p->GetOffset_Internal(), p, dir.x * scale, dir.y * scale, 0.0);
        g_stick_walk_until = now_seconds() + 0.5;
        static double last = 0.0;
        double now = now_seconds();
        if (now - last >= 1.0)
        {
            last = now;
            double v[3] = {0, 0, 0};
            if (UObject* movement = object_property(pawn, STR("CharacterMovement")))
                if (FProperty* vp = movement->GetPropertyByNameInChain(STR("Velocity")))
                    read_three(reinterpret_cast<uint8_t*>(movement) + vp->GetOffset_Internal(), vp, v);
            HSVR_LOG("Walk: stick %.2f toward %.0f deg, speed %.0f cm/s", scale, yaw, std::sqrt(v[0] * v[0] + v[1] * v[1]));
        }
    }

    void turn_body_before_tick()
    {
        using namespace hsvr;
        UObject* controller = find_player_controller();
        ue::FRotator* rotation = controller ? controller->GetValuePtrByPropertyNameInChain<ue::FRotator>(STR("ControlRotation")) : nullptr;
        if (!rotation) return;
        auto wrap = [](double a) {
            while (a > 180.0) a -= 360.0;
            while (a < -180.0) a += 360.0;
            return a;
        };
        double now = now_seconds();
        if (!g_pose_cfg_turn)
        {
            if (g_turn_valid) HSVR_LOG("Turn: body turning off; the mouse turns the body again");
            g_turn_valid = false;
            return;
        }
        if (!g_turn_valid)
        {
            g_turn_yaw = rotation->Yaw;
            g_body_rel = 0.0;
            g_last_written_yaw = rotation->Yaw;
            g_turn_time = now;
            g_body_turning = false;
            g_turn_valid = true;
            HSVR_LOG("Turn: your body now faces where you face (start %.0f)", g_turn_yaw);
        }
        // Whatever turned the control yaw since we last wrote it (mouse, stick) is your turn.
        g_turn_yaw = wrap(g_turn_yaw + wrap(rotation->Yaw - g_last_written_yaw));
        double dt = (std::min)(0.1, (std::max)(0.0, now - g_turn_time));
        g_turn_time = now;
        ControllerState state = controllers_state();
        // The right stick turns you (view and body together), smoothly or in snaps.
        {
            Config cfg = config();
            double x = state.active && state.focused ? state.hand[1].stick_x : 0.0;
            static bool snap_armed = true;
            if (cfg.pose_stick_turn == "smooth")
            {
                double dz = cfg.pose_stick_dead_zone;
                if (std::fabs(x) > dz)
                {
                    double v = (std::fabs(x) - dz) / (1.0 - dz);
                    g_turn_yaw = wrap(g_turn_yaw + (x > 0 ? v : -v) * cfg.pose_stick_turn_dps * dt);
                }
            }
            else if (cfg.pose_stick_turn == "snap")
            {
                if (snap_armed && std::fabs(x) > 0.6)
                {
                    g_turn_yaw = wrap(g_turn_yaw + (x > 0 ? cfg.pose_snap_turn_deg : -cfg.pose_snap_turn_deg));
                    snap_armed = false;
                }
                else if (std::fabs(x) < 0.3)
                    snap_armed = true;
            }
        }
        Vec3 head_world;
        Quat head_rot;
        bool have_head = !g_pose_fake && state.head_valid && stereo_tracking_to_world(state.head, head_world, head_rot);
        double head_rel = 0.0;
        if (have_head) head_rel = wrap(quat_yaw_deg(head_rot) - g_turn_yaw);
        else if (g_pose_fake)
        {
            // Made-up path: looks up to 80 degrees left and right, slowly.
            head_rel = 80.0 * std::sin(now * 0.25);
            have_head = true;
        }
        if (have_head)
        {
            double diff = wrap(head_rel - g_body_rel);
            if (std::fabs(diff) > g_turn_dead_deg) g_body_turning = true;
            if (g_body_turning)
            {
                double step = (std::min)(std::fabs(diff), g_turn_speed_dps * dt);
                g_body_rel = wrap(g_body_rel + (diff > 0 ? step : -step));
                if (std::fabs(diff) - step < 5.0) g_body_turning = false;
            }
            // A neck turns only so far: on a quick spin the hips are dragged round with you at once
            // rather than trailing, so his shoulders stay under your eyes.
            double left = wrap(head_rel - g_body_rel);
            if (std::fabs(left) > g_turn_max_deg) g_body_rel = wrap(head_rel - (left > 0 ? g_turn_max_deg : -g_turn_max_deg));
        }
        double body_yaw = wrap(g_turn_yaw + g_body_rel);
        rotation->Yaw = body_yaw;
        rotation->Roll = 0.0;
        g_last_written_yaw = body_yaw;
        static double last = 0.0;
        if (now - last >= 1.0)
        {
            last = now;
            HSVR_LOG("Turn: view straight ahead %.0f, body %.0f (%+.0f from it)%s, crouch %.0f cm", g_turn_yaw, body_yaw, g_body_rel,
                     g_body_turning ? ", turning" : "", g_crouch_drop);
        }
    }

    // ---- Crouching: the game's own crouch values follow yours ----------------------------------
    // The game rewrites "Crouched Height" and "Root Height" every tick (85 standing, 25 a standing
    // crouch, times his size). After his tick the mod sets them from how far you are down, so his
    // torso bend, stamina, step size and leg logic match. They are kept at 61 or more, because 60
    // or less while standing still starts the game's canned kneel (legs without IK).
    void set_named_number(UObject* owner, const TCHAR* name, double value)
    {
        FProperty* p = owner ? owner->GetPropertyByNameInChain(name) : nullptr;
        if (!p) return;
        uint8_t* at = reinterpret_cast<uint8_t*>(owner) + p->GetOffset_Internal();
        auto kind = p->GetClass().GetName();
        if (kind == STR("DoubleProperty")) *reinterpret_cast<double*>(at) = value;
        else if (kind == STR("FloatProperty")) *reinterpret_cast<float*>(at) = (float)value;
    }

    void crouch_after_tick(UObject* pawn)
    {
        if (!g_pose_cfg_crouch || !g_pose.mesh) return;
        double scale = component_scale_z(g_pose.mesh);
        double height = (std::min)(85.0 * scale, (std::max)(61.0, 85.0 * scale - g_crouch_drop));
        set_named_number(pawn, STR("Crouched Height"), height);
        set_named_number(pawn, STR("Root Height"), height);
    }

    void log_joint_before_tick(UObject* pawn);

    void block_offhand_search_before_tick(UObject* pawn);
    // Test: hsvr.pawn_pin <variable> <value> holds a number variable of his pawn at a value every tick,
    // after his tick and before the hold reads it (e.g. Stamina 10, Arm R Tonus 0.25); "off" clears all.
    std::vector<std::pair<std::wstring, double>> g_pawn_pins;
    bool set_variable(UObject* owner, const TCHAR* name, double number, UObject* object, const FName* fname);

    void capsule_before_tick(Hook::TCallbackIterationData<void>&, UObject* context, UFunction* function, void*)
    {
        if (!is_receive_tick(context, function)) return;
        g_hold_location = actor_location(context);
        g_hold_have = true;
        if (g_pose_active.load())
        {
            log_joint_before_tick(context);
            block_offhand_search_before_tick(context);
            turn_body_before_tick();
            stick_walk_before_tick(context);
        }
    }

    // ---- The weapon in your hand: a strength-limited spring --------------------------------------
    // The game holds a weapon with a physics joint between the weapon and the hand bone
    // ("PhysicsConstraint R/L Hand"), locked once the weapon is seated, and re-shapes it in every
    // tick (decompile: Willie_BP @454086-455803). With your hand posed from the controller, a locked
    // joint makes the weapon weightless. Right after his tick the mod turns the seated joint into a
    // spring toward the hand with a force limit (pose_grip_strength_kg), so a heavy weapon lags and
    // sags, and the game's own grip slip and stamina (which read this joint's force) keep working.
    // Its stiffness follows the weapon's mass, so every weapon follows equally well while within
    // your strength. The controller buzzes with the gap.
    // Test overrides from the dev script: -1 = use the ini. g_grip_spring_override 0 leaves the game's
    // own joint alone (still measured); g_grip_k_scale multiplies the spring's stiffness, damping and
    // force cap; g_weapon_gravity 0 turns gravity off on the held weapon.
    std::atomic<int> g_grip_spring_override{-1};
    std::atomic<double> g_grip_k_scale{1.0};
    std::atomic<int> g_weapon_gravity{-1};
    // -1 = leave alone; otherwise the ECollisionEnabled value to give the held weapon's BaseMesh
    // (0 none, 1 query only, 3 query and physics).
    std::atomic<int> g_weapon_collision{-1};
    // Set by hsvr.grip_rejoin: re-create the hand joint once (SetConstrainedComponents) after the
    // spring settings, in case the physics side never took them.
    std::atomic<bool> g_grip_rejoin{false};
    // hsvr.grip_frame_offset x y z: moves the joint's hand-side anchor (Frame2) by this much from
    // where the game put it, in the hand's frame. A working joint must carry the weapon along with
    // it; a dead one leaves the weapon in the fist. Changed values are written once per change.
    std::atomic<double> g_grip_frame_offset[3] = {0.0, 0.0, 0.0};
    std::atomic<int> g_grip_frame_serial{0};
    // Linear drive target for the test (the joint's own frame, cm); zero is the normal spring.
    std::atomic<double> g_grip_target[3] = {0.0, 0.0, 0.0};

    struct GripLog
    {
        double gap = 0.0, mass = 0.0, force = 0.0;
        bool active = false;
        double hand_dist = 0.0; // weapon body to the hand bone, measured directly
        hsvr::Vec3 weapon_at, hand_at;
        UObject* weapon_seen = nullptr;
        hsvr::Vec3 rest; // weapon relative to the hand bone when it was seated (hand frame)
        // The joint's own stretch: its weapon-side anchor (BaseMesh origin + Pos1) minus its hand-side
        // anchor (hand bone + Pos2, in the bone's frame). Zero when the spring holds perfectly.
        hsvr::Vec3 stretch_vec, stretch_hand;
        int broken = -1;
        double mesh_hand_diff = -1.0;
        hsvr::Vec3 hand_com_local, weapon_com_local;
        double turn_err_deg = 0.0;
        hsvr::Vec3 hand_com_world;
        int same_mesh = -1;
        double stretch = 0.0;
        hsvr::Vec3 pos2;
        int visible = -1;
        std::wstring joined;
        int simulating = -1;
        double scale = 0.0;
    };
    GripLog g_grip_log[2];

    // The game's own weapon grip: every direction locked, no drives (as the joint starts out).
    void lock_joint(UObject* joint)
    {
        for (const TCHAR* fn : {STR("SetLinearXLimit"), STR("SetLinearYLimit"), STR("SetLinearZLimit")})
        {
            Call c1(joint, fn);
            c1.byte(STR("ConstraintType"), 2); // locked
            c1.number(STR("LimitSize"), 0.0);
            c1.run();
        }
        for (const TCHAR* fn : {STR("SetAngularSwing1Limit"), STR("SetAngularSwing2Limit"), STR("SetAngularTwistLimit")})
        {
            Call c1(joint, fn);
            c1.byte(STR("MotionType"), 2); // locked
            c1.run();
        }
        Call pos(joint, STR("SetLinearPositionDrive"));
        pos.run(); // all three off
        Call vel(joint, STR("SetLinearVelocityDrive"));
        vel.run();
        Call slerp(joint, STR("SetOrientationDriveSLERP"));
        slerp.first_flag(false);
        slerp.run();
        Call aslerp(joint, STR("SetAngularVelocityDriveSLERP"));
        aslerp.first_flag(false);
        aslerp.run();
    }

    void spring_joint(UObject* joint, double mass, const hsvr::Config& cfg)
    {
        const double pi = 3.14159265358979;
        double w = 2.0 * pi * cfg.pose_grip_hz;
        double scale = g_grip_k_scale.load();
        // Chaos always runs constraint drives in acceleration mode (PhysInterface_Chaos.cpp,
        // UpdateLinearDrive_AssumesLocked: SetLinearDriveForceMode(Acceleration)), so the mass
        // cancels: stiffness is w^2 (1/s^2), damping 2*zeta*w (1/s), and MaxForce is a maximum
        // ACCELERATION (the solver divides it by the effective inverse mass). Strength in kg means:
        // the hand can hold strength_kg against gravity, so the cap is strength_kg*g/mass.
        double k = scale * w * w;
        double c = scale * 2.0 * cfg.pose_grip_damping * w;
        double max_force = scale * cfg.pose_grip_strength_kg * 980.0 / (std::max)(0.05, mass); // cm/s^2
        double wa = w * std::sqrt((std::max)(0.0, cfg.pose_grip_turn_scale));
        double ka = scale * wa * wa, ca = scale * 2.0 * cfg.pose_grip_turn_damping * wa;
        double max_torque = cfg.pose_grip_turn_max_scale * max_force / (std::max)(1.0, cfg.pose_grip_radius_cm); // rad/s^2 for that pull at the radius
        for (const TCHAR* fn : {STR("SetLinearXLimit"), STR("SetLinearYLimit"), STR("SetLinearZLimit")})
        {
            Call c1(joint, fn);
            c1.byte(STR("ConstraintType"), 0); // free
            c1.number(STR("LimitSize"), 0.0);
            c1.run();
        }
        for (const TCHAR* fn : {STR("SetAngularSwing1Limit"), STR("SetAngularSwing2Limit"), STR("SetAngularTwistLimit")})
        {
            Call c1(joint, fn);
            c1.byte(STR("MotionType"), 0); // free
            c1.run();
        }
        Call pos(joint, STR("SetLinearPositionDrive"));
        pos.flag(STR("bEnableDriveX"), true);
        pos.flag(STR("bEnableDriveY"), true);
        pos.flag(STR("bEnableDriveZ"), true);
        pos.run();
        Call vel(joint, STR("SetLinearVelocityDrive"));
        vel.flag(STR("bEnableDriveX"), true);
        vel.flag(STR("bEnableDriveY"), true);
        vel.flag(STR("bEnableDriveZ"), true);
        vel.run();
        Call target(joint, STR("SetLinearPositionTarget"));
        target.three(STR("InPosTarget"), g_grip_target[0].load(), g_grip_target[1].load(), g_grip_target[2].load());
        target.run();
        Call vtarget(joint, STR("SetLinearVelocityTarget"));
        vtarget.three(STR("InVelTarget"), 0.0, 0.0, 0.0);
        vtarget.run();
        Call lin(joint, STR("SetLinearDriveParams"));
        lin.number(STR("PositionStrength"), k);
        lin.number(STR("VelocityStrength"), c);
        lin.number(STR("InForceLimit"), max_force);
        lin.run();
        Call mode(joint, STR("SetAngularDriveMode"));
        mode.byte(STR("DriveMode"), 0); // SLERP
        mode.run();
        Call slerp(joint, STR("SetOrientationDriveSLERP"));
        slerp.first_flag(true);
        slerp.run();
        Call aslerp(joint, STR("SetAngularVelocityDriveSLERP"));
        aslerp.first_flag(true);
        aslerp.run();
        Call atarget(joint, STR("SetAngularOrientationTarget"));
        atarget.three(STR("InPosTarget"), 0.0, 0.0, 0.0);
        atarget.run();
        Call ang(joint, STR("SetAngularDriveParams"));
        ang.number(STR("PositionStrength"), ka);
        ang.number(STR("VelocityStrength"), ca);
        ang.number(STR("InForceLimit"), max_torque);
        ang.run();
    }

    // Reads a number inside nested structs of an object, by member names (e.g. ConstraintInstance >
    // ProfileInstance > LinearDrive > XDrive > Stiffness). NaN when a name is missing.
    double nested_number(UObject* owner, std::initializer_list<const TCHAR*> path)
    {
        if (!owner) return NAN;
        uint8_t* at = reinterpret_cast<uint8_t*>(owner);
        FProperty* p = nullptr;
        UScriptStruct* type = nullptr;
        bool first = true;
        for (const TCHAR* name : path)
        {
            p = first ? owner->GetPropertyByNameInChain(name) : struct_member(type, name);
            first = false;
            if (!p) return NAN;
            at += p->GetOffset_Internal();
            type = struct_type(p);
        }
        auto kind = p->GetClass().GetName();
        if (kind == STR("DoubleProperty")) return *reinterpret_cast<double*>(at);
        if (kind == STR("FloatProperty")) return *reinterpret_cast<float*>(at);
        if (kind == STR("ByteProperty") || kind == STR("EnumProperty")) return *at;
        if (kind == STR("BoolProperty"))
        {
            auto* flag = static_cast<FBoolProperty*>(p);
            return (at[flag->GetByteOffset()] & flag->GetFieldMask()) != 0 ? 1.0 : 0.0;
        }
        return NAN;
    }

    // A name inside nested structs (e.g. ConstraintInstance.ConstraintBone2), or "" if missing.
    std::string nested_name(UObject* owner, std::initializer_list<const TCHAR*> path)
    {
        if (!owner) return {};
        uint8_t* at = reinterpret_cast<uint8_t*>(owner);
        FProperty* p = nullptr;
        UScriptStruct* type = nullptr;
        bool first = true;
        for (const TCHAR* name : path)
        {
            p = first ? owner->GetPropertyByNameInChain(name) : struct_member(type, name);
            first = false;
            if (!p) return {};
            at += p->GetOffset_Internal();
            type = struct_type(p);
        }
        if (p->GetClass().GetName() != STR("NameProperty")) return {};
        std::wstring w = reinterpret_cast<FName*>(at)->ToString();
        std::string out;
        for (wchar_t ch : w) out += (char)(ch < 128 ? ch : '?');
        return out;
    }

    // Logs every number, flag and enum inside a struct (recursing into nested structs), one line
    // per value, so the joint's whole ConstraintInstance can be read instead of guessed.
    void dump_struct(const std::wstring& prefix, UScriptStruct* type, const uint8_t* at, int depth)
    {
        if (!type || depth > 5) return;
        for (UStruct* level = type; level; level = level->GetSuperStruct())
            for (FProperty* m : level->ForEachProperty())
            {
                const uint8_t* v = at + m->GetOffset_Internal();
                std::wstring name = prefix + STR(".") + m->GetName();
                auto kind = m->GetClass().GetName();
                if (kind == STR("StructProperty")) dump_struct(name, struct_type(m), v, depth + 1);
                else if (kind == STR("DoubleProperty")) HSVR_LOG("Grip:   %ls = %g", name.c_str(), *reinterpret_cast<const double*>(v));
                else if (kind == STR("FloatProperty")) HSVR_LOG("Grip:   %ls = %g", name.c_str(), *reinterpret_cast<const float*>(v));
                else if (kind == STR("IntProperty")) HSVR_LOG("Grip:   %ls = %d", name.c_str(), *reinterpret_cast<const int32_t*>(v));
                else if (kind == STR("ByteProperty") || (kind == STR("EnumProperty") && m->GetSize() == 1)) HSVR_LOG("Grip:   %ls = %d", name.c_str(), (int)*v);
                else if (kind == STR("BoolProperty"))
                {
                    auto* flag = static_cast<FBoolProperty*>(m);
                    HSVR_LOG("Grip:   %ls = %d", name.c_str(), (v[flag->GetByteOffset()] & flag->GetFieldMask()) ? 1 : 0);
                }
                else if (kind == STR("NameProperty")) HSVR_LOG("Grip:   %ls = %ls", name.c_str(), reinterpret_cast<const FName*>(v)->ToString().c_str());
            }
    }

    // Every physics constraint or physics handle an object points at, and what each one joins, to
    // find anything else holding the weapon besides the hand joint.
    void list_holders(UObject* owner)
    {
        if (!owner) return;
        for (UStruct* level = owner->GetClassPrivate(); level; level = level->GetSuperStruct())
            for (FProperty* p : level->ForEachProperty())
            {
                if (p->GetClass().GetName() != STR("ObjectProperty")) continue;
                UObject* o = *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(owner) + p->GetOffset_Internal());
                if (!o) continue;
                std::wstring cls = o->GetClassPrivate()->GetName();
                if (cls.find(STR("PhysicsConstraint")) == std::wstring::npos && cls.find(STR("PhysicsHandle")) == std::wstring::npos) continue;
                std::wstring joined;
                Call who(o, STR("GetConstrainedComponents"));
                if (who.run())
                    for (const TCHAR* pn : {STR("OutComponent1"), STR("OutBoneName1"), STR("OutComponent2"), STR("OutBoneName2")})
                    {
                        FProperty* pp = who.param(pn);
                        if (!pp) continue;
                        uint8_t* at = who.buffer.data() + pp->GetOffset_Internal();
                        if (pp->GetClass().GetName() == STR("NameProperty")) joined += reinterpret_cast<FName*>(at)->ToString() + STR(" ");
                        else
                        {
                            UObject* c = *reinterpret_cast<UObject**>(at);
                            joined += (c ? c->GetFullName() : std::wstring(STR("none"))) + STR(" : ");
                        }
                    }
                UObject* grabbed = object_property(o, STR("GrabbedComponent"));
                if (grabbed) joined += STR("grabs ") + grabbed->GetFullName();
                double xm = nested_number(o, {STR("ConstraintInstance"), STR("ProfileInstance"), STR("LinearLimit"), STR("XMotion")});
                HSVR_LOG("Grip: holder %ls.%ls = %ls (%ls) X motion %.0f: %ls", owner->GetName().c_str(), p->GetName().c_str(), o->GetName().c_str(), cls.c_str(), xm,
                         joined.c_str());
            }
    }

    // ---- keeping the held weapon from colliding with his own hand and forearm ---------------------
    // Contacts are rigid and beat any drive, so a soft grip spring lost to his own fist and forearm
    // bodies pressing on the grip (tests 18-22: ~15 cm off with any stiffness, while the game's
    // locked joint overpowers them). UE turns collision off between two bodies joined by a
    // constraint with bDisableCollision, so the mod adds one free (no-force) constraint between the
    // weapon and each physics body of that arm from the forearm down.
    std::vector<FName> physics_body_bones(UObject* mesh)
    {
        std::vector<FName> out;
        UObject* asset = nullptr;
        for (const TCHAR* n : {STR("SkeletalMeshAsset"), STR("SkinnedAsset"), STR("SkeletalMesh")})
            if (!asset) asset = object_property(mesh, n);
        UObject* physics = object_property(mesh, STR("PhysicsAssetOverride"));
        if (!physics && asset) physics = object_property(asset, STR("PhysicsAsset"));
        if (!physics)
        {
            HSVR_LOG("Grip: no physics asset found on %ls", mesh->GetFullName().c_str());
            return out;
        }
        FProperty* list = physics->GetPropertyByNameInChain(STR("SkeletalBodySetups"));
        if (!list) return out;
        const RawArray* arr = reinterpret_cast<const RawArray*>(reinterpret_cast<uint8_t*>(physics) + list->GetOffset_Internal());
        for (int32_t i = 0; i < arr->Num; ++i)
        {
            UObject* body = reinterpret_cast<UObject*>(arr->Data[i]);
            if (!body) continue;
            FProperty* bn = body->GetPropertyByNameInChain(STR("BoneName"));
            if (bn) out.push_back(*reinterpret_cast<FName*>(reinterpret_cast<uint8_t*>(body) + bn->GetOffset_Internal()));
        }
        return out;
    }

    FName parent_bone(UObject* mesh, FName bone)
    {
        Call c(mesh, STR("GetParentBone"));
        c.name(STR("BoneName"), bone);
        if (!c.run()) return FName();
        FProperty* rp = c.param(STR("ReturnValue"));
        return rp ? *reinterpret_cast<FName*>(c.buffer.data() + rp->GetOffset_Internal()) : FName();
    }

    // True for the forearm and everything below it on that side (twist bones, hand, fingers).
    bool in_forearm(UObject* mesh, FName bone, int side)
    {
        std::wstring stop = side == 0 ? STR("lowerarm_l") : STR("lowerarm_r");
        FName at = bone;
        for (int i = 0; i < 12; ++i)
        {
            std::wstring n = at.ToString();
            if (n == stop) return true;
            if (n == STR("None") || n.empty()) return false;
            at = parent_bone(mesh, at);
        }
        return false;
    }

    struct ArmIgnore
    {
        UObject* weapon = nullptr;
        std::vector<UObject*> joints;
    };
    ArmIgnore g_arm_ignore[2];
    std::atomic<int> g_grip_ignore_arm{1};
    // hsvr.weapon_channel <channel> <response>: sets the held weapon's response to one collision
    // channel (0 ignore, 1 overlap, 2 block). Changing responses only updates the physics filter,
    // unlike switching collision off, which rebuilt the body and orphaned the hand joint (test 21).
    std::atomic<int> g_weapon_channel_cmd{-1}, g_weapon_channel_resp{2};

    int byte_result(Call& c)
    {
        FProperty* rp = c.param(STR("ReturnValue"));
        return rp ? *(c.buffer.data() + rp->GetOffset_Internal()) : -1;
    }

    // What his own bodies are to the physics (object channel), and how the weapon responds to each.
    void log_collision_types(UObject* pawn, UObject* base)
    {
        std::vector<std::pair<std::wstring, UObject*>> parts;
        parts.push_back({STR("Mesh"), object_property(pawn, STR("Mesh"))});
        parts.push_back({STR("Capsule"), object_property(pawn, STR("CapsuleComponent"))});
        for (const TCHAR* n : {STR("CM_Arm_R"), STR("CM_Arm_L"), STR("CM_Cuirass"), STR("CM_Helmet"), STR("CM_Leg_L"), STR("CM_Leg_R")})
            parts.push_back({n, object_property(pawn, n)});
        for (auto& [name, comp] : parts)
        {
            if (!comp) continue;
            Call t(comp, STR("GetCollisionObjectType"));
            t.run();
            int type = byte_result(t);
            Call e(comp, STR("GetCollisionEnabled"));
            e.run();
            Call r(base, STR("GetCollisionResponseToChannel"));
            r.byte(STR("Channel"), (uint8_t)(type < 0 ? 0 : type));
            r.run();
            HSVR_LOG("Grip: his %ls is channel %d (collision %d); the weapon's response to that channel is %d", name.c_str(), type, byte_result(e), byte_result(r));
        }
        Call t(base, STR("GetCollisionObjectType"));
        t.run();
        HSVR_LOG("Grip: the weapon itself is channel %d", byte_result(t));
    }

    void clear_arm_ignore(int side)
    {
        for (UObject* j : g_arm_ignore[side].joints)
        {
            Call d(j, STR("K2_DestroyComponent"));
            d.object(STR("Object"), j);
            d.run();
        }
        g_arm_ignore[side].joints.clear();
        g_arm_ignore[side].weapon = nullptr;
    }

    void ignore_arm_collision(UObject* pawn, UObject* mesh, UObject* weapon, UObject* base, int side)
    {
        if (g_arm_ignore[side].weapon == weapon) return;
        clear_arm_ignore(side);
        g_arm_ignore[side].weapon = weapon;
        UClass* cls = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Engine.PhysicsConstraintComponent"));
        if (!cls || !mesh) return;
        FName hand = bone_fname(side == 0 ? "hand_l" : "hand_r");
        std::wstring names;
        for (FName bone : physics_body_bones(mesh))
        {
            // The hand too (kick-test3: the hand body, 7.4 cm off the bone, sits in the grip; when the
            // pair collides it shoves the weapon out across the palm about twice a second). B&S never
            // lets a held item touch the hand that holds it.
            if ((!hsvr::config().pose_hold_ignore_hand && bone.ToString() == hand.ToString()) || !in_forearm(mesh, bone, side)) continue;
            Call add(pawn, STR("AddComponentByClass"));
            add.object(STR("Class"), cls);
            add.flag(STR("bManualAttachment"), false);
            if (FProperty* t = add.param(STR("RelativeTransform")))
            {
                uint8_t* at = add.buffer.data() + t->GetOffset_Internal();
                UScriptStruct* type = struct_type(t);
                if (FProperty* r = struct_member(type, STR("Rotation")))
                    if (FProperty* w = struct_member(struct_type(r), STR("W")))
                    {
                        uint8_t* wat = at + r->GetOffset_Internal() + w->GetOffset_Internal();
                        if (w->GetClass().GetName() == STR("DoubleProperty")) *reinterpret_cast<double*>(wat) = 1.0;
                        else *reinterpret_cast<float*>(wat) = 1.0f;
                    }
                if (FProperty* sc = struct_member(type, STR("Scale3D"))) write_three(at + sc->GetOffset_Internal(), sc, 1.0, 1.0, 1.0);
            }
            add.flag(STR("bDeferredFinish"), false);
            if (!add.run()) continue;
            UObject* j = add.object_result();
            if (!j) continue;
            // Free in every direction and no drives: it holds nothing, it only switches off the pair's collision.
            for (const TCHAR* fn : {STR("SetLinearXLimit"), STR("SetLinearYLimit"), STR("SetLinearZLimit")})
            {
                Call c1(j, fn);
                c1.byte(STR("ConstraintType"), 0);
                c1.number(STR("LimitSize"), 0.0);
                c1.run();
            }
            for (const TCHAR* fn : {STR("SetAngularSwing1Limit"), STR("SetAngularSwing2Limit"), STR("SetAngularTwistLimit")})
            {
                Call c1(j, fn);
                c1.byte(STR("MotionType"), 0);
                c1.run();
            }
            Call dc(j, STR("SetDisableCollision"));
            dc.first_flag(true);
            dc.run();
            Call join(j, STR("SetConstrainedComponents"));
            join.object(STR("Component1"), base);
            join.name(STR("BoneName1"), FName());
            join.object(STR("Component2"), mesh);
            join.name(STR("BoneName2"), bone);
            join.run();
            g_arm_ignore[side].joints.push_back(j);
            names += bone.ToString() + STR(" ");
        }
        HSVR_LOG("Grip: %s weapon no longer collides with: %ls", side == 0 ? "left" : "right", names.empty() ? STR("(no forearm bodies found)") : names.c_str());
    }

    // ---- what the held weapon is touching (test tool) ----------------------------------------------
    // For each of his physics bodies, the closest distance from the weapon's axis (pommel end to tip,
    // 12 points) to that body's collision. Bodies within 4 cm of the axis are touching or nearly
    // (a grip is ~1.5 cm in radius). Also every other component overlapping the weapon's meshes.
    void log_weapon_contacts(UObject* pawn, UObject* weapon, UObject* base)
    {
        using namespace hsvr;
        UObject* mesh = object_property(pawn, STR("Mesh"));
        UObject* pommel = object_property(weapon, STR("PommelSk"));
        UObject* tip = object_property(weapon, STR("TippyTipScene"));
        if (!mesh || !pommel || !tip) return;
        Vec3 a = component_location(pommel), b = component_location(tip);
        std::wstring near_list;
        for (FName bone : physics_body_bones(mesh))
        {
            double best = 1e9;
            for (int i = 0; i <= 11; ++i)
            {
                Vec3 pt = a + (b - a) * (i / 11.0);
                Call c(mesh, STR("GetClosestPointOnCollision"));
                c.three(STR("Point"), pt.x, pt.y, pt.z);
                c.name(STR("BoneName"), bone);
                if (!c.run()) break;
                double d = c.number();
                if (d >= 0.0 && d < best) best = d;
            }
            if (best < 4.0)
            {
                wchar_t one[96];
                swprintf(one, 96, L"%ls %.1f, ", bone.ToString().c_str(), best);
                near_list += one;
            }
        }
        HSVR_LOG("Grip: contacts: his bodies within 4 cm of the weapon axis: %ls", near_list.empty() ? STR("none") : near_list.c_str());
        static UObject* cdo = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetSystemLibrary"));
        if (!cdo) return;
        static uint8_t types[32];
        for (int i = 0; i < 32; ++i) types[i] = (uint8_t)i;
        std::wstring others;
        for (const TCHAR* part : {STR("Head"), STR("Guard"), STR("Grip"), STR("Pommel"), STR("BaseMesh")})
        {
            UObject* comp = object_property(weapon, part);
            if (!comp) continue;
            Call world(comp, STR("K2_GetComponentToWorld"));
            if (!world.run()) continue;
            FProperty* wr = world.param(STR("ReturnValue"));
            Call ov(cdo, STR("ComponentOverlapComponents"));
            FProperty* ct = ov.param(STR("ComponentTransform"));
            FProperty* ot = ov.param(STR("ObjectTypes"));
            FProperty* out = ov.param(STR("OutComponents"));
            if (!wr || !ct || !ot || !out) continue;
            ov.object(STR("Component"), comp);
            std::memcpy(ov.buffer.data() + ct->GetOffset_Internal(), world.buffer.data() + wr->GetOffset_Internal(), ct->GetSize());
            RawArray* list = reinterpret_cast<RawArray*>(ov.buffer.data() + ot->GetOffset_Internal());
            list->Data = reinterpret_cast<void**>(types);
            list->Num = list->Max = 32;
            if (!ov.run()) continue;
            RawArray* found = reinterpret_cast<RawArray*>(ov.buffer.data() + out->GetOffset_Internal());
            for (int32_t i = 0; i < found->Num && found->Data; ++i)
            {
                UObject* o = reinterpret_cast<UObject*>(found->Data[i]);
                if (!o || o->GetOuterPrivate() == weapon) continue;
                others += std::wstring(part) + STR(" touches ") + o->GetFullName() + STR("; ");
            }
            // The engine allocated that list; leave it (a few bytes per test log line).
            found->Data = nullptr;
            found->Num = found->Max = 0;
            list->Data = nullptr;
            list->Num = list->Max = 0;
        }
        HSVR_LOG("Grip: contacts: overlapping components: %ls", others.empty() ? STR("none") : others.c_str());
    }

    // The physics solver's console variables that shape a joint drive (read through the Kismet
    // library, since the shipping build writes no game log). Logged once per weapon.
    void log_chaos_cvars()
    {
        static UObject* cdo = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetSystemLibrary"));
        if (!cdo) return;
        for (const TCHAR* name :
             {STR("p.Chaos.JointConstraint.LinearDriveStiffnessScale"), STR("p.Chaos.JointConstraint.LinaearDriveDampingScale"),
              STR("p.Chaos.JointConstraint.AngularDriveStiffnessScale"), STR("p.Chaos.JointConstraint.AngularDriveDampingScale"),
              STR("p.Chaos.JointConstraint.SoftLinearForceMode"), STR("p.Chaos.JointConstraint.SoftAngularForceMode"),
              STR("p.Chaos.JointConstraint.JointStiffness"), STR("p.Chaos.Solver.Joint.UsePBDDrives"), STR("p.Chaos.Solver.Joint.UseLinearSolver"),
              STR("p.Chaos.Solver.Joint.MinSolverStiffness"), STR("p.Chaos.Solver.Joint.MaxSolverStiffness"),
              STR("p.Chaos.Solver.Joint.NumIterationsAtMaxSolverStiffness"), STR("p.Chaos.Solver.Joint.SolvePositionLast"),
              STR("p.Chaos.Solver.Joint.TransferCollisions"), STR("p.Chaos.Solver.Joint.MinParentMassRatio"), STR("p.Chaos.Solver.Joint.MaxInertiaRatio"),
              STR("p.Chaos.Solver.Iterations.Position"), STR("p.Chaos.Solver.Iterations.Velocity"), STR("p.Chaos.Solver.Iterations.Projection"),
              STR("p.Chaos.Joint.DisableSoftLimits"), STR("p.Chaos.Joint.VelProjectionAlpha"), STR("p.Chaos.Solver.Collision.MaxPushOutVelocity"),
              STR("p.Chaos.Solver.Collision.DepenetrationVelocity"), STR("p.Chaos.Solver.Joint.ShockPropagation"), STR("p.ChaosSolverEnableJointConstraints")})
        {
            Call f(cdo, STR("GetConsoleVariableFloatValue"));
            FProperty* vp = f.param(STR("VariableName"));
            if (!vp) return;
            FString text(name);
            *reinterpret_cast<FString*>(f.buffer.data() + vp->GetOffset_Internal()) = text;
            f.run();
            double v = f.number();
            *reinterpret_cast<FString*>(f.buffer.data() + vp->GetOffset_Internal()) = FString();
            Call i(cdo, STR("GetConsoleVariableIntValue"));
            FProperty* ip = i.param(STR("VariableName"));
            int iv = 0;
            if (ip)
            {
                *reinterpret_cast<FString*>(i.buffer.data() + ip->GetOffset_Internal()) = FString(name);
                i.run();
                iv = i.integer();
                *reinterpret_cast<FString*>(i.buffer.data() + ip->GetOffset_Internal()) = FString();
            }
            HSVR_LOG("Grip: cvar %ls = %g (int %d)", name, v, iv);
        }
    }

    // Rotation whose X, Y, Z axes are the given (orthonormalised) X and Y axes and their cross product.
    hsvr::Quat quat_from_axes(hsvr::Vec3 x, hsvr::Vec3 y)
    {
        using namespace hsvr;
        x = x * (1.0 / (std::max)(1e-9, length(x)));
        Vec3 z = cross(x, y);
        z = z * (1.0 / (std::max)(1e-9, length(z)));
        y = cross(z, x);
        double m00 = x.x, m10 = x.y, m20 = x.z, m01 = y.x, m11 = y.y, m21 = y.z, m02 = z.x, m12 = z.y, m22 = z.z;
        double tr = m00 + m11 + m22;
        Quat q;
        if (tr > 0)
        {
            double s = std::sqrt(tr + 1.0) * 2;
            q = Quat{(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25 * s};
        }
        else if (m00 > m11 && m00 > m22)
        {
            double s = std::sqrt(1.0 + m00 - m11 - m22) * 2;
            q = Quat{0.25 * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
        }
        else if (m11 > m22)
        {
            double s = std::sqrt(1.0 + m11 - m00 - m22) * 2;
            q = Quat{(m01 + m10) / s, 0.25 * s, (m12 + m21) / s, (m02 - m20) / s};
        }
        else
        {
            double s = std::sqrt(1.0 + m22 - m00 - m11) * 2;
            q = Quat{(m02 + m20) / s, (m12 + m21) / s, 0.25 * s, (m10 - m01) / s};
        }
        return q.normalized();
    }

    void dump_joint(UObject* joint)
    {
        FProperty* p = joint ? joint->GetPropertyByNameInChain(STR("ConstraintInstance")) : nullptr;
        if (!p) return;
        HSVR_LOG("Grip: full joint settings of %ls:", joint->GetFullName().c_str());
        dump_struct(STR("ConstraintInstance"), struct_type(p), reinterpret_cast<const uint8_t*>(joint) + p->GetOffset_Internal(), 0);
    }

    // What the hand joint really has set, for the log: linear X drive (on, stiffness, damping, max
    // force), slerp drive (on, stiffness, max), linear X motion (0 free, 1 limited, 2 locked).
    std::string joint_settings(UObject* joint)
    {
        auto n = [&](std::initializer_list<const TCHAR*> path) { return nested_number(joint, path); };
        char text[320];
        std::snprintf(text, sizeof(text), "linX pos %.0f k %.0f c %.0f max %.0f, slerp %.0f k %.0f max %.0f, mode %.0f, X motion %.0f, swing1 %.0f",
                      n({STR("ConstraintInstance"), STR("ProfileInstance"), STR("LinearDrive"), STR("XDrive"), STR("bEnablePositionDrive")}),
                      n({STR("ConstraintInstance"), STR("ProfileInstance"), STR("LinearDrive"), STR("XDrive"), STR("Stiffness")}),
                      n({STR("ConstraintInstance"), STR("ProfileInstance"), STR("LinearDrive"), STR("XDrive"), STR("Damping")}),
                      n({STR("ConstraintInstance"), STR("ProfileInstance"), STR("LinearDrive"), STR("XDrive"), STR("MaxForce")}),
                      n({STR("ConstraintInstance"), STR("ProfileInstance"), STR("AngularDrive"), STR("SlerpDrive"), STR("bEnablePositionDrive")}),
                      n({STR("ConstraintInstance"), STR("ProfileInstance"), STR("AngularDrive"), STR("SlerpDrive"), STR("Stiffness")}),
                      n({STR("ConstraintInstance"), STR("ProfileInstance"), STR("AngularDrive"), STR("SlerpDrive"), STR("MaxForce")}),
                      n({STR("ConstraintInstance"), STR("ProfileInstance"), STR("AngularDrive"), STR("AngularDriveMode")}),
                      n({STR("ConstraintInstance"), STR("ProfileInstance"), STR("LinearLimit"), STR("XMotion")}),
                      n({STR("ConstraintInstance"), STR("ProfileInstance"), STR("ConeLimit"), STR("Swing1Motion")}));
        return text;
    }

    // Just before his tick: what the hand joint looks like after everything else ran since our
    // last write (the weapon's own tick, physics), to catch anyone else changing it.
    void log_joint_before_tick(UObject* pawn)
    {
        static double last = 0.0;
        double now = hsvr::now_seconds();
        if (now - last < 1.0) return;
        UObject* joint = object_property(pawn, STR("PhysicsConstraint R Hand"));
        if (!joint || !object_property(pawn, STR("Weapon R"))) return;
        last = now;
        HSVR_LOG("Grip: right joint before his tick: %s", joint_settings(joint).c_str());
    }

    // ---- The weapon in your hand: the hold from Matthew's Chasms VR Framework ---------------------
    // Ported from crates/runtime/src/rig/physics.rs (Physics::joint, Joint::one_hand,
    // body_inertia_tensor_about; MIT, Copyright (c) 2026 chasmlol). No joint pulls the weapon: the
    // game's hand joint is left in place but free (no limits, no drives), so the game still knows the
    // weapon is held. Every tick, before physics, the weapon's own velocity and spin are set so the
    // point held arrives where the controller has the hand at the end of the step: a spring and a
    // damper from the controller to that point, solved implicitly (steady at any frame rate) and no
    // stronger than the hand. The weapon keeps its mass and gravity: its weight turns it about the
    // grip against its inertia about the grip (a heavy head sags and trails a fast grip), and what it
    // hits stops it.
    struct Mat3
    {
        double m[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
        static Mat3 diag(double a, double b, double c)
        {
            Mat3 r;
            r.m[0][0] = a, r.m[1][1] = b, r.m[2][2] = c;
            return r;
        }
        static Mat3 from_quat(const hsvr::Quat& q)
        {
            Mat3 r;
            hsvr::Vec3 cols[3] = {q.rotate({1, 0, 0}), q.rotate({0, 1, 0}), q.rotate({0, 0, 1})};
            for (int j = 0; j < 3; ++j) r.m[0][j] = cols[j].x, r.m[1][j] = cols[j].y, r.m[2][j] = cols[j].z;
            return r;
        }
        Mat3 operator*(const Mat3& o) const
        {
            Mat3 r;
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    for (int k = 0; k < 3; ++k) r.m[i][j] += m[i][k] * o.m[k][j];
            return r;
        }
        Mat3 operator+(const Mat3& o) const
        {
            Mat3 r;
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) r.m[i][j] = m[i][j] + o.m[i][j];
            return r;
        }
        Mat3 operator*(double s) const
        {
            Mat3 r;
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) r.m[i][j] = m[i][j] * s;
            return r;
        }
        hsvr::Vec3 operator*(const hsvr::Vec3& v) const
        {
            return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z, m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
                    m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z};
        }
        Mat3 transpose() const
        {
            Mat3 r;
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) r.m[i][j] = m[j][i];
            return r;
        }
        Mat3 inverse() const
        {
            const auto& a = m;
            double c00 = a[1][1] * a[2][2] - a[1][2] * a[2][1], c01 = a[1][2] * a[2][0] - a[1][0] * a[2][2], c02 = a[1][0] * a[2][1] - a[1][1] * a[2][0];
            double det = a[0][0] * c00 + a[0][1] * c01 + a[0][2] * c02;
            Mat3 r;
            if (std::fabs(det) < 1e-18) return r;
            double s = 1.0 / det;
            r.m[0][0] = c00 * s;
            r.m[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) * s;
            r.m[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) * s;
            r.m[1][0] = c01 * s;
            r.m[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) * s;
            r.m[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) * s;
            r.m[2][0] = c02 * s;
            r.m[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) * s;
            r.m[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) * s;
            return r;
        }
    };

    hsvr::Vec3 clamp_length(const hsvr::Vec3& v, double most)
    {
        double l = hsvr::length(v);
        return l > most && l > 1e-12 ? v * (most / l) : v;
    }

    // Rotation vector (axis times angle, radians) of a rotation, the short way round.
    hsvr::Vec3 scaled_axis(hsvr::Quat q)
    {
        q = q.normalized();
        if (q.w < 0) q = hsvr::Quat{-q.x, -q.y, -q.z, -q.w};
        double s = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
        if (s < 1e-12) return {q.x * 2.0, q.y * 2.0, q.z * 2.0};
        double angle = 2.0 * std::atan2(s, q.w);
        return hsvr::Vec3{q.x, q.y, q.z} * (angle / s);
    }

    hsvr::Quat from_scaled_axis(const hsvr::Vec3& v)
    {
        double angle = hsvr::length(v);
        if (angle < 1e-12) return hsvr::Quat{v.x * 0.5, v.y * 0.5, v.z * 0.5, 1.0}.normalized();
        return hsvr::quat_axis_angle(v * (1.0 / angle), angle);
    }

    void free_joint(UObject* joint)
    {
        for (const TCHAR* fn : {STR("SetLinearXLimit"), STR("SetLinearYLimit"), STR("SetLinearZLimit")})
        {
            Call c1(joint, fn);
            c1.byte(STR("ConstraintType"), 0); // free
            c1.number(STR("LimitSize"), 0.0);
            c1.run();
        }
        for (const TCHAR* fn : {STR("SetAngularSwing1Limit"), STR("SetAngularSwing2Limit"), STR("SetAngularTwistLimit")})
        {
            Call c1(joint, fn);
            c1.byte(STR("MotionType"), 0); // free
            c1.run();
        }
        Call pos(joint, STR("SetLinearPositionDrive"));
        pos.run(); // all three off
        Call vel(joint, STR("SetLinearVelocityDrive"));
        vel.run();
        Call slerp(joint, STR("SetOrientationDriveSLERP"));
        slerp.first_flag(false);
        slerp.run();
        Call aslerp(joint, STR("SetAngularVelocityDriveSLERP"));
        aslerp.first_flag(false);
        aslerp.run();
        Call twist(joint, STR("SetOrientationDriveTwistAndSwing"));
        twist.run(); // both off
        Call atwist(joint, STR("SetAngularVelocityDriveTwistAndSwing"));
        atwist.run();
    }

    // -1 = the ini (pose_grip_hold); 0 / 1 from the dev script (hsvr.grip_hold).
    std::atomic<int> g_grip_hold_override{-1};

    // A grip the mod chose instead of the game's (shields: Matthew 2026-11-18, "if i can see more then
    // one grab point then i want to be able to grab it", and the rim anywhere). The weapon is held at
    // `pivot` (BaseMesh frame) by the point `in_anchor` of the anchor (the hand bone, or for a strap
    // the bone `bone`), turned `rel` from the anchor's frame. The game still thinks the hand holds it
    // its own way; only where the hold puts it changes.
    struct FreeGrip
    {
        bool set = false;
        hsvr::Vec3 pivot, in_anchor;
        hsvr::Quat rel;
        bool forearm = false;
        std::string bone;
        std::string what; // for the log: "rim", a strap's name...
        bool place = false; // put the weapon there on the hold's first tick
    };
    FreeGrip g_pending_free[2]; // applied when that hand's next hold starts

    struct HoldState
    {
        UObject* weapon = nullptr;
        FreeGrip free;
        hsvr::Vec3 last_target_pos;
        hsvr::Quat last_target_rot;
        double last_time = 0.0;
        bool have_last = false;
        bool inertia_logged = false;
        // For the log: the lag of the point held behind the controller (cm), the turn behind it
        // (deg), the hand's force (N) and torque (N m) this tick, and whether either hit its cap.
        double lag = 0.0, turn_deg = 0.0, force_n = 0.0, torque_nm = 0.0, dt = 0.0;
        bool force_capped = false, torque_capped = false;
        // The joint's frames, for placing the hand on the weapon.
        hsvr::Vec3 pivot, in_hand;
        hsvr::Quat f1, f2;
        // His hand on the weapon this frame: how far the weapon is from where the hold sent it (cm),
        // and how much of that the hand took (0 follows the controller, 1 sits on the weapon).
        double blocked_cm = 0.0, on_weapon = 0.0;
        double stuck_since = -1.0;
        double started = -1.0;        // when this hand began holding this weapon
        // Contact, from the physics' answer: where the hold sent the weapon's centre of mass last tick
        // against where it got to. Free, it gets there (within gravity's share of a step); something in
        // the way (the floor, a body, a shield) stops it short. The stuck rule needs this, so a weapon
        // that only lags (a long spear swinging up at the hand's torque limit) is never let go.
        bool have_expected = false;
        hsvr::Vec3 expected_com;
        hsvr::Vec3 last_set_v, last_set_w;
        std::string last_responses;
        double miss_cm = 0.0, last_contact = -100.0;
        double last_blocked_cm = -1.0; // last tick's blocked_cm (a weapon still coming is not stuck)
        // The slide: how far the hand has moved along the weapon (cm along its +Z from the game's grip
        // point), and the grip zone it may move in (found once per weapon).
        double slide = 0.0;
        bool zone_known = false;
        double zone_lo = 0.0, zone_hi = 0.0;
        double zone_stop = 0.0, zone_tip = 0.0; // the guard (top stop) and the tip, from the game's grip point
        double body_strength = 1.0; // the game's stats on this hand's hold (strength x injury x stamina)
        bool forearm = false;       // held on the forearm (strap shields: the game's joint is to the lower arm)
        std::string anchor_bone;    // the bone the game's joint holds it to
        bool sliding = false;
        double far_since = -1.0, far_logged = 0.0;
    };
    HoldState g_hold[2];

    // The game's off-hand search: with a two-handed weapon (longswords) in one hand and the other
    // free, about once a second the game drops "Block <other> OffHand Grab", and on the next tick
    // sets "<other> Offhand Searching Grip" and re-seats the weapon (teleports BaseMesh to the hand
    // socket and back, UF @499290-@504348), all inside that one tick. Under the hold that teleport is
    // a one-tick shove across the blade (kick tests 13-14). The mod owns grips, so just before his
    // tick the gate is kept shut for a free other hand while the mod holds a weapon. A hand that holds
    // or grabs something itself is left alone.
    void block_offhand_search_before_tick(UObject* pawn)
    {
        if (!hsvr::config().pose_hold_block_offhand_search) return;
        static const TCHAR* weapon_name[2] = {STR("Weapon L"), STR("Weapon R")};
        static const TCHAR* grabbed_name[2] = {STR("Grabbed L"), STR("Grabbed R")};
        static const TCHAR* block_name[2] = {STR("Block L OffHand Grab"), STR("Block R OffHand Grab")};
        static const TCHAR* search_name[2] = {STR("L Offhand Searching Grip"), STR("R Offhand Searching Grip")};
        static bool reported[2] = {false, false};
        for (int side = 0; side < 2; ++side)
        {
            UObject* weapon = g_hold[side].weapon;
            if (!weapon || object_property(pawn, weapon_name[side]) != weapon) continue;
            int other = 1 - side;
            if (object_property(pawn, weapon_name[other]) || bool_property(pawn, grabbed_name[other])) continue;
            bool block_ok = set_bool_property(pawn, block_name[other], true);
            bool search_ok = set_bool_property(pawn, search_name[other], false);
            if (!reported[other])
            {
                reported[other] = true;
                HSVR_LOG("Hold: the game's %s off-hand search kept off while the %s hand holds a weapon (%ls %s, %ls %s)", other == 0 ? "left" : "right",
                         side == 0 ? "left" : "right", block_name[other], block_ok ? "set" : "NOT FOUND", search_name[other], search_ok ? "cleared" : "NOT FOUND");
            }
        }
    }

    double hand_on_weapon_amount(int side) { return g_hold[side].on_weapon; }

    // ---- The ghost hand ------------------------------------------------------------------------
    // While his hand stays on a stopped weapon, a see-through hand shows where your real hand is
    // (Matthew's framework, rig/mod.rs: "ghosts"). The cooked game has no separate hand mesh, so it
    // is one of the game's static glove meshes with the engine's unlit translucent material
    // (ghost_hand_assets.md). Its offset and turn from the hand bone are in the ini for fitting.
    struct Ghost
    {
        UObject* pawn = nullptr;
        UObject* comp = nullptr;
        bool visible = false;
        bool failed = false;
    };
    Ghost g_ghost[2];
    std::atomic<bool> g_ghost_force{false};

    void ghost_hand(int side, const hsvr::Config& cfg, bool show, const hsvr::Vec3& wrist, const hsvr::Quat& hand_rot)
    {
        using namespace hsvr;
        Ghost& g = g_ghost[side];
        if (g.pawn != g_pose.pawn)
        {
            g = Ghost{};
            g.pawn = g_pose.pawn;
        }
        if (g_ghost_force.load()) show = true; // test: hsvr.ghost_show 1 shows it at your hand all the time
        if (!cfg.pose_ghost_hand) show = false;
        if (!g.comp && show && !g.failed && g.pawn)
        {
            g.failed = true; // tried once per character
            UClass* cls = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Engine.StaticMeshComponent"));
            std::wstring mesh_path(cfg.pose_ghost_mesh.begin(), cfg.pose_ghost_mesh.end());
            std::wstring mat_path(cfg.pose_ghost_material.begin(), cfg.pose_ghost_material.end());
            UObject* mesh = load_asset_object(mesh_path);
            UObject* mat = load_asset_object(mat_path);
            if (!cls || !mesh)
            {
                HSVR_LOG("Ghost: no %ls (mesh %ls), no ghost hand", cls ? STR("mesh") : STR("StaticMeshComponent class"), mesh_path.c_str());
                return;
            }
            Call add(g.pawn, STR("AddComponentByClass"));
            add.object(STR("Class"), cls);
            add.flag(STR("bManualAttachment"), true);
            if (FProperty* t = add.param(STR("RelativeTransform")))
            {
                uint8_t* at = add.buffer.data() + t->GetOffset_Internal();
                UScriptStruct* type = struct_type(t);
                if (FProperty* r = struct_member(type, STR("Rotation")))
                    if (FProperty* w = struct_member(struct_type(r), STR("W")))
                    {
                        uint8_t* wat = at + r->GetOffset_Internal() + w->GetOffset_Internal();
                        if (w->GetClass().GetName() == STR("DoubleProperty")) *reinterpret_cast<double*>(wat) = 1.0;
                        else *reinterpret_cast<float*>(wat) = 1.0f;
                    }
                if (FProperty* sc = struct_member(type, STR("Scale3D"))) write_three(at + sc->GetOffset_Internal(), sc, 1.0, 1.0, 1.0);
            }
            add.flag(STR("bDeferredFinish"), false);
            if (!add.run() || !add.object_result())
            {
                HSVR_LOG("Ghost: could not add the ghost hand component");
                return;
            }
            UObject* c = add.object_result();
            Call sm(c, STR("SetStaticMesh"));
            sm.object(STR("NewMesh"), mesh);
            sm.run();
            Call ce(c, STR("SetCollisionEnabled"));
            ce.byte(STR("NewType"), 0);
            ce.run();
            Call cs(c, STR("SetCastShadow"));
            cs.first_flag(false);
            cs.run();
            int slots = 0;
            if (mat)
            {
                Call n(c, STR("GetNumMaterials"));
                n.run();
                slots = (std::max)(1, n.integer());
                for (int i = 0; i < slots; ++i)
                {
                    Call m(c, STR("SetMaterial"));
                    if (FProperty* ip = m.param(STR("ElementIndex"))) *reinterpret_cast<int32_t*>(m.buffer.data() + ip->GetOffset_Internal()) = i;
                    m.object(STR("Material"), mat);
                    m.run();
                }
            }
            // The left hand is the right glove mirrored.
            Call scale(c, STR("SetWorldScale3D"));
            scale.three(STR("NewScale"), cfg.pose_ghost_scale, cfg.pose_ghost_scale * (side == 0 ? -1.0 : 1.0), cfg.pose_ghost_scale);
            scale.run();
            g.comp = c;
            g.failed = false;
            HSVR_LOG("Ghost: %s ghost hand made from %ls, material %ls on %d slots", side == 0 ? "left" : "right", mesh_path.c_str(),
                     mat ? mat_path.c_str() : STR("(not found, the glove's own)"), slots);
        }
        if (!g.comp) return;
        if (show)
        {
            double out = side == 0 ? -1.0 : 1.0;
            // The glove mesh (bounds from the cooked asset, run 32): its cuff at the origin, fingers
            // along +Z, thumb toward -Y. Turned so its fingers and thumb lie along the hand bone's
            // (measured at calibration); the left glove is the right one mirrored in Y (scale -1), so
            // its thumb is toward +Y. The ini turn (pitch, yaw, roll) is applied on top, in the glove's frame.
            const ArmCalibration& a = g_pose.arm[side];
            Vec3 fingers = hand_rot.rotate(a.fingers_local), thumb = hand_rot.rotate(a.thumb_local);
            Vec3 y = side == 0 ? thumb : thumb * -1.0;
            Vec3 x = cross(y, fingers);
            Quat glove = quat_from_axes(x, y);
            Quat rot = (glove * rot_to_quat(cfg.pose_ghost_pitch, cfg.pose_ghost_yaw * out, cfg.pose_ghost_roll * out)).normalized();
            Vec3 at = wrist + hand_rot.rotate(Vec3{cfg.pose_ghost_x_cm, cfg.pose_ghost_y_cm * out, cfg.pose_ghost_z_cm});
            set_component_transform(g.comp, at, rot, true);
            Call scale_set(g.comp, STR("SetWorldScale3D"));
            scale_set.three(STR("NewScale"), cfg.pose_ghost_scale, cfg.pose_ghost_scale * (side == 0 ? -1.0 : 1.0), cfg.pose_ghost_scale);
            scale_set.run();
            static double last_log[2] = {0.0, 0.0};
            double now = now_seconds();
            if (now - last_log[side] >= 1.0 && g_pose.mesh)
            {
                last_log[side] = now;
                FName hb = bone_fname(side == 0 ? "hand_l" : "hand_r");
                Vec3 hand_at = socket_location(g_pose.mesh, hb);
                Rot hr = quat_to_rot(socket_rotation(g_pose.mesh, hb)), gr = quat_to_rot(rot), cr = quat_to_rot(hand_rot);
                Vec3 tip = at + rot.rotate(Vec3{0.0, 0.0, 20.0 * cfg.pose_ghost_scale});
                Call sc(g.comp, STR("K2_GetComponentScale"));
                sc.run();
                Vec3 scale = sc.vec();
                HSVR_LOG("Ghost: %s at (%.1f, %.1f, %.1f) rot (%.0f, %.0f, %.0f), its fingertips at (%.1f, %.1f, %.1f); your hand at (%.1f, %.1f, %.1f) "
                         "rot (%.0f, %.0f, %.0f); his hand bone at (%.1f, %.1f, %.1f) rot (%.0f, %.0f, %.0f); scale (%.2f, %.2f, %.2f)",
                         side == 0 ? "left" : "right", at.x, at.y, at.z, gr.pitch, gr.yaw, gr.roll, tip.x, tip.y, tip.z, wrist.x, wrist.y, wrist.z, cr.pitch,
                         cr.yaw, cr.roll, hand_at.x, hand_at.y, hand_at.z, hr.pitch, hr.yaw, hr.roll, scale.x, scale.y, scale.z);
            }
        }
        if (show != g.visible)
        {
            g.visible = show;
            Call v(g.comp, STR("SetVisibility"));
            v.flag(STR("bNewVisibility"), show);
            v.flag(STR("bPropagateToChildren"), true);
            v.run();
            Call h(g.comp, STR("SetHiddenInGame"));
            h.flag(STR("NewHidden"), !show);
            h.flag(STR("bPropagateToChildren"), true);
            h.run();
        }
    }

    // Lets go of the weapon in that hand the game's own way: Set Up Right/Left Hand Weapon with no
    // weapon and "Dropped With no Damage" (decompile: what the game's tap on grab does; it breaks the
    // hand joint, detaches the weapon and gives it its dropped damping).
    bool is_fist(UObject* weapon);
    void remove_fist(int side, UObject* pawn, const char* why);
    void drop_weapon(int side, UObject* pawn)
    {
        // A fist is not a weapon to drop: it goes.
        if (is_fist(object_property(pawn, side == 0 ? STR("Weapon L") : STR("Weapon R"))))
        {
            remove_fist(side, pawn, "dropped");
            return;
        }
        const TCHAR* name = side == 0 ? STR("Set Up Left Hand Weapon") : STR("Set Up Right Hand Weapon");
        UFunction* fn = pawn ? pawn->GetFunctionByNameInChain(name) : nullptr;
        if (!fn)
        {
            HSVR_LOG("Hold: no \"%ls\" on the character, the weapon is not dropped", name);
            return;
        }
        // Sized from the function. The fixed 4096-byte static block this replaced was overrun: the
        // hand position written for the fist landed on the grab button's state (fist-eat test 4).
        std::vector<uint8_t> block = param_block(fn);
        uint8_t* parameters = block.data();
        std::wstring names;
        for (FProperty* p : fn->ForEachProperty())
        {
            std::wstring n = p->GetName();
            names += n + STR(", ");
            if (n.find(STR("Dropped")) != std::wstring::npos && p->GetClass().GetName() == STR("BoolProperty"))
            {
                auto* flag = static_cast<FBoolProperty*>(p);
                parameters[p->GetOffset_Internal() + flag->GetByteOffset()] |= flag->GetFieldMask();
            }
            // A transform parameter gets a valid (identity) rotation and scale 1, in case it is read.
            if (p->GetClass().GetName() == STR("StructProperty"))
                if (UScriptStruct* type = struct_type(p))
                {
                    uint8_t* at = parameters + p->GetOffset_Internal();
                    if (FProperty* r = struct_member(type, STR("Rotation")))
                        if (FProperty* w = struct_member(struct_type(r), STR("W")))
                        {
                            uint8_t* wat = at + r->GetOffset_Internal() + w->GetOffset_Internal();
                            if (w->GetClass().GetName() == STR("DoubleProperty")) *reinterpret_cast<double*>(wat) = 1.0;
                            else *reinterpret_cast<float*>(wat) = 1.0f;
                        }
                    if (FProperty* sc = struct_member(type, STR("Scale3D"))) write_three(at + sc->GetOffset_Internal(), sc, 1.0, 1.0, 1.0);
                }
        }
        pawn->ProcessEvent(fn, parameters);
        g_hold[side] = HoldState{}; // a new hold (and slide) when anything is next held
        HSVR_LOG("Hold: dropped with %ls(%ls)", name, names.c_str());
    }

    // Puts a weapon lying in the world into that hand the game's own way: Set Up Right/Left Hand
    // Weapon(no class, the weapon, not dropped, don't destroy, empty passport, save) - the call the
    // game's grab makes once its hand reaches a ModularWeaponBP (grab_pickup.md, UF @523466 / @535604).
    // It snaps the weapon's "Grip R Hand Default Pos" onto the hand socket and makes the hand joint.
    bool give_hand_weapon(int side, UObject* pawn, UObject* weapon)
    {
        const TCHAR* name = side == 0 ? STR("Set Up Left Hand Weapon") : STR("Set Up Right Hand Weapon");
        UFunction* fn = pawn ? pawn->GetFunctionByNameInChain(name) : nullptr;
        if (!fn || !weapon) return false;
        // Sized from the function. The fixed 4096-byte static block this replaced was overrun: the
        // hand position written for the fist landed on the grab button's state (fist-eat test 4).
        std::vector<uint8_t> block = param_block(fn);
        uint8_t* parameters = block.data();
        bool set = false;
        for (FProperty* p : fn->ForEachProperty())
        {
            if (p->GetClass().GetName() == STR("ObjectProperty") && !set)
            {
                *reinterpret_cast<UObject**>(parameters + p->GetOffset_Internal()) = weapon;
                set = true;
            }
            if (p->GetClass().GetName() == STR("StructProperty"))
                if (UScriptStruct* type = struct_type(p))
                {
                    uint8_t* at = parameters + p->GetOffset_Internal();
                    if (FProperty* r = struct_member(type, STR("Rotation")))
                        if (FProperty* w = struct_member(struct_type(r), STR("W")))
                        {
                            uint8_t* wat = at + r->GetOffset_Internal() + w->GetOffset_Internal();
                            if (w->GetClass().GetName() == STR("DoubleProperty")) *reinterpret_cast<double*>(wat) = 1.0;
                            else *reinterpret_cast<float*>(wat) = 1.0f;
                        }
                    if (FProperty* sc = struct_member(type, STR("Scale3D"))) write_three(at + sc->GetOffset_Internal(), sc, 1.0, 1.0, 1.0);
                }
        }
        if (!set) return false;
        pawn->ProcessEvent(fn, parameters);
        g_hold[side] = HoldState{};
        return true;
    }

    // Where a weapon would sit in that hand: the hand socket the game snaps weapons to, else the bone.
    hsvr::Vec3 hand_grab_point(UObject* mesh, int side)
    {
        static const FName sockets[2] = {FName(STR("weapon_lSocket"), FNAME_Add), FName(STR("weapon_rSocket"), FNAME_Add)};
        Call has(mesh, STR("DoesSocketExist"));
        has.name(STR("InSocketName"), sockets[side]);
        has.run();
        FProperty* rp = has.param(STR("ReturnValue"));
        bool exists = rp && *(has.buffer.data() + rp->GetOffset_Internal()) != 0;
        return socket_location(mesh, exists ? sockets[side] : bone_fname(side == 0 ? "hand_l" : "hand_r"));
    }

    // Test (hsvr.weapon_dump): everything about the weapon in a hand that shapes its grip zones, in
    // its BaseMesh's frame (cm; the blade is along +Z): every component of the weapon (class, tags,
    // origin, local bounds), the BaseMesh's sockets, the hand joint's weapon point (Pos1), and every
    // number variable with "Length" in its name. Facts for the slide's zones, not guesses.
    void weapon_dump(int side, UObject* pawn)
    {
        using namespace hsvr;
        UObject* weapon = object_property(pawn, side == 0 ? STR("Weapon L") : STR("Weapon R"));
        UObject* base = object_property(weapon, STR("BaseMesh"));
        if (!base)
        {
            HSVR_LOG("Dump: no weapon in the %s hand", side == 0 ? "left" : "right");
            return;
        }
        Vec3 bp = component_location(base);
        Quat bq = component_rotation(base);
        Quat binv = bq.inverse();
        auto local = [&](const Vec3& w) { return binv.rotate(w - bp); };
        HSVR_LOG("Dump: %ls (class %ls), BaseMesh %ls", weapon->GetFullName().c_str(), weapon->GetClassPrivate()->GetName().c_str(), base->GetFullName().c_str());
        // The weapon in the hand bone's frame (for checking that the left mirrors the right).
        if (UObject* mesh = find_pawn_mesh())
        {
            FName hb = bone_fname(side == 0 ? "hand_l" : "hand_r");
            Vec3 hp = socket_location(mesh, hb);
            Quat hq = socket_rotation(mesh, hb);
            Vec3 at = hq.inverse().rotate(bp - hp);
            Quat q = (hq.inverse() * bq).normalized();
            Vec3 bx = q.rotate(Vec3{1, 0, 0}), bz = q.rotate(Vec3{0, 0, 1});
            HSVR_LOG("Dump: BaseMesh in the %s hand bone's frame: at (%.1f, %.1f, %.1f) cm, its X axis (%.2f, %.2f, %.2f), its Z axis (blade) (%.2f, %.2f, %.2f)",
                     side == 0 ? "left" : "right", at.x, at.y, at.z, bx.x, bx.y, bx.z, bz.x, bz.y, bz.z);
        }
        for (UStruct* level = weapon->GetClassPrivate(); level; level = level->GetSuperStruct())
            for (FProperty* p : level->ForEachProperty())
            {
                std::wstring n = p->GetName();
                auto kind = p->GetClass().GetName();
                if (n.find(STR("Length")) != std::wstring::npos && (kind == STR("DoubleProperty") || kind == STR("FloatProperty")))
                    HSVR_LOG("Dump:   var %ls = %.2f", n.c_str(), number_property(weapon, p));
            }
        std::vector<UObject*> comps;
        UObjectGlobals::FindAllOf(STR("SceneComponent"), comps);
        for (UObject* c : comps)
        {
            if (!c || c->GetOuterPrivate() != weapon) continue;
            Vec3 o = local(component_location(c));
            std::wstring tags;
            if (RawArray* t = c->GetValuePtrByPropertyNameInChain<RawArray>(STR("ComponentTags")))
                for (int32_t i = 0; i < t->Num && t->Data; ++i) tags += reinterpret_cast<const FName*>(t->Data)[i].ToString() + STR(" ");
            double lo = 0, hi = 0;
            bool have_bounds = false;
            Call lb(c, STR("GetLocalBounds"));
            if (lb.fn && lb.run())
            {
                // Local bounds are in the component's own frame; take their Z ends along the BaseMesh axis.
                Vec3 mn = lb.vec(STR("Min")), mx = lb.vec(STR("Max"));
                Quat cq = component_rotation(c);
                Vec3 cp = component_location(c);
                double zs[8];
                for (int k = 0; k < 8; ++k)
                {
                    Vec3 corner{(k & 1) ? mx.x : mn.x, (k & 2) ? mx.y : mn.y, (k & 4) ? mx.z : mn.z};
                    zs[k] = local(cp + cq.rotate(corner)).z;
                }
                lo = *std::min_element(zs, zs + 8);
                hi = *std::max_element(zs, zs + 8);
                have_bounds = true;
            }
            std::wstring span = have_bounds ? STR(", z ") + std::to_wstring(lo) + STR(" .. ") + std::to_wstring(hi) : std::wstring();
            HSVR_LOG("Dump:   comp %ls (%ls) at %.1f %.1f %.1f%ls tags [%ls]", c->GetName().c_str(), c->GetClassPrivate()->GetName().c_str(), o.x, o.y, o.z,
                     span.c_str(), tags.c_str());
        }
        Call sn(base, STR("GetAllSocketNames"));
        if (sn.run())
            if (FProperty* rp = sn.param(STR("ReturnValue")))
            {
                RawArray* names = reinterpret_cast<RawArray*>(sn.buffer.data() + rp->GetOffset_Internal());
                for (int32_t i = 0; i < names->Num && names->Data; ++i)
                {
                    FName n = reinterpret_cast<const FName*>(names->Data)[i];
                    Vec3 at = local(socket_location(base, n));
                    HSVR_LOG("Dump:   socket %ls at %.1f %.1f %.1f", n.ToString().c_str(), at.x, at.y, at.z);
                }
                names->Data = nullptr; // the engine's list; left
                names->Num = names->Max = 0;
            }
        HoldState& h = g_hold[side];
        if (h.weapon == weapon) HSVR_LOG("Dump:   hand joint weapon point (Pos1) %.1f %.1f %.1f", h.pivot.x, h.pivot.y, h.pivot.z);
        Call ext(base, STR("GetLocalBounds"));
        if (ext.run())
        {
            Vec3 mn = ext.vec(STR("Min")), mx = ext.vec(STR("Max"));
            HSVR_LOG("Dump:   BaseMesh local bounds %.1f %.1f %.1f .. %.1f %.1f %.1f", mn.x, mn.y, mn.z, mx.x, mx.y, mx.z);
        }
    }

    // Writes a Blueprint variable of the character by name: a number, yes/no, object or name. False
    // (and the name logged by the caller) if it has no such variable of a fitting kind.
    bool set_variable(UObject* owner, const TCHAR* name, double number, UObject* object = nullptr, const FName* fname = nullptr)
    {
        if (!owner) return false;
        FProperty* p = owner->GetPropertyByNameInChain(name);
        if (!p) return false;
        uint8_t* at = reinterpret_cast<uint8_t*>(owner) + p->GetOffset_Internal();
        auto kind = p->GetClass().GetName();
        if (kind == STR("DoubleProperty")) *reinterpret_cast<double*>(at) = number;
        else if (kind == STR("FloatProperty")) *reinterpret_cast<float*>(at) = (float)number;
        else if (kind == STR("IntProperty")) *reinterpret_cast<int32_t*>(at) = (int32_t)number;
        else if (kind == STR("ByteProperty") || kind == STR("EnumProperty")) *at = (uint8_t)number;
        else if (kind == STR("BoolProperty")) return set_bool_property(owner, name, number != 0.0);
        else if (kind == STR("ObjectProperty")) *reinterpret_cast<UObject**>(at) = object;
        else if (kind == STR("NameProperty") && fname) *reinterpret_cast<FName*>(at) = *fname;
        else return false;
        return true;
    }

    // Is the object's class, or one it is built on, named `class_name` (e.g. ModularWeaponBP_C)?
    bool is_a(UObject* object, const TCHAR* class_name)
    {
        if (!object) return false;
        for (UStruct* level = object->GetClassPrivate(); level; level = level->GetSuperStruct())
            if (level->GetName() == class_name) return true;
        return false;
    }

    // Does a list of names (actor Tags / ComponentTags) hold this one?
    bool has_tag(UObject* owner, const TCHAR* list, const TCHAR* tag)
    {
        if (!owner) return false;
        RawArray* tags = owner->GetValuePtrByPropertyNameInChain<RawArray>(list);
        if (!tags || !tags->Data) return false;
        std::wstring want(tag);
        const FName* names = reinterpret_cast<const FName*>(tags->Data);
        for (int32_t i = 0; i < tags->Num; ++i)
            if (names[i].ToString() == want) return true;
        return false;
    }

    // An object or body grab in progress: handed to the game's own latch, checked a few ticks later.
    struct ObjectGrab
    {
        UObject* component = nullptr;
        FName bone;
        hsvr::Vec3 point{0, 0, 0};
        bool character = false;
        double since = -1.0;
        bool fallback_tried = false;
    };
    ObjectGrab g_object_grab[2];
    // Test (hsvr.bring_object): an object kept at the right hand each tick until it is grabbed or 4 s pass.
    UObject* g_bring = nullptr;
    double g_bring_until = -1.0;
    int g_bring_side = 1;
    bool g_bring_weapon = false;   // a shield brought (hsvr.bring_object shield): kept until that hand holds it
    bool g_bring_at_point = false; // kept with g_bring_point (its own frame) at the hand: a rim test
    hsvr::Vec3 g_bring_point;

    void keep_brought_object(UObject* pawn, UObject* mesh)
    {
        if (!g_bring) return;
        const TCHAR* grabbed = g_bring_side == 0 ? STR("Grabbed L") : STR("Grabbed R");
        bool held = bool_property(pawn, grabbed);
        if (g_bring_weapon)
        {
            Call owner_call(g_bring, STR("GetOwner"));
            owner_call.run();
            UObject* owner = owner_call.object_result();
            held = owner && object_property(pawn, g_bring_side == 0 ? STR("Weapon L") : STR("Weapon R")) == owner;
        }
        if (hsvr::now_seconds() > g_bring_until || held)
        {
            HSVR_LOG("Script: hsvr.bring_object: no longer kept at the %s hand (grabbed %d)", g_bring_side == 0 ? "left" : "right", (int)bool_property(pawn, grabbed));
            g_bring = nullptr;
            return;
        }
        hsvr::Vec3 hand = hand_grab_point(mesh, g_bring_side);
        hsvr::Vec3 put = hand + hsvr::Vec3{0, 0, 4.0};
        if (g_bring_at_point) put = hand - component_rotation(g_bring).rotate(g_bring_point);
        Call move(g_bring, STR("K2_SetWorldLocation"));
        move.three(STR("NewLocation"), put.x, put.y, put.z);
        move.flag(STR("bTeleport"), true);
        move.run();
        Call stop(g_bring, STR("SetPhysicsLinearVelocity"));
        stop.run();
        Call spin(g_bring, STR("SetPhysicsAngularVelocityInDegrees"));
        spin.run();
    }

    // Squeezing near something that is not a weapon: a loose physics object (brick, barrel, armour
    // piece, food) or a body (anyone's bone, the dead too). The nearest simulating body within
    // pose_grab_radius_cm of the hand is handed to the game's own grab the way its search does it
    // (grab_pickup.md "Grab button follow-up", UF @523112): Grab Component / Grab Bone set, GrabPoint
    // put on the spot, Stuck Meter 0, then Grab Target Found, and its next tick builds the hand joint
    // with its full handling (unbreakable joints and limb/throat flags on characters, carried armour).
    // If the game has not grabbed it 0.3 s later, a plain object is given to Grab Specific Object
    // (the game's direct grab, which has no special handling); a body is left.
    bool grab_object(int side, UObject* pawn, UObject* mesh, const hsvr::Config& cfg, const hsvr::Vec3& at)
    {
        using namespace hsvr;
        const char* hand = side == 0 ? "left" : "right";
        static UObject* cdo = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetSystemLibrary"));
        if (!cdo) return false;
        static uint8_t types[32];
        for (int i = 0; i < 32; ++i) types[i] = (uint8_t)i;
        Call ov(cdo, STR("SphereOverlapComponents"));
        FProperty* ot = ov.param(STR("ObjectTypes"));
        FProperty* out = ov.param(STR("OutComponents"));
        if (!ot || !out)
        {
            HSVR_LOG("Grab: no SphereOverlapComponents, objects can't be grabbed");
            return false;
        }
        ov.object(STR("WorldContextObject"), pawn);
        ov.three(STR("SpherePos"), at.x, at.y, at.z);
        ov.number(STR("SphereRadius"), cfg.pose_grab_radius_cm);
        RawArray* list = reinterpret_cast<RawArray*>(ov.buffer.data() + ot->GetOffset_Internal());
        list->Data = reinterpret_cast<void**>(types);
        list->Num = list->Max = 32;
        ov.run();
        RawArray* found = reinterpret_cast<RawArray*>(ov.buffer.data() + out->GetOffset_Internal());
        std::vector<UObject*> components;
        for (int32_t i = 0; i < found->Num && found->Data; ++i) components.push_back(reinterpret_cast<UObject*>(found->Data[i]));
        found->Data = nullptr; // the engine's list; left (a few bytes per grab)
        found->Num = found->Max = 0;
        list->Data = nullptr;
        list->Num = list->Max = 0;

        static const FName none_bone = FName(STR("None"), FNAME_Add);
        UObject* best = nullptr;
        FName best_bone = none_bone;
        Vec3 best_point{0, 0, 0};
        double best_d = cfg.pose_grab_radius_cm;
        std::wstring seen;
        for (UObject* c : components)
        {
            if (!c) continue;
            Call owner_call(c, STR("GetOwner"));
            owner_call.run();
            UObject* owner = owner_call.object_result();
            if (!owner || owner == pawn) continue;
            if (is_a(owner, STR("ModularWeaponBP_C"))) continue;
            if (has_tag(c, STR("ComponentTags"), STR("NoGrab")) || has_tag(owner, STR("Tags"), STR("Sheathed"))) continue;
            FName bone = none_bone;
            bool skeletal = c->GetFunctionByNameInChain(STR("FindClosestBone_K2")) != nullptr;
            if (skeletal)
            {
                Call fb(c, STR("FindClosestBone_K2"));
                fb.three(STR("TestLocation"), at.x, at.y, at.z);
                fb.flag(STR("bRequirePhysicsAsset"), true);
                fb.run();
                if (FProperty* rp = fb.param(STR("ReturnValue"))) bone = *reinterpret_cast<FName*>(fb.buffer.data() + rp->GetOffset_Internal());
            }
            Call sim(c, STR("IsSimulatingPhysics"));
            sim.name(STR("BoneName"), bone);
            sim.run();
            FProperty* sp = sim.param(STR("ReturnValue"));
            bool simulating = sp && *(sim.buffer.data() + sp->GetOffset_Internal());
            seen += owner->GetName() + STR(".") + c->GetName() + (skeletal ? STR(":") + bone.ToString() : std::wstring()) + (simulating ? STR(" sim, ") : STR(" still, "));
            if (!simulating) continue;
            Call closest(c, STR("GetClosestPointOnCollision"));
            closest.three(STR("Point"), at.x, at.y, at.z);
            closest.name(STR("BoneName"), bone);
            closest.run();
            double d = closest.number();
            if (d < 0.0 || d > best_d) continue;
            best_d = d;
            best = c;
            best_bone = bone;
            best_point = d > 0.0 ? closest.vec(STR("OutPointOnBody")) : at;
        }
        if (!best)
        {
            HSVR_LOG("Grab: %s grip squeezed, nothing to grab within %.0f cm (overlapping: %ls)", hand, cfg.pose_grab_radius_cm, seen.empty() ? STR("none") : seen.c_str());
            return false;
        }
        Call owner_call(best, STR("GetOwner"));
        owner_call.run();
        UObject* owner = owner_call.object_result();
        bool character = is_a(owner, STR("Willie_BP_C"));
        bool L = side == 0;
        std::wstring missing;
        auto need = [&](bool ok, const TCHAR* n) { if (!ok) missing += std::wstring(n) + STR(", "); };
        bool blocked = bool_property(pawn, L ? STR("Block Grab L") : STR("Block Grab R")) || bool_property(pawn, L ? STR("Hand L Torn Off") : STR("Hand R Torn Off")) ||
                       bool_property(pawn, STR("Swapping weapons"));
        if (blocked) HSVR_LOG("Grab: %s hand: the game blocks grabbing right now (Block Grab / Torn Off / Swapping weapons)", hand);
        need(set_variable(pawn, L ? STR("Grab Component L") : STR("Grab Component R"), 0, best), L ? STR("Grab Component L") : STR("Grab Component R"));
        need(set_variable(pawn, L ? STR("Grab Bone L") : STR("Grab Bone R"), 0, nullptr, &best_bone), L ? STR("Grab Bone L") : STR("Grab Bone R"));
        UObject* grab_point = object_property(pawn, L ? STR("GrabPoint L") : STR("GrabPoint R"));
        need(grab_point != nullptr, L ? STR("GrabPoint L") : STR("GrabPoint R"));
        if (grab_point)
        {
            Call move(grab_point, STR("K2_SetWorldLocation"));
            move.three(STR("NewLocation"), best_point.x, best_point.y, best_point.z);
            move.run();
        }
        need(set_variable(pawn, L ? STR("Stuck Meter L Hand") : STR("Stuck Meter R Hand"), 0.0), L ? STR("Stuck Meter L Hand") : STR("Stuck Meter R Hand"));
        need(set_variable(pawn, L ? STR("Grab L Target Found") : STR("Grab R Target Found"), 1.0), L ? STR("Grab L Target Found") : STR("Grab R Target Found"));
        ObjectGrab& g = g_object_grab[side];
        g.component = best;
        g.bone = best_bone;
        g.point = best_point;
        g.character = character;
        g.since = now_seconds();
        g.fallback_tried = false;
        controllers_buzz(side, 0.4f);
        HSVR_LOG("Grab: %s grip on %ls.%ls bone %ls (%.1f cm from the hand)%ls: handed to the game's latch%ls%ls", hand, owner ? owner->GetName().c_str() : STR("?"),
                 best->GetName().c_str(), best_bone.ToString().c_str(), best_d, character ? STR(", a body") : STR(""), missing.empty() ? STR("") : STR("; missing: "),
                 missing.c_str());
        return true;
    }

    // A few ticks after handing an object to the game's latch: did the game grab it?
    void check_object_grab(int side, UObject* pawn)
    {
        using namespace hsvr;
        ObjectGrab& g = g_object_grab[side];
        if (!g.component || g.since < 0.0 || g.fallback_tried) return;
        double age = now_seconds() - g.since;
        bool L = side == 0;
        bool grabbed = bool_property(pawn, L ? STR("Grabbed L") : STR("Grabbed R"));
        if (grabbed)
        {
            HSVR_LOG("Grab: %s hand: the game grabbed %ls after %.2f s", L ? "left" : "right", g.component->GetName().c_str(), age);
            g.fallback_tried = true;
            return;
        }
        if (age < 0.3) return;
        g.fallback_tried = true;
        set_variable(pawn, L ? STR("Grab L Target Found") : STR("Grab R Target Found"), 0.0);
        if (g.character)
        {
            HSVR_LOG("Grab: %s hand: the game did not take the body in 0.3 s; left", L ? "left" : "right");
            return;
        }
        UFunction* fn = pawn->GetFunctionByNameInChain(L ? STR("Grab Specific Object L H") : STR("Grab Specific Object R H"));
        if (!fn)
        {
            HSVR_LOG("Grab: %s hand: the game did not take it and there is no Grab Specific Object", L ? "left" : "right");
            return;
        }
        Call c(pawn, L ? STR("Grab Specific Object L H") : STR("Grab Specific Object R H"));
        for (FProperty* p : c.fn->ForEachProperty())
            if (p->GetClass().GetName() == STR("ObjectProperty")) *reinterpret_cast<UObject**>(c.buffer.data() + p->GetOffset_Internal()) = g.component;
            else if (p->GetClass().GetName() == STR("StructProperty")) write_three(c.buffer.data() + p->GetOffset_Internal(), p, g.point.x, g.point.y, g.point.z);
        c.run();
        HSVR_LOG("Grab: %s hand: the game did not take it in 0.3 s; Grab Specific Object -> Grabbed %d", L ? "left" : "right",
                 (int)bool_property(pawn, L ? STR("Grabbed L") : STR("Grabbed R")));
    }

    // Lets go of an object or body the game's grab holds in that hand (Release Grab R/L).
    void release_object(int side, UObject* pawn)
    {
        bool L = side == 0;
        g_object_grab[side] = ObjectGrab{};
        if (!bool_property(pawn, L ? STR("Grabbed L") : STR("Grabbed R"))) return;
        Call c(pawn, L ? STR("Release Grab L") : STR("Release Grab R"));
        bool ok = c.run();
        HSVR_LOG("Grab: %s grip let go: %ls (Grabbed now %d)", L ? "left" : "right", ok ? STR("Release Grab called") : STR("no Release Grab function"),
                 (int)bool_property(pawn, L ? STR("Grabbed L") : STR("Grabbed R")));
    }

    // The grab button (Phase 4): squeezing a controller's grip with that hand empty picks up the
    // nearest loose weapon whose body is within pose_grab_radius_cm of the hand; letting go of the
    // grip drops what that hand holds (B&S-style hold-to-hold). A weapon he already holds when the
    // grip is not squeezed (one he spawned with) stays until the next squeeze-and-release.
    // Weapons only for now: objects and bodies need the game's latch tick (grab_pickup.md).
    // ---- Two hands on one weapon ---------------------------------------------------------------
    // B&S rules (Matthew's spec 2026-11-18): the free hand squeezes on a weapon the other hand holds,
    // on its grip or directly on the blade anywhere along it (half-swording, as in real life and the
    // B&S MegaPack). The first hand keeps the game's hold; the weapon then lies along the line from
    // the first hand's grip point to the second's, the first hand sets its roll, and the hold is
    // pose_two_hand_strength times as strong. The second hand's arm is posed onto its spot on the
    // weapon. Its trigger slides it along the zone it is on, as the first hand's does.
    struct TwoHand
    {
        bool active = false;
        int main = 1, second = 0;
        UObject* weapon = nullptr;
        double z = 0.0;            // the second hand's spot along the weapon, cm from the game's grip point
        bool on_blade = false;
        hsvr::Vec3 local{0, 0, 0}; // that spot in BaseMesh's frame (set by the hold each tick)
        hsvr::Vec3 in_hand{0, 0, 0}; // the palm point in the second hand bone's frame (the first's, mirrored)
        bool have_local = false;
        double far_since = -1.0;
        bool sliding = false;
        double grip_type_before = -1.0; // the second hand's grip type before it took hold (its finger pose)
    };
    TwoHand g_two;

    // The second hand's fingers: the game curls a hand's fingers from its grip type (ABP_Willie copies
    // L/R_GripType_Current, @153084; 1-4 play the sword grip, hand_pose.md), which it sets only for a
    // hand it gave a weapon. While the second hand holds, its grip type is the sword grip (2).
    const TCHAR* grip_type_name(int side) { return side == 0 ? STR("L_GripType_Current") : STR("R_GripType_Current"); }
    double read_number_variable(UObject* owner, const TCHAR* name)
    {
        FProperty* p = owner ? owner->GetPropertyByNameInChain(name) : nullptr;
        if (!p) return -1.0;
        uint8_t* at = reinterpret_cast<uint8_t*>(owner) + p->GetOffset_Internal();
        auto kind = p->GetClass().GetName();
        if (kind == STR("IntProperty")) return *reinterpret_cast<int32_t*>(at);
        if (kind == STR("ByteProperty") || kind == STR("EnumProperty")) return *at;
        if (kind == STR("DoubleProperty")) return *reinterpret_cast<double*>(at);
        if (kind == STR("FloatProperty")) return *reinterpret_cast<float*>(at);
        return -1.0;
    }
    double g_pending_slide[2] = {1e9, 1e9}; // a hand-over: where the new hand holds, applied when its hold starts

    void end_two_hand(const char* why)
    {
        if (!g_two.active) return;
        if (g_two.grip_type_before >= 0.0 && g_pose.pawn)
        {
            double was = read_number_variable(g_pose.pawn, grip_type_name(g_two.second));
            bool ok = set_variable(g_pose.pawn, grip_type_name(g_two.second), g_two.grip_type_before);
            HSVR_LOG("Two hands: %s hand's grip type %.0f -> %.0f restored%s (first hand's now %.0f)", g_two.second == 0 ? "left" : "right", was, g_two.grip_type_before,
                     ok ? "" : ": NOT SET", read_number_variable(g_pose.pawn, grip_type_name(g_two.main)));
        }
        HSVR_LOG("Two hands: %s hand lets go of %ls (%s)", g_two.second == 0 ? "left" : "right", g_two.weapon ? g_two.weapon->GetName().c_str() : STR("?"), why);
        g_two = TwoHand{};
    }

    // The zones the second hand may hold, cm from the game's grip point: the grip (the first hand's
    // slide zone) and, past the guard, the blade up to half a hand short of the tip.
    int two_hand_zones(const HoldState& h, const hsvr::Config& cfg, double lo[2], double hi[2])
    {
        int n = 0;
        if (h.zone_hi > h.zone_lo)
        {
            lo[n] = h.zone_lo;
            hi[n] = h.zone_hi;
            ++n;
        }
        double half = cfg.pose_slide_hand_half_cm;
        double blade_lo = h.zone_stop + half + cfg.pose_two_hand_blade_gap_cm, blade_hi = h.zone_tip - half;
        if (h.zone_stop > 0.0 && blade_hi > blade_lo + 2.0)
        {
            lo[n] = blade_lo;
            hi[n] = blade_hi;
            ++n;
        }
        return n;
    }

    // Clamps z into the zone it is in or nearest (within reach); keeps a hand's width from the first
    // hand at `main_z`. Returns the zone's index, or -1 when z is out of reach of every zone.
    int two_hand_place(double& z, double main_z, const double lo[2], const double hi[2], int n, double reach, double half)
    {
        int best = -1;
        double best_d = reach;
        for (int i = 0; i < n; ++i)
        {
            double d = z < lo[i] ? lo[i] - z : (z > hi[i] ? z - hi[i] : 0.0);
            if (d <= best_d)
            {
                best_d = d;
                best = i;
            }
        }
        if (best < 0) return -1;
        z = (std::min)(hi[best], (std::max)(lo[best], z));
        if (std::fabs(z - main_z) < 2.0 * half)
        {
            z = main_z + (z >= main_z ? 2.0 : -2.0) * half;
            if (z < lo[best] || z > hi[best]) return -1;
        }
        return best;
    }

    void find_grip_zone(HoldState& h, UObject* weapon, UObject* base, const hsvr::Config& cfg);

    // The first hand lets go while both hold (B&S): the weapon stays in the second hand, where it holds
    // it. The game moves it into that hand (Set Up Hand Weapon snaps its grip to the hand socket), so
    // it is put back where it was, and the new hold starts at the second hand's spot.
    bool hand_over(UObject* pawn)
    {
        using namespace hsvr;
        if (!g_two.active) return false;
        int to = g_two.second, from = g_two.main;
        UObject* weapon = g_two.weapon;
        UObject* base = weapon ? object_property(weapon, STR("BaseMesh")) : nullptr;
        if (!base) return false;
        double z = g_two.z;
        Vec3 pos = component_location(base);
        Quat rot = component_rotation(base);
        g_two = TwoHand{};
        drop_weapon(from, pawn);
        bool ok = give_hand_weapon(to, pawn, weapon);
        UObject* now = object_property(pawn, to == 0 ? STR("Weapon L") : STR("Weapon R"));
        if (now == weapon)
        {
            set_component_transform(base, pos, rot, true);
            g_pending_slide[to] = z;
        }
        HSVR_LOG("Two hands: %s hand lets go, %ls handed to the %s hand at %.1f cm (%s, %s hand now holds %ls)", from == 0 ? "left" : "right", weapon->GetName().c_str(),
                 to == 0 ? "left" : "right", z, ok ? "set up" : "set-up call missing", to == 0 ? "left" : "right", now ? now->GetName().c_str() : STR("nothing"));
        return now == weapon;
    }

    // A squeeze with an empty hand: onto the weapon the other hand holds, if the hand is on it.
    bool try_two_hand(int side, UObject* pawn, UObject* mesh, const hsvr::Config& cfg)
    {
        using namespace hsvr;
        if (!cfg.pose_two_hand) return false;
        int other = 1 - side;
        UObject* weapon = object_property(pawn, other == 0 ? STR("Weapon L") : STR("Weapon R"));
        HoldState& h = g_hold[other];
        if (!weapon || h.weapon != weapon || !h.have_last || is_fist(weapon) || h.forearm) return false;
        UObject* base = object_property(weapon, STR("BaseMesh"));
        if (!base) return false;
        double game_z = h.pivot.z - h.slide;
        if (!h.zone_known)
        {
            Vec3 keep = h.pivot;
            h.pivot.z = game_z;
            find_grip_zone(h, weapon, base, cfg);
            h.pivot = keep;
        }
        Vec3 bp = component_location(base);
        Quat bq = component_rotation(base);
        Vec3 at = hand_grab_point(mesh, side);
        Vec3 L = bq.inverse().rotate(at - bp);
        double radial = std::hypot(L.x - h.pivot.x, L.y - h.pivot.y);
        double want = L.z - game_z;
        if (radial > cfg.pose_grab_radius_cm)
        {
            HSVR_LOG("Two hands: %s grip squeezed %.0f cm off the axis of %ls (%.0f cm along it), not on it", side == 0 ? "left" : "right", radial,
                     weapon->GetName().c_str(), want);
            return false;
        }
        double lo[2], hi[2];
        int n = two_hand_zones(h, cfg, lo, hi);
        double z = want;
        int zone = two_hand_place(z, h.slide, lo, hi, n, cfg.pose_two_hand_reach_cm, cfg.pose_slide_hand_half_cm);
        if (zone < 0)
        {
            HSVR_LOG("Two hands: %s grip squeezed on %ls at %.1f cm, outside its zones (%d zones: %.1f..%.1f / %.1f..%.1f; first hand at %.1f)",
                     side == 0 ? "left" : "right", weapon->GetName().c_str(), want, n, n > 0 ? lo[0] : 0.0, n > 0 ? hi[0] : 0.0, n > 1 ? lo[1] : 0.0,
                     n > 1 ? hi[1] : 0.0, h.slide);
            return false;
        }
        g_two = TwoHand{};
        g_two.active = true;
        g_two.main = other;
        g_two.second = side;
        g_two.weapon = weapon;
        g_two.z = z;
        g_two.on_blade = lo[zone] > h.zone_stop && h.zone_stop > 0.0;
        // Willie's hands mirror across the hand bone's X (kick test 15: a weapon in the left hand sits
        // at the right's place with X negated).
        g_two.in_hand = Vec3{-h.in_hand.x, h.in_hand.y, h.in_hand.z};
        if (cfg.pose_two_hand_curl)
        {
            g_two.grip_type_before = read_number_variable(pawn, grip_type_name(side));
            bool ok = set_variable(pawn, grip_type_name(side), 2.0);
            HSVR_LOG("Two hands: %s hand's grip type %.0f -> 2 (sword grip fingers)%s", side == 0 ? "left" : "right", g_two.grip_type_before, ok ? "" : ": NOT SET");
            if (!ok) g_two.grip_type_before = -1.0;
        }
        controllers_buzz(side, 0.4f);
        HSVR_LOG("Two hands: %s hand takes %ls on the %s at %.1f cm (squeezed at %.1f, %.1f cm off the axis; first hand at %.1f)", side == 0 ? "left" : "right",
                 weapon->GetName().c_str(), g_two.on_blade ? "blade" : "grip", z, want, radial, h.slide);
        return true;
    }

    // The second hand's arm: its palm on its spot on the weapon, and the hand locked onto the weapon
    // the way the first hand holds it, mirrored (playtest 2026-11-18: following the controller's turn,
    // it twisted and clipped through the handle). Willie's hands mirror across the hand bone's X and
    // the weapon's Y (kick test 15: a weapon in the left hand has X and Z mirrored in the hand frame,
    // so Y flips), so the second hand's weapon frame is M_hand * R_first * M_weapon.
    bool two_hand_pose(int side, hsvr::Vec3& target, hsvr::Quat& hand_rot)
    {
        using namespace hsvr;
        if (!g_two.active) return false;
        if (side == g_two.main)
        {
            // The first hand too sits on the weapon as it holds it while both hands hold: the weapon
            // follows the line between your hands, not the first controller's turn.
            if (!hsvr::config().pose_two_hand_lock_rotation) return false;
            const HoldState& h = g_hold[side];
            if (h.weapon != g_two.weapon || !h.have_last) return false;
            UObject* b = object_property(g_two.weapon, STR("BaseMesh"));
            if (!b) return false;
            Quat wr = component_rotation(b);
            hand_rot = (wr * (h.f2 * h.f1.inverse()).normalized().inverse()).normalized();
            target = component_location(b) + wr.rotate(h.pivot) - hand_rot.rotate(h.in_hand);
            return true;
        }
        if (side != g_two.second) return false;
        if (!g_pose.pawn || object_property(g_pose.pawn, g_two.main == 0 ? STR("Weapon L") : STR("Weapon R")) != g_two.weapon)
        {
            end_two_hand("the first hand no longer holds it");
            return false;
        }
        if (!g_two.have_local) return false;
        UObject* base = object_property(g_two.weapon, STR("BaseMesh"));
        if (!base) return false;
        Quat weapon_rot = component_rotation(base);
        const HoldState& first = g_hold[g_two.main];
        if (hsvr::config().pose_two_hand_lock_rotation)
        {
            Quat r = (first.f2 * first.f1.inverse()).normalized(); // the weapon in the first hand's frame
            Vec3 x = r.rotate(Vec3{1, 0, 0}), y = r.rotate(Vec3{0, 1, 0}), z = r.rotate(Vec3{0, 0, 1});
            // The mirror plane on the weapon: the one through its axis whose normal is the palm's
            // (pose_two_hand_mirror: which weapon axis to flip, 0 = X, 1 = Y; Y from kick test 15).
            int flip = hsvr::config().pose_two_hand_mirror;
            double fx = flip == 0 ? -1.0 : 1.0, fy = flip == 1 ? -1.0 : 1.0;
            x = x * fx;
            y = y * fy;
            Vec3 mx{-x.x, x.y, x.z}, my{-y.x, y.y, y.z}, mz{-z.x, z.y, z.z};
            Quat mirrored = quat_from_axes(mx, my, mz).normalized();
            hand_rot = (weapon_rot * mirrored.inverse()).normalized();
        }
        Vec3 spot = component_location(base) + weapon_rot.rotate(g_two.local);
        target = spot - hand_rot.rotate(g_two.in_hand);
        return true;
    }

    // ---- Fists ---------------------------------------------------------------------------------
    // The game punches with an invisible weapon, Weapon_Fists_C (a ModularWeaponBP child: a 13 cm
    // flesh sphere of 0.01 kg, grip type 0), which Willie's tick puts in an empty hand once the
    // mouse's "Aim Swing Speed" reaches 30 and takes away when the hand grabs, falls or he is out
    // (fists_eating.md, UF @556975-@560200). VR never moves that aim speed, so the mod puts it there
    // itself, the game's way (Set Up Hand Weapon(Weapon_Fists_C, none, not dropped, don't destroy,
    // empty passport, don't save) with "Fist R/L Spawned" set, so the game's own removal still
    // works): while you squeeze an empty grip, that hand is a fist. Damage is the game's blunt hit
    // from the fist's speed; the hold moves it with your hand like any weapon.
    UClass* g_fist_class = nullptr;
    bool g_fist_looked = false;

    bool is_fist(UObject* weapon) { return weapon && is_a(weapon, STR("Weapon_Fists_C")); }

    UClass* fist_class(const hsvr::Config& cfg)
    {
        if (g_fist_class) return g_fist_class;
        {
            // An empty ini value (written by an earlier build) means the game's own path.
            std::string chosen = cfg.pose_fist_class.empty() ? std::string("/Game/Assets/Weapons/Blueprints/Built_Weapons/Weapon_Fists.Weapon_Fists_C") : cfg.pose_fist_class;
            std::wstring path(chosen.begin(), chosen.end());
            g_fist_class = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, path.c_str());
            if (!g_fist_class)
            {
                std::wstring asset = path;
                if (asset.size() > 2 && asset.substr(asset.size() - 2) == STR("_C")) asset.resize(asset.size() - 2);
                load_asset_object(asset);
                g_fist_class = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, path.c_str());
            }
        }
        if (!g_fist_class)
        {
            // Already loaded by the game: any Blueprint class of that name.
            std::vector<UObject*> classes;
            UObjectGlobals::FindAllOf(STR("BlueprintGeneratedClass"), classes);
            for (UObject* c : classes)
                if (c && c->GetName() == STR("Weapon_Fists_C"))
                {
                    g_fist_class = static_cast<UClass*>(c);
                    break;
                }
        }
        if (!g_fist_looked)
        {
            HSVR_LOG("Fist: class %ls", g_fist_class ? g_fist_class->GetFullName().c_str() : STR("NOT FOUND (set pose_fist_class to its object path)"));
            g_fist_looked = true;
        }
        return g_fist_class;
    }

    // Set Up Right/Left Hand Weapon with a class (or none), and its yes/no switches by name.
    bool set_up_hand(int side, UObject* pawn, UClass* cls, bool dropped, bool destroy_previous, bool dont_save)
    {
        const TCHAR* name = side == 0 ? STR("Set Up Left Hand Weapon") : STR("Set Up Right Hand Weapon");
        UFunction* fn = pawn ? pawn->GetFunctionByNameInChain(name) : nullptr;
        if (!fn) return false;
        // Sized from the function. The fixed 4096-byte static block this replaced was overrun: the
        // hand position written for the fist landed on the grab button's state (fist-eat test 4).
        std::vector<uint8_t> block = param_block(fn);
        uint8_t* parameters = block.data();
        bool class_set = false;
        static bool sized_logged = false;
        if (!sized_logged)
        {
            sized_logged = true;
            HSVR_LOG("Fist: %ls takes a %zu-byte parameter block", name, block.size() - 64);
        }
        UObject* mesh = object_property(pawn, STR("Mesh"));
        hsvr::Vec3 hand = mesh ? hand_grab_point(mesh, side) : hsvr::Vec3{0, 0, 0};
        for (FProperty* p : fn->ForEachProperty())
        {
            std::wstring n = p->GetName();
            auto kind = p->GetClass().GetName();
            if (kind == STR("ClassProperty") && !class_set)
            {
                *reinterpret_cast<UClass**>(parameters + p->GetOffset_Internal()) = cls;
                class_set = true;
            }
            if (kind == STR("BoolProperty"))
            {
                bool value = n.find(STR("Dropped")) != std::wstring::npos   ? dropped
                             : n.find(STR("Destroy")) != std::wstring::npos ? destroy_previous
                             : n.find(STR("Save")) != std::wstring::npos    ? dont_save
                                                                            : false;
                auto* flag = static_cast<FBoolProperty*>(p);
                if (value) parameters[p->GetOffset_Internal() + flag->GetByteOffset()] |= flag->GetFieldMask();
            }
            if (kind == STR("StructProperty"))
                if (UScriptStruct* type = struct_type(p))
                {
                    uint8_t* at = parameters + p->GetOffset_Internal();
                    if (FProperty* r = struct_member(type, STR("Rotation")))
                        if (FProperty* w = struct_member(struct_type(r), STR("W")))
                        {
                            uint8_t* wat = at + r->GetOffset_Internal() + w->GetOffset_Internal();
                            if (w->GetClass().GetName() == STR("DoubleProperty")) *reinterpret_cast<double*>(wat) = 1.0;
                            else *reinterpret_cast<float*>(wat) = 1.0f;
                        }
                    if (FProperty* t = struct_member(type, STR("Translation"))) write_three(at + t->GetOffset_Internal(), t, hand.x, hand.y, hand.z);
                    if (FProperty* sc = struct_member(type, STR("Scale3D"))) write_three(at + sc->GetOffset_Internal(), sc, 1.0, 1.0, 1.0);
                }
        }
        pawn->ProcessEvent(fn, parameters);
        g_hold[side] = HoldState{};
        return true;
    }

    const TCHAR* fist_spawned_name(int side) { return side == 0 ? STR("Fist L Spawned") : STR("Fist R Spawned"); }
    double g_fist_made_at[2] = {-10.0, -10.0};

    // For half a second after a fist is made, what the game's fist rules read, each tick (the first
    // fist of a run went within one tick, fist test 5).
    void fist_trace(UObject* pawn)
    {
        for (int side = 0; side < 2; ++side)
        {
            double age = hsvr::now_seconds() - g_fist_made_at[side];
            if (age < 0.0 || age > 0.5) continue;
            bool r = side == 1;
            UObject* w = object_property(pawn, r ? STR("Weapon R") : STR("Weapon L"));
            HSVR_LOG("Fist trace %.3f s: %s weapon %ls, Fist Spawned %d, Grabbed %d, Grab Target Found %d, Hand Active %d, GripType_Current %.0f, Free Fall Rate %.2f, Consciousness %.0f",
                     age, r ? "right" : "left", w ? w->GetName().c_str() : STR("none"), (int)bool_property(pawn, fist_spawned_name(side)),
                     (int)bool_property(pawn, r ? STR("Grabbed R") : STR("Grabbed L")), (int)bool_property(pawn, r ? STR("Grab R Target Found") : STR("Grab L Target Found")),
                     (int)bool_property(pawn, r ? STR("R Hand Active") : STR("L Hand Active")), read_number_variable(pawn, r ? STR("R_GripType_Current") : STR("L_GripType_Current")),
                     read_number_variable(pawn, STR("Free Fall Rate")), read_number_variable(pawn, STR("Consciousness")));
        }
    }

    bool make_fist(int side, UObject* pawn, const hsvr::Config& cfg)
    {
        if (!cfg.pose_fists) return false;
        UClass* cls = fist_class(cfg);
        if (!cls) return false;
        if (object_property(pawn, side == 0 ? STR("Weapon L") : STR("Weapon R"))) return false;
        set_up_hand(side, pawn, cls, false, false, true);
        UObject* now = object_property(pawn, side == 0 ? STR("Weapon L") : STR("Weapon R"));
        bool ok = is_fist(now);
        if (cfg.pose_fist_mark_spawned) set_bool_property(pawn, fist_spawned_name(side), ok);
        HSVR_LOG("Fist: %s hand %s", side == 0 ? "left" : "right", ok ? "is a fist" : "could not be made a fist");
        if (ok) g_fist_made_at[side] = hsvr::now_seconds();
        return ok;
    }

    void remove_fist(int side, UObject* pawn, const char* why)
    {
        UObject* mine = object_property(pawn, side == 0 ? STR("Weapon L") : STR("Weapon R"));
        if (!is_fist(mine)) return;
        set_up_hand(side, pawn, nullptr, false, true, true);
        // The game destroys a fist on any set-up but leaves "Fist Spawned" set (fists_eating.md).
        set_bool_property(pawn, fist_spawned_name(side), false);
        HSVR_LOG("Fist: %s hand opens (%s)", side == 0 ? "left" : "right", why);
    }

    // ---- Eating and drinking -------------------------------------------------------------------
    // Eating (fists_eating.md): the game's "Bite Event" looks for food at his mouth (a 20 cm sphere
    // trace from VoiceScene + Right x10..15 during its 1 s timeline) and bites it; the mod calls it
    // once each time a grabbed food item (BP_Food_Master_C) comes within pose_eat_cm of that point.
    // Drinking: the game's "Drink From Bottle"(bottle), or "Event Gulp" once "Liquid In Mouth" is
    // 0.5 or more, at most once a second, while a held bottle (BP_Weapon_Consumable_Bottle*) is at
    // the mouth with its neck tipped below level.
    bool class_name_starts(UObject* object, const TCHAR* prefix)
    {
        if (!object) return false;
        std::wstring want = prefix;
        for (UStruct* level = object->GetClassPrivate(); level; level = level->GetSuperStruct())
            if (level->GetName().rfind(want, 0) == 0) return true;
        return false;
    }

    // The game's bite trace starts at VoiceScene + Right x10..15 (UF @753233). On his character the
    // component (and its variable) is "Voice Scene", with a space (fist-eat test 2). Read from the
    // variable every call (no cached pointer that could outlive it), and only used if it really is a
    // scene component of his; without it, the head bone of his body 8 cm forward (logged once).
    UObject* find_voice_scene(UObject* pawn)
    {
        UObject* found = nullptr;
        for (const TCHAR* name : {STR("Voice Scene"), STR("VoiceScene")})
        {
            FProperty* p = pawn ? pawn->GetPropertyByNameInChain(name) : nullptr;
            if (!p || p->GetClass().GetName() != STR("ObjectProperty")) continue;
            UObject* c = *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(pawn) + p->GetOffset_Internal());
            if (c && is_a(c, STR("SceneComponent")) && c->GetOuterPrivate() == pawn)
            {
                found = c;
                break;
            }
        }
        static UObject* logged_pawn = nullptr;
        if (pawn != logged_pawn)
        {
            logged_pawn = pawn;
            HSVR_LOG("Eat: mouth point from %ls", found ? found->GetFullName().c_str() : STR("his head bone (no Voice Scene component found)"));
        }
        return found;
    }

    bool mouth_point(UObject* pawn, hsvr::Vec3& out)
    {
        if (!pawn) return false;
        Call right(pawn, STR("GetActorRightVector"));
        right.run();
        if (UObject* voice = find_voice_scene(pawn))
        {
            out = component_location(voice) + right.vec() * 12.5;
            return true;
        }
        UObject* mesh = object_property(pawn, STR("Mesh"));
        if (!mesh) return false;
        Call fwd(pawn, STR("GetActorForwardVector"));
        fwd.run();
        out = socket_location(mesh, bone_fname("Head")) + fwd.vec() * 8.0;
        return true;
    }

    void eat_and_drink(UObject* pawn, const hsvr::Config& cfg)
    {
        using namespace hsvr;
        if (!cfg.pose_eat || !pawn) return;
        Vec3 mouth;
        if (!mouth_point(pawn, mouth)) return;
        static bool at_mouth[2] = {false, false};
        static double last_drink = -10.0, last_bite = -10.0;
        double now = now_seconds();
        for (int side = 0; side < 2; ++side)
        {
            // Food in a grabbing hand.
            UObject* food = nullptr;
            if (bool_property(pawn, side == 0 ? STR("Grabbed L") : STR("Grabbed R")))
            {
                UObject* comp = object_property(pawn, side == 0 ? STR("Grab Component L") : STR("Grab Component R"));
                if (comp)
                {
                    Call owner_call(comp, STR("GetOwner"));
                    owner_call.run();
                    UObject* owner = owner_call.object_result();
                    if (is_a(owner, STR("BP_Food_Master_C")) || is_a(comp, STR("BP_Food_Master_C"))) food = comp;
                }
            }
            UObject* bottle = object_property(pawn, side == 0 ? STR("Weapon L") : STR("Weapon R"));
            if (!class_name_starts(bottle, STR("BP_Weapon_Consumable_Bottle"))) bottle = nullptr;
            UObject* item = food ? food : (bottle ? object_property(bottle, STR("BaseMesh")) : nullptr);
            if (!item)
            {
                at_mouth[side] = false;
                continue;
            }
            Call closest(item, STR("GetClosestPointOnCollision"));
            closest.three(STR("Point"), mouth.x, mouth.y, mouth.z);
            closest.run();
            double d = closest.number();
            if (d < 0.0) d = length(component_location(item) - mouth);
            bool close = d <= (at_mouth[side] ? cfg.pose_eat_cm + 10.0 : cfg.pose_eat_cm);
            if (food)
            {
                if (close && !at_mouth[side] && now - last_bite > 1.0)
                {
                    Call bite(pawn, STR("Bite Event"));
                    bool ok = bite.run();
                    last_bite = now;
                    HSVR_LOG("Eat: %s hand's food at your mouth (%.0f cm): %s", side == 0 ? "left" : "right", d, ok ? "Bite Event called" : "no Bite Event");
                }
            }
            else if (close && now - last_drink >= cfg.pose_drink_sip_s)
            {
                Quat r = component_rotation(item);
                double tip = dot(r.rotate(Vec3{0, 0, 1}), Vec3{0, 0, 1});
                if (tip < cfg.pose_drink_tip)
                {
                    last_drink = now;
                    double in_mouth = read_number_variable(pawn, STR("Liquid In Mouth"));
                    double bottle_before = read_number_variable(bottle, STR("Liquid Amount In Bottle"));
                    double body_before = read_number_variable(pawn, STR("Body Liquid"));
                    bool ok;
                    const char* what;
                    if (in_mouth >= 0.5)
                    {
                        Call gulp(pawn, STR("Event Gulp"));
                        ok = gulp.run();
                        what = "Event Gulp";
                    }
                    else
                    {
                        Call drink(pawn, STR("Drink From Bottle"));
                        // The bottle goes in the parameter named for it: a function's property list
                        // also holds its locals, so "the first object" was not the parameter (drink
                        // test 2: the call did nothing).
                        static bool listed = false;
                        if (drink.fn)
                        {
                            FProperty* target = nullptr;
                            std::wstring names;
                            for (FProperty* p : drink.fn->ForEachProperty())
                            {
                                names += p->GetName() + STR(" (") + p->GetClass().GetName() + STR("), ");
                                if (!target && p->GetClass().GetName() == STR("ObjectProperty") && p->GetName().find(STR("Bottle")) != std::wstring::npos) target = p;
                            }
                            if (!listed)
                            {
                                listed = true;
                                HSVR_LOG("Drink: Drink From Bottle has %ls; the bottle goes in %ls", names.c_str(), target ? target->GetName().c_str() : STR("NOTHING (no object named Bottle)"));
                            }
                            if (target) *reinterpret_cast<UObject**>(drink.buffer.data() + target->GetOffset_Internal()) = bottle;
                        }
                        ok = drink.run();
                        what = "Drink From Bottle";
                    }
                    HSVR_LOG("Drink: %s hand's %ls at your mouth (%.0f cm, neck %.2f): %s %s (liquid in mouth %.3f -> %.3f, in the bottle %.2f -> %.2f, body liquid %.4f -> %.4f)",
                             side == 0 ? "left" : "right", bottle->GetName().c_str(), d, tip, what, ok ? "called" : "MISSING", in_mouth,
                             read_number_variable(pawn, STR("Liquid In Mouth")), bottle_before, read_number_variable(bottle, STR("Liquid Amount In Bottle")), body_before,
                             read_number_variable(pawn, STR("Body Liquid")));
                }
            }
            // A bottle taken from the mouth with liquid in it: he swallows (the game's own Gulp
            // needs 0.5 in the mouth, which short sips never reach: fist-eat test 4).
            if (bottle && at_mouth[side] && !close)
            {
                double in_mouth = read_number_variable(pawn, STR("Liquid In Mouth"));
                if (in_mouth > 0.01)
                {
                    Call gulp(pawn, STR("Event Gulp"));
                    bool ok = gulp.run();
                    HSVR_LOG("Drink: bottle away from your mouth, swallowing (liquid in mouth %.2f): Event Gulp %s", in_mouth, ok ? "called" : "MISSING");
                }
            }
            at_mouth[side] = close;
        }
    }

    // A weapon's real shape. Its BaseMesh is a 2 cm grip dummy (SM_Grip_Dummy); the blade, grip, guard,
    // pommel or shield plate are child static meshes welded into its body, and a closest-point query
    // on the root skips welded shapes (PhysInterface_Chaos GetSquaredDistanceToBody, IsShapeBoundToBody)
    // while one on a welded child answers with that child's own shapes (collision dump, 2026-11-18).
    // So every static mesh part of the weapon is asked, and the nearest answer wins. The parts list
    // (by outer) is rebuilt at most every 0.25 s.
    std::unordered_map<UObject*, std::vector<UObject*>> g_weapon_parts;
    double g_weapon_parts_time = -10.0;

    double closest_on_weapon(UObject* weapon, const hsvr::Vec3& point, hsvr::Vec3* out = nullptr)
    {
        if (!weapon) return -1.0;
        double now = hsvr::now_seconds();
        if (now - g_weapon_parts_time > 0.25)
        {
            g_weapon_parts_time = now;
            g_weapon_parts.clear();
            std::vector<UObject*> comps;
            UObjectGlobals::FindAllOf(STR("StaticMeshComponent"), comps);
            for (UObject* c : comps)
                if (c && c->GetOuterPrivate()) g_weapon_parts[c->GetOuterPrivate()].push_back(c);
        }
        double best = -1.0;
        auto it = g_weapon_parts.find(weapon);
        if (it == g_weapon_parts.end()) return -1.0;
        for (UObject* c : it->second)
        {
            Call q(c, STR("GetClosestPointOnCollision"));
            if (!q.fn) continue;
            q.three(STR("Point"), point.x, point.y, point.z);
            q.run();
            double d = q.number();
            if (d < 0.0 || (best >= 0.0 && d >= best)) continue;
            best = d;
            if (out) *out = q.vec(STR("OutPointOnBody"));
        }
        return best;
    }

    // Every strap and handle you can see on a shield is a grab point (Matthew 2026-11-18: "if i can
    // see more then one grab point then i want to be able to grab it"). Measured from the shields'
    // own meshes (HalfSwordDecomp defaults/shield_grab_points_measured.csv: the cooked StaticMesh
    // LOD0, each fitting a separate piece of the mesh): its centre line in the BaseMesh frame (cm),
    // and which hands the game itself puts on it (its own grip: "Grip R/L Hand Default" and the arm
    // pose it snaps to). Flush fittings with no room for a hand are left out.
    struct ShieldFitting
    {
        const wchar_t* shield; // class name without _C
        const char* name;
        double cx, cy, cz, ax, ay, az, length;
        bool game_r, game_l; // part of the game's own grip for that hand
    };
    const ShieldFitting k_shield_fittings[] = {
        {L"Shield_Buckler", "handle", 0.00, 0.00, -0.04, -0.000, -0.006, -1.000, 33.4, true, true},
        {L"Shield_Buckler_2", "handle", 0.01, 0.01, 0.05, 0.000, -0.002, -1.000, 23.6, true, true},
        {L"Shield_Buckler_3", "handle", -0.01, 0.00, 0.07, -0.000, -0.000, -1.000, 26.6, true, true},
        {L"Shield_Tagre", "big strap", 6.91, -6.56, -7.52, 0.643, 0.000, 0.766, 19.3, true, true},
        {L"Shield_Tagre", "narrow strap", -6.00, -4.54, 0.54, 0.735, 0.000, 0.678, 19.6, true, false},
        {L"Shield_Tagre", "upright strap", -6.12, -4.06, 0.61, 0.164, 0.000, 0.987, 19.4, true, true},
        {L"Shield_Pavise_Light", "upright handle", 0.18, 1.43, -0.16, -0.014, 0.000, 1.000, 21.1, false, false},
        {L"Shield_Pavise_Light", "top handle", 0.12, -3.03, 15.94, -1.000, 0.000, 0.022, 27.5, true, false},
        {L"Shield_Pavise_Light", "lower strap", 0.38, -7.42, -8.74, 1.000, 0.000, 0.004, 26.0, false, false},
        {L"Shield_Pavise_Heavy", "centre handle", -0.09, 0.60, -3.33, 0.008, 0.000, 1.000, 18.9, true, false},
        {L"Shield_Pavise_Heavy", "centre handle's right arm", 5.47, 1.52, 9.55, 0.922, 0.000, 0.386, 14.0, false, false},
        {L"Shield_Pavise_Heavy", "centre handle's left arm", -5.53, 1.53, 9.57, -0.929, 0.000, 0.370, 13.9, false, false},
        {L"Shield_Pavise_Heavy", "side bar", -16.42, -2.18, 14.36, 0.018, 0.000, 1.000, 25.9, false, true},
        {L"Shield_Pavise_Heavy", "long strap", 13.80, -6.77, 12.38, -0.007, 0.000, 1.000, 39.6, false, true},
        {L"Shield_Pavise_Tower", "crossed strap's middle", -0.21, -0.60, -0.31, 0.008, 0.000, 1.000, 20.9, true, true},
        {L"Shield_Pavise_Tower", "crossed strap, upper right", 5.66, 1.34, 12.63, 0.808, 0.000, 0.589, 15.2, false, false},
        {L"Shield_Pavise_Tower", "crossed strap, upper left", -5.54, 1.64, 12.43, -0.797, 0.000, 0.604, 13.1, false, false},
        {L"Shield_Pavise_Tower", "crossed strap, lower right", 5.82, 1.26, -11.79, -0.833, 0.000, 0.553, 14.6, false, false},
        {L"Shield_Pavise_Tower", "crossed strap, lower left", -5.71, 1.47, -11.77, 0.880, 0.000, 0.474, 14.2, false, false},
        {L"Shield_Pavise_Tower", "top rod", 0.31, 2.67, 51.44, 1.000, 0.000, 0.000, 13.2, false, false},
        {L"Shield_Pavise_Tower", "bottom bar", 0.18, 0.69, -48.85, 1.000, 0.000, -0.000, 25.5, false, false},
        {L"Shield_BossGrip", "handle", -0.00, -0.39, 0.70, -0.001, 0.000, 1.000, 50.1, true, true},
    };

    // A shield taken with the grip button. The hand point, in the shield's frame from before the
    // game's set-up moves it, is matched against its fittings: the nearest within
    // pose_shield_point_cm wins. One of the game's own for that hand keeps the game's grip (a strap
    // shield then sits on the forearm as the game has it). Any other fitting is held in the hand where
    // the hand closed on it, and anywhere else on the shield (the rim, an edge, the face) the hand
    // holds it where it touched; both keep the shield turned as it was in the hand. The hold places
    // it there on its first tick (the game's set-up snaps it to its own grip a tick later).
    void shield_grip(int side, UObject* mesh, UObject* shield, UObject* base, const hsvr::Vec3& w0_pos, const hsvr::Quat& w0_rot, const hsvr::Vec3& palm,
                     const hsvr::Vec3& touch, const hsvr::Config& cfg)
    {
        using namespace hsvr;
        const char* hand = side == 0 ? "left" : "right";
        std::wstring cls = shield->GetClassPrivate() ? shield->GetClassPrivate()->GetName() : std::wstring();
        if (cls.size() > 2 && cls.substr(cls.size() - 2) == STR("_C")) cls.resize(cls.size() - 2);
        Vec3 local = w0_rot.inverse().rotate(palm - w0_pos);
        const ShieldFitting* best = nullptr;
        double best_d = cfg.pose_shield_point_cm;
        Vec3 best_on;
        int known = 0;
        for (const ShieldFitting& f : k_shield_fittings)
        {
            if (cls != f.shield) continue;
            ++known;
            Vec3 c{f.cx, f.cy, f.cz}, a = normalize(Vec3{f.ax, f.ay, f.az});
            double t = (std::max)(-f.length * 0.5, (std::min)(f.length * 0.5, dot(local - c, a)));
            Vec3 on = c + a * t;
            double d = length(local - on);
            if (d <= best_d)
            {
                best_d = d;
                best = &f;
                best_on = on;
            }
        }
        if (best && (side == 1 ? best->game_r : best->game_l))
        {
            HSVR_LOG("Shield: %s hand took %ls by its %s (%.1f cm), the game's own grip", hand, shield->GetName().c_str(), best->name, best_d);
            return;
        }
        if (!known && length(touch - palm) <= cfg.pose_shield_point_cm)
        {
            HSVR_LOG("Shield: %s hand took %ls, a shield with no measured grab points: the game's own grip", hand, cls.c_str());
            return;
        }
        Vec3 a_pos = socket_location(mesh, bone_fname(side == 0 ? "hand_l" : "hand_r"));
        Quat a_rot = socket_rotation(mesh, bone_fname(side == 0 ? "hand_l" : "hand_r"));
        FreeGrip g;
        g.set = true;
        g.place = true;
        g.pivot = best ? best_on : w0_rot.inverse().rotate(touch - w0_pos);
        g.in_anchor = a_rot.inverse().rotate(palm - a_pos);
        g.rel = (a_rot.inverse() * w0_rot).normalized();
        g.what = best ? best->name : "edge where the hand touched it";
        g_pending_free[side] = g;
        set_component_transform(base, w0_pos, w0_rot, true);
        HSVR_LOG("Shield: %s hand took %ls by the %s (%.1f cm from the hand)", hand, shield->GetName().c_str(), g.what.c_str(),
                 best ? best_d : length(touch - palm));
    }

    void grab_button(const hsvr::Config& cfg, const hsvr::ControllerState& state, bool menu_up)
    {
        using namespace hsvr;
        static bool held[2] = {false, false};
        UObject* pawn = nullptr;
        UObject* mesh = find_pawn_mesh(&pawn);
        bool faked = g_fake_squeeze[0].load() >= 0.0f || g_fake_squeeze[1].load() >= 0.0f;
        // A new character starts with open hands. A tick where the grab cannot run (no focus, the body
        // not posed this tick) keeps what each grip was doing: resetting it there read as a release and
        // a new squeeze on the next tick (fist-eat test 3: a fist made and opened every tick).
        static UObject* last_pawn = nullptr;
        if (pawn != last_pawn)
        {
            last_pawn = pawn;
            held[0] = held[1] = false;
        }
        // Not marked as the game's fist (pose_fist_mark_spawned 0: its one-shot removal fired at once,
        // "R Hand Active" being 0 in VR, fist test 6), so its other reasons are the mod's: knocked
        // out or falling, the fist opens (UF @556985).
        if (pawn && !cfg.pose_fist_mark_spawned)
            for (int side = 0; side < 2; ++side)
                if (is_fist(object_property(pawn, side == 0 ? STR("Weapon L") : STR("Weapon R"))))
                {
                    double conscious = read_number_variable(pawn, STR("Consciousness"));
                    double falling = read_number_variable(pawn, STR("Free Fall Rate"));
                    if ((conscious >= 0.0 && conscious <= 0.0) || falling > 0.5) remove_fist(side, pawn, conscious <= 0.0 ? "knocked out" : "falling");
                }
        if (!cfg.pose_grab_button || !pawn || !mesh || (!faked && (!state.active || !state.focused)) || !g_pose_active.load())
        {
            if (held[0] || held[1])
                HSVR_LOG_EVERY(2.0, "Grab: skipped a tick with a grip held (pawn %d, mesh %d, faked %d, controllers active %d focused %d, body posed %d)", pawn != nullptr,
                               mesh != nullptr, (int)faked, (int)state.active, (int)state.focused, (int)g_pose_active.load());
            return;
        }
        keep_brought_object(pawn, mesh);
        fist_trace(pawn);
        for (int side = 0; side < 2; ++side) check_object_grab(side, pawn);
        eat_and_drink(pawn, cfg);
        for (int side = 0; side < 2; ++side)
        {
            float squeeze = g_fake_squeeze[side].load() >= 0.0f ? g_fake_squeeze[side].load() : state.hand[side].squeeze;
            bool now_held = held[side] ? squeeze > 0.35f : squeeze > 0.6f;
            if (now_held == held[side]) continue;
            held[side] = now_held;
            const char* hand = side == 0 ? "left" : "right";
            {
                static uint64_t calls = 0;
                static double last_edge[2] = {-1.0, -1.0};
                ++calls;
                double t = now_seconds();
                // Two edges within 50 ms are not a hand: say what the grip read (fist flicker hunt).
                if (t - last_edge[side] < 0.05)
                    HSVR_LOG("Grab: %s grip changed again after %.3f s (now %s, squeeze %.2f, fake %.2f, call %llu, pawn %p)", hand, t - last_edge[side],
                             now_held ? "held" : "open", squeeze, g_fake_squeeze[side].load(), (unsigned long long)calls, (void*)pawn);
                last_edge[side] = t;
            }
            UObject* mine = object_property(pawn, side == 0 ? STR("Weapon L") : STR("Weapon R"));
            if (is_fist(mine))
            {
                // A squeeze opens the fist before it looks for something to take.
                remove_fist(side, pawn, now_held ? "grabbing" : "grip released");
                mine = nullptr;
                if (!now_held) continue;
            }
            if (!now_held)
            {
                if (g_two.active && side == g_two.second)
                {
                    end_two_hand("grip released");
                    continue;
                }
                if (g_two.active && side == g_two.main)
                {
                    if (cfg.pose_grab_release_drops && hand_over(pawn)) continue;
                    end_two_hand("the other hand let go");
                }
                if (mine && cfg.pose_grab_release_drops)
                {
                    drop_weapon(side, pawn);
                    HSVR_LOG("Grab: %s grip let go: dropped %ls", hand, mine->GetName().c_str());
                }
                else if (!mine)
                    release_object(side, pawn);
                continue;
            }
            if (!mine && bool_property(pawn, side == 0 ? STR("Grabbed L") : STR("Grabbed R"))) continue; // already holds an object
            if (mine || menu_up) continue;
            if (try_two_hand(side, pawn, mesh, cfg)) continue;
            Vec3 at = hand_grab_point(mesh, side);
            std::vector<UObject*> weapons, willies;
            UObjectGlobals::FindAllOf(STR("ModularWeaponBP_C"), weapons);
            UObjectGlobals::FindAllOf(STR("Willie_BP_C"), willies);
            UObject* best = nullptr;
            double best_d = cfg.pose_grab_radius_cm;
            UObject* nearest = nullptr;
            double nearest_d = 1e9;
            int looked = 0;
            for (UObject* w : weapons)
            {
                if (!w || w->GetName().rfind(STR("Default__"), 0) == 0) continue;
                if (bool_property(w, STR("Is Sheathed")) || is_fist(w)) continue;
                bool taken = false;
                for (UObject* c : willies)
                    if (c && (object_property(c, STR("Weapon R")) == w || object_property(c, STR("Weapon L")) == w)) taken = true;
                if (taken) continue;
                UObject* base = object_property(w, STR("BaseMesh"));
                if (!base) continue;
                Call sim(base, STR("IsSimulatingPhysics"));
                sim.run();
                FProperty* sp = sim.param(STR("ReturnValue"));
                if (!sp || !*(sim.buffer.data() + sp->GetOffset_Internal())) continue;
                ++looked;
                double d = closest_on_weapon(w, at);
                if (d >= 0.0 && d < nearest_d)
                {
                    nearest_d = d;
                    nearest = w;
                }
                if (d >= 0.0 && d <= best_d)
                {
                    best_d = d;
                    best = w;
                }
            }
            if (!best && cfg.pose_grab_objects && grab_object(side, pawn, mesh, cfg, at)) continue;
            if (!best)
            {
                // Nothing to take: the hand closes into a fist.
                make_fist(side, pawn, cfg);
                HSVR_LOG("Grab: %s grip squeezed, no loose weapon within %.0f cm (%d loose of %d; nearest %ls at %.0f cm; hand at %.0f %.0f %.0f)", hand,
                         cfg.pose_grab_radius_cm, looked, (int)weapons.size(), nearest ? nearest->GetName().c_str() : STR("none"), nearest ? nearest_d : -1.0,
                         at.x, at.y, at.z);
                continue;
            }
            // Shields: where the hand touched it, before the game's set-up snaps it to its own grip.
            bool shield = cfg.pose_shield_free_grip && class_name_starts(best, STR("Shield"));
            UObject* best_base = object_property(best, STR("BaseMesh"));
            Vec3 w0_pos, touch;
            Quat w0_rot;
            if (shield && best_base)
            {
                w0_pos = component_location(best_base);
                w0_rot = component_rotation(best_base);
                if (closest_on_weapon(best, at, &touch) < 0.0) touch = at;
            }
            bool ok = give_hand_weapon(side, pawn, best);
            UObject* now = object_property(pawn, side == 0 ? STR("Weapon L") : STR("Weapon R"));
            if (shield && best_base && ok && now == best)
                shield_grip(side, mesh, best, best_base, w0_pos, w0_rot, at, touch, cfg);
            controllers_buzz(side, 0.4f);
            HSVR_LOG("Grab: %s grip took %ls (%.1f cm from the hand): %s, hand now holds %ls", hand, best->GetName().c_str(), best_d,
                     ok ? "set up" : "set-up call missing", now ? now->GetName().c_str() : STR("nothing"));
        }
    }

    // Where his hand goes while it holds a weapon. The hold sent the weapon (last tick) to where the
    // controller had the hand; if it is not there now, something stopped it, and the hand goes onto
    // the weapon by the same amount, so a stopped weapon keeps his hand on it while a free one leaves
    // the hand exactly on the controller ("a holding hand sits on its object", Matthew's framework,
    // rig/mod.rs). It eases in from 1 cm to pose_hold_blocked_from_cm, so the hold's few millimetres
    // of play never shake the hand.
    bool hand_on_weapon(int side, const hsvr::Config& cfg, hsvr::Vec3& target, hsvr::Quat& hand_rot)
    {
        using namespace hsvr;
        HoldState& h = g_hold[side];
        h.blocked_cm = 0.0;
        h.on_weapon = 0.0;
        if (!cfg.pose_hold_hand_on_weapon || !h.weapon || !h.have_last || !g_pose.pawn || h.forearm) return false;
        if (object_property(g_pose.pawn, side == 0 ? STR("Weapon L") : STR("Weapon R")) != h.weapon) return false;
        UObject* base = object_property(h.weapon, STR("BaseMesh"));
        if (!base) return false;
        Vec3 pos = component_location(base);
        Quat rot = component_rotation(base);
        // Where it is against where the hold sent it.
        Vec3 off = (pos + rot.rotate(h.pivot)) - (h.last_target_pos + h.last_target_rot.rotate(h.pivot));
        Vec3 turn = scaled_axis((rot * h.last_target_rot.inverse()).normalized());
        double coming = h.last_blocked_cm >= 0.0 ? h.last_blocked_cm - length(off) : 0.0;
        h.blocked_cm = length(off);
        h.last_blocked_cm = h.blocked_cm;
        double from = 1.0, full = (std::max)(from + 0.5, cfg.pose_hold_blocked_from_cm);
        double turn_cm = length(turn) * 10.0; // a turn counts as its movement 10 cm out
        double amount = (std::max)(h.blocked_cm, turn_cm);
        double t = (std::min)(1.0, (std::max)(0.0, (amount - from) / (full - from)));
        t = t * t * (3.0 - 2.0 * t);
        h.on_weapon = t;
        if (t <= 0.0) return false;
        // The stuck rule (Matthew's framework, hold_melee): a weapon stopped with its grip out of his
        // reach from the shoulder, or your hand pulled more than an arm's length from it, is let go
        // rather than dragged through or snapped back. Held for pose_hold_let_go_s first, so a glancing knock
        // never drops it.
        if (cfg.pose_hold_let_go && t >= 0.99 && g_pose.driver)
        {
            const ArmCalibration& a = g_pose.arm[side];
            double reach = a.upper_length + a.lower_length;
            Vec3 shoulder = socket_location(g_pose.driver, bone_fname(side == 0 ? "upperarm_l" : "upperarm_r"));
            Vec3 grip_at = pos + rot.rotate(h.pivot);
            Vec3 hand_point = target + hand_rot.rotate(h.in_hand);
            double from_shoulder = length(grip_at - shoulder) - length(h.in_hand);
            double from_hand = length(grip_at - hand_point);
            bool out = from_shoulder > reach + 2.0 || from_hand > reach;
            // Not right after it came into the hand (a new or long weapon swings into place first),
            // and not while it is still closing in on where the hold sends it.
            double t_now = now_seconds();
            bool touching = t_now - h.last_contact < 0.15;
            bool settling = h.started < 0.0 || t_now - h.started < cfg.pose_hold_let_go_grace_s || coming > 0.3 || !touching;
            if (!out || settling) h.stuck_since = -1.0;
            else
            {
                double now = now_seconds();
                if (h.stuck_since < 0.0) h.stuck_since = now;
                else if (now - h.stuck_since >= cfg.pose_hold_let_go_s)
                {
                    HSVR_LOG("Hold: %s hand lets go of %ls: stopped %.0f cm from where you hold it, its grip %.0f cm from the shoulder (reach %.0f), "
                             "your hand %.0f cm from it (stopped %.1f cm short of where it was sent)",
                             side == 0 ? "left" : "right", h.weapon->GetName().c_str(), h.blocked_cm, from_shoulder, reach, from_hand, h.miss_cm);
                    h.stuck_since = -1.0;
                    g_count_letgo.fetch_add(1);
                    drop_weapon(side, g_pose.pawn);
                    return false;
                }
            }
        }
        else
            h.stuck_since = -1.0;
        // The hand where it holds the weapon, moved and turned as the weapon was kept from following.
        Quat dr = from_scaled_axis(turn * t);
        Vec3 point = target + hand_rot.rotate(h.in_hand) + off * t;
        hand_rot = (dr * hand_rot).normalized();
        target = point - hand_rot.rotate(h.in_hand);
        return true;
    }

    // The grip zone of a weapon along its blade axis (BaseMesh +Z, cm from the game's grip point
    // Pos1), from the game's own data (hsvr.weapon_dump of 7 weapons, 2026-11-18):
    //  - the pommel end is "Length to Bottom" below the grip point;
    //  - the top stop is the Head module's start when it is offset (modular weapons: a sword's guard,
    //    a mace's or pollaxe's head), else the highest "base"-tagged scene below the tip (reforged
    //    weapons' guard, tools' head). A pollaxe's "Guard" sits on the haft, so Head is used, not Guard.
    // The hand's centre stays half a hand (pose_slide_hand_half_cm) inside both ends: the hand stops
    // at the guard instead of going through it (Matthew: hands can't slide through hand guards).
    void find_grip_zone(HoldState& h, UObject* weapon, UObject* base, const hsvr::Config& cfg)
    {
        using namespace hsvr;
        h.zone_known = true;
        Vec3 bp = component_location(base);
        Quat binv = component_rotation(base).inverse();
        auto z_of = [&](UObject* c) { return binv.rotate(component_location(c) - bp).z - h.pivot.z; };
        double bottom = 0.0, top = 0.0;
        if (FProperty* p = weapon->GetPropertyByNameInChain(STR("Length to Bottom"))) bottom = number_property(weapon, p);
        if (FProperty* p = weapon->GetPropertyByNameInChain(STR("Length to Top R Default"))) top = number_property(weapon, p);
        double stop = 1e9;
        const char* how = "none";
        if (UObject* head = object_property(weapon, STR("Head")))
        {
            double z = z_of(head);
            if (std::fabs(z) > 0.5)
            {
                stop = z;
                how = "Head module";
            }
        }
        if (stop > 1e8)
        {
            std::vector<UObject*> comps;
            UObjectGlobals::FindAllOf(STR("SceneComponent"), comps);
            for (UObject* c : comps)
            {
                if (!c || c->GetOuterPrivate() != weapon) continue;
                RawArray* t = c->GetValuePtrByPropertyNameInChain<RawArray>(STR("ComponentTags"));
                bool tagged = false;
                for (int32_t i = 0; t && t->Data && i < t->Num; ++i)
                {
                    std::wstring n = reinterpret_cast<const FName*>(t->Data)[i].ToString();
                    for (auto& ch : n) ch = (wchar_t)towlower(ch);
                    if (n == STR("base")) tagged = true;
                }
                if (!tagged) continue;
                double z = z_of(c);
                if (z > 0.0 && (top <= 0.0 || z < top) && (stop > 1e8 || z > stop))
                {
                    stop = z;
                    how = "base-tagged scene";
                }
            }
        }
        double half = cfg.pose_slide_hand_half_cm;
        h.zone_lo = bottom > 0.0 ? (std::min)(0.0, -bottom + half) : 0.0;
        h.zone_hi = stop < 1e8 ? (std::max)(0.0, stop - half) : 0.0;
        h.zone_stop = stop < 1e8 ? stop : 0.0;
        h.zone_tip = top;
        HSVR_LOG("Slide: %ls grip zone %.1f .. %.1f cm from the game's grip (pommel end %.1f, top stop %.1f from the %s, tip %.1f)", weapon->GetName().c_str(),
                 h.zone_lo, h.zone_hi, -bottom, stop < 1e8 ? stop : 0.0, how, top);
    }

    // The slide (Matthew: hold the trigger and that hand slides along the shaft, B&S-style, either
    // way, one-handed too). While the trigger is held, the point held moves toward where your hand
    // is along the weapon's axis, faster the harder you squeeze (B&S: a light squeeze drags, a full
    // one slides freely), so the weapon is no longer held along its length: it can slip through a
    // loose hand, or the hand runs up and down it. It stops at the ends of the grip zone. Let go of
    // the trigger and the hand holds there.
    void slide_grip(int side, HoldState& h, UObject* weapon, UObject* base, const hsvr::Vec3& hand_point, const hsvr::Config& cfg, double dt)
    {
        using namespace hsvr;
        if (!cfg.pose_slide || h.forearm) return;
        if (!h.zone_known) find_grip_zone(h, weapon, base, cfg);
        ControllerState st = controllers_state();
        float trig = g_fake_trigger[side].load() >= 0.0f ? g_fake_trigger[side].load() : st.hand[side].trigger;
        bool on = h.sliding ? trig > 0.15f : trig > 0.3f;
        if (on != h.sliding)
        {
            h.sliding = on;
            bool blade = h.slide > h.zone_hi + 0.5;
            HSVR_LOG("Slide: %s hand %s at %.1f cm (%s; grip zone %.1f .. %.1f, blade from %.1f)", side == 0 ? "left" : "right", on ? "starts sliding" : "holds", h.slide,
                     blade ? "on the blade" : "on the grip", h.zone_lo, h.zone_hi, h.zone_stop + cfg.pose_slide_hand_half_cm + cfg.pose_two_hand_blade_gap_cm);
        }
        if (!on) return;
        // Not while a weapon that just came into the hand swings into place.
        if (h.started >= 0.0 && now_seconds() - h.started < cfg.pose_hold_let_go_grace_s) return;
        Vec3 bp = component_location(base);
        Quat binv = component_rotation(base).inverse();
        double want = binv.rotate(hand_point - bp).z - h.pivot.z; // along the axis, from the game's grip point
        double k = cfg.pose_slide_rate_min + (cfg.pose_slide_rate_max - cfg.pose_slide_rate_min) * (std::min)(1.0, (std::max)(0.0, (trig - 0.15) / 0.85));
        double before = h.slide;
        double step = (want - h.slide) * (1.0 - std::exp(-k * dt));
        double most = cfg.pose_slide_max_cm_s * dt; // a hitch or a jump is not a slide
        // The zone the hand is on: the grip, or the blade after a hand-over from two hands.
        double zlo = h.zone_lo, zhi = h.zone_hi;
        if (before > h.zone_hi + 0.5)
        {
            double lo[2], hi[2];
            int n = two_hand_zones(h, cfg, lo, hi);
            for (int i = 0; i < n; ++i)
                if (lo[i] > h.zone_hi)
                {
                    zlo = lo[i];
                    zhi = hi[i];
                }
        }
        h.slide += (std::min)(most, (std::max)(-most, step));
        h.slide = (std::min)(zhi, (std::max)(zlo, h.slide));
        bool at_end = (h.slide <= zlo + 0.01 && want < zlo) || (h.slide >= zhi - 0.01 && want > zhi);
        static bool was_at_end[2] = {false, false};
        if (at_end && !was_at_end[side]) controllers_buzz(side, 0.5f);
        was_at_end[side] = at_end;
        HSVR_LOG_EVERY(0.5, "Slide: %s hand at %.1f cm (wants %.1f, zone %.1f .. %.1f, trigger %.2f, moved %.2f this tick)%s", side == 0 ? "left" : "right", h.slide,
                       want, zlo, zhi, trig, h.slide - before, at_end ? " at the stop" : "");
    }

    // How strong his hand is now, from the game's own stats (strength_stamina.md): his strength
    // (Muscle Rate, as the game's grip uses it: 0..1 -> 1..1.5, at least 0.5, here over its value at
    // the usual 0.5), that arm's tonus (injury and exhaustion: the game caps it from 1 down to 0.25
    // when exhausted) times his consciousness, and his stamina (100..0 -> 1..pose_body_stamina_floor).
    // 1 for a fresh, healthy Willie of usual strength. Each part can be switched off for testing.
    struct BodyStrength
    {
        double total = 1.0, strength = 1.0, injury = 1.0, stamina = 1.0;
        double muscle_rate = -1.0, tonus = -1.0, consciousness = -1.0, stamina_value = -1.0;
    };
    BodyStrength body_strength(UObject* pawn, int side, const hsvr::Config& cfg)
    {
        BodyStrength b;
        if (!cfg.pose_body_strength || !pawn) return b;
        b.muscle_rate = read_number_variable(pawn, STR("Muscle Rate"));
        b.tonus = read_number_variable(pawn, side == 0 ? STR("Arm L Tonus") : STR("Arm R Tonus"));
        b.consciousness = read_number_variable(pawn, STR("Consciousness"));
        b.stamina_value = read_number_variable(pawn, STR("Stamina"));
        if (b.muscle_rate >= 0.0) b.strength = (std::max)(0.5, 1.0 + 0.5 * b.muscle_rate) / 1.25;
        if (b.tonus >= 0.0) b.injury = (std::min)(1.0, b.tonus);
        if (b.consciousness >= 0.0) b.injury *= (std::min)(1.0, b.consciousness / 100.0);
        if (b.stamina_value >= 0.0)
        {
            double t = (std::min)(1.0, (std::max)(0.0, b.stamina_value / 100.0));
            b.stamina = cfg.pose_body_stamina_floor + (1.0 - cfg.pose_body_stamina_floor) * t;
        }
        // The arm's tonus already falls with exhaustion (the game caps it 1 -> 0.25 as Exhaustion rises,
        // which happens only once stamina is nearly gone), so stamina and tonus are not multiplied
        // (strength test 1: that counted it twice, 0.33 at stamina 0): the weaker of the two counts.
        b.total = (std::max)(cfg.pose_body_strength_min, b.strength * (std::min)(b.injury, b.stamina));
        static bool reported = false;
        if (!reported)
        {
            reported = true;
            HSVR_LOG("Strength: Muscle Rate %.2f, Arm %s Tonus %.3f, Consciousness %.0f, Stamina %.1f (-1 = not found) -> strength %.2f x injury %.2f x stamina %.2f = %.2f",
                     b.muscle_rate, side == 0 ? "L" : "R", b.tonus, b.consciousness, b.stamina_value, b.strength, b.injury, b.stamina, b.total);
        }
        return b;
    }

    // Holding and swinging tire him. The game's own drain (strength_stamina.md, @441296 and @354657)
    // reads the hand joint's force, which the hold leaves at 0, and the mouse's "Aim Swing Speed",
    // which VR never moves. The mod runs the same formula on the hold: the hand's burn rate is the
    // force it pulls with (in the game's units, x0.01, so N) times MRC(Muscle Rate, 0..3 -> 1..0.1),
    // kept only from 10 up, and the swing is the speed at the grip mapped like the mouse's aim speed
    // (pose_stamina_swing_cm_s lo..hi -> x10..x85), x0.0125, x pose_stamina_scale.
    void drain_stamina(UObject* pawn, double force_n, double grip_speed_cm_s, double dt, const hsvr::Config& cfg)
    {
        if (!pawn || cfg.pose_stamina_scale <= 0.0) return;
        double mr = read_number_variable(pawn, STR("Muscle Rate"));
        double strength = mr >= 0.0 ? 1.0 - 0.9 * (std::min)(1.0, (std::max)(0.0, mr / 3.0)) : 1.0;
        double burn = force_n * strength;
        if (burn < 10.0) return;
        double lo = cfg.pose_stamina_swing_lo_cm_s, hi = (std::max)(lo + 1.0, cfg.pose_stamina_swing_hi_cm_s);
        double t = (std::min)(1.0, (std::max)(0.0, (grip_speed_cm_s - lo) / (hi - lo)));
        double swing = 10.0 + 75.0 * t;
        double st = read_number_variable(pawn, STR("Stamina"));
        if (st < 0.0) return;
        double per_s = burn * swing * 0.0125 * cfg.pose_stamina_scale;
        set_variable(pawn, STR("Stamina"), (std::max)(0.0, st - per_s * dt));
        HSVR_LOG_EVERY(1.0, "Stamina: %.1f, taking %.1f a second (hold %.0f N, grip %.0f cm/s, swing x%.0f)", st, per_s, force_n, grip_speed_cm_s, swing);
    }

    // One tick of the hold for one hand. `joint` is the game's hand joint (its frames say where on the
    // weapon the hand holds it and how the weapon sits in the hand); `base` the weapon's BaseMesh.
    void hold_weapon(int side, UObject* pawn, UObject* joint, UObject* base, UObject* weapon, double mass, const hsvr::Config& cfg)
    {
        using namespace hsvr;
        HoldState& h = g_hold[side];
        double now = now_seconds();
        if (h.weapon != weapon)
        {
            h = HoldState{};
            h.weapon = weapon;
            h.started = now;
            if (g_pending_slide[side] < 1e8)
            {
                h.slide = g_pending_slide[side];
                h.started = now - cfg.pose_hold_let_go_grace_s; // handed over, already in place: no settling grace
                g_pending_slide[side] = 1e9;
            }
            if (g_pending_free[side].set)
            {
                h.free = g_pending_free[side];
                g_pending_free[side] = FreeGrip{};
                HSVR_LOG("Hold: %s hand holds %ls by the %s at (%.1f, %.1f, %.1f) on it%s", side == 0 ? "left" : "right", weapon->GetName().c_str(), h.free.what.c_str(),
                         h.free.pivot.x, h.free.pivot.y, h.free.pivot.z, h.free.forearm ? ", on the forearm" : "");
            }
        }
        // Where the controller has the hand: the hand bone's target this frame (exact arms), or the
        // bone itself when there is no controller.
        Vec3 hand_pos;
        Quat hand_rot;
        if (g_pose.hand_valid[side])
        {
            hand_pos = g_pose.target[side];
            hand_rot = g_pose.hand_rot[side];
        }
        else
        {
            UObject* mesh = object_property(pawn, STR("Mesh"));
            if (!mesh) return;
            hand_pos = socket_location(mesh, bone_fname(side == 0 ? "hand_l" : "hand_r"));
            hand_rot = socket_rotation(mesh, bone_fname(side == 0 ? "hand_l" : "hand_r"));
        }
        // Strap shields (grip types 6/7) are held on the forearm: the game's joint is to the lower arm
        // (shield_x_socket), and its frame there is in that bone's space (shield_grips.md). Reading
        // it as the hand's put the strap ~10 cm toward the fingers (playtest 2026-11-18).
        {
            std::string bone2 = nested_name(joint, {STR("ConstraintInstance"), STR("ConstraintBone2")});
            std::string lower = bone2;
            for (auto& ch : lower) ch = (char)tolower((unsigned char)ch);
            bool forearm = lower.rfind("lowerarm", 0) == 0;
            if (forearm)
            {
                UObject* mesh = object_property(pawn, STR("Mesh"));
                if (!mesh) return;
                hand_pos = socket_location(mesh, bone_fname(bone2));
                hand_rot = socket_rotation(mesh, bone_fname(bone2));
            }
            if (bone2 != h.anchor_bone)
            {
                h.anchor_bone = bone2;
                HSVR_LOG("Hold: %s hand's %ls is held to %s%s", side == 0 ? "left" : "right", weapon->GetName().c_str(), bone2.empty() ? "(no bone name)" : bone2.c_str(),
                         forearm ? " (on the forearm: strap shield)" : "");
            }
            h.forearm = forearm;
        }
        if (h.free.set)
        {
            // The mod's own grip: anchored on the hand (as the controller has it) or a strap's bone.
            if (h.free.forearm)
            {
                UObject* mesh = object_property(pawn, STR("Mesh"));
                if (!mesh) return;
                hand_pos = socket_location(mesh, bone_fname(h.free.bone));
                hand_rot = socket_rotation(mesh, bone_fname(h.free.bone));
            }
            else if (h.forearm)
            {
                if (g_pose.hand_valid[side])
                {
                    hand_pos = g_pose.target[side];
                    hand_rot = g_pose.hand_rot[side];
                }
                else if (UObject* mesh = object_property(pawn, STR("Mesh")))
                {
                    hand_pos = socket_location(mesh, bone_fname(side == 0 ? "hand_l" : "hand_r"));
                    hand_rot = socket_rotation(mesh, bone_fname(side == 0 ? "hand_l" : "hand_r"));
                }
            }
            h.forearm = h.free.forearm;
        }
        auto jn = [&](const TCHAR* v, const TCHAR* axis) {
            double x = nested_number(joint, {STR("ConstraintInstance"), v, axis});
            return std::isnan(x) ? 0.0 : x;
        };
        auto ax = [&](const TCHAR* v) { return Vec3{jn(v, STR("X")), jn(v, STR("Y")), jn(v, STR("Z"))}; };
        Vec3 pivot = ax(STR("Pos1"));   // the point held, in the weapon's frame
        Vec3 in_hand = ax(STR("Pos2")); // where the hand holds it, in the hand bone's frame
        Quat f1 = quat_from_axes(ax(STR("PriAxis1")), ax(STR("SecAxis1")));
        Quat f2 = quat_from_axes(ax(STR("PriAxis2")), ax(STR("SecAxis2")));
        if (h.free.set)
        {
            // weapon rotation = anchor x rel, and the point held sits on the anchor's point (no slide).
            pivot = h.free.pivot;
            in_hand = h.free.in_anchor;
            f1 = Quat{};
            f2 = h.free.rel;
        }
        else
        {
            // The slide moves the point held along the weapon (its +Z) from the game's grip point.
            h.pivot = pivot; // the game's grip point, for the zone and the slide
            double dt_slide = h.have_last ? (std::min)(1.0 / 30.0, (std::max)(1.0 / 240.0, now - h.last_time)) : 1.0 / 72.0;
            slide_grip(side, h, weapon, base, hand_pos + hand_rot.rotate(in_hand), cfg, dt_slide);
            pivot.z += h.slide;
        }
        h.pivot = pivot;
        h.in_hand = in_hand;
        h.f1 = f1;
        h.f2 = f2;
        // The weapon as the game's locked joint would sit it in that hand.
        Quat target_rot = (hand_rot * f2 * f1.inverse()).normalized();
        Vec3 target_pos = hand_pos + hand_rot.rotate(in_hand) - target_rot.rotate(pivot); // weapon origin
        if (h.free.place)
        {
            // Once, where the hand took it: the game's set-up has just snapped it to its own grip.
            h.free.place = false;
            set_component_transform(base, target_pos, target_rot, true);
        }
        double two_hand_gain = 1.0;
        if (g_two.active && g_two.main == side)
        {
            if (g_two.weapon != weapon) end_two_hand("the first hand holds another weapon");
            else if (g_pose.hand_valid[g_two.second])
            {
                int s2 = g_two.second;
                double game_z = pivot.z - h.slide;
                Vec3 p1 = hand_pos + hand_rot.rotate(in_hand);
                Vec3 p2 = g_pose.target[s2] + g_pose.hand_rot[s2].rotate(g_two.in_hand);
                // The second hand's trigger slides it along the zone it is on.
                ControllerState st2 = controllers_state();
                float trig = g_fake_trigger[s2].load() >= 0.0f ? g_fake_trigger[s2].load() : st2.hand[s2].trigger;
                bool on = g_two.sliding ? trig > 0.15f : trig > 0.3f;
                if (on != g_two.sliding)
                {
                    g_two.sliding = on;
                    HSVR_LOG("Two hands: %s hand %s at %.1f cm", s2 == 0 ? "left" : "right", on ? "starts sliding" : "holds", g_two.z);
                }
                Vec3 bp_now = component_location(base);
                Quat bq_now = component_rotation(base);
                if (on && h.have_last)
                {
                    double dt2 = (std::min)(1.0 / 30.0, (std::max)(1.0 / 240.0, now - h.last_time));
                    double want = bq_now.inverse().rotate(p2 - bp_now).z - game_z;
                    double rate = cfg.pose_slide_rate_min + (cfg.pose_slide_rate_max - cfg.pose_slide_rate_min) * (std::min)(1.0, (std::max)(0.0, (trig - 0.15) / 0.85));
                    double step = (want - g_two.z) * (1.0 - std::exp(-rate * dt2));
                    double most = cfg.pose_slide_max_cm_s * dt2;
                    double z = g_two.z + (std::min)(most, (std::max)(-most, step));
                    double lo[2], hi[2];
                    int n = two_hand_zones(h, cfg, lo, hi);
                    // Stays on the zone it is on: the guard stops it.
                    for (int i = 0; i < n; ++i)
                        if (g_two.z >= lo[i] - 0.01 && g_two.z <= hi[i] + 0.01)
                        {
                            z = (std::min)(hi[i], (std::max)(lo[i], z));
                            if (std::fabs(z - h.slide) >= 2.0 * cfg.pose_slide_hand_half_cm) g_two.z = z;
                        }
                }
                Vec3 local2{pivot.x, pivot.y, game_z + g_two.z};
                g_two.local = local2;
                g_two.have_local = true;
                double dz = local2.z - pivot.z;
                Vec3 between = p2 - p1;
                if (std::fabs(dz) > 1.0 && length(between) > 1.0)
                {
                    Vec3 axis_now = target_rot.rotate(Vec3{0, 0, 1});
                    Vec3 want_axis = normalized(between) * (dz > 0.0 ? 1.0 : -1.0);
                    Vec3 c = cross(axis_now, want_axis);
                    double d = dot(axis_now, want_axis);
                    Quat swing = d < -0.9999 ? quat_axis_angle(normalized(target_rot.rotate(Vec3{1, 0, 0})), 3.14159265358979) : Quat{c.x, c.y, c.z, 1.0 + d}.normalized();
                    target_rot = (swing * target_rot).normalized();
                    // The point halfway between the hands on the weapon goes halfway between your hands.
                    Vec3 mid_local = (pivot + local2) * 0.5;
                    target_pos = (p1 + p2) * 0.5 - target_rot.rotate(mid_local);
                    pivot = mid_local;
                }
                // Both hands pull: the second adds its own strength (B&S: two hands, twice the hold).
                BodyStrength first = body_strength(pawn, side, cfg), second = body_strength(pawn, s2, cfg);
                two_hand_gain = 1.0 + (std::max)(0.0, cfg.pose_two_hand_strength - 1.0) * (second.total / (std::max)(0.05, first.total));
                // A controller pulled well off its spot on the weapon lets go (B&S).
                Vec3 spot = bp_now + bq_now.rotate(local2);
                double off = length(p2 - spot);
                if (off > cfg.pose_two_hand_let_go_cm)
                {
                    if (g_two.far_since < 0.0) g_two.far_since = now;
                    else if (now - g_two.far_since > 0.3) end_two_hand("your hand was pulled off it");
                }
                else
                    g_two.far_since = -1.0;
                HSVR_LOG_EVERY(1.0, "Two hands: %ls on the %s at %.1f cm, first hand at %.1f; hands %.1f cm apart for %.1f cm on the weapon; second hand %.1f cm off its spot",
                               weapon->GetName().c_str(), g_two.on_blade ? "blade" : "grip", g_two.z, h.slide, length(between), std::fabs(dz), off);
            }
        }

        double dt = h.have_last ? now - h.last_time : 1.0 / 72.0;
        dt = (std::min)(1.0 / 30.0, (std::max)(1.0 / 240.0, dt));
        h.dt = dt;
        // How the controller moves and turns (at the point held): the damper works on the weapon's
        // motion against it, so a steady swing carries no lag. A jump (recentring, a teleport) is not motion.
        Vec3 target_vel, target_ang;
        Vec3 target_pivot = target_pos + target_rot.rotate(pivot);
        if (h.have_last)
        {
            Vec3 last_pivot = h.last_target_pos + h.last_target_rot.rotate(pivot);
            if (length(target_pivot - last_pivot) < cfg.pose_teleport_above_cm)
            {
                target_vel = (target_pivot - last_pivot) * (1.0 / dt);
                target_ang = scaled_axis((target_rot * h.last_target_rot.inverse()).normalized()) * (1.0 / dt);
            }
        }
        h.last_target_pos = target_pos;
        h.last_target_rot = target_rot;
        h.last_time = now;
        h.have_last = true;

        // The hand's hold (SI as in the framework, then in Unreal's units: cm, so a force x100, a
        // turning spring, damper and torque x100^2), times its strength (its dampers by half of it past 1).
        double strength = (std::max)(0.0, cfg.pose_hold_strength) * two_hand_gain;
        double damp_strength = (std::max)(1.0, strength * 0.5);
        double k = cfg.pose_hold_spring * strength;
        double c = cfg.pose_hold_damper * damp_strength;
        BodyStrength body = body_strength(pawn, side, cfg);
        h.body_strength = body.total;
        double max_force = cfg.pose_hold_max_force * strength * body.total * 100.0;
        double ka = cfg.pose_hold_turn_spring * strength * 1e4;
        double ca = cfg.pose_hold_turn_damper * damp_strength * 1e4;
        double max_torque = cfg.pose_hold_max_torque * strength * body.total * 1e4;

        // The weapon as it is.
        Vec3 pos = component_location(base);
        Quat rot = component_rotation(base);
        Call cm(base, STR("GetCenterOfMass"));
        cm.run();
        Vec3 com = cm.vec();
        Call lv(base, STR("GetPhysicsLinearVelocity"));
        lv.run();
        Call av(base, STR("GetPhysicsAngularVelocityInRadians"));
        av.run();
        Vec3 w_now = av.vec();
        if (h.have_expected)
        {
            h.miss_cm = length(com - h.expected_com);
            if (h.miss_cm > cfg.pose_hold_contact_cm) h.last_contact = now;
            if (h.miss_cm > 8.0) g_count_kicks.fetch_add(1);
            // The kick hunt: what the physics did with last tick's velocity, and whether the weapon's
            // collision answers changed. Logged on every tick it went more than 3 cm astray.
            if (g_joint_trace.load() && h.miss_cm > 3.0)
            {
                std::string resp;
                for (int ch = 0; ch < 32; ++ch)
                {
                    Call r(base, STR("GetCollisionResponseToChannel"));
                    r.byte(STR("Channel"), (uint8_t)ch);
                    r.run();
                    FProperty* rp = r.param(STR("ReturnValue"));
                    resp += (char)('0' + (rp ? *(r.buffer.data() + rp->GetOffset_Internal()) : 9));
                }
                Call ce(base, STR("GetCollisionEnabled"));
                ce.run();
                FProperty* cp = ce.param(STR("ReturnValue"));
                int enabled = cp ? *(ce.buffer.data() + cp->GetOffset_Internal()) : -1;
                Vec3 got_v = lv.vec();
                Vec3 off = com - h.expected_com;
                Vec3 off_local = rot.inverse().rotate(off);
                HSVR_LOG("Kick: %s weapon %.1f cm off where it was sent (weapon frame %.1f %.1f %.1f; +Z = blade), velocity set (%.0f %.0f %.0f) now (%.0f %.0f %.0f), "
                         "spin set (%.1f %.1f %.1f) now (%.1f %.1f %.1f), collision %d, responses %s%s",
                         side == 0 ? "left" : "right", h.miss_cm, off_local.x, off_local.y, off_local.z, h.last_set_v.x, h.last_set_v.y, h.last_set_v.z, got_v.x,
                         got_v.y, got_v.z, h.last_set_w.x, h.last_set_w.y, h.last_set_w.z, w_now.x, w_now.y, w_now.z, enabled, resp.c_str(),
                         (!h.last_responses.empty() && resp != h.last_responses) ? " (CHANGED)" : "");
                h.last_responses = resp;
            }
        }
        Vec3 pivot_now = pos + rot.rotate(pivot);
        Vec3 v = lv.vec() + cross(w_now, pivot_now - com);
        Vec3 w = w_now;

        // The spring pulls the point held onto where the controller has it at the end of the step:
        // m (v' - v) = dt (k (x - v' dt) + c (vt - v')), the force no more than the hand's.
        Vec3 to = target_pivot - pivot_now;
        Vec3 free_v = (v * mass + to * (dt * k) + target_vel * (dt * c)) * (1.0 / (mass + c * dt + k * dt * dt));
        Vec3 force = (free_v - v) * (mass / dt);
        h.force_capped = length(force) > max_force;
        force = clamp_length(force, max_force);
        Vec3 v_next = v + force * (dt / mass);
        h.force_n = length(force) / 100.0;
        drain_stamina(pawn, h.force_n, length(v), dt, cfg);
        // The weight (less what the grip's acceleration takes) turns the weapon about the point held.
        double gz = g_weapon_gravity.load() == 0 ? 0.0 : -980.0;
        Vec3 gravity{0.0, 0.0, gz};
        Vec3 weight = cross(com - pivot_now, (gravity - (v_next - v) * (1.0 / dt)) * mass);
        // Turning, against its inertia about the point held each way round.
        Vec3 angle = scaled_axis((target_rot * rot.inverse()).normalized());
        Call it(base, STR("GetInertiaTensor"));
        it.run();
        Vec3 I = it.vec(); // kg cm^2, about the centre of mass, in the body's frame
        if (!h.inertia_logged)
        {
            h.inertia_logged = true;
            Vec3 lc = rot.inverse().rotate(com - pos);
            HSVR_LOG("Hold: %s weapon %ls: mass %.2f kg, inertia (%.0f, %.0f, %.0f) kg cm^2, centre of mass (%.1f, %.1f, %.1f), held at (%.1f, %.1f, %.1f) in "
                     "its frame; hand point (%.1f, %.1f, %.1f) in the hand's; k %.0f c %.0f max %.0f N, turn k %.0f c %.0f max %.0f N m",
                     side == 0 ? "left" : "right", weapon->GetName().c_str(), mass, I.x, I.y, I.z, lc.x, lc.y, lc.z, pivot.x, pivot.y, pivot.z, in_hand.x,
                     in_hand.y, in_hand.z, k, c, max_force / 100.0, ka / 1e4, ca / 1e4, max_torque / 1e4);
        }
        Mat3 R = Mat3::from_quat(rot);
        Mat3 about_com = R * Mat3::diag(I.x, I.y, I.z) * R.transpose();
        Vec3 r = com - pivot_now;
        Mat3 outer;
        double rr[3] = {r.x, r.y, r.z};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) outer.m[i][j] = rr[i] * rr[j];
        double r2 = dot(r, r);
        Mat3 inertia = about_com + (Mat3::diag(r2, r2, r2) + outer * -1.0) * mass + Mat3::diag(1e-2, 1e-2, 1e-2);
        double d = ca * dt + ka * dt * dt;
        Vec3 free_w = (inertia + Mat3::diag(d, d, d)).inverse() * (inertia * w + (angle * ka + target_ang * ca + weight) * dt);
        Vec3 torque = inertia * (free_w - w) * (1.0 / dt) - weight;
        h.torque_capped = length(torque) > max_torque;
        torque = clamp_length(torque, max_torque);
        h.torque_nm = length(torque) / 1e4;
        Vec3 w_next = clamp_length(w + inertia.inverse() * ((torque + weight) * dt), 60.0);
        // 20 m/s is beyond any real hand; it only guards against tracking glitches.
        v_next = clamp_length(v_next, 2000.0);
        Quat rot_next = (from_scaled_axis(w_next * dt) * rot).normalized();
        Vec3 local_com = rot.inverse().rotate(com - pos);
        Vec3 com_next = pivot_now + v_next * dt + rot_next.rotate(local_com - pivot);
        h.expected_com = com_next;
        h.have_expected = true;
        // Wake it first: a body the solver has put to sleep (a weapon lying still on the ground) may
        // keep the velocity it is given without moving (run 29: the knife stayed frozen on the floor).
        Call wake(base, STR("WakeRigidBody"));
        wake.run();
        Call slv(base, STR("SetPhysicsLinearVelocity"));
        Vec3 set_v = (com_next - com) * (1.0 / dt);
        slv.three(STR("NewVel"), set_v.x, set_v.y, set_v.z);
        slv.flag(STR("bAddToCurrent"), false);
        slv.run();
        h.last_set_v = set_v;
        h.last_set_w = w_next;
        Call sav(base, STR("SetPhysicsAngularVelocityInRadians"));
        sav.three(STR("NewAngVel"), w_next.x, w_next.y, w_next.z);
        sav.flag(STR("bAddToCurrent"), false);
        sav.run();
        h.lag = length(to);
        if (h.lag > g_worst_lag[side].load()) g_worst_lag[side].store(h.lag);
        // Far behind for a while: why it isn't coming (asleep, not simulating, what it does with the
        // velocity it was given, its damping, and how it answers the world's channels).
        if (h.lag > 20.0)
        {
            if (h.far_since < 0.0) h.far_since = now;
            if (now - h.far_since > 0.5 && now - h.far_logged > 1.0)
            {
                h.far_logged = now;
                Call sim(base, STR("IsSimulatingPhysics"));
                sim.run();
                Call awake(base, STR("RigidBodyIsAwake"));
                awake.run();
                Call back(base, STR("GetPhysicsLinearVelocity"));
                back.run();
                Call ld(base, STR("GetLinearDamping"));
                ld.run();
                Call ad(base, STR("GetAngularDamping"));
                ad.run();
                auto boolean = [](Call& c) {
                    FProperty* rp = c.param(STR("ReturnValue"));
                    return rp ? (int)(*(c.buffer.data() + rp->GetOffset_Internal()) != 0) : -1;
                };
                int resp[3] = {-1, -1, -1};
                const uint8_t channels[3] = {0, 1, 5};
                for (int i = 0; i < 3; ++i)
                {
                    Call r(base, STR("GetCollisionResponseToChannel"));
                    r.byte(STR("Channel"), channels[i]);
                    r.run();
                    FProperty* rp = r.param(STR("ReturnValue"));
                    resp[i] = rp ? *(r.buffer.data() + rp->GetOffset_Internal()) : -1;
                }
                Vec3 got = back.vec();
                HSVR_LOG("Hold: %s weapon %.0f cm behind for %.1f s: simulating %d, awake %d, velocity set (%.0f, %.0f, %.0f) reads back (%.0f, %.0f, %.0f), "
                         "damping %.2f / %.2f, responses to channels 0/1/5: %d %d %d, at (%.1f, %.1f, %.1f)",
                         side == 0 ? "left" : "right", h.lag, now - h.far_since, boolean(sim), boolean(awake), set_v.x, set_v.y, set_v.z, got.x, got.y,
                         got.z, ld.number(), ad.number(), resp[0], resp[1], resp[2], pos.x, pos.y, pos.z);
            }
        }
        else
            h.far_since = -1.0;
        Quat dq = (target_rot * rot.inverse()).normalized();
        h.turn_deg = 2.0 * std::acos((std::min)(1.0, std::fabs(dq.w))) * 180.0 / 3.14159265358979;
    }

    // The kick hunt (kick-test1): the game's Check Stuck In Static Mesh (MW @77374), run on a 1 s
    // loop from Willie (UF @478416-@480455) and from the weapon itself once it has unstuck, toggles
    // the collision of every part of a weapon, which in UE 5.4 rebuilds the welded body (new shapes,
    // mass and centre of mass). Logs each tick a held weapon's body changes, and the flags around it.
    // hsvr.stuck_check 0 pins Willie's check rate at 0 so his loop never fires (a test, not a fix).
    std::atomic<int> g_stuck_check{1};
    std::atomic<bool> g_pin_disabled{false};
    std::atomic<bool> g_break_joint{false};
    void watch_weapon_rebuild(int side, UObject* pawn, UObject* weapon, UObject* base, double mass)
    {
        using namespace hsvr;
        struct Seen
        {
            UObject* weapon = nullptr;
            double mass = 0.0;
            Vec3 com_local{0, 0, 0};
            bool unstuck = false;
        };
        static Seen seen[2];
        if (!g_stuck_check.load()) set_variable(pawn, STR("Weapon Stuck In Static Mesh Check Rate"), 0.0);
        // Test (hsvr.pin_disabled 1): keep his "Weapon R/L Collisions Disabled Temporarily" set, which
        // gates his back sweep (UF @478227 -> @95012) that disables skeleton-vs-weapon collision and
        // re-enables it 1 s later (@96043, @96667 -> @801723) - suspected to undo our hand ignore.
        if (g_pin_disabled.load())
            set_variable(pawn, side == 0 ? STR("Weapon L Collisions Disabled Temporarily") : STR("Weapon R Collisions Disabled Temporarily"), 1.0);
        Call cm(base, STR("GetCenterOfMass"));
        cm.run();
        Vec3 com_local = component_rotation(base).inverse().rotate(cm.vec() - component_location(base));
        bool unstuck = bool_property(weapon, STR("Static Mesh Collision Disabled (Unstuck)"));
        Seen& s = seen[side];
        if (s.weapon == weapon)
        {
            double moved = length(com_local - s.com_local);
            if (std::fabs(mass - s.mass) > 0.005 || moved > 0.3 || unstuck != s.unstuck)
            {
                double rate = 0.0;
                if (FProperty* p = pawn->GetPropertyByNameInChain(STR("Weapon Stuck In Static Mesh Check Rate"))) rate = number_property(pawn, p);
                double md = 0.0;
                if (FProperty* p = pawn->GetPropertyByNameInChain(STR("Movement Dislocation"))) md = number_property(pawn, p);
                HSVR_LOG("Kick: %s weapon body changed: mass %.3f -> %.3f kg, centre of mass moved %.1f cm in its frame, Unstuck %d -> %d, "
                         "check rate %.2f, Movement Dislocation %.1f, Collisions Disabled Temporarily %d",
                         side == 0 ? "left" : "right", s.mass, mass, moved, (int)s.unstuck, (int)unstuck, rate, md,
                         (int)bool_property(pawn, side == 0 ? STR("Weapon L Collisions Disabled Temporarily") : STR("Weapon R Collisions Disabled Temporarily")));
            }
        }
        s.weapon = weapon;
        s.mass = mass;
        s.com_local = com_local;
        s.unstuck = unstuck;
    }

    // The kick hunt: who pushes the held weapon. Hooks the engine's physics writes (impulses, forces,
    // velocities, teleports) and logs every call from game script whose target is a part of a weapon
    // in his hands or his own mesh, with the calling Blueprint function and its bytecode offset (the
    // "@" offsets of the decompile). Calls made through ProcessEvent (ours) are skipped: their frame's
    // function is the hooked function itself. hsvr.force_watch 1 installs it (once).
    std::atomic<bool> g_force_watch{false};
    void force_watch_hook(Unreal::UnrealScriptFunctionCallableContext& context, void* custom_data)
    {
        if (!g_force_watch.load()) return;
        UObject* target = context.Context;
        UFunction* hooked = static_cast<UFunction*>(custom_data);
        UFunction* caller = context.TheStack.Node();
        {
            // Proof the hooks fire: every call counted (any target, ours included), summed every 2 s.
            static std::map<std::wstring, int> counts;
            static double last = 0.0;
            std::wstring key = hooked->GetName() + (caller == hooked ? STR(" (ProcessEvent)") : STR(" (script)"));
            ++counts[key];
            double now = hsvr::now_seconds();
            if (now - last >= 2.0)
            {
                last = now;
                std::wstring all;
                for (auto& [k, n] : counts) all += k + STR(" ") + std::to_wstring(n) + STR(", ");
                HSVR_LOG("Force: calls in the last 2 s, any target: %ls", all.c_str());
                counts.clear();
            }
        }
        if (!target || !g_pose.pawn) return;
        if (!caller || caller == hooked) return; // ours, through ProcessEvent
        UObject* outer = target->GetOuterPrivate();
        if (outer && !is_a(outer, STR("Actor")))
        {
            Call own_call(target, STR("GetOwner"));
            own_call.run();
            if (UObject* o = own_call.object_result()) outer = o;
        }
        UObject* wr = object_property(g_pose.pawn, STR("Weapon R"));
        UObject* wl = object_property(g_pose.pawn, STR("Weapon L"));
        bool weapon = outer && (outer == wr || outer == wl);
        bool own = target == g_pose.mesh || outer == g_pose.pawn;
        if (!weapon && !own) return;
        long long offset = -1;
        const auto& script = caller->GetScript();
        if (context.TheStack.Code() && script.GetData()) offset = (long long)(context.TheStack.Code() - script.GetData());
        HSVR_LOG("Force: %ls on %ls.%ls from %ls (%ls) @%lld", hooked->GetName().c_str(), outer ? outer->GetName().c_str() : STR("?"), target->GetName().c_str(),
                 caller->GetName().c_str(), caller->GetOuterPrivate() ? caller->GetOuterPrivate()->GetName().c_str() : STR("?"), offset);
    }

    void install_force_watch()
    {
        static bool installed = false;
        if (installed) return;
        installed = true;
        int hooked = 0;
        for (const TCHAR* path : {STR("/Script/Engine.PrimitiveComponent:AddImpulse"), STR("/Script/Engine.PrimitiveComponent:AddImpulseAtLocation"),
                                  STR("/Script/Engine.PrimitiveComponent:AddForce"), STR("/Script/Engine.PrimitiveComponent:AddForceAtLocation"),
                                  STR("/Script/Engine.PrimitiveComponent:AddForceAtLocationLocal"), STR("/Script/Engine.PrimitiveComponent:AddTorqueInRadians"),
                                  STR("/Script/Engine.PrimitiveComponent:AddTorqueInDegrees"), STR("/Script/Engine.PrimitiveComponent:AddAngularImpulseInRadians"),
                                  STR("/Script/Engine.PrimitiveComponent:AddAngularImpulseInDegrees"), STR("/Script/Engine.PrimitiveComponent:AddRadialImpulse"),
                                  STR("/Script/Engine.PrimitiveComponent:AddRadialForce"), STR("/Script/Engine.PrimitiveComponent:SetPhysicsLinearVelocity"),
                                  STR("/Script/Engine.PrimitiveComponent:SetAllPhysicsLinearVelocity"),
                                  STR("/Script/Engine.PrimitiveComponent:SetPhysicsAngularVelocityInRadians"),
                                  STR("/Script/Engine.PrimitiveComponent:SetPhysicsAngularVelocityInDegrees"), STR("/Script/Engine.PrimitiveComponent:SetSimulatePhysics"),
                                  STR("/Script/Engine.PrimitiveComponent:SetMassOverrideInKg"), STR("/Script/Engine.PrimitiveComponent:SetCenterOfMass"),
                                  STR("/Script/Engine.PrimitiveComponent:SetCollisionEnabled"), STR("/Script/Engine.PrimitiveComponent:SetCollisionResponseToChannel"),
                                  STR("/Script/Engine.PrimitiveComponent:WakeRigidBody"), STR("/Script/Engine.SceneComponent:K2_SetWorldLocation"),
                                  STR("/Script/Engine.SceneComponent:K2_SetWorldTransform"), STR("/Script/Engine.SceneComponent:K2_SetWorldLocationAndRotation"),
                                  STR("/Script/Engine.SceneComponent:K2_AddWorldOffset"), STR("/Script/Engine.SceneComponent:K2_SetRelativeLocation"),
                                  STR("/Script/Engine.SceneComponent:K2_AttachToComponent"), STR("/Script/Engine.SceneComponent:DetachFromParent"),
                                  STR("/Script/Engine.SkeletalMeshComponent:SetAllBodiesSimulatePhysics"),
                                  STR("/Script/Engine.SkeletalMeshComponent:SetAllBodiesBelowSimulatePhysics"),
                                  STR("/Script/Engine.SkeletalMeshComponent:SetAllBodiesPhysicsBlendWeight"),
                                  STR("/Script/Engine.SkeletalMeshComponent:SetAllMassScale"), STR("/Script/Engine.PrimitiveComponent:SetPhysMaterialOverride"),
                                  STR("/Script/Engine.PhysicsConstraintComponent:SetConstrainedComponents"),
                                  STR("/Script/Engine.PhysicsConstraintComponent:SetConstraintReferencePosition")})
        {
            UFunction* fn = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, path);
            if (!fn) continue;
            UObjectGlobals::RegisterHook(fn, &force_watch_hook, [](Unreal::UnrealScriptFunctionCallableContext&, void*) {}, fn);
            ++hooked;
        }
        HSVR_LOG("Force: watching %d engine physics functions for calls on his weapons and his body", hooked);
    }

    void weapon_spring_after_tick(UObject* pawn)
    {
        using namespace hsvr;
        Config cfg = config();
        for (int side = 0; side < 2; ++side)
        {
            GripLog& log = g_grip_log[side];
            log.active = false;
            int spring_override = g_grip_spring_override.load();
            int hold_override = g_grip_hold_override.load();
            bool hold_on = hold_override < 0 ? cfg.pose_grip_hold : hold_override != 0;
            // The hold replaces the spring: the joint goes free and the mod moves the weapon itself.
            bool spring_on = !hold_on && (spring_override < 0 ? cfg.pose_grip_spring != 0 : spring_override != 0);
            UObject* joint = object_property(pawn, side == 0 ? STR("PhysicsConstraint L Hand") : STR("PhysicsConstraint R Hand"));
            // Switching the spring off (ini, live) puts the game's own locked grip back on a joint we sprung.
            static UObject* sprung[2] = {nullptr, nullptr};
            if (!spring_on && !hold_on && joint && sprung[side] == joint)
            {
                sprung[side] = nullptr;
                lock_joint(joint);
                HSVR_LOG("Grip: %s hand back on the game's locked grip: %s", side == 0 ? "left" : "right", joint_settings(joint).c_str());
            }
            if ((spring_on || hold_on) && joint) sprung[side] = joint;
            // With the spring off the game's own locked grip holds the weapon; it is still measured and logged.
            UObject* weapon = object_property(pawn, side == 0 ? STR("Weapon L") : STR("Weapon R"));
            if (g_arm_ignore[side].weapon && g_arm_ignore[side].weapon != weapon) clear_arm_ignore(side);
            if (!joint || !weapon) continue;
            // Only a seated weapon (the game first pulls a new weapon into the hand over a short distance).
            if (side == 1)
                if (FProperty* p = pawn->GetPropertyByNameInChain(STR("R Hand Soft Attach Distance")))
                    if (number_property(pawn, p) > 0.0) continue;
            if (bool_property(weapon, STR("Is Sheathed")) || bool_property(pawn, STR("Sheathing R")) || bool_property(pawn, STR("Unsheathing R"))) continue;
            UObject* base = object_property(weapon, STR("BaseMesh"));
            if (!base) continue;
            Call m(base, STR("GetMass"));
            m.run();
            double mass = (std::max)(0.05, m.number());
            watch_weapon_rebuild(side, pawn, weapon, base, mass);
            {
                static double last_before = 0.0;
                double t = now_seconds();
                bool show = side == 1 && t - last_before >= 1.0;
                if (show)
                {
                    last_before = t;
                    HSVR_LOG("Grip: right joint after his tick (the game's): %s", joint_settings(joint).c_str());
                }
                // Override 2: write the spring once per weapon, then leave the joint alone.
                static UObject* sprung_once[2] = {nullptr, nullptr};
                if (spring_override == 2)
                {
                    if (sprung_once[side] != weapon)
                    {
                        sprung_once[side] = weapon;
                        spring_joint(joint, mass, cfg);
                        HSVR_LOG("Grip: spring written once: %s", joint_settings(joint).c_str());
                    }
                }
                else if (hold_on)
                {
                    if (g_joint_trace.load() && side == 1)
                    {
                        auto var = [&](const TCHAR* n) {
                            FProperty* p = pawn->GetPropertyByNameInChain(n);
                            return p ? number_property(pawn, p) : -999.0;
                        };
                        HSVR_LOG("Joint: before free: %s | Soft Not Attached %.0f, Soft Attach Distance %.1f, Attached Correctly %.0f, Weapon R Collisions Disabled Temporarily %.0f",
                                 joint_settings(joint).c_str(), var(STR("R Hand Soft Not Attached")), var(STR("R Hand Soft Attach Distance")),
                                 var(STR("R Hand Attached Correctly")), var(STR("Weapon R Collisions Disabled Temporarily")));
                    }
                    free_joint(joint);
                    if (g_joint_trace.load() && side == 1) HSVR_LOG("Joint: after free: %s", joint_settings(joint).c_str());
                    // Test (hsvr.break_joint 1): break the game's hand joint outright, so nothing links
                    // the hand and the weapon but the mod's hold (decoded default: a 50 cm linear limit).
                    static UObject* broken[2] = {nullptr, nullptr};
                    if (g_break_joint.load() && broken[side] != joint)
                    {
                        broken[side] = joint;
                        Call b(joint, STR("BreakConstraint"));
                        b.run();
                        HSVR_LOG("Joint: %s hand joint broken (test); the game still holds %ls: %d", side == 0 ? "left" : "right", weapon->GetName().c_str(),
                                 (int)(object_property(pawn, side == 0 ? STR("Weapon L") : STR("Weapon R")) == weapon));
                    }
                    if (!g_break_joint.load()) broken[side] = nullptr;
                }
                else if (spring_on) spring_joint(joint, mass, cfg);
                {
                    static UObject* typed = nullptr;
                    if (side == 1 && typed != weapon)
                    {
                        typed = weapon;
                        log_collision_types(pawn, base);
                        log_chaos_cvars();
                    }
                    int ch = g_weapon_channel_cmd.exchange(-1);
                    if (side == 1 && ch >= 0)
                    {
                        Call r(base, STR("SetCollisionResponseToChannel"));
                        r.byte(STR("Channel"), (uint8_t)ch);
                        r.byte(STR("NewResponse"), (uint8_t)g_weapon_channel_resp.load());
                        r.run();
                        Call readback(base, STR("GetCollisionResponseToChannel"));
                        readback.byte(STR("Channel"), (uint8_t)ch);
                        readback.run();
                        HSVR_LOG("Grip: weapon response to channel %d set to %d, reads %d", ch, g_weapon_channel_resp.load(), byte_result(readback));
                    }
                }
                // Anchor the weapon at its own handle marker instead of the game's anchor point.
                {
                    static UObject* anchored[2] = {nullptr, nullptr};
                    static bool anchored_marker[2] = {false, false};
                    static Vec3 game_pos1[2];
                    bool want = cfg.pose_grip_anchor_marker;
                    if (anchored[side] != joint || anchored_marker[side] != want)
                    {
                        if (anchored[side] != joint)
                        {
                            auto p1 = [&](const TCHAR* axis) {
                                double v = nested_number(joint, {STR("ConstraintInstance"), STR("Pos1"), axis});
                                return std::isnan(v) ? 0.0 : v;
                            };
                            game_pos1[side] = Vec3{p1(STR("X")), p1(STR("Y")), p1(STR("Z"))};
                        }
                        anchored[side] = joint;
                        anchored_marker[side] = want;
                        Vec3 at = game_pos1[side];
                        UObject* marker = object_property(weapon, side == 0 ? STR("Grip L Hand Default Pos") : STR("Grip R Hand Default Pos"));
                        if (want && marker)
                            at = component_rotation(base).inverse().rotate(component_location(marker) - component_location(base));
                        Call rp(joint, STR("SetConstraintReferencePosition"));
                        rp.byte(STR("Frame"), 0); // Frame1, the weapon's side
                        rp.three(STR("RefPosition"), at.x, at.y, at.z);
                        rp.run();
                        HSVR_LOG("Grip: %s weapon anchor at (%.1f, %.1f, %.1f) in the weapon (%s)", side == 0 ? "left" : "right", at.x, at.y, at.z,
                                 want ? (marker ? "its handle marker" : "no handle marker, the game's anchor") : "the game's anchor");
                    }
                }
                if ((spring_on || hold_on) && g_grip_ignore_arm.load())
                    ignore_arm_collision(pawn, object_property(pawn, STR("Mesh")), weapon, base, side);
                else if (g_arm_ignore[side].weapon)
                    clear_arm_ignore(side);
                if (side == 1)
                {
                    static int done_serial = 0;
                    static UObject* original_for = nullptr;
                    static hsvr::Vec3 original;
                    if (original_for != joint)
                    {
                        original_for = joint;
                        auto jn = [&](const TCHAR* axis) {
                            double v = nested_number(joint, {STR("ConstraintInstance"), STR("Pos2"), axis});
                            return std::isnan(v) ? 0.0 : v;
                        };
                        original = hsvr::Vec3{jn(STR("X")), jn(STR("Y")), jn(STR("Z"))};
                        done_serial = 0;
                    }
                    int serial = g_grip_frame_serial.load();
                    if (serial != done_serial)
                    {
                        done_serial = serial;
                        hsvr::Vec3 at{original.x + g_grip_frame_offset[0].load(), original.y + g_grip_frame_offset[1].load(),
                                      original.z + g_grip_frame_offset[2].load()};
                        Call rp(joint, STR("SetConstraintReferencePosition"));
                        rp.byte(STR("Frame"), 1); // EConstraintFrame::Frame2, the hand's side
                        rp.three(STR("RefPosition"), at.x, at.y, at.z);
                        bool ok = rp.run();
                        HSVR_LOG("Grip: hand anchor moved to (%.1f, %.1f, %.1f) (%d)", at.x, at.y, at.z, (int)ok);
                    }
                }
                if (side == 1 && g_grip_rejoin.exchange(false))
                {
                    Call rj(joint, STR("SetConstrainedComponents"));
                    rj.object(STR("Component1"), base);
                    rj.name(STR("BoneName1"), FName(STR("None"), FNAME_Add));
                    rj.object(STR("Component2"), object_property(pawn, STR("Mesh")));
                    rj.name(STR("BoneName2"), bone_fname("hand_r"));
                    bool ok = rj.run();
                    HSVR_LOG("Grip: rejoined the right hand joint (%d): %s", (int)ok, joint_settings(joint).c_str());
                }
                int collision = g_weapon_collision.load();
                if (collision >= 0)
                {
                    static int applied = -1;
                    static UObject* applied_to = nullptr;
                    if (applied != collision || applied_to != base)
                    {
                        applied = collision;
                        applied_to = base;
                        Call before(base, STR("GetCollisionEnabled"));
                        before.run();
                        FProperty* rp = before.param(STR("ReturnValue"));
                        int was = rp ? *(before.buffer.data() + rp->GetOffset_Internal()) : -1;
                        Call ce(base, STR("SetCollisionEnabled"));
                        ce.byte(STR("NewType"), (uint8_t)collision);
                        ce.run();
                        HSVR_LOG("Grip: weapon BaseMesh collision was %d, now %d", was, collision);
                    }
                }
                int gravity = g_weapon_gravity.load();
                if (gravity >= 0)
                {
                    Call g(base, STR("SetEnableGravity"));
                    g.first_flag(gravity != 0);
                    g.run();
                }
                // Both hands on one weapon: the right hand holds it for now (two-handed holds come later).
                if (hold_on && !(side == 0 && weapon == object_property(pawn, STR("Weapon R"))))
                {
                    Call sim(base, STR("IsSimulatingPhysics"));
                    sim.run();
                    FProperty* rp = sim.param(STR("ReturnValue"));
                    if (rp && *(sim.buffer.data() + rp->GetOffset_Internal())) hold_weapon(side, pawn, joint, base, weapon, mass, cfg);
                }
                else
                    g_hold[side].weapon = nullptr;
                if (show) HSVR_LOG("Grip: right joint after the mod's change: %s", joint_settings(joint).c_str());
                static UObject* dumped[2] = {nullptr, nullptr};
                if (dumped[side] != weapon)
                {
                    dumped[side] = weapon;
                    dump_joint(joint);
                    list_holders(pawn);
                    list_holders(weapon);
                }
            }
            log.active = true;
            log.mass = mass;
            {
                Call sim(base, STR("IsSimulatingPhysics"));
                sim.run();
                FProperty* rp = sim.param(STR("ReturnValue"));
                log.simulating = rp ? (*(sim.buffer.data() + rp->GetOffset_Internal()) ? 1 : 0) : -1;
                Call sc(base, STR("K2_GetComponentScale"));
                sc.run();
                log.scale = sc.vec().x;
                if (g_pose.mesh)
                {
                    log.weapon_at = component_location(base);
                    log.hand_at = socket_location(g_pose.mesh, bone_fname(side == 0 ? "hand_l" : "hand_r"));
                    log.hand_dist = length(log.weapon_at - log.hand_at);
                    Quat hand_rot = socket_rotation(g_pose.mesh, bone_fname(side == 0 ? "hand_l" : "hand_r"));
                    if (log.weapon_seen != weapon)
                    {
                        // Once per weapon: what it is made of (a modular weapon's parts are welded children of BaseMesh).
                        Call kids(base, STR("GetChildrenComponents"));
                        kids.flag(STR("bIncludeAllDescendants"), true);
                        if (kids.run())
                            if (FProperty* cp = kids.param(STR("Children")))
                            {
                                struct RawArray
                                {
                                    UObject** data;
                                    int32_t num, max;
                                };
                                RawArray* arr = reinterpret_cast<RawArray*>(kids.buffer.data() + cp->GetOffset_Internal());
                                HSVR_LOG("Grip: %ls has %d parts under BaseMesh", weapon->GetFullName().c_str(), arr->num);
                                for (int32_t i = 0; arr->data && i < arr->num && i < 32; ++i)
                                {
                                    UObject* part = arr->data[i];
                                    if (!part) continue;
                                    Call pv(part, STR("IsVisible"));
                                    pv.run();
                                    FProperty* vp = pv.param(STR("ReturnValue"));
                                    Vec3 at = component_location(part);
                                    HSVR_LOG("Grip:   part %ls visible %d at (%.0f, %.0f, %.0f)", part->GetFullName().c_str(),
                                             vp ? (*(pv.buffer.data() + vp->GetOffset_Internal()) ? 1 : 0) : -1, at.x, at.y, at.z);
                                }
                            }
                        log.weapon_seen = weapon;
                        log.rest = hand_rot.inverse().rotate(log.weapon_at - log.hand_at);
                    }
                    // How far the weapon is from where it sat in the hand when it was seated.
                    log.gap = length(log.weapon_at - (log.hand_at + hand_rot.rotate(log.rest)));
                    auto jn = [&](const TCHAR* vec, const TCHAR* axis) {
                        double v = nested_number(joint, {STR("ConstraintInstance"), vec, axis});
                        return std::isnan(v) ? 0.0 : v;
                    };
                    Vec3 pos1{jn(STR("Pos1"), STR("X")), jn(STR("Pos1"), STR("Y")), jn(STR("Pos1"), STR("Z"))};
                    log.pos2 = Vec3{jn(STR("Pos2"), STR("X")), jn(STR("Pos2"), STR("Y")), jn(STR("Pos2"), STR("Z"))};
                    Quat base_rot = component_rotation(base);
                    Vec3 weapon_anchor = log.weapon_at + base_rot.rotate(pos1);
                    Vec3 hand_anchor = log.hand_at + hand_rot.rotate(log.pos2);
                    log.stretch_vec = weapon_anchor - hand_anchor;
                    log.stretch = length(log.stretch_vec);
                    log.stretch_hand = hand_rot.inverse().rotate(log.stretch_vec);
                    // Turning error: the joint's two frames (PriAxis/SecAxis on each side) should line up.
                    {
                        auto ax = [&](const TCHAR* v) {
                            return Vec3{jn(v, STR("X")), jn(v, STR("Y")), jn(v, STR("Z"))};
                        };
                        Quat f1 = base_rot * quat_from_axes(ax(STR("PriAxis1")), ax(STR("SecAxis1")));
                        Quat f2 = hand_rot * quat_from_axes(ax(STR("PriAxis2")), ax(STR("SecAxis2")));
                        Quat d = (f2.inverse() * f1).normalized();
                        double w = (std::min)(1.0, std::fabs(d.w));
                        log.turn_err_deg = 2.0 * std::acos(w) * 180.0 / 3.14159265358979;
                    }
                    // Where the physics bodies themselves are (centre of mass of the hand_r body and of the
                    // weapon body), in the bone's / component's own frame. Constant = just a local offset;
                    // changing with the arm's motion = the physics body is not where the bone is.
                    {
                        Call hc(g_pose.mesh, STR("GetCenterOfMass"));
                        hc.name(STR("BoneName"), bone_fname(side == 0 ? "hand_l" : "hand_r"));
                        hc.run();
                        log.hand_com_local = hand_rot.inverse().rotate(hc.vec() - log.hand_at);
                        log.hand_com_world = hc.vec();
                        Call wc(base, STR("GetCenterOfMass"));
                        wc.name(STR("BoneName"), FName());
                        wc.run();
                        log.weapon_com_local = base_rot.inverse().rotate(wc.vec() - log.weapon_at);
                    }
                    if (cfg.dev_grip_trace && side == 1)
                    {
                        static double last_t = 0.0;
                        double t = now_seconds();
                        Vec3 tg = g_pose.target[1];
                        HSVR_LOG("Trace: dt %.4f hand (%.2f, %.2f, %.2f) body (%.2f, %.2f, %.2f) weapon (%.2f, %.2f, %.2f) target (%.2f, %.2f, %.2f) "
                                 "stretch %.2f turn %.2f hold lag %.2f turn %.2f force %.0f%s torque %.1f%s",
                                 t - last_t, log.hand_at.x, log.hand_at.y, log.hand_at.z, log.hand_com_world.x, log.hand_com_world.y, log.hand_com_world.z,
                                 log.weapon_at.x, log.weapon_at.y, log.weapon_at.z, tg.x, tg.y, tg.z, log.stretch, log.turn_err_deg, g_hold[1].lag,
                                 g_hold[1].turn_deg, g_hold[1].force_n, g_hold[1].force_capped ? " (cap)" : "", g_hold[1].torque_nm,
                                 g_hold[1].torque_capped ? " (cap)" : "");
                        last_t = t;
                    }
                    // The joint binds the pawn's Mesh (CharacterMesh0); the measurement above used the
                    // posed mesh. Log how far apart their hand bones are, in case they differ.
                    if (UObject* cm = object_property(pawn, STR("Mesh")))
                        log.mesh_hand_diff = length(socket_location(cm, bone_fname(side == 0 ? "hand_l" : "hand_r")) - log.hand_at),
                        log.same_mesh = cm == g_pose.mesh ? 1 : 0;
                }
                Call vis(base, STR("IsVisible"));
                vis.run();
                if (FProperty* vp = vis.param(STR("ReturnValue"))) log.visible = *(vis.buffer.data() + vp->GetOffset_Internal()) ? 1 : 0;
                Call who(joint, STR("GetConstrainedComponents"));
                if (who.run())
                {
                    log.joined.clear();
                    for (const TCHAR* pn : {STR("OutComponent1"), STR("OutBoneName1"), STR("OutComponent2"), STR("OutBoneName2")})
                    {
                        FProperty* pp = who.param(pn);
                        if (!pp) continue;
                        uint8_t* at = who.buffer.data() + pp->GetOffset_Internal();
                        if (pp->GetClass().GetName() == STR("NameProperty")) log.joined += reinterpret_cast<FName*>(at)->ToString() + STR(" ");
                        else
                        {
                            UObject* o = *reinterpret_cast<UObject**>(at);
                            log.joined += (o ? o->GetName() : std::wstring(STR("none"))) + STR(":");
                        }
                    }
                }
                else
                    log.joined = STR("(no GetConstrainedComponents)");
            }
            // How far the weapon is from where the hand holds it, from the joint's own force.
            Call f(joint, STR("GetConstraintForce"));
            if (f.run())
            {
                Vec3 lf = f.vec(STR("OutLinearForce"));
                log.force = length(lf);
                Call br(joint, STR("IsBroken"));
                if (br.run())
                    if (FProperty* rp = br.param(STR("ReturnValue"))) log.broken = *(br.buffer.data() + rp->GetOffset_Internal()) ? 1 : 0;

            }
            // Buzz with the joint's stretch: nothing up to pose_grip_buzz_from_cm, full 13 cm past it.
            ControllerState state = controllers_state();
            if (state.hand[side].grip_valid && log.stretch > cfg.pose_grip_buzz_from_cm)
                controllers_buzz(side, (float)(std::min)(1.0, (log.stretch - cfg.pose_grip_buzz_from_cm) / 13.0) * (float)cfg.pose_grip_buzz);
        }
        static double last = 0.0;
        double now = now_seconds();
        if (now - last >= 0.5 && (g_grip_log[0].active || g_grip_log[1].active))
        {
            last = now;
            double rate[2] = {-1, -1};
            if (FProperty* p = pawn->GetPropertyByNameInChain(STR("Grab L Release Rate"))) rate[0] = number_property(pawn, p);
            if (FProperty* p = pawn->GetPropertyByNameInChain(STR("Grab R Release Rate"))) rate[1] = number_property(pawn, p);
            HSVR_LOG("Grip: left %s mass %.2f kg force %.0f gap %.1f cm slip %.2f, weapon %.0f cm from hand, simulating %d, scale %.2f; "
                     "right %s mass %.2f kg force %.0f gap %.1f cm slip %.2f, weapon %.0f cm from hand, simulating %d, scale %.2f",
                     g_grip_log[0].active ? "spring" : "-", g_grip_log[0].mass, g_grip_log[0].force, g_grip_log[0].gap, rate[0], g_grip_log[0].hand_dist,
                     g_grip_log[0].simulating, g_grip_log[0].scale, g_grip_log[1].active ? "spring" : "-", g_grip_log[1].mass, g_grip_log[1].force,
                     g_grip_log[1].gap, rate[1], g_grip_log[1].hand_dist, g_grip_log[1].simulating, g_grip_log[1].scale);
            if (g_grip_log[1].active && g_contact_log.load())
                if (UObject* w = object_property(pawn, STR("Weapon R")))
                    log_weapon_contacts(pawn, w, object_property(w, STR("BaseMesh")));
            for (int side = 0; side < 2; ++side)
                if (g_grip_log[side].active)
                {
                    HSVR_LOG("Grip: %s weapon at (%.0f, %.0f, %.0f), hand at (%.0f, %.0f, %.0f), visible %d, joint between %ls", side == 0 ? "left" : "right",
                             g_grip_log[side].weapon_at.x, g_grip_log[side].weapon_at.y, g_grip_log[side].weapon_at.z, g_grip_log[side].hand_at.x,
                             g_grip_log[side].hand_at.y, g_grip_log[side].hand_at.z, g_grip_log[side].visible, g_grip_log[side].joined.c_str());
                    HSVR_LOG("Grip: %s joint stretch %.1f cm (%.1f, %.1f, %.1f) world, (%.1f, %.1f, %.1f) hand frame, hand anchor Pos2 (%.1f, %.1f, %.1f) [spring %d, k x%g, gravity %d, collision %d, target (%g, %g, %g), broken %d, arm ignored %d, posed mesh is the joint's %d, hand bones apart %.1f]",
                             side == 0 ? "left" : "right", g_grip_log[side].stretch, g_grip_log[side].stretch_vec.x, g_grip_log[side].stretch_vec.y, g_grip_log[side].stretch_vec.z,
                             g_grip_log[side].stretch_hand.x, g_grip_log[side].stretch_hand.y, g_grip_log[side].stretch_hand.z,
                             g_grip_log[side].pos2.x, g_grip_log[side].pos2.y, g_grip_log[side].pos2.z, g_grip_spring_override.load(),
                             g_grip_k_scale.load(), g_weapon_gravity.load(), g_weapon_collision.load(), g_grip_target[0].load(), g_grip_target[1].load(),
                             g_grip_target[2].load(), g_grip_log[side].broken, (int)g_arm_ignore[side].joints.size(),
                             g_grip_log[side].same_mesh, g_grip_log[side].mesh_hand_diff);
                    HSVR_LOG("Grip: %s turning error %.1f deg", side == 0 ? "left" : "right", g_grip_log[side].turn_err_deg);
                    if (g_hold[side].weapon)
                        HSVR_LOG("Hold: %s point held %.1f cm and %.1f deg behind the controller, hand force %.0f N%s, torque %.1f N m%s, dt %.4f, "
                                 "stopped %.1f cm, hand on weapon %.2f, contact miss %.2f cm",
                                 side == 0 ? "left" : "right", g_hold[side].lag, g_hold[side].turn_deg, g_hold[side].force_n,
                                 g_hold[side].force_capped ? " (at its cap)" : "", g_hold[side].torque_nm, g_hold[side].torque_capped ? " (at its cap)" : "",
                                 g_hold[side].dt, g_hold[side].blocked_cm, g_hold[side].on_weapon, g_hold[side].miss_cm);
                    HSVR_LOG("Grip: %s bodies: hand body centre (%.1f, %.1f, %.1f) in the hand bone's frame, weapon body centre (%.1f, %.1f, %.1f) in its frame",
                             side == 0 ? "left" : "right", g_grip_log[side].hand_com_local.x, g_grip_log[side].hand_com_local.y,
                             g_grip_log[side].hand_com_local.z, g_grip_log[side].weapon_com_local.x, g_grip_log[side].weapon_com_local.y,
                             g_grip_log[side].weapon_com_local.z);
                }
        }
    }

    void capsule_after_tick(Hook::TCallbackIterationData<void>&, UObject* context, UFunction* function, void*)
    {
        if (!is_receive_tick(context, function)) return;
        for (auto& pin : g_pawn_pins) set_variable(context, pin.first.c_str(), pin.second, nullptr, nullptr);
        if (g_pose_active.load())
        {
            crouch_after_tick(context);
            weapon_spring_after_tick(context);
        }
        if (!g_hold_have || !g_hold_capsule.load()) return;
        g_hold_have = false;
        using namespace hsvr;
        Vec3 now_at = actor_location(context);
        // Sideways the capsule goes back where it was. Its height is the game's own: it sets it
        // from where his feet stand (so he lands and follows the floor), except a rise of more
        // than 30 cm in one tick (the game's climb over an opponent's head), which is undone.
        Vec3 moved{now_at.x - g_hold_location.x, now_at.y - g_hold_location.y, 0.0};
        double rise = now_at.z - g_hold_location.z;
        Vec3 keep{g_hold_location.x, g_hold_location.y, rise > 30.0 ? g_hold_location.z : now_at.z};
        static double last_line = 0.0;
        static double moved_max = 0.0;
        static int climbs = 0;
        moved_max = (std::max)(moved_max, length(moved));
        if (rise > 30.0) ++climbs;
        double now = now_seconds();
        if (now - last_line >= 0.25)
        {
            last_line = now;
            std::string values;
            for (const TCHAR* name : {STR("Pain Stumble Immediate"), STR("Pain Stumble Delayed"), STR("Stumble Direction"), STR("Dodge Vector"),
                                      STR("Capsule Max Dislocation"), STR("Climbing In Air")})
            {
                FProperty* p = context->GetPropertyByNameInChain(name);
                char part[96];
                std::snprintf(part, sizeof(part), ", %ls %.1f", name, p ? number_property(context, p) : -1.0);
                values += part;
            }
            HSVR_LOG("Capsule: the game moved it sideways up to %.1f cm in a tick (put back), height %.1f, climbs undone %d%s", moved_max, keep.z, climbs,
                     values.c_str());
            moved_max = 0.0;
            climbs = 0;
        }
        if (length(moved) > 0.01 || keep.z != now_at.z)
        {
            Call put(context, STR("K2_SetActorLocation"));
            put.three(STR("NewLocation"), keep.x, keep.y, keep.z);
            put.flag(STR("bSweep"), false);
            put.flag(STR("bTeleport"), true);
            put.run();
        }
        for (const TCHAR* name : {STR("Pain Stumble Immediate"), STR("Pain Stumble Delayed"), STR("Stumble Direction"), STR("Dodge Vector")})
        {
            FProperty* p = context->GetPropertyByNameInChain(name);
            if (p && p->GetClass().GetName() == STR("StructProperty"))
                std::memset(reinterpret_cast<uint8_t*>(context) + p->GetOffset_Internal(), 0, p->GetSize());
        }
        // His speed is cleared so the game's pull doesn't carry over, except while you walk with the
        // stick (and half a second after, so he slows down by the game's own braking).
        if (!g_stick_walking && now_seconds() > g_stick_walk_until)
            if (UObject* movement = object_property(context, STR("CharacterMovement")))
                if (FProperty* vp = movement->GetPropertyByNameInChain(STR("Velocity")))
                    write_three(reinterpret_cast<uint8_t*>(movement) + vp->GetOffset_Internal(), vp, 0.0, 0.0, 0.0);
    }

    // ---- Your own head, hidden from your view only --------------------------------------------
    // The head is its own component (CharacterMesh_Head) and the hair another (Hair, a groom);
    // helmets are separate armour pieces and stay visible. Under the skin the game draws a whole
    // gore skeleton (SK_Skeleton: flesh, organs, bones, with the skull, jaw and teeth), which shows
    // from inside once the head is hidden, so it is hidden from your view too (your own exposed
    // bones in a wound then don't show to you; everyone else still sees them). "Owner no see" hides a component only in
    // the views of the player who owns it; "cast hidden shadow" keeps its shadow. The game makes new
    // components on a haircut or armour change, so this is checked once a second.
    void set_owner_hidden(UObject* component, bool hidden)
    {
        Call see(component, STR("SetOwnerNoSee"));
        see.first_flag(hidden);
        see.run();
        Call shadow(component, STR("SetCastHiddenShadow"));
        shadow.first_flag(hidden);
        shadow.run();
    }

    void hide_own_head(const hsvr::Config& cfg)
    {
        constexpr int N = 2;
        static UObject* hidden[N] = {nullptr, nullptr};
        static double last = 0.0;
        double now = hsvr::now_seconds();
        if (now - last < 1.0) return;
        last = now;
        UObject* pawn = nullptr;
        find_pawn_mesh(&pawn);
        bool willie = pawn && pawn->GetFullName().find(STR("Willie_BP")) != std::wstring::npos;
        bool want = cfg.hide_own_head && willie && cfg.anchor == "head" && !hsvr::shared().third_person.load() && hsvr::shared().stereo_installed.load();
        UObject* parts[N] = {nullptr, nullptr};
        if (willie)
        {
            parts[0] = object_property(pawn, STR("CharacterMesh_Head"));
            parts[1] = object_property(pawn, STR("Hair"));
        }
        // The gore skeleton: only its part from hide_own_gore_from (the neck) up is hidden, by
        // folding those bones away, so your own wounds elsewhere still show their bones. This is
        // not owner-only, but nobody else looks out from inside your head, and your head is
        // hidden from you anyway.
        {
            static UObject* gore = nullptr;
            static std::string gore_bone;
            UObject* skeleton = willie ? object_property(pawn, STR("SK_Skeleton")) : nullptr;
            bool hide_gore = want && skeleton && !cfg.hide_own_gore_from.empty();
            auto bone_call = [](UObject* c, const TCHAR* fn, const std::string& bone) {
                Call call(c, fn);
                call.name(STR("BoneName"), bone_fname(bone));
                call.run();
                return call;
            };
            if (gore && (gore != skeleton || !hide_gore || gore_bone != cfg.hide_own_gore_from))
            {
                if (gore == skeleton) bone_call(gore, STR("UnHideBoneByName"), gore_bone);
                HSVR_LOG("Head: gore skeleton from %s up shown again", gore_bone.c_str());
                gore = nullptr;
            }
            if (hide_gore)
            {
                Call check = bone_call(skeleton, STR("IsBoneHiddenByName"), cfg.hide_own_gore_from);
                bool already = false;
                if (FProperty* r = check.param(STR("ReturnValue")))
                    already = (check.buffer[r->GetOffset_Internal()] & static_cast<FBoolProperty*>(r)->GetFieldMask()) != 0;
                if (!already)
                {
                    if (!bone_exists(skeleton, cfg.hide_own_gore_from))
                        HSVR_LOG_EVERY(30.0, "Head: the gore skeleton has no bone %s; set hide_own_gore_from", cfg.hide_own_gore_from.c_str());
                    else
                    {
                        Call hide(skeleton, STR("HideBoneByName"));
                        hide.name(STR("BoneName"), bone_fname(cfg.hide_own_gore_from));
                        hide.run(); // PhysBodyOption stays 0: no physics change
                        HSVR_LOG_EVERY(10.0, "Head: gore skeleton (%ls) hidden from bone %s up", skeleton->GetFullName().c_str(), cfg.hide_own_gore_from.c_str());
                    }
                }
                gore = skeleton;
                gore_bone = cfg.hide_own_gore_from;
            }
        }
        for (int i = 0; i < N; ++i)
        {
            UObject* now_part = want ? parts[i] : nullptr;
            if (hidden[i] && hidden[i] != now_part && hidden[i] == parts[i])
            {
                set_owner_hidden(hidden[i], false);
                HSVR_LOG("Head: %ls shown in your view again", hidden[i]->GetFullName().c_str());
            }
            if (now_part && hidden[i] != now_part)
            {
                set_owner_hidden(now_part, true);
                HSVR_LOG("Head: %ls hidden from your view (shadow kept)", now_part->GetFullName().c_str());
            }
            hidden[i] = now_part;
        }
    }

    struct ConsoleCommandParams
    {
        UObject* WorldContextObject;
        FString Command;
        UObject* SpecificPlayer;
    };

    void run_console_command(const TCHAR* command)
    {
        static UFunction* fn = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Engine.KismetSystemLibrary:ExecuteConsoleCommand"));
        static UObject* cdo = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetSystemLibrary"));
        UObject* viewport = object_property(g_engine, STR("GameViewport"));
        if (!fn || !cdo || !viewport) return;
        ConsoleCommandParams params{viewport, FString(command), nullptr};
        cdo->ProcessEvent(fn, &params);
    }

    void run_console_command(const std::string& command)
    {
        std::wstring wide(command.begin(), command.end());
        run_console_command(wide.c_str());
    }

    struct ConsoleVariableParams
    {
        FString VariableName;
        float ReturnValue;
    };

    // The value of an engine setting, as a number. A setting this game does not have reads as 0.
    float console_variable(const TCHAR* name)
    {
        static UFunction* fn =
                UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Engine.KismetSystemLibrary:GetConsoleVariableFloatValue"));
        static UObject* cdo = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetSystemLibrary"));
        if (!fn || !cdo) return -999.0f;
        ConsoleVariableParams params{FString(name), 0.0f};
        cdo->ProcessEvent(fn, &params);
        return params.ReturnValue;
    }

    // Writes the game's picture settings to the log, so that sharpness can be tuned from facts.
    void log_graphics(const char* reason)
    {
        static const TCHAR* const names[] = {
                STR("r.ScreenPercentage"),
                STR("r.ScreenPercentage.Default"),
                STR("r.ScreenPercentage.Default.VR.Mode"),
                STR("r.SecondaryScreenPercentage.GameViewport"),
                STR("r.AntiAliasingMethod"),
                STR("r.TemporalAA.Upsampling"),
                STR("r.TSR.History.ScreenPercentage"),
                STR("r.Tonemapper.Sharpen"),
                STR("r.Tonemapper.Quality"),
                STR("r.SceneColorFringeQuality"),
                STR("r.Tonemapper.GrainQuantization"),
                STR("r.FilmGrain"),
                STR("r.BloomQuality"),
                STR("r.LensFlareQuality"),
                STR("r.MotionBlurQuality"),
                STR("r.DepthOfFieldQuality"),
                STR("r.VolumetricCloud"),
                STR("r.VolumetricRenderTarget"),
                STR("r.VolumetricRenderTarget.Mode"),
                STR("r.VolumetricRenderTarget.UpsamplingMode"),
                STR("Slate.CopyBackbufferToSlatePostRenderTargets"),
                STR("r.TSR.ShadingRejection.Flickering"),
                STR("r.TSR.History.UpdateQuality"),
                STR("r.DynamicRes.OperationMode"),
                STR("r.NGX.DLSS.Enable"),
                STR("r.NGX.DLAA.Enable"),
                STR("r.Streamline.DLSSG.Enable"),
                STR("r.FidelityFX.FSR3.Enabled"),
                STR("r.FidelityFX.FSR3.QualityMode"),
                STR("r.FidelityFX.FI.Enabled"),
                STR("r.DynamicGlobalIlluminationMethod"),
                STR("r.ReflectionMethod"),
                STR("r.Lumen.DiffuseIndirect.Allow"),
                STR("r.Lumen.Reflections.Allow"),
                STR("r.Lumen.HardwareRayTracing"),
                STR("r.Lumen.TraceMeshSDFs.Allow"),
                STR("r.Lumen.ScreenProbeGather.DownsampleFactor"),
                STR("r.Lumen.ScreenProbeGather.TracingOctahedronResolution"),
                STR("r.Lumen.ScreenProbeGather.IrradianceFormat"),
                STR("r.Lumen.ScreenProbeGather.StochasticInterpolation"),
                STR("r.Lumen.ScreenProbeGather.FullResolutionJitterWidth"),
                STR("r.Lumen.ScreenProbeGather.FixedJitterIndex"),
                STR("r.Lumen.ScreenProbeGather.ScreenTraces"),
                STR("r.Lumen.ScreenProbeGather.ScreenTraces.HZBTraversal.FullResDepth"),
                STR("r.Lumen.ScreenProbeGather.ShortRangeAO"),
                STR("r.Lumen.ScreenProbeGather.Temporal.MaxFramesAccumulated"),
                STR("r.Lumen.ScreenProbeGather.TemporalFilterProbes"),
                STR("r.Lumen.ScreenProbeGather.TwoSidedFoliageBackfaceDiffuse"),
                STR("r.Lumen.ScreenProbeGather.RadianceCache.ProbeResolution"),
                STR("r.LumenScene.Radiosity.ProbeSpacing"),
                STR("r.DistanceFieldAO"),
                STR("r.AOQuality"),
                STR("r.Shadow.Virtual.Enable"),
                STR("r.MaxAnisotropy"),
                STR("r.MipMapLODBias"),
                STR("r.VSync"),
                STR("t.MaxFPS"),
                STR("sg.ResolutionQuality"),
                STR("sg.ViewDistanceQuality"),
                STR("sg.AntiAliasingQuality"),
                STR("sg.ShadowQuality"),
                STR("sg.GlobalIlluminationQuality"),
                STR("sg.ReflectionQuality"),
                STR("sg.PostProcessQuality"),
                STR("sg.TextureQuality"),
                STR("sg.EffectsQuality"),
                STR("sg.FoliageQuality"),
                STR("sg.ShadingQuality"),
        };
        HSVR_LOG("Graphics (%s), the engine's picture settings (one the game does not have reads 0):", reason);
        for (const TCHAR* name : names) HSVR_LOG("  Graphics: %ls = %.3f", name, console_variable(name));
        HSVR_LOG("Graphics: end of list");
    }

    // Passes the picture tuning from the settings file on to the engine, when it is set and
    // whenever it changes (the file can be edited while the game runs).
    void apply_picture_settings(const hsvr::Config& cfg)
    {
        static double applied_sharpen = -1.0, applied_percentage = -1.0;
        static std::string applied_commands;
        char command[96];
        if (cfg.sharpen >= 0.0 && cfg.sharpen != applied_sharpen)
        {
            applied_sharpen = cfg.sharpen;
            snprintf(command, sizeof(command), "r.Tonemapper.Sharpen %.2f", cfg.sharpen);
            run_console_command(std::string(command));
            HSVR_LOG("Picture: %s", command);
        }
        if (cfg.screen_percentage >= 0.0 && cfg.screen_percentage != applied_percentage)
        {
            applied_percentage = cfg.screen_percentage;
            snprintf(command, sizeof(command), "r.ScreenPercentage %.0f", cfg.screen_percentage);
            run_console_command(std::string(command));
            HSVR_LOG("Picture: %s", command);
        }
        if (cfg.extra_commands != applied_commands)
        {
            applied_commands = cfg.extra_commands;
            size_t start = 0;
            while (start < applied_commands.size())
            {
                size_t end = applied_commands.find(';', start);
                if (end == std::string::npos) end = applied_commands.size();
                std::string one = applied_commands.substr(start, end - start);
                size_t first = one.find_first_not_of(" \t");
                size_t last = one.find_last_not_of(" \t");
                if (first != std::string::npos)
                {
                    one = one.substr(first, last - first + 1);
                    run_console_command(one);
                    HSVR_LOG("Picture: extra command: %s", one.c_str());
                }
                start = end + 1;
            }
        }
    }

    // Unreal 5.4 draws volumetric clouds per view in one of four modes. In mode 1 (and 3) it looks
    // up the scene's depth for a view at the wrong place when that view does not start at the
    // left edge of the picture, which is the case for our second eye: the clouds then land in
    // front of everything in that eye. Modes 0 and 2 are right. Half Sword switches to mode 1
    // when an outdoor arena loads, so the mode is checked every frame and replaced. Mode 2 is the
    // default replacement: mode 0 lays a half-size cloud picture over the scene, and cloud then
    // lands on thin far things against the sky (a steeple, leaves), which flicker.
    void keep_clouds_correct(const hsvr::Config& cfg)
    {
        if (!cfg.fix_clouds) return;
        // Modes 1 and 3 are the faulty ones; 0 and 2 are right in both eyes (ini cloud_mode).
        float target = cfg.cloud_mode == 2 ? 2.0f : 0.0f;
        static float last_set = -1.0f;
        float mode = console_variable(STR("r.VolumetricRenderTarget.Mode"));
        if (mode < -900.0f || mode == target) return;
        // Leave a good mode the game chose itself alone; replace a faulty one, and one we set
        // ourselves when the setting has since changed.
        if (mode != 1.0f && mode != 3.0f && mode != last_set) return;
        run_console_command(target == 2.0f ? STR("r.VolumetricRenderTarget.Mode 2") : STR("r.VolumetricRenderTarget.Mode 0"));
        last_set = target;
        static int lines_left = 20;
        if (lines_left > 0)
        {
            --lines_left;
            HSVR_LOG("Clouds: cloud mode was %g, set to %g so that both eyes show them behind the scenery (now reads %g)", (double)mode, (double)target,
                     (double)console_variable(STR("r.VolumetricRenderTarget.Mode")));
        }
    }

    // Half Sword runs the engine's bounce lighting (Lumen global illumination) with one light
    // sample per 64 pixels. On a monitor that is about 30 samples across the picture; in the
    // headset each eye is drawn only about 1000 pixels wide, which leaves about 16. Far thin
    // things against the sky (a steeple, leaves) then catch a different sample from frame to frame
    // and flicker, and plants are lit in blotches. The player found this with a key that stepped
    // through tests (since removed): lighting off stopped the flicker, twice as dense samples
    // stopped most of it and made the plants look better, and of 32, 16 and 8 he preferred 16.
    // The game sets its value again when a map loads, so it is checked every frame.
    void keep_light_samples(const hsvr::Config& cfg)
    {
        if (cfg.light_samples <= 0) return;
        float target = (float)cfg.light_samples;
        static float last_set = -1.0f;
        float value = console_variable(STR("r.Lumen.ScreenProbeGather.DownsampleFactor"));
        if (value < -900.0f || value == target) return;
        // Lower a coarser value; leave a finer one the game chose alone, but follow the setting
        // when it is changed while the game runs.
        if (value < target && value != last_set) return;
        char command[96];
        snprintf(command, sizeof(command), "r.Lumen.ScreenProbeGather.DownsampleFactor %d", cfg.light_samples);
        run_console_command(std::string(command));
        last_set = target;
        static int lines_left = 20;
        if (lines_left > 0)
        {
            --lines_left;
            HSVR_LOG("Lighting: one bounce light sample per %g pixels, set to %g so that far thin things do not flicker in the headset (now reads %g)",
                     (double)value, (double)target, (double)console_variable(STR("r.Lumen.ScreenProbeGather.DownsampleFactor")));
        }
    }

    // ---- unattended tests (ini dev_script) -------------------------------------------------------
    // Runs console commands at set times, so that a test needs no key or click.
    struct BoolReturnParams
    {
        bool ReturnValue;
    };

    struct ScriptEntry
    {
        double at = 0.0;
        std::string command;
        bool done = false;
        bool after_menu = false; // "M+<s>:" steps count from when the first menu is on screen
    };

    // dev_script step "hsvr.set <Name>=<value>": writes one variable of the game instance (GI_Settings
    // in Half Sword), the way the menus do before a fight is opened. <Name> is the variable's name as
    // the Blueprint shows it, any case; a name the engine stores with a suffix ("<Name>_12_ABC...") also matches.
    // Yes/no, byte, enum and whole-number variables only.
    void script_set_game_instance(const std::string& step)
    {
        size_t equals = step.find('=');
        if (equals == std::string::npos)
        {
            HSVR_LOG("Script: hsvr.set needs <Name>=<value>: %s", step.c_str());
            return;
        }
        std::string name_text = step.substr(0, equals);
        while (!name_text.empty() && name_text.back() == ' ') name_text.pop_back();
        std::string value_text = step.substr(equals + 1);
        std::wstring wanted(name_text.begin(), name_text.end());
        std::wstring wanted_prefix = wanted + STR("_");

        UObject* viewport = object_property(g_engine, STR("GameViewport"));
        UObject* game_instance = object_property(viewport, STR("GameInstance"));
        if (!game_instance)
        {
            HSVR_LOG("Script: hsvr.set: no game instance yet");
            return;
        }
        UClass* game_class = game_instance->GetClassPrivate();
        FProperty* found = nullptr;
        for (UStruct* level = game_class; level && !found; level = level->GetSuperStruct())
        {
            for (FProperty* property : level->ForEachProperty())
            {
                auto property_name = property->GetName();
                // Case is ignored: the editor shows names as typed, and they are easy to mistype.
                if (_wcsicmp(property_name.c_str(), wanted.c_str()) == 0 ||
                    _wcsnicmp(property_name.c_str(), wanted_prefix.c_str(), wanted_prefix.size()) == 0)
                {
                    found = property;
                    break;
                }
            }
        }
        if (!found)
        {
            HSVR_LOG("Script: hsvr.set: %ls has no variable \"%s\". Its variables:", game_instance->GetFullName().c_str(), name_text.c_str());
            for (FProperty* property : game_class->ForEachProperty())
                HSVR_LOG("  %ls (%ls)", property->GetName().c_str(), property->GetClass().GetName().c_str());
            return;
        }
        auto type = found->GetClass().GetName();
        uint8_t* address = reinterpret_cast<uint8_t*>(game_instance) + found->GetOffset_Internal();
        if (type == STR("BoolProperty"))
        {
            auto* flag = static_cast<FBoolProperty*>(found);
            uint8_t* byte = address + flag->GetByteOffset();
            bool before = (*byte & flag->GetFieldMask()) != 0;
            bool value = value_text.find("true") != std::string::npos || atoi(value_text.c_str()) != 0;
            if (value) *byte |= flag->GetFieldMask();
            else *byte &= ~flag->GetFieldMask();
            HSVR_LOG("Script: game instance %ls (yes/no) was %d, now %d", found->GetName().c_str(), (int)before, (int)value);
            return;
        }
        if (type == STR("ByteProperty") || type == STR("EnumProperty") || type == STR("IntProperty"))
        {
            int32_t size = found->GetSize();
            if (size != 1 && size != 4)
            {
                HSVR_LOG("Script: hsvr.set: %ls is %ls of %d bytes, not written", found->GetName().c_str(), type.c_str(), size);
                return;
            }
            int64_t value = atoll(value_text.c_str());
            int64_t before = size == 1 ? (int64_t)*address : (int64_t)*reinterpret_cast<int32_t*>(address);
            if (size == 1) *address = (uint8_t)value;
            else *reinterpret_cast<int32_t*>(address) = (int32_t)value;
            HSVR_LOG("Script: game instance %ls (%ls) was %lld, now %lld", found->GetName().c_str(), type.c_str(), (long long)before, (long long)value);
            return;
        }
        HSVR_LOG("Script: hsvr.set: %ls is a %ls, which this step does not write", found->GetName().c_str(), type.c_str());
    }

    // dev_script step "hsvr.dump": writes every reflected variable of the player's character, its
    // components, its animation Blueprint, its class defaults, the controller and the game instance to
    // the log ("Dump <object>: <name> (<type>) = <value>"), to read the values the game really runs
    // with (Blueprint defaults could not be decoded from the pak). Read only.
    void dump_value(const std::wstring& label, const std::wstring& path, FProperty* property, const uint8_t* address, int depth)
    {
        auto type = property->GetClass().GetName();
        wchar_t value[256] = L"?";
        if (type == STR("BoolProperty"))
        {
            auto* flag = static_cast<FBoolProperty*>(property);
            swprintf(value, 256, L"%d", (address[flag->GetByteOffset()] & flag->GetFieldMask()) != 0 ? 1 : 0);
        }
        else if (type == STR("FloatProperty")) swprintf(value, 256, L"%g", (double)*reinterpret_cast<const float*>(address));
        else if (type == STR("DoubleProperty")) swprintf(value, 256, L"%g", *reinterpret_cast<const double*>(address));
        else if (type == STR("IntProperty")) swprintf(value, 256, L"%d", *reinterpret_cast<const int32_t*>(address));
        else if (type == STR("Int64Property")) swprintf(value, 256, L"%lld", (long long)*reinterpret_cast<const int64_t*>(address));
        else if (type == STR("ByteProperty") || (type == STR("EnumProperty") && property->GetSize() == 1)) swprintf(value, 256, L"%d", (int)*address);
        else if (type == STR("NameProperty")) swprintf(value, 256, L"%ls", reinterpret_cast<const FName*>(address)->ToString().c_str());
        else if (type == STR("ObjectProperty") || type == STR("ClassProperty"))
        {
            UObject* object = *reinterpret_cast<UObject* const*>(address);
            swprintf(value, 256, L"%ls", object ? object->GetFullName().c_str() : L"none");
        }
        else if (type == STR("ArrayProperty")) swprintf(value, 256, L"%d entries", reinterpret_cast<const RawArray*>(address)->Num);
        else if (type == STR("StructProperty") && depth < 3)
        {
            UScriptStruct* inner = static_cast<FStructProperty*>(property)->GetStruct();
            if (inner)
            {
                for (FProperty* member : inner->ForEachProperty())
                    dump_value(label, path + property->GetName() + STR("."), member, address + member->GetOffset_Internal(), depth + 1);
                return;
            }
        }
        HSVR_LOG("Dump %ls: %ls%ls (%ls) = %ls", label.c_str(), path.c_str(), property->GetName().c_str(), type.c_str(), value);
    }

    void dump_object(const std::wstring& label, UObject* object)
    {
        if (!object)
        {
            HSVR_LOG("Dump %ls: none", label.c_str());
            return;
        }
        HSVR_LOG("Dump %ls: %ls", label.c_str(), object->GetFullName().c_str());
        const uint8_t* base = reinterpret_cast<const uint8_t*>(object);
        for (UStruct* level = object->GetClassPrivate(); level; level = level->GetSuperStruct())
            for (FProperty* property : level->ForEachProperty())
                dump_value(label, L"", property, base + property->GetOffset_Internal(), 0);
    }

    void script_dump()
    {
        UObject* pawn = nullptr;
        UObject* mesh = find_pawn_mesh(&pawn);
        UObject* controller = find_player_controller();
        UObject* viewport = object_property(g_engine, STR("GameViewport"));
        dump_object(STR("game instance"), object_property(viewport, STR("GameInstance")));
        dump_object(STR("controller"), controller);
        if (!pawn)
        {
            HSVR_LOG("Dump: no player character");
            return;
        }
        dump_object(STR("character"), pawn);
        dump_object(STR("character defaults"), pawn->GetClassPrivate()->GetClassDefaultObject());
        dump_object(STR("animation"), object_property(mesh, STR("AnimScriptInstance")));
        // Components: every object variable of the character that points at an object it owns.
        for (UStruct* level = pawn->GetClassPrivate(); level; level = level->GetSuperStruct())
        {
            for (FProperty* property : level->ForEachProperty())
            {
                if (property->GetClass().GetName() != STR("ObjectProperty")) continue;
                UObject* component = *reinterpret_cast<UObject* const*>(reinterpret_cast<const uint8_t*>(pawn) + property->GetOffset_Internal());
                if (!component) continue;
                // Owned by the character (components and other sub-objects), not things it points at.
                if (component->GetOuterPrivate() != pawn) continue;
                dump_object(STR("part ") + property->GetName(), component);
            }
        }
        HSVR_LOG("Dump: done");
    }

    // dev_script step "hsvr.call <Function>": calls a function of the game instance that takes no
    // parameters, e.g. GI_Settings "Save Game", which the Fight button calls before opening the arena
    // (the arena's game manager loads the save again on BeginPlay, undoing hsvr.set otherwise).
    void script_call_game_instance(const std::string& step)
    {
        std::string name_text = step;
        while (!name_text.empty() && name_text.back() == ' ') name_text.pop_back();
        std::wstring wanted(name_text.begin(), name_text.end());
        UObject* viewport = object_property(g_engine, STR("GameViewport"));
        UObject* game_instance = object_property(viewport, STR("GameInstance"));
        if (!game_instance)
        {
            HSVR_LOG("Script: hsvr.call: no game instance yet");
            return;
        }
        UFunction* function = game_instance->GetFunctionByNameInChain(wanted.c_str());
        if (!function)
        {
            HSVR_LOG("Script: hsvr.call: %ls has no function \"%s\"", game_instance->GetFullName().c_str(), name_text.c_str());
            return;
        }
        // Zeroed room for the parameter block; the functions this is meant for have none.
        std::vector<uint8_t> block = param_block(function);
        uint8_t* parameters = block.data();
        game_instance->ProcessEvent(function, parameters);
        HSVR_LOG("Script: called %ls on the game instance", function->GetFullName().c_str());
    }

    // dev_script step "hsvr.give_weapon <class path>": puts a weapon in the player's right hand the
    // way the game equips one, through Willie's "Set Up Right Hand Weapon" with the weapon class
    // (Willie_BP: called with a class for the start loadout, UG @24976). For unattended tests: the
    // scripted Free Mode start has no weapon. The class is loaded through the asset registry if it
    // isn't in memory yet. Example: /Game/.../ModularWeaponBP_ArmingSword.ModularWeaponBP_ArmingSword_C
    // Loads an asset by its object path through the asset registry, if it is not loaded yet.
    UObject* load_asset_object(const std::wstring& object_path)
    {
        UObject* found = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, object_path.c_str());
        if (found) return found;
        auto* registry = static_cast<UAssetRegistry*>(UAssetRegistryHelpers::GetAssetRegistry().ObjectPointer);
        if (!registry) return nullptr;
        FAssetData data = registry->GetAssetByObjectPath(FName(object_path.c_str(), FNAME_Add));
        if (!data.PackageName().GetComparisonIndex())
        {
            HSVR_LOG("Script: the asset registry has no %ls", object_path.c_str());
            return nullptr;
        }
        UAssetRegistryHelpers::GetAsset(data);
        return UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, object_path.c_str());
    }

    FProperty* class_member(UObject* object, const TCHAR* name, bool prefix = false)
    {
        if (!object) return nullptr;
        std::wstring want = name;
        for (UStruct* level = object->GetClassPrivate(); level; level = level->GetSuperStruct())
            for (FProperty* member : level->ForEachProperty())
            {
                std::wstring n = member->GetName();
                if (prefix ? n.rfind(want, 0) == 0 : n == want) return member;
            }
        return nullptr;
    }

    FProperty* struct_member_prefix(UScriptStruct* type, const TCHAR* prefix)
    {
        if (!type) return nullptr;
        std::wstring want = prefix;
        for (UStruct* level = type; level; level = level->GetSuperStruct())
            for (FProperty* member : level->ForEachProperty())
                if (member->GetName().rfind(want, 0) == 0) return member;
        return nullptr;
    }

    // Finds a weapon passport in a Free Mode inventory data asset: Inventory.WeaponPssports[index].
    // The game spells the member "WeaponPssports". Returns the passport's bytes, or nullptr.
    const uint8_t* inventory_passport(const std::wstring& asset_path, int index, int32_t* count)
    {
        *count = 0;
        UObject* asset = load_asset_object(asset_path);
        if (!asset)
        {
            HSVR_LOG("Script: hsvr.give_weapon: could not load %ls", asset_path.c_str());
            return nullptr;
        }
        FProperty* inventory = class_member(asset, STR("Inventory"));
        UScriptStruct* type = struct_type(inventory);
        FProperty* list = struct_member_prefix(type, STR("WeaponPssports"));
        if (!list || list->GetClass().GetName() != STR("ArrayProperty"))
        {
            HSVR_LOG("Script: hsvr.give_weapon: %ls has no Inventory.WeaponPssports list", asset->GetFullName().c_str());
            return nullptr;
        }
        const RawArray* arr = reinterpret_cast<const RawArray*>(reinterpret_cast<uint8_t*>(asset) + inventory->GetOffset_Internal() + list->GetOffset_Internal());
        *count = arr->Num;
        if (index < 0 || index >= arr->Num || !arr->Data) return nullptr;
        return reinterpret_cast<const uint8_t*>(arr->Data);
    }

    void script_give_weapon(const std::string& step)
    {
        std::string path = step;
        while (!path.empty() && path.back() == ' ') path.pop_back();
        // "left ..." = the left hand (Set Up Left Hand Weapon); otherwise the right.
        bool left = path.rfind("left ", 0) == 0;
        if (left) path = path.substr(5);
        const TCHAR* setup_name = left ? STR("Set Up Left Hand Weapon") : STR("Set Up Right Hand Weapon");
        // "passport <inventory data asset> <index>": hand him a fully built weapon from that list.
        std::wstring passport_asset;
        int passport_index = 0;
        if (path.rfind("passport ", 0) == 0)
        {
            std::string rest = path.substr(9);
            size_t space = rest.find(' ');
            if (space != std::string::npos)
            {
                passport_index = atoi(rest.c_str() + space + 1);
                rest.resize(space);
            }
            passport_asset.assign(rest.begin(), rest.end());
        }
        std::wstring wide(path.begin(), path.end());
        UClass* cls = passport_asset.empty() ? UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, wide.c_str()) : nullptr;
        if (!cls && passport_asset.empty())
        {
            // The blueprint asset is the class path without "_C" on the object name.
            std::wstring asset = wide;
            if (asset.size() > 2 && asset.substr(asset.size() - 2) == STR("_C")) asset.resize(asset.size() - 2);
            auto* registry = static_cast<UAssetRegistry*>(UAssetRegistryHelpers::GetAssetRegistry().ObjectPointer);
            if (registry)
            {
                FAssetData data = registry->GetAssetByObjectPath(FName(asset.c_str(), FNAME_Add));
                if (data.PackageName().GetComparisonIndex())
                {
                    UObject* loaded = UAssetRegistryHelpers::GetAsset(data);
                    HSVR_LOG("Script: hsvr.give_weapon: loaded %ls", loaded ? loaded->GetFullName().c_str() : STR("nothing"));
                }
                else
                    HSVR_LOG("Script: hsvr.give_weapon: the asset registry has no %ls", asset.c_str());
            }
            cls = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, wide.c_str());
        }
        if (!cls && passport_asset.empty())
        {
            HSVR_LOG("Script: hsvr.give_weapon: no class %s", path.c_str());
            return;
        }
        // Set Up Hand Weapon calls weapon-only events on what it is given; anything else is a fatal
        // engine error (fist-eat test 1: an apple). Only ModularWeaponBP classes.
        if (cls)
        {
            bool weapon = false;
            for (UStruct* level = cls; level; level = level->GetSuperStruct())
                if (level->GetName() == STR("ModularWeaponBP_C")) weapon = true;
            if (!weapon)
            {
                HSVR_LOG("Script: hsvr.give_weapon: %s is not a ModularWeaponBP, not given (use hsvr.spawn)", path.c_str());
                return;
            }
        }
        UObject* pawn = nullptr;
        find_pawn_mesh(&pawn);
        UFunction* fn = pawn ? pawn->GetFunctionByNameInChain(setup_name) : nullptr;
        if (!fn)
        {
            HSVR_LOG("Script: hsvr.give_weapon: no character or no \"%ls\"", setup_name);
            return;
        }
        // Sized from the function. The fixed 4096-byte static block this replaced was overrun: the
        // hand position written for the fist landed on the grab button's state (fist-eat test 4).
        std::vector<uint8_t> block = param_block(fn);
        uint8_t* parameters = block.data();
        std::string names;
        bool set = false;
        for (FProperty* p : fn->ForEachProperty())
        {
            std::wstring n = p->GetName();
            std::wstring kind = p->GetClass().GetName();
            names += (names.empty() ? "" : ", ");
            for (wchar_t ch : n) names += (char)(ch < 128 ? ch : '?');
            names += " (";
            for (wchar_t ch : kind) names += (char)(ch < 128 ? ch : '?');
            names += ")";
            if (!passport_asset.empty() && !set && n.find(STR("Weapon Passport")) != std::wstring::npos)
            {
                int32_t count = 0;
                const uint8_t* first = inventory_passport(passport_asset, passport_index, &count);
                int32_t size = p->GetSize();
                if (first)
                {
                    // The list holds the same passport struct as this parameter, so one entry is this many bytes.
                    std::memcpy(parameters + p->GetOffset_Internal(), first + (size_t)size * passport_index, size);
                    set = true;
                    HSVR_LOG("Script: hsvr.give_weapon: passport %d of %d (%d bytes) from %ls", passport_index, count, size, passport_asset.c_str());
                    if (UScriptStruct* pt = struct_type(p))
                        for (FProperty* m : pt->ForEachProperty())
                        {
                            uint8_t* at = parameters + p->GetOffset_Internal() + m->GetOffset_Internal();
                            if (m->GetClass().GetName() == STR("ClassProperty"))
                            {
                                UObject* c = *reinterpret_cast<UObject**>(at);
                                HSVR_LOG("Script:   %ls = %ls", m->GetName().c_str(), c ? c->GetFullName().c_str() : STR("None"));
                            }
                        }
                }
                else
                    HSVR_LOG("Script: hsvr.give_weapon: no passport %d (the list has %d)", passport_index, count);
            }
            if (passport_asset.empty() && !set && kind == STR("ClassProperty"))
            {
                *reinterpret_cast<UClass**>(parameters + p->GetOffset_Internal()) = cls;
                set = true;
            }
            // A zeroed transform has scale 0 and no valid rotation: give it the right hand's spot.
            // Replace the weapon he holds instead of dropping it, so repeated tests don't pile swords up.
            if (n == STR("Destroy Previous") && kind == STR("BoolProperty"))
            {
                auto* flag = static_cast<FBoolProperty*>(p);
                parameters[p->GetOffset_Internal() + flag->GetByteOffset()] |= flag->GetFieldMask();
            }
            if (n == STR("Weapon Origin Transform"))
            {
                UScriptStruct* type = struct_type(p);
                uint8_t* at = parameters + p->GetOffset_Internal();
                hsvr::Vec3 hand{0, 0, 0};
                if (UObject* mesh = find_pawn_mesh()) hand = socket_location(mesh, bone_fname(left ? "hand_l" : "hand_r"));
                if (FProperty* r = struct_member(type, STR("Rotation")))
                {
                    UScriptStruct* qt = struct_type(r);
                    if (FProperty* w = struct_member(qt, STR("W")))
                    {
                        uint8_t* wat = at + r->GetOffset_Internal() + w->GetOffset_Internal();
                        if (w->GetClass().GetName() == STR("DoubleProperty")) *reinterpret_cast<double*>(wat) = 1.0;
                        else *reinterpret_cast<float*>(wat) = 1.0f;
                    }
                }
                if (FProperty* t = struct_member(type, STR("Translation"))) write_three(at + t->GetOffset_Internal(), t, hand.x, hand.y, hand.z);
                if (FProperty* sc = struct_member(type, STR("Scale3D"))) write_three(at + sc->GetOffset_Internal(), sc, 1.0, 1.0, 1.0);
            }
        }
        HSVR_LOG("Script: hsvr.give_weapon: parameters %s", names.c_str());
        if (!set)
        {
            HSVR_LOG("Script: hsvr.give_weapon: no class or passport given, not called");
            return;
        }
        pawn->ProcessEvent(fn, parameters);
        UObject* weapon = object_property(pawn, left ? STR("Weapon L") : STR("Weapon R"));
        HSVR_LOG("Script: hsvr.give_weapon: %s hand now holds %ls", left ? "left" : "right", weapon ? weapon->GetFullName().c_str() : STR("nothing"));
        // A bottle given this way comes empty (drink test 3: Liquid Amount In Bottle 0, the class
        // default 1): it gets its class default back, read from the default object.
        if (weapon && class_name_starts(weapon, STR("BP_Weapon_Consumable_Bottle")) && read_number_variable(weapon, STR("Liquid Amount In Bottle")) < 0.1)
        {
            std::wstring full = weapon->GetClassPrivate()->GetFullName(); // "BlueprintGeneratedClass /Game/.../BP_X.BP_X_C"
            size_t space = full.find(L' ');
            std::wstring path = space == std::wstring::npos ? full : full.substr(space + 1);
            size_t dot = path.rfind(L'.');
            UObject* cdo = dot == std::wstring::npos ? nullptr
                                                     : UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, (path.substr(0, dot + 1) + STR("Default__") + path.substr(dot + 1)).c_str());
            double full_amount = cdo ? read_number_variable(cdo, STR("Liquid Amount In Bottle")) : -1.0;
            if (full_amount > 0.0) set_variable(weapon, STR("Liquid Amount In Bottle"), full_amount);
            HSVR_LOG("Script: hsvr.give_weapon: the bottle came empty; filled to %.2f (class default %ls)", read_number_variable(weapon, STR("Liquid Amount In Bottle")),
                     cdo ? STR("found") : STR("NOT FOUND"));
        }
    }

    // Parses "<time>: <command>" steps separated by ';' or new lines. Times are seconds from `base`
    // (script time), or "M+<s>" from the first menu on screen.
    int parse_script_steps(const std::string& text, double base, std::vector<ScriptEntry>& entries)
    {
        int added = 0;
        size_t pos = 0;
        while (pos < text.size())
        {
            size_t end = text.find_first_of(";\n", pos);
            if (end == std::string::npos) end = text.size();
            std::string one = text.substr(pos, end - pos);
            pos = end + 1;
            size_t colon = one.find(':');
            if (colon == std::string::npos) continue;
            ScriptEntry entry;
            std::string when = one.substr(0, colon);
            size_t w0 = when.find_first_not_of(" \t\r\n");
            if (w0 == std::string::npos) continue;
            if (when[w0] == '#') continue;
            if (when[w0] == 'M' || when[w0] == 'm')
            {
                entry.after_menu = true;
                when = when.substr(w0 + 1);
                if (!when.empty() && when[0] == '+') when = when.substr(1);
            }
            entry.at = atof(when.c_str()) + (entry.after_menu ? 0.0 : base);
            entry.command = one.substr(colon + 1);
            size_t first = entry.command.find_first_not_of(" \t");
            size_t last = entry.command.find_last_not_of(" \t\r\n");
            if (first == std::string::npos) continue;
            entry.command = entry.command.substr(first, last - first + 1);
            entries.push_back(entry);
            ++added;
        }
        return added;
    }

    // ---- Variable watch (kick hunt, hsvr.watch_vars) -------------------------------------------
    // Every tick, the number, yes/no, byte and enum variables of the player's pawn and of the weapon
    // in each hand whose names contain one of the given words (case ignored) are read, and each
    // change is logged with the time, so a game routine can be matched to a kick by its timing.
    struct WatchedVar
    {
        UObject* owner;
        FProperty* property;
        std::string label;
        double value;
    };
    std::vector<std::wstring> g_watch_words;
    std::vector<WatchedVar> g_watched;
    std::vector<WatchedVar> g_watch_previous; // the list before a re-collect
    UObject* g_watch_owners[3] = {nullptr, nullptr, nullptr};

    double watch_read(UObject* owner, FProperty* p)
    {
        uint8_t* at = reinterpret_cast<uint8_t*>(owner) + p->GetOffset_Internal();
        auto kind = p->GetClass().GetName();
        if (kind == STR("DoubleProperty")) return *reinterpret_cast<double*>(at);
        if (kind == STR("FloatProperty")) return *reinterpret_cast<float*>(at);
        if (kind == STR("IntProperty")) return *reinterpret_cast<int32_t*>(at);
        if (kind == STR("ByteProperty") || kind == STR("EnumProperty")) return *at;
        if (kind == STR("BoolProperty"))
        {
            auto* flag = static_cast<FBoolProperty*>(p);
            return (at[flag->GetByteOffset()] & flag->GetFieldMask()) != 0 ? 1.0 : 0.0;
        }
        return 0.0;
    }

    void watch_collect(UObject* owner, const char* who)
    {
        if (!owner) return;
        int found = 0;
        for (UStruct* level = static_cast<UStruct*>(owner->GetClassPrivate()); level; level = level->GetSuperStruct())
            for (FProperty* p : level->ForEachProperty())
            {
                auto kind = p->GetClass().GetName();
                if (kind != STR("DoubleProperty") && kind != STR("FloatProperty") && kind != STR("IntProperty") && kind != STR("ByteProperty") &&
                    kind != STR("EnumProperty") && kind != STR("BoolProperty"))
                    continue;
                std::wstring name = p->GetName(), lower;
                for (wchar_t ch : name)
                    if (ch != L' ' && ch != L'_') lower += (wchar_t)towlower(ch); // words match with or without spaces
                bool match = false;
                for (auto& w : g_watch_words)
                    if (lower.find(w) != std::wstring::npos) match = true;
                if (!match) continue;
                std::string label = who;
                label += ".";
                for (wchar_t ch : name) label += (char)(ch < 128 ? ch : '?');
                double value = watch_read(owner, p);
                // A re-collect (a weapon changed hands) keeps what was already being watched, so a
                // change on that same tick is still logged; anything new logs where it starts.
                double before = value;
                bool known = false;
                for (auto& old : g_watch_previous)
                    if (old.owner == owner && old.property == p)
                    {
                        before = old.value;
                        known = true;
                    }
                if (!known) HSVR_LOG("Watch: %s starts at %g", label.c_str(), value);
                g_watched.push_back({owner, p, label, before});
                ++found;
            }
        HSVR_LOG("Watch: %d variables on %s (%ls)", found, who, owner->GetName().c_str());
    }

    void watch_vars_tick()
    {
        if (g_watch_words.empty()) return;
        UObject* pawn = g_pose.pawn;
        UObject* owners[3] = {pawn, pawn ? object_property(pawn, STR("Weapon L")) : nullptr, pawn ? object_property(pawn, STR("Weapon R")) : nullptr};
        if (owners[0] != g_watch_owners[0] || owners[1] != g_watch_owners[1] || owners[2] != g_watch_owners[2])
        {
            g_watch_previous.swap(g_watched);
            g_watched.clear();
            for (int i = 0; i < 3; ++i) g_watch_owners[i] = owners[i];
            watch_collect(owners[0], "pawn");
            watch_collect(owners[1], "weaponL");
            watch_collect(owners[2], "weaponR");
        }
        double now = hsvr::now_seconds();
        for (auto& v : g_watched)
        {
            double value = watch_read(v.owner, v.property);
            if (value == v.value) continue;
            HSVR_LOG("Watch: %.3f %s %g -> %g", now, v.label.c_str(), v.value, value);
            v.value = value;
        }
    }

    void run_dev_script(const hsvr::Config& cfg)
    {
        static bool parsed = false;
        static std::vector<ScriptEntry> entries;
        static ULONGLONG start = 0;
        if (!parsed)
        {
            parsed = true;
            start = GetTickCount64();
            int n = parse_script_steps(cfg.dev_script, 0.0, entries);
            if (n > 0) HSVR_LOG("Script: %d timed step(s) from the settings file (dev_script)", n);
        }
        // Live steps: HalfSwordVR_commands.txt next to the ini, checked once a second. Each time it is
        // written, its steps are added with their times counted from that moment, so one game launch
        // can run many tests (write a batch, wait, write the next).
        {
            static double checked = -10.0;
            static FILETIME seen{};
            double t = (double)(GetTickCount64() - start) / 1000.0;
            if (t - checked >= 1.0)
            {
                checked = t;
                std::wstring path = hsvr::mod_dir() + L"HalfSwordVR_commands.txt";
                WIN32_FILE_ATTRIBUTE_DATA info{};
                if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) && CompareFileTime(&info.ftLastWriteTime, &seen) != 0)
                {
                    bool first_look = seen.dwLowDateTime == 0 && seen.dwHighDateTime == 0;
                    seen = info.ftLastWriteTime;
                    // A file left over from before this launch is not run.
                    if (!first_look || t > 0.0)
                        if (FILE* f = _wfopen(path.c_str(), L"rb"))
                        {
                            std::string text;
                            char buf[4096];
                            size_t got;
                            while ((got = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, got);
                            fclose(f);
                            if (first_look)
                            {
                                HSVR_LOG("Script: %ls was already there at launch; not run (write it again to run it)", path.c_str());
                            }
                            else
                            {
                                int n = parse_script_steps(text, t, entries);
                                HSVR_LOG("Script: %d live step(s) from HalfSwordVR_commands.txt, counted from %.1f s", n, t);
                            }
                        }
                }
                else if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) && seen.dwLowDateTime == 0 && seen.dwHighDateTime == 0)
                    seen.dwLowDateTime = 1; // no file at launch: the first one written is new
            }
        }
        if (entries.empty()) return;
        double now = (double)(GetTickCount64() - start) / 1000.0;
        // The first menu on screen (the startup movie is not a widget, so this is the main menu):
        // "M+" steps count from here instead of a fixed wait that must cover the slowest load.
        static double menu_at = -1.0;
        static double menu_checked = -1.0;
        if (menu_at < 0.0 && now - menu_checked >= 0.25)
        {
            menu_checked = now;
            int shown = 0;
            std::vector<UObject*> widgets;
            UObjectGlobals::FindAllOf(STR("UserWidget"), widgets);
            for (UObject* widget : widgets)
            {
                UFunction* in_viewport_fn = widget ? widget->GetFunctionByNameInChain(STR("IsInViewport")) : nullptr;
                if (!in_viewport_fn) continue;
                BoolReturnParams in_viewport{};
                widget->ProcessEvent(in_viewport_fn, &in_viewport);
                if (in_viewport.ReturnValue) ++shown;
            }
            if (shown > 0)
            {
                menu_at = now;
                HSVR_LOG("Script: first menu on screen at %.1f s; M+ steps count from here", now);
            }
        }
        for (ScriptEntry& entry : entries)
        {
            if (entry.done) continue;
            if (entry.after_menu ? (menu_at < 0.0 || now - menu_at < entry.at) : now < entry.at) continue;
            entry.done = true;
            SYSTEMTIME utc{};
            GetSystemTime(&utc);
            HSVR_LOG("Script: %s%.0f s: %s (%02d:%02d:%02d.%03d UTC)", entry.after_menu ? "M+" : "", entry.at, entry.command.c_str(), (int)utc.wHour, (int)utc.wMinute, (int)utc.wSecond,
                     (int)utc.wMilliseconds);
            // Mod commands for unattended tests; everything else goes to the engine console.
            if (entry.command.rfind("hsvr.set ", 0) == 0)
            {
                script_set_game_instance(entry.command.substr(9));
                continue;
            }
            if (entry.command == "hsvr.dump")
            {
                script_dump();
                continue;
            }
            if (entry.command.rfind("hsvr.third_person ", 0) == 0)
            {
                // The game's own camera behind him (like the Insert key), so test pictures show his body.
                hsvr::shared().third_person.store(atoi(entry.command.c_str() + 18) != 0);
                continue;
            }
            if (entry.command.rfind("hsvr.grip_spring ", 0) == 0)
            {
                g_grip_spring_override.store(atoi(entry.command.c_str() + 17));
                continue;
            }
            if (entry.command.rfind("hsvr.ghost_show ", 0) == 0)
            {
                g_ghost_force.store(atoi(entry.command.c_str() + 16) != 0);
                continue;
            }
            if (entry.command == "hsvr.mark")
            {
                g_count_letgo.store(0);
                g_count_kicks.store(0);
                g_worst_lag[0].store(0.0);
                g_worst_lag[1].store(0.0);
                HSVR_LOG("Script: hsvr.mark: test counters cleared");
                continue;
            }
            if (entry.command.rfind("hsvr.expect ", 0) == 0)
            {
                // hsvr.expect <what> <op> <value> [label]; what: held_r, held_l, grabbed_r, grabbed_l,
                // letgo, kicks (since hsvr.mark), lag_r, lag_l (worst cm since hsvr.mark); op: < <= = >= >.
                char what[64] = {0}, op[4] = {0}, label[128] = {0};
                double want = 0.0;
                int got_n = sscanf(entry.command.c_str() + 12, "%63s %3s %lf %127[^\n]", what, op, &want, label);
                UObject* pawn = nullptr;
                find_pawn_mesh(&pawn);
                std::string w = what;
                double value = 0.0;
                bool known = true;
                if (w == "held_r") value = object_property(pawn, STR("Weapon R")) ? 1.0 : 0.0;
                else if (w == "held_l") value = object_property(pawn, STR("Weapon L")) ? 1.0 : 0.0;
                else if (w == "grabbed_r") value = bool_property(pawn, STR("Grabbed R")) ? 1.0 : 0.0;
                else if (w == "grabbed_l") value = bool_property(pawn, STR("Grabbed L")) ? 1.0 : 0.0;
                else if (w == "letgo") value = g_count_letgo.load();
                else if (w == "kicks") value = g_count_kicks.load();
                else if (w == "lag_r") value = g_worst_lag[1].load();
                else if (w == "lag_l") value = g_worst_lag[0].load();
                else if (w == "stamina") value = read_number_variable(pawn, STR("Stamina"));
                else if (w == "strength_r") value = g_hold[1].body_strength;
                else if (w == "strength_l") value = g_hold[0].body_strength;
                else if (w == "two_hand") value = g_two.active ? 1.0 : 0.0;
                else if (w == "fist_r") value = is_fist(object_property(pawn, STR("Weapon R"))) ? 1.0 : 0.0;
                else if (w == "fist_l") value = is_fist(object_property(pawn, STR("Weapon L"))) ? 1.0 : 0.0;
                else if (w == "food") value = read_number_variable(pawn, STR("Nutrients Due"));
                else if (w == "liquid") value = read_number_variable(pawn, STR("Body Liquid"));
                else known = false;
                std::string o = op;
                bool pass = false;
                if (o == "<") pass = value < want;
                else if (o == "<=") pass = value <= want;
                else if (o == "=" || o == "==") pass = std::fabs(value - want) < 1e-6;
                else if (o == ">=") pass = value >= want;
                else if (o == ">") pass = value > want;
                else known = false;
                if (got_n < 3 || !known) HSVR_LOG("EXPECT FAIL: could not read \"%s\"", entry.command.c_str());
                else HSVR_LOG("EXPECT %s: %s %s %g (is %g)%s%s", pass ? "PASS" : "FAIL", what, op, want, value, label[0] ? " - " : "", label);
                continue;
            }
            if (entry.command.rfind("hsvr.pawn_pin ", 0) == 0)
            {
                std::string rest = entry.command.substr(14);
                if (rest == "off")
                {
                    g_pawn_pins.clear();
                    HSVR_LOG("Script: hsvr.pawn_pin: all cleared");
                    continue;
                }
                size_t sp = rest.find_last_of(' ');
                if (sp == std::string::npos) continue;
                std::wstring name(rest.begin(), rest.begin() + sp);
                double value = atof(rest.c_str() + sp + 1);
                bool replaced = false;
                for (auto& pin : g_pawn_pins)
                    if (pin.first == name)
                    {
                        pin.second = value;
                        replaced = true;
                    }
                if (!replaced) g_pawn_pins.push_back({name, value});
                HSVR_LOG("Script: hsvr.pawn_pin %ls = %g", name.c_str(), value);
                continue;
            }
            if (entry.command.rfind("hsvr.watch_vars", 0) == 0)
            {
                // hsvr.watch_vars grip,offhand,stuck  (words, comma separated; none = stop)
                g_watch_words.clear();
                g_watched.clear();
                for (auto& o : g_watch_owners) o = nullptr;
                std::string rest = entry.command.size() > 16 ? entry.command.substr(16) : std::string();
                size_t start = 0;
                while (start <= rest.size())
                {
                    size_t end = rest.find(',', start);
                    if (end == std::string::npos) end = rest.size();
                    std::wstring word;
                    for (size_t i = start; i < end; ++i)
                        if (rest[i] != ' ' && rest[i] != '_') word += (wchar_t)tolower((unsigned char)rest[i]);
                    if (!word.empty()) g_watch_words.push_back(word);
                    start = end + 1;
                }
                HSVR_LOG("Watch: %d words", (int)g_watch_words.size());
                continue;
            }
            if (entry.command.rfind("hsvr.joint_trace ", 0) == 0)
            {
                g_joint_trace.store(atoi(entry.command.c_str() + 17) != 0);
                continue;
            }
            if (entry.command == "hsvr.passive_foe")
            {
                // A training dummy for unattended tests (passive_foe.md): every foe's AI controller
                // (AI_BP_C) is stopped - DED set, its tick off, its movement stopped - and the inputs it
                // writes on its pawn cleared. The foe keeps its own physics, balance and get-up (those
                // run in Willie_BP's tick), so it can be hit, grabbed and stuck into. It can still die.
                UObject* player = nullptr;
                find_pawn_mesh(&player);
                std::vector<UObject*> ais;
                UObjectGlobals::FindAllOf(STR("AI_BP_C"), ais);
                int stopped = 0;
                for (UObject* ai : ais)
                {
                    if (!ai || ai->GetName().rfind(STR("Default__"), 0) == 0) continue;
                    Call gp(ai, STR("K2_GetPawn"));
                    gp.run();
                    UObject* foe = gp.object_result();
                    if (!foe || foe == player) continue;
                    set_variable(ai, STR("DED"), 1.0);
                    Call tick(ai, STR("SetActorTickEnabled"));
                    tick.first_flag(false);
                    tick.run();
                    Call stop(ai, STR("StopMovement"));
                    stop.run();
                    std::wstring missing;
                    for (const TCHAR* v : {STR("R Hand Active"), STR("L Hand Active"), STR("R Down"), STR("L Down"), STR("Chain Attack"), STR("BobnWeave"),
                                           STR("Dodging"), STR("Alt Thrusting"), STR("AI In Combat"), STR("Is Crouched??")})
                        if (!set_variable(foe, v, 0.0)) missing += std::wstring(v) + STR(", ");
                    if (FProperty* p = foe->GetPropertyByNameInChain(STR("Aim Vector_0")))
                        write_three(reinterpret_cast<uint8_t*>(foe) + p->GetOffset_Internal(), p, 0.0, 0.0, 0.0);
                    ++stopped;
                    HSVR_LOG("Script: hsvr.passive_foe: %ls (controller %ls) made passive%ls%ls", foe->GetName().c_str(), ai->GetName().c_str(),
                             missing.empty() ? STR("") : STR("; not found: "), missing.c_str());
                }
                HSVR_LOG("Script: hsvr.passive_foe: %d foe(s) made passive (%d AI controllers)", stopped, (int)ais.size());
                continue;
            }
            if (entry.command.rfind("hsvr.force_watch ", 0) == 0)
            {
                install_force_watch();
                g_force_watch.store(atoi(entry.command.c_str() + 17) != 0);
                continue;
            }
            if (entry.command.rfind("hsvr.break_joint ", 0) == 0)
            {
                g_break_joint.store(atoi(entry.command.c_str() + 17) != 0);
                continue;
            }
            if (entry.command.rfind("hsvr.pin_disabled ", 0) == 0)
            {
                g_pin_disabled.store(atoi(entry.command.c_str() + 18) != 0);
                HSVR_LOG("Script: hsvr.pin_disabled %d", (int)g_pin_disabled.load());
                continue;
            }
            if (entry.command.rfind("hsvr.stuck_check ", 0) == 0)
            {
                g_stuck_check.store(atoi(entry.command.c_str() + 17));
                HSVR_LOG("Script: hsvr.stuck_check %d", g_stuck_check.load());
                continue;
            }
            if (entry.command.rfind("hsvr.weapon_dump", 0) == 0)
            {
                UObject* pawn = nullptr;
                find_pawn_mesh(&pawn);
                if (pawn) weapon_dump(entry.command.size() > 17 && entry.command[17] == '0' ? 0 : 1, pawn);
                continue;
            }
            if (entry.command.rfind("hsvr.spawn ", 0) == 0)
            {
                // hsvr.spawn <class path> [left]: an actor of that class at the hand (right, or left).
                std::string rest = entry.command.substr(11);
                int spawn_side = 1;
                if (rest.size() > 5 && rest.substr(rest.size() - 5) == " left")
                {
                    spawn_side = 0;
                    rest.resize(rest.size() - 5);
                }
                std::wstring path(rest.begin(), rest.end());
                UClass* cls = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, path.c_str());
                if (!cls)
                {
                    std::wstring asset = path;
                    if (asset.size() > 2 && asset.substr(asset.size() - 2) == STR("_C")) asset.resize(asset.size() - 2);
                    load_asset_object(asset);
                    cls = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, path.c_str());
                }
                UObject* pawn = nullptr;
                UObject* mesh = find_pawn_mesh(&pawn);
                static UObject* gs = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__GameplayStatics"));
                if (!cls || !mesh || !gs)
                {
                    HSVR_LOG("Script: hsvr.spawn: %s (class %s, character %s)", rest.c_str(), cls ? "found" : "NOT FOUND", mesh ? "found" : "none");
                    continue;
                }
                hsvr::Vec3 at = hand_grab_point(mesh, spawn_side) + hsvr::Vec3{0, 0, 6.0};
                auto fill_transform = [&](Call& c) {
                    FProperty* p = c.param(STR("SpawnTransform"));
                    UScriptStruct* type = p ? struct_type(p) : nullptr;
                    if (!type) return;
                    uint8_t* tat = c.buffer.data() + p->GetOffset_Internal();
                    if (FProperty* r = struct_member(type, STR("Rotation")))
                        if (FProperty* w = struct_member(struct_type(r), STR("W")))
                        {
                            uint8_t* wat = tat + r->GetOffset_Internal() + w->GetOffset_Internal();
                            if (w->GetClass().GetName() == STR("DoubleProperty")) *reinterpret_cast<double*>(wat) = 1.0;
                            else *reinterpret_cast<float*>(wat) = 1.0f;
                        }
                    if (FProperty* t = struct_member(type, STR("Translation"))) write_three(tat + t->GetOffset_Internal(), t, at.x, at.y, at.z);
                    if (FProperty* sc = struct_member(type, STR("Scale3D"))) write_three(tat + sc->GetOffset_Internal(), sc, 1.0, 1.0, 1.0);
                };
                Call begin(gs, STR("BeginDeferredActorSpawnFromClass"));
                begin.object(STR("WorldContextObject"), pawn);
                begin.object(STR("ActorClass"), cls);
                fill_transform(begin);
                begin.byte(STR("CollisionHandlingOverride"), 1); // always spawn
                begin.run();
                UObject* actor = begin.object_result();
                if (actor)
                {
                    Call finish(gs, STR("FinishSpawningActor"));
                    finish.object(STR("Actor"), actor);
                    fill_transform(finish);
                    finish.run();
                }
                HSVR_LOG("Script: hsvr.spawn: %ls at the %s hand (%.0f, %.0f, %.0f)", actor ? actor->GetFullName().c_str() : STR("NOTHING spawned"),
                         spawn_side == 0 ? "left" : "right", at.x, at.y, at.z);
                continue;
            }
            if (entry.command.rfind("hsvr.bring_object", 0) == 0)
            {
                // Test: the nearest loose physics object (not a weapon, not a character) is moved to
                // the right hand with its gravity off, so the object grab can be tried unattended.
                UObject* pawn = nullptr;
                UObject* mesh = find_pawn_mesh(&pawn);
                if (!mesh) continue;
                // "hsvr.bring_object left" = to the left hand.
                int bring_side = entry.command.find("left", 17) != std::string::npos ? 0 : 1;
                bool want_food = entry.command.find("food", 17) != std::string::npos; // "hsvr.bring_object [left] food": food only
                // "hsvr.bring_object [left] shield [rim]": the nearest loose shield, its middle at the
                // hand, or with "rim" the edge of it (found from its collision) at the hand.
                bool want_shield = entry.command.find("shield", 17) != std::string::npos;
                bool want_rim = entry.command.find("rim", 17) != std::string::npos;
                // "hsvr.bring_object [left] weapon ...": the nearest loose weapon that is not a shield.
                // "... at x y z" (either): that point of it (BaseMesh frame, cm) kept at the hand.
                bool want_weapon = entry.command.find("weapon", 17) != std::string::npos;
                bool have_at = false;
                hsvr::Vec3 at_point;
                {
                    size_t ap = entry.command.find(" at ", 17);
                    if (ap != std::string::npos &&
                        sscanf(entry.command.c_str() + ap + 4, "%lf %lf %lf", &at_point.x, &at_point.y, &at_point.z) == 3)
                        have_at = true;
                }
                bool want_held_kind = want_shield || want_weapon;
                hsvr::Vec3 hand = hand_grab_point(mesh, bring_side);
                std::vector<UObject*> comps;
                UObjectGlobals::FindAllOf(STR("StaticMeshComponent"), comps);
                UObject* best = nullptr;
                double best_d = 3000.0;
                int loose = 0;
                for (UObject* c : comps)
                {
                    if (!c || c->GetName().rfind(STR("Default__"), 0) == 0) continue;
                    Call owner_call(c, STR("GetOwner"));
                    owner_call.run();
                    UObject* owner = owner_call.object_result();
                    if (!owner || owner == pawn || is_a(owner, STR("Willie_BP_C"))) continue;
                    if (want_held_kind)
                    {
                        if (!is_a(owner, STR("ModularWeaponBP_C")) || object_property(owner, STR("BaseMesh")) != c) continue;
                        if (want_shield != class_name_starts(owner, STR("Shield"))) continue;
                        if (is_fist(owner) || bool_property(owner, STR("Is Sheathed"))) continue;
                        if (object_property(pawn, STR("Weapon L")) == owner || object_property(pawn, STR("Weapon R")) == owner) continue;
                    }
                    else if (is_a(owner, STR("ModularWeaponBP_C")))
                        continue;
                    if (want_food && !is_a(owner, STR("BP_Food_Master_C"))) continue;
                    Call sim(c, STR("IsSimulatingPhysics"));
                    sim.run();
                    FProperty* sp = sim.param(STR("ReturnValue"));
                    if (!want_held_kind && (!sp || !*(sim.buffer.data() + sp->GetOffset_Internal()))) continue; // a weapon just spawned may not simulate yet
                    ++loose;
                    double d = hsvr::length(component_location(c) - hand);
                    if (d < best_d)
                    {
                        best_d = d;
                        best = c;
                    }
                }
                if (best)
                {
                    Call g(best, STR("SetEnableGravity"));
                    g.first_flag(false);
                    g.run();
                    Call move(best, STR("K2_SetWorldLocation"));
                    move.three(STR("NewLocation"), hand.x, hand.y, hand.z + 4.0);
                    move.flag(STR("bTeleport"), true);
                    move.run();
                    Call stop(best, STR("SetPhysicsLinearVelocity"));
                    stop.run();
                    g_bring = best;
                    g_bring_side = bring_side;
                    g_bring_until = hsvr::now_seconds() + (want_held_kind ? 8.0 : 4.0);
                    g_bring_weapon = want_held_kind;
                    g_bring_at_point = have_at;
                    g_bring_point = at_point;
                    if (want_shield && want_rim && !have_at)
                    {
                        // Its thin axis: the local axis with the least collision across it (asked
                        // from 200 cm out each way). The rim point is the farthest edge along the
                        // longer of the other two.
                        hsvr::Vec3 centre = component_location(best);
                        hsvr::Quat rot = component_rotation(best);
                        const hsvr::Vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
                        double across[3];
                        hsvr::Vec3 edge[3];
                        for (int a = 0; a < 3; ++a)
                        {
                            across[a] = 0.0;
                            for (double sgn : {1.0, -1.0})
                            {
                                hsvr::Vec3 dir = rot.rotate(axes[a] * sgn);
                                hsvr::Vec3 from = centre + dir * 200.0;
                                hsvr::Vec3 p = centre;
                                if (closest_on_weapon(best->GetOuterPrivate(), from, &p) < 0.0) p = centre;
                                double e = hsvr::dot(p - centre, dir);
                                across[a] += e;
                                if (sgn > 0) edge[a] = p;
                            }
                        }
                        int thin = 0;
                        for (int a = 1; a < 3; ++a)
                            if (across[a] < across[thin]) thin = a;
                        int rim = -1;
                        for (int a = 0; a < 3; ++a)
                            if (a != thin && (rim < 0 || across[a] > across[rim])) rim = a;
                        g_bring_point = rot.inverse().rotate(edge[rim] - centre);
                        g_bring_at_point = true;
                        HSVR_LOG("Script: hsvr.bring_object: shield across its local X %.1f, Y %.1f, Z %.1f cm; thin axis %c; rim point (%.1f, %.1f, %.1f) at the hand",
                                 across[0], across[1], across[2], "XYZ"[thin], g_bring_point.x, g_bring_point.y, g_bring_point.z);
                    }
                }
                HSVR_LOG("Script: hsvr.bring_object: %d loose objects; moved %ls (was %.0f cm away) to the %s hand", loose,
                         best ? best->GetFullName().c_str() : STR("nothing"), best_d, bring_side == 0 ? "left" : "right");
                continue;
            }
            if (entry.command.rfind("hsvr.trigger ", 0) == 0)
            {
                int side = atoi(entry.command.c_str() + 13) != 0 ? 1 : 0;
                size_t sp = entry.command.find(' ', 13);
                float v = sp == std::string::npos ? -1.0f : (float)atof(entry.command.c_str() + sp + 1);
                g_fake_trigger[side].store(v);
                HSVR_LOG("Script: hsvr.trigger %d %.2f", side, v);
                continue;
            }
            if (entry.command.rfind("hsvr.squeeze ", 0) == 0)
            {
                int side = atoi(entry.command.c_str() + 13) != 0 ? 1 : 0;
                size_t sp = entry.command.find(' ', 13);
                float v = sp == std::string::npos ? -1.0f : (float)atof(entry.command.c_str() + sp + 1);
                g_fake_squeeze[side].store(v);
                HSVR_LOG("Script: hsvr.squeeze %d %.2f", side, v);
                continue;
            }
            if (entry.command.rfind("hsvr.press_a ", 0) == 0)
            {
                g_fake_continue.store(atoi(entry.command.c_str() + 13) != 0);
                continue;
            }
            if (entry.command.rfind("hsvr.match_won", 0) == 0)
            {
                // What the GameMode's Minus Enemy sets when the last enemy dies, so the "Hold G to
                // continue" path can be tested without a fight.
                UObject* pawn = nullptr;
                find_pawn_mesh(&pawn);
                UObject* world = object_property(object_property(g_engine, STR("GameViewport")), STR("World"));
                UObject* game_mode = object_property(world, STR("AuthorityGameMode"));
                bool won = entry.command.size() <= 15 || atoi(entry.command.c_str() + 15) != 0; // "hsvr.match_won 0" clears
                bool a = set_bool_property(game_mode, STR("Match Won"), won);
                bool b = set_bool_property(pawn, STR("Match Won (Player)"), won);
                HSVR_LOG("Script: hsvr.match_won %d: game mode %ls (written %d, Match Won now %d), character (written %d, Match Won (Player) now %d)", (int)won,
                         game_mode ? game_mode->GetFullName().c_str() : STR("missing"), (int)a, (int)bool_property(game_mode, STR("Match Won")), (int)b,
                         (int)bool_property(pawn, STR("Match Won (Player)")));
                continue;
            }
            if (entry.command.rfind("hsvr.hold_reach ", 0) == 0)
            {
                g_hold_reach_clamp.store(atoi(entry.command.c_str() + 16) != 0);
                continue;
            }
            if (entry.command.rfind("hsvr.grip_hold ", 0) == 0)
            {
                g_grip_hold_override.store(atoi(entry.command.c_str() + 15));
                continue;
            }
            if (entry.command.rfind("hsvr.grip_k_scale ", 0) == 0)
            {
                g_grip_k_scale.store(atof(entry.command.c_str() + 18));
                continue;
            }
            if (entry.command.rfind("hsvr.grip_frame_offset ", 0) == 0)
            {
                double x = 0, y = 0, z = 0;
                sscanf(entry.command.c_str() + 23, "%lf %lf %lf", &x, &y, &z);
                g_grip_frame_offset[0].store(x);
                g_grip_frame_offset[1].store(y);
                g_grip_frame_offset[2].store(z);
                g_grip_frame_serial.fetch_add(1);
                continue;
            }
            if (entry.command.rfind("hsvr.contact_log ", 0) == 0)
            {
                g_contact_log.store(atoi(entry.command.c_str() + 17) != 0);
                continue;
            }
            if (entry.command.rfind("hsvr.arm_move ", 0) == 0)
            {
                g_fake_arm_mode.store(atoi(entry.command.c_str() + 14));
                size_t sp = entry.command.find(' ', 14);
                if (sp != std::string::npos) g_fake_two_up.store(atof(entry.command.c_str() + sp + 1));
                continue;
            }
            if (entry.command.rfind("hsvr.weapon_channel ", 0) == 0)
            {
                int ch = -1, resp = 2;
                sscanf(entry.command.c_str() + 20, "%d %d", &ch, &resp);
                g_weapon_channel_resp.store(resp);
                g_weapon_channel_cmd.store(ch);
                continue;
            }
            if (entry.command.rfind("hsvr.grip_ignore_arm ", 0) == 0)
            {
                g_grip_ignore_arm.store(atoi(entry.command.c_str() + 21));
                continue;
            }
            if (entry.command == "hsvr.grip_rejoin")
            {
                g_grip_rejoin.store(true);
                continue;
            }
            if (entry.command.rfind("hsvr.weapon_collision ", 0) == 0)
            {
                g_weapon_collision.store(atoi(entry.command.c_str() + 22));
                continue;
            }
            if (entry.command.rfind("hsvr.grip_target ", 0) == 0)
            {
                double x = 0, y = 0, z = 0;
                sscanf(entry.command.c_str() + 17, "%lf %lf %lf", &x, &y, &z);
                g_grip_target[0].store(x);
                g_grip_target[1].store(y);
                g_grip_target[2].store(z);
                continue;
            }
            if (entry.command.rfind("hsvr.weapon_gravity ", 0) == 0)
            {
                g_weapon_gravity.store(atoi(entry.command.c_str() + 20));
                continue;
            }
            if (entry.command.rfind("hsvr.give_weapon ", 0) == 0)
            {
                script_give_weapon(entry.command.substr(17));
                continue;
            }
            if (entry.command.rfind("hsvr.call ", 0) == 0)
            {
                script_call_game_instance(entry.command.substr(10));
                continue;
            }
            run_console_command(entry.command);
            return; // one a tick
        }
    }

    // ---- installing the stereo device ------------------------------------------------------------
    // UEngine keeps `TSharedPtr<IStereoRendering> StereoRenderingDevice` followed directly by
    // `TSharedPtr<IXRTrackingSystem> XRSystem`. Neither is a reflected property, so their offset is
    // worked out from reflected neighbours (filled in from the UE 5.4.4 layout; see find_stereo_slot).
    ue::TSharedPtr<ue::IStereoRendering>* find_stereo_slot(UObject* engine)
    {
        auto* base = reinterpret_cast<uint8_t*>(engine);
        size_t offset = (size_t)hsvr::config().stereo_slot_offset;
        if (offset == 0)
        {
            // Both neighbours are reflected properties, so their offsets can be asked for by name.
            size_t begin = 0x400, end = 0x1800;
            FProperty* before = engine->GetPropertyByNameInChain(STR("SelectionHighlightIntensityBillboards"));
            FProperty* after = engine->GetPropertyByNameInChain(STR("GlobalNetTravelCount"));
            if (before && after && after->GetOffset_Internal() > before->GetOffset_Internal())
            {
                begin = (size_t)before->GetOffset_Internal() + 4;
                end = (size_t)after->GetOffset_Internal();
            }
            else
            {
                HSVR_LOG("Neighbouring engine properties not found; searching a default range");
            }
            HSVR_LOG("Searching for the stereo device slot in UEngine between 0x%zX and 0x%zX", begin, end);
            offset = hsvr::find_stereo_slot_offset(base, begin, end);
        }
        if (offset == 0) return nullptr;
        return reinterpret_cast<ue::TSharedPtr<ue::IStereoRendering>*>(base + offset);
    }

    void install_stereo_device()
    {
        auto* slot = find_stereo_slot(g_engine);
        if (!slot)
        {
            g_install_failed = true;
            HSVR_LOG("Could not find the engine's stereo device slot; VR stays off");
            return;
        }
        if (slot->Object != nullptr)
        {
            g_install_failed = true;
            HSVR_LOG("The engine already has a stereo device (%p); not replacing it. Remove -emulatestereo from the launch options.", (void*)slot->Object);
            return;
        }
        slot->ReferenceController = hsvr::stereo_device_reference_controller();
        slot->Object = static_cast<ue::IStereoRendering*>(hsvr::stereo_device_object());
        hsvr::shared().stereo_installed.store(true);
        HSVR_LOG("Stereo device installed in the engine at offset 0x%llX",
                 (unsigned long long)(reinterpret_cast<uint8_t*>(slot) - reinterpret_cast<uint8_t*>(g_engine)));
    }

    struct IsPausedParams
    {
        UObject* WorldContextObject;
        bool ReturnValue;
    };

    // ---- what the game's menus and HUD are made of (for the log) --------------------------------
    struct ByteReturnParams
    {
        uint8_t ReturnValue;
    };

    struct IntReturnParams
    {
        int32_t ReturnValue;
    };

    struct ChildAtParams
    {
        int32_t Index;
        UObject* ReturnValue;
    };

    struct FloatParams
    {
        float Value;
    };

    // Calls `visit(widget, depth)` for a widget and everything inside it.
    template <typename Visit>
    void walk_widget_tree(UObject* widget, int depth, int& budget, Visit&& visit)
    {
        if (!widget || depth > 6 || budget <= 0) return;
        --budget;
        visit(widget, depth);
        // A widget made in the game's editor keeps its parts in a tree of its own.
        if (UObject* tree = object_property(widget, STR("WidgetTree")))
        {
            walk_widget_tree(object_property(tree, STR("RootWidget")), depth + 1, budget, visit);
        }
        UFunction* count_fn = widget->GetFunctionByNameInChain(STR("GetChildrenCount"));
        UFunction* child_fn = widget->GetFunctionByNameInChain(STR("GetChildAt"));
        if (!count_fn || !child_fn) return;
        IntReturnParams count{};
        widget->ProcessEvent(count_fn, &count);
        for (int32_t i = 0; i < count.ReturnValue && i < 64; ++i)
        {
            ChildAtParams child{};
            child.Index = i;
            widget->ProcessEvent(child_fn, &child);
            walk_widget_tree(child.ReturnValue, depth + 1, budget, visit);
        }
    }

    // Calls `visit` for every widget the game currently has on screen.
    template <typename Visit>
    int walk_screen_widgets(Visit&& visit)
    {
        std::vector<UObject*> widgets;
        UObjectGlobals::FindAllOf(STR("UserWidget"), widgets);
        int budget = 400;
        for (UObject* widget : widgets)
        {
            if (!widget) continue;
            UFunction* in_viewport_fn = widget->GetFunctionByNameInChain(STR("IsInViewport"));
            if (!in_viewport_fn) continue;
            BoolReturnParams in_viewport{};
            widget->ProcessEvent(in_viewport_fn, &in_viewport);
            if (!in_viewport.ReturnValue) continue;
            walk_widget_tree(widget, 0, budget, visit);
        }
        return (int)widgets.size();
    }

    // Visibility values are the engine's ESlateVisibility: 0 visible, 1 collapsed, 2 hidden,
    // 3 not hit-testable (self and children), 4 not hit-testable (self only).
    void log_widgets(const char* reason)
    {
        HSVR_LOG("Widgets (%s), those on screen:", reason);
        int lines = 0;
        int total = walk_screen_widgets([&lines](UObject* widget, int depth) {
            int visibility = -1;
            if (UFunction* visibility_fn = widget->GetFunctionByNameInChain(STR("GetVisibility")))
            {
                ByteReturnParams v{};
                widget->ProcessEvent(visibility_fn, &v);
                visibility = v.ReturnValue;
            }
            float opacity = -1.0f;
            if (UFunction* opacity_fn = widget->GetFunctionByNameInChain(STR("GetRenderOpacity")))
            {
                FloatParams o{};
                widget->ProcessEvent(opacity_fn, &o);
                opacity = o.Value;
            }
            ++lines;
            if (UObject* resource = brush_resource(widget))
            {
                HSVR_LOG("  %*s%ls | visibility %d, opacity %.2f | draws %ls", depth * 2, "", widget->GetFullName().c_str(), visibility, opacity,
                         resource->GetFullName().c_str());
            }
            else
            {
                HSVR_LOG("  %*s%ls | visibility %d, opacity %.2f", depth * 2, "", widget->GetFullName().c_str(), visibility, opacity);
            }
        });
        HSVR_LOG("Widgets: end of list (%d line(s), %d widgets exist)", lines, total);
    }

    // What each slider on screen is doing, written whenever it changes. A slider follows the
    // pointer only while it "holds the mouse": from a button press on it until the release.
    // Holding it with no real button down means the game missed the release.
    void log_sliders()
    {
        static std::map<UObject*, std::string> last;
        static int lines_left = 300;
        if (lines_left <= 0) return;
        bool button = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        walk_screen_widgets([&](UObject* widget, int) {
            auto name = widget->GetFullName();
            if (name.rfind(STR("Slider "), 0) != 0) return;
            bool holds = false, hovered = false;
            float value = -1.0f;
            if (UFunction* fn = widget->GetFunctionByNameInChain(STR("HasMouseCapture")))
            {
                BoolReturnParams r{};
                widget->ProcessEvent(fn, &r);
                holds = r.ReturnValue;
            }
            if (UFunction* fn = widget->GetFunctionByNameInChain(STR("IsHovered")))
            {
                BoolReturnParams r{};
                widget->ProcessEvent(fn, &r);
                hovered = r.ReturnValue;
            }
            if (UFunction* fn = widget->GetFunctionByNameInChain(STR("GetValue")))
            {
                FloatParams r{};
                widget->ProcessEvent(fn, &r);
                value = r.Value;
            }
            char state[96];
            snprintf(state, sizeof(state), "value %.3f, holds the mouse %d, pointer over it %d, real left button down %d", (double)value, holds ? 1 : 0,
                     hovered ? 1 : 0, button ? 1 : 0);
            std::string& before = last[widget];
            if (before == state || lines_left <= 0) return;
            // A slider nobody touches is written once only.
            bool first = before.empty();
            before = state;
            if (!first || holds || hovered)
            {
                --lines_left;
                HSVR_LOG("Slider: %ls | %s", name.c_str(), state);
            }
        });
    }

    // A number that changes when a menu screen opens or closes: which widgets are on screen and
    // which of them are visible.
    uint64_t screen_signature()
    {
        uint64_t hash = 1469598103934665603ull;
        walk_screen_widgets([&hash](UObject* widget, int) {
            int visibility = -1;
            if (UFunction* visibility_fn = widget->GetFunctionByNameInChain(STR("GetVisibility")))
            {
                ByteReturnParams v{};
                widget->ProcessEvent(visibility_fn, &v);
                visibility = v.ReturnValue;
            }
            hash = (hash ^ (uint64_t)(uintptr_t)widget) * 1099511628211ull;
            hash = (hash ^ (uint64_t)(visibility + 1)) * 1099511628211ull;
        });
        return hash;
    }

    // Feeds the input record in the log (see input_watch.hpp) and tells the picture side whether
    // a menu is up. Reads only.
    void watch_input()
    {
        hsvr::InputWatchGame game;
        UObject* pawn = nullptr;
        UObject* pawn_mesh = find_pawn_mesh(&pawn);
        if (UObject* controller = find_player_controller())
        {
            game.have_player = true;
            if (ue::FRotator* rotation = controller->GetValuePtrByPropertyNameInChain<ue::FRotator>(STR("ControlRotation")))
            {
                game.view_yaw = rotation->Yaw;
            }
            game.wants_pointer = bool_property(controller, STR("bShowMouseCursor"));
            static UFunction* fn = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Engine.GameplayStatics:IsGamePaused"));
            static UObject* cdo = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__GameplayStatics"));
            if (fn && cdo)
            {
                IsPausedParams params{controller, false};
                cdo->ProcessEvent(fn, &params);
                game.paused = params.ReturnValue;
            }
        }
        auto& shared = hsvr::shared();
        shared.game_paused.store(game.paused);
        shared.game_wants_pointer.store(game.wants_pointer);
        // No character yet means the intro or the main menu.
        shared.menu_up.store(game.paused || !pawn || !pawn_mesh);
        hsvr::input_watch_tick(game);

        // A few times per run, write down what is on screen: once early (intro), a few seconds
        // into each new character's life, and whenever the game pauses.
        if (!hsvr::config().log_input) return;
        static int lists_left = 8;
        static double due = -1.0;
        static const char* due_reason = "";
        static bool intro_done = false, was_paused = false;
        static UObject* last_pawn = nullptr;
        static double first_tick = hsvr::now_seconds();
        double now = hsvr::now_seconds();
        if (!intro_done && now - first_tick > 20.0)
        {
            intro_done = true;
            due = now;
            due_reason = "20 seconds in";
        }
        if (pawn && pawn_mesh && pawn != last_pawn)
        {
            last_pawn = pawn;
            due = now + 5.0;
            due_reason = "a few seconds after the character appeared";
        }
        if (game.paused && !was_paused)
        {
            due = now + 0.5;
            due_reason = "game paused";
        }
        was_paused = game.paused;
        if (due >= 0.0 && now >= due && lists_left > 0)
        {
            due = -1.0;
            --lists_left;
            log_widgets(due_reason);
            // The picture settings alongside the first two lists (start-up, and once in a fight).
            static int graphics_left = 2;
            if (graphics_left > 0)
            {
                --graphics_left;
                log_graphics(due_reason);
            }
        }

        // While a menu has the mouse pointer, also write the list whenever the screen changes, so
        // that every menu screen the player visits is on record (checked twice a second).
        static double last_slider_scan = 0.0;
        if (hsvr::config().log_sliders && game.wants_pointer && now - last_slider_scan >= 0.25)
        {
            last_slider_scan = now;
            log_sliders();
        }
        static double last_scan = 0.0;
        static uint64_t last_signature = 0;
        static int change_lists_left = 16;
        if (game.wants_pointer && change_lists_left > 0 && now - last_scan >= 0.5)
        {
            last_scan = now;
            uint64_t signature = screen_signature();
            if (signature != last_signature)
            {
                last_signature = signature;
                --change_lists_left;
                log_widgets("the menu screen changed");
            }
        }
    }

    bool key_pressed_once(int vk, bool& was_down)
    {
        bool down = vk > 0 && (GetAsyncKeyState(vk) & 0x8000) != 0;
        bool pressed = down && !was_down;
        was_down = down;
        return pressed;
    }

    // The game's menu keys from the controllers (its menus take only the keyboard and mouse;
    // menus_and_prompts.md). Called only while the game window is in front.
    //  - Left menu button = Escape, down while held: opens the pause menu, and holding it closes the
    //    pause menu (UI_Pause_Eng's Tick closes itself while Escape is held).
    //  - Right A held menu_continue_hold_s = "Hold G to continue": calls the player's VO Deescalate
    //    (what the game's held Talk calls after a win: UI_WIN, then UI_NextFIght), only while the
    //    GameMode's "Match Won" is set and his "Player Match End" is not. G itself also surrenders
    //    (held when not won) and bites (double press), so the key is never sent.
    void menu_buttons(const hsvr::Config& cfg, HWND game_window)
    {
        using namespace hsvr;
        static bool escape_down = false;
        static HWND escape_window = nullptr;
        static double continue_since = -1.0;
        static bool continue_done = false;
        ControllerState state = controllers_state();
        bool live = cfg.menu_buttons && state.active && state.focused && game_window;
        const LPARAM escape_scan = (LPARAM)MapVirtualKeyW(VK_ESCAPE, MAPVK_VK_TO_VSC) << 16;

        bool menu = live && state.hand[0].menu;
        if (menu && !escape_down)
        {
            escape_down = true;
            escape_window = game_window;
            PostMessageW(game_window, WM_KEYDOWN, VK_ESCAPE, escape_scan | 1);
            HSVR_LOG("Menu: left menu button down (Escape)");
        }
        else if (!menu && escape_down)
        {
            escape_down = false;
            PostMessageW(escape_window, WM_KEYUP, VK_ESCAPE, escape_scan | 1 | (LPARAM)0xC0000000);
            HSVR_LOG("Menu: left menu button up");
        }

        bool a = (live && state.hand[1].primary) || g_fake_continue.load();
        if (!a)
        {
            continue_since = -1.0;
            continue_done = false;
            return;
        }
        double now = now_seconds();
        if (continue_since < 0.0) continue_since = now;
        if (continue_done || now - continue_since < cfg.menu_continue_hold_s) return;
        continue_done = true; // once per hold
        UObject* pawn = nullptr;
        find_pawn_mesh(&pawn);
        UObject* viewport = object_property(g_engine, STR("GameViewport"));
        UObject* world = object_property(viewport, STR("World"));
        UObject* game_mode = object_property(world, STR("AuthorityGameMode"));
        bool won = bool_property(game_mode, STR("Match Won"));
        bool ended = bool_property(pawn, STR("Player Match End"));
        UFunction* fn = pawn ? pawn->GetFunctionByNameInChain(STR("VO Deescalate")) : nullptr;
        if (!pawn || !game_mode || !fn || !won || ended)
        {
            HSVR_LOG("Menu: A held, not continuing (character %s, game mode %s, VO Deescalate %s, Match Won %d, Player Match End %d)",
                     pawn ? "found" : "missing", game_mode ? "found" : "missing", fn ? "found" : "missing", (int)won, (int)ended);
            return;
        }
        std::vector<uint8_t> block = param_block(fn);
        uint8_t* parameters = block.data();
        pawn->ProcessEvent(fn, parameters);
        controllers_buzz(1, 0.6f);
        HSVR_LOG("Menu: A held %.1f s after a won fight: VO Deescalate called (Player Match End now %d)", cfg.menu_continue_hold_s,
                 (int)bool_property(pawn, STR("Player Match End")));
    }

    void game_tick(UObject* engine, float delta_seconds)
    {
        g_engine = engine;
        hsvr::config_reload_if_changed();
        hsvr::stereo_begin_game_frame(delta_seconds);
        if (!g_vr_started) return;
        auto& shared = hsvr::shared();

        if (!shared.stereo_installed.load() && !g_install_failed && shared.xr_ready.load()) install_stereo_device();

        if (shared.stereo_installed.load() && !g_console_done)
        {
            g_console_done = true;
            // The headset paces the frames; the game's own limits would only add judder.
            run_console_command(STR("r.VSync 0"));
            run_console_command(STR("t.MaxFPS 0"));
            run_console_command(STR("r.MotionBlurQuality 0"));
            run_console_command(STR("r.DepthOfFieldQuality 0"));
            // Keep menus and HUD in the game window (we show that on a floating panel) instead of
            // letting the engine smear them across both eyes. Only matters while the eyes have
            // their own target.
            run_console_command(STR("Slate.DrawToVRRenderTarget 0"));
            HSVR_LOG("Applied VR console settings (vsync off, no frame cap, no motion blur, no depth of field)");
        }

        watch_input();

        hsvr::Config cfg = hsvr::config();
        log_controllers(cfg);
        body_log_tick(cfg);
        pose_probe(cfg);
        pose_body(cfg);
        hide_own_head(cfg);
        grab_button(cfg, hsvr::controllers_state(), shared.menu_up.load());
        watch_vars_tick();
        if (g_console_done)
        {
            apply_picture_settings(cfg);
            keep_clouds_correct(cfg);
            keep_light_samples(cfg);
            run_dev_script(cfg);
        }
        // The mod's keys count only while the game has the keyboard, so that Home or End typed in
        // another program does not move the view or hide the panel.
        static bool recenter_down = false, ui_down = false, view_down = false, mode_down = false;
        bool recenter = key_pressed_once(cfg.recenter_key, recenter_down);
        bool ui = key_pressed_once(cfg.toggle_ui_key, ui_down);
        bool mode = key_pressed_once(cfg.toggle_mode_key, mode_down);
        bool view = key_pressed_once(cfg.toggle_view_key, view_down);
        DWORD front_process = 0;
        if (HWND front = GetForegroundWindow()) GetWindowThreadProcessId(front, &front_process);
        if (front_process != GetCurrentProcessId())
        {
            menu_buttons(cfg, nullptr); // lets a held Escape go
            return;
        }
        menu_buttons(cfg, GetForegroundWindow());
        if (recenter)
        {
            shared.recenter_requested.store(true);
            HSVR_LOG("Key: recenter");
        }
        if (ui)
        {
            bool toggled = !shared.ui_toggled.load();
            shared.ui_toggled.store(toggled);
            if (cfg.ui_panel_auto)
            {
                HSVR_LOG(toggled ? "Key: menu panel always shown" : "Key: menu panel shown only while a menu is up");
            }
            else
            {
                HSVR_LOG(toggled ? "Key: menu panel hidden during play (menus still show)" : "Key: menu panel shown");
            }
        }
        if (mode)
        {
            bool separate = hsvr::stereo_separate_target();
            shared.separate_override.store(separate ? 0 : 1);
            HSVR_LOG(separate ? "Key: fallback mode through the game window" : "Key: full-resolution mode");
        }
        if (view)
        {
            bool third = !shared.third_person.load();
            shared.third_person.store(third);
            HSVR_LOG(third ? "Key: view from the game's own camera (third person)" : "Key: view from the character's head (first person)");
        }
    }
} // namespace

class HalfSwordVR : public CppUserModBase
{
  public:
    HalfSwordVR() : CppUserModBase()
    {
        ModName = STR("HalfSwordVR");
        ModVersion = STR("0.1.0");
        ModDescription = STR("Native VR for Half Sword");
        ModAuthors = STR("Half-Sword-VR contributors");
        hsvr::log_open();
        HSVR_LOG("HalfSwordVR 0.1.0 loaded");
        hsvr::config_reload_if_changed(true);
    }

    ~HalfSwordVR() override = default;

    auto on_unreal_init() -> void override
    {
        HSVR_LOG("Unreal initialised");
        g_vr_started = hsvr::vr_start();
        if (g_vr_started)
        {
            hsvr::stereo_set_anchor_provider(&provide_anchor);
            hsvr::vr_install_present_hook();
        }
        Hook::RegisterProcessEventPreCallback(&capsule_before_tick, {false, false, STR("HalfSwordVR"), STR("CapsuleBefore")});
        Hook::RegisterProcessEventPostCallback(&capsule_after_tick, {false, false, STR("HalfSwordVR"), STR("CapsuleAfter")});
        Hook::RegisterProcessEventPostCallback(&feet_after_anim_update, {false, false, STR("HalfSwordVR"), STR("FeetAfterAnim")});
        Hook::RegisterEngineTickPreCallback(
                [](auto&, UEngine* engine, float delta_seconds, bool) {
                    game_tick(reinterpret_cast<UObject*>(engine), delta_seconds);
                },
                {false, false, STR("HalfSwordVR"), STR("Tick")});
        Output::send<LogLevel::Verbose>(STR("[HalfSwordVR] started (headset link {})\n"), g_vr_started ? STR("ok") : STR("off, see HalfSwordVR.log"));
    }
};

#define HALFSWORDVR_API __declspec(dllexport)
extern "C"
{
    HALFSWORDVR_API RC::CppUserModBase* start_mod()
    {
        return new HalfSwordVR();
    }

    HALFSWORDVR_API void uninstall_mod(RC::CppUserModBase* mod)
    {
        delete mod;
    }
}
