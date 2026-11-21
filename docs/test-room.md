# Test room (ini `test_room=1`)

**The way in: the Train button (Free Mode).** In Free Mode the tavern's Train (the fencer) opens a dialog that the game made without a Train button (only Play Progression and Leave). With `test_room_button=1` (default) the mod adds a **Train** button there, above Leave and drawn like it. Clicking it, with the mouse or the VR panel pointer, takes you into the test room instead of the cellar. The game's own Training is never run, so neither Training mode nor the cellar is written into the save. Progression's Train still goes to the cellar. Details: `defaults\train_button.md` in the decompile folder.

With `test_room=1` the game instead goes from the first menu straight into the test room. Either way the room is a Free Mode arena set up for testing:

- `test_room_foes` Willies (default 4, 1-10) whose AI is switched off. They never attack, move, kick or block, but keep the game's own physics: they stand, balance, fall when hit or pushed, get up again, can be grabbed, punched, stabbed and killed. They are stood in an arc 2.2 m behind you, facing you. A foe whose AI comes back on (zombie revive) is switched off again.
- Two of the game's weapon tables (`BP_Container_Weapon_Table_1`) 1.1 m in front of you, 35 cm apart, stocked through the game's own loot point (`BP_Loot_Weapon`, Spawn Chance 1) with one of each: 1H sword, 2H sword, 1H mace, 2H mace, shield, 1H hafted, 2H hafted, polearm, secondary (the game's secondary kind can be a bottle). The tables' own random loot is switched off (it landed on the same spots and threw the weapons). When the game has no weapon of a kind at the chosen tier, the nearest other tier is used (log: `the game has none at tier N`).
- The foes are moved with their physics off and get it back 0.4 s later at rest (a teleport of a simulating Willie throws him about 5 m).
- `hsvr.test_room_report` (live command, HalfSwordVR_commands.txt) logs every table, loose weapon and foe (`TestRoom: check:`); `TestRoom: watch` lines follow everything near the tables that moves in the room's first 5 s.
- **Respawn** (`test_room_respawn=1`, default): a foe who is killed (`DED`) or beaten (`Give Up`: he went under 5 health and yielded) is out. His body is removed `test_room_despawn_seconds` (default 7) later with the game's own `Destroy Willie`, and his slot gets a new foe `test_room_respawn_seconds` (default 10) after he was out: made the way the arena's spawner makes a Free Mode foe (`BP_Generator_Characters_Random` "Generate Character" at `test_room_tier`, a Willie_BP with that passport, Team Int 2, sized by his Height; AI_BP takes him by itself), made passive at once, standing in the slot facing you (1 m further back if a body still lies there). A body you are using is never removed: one of your hands grabs him or a piece of him, you hold one of his weapons, or a weapon in your hand is stuck in a body within 150 cm of him; it is removed once you let go. When every foe is out at once, the one out longest comes back immediately. While respawn is on the match is never won: the game mode's Enemy Count is held at 100 or more, and a win the game calls anyway is undone. Log lines `TestRoom: respawn: ...`.
- Test commands (HalfSwordVR_commands.txt): `hsvr.test_room_kill [slot]` (Health 0 and the foe's Death event), `hsvr.test_room_beat [slot]` (Health 0 only: he goes down and gives up).
- The tables are physics props (very heavy, but they can be shoved): a long weapon swung into them knocks them and what lies on them.

| Setting | Default | Meaning |
|---|---|---|
| `test_room` | 0 | 1 = boot straight into the test room. Read at launch. |
| `test_room_button` | 1 | 1 = in Free Mode the fencer's dialog has a Train button that opens the test room. |
| `test_room_map` | /Game/Maps/Arenas/Map_Arena_Yard | The arena (Yard and Pit are flat and open; up to 10 foes in any arena). |
| `test_room_foes` | 4 | Number of passive Willies (1-10). |
| `test_room_tier` | -1 | Their tier (armour, weapons): -1 random each, 0 Beggar .. 8 Lord. |
| `test_room_weapon_tier` | -1 | The table weapons' tier: -1 random 0-7 each, 0 Beggar .. 8 Lord. |
| `test_room_respawn` | 1 | 1 = a killed or beaten foe is replaced; 0 = the dead stay dead (and killing them all wins the match as usual). |
| `test_room_respawn_seconds` | 10 | Seconds from a foe being out to the new foe in his slot. |
| `test_room_despawn_seconds` | 7 | Seconds from a foe being out to his body being removed (0 = bodies stay). |

## Your save

The mod never saves the test settings. At boot it copies `GameProgress.sav` to `save_backups\GameProgress.sav.<UTC time>.bak` in the mod folder (one per launch, never deleted); without that copy there is no test room. The Free Mode values go into the game instance only, and a hook on GI_Settings "Load Game" (which the arena calls on start) puts them back after the arena reads the save. Every save the game writes while the room is up (arena start, ~1 s, death, pause menu) is undone at once by copying that launch's copy back. Leaving the room map (pause menu > Menu) puts the save back, loads it into the game instance and restores every game-instance value the room changed, so normal play after it starts from your real save. Log lines start with `TestRoom:`.

Facts from the decompile: `defaults\test_room.md` in the decompile folder.

Test commands: `hsvr.widget_call <widget class> <function>` (for example the tavern's `UI_Hub_Tavern_C` Fencer click event), `hsvr.train_click` (what the Train button does). The button logs where it is on the desktop (`TestRoom: Train button at desktop (x, y)`).
