# Phase 4: the character moves with the player ("surgery")

Status 2026-11-16: written and compiled, **not yet run in the game**. Branch `claude/body-follow`, on top of
`claude/controller-input`. Background: `docs/phase3-motion-controls.md`, `docs/phase2-arm-research.md`.

## Second cut (2026-11-16, after the first headset test): the player first

Matthew after the first cut: "its better but doesn't seem like the solution. im still just a puppeteer
instead of actually being the player character ... im controling a thing that controls willie, this
isnt how VR games work". The log of that test agreed: everything worked as built (spring anchored 0.0 cm
from the controller, no faults), yet the hand was 3 to 5 cm off at rest and 10 to 20 cm in swings, and
the real head 10 to 25 cm from the view. Facts learned from the "Pull:" lines: the game drives 22 bodies,
all in the world (not relative to the parent), turning strength 894 to 1000 (legs about 1900), damping a
tenth of that, torque limit five times the strength (MaxAngularForce 4470 to 5000), no positional
strength at all, and it writes these settings again every frame (so the mod writes after it every frame).

The second cut turns the chain around, as VR physics games do. The sections below describe the first
cut; these points replace what they say where they differ:

- **Hands**: `hand_pull_strength` 6000, damping 110, `hand_pull_max_force` 12000 (the weight knob: a
  one-hander keeps up, heavy weapons drag), `hand_turn_scale` 3 now also scales the wrist's torque
  limit. New `arm_soft_scale` 0.3: while a hand is pulled, that arm's clavicle, upper arm and forearm
  turn with 0.3 of the game's strength, so the arm follows the hand instead of carrying it.
- **Head**: the head body is held at the headset's place (`head_pull_strength` 3000, damping 80,
  `head_pull_max_force` 20000, which is well under the body's weight so it cannot lift the body).
- **View**: `view_follow_seconds` 0. The view is placed on the character's real eyes when recentering
  (and at spawn) and from then on is the headset one to one on the character's position; it only gives
  way beyond `view_leash_cm` 40. It must not drift towards the real head while the head is pulled to
  the view: a steady sag would make both sink for ever.
- **Upper body** (`own_upper_body` 1): the game's upper-body animation is not used. The shape of the
  bones from `spine_01` up is taken once from the standing pose after spawn ("Body: upper body shape
  taken ..."), kept upright in the world on the game's hips, turned to a chest heading that follows the
  headset loosely (free within 25 degrees, at most `chest_turn_max_deg` 70 from the legs), and leaned
  so the head is at the headset. Fingers and the weapon bone keep the game's animation.
- **Aim** (`aim_sync` now 0): the game's aim is no longer fed from the blade, so the game does not turn
  or pose the body by itself. To check in play: whether damage, stamina or swing sounds depended on it.
- The hand error in the "Arms:" line is now measured against the previous frame's target (what the
  physics has had time to reach); before it included one frame of hand travel.
- New "Body:" status line: how far the real head is from the headset's place, whether it is held, the
  leash, the chest turn, and how far the body mesh's own frame is tilted (to learn whether that frame
  follows the simulated hips).

Head hiding (`hide_head`, on): Matthew explained why he had the Phase 1 version removed: hiding the
head BONE made the game treat his character as beheaded (the game uses that switch for a severed
head). The first cut of this branch did the same and was never switched on. It now only sets the
"not drawn for its owner" flag on head pieces (`CharacterMesh_Head`, hair, anything named or attached
like headgear) and never touches a bone. **Never use HideBoneByName on this game's characters.**
Where the sections below mention `HideBoneByName` or `hide_head_bone`, that is the removed way.

## Twelfth cut (2026-11-16): what the game's script does to a placed body

Matthew on the eleventh cut: "kicking whilst crouching damages me. i crouch walked into the hanging
kettle and it messed with the physics. also i held my hand straight but my in game wrist was bent.
my guess is open XR is mis-angle my controllers 90degress upwards like usual".

Every cause below was read out of the decompiled script (`docs/game-internals.md`, section 12) and
confirmed by a self-test run; the build passed the self-test twice (39 checks, `selftest-pass-12th.log`).

- **The game drags the character.** Its script sets the capsule's place outright every frame: to the
  hips and chest, plus the shoulders' lean, a 50 cm "stumble" after each kick, pain stumbles and
  dodges, once that is more than a reach away which it works out anew every frame (10 cm standing
  still; the mod's write to `Capsule Max Dislocation` only ever counted while fallen). A kick, a duck
  or a lean dragged the player 20 to 35 cm a frame. The size-based `hold_ground` caught only the
  large jumps, and took them back as a teleport, which carried the physical hands along and threw
  them. Now the character's own tick is wrapped (`PrimaryActorTick`, same vtable swap as the mesh
  ticks): whatever moved the capsule sideways inside that tick is put back the moment the tick
  returns, exactly, without teleport. Walking is the movement component's tick and is untouched.
  The size-based take-back stays as a fallback. Lost: the game's own dodge and stagger steps.
- **The crouch dislocated the knee.** `Dislocate Bone` judges a joint by how far the bone is bent
  against its parent and wears the limb down until `Leg R Dislocated` (health 94 to 0 in six frames,
  nobody near). The mod's crouch bends the knee further than the ragdoll's joint limits ever allow.
  While the body is placed, `Dislocation Blocked` (the game's own switch) is held on.
- **Falling.** `Free Fall Rate` rises while the game's foot checks fail and turns any leg contact
  into landing damage; held at 0 while placed. Landing damage after a real fall is lost.
- **Bumps.** The body mesh's hit event turns contact force into damage. A placed limb cannot give
  way, so resting against a loose prop is hundreds of full-force contacts a second. A ProcessEvent
  pre-callback hands the script a force of zero for contacts with anything that is neither a
  fighter nor a weapon (`bumps_harmless`).
- **The collision switches never held.** `CollisionDisablerFunctionLibrary`'s duration is a float;
  the mod passed the whole number -1, which the game read as "not a number". The held weapon has
  been colliding with the player's own body in every cut so far. Fixed; also every part in a
  weapon's `Collision Components Array` is switched, not only `BaseMesh` (the kettle's pot).
- **Stuck weapons.** The game sticks points and edges into wood, floors and bodies
  (`Constraint_Weapon_Stuck_BP`). The hand's pull never reaches the force that frees them, so the
  hand stayed behind. A stuck weapon now comes free (`Release All Jammed Bones`) once the hand has
  been pulled `stuck_pull_free_cm` (25) away.
- **Wrist.** The controller's handle and wrist-to-fist axes were assumed. The calibration pose now
  measures them (arms straight out in front, thumbs up, wrists straight, both grips 2 s) and stores
  `ctrl_axes_l` / `ctrl_axes_r`. Only Matthew's own pose can confirm this one.
- `carry_hands`: changes of the character's walking speed are given to the physical hands and what
  they hold directly. `body_ignores_level` exists but is off (it only let the hands through walls).

Self-test: parts 10 and 11 now kick twice while crouched and crouch-walk into the hanging kettle.
The walking part goes backwards first: the cellar spawn is at arm's length from a training dummy
(`BP_Prop_Training_Dummy_001_C`), and walking forward hooked Matthew's sickle on its shield, which
is the physical hand doing its job. A blow from the kettle itself is reported as a NOTE.

Not covered by any test: enemies hitting the placed body, thrust, jump, two-handed grips.

## Eleventh cut (2026-11-16): crouch is the player's own

Matthew on the tenth cut: "crouching breaks in more was then i can type." (No word against the wrists
or the walking.) Crouch had not been in the self-test. His log during crouches: the game's
`Is Crouched??` on for long stretches, the view dropping and returning at odd moments, the hips
pushed up 35 to 48 cm against the game's own crouch animation, consciousness falling from 100 to 50
within three seconds, the character moved 17 to 37 cm at a time "by something else" a dozen times, and
hands lost 1 to 5 m away and brought back. From the decompile: the game's crouch is a toggle on the
button, holding it starts a `Dolphin Dive`, and crouch feeds the hip height and several balance terms.

- `own_crouch` (1): while the body is placed the crouch button (LS on the virtual gamepad) is not passed
  to the game at all; it toggles `shared().own_crouch`, which lowers the view by `crouch_view_drop_cm`
  (40) over a quarter of a second. Hip height follows the head as always, so the body crouches with the
  view. The game's crouch, its dive and its hip animation never start. Real ducking is unchanged.
- `no_self_hits` (1): what a hand holds no longer collides with the player's own body, through the
  game's own `CollisionDisablerFunctionLibrary:DisableCollision_SkeletalVsSingleBody(Mesh, Rigid Bones,
  weapon BaseMesh, -1)` (the game only does this for the hand and forearm; with 1:1 arms and a squat
  the sword met the knees).
- Self-test part 10, "crouch button": crouched 9 s with low swings across the knees and a short walk,
  then up. Measured: view down 40 cm, hips down 40 cm, head 0.0 cm from the headset's place, hands 1.9 /
  0.1 cm from the controllers, nothing moved the character, weapon held, the game's crouch never on, up
  again at the end. The consciousness check now looks for a sudden drop (a blow), because the number
  drifts by itself (100 to 92 over a run, even standing still; cause not known, see below).
- Passed 34 of 34 (`C:\dev\hsvr-test\surgery\selftest-pass-3.log`).
- Open: the slow drift of `Consciousness` while the body is placed (about a point every 2 s while the
  arms work, recovering while walking). It may be the game's stamina model; if Matthew reports fading
  or passing out for no reason, look at what feeds `Consciousness` in `Willie_BP_C.kismet.txt`.

## Tenth cut (2026-11-16): wrist turning and a walking rhythm

Matthew on the ninth cut (the first build he got after a passed self-test): "i twisted my wrists but
willies didnt stay in line with mine. walking is unreliable and sporadic, willie some times takes a lot
of steps really quickly." His log: hands 0.0 to 1 cm from the controllers most of the time, but the
right hand (holding a weapon) 23 to 67 degrees behind in rotation at moments; up to 10 to 11 steps a
second.

- **Wrists.** The engine drive's torque limit (the game's 5000 times our 5, times the multiplier 2) is
  far too small for a real sword: about 18 rad/s^2 on a 1 kg, 90 cm blade. The self-test had not shown
  it because the arena's sickle is tiny and the pretend swings gentle. Now `hand_turn_servo` (1): right
  before each physics step the hand body's angular velocity is set towards (remaining turn /
  `hand_turn_seconds` 0.03), capped at 45 rad/s, taken a share 1 / (1 + weapon mass / `hand_turn_mass_kg`
  2.0) of the way, with `SetPhysicsAngularVelocityInRadians` on the hand bone (weapon mass from the
  pawn's `R/L Weapon Mass`). The drive stays on underneath. Self-test, new part "twisting the wrists
  fast" (100 degrees, 2.5 times a second, with a weapon): 1 to 2 degrees behind on average, 4 at most.
- **Legs.** The first stepper took a new step every 20 cm whatever the speed. Now a gait: above about
  30 cm/s a rhythm of 1.7 (stroll) to 4.5 (sprint) steps a second, feet alternating, each landing ahead
  of the body by half the distance covered during one stance (at most 45 cm), lift growing with speed;
  below that, a foot steps only when the body is `step_length_cm` (14) past it or turned 35 degrees.
- **Self-test:** nine parts now; draws a weapon from the sheath with the game's `Unsheath R Event` when
  the right hand is empty (after Matthew's own session his character spawned empty-handed, and two
  runs could not check the weapon); reports "PASSED WITHOUT A WEAPON" if there is none. Passed 25 of 25
  (`C:\dev\hsvr-test\surgery\selftest-pass-2.log`).
- Note for later: the arena spawns the player with whatever the save has in hand or sheathed; the
  unattended runs do not seem to change that (five runs kept the sickle; it changed after his session).

## Ninth cut (2026-11-16): built from the decompile, self-tested

Matthew asked for the game's code to be decompiled rather than guessed at (`docs/game-internals.md`,
raw dumps in `C:\dev\hsvr-study\dumps\study`) and agreed to an unattended self-test: the game starts by
itself with a pretend headset, no desktop clicks, only when he is not using the PC, and no build
reaches him before it passes. He refused Claude driving his desktop with mouse and keyboard.

**The self-test** (`dev_selftest=1`, with `dev_fake_headset=1`, `dev_fake_hands=1`, `dev_fake_still=1` and
`dev_script=25:open /Game/Maps/Arenas/Map_Arena_Cellar;112:quit`): eight parts (settle, hands still, slow
movement, fast swings, striking the floor while the pretend head ducks 50 cm, still again, walking by the
virtual gamepad's stick 2 s forward and back twice, turning) and 22 checks, written to the log as "Test:"
lines and to `Mods\HalfSwordVR\selftest_result.txt`. The script that runs one test and puts Matthew's ini
back is `tools/selftest.sh`. It cannot judge feel, an enemy's hits, or anything the pretend hands do
not do.

What the five runs found, each of which would otherwise have been one of Matthew's headset tests:

1. **The hand drive never worked from the mesh's tick.** The game writes its own physical animation
   settings every frame (about 180 writes a second were needed to keep ours) at a point of the frame after
   the body mesh's update, so the muscles' own update always got the game's values (no positional
   strength): the freed hands dropped to the floor, 131 cm from the controllers. Earlier "improvements"
   from hand pull were the pose, not the drive. Fix: the physical animation component's own tick is
   hooked and our settings are written immediately before it. Result: hands 0.1 cm from the controllers
   at rest, 3.0 cm on average and 9.6 at most in fast swings.
2. **Hands must not be physical before their drive exists.** The game's muscle settings are only found
   a few seconds after spawn; until then the hands stay placed rigidly.
3. **`hold_ground` pushed the character through a wall.** A wall stopping the character read as
   "moved by something else" and was "taken back" into the wall, and the right hand was left 15 m
   behind. Now only a single-frame move of more than 15 cm beyond what the movement component's own
   speed explains is taken back, with a sweep. The game's running really is faster than that speed says,
   so smaller differences must be left alone (a summed version made walking stutter).
4. **The capsule leash has to be switched off after the game's script as well as before it.**
   `Capsule Max Dislocation` is now also written in the muscles' tick hook; before that the game nudged
   the character 16 cm at every change of direction.
5. **The hand's roll limits must not apply to a freed or placed hand.** The solver's roll clamp left
   the hands turned 140 degrees from the controllers after a disturbance; the pose's hand bone now takes
   the controller's rotation exactly.

Also new in this cut, from the decompile's facts: the wrists are freed by name (`UserConstraint_12` right,
`UserConstraint_16` left, with the body's joint list as fallback); `rigid_grip` locks the game's grip joint
(`PhysicsConstraint R/L Hand`: soft swing and twist limits, breakable at 300 / 500) hard and unbreakable
while the hands are physical, so hand and weapon are one object (measured: the weapon did not move in the
hand at all); `kick_legs` lets a kicking leg play the game's kick animation; a physical hand more than
1 m from its controller for 20 frames is set at the controller for 0.15 s and let go again (logged).

Passed 22 of 22 on the fifth run (`C:\dev\hsvr-test\surgery\selftest-pass.log`). Not covered by the
test and so still unknown: how it feels, enemy hits on the placed body, the crouch button's view drop,
kick, jump (the game's jump works through the capsule leash, which is off, so it probably does nothing),
real controller noise, two-handed grips.

## Sixth and seventh cuts (2026-11-16): the Blade and Sorcery structure

Matthew rejected the held body outright: "get rid of the puppeteering entirely. it will NEVER work. how
does Blade and sorcery do-it thats how we should do it". **Rule: no springs, pulls, holds or muscle
targets anywhere in the player's body.** He also refused self-testing in the flat window ("no desktop
mode it takes 2-3x times longer then me doing it"): he tests, in the headset.

Sixth cut (2ddc977), tested: `kinematic_body` again, done the standard way: bodies stop simulating
(hands included) **and the mesh is re-attached to the capsule** (`K2_AttachToComponent` snap, then
`K2_SetRelativeLocationAndRotation` to `BaseTranslationOffset` / `BaseRotationOffset`). That fixed the
fourth cut's failure: the log showed "mesh rides on the character", the forearm's physics body 11 to
14 cm from where it is drawn (normal), hands 0.0 to 0.1 cm from the controllers for the whole run. The
missing re-attach was why the kinematic parts had been left behind. Matthew's verdict on what remains:
"i walk: willie glides. i swing and hit the sword Pell the i get thrown across the room. i move my hand
and the weapon bounces around like crazy and doesn't feel like its in my hand but instead just floating
near it and there no physicality to it at all." The log shows him launched 4.4 m and 6.7 m within one
second each, with no stick input and no roomscale movement.

Seventh cut (this one): the two things Blade and Sorcery has that the sixth lacked.

- **Physical hands** (`kinematic_hands` 0, `free_wrists` 1). The wrist joint (constraint named after
  the hand bone) is read and set free in place and turn through the engine's
  `ConstraintInstanceBlueprintLibrary` (`GetConstraintByName` on the mesh, then `Get/SetLinearLimits`,
  `Get/SetAngularLimits`; the original values are kept and put back when the body is given back). The
  hand bodies simulate again and are driven to the controller by the physical animation drive on the
  hand only (`hand_pull_strength` 12000, damping 150, `hand_pull_max_force` 30000, turning strength
  times `hand_turn_scale` 5): strong but stoppable, so a blocked sword stops the hand. The arm (still
  kinematic) is drawn to where the hand body really is; the pose's hand bone carries the controller's
  place as the drive's target. If the wrist cannot be freed (joint not found, functions missing) the
  hands fall back to rigid placement and the log says so.
- **Own legs** (`own_legs` 1, `step_length_cm` 20, `step_height_cm` 9). Feet are planted in the world
  and step (one at a time unless overstretched) when the body has moved past them or turned more than
  35 degrees; the step lands under the body plus 0.12 s of travel. Foot rest place and rotation come from
  the standing pose. Legs are solved with the two-bone IK, knees along the foot's heading.
- A log line when the character moves more than 20 cm in one frame, with the movement component's
  velocity and mode, to find what throws him.

Log: `Hands: right wrist joint as the game made it: place 2 2 2 ...`, `Hands: right hand cut loose
from the forearm`, `Legs: N steps this second | feet the mod's own | hands physical ...`,
`Room: the character moved N cm in one frame ...`.

## Fifth cut (2026-11-16): the held body (replaces the kinematic body)

Fourth test (kinematic body, hands simulated): standing still the hands were about 3 cm off; then the
arms broke. Matthew: "pressing the grip broke the arms", "it locked the arms into place even when i
walked away, when i did walk away it caused the arms to flail about like crazy", and, when told the
grip looked like a coincidence, "when i was standing still and i pressed grip it did break the arms, i
could see it". Take his word over the log: the first grip press (44.86 s) came 0.4 s after the hand
springs switched on (44.45 s; the muscle settings are only found about 3 s after spawn), nothing was
sent to the game on grip (controller thread checked), and from then on hand errors rose to 20 to 135 cm
with the hands turned backwards, growing with the distance walked. The body mesh component was frozen
where it stood when the bodies went kinematic ("body mesh N cm from its place over the feet" grew to
10 m). Reading: the kinematic parts did not follow the character in the physics, so the simulated hands
hung from forearm bodies left behind while their springs pulled them to the controllers. Why the engine
left them behind is not known (its own source says kinematic bodies follow the pose each frame).
**Do not use kinematic bodies on this character again without proving in the log that they move.**

`body_hold` (1, with `kinematic_body` now 0): nothing is switched to kinematic. Every body the game
drives (22) keeps simulating and gets, through the same `ApplyPhysicalAnimationSettings` path that held
the head within millimetres in the second test: world space, position strength `hold_strength` 20000,
damping `hold_damping` 200, `hold_max_force` 0 (none), turning strength times `hold_turn_scale` 5 with
no torque limit. The feet get the turning part only (a stiff spring pressing a foot into the floor
would fight the floor). The pose is built exactly as for the kinematic body (legs from the game's
animation over the capsule, hips lowered with knee IK, own upper body, arms from this frame's shoulder,
hand at the controller). Given back to the game's values when knocked out (consciousness under 15), with
the VR controls off, or without a first-person view.

New log lines: `Grip: right grip pressed | hands off their targets ... | body held | hand pull ...` on
every press and release; `Body: held at its place | right forearm's physics body N cm from where it is
drawn (about 13 is normal)` once a second (centre of mass against the drawn bone: proves the physics is
where the picture is); the controller thread's `Pad: the real gamepad 0 now reads ...`.

## Fourth cut (2026-11-16): the placed body

Third test (roomscale build): the capsule move and the crouch guess worked (`Crouch Key Holding` makes
the game crouch, `Root Height` 85 standing, 27 to 50 crouched; it sometimes stays crouched after
release), but the head was often 10 to 50 cm from the headset and the hands 15 to 30 cm off. Matthew:
"i dont know what to do or say so you can wrap your head around it ... how am i currently controlling
willie?" Told plainly that every part of Willie was a goal a self-balancing muscle ragdoll chases, and
that Blade & Sorcery instead treats the player as the truth and draws the body to fit, he answered
"yes": that is the difference he feels. **The player's body must not be a creature that chases him.**

`kinematic_body` (1): `SetAllBodiesBelowSimulatePhysics(pelvis, false)` on the body mesh, then `true`
again for `hand_r` and `hand_l`. Kinematic bodies follow the pose exactly every frame, so the head is at
the headset and the shoulders are where the pose puts them, with no muscles in between. The hands stay
simulated: the wrist joint holds them at the end of the forearm and the game's wrist muscle (times
`hand_turn_scale`) and the grip carry the weapon, so weight shows in the blade's angle, not in the
hand's place. Every frame the mod checks `IsSimulatingPhysics(spine_03 / thigh_r)` and undoes it if the
game switched simulation back on (counted in the log).

- Legs: the game's own leg animation (driver skeleton), set over the capsule: bones outside the upper
  body are taken as posed at the mesh's nominal place (`BaseTranslationOffset` / `BaseRotationOffset`
  on the actor) and converted to wherever the body mesh really is (logged; it used to ride on the
  simulated hips).
- `leg_ik` (1): the hips are lowered (up to 75 cm) or raised (4 cm) so the fixed-length upper body puts
  the head at the headset's height, and each knee bends (two-bone IK in the animation's own knee
  direction) to keep the foot where the animation had it.
- Given back to the simulation: consciousness below `kinematic_min_consciousness` (15), VR controls off
  (Page Up / X held), no first-person view. The pawn's `Fallen` flag is only logged for now.
- Unknown until tested: whether hits still hurt a body that is not thrown about, whether the game's
  script fights the switch, how the game's leg animation looks without its physics, arms passing
  through walls (kinematic arms do not stop at them).

Page Up / X held now also sets `shared().vr_controls_off`: every VR control from both threads is off and
the game plays with mouse and keyboard as it does flat (Matthew needed this to recover lost weapons).

Log: `Body: placed, not simulated (the game switched simulation back on N times) | hips moved N cm ... |
body mesh N cm from its place over the feet | consciousness N, the game says fallen: ...`.

## Third cut (2026-11-16): roomscale, Blade and Sorcery as the reference

After the second cut Matthew: "you're not getting it". Asked directly, he named Blade & Sorcery as the
game whose body feels right, said roomscale is mandatory for his VR mods ("willies legs should
absolutely do the same" when he steps, turns or crouches), wants an option for whether a shove throws
the view or not, and is not sure yet whether a heavy weapon may pull the hand off his hand. He also
does not want the head removed as the fix ("other VR games have character heads but dont have this
issue"); `hide_head=0` is in his ini. The second cut's log (head within 0.2 to 4 cm of the headset
most of the time) says the hold itself worked; what was missing is that the body stood where he
recentered instead of under him.

`roomscale_tick` (arm_control.cpp), before the world ticks:

- **Steps** (`roomscale` 2): when the head bone's place is more than `roomscale_lean_cm` (8) from over
  the feet, the pawn is moved towards it with `K2_AddActorWorldOffset` (sweep on, at most
  `roomscale_max_speed` 400 cm/s) and the distance it really went is taken out of the recentered place
  (`stereo_consume_room` / `vr_shift_recenter`), so the view does not change and the menu panel comes
  along. `roomscale` 1 uses the game's own walking (`AddMovementInput`) instead and takes off whatever
  the pawn moved towards the head. Unknown until tested: whether the simulated legs step or drag.
- **Turns**: when the head looks more than `roomscale_turn_deg` (25) from the aim heading, the
  controller's `ControlRotation` yaw and the recenter heading turn together (at most
  `roomscale_turn_speed` 360 deg/s). Only with `heading=aim`.
- **Ducking**: below `roomscale_crouch_cm` (25) under the recentered height the pawn's
  `Crouch Key Holding` is set (a guess at the game's crouch; the "Room:" line logs `Is Crouched??`,
  `Root Height` and `Height Rate` to learn what really happens).
- Shove option: `view_leash_cm` (40 = the view is dragged along when the real head is more than 40 cm
  away; a large value = the view always stays with the real head).

Log: `Room: mode 2 | head N cm from over the feet, N cm below standing | this second: feet moved N cm
(blocked N), legs turned N deg | crouch asked ..., game says crouched ..., root height ..., height rate ...`.

## What was wrong, and why

Matthew's three complaints after phase 3: the character moves without him and not with him, the hand
(and so the sword) is not where his hand is, and the camera floats at the character's eyeballs so he
sees the inside of the head. All three come from one cause: the view rode on the simulated body, and
the body only followed poses.

What the arm probe's dump (2026-11-15) shows about the game's "muscles":

- They are the engine's physical animation component (`PhysicalAnimation` on the pawn, strength
  multiplier 2). The pawn keeps one strength per body part (`PA Strength Hand R` = 1000, `UA R` = 926,
  spine about 970, ...), an `Orientation Strength` of 1000 and an `Angular Strength` of 100.
- Those are the *turning* part of the spring between each simulated body and its place in the pose.
  Nothing in the pawn's variables sets the *positional* part. So no body part is ever pulled to a
  place: each bone is turned towards its pose angle and the hand ends up wherever the chain of bones
  leaves it. That is why the hand sat a steady 6 to 13 cm off even at rest, sagged under the sword, and
  trailed 20 to 50 cm on fast swings, however good the pose was.
- The character's arm is about as long as Matthew's (62 to 63 cm against 59 to 60 cm measured), so arm
  length was never the real problem. Scaling reach treated a symptom.
- The view was anchored to the simulated head bone with 0.06 s of smoothing, so every sway of the
  ragdoll moved the world, and leaning moved the view out of a head that stayed put.

## What the rework does

Four parts, each with its own switch in `HalfSwordVR.ini`.

1. **The view is the player's head** (`body_follow`, `stereo_device.cpp`). View = character position +
   a slow offset + the headset's movement since recentering, one to one. The slow offset is where the
   view must be for the eyes to sit in the character's real head; it is followed over
   `view_follow_seconds` (2 s) and the view is never more than `view_leash_cm` (30 cm) from the real
   head, so a knock-down still takes the view along. Ragdoll sway no longer moves the world.
2. **The upper body and head follow the headset** (`body_follow`, `apply_head` in `arm_control.cpp`).
   Before the arms are solved, the driver skeleton's upper body is swung about `spine_01` so that its
   head moves by the headset's displacement from its recentered place (`lean_follow`, at most
   `lean_max_deg`), and the head bone is turned like the headset (`head_turn`, at most
   `head_turn_max_deg` from the game's own pose). The game's muscles move the real body after it, so
   balance and strength stay the game's.
3. **The character's own head can be left undrawn in first person** (`hide_head`, off by default because Matthew had an earlier head-hiding version removed; his to switch on): `HideBoneByName` on every
   skinned mesh of the pawn (`hide_head_bone`, default `head`) and the hair hidden; shown again in the
   third-person view.
4. **The hands are pulled to the controllers** (`hand_pull`, "pulling the hands" in `arm_control.cpp`).
   The two hand bodies get the positional part of the same engine spring, in the world
   (`hand_pull_strength` 1500, `hand_pull_damping` 60, `hand_pull_max_force` 0 = no limit), and the
   hand's own turning strength times `hand_turn_scale` (2). The driver's hand bone is written at the
   controller's true place, with the arm solved from the *physical* shoulder, and the reach scaling of
   phase 3 is skipped. Only the hand is pulled: the sword hangs from the hand by the game's own grip,
   so its mass still slows the hand and its inertia still makes the blade trail. Upper arm and forearm
   keep the game's strengths.

How the pull is done without engine source in the mod: the engine keeps the per-body settings in
`UPhysicalAnimationComponent::DriveData`, which is not reflected. It sits right after the reflected
`SkeletalMeshComponent` pointer; the mod finds it by shape (every entry starts with a bone name, one of
them `hand_r`), reads the struct layout from the reflected `ApplyPhysicalAnimationSettings` parameter,
logs every entry once ("Pull:" lines, the first hard facts on the game's muscle settings), and changes
entries only through that engine function, from inside the mesh tick (after the game's own script, before
the engine uses the settings). Whatever is in an entry that the mod did not write is taken as the game's
own value, kept, and put back when the pull is off (menu, Page Up, controller lost).

## Log lines to read after a test

- `Pull: the game's muscle settings found ...` and one line per body: whether the game drives each
  body in the world or relative to its parent, and its strengths.
- `Body: head bone axes learned ...`, `Body: the character's own head is hidden ...`.
- Once a second: `Body: follow on | headset N cm from its recentered place | view N cm from the
  character's real eyes | hand pull right on left on (spring anchored A / B cm from the controller;
  settings written K times)`. A and B should be near 0: they compare the engine's own spring anchor
  with where the mod meant to put it. K growing every second means the game rewrites its settings
  every frame (still works, noted for later).
- `Arms: ... right hand sent err X cm Y deg`: X is now the true distance between the simulated hand
  and the controller's place. Phase 3 had 6 to 13 cm at rest and 20 to 50 cm in swings.

## Tuning (all re-read while the game runs)

| If | Change |
| --- | --- |
| Hand still trails | `hand_pull_strength` up (3000, 6000), damping about 2 x sqrt(strength / 2) |
| Hand shakes or buzzes | strength down, or damping up |
| Heavy weapons feel weightless | `hand_pull_max_force` above 0 (start near 20000), or strength down |
| Blade angle lags the hand | `hand_turn_scale` up (3 to 5) |
| View drifts when standing off-centre | recenter (Home), or `view_follow_seconds` up / 0 |
| Character stumbles when leaning | `lean_follow` down (0.5), `lean_max_deg` down |
| Something feels wrong | switch parts off one by one: `hand_pull=0`, `body_follow=0`, `hide_head=0` |

## Known limits of this first cut

- Ducking: the upper body leans but the legs do not crouch with the headset; a deep duck is limited
  by the leash and the view then drifts back up. Needs the game's crouch driven from headset height.
- Walking around the room leans the body rather than walking the character; recenter after moving.
- The torso does not turn with the player's shoulders (only the head turns; nothing tracks the chest).
- Two-handed grip: both hands are pulled to their own controllers.
- The drive may be mass-independent or not (engine setting unknown from outside); the strengths above
  are a first guess for either case.
- Hand pull makes the player's arms stronger in position than the enemies'. Matthew asked for the
  hand truly at his hand; `hand_pull_max_force` is the balance knob.
