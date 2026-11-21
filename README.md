# Half Sword VR

A from-scratch native VR mod for Half Sword (Unreal Engine 5.4.4), targeting SteamVR through OpenXR. Single-player only.

Status: Phase 1. The game shows in the headset with head tracking; controls are still mouse and keyboard. See `docs/phase1-results.md` for what works, the keys and the settings.

## What's in the repo

| Path | What it is |
| --- | --- |
| `mod/HalfSwordVR` | The in-game mod, a UE4SS C++ mod: head-tracked stereo view through OpenXR. |
| `deps/RE-UE4SS` | UE4SS source, pinned to the exact build installed in the game (commit e3ba1016). |
| `tools/xrtest` | A standalone headset test. Shows a room-fixed grid in the headset and logs the head pose, to confirm SteamVR + OpenXR + Direct3D 12 work on your PC. |

## Building (Windows)

Needs Visual Studio 2022 or the Build Tools with the C++ workload, plus CMake and Git.

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The OpenXR SDK is downloaded automatically during the first configure.

## Building the mod

Also needs the Rust toolchain (for UE4SS) and a GitHub account linked to Epic Games (UE4SS pulls Unreal headers from a private repo).

```
git -c url."https://github.com/".insteadOf="git@github.com:" submodule update --init --recursive
cmake -S mod -B build-mod -G "Visual Studio 17 2022" -A x64
cmake --build build-mod --config Game__Shipping__Win64 --target HalfSwordVR
```

Install by copying `main.dll` to `HalfswordUE5\Binaries\Win64\ue4ss\Mods\HalfSwordVR\dlls\main.dll` and creating an empty `enabled.txt` in `Mods\HalfSwordVR`. Remove that folder to uninstall.

**VR or flat is chosen at launch** (`vr_auto=1`, the default): start SteamVR first and the game starts in VR (and, with `test_room=1`, in the test room). Start the game without SteamVR and it plays flat with the mod switched off for that session: no OpenXR, so SteamVR is not opened, and no hooks. `vr_auto=0` always starts VR (OpenXR opens SteamVR if it is closed). `vr_auto_processes` lists the process names that count as "SteamVR is running" (default `vrserver.exe,vrmonitor.exe`).

## Running the headset test

1. Start SteamVR and put on the headset.
2. Run `build\tools\xrtest\Release\xrtest.exe`.
3. You should see a dark grid that stays put as you turn your head; left eye tinted red, right eye blue. Press Esc to quit (it also stops after 2 minutes).
4. The result is written to `xrtest.log` next to the exe.

## License

Our own code only. No game files, Unreal Engine source, or UEVR code are included.
