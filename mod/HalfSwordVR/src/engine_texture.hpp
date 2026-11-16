// Gets the Direct3D 12 texture behind one of the engine's own texture objects.
#pragma once

struct ID3D12Resource;

namespace hsvr
{
    // `rhi_texture` is an engine FRHITexture*. Returns the D3D12 resource behind it (not
    // add-ref'd), or nullptr if it cannot be read safely. After repeated failures it gives up
    // for good and engine_texture_unavailable() turns true.
    ID3D12Resource* engine_texture_resource(void* rhi_texture);
    bool engine_texture_unavailable();
} // namespace hsvr
