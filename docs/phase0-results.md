# Phase 0 results (2026-11-14)

| Check | Result |
| --- | --- |
| Engine | Unreal Engine 5.4.4 Shipping, D3D12. No anti-cheat. Pak v11, index encrypted. |
| UE4SS | Installed build is commit e3ba1016 ("3.0.1 Beta"); our C++ mod builds against it and loads cleanly. |
| Hello mod | Loaded, saw Unreal initialise, found the player (`Willie_BP_DressUp_C` in the tavern hub). No errors or crashes. |
| xrtest (OpenXR) | SteamVR OpenXR 2.19.3, Steam Frame. 2480 x 2480 per eye recommended. ~2,300 frames rendered, world-fixed grid confirmed in headset. |
| `-emulatestereo` | Not yet confirmed. The first screenshots were produced by vorpX (its service was running), not the engine. Rerun pending with vorpX closed. |

Conclusion: pending the `-emulatestereo` rerun. Keep vorpX closed whenever testing this mod.
