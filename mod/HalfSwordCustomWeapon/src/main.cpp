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
        // Material texture parameters that get the model's own maps (<model>_BaseColor.png,
        // _Normal.png, _ORM.png next to it). Empty = leave that parameter alone.
        // Defaults: the game's plain (not layered) lit material M_ORM_Base_Material through one of its
        // cooked instances (a metal pot), whose texture parameters are D (colour), N (normal) and
        // ORM (R occlusion, G roughness, B metallic), the same packing the exporter writes.
        std::wstring own_material = L"/Game/Assets/Props/Cooking_Utencils/Materials/MI_Prop_Cooking_Utencil_Pot_002.MI_Prop_Cooking_Utencil_Pot_002";
        std::wstring tex_base = L"D", tex_normal = L"N", tex_orm = L"ORM";
        int own_textures = 1;     // 0: keep the game's material untouched
        // Hands on the model's handle. fit_hands 0 leaves the game weapon's grip as it is.
        int fit_hands = 1;
        double two_hand_grip_cm = 14.0; // handle at least this long -> two-handed sword grip (game: 12 cm grips
                                        // are one-handed, type 2; 15 and 20 cm grips two-handed, type 11)
        // Long handles: the main hand rests by the guard and slides down to the off hand during a swing
        // (Matthew 2026-11-21). Swing = weapon turning faster than swing_slide_start_dps.
        int swing_slide = 1;
        double swing_slide_start_dps = 400.0, swing_slide_end_dps = 150.0, swing_slide_hold_s = 0.25;
        double swing_slide_down_s = 0.12, swing_slide_up_s = 0.4;
        // Only slide for a far target (Matthew 2026-11-22): with the game's Camera Lock On on a target, a swing
        // slides only when the target's head is farther from the weapon's grip than this (cm). 0 = automatic:
        // main hand to tip + swing_slide_arm_cm. swing_slide_no_target: 1 = slide when nothing is locked on.
        double swing_slide_min_target_cm = 0.0, swing_slide_arm_cm = 40.0;
        int swing_slide_no_target = 1;
        std::wstring offhand = L"pommel"; // pommel: off hand at the handle's end (Matthew 2026-11-19); next: beside the main hand (game spacing)
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
            else if (k == "tex_base") g_settings.tex_base = std::wstring(v.begin(), v.end());
            else if (k == "tex_normal") g_settings.tex_normal = std::wstring(v.begin(), v.end());
            else if (k == "tex_orm") g_settings.tex_orm = std::wstring(v.begin(), v.end());
            else if (k == "own_material") g_settings.own_material = std::wstring(v.begin(), v.end());
            else if (k == "own_textures") g_settings.own_textures = atoi(v.c_str());
            else if (k == "fit_hands") g_settings.fit_hands = atoi(v.c_str());
            else if (k == "two_hand_grip_cm") g_settings.two_hand_grip_cm = atof(v.c_str());
            else if (k == "swing_slide") g_settings.swing_slide = atoi(v.c_str());
            else if (k == "swing_slide_start_dps") g_settings.swing_slide_start_dps = atof(v.c_str());
            else if (k == "swing_slide_end_dps") g_settings.swing_slide_end_dps = atof(v.c_str());
            else if (k == "swing_slide_hold_s") g_settings.swing_slide_hold_s = atof(v.c_str());
            else if (k == "swing_slide_down_s") g_settings.swing_slide_down_s = atof(v.c_str());
            else if (k == "swing_slide_up_s") g_settings.swing_slide_up_s = atof(v.c_str());
            else if (k == "swing_slide_min_target_cm") g_settings.swing_slide_min_target_cm = atof(v.c_str());
            else if (k == "swing_slide_arm_cm") g_settings.swing_slide_arm_cm = atof(v.c_str());
            else if (k == "swing_slide_no_target") g_settings.swing_slide_no_target = atoi(v.c_str());
            else if (k == "offhand") g_settings.offhand = std::wstring(v.begin(), v.end());
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
        // Per-tick calls: the function looked up once by the caller (name lookups are slow).
        Call(UObject* object, UFunction* function, const char* what) : target(object), fn(function), label(what)
        {
            ok = object && function;
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

        // Tangents from the UVs. Small models: the engine's own routine (KismetProceduralMeshLibrary
        // CalculateTangentsForMesh), whose output arrays are engine-allocated and left alone (a one-off
        // leak per F9 press). It compares every vertex with every other one (it took 15 s on a model
        // of 177k vertices), so big models get the same per-triangle construction done here instead:
        // TangentX = direction of increasing U, made perpendicular to the normal; bFlipTangentY set
        // when the direction of increasing V points against cross(normal, TangentX), the basis the
        // engine's vertex shader rebuilds (TangentY = cross(Z, X) * sign).
        RawArray tangents{};
        std::vector<uint8_t> own_tangents;
        if (m.positions.size() <= 30000)
        {
            static UObject* pml = find(STR("/Script/ProceduralMeshComponent.Default__KismetProceduralMeshLibrary"));
            Call calc(pml, STR("CalculateTangentsForMesh"));
            calc.array(STR("Vertices"), m.positions.data(), (int32_t)m.positions.size())
                    .array(STR("Triangles"), m.triangles.data(), (int32_t)m.triangles.size())
                    .array(STR("UVs"), m.uvs.data(), (int32_t)(m.uvs.size() / 2));
            if (calc.run())
                if (uint8_t* t = calc.at(STR("Tangents"))) tangents = *reinterpret_cast<RawArray*>(t);
            log("  tangents from UVs (engine): %d (vertices %zu)", tangents.Num, m.positions.size());
        }
        else
        {
            auto* type = UObjectGlobals::StaticFindObject<UScriptStruct*>(nullptr, nullptr, STR("/Script/ProceduralMeshComponent.ProcMeshTangent"));
            FProperty* tx = member(type, STR("TangentX"));
            FProperty* flip = member(type, STR("bFlipTangentY"));
            if (tx && flip)
            {
                size_t stride = ((size_t)type->GetPropertiesSize() + 7) & ~(size_t)7;
                own_tangents.assign(stride * m.positions.size(), 0);
                auto* fb = static_cast<FBoolProperty*>(flip);
                auto sub = [](V3 a, V3 b) { return V3{a.x - b.x, a.y - b.y, a.z - b.z}; };
                auto dot = [](V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
                auto cross = [](V3 a, V3 b) { return V3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; };
                for (size_t k = 0; k + 2 < m.triangles.size(); k += 3)
                {
                    int i0 = m.triangles[k], i1 = m.triangles[k + 1], i2 = m.triangles[k + 2];
                    V3 e1 = sub(m.positions[i1], m.positions[i0]), e2 = sub(m.positions[i2], m.positions[i0]);
                    double du1 = m.uvs[2 * i1] - m.uvs[2 * i0], dv1 = m.uvs[2 * i1 + 1] - m.uvs[2 * i0 + 1];
                    double du2 = m.uvs[2 * i2] - m.uvs[2 * i0], dv2 = m.uvs[2 * i2 + 1] - m.uvs[2 * i0 + 1];
                    double r = du1 * dv2 - du2 * dv1;
                    if (std::fabs(r) < 1e-12) r = 1e-12;
                    V3 tu{(e1.x * dv2 - e2.x * dv1) / r, (e1.y * dv2 - e2.y * dv1) / r, (e1.z * dv2 - e2.z * dv1) / r};
                    V3 tv{(e2.x * du1 - e1.x * du2) / r, (e2.y * du1 - e1.y * du2) / r, (e2.z * du1 - e1.z * du2) / r};
                    for (int c : {i0, i1, i2})
                    {
                        V3 n = m.normals[c];
                        double d = dot(n, tu);
                        V3 t = unit(V3{tu.x - n.x * d, tu.y - n.y * d, tu.z - n.z * d});
                        uint8_t* at = own_tangents.data() + (size_t)c * stride;
                        write_three(at + tx->GetOffset_Internal(), tx, t.x, t.y, t.z);
                        if (dot(cross(n, t), tv) < 0) at[flip->GetOffset_Internal() + fb->GetByteOffset()] |= fb->GetFieldMask();
                    }
                }
                tangents = RawArray{own_tangents.data(), (int32_t)m.positions.size(), (int32_t)m.positions.size()};
                log("  tangents from UVs (own, per triangle): %zu", m.positions.size());
            }
        }
        if (tangents.Num == (int32_t)m.positions.size())
            section.array(STR("Tangents"), tangents.Data, tangents.Num);
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

    // ---- materials and textures ----------------------------------------------------------------
    // Logs the texture, scalar and vector parameters a material instance (and its parents) sets,
    // read from the reflected arrays TextureParameterValues etc., so parameter names come from the
    // game, not from a guess.
    void log_material_params(UObject* material)
    {
        struct Kind
        {
            const TCHAR* array;
            const TCHAR* type;
            const TCHAR* value;
        };
        const Kind kinds[] = {{STR("TextureParameterValues"), STR("/Script/Engine.TextureParameterValue"), STR("ParameterValue")},
                              {STR("ScalarParameterValues"), STR("/Script/Engine.ScalarParameterValue"), STR("ParameterValue")},
                              {STR("VectorParameterValues"), STR("/Script/Engine.VectorParameterValue"), STR("ParameterValue")}};
        for (int depth = 0; material && depth < 8; ++depth)
        {
            log("  material %s (%s)", name_of(material).c_str(), narrow(material->GetClassPrivate()->GetName()).c_str());
            for (const Kind& k : kinds)
            {
                auto* arr = material->GetValuePtrByPropertyNameInChain<RawArray>(k.array);
                auto* type = UObjectGlobals::StaticFindObject<UScriptStruct*>(nullptr, nullptr, k.type);
                if (!arr || !type || !arr->Data) continue;
                FProperty* info = member(type, STR("ParameterInfo"));
                FProperty* name = member(struct_type(info), STR("Name"));
                FProperty* value = member(type, k.value);
                if (!info || !name || !value) continue;
                int32_t stride = (type->GetPropertiesSize() + 7) & ~7;
                for (int32_t i = 0; i < arr->Num; ++i)
                {
                    const uint8_t* e = static_cast<const uint8_t*>(arr->Data) + (size_t)i * stride;
                    const FName* n = reinterpret_cast<const FName*>(e + info->GetOffset_Internal() + name->GetOffset_Internal());
                    const uint8_t* v = e + value->GetOffset_Internal();
                    std::string text;
                    if (k.array[0] == 'T') text = name_of(*reinterpret_cast<UObject* const*>(v));
                    else if (k.array[0] == 'S') text = std::to_string(*reinterpret_cast<const float*>(v));
                    else
                    {
                        const float* c = reinterpret_cast<const float*>(v);
                        char b[96];
                        snprintf(b, sizeof b, "(%.3f %.3f %.3f %.3f)", c[0], c[1], c[2], c[3]);
                        text = b;
                    }
                    log("    %s %s = %s", narrow(k.array).substr(0, 6).c_str(), narrow(n->ToString()).c_str(), text.c_str());
                }
            }
            material = object_property(material, STR("Parent"));
        }
    }

    // A PNG/JPG file loaded as a runtime texture (KismetRenderingLibrary ImportFileAsTexture2D).
    UObject* load_texture(UObject* world_context, const std::wstring& path)
    {
        if (FILE* f = _wfopen(path.c_str(), L"rb")) fclose(f);
        else return nullptr;
        static UObject* rl = find(STR("/Script/Engine.Default__KismetRenderingLibrary"));
        Call c(rl, STR("ImportFileAsTexture2D"));
        c.object(STR("WorldContextObject"), world_context);
        if (uint8_t* a = c.at(STR("Filename")))
            *reinterpret_cast<RawArray*>(a) = RawArray{path.c_str(), (int32_t)path.size() + 1, (int32_t)path.size() + 1};
        UObject* t = c.run() ? c.ret_object() : nullptr;
        log("  texture %s -> %s", narrow(path).c_str(), name_of(t).c_str());
        return t;
    }

    // An asset by path: found if already loaded, otherwise loaded (KismetSystemLibrary
    // MakeSoftObjectPath -> Conv_SoftObjPathToSoftObjRef -> LoadAsset_Blocking).
    UObject* load_asset(const std::wstring& path)
    {
        if (UObject* o = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, path.c_str())) return o;
        static UObject* ksl = find(STR("/Script/Engine.Default__KismetSystemLibrary"));
        Call make(ksl, STR("MakeSoftObjectPath"));
        if (uint8_t* a = make.at(STR("PathString")))
            *reinterpret_cast<RawArray*>(a) = RawArray{path.c_str(), (int32_t)path.size() + 1, (int32_t)path.size() + 1};
        if (!make.run()) return nullptr;
        Call conv(ksl, STR("Conv_SoftObjPathToSoftObjRef"));
        FProperty* made = make.param(STR("ReturnValue"));
        FProperty* in = conv.param(STR("SoftObjectPath"));
        if (!made || !in || made->GetSize() != in->GetSize()) return nullptr;
        memcpy(conv.buffer.data() + in->GetOffset_Internal(), make.buffer.data() + made->GetOffset_Internal(), made->GetSize());
        if (!conv.run()) return nullptr;
        Call load(ksl, STR("LoadAsset_Blocking"));
        FProperty* ref = conv.param(STR("ReturnValue"));
        FProperty* asset = load.param(STR("Asset"));
        if (!ref || !asset || ref->GetSize() != asset->GetSize()) return nullptr;
        memcpy(load.buffer.data() + asset->GetOffset_Internal(), conv.buffer.data() + ref->GetOffset_Internal(), ref->GetSize());
        UObject* o = load.run() ? load.ret_object() : nullptr;
        log("  loaded %s -> %s", narrow(path).c_str(), name_of(o).c_str());
        return o;
    }

    void set_param_name(Call& c, const TCHAR* name)
    {
        if (FProperty* p = c.param(STR("ParameterName"))) *reinterpret_cast<FName*>(c.buffer.data() + p->GetOffset_Internal()) = FName(name, FNAME_Add);
    }

    // Puts the model's own maps into a dynamic copy of a plain game material on the visual.
    void apply_own_textures(UObject* weapon, UObject* proc, UObject* fallback, const std::wstring& base_path)
    {
        if (!g_settings.own_textures) return;
        if (FILE* f = _wfopen((base_path + L"_BaseColor.png").c_str(), L"rb")) fclose(f);
        else
        {
            log("  no %s_BaseColor.png: keeping the game's material", narrow(base_path).c_str());
            return;
        }
        UObject* material = g_settings.own_material.empty() ? fallback : load_asset(g_settings.own_material);
        if (!material)
        {
            log("  ! material %s not found; keeping the game's material", narrow(g_settings.own_material).c_str());
            return;
        }
        log_material_params(material);
        Call mid_call(proc, STR("CreateDynamicMaterialInstance"));
        mid_call.i32(STR("ElementIndex"), 0).object(STR("SourceMaterial"), material);
        UObject* mid = mid_call.run() ? mid_call.ret_object() : nullptr;
        log("  dynamic material %s", name_of(mid).c_str());
        if (!mid) return;
        struct Map
        {
            const std::wstring& param;
            const wchar_t* file;
        };
        const Map maps[] = {{g_settings.tex_base, L"_BaseColor.png"}, {g_settings.tex_normal, L"_Normal.png"}, {g_settings.tex_orm, L"_ORM.png"}};
        for (const Map& m : maps)
        {
            if (m.param.empty()) continue;
            UObject* tex = load_texture(weapon, base_path + m.file);
            if (!tex) continue;
            Call set(mid, STR("SetTextureParameterValue"));
            set_param_name(set, m.param.c_str());
            set.object(STR("Value"), tex);
            set.run();
            Call get(mid, STR("K2_GetTextureParameterValue"));
            set_param_name(get, m.param.c_str());
            log("  %s <- %s (reads back %s)", narrow(m.param).c_str(), name_of(tex).c_str(), get.run() ? name_of(get.ret_object()).c_str() : "?");
        }
        // The pot's own tiling and tint would not fit this model's UVs and colours. (Intensity and
        // contrast values are left as the pot has them; they are logged above.)
        for (const TCHAR* tile : {STR("U Tile"), STR("V Tile")})
        {
            Call set(mid, STR("SetScalarParameterValue"));
            set_param_name(set, tile);
            set.f32(STR("Value"), 1.0f);
            set.run();
        }
        Call tint(mid, STR("SetVectorParameterValue"));
        set_param_name(tint, STR("Color Tint"));
        if (uint8_t* a = tint.at(STR("Value")))
        {
            float white[4] = {1, 1, 1, 1};
            memcpy(a, white, sizeof white);
        }
        tint.run();
        log("  U/V Tile = 1, Color Tint = white");
    }

    void apply_test_blade(UObject* weapon);
    void apply_full_weapon(UObject* weapon, const std::wstring& base_name);

    bool vector_sizes_ok()
    {
        // Our arrays are handed to the engine as FVector / FVector2D arrays: check they are doubles.
        int32_t vec = struct_size(STR("/Script/CoreUObject.Vector"));
        int32_t vec2 = struct_size(STR("/Script/CoreUObject.Vector2D"));
        log("Engine FVector %d bytes, FVector2D %d bytes", vec, vec2);
        if (vec != 24 || vec2 != 16)
        {
            log("  ! unexpected vector sizes, stopping");
            return false;
        }
        return true;
    }

    std::wstring model_base_name()
    {
        std::wstring base_name = g_settings.model;
        if (base_name.size() > 4 && _wcsicmp(base_name.c_str() + base_name.size() - 4, L".obj") == 0)
            base_name = base_name.substr(0, base_name.size() - 4);
        return base_name;
    }

    bool has_collision_file(const std::wstring& base_name)
    {
        if (FILE* probe = _wfopen((g_dir + base_name + L"_collision.obj").c_str(), L"r"))
        {
            fclose(probe);
            return true;
        }
        return false;
    }

    void apply()
    {
        log("---- F9: make the held weapon the custom model ----");
        load_settings();
        if (!vector_sizes_ok()) return;

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
        std::wstring base_name = model_base_name();
        if (has_collision_file(base_name))
            apply_full_weapon(weapon, base_name);
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
    // One-byte enum/byte property (GripType_Enum fields), read or written in place.
    bool byte_property(UObject* owner, const TCHAR* name, bool write, uint8_t& value)
    {
        FProperty* p = owner ? owner->GetPropertyByNameInChain(name) : nullptr;
        if (!p || p->GetSize() != 1) return false;
        uint8_t* at = reinterpret_cast<uint8_t*>(owner) + p->GetOffset_Internal();
        if (write) *at = value;
        else value = *at;
        return true;
    }

    // Puts Willie's hands on the model's handle (decompile: grip_type_flow.md, grab_and_mass.md):
    // - The grip type decides one or two hands. Willie copies the weapon's "Grip R Hand Default" into
    //   R_GripType_Default when it is equipped (Set Up Right Hand Weapon @16075) and derives the current
    //   type from it every tick (UG @493002); two hands need type 11 (the game's 15/20 cm longsword
    //   grips), its 12 cm arming-sword grip is type 2. Both the weapon and the pawn get the new value,
    //   so it holds now and on the next pick-up.
    // - The off hand goes to the weapon's "Grip L Hand Off Hand" scene each time it attaches
    //   (Attach L Hand To Offhand Position via Current Offhand Position Scene). The scene is moved
    //   along the head's axis onto the model's handle; a hand already attached is let go (Break L
    //   Constraint, what the game itself does) so it re-attaches at the new place.
    // Positions along the model: model cm, guard bottom 0, handle down to numbers["handle_bottom"].
    // The main hand sliding down a long handle during a swing. The game has no slide for swords (only
    // polearm handles: custom_weapon_hand_spots.md 4b, which moves the right constraint's weapon-side
    // reference with SetConstraintReferencePosition(Frame1)). Here the same reference point is moved
    // along the handle from the main hand's marker (by the guard) to just above the off hand, and back
    // when the swing ends. Frame1 = component 1 = the weapon's BaseMesh (SetConstrainedComponents(BaseMesh,
    // Mesh, hand_r)), so the point is in BaseMesh space, where the hand markers live.
    struct SwingSlide
    {
        UObject* pawn = nullptr;
        UObject* weapon = nullptr;
        UObject* base = nullptr;
        V3 top{}, bottom{};
        double a = 0, target = 0, calm = 0;
        bool touched = false;
        UFunction* set_ref = nullptr;
        UFunction* ang_vel = nullptr;
        FProperty* blockers[4] = {};
        // Camera Lock On (lock_on_target.md): Willie_BP "Lock On Found" (bool, false while the setting is off
        // or nothing is found) and "Lock Camera At" (world position of the target's head beacon, never cleared).
        FProperty* lock_found = nullptr;
        FProperty* lock_at = nullptr;
        double reach = 0;   // cm, main hand's spot to the tip
        bool skip = false;  // this swing: target too close, no slide
        double log_t = 0;
    };
    SwingSlide g_slide;

    bool pawn_flag(UObject* pawn, FProperty* p)
    {
        if (!p) return false;
        auto* b = static_cast<FBoolProperty*>(p);
        return (*(reinterpret_cast<uint8_t*>(pawn) + p->GetOffset_Internal() + b->GetByteOffset()) & b->GetFieldMask()) != 0;
    }

    // At the start of a swing: is the locked-on target far enough away to slide for reach?
    bool far_target(SwingSlide& s, UObject* pawn)
    {
        if (!s.lock_found || !s.lock_at || !pawn_flag(pawn, s.lock_found))
        {
            if (!g_settings.swing_slide_no_target) log("Swing slide: nothing locked on, no slide (swing_slide_no_target=0)");
            return g_settings.swing_slide_no_target != 0;
        }
        V3 head = read_three(reinterpret_cast<uint8_t*>(pawn) + s.lock_at->GetOffset_Internal(), s.lock_at);
        V3 grip = transform_point(world_transform(s.base), V3{0, 0, 0}, false);
        double dx = head.x - grip.x, dy = head.y - grip.y, dz = head.z - grip.z;
        double d = std::sqrt(dx * dx + dy * dy + dz * dz);
        double need = g_settings.swing_slide_min_target_cm > 0 ? g_settings.swing_slide_min_target_cm : s.reach + g_settings.swing_slide_arm_cm;
        bool far = d > need;
        log("Swing slide: target's head %.0f cm from the grip (slides beyond %.0f cm): %s", d, need, far ? "slide" : "too close, no slide");
        return far;
    }

    void slide_tick(float dt)
    {
        SwingSlide& s = g_slide;
        if (!s.weapon || !g_settings.swing_slide) return;
        UObject* pawn = player_pawn();
        if (pawn != s.pawn || object_property(pawn, STR("Weapon R")) != s.weapon)
        {
            log("Swing slide: weapon no longer held, stopped");
            s = SwingSlide{};
            return;
        }
        UObject* rc = object_property(pawn, STR("PhysicsConstraint R Hand"));
        bool blocked = !rc;
        for (FProperty* p : s.blockers) blocked = blocked || pawn_flag(pawn, p);
        if (blocked)
        {
            // Half-sword, Mordhau, swapped hands or a hand re-gripping: the game owns the grip; it re-makes
            // the constraint at the marker, so start again from the top.
            s.a = s.target = 0;
            s.touched = false;
            return;
        }
        V3 w{};
        {
            Call c(s.base, s.ang_vel, "GetPhysicsAngularVelocityInDegrees");
            if (c.run())
                if (FProperty* r = c.param(STR("ReturnValue"))) w = read_three(c.buffer.data() + r->GetOffset_Internal(), r);
        }
        double dps = std::sqrt(w.x * w.x + w.y * w.y + w.z * w.z);
        if (dps >= g_settings.swing_slide_start_dps)
        {
            if (s.target == 0 && !s.skip)
                s.skip = !far_target(s, pawn);  // decided once, when the swing starts
            if (!s.skip) s.target = 1;
            s.calm = 0;
        }
        else if (dps <= g_settings.swing_slide_end_dps)
        {
            s.calm += dt;
            if (s.calm >= g_settings.swing_slide_hold_s) s.target = 0, s.skip = false;
        }
        double was = s.a;
        double rate = dt / (std::max)(0.01, s.target > s.a ? g_settings.swing_slide_down_s : g_settings.swing_slide_up_s);
        s.a = s.target > s.a ? (std::min)(s.target, s.a + rate) : (std::max)(s.target, s.a - rate);
        if (s.a == was && !(s.a > 0 && !s.touched)) return;
        V3 ref{s.top.x + (s.bottom.x - s.top.x) * s.a, s.top.y + (s.bottom.y - s.top.y) * s.a, s.top.z + (s.bottom.z - s.top.z) * s.a};
        Call c(rc, s.set_ref, "SetConstraintReferencePosition");
        c.byte(STR("Frame"), 0).three(STR("RefPosition"), ref.x, ref.y, ref.z);
        bool ok = c.run();
        if (!c.ok)
        {
            log("Swing slide: SetConstraintReferencePosition could not be called, slide switched off");
            s = SwingSlide{};
            return;
        }
        s.touched = s.a > 0;
        if ((was == 0 && s.a > 0) || (s.a == 0 && was > 0) || (s.a == 1 && was < 1))
            log("Swing slide: %s (weapon turning %.0f deg/s), hand point z %.1f%s", s.a > was ? "down" : "up", dps, ref.z,
                ok ? "" : " (call FAILED)");
    }

    // A held main hand keeps the constraint frame it was made with. Breaking it alone does not make the
    // game re-grip (needs R Offhand Searching Grip), so the joint's weapon-side point is moved instead
    // (SetConstraintReferencePosition Frame1 = BaseMesh space, where the markers live; the game's own
    // polearm slide does the same, custom_weapon_hand_spots.md 4b). Skipped while the game holds the hand
    // in another grip; the new marker is used at the next grip change anyway.
    void regrip_main(UObject* pawn, UObject* mark)
    {
        UObject* rc = object_property(pawn, STR("PhysicsConstraint R Hand"));
        FProperty* rel = mark ? mark->GetPropertyByNameInChain(STR("RelativeLocation")) : nullptr;
        if (!rc || !rel) return;
        for (const TCHAR* f : {STR("R Alt Grip"), STR("Mordhau Grip"), STR("2H Switched Hands (Temp)"), STR("R Offhand Searching Grip"),
                               STR("R Hand Grip Slide"), STR("Hands Switching (2H)")})
            if (pawn_flag(pawn, pawn->GetPropertyByNameInChain(f)))
            {
                log("  main hand: in another grip now (%s); the new spot is used at the next grip change", narrow(f).c_str());
                return;
            }
        V3 r = read_three(reinterpret_cast<uint8_t*>(mark) + rel->GetOffset_Internal(), rel);
        Call c(rc, STR("SetConstraintReferencePosition"));
        c.byte(STR("Frame"), 0).three(STR("RefPosition"), r.x, r.y, r.z);
        log("  main hand moved to its new spot now (joint point %.1f %.1f %.1f: %s)", r.x, r.y, r.z, c.run() && c.ok ? "ok" : "FAILED");
    }

    void fit_hands(UObject* pawn, UObject* weapon, const Xf& head_xf, V3 hs, double shift, std::map<std::string, double>& numbers)
    {
        if (!g_settings.fit_hands) return;
        const double half_hand = 4.5; // cm, half a hand's width
        double handle_bottom = numbers["handle_bottom"];
        double grip_len = -handle_bottom;
        // The converter writes hafted=1/0; older files: no guard means hafted.
        bool hafted = numbers.count("hafted") ? numbers["hafted"] > 0.5 : numbers["guard_top"] - numbers["guard_bottom"] < 0.01;
        auto model_z = [&](UObject* c, V3& hl) {
            hl = transform_point(head_xf, world_location(c), true);
            return hl.z * hs.z - shift;
        };
        UObject* r_mark = object_property(weapon, STR("Grip R Hand Default Pos"));
        UObject* off_mark = object_property(weapon, STR("Grip L Hand Off Hand"));
        V3 rhl{}, ohl{};
        double rz = r_mark ? model_z(r_mark, rhl) : 0.0;
        double oz = off_mark ? model_z(off_mark, ohl) : 0.0;
        log("Hands: model handle %.1f cm long (guard bottom 0 to %.1f); main hand marker at %.1f cm, off hand marker at %.1f cm",
            grip_len, handle_bottom, rz, oz);

        if (!hafted)
        {
            uint8_t want = grip_len >= g_settings.two_hand_grip_cm ? 11 : 2, was = 0, l = 2;
            byte_property(weapon, STR("Grip R Hand Default"), false, was);
            byte_property(weapon, STR("Grip R Hand Default"), true, want);
            byte_property(weapon, STR("Grip L Hand Default"), true, l);
            uint8_t pawn_was = 0;
            byte_property(pawn, STR("R_GripType_Default"), false, pawn_was);
            bool pawn_ok = byte_property(pawn, STR("R_GripType_Default"), true, want);
            log("  grip type %u -> %u (%s; handle %.1f cm, two hands from %.1f cm); pawn R_GripType_Default %u -> %u%s", was, want,
                want == 11 ? "two-handed" : "one-handed", grip_len, g_settings.two_hand_grip_cm, pawn_was, want,
                pawn_ok ? "" : " (NOT written)");
        }
        else
            log("  hafted model: the held weapon's grip type is kept");

        if (hafted)
        {
            // Hafted: the game's own hand spots (spears: R -10, L 45, off hand 60, alt 45/95 ...) keep their
            // distance from the head (the model is lined up by the head); any that would be off the model's
            // haft is pulled onto it, half a hand from either end.
            double lo = handle_bottom + half_hand, hi = numbers["guard_bottom"] - half_hand;
            bool moved_l = false;
            // Spots placed by hand in the converter win (spot_* in the .txt, model cm). Spears and other
            // weapons whose "Grip L Hand Default" is 3 hold the main hand at "Grip L Hand Default Pos".
            uint8_t l_default = 0;
            byte_property(weapon, STR("Grip L Hand Default"), false, l_default);
            auto spot_for = [&](const std::string& n) -> const char* {
                // L Default 3 (spears, polearms): the main hand holds at L Default Pos; R Default Pos is also the
                // game's slide distance and hands-switch reference there, so it stays.
                if ((n == "Grip R Hand Default Pos" && l_default != 3) || (n == "Grip L Hand Default Pos" && l_default == 3))
                    return "spot_main";
                if (n == "Grip L Hand Off Hand") return "spot_off";
                if (n == "Grip L Hand Alt Pos") return "spot_half_off";
                if (n == "Grip R Hand Alt Pos") return "spot_half_main";
                if (n == "Grip R Hand Default Pos Reverse") return "spot_reverse";
                return nullptr;
            };
            for (const TCHAR* n : {STR("Grip R Hand Default Pos"), STR("Grip L Hand Default Pos"), STR("Grip L Hand Off Hand"),
                                   STR("Grip L Hand Alt Pos"), STR("Grip R Hand Alt Pos"), STR("Grip R Hand Default Pos Reverse")})
            {
                UObject* mark = object_property(weapon, n);
                if (!mark) continue;
                V3 hl{};
                double z = model_z(mark, hl);
                const char* key = spot_for(narrow(n));
                bool chosen = key && numbers.count(key);
                if (!chosen && narrow(n) == "Grip R Hand Default Pos Reverse") continue; // the game's own place
                double want = chosen ? numbers[key] : (std::min)((std::max)(z, lo), hi);
                if (hi < lo && !chosen) want = 0.5 * (lo + hi);
                if (std::fabs(want - z) < 0.5)
                {
                    log("  %s at %.1f cm: %s", narrow(n).c_str(), z, chosen ? "already where it was placed" : "on the haft");
                    continue;
                }
                V3 w = transform_point(head_xf, V3{hl.x, hl.y, (want + shift) / hs.z}, false);
                Call move(mark, STR("K2_SetWorldLocation"));
                move.three(STR("NewLocation"), w.x, w.y, w.z).flag(STR("bTeleport"), true);
                move.run();
                log("  %s %.1f -> %.1f cm (%s)", narrow(n).c_str(), z, want, chosen ? "placed in the converter" : "was off the model's haft");
                if (key && std::string(key) == "spot_main") regrip_main(pawn, mark);
                moved_l = moved_l || narrow(n) == "Grip L Hand Off Hand";
            }
            if (moved_l && off_mark)
            {
                FProperty* p = pawn->GetPropertyByNameInChain(STR("L Hand In Offhand Attached"));
                if (pawn_flag(pawn, p))
                {
                    Call brk(pawn, STR("Break L Constraint"));
                    brk.flag(STR("Destroy Constraint"), true);
                    log("  off hand let go so it re-attaches at the new place (%s)", brk.run() ? "ok" : "FAILED");
                }
            }
            log("  a main hand spot that moved takes effect at the next grab (drop and pick up, or a grip change)");
            return;
        }
        if (!off_mark || !r_mark)
        {
            log("  ! weapon has no %s marker", r_mark ? "off hand" : "main hand");
            return;
        }
        // Hand spots (decompile: defaults\custom_weapon_hand_spots.md). All are fixed scene markers under
        // the weapon's BaseMesh, read fresh on every pick-up, re-grip and off-hand attach:
        //   Grip R Hand Default Pos  main hand, normal grip
        //   Grip L Hand Off Hand     off hand, normal grip (Current Offhand Position Scene)
        //   Grip L Hand Alt Pos      off hand while half-swording (R Alt Grip): the game's +45/+50/+60 cm
        //   Grip R Hand Alt Pos      main hand while half-swording (0 on every game sword)
        // Swords never slide a hand (only polearm handles do), so a long handle gets its spots placed
        // where they give the reach: normal grip at the pommel end, half-sword grip pommel + guard.
        UObject* l_alt = object_property(weapon, STR("Grip L Hand Alt Pos"));
        UObject* r_alt = object_property(weapon, STR("Grip R Hand Alt Pos"));
        auto place = [&](UObject* mark, const char* what, double target) {
            if (!mark) { log("  ! weapon has no %s marker", what); return; }
            V3 hl{};
            double was = model_z(mark, hl);
            V3 w = transform_point(head_xf, V3{hl.x, hl.y, (target + shift) / hs.z}, false);
            Call move(mark, STR("K2_SetWorldLocation"));
            move.three(STR("NewLocation"), w.x, w.y, w.z).flag(STR("bTeleport"), true);
            move.run();
            V3 dummy{};
            log("  %s marker %.1f -> %.1f cm; now reads %.1f cm", what, was, target, model_z(mark, dummy));
        };
        double lo = handle_bottom + half_hand;     // lowest hand centre on the handle
        double hand = 2.0 * half_hand;             // one hand's width
        double tip = numbers["tip"];
        double blade_from = numbers["guard_top"];
        // Per weapon (Matthew 2026-11-21): the converter's "hand slides" switch, hand_slide=1 in the model's
        // .txt, for long-handled odd weapons like the Spit; traditional swords keep the game's grip.
        bool long_handle = !hafted && numbers.count("hand_slide") && numbers["hand_slide"] > 0.5;
        double r_target = rz, off_target = oz, l_alt_target = 0, r_alt_target = 0;
        if (long_handle)
        {
            // Main hand by the guard (its own marker), off hand at the pommel end; the main hand slides down
            // to just above the off hand during a swing (slide_tick).
            off_target = lo;
            // Half-sword grip on a long handle: main hand at the pommel end, off hand just under the guard.
            r_alt_target = lo;
            l_alt_target = -half_hand;
            log("  hand slide on for this weapon (handle %.1f cm): main hand by the guard, off hand at the pommel end, "
                "main hand slides down in swings; half-sword grip pommel + guard", grip_len);
        }
        else
        {
            if (g_settings.offhand == L"pommel") off_target = lo;
            off_target = (std::min)((std::max)(off_target, lo), r_target - 5.0); // never on top of the main hand
            if (off_target < lo) off_target = lo;
            // Half-sword: the game puts the off hand at about half a game blade (+50 cm on 85-110 cm blades).
            // Same share of the model's blade, kept a hand away from the guard and the tip.
            double blade = tip - blade_from;
            l_alt_target = (std::min)((std::max)(blade_from + 0.5 * blade, blade_from + hand), tip - hand);
            r_alt_target = r_target;
        }
        // Reverse (Mordhau) grip: the game puts it halfway between the half-sword spot and the tip
        // (MW @7331-7833, at BeginPlay, from the GAME blade), so it is redone for the model's blade.
        double rev_target = 0.5 * (l_alt_target + tip);
        // Spots placed by hand in the converter win (spot_* in the .txt, model cm along the weapon).
        auto chosen = [&](const char* key, double& v, const char* what) {
            if (!numbers.count(key)) return false;
            log("  %s: placed in the converter at %.1f cm (instead of %.1f)", what, numbers[key], v);
            v = numbers[key];
            return true;
        };
        bool main_chosen = chosen("spot_main", r_target, "main hand");
        chosen("spot_off", off_target, "off hand");
        chosen("spot_half_main", r_alt_target, "half-sword main hand");
        chosen("spot_half_off", l_alt_target, "half-sword off hand");
        chosen("spot_reverse", rev_target, "reverse grip main hand");
        if (main_chosen)
        {
            place(r_mark, "main hand", r_target);
            regrip_main(pawn, r_mark);
        }
        g_slide = SwingSlide{};
        if (long_handle && g_settings.swing_slide)
        {
            UObject* base = object_property(weapon, STR("BaseMesh"));
            UObject* rc = object_property(pawn, STR("PhysicsConstraint R Hand"));
            FProperty* rel = r_mark->GetPropertyByNameInChain(STR("RelativeLocation"));
            if (base && rel)
            {
                SwingSlide& sl = g_slide;
                sl.pawn = pawn;
                sl.weapon = weapon;
                sl.base = base;
                sl.top = read_three(reinterpret_cast<uint8_t*>(r_mark) + rel->GetOffset_Internal(), rel);
                V3 w = transform_point(head_xf, V3{rhl.x, rhl.y, (lo + hand + shift) / hs.z}, false);
                sl.bottom = transform_point(world_transform(base), w, true);
                UObject* pc = find(STR("/Script/Engine.Default__PhysicsConstraintComponent"));
                if (pc) sl.set_ref = pc->GetFunctionByNameInChain(STR("SetConstraintReferencePosition"));
                sl.ang_vel = base->GetFunctionByNameInChain(STR("GetPhysicsAngularVelocityInDegrees"));
                const TCHAR* names[4] = {STR("R Alt Grip"), STR("Mordhau Grip"), STR("2H Switched Hands (Temp)"),
                                         STR("R Offhand Searching Grip")};
                for (int i = 0; i < 4; ++i)
                {
                    sl.blockers[i] = pawn->GetPropertyByNameInChain(names[i]);
                    if (!sl.blockers[i]) log("  ! pawn has no %s flag", narrow(names[i]).c_str());
                }
                sl.lock_found = pawn->GetPropertyByNameInChain(STR("Lock On Found"));
                sl.lock_at = pawn->GetPropertyByNameInChain(STR("Lock Camera At"));
                if (!sl.lock_found || !sl.lock_at) log("  ! pawn has no Lock On Found / Lock Camera At: slide ignores the target");
                sl.reach = tip - r_target;
                log("  swing slide: main hand to tip %.0f cm", sl.reach);
                log("  swing slide: hand point (BaseMesh) z %.1f -> %.1f (model %.1f -> %.1f cm); constraint %s; functions %s/%s",
                    sl.top.z, sl.bottom.z, r_target, lo + hand, rc ? "found" : "NOT found", sl.set_ref ? "ok" : "MISSING",
                    sl.ang_vel ? "ok" : "MISSING");
                if (!sl.set_ref || !sl.ang_vel) g_slide = SwingSlide{};
            }
        }
        place(off_mark, "off hand", off_target);
        if (!hafted && blade_from < tip)
        {
            place(l_alt, "half-sword off hand", l_alt_target);
            place(r_alt, "half-sword main hand", r_alt_target);
            place(object_property(weapon, STR("Grip R Hand Default Pos Reverse")), "reverse grip main hand", rev_target);
        }
        // A hand already holding keeps its old spot until it grips again: let go the way the game itself
        // does when it switches grips (Break R/L Constraint); each hand re-attaches at its new marker.
        auto attached = [&](const TCHAR* name) {
            FProperty* p = pawn->GetPropertyByNameInChain(name);
            if (!p) return false;
            auto* b = static_cast<FBoolProperty*>(p);
            return (*(reinterpret_cast<uint8_t*>(pawn) + p->GetOffset_Internal() + b->GetByteOffset()) & b->GetFieldMask()) != 0;
        };
        if (attached(STR("L Hand In Offhand Attached")))
        {
            Call brk(pawn, STR("Break L Constraint"));
            brk.flag(STR("Destroy Constraint"), true);
            log("  off hand was attached: let go so it re-attaches at the new place (%s)", brk.run() ? "ok" : "FAILED");
        }
    }

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

    bool has_mesh(UObject* component)
    {
        return component && object_property(component, STR("StaticMesh"));
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

        // The weapon's parts (decompile: defaults\custom_weapon_hafted.md). Three layouts:
        //   A modular (forge swords, pollaxes, maces, *_Tier_*): Head / Guard / Grip / Pommel slots
        //     (+ "Sub Module 1 Mesh"/"SubModule 2 Mesh" side pieces on pollaxes);
        //   B reforged two-piece (spears, halberds, billhooks, war staffs, axes): "Blade" + "Grip_0"
        //     (+ "Spike"), the inherited slots empty;
        //   C one piece (tools, hurlbat, staff): "Weapon" (or Blade / Grip_0 alone).
        // New collision goes into those components with SetStaticMesh, which keeps their hit bindings,
        // tags, collision profile and Tip children. Flails (simulating chain) are refused.
        bool flail = has_mesh(object_property(weapon, STR("Blade1"))) || object_property(weapon, STR("Link 1"));
        if (UObject* h = object_property(weapon, STR("Head")))
        {
            Call tag(h, STR("ComponentHasTag"));
            static FName flail_tag(STR("Flail"), FNAME_Add);
            if (FProperty* p = tag.param(STR("Tag"))) *reinterpret_cast<FName*>(tag.buffer.data() + p->GetOffset_Internal()) = flail_tag;
            flail = flail || (tag.run() && tag.ret_bool()); // Tool_Flail: its Head is the swinging part
        }
        if (flail)
        {
            log("  ! flails are not supported (their head hangs on a simulating chain)");
            return;
        }
        struct Part
        {
            const char* name;
            UObject* comp;
            std::vector<std::string> model_slots; // which model parts' collision pieces it gets
        };
        std::vector<Part> parts;
        UObject* head = nullptr;
        bool one_piece = false;
        auto comp = [&](const TCHAR* n) { return object_property(weapon, n); };
        if (has_mesh(comp(STR("Head"))))
        {
            head = comp(STR("Head"));
            parts = {{"Head", head, {"Head"}}, {"Guard", comp(STR("Guard")), {"Guard"}},
                     {"Grip", comp(STR("Grip")), {"Grip"}}, {"Pommel", comp(STR("Pommel")), {"Pommel"}}};
            if (!has_mesh(parts[1].comp) && !has_mesh(parts[3].comp) && !has_mesh(parts[2].comp))
                parts[0].model_slots = {"Head", "Guard", "Grip", "Pommel"}; // a bare head (old test weapons)
        }
        else if (has_mesh(comp(STR("Blade"))) && has_mesh(comp(STR("Grip_0"))))
        {
            head = comp(STR("Blade"));
            parts = {{"Blade", head, {"Head"}}, {"Grip_0", comp(STR("Grip_0")), {"Guard", "Grip", "Pommel"}}};
        }
        else
        {
            for (const TCHAR* n : {STR("Weapon"), STR("Blade"), STR("Grip_0")})
                if (has_mesh(comp(n)))
                {
                    head = comp(n);
                    static std::string one_names[3] = {"Weapon", "Blade", "Grip_0"};
                    const char* label = "Weapon";
                    for (auto& on : one_names)
                        if (narrow(n) == on) label = on.c_str();
                    parts = {{label, head, {"Head", "Guard", "Grip", "Pommel"}}};
                    one_piece = true;
                    break;
                }
        }
        if (!head)
        {
            log("  ! no weapon part with a mesh found (Head, Blade, Weapon), or it was already replaced");
            return;
        }
        // Extra pieces the model has nothing for: shrunk to a 1 cm box inside the model and made light.
        for (const TCHAR* extra : {STR("Sub Module 1 Mesh"), STR("SubModule 2 Mesh"), STR("Spike")})
            if (has_mesh(comp(extra)))
            {
                static std::string names[3] = {"Sub Module 1 Mesh", "SubModule 2 Mesh", "Spike"};
                for (auto& n : names)
                    if (narrow(extra) == n) parts.push_back({n.c_str(), comp(extra), {}});
            }

        UObject* base = object_property(weapon, STR("BaseMesh"));
        double parts_before = 0;
        for (Part& pt : parts)
        {
            float kg = 0;
            body_mass(pt.comp, false, kg);
            parts_before += kg;
            V3 lo{}, hi{};
            mesh_box(object_property(pt.comp, STR("StaticMesh")), lo, hi);
            log("  part %s: mesh %s, own mass setting %.3f kg, mesh z %.1f to %.1f", pt.name,
                name_of(object_property(pt.comp, STR("StaticMesh"))).c_str(), kg, lo.z, hi.z);
        }
        // BaseMesh (the grip dummy) has a mass of its own, which stays; the parts get the rest.
        double whole_before = mass_of(base);
        double base_share = whole_before - parts_before;
        double parts_target = mass_total - base_share;
        if (base_share < 0 || parts_target < 0.25 * mass_total) parts_target = mass_total;
        log("  whole weapon %.3f kg (BaseMesh's own %.3f kg; parts get %.3f kg)", whole_before, base_share, parts_target);
        V3 olo{}, ohi{};
        mesh_box(object_property(head, STR("StaticMesh")), olo, ohi);
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

        // Lining the model up along the head's axis:
        // - a sword model on a weapon with a guard: the model's guard goes where the game's guard starts,
        //   so the hands (placed by the game's grip) stay below it;
        // - one-piece weapons: the model's bottom end on the game mesh's bottom end;
        // - otherwise (hafted heads): the model's head starts where the game's head mesh starts, so the
        //   reach and the game's hand spots keep their distance from the head.
        bool model_hafted = numbers.count("hafted") && numbers["hafted"] > 0.5;
        double shift = olo.z * hs.z - guard_top;
        const char* how = "head bottoms";
        UObject* guard = comp(STR("Guard"));
        if (one_piece)
        {
            shift = olo.z * hs.z - numbers["pommel_end"];
            how = "bottom ends";
        }
        else if (!model_hafted && has_mesh(guard))
        {
            V3 glo{}, ghi{};
            if (mesh_box(object_property(guard, STR("StaticMesh")), glo, ghi))
            {
                V3 w = transform_point(world_transform(guard), V3{0, 0, glo.z}, false);
                V3 h = transform_point(head_xf, w, true);
                shift = h.z * hs.z - numbers["guard_bottom"];
                how = "guards";
            }
        }
        log("  model lined up by the %s: shift %.1f cm (game head mesh z %.1f..%.1f)", how, shift, olo.z * hs.z, ohi.z * hs.z);

        Call get_mat(head, STR("GetMaterial"));
        get_mat.i32(STR("ElementIndex"), 0);
        UObject* material = get_mat.run() ? get_mat.ret_object() : nullptr;

        // Mass: shared between the parts by the volume of their pieces (one material throughout); parts the
        // model has nothing for get a token 10 g.
        const double token_kg = 0.01;
        double volume_all = 0;
        std::vector<double> volume(parts.size(), 0.0);
        int empty_parts = 0;
        for (Hull& h : hulls) volume_all += h.volume;
        for (size_t i = 0; i < parts.size(); ++i)
        {
            for (Hull& h : hulls)
                for (auto& ms : parts[i].model_slots)
                    if (ms == h.slot) volume[i] += h.volume;
            if (volume[i] <= 0 && has_mesh(parts[i].comp)) ++empty_parts;
        }
        double share_target = (std::max)(0.05, parts_target - token_kg * empty_parts);

        for (size_t i = 0; i < parts.size(); ++i)
        {
            Part& pt = parts[i];
            if (!pt.comp) continue;
            if (!has_mesh(pt.comp)) continue; // an empty slot (maces' Guard/Pommel): no body, keep it that way
            Xf slot_xf = pt.comp == head ? head_xf : world_transform(pt.comp);
            auto to_slot = [&](V3 p) {
                p.z += shift;
                V3 hl = to_head_local(p);
                return pt.comp == head ? hl : transform_point(slot_xf, transform_point(head_xf, hl, false), true);
            };
            std::vector<std::vector<V3>> pieces;
            for (Hull& h : hulls)
            {
                bool mine = false;
                for (auto& ms : pt.model_slots) mine = mine || ms == h.slot;
                if (!mine) continue;
                std::vector<V3> local;
                for (V3 p : h.points) local.push_back(to_slot(p));
                pieces.push_back(local);
            }
            float kg = volume_all > 0 ? (float)(share_target * volume[i] / volume_all) : 0.0f;
            if (pieces.empty())
            {
                // Nothing in the model for this part (no guard, no side pieces): a 1 cm box inside the
                // model where the handle meets the head, so it neither shows nor hits anything of its own.
                std::vector<V3> cube;
                for (int k = 0; k < 8; ++k)
                    cube.push_back(to_slot(V3{(k & 1) ? 0.5 : -0.5, (k & 2) ? 0.5 : -0.5, guard_top + ((k & 4) ? 0.5 : -0.5)}));
                pieces.push_back(cube);
                kg = (float)token_kg;
            }
            UObject* mesh = build_part_mesh(weapon, pt.comp, pieces);
            if (!mesh) continue;
            body_mass(pt.comp, true, kg);
            Call set(pt.comp, STR("SetStaticMesh"));
            set.object(STR("NewMesh"), mesh);
            set.run();
            hide(pt.comp);
            log("  %s: new collision in place (%s; %zu pieces%s), mass setting %.3f kg", pt.name,
                set.ret_bool() ? "changed" : "NOT changed", pieces.size(), volume[i] > 0 ? "" : ", none in the model: 1 cm box", kg);
        }
        log("Whole weapon mass now %.3f kg (target %.3f kg)", mass_of(base), mass_total);

        // Damage points (Tip/Base scenes, damage boxes) on the head move with it: the game head's length
        // [olo, ohi] maps onto the model's head [head start, tip] (one piece: the whole model). Found by
        // parent, not by name: reforged weapons' tips are plain components (Tip1, Tip2, Box), modular
        // ones are NODE_AddSceneComponent copies.
        double from_lo = olo.z, from_hi = ohi.z;
        double to_lo = ((one_piece ? numbers["pommel_end"] : guard_top) + shift) / hs.z, to_hi = (tip + shift) / hs.z;
        std::vector<UObject*> comps;
        UObjectGlobals::FindAllOf(STR("SceneComponent"), comps);
        for (UObject* c : comps)
        {
            if (object_property(c, STR("AttachParent")) != head) continue;
            std::wstring cls = c->GetClassPrivate()->GetName();
            if (cls.find(STR("ProceduralMesh")) != std::wstring::npos || cls.find(STR("StaticMesh")) != std::wstring::npos) continue;
            FProperty* rp = c->GetPropertyByNameInChain(STR("RelativeLocation"));
            if (!rp || from_hi - from_lo < 1.0) continue;
            V3 r = read_three(reinterpret_cast<uint8_t*>(c) + rp->GetOffset_Internal(), rp);
            double z = to_lo + (r.z - from_lo) * (to_hi - to_lo) / (from_hi - from_lo);
            Call move(c, STR("K2_SetRelativeLocation"));
            move.three(STR("NewLocation"), r.x, r.y, z).flag(STR("bTeleport"), true);
            move.run();
            log("  damage point %s (%s): z %.1f -> %.1f (head units)", narrow(c->GetName()).c_str(), narrow(cls).c_str(), r.z, z);
        }
        // Reforged hafts keep blunt tips on Grip_0 (Spear_B Tip2 at 112, Base1_1 at 150): any that would
        // sit beyond the model's haft top (inside the new head) or below its bottom is pulled onto the haft.
        for (Part& pt : parts)
        {
            if (pt.comp == head || std::string(pt.name) != "Grip_0") continue;
            Xf gx = world_transform(pt.comp);
            for (UObject* c : comps)
            {
                if (object_property(c, STR("AttachParent")) != pt.comp) continue;
                std::wstring cls = c->GetClassPrivate()->GetName();
                if (cls.find(STR("ProceduralMesh")) != std::wstring::npos || cls.find(STR("StaticMesh")) != std::wstring::npos) continue;
                V3 hl = transform_point(head_xf, world_location(c), true);
                double mz = hl.z * hs.z - shift; // model cm
                double want = (std::min)((std::max)(mz, numbers["pommel_end"] + 1.0), numbers["guard_bottom"] - 1.0);
                if (std::fabs(want - mz) < 0.5) continue;
                V3 w = transform_point(head_xf, V3{hl.x, hl.y, (want + shift) / hs.z}, false);
                Call move(c, STR("K2_SetWorldLocation"));
                move.three(STR("NewLocation"), w.x, w.y, w.z).flag(STR("bTeleport"), true);
                move.run();
                log("  haft damage point %s: model z %.1f -> %.1f cm (kept on the haft)", narrow(c->GetName()).c_str(), mz, want);
            }
        }

        // The visible model in the head's local units; normals and tangents adjusted so that, after the
        // engine applies the head's scale, they are right in the world.
        for (V3& p : model.positions)
        {
            p.z += shift;
            p = to_head_local(p);
        }
        for (V3& n : model.normals) n = unit(V3{n.x * hs.x, n.y * hs.y, n.z * hs.z});
        UObject* proc = build_visual(weapon, head, model, material);
        if (proc) apply_own_textures(weapon, proc, material, g_dir + base_name);
        fit_hands(player_pawn(), weapon, head_xf, hs, shift, numbers);
        log(proc ? "Done. Swing it at something and check the hits." : "  ! visual model failed (collision swap above still applies)");
    }

    bool g_key_was_down = false;

    void tick(UObject* engine, float dt)
    {
        g_engine = engine;
        slide_tick(dt);
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
                [](auto&, UEngine* engine, float dt, bool) { tick(reinterpret_cast<UObject*>(engine), dt); },
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
