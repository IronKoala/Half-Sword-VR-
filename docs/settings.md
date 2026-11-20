# HalfSwordVR.ini layout

`Mods\HalfSwordVR\HalfSwordVR.ini` is plain `key=value` lines. The mod skips blank lines and lines
starting with `;`, `#` or `[`, so `[Section]` headings and `;` notes are only for reading. No notes at
the end of a value line (a text value would take them in). A key written twice: the last one counts,
so every key appears once.

Sections, top to bottom: **Quick settings** (the ones most often changed by hand: test room,
picture, turning, crouch/kick, hold weight/lag, wrist/shoulder strength, stamina, throwing, grab
reach, hit buzz, lean, hand placement), then Startup, Test room, Picture and view, Menus, Moving,
Body posing, Arms and hands, Holding, Grabbing, Drawing/sheathing, Swinging/throwing, Stabbing,
Fists/elbows/body grabs, Eating, Hit vibration, Old grip spring, Ghost hand, Logging, Developer test
switches, and last "No longer read by the mod".

`mod/HalfSwordVR/HalfSwordVR.default.ini` is the same layout with the mod's own defaults.

## Adding a setting

1. In `config.cpp`, read it (`num`/`flag`/`integer`/`text`) right after the setting it belongs with.
   A player's existing ini gets it inserted on the line under that setting, so it lands in the
   right section (only when nothing before it is in the file does it go to the end).
2. Add it to `LAYOUT` in `tools/ini/organize_ini.py` in its section (the Quick section if players
   will want to tune it by hand), then regenerate the default:
   `python3 tools/ini/organize_ini.py --defaults mod/HalfSwordVR/HalfSwordVR.default.ini`.
   The script stops and names any setting the mod reads that has no place in `LAYOUT`.

## Reorganizing an installed ini

`python3 tools/ini/organize_ini.py <installed.ini> <out.ini>` keeps every key and value (it
compares the key=value sets before and after and stops on any difference). Back up the installed
file first, and only swap it while the game is closed.
