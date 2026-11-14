// xrtest: a standalone check that SteamVR + OpenXR + Direct3D 12 work on this PC.
// Shows a world-fixed grid in the headset (left eye tinted red, right eye blue) and
// logs the headset pose. Runs until you press Esc, close SteamVR, or 120 seconds pass.
// Writes xrtest.log next to the exe.

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

static FILE* g_log = nullptr;

static void Log(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    fputs(buf, stdout);
    fputc('\n', stdout);
    if (g_log) {
        fputs(buf, g_log);
        fputc('\n', g_log);
        fflush(g_log);
    }
}

#define XR_CHECK(call)                                                        \
    do {                                                                      \
        XrResult r_ = (call);                                                 \
        if (XR_FAILED(r_)) {                                                  \
            Log("FAILED: %s -> XrResult %d (line %d)", #call, (int)r_, __LINE__); \
            return 1;                                                         \
        }                                                                     \
    } while (0)

#define HR_CHECK(call)                                                        \
    do {                                                                      \
        HRESULT h_ = (call);                                                  \
        if (FAILED(h_)) {                                                     \
            Log("FAILED: %s -> HRESULT 0x%08X (line %d)", #call, (unsigned)h_, __LINE__); \
            return 1;                                                         \
        }                                                                     \
    } while (0)

struct Vec3 { float x, y, z; };

static Vec3 Rotate(const XrQuaternionf& q, Vec3 v) {
    // v' = v + 2w(q x v) + 2 q x (q x v)
    Vec3 u{q.x, q.y, q.z};
    Vec3 c1{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
    Vec3 c2{u.y * c1.z - u.z * c1.y, u.z * c1.x - u.x * c1.z, u.x * c1.y - u.y * c1.x};
    return {v.x + 2 * (q.w * c1.x + c2.x), v.y + 2 * (q.w * c1.y + c2.y), v.z + 2 * (q.w * c1.z + c2.z)};
}

static void YawPitch(const XrQuaternionf& q, float& yaw, float& pitch) {
    Vec3 f = Rotate(q, {0, 0, -1});  // OpenXR: -Z is forward, +Y is up
    yaw = std::atan2(-f.x, -f.z);    // positive = turned left
    pitch = std::asin(std::clamp(f.y, -1.0f, 1.0f));
}

struct EyeSwapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    int32_t width = 0, height = 0;
    std::vector<XrSwapchainImageD3D12KHR> images;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
};

int main() {
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string logPath(exePath);
    logPath = logPath.substr(0, logPath.find_last_of("\\/") + 1) + "xrtest.log";
    g_log = fopen(logPath.c_str(), "w");
    Log("xrtest starting. Log: %s", logPath.c_str());

    // ---- Instance -------------------------------------------------------------
    const char* extensions[] = {XR_KHR_D3D12_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ici.applicationInfo.applicationName, "HalfSwordVR xrtest");
    ici.applicationInfo.applicationVersion = 1;
    strcpy_s(ici.applicationInfo.engineName, "none");
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = extensions;
    XrInstance instance = XR_NULL_HANDLE;
    XR_CHECK(xrCreateInstance(&ici, &instance));

    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    XR_CHECK(xrGetInstanceProperties(instance, &ip));
    Log("Runtime: %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
        XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XR_CHECK(xrGetSystem(instance, &sgi, &systemId));
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    XR_CHECK(xrGetSystemProperties(instance, systemId, &sp));
    Log("Headset: %s (vendor %u), orientation tracking %d, position tracking %d", sp.systemName, sp.vendorId,
        sp.trackingProperties.orientationTracking, sp.trackingProperties.positionTracking);

    // ---- D3D12 device on the adapter the runtime wants --------------------------
    PFN_xrGetD3D12GraphicsRequirementsKHR getReqs = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(instance, "xrGetD3D12GraphicsRequirementsKHR",
                                   reinterpret_cast<PFN_xrVoidFunction*>(&getReqs)));
    XrGraphicsRequirementsD3D12KHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    XR_CHECK(getReqs(instance, systemId, &reqs));

    ComPtr<IDXGIFactory4> factory;
    HR_CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (memcmp(&desc.AdapterLuid, &reqs.adapterLuid, sizeof(LUID)) == 0) {
            char name[128];
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
            Log("GPU: %s (%llu MB VRAM)", name, (unsigned long long)(desc.DedicatedVideoMemory >> 20));
            break;
        }
        adapter.Reset();
    }
    if (!adapter) {
        Log("FAILED: could not find the GPU that SteamVR asked for");
        return 1;
    }
    ComPtr<ID3D12Device> device;
    HR_CHECK(D3D12CreateDevice(adapter.Get(), reqs.minFeatureLevel, IID_PPV_ARGS(&device)));
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    HR_CHECK(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator;
    HR_CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> cmd;
    HR_CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                       IID_PPV_ARGS(&cmd)));
    cmd->Close();
    ComPtr<ID3D12Fence> fence;
    HR_CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    UINT64 fenceValue = 0;
    HANDLE fenceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);

    // ---- Session + spaces -----------------------------------------------------
    XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    binding.device = device.Get();
    binding.queue = queue.Get();
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = systemId;
    XrSession session = XR_NULL_HANDLE;
    XR_CHECK(xrCreateSession(instance, &sci, &session));

    XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsci.poseInReferenceSpace.orientation.w = 1.0f;
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    XrSpace worldSpace = XR_NULL_HANDLE;
    XR_CHECK(xrCreateReferenceSpace(session, &rsci, &worldSpace));
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XrSpace headSpace = XR_NULL_HANDLE;
    XR_CHECK(xrCreateReferenceSpace(session, &rsci, &headSpace));

    // ---- Swapchains -------------------------------------------------------------
    uint32_t viewCount = 0;
    XR_CHECK(xrEnumerateViewConfigurationViews(instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0,
                                               &viewCount, nullptr));
    std::vector<XrViewConfigurationView> configViews(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    XR_CHECK(xrEnumerateViewConfigurationViews(instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                               viewCount, &viewCount, configViews.data()));

    uint32_t formatCount = 0;
    XR_CHECK(xrEnumerateSwapchainFormats(session, 0, &formatCount, nullptr));
    std::vector<int64_t> formats(formatCount);
    XR_CHECK(xrEnumerateSwapchainFormats(session, formatCount, &formatCount, formats.data()));
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    for (DXGI_FORMAT wanted : {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                               DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM}) {
        if (std::find(formats.begin(), formats.end(), (int64_t)wanted) != formats.end()) {
            format = wanted;
            break;
        }
    }
    if (format == DXGI_FORMAT_UNKNOWN) {
        Log("FAILED: no supported 8-bit colour swapchain format");
        return 1;
    }

    std::vector<EyeSwapchain> eyes(viewCount);
    UINT rtvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (uint32_t e = 0; e < viewCount; ++e) {
        auto& eye = eyes[e];
        eye.width = (int32_t)configViews[e].recommendedImageRectWidth;
        eye.height = (int32_t)configViews[e].recommendedImageRectHeight;
        Log("Eye %u: %d x %d recommended (max %u x %u)", e, eye.width, eye.height,
            configViews[e].maxImageRectWidth, configViews[e].maxImageRectHeight);
        XrSwapchainCreateInfo swci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        swci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        swci.format = format;
        swci.sampleCount = 1;
        swci.width = eye.width;
        swci.height = eye.height;
        swci.faceCount = 1;
        swci.arraySize = 1;
        swci.mipCount = 1;
        XR_CHECK(xrCreateSwapchain(session, &swci, &eye.handle));
        uint32_t imageCount = 0;
        XR_CHECK(xrEnumerateSwapchainImages(eye.handle, 0, &imageCount, nullptr));
        eye.images.assign(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        XR_CHECK(xrEnumerateSwapchainImages(eye.handle, imageCount, &imageCount,
                                            reinterpret_cast<XrSwapchainImageBaseHeader*>(eye.images.data())));
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = imageCount;
        HR_CHECK(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&eye.rtvHeap)));
        for (uint32_t i = 0; i < imageCount; ++i) {
            D3D12_RENDER_TARGET_VIEW_DESC rtv{};
            rtv.Format = format;
            rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            D3D12_CPU_DESCRIPTOR_HANDLE h = eye.rtvHeap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += (SIZE_T)i * rtvSize;
            device->CreateRenderTargetView(eye.images[i].texture, &rtv, h);
        }
    }

    // ---- Main loop ------------------------------------------------------------
    bool running = true, sessionRunning = false;
    auto start = std::chrono::steady_clock::now();
    auto lastPoseLog = start;
    uint64_t framesSubmitted = 0, framesRendered = 0;
    float minYaw = 10, maxYaw = -10;

    while (running) {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            Log("Esc pressed, ending session");
            xrRequestExitSession(session);
        }
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(120) && sessionRunning) {
            Log("120 seconds elapsed, ending session");
            xrRequestExitSession(session);
            start = std::chrono::steady_clock::now() + std::chrono::hours(1);
        }

        XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(instance, &ev) == XR_SUCCESS) {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                auto* s = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
                Log("Session state -> %d", (int)s->state);
                if (s->state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    XR_CHECK(xrBeginSession(session, &bi));
                    sessionRunning = true;
                    Log("Session running. Look around: the grid should stay fixed in the room.");
                } else if (s->state == XR_SESSION_STATE_STOPPING) {
                    xrEndSession(session);
                    sessionRunning = false;
                } else if (s->state == XR_SESSION_STATE_EXITING || s->state == XR_SESSION_STATE_LOSS_PENDING) {
                    running = false;
                }
            } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
                running = false;
            }
            ev = {XR_TYPE_EVENT_DATA_BUFFER};
        }
        if (!sessionRunning) {
            Sleep(10);
            continue;
        }

        XrFrameState fs{XR_TYPE_FRAME_STATE};
        XR_CHECK(xrWaitFrame(session, nullptr, &fs));
        XR_CHECK(xrBeginFrame(session, nullptr));

        std::vector<XrCompositionLayerProjectionView> projViews(viewCount,
                                                                {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
        XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        bool haveLayer = false;

        if (fs.shouldRender) {
            std::vector<XrView> views(viewCount, {XR_TYPE_VIEW});
            XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
            vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            vli.displayTime = fs.predictedDisplayTime;
            vli.space = worldSpace;
            XrViewState vs{XR_TYPE_VIEW_STATE};
            uint32_t located = 0;
            XR_CHECK(xrLocateViews(session, &vli, &vs, viewCount, &located, views.data()));

            XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
            xrLocateSpace(headSpace, worldSpace, fs.predictedDisplayTime, &head);

            HR_CHECK(allocator->Reset());
            HR_CHECK(cmd->Reset(allocator.Get(), nullptr));

            for (uint32_t e = 0; e < viewCount; ++e) {
                auto& eye = eyes[e];
                uint32_t idx = 0;
                XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                XR_CHECK(xrAcquireSwapchainImage(eye.handle, &ai, &idx));
                XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                wi.timeout = XR_INFINITE_DURATION;
                XR_CHECK(xrWaitSwapchainImage(eye.handle, &wi));

                D3D12_CPU_DESCRIPTOR_HANDLE rtv = eye.rtvHeap->GetCPUDescriptorHandleForHeapStart();
                rtv.ptr += (SIZE_T)idx * rtvSize;

                // Background: dark, tinted per eye so you can tell them apart.
                float bg[4] = {e == 0 ? 0.25f : 0.05f, 0.05f, e == 0 ? 0.05f : 0.25f, 1.0f};
                cmd->ClearRenderTargetView(rtv, bg, 0, nullptr);

                // World-fixed grid: shift lines opposite to head rotation using this eye's FOV.
                float yaw, pitch;
                YawPitch(views[e].pose.orientation, yaw, pitch);
                const XrFovf& fov = views[e].fov;
                float pxPerRadX = eye.width / (fov.angleRight - fov.angleLeft);
                float pxPerRadY = eye.height / (fov.angleUp - fov.angleDown);
                const float step = 0.1745f;  // a line every 10 degrees
                std::vector<D3D12_RECT> lines;
                for (int k = -40; k <= 40; ++k) {
                    float a = k * step;
                    // Vertical lines at fixed world yaw angles.
                    float dx = a - yaw;  // angle of line relative to view direction (left positive)
                    int x = (int)((-dx - fov.angleLeft) * pxPerRadX);
                    int w = (k % 9 == 0) ? 6 : 2;  // thicker line every 90 degrees
                    if (x > -w && x < eye.width) lines.push_back({std::max(0, x - w / 2), 0, std::min(eye.width, x + w / 2 + 1), eye.height});
                    // Horizontal lines at fixed pitch angles (only within +-80 degrees).
                    if (std::abs(a) < 1.4f) {
                        float dy = a - pitch;
                        int y = (int)((fov.angleUp - dy) * pxPerRadY);
                        int hgt = (k == 0) ? 6 : 2;  // thicker horizon
                        if (y > -hgt && y < eye.height) lines.push_back({0, std::max(0, y - hgt / 2), eye.width, std::min(eye.height, y + hgt / 2 + 1)});
                    }
                }
                float lineColor[4] = {0.9f, 0.9f, 0.9f, 1.0f};
                if (!lines.empty()) cmd->ClearRenderTargetView(rtv, lineColor, (UINT)lines.size(), lines.data());

                // Note: this simple grid ignores head roll and position, so it is only roughly world-fixed.

                projViews[e].pose = views[e].pose;
                projViews[e].fov = views[e].fov;
                projViews[e].subImage.swapchain = eye.handle;
                projViews[e].subImage.imageRect = {{0, 0}, {eye.width, eye.height}};
                projViews[e].subImage.imageArrayIndex = 0;
            }
            HR_CHECK(cmd->Close());
            ID3D12CommandList* lists[] = {cmd.Get()};
            queue->ExecuteCommandLists(1, lists);
            HR_CHECK(queue->Signal(fence.Get(), ++fenceValue));
            if (fence->GetCompletedValue() < fenceValue) {
                fence->SetEventOnCompletion(fenceValue, fenceEvent);
                WaitForSingleObject(fenceEvent, INFINITE);
            }
            for (uint32_t e = 0; e < viewCount; ++e) {
                XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                XR_CHECK(xrReleaseSwapchainImage(eyes[e].handle, &ri));
            }

            layer.space = worldSpace;
            layer.viewCount = viewCount;
            layer.views = projViews.data();
            haveLayer = true;
            ++framesRendered;

            if ((head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
                float hy, hp;
                YawPitch(head.pose.orientation, hy, hp);
                minYaw = std::min(minYaw, hy);
                maxYaw = std::max(maxYaw, hy);
                auto now = std::chrono::steady_clock::now();
                if (now - lastPoseLog > std::chrono::seconds(1)) {
                    lastPoseLog = now;
                    Log("Head: pos (%.2f, %.2f, %.2f) m, yaw %.0f deg, pitch %.0f deg, frames %llu",
                        head.pose.position.x, head.pose.position.y, head.pose.position.z, hy * 57.2958f,
                        hp * 57.2958f, (unsigned long long)framesSubmitted);
                }
            }
        }

        XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
        fei.displayTime = fs.predictedDisplayTime;
        fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer)};
        fei.layerCount = haveLayer ? 1 : 0;
        fei.layers = haveLayer ? layers : nullptr;
        XR_CHECK(xrEndFrame(session, &fei));
        ++framesSubmitted;
    }

    Log("Done. Frames submitted: %llu, rendered: %llu. Head yaw range seen: %.0f to %.0f deg.", (unsigned long long)framesSubmitted,
        (unsigned long long)framesRendered, minYaw * 57.2958f, maxYaw * 57.2958f);
    Log(framesRendered > 0 ? "RESULT: PASS (OpenXR + D3D12 rendering into the headset works)"
                            : "RESULT: FAIL (no frames reached the headset)");
    for (auto& eye : eyes) xrDestroySwapchain(eye.handle);
    xrDestroySpace(headSpace);
    xrDestroySpace(worldSpace);
    xrDestroySession(session);
    xrDestroyInstance(instance);
    if (g_log) fclose(g_log);
    return framesRendered > 0 ? 0 : 1;
}
