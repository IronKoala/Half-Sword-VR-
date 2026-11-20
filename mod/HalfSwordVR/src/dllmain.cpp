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
#include <cwchar>
#include <cwctype>
#include <functional>
#include <map>
#include <mutex>
#include <unordered_set>
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
#include <Unreal/UObjectArray.hpp>

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

    // ---- Objects the game has freed -------------------------------------------------------------
    // The mod keeps pointers to Willies, weapons and their parts from one frame to the next. Once the
    // game destroys one (a corpse removed by the test room's respawn, a fist, a new fight) its memory
    // is freed at the next garbage collection, and using the old pointer after that crashes (crash
    // 2026-11-20 08:13: elbow_tick read a respawn-removed Willie's mesh). The engine tells us what it
    // frees (a delete listener); every place that keeps a pointer is cleaned of those before it is
    // used again: gc_sync() runs at the top of every entry point of the mod (the engine tick and every
    // hook) and, when anything was freed, drops it from every holder (prune_freed and the holders
    // that registered with track_freed / on_freed). gc_sync() is one atomic read when nothing was.
    // The listener only appends (one atomic add and a store; no lock, no hashing): the game frees
    // every object at quit, on several threads, and a locked set made quitting take up to a minute
    // (test 2026-11-20 02:53-03:01: 24-63 s against 9 s without it). Two buffers: writers append to
    // the active one; gc_sync switches them, waits out writers already inside, and reads the old one.
    constexpr size_t kFreedCap = size_t(1) << 20;
    struct FreedBuffer
    {
        std::unique_ptr<const void*[]> slots{new const void*[kFreedCap]};
        std::atomic<size_t> count{0};
    };
    FreedBuffer g_freed_buf[2];
    std::atomic<int> g_freed_active{0};
    std::atomic<int> g_freed_writers{0};
    std::atomic<bool> g_freed_pending{false};
    std::unordered_set<const void*> g_freed_now; // the batch being pruned (game thread)
    bool g_freed_all = false;                    // more freed than the buffer holds: treat every pointer as freed
    uint64_t g_gc_epoch = 0;                     // + 1 for each batch: local caches rebuild on a change
    struct FreedListener : Unreal::FUObjectDeleteListener
    {
        void NotifyUObjectDeleted(const Unreal::UObjectBase* object, int32_t) override
        {
            g_freed_writers.fetch_add(1, std::memory_order_acq_rel);
            FreedBuffer& b = g_freed_buf[g_freed_active.load(std::memory_order_acquire)];
            size_t i = b.count.fetch_add(1, std::memory_order_relaxed);
            if (i < kFreedCap) b.slots[i] = object;
            g_freed_pending.store(true, std::memory_order_release);
            g_freed_writers.fetch_sub(1, std::memory_order_acq_rel);
        }
        // The engine asserts at quit if a delete listener is still registered (crash 2026-11-20 03:05).
        void OnUObjectArrayShutdown() override { Unreal::FUObjectArray::RemoveUObjectDeleteListener(this); }
    };
    FreedListener g_freed_listener;
    std::vector<std::function<void()>> g_prune_hooks;
    // True when p was freed in the batch being pruned (pointer compare only, never read).
    bool gone(const void* p) { return p && (g_freed_all || g_freed_now.count(p) != 0); }
    // A holder outside prune_freed (a function's own static) asks to be cleaned: f runs on each batch.
    bool on_freed(std::function<void()> f)
    {
        g_prune_hooks.push_back(std::move(f));
        return true;
    }
    bool track_freed(UObject** slot)
    {
        return on_freed([slot] {
            if (gone(*slot)) *slot = nullptr;
        });
    }
    template <typename M> void erase_gone_keys(M& m)
    {
        for (auto it = m.begin(); it != m.end();)
            it = gone(it->first) ? m.erase(it) : std::next(it);
    }
    void erase_gone(std::vector<UObject*>& v)
    {
        v.erase(std::remove_if(v.begin(), v.end(), [](UObject* o) { return gone(o); }), v.end());
    }
    void prune_freed(); // after every global holder
    void gc_sync()
    {
        if (!g_freed_pending.load(std::memory_order_acquire)) return;
        g_freed_pending.store(false, std::memory_order_release);
        int old = g_freed_active.load(std::memory_order_acquire);
        g_freed_active.store(1 - old, std::memory_order_release);
        while (g_freed_writers.load(std::memory_order_acquire) != 0) YieldProcessor();
        FreedBuffer& b = g_freed_buf[old];
        size_t n = b.count.load(std::memory_order_acquire);
        g_freed_all = n > kFreedCap;
        g_freed_now.clear();
        if (!g_freed_all)
        {
            g_freed_now.reserve(n);
            for (size_t k = 0; k < n; ++k) g_freed_now.insert(b.slots[k]);
        }
        b.count.store(0, std::memory_order_release);
        if (n == 0) return;
        ++g_gc_epoch;
        HSVR_LOG("GC: the game freed %zu object(s)%s; dropping them from the mod's lists", n, g_freed_all ? " (more than recorded: every kept pointer dropped)" : "");
        prune_freed();
        for (auto& f : g_prune_hooks) f();
        g_freed_now.clear();
        g_freed_all = false;
    }
    // ---- Where the mod's time goes (Perf: lines) -------------------------------------------------
    // Every entry point of the mod times itself (QueryPerformanceCounter, self time only: a hook that
    // runs inside another's call is taken out of the outer one). game_tick writes the totals every
    // perf_log_seconds with the frame rate and how many objects the game has, so a session that gets
    // slower shows whether the mod's own time or the game's grew (playtest 2026-11-20 10:05Z: 63 fps
    // fell to 13-25 fps over 7 minutes).
    enum PerfId
    {
        PerfTick, PerfDeepEvent, PerfCapsuleBefore, PerfCapsuleAfter, PerfPoseAnim, PerfFingersAnim, PerfFeetAnim, PerfFingersCopy,
        PerfHitBody, PerfStabHit, PerfStabPost, PerfStabRelease, PerfForceWatch, PerfPunchPre, PerfPunchPost, PerfRoomHooks, PerfCount
    };
    const char* const kPerfNames[PerfCount] = {"tick", "events", "before tick", "after tick", "pose", "fingers", "feet", "finger copy",
                                               "body hit", "weapon hit", "weapon hit post", "stab release", "force watch", "punch", "punch post", "room hooks"};
    struct PerfAcc
    {
        std::atomic<uint64_t> ticks{0}, calls{0};
    };
    PerfAcc g_perf[PerfCount];
    thread_local uint64_t t_perf_child = 0;
    inline uint64_t perf_now()
    {
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        return (uint64_t)c.QuadPart;
    }
    struct PerfScope
    {
        PerfId id;
        uint64_t start, saved;
        explicit PerfScope(PerfId i) : id(i), start(perf_now()), saved(t_perf_child) { t_perf_child = 0; }
        ~PerfScope()
        {
            uint64_t all = perf_now() - start;
            g_perf[id].ticks.fetch_add(all - (std::min)(all, t_perf_child), std::memory_order_relaxed);
            g_perf[id].calls.fetch_add(1, std::memory_order_relaxed);
            t_perf_child = saved + all;
        }
    };
#define HSVR_PROF(id) PerfScope perf_scope_(id)

    // Still a live object: not destroyed and not waiting for clean-up (below, after Call). Only for
    // pointers that are not freed (taken this frame, or kept in a holder that gc_sync cleans).
    bool object_alive(UObject* p);

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

    // The posed player pawn, only while it is still the one the player controller has. g_pose.pawn is
    // a plain pointer refreshed from the game tick; the hooks below (weapon hits, releases, forces) also
    // fire while a map unloads, after the old pawn has been freed, and reading it then crashed the game
    // on every level change (UE4SS+4163ee via stab_hit_hook, 2026-11-19 17:47 and the night before).
    // The controller's Pawn is a property the engine clears, so it never points at a freed pawn.
    struct PoseState;
    UObject* live_posed_pawn();
    void forget_level_objects();

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
    double g_button_crouch = 0.0; // button crouch depth (cm), see g_button_crouch_on
    // His capsule's height as the view and his body use it: smoothed (pose_actor_z_smooth_s), because
    // walking, the game's capsule loses and finds its floor every other tick and hops ~2 cm about once
    // a second; the view hopped at once and his body a frame later (playtest 11:44). -1e9 = not yet.
    double g_actor_z_smooth = -1e9;
    bool g_actor_z_smooth_on = false;
    void crouch_push_log(UObject* pawn, UObject* mesh);
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
        if (g_actor_z_smooth_on && g_actor_z_smooth > -1e8 && std::fabs(out.actor_position.z - g_actor_z_smooth) < 15.0)
            out.actor_position.z = g_actor_z_smooth;
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
        out.head_position.z -= g_button_crouch; // the button crouch takes the view down with him
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
        static bool reported_tracked = track_freed(&reported);
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
        static bool logged_for_tracked = track_freed(&logged_for);
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

    // The engine's own test (KismetSystemLibrary IsValid: not null, not marked as garbage). UE4SS's
    // IsPendingKill does not see a destroyed actor in this game (test 2026-11-20 08:36: removed
    // corpses stayed "alive").
    bool object_alive(UObject* p)
    {
        if (!p) return false;
        Unreal::FUObjectItem* item = Unreal::FUObjectArray::IndexToObject(p->GetInternalIndex());
        if (!item || item->GetUObject() != p || item->IsUnreachable()) return false;
        static UObject* ksl = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetSystemLibrary"));
        if (!ksl) return true;
        Call v(ksl, STR("IsValid"));
        if (!v.fn) return true;
        v.object(STR("Object"), p);
        v.run();
        FProperty* r = v.param(STR("ReturnValue"));
        return !r || *(v.buffer.data() + r->GetOffset_Internal()) != 0;
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

    // Puts the bone at this spot (component space), replacing the animation's.
    bool node_set_translation(ModifyBoneNode& n, const hsvr::Vec3& component_space)
    {
        if (!n.translation || !n.translation_mode || !n.translation_space) return false;
        *(n.node + n.translation_space->GetOffset_Internal()) = 1; // component space
        if (!write_three(n.node + n.translation->GetOffset_Internal(), n.translation, component_space.x, component_space.y, component_space.z)) return false;
        *(n.node + n.translation_mode->GetOffset_Internal()) = 1; // replace
        return true;
    }

    void node_release_translation(ModifyBoneNode& n)
    {
        if (n.translation_mode) *(n.node + n.translation_mode->GetOffset_Internal()) = 0;
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
        hsvr::Vec3 reach_target[2]; // scaled to his reach but not yet clamped to it (where you mean his hand to be)
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

    // ---- Torso lean (see pose_lean in config.hpp) -----------------------------------------------
    // The bones it bends, parent first; the last one (head) is only measured, never bent here.
    const char* const kLeanBones[7] = {"spine_02", "spine_03", "spine_04", "spine_05", "neck_01", "neck_02", "head"};
    const double kTwistWeight[6] = {0.10, 0.12, 0.14, 0.16, 0.22, 0.26};
    struct LeanState
    {
        bool calibrated = false;
        hsvr::Vec3 pts[7];       // the bones standing, relative to the actor, in the body's frame (at calibration)
        double fwd = 0.0, side = 0.0;     // degrees bent now (smoothed and held to what the head height allows)
        double want_fwd = 0.0, want_side = 0.0; // from the head alone
        double twist = 0.0;      // torso twist total (degrees), shared with pose_torso_twist
        hsvr::Quat q[6];         // each bone's turn this frame, in the body's frame (twist and bend)
        hsvr::Vec3 head_moved;   // how far the bend and twist carry the head, in the body's frame
        double pitch = 0.0, roll = 0.0; // your head's pitch down and roll right (degrees)
        double t = 0.0;
        double held = 1.0;       // share of the wanted bend allowed by the head height
    };
    LeanState g_lean;
    // Test: hsvr.fake_head_tilt <pitch down> <roll right> <drop cm> nods the TRACKED headset about the
    // base of the neck (12 cm below the head bone) and lowers it, for the pose only (not the view).
    std::atomic<double> g_fake_tilt_pitch{0.0}, g_fake_tilt_roll{0.0}, g_fake_tilt_drop{0.0};
    // Test: hsvr.fake_head_pose 1 = with no tracked headset (dev_fake_headset), a still head at the view's
    // anchor (where your eyes were at recentring), looking straight ahead; the tilt above applies to it.
    std::atomic<bool> g_fake_head_pose{false};
    // Test: hsvr.fake_head_wobble <deg> <hz> nods the head (tracked or fake) up and down by that much,
    // live from the clock, so the pose and the view's late copy see different head poses (as a moving
    // real head does a frame apart).
    std::atomic<double> g_fake_wobble_deg{0.0}, g_fake_wobble_hz{1.0};

    // The bend and twist of the 6 bones, and where they carry the head (body frame). Each bone turns
    // about its own joint and carries everything above it, as the end-of-graph Modify Bone nodes do
    // in component space (Additive).
    hsvr::Vec3 lean_fk(double fwd_deg, double side_deg, double twist_deg, hsvr::Quat out[6])
    {
        using namespace hsvr;
        Vec3 p[7];
        for (int i = 0; i < 7; ++i) p[i] = g_lean.pts[i];
        const Vec3 up{0, 0, 1}, right{0, 1, 0}, fwd{1, 0, 0};
        for (int i = 0; i < 6; ++i)
        {
            // Half Sword's Bend_F: spine_02..05 +20 deg each, neck_01 and neck_02 -30 each (of 80);
            // Bend_L/R: spine 22.5 each, neck -30 each (of 90). Forward is a turn about right; a lean to
            // the right is a turn about forward by minus the angle.
            double f = i < 4 ? fwd_deg / 4.0 : -fwd_deg * 0.375;
            double sd = i < 4 ? side_deg / 4.0 : -side_deg / 3.0;
            Quat bend = (quat_axis_angle(right, f * kDegToRad) * quat_axis_angle(fwd, -sd * kDegToRad)).normalized();
            Quat q = (quat_axis_angle(up, twist_deg * kTwistWeight[i] * kDegToRad) * bend).normalized();
            out[i] = q;
            for (int j = i + 1; j < 7; ++j) p[j] = p[i] + q.rotate(p[j] - p[i]);
        }
        return p[6] - g_lean.pts[6];
    }

    UObject* live_posed_pawn()
    {
        UObject* posed = g_pose.pawn;
        if (!posed) return nullptr;
        UObject* pawn = nullptr;
        find_pawn_mesh(&pawn);
        return pawn == posed ? pawn : nullptr;
    }
    std::atomic<bool> g_hold_capsule{false}; // see "The capsule stays where you are" below
    // Posed and calibrated this frame (crouch and body turning run around his tick).
    std::atomic<bool> g_pose_active{false};
    bool g_body_turning = false;
    // Crouch: how far your headset is below where you stood at recentering (cm, 0 standing).
    double g_crouch_drop = 0.0;
    // Button crouch (B&S: the stick crouches as well as bending down): left stick click toggles it; the
    // view and his body go down together by this much (cm, eased), on top of your own crouch.
    // (g_button_crouch is defined near the top: provide_anchor uses it)
    bool g_button_crouch_on = false;
    std::atomic<double> g_fake_head_drop{-1.0}; // test: hsvr.fake_head_drop <cm> (fake tracking only)
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
    double number_property(UObject* owner, FProperty* p);

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
        g_lean = LeanState{};
        g_lean.calibrated = true;
        for (int i = 0; i < 7; ++i)
        {
            if (!bone_exists(d, kLeanBones[i])) g_lean.calibrated = false;
            g_lean.pts[i] = body_inv.rotate(socket_location(d, bone_fname(kLeanBones[i])) - actor);
        }
        HSVR_LOG("Lean: %s; spine_02 at %.1f cm, head %.1f cm above the actor, head %.1f cm in front of spine_02",
                 g_lean.calibrated ? "calibrated" : "a bone is MISSING, lean off", g_lean.pts[0].z, g_lean.pts[6].z, g_lean.pts[6].x - g_lean.pts[0].x);
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
    // your own character (and looks through him too, should a part of him be hit). Weapons (held, belted or loose, scabbards included: a scabbard is a part of
    // its weapon) and creatures are looked through: the trace starts at his hips, and a weapon on the
    // left hip or in a hand over a foot used to count as the floor (the left foot popped up 85 cm,
    // about 40 times a minute). False if nothing else is hit within 2 m below `from_z`.
    bool is_a(UObject* object, const TCHAR* class_name);
    bool ground_below(UObject* pawn, double x, double y, double from_z, double& ground_z)
    {
        static UObject* cdo = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetSystemLibrary"));
        static UObject* gs = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__GameplayStatics"));
        if (!cdo || !pawn) return false;
        const double bottom = from_z - 200.0;
        double start = from_z;
        for (int attempt = 0; attempt < 5 && start > bottom; ++attempt)
        {
            Call trace(cdo, STR("LineTraceSingle"));
            if (!trace.fn) return false;
            trace.object(STR("WorldContextObject"), pawn);
            trace.three(STR("Start"), x, y, start);
            trace.three(STR("End"), x, y, bottom);
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
            double hit_z = z->GetClass().GetName() == STR("DoubleProperty") ? *reinterpret_cast<const double*>(at) : *reinterpret_cast<const float*>(at);
            // What was hit (the game's BreakHitResult gives the actor).
            UObject* actor = nullptr;
            if (gs)
            {
                Call b(gs, STR("BreakHitResult"));
                if (FProperty* hp = b.param(STR("Hit")))
                {
                    std::memcpy(b.buffer.data() + hp->GetOffset_Internal(), trace.buffer.data() + hit->GetOffset_Internal(), (std::min)(hp->GetSize(), hit->GetSize()));
                    b.run();
                    actor = b.object_result(STR("HitActor"));
                }
            }
            bool look_through = actor && (actor == pawn || is_a(actor, STR("ModularWeaponBP_C")) || is_a(actor, STR("Pawn")));
            if (!look_through)
            {
                ground_z = hit_z;
                return true;
            }
            HSVR_LOG_EVERY(2.0, "Feet: ground trace looked through %ls at z %.0f (%.0f cm below the hips)", actor->GetName().c_str(), hit_z, from_z - hit_z);
            start = hit_z - 1.0;
        }
        return false;
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
    // ---- Finger shapes, shown to the animation only ---------------------------------------------
    // ABP_Willie copies each hand's finger shape from the pawn's R/L_GripType_Current into its own
    // 'R/L Grip Type' inside BlueprintUpdateAnimation (@152268 / @153084), and the AnimGraph reads only
    // that copy (grip_type_flow.md). Willie's own tick reads the pawn's values for its rules (a non-zero
    // L_GripType_Current drops the right hand's two-handed grip 11 -> 2 every tick, @493002-496128), and
    // Set Up Hand Weapon rewrites them. So the mod never writes the pawn's values: it writes the
    // animation's copy right after that update, for the player only (pose_fingers_anim_only).
    int g_anim_grip[2] = {-1, -1};          // the finger shape the mod wants for that hand (-1 = the game's)
    double g_anim_grip_time[2] = {-1e9, -1e9};
    int g_anim_grip_two = -1;               // the second hand's, while two hands hold (g_two.second)
    int g_anim_grip_two_side = -1;
    void set_anim_grip(int side, int g)
    {
        g_anim_grip[side] = g;
        g_anim_grip_time[side] = hsvr::now_seconds();
    }
    // The body pose written inside his animation update (pose_in_anim_update), right before the arm
    // nodes are evaluated, instead of at the start of the engine tick: on frames where a held item
    // moved, his drawn hand showed the previous frame's solve (96% of the worst ticks, playtest 10:36),
    // which shook the hand on a steady sword. The engine tick still runs it when this has not fired.
    void pose_body(const hsvr::Config& cfg);
    std::atomic<double> g_pose_in_anim_at{-1.0};
    void pose_after_anim_update(Hook::TCallbackIterationData<void>&, UObject* context, UFunction* function, void*)
    {
        gc_sync();
        HSVR_PROF(PerfPoseAnim);
        if (!context || !g_pose.instance || context != g_pose.instance || !function) return;
        if (g_anim_update) { if (function != g_anim_update) return; }
        else
        {
            if (function->GetName() != STR("BlueprintUpdateAnimation")) return;
            g_anim_update = function;
        }
        hsvr::Config cfg = hsvr::config();
        if (!cfg.pose_in_anim_update) return;
        static bool said = false;
        if (!said)
        {
            said = true;
            HSVR_LOG("Pose: written inside his animation update (pose_in_anim_update)");
        }
        g_pose_in_anim_at.store(hsvr::now_seconds());
        pose_body(cfg);
    }

    // The mod's fist is in that hand (set each tick by grab_button), for the fist's finger shape.
    std::atomic<bool> g_fist_side[2] = {false, false};
    std::atomic<bool> g_fist_open[2] = {false, false}; // a slap: the fist is there but the hand is open (fingers as the game's)
    std::atomic<int> g_fist_shape_test{-99}; // test: hsvr.fist_shape <n> (-99 = pose_fist_shape)
    bool set_bool_property(UObject* owner, const TCHAR* name, bool value);
    bool set_variable(UObject* owner, const TCHAR* name, double number, UObject* object, const FName* fname);
    double read_number_variable(UObject* owner, const TCHAR* name);
    bool fingers_tracked_on(int side); // the hand's fingers follow yours (then the fist needs no shape)
    int fist_shape(const hsvr::Config& cfg)
    {
        int t = g_fist_shape_test.load();
        return t != -99 ? t : cfg.pose_fist_shape;
    }

    void fingers_after_anim_update(Hook::TCallbackIterationData<void>&, UObject* context, UFunction* function, void*)
    {
        gc_sync();
        HSVR_PROF(PerfFingersAnim);
        if (!g_pose_active.load() || !context || context != g_pose.instance || !function) return;
        if (g_anim_update) { if (function != g_anim_update) return; }
        else
        {
            if (function->GetName() != STR("BlueprintUpdateAnimation")) return;
            g_anim_update = function;
        }
        if (!hsvr::config().pose_fingers_anim_only) return;
        static FProperty* prop[2] = {nullptr, nullptr};
        static bool looked = false;
        if (!looked)
        {
            looked = true;
            prop[0] = context->GetPropertyByNameInChain(STR("L Grip Type"));
            prop[1] = context->GetPropertyByNameInChain(STR("R Grip Type"));
            HSVR_LOG("Fingers: the animation's grip types L %s, R %s (%ls / %ls)", prop[0] ? "found" : "MISSING", prop[1] ? "found" : "MISSING",
                     prop[0] ? prop[0]->GetClass().GetName().c_str() : STR("-"), prop[1] ? prop[1]->GetClass().GetName().c_str() : STR("-"));
        }
        double now = hsvr::now_seconds();
        static int last[2] = {-2, -2};
        for (int side = 0; side < 2; ++side)
        {
            if (!prop[side]) continue;
            int g = side == g_anim_grip_two_side ? g_anim_grip_two : (now - g_anim_grip_time[side] < 0.1 ? g_anim_grip[side] : -1);
            // A fist: the shape chosen for it (pose_fist_shape). The animation's own fist needs 'Hand
            // Active' (0 in VR), so the hand would stay open (playtest 2026-11-19 21:10).
            if (g_fist_side[side].load() && !g_fist_open[side].load() && !fingers_tracked_on(side))
            {
                int shape = fist_shape(hsvr::config());
                if (shape >= 0 && shape <= 17) g = shape;
                else if (shape == -1) set_bool_property(context, side == 0 ? STR("Grabbed L") : STR("Grabbed R"), true);
                else if (shape == 100 || shape == 101)
                {
                    set_bool_property(context, side == 0 ? STR("L Guarding") : STR("R Guarding"), true);
                    if (shape == 101 && read_number_variable(context, STR("Aim Swing Speed")) < 75.0)
                        set_variable(context, STR("Aim Swing Speed"), 75.0, nullptr, nullptr);
                }
            }
            if (g != last[side])
            {
                HSVR_LOG("Fingers: %s hand shows %d%s", side == 0 ? "left" : "right", g, g < 0 ? " (the game's own)" : "");
                last[side] = g;
            }
            if (g < 0) continue;
            uint8_t* at = reinterpret_cast<uint8_t*>(context) + prop[side]->GetOffset_Internal();
            auto kind = prop[side]->GetClass().GetName();
            if (kind == STR("ByteProperty") || kind == STR("EnumProperty")) *at = (uint8_t)g;
            else if (kind == STR("IntProperty")) *reinterpret_cast<int32_t*>(at) = g;
        }
    }

    void feet_after_anim_update(Hook::TCallbackIterationData<void>&, UObject* context, UFunction* function, void*)
    {
        gc_sync();
        HSVR_PROF(PerfFeetAnim);
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
        // His legs stay on the leg IK: the animation's kneel and falling branches (ABP_Willie BLB_131
        // R/L Kneel, BLB_29/TWB_22 Kneel Fall, TWB_80 Free Fall Rate) replace the IK'd legs with canned
        // poses that carry their own lower pelvis (crouch_ik.md). At a deep crouch the game's kneel
        // trigger fired and the whole body sank 50-100 cm (playtest 2026-11-18). Cleared in the
        // animation's copy only, after its update reads the pawn; the pawn's own values are untouched.
        if (hsvr::config().pose_crouch_keep_leg_ik)
        {
            static std::vector<FProperty*> clear_bools, clear_numbers;
            static bool looked = false;
            if (!looked)
            {
                looked = true;
                std::string names;
                for (UStruct* level = static_cast<UStruct*>(context->GetClassPrivate()); level; level = level->GetSuperStruct())
                    for (FProperty* p : level->ForEachProperty())
                    {
                        std::wstring n = p->GetName();
                        auto kind = p->GetClass().GetName();
                        bool kneel_flag = kind == STR("BoolProperty") && n.find(STR("Kneel")) != std::wstring::npos;
                        bool fall_rate = (kind == STR("DoubleProperty") || kind == STR("FloatProperty")) && n == STR("Free Fall Rate");
                        if (!kneel_flag && !fall_rate) continue;
                        (kneel_flag ? clear_bools : clear_numbers).push_back(p);
                        if (!names.empty()) names += ", ";
                        for (wchar_t ch : n) names += (char)(ch < 128 ? ch : '?');
                    }
                HSVR_LOG("Crouch: kept off in the animation every frame (legs stay on the IK): %s", names.empty() ? "NONE FOUND" : names.c_str());
            }
            std::string was;
            for (FProperty* p : clear_bools)
            {
                auto* flag = static_cast<FBoolProperty*>(p);
                uint8_t* byte = reinterpret_cast<uint8_t*>(context) + p->GetOffset_Internal() + flag->GetByteOffset();
                if (*byte & flag->GetFieldMask())
                {
                    std::wstring n = p->GetName();
                    was += " ";
                    for (wchar_t ch : n) was += (char)(ch < 128 ? ch : '?');
                }
                *byte &= (uint8_t)~flag->GetFieldMask();
            }
            for (FProperty* p : clear_numbers)
            {
                double v = number_property(context, p);
                if (v > 0.01)
                {
                    char part[48];
                    std::snprintf(part, sizeof(part), " Free Fall Rate %.2f", v);
                    was += part;
                }
                uint8_t* at = reinterpret_cast<uint8_t*>(context) + p->GetOffset_Internal();
                if (p->GetClass().GetName() == STR("DoubleProperty")) *reinterpret_cast<double*>(at) = 0.0;
                else *reinterpret_cast<float*>(at) = 0.0f;
            }
            if (!was.empty()) HSVR_LOG_EVERY(0.5, "Crouch: the animation wanted canned legs (crouch %.0f cm):%s; kept on the leg IK", g_crouch_drop, was.c_str());
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
        if (g_lean.calibrated)
        {
            // With the torso lean: the same twist plus the bend (lean_fk, the bones' turns in the body's
            // frame), turned into his component's frame.
            Quat q[6];
            lean_fk(g_lean.fwd, g_lean.side, total, q);
            Quat body = quat_from_yaw_deg(hips_yaw);
            Quat to_comp = component_inv * body;
            for (int i = 0; i < 6; ++i)
            {
                ModifyBoneNode* n = chain_node(kLeanBones[i]);
                if (!n) continue;
                if (total == 0.0 && g_lean.fwd == 0.0 && g_lean.side == 0.0) node_release(*n);
                else node_add_rotation(*n, (to_comp * q[i] * to_comp.inverse()).normalized());
            }
            g_pose.twist_deg = total;
            return;
        }
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
    std::atomic<int> g_fake_arm_epoch{0}; // bumped by every hsvr.arm_move: timed test paths restart
    std::atomic<double> g_fake_two_up{20.0};
    // hsvr.arm_move 10 <speed cm/s> <pitch deg> <height cm>: the right hand thrusts (stab tests).
    std::atomic<double> g_fake_thrust_pitch{60.0};
    std::atomic<double> g_fake_thrust_up{-5.0};
    double blade_length(UObject* w);
    bool mouth_point(UObject* pawn, hsvr::Vec3& out); // hsvr.arm_move 7 <cm>: the left hand this far above the right
    std::atomic<bool> g_contact_log{true};

    bool hand_on_weapon(int side, const hsvr::Config& cfg, hsvr::Vec3& target, hsvr::Quat& hand_rot);
    bool stuck_is_thrust(UObject* weapon);
    bool two_hand_pose(int side, hsvr::Vec3& target, hsvr::Quat& hand_rot);
    bool hand_on_scabbard(int side, hsvr::Vec3& target, hsvr::Quat& hand_rot);
    std::atomic<bool> g_hold_reach_clamp{true};
    // Test counters since the last hsvr.mark (for hsvr.expect): stuck let-goes, kicks (a held weapon
    // ending a tick more than 8 cm from where it was sent) and the worst hold lag (cm) per hand.
    std::atomic<int> g_count_letgo{0}, g_count_kicks{0};
    std::atomic<double> g_worst_lag[2] = {0.0, 0.0};
    std::atomic<bool> g_fake_continue{false}; // test: hsvr.press_a 1 holds right A for menu_buttons
    std::atomic<bool> g_fake_stick_click{false}; // test: hsvr.stick_click 1 holds the left stick click
    std::string g_park_socket;                  // test: hsvr.arm_park <socket> (arm mode 13): right hand there
    double g_arm_floor[3] = {45.0, 15.0, 5.0};
    std::atomic<double> g_fake_jitter_cm{0.0};
    bool is_fist(UObject* weapon);  // test: hsvr.fake_jitter <cm>: tracking-like noise on the fake hands (rms per axis)  // test: hsvr.arm_floor <fwd> <side> <height> (arm mode 14): right hand near the floor
    std::atomic<float> g_fake_trigger[2] = {-1.0f, -1.0f}; // test: hsvr.trigger <0|1> <value>, -1 = the controller
    std::atomic<float> g_fake_squeeze[2] = {-1.0f, -1.0f}; // test: hsvr.squeeze <0|1> <value>, -1 = the controller

    // Sets a yes/no property that the engine packs into a single bit (tests only).
    const char* sheathing_socket(int side); // the slot socket a hand is sheathing into, or null
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

    // What the arm solve used and made this frame (deep log): shoulder, elbow, hand (world), and what moved
    // the hand off the controller (1 on the held item, 2 two hands, 4 on a scabbard).
    hsvr::Vec3 g_solved_S[2], g_solved_E[2], g_solved_H[2];
    int g_solved_on[2] = {0, 0};
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
            else if (mode == 10)
            {
                // Stab test: the left hand still; the right thrusts straight ahead from 20 to 62 cm
                // at hsvr.arm_move 10 <speed> cm/s, holds 0.4 s, draws back over 0.8 s, waits; every 2.5 s.
                local = Vec3{40.0, out * 15.0, -10.0};
                if (side == 1)
                {
                    double v = (std::max)(20.0, g_fake_two_up.load());
                    double t_in = 42.0 / v;
                    double t = std::fmod(now, 2.5);
                    double x = 20.0;
                    if (g_fake_two_up.load() < 1.0) t = 99.0; // speed 0: held back, aiming only
                    if (t < t_in) x = 20.0 + v * t;
                    else if (t < t_in + 0.4) x = 62.0;
                    else if (t < t_in + 1.2) x = 62.0 - 42.0 * (t - t_in - 0.4) / 0.8;
                    local = Vec3{x, out * 5.0, g_fake_thrust_up.load()};
                }
            }
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
            if (mode == 15 && side == 1 && g_pose.pawn)
            {
                // Pickup test: the right controller this high above the grip point of the nearest loose
                // weapon (one no one holds, not sheathed), as a hand reaching for it on the floor.
                static UObject* item = nullptr;
                static bool item_tracked = track_freed(&item);
                static double found_at = -1.0;
                if (!item || now - found_at > 1.0)
                {
                    found_at = now;
                    item = nullptr;
                    std::vector<UObject*> ws;
                    UObjectGlobals::FindAllOf(STR("ModularWeaponBP_C"), ws);
                    Vec3 ap = actor_location(g_pose.pawn);
                    double best = 1e9;
                    for (UObject* w : ws)
                    {
                        if (!w || w->GetName().rfind(STR("Default__"), 0) == 0 || bool_property(w, STR("Is Sheathed")) || is_fist(w)) continue;
                        if (object_property(g_pose.pawn, STR("Weapon R")) == w || object_property(g_pose.pawn, STR("Weapon L")) == w) continue;
                        UObject* b = object_property(w, STR("BaseMesh"));
                        if (!b) continue;
                        double d = length(component_location(b) - ap);
                        if (d < best) { best = d; item = w; }
                    }
                }
                if (item)
                    if (UObject* b = object_property(item, STR("BaseMesh"))) grip = component_location(b) + Vec3{0, 0, g_arm_floor[2]};
            }
            if (mode == 18 && side == 1)
            {
                // Elbow / forearm test: the right hand swept from out to his right side, raised, across in
                // front of his chest in 0.12 s (a horizontal elbow strike: the forearm and elbow lead),
                // back over 0.6 s, every 2.2 s. Put a foe 35-45 cm in front.
                static double t0 = -1.0;
                static int seen18 = -1;
                if (seen18 != g_fake_arm_epoch.load()) t0 = now;
                seen18 = g_fake_arm_epoch.load();
                // Aimed at the nearest other Willie's head when there is one (the sweep's frame faces him, the
                // hand passing at 60% of his distance, so the forearm and elbow cross his head/neck).
                Quat frame = body;
                double reach = 18.0, height = 12.0;
                {
                    std::vector<UObject*> ws;
                    UObjectGlobals::FindAllOf(STR("Willie_BP_C"), ws);
                    UObject* foe = nullptr;
                    double best = 1e9;
                    for (UObject* w : ws)
                    {
                        if (!w || w == g_pose.pawn || w->GetName().rfind(STR("Default__"), 0) == 0) continue;
                        double d = length(actor_location(w) - shoulder);
                        if (d < best) { best = d; foe = w; }
                    }
                    if (UObject* fm = foe ? object_property(foe, STR("Mesh")) : nullptr)
                    {
                        Vec3 head = socket_location(fm, bone_fname("head"));
                        Vec3 to = head - shoulder;
                        double flat = std::sqrt(to.x * to.x + to.y * to.y);
                        if (flat > 5.0 && flat < 120.0)
                        {
                            frame = quat_from_yaw_deg(std::atan2(to.y, to.x) * 180.0 / 3.14159265358979);
                            reach = flat * 0.6;
                            height = to.z;
                        }
                    }
                }
                Vec3 a{reach, out * 38.0, height}, b{reach, -out * 25.0, height};
                double t = std::fmod(now - t0, 2.2);
                double u = t < 1.4 ? 0.0 : (t < 1.52 ? (t - 1.4) / 0.12 : (t < 1.6 ? 1.0 : (std::max)(0.0, 1.0 - (t - 1.6) / 0.6)));
                grip = shoulder + frame.rotate(a + (b - a) * u);
            }
            if ((mode == 16 || mode == 17) && side == 1 && g_pose.pawn)
            {
                // Fist and grapple tests, on the nearest other Willie. 16: the right hand rests 25 cm in
                // front of his shoulder 1.4 s, punches through the foe's head (10 cm past it) in 0.12 s,
                // stays 0.2 s and comes back over 0.5 s (a punch at ~5-8 m/s), every 2.2 s.
                // 17: the right hand on the foe's neck_02 for 2 s, then pulls back toward him 30 cm
                // over 1 s and holds there (a throat grab and pull).
                static UObject* foe = nullptr;
                static bool foe_tracked = track_freed(&foe);
                static double found_at = -1.0, t0 = -1.0;
                static int seen = -1;
                if (seen != g_fake_arm_epoch.load()) t0 = now;
                seen = g_fake_arm_epoch.load();
                if (!foe || now - found_at > 1.0)
                {
                    found_at = now;
                    foe = nullptr;
                    std::vector<UObject*> ws;
                    UObjectGlobals::FindAllOf(STR("Willie_BP_C"), ws);
                    Vec3 ap = actor_location(g_pose.pawn);
                    double best = 1e9;
                    for (UObject* w : ws)
                    {
                        if (!w || w == g_pose.pawn || w->GetName().rfind(STR("Default__"), 0) == 0) continue;
                        double d = length(actor_location(w) - ap);
                        if (d < best) { best = d; foe = w; }
                    }
                }
                UObject* fm = foe ? object_property(foe, STR("Mesh")) : nullptr;
                if (fm)
                {
                    Vec3 rest = shoulder + body.rotate(Vec3{25.0, out * 10.0, -5.0});
                    if (mode == 16)
                    {
                        Vec3 head = socket_location(fm, bone_fname("head"));
                        Vec3 dir = head - rest;
                        double dl = length(dir);
                        Vec3 hit = dl > 1.0 ? head + dir * (10.0 / dl) : head;
                        double t = std::fmod(now - t0, 2.2);
                        if (t < 1.4) grip = rest;
                        else if (t < 1.52) grip = rest + (hit - rest) * ((t - 1.4) / 0.12);
                        else if (t < 1.72) grip = hit;
                        else grip = hit + (rest - hit) * (std::min)(1.0, (t - 1.72) / 0.5);
                    }
                    else
                    {
                        Vec3 neck = socket_location(fm, bone_fname("neck_02"));
                        Vec3 back = rest - neck;
                        double bl = length(back);
                        Vec3 pulled = bl > 1.0 ? neck + back * (30.0 / bl) : neck;
                        double t = now - t0;
                        static Vec3 hold_at, pull_to;
                        if (t < 2.0)
                        {
                            hold_at = neck;
                            pull_to = pulled;
                            grip = neck;
                        }
                        else grip = hold_at + (pull_to - hold_at) * (std::min)(1.0, (t - 2.0) / 1.0);
                    }
                }
            }
            if (mode == 14 && side == 1 && g_pose.pawn)
            {
                // Floor test: the right controller this far ahead of and beside his capsule, this high above
                // the floor under it (the capsule's bottom).
                Vec3 ap = actor_location(g_pose.pawn);
                double floor_z = ap.z - 90.0 * component_scale_z(object_property(g_pose.pawn, STR("Mesh")));
                Vec3 flat = body.rotate(Vec3{g_arm_floor[0], g_arm_floor[1], 0.0});
                grip = Vec3{ap.x + flat.x, ap.y + flat.y, floor_z + g_arm_floor[2]};
            }
            if (mode == 13 && side == 1 && g_pose.pawn && !g_park_socket.empty())
            {
                // Holster test: the right hand's grip point parked on a named socket of his mesh.
                if (UObject* mesh = object_property(g_pose.pawn, STR("Mesh"))) grip = socket_location(mesh, bone_fname(g_park_socket));
            }
            if (mode == 12 && side == 1 && g_pose.pawn)
            {
                // Sheathe test: the right hand goes from a blade's length out along the right belt
                // scabbard's line in to its mouth over 2.5 s (the weapon in the hand goes in).
                static double t0 = -1.0;
                static int seen12 = -1;
                if (seen12 != 12) t0 = now;
                seen12 = mode;
                UObject* wr = object_property(g_pose.pawn, STR("Weapon R"));
                UObject* mesh = object_property(g_pose.pawn, STR("Mesh"));
                if (mesh)
                {
                    // The slot it is going into, else the first right slot.
                    const char* sock = object_property(g_pose.pawn, STR("Weapon Slot R 1")) && !(wr && object_property(g_pose.pawn, STR("Weapon Slot R 1")) == wr)
                                           ? "Sheath_Slot_R_2" : "Sheath_Slot_R_1";
                    if (const char* now_into = sheathing_socket(1)) sock = now_into; // across too
                    Vec3 m = socket_location(mesh, bone_fname(sock));
                    Vec3 ax = socket_rotation(mesh, bone_fname(sock)).rotate(Vec3{0, 0, 1});
                    double len = wr ? blade_length(wr) : 80.0;
                    double u = (std::min)(1.0, (now - t0) / 2.5);
                    grip = m - ax * ((len + 3.0) * (1.0 - u) - 8.0 * u);
                }
            }
            if (mode == 11 && side == 1 && g_pose.pawn)
            {
                // Draw test: the right hand goes to the first sheathed weapon's grip, holds 1.5 s, then
                // pulls along its blade's -Z (out of the scabbard) to 1.3 x its length over 2 s.
                static double t0 = -1.0, len = 80.0;
                static Vec3 p0, ax;
                static int seen = -1;
                if (seen != 11 || t0 < 0.0)
                {
                    t0 = -1.0;
                    for (const TCHAR* v : {STR("Weapon Slot R 1"), STR("Weapon Slot R 2"), STR("Weapon Slot L 1"), STR("Weapon Slot L 2"), STR("Weapon Slot Back")})
                    {
                        UObject* w = object_property(g_pose.pawn, v);
                        UObject* b = w ? object_property(w, STR("BaseMesh")) : nullptr;
                        if (!b) continue;
                        p0 = component_location(b);
                        ax = component_rotation(b).rotate(Vec3{0, 0, 1});
                        if (FProperty* lp = w->GetPropertyByNameInChain(STR("Length to Top R Default"))) len = (std::max)(20.0, number_property(w, lp));
                        t0 = now;
                        break;
                    }
                }
                seen = mode;
                if (t0 >= 0.0)
                {
                    double t = now - t0;
                    double s = t < 1.5 ? 0.0 : (std::min)(1.3 * len, (t - 1.5) / 2.0 * 1.3 * len);
                    grip = p0 - ax * s;
                }
            }
            grip_rot = (body * rot_to_quat(60.0, 0.0, 0.0)).normalized(); // controller handle tilted forward, as held
            // Mode 9: at the mouth, the hand turning over (a full turn about his right in 6 s), so a
            // held bottle tips its neck down at some point (drinking tests).
            if (mode == 10 && side == 1)
            {
                // The test hand turns, a little each tick, until the held weapon's tip points
                // straight ahead (its first "Tip" marker's forward, the direction the game reads a
                // thrust in), pitched down by hsvr.arm_move 10's pitch argument.
                static Quat aim{0, 0, 0, 1};
                Vec3 want = (body * rot_to_quat(-g_fake_thrust_pitch.load(), 0.0, 0.0)).normalized().rotate({1.0, 0.0, 0.0});
                UObject* held = g_pose.pawn ? object_property(g_pose.pawn, STR("Weapon R")) : nullptr;
                RawArray* tips = held ? held->GetValuePtrByPropertyNameInChain<RawArray>(STR("Tips")) : nullptr;
                if (tips && tips->Data && tips->Num > 0 && tips->Data[0])
                {
                    Call f(static_cast<UObject*>(tips->Data[0]), STR("GetForwardVector"));
                    f.run();
                    Vec3 have = f.vec();
                    Vec3 axis = cross(have, want);
                    double sn = length(axis), cs = have.x * want.x + have.y * want.y + have.z * want.z;
                    if (sn > 1e-6)
                    {
                        double ang = std::atan2(sn, cs) * 0.15;
                        aim = (quat_axis_angle(axis * (1.0 / sn), ang) * aim).normalized();
                    }
                }
                grip_rot = (aim * body * rot_to_quat(60.0, 0.0, 0.0)).normalized();
            }
            if (mode == 9) grip_rot = (body * quat_axis_angle(Vec3{0, 1, 0}, std::fmod(now, 6.0) / 6.0 * 2.0 * 3.14159265358979) * rot_to_quat(60.0, 0.0, 0.0)).normalized();
        }
        if (cfg.dev_pose_fake_tracking && g_fake_jitter_cm.load() > 0.0)
        {
            // Like a tracked controller: noise of this size at ~10-20 Hz (a fresh offset every 3 frames, eased).
            static Vec3 cur[2], next[2];
            static int n[2] = {0, 0};
            auto gauss = []() {
                double u1 = (std::rand() + 1.0) / (RAND_MAX + 2.0), u2 = (std::rand() + 1.0) / (RAND_MAX + 2.0);
                return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307 * u2);
            };
            double j = g_fake_jitter_cm.load();
            if (++n[side] % 3 == 0) next[side] = Vec3{gauss() * j, gauss() * j, gauss() * j};
            cur[side] = cur[side] + (next[side] - cur[side]) * 0.5;
            grip = grip + cur[side];
        }
        Vec3 target = grip + grip_rot.rotate({cfg.pose_wrist_forward_cm, cfg.pose_wrist_right_cm * out, cfg.pose_wrist_up_cm});
        g_pose.raw_target[side] = target;
        Vec3 S = socket_location(g_pose.driver, bone_fname("upperarm" + sx));
        // Your reach scaled to his (once your arm length is measured): from his shoulder, your hand is
        // as far out along his arm as it is along yours.
        if (cfg.pose_user_arm_cm > 1.0 && tracked) target = S + (target - S) * ((a.upper_length + a.lower_length) / cfg.pose_user_arm_cm);
        g_pose.target[side] = target;
        g_pose.reach_target[side] = target;
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
        g_solved_on[side] = on_weapon ? 1 : 0;
        if (two_hand_pose(side, target, Wh)) on_weapon = true, g_solved_on[side] |= 2;
        if (!on_weapon && hand_on_scabbard(side, target, Wh)) on_weapon = true, g_solved_on[side] |= 4;
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
        g_solved_S[side] = S, g_solved_E[side] = E, g_solved_H[side] = H;
        node_set_rotation(*chain_node("upperarm" + sx), component_inv * Wu);
        node_set_rotation(*chain_node("lowerarm" + sx), component_inv * Wl);
        node_set_rotation(*chain_node("hand" + sx), component_inv * Wh);
    }

    // pose_post_physics: his posed skeleton (the driver, where the pose is written in its animation
    // update, and the visible body that copies it) ticks after the physics step, so a drawn hand riding a
    // held item is placed on the item where it is drawn this frame, with no guess ahead. Posed before
    // the physics (as the game ticks it), the hand was placed from where the item was a frame earlier
    // plus a guess, while the hold sets the item a new velocity after that; the guess missed by about
    // the item's own frame-to-frame change and the hand shook on a steady sword (playtest 11:00).
    std::atomic<bool> g_pose_post_physics{false};
    void pose_tick_group(const hsvr::Config& cfg, UObject* driver, UObject* mesh)
    {
        static UObject* set_on[2] = {nullptr, nullptr};
        static bool set_on_tracked = on_freed([] { for (UObject*& o : set_on) if (gone(o)) o = nullptr; });
        static bool set_post[2] = {false, false};
        bool want = cfg.pose_post_physics && cfg.pose_in_anim_update;
        UObject* comps[2] = {driver, mesh};
        for (int i = 0; i < 2; ++i)
        {
            UObject* c = comps[i];
            if (!c) continue;
            if (set_on[i] == c && set_post[i] == want) continue;
            Call tg(c, STR("SetTickGroup"));
            if (!tg.fn) continue;
            // ETickingGroup: TG_PrePhysics 0, TG_PostPhysics 4.
            tg.byte(STR("NewTickGroup"), want ? 4 : 0);
            tg.run();
            set_on[i] = c;
            set_post[i] = want;
            HSVR_LOG("Pose: %s now ticks %s the physics", i == 0 ? "his posed skeleton" : "his visible body", want ? "after" : "before");
        }
        g_pose_post_physics.store(want && set_on[0] == driver && set_post[0]);
        static UObject* prereq_on = nullptr;
        static bool prereq_on_tracked = track_freed(&prereq_on);
        if (cfg.pose_mesh_after_driver && mesh && driver && prereq_on != mesh)
        {
            Call add(mesh, STR("AddTickPrerequisiteComponent"));
            add.object(STR("PrerequisiteComponent"), driver);
            if (add.fn && add.run())
            {
                prereq_on = mesh;
                HSVR_LOG("Pose: his visible body now always ticks after his posed skeleton");
            }
        }
    }

    void set_named_number(UObject* owner, const TCHAR* name, double value);

    // ---- Worn armour: animated every frame, and the helmet measured against your headset ---------
    // Each worn piece is its own skeletal mesh on CharacterMesh0 copying its pose (ABP_CopyPose,
    // PostUpdateWork), with update-rate optimisation on (torso_lean_and_helmet.md). pose_no_uro turns
    // that off on his body, head and armour, and has his body refresh its bones every frame.
    UObject* g_helmet = nullptr;     // the worn piece tagged "Head 2" or "Head 1" (helmet, hood), or null
    UObject* g_helmet_pawn = nullptr;
    std::vector<UObject*> g_head_pieces; // every worn piece on the head, and his head (CharacterMesh_Head)
    void armour_scan(const hsvr::Config& cfg, UObject* pawn, UObject* mesh)
    {
        static double last = 0.0;
        double now = hsvr::now_seconds();
        if (pawn == g_helmet_pawn && now - last < 1.0) return;
        last = now;
        if (pawn != g_helmet_pawn) g_helmet = nullptr;
        g_helmet_pawn = pawn;
        UObject* helmet = nullptr;
        std::vector<UObject*> parts;
        Call kids(mesh, STR("GetChildrenComponents"));
        kids.flag(STR("bIncludeAllDescendants"), false);
        if (kids.run())
            if (FProperty* cp = kids.param(STR("Children")))
            {
                struct RawArray
                {
                    UObject** data;
                    int32_t num, max;
                };
                RawArray* arr = reinterpret_cast<RawArray*>(kids.buffer.data() + cp->GetOffset_Internal());
                for (int32_t i = 0; arr->data && i < arr->num && i < 512; ++i)
                    if (UObject* c = arr->data[i])
                        if (c->GetClassPrivate()->GetName().find(STR("SkeletalMeshComponent")) != std::wstring::npos) parts.push_back(c);
            }
        int tagged = 0;
        std::vector<UObject*> head_pieces;
        if (UObject* head = object_property(pawn, STR("CharacterMesh_Head"))) head_pieces.push_back(head);
        for (UObject* c : parts)
            for (const TCHAR* tag : {STR("Head 2"), STR("Head 1")})
            {
                Call has(c, STR("ComponentHasTag"));
                has.name(STR("Tag"), FName(tag, FNAME_Add));
                if (!has.run()) continue;
                FProperty* rp = has.param(STR("ReturnValue"));
                if (rp && *(has.buffer.data() + rp->GetOffset_Internal()))
                {
                    if (!helmet || tag[5] == L'2') helmet = c; // the helmet slot (Head 2) over a hood (Head 1)
                    head_pieces.push_back(c);
                    ++tagged;
                    break;
                }
            }
        if (helmet != g_helmet)
            HSVR_LOG("Helmet: %ls (%d skeletal pieces on his body, %d on the head)", helmet ? helmet->GetFullName().c_str() : STR("none worn"), (int)parts.size(), tagged);
        g_helmet = helmet;
        g_head_pieces = head_pieces;
        if (cfg.pose_no_uro)
        {
            static int said = 0;
            parts.push_back(mesh);
            parts.push_back(object_property(pawn, STR("CharacterMesh_Head")));
            parts.push_back(object_property(pawn, STR("DriverSkeleton")));
            int changed = 0;
            for (UObject* c : parts)
            {
                if (!c) continue;
                FProperty* p = c->GetPropertyByNameInChain(STR("bEnableUpdateRateOptimizations"));
                if (!p || p->GetClass().GetName() != STR("BoolProperty")) continue;
                auto* flag = static_cast<FBoolProperty*>(p);
                uint8_t* byte = reinterpret_cast<uint8_t*>(c) + p->GetOffset_Internal() + flag->GetByteOffset();
                if (*byte & flag->GetFieldMask())
                {
                    *byte &= (uint8_t)~flag->GetFieldMask();
                    ++changed;
                }
            }
            // EVisibilityBasedAnimTickOption: AlwaysTickPoseAndRefreshBones 0.
            if (FProperty* p = mesh->GetPropertyByNameInChain(STR("VisibilityBasedAnimTickOption")))
            {
                uint8_t* at = reinterpret_cast<uint8_t*>(mesh) + p->GetOffset_Internal();
                if (*at != 0)
                {
                    HSVR_LOG("Pose: his body's animation tick option was %d; now refreshes its bones every frame", (int)*at);
                    *at = 0;
                }
            }
            if (changed && said < 20)
            {
                ++said;
                HSVR_LOG("Pose: update-rate optimisation off on %d of his meshes (body, head, armour)", changed);
            }
        }
    }

    // ---- Helmet on this frame's headset pose (pose_helmet_late) ----------------------------------
    // The pose runs on the headset pose read when the previous frame's views were set up; the view then
    // takes a newer one. Right after the view takes it, every head piece is moved rigidly by the change
    // (new headset pose x the one the pose used, inverted), so it sits exactly where the view is. Its
    // place on the body is put back at the start of the next pose.
    bool head_pose_world(const hsvr::Config& cfg, const hsvr::ControllerState& state, hsvr::Vec3& w, hsvr::Quat& r);
    struct HeadUsed
    {
        bool valid = false;
        hsvr::Vec3 at;   // headset (eyes) in the world, as this frame's pose used it
        hsvr::Quat rot;
        UObject* pawn = nullptr;
    } g_head_used;
    struct MovedPiece
    {
        UObject* comp;
        double loc[3], rot[3];
    };
    std::vector<MovedPiece> g_late_moved;
    struct LateStats
    {
        double worst_cm = 0, worst_deg = 0, sum_cm = 0, after_worst = 0, view_worst = 0, view_worst_deg = 0;
        int n = 0;
        double last_log = 0;
    } g_late_stats;

    void late_restore()
    {
        for (auto& m : g_late_moved)
        {
            Call c(m.comp, STR("K2_SetRelativeLocationAndRotation"));
            c.three(STR("NewLocation"), m.loc[0], m.loc[1], m.loc[2]);
            c.three(STR("NewRotation"), m.rot[0], m.rot[1], m.rot[2]);
            c.flag(STR("bSweep"), false);
            c.flag(STR("bTeleport"), true);
            c.run();
        }
        g_late_moved.clear();
    }

    void helmet_on_latch()
    {
        using namespace hsvr;
        hsvr::Config cfg = config();
        if (!cfg.pose_helmet_late || !g_head_used.valid || !g_pose_active.load()) return;
        UObject* pawn = live_posed_pawn();
        if (!pawn || pawn != g_head_used.pawn || pawn != g_helmet_pawn || g_head_pieces.empty()) return;
        Vec3 w;
        Quat r;
        if (!head_pose_world(cfg, controllers_state(), w, r)) return;
        // Where the head bone belongs on the view's headset pose (as the head lock puts it).
        Vec3 want = w - r.rotate(Vec3{cfg.eye_forward_cm, 0.0, cfg.eye_up_cm});
        Quat want_rot = (r * g_pose.head_offset).normalized();
        Quat dr = (r * g_head_used.rot.inverse()).normalized();
        double view_cm = length(w - g_head_used.at), view_deg = 2.0 * std::acos((std::min)(1.0, std::fabs(dr.w))) * kRadToDeg;
        late_restore();
        double off_cm = 0.0, off_deg = 0.0, after = 0.0;
        for (UObject* c : g_head_pieces)
        {
            if (!c) continue;
            FProperty* lp = c->GetPropertyByNameInChain(STR("RelativeLocation"));
            FProperty* rp = c->GetPropertyByNameInChain(STR("RelativeRotation"));
            if (!lp || !rp) continue;
            // Each piece is moved so ITS head bone lands on the view's head spot: this takes out both the
            // view's newer headset pose and anything his body drew late.
            Vec3 bone = socket_location(c, bone_fname("head"));
            Quat bone_rot = socket_rotation(c, bone_fname("head"));
            Quat d = (want_rot * bone_rot.inverse()).normalized();
            double cm = length(want - bone), deg = 2.0 * std::acos((std::min)(1.0, std::fabs(d.w))) * kRadToDeg;
            off_cm = (std::max)(off_cm, cm);
            off_deg = (std::max)(off_deg, deg);
            if (cm > 20.0 || deg > 45.0) continue; // not a frame's drift: leave it on the body
            MovedPiece m{c, {0, 0, 0}, {0, 0, 0}};
            read_three(reinterpret_cast<uint8_t*>(c) + lp->GetOffset_Internal(), lp, m.loc);
            read_three(reinterpret_cast<uint8_t*>(c) + rp->GetOffset_Internal(), rp, m.rot);
            Vec3 at = component_location(c);
            Quat rot = component_rotation(c);
            set_component_transform(c, want + d.rotate(at - bone), (d * rot).normalized(), true);
            g_late_moved.push_back(m);
            after = (std::max)(after, length(socket_location(c, bone_fname("head")) - want));
        }
        auto& st = g_late_stats;
        st.worst_cm = (std::max)(st.worst_cm, off_cm);
        st.worst_deg = (std::max)(st.worst_deg, off_deg);
        st.after_worst = (std::max)(st.after_worst, after);
        st.sum_cm += off_cm;
        st.view_worst = (std::max)(st.view_worst, view_cm);
        st.view_worst_deg = (std::max)(st.view_worst_deg, view_deg);
        ++st.n;
        double now = now_seconds();
        if (now - st.last_log >= 0.25)
        {
            st.last_log = now;
            HSVR_LOG("Helmet: as drawn before the late move, off the view's head spot by worst %.2f cm / %.2f deg (mean %.2f cm) over %d frames, %d head pieces; "
                     "after the move worst %.3f cm; the view's headset moved %.2f cm / %.2f deg since the pose",
                     st.worst_cm, st.worst_deg, st.n ? st.sum_cm / st.n : 0.0, st.n, (int)g_head_pieces.size(), st.after_worst, st.view_worst, st.view_worst_deg);
            st = LateStats{};
            st.last_log = now;
        }
    }

    // Your headset in the world, as the pose uses it (with the test's made-up nod, hsvr.fake_head_tilt).
    bool head_pose_world(const hsvr::Config& cfg, const hsvr::ControllerState& state, hsvr::Vec3& w, hsvr::Quat& r)
    {
        using namespace hsvr;
        if (state.head_valid)
        {
            if (!stereo_tracking_to_world(state.head, w, r)) return false;
        }
        else
        {
            double base_yaw = 0.0;
            if (!g_fake_head_pose.load() || !stereo_view_anchor(w, base_yaw)) return false;
            r = quat_from_yaw_deg(base_yaw);
        }
        double p = g_fake_tilt_pitch.load(), ro = g_fake_tilt_roll.load(), dr = g_fake_tilt_drop.load();
        if (double wa = g_fake_wobble_deg.load(); wa != 0.0) p += wa * std::sin(now_seconds() * 2.0 * kPi * g_fake_wobble_hz.load());
        if (p != 0.0 || ro != 0.0 || dr != 0.0)
        {
            Vec3 eye_offset{cfg.eye_forward_cm, 0.0, cfg.eye_up_cm};
            Vec3 pivot = w - r.rotate(eye_offset) - Vec3{0.0, 0.0, 12.0};
            Quat y = quat_from_yaw_deg(quat_yaw_deg(r));
            Quat t = (quat_axis_angle(y.rotate({0, 1, 0}), p * kDegToRad) * quat_axis_angle(y.rotate({1, 0, 0}), -ro * kDegToRad)).normalized();
            w = pivot + t.rotate(w - pivot) - Vec3{0.0, 0.0, dr};
            r = (t * r).normalized();
        }
        return true;
    }

    // Torso lean for this frame: how far to bend (from your head's pitch and roll, smoothed), held to
    // what the head height allows (his hips can't rise above standing: drop + head_moved.z >= -slack),
    // and the twist share (pose_torso_twist). Fills g_lean.q / head_moved.
    void lean_update(const hsvr::Config& cfg, bool have_head, const hsvr::Quat& head_rot, double heading, double drop)
    {
        using namespace hsvr;
        double now = now_seconds(), dt = now - g_lean.t;
        g_lean.t = now;
        double want_f = 0.0, want_s = 0.0, twist = 0.0;
        if (have_head)
        {
            Vec3 f = head_rot.rotate({1, 0, 0}), rt = head_rot.rotate({0, 1, 0});
            g_lean.pitch = std::asin((std::max)(-1.0, (std::min)(1.0, -f.z))) * kRadToDeg;
            g_lean.roll = std::asin((std::max)(-1.0, (std::min)(1.0, -rt.z))) * kRadToDeg;
            if (cfg.pose_lean && g_lean.calibrated)
            {
                want_f = (std::min)(cfg.pose_lean_max_deg, (std::max)(0.0, g_lean.pitch - cfg.pose_lean_pitch_dead_deg) * cfg.pose_lean_pitch_gain);
                double r = std::fabs(g_lean.roll) - cfg.pose_lean_roll_dead_deg;
                want_s = r > 0.0 ? (std::min)(cfg.pose_lean_side_max_deg, r * cfg.pose_lean_roll_gain) * (g_lean.roll < 0.0 ? -1.0 : 1.0) : 0.0;
            }
            if (cfg.pose_torso_twist > 0.0)
            {
                double rel = quat_yaw_deg(head_rot) - heading;
                while (rel > 180.0) rel -= 360.0;
                while (rel < -180.0) rel += 360.0;
                twist = (std::max)(-90.0, (std::min)(90.0, rel)) * (std::min)(1.0, cfg.pose_torso_twist);
            }
        }
        g_lean.want_fwd = want_f;
        g_lean.want_side = want_s;
        double a = (cfg.pose_lean_smooth_s > 1e-4 && dt > 0.0 && dt < 0.2) ? 1.0 - std::exp(-dt / cfg.pose_lean_smooth_s) : 1.0;
        double f = g_lean.fwd + (want_f - g_lean.fwd) * a, sd = g_lean.side + (want_s - g_lean.side) * a;
        g_lean.twist = twist;
        if (!g_lean.calibrated)
        {
            g_lean.fwd = g_lean.side = 0.0;
            for (auto& q : g_lean.q) q = Quat{};
            g_lean.head_moved = {};
            return;
        }
        // Held to the head height: the largest share of (f, sd) whose head drop fits.
        double lo = 0.0, hi = 1.0;
        Quat q[6];
        Vec3 m = lean_fk(f, sd, twist, q);
        if (drop + m.z < -cfg.pose_lean_slack_cm)
        {
            for (int it = 0; it < 10; ++it)
            {
                double mid = 0.5 * (lo + hi);
                if (drop + lean_fk(f * mid, sd * mid, twist, q).z < -cfg.pose_lean_slack_cm) hi = mid;
                else lo = mid;
            }
            f *= lo;
            sd *= lo;
            m = lean_fk(f, sd, twist, q);
            g_lean.held = lo;
        }
        else
            g_lean.held = 1.0;
        g_lean.fwd = f;
        g_lean.side = sd;
        for (int i = 0; i < 6; ++i) g_lean.q[i] = q[i];
        g_lean.head_moved = m;
    }

    void pose_body(const hsvr::Config& cfg)
    {
        using namespace hsvr;
        UObject* pawn = nullptr;
        UObject* mesh = find_pawn_mesh(&pawn);
        // A new map or a new life: everything kept about the old pawn's weapons and holds goes.
        if (g_pose.pawn && pawn != g_pose.pawn) forget_level_objects();
        if (pawn && pawn == g_pose.pawn) late_restore(); // the head pieces back on his body (pose_helmet_late)
        else g_late_moved.clear();
        g_head_used.valid = false;
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
            pose_tick_group(cfg, driver, mesh);
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
        if (cfg.pose_follow_room && (state.head_valid || g_fake_head_pose.load()) && stereo_view_anchor(anchor, base_yaw))
        {
            Vec3 head_world;
            Quat head_rot;
            if (head_pose_world(cfg, state, head_world, head_rot))
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
        // The capsule's height, smoothed once a frame (see g_actor_z_smooth); the view uses the same value.
        {
            static double last_t = 0.0;
            double tnow = now_seconds(), dt = tnow - last_t;
            last_t = tnow;
            double tau = cfg.pose_actor_z_smooth_s;
            g_actor_z_smooth_on = tau > 1e-4;
            if (!g_actor_z_smooth_on || g_actor_z_smooth < -1e8 || std::fabs(actor.z - g_actor_z_smooth) > 15.0 || !(dt > 0.0 && dt < 0.2))
                g_actor_z_smooth = actor.z;
            else
                g_actor_z_smooth += (actor.z - g_actor_z_smooth) * (1.0 - std::exp(-dt / tau));
        }
        actor.z = g_actor_z_smooth;
        Vec3 place = actor + body.rotate(g_pose.mesh_offset_local);
        place.x = actor.x - pelvis_off.x;
        place.y = actor.y - pelvis_off.y;
        // Crouching: the body goes down by as much as your headset is below where you stood when
        // you recentred. His leg IK keeps the feet planted, so the knees bend. Never raised above
        // standing (the leg IK only stretches 1.1x), and at most 55 cm (times his size) down.
        g_crouch_drop = 0.0;
        if (cfg.pose_crouch && cfg.dev_pose_fake_tracking)
        {
            // Made-up path: slowly down to 40 cm and back up; hsvr.fake_head_drop <cm> holds a depth.
            double held = g_fake_head_drop.load();
            g_crouch_drop = held >= 0.0 ? (std::min)(55.0 * component_scale_z(mesh), held) : 20.0 - 20.0 * std::cos(now_seconds() * 0.4);
        }
        else if (cfg.pose_crouch && (state.head_valid || g_fake_head_pose.load()))
        {
            Vec3 view_anchor, head_world;
            Quat head_rot;
            double unused_yaw = 0.0;
            if (stereo_view_anchor(view_anchor, unused_yaw) && head_pose_world(cfg, state, head_world, head_rot))
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
        if (cfg.pose_crouch && cfg.pose_crouch_button_cm > 0.0)
        {
            static bool click_was = false;
            bool click = state.hand[0].stick_click || g_fake_stick_click.load();
            if (click && !click_was)
            {
                g_button_crouch_on = !g_button_crouch_on;
                controllers_buzz(0, 0.25f);
                HSVR_LOG("Crouch: left stick click: button crouch %s (%.0f cm)", g_button_crouch_on ? "on" : "off", cfg.pose_crouch_button_cm);
            }
            click_was = click;
            static double last = now_seconds();
            double tnow = now_seconds(), step = (std::min)(0.1, tnow - last) * cfg.pose_crouch_button_cms;
            last = tnow;
            double want = g_button_crouch_on ? cfg.pose_crouch_button_cm : 0.0;
            g_button_crouch += (std::max)(-step, (std::min)(step, want - g_button_crouch));
        }
        else
            g_button_crouch = 0.0;
        g_crouch_drop = (std::min)(55.0 * component_scale_z(mesh), g_crouch_drop + g_button_crouch);
        // Torso lean: the bend carries his head forward and down; his hips go back by the forward part
        // and stay up by the downward part, so his head still ends at your neck and the chest moves in.
        {
            Vec3 hw;
            Quat hr;
            bool have_head = !cfg.dev_pose_fake_tracking && head_pose_world(cfg, state, hw, hr);
            lean_update(cfg, have_head, hr, heading, g_crouch_drop);
            Vec3 m = g_lean.head_moved;
            place = place - body.rotate(Vec3{m.x, m.y, 0.0});
            g_crouch_drop = (std::max)(0.0, g_crouch_drop + m.z);
        }
        if (g_crouch_drop > 10.0) crouch_push_log(pawn, mesh);
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
            bool have = !cfg.dev_pose_fake_tracking && head_pose_world(cfg, state, head_world, head_rot);
            if (!have && cfg.dev_pose_fake_tracking)
            {
                // Made-up path: looks left and right, and up and down.
                double t = now_seconds();
                head_rot = (body * rot_to_quat(20.0 * std::sin(t * 0.7), 40.0 * std::sin(t * 0.5), 0.0)).normalized();
                if (g_turn_valid && cfg.pose_body_turn) // the same made-up look the body turning follows
                    head_rot = (quat_from_yaw_deg(g_turn_yaw + 80.0 * std::sin(t * 0.25)) * rot_to_quat(20.0 * std::sin(t * 0.7), 0.0, 0.0)).normalized();
                have = true;
            }
            g_head_used.valid = have && !cfg.dev_pose_fake_tracking;
            g_head_used.at = head_world;
            g_head_used.rot = head_rot;
            g_head_used.pawn = pawn;
            // The helmet (and his head) as drawn last frame against where the head was sent last frame:
            // worst and mean over each quarter second (log "Helmet:").
            {
                static Vec3 prev_want;
                static Quat prev_rot;
                static bool prev_ok = false;
                static double worst_cm = 0, worst_deg = 0, sum_cm = 0, mesh_worst = 0, last_log = 0;
                static int count = 0;
                if (prev_ok)
                {
                    auto angle = [](const Quat& a, const Quat& b) {
                        double d = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
                        return 2.0 * std::acos((std::min)(1.0, d)) * kRadToDeg;
                    };
                    double mesh_cm = length(socket_location(mesh, bone_fname("head")) - prev_want);
                    mesh_worst = (std::max)(mesh_worst, mesh_cm);
                    if (g_helmet && g_helmet_pawn == pawn)
                    {
                        double cm = length(socket_location(g_helmet, bone_fname("head")) - prev_want);
                        double deg = angle(socket_rotation(g_helmet, bone_fname("head")), prev_rot);
                        worst_cm = (std::max)(worst_cm, cm);
                        worst_deg = (std::max)(worst_deg, deg);
                        sum_cm += cm;
                        ++count;
                    }
                }
                if (now - last_log >= 0.25)
                {
                    last_log = now;
                    if (prev_ok)
                        HSVR_LOG("Helmet: vs the headset last frame: worst %.2f cm / %.2f deg, mean %.2f cm over %d frames%s; his head bone worst %.2f cm",
                                 worst_cm, worst_deg, count ? sum_cm / count : 0.0, count, g_helmet ? "" : " (no helmet worn)", mesh_worst);
                    worst_cm = worst_deg = sum_cm = mesh_worst = 0.0;
                    count = 0;
                }
                prev_ok = false;
                if (have && cfg.pose_head_lock && g_pose_anchor_fixed && !cfg.dev_pose_fake_tracking)
                {
                    prev_want = head_world - head_rot.rotate(Vec3{cfg.eye_forward_cm, 0.0, cfg.eye_up_cm});
                    prev_rot = (head_rot * g_pose.head_offset).normalized();
                    prev_ok = true;
                }
            }
            if (ModifyBoneNode* n = chain_node("head"))
            {
                if (have) node_set_rotation(*n, component_inv * (head_rot * g_pose.head_offset));
                else node_release(*n);
                // The head where the headset is: the eyes sit eye_forward/eye_up from the head bone (as
                // the view anchor was measured), so the bone is that far back and down from them.
                bool placed = false;
                if (have && cfg.pose_head_lock && g_pose_anchor_fixed && !cfg.dev_pose_fake_tracking)
                {
                    Vec3 eye_offset{cfg.eye_forward_cm, 0.0, cfg.eye_up_cm};
                    Vec3 want = head_world - head_rot.rotate(eye_offset);
                    // Never farther than the animation's neck reaches, plus pose_head_lock_max_cm.
                    Vec3 neck = socket_location(driver, bone_fname("neck_02"));
                    static double neck_len = -1.0;
                    if (neck_len < 0.0 && !*(n->node + n->translation_mode->GetOffset_Internal()))
                        neck_len = length(socket_location(driver, bone_fname("head")) - neck);
                    if (neck_len > 0.0)
                    {
                        Vec3 d = want - neck;
                        double dl = length(d), cap = neck_len + (std::max)(0.0, cfg.pose_head_lock_max_cm);
                        if (dl > cap) want = neck + d * (cap / dl);
                        double scale = (std::max)(1e-3, component_scale_z(driver));
                        placed = node_set_translation(*n, component_inv.rotate(want - component_location(driver)) * (1.0 / scale));
                        HSVR_LOG_EVERY(2.0, "Pose: head bone put at the headset (%.1f cm from the neck, its own neck %.1f cm%s)", dl, neck_len,
                                       dl > cap ? ", held at the limit" : "");
                    }
                }
                if (!placed) node_release_translation(*n);
            }
            have_look = have;
            look_rot = head_rot;
        }
        else if (ModifyBoneNode* n = chain_node("head"))
        {
            node_release(*n);
        }
        pose_torso_twist(cfg, have_look, look_rot, heading, component_inv);
        // The game's own forward bends (crouch Root Height, Lean Forward (Grabbing)) feed
        // Anim_Add_Bend_AO from the animation's copies, written by the update this runs after; with the
        // lean they are set to standing, so only your lean bends him (torso_lean_and_helmet.md).
        if (cfg.pose_lean && cfg.pose_lean_cancel_game_bend && g_lean.calibrated && cfg.pose_in_anim_update && g_pose_in_anim_at.load() >= now - 0.001)
        {
            set_named_number(g_pose.instance, STR("Root Height"), 85.0 * component_scale_z(mesh));
            set_named_number(g_pose.instance, STR("Lean Forward (Grabbing)"), 0.0);
        }
        armour_scan(cfg, pawn, mesh);
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
            {
                // Where his chest is from your eyes (body frame): + forward = under your view when you look down.
                Vec3 hw;
                Quat hr;
                if (!cfg.dev_pose_fake_tracking && head_pose_world(cfg, state, hw, hr))
                {
                    Vec3 c = body.inverse().rotate(socket_location(mesh, bone_fname("spine_05")) - hw);
                    Vec3 cl = body.inverse().rotate(socket_location(mesh, bone_fname("clavicle_l")) - hw);
                    Vec3 cr = body.inverse().rotate(socket_location(mesh, bone_fname("clavicle_r")) - hw);
                    HSVR_LOG("Lean: your head pitch %.0f roll %.0f; wanted forward %.0f side %.0f, bent forward %.0f side %.0f (%.0f%% allowed by head height); "
                             "head carried (%.1f, %.1f, %.1f) cm, crouch %.0f cm; chest (spine_05) %.1f cm in front of your eyes, %.1f below; clavicles %.1f / %.1f in front",
                             g_lean.pitch, g_lean.roll, g_lean.want_fwd, g_lean.want_side, g_lean.fwd, g_lean.side, g_lean.held * 100.0, g_lean.head_moved.x,
                             g_lean.head_moved.y, g_lean.head_moved.z, g_crouch_drop, c.x, -c.z, cl.x, cr.x);
                }
            }
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
    // Step Height 2 just before his tick: the leash's height uses this value (walking_floor_loss.md).
    bool g_step_h2_have = false;
    double g_step_h2_pre = 0.0;

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
    void keep_player_off_knees(UObject* pawn, bool after);
    // Test: hsvr.pawn_pin <variable> <value> holds a number variable of his pawn at a value every tick,
    // after his tick and before the hold reads it (e.g. Stamina 10, Arm R Tonus 0.25); "off" clears all.
    std::vector<std::pair<std::wstring, double>> g_pawn_pins;
    bool set_variable(UObject* owner, const TCHAR* name, double number, UObject* object, const FName* fname);

    // ---- Swing speed from the controllers -------------------------------------------------------
    // The game's "Aim Swing Speed" (ASS, 0..75) comes only from the mouse (Willie UG @667580) and is
    // 0 in VR, yet weapon skill growth, combat stance, fists, wrist locks, the AI's threat timing,
    // weapons staying live when fallen, the unsheath length and more read it (aim_swing_speed_flow.md).
    // It is written in his tick before nearly every reader, so the mod sets its only input, "Swing
    // Velocity proxy" (ASS = (proxy - 25) * 1.5), before his tick, divided by how much the tick's own
    // decay takes off (measured after each tick). The value: the faster hand that holds something
    // (a weapon, item or fist; an empty hand waving does not swing), relative to the body, mapped
    // pose_swing_speed_lo..hi m/s -> 0..75, exactly 0 at rest (some game rules need == 0).
    double read_number_variable(UObject* owner, const TCHAR* name);
    double g_hand_world_v[2] = {0.0, 0.0}; // m/s
    // A draw from a scabbard in progress, per hand (see try_draw).
    struct DrawState
    {
        bool active = false;
        int slot_side = 1;           // whose flags the game uses (the slot's side: 0 L, 1 R)
        UObject* weapon = nullptr;
        UObject* scabbard = nullptr;
        double t0 = 0.0;
        double last_log = -1e9;
        bool sheathing = false;      // going in rather than out
        const char* socket = "";     // the slot's socket (where the scabbard belongs)
        double length = 80.0;        // the blade's length (cm)
        bool block_cleared = false;
    };
    DrawState g_draw[2];
    const char* sheathing_socket(int side)
    {
        const DrawState& d = g_draw[side];
        return d.active && d.sheathing && d.socket && d.socket[0] ? d.socket : nullptr;
    }
    // A blade just drawn still has its tip at the mouth, lined up: no sheathe until it has been taken
    // well away first (playtest 02:44: every draw went straight back into a sheathe, 1 ms after it freed).
    bool g_sheathe_armed[2] = {true, true};
    // Where each drawn weapon's own scabbard really opens, measured when it was drawn (it sat seated):
    // the blade's start (the guard end of its blade zone) and its line, in the scabbard's frame. The
    // belt socket is only where the scabbard hangs from, and moves with it when the off hand angles it.
    struct SheathFit
    {
        UObject* scab = nullptr;
        int slot_side = 1;
        int slot = 0; // 0 = slot 1, 1 = slot 2 of that side
        const char* socket = "";
        hsvr::Vec3 mouth_local, axis_local;
    };
    std::unordered_map<UObject*, SheathFit> g_sheath_fit;
    bool any_draw() { return g_draw[0].active || g_draw[1].active; }
    bool cfg_scabbard_free() { return hsvr::config().pose_draw_scabbard_free; }
    bool try_draw(int side, UObject* pawn, UObject* mesh, const hsvr::Vec3& at, const hsvr::Config& cfg);
    bool try_scabbard_grab(int side, UObject* pawn, UObject* mesh, const hsvr::Vec3& at, const hsvr::Config& cfg);
    void scabbard_let_go(int side, const char* why);
    bool scabbard_held(int side);
    void scabbard_before_tick(UObject* pawn);
    void scabbard_tick(UObject* pawn, const hsvr::Config& cfg);
    double g_swing_want = 0.0, g_swing_written = 0.0, g_swing_keep = 0.89, g_swing_last_t = -1.0;
    hsvr::Vec3 g_swing_rel[2];
    double g_swing_v[2] = {0.0, 0.0};
    FProperty* g_swing_proxy = nullptr;
    bool g_swing_looked = false;
    void swing_speed_before_tick(UObject* pawn)
    {
        using namespace hsvr;
        const Config& cfg = hsvr::config();
        g_swing_written = 0.0;
        {
            // Each hand's speed in the world (for throws), ~30 ms smoothing.
            static hsvr::Vec3 last[2];
            static double last_t = -1.0;
            double t = now_seconds(), d = last_t > 0.0 ? t - last_t : 0.0;
            last_t = t;
            for (int side = 0; side < 2; ++side)
            {
                Vec3 at = g_pose.target[side];
                if (g_pose.hand_valid[side] && d > 1e-4 && d < 0.1)
                {
                    double v = length(at - last[side]) / d / 100.0;
                    g_hand_world_v[side] += (v - g_hand_world_v[side]) * (1.0 - std::exp(-d / 0.03));
                }
                else g_hand_world_v[side] = 0.0;
                last[side] = at;
            }
        }
        if (!cfg.pose_feed_swing_speed) return;
        if (!g_swing_looked)
        {
            g_swing_looked = true;
            g_swing_proxy = pawn->GetPropertyByNameInChain(STR("Swing Velocity proxy"));
            HSVR_LOG("Swing: the game's swing speed input %s (%ls)", g_swing_proxy ? "found" : "MISSING", g_swing_proxy ? g_swing_proxy->GetClass().GetName().c_str() : STR("-"));
        }
        if (!g_swing_proxy) return;
        double now = now_seconds(), dt = g_swing_last_t > 0.0 ? now - g_swing_last_t : 0.0;
        g_swing_last_t = now;
        Vec3 body = actor_location(pawn);
        double fastest = 0.0;
        for (int side = 0; side < 2; ++side)
        {
            Vec3 rel = g_pose.target[side] - body;
            if (g_pose.hand_valid[side] && dt > 1e-4 && dt < 0.1)
            {
                double v = length(rel - g_swing_rel[side]) / dt / 100.0; // m/s
                double a = 1.0 - std::exp(-dt / 0.03);                   // ~30 ms smoothing (tracking jitter)
                g_swing_v[side] += (v - g_swing_v[side]) * a;
            }
            else g_swing_v[side] = 0.0;
            g_swing_rel[side] = rel;
            bool holds = object_property(pawn, side == 0 ? STR("Weapon L") : STR("Weapon R")) != nullptr;
            if (holds) fastest = (std::max)(fastest, g_swing_v[side]);
        }
        double lo = cfg.pose_swing_speed_lo, hi = (std::max)(lo + 0.1, cfg.pose_swing_speed_hi);
        double want = fastest <= lo ? 0.0 : (std::min)(75.0, (fastest - lo) / (hi - lo) * 75.0);
        // Rises at once, falls like the game's own (a swing's speed lingers ~0.1-0.2 s).
        if (want >= g_swing_want || dt <= 0.0) g_swing_want = want;
        else g_swing_want += (want - g_swing_want) * (1.0 - std::exp(-dt / (std::max)(0.01, cfg.pose_swing_speed_fall_s)));
        if (g_swing_want < 0.5) g_swing_want = 0.0;
        if (any_draw())
        {
            // A draw breaks free at Length x MRC(Aim Swing Speed 0..75 -> 0.85..0): kept at the
            // game's 85% (Matthew 2026-11-19) by holding the swing speed at 0 while drawing.
            if (number_property(pawn, g_swing_proxy) > 0.0) set_variable(pawn, STR("Swing Velocity proxy"), 0.0, nullptr, nullptr);
            return;
        }
        if (g_swing_want <= 0.0) return; // the game's own (0 at rest, or the mouse's)
        double proxy_now = number_property(pawn, g_swing_proxy);
        double p = (25.0 + g_swing_want / 1.5) / (std::max)(0.1, g_swing_keep);
        if (p <= proxy_now) return; // the mouse already swings harder
        set_variable(pawn, STR("Swing Velocity proxy"), p, nullptr, nullptr);
        g_swing_written = p;
    }
    void swing_speed_after_tick(UObject* pawn)
    {
        if (g_swing_written <= 0.0 || !g_swing_proxy) return;
        double after = number_property(pawn, g_swing_proxy);
        double k = after / g_swing_written;
        if (k > 0.05 && k <= 1.0) g_swing_keep += (k - g_swing_keep) * 0.5; // the tick's decay, measured
        static double last_log = -1e9;
        double now = hsvr::now_seconds();
        if (now - last_log > 1.0)
        {
            last_log = now;
            HSVR_LOG("Swing: hands %.1f / %.1f m/s, swing speed wanted %.0f, game's now %.1f (input kept %.2f per tick)", g_swing_v[0], g_swing_v[1], g_swing_want,
                     read_number_variable(pawn, STR("Aim Swing Speed")), g_swing_keep);
        }
    }

    void fist_flags_before_tick(UObject* pawn);
    void body_grab_before_tick(UObject* pawn);
    void fist_flags_after_tick(UObject* pawn);
    void body_grab_after_tick(UObject* pawn);
    void body_hit_watch(UObject* pawn);
    // ---- Test input: the pawn's own input events, as the mouse and keys call them ------------------
    // hsvr.input <name part> <value> <seconds>: every tick for that long, each of the pawn's
    // InpAxisEvt_/InpActEvt_ functions whose name contains <name part> is called with AxisValue =
    // <value> (seconds 0 = once). hsvr.input_list logs their names. For flat-mode punch measurements.
    struct TestInput { std::wstring part; double value = 0.0; double until = 0.0; bool once = false; };
    std::vector<TestInput> g_test_inputs;
    bool g_input_list_now = false;
    void install_punch_watch();
    void input_before_tick(UObject* pawn)
    {
        if (!pawn) return;
        install_punch_watch(); // flat mode has no posed tick to install it
        if (g_input_list_now)
        {
            g_input_list_now = false;
            std::wstring names;
            for (UStruct* level = static_cast<UStruct*>(pawn->GetClassPrivate()); level; level = level->GetSuperStruct())
                for (UFunction* f : level->ForEachFunction())
                {
                    std::wstring n = f->GetName();
                    if (n.rfind(STR("InpAxisEvt_"), 0) == 0 || n.rfind(STR("InpActEvt_"), 0) == 0) names += n + STR("; ");
                }
            HSVR_LOG("Input: the pawn's input events: %ls", names.c_str());
        }
        if (g_test_inputs.empty()) return;
        double now = hsvr::now_seconds();
        for (auto it = g_test_inputs.begin(); it != g_test_inputs.end();)
        {
            for (UStruct* level = static_cast<UStruct*>(pawn->GetClassPrivate()); level; level = level->GetSuperStruct())
                for (UFunction* f : level->ForEachFunction())
                {
                    std::wstring n = f->GetName();
                    if (n.rfind(STR("InpAxisEvt_"), 0) != 0 && n.rfind(STR("InpActEvt_"), 0) != 0) continue;
                    if (n.find(it->part) == std::wstring::npos) continue;
                    std::vector<uint8_t> buf = param_block(f);
                    for (FProperty* p : f->ForEachProperty())
                        if (p->GetName() == STR("AxisValue"))
                        {
                            auto kind = p->GetClass().GetName();
                            if (kind == STR("FloatProperty")) *reinterpret_cast<float*>(buf.data() + p->GetOffset_Internal()) = (float)it->value;
                            else if (kind == STR("DoubleProperty")) *reinterpret_cast<double*>(buf.data() + p->GetOffset_Internal()) = it->value;
                        }
                    pawn->ProcessEvent(f, buf.data());
                    if (it->once) HSVR_LOG("Input: called %ls (%.2f)", n.c_str(), it->value);
                }
            if (it->once || now >= it->until) it = g_test_inputs.erase(it);
            else ++it;
        }
    }

    void capsule_before_tick(Hook::TCallbackIterationData<void>&, UObject* context, UFunction* function, void*)
    {
        gc_sync();
        HSVR_PROF(PerfCapsuleBefore);
        // Test input runs in flat mode too (no posed body): the player's pawn's own tick.
        if ((!g_test_inputs.empty() || g_input_list_now) && context && function && function->GetName() == STR("ReceiveTick"))
        {
            UObject* pc = find_player_controller();
            if (pc && object_property(pc, STR("Pawn")) == context) input_before_tick(context);
        }
        if (!is_receive_tick(context, function)) return;
        g_hold_location = actor_location(context);
        g_hold_have = true;
        {
            FProperty* p = context->GetPropertyByNameInChain(STR("Step Height 2"));
            g_step_h2_have = p != nullptr;
            g_step_h2_pre = p ? number_property(context, p) : 0.0;
        }
        if (g_pose_active.load())
        {
            log_joint_before_tick(context);
            block_offhand_search_before_tick(context);
            keep_player_off_knees(context, false);
            turn_body_before_tick();
            stick_walk_before_tick(context);
            swing_speed_before_tick(context);
            fist_flags_before_tick(context);
            body_grab_before_tick(context);
            for (int side = 0; side < 2; ++side)
                if (g_draw[side].active) set_bool_property(context, side == 0 ? STR("Grab L Target Found") : STR("Grab R Target Found"), false);
            scabbard_before_tick(context);
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
            // B&S: a held item ignores the whole player body (item.ignoredRagdoll, bs_hold_measured.md §1);
            // the other hand's call (a second hand on it) adds only its own forearm.
            bool whole = hsvr::config().pose_hold_ignore_body && g_arm_ignore[1 - side].weapon != weapon;
            if ((!hsvr::config().pose_hold_ignore_hand && bone.ToString() == hand.ToString()) || (!whole && !in_forearm(mesh, bone, side))) continue;
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

    // His own body against a held weapon: the closest of his physics bodies to the weapon's axis
    // (pommel end to tip, 12 points), if within `within` cm. Vanilla weapons collide with his own
    // body too (self_collision.md), so a weapon stopped by it is not stuck in something else.
    double own_body_contact(UObject* pawn, UObject* weapon, double within, std::wstring* bone_out = nullptr, int holding_side = -1)
    {
        using namespace hsvr;
        UObject* mesh = object_property(pawn, STR("Mesh"));
        UObject* pommel = object_property(weapon, STR("PommelSk"));
        UObject* tip = object_property(weapon, STR("TippyTipScene"));
        if (!mesh || !pommel || !tip) return -1.0;
        Vec3 a = component_location(pommel), b = component_location(tip);
        double best = -1.0;
        for (FName bone : physics_body_bones(mesh))
        {
            // The holding arm from the forearm down never collides with its weapon (ignore_arm_collision),
            // so it is not what stops it (playtest 2: "stopped by his own hand_r").
            if (holding_side >= 0 && in_forearm(mesh, bone, holding_side)) continue;
            for (int i = 0; i <= 11; ++i)
            {
                Vec3 pt = a + (b - a) * (i / 11.0);
                Call c(mesh, STR("GetClosestPointOnCollision"));
                c.three(STR("Point"), pt.x, pt.y, pt.z);
                c.name(STR("BoneName"), bone);
                if (!c.run()) break;
                double d = c.number();
                if (d >= 0.0 && d <= within && (best < 0.0 || d < best))
                {
                    best = d;
                    if (bone_out) *bone_out = bone.ToString();
                }
            }
        }
        return best;
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

    // ---- B&S's hold (Matthew 2026-11-19, measured from his own B&S session: bs_hold_measured.md) ----
    // Each hand holds the item by soft drives (a spring and damper with a force cap), not by putting it
    // exactly on the hand: PhysX's D6 drives, solved here the same way, as soft constraints with
    // sequential impulses over the frame (Box2D-style soft step: gamma = 1/(dt(c + dt k)), bias =
    // k/(c + dt k) * error), all hands together. Positions cm, mass kg, so forces are N x 100 and
    // torques N m x 1e4; springs in N/m are the same number in kg/s^2.
    hsvr::Vec3 clamp_length(const hsvr::Vec3& v, double most);
    struct SoftPoint
    {
        hsvr::Vec3 r;      // from the centre of mass to the held point (world)
        hsvr::Vec3 err;    // held point minus its target
        hsvr::Vec3 vt;     // the target's velocity
        double k = 0.0, c = 0.0, fmax = 0.0;
        bool slide = false; // free along `axis` (the hand sliding on the handle)
        hsvr::Vec3 axis;
        hsvr::Vec3 acc;    // accumulated impulse
    };
    inline Mat3 skew(const hsvr::Vec3& r)
    {
        Mat3 m;
        m.m[0][1] = -r.z, m.m[0][2] = r.y, m.m[1][0] = r.z, m.m[1][2] = -r.x, m.m[2][0] = -r.y, m.m[2][1] = r.x;
        return m;
    }
    // v, w: the item's centre-of-mass velocity and spin, changed in place. Angular drive (the main
    // hand's turn) when ka > 0: err_rot = the turn from target to current (scaled axis), wt its target spin.
    void bs_solve(double mass, const Mat3& invI, hsvr::Vec3& v, hsvr::Vec3& w, SoftPoint* pts, int n, const hsvr::Vec3& err_rot, const hsvr::Vec3& wt, double ka,
                  double ca, double tmax, double dt, int iters, hsvr::Vec3& ang_acc)
    {
        using namespace hsvr;
        Mat3 I3 = Mat3::diag(1, 1, 1);
        Mat3 Ksoft[4];
        for (int i = 0; i < n; ++i)
        {
            SoftPoint& p = pts[i];
            Mat3 S = skew(p.r);
            Mat3 K = I3 * (1.0 / mass) + (S * invI * S) * -1.0;
            double gamma = 1.0 / (dt * (p.c + dt * p.k) + 1e-12);
            Ksoft[i] = (K + I3 * gamma).inverse();
            p.acc = Vec3{};
        }
        double ga = ka > 0.0 ? 1.0 / (dt * (ca + dt * ka) + 1e-12) : 0.0;
        Mat3 Kang = ka > 0.0 ? (invI + I3 * ga).inverse() : Mat3{};
        ang_acc = Vec3{};
        for (int it = 0; it < iters; ++it)
        {
            for (int i = 0; i < n; ++i)
            {
                SoftPoint& p = pts[i];
                double gamma = 1.0 / (dt * (p.c + dt * p.k) + 1e-12);
                Vec3 bias = p.err * (p.k / (p.c + dt * p.k + 1e-12));
                Vec3 cdot = v + cross(w, p.r) - p.vt;
                Vec3 lam = (Ksoft[i] * (cdot + bias + p.acc * gamma)) * -1.0;
                if (p.slide) lam = lam - p.axis * dot(lam, p.axis);
                Vec3 old = p.acc;
                p.acc = clamp_length(p.acc + lam, p.fmax * dt);
                lam = p.acc - old;
                v = v + lam * (1.0 / mass);
                w = w + invI * cross(p.r, lam);
            }
            if (ka > 0.0)
            {
                Vec3 bias = err_rot * (ka / (ca + dt * ka + 1e-12));
                Vec3 lam = (Kang * ((w - wt) + bias + ang_acc * ga)) * -1.0;
                Vec3 old = ang_acc;
                ang_acc = clamp_length(ang_acc + lam, tmax * dt);
                w = w + invI * (ang_acc - old);
            }
        }
    }

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
        bool fingers = false; // curl that hand's fingers (pose_shield_hand_grip_type) while held
        // On a handle (B&S): the segment it may slide along (BaseMesh frame), where on it (0..1), and
        // the finger shape (game grip type, -1 = the game's own).
        bool has_handle = false;
        hsvr::Vec3 ha, hb;
        double t = 0.0;
        std::string handle;
        int grip_type = -1;
        // B&S: on a grab the item stays where it was (kept there while the game sets it up) and the
        // hold then pulls it into the hand over pose_grab_settle_s.
        bool hold_at = false;
        hsvr::Vec3 at_pos;
        hsvr::Quat at_rot;
    };
    FreeGrip g_pending_free[2]; // applied when that hand's next hold starts
    UObject* g_pending_item[2] = {nullptr, nullptr}; // the item it is for (kept in the hand until the hold starts)

    struct HoldState
    {
        UObject* weapon = nullptr;
        FreeGrip free;
        bool handle_slide = false;     // sliding along its handle this tick (axial momentum kept)
        hsvr::Vec3 vel_f, ang_f;       // the hand's velocity and spin at the point held, smoothed
        bool vel_f_set = false;
        hsvr::Vec3 handle_axis;        // that handle's line, world
        double handle_keep = 0.0;      // how much of its own motion along the line it keeps (0..1)
        bool handle_sliding_logged = false;
        double handle_damp = 0.0;      // the slide's damper along the handle (N s/m), from the trigger
        double settle_until = -1.0;    // pulled from settle_from into the hand until then
        bool flipped = false;          // turned end for end by the grip flip (for the log)
        bool drag_capped = false;      // dragging a body by a stuck weapon with all the hand's strength
        double drag_slip_since = -1.0; // ... since when, with the grip past reach (the grip slipping)
        double settle_s = 0.0;
        hsvr::Vec3 settle_from_pos;
        hsvr::Quat settle_from_rot;
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
        double self_checked_at = -1.0; // the stop the own-body check last looked at
        bool self_blocked = false;
        double last_blocked_cm = -1.0; // last tick's blocked_cm (a weapon still coming is not stuck)
        // The slide: how far the hand has moved along the weapon (cm along its +Z from the game's grip
        // point), and the grip zone it may move in (found once per weapon).
        double slide = 0.0;
        bool zone_known = false;
        double zone_lo = 0.0, zone_hi = 0.0;
        double zone_stop = 0.0, zone_tip = 0.0; // the guard (top stop) and the tip, from the game's grip point
        double body_strength = 1.0; // the game's stats on this hand's hold (strength x injury x stamina)
        bool into_hand_tried = false; // pose_shield_in_hand looked once
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

    // ---- No knockdowns for the player ------------------------------------------------------------
    // The game's kneel (Willie_BP UG @616819/@618028 Kneel Falling: a knee within ~6 cm of the floor
    // once Consciousness + All Body Tonus drops under 200 and he leans forward; UG @620739 fires Kneel
    // Event from it, from a crouch under 60 cm, or from a weak pelvis drive) is a fall for an AI body.
    // For a posed player it is a trap: while R/L Kneel is on, Legs Tonus Far Step goes to 0 when the
    // nearer foot is 1 m from Last Step Location (UG @397262), which leg tonus and Root Linear
    // Constraint Power (UG @427425) are multiplied by, and the body sinks with nothing to get it up.
    // So for the player the flags are kept off and Far Step at 1, before and after his tick. The
    // properties are found once per class; the cost is a few byte writes a tick.
    void keep_player_off_knees(UObject* pawn, bool after)
    {
        if (!pawn || !hsvr::config().pose_player_no_kneel) return;
        static UClass* looked_for = nullptr;
        static FProperty* flags[4] = {};
        static FProperty* far_step = nullptr;
        static const TCHAR* flag_names[4] = {STR("R Kneel"), STR("L Kneel"), STR("R Kneel Falling"), STR("L Kneel Falling")};
        UClass* cls = pawn->GetClassPrivate();
        if (cls != looked_for)
        {
            looked_for = cls;
            std::string missing;
            for (int i = 0; i < 4; ++i)
            {
                FProperty* p = pawn->GetPropertyByNameInChain(flag_names[i]);
                flags[i] = p && p->GetClass().GetName() == STR("BoolProperty") ? p : nullptr;
                if (!flags[i]) { missing += " "; for (const TCHAR* c = flag_names[i]; *c; ++c) missing += (char)*c; }
            }
            FProperty* p = pawn->GetPropertyByNameInChain(STR("Legs Tonus Far Step"));
            far_step = p && p->GetClass().GetName() == STR("DoubleProperty") ? p : nullptr;
            if (!far_step) missing += " Legs Tonus Far Step";
            HSVR_LOG("Knees: your player's kneel and kneel-fall kept off, leg tonus far-step kept at 1 (pose_player_no_kneel)%s%s",
                     missing.empty() ? "" : "; NOT FOUND:", missing.c_str());
        }
        uint8_t* base = reinterpret_cast<uint8_t*>(pawn);
        unsigned was = 0;
        for (int i = 0; i < 4; ++i)
        {
            if (!flags[i]) continue;
            auto* flag = static_cast<FBoolProperty*>(flags[i]);
            uint8_t* byte = base + flags[i]->GetOffset_Internal() + flag->GetByteOffset();
            if (*byte & flag->GetFieldMask()) was |= 1u << i;
            *byte &= (uint8_t)~flag->GetFieldMask();
        }
        double far_now = 1.0; // ("far" is a Windows macro)
        if (far_step)
        {
            double* v = reinterpret_cast<double*>(base + far_step->GetOffset_Internal());
            far_now = *v;
            *v = 1.0;
        }
        if (was || far_now < 0.999)
        {
            static double last_line = -10.0;
            static unsigned seen = 0;
            static double far_low = 1.0;
            seen |= was;
            far_low = (std::min)(far_low, far_now);
            double now = hsvr::now_seconds();
            if (now - last_line >= 1.0)
            {
                std::string names;
                for (int i = 0; i < 4; ++i)
                    if (seen & (1u << i)) { names += " "; for (const TCHAR* c = flag_names[i]; *c; ++c) names += (char)*c; }
                HSVR_LOG("Knees: the game wanted your player down (found %s his tick):%s; far-step low %.2f; kept up (Leg R Tonus %.2f, Root Linear Constraint Power %.2f)",
                         after ? "after" : "before", names.empty() ? " -" : names.c_str(), far_low, read_number_variable(pawn, STR("Leg R Tonus")),
                         read_number_variable(pawn, STR("Root Linear Constraint Power")));
                last_line = now;
                seen = 0;
                far_low = 1.0;
            }
        }
    }

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
    bool holster_snap(int side, UObject* pawn, UObject* mesh, UObject* w, const hsvr::Config& cfg);
    UObject* scabbard_pin(UObject* scab);
    void remove_fist(int side, UObject* pawn, const char* why);
    // A throw (B&S: released faster than 2 m/s): the game counts an unheld weapon's hits only while its
    // "Just Thrown" is set and "Last Parent" is the thrower (ModularWeaponBP @67055; hs_throwing.md),
    // which Set Up Hand Weapon sets from "Dropped With no Damage" == false. Its flight is its own (the
    // game adds nothing); B&S doubles the release velocity. The game clears Just Thrown only after a
    // hit, so the mod clears it when the throw lands or misses, and keeps Last Parent while it flies
    // (the game drops it beyond 3 m).
    struct Thrown
    {
        UObject* weapon = nullptr;
        UObject* base = nullptr;
        double t0 = 0.0;
    };
    std::vector<Thrown> g_thrown;

    // The held weapon's centre of mass over the last ticks (time, position), for a throw's speed: the
    // physics' own velocity on the release tick is whatever the hold last set, which can be a large
    // catch-up (playtest 11:25: 40.1 m/s from a 2.2 m/s hand = the B&S solver's 20 m/s clamp x2).
    struct ComSample { double t; hsvr::Vec3 p; UObject* weapon; };
    ComSample g_com_hist[2][8];
    int g_com_head[2] = {0, 0};
    void note_com(int side, UObject* weapon, const hsvr::Vec3& p)
    {
        g_com_hist[side][g_com_head[side]] = ComSample{hsvr::now_seconds(), p, weapon};
        g_com_head[side] = (g_com_head[side] + 1) % 8;
    }
    // Its velocity over about the last 60 ms (false when there is too little history for it).
    bool recent_com_velocity(int side, UObject* weapon, hsvr::Vec3& v)
    {
        int newest = (g_com_head[side] + 7) % 8;
        const ComSample& a = g_com_hist[side][newest];
        if (a.weapon != weapon) return false;
        for (int k = 1; k < 8; ++k)
        {
            const ComSample& b = g_com_hist[side][(newest - k + 8) % 8];
            if (b.weapon != weapon || b.t <= 0.0) return false;
            if (a.t - b.t >= 0.06 || k == 7)
            {
                if (a.t - b.t < 1e-3) return false;
                v = (a.p - b.p) * (1.0 / (a.t - b.t));
                return true;
            }
        }
        return false;
    }

    void drop_weapon(int side, UObject* pawn, bool thrown = false)
    {
        UObject* held = pawn ? object_property(pawn, side == 0 ? STR("Weapon L") : STR("Weapon R")) : nullptr;
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
            if (n.find(STR("Dropped")) != std::wstring::npos && p->GetClass().GetName() == STR("BoolProperty") && !thrown)
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
        // B&S: a holster stays on the body when its item is dropped. The game's release takes a drawn
        // weapon's scabbard off the belt (Set Up <S> Hand Weapon @11946-@13481: it destroys the scabbard's
        // pelvis pin when the slot's scabbard is the released weapon's), also on the mod's hand-overs. Its
        // Scabbard reference is hidden from that check for the call and put back after.
        UObject* kept_scab = nullptr;
        FProperty* scab_prop = held ? held->GetPropertyByNameInChain(STR("Scabbard")) : nullptr;
        if (hsvr::config().pose_keep_scabbard_on_drop && scab_prop && scab_prop->GetClass().GetName() == STR("ObjectProperty"))
        {
            UObject** slot = reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(held) + scab_prop->GetOffset_Internal());
            if (*slot && scabbard_pin(*slot))
            {
                kept_scab = *slot;
                *slot = nullptr;
            }
        }
        pawn->ProcessEvent(fn, parameters);
        if (kept_scab)
        {
            *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(held) + scab_prop->GetOffset_Internal()) = kept_scab;
            HSVR_LOG("Hold: %ls's scabbard kept on the belt (%s)", held->GetName().c_str(), scabbard_pin(kept_scab) ? "still pinned" : "its pin is GONE anyway");
        }
        g_hold[side] = HoldState{}; // a new hold (and slide) when anything is next held
        g_grip_log[side] = GripLog{}; // its 0.5 s "Grip:" line stops with the hold
        HSVR_LOG("Hold: dropped with %ls(%ls)%s", name, names.c_str(), thrown ? " as a throw" : "");
        if (thrown && held)
        {
            using namespace hsvr;
            const Config& cfg = hsvr::config();
            UObject* base = object_property(held, STR("BaseMesh"));
            if (!object_property(held, STR("Last Parent"))) set_variable(held, STR("Last Parent"), 0.0, pawn, nullptr);
            Vec3 v{};
            if (base)
            {
                Call lv(base, STR("GetPhysicsLinearVelocity"));
                lv.run();
                v = lv.vec();
                Vec3 mv;
                bool measured = recent_com_velocity(side, held, mv);
                if (measured)
                {
                    HSVR_LOG("Throw: %s hand's weapon moved at %.1f m/s over the last ticks (the physics had %.1f)", side == 0 ? "left" : "right", length(mv) / 100.0,
                             length(v) / 100.0);
                    v = mv;
                }
                if (cfg.pose_throw_multiplier != 1.0 || measured)
                {
                    Vec3 nv = v * cfg.pose_throw_multiplier;
                    Call slv(base, STR("SetPhysicsLinearVelocity"));
                    slv.three(STR("NewVel"), nv.x, nv.y, nv.z);
                    slv.flag(STR("bAddToCurrent"), false);
                    slv.run();
                }
            }
            g_thrown.push_back(Thrown{held, base, now_seconds()});
            HSVR_LOG("Throw: %s hand throws %ls at %.1f m/s (hand %.1f m/s, x%.1f); Just Thrown %d, Last Parent %s", side == 0 ? "left" : "right",
                     held->GetName().c_str(), length(v) * cfg.pose_throw_multiplier / 100.0, g_hand_world_v[side], cfg.pose_throw_multiplier,
                     (int)bool_property(held, STR("Just Thrown")), object_property(held, STR("Last Parent")) == pawn ? "you" : "NOT you");
        }
    }

    void thrown_tick(UObject* pawn)
    {
        using namespace hsvr;
        double now = now_seconds();
        for (size_t i = 0; i < g_thrown.size();)
        {
            Thrown& t = g_thrown[i];
            bool done = !t.weapon || !t.base || now - t.t0 > 4.0;
            bool held = false;
            if (!done)
                for (const TCHAR* slot : {STR("Weapon L"), STR("Weapon R")}) held = held || object_property(pawn, slot) == t.weapon;
            double speed = 0.0;
            if (!done && !held)
            {
                Call lv(t.base, STR("GetPhysicsLinearVelocity"));
                lv.run();
                speed = length(lv.vec());
                if (now - t.t0 > 0.3 && speed < 150.0) done = true; // landed or stopped
                else if (bool_property(t.weapon, STR("Just Thrown")) && object_property(t.weapon, STR("Last Parent")) != pawn)
                    set_variable(t.weapon, STR("Last Parent"), 0.0, pawn, nullptr);
            }
            if (done || held)
            {
                if (t.weapon && !held && bool_property(t.weapon, STR("Just Thrown"))) set_bool_property(t.weapon, STR("Just Thrown"), false);
                if (t.weapon) HSVR_LOG("Throw: %ls %s after %.2f s", t.weapon->GetName().c_str(), held ? "caught" : "landed", now - t.t0);
                g_thrown.erase(g_thrown.begin() + i);
                continue;
            }
            ++i;
        }
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
    // Crouched (Matthew 2026-11-19: "it kicks stuff around"): every 0.25 s, the loose things and bodies
    // near his legs that are moving, how fast, and how near his nearest leg bone is. Logging only.
    void crouch_push_log(UObject* pawn, UObject* mesh)
    {
        using namespace hsvr;
        static double last = 0.0;
        double now = now_seconds();
        if (now - last < 0.25) return;
        last = now;
        static UObject* cdo = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetSystemLibrary"));
        if (!cdo || !pawn || !mesh) return;
        const char* legs[6] = {"thigh_l", "calf_l", "foot_l", "thigh_r", "calf_r", "foot_r"};
        Vec3 leg_at[6];
        Vec3 mid{0, 0, 0};
        for (int i = 0; i < 6; ++i)
        {
            leg_at[i] = socket_location(mesh, bone_fname(legs[i]));
            mid = mid + leg_at[i] * (1.0 / 6.0);
        }
        static uint8_t types[32];
        for (int i = 0; i < 32; ++i) types[i] = (uint8_t)i;
        Call ov(cdo, STR("SphereOverlapComponents"));
        FProperty* ot = ov.param(STR("ObjectTypes"));
        FProperty* out = ov.param(STR("OutComponents"));
        if (!ot || !out) return;
        ov.object(STR("WorldContextObject"), pawn);
        ov.three(STR("SpherePos"), mid.x, mid.y, mid.z);
        ov.number(STR("SphereRadius"), 70.0);
        RawArray* list = reinterpret_cast<RawArray*>(ov.buffer.data() + ot->GetOffset_Internal());
        list->Data = reinterpret_cast<void**>(types);
        list->Num = list->Max = 32;
        ov.run();
        RawArray* found = reinterpret_cast<RawArray*>(ov.buffer.data() + out->GetOffset_Internal());
        std::vector<UObject*> components;
        for (int32_t i = 0; i < found->Num && found->Data; ++i) components.push_back(reinterpret_cast<UObject*>(found->Data[i]));
        found->Data = nullptr;
        found->Num = found->Max = 0;
        list->Data = nullptr;
        list->Num = list->Max = 0;
        std::wstring moving;
        for (UObject* c : components)
        {
            if (!c) continue;
            Call owner_call(c, STR("GetOwner"));
            owner_call.run();
            UObject* owner = owner_call.object_result();
            if (!owner || owner == pawn) continue;
            if (owner == object_property(pawn, STR("Weapon L")) || owner == object_property(pawn, STR("Weapon R"))) continue;
            static const FName none_bone = FName(STR("None"), FNAME_Add);
            FName bone = none_bone;
            if (c->GetFunctionByNameInChain(STR("FindClosestBone_K2")))
            {
                Call fb(c, STR("FindClosestBone_K2"));
                fb.three(STR("TestLocation"), mid.x, mid.y, mid.z);
                fb.flag(STR("bRequirePhysicsAsset"), true);
                fb.run();
                if (FProperty* rp = fb.param(STR("ReturnValue"))) bone = *reinterpret_cast<FName*>(fb.buffer.data() + rp->GetOffset_Internal());
            }
            Call sim(c, STR("IsSimulatingPhysics"));
            sim.name(STR("BoneName"), bone);
            sim.run();
            FProperty* sp = sim.param(STR("ReturnValue"));
            if (!(sp && *(sim.buffer.data() + sp->GetOffset_Internal()))) continue;
            Call lv(c, STR("GetPhysicsLinearVelocity"));
            lv.name(STR("BoneName"), bone);
            lv.run();
            double speed = length(lv.vec()) / 100.0;
            if (speed < 0.5) continue;
            int near_i = -1;
            double near_d = 1e9;
            for (int i = 0; i < 6; ++i)
            {
                Call cp(c, STR("GetClosestPointOnCollision"));
                cp.three(STR("Point"), leg_at[i].x, leg_at[i].y, leg_at[i].z);
                cp.name(STR("BoneName"), bone);
                cp.run();
                double d = cp.number();
                if (d >= 0.0 && d < near_d)
                {
                    near_d = d;
                    near_i = i;
                }
            }
            wchar_t buf[256];
            std::swprintf(buf, 256, L"%ls.%ls:%ls %.1f m/s, %.0f cm from his %hs; ", owner->GetName().c_str(), c->GetName().c_str(), bone.ToString().c_str(), speed,
                          near_d, near_i >= 0 ? legs[near_i] : "?");
            moving += buf;
        }
        if (!moving.empty()) HSVR_LOG("Crouch: %.0f cm down, moving near his legs: %ls", g_crouch_drop, moving.c_str());
    }

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

    // ---- Handles (the grab rebuild, B&S-style) --------------------------------------------------
    // Every item's handles: segments in its BaseMesh frame a hand can hold and slide along. Weapons:
    // the grip (measured along the axis from the real parts: the haft of a polearm, a sword's grip
    // between guard and pommel) and the blade past the guard (half-swording). Shields: each measured
    // strap and handle. Anywhere else on an item, the hand holds it where it touched, with no slide.
    struct Handle
    {
        hsvr::Vec3 a, b;
        std::string name;
        int grip_r = -1, grip_l = -1; // finger shape for that hand (-1 = the game's own)
    };
    const std::vector<Handle>& item_handles(UObject* item, UObject* base);
    // Weapons and tools are taken only by a handle; shields and bottles anywhere.
    bool grab_needs_handle(UObject* item)
    {
        if (!item) return false;
        std::wstring cls = item->GetClassPrivate() ? item->GetClassPrivate()->GetName() : std::wstring();
        if (cls.rfind(STR("Shield"), 0) == 0) return false;
        if (cls.find(STR("Consumable")) != std::wstring::npos || cls.find(STR("Bottle")) != std::wstring::npos) return false;
        if (cls.find(STR("Fists")) != std::wstring::npos) return false;
        return true;
    }
    int nearest_handle(const std::vector<Handle>& hs, const hsvr::Vec3& local, double& t, double& dist);
    FreeGrip make_free_grip(int side, UObject* mesh, const hsvr::Vec3& w0_pos, const hsvr::Quat& w0_rot, const hsvr::Vec3& palm, const hsvr::Vec3& point,
                            const hsvr::Vec3* axis, const char* what);
    FreeGrip handle_grip(int side, UObject* mesh, const hsvr::Vec3& w0_pos, const hsvr::Quat& w0_rot, const hsvr::Vec3& palm, const Handle& hd, double t);
    int clamp_grip_type(int side, int g);
    double closest_on_weapon(UObject* weapon, const hsvr::Vec3& point, hsvr::Vec3* out = nullptr, UObject** out_part = nullptr);

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
        hsvr::Quat last_rot;       // last tick's two-hand target turn (diagnostics: how far it jumps per tick)
        bool have_last_rot = false;
        double z = 0.0;            // the second hand's spot along the weapon, cm from the game's grip point
        bool on_blade = false;
        hsvr::Vec3 local{0, 0, 0}; // that spot in BaseMesh's frame (set by the hold each tick)
        hsvr::Vec3 in_hand{0, 0, 0}; // the palm point in the second hand bone's frame (the first's, mirrored)
        bool have_local = false;
        double far_since = -1.0;
        bool sliding = false;
        double grip_type_before = -1.0; // the second hand's grip type before it took hold (its finger pose)
        int anim_grip = -1;              // the second hand's finger shape (handle system)
        // The handle system (pose_grab_handles): the second hand's own grip on the weapon.
        bool use_rel = false;
        hsvr::Quat rel;              // weapon rotation in the second hand's frame
        bool has_handle = false;
        hsvr::Vec3 ha, hb;
        double t = 0.0;
        std::string handle;
        double couple = 0.0; // the second hand's force x the hands' distance on the weapon (kg cm^2/s^2), this tick
        // B&S hold: the second hand's palm point (world), its velocity, and its spot on the weapon (BaseMesh frame).
        hsvr::Vec3 bs_p2, bs_vel, bs_local;
        bool bs_valid = false;
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
        if (g_two.weapon && g_arm_ignore[g_two.second].weapon == g_two.weapon) clear_arm_ignore(g_two.second);
        g_two = TwoHand{};
        g_anim_grip_two_side = -1;
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
        FreeGrip keep;
        if (g_two.use_rel)
        {
            // The handle system: the second hand keeps its own grip as it was.
            keep.set = true;
            keep.pivot = g_two.local;
            keep.in_anchor = g_two.in_hand;
            keep.rel = g_two.rel;
            keep.what = g_two.handle;
            keep.has_handle = g_two.has_handle;
            keep.ha = g_two.ha;
            keep.hb = g_two.hb;
            keep.t = g_two.t;
            keep.handle = g_two.handle;
            keep.grip_type = hsvr::config().pose_fingers_anim_only ? g_two.anim_grip
                             : g_two.grip_type_before >= 0.0 ? (int)read_number_variable(pawn, grip_type_name(to)) : -1;
            keep.hold_at = true;
            keep.at_pos = pos;
            keep.at_rot = rot;
        }
        g_two = TwoHand{};
        g_anim_grip_two_side = -1;
        drop_weapon(from, pawn);
        bool ok = give_hand_weapon(to, pawn, weapon);
        UObject* now = object_property(pawn, to == 0 ? STR("Weapon L") : STR("Weapon R"));
        if (now == weapon)
        {
            set_component_transform(base, pos, rot, true);
            if (keep.set)
            {
                g_pending_free[to] = keep;
                g_pending_item[to] = weapon;
            }
            else g_pending_slide[to] = z;
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
        if (!weapon || h.weapon != weapon || !h.have_last || is_fist(weapon) || h.forearm || h.free.set) return false; // the mod's free grip has no grip zones yet
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
        // A fist still in this hand (made by an earlier empty squeeze) would sit inside the handle and
        // shove it every tick (playtest 02:44: a fist was in the second hand in 3 of the two-hand holds;
        // the game's whoosh plays on that tick-to-tick shove, "frozen but swooshing").
        if (is_fist(object_property(pawn, side == 0 ? STR("Weapon L") : STR("Weapon R")))) remove_fist(side, pawn, "the hand takes a weapon in two hands");
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
        if (cfg.pose_fingers_anim_only)
        {
            g_anim_grip_two = clamp_grip_type(side, 2);
            g_anim_grip_two_side = side;
        }
        else if (cfg.pose_two_hand_curl)
        {
            g_two.grip_type_before = read_number_variable(pawn, grip_type_name(side));
            bool ok = set_variable(pawn, grip_type_name(side), 2.0);
            HSVR_LOG("Two hands: %s hand's grip type %.0f -> 2 (sword grip fingers)%s", side == 0 ? "left" : "right", g_two.grip_type_before, ok ? "" : ": NOT SET");
            if (!ok) g_two.grip_type_before = -1.0;
        }
        // The second hand's own forearm must not collide with the weapon either (playtest 10:35: two-handed,
        // the sword stood still while the hands moved up to 28 cm away, then caught up, over and over,
        // "contact miss" = "stopped" at 16-50 N: the left hand body sat on the grip. The game switches
        // that pair's collision off only for grips that list hand_l (self_collision.md).)
        if (UObject* m = object_property(pawn, STR("Mesh"))) ignore_arm_collision(pawn, m, weapon, object_property(weapon, STR("BaseMesh")), side);
        controllers_buzz(side, 0.4f);
        HSVR_LOG("Two hands: %s hand takes %ls on the %s at %.1f cm (squeezed at %.1f, %.1f cm off the axis; first hand at %.1f)", side == 0 ? "left" : "right",
                 weapon->GetName().c_str(), g_two.on_blade ? "blade" : "grip", z, want, radial, h.slide);
        return true;
    }

    // The handle system's second hand (B&S): the free hand squeezes on a weapon the other hand holds,
    // touching it (pose_grab_touch_cm): it holds the nearest handle (grip, blade, a strap) lined up
    // with its grip line, or the spot it touched. The weapon then lies along the line between the two
    // hands' points, the first hand sets its roll, and the hold is pose_two_hand_strength as strong.
    bool try_second_hand(int side, UObject* pawn, UObject* mesh, const hsvr::Config& cfg)
    {
        using namespace hsvr;
        if (!cfg.pose_two_hand) return false;
        int other = 1 - side;
        UObject* weapon = object_property(pawn, other == 0 ? STR("Weapon L") : STR("Weapon R"));
        HoldState& h = g_hold[other];
        if (!weapon || h.weapon != weapon || !h.have_last || is_fist(weapon) || h.forearm) return false;
        // Not a blade on its way into or out of a scabbard (playtest 02:44: the off hand reaching for
        // the scabbard took the sheathing sword's blade, and the two holds froze it).
        if (g_draw[other].active && g_draw[other].weapon == weapon) return false;
        UObject* base = object_property(weapon, STR("BaseMesh"));
        if (!base) return false;
        Vec3 at = hand_grab_point(mesh, side);
        double touch_d = closest_on_weapon(weapon, at);
        if (touch_d < 0.0 || touch_d > cfg.pose_grab_touch_cm)
        {
            HSVR_LOG("Two hands: %s grip squeezed %.0f cm from %ls (needs %.0f), not on it", side == 0 ? "left" : "right", touch_d, weapon->GetName().c_str(),
                     cfg.pose_grab_touch_cm);
            return false;
        }
        Vec3 bp = component_location(base);
        Quat bq = component_rotation(base);
        Vec3 local = bq.inverse().rotate(at - bp);
        const std::vector<Handle>& hs = item_handles(weapon, base);
        double t = 0.0, hd = 1e9;
        int hi = nearest_handle(hs, local, t, hd);
        bool on_handle = hi >= 0 && hd <= cfg.pose_grab_handle_cm;
        if (!on_handle && cfg.pose_grab_weapon_handles_only && grab_needs_handle(weapon))
        {
            HSVR_LOG("Two hands: %s grip squeezed on %ls off its handles (nearest %s %.1f cm away): not taken", side == 0 ? "left" : "right",
                     weapon->GetName().c_str(), hi >= 0 ? hs[hi].name.c_str() : "none", hd);
            return false;
        }
        Vec3 spot = local;
        if (!on_handle)
        {
            Vec3 touch;
            if (closest_on_weapon(weapon, at, &touch) >= 0.0) spot = bq.inverse().rotate(touch - bp);
        }
        // Not on the first hand's own spot.
        if (length(spot - h.free.pivot) < 2.0 * cfg.pose_slide_hand_half_cm && h.free.set)
        {
            HSVR_LOG("Two hands: %s grip squeezed on the first hand's spot of %ls", side == 0 ? "left" : "right", weapon->GetName().c_str());
            return false;
        }
        FreeGrip g = on_handle ? handle_grip(side, mesh, bp, bq, at, hs[hi], t)
                               : make_free_grip(side, mesh, bp, bq, at, bp + bq.rotate(spot), nullptr, "spot where the hand touched it");
        // A fist still in this hand (made by an earlier empty squeeze) would sit inside the handle and
        // shove it every tick (playtest 02:44: a fist was in the second hand in 3 of the two-hand holds;
        // the game's whoosh plays on that tick-to-tick shove, "frozen but swooshing").
        if (is_fist(object_property(pawn, side == 0 ? STR("Weapon L") : STR("Weapon R")))) remove_fist(side, pawn, "the hand takes a weapon in two hands");
        g_two = TwoHand{};
        g_two.active = true;
        g_two.main = other;
        g_two.second = side;
        g_two.weapon = weapon;
        g_two.use_rel = true;
        g_two.rel = g.rel;
        g_two.in_hand = g.in_anchor;
        g_two.local = g.pivot;
        g_two.have_local = true;
        g_two.has_handle = g.has_handle;
        g_two.ha = g.ha;
        g_two.hb = g.hb;
        g_two.t = g.t;
        g_two.handle = on_handle ? hs[hi].name : std::string("spot");
        g_two.on_blade = on_handle && hs[hi].name == "blade";
        int gt = on_handle ? clamp_grip_type(side, side == 1 ? hs[hi].grip_r : hs[hi].grip_l) : 5;
        if (gt < 0) gt = 2;
        g_two.anim_grip = gt;
        if (cfg.pose_fingers_anim_only)
        {
            g_anim_grip_two = gt;
            g_anim_grip_two_side = side;
        }
        else if (cfg.pose_two_hand_curl)
        {
            g_two.grip_type_before = read_number_variable(pawn, grip_type_name(side));
            if (!set_variable(pawn, grip_type_name(side), gt)) g_two.grip_type_before = -1.0;
        }
        // The second hand's own forearm must not collide with the weapon either (playtest 10:35: two-handed,
        // the sword stood still while the hands moved up to 28 cm away, then caught up, over and over,
        // "contact miss" = "stopped" at 16-50 N: the left hand body sat on the grip. The game switches
        // that pair's collision off only for grips that list hand_l (self_collision.md).)
        if (UObject* m = object_property(pawn, STR("Mesh"))) ignore_arm_collision(pawn, m, weapon, object_property(weapon, STR("BaseMesh")), side);
        controllers_buzz(side, 0.4f);
        HSVR_LOG("Two hands: %s hand takes %ls by its %s (%.1f cm from the handle, touched at %.1f cm), at (%.1f, %.1f, %.1f) on it, fingers %d",
                 side == 0 ? "left" : "right", weapon->GetName().c_str(), g_two.handle.c_str(), on_handle ? hd : 0.0, touch_d, spot.x, spot.y, spot.z, gt);
        return true;
    }

    // The second hand's arm: its palm on its spot on the weapon, and the hand locked onto the weapon
    // the way the first hand holds it, mirrored (playtest 2026-11-18: following the controller's turn,
    // it twisted and clipped through the handle). Willie's hands mirror across the hand bone's X and
    // the weapon's Y (kick test 15: a weapon in the left hand has X and Z mirrored in the hand frame,
    // so Y flips), so the second hand's weapon frame is M_hand * R_first * M_weapon.
    // Where an item will be once this frame's physics has moved it (pos/rot in: as it is now).
    // pose_hold_hand_follows_predict 1 (default): from how the item actually moved between the last two
    // frames, at most pose_hold_follow_predict_max_cm / 30 deg ahead. 2: from the velocity the physics
    // reports now (the old way): that is the velocity the hold asked for, so when something blocks
    // the item (or Chaos pushes it out of a contact) it can be metres a second off what the item
    // does, and the drawn hand jumped up to 43 cm for a frame (playtest 2026-11-19 10:04, item still).
    // 0: no prediction (the hand a frame behind the item).
    struct ItemTrack
    {
        hsvr::Vec3 pos, prev_pos;
        hsvr::Quat rot, prev_rot;
        double t = 0.0, prev_t = 0.0;
        bool have = false, have_prev = false;
        hsvr::Vec3 v, w;        // smoothed measured velocity (cm/s) and turn rate (rad/s)
        bool have_v = false;
    };
    std::unordered_map<UObject*, ItemTrack> g_item_track;
    double world_delta_seconds(UObject* context);
    void predicted_item_pose(UObject* base, hsvr::Vec3& pos, hsvr::Quat& rot)
    {
        using namespace hsvr;
        const Config& cfg = hsvr::config();
        int mode = cfg.pose_hold_hand_follows_predict;
        if (mode <= 0 || !base) return;
        if (g_pose_post_physics.load()) return; // the item is already where it is drawn this frame
        double DT = world_delta_seconds(g_pose.pawn);
        if (!(DT > 1e-4 && DT < 0.1)) DT = 1.0 / 90.0;
        if (mode >= 2)
        {
            Call cm(base, STR("GetCenterOfMass"));
            cm.run();
            Call lv(base, STR("GetPhysicsLinearVelocity"));
            lv.run();
            Call av(base, STR("GetPhysicsAngularVelocityInRadians"));
            av.run();
            Vec3 com = cm.vec(), v = lv.vec(), w = av.vec();
            Quat dq = from_scaled_axis(w * DT);
            Vec3 com2 = com + v * DT + Vec3{0.0, 0.0, -980.0 * DT * DT};
            pos = com2 + dq.rotate(pos - com);
            rot = (dq * rot).normalized();
            return;
        }
        // A new frame when the item has moved or 3 ms have passed (several calls a frame read the same pose).
        if (g_item_track.size() > 64) g_item_track.clear();
        ItemTrack& k = g_item_track[base];
        double now = now_seconds();
        bool moved = !k.have || length(pos - k.pos) > 1e-4 || length(scaled_axis((rot * k.rot.inverse()).normalized())) > 1e-6;
        if (!k.have || (moved && now - k.t > 0.003) || now - k.t > 0.1)
        {
            k.prev_pos = k.pos;
            k.prev_rot = k.rot;
            k.prev_t = k.t;
            k.have_prev = k.have;
            k.pos = pos;
            k.rot = rot;
            k.t = now;
            k.have = true;
        }
        double gap = k.t - k.prev_t;
        if (!k.have_prev || !(gap > 0.002 && gap < 0.05)) return;
        // Its velocity from the last two frames, smoothed over pose_hold_follow_predict_smooth_s (once per
        // new frame): straight frame-to-frame extrapolation tripled the item's own frame-to-frame jerk in
        // the hand (playtest 10:36: hand target 2nd difference p50 7.9 cm against the grip's 2.7 on the
        // worst ticks).
        if (k.t != k.prev_t && now == k.t)
        {
            Vec3 v_now = (k.pos - k.prev_pos) * (1.0 / gap);
            Vec3 w_now = scaled_axis((k.rot * k.prev_rot.inverse()).normalized()) * (1.0 / gap);
            double a = cfg.pose_hold_follow_predict_smooth_s > 1e-4 ? 1.0 - std::exp(-gap / cfg.pose_hold_follow_predict_smooth_s) : 1.0;
            if (!k.have_v) k.v = v_now, k.w = w_now, k.have_v = true;
            else k.v = k.v + (v_now - k.v) * a, k.w = k.w + (w_now - k.w) * a;
        }
        if (!k.have_v) return;
        Vec3 step = k.v * DT;
        Vec3 turn = k.w * DT;
        step = clamp_length(step, (std::max)(0.0, cfg.pose_hold_follow_predict_max_cm));
        turn = clamp_length(turn, 30.0 / 57.2958);
        Quat dq = from_scaled_axis(turn);
        // The item's origin moves on by `step` and the item turns about it by `turn`.
        pos = pos + step;
        rot = (dq * rot).normalized();
    }

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
            Vec3 wp = component_location(b);
            if (hsvr::config().pose_hold_hand_follows) predicted_item_pose(b, wp, wr);
            hand_rot = (wr * (h.f2 * h.f1.inverse()).normalized().inverse()).normalized();
            target = wp + wr.rotate(h.pivot) - hand_rot.rotate(h.in_hand);
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
        Vec3 weapon_pos = component_location(base);
        if (hsvr::config().pose_hold_hand_follows) predicted_item_pose(base, weapon_pos, weapon_rot);
        const HoldState& first = g_hold[g_two.main];
        if (g_two.use_rel)
            hand_rot = (weapon_rot * g_two.rel.inverse()).normalized();
        else if (hsvr::config().pose_two_hand_lock_rotation)
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
        Vec3 spot = weapon_pos + weapon_rot.rotate(g_two.local);
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
        // Playtest 02:44: after an opened fist the hand still carried a 0.05 kg weapon for minutes (and
        // the second hand of a two-hand hold later sat it inside the handle). Make sure it is gone.
        UObject* after = object_property(pawn, side == 0 ? STR("Weapon L") : STR("Weapon R"));
        bool still = after == mine;
        {
            // Same tick as the set-up, so the actor is still in memory even if it was destroyed
            // (destroying a destroyed actor does nothing).
            Call kill(mine, STR("K2_DestroyActor"));
            kill.run();
        }
        if (still) set_variable(pawn, side == 0 ? STR("Weapon L") : STR("Weapon R"), 0.0, nullptr, nullptr);
        HSVR_LOG("Fist: %s hand opens (%s); hand now holds %ls%s", side == 0 ? "left" : "right", why, after ? after->GetName().c_str() : STR("nothing"),
                 still ? " (the set-up left the fist: destroyed and cleared by the mod)" : "");
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
        static bool logged_pawn_tracked = track_freed(&logged_pawn);
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

    double closest_on_weapon(UObject* weapon, const hsvr::Vec3& point, hsvr::Vec3* out, UObject** out_part)
    {
        if (!weapon) return -1.0;
        double now = hsvr::now_seconds();
        // A weapon spawned straight into the hand (free play) is not in a list made before it existed:
        // rebuild at once for it (at most every 0.05 s), not only every 0.25 s (playtest 12:19: a fresh
        // two-handed axe measured a zero-length handle and kept it).
        bool unknown = g_weapon_parts.find(weapon) == g_weapon_parts.end();
        if (now - g_weapon_parts_time > 0.25 || (unknown && now - g_weapon_parts_time > 0.05))
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
        // The weapon's scabbard is one of its own components too (NODE_AddStaticMeshComponent-N, its
        // "Scabbard"), and stays on the belt after a draw: it is not the weapon (playtest 09:38: the
        // off hand on the belt scabbard took the drawn sword "by its spot" 55-92 cm away from it).
        UObject* scab = object_property(weapon, STR("Scabbard"));
        auto under_scabbard = [&](UObject* c) {
            if (!scab) return false;
            UObject* p = c;
            for (int i = 0; i < 6 && p; ++i)
            {
                if (p == scab) return true;
                Call ap(p, STR("GetAttachParent"));
                if (!ap.fn || !ap.run()) return false;
                p = ap.object_result();
            }
            return false;
        };
        for (UObject* c : it->second)
        {
            if (under_scabbard(c)) continue;
            Call q(c, STR("GetClosestPointOnCollision"));
            if (!q.fn) continue;
            q.three(STR("Point"), point.x, point.y, point.z);
            q.run();
            double d = q.number();
            if (d < 0.0 || (best >= 0.0 && d >= best)) continue;
            best = d;
            if (out) *out = q.vec(STR("OutPointOnBody"));
            if (out_part) *out_part = c;
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

    // The hand's grip line in the hand bone's frame (right: the game's sword grip, measured; left mirrored across X).
    hsvr::Vec3 hand_grip_axis(int side) { return hsvr::normalized(hsvr::Vec3{side == 0 ? 0.50 : -0.50, 0.86, 0.11}); }

    // The mod's free grip: held in the hand (the hand bone) by `point` (world, on the weapon), turned
    // as it was in pose (w0), or, given `axis` (world, a strap's or handle's line), turned the least
    // that lays that line along the hand's grip line.
    FreeGrip make_free_grip(int side, UObject* mesh, const hsvr::Vec3& w0_pos, const hsvr::Quat& w0_rot, const hsvr::Vec3& palm, const hsvr::Vec3& point,
                            const hsvr::Vec3* axis, const char* what)
    {
        using namespace hsvr;
        Vec3 a_pos = socket_location(mesh, bone_fname(side == 0 ? "hand_l" : "hand_r"));
        Quat a_rot = socket_rotation(mesh, bone_fname(side == 0 ? "hand_l" : "hand_r"));
        FreeGrip g;
        g.set = true;
        g.place = true;
        g.pivot = w0_rot.inverse().rotate(point - w0_pos);
        g.in_anchor = a_rot.inverse().rotate(palm - a_pos);
        g.rel = (a_rot.inverse() * w0_rot).normalized();
        if (axis)
        {
            Vec3 f = normalized(a_rot.inverse().rotate(*axis)), want = hand_grip_axis(side);
            if (dot(f, want) < 0.0) want = want * -1.0;
            Vec3 n = cross(f, want);
            double sn = length(n), c = dot(f, want);
            if (sn > 1e-6)
            {
                Quat turn = quat_axis_angle(n * (1.0 / sn), std::atan2(sn, c));
                g.rel = (turn * g.rel).normalized();
                HSVR_LOG("Grab: %s hand turns the %s %.0f deg to lie along the hand's grip line", side == 0 ? "left" : "right", what, std::atan2(sn, c) * 180.0 / 3.14159265358979);
            }
        }
        g.what = what;
        return g;
    }

    // The same, for a weapon just set up in that hand: applied when its hold starts.
    void hold_where_touched(int side, UObject* mesh, UObject* weapon, UObject* base, const hsvr::Vec3& w0_pos, const hsvr::Quat& w0_rot, const hsvr::Vec3& palm,
                            const hsvr::Vec3& point, const char* what, const hsvr::Vec3* axis = nullptr, bool fingers = false)
    {
        using namespace hsvr;
        g_pending_free[side] = make_free_grip(side, mesh, w0_pos, w0_rot, palm, point, axis, what);
        g_pending_item[side] = weapon;
        g_pending_free[side].hold_at = hsvr::config().pose_grab_settle_s > 0.0;
        g_pending_free[side].at_pos = w0_pos;
        g_pending_free[side].at_rot = w0_rot;
        g_pending_free[side].place = !g_pending_free[side].hold_at;
        g_pending_free[side].fingers = fingers;
        set_component_transform(base, w0_pos, w0_rot, true);
        HSVR_LOG("Grab: %s hand took %ls by the %s (%.1f cm from the hand), held in the hand there", side == 0 ? "left" : "right", weapon->GetName().c_str(), what,
                 length(point - palm));
    }

    // The fitting of a shield nearest a point (shield BaseMesh frame): its name, the point on it and
    // its line. `within` limits the distance (cm); game_only takes only the game's own for that hand.
    const ShieldFitting* nearest_fitting(const std::wstring& cls, const hsvr::Vec3& local, double within, int side, bool game_only, hsvr::Vec3& on, double& dist,
                                         int* known = nullptr);

    const ShieldFitting* nearest_fitting(const std::wstring& cls, const hsvr::Vec3& local, double within, int side, bool game_only, hsvr::Vec3& on, double& dist,
                                         int* known)
    {
        using namespace hsvr;
        const ShieldFitting* best = nullptr;
        dist = within;
        if (known) *known = 0;
        for (const ShieldFitting& f : k_shield_fittings)
        {
            if (cls != f.shield) continue;
            if (known) ++*known;
            if (game_only && !(side == 1 ? f.game_r : f.game_l)) continue;
            Vec3 c{f.cx, f.cy, f.cz}, a = normalized(Vec3{f.ax, f.ay, f.az});
            double t = (std::max)(-f.length * 0.5, (std::min)(f.length * 0.5, dot(local - c, a)));
            Vec3 p = c + a * t;
            double d = length(local - p);
            if (d <= dist)
            {
                dist = d;
                best = &f;
                on = p;
            }
        }
        return best;
    }

    std::wstring shield_class(UObject* shield)
    {
        std::wstring cls = shield && shield->GetClassPrivate() ? shield->GetClassPrivate()->GetName() : std::wstring();
        if (cls.size() > 2 && cls.substr(cls.size() - 2) == STR("_C")) cls.resize(cls.size() - 2);
        return cls;
    }

    // A shield taken with the grip button. The hand point, in the shield's frame from before the
    // game's set-up moves it, is matched against its fittings: the nearest within
    // pose_shield_point_cm wins. One of the game's own for that hand keeps the game's grip (bucklers
    // and the heater's boss grip are held in the hand by it; a strap shield the game puts on the
    // forearm is moved into the hand when its hold starts, pose_shield_in_hand). Any other fitting is
    // held in the hand, its line along the hand's grip line, and anywhere else on the shield (the
    // rim, an edge, the face) the hand holds it where it touched, turned as it was. The hold places
    // it there on its first tick (the game's set-up snaps it to its own grip a tick later).
    void shield_grip(int side, UObject* mesh, UObject* shield, UObject* base, const hsvr::Vec3& w0_pos, const hsvr::Quat& w0_rot, const hsvr::Vec3& palm,
                     const hsvr::Vec3& touch, const hsvr::Config& cfg)
    {
        using namespace hsvr;
        const char* hand = side == 0 ? "left" : "right";
        std::wstring cls = shield_class(shield);
        Vec3 local = w0_rot.inverse().rotate(palm - w0_pos);
        Vec3 best_on;
        double best_d = 0.0;
        int known = 0;
        const ShieldFitting* best = nearest_fitting(cls, local, cfg.pose_shield_point_cm, side, false, best_on, best_d, &known);
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
        if (best)
        {
            Vec3 axis = w0_rot.rotate(normalized(Vec3{best->ax, best->ay, best->az}));
            hold_where_touched(side, mesh, shield, base, w0_pos, w0_rot, palm, w0_pos + w0_rot.rotate(best_on), best->name, &axis, true);
        }
        else
            hold_where_touched(side, mesh, shield, base, w0_pos, w0_rot, palm, touch, "edge where the hand touched it", nullptr, true);
    }

    // A strap shield the game holds on the forearm, moved into the hand (pose_shield_in_hand): by the
    // fitting nearest the hand, the game's own for that hand first (within 15 cm), else any.
    bool shield_into_hand(int side, UObject* pawn, UObject* shield, UObject* base, FreeGrip& out)
    {
        using namespace hsvr;
        UObject* mesh = object_property(pawn, STR("Mesh"));
        if (!mesh || !base) return false;
        std::wstring cls = shield_class(shield);
        Vec3 palm = hand_grab_point(mesh, side);
        Vec3 w0_pos = component_location(base);
        Quat w0_rot = component_rotation(base);
        Vec3 local = w0_rot.inverse().rotate(palm - w0_pos), on;
        double d = 0.0;
        const ShieldFitting* f = nearest_fitting(cls, local, 15.0, side, true, on, d);
        if (!f) f = nearest_fitting(cls, local, 1e9, side, false, on, d);
        if (!f)
        {
            HSVR_LOG("Shield: %ls has no measured fittings: left on the forearm", cls.c_str());
            return false;
        }
        Vec3 axis = w0_rot.rotate(normalized(Vec3{f->ax, f->ay, f->az}));
        out = make_free_grip(side, mesh, w0_pos, w0_rot, palm, w0_pos + w0_rot.rotate(on), &axis, f->name);
        out.fingers = true;
        HSVR_LOG("Shield: %s hand holds %ls in the hand by its %s (was on the forearm; the hand was %.1f cm from it)", side == 0 ? "left" : "right",
                 shield->GetName().c_str(), f->name, d);
        return true;
    }

    // ---- Body grabs (a Willie's throat, arm, ...) ---------------------------------------------------
    // The game's latch joins hand_r to the victim's bone with 'PhysicsConstraint R Hand' (linear
    // limited, reeled in over 0.2 s; grab_pickup.md). Its grip-slip (UG @360300) lets go when the
    // joint's force beats his grip strength; with his hand posed from your controller (kinematic),
    // the pull has no limit. log_body_grab writes what the game sees every 0.1 s, to measure it.
    // pose_grab_body_hold 1: after 0.25 s the joint's linear part becomes a capped spring (as Matthew's
    // framework, protocol/grab.rs: the body's weight holds the hand back), and the hand pulled
    // pose_grab_body_let_go_cm past where it took hold for 0.3 s lets go (B&S).
    struct BodyGrab
    {
        bool on = false;
        double since = -1.0, rest = 0.0, far_since = -1.0, last_log = -10.0;
        UObject* comp = nullptr;
        UObject* victim = nullptr;
        FName bone;
        bool softened = false;
        double force = 0.0, torque = 0.0, release_rate = 0.0, stretch = 0.0, strangle = 0.0, choke = 0.0, conscious = 0.0;
        double grip_t = 121.0; // his grip strength T as the game's slip rule computes it
    };
    BodyGrab g_body_grab[2];
    std::atomic<int> g_body_hold_test{-99}; // test: hsvr.body_hold <n> (-99 = pose_grab_body_hold)

    // The spring's cap: pose_grab_body_cap_per_t x his grip strength T, so the joint's force stays under
    // T (the game's slip threshold) and his strength, injuries, pain and consciousness set how hard he
    // can pull a body (test 4, 2026-11-19: the force levels off at about cap/500). 0 = pose_grab_body_max.
    double body_spring_cap(const hsvr::Config& cfg, double grip_t)
    {
        return cfg.pose_grab_body_cap_per_t > 0.0 ? cfg.pose_grab_body_cap_per_t * (std::max)(1.0, grip_t) : cfg.pose_grab_body_max;
    }
    void body_spring_joint(UObject* joint, const hsvr::Config& cfg, double cap)
    {
        for (const TCHAR* fn : {STR("SetLinearXLimit"), STR("SetLinearYLimit"), STR("SetLinearZLimit")})
        {
            Call c1(joint, fn);
            c1.byte(STR("ConstraintType"), 0); // free: the spring holds it
            c1.number(STR("LimitSize"), 0.0);
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
        target.three(STR("InPosTarget"), 0.0, 0.0, 0.0);
        target.run();
        Call vtarget(joint, STR("SetLinearVelocityTarget"));
        vtarget.three(STR("InVelTarget"), 0.0, 0.0, 0.0);
        vtarget.run();
        Call lin(joint, STR("SetLinearDriveParams"));
        lin.number(STR("PositionStrength"), cfg.pose_grab_body_spring);
        lin.number(STR("VelocityStrength"), cfg.pose_grab_body_damper);
        lin.number(STR("InForceLimit"), cap);
        lin.run();
    }

    // Willie's tick re-shapes his hand joints every tick (as for weapons, @454086), so a softened body
    // grab's spring is written again right after it.
    void body_grab_after_tick(UObject* pawn)
    {
        hsvr::Config cfg = hsvr::config();
        int hold = g_body_hold_test.load() != -99 ? g_body_hold_test.load() : cfg.pose_grab_body_hold;
        if (hold != 1) return;
        for (int side = 0; side < 2; ++side)
            if (g_body_grab[side].on && g_body_grab[side].softened)
                if (UObject* joint = object_property(pawn, side == 0 ? STR("PhysicsConstraint L Hand") : STR("PhysicsConstraint R Hand")))
                    body_spring_joint(joint, cfg, body_spring_cap(cfg, g_body_grab[side].grip_t));
    }

    // Limb strikes (elbows, forearms, open hands): the struck Willie's body-hit block (UG 791446, from
    // his armour proxies' OnComponentHit -> NewFunction) damages himself (impulse capped at 1500 while
    // standing, x the hitter's surface rigidity). Test 9: the posed arm's contacts do reach it.
    // log_body_hits: every other Willie's 'Last Hit Impulse (Other Body)', Health and
    // Consciousness, logged when they change, with what hit him ('Other Comp').
    void body_hit_watch(UObject* pawn)
    {
        if (!hsvr::config().log_body_hits) return;
        static std::vector<UObject*> foes;
        static double found_at = -10.0;
        struct Seen { double impulse = -1.0, health = -1.0, conscious = -1.0; };
        static std::map<UObject*, Seen> seen;
        // Willies the game frees (a removed corpse) leave both lists at once (gc_sync); a new list
        // is also taken after every garbage collection (crash 2026-11-20: a freed Willie was used).
        static bool tracked = on_freed([] {
            erase_gone(foes);
            erase_gone_keys(seen);
        });
        static uint64_t epoch = 0;
        double now = hsvr::now_seconds();
        if (now - found_at > 1.0 || epoch != g_gc_epoch)
        {
            found_at = now;
            epoch = g_gc_epoch;
            foes.clear();
            std::vector<UObject*> all;
            UObjectGlobals::FindAllOf(STR("Willie_BP_C"), all);
            for (UObject* w : all)
                if (w && w != pawn && w->GetName().rfind(STR("Default__"), 0) != 0 && object_alive(w)) foes.push_back(w);
            std::map<UObject*, Seen> keep;
            for (UObject* w : foes) keep[w] = seen[w];
            seen.swap(keep);
        }
        for (UObject* w : foes)
        {
            if (!object_alive(w)) continue; // destroyed this second (still readable, not a fighter)
            Seen& was = seen[w];
            // Willie-on-Willie contacts go through branch H of the block, which writes 'Last Hit Impulse
            // (Other Body)' (reset after 1 s), not 'Last Hit Impulse' (ground hits only).
            double impulse = read_number_variable(w, STR("Last Hit Impulse (Other Body)"));
            double health = read_number_variable(w, STR("Health"));
            double conscious = read_number_variable(w, STR("Consciousness"));
            bool hit = was.impulse >= 0.0 && impulse != was.impulse && impulse > 0.0;
            bool hurt = was.health >= 0.0 && health < was.health - 0.01;
            if (hit || hurt)
            {
                UObject* other = object_property(w, STR("Other Comp"));
                UObject* other_owner = nullptr;
                if (other)
                {
                    Call oc(other, STR("GetOwner"));
                    oc.run();
                    other_owner = oc.object_result();
                }
                HSVR_LOG("Body hit: %ls last hit impulse %.0f, health %.1f -> %.1f, consciousness %.0f -> %.0f, hit by limbs %d, by %ls.%ls%s", w->GetName().c_str(), impulse,
                         was.health, health, was.conscious, conscious, (int)bool_property(w, STR("Beig Hit By Limbs")), other_owner ? other_owner->GetName().c_str() : STR("?"),
                         other ? other->GetName().c_str() : STR("?"), other_owner == pawn ? " (you)" : "");
            }
            was.impulse = impulse;
            was.health = health;
            was.conscious = conscious;
        }
    }

    // ---- Elbows hit like a punch (Matthew 2026-11-20) -------------------------------------------
    // Flat Half Sword has no elbow attack: a limb touching a Willie only runs his body-hit block,
    // whose self-damage is weak (rigidity 0.225-0.5 x a capped impulse). A punch instead is the fist
    // weapon calling his 'Deal Complex Damage' (BPI_ComplexCollision, ModularWeaponBP UG @41944) with
    // the fist's velocity, impulse, rigidity and cutting rate. So when the elbow (the lowerarm bone's
    // root, and 7 cm either side along the arm) meets a Willie's body at fist speed (Aim Swing Speed
    // 30, the speed the game's fist needs), the mod makes that same call with the elbow's velocity
    // and the punch's own numbers: the running mean of your last real fist hits (watched on the
    // way in), or pose_elbow_* until you have punched.
    struct PunchStats
    {
        int n = 0;
        double rigidity = 0, cut = 0, stab = 0, impulse_per_v = 0, v_per_speed = 0;
        bool have_template = false;
        double kick = 1.0, blunt_int = 1.0, draw_cut = 0.0;
        bool lower_threshold = false, damage_parent = false, extra_high = false;
    };
    PunchStats g_punch;

    double read_param_number(uint8_t* base, FProperty* p)
    {
        auto kind = p->GetClass().GetName();
        uint8_t* at = base + p->GetOffset_Internal();
        if (kind == STR("DoubleProperty")) return *reinterpret_cast<double*>(at);
        if (kind == STR("FloatProperty")) return *reinterpret_cast<float*>(at);
        if (kind == STR("IntProperty")) return *reinterpret_cast<int32_t*>(at);
        if (kind == STR("ByteProperty")) return *at;
        return 0.0;
    }
    void write_param_number(uint8_t* base, FProperty* p, double v)
    {
        auto kind = p->GetClass().GetName();
        uint8_t* at = base + p->GetOffset_Internal();
        if (kind == STR("DoubleProperty")) *reinterpret_cast<double*>(at) = v;
        else if (kind == STR("FloatProperty")) *reinterpret_cast<float*>(at) = (float)v;
        else if (kind == STR("IntProperty")) *reinterpret_cast<int32_t*>(at) = (int32_t)v;
        else if (kind == STR("ByteProperty")) *at = (uint8_t)v;
    }
    bool read_param_bool(uint8_t* base, FProperty* p)
    {
        auto* b = static_cast<FBoolProperty*>(p);
        return (*(base + p->GetOffset_Internal() + b->GetByteOffset()) & b->GetFieldMask()) != 0;
    }
    void write_param_bool(uint8_t* base, FProperty* p, bool v)
    {
        auto* b = static_cast<FBoolProperty*>(p);
        uint8_t* at = base + p->GetOffset_Internal() + b->GetByteOffset();
        if (v) *at |= b->GetFieldMask();
        else *at &= ~b->GetFieldMask();
    }
    hsvr::Vec3 read_param_vec(uint8_t* base, FProperty* p)
    {
        double v[3];
        read_three(base + p->GetOffset_Internal(), p, v);
        return {v[0], v[1], v[2]};
    }

    std::atomic<bool> g_elbow_calling{false};
    UObject* g_punch_victim = nullptr; // the blow being watched (punch_watch_post logs what it did)
    double g_punch_c0 = 0.0, g_punch_h0 = 0.0;
    // Watches every 'Deal Complex Damage' a Willie gets: the player's fist hits are learnt (and logged).
    void punch_watch_hook(Unreal::UnrealScriptFunctionCallableContext& context, void* custom_data)
    {
        gc_sync();
        HSVR_PROF(PerfPunchPre);
        if (g_elbow_calling.load()) return;
        UFunction* fn = static_cast<UFunction*>(custom_data);
        uint8_t* locals = context.TheStack.Locals();
        UObject* victim = context.Context;
        UObject* pawn = live_posed_pawn();
        if (!pawn) find_pawn_mesh(&pawn); // flat mode: the player's pawn, unposed
        if (!fn || !locals || !victim || !pawn || victim == pawn) return;
        FProperty *normal_p = nullptr, *collided = nullptr, *vel = nullptr, *imp = nullptr, *cut = nullptr, *stab = nullptr, *rig = nullptr, *kick = nullptr, *blunt = nullptr,
                  *lower = nullptr, *parent = nullptr, *extra = nullptr, *draw = nullptr, *bone = nullptr;
        for (FProperty* p : fn->ForEachProperty())
        {
            std::wstring n = p->GetName();
            if (n == STR("Collided Component")) collided = p;
            else if (n == STR("Hit Velocity")) vel = p;
            else if (n == STR("Hit Impulse")) imp = p;
            else if (n == STR("Cutting Power")) cut = p;
            else if (n == STR("Stab Rate")) stab = p;
            else if (n == STR("Rigidity")) rig = p;
            else if (n == STR("Kick Power")) kick = p;
            else if (n == STR("Blunt Destruction Int")) blunt = p;
            else if (n == STR("Lower Threshold In")) lower = p;
            else if (n == STR("Damage Parent Bone?")) parent = p;
            else if (n == STR("Extra High Velocity")) extra = p;
            else if (n == STR("Draw Cut")) draw = p;
            else if (n == STR("Hit Bone")) bone = p;
            else if (n == STR("Normal")) normal_p = p;
        }
        if (!collided || !vel || !imp || !rig) return;
        UObject* comp = *reinterpret_cast<UObject**>(locals + collided->GetOffset_Internal());
        if (!comp) return;
        Call oc(comp, STR("GetOwner"));
        oc.run();
        UObject* owner = oc.object_result();
        int side = owner && owner == object_property(pawn, STR("Weapon R")) ? 1 : owner && owner == object_property(pawn, STR("Weapon L")) ? 0 : -1;
        if (side < 0 || !is_fist(owner)) return;
        using namespace hsvr;
        Vec3 v = read_param_vec(locals, vel), ni = read_param_vec(locals, imp);
        double speed = length(v), r = read_param_number(locals, rig), c = cut ? read_param_number(locals, cut) : 0.0, s = stab ? read_param_number(locals, stab) : 0.0;
        // The hand's own speed when it hit (to learn the fist's velocity factor).
        double hand_speed = g_swing_v[side] * 100.0;
        // Learnt only from blows at fist speed (test 12: each blow also logs 3-7 slow touches at
        // 1-11 cm/s, which made impulse per velocity 56x too big after three of them).
        Config cfg = config();
        double lo = cfg.pose_swing_speed_lo, hi = (std::max)(lo + 0.1, cfg.pose_swing_speed_hi);
        bool blow = speed >= (lo + (hi - lo) * cfg.pose_elbow_min_ass / 75.0) * 100.0;
        PunchStats& P = g_punch;
        int k = (std::min)(P.n, 19);
        auto mean = [&](double& m, double x) { m = (m * k + x) / (k + 1); };
        if (blow)
        {
            mean(P.rigidity, r);
            mean(P.cut, c);
            mean(P.stab, s);
            mean(P.impulse_per_v, length(ni) / speed);
            if (hand_speed > 50.0) mean(P.v_per_speed, speed / hand_speed);
            P.n += 1;
        }
        if (kick) P.kick = read_param_number(locals, kick);
        if (blunt) P.blunt_int = read_param_number(locals, blunt);
        if (draw) P.draw_cut = read_param_number(locals, draw);
        if (lower) P.lower_threshold = read_param_bool(locals, lower);
        if (parent) P.damage_parent = read_param_bool(locals, parent);
        if (extra) P.extra_high = read_param_bool(locals, extra);
        P.have_template = true;
        Vec3 nrm = normal_p ? read_param_vec(locals, normal_p) : Vec3{};
        double cos_ni = speed > 1.0 && length(ni) > 1e-6 ? dot(v, ni) / (speed * length(ni)) : 0.0;
        double cos_n = speed > 1.0 && length(nrm) > 1e-6 ? dot(v, nrm) / (speed * length(nrm)) : 0.0;
        g_punch_victim = victim;
        g_punch_c0 = read_number_variable(victim, STR("Consciousness"));
        g_punch_h0 = read_number_variable(victim, STR("Health"));
        std::wstring b = bone ? reinterpret_cast<FName*>(locals + bone->GetOffset_Internal())->ToString() : STR("?");
        HSVR_LOG("Punch: %s fist %s %ls (%ls): velocity %.0f cm/s (hand %.0f), impulse %.0f, rigidity %.2f, cut %.2f, stab %.2f, kick %.2f, blunt %.0f, draw %.2f, "
                 "lower %d, parent %d, extra %d, cos(velocity, impulse) %.2f, cos(velocity, normal) %.2f; learnt from %d hits: rigidity %.2f, cut %.2f, impulse/velocity %.3f, velocity/hand %.2f",
                 side == 0 ? "left" : "right", blow ? "blow on" : "touch on", victim->GetName().c_str(), b.c_str(), speed, hand_speed, length(ni), r, c, s, P.kick, P.blunt_int, P.draw_cut,
                 (int)P.lower_threshold, (int)P.damage_parent, (int)P.extra_high, cos_ni, cos_n, P.n, P.rigidity, P.cut, P.impulse_per_v, P.v_per_speed);
    }

    // After his Deal Complex Damage: what the blow (or touch) did to him.
    void punch_watch_post(Unreal::UnrealScriptFunctionCallableContext& context, void*)
    {
        gc_sync();
        HSVR_PROF(PerfPunchPost);
        if (!g_punch_victim || context.Context != g_punch_victim) return;
        UObject* w = g_punch_victim;
        g_punch_victim = nullptr;
        HSVR_LOG("Punch: -> %ls health %.2f -> %.2f, consciousness %.2f -> %.2f", w->GetName().c_str(), g_punch_h0, read_number_variable(w, STR("Health")), g_punch_c0,
                 read_number_variable(w, STR("Consciousness")));
    }

    void install_punch_watch()
    {
        static bool installed = false;
        static double last_try = -10.0;
        if (installed) return;
        double now = hsvr::now_seconds();
        if (now - last_try < 2.0) return;
        last_try = now;
        UFunction* fn = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Game/Character/Blueprints/Willie_BP.Willie_BP_C:Deal Complex Damage"));
        if (!fn) return;
        installed = true;
        UObjectGlobals::RegisterHook(fn, &punch_watch_hook, &punch_watch_post, fn);
        HSVR_LOG("Punch: watching Willie's Deal Complex Damage");
    }

    struct ElbowState
    {
        bool have = false;
        hsvr::Vec3 last[3];
        double last_t = 0.0;
        hsvr::Vec3 vel[3];
        std::map<UObject*, double> touching_until; // a hit needs the elbow to leave him first
        double last_hit = -10.0;
    };
    ElbowState g_elbow[2];

    void elbow_tick(UObject* pawn)
    {
        using namespace hsvr;
        Config cfg = config();
        install_punch_watch();
        if (!cfg.pose_elbow_strikes || !pawn || !g_pose.mesh || !g_pose_active.load()) return;
        static std::vector<UObject*> foes;
        static double found_at = -10.0;
        // Willies the game frees (a removed corpse) leave the list and the touch marks at once
        // (gc_sync); a new list is also taken after every garbage collection (crash 2026-11-20 08:13:
        // this list still held a Willie the test room had removed and the game had freed).
        static bool tracked = on_freed([] {
            size_t before = foes.size();
            erase_gone(foes);
            if (foes.size() != before) HSVR_LOG("Elbow: %d freed Willie(s) taken off the foe list", (int)(before - foes.size()));
            for (ElbowState& e : g_elbow) erase_gone_keys(e.touching_until);
        });
        static uint64_t epoch = 0;
        double now = now_seconds();
        if (now - found_at > 1.0 || epoch != g_gc_epoch)
        {
            found_at = now;
            epoch = g_gc_epoch;
            foes.clear();
            std::vector<UObject*> all;
            UObjectGlobals::FindAllOf(STR("Willie_BP_C"), all);
            int skipped = 0;
            for (UObject* w : all)
                if (w && w != pawn && w->GetName().rfind(STR("Default__"), 0) != 0)
                {
                    if (object_alive(w)) foes.push_back(w);
                    else ++skipped;
                }
            static int skipped_logged = 0, count_logged = -1;
            if (skipped != skipped_logged || (int)foes.size() != count_logged)
                HSVR_LOG("Elbow: %d foe(s) on the list, %d Willie(s) being destroyed left off", (int)foes.size(), skipped);
            skipped_logged = skipped;
            count_logged = (int)foes.size();
        }
        for (int side = 0; side < 2; ++side)
        {
            ElbowState& E = g_elbow[side];
            std::string sx = side == 0 ? "_l" : "_r";
            Vec3 elbow = socket_location(g_pose.mesh, bone_fname("lowerarm" + sx));
            Vec3 hand = socket_location(g_pose.mesh, bone_fname("hand" + sx));
            Vec3 shoulder = socket_location(g_pose.mesh, bone_fname("upperarm" + sx));
            Vec3 pts[3] = {elbow, elbow + normalized(hand - elbow) * 7.0, elbow + normalized(shoulder - elbow) * 7.0};
            double dt = now - E.last_t;
            if (E.have && dt > 1e-4 && dt < 0.1)
                for (int i = 0; i < 3; ++i) E.vel[i] = E.vel[i] + ((pts[i] - E.last[i]) * (1.0 / dt) - E.vel[i]) * 0.5;
            else
                for (int i = 0; i < 3; ++i) E.vel[i] = {};
            for (int i = 0; i < 3; ++i) E.last[i] = pts[i];
            E.last_t = now;
            E.have = true;
            // Fist speed: the hand speed that gives the game's Aim Swing Speed 30 (the fist's spawn rule).
            double lo = cfg.pose_swing_speed_lo, hi = (std::max)(lo + 0.1, cfg.pose_swing_speed_hi);
            double min_speed = (lo + (hi - lo) * cfg.pose_elbow_min_ass / 75.0) * 100.0;
            for (UObject* w : foes)
            {
                if (!object_alive(w)) continue; // destroyed this second (still readable, not a fighter)
                UObject* mesh = object_property(w, STR("Mesh"));
                if (!mesh) continue;
                if (length(component_location(mesh) - elbow) > 200.0) continue;
                double best_d = 1e9;
                int best_i = -1;
                Vec3 best_pt;
                FName best_bone;
                for (int i = 0; i < 3; ++i)
                {
                    // His nearest physics body first: GetClosestPointOnCollision with no bone measures only
                    // his root body (the pelvis), so test 12 never saw the elbow reach his head.
                    Call fb(mesh, STR("FindClosestBone_K2"));
                    fb.three(STR("TestLocation"), pts[i].x, pts[i].y, pts[i].z);
                    fb.number(STR("IgnoreScale"), 0.0);
                    fb.flag(STR("bRequirePhysicsAsset"), true);
                    fb.run();
                    FProperty* rp = fb.param(STR("ReturnValue"));
                    if (!rp) continue;
                    FName bone = *reinterpret_cast<FName*>(fb.buffer.data() + rp->GetOffset_Internal());
                    Call cp(mesh, STR("GetClosestPointOnCollision"));
                    cp.three(STR("Point"), pts[i].x, pts[i].y, pts[i].z);
                    cp.name(STR("BoneName"), bone);
                    cp.run();
                    double d = cp.number();
                    if (d < 0.0 || d >= best_d) continue;
                    best_d = d, best_i = i, best_bone = bone;
                    best_pt = d > 0.0 ? cp.vec(STR("OutPointOnBody")) : pts[i];
                }
                static double near_log = -10.0;
                if (best_i >= 0 && best_d < 30.0 && now - near_log >= 1.0)
                {
                    near_log = now;
                    HSVR_LOG("Elbow: %s arm near %ls: %s %.1f cm from his %ls, speed %.0f cm/s (needs %.0f), in contact since last hit %d",
                             side == 0 ? "left" : "right", w->GetName().c_str(), best_i == 0 ? "elbow" : best_i == 1 ? "forearm" : "upper arm", best_d,
                             best_bone.ToString().c_str(), length(E.vel[best_i]), min_speed, (int)(E.touching_until[w] > 0.0));
                }
                double& until = E.touching_until[w];
                if (best_i < 0 || best_d > cfg.pose_elbow_radius_cm)
                {
                    if (best_i >= 0 && best_d > cfg.pose_elbow_radius_cm + 3.0) until = 0.0;
                    continue;
                }
                if (until > 0.0) continue; // still in contact since the last hit
                Vec3 v = E.vel[best_i];
                Vec3 into = best_d > 0.5 ? normalized(best_pt - pts[best_i]) : normalized(v);
                double closing = dot(v, into);
                if (closing < min_speed) continue;
                until = 1.0;
                if (now - E.last_hit < 0.1) continue; // the game's own limb-hit cooldown
                E.last_hit = now;
                // The punch's numbers (learnt from your fist hits, else the defaults).
                bool learnt = g_punch.n >= cfg.pose_elbow_learn_min_hits;
                double rig = learnt ? g_punch.rigidity : cfg.pose_elbow_rigidity;
                double cutp = learnt ? g_punch.cut : cfg.pose_elbow_cut;
                double ipv = learnt ? g_punch.impulse_per_v : cfg.pose_elbow_impulse_per_v;
                double vps = learnt && g_punch.v_per_speed > 0.0 ? g_punch.v_per_speed : cfg.pose_elbow_velocity_scale;
                Vec3 hv = v * vps;
                Vec3 hi_v = into * (length(hv) * ipv);
                Vec3 normal = into * -1.0; // his surface, facing the elbow
                Call dc(w, STR("Deal Complex Damage"));
                if (!dc.fn)
                {
                    HSVR_LOG_EVERY(10.0, "Elbow: %ls has no Deal Complex Damage", w->GetName().c_str());
                    continue;
                }
                uint8_t* base = dc.buffer.data();
                dc.object(STR("Hit Component"), mesh);
                dc.object(STR("Collided Component"), g_pose.mesh);
                dc.name(STR("Hit Bone"), best_bone);
                dc.three(STR("Location"), best_pt.x, best_pt.y, best_pt.z);
                dc.three(STR("Normal"), normal.x, normal.y, normal.z);
                dc.three(STR("Hit Velocity"), hv.x, hv.y, hv.z);
                dc.three(STR("Hit Impulse"), hi_v.x, hi_v.y, hi_v.z);
                auto num = [&](const TCHAR* n, double x) { if (FProperty* p = dc.param(n)) write_param_number(base, p, x); };
                auto flag = [&](const TCHAR* n, bool x) { if (FProperty* p = dc.param(n)) write_param_bool(base, p, x); };
                num(STR("Cutting Power"), cutp);
                num(STR("Stab Rate"), learnt ? g_punch.stab : 0.0);
                num(STR("Rigidity"), rig);
                num(STR("Blunt Destruction Int"), g_punch.have_template ? g_punch.blunt_int : 1.0);
                num(STR("Kick Power"), g_punch.have_template ? g_punch.kick : 1.0);
                num(STR("Draw Cut"), 0.0);
                flag(STR("Lower Threshold In"), g_punch.have_template && g_punch.lower_threshold);
                flag(STR("Damage Parent Bone?"), g_punch.have_template && g_punch.damage_parent);
                flag(STR("Extra High Velocity"), g_punch.have_template && g_punch.extra_high);
                double h0 = read_number_variable(w, STR("Health")), c0 = read_number_variable(w, STR("Consciousness"));
                g_elbow_calling.store(true);
                dc.run();
                g_elbow_calling.store(false);
                double dmg = -1.0, rig_out = -1.0;
                if (FProperty* p = dc.param(STR("Damage Out"))) dmg = read_param_number(base, p);
                if (FProperty* p = dc.param(STR("Rigidity Out"))) rig_out = read_param_number(base, p);
                controllers_buzz(side, 0.5f);
                const char* where[3] = {"elbow", "forearm", "upper arm"};
                HSVR_LOG("Elbow: %s %s hit %ls %ls at %.0f cm/s (%.1f cm off): velocity %.0f, impulse %.0f, rigidity %.2f, cut %.2f (%s); damage out %.1f, rigidity out %.2f; "
                         "health %.1f -> %.1f, consciousness %.0f -> %.0f",
                         side == 0 ? "left" : "right", where[best_i], w->GetName().c_str(), best_bone.ToString().c_str(), closing, best_d, length(hv), length(hi_v), rig, cutp,
                         learnt ? "learnt from your punches" : "defaults", dmg, rig_out, h0, read_number_variable(w, STR("Health")), c0,
                         read_number_variable(w, STR("Consciousness")));
            }
        }
    }

    // pose_grab_body_no_slip: right before Willie's tick (where the slip rule runs), the held hand's
    // 'Hand Block Drop' is set while his grip strength is above pose_grab_body_slip_below_t.
    bool g_body_blocking[2] = {false, false};
    void body_grab_before_tick(UObject* pawn)
    {
        hsvr::Config cfg = hsvr::config();
        for (int side = 0; side < 2; ++side)
        {
            const BodyGrab& b = g_body_grab[side];
            bool want = cfg.pose_grab_body_no_slip && b.on && b.grip_t >= cfg.pose_grab_body_slip_below_t;
            const TCHAR* flag = side == 0 ? STR("L Hand Block Drop") : STR("R Hand Block Drop");
            if (want) set_bool_property(pawn, flag, true);
            else if (g_body_blocking[side]) set_bool_property(pawn, flag, false);
            if (want != g_body_blocking[side])
                HSVR_LOG("Body grab: %s hand's slip rule %s (his grip T %.0f)", side == 0 ? "left" : "right", want ? "held off" : "back to the game's", b.grip_t);
            g_body_blocking[side] = want;
        }
    }

    void body_grab_tick(UObject* pawn, const hsvr::Config& cfg_in)
    {
        using namespace hsvr;
        Config cfg = cfg_in;
        if (g_body_hold_test.load() != -99) cfg.pose_grab_body_hold = g_body_hold_test.load();
        UObject* mesh = object_property(pawn, STR("Mesh"));
        double now = now_seconds();
        for (int side = 0; side < 2; ++side)
        {
            bool L = side == 0;
            const char* hand = L ? "left" : "right";
            BodyGrab& b = g_body_grab[side];
            ObjectGrab& og = g_object_grab[side];
            bool grabbed = bool_property(pawn, L ? STR("Grabbed L") : STR("Grabbed R"));
            bool body = grabbed && og.component && og.character;
            if (!body)
            {
                if (b.on)
                    HSVR_LOG("Body grab: %s hand let go after %.2f s (Grabbed %d); last: joint force %.0f / torque %.0f, release rate %.2f, stretch %.1f cm, strangle %.2f, choke %.3f, consciousness %.0f",
                             hand, now - b.since, (int)grabbed, b.force, b.torque, b.release_rate, b.stretch, b.strangle, b.choke, b.conscious);
                b = BodyGrab{};
                continue;
            }
            Vec3 hb = mesh ? socket_location(mesh, bone_fname(L ? "hand_l" : "hand_r")) : Vec3{};
            if (!b.on)
            {
                b.on = true;
                b.since = now;
                b.comp = og.component;
                b.bone = og.bone;
                Call oc(og.component, STR("GetOwner"));
                oc.run();
                b.victim = oc.object_result();
                b.rest = length(hb - socket_location(b.comp, b.bone));
                HSVR_LOG("Body grab: %s hand holds %ls bone %ls (%.1f cm from the hand bone)", hand, b.victim ? b.victim->GetName().c_str() : STR("?"),
                         b.bone.ToString().c_str(), b.rest);
            }
            b.stretch = length(hb - socket_location(b.comp, b.bone)) - b.rest;
            UObject* joint = object_property(pawn, L ? STR("PhysicsConstraint L Hand") : STR("PhysicsConstraint R Hand"));
            if (joint)
            {
                Call f(joint, STR("GetConstraintForce"));
                if (f.run())
                {
                    b.force = length(f.vec(STR("OutLinearForce")));
                    b.torque = length(f.vec(STR("OutAngularForce")));
                }
            }
            b.release_rate = read_number_variable(pawn, L ? STR("Grab L Release Rate") : STR("Grab R Release Rate"));
            if (b.victim)
            {
                b.strangle = read_number_variable(b.victim, STR("Strangle Rate"));
                b.choke = read_number_variable(b.victim, STR("Choke To Death Rate"));
                b.conscious = read_number_variable(b.victim, STR("Consciousness"));
            }
            // His grip strength as the game's slip rule computes it (grab_and_mass.md, UG @361596-362128;
            // the gauntlet/Mordhau/zombie/lawnmower factors left out): T = Arm Tonus x All Body Tonus x
            // Consciousness/100 x MRC(Pain, 5..10 -> 1..2) x max(MRU(Muscle Rate, 0..1 -> 1..1.5), 0.5),
            // then max(T, Arm Health). It slips while the joint force is above T.
            double tonus = read_number_variable(pawn, L ? STR("Arm L Tonus") : STR("Arm R Tonus"));
            double body_tonus = read_number_variable(pawn, STR("All Body Tonus"));
            double my_conscious = read_number_variable(pawn, STR("Consciousness"));
            double pain = read_number_variable(pawn, STR("Pain"));
            double muscle = read_number_variable(pawn, STR("Muscle Rate"));
            double arm_health = read_number_variable(pawn, L ? STR("Arm_L Health") : STR("Arm_R Health"));
            double pain_k = 1.0 + (std::max)(0.0, (std::min)(1.0, (pain - 5.0) / 5.0));
            double t_est = tonus * body_tonus * my_conscious * 0.01 * pain_k * (std::max)(1.0 + 0.5 * muscle, 0.5);
            t_est = (std::max)(t_est, arm_health);
            b.grip_t = t_est;
            if (cfg.pose_grab_body_hold == 1 && !b.softened && joint && now - b.since > 0.25)
            {
                double cap = body_spring_cap(cfg, b.grip_t);
                body_spring_joint(joint, cfg, cap);
                b.softened = true;
                HSVR_LOG("Body grab: %s hand's joint made a spring (%.0f, %.0f, cap %.0f for his grip T %.0f)", hand, cfg.pose_grab_body_spring, cfg.pose_grab_body_damper, cap, b.grip_t);
            }
            if ((cfg.pose_grab_body_hold == 1 || cfg.pose_grab_body_no_slip) && b.stretch > cfg.pose_grab_body_let_go_cm)
            {
                if (b.far_since < 0.0) b.far_since = now;
                else if (now - b.far_since > 0.3)
                {
                    HSVR_LOG("Body grab: %s hand pulled %.1f cm off its hold for 0.3 s: lets go", hand, b.stretch);
                    release_object(side, pawn);
                    continue;
                }
            }
            else
                b.far_since = -1.0;
            if (cfg.log_body_grab && now - b.last_log >= 0.1)
            {
                b.last_log = now;
                double vs = 0.0;
                if (b.victim)
                {
                    Call v(b.victim, STR("GetVelocity"));
                    if (v.run()) vs = length(v.vec());
                }
                // The slip rule's gates (UG @360189-362402): it runs only when all of these allow it.
                HSVR_LOG("Body grab: %s %.2f s: his grip T~%.0f (arm tonus %.2f, body tonus %.2f, consciousness %.0f, pain %.1f, muscle %.2f, arm health %.0f); gates: invulnerable %d, dress-up %d, block drop %d, two-handed %d, soft not attached %d, sheathing %d, unsheathing %d",
                         hand, now - b.since, b.grip_t, tonus, body_tonus, my_conscious, pain, muscle, arm_health, (int)bool_property(pawn, STR("Invulnerable")),
                         (int)bool_property(pawn, STR("DressUp")), (int)bool_property(pawn, L ? STR("L Hand Block Drop") : STR("R Hand Block Drop")),
                         (int)bool_property(pawn, L ? STR("L Two Handed Grip") : STR("R Two Handed Grip")),
                         (int)bool_property(pawn, L ? STR("L Hand Soft Not Attached") : STR("R Hand Soft Not Attached")),
                         (int)bool_property(pawn, L ? STR("L Weapon Sheathing") : STR("R Weapon Sheathing")),
                         (int)bool_property(pawn, L ? STR("Weapon UnSheathing L") : STR("Weapon UnSheathing R")));
                HSVR_LOG("Body grab: %s %.2f s: joint force %.0f / torque %.0f, release rate %.2f, stretch %.1f cm; victim strangle %.2f, choke %.3f, consciousness %.0f, throat force %.0f, being grabbed %d, moving %.0f cm/s",
                         hand, now - b.since, b.force, b.torque, b.release_rate, b.stretch, b.strangle, b.choke, b.conscious,
                         b.victim ? read_number_variable(b.victim, L ? STR("Grabbed Throat Constraint Force L") : STR("Grabbed Throat Constraint Force R")) : -1.0,
                         b.victim ? (int)bool_property(b.victim, STR("Being Grabbed")) : -1, vs);
            }
        }
    }

    // ---- Fists from your real hand ---------------------------------------------------------------
    // ---- Fingers from the real hand (Matthew 2026-11-19) ----------------------------------------
    // ABP_Willie has no finger nodes (its 22 end-of-graph Modify Bone nodes are all the arms/head),
    // so the fingers are written where his visible body takes its pose: CharacterMesh0's ABP_CopyPose1
    // copies the DriverSkeleton through a Copy Pose From Mesh node, whose PreUpdate (game thread, right
    // before BlueprintUpdateAnimation) snapshots the driver's component-space bones into the node's
    // SourceMeshTransformArray; the worker evaluation later turns each into a local transform against
    // its parent's snapshot (finger_pose_research.md section 3). So after that update the mod turns
    // each finger joint, parent first, in that snapshot. Head, armour and gore copy CharacterMesh0,
    // so gauntlets follow. Curl per joint at a full curl: the VR framework's table (ik/fingers.rs).
    // The bend axis is found from the hand's own geometry (finger direction x palm normal), not
    // assumed from the bone axes, and checked by sign each time.
    struct XForm96
    {
        double qx, qy, qz, qw;
        double tx, ty, tz, tpad;
        double sx, sy, sz, spad;
    };
    static_assert(sizeof(XForm96) == 96, "FTransform (double) is 96 bytes in UE 5.4");
    struct XFormArray
    {
        XForm96* data;
        int32_t num;
        int32_t max;
    };

    struct FingerRig
    {
        UObject* instance = nullptr; // ABP_CopyPose1 on CharacterMesh0
        UObject* driver = nullptr;
        bool looked = false;
        bool ok = false;
        int node_offset = -1;  // the copy node inside the anim instance
        int array_offset = -1; // SourceMeshTransformArray inside the node
        int bone_count = 0;
        int hand[2] = {-1, -1};
        int chain[2][5][3]; // [side][thumb..pinky][_01.._03]
        std::vector<int> parent;
            std::vector<int> subtree[2][5][3];           // bone and all its descendants
        double found_at = -1.0;
        int tries = 0;
        double next_try = 0.0;
    };
    FingerRig g_frig;
    // What the hand wants, set on the game tick (fingers_track_tick), read by the animation hook.
    struct FingerWant
    {
        bool on = false;            // the hand is free: its fingers follow yours
        double curl[5] = {0, 0, 0, 0, 0};    // smoothed, thumb .. little (the thumb may go below 0: lifted)
        double target[5] = {0, 0, 0, 0, 0};
        double splay = 0.0, target_splay = 0.0;
        double at = -1.0;
    };
    FingerWant g_fwant[2];
    std::atomic<float> g_fake_thumb[2] = {-1.0f, -1.0f}; // test: hsvr.thumb <0|1> <curl>, -1 = the controller
    std::atomic<float> g_fake_fingers[2][5] = {{-1.0f, -1.0f, -1.0f, -1.0f, -1.0f}, {-1.0f, -1.0f, -1.0f, -1.0f, -1.0f}}; // test: hsvr.fingers
    std::atomic<bool> g_finger_log_now{false};

    // Full-curl bend per joint (radians), thumb .. little, _01 .. _03.
    // The VR framework's table (avatar/src/rig.rs CURL, finger_pose_research.md 1.3), with how far a
    // base may bend back (LIFT, a curl below 0) and each base's spread at splay 1 (+ toward the thumb).
    const double g_finger_full[5][3] = {
        {0.45, 0.60, 0.80},
        {1.22, 1.66, 1.05},
        {1.36, 1.75, 1.13},
        {1.43, 1.75, 1.08},
        {1.54, 1.66, 1.05},
    };
    const double g_finger_lift[5] = {1.6, 0.35, 0.35, 0.35, 0.35};
    const double g_finger_spread[5] = {0.35, 0.21, 0.03, -0.14, -0.31};
    const char* kFingerNames[5] = {"thumb", "index", "middle", "ring", "pinky"};

    int driver_bone_index(UObject* mesh, const std::string& bone)
    {
        Call c(mesh, STR("GetBoneIndex"));
        c.name(STR("BoneName"), bone_fname(bone));
        return c.run() ? c.integer() : -1;
    }

    // The bone's transform in the component's space, by GetSocketTransform(RTS_Component).
    bool component_space_bone(UObject* mesh, const std::string& bone, XForm96& out)
    {
        Call c(mesh, STR("GetSocketTransform"));
        if (!c.fn) return false;
        c.name(STR("InSocketName"), bone_fname(bone));
        c.byte(STR("TransformSpace"), 2); // RTS_Component
        FProperty* r = c.param(STR("ReturnValue"));
        if (!r || r->GetSize() != (int)sizeof(XForm96)) return false;
        if (!c.run()) return false;
        std::memcpy(&out, c.buffer.data() + r->GetOffset_Internal(), sizeof(out));
        return true;
    }

    bool readable(const void* p, size_t n)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!p || !VirtualQuery(p, &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
        const uint8_t* end = reinterpret_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
        return reinterpret_cast<const uint8_t*>(p) + n <= end;
    }

    bool finger_rig_find(UObject* instance, UObject* driver)
    {
        FingerRig& R = g_frig;
        R = FingerRig{};
        R.instance = instance;
        R.driver = driver;
        R.looked = true;
        // The driver's bones: count, parents, the hand and finger chains.
        {
            Call n(driver, STR("GetNumBones"));
            if (!n.run()) return false;
            R.bone_count = n.integer();
        }
        if (R.bone_count <= 0 || R.bone_count > 1024) return false;
        R.parent.assign(R.bone_count, -1);
        for (int i = 0; i < R.bone_count; ++i)
        {
            Call bn(driver, STR("GetBoneName"));
            if (FProperty* p = bn.param(STR("BoneIndex"))) *reinterpret_cast<int32_t*>(bn.buffer.data() + p->GetOffset_Internal()) = i;
            bn.run();
            FProperty* rp = bn.param(STR("ReturnValue"));
            if (!rp) return false;
            FName name = *reinterpret_cast<FName*>(bn.buffer.data() + rp->GetOffset_Internal());
            Call pb(driver, STR("GetParentBone"));
            pb.name(STR("BoneName"), name);
            pb.run();
            FProperty* pr = pb.param(STR("ReturnValue"));
            if (!pr) return false;
            FName parent = *reinterpret_cast<FName*>(pb.buffer.data() + pr->GetOffset_Internal());
            std::wstring pw = parent.ToString();
            R.parent[i] = pw == STR("None") ? -1 : driver_bone_index(driver, std::string(pw.begin(), pw.end()));
        }
        bool bones_ok = true;
        for (int side = 0; side < 2; ++side)
        {
            std::string sx = side == 0 ? "_l" : "_r";
            R.hand[side] = driver_bone_index(driver, "hand" + sx);
            if (R.hand[side] < 0) bones_ok = false;
            for (int f = 0; f < 5; ++f)
                for (int j = 0; j < 3; ++j)
                {
                    char nm[32];
                    std::snprintf(nm, sizeof(nm), "%s_%02d%s", kFingerNames[f], j + 1, sx.c_str());
                    R.chain[side][f][j] = driver_bone_index(driver, nm);
                    if (R.chain[side][f][j] < 0) bones_ok = false;
                }
        }
        if (!bones_ok)
        {
            HSVR_LOG("Fingers: the driver lacks a hand or finger bone; per-finger posing off");
            return false;
        }
        // Each joint's subtree (it and everything below it), for turning a joint with what it carries.
        for (int side = 0; side < 2; ++side)
            for (int f = 0; f < 5; ++f)
                for (int j = 0; j < 3; ++j)
                {
                    int root = R.chain[side][f][j];
                    auto& out = R.subtree[side][f][j];
                    for (int i = 0; i < R.bone_count; ++i)
                        for (int a = i, guard = 0; a >= 0 && guard < 64; a = R.parent[a], ++guard)
                            if (a == root)
                            {
                                out.push_back(i);
                                break;
                            }
                }
        // The copy node: a struct property of the anim instance whose struct is a Copy Pose From Mesh.
        UScriptStruct* node_struct = nullptr;
        for (UStruct* level = static_cast<UStruct*>(instance->GetClassPrivate()); level && R.node_offset < 0; level = level->GetSuperStruct())
            for (FProperty* p : level->ForEachProperty())
            {
                if (p->GetClass().GetName() != STR("StructProperty")) continue;
                UScriptStruct* s = static_cast<FStructProperty*>(p)->GetStruct();
                if (!s || s->GetName().find(STR("AnimNode_CopyPoseFromMesh")) == std::wstring::npos) continue;
                R.node_offset = p->GetOffset_Internal();
                node_struct = s;
                HSVR_LOG("Fingers: copy node %ls (%ls) at +%d, %d bytes", p->GetName().c_str(), s->GetName().c_str(), R.node_offset, s->GetPropertiesSize());
                break;
            }
        if (R.node_offset < 0 || !node_struct)
        {
            HSVR_LOG("Fingers: no Copy Pose From Mesh node in %ls; per-finger posing off", instance->GetClassPrivate()->GetName().c_str());
            return false;
        }
        // Its private snapshot: a transform array as long as the driver's skeleton, whose hand matches
        // the driver's hand in component space.
        XForm96 hand_cs{};
        if (!component_space_bone(driver, "hand_r", hand_cs)) return false;
        uint8_t* node = reinterpret_cast<uint8_t*>(instance) + R.node_offset;
        int size = node_struct->GetPropertiesSize();
        for (int off = 0; off + (int)sizeof(XFormArray) <= size; off += 8)
        {
            auto* a = reinterpret_cast<XFormArray*>(node + off);
            if (a->num != R.bone_count || a->max < a->num || a->max > 4096 || !a->data) continue;
            if (!readable(a->data, sizeof(XForm96) * (size_t)a->num)) continue;
            const XForm96& h = a->data[R.hand[1]];
            double d = std::sqrt((h.tx - hand_cs.tx) * (h.tx - hand_cs.tx) + (h.ty - hand_cs.ty) * (h.ty - hand_cs.ty) + (h.tz - hand_cs.tz) * (h.tz - hand_cs.tz));
            HSVR_LOG("Fingers: transform array at node+%d (%d bones): hand_r (%.1f, %.1f, %.1f) vs driver (%.1f, %.1f, %.1f), %.2f cm apart, scale %.2f",
                     off, a->num, h.tx, h.ty, h.tz, hand_cs.tx, hand_cs.ty, hand_cs.tz, d, h.sx);
            if (d < 3.0)
            {
                R.array_offset = off;
                break;
            }
        }
        if (R.array_offset < 0)
        {
            HSVR_LOG("Fingers: the copy node's snapshot was not found; per-finger posing off (the fist shape stays)");
            return false;
        }
        R.ok = true;
        R.found_at = hsvr::now_seconds();
        return true;
    }

    hsvr::Vec3 xf_pos(const XForm96& x) { return {x.tx, x.ty, x.tz}; }
    hsvr::Quat xf_rot(const XForm96& x) { return {x.qx, x.qy, x.qz, x.qw}; }

    // Turns bone `j` of a finger (and all it carries) about `axis` through its own position.
    void turn_subtree(XForm96* bones, const std::vector<int>& subtree, const hsvr::Vec3& pivot, const hsvr::Quat& turn)
    {
        for (int i : subtree)
        {
            XForm96& b = bones[i];
            hsvr::Quat r = (turn * xf_rot(b)).normalized();
            hsvr::Vec3 p = pivot + turn.rotate(xf_pos(b) - pivot);
            b.qx = r.x, b.qy = r.y, b.qz = r.z, b.qw = r.w;
            b.tx = p.x, b.ty = p.y, b.tz = p.z;
        }
    }

    // Signed bend of `child_dir` from `parent_dir` about `axis` (radians; + = toward the palm).
    double bend_about(const hsvr::Vec3& parent_dir, const hsvr::Vec3& child_dir, const hsvr::Vec3& axis)
    {
        using namespace hsvr;
        Vec3 a = parent_dir - axis * dot(parent_dir, axis), b = child_dir - axis * dot(child_dir, axis);
        if (length(a) < 1e-6 || length(b) < 1e-6) return 0.0;
        a = normalized(a), b = normalized(b);
        return std::atan2(dot(cross(a, b), axis), dot(a, b));
    }

    // Writes one hand's fingers into the snapshot (the framework's Finger::bend, avatar/src/rig.rs):
    // each finger's base spreads (splay), then each joint bends about the finger's flex axis to
    // curl x CURL, replacing the bend the pose had (so curl 0 = straight); the thumb's base adds its
    // bend to the pose's own (the game's open hand, the framework's rest pose). Axes from the hand
    // bone (hand_bones.md: fingers +X on hand_l / -X on hand_r, thumb +Y, palm -Z), checked against
    // the knuckles' own geometry in the log.
    bool pose_hand_fingers(XForm96* B, int side, const double curl[5], double splay, bool log)
    {
        using namespace hsvr;
        FingerRig& R = g_frig;
        auto P = [&](int i) { return xf_pos(B[i]); };
        const int(*ch)[3] = R.chain[side];
        Quat hr = xf_rot(B[R.hand[side]]);
        Vec3 hand = P(R.hand[side]);
        Vec3 along = normalized(hr.rotate(Vec3{side == 0 ? 1.0 : -1.0, 0, 0}));
        Vec3 thumbward = normalized(hr.rotate(Vec3{0, 1, 0}));
        Vec3 palm = normalized(hr.rotate(Vec3{0, 0, -1}));
        char line[900];
        int at = 0;
        if (log)
        {
            Vec3 g_along = normalized(P(ch[2][0]) - hand);
            Vec3 g_across = normalized(P(ch[1][0]) - P(ch[4][0]));
            Vec3 g_palm = normalized(cross(g_across, g_along) * (side == 1 ? -1.0 : 1.0));
            at += std::snprintf(line, sizeof(line), " axes vs knuckles: along %.2f, thumb side %.2f, palm %.2f;", dot(along, g_along), dot(thumbward, g_across),
                                dot(palm, g_palm));
        }
        Vec3 splay_axis = normalized(cross(along, thumbward)); // + turns a finger toward the thumb
        for (int f = 0; f < 5; ++f)
        {
            bool thumb = f == 0;
            int b0 = ch[f][0], b1 = ch[f][1], b2 = ch[f][2];
            Vec3 seg1 = P(b2) - P(b1);
            if (length(seg1) < 0.05) continue;
            // The last segment has no child bone: it points as the one before it does, in its own bone's frame.
            Vec3 u = xf_rot(B[b1]).inverse().rotate(normalized(seg1));
            auto seg = [&](int j) {
                return normalized(j < 2 ? P(ch[f][j + 1]) - P(ch[f][j]) : xf_rot(B[ch[f][j]]).rotate(u));
            };
            Vec3 tip_before = P(b2) + xf_rot(B[b2]).rotate(u) * 2.5;
            double c0 = (std::max)(-g_finger_lift[f], (std::min)(1.0, curl[f]));
            double c12 = (std::max)(0.0, (std::min)(1.0, curl[f]));
            // Splay at the base: the fingers close up as the base curls.
            double sp = (std::max)(0.0, (std::min)(1.0, splay)) * (1.0 - (std::max)(0.0, (std::min)(1.0, c0)));
            double spread_now = bend_about(along, seg(0), splay_axis);
            double spread_turn = thumb ? (sp - 0.3) * g_finger_spread[0] : sp * g_finger_spread[f] - spread_now;
            turn_subtree(B, R.subtree[side][f][0], P(b0), quat_axis_angle(splay_axis, spread_turn));
            Vec3 d0 = seg(0);
            Vec3 toward = thumb ? normalized(palm * 0.6 - thumbward) : palm;
            Vec3 axis = cross(d0, toward);
            if (length(axis) < 1e-6) continue;
            axis = normalized(axis);
            double before[3] = {0, 0, 0}, after[3] = {0, 0, 0};
            for (int j = 0; j < 3; ++j)
            {
                Vec3 prev = j == 0 ? along : seg(j - 1);
                double bend = bend_about(prev, seg(j), axis);
                double c = j == 0 ? c0 : c12;
                double turn = (j == 0 && thumb) ? c * g_finger_full[0][0] : c * g_finger_full[f][j] - bend;
                before[j] = bend;
                turn_subtree(B, R.subtree[side][f][j], P(ch[f][j]), quat_axis_angle(axis, turn));
                after[j] = bend_about(prev, seg(j), axis);
            }
            if (log && at < (int)sizeof(line) - 160)
            {
                Vec3 tip = P(b2) + xf_rot(B[b2]).rotate(u) * 2.5;
                at += std::snprintf(line + at, sizeof(line) - at, " %s %.2f: %.0f/%.0f/%.0f -> %.0f/%.0f/%.0f deg, spread %.0f->%.0f, tip %.1f cm from hand, palmward %.1f->%.1f;",
                                    kFingerNames[f], curl[f], before[0] * 57.3, before[1] * 57.3, before[2] * 57.3, after[0] * 57.3, after[1] * 57.3,
                                    after[2] * 57.3, spread_now * 57.3, bend_about(along, d0, splay_axis) * 57.3, length(tip - hand), dot(tip_before - hand, palm),
                                    dot(tip - hand, palm));
            }
        }
        if (log) HSVR_LOG("Fingers %s (splay %.2f):%s", side == 0 ? "left" : "right", splay, line);
        return true;
    }

    UFunction* g_copy_update_fn = nullptr;
    void fingers_after_copy_update(Hook::TCallbackIterationData<void>&, UObject* context, UFunction* function, void*)
    {
        gc_sync();
        HSVR_PROF(PerfFingersCopy);
        FingerRig& R = g_frig;
        if (!context || !function || !R.instance || context != R.instance || !g_pose_active.load()) return;
        if (g_copy_update_fn) { if (function != g_copy_update_fn) return; }
        else
        {
            if (function->GetName() != STR("BlueprintUpdateAnimation")) return;
            g_copy_update_fn = function;
            HSVR_LOG("Fingers: %ls's animation update seen", context->GetClassPrivate()->GetName().c_str());
        }
        if (!R.ok)
        {
            // Looked for every 2 s, up to 10 times (the snapshot can be empty or a frame apart at first).
            double t = hsvr::now_seconds();
            if (R.tries >= 10 || t < R.next_try || !R.driver) return;
            int tries = R.tries + 1;
            bool found = finger_rig_find(context, R.driver);
            R.tries = tries;
            R.next_try = t + 2.0;
            if (!found)
            {
                if (tries == 10) HSVR_LOG("Fingers: gave up finding the copy node's snapshot; the fist shape stays");
                return;
            }
            HSVR_LOG("Fingers: per-finger posing on (try %d)", tries);
        }
        uint8_t* node = reinterpret_cast<uint8_t*>(context) + R.node_offset;
        auto* a = reinterpret_cast<XFormArray*>(node + R.array_offset);
        if (a->num != R.bone_count || !a->data) return;
        static double last_log = -10.0;
        double now = hsvr::now_seconds();
        bool log = g_finger_log_now.exchange(false) || (hsvr::config().log_fingers && now - last_log >= 1.0 && (g_fwant[0].on || g_fwant[1].on));
        if (log) last_log = now;
        for (int side = 0; side < 2; ++side)
        {
            const FingerWant& w = g_fwant[side];
            if (!w.on || now - w.at > 0.2) continue;
            pose_hand_fingers(a->data, side, w.curl, w.splay, log);
        }
    }

    // Game tick: which hands are free, and the curls they want (smoothed).
    void fingers_track_tick(const hsvr::Config& cfg, const hsvr::ControllerState& state, UObject* pawn)
    {
        double now = hsvr::now_seconds();
        UObject* copy = g_pose.mesh ? object_property(g_pose.mesh, STR("AnimScriptInstance")) : nullptr;
        if (copy != g_frig.instance || g_pose.driver != g_frig.driver)
        {
            g_frig = FingerRig{};
            g_frig.instance = copy;
            g_frig.driver = g_pose.driver;
            g_copy_update_fn = nullptr;
        }
        for (int side = 0; side < 2; ++side)
        {
            FingerWant& w = g_fwant[side];
            UObject* mine = object_property(pawn, side == 0 ? STR("Weapon L") : STR("Weapon R"));
            bool free = cfg.pose_fingers_tracked && (!mine || is_fist(mine)) &&
                        !bool_property(pawn, side == 0 ? STR("Grabbed L") : STR("Grabbed R")) && !scabbard_held(side) && !(g_two.active && side == g_two.second);
            const hsvr::HandState& h = state.hand[side];
            double target[5];
            double touch_override = -1.0;
            if (h.joints_valid && cfg.input_hand_tracking)
                for (int f = 0; f < 5; ++f) target[f] = h.joint_curl[f];
            else
            {
                // The VR framework's empty-hand curls from the sensors (ik/src/fingers.rs FingerSolver::target).
                double sq = g_fake_squeeze[side].load() >= 0.0f ? g_fake_squeeze[side].load() : h.squeeze;
                double trig = g_fake_trigger[side].load() >= 0.0f ? g_fake_trigger[side].load() : h.trigger;
                if (g_fake_thumb[side].load() >= 0.0f) touch_override = g_fake_thumb[side].load();
                double tc = touch_override >= 0.0 ? touch_override : h.sensor_curl[0];
                bool touch_thumb = tc >= 0.79, thumb_press = tc >= 0.99;
                target[0] = touch_thumb ? 0.12 + (0.75 - 0.12) * sq + (thumb_press ? 0.1 : 0.0) : -0.5; // lifted: a thumbs-up
                target[1] = 0.05 + 0.95 * trig; // the trigger's touch sensor does not move it
                const double lead[3] = {1.0, 1.03, 1.06};
                for (int f = 2; f < 5; ++f) target[f] = 0.15 + 0.85 * sq * lead[f - 2];
            }
            for (int f = 0; f < 5; ++f)
            {
                float fake = g_fake_fingers[side][f].load();
                if (fake >= 0.0f) target[f] = fake;
                target[f] = (std::max)(f == 0 ? -g_finger_lift[0] : 0.0, (std::min)(1.0, target[f]));
            }
            double open = 1.0 - (target[1] + target[2] + target[3] + target[4]) / 4.0;
            if (!free) { w.on = false; w.at = now; continue; }
            double dt = w.on ? (std::min)(3.0 / 90.0, (std::max)(0.0, now - w.at)) : 1.0; // MAX_DT 3/90 s
            double k = cfg.pose_fingers_tau_s > 0.0 ? 1.0 - std::exp(-dt / cfg.pose_fingers_tau_s) : 1.0;
            for (int f = 0; f < 5; ++f)
            {
                w.target[f] = target[f];
                w.curl[f] = w.on ? w.curl[f] + (target[f] - w.curl[f]) * k : target[f];
            }
            w.target_splay = 0.5 * open;
            w.splay = w.on ? w.splay + (w.target_splay - w.splay) * k : w.target_splay;
            w.on = true;
            w.at = now;
            // What the visible body shows (its last evaluated pose): fingertips from the hand bone.
            static double shown_at[2] = {-10.0, -10.0};
            if (cfg.log_fingers && g_frig.ok && now - shown_at[side] >= 1.0 && g_pose.mesh)
            {
                shown_at[side] = now;
                std::string sx = side == 0 ? "_l" : "_r";
                hsvr::Vec3 hand = socket_location(g_pose.mesh, bone_fname("hand" + sx));
                double d[5];
                for (int f = 0; f < 5; ++f) d[f] = hsvr::length(socket_location(g_pose.mesh, bone_fname(std::string(kFingerNames[f]) + "_03" + sx)) - hand);
                HSVR_LOG("Fingers shown: %s curls %.2f/%.2f/%.2f/%.2f/%.2f; _03 from the hand bone: thumb %.1f, index %.1f, middle %.1f, ring %.1f, pinky %.1f cm (four-finger mean %.1f)",
                         side == 0 ? "left" : "right", w.curl[0], w.curl[1], w.curl[2], w.curl[3], w.curl[4], d[0], d[1], d[2], d[3], d[4], (d[1] + d[2] + d[3] + d[4]) / 4.0);
            }
        }
    }

    bool fingers_tracked_on(int side)
    {
        return g_frig.ok && g_fwant[side].on && hsvr::now_seconds() - g_fwant[side].at < 0.2;
    }

    // Matthew 2026-11-19: the hand is a fist while you close it (grip, trigger, thumb resting on the
    // stick or a button), and opens when you open it. Curls come from SteamVR's hand skeleton when it
    // gives one, else from the sensors (controllers.cpp). A squeeze near something still grabs it.
    bool g_finger_fist[2] = {false, false};
    double g_fist_retry_at[2] = {-10.0, -10.0};
    double g_slap_until[2] = {-10.0, -10.0};

    void fist_finger_log(int side, UObject* pawn)
    {
        static double last[2] = {-10.0, -10.0};
        double now = hsvr::now_seconds();
        if (now - last[side] < 0.5) return;
        last[side] = now;
        UObject* mesh = object_property(pawn, STR("Mesh"));
        if (!mesh) return;
        std::string sx = side == 0 ? "_l" : "_r";
        hsvr::Vec3 hand = socket_location(mesh, bone_fname("hand" + sx));
        double d[5];
        const char* names[5] = {"thumb_03", "index_03", "middle_03", "ring_03", "pinky_03"};
        for (int f = 0; f < 5; ++f) d[f] = length(socket_location(mesh, bone_fname(std::string(names[f]) + sx)) - hand);
        HSVR_LOG("Fist shape %d: %s fingertips from the hand bone: thumb %.1f, index %.1f, middle %.1f, ring %.1f, pinky %.1f cm (four-finger mean %.1f)",
                 fist_shape(hsvr::config()), side == 0 ? "left" : "right", d[0], d[1], d[2], d[3], d[4], (d[1] + d[2] + d[3] + d[4]) / 4.0);
    }

    void fists_from_fingers(const hsvr::Config& cfg, const hsvr::ControllerState& state, UObject* pawn, bool menu_up)
    {
        if (!cfg.pose_fists || !cfg.pose_fist_from_fingers) return;
        for (int side = 0; side < 2; ++side)
        {
            const hsvr::HandState& h = state.hand[side];
            float index = g_fake_trigger[side].load() >= 0.0f ? g_fake_trigger[side].load() : h.curl[1];
            float grip = g_fake_squeeze[side].load() >= 0.0f ? g_fake_squeeze[side].load() : (h.curl[2] + h.curl[3] + h.curl[4]) / 3.0f;
            float thumb = g_fake_thumb[side].load() >= 0.0f ? g_fake_thumb[side].load() : h.curl[0];
            bool was = g_finger_fist[side];
            bool now = was ? (index >= cfg.pose_fist_index_off && grip >= cfg.pose_fist_grip_off && thumb >= cfg.pose_fist_thumb_off)
                           : (index >= cfg.pose_fist_index_on && grip >= cfg.pose_fist_grip_on && thumb >= cfg.pose_fist_thumb_on);
            g_finger_fist[side] = now;
            const char* hand = side == 0 ? "left" : "right";
            if (now != was)
                HSVR_LOG("Fist: %s hand %s (curls thumb %.2f index %.2f grip %.2f; skeleton %d: %.0f/%.0f/%.0f/%.0f/%.0f deg; trigger %.2f touch %d, squeeze %.2f, thumb touch %d)",
                         hand, now ? "closed" : "opened", thumb, index, grip, (int)h.joints_valid, h.joint_deg[0], h.joint_deg[1], h.joint_deg[2], h.joint_deg[3],
                         h.joint_deg[4], h.trigger, (int)h.trigger_touch, h.squeeze, (int)h.thumb_touch);
            UObject* mine = object_property(pawn, side == 0 ? STR("Weapon L") : STR("Weapon R"));
            // Slaps (Matthew 2026-11-19): the game has no open-hand weapon; an empty hand swung at Aim Swing
            // Speed >= 30 becomes its fist (spawn rule @557803, fists_eating.md 1.1). So an open hand moving
            // that fast gets the fist too (fingers left open), kept pose_fist_slap_keep_s after it slows.
            double t = hsvr::now_seconds();
            double lo = cfg.pose_swing_speed_lo, hi = (std::max)(lo + 0.1, cfg.pose_swing_speed_hi);
            double hand_ass = g_swing_v[side] <= lo ? 0.0 : (std::min)(75.0, (g_swing_v[side] - lo) / (hi - lo) * 75.0);
            if (cfg.pose_fist_slap && hand_ass >= cfg.pose_fist_slap_ass) g_slap_until[side] = t + cfg.pose_fist_slap_keep_s;
            bool slap = !now && t < g_slap_until[side];
            if (slap != g_fist_open[side].load() && (slap || is_fist(mine)))
                HSVR_LOG("Fist: %s hand %s (hand swing speed %.0f)", hand, slap ? "slaps: open-hand fist" : "closed over its slap fist", hand_ass);
            g_fist_open[side].store(slap);
            if (!now && !slap)
            {
                if (is_fist(mine)) remove_fist(side, pawn, "hand opened");
                continue;
            }
            if (is_fist(mine))
            {
                if (now) fist_finger_log(side, pawn);
                continue;
            }
            if (mine || menu_up || bool_property(pawn, side == 0 ? STR("Grabbed L") : STR("Grabbed R")) || scabbard_held(side) ||
                (g_two.active && side == g_two.second))
                continue;
            if (t < g_fist_retry_at[side]) continue;
            if (!make_fist(side, pawn, cfg)) g_fist_retry_at[side] = t + 0.5;
        }
    }

    // The game's own fist flags (fists_eating.md 1.1 / 4, plan B): 'Fist R/L Spawned' hidden from
    // Willie's tick, whose removal fires on '!Hand Active' (always in VR), and set after it while the
    // mod's fist is in that hand: his hit rules read it (Deal Complex Damage @22681: the punching arm
    // with Aim Swing Speed >= 60). 'R/L Fisting Rate' (punch stamina): the game's own formula, target
    // MRC(Aim Swing Speed, 25..50 -> Thrust Alpha..1), FInterpTo speed 0.1 (UF @473829), which the
    // tick forces to 0 without 'Hand Active', so the mod keeps its own value and writes it after.
    double g_fisting_rate[2] = {0.0, 0.0};
    double g_fisting_t = -1.0;
    void fist_flags_before_tick(UObject* pawn)
    {
        if (!hsvr::config().pose_fist_game_flags) return;
        for (int side = 0; side < 2; ++side)
            if (is_fist(object_property(pawn, side == 0 ? STR("Weapon L") : STR("Weapon R")))) set_bool_property(pawn, fist_spawned_name(side), false);
    }
    void fist_flags_after_tick(UObject* pawn)
    {
        if (!hsvr::config().pose_fist_game_flags) return;
        double now = hsvr::now_seconds(), dt = g_fisting_t > 0.0 ? (std::min)(0.1, now - g_fisting_t) : 0.0;
        g_fisting_t = now;
        double ass = read_number_variable(pawn, STR("Aim Swing Speed"));
        for (int side = 0; side < 2; ++side)
        {
            bool fist = is_fist(object_property(pawn, side == 0 ? STR("Weapon L") : STR("Weapon R")));
            set_bool_property(pawn, fist_spawned_name(side), fist);
            if (!fist)
            {
                g_fisting_rate[side] = 0.0;
                continue;
            }
            double thrust = read_number_variable(pawn, side == 0 ? STR("L Thrust Alpha") : STR("R Thrust Alpha"));
            if (thrust < 0.0) thrust = 0.0;
            double u = (std::max)(0.0, (std::min)(1.0, (ass - 25.0) / 25.0));
            double target = thrust + (1.0 - thrust) * u;
            g_fisting_rate[side] += (target - g_fisting_rate[side]) * (std::min)(1.0, dt * 0.1);
            set_variable(pawn, side == 0 ? STR("L Fisting Rate") : STR("R Fisting Rate"), g_fisting_rate[side], nullptr, nullptr);
        }
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
        static bool last_pawn_tracked = track_freed(&last_pawn);
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
        for (int side = 0; side < 2; ++side)
            g_fist_side[side].store(pawn && is_fist(object_property(pawn, side == 0 ? STR("Weapon L") : STR("Weapon R"))));
        // Fingers every tick the body is posed, even with the controllers idle (test 10: behind the grab
        // gate below, an unfaked fake headset never reached it).
        if (pawn && g_pose_active.load()) fingers_track_tick(cfg, state, pawn);
        if (!cfg.pose_grab_button || !pawn || !mesh || (!faked && (!state.active || !state.focused)) || !g_pose_active.load())
        {
            if (held[0] || held[1])
                HSVR_LOG_EVERY(2.0, "Grab: skipped a tick with a grip held (pawn %d, mesh %d, faked %d, controllers active %d focused %d, body posed %d)", pawn != nullptr,
                               mesh != nullptr, (int)faked, (int)state.active, (int)state.focused, (int)g_pose_active.load());
            return;
        }
        keep_brought_object(pawn, mesh);
        fist_trace(pawn);
        body_grab_tick(pawn, cfg);
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
            if (!now_held && scabbard_held(side))
            {
                set_anim_grip(side, -1);
                scabbard_let_go(side, "grip released");
                continue;
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
                if (mine && cfg.pose_grab_release_drops && holster_snap(side, pawn, mesh, mine, cfg)) continue;
                if (mine && cfg.pose_grab_release_drops)
                {
                    bool throw_it = cfg.pose_throw && g_hand_world_v[side] >= cfg.pose_throw_speed && !is_fist(mine);
                    drop_weapon(side, pawn, throw_it);
                    HSVR_LOG("Grab: %s grip let go: %s %ls (hand %.1f m/s)", hand, throw_it ? "threw" : "dropped", mine->GetName().c_str(), g_hand_world_v[side]);
                }
                else if (!mine)
                    release_object(side, pawn);
                continue;
            }
            if (!mine && bool_property(pawn, side == 0 ? STR("Grabbed L") : STR("Grabbed R"))) continue; // already holds an object
            if (mine || menu_up) continue;
            if (cfg.pose_grab_handles ? try_second_hand(side, pawn, mesh, cfg) : try_two_hand(side, pawn, mesh, cfg)) continue;
            Vec3 at = hand_grab_point(mesh, side);
            if (cfg.pose_draw && try_draw(side, pawn, mesh, at, cfg)) continue;
            if (try_scabbard_grab(side, pawn, mesh, at, cfg)) continue;
            // His arm can be too short for where your hand is (reaching to the floor: his crouch stops at
            // 55 cm and he does not bend forward, so his hand stayed 20 cm above your controller on the
            // floor, playtest 14:31). Then items are looked for at your controller, and what you touch there
            // is pulled up into his hand (the grab's settle) instead of never being touched.
            Vec3 palm_drawn = at;
            if (cfg.pose_grab_from_controller && g_pose.hand_valid[side])
            {
                Vec3 hand_bone = socket_location(mesh, bone_fname(side == 0 ? "hand_l" : "hand_r"));
                double held_back = length(g_pose.reach_target[side] - hand_bone);
                if (held_back > cfg.pose_grab_controller_gap_cm && length(g_pose.raw_target[side] - hand_bone) <= cfg.pose_grab_controller_max_cm)
                {
                    at = at + (g_pose.raw_target[side] - hand_bone);
                    HSVR_LOG("Grab: %s hand is held %.1f cm short of your hand by his reach: looking at your controller (%.0f %.0f %.0f)", hand, held_back, at.x,
                             at.y, at.z);
                }
            }
            std::vector<UObject*> weapons, willies;
            UObjectGlobals::FindAllOf(STR("ModularWeaponBP_C"), weapons);
            UObjectGlobals::FindAllOf(STR("Willie_BP_C"), willies);
            UObject* best = nullptr;
            double best_d = cfg.pose_grab_handles ? cfg.pose_grab_touch_cm : cfg.pose_grab_radius_cm;
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
                // Nothing to take: the hand closes into a fist (pose_fist_from_fingers: only when the
                // whole hand closes, fists_from_fingers).
                if (!cfg.pose_fist_from_fingers) make_fist(side, pawn, cfg);
                HSVR_LOG("Grab: %s grip squeezed, no loose weapon within %.0f cm (%d loose of %d; nearest %ls at %.0f cm; hand at %.0f %.0f %.0f)", hand,
                         cfg.pose_grab_handles ? cfg.pose_grab_touch_cm : cfg.pose_grab_radius_cm, looked, (int)weapons.size(), nearest ? nearest->GetName().c_str() : STR("none"), nearest ? nearest_d : -1.0,
                         at.x, at.y, at.z);
                continue;
            }
            if (cfg.pose_grab_handles)
            {
                // B&S: the hand touched it; it lands in the hand on the nearest handle (lined up with
                // the hand's grip line), or where the hand touched it when no handle is near.
                UObject* hb = object_property(best, STR("BaseMesh"));
                if (hb)
                {
                    Vec3 w0_pos = component_location(hb), touch;
                    Quat w0_rot = component_rotation(hb);
                    if (closest_on_weapon(best, at, &touch) < 0.0) touch = at;
                    const std::vector<Handle>& hs = item_handles(best, hb);
                    double t = 0.0, hd = 1e9;
                    int hi = nearest_handle(hs, w0_rot.inverse().rotate(at - w0_pos), t, hd);
                    // B&S: a weapon is taken only by a handle (its grip, a haft stretch, a sword's blade);
                    // shields (straps or anywhere on the rim) and bottles still go where the hand touched.
                    if (cfg.pose_grab_weapon_handles_only && !(hi >= 0 && hd <= cfg.pose_grab_handle_cm) && grab_needs_handle(best))
                    {
                        HSVR_LOG("Grab: %s hand touched %ls off its handles (nearest %s %.1f cm away, needs %.0f): not taken", hand, best->GetName().c_str(),
                                 hi >= 0 ? hs[hi].name.c_str() : "none", hd, cfg.pose_grab_handle_cm);
                        continue;
                    }
                    bool ok = give_hand_weapon(side, pawn, best);
                    UObject* now = object_property(pawn, side == 0 ? STR("Weapon L") : STR("Weapon R"));
                    if (ok && now == best)
                    {
                        if (hi >= 0 && hd <= cfg.pose_grab_handle_cm)
                        {
                            g_pending_free[side] = handle_grip(side, mesh, w0_pos, w0_rot, palm_drawn, hs[hi], t);
                            g_pending_item[side] = best;
                            g_pending_free[side].hold_at = cfg.pose_grab_settle_s > 0.0;
                            g_pending_free[side].at_pos = w0_pos;
                            g_pending_free[side].at_rot = w0_rot;
                            g_pending_free[side].place = !g_pending_free[side].hold_at;
                            set_component_transform(hb, w0_pos, w0_rot, true);
                            HSVR_LOG("Grab: %s hand took %ls by its %s at %.0f%% along it (%.1f cm from the hand, touched at %.1f cm)", hand, best->GetName().c_str(),
                                     hs[hi].name.c_str(), t * 100.0, hd, best_d);
                        }
                        else
                        {
                            hold_where_touched(side, mesh, best, hb, w0_pos, w0_rot, palm_drawn, touch, "spot where the hand touched it", nullptr, true);
                            g_pending_free[side].grip_type = 5;
                        }
                    }
                    controllers_buzz(side, 0.4f);
                    HSVR_LOG("Grab: %s grip took %ls (%.1f cm from the hand): %s, hand now holds %ls", hand, best->GetName().c_str(), best_d,
                             ok ? "set up" : "set-up call missing", now ? now->GetName().c_str() : STR("nothing"));
                    continue;
                }
            }
            // Shields: where the hand touched it, before the game's set-up snaps it to its own grip.
            bool is_shield = class_name_starts(best, STR("Shield"));
            bool shield = cfg.pose_shield_free_grip && is_shield;
            bool loose_free = !is_shield && cfg.pose_weapon_free_grip;
            UObject* best_base = object_property(best, STR("BaseMesh"));
            Vec3 w0_pos, touch;
            Quat w0_rot;
            if ((shield || loose_free) && best_base)
            {
                w0_pos = component_location(best_base);
                w0_rot = component_rotation(best_base);
                if (closest_on_weapon(best, at, &touch) < 0.0) touch = at;
            }
            bool ok = give_hand_weapon(side, pawn, best);
            UObject* now = object_property(pawn, side == 0 ? STR("Weapon L") : STR("Weapon R"));
            if (shield && best_base && ok && now == best)
                shield_grip(side, mesh, best, best_base, w0_pos, w0_rot, at, touch, cfg);
            if (loose_free && best_base && ok && now == best)
            {
                // A weapon's BaseMesh is its grip dummy, at the game's grip point (collision dump):
                // taken farther than pose_weapon_free_cm from it (by the blade, the haft, the tip),
                // it is held where the hand closed, not snapped onto its grip (test 7de-J: an 80 cm
                // jump and a quarter turn when taken by the tip).
                double off = length(w0_rot.inverse().rotate(at - w0_pos));
                if (off > cfg.pose_weapon_free_cm)
                    hold_where_touched(side, mesh, best, best_base, w0_pos, w0_rot, at, touch, "spot where the hand closed on it");
                else
                    HSVR_LOG("Grab: %s hand took %ls by its grip (%.1f cm from the game's grip point)", hand, best->GetName().c_str(), off);
            }
            controllers_buzz(side, 0.4f);
            HSVR_LOG("Grab: %s grip took %ls (%.1f cm from the hand): %s, hand now holds %ls", hand, best->GetName().c_str(), best_d,
                     ok ? "set up" : "set-up call missing", now ? now->GetName().c_str() : STR("nothing"));
        }
        fists_from_fingers(cfg, state, pawn, menu_up);
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
        // Not while drawing or sheathing: there the game's hand joint helps pull the blade along the
        // scabbard toward your hand (test b3: with the hand on the weapon a draw took 6.5 s, not 2.4).
        bool follows = cfg.pose_hold_hand_follows && (!g_draw[side].active || cfg.pose_hold_hand_follows_draw);
        if (follows) t = 1.0; // B&S: the drawn hand is always on the item
        h.on_weapon = t;
        if (t <= 0.0) return false;
        // The stuck rule (Matthew's framework, hold_melee): a weapon stopped with its grip out of his
        // reach from the shoulder, or your hand pulled more than an arm's length from it, is let go
        // rather than dragged through or snapped back. Held for pose_hold_let_go_s first, so a glancing knock
        // never drops it.
        // Stuck in someone, B&S's own rule (PlayerHand, StuckBehaviour.Drop, also while penetrating):
        // the hand lets go only once the grip is past arm's length from the shoulder; how far your
        // real hand is does not count (the wound holds the blade, and pulling it out is yours to do).
        bool in_body = bool_property(h.weapon, STR("Stuck In Body"));
        if (cfg.pose_hold_let_go && t >= 0.99 && g_pose.driver && !g_draw[side].active)
        {
            const ArmCalibration& a = g_pose.arm[side];
            double reach = a.upper_length + a.lower_length;
            Vec3 shoulder = socket_location(g_pose.driver, bone_fname(side == 0 ? "upperarm_l" : "upperarm_r"));
            Vec3 grip_at = pos + rot.rotate(h.pivot);
            Vec3 hand_point = target + hand_rot.rotate(h.in_hand);
            double from_shoulder = length(grip_at - shoulder) - length(h.in_hand);
            double from_hand = length(grip_at - hand_point);
            bool out = from_shoulder > reach + 2.0 || (!in_body && from_hand > reach);
            if (in_body && !stuck_is_thrust(h.weapon)) out = false; // a lodged cut: cut_through_tick decides
            if (in_body && cfg.stab_drag && stuck_is_thrust(h.weapon))
            {
                // Dragging a body: the weapon stays in the hand; it slips out only when the hand has
                // pulled with all its strength, the grip still past arm's length, for longer than the
                // grip holds (stab_drag_grip_s x his strength: tired, hurt or weak hands let go sooner).
                out = false;
                double tn = now_seconds();
                // Past what his arm can follow: the blade comes out of the body and stays in the hand
                // (Matthew 2026-11-20: a stuck weapon must never fly off away from his hand).
                if (from_shoulder > reach + cfg.stab_drag_reach_slack_cm)
                {
                    if (h.drag_slip_since < 0.0) h.drag_slip_since = tn;
                    if (tn - h.drag_slip_since >= cfg.stab_drag_reach_s)
                    {
                        HSVR_LOG("Hold: %s hand pulls %ls out at arm's length (grip %.0f cm past reach, strength %.0f%%)", side == 0 ? "left" : "right",
                                 h.weapon->GetName().c_str(), from_shoulder - reach, h.body_strength * 100.0);
                        h.drag_slip_since = -1.0;
                        Call r(h.weapon, STR("Release All Jammed Bones"));
                        r.run();
                    }
                }
                else
                    h.drag_slip_since = -1.0;
            }
            // How far the hand is out along the wound (pommel side), for the yank: a pull sideways
            // drags him instead.
            double out_along = from_hand;
            if (in_body && cfg.stab_drag)
                if (UObject* jt = object_property(h.weapon, STR("Jammed Tip")))
                {
                    Call f(jt, STR("GetForwardVector"));
                    f.run();
                    Vec3 fw = f.vec();
                    if (length(fw) > 0.5) out_along = -dot(hand_point - grip_at, fw * (1.0 / length(fw)));
                }
            // Pulled hard out of a wound: your hand more than stab_yank_cm from the grip for
            // stab_yank_s tears the blade out (the game's own Release All Jammed Bones) and the hand
            // keeps it (flip-test1: the hand was 74 cm away while the blade stayed in him).
            static double yank_since[2] = {-1.0, -1.0};
            if (in_body && cfg.stab_yank_cm > 0.0 && out_along > cfg.stab_yank_cm)
            {
                double tn = now_seconds();
                if (yank_since[side] < 0.0) yank_since[side] = tn;
                else if (tn - yank_since[side] >= cfg.stab_yank_s)
                {
                    yank_since[side] = -1.0;
                    Call r(h.weapon, STR("Release All Jammed Bones"));
                    r.run();
                    HSVR_LOG("Hold: %s hand pulls %ls out of the wound (hand %.0f cm from the grip)", side == 0 ? "left" : "right", h.weapon->GetName().c_str(), from_hand);
                }
            }
            else
                yank_since[side] = -1.0;
            // A pull that the yank will answer is not a let-go (flip-test3: the past-reach let-go fired
            // after 0.15 s and dropped the weapon before the 0.25 s yank could pull it out).
            if (in_body && cfg.stab_yank_cm > 0.0 && out_along > cfg.stab_yank_cm) out = false;
            // Not right after it came into the hand (a new or long weapon swings into place first),
            // and not while it is still closing in on where the hold sends it.
            double t_now = now_seconds();
            bool touching = t_now - h.last_contact < 0.15;
            bool settling = h.started < 0.0 || t_now - h.started < cfg.pose_hold_let_go_grace_s || (!in_body && (coming > 0.3 || !touching));
            if (!out || settling) h.stuck_since = -1.0;
            else
            {
                double now = now_seconds();
                if (h.stuck_since < 0.0) h.stuck_since = now;
                else if (now - h.stuck_since >= cfg.pose_hold_let_go_s && cfg.pose_hold_let_go_ignore_self && h.self_checked_at != h.stuck_since)
                {
                    // Stopped by his own body (playtest 2026-11-18: 2 of 5 let-goes): not stuck in
                    // anything; it stays in the hand. Looked at once per stop.
                    h.self_checked_at = h.stuck_since;
                    std::wstring bone;
                    double d = own_body_contact(g_pose.pawn, h.weapon, 3.0, &bone, side);
                    h.self_blocked = d >= 0.0;
                    if (h.self_blocked)
                        HSVR_LOG("Hold: %s hand keeps %ls: it is stopped by his own %ls (%.1f cm), not stuck in anything", side == 0 ? "left" : "right",
                                 h.weapon->GetName().c_str(), bone.c_str(), d);
                }
                if (h.self_blocked && h.self_checked_at == h.stuck_since) {}
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
        if (follows)
        {
            // Where the item will be once this frame's physics has moved it (its velocity now, about its
            // centre of mass), and the hand at its grip there: hand = item x (the grip's place in the hand)^-1.
            Vec3 pos2 = pos;
            Quat rot2 = rot;
            predicted_item_pose(base, pos2, rot2);
            Vec3 controller_point = target + hand_rot.rotate(h.in_hand);
            hand_rot = (rot2 * (h.f2 * h.f1.inverse()).normalized().inverse()).normalized();
            target = pos2 + rot2.rotate(h.pivot) - hand_rot.rotate(h.in_hand);
            // What you saw last frame: his drawn hand bone against the item's grip as it was drawn.
            if (g_pose.driver)
            {
                Quat seen_rot = (rot * (h.f2 * h.f1.inverse()).normalized().inverse()).normalized();
                Vec3 seen_at = pos + rot.rotate(h.pivot) - seen_rot.rotate(h.in_hand);
                FName bn = bone_fname(side == 0 ? "hand_l" : "hand_r");
                Vec3 drawn = socket_location(g_pose.driver, bn);
                Quat drawn_rot = socket_rotation(g_pose.driver, bn);
                HSVR_LOG_EVERY(0.5, "Hold: %s drawn hand %.1f cm / %.0f deg from its grip on %ls; item %.1f cm from your controller", side == 0 ? "left" : "right",
                               length(drawn - seen_at), length(scaled_axis((drawn_rot * seen_rot.inverse()).normalized())) * 57.2958, h.weapon->GetName().c_str(),
                               length((pos + rot.rotate(h.pivot)) - controller_point));
            }
            return true;
        }
        // The hand where it holds the weapon, moved and turned as the weapon was kept from following.
        Quat dr = from_scaled_axis(turn * t);
        Vec3 point = target + hand_rot.rotate(h.in_hand) + off * t;
        hand_rot = (dr * hand_rot).normalized();
        target = point - hand_rot.rotate(h.in_hand);
        return true;
    }

    // Walks a weapon's axis (BaseMesh +Z through the grip point) both ways from the grip, in 2 cm
    // steps, asking its real parts (closest_on_weapon): a step is on the weapon while the axis point
    // is inside (or within 3 cm of) a part (grip points sit 1-3 cm off some hafts' surface), and wide when, seen from 12 cm out on any of four sides,
    // a part level with the step reaches out further than wide_cm from the axis. lo/hi = the last step on the weapon and not wide, each
    // way; wide_lo/hi say whether it stopped at a wide part (else at the weapon's end). False when
    // the grip itself is not inside the parts. Cached per weapon.
    struct AxisMeasure
    {
        bool ok;
        double lo, hi;
        bool wide_lo, wide_hi;
        double at_z;
        std::vector<std::pair<double, double>> extra; // further haft stretches above the first stop (hafted weapons)
        bool guard_hi = false; // the first stop above the grip is a Guard part (a sword: its blade is a handle)
    };
    std::unordered_map<UObject*, AxisMeasure> g_axis_measures;
    bool measure_weapon_axis(UObject* weapon, UObject* base, const hsvr::Vec3& pivot, double wide_cm, double tip, double& lo, double& hi, bool& wide_lo,
                             bool& wide_hi, std::vector<std::pair<double, double>>* extra = nullptr)
    {
        using namespace hsvr;
        auto cached = g_axis_measures.find(weapon);
        if (cached == g_axis_measures.end())
        {
            AxisMeasure m{false, 0.0, 0.0, false, false, pivot.z};
            Vec3 bp = component_location(base);
            Quat br = component_rotation(base);
            Vec3 side[4] = {br.rotate({1, 0, 0}), br.rotate({-1, 0, 0}), br.rotate({0, 1, 0}), br.rotate({0, -1, 0})};
            const double out = 12.0, step = 2.0;
            const Config& hc = hsvr::config();
            // 0 = on the weapon and narrow, 1 = wide, 2 = off the weapon; width = how far the widest part
            // level with the step reaches from the axis (cm), part = that part.
            struct Probe { int state; double width; UObject* part; double off; };
            auto probe = [&](double z) {
                Vec3 a = bp + br.rotate({pivot.x, pivot.y, pivot.z + z});
                Probe r{0, 0.0, nullptr, 0.0};
                double d = closest_on_weapon(weapon, a);
                r.off = d;
                if (d < 0.0 || d > 3.0) { r.state = 2; return r; }
                // The nearest point of the parts seen from 12 cm out: wide only when that point is level
                // with this step (a guard a few cm further along would otherwise read as wide here).
                for (const Vec3& dir : side)
                {
                    Vec3 q;
                    UObject* part = nullptr;
                    double e = closest_on_weapon(weapon, a + dir * out, &q, &part);
                    if (e < 0.0) continue;
                    Vec3 l = br.inverse().rotate(q - a);
                    double w = std::sqrt(l.x * l.x + l.y * l.y);
                    if (std::fabs(l.z) <= step * 0.75 && w > r.width) { r.width = w; r.part = part; }
                }
                if (r.width > wide_cm) r.state = 1;
                return r;
            };
            auto part_name = [](UObject* p) { return p ? p->GetName() : std::wstring(STR("?")); };
            double bottom = 0.0;
            if (FProperty* bpz = weapon->GetPropertyByNameInChain(STR("Length to Bottom"))) bottom = number_property(weapon, bpz);
            if (probe(0.0).state != 2)
            {
                m.ok = true;
                double limit = (std::max)(300.0, tip + 20.0);
                for (int dirn = -1; dirn <= 1; dirn += 2)
                {
                    double last = 0.0;
                    bool wide = false;
                    for (double z = step; z <= limit; z += step)
                    {
                        Probe r = probe(dirn * z);
                        if (r.state == 0)
                        {
                            last = z;
                            continue;
                        }
                        // A short wide bit (a ring, a langet's end) or a short gap between parts, with narrow
                        // haft past it, is crossed (pose_slide_ring_*); a guard, a rondel or a head is not.
                        double run_w = r.state == 1 ? r.width : 0.0;
                        UObject* run_part = r.part;
                        bool any_off = r.state == 2, resumed = false;
                        double z2 = z;
                        double scan = (std::max)(hc.pose_slide_ring_max_len_cm, hc.pose_slide_gap_max_cm) + step;
                        // Below the grip, the game's own bottom length says how far the weapon goes on.
                        if (dirn < 0 && hc.pose_slide_butt_is_haft && bottom > z + step) scan = (std::max)(scan, (std::min)(bottom - z, 40.0));
                        for (; z2 <= z + scan; z2 += step)
                        {
                            Probe r2 = z2 == z ? r : probe(dirn * z2);
                            if (r2.state == 0) { resumed = true; break; }
                            if (r2.state == 1 && r2.width > run_w) { run_w = r2.width; run_part = r2.part; }
                            any_off |= r2.state == 2;
                        }
                        double run_len = z2 - z;
                        bool hafted = last >= 30.0;
                        bool rondel = hafted && run_part && part_name(run_part).find(STR("Guard")) != std::wstring::npos;
                        bool butt = dirn < 0 && resumed && bottom > z2 && hc.pose_slide_butt_is_haft; // a pollaxe butt cap: 9 cm wide, haft past it
                        bool cross = hc.pose_slide_ring_max_len_cm > 0.0 && resumed && !(rondel && hc.pose_slide_rondel_stops) &&
                                     (butt || (run_len <= (any_off ? hc.pose_slide_gap_max_cm : hc.pose_slide_ring_max_len_cm) &&
                                               run_w <= hc.pose_slide_ring_max_width_cm));
                        if (cross)
                        {
                            HSVR_LOG("Haft: %ls %s %.0f..%.0f cm (%.0f cm long, %.1f cm from the axis, part %ls%s): crossed", weapon->GetName().c_str(),
                                     any_off ? "gap" : "wide bit", dirn * z, dirn * z2, run_len, run_w, part_name(run_part).c_str(), butt ? ", butt end" : "");
                            last = z2;
                            z = z2;
                            continue;
                        }
                        HSVR_LOG("Haft: %ls stops at %.0f cm: %s (%.1f cm from the axis, part %ls, %.0f cm long%s%s; parts nearest %.1f cm)",
                                 weapon->GetName().c_str(), dirn * z, r.state == 1 ? "wide" : "off the parts", run_w, part_name(run_part).c_str(), run_len,
                                 resumed ? ", narrow again past it" : "", rondel ? ", a rondel" : "", r.off);
                        wide = r.state == 1;
                        if (dirn > 0) m.guard_hi = wide && run_part && part_name(run_part).find(STR("Guard")) != std::wstring::npos;
                        break;
                    }
                    // A wide part stops the hand at the part itself (its near edge, a step past the last narrow one).
                    if (dirn < 0) { m.lo = -(wide ? last + step * 0.5 : last); m.wide_lo = wide; }
                    else { m.hi = wide ? last + step * 0.5 : last; m.wide_hi = wide; }
                }
                // A hafted weapon (a grip stretch of 30 cm or more) goes on past its first stop: every further
                // narrow stretch of 15 cm or more on its axis is haft too (a pollaxe above its rondel, a war
                // staff above its fitting); the stops between them stay stops (B&S: grab only on a handle).
                if (m.hi - m.lo >= 30.0)
                {
                    double z = m.hi + step;
                    while (z <= limit)
                    {
                        while (z <= limit && probe(z).state != 0) z += step;
                        if (z > limit) break;
                        double s0 = z, last = z;
                        while (z <= limit && probe(z).state == 0)
                        {
                            last = z;
                            z += step;
                        }
                        // 15 cm: a pollaxe's top spike (14) and a halberd's (12) are not haft; nor is a stretch that
                        // runs up to the weapon's tip (a spearhead, a top spike: heads are not handles).
                        bool at_tip = tip > 0.0 && last + step * 0.5 >= tip - 10.0;
                        if (at_tip) HSVR_LOG("Haft: %ls %.0f .. %.0f cm runs to its tip (%.0f): its head, not haft", weapon->GetName().c_str(), s0, last, tip);
                        if (last - s0 >= 15.0 && !at_tip)
                        {
                            m.extra.push_back({s0 - step * 0.5, last + step * 0.5});
                            HSVR_LOG("Haft: %ls more haft at %.0f .. %.0f cm", weapon->GetName().c_str(), s0 - step * 0.5, last + step * 0.5);
                        }
                    }
                }
            }
            // A failed or zero-length measure (its parts not found yet) is not kept: asked again next time.
            if (!m.ok || (m.lo == 0.0 && m.hi == 0.0))
            {
                lo = m.lo + m.at_z - pivot.z;
                hi = m.hi + m.at_z - pivot.z;
                wide_lo = m.wide_lo;
                wide_hi = m.wide_hi;
                return m.ok;
            }
            cached = g_axis_measures.emplace(weapon, m).first;
        }
        const AxisMeasure& m = cached->second;
        // Measured from the point it was first asked about; given from this one.
        lo = m.lo + m.at_z - pivot.z;
        hi = m.hi + m.at_z - pivot.z;
        wide_lo = m.wide_lo;
        wide_hi = m.wide_hi;
        if (extra)
        {
            extra->clear();
            for (const auto& e : m.extra) extra->push_back({e.first + m.at_z - pivot.z, e.second + m.at_z - pivot.z});
        }
        return m.ok;
    }

    bool weapon_has_guard(UObject* weapon)
    {
        auto f = g_axis_measures.find(weapon);
        return f != g_axis_measures.end() && f->second.guard_hi;
    }

    std::unordered_map<UObject*, std::vector<Handle>> g_item_handles;
    std::wstring shield_class(UObject* shield);
    const std::vector<Handle>& item_handles(UObject* item, UObject* base)
    {
        using namespace hsvr;
        auto found = g_item_handles.find(item);
        if (found != g_item_handles.end()) return found->second;
        const Config& cfg = hsvr::config();
        std::vector<Handle> hs;
        double half = cfg.pose_slide_hand_half_cm;
        bool measured = true; // false: the weapon's parts were not found (yet)
        if (class_name_starts(item, STR("Shield")))
        {
            std::wstring cls = shield_class(item);
            for (const ShieldFitting& f : k_shield_fittings)
            {
                if (cls != f.shield) continue;
                Vec3 c{f.cx, f.cy, f.cz}, a = normalized(Vec3{f.ax, f.ay, f.az});
                double h = (std::max)(0.0, f.length * 0.5 - half);
                hs.push_back(Handle{c - a * h, c + a * h, f.name, 5, 5});
            }
        }
        else
        {
            double lo = 0.0, hi = 0.0, top = 0.0;
            bool wide_lo = false, wide_hi = false;
            if (FProperty* p = item->GetPropertyByNameInChain(STR("Length to Top R Default"))) top = number_property(item, p);
            std::vector<std::pair<double, double>> extra;
            bool ok = measure_weapon_axis(item, base, Vec3{}, cfg.pose_slide_wide_cm, top, lo, hi, wide_lo, wide_hi, &extra);
            measured = ok && !(lo == 0.0 && hi == 0.0);
            if (ok)
            {
                double g0 = (std::min)(0.0, lo + half), g1 = (std::max)(0.0, hi - half);
                hs.push_back(Handle{Vec3{0, 0, g0}, Vec3{0, 0, g1}, "grip", -1, 2});
                bool hafted = hi - lo >= 30.0;
                if (hafted)
                {
                    // Every further haft stretch is a handle of its own; the head is not (B&S).
                    for (const auto& e : extra)
                        if (e.second - e.first > 2.0 * half + 1.0) hs.push_back(Handle{Vec3{0, 0, e.first + half}, Vec3{0, 0, e.second - half}, "haft", 2, 2});
                }
                else
                {
                    // Only a sword's blade (past a Guard) is a handle (half-swording); an axe's or a tool's head is not.
                    double b0 = hi + half + cfg.pose_two_hand_blade_gap_cm, b1 = top - half;
                    if (wide_hi && weapon_has_guard(item) && top > 0.0 && b1 > b0 + 2.0) hs.push_back(Handle{Vec3{0, 0, b0}, Vec3{0, 0, b1}, "blade", 5, 5});
                }
            }
            else
                hs.push_back(Handle{Vec3{}, Vec3{}, "grip", -1, 2});
        }
        std::string list;
        for (const Handle& h : hs)
        {
            char one[160];
            std::snprintf(one, sizeof(one), "%s%s (%.1f, %.1f, %.1f)..(%.1f, %.1f, %.1f)", list.empty() ? "" : "; ", h.name.c_str(), h.a.x, h.a.y, h.a.z, h.b.x, h.b.y, h.b.z);
            list += one;
        }
        // A weapon whose parts were not found yet (spawned straight into the hand) is measured again next
        // time, up to 40 times; a grip that is really this short (fists, a short dagger grip) is kept.
        static std::unordered_map<UObject*, int> unsure_tries;
        static bool unsure_tries_tracked = on_freed([] { erase_gone_keys(unsure_tries); });
        if (!measured && ++unsure_tries[item] <= 40)
        {
            if (unsure_tries[item] == 1)
                HSVR_LOG("Handles: %ls: its parts are not found yet (%s), measuring again", item->GetName().c_str(), list.empty() ? "none" : list.c_str());
            static std::unordered_map<UObject*, std::vector<Handle>> unsure;
            static bool unsure_tracked = on_freed([] { erase_gone_keys(unsure); });
            return unsure[item] = std::move(hs);
        }
        HSVR_LOG("Handles: %ls: %s%s", item->GetName().c_str(), list.empty() ? "none (held where touched)" : list.c_str(),
                 unsure_tries.count(item) ? " (measured again after its parts appeared)" : "");
        return g_item_handles.emplace(item, std::move(hs)).first->second;
    }

    int nearest_handle(const std::vector<Handle>& hs, const hsvr::Vec3& local, double& t, double& dist)
    {
        using namespace hsvr;
        int best = -1;
        dist = 1e9;
        for (int i = 0; i < (int)hs.size(); ++i)
        {
            Vec3 ab = hs[i].b - hs[i].a;
            double len2 = dot(ab, ab);
            double u = len2 > 1e-6 ? (std::min)(1.0, (std::max)(0.0, dot(local - hs[i].a, ab) / len2)) : 0.0;
            double d = length(local - (hs[i].a + ab * u));
            if (d < dist)
            {
                dist = d;
                best = i;
                t = u;
            }
        }
        return best;
    }

    int clamp_grip_type(int side, int g) { return side == 0 && g >= 10 && g <= 15 ? 2 : g; } // left 10-15 = no curl

    // A hand's grip on a handle: the handle's point at t in the palm, its line along the hand's grip
    // line (turned the least from how the item was), the item turned as it was otherwise.
    FreeGrip handle_grip(int side, UObject* mesh, const hsvr::Vec3& w0_pos, const hsvr::Quat& w0_rot, const hsvr::Vec3& palm, const Handle& hd, double t)
    {
        using namespace hsvr;
        Vec3 local = hd.a + (hd.b - hd.a) * t;
        Vec3 axis_local = length(hd.b - hd.a) > 1e-3 ? normalized(hd.b - hd.a) : Vec3{0, 0, 1};
        Vec3 axis = w0_rot.rotate(axis_local);
        FreeGrip g = make_free_grip(side, mesh, w0_pos, w0_rot, palm, w0_pos + w0_rot.rotate(local), &axis, hd.name.c_str());
        g.has_handle = length(hd.b - hd.a) > 1e-3;
        g.ha = hd.a;
        g.hb = hd.b;
        g.t = t;
        g.handle = hd.name;
        g.grip_type = clamp_grip_type(side, side == 1 ? hd.grip_r : hd.grip_l);
        if (hsvr::config().pose_grab_seat && (hd.name == "grip" || hd.name == "blade"))
        {
            // B&S: an item sits in the hand in one of a few fixed orientations, the one nearest to how
            // it was. Here: the game's own seat (weapon_rSocket / weapon_lSocket in the hand bone, the same
            // for every ordinary weapon; seated_orientation.md), its blade either way along the grip line
            // and its edge either way (the game's reverse and Mordhau seats are these turned 180). The
            // handle's line goes through the seat's point, where the fingers close.
            double mx = side == 0 ? -1.0 : 1.0;
            Vec3 sz = normalized(Vec3{-0.495 * mx, 0.861, 0.113});
            Vec3 sx = Vec3{0.866 * mx, 0.500, -0.013};
            sx = normalized(sx - sz * dot(sx, sz));
            Quat seat = quat_from_axes(sx, cross(sz, sx), sz);
            Quat a_rot = socket_rotation(mesh, bone_fname(side == 0 ? "hand_l" : "hand_r"));
            Quat natural = (a_rot.inverse() * w0_rot).normalized();
            const Quat flips[4] = {Quat{0, 0, 0, 1}, Quat{0, 0, 1, 0}, Quat{1, 0, 0, 0}, Quat{0, 1, 0, 0}};
            const char* flip_name[4] = {"as the game seats it", "edge turned", "blade reversed", "blade reversed, edge turned"};
            int best = 0;
            double best_a = 1e9, best_cost = 1e9;
            for (int i = 0; i < 4; ++i)
            {
                Quat c = (seat * flips[i]).normalized();
                double d = std::fabs(c.x * natural.x + c.y * natural.y + c.z * natural.z + c.w * natural.w);
                double ang = 2.0 * std::acos((std::min)(1.0, d));
                // A reversed blade (pommel up) only when clearly nearer: a sword picked up is held blade up.
                double cost = ang + (i >= 2 ? hsvr::config().pose_grab_seat_reverse_bias_deg * 3.14159265358979 / 180.0 : 0.0);
                if (cost < best_cost)
                {
                    best_cost = cost;
                    best_a = ang;
                    best = i;
                }
            }
            g.rel = (seat * flips[best]).normalized();
            g.in_anchor = Vec3{-7.25 * mx, 1.34, -2.87};
            HSVR_LOG("Grab: %s hand seats it %s (%.0f deg from how it was)", side == 0 ? "left" : "right", flip_name[best], best_a * 180.0 / 3.14159265358979);
        }
        return g;
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
        // Measured from the real parts (billhook, playtest 2026-11-18: its Head module starts 10 cm
        // above the hand and "Length to Bottom" is 20 cm, but the haft runs 56+ cm below the grip).
        if (cfg.pose_slide_zone_measured)
        {
            double m_lo = 0.0, m_hi = 0.0;
            bool wide_lo = false, wide_hi = false;
            if (measure_weapon_axis(weapon, base, h.pivot, cfg.pose_slide_wide_cm, top, m_lo, m_hi, wide_lo, wide_hi))
            {
                HSVR_LOG("Slide: %ls measured along its axis: %s %.1f cm below the grip, %s %.1f cm above (game's data: pommel end %.1f, %s at %.1f)",
                         weapon->GetName().c_str(), wide_lo ? "wide part" : "end", -m_lo, wide_hi ? "wide part" : "end", m_hi, bottom,
                         how, stop < 1e8 ? stop : 0.0);
                bottom = -m_lo;
                if (wide_hi)
                {
                    stop = m_hi;
                    how = "measured wide part";
                }
                else if (stop < 1e8 && stop > m_hi)
                {
                    stop = m_hi;
                    how = "measured end";
                }
            }
            else
                HSVR_LOG("Slide: %ls could not be measured along its axis (the grip is not inside its parts); the game's data is used", weapon->GetName().c_str());
        }
        double half = cfg.pose_slide_hand_half_cm;
        h.zone_lo = bottom > 0.0 ? (std::min)(0.0, -bottom + half) : 0.0;
        h.zone_hi = stop < 1e8 ? (std::max)(0.0, stop - half) : 0.0;
        h.zone_stop = stop < 1e8 ? stop : 0.0;
        h.zone_tip = top;
        HSVR_LOG("Slide: %ls grip zone %.1f .. %.1f cm from the game's grip (pommel end %.1f, top stop %.1f from the %s, tip %.1f)", weapon->GetName().c_str(),
                 h.zone_lo, h.zone_hi, -bottom, stop < 1e8 ? stop : 0.0, how, top);
        // No room at all (a weapon spawned into the hand before its parts were listed): look again in a
        // moment rather than keep a zero zone for as long as it is held (playtest 12:19, two-handed axe).
        if (h.zone_hi - h.zone_lo < 1.0)
        {
            static std::unordered_map<UObject*, int> tries;
            static bool tries_tracked = on_freed([] { erase_gone_keys(tries); });
            if (++tries[weapon] <= 20) h.zone_known = false;
        }
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
        // A fist (the game's invisible 13 cm sphere) has no handle to slide along.
        if (!cfg.pose_slide || h.forearm || is_fist(weapon)) return;
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
        double total = 1.0, strength = 1.0, injury = 1.0, stamina = 1.0, size = 1.0;
        double muscle_rate = -1.0, tonus = -1.0, consciousness = -1.0, stamina_value = -1.0;
    };

    // His arm's mass against a default creator body's (Height 0.5 -> scale 0.9375, Weight 0.5 -> Mass Scale
    // 1.125). Flat Half Sword's arm motors are the same for every body (acceleration-mode drives, CPA's
    // strength ratio Mass Scale / GetMassScale = 1), so a weapon slows a light arm more than a heavy one:
    // what differs between bodies is the arm's mass next to the weapon's. Arm mass = (upperarm 1.39 +
    // lowerarm 1.22 kg, from their volume, so x scale^3) + hand 1 kg (a fixed override), all x Mass Scale
    // (willie_physics_assets.md; Set Character Height: scale = MapRange(Height 0..1 -> 0.875..1);
    // Mass Scale (Set in BP) = MapRange(Muscle Rate 0..1 -> 1..1.25)). Range 0.78 (min/min) .. 1.27 (max/max).
    struct BodyDims
    {
        double scale = 0.9375, mass_scale = 1.125; // a default creator body until read
    };
    BodyDims g_body_dims;
    // His arm's inertia about the shoulder (kg m^2): upper arm 1.39 kg and forearm 1.22 kg (x scale^3) and
    // the 1 kg hand, all x Mass Scale, their centres at 0.15 / 0.40 / reach m from the shoulder for a default
    // body, x his size. Default body about 0.55, the smallest weakest about 0.30.
    double arm_inertia_shoulder(const BodyDims& d, double reach_m)
    {
        double k = d.scale / 0.9375, s3 = d.scale * d.scale * d.scale;
        double a = 0.15 * k, b = 0.40 * k, c = reach_m * k;
        return d.mass_scale * (1.39 * s3 * a * a + 1.22 * s3 * b * b + 1.0 * c * c);
    }
    double body_size_factor(UObject* pawn)
    {
        static UObject* last_pawn = nullptr;
        static bool last_pawn_tracked = track_freed(&last_pawn);
        static double last = 1.0;
        if (pawn == last_pawn) return last;
        double scale = -1.0, mass_scale = -1.0;
        if (FProperty* p = pawn->GetPropertyByNameInChain(STR("Character Scale (Set in BP)")))
        {
            if (p->GetClass().GetName() == STR("StructProperty"))
            {
                double v[3] = {0, 0, 0};
                read_three(reinterpret_cast<uint8_t*>(pawn) + p->GetOffset_Internal(), p, v);
                scale = v[0];
            }
        }
        if (FProperty* p = pawn->GetPropertyByNameInChain(STR("Mass Scale (Set in BP)"))) mass_scale = number_property(pawn, p);
        // Read too early (before the game sets them) they are 0: try again next time.
        if (!(scale > 0.3 && scale < 3.0) || !(mass_scale > 0.1 && mass_scale < 5.0)) return 1.0;
        auto arm = [](double sc, double ms) { return (2.61 * sc * sc * sc + 1.0) * ms; };
        double f = arm(scale, mass_scale) / arm(0.9375, 1.125);
        g_body_dims.scale = scale;
        g_body_dims.mass_scale = mass_scale;
        last_pawn = pawn;
        last = f;
        HSVR_LOG("Strength: body size: Character Scale %.4f, Mass Scale %.3f -> arm mass %.2f kg vs %.2f kg for a default body -> hold x%.2f", scale, mass_scale,
                 arm(scale, mass_scale), arm(0.9375, 1.125), f);
        return f;
    }
    BodyStrength body_strength(UObject* pawn, int side, const hsvr::Config& cfg)
    {
        BodyStrength b;
        if (!cfg.pose_body_strength || !pawn) return b;
        // Muscle Rate can be negative (the creator's lowest weight is -1), so "not found" is asked of the
        // property itself, not read off a -1 value (playtest 14:14: a weight -1 Willie got full grip strength).
        FProperty* muscle = pawn->GetPropertyByNameInChain(STR("Muscle Rate"));
        bool muscle_found = muscle != nullptr;
        b.muscle_rate = muscle ? number_property(pawn, muscle) : 0.0;
        b.tonus = read_number_variable(pawn, side == 0 ? STR("Arm L Tonus") : STR("Arm R Tonus"));
        b.consciousness = read_number_variable(pawn, STR("Consciousness"));
        b.stamina_value = read_number_variable(pawn, STR("Stamina"));
        if (muscle_found) b.strength = (std::max)(0.5, 1.0 + 0.5 * b.muscle_rate) / 1.25;
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
        // With the arm-mass factor on, Muscle Rate does not scale the hold: in flat Half Sword the arm motors
        // are the same for every body and muscle acts through the arm's mass (already in size) and through
        // the hand joint's slip threshold (x1..1.5). The mod has no force-based slip of its own (the game's
        // reads its hand joint, which the hold leaves free), so that part is only logged. Off: as before.
        if (cfg.pose_body_size_strength) b.size = body_size_factor(pawn);
        double on_hold = cfg.pose_body_size_strength ? b.size : b.strength;
        b.total = (std::max)(cfg.pose_body_strength_min, on_hold * (std::min)(b.injury, b.stamina));
        static bool reported = false;
        if (!reported)
        {
            reported = true;
            HSVR_LOG("Strength: Muscle Rate %.2f, Arm %s Tonus %.3f, Consciousness %.0f, Stamina %.1f (tonus, consciousness, stamina -1 = not found; Muscle Rate %s) -> grip (slip, logged only with size on) %.2f, size %.2f x injury %.2f x stamina %.2f = %.2f",
                     b.muscle_rate, side == 0 ? "L" : "R", b.tonus, b.consciousness, b.stamina_value, muscle_found ? "found" : "not found", b.strength, b.size,
                     b.injury, b.stamina, b.total);
        }
        return b;
    }

    // Holding and swinging tire him, by the game's own drain (stamina_calibration.md, Willie @441248,
    // "R Hand Stamina Burn Rate f", @354657), which reads the hand joint (0 under the mod's hold) and
    // the mouse's "Aim Swing Speed" (never moved in VR). Per hand:
    //   burn B = (F + 100 tau) / 60 x MRC(Muscle Rate, 0..3 -> 1..0.1), kept only from 10 up
    //     (the game's joint "force" is an impulse per step, so at 60 fps it is (F N + 100 tau N m) / 60);
    //     F and tau the hold's own, capped at pose_stamina_force_cap_n / pose_stamina_torque_cap_nm
    //     (a weapon snagged or blocked made 680 N and emptied the bar in a second, playtest 2026-11-18);
    //   swing M = the controller's speed at the grip, smoothed over pose_stamina_speed_smooth_s,
    //     lo..hi cm/s -> x10..x85 (the game's Aim Swing Speed builds up the same way: one swing stays near x10);
    //   per second = B x M x 0.0125 x MRC(Fat Rate, -1..10 -> 0.5..2.25) x pose_stamina_scale.
    struct StaminaHand
    {
        bool have = false;
        hsvr::Vec3 last;
        double speed = 0.0;
    };
    StaminaHand g_stamina_hand[2];
    double number_var(UObject* owner, const TCHAR* name, bool& found)
    {
        FProperty* p = owner ? owner->GetPropertyByNameInChain(name) : nullptr;
        found = p != nullptr;
        return p ? number_property(owner, p) : 0.0;
    }
    double map_range(double v, double a, double b, double x, double y)
    {
        double t = (std::min)(1.0, (std::max)(0.0, (v - a) / (b - a)));
        return x + (y - x) * t;
    }
    void drain_stamina(int side, UObject* pawn, double force_n, double torque_nm, const hsvr::Vec3& grip_target, double dt, const hsvr::Config& cfg)
    {
        using namespace hsvr;
        StaminaHand& sh = g_stamina_hand[side];
        if (!pawn || dt <= 0.0) return;
        if (sh.have)
        {
            double inst = length(grip_target - sh.last) / dt;
            if (inst > 3000.0) inst = sh.speed; // a recentre or teleport, not a swing
            double a = (std::min)(1.0, dt / (std::max)(0.01, cfg.pose_stamina_speed_smooth_s));
            sh.speed += (inst - sh.speed) * a;
        }
        sh.last = grip_target;
        sh.have = true;
        if (cfg.pose_stamina_scale <= 0.0) return;
        bool has_mr = false, has_fat = false;
        double mr = number_var(pawn, STR("Muscle Rate"), has_mr);
        double fat = number_var(pawn, STR("Fat Rate"), has_fat);
        double strength = has_mr ? map_range(mr, 0.0, 3.0, 1.0, 0.1) : 1.0;
        double fat_k = has_fat ? map_range(fat, -1.0, 10.0, 0.5, 2.25) : 1.0;
        double f = (std::min)(force_n, cfg.pose_stamina_force_cap_n), t = (std::min)(torque_nm, cfg.pose_stamina_torque_cap_nm);
        double burn = (f + 100.0 * t) / 60.0 * strength;
        double lo = cfg.pose_stamina_swing_lo_cm_s, hi = (std::max)(lo + 1.0, cfg.pose_stamina_swing_hi_cm_s);
        double swing = map_range(sh.speed, lo, hi, 10.0, 85.0);
        double per_s = burn >= 10.0 ? burn * swing * 0.0125 * fat_k * cfg.pose_stamina_scale : 0.0;
        double st = read_number_variable(pawn, STR("Stamina"));
        if (st < 0.0) return;
        if (per_s > 0.0) set_variable(pawn, STR("Stamina"), (std::max)(0.0, st - per_s * dt));
        static double last_line[2] = {0.0, 0.0};
        double now = now_seconds();
        if (now - last_line[side] >= 1.0)
        {
            last_line[side] = now;
            HSVR_LOG("Stamina: %s hand: %.1f, taking %.1f a second (burn %.1f from %.0f N, %.1f N m; speed %.0f cm/s -> x%.0f; Muscle Rate %s%.2f x%.2f, Fat Rate %s%.2f x%.2f)",
                     side == 0 ? "left" : "right", st, per_s, burn, force_n, torque_nm, sh.speed, swing, has_mr ? "" : "(not found) ", mr, strength,
                     has_fat ? "" : "(not found) ", fat, fat_k);
        }
    }

    // This frame's delta time, the one the physics will step by after the ticks (GetWorldDeltaSeconds).
    double world_delta_seconds(UObject* context)
    {
        static UObject* gs = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__GameplayStatics"));
        if (!gs || !context) return -1.0;
        Call c(gs, STR("GetWorldDeltaSeconds"));
        if (!c.fn) return -1.0;
        c.object(STR("WorldContextObject"), context);
        if (!c.run()) return -1.0;
        return c.number();
    }

    // One tick of the hold for one hand. `joint` is the game's hand joint (its frames say where on the
    // weapon the hand holds it and how the weapon sits in the hand); `base` the weapon's BaseMesh.
    std::wstring stab_name_var(UObject* owner, const TCHAR* name);
    double g_wound_since[2] = {-1.0, -1.0}; // when each hand's weapon went into a wound
    // Is the weapon stuck by a thrust (the game's stuck joint "Thrust?"), not a lodged cut?
    bool stuck_is_thrust(UObject* weapon)
    {
        if (!weapon) return false;
        RawArray* joints = weapon->GetValuePtrByPropertyNameInChain<RawArray>(STR("Stuck Constraints Array"));
        UObject* cs = joints && joints->Data && joints->Num > 0 ? static_cast<UObject*>(joints->Data[joints->Num - 1]) : nullptr;
        return cs && bool_property(cs, STR("Thrust?"));
    }
    UObject* g_young_joint[2] = {nullptr, nullptr}; // a stick still held open (stab_hold_keep)
    double g_young_since[2] = {-1.0, -1.0};
    int g_young_trace_ok[2] = {0, 0}; // ticks in a row the normal 3 cm keep trace found the stuck bone

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
                if (h.free.hold_at)
                {
                    h.free.place = false;
                    h.settle_from_pos = h.free.at_pos;
                    h.settle_from_rot = h.free.at_rot;
                    h.settle_s = (std::max)(0.02, cfg.pose_grab_settle_s);
                    h.settle_until = now + h.settle_s;
                }
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
                HSVR_LOG("Hold: %s hand's %ls is held to %s by the game%s", side == 0 ? "left" : "right", weapon->GetName().c_str(), bone2.empty() ? "(no bone name)" : bone2.c_str(),
                         h.free.set ? (h.free.forearm ? "; the mod's grip holds it on the forearm" : "; the mod's grip holds it in the hand")
                                    : (forearm ? " (on the forearm: strap shield)" : ""));
            }
            h.forearm = forearm;
            if (forearm && !h.free.set && !h.into_hand_tried && cfg.pose_shield_in_hand)
            {
                h.into_hand_tried = true;
                FreeGrip g;
                if (shield_into_hand(side, pawn, weapon, base, g)) h.free = g;
            }
        }
        if (cfg.pose_fingers_anim_only)
        {
            if (h.free.set && h.free.grip_type >= 0) set_anim_grip(side, h.free.grip_type);
            else if (h.free.set && h.free.fingers && cfg.pose_shield_hand_grip_type >= 0.0) set_anim_grip(side, (int)cfg.pose_shield_hand_grip_type);
        }
        else if (h.free.set && h.free.grip_type >= 0)
        {
            if (read_number_variable(pawn, grip_type_name(side)) != h.free.grip_type) set_variable(pawn, grip_type_name(side), h.free.grip_type);
        }
        else if (h.free.set && h.free.fingers && cfg.pose_shield_hand_grip_type >= 0.0 &&
                 read_number_variable(pawn, grip_type_name(side)) != cfg.pose_shield_hand_grip_type)
            set_variable(pawn, grip_type_name(side), cfg.pose_shield_hand_grip_type);
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
        h.handle_slide = false;
        if (cfg.pose_grab_handles && !h.free.set && !h.forearm && !is_fist(weapon))
        {
            // The handle system: the game's own grip (a weapon he spawned with, or set up by the game)
            // becomes the same kind of grip as any other, on the handle nearest its grip point, so
            // either hand can slide along it.
            FreeGrip g;
            g.set = true;
            g.pivot = pivot;
            g.in_anchor = in_hand;
            g.rel = (f2 * f1.inverse()).normalized();
            g.what = "game's grip";
            const std::vector<Handle>& hs = item_handles(weapon, base);
            double t = 0.0, d = 1e9;
            int hi = nearest_handle(hs, pivot, t, d);
            if (hi >= 0 && d <= 4.0 && length(hs[hi].b - hs[hi].a) > 1e-3)
            {
                g.has_handle = true;
                g.ha = hs[hi].a;
                g.hb = hs[hi].b;
                Vec3 ab = g.hb - g.ha;
                g.t = (std::min)(1.0, (std::max)(0.0, dot(pivot - g.ha, ab) / dot(ab, ab)));
                g.handle = hs[hi].name;
                // Its point onto the handle's line (the hand stays where the game put it along it).
                g.pivot = g.ha + ab * g.t;
            }
            h.free = g;
            HSVR_LOG("Hold: %s hand holds %ls by the game's grip%s%s", side == 0 ? "left" : "right", weapon->GetName().c_str(), g.has_handle ? ", on its " : " (no handle near)",
                     g.has_handle ? g.handle.c_str() : "");
        }
        if (h.free.set && h.free.has_handle && cfg.pose_slide && cfg.pose_hold_exact)
        {
            // The slide (B&S): hold the trigger and the hand runs along the handle; the item keeps its
            // own motion along it (a light squeeze drags, a full one lets it slide freely) and the hand
            // stays where it is on it. It stops at the handle's ends.
            ControllerState st = controllers_state();
            float trig = g_fake_trigger[side].load() >= 0.0f ? g_fake_trigger[side].load() : st.hand[side].trigger;
            bool on = h.handle_sliding_logged ? trig > 0.15f : trig > 0.3f;
            if (on != h.handle_sliding_logged)
            {
                h.handle_sliding_logged = on;
                HSVR_LOG("Slide: %s hand %s on the %s of %ls at %.0f%%", side == 0 ? "left" : "right", on ? "starts sliding" : "holds", h.free.handle.c_str(),
                         weapon->GetName().c_str(), h.free.t * 100.0);
            }
            if (on && !(h.started >= 0.0 && now - h.started < cfg.pose_hold_let_go_grace_s))
            {
                Vec3 bp = component_location(base);
                Quat bq = component_rotation(base);
                Vec3 palm = hand_pos + hand_rot.rotate(h.free.in_anchor);
                Vec3 local = bq.inverse().rotate(palm - bp);
                Vec3 ab = h.free.hb - h.free.ha;
                double u = (std::min)(1.0, (std::max)(0.0, dot(local - h.free.ha, ab) / dot(ab, ab)));
                h.free.t = u;
                h.free.pivot = h.free.ha + ab * u;
                bool at_end = u <= 0.0 || u >= 1.0;
                if (!at_end)
                {
                    h.handle_slide = true;
                    h.handle_axis = bq.rotate(normalized(ab));
                    double squeeze = (std::min)(1.0, (std::max)(0.0, (trig - 0.15) / 0.85));
                    h.handle_keep = cfg.pose_slide_keep_light + (cfg.pose_slide_keep_full - cfg.pose_slide_keep_light) * squeeze;
                    // B&S (Handle.cs:1053-1087): the axis drive's damper goes 300 -> 20 N s/m as the trigger goes in.
                    h.handle_damp = cfg.pose_slide_damper_light + (cfg.pose_slide_damper_full - cfg.pose_slide_damper_light) * squeeze;
                }
            }
        }
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
                if (g_two.use_rel)
                {
                    // The handle system: the second hand slides along its handle (trigger), its point
                    // the projection of your hand onto it.
                    if (on && g_two.has_handle)
                    {
                        Vec3 local = bq_now.inverse().rotate(p2 - bp_now);
                        Vec3 ab = g_two.hb - g_two.ha;
                        double u = (std::min)(1.0, (std::max)(0.0, dot(local - g_two.ha, ab) / dot(ab, ab)));
                        Vec3 cand = g_two.ha + ab * u;
                        if (length(cand - pivot) >= 2.0 * cfg.pose_slide_hand_half_cm)
                        {
                            g_two.t = u;
                            g_two.local = cand;
                        }
                    }
                }
                else if (on && h.have_last)
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
                Vec3 local2 = g_two.use_rel ? g_two.local : Vec3{pivot.x, pivot.y, game_z + g_two.z};
                g_two.local = local2;
                g_two.have_local = true;
                Vec3 dl = local2 - pivot;
                double dz = g_two.use_rel ? length(dl) : local2.z - pivot.z;
                Vec3 between = p2 - p1;
                // B&S: the second hand only pulls its own point toward its controller (no turning); the
                // first keeps its own target (bs_hold_measured.md §3).
                {
                    double dtp = (std::min)(1.0 / 30.0, (std::max)(1.0 / 240.0, now - h.last_time));
                    Vec3 raw_v = g_two.bs_valid && h.have_last ? (p2 - g_two.bs_p2) * (1.0 / dtp) : Vec3{};
                    // Smoothed like the first hand's (pose_hold_target_vel_smooth_s): two noisy hands pulling one
                    // weapon shook it most (playtest 14:37: halberd and sword in two hands 1.0-1.4 cm at ~10 Hz).
                    double tau2 = hsvr::config().pose_hold_target_vel_smooth_s;
                    if (tau2 > 1e-4 && g_two.bs_valid && h.have_last)
                        g_two.bs_vel = g_two.bs_vel + (raw_v - g_two.bs_vel) * (1.0 - std::exp(-dtp / tau2));
                    else
                        g_two.bs_vel = raw_v;
                    if (length(g_two.bs_vel) > 3000.0) g_two.bs_vel = Vec3{};
                    g_two.bs_p2 = p2;
                    g_two.bs_local = local2;
                    g_two.bs_valid = true;
                }
                if (std::fabs(dz) > 1.0 && length(between) > 1.0 && !cfg.pose_hold_bs)
                {
                    Vec3 axis_now = g_two.use_rel ? target_rot.rotate(normalized(dl)) : target_rot.rotate(Vec3{0, 0, 1});
                    Vec3 want_axis = normalized(between) * (g_two.use_rel || dz > 0.0 ? 1.0 : -1.0);
                    Vec3 c = cross(axis_now, want_axis);
                    double d = dot(axis_now, want_axis);
                    Quat swing = d < -0.9999 ? quat_axis_angle(normalized(target_rot.rotate(Vec3{1, 0, 0})), 3.14159265358979) : Quat{c.x, c.y, c.z, 1.0 + d}.normalized();
                    target_rot = (swing * target_rot).normalized();
                    // The point halfway between the hands on the weapon goes halfway between your hands.
                    Vec3 mid_local = (pivot + local2) * 0.5;
                    target_pos = (p1 + p2) * 0.5 - target_rot.rotate(mid_local);
                    pivot = mid_local;
                }
                // Playtest 02:44 ("frozen in mid-air, but it swooshes"): while the weapon trails its
                // two-hand target, every tick says how the target moves and what holds it back.
                {
                    double jump = g_two.have_last_rot ? length(scaled_axis((target_rot * g_two.last_rot.inverse()).normalized())) * 57.2958 : 0.0;
                    g_two.last_rot = target_rot;
                    g_two.have_last_rot = true;
                    if (h.lag > 8.0 || jump > 6.0)
                    {
                        UObject* bm = base;
                        Call av(bm, STR("GetPhysicsAngularVelocityInRadians"));
                        av.run();
                        HSVR_LOG_EVERY(0.05, "Two hands trail: hands %.1f cm apart for %.1f on the weapon, target turned %.1f deg this tick, weapon %.0f deg/s, "
                                             "behind %.1f cm, force %.0f N%s torque %.1f N m%s, contact miss %.1f cm, hand speeds %.1f / %.1f m/s",
                                       length(between), std::fabs(dz), jump, length(av.vec()) * 57.2958, h.lag, h.force_n, h.force_capped ? " (cap)" : "", h.torque_nm,
                                       h.torque_capped ? " (cap)" : "", h.miss_cm, g_hand_world_v[side], g_hand_world_v[s2]);
                    }
                }
                // Both hands pull: the second adds its own strength (B&S: two hands, twice the hold).
                BodyStrength first = body_strength(pawn, side, cfg), second = body_strength(pawn, s2, cfg);
                two_hand_gain = 1.0 + (std::max)(0.0, cfg.pose_two_hand_strength - 1.0) * (second.total / (std::max)(0.05, first.total));
                g_two.couple = std::fabs(dz) > 1.0 ? cfg.pose_hold_max_force * (std::max)(0.0, cfg.pose_hold_strength) * second.total * 100.0 * std::fabs(dz) : 0.0;
                // A controller pulled well off its spot on the weapon lets go (B&S).
                Vec3 spot = bp_now + bq_now.rotate(local2);
                double off = length(p2 - spot);
                if (off > (cfg.pose_hold_bs ? cfg.pose_bs_two_let_go_cm : cfg.pose_two_hand_let_go_cm))
                {
                    if (g_two.far_since < 0.0) g_two.far_since = now;
                    else if (now - g_two.far_since > 0.3) end_two_hand("your hand was pulled off it");
                }
                else
                    g_two.far_since = -1.0;
                HSVR_LOG_EVERY(1.0, "Two hands: %ls on the %s at %.1f cm, first hand at %.1f; hands %.1f cm apart for %.1f cm on the weapon; second hand %.1f cm off its spot",
                               weapon->GetName().c_str(), g_two.use_rel ? g_two.handle.c_str() : (g_two.on_blade ? "blade" : "grip"),
                               g_two.use_rel ? g_two.local.z : g_two.z, g_two.use_rel ? pivot.z : h.slide, length(between), std::fabs(dz), off);
            }
        }

        if (g_draw[side].active && g_draw[side].weapon == weapon)
        {
            // Drawing: the scabbard's slide joint holds the blade on its line; the hand moves it only
            // along that line (its grip point follows your hand's along the blade), turned as it is.
            Vec3 bp = component_location(base);
            Quat bq = component_rotation(base);
            Vec3 ax = bq.rotate(Vec3{0, 0, 1});
            Vec3 want = target_pos + target_rot.rotate(pivot), have = bp + bq.rotate(pivot);
            double d = dot(want - have, ax);
            // At most a fast hand's travel per tick (no yank when the hand starts far from where it is held).
            d = (std::min)(cfg.pose_draw_step_cm, (std::max)(-cfg.pose_draw_step_cm, d));
            target_rot = bq;
            target_pos = bp + ax * d;
            h.settle_until = -1.0;
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
                // Smoothed (pose_hold_target_vel_smooth_s): a frame-to-frame difference of a tracked hand
                // turns its ~0.4 cm of tracking noise into ~35 cm/s of noise, and the dampers drove the weapon
                // with it: held still, the weapon and the hand drawn on it shook 1.0-1.4 cm at ~10 Hz while
                // the controller moved 0.3-0.6 cm (playtest 14:37, halberd / sword / axe).
                double tau = cfg.pose_hold_target_vel_smooth_s;
                if (tau > 1e-4 && h.vel_f_set)
                {
                    double a = 1.0 - std::exp(-dt / tau);
                    h.vel_f = h.vel_f + (target_vel - h.vel_f) * a;
                    h.ang_f = h.ang_f + (target_ang - h.ang_f) * a;
                    target_vel = h.vel_f;
                    target_ang = h.ang_f;
                }
                else
                {
                    h.vel_f = target_vel;
                    h.ang_f = target_ang;
                    h.vel_f_set = true;
                }
            }
            else
                h.vel_f_set = false;
        }
        h.last_target_pos = target_pos;
        h.last_target_rot = target_rot;
        h.last_time = now;
        h.have_last = true;

        // The hand's hold (SI as in the framework, then in Unreal's units: cm, so a force x100, a
        // turning spring, damper and torque x100^2), times its strength (its dampers by half of it past 1).
        double strength = (std::max)(0.0, cfg.pose_hold_strength) * (cfg.pose_hold_bs ? 1.0 : two_hand_gain);
        double damp_strength = (std::max)(1.0, strength * 0.5);
        double k = cfg.pose_hold_spring * strength;
        double c = cfg.pose_hold_damper * damp_strength;
        BodyStrength body = body_strength(pawn, side, cfg);
        h.body_strength = body.total;
        double max_force = cfg.pose_hold_max_force * strength * body.total * 100.0;
        double ka = cfg.pose_hold_turn_spring * strength * 1e4;
        double ca = cfg.pose_hold_turn_damper * damp_strength * 1e4;
        double max_torque = cfg.pose_hold_max_torque * strength * body.total * 1e4;
        // Stuck in someone (a stab or an edge lodged): the hand pulls with only part of its strength, so
        // the hold's own catch-up does not tear the blade out at once (stab-test8: the game released
        // every lodge 10-20 ms later while the hold pulled 356-2860 N); pulling it out is your doing.
        double draw_scale_cfg = cfg.pose_hold_hand_follows && cfg.pose_hold_hand_follows_draw ? cfg.pose_draw_force_scale_follows : cfg.pose_draw_force_scale;
        if (g_draw[side].active && g_draw[side].weapon == weapon && draw_scale_cfg < 1.0)
        {
            // Drawing or sheathing: the hand slides it along the scabbard with part of its strength,
            // so it never drags the scabbard (and the belt) along with it.
            max_force *= (std::max)(0.0, draw_scale_cfg);
            max_torque *= (std::max)(0.0, draw_scale_cfg);
        }
        double max_force_unstuck = max_force; // the hand's whole strength (drag, below)
        if (cfg.pose_hold_stuck_force_scale < 1.0 && bool_property(weapon, STR("Stuck In Body")) && stuck_is_thrust(weapon))
        {
            max_force *= (std::max)(0.0, cfg.pose_hold_stuck_force_scale);
            max_torque *= (std::max)(0.0, cfg.pose_hold_stuck_force_scale);
            HSVR_LOG_EVERY(0.5, "Hold: %s hand's %ls is stuck in a body: pulling at %.0f%% strength", side == 0 ? "left" : "right", weapon->GetName().c_str(),
                           cfg.pose_hold_stuck_force_scale * 100.0);
        }

        // The weapon as it is.
        Vec3 pos = component_location(base);
        Quat rot = component_rotation(base);
        Call cm(base, STR("GetCenterOfMass"));
        cm.run();
        Vec3 com = cm.vec();
        note_com(side, weapon, com);
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
        bool exact_set = false;
        Vec3 exact_v;
        if (cfg.pose_hold_exact)
        {
            // B&S-style: the weapon is in the hand. Its velocity this frame is exactly what puts it
            // where the hand is drawn at the end of this frame's physics (the frame's own delta time,
            // not the last frame's: the physics steps by it after the ticks), so nothing trails the
            // hand and it hits with its real speed. Only the hand's strength limits the change (a
            // blocked or too heavy weapon falls behind, and the hand goes onto it). Gravity during
            // the frame is taken off ahead.
            double DT = world_delta_seconds(pawn);
            if (!(DT > 1e-4 && DT < 0.1)) DT = dt;
            DT = (std::min)(0.08, DT);
            Vec3 com_t = target_pos + target_rot.rotate(local_com);
            Quat rot_t = target_rot;
            if (h.settle_until > now)
            {
                // Pulled into the hand from where it lay (B&S: the springs bring it in over 50-100 ms).
                double u = 1.0 - (h.settle_until - now) / h.settle_s;
                u = u * u * (3.0 - 2.0 * u);
                Vec3 from_com = h.settle_from_pos + h.settle_from_rot.rotate(local_com);
                com_t = from_com + (com_t - from_com) * u;
                rot_t = (from_scaled_axis(scaled_axis((target_rot * h.settle_from_rot.inverse()).normalized()) * u) * h.settle_from_rot).normalized();
            }
            Vec3 g{0.0, 0.0, gz};
            Vec3 v_com_now = lv.vec();
            Vec3 v_need = (com_t - com) * (1.0 / DT) - g * (DT * 0.5);
            if (h.handle_slide)
            {
                // Sliding: along the handle it keeps (most of) its own motion; across it, in the hand.
                Vec3 u = h.handle_axis;
                if (cfg.pose_slide_damper_light > 0.0) h.handle_keep = std::exp(-h.handle_damp * DT / (std::max)(0.05, mass));
                double now_ax = dot(v_com_now, u), need_ax = dot(v_need, u);
                v_need = v_need + u * ((now_ax * h.handle_keep + need_ax * (1.0 - h.handle_keep)) - need_ax);
            }
            // In a wound (Stuck In Body): the blade only slides along the wound, slowed by it as in
            // B&S (held penetration dampers 800 in / 1000 N s/m out: with the hand's ~400 N that is
            // ~0.5 m/s in, ~0.4 m/s out), harder in the head (S&S: pull-out needs about 3x more);
            // sideways and in turn it goes with him (the game's stuck joint holds it in a 15 deg
            // cone). Pulling back far enough draws it out; the game lets go once the tip box leaves
            // the body (stab-test9).
            bool in_wound = cfg.stab_wound_hold && bool_property(weapon, STR("Stuck In Body")) && stuck_is_thrust(weapon);
            bool drag = false;
            Vec3 wound_axis;
            static bool was_in_wound[2] = {false, false};
            if (!in_wound && was_in_wound[side]) HSVR_LOG("Hold: %s hand's weapon is out of the wound", side == 0 ? "left" : "right");
            was_in_wound[side] = in_wound;
            if (!in_wound) g_wound_since[side] = -1.0;
            if (in_wound)
            {
                Vec3 u = rot.rotate({0.0, 0.0, 1.0});
                if (UObject* jt = object_property(weapon, STR("Jammed Tip")))
                {
                    Call f(jt, STR("GetForwardVector"));
                    f.run();
                    Vec3 fw = f.vec();
                    if (length(fw) > 0.5) u = fw * (1.0 / length(fw));
                }
                std::wstring bone = stab_name_var(weapon, STR("Hit Bone Name"));
                bool head = bone == STR("Head") || bone == STR("head") || bone.rfind(STR("neck"), 0) == 0;
                double k = head ? cfg.stab_wound_head_factor : 1.0;
                double need_ax = dot(v_need, u), now_ax = dot(v_com_now, u);
                // The first moments in the wound: no pulling out yet (stab-test11: the hold's target
                // sits behind a blade that has just gone in, and its catch-up drew it straight out).
                if (g_wound_since[side] < 0.0) g_wound_since[side] = now;
                bool settling = now - g_wound_since[side] < cfg.stab_wound_settle_s || g_young_joint[side] != nullptr;
                double out_most = settling ? 0.0 : cfg.stab_wound_out_cms * k;
                // Going deeper is easier than coming out (S&S: a plunge follows the hand; pulling out
                // of a skull takes ~3x the effort): the head factor slows only the way out fully.
                double k_in = head ? cfg.stab_wound_head_in_factor : 1.0;
                double ax = (std::min)(cfg.stab_wound_in_cms * k_in, (std::max)(-out_most, need_ax));
                // Going in: the game sticks a blade at the skin (stab-test13: its keep trace found the
                // tip 1.2 cm inside, then outside), so for the first moments it is carried in at
                // stab_wound_entry_cms whatever the hand does, as the arm's momentum would.
                if (settling) ax = (std::max)(ax, cfg.stab_wound_entry_cms);
                // Dragging him (Matthew 2026-11-20: a falling body should not rip the weapon out of
                // his hand, his weapon should pull the body): once the stick has settled, the hand
                // pulls the blade sideways toward where it holds it with its whole strength (arm
                // strength, stamina, injuries: max_force), and the game's stuck joint pulls the body
                // after it. Along the wound the slide limits above still decide (pulling out).
                drag = cfg.stab_drag && !settling;
                Vec3 lat = drag ? (v_need - u * need_ax) : (v_com_now - u * now_ax);
                v_need = lat + u * ax;
                wound_axis = u;
                HSVR_LOG_EVERY(0.25, "Hold: %s hand's %ls in a wound (%ls): wanted %.0f cm/s along it, slides %.0f", side == 0 ? "left" : "right",
                               weapon->GetName().c_str(), bone.c_str(), need_ax, ax);
            }
            Vec3 dv = v_need - v_com_now;
            double dv_most = (max_force / (std::max)(0.05, mass)) * DT + std::fabs(gz) * DT;
            bool f_capped = length(dv) > dv_most;
            if (drag)
            {
                // Along the wound: the stuck share of the strength; across it: the drag share of all of it.
                double dax = dot(dv, wound_axis);
                Vec3 dlat = dv - wound_axis * dax;
                double lat_most = (max_force_unstuck * cfg.stab_drag_force_scale / (std::max)(0.05, mass)) * DT + std::fabs(gz) * DT;
                f_capped = length(dlat) > lat_most;
                dlat = clamp_length(dlat, lat_most);
                dax = (std::min)(dv_most, (std::max)(-dv_most, dax));
                dv = dlat + wound_axis * dax;
                h.drag_capped = f_capped;
                HSVR_LOG_EVERY(0.5, "Hold: %s hand drags him by the stuck %ls: %.0f N of the %.0f N it has%s", side == 0 ? "left" : "right", weapon->GetName().c_str(),
                               length(dlat) * mass / DT / 100.0, max_force_unstuck * cfg.stab_drag_force_scale / 100.0, f_capped ? " (all of it)" : "");
            }
            else
            {
                dv = clamp_length(dv, dv_most);
                h.drag_capped = false;
            }
            Vec3 w_need = scaled_axis((rot_t * rot.inverse()).normalized()) * (1.0 / DT);
            if (in_wound) w_need = w_now;
            Vec3 dL = about_com * (w_need - w_now);
            bool t_capped = length(dL) > max_torque * DT;
            dL = clamp_length(dL, max_torque * DT);
            exact_v = clamp_length(v_com_now + dv, 2000.0);
            w_next = clamp_length(w_now + about_com.inverse() * dL, 60.0);
            h.force_capped = f_capped;
            h.torque_capped = t_capped;
            h.force_n = length(dv) * mass / DT / 100.0;
            h.torque_nm = length(dL) / DT / 1e4;
            // Weight (Matthew 2026-11-19, B&S-style): the hand keeps the grip where it holds it, but its
            // wrist turns the weapon about the grip with no more than max_torque. So the torque the turn
            // needs is about the GRIP (the inertia there, plus the weapon's mass carried round by the
            // grip's own acceleration and gravity): tau = I_grip alpha + m (C - P) x a_P. A dagger turns
            // 1:1; a polearm (5 kg m^2 about its grip) trails a fast flick and droops under its own lever.
            // (The plain exact hold above split it into the centre of mass's push plus a turn about it,
            // so the lever was free and every weapon followed 1:1.)
            if (cfg.pose_hold_weight && !cfg.pose_hold_bs && !in_wound && !h.handle_slide && !(g_draw[side].active && g_draw[side].weapon == weapon))
            {
                Vec3 r = com - pivot_now; // grip -> centre of mass
                Vec3 P_t = com_t - rot_t.rotate(local_com - pivot);
                Vec3 vP_need = (P_t - pivot_now) * (1.0 / DT) - g * (DT * 0.5);
                Vec3 dvP = vP_need - v;
                double dvP_most = (max_force / (std::max)(0.05, mass)) * DT + std::fabs(gz) * DT;
                bool fcap = length(dvP) > dvP_most;
                dvP = clamp_length(dvP, dvP_most);
                Vec3 aP = dvP * (1.0 / DT);
                Vec3 wn = scaled_axis((rot_t * rot.inverse()).normalized()) * (1.0 / DT);
                Vec3 alpha_need = (wn - w_now) * (1.0 / DT);
                Vec3 lever = cross(r, aP) * mass;
                Vec3 tau_need = inertia * alpha_need + lever;
                double couple = (cfg.pose_hold_weight_two_hand_couple && g_two.active && g_two.main == side && g_two.weapon == weapon) ? g_two.couple : 0.0;
                double tcap = (max_torque + couple) * (std::max)(0.0, cfg.pose_hold_weight_torque_scale);
                bool tc = length(tau_need) > tcap;
                Vec3 tau = clamp_length(tau_need, tcap);
                Vec3 alpha = inertia.inverse() * (tau - lever);
                w_next = clamp_length(w_now + alpha * DT, 60.0);
                if (cfg.pose_hold_weight_spin_extra >= 0.0)
                {
                    double most = length(wn) + cfg.pose_hold_weight_spin_extra;
                    if (length(w_next) > most)
                    {
                        HSVR_LOG_EVERY(0.25, "Hold: %s weight: %ls would spin at %.0f deg/s for a %.0f deg/s turn; held to %.0f", side == 0 ? "left" : "right",
                                       weapon->GetName().c_str(), length(w_next) * 57.2958, length(wn) * 57.2958, most * 57.2958);
                        w_next = normalized(w_next) * most;
                    }
                }
                Vec3 vP_next = v + dvP;
                exact_v = clamp_length(vP_next + cross(w_next, r), 2000.0);
                h.force_capped = fcap;
                h.torque_capped = tc;
                h.force_n = length(dvP) * mass / DT / 100.0;
                h.torque_nm = length(tau) / 1e4;
                if (tc)
                    HSVR_LOG_EVERY(0.5, "Hold: %s weight: %ls needs %.0f N m at the grip to turn as you do, the hand has %.0f%s", side == 0 ? "left" : "right",
                                   weapon->GetName().c_str(), length(tau_need) / 1e4, tcap / 1e4, couple > 0.0 ? " (with both hands)" : "");
            }
            if (cfg.pose_hold_bs && !in_wound && !(g_draw[side].active && g_draw[side].weapon == weapon))
            {
                bool two = g_two.active && g_two.main == side && g_two.weapon == weapon && g_two.bs_valid && g_pose.hand_valid[g_two.second];
                BodyStrength s2b = two ? body_strength(pawn, g_two.second, cfg) : BodyStrength{};
                double pk = two ? cfg.pose_bs_two_pos_k : cfg.pose_hold_spring, pc = two ? cfg.pose_bs_two_pos_c : cfg.pose_hold_damper,
                       pf = two ? cfg.pose_bs_two_pos_max : cfg.pose_hold_max_force;
                double rk = two ? cfg.pose_bs_two_rot_k : cfg.pose_hold_turn_spring, rc = two ? cfg.pose_bs_two_rot_c : cfg.pose_hold_turn_damper,
                       rf = two ? cfg.pose_bs_two_rot_max : cfg.pose_hold_max_torque;
                double st = (std::max)(0.0, cfg.pose_hold_strength);
                double dst = (std::max)(1.0, st * 0.5);
                bool arm_lag = cfg.pose_hold_arm_lag && cfg.pose_hold_arm_lag_s > 0.0;
                if (arm_lag)
                {
                    // Flat Half Sword's arm (flat_hs_weight_vs_hold.md, weak_willie_weight.md): the same pull for
                    // every body on arm + weapon, so the follow slows by sqrt((arm + weapon) / arm) about the
                    // shoulder. Critically damped spring: steady-swing lag = 2 / omega.
                    if (cfg.pose_body_size_strength) body_size_factor(pawn);
                    Vec3 rp = pivot_now - com;
                    double ig = (std::max)((std::max)(I.x, I.y), I.z) + mass * dot(rp, rp); // kg cm^2 about the grip
                    double reach = cfg.pose_hold_arm_reach_cm * 0.01;
                    double A = arm_inertia_shoulder(g_body_dims, reach) * (two ? 2.0 : 1.0);
                    double L = reach * g_body_dims.scale / 0.9375;
                    double W = ig / 1e4 + mass * L * L;
                    double fit = (std::min)(body.injury, body.stamina);
                    if (two) fit = 0.5 * (fit + (std::min)(s2b.injury, s2b.stamina));
                    fit = (std::max)(0.05, fit);
                    double lag = cfg.pose_hold_arm_lag_s * std::sqrt((A + W) / (std::max)(0.01, A)) / std::sqrt(fit);
                    lag = (std::min)((std::max)(cfg.pose_hold_arm_lag_s, cfg.pose_hold_arm_lag_max_s), lag);
                    double om = 2.0 / lag;
                    double z = cfg.pose_hold_damping_ratio > 0.0 ? cfg.pose_hold_damping_ratio : 1.0;
                    double per_point = two ? mass * 0.5 : mass;
                    pk = per_point * om * om / (std::max)(1e-3, st);
                    pc = 2.0 * z * per_point * om / dst;
                    rk = (ig / 1e4) * om * om / (std::max)(1e-3, st);
                    rc = 2.0 * z * (ig / 1e4) * om / dst;
                    HSVR_LOG_EVERY(2.0, "Hold: %s arm lag for %ls (%.2f kg, %.3f kg m^2 about the grip%s): arm %.2f + weapon %.2f kg m^2 about the shoulder, fit %.2f -> lag %.3f s "
                                   "(spring %.0f N/m, turn %.1f N m/rad)", side == 0 ? "left" : "right", weapon->GetName().c_str(), mass, ig / 1e4,
                                   two ? ", two hands" : "", A, W, fit, lag, pk * st, rk * st);
                }
                // Dampers per weapon (pose_hold_damping_ratio): critical damping for this weapon's own mass and
                // its inertia about the grip, so a long heavy one (war staff, 1.9 kg m^2 about a low grip) settles
                // without bouncing and a short sword (0.04 kg m^2) doesn't trail the wrist and swing on past a stop
                // (playtest 14:19 with fixed dampers tuned for the staff). 0 = the fixed dampers above.
                if (cfg.pose_hold_damping_ratio > 0.0 && !arm_lag)
                {
                    Vec3 rp = pivot_now - com;
                    double ig = (std::max)((std::max)(I.x, I.y), I.z) + mass * dot(rp, rp); // kg cm^2
                    double z = cfg.pose_hold_damping_ratio;
                    double per_point = two ? mass * 0.5 : mass;
                    pc = 2.0 * z * std::sqrt((std::max)(0.0, pk * st * per_point)) / dst;
                    rc = 2.0 * z * std::sqrt((std::max)(0.0, rk * st * 1e4 * ig)) / (dst * 1e4);
                    HSVR_LOG_EVERY(2.0, "Hold: %s dampers for %ls (%.2f kg, %.3f kg m^2 about the grip): position %.0f, turn %.1f", side == 0 ? "left" : "right",
                                   weapon->GetName().c_str(), mass, ig / 1e4, pc * dst, rc * dst);
                }
                double draw_scale = 1.0;
                if (cfg.pose_hold_stuck_force_scale < 1.0 && bool_property(weapon, STR("Stuck In Body")) && stuck_is_thrust(weapon)) draw_scale = (std::max)(0.0, cfg.pose_hold_stuck_force_scale);
                SoftPoint pts[2];
                int n = 0;
                {
                    SoftPoint& p = pts[n++];
                    p.r = pivot_now - com;
                    p.err = pivot_now - target_pivot;
                    p.vt = target_vel;
                    p.k = pk * st;
                    p.c = pc * dst;
                    p.fmax = pf * st * body.total * 100.0 * draw_scale;
                    p.slide = h.handle_slide;
                    p.axis = h.handle_axis;
                }
                if (two)
                {
                    SoftPoint& p = pts[n++];
                    Vec3 at = pos + rot.rotate(g_two.bs_local);
                    p.r = at - com;
                    p.err = at - g_two.bs_p2;
                    p.vt = g_two.bs_vel;
                    p.k = pk * st;
                    p.c = pc * dst;
                    p.fmax = pf * st * s2b.total * 100.0 * draw_scale;
                }
                Mat3 invI = about_com.inverse();
                Vec3 vb = v_com_now + g * DT, wb = w_now; // gravity this frame, taken off again below (the physics adds it)
                if (arm_lag)
                {
                    // The arm carries the weapon's weight at the grip (the soft arm spring would let the hand
                    // sag g / omega^2, 7 cm for a polearm); its lever about the grip is still the turn's to hold.
                    for (int i = 0; i < n; ++i)
                    {
                        Vec3 J = g * (-mass * DT / n);
                        vb = vb + J * (1.0 / mass);
                        wb = wb + invI * cross(pts[i].r, J);
                    }
                }
                Vec3 ang;
                Vec3 err_rot = scaled_axis((rot * target_rot.inverse()).normalized());
                // The wrist's end stop (Matthew 2026-11-19: with the turning limit at a real wrist's 30 N m,
                // a short sword kept going past the hand when he stopped a swing, "the wrist hyperextends").
                // Within pose_hold_wrist_free_deg of the hand the turn is limited to the wrist's strength (a
                // long weapon held at its end droops); past it the limit rises to pose_hold_wrist_stop_torque
                // by pose_hold_wrist_stop_deg, as a real wrist bottoms out and the forearm takes the load.
                double tpow = cfg.pose_hold_torque_strength_power > 0.0 ? cfg.pose_hold_torque_strength_power : 1.0;
                double tbody = std::pow((std::max)(0.0, body.total), tpow);
                if (two) tbody = 0.5 * (tbody + std::pow((std::max)(0.0, s2b.total), tpow));
                double tcap = rf * st * tbody * 1e4 * draw_scale;
                {
                    double bend = length(err_rot) * 57.2958;
                    double f0 = cfg.pose_hold_wrist_free_deg, f1 = (std::max)(f0 + 1.0, cfg.pose_hold_wrist_stop_deg);
                    double hard = cfg.pose_hold_wrist_stop_torque * st * tbody * 1e4 * draw_scale;
                    double away = 1.0;
                    if (cfg.pose_hold_wrist_stop_spin > 0.0 && bend > 0.01)
                    {
                        // err_rot is the turn from the hand to the weapon: spinning further along it is going on past.
                        double sp = dot(w_now - target_ang, err_rot * (1.0 / length(err_rot)));
                        away = (std::min)(1.0, (std::max)(0.0, sp / cfg.pose_hold_wrist_stop_spin));
                    }
                    if (cfg.pose_hold_wrist_stop_torque > 0.0 && bend > f0 && hard > tcap && away > 0.0)
                    {
                        double u = (std::min)(1.0, (bend - f0) / (f1 - f0)) * away;
                        tcap = tcap + (hard - tcap) * u;
                        HSVR_LOG_EVERY(0.5, "Hold: %s wrist bent %.0f deg from the hand: turning limit %.0f N m (wrist stop)", side == 0 ? "left" : "right", bend,
                                       tcap / 1e4);
                    }
                }
                bs_solve(mass, invI, vb, wb, pts, n, err_rot, target_ang, rk * st * 1e4, rc * dst * 1e4, tcap, DT,
                         (std::max)(1, cfg.pose_bs_iterations), ang);
                exact_v = clamp_length(vb - g * DT, 2000.0);
                w_next = clamp_length(wb, (std::max)(1.0, cfg.pose_bs_max_spin));
                h.force_n = length(pts[0].acc) / DT / 100.0;
                h.force_capped = length(pts[0].acc) >= pts[0].fmax * DT * 0.999;
                h.torque_nm = length(ang) / DT / 1e4;
                h.torque_capped = length(ang) >= tcap * DT * 0.999;
                HSVR_LOG_EVERY(0.5, "Hold: %s B&S hold%s: grip %.1f cm / %.0f deg off, %.0f N%s, turn %.1f N m%s%s", side == 0 ? "left" : "right",
                               two ? " (two hands)" : "", length(pts[0].err), length(err_rot) * 57.2958, h.force_n, h.force_capped ? " (cap)" : "", h.torque_nm,
                               h.torque_capped ? " (cap)" : "",
                               two ? (std::string(", second hand ") + std::to_string((int)std::lround(length(pts[1].err))) + " cm off, " +
                                      std::to_string((int)std::lround(length(pts[1].acc) / DT / 100.0)) + " N").c_str() : "");
            }
            com_next = com + exact_v * DT + g * (DT * DT * 0.5);
            exact_set = true;
            HSVR_LOG_EVERY(1.0, "Hold: %s exact hold, frame %.1f ms (measured %.1f), %.1f cm and %.1f deg to go, force %.0f N%s, torque %.1f N m%s",
                           side == 0 ? "left" : "right", DT * 1000.0, dt * 1000.0, length(com_t - com), length(scaled_axis((target_rot * rot.inverse()).normalized())) * 57.2958,
                           h.force_n, f_capped ? " (cap)" : "", h.torque_nm, t_capped ? " (cap)" : "");
        }
        h.expected_com = com_next;
        h.have_expected = true;
        drain_stamina(side, pawn, h.force_n, h.torque_nm, target_pivot, dt, cfg);
        // Wake it first: a body the solver has put to sleep (a weapon lying still on the ground) may
        // keep the velocity it is given without moving (run 29: the knife stayed frozen on the floor).
        Call wake(base, STR("WakeRigidBody"));
        wake.run();
        Call slv(base, STR("SetPhysicsLinearVelocity"));
        Vec3 set_v = exact_set ? exact_v : (com_next - com) * (1.0 / dt);
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
        gc_sync();
        HSVR_PROF(PerfForceWatch);
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
        UObject* posed = target ? live_posed_pawn() : nullptr;
        if (!target || !posed) return;
        if (!caller || caller == hooked) return; // ours, through ProcessEvent
        UObject* outer = target->GetOuterPrivate();
        if (outer && !is_a(outer, STR("Actor")))
        {
            Call own_call(target, STR("GetOwner"));
            own_call.run();
            if (UObject* o = own_call.object_result()) outer = o;
        }
        UObject* wr = object_property(posed, STR("Weapon R"));
        UObject* wl = object_property(posed, STR("Weapon L"));
        bool weapon = outer && (outer == wr || outer == wl);
        bool own = target == g_pose.mesh || outer == posed;
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

    // ---- Hit vibration (Blade & Sorcery's way) -------------------------------------------------
    // B&S has no haptic effect for hits: the buzz is the hit sound (defaults\bs_haptics.md). Each
    // hit sound layer has a volume from the hit's intensity (InverseLerp of the relative contact
    // speed: 0.25-15 m/s, maces 6-16, punches 2-9) through its volumeIntensityCurve, layers under
    // 0.05 are not played, and every frame a hand gets the sum of its sounds' amplitude envelopes x
    // volume, clamped to 1, as a 1/30 s pulse (controllers_hit). Weapon and shield hits buzz every
    // hand holding either item; a punch only the punching hand; a hit taken on the body, nothing.
    // Contact that stays on (sliding) gives B&S's scrape ticks: 0.8 per 2 cm of slide.
    // Here: our weapon's speed at the contact is the velocity the hold gave it last tick (the hit
    // event comes after the physics step, when the speeds have already changed), the other body's
    // is read at the point. A blow taken on a parked weapon or shield (its own speed ~0, the
    // attacker's already slowed) also gets an intensity from the game's own impulse, so a block
    // is felt by how hard it was hit.
    struct HitCurveKey { float x, y; };
    // Unity curve with flat tangents between keys (a smoothstep per segment).
    double hit_curve(const HitCurveKey* k, int n, double x)
    {
        if (x <= k[0].x) return k[0].y;
        for (int i = 1; i < n; ++i)
            if (x <= k[i].x)
            {
                double u = (x - k[i - 1].x) / (std::max)(1e-6, (double)(k[i].x - k[i - 1].x));
                u = u * u * (3.0 - 2.0 * u);
                return k[i - 1].y + (k[i].y - k[i - 1].y) * u;
            }
        return k[n - 1].y;
    }
    // B&S envelopes (mean over each container's clips), sampled every 50 ms over 0.4 s.
    struct HitEnvelopeDef { const char* name; float v[9]; };
    const HitEnvelopeDef kEnvSword = {"HitSword1", {.66f, .71f, .63f, .37f, .16f, .09f, .09f, .09f, .02f}};
    const HitEnvelopeDef kEnvSwordSoft = {"HitSwordSoft", {.85f, .47f, .29f, .08f, .03f, .03f, .02f, .00f, .00f}};
    const HitEnvelopeDef kEnvSlashFlesh = {"HitSwordSlashOnFlesh", {.00f, .50f, .82f, .71f, .30f, .11f, .06f, .03f, .01f}};
    const HitEnvelopeDef kEnvPierceFlesh = {"HitSwordPierceOnFlesh", {.06f, .60f, .96f, .64f, .27f, .15f, .08f, .04f, .03f}};
    const HitEnvelopeDef kEnvMaceFlesh = {"HitMaceOnFlesh", {.19f, .89f, .61f, .26f, .08f, .08f, .01f, .00f, .00f}};
    const HitEnvelopeDef kEnvPunch = {"Punch", {.02f, .90f, .39f, .10f, .03f, .01f, .00f, .00f, .00f}};
    const HitEnvelopeDef kEnvPunchHeavy = {"PunchHeavy", {.15f, .91f, .81f, .39f, .20f, .10f, .02f, .02f, .01f}};
    const HitEnvelopeDef kEnvMace = {"HitMace", {.09f, .86f, .65f, .24f, .05f, .02f, .01f, .01f, .00f}};
    const HitEnvelopeDef kEnvTick = {"Scrape", {1.0f, .10f, .00f, .00f, .00f, .00f, .00f, .00f, .00f}};
    const HitCurveKey kCurveSoft[] = {{0.0f, 0.0f}, {0.2f, 1.0f}, {1.0f, 0.0f}};
    const HitCurveKey kCurveHard[] = {{0.0f, 0.0f}, {0.1f, 0.0f}, {1.0f, 1.0f}};
    const HitCurveKey kCurveOnFlesh[] = {{0.0f, 0.2f}, {1.0f, 1.0f}};
    const HitCurveKey kCurveOnPlate[] = {{0.0f, 0.5f}, {1.0f, 1.0f}};
    const HitCurveKey kCurvePunch[] = {{0.0f, 0.0f}, {0.5f, 0.6f}, {1.0f, 0.0f}};
    const HitCurveKey kCurvePunchHeavy[] = {{0.0f, 0.2f}, {1.0f, 0.6f}};

    // One sound layer on a hand; returns the volume played (0 = culled).
    double hit_layer(int hand, const HitEnvelopeDef& env, const HitCurveKey* curve, int keys, double intensity, double db_gain, double seconds = 0.4)
    {
        static const float t[9] = {0.0f, 0.125f, 0.25f, 0.375f, 0.5f, 0.625f, 0.75f, 0.875f, 1.0f};
        double vol = hit_curve(curve, keys, intensity) * db_gain * hsvr::config().hit_buzz;
        if (vol < 0.05) return 0.0;
        hsvr::controllers_hit(hand, t, env.v, 9, seconds, (float)vol);
        return vol;
    }

    // A weapon is blunt when none of its damage points (the game's Tips, tags sEdge### / sTip###)
    // is sharp (both under 10): maces, hammers, flails, staffs. Cached per weapon.
    bool weapon_is_blunt(UObject* weapon)
    {
        static std::map<UObject*, bool> cache;
        static bool cache_tracked = on_freed([] { erase_gone_keys(cache); });
        if (!weapon) return false;
        auto it = cache.find(weapon);
        if (it != cache.end()) return it->second;
        int sharp = 0;
        bool any = false;
        if (RawArray* tips = weapon->GetValuePtrByPropertyNameInChain<RawArray>(STR("Tips")); tips && tips->Data)
            for (int32_t i = 0; i < tips->Num; ++i)
                if (UObject* tp = static_cast<UObject*>(tips->Data[i]))
                    if (RawArray* tags = tp->GetValuePtrByPropertyNameInChain<RawArray>(STR("ComponentTags")); tags && tags->Data)
                        for (int32_t j = 0; j < tags->Num; ++j)
                        {
                            std::wstring s = reinterpret_cast<const FName*>(tags->Data)[j].ToString();
                            if (s.size() > 5 && (s.rfind(STR("sEdge"), 0) == 0)) any = true, sharp = (std::max)(sharp, _wtoi(s.c_str() + 5));
                            else if (s.size() > 4 && s.rfind(STR("sTip"), 0) == 0) any = true, sharp = (std::max)(sharp, _wtoi(s.c_str() + 4));
                        }
        bool blunt = any && sharp < 10;
        if (cache.size() > 256) cache.clear();
        cache[weapon] = blunt;
        return blunt;
    }

    // The player's hands holding this item (bit 0 left, bit 1 right).
    // The hit hooks fire for EVERY weapon's Collision Hit and every Willie's mesh hit in the level,
    // thousands a second once dropped weapons and bodies lie in contact (and more substeps as frames
    // slow). Looking up the player controller, pawn and weapons by property name took 20-85 us a call:
    // in perf1 (2026-11-20) 40 of a 76 ms frame after 10 minutes, the cause of the playtest's 63 -> 13
    // fps slide. The game tick takes the pawn and his weapons once a frame; the hooks drop anything
    // that is not his with a few pointer compares before any lookup.
    struct HitFrame
    {
        UObject* pawn = nullptr;
        UObject* weapon[2] = {nullptr, nullptr};
        double hit_buzz = 0.0;
    };
    HitFrame g_hitf;
    void hit_frame_refresh()
    {
        static bool tracked = track_freed(&g_hitf.pawn) && track_freed(&g_hitf.weapon[0]) && track_freed(&g_hitf.weapon[1]);
        (void)tracked;
        g_hitf.hit_buzz = hsvr::config().hit_buzz;
        g_hitf.pawn = live_posed_pawn();
        g_hitf.weapon[0] = g_hitf.pawn ? object_property(g_hitf.pawn, STR("Weapon L")) : nullptr;
        g_hitf.weapon[1] = g_hitf.pawn ? object_property(g_hitf.pawn, STR("Weapon R")) : nullptr;
    }
    bool hit_is_his_item(UObject* x)
    {
        return x && (x == g_hitf.weapon[0] || x == g_hitf.weapon[1] || x == g_hold[0].weapon || x == g_hold[1].weapon || (g_two.active && x == g_two.weapon));
    }
    // The hooked function's OtherActor, from its locals (offset found once per function).
    UObject* hit_other_actor(Unreal::UnrealScriptFunctionCallableContext& context, UFunction* fn)
    {
        static std::unordered_map<UFunction*, int> offsets;
        uint8_t* locals = context.TheStack.Locals();
        if (!fn || !locals) return nullptr;
        auto it = offsets.find(fn);
        if (it == offsets.end())
        {
            int off = -1;
            for (FProperty* p : fn->ForEachProperty())
            {
                std::wstring n = p->GetName();
                if (n == STR("OtherActor") || n == STR("Other Actor")) off = p->GetOffset_Internal();
            }
            it = offsets.emplace(fn, off).first;
        }
        return it->second >= 0 ? *reinterpret_cast<UObject**>(locals + it->second) : nullptr;
    }

    int hit_holders(UObject* pawn, UObject* item)
    {
        if (!item) return 0;
        int m = 0;
        if (g_hold[0].weapon == item || object_property(pawn, STR("Weapon L")) == item) m |= 1;
        if (g_hold[1].weapon == item || object_property(pawn, STR("Weapon R")) == item) m |= 2;
        if (g_two.active && g_two.weapon == item) m |= 3;
        return m;
    }

    // The pre-hit speed of a held item at a point (cm/s): the velocity the hold set last tick.
    bool hit_held_velocity(UObject* pawn, UObject* item, const hsvr::Vec3& at, hsvr::Vec3& out)
    {
        using namespace hsvr;
        for (int side = 0; side < 2; ++side)
        {
            const HoldState& h = g_hold[side];
            if (h.weapon != item || !h.have_last) continue;
            UObject* base = object_property(item, STR("BaseMesh"));
            if (!base) return false;
            Call cm(base, STR("GetCenterOfMass"));
            cm.run();
            Vec3 r = at - cm.vec();
            const Vec3& w = h.last_set_w;
            out = h.last_set_v + Vec3{w.y * r.z - w.z * r.y, w.z * r.x - w.x * r.z, w.x * r.y - w.y * r.x};
            return true;
        }
        return false;
    }

    hsvr::Vec3 hit_body_velocity(UObject* comp, FName bone, const hsvr::Vec3& at)
    {
        if (!comp) return {};
        Call v(comp, STR("GetPhysicsLinearVelocityAtPoint"));
        v.three(STR("Point"), at.x, at.y, at.z);
        v.name(STR("BoneName"), bone);
        v.run();
        return v.vec();
    }

    // Recent contacts per hand: the same thing touched again within 0.2 s is the same contact
    // (UE reports a touching pair every physics step) unless it is clearly harder.
    struct HitContact
    {
        UObject* other = nullptr;
        double last = -10.0, intensity = 0.0;
        hsvr::Vec3 point;
        double slid = 0.0;
    };
    HitContact g_hit_contact[2][4];

    // Returns true when this is a new hit on that hand (and remembers it); scrape ticks for a
    // contact that stays on and slides.
    bool hit_is_new(int hand, UObject* other, double intensity, const hsvr::Vec3& point, double now)
    {
        hsvr::Config cfg = hsvr::config();
        HitContact* slot = nullptr;
        HitContact* oldest = &g_hit_contact[hand][0];
        for (HitContact& c : g_hit_contact[hand])
        {
            if (c.other == other && now - c.last < 0.2) slot = &c;
            if (c.last < oldest->last) oldest = &c;
        }
        if (slot)
        {
            double moved = hsvr::length(point - slot->point);
            if (moved < 30.0) slot->slid += moved; // a jump this big is a new spot, not a slide
            slot->point = point;
            slot->last = now;
            if (intensity > slot->intensity * 1.5 + 0.1)
            {
                slot->intensity = intensity;
                slot->slid = 0.0;
                return true;
            }
            slot->intensity = (std::max)(slot->intensity * 0.9, intensity);
            if (cfg.hit_buzz_scrape > 0.0 && slot->slid >= 2.0)
            {
                slot->slid = std::fmod(slot->slid, 2.0);
                hit_layer(hand, kEnvTick, kCurveOnPlate, 2, 1.0, 0.8 * cfg.hit_buzz_scrape, 0.06);
            }
            return false;
        }
        *oldest = HitContact{other, now, intensity, point, 0.0};
        return true;
    }

    enum class HitSource { Blade, Blunt, Fist };
    enum class HitTarget { Item, Flesh, Plate, World };

    // Plays B&S's sound layers for one hit on the hands in `mask`; returns the summed peak volume.
    double hit_play(int mask, HitSource src, HitTarget tgt, double intensity)
    {
        hsvr::Config cfg = hsvr::config();
        double total = 0.0;
        for (int hand = 0; hand < 2; ++hand)
        {
            if (!(mask & (1 << hand))) continue;
            double v = 0.0;
            if (src == HitSource::Fist)
            {
                v += hit_layer(hand, kEnvPunch, kCurvePunch, 3, intensity, 1.0);
                v += hit_layer(hand, kEnvPunchHeavy, kCurvePunchHeavy, 2, intensity, 1.0);
            }
            else if (tgt == HitTarget::Item)
            {
                const HitEnvelopeDef& hard = src == HitSource::Blunt ? kEnvMace : kEnvSword;
                v += hit_layer(hand, kEnvSwordSoft, kCurveSoft, 3, intensity, 1.0);
                v += hit_layer(hand, hard, kCurveHard, 3, intensity, 1.0);
            }
            else if (tgt == HitTarget::Flesh)
            {
                if (src == HitSource::Blunt) v += hit_layer(hand, kEnvMaceFlesh, kCurveOnFlesh, 2, intensity, 1.26);
                else v += hit_layer(hand, kEnvSlashFlesh, kCurveOnFlesh, 2, intensity, 1.0);
            }
            else if (tgt == HitTarget::Plate)
            {
                // B&S plays a blade on plate silently (only blunt damage passes its filter);
                // hit_buzz_blade_on_plate gives it the metal-on-metal hard layer instead.
                if (src == HitSource::Blunt) v += hit_layer(hand, kEnvMace, kCurveOnPlate, 2, intensity, 1.0);
                else if (cfg.hit_buzz_blade_on_plate) v += hit_layer(hand, kEnvSword, kCurveHard, 3, intensity, 1.0);
            }
            total = (std::max)(total, v);
        }
        return total;
    }

    double inverse_lerp(double a, double b, double x) { return (std::max)(0.0, (std::min)(1.0, (x - a) / (b - a))); }

    // The hit event's FHitResult, broken out (BreakHitResult): what was hit, its bone, our own bone
    // (BoneName = Hit.MyBoneName) and the point.
    struct HitInfo
    {
        UObject* comp = nullptr;
        FName bone, my_bone;
        hsvr::Vec3 point;
        bool ok = false;
    };
    HitInfo break_hit(Unreal::UnrealScriptFunctionCallableContext& context, FProperty* hit_prop)
    {
        HitInfo r;
        if (!hit_prop) return r;
        static UObject* gs = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__GameplayStatics"));
        Call b(gs, STR("BreakHitResult"));
        FProperty* hp = b.param(STR("Hit"));
        if (!hp) return r;
        const uint8_t* src = context.TheStack.Locals() + hit_prop->GetOffset_Internal();
        for (FOutParmRec* o = context.TheStack.OutParms(); o; o = o->NextOutParm)
            if (o->Property == hit_prop && o->PropAddr)
            {
                src = o->PropAddr;
                break;
            }
        std::memcpy(b.buffer.data() + hp->GetOffset_Internal(), src, (std::min)(hp->GetSize(), hit_prop->GetSize()));
        b.run();
        if (FProperty* c = b.param(STR("HitComponent"))) r.comp = *reinterpret_cast<UObject**>(b.buffer.data() + c->GetOffset_Internal());
        if (FProperty* c = b.param(STR("HitBoneName"))) r.bone = *reinterpret_cast<FName*>(b.buffer.data() + c->GetOffset_Internal());
        if (FProperty* c = b.param(STR("BoneName"))) r.my_bone = *reinterpret_cast<FName*>(b.buffer.data() + c->GetOffset_Internal());
        r.point = b.vec(STR("ImpactPoint"));
        r.ok = true;
        return r;
    }

    bool hit_rate_ok(double now)
    {
        static double window = 0.0;
        static int n = 0;
        if (now - window >= 1.0) window = now, n = 0;
        return ++n <= 120;
    }

    // A person's part: his skeletal mesh is flesh, anything else on him (armour pieces) plate.
    HitTarget hit_target_kind(UObject* other_actor, UObject* comp)
    {
        if (!other_actor) return HitTarget::World;
        if (is_a(other_actor, STR("ModularWeaponBP_C"))) return HitTarget::Item;
        if (is_a(other_actor, STR("Willie_BP_C")))
            return comp && is_a(comp, STR("SkeletalMeshComponent")) ? HitTarget::Flesh : HitTarget::Plate;
        return HitTarget::World;
    }

    // ModularWeaponBP "Collision Hit" (before it runs): a weapon hits something. Ours hitting
    // anything, or anyone's hitting an item we hold.
    void hit_haptics_weapon(Unreal::UnrealScriptFunctionCallableContext& context, UFunction* fn)
    {
        using namespace hsvr;
        Config cfg = config();
        if (cfg.hit_buzz <= 0.0 || !fn) return;
        UObject* weapon = context.Context;
        UObject* pawn = live_posed_pawn();
        uint8_t* locals = context.TheStack.Locals();
        if (!weapon || !pawn || !locals) return;
        UObject* other = nullptr;
        UObject* other_comp = nullptr;
        FProperty* hit_prop = nullptr;
        double ni[3] = {0, 0, 0};
        for (FProperty* p : fn->ForEachProperty())
        {
            std::wstring n = p->GetName();
            uint8_t* at = locals + p->GetOffset_Internal();
            if (n == STR("OtherActor") || n == STR("Other Actor")) other = *reinterpret_cast<UObject**>(at);
            else if (n == STR("OtherComp") || n == STR("Other Comp")) other_comp = *reinterpret_cast<UObject**>(at);
            else if (n == STR("NormalImpulse") || n == STR("Normal Impulse")) read_three(at, p, ni);
            else if (n == STR("Hit")) hit_prop = p;
        }
        if (other == pawn) return; // his own body (the game ignores it too)
        int mine = hit_holders(pawn, weapon);
        int theirs = other ? hit_holders(pawn, other) : 0;
        int mask = mine | theirs;
        if (!mask) return;
        double now = now_seconds();
        if (!hit_rate_ok(now)) return;
        HitInfo hit = break_hit(context, hit_prop);
        UObject* target_comp = hit.comp ? hit.comp : other_comp;
        double impulse = length(Vec3{ni[0], ni[1], ni[2]});

        // Relative contact speed (m/s): each side's velocity at the point, ours from the hold.
        Vec3 va, vb;
        if (!hit_held_velocity(pawn, weapon, hit.point, va))
            if (UObject* base = object_property(weapon, STR("BaseMesh"))) va = hit_body_velocity(base, FName(), hit.point);
        if (!other || !hit_held_velocity(pawn, other, hit.point, vb)) vb = hit_body_velocity(target_comp, hit.bone, hit.point);
        double speed = length(va - vb) / 100.0;

        // Who strikes: our weapon (its kind), or theirs on our item (its kind decides the sound).
        bool fist = is_fist(weapon);
        HitSource src = fist ? HitSource::Fist : weapon_is_blunt(weapon) ? HitSource::Blunt : HitSource::Blade;
        HitTarget tgt = hit_target_kind(other, target_comp);
        if (!mine) tgt = HitTarget::Item; // their weapon on the item we hold
        double lo = 0.25, hi = 15.0;
        if (src == HitSource::Fist) lo = 2.0, hi = 9.0;
        else if (src == HitSource::Blunt) lo = 6.0, hi = 16.0;
        double i_speed = inverse_lerp(lo, hi, speed);
        double i_imp = src == HitSource::Fist ? 0.0 : inverse_lerp(15.0, (std::max)(16.0, cfg.hit_buzz_impulse_full), impulse);
        double intensity = (std::max)(i_speed, i_imp);

        UObject* key = mine ? other : weapon;
        // B&S spawns no hit sound for a contact under its range's low end (intensity 0): a fist or
        // blade resting against something is touch, not a hit (test hb-hits2: 0 m/s at volume 0.20).
        if (tgt != HitTarget::World && intensity < cfg.hit_buzz_min_intensity)
        {
            for (int hand = 0; hand < 2; ++hand)
                if (mask & (1 << hand)) hit_is_new(hand, key, intensity, hit.point, now);
            return;
        }
        if (tgt == HitTarget::World)
        {
            // B&S: blades on stone and dirt make no hit buzz, only scrape ticks while sliding.
            for (int hand = 0; hand < 2; ++hand)
                if (mask & (1 << hand)) hit_is_new(hand, target_comp ? target_comp : other, 0.0, hit.point, now);
            return;
        }
        int play = 0;
        for (int hand = 0; hand < 2; ++hand)
            if ((mask & (1 << hand)) && hit_is_new(hand, key, intensity, hit.point, now)) play |= 1 << hand;
        // A punch: only the punching hand.
        if (src == HitSource::Fist) play &= mine;
        if (!play) return;
        double vol = hit_play(play, src, tgt, intensity);
        if (cfg.hit_buzz_log && vol > 0.0)
            HSVR_LOG("Hit buzz: %ls %s %ls (%ls bone %ls) %s, %.1f m/s, impulse %.0f, intensity %.2f (speed %.2f, impulse %.2f), hands %s, volume %.2f",
                     weapon->GetName().c_str(), mine ? "hit" : "hit our", other ? other->GetName().c_str() : STR("?"), target_comp ? target_comp->GetName().c_str() : STR("?"),
                     hit.bone.ToString().c_str(),
                     tgt == HitTarget::Item ? "item" : tgt == HitTarget::Flesh ? "flesh" : "plate", speed, impulse, intensity, i_speed, i_imp,
                     play == 3 ? "both" : play == 1 ? "left" : "right", vol);
    }

    // Willie's mesh OnComponentHit, on the player's pawn: an open hand, forearm or arm (no fist
    // weapon in it) landing on another person is a punch on that side (B&S Punch, 2-9 m/s).
    void hit_haptics_body(Unreal::UnrealScriptFunctionCallableContext& context, void* custom_data)
    {
        gc_sync();
        HSVR_PROF(PerfHitBody);
        // Only his own mesh's hits (every Willie's mesh hit comes through here).
        if (!g_hitf.pawn || context.Context != g_hitf.pawn || g_hitf.hit_buzz <= 0.0) return;
        using namespace hsvr;
        Config cfg = config();
        if (cfg.hit_buzz <= 0.0) return;
        UFunction* fn = static_cast<UFunction*>(custom_data);
        UObject* pawn = live_posed_pawn();
        uint8_t* locals = context.TheStack.Locals();
        if (!fn || !pawn || context.Context != pawn || !locals) return;
        UObject* other = nullptr;
        UObject* other_comp = nullptr;
        FProperty* hit_prop = nullptr;
        for (FProperty* p : fn->ForEachProperty())
        {
            std::wstring n = p->GetName();
            uint8_t* at = locals + p->GetOffset_Internal();
            if (n == STR("OtherActor")) other = *reinterpret_cast<UObject**>(at);
            else if (n == STR("OtherComp")) other_comp = *reinterpret_cast<UObject**>(at);
            else if (n == STR("Hit")) hit_prop = p;
        }
        if (!other || other == pawn || !is_a(other, STR("Willie_BP_C"))) return;
        double now = now_seconds();
        if (!hit_rate_ok(now)) return;
        HitInfo hit = break_hit(context, hit_prop);
        std::wstring bone = hit.my_bone.ToString();
        bool arm = bone.find(STR("hand")) != std::wstring::npos || bone.find(STR("arm")) != std::wstring::npos || bone.find(STR("finger")) != std::wstring::npos ||
                   bone.find(STR("thumb")) != std::wstring::npos || bone.find(STR("index")) != std::wstring::npos || bone.find(STR("middle")) != std::wstring::npos ||
                   bone.find(STR("ring")) != std::wstring::npos || bone.find(STR("pinky")) != std::wstring::npos;
        if (!arm || bone.size() < 2) return;
        int side = bone.size() >= 2 && bone.substr(bone.size() - 2) == STR("_l") ? 0 : bone.substr(bone.size() - 2) == STR("_r") ? 1 : -1;
        if (side < 0) return;
        // A fist weapon in that hand reports its own hit (above).
        if (is_fist(object_property(pawn, side == 0 ? STR("Weapon L") : STR("Weapon R")))) return;
        UObject* mesh = object_property(pawn, STR("Mesh"));
        Vec3 va = hit_body_velocity(mesh, hit.my_bone, hit.point);
        Vec3 vb = hit_body_velocity(hit.comp ? hit.comp : other_comp, hit.bone, hit.point);
        // The arm's speed against him at the contact (not the hand's world speed: walking into him
        // with an arm on him is not a strike).
        double speed = length(va - vb) / 100.0;
        double intensity = inverse_lerp(2.0, 9.0, speed);
        // An arm resting or leaning on him is touch: only a strike from hit_buzz_limb_min_ms counts
        // (hb-hits2: arms on the foe buzzed faintly about once a second at 1.3-2.7 m/s).
        bool strike = speed >= cfg.hit_buzz_limb_min_ms && intensity >= cfg.hit_buzz_min_intensity;
        if (!hit_is_new(side, other, strike ? intensity : 0.0, hit.point, now) || !strike) return;
        double vol = hit_play(1 << side, HitSource::Fist, HitTarget::Flesh, intensity);
        if (cfg.hit_buzz_log && vol > 0.0)
            HSVR_LOG("Hit buzz: %s %ls hit %ls (%ls bone %ls), %.1f m/s, intensity %.2f, volume %.2f", side == 0 ? "left" : "right", bone.c_str(), other->GetName().c_str(),
                     hit.comp ? hit.comp->GetName().c_str() : STR("?"), hit.bone.ToString().c_str(), speed, intensity, vol);
    }

    // ---- Stab log (read only) ------------------------------------------------------------------
    // Why VR stabs bounce (defaults\stabbing_and_ss_reference.md in the decompile folder): the game
    // sticks a blade only when CuttingRate * |NormalImpulse| >= 10000 (ModularWeaponBP @44253-@44512)
    // and the weapon is not "Being Rubber Banded" or "Just Picked Up". This logs, for every hit of a
    // weapon in his hands on a person, what the game was given (a hook on the weapon's own
    // "Collision Hit" event, before it runs) and what it decided (the weapon's variables one tick
    // and 0.15 s later, after its 0.1 s delay), with the controller's speed along the blade.
    struct StabWatch
    {
        UObject* weapon = nullptr;
        int id = 0;
        double hit_at = -1.0;
        bool logged_next = true, logged_late = true;
        bool was_stuck = false;
        UObject* push_comp = nullptr; // the slow push: same body part hit again and again
        double push_since = -1.0, push_last = -1.0;
    };
    StabWatch g_stab_watch[2];
    hsvr::Vec3 g_stab_ctrl_last[2];
    hsvr::Vec3 g_stab_ctrl_vel[2];
    double g_stab_ctrl_time[2] = {-1.0, -1.0};
    int g_stab_count = 0;

    double stab_var(UObject* owner, const TCHAR* name)
    {
        if (!owner) return -999.0;
        FProperty* p = owner->GetPropertyByNameInChain(name);
        if (!p) return -999.0;
        auto kind = p->GetClass().GetName();
        const uint8_t* at = reinterpret_cast<const uint8_t*>(owner) + p->GetOffset_Internal();
        if (kind == STR("ByteProperty") || kind == STR("EnumProperty")) return *at;
        if (kind == STR("IntProperty")) return *reinterpret_cast<const int32_t*>(at);
        return number_property(owner, p);
    }

    int stab_array_num(UObject* owner, const TCHAR* name)
    {
        if (!owner) return -1;
        FProperty* p = owner->GetPropertyByNameInChain(name);
        if (!p || p->GetClass().GetName() != STR("ArrayProperty")) return -1;
        const uint8_t* at = reinterpret_cast<const uint8_t*>(owner) + p->GetOffset_Internal();
        return *reinterpret_cast<const int32_t*>(at + sizeof(void*));
    }

    std::wstring stab_name_var(UObject* owner, const TCHAR* name)
    {
        if (!owner) return STR("?");
        FName* n = owner->GetValuePtrByPropertyNameInChain<FName>(name);
        return n ? n->ToString() : STR("?");
    }

    // The weapon's blade axis (BaseMesh +Z, pommel to tip) and the controller's and the weapon's
    // speed along it (cm/s; positive = toward the tip).
    void stab_speeds(int side, UObject* weapon, double& ctrl_along, double& weapon_along, double& ctrl_speed, double& mass)
    {
        using namespace hsvr;
        ctrl_along = weapon_along = ctrl_speed = mass = 0.0;
        UObject* base = object_property(weapon, STR("BaseMesh"));
        if (!base) return;
        Vec3 axis = component_rotation(base).rotate({0.0, 0.0, 1.0});
        Call lv(base, STR("GetPhysicsLinearVelocity"));
        lv.run();
        Vec3 wv = lv.vec();
        Call m(base, STR("GetMass"));
        m.run();
        mass = m.number();
        Vec3 cv = g_stab_ctrl_vel[side];
        ctrl_along = cv.x * axis.x + cv.y * axis.y + cv.z * axis.z;
        weapon_along = wv.x * axis.x + wv.y * axis.y + wv.z * axis.z;
        ctrl_speed = length(cv);
    }

    void stab_log_state(const char* when, const StabWatch& w)
    {
        UObject* wp = w.weapon;
        UObject* pawn = g_pose.pawn;
        UObject* jammed = object_property(wp, STR("Jammed Tip"));
        UObject* hit_comp = object_property(wp, STR("Hit Comp"));
        HSVR_LOG("Stab: #%d %s: cutting rate %.1f, tip align %.2f, edge align %.2f, stab %.0f, normal impulse %.0f, last hit power %.0f, "
                 "sliding %.0f/%.0f, stuck in body %.0f, stuck joints %d, jammed tip %s, hit %ls bone %ls, surface %.0f, temp no damage %.0f, "
                 "rubber banded %.0f, high threshold %.0f, just picked up %.0f; his soft-not-attached R %.0f L %.0f; just parried %.2f, "
                 "edge sharp %.0f, tip sharp %.0f, rigidity %.2f, mat density %.0f, stab rate %.2f, piercing %.0f",
                 w.id, when, stab_var(wp, STR("Cutting Rate")), stab_var(wp, STR("Tip Alignment")), stab_var(wp, STR("Edge Alignment")),
                 stab_var(wp, STR("Stab?")), stab_var(wp, STR("Normal Impulse")), stab_var(wp, STR("Last Hit Power")), stab_var(wp, STR("Sliding")),
                 stab_var(wp, STR("Sliding 2")), stab_var(wp, STR("Stuck In Body")), stab_array_num(wp, STR("Stuck Constraints Array")),
                 jammed ? "yes" : "no", hit_comp ? hit_comp->GetName().c_str() : STR("none"), stab_name_var(wp, STR("Hit Bone Name")).c_str(),
                 stab_var(wp, STR("Hit Surface")), stab_var(wp, STR("Temp Disable Damage")), stab_var(wp, STR("Being Rubber Banded")),
                 stab_var(wp, STR("High Collision Threshold")), stab_var(wp, STR("Just Picked Up")), stab_var(pawn, STR("R Hand Soft Not Attached")),
                 stab_var(pawn, STR("L Hand Soft Not Attached")), stab_var(wp, STR("Just Parried Rate")), stab_var(wp, STR("Edge Sharpness")),
                 stab_var(wp, STR("Tip Sharpness")), stab_var(wp, STR("Rigidity")), stab_var(wp, STR("Mat Density")), stab_var(wp, STR("Stab Rate")),
                 stab_var(wp, STR("Piercing")));
    }

    void stab_hit_hook(Unreal::UnrealScriptFunctionCallableContext& context, void* custom_data)
    {
        gc_sync();
        HSVR_PROF(PerfStabHit);
        // Only a weapon he holds, or something hitting one he holds (every weapon's hits come here).
        if (!g_hitf.pawn || (!hit_is_his_item(context.Context) && !hit_is_his_item(hit_other_actor(context, static_cast<UFunction*>(custom_data))))) return;
        // Hit vibration first: it reads the game's own impulse before the stab assist may raise it.
        hit_haptics_weapon(context, static_cast<UFunction*>(custom_data));
        {
            hsvr::Config c = hsvr::config();
            if (!c.stab_log && !c.stab_assist) return;
        }
        UObject* weapon = context.Context;
        UObject* pawn = live_posed_pawn();
        if (!weapon || !pawn) return;
        int side = weapon == object_property(pawn, STR("Weapon R")) ? 1 : weapon == object_property(pawn, STR("Weapon L")) ? 0 : -1;
        if (side < 0) return;
        UFunction* fn = static_cast<UFunction*>(custom_data);
        uint8_t* locals = context.TheStack.Locals();
        if (!fn || !locals) return;
        using namespace hsvr;
        Config cfg = config();
        UObject* other = nullptr;
        UObject* part = nullptr; // the weapon's own part that hit
        FProperty* ni_prop = nullptr;
        FProperty* hit_prop = nullptr;
        double ni[3] = {0, 0, 0};
        for (FProperty* p : fn->ForEachProperty())
        {
            std::wstring n = p->GetName();
            uint8_t* at = locals + p->GetOffset_Internal();
            if (n == STR("OtherActor") || n == STR("Other Actor")) other = *reinterpret_cast<UObject**>(at);
            else if (n == STR("HitComp") || n == STR("Hit Comp") || n == STR("Hit Component")) part = *reinterpret_cast<UObject**>(at);
            else if (n == STR("NormalImpulse") || n == STR("Normal Impulse")) read_three(at, p, ni), ni_prop = p;
            else if (n == STR("Hit")) hit_prop = p;
        }
        // People only (the game itself drops hits on the wielder).
        if (!other || other == pawn || !is_a(other, STR("Willie_BP_C"))) return;
        // At most 30 hits a second.
        static double window = 0.0;
        static int in_window = 0;
        double now = now_seconds();
        if (now - window >= 1.0) window = now, in_window = 0;
        if (++in_window > 30) return;

        // What was hit: his component, bone and the impact point (the game's BreakHitResult).
        UObject* victim_comp = nullptr;
        std::wstring bone_s = STR("?");
        FName bone;
        Vec3 impact;
        if (hit_prop)
        {
            static UObject* gs = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__GameplayStatics"));
            Call b(gs, STR("BreakHitResult"));
            if (FProperty* hp = b.param(STR("Hit")))
            {
                // "Hit" is passed by reference: called from script, its value lives at the caller's
                // address (the out-parameter list), not in this frame's locals.
                const uint8_t* src = locals + hit_prop->GetOffset_Internal();
                for (FOutParmRec* o = context.TheStack.OutParms(); o; o = o->NextOutParm)
                    if (o->Property == hit_prop && o->PropAddr)
                    {
                        src = o->PropAddr;
                        break;
                    }
                std::memcpy(b.buffer.data() + hp->GetOffset_Internal(), src, (std::min)(hp->GetSize(), hit_prop->GetSize()));
                b.run();
                if (FProperty* c = b.param(STR("HitComponent"))) victim_comp = *reinterpret_cast<UObject**>(b.buffer.data() + c->GetOffset_Internal());
                // HitBoneName is the bone of what was hit; BoneName is our own (none on a weapon part).
                if (FProperty* c = b.param(STR("HitBoneName"))) bone = *reinterpret_cast<FName*>(b.buffer.data() + c->GetOffset_Internal()), bone_s = bone.ToString();
                impact = b.vec(STR("ImpactPoint"));
            }
        }

        // The tip nearest the impact (the game's own "Tip" markers, the weapon's Tips list) and its
        // forward, the direction a thrust goes in (ModularWeaponBP @36603).
        UObject* tip = nullptr;
        double tip_d = 1e9;
        if (RawArray* tips = weapon->GetValuePtrByPropertyNameInChain<RawArray>(STR("Tips")); tips && tips->Data)
            for (int32_t i = 0; i < tips->Num; ++i)
                if (UObject* t = static_cast<UObject*>(tips->Data[i]))
                {
                    double d = length(component_location(t) - impact);
                    if (d < tip_d) tip_d = d, tip = t;
                }
        Vec3 tip_fwd;
        if (tip)
        {
            Call f(tip, STR("GetForwardVector"));
            f.run();
            tip_fwd = f.vec();
        }
        double ctrl_along = 0, weapon_along = 0, ctrl_speed = 0, mass = 0;
        stab_speeds(side, weapon, ctrl_along, weapon_along, ctrl_speed, mass);
        if (tip)
        {
            // Speeds along the thrust direction (the tip's forward), not the grip's axis.
            UObject* base = object_property(weapon, STR("BaseMesh"));
            Vec3 cv = g_stab_ctrl_vel[side];
            ctrl_along = cv.x * tip_fwd.x + cv.y * tip_fwd.y + cv.z * tip_fwd.z;
            if (base)
            {
                Call lv(base, STR("GetPhysicsLinearVelocity"));
                lv.run();
                Vec3 wv = lv.vec();
                weapon_along = wv.x * tip_fwd.x + wv.y * tip_fwd.y + wv.z * tip_fwd.z;
            }
        }
        // Does the tip point into him: its forward against the hit body's centre.
        double into = -2.0;
        if (tip && victim_comp && bone_s != STR("None") && bone_s != STR("?"))
        {
            Call cm(victim_comp, STR("GetCenterOfMass"));
            cm.name(STR("BoneName"), bone);
            cm.run();
            Vec3 c = cm.vec();
            Vec3 to = c - impact;
            double l = length(to);
            if (l > 0.5) into = (to.x * tip_fwd.x + to.y * tip_fwd.y + to.z * tip_fwd.z) / l;
        }
        bool soft = victim_comp && has_tag(victim_comp, STR("ComponentTags"), STR("Soft"));
        // From the tip's own tags (the game reads them from the closest tip during the hit, so its
        // "Piercing" variable still holds the previous hit's part here, stab-test5).
        bool piercing = tip && has_tag(tip, STR("ComponentTags"), STR("Piercing")) && !has_tag(tip, STR("ComponentTags"), STR("NoStab"));
        bool stuck = bool_property(weapon, STR("Stuck In Body")) || stab_array_num(weapon, STR("Stuck Constraints Array")) > 0;

        StabWatch& w = g_stab_watch[side];
        // Slow push (B&S's pressure rule): the hold keeps pushing the tip into the same place.
        HoldState& h = g_hold[side];
        if (w.push_comp != victim_comp || now - w.push_last > 0.12) w.push_since = now;
        w.push_comp = victim_comp;
        w.push_last = now;
        bool pushing = ctrl_along > 10.0 && h.weapon == weapon && h.force_n >= cfg.stab_push_force_n && now - w.push_since >= cfg.stab_push_time_s;

        double ni_len = length(Vec3{ni[0], ni[1], ni[2]});
        double speed = (std::max)(ctrl_along, weapon_along);
        bool fast = speed >= cfg.stab_min_speed_cms;
        bool aimed = tip && tip_d <= 15.0 && into >= cfg.stab_min_into_cos;
        bool assist = cfg.stab_assist && piercing && soft && !stuck && aimed && (fast || pushing) && ni_prop;
        double boosted = ni_len;
        if (assist)
        {
            // The impulse his arm and body would have put behind the tip: the arm's mass at the
            // controller's speed along the thrust (a slow push counts as stab_min_speed), along the
            // thrust (the game's impulse on the weapon points back out of the wound, @36787).
            double v = fast ? speed : cfg.stab_min_speed_cms;
            boosted = (std::max)(ni_len, cfg.stab_arm_mass_kg * v);
            write_three(locals + ni_prop->GetOffset_Internal(), ni_prop, -tip_fwd.x * boosted, -tip_fwd.y * boosted, -tip_fwd.z * boosted);
        }
        else if (cfg.hit_assist && ni_prop && ni_len > 1.0 && h.weapon == weapon && !stuck)
        {
            // Every other hit on a person (cuts, blunt blows, armour): the game reads the blow from
            // the contact impulse (its hit velocity is the larger of the weapon's speed and that
            // impulse, ModularWeaponBP @40497; a lodge needs cutting rate x impulse >= 10000). In
            // the flat game the arm is jointed behind the weapon and adds its mass to that impulse;
            // the VR hold leaves only the weapon's own (playtest 2026-11-20: a 1.3 kg mace's median
            // impulse 129 on flesh, 46 on a helmet). So his arm's mass, times his strength, is added
            // at the speed the struck point closes on the surface.
            UObject* base = object_property(weapon, STR("BaseMesh"));
            Vec3 n{ni[0] / ni_len, ni[1] / ni_len, ni[2] / ni_len};
            double closing = 0.0;
            if (base)
            {
                Call va(base, STR("GetPhysicsLinearVelocityAtPoint"));
                va.three(STR("Point"), impact.x, impact.y, impact.z);
                va.run();
                Vec3 vp = va.vec();
                closing = std::fabs(dot(vp, n));
                if (closing < 1e-3)
                {
                    Call lv(base, STR("GetPhysicsLinearVelocity"));
                    lv.run();
                    closing = std::fabs(dot(lv.vec(), n));
                }
            }
            if (closing >= cfg.hit_assist_min_cms)
            {
                double strength = (std::max)(0.05, h.body_strength);
                boosted = ni_len + cfg.hit_arm_mass_kg * strength * closing;
                write_three(locals + ni_prop->GetOffset_Internal(), ni_prop, n.x * boosted, n.y * boosted, n.z * boosted);
                HSVR_LOG("Stab: hit assist: %s %ls on %ls %ls: impulse %.0f -> %.0f (struck point closing at %.0f cm/s, arm %.1f kg x strength %.0f%%)",
                         side == 0 ? "left" : "right", weapon->GetName().c_str(), victim_comp ? victim_comp->GetName().c_str() : STR("?"), bone_s.c_str(), ni_len, boosted,
                         closing, cfg.hit_arm_mass_kg, strength * 100.0);
            }
        }

        w.weapon = weapon;
        w.id = ++g_stab_count;
        w.hit_at = now;
        w.logged_next = w.logged_late = false;
        HSVR_LOG("Stab: #%d %s %ls hit %ls %ls bone %ls (part %ls): impulse %.0f kg cm/s%s%.0f, weapon along thrust %.0f cm/s, controller along "
                 "thrust %.0f cm/s (speed %.0f), mass %.2f kg; tip %.1f cm from impact, points into him %.2f, soft %d, piercing %d, stuck %d, push %d",
                 w.id, side == 0 ? "left" : "right", weapon->GetName().c_str(), other->GetName().c_str(), victim_comp ? victim_comp->GetName().c_str() : STR("?"),
                 bone_s.c_str(), part ? part->GetName().c_str() : STR("?"), ni_len, assist ? " -> assisted " : " ", assist ? boosted : 0.0, weapon_along,
                 ctrl_along, ctrl_speed, mass, tip ? tip_d : -1.0, into, (int)soft, (int)piercing, (int)stuck, (int)pushing);
        stab_log_state("before", w);
    }

    // After the game handled the hit: a blade it has just stuck in a body is set into the wound by
    // stab_start_depth_cm along the jammed tip (S&S starts every stab 2 cm in). Without it the tip
    // sits on the skin, the stuck joint's first trace along the blade misses the body and the game
    // lets go on the next tick (stab-test10: 4 of 5 sticks, "hitting body 0" on their first tick).
    void stab_set_depth(int side, UObject* weapon, const char* when);

    // A young stick: the game's stuck joint keeps a blade only while a 3 cm trace along it finds the
    // stuck bone first, from its very first tick, and the game sticks a VR blade at the skin
    // (stab-test13/14: head sticks all let go on that first tick, trace "hit 0"). For the first
    // stab_wound_settle_s its "Spikes (temp)" is set, the game's own way to keep spiked weapons
    // (the keep test then takes any bone, with a 15 cm trace); the wound hold carries the blade in
    // meanwhile, and afterwards the normal test decides (pulling out lets go).
    void stab_hold_keep(int side, UObject* weapon, bool on)
    {
        hsvr::Config cfg = hsvr::config();
        if (!cfg.stab_wound_hold || cfg.stab_wound_settle_s <= 0.0) return;
        if (on && !stuck_is_thrust(weapon)) return; // a lodged cut goes through (cut_follow_through)
        if (on)
        {
            RawArray* joints = weapon->GetValuePtrByPropertyNameInChain<RawArray>(STR("Stuck Constraints Array"));
            UObject* cs = joints && joints->Data && joints->Num > 0 ? static_cast<UObject*>(joints->Data[joints->Num - 1]) : nullptr;
            if (!cs) return;
            if (g_young_joint[side] == cs) return;
            set_variable(cs, STR("Spikes (temp)"), 1.0);
            g_young_trace_ok[side] = 0;
            g_young_joint[side] = cs;
            g_young_since[side] = hsvr::now_seconds();
            HSVR_LOG("Stab: %s stick %ls held open for %.2f s", side == 0 ? "left" : "right", cs->GetName().c_str(), cfg.stab_wound_settle_s);
        }
        else if (UObject* cs = g_young_joint[side])
        {
            g_young_joint[side] = nullptr;
            g_young_since[side] = -1.0;
            RawArray* joints = weapon ? weapon->GetValuePtrByPropertyNameInChain<RawArray>(STR("Stuck Constraints Array")) : nullptr;
            bool alive = false;
            if (joints && joints->Data)
                for (int32_t i = 0; i < joints->Num; ++i) alive = alive || joints->Data[i] == cs;
            if (alive) set_variable(cs, STR("Spikes (temp)"), 0.0);
            HSVR_LOG("Stab: %s stick %s: normal keep test again", side == 0 ? "left" : "right", alive ? "settled" : "already gone");
        }
    }

    void stab_hit_post(Unreal::UnrealScriptFunctionCallableContext& context, void* custom_data)
    {
        gc_sync();
        HSVR_PROF(PerfStabPost);
        using namespace hsvr;
        Config cfg = config();
        if (cfg.stab_start_depth_cm <= 0.0) return;
        UObject* weapon = context.Context;
        UObject* pawn = live_posed_pawn();
        if (!weapon || !pawn) return;
        int side = weapon == object_property(pawn, STR("Weapon R")) ? 1 : weapon == object_property(pawn, STR("Weapon L")) ? 0 : -1;
        if (side < 0) return;
        StabWatch& w = g_stab_watch[side];
        bool stuck = bool_property(weapon, STR("Stuck In Body"));
        bool fresh = stuck && !w.was_stuck;
        w.was_stuck = stuck;
        if (fresh)
        {
            stab_hold_keep(side, weapon, true);
            stab_set_depth(side, weapon, "hit");
        }
    }

    void stab_set_depth(int side, UObject* weapon, const char* when)
    {
        using namespace hsvr;
        Config cfg = config();
        if (cfg.stab_start_depth_cm <= 0.0) return;
        UObject* jt = object_property(weapon, STR("Jammed Tip"));
        UObject* base = object_property(weapon, STR("BaseMesh"));
        if (!jt || !base) return;
        Call f(jt, STR("GetForwardVector"));
        f.run();
        Vec3 u = f.vec();
        double ul = length(u);
        if (ul < 0.5) return;
        u = u * (1.0 / ul);
        double speed = (std::max)(0.0, dot(g_stab_ctrl_vel[side], u));
        double depth = (std::min)(cfg.stab_start_depth_max_cm, cfg.stab_start_depth_cm + speed * cfg.stab_start_depth_per_ms);
        Call off(base, STR("K2_AddWorldOffset"));
        off.three(STR("DeltaLocation"), u.x * depth, u.y * depth, u.z * depth);
        off.flag(STR("bTeleport"), true);
        off.run();
        HSVR_LOG("Stab: %s %ls stuck in %ls (%s): set %.1f cm into the wound (controller %.0f cm/s along it)", side == 0 ? "left" : "right", weapon->GetName().c_str(),
                 stab_name_var(weapon, STR("Hit Bone Name")).c_str(), when, depth, speed);
    }

    void install_stab_log()
    {
        static bool installed = false;
        static double last_try = -10.0;
        if (installed) return;
        double now = hsvr::now_seconds();
        if (now - last_try < 2.0) return;
        last_try = now;
        UFunction* fn = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Game/Assets/Weapons/Blueprints/ModularWeaponBP.ModularWeaponBP_C:Collision Hit"));
        if (!fn) return; // not loaded yet
        installed = true;
        UObjectGlobals::RegisterHook(fn, &stab_hit_hook, &stab_hit_post, fn);
        std::wstring params;
        for (FProperty* p : fn->ForEachProperty()) params += p->GetName() + STR(", ");
        HSVR_LOG("Stab: watching the weapons' Collision Hit (parameters %ls)", params.c_str());
    }

    // Hit vibration needs the weapons' Collision Hit (shared with the stab log) and Willie's mesh hit.
    void install_hit_haptics()
    {
        if (hsvr::config().hit_buzz <= 0.0) return;
        install_stab_log();
        static bool installed = false;
        static double last_try = -10.0;
        if (installed) return;
        double now = hsvr::now_seconds();
        if (now - last_try < 2.0) return;
        last_try = now;
        UFunction* fn = UObjectGlobals::StaticFindObject<UFunction*>(
            nullptr, nullptr, STR("/Game/Character/Blueprints/Willie_BP.Willie_BP_C:BndEvt__BP_ThirdPersonCharacter_Mesh_K2Node_ComponentBoundEvent_0_ComponentHitSignature__DelegateSignature"));
        if (!fn) return;
        installed = true;
        UObjectGlobals::RegisterHook(fn, &hit_haptics_body, [](Unreal::UnrealScriptFunctionCallableContext&, void*) {}, fn);
        HSVR_LOG("Hit buzz: watching the weapons' Collision Hit and Willie's mesh hits (B&S-style hit vibration)");
    }

    // Who lets a stuck blade go (stab-test7: the game stuck thrusts, then let them go within 0.15 s).
    // Logs every call of the stuck joint's Release Constraint and the weapon's release functions for
    // a weapon in his hands, with the calling Blueprint function and bytecode offset (the "@" of the
    // decompile), the stuck joint's own state and the hold's.
    void stab_release_hook(Unreal::UnrealScriptFunctionCallableContext& context, void* custom_data)
    {
        gc_sync();
        HSVR_PROF(PerfStabRelease);
        UObject* self = context.Context;
        UObject* pawn = live_posed_pawn();
        if (!self || !pawn) return;
        UFunction* hooked = static_cast<UFunction*>(custom_data);
        UObject* wr = object_property(pawn, STR("Weapon R"));
        UObject* wl = object_property(pawn, STR("Weapon L"));
        bool is_joint = self->GetName().find(STR("Constraint_Weapon_Stuck_BP")) != std::wstring::npos;
        UObject* weapon = is_joint ? object_property(self, STR("My Weapon")) : self;
        if (!weapon || (weapon != wr && weapon != wl)) return;
        int side = weapon == wr ? 1 : 0;
        // The game calls Temporary Disable Damage right after it sticks a blade (@47985), before the
        // new stuck joint's first tick: the moment to hold a young stick open.
        if (!is_joint && hooked->GetName() == STR("Temporary Disable Damage") && bool_property(weapon, STR("Stuck In Body")))
            stab_hold_keep(side, weapon, true);
        UFunction* caller = context.TheStack.Node();
        // At most one line per function and caller offset each 0.25 s (some are called every tick).
        {
            static std::map<std::wstring, double> last;
            std::wstring key = hooked->GetName() + (caller ? caller->GetName() : STR("")) +
                               std::to_wstring(caller && context.TheStack.Code() ? (long long)(context.TheStack.Code() - caller->GetScript().GetData()) : -1);
            double now = hsvr::now_seconds();
            auto it = last.find(key);
            if (it != last.end() && now - it->second < 0.25) return;
            last[key] = now;
        }
        long long offset = -1;
        if (caller && caller != hooked && context.TheStack.Code())
        {
            const auto& script = caller->GetScript();
            if (script.GetData()) offset = (long long)(context.TheStack.Code() - script.GetData());
        }
        const HoldState& h = g_hold[side];
        if (is_joint)
            HSVR_LOG("Stab: release: %ls.%ls from %ls @%lld; joint: hitting body %.0f, activate %.0f, through %.0f, is meat %.0f, distance %.1f, "
                     "bone mass %.1f, var14 %.0f; hold lag %.1f cm, force %.0f N, controller along %.0f cm/s",
                     self->GetName().c_str(), hooked->GetName().c_str(), caller && caller != hooked ? caller->GetName().c_str() : STR("(ProcessEvent)"), offset,
                     stab_var(self, STR("Hitting Body")), stab_var(self, STR("Activate Constraint")), stab_var(self, STR("Through")), stab_var(self, STR("Is Meat")),
                     stab_var(self, STR("Distance")), stab_var(self, STR("Bone Mass")), stab_var(self, STR("NewVar_14")), h.lag, h.force_n,
                     hsvr::length(g_stab_ctrl_vel[side]));
        else
            HSVR_LOG("Stab: release: %ls.%ls from %ls @%lld; stuck in body %.0f, stuck joints %d; hold lag %.1f cm, force %.0f N", weapon->GetName().c_str(),
                     hooked->GetName().c_str(), caller && caller != hooked ? caller->GetName().c_str() : STR("(ProcessEvent)"), offset,
                     stab_var(weapon, STR("Stuck In Body")), stab_array_num(weapon, STR("Stuck Constraints Array")), h.lag, h.force_n);
    }

    void install_stab_release_watch()
    {
        static bool installed = false;
        static double last_try = -10.0;
        if (installed) return;
        double now = hsvr::now_seconds();
        if (now - last_try < 2.0) return;
        last_try = now;
        UFunction* rc = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Game/Blueprints/Utility/Constraint_Weapon_Stuck_BP.Constraint_Weapon_Stuck_BP_C:Release Constraint"));
        if (!rc) return; // its class loads with the weapons
        installed = true;
        int n = 0;
        for (const TCHAR* path : {STR("/Game/Blueprints/Utility/Constraint_Weapon_Stuck_BP.Constraint_Weapon_Stuck_BP_C:Release Constraint"),
                                  STR("/Game/Blueprints/Utility/Constraint_Weapon_Stuck_BP.Constraint_Weapon_Stuck_BP_C:Event Destroy Self"),
                                  STR("/Game/Assets/Weapons/Blueprints/ModularWeaponBP.ModularWeaponBP_C:Release All Jammed Bones"),
                                  STR("/Game/Assets/Weapons/Blueprints/ModularWeaponBP.ModularWeaponBP_C:GGAGA"),
                                  STR("/Game/Assets/Weapons/Blueprints/ModularWeaponBP.ModularWeaponBP_C:Clear Stuck Bone"),
                                  STR("/Game/Assets/Weapons/Blueprints/ModularWeaponBP.ModularWeaponBP_C:Temporary Disable Damage")})
            if (UFunction* fn = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, path))
            {
                UObjectGlobals::RegisterHook(fn, &stab_release_hook, [](Unreal::UnrealScriptFunctionCallableContext&, void*) {}, fn);
                ++n;
            }
        HSVR_LOG("Stab: watching %d release functions of the stuck joint and the weapon", n);
    }

    // After his tick, before the physics (where the weapons' hits happen): a weapon the mod holds is
    // never "Being Rubber Banded". Willie sets that every tick unless BOTH hands' "Soft Not Attached"
    // are true (UG @459904-@460309); the mod handles the left hand itself, so the game's left flag
    // stays false and the right weapon was rubber-banded on 40% of his hits (playtest 2026-11-19:
    // 475 of 1116, 644 of 1768). A rubber-banded weapon never sticks, deals x0.1 damage clamped to
    // 1000 and needs a 100-1000 impulse to register a hit at all (ModularWeaponBP @38582-@42321):
    // "poking him with a stick".
    void stab_no_rubber_band(UObject* pawn)
    {
        if (!hsvr::config().stab_no_rubber_band || !pawn) return;
        static int cleared[2] = {0, 0};
        static double last_log = 0.0;
        for (int side = 0; side < 2; ++side)
        {
            UObject* weapon = object_property(pawn, side == 0 ? STR("Weapon L") : STR("Weapon R"));
            if (!weapon || g_hold[side].weapon != weapon) continue;
            if (!bool_property(weapon, STR("Being Rubber Banded"))) continue;
            set_variable(weapon, STR("Being Rubber Banded"), 0.0);
            ++cleared[side];
            double now = hsvr::now_seconds();
            if (now - last_log >= 5.0)
            {
                last_log = now;
                HSVR_LOG("Stab: rubber band taken off the held weapons (right %d, left %d ticks so far; his soft-not-attached R %d L %d)", cleared[1], cleared[0],
                         (int)bool_property(pawn, STR("R Hand Soft Not Attached")), (int)bool_property(pawn, STR("L Hand Soft Not Attached")));
            }
        }
    }

    // A cut that lodges goes through when the hand carries on (Matthew 2026-11-20: "the sword just
    // doesn't follow through, it stays stuck in the enemy's body but my hand's somewhere else"). In
    // the flat game his arm keeps pulling on the lodged edge until the stuck joint gives or the
    // blade slides out; under the VR hold the joint never feels that pull (its force reads 0), so
    // a lodged edge (not a thrust) is let go once the hand is cut_release_cm past it.
    void cut_through_tick(UObject* pawn)
    {
        hsvr::Config cfg = hsvr::config();
        if (!cfg.cut_follow_through || !pawn) return;
        static double since[2] = {-1.0, -1.0};
        for (int side = 0; side < 2; ++side)
        {
            UObject* weapon = object_property(pawn, side == 0 ? STR("Weapon L") : STR("Weapon R"));
            if (!weapon || g_hold[side].weapon != weapon || !bool_property(weapon, STR("Stuck In Body")) || stuck_is_thrust(weapon))
            {
                since[side] = -1.0;
                continue;
            }
            double now = hsvr::now_seconds();
            if (since[side] < 0.0) since[side] = now;
            double lag = g_hold[side].lag;
            if (lag >= cfg.cut_release_cm && now - since[side] >= cfg.cut_release_after_s)
            {
                Call r(weapon, STR("Release All Jammed Bones"));
                r.run();
                HSVR_LOG("Stab: %s cut goes through: %ls let out of %ls after %.2f s, hand %.0f cm past it", side == 0 ? "left" : "right", weapon->GetName().c_str(),
                         stab_name_var(weapon, STR("Hit Bone Name")).c_str(), now - since[side], lag);
                since[side] = -1.0;
            }
        }
    }

    // Every tick after his: the controllers' velocity, and the deferred lines after a hit.
    void stab_log_tick(UObject* pawn)
    {
        using namespace hsvr;
        if (!config().stab_log || !pawn) return;
        install_stab_log();
        install_stab_release_watch();
        double now = now_seconds();
        for (int side = 0; side < 2; ++side)
        {
            if (g_pose.hand_valid[side])
            {
                Vec3 at = g_pose.target[side];
                double dt = now - g_stab_ctrl_time[side];
                if (g_stab_ctrl_time[side] > 0.0 && dt > 1e-4 && dt < 0.1)
                {
                    Vec3 v = (at - g_stab_ctrl_last[side]) * (1.0 / dt);
                    g_stab_ctrl_vel[side] = g_stab_ctrl_vel[side] * 0.5 + v * 0.5;
                }
                g_stab_ctrl_last[side] = at;
                g_stab_ctrl_time[side] = now;
            }
            // While a stuck joint holds the weapon: its state every tick, against the game's keep rule
            // (kept while linear + angular constraint force < 100000 x bone mass and the box still
            // overlaps the body, Constraint_Weapon_Stuck_BP @25565-@26093).
            if (UObject* held = object_property(pawn, side == 0 ? STR("Weapon L") : STR("Weapon R")))
            {
                RawArray* joints = held->GetValuePtrByPropertyNameInChain<RawArray>(STR("Stuck Constraints Array"));
                UObject* cs = joints && joints->Data && joints->Num > 0 ? static_cast<UObject*>(joints->Data[0]) : nullptr;
                if (cs)
                {
                    static UObject* pcc = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.PhysicsConstraintComponent"));
                    double lin = -1.0, ang = -1.0;
                    Call gc(cs, STR("GetComponentByClass"));
                    if (FProperty* p = gc.param(STR("ComponentClass")); p && pcc)
                    {
                        *reinterpret_cast<UObject**>(gc.buffer.data() + p->GetOffset_Internal()) = pcc;
                        gc.run();
                        if (UObject* joint = gc.object_result())
                        {
                            Call f(joint, STR("GetConstraintForce"));
                            f.run();
                            lin = length(f.vec(STR("OutLinearForce")));
                            ang = length(f.vec(STR("OutAngularForce")));
                        }
                    }
                    double bm = stab_var(cs, STR("Bone Mass"));
                    // The joint's own keep test, redone here (@35540-@37600): a sphere (3 cm, 15 with
                    // spikes) from the blade's Base, moved toward the Tip by 50 - length for a thrust,
                    // to the Tip, against his body; kept when the first bone hit is the stuck bone.
                    std::wstring trace_bone = STR("-");
                    int trace_hit = -1;
                    UObject* tipc = object_property(cs, STR("Tip"));
                    UObject* basec = object_property(cs, STR("Base"));
                    UObject* bodyc = object_property(cs, STR("Component 2 (Body)"));
                    if (tipc && basec && bodyc)
                    {
                        Vec3 tp = component_location(tipc), bp = component_location(basec);
                        Vec3 d = tp - bp;
                        double len = length(d);
                        Vec3 dir = len > 1e-3 ? d * (1.0 / len) : Vec3{};
                        double shift = bool_property(cs, STR("Thrust?")) ? (std::max)(0.0, 50.0 - len) : 0.0;
                        if (len >= 50.0) shift = 0.0;
                        Vec3 start = bp + dir * shift;
                        Call tr(bodyc, STR("K2_SphereTraceComponent"));
                        tr.three(STR("TraceStart"), start.x, start.y, start.z);
                        tr.three(STR("TraceEnd"), tp.x, tp.y, tp.z);
                        tr.number(STR("SphereRadius"), bool_property(cs, STR("Spikes (temp)")) ? 15.0 : 3.0);
                        tr.run();
                        if (FProperty* rp = tr.param(STR("ReturnValue")))
                        {
                            auto* bp2 = static_cast<FBoolProperty*>(rp);
                            trace_hit = (tr.buffer[rp->GetOffset_Internal() + bp2->GetByteOffset()] & bp2->GetFieldMask()) ? 1 : 0;
                        }
                        if (FProperty* np = tr.param(STR("BoneName"))) trace_bone = reinterpret_cast<FName*>(tr.buffer.data() + np->GetOffset_Internal())->ToString();
                        bool ok = trace_hit == 1 && trace_bone == stab_name_var(cs, STR("Dismember Bone Name"));
                        g_young_trace_ok[side] = ok ? g_young_trace_ok[side] + 1 : 0;
                        HSVR_LOG("Stab: stuck trace: blade %.1f cm, start shifted %.1f, trace hit %d bone %ls, stuck bone %ls",
                                 len, shift, trace_hit, trace_bone.c_str(), stab_name_var(cs, STR("Dismember Bone Name")).c_str());
                    }
                    const HoldState& h = g_hold[side];
                    HSVR_LOG("Stab: stuck tick: joint %ls hitting body %.0f, activate %.0f, distance %.1f, force lin %.0f + ang %.0f vs keep limit %.0f "
                             "(bone mass %.2f); hold lag %.1f cm, force %.0f N, torque %.1f N m",
                             cs->GetName().c_str(), stab_var(cs, STR("Hitting Body")), stab_var(cs, STR("Activate Constraint")), stab_var(cs, STR("Distance")), lin,
                             ang, 100000.0 * bm, bm, h.lag, h.force_n, h.torque_nm);
                }
            }
            StabWatch& w = g_stab_watch[side];
            // A stick the post-hook did not see (the game stuck it later than the hit): set it in now.
            if (UObject* held = object_property(pawn, side == 0 ? STR("Weapon L") : STR("Weapon R")))
            {
                bool stuck = bool_property(held, STR("Stuck In Body"));
                if (stuck && !w.was_stuck)
                {
                    stab_hold_keep(side, held, true);
                    stab_set_depth(side, held, "tick");
                }
                if (g_young_joint[side])
                {
                    double age = now - g_young_since[side];
                    bool earned = age >= config().stab_wound_settle_s && g_young_trace_ok[side] >= 3;
                    if (earned || age >= config().stab_wound_settle_max_s)
                    {
                        HSVR_LOG("Stab: %s stick young window ends after %.2f s: %s", side == 0 ? "left" : "right", age,
                                 earned ? "earned (the normal keep trace found the stuck bone 3 ticks running)" : "capped (stab_wound_settle_max_s)");
                        stab_hold_keep(side, held, false);
                    }
                }
                w.was_stuck = stuck;
            }
            if (!w.weapon || w.hit_at < 0.0) continue;
            if (w.weapon != object_property(pawn, side == 0 ? STR("Weapon L") : STR("Weapon R")))
            {
                w.weapon = nullptr;
                continue;
            }
            if (!w.logged_next && now > w.hit_at)
            {
                w.logged_next = true;
                stab_log_state("next tick", w);
            }
            if (!w.logged_late && now - w.hit_at >= 0.15)
            {
                w.logged_late = true;
                stab_log_state("0.15 s later", w);
            }
        }
    }

    // ---- Reverse grip flip (Saints & Sinners' knife, stabbing_and_ss_reference.md section 6) ----
    // A tap of the holding hand's B (right) / Y (left) turns a one-handed weapon end for end in the
    // hand at the same spot on its handle: forward grip <-> ice-pick grip, the edge kept as it was
    // (the seat's "blade reversed" flip, a half turn about the seat's X). The weapon is not snapped:
    // the hold glides it to the new grip over grip_flip_s (S&S: 200 cm/s and 500 deg/s, ~0.36 s for
    // the half turn), with a short buzz. Not while it is stuck in someone or held in two hands, and
    // not while the other hand's B/Y is held too (both together is the recenter/calibration chord).
    std::atomic<int> g_flip_request{-1}; // hsvr.flip <0|1>: a pretend tap (unattended tests)
    void grip_flip_tick(UObject* pawn)
    {
        using namespace hsvr;
        Config cfg = config();
        if (!cfg.grip_flip || !pawn) return;
        ControllerState state = controllers_state();
        static bool was[2] = {false, false};
        static double down_at[2] = {-1.0, -1.0};
        static bool other_seen[2] = {false, false};
        double now = now_seconds();
        static double flip_at[2] = {-1.0, -1.0};
        static Quat flip_from[2];
        for (int side = 0; side < 2; ++side)
        {
            // For 0.6 s after a flip: how far it has turned, every tick (flip-test1 asked whether the
            // glide really takes grip_flip_s).
            if (flip_at[side] > 0.0)
            {
                UObject* w = object_property(pawn, side == 0 ? STR("Weapon L") : STR("Weapon R"));
                UObject* bm = w ? object_property(w, STR("BaseMesh")) : nullptr;
                if (!bm || now - flip_at[side] > 0.6) flip_at[side] = -1.0;
                else
                {
                    Quat q = component_rotation(bm);
                    double turned = length(scaled_axis((q * flip_from[side].inverse()).normalized())) * 57.29578;
                    HSVR_LOG("Flip: %s +%.3f s: turned %.0f of 180 deg, hold %.1f cm / %.0f deg behind", side == 0 ? "left" : "right", now - flip_at[side], turned,
                             g_hold[side].lag, g_hold[side].turn_deg);
                }
            }
            bool b = state.active && state.hand[side].secondary;
            bool other = state.active && state.hand[1 - side].secondary;
            if (b && !was[side])
            {
                down_at[side] = now;
                other_seen[side] = other;
            }
            if (b && other) other_seen[side] = true;
            bool tap = !b && was[side] && down_at[side] > 0.0 && now - down_at[side] <= cfg.grip_flip_tap_s && !other_seen[side];
            was[side] = b;
            if (g_flip_request.load() == side)
            {
                g_flip_request.store(-1);
                tap = true;
            }
            if (!tap) continue;
            HoldState& h = g_hold[side];
            UObject* weapon = object_property(pawn, side == 0 ? STR("Weapon L") : STR("Weapon R"));
            const char* why = nullptr;
            if (!weapon || h.weapon != weapon) why = "nothing held";
            else if (!h.free.set || h.free.forearm) why = "not held by a handle";
            else if (bool_property(weapon, STR("Stuck In Body"))) why = "it is stuck in a body";
            else if (g_two.active && g_two.weapon == weapon) why = "held in two hands";
            if (why)
            {
                HSVR_LOG("Flip: %s hand: no flip (%s)", side == 0 ? "left" : "right", why);
                continue;
            }
            UObject* base = object_property(weapon, STR("BaseMesh"));
            if (!base) continue;
            h.free.rel = (h.free.rel * Quat{1.0, 0.0, 0.0, 0.0}).normalized();
            h.settle_from_pos = component_location(base);
            h.settle_from_rot = component_rotation(base);
            h.settle_s = (std::max)(0.05, cfg.grip_flip_s);
            h.settle_until = now + h.settle_s;
            h.flipped = !h.flipped;
            flip_at[side] = now;
            flip_from[side] = h.settle_from_rot;
            controllers_buzz(side, 0.35f);
            HSVR_LOG("Flip: %s hand turns %ls end for end (%s grip), over %.2f s", side == 0 ? "left" : "right", weapon->GetName().c_str(),
                     h.flipped ? "reversed" : "forward", h.settle_s);
        }
    }

    // Between a grab and the start of its hold (the game's set-up snaps the item to its own grip and
    // takes several frames to make the hand joint; playtest 3: a sword taken by the blade showed in
    // the hand by its grip for 0.15 s, then jumped 30 cm), the item is kept exactly where the mod's
    // grip has it in the hand, every tick after the game's.
    // ---- Drawing from a scabbard (Matthew 2026-11-19: grab the handle, pull it all the way out
    // along the scabbard; the game's own 85% release kept) -------------------------------------------
    // The game's draw (hs_draw_sheathe_calls.md): Draw Weapon(Hand, Side, Slot) sets the slot bytes and
    // flags; its attach (Set Up Hand Weapon, Weapon UnSheathing, Set Scabbard Constraint Free) is done
    // here at once instead of by the game's arm reaching for the grip. The slot's slide joint keeps the
    // blade on the scabbard's line; the mod moves it only along that line (hold_weapon); the game breaks
    // it free at 0.85 L (swing speed held at 0) and does its bookkeeping; the hold then eases the
    // weapon into the hand. Let go mid-draw and the game seats it again.
    struct SlotDef
    {
        const TCHAR* var;
        int side, slot;
        const TCHAR* constraint;
        const char* socket;
    };
    const SlotDef k_slots[5] = {{STR("Weapon Slot R 1"), 1, 0, STR("Slot R 1 Constraint"), "Sheath_Slot_R_1"},
                                {STR("Weapon Slot R 2"), 1, 1, STR("Slot R 2 Constraint"), "Sheath_Slot_R_2"},
                                {STR("Weapon Slot L 1"), 0, 0, STR("Slot L 1 Constraint"), "Sheath_Slot_L_1"},
                                {STR("Weapon Slot L 2"), 0, 1, STR("Slot L 2 Constraint"), "Sheath_Slot_L_2"},
                                {STR("Weapon Slot Back"), 1, 2, STR("Slot Back Constraint"), "Sheath_Slot_Back"}};

    bool call_one_object(UObject* owner, const TCHAR* name, UObject* arg)
    {
        Call c(owner, name);
        if (!c.fn) return false;
        for (FProperty* p : c.fn->ForEachProperty())
            if (p->GetClass().GetName() == STR("ObjectProperty"))
            {
                *reinterpret_cast<UObject**>(c.buffer.data() + p->GetOffset_Internal()) = arg;
                break;
            }
        return c.run();
    }

    bool try_draw(int side, UObject* pawn, UObject* mesh, const hsvr::Vec3& at, const hsvr::Config& cfg)
    {
        using namespace hsvr;
        int best = -1;
        double best_d = 1e9, best_t = 0.0;
        const Handle* best_h = nullptr;
        for (int i = 0; i < 5; ++i)
        {
            UObject* w = object_property(pawn, k_slots[i].var);
            UObject* base = w ? object_property(w, STR("BaseMesh")) : nullptr;
            if (!base) continue;
            Vec3 bp = component_location(base);
            Quat bq = component_rotation(base);
            Vec3 local = bq.inverse().rotate(at - bp);
            for (const Handle& hd : item_handles(w, base))
            {
                if (hd.name != "grip") continue;
                Vec3 ab = hd.b - hd.a;
                double len2 = dot(ab, ab);
                double u = len2 > 1e-6 ? (std::min)(1.0, (std::max)(0.0, dot(local - hd.a, ab) / len2)) : 0.0;
                double d = length(local - (hd.a + ab * u));
                if (d < best_d)
                {
                    best_d = d;
                    best = i;
                    best_t = u;
                    best_h = &hd;
                }
            }
        }
        if (best < 0) return false;
        const SlotDef& sd = k_slots[best];
        UObject* w = object_property(pawn, sd.var);
        if (best_d > cfg.pose_draw_touch_cm)
        {
            if (best_d < 30.0) HSVR_LOG("Draw: %s hand squeezed %.1f cm from the grip of %ls (needs %.0f)", side == 0 ? "left" : "right", best_d, w->GetName().c_str(), cfg.pose_draw_touch_cm);
            return false;
        }
        const TCHAR* S = sd.side == 0 ? STR("L") : STR("R");
        auto name = [&](const wchar_t* fmt) {
            wchar_t buf[96];
            std::swprintf(buf, 96, fmt, S);
            return std::wstring(buf);
        };
        double conc_r = read_number_variable(pawn, STR("R Sheath Slot Concrete")), conc_l = read_number_variable(pawn, STR("L Sheath Slot Concrete"));
        bool busy = conc_r != 11.0 || conc_l != 11.0 || bool_property(pawn, STR("Weapon UnSheathing R")) || bool_property(pawn, STR("Weapon UnSheathing L")) ||
                    bool_property(pawn, STR("R Weapon Sheathing")) || bool_property(pawn, STR("L Weapon Sheathing"));
        if (busy)
        {
            HSVR_LOG("Draw: %s hand on %ls, but the game is already drawing or sheathing (concrete R %.0f L %.0f)", side == 0 ? "left" : "right", w->GetName().c_str(), conc_r, conc_l);
            return true; // the squeeze is spent on it either way
        }
        Call draw(pawn, STR("Draw Weapon"));
        if (!draw.fn)
        {
            HSVR_LOG("Draw: no \"Draw Weapon\" on the character");
            return false;
        }
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            std::wstring ps;
            for (FProperty* p : draw.fn->ForEachProperty()) ps += p->GetName() + STR(" (") + p->GetClass().GetName() + STR("), ");
            HSVR_LOG("Draw: Draw Weapon takes %ls", ps.c_str());
        }
        draw.byte(STR("Hand"), (uint8_t)side);
        draw.byte(STR("Side"), (uint8_t)sd.side);
        draw.byte(STR("Slot"), (uint8_t)sd.slot);
        draw.run();
        UObject* scab = object_property(w, STR("Scabbard"));
        if (!scab)
        {
            // No scabbard (an axe or mace hung on the belt): just taken, as any grab (Matthew 2026-11-19:
            // only weapons with a real scabbard are drawn). The game frees its belt joint on the first
            // tick it is held (Check Scabbard Unsheath State: no "Scabbard" tag, hs_draw_sheathe_calls.md
            // Addendum 2).
            UObject* base0 = object_property(w, STR("BaseMesh"));
            Vec3 p0 = component_location(base0);
            Quat q0 = component_rotation(base0);
            bool ok0 = give_hand_weapon(side, pawn, w);
            if (!ok0 || object_property(pawn, side == 0 ? STR("Weapon L") : STR("Weapon R")) != w)
            {
                HSVR_LOG("Draw: %s hand could not take %ls (no scabbard) from %ls", side == 0 ? "left" : "right", w->GetName().c_str(), sd.var);
                return true;
            }
            FreeGrip g0 = handle_grip(side, mesh, p0, q0, at, *best_h, best_t);
            g0.hold_at = cfg.pose_grab_settle_s > 0.0;
            g0.at_pos = p0;
            g0.at_rot = q0;
            g0.place = !g0.hold_at;
            g_pending_free[side] = g0;
            g_pending_item[side] = w;
            set_component_transform(base0, p0, q0, true);
            controllers_buzz(side, 0.3f);
            HSVR_LOG("Draw: %s hand takes %ls off %ls (no scabbard: a plain grab, grip at %.0f%%, %.1f cm from the hand)", side == 0 ? "left" : "right",
                     w->GetName().c_str(), sd.var, best_t * 100.0, best_d);
            return true;
        }
        // A hard bottom at the seat: after a seat the game leaves the slide's reference at 0 (a +-L
        // limit, the blade could go in past the seat; draw-test2 reached -33 cm). Back to (0,0,L) as
        // Create Sheath Constraint sets it: travel 0..2L.
        if (UObject* slide = object_property(pawn, sd.constraint))
        {
            Call ref(slide, STR("SetConstraintReferencePosition"));
            ref.byte(STR("Frame"), 1);
            ref.three(STR("RefPos"), 0.0, 0.0, blade_length(w));
            if (!ref.run()) HSVR_LOG("Draw: SetConstraintReferencePosition not found on %ls", sd.constraint);
        }
        else
            HSVR_LOG("Draw: no %ls on the character", sd.constraint);
        UObject* base = object_property(w, STR("BaseMesh"));
        Vec3 w0_pos = component_location(base);
        Quat w0_rot = component_rotation(base);
        Call(pawn, STR("Event Temporary Block Sheathing After Grab")).run();
        bool ok = give_hand_weapon(side, pawn, w);
        UObject* now_held = object_property(pawn, side == 0 ? STR("Weapon L") : STR("Weapon R"));
        if (!ok || now_held != w)
        {
            HSVR_LOG("Draw: %s hand could not take %ls from %ls", side == 0 ? "left" : "right", w->GetName().c_str(), sd.var);
            return true;
        }
        set_component_transform(base, w0_pos, w0_rot, true); // where it sits in the scabbard
        set_bool_property(pawn, name(L"Weapon UnSheathing %ls").c_str(), true);
        set_bool_property(pawn, (std::wstring(sd.side == 0 ? STR("Block L") : STR("Block R")) + STR(" Weapon From Sheathing")).c_str(), true);
        if (scab)
        {
            set_variable(pawn, name(L"Current Scabbard %ls").c_str(), 0.0, scab, nullptr);
            // Locked to the belt (2 cm / 5 deg), not the game's Free (15 cm / 30 deg, for its off hand to
            // take it): draw-test3 dragged a free scabbard 34-71 cm off the belt.
            call_one_object(pawn, cfg_scabbard_free() ? STR("Set Scabbard Constraint Free") : STR("Return Scabbard Constraint To Locked"), scab);
        }
        set_bool_property(pawn, side == 0 ? STR("Grab L Target Found") : STR("Grab R Target Found"), false);
        FreeGrip g = handle_grip(side, mesh, w0_pos, w0_rot, at, *best_h, best_t);
        g.place = false;
        g.hold_at = false;
        g_pending_free[side] = g;
        if (scab && base)
        {
            // Its real mouth for sheathing it again: where the blade leaves the scabbard while seated.
            double z0 = 0.0;
            for (const Handle& hd : item_handles(w, base))
                if (hd.name == "blade") z0 = (std::min)(hd.a.z, hd.b.z);
            Vec3 sp = component_location(scab);
            Quat sq = component_rotation(scab);
            SheathFit f;
            f.scab = scab;
            f.slot_side = sd.side;
            f.slot = sd.slot;
            f.socket = sd.socket;
            f.mouth_local = sq.inverse().rotate(w0_pos + w0_rot.rotate(Vec3{0, 0, z0}) - sp);
            f.axis_local = sq.inverse().rotate(w0_rot.rotate(Vec3{0, 0, 1}));
            g_sheath_fit[w] = f;
            HSVR_LOG("Draw: %ls's scabbard opens at (%.1f, %.1f, %.1f) in its frame, blade along (%.2f, %.2f, %.2f) (blade starts %.1f cm up the weapon)",
                     w->GetName().c_str(), f.mouth_local.x, f.mouth_local.y, f.mouth_local.z, f.axis_local.x, f.axis_local.y, f.axis_local.z, z0);
        }
        DrawState ds;
        ds.active = true;
        ds.slot_side = sd.side;
        ds.weapon = w;
        ds.scabbard = scab;
        ds.t0 = now_seconds();
        ds.length = blade_length(w);
        ds.socket = sd.socket;
        g_draw[side] = ds;
        controllers_buzz(side, 0.3f);
        HSVR_LOG("Draw: %s hand draws %ls from %ls (grip at %.0f%%, %.1f cm from the hand; scabbard %s; sheath slot %s %.0f)", side == 0 ? "left" : "right",
                 w->GetName().c_str(), sd.var, best_t * 100.0, best_d, scab ? "yes" : "none", sd.side == 0 ? "L" : "R",
                 read_number_variable(pawn, sd.side == 0 ? STR("Sheath Slot L") : STR("Sheath Slot R")));
        return true;
    }

    // ---- Sheathing (the reverse of a draw): the blade's tip at an empty belt scabbard's mouth, lined
    // up with it, goes in along its line; the game lets go of it seated (hs_draw_sheathe_calls.md §3) --
    // The mouth is the slot socket (the scabbard sits on it, its +Z down toward the chape). The game's
    // own sheathe (Assign R Weapon Slot -> Sheath R) would send the arm reaching for the socket; the mod
    // calls Assign for its bookkeeping, undoes Sheath R's reach and makes the slide joint at once.
    struct BeltSlot
    {
        const TCHAR* var;
        const char* socket;
    };
    const BeltSlot k_belt[2][2] = {{{STR("Weapon Slot L 1"), "Sheath_Slot_L_1"}, {STR("Weapon Slot L 2"), "Sheath_Slot_L_2"}},
                                   {{STR("Weapon Slot R 1"), "Sheath_Slot_R_1"}, {STR("Weapon Slot R 2"), "Sheath_Slot_R_2"}}};

    double blade_length(UObject* w)
    {
        FProperty* lp = w ? w->GetPropertyByNameInChain(STR("Length to Top R Default")) : nullptr;
        return lp ? (std::max)(10.0, number_property(w, lp)) : 80.0;
    }

    // The empty belt slot (the one the game's Assign picks: 1, else 2) for that hand: its mouth and line.
    bool belt_mouth(int side, UObject* pawn, hsvr::Vec3& mouth, hsvr::Vec3& axis, const char** socket_out = nullptr)
    {
        UObject* mesh = object_property(pawn, STR("Mesh"));
        if (!mesh) return false;
        for (const BeltSlot& b : k_belt[side])
        {
            if (object_property(pawn, b.var)) continue;
            mouth = socket_location(mesh, bone_fname(b.socket));
            axis = socket_rotation(mesh, bone_fname(b.socket)).rotate(hsvr::Vec3{0, 0, 1});
            if (socket_out) *socket_out = b.socket;
            return true;
        }
        return false;
    }

    // ---- Holster snap (B&S), for weapons without a scabbard -------------------------------------
    // The game's own way to hang one (hs_draw_sheathe_calls.md Addendum 2): Assign <S> Weapon Slot picks
    // the slot from the weapon's SheathSlot (0 belt: Sheath_Slot_<S>_1/_2; 1 hafted: _3/_4, both in the
    // Weapon Slot <S> 1/2 variables; 3 Back) and occupancy; Create Sheath Constraint <S> then puts its
    // BaseMesh on that socket and joins it to his mesh there; Set Up <S> Hand Weapon(None) lets go.
    // Drawing it again is the plain no-scabbard grab the draw already does.
    bool holster_snap(int side, UObject* pawn, UObject* mesh, UObject* w, const hsvr::Config& cfg)
    {
        using namespace hsvr;
        if (!cfg.pose_holster_snap || !pawn || !mesh || !w || is_fist(w)) return false;
        const char* hand = side == 0 ? "left" : "right";
        auto no = [&](const char* why) {
            HSVR_LOG("Holster: %s hand let go of %ls: no holster (%s)", hand, w->GetName().c_str(), why);
            return false;
        };
        if (object_property(w, STR("Scabbard"))) return no("it has its own scabbard: the physical sheathe");
        if (g_two.active && g_two.weapon == w) return no("held in two hands");
        if (g_draw[side].active) return no("mid draw or sheathe");
        if (g_hand_world_v[side] > cfg.pose_holster_max_speed) return no("hand too fast: a throw");
        const TCHAR* S = side == 0 ? STR("L") : STR("R");
        auto nm = [&](const wchar_t* fmt) {
            wchar_t buf[96];
            std::swprintf(buf, 96, fmt, S);
            return std::wstring(buf);
        };
        double kind = read_number_variable(w, STR("SheathSlot"));
        std::string socket;
        std::string sx = side == 0 ? "L" : "R";
        if (kind == 0.0 || kind == 1.0)
        {
            int first = kind == 0.0 ? 1 : 3;
            if (!object_property(pawn, nm(L"Weapon Slot %ls 1").c_str())) socket = "Sheath_Slot_" + sx + "_" + std::to_string(first);
            else if (!object_property(pawn, nm(L"Weapon Slot %ls 2").c_str())) socket = "Sheath_Slot_" + sx + "_" + std::to_string(first + 1);
        }
        else if (kind == 3.0 && side == 1 && !object_property(pawn, STR("Weapon Slot Back")))
            socket = "Sheath_Slot_Back";
        if (socket.empty())
        {
            HSVR_LOG("Holster: %s hand let go of %ls: no holster (SheathSlot %.0f: no free slot of that kind on this side)", hand, w->GetName().c_str(), kind);
            return false;
        }
        Vec3 slot_at = socket_location(mesh, bone_fname(socket));
        Vec3 palm = hand_grab_point(mesh, side);
        double d = length(palm - slot_at);
        if (d > cfg.pose_holster_zone_cm)
        {
            HSVR_LOG("Holster: %s hand let go of %ls %.0f cm from %s (needs %.0f): dropped", hand, w->GetName().c_str(), d, socket.c_str(), cfg.pose_holster_zone_cm);
            return false;
        }
        double conc_r = read_number_variable(pawn, STR("R Sheath Slot Concrete")), conc_l = read_number_variable(pawn, STR("L Sheath Slot Concrete"));
        if (conc_r != 11.0 || conc_l != 11.0 || bool_property(pawn, STR("Weapon UnSheathing R")) || bool_property(pawn, STR("Weapon UnSheathing L")) ||
            bool_property(pawn, STR("R Weapon Sheathing")) || bool_property(pawn, STR("L Weapon Sheathing")))
        {
            HSVR_LOG("Holster: %s hand let go of %ls %.0f cm from %s, but the game is mid sheathe/draw (concrete R %.0f L %.0f); dropped", hand, w->GetName().c_str(), d,
                     socket.c_str(), conc_r, conc_l);
            return false;
        }
        Call assign(pawn, nm(L"Assign %ls Weapon Slot").c_str());
        if (!assign.run())
        {
            HSVR_LOG("Holster: no \"Assign %ls Weapon Slot\"", S);
            return false;
        }
        // Undo Sheath <S>'s reach for the socket: it hangs at once.
        set_bool_property(pawn, nm(L"%ls Weapon Sheathing").c_str(), false);
        set_bool_property(pawn, nm(L"Wrist %ls Direct Control").c_str(), false);
        set_bool_property(pawn, nm(L"Grab %ls Target Found").c_str(), false);
        double slot = read_number_variable(pawn, nm(L"Sheath Slot %ls").c_str());
        if (slot < 0.0 || slot > 4.0)
        {
            HSVR_LOG("Holster: the game gave %ls no slot (%.0f); dropped", w->GetName().c_str(), slot);
            return false;
        }
        Call make(pawn, nm(L"Create Sheath Constraint %ls").c_str());
        make.byte(STR("Slot"), (uint8_t)slot);
        bool made = make.run();
        drop_weapon(side, pawn, false);
        controllers_buzz(side, 0.4f);
        HSVR_LOG("Holster: %s hand let go of %ls %.0f cm from %s at %.1f m/s: hung on the belt (SheathSlot %.0f, game slot %.0f, constraint %s; concrete R %.0f L %.0f)",
                 hand, w->GetName().c_str(), d, socket.c_str(), g_hand_world_v[side], kind, slot, made ? "made" : "NOT made",
                 read_number_variable(pawn, STR("R Sheath Slot Concrete")), read_number_variable(pawn, STR("L Sheath Slot Concrete")));
        return true;
    }

    bool sheathe_begin(int side, UObject* pawn, UObject* w)
    {
        using namespace hsvr;
        const TCHAR* S = side == 0 ? STR("L") : STR("R");
        auto nm = [&](const wchar_t* fmt) {
            wchar_t buf[96];
            std::swprintf(buf, 96, fmt, S);
            return std::wstring(buf);
        };
        if (!object_property(w, STR("Scabbard")))
        {
            HSVR_LOG("Sheathe: %ls has no scabbard of its own; not sheathed (only scabbarded weapons for now)", w->GetName().c_str());
            return false;
        }
        double conc_r = read_number_variable(pawn, STR("R Sheath Slot Concrete")), conc_l = read_number_variable(pawn, STR("L Sheath Slot Concrete"));
        if (conc_r != 11.0 || conc_l != 11.0 || bool_property(pawn, STR("Weapon UnSheathing R")) || bool_property(pawn, STR("Weapon UnSheathing L")) ||
            bool_property(pawn, STR("R Weapon Sheathing")) || bool_property(pawn, STR("L Weapon Sheathing")))
            return false;
        Call assign(pawn, nm(L"Assign %ls Weapon Slot").c_str());
        if (!assign.run())
        {
            HSVR_LOG("Sheathe: no \"Assign %ls Weapon Slot\"", S);
            return false;
        }
        // Undo Sheath R's reach for the socket (the mod does the going in).
        set_bool_property(pawn, nm(L"%ls Weapon Sheathing").c_str(), false);
        set_bool_property(pawn, nm(L"Wrist %ls Direct Control").c_str(), false);
        set_bool_property(pawn, nm(L"Grab %ls Target Found").c_str(), false);
        UObject* scab = object_property(pawn, nm(L"Current Scabbard %ls").c_str());
        double slot = read_number_variable(pawn, nm(L"Sheath Slot %ls").c_str());
        if (!scab || slot < 0.0 || slot > 1.0)
        {
            HSVR_LOG("Sheathe: the game picked slot %.0f, scabbard %s: not one the mod sheathes into; left to the game", slot, scab ? "yes" : "none");
            return false;
        }
        set_bool_property(pawn, nm(L"Block %ls Weapon From Sheathing").c_str(), true);
        set_bool_property(pawn, nm(L"%ls Weapon Sheathing").c_str(), true);
        Call make(pawn, nm(L"Create Sheath Constraint %ls").c_str());
        make.byte(STR("Slot"), (uint8_t)slot);
        bool made = make.run();
        // Locked to the belt (2 cm / 5 deg), not the game's Free (15 cm / 30 deg, for its off hand to
            // take it): draw-test3 dragged a free scabbard 34-71 cm off the belt.
            call_one_object(pawn, cfg_scabbard_free() ? STR("Set Scabbard Constraint Free") : STR("Return Scabbard Constraint To Locked"), scab);
        set_bool_property(pawn, nm(L"Weapon UnSheathing %ls").c_str(), true);
        DrawState d;
        d.active = true;
        d.slot_side = side;
        d.weapon = w;
        d.scabbard = scab;
        d.t0 = now_seconds();
        d.sheathing = true;
        d.length = blade_length(w);
        d.socket = slot < 0.5 ? (side == 0 ? "Sheath_Slot_L_1" : "Sheath_Slot_R_1") : (side == 0 ? "Sheath_Slot_L_2" : "Sheath_Slot_R_2");
        g_draw[side] = d;
        controllers_buzz(side, 0.3f);
        HSVR_LOG("Sheathe: %s hand puts %ls into slot %ls %.0f%s", side == 0 ? "left" : "right", w->GetName().c_str(), S, slot + 1.0, made ? "" : " (Create Sheath Constraint NOT found)");
        return true;
    }

    // Across the body (Matthew's playtest 11:25: the rondel drawn by the right hand from the left hip
    // could not go back): the game's own cross path (cross_sheathe.md). Check Scabbard Unsheath State
    // <slot side> already handles "Cross Unsheath": it reads the HAND's concrete and grip, and seats
    // with Set Up <hand> Hand Weapon(None). Create Sheath Constraint <slot side> reads "Weapon <slot
    // side>", so the weapon is written there for that one call only (the other hand must be empty).
    bool sheathe_cross_begin(int side, UObject* pawn, UObject* w, const SheathFit& fit)
    {
        using namespace hsvr;
        int ss = fit.slot_side;
        const wchar_t* H = side == 0 ? L"L" : L"R";
        const wchar_t* S = ss == 0 ? L"L" : L"R";
        auto nm = [](const wchar_t* fmt, const wchar_t* a) {
            wchar_t buf[96];
            std::swprintf(buf, 96, fmt, a);
            return std::wstring(buf);
        };
        const char* hand = side == 0 ? "left" : "right";
        std::wstring slot_var = nm(L"Weapon Slot %ls ", S) + (fit.slot == 0 ? L"1" : L"2");
        if (fit.slot < 0 || fit.slot > 1 || object_property(pawn, slot_var.c_str())) return false;
        if (object_property(pawn, ss == 0 ? STR("Weapon L") : STR("Weapon R")))
        {
            HSVR_LOG_EVERY(2.0, "Sheathe: %s hand can't sheathe across: the other hand holds something", hand);
            return false;
        }
        double conc_r = read_number_variable(pawn, STR("R Sheath Slot Concrete")), conc_l = read_number_variable(pawn, STR("L Sheath Slot Concrete"));
        if (conc_r != 11.0 || conc_l != 11.0 || bool_property(pawn, STR("Weapon UnSheathing R")) || bool_property(pawn, STR("Weapon UnSheathing L")) ||
            bool_property(pawn, STR("R Weapon Sheathing")) || bool_property(pawn, STR("L Weapon Sheathing")))
            return false;
        double hafted = read_number_variable(w, STR("SheathSlot"));
        if (hafted != 0.0 && hafted != 1.0)
        {
            HSVR_LOG_EVERY(2.0, "Sheathe: %ls has sheath slot type %.0f; only belt slots sheathe across", w->GetName().c_str(), hafted);
            return false;
        }
        UObject* scab = fit.scab;
        // The value Draw Weapon writes for that physical slot (L1 5, L2 6, L1/L2 hafted 7/8; R1 0, R2 1, hafted 2/3).
        double concrete = ss == 0 ? (hafted == 0.0 ? 5.0 : 7.0) + fit.slot : (hafted == 0.0 ? 0.0 : 2.0) + fit.slot;
        Vec3 sp = component_location(scab);
        Quat sq = component_rotation(scab);
        FName sock = bone_fname(fit.socket);
        bool ok = set_bool_property(pawn, STR("Cross Unsheath"), true);
        ok &= set_variable(pawn, nm(L"Sheath Slot %ls", S).c_str(), fit.slot);
        ok &= set_variable(pawn, nm(L"%ls Sheath Slot Name", S).c_str(), 0.0, nullptr, &sock);
        ok &= set_variable(pawn, nm(L"%ls Sheath Slot Concrete", H).c_str(), concrete);
        set_variable(pawn, nm(L"Current Scabbard %ls", H).c_str(), 0.0, scab, nullptr);
        set_variable(pawn, nm(L"Current Scabbard %ls", S).c_str(), 0.0, scab, nullptr);
        set_variable(pawn, ss == 0 ? STR("Weapon L") : STR("Weapon R"), 0.0, w, nullptr);
        Call make(pawn, nm(L"Create Sheath Constraint %ls", S).c_str());
        make.byte(STR("Slot"), (uint8_t)fit.slot);
        bool made = make.run();
        set_variable(pawn, ss == 0 ? STR("Weapon L") : STR("Weapon R"), 0.0, nullptr, nullptr);
        set_component_transform(scab, sp, sq, true);
        call_one_object(pawn, cfg_scabbard_free() ? STR("Set Scabbard Constraint Free") : STR("Return Scabbard Constraint To Locked"), scab);
        set_bool_property(pawn, nm(L"%ls Weapon Sheathing", S).c_str(), true);
        set_bool_property(pawn, nm(L"Block %ls Weapon From Sheathing", S).c_str(), true);
        set_bool_property(pawn, nm(L"Weapon UnSheathing %ls", S).c_str(), true);
        DrawState d;
        d.active = true;
        d.slot_side = ss;
        d.weapon = w;
        d.scabbard = scab;
        d.t0 = now_seconds();
        d.sheathing = true;
        d.length = blade_length(w);
        d.socket = fit.socket;
        g_draw[side] = d;
        controllers_buzz(side, 0.3f);
        HSVR_LOG("Sheathe: %s hand puts %ls across into slot %ls %d (concrete %.0f)%s%s", hand, w->GetName().c_str(), S, fit.slot + 1, concrete,
                 made ? "" : " (Create Sheath Constraint NOT found)", ok ? "" : " (a game variable was NOT set)");
        return true;
    }

    // Each tick, for a hand holding a weapon: its tip at its side's empty belt scabbard mouth, lined up.
    void sheathe_check(int side, UObject* pawn, const hsvr::Config& cfg)
    {
        using namespace hsvr;
        if (!cfg.pose_sheathe || g_draw[side].active || g_two.active) return;
        UObject* w = object_property(pawn, side == 0 ? STR("Weapon L") : STR("Weapon R"));
        if (!w || is_fist(w) || g_hold[side].weapon != w || !object_property(w, STR("Scabbard"))) return;
        UObject* base = object_property(w, STR("BaseMesh"));
        if (!base) return;
        Vec3 sock_mouth, sock_ax;
        bool have_sock = belt_mouth(side, pawn, sock_mouth, sock_ax);
        // Its own scabbard's real mouth (measured at the draw), when that scabbard hangs on this hand's
        // side (the game's Assign for this hand only sheathes into this side's slots).
        Vec3 mouth = sock_mouth, ax = sock_ax;
        bool real = false, other_side = false;
        auto fit = g_sheath_fit.find(w);
        bool pinned = fit != g_sheath_fit.end() && fit->second.scab && scabbard_pin(fit->second.scab) != nullptr;
        if (fit != g_sheath_fit.end() && fit->second.scab == object_property(w, STR("Scabbard")) && !pinned)
            HSVR_LOG_EVERY(2.0, "Sheathe: %ls's scabbard is no longer on the belt (the game takes it off when its weapon is dropped); not sheathed",
                           w->GetName().c_str());
        if (fit != g_sheath_fit.end() && fit->second.scab == object_property(w, STR("Scabbard")) && pinned)
        {
            Vec3 sp = component_location(fit->second.scab);
            Quat sq = component_rotation(fit->second.scab);
            if (fit->second.slot_side == side)
            {
                mouth = sp + sq.rotate(fit->second.mouth_local);
                ax = sq.rotate(fit->second.axis_local);
                real = true;
            }
            else
                other_side = true;
        }
        if (!real && !have_sock) return;
        Vec3 bp = component_location(base);
        Quat bq = component_rotation(base);
        Vec3 blade = bq.rotate(Vec3{0, 0, 1});
        Vec3 tip = bp + blade * blade_length(w);
        double d = length(tip - mouth), c = dot(blade, ax);
        double off_deg = std::acos((std::max)(-1.0, (std::min)(1.0, c))) * 57.2958;
        if (!g_sheathe_armed[side])
        {
            if (d > 2.0 * cfg.pose_sheathe_mouth_cm + 10.0)
            {
                g_sheathe_armed[side] = true;
                HSVR_LOG("Sheathe: %s hand's blade is clear of the scabbard (tip %.0f cm from the mouth); it can be sheathed again", side == 0 ? "left" : "right", d);
            }
            return;
        }
        // Playtest 09:38: no sheathe ever began and nothing said how close the tip came. Near a mouth,
        // twice a second: how far and how lined up (and where the belt socket's mouth is, for comparison).
        if (d < 40.0 || other_side)
            HSVR_LOG_EVERY(0.5, "Sheathe: %s hand's tip %.1f cm from the %s mouth, %.0f deg off its line (needs %.0f cm, %.0f deg)%s%s",
                           side == 0 ? "left" : "right", d, real ? "scabbard's" : "belt socket's", off_deg, cfg.pose_sheathe_mouth_cm, cfg.pose_sheathe_angle_deg,
                           real && have_sock ? "; the belt socket's is " : "", real && have_sock ? (std::to_string((int)std::lround(length(tip - sock_mouth))) + " cm").c_str() : "");
        if (other_side)
        {
            // Its own scabbard on the other hip: its real mouth, and the game's cross path.
            Vec3 sp = component_location(fit->second.scab);
            Quat sq = component_rotation(fit->second.scab);
            Vec3 m2 = sp + sq.rotate(fit->second.mouth_local), a2 = sq.rotate(fit->second.axis_local);
            double d2 = length(tip - m2), c2 = dot(blade, a2);
            if (d2 < 40.0)
                HSVR_LOG_EVERY(0.5, "Sheathe: %s hand's tip %.1f cm from the mouth on the other hip, %.0f deg off its line (needs %.0f cm, %.0f deg)",
                               side == 0 ? "left" : "right", d2, std::acos((std::max)(-1.0, (std::min)(1.0, c2))) * 57.2958, cfg.pose_sheathe_mouth_cm,
                               cfg.pose_sheathe_angle_deg);
            if (cfg.pose_sheathe_cross && d2 <= cfg.pose_sheathe_mouth_cm && c2 >= std::cos(cfg.pose_sheathe_angle_deg * 3.14159265358979 / 180.0))
                sheathe_cross_begin(side, pawn, w, fit->second);
            return;
        }
        if (d <= cfg.pose_sheathe_mouth_cm && c >= std::cos(cfg.pose_sheathe_angle_deg * 3.14159265358979 / 180.0)) sheathe_begin(side, pawn, w);
    }

    void draw_tick(UObject* pawn)
    {
        using namespace hsvr;
        const Config& cfg = hsvr::config();
        double now = now_seconds();
        for (int side = 0; side < 2; ++side)
        {
            DrawState& d = g_draw[side];
            if (!d.active) continue;
            const TCHAR* hand = side == 0 ? STR("Weapon L") : STR("Weapon R");
            UObject* base = d.weapon ? object_property(d.weapon, STR("BaseMesh")) : nullptr;
            if (object_property(pawn, hand) != d.weapon || !base)
            {
                if (d.sheathing)
                    HSVR_LOG("Sheathe: %s hand's weapon %s after %.2f s", side == 0 ? "left" : "right",
                             base && bool_property(d.weapon, STR("Sheathed")) ? "is sheathed" : "was let go (the game seats it)", now - d.t0);
                else
                    HSVR_LOG("Draw: %s hand let go mid-draw after %.2f s (the game seats it again)", side == 0 ? "left" : "right", now - d.t0);
                d = DrawState{};
                continue;
            }
            const TCHAR* S = d.slot_side == 0 ? STR("L") : STR("R");
            bool unsheathing = bool_property(pawn, d.slot_side == 0 ? STR("Weapon UnSheathing L") : STR("Weapon UnSheathing R"));
            // The concrete is the HAND's (Draw Weapon @751), the flags the slot side's (cross_sheathe.md).
            double conc = read_number_variable(pawn, side == 0 ? STR("L Sheath Slot Concrete") : STR("R Sheath Slot Concrete"));
            Vec3 bp = component_location(base);
            Quat bq = component_rotation(base);
            double z = d.scabbard ? bq.inverse().rotate(component_location(d.scabbard) - bp).z : 0.0;
            if (d.sheathing && !d.block_cleared && z < 0.9 * d.length)
            {
                // In past the mouth: the game may now seat it (and let the hand go) at 20 cm.
                set_bool_property(pawn, d.slot_side == 0 ? STR("Block L Weapon From Sheathing") : STR("Block R Weapon From Sheathing"), false);
                d.block_cleared = true;
                HSVR_LOG("Sheathe: in past the mouth (%.1f cm out)", z);
            }
            if (!unsheathing && conc == 11.0)
            {
                HSVR_LOG("Draw: %s hand drew %ls free after %.2f s (out %.1f cm)%s", side == 0 ? "left" : "right", d.weapon->GetName().c_str(), now - d.t0, z,
                         d.sheathing ? " (pulled back out of a sheathe)" : "");
                HoldState& h = g_hold[side];
                h.settle_from_pos = bp;
                h.settle_from_rot = bq;
                h.settle_s = (std::max)(0.02, cfg.pose_draw_settle_s);
                h.settle_until = now + h.settle_s;
                d = DrawState{};
                g_sheathe_armed[side] = false;
                continue;
            }
            if (now - d.last_log > 0.5)
            {
                d.last_log = now;
                double off_belt = -1.0;
                if (UObject* mesh = object_property(pawn, STR("Mesh")))
                    if (d.scabbard && d.socket[0]) off_belt = length(component_location(d.scabbard) - socket_location(mesh, bone_fname(d.socket)));
                HSVR_LOG("%s: %s hand %s %ls: out %.1f cm (UnSheathing %ls %d, concrete %.0f; scabbard %.1f cm from %s)", d.sheathing ? "Sheathe" : "Draw",
                         side == 0 ? "left" : "right", d.sheathing ? "sheathing" : "drawing", d.weapon->GetName().c_str(), z, S, (int)unsheathing, conc, off_belt, d.socket);
                // What the game's break reads (Check Scabbard Unsheath State: Z >= L x (Sheathing ? 1 : MRC(Aim Swing Speed 0..75 -> 0.85..0))).
                HSVR_LOG("Draw: break inputs: R Weapon Sheathing %d, L Weapon Sheathing %d, Block R %d, Block L %d, Aim Swing Speed %.1f, swing proxy %.1f, Cross Unsheath %d, length %.1f",
                         (int)bool_property(pawn, STR("R Weapon Sheathing")), (int)bool_property(pawn, STR("L Weapon Sheathing")),
                         (int)bool_property(pawn, STR("Block R Weapon From Sheathing")), (int)bool_property(pawn, STR("Block L Weapon From Sheathing")),
                         read_number_variable(pawn, STR("Aim Swing Speed")), read_number_variable(pawn, STR("Swing Velocity proxy")),
                         (int)bool_property(pawn, STR("Cross Unsheath")), d.length);
            }
            if (now - d.t0 > 20.0)
            {
                HSVR_LOG("Draw: gave up waiting for the game after 20 s");
                d = DrawState{};
            }
        }
        for (int side = 0; side < 2; ++side) sheathe_check(side, pawn, cfg);
    }

    // ---- The off hand on a belt scabbard (Matthew 2026-11-19: the off hand can grab the scabbard and
    // angle it; B&S-style, on a squeeze, not the game's automatic grab) -------------------------------
    // The scabbard is pinned to the pelvis by a PhysicsConstraintComponent that is the scabbard's child 0
    // (Set Scabbard Constraint Free / Return Scabbard Constraint To Locked act on it; component 2 is
    // Mesh:pelvis; hs_scabbard_offhand.md). While held, the mod widens that pin's angular limits to
    // pose_scabbard_angle_deg (its linear limit stays at the locked 2 cm, so it stays on the belt) and
    // turns the scabbard toward the hand's angle; let go and it is turned back to where it sat on the
    // belt before the game's locked limits (5 deg) are put back. The game's own automatic off-hand grab
    // (teleports the scabbard into the left hand) is kept from firing (scabbard_before_tick).
    struct ScabHold
    {
        bool active = false;
        bool returning = false;
        UObject* scab = nullptr;
        hsvr::Quat rel;        // the scabbard's rotation in the hand's frame at the grab
        hsvr::Vec3 touch;      // where the hand took it, in the scabbard's frame
        hsvr::Quat rest;       // its rotation in the pelvis's frame before it was taken
        hsvr::Vec3 rest_pos;   // and its place there (how far it moves off the belt is logged)
        bool test = false;     // hsvr.scabbard_test: a fixed angle on the belt (not a hand's), no let-go
        hsvr::Quat test_rel;   // that angle, in the pelvis's frame
        hsvr::Vec3 last_pelvis; // the pelvis's place last tick (its velocity is carried)
        double last_pelvis_t = -1.0;
        double t0 = 0.0;
        double far_since = -1.0;
        double last_log = -1e9;
        hsvr::Vec3 hand_pos_local; // the hand bone in the scabbard's frame at the grab
        hsvr::Quat hand_rot_local;
        bool hand_set = false;
        // Held as a handle (pose_scabbard_handle): its length along its own Z (z0..z1, measured from its
        // collision), the palm in the hand's frame, and the trigger slide.
        bool seg = false;
        double z0 = 0.0, z1 = 0.0;
        hsvr::Vec3 palm_h;
        bool sliding = false;
        double last_slide_t = -1.0;
    };
    ScabHold g_scab[2];

    // A scabbard's length along its own Z, from its collision: the stretch of its axis within 3.5 cm of it.
    bool scabbard_segment(UObject* scab, double len, double& z0, double& z1)
    {
        using namespace hsvr;
        static std::unordered_map<UObject*, std::pair<double, double>> cache;
        static bool cache_tracked = on_freed([] { erase_gone_keys(cache); });
        auto f = cache.find(scab);
        if (f != cache.end())
        {
            z0 = f->second.first;
            z1 = f->second.second;
            return z1 > z0;
        }
        Vec3 sp = component_location(scab);
        Quat sq = component_rotation(scab);
        double lo = 1e9, hi = -1e9;
        double reach = (std::max)(20.0, len + 10.0);
        for (double z = -reach; z <= reach; z += 2.0)
        {
            Vec3 a = sp + sq.rotate(Vec3{0, 0, z});
            Call cp(scab, STR("GetClosestPointOnCollision"));
            cp.three(STR("Point"), a.x, a.y, a.z);
            if (!cp.run()) return false;
            double d = cp.number();
            if (d >= 0.0 && d <= 3.5)
            {
                lo = (std::min)(lo, z);
                hi = (std::max)(hi, z);
            }
        }
        if (hi <= lo) return false;
        cache[scab] = {lo, hi};
        z0 = lo;
        z1 = hi;
        HSVR_LOG("Scabbard: %ls is a handle from %.0f to %.0f cm along it", scab->GetName().c_str(), lo, hi);
        return true;
    }

    // B&S (as for weapons, pose_hold_hand_follows): his hand holding a scabbard is drawn on the scabbard
    // where it took it, not on your controller (the scabbard is pinned to the belt and only turns).
    bool hand_on_scabbard(int side, hsvr::Vec3& target, hsvr::Quat& hand_rot)
    {
        using namespace hsvr;
        const ScabHold& s = g_scab[side];
        if (!hsvr::config().pose_hold_hand_follows || !s.active || !s.hand_set || !s.scab) return false;
        Vec3 sp = component_location(s.scab);
        Quat sq = component_rotation(s.scab);
        hand_rot = (sq * s.hand_rot_local).normalized();
        target = sp + sq.rotate(s.hand_pos_local);
        return true;
    }

    UObject* scabbard_pin(UObject* scab)
    {
        Call c(scab, STR("GetChildComponent"));
        if (!c.fn) return nullptr;
        if (FProperty* p = c.param(STR("ChildIndex"))) *reinterpret_cast<int32_t*>(c.buffer.data() + p->GetOffset_Internal()) = 0;
        c.run();
        UObject* pin = c.object_result();
        return pin && is_a(pin, STR("PhysicsConstraintComponent")) ? pin : nullptr;
    }

    // SetAngularSwing1Limit / Swing2 / Twist: (motion byte, angle) whatever the parameters are called.
    void pin_angular_limit(UObject* pin, const TCHAR* fn, uint8_t motion, double degrees)
    {
        Call c(pin, fn);
        if (!c.fn) return;
        bool byte_done = false, num_done = false;
        for (FProperty* p : c.fn->ForEachProperty())
        {
            auto kind = p->GetClass().GetName();
            uint8_t* at = c.buffer.data() + p->GetOffset_Internal();
            if (!byte_done && (kind == STR("ByteProperty") || kind == STR("EnumProperty")))
            {
                *at = motion;
                byte_done = true;
            }
            else if (!num_done && kind == STR("FloatProperty"))
            {
                *reinterpret_cast<float*>(at) = (float)degrees;
                num_done = true;
            }
            else if (!num_done && kind == STR("DoubleProperty"))
            {
                *reinterpret_cast<double*>(at) = degrees;
                num_done = true;
            }
        }
        c.run();
    }

    // The pin's limits as they are now (scabbard tests).
    void log_scabbard_pin(UObject* scab, const char* when)
    {
        UObject* pin = scabbard_pin(scab);
        if (!pin) return;
        auto n = [&](std::initializer_list<const TCHAR*> path) { return nested_number(pin, path); };
        HSVR_LOG("Scabbard pin (%s): swing1 %.0f deg motion %.0f, swing2 %.0f motion %.0f, twist %.0f motion %.0f, cone soft %.0f stiff %.0f damp %.0f; "
                 "linear %.1f cm motions %.0f %.0f %.0f soft %.0f; bone2 %s",
                 when, n({STR("ConstraintInstance"), STR("ProfileInstance"), STR("ConeLimit"), STR("Swing1LimitDegrees")}),
                 n({STR("ConstraintInstance"), STR("ProfileInstance"), STR("ConeLimit"), STR("Swing1Motion")}),
                 n({STR("ConstraintInstance"), STR("ProfileInstance"), STR("ConeLimit"), STR("Swing2LimitDegrees")}),
                 n({STR("ConstraintInstance"), STR("ProfileInstance"), STR("ConeLimit"), STR("Swing2Motion")}),
                 n({STR("ConstraintInstance"), STR("ProfileInstance"), STR("TwistLimit"), STR("TwistLimitDegrees")}),
                 n({STR("ConstraintInstance"), STR("ProfileInstance"), STR("TwistLimit"), STR("TwistMotion")}),
                 n({STR("ConstraintInstance"), STR("ProfileInstance"), STR("ConeLimit"), STR("bSoftConstraint")}),
                 n({STR("ConstraintInstance"), STR("ProfileInstance"), STR("ConeLimit"), STR("Stiffness")}),
                 n({STR("ConstraintInstance"), STR("ProfileInstance"), STR("ConeLimit"), STR("Damping")}),
                 n({STR("ConstraintInstance"), STR("ProfileInstance"), STR("LinearLimit"), STR("Limit")}),
                 n({STR("ConstraintInstance"), STR("ProfileInstance"), STR("LinearLimit"), STR("XMotion")}),
                 n({STR("ConstraintInstance"), STR("ProfileInstance"), STR("LinearLimit"), STR("YMotion")}),
                 n({STR("ConstraintInstance"), STR("ProfileInstance"), STR("LinearLimit"), STR("ZMotion")}),
                 n({STR("ConstraintInstance"), STR("ProfileInstance"), STR("LinearLimit"), STR("bSoftConstraint")}),
                 nested_name(pin, {STR("ConstraintInstance"), STR("ConstraintBone2")}).c_str());
    }

    void widen_scabbard_pin(UObject* scab, double degrees)
    {
        UObject* pin = scabbard_pin(scab);
        if (!pin) return;
        pin_angular_limit(pin, STR("SetAngularSwing1Limit"), 1, degrees); // 1 = Limited
        pin_angular_limit(pin, STR("SetAngularSwing2Limit"), 1, degrees);
        pin_angular_limit(pin, STR("SetAngularTwistLimit"), 1, degrees);
    }

    // The scabbards on his belt: the empty ones left by a draw and those of the sheathed weapons.
    std::vector<UObject*> belt_scabbards(UObject* pawn)
    {
        std::vector<UObject*> out;
        auto add = [&](UObject* s) {
            if (s && std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
        };
        add(object_property(pawn, STR("Current Scabbard R")));
        add(object_property(pawn, STR("Current Scabbard L")));
        for (const SlotDef& sd : k_slots)
            if (UObject* w = object_property(pawn, sd.var)) add(object_property(w, STR("Scabbard")));
        return out;
    }

    double scabbard_length(UObject* scab)
    {
        Call c(scab, STR("GetOwner"));
        c.run();
        return blade_length(c.object_result());
    }

    void take_scabbard(int side, UObject* mesh, UObject* scab, const hsvr::Vec3& touch, const hsvr::Quat& hand_rot, const hsvr::Quat& turn, bool test,
                       const hsvr::Config& cfg, const hsvr::Vec3* palm_at = nullptr)
    {
        using namespace hsvr;
        ScabHold& s = g_scab[side];
        s = ScabHold{};
        s.active = true;
        s.test = test;
        s.scab = scab;
        s.touch = touch;
        s.t0 = now_seconds();
        Quat sq = component_rotation(scab);
        // turn (tests only): the scabbard is wanted this much turned from where it is.
        s.rel = (hand_rot.inverse() * turn * sq).normalized();
        Quat pelvis = socket_rotation(mesh, bone_fname("pelvis"));
        s.rest = (pelvis.inverse() * sq).normalized();
        s.rest_pos = pelvis.inverse().rotate(component_location(scab) - socket_location(mesh, bone_fname("pelvis")));
        s.test_rel = (pelvis.inverse() * turn * sq).normalized();
        if (g_pose.hand_valid[side])
        {
            Vec3 sp = component_location(scab);
            s.hand_pos_local = sq.inverse().rotate(g_pose.target[side] - sp);
            s.hand_rot_local = (sq.inverse() * g_pose.hand_rot[side]).normalized();
            s.hand_set = true;
        }
        // As a handle (B&S): it lines up with the hand's grip line (either way along it), and the hand sits
        // on its axis where it closed (pose_scabbard_handle).
        double z0 = 0.0, z1 = 0.0;
        if (!test && palm_at && cfg.pose_scabbard_handle && g_pose.hand_valid[side] && scabbard_segment(scab, scabbard_length(scab), z0, z1))
        {
            Vec3 sp = component_location(scab);
            Vec3 f = normalized(hand_rot.inverse().rotate(sq.rotate(Vec3{0, 0, 1}))), want = hand_grip_axis(side);
            if (dot(f, want) < 0.0) want = want * -1.0;
            Vec3 n = cross(f, want);
            double sn = length(n), c = dot(f, want);
            Quat line_up = sn > 1e-6 ? quat_axis_angle(n * (1.0 / sn), std::atan2(sn, c)) : Quat{};
            s.rel = (line_up * hand_rot.inverse() * sq).normalized();
            double half = cfg.pose_slide_hand_half_cm;
            double lz = sq.inverse().rotate(*palm_at - sp).z;
            double a = z0 + half, b = z1 - half;
            if (b < a) a = b = (z0 + z1) * 0.5;
            s.touch = Vec3{0, 0, (std::min)(b, (std::max)(a, lz))};
            s.palm_h = hand_rot.inverse().rotate(*palm_at - g_pose.target[side]);
            s.hand_rot_local = s.rel.inverse();
            s.hand_pos_local = s.touch - s.hand_rot_local.rotate(s.palm_h);
            s.hand_set = true;
            s.seg = true;
            s.z0 = a;
            s.z1 = b;
            HSVR_LOG("Scabbard: %s hand holds it as a handle at %.1f cm (handle %.0f .. %.0f), turned %.0f deg to lie along the hand's grip line",
                     side == 0 ? "left" : "right", s.touch.z, z0, z1, std::atan2(sn, c) * 57.2958);
        }
        log_scabbard_pin(scab, "before");
        widen_scabbard_pin(scab, cfg.pose_scabbard_angle_deg);
        log_scabbard_pin(scab, "widened");
    }

    bool try_scabbard_grab(int side, UObject* pawn, UObject* mesh, const hsvr::Vec3& at, const hsvr::Config& cfg)
    {
        using namespace hsvr;
        if (!cfg.pose_scabbard_grab || g_scab[1 - side].active) return false;
        UObject* best = nullptr;
        double best_d = 1e9;
        Vec3 best_touch;
        for (UObject* s : belt_scabbards(pawn))
        {
            if (!scabbard_pin(s)) continue; // not pinned to the belt (fallen off)
            Vec3 sp = component_location(s);
            Quat sq = component_rotation(s);
            Vec3 local = sq.inverse().rotate(at - sp);
            double len = scabbard_length(s);
            // Its own collision when it has any (playtest 02:44: squeezes on the scabbard read 12-19 cm
            // from the line the mod guessed); else that line, either way from its origin.
            Vec3 on{0, 0, (std::min)(len, (std::max)(-len, local.z))};
            double d = length(local - on);
            {
                Call cp(s, STR("GetClosestPointOnCollision"));
                cp.three(STR("Point"), at.x, at.y, at.z);
                if (cp.run())
                {
                    double dc = cp.number();
                    if (dc >= 0.0)
                    {
                        d = dc;
                        on = sq.inverse().rotate((dc > 0.0 ? cp.vec(STR("OutPointOnBody")) : at) - sp);
                    }
                }
            }
            if (d < best_d)
            {
                best_d = d;
                best = s;
                best_touch = on;
            }
        }
        if (!best) return false;
        const char* hand = side == 0 ? "left" : "right";
        if (best_d > cfg.pose_scabbard_touch_cm)
        {
            if (best_d < 30.0) HSVR_LOG("Scabbard: %s hand squeezed %.1f cm from a belt scabbard (needs %.0f)", hand, best_d, cfg.pose_scabbard_touch_cm);
            return false;
        }
        if (!g_pose.hand_valid[side]) return false;
        // Only on its handle: within pose_grab_handle_cm of its axis, along its length.
        if (cfg.pose_scabbard_handle)
        {
            double z0 = 0.0, z1 = 0.0;
            if (scabbard_segment(best, scabbard_length(best), z0, z1))
            {
                Vec3 local = component_rotation(best).inverse().rotate(at - component_location(best));
                double d = length(local - Vec3{0, 0, (std::min)(z1, (std::max)(z0, local.z))});
                if (d > cfg.pose_grab_handle_cm)
                {
                    HSVR_LOG("Scabbard: %s hand squeezed %.1f cm from the handle of %ls (needs %.0f): not taken", hand, d, best->GetName().c_str(), cfg.pose_grab_handle_cm);
                    return false;
                }
            }
        }
        take_scabbard(side, mesh, best, best_touch, g_pose.hand_rot[side], Quat{}, false, cfg, &at);
        controllers_buzz(side, 0.3f);
        HSVR_LOG("Scabbard: %s hand took %ls at %.1f cm along it (%.1f cm from the hand); it turns up to %.0f deg on the belt", hand, best->GetName().c_str(),
                 best_touch.z, best_d, cfg.pose_scabbard_angle_deg);
        return true;
    }

    void scabbard_let_go(int side, const char* why)
    {
        ScabHold& s = g_scab[side];
        if (!s.active) return;
        s.active = false;
        s.returning = true;
        s.t0 = hsvr::now_seconds();
        HSVR_LOG("Scabbard: %s hand lets go (%s); turning it back onto the belt", side == 0 ? "left" : "right", why);
    }

    bool scabbard_held(int side) { return g_scab[side].active; }

    // Before his tick: keep the game's automatic off-hand scabbard grab (UG @451824-452352) from firing
    // for the player. Its gate needs "L Hand Occupied by R Unsheath" false and its release fires when it
    // is true while the rest of the gate (C) fails, so it is set to exactly C && a scabbard: neither runs.
    void scabbard_before_tick(UObject* pawn)
    {
        if (!hsvr::config().pose_scabbard_block_auto) return;
        bool c = !bool_property(pawn, STR("Hand L Torn Off")) && !bool_property(pawn, STR("Grabbed L")) && !bool_property(pawn, STR("Grab L Target Found")) &&
                 !bool_property(pawn, STR("Fallen")) && read_number_variable(pawn, STR("Arm_L Health")) > 25.0 && read_number_variable(pawn, STR("L_GripType_Current")) == 0.0 &&
                 (bool_property(pawn, STR("Weapon UnSheathing R")) || bool_property(pawn, STR("R Weapon Sheathing")));
        bool occupied = c && object_property(pawn, STR("Current Scabbard R")) != nullptr;
        if (bool_property(pawn, STR("L Hand Occupied by R Unsheath")) != occupied)
        {
            set_bool_property(pawn, STR("L Hand Occupied by R Unsheath"), occupied);
            HSVR_LOG_EVERY(1.0, "Scabbard: the game's own off-hand scabbard grab held off (occupied %d)", (int)occupied);
        }
    }

    // After his tick: the held scabbard turns toward the hand's angle (its pin keeps it on the belt and
    // within the widened limits); a let-go one turns back to where it sat, then the game's lock returns.
    void scabbard_tick(UObject* pawn, const hsvr::Config& cfg)
    {
        using namespace hsvr;
        UObject* mesh = object_property(pawn, STR("Mesh"));
        if (!mesh) return;
        double now = now_seconds();
        for (int side = 0; side < 2; ++side)
        {
            ScabHold& s = g_scab[side];
            if (!s.active && !s.returning) continue;
            const char* hand = side == 0 ? "left" : "right";
            UObject* pin = s.scab ? scabbard_pin(s.scab) : nullptr;
            if (!pin)
            {
                HSVR_LOG("Scabbard: %s hand's scabbard is no longer on the belt; let go", hand);
                if (s.active) set_anim_grip(side, -1);
                s = ScabHold{};
                continue;
            }
            Quat cur = component_rotation(s.scab);
            Quat want;
            Quat pelvis_q = socket_rotation(mesh, bone_fname("pelvis"));
            Vec3 pelvis_p = socket_location(mesh, bone_fname("pelvis"));
            double off_belt = length(pelvis_q.inverse().rotate(component_location(s.scab) - pelvis_p) - s.rest_pos);
            Vec3 pelvis_v;
            if (s.last_pelvis_t > 0.0 && now - s.last_pelvis_t > 1e-4 && now - s.last_pelvis_t < 0.1) pelvis_v = (pelvis_p - s.last_pelvis) * (1.0 / (now - s.last_pelvis_t));
            s.last_pelvis = pelvis_p;
            s.last_pelvis_t = now;
            if (s.active)
            {
                Quat hand_rot = g_pose.hand_rot[side];
                if (!s.test && !g_pose.hand_valid[side])
                {
                    scabbard_let_go(side, "no controller");
                    continue;
                }
                // The game's draw/sheathe calls put its locked or free limits back: widen again each tick.
                widen_scabbard_pin(s.scab, cfg.pose_scabbard_angle_deg);
                set_anim_grip(side, (int)cfg.pose_scabbard_grip_type);
                want = s.test ? (pelvis_q * s.test_rel).normalized() : (hand_rot * s.rel).normalized();
                // No further than pose_scabbard_angle_deg from its belt angle (the pin's cone is soft,
                // stiffness 50: driven past it, it gives; scabbard-test2 went to 35 at 30).
                {
                    Quat rest_w = (pelvis_q * s.rest).normalized();
                    Vec3 v = scaled_axis((rest_w.inverse() * want).normalized());
                    double a = cfg.pose_scabbard_angle_deg * 3.14159265358979 / 180.0, vl = length(v);
                    if (vl > a) want = (rest_w * from_scaled_axis(v * (a / vl))).normalized();
                }
                // The trigger slides the hand along it (as on a weapon's handle), within its length.
                if (s.seg && !s.test)
                {
                    ControllerState cs = controllers_state();
                    float trig = g_fake_trigger[side].load() >= 0.0f ? g_fake_trigger[side].load() : cs.hand[side].trigger;
                    bool on = s.sliding ? trig > 0.15f : trig > 0.3f;
                    if (on != s.sliding)
                    {
                        s.sliding = on;
                        HSVR_LOG("Scabbard: %s hand %s at %.1f cm (%.0f .. %.0f)", hand, on ? "starts sliding" : "holds", s.touch.z, s.z0, s.z1);
                    }
                    double dt = s.last_slide_t > 0.0 ? (std::min)(1.0 / 30.0, (std::max)(1.0 / 240.0, now - s.last_slide_t)) : 1.0 / 72.0;
                    s.last_slide_t = now;
                    if (on)
                    {
                        Vec3 palm = g_pose.target[side] + hand_rot.rotate(s.palm_h);
                        double wz = cur.inverse().rotate(palm - component_location(s.scab)).z;
                        double k = cfg.pose_slide_rate_min +
                                   (cfg.pose_slide_rate_max - cfg.pose_slide_rate_min) * (std::min)(1.0, (std::max)(0.0, (trig - 0.15) / 0.85));
                        double step = (wz - s.touch.z) * (1.0 - std::exp(-k * dt));
                        double most = cfg.pose_slide_max_cm_s * dt;
                        double nz = (std::min)(s.z1, (std::max)(s.z0, s.touch.z + (std::min)(most, (std::max)(-most, step))));
                        s.hand_pos_local.z += nz - s.touch.z;
                        s.touch.z = nz;
                    }
                }
                Vec3 spot = component_location(s.scab) + cur.rotate(s.touch);
                double off = length(g_pose.target[side] - spot);
                if (!s.test && off > cfg.pose_scabbard_let_go_cm)
                {
                    if (s.far_since < 0.0) s.far_since = now;
                    else if (now - s.far_since > 0.3)
                    {
                        set_anim_grip(side, -1);
                        scabbard_let_go(side, "the hand was pulled off it");
                        continue;
                    }
                }
                else
                    s.far_since = -1.0;
                if (now - s.last_log > 0.5)
                {
                    s.last_log = now;
                    // The turn from its belt angle in its own frame (X, Y, Z parts, deg).
                    Vec3 turn = scaled_axis((s.rest.inverse() * pelvis_q.inverse() * cur).normalized()) * 57.2958;
                    HSVR_LOG("Scabbard: %s hand holds it %.1f deg off its belt angle (own X %.1f Y %.1f Z %.1f), %.1f cm off its belt spot (hand %.1f cm from where it took it; %.1f deg to the hand's angle)",
                             hand, length(turn), turn.x, turn.y, turn.z, off_belt, off, length(scaled_axis((want * cur.inverse()).normalized())) * 57.2958);
                }
            }
            else
            {
                want = (pelvis_q * s.rest).normalized();
                double left = length(scaled_axis((want * cur.inverse()).normalized())) * 57.2958;
                if ((left < 2.0 && off_belt < 1.5) || now - s.t0 > 1.0)
                {
                    UObject* scab = s.scab;
                    s = ScabHold{};
                    bool drawing = false;
                    for (int k = 0; k < 2; ++k) drawing = drawing || g_draw[k].active;
                    call_one_object(pawn, drawing && cfg_scabbard_free() ? STR("Set Scabbard Constraint Free") : STR("Return Scabbard Constraint To Locked"), scab);
                    HSVR_LOG("Scabbard: back on the belt (%.1f deg, %.1f cm off), the game's lock put back", left, off_belt);
                    continue;
                }
            }
            Vec3 err = scaled_axis((want * cur.inverse()).normalized());
            Vec3 w = err * (1.0 / (std::max)(0.01, cfg.pose_scabbard_follow_s));
            double wl = length(w);
            if (wl > 25.0) w = w * (25.0 / wl);
            Call wake(s.scab, STR("WakeRigidBody"));
            wake.run();
            Call sav(s.scab, STR("SetPhysicsAngularVelocityInRadians"));
            sav.three(STR("NewAngVel"), w.x, w.y, w.z);
            sav.flag(STR("bAddToCurrent"), false);
            sav.run();
            // And kept on its spot on the belt (scabbard-test3: a draw dragged it 5.7 cm off, and it
            // re-locked 4.1 cm off).
            Vec3 spot_err = (pelvis_p + pelvis_q.rotate(s.rest_pos)) - component_location(s.scab);
            Vec3 v = spot_err * (1.0 / (std::max)(0.01, cfg.pose_scabbard_follow_s)) + pelvis_v;
            double vl = length(v);
            if (vl > 400.0) v = v * (400.0 / vl);
            Call slv(s.scab, STR("SetPhysicsLinearVelocity"));
            slv.three(STR("NewVel"), v.x, v.y, v.z);
            slv.flag(STR("bAddToCurrent"), false);
            slv.run();
        }
    }

    void keep_pending_in_hand(UObject* pawn)
    {
        using namespace hsvr;
        for (int side = 0; side < 2; ++side)
        {
            UObject* item = g_pending_item[side];
            const FreeGrip& g = g_pending_free[side];
            if (!item || !g.set) continue;
            if (object_property(pawn, side == 0 ? STR("Weapon L") : STR("Weapon R")) != item || g_hold[side].weapon == item)
            {
                if (g_hold[side].weapon == item || !g.set) g_pending_item[side] = nullptr;
                continue;
            }
            UObject* base = object_property(item, STR("BaseMesh"));
            if (!base || !g_pose.hand_valid[side]) continue;
            if (g.hold_at)
            {
                set_component_transform(base, g.at_pos, g.at_rot, true);
                continue;
            }
            Vec3 hp = g_pose.target[side];
            Quat hr = g_pose.hand_rot[side];
            Quat rot = (hr * g.rel).normalized();
            Vec3 pos = hp + hr.rotate(g.in_anchor) - rot.rotate(g.pivot);
            set_component_transform(base, pos, rot, true);
        }
    }

    void weapon_spring_after_tick(UObject* pawn)
    {
        using namespace hsvr;
        Config cfg = config();
        keep_pending_in_hand(pawn);
        draw_tick(pawn);
        scabbard_tick(pawn, cfg);
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
            static bool sprung_tracked = on_freed([] { for (UObject*& o : sprung) if (gone(o)) o = nullptr; });
            if (!spring_on && !hold_on && joint && sprung[side] == joint)
            {
                sprung[side] = nullptr;
                lock_joint(joint);
                HSVR_LOG("Grip: %s hand back on the game's locked grip: %s", side == 0 ? "left" : "right", joint_settings(joint).c_str());
            }
            if ((spring_on || hold_on) && joint) sprung[side] = joint;
            // With the spring off the game's own locked grip holds the weapon; it is still measured and logged.
            UObject* weapon = object_property(pawn, side == 0 ? STR("Weapon L") : STR("Weapon R"));
            // (Not the second hand's: it holds no weapon of its own, but must not collide with the one it holds too.)
            if (g_arm_ignore[side].weapon && g_arm_ignore[side].weapon != weapon && !(g_two.active && g_two.second == side && g_arm_ignore[side].weapon == g_two.weapon))
                clear_arm_ignore(side);
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
                static bool sprung_once_tracked = on_freed([] { for (UObject*& o : sprung_once) if (gone(o)) o = nullptr; });
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
                    static bool broken_tracked = on_freed([] { for (UObject*& o : broken) if (gone(o)) o = nullptr; });
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
                    static bool typed_tracked = track_freed(&typed);
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
                    static bool anchored_tracked = on_freed([] { for (UObject*& o : anchored) if (gone(o)) o = nullptr; });
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
                    static bool original_for_tracked = track_freed(&original_for);
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
                    static bool applied_to_tracked = track_freed(&applied_to);
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
                static bool dumped_tracked = on_freed([] { for (UObject*& o : dumped) if (gone(o)) o = nullptr; });
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

    // ---- Deep recording (log_frames; Matthew 2026-11-19: "make it as deep as you can") ------------------
    // logs\deep-<time>\: frames.csv (one row per tick of his: headset, controllers and every input, his
    // drawn hands and every physics body of his, each held item and its hold, crouch, game state),
    // state.txt (every change of every plain variable on him, his movement, his held weapons and every
    // fighter within 10 m), events.txt (every Blueprint function or event the engine calls on a fighter,
    // a weapon or an AI, with all its parameters) and creatures.txt (fighters within 10 m, 10 a second).
    std::wstring g_deep_dir;
    FILE* g_frames = nullptr;
    FILE* g_state = nullptr;
    FILE* g_events = nullptr;
    FILE* g_creatures = nullptr;
    double g_frames_flush = 0.0;
    std::vector<FName> g_frame_bones;

    std::string narrow(const std::wstring& w)
    {
        std::string s;
        s.reserve(w.size());
        for (wchar_t c : w) s.push_back(c > 0 && c < 128 ? (char)c : '?');
        return s;
    }

    FILE* deep_open(const wchar_t* name)
    {
        if (g_deep_dir.empty())
        {
            std::wstring base = hsvr::mod_dir() + L"logs\\";
            CreateDirectoryW(base.c_str(), nullptr);
            SYSTEMTIME st{};
            GetLocalTime(&st);
            wchar_t dir[64];
            swprintf(dir, 64, L"deep-%04u-%02u-%02u_%02u-%02u-%02u\\", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
            g_deep_dir = base + dir;
            CreateDirectoryW(g_deep_dir.c_str(), nullptr);
            HSVR_LOG("Deep log: recording to logs\\%ls", dir);
        }
        FILE* f = _wfopen((g_deep_dir + name).c_str(), L"wb");
        if (f) setvbuf(f, nullptr, _IOFBF, 1 << 20);
        return f;
    }

    // A property's value as text, for the plain kinds (false: a kind not written).
    bool prop_text(FProperty* p, const uint8_t* at, std::string& out)
    {
        auto k = p->GetClass().GetName();
        char b[96];
        if (k == STR("BoolProperty"))
        {
            auto* f = static_cast<FBoolProperty*>(p);
            out = (at[f->GetByteOffset()] & f->GetFieldMask()) ? "1" : "0";
        }
        else if (k == STR("ByteProperty") || k == STR("EnumProperty")) out = std::to_string((int)*at);
        else if (k == STR("IntProperty")) out = std::to_string(*reinterpret_cast<const int32_t*>(at));
        else if (k == STR("Int64Property")) out = std::to_string(*reinterpret_cast<const int64_t*>(at));
        else if (k == STR("FloatProperty")) snprintf(b, sizeof b, "%.5g", *reinterpret_cast<const float*>(at)), out = b;
        else if (k == STR("DoubleProperty")) snprintf(b, sizeof b, "%.5g", *reinterpret_cast<const double*>(at)), out = b;
        else if (k == STR("NameProperty")) out = narrow(reinterpret_cast<const FName*>(at)->ToString());
        else if (k == STR("ObjectProperty"))
        {
            UObject* o = *reinterpret_cast<UObject* const*>(at);
            out = o ? narrow(o->GetName()) : "None";
        }
        else if (k == STR("StructProperty"))
        {
            UScriptStruct* t = struct_type(p);
            std::wstring tn = t ? t->GetName() : STR("");
            if (tn != STR("Vector") && tn != STR("Rotator")) return false;
            double v[3];
            read_three(at, p, v);
            snprintf(b, sizeof b, "(%.2f;%.2f;%.2f)", v[0], v[1], v[2]);
            out = b;
        }
        else
            return false;
        return true;
    }

    // Every plain variable of an object, written when it changes (all of them the first time).
    struct Watched
    {
        std::vector<FProperty*> props;
        std::vector<std::vector<uint8_t>> last;
        bool first = true;
        UStruct* cls = nullptr; // a freed object's address taken by another kind of object starts afresh
    };
    std::unordered_map<UObject*, Watched> g_watch;
    void state_delta(UObject* o, const char* label, double t)
    {
        if (!o || !g_state) return;
        Watched& w = g_watch[o];
        if (w.cls != o->GetClassPrivate()) w = Watched{}, w.cls = o->GetClassPrivate();
        if (w.props.empty())
        {
            for (UStruct* level = o->GetClassPrivate(); level; level = level->GetSuperStruct())
                for (FProperty* p : level->ForEachProperty())
                {
                    std::string probe;
                    static const uint8_t zero[64] = {};
                    if (p->GetSize() <= 0 || p->GetSize() > 64) continue;
                    auto k = p->GetClass().GetName();
                    if (k == STR("StructProperty"))
                    {
                        UScriptStruct* st = struct_type(p);
                        if (!st || (st->GetName() != STR("Vector") && st->GetName() != STR("Rotator"))) continue;
                    }
                    if (!prop_text(p, zero, probe)) continue;
                    w.props.push_back(p);
                }
            w.last.resize(w.props.size());
        }
        std::string out;
        for (size_t i = 0; i < w.props.size(); ++i)
        {
            FProperty* p = w.props[i];
            const uint8_t* at = reinterpret_cast<const uint8_t*>(o) + p->GetOffset_Internal();
            int n = p->GetSize();
            std::vector<uint8_t>& last = w.last[i];
            if (!w.first && (int)last.size() == n && std::memcmp(last.data(), at, n) == 0) continue;
            last.assign(at, at + n);
            std::string v;
            if (!prop_text(p, at, v)) continue;
            out += "\t" + narrow(p->GetName()) + "=" + v;
        }
        w.first = false;
        if (!out.empty()) fprintf(g_state, "%.4f\t%s\t%s%s\n", t, label, narrow(o->GetName()).c_str(), out.c_str());
    }

    // Every Blueprint function or event the engine calls on a fighter, a weapon or an AI.
    std::unordered_map<UStruct*, bool> g_event_class;
    std::unordered_map<UFunction*, std::pair<double, int>> g_event_rate;
    void deep_event_cb(Hook::TCallbackIterationData<void>&, UObject* context, UFunction* function, void* parms)
    {
        gc_sync();
        HSVR_PROF(PerfDeepEvent);
        if (!g_events || !context || !function) return;
        UStruct* cls = context->GetClassPrivate();
        auto it = g_event_class.find(cls);
        bool want;
        if (it == g_event_class.end())
        {
            want = false;
            for (UStruct* level = cls; level && !want; level = level->GetSuperStruct())
            {
                std::wstring n = level->GetName();
                want = n == STR("Willie_BP_C") || n == STR("ModularWeaponBP_C") || n.find(STR("AI_BP")) == 0 || n.find(STR("BP_GameManager")) == 0 ||
                       n.find(STR("GameMode")) != std::wstring::npos;
            }
            g_event_class[cls] = want;
        }
        else
            want = it->second;
        if (!want) return;
        std::wstring fname = function->GetName();
        if (fname == STR("ReceiveTick") || fname == STR("BlueprintUpdateAnimation") || fname.rfind(STR("ExecuteUbergraph"), 0) == 0) return;
        double t = hsvr::now_seconds();
        auto& r = g_event_rate[function];
        if (t - r.first >= 1.0)
        {
            if (r.second > 40) fprintf(g_events, "%.4f\t(%s: %d more calls in the last second not written)\n", t, narrow(fname).c_str(), r.second - 40);
            r.first = t;
            r.second = 0;
        }
        if (++r.second > 40) return;
        std::string line;
        char b[64];
        snprintf(b, sizeof b, "%.4f\t", t);
        line = b;
        line += narrow(context->GetName()) + "\t" + narrow(fname);
        if (parms)
            for (FProperty* p : function->ForEachProperty())
            {
                std::string v;
                if (p->GetSize() > 0 && prop_text(p, reinterpret_cast<const uint8_t*>(parms) + p->GetOffset_Internal(), v))
                    line += "\t" + narrow(p->GetName()) + "=" + v;
            }
        line += "\n";
        fputs(line.c_str(), g_events);
    }

    // Fighters within 10 m, 10 a second: where they are, their pelvis, head and right hand, and what they hold.
    void creatures_tick(UObject* pawn, double t)
    {
        static double last = 0.0;
        if (t - last < 0.1) return;
        last = t;
        std::vector<UObject*> willies;
        UObjectGlobals::FindAllOf(STR("Willie_BP_C"), willies);
        hsvr::Vec3 me = actor_location(pawn);
        for (UObject* w : willies)
        {
            if (!w || w == pawn) continue;
            hsvr::Vec3 at = actor_location(w);
            if (hsvr::length(at - me) > 1000.0) continue;
            UObject* m = object_property(w, STR("Mesh"));
            UObject* wr = object_property(w, STR("Weapon R"));
            UObject* wl = object_property(w, STR("Weapon L"));
            if (g_creatures && m)
            {
                hsvr::Vec3 pe = socket_location(m, bone_fname("pelvis")), hd = socket_location(m, bone_fname("head")), hr = socket_location(m, bone_fname("hand_r"));
                fprintf(g_creatures, "%.4f\t%s\tactor (%.1f;%.1f;%.1f)\tpelvis (%.1f;%.1f;%.1f)\thead (%.1f;%.1f;%.1f)\thand_r (%.1f;%.1f;%.1f)\tR %s\tL %s\n", t,
                        narrow(w->GetName()).c_str(), at.x, at.y, at.z, pe.x, pe.y, pe.z, hd.x, hd.y, hd.z, hr.x, hr.y, hr.z, wr ? narrow(wr->GetName()).c_str() : "-",
                        wl ? narrow(wl->GetName()).c_str() : "-");
            }
            state_delta(w, "foe", t);
            if (wr) state_delta(wr, "foe weapon", t);
        }
    }
    void frames_tick(UObject* pawn)
    {
        using namespace hsvr;
        const Config& cfg = hsvr::config();
        if (!cfg.log_frames || !pawn || !g_pose.driver)
        {
            if (g_frames && !cfg.log_frames)
            {
                fclose(g_frames);
                g_frames = nullptr;
            }
            return;
        }
        double now = now_seconds();
        if (!g_frames)
        {
            g_frames = deep_open(L"frames.csv");
            if (!g_frames) return;
            g_state = deep_open(L"state.txt");
            g_events = deep_open(L"events.txt");
            g_creatures = deep_open(L"creatures.txt");
            static bool hooked = false;
            if (!hooked)
            {
                hooked = true;
                Hook::RegisterProcessEventPostCallback(&deep_event_cb, {false, false, STR("HalfSwordVR"), STR("DeepEvents")});
            }
            g_frame_bones = physics_body_bones(g_pose.mesh ? g_pose.mesh : g_pose.driver);
            std::string h = "t,dt,head_x,head_y,head_z,head_qx,head_qy,head_qz,head_qw";
            for (const char* s : {"l", "r"})
            {
                std::string p = std::string(s) + "_";
                for (const char* c : {"ctl_x", "ctl_y", "ctl_z", "ctl_qx", "ctl_qy", "ctl_qz", "ctl_qw", "trigger", "squeeze", "stick_x", "stick_y", "buttons",
                                      "target_x", "target_y", "target_z", "hand_x", "hand_y", "hand_z", "hand_qx", "hand_qy", "hand_qz", "hand_qw", "item",
                                      "grip_x", "grip_y", "grip_z", "item_qx", "item_qy", "item_qz", "item_qw", "item_vx", "item_vy", "item_vz", "item_wx",
                                      "item_wy", "item_wz", "lag_cm", "turn_deg", "force_n", "torque_nm", "capped", "strength", "on_item", "sliding", "drawing",
                                      "scabbard"})
                    h += "," + p + c;
            }
            h += ",actor_x,actor_y,actor_z,pelvis_x,pelvis_y,pelvis_z,headbone_x,headbone_y,headbone_z,foot_l_x,foot_l_y,foot_l_z,foot_r_x,foot_r_y,foot_r_z,"
                 "crouch_cm,button_crouch_cm,two_hands,stamina,aim_swing_speed,kicking_l,kicking_r,fallen,conc_r,conc_l,"
                 "drv_comp_x,drv_comp_y,drv_comp_z,drv_comp_qx,drv_comp_qy,drv_comp_qz,drv_comp_qw,vis_comp_x,vis_comp_y,vis_comp_z,vis_comp_qx,vis_comp_qy,"
                 "vis_comp_qz,vis_comp_qw,drv_lowerarm_l_x,drv_lowerarm_l_y,drv_lowerarm_l_z,drv_lowerarm_r_x,drv_lowerarm_r_y,drv_lowerarm_r_z,"
                 "vis_hand_l_x,vis_hand_l_y,vis_hand_l_z,vis_hand_r_x,vis_hand_r_y,vis_hand_r_z,"
                 "solve_l_on,solve_l_S_x,solve_l_S_y,solve_l_S_z,solve_l_E_x,solve_l_E_y,solve_l_E_z,solve_l_H_x,solve_l_H_y,solve_l_H_z,"
                 "drv_upperarm_l_x,drv_upperarm_l_y,drv_upperarm_l_z,"
                 "solve_r_on,solve_r_S_x,solve_r_S_y,solve_r_S_z,solve_r_E_x,solve_r_E_y,solve_r_E_z,solve_r_H_x,solve_r_H_y,solve_r_H_z,"
                 "drv_upperarm_r_x,drv_upperarm_r_y,drv_upperarm_r_z";
            for (FName bn : g_frame_bones)
            {
                std::string n = narrow(bn.ToString());
                h += "," + n + "_x," + n + "_y," + n + "_z";
            }
            h += "\n";
            fputs(h.c_str(), g_frames);
        }
        static double last_t = -1.0;
        double dt = last_t > 0.0 ? now - last_t : 0.0;
        last_t = now;
        ControllerState st = controllers_state();
        std::string row;
        row.reserve(2048);
        char b[160];
        auto num = [&](double v) {
            snprintf(b, sizeof b, ",%.3f", v);
            row += b;
        };
        auto v3 = [&](const Vec3& v) { num(v.x), num(v.y), num(v.z); };
        auto q4 = [&](const Quat& q) {
            snprintf(b, sizeof b, ",%.4f,%.4f,%.4f,%.4f", q.x, q.y, q.z, q.w);
            row += b;
        };
        snprintf(b, sizeof b, "%.4f,%.4f", now, dt);
        row += b;
        Vec3 hp;
        Quat hq;
        if (st.head_valid && stereo_tracking_to_world(st.head, hp, hq)) v3(hp), q4(hq);
        else row += ",,,,,,,";
        for (int side = 0; side < 2; ++side)
        {
            const HandState& hs = st.hand[side];
            Vec3 cp;
            Quat cq;
            if (hs.grip_valid && stereo_tracking_to_world(hs.grip, cp, cq)) v3(cp), q4(cq);
            else row += ",,,,,,,";
            num(hs.trigger), num(hs.squeeze), num(hs.stick_x), num(hs.stick_y);
            snprintf(b, sizeof b, ",%d", (hs.primary ? 1 : 0) | (hs.secondary ? 2 : 0) | (hs.stick_click ? 4 : 0) | (hs.menu ? 8 : 0));
            row += b;
            v3(g_pose.target[side]);
            FName bn = bone_fname(side == 0 ? "hand_l" : "hand_r");
            v3(socket_location(g_pose.driver, bn));
            q4(socket_rotation(g_pose.driver, bn));
            UObject* w = object_property(pawn, side == 0 ? STR("Weapon L") : STR("Weapon R"));
            UObject* base = w ? object_property(w, STR("BaseMesh")) : nullptr;
            const HoldState& h = g_hold[side];
            if (base)
            {
                std::string nm = narrow(w->GetName());
                for (char& c : nm)
                    if (c == ',') c = ';';
                row += "," + nm;
                Vec3 p = component_location(base);
                Quat q = component_rotation(base);
                v3(p + q.rotate(h.weapon == w ? h.pivot : Vec3{0, 0, 0}));
                q4(q);
                Call lv(base, STR("GetPhysicsLinearVelocity"));
                lv.run();
                Call av(base, STR("GetPhysicsAngularVelocityInRadians"));
                av.run();
                v3(lv.vec() * 0.01);
                v3(av.vec());
                num(h.lag), num(h.turn_deg), num(h.force_n), num(h.torque_nm);
                snprintf(b, sizeof b, ",%d", (h.force_capped ? 1 : 0) | (h.torque_capped ? 2 : 0));
                row += b;
                num(h.body_strength), num(h.on_weapon);
                snprintf(b, sizeof b, ",%d,%d", h.handle_slide ? 1 : 0, g_draw[side].active ? (g_draw[side].sheathing ? 2 : 1) : 0);
                row += b;
            }
            else
                row += ",,,,,,,,,,,,,,,,,,,,,,,";
            snprintf(b, sizeof b, ",%d", g_scab[side].active ? 1 : 0);
            row += b;
        }
        UObject* mesh = g_pose.mesh ? g_pose.mesh : g_pose.driver;
        v3(actor_location(pawn));
        v3(socket_location(mesh, bone_fname("pelvis")));
        v3(socket_location(mesh, bone_fname("head")));
        v3(socket_location(mesh, bone_fname("foot_l")));
        v3(socket_location(mesh, bone_fname("foot_r")));
        num(g_crouch_drop), num(g_button_crouch);
        snprintf(b, sizeof b, ",%d", g_two.active ? 1 : 0);
        row += b;
        num(read_number_variable(pawn, STR("Stamina"))), num(read_number_variable(pawn, STR("Aim Swing Speed")));
        snprintf(b, sizeof b, ",%d,%d,%d", (int)bool_property(pawn, STR("Kicking L")), (int)bool_property(pawn, STR("Kicking R")), (int)bool_property(pawn, STR("Fallen")));
        row += b;
        num(read_number_variable(pawn, STR("R Sheath Slot Concrete"))), num(read_number_variable(pawn, STR("L Sheath Slot Concrete")));
        // Which skeleton moves: the driver (ABP_Willie, where the arm pose is written) and the visible
        // body that copies it, each with its component's own place (hand_* above = the driver's).
        v3(component_location(g_pose.driver)), q4(component_rotation(g_pose.driver));
        if (g_pose.mesh) v3(component_location(g_pose.mesh)), q4(component_rotation(g_pose.mesh));
        else row += ",,,,,,,";
        v3(socket_location(g_pose.driver, bone_fname("lowerarm_l"))), v3(socket_location(g_pose.driver, bone_fname("lowerarm_r")));
        v3(socket_location(mesh, bone_fname("hand_l"))), v3(socket_location(mesh, bone_fname("hand_r")));
        // The arm solve's own shoulder, elbow and hand this frame, and the driver's shoulder as evaluated
        // (the solve reads last frame's shoulder: if the animation moves it, the hand lands off).
        for (int side = 0; side < 2; ++side)
        {
            snprintf(b, sizeof b, ",%d", g_solved_on[side]);
            row += b;
            v3(g_solved_S[side]), v3(g_solved_E[side]), v3(g_solved_H[side]);
            v3(socket_location(g_pose.driver, bone_fname(side == 0 ? "upperarm_l" : "upperarm_r")));
        }
        for (FName bn : g_frame_bones) v3(socket_location(mesh, bn));
        row += "\n";
        fputs(row.c_str(), g_frames);
        // His every variable, his movement and his held weapons, as they change; then the fighters near him.
        state_delta(pawn, "player", now);
        state_delta(object_property(pawn, STR("CharacterMovement")), "player movement", now);
        for (const TCHAR* slot : {STR("Weapon L"), STR("Weapon R")})
            if (UObject* w = object_property(pawn, slot)) state_delta(w, "player weapon", now);
        creatures_tick(pawn, now);
        if (now - g_frames_flush > 1.0)
        {
            g_frames_flush = now;
            for (FILE* f : {g_frames, g_state, g_events, g_creatures})
                if (f) fflush(f);
        }
    }

    // Pointers kept to the old pawn's weapons, scabbards and thrown items are dropped when the pawn
    // changes (a map change frees them; the old pawn's holds end with it).
    void forget_level_objects()
    {
        static int times = 0;
        HSVR_LOG("Pose: the player's pawn changed: forgetting the old one's holds, draws, scabbards, stabs and throws (%d)", ++times);
        for (int side = 0; side < 2; ++side)
        {
            g_stab_watch[side] = StabWatch{};
            g_hold[side] = HoldState{};
            g_draw[side] = DrawState{};
            g_scab[side] = ScabHold{};
            g_grip_log[side] = GripLog{};
            g_pending_item[side] = nullptr;
            g_pending_free[side] = FreeGrip{};
            g_arm_ignore[side].joints.clear();
            g_arm_ignore[side].weapon = nullptr;
        }
        g_two = TwoHand{};
        g_anim_grip_two_side = -1;
        g_thrown.clear();
        g_item_track.clear();
        g_sheath_fit.clear();
    }

    void capsule_after_tick(Hook::TCallbackIterationData<void>&, UObject* context, UFunction* function, void*)
    {
        gc_sync();
        HSVR_PROF(PerfCapsuleAfter);
        if (!is_receive_tick(context, function)) return;
        for (auto& pin : g_pawn_pins) set_variable(context, pin.first.c_str(), pin.second, nullptr, nullptr);
        if (g_pose_active.load())
        {
            keep_player_off_knees(context, true);
            crouch_after_tick(context);
            weapon_spring_after_tick(context);
            swing_speed_after_tick(context);
            fist_flags_after_tick(context);
            body_grab_after_tick(context);
            body_hit_watch(context);
            elbow_tick(context);
            thrown_tick(context);
            frames_tick(context);
        }
        stab_no_rubber_band(context);
        cut_through_tick(context);
        stab_log_tick(context);
        install_hit_haptics();
        grip_flip_tick(context);
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
        // Take the game's step term back out of the height (pose_capsule_no_step_offset): it bobbed the
        // capsule ~3 cm down through every step and snapped it back up as each foot landed.
        static double step_off_max = 0.0;
        if (hsvr::config().pose_capsule_no_step_offset && g_step_h2_have && rise <= 30.0)
        {
            auto flag_on = [&](const TCHAR* name) {
                FProperty* p = context->GetPropertyByNameInChain(name);
                return p && number_property(context, p) > 0.5;
            };
            if (!flag_on(STR("Climbing In Air")) && !flag_on(STR("Jump Pressed")) && !flag_on(STR("Fallen")))
            {
                double off = (std::max)(-10.0, (std::min)(10.0, g_step_h2_pre));
                keep.z -= off;
                step_off_max = (std::max)(step_off_max, std::fabs(off));
            }
        }
        // Never sunk into the floor: when its foot traces miss (a foot posed under the floor) the
        // game drops the capsule ~77 cm (Willie @594860, crouch_ik.md). The capsule's centre is kept
        // at least its standing height above the floor under it, less 15 cm (the game's own step
        // offset is within +-10).
        if (hsvr::config().pose_capsule_floor_clamp && g_pose.mesh)
        {
            double floor_z = 0.0;
            // Only when the game dropped it this tick (its miss drop is ~77 cm): the trace can land on a
            // loose shield or weapon, which once lifted him 57 cm (playtest 2, 147.8 s).
            if (rise < -20.0 && ground_below(context, keep.x, keep.y, keep.z + 40.0, floor_z))
            {
                double lowest = floor_z + 90.0 * component_scale_z(g_pose.mesh) - 15.0;
                if (keep.z < lowest)
                {
                    HSVR_LOG_EVERY(0.5, "Capsule: the game put it %.1f cm into the floor (centre %.1f, floor %.1f, crouch %.0f cm): kept at %.1f", lowest - keep.z, keep.z,
                                   floor_z, g_crouch_drop, lowest);
                    keep.z = lowest;
                }
            }
        }
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
            HSVR_LOG("Capsule: the game moved it sideways up to %.1f cm in a tick (put back), height %.1f, step offset taken out up to %.1f, climbs undone %d%s",
                     moved_max, keep.z, step_off_max, climbs, values.c_str());
            moved_max = 0.0;
            step_off_max = 0.0;
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
        static bool hidden_tracked = on_freed([] { for (UObject*& o : hidden) if (gone(o)) o = nullptr; });
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
            static bool gore_tracked = track_freed(&gore);
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
    // before_out (optional): the value it had, as text "<number>" the step can take back.
    void script_set_game_instance(const std::string& step, std::string* before_out = nullptr)
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
            if (before_out) *before_out = before ? "1" : "0";
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
            if (before_out) *before_out = std::to_string((long long)before);
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

    // ---- Passive foes (hsvr.passive_foe and the test room) ---------------------------------------
    // A training dummy (passive_foe.md in the decompile folder): the foe's AI controller (AI_BP_C) is
    // stopped - DED set, its tick off, its movement stopped - and the inputs it writes on its pawn
    // cleared. The foe keeps its own physics, balance and get-up (those run in Willie_BP's tick), so
    // it can be hit, grabbed and stuck into. It can still die. Only a zombie revive turns AI_BP on again.
    bool make_foe_passive(UObject* ai, UObject* foe)
    {
        if (!ai || !foe) return false;
        set_variable(ai, STR("DED"), 1.0);
        Call tick(ai, STR("SetActorTickEnabled"));
        tick.first_flag(false);
        tick.run();
        Call stop(ai, STR("StopMovement"));
        stop.run();
        std::wstring missing;
        for (const TCHAR* v : {STR("R Hand Active"), STR("L Hand Active"), STR("R Down"), STR("L Down"), STR("Chain Attack"), STR("BobnWeave"), STR("Dodging"),
                               STR("Alt Thrusting"), STR("AI In Combat"), STR("Is Crouched??")})
            if (!set_variable(foe, v, 0.0)) missing += std::wstring(v) + STR(", ");
        if (FProperty* p = foe->GetPropertyByNameInChain(STR("Aim Vector_0")))
            write_three(reinterpret_cast<uint8_t*>(foe) + p->GetOffset_Internal(), p, 0.0, 0.0, 0.0);
        HSVR_LOG("Passive: %ls (controller %ls) made passive%ls%ls", foe->GetName().c_str(), ai->GetName().c_str(), missing.empty() ? STR("") : STR("; not found: "),
                 missing.c_str());
        return true;
    }

    // ---- Test room (ini test_room=1) -------------------------------------------------------------
    // Launching the game goes from the main menu straight into a Free Mode arena (test_room_map) with
    // test_room_foes foes whose AI is stopped (make_foe_passive) and stood in an arc behind him, and
    // two of the game's weapon tables in front of him stocked with one weapon of every kind through
    // the game's own loot point (BP_Loot_Weapon: Generate Weapon from Weapon Type and Tier). Facts
    // from the decompile: defaults\test_room.md.
    //
    // His save: the Free Mode settings are written to the game instance only, never saved by the mod.
    // The arena's BP_GameManager calls GI_Settings "Load Game" on start, which would put the save's
    // play/game/combat mode back, so a hook puts the test values back right after it. The game still
    // saves during a fight (start, ~1 s, death, pause menu); every such write is undone at once by
    // copying back the copy of GameProgress.sav made at boot (save_backups\ in the mod folder, one
    // per launch, never deleted). Leaving the room map restores the save, loads it into the game
    // instance and puts back every game-instance value the room changed. No room without that copy.
    struct TestRoom
    {
        bool booted = false;    // the boot step ran and made the save copy
        bool failed = false;    // no save copy: no room
        bool armed = false;     // Load Game hook re-applies the test values
        bool done = false;      // left the room; nothing more this launch
        std::wstring map_path;  // /Game/Maps/Arenas/Map_Arena_Yard
        std::wstring map_key;   // "/Map_Arena_Yard." as it appears in an object's full name
        std::wstring save_path, backup_path;
        FILETIME save_time{};   // the save file's write time after the mod's last copy back
        bool save_time_known = false;
        std::vector<std::string> values;                              // "Name=value" set at boot
        std::vector<std::pair<std::string, std::string>> before;      // name, value before
        bool entered = false;
        UObject* pawn = nullptr; // the pawn the room was furnished for
        double pawn_at = -1.0;
        int stage = 0;           // per pawn: 0 waiting, 1 foes and tables placed, 2 weapons spawned
        hsvr::Vec3 origin, forward;
        std::vector<hsvr::Vec3> table_at;
        double table_yaw = 0.0;
        double next_sweep = 0.0, next_save_check = 0.0;
        std::vector<UObject*> settling; // foe meshes moved with physics off (room_settle_foes)
        double settle_at = 0.0;
        bool settled = false;
        // Respawn (test_room_respawn): one slot per foe of the arc, with who stands there now.
        struct Slot
        {
            hsvr::Vec3 at;
            double yaw;
            UObject* foe;
            double dead_since; // < 0 = alive
            std::vector<UObject*> gear; // what he has had in hand or sheathed (room_note_gear)
        };
        std::vector<Slot> slots;
        // Bodies of foes who are out, removed test_room_despawn_seconds later (not while in use).
        struct Body
        {
            UObject* body;
            double out_at;
            bool waiting = false; // in use (room_body_in_use): removed later
            std::vector<UObject*> gear; // his weapons and shield, removed with him
        };
        std::vector<Body> bodies;
        double next_respawn_check = 0.0;
        double watch_from = 0.0, next_watch = 0.0;     // diagnostics after the weapons are put down
        std::map<UObject*, hsvr::Vec3> watch_last;
    };
    TestRoom g_room;

    std::wstring local_app_data()
    {
        wchar_t buf[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
        return (n > 0 && n < MAX_PATH) ? std::wstring(buf, n) : std::wstring();
    }

    bool file_time(const std::wstring& path, FILETIME& out)
    {
        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) return false;
        out = info.ftLastWriteTime;
        return true;
    }

    // Puts the boot copy back over GameProgress.sav (only ever the copy made this launch).
    void room_restore_save(const char* why)
    {
        if (g_room.backup_path.empty()) return;
        if (!CopyFileW(g_room.backup_path.c_str(), g_room.save_path.c_str(), FALSE))
        {
            HSVR_LOG("TestRoom: could NOT put the save back (%s): error %lu", why, GetLastError());
            return;
        }
        g_room.save_time_known = file_time(g_room.save_path, g_room.save_time);
        HSVR_LOG("TestRoom: save put back from %ls (%s)", g_room.backup_path.c_str(), why);
    }

    UObject* game_instance_object()
    {
        UObject* viewport = object_property(g_engine, STR("GameViewport"));
        return object_property(viewport, STR("GameInstance"));
    }

    void room_apply_values()
    {
        for (const std::string& v : g_room.values) script_set_game_instance(v);
    }

    void room_load_game_post(Unreal::UnrealScriptFunctionCallableContext&, void*)
    {
        gc_sync();
        HSVR_PROF(PerfRoomHooks);
        if (!g_room.armed) return;
        HSVR_LOG("TestRoom: the game loaded the save; test values put back");
        room_apply_values();
    }

    void room_install_hook()
    {
        static bool installed = false;
        if (installed) return;
        UObject* gi = game_instance_object();
        UFunction* fn = gi ? gi->GetFunctionByNameInChain(STR("Load Game")) : nullptr;
        if (!fn)
        {
            HSVR_LOG("TestRoom: the game instance has no \"Load Game\"");
            return;
        }
        installed = true;
        UObjectGlobals::RegisterHook(fn, [](Unreal::UnrealScriptFunctionCallableContext&, void*) {}, &room_load_game_post, nullptr);
        HSVR_LOG("TestRoom: watching %ls", fn->GetFullName().c_str());
    }

    // Boot step (M+2 after the first menu): copy the save, set Free Mode in the game instance.
    void room_boot(const hsvr::Config& cfg)
    {
        g_room.booted = true;
        std::wstring lad = local_app_data();
        g_room.save_path = lad + L"\\HalfswordUE5\\Saved\\SaveGames\\GameProgress.sav";
        std::wstring dir = hsvr::mod_dir() + L"save_backups";
        CreateDirectoryW(dir.c_str(), nullptr);
        SYSTEMTIME utc{};
        GetSystemTime(&utc);
        wchar_t name[96];
        swprintf(name, 96, L"\\GameProgress.sav.%04d%02d%02d-%02d%02d%02dZ.bak", (int)utc.wYear, (int)utc.wMonth, (int)utc.wDay, (int)utc.wHour, (int)utc.wMinute,
                 (int)utc.wSecond);
        g_room.backup_path = dir + name;
        if (lad.empty() || !CopyFileW(g_room.save_path.c_str(), g_room.backup_path.c_str(), TRUE))
        {
            HSVR_LOG("TestRoom: could not copy %ls to %ls (error %lu); no test room this launch", g_room.save_path.c_str(), g_room.backup_path.c_str(), GetLastError());
            g_room.backup_path.clear();
            g_room.failed = true;
            return;
        }
        g_room.save_time_known = file_time(g_room.save_path, g_room.save_time);
        HSVR_LOG("TestRoom: save copied to %ls", g_room.backup_path.c_str());

        g_room.map_path.assign(cfg.test_room_map.begin(), cfg.test_room_map.end());
        size_t slash = g_room.map_path.find_last_of(L'/');
        g_room.map_key = L"/" + (slash == std::wstring::npos ? g_room.map_path : g_room.map_path.substr(slash + 1)) + L".";
        int foes = (std::max)(1, (std::min)(10, cfg.test_room_foes));
        int tier = cfg.test_room_tier;
        g_room.values = {"Current Play Mode=1",        "Free Mode Activated=1",         "Current Game Mode Enum=1",       "Current Combat Mode=1",
                         "Current Lose Condition=0",   "Free Mode Foes Amount=" + std::to_string(foes), "FreeMode Foes Amount Random=0",
                         "Free Mode Tier=" + std::to_string(tier < 0 ? 0 : (std::min)(tier, 8)), std::string("FreeMode Tier Random=") + (tier < 0 ? "1" : "0"),
                         "Free Mode Carnage=0",        "Free Mode Brawling=0",          "Free Mode Blossfechten=0",       "FreeMode Multiplayer=0"};
        for (const std::string& v : g_room.values)
        {
            std::string before;
            script_set_game_instance(v, &before);
            if (!before.empty()) g_room.before.emplace_back(v.substr(0, v.find('=')), before);
        }
        room_install_hook();
        g_room.armed = true;
    }

    // Leaving the room: the save back, loaded into the game instance, and the values before the room.
    void room_leave()
    {
        g_room.armed = false;
        g_room.done = true;
        room_restore_save("left the room");
        script_call_game_instance("Load Game");
        for (const auto& b : g_room.before) script_set_game_instance(b.first + "=" + b.second);
        HSVR_LOG("TestRoom: left the room; save and game settings as before it");
    }

    // Spawns an actor of a class (loaded if needed) with a transform; `before` runs before its
    // BeginPlay (FinishSpawning), for ExposeOnSpawn variables.
    template <typename F> UObject* room_spawn(const TCHAR* class_path, UObject* world_object, const hsvr::Vec3& at, const hsvr::Quat& q, F before, double scale = 1.0)
    {
        UClass* cls = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, class_path);
        if (!cls)
        {
            std::wstring asset = class_path;
            if (asset.size() > 2 && asset.substr(asset.size() - 2) == STR("_C")) asset.resize(asset.size() - 2);
            load_asset_object(asset);
            cls = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, class_path);
        }
        static UObject* gs = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__GameplayStatics"));
        if (!cls || !gs)
        {
            HSVR_LOG("TestRoom: class %ls not found", class_path);
            return nullptr;
        }
        auto fill = [&](Call& c) {
            FProperty* p = c.param(STR("SpawnTransform"));
            UScriptStruct* type = p ? struct_type(p) : nullptr;
            if (!type) return;
            uint8_t* tat = c.buffer.data() + p->GetOffset_Internal();
            if (FProperty* r = struct_member(type, STR("Rotation")))
            {
                const double parts[4] = {q.x, q.y, q.z, q.w};
                const TCHAR* names[4] = {STR("X"), STR("Y"), STR("Z"), STR("W")};
                for (int i = 0; i < 4; ++i)
                    if (FProperty* m = struct_member(struct_type(r), names[i]))
                    {
                        uint8_t* mat = tat + r->GetOffset_Internal() + m->GetOffset_Internal();
                        if (m->GetClass().GetName() == STR("DoubleProperty")) *reinterpret_cast<double*>(mat) = parts[i];
                        else *reinterpret_cast<float*>(mat) = (float)parts[i];
                    }
            }
            if (FProperty* t = struct_member(type, STR("Translation"))) write_three(tat + t->GetOffset_Internal(), t, at.x, at.y, at.z);
            if (FProperty* sc = struct_member(type, STR("Scale3D"))) write_three(tat + sc->GetOffset_Internal(), sc, scale, scale, scale);
        };
        Call begin(gs, STR("BeginDeferredActorSpawnFromClass"));
        begin.object(STR("WorldContextObject"), world_object);
        begin.object(STR("ActorClass"), cls);
        fill(begin);
        begin.byte(STR("CollisionHandlingOverride"), 1); // always spawn
        begin.run();
        UObject* actor = begin.object_result();
        if (!actor) return nullptr;
        before(actor);
        Call finish(gs, STR("FinishSpawningActor"));
        finish.object(STR("Actor"), actor);
        fill(finish);
        finish.run();
        return actor;
    }

    hsvr::Quat yaw_quat(double yaw_deg)
    {
        double h = yaw_deg * 3.14159265358979 / 360.0;
        hsvr::Quat q;
        q.z = std::sin(h);
        q.w = std::cos(h);
        return q;
    }

    // Foes: AI stopped, then stood in an arc behind him (2.2 m away), facing him.
    void room_place_foes(UObject* player)
    {
        using hsvr::Vec3;
        std::vector<UObject*> ais;
        UObjectGlobals::FindAllOf(STR("AI_BP_C"), ais);
        std::vector<UObject*> foes;
        for (UObject* ai : ais)
        {
            if (!ai || ai->GetName().rfind(STR("Default__"), 0) == 0) continue;
            Call gp(ai, STR("K2_GetPawn"));
            gp.run();
            UObject* foe = gp.object_result();
            if (!foe || foe == player) continue;
            make_foe_passive(ai, foe);
            foes.push_back(foe);
        }
        static const FName pelvis_bone(STR("pelvis"), FNAME_Add);
        g_room.settling.clear();
        g_room.settled = false;
        g_room.slots.clear();
        int n = (int)foes.size();
        for (int i = 0; i < n; ++i)
        {
            double a = (180.0 + (i - (n - 1) * 0.5) * 32.0) * 3.14159265358979 / 180.0;
            Vec3 right{-g_room.forward.y, g_room.forward.x, 0.0};
            Vec3 dir = g_room.forward * std::cos(a) + right * std::sin(a);
            Vec3 at = g_room.origin + dir * 220.0;
            double yaw = std::atan2(-dir.y, -dir.x) * 57.29577951308232;
            UObject* foe = foes[i];
            UObject* fm = object_property(foe, STR("Mesh"));
            // A teleport of a simulating body throws him ~5 m (test 2026-11-19 16:31: the pose drives
            // see a 10 m jump in one frame). His body stops simulating while he is moved, and comes
            // back to physics 0.4 s later at rest (room_settle_foes).
            if (fm)
            {
                Call sim(fm, STR("SetAllBodiesSimulatePhysics"));
                sim.first_flag(false);
                sim.run();
            }
            Call mv(foe, STR("K2_SetActorLocationAndRotation"));
            mv.three(STR("NewLocation"), at.x, at.y, at.z);
            mv.three(STR("NewRotation"), 0.0, yaw, 0.0);
            mv.flag(STR("bTeleport"), true);
            mv.run();
            // His body moves with him (a teleport of the actor alone leaves the body where it lay).
            if (fm)
            {
                Vec3 before = socket_location(fm, pelvis_bone);
                Call off(fm, STR("K2_AddWorldOffset"));
                off.three(STR("DeltaLocation"), at.x - before.x, at.y - before.y, 0.0);
                off.flag(STR("bTeleport"), true);
                off.run();
                g_room.settling.push_back(fm);
            }
            g_room.slots.push_back(TestRoom::Slot{at, yaw, foe, -1.0, {}});
            HSVR_LOG("TestRoom: %ls stood at (%.0f, %.0f, %.0f), facing him", foe->GetName().c_str(), at.x, at.y, at.z);
        }
        g_room.settle_at = hsvr::now_seconds();
        HSVR_LOG("TestRoom: %d passive foe(s)", n);
    }

    // Foes placed by room_place_foes: physics back on 0.4 s after the move, then held at rest
    // (no velocity) for 1 s, so that they stand where they were put.
    void room_settle_foes()
    {
        if (g_room.settling.empty()) return;
        double since = hsvr::now_seconds() - g_room.settle_at;
        if (since < 0.4) return;
        static const FName pelvis_bone(STR("pelvis"), FNAME_Add);
        for (UObject* fm : g_room.settling)
        {
            if (!g_room.settled)
            {
                Call sim(fm, STR("SetAllBodiesSimulatePhysics"));
                sim.first_flag(true);
                sim.run();
            }
            Call lin(fm, STR("SetAllPhysicsLinearVelocity"));
            lin.three(STR("NewVel"), 0.0, 0.0, 0.0);
            lin.run();
            Call ang(fm, STR("SetAllPhysicsAngularVelocityInDegrees"));
            ang.three(STR("NewAngVel"), 0.0, 0.0, 0.0);
            ang.run();
            if (since >= 1.4)
            {
                hsvr::Vec3 p = socket_location(fm, pelvis_bone);
                HSVR_LOG("TestRoom: %ls settled, pelvis at (%.0f, %.0f, %.0f)", fm->GetFullName().c_str(), p.x, p.y, p.z);
            }
        }
        if (!g_room.settled) HSVR_LOG("TestRoom: %d foe(s) back to physics", (int)g_room.settling.size());
        g_room.settled = true;
        if (since >= 1.4) g_room.settling.clear();
    }

    // ---- Respawn (test_room_respawn) ----------------------------------------------------------------
    // A dead foe is replaced test_room_respawn_seconds later: his body is removed the way the arena
    // removes Willies (Willie_BP "Destroy Willie", Clean Up Map), then a new one is made the way the
    // game makes a Free Mode foe (BP_SpawnerPoint_Willies UG @5101-@9167, defaults\test_room.md §7):
    // BP_Generator_Characters_Random (Equipment Budget 0) "Generate Character" -> his passport; a
    // deferred Willie_BP with Team Int, Character Passport, Spawn in Pants and Blossfechten Gear set
    // before FinishSpawning, scaled by his Height; AI_BP possesses him itself (AutoPossessAI); the game
    // mode's Enemy Count +1. He is made passive at once and stands in the dead one's slot.

    UObject* room_game_mode()
    {
        std::vector<UObject*> gms;
        UObjectGlobals::FindAllOf(STR("BP_HalfSwordGameMode_C"), gms);
        for (UObject* g : gms)
            if (g && g->GetName().rfind(STR("Default__"), 0) != 0) return g;
        return nullptr;
    }

    // The game mode's Enemy Count (a foe's death or giving up takes 1 off; at 0 the match is won:
    // Minus Enemy). With respawn it is held at 100 or more, so the match is never won: a foe beaten and
    // then killed counts twice (Call Minus Enemy and Minus Enemy are separate DoOnces), and killing them
    // all faster than they come back took one spare to 0 (test 2026-11-20 01:28).
    // Diagnostics: every call of the game mode's enemy counting, with the count before and after.
    int32_t g_room_count_before = 0;
    void room_count_pre(Unreal::UnrealScriptFunctionCallableContext& ctx, void*)
    {
        gc_sync();
        HSVR_PROF(PerfRoomHooks);
        int32_t* c = ctx.Context ? ctx.Context->GetValuePtrByPropertyNameInChain<int32_t>(STR("Enemy Count")) : nullptr;
        g_room_count_before = c ? *c : -1;
    }
    void room_count_post(Unreal::UnrealScriptFunctionCallableContext& ctx, void*)
    {
        gc_sync();
        HSVR_PROF(PerfRoomHooks);
        int32_t* c = ctx.Context ? ctx.Context->GetValuePtrByPropertyNameInChain<int32_t>(STR("Enemy Count")) : nullptr;
        HSVR_LOG("TestRoom: game mode %ls: Enemy Count %d -> %d, Match Won %d", ctx.TheStack.Node() ? ctx.TheStack.Node()->GetName().c_str() : STR("?"), g_room_count_before,
                 c ? *c : -1, (int)bool_property(ctx.Context, STR("Match Won")));
    }
    void room_watch_counting(UObject* gm)
    {
        static bool done = false;
        if (done || !gm) return;
        done = true;
        for (const TCHAR* f : {STR("Minus Enemy"), STR("Plus Enemy"), STR("Multiplayer Match Win Check")})
            if (UFunction* fn = gm->GetFunctionByNameInChain(f)) UObjectGlobals::RegisterHook(fn, &room_count_pre, &room_count_post, nullptr);
    }

    // The match is still called won now and then when every foe is out at about the same moment
    // (2026-11-20 08:37: a new foe was already up; not Minus Enemy, whose calls are all logged).
    // UI_WinScreen's Construct then sets Match Won, Enemy Count 0, Rounds Won + 1 and half-speed time
    // for 0.5 s. While foes respawn, the screen is taken down at once and all of that undone (its
    // music still plays: PlaySound2D cannot be called back).
    void room_winscreen_post(Unreal::UnrealScriptFunctionCallableContext& context, void*)
    {
        gc_sync();
        HSVR_PROF(PerfRoomHooks);
        if (!g_room.entered || g_room.done || !hsvr::config().test_room_respawn) return;
        UObject* w = context.Context;
        if (!w) return;
        Call rm(w, STR("RemoveFromParent"));
        rm.run();
        UObject* gi = game_instance_object();
        if (gi)
        {
            set_variable(gi, STR("Time Dilation"), 1.0);
            double rounds = read_number_variable(gi, STR("Rounds Won"));
            if (rounds > 0.0) set_variable(gi, STR("Rounds Won"), rounds - 1.0);
        }
        static UObject* gs = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__GameplayStatics"));
        if (gs)
        {
            Call td(gs, STR("SetGlobalTimeDilation"));
            td.object(STR("WorldContextObject"), w);
            td.number(STR("TimeDilation"), 1.0);
            td.run();
        }
        if (UObject* gm = room_game_mode())
        {
            set_variable(gm, STR("Match Won"), 0.0);
            set_variable(gm, STR("All Enemies Dead"), 0.0);
            if (int32_t* count = gm->GetValuePtrByPropertyNameInChain<int32_t>(STR("Enemy Count"))) *count = 100;
        }
        HSVR_LOG("TestRoom: respawn: the game put up its win screen; taken down (time, Rounds Won, Match Won and Enemy Count put back)");
    }

    void room_watch_winscreen()
    {
        static bool done = false;
        if (done) return;
        UFunction* fn = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Game/UI/UI_WinScreen.UI_WinScreen_C:Construct"));
        if (!fn)
        {
            load_asset_object(STR("/Game/UI/UI_WinScreen"));
            fn = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Game/UI/UI_WinScreen.UI_WinScreen_C:Construct"));
        }
        done = true;
        if (!fn)
        {
            HSVR_LOG("TestRoom: respawn: UI_WinScreen Construct not found");
            return;
        }
        UObjectGlobals::RegisterHook(fn, [](Unreal::UnrealScriptFunctionCallableContext&, void*) {}, &room_winscreen_post, nullptr);
        HSVR_LOG("TestRoom: respawn: watching %ls", fn->GetFullName().c_str());
    }

    void room_hold_enemy_count()
    {
        room_watch_counting(room_game_mode());
        room_watch_winscreen();
        UObject* gm = room_game_mode();
        int32_t* count = gm ? gm->GetValuePtrByPropertyNameInChain<int32_t>(STR("Enemy Count")) : nullptr;
        if (!count) return;
        if (*count < 100)
        {
            HSVR_LOG("TestRoom: respawn: Enemy Count %d -> 100 (the match is never won while foes respawn)", *count);
            *count = 100;
        }
        // Something else also wins the match about 1.7 s after every foe is dead at once (Enemy Count
        // was 100; UI_WinScreen then sets Match Won and Enemy Count 0, test 2026-11-20 01:33): never
        // let the room be empty (room_respawn_tick), and undo a win that gets through anyway.
        if (bool_property(gm, STR("Match Won")) || bool_property(gm, STR("All Enemies Dead")))
        {
            set_variable(gm, STR("Match Won"), 0.0);
            set_variable(gm, STR("All Enemies Dead"), 0.0);
            HSVR_LOG("TestRoom: respawn: the game called the match won; undone");
        }
    }

    UObject* room_spawn_foe(UObject* player, const TestRoom::Slot& slot)
    {
        using hsvr::Vec3;
        const hsvr::Config& cfg = hsvr::config();
        static unsigned seed = (unsigned)GetTickCount64() ^ 0x9e3779b9u;
        int tier = cfg.test_room_tier;
        if (tier < 0)
        {
            seed = seed * 1103515245u + 12345u;
            tier = (int)((seed >> 16) % 8u); // the Free Mode menu's range, Beggar .. Knight
        }
        tier = (std::min)(tier, 8);
        UObject* gen = room_spawn(STR("/Game/Blueprints/Generators/BP_Generator_Characters_Random.BP_Generator_Characters_Random_C"), player, slot.at, yaw_quat(0.0),
                                  [&](UObject* a) { set_variable(a, STR("Equipment Budget"), 0.0); });
        if (!gen)
        {
            HSVR_LOG("TestRoom: respawn: no character generator");
            return nullptr;
        }
        UClass* willie = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Game/Character/Blueprints/Willie_BP.Willie_BP_C"));
        Call gc(gen, STR("Generate Character"));
        gc.object(STR("Actor Class"), willie);
        gc.byte(STR("Nationality"), 0);
        gc.byte(STR("Faction"), 0);
        gc.byte(STR("Tier"), (uint8_t)tier);
        gc.run();
        FProperty* pp = gc.param(STR("Character Passport"));
        uint8_t* passport = pp ? gc.buffer.data() + pp->GetOffset_Internal() : nullptr;
        double height = 0.5;
        if (passport)
            if (FProperty* h = struct_member_prefix(struct_type(pp), STR("Height")))
            {
                uint8_t* at = passport + h->GetOffset_Internal();
                height = h->GetClass().GetName() == STR("DoubleProperty") ? *reinterpret_cast<double*>(at) : *reinterpret_cast<float*>(at);
            }
        // Where the game puts him: the spawn point 100 cm above the floor, + Height * 10 cm, his
        // size MapRange(Height 0..1 -> 0.875..1).
        double floor_z = slot.at.z;
        if (UObject* capsule = object_property(player, STR("CapsuleComponent")))
        {
            Call hh(capsule, STR("GetScaledCapsuleHalfHeight"));
            hh.run();
            if (FProperty* p = hh.param(STR("ReturnValue")))
            {
                uint8_t* at = hh.buffer.data() + p->GetOffset_Internal();
                floor_z = actor_location(player).z - (p->GetClass().GetName() == STR("DoubleProperty") ? *reinterpret_cast<double*>(at) : *reinterpret_cast<float*>(at));
            }
        }
        Vec3 at = slot.at;
        at.z = floor_z + 100.0 + height * 10.0;
        UObject* foe = room_spawn(STR("/Game/Character/Blueprints/Willie_BP.Willie_BP_C"), player, at, yaw_quat(slot.yaw), [&](UObject* a) {
            set_variable(a, STR("Team Int"), 2.0); // the spawner's value for a Free Mode foe (Duel)
            set_variable(a, STR("Spawn in Pants"), 0.0);
            set_variable(a, STR("Blossfechten Gear"), 0.0);
            if (passport)
                if (FProperty* dst = a->GetPropertyByNameInChain(STR("Character Passport")))
                    dst->CopyCompleteValue(reinterpret_cast<uint8_t*>(a) + dst->GetOffset_Internal(), passport);
        }, 0.875 + 0.125 * height);
        if (passport) pp->DestroyValue(passport);
        Call gone(gen, STR("K2_DestroyActor"));
        gone.run();
        if (!foe)
        {
            HSVR_LOG("TestRoom: respawn: the new foe did NOT spawn");
            return nullptr;
        }
        UObject* ai = object_property(foe, STR("Controller"));
        if (ai) make_foe_passive(ai, foe);
        HSVR_LOG("TestRoom: respawn: %ls (tier %d, height %.2f) at (%.0f, %.0f, %.0f), facing you, AI %ls", foe->GetName().c_str(), tier, height, at.x, at.y, at.z,
                 ai ? STR("off") : STR("not there yet (the sweep stops it)"));
        return foe;
    }

    // Respawn never removes a body you are using: one of your hands grabs him (or a piece of him),
    // you hold one of his weapons, or a weapon in your hand is stuck in a body within 150 cm of him
    // (the game does not say whose body a stuck weapon is in). The new foe waits for it.
    const char* room_body_in_use(UObject* corpse, UObject* player)
    {
        if (!corpse || !player) return nullptr;
        for (int side = 0; side < 2; ++side)
        {
            if (g_body_grab[side].victim == corpse) return "your hand grabs him";
            if (UObject* c = g_object_grab[side].component)
            {
                Call owner(c, STR("GetOwner"));
                owner.run();
                if (owner.object_result() == corpse) return "your hand holds a piece of him";
            }
        }
        UObject* his[2] = {object_property(corpse, STR("Weapon L")), object_property(corpse, STR("Weapon R"))};
        static const FName pelvis_bone(STR("pelvis"), FNAME_Add);
        UObject* cm = object_property(corpse, STR("Mesh"));
        hsvr::Vec3 body = cm ? socket_location(cm, pelvis_bone) : actor_location(corpse);
        for (const TCHAR* hand : {STR("Weapon L"), STR("Weapon R")})
        {
            UObject* w = object_property(player, hand);
            if (!w || !object_alive(w)) continue;
            if (w == his[0] || w == his[1]) return "you hold his weapon";
            if (!bool_property(w, STR("Stuck In Body"))) continue;
            UObject* base = object_property(w, STR("BaseMesh"));
            if (base && hsvr::length(component_location(base) - body) < 150.0) return "a weapon in your hand is stuck in him";
        }
        return nullptr;
    }

    bool room_alive(UObject* foe, const std::vector<UObject*>& willies)
    {
        // Out of the fight = dead (DED) or beaten ("Give Up": Health under 5 -> Dying, he gives up and
        // the game has already taken him off Enemy Count; test 2026-11-20 01:28: Give Up 1, Downed and
        // DED stay 0, Health regrows).
        return foe && std::find(willies.begin(), willies.end(), foe) != willies.end() && !bool_property(foe, STR("DED")) && !bool_property(foe, STR("Give Up"));
    }

    // A removed foe's weapons and shield stayed on the floor as physics bodies; with a foe out every
    // minute or so they piled up and each one in contact adds hit events and solver work every substep
    // (perf2 run 2026-11-20: 22 -> 31 weapons and 132 -> 172 simulating bodies in 10 min, 78 -> 52 fps).
    // So what a room foe has held or sheathed goes with his body, unless you or a standing foe hold it.
    void room_note_gear(UObject* foe, std::vector<UObject*>& gear)
    {
        if (!foe) return;
        const TCHAR* vars[] = {STR("Weapon R"), STR("Weapon L"), STR("Weapon Slot R 1"), STR("Weapon Slot R 2"), STR("Weapon Slot L 1"), STR("Weapon Slot L 2"), STR("Weapon Slot Back")};
        for (const TCHAR* v : vars)
        {
            UObject* item = object_property(foe, v);
            if (!item || is_fist(item) || hit_is_his_item(item)) continue;
            if (std::find(gear.begin(), gear.end(), item) == gear.end()) gear.push_back(item);
        }
    }
    int room_clear_gear(std::vector<UObject*>& gear, const std::vector<UObject*>& willies, UObject* removed)
    {
        int n = 0;
        for (UObject* item : gear)
        {
            if (!item || !object_alive(item) || hit_is_his_item(item)) continue;
            bool held = false;
            for (UObject* w : willies)
                if (w && w != removed && w != g_hitf.pawn && (object_property(w, STR("Weapon R")) == item || object_property(w, STR("Weapon L")) == item)) held = true;
            if (held) continue;
            Call d(item, STR("K2_DestroyActor"));
            d.run();
            ++n;
        }
        gear.clear();
        return n;
    }

    void room_respawn_tick(UObject* player)
    {
        const hsvr::Config& cfg = hsvr::config();
        if (!cfg.test_room_respawn || g_room.slots.empty()) return;
        double now = hsvr::now_seconds();
        room_hold_enemy_count(); // every tick
        if (now < g_room.next_respawn_check) return;
        g_room.next_respawn_check = now + 0.25;
        std::vector<UObject*> willies;
        UObjectGlobals::FindAllOf(STR("Willie_BP_C"), willies);
        // Never all out at once: when no foe is left standing, the one out longest comes back now.
        {
            int alive = 0;
            TestRoom::Slot* oldest = nullptr;
            for (TestRoom::Slot& s : g_room.slots)
            {
                if (room_alive(s.foe, willies)) ++alive;
                else if (s.dead_since >= 0.0 && (!oldest || s.dead_since < oldest->dead_since)) oldest = &s;
            }
            if (alive == 0 && oldest && oldest->dead_since > now - cfg.test_room_respawn_seconds)
            {
                oldest->dead_since = now - cfg.test_room_respawn_seconds;
                HSVR_LOG("TestRoom: respawn: every foe is out; one comes back now");
            }
        }
        // A foe out of the fight leaves his slot: his body goes on the body list (removed after
        // test_room_despawn_seconds), and the slot gets a new foe after test_room_respawn_seconds.
        for (TestRoom::Slot& s : g_room.slots)
        {
            if (room_alive(s.foe, willies))
            {
                s.dead_since = -1.0;
                room_note_gear(s.foe, s.gear);
                continue;
            }
            if (s.dead_since < 0.0)
            {
                s.dead_since = now;
                bool body = s.foe && std::find(willies.begin(), willies.end(), s.foe) != willies.end();
                HSVR_LOG("TestRoom: respawn: %ls is out; body removed in %.0f s, a new foe in %.0f s", body ? s.foe->GetName().c_str() : STR("a foe"),
                         cfg.test_room_despawn_seconds, cfg.test_room_respawn_seconds);
                if (body) g_room.bodies.push_back(TestRoom::Body{s.foe, now, false, std::move(s.gear)});
                s.gear.clear();
                s.foe = nullptr;
            }
        }
        // Bodies: removed the way the arena removes Willies, but never one you are using.
        for (size_t i = 0; i < g_room.bodies.size();)
        {
            TestRoom::Body& b = g_room.bodies[i];
            if (!b.body || std::find(willies.begin(), willies.end(), b.body) == willies.end())
            {
                g_room.bodies.erase(g_room.bodies.begin() + (std::ptrdiff_t)i);
                continue;
            }
            if (cfg.test_room_despawn_seconds <= 0.0 || now - b.out_at < cfg.test_room_despawn_seconds)
            {
                ++i;
                continue;
            }
            if (const char* why = room_body_in_use(b.body, player))
            {
                if (!b.waiting) HSVR_LOG("TestRoom: respawn: %ls stays for now: %s", b.body->GetName().c_str(), why);
                b.waiting = true;
                ++i;
                continue;
            }
            Call d(b.body, STR("Destroy Willie"));
            d.run();
            int cleared = room_clear_gear(b.gear, willies, b.body);
            HSVR_LOG("TestRoom: respawn: %ls removed (Destroy Willie), %.1f s after he was out; %d of his dropped weapons and shields removed too",
                     b.body->GetName().c_str(), now - b.out_at, cleared);
            g_room.bodies.erase(g_room.bodies.begin() + (std::ptrdiff_t)i);
        }
        // New foes. A body still lying within 80 cm of the slot (kept, or removal off) would throw a
        // foe spawned into it: then he stands 1 m further back.
        for (TestRoom::Slot& s : g_room.slots)
        {
            if (s.foe || s.dead_since < 0.0 || now - s.dead_since < cfg.test_room_respawn_seconds) continue;
            TestRoom::Slot at = s;
            static const FName pelvis_bone(STR("pelvis"), FNAME_Add);
            for (const TestRoom::Body& b : g_room.bodies)
            {
                UObject* bm = object_property(b.body, STR("Mesh"));
                hsvr::Vec3 p = bm ? socket_location(bm, pelvis_bone) : actor_location(b.body);
                hsvr::Vec3 d = p - s.at;
                d.z = 0.0;
                if (hsvr::length(d) < 80.0)
                {
                    hsvr::Vec3 out = s.at - g_room.origin;
                    out.z = 0.0;
                    double ol = hsvr::length(out);
                    if (ol > 1.0) at.at = s.at + out * (100.0 / ol);
                    HSVR_LOG("TestRoom: respawn: a body lies on slot %d; the new foe stands 1 m further back", (int)(&s - &g_room.slots[0]));
                    break;
                }
            }
            s.foe = room_spawn_foe(player, at);
            s.dead_since = s.foe ? -1.0 : now; // no foe: tried again after the delay
        }
    }

    // Two weapon tables side by side, 1.1 m in front of him, long side across his view.
    void room_place_tables(UObject* player)
    {
        using hsvr::Vec3;
        double floor_z = g_room.origin.z;
        if (UObject* capsule = object_property(player, STR("CapsuleComponent")))
        {
            Call hh(capsule, STR("GetScaledCapsuleHalfHeight"));
            hh.run();
            if (FProperty* p = hh.param(STR("ReturnValue")))
            {
                uint8_t* at = hh.buffer.data() + p->GetOffset_Internal();
                double half = p->GetClass().GetName() == STR("DoubleProperty") ? *reinterpret_cast<double*>(at) : *reinterpret_cast<float*>(at);
                floor_z -= half;
            }
        }
        Vec3 right{-g_room.forward.y, g_room.forward.x, 0.0};
        g_room.table_yaw = std::atan2(g_room.forward.y, g_room.forward.x) * 57.29577951308232;
        g_room.table_at.clear();
        int tier = hsvr::config().test_room_weapon_tier;
        for (int t = 0; t < 2; ++t)
        {
            Vec3 at = g_room.origin + g_room.forward * 110.0 + right * (t == 0 ? -104.0 : 104.0); // 35 cm between them (Matthew 2026-11-20)
            at.z = floor_z + 1.0;
            UObject* table = room_spawn(STR("/Game/Assets/Props/Blueprints/BP_Container_Weapon_Table_1.BP_Container_Weapon_Table_1_C"), player, at, yaw_quat(g_room.table_yaw),
                                        [&](UObject* a) {
                                            set_variable(a, STR("Tier"), tier < 0 ? 4.0 : (double)tier);
                                            // None of the table's own loot: it lands on the same spots as
                                            // the room's weapons and the overlap threw them (test 16:38).
                                            set_variable(a, STR("Weapon Spawn Chance"), 0.0);
                                            set_variable(a, STR("Armor Spawn Chance"), 0.0);
                                            set_variable(a, STR("Treasure Spawn Chance"), 0.0);
                                            set_variable(a, STR("Bolts Spawn Chance Modifier"), 0.0);
                                        });
            HSVR_LOG("TestRoom: weapon table %ls at (%.0f, %.0f, %.0f)", table ? table->GetName().c_str() : STR("NOT spawned"), at.x, at.y, at.z);
            if (table) g_room.table_at.push_back(at);
        }
    }

    // One weapon of every kind, lying along the tables (BaseMesh +Z, the blade, across his view),
    // through BP_Loot_Weapon (Spawn Chance 1): 1H/2H swords, maces, hafted, polearm, a shield, a
    // secondary. Each table also stocks one random piece of its own.
    void room_spawn_weapons(UObject* player)
    {
        using hsvr::Vec3;
        if (g_room.table_at.empty()) return;
        struct Kind { int specific; int premade; const char* label; };
        // Enum_WeaponType_Specific: 1 Swords 1H, 2 Swords 2H, 3 Maces 1H, 4 Maces 2H, 5 Hafted 1H,
        // 6 Hafted 2H, 7 Polearms. Premade kinds (Enum_WeaponType): 2 Shield, 3 Secondary.
        static const Kind kinds[] = {{1, -1, "sword 1H"}, {2, -1, "sword 2H"}, {3, -1, "mace 1H"}, {4, -1, "mace 2H"}, {2, 2, "shield"},
                                     {5, -1, "hafted 1H"}, {6, -1, "hafted 2H"}, {7, -1, "polearm"}, {3, 3, "secondary"}};
        int tier_cfg = hsvr::config().test_room_weapon_tier;
        static unsigned seed = (unsigned)GetTickCount64();
        Vec3 right{-g_room.forward.y, g_room.forward.x, 0.0};
        // Lying: weapon +Z along `right`, its +X up.
        hsvr::Quat lying = hsvr::quat_from_axes(Vec3{0, 0, 1}, hsvr::cross(right, Vec3{0, 0, 1}), right);
        const int count = (int)(sizeof(kinds) / sizeof(kinds[0]));
        for (int i = 0; i < count; ++i)
        {
            int t = (i < 5 || g_room.table_at.size() < 2) ? 0 : 1;
            int row = t == 0 ? i : i - 5;
            int rows = t == 0 ? 5 : count - 5;
            // Along his forward, 16 cm apart (the top is 101 cm deep, +-50; at 20 cm the outer ones
            // rolled off). Every other one lies 30 cm further along, so neighbouring heads and guards
            // are never side by side and don't start inside each other.
            double depth = (row - (rows - 1) * 0.5) * 16.0;
            Vec3 base = g_room.table_at[t];
            Vec3 at = base + g_room.forward * depth - right * (row % 2 ? 25.0 : 55.0);
            if (kinds[i].premade == 2) at = base + g_room.forward * depth; // a shield lies in the middle
            // The game's own weapon points are at 112 (top at 89-95 cm). Lower, a guard starts inside
            // the top and the push-out threw the weapons and tipped the table (test 2026-11-19 16:36).
            at.z = base.z + 112.0;
            seed = seed * 1103515245u + 12345u;
            int first = tier_cfg < 0 ? (int)((seed >> 16) % 8u) : (std::min)(tier_cfg, 8);
            hsvr::Quat q = kinds[i].premade == 2 ? hsvr::quat_from_axes(right, hsvr::cross(Vec3{0, 0, 1}, right), Vec3{0, 0, 1}) : lying;
            // The game's generator gives no weapon when a kind has none at that tier (Generate Weapon:
            // the kind's types that the tier allows; mace 2H and secondary at tier 6 gave none in the
            // test). Then the nearest other tiers are tried.
            bool got = false;
            int tier = first;
            for (int k = 0; k < 17 && !got; ++k)
            {
                int d = (k + 1) / 2;
                tier = k % 2 ? first - d : first + d;
                if (k == 0) tier = first;
                if (tier < 0 || tier > 8) continue;
                UObject* loot = room_spawn(STR("/Game/Blueprints/Procedural/LootPoints/BP_Loot_Weapon.BP_Loot_Weapon_C"), player, at, q, [&](UObject* a) {
                    set_variable(a, STR("Spawn Chance"), 1.0);
                    set_variable(a, STR("Tier"), (double)tier);
                    set_variable(a, STR("Premade"), kinds[i].premade >= 0 ? 1.0 : 0.0);
                    if (kinds[i].premade >= 0) set_variable(a, STR("Weapon Type (Premade)"), (double)kinds[i].premade);
                    else
                    {
                        set_variable(a, STR("Weapon Type"), (double)kinds[i].specific);
                        // Enum_WeaponType of that kind (0 1H, 1 2H), which the generator also reads.
                        int s = kinds[i].specific;
                        set_variable(a, STR("Weapon Type (Premade)"), (s == 1 || s == 3 || s == 5) ? 0.0 : 1.0);
                    }
                });
                // The loot point is destroyed in its BeginPlay but still readable this tick: its
                // passport says which weapon class it spawned (none = nothing).
                UObject* cls = nullptr;
                if (loot)
                    if (FProperty* pp = loot->GetPropertyByNameInChain(STR("Weapon Passport")))
                        if (FProperty* wc = struct_member_prefix(struct_type(pp), STR("WeaponClass")))
                            cls = *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(loot) + pp->GetOffset_Internal() + wc->GetOffset_Internal());
                got = cls != nullptr;
                if (got)
                    HSVR_LOG("TestRoom: %s (tier %d) spawned at (%.0f, %.0f, %.0f): %ls", kinds[i].label, tier, at.x, at.y, at.z, cls->GetName().c_str());
                else
                    HSVR_LOG("TestRoom: %s: the game has none at tier %d%s", kinds[i].label, tier, loot ? "" : " (no loot point)");
            }
            if (!got) HSVR_LOG("TestRoom: %s NOT spawned at any tier", kinds[i].label);
        }
    }

    void test_room_tick(const hsvr::Config& cfg)
    {
        if (!g_room.booted || g_room.failed || g_room.done) return;
        double now = hsvr::now_seconds();
        // Any save the game writes while the room is up is undone.
        if (g_room.armed && now >= g_room.next_save_check)
        {
            g_room.next_save_check = now + 0.25;
            FILETIME t{};
            if (file_time(g_room.save_path, t) && (!g_room.save_time_known || CompareFileTime(&t, &g_room.save_time) != 0)) room_restore_save("the game saved");
        }
        UObject* controller = find_player_controller();
        if (!controller) return;
        bool in_room = controller->GetFullName().find(g_room.map_key) != std::wstring::npos;
        if (!in_room)
        {
            if (g_room.entered) room_leave();
            return;
        }
        g_room.entered = true;
        UObject* player = nullptr;
        find_pawn_mesh(&player);
        if (!player) return;
        if (player != g_room.pawn)
        {
            g_room.pawn = player;
            g_room.pawn_at = now;
            g_room.stage = 0;
            HSVR_LOG("TestRoom: in the room (%ls)", player->GetFullName().c_str());
        }
        // The arena clears weapons, tables and foes at BeginPlay and again at +1 s, then spawns the
        // foes: the room is set up after that.
        if (g_room.stage == 0 && now - g_room.pawn_at >= 3.0)
        {
            g_room.stage = 1;
            g_room.origin = actor_location(player);
            Call fw(player, STR("GetActorForwardVector"));
            fw.run();
            hsvr::Vec3 f = fw.vec();
            f.z = 0.0;
            double fl = hsvr::length(f);
            g_room.forward = fl > 1e-3 ? f * (1.0 / fl) : hsvr::Vec3{1, 0, 0};
            room_place_foes(player);
            room_place_tables(player);
            g_room.watch_from = now; // diagnostics: tables now, weapons at +1.5 s
        }
        if (g_room.stage == 1 && now - g_room.pawn_at >= 4.5)
        {
            g_room.stage = 2;
            room_spawn_weapons(player);
        }
        room_settle_foes();
        if (g_room.stage >= 1) room_respawn_tick(player);
        // Diagnostics: for 5 s from the tables (weapons at +1.5 s), every table or loose weapon near the
        // tables that moves more than 3 cm between samples (20 a second).
        if (g_room.watch_from > 0.0 && now - g_room.watch_from < 5.0 && now >= g_room.next_watch)
        {
            g_room.next_watch = now + 0.05;
            std::vector<UObject*> items;
            UObjectGlobals::FindAllOf(STR("ModularWeaponBP_C"), items);
            std::vector<UObject*> tables;
            UObjectGlobals::FindAllOf(STR("BP_Container_Master_C"), tables);
            for (UObject* t : tables) items.push_back(t);
            for (UObject* o : items)
            {
                if (!o || o->GetName().rfind(STR("Default__"), 0) == 0) continue;
                UObject* c = object_property(o, STR("BaseMesh"));
                if (!c) c = object_property(o, STR("Box"));
                if (!c) continue;
                hsvr::Vec3 p = component_location(c);
                if (p.x == 0.0 && p.y == 0.0 && p.z == 0.0) continue;
                bool close_by = false;
                for (const hsvr::Vec3& ta : g_room.table_at) close_by = close_by || hsvr::length(p - ta) < 400.0;
                if (!close_by) continue;
                auto it = g_room.watch_last.find(o);
                if (it != g_room.watch_last.end() && hsvr::length(p - it->second) > 3.0)
                    HSVR_LOG("TestRoom: watch +%.2f s: %ls moved %.0f cm to (%.0f, %.0f, %.0f)", now - g_room.watch_from, o->GetName().c_str(), hsvr::length(p - it->second), p.x,
                             p.y, p.z);
                g_room.watch_last[o] = p;
            }
        }
        // Every foe's AI is stopped as soon as it is there (they spawn ~1 s in and would start
        // fighting before the room is set up), and again if it comes back on (zombie revive).
        if (now >= g_room.next_sweep)
        {
            g_room.next_sweep = now + 0.25;
            std::vector<UObject*> ais;
            UObjectGlobals::FindAllOf(STR("AI_BP_C"), ais);
            for (UObject* ai : ais)
            {
                if (!ai || ai->GetName().rfind(STR("Default__"), 0) == 0 || bool_property(ai, STR("DED"))) continue;
                Call gp(ai, STR("K2_GetPawn"));
                gp.run();
                UObject* foe = gp.object_result();
                if (foe && foe != player) make_foe_passive(ai, foe);
            }
        }
        (void)cfg;
    }

    // hsvr.test_room_report: where everything in the room is (unattended checks): him, every weapon
    // table (and whether it still stands), every loose weapon (on which table, and where on its top
    // in the table's own frame) and every foe (standing or down, AI off, what he holds).
    void room_report()
    {
        using hsvr::Vec3;
        UObject* player = nullptr;
        find_pawn_mesh(&player);
        if (!player)
        {
            HSVR_LOG("TestRoom: check: no player pawn");
            return;
        }
        Vec3 me = actor_location(player);
        HSVR_LOG("TestRoom: check: you at (%.0f, %.0f, %.0f), %s", me.x, me.y, me.z, bool_property(player, STR("DED")) ? "DEAD" : "alive");
        struct Table { Vec3 at; hsvr::Quat q; };
        std::vector<Table> tables;
        std::vector<UObject*> containers;
        UObjectGlobals::FindAllOf(STR("BP_Container_Master_C"), containers);
        for (UObject* c : containers)
        {
            if (!c || c->GetName().rfind(STR("Default__"), 0) == 0) continue;
            UObject* box = object_property(c, STR("Box"));
            if (!box) continue;
            Table t{component_location(box), component_rotation(box)};
            Vec3 up = t.q.rotate(Vec3{0, 0, 1});
            HSVR_LOG("TestRoom: check: table %ls at (%.0f, %.0f, %.0f), its up (%.2f, %.2f, %.2f) - %s", c->GetName().c_str(), t.at.x, t.at.y, t.at.z, up.x, up.y, up.z,
                     up.z > 0.95 ? "standing" : "TIPPED");
            tables.push_back(t);
        }
        std::vector<UObject*> weapons, willies;
        UObjectGlobals::FindAllOf(STR("ModularWeaponBP_C"), weapons);
        UObjectGlobals::FindAllOf(STR("Willie_BP_C"), willies);
        int loose = 0, on_table = 0;
        for (UObject* w : weapons)
        {
            if (!w || w->GetName().rfind(STR("Default__"), 0) == 0 || is_fist(w) || bool_property(w, STR("Is Sheathed"))) continue;
            bool held = false;
            for (UObject* c : willies)
                if (c && (object_property(c, STR("Weapon R")) == w || object_property(c, STR("Weapon L")) == w)) held = true;
            if (held) continue;
            UObject* base = object_property(w, STR("BaseMesh"));
            if (!base) continue;
            Vec3 at = component_location(base);
            // The generator's own throwaway weapons (destroyed, waiting for clean-up) sit at 0,0,0.
            if (at.x == 0.0 && at.y == 0.0 && at.z == 0.0) continue;
            ++loose;
            int best = -1;
            Vec3 local{};
            double best_d = 1e9;
            for (size_t i = 0; i < tables.size(); ++i)
            {
                Vec3 l = tables[i].q.inverse().rotate(at - tables[i].at);
                double d = std::sqrt(l.x * l.x + l.y * l.y);
                if (d < best_d) { best_d = d; best = (int)i; local = l; }
            }
            bool on = best >= 0 && std::fabs(local.x) < 55.0 && std::fabs(local.y) < 90.0 && local.z > 80.0 && local.z < 130.0;
            if (on) ++on_table;
            HSVR_LOG("TestRoom: check: weapon %ls at (%.0f, %.0f, %.0f); table %d frame (%.0f, %.0f, %.0f) - %s", w->GetName().c_str(), at.x, at.y, at.z, best, local.x,
                     local.y, local.z, on ? "on the table" : (best >= 0 && local.z <= 80.0 ? "OFF the table (low)" : "OFF the table"));
        }
        int foes = 0, standing = 0, ai_on = 0;
        static const FName pelvis_bone(STR("pelvis"), FNAME_Add);
        for (UObject* f : willies)
        {
            if (!f || f == player || f->GetName().rfind(STR("Default__"), 0) == 0) continue;
            Vec3 at = actor_location(f);
            if (at.x == 0.0 && at.y == 0.0 && at.z == 0.0) continue; // removed (Destroy Willie), waiting for clean-up
            ++foes;
            UObject* m = object_property(f, STR("Mesh"));
            Vec3 pe = m ? socket_location(m, pelvis_bone) : at;
            UObject* ai = object_property(f, STR("Controller"));
            bool ai_off = ai && bool_property(ai, STR("DED"));
            if (ai && !ai_off) ++ai_on;
            bool up = pe.z - me.z > -40.0;
            if (up) ++standing;
            UObject* wr = object_property(f, STR("Weapon R"));
            HSVR_LOG("TestRoom: check: foe %ls at (%.0f, %.0f, %.0f) %.0f cm from you, pelvis %.0f cm above your capsule centre (%s), %s, AI %s, AI In Combat %d, holds %ls",
                     f->GetName().c_str(), at.x, at.y, at.z, hsvr::length(at - me), pe.z - me.z, up ? "standing" : "down", bool_property(f, STR("DED")) ? "DEAD" : "alive",
                     ai ? (ai_off ? "off" : "ON") : "none", (int)bool_property(f, STR("AI In Combat")), wr ? wr->GetName().c_str() : STR("nothing"));
        }
        HSVR_LOG("TestRoom: check: %d table(s), %d loose weapon(s) of which %d on a table, %d foe(s) of which %d standing and %d with AI on", (int)tables.size(), loose,
                 on_table, foes, standing, ai_on);
        if (UObject* gm = room_game_mode())
        {
            int32_t* count = gm->GetValuePtrByPropertyNameInChain<int32_t>(STR("Enemy Count"));
            HSVR_LOG("TestRoom: check: game mode Enemy Count %d, Match Won %d, All Enemies Dead %d", count ? *count : -1, (int)bool_property(gm, STR("Match Won")),
                     (int)bool_property(gm, STR("All Enemies Dead")));
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
            // ini test_room=1: from the first menu straight into the test room.
            if (cfg.test_room)
            {
                parse_script_steps("M+2:hsvr.test_room_boot;M+4:hsvr.test_room_open", 0.0, entries);
                HSVR_LOG("TestRoom: on (test_room=1); the game goes to %s from the first menu", cfg.test_room_map.c_str());
            }
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
            if (entry.command == "hsvr.test_room_boot")
            {
                room_boot(cfg);
                continue;
            }
            if (entry.command == "hsvr.gc")
            {
                // Test: a garbage collection now (the shipping game has no "obj gc"); it runs at the
                // start of the next frame, and gc_sync picks up what it frees.
                static UObject* ksl = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetSystemLibrary"));
                if (ksl)
                {
                    Call c(ksl, STR("CollectGarbage"));
                    c.run();
                }
                HSVR_LOG("Script: hsvr.gc: garbage collection asked for");
                continue;
            }
            if (entry.command.rfind("hsvr.test_room_kill", 0) == 0 || entry.command.rfind("hsvr.test_room_beat", 0) == 0)
            {
                // "beat": Health 0 only (he goes down and gives up, not dead).
                bool beat_only = entry.command.rfind("hsvr.test_room_beat", 0) == 0;
                // Test: kills one room foe through the game's own path (Health 0, then his Death event:
                // DED, AI_BP DED, Minus Enemy). "hsvr.test_room_kill <slot>" or the first alive.
                int want = entry.command.size() > 20 ? atoi(entry.command.c_str() + 20) : -1;
                std::vector<UObject*> willies;
                UObjectGlobals::FindAllOf(STR("Willie_BP_C"), willies);
                bool killed = false;
                for (size_t i = 0; i < g_room.slots.size() && !killed; ++i)
                {
                    if (want >= 0 && (int)i != want) continue;
                    UObject* f = g_room.slots[i].foe;
                    if (!room_alive(f, willies)) continue;
                    set_variable(f, STR("Health"), 0.0);
                    // Health 0 alone makes him Dying (down, gives up); his Death event kills him.
                    if (!beat_only)
                    {
                        Call death(f, STR("Death"));
                        death.run();
                    }
                    killed = true;
                    HSVR_LOG("TestRoom: kill: slot %d, %ls (Health 0%s)", (int)i, f->GetName().c_str(), beat_only ? ", beaten only" : ", Death");
                }
                if (!killed) HSVR_LOG("TestRoom: kill: no living foe%s", want >= 0 ? " in that slot" : "");
                continue;
            }
            if (entry.command == "hsvr.test_room_report")
            {
                room_report();
                continue;
            }
            if (entry.command == "hsvr.test_room_open")
            {
                if (g_room.booted && !g_room.failed) run_console_command(std::string("open ") + cfg.test_room_map);
                return;
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
                else if (w == "actor_z") value = pawn ? actor_location(pawn).z : 0.0;
                else if (w.rfind("var:", 0) == 0 && pawn)
                {
                    // var:<pawn variable>, '_' for spaces (var:Leg_R_Tonus); a variable named with '_' is tried as written first
                    std::wstring n(w.begin() + 4, w.end());
                    FProperty* p = pawn->GetPropertyByNameInChain(n.c_str());
                    if (!p)
                    {
                        for (auto& ch : n)
                            if (ch == L'_') ch = L' ';
                        p = pawn->GetPropertyByNameInChain(n.c_str());
                    }
                    if (p) value = number_property(pawn, p);
                    else known = false;
                }
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
                    if (make_foe_passive(ai, foe)) ++stopped;
                }
                HSVR_LOG("Script: hsvr.passive_foe: %d foe(s) made passive (%d AI controllers)", stopped, (int)ais.size());
                continue;
            }
            if (entry.command.rfind("hsvr.foe_freeze ", 0) == 0)
            {
                // Stab tests: 1 = every foe's body stops simulating and holds his animated pose (a
                // standing target that placement and hits can't knock over); 0 = physics again.
                bool freeze = atoi(entry.command.c_str() + 16) != 0;
                UObject* player = nullptr;
                find_pawn_mesh(&player);
                std::vector<UObject*> foes;
                UObjectGlobals::FindAllOf(STR("Willie_BP_C"), foes);
                static const FName pelvis_bone(STR("pelvis"), FNAME_Add);
                for (UObject* foe : foes)
                {
                    if (!foe || foe == player || foe->GetName().rfind(STR("Default__"), 0) == 0) continue;
                    UObject* fm = object_property(foe, STR("Mesh"));
                    if (!fm) continue;
                    Call sim(fm, STR("SetAllBodiesSimulatePhysics"));
                    sim.first_flag(!freeze);
                    sim.run();
                    hsvr::Vec3 p = socket_location(fm, pelvis_bone);
                    HSVR_LOG("Script: hsvr.foe_freeze %d: %ls, pelvis (%.0f, %.0f, %.0f)", (int)freeze, foe->GetName().c_str(), p.x, p.y, p.z);
                }
                continue;
            }
            if (entry.command.rfind("hsvr.scabbard_test", 0) == 0)
            {
                // Off-hand scabbard test: the left hand takes the first belt scabbard (wherever the hand
                // is) and wants it turned <deg> about its own X; "hsvr.scabbard_test off" lets go.
                UObject* player = nullptr;
                UObject* pm = find_pawn_mesh(&player);
                std::string arg = entry.command.size() > 19 ? entry.command.substr(19) : "20";
                if (arg == "off")
                {
                    set_anim_grip(0, -1);
                    scabbard_let_go(0, "test");
                    continue;
                }
                double deg = std::atof(arg.c_str());
                int axis_i = arg.find('y') != std::string::npos ? 1 : (arg.find('z') != std::string::npos ? 2 : 0);
                UObject* sc = nullptr;
                if (player)
                    for (UObject* c : belt_scabbards(player))
                        if (scabbard_pin(c))
                        {
                            sc = c;
                            break;
                        }
                if (sc && pm)
                {
                    hsvr::Vec3 ax = hsvr::normalized(component_rotation(sc).rotate(axis_i == 0 ? hsvr::Vec3{1, 0, 0} : (axis_i == 1 ? hsvr::Vec3{0, 1, 0} : hsvr::Vec3{0, 0, 1})));
                    take_scabbard(0, pm, sc, hsvr::Vec3{0, 0, 20}, socket_rotation(pm, bone_fname("hand_l")), hsvr::quat_axis_angle(ax, deg * 3.14159265358979 / 180.0), true,
                                  hsvr::config());
                    HSVR_LOG("Script: hsvr.scabbard_test: left hand took %ls, wanted %.0f deg turned about its %c (limit %.0f)", sc->GetName().c_str(), deg, "XYZ"[axis_i],
                             hsvr::config().pose_scabbard_angle_deg);
                }
                else
                    HSVR_LOG("Script: hsvr.scabbard_test: no belt scabbard with a pin");
                continue;
            }
            if (entry.command.rfind("hsvr.sheathe_spawn ", 0) == 0)
            {
                // Test: the right hand's weapon goes onto the belt as the game puts a spawned weapon there
                // (Willie "Sheathe on Spawn"(Weapon, Slot): its own scabbard hangs at that slot).
                UObject* player = nullptr;
                find_pawn_mesh(&player);
                UObject* wr = player ? object_property(player, STR("Weapon R")) : nullptr;
                int slot = std::atoi(entry.command.c_str() + std::strlen("hsvr.sheathe_spawn "));
                if (wr)
                {
                    drop_weapon(1, player);
                    Call c(player, STR("Sheathe on Spawn"));
                    c.object(STR("Weapon"), wr);
                    c.byte(STR("Slot"), (uint8_t)slot);
                    bool ran = c.run();
                    std::wstring where;
                    for (const SlotDef& sd : k_slots)
                        if (object_property(player, sd.var) == wr) where = sd.var;
                    HSVR_LOG("Script: hsvr.sheathe_spawn %d: %ls %s; now in %ls", slot, wr->GetName().c_str(), ran ? "sheathed on spawn" : "(Sheathe on Spawn NOT found)",
                             where.empty() ? STR("no slot") : where.c_str());
                }
                else
                    HSVR_LOG("Script: hsvr.sheathe_spawn: nothing in the right hand");
                continue;
            }
            if (entry.command == "hsvr.sheathe_test L1" || entry.command == "hsvr.sheathe_test L2")
            {
                // Across: the right hand's weapon at the left belt slot's mouth, into the game's cross path.
                UObject* player = nullptr;
                find_pawn_mesh(&player);
                UObject* wr = player ? object_property(player, STR("Weapon R")) : nullptr;
                UObject* b = wr ? object_property(wr, STR("BaseMesh")) : nullptr;
                UObject* scab = wr ? object_property(wr, STR("Scabbard")) : nullptr;
                UObject* mesh = player ? object_property(player, STR("Mesh")) : nullptr;
                int slot = entry.command.back() == '2' ? 1 : 0;
                const char* sock = slot == 0 ? "Sheath_Slot_L_1" : "Sheath_Slot_L_2";
                if (b && scab && mesh)
                {
                    hsvr::Vec3 mouth = socket_location(mesh, bone_fname(sock));
                    hsvr::Vec3 z = hsvr::normalized(socket_rotation(mesh, bone_fname(sock)).rotate(hsvr::Vec3{0, 0, 1}));
                    hsvr::Vec3 xh = std::fabs(z.z) < 0.9 ? hsvr::Vec3{0, 0, 1} : hsvr::Vec3{1, 0, 0};
                    hsvr::Vec3 x = hsvr::normalized(xh - z * hsvr::dot(xh, z));
                    hsvr::Quat q = hsvr::quat_from_axes(x, hsvr::cross(z, x), z);
                    double len = blade_length(wr);
                    set_component_transform(b, mouth - z * (len + 3.0), q, true);
                    g_hold[1].settle_until = -1.0;
                    SheathFit fit;
                    auto known = g_sheath_fit.find(wr);
                    if (known != g_sheath_fit.end() && known->second.scab == scab) fit = known->second;
                    fit.scab = scab;
                    fit.slot_side = 0;
                    fit.slot = slot;
                    fit.socket = sock;
                    bool ok = sheathe_cross_begin(1, player, wr, fit);
                    HSVR_LOG("Script: hsvr.sheathe_test across: %ls at the mouth of %s: %s", wr->GetName().c_str(), sock, ok ? "sheathing" : "NOT started");
                }
                else
                    HSVR_LOG("Script: hsvr.sheathe_test across: %s", !wr ? "nothing in the right hand" : (!scab ? "its weapon has no scabbard" : "no mesh"));
                continue;
            }
            if (entry.command == "hsvr.sheathe_test")
            {
                // Sheathe test: the right hand's weapon is put with its tip at the right belt scabbard's
                // mouth, lined up, and the sheathe starts (then hsvr.arm_move 12 pushes it in).
                UObject* player = nullptr;
                find_pawn_mesh(&player);
                UObject* wr = player ? object_property(player, STR("Weapon R")) : nullptr;
                UObject* b = wr ? object_property(wr, STR("BaseMesh")) : nullptr;
                hsvr::Vec3 mouth, ax;
                const char* sock = "";
                if (b && belt_mouth(1, player, mouth, ax, &sock))
                {
                    // BaseMesh +Z along the slot's +Z.
                    hsvr::Vec3 z = hsvr::normalized(ax), xh = std::fabs(z.z) < 0.9 ? hsvr::Vec3{0, 0, 1} : hsvr::Vec3{1, 0, 0};
                    hsvr::Vec3 x = hsvr::normalized(xh - z * hsvr::dot(xh, z));
                    hsvr::Quat q = hsvr::quat_from_axes(x, hsvr::cross(z, x), z);
                    double len = blade_length(wr);
                    set_component_transform(b, mouth - z * (len + 3.0), q, true);
                    g_hold[1].settle_until = -1.0;
                    bool ok = sheathe_begin(1, player, wr);
                    HSVR_LOG("Script: hsvr.sheathe_test: %ls at the mouth of %s (blade %.0f cm): %s", wr->GetName().c_str(), sock, len, ok ? "sheathing" : "NOT started");
                }
                else
                    HSVR_LOG("Script: hsvr.sheathe_test: %s", wr ? "no empty right belt slot" : "nothing in the right hand");
                continue;
            }
            if (entry.command.rfind("hsvr.flip ", 0) == 0)
            {
                g_flip_request.store(atoi(entry.command.c_str() + 10));
                HSVR_LOG("Script: hsvr.flip %d", g_flip_request.load());
                continue;
            }
            if (entry.command == "hsvr.unstick")
            {
                // Stab tests: the weapon in his right hand lets go of any body it is stuck in (the
                // game's own Release All Jammed Bones).
                UObject* player = nullptr;
                find_pawn_mesh(&player);
                UObject* wr = player ? object_property(player, STR("Weapon R")) : nullptr;
                if (wr)
                {
                    Call r(wr, STR("Release All Jammed Bones"));
                    bool ok = r.run();
                    HSVR_LOG("Script: hsvr.unstick: %s; stuck in body now %d", ok ? "released" : "function not found", (int)bool_property(wr, STR("Stuck In Body")));
                }
                continue;
            }
            if (entry.command.rfind("hsvr.foe_front ", 0) == 0)
            {
                // Stab tests: every foe stood in front of him, facing him: "hsvr.foe_front <cm>" from
                // his actor, or "hsvr.foe_front tip <cm>" with the foe's chest that far beyond the tip of
                // the weapon in his right hand. His ragdoll is moved with him (a teleport of the
                // actor alone leaves the simulated body where it lay, stab-test3).
                using hsvr::Vec3;
                using hsvr::length;
                std::string arg = entry.command.substr(15);
                bool from_tip = arg.rfind("tip ", 0) == 0;
                double cm = atof(from_tip ? arg.c_str() + 4 : arg.c_str());
                UObject* player = nullptr;
                find_pawn_mesh(&player);
                if (!player) continue;
                Call fw(player, STR("GetActorForwardVector"));
                fw.run();
                Vec3 f = fw.vec();
                f.z = 0.0;
                double fl = length(f);
                if (fl < 1e-3) continue;
                f = f * (1.0 / fl);
                Vec3 base_at = actor_location(player);
                double ahead = cm;
                if (from_tip)
                {
                    UObject* wr = object_property(player, STR("Weapon R"));
                    RawArray* tips = wr ? wr->GetValuePtrByPropertyNameInChain<RawArray>(STR("Tips")) : nullptr;
                    double best = 0.0;
                    if (tips && tips->Data)
                        for (int32_t i = 0; i < tips->Num; ++i)
                            if (UObject* t = static_cast<UObject*>(tips->Data[i]))
                            {
                                Vec3 d = component_location(t) - base_at;
                                best = (std::max)(best, d.x * f.x + d.y * f.y);
                            }
                    ahead = best + 15.0 + cm; // his chest is about 15 cm in front of his pelvis line
                }
                Vec3 at = base_at + f * ahead;
                double yaw = std::atan2(-f.y, -f.x) * 57.29577951308232;
                std::vector<UObject*> foes;
                UObjectGlobals::FindAllOf(STR("Willie_BP_C"), foes);
                int moved = 0;
                static const FName pelvis_bone(STR("pelvis"), FNAME_Add);
                for (UObject* foe : foes)
                {
                    if (!foe || foe == player || foe->GetName().rfind(STR("Default__"), 0) == 0) continue;
                    Call mv(foe, STR("K2_SetActorLocationAndRotation"));
                    mv.three(STR("NewLocation"), at.x, at.y, at.z);
                    mv.three(STR("NewRotation"), 0.0, yaw, 0.0);
                    mv.flag(STR("bTeleport"), true);
                    mv.run();
                    UObject* fm = object_property(foe, STR("Mesh"));
                    Vec3 before, after;
                    if (fm)
                    {
                        before = socket_location(fm, pelvis_bone);
                        Call off(fm, STR("K2_AddWorldOffset"));
                        off.three(STR("DeltaLocation"), at.x - before.x, at.y - before.y, 0.0);
                        off.flag(STR("bTeleport"), true);
                        off.run();
                        Call still(fm, STR("SetAllPhysicsLinearVelocity"));
                        still.three(STR("NewVel"), 0.0, 0.0, 0.0);
                        still.run();
                        after = socket_location(fm, pelvis_bone);
                    }
                    ++moved;
                    HSVR_LOG("Script: hsvr.foe_front: %ls put %.0f cm in front of him%s, at (%.0f, %.0f, %.0f), yaw %.0f; his pelvis was (%.0f, %.0f, %.0f), now (%.0f, %.0f, %.0f)%s",
                             foe->GetName().c_str(), ahead, from_tip ? " (from the tip)" : "", at.x, at.y, at.z, yaw, before.x, before.y, before.z, after.x, after.y,
                             after.z, after.z < at.z - 40.0 ? " - he is down" : "");
                }
                if (!moved) HSVR_LOG("Script: hsvr.foe_front: no foe found");
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
            if (entry.command.rfind("hsvr.hide ", 0) == 0 || entry.command.rfind("hsvr.camera_probe", 0) == 0)
            {
                // hsvr.hide <text> 0|1: every component of his whose name or tags hold <text> (any case)
                // hidden from your view (1) or shown again (0). hsvr.camera_probe: where your eye is
                // against his neck and every visible part of him (the view-blocking hunt, playtest
                // 2026-11-18: flesh and cloth filling the lower view in armour).
                bool probe = entry.command.rfind("hsvr.camera_probe", 0) == 0;
                std::string text;
                int hide_value = 1;
                if (!probe)
                {
                    std::string rest = entry.command.substr(10);
                    size_t sp = rest.find_last_of(' ');
                    if (sp != std::string::npos && (rest.substr(sp + 1) == "0" || rest.substr(sp + 1) == "1"))
                    {
                        hide_value = rest[sp + 1] - '0';
                        rest.resize(sp);
                    }
                    for (char& ch : rest) ch = (char)tolower((unsigned char)ch);
                    text = rest;
                }
                UObject* pawn = nullptr;
                UObject* mesh = find_pawn_mesh(&pawn);
                if (!pawn || !mesh) continue;
                hsvr::Vec3 eye;
                hsvr::Quat eye_rot;
                bool eye_tracked = false;
                {
                    hsvr::ControllerState st = hsvr::controllers_state();
                    if (st.head_valid && hsvr::stereo_tracking_to_world(st.head, eye, eye_rot)) eye_tracked = true;
                }
                hsvr::Vec3 head = socket_location(mesh, bone_fname("head"));
                hsvr::Quat head_rot = socket_rotation(mesh, bone_fname("head"));
                if (!eye_tracked)
                {
                    // As the view's anchor: the eye offset forward along his facing and up.
                    Call fv(pawn, STR("GetActorForwardVector"));
                    fv.run();
                    hsvr::Vec3 f = fv.vec();
                    eye = head + f * hsvr::config().eye_forward_cm + hsvr::Vec3{0, 0, hsvr::config().eye_up_cm};
                    eye_rot = head_rot;
                }
                if (probe)
                {
                    for (const char* b : {"head", "neck_02", "neck_01", "spine_05", "spine_04", "clavicle_l", "clavicle_r"})
                    {
                        hsvr::Vec3 p = socket_location(mesh, bone_fname(b));
                        hsvr::Vec3 d = p - eye;
                        HSVR_LOG("Probe: %s is %.1f cm from your eye (dx %.1f dy %.1f dz %.1f)", b, hsvr::length(d), d.x, d.y, d.z);
                    }
                    hsvr::Vec3 fwd = eye_rot.rotate(hsvr::Vec3{1, 0, 0});
                    HSVR_LOG("Probe: eye (%.1f, %.1f, %.1f) %s, looking (%.2f, %.2f, %.2f)", eye.x, eye.y, eye.z, eye_tracked ? "from the headset" : "from the head bone + eye offset (no headset)",
                             fwd.x, fwd.y, fwd.z);
                }
                std::vector<UObject*> comps;
                UObjectGlobals::FindAllOf(STR("SceneComponent"), comps);
                int matched = 0;
                for (UObject* c : comps)
                {
                    if (!c || c->GetOuterPrivate() != pawn || !is_a(c, STR("PrimitiveComponent"))) continue;
                    std::wstring tags;
                    if (RawArray* t = c->GetValuePtrByPropertyNameInChain<RawArray>(STR("ComponentTags")))
                        for (int32_t i = 0; i < t->Num && t->Data; ++i) tags += reinterpret_cast<const FName*>(t->Data)[i].ToString() + STR(" ");
                    std::wstring name = c->GetName();
                    if (probe)
                    {
                        Call vis(c, STR("IsVisible"));
                        vis.run();
                        bool visible = false;
                        if (FProperty* r = vis.param(STR("ReturnValue"))) visible = (vis.buffer[r->GetOffset_Internal()] & static_cast<FBoolProperty*>(r)->GetFieldMask()) != 0;
                        if (!visible) continue;
                        // Its collision (physics bodies; armour and cloth pieces may have none: -1).
                        Call q(c, STR("GetClosestPointOnCollision"));
                        q.three(STR("Point"), eye.x, eye.y, eye.z);
                        q.run();
                        double d = q.number();
                        Call lb(c, STR("GetLocalBounds"));
                        hsvr::Vec3 mn, mx;
                        bool inside = false;
                        double box_d = -1.0;
                        if (lb.fn && lb.run())
                        {
                            mn = lb.vec(STR("Min"));
                            mx = lb.vec(STR("Max"));
                            hsvr::Vec3 local = component_rotation(c).inverse().rotate(eye - component_location(c));
                            hsvr::Vec3 cl{(std::max)(mn.x, (std::min)(mx.x, local.x)), (std::max)(mn.y, (std::min)(mx.y, local.y)), (std::max)(mn.z, (std::min)(mx.z, local.z))};
                            box_d = hsvr::length(local - cl);
                            inside = box_d == 0.0;
                        }
                        std::string mesh_name;
                        if (UObject* m = object_property(c, STR("SkeletalMesh"))) for (wchar_t ch : m->GetName()) mesh_name += (char)ch;
                        else if (UObject* m2 = object_property(c, STR("StaticMesh"))) for (wchar_t ch : m2->GetName()) mesh_name += (char)ch;
                        HSVR_LOG("Probe: visible %ls [%ls] mesh %s tags [%ls]: eye %.1f cm from its collision (-1 none), %.1f cm from its bounds%s", name.c_str(),
                                 c->GetClassPrivate()->GetName().c_str(), mesh_name.c_str(), tags.c_str(), d, box_d, inside ? " (INSIDE its bounds)" : "");
                        continue;
                    }
                    std::wstring hay = name + STR(" ") + tags;
                    for (auto& ch : hay) ch = (wchar_t)towlower(ch);
                    std::wstring want(text.begin(), text.end());
                    if (text != "all" && hay.find(want) == std::wstring::npos) continue;
                    set_owner_hidden(c, hide_value != 0);
                    ++matched;
                    HSVR_LOG("Script: hsvr.hide: %ls [%ls] %s", name.c_str(), tags.c_str(), hide_value ? "hidden from your view" : "shown again");
                }
                if (!probe) HSVR_LOG("Script: hsvr.hide %s %d: %d components", text.c_str(), hide_value, matched);
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
            if (entry.command.rfind("hsvr.thumb ", 0) == 0)
            {
                int side = atoi(entry.command.c_str() + 11) != 0 ? 1 : 0;
                size_t sp = entry.command.find(' ', 11);
                float v = sp == std::string::npos ? -1.0f : (float)atof(entry.command.c_str() + sp + 1);
                g_fake_thumb[side].store(v);
                HSVR_LOG("Script: hsvr.thumb %d %.2f", side, v);
                continue;
            }
            if (entry.command == "hsvr.input_list")
            {
                g_input_list_now = true;
                continue;
            }
            if (entry.command.rfind("hsvr.input ", 0) == 0)
            {
                // hsvr.input <name part, spaces as _> <value> <seconds>
                std::string rest = entry.command.substr(11);
                size_t a = rest.find(' ');
                std::string part = rest.substr(0, a);
                for (char& ch : part) if (ch == '_') ch = ' ';
                double value = 0.0, secs = 0.0;
                if (a != std::string::npos) std::sscanf(rest.c_str() + a, "%lf %lf", &value, &secs);
                TestInput t;
                t.part = std::wstring(part.begin(), part.end());
                t.value = value;
                t.once = secs <= 0.0;
                t.until = hsvr::now_seconds() + secs;
                g_test_inputs.push_back(t);
                HSVR_LOG("Script: hsvr.input '%s' %.2f for %.2f s", part.c_str(), value, secs);
                continue;
            }
            if (entry.command.rfind("hsvr.fingers ", 0) == 0)
            {
                // hsvr.fingers <side> <thumb> <index> <middle> <ring> <little> (-1 = your hand), or hsvr.fingers log
                if (entry.command == "hsvr.fingers log")
                {
                    g_finger_log_now.store(true);
                    continue;
                }
                const char* at = entry.command.c_str() + 13;
                char* end = nullptr;
                int side = (int)std::strtol(at, &end, 10) != 0 ? 1 : 0;
                float v[5];
                for (int f = 0; f < 5; ++f)
                {
                    v[f] = end ? std::strtof(end, &end) : -1.0f;
                    g_fake_fingers[side][f].store(v[f]);
                }
                g_finger_log_now.store(true);
                HSVR_LOG("Script: hsvr.fingers %d %.2f %.2f %.2f %.2f %.2f", side, v[0], v[1], v[2], v[3], v[4]);
                continue;
            }
            if (entry.command.rfind("hsvr.fist_shape ", 0) == 0)
            {
                g_fist_shape_test.store(atoi(entry.command.c_str() + 16));
                HSVR_LOG("Script: hsvr.fist_shape %d", g_fist_shape_test.load());
                continue;
            }
            if (entry.command.rfind("hsvr.body_hold ", 0) == 0)
            {
                g_body_hold_test.store(atoi(entry.command.c_str() + 15));
                HSVR_LOG("Script: hsvr.body_hold %d", g_body_hold_test.load());
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
            if (entry.command.rfind("hsvr.stick_click ", 0) == 0)
            {
                g_fake_stick_click.store(atoi(entry.command.c_str() + 17) != 0);
                HSVR_LOG("Script: hsvr.stick_click %d", (int)g_fake_stick_click.load());
                continue;
            }
            if (entry.command.rfind("hsvr.arm_to_item ", 0) == 0)
            {
                g_arm_floor[2] = atof(entry.command.c_str() + 17);
                g_fake_arm_mode.store(15);
                HSVR_LOG("Script: hsvr.arm_to_item %.0f cm above the nearest loose weapon's grip (right hand)", g_arm_floor[2]);
                continue;
            }
            if (entry.command.rfind("hsvr.fake_jitter ", 0) == 0)
            {
                g_fake_jitter_cm.store(atof(entry.command.c_str() + 17));
                HSVR_LOG("Script: hsvr.fake_jitter %.2f cm", g_fake_jitter_cm.load());
                continue;
            }
            if (entry.command.rfind("hsvr.arm_floor ", 0) == 0)
            {
                std::sscanf(entry.command.c_str() + 15, "%lf %lf %lf", &g_arm_floor[0], &g_arm_floor[1], &g_arm_floor[2]);
                g_fake_arm_mode.store(14);
                HSVR_LOG("Script: hsvr.arm_floor %.0f ahead, %.0f to the side, %.0f cm above the floor (right hand)", g_arm_floor[0], g_arm_floor[1], g_arm_floor[2]);
                continue;
            }
            if (entry.command.rfind("hsvr.arm_park ", 0) == 0)
            {
                g_park_socket = entry.command.substr(14);
                g_fake_arm_mode.store(13);
                HSVR_LOG("Script: hsvr.arm_park %s (right hand on that socket)", g_park_socket.c_str());
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
            if (entry.command.rfind("hsvr.fake_head_wobble ", 0) == 0)
            {
                double a = 0, hz = 1;
                sscanf(entry.command.c_str() + 22, "%lf %lf", &a, &hz);
                g_fake_wobble_deg.store(a);
                g_fake_wobble_hz.store(hz);
                HSVR_LOG("Script: hsvr.fake_head_wobble %.0f deg at %.2f Hz", a, hz);
            }
            if (entry.command.rfind("hsvr.fake_head_pose ", 0) == 0)
            {
                g_fake_head_pose.store(atoi(entry.command.c_str() + 20) != 0);
                HSVR_LOG("Script: hsvr.fake_head_pose %d (a still head at the view anchor when no headset is tracked)", g_fake_head_pose.load() ? 1 : 0);
            }
            if (entry.command.rfind("hsvr.fake_head_tilt ", 0) == 0)
            {
                double p = 0, r = 0, d = 0;
                sscanf(entry.command.c_str() + 20, "%lf %lf %lf", &p, &r, &d);
                g_fake_tilt_pitch.store(p);
                g_fake_tilt_roll.store(r);
                g_fake_tilt_drop.store(d);
                HSVR_LOG("Script: hsvr.fake_head_tilt pitch %.0f roll %.0f drop %.0f cm", p, r, d);
            }
            if (entry.command.rfind("hsvr.fake_head_drop ", 0) == 0)
            {
                g_fake_head_drop.store(atof(entry.command.c_str() + 20));
                HSVR_LOG("Script: hsvr.fake_head_drop %.1f cm (-1 = the made-up path)", g_fake_head_drop.load());
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
                g_fake_arm_epoch.fetch_add(1);
                int mode_arg = 0;
                double a1 = 0, a2 = 0, a3 = 0;
                int got = sscanf(entry.command.c_str() + 14, "%d %lf %lf %lf", &mode_arg, &a1, &a2, &a3);
                if (got >= 2) g_fake_two_up.store(a1);
                if (got >= 3) g_fake_thrust_pitch.store(a2);
                if (got >= 4) g_fake_thrust_up.store(a3);
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
            gc_sync(); // "obj gc" frees objects in the middle of this tick
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
        static bool last_tracked = on_freed([] { erase_gone_keys(last); });
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
        static bool last_pawn_tracked = track_freed(&last_pawn);
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
            // A short tap of right A kicks (B&S: a kick is a button): the game's own "Kick Event" (the
            // one its jump-tap and the AI use: leg from leg health and footing, its cooldown, the foot's
            // physics does the hit; kick_input research 2026-11-19).
            if (continue_since >= 0.0 && !continue_done && cfg.kick_on_a && now_seconds() - continue_since < cfg.kick_tap_s)
            {
                UObject* pawn = nullptr;
                find_pawn_mesh(&pawn);
                UFunction* kick = pawn ? pawn->GetFunctionByNameInChain(STR("Kick Event")) : nullptr;
                if (kick)
                {
                    std::vector<uint8_t> block = param_block(kick);
                    bool busy = bool_property(pawn, STR("Kick Cooldown")) || bool_property(pawn, STR("Kicking L")) || bool_property(pawn, STR("Kicking R"));
                    if (busy)
                        HSVR_LOG("Kick: right A tapped, still kicking or cooling down, no kick");
                    else
                    {
                        pawn->ProcessEvent(kick, block.data());
                        HSVR_LOG("Kick: right A tapped: Kick Event (kicking L %d R %d)", (int)bool_property(pawn, STR("Kicking L")),
                                 (int)bool_property(pawn, STR("Kicking R")));
                    }
                }
                else
                    HSVR_LOG("Kick: right A tapped, no Kick Event on %s", pawn ? "the character" : "(no character)");
            }
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

    // Drops every object the game has freed (the batch in g_freed_now) from the mod's global holders.
    // A state whose object is gone is cleared as if it had ended: nothing is called on the object.
    void prune_freed()
    {
        int dropped = 0;
        auto clear = [&](const char* what) {
            ++dropped;
            HSVR_LOG("Freed: %s: its object was freed by the game; dropped", what);
        };
        if (gone(g_logged_pawn)) g_logged_pawn = nullptr;
        if (gone(g_pose.pawn) || gone(g_pose.mesh) || gone(g_pose.driver) || gone(g_pose.instance))
        {
            g_pose = PoseState{}; // character gone: nothing to restore
            feet_reset();
            g_pose_anchor_fixed = false;
            g_pose_room_moved_local = {};
            clear("the posed character");
        }
        if (gone(g_helmet) || gone(g_helmet_pawn))
        {
            g_helmet = nullptr;
            g_helmet_pawn = nullptr;
            g_head_pieces.clear();
        }
        erase_gone(g_head_pieces);
        if (gone(g_head_used.pawn)) g_head_used = HeadUsed{};
        g_late_moved.erase(std::remove_if(g_late_moved.begin(), g_late_moved.end(), [](const MovedPiece& m) { return gone(m.comp); }), g_late_moved.end());
        erase_gone_keys(g_sheath_fit);
        for (auto it = g_sheath_fit.begin(); it != g_sheath_fit.end(); ++it)
            if (gone(it->second.scab)) it->second.scab = nullptr;
        for (int side = 0; side < 2; ++side)
        {
            if (gone(g_draw[side].weapon) || gone(g_draw[side].scabbard))
            {
                g_draw[side] = DrawState{};
                clear("a draw / sheathe");
            }
            if (gone(g_grip_log[side].weapon_seen)) g_grip_log[side].weapon_seen = nullptr;
            if (gone(g_arm_ignore[side].weapon)) g_arm_ignore[side] = ArmIgnore{};
            erase_gone(g_arm_ignore[side].joints);
            if (gone(g_pending_item[side])) g_pending_item[side] = nullptr;
            if (gone(g_hold[side].weapon))
            {
                g_hold[side] = HoldState{};
                clear(side == 0 ? "the left hand's hold" : "the right hand's hold");
            }
            if (gone(g_ghost[side].pawn) || gone(g_ghost[side].comp)) g_ghost[side] = Ghost{};
            for (ComSample& c : g_com_hist[side])
                if (gone(c.weapon)) c.weapon = nullptr;
            if (gone(g_object_grab[side].component))
            {
                g_object_grab[side] = ObjectGrab{};
                clear(side == 0 ? "the left hand's grab" : "the right hand's grab");
            }
            if (gone(g_body_grab[side].comp) || gone(g_body_grab[side].victim))
            {
                g_body_grab[side] = BodyGrab{};
                clear(side == 0 ? "the left hand's body grab" : "the right hand's body grab");
            }
            if (gone(g_young_joint[side]))
            {
                g_young_joint[side] = nullptr;
                g_young_since[side] = -1.0;
            }
            for (HitContact& h : g_hit_contact[side])
                if (gone(h.other)) h = HitContact{};
            if (gone(g_stab_watch[side].weapon))
            {
                g_stab_watch[side] = StabWatch{};
                clear("a stab watch");
            }
            else if (gone(g_stab_watch[side].push_comp))
                g_stab_watch[side].push_comp = nullptr;
        }
        g_thrown.erase(std::remove_if(g_thrown.begin(), g_thrown.end(), [](const Thrown& t) { return gone(t.weapon) || gone(t.base); }), g_thrown.end());
        if (gone(g_bring)) g_bring = nullptr;
        erase_gone_keys(g_item_track);
        if (gone(g_two.weapon))
        {
            g_two = TwoHand{};
            clear("the two-handed hold");
        }
        erase_gone_keys(g_weapon_parts);
        for (auto& wp : g_weapon_parts) erase_gone(wp.second);
        if (gone(g_punch_victim)) g_punch_victim = nullptr;
        erase_gone_keys(g_axis_measures);
        erase_gone_keys(g_item_handles);
        erase_gone_keys(g_watch);
        auto watched_gone = [](const WatchedVar& w) { return gone(w.owner); };
        g_watched.erase(std::remove_if(g_watched.begin(), g_watched.end(), watched_gone), g_watched.end());
        g_watch_previous.erase(std::remove_if(g_watch_previous.begin(), g_watch_previous.end(), watched_gone), g_watch_previous.end());
        for (UObject*& o : g_watch_owners)
            if (gone(o)) o = nullptr;
        // The test room.
        if (gone(g_room.pawn)) g_room.pawn = nullptr;
        erase_gone(g_room.settling);
        erase_gone_keys(g_room.watch_last);
        for (TestRoom::Slot& s : g_room.slots)
        {
            if (gone(s.foe)) s.foe = nullptr; // counts as dead; a new one comes after the delay
            erase_gone(s.gear);
        }
        for (TestRoom::Body& b : g_room.bodies) erase_gone(b.gear);
        g_room.bodies.erase(std::remove_if(g_room.bodies.begin(), g_room.bodies.end(), [](const TestRoom::Body& b) { return gone(b.body); }), g_room.bodies.end());
        if (dropped) HSVR_LOG("Freed: %d holder(s) cleared after the game freed %d object(s)", dropped, (int)g_freed_now.size());
    }

    // The Perf: line (see PerfScope). Object counts come from the engine's class hash tables.
    void perf_report(float delta_seconds)
    {
        static double since = -1.0;
        static uint64_t frames = 0;
        static double dt_sum = 0.0, dt_max = 0.0;
        static uint64_t last_ticks[PerfCount] = {}, last_calls[PerfCount] = {};
        double period = hsvr::config().perf_log_seconds;
        double now = hsvr::now_seconds();
        if (period <= 0.0) return;
        ++frames;
        dt_sum += delta_seconds;
        dt_max = (std::max)(dt_max, (double)delta_seconds);
        if (since < 0.0)
        {
            since = now;
            frames = 0, dt_sum = 0.0, dt_max = 0.0;
            for (int i = 0; i < PerfCount; ++i) last_ticks[i] = g_perf[i].ticks.load(), last_calls[i] = g_perf[i].calls.load();
            return;
        }
        if (now - since < period || frames == 0) return;
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        double ms_per_tick = 1000.0 / (double)f.QuadPart;
        double total = 0.0;
        std::string parts;
        for (int i = 0; i < PerfCount; ++i)
        {
            uint64_t t = g_perf[i].ticks.load(), c = g_perf[i].calls.load();
            double ms = (t - last_ticks[i]) * ms_per_tick / frames;
            double calls = (double)(c - last_calls[i]) / frames;
            last_ticks[i] = t, last_calls[i] = c;
            total += ms;
            if (ms < 0.005 && calls < 0.5) continue;
            char b[96];
            snprintf(b, sizeof b, ", %s %.2f (%.0f calls)", kPerfNames[i], ms, calls);
            parts += b;
        }
        // What the game has, to tell a pile-up (bodies, decals, actors) from the mod's own time.
        uint64_t census_start = perf_now();
        auto count = [](const TCHAR* cls) {
            std::vector<UObject*> v;
            UObjectGlobals::FindAllOf(cls, v);
            return (int)v.size();
        };
        int actors = count(STR("Actor")), willies = count(STR("Willie_BP_C")), weapons = count(STR("ModularWeaponBP_C")), ais = count(STR("AI_BP_C"));
        int decals = count(STR("DecalComponent"));
        std::vector<UObject*> prims;
        UObjectGlobals::FindAllOf(STR("PrimitiveComponent"), prims);
        int simulating = 0, sim_skel = 0;
        for (UObject* p : prims)
        {
            if (!p || p->GetName().rfind(STR("Default__"), 0) == 0) continue;
            UFunction* fn = p->GetFunctionByNameInChain(STR("IsSimulatingPhysics"));
            if (!fn) continue;
            IsSimulatingParams q{};
            p->ProcessEvent(fn, &q);
            if (!q.ReturnValue) continue;
            ++simulating;
            if (is_a(p, STR("SkeletalMeshComponent"))) ++sim_skel;
        }
        double census_ms = (perf_now() - census_start) * ms_per_tick;
        HSVR_LOG("Perf: %.1f fps (frame %.1f ms mean, %.0f worst) over %.0f s; mod %.2f ms a frame%s; game has %d objects, %d actors, %d Willies, %d weapons, %d AIs, "
                 "%d decals, %d primitives (%d simulating, %d of them skeletal); counted in %.1f ms",
                 frames / (now - since), 1000.0 * dt_sum / frames, 1000.0 * dt_max, now - since, total, parts.c_str(), Unreal::UObjectArray::GetNumElements(), actors,
                 willies, weapons, ais, decals, (int)prims.size(), simulating, sim_skel, census_ms);
        since = now;
        frames = 0, dt_sum = 0.0, dt_max = 0.0;
    }

    void game_tick(UObject* engine, float delta_seconds)
    {
        gc_sync();
        perf_report(delta_seconds); // outside the tick's own scope: the census is not per-frame time
        HSVR_PROF(PerfTick);
        g_engine = engine;
        hit_frame_refresh();
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
        // Inside his animation update when that runs (pose_after_anim_update); here otherwise (first
        // frames, menus, the switch off), so the pose still starts and releases.
        if (!cfg.pose_in_anim_update || hsvr::now_seconds() - g_pose_in_anim_at.load() > 0.25) pose_body(cfg);
        hide_own_head(cfg);
        grab_button(cfg, hsvr::controllers_state(), shared.menu_up.load());
        watch_vars_tick();
        if (g_console_done)
        {
            apply_picture_settings(cfg);
            keep_clouds_correct(cfg);
            keep_light_samples(cfg);
            run_dev_script(cfg);
            test_room_tick(cfg);
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
        if (!g_vr_started)
        {
            // No VR this session (SteamVR closed at launch with vr_auto=1, enabled=0, or no headset):
            // stay out of the game entirely, so it plays flat as if the mod weren't installed. No
            // hooks, no engine-tick work, no test room (that is driven from the tick).
            HSVR_LOG("Mod off for this session: no hooks installed, the game runs unmodified");
            Output::send<LogLevel::Verbose>(STR("[HalfSwordVR] off this session (no VR), see HalfSwordVR.log\n"));
            return;
        }
        Unreal::FUObjectArray::AddUObjectDeleteListener(&g_freed_listener); // gc_sync: what the game frees
        if (g_vr_started)
        {
            hsvr::stereo_set_anchor_provider(&provide_anchor);
            hsvr::stereo_set_latch_callback(&helmet_on_latch);
            hsvr::vr_install_present_hook();
        }
        Hook::RegisterProcessEventPreCallback(&capsule_before_tick, {false, false, STR("HalfSwordVR"), STR("CapsuleBefore")});
        Hook::RegisterProcessEventPostCallback(&capsule_after_tick, {false, false, STR("HalfSwordVR"), STR("CapsuleAfter")});
        Hook::RegisterProcessEventPostCallback(&pose_after_anim_update, {false, false, STR("HalfSwordVR"), STR("PoseAfterAnim")});
        Hook::RegisterProcessEventPostCallback(&feet_after_anim_update, {false, false, STR("HalfSwordVR"), STR("FeetAfterAnim")});
        Hook::RegisterProcessEventPostCallback(&fingers_after_anim_update, {false, false, STR("HalfSwordVR"), STR("FingersAfterAnim")});
        Hook::RegisterProcessEventPostCallback(&fingers_after_copy_update, {false, false, STR("HalfSwordVR"), STR("FingersAfterCopy")});
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
