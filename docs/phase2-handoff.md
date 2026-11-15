# Handoff from Phase 1 to Phase 2 (2026-11-15)

Phase 2 in the plan: learn how the game drives the character's arms, as the groundwork for motion controls (Phase 3: the headset's controllers steer the sword). Top priority throughout: accurate sword control.

Read `docs/phase1-results.md` first for what the mod does today.

## What Phase 1 leaves ready

- Build, install and test routine on Matthew's PC: unchanged from `docs/phase1-handoff.md`. Branch `claude/project-thread-un902v` (PR #2, stacked on PR #1).
- A running OpenXR session inside the game (`vr_present.cpp`, `xr_loader.cpp`). It asks only for head and eye poses. There is no action set yet: no controller poses, buttons, sticks or haptics.
- Game-thread glue through UE4SS (`dllmain.cpp`): finds the player controller and pawn every tick, calls Blueprint-visible functions with `ProcessEvent`, reads properties by name (`object_property`, `bool_property`).
- Coordinate conversion between OpenXR tracking space and the game world, including recentering and the head anchor (`stereo_device.cpp`, `vrmath.hpp`). A controller pose can go through the same path to become a world position next to the character.
- Log aids that stay useful: "Input:" lines (pause state, whether the game wants a pointer, turn rate), a list of on-screen widgets, and optional pictures (`capture_seconds`).

## Known facts about the character (from the live game)

- Pawn `Willie_BP_C`; main mesh `CharacterMesh0` (`SK_Body_Man`, 67 bones, head bone `Head`); a separate head model `CharacterMesh_Head`; a low-poly "driver" skeleton `DriverSkeleton` (`Willie_Lopoly`) that the physical body follows; a gore mesh `SK_Skeleton`.
- From the earlier read-only observer mod: animation Blueprint `ABP_Willie` (copies the pose from the driver skeleton), a `PhysicalAnimationComponent` with a strength multiplier, constraint angular drives, inputs named "Axis Value X/Y" (with interpolated versions), `R_Guarding`, `L_Guarding`, `R_Thrusting`, "R Two Handed Grip", and the weapon slot "Weapon R".
- The game has its own first-person camera on the head and a third-person follow camera. The mod works with the game left in third person.
- The tavern hub uses a different pawn (`NewBlueprint_C`) and a full-screen 2D menu (`UI_Hub_Tavern_C`).

## Not known yet (the Phase 2 questions)

1. Which values turn mouse movement into arm and weapon movement, and where they live (pawn variables, the animation Blueprint, the driver skeleton's bones).
2. Whether the hands follow targets we can set (for example by moving bones or targets on the driver skeleton) or only react to the mouse axes.
3. How grip, guard, thrust and two-handed states change that.
4. How strong the physical drives are, since they decide how closely a hand can follow a real controller.

## Suggested first steps

1. Add an OpenXR action set with the two grip poses and log them in game-world coordinates. No effect on the game yet.
2. In a fight, log the candidate variables above at a steady rate while Matthew plays with the mouse, and match them to what the arms do.
3. Try the smallest possible intervention on one hand (for example writing the mouse-axis values from a controller's motion) before attempting hand targets.

## Rules that carry over

- Never send automated keys or clicks to Matthew's PC. Announce automatic game launches in the thread before they start and when they end.
- No UEVR. Leave the `HalfPortibleObserver` mod alone. Keep the vorpX app closed during tests. No engine source in the repo.
- Do not hide the game's overlays or the menu panel; they carry game information. Do not add visible extras that were not asked for.
