# Phase 1 results: head-tracked VR view (2026-11-15)

Phase 1 goal: the game shown in the headset through SteamVR/OpenXR, the view in the character's head following the player's real head, mouse and keyboard controls unchanged.

## What works

| Item | State |
| --- | --- |
| VR start | Starts when SteamVR is running at game launch; otherwise the game starts flat. |
| Rendering | The engine draws both eyes into its own texture at headset size (3728 x 1864 for two 1864 x 1864 eyes at `render_scale=0.75`). 72 fps, 0 dropped frames in every headset session on a Steam Frame (RTX 4080 SUPER). |
| Head tracking | View sits at the character's head bone (lightly smoothed), heading follows the game camera, Home recenters. |
| Menus and HUD | The game window's flat picture (menus, HUD, the game's edge overlays) floats on a panel 1.1 m in front, with a dark sheet behind it while a menu is up. The monitor shows the left eye with the menus. |
| Controls | Mouse and keyboard as in the flat game. Esc pauses and unpauses. |

Played by Matthew in the headset: "everything looks good" (commit 0a2e88e).

## Keys

| Key | Does |
| --- | --- |
| Home | Recenter the view and the menu panel |
| End | Hide or show the menu panel |
| Insert | Switch between the view from the head and the game's own camera |
| Page Down | Flip between full-resolution mode and the fallback through the game window |

## Use

- Start SteamVR and wake the headset, then start Half Sword from Steam.
- Leave the game in its normal third-person view. With the game's own first-person view on, the heading follows the physics-driven head and wanders.
- Settings: `Mods\HalfSwordVR\HalfSwordVR.ini` (written with defaults on first run, re-read while the game runs). Log: `HalfSwordVR.log` in the same folder.

## How it is built

- Our own object implements the engine's stereo rendering interface (`IStereoRendering`, plus `IStereoRenderTargetManager`) and is written into `UEngine::StereoRenderingDevice`, found at runtime between two reflected neighbours (offset 0xE30 in this build). `XRSystem` stays empty, so none of the engine's own HMD code paths run. Mirror declarations live in `ue_abi.hpp`; no engine source is in the repo.
- The OpenXR session runs on the game's own D3D12 queue. A D3D12 swap chain does not hand out its queue, so the mod hooks `ID3D12CommandQueue::ExecuteCommandLists` and `IDXGISwapChain::Present` and picks the queue that belongs to the game's swap chain. Eyes are copied to the OpenXR swap chains inside Present.
- A pose travels with its frame from the game thread through the render thread to Present. The pipeline is three frames deep; the prediction lead settles at 41.7 ms at 72 Hz.
- `Slate.DrawToVRRenderTarget 0` keeps menus and HUD in the game window, which is copied to the panel (black is treated as see-through).

## Findings worth keeping

- The game's HUD (`UI_HUD_C`) holds full-screen edge overlays named `Vignette_Hot`, `Vignette_Cold`, `Vignette_Pain`, `Vignette`, `Vignette_WakeUp`. They are game information and are shown as the game draws them. On the panel they appear as a frame around the panel, not around the whole view.
- The game hides the Windows mouse pointer in menus; `PlayerController.bShowMouseCursor` tells when a menu wants one.
- The engine's input, pause and hit-testing code has no special case for stereo when `XRSystem` is empty, and the viewport size stays the window size.
- A character head-hiding feature was built and removed again at Matthew's request.
- One fatal error happened in a headset session (render thread, inside the game's own code, nothing of the mod on that thread's stack). The crash folder on that PC holds about twenty earlier crashes of the game going back to 2025, three with the same shape, so it is probably a crash the game already had.

## Open

- Idle helper (`idle_helper`): keeps the game at normal speed while the headset sleeps. Written; the handoff when the headset wakes has not been tried in the headset.
- Insert and Page Down have not been confirmed in the headset.
- Comfort: nothing is done about the game's camera shake or about being knocked down.
- Sharpness: `render_scale` is 0.75 of the headset's recommended size. GPU time was about 10.5 ms of the 13.9 ms budget in a fight, so there is little room to raise it without an upscaler.
- The aim marker and other HUD parts sit on the panel, not in the world.
