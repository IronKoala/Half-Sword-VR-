#include "engine_slot.hpp"

#include "log.hpp"

#include <windows.h>

#include <cstring>
#include <vector>

namespace hsvr
{
    bool memory_readable(const void* address, size_t size)
    {
        if (!address) return false;
        const uint8_t* p = static_cast<const uint8_t*>(address);
        const uint8_t* end = p + size;
        while (p < end)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return false;
            if (mbi.State != MEM_COMMIT) return false;
            if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
            const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
            if (!(mbi.Protect & readable)) return false;
            p = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
        }
        return true;
    }

    bool address_in_game_image(const void* address)
    {
        static uintptr_t base = 0, limit = 0;
        if (!base)
        {
            HMODULE exe = GetModuleHandleW(nullptr);
            auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
            auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(exe) + dos->e_lfanew);
            base = reinterpret_cast<uintptr_t>(exe);
            limit = base + nt->OptionalHeader.SizeOfImage;
        }
        uintptr_t a = reinterpret_cast<uintptr_t>(address);
        return a >= base && a < limit;
    }

    namespace
    {
        uint64_t read_u64(const uint8_t* p)
        {
            uint64_t v;
            memcpy(&v, p, sizeof(v));
            return v;
        }

        // A live TSharedPtr: object pointer plus a reference controller whose vtable is in the game
        // image and whose counts look sane.
        bool looks_like_shared_ptr(const uint8_t* p)
        {
            const void* object = reinterpret_cast<const void*>(read_u64(p));
            const void* controller = reinterpret_cast<const void*>(read_u64(p + 8));
            if (!memory_readable(object, 16) || !memory_readable(controller, 16)) return false;
            const void* vtable = reinterpret_cast<const void*>(read_u64(static_cast<const uint8_t*>(controller)));
            if (!address_in_game_image(vtable)) return false;
            int32_t shared_count, weak_count;
            memcpy(&shared_count, static_cast<const uint8_t*>(controller) + 8, 4);
            memcpy(&weak_count, static_cast<const uint8_t*>(controller) + 12, 4);
            return shared_count >= 1 && shared_count < 100000 && weak_count >= 1 && weak_count < 100000;
        }

        bool all_zero(const uint8_t* p, size_t size)
        {
            for (size_t i = 0; i < size; ++i)
            {
                if (p[i] != 0) return false;
            }
            return true;
        }
    } // namespace

    size_t find_stereo_slot_offset(const uint8_t* engine, size_t begin, size_t end)
    {
        // Layout in UEngine (UE 5.4.4), each a 16-byte shared pointer:
        //   StereoRenderingDevice (empty in a flat game), XRSystem (empty), ViewExtensions (always set),
        //   EyeTrackingDevice (empty).
        begin = (begin + 7) & ~size_t(7);
        if (end < begin + 64 || !memory_readable(engine + begin, end - begin)) return 0;
        std::vector<size_t> candidates;
        for (size_t offset = begin; offset + 64 <= end; offset += 8)
        {
            const uint8_t* p = engine + offset;
            if (!all_zero(p, 32)) continue;
            if (!all_zero(p + 48, 16)) continue;
            if (!looks_like_shared_ptr(p + 32)) continue;
            candidates.push_back(offset);
        }
        if (candidates.size() == 1) return candidates[0];
        if (candidates.size() > 1)
        {
            // Other runs of empty shared pointers look the same, so check the neighbours too.
            // After EyeTrackingDevice comes a multicast delegate (empty list, compaction threshold 2);
            // before the slot comes a TMap, which when empty ends with a recognisable bit-array header.
            std::vector<size_t> confirmed;
            for (size_t c : candidates)
            {
                const uint8_t* p = engine + c;
                bool delegate_after = c + 0x58 <= end && all_zero(p + 0x40, 16) && read_u64(p + 0x50) == 2;
                bool map_before = c >= begin + 0x28 && read_u64(p - 0x28) == 0x0000008000000000ull && read_u64(p - 0x20) == 0x00000000FFFFFFFFull;
                HSVR_LOG("  candidate 0x%zX: delegate after %d, empty map before %d", c, delegate_after ? 1 : 0, map_before ? 1 : 0);
                if (delegate_after || map_before) confirmed.push_back(c);
            }
            if (confirmed.size() == 1) return confirmed[0];
        }
        HSVR_LOG("Stereo slot search between 0x%zX and 0x%zX found %zu candidates", begin, end, candidates.size());
        for (size_t c : candidates) HSVR_LOG("  candidate at 0x%zX", c);
        // Dump the pointer-shaped contents of the range so the layout can be worked out by hand.
        for (size_t offset = begin; offset + 8 <= end; offset += 8)
        {
            uint64_t v = read_u64(engine + offset);
            if (v != 0) HSVR_LOG("  +0x%04zX: %016llX%s", offset, (unsigned long long)v, looks_like_shared_ptr(engine + offset) ? "  <- shared pointer" : "");
        }
        return 0;
    }
} // namespace hsvr
