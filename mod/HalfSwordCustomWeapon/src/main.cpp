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
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/Core/HAL/UnrealMemory.hpp>

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

    std::wstring widen(const std::string& s)
    {
        if (s.empty()) return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
        return w;
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
    bool apply_full_weapon(UObject* weapon, const std::wstring& base, bool held);

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
            apply_full_weapon(weapon, g_dir + base_name, true);
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

    // ---- converted weapons: loaded model files, cached --------------------------------------------
    // The game spawns a weapon for every trader list row and loot point, so the files are read once and
    // kept until they change on disk.
    // A converted weapon's own damage point (tools/damage_points.py, <name>_points.txt): model cm, the
    // frame of the model (z along the weapon, 0 at the top of the grip); f = thrust direction (the scene's
    // +X), u = the way the edge faces (+Z); the five values and the flags become the scene's tags.
    struct DPoint
    {
        std::string slot;
        V3 p{}, f{}, u{};
        int sTip = 0, sEdge = 0, dSharp = 0, dBlunt = 0, rig = 100;
        std::vector<std::string> flags;
    };

    std::vector<DPoint> load_points(const std::wstring& path)
    {
        std::vector<DPoint> out;
        FILE* f = _wfopen(path.c_str(), L"r");
        if (!f) return out;
        char line[512];
        while (fgets(line, sizeof line, f))
        {
            if (line[0] == ';' || line[0] == '#') continue;
            char slot[32] = {};
            DPoint d;
            int used = 0;
            if (sscanf(line, "%31s %lf %lf %lf %lf %lf %lf %lf %lf %lf %d %d %d %d %d%n", slot, &d.p.x, &d.p.y, &d.p.z,
                       &d.f.x, &d.f.y, &d.f.z, &d.u.x, &d.u.y, &d.u.z, &d.sTip, &d.sEdge, &d.dSharp, &d.dBlunt, &d.rig,
                       &used) < 15)
                continue;
            d.slot = slot;
            const char* rest = line + used;
            char flag[32];
            int n = 0;
            while (sscanf(rest, "%31s%n", flag, &n) == 1)
            {
                d.flags.push_back(flag);
                rest += n;
            }
            out.push_back(d);
        }
        fclose(f);
        return out;
    }

    struct ModelData
    {
        Model model;
        std::vector<Hull> hulls;
        std::vector<DPoint> points;
        std::map<std::string, double> numbers;
        uint64_t stamp = 0;
    };
    std::map<std::wstring, ModelData> g_models;

    uint64_t file_stamp(const std::wstring& path)
    {
        WIN32_FILE_ATTRIBUTE_DATA a{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return 0;
        return (uint64_t(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime;
    }

    const ModelData* model_data(const std::wstring& base)
    {
        uint64_t stamp = file_stamp(base + L".obj") ^ (file_stamp(base + L"_collision.obj") * 3) ^ (file_stamp(base + L".txt") * 7) ^
                         (file_stamp(base + L"_points.txt") * 13);
        auto it = g_models.find(base);
        if (it != g_models.end() && it->second.stamp == stamp) return &it->second;
        ModelData d;
        d.stamp = stamp;
        d.numbers = read_numbers(base + L".txt");
        d.hulls = load_hulls(base + L"_collision.obj");
        d.points = load_points(base + L"_points.txt");
        if (!load_obj(base + L".obj", d.model))
        {
            log("  ! could not read %s.obj", narrow(base).c_str());
            return nullptr;
        }
        return &(g_models[base] = std::move(d));
    }

    // What the hands need when the player takes a converted weapon (fit_hands runs again then).
    struct Built
    {
        UObject* head = nullptr;
        V3 hs{1, 1, 1};
        double shift = 0;
        std::map<std::string, double> numbers;
        V3 off{0, 0, 0}; // shields: model cm + off = the shield component's frame (before its scale)
        bool shield = false;
    };
    std::map<UObject*, Built> g_built;

    // A scene component's direct children (USceneComponent::AttachChildren).
    std::vector<UObject*> children_of(UObject* comp)
    {
        std::vector<UObject*> out;
        if (!comp) return out;
        auto* arr = comp->GetValuePtrByPropertyNameInChain<RawArray>(STR("AttachChildren"));
        if (!arr || !arr->Data) return out;
        auto* items = static_cast<UObject* const*>(arr->Data);
        for (int32_t i = 0; i < arr->Num; ++i)
            if (items[i]) out.push_back(items[i]);
        return out;
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
        bool is_far = d > need;
        log("Swing slide: target's head %.0f cm from the grip (slides beyond %.0f cm): %s", d, need, is_far ? "slide" : "too close, no slide");
        return is_far;
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
            if (moved_l && off_mark && pawn)
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
        if (pawn) g_slide = SwingSlide{};
        if (pawn && long_handle && g_settings.swing_slide)
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
            FProperty* p = pawn ? pawn->GetPropertyByNameInChain(name) : nullptr;
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

    // ---- the converted weapon's own damage points ----------------------------------------------------
    // Half Sword reads cut/stab/blunt from "Tip"-tagged scene components under the part that was hit, at
    // every hit (decompile: defaults\weapon_damage_points.md): the nearest one wins, its +X is the thrust
    // direction, its +Z the way the edge faces, its tags carry sTipNNN sEdgeNNN dSharpN dBluntN rigNNN and
    // the flags. The carrier's own points are untagged ("Tip" -> "CW_Tip_Off") and ours are added.
    void descendants(UObject* comp, std::vector<UObject*>& out)
    {
        for (UObject* c : children_of(comp))
        {
            out.push_back(c);
            descendants(c, out);
        }
    }

    // Replaces tag `from` with `to` in a component's ComponentTags; how many were replaced.
    int retag(UObject* comp, const FName& from, const FName& to)
    {
        auto* arr = comp ? comp->GetValuePtrByPropertyNameInChain<RawArray>(STR("ComponentTags")) : nullptr;
        if (!arr || !arr->Data) return 0;
        auto* names = static_cast<FName*>(const_cast<void*>(arr->Data));
        int n = 0;
        for (int32_t i = 0; i < arr->Num; ++i)
            if (names[i] == from)
            {
                names[i] = to;
                ++n;
            }
        return n;
    }

    bool write_identity_transform(uint8_t* field, FProperty* property)
    {
        UScriptStruct* type = struct_type(property);
        if (!type) return false;
        bool rot = false, scale = false;
        for (FProperty* m : type->ForEachProperty())
        {
            uint8_t* at = field + m->GetOffset_Internal();
            if (same_name(m, STR("Rotation")))
            {
                // FQuat X, Y, Z, W: W = 1
                if (FProperty* w = member(struct_type(m), STR("W")))
                {
                    uint8_t* wa = at + w->GetOffset_Internal();
                    if (w->GetClass().GetName() == STR("DoubleProperty")) *reinterpret_cast<double*>(wa) = 1.0;
                    else *reinterpret_cast<float*>(wa) = 1.0f;
                    rot = true;
                }
            }
            else if (same_name(m, STR("Scale3D")))
                scale = write_three(at, m, 1, 1, 1);
        }
        return rot && scale;
    }

    UObject* add_damage_point(UObject* weapon, UObject* parent, V3 world, V3 fwd, V3 up, const std::vector<FName>& tags)
    {
        static UObject* scene_class = find(STR("/Script/Engine.SceneComponent"));
        static UObject* math = find(STR("/Script/Engine.Default__KismetMathLibrary"));
        Call add(weapon, STR("AddComponentByClass"));
        add.object(STR("Class"), scene_class).flag(STR("bManualAttachment"), true).flag(STR("bDeferredFinish"), false);
        FProperty* t = add.param(STR("RelativeTransform"));
        if (!t || !write_identity_transform(add.buffer.data() + t->GetOffset_Internal(), t))
        {
            log("  ! AddComponentByClass: could not write RelativeTransform");
            return nullptr;
        }
        if (!add.run()) return nullptr;
        UObject* c = add.ret_object();
        if (!c) return nullptr;
        if (auto* arr = c->GetValuePtrByPropertyNameInChain<RawArray>(STR("ComponentTags")))
        {
            auto* mem = static_cast<FName*>(FMemory::Malloc(sizeof(FName) * tags.size()));
            for (size_t i = 0; i < tags.size(); ++i) new (&mem[i]) FName(tags[i]);
            if (arr->Data) FMemory::Free(const_cast<void*>(arr->Data));
            *arr = RawArray{mem, (int32_t)tags.size(), (int32_t)tags.size()};
        }
        Call at(c, STR("K2_AttachToComponent"));
        at.object(STR("Parent"), parent); // rules 0 = KeepRelative, socket None, no welding (all zero)
        at.run();
        Call rot(math, STR("MakeRotFromXZ"));
        rot.three(STR("X"), fwd.x, fwd.y, fwd.z).three(STR("Z"), up.x, up.y, up.z);
        Call place(c, STR("K2_SetWorldLocationAndRotation"));
        place.three(STR("NewLocation"), world.x, world.y, world.z).flag(STR("bTeleport"), true);
        FProperty* r = rot.run() ? rot.param(STR("ReturnValue")) : nullptr;
        FProperty* nr = place.param(STR("NewRotation"));
        if (r && nr && r->GetSize() == nr->GetSize())
            memcpy(place.buffer.data() + nr->GetOffset_Internal(), rot.buffer.data() + r->GetOffset_Internal(), r->GetSize());
        place.run();
        return c;
    }

    // Height (model z) of a convex piece's centre: tetrahedra from its mean point to each triangle.
    double hull_centroid_z(const Hull& h)
    {
        if (h.points.empty()) return 0;
        V3 a{0, 0, 0};
        for (const V3& p : h.points) a = {a.x + p.x, a.y + p.y, a.z + p.z};
        double n = (double)h.points.size();
        a = {a.x / n, a.y / n, a.z / n};
        double vs = 0, vz = 0;
        for (size_t i = 0; i + 2 < h.faces.size(); i += 3)
        {
            const V3 &p = h.points[h.faces[i]], &q = h.points[h.faces[i + 1]], &r = h.points[h.faces[i + 2]];
            V3 c = cross(sub(q, a), sub(r, a));
            V3 d = sub(p, a);
            double v = std::fabs(d.x * c.x + d.y * c.y + d.z * c.z) / 6.0;
            vs += v;
            vz += v * (p.z + q.z + r.z + a.z) / 4.0;
        }
        return vs > 0 ? vz / vs : a.z;
    }

    // base = the model's path without extension (<pack folder>/<name>); its .obj, _collision.obj, .txt and
    // textures sit there. held: the player holds it now (F9 style); otherwise only the weapon is changed,
    // and the hands are fitted when the player takes it (held_tick).
    bool apply_full_weapon(UObject* weapon, const std::wstring& base, bool held)
    {
        const ModelData* data = model_data(base);
        if (!data) return false;
        auto numbers = data->numbers;
        const auto& hulls = data->hulls;
        Model model = data->model;
        std::wstring base_name = base.substr(base.find_last_of(L"\\/") + 1);
        if (hulls.empty() || numbers.empty())
        {
            log("  ! %s: need %s.obj, %s_collision.obj and %s.txt", narrow(base_name).c_str(), narrow(base_name).c_str(),
                narrow(base_name).c_str(), narrow(base_name).c_str());
            return false;
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
            return false;
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
            return false;
        }
        // Extra pieces the model has nothing for: shrunk to a 1 cm box inside the model and made light.
        for (const TCHAR* extra : {STR("Sub Module 1 Mesh"), STR("SubModule 2 Mesh"), STR("Spike")})
            if (has_mesh(comp(extra)))
            {
                static std::string names[3] = {"Sub Module 1 Mesh", "SubModule 2 Mesh", "Spike"};
                for (auto& n : names)
                    if (narrow(extra) == n) parts.push_back({n.c_str(), comp(extra), {}});
            }

        UObject* base_mesh = object_property(weapon, STR("BaseMesh"));
        double parts_before = 0;
        std::vector<double> game_kg;
        for (Part& pt : parts)
        {
            float kg = 0;
            body_mass(pt.comp, false, kg);
            parts_before += kg;
            game_kg.push_back(kg);
            V3 lo{}, hi{};
            mesh_box(object_property(pt.comp, STR("StaticMesh")), lo, hi);
            log("  part %s: mesh %s, own mass setting %.3f kg, mesh z %.1f to %.1f", pt.name,
                name_of(object_property(pt.comp, STR("StaticMesh"))).c_str(), kg, lo.z, hi.z);
        }
        // BaseMesh (the grip dummy) has a mass of its own, which stays; the parts get the rest.
        double whole_before = mass_of(base_mesh);
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
            return false;
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
        for (const Hull& h : hulls) volume_all += h.volume;
        for (size_t i = 0; i < parts.size(); ++i)
        {
            for (const Hull& h : hulls)
                for (auto& ms : parts[i].model_slots)
                    if (ms == h.slot) volume[i] += h.volume;
            if (volume[i] <= 0 && has_mesh(parts[i].comp)) ++empty_parts;
        }
        double share_target = (std::max)(0.05, parts_target - token_kg * empty_parts);
        // Each part's share: its pieces' volume x density (the handle wood and leather, the rest steel; the
        // converter's estimate, model_to_weapon.py STEEL/GRIP/HAFT), then shifted between the parts so the
        // weapon balances where the converter says ("balance", cm above the top of the grip: the model's own
        // balance unless the user typed one). Masses m_i at heights z_i become m_i * exp(k (z_i - b)), k found
        // by bisection so that the centre of mass is at b (as close as the parts allow).
        std::vector<double> weight(parts.size(), 0.0), zc(parts.size(), 0.0);
        {
            double grip_density = (model_hafted ? 0.75 : 1.0) / 7.85;
            for (size_t i = 0; i < parts.size(); ++i)
            {
                double wz = 0;
                for (const Hull& h : hulls)
                    for (auto& ms : parts[i].model_slots)
                        if (ms == h.slot)
                        {
                            double w = h.volume * (h.slot == "Grip" ? grip_density : 1.0);
                            weight[i] += w;
                            wz += w * hull_centroid_z(h);
                        }
                if (weight[i] > 0) zc[i] = wz / weight[i];
            }
            // Swords: the weight shared between the parts as the carrier sword shares it (Matthew's test
            // 2026-11-22: by volume the guard got 4x the game's and throws felt wrong), then balanced below.
            if (!model_hafted && !one_piece)
            {
                double game_sum = 0;
                for (size_t i = 0; i < parts.size(); ++i)
                    if (weight[i] > 0) game_sum += game_kg[i];
                bool all = game_sum > 0.05;
                if (all)
                    for (size_t i = 0; i < parts.size(); ++i)
                        if (weight[i] > 0) weight[i] = (std::max)(game_kg[i], 1e-4);
                log("  part weights: %s", all ? "the carrier sword's spread" : "by volume (the carrier's parts have no mass settings)");
            }
            double wsum = 0, wz = 0;
            for (size_t i = 0; i < parts.size(); ++i)
            {
                wsum += weight[i];
                wz += weight[i] * zc[i];
            }
            if (wsum > 0 && numbers.count("balance"))
            {
                double b = numbers["balance"];
                double span = (std::max)(1.0, tip - numbers["pommel_end"]);
                auto moment = [&](double k) {
                    double m = 0;
                    for (size_t i = 0; i < parts.size(); ++i)
                        if (weight[i] > 0) m += weight[i] * std::exp(k * (zc[i] - b) / span) * (zc[i] - b);
                    return m;
                };
                double lo = -12, hi = 12;
                for (int it = 0; it < 60; ++it)
                {
                    double mid = 0.5 * (lo + hi);
                    (moment(mid) > 0 ? hi : lo) = mid;
                }
                double k = 0.5 * (lo + hi);
                double after = 0, before_z = wz / wsum;
                for (size_t i = 0; i < parts.size(); ++i)
                    if (weight[i] > 0) weight[i] *= std::exp(k * (zc[i] - b) / span);
                double nsum = 0, nz = 0;
                for (size_t i = 0; i < parts.size(); ++i)
                {
                    nsum += weight[i];
                    nz += weight[i] * zc[i];
                }
                after = nz / nsum;
                log("  balance: parts give %.1f cm, wanted %.1f cm, now %.1f cm above the grip", before_z, b, after);
            }
        }
        double weight_all = 0;
        for (double w : weight) weight_all += w;

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
            for (const Hull& h : hulls)
            {
                bool mine = false;
                for (auto& ms : pt.model_slots) mine = mine || ms == h.slot;
                if (!mine) continue;
                std::vector<V3> local;
                for (V3 p : h.points) local.push_back(to_slot(p));
                pieces.push_back(local);
            }
            float kg = weight_all > 0 ? (float)(share_target * weight[i] / weight_all) : 0.0f;
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
        // (The whole weapon's mass is read in measure_weapon, once the swapped parts are welded again.)

        // Damage points (Tip/Base scenes, damage boxes) on the head move with it: the game head's length
        // [olo, ohi] maps onto the model's head [head start, tip] (one piece: the whole model). Found by
        // parent, not by name: reforged weapons' tips are plain components (Tip1, Tip2, Box), modular
        // ones are NODE_AddSceneComponent copies.
        double from_lo = olo.z, from_hi = ohi.z;
        double to_lo = ((one_piece ? numbers["pommel_end"] : guard_top) + shift) / hs.z, to_hi = (tip + shift) / hs.z;
        for (UObject* c : children_of(head))
        {
            std::wstring cls = c->GetClassPrivate()->GetName();
            if (cls.find(STR("ProceduralMesh")) != std::wstring::npos || cls.find(STR("StaticMesh")) != std::wstring::npos) continue;
            FProperty* rp = c->GetPropertyByNameInChain(STR("RelativeLocation"));
            if (!rp || from_hi - from_lo < 1.0) continue;
            V3 r = read_three(reinterpret_cast<uint8_t*>(c) + rp->GetOffset_Internal(), rp);
            double z = to_lo + (r.z - from_lo) * (to_hi - to_lo) / (from_hi - from_lo);
            if (cls.find(STR("BoxComponent")) != std::wstring::npos)
            {
                // The hit box (dismembering cuts gather their markers with it) stretches with the head.
                FProperty* ep = c->GetPropertyByNameInChain(STR("BoxExtent"));
                if (ep)
                {
                    V3 e = read_three(reinterpret_cast<uint8_t*>(c) + ep->GetOffset_Internal(), ep);
                    double ez = e.z * (to_hi - to_lo) / (from_hi - from_lo);
                    Call ext(c, STR("SetBoxExtent"));
                    ext.three(STR("InBoxExtent"), e.x, e.y, ez).flag(STR("bUpdateOverlaps"), false);
                    ext.run();
                    log("  hit box %s: extent z %.1f -> %.1f", narrow(c->GetName()).c_str(), e.z, ez);
                }
            }
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
            for (UObject* c : children_of(pt.comp))
            {
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

        // The model's own damage points, when the converter wrote them.
        if (!data->points.empty())
        {
            static FName tip_tag(STR("Tip"), FNAME_Add);
            static FName off_tag(STR("CW_Tip_Off"), FNAME_Add);
            static FName base_tag(STR("Base"), FNAME_Add);
            static FName base_off_tag(STR("CW_Base_Off"), FNAME_Add);
            std::map<std::string, UObject*> parent_of;
            int untagged = 0;
            for (Part& pt : parts)
            {
                if (!has_mesh(pt.comp)) continue;
                for (auto& ms : pt.model_slots)
                    if (!parent_of.count(ms)) parent_of[ms] = pt.comp;
                std::vector<UObject*> all;
                descendants(pt.comp, all);
                for (UObject* c : all) untagged += retag(c, tip_tag, off_tag);
            }
            // A part that gets the model's own Base loses the carrier's (the last direct child tagged Base wins,
            // and ours must be the one).
            for (const DPoint& d : data->points)
            {
                if (d.flags.empty() || d.flags[0] != "BASE") continue;
                auto it = parent_of.find(d.slot);
                if (it == parent_of.end()) continue;
                for (UObject* c : children_of(it->second)) retag(c, base_tag, base_off_tag);
            }
            // Model cm -> world: the model's frame in the head's local units (as the visible model).
            auto world_of = [&](V3 p) {
                p.z += shift;
                return transform_point(head_xf, to_head_local(p), false);
            };
            int added = 0;
            for (const DPoint& d : data->points)
            {
                auto it = parent_of.find(d.slot);
                if (it == parent_of.end()) it = parent_of.find("Head");
                if (it == parent_of.end()) continue;
                V3 w = world_of(d.p);
                V3 wf = sub(world_of(V3{d.p.x + d.f.x, d.p.y + d.f.y, d.p.z + d.f.z}), w);
                V3 wu = sub(world_of(V3{d.p.x + d.u.x, d.p.y + d.u.y, d.p.z + d.u.z}), w);
                wchar_t buf[32];
                if (!d.flags.empty() && d.flags[0] == "BASE")
                {
                    if (add_damage_point(weapon, it->second, w, unit(wf), unit(wu), {base_tag})) ++added;
                    continue;
                }
                std::vector<FName> tags{tip_tag};
                swprintf(buf, 32, L"sTip%03d", d.sTip);
                tags.emplace_back(buf, FNAME_Add);
                swprintf(buf, 32, L"sEdge%03d", d.sEdge);
                tags.emplace_back(buf, FNAME_Add);
                swprintf(buf, 32, L"dSharp%d", d.dSharp);
                tags.emplace_back(buf, FNAME_Add);
                swprintf(buf, 32, L"dBlunt%d", d.dBlunt);
                tags.emplace_back(buf, FNAME_Add);
                swprintf(buf, 32, L"rig%03d", d.rig);
                tags.emplace_back(buf, FNAME_Add);
                for (auto& fl : d.flags) tags.emplace_back(widen(fl).c_str(), FNAME_Add);
                if (add_damage_point(weapon, it->second, w, unit(wf), unit(wu), tags)) ++added;
            }
            log("  damage points: %d of the carrier's untagged, %d of the model's added", untagged, added);
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
        if (proc) apply_own_textures(weapon, proc, material, base);
        g_built[weapon] = Built{head, hs, shift, numbers};
        fit_hands(held ? player_pawn() : nullptr, weapon, head_xf, hs, shift, numbers);
        log(proc ? "Done." : "  ! visual model failed (collision swap above still applies)");
        return true;
    }

    // ---- converted weapons added to the game: carrier passports -------------------------------------
    // Matthew 2026-11-22: converted weapons are NEW weapons in free play (and progression), not a game
    // weapon reshaped with F9. Decompile notes: defaults\add_new_weapons.md, carrier_passport_impl.md.
    // The game keeps every weapon as a passport (Str_Passport_Weapon1: weapon class, part classes, sizes,
    // Name, ID, Price, Tier). A converted weapon is a passport of a game weapon of the same kind (the
    // "carrier") whose Name is a marker, "cw <id>" ("cw sword <id>" for swords: Blossfechten keeps only
    // names with "sword" or "messer"; never "great"). When any weapon with a marker spawns (menu, trader,
    // loot, save), the converted model is put on it at the end of its BeginPlay, after the game built
    // the parts and before anyone reads them. The passport itself is never changed (the game saves and
    // compares it), so without this mod a saved converted weapon is simply the carrier: nothing is lost.
    enum Kind { K_DAGGER, K_ARMING, K_BASTARD, K_LONGSWORD, K_MACE, K_AXE2H, K_SPEAR, K_BUCKLER, K_TARGE, K_PAVISE, K_FLAIL, K_CROSSBOW, K_COUNT };
    const char* const kind_names[K_COUNT] = {"dagger", "arming", "bastard", "longsword", "mace", "axe2h", "spear",
                                             "shield_buckler", "shield_targe", "shield_pavise", "flail", "crossbow"};

    struct Pack
    {
        std::wstring id, dir, title, model; // dir ends with a slash; model: file name without .obj
        bool free_play = true, progression = false;
        int tier = 3;      // progression: the player's rank it needs (0 Beggar .. 7 Knight)
        int price = 0;     // progression: the merchant's asking price in crowns (0: by tier)
        std::string scabbard = "fit"; // fit, a game scabbard's short name (SS25 .. LS110), own, none
        int kind = K_ARMING;
        FName marker;
        int32_t hash = 1;
    };
    std::vector<Pack> g_packs;

    int kind_for(const std::map<std::string, std::string>& kv, const std::map<std::string, double>& nums)
    {
        auto c = kv.find("carrier");
        if (c != kv.end())
            for (int k = 0; k < K_COUNT; ++k)
                if (c->second == kind_names[k]) return k;
        auto num = [&](const char* k) { auto i = nums.find(k); return i == nums.end() ? 0.0 : i->second; };
        if (num("shield") > 0.5) return K_BUCKLER;
        if (num("flail") > 0.5) return K_FLAIL;
        if (num("crossbow") > 0.5) return K_CROSSBOW;
        bool hafted = num("hafted") > 0.5;
        double grip = -num("handle_bottom"), total = num("tip") - num("pommel_end");
        if (!hafted)
        {
            if (total < 55) return K_DAGGER;
            if (grip < g_settings.two_hand_grip_cm) return K_ARMING;
            return grip < 20 ? K_BASTARD : K_LONGSWORD;
        }
        int held = (int)std::lround(num("held")); // the converter's "Held with" choice
        if (held == 1) return K_MACE;
        if (held == 2) return K_AXE2H;
        if (held == 3) return K_SPEAR;
        if (num("double") > 0.5) return total < 90 ? K_MACE : K_SPEAR; // the lower head rides on the haft part
        if (total < 90) return K_MACE;
        return total < 160 ? K_AXE2H : K_SPEAR;
    }

    // Every weapons\<id>\pack.txt in the mod folder (written by WeaponConverter's Install).
    void load_packs()
    {
        std::vector<Pack> packs;
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((g_dir + L"weapons\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE)
        {
            g_packs.clear();
            return;
        }
        do
        {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
            Pack pk;
            pk.id = fd.cFileName;
            pk.dir = g_dir + L"weapons\\" + pk.id + L"\\";
            FILE* f = _wfopen((pk.dir + L"pack.txt").c_str(), L"r");
            if (!f) continue;
            std::map<std::string, std::string> kv;
            char line[512];
            while (fgets(line, sizeof line, f))
            {
                if (line[0] == ';' || line[0] == '#') continue;
                std::string l(line);
                size_t eq = l.find('=');
                if (eq == std::string::npos) continue;
                std::string v = l.substr(eq + 1);
                while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) v.pop_back();
                kv[l.substr(0, eq)] = v;
            }
            fclose(f);
            pk.title = widen(kv.count("title") ? kv["title"] : narrow(pk.id));
            std::string model = kv.count("model") ? kv["model"] : narrow(pk.id) + ".obj";
            if (model.size() > 4 && _stricmp(model.c_str() + model.size() - 4, ".obj") == 0) model.resize(model.size() - 4);
            pk.model = widen(model);
            pk.free_play = !kv.count("free_play") || kv["free_play"] != "0";
            pk.progression = kv.count("progression") && kv["progression"] == "1";
            if (kv.count("tier")) pk.tier = (std::max)(0, (std::min)(7, atoi(kv["tier"].c_str())));
            if (kv.count("price")) pk.price = (std::max)(0, atoi(kv["price"].c_str()));
            if (kv.count("scabbard")) pk.scabbard = kv["scabbard"];
            const ModelData* d = model_data(pk.dir + pk.model);
            if (!d)
            {
                log("Weapon %s: model %s.obj missing, skipped", narrow(pk.id).c_str(), model.c_str());
                continue;
            }
            pk.kind = kind_for(kv, d->numbers);
            bool sword = pk.kind <= K_LONGSWORD;
            uint32_t hsh = 2166136261u;
            for (wchar_t ch : pk.id) hsh = (hsh ^ (uint32_t)towlower(ch)) * 16777619u;
            pk.hash = (int32_t)(hsh & 0x7fffffff) | 1;
            // The marker is built from the hash, not the folder name: a name like "Greatsword" would put
            // "great" into it (Blossfechten drops those).
            wchar_t tag[16];
            swprintf(tag, 16, L"%08x", (unsigned)pk.hash);
            pk.marker = FName((std::wstring(sword ? L"cw sword " : L"cw ") + tag).c_str(), FNAME_Add);
            packs.push_back(pk);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        if (packs.size() != g_packs.size()) log("Converted weapons found: %zu", packs.size());
        for (const Pack& pk : packs)
        {
            bool known = false;
            for (const Pack& old : g_packs) known = known || old.id == pk.id;
            if (!known)
                log("  %s \"%s\": carried by a game %s; free play %s, progression %s", narrow(pk.id).c_str(), narrow(pk.title).c_str(),
                    kind_names[pk.kind], pk.free_play ? "yes" : "no", pk.progression ? "yes" : "no");
        }
        g_packs = std::move(packs);
    }

    // Passport layout, read from the game (member names carry GUID suffixes: matched by prefix).
    struct PassportLayout
    {
        UScriptStruct* type = nullptr;
        int32_t size = 0, name = -1, id = -1, weapon_class = -1, grip = -1, price = -1, tier = -1;
        FProperty *price_p = nullptr, *tier_p = nullptr, *id_p = nullptr;
    } g_pp;

    bool passport_layout()
    {
        if (g_pp.type) return true;
        auto* t = UObjectGlobals::StaticFindObject<UScriptStruct*>(nullptr, nullptr,
                                                                    STR("/Game/Blueprints/Structure/Passports/Str_Passport_Weapon1.Str_Passport_Weapon1"));
        if (!t) return false;
        PassportLayout l;
        l.type = t;
        l.size = t->GetStructureSize();
        for (FProperty* p : t->ForEachProperty())
        {
            std::wstring n = p->GetName();
            auto pre = [&](const wchar_t* x) { return n.rfind(x, 0) == 0; };
            if (pre(L"Name_")) l.name = p->GetOffset_Internal();
            else if (pre(L"ID_")) l.id = p->GetOffset_Internal(), l.id_p = p;
            else if (pre(L"WeaponClass_")) l.weapon_class = p->GetOffset_Internal();
            else if (pre(L"GripModule_")) l.grip = p->GetOffset_Internal();
            else if (pre(L"Price_")) l.price = p->GetOffset_Internal(), l.price_p = p;
            else if (pre(L"Tier_")) l.tier = p->GetOffset_Internal(), l.tier_p = p;
        }
        log("Weapon passport: %d bytes; Name @%d, ID @%d, WeaponClass @%d, GripModule @%d, Price @%d, Tier @%d", l.size, l.name, l.id,
            l.weapon_class, l.grip, l.price, l.tier);
        if (l.size <= 0 || l.name < 0 || l.id < 0 || l.weapon_class < 0 || l.grip < 0)
        {
            log("  ! passport members not found: converted weapons are off");
            return false;
        }
        g_pp = l;
        return true;
    }

    FName& passport_name(uint8_t* passport) { return *reinterpret_cast<FName*>(passport + g_pp.name); }
    UObject* passport_class(const uint8_t* passport, int32_t at) { return *reinterpret_cast<UObject* const*>(passport + at); }

    // The weapon passports inside a Str_Inventory struct property (member WeaponPssports_6_..., sic).
    RawArray* weapon_passports(UObject* owner, const TCHAR* property)
    {
        FProperty* p = owner ? owner->GetPropertyByNameInChain(property) : nullptr;
        UScriptStruct* inv = struct_type(p);
        if (!inv) return nullptr;
        for (FProperty* m : inv->ForEachProperty())
            if (m->GetName().rfind(L"WeaponPssports_", 0) == 0)
                return reinterpret_cast<RawArray*>(reinterpret_cast<uint8_t*>(owner) + p->GetOffset_Internal() + m->GetOffset_Internal());
        return nullptr;
    }

    // Carrier passports, copied from the game's own free mode list (DA_FreeMode_Inventory_Tier_Knight):
    // arming sword (Grip_12_T2), bastard (Grip_15_T3), longsword (Grip_20_T4), mace (Mace_Grip_I_S),
    // great axe (ModularWeaponBP_Axe2H), spear (ModularWeaponBP_Spear_A). The dagger has no passport there:
    // a knife class alone (grip-less classes need only WeaponClass).
    std::vector<uint8_t> g_tmpl[K_COUNT];

    bool carrier_templates()
    {
        if (!g_tmpl[K_ARMING].empty()) return true;
        if (!passport_layout()) return false;
        UObject* da = load_asset(L"/Game/Blueprints/DataAssets/Equipment/FreeMode/DA_FreeMode_Inventory_Tier_Knight.DA_FreeMode_Inventory_Tier_Knight");
        RawArray* arr = weapon_passports(da, STR("Inventory"));
        if (!arr || !arr->Data)
        {
            log("  ! free mode weapon list (DA_FreeMode_Inventory_Tier_Knight Inventory) not found");
            return false;
        }
        for (int32_t i = 0; i < arr->Num; ++i)
        {
            const uint8_t* pp = static_cast<const uint8_t*>(arr->Data) + (size_t)i * g_pp.size;
            UObject* cls = passport_class(pp, g_pp.weapon_class);
            UObject* grip = passport_class(pp, g_pp.grip);
            std::wstring c = cls ? cls->GetName() : L"", g = grip ? grip->GetName() : L"";
            int k = g == L"Weapon_Sword_Grip_12_T2_C"   ? K_ARMING
                    : g == L"Weapon_Sword_Grip_15_T3_C" ? K_BASTARD
                    : g == L"Weapon_Sword_Grip_20_T4_C" ? K_LONGSWORD
                    : g == L"Weapon_Part_Mace_Grip_I_S_C" ? K_MACE
                    : c == L"ModularWeaponBP_Axe2H_C"   ? K_AXE2H
                    : c == L"ModularWeaponBP_Spear_A_C" ? K_SPEAR
                    : c == L"Shield_Buckler_C"          ? K_BUCKLER
                    : c == L"Shield_Pavise_Light_C"     ? K_PAVISE
                    : c == L"ModularWeaponBP_Flail_A_C" ? K_FLAIL
                                                        : -1;
            if (k >= 0 && g_tmpl[k].empty()) g_tmpl[k].assign(pp, pp + g_pp.size);
        }
        if (UObject* knife = load_asset(L"/Game/Assets/Weapons/Blueprints/Built_Weapons/Tools/BP_Weapon_Tool_Knife_A.BP_Weapon_Tool_Knife_A_C"))
        {
            g_tmpl[K_DAGGER].assign(g_pp.size, 0);
            *reinterpret_cast<UObject**>(g_tmpl[K_DAGGER].data() + g_pp.weapon_class) = knife;
        }
        // The crossbow is in neither free mode list: a grip-less carrier, so a zero passport with its class.
        if (UObject* xbow = load_asset(L"/Game/Assets/Weapons/Blueprints/Built_Weapons/Ranged/Weapon/BP_Weapon_Ranged_Weapon_Crossbow_Light.BP_Weapon_Ranged_Weapon_Crossbow_Light_C"))
        {
            g_tmpl[K_CROSSBOW].assign(g_pp.size, 0);
            *reinterpret_cast<UObject**>(g_tmpl[K_CROSSBOW].data() + g_pp.weapon_class) = xbow;
        }
        // The targe has no passport in that list: a copy of a shield passport with its class (shields have
        // no part classes, add_new_weapons.md §7).
        const std::vector<uint8_t>& shield = !g_tmpl[K_PAVISE].empty() ? g_tmpl[K_PAVISE] : g_tmpl[K_BUCKLER];
        if (!shield.empty())
            if (UObject* targe = load_asset(L"/Game/Assets/Weapons/Blueprints/Built_Weapons/Shield_Tagre.Shield_Tagre_C"))
            {
                g_tmpl[K_TARGE] = shield;
                *reinterpret_cast<UObject**>(g_tmpl[K_TARGE].data() + g_pp.weapon_class) = targe;
            }
        std::string have;
        for (int k = 0; k < K_COUNT; ++k) have += std::string(" ") + kind_names[k] + (g_tmpl[k].empty() ? ":MISSING" : ":ok");
        log("Carrier passports from the game:%s", have.c_str());
        return !g_tmpl[K_ARMING].empty();
    }

    bool make_passport(const Pack& pk, std::vector<uint8_t>& out)
    {
        if (!carrier_templates()) return false;
        int k = pk.kind;
        if (g_tmpl[k].empty()) k = k <= K_LONGSWORD ? K_ARMING : k == K_FLAIL || k == K_CROSSBOW ? K_MACE : k >= K_BUCKLER ? (k == K_BUCKLER ? K_PAVISE : K_BUCKLER) : K_MACE; // nearest kind
        if (g_tmpl[k].empty()) return false;
        out = g_tmpl[k];
        passport_name(out.data()) = pk.marker;
        *reinterpret_cast<int32_t*>(out.data() + g_pp.id) = pk.hash;
        return true;
    }

    // A number into a passport member of whatever numeric type it has (int, byte/enum, float, double).
    void put_number(uint8_t* passport, FProperty* p, double v)
    {
        if (!p) return;
        uint8_t* at = passport + p->GetOffset_Internal();
        auto kind = p->GetClass().GetName();
        if (kind == STR("IntProperty")) *reinterpret_cast<int32_t*>(at) = (int32_t)std::lround(v);
        else if (kind == STR("Int64Property")) *reinterpret_cast<int64_t*>(at) = (int64_t)std::llround(v);
        else if (kind == STR("FloatProperty")) *reinterpret_cast<float*>(at) = (float)v;
        else if (kind == STR("DoubleProperty")) *reinterpret_cast<double*>(at) = v;
        else if (kind == STR("ByteProperty") || kind == STR("EnumProperty")) *at = (uint8_t)std::lround(v);
        else log("  ! passport member %s is a %s: not written", narrow(p->GetName()).c_str(), narrow(kind).c_str());
    }

    const Pack* pack_for(FName marker)
    {
        for (const Pack& pk : g_packs)
            if (pk.marker == marker) return &pk;
        return nullptr;
    }

    // GI_Settings "Set Up Free Mode Inventory" rebuilds the free mode list from the data asset plus the
    // player's Insured Items each time the inventory screen opens (never saved); the converted weapons
    // are appended after it, unless the same weapon is already there (insured).
    void free_mode_list_built(UnrealScriptFunctionCallableContext& context, void*)
    {
        UObject* gi = context.Context;
        load_packs();
        if (g_packs.empty() || !carrier_templates()) return;
        RawArray* arr = weapon_passports(gi, STR("Free Mode Player Inventory"));
        if (!arr)
        {
            log("  ! GI_Settings Free Mode Player Inventory weapons not found");
            return;
        }
        int added = 0;
        for (const Pack& pk : g_packs)
        {
            if (!pk.free_play) continue;
            bool there = false;
            for (int32_t i = 0; i < arr->Num && !there; ++i)
                there = passport_name(static_cast<uint8_t*>(const_cast<void*>(arr->Data)) + (size_t)i * g_pp.size) == pk.marker;
            std::vector<uint8_t> pp;
            if (there || !make_passport(pk, pp)) continue;
            int32_t n = arr->Num;
            if (n + 1 > arr->Max)
            {
                void* grown = FMemory::Realloc(const_cast<void*>(arr->Data), (size_t)(n + 4) * g_pp.size);
                if (!grown) break;
                arr->Data = grown;
                arr->Max = n + 4;
            }
            memcpy(static_cast<uint8_t*>(const_cast<void*>(arr->Data)) + (size_t)n * g_pp.size, pp.data(), g_pp.size);
            arr->Num = n + 1;
            ++added;
        }
        log("Free mode list: %d converted weapons added (list now %d)", added, arr->Num);
    }

    // ---- progression: the merchant (decompile: add_new_weapons.md "Steps 3, 4, 7 facts" §3) -----------
    // GI_Settings "Renew Merchant" rebuilds the merchant's weapons (Merchant Inventory) from the random pools
    // after fights, time skips, rank-ups and the Restock button; nothing filters by tier, so the mod does:
    // a converted weapon is offered when its tier <= the player's. Each offer gets a random ID (buying
    // compares whole passports: two identical ones would merge). The shop asks 2x the passport Price.
    // Old offers of ours are removed first, and our passports are taken out of the pools too (unsold
    // weapons with a grip module go back there and could be drawn again as if the game's own).
    int number_of(UObject* owner, const TCHAR* name, bool& found)
    {
        FProperty* p = owner ? owner->GetPropertyByNameInChain(name) : nullptr;
        found = p != nullptr;
        if (!p) return 0;
        uint8_t* at = reinterpret_cast<uint8_t*>(owner) + p->GetOffset_Internal();
        auto kind = p->GetClass().GetName();
        if (kind == STR("IntProperty")) return *reinterpret_cast<int32_t*>(at);
        if (kind == STR("ByteProperty") || kind == STR("EnumProperty")) return *at;
        if (kind == STR("FloatProperty")) return (int)*reinterpret_cast<float*>(at);
        if (kind == STR("DoubleProperty")) return (int)*reinterpret_cast<double*>(at);
        found = false;
        return 0;
    }

    // Removes our passports (by marker) from a passport array; how many.
    int purge_ours(RawArray* arr)
    {
        if (!arr || !arr->Data) return 0;
        auto* base = static_cast<uint8_t*>(const_cast<void*>(arr->Data));
        int32_t w = 0, removed = 0;
        for (int32_t r = 0; r < arr->Num; ++r)
        {
            uint8_t* pp = base + (size_t)r * g_pp.size;
            if (passport_name(pp).ToString().rfind(STR("cw "), 0) == 0)
            {
                ++removed;
                continue;
            }
            if (w != r) memmove(base + (size_t)w * g_pp.size, pp, g_pp.size);
            ++w;
        }
        arr->Num = w;
        return removed;
    }

    // A plain passport array property (Array<Str_Passport_Weapon1>) on an object.
    RawArray* passport_array(UObject* owner, const TCHAR* name)
    {
        FProperty* p = owner ? owner->GetPropertyByNameInChain(name) : nullptr;
        if (!p || p->GetClass().GetName() != STR("ArrayProperty")) return nullptr;
        return reinterpret_cast<RawArray*>(reinterpret_cast<uint8_t*>(owner) + p->GetOffset_Internal());
    }

    bool append_passport(RawArray* arr, const std::vector<uint8_t>& pp)
    {
        int32_t n = arr->Num;
        if (n + 1 > arr->Max)
        {
            void* grown = FMemory::Realloc(const_cast<void*>(arr->Data), (size_t)(n + 4) * g_pp.size);
            if (!grown) return false;
            arr->Data = grown;
            arr->Max = n + 4;
        }
        memcpy(static_cast<uint8_t*>(const_cast<void*>(arr->Data)) + (size_t)n * g_pp.size, pp.data(), g_pp.size);
        arr->Num = n + 1;
        return true;
    }

    void merchant_renewed(UnrealScriptFunctionCallableContext& context, void*)
    {
        UObject* gi = context.Context;
        load_packs();
        if (!carrier_templates()) return;
        RawArray* stock = weapon_passports(gi, STR("Merchant Inventory"));
        if (!stock)
        {
            log("  ! GI_Settings Merchant Inventory weapons not found");
            return;
        }
        int old = purge_ours(stock);
        // The pools: on GI_Settings or the game manager, wherever the game keeps them.
        int pooled = 0;
        std::vector<UObject*> owners{gi};
        if (UObject* gm = UObjectGlobals::FindFirstOf(STR("BP_GameManager_C"))) owners.push_back(gm);
        for (UObject* o : owners)
            for (const TCHAR* pool : {STR("Available Weapons 1H"), STR("Available Weapons 2H"), STR("Available Shields")})
                pooled += purge_ours(passport_array(o, pool));
        bool have_tier = false;
        int player_tier = number_of(gi, STR("Player Tier"), have_tier);
        if (!have_tier)
            for (UObject* o : owners)
                if (!have_tier) player_tier = number_of(o, STR("Player Tier"), have_tier);
        if (!have_tier) log("  ! Player Tier not found: every progression weapon is offered");
        static uint32_t rng = (uint32_t)GetTickCount() | 1;
        int added = 0;
        for (const Pack& pk : g_packs)
        {
            if (!pk.progression || (have_tier && pk.tier > player_tier)) continue;
            std::vector<uint8_t> pp;
            if (!make_passport(pk, pp)) continue;
            rng ^= rng << 13, rng ^= rng >> 17, rng ^= rng << 5;
            put_number(pp.data(), g_pp.id_p, (double)(rng & 0x7fffffff | 1));
            int buy = pk.price > 0 ? pk.price : (int)(15 + 20 * pk.tier); // asking price
            put_number(pp.data(), g_pp.price_p, (std::max)(1, buy / 2));   // the shop asks 2x
            put_number(pp.data(), g_pp.tier_p, pk.tier);
            if (append_passport(stock, pp)) ++added;
        }
        log("Merchant renewed: %d converted weapons offered (player tier %d%s), %d old offers and %d pool copies removed",
            added, player_tier, have_tier ? "" : " unknown", old, pooled);
    }

    bool set_number(UObject* owner, const TCHAR* name, double value)
    {
        FProperty* p = owner ? owner->GetPropertyByNameInChain(name) : nullptr;
        if (!p) return false;
        uint8_t* at = reinterpret_cast<uint8_t*>(owner) + p->GetOffset_Internal();
        auto kind = p->GetClass().GetName();
        if (kind == STR("DoubleProperty")) *reinterpret_cast<double*>(at) = value;
        else if (kind == STR("FloatProperty")) *reinterpret_cast<float*>(at) = (float)value;
        else return false;
        return true;
    }

    // A point in one component's space to another's, as plain numbers (4 engine calls to set up, then
    // free): x' = o + X*x + Y*y + Z*z from where the component puts its origin and unit axes.
    struct Affine
    {
        V3 o{}, X{1, 0, 0}, Y{0, 1, 0}, Z{0, 0, 1};
        V3 apply(V3 p) const
        {
            return {o.x + X.x * p.x + Y.x * p.y + Z.x * p.z, o.y + X.y * p.x + Y.y * p.y + Z.y * p.z,
                    o.z + X.z * p.x + Y.z * p.y + Z.z * p.z};
        }
    };
    Affine affine_of(const Xf& x, bool inverse)
    {
        Affine a;
        a.o = transform_point(x, {0, 0, 0}, inverse);
        a.X = sub(transform_point(x, {1, 0, 0}, inverse), a.o);
        a.Y = sub(transform_point(x, {0, 1, 0}, inverse), a.o);
        a.Z = sub(transform_point(x, {0, 0, 1}, inverse), a.o);
        return a;
    }
    double dist(V3 a, V3 b)
    {
        V3 d = sub(a, b);
        return std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    }

    // What the game measured from the carrier's parts in its BeginPlay, measured again for the converted
    // weapon (decompile: weapon_damage_points.md §9, §10):
    // - Mass Default and Center of Mass: BaseMesh's whole welded body. SetStaticMesh unwelded the swapped
    //   parts and they only weld back while BaseMesh simulates, so (the game's own pattern) simulate, read,
    //   stop again (stopping does not unweld; BeginPlay's Delay(0) tail turns simulation on as usual).
    // - The lengths: the game sweeps a 100 cm sphere down BaseMesh's local z from +1000 and up from -1000
    //   onto the weapon's collision; the first contact is the point of the hulls that maximises
    //   z + sqrt(100^2 - r^2) (top) / minimises z - sqrt(...) (bottom), r = distance from the axis.
    void measure_weapon(UObject* actor, const std::wstring& model_base)
    {
        UObject* base = object_property(actor, STR("BaseMesh"));
        const ModelData* data = model_data(model_base);
        auto bi = g_built.find(actor);
        if (!base || !data || bi == g_built.end()) return;
        const Built& b = bi->second;

        Call sim_on(base, STR("SetSimulatePhysics"));
        sim_on.flag(STR("bSimulate"), true).run();
        float mass = mass_of(base);
        V3 com{};
        {
            Call c(base, STR("GetCenterOfMass"));
            FProperty* r = c.run() ? c.param(STR("ReturnValue")) : nullptr;
            if (r) com = read_three(c.buffer.data() + r->GetOffset_Internal(), r);
        }
        Call sim_off(base, STR("SetSimulatePhysics"));
        sim_off.flag(STR("bSimulate"), false).run();
        Xf base_xf = world_transform(base);
        V3 com_local = transform_point(base_xf, com, true);
        set_number(actor, STR("Mass Default"), mass);
        if (FProperty* p = actor->GetPropertyByNameInChain(STR("Center of Mass")))
            write_three(reinterpret_cast<uint8_t*>(actor) + p->GetOffset_Internal(), p, com_local.x, com_local.y, com_local.z);
        log("  whole weapon %.3f kg, centre of mass (%.1f %.1f %.1f) in BaseMesh space", mass, com_local.x, com_local.y, com_local.z);

        // Model cm -> world (as the model was placed on the head) -> BaseMesh local.
        Xf head_xf = world_transform(b.head);
        if (head_xf.bytes.empty() || base_xf.bytes.empty()) return;
        Affine to_world = affine_of(head_xf, false), to_base = affine_of(base_xf, true);
        V3 top_l{0, 0, 1}, bot_l{0, 0, -1};
        double top_s = 1 + 100, bot_s = -1 - 100; // the dummy's own sphere (r 1 at the origin)
        for (const Hull& h : data->hulls)
            for (V3 p : h.points)
            {
                p.z += b.shift;
                p = V3{p.x + b.off.x, p.y + b.off.y, p.z + b.off.z};
                V3 l = to_base.apply(to_world.apply(V3{p.x / b.hs.x, p.y / b.hs.y, p.z / b.hs.z}));
                double r2 = l.x * l.x + l.y * l.y;
                if (r2 > 100.0 * 100.0) continue;
                double cap = std::sqrt(100.0 * 100.0 - r2);
                if (l.z + cap > top_s) top_s = l.z + cap, top_l = l;
                if (l.z - cap < bot_s) bot_s = l.z - cap, bot_l = l;
            }
        Affine base_to_world = affine_of(base_xf, false);
        V3 T = base_to_world.apply(top_l), B = base_to_world.apply(bot_l);
        double len = dist(T, B);
        auto at = [&](const TCHAR* n, V3& out) {
            UObject* c = object_property(actor, n);
            if (c) out = world_location(c);
            return c != nullptr;
        };
        V3 R{}, L{}, LOff{}, RAlt{}, LAlt{};
        bool hr = at(STR("Grip R Hand Default Pos"), R), hl = at(STR("Grip L Hand Default Pos"), L),
             hoff = at(STR("Grip L Hand Off Hand"), LOff), hra = at(STR("Grip R Hand Alt Pos"), RAlt),
             hla = at(STR("Grip L Hand Alt Pos"), LAlt);
        set_number(actor, STR("Weapon Length"), len);
        if (len > 1e-3) set_number(actor, STR("AI Balance Point (temp)"), dist(B, com) / len);
        if (hr) set_number(actor, STR("Length to Bottom"), dist(B, R));
        double old_top_r = 0;
        if (FProperty* p = actor->GetPropertyByNameInChain(STR("Length to Top R Default")))
            old_top_r = *reinterpret_cast<double*>(reinterpret_cast<uint8_t*>(actor) + p->GetOffset_Internal());
        if (hr && old_top_r != 0) set_number(actor, STR("Length to Top R Default"), dist(T, R)); // shields keep 0
        if (hl) set_number(actor, STR("Length to Top L Default"), dist(T, L));
        set_number(actor, STR("Length to Bottom (From Pivot)"), dist(T, world_location(base)));
        if (hr && hoff) set_number(actor, STR("Length to Top 2H"), (std::min)(dist(T, R), dist(T, LOff)));
        if (hra && hla) set_number(actor, STR("Length to Top Alt"), (std::min)(dist(T, RAlt), dist(T, LAlt)));
        if (UObject* tippy = object_property(actor, STR("TippyTipScene")))
        {
            Call mv(tippy, STR("K2_SetWorldLocation"));
            mv.three(STR("NewLocation"), T.x, T.y, T.z).flag(STR("bTeleport"), true);
            mv.run();
        }
        log("  measured: length %.1f cm (top %.1f, bottom %.1f in BaseMesh z), balance %.2f of the length", len, top_l.z,
            bot_l.z, len > 1e-3 ? dist(B, com) / len : 0.0);
    }

    // ---- shields (step 7; decompile: add_new_weapons.md §7 "Shield frames") ----------------------------
    // Shield_* classes are ModularWeaponBP children with no part classes: one StaticMeshComponent (Buckler,
    // Targe, Pavise) is both the visible plate and the blocking body, a Box child is the hit box, Tip/Base
    // children hold the damage tags. Its local frame: front = +Y for all three (the arm is on -Y), height Z,
    // width X. The converter's shield frame (model_to_weapon.py --type shield) is the same: z height,
    // x width, +y front, origin in the middle of its outline. The model goes in with its back where the
    // game plate's back is and its middle where the game plate's middle is, so the game's grip scenes and
    // arm joint (5 hand / 6 forearm) still fit.
    struct ShieldFrame
    {
        const TCHAR* comp;
        double back_y, mid_x, mid_z; // plate's back (min y), middle of its outline, component space
    };
    const ShieldFrame* shield_frame(int kind)
    {
        static const ShieldFrame buckler{STR("Buckler"), 4.79, 0.012, -0.002}, targe{STR("Targe"), -0.04, -0.212, 2.512},
                pavise{STR("Pavise"), -2.08, 0.0, -1.085};
        return kind == K_BUCKLER ? &buckler : kind == K_TARGE ? &targe : kind == K_PAVISE ? &pavise : nullptr;
    }

    bool apply_shield(UObject* actor, const Pack& pk)
    {
        const ShieldFrame* fr = shield_frame(pk.kind);
        UObject* comp = fr ? object_property(actor, fr->comp) : nullptr;
        if (!comp || !has_mesh(comp))
        {
            log("  ! shield component %s not found on %s", fr ? narrow(fr->comp).c_str() : "?", name_of(actor).c_str());
            return false;
        }
        const ModelData* data = model_data(pk.dir + pk.model);
        if (!data || data->hulls.empty()) return false;
        auto numbers = data->numbers;
        Model model = data->model;

        // Where the model goes: its back on the plate's back. A buckler model whose middle stands out behind
        // its rim by more than 2 cm has its own handle there: that goes on the game's grip bar (y -0.5).
        double ymin = 1e30, xlo = 1e30, xhi = -1e30, zlo = 1e30, zhi = -1e30, ylo_mid = 1e30, ylo_rim = 1e30;
        for (const V3& p : model.positions)
        {
            ymin = (std::min)(ymin, p.y);
            xlo = (std::min)(xlo, p.x), xhi = (std::max)(xhi, p.x), zlo = (std::min)(zlo, p.z), zhi = (std::max)(zhi, p.z);
        }
        double hx = (std::max)(1e-3, 0.5 * (xhi - xlo)), hz = (std::max)(1e-3, 0.5 * (zhi - zlo));
        for (const V3& p : model.positions)
        {
            double r = std::sqrt(std::pow((p.x - 0.5 * (xlo + xhi)) / hx, 2) + std::pow((p.z - 0.5 * (zlo + zhi)) / hz, 2));
            if (r < 0.3) ylo_mid = (std::min)(ylo_mid, p.y);
            if (r > 0.8) ylo_rim = (std::min)(ylo_rim, p.y);
        }
        double back = fr->back_y;
        bool own_handle = pk.kind == K_BUCKLER && ylo_mid < 1e29 && ylo_rim < 1e29 && ylo_rim - ylo_mid > 2.0;
        if (own_handle) back = -0.5;
        V3 off{fr->mid_x - 0.5 * (xlo + xhi), back - ymin, fr->mid_z - 0.5 * (zlo + zhi)};

        V3 ss{1, 1, 1};
        {
            Call sc(comp, STR("K2_GetComponentScale"));
            FProperty* r = sc.run() ? sc.param(STR("ReturnValue")) : nullptr;
            if (r) ss = read_three(sc.buffer.data() + r->GetOffset_Internal(), r);
            if (ss.x == 0 || ss.y == 0 || ss.z == 0) ss = {1, 1, 1};
        }
        auto local = [&](V3 p) { return V3{(p.x + off.x) / ss.x, (p.y + off.y) / ss.y, (p.z + off.z) / ss.z}; };
        Xf cxf = world_transform(comp);
        if (cxf.bytes.empty()) return false;
        auto world = [&](V3 p) { return transform_point(cxf, local(p), false); };
        log("Shield %s on %s: %.0f x %.0f cm, offset (%.1f %.1f %.1f)%s", narrow(pk.id).c_str(), narrow(fr->comp).c_str(), xhi - xlo,
            zhi - zlo, off.x, off.y, off.z, own_handle ? ", own handle on the grip bar" : "");

        // Collision and mass.
        UObject* base_mesh = object_property(actor, STR("BaseMesh"));
        float part_kg = 0;
        body_mass(comp, false, part_kg);
        double whole = mass_of(base_mesh), base_share = whole - part_kg;
        double mass_total = numbers.count("mass_kg") ? numbers["mass_kg"] : 3.0;
        double kg_d = mass_total - base_share;
        if (base_share < 0 || kg_d < 0.25 * mass_total) kg_d = mass_total;
        std::vector<std::vector<V3>> pieces;
        for (const Hull& h : data->hulls)
        {
            std::vector<V3> pts;
            for (V3 p : h.points) pts.push_back(local(p));
            pieces.push_back(pts);
        }
        Call get_mat(comp, STR("GetMaterial"));
        get_mat.i32(STR("ElementIndex"), 0);
        UObject* material = get_mat.run() ? get_mat.ret_object() : nullptr;
        UObject* mesh = build_part_mesh(actor, comp, pieces);
        if (!mesh) return false;
        float kg = (float)kg_d;
        body_mass(comp, true, kg);
        Call set(comp, STR("SetStaticMesh"));
        set.object(STR("NewMesh"), mesh).run();
        hide(comp);
        log("  %s: %zu collision pieces, mass setting %.2f kg (BaseMesh keeps %.2f)", narrow(fr->comp).c_str(), pieces.size(), kg, base_share);

        // The hit box around the model; damage points: the carrier's untagged, the model's added.
        static FName tip_tag(STR("Tip"), FNAME_Add), off_tag(STR("CW_Tip_Off"), FNAME_Add), base_tag(STR("Base"), FNAME_Add),
                base_off_tag(STR("CW_Base_Off"), FNAME_Add);
        V3 mid = local(V3{0.5 * (xlo + xhi), 0, 0.5 * (zlo + zhi)});
        double ymax = -1e30;
        for (const V3& p : model.positions) ymax = (std::max)(ymax, p.y);
        V3 ylo = local(V3{0, ymin, 0}), yhi = local(V3{0, ymax, 0});
        for (UObject* c : children_of(comp))
        {
            std::wstring cls = c->GetClassPrivate()->GetName();
            if (cls.find(STR("BoxComponent")) == std::wstring::npos) continue;
            Call ext(c, STR("SetBoxExtent"));
            ext.three(STR("InBoxExtent"), hx / ss.x + 0.5, 0.5 * (yhi.y - ylo.y) + 0.5, hz / ss.z + 0.5).flag(STR("bUpdateOverlaps"), false);
            ext.run();
            Call mv(c, STR("K2_SetRelativeLocation"));
            mv.three(STR("NewLocation"), mid.x, 0.5 * (ylo.y + yhi.y), mid.z).flag(STR("bTeleport"), true);
            mv.run();
            log("  hit box %s fitted to the model", narrow(c->GetName()).c_str());
        }
        if (!data->points.empty())
        {
            int untagged = 0, added = 0;
            std::vector<UObject*> all;
            descendants(comp, all);
            for (UObject* c : all) untagged += retag(c, tip_tag, off_tag);
            bool own_base = false;
            for (const DPoint& d : data->points) own_base = own_base || (!d.flags.empty() && d.flags[0] == "BASE");
            if (own_base)
                for (UObject* c : children_of(comp)) retag(c, base_tag, base_off_tag);
            for (const DPoint& d : data->points)
            {
                V3 w = world(d.p);
                V3 wf = unit(sub(world(V3{d.p.x + d.f.x, d.p.y + d.f.y, d.p.z + d.f.z}), w));
                V3 wu = unit(sub(world(V3{d.p.x + d.u.x, d.p.y + d.u.y, d.p.z + d.u.z}), w));
                if (!d.flags.empty() && d.flags[0] == "BASE")
                {
                    if (add_damage_point(actor, comp, w, wf, wu, {base_tag})) ++added;
                    continue;
                }
                std::vector<FName> tags{tip_tag};
                wchar_t buf[32];
                swprintf(buf, 32, L"sTip%03d", d.sTip), tags.emplace_back(buf, FNAME_Add);
                swprintf(buf, 32, L"sEdge%03d", d.sEdge), tags.emplace_back(buf, FNAME_Add);
                swprintf(buf, 32, L"dSharp%d", d.dSharp), tags.emplace_back(buf, FNAME_Add);
                swprintf(buf, 32, L"dBlunt%d", d.dBlunt), tags.emplace_back(buf, FNAME_Add);
                swprintf(buf, 32, L"rig%03d", d.rig), tags.emplace_back(buf, FNAME_Add);
                for (auto& fl : d.flags) tags.emplace_back(widen(fl).c_str(), FNAME_Add);
                if (add_damage_point(actor, comp, w, wf, wu, tags)) ++added;
            }
            log("  damage points: %d of the carrier's untagged, %d of the model's added", untagged, added);
        }

        // The visible model on the component (normals: inverse transpose of the scale).
        for (V3& p : model.positions) p = local(p);
        for (V3& n : model.normals) n = unit(V3{n.x * ss.x, n.y * ss.y, n.z * ss.z});
        UObject* proc = build_visual(actor, comp, model, material);
        if (proc) apply_own_textures(actor, proc, material, pk.dir + pk.model);
        Built b;
        b.head = comp, b.hs = ss, b.shift = 0, b.numbers = numbers, b.off = off, b.shield = true;
        g_built[actor] = b;
        log(proc ? "Done." : "  ! visual model failed (collision above still applies)");
        return true;
    }

    // ---- flails (step 6; decompile: custom_weapon_hafted.md §6) --------------------------------------
    // Carrier ModularWeaponBP_Flail_A: `Grip` (the staff, welded to BaseMesh), `Link 1` (the chain, its own
    // simulating body), `Head` (simulating; connector) with `Blade1` under it (the head: hit binding, Tips,
    // Base, most of the mass). Joints: `PhysicsConstraint` on Grip (Link 1 <-> Grip, at the staff's top) and
    // `PhysicsConstraint1` on Link 1 (Head <-> Link 1, at the chain's end). The converter lays the flail out
    // straight (model_to_weapon.py --type flail): staff z <= 0, chain 0..guard_top, head beyond, and writes
    // one visual per part (<model>_flail_Grip/Link/Head.obj). The model's staff top goes on the first joint,
    // so that joint stays; the second joint is moved to the model's chain end and both are re-initialised
    // (SetConstrainedComponents) because a mesh swap recreates the bodies.
    void flail_rejoin(UObject* actor)
    {
        UObject* grip = object_property(actor, STR("Grip"));
        UObject* link = object_property(actor, STR("Link 1"));
        UObject* headc = object_property(actor, STR("Head"));
        UObject* pc0 = object_property(actor, STR("PhysicsConstraint"));
        UObject* pc1 = object_property(actor, STR("PhysicsConstraint1"));
        if (!grip || !link || !headc || !pc0 || !pc1) return;
        Call j0(pc0, STR("SetConstrainedComponents"));
        j0.object(STR("Component1"), link).object(STR("Component2"), grip).run();
        Call j1(pc1, STR("SetConstrainedComponents"));
        j1.object(STR("Component1"), headc).object(STR("Component2"), link).run();
        log("  flail joints made again after the weld");
    }

    bool apply_flail(UObject* actor, const Pack& pk)
    {
        UObject* grip = object_property(actor, STR("Grip"));
        UObject* link = object_property(actor, STR("Link 1"));
        UObject* headc = object_property(actor, STR("Head"));
        UObject* blade = object_property(actor, STR("Blade1"));
        UObject* pc0 = object_property(actor, STR("PhysicsConstraint"));
        UObject* pc1 = object_property(actor, STR("PhysicsConstraint1"));
        if (!grip || !link || !headc || !blade || !pc0 || !pc1)
        {
            log("  ! flail carrier %s lacks Grip/Link 1/Head/Blade1/PhysicsConstraint(1)", name_of(actor).c_str());
            return false;
        }
        const ModelData* data = model_data(pk.dir + pk.model);
        if (!data || data->hulls.empty()) return false;
        auto numbers = data->numbers;
        double chain = numbers["guard_top"];

        V3 gs{1, 1, 1};
        {
            Call sc(grip, STR("K2_GetComponentScale"));
            FProperty* r = sc.run() ? sc.param(STR("ReturnValue")) : nullptr;
            if (r) gs = read_three(sc.buffer.data() + r->GetOffset_Internal(), r);
            if (gs.x == 0 || gs.y == 0 || gs.z == 0) gs = {1, 1, 1};
        }
        Xf gxf = world_transform(grip);
        if (gxf.bytes.empty()) return false;
        Affine G = affine_of(gxf, false), Ginv = affine_of(gxf, true);
        // The model's staff bottom on the game staff's bottom (the game's hand spots stay on it: Matthew's test
        // 2026-11-22 had all five clamped to the bottom when the top was put on the joint); the first joint
        // moves to the model's staff top.
        V3 pl = Ginv.apply(world_location(pc0));
        V3 glo{}, ghi{};
        bool have_box = mesh_box(object_property(grip, STR("StaticMesh")), glo, ghi);
        double off_z = have_box ? glo.z * gs.z - numbers["pommel_end"] : pl.z * gs.z;
        V3 off{pl.x * gs.x, pl.y * gs.y, off_z}; // model cm + off = Grip local cm
        auto world = [&](V3 p) { return G.apply(V3{(p.x + off.x) / gs.x, (p.y + off.y) / gs.y, (p.z + off.z) / gs.z}); };
        log("Flail %s: staff %.1f cm, chain %.1f cm, head to %.1f cm; staff top on the first joint (Grip z %.1f)", narrow(pk.id).c_str(),
            -numbers["pommel_end"], chain, numbers["tip"] - chain, pl.z);

        struct FPart
        {
            const char* slot;
            UObject* comp;
            double density; // relative to steel
        };
        FPart parts[] = {{"Grip", grip, 0.75 / 7.85}, {"Link", link, 1.0}, {"Head", blade, 1.0}};
        double w[3] = {0, 0, 0}, wsum = 0;
        for (int i = 0; i < 3; ++i)
        {
            for (const Hull& h : data->hulls)
                if (h.slot == parts[i].slot) w[i] += h.volume * parts[i].density;
            wsum += w[i];
        }
        UObject* base_mesh = object_property(actor, STR("BaseMesh"));
        float grip_was = 0;
        body_mass(grip, false, grip_was);
        double base_share = (std::max)(0.0, static_cast<double>(mass_of(base_mesh) - grip_was));
        const float connector_kg = 0.05f;
        double share = (std::max)(0.2, numbers["mass_kg"] - base_share - connector_kg);
        Call get_mat(grip, STR("GetMaterial"));
        get_mat.i32(STR("ElementIndex"), 0);
        UObject* staff_mat = get_mat.run() ? get_mat.ret_object() : nullptr;
        Call get_mat2(blade, STR("GetMaterial"));
        get_mat2.i32(STR("ElementIndex"), 0);
        UObject* head_mat = get_mat2.run() ? get_mat2.ret_object() : nullptr;

        for (int i = 0; i < 3; ++i)
        {
            FPart& fp = parts[i];
            Affine C = affine_of(world_transform(fp.comp), true);
            auto local = [&](V3 p) { return C.apply(world(p)); };
            std::vector<std::vector<V3>> pieces;
            for (const Hull& h : data->hulls)
                if (h.slot == fp.slot)
                {
                    std::vector<V3> pts;
                    for (V3 p : h.points) pts.push_back(local(p));
                    pieces.push_back(pts);
                }
            if (pieces.empty())
            {
                log("  ! flail: no %s pieces in the model", fp.slot);
                return false;
            }
            UObject* mesh = build_part_mesh(actor, fp.comp, pieces);
            if (!mesh) return false;
            float kg = (float)(wsum > 0 ? share * w[i] / wsum : share / 3);
            body_mass(fp.comp, true, kg);
            Call set(fp.comp, STR("SetStaticMesh"));
            set.object(STR("NewMesh"), mesh).run();
            hide(fp.comp);
            log("  %s (%s): %zu collision pieces, %.2f kg", narrow(fp.comp->GetName()).c_str(), fp.slot, pieces.size(), kg);

            Model m;
            std::wstring vis = pk.dir + pk.model + L"_flail_" + widen(fp.slot);
            if (!load_obj(vis + L".obj", m)) continue;
            V3 o = local(V3{0, 0, 0});
            for (V3& n : m.normals) n = unit(sub(local(n), o));
            for (V3& p : m.positions) p = local(p);
            UObject* mat = i == 0 ? staff_mat : head_mat;
            UObject* proc = build_visual(actor, fp.comp, m, mat);
            if (proc) apply_own_textures(actor, proc, mat, pk.dir + pk.model);
        }
        // Head: only the connector at the chain's end now.
        {
            Affine C = affine_of(world_transform(headc), true);
            std::vector<V3> cube;
            for (int k = 0; k < 8; ++k)
                cube.push_back(C.apply(world(V3{(k & 1) ? 0.5 : -0.5, (k & 2) ? 0.5 : -0.5, chain + ((k & 4) ? 0.5 : -0.5)})));
            UObject* mesh = build_part_mesh(actor, headc, {cube});
            if (mesh)
            {
                float kg = connector_kg;
                body_mass(headc, true, kg);
                Call set(headc, STR("SetStaticMesh"));
                set.object(STR("NewMesh"), mesh).run();
                hide(headc);
            }
        }
        // The joints: onto the staff's top and the chain's end. They are made again in flail_rejoin, after
        // measure_weapon's weld (a weld drops a joint made before it: Matthew's test 2026-11-22).
        {
            V3 t = world(V3{0, 0, 0}), e = world(V3{0, 0, chain});
            Call mv0(pc0, STR("K2_SetWorldLocation"));
            mv0.three(STR("NewLocation"), t.x, t.y, t.z).flag(STR("bTeleport"), true).run();
            Call mv(pc1, STR("K2_SetWorldLocation"));
            mv.three(STR("NewLocation"), e.x, e.y, e.z).flag(STR("bTeleport"), true).run();
            log("  joints placed: staff top (%.1f %.1f %.1f), chain end (%.1f %.1f %.1f)", t.x, t.y, t.z, e.x, e.y, e.z);
        }
        // Damage points: the carrier's untagged on every part, the model's added (Head -> Blade1, Grip -> Grip).
        if (!data->points.empty())
        {
            static FName tip_tag(STR("Tip"), FNAME_Add), off_tag(STR("CW_Tip_Off"), FNAME_Add), base_tag(STR("Base"), FNAME_Add),
                    base_off_tag(STR("CW_Base_Off"), FNAME_Add);
            int untagged = 0, added = 0;
            for (UObject* c0 : {grip, link, headc})
            {
                std::vector<UObject*> all{c0};
                descendants(c0, all);
                for (UObject* c : all) untagged += retag(c, tip_tag, off_tag);
            }
            for (UObject* c : children_of(blade)) retag(c, base_tag, base_off_tag);
            for (UObject* c : children_of(headc)) retag(c, base_tag, base_off_tag);
            for (const DPoint& d : data->points)
            {
                UObject* parent = d.slot == "Grip" ? grip : d.slot == "Link" ? link : blade;
                V3 wp_ = world(d.p);
                V3 wf = unit(sub(world(V3{d.p.x + d.f.x, d.p.y + d.f.y, d.p.z + d.f.z}), wp_));
                V3 wu = unit(sub(world(V3{d.p.x + d.u.x, d.p.y + d.u.y, d.p.z + d.u.z}), wp_));
                if (!d.flags.empty() && d.flags[0] == "BASE")
                {
                    if (add_damage_point(actor, parent, wp_, wf, wu, {base_tag})) ++added;
                    continue;
                }
                std::vector<FName> tags{tip_tag};
                wchar_t buf[32];
                swprintf(buf, 32, L"sTip%03d", d.sTip), tags.emplace_back(buf, FNAME_Add);
                swprintf(buf, 32, L"sEdge%03d", d.sEdge), tags.emplace_back(buf, FNAME_Add);
                swprintf(buf, 32, L"dSharp%d", d.dSharp), tags.emplace_back(buf, FNAME_Add);
                swprintf(buf, 32, L"dBlunt%d", d.dBlunt), tags.emplace_back(buf, FNAME_Add);
                swprintf(buf, 32, L"rig%03d", d.rig), tags.emplace_back(buf, FNAME_Add);
                for (auto& fl : d.flags) tags.emplace_back(widen(fl).c_str(), FNAME_Add);
                if (add_damage_point(actor, parent, wp_, wf, wu, tags)) ++added;
            }
            log("  damage points: %d of the carrier's untagged, %d of the model's added", untagged, added);
        }
        Built b;
        b.head = grip, b.hs = gs, b.shift = off.z, b.numbers = numbers, b.off = V3{off.x, off.y, 0};
        g_built[actor] = b;
        fit_hands(nullptr, actor, gxf, gs, off.z, numbers);
        log("Done.");
        return true;
    }

    // ---- the crossbow (step 9; decompile: add_new_weapons.md §8) -------------------------------------
    // BP_Weapon_Ranged_Weapon_Crossbow_Light shows a SkeletalMesh (animated string) over hidden physics
    // cubes that do the work: CM_Test_Stock (parent of the rest), the bow arms CM_Test_Shoulder_L/_R and the
    // string nut CM_Test_Peg on joints; the bolt is loaded at Scene_0 and shot along the stock's Up. So the
    // converted model is the look only: the skeletal mesh is hidden and the model rides on the stock; the
    // cubes, joints, masses and bolt stay the game's. Its frame comes from the crossbow itself: forward =
    // the stock's Up, across = left arm to right arm, up = towards the bolt seat; the model's bow middle
    // (converter origin; model_to_weapon.py --type crossbow: z front, y across, x up) on the arms' middle.
    bool apply_crossbow(UObject* actor, const Pack& pk)
    {
        UObject* stock = object_property(actor, STR("CM_Test_Stock"));
        UObject* sl = object_property(actor, STR("CM_Test_Shoulder_L"));
        UObject* sr = object_property(actor, STR("CM_Test_Shoulder_R"));
        UObject* seat = object_property(actor, STR("Scene_0"));
        UObject* skel = object_property(actor, STR("SkeletalMesh"));
        if (!stock || !sl || !sr || !seat)
        {
            log("  ! crossbow carrier %s lacks CM_Test_Stock / CM_Test_Shoulder_L/_R / Scene_0", name_of(actor).c_str());
            return false;
        }
        const ModelData* data = model_data(pk.dir + pk.model);
        if (!data) return false;
        auto dot = [](V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
        Xf sxf = world_transform(stock);
        Affine SW = affine_of(sxf, false), Sinv = affine_of(sxf, true);
        V3 F = unit(SW.Z);
        V3 L = world_location(sl), R = world_location(sr);
        V3 P{0.5 * (L.x + R.x), 0.5 * (L.y + R.y), 0.5 * (L.z + R.z)};
        V3 S = sub(R, L);
        double sf = dot(S, F);
        S = unit(V3{S.x - sf * F.x, S.y - sf * F.y, S.z - sf * F.z});
        V3 U = cross(S, F); // with x = up, y = across, z = forward: x = y cross z
        if (dot(sub(world_location(seat), world_location(stock)), U) < 0)
        {
            // The bolt seat is on the other side: keep the frame right-handed by turning it over round F.
            S = V3{-S.x, -S.y, -S.z};
            U = cross(S, F);
        }
        auto world = [&](V3 p) {
            return V3{P.x + U.x * p.x + S.x * p.y + F.x * p.z, P.y + U.y * p.x + S.y * p.y + F.y * p.z, P.z + U.z * p.x + S.z * p.y + F.z * p.z};
        };
        log("Crossbow %s: bow middle at the arms' middle, arms %.1f cm apart", narrow(pk.id).c_str(), dist(L, R));
        if (skel) hide(skel);
        Model m = data->model;
        V3 o = Sinv.apply(world(V3{0, 0, 0}));
        for (V3& n : m.normals) n = unit(sub(Sinv.apply(world(n)), o));
        for (V3& p : m.positions) p = Sinv.apply(world(p));
        Call get_mat(stock, STR("GetMaterial"));
        get_mat.i32(STR("ElementIndex"), 0);
        UObject* material = get_mat.run() ? get_mat.ret_object() : nullptr;
        if (skel)
        {
            Call gm(skel, STR("GetMaterial"));
            gm.i32(STR("ElementIndex"), 0);
            if (UObject* sm = gm.run() ? gm.ret_object() : nullptr) material = sm;
        }
        UObject* proc = build_visual(actor, stock, m, material);
        if (proc)
        {
            Call vis(proc, STR("SetVisibility"));
            vis.flag(STR("bNewVisibility"), true).flag(STR("bPropagateToChildren"), false).run();
            apply_own_textures(actor, proc, material, pk.dir + pk.model);
        }
        log(proc ? "Done." : "  ! visual model failed");
        return proc != nullptr;
    }

    // ---- the scabbard (step 4; decompile: add_new_weapons.md "Steps 3, 4, 7 facts" §4) ----------------
    // A sword's scabbard is the weapon actor's StaticMeshComponent "Scabbard" (from the blade module), laid
    // out in BaseMesh's frame (+Z to the tip, mouth at z 5.2). Sheathing needs SheathSlot 0 AND a valid
    // Scabbard, or the weapon is dropped; with no scabbard the weapon goes on the belt (SheathSlot 1) and
    // the variable must be None (the draw gate tests IsValid(Current Scabbard R)).
    // pack.txt scabbard=: fit (the shortest game scabbard that covers the blade), SS25..LS110 (that one),
    // own (<model>_scabbard.obj / _scabbard_collision.obj from the converter, already in the weapon's
    // model frame), none.
    struct GameScabbard
    {
        const char* name;
        double end_z; // the scabbard's far end, BaseMesh z (cm)
    };
    const GameScabbard g_scabbards[] = {{"SS25", 42.7}, {"SS45", 63.9}, {"AS60", 78.0}, {"AS70", 88.5},
                                        {"LS75", 94.7}, {"BS85", 104.4}, {"BS100", 115.8}, {"LS110", 127.8}};

    void apply_scabbard(UObject* actor, const Pack& pk)
    {
        UObject* scab = object_property(actor, STR("Scabbard"));
        UObject* base = object_property(actor, STR("BaseMesh"));
        auto bi = g_built.find(actor);
        uint8_t slot = 4;
        byte_property(actor, STR("SheathSlot"), false, slot);
        std::string want = pk.scabbard;
        if (want != "none" && !scab)
        {
            log("  scabbard: the carrier has no Scabbard component; the weapon goes on the belt instead");
            want = "none";
        }
        if (want != "none" && (bi == g_built.end() || !base)) want = "none";
        auto belt = [&](const char* why) {
            if (scab)
            {
                Call d(scab, STR("K2_DestroyComponent"));
                d.object(STR("Object"), actor).run();
                set_object_property(actor, STR("Scabbard"), nullptr);
            }
            uint8_t s = slot;
            if (s == 0) s = 1; // never slot 0 without a scabbard
            byte_property(actor, STR("SheathSlot"), true, s);
            log("  scabbard: none (%s); SheathSlot %d -> %d", why, slot, s);
        };
        if (want == "none")
        {
            belt("as chosen");
            return;
        }
        const Built& b = bi->second;
        const ModelData* data = model_data(pk.dir + pk.model);
        if (!data) return;
        // Model cm -> BaseMesh local (the scabbard's own layout frame).
        Xf head_xf = world_transform(b.head), base_xf = world_transform(base);
        if (head_xf.bytes.empty() || base_xf.bytes.empty()) return;
        Affine to_world = affine_of(head_xf, false), to_base = affine_of(base_xf, true);
        auto base_of = [&](V3 p) {
            p.z += b.shift;
            return to_base.apply(to_world.apply(V3{p.x / b.hs.x, p.y / b.hs.y, p.z / b.hs.z}));
        };
        auto num = [&](const char* k) { auto i = data->numbers.find(k); return i == data->numbers.end() ? 0.0 : i->second; };
        double tip_z = base_of(V3{0, 0, num("tip")}).z, blade_cm = num("tip") - num("guard_top");
        float kg = (float)(0.005 * (std::max)(10.0, blade_cm));

        // Own model (Matthew's test 2026-11-22: a solid hull round the blade blocked drawing and sheathing): the
        // body is the best-fitting game scabbard (its collision lets the blade in), the model is only the look.
        Model own;
        bool have_own = false;
        std::wstring sb = pk.dir + pk.model + L"_scabbard";
        if (want == "own")
        {
            have_own = load_obj(sb + L".obj", own);
            if (!have_own) log("  ! scabbard: %s_scabbard.obj missing; the game scabbard is shown", narrow(pk.model).c_str());
            want = "fit";
        }
        // A game scabbard: the named one, or the shortest whose end is at least 4 cm past the blade's tip.
        const GameScabbard* pick = nullptr;
        for (const GameScabbard& g : g_scabbards)
            if (want == g.name) pick = &g;
        double zscale = 1.0;
        if (!pick)
        {
            for (const GameScabbard& g : g_scabbards)
                if (!pick && g.end_z >= tip_z + 4.0) pick = &g;
            if (!pick)
            {
                pick = &g_scabbards[7]; // longer than any: the longest, stretched along its length
                zscale = (tip_z + 6.0) / pick->end_z;
            }
        }
        std::wstring nm = widen(pick->name);
        std::wstring path = L"/Game/Assets/Weapons/Swords/Meshes/Scabbards/Meshes/Equipment_Scabbard_A_" + nm + L"_001.Equipment_Scabbard_A_" + nm + L"_001";
        UObject* mesh = load_asset(path);
        if (!mesh)
        {
            log("  ! scabbard mesh %s not found", narrow(path).c_str());
            belt("game scabbard missing");
            return;
        }
        Call set(scab, STR("SetStaticMesh"));
        set.object(STR("NewMesh"), mesh).run();
        if (zscale != 1.0)
        {
            Call rs(scab, STR("SetRelativeScale3D"));
            rs.three(STR("NewScale3D"), 1.0, 1.0, zscale).run();
        }
        body_mass(scab, true, kg);
        uint8_t zero = 0;
        byte_property(actor, STR("SheathSlot"), true, zero);
        log("  scabbard: game %s (end z %.1f, blade tip z %.1f%s), %.2f kg, SheathSlot %d -> 0", pick->name, pick->end_z * zscale, tip_z,
            zscale != 1.0 ? ", stretched" : "", kg, slot);
        if (have_own)
        {
            V3 ss{1, 1, 1};
            {
                Call sc(scab, STR("K2_GetComponentScale"));
                FProperty* r = sc.run() ? sc.param(STR("ReturnValue")) : nullptr;
                if (r) ss = read_three(sc.buffer.data() + r->GetOffset_Internal(), r);
                if (ss.x == 0 || ss.y == 0 || ss.z == 0) ss = {1, 1, 1};
            }
            auto local = [&](V3 p) {
                V3 l = base_of(p);
                return V3{l.x / ss.x, l.y / ss.y, l.z / ss.z};
            };
            V3 o = base_of(V3{0, 0, -b.shift});
            for (V3& n : own.normals)
            {
                V3 w = sub(base_of(V3{n.x * b.hs.x * b.hs.x, n.y * b.hs.y * b.hs.y, n.z * b.hs.z * b.hs.z - b.shift}), o);
                n = unit(V3{w.x * ss.x, w.y * ss.y, w.z * ss.z});
            }
            for (V3& p : own.positions) p = local(p);
            Call get_mat(scab, STR("GetMaterial"));
            get_mat.i32(STR("ElementIndex"), 0);
            UObject* material = get_mat.run() ? get_mat.ret_object() : nullptr;
            hide(scab);
            UObject* proc = build_visual(actor, scab, own, material);
            if (proc) apply_own_textures(actor, proc, material, sb);
            log("  scabbard: own model shown on it (%s)", proc ? "ok" : "FAILED");
        }
    }

    // Every ModularWeaponBP after its BeginPlay (which built the parts from the passport).
    void weapon_begin_play(UObject* actor)
    {
        static UClass* weapon_class = nullptr;
        if (!weapon_class)
        {
            weapon_class = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Game/Assets/Weapons/Blueprints/ModularWeaponBP.ModularWeaponBP_C"));
            if (!weapon_class) return;
        }
        if (!actor || !actor->IsA(weapon_class)) return;
        g_built.erase(actor); // an old entry at a reused address
        if (!passport_layout()) return;
        uint8_t* pp = actor->GetValuePtrByPropertyNameInChain<uint8_t>(STR("Weapon Passport"));
        if (!pp) return;
        FName marker = passport_name(pp);
        const Pack* pk = pack_for(marker);
        if (!pk)
        {
            if (marker.ToString().rfind(STR("cw "), 0) != 0) return; // a game weapon
            load_packs();                                             // installed while the game runs?
            pk = pack_for(marker);
            if (!pk)
            {
                log("A converted weapon (%s) spawned but its folder is gone: it stays the game weapon it is carried by",
                    narrow(marker.ToString()).c_str());
                return;
            }
        }
        log("---- %s spawned: \"%s\" from weapons\\%s ----", name_of(actor).c_str(), narrow(pk->title).c_str(), narrow(pk->id).c_str());
        load_settings();
        double mass_was = g_settings.mass_kg;
        g_settings.mass_kg = 0; // the pack's own mass (from the converter)
        bool ok = pk->kind == K_CROSSBOW  ? apply_crossbow(actor, *pk)
                  : pk->kind == K_FLAIL   ? apply_flail(actor, *pk)
                  : pk->kind >= K_BUCKLER ? apply_shield(actor, *pk)
                                          : apply_full_weapon(actor, pk->dir + pk->model, false);
        g_settings.mass_kg = mass_was;
        if (!ok) return;
        // The name the trader and menus show (the passport keeps the marker).
        if (FName* n = actor->GetValuePtrByPropertyNameInChain<FName>(STR("Name"))) *n = FName(pk->title.c_str(), FNAME_Add);
        // Values the game measured from the old parts in BeginPlay (carrier_passport_impl.md b2).
        measure_weapon(actor, pk->dir + pk->model);
        if (pk->kind == K_FLAIL) flail_rejoin(actor);
        if (pk->kind <= K_LONGSWORD) apply_scabbard(actor, *pk);
        // The damage tags are scaled by the weapon's Quality (0 good x1.25, 1 x1, 2 low x0.85): 1 keeps the
        // converter's numbers as they are (weapon_damage_points.md).
        uint8_t quality = 1;
        if (!byte_property(actor, STR("Quality"), true, quality)) log("  ! could not set Quality");
    }

    // When the player takes a converted weapon, its hands are fitted (grip type, hand spots, slide).
    UObject* g_last_held = nullptr;
    double g_held_clock = 0;

    void held_tick(float dt)
    {
        g_held_clock += dt;
        if (g_held_clock < 0.1) return;
        g_held_clock = 0;
        UObject* pawn = player_pawn();
        UObject* weapon = object_property(pawn, STR("Weapon R"));
        if (weapon == g_last_held) return;
        g_last_held = weapon;
        auto it = weapon ? g_built.find(weapon) : g_built.end();
        if (it == g_built.end()) return;
        uint8_t* pp = weapon->GetValuePtrByPropertyNameInChain<uint8_t>(STR("Weapon Passport"));
        if (!pp || !pack_for(passport_name(pp))) return; // an old pointer reused by another weapon
        log("---- player took %s ----", name_of(weapon).c_str());
        Built& b = it->second;
        if (b.shield) return; // a shield's hands are its own grip scenes
        fit_hands(pawn, weapon, world_transform(b.head), b.hs, b.shift, b.numbers);
    }

    // The free mode list function is a Blueprint function called from Blueprint: hooked once it is loaded.
    bool g_free_mode_hooked = false;
    double g_hook_clock = 0;

    bool g_merchant_hooked = false;

    void hook_tick(float dt)
    {
        if (g_free_mode_hooked && g_merchant_hooked) return;
        g_hook_clock += dt;
        if (g_hook_clock < 1.0) return;
        g_hook_clock = 0;
        auto* fn = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr,
                                                                STR("/Game/Blueprints/GameInstances/GI_Settings.GI_Settings_C:Set Up Free Mode Inventory"));
        if (fn && !g_free_mode_hooked)
        {
            UObjectGlobals::RegisterHook(fn, [](UnrealScriptFunctionCallableContext&, void*) {}, &free_mode_list_built, nullptr);
            g_free_mode_hooked = true;
            log("Hooked GI_Settings Set Up Free Mode Inventory: converted weapons join the free mode list");
        }
        auto* renew = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr,
                                                                   STR("/Game/Blueprints/GameInstances/GI_Settings.GI_Settings_C:Renew Merchant"));
        if (renew && !g_merchant_hooked)
        {
            UObjectGlobals::RegisterHook(renew, [](UnrealScriptFunctionCallableContext&, void*) {}, &merchant_renewed, nullptr);
            g_merchant_hooked = true;
            log("Hooked GI_Settings Renew Merchant: progression weapons are offered by the merchant");
        }
    }

    void tick(UObject* engine, float dt)
    {
        g_engine = engine;
        hook_tick(dt);
        held_tick(dt);
        slide_tick(dt);
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
        Hook::RegisterBeginPlayPostCallback([](auto&, auto* actor) { weapon_begin_play(reinterpret_cast<UObject*>(actor)); },
                                            {false, false, STR("HalfSwordCustomWeapon"), STR("WeaponBeginPlay")});
        load_packs();
        log("Ready: converted weapons in weapons\\ are added to the game");
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
