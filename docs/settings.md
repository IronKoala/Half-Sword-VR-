# HalfSwordVR.ini layout

`Mods\HalfSwordVR\HalfSwordVR.ini` is plain `key=value` lines. The mod skips blank lines and lines
starting with `;`, `#` or `[`, so `[Section]` headings and `;` notes are only for reading. No notes at
the end of a value line (a text value would take them in). A key written twice: the last one counts,
so every key appears once.

The file opens with a numbered list of sections. Each section has a banner heading and a
`[N. Title]` line (Ctrl+F `[16.` jumps there). Sections are grouped by feature: Startup, Test room,
Picture/performance, Eyes, Helmet and own head, Menus, Sticks/turning, Crouch/kick/jump, Lean,
Body posing, Your damage/injuries, Hands and arms, Fingers/grip wrap, Grabbing, Sliding,
Two hands, Holding, Stamina/strength, Shields/pavise, Drawing/sheathing, Swinging/throwing/whoosh,
Stabbing/cutting, Punches/elbows/body grabs, Hit vibration, Eating, Old grip spring, Ghost hand,
Logging, Developer test switches, and last "No longer read by the mod". Every setting has a
one-line `;` note above it and a blank line after it; long notes wrap at 96 characters.

`mod/HalfSwordVR/HalfSwordVR.default.ini` is the same layout with the mod's own defaults.

## Adding a setting

1. In `config.cpp`, read it (`num`/`flag`/`integer`/`text`) right after the setting it belongs with.
   A player's existing ini gets it inserted on the line under that setting, so it lands in the
   right section (only when nothing before it is in the file does it go to the end).
2. Add it to `LAYOUT` in `tools/ini/organize_ini.py` in its feature section, with a short plain
   comment (the script stops on a setting without one), then regenerate the default:
   `python3 tools/ini/organize_ini.py --defaults mod/HalfSwordVR/HalfSwordVR.default.ini`.
   The script stops and names any setting the mod reads that has no place in `LAYOUT`.

## Reorganizing an installed ini

`python3 tools/ini/organize_ini.py <installed.ini> <out.ini>` keeps every key and value (it
compares the key=value sets before and after and stops on any difference). Back up the installed
file first. If the game is running, write the new file next to it and swap it in with one rename
(the mod re-reads the ini every second and would add a half-written file's missing keys back at
defaults). Never run `--defaults` on a player's ini: it writes the code defaults, not their values.
