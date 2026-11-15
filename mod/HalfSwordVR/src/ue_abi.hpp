// Our own minimal declarations of the Unreal Engine 5.4 interfaces the mod has to implement or
// call. Only layout matters here: the order and signatures of the virtual functions must match
// the engine build inside the game exactly (MSVC x64). Nothing in this file is engine source.
//
// Verified against Unreal Engine 5.4.4. A different engine version needs a fresh check.
#pragma once
#include <cstdint>

namespace ue
{
    struct FVector
    {
        double X, Y, Z;
    };
    struct FRotator
    {
        double Pitch, Yaw, Roll;
    };
    struct FVector2D
    {
        double X, Y;
    };
    struct alignas(16) FMatrix
    {
        double M[4][4];
    };
    struct FIntRect
    {
        int32_t MinX, MinY, MaxX, MaxY;
    };

    enum EStereoscopicPass
    {
        eSSP_FULL,
        eSSP_PRIMARY,
        eSSP_SECONDARY
    };

    // Engine types we only ever handle by pointer or reference.
    class FRHICommandListImmediate;
    class FRHITexture;
    class FSceneView;
    class UCanvas;
    class FViewport;
    class SViewport;
    class IStereoLayers;
    struct FPostProcessSettings;
    class IStereoRenderTargetManager;

    // TSharedPtr<T, ESPMode::ThreadSafe>
    template <typename T>
    struct TSharedPtr
    {
        T* Object;
        void* ReferenceController;
    };
} // namespace ue
