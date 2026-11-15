# Phase 0 results (2026-11-14)

| Check | Result |
| --- | --- |
| Engine | Unreal Engine 5.4.4 Shipping, D3D12. No anti-cheat. Pak v11, index encrypted. |
| UE4SS | Installed build is commit e3ba1016 ("3.0.1 Beta"); our C++ mod builds against it and loads cleanly. |
| Hello mod | Loaded, saw Unreal initialise, found the player (`Willie_BP_DressUp_C` in the tavern hub). No errors or crashes. |
| xrtest (OpenXR) | SteamVR OpenXR 2.19.3, Steam Frame. 2480 x 2480 per eye recommended. ~2,300 frames rendered, world-fixed grid confirmed in headset. |
| `-emulatestereo` | Pass. Engine renders two full side-by-side views with lighting, shadows and fog and a visible eye offset (vorpX app closed, confirmed by its logs). The right view looks somewhat softer, and limbs differ slightly between views; suspect temporal AA/upscaler history shared across views. To check in Phase 1. |

Conclusion: the engine's own stereo path works in this build, so Phase 1 can use a native stereo device instead of alternating eyes. Keep vorpX closed whenever testing this mod.
