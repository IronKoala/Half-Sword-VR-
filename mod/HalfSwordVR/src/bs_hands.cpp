// Hands that hold what they grip (build 3). See bs_hands.hpp.
//
// Everything here goes through the game's own code, read from its decompiled script
// (docs/game-internals.md section 7, and "Hands like Blade & Sorcery" in docs/phase4-body-follow.md):
// - The game's grab key is a tap: press and let go within 0.25 s. On a full hand it drops the
//   weapon with "Set Up Right/Left Hand Weapon(none)"; on an empty hand it searches for something
//   to grab (a sphere trace for simulating bodies, 25 to 50 cm in front of the hand) or lets go
//   of what it holds ("Release Grab R/L"). A tap cannot mean "hold while squeezed", so the mod
//   does the same steps itself at the moment the grip is squeezed or let go.
// - Grabbing: "Grab Specific Object R H / L H" (component, location) is the game's grab with a
//   given target. It joins the hand to the component's root body; the joint is then moved onto
//   the exact bone that was touched (the game's own search does the same with the bone it hit).
#include "bs_hands.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <Unreal/Core/HAL/UnrealMemory.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FProperty.hpp>
#include <Unreal/Property/FBoolProperty.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include "log.hpp"

using namespace RC;
using namespace RC::Unreal;

namespace hsvr
{
    namespace
    {
        struct V3
        {
            double x, y, z;
        };
        double dist(const V3& a, const V3& b)
        {
            double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }
        struct RawArray
        {
            void* data;
            int32_t num;
            int32_t max;
        };

        // A call to any reflected function with its parameters set and read by name.
        struct Call
        {
            UObject* target = nullptr;
            UFunction* fn = nullptr;
            alignas(16) uint8_t buf[2048];
            bool init(UObject* object, const TCHAR* name)
            {
                target = object;
                fn = object ? object->GetFunctionByNameInChain(name) : nullptr;
                if (!fn || fn->GetParmsSize() > (int)sizeof(buf)) return false;
                memset(buf, 0, sizeof(buf));
                return true;
            }
            FProperty* prop(const wchar_t* name)
            {
                for (FProperty* p : fn->ForEachProperty())
                    if (p->GetName() == name) return p;
                return nullptr;
            }
            template <class T> T* at(const wchar_t* name)
            {
                FProperty* p = prop(name);
                return p ? reinterpret_cast<T*>(buf + p->GetOffset_Internal()) : nullptr;
            }
            void run() { target->ProcessEvent(fn, buf); }
        };

        UObject* obj(UObject* owner, const TCHAR* name)
        {
            if (!owner || !owner->GetPropertyByNameInChain(name)) return nullptr;
            UObject** value = owner->GetValuePtrByPropertyNameInChain<UObject*>(name);
            return value ? *value : nullptr;
        }

        bool read_bool(UObject* owner, const TCHAR* name)
        {
            FProperty* p = owner ? owner->GetPropertyByNameInChain(name) : nullptr;
            if (!p) return false;
            auto* flag = static_cast<FBoolProperty*>(p);
            return (*(reinterpret_cast<uint8_t*>(owner) + p->GetOffset_Internal() + flag->GetByteOffset()) & flag->GetFieldMask()) != 0;
        }

        bool socket_at(UObject* component, const TCHAR* socket, V3& out)
        {
            Call c;
            if (!c.init(component, STR("GetSocketLocation"))) return false;
            FName* name = c.at<FName>(L"InSocketName");
            V3* ret = c.at<V3>(L"ReturnValue");
            if (!name || !ret) return false;
            *name = FName(socket, FNAME_Add);
            c.run();
            out = *ret;
            return true;
        }

        bool bone_at(UObject* component, FName bone, V3& out)
        {
            Call c;
            if (!c.init(component, bone == FName() ? STR("K2_GetComponentLocation") : STR("GetSocketLocation"))) return false;
            if (bone != FName())
            {
                FName* name = c.at<FName>(L"InSocketName");
                if (!name) return false;
                *name = bone;
            }
            V3* ret = c.at<V3>(L"ReturnValue");
            if (!ret) return false;
            c.run();
            out = *ret;
            return true;
        }

        bool ask_bool(UObject* object, const TCHAR* function, const wchar_t* param, FName value)
        {
            Call c;
            if (!c.init(object, function)) return false;
            if (FName* n = c.at<FName>(param)) *n = value;
            bool* ret = c.at<bool>(L"ReturnValue");
            if (!ret) return false;
            c.run();
            return *ret;
        }

        std::string narrow(const std::wstring& w)
        {
            std::string s;
            for (wchar_t ch : w) s += (ch < 128) ? (char)ch : '?';
            return s;
        }

        std::string g_last_grab;
    } // namespace

    bool bs_grabbing(UObject* pawn, int hand) { return read_bool(pawn, hand ? STR("Grabbed R") : STR("Grabbed L")); }

    const char* bs_last_grab() { return g_last_grab.c_str(); }

    bool bs_grab(UObject* pawn, int hand, double radius_cm)
    {
        const char* side = hand ? "right" : "left";
        UObject* mesh = obj(pawn, STR("Mesh"));
        V3 palm{};
        if (!mesh || !socket_at(mesh, hand ? STR("weapon_rSocket") : STR("weapon_lSocket"), palm)) return false;

        // The game's own search: a sphere trace for the object types it grabs ([3, 6, 9, 7]).
        static UObject* kismet = nullptr;
        static UObject* statics = nullptr;
        if (!kismet) kismet = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetSystemLibrary"));
        if (!statics) statics = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__GameplayStatics"));
        Call trace;
        if (!kismet || !statics || !trace.init(kismet, STR("SphereTraceMultiForObjects")))
        {
            HSVR_LOG("Grab: the engine's sphere trace was not found; nothing grabbed");
            return false;
        }
        static uint8_t types[4] = {3, 6, 9, 7};
        UObject* ignore[3] = {pawn, obj(pawn, STR("Weapon R")), obj(pawn, STR("Weapon L"))};
        int ignore_n = 1 + (ignore[1] ? 1 : 0);
        if (ignore[2]) ignore[ignore_n++] = ignore[2];
        RawArray* types_arg = trace.at<RawArray>(L"ObjectTypes");
        RawArray* ignore_arg = trace.at<RawArray>(L"ActorsToIgnore");
        RawArray* hits = trace.at<RawArray>(L"OutHits");
        auto* hits_prop = CastField<FArrayProperty>(trace.prop(L"OutHits"));
        V3* start = trace.at<V3>(L"Start");
        V3* end = trace.at<V3>(L"End");
        float* radius = trace.at<float>(L"Radius");
        UObject** context = trace.at<UObject*>(L"WorldContextObject");
        bool* ignore_self = trace.at<bool>(L"bIgnoreSelf");
        bool* found = trace.at<bool>(L"ReturnValue");
        if (!types_arg || !ignore_arg || !hits || !hits_prop || !start || !end || !radius || !context || !found)
        {
            HSVR_LOG("Grab: the engine's sphere trace has unexpected parameters; nothing grabbed");
            return false;
        }
        *context = pawn;
        *start = palm;
        *end = V3{palm.x, palm.y, palm.z + 0.5};
        *radius = (float)radius_cm;
        *types_arg = RawArray{types, 4, 4};
        *ignore_arg = RawArray{ignore, ignore_n, ignore_n};
        if (ignore_self) *ignore_self = true;
        trace.run();
        int32_t stride = hits_prop->GetInner()->GetElementSize();
        int32_t count = hits->num;
        uint8_t* data = static_cast<uint8_t*>(hits->data);
        // The inputs point at the mod's own memory: clear them before anything could free them.
        *types_arg = RawArray{nullptr, 0, 0};
        *ignore_arg = RawArray{nullptr, 0, 0};

        UObject* best_comp = nullptr;
        UObject* best_actor = nullptr;
        FName best_bone;
        double best_d = 1e9;
        int considered = 0;
        Call brk;
        if (*found && data && count > 0 && stride > 0 && brk.init(statics, STR("BreakHitResult")))
        {
            FProperty* hit_param = brk.prop(L"Hit");
            for (int32_t i = 0; hit_param && i < count && i < 64; ++i)
            {
                memset(brk.buf, 0, sizeof(brk.buf));
                memcpy(brk.buf + hit_param->GetOffset_Internal(), data + (size_t)i * stride, (size_t)std::min<int32_t>(stride, hit_param->GetSize()));
                brk.run();
                UObject* comp = *brk.at<UObject*>(L"HitComponent");
                UObject* actor = *brk.at<UObject*>(L"HitActor");
                FName bone = *brk.at<FName>(L"HitBoneName");
                if (!comp || comp == mesh || actor == pawn) continue;
                // As the game's own search: not tagged NoGrab, not a sheathed thing, simulating, and
                // not a weapon that someone holds.
                if (ask_bool(comp, STR("ComponentHasTag"), L"Tag", FName(STR("NoGrab"), FNAME_Add))) continue;
                if (actor && ask_bool(actor, STR("ActorHasTag"), L"Tag", FName(STR("Sheathed"), FNAME_Add))) continue;
                if (!ask_bool(comp, STR("IsSimulatingPhysics"), L"BoneName", bone)) continue;
                if (actor && actor->GetPropertyByNameInChain(STR("Is Held")) && read_bool(actor, STR("Is Held"))) continue;
                ++considered;
                V3 where{};
                if (!bone_at(comp, bone, where)) where = *brk.at<V3>(L"ImpactPoint");
                double d = dist(where, palm);
                if (d < best_d)
                {
                    best_d = d;
                    best_comp = comp;
                    best_actor = actor;
                    best_bone = bone;
                }
            }
        }
        if (data) FMemory::Free(data);
        *hits = RawArray{nullptr, 0, 0};
        if (!best_comp)
        {
            HSVR_LOG("Grab: %s hand squeezed; nothing to grab within %.0f cm (%d hit(s), %d grabbable)", side, radius_cm, count, considered);
            return false;
        }

        // The game's grab with that target, at the palm (so nothing is moved to meet the hand) ...
        Call grab;
        if (!grab.init(pawn, hand ? STR("Grab Specific Object R H") : STR("Grab Specific Object L H")) || grab.fn->GetParmsSize() != 32)
        {
            HSVR_LOG("Grab: the game's grab-this-object event is missing or changed; nothing grabbed");
            return false;
        }
        *reinterpret_cast<UObject**>(grab.buf) = best_comp;
        *reinterpret_cast<V3*>(grab.buf + 8) = palm;
        grab.run();

        // ... then its joint moved from the root body onto the bone that was touched.
        std::wstring bone_text = best_bone.ToString();
        bool rebound = false;
        if (best_bone != FName())
        {
            if (FName* stored = pawn->GetValuePtrByPropertyNameInChain<FName>(hand ? STR("Grab Bone R") : STR("Grab Bone L"))) *stored = best_bone;
            UObject* joint = obj(pawn, hand ? STR("PhysicsConstraint R Hand") : STR("PhysicsConstraint L Hand"));
            Call set;
            if (joint && set.init(joint, STR("SetConstrainedComponents")))
            {
                UObject** c1 = set.at<UObject*>(L"Component1");
                FName* b1 = set.at<FName>(L"BoneName1");
                UObject** c2 = set.at<UObject*>(L"Component2");
                FName* b2 = set.at<FName>(L"BoneName2");
                if (c1 && b1 && c2 && b2)
                {
                    *c1 = mesh;
                    *b1 = FName(hand ? STR("hand_r") : STR("hand_l"), FNAME_Add);
                    *c2 = best_comp;
                    *b2 = best_bone;
                    set.run();
                    rebound = true;
                }
            }
        }
        g_last_grab = narrow(best_actor ? best_actor->GetName() : best_comp->GetName()) + " by " + (bone_text.empty() || bone_text == L"None" ? "its body" : narrow(bone_text));
        HSVR_LOG("Grab: %s hand holds %s (%.0f cm from the palm)%s; the game says grabbed: %s", side, g_last_grab.c_str(), best_d,
                 best_bone != FName() ? (rebound ? ", joined at that bone" : ", but the joint could NOT be moved to that bone") : "",
                 bs_grabbing(pawn, hand) ? "yes" : "NO");
        return true;
    }

    void bs_release(UObject* pawn, int hand)
    {
        Call c;
        if (!c.init(pawn, hand ? STR("Release Grab R") : STR("Release Grab L")) || c.fn->GetParmsSize() != 0)
        {
            HSVR_LOG("Grab: the game's Release Grab event is missing or changed");
            return;
        }
        c.run();
        HSVR_LOG("Grab: %s hand lets go; the game says still grabbed: %s", hand ? "right" : "left", bs_grabbing(pawn, hand) ? "YES" : "no");
    }

    bool bs_drop(UObject* pawn, int hand)
    {
        // The two places in the game's event graph that drop a held weapon (its grab key on a
        // full hand: "Set Up Right/Left Hand Weapon(none, ...)", then nothing else). Entered
        // directly, after checking that the script there really is that call, so a game update
        // that moved the code cannot run something else.
        const int32_t entry = hand ? 218772 : 226565;
        const TCHAR* expect = hand ? STR("Set Up Right Hand Weapon") : STR("Set Up Left Hand Weapon");
        UFunction* graph = pawn ? pawn->GetFunctionByNameInChain(STR("ExecuteUbergraph_Willie_BP")) : nullptr;
        if (!graph || graph->GetParmsSize() != 4)
        {
            HSVR_LOG("Drop: the game's event graph was not found; the weapon stays in the hand");
            return false;
        }
        auto& script = graph->GetScript();
        bool ok = false;
        if (entry + 5 <= script.Num())
        {
            const uint8_t* code = script.GetData() + entry;
            // EX_VirtualFunction (0x1B) or EX_LocalVirtualFunction (0x45), then the function's name.
            if (code[0] == 0x1B || code[0] == 0x45)
            {
                uint32_t name_index = 0, want_index = 0;
                memcpy(&name_index, code + 1, 4);
                FName want(expect, FNAME_Add);
                memcpy(&want_index, &want, 4);
                ok = name_index == want_index;
            }
        }
        if (!ok)
        {
            HSVR_LOG("Drop: the game's drop code is not where it was (game updated?); the weapon stays in the hand");
            return false;
        }
        int32_t param = entry;
        pawn->ProcessEvent(graph, &param);
        return true;
    }
} // namespace hsvr
