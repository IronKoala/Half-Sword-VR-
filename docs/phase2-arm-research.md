# Phase 2: how Half Sword moves the arms (research notes)

Status 2026-11-15. Done without running the game: from the game executable, and from
recordings Matthew made earlier with a read-only observer mod ("Half-Portible", Oct 31 to
Nov 1 2026, same game build, exe SHA-256 `367dfccf…2e03`). Nothing below changes the game.

## Short version

Half Sword drives each arm in two layers:

1. **An invisible "driver" skeleton** (`DriverSkeleton`, a SkeletalMeshComponent on the pawn)
   runs the animation Blueprint `ABP_Willie_C`. Mouse input and combat state end up as
   an arm pose on this skeleton. It is pure animation: no physics.
2. **The visible physical body** (`CharacterMesh0`) runs `ABP_CopyPose1_C`, whose
   `CopyPoseFromMesh` node copies the driver skeleton's pose every frame
   (verified on all 735 frames of a live capture: `SourceMeshComponent` = DriverSkeleton).
   The ragdoll's motors then pull the real, simulated arm bodies toward that copied pose.
   The sword is a physics object held in the hand, so the blade follows the physical hand.

So the "target" the game's muscles chase is the **driver skeleton's arm pose**. The
recordings show driver upper arm, forearm and hand orientations agree with the
physical-animation target getter to within 0.01 to 0.02 degrees while still, and the
physical arm differs from it by about 3 to 5 degrees (that gap is the muscles lagging/sagging).

This is good news for VR: we do not have to reverse-engineer the mouse-to-arm math
(Half-Portible tried and found it nonlinear and pose-dependent, failing its validation
at 28 to 58 degrees error). We can **replace the driver arm pose with one computed
from the controller** and let the game's own muscles, strength, pain, stamina and
injuries do the rest.

## Objects and names (verified live unless marked)

| What | Name |
| --- | --- |
| Pawn | `Willie_BP_C` (`/Game/Character/Blueprints/Willie_BP`) |
| Driver skeleton | component `DriverSkeleton`, anim `ABP_Willie_C` (`/Game/Character/Skeleton/Animation/ABP_Willie`) |
| Physical body | `CharacterMesh0`, anim `ABP_CopyPose1_C`; ~28 more meshes (armour/clothes) run `ABP_CopyPose_C` |
| Head | `CharacterMesh_Head` (runs `ABP_CopyPose_C`) |
| Physical animation | one `PhysicalAnimationComponent` bound to `CharacterMesh0`, strength 2, `IsActive=false` (so it may not be the thing doing the pulling; joint motors are the other candidate) |
| Extra constraints | `PhysicsConstraint Main`, `PhysicsConstraint_Z` (pelvis), one on `spine_03`, one on `hand_r` (`NODE_AddPhysicsConstraintComponent-77`, probably the weapon grip) |
| Other | `PhysicsHandleComponent` (likely grabbing), `CollisionDisabler` plugin (`ModifyCollision`) |
| Right arm chain | `upperarm_r → clavicle_r → spine_05 → spine_04 → spine_03`, plus `lowerarm_r`, `hand_r` |
| Weapon | pawn property `Weapon R`; actor root can be `SM_Grip_Dummy`; blade is a child component `Head` (1H sword blade 0.628 m) |

### Arm-related pawn variables (Willie_BP_C)

Input and aim: `Axis Value X`, `Axis Value Y` (mouse motion; right = +X, up/away = −Y; zero when the
mouse is still), `Mouse Sensitivity Rate`, `Aim Vector`, `Aim Vector Delayed`, `Aim Velocity`,
`Aim Swing Speed`, `Swing Velocity proxy`, `R Swing Direction`, `L Swing Direction`,
`Current Control Rotation`, `Control Rotation Y`, `Body Rotation`, `Shoulders Swing`.

Combat state: `R_Guarding`, `L_Guarding`, `Any_Guarding` (plus `R_Thrusting`, `R Two Handed Grip`
from earlier notes).

Per-joint arm targets (the most promising write points if we go the Blueprint-variable route):
`R Aim Rotation`, `R Shoulder Rotation`, `R Elbow Rotation`, `R Hand Rotation`,
`R Wrist Direct Rotation`, `R Shoulder Additive Rotation`, `R Elbow Additive Rotation`,
`R_Arm_Control_IK_Rate`, `Default Arm Position R`, `Current Default R Arm Position`
(and the same with L).

Muscle state: `Muscle Power`, `All Body Tonus`, `Arm R Tonus`, `R Arm Extra Tonus`,
`Pain R Arm Alpha`, `Arm_R Health`, `R Arm Being Grabbed`, `Grab R Target Found`.

### ABP_Willie_C (driver anim) variables

`Aim Vector`, `Total Y Aim`, `Aim Swing Speed`, `Swing Direction`, `Control Rotation`,
`Look Rotation`, `Upper Body Aim Rotation Rate`, `R Guarding`, `L Guarding`, `Toggle Aim`,
`Aim Prepare`, `Left Arm Dominates`, `Block Arm Muscles`, `Block Shoulder Muscles`,
`R Hand Stuck Shoulder`, `R Hand Stuck Under L Armpit`, and **eight
`RotationOffsetBlendSpace` nodes** (aim offsets). That suggests the arm pose is at least partly
a blend of authored poses indexed by aim values, with IK and per-bone rotations layered on top.
The exact graph is the main unknown (see below).

## What is still unknown (needs the game running)

1. The AnimGraph of `ABP_Willie`: which node types exist (two-bone IK? modify-bone? FABRIK?)
   and what they read. This decides where to inject the controller pose.
2. Which Blueprint functions run each frame to turn `Axis Value X/Y` into the per-joint
   rotations.
3. What pulls the physical arm: joint motors driven from the copied animation, or the
   `PhysicalAnimationComponent` (reported inactive), and how strong they are.
4. How the sword is attached to the hand (the `hand_r` constraint) and its drive settings.

The game's asset archive is encrypted, so we cannot read the Blueprints from disk.
A simple scan of the exe did not find the key, and it is not worth more time: the live
game exposes all of the above through reflection.

## The probe (tools/armprobe)

A read-only UE4SS Lua mod that answers questions 1 to 4 in one short session:

- **F7** writes a full dump: every pawn / ABP_Willie / ABP_CopyPose1 / mesh / physical-animation
  property with its type and value, every Blueprint function name, every component with its
  attach parent, every AnimGraph node **with its node type and settings**, and every physics
  constraint's drive settings.
- **F8** records 8 seconds (nothing runs per frame except during a recording): every number/bool/vector/rotator on the pawn and ABP_Willie, only
  when it changes, plus driver vs physical arm bone poses each frame, plus a count of which
  Blueprint functions ran.

Install: copy `tools/armprobe` to `ue4ss\Mods\HalfSwordVR_ArmProbe` and add an empty
`enabled.txt`. Output goes to `ue4ss\Mods\HalfSwordVR_ArmProbe\output`. Remove the folder
to uninstall. It does not touch HalfPortibleObserver or the HalfSwordVR mod.

Suggested session (about 5 minutes, no headset): load an arena, equip a one-handed sword,
press F7 once; then F8 and for 8 seconds hold right mouse (guard/aim), sweep left-right,
then up-down; then F8 again doing a few swings without guard.

## Candidate ways to drive the hands (to decide after the probe)

| Option | How | Pros | Cons |
| --- | --- | --- | --- |
| A. Override the driver pose (preferred) | In our C++ DLL, after `DriverSkeleton` finishes its animation and before `CharacterMesh0` copies it, overwrite the arm bones (clavicle, upper arm, forearm, hand) with a pose solved from the controller (two-bone IK + wrist = controller rotation). | Game muscles, strength, injuries, stamina and weapon weight all keep working; sword lags and collides realistically. Independent of the mouse math. | Needs a native hook point in the anim update; physical hand will trail fast swings (as in the real game). |
| B. Write the Blueprint arm variables | Each frame set `R Shoulder/Elbow/Hand Rotation` (or `R Aim Rotation`) from the controller and zero `Axis Value X/Y`. | Lua-friendly, quick to prototype. | Depends on Blueprint semantics we do not know yet; may be fought by the Blueprint each frame. |
| C. Pull the hand body directly | Add our own physics drive (PhysicsControl plugin is shipped, or a PhysicsHandle) from the controller pose to the `hand_r` body. | Most direct tip accuracy. | Bypasses game balance; risk of jitter and super-strength. Best kept as an assist on top of A. |

Recommendation: probe first, then prototype B in Lua (fast to try) while building A in the
C++ mod as the real implementation, with C as an optional "assist" slider for accuracy.

## Live probe results (2026-11-15, Map_Arena_Cellar, one-handed sword)

Probe v3 wrote a full dump and about 20 s of recording before the game crashed. The crash came about 20 s
after the probe hooked 433 Blueprint functions with Lua counters, so the hooks are the main suspect.
**Never hook Blueprint functions in bulk from Lua.** Earlier versions (1 and 2) broke menu clicking.
The suspects there are UE4SS `RegisterKeyBind` and/or one `os.execute` mkdir at load time. Avoid both.
Bone poses were not captured: `GetSocketTransform(FName, RTS_Component)` returned nothing through the Lua bridge.

### How mouse input becomes an arm pose (now verified)

1. `Axis Value X` / `Axis Value Y` on the pawn = mouse motion per frame, clamped to ±10
   (with smoothed `... Interp` copies).
2. These are integrated into **`Aim Vector`**, a 2-D point clamped to **±90 on X and Y**
   (`Aim Vector_0` and the X/Y temporaries are the same). The pawn also derives `Aim Vector Delayed`,
   `Aim Velocity`, `Aim Swing Speed`, `Swing Direction`, `Swing Direction SlowDown (Inertia)`
   and `Swing Velocity proxy` for stamina and swing physics.
3. ABP_Willie receives `Aim Vector` plus `Aim Inertia` (±16, so the effective range is about ±104),
   `Total Y Aim`, `Swing Direction`, `Control Rotation`, `R Guarding`, `R Hand Toggle`, and so on.
4. The ABP_Willie AnimGraph turns that into the driver skeleton pose. It is almost all authored-pose blending:
   270 BlendSpacePlayer, 238 BlendListByBool, 203 SequencePlayer, 198 TwoWayBlend, 11 RotationOffsetBlendSpace,
   8 BlendSpaceEvaluator, 49 ApplyAdditive, 40 Mirror and 77 ModifyBone nodes. There are only 2 TwoBoneIK
   nodes (effectors around 70 to 78 cm below the hip in bone space, very likely the legs; ABP also has
   `R/L Knee IK Location` and `Right/Left Foot Effector Location`) and 1 CCDIK.
5. Pawn per-joint variables (`R Shoulder Rotation`, `R Elbow Rotation`, `R Hand Rotation`,
   `R Wrist Direct Rotation`, `R Aim Rotation`) **did not change at all** during sword movement. They are not
   the control path, at least in this mode.
6. Left click toggles `R Hand Toggle` (pawn and ABP). `R Guarding` stayed true. There are input events for
   `Thrust`, `Grab Left/Right`, `Swap Hands`, `RightMouseButton`, `Chain Attack`, and others.

### What this means for VR

The game's own arm control is a **2-D aim space** (where the sword points on a ±90 grid) mapped to animator-made
poses. It cannot represent an arbitrary hand position and rotation. Feeding the controller into `Aim Vector`
(option B) would therefore only give a rough "point the sword" control, with no real reach, wrist roll or blade
angle. For accurate sword control, **option A is required**: replace the driver skeleton's arm bones
(clavicle, upper arm, forearm, hand) after ABP_Willie has evaluated, using a pose solved from the controller.
The physical arm then chases it through the game's own muscles. Next steps:

1. In the C++ mod, find a hook after the DriverSkeleton anim evaluation and before CharacterMesh0's
   CopyPoseFromMesh reads it. Candidates: `USkeletalMeshComponent::FinalizeBoneTransform` /
   `PostAnimEvaluation` on the DriverSkeleton instance, or writing its component-space transforms on the game
   thread in a tick that runs before the physical mesh's anim update.
2. Solve the arm with a two-bone IK from the shoulder to the controller-held hand, set the hand rotation from
   the controller, and scale for the player's arm length versus the character's.
3. Keep `Aim Vector` roughly in sync (from the controller direction), so swing speed, stamina burn and swing
   sounds still behave.
4. Measure how far the physical hand trails the target, and add an optional physics assist only if needed.
