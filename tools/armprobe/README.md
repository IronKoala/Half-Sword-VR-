# Arm probe (Phase 2 research tool)

Read-only UE4SS Lua mod that maps how Half Sword drives the arms. See
[docs/phase2-arm-research.md](../../docs/phase2-arm-research.md).

Install: copy this folder (with `Scripts`) to
`HalfswordUE5\Binaries\Win64\ue4ss\Mods\HalfSwordVR_ArmProbe\` and create an empty
`enabled.txt` in it. Remove the folder (or empty main.lua) to uninstall.

No hotkeys: version 1 and 2 used UE4SS key binds (plus an `os.execute` mkdir) and
menu clicks stopped registering on this game build. Version 3 runs once by itself:
when you enter a fight with a weapon it writes `armprobe-dump-*.txt`, waits 10 s, records
30 s to `armprobe-rec-*.txt`, then stops. Files go into the mod folder.
