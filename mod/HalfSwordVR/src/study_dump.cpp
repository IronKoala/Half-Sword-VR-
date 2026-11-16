// Study dump: writes the game's own logic out as text, to be read instead of guessed at.
//
// Matthew, 2026-11-16: "decompile the necessary code so you can understand it". The game's
// asset files are encrypted on disk, but the running game holds every Blueprint's compiled
// script (kismet bytecode) and every object's settings in memory. This file writes, into
// Mods\HalfSwordVR\study\:
//   <Class>.kismet.txt   every scripted function of a Blueprint class, disassembled with names
//   <Class>.vars.txt     the class's variables (name, type, offset) and its functions' parameters
//   objects_<level>.txt  the live settings of the player character, its components, its physics
//                        asset (bodies and joints) and held weapons
// Nothing in the game is changed. Each file is written once (an existing file is left alone).
//
// The bytecode layout follows the engine's own script reader for UE 5.4 (see the UE4SS
// KismetDebugger mod, which this walker is modelled on): object and field references are raw
// pointers, names are 12 bytes, code offsets 32 bits, vectors and rotators doubles.

#include "study_dump.hpp"

#include "config.hpp"
#include "log.hpp"

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/FProperty.hpp>
#include <Unreal/FString.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/Property/FStructProperty.hpp>
#include <Unreal/Script.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UScriptStruct.hpp>

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

using namespace RC;
using namespace RC::Unreal;

namespace hsvr
{
    namespace
    {
        std::string narrow(const std::wstring& text)
        {
            if (text.empty()) return {};
            int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
            std::string out((size_t)size, '\0');
            WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), size, nullptr, nullptr);
            return out;
        }

        std::wstring study_dir()
        {
            std::wstring dir = mod_dir() + L"study\\";
            CreateDirectoryW(dir.c_str(), nullptr);
            return dir;
        }

        std::wstring file_safe(std::wstring name)
        {
            for (wchar_t& ch : name)
            {
                if (!(iswalnum(ch) || ch == L'_' || ch == L'-' || ch == L'.')) ch = L'_';
            }
            return name;
        }

        bool file_exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

        // ---- names of things referenced from script (all may be null) -----------------------------
        std::string object_name(UObject* object)
        {
            if (!object) return "null";
            return narrow(object->GetFullName());
        }
        std::string short_object_name(UObject* object)
        {
            if (!object) return "null";
            UObject* outer = object->GetOuterPrivate();
            return (outer ? narrow(outer->GetName()) + ":" : std::string()) + narrow(object->GetName());
        }
        std::string property_name(FProperty* property)
        {
            if (!property) return "null-property";
            return narrow(property->GetName());
        }

        // ---- the bytecode walker ----------------------------------------------------------------------
        struct Walker
        {
            const uint8_t* script = nullptr;
            int size = 0;
            int index = 0;
            bool failed = false;
            int depth = 0;

            template <typename T> T read()
            {
                T value{};
                if (index + (int)sizeof(T) > size)
                {
                    failed = true;
                    index = size;
                    return value;
                }
                memcpy(&value, script + index, sizeof(T));
                index += (int)sizeof(T);
                return value;
            }
            UObject* read_object() { return reinterpret_cast<UObject*>(read<uint64_t>()); }
            FProperty* read_property() { return reinterpret_cast<FProperty*>(read<uint64_t>()); }
            std::string read_name()
            {
                // FScriptName: comparison index, display index, number.
                uint32_t comparison = read<uint32_t>();
                read<uint32_t>();
                uint32_t number = read<uint32_t>();
                if (failed) return "?";
                FName name;
                uint32_t parts[2] = {comparison, number};
                static_assert(sizeof(FName) == sizeof(parts), "FName is a comparison index and a number");
                memcpy(&name, parts, sizeof(parts));
                return narrow(name.ToString());
            }

            std::string args(EExprToken end)
            {
                std::string out;
                bool first = true;
                while (index < size && !failed)
                {
                    if (static_cast<EExprToken>(script[index]) == end)
                    {
                        ++index;
                        return out;
                    }
                    if (!first) out += ", ";
                    first = false;
                    out += expr();
                }
                failed = true;
                return out + " <ran off the end>";
            }

            std::string expr()
            {
                if (index >= size || failed || depth > 200)
                {
                    failed = true;
                    return "<end>";
                }
                ++depth;
                EExprToken op = static_cast<EExprToken>(read<uint8_t>());
                std::string out;
                char number[96];
                switch (op)
                {
                case EX_LocalVariable: out = "local " + property_name(read_property()); break;
                case EX_InstanceVariable: out = "self." + property_name(read_property()); break;
                case EX_DefaultVariable: out = "default." + property_name(read_property()); break;
                case EX_LocalOutVariable: out = "out " + property_name(read_property()); break;
                case EX_ClassSparseDataVariable: out = "sparse." + property_name(read_property()); break;
                case EX_PropertyConst: out = "property(" + property_name(read_property()) + ")"; break;
                case EX_Return: out = "return " + expr(); break;
                case EX_Jump:
                    snprintf(number, sizeof(number), "goto %u", read<uint32_t>());
                    out = number;
                    break;
                case EX_JumpIfNot:
                {
                    uint32_t target = read<uint32_t>();
                    snprintf(number, sizeof(number), "if not (%s) goto %u", expr().c_str(), target);
                    out = "if not (" + std::string(number + 8);
                    break;
                }
                case EX_ComputedJump: out = "goto computed(" + expr() + ")"; break;
                case EX_PushExecutionFlow:
                    snprintf(number, sizeof(number), "push flow %u", read<uint32_t>());
                    out = number;
                    break;
                case EX_PopExecutionFlow: out = "pop flow"; break;
                case EX_PopExecutionFlowIfNot: out = "pop flow if not (" + expr() + ")"; break;
                case EX_Assert:
                    read<uint16_t>();
                    read<uint8_t>();
                    out = "assert(" + expr() + ")";
                    break;
                case EX_Nothing: out = "nothing"; break;
                case EX_NothingInt32:
                    read<int32_t>();
                    out = "nothing";
                    break;
                case EX_EndOfScript: out = "end of script"; break;
                case EX_Let:
                {
                    read_property();
                    std::string target = expr();
                    out = target + " = " + expr();
                    break;
                }
                case EX_LetObj:
                case EX_LetWeakObjPtr:
                case EX_LetBool:
                case EX_LetDelegate:
                case EX_LetMulticastDelegate:
                {
                    std::string target = expr();
                    out = target + " = " + expr();
                    break;
                }
                case EX_LetValueOnPersistentFrame:
                {
                    std::string target = "frame." + property_name(read_property());
                    out = target + " = " + expr();
                    break;
                }
                case EX_StructMemberContext:
                {
                    std::string member = property_name(read_property());
                    out = expr() + "." + member;
                    break;
                }
                case EX_ClassContext:
                case EX_Context:
                case EX_Context_FailSilent:
                {
                    std::string object = expr();
                    read<uint32_t>();
                    read<uint64_t>(); // the property the result goes to, if it has to be zeroed
                    out = object + "->" + expr();
                    break;
                }
                case EX_InterfaceContext: out = "interface(" + expr() + ")"; break;
                case EX_Self: out = "self"; break;
                case EX_Skip:
                    read<uint32_t>();
                    out = expr();
                    break;
                case EX_CallMath:
                case EX_FinalFunction:
                case EX_LocalFinalFunction:
                {
                    UObject* fn = read_object();
                    out = short_object_name(fn) + "(" + args(EX_EndFunctionParms) + ")";
                    break;
                }
                case EX_VirtualFunction:
                case EX_LocalVirtualFunction:
                {
                    std::string name = read_name();
                    out = "virtual " + name + "(" + args(EX_EndFunctionParms) + ")";
                    break;
                }
                case EX_CallMulticastDelegate:
                {
                    UObject* signature = read_object();
                    out = "broadcast[" + short_object_name(signature) + "](" + args(EX_EndFunctionParms) + ")";
                    break;
                }
                case EX_IntConst:
                    snprintf(number, sizeof(number), "%d", read<int32_t>());
                    out = number;
                    break;
                case EX_Int64Const:
                    snprintf(number, sizeof(number), "%lld", (long long)read<int64_t>());
                    out = number;
                    break;
                case EX_UInt64Const:
                    snprintf(number, sizeof(number), "%llu", (unsigned long long)read<uint64_t>());
                    out = number;
                    break;
                case EX_FloatConst:
                    snprintf(number, sizeof(number), "%g", read<float>());
                    out = number;
                    break;
                case EX_DoubleConst:
                    snprintf(number, sizeof(number), "%g", read<double>());
                    out = number;
                    break;
                case EX_ByteConst:
                case EX_IntConstByte:
                    snprintf(number, sizeof(number), "%u", (unsigned)read<uint8_t>());
                    out = number;
                    break;
                case EX_IntZero: out = "0"; break;
                case EX_IntOne: out = "1"; break;
                case EX_True: out = "true"; break;
                case EX_False: out = "false"; break;
                case EX_NoObject: out = "none"; break;
                case EX_NoInterface: out = "no-interface"; break;
                case EX_StringConst:
                {
                    out = "\"";
                    while (index < size)
                    {
                        char ch = (char)read<uint8_t>();
                        if (!ch) break;
                        out += ch;
                    }
                    out += "\"";
                    break;
                }
                case EX_UnicodeStringConst:
                {
                    std::wstring text;
                    while (index < size)
                    {
                        wchar_t ch = (wchar_t)read<uint16_t>();
                        if (!ch) break;
                        text += ch;
                    }
                    out = "\"" + narrow(text) + "\"";
                    break;
                }
                case EX_TextConst:
                {
                    uint8_t kind = read<uint8_t>();
                    if (kind == 1)
                    {
                        std::string source = expr();
                        expr();
                        expr();
                        out = "text(" + source + ")";
                    }
                    else if (kind == 2 || kind == 3)
                    {
                        out = "text(" + expr() + ")";
                    }
                    else if (kind == 4)
                    {
                        read_object();
                        std::string table = expr();
                        out = "text-table(" + table + ", " + expr() + ")";
                    }
                    else
                    {
                        out = "text()";
                    }
                    break;
                }
                case EX_ObjectConst: out = "object<" + object_name(read_object()) + ">"; break;
                case EX_SoftObjectConst: out = "soft(" + expr() + ")"; break;
                case EX_FieldPathConst: out = "field-path(" + expr() + ")"; break;
                case EX_NameConst: out = "name'" + read_name() + "'"; break;
                case EX_RotationConst:
                {
                    double p = read<double>(), y = read<double>(), r = read<double>();
                    snprintf(number, sizeof(number), "rot(P=%g Y=%g R=%g)", p, y, r);
                    out = number;
                    break;
                }
                case EX_VectorConst:
                {
                    double x = read<double>(), y = read<double>(), z = read<double>();
                    snprintf(number, sizeof(number), "vec(%g, %g, %g)", x, y, z);
                    out = number;
                    break;
                }
                case EX_Vector3fConst:
                {
                    float x = read<float>(), y = read<float>(), z = read<float>();
                    snprintf(number, sizeof(number), "vec3f(%g, %g, %g)", x, y, z);
                    out = number;
                    break;
                }
                case EX_TransformConst:
                {
                    double v[10];
                    for (double& value : v) value = read<double>();
                    snprintf(number, sizeof(number), "transform(rot %g %g %g %g, at %g %g %g, scale %g %g %g)", v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9]);
                    out = number;
                    break;
                }
                case EX_StructConst:
                {
                    UObject* type = read_object();
                    read<int32_t>();
                    out = (type ? narrow(type->GetName()) : std::string("struct")) + "{" + args(EX_EndStructConst) + "}";
                    break;
                }
                case EX_SetArray:
                {
                    std::string target = expr();
                    out = target + " = [" + args(EX_EndArray) + "]";
                    break;
                }
                case EX_SetSet:
                {
                    std::string target = expr();
                    read<int32_t>();
                    out = target + " = set[" + args(EX_EndSet) + "]";
                    break;
                }
                case EX_SetMap:
                {
                    std::string target = expr();
                    read<int32_t>();
                    out = target + " = map[" + args(EX_EndMap) + "]";
                    break;
                }
                case EX_ArrayConst:
                    read_property();
                    read<int32_t>();
                    out = "[" + args(EX_EndArrayConst) + "]";
                    break;
                case EX_SetConst:
                    read_property();
                    read<int32_t>();
                    out = "set[" + args(EX_EndSetConst) + "]";
                    break;
                case EX_MapConst:
                    read_property();
                    read_property();
                    read<int32_t>();
                    out = "map[" + args(EX_EndMapConst) + "]";
                    break;
                case EX_BitFieldConst:
                {
                    std::string field = property_name(read_property());
                    snprintf(number, sizeof(number), "%u", (unsigned)read<uint8_t>());
                    out = "bit(" + field + ")=" + number;
                    break;
                }
                case EX_MetaCast:
                case EX_DynamicCast:
                case EX_ObjToInterfaceCast:
                case EX_CrossInterfaceCast:
                case EX_InterfaceToObjCast:
                {
                    UObject* type = read_object();
                    out = "cast<" + (type ? narrow(type->GetName()) : std::string("?")) + ">(" + expr() + ")";
                    break;
                }
                case EX_Cast:
                    snprintf(number, sizeof(number), "convert%u(", (unsigned)read<uint8_t>());
                    out = number + expr() + ")";
                    break;
                case EX_InstanceDelegate: out = "delegate'" + read_name() + "'"; break;
                case EX_BindDelegate:
                {
                    std::string name = read_name();
                    std::string target = expr();
                    out = "bind " + target + " to " + expr() + "." + name;
                    break;
                }
                case EX_AddMulticastDelegate:
                {
                    std::string target = expr();
                    out = target + " += " + expr();
                    break;
                }
                case EX_RemoveMulticastDelegate:
                {
                    std::string target = expr();
                    out = target + " -= " + expr();
                    break;
                }
                case EX_ClearMulticastDelegate: out = "clear " + expr(); break;
                case EX_SwitchValue:
                {
                    uint16_t cases = read<uint16_t>();
                    read<uint32_t>();
                    out = "switch(" + expr() + "){";
                    for (uint16_t i = 0; i < cases && !failed; ++i)
                    {
                        std::string key = expr();
                        read<uint32_t>();
                        out += key + ": " + expr() + "; ";
                    }
                    out += "default: " + expr() + "}";
                    break;
                }
                case EX_ArrayGetByRef:
                {
                    std::string array = expr();
                    out = array + "[" + expr() + "]";
                    break;
                }
                case EX_SkipOffsetConst:
                    snprintf(number, sizeof(number), "offset %u", read<uint32_t>());
                    out = number;
                    break;
                case EX_Tracepoint:
                case EX_WireTracepoint:
                case EX_Breakpoint: out = "trace"; break;
                case EX_InstrumentationEvent:
                {
                    uint8_t kind = read<uint8_t>();
                    if (kind == 4) read_name(); // an inline event carries its name
                    out = "instrumentation";
                    break;
                }
                case EX_AutoRtfmTransact:
                    read<int32_t>();
                    read<uint32_t>();
                    out = "transact{" + args(EX_AutoRtfmStopTransact) + "}";
                    read<int32_t>();
                    read<uint8_t>();
                    break;
                case EX_AutoRtfmStopTransact:
                    read<int32_t>();
                    read<uint8_t>();
                    out = "stop transact";
                    break;
                case EX_AutoRtfmAbortIfNot: out = "abort if not (" + expr() + ")"; break;
                case EX_EndFunctionParms:
                case EX_EndStructConst:
                case EX_EndArray:
                case EX_EndArrayConst:
                case EX_EndSet:
                case EX_EndMap:
                case EX_EndSetConst:
                case EX_EndMapConst:
                case EX_EndParmValue:
                case EX_DeprecatedOp4A: out = "<end marker>"; break;
                default:
                    snprintf(number, sizeof(number), "<unknown code 0x%02X>", (unsigned)op);
                    out = number;
                    failed = true;
                    break;
                }
                --depth;
                return out;
            }
        };

        std::string property_type(FProperty* property)
        {
            std::string type = narrow(std::wstring(property->GetClass().GetName()));
            if (type == "StructProperty")
            {
                UScriptStruct* inner = static_cast<FStructProperty*>(property)->GetStruct();
                if (inner) type += "<" + narrow(inner->GetName()) + ">";
            }
            return type;
        }

        // One function's script as text. Kept apart so that a fault while reading one function
        // (caught by the caller) does not lose the others.
        void function_text(UFunction* fn, std::string* out)
        {
            auto& script = fn->GetScript();
            std::string text = "\n==== " + narrow(fn->GetName()) + "  (";
            bool first = true;
            for (FProperty* p : fn->ForEachProperty())
            {
                if (!first) text += ", ";
                first = false;
                text += property_type(p) + " " + narrow(p->GetName());
            }
            char line[96];
            snprintf(line, sizeof(line), ")  %d bytes of script, flags 0x%X\n", script.Num(), (unsigned)fn->GetFunctionFlags());
            text += line;
            Walker walker;
            walker.script = script.GetData();
            walker.size = script.Num();
            while (walker.index < walker.size && !walker.failed)
            {
                int at = walker.index;
                std::string statement = walker.expr();
                snprintf(line, sizeof(line), "%6d: ", at);
                text += line + statement + "\n";
            }
            if (walker.failed) text += "        <stopped: could not read further>\n";
            *out = std::move(text);
        }

        bool function_text_guarded(UFunction* fn, std::string* out)
        {
            __try
            {
                function_text(fn, out);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        std::set<UClass*> g_dumped_classes;

        // Writes <Class>.kismet.txt and <Class>.vars.txt. Returns false when there was nothing to write.
        bool dump_class(UClass* cls)
        {
            if (!cls || g_dumped_classes.count(cls)) return false;
            g_dumped_classes.insert(cls);
            std::wstring name = cls->GetName();
            std::wstring base = study_dir() + file_safe(name);
            if (file_exists(base + L".kismet.txt")) return false;
            int scripted = 0;
            for (UFunction* fn : cls->ForEachFunction())
            {
                if (fn && fn->GetScript().Num() > 0) ++scripted;
            }
            if (!scripted) return false;
            FILE* vars = _wfopen((base + L".vars.txt").c_str(), L"wb");
            if (vars)
            {
                UStruct* super = cls->GetSuperStruct();
                fprintf(vars, "%s\nparent: %s\n\nvariables:\n", narrow(cls->GetFullName()).c_str(), super ? narrow(super->GetFullName()).c_str() : "none");
                for (FProperty* p : cls->ForEachProperty())
                {
                    fprintf(vars, "  0x%04X  %-40s %s\n", (unsigned)p->GetOffset_Internal(), property_type(p).c_str(), narrow(p->GetName()).c_str());
                }
                fclose(vars);
            }
            FILE* out = _wfopen((base + L".kismet.txt").c_str(), L"wb");
            if (!out) return false;
            fprintf(out, "%s\n%d scripted functions. Each line: offset in the script, then one statement.\n", narrow(cls->GetFullName()).c_str(), scripted);
            int failed = 0;
            for (UFunction* fn : cls->ForEachFunction())
            {
                if (!fn || fn->GetScript().Num() <= 0) continue;
                std::string text;
                if (function_text_guarded(fn, &text)) fwrite(text.data(), 1, text.size(), out);
                else
                {
                    ++failed;
                    fprintf(out, "\n==== %s  <fault while reading this function>\n", narrow(fn->GetName()).c_str());
                }
            }
            fclose(out);
            HSVR_LOG("Study: wrote %ls.kismet.txt (%d scripted functions, %d unreadable)", file_safe(name).c_str(), scripted, failed);
            return true;
        }

        // ---- live objects as text ---------------------------------------------------------------------
        void value_text(FProperty* property, UObject* object, FString* out) { property->ExportTextItem_InContainer(*out, object, nullptr, object, 0); }

        bool value_text_guarded(FProperty* property, UObject* object, FString* out)
        {
            __try
            {
                value_text(property, object, out);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        void export_object(FILE* out, UObject* object, const char* title)
        {
            if (!object) return;
            fprintf(out, "\n######## %s: %s\n", title, narrow(object->GetFullName()).c_str());
            for (UStruct* type = object->GetClassPrivate(); type; type = type->GetSuperStruct())
            {
                for (FProperty* p : type->ForEachProperty())
                {
                    FString value;
                    bool ok = value_text_guarded(p, object, &value);
                    std::string text = ok && value.Len() > 0 ? narrow(std::wstring(*value)) : std::string(ok ? "" : "<fault>");
                    if (text.size() > 6000) text = text.substr(0, 6000) + " ...<cut>";
                    fprintf(out, "  %-34s %-44s = %s\n", property_type(p).c_str(), narrow(p->GetName()).c_str(), text.c_str());
                }
            }
        }

        struct RawObjectArray
        {
            UObject** data;
            int32_t num;
            int32_t max;
        };

        UObject* object_value(UObject* owner, const TCHAR* name)
        {
            if (!owner || !owner->GetPropertyByNameInChain(name)) return nullptr;
            UObject** value = owner->GetValuePtrByPropertyNameInChain<UObject*>(name);
            return value ? *value : nullptr;
        }

        void export_array_objects(FILE* out, UObject* owner, const TCHAR* array_name, const char* title, int limit = 400)
        {
            if (!owner || !owner->GetPropertyByNameInChain(array_name)) return;
            RawObjectArray* arr = owner->GetValuePtrByPropertyNameInChain<RawObjectArray>(array_name);
            if (!arr || !arr->data || arr->num <= 0 || arr->num > 5000) return;
            for (int32_t i = 0; i < arr->num && i < limit; ++i) export_object(out, arr->data[i], title);
        }

        std::set<std::wstring> g_dumped_objects;

        void dump_character(UObject* pawn, const std::wstring& level)
        {
            std::wstring key = file_safe(level) + L"_" + file_safe(std::wstring(pawn->GetClassPrivate()->GetName()));
            if (g_dumped_objects.count(key)) return;
            g_dumped_objects.insert(key);
            std::wstring path = study_dir() + L"objects_" + key + L".txt";
            if (file_exists(path)) return;
            FILE* out = _wfopen(path.c_str(), L"wb");
            if (!out) return;
            fprintf(out, "Live settings of the character and what belongs to it, as the game has them right now.\n");
            export_object(out, pawn, "character");
            UObject* mesh = object_value(pawn, STR("Mesh"));
            export_object(out, mesh, "body mesh component");
            export_object(out, object_value(pawn, STR("CharacterMovement")), "movement component");
            export_object(out, object_value(pawn, STR("CapsuleComponent")), "capsule");
            export_array_objects(out, pawn, STR("BlueprintCreatedComponents"), "component");
            export_array_objects(out, pawn, STR("InstanceComponents"), "component (added at run time)");
            // The physics asset: every body and every joint as authored.
            UObject* asset = object_value(mesh, STR("SkeletalMeshAsset"));
            if (!asset) asset = object_value(mesh, STR("SkinnedAsset"));
            UObject* physics = object_value(mesh, STR("PhysicsAssetOverride"));
            if (!physics) physics = object_value(asset, STR("PhysicsAsset"));
            export_object(out, asset, "skeletal mesh asset");
            export_object(out, physics, "physics asset");
            export_array_objects(out, physics, STR("SkeletalBodySetups"), "physics body");
            export_array_objects(out, physics, STR("ConstraintSetup"), "physics joint");
            // The hidden animated skeleton the body copies its pose from.
            UObject* driver = object_value(pawn, STR("DriverSkeleton"));
            export_object(out, object_value(driver, STR("AnimScriptInstance")), "driver skeleton anim instance");
            export_object(out, object_value(mesh, STR("AnimScriptInstance")), "body anim instance");
            export_object(out, object_value(pawn, STR("PhysicalAnimation")), "physical animation component");
            for (const TCHAR* hand : {STR("Weapon R"), STR("Weapon L")})
            {
                UObject* weapon = object_value(pawn, hand);
                if (!weapon) continue;
                export_object(out, weapon, "held weapon");
                export_array_objects(out, weapon, STR("BlueprintCreatedComponents"), "weapon component");
                export_array_objects(out, weapon, STR("InstanceComponents"), "weapon component (added at run time)");
            }
            fclose(out);
            HSVR_LOG("Study: wrote objects_%ls.txt", key.c_str());
        }

        std::set<std::wstring> g_levels_done;
    } // namespace

    void study_tick(UObject* pawn, const std::wstring& level_name)
    {
        Config cfg = config();
        if (!cfg.dev_study) return;
        // A few seconds after a level is first seen (things are still loading before that), and
        // again whenever a new character appears.
        static std::wstring current;
        static double seen_at = 0.0;
        static UObject* dumped_pawn = nullptr;
        double now = now_seconds();
        if (level_name != current)
        {
            current = level_name;
            seen_at = now;
        }
        bool level_due = !g_levels_done.count(level_name) && now - seen_at > 4.0;
        bool pawn_due = pawn && pawn != dumped_pawn && now - seen_at > 4.0;
        if (!level_due && !pawn_due) return;
        if (level_due)
        {
            g_levels_done.insert(level_name);
            // Every Blueprint class in memory that has script.
            int written = 0, classes = 0;
            std::vector<UClass*> found;
            UObjectGlobals::ForEachUObject([&](UObject* object, int32, int32) {
                if (!object) return LoopAction::Continue;
                UClass* type = object->GetClassPrivate();
                if (!type) return LoopAction::Continue;
                std::wstring kind = type->GetName();
                if (kind == STR("BlueprintGeneratedClass") || kind == STR("AnimBlueprintGeneratedClass"))
                {
                    if (object->GetPathName().rfind(STR("/Game/"), 0) == 0) found.push_back(static_cast<UClass*>(object));
                }
                return LoopAction::Continue;
            });
            for (UClass* cls : found)
            {
                ++classes;
                if (dump_class(cls)) ++written;
            }
            HSVR_LOG("Study: level %ls: %d Blueprint classes in memory, %d newly written to the study folder", level_name.c_str(), classes, written);
        }
        if (pawn_due)
        {
            dumped_pawn = pawn;
            for (UClass* cls = pawn->GetClassPrivate(); cls; cls = cls->GetSuperClass()) dump_class(cls);
            dump_character(pawn, level_name);
        }
    }
} // namespace hsvr
