# HalfSwordCustomWeapon

Adds weapons made with WeaponConverter (`tools/weapon_converter_app.py`) to Half Sword as **new weapons**, built while the game runs from model files, with no editor and no cooked asset. No game weapon is changed.

## How it works

- **Packs:** the converter's Install writes one folder per weapon, `weapons/<id>/`, with the model (`.obj`), its collision pieces (`_collision.obj`), its numbers (`.txt`), textures and `pack.txt` (title, free play, progression).
- **Carrier passports:** the game keeps every weapon as a passport (`Str_Passport_Weapon1`). A converted weapon is the passport of a game weapon of the same kind (arming, bastard or longsword by grip length, dagger, mace, great axe or spear), copied from the game's own free mode list, with a marker Name (`cw <hash>`, or `cw sword <hash>` for swords).
- **Free mode:** after GI_Settings `Set Up Free Mode Inventory` builds the inventory list, the packs marked for free play are appended.
- **Spawning:** after every ModularWeaponBP's BeginPlay, a weapon whose passport Name is a marker gets the converted model, its collision, mass, damage points, hand spots and display name.
- **Progression:** after GI_Settings `Renew Merchant`, packs marked for progression are offered from their rank (pack.txt `tier`, `price`).
- **Scabbards (swords):** pack.txt `scabbard=` `fit` (the shortest of the game's 8 scabbards that covers the blade; stretched if none is long enough), one of them by name (`SS25` `SS45` `AS60` `AS70` `LS75` `BS85` `BS100` `LS110`), `own` (`<model>_scabbard.obj` + `_scabbard_collision.obj` made by `tools/scabbard.py` at Install) or `none` (SheathSlot 1, belt).
- **Shields:** carried by the game's `Shield_Buckler` (fist grip), `Shield_Tagre` or `Shield_Pavise_Light` (forearm straps), pack.txt `carrier=shield_buckler|shield_targe|shield_pavise`. The model's collision, mass, hit box, damage points and visual go on that class's shield component (front +Y), its back on the game plate's back.
- **Flails:** carried by `ModularWeaponBP_Flail_A`. The converter (type Flail) lays staff, chain and head out straight; the staff goes on `Grip` (its top on the first joint), the chain on `Link 1`, the head on `Blade1` (`Head` keeps only a small connector), the second joint moves to the chain's end and both joints are re-initialised.
- **Saves:** the game saves the passport unchanged, so without this mod a saved converted weapon is simply the carrier weapon; reinstalling brings it back.

The log is `CustomWeapon.log` in this folder; settings are in `CustomWeapon.ini`. Decompile notes: `HalfSwordDecomp\defaults\add_new_weapons.md` and `carrier_passport_impl.md` (not in the repo).

## Not yet

The crossbow is the next step. Untested in the game so far: progression, scabbards, shields and flails. Done: own damage points per weapon (edges, point, blunt, rigidity, hit boxes, Base), balance by part masses, mass/centre of mass/lengths measured again after the swap, double-ended weapons, .fbx import (tools/fbx_reader.py, no extra packages).

## Install

1. Copy `dlls/main.dll` and `CustomWeapon.ini` to `Half Sword/HalfswordUE5/Binaries/Win64/ue4ss/Mods/HalfSwordCustomWeapon/`.
2. Add an empty `enabled.txt` in that folder, or add `HalfSwordCustomWeapon : 1` to `Mods/mods.txt`.
