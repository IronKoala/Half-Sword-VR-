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

    enum class EStereoscopicPass
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
    // in this exact order; slot 0 is the destructor. 20 slots, checked against the 5.4.4 header.
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

namespace ue
{
    // Mirror of the engine's stereo render target manager interface (UE 5.4.4). It has no
    // virtual destructor, so slot 0 is ShouldUseSeparateRenderTarget; 15 slots in all, checked
    // against the 5.4.4 header. Arguments we never look at are declared as plain pointers or
    // integers of the same size.
    class IStereoRenderTargetManager
    {
      public:
        virtual bool ShouldUseSeparateRenderTarget() const = 0;
        virtual void UpdateViewport(bool bUseSeparateRenderTarget, const FViewport& Viewport, SViewport* ViewportWidget) = 0;
        virtual void CalculateRenderTargetSize(const FViewport& Viewport, uint32_t& InOutSizeX, uint32_t& InOutSizeY) = 0;
        virtual bool NeedReAllocateViewportRenderTarget(const FViewport& Viewport) = 0;
        virtual bool NeedReAllocateDepthTexture(const void* DepthTarget) = 0;
        virtual bool NeedReAllocateShadingRateTexture(const void* ShadingRateTarget) = 0;
        virtual uint32_t GetNumberOfBufferedFrames() const = 0;
        virtual bool AllocateRenderTargetTexture(uint32_t Index, uint32_t SizeX, uint32_t SizeY, uint8_t Format, uint32_t NumMips, uint64_t Flags,
                                                 uint64_t TargetableTextureFlags, void* OutTargetableTexture, void* OutShaderResourceTexture, uint32_t NumSamples) = 0;
        virtual bool AllocateRenderTargetTextures(uint32_t SizeX, uint32_t SizeY, uint8_t Format, uint32_t NumLayers, uint64_t Flags, uint64_t TargetableTextureFlags,
                                                  void* OutTargetableTextures, void* OutShaderResourceTextures, uint32_t NumSamples) = 0;
        virtual uint8_t GetActualColorSwapchainFormat() const = 0; // EPixelFormat, 0 = PF_Unknown
        virtual int32_t AcquireColorTexture() = 0;
        virtual bool AllocateDepthTexture(uint32_t Index, uint32_t SizeX, uint32_t SizeY, uint8_t Format, uint32_t NumMips, uint64_t Flags,
                                          uint64_t TargetableTextureFlags, void* OutTargetableTexture, void* OutShaderResourceTexture, uint32_t NumSamples) = 0;
        virtual bool AllocateShadingRateTexture(uint32_t Index, uint32_t RenderSizeX, uint32_t RenderSizeY, uint8_t Format, uint32_t NumMips, uint64_t Flags,
                                                uint64_t TargetableTextureFlags, void* OutTexture, void* OutTextureSize) = 0;
        virtual bool HDRGetMetaDataForStereo(void* OutDisplayOutputFormat, void* OutDisplayColorGamut, bool& OutbHDRSupported) = 0;
        virtual bool ReconfigureForShaderPlatform(uint32_t NewShaderPlatform) = 0;
    };
} // namespace ue
