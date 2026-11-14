# Half Sword VR

A from-scratch native VR mod for Half Sword (Unreal Engine 5.4.4), targeting SteamVR through OpenXR. Single-player only.

Status: Phase 0 (groundwork). Nothing here changes the game yet.

## What's in the repo

| Path | What it is |
| --- | --- |
| `mod/HalfSwordVR` | The in-game mod, a UE4SS C++ mod. Phase 0 version only logs that it loaded and finds the player. |
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

## Running the headset test

1. Start SteamVR and put on the headset.
2. Run `build\tools\xrtest\Release\xrtest.exe`.
3. You should see a dark grid that stays put as you turn your head; left eye tinted red, right eye blue. Press Esc to quit (it also stops after 2 minutes).
4. The result is written to `xrtest.log` next to the exe.

## License

Our own code only. No game files, Unreal Engine source, or UEVR code are included.
