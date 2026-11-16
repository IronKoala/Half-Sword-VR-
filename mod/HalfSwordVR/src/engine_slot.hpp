// Finds where the running engine keeps its stereo rendering device pointer.
#pragma once
#include <cstddef>
#include <cstdint>

namespace hsvr
{
    // `engine` is the UEngine object. [begin, end) is the byte range inside it to search
    // (between two reflected properties that are known to surround the slot).
    // Returns the offset of UEngine::StereoRenderingDevice, or 0 if it is not found unambiguously.
    size_t find_stereo_slot_offset(const uint8_t* engine, size_t begin, size_t end);
    // True when `address` lies inside the game's main executable image.
    bool address_in_game_image(const void* address);
    bool memory_readable(const void* address, size_t size);
} // namespace hsvr
