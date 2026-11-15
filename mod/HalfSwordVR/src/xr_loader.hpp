// Minimal OpenXR loader: finds the active runtime (SteamVR) in the registry, loads it and
// resolves the functions we use. Replaces the Khronos loader library so the mod is one DLL.
// API layers are not supported.
#pragma once

#include <windows.h>
#include <d3d12.h>

#define XR_USE_GRAPHICS_API_D3D12
#define XR_USE_PLATFORM_WIN32
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

namespace hsvr
{
#define HSVR_XR_FUNCTIONS(X)              \
    X(DestroyInstance)                    \
    X(GetInstanceProperties)              \
    X(GetSystem)                          \
    X(GetSystemProperties)                \
    X(EnumerateViewConfigurationViews)    \
    X(GetD3D12GraphicsRequirementsKHR)    \
    X(CreateSession)                      \
    X(DestroySession)                     \
    X(CreateReferenceSpace)               \
    X(DestroySpace)                       \
    X(EnumerateSwapchainFormats)          \
    X(CreateSwapchain)                    \
    X(DestroySwapchain)                   \
    X(EnumerateSwapchainImages)           \
    X(AcquireSwapchainImage)              \
    X(WaitSwapchainImage)                 \
    X(ReleaseSwapchainImage)              \
    X(PollEvent)                          \
    X(BeginSession)                       \
    X(EndSession)                         \
    X(WaitFrame)                          \
    X(BeginFrame)                         \
    X(EndFrame)                           \
    X(LocateViews)                        \
    X(LocateSpace)                        \
    X(ResultToString)                     \
    X(StringToPath)                       \
    X(PathToString)                       \
    X(CreateActionSet)                    \
    X(CreateAction)                       \
    X(SuggestInteractionProfileBindings)  \
    X(AttachSessionActionSets)            \
    X(CreateActionSpace)                  \
    X(SyncActions)                        \
    X(GetActionStatePose)                 \
    X(GetActionStateBoolean)              \
    X(GetActionStateFloat)                \
    X(GetCurrentInteractionProfile)

    struct XrApi
    {
        PFN_xrGetInstanceProcAddr GetInstanceProcAddr = nullptr;
        PFN_xrCreateInstance CreateInstance = nullptr;
        PFN_xrEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties = nullptr;
#define HSVR_XR_DECLARE(name) PFN_xr##name name = nullptr;
        HSVR_XR_FUNCTIONS(HSVR_XR_DECLARE)
#undef HSVR_XR_DECLARE
    };

    // Loads the active runtime. Fills GetInstanceProcAddr, CreateInstance and
    // EnumerateInstanceExtensionProperties. Returns false (and logs why) if there is no runtime.
    bool xr_load_runtime(XrApi& api);
    // Resolves the remaining functions once an instance exists.
    bool xr_resolve_functions(XrApi& api, XrInstance instance);
} // namespace hsvr
