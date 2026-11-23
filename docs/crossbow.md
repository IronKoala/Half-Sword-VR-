# Crossbow (VR)

The game has one crossbow, `BP_Weapon_Ranged_Weapon_Crossbow_Light`, and one bolt, `BP_Weapon_Ranged_Projectle_Bolt_1`. The mod drives the crossbow's own physics and events, so the string, the bolt and the shot are the game's own. The game's side is described in `add_new_weapons.md` Part 3 §2 in the decompile folder.

## In VR

1. **Two grips.** The rear grip is the game's right-hand point (`Grip R Hand Default Pos`, by the trigger). The front grip is the game's off-hand point (`Grip L Hand Off Hand`). Both are fixed, so the hand does not slide on them, and both have a fixed hand pose (Matthew 2026-11-23): however the hand comes to the grip, it sits in the one seat whose forearm hangs under the stock and back toward you, never with the wrist up over the crossbow in your line of sight. "Under" is the side away from the string's nut. The log line `Grab: ... takes its fixed pose` shows the seat and how well each of the four seats fits. Either hand can take either grip, and the second hand makes it a two-handed hold.
2. **Cocking.** Hold the crossbow in one hand. With the other, empty hand, pull its **trigger** within `crossbow_string_reach_cm` of the string's nut (`CM_Test_Peg`) to take the string, and keep the trigger held while you pull it back along the stock. (Matthew 2026-11-22: the grip button takes the fore end, the trigger takes the string, so neither is grabbed by mistake; the grip button never takes the string.) The nut follows your hand through the string joint's own reference (`PhysicsConstraint_Peg_Stock`, Z limit shut), with no extra joint to your hand. Within 5 cm of the loaded spot the game's tick cocks it (`Event Load String`: click, `String Loaded`); if it hasn't after 0.6 s at full pull, the mod calls that event itself. Let go of the trigger before then and the string goes back.
3. **A bolt.** Close a hand on the quiver on your hip (`Attached Bolt Quiver`, within `crossbow_quiver_reach_cm`). The left hand uses the game's `Grab Bolt From Quiver`. The right hand goes through the same steps itself, because the game's function only fills the left hand.
4. **Loading.** Bring the bolt within `crossbow_bolt_seat_cm` of the cradle (`Scene_Location_Peg_Loaded`) while the string is cocked. It leaves your hand and goes in through `Event Load Bolt`. The mod clears the bolt's `Last Parent` first, because otherwise that event empties your left hand.
5. **Firing.** Pull the trigger on the hand that is on the rear grip, past `crossbow_trigger_fire`. That calls `Event Shoot`: the bolt is pushed along the stock and the string uncocks. The trigger has to come back under `crossbow_trigger_release` before the next shot. With an uncocked string, the trigger only gives a small buzz.

Not done yet (later work): weight and feel, how hard the string is to pull, and picking bolts up off the ground or the dead.

## Settings ([Crossbow])

| Setting | Default | Meaning |
|---|---|---|
| `crossbow` | 1 | Crossbow handling on. |
| `crossbow_string_reach_cm` | 10 | How near the nut the free hand's trigger must be pulled to take the string. |
| `crossbow_quiver_reach_cm` | 12 | How near the quiver a hand must close to take a bolt. |
| `crossbow_bolt_seat_cm` | 10 | A bolt this near the cradle loads (the game's own rule is 10). |
| `crossbow_trigger_fire` | 0.75 | Trigger pull that fires. |
| `crossbow_trigger_release` | 0.3 | The trigger must come back under this before it fires again. |
| `crossbow_trigger_grip_cm` | 8 | Only the hand within this of the rear grip fires. |

## Test commands (HalfSwordVR_commands.txt)

- `hsvr.xbow_hand <0|1> <off|rear|front|string|pull <cm>|quiver|cradle|keep>`: moves that fake hand's grab point to that spot. `pull` draws the hand back from the nut along the stock at 30 cm/s. Use it with `hsvr.arm_move 1`. `keep` holds that controller where it is against the other controller (any weapon), so two hands on one weapon move it rigidly with the other hand's arm moves.
- `hsvr.xbow_wrist <0|1> <pitch|roll> <deg> <seconds>`: turns that fake controller about its own right (pitch) or forward (roll) axis to <deg>, evenly over <seconds> (from where it is on that axis).
- `hsvr.fake_look <0|1>`: 0 stops the made-up head looking left and right.
- Every `hsvr.arm_move` switch eases the fake controllers over 0.3 s. `keep` holds a controller rigid to the other one in that controller's own frame (position and turn).
- `hsvr.xbow_report`: logs the quiver, both hands, the crossbow's parts in its BaseMesh frame and its flags.
- `hsvr.xbow_quiver <n>`: calls your character's own `Add Bolt to Quiver` n times.
- `hsvr.expect` values: `xbow_string`, `xbow_bolt` (the crossbow in either hand), `xbow_quiver` (bolts left), `bolt_l`, `bolt_r`, `xstring` (0 no hand on the string, 1 left, 2 right).

Log lines start with `Crossbow:`.
