// HalfSwordVR: native VR for Half Sword, loaded by UE4SS.
// This file is the glue to the game: it starts the headset link, installs our stereo device in
// the running engine, and tells it where the player's head is each frame.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwctype>
#include <string>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Mod/CppUserModBase.hpp>
#include <Unreal/FProperty.hpp>
#include <Unreal/FString.hpp>
#include <Unreal/Hooks.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "config.hpp"
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

    struct HideBoneParams
    {
        FName BoneName;
        uint8_t PhysBodyOption;
    };

    struct UnHideBoneParams
    {
        FName BoneName;
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

    // ---- hiding the head in first person -------------------------------------------------------
    struct BoneHiddenParams
    {
        FName BoneName;
        bool ReturnValue;
    };

    struct BoneIndexParams
    {
        FName BoneName;
        int32_t ReturnValue;
    };

    struct NameReturnParams
    {
        FName ReturnValue;
    };

    struct ObjectReturnParams
    {
        UObject* ReturnValue;
    };

    struct NameToNameParams
    {
        FName In;
        FName ReturnValue;
    };

    struct BoolParams
    {
        bool Value;
    };

    struct AttachedActorsParams
    {
        RawArray OutActors;
        bool bResetArray;
        bool bRecursivelyIncludeAttachedActors;
    };

    std::vector<UObject*> g_no_see_components;      // components we told "do not draw for the player"
    std::vector<UObject*> g_bone_hidden_components; // models whose head bone we stopped drawing
    UObject* g_head_pawn = nullptr;

    // The pawn's own components: its main mesh plus everything its blueprint added.
    void collect_components(UObject* pawn, UObject* mesh, std::vector<UObject*>& out)
    {
        if (mesh) out.push_back(mesh);
        const TCHAR* lists[] = {STR("BlueprintCreatedComponents"), STR("InstanceComponents")};
        for (const TCHAR* list_name : lists)
        {
            RawArray* list = pawn->GetValuePtrByPropertyNameInChain<RawArray>(list_name);
            if (!list || !list->Data || list->Num <= 0 || list->Num > 512) continue;
            for (int32_t i = 0; i < list->Num; ++i)
            {
                UObject* component = static_cast<UObject*>(list->Data[i]);
                if (component && std::find(out.begin(), out.end(), component) == out.end()) out.push_back(component);
            }
        }
    }

    std::wstring head_bone_text()
    {
        std::string narrow = hsvr::config().head_bone;
        return std::wstring(narrow.begin(), narrow.end());
    }

    // True if `bone` of the skinned mesh `skinned` is the head bone or hangs below it.
    bool is_head_bone(UObject* skinned, FName bone)
    {
        std::wstring head = head_bone_text();
        UFunction* parent_fn = skinned->GetFunctionByNameInChain(STR("GetParentBone"));
        for (int depth = 0; depth < 4; ++depth)
        {
            std::wstring text = bone.ToString();
            if (text.empty() || _wcsicmp(text.c_str(), L"None") == 0) return false;
            if (_wcsicmp(text.c_str(), head.c_str()) == 0) return true;
            if (!parent_fn) return false;
            NameToNameParams up{};
            up.In = bone;
            skinned->ProcessEvent(parent_fn, &up);
            bone = up.ReturnValue;
        }
        return false;
    }

    int head_bone_index(UObject* component)
    {
        UFunction* index_fn = component->GetFunctionByNameInChain(STR("GetBoneIndex"));
        if (!index_fn) return -1;
        BoneIndexParams index{};
        index.BoneName = head_bone_name();
        index.ReturnValue = -1;
        component->ProcessEvent(index_fn, &index);
        return index.ReturnValue;
    }

    // Follows what a component is attached to until it reaches a skinned mesh. Returns true if
    // that attachment point is on the head (a helmet, hair, a visor).
    bool hangs_off_head(UObject* component)
    {
        UObject* current = component;
        for (int depth = 0; depth < 3 && current; ++depth)
        {
            UFunction* parent_fn = current->GetFunctionByNameInChain(STR("GetAttachParent"));
            UFunction* socket_fn = current->GetFunctionByNameInChain(STR("GetAttachSocketName"));
            if (!parent_fn || !socket_fn) return false;
            ObjectReturnParams parent{};
            current->ProcessEvent(parent_fn, &parent);
            if (!parent.ReturnValue) return false;
            if (UFunction* socket_bone_fn = parent.ReturnValue->GetFunctionByNameInChain(STR("GetSocketBoneName")))
            {
                NameReturnParams socket{};
                current->ProcessEvent(socket_fn, &socket);
                NameToNameParams bone{};
                bone.In = socket.ReturnValue;
                parent.ReturnValue->ProcessEvent(socket_bone_fn, &bone);
                return is_head_bone(parent.ReturnValue, bone.ReturnValue);
            }
            current = parent.ReturnValue;
        }
        return false;
    }

    // True for a model that is only the head or something worn on it (told by its name or the
    // name of the asset it shows), as opposed to a whole body that merely has a head bone.
    bool is_head_piece(UObject* component)
    {
        static const wchar_t* const words[] = {L"head", L"helm", L"hair", L"hood", L"coif", L"visor"};
        std::wstring names = component->GetName();
        UObject* asset = object_property(component, STR("SkinnedAsset"));
        if (asset) names += L" " + asset->GetName();
        std::transform(names.begin(), names.end(), names.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
        for (const wchar_t* word : words)
        {
            if (names.find(word) != std::wstring::npos) return true;
        }
        return false;
    }

    // One-time description of how the character is put together, for the log.
    void log_components_once(UObject* pawn, const std::vector<UObject*>& components, const ue::FVector& view_head)
    {
        HSVR_LOG("Character parts (%d):", (int)components.size());
        for (UObject* component : components)
        {
            std::wstring attach = L"-";
            std::wstring socket_text = L"-";
            UFunction* parent_fn = component->GetFunctionByNameInChain(STR("GetAttachParent"));
            UFunction* socket_fn = component->GetFunctionByNameInChain(STR("GetAttachSocketName"));
            if (parent_fn && socket_fn)
            {
                ObjectReturnParams parent{};
                component->ProcessEvent(parent_fn, &parent);
                if (parent.ReturnValue) attach = parent.ReturnValue->GetName();
                NameReturnParams socket{};
                component->ProcessEvent(socket_fn, &socket);
                socket_text = socket.ReturnValue.ToString();
            }
            int bone = head_bone_index(component);
            double distance = -1.0;
            if (bone >= 0)
            {
                if (UFunction* location_fn = component->GetFunctionByNameInChain(STR("GetSocketLocation")))
                {
                    SocketLocationParams head{};
                    head.InSocketName = head_bone_name();
                    component->ProcessEvent(location_fn, &head);
                    double dx = head.ReturnValue.X - view_head.X, dy = head.ReturnValue.Y - view_head.Y, dz = head.ReturnValue.Z - view_head.Z;
                    distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                }
            }
            UObject* asset = object_property(component, STR("SkinnedAsset"));
            if (!asset) asset = object_property(component, STR("StaticMesh"));
            HSVR_LOG("  %ls | attached to %ls at %ls | head bone %d (%.0f cm from view head) | asset %ls", component->GetFullName().c_str(), attach.c_str(),
                     socket_text.c_str(), bone, distance, asset ? asset->GetName().c_str() : L"-");
        }
        // Things carried or worn that are separate actors (weapons, armour pieces).
        if (UFunction* attached_fn = pawn->GetFunctionByNameInChain(STR("GetAttachedActors")))
        {
            AttachedActorsParams attached{};
            attached.bResetArray = true;
            attached.bRecursivelyIncludeAttachedActors = true;
            pawn->ProcessEvent(attached_fn, &attached);
            HSVR_LOG("Attached actors (%d):", attached.OutActors.Num);
            for (int32_t i = 0; i < attached.OutActors.Num && i < 64 && attached.OutActors.Data; ++i)
            {
                UObject* actor = static_cast<UObject*>(attached.OutActors.Data[i]);
                if (!actor) continue;
                std::wstring socket_text = L"-";
                if (UFunction* socket_fn = actor->GetFunctionByNameInChain(STR("GetAttachParentSocketName")))
                {
                    NameReturnParams socket{};
                    actor->ProcessEvent(socket_fn, &socket);
                    socket_text = socket.ReturnValue.ToString();
                }
                UObject* owner = object_property(actor, STR("Owner"));
                HSVR_LOG("  %ls | at %ls | owner %ls", actor->GetFullName().c_str(), socket_text.c_str(), owner ? owner->GetName().c_str() : L"-");
            }
            // The small list the engine allocated for this one-time call is left to the engine.
        }
    }

    // Keeps the character's head (and anything worn on it) out of sight while we look out of it,
    // and brings it back for the third-person view. Checked a few times a second because the game
    // resets this when it changes the character's mesh.
    void update_head_visibility(UObject* pawn, UObject* mesh, const ue::FVector& view_head)
    {
        hsvr::Config cfg = hsvr::config();
        bool want_hidden = cfg.hide_head && cfg.anchor == "head" && !hsvr::shared().third_person.load();
        static double last_check = 0.0;
        static bool last_want = false;
        double now = hsvr::now_seconds();
        bool new_pawn = pawn != g_head_pawn;
        if (!new_pawn && want_hidden == last_want && now - last_check < 0.25) return;
        last_check = now;
        last_want = want_hidden;
        if (new_pawn)
        {
            g_head_pawn = pawn;
            g_no_see_components.clear(); // those belonged to the previous character
            g_bone_hidden_components.clear();
        }

        std::vector<UObject*> components;
        collect_components(pawn, mesh, components);
        if (new_pawn) log_components_once(pawn, components, view_head);

        // Two ways to hide a head. The default marks whole pieces (the head model, hair, anything
        // attached at the head) as "not drawn for the player who owns them", which changes nothing
        // the game's own logic looks at. The other way (hide_head_bones=1) also stops drawing the
        // head bone of every body model; games use that same switch for a severed head, so the
        // game may react to it.
        int bones_changed = 0, parts_changed = 0;
        for (UObject* component : components)
        {
            bool has_head_bone = head_bone_index(component) >= 0;
            if (has_head_bone)
            {
                auto ours = std::find(g_bone_hidden_components.begin(), g_bone_hidden_components.end(), component);
                bool is_ours = ours != g_bone_hidden_components.end();
                bool want_bone_hidden = want_hidden && cfg.hide_head_bones;
                UFunction* hidden_fn = component->GetFunctionByNameInChain(STR("IsBoneHiddenByName"));
                UFunction* hide_fn = component->GetFunctionByNameInChain(STR("HideBoneByName"));
                UFunction* show_fn = component->GetFunctionByNameInChain(STR("UnHideBoneByName"));
                if (hidden_fn && hide_fn && show_fn && (want_bone_hidden || is_ours))
                {
                    BoneHiddenParams hidden{};
                    hidden.BoneName = head_bone_name();
                    component->ProcessEvent(hidden_fn, &hidden);
                    if (want_bone_hidden)
                    {
                        if (!hidden.ReturnValue)
                        {
                            HideBoneParams hide{};
                            hide.BoneName = head_bone_name();
                            hide.PhysBodyOption = 0; // PBO_None: only stop drawing it
                            component->ProcessEvent(hide_fn, &hide);
                            ++bones_changed;
                        }
                        if (!is_ours) g_bone_hidden_components.push_back(component);
                    }
                    else
                    {
                        // Only bring back what we hid ourselves, never a head the game removed.
                        if (hidden.ReturnValue)
                        {
                            UnHideBoneParams show{};
                            show.BoneName = head_bone_name();
                            component->ProcessEvent(show_fn, &show);
                            ++bones_changed;
                        }
                        g_bone_hidden_components.erase(ours);
                    }
                }
            }
            UFunction* no_see_fn = component->GetFunctionByNameInChain(STR("SetOwnerNoSee"));
            if (!no_see_fn) continue;
            auto marked = std::find(g_no_see_components.begin(), g_no_see_components.end(), component);
            bool is_marked = marked != g_no_see_components.end();
            if (!want_hidden && !is_marked) continue;
            if (want_hidden && !is_marked)
            {
                bool is_head_part = has_head_bone ? (component != mesh && is_head_piece(component)) : hangs_off_head(component);
                if (!is_head_part) continue;
            }
            // Sent again on every check while hidden: it costs nothing when already set, and the
            // game may have switched it back.
            BoolParams no_see{};
            no_see.Value = want_hidden;
            component->ProcessEvent(no_see_fn, &no_see);
            if (want_hidden == is_marked) continue;
            if (want_hidden)
            {
                g_no_see_components.push_back(component);
                HSVR_LOG("  not drawn for the player: %ls", component->GetName().c_str());
            }
            else
            {
                g_no_see_components.erase(marked);
            }
            ++parts_changed;
        }
        if (bones_changed || parts_changed)
        {
            HSVR_LOG("%s the character's head: %d part(s) switched, head bone changed on %d model(s)", want_hidden ? "Hid" : "Showing", parts_changed, bones_changed);
        }
    }

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

        update_head_visibility(pawn, mesh, socket.ReturnValue);
        return true;
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

    // Visibility values are the engine's ESlateVisibility: 0 visible, 1 collapsed, 2 hidden,
    // 3 not hit-testable (self and children), 4 not hit-testable (self only).
    void log_widget_tree(UObject* widget, int depth, int& lines)
    {
        if (!widget || depth > 6 || lines >= 400) return;
        int visibility = -1;
        if (UFunction* visibility_fn = widget->GetFunctionByNameInChain(STR("GetVisibility")))
        {
            ByteReturnParams v{};
            widget->ProcessEvent(visibility_fn, &v);
            visibility = v.ReturnValue;
        }
        ++lines;
        HSVR_LOG("  %*s%ls | visibility %d", depth * 2, "", widget->GetFullName().c_str(), visibility);
        // A widget made in the game's editor keeps its parts in a tree of its own.
        if (UObject* tree = object_property(widget, STR("WidgetTree")))
        {
            log_widget_tree(object_property(tree, STR("RootWidget")), depth + 1, lines);
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
            log_widget_tree(child.ReturnValue, depth + 1, lines);
        }
    }

    void log_widgets(const char* reason)
    {
        std::vector<UObject*> widgets;
        UObjectGlobals::FindAllOf(STR("UserWidget"), widgets);
        HSVR_LOG("Widgets (%s): %d exist; those on screen:", reason, (int)widgets.size());
        int lines = 0;
        for (UObject* widget : widgets)
        {
            if (!widget) continue;
            if (widget->GetName().starts_with(STR("Default__"))) continue;
            UFunction* in_viewport_fn = widget->GetFunctionByNameInChain(STR("IsInViewport"));
            if (!in_viewport_fn) continue;
            BoolReturnParams in_viewport{};
            widget->ProcessEvent(in_viewport_fn, &in_viewport);
            if (!in_viewport.ReturnValue) continue;
            log_widget_tree(widget, 0, lines);
        }
        HSVR_LOG("Widgets: end of list (%d line(s))", lines);
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
        static bool recenter_down = false, ui_down = false, view_down = false, mode_down = false;
        if (GetForegroundWindow() != nullptr)
        {
            if (key_pressed_once(cfg.recenter_key, recenter_down)) shared.recenter_requested.store(true);
            if (key_pressed_once(cfg.toggle_ui_key, ui_down))
            {
                bool toggled = !shared.ui_toggled.load();
                shared.ui_toggled.store(toggled);
                if (cfg.ui_panel_auto)
                {
                    HSVR_LOG(toggled ? "Menu panel: always shown" : "Menu panel: shown only while a menu is up");
                }
                else
                {
                    HSVR_LOG(toggled ? "Menu panel: hidden" : "Menu panel: shown");
                }
            }
            if (key_pressed_once(cfg.toggle_mode_key, mode_down)) shared.separate_override.store(hsvr::stereo_separate_target() ? 0 : 1);
            if (key_pressed_once(cfg.toggle_view_key, view_down))
            {
                bool third = !shared.third_person.load();
                shared.third_person.store(third);
                HSVR_LOG(third ? "View: the game's own camera (third person)" : "View: from the character's head (first person)");
            }
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
