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

namespace ue
{
    // Mirror of the engine's stereo rendering interface (UE 5.4.4). One vtable slot per virtual,
    // in this exact order; slot 0 is the destructor.
    class IStereoRendering
    {
      public:
        virtual ~IStereoRendering() = default;
        virtual bool IsStereoEnabled() const = 0;
        virtual bool IsStereoEnabledOnNextFrame() const = 0;
        virtual bool EnableStereo(bool bStereo) = 0;
        virtual int32_t GetDesiredNumberOfViews(bool bStereoRequested) const = 0;
        virtual EStereoscopicPass GetViewPassForIndex(bool bStereoRequested, int32_t ViewIndex) const = 0;
        virtual uint32_t GetLODViewIndex() const = 0;
        virtual bool IsStandaloneStereoOnlyDevice() const = 0;
        virtual void AdjustViewRect(int32_t ViewIndex, int32_t& X, int32_t& Y, uint32_t& SizeX, uint32_t& SizeY) const = 0;
        virtual void SetFinalViewRect(FRHICommandListImmediate& RHICmdList, const int32_t ViewIndex, const FIntRect& FinalViewRect) = 0;
        virtual FVector2D GetTextSafeRegionBounds() const = 0;
        virtual void CalculateStereoViewOffset(const int32_t ViewIndex, FRotator& ViewRotation, const float WorldToMeters, FVector& ViewLocation) = 0;
        virtual FMatrix GetStereoProjectionMatrix(const int32_t ViewIndex) const = 0;
        virtual void InitCanvasFromView(FSceneView* InView, UCanvas* Canvas) = 0;
        virtual void RenderTexture_RenderThread(FRHICommandListImmediate& RHICmdList, FRHITexture* BackBuffer, FRHITexture* SrcTexture, FVector2D WindowSize) const = 0;
        virtual IStereoRenderTargetManager* GetRenderTargetManager() = 0;
        virtual IStereoLayers* GetStereoLayers() = 0;
        virtual void StartFinalPostprocessSettings(FPostProcessSettings* Settings, const EStereoscopicPass Pass, const int32_t ViewIndex) = 0;
        virtual bool OverrideFinalPostprocessSettings(FPostProcessSettings* Settings, const EStereoscopicPass Pass, const int32_t ViewIndex, float& BlendWeight) = 0;
        virtual void EndFinalPostprocessSettings(FPostProcessSettings* Settings, const EStereoscopicPass Pass, const int32_t ViewIndex) = 0;
    };
} // namespace ue
