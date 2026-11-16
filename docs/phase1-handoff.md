# Handoff from Phase 0 to Phase 1 (2026-11-15)

## Build and install (Matthew's PC, The-Fishtank)
- Checkout: `C:\dev\Half-Sword-VR-`, branch `phase0-groundwork` (PR #1), submodules initialised. Untracked build folders: `build\` (xrtest), `build-mod\` (mod + UE4SS).
- Toolchain: VS Community 2022 17.14 (MSVC 14.44, SDK 10.0.26100). VS Build Tools 2026 is also installed, so always pass `-G "Visual Studio 17 2022" -A x64`. CMake 4.4.4, Git 2.55, Rust 1.99.1 (needed by UE4SS).
- Submodules: `git -c url."https://github.com/".insteadOf="git@github.com:" submodule update --init --recursive` (UE4SS lists SSH URLs; Matthew's HTTPS credentials reach the Epic-gated UEPseudo).
- Mod: `cmake -S mod -B build-mod -G "Visual Studio 17 2022" -A x64` then `cmake --build build-mod --config Game__Shipping__Win64 --target HalfSwordVR`. Output `build-mod\HalfSwordVR\Game__Shipping__Win64\main.dll`.
- Install: copy main.dll to `C:\Games\Steam\steamapps\common\Half Sword\HalfswordUE5\Binaries\Win64\ue4ss\Mods\HalfSwordVR\dlls\main.dll` with an empty `Mods\HalfSwordVR\enabled.txt`. Log: `ue4ss\UE4SS.log`.
- Remote Control auto-mode blocks downloads/installs unless Matthew names the action in his own message.
- Keep the vorpX app closed when testing (its service idles in the background; harmless).

## Engine stereo facts (UE 5.4.4)
- Reference headers (not for the repo): sparse checkout at `C:\dev\ue-5.4.4-ref` (HeadMountedDisplay, StereoRendering.h, StereoRenderTargetManager.h, UnrealEngine.cpp, Engine.h, XRBase plugin, OpenXRHMD plugin).
- `-emulatestereo` (or read-only cvar `r.EnableStereoEmulation=1`) makes `UEngine::InitializeHMDDevice` create `FFakeStereoRenderingDevice` and assign only `UEngine::StereoRenderingDevice`; `XRSystem` stays null. Confirmed working in this build (correct side-by-side stereo).
- `UEngine` members (Engine.h): `TSharedPtr<IStereoRendering, ThreadSafe> StereoRenderingDevice;` immediately followed by `TSharedPtr<IXRTrackingSystem, ThreadSafe> XRSystem;`. Offsets not yet measured.
- `IStereoRendering` vtable order (MSVC, slot 0 = deleting dtor): 1 IsStereoEnabled, 2 IsStereoEnabledOnNextFrame, 3 EnableStereo, 4 GetDesiredNumberOfViews, 5 GetViewPassForIndex, 6 GetLODViewIndex, 7 IsStandaloneStereoOnlyDevice, 8 AdjustViewRect, 9 SetFinalViewRect, 10 GetTextSafeRegionBounds, 11 CalculateStereoViewOffset, 12 GetStereoProjectionMatrix, 13 InitCanvasFromView, 14 RenderTexture_RenderThread, 15 GetRenderTargetManager, 16 GetStereoLayers, 17-19 Start/Override/EndFinalPostprocessSettings. Verify against the binary before relying on it.
- `FFakeStereoRenderingDevice` layout: vtable, float FOVInDegrees, int32 Width, Height, NumViews (=2).

## Suggested first step (not started)
Phase 1a without any D3D12 interop: launch with `-emulatestereo`, find the fake device by scanning the `GameEngine` instance for a TSharedPtr whose object matches that layout (vtable inside the exe image, NumViews 2, sane FOV/size), copy its vtable, and replace only slots 11 and 12 with ours: per-eye pose from OpenXR (own D3D12 device just to run the session, no layers submitted) and the asymmetric OpenXR projection. Head tracking then moves the side-by-side monitor view, which proves the vtable ABI and coordinate conversion. Phase 1b then adds a render target manager and copies each eye into the OpenXR swapchain on the engine's D3D12 queue.
- Coordinates: OpenXR (-Z fwd, +X right, +Y up, m) to UE (X fwd, Y right, Z up, cm): `FVector(-z, x, y) * WorldToMeters`, `FQuat(-q.z, q.x, q.y, -q.w)` (as in UE's OpenXRHMD).

## Open observations
- In the `-emulatestereo` screenshots the right view looked softer and limbs differed slightly between views. Suspect temporal AA/upscaler history shared across views; check with AA/upscaling off.
- Steam Frame wants 2480 x 2480 per eye; frame generation won't work in VR, DLSS upscaling is untested.
