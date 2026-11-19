# Test room (ini `test_room=1`)

Launching the game with `test_room=1` in `HalfSwordVR.ini` goes from the first menu straight into a Free Mode arena set up for testing:

- `test_room_foes` Willies (default 4, 1-10) whose AI is switched off. They never attack, move, kick or block, but keep the game's own physics: they stand, balance, fall when hit or pushed, get up again, can be grabbed, punched, stabbed and killed. They are stood in an arc 2.2 m behind you, facing you. A foe whose AI comes back on (zombie revive) is switched off again.
- Two of the game's weapon tables (`BP_Container_Weapon_Table_1`) 1.1 m in front of you, stocked through the game's own loot point (`BP_Loot_Weapon`, Spawn Chance 1) with one of each: 1H sword, 2H sword, 1H mace, 2H mace, shield, 1H hafted, 2H hafted, polearm, secondary. Each table also stocks one random piece of its own (weapon, maybe armour).

| Setting | Default | Meaning |
|---|---|---|
| `test_room` | 0 | 1 = boot into the test room. Read at launch. |
| `test_room_map` | /Game/Maps/Arenas/Map_Arena_Yard | The arena (Yard and Pit are flat and open; up to 10 foes in any arena). |
| `test_room_foes` | 4 | Number of passive Willies (1-10). |
| `test_room_tier` | -1 | Their tier (armour, weapons): -1 random each, 0 Beggar .. 8 Lord. |
| `test_room_weapon_tier` | -1 | The table weapons' tier: -1 random 0-7 each, 0 Beggar .. 8 Lord. |

## Your save

The mod never saves the test settings. At boot it copies `GameProgress.sav` to `save_backups\GameProgress.sav.<UTC time>.bak` in the mod folder (one per launch, never deleted); without that copy there is no test room. The Free Mode values go into the game instance only, and a hook on GI_Settings "Load Game" (which the arena calls on start) puts them back after the arena reads the save. Every save the game writes while the room is up (arena start, ~1 s, death, pause menu) is undone at once by copying that launch's copy back. Leaving the room map (pause menu > Menu) puts the save back, loads it into the game instance and restores every game-instance value the room changed, so normal play after it starts from your real save. Log lines start with `TestRoom:`.

Facts from the decompile: `defaults\test_room.md` in the decompile folder.
