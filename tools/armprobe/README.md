# Arm probe (Phase 2 research tool)

Read-only UE4SS Lua mod that maps how Half Sword drives the arms. See
[docs/phase2-arm-research.md](../../docs/phase2-arm-research.md) for why and what it records.

Install: close the game, copy this folder (with `Scripts`) to
`HalfswordUE5\Binaries\Win64\ue4ss\Mods\HalfSwordVR_ArmProbe\`, and create an empty
`enabled.txt` in it. Remove the folder to uninstall.

In a match: **F7** = full dump, **F8** = 8-second recording. Files appear in the mod's
`output` folder.
