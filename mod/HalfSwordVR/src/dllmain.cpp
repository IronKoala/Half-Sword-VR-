// HalfSwordVR: native VR for Half Sword, loaded by UE4SS.
// This file is the glue to the game: it starts the headset link, installs our stereo device in
// the running engine, and tells it where the player's head is each frame.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <map>
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
        if (UObject* controller = find_player_controller())
        {
            if (ue::FRotator* rotation = controller->GetValuePtrByPropertyNameInChain<ue::FRotator>(STR("ControlRotation")))
            {
                out.has_heading = true;
                out.heading_yaw = rotation->Yaw;
            }
        }
        if (g_pose_anchor_fixed)
            out.head_position = out.actor_position +
                                hsvr::quat_from_yaw_deg(out.has_heading ? out.heading_yaw : 0.0).rotate(g_pose_anchor_local - g_pose_room_moved_local);
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

    struct BodiesBelowParams
    {
        FName InBoneName;
        bool bNewSimulate;
        bool bIncludeSelf;
    };

    // Physics back on for one branch of the skeleton (the game itself does this from the pelvis
    // when it rescales the character).
    void set_simulate_below(UObject* mesh, const std::string& bone, bool simulate)
    {
        UFunction* fn = mesh->GetFunctionByNameInChain(STR("SetAllBodiesBelowSimulatePhysics"));
        if (!fn) return;
        BodiesBelowParams p{};
        p.InBoneName = bone_fname(bone);
        p.bNewSimulate = simulate;
        p.bIncludeSelf = true;
        mesh->ProcessEvent(fn, &p);
    }

    // The physics test's "off": everything off, then the arms back on when dev_player_arm_physics is set.
    void physics_test_off(UObject* mesh, const hsvr::Config& cfg, bool report)
    {
        set_simulate(mesh, false, report);
        if (!cfg.dev_player_arm_physics) return;
        set_simulate_below(mesh, cfg.arm_bone_left, true);
        set_simulate_below(mesh, cfg.arm_bone_right, true);
        if (report)
            HSVR_LOG("Physics test: arms kept on from %s / %s: pelvis %d, %s %d, %s %d", cfg.arm_bone_left.c_str(), cfg.arm_bone_right.c_str(),
                     is_simulating(mesh, "pelvis"), cfg.hand_bone_left.c_str(), is_simulating(mesh, cfg.hand_bone_left), cfg.hand_bone_right.c_str(),
                     is_simulating(mesh, cfg.hand_bone_right));
    }

    // TEST ONLY (dev_player_physics_off): physics off on the player's own body, to see what the
    // game does without its ragdoll. Only the pawn the player controls is touched; the enemies run
    // the same Blueprint and are left alone.
    // -1 = follow dev_player_physics_off; 0/1 = set by the unattended script (hsvr.physics_off).
    int g_physics_override = -1;

    // Once a second while log_body is on or the physics test runs: where the character is.
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

    void player_physics_test(const hsvr::Config& cfg)
    {
        static UObject* switched_off = nullptr; // the mesh we turned physics off on
        static UObject* checked_pawn = nullptr;
        static bool pawn_is_willie = false;
        UObject* pawn = nullptr;
        UObject* mesh = find_pawn_mesh(&pawn);
        if (pawn != checked_pawn)
        {
            checked_pawn = pawn;
            pawn_is_willie = pawn && pawn->GetFullName().find(STR("Willie_BP")) != std::wstring::npos;
        }
        bool flag = g_physics_override >= 0 ? g_physics_override != 0 : cfg.dev_player_physics_off;
        bool want_off = flag && mesh && pawn_is_willie;
        if (mesh && pawn_is_willie && (cfg.log_body || switched_off)) log_body(pawn, mesh, cfg);
        if (switched_off && (switched_off != mesh || !want_off))
        {
            if (switched_off == mesh)
            {
                set_simulate(mesh, true, true);
                HSVR_LOG("Physics test: physics back ON for your character (may differ from the game's own setup; restart the fight to be sure)");
            }
            else
            {
                HSVR_LOG("Physics test: your character changed; the new one keeps its physics until the test applies again");
            }
            switched_off = nullptr;
        }
        if (want_off && switched_off != mesh)
        {
            physics_test_off(mesh, cfg, true);
            switched_off = mesh;
            HSVR_LOG("Physics test: physics OFF for your character (%ls)%s", pawn->GetFullName().c_str(),
                     cfg.dev_player_arm_physics ? ", arms kept on" : "");
        }
        // dev_player_arm_physics changed while the test runs: apply it now.
        static bool arms_applied = false;
        if (switched_off && switched_off == mesh && arms_applied != cfg.dev_player_arm_physics)
        {
            physics_test_off(mesh, cfg, true);
            HSVR_LOG("Physics test: arms %s", cfg.dev_player_arm_physics ? "on (physics), rest of the body off" : "off with the rest of the body");
        }
        arms_applied = switched_off ? cfg.dev_player_arm_physics : false;
        if (!switched_off) return;
        static double last = 0.0;
        double now = hsvr::now_seconds();
        if (now - last < 1.0) return;
        last = now;
        // Watch whether the game turns physics back on by itself.
        int pelvis = is_simulating(mesh, "pelvis");
        int hand = is_simulating(mesh, cfg.hand_bone_right);
        HSVR_LOG("Physics test: simulating now: pelvis %d, %s %d (1 = physics on, -1 = unknown)", pelvis, cfg.hand_bone_right.c_str(), hand);
        // With the arms kept on, only the pelvis tells whether the game switched the whole body on.
        if (pelvis == 1 || (hand == 1 && !cfg.dev_player_arm_physics))
        {
            HSVR_LOG("Physics test: the game switched physics back on; switching it off again");
            static int reports_left = 5;
            physics_test_off(mesh, cfg, reports_left > 0);
            if (reports_left > 0) --reports_left;
        }
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

    void set_component_transform(UObject* component, const hsvr::Vec3& location, const hsvr::Quat& rotation)
    {
        Call c(component, STR("K2_SetWorldLocationAndRotation"));
        hsvr::Rot r = hsvr::quat_to_rot(rotation);
        c.three(STR("NewLocation"), location.x, location.y, location.z);
        c.three(STR("NewRotation"), r.pitch, r.yaw, r.roll);
        c.flag(STR("bSweep"), false);
        c.flag(STR("bTeleport"), true);
        c.run();
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
        hsvr::Quat wrist_rest;                           // forearm -> hand at calibration (the game's relaxed wrist)
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
        std::string arm_style;
        double last_check = 0.0;
        hsvr::Vec3 target[2]; // last hand targets, for the log
        hsvr::Vec3 swivel_local[2]; // smoothed elbow direction, in the body's frame
        bool swivel_valid[2] = {false, false};
        double swivel_time[2] = {0.0, 0.0};
    };
    PoseState g_pose;
    std::atomic<bool> g_hold_capsule{false}; // see "The capsule stays where you are" below

    const char* side_suffix(int side) { return side == 0 ? "_l" : "_r"; }

    ModifyBoneNode* chain_node(const std::string& bone)
    {
        std::wstring key(bone.begin(), bone.end());
        auto it = g_pose.chain.find(key);
        return it == g_pose.chain.end() ? nullptr : &it->second;
    }

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

    void apply_arm_physics(const hsvr::Config& cfg)
    {
        set_simulate(g_pose.mesh, false);
        if (cfg.pose_arm_style == "muscle")
        {
            set_simulate_below(g_pose.mesh, cfg.arm_bone_left, true);
            set_simulate_below(g_pose.mesh, cfg.arm_bone_right, true);
        }
        g_pose.arm_style = cfg.pose_arm_style;
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

        for (int side = 0; side < 2; ++side)
        {
            ArmCalibration& a = g_pose.arm[side];
            std::string sx = side_suffix(side);
            FName upper = bone_fname("upperarm" + sx), lower = bone_fname("lowerarm" + sx), hand = bone_fname("hand" + sx);
            Vec3 S = socket_location(d, upper), E = socket_location(d, lower), H = socket_location(d, hand);
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
            a.wrist_rest = (Wl.inverse() * Wh).normalized();
            a.fingers_local = normalized(Wh.inverse().rotate(fingers));
            Vec3 t = Wh.inverse().rotate(across);
            a.thumb_local = normalized(t - a.fingers_local * dot(t, a.fingers_local));
            HSVR_LOG("Pose: %s hand frame from %s: fingers (%.2f, %.2f, %.2f), thumb side (%.2f, %.2f, %.2f) in the hand bone", side == 0 ? "left" : "right",
                     how, a.fingers_local.x, a.fingers_local.y, a.fingers_local.z, a.thumb_local.x, a.thumb_local.y, a.thumb_local.z);
            a.valid = a.upper_length > 5.0 && a.lower_length > 5.0 && chain_node("upperarm" + sx) && chain_node("lowerarm" + sx) && chain_node("hand" + sx);
            HSVR_LOG("Pose: %s arm upper %.1f cm, lower %.1f cm%s", side == 0 ? "left" : "right", a.upper_length, a.lower_length, a.valid ? "" : " (not usable)");
        }
        HSVR_LOG("Pose: on. Head %.0f cm above the actor, mesh offset (%.0f, %.0f, %.0f), arms %s", g_pose_anchor_local.z, g_pose.mesh_offset_local.x,
                 g_pose.mesh_offset_local.y, g_pose.mesh_offset_local.z, cfg.pose_arm_style.c_str());
        return true;
    }

    // The bone rotation that points its calibrated direction along `dir` with its calibrated
    // bend-plane normal along `normal`.
    hsvr::Quat aim_bone(const hsvr::Vec3& dir_local, const hsvr::Vec3& normal_local, const hsvr::Vec3& dir, const hsvr::Vec3& normal)
    {
        return (hsvr::quat_from_two(dir, normal) * hsvr::quat_from_two(dir_local, normal_local).inverse()).normalized();
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
                for (const char* b : {"upperarm", "lowerarm", "hand"})
                    if (ModifyBoneNode* n = chain_node(std::string(b) + sx)) node_release(*n);
                return;
            }
            // Made-up path: each hand circles slowly in front of its shoulder.
            double t = now_seconds() * 1.5 + side * 1.5;
            Vec3 shoulder = socket_location(g_pose.driver, bone_fname("upperarm" + sx));
            grip = shoulder + body.rotate(Vec3{35.0 + 10.0 * std::sin(t), out * 15.0, -15.0 + 20.0 * std::cos(t)});
            grip_rot = (body * rot_to_quat(60.0, 0.0, 0.0)).normalized(); // controller handle tilted forward, as held
        }
        Vec3 target = grip + grip_rot.rotate({cfg.pose_wrist_forward_cm, cfg.pose_wrist_right_cm * out, cfg.pose_wrist_up_cm});
        g_pose.target[side] = target;

        Vec3 S = socket_location(g_pose.driver, bone_fname("upperarm" + sx));
        Vec3 to = target - S;
        double D = length(to);
        double L1 = a.upper_length, L2 = a.lower_length;
        double lo = std::fabs(L1 - L2) + 0.5, hi = L1 + L2 - 0.5;
        if (D < 1e-3) return;
        Vec3 dir = to * (1.0 / D);
        D = (std::min)((std::max)(D, lo), hi);
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
        Vec3 n = cross(E - S, H - E);
        if (length(n) < 1e-6) n = cross(normalized(reject(pole, dir)), dir);
        Quat Wu = aim_bone(a.upper_dir_local, a.upper_normal_local, E - S, n);
        Quat Wl = aim_bone(a.lower_dir_local, a.lower_normal_local, H - E, n);
        // The forearm has no twist bones, so a hand turned about the forearm's axis would wring
        // the wrist. Measure that turn against the game's relaxed wrist and give the forearm bone
        // its share (pose_forearm_twist_share; the hand keeps its exact rotation).
        Vec3 axis = normalized(H - E);
        Quat delta = (Wh * (Wl * a.wrist_rest).inverse()).normalized();
        double proj = delta.x * axis.x + delta.y * axis.y + delta.z * axis.z;
        double twist = 2.0 * std::atan2(proj, delta.w);
        if (twist > 3.14159265358979323846) twist -= 2.0 * 3.14159265358979323846;
        if (twist < -3.14159265358979323846) twist += 2.0 * 3.14159265358979323846;
        Wl = (quat_axis_angle(axis, twist * cfg.pose_forearm_twist_share) * Wl).normalized();
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
        if (!cfg.pose_body || !abp || !mesh)
        {
            if (g_pose.pawn && (pawn == g_pose.pawn)) pose_release();
            else if (g_pose.pawn)
            {
                g_pose = PoseState{}; // character gone: nothing to restore
                g_pose_anchor_fixed = false;
                g_pose_room_moved_local = {};
            }
            return;
        }
        if (g_pose.pawn != pawn || g_pose.instance != instance)
        {
            g_pose = PoseState{};
            g_pose.pawn = pawn;
            g_pose.mesh = mesh;
            g_pose.driver = driver;
            g_pose.instance = instance;
            g_pose.chain = final_chain_nodes(instance);
            g_pose.calibrated = pose_calibrate(cfg);
        }
        // Keep physics off: the game turns it on again after a spawn and when it rescales the body.
        double now = now_seconds();
        if (now - g_pose.last_check >= 0.5 || g_pose.arm_style != cfg.pose_arm_style)
        {
            g_pose.last_check = now;
            if (is_simulating(mesh, "pelvis") == 1 || g_pose.arm_style != cfg.pose_arm_style)
            {
                HSVR_LOG_EVERY(5.0, "Pose: the game switched physics on again; switching it off");
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
                Quat base = quat_from_yaw_deg(base_yaw);
                Vec3 total = base.inverse().rotate(Vec3{head_world.x - anchor.x, head_world.y - anchor.y, 0.0});
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
        set_component_transform(mesh, place, (body * g_pose.mesh_rotation_local).normalized());

        Quat component_inv = component_rotation(driver).inverse();
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
                have = true;
            }
            if (ModifyBoneNode* n = chain_node("head"))
            {
                if (have) node_set_rotation(*n, component_inv * (head_rot * g_pose.head_offset));
                else node_release(*n);
            }
        }
        else if (ModifyBoneNode* n = chain_node("head"))
        {
            node_release(*n);
        }
        for (int side = 0; side < 2; ++side)
        {
            if (cfg.pose_arms) pose_arm(side, cfg, state, body, component_inv);
            else
                for (const char* b : {"upperarm", "lowerarm", "hand"})
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
            HSVR_LOG("Pose: capsule velocity (%.0f, %.0f, %.0f) mode %d", v[0], v[1], v[2], mode);
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
        if (!g_hold_capsule.load() || !context || context != g_pose.pawn || !function) return false;
        if (g_receive_tick) return function == g_receive_tick;
        if (function->GetName() != STR("ReceiveTick")) return false;
        g_receive_tick = function;
        HSVR_LOG("Capsule: found your character's tick (%ls)", function->GetFullName().c_str());
        return true;
    }

    void capsule_before_tick(Hook::TCallbackIterationData<void>&, UObject* context, UFunction* function, void*)
    {
        if (!is_receive_tick(context, function)) return;
        g_hold_location = actor_location(context);
        g_hold_have = true;
    }

    void capsule_after_tick(Hook::TCallbackIterationData<void>&, UObject* context, UFunction* function, void*)
    {
        if (!is_receive_tick(context, function) || !g_hold_have) return;
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
        if (UObject* movement = object_property(context, STR("CharacterMovement")))
            if (FProperty* vp = movement->GetPropertyByNameInChain(STR("Velocity")))
                write_three(reinterpret_cast<uint8_t*>(movement) + vp->GetOffset_Internal(), vp, 0.0, 0.0, 0.0);
    }

    // ---- Your own head, hidden from your view only --------------------------------------------
    // The head is its own component (CharacterMesh_Head) and the hair another (Hair, a groom);
    // helmets are separate armour pieces and stay visible. "Owner no see" hides a component only in
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
        static UObject* hidden[2] = {nullptr, nullptr};
        static double last = 0.0;
        double now = hsvr::now_seconds();
        if (now - last < 1.0) return;
        last = now;
        UObject* pawn = nullptr;
        find_pawn_mesh(&pawn);
        bool willie = pawn && pawn->GetFullName().find(STR("Willie_BP")) != std::wstring::npos;
        bool want = cfg.hide_own_head && willie && cfg.anchor == "head" && !hsvr::shared().third_person.load() && hsvr::shared().stereo_installed.load();
        UObject* parts[2] = {nullptr, nullptr};
        if (willie)
        {
            parts[0] = object_property(pawn, STR("CharacterMesh_Head"));
            parts[1] = object_property(pawn, STR("Hair"));
        }
        for (int i = 0; i < 2; ++i)
        {
            UObject* now_part = want ? parts[i] : nullptr;
            if (hidden[i] && hidden[i] != now_part && hidden[i] == parts[i]) set_owner_hidden(hidden[i], false);
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
    struct ScriptEntry
    {
        double at = 0.0;
        std::string command;
        bool done = false;
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
        alignas(16) static uint8_t parameters[1024];
        memset(parameters, 0, sizeof(parameters));
        game_instance->ProcessEvent(function, parameters);
        HSVR_LOG("Script: called %ls on the game instance", function->GetFullName().c_str());
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
            size_t pos = 0;
            const std::string& text = cfg.dev_script;
            while (pos < text.size())
            {
                size_t end = text.find(';', pos);
                if (end == std::string::npos) end = text.size();
                std::string one = text.substr(pos, end - pos);
                pos = end + 1;
                size_t colon = one.find(':');
                if (colon == std::string::npos) continue;
                ScriptEntry entry;
                entry.at = atof(one.substr(0, colon).c_str());
                entry.command = one.substr(colon + 1);
                size_t first = entry.command.find_first_not_of(" \t");
                size_t last = entry.command.find_last_not_of(" \t\r\n");
                if (first == std::string::npos) continue;
                entry.command = entry.command.substr(first, last - first + 1);
                entries.push_back(entry);
            }
            if (!entries.empty()) HSVR_LOG("Script: %d timed step(s) from the settings file (dev_script)", (int)entries.size());
        }
        if (entries.empty()) return;
        double now = (double)(GetTickCount64() - start) / 1000.0;
        for (ScriptEntry& entry : entries)
        {
            if (entry.done || now < entry.at) continue;
            entry.done = true;
            SYSTEMTIME utc{};
            GetSystemTime(&utc);
            HSVR_LOG("Script: %.0f s: %s (%02d:%02d:%02d.%03d UTC)", entry.at, entry.command.c_str(), (int)utc.wHour, (int)utc.wMinute, (int)utc.wSecond,
                     (int)utc.wMilliseconds);
            // Mod commands for unattended tests; everything else goes to the engine console.
            if (entry.command.rfind("hsvr.physics_off ", 0) == 0)
            {
                g_physics_override = atoi(entry.command.c_str() + 17) != 0 ? 1 : 0;
                continue;
            }
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
    struct BoolReturnParams
    {
        bool ReturnValue;
    };

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
        player_physics_test(cfg);
        pose_probe(cfg);
        pose_body(cfg);
        hide_own_head(cfg);
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
        if (front_process != GetCurrentProcessId()) return;
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
