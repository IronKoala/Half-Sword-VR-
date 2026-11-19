// HalfSwordCustomWeapon: test of an UNCOOKED custom weapon part, built at runtime from an OBJ file.
//
// Press F9 (configurable) while holding a weapon in the right hand. The mod then:
//   1. reads <mod folder>/test_blade.obj (or the file named in CustomWeapon.ini),
//   2. builds a runtime UStaticMesh that is just the model's bounding box, with simple box collision
//      (UStaticMesh::BuildFromStaticMeshDescriptions, fast build: the only build a shipping game can do),
//      and puts it in the weapon's "Head" slot. The Head component keeps the game's OnComponentHit
//      binding, mass override and Tip damage points, so hits still go through the game's own damage code.
//      The box is hidden; it only collides.
//   3. shows the real model with a ProceduralMeshComponent attached to the Head (visual only, no collision),
//      using the Head's original material.
// Everything is logged to <mod folder>/CustomWeapon.log.
//
// Engine facts this relies on (UE 5.4 source):
//   - StaticMesh.cpp BuildFromMeshDescriptions: non-editor builds require bFastBuild; with
//     bBuildSimpleCollision it adds one FKBoxElem of the render bounds and calls CreatePhysicsMeshes.
//   - StaticMeshComponent.cpp SetStaticMesh: recreates render and physics state.
//   - BodyInstance.cpp InitBody: a non-static body with bAutoWeld re-welds to its attach parent in a
//     game world, so the Head stays welded to BaseMesh after the swap.
//   - GameplayStatics.cpp SpawnObject: NewObject(Outer, Class); UObject::GetWorld follows the outer,
//     so a mesh outered to the weapon actor counts as runtime.
//   - UStaticMeshDescription::CreateCube is NOT used: it computes corners as (Center+HalfExtents)*sign,
//     which is only right for a box centred on the origin.

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

#include <windows.h>

using namespace RC;
using namespace RC::Unreal;

namespace
{
    // ---- files and log ------------------------------------------------------------------------
    std::wstring g_dir; // the mod folder (…/Mods/HalfSwordCustomWeapon/)
    FILE* g_log = nullptr;

    void find_mod_dir()
    {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&find_mod_dir), &self);
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(self, path, MAX_PATH);
        std::wstring p(path);
        // …/Mods/HalfSwordCustomWeapon/dlls/main.dll -> …/Mods/HalfSwordCustomWeapon/
        for (int i = 0; i < 2; ++i)
        {
            size_t cut = p.find_last_of(L"\\/");
            if (cut == std::wstring::npos) break;
            p = p.substr(0, cut);
        }
        g_dir = p + L"\\";
    }

    void log(const char* format, ...)
    {
        if (!g_log) g_log = _wfopen((g_dir + L"CustomWeapon.log").c_str(), L"w");
        if (!g_log) return;
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        va_list args;
        va_start(args, format);
        vfprintf(g_log, format, args);
        va_end(args);
        fputc('\n', g_log);
        fflush(g_log);
    }

    std::string narrow(const std::wstring& w)
    {
        std::string s;
        for (wchar_t c : w) s += (c < 128) ? (char)c : '?';
        return s;
    }

    std::string name_of(UObject* o)
    {
        return o ? narrow(o->GetFullName()) : std::string("(none)");
    }

    // ---- settings (CustomWeapon.ini, key=value) ------------------------------------------------
    struct Settings
    {
        std::wstring model = L"test_blade.obj";
        int key = 0x78;           // F9
        int fit_to_original = 1;  // scale the model so its far end (max Z) matches the original head's
        int flip_winding = 0;     // flip if the model shows inside out
        double mass_kg = 0.0;     // > 0: set the Head's mass to this; 0: keep the game's mass
        int hide_original = 1;    // hide the box (the old head is replaced either way)
    };
    Settings g_settings;

    void load_settings()
    {
        std::wstring path = g_dir + L"CustomWeapon.ini";
        FILE* f = _wfopen(path.c_str(), L"r");
        if (!f)
        {
            log("No CustomWeapon.ini, using defaults");
            return;
        }
        char line[512];
        while (fgets(line, sizeof line, f))
        {
            std::string s(line);
            size_t hash = s.find_first_of(";#");
            if (hash != std::string::npos) s = s.substr(0, hash);
            size_t eq = s.find('=');
            if (eq == std::string::npos) continue;
            auto trim = [](std::string v) {
                size_t a = v.find_first_not_of(" \t\r\n"), b = v.find_last_not_of(" \t\r\n");
                return a == std::string::npos ? std::string() : v.substr(a, b - a + 1);
            };
            std::string k = trim(s.substr(0, eq)), v = trim(s.substr(eq + 1));
            if (k == "model") g_settings.model = std::wstring(v.begin(), v.end());
            else if (k == "key") g_settings.key = (int)strtol(v.c_str(), nullptr, 0);
            else if (k == "fit_to_original") g_settings.fit_to_original = atoi(v.c_str());
            else if (k == "flip_winding") g_settings.flip_winding = atoi(v.c_str());
            else if (k == "mass_kg") g_settings.mass_kg = atof(v.c_str());
            else if (k == "hide_original") g_settings.hide_original = atoi(v.c_str());
        }
        fclose(f);
        log("Settings: model=%s key=0x%X fit_to_original=%d flip_winding=%d mass_kg=%.3f hide_original=%d",
            narrow(g_settings.model).c_str(), g_settings.key, g_settings.fit_to_original, g_settings.flip_winding,
            g_settings.mass_kg, g_settings.hide_original);
    }

    // ---- the model -----------------------------------------------------------------------------
    struct V3
    {
        double x, y, z;
    };
    V3 sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
    V3 unit(V3 a)
    {
        double l = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
        return l > 1e-12 ? V3{a.x / l, a.y / l, a.z / l} : V3{0, 0, 1};
    }

    // Laid out exactly like the engine's FVector / FVector2D (doubles in UE 5). Checked at runtime
    // against the reflected struct sizes before use.
    struct Model
    {
        std::vector<V3> positions;           // one per triangle corner (no sharing: flat or file normals)
        std::vector<V3> normals;
        std::vector<double> uvs;             // u, v pairs
        std::vector<int32_t> triangles;      // Unreal winding
        V3 min{1e30, 1e30, 1e30}, max{-1e30, -1e30, -1e30};
        V3 tangent_scale{1, 1, 1};           // scale of the component the model is shown on
    };

    bool load_obj(const std::wstring& path, Model& m)
    {
        FILE* f = _wfopen(path.c_str(), L"r");
        if (!f)
        {
            log("Cannot open model %s", narrow(path).c_str());
            return false;
        }
        std::vector<V3> v, vn;
        std::vector<std::pair<double, double>> vt;
        struct Corner
        {
            int v, t, n;
        };
        std::vector<std::vector<Corner>> faces;
        char line[1024];
        while (fgets(line, sizeof line, f))
        {
            if (line[0] == 'v' && line[1] == ' ')
            {
                V3 p{};
                sscanf(line + 2, "%lf %lf %lf", &p.x, &p.y, &p.z);
                v.push_back(p);
            }
            else if (line[0] == 'v' && line[1] == 'n')
            {
                V3 p{};
                sscanf(line + 3, "%lf %lf %lf", &p.x, &p.y, &p.z);
                vn.push_back(p);
            }
            else if (line[0] == 'v' && line[1] == 't')
            {
                double a = 0, b = 0;
                sscanf(line + 3, "%lf %lf", &a, &b);
                vt.push_back({a, b});
            }
            else if (line[0] == 'f' && line[1] == ' ')
            {
                std::vector<Corner> face;
                char* s = line + 2;
                while (*s)
                {
                    while (*s == ' ' || *s == '\t') ++s;
                    if (!*s || *s == '\n' || *s == '\r') break;
                    Corner c{0, 0, 0};
                    c.v = strtol(s, &s, 10);
                    if (*s == '/')
                    {
                        ++s;
                        if (*s != '/') c.t = strtol(s, &s, 10);
                        if (*s == '/')
                        {
                            ++s;
                            c.n = strtol(s, &s, 10);
                        }
                    }
                    while (*s && *s != ' ' && *s != '\t' && *s != '\n' && *s != '\r') ++s;
                    face.push_back(c);
                }
                if (face.size() >= 3) faces.push_back(face);
            }
        }
        fclose(f);

        auto fix = [](int i, size_t count) { return i < 0 ? (int)count + i : i - 1; };
        for (auto& face : faces)
        {
            // Triangle fan; OBJ order is counter-clockwise from outside.
            for (size_t k = 1; k + 1 < face.size(); ++k)
            {
                Corner c[3] = {face[0], face[k], face[k + 1]};
                V3 p[3];
                bool ok = true;
                for (int i = 0; i < 3; ++i)
                {
                    int vi = fix(c[i].v, v.size());
                    if (vi < 0 || vi >= (int)v.size()) ok = false;
                    else p[i] = v[vi];
                }
                if (!ok) continue;
                V3 flat = unit(cross(sub(p[1], p[0]), sub(p[2], p[0])));
                // Unreal's front face is the reverse (StaticMeshOperations.cpp: normal = Cross(P2-P0, P1-P0)).
                int order[3] = {0, 2, 1};
                if (g_settings.flip_winding) order[1] = 1, order[2] = 2;
                for (int j : order)
                {
                    m.positions.push_back(p[j]);
                    int ni = fix(c[j].n, vn.size());
                    m.normals.push_back(c[j].n && ni >= 0 && ni < (int)vn.size() ? unit(vn[ni]) : flat);
                    int ti = fix(c[j].t, vt.size());
                    if (c[j].t && ti >= 0 && ti < (int)vt.size())
                    {
                        m.uvs.push_back(vt[ti].first);
                        m.uvs.push_back(1.0 - vt[ti].second);
                    }
                    else
                    {
                        m.uvs.push_back(0.0);
                        m.uvs.push_back(0.0);
                    }
                    m.triangles.push_back((int32_t)m.triangles.size());
                }
            }
        }
        for (V3& p : m.positions)
        {
            m.min = {std::min(m.min.x, p.x), std::min(m.min.y, p.y), std::min(m.min.z, p.z)};
            m.max = {std::max(m.max.x, p.x), std::max(m.max.y, p.y), std::max(m.max.z, p.z)};
        }
        log("Model %s: %zu file vertices, %zu triangles, bounds (%.1f %.1f %.1f) to (%.1f %.1f %.1f) cm",
            narrow(path).c_str(), v.size(), m.triangles.size() / 3, m.min.x, m.min.y, m.min.z, m.max.x, m.max.y,
            m.max.z);
        return !m.triangles.empty();
    }

    void scale_model(Model& m, double s)
    {
        for (V3& p : m.positions) p = {p.x * s, p.y * s, p.z * s};
        m.min = {m.min.x * s, m.min.y * s, m.min.z * s};
        m.max = {m.max.x * s, m.max.y * s, m.max.z * s};
    }

    // ---- reflection calls ----------------------------------------------------------------------
    struct RawArray
    {
        const void* Data;
        int32_t Num;
        int32_t Max;
    };

    UObject* object_property(UObject* owner, const TCHAR* name)
    {
        if (!owner) return nullptr;
        UObject** value = owner->GetValuePtrByPropertyNameInChain<UObject*>(name);
        return value ? *value : nullptr;
    }

    UScriptStruct* struct_type(FProperty* property)
    {
        if (!property || property->GetClass().GetName() != STR("StructProperty")) return nullptr;
        return static_cast<FStructProperty*>(property)->GetStruct();
    }

    // FName text keeps the letter case of whichever spelling the engine registered first (a
    // parameter "Position" can read back as "position"), so names are compared ignoring case.
    bool same_name(FProperty* p, const TCHAR* name)
    {
        return _wcsicmp(p->GetName().c_str(), name) == 0;
    }

    std::string list_names(UStruct* type)
    {
        std::string all;
        if (!type) return all;
        for (FProperty* p : type->ForEachProperty()) all += (all.empty() ? "" : ", ") + narrow(p->GetName());
        return all;
    }

    FProperty* member(UScriptStruct* type, const TCHAR* name)
    {
        if (!type) return nullptr;
        for (FProperty* p : type->ForEachProperty())
            if (same_name(p, name)) return p;
        log("  ! struct %s has no member %s (members: %s)", narrow(type->GetName()).c_str(), narrow(name).c_str(), list_names(type).c_str());
        return nullptr;
    }

    bool write_three(uint8_t* field, FProperty* property, double a, double b, double c)
    {
        UScriptStruct* type = struct_type(property);
        if (!type) return false;
        double values[3] = {a, b, c};
        int i = 0;
        for (FProperty* m : type->ForEachProperty())
        {
            if (i >= 3) break;
            auto kind = m->GetClass().GetName();
            uint8_t* at = field + m->GetOffset_Internal();
            if (kind == STR("DoubleProperty")) *reinterpret_cast<double*>(at) = values[i];
            else if (kind == STR("FloatProperty")) *reinterpret_cast<float*>(at) = (float)values[i];
            else return false;
            ++i;
        }
        return i == 3;
    }

    V3 read_three(const uint8_t* field, FProperty* property)
    {
        double out[3] = {0, 0, 0};
        UScriptStruct* type = struct_type(property);
        if (!type) return {0, 0, 0};
        int i = 0;
        for (FProperty* m : type->ForEachProperty())
        {
            if (i >= 3) break;
            auto kind = m->GetClass().GetName();
            const uint8_t* at = field + m->GetOffset_Internal();
            if (kind == STR("DoubleProperty")) out[i] = *reinterpret_cast<const double*>(at);
            else if (kind == STR("FloatProperty")) out[i] = *reinterpret_cast<const float*>(at);
            ++i;
        }
        return {out[0], out[1], out[2]};
    }

    // Calls a reflected (Blueprint-callable) function with parameters set by name. A parameter the
    // function does not have is logged, so a wrong guess about a name shows up in the log.
    struct Call
    {
        UObject* target = nullptr;
        UFunction* fn = nullptr;
        std::string label;
        std::vector<uint8_t> buffer;
        bool ok = true;

        Call(UObject* object, const TCHAR* name) : target(object)
        {
            label = narrow(name);
            fn = object ? object->GetFunctionByNameInChain(name) : nullptr;
            if (!fn)
            {
                log("  ! function %s not found on %s", label.c_str(), name_of(object).c_str());
                ok = false;
            }
            size_t size = 16;
            if (fn)
                for (FProperty* p : fn->ForEachProperty())
                    size = (std::max)(size, (size_t)p->GetOffset_Internal() + (size_t)p->GetSize());
            buffer.assign(size + 16, 0);
        }
        FProperty* param(const TCHAR* name)
        {
            if (!fn) return nullptr;
            for (FProperty* p : fn->ForEachProperty())
                if (same_name(p, name)) return p;
            log("  ! %s has no parameter %s (parameters: %s)", label.c_str(), narrow(name).c_str(), list_names(fn).c_str());
            ok = false;
            return nullptr;
        }
        uint8_t* at(const TCHAR* name)
        {
            FProperty* p = param(name);
            return p ? buffer.data() + p->GetOffset_Internal() : nullptr;
        }
        Call& object(const TCHAR* name, UObject* value)
        {
            if (uint8_t* a = at(name)) *reinterpret_cast<UObject**>(a) = value;
            return *this;
        }
        Call& i32(const TCHAR* name, int32_t value)
        {
            if (uint8_t* a = at(name)) *reinterpret_cast<int32_t*>(a) = value;
            return *this;
        }
        Call& f32(const TCHAR* name, float value)
        {
            if (uint8_t* a = at(name)) *reinterpret_cast<float*>(a) = value;
            return *this;
        }
        Call& byte(const TCHAR* name, uint8_t value)
        {
            if (uint8_t* a = at(name)) *a = value;
            return *this;
        }
        Call& flag(const TCHAR* name, bool value)
        {
            FProperty* p = param(name);
            if (!p) return *this;
            auto* b = static_cast<FBoolProperty*>(p);
            uint8_t* a = buffer.data() + p->GetOffset_Internal() + b->GetByteOffset();
            if (value) *a |= b->GetFieldMask();
            else *a &= ~b->GetFieldMask();
            return *this;
        }
        Call& three(const TCHAR* name, double x, double y, double z)
        {
            FProperty* p = param(name);
            if (p && !write_three(buffer.data() + p->GetOffset_Internal(), p, x, y, z))
            {
                log("  ! %s: %s is not a 3-number struct", label.c_str(), narrow(name).c_str());
                ok = false;
            }
            return *this;
        }
        // A read-only array parameter pointing at our own memory (the engine copies it).
        Call& array(const TCHAR* name, const void* data, int32_t count)
        {
            if (uint8_t* a = at(name)) *reinterpret_cast<RawArray*>(a) = RawArray{count ? data : nullptr, count, count};
            return *this;
        }
        bool run()
        {
            if (!ok || !fn || !target)
            {
                log("  ! not calling %s", label.c_str());
                return false;
            }
            target->ProcessEvent(fn, buffer.data());
            return true;
        }
        UObject* ret_object()
        {
            uint8_t* a = at(STR("ReturnValue"));
            return a ? *reinterpret_cast<UObject**>(a) : nullptr;
        }
        int32_t ret_i32()
        {
            uint8_t* a = at(STR("ReturnValue"));
            return a ? *reinterpret_cast<int32_t*>(a) : -1;
        }
        float ret_f32()
        {
            uint8_t* a = at(STR("ReturnValue"));
            return a ? *reinterpret_cast<float*>(a) : 0.0f;
        }
        bool ret_bool()
        {
            FProperty* p = param(STR("ReturnValue"));
            if (!p) return false;
            auto* b = static_cast<FBoolProperty*>(p);
            return (buffer[p->GetOffset_Internal() + b->GetByteOffset()] & b->GetFieldMask()) != 0;
        }
    };

    UObject* find(const TCHAR* path)
    {
        UObject* o = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, path);
        if (!o) log("  ! %s not found in the game", narrow(path).c_str());
        return o;
    }

    int32_t struct_size(const TCHAR* path)
    {
        auto* s = UObjectGlobals::StaticFindObject<UScriptStruct*>(nullptr, nullptr, path);
        return s ? s->GetPropertiesSize() : -1;
    }

    // ---- the game ------------------------------------------------------------------------------
    UObject* g_engine = nullptr;

    UObject* player_pawn()
    {
        UObject* viewport = object_property(g_engine, STR("GameViewport"));
        UObject* game_instance = object_property(viewport, STR("GameInstance"));
        if (!game_instance) return nullptr;
        auto* players = game_instance->GetValuePtrByPropertyNameInChain<RawArray>(STR("LocalPlayers"));
        if (!players || players->Num < 1 || !players->Data) return nullptr;
        UObject* local_player = static_cast<UObject* const*>(players->Data)[0];
        UObject* controller = object_property(local_player, STR("PlayerController"));
        return object_property(controller, STR("Pawn"));
    }

    float mass_of(UObject* component)
    {
        if (!component) return -1.0f;
        Call c(component, STR("GetMass"));
        return c.run() ? c.ret_f32() : -1.0f;
    }

    V3 world_location(UObject* component)
    {
        Call c(component, STR("K2_GetComponentLocation"));
        if (!c.run()) return {0, 0, 0};
        FProperty* p = c.param(STR("ReturnValue"));
        return p ? read_three(c.buffer.data() + p->GetOffset_Internal(), p) : V3{0, 0, 0};
    }

    // The mesh-space box of a static mesh asset (UStaticMesh::GetBoundingBox).
    bool mesh_box(UObject* mesh, V3& lo, V3& hi)
    {
        if (!mesh) return false;
        Call c(mesh, STR("GetBoundingBox"));
        if (!c.run()) return false;
        FProperty* ret = c.param(STR("ReturnValue"));
        UScriptStruct* box = struct_type(ret);
        FProperty* mn = member(box, STR("Min"));
        FProperty* mx = member(box, STR("Max"));
        if (!ret || !mn || !mx)
        {
            log("  ! could not read the mesh bounds");
            return false;
        }
        uint8_t* base = c.buffer.data() + ret->GetOffset_Internal();
        lo = read_three(base + mn->GetOffset_Internal(), mn);
        hi = read_three(base + mx->GetOffset_Internal(), mx);
        return true;
    }

    void log_tips(UObject* weapon)
    {
        std::vector<UObject*> comps;
        UObjectGlobals::FindAllOf(STR("SceneComponent"), comps);
        static FName tip_tag(STR("Tip"), FNAME_Add);
        for (UObject* c : comps)
        {
            if (object_property(c, STR("AttachParent")) == nullptr) continue;
            Call owner(c, STR("GetOwner"));
            if (!owner.run() || owner.ret_object() != weapon) continue;
            Call has(c, STR("ComponentHasTag"));
            if (FProperty* p = has.param(STR("Tag"))) *reinterpret_cast<FName*>(has.buffer.data() + p->GetOffset_Internal()) = tip_tag;
            if (!has.run() || !has.ret_bool()) continue;
            V3 w = world_location(c);
            log("  Tip %s, attached to %s, world (%.1f %.1f %.1f)", narrow(c->GetName()).c_str(),
                narrow(object_property(c, STR("AttachParent"))->GetName()).c_str(), w.x, w.y, w.z);
        }
    }

    // The 8 corners of the box and its 12 triangles, built into a UStaticMeshDescription, then into
    // a new UStaticMesh with box collision of its bounds.
    UObject* build_box_mesh(UObject* outer, V3 lo, V3 hi)
    {
        UObject* mesh_cdo = find(STR("/Script/Engine.Default__StaticMesh"));
        UObject* statics = find(STR("/Script/Engine.Default__GameplayStatics"));
        UObject* mesh_class = find(STR("/Script/Engine.StaticMesh"));
        if (!mesh_cdo || !statics || !mesh_class) return nullptr;

        Call make_desc(mesh_cdo, STR("CreateStaticMeshDescription"));
        make_desc.object(STR("Outer"), outer);
        if (!make_desc.run()) return nullptr;
        UObject* desc = make_desc.ret_object();
        log("  mesh description: %s", name_of(desc).c_str());
        if (!desc) return nullptr;

        Call group(desc, STR("CreatePolygonGroup"));
        if (!group.run()) return nullptr;
        int32_t group_id = group.ret_i32();

        int32_t verts[8];
        for (int i = 0; i < 8; ++i)
        {
            Call cv(desc, STR("CreateVertex"));
            if (!cv.run()) return nullptr;
            verts[i] = cv.ret_i32();
            Call sp(desc, STR("SetVertexPosition"));
            sp.i32(STR("VertexID"), verts[i]).three(STR("Position"), (i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z);
            if (!sp.run()) return nullptr;
        }
        // Corner index bits: 1 = +X, 2 = +Y, 4 = +Z. Each face is listed counter-clockwise from
        // outside, then reversed for Unreal's winding (only matters for rendering; the box is hidden).
        const int faces[6][4] = {{1, 3, 7, 5}, {0, 4, 6, 2}, {2, 6, 7, 3}, {0, 1, 5, 4}, {4, 5, 7, 6}, {0, 2, 3, 1}};
        int triangles = 0;
        for (auto& q : faces)
        {
            const int tri[2][3] = {{q[0], q[2], q[1]}, {q[0], q[3], q[2]}};
            for (auto& t : tri)
            {
                int32_t instances[3];
                for (int k = 0; k < 3; ++k)
                {
                    Call ci(desc, STR("CreateVertexInstance"));
                    ci.i32(STR("VertexID"), verts[t[k]]);
                    if (!ci.run()) return nullptr;
                    instances[k] = ci.ret_i32();
                }
                // NewEdgeIDs is an out array the engine fills with its own allocator; the few bytes
                // it allocates for this one-off test are left alone (not freed).
                Call ct(desc, STR("CreateTriangle"));
                ct.i32(STR("PolygonGroupID"), group_id).array(STR("VertexInstanceIDs"), instances, 3);
                if (!ct.run()) return nullptr;
                ++triangles;
            }
        }
        log("  box description: polygon group %d, %d triangles", group_id, triangles);

        Call spawn(statics, STR("SpawnObject"));
        spawn.object(STR("ObjectClass"), mesh_class).object(STR("Outer"), outer);
        if (!spawn.run()) return nullptr;
        UObject* mesh = spawn.ret_object();
        log("  new static mesh: %s", name_of(mesh).c_str());
        if (!mesh) return nullptr;

        UObject* list[1] = {desc};
        Call build(mesh, STR("BuildFromStaticMeshDescriptions"));
        build.array(STR("StaticMeshDescriptions"), list, 1).flag(STR("bBuildSimpleCollision"), true).flag(STR("bFastBuild"), true);
        if (!build.run()) return nullptr;
        V3 blo{}, bhi{};
        if (mesh_box(mesh, blo, bhi))
            log("  built: bounds (%.1f %.1f %.1f) to (%.1f %.1f %.1f)", blo.x, blo.y, blo.z, bhi.x, bhi.y, bhi.z);
        return mesh;
    }

    // A new ProceduralMeshComponent on the weapon, not attached, identity transform, no collision.
    UObject* add_proc(UObject* weapon)
    {
        UObject* proc_class = find(STR("/Script/ProceduralMeshComponent.ProceduralMeshComponent"));
        if (!proc_class) return nullptr;

        Call add(weapon, STR("AddComponentByClass"));
        add.object(STR("Class"), proc_class).flag(STR("bManualAttachment"), true).flag(STR("bDeferredFinish"), false);
        // RelativeTransform = identity: rotation W = 1, scale 1 (everything else zero).
        if (FProperty* t = add.param(STR("RelativeTransform")))
        {
            UScriptStruct* type = struct_type(t);
            uint8_t* base = add.buffer.data() + t->GetOffset_Internal();
            FProperty* rot = member(type, STR("Rotation"));
            FProperty* scale = member(type, STR("Scale3D"));
            FProperty* w = rot ? member(struct_type(rot), STR("W")) : nullptr;
            if (!rot || !scale || !w)
            {
                log("  ! unexpected FTransform layout");
                return nullptr;
            }
            uint8_t* wa = base + rot->GetOffset_Internal() + w->GetOffset_Internal();
            if (w->GetClass().GetName() == STR("DoubleProperty")) *reinterpret_cast<double*>(wa) = 1.0;
            else *reinterpret_cast<float*>(wa) = 1.0f;
            write_three(base + scale->GetOffset_Internal(), scale, 1, 1, 1);
        }
        if (!add.run()) return nullptr;
        UObject* proc = add.ret_object();
        if (!proc) return nullptr;

        // No collision means no physics body (PrimitiveComponent ShouldCreatePhysicsState), so nothing
        // of this component ever welds into the weapon.
        Call nocol(proc, STR("SetCollisionEnabled"));
        nocol.byte(STR("NewType"), 0); // ECollisionEnabled::NoCollision
        nocol.run();
        return proc;
    }

    // The visible model: a ProceduralMeshComponent on the weapon, snapped onto the Head, no collision.
    UObject* build_visual(UObject* weapon, UObject* head, const Model& m, UObject* material)
    {
        UObject* proc = add_proc(weapon);
        log("  new procedural mesh component (visual): %s", name_of(proc).c_str());
        if (!proc) return nullptr;

        Call attach(proc, STR("K2_AttachToComponent"));
        attach.object(STR("Parent"), head)
                .byte(STR("LocationRule"), 2) // EAttachmentRule::SnapToTarget
                .byte(STR("RotationRule"), 2)
                .byte(STR("ScaleRule"), 2)
                .flag(STR("bWeldSimulatedBodies"), false);
        if (attach.run()) log("  attached to Head: %s", attach.ret_bool() ? "yes" : "no");

        Call section(proc, STR("CreateMeshSection_LinearColor"));
        section.i32(STR("SectionIndex"), 0)
                .array(STR("Vertices"), m.positions.data(), (int32_t)m.positions.size())
                .array(STR("Triangles"), m.triangles.data(), (int32_t)m.triangles.size())
                .array(STR("Normals"), m.normals.data(), (int32_t)m.normals.size())
                .array(STR("UV0"), m.uvs.data(), (int32_t)(m.uvs.size() / 2))
                .flag(STR("bCreateCollision"), false);

        // Tangents (FProcMeshTangent: TangentX + bFlipTangentY), laid out from the reflected struct.
        // Without them the engine gets zero tangents and normal-mapped materials shade wrongly.
        // Tangent = the blade axis (Z) made perpendicular to the normal, worked out in world terms
        // (normals scale by 1/s, directions by s) and handed over in the component's local units.
        std::vector<uint8_t> tangents;
        auto* tstruct = UObjectGlobals::StaticFindObject<UScriptStruct*>(nullptr, nullptr, STR("/Script/ProceduralMeshComponent.ProcMeshTangent"));
        FProperty* tx = member(tstruct, STR("TangentX"));
        FProperty* tflip = member(tstruct, STR("bFlipTangentY"));
        if (tx && tflip)
        {
            int32_t stride = (tstruct->GetPropertiesSize() + 7) & ~7;
            const V3 s = m.tangent_scale;
            tangents.assign((size_t)stride * m.normals.size(), 0);
            for (size_t i = 0; i < m.normals.size(); ++i)
            {
                V3 n = unit(V3{m.normals[i].x / s.x, m.normals[i].y / s.y, m.normals[i].z / s.z});
                V3 a{0, 0, 1};
                if (std::fabs(n.z) > 0.95) a = {1, 0, 0};
                double d = n.x * a.x + n.y * a.y + n.z * a.z;
                V3 t = unit(V3{a.x - n.x * d, a.y - n.y * d, a.z - n.z * d});
                t = unit(V3{t.x / s.x, t.y / s.y, t.z / s.z});
                write_three(tangents.data() + i * stride + tx->GetOffset_Internal(), tx, t.x, t.y, t.z);
            }
            section.array(STR("Tangents"), tangents.data(), (int32_t)m.normals.size());
            log("  tangents: %zu (stride %d)", m.normals.size(), stride);
        }
        if (!section.run()) return nullptr;
        Call sections(proc, STR("GetNumSections"));
        if (sections.run()) log("  procedural mesh sections: %d", sections.ret_i32());

        if (material)
        {
            Call mat(proc, STR("SetMaterial"));
            mat.i32(STR("ElementIndex"), 0).object(STR("Material"), material);
            mat.run();
        }
        return proc;
    }

    void apply_test_blade(UObject* weapon);
    void apply_full_weapon(UObject* weapon, const std::wstring& base_name);

    void apply()
    {
        log("---- F9: make the held weapon the custom model ----");
        load_settings();

        // Our arrays are handed to the engine as FVector / FVector2D arrays: check they are doubles.
        int32_t vec = struct_size(STR("/Script/CoreUObject.Vector"));
        int32_t vec2 = struct_size(STR("/Script/CoreUObject.Vector2D"));
        log("Engine FVector %d bytes, FVector2D %d bytes", vec, vec2);
        if (vec != 24 || vec2 != 16)
        {
            log("  ! unexpected vector sizes, stopping");
            return;
        }

        UObject* pawn = player_pawn();
        UObject* weapon = object_property(pawn, STR("Weapon R"));
        log("Player %s, right-hand weapon %s", name_of(pawn).c_str(), name_of(weapon).c_str());
        if (!weapon)
        {
            log("  ! hold a weapon in the right hand first");
            return;
        }
        // A model with a collision file next to it (from tools/bs_weapon_export.py) replaces the
        // whole weapon; a bare OBJ only replaces the head (the first test).
        std::wstring base_name = g_settings.model;
        if (base_name.size() > 4 && _wcsicmp(base_name.c_str() + base_name.size() - 4, L".obj") == 0)
            base_name = base_name.substr(0, base_name.size() - 4);
        if (FILE* probe = _wfopen((g_dir + base_name + L"_collision.obj").c_str(), L"r"))
        {
            fclose(probe);
            apply_full_weapon(weapon, base_name);
        }
        else
        {
            apply_test_blade(weapon);
        }
    }

    void apply_test_blade(UObject* weapon)
    {
        UObject* head = object_property(weapon, STR("Head"));
        UObject* base = object_property(weapon, STR("BaseMesh"));
        UObject* old_mesh = object_property(head, STR("StaticMesh"));
        log("Head %s, mesh %s", name_of(head).c_str(), name_of(old_mesh).c_str());
        if (!head || !base) return;
        if (!old_mesh)
        {
            log("  ! this weapon has no head part (or it was already replaced)");
            return;
        }

        float head_mass = mass_of(head), base_mass = mass_of(base);
        log("Before: Head mass %.3f kg, BaseMesh (whole welded weapon) mass %.3f kg", head_mass, base_mass);
        V3 olo{}, ohi{};
        if (mesh_box(old_mesh, olo, ohi))
            log("Original head mesh bounds (%.1f %.1f %.1f) to (%.1f %.1f %.1f) cm", olo.x, olo.y, olo.z, ohi.x, ohi.y, ohi.z);
        log_tips(weapon);

        Model model;
        if (!load_obj(g_dir + g_settings.model, model)) return;
        if (g_settings.fit_to_original && model.max.z > 1.0 && ohi.z > 1.0)
        {
            double s = ohi.z / model.max.z;
            scale_model(model, s);
            log("Scaled model by %.3f so its end matches the original head (%.1f cm)", s, ohi.z);
        }

        Call get_mat(head, STR("GetMaterial"));
        get_mat.i32(STR("ElementIndex"), 0);
        UObject* material = get_mat.run() ? get_mat.ret_object() : nullptr;
        log("Head material %s", name_of(material).c_str());

        UObject* box = build_box_mesh(weapon, model.min, model.max);
        if (!box)
        {
            log("  ! runtime static mesh failed, stopping");
            return;
        }
        Call set(head, STR("SetStaticMesh"));
        set.object(STR("NewMesh"), box);
        if (set.run()) log("SetStaticMesh on Head: %s", set.ret_bool() ? "changed" : "NOT changed");
        if (g_settings.hide_original)
        {
            Call hide(head, STR("SetVisibility"));
            hide.flag(STR("bNewVisibility"), false).flag(STR("bPropagateToChildren"), false);
            hide.run();
        }
        if (g_settings.mass_kg > 0)
        {
            Call mass(head, STR("SetMassOverrideInKg"));
            mass.f32(STR("MassInKg"), (float)g_settings.mass_kg).flag(STR("bOverrideMass"), true);
            mass.run();
        }
        log("After: Head mass %.3f kg, BaseMesh (whole welded weapon) mass %.3f kg", mass_of(head), mass_of(base));

        UObject* proc = build_visual(weapon, head, model, material);
        log(proc ? "Done. Swing it at something and check the hits." : "  ! visual model failed (collision swap above still applies)");
    }

    // ---- whole-weapon replacement (models from tools/bs_weapon_export.py) -----------------------
    std::map<std::string, double> read_numbers(const std::wstring& path)
    {
        std::map<std::string, double> out;
        FILE* f = _wfopen(path.c_str(), L"r");
        if (!f) return out;
        char line[256];
        while (fgets(line, sizeof line, f))
        {
            if (line[0] == ';' || line[0] == '#') continue;
            char* eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            out[line] = atof(eq + 1);
        }
        fclose(f);
        return out;
    }

    struct Hull
    {
        std::string slot;
        std::vector<V3> points;
        std::vector<int> faces; // triangles, indices into points
        double volume = 0;      // cm^3
    };

    std::vector<Hull> load_hulls(const std::wstring& path)
    {
        std::vector<Hull> hulls;
        FILE* f = _wfopen(path.c_str(), L"r");
        if (!f) return hulls;
        char line[512];
        int base = 1; // OBJ index of the current hull's first vertex
        int total = 0;
        while (fgets(line, sizeof line, f))
        {
            if (line[0] == 'g' && line[1] == ' ')
            {
                // UCX_<slot>_<n>
                std::string g(line + 2);
                while (!g.empty() && (g.back() == '\n' || g.back() == '\r' || g.back() == ' ')) g.pop_back();
                Hull h;
                size_t a = g.find('_'), b = g.rfind('_');
                h.slot = (a != std::string::npos && b > a) ? g.substr(a + 1, b - a - 1) : g;
                hulls.push_back(h);
                base = total + 1;
            }
            else if (line[0] == 'v' && line[1] == ' ' && !hulls.empty())
            {
                V3 p{};
                sscanf(line + 2, "%lf %lf %lf", &p.x, &p.y, &p.z);
                hulls.back().points.push_back(p);
                ++total;
            }
            else if (line[0] == 'f' && line[1] == ' ' && !hulls.empty())
            {
                int i, j, k;
                if (sscanf(line + 2, "%d %d %d", &i, &j, &k) == 3)
                {
                    hulls.back().faces.push_back(i - base);
                    hulls.back().faces.push_back(j - base);
                    hulls.back().faces.push_back(k - base);
                }
            }
        }
        fclose(f);
        // Volume of each closed hull (faces wound outward): sum of signed tetrahedra.
        for (Hull& h : hulls)
        {
            double v = 0;
            for (size_t t = 0; t + 2 < h.faces.size(); t += 3)
            {
                const V3 &a = h.points[h.faces[t]], &b = h.points[h.faces[t + 1]], &c = h.points[h.faces[t + 2]];
                v += (a.x * (b.y * c.z - b.z * c.y) - a.y * (b.x * c.z - b.z * c.x) + a.z * (b.x * c.y - b.y * c.x)) / 6.0;
            }
            h.volume = std::fabs(v);
        }
        return hulls;
    }

    // A component's world transform, kept as the engine's own bytes so it can be handed back to
    // KismetMathLibrary's TransformLocation / InverseTransformLocation.
    struct Xf
    {
        std::vector<uint8_t> bytes;
    };

    Xf world_transform(UObject* component)
    {
        Xf x;
        Call c(component, STR("K2_GetComponentToWorld"));
        if (!c.run()) return x;
        if (FProperty* p = c.param(STR("ReturnValue")))
        {
            uint8_t* at = c.buffer.data() + p->GetOffset_Internal();
            x.bytes.assign(at, at + p->GetSize());
        }
        return x;
    }

    V3 transform_point(const Xf& x, V3 p, bool inverse)
    {
        static UObject* math = find(STR("/Script/Engine.Default__KismetMathLibrary"));
        Call c(math, inverse ? STR("InverseTransformLocation") : STR("TransformLocation"));
        FProperty* t = c.param(STR("T"));
        if (!t || x.bytes.size() != (size_t)t->GetSize()) return p;
        memcpy(c.buffer.data() + t->GetOffset_Internal(), x.bytes.data(), x.bytes.size());
        c.three(STR("Location"), p.x, p.y, p.z);
        if (!c.run()) return p;
        FProperty* r = c.param(STR("ReturnValue"));
        return r ? read_three(c.buffer.data() + r->GetOffset_Internal(), r) : p;
    }

    bool set_object_property(UObject* owner, const TCHAR* name, UObject* value)
    {
        if (!owner) return false;
        UObject** slot = owner->GetValuePtrByPropertyNameInChain<UObject*>(name);
        if (!slot) return false;
        *slot = value;
        return true;
    }

    bool set_bool_property(UObject* owner, const TCHAR* name, bool value)
    {
        FProperty* p = owner ? owner->GetPropertyByNameInChain(name) : nullptr;
        if (!p) return false;
        auto* b = static_cast<FBoolProperty*>(p);
        uint8_t* at = reinterpret_cast<uint8_t*>(owner) + p->GetOffset_Internal() + b->GetByteOffset();
        if (value) *at |= b->GetFieldMask();
        else *at &= ~b->GetFieldMask();
        return true;
    }

    // The component's own mass setting (BodyInstance.MassInKgOverride), read or written straight
    // in the component: SetMassOverrideInKg on a welded part would change the weapon's root body
    // instead (PrimitiveComponentPhysics.cpp: GetBodyInstance returns the weld parent).
    // bOverrideMass is not a reflected property in this game, so only the number is written; the game
    // already turned the override on for its parts (Set Up Module calls SetMassOverrideInKg).
    bool body_mass(UObject* component, bool write, float& kg)
    {
        FProperty* bi = component ? component->GetPropertyByNameInChain(STR("BodyInstance")) : nullptr;
        FProperty* mass = member(struct_type(bi), STR("MassInKgOverride"));
        if (!mass) return false;
        float* ma = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(component) + bi->GetOffset_Internal() + mass->GetOffset_Internal());
        if (write) *ma = kg;
        else kg = *ma;
        return true;
    }

    // Convex collision for one weapon part: a hidden ProceduralMeshComponent cooks the convex pieces
    // at runtime (ProceduralMeshComponent.cpp UpdateCollision -> BodySetup CreatePhysicsMeshes ->
    // RuntimeCookPhysics_Chaos), then that BodySetup is given to a runtime UStaticMesh, which goes
    // into the part's slot. The slot keeps the game's hit binding and damage points.
    UObject* build_part_mesh(UObject* weapon, UObject* slot, const std::vector<std::vector<V3>>& pieces)
    {
        V3 lo{1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30};
        for (auto& piece : pieces)
            for (const V3& p : piece)
            {
                lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
                hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
            }
        UObject* mesh = build_box_mesh(weapon, lo, hi);
        if (!mesh) return nullptr;

        UObject* cooker = add_proc(weapon);
        if (!cooker) return nullptr;
        // Complex-as-simple would make the body a triangle mesh, which cannot simulate.
        if (!set_bool_property(cooker, STR("bUseComplexAsSimpleCollision"), false))
            log("  ! could not turn off bUseComplexAsSimpleCollision");
        set_bool_property(cooker, STR("bUseAsyncCooking"), false);
        for (auto& piece : pieces)
        {
            Call add(cooker, STR("AddCollisionConvexMesh"));
            add.array(STR("ConvexVerts"), piece.data(), (int32_t)piece.size());
            add.run();
        }
        UObject* body = object_property(cooker, STR("ProcMeshBodySetup"));
        log("  %s: %zu convex pieces cooked into %s", narrow(slot->GetName()).c_str(), pieces.size(), name_of(body).c_str());
        if (!body) return nullptr;

        // Keep the old part's physical material (surface type for hit sounds and effects).
        UObject* old_mesh = object_property(slot, STR("StaticMesh"));
        UObject* old_body = object_property(old_mesh, STR("BodySetup"));
        UObject* phys = object_property(old_body, STR("PhysMaterial"));
        if (phys) set_object_property(body, STR("PhysMaterial"), phys);
        log("  %s: physical material %s", narrow(slot->GetName()).c_str(), name_of(phys).c_str());

        if (!set_object_property(mesh, STR("BodySetup"), body))
        {
            log("  ! could not set the new mesh's BodySetup");
            return nullptr;
        }
        return mesh;
    }

    void hide(UObject* component)
    {
        if (!component) return;
        Call c(component, STR("SetVisibility"));
        c.flag(STR("bNewVisibility"), false).flag(STR("bPropagateToChildren"), false);
        c.run();
    }

    void apply_full_weapon(UObject* weapon, const std::wstring& base_name)
    {
        auto numbers = read_numbers(g_dir + base_name + L".txt");
        auto hulls = load_hulls(g_dir + base_name + L"_collision.obj");
        Model model;
        if (!load_obj(g_dir + base_name + L".obj", model) || hulls.empty() || numbers.empty())
        {
            log("  ! %s: need %s.obj, %s_collision.obj and %s.txt", narrow(base_name).c_str(), narrow(base_name).c_str(),
                narrow(base_name).c_str(), narrow(base_name).c_str());
            return;
        }
        double guard_top = numbers["guard_top"];
        double tip = numbers["tip"];
        double mass_total = g_settings.mass_kg > 0 ? g_settings.mass_kg : numbers["mass_kg"];
        log("Model %s: %zu collision pieces, guard top %.1f cm, tip %.1f cm, mass %.2f kg", narrow(base_name).c_str(),
            hulls.size(), guard_top, tip, mass_total);

        // The parts, as the forge builds them (ModularWeaponBP slots).
        const TCHAR* slot_names[] = {STR("Head"), STR("Guard"), STR("Grip"), STR("Pommel")};
        UObject* slots[4];
        UObject* base = object_property(weapon, STR("BaseMesh"));
        for (int i = 0; i < 4; ++i)
        {
            slots[i] = object_property(weapon, slot_names[i]);
            float kg = 0;
            body_mass(slots[i], false, kg);
            V3 lo{}, hi{};
            mesh_box(object_property(slots[i], STR("StaticMesh")), lo, hi);
            log("  slot %s: mesh %s, own mass setting %.3f kg, mesh z %.1f to %.1f", narrow(slot_names[i]).c_str(),
                name_of(object_property(slots[i], STR("StaticMesh"))).c_str(), kg, lo.z, hi.z);
        }
        log("  whole weapon %.3f kg", mass_of(base));
        for (const TCHAR* extra : {STR("Sub Module 1 Mesh"), STR("SubModule 2 Mesh")})
            if (UObject* c = object_property(weapon, extra))
                if (UObject* m = object_property(c, STR("StaticMesh")))
                    log("  also on this weapon: %s, mesh %s (left as it is)", narrow(extra).c_str(), name_of(m).c_str());
        UObject* head = slots[0];
        UObject* old_head_mesh = object_property(head, STR("StaticMesh"));
        if (!head || !old_head_mesh)
        {
            log("  ! this weapon has no head part (or it was already replaced)");
            return;
        }
        V3 olo{}, ohi{};
        mesh_box(old_head_mesh, olo, ohi);
        log_tips(weapon);

        // The head's scale (the forge's size sliders) must not stretch the model: model points are in
        // true centimetres in the head's frame ("u"), and are divided by the scale for the head itself.
        V3 hs{1, 1, 1};
        {
            Call sc(head, STR("K2_GetComponentScale"));
            FProperty* r = sc.run() ? sc.param(STR("ReturnValue")) : nullptr;
            if (r) hs = read_three(sc.buffer.data() + r->GetOffset_Internal(), r);
            log("  head scale (%.3f %.3f %.3f)", hs.x, hs.y, hs.z);
        }
        Xf head_xf = world_transform(head);
        if (head_xf.bytes.empty() || hs.x == 0 || hs.y == 0 || hs.z == 0)
        {
            log("  ! could not read the head's transform");
            return;
        }
        auto to_head_local = [&](V3 u) { return V3{u.x / hs.x, u.y / hs.y, u.z / hs.z}; };

        // Where the game's own guard starts, along the head's axis: the model's guard goes there, so
        // the hands (placed by the game's grip) stay below it.
        double shift = -guard_top;
        if (UObject* guard = slots[1])
        {
            V3 glo{}, ghi{};
            if (mesh_box(object_property(guard, STR("StaticMesh")), glo, ghi))
            {
                V3 w = transform_point(world_transform(guard), V3{0, 0, glo.z}, false);
                V3 h = transform_point(head_xf, w, true);
                double hs_guard_bottom = h.z * hs.z;
                shift = hs_guard_bottom - numbers["guard_bottom"];
                log("  game guard starts at z %.1f cm on the head axis; model guard %.1f..%.1f cm -> shift %.1f cm", hs_guard_bottom,
                    numbers["guard_bottom"], guard_top, shift);
            }
        }

        Call get_mat(head, STR("GetMaterial"));
        get_mat.i32(STR("ElementIndex"), 0);
        UObject* material = get_mat.run() ? get_mat.ret_object() : nullptr;

        // Mass: shared between the parts by the volume of their pieces (one material throughout).
        double volume_all = 0, volume[4] = {0, 0, 0, 0};
        for (Hull& h : hulls)
        {
            volume_all += h.volume;
            for (int i = 0; i < 4; ++i)
                if (narrow(slot_names[i]) == h.slot) volume[i] += h.volume;
        }

        for (int i = 0; i < 4; ++i)
        {
            if (!slots[i]) continue;
            Xf slot_xf = i == 0 ? head_xf : world_transform(slots[i]);
            std::vector<std::vector<V3>> pieces;
            for (Hull& h : hulls)
            {
                if (narrow(slot_names[i]) != h.slot) continue;
                std::vector<V3> local;
                for (V3 p : h.points)
                {
                    p.z += shift;
                    V3 hl = to_head_local(p);
                    local.push_back(i == 0 ? hl : transform_point(slot_xf, transform_point(head_xf, hl, false), true));
                }
                pieces.push_back(local);
            }
            if (pieces.empty())
            {
                log("  %s: no pieces in the model; left as it is but hidden", narrow(slot_names[i]).c_str());
                hide(slots[i]);
                continue;
            }
            UObject* mesh = build_part_mesh(weapon, slots[i], pieces);
            if (!mesh) continue;
            float kg = volume_all > 0 ? (float)(mass_total * volume[i] / volume_all) : 0.0f;
            body_mass(slots[i], true, kg);
            Call set(slots[i], STR("SetStaticMesh"));
            set.object(STR("NewMesh"), mesh);
            set.run();
            hide(slots[i]);
            log("  %s: new collision in place (%s), mass setting %.3f kg", narrow(slot_names[i]).c_str(),
                set.ret_bool() ? "changed" : "NOT changed", kg);
        }
        log("Whole weapon mass now %.3f kg (target %.3f kg; BaseMesh's own share is not changed)", mass_of(base), mass_total);

        // Damage points on the head move with the blade: same layout, stretched to the new length.
        double old_len = ohi.z, new_len = (tip + shift) / hs.z; // both in the head's unscaled local units
        std::vector<UObject*> comps;
        UObjectGlobals::FindAllOf(STR("SceneComponent"), comps);
        for (UObject* c : comps)
        {
            if (object_property(c, STR("AttachParent")) != head) continue;
            if (c->GetName().find(STR("NODE_AddSceneComponent")) == std::wstring::npos) continue;
            FProperty* rp = c->GetPropertyByNameInChain(STR("RelativeLocation"));
            if (!rp || old_len < 1.0) continue;
            V3 r = read_three(reinterpret_cast<uint8_t*>(c) + rp->GetOffset_Internal(), rp);
            Call move(c, STR("K2_SetRelativeLocation"));
            move.three(STR("NewLocation"), r.x, r.y, r.z * new_len / old_len).flag(STR("bTeleport"), true);
            move.run();
            log("  head damage point %s: z %.1f -> %.1f (head units)", narrow(c->GetName()).c_str(), r.z, r.z * new_len / old_len);
        }

        // The visible model in the head's local units; normals and tangents adjusted so that, after the
        // engine applies the head's scale, they are right in the world.
        for (V3& p : model.positions)
        {
            p.z += shift;
            p = to_head_local(p);
        }
        for (V3& n : model.normals) n = unit(V3{n.x * hs.x, n.y * hs.y, n.z * hs.z});
        model.tangent_scale = hs;
        UObject* proc = build_visual(weapon, head, model, material);
        log(proc ? "Done. Swing it at something and check the hits." : "  ! visual model failed (collision swap above still applies)");
    }

    bool g_key_was_down = false;

    void tick(UObject* engine)
    {
        g_engine = engine;
        bool down = (GetAsyncKeyState(g_settings.key) & 0x8000) != 0;
        if (down && !g_key_was_down) apply();
        g_key_was_down = down;
    }
} // namespace

class HalfSwordCustomWeapon : public CppUserModBase
{
  public:
    HalfSwordCustomWeapon() : CppUserModBase()
    {
        ModName = STR("HalfSwordCustomWeapon");
        ModVersion = STR("0.1.0");
        ModDescription = STR("Test: an uncooked weapon part built at runtime from an OBJ file");
        ModAuthors = STR("Half-Sword-VR contributors");
        find_mod_dir();
        log("HalfSwordCustomWeapon 0.1.0 loaded from %s", narrow(g_dir).c_str());
        load_settings();
    }

    ~HalfSwordCustomWeapon() override = default;

    auto on_unreal_init() -> void override
    {
        Hook::RegisterEngineTickPreCallback(
                [](auto&, UEngine* engine, float, bool) { tick(reinterpret_cast<UObject*>(engine)); },
                {false, false, STR("HalfSwordCustomWeapon"), STR("Tick")});
        log("Ready: hold a weapon in the right hand and press key 0x%X", g_settings.key);
    }
};

#define CUSTOMWEAPON_API __declspec(dllexport)
extern "C"
{
    CUSTOMWEAPON_API RC::CppUserModBase* start_mod()
    {
        return new HalfSwordCustomWeapon();
    }

    CUSTOMWEAPON_API void uninstall_mod(RC::CppUserModBase* mod)
    {
        delete mod;
    }
}
