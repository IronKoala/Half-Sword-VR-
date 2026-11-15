# Phase 1 results: head-tracked VR view (2026-11-15)

Phase 1 goal: the game shown in the headset through SteamVR/OpenXR, the view in the character's head following the player's real head, mouse and keyboard controls unchanged.

## What works

| Item | State |
| --- | --- |
| VR start | Starts when SteamVR is running at game launch; otherwise the game starts flat. |
| Rendering | The engine draws both eyes into its own texture at headset size (3728 x 1864 for two 1864 x 1864 eyes at `render_scale=0.75`). 72 fps, 0 dropped frames in every headset session on a Steam Frame (RTX 4080 SUPER). |
| Head tracking | View sits at the character's head bone (lightly smoothed), heading follows the player's aim (`heading=aim`), Home recenters. |
| Menus and HUD | The game window's flat picture (menus, HUD, the game's edge overlays) floats on a panel 1.1 m in front, with a dark sheet behind it while a menu is up. The monitor shows the left eye with the menus. |
| Controls | Mouse and keyboard as in the flat game. Esc pauses and unpauses. |

Played by Matthew in the headset: "everything looks good" (commit 0a2e88e).

## Keys

| Key | Does |
| --- | --- |
| Home | Recenter the view and the menu panel |
| End | Hide or show the menu panel during play (menus always show) |
| Insert | Switch between the view from the head and the game's own camera |
| Page Down | Flip between full-resolution mode and the fallback through the game window |

## Use

- Start SteamVR and wake the headset, then start Half Sword from Steam.
- Leave the game in its normal third-person view. With the game's own first-person view on, the heading follows the physics-driven head and wanders.
- Settings: `Mods\HalfSwordVR\HalfSwordVR.ini` (written with defaults on first run, re-read while the game runs). Log: `HalfSwordVR.log` in the same folder.

## How it is built

- Our own object implements the engine's stereo rendering interface (`IStereoRendering`, plus `IStereoRenderTargetManager`) and is written into `UEngine::StereoRenderingDevice`, found at runtime between two reflected neighbours (offset 0xE30 in this build). `XRSystem` stays empty, so none of the engine's own HMD code paths run. Mirror declarations live in `ue_abi.hpp`; no engine source is in the repo.
- The OpenXR session runs on the game's own D3D12 queue. A D3D12 swap chain does not hand out its queue, so the mod hooks `ID3D12CommandQueue::ExecuteCommandLists` and `IDXGISwapChain::Present` and picks the queue that belongs to the game's swap chain. Eyes are copied to the OpenXR swap chains inside Present.
- A pose is taken on the game thread and matched to its picture at Present, by order. The engine cannot run more than two Presents ahead (the game thread waits for the render thread of two frames before, which waits for the Present of three frames before), so the prediction lead settles at 27.8 ms at 72 Hz, and a pose older than two finished Presents is dropped (`pose_max_age`). The log line "Pairing:" shows the ages. An earlier version took the pose on the render thread and could stay one frame out for good (lead 41.7 ms): the world swam and juddered on head turns.
- `Slate.DrawToVRRenderTarget 0` keeps menus and HUD in the game window, which is copied to the panel (black is treated as see-through).

## Findings worth keeping

- The game's HUD (`UI_HUD_C`) holds full-screen edge overlays named `Vignette_Hot`, `Vignette_Cold`, `Vignette_Pain`, `Vignette`, `Vignette_WakeUp`. They are game information and are shown as the game draws them. On the panel they appear as a frame around the panel, not around the whole view.
- The game hides the Windows mouse pointer in menus; `PlayerController.bShowMouseCursor` tells when a menu wants one.
- The engine's input, pause and hit-testing code has no special case for stereo when `XRSystem` is empty, and the viewport size stays the window size.
- A character head-hiding feature was built and removed again at Matthew's request.
- Clouds in front of everything in the right eye: Unreal 5.4.4 has a fault in volumetric cloud mode 1 (`r.VolumetricRenderTarget.Mode 1`, also mode 3). The cloud trace adds the full-resolution view rect offset to a half-resolution depth lookup (`VolumetricCloudRendering.cpp`, where `TracingCoordToZbufferCoordScaleBias` is set), so a view that does not start at x = 0 reads no depth and its clouds cover the scene. Half Sword switches to mode 1 when the Yard arena loads. The mod checks the mode every frame and replaces it (`fix_clouds`), by default with mode 2 (`cloud_mode`). Mode 0 was used first, but it lays the clouds over the scene from a half-size picture whose depth misses thin far things, so a steeple and tree leaves against the sky flickered on the Slums map; mode 2 tests every full-size pixel. (Mode 2 was active when the flicker was looked at again and the flicker stayed, so the clouds are not its cause; mode 2 is kept as the cleaner choice.) `r.VolumetricRenderTarget 0` is no way out: that path draws each view's clouds over the whole shared target.
- The game sets `r.ScreenPercentage 55` with TSR and switches DLSS off when the hub loads, overriding an earlier value from the mod's `screen_percentage`.
- One fatal error happened in a headset session (render thread, inside the game's own code, nothing of the mod on that thread's stack). The crash folder on that PC holds about twenty earlier crashes of the game going back to 2025, three with the same shape, so it is probably a crash the game already had.

## Open

- Idle helper (`idle_helper`, on by default): keeps the game at normal speed while the headset sleeps. A 7-minute run with the headset asleep held 72 frames a second throughout and SteamVR stayed up (without it the game fell to about 6 fps after 5 minutes and SteamVR quit). The handoff when the headset wakes has not been tried in the headset.
- Comfort: the heading follows the player's aim (it measured the same as the camera's heading except at spawn). Nothing is done about being knocked down.
- Sharpness: `render_scale` is 0.75 of the headset's recommended size, with mild sharpening (`sharpen=0.6`). At 0.90 the GPU time rose to 13.6 ms of the 13.9 ms budget (10.5 to 12.9 ms at 0.75), so 0.75 stays.
- Old mouse pointers pile up in the see-through arch of the equipment screen (`UI_Tavern_Inventory_C`), and a menu closed during play stays visible on the panel. Pictures prove that the window buffer is black after the mod wipes it and that the next Present still carries the old picture in every place the game's menu pass did not draw over. Ruled out: Unreal's Slate post buffers (`Slate.CopyBackbufferToSlatePostRenderTargets` reads 0 in this game), and the inventory's own images, which all draw plain textures. Working explanation: the game has AMD's FSR frame generation plugin compiled in, whose swap chain override (`r.FidelityFX.FI.OverrideSwapChainDX12`) hands the engine a proxy swap chain with its own stand-in buffers and copies them into the real ones at each Present; the real swap chain's flags (0x842, with the waitable-object bit the engine never sets itself) fit. The mod's wipe reached only the real buffers. Since this commit the mod recognises the stand-ins by that copy (it watches `CopyResource` and `CopyTextureRegion` for whole-picture copies into a window buffer) and wipes the one the game draws on next (`wipe_standins`). Not yet confirmed in the headset.
- Far, thin things (a steeple, tree leaves) flickered on the Slums map, in both eyes, more in the right. Found with the Page Up picture tests (`test_key`, a temporary aid): switching Lumen global illumination off stopped it but made the plants look flat. Half Sword runs Lumen at `r.Lumen.ScreenProbeGather.DownsampleFactor 64` (one light sample per 64 pixels; hardware ray tracing on, `sg.GlobalIlluminationQuality 0`). On a monitor that is about 30 samples across the picture, in the headset about 16 per eye, because each eye is drawn only about 1000 pixels wide. With 32 most of the flicker is gone and the plants look better, and of 32, 16 and 8 the player preferred 16, so the mod keeps the value at 16 (`light_samples`, checked every frame because the game sets it again on a map load). Cost on the Slums map: 63 to 72 pictures a second with it against 70 to 72 just before. Not the cause: cloud mode, sharpening, the upscaler's settings and method, fog, occlusion culling, and within Lumen: screen traces, short-range AO, stochastic interpolation, sample jitter, longer accumulation, probe filtering (each looked at for only a few seconds). Two unattended runs with the pretend headset in a paused world did not show the flicker at all. `r.ScreenPercentage 100` as a test dropped the game to about 9 fps and the SteamVR wireless link driver stopped with a fatal error, so do not test with it.
- The aim marker and other HUD parts sit on the panel, not in the world.
