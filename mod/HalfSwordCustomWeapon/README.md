# HalfSwordCustomWeapon

Adds weapons made with WeaponConverter (`tools/weapon_converter_app.py`) to Half Sword as **new weapons**, built while the game runs from model files, with no editor and no cooked asset. No game weapon is changed.

## How it works

- **Packs:** the converter's Install writes one folder per weapon, `weapons/<id>/`, with the model (`.obj`), its collision pieces (`_collision.obj`), its numbers (`.txt`), textures and `pack.txt` (title, free play, progression).
- **Carrier passports:** the game keeps every weapon as a passport (`Str_Passport_Weapon1`). A converted weapon is the passport of a game weapon of the same kind (arming, bastard or longsword by grip length, dagger, mace, great axe or spear), copied from the game's own free mode list, with a marker Name (`cw <hash>`, or `cw sword <hash>` for swords).
- **Free mode:** after GI_Settings `Set Up Free Mode Inventory` builds the inventory list, the packs marked for free play are appended.
- **Spawning:** after every ModularWeaponBP's BeginPlay, a weapon whose passport Name is a marker gets the converted model, its collision, mass, damage points, hand spots and display name.
- **Saves:** the game saves the passport unchanged, so without this mod a saved converted weapon is simply the carrier weapon; reinstalling brings it back.

The log is `CustomWeapon.log` in this folder; settings are in `CustomWeapon.ini`. Decompile notes: `HalfSwordDecomp\defaults\add_new_weapons.md` and `carrier_passport_impl.md` (not in the repo).

## Not yet

Progression (merchant), scabbard choice, flails, shields in the game (the converter already makes them) and the crossbow are the next steps. Done: own damage points per weapon (edges, point, blunt, rigidity, hit boxes, Base), balance by part masses, mass/centre of mass/lengths measured again after the swap, double-ended weapons, .fbx import (tools/fbx_reader.py, no extra packages).

## Install

1. Copy `dlls/main.dll` and `CustomWeapon.ini` to `Half Sword/HalfswordUE5/Binaries/Win64/ue4ss/Mods/HalfSwordCustomWeapon/`.
2. Add an empty `enabled.txt` in that folder, or add `HalfSwordCustomWeapon : 1` to `Mods/mods.txt`.
