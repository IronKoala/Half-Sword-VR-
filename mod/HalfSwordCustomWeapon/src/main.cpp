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

    FProperty* member(UScriptStruct* type, const TCHAR* name)
    {
        if (!type) return nullptr;
        for (FProperty* p : type->ForEachProperty())
            if (p->GetName() == name) return p;
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
                if (p->GetName() == name) return p;
            log("  ! %s has no parameter %s", label.c_str(), narrow(name).c_str());
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
        if (!ret || !mn || !mx) return false;
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

    // The visible model: a ProceduralMeshComponent on the weapon, snapped onto the Head, no collision.
    UObject* build_visual(UObject* weapon, UObject* head, const Model& m, UObject* material)
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
        log("  new procedural mesh component: %s", name_of(proc).c_str());
        if (!proc) return nullptr;

        Call nocol(proc, STR("SetCollisionEnabled"));
        nocol.byte(STR("NewType"), 0); // ECollisionEnabled::NoCollision
        nocol.run();

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

    void apply()
    {
        log("---- F9: make the held weapon's head the custom model ----");
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
