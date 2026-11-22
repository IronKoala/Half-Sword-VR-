# Crossbow (VR)

The game has one crossbow, `BP_Weapon_Ranged_Weapon_Crossbow_Light`, and one bolt, `BP_Weapon_Ranged_Projectle_Bolt_1`. The mod drives the crossbow's own physics and events, so the string, the bolt and the shot are the game's own. The game's side is described in `add_new_weapons.md` Part 3 §2 in the decompile folder.

## In VR

1. **Two grips.** The rear grip is the game's right-hand point (`Grip R Hand Default Pos`, by the trigger). The front grip is the game's off-hand point (`Grip L Hand Off Hand`). Both are fixed, so the hand does not slide on them. Either hand can take either grip, and the second hand makes it a two-handed hold.
2. **Cocking.** Hold the crossbow in one hand, then close the other hand within `crossbow_string_reach_cm` of the string's nut (`CM_Test_Peg`) to take the string. Pull it back along the stock. The nut follows your hand through the string joint's own reference (`PhysicsConstraint_Peg_Stock`, Z limit shut), with no extra joint to your hand. Within 5 cm of the loaded spot the game's tick cocks it (`Event Load String`: click, `String Loaded`). If it hasn't after 0.6 s at full pull, the mod calls that event itself. Let go before then and the string goes back.
3. **A bolt.** Close a hand on the quiver on your hip (`Attached Bolt Quiver`, within `crossbow_quiver_reach_cm`). The left hand uses the game's `Grab Bolt From Quiver`. The right hand goes through the same steps itself, because the game's function only fills the left hand.
4. **Loading.** Bring the bolt within `crossbow_bolt_seat_cm` of the cradle (`Scene_Location_Peg_Loaded`) while the string is cocked. It leaves your hand and goes in through `Event Load Bolt`. The mod clears the bolt's `Last Parent` first, because otherwise that event empties your left hand.
5. **Firing.** Pull the trigger on the hand that is on the rear grip, past `crossbow_trigger_fire`. That calls `Event Shoot`: the bolt is pushed along the stock and the string uncocks. The trigger has to come back under `crossbow_trigger_release` before the next shot. With an uncocked string, the trigger only gives a small buzz.

Not done yet (later work): weight and feel, how hard the string is to pull, and picking bolts up off the ground or the dead.

## Settings ([Crossbow])

| Setting | Default | Meaning |
|---|---|---|
| `crossbow` | 1 | Crossbow handling on. |
| `crossbow_string_reach_cm` | 8 | How near the nut a hand must close to take the string. |
| `crossbow_string_margin_cm` | 2 | ...and at least this much nearer the nut than a grip (the front grip is 6 cm from the resting nut). |
| `crossbow_quiver_reach_cm` | 12 | How near the quiver a hand must close to take a bolt. |
| `crossbow_bolt_seat_cm` | 10 | A bolt this near the cradle loads (the game's own rule is 10). |
| `crossbow_trigger_fire` | 0.75 | Trigger pull that fires. |
| `crossbow_trigger_release` | 0.3 | The trigger must come back under this before it fires again. |
| `crossbow_trigger_grip_cm` | 8 | Only the hand within this of the rear grip fires. |

## Test commands (HalfSwordVR_commands.txt)

- `hsvr.xbow_hand <0|1> <off|rear|front|string|pull <cm>|quiver|cradle|keep>`: moves that fake hand's grab point to that spot. `pull` draws the hand back from the nut along the stock at 30 cm/s. Use it with `hsvr.arm_move 1`. `keep` holds that controller where it is against the other controller (any weapon), so two hands on one weapon move it rigidly with the other hand's arm moves.
- `hsvr.xbow_report`: logs the quiver, both hands, the crossbow's parts in its BaseMesh frame and its flags.
- `hsvr.xbow_quiver <n>`: calls your character's own `Add Bolt to Quiver` n times.
- `hsvr.expect` values: `xbow_string`, `xbow_bolt` (the crossbow in either hand), `xbow_quiver` (bolts left), `bolt_l`, `bolt_r`, `xstring` (0 no hand on the string, 1 left, 2 right).

Log lines start with `Crossbow:`.
