#include "engine_texture.hpp"

#include "engine_slot.hpp"
#include "log.hpp"

#include <windows.h>
#include <d3d12.h>

#include <atomic>
#include <cwchar>

namespace hsvr
{
    namespace
    {
        // FRHITexture::GetNativeResource is slot 4 of the texture's function table in a shipping
        // build of UE 5.4 (later slots in builds with extra debug features). The answer is checked
        // before it is trusted, so a wrong guess costs a log line, not a crash.
        constexpr int kCandidateSlots[] = {4, 5, 6};
        std::atomic<int> g_slot{-1};
        std::atomic<void*> g_resource_vtable{nullptr};
        std::atomic<bool> g_unavailable{false};
        int g_failures = 0;

        using Getter = void* (*)(void* self, void* scratch);

        // Calls one slot of the texture's function table. The second argument is spare memory:
        // if the slot turns out to be a function that returns a small struct, it writes there.
        void* call_slot(void* texture, int slot)
        {
            void** vtable = *reinterpret_cast<void***>(texture);
            if (!memory_readable(&vtable[slot], sizeof(void*))) return nullptr;
            void* fn = vtable[slot];
            if (!address_in_game_image(fn)) return nullptr;
            alignas(16) unsigned char scratch[64] = {};
            void* result = reinterpret_cast<Getter>(fn)(texture, scratch);
            if (result == static_cast<void*>(scratch)) return nullptr;
            return result;
        }

        // True if `object` looks like a COM object implemented by a system library (not the game).
        bool looks_like_system_com_object(void* object, wchar_t* module_name, size_t module_name_len)
        {
            if (!memory_readable(object, sizeof(void*))) return false;
            void** vtable = *reinterpret_cast<void***>(object);
            if (!memory_readable(vtable, sizeof(void*) * 12)) return false;
            void* first = vtable[0];
            if (address_in_game_image(first) || address_in_game_image(vtable)) return false;
            HMODULE module = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(first), &module) || !module)
            {
                return false;
            }
            wchar_t path[MAX_PATH] = {};
            GetModuleFileNameW(module, path, MAX_PATH);
            const wchar_t* name = wcsrchr(path, L'\\');
            name = name ? name + 1 : path;
            wcsncpy(module_name, name, module_name_len - 1);
            module_name[module_name_len - 1] = 0;
            return true;
        }

        ID3D12Resource* validate(void* candidate, int slot)
        {
            wchar_t module_name[64] = {};
            if (!looks_like_system_com_object(candidate, module_name, 64)) return nullptr;
            ID3D12Resource* resource = nullptr;
            if (FAILED(static_cast<IUnknown*>(candidate)->QueryInterface(__uuidof(ID3D12Resource), reinterpret_cast<void**>(&resource))) || !resource) return nullptr;
            resource->Release(); // the engine keeps it alive; the caller add-refs if it holds on
            if (static_cast<void*>(resource) != candidate) return nullptr;
            D3D12_RESOURCE_DESC desc = resource->GetDesc();
            if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width < 64 || desc.Height < 64) return nullptr;
            g_resource_vtable.store(*reinterpret_cast<void**>(candidate));
            g_slot.store(slot);
            HSVR_LOG("Engine eye target found: %llux%u, format %d, flags 0x%X (texture function %d, implemented in %ls)",
                     (unsigned long long)desc.Width, desc.Height, (int)desc.Format, (unsigned)desc.Flags, slot, module_name);
            return resource;
        }
    } // namespace

    bool engine_texture_unavailable()
    {
        return g_unavailable.load();
    }

    ID3D12Resource* engine_texture_resource(void* rhi_texture)
    {
        if (!rhi_texture || g_unavailable.load()) return nullptr;
        if (!memory_readable(rhi_texture, sizeof(void*))) return nullptr;
        void** vtable = *reinterpret_cast<void***>(rhi_texture);
        if (!address_in_game_image(vtable)) return nullptr;

        int slot = g_slot.load();
        if (slot >= 0)
        {
            void* result = call_slot(rhi_texture, slot);
            if (result && memory_readable(result, sizeof(void*)) && *reinterpret_cast<void**>(result) == g_resource_vtable.load())
            {
                return static_cast<ID3D12Resource*>(result);
            }
            if (!result) return nullptr;
            // A different kind of object than last time: check it from scratch.
            return validate(result, slot);
        }

        for (int candidate_slot : kCandidateSlots)
        {
            void* result = call_slot(rhi_texture, candidate_slot);
            if (!result) continue;
            if (ID3D12Resource* resource = validate(result, candidate_slot)) return resource;
        }
        if (++g_failures >= 30)
        {
            g_unavailable.store(true);
            HSVR_LOG("Could not read the engine's eye target texture; falling back to drawing through the game window");
        }
        return nullptr;
    }
} // namespace hsvr
