# Test room (ini `test_room=1`)

**The way in: the Train button (Free Mode).** In Free Mode the tavern's Train (the fencer) opens a dialog that the game made without a Train button (only Play Progression and Leave). With `test_room_button=1` (default) the mod adds a **Train** button there, above Leave and drawn like it. Clicking it, with the mouse or the VR panel pointer, takes you into the test room. The game's own Training is never run, so neither Training mode nor the cellar is written into the save. Progression's Train still goes to the game's own training. Details: `defaults\train_button.md` in the decompile folder.

With `test_room=1` the game instead goes from the first menu straight into the test room. Either way the room is the game's **cellar** (`/Game/Maps/Arenas/Map_Arena_Cellar`, a Free Mode arena; you start in its middle, facing the long end) set up for testing.

**No staged loading.** With `test_room_blackout=1` (default) the headset shows black from the moment the room is opened. The level loads, the arena clears and spawns its fighters, and then, in one tick, the foes are stood in their places, the tables and books are put up and every weapon is laid on a table. 1.6 s later (the foes are back on their own physics and at rest) the headset fades in over 0.5 s. If the room is still not up after 20 s the headset shows it anyway. Log: `TestRoom: room put up in N ms`, `TestRoom: the room is ready`.

- The tables are the game's table mesh (`SM_Prop_Furniture_Table_011`, the one the game's weapon table uses) as a plain static mesh: nothing can shove them. (The game's weapon table is a physics body; a swing could push it and what lay on it.)
- A weapon is never dropped: it is made just above its spot, the box round its visible parts is measured (not the actor's bounds: a weapon with a scabbard carries an invisible mass part 1e15 cm below it, and its AI search beacon is a big sphere), the table top under it is found with 7 straight-down traces along it, and it is moved so that its lowest point is 0.5 cm above the highest hit, with no speed. Log: `TestRoom: <weapon> laid down: surface z .., moved .. cm`.

**Spawn books** (`test_room_books=1`, default), Blade & Sorcery style: two lecterns (the game's `SM_Prop_Furniture_Lectern_001`, static) 1.5 m to your left and right, a page on each, lit by a small warm light.
- Point at a line with an **empty hand** (nothing held, not grabbing): a yellow dot and a thin beam show where the hand's pointing ray meets the page, and the line turns gold. Pull the **trigger** to pick it (a short buzz).
- **Weapons** (left): `< Tier >` (point at the left or right end to go down or up: Random, Beggar .. Lord), then Sword, Longsword, Mace, Two-hand mace, Shield, Axe / hammer, Two-hand axe, Polearm, Dagger / secondary. The weapon comes from the game's own generator at that tier (the nearest tier that has one) and is laid in the first free spot on the tables; when the tables are full it lies on the floor in front of the book.
- **Willies** (right): `Mode: Passive / Fighting` (click to change), then one line per tier Beggar .. Lord, `Random tier` and `Remove all Willies`. A new Willie stands on a free spot behind where you started (2.2 m, else 3.2 m; at least 1 m from every other Willie, never in a wall), facing you. Passive ones are like the room's own foes; Fighting ones keep their AI and come for you. A book Willie is never replaced when he is out; his body is removed like the others'. `Remove all Willies` removes every Willie but you (not one you are holding or have a weapon stuck in) and the room's own foes too: none of them come back.
- The page is placed on the lectern's slanted top, found with traces (the lectern is turned so its slope faces you). Log: `TestRoom: lectern top at .., tilted N deg`; without a slanted top the page stands above the lectern, leaning back 35 degrees.
- Test command: `hsvr.book_click <book 0 weapons | 1 Willies> <line> [u]` does what pointing at that line (line 0 is the title; u -1 left .. 1 right) and pulling the trigger does.

- `test_room_foes` Willies (default 4, 1-10) whose AI is switched off. They never attack, move, kick or block, but keep the game's own physics: they stand, balance, fall when hit or pushed, get up again, can be grabbed, punched, stabbed and killed. They are stood in an arc 2.2 m behind you, facing you. A foe whose AI comes back on (zombie revive) is switched off again.
- Two static tables 1.1 m in front of you, 35 cm apart, stocked through the game's own loot point (`BP_Loot_Weapon`, Spawn Chance 1) with one of each: 1H sword, 2H sword, 1H mace, 2H mace, shield, 1H hafted, 2H hafted, polearm, secondary (the game's secondary kind can be a bottle). When the game has no weapon of a kind at the chosen tier, the nearest other tier is used (log: `the game has none at tier N`).
- The foes are moved with their physics off and get it back 0.4 s later at rest (a teleport of a simulating Willie throws him about 5 m).
- `hsvr.test_room_report` (live command, HalfSwordVR_commands.txt) logs every table, book, loose weapon and foe (`TestRoom: check:`); `TestRoom: watch` lines follow everything near the tables that moves in the room's first 5 s.
- **Respawn** (`test_room_respawn=1`, default): a foe who is killed (`DED`) or beaten (`Give Up`: he went under 5 health and yielded) is out. His body is removed `test_room_despawn_seconds` (default 7) later with the game's own `Destroy Willie`, and his slot gets a new foe `test_room_respawn_seconds` (default 10) after he was out: made the way the arena's spawner makes a Free Mode foe (`BP_Generator_Characters_Random` "Generate Character" at `test_room_tier`, a Willie_BP with that passport, Team Int 2, sized by his Height; AI_BP takes him by itself), made passive at once, standing in the slot facing you (1 m further back if a body still lies there). A body you are using is never removed: one of your hands grabs him or a piece of him, you hold one of his weapons, or a weapon in your hand is stuck in a body within 150 cm of him; it is removed once you let go. When every foe is out at once, the one out longest comes back immediately. While respawn is on the match is never won: the game mode's Enemy Count is held at 100 or more, and a win the game calls anyway is undone. Log lines `TestRoom: respawn: ...`.
- Test commands (HalfSwordVR_commands.txt): `hsvr.test_room_kill [slot]` (Health 0 and the foe's Death event), `hsvr.test_room_beat [slot]` (Health 0 only: he goes down and gives up).

| Setting | Default | Meaning |
|---|---|---|
| `test_room` | 0 | 1 = boot straight into the test room. Read at launch. |
| `test_room_button` | 1 | 1 = in Free Mode the fencer's dialog has a Train button that opens the test room. |
| `test_room_map` | /Game/Maps/Arenas/Map_Arena_Cellar | The level (any Free Mode arena; the Yard was used before). |
| `test_room_blackout` | 1 | 1 = black while the room is put up, then a fade in. |
| `test_room_books` | 1 | 1 = the weapon and Willie spawn books. |
| `test_room_book_light` | 40 | The small light over each page (0 = none). |
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
