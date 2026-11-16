# How Half Sword runs the player character (from the game's own code)

Written 2026-11-16 from the game's compiled Blueprint script and live object settings, which were dumped
from the running game (three unattended runs into the Cellar arena). Nothing here is guessed from
behaviour. Offsets like `@641773` are byte offsets in the `ExecuteUbergraph_Willie_BP` function of the
dump, so any claim can be checked.

**Where the raw dumps are:** `C:\dev\hsvr-study\dumps\study\` on Matthew's PC (591 files):
- `Willie_BP_C.kismet.txt` has all 432 script functions of the player Blueprint, disassembled. `Willie_BP_C.vars.txt` lists its variables.
- `ABP_Willie_C.*` is the animation Blueprint. `ABP_CopyPose1_C.*` is the body's pose copier. `ModularWeaponBP_C.*` is the weapon base class.
- The dumps also cover 904 other Blueprints: the AI, the game mode, the level manager and so on.
- `objects_Map_Arena_Cellar_Willie_BP_C.txt` has the live settings of the character, every component, the physics asset (bodies and joints), the anim instances and the held weapon.

**How to read them:**
- `tools/study/kx.py FILE OFFSET N [FUNCTION]` prints N compacted statements starting at an offset.
- `tools/study/kgrep.py FILE REGEX` searches every function.
- To dump again, set `dev_study=1` in the ini. The code is `mod/HalfSwordVR/src/study_dump.cpp` on this branch, and it writes to `Mods\HalfSwordVR\study`.

**Why it's a live dump:** the pak index is AES-encrypted. The key is not stored in the shipping exe as plain bytes, and it isn't loaded by simple immediate moves either: about 200k candidates were tested against the index and none matched. So offline extraction would need the key pulled from process memory. The live dump made that unnecessary.

## 1. The overall design: three skeletons and a capsule

| Piece | What it is |
|---|---|
| `CapsuleComponent` (`CollisionCylinder`) | The ordinary Character capsule, walked by `CharacterMovementComponent`. The tick forces movement mode 1 (walking) every frame (`@660392`). |
| `DriverSkeleton` | A **hidden** (`bVisible=False`) skeletal mesh attached to the body mesh. It runs **`ABP_Willie`**, the real animation graph: about 270 blend spaces, about 200 sequence players, 19 state machines, 2 TwoBoneIK (arms), 1 CCDIK and 77 ModifyBone nodes. This is "where Willie wants to be". |
| `Mesh` (`CharacterMesh0`, `SK_Body_Man`) | The visible body. It runs **`ABP_CopyPose1`**, which is only Root plus `CopyPoseFromMesh`, so it copies DriverSkeleton's pose. Its physics asset is `PhysicsAssetOverride = Willie_PhysicsAsset_Capsules`. All bodies below `pelvis` are switched to simulate at spawn (`@19612`, `@21733`), so the copied pose is only a **target**. |
| `PhysicalAnimation` | A PhysicalAnimationComponent on `Mesh`. It pulls each simulated bone's **rotation** toward the DriverSkeleton pose. |
| `PhysicsHandle LowerBody` / `UpperBody` / `GetUp` | Physics handles that grab `pelvis` (location and rotation) and `spine_05` (location) and drag them toward targets computed from the capsule. **These hold the body up and carry it along.** |

So a frame goes like this:
1. Input and AI set variables on Willie: aim, thrust alphas, kick, crouch, jump.
2. ABP_Willie animates the hidden DriverSkeleton from those variables.
3. The body copies that pose as a target. Physical animation twists the bones toward it.
4. The two handles drag the hips and chest toward the capsule.
5. The capsule is then dragged to follow the hips and feet.

Steps 4 and 5 form a loop between the body and the capsule.

## 2. Per-frame flow (`ReceiveTick` → ubergraph `@660378`)

The tick is gated by `Tick Enabled`, which is set to true once setup finishes (`@13053`). It forces walking mode, then runs about 15 branches. The ones that matter for the body:
- **`@660814` body yaw.** It builds the capsule's yaw from the pelvis yaw and `Current Control Rotation`, interpolated by `Consciousness`, `All Body Tonus` and how far the body faces away from the view. Then it `K2_SetWorldRotation`s the capsule (`@663161`). When `Fallen`, the yaw comes straight from the pelvis and spine.
- **`@663470` / `@665893` swing model.** `Aim Velocity`, `Swing Direction`, `Aim Swing Speed` (0–75), `Swing Velocity proxy`, `Lawnmower Rate` and `Weapon Skill (Temp)` all come from the mouse or stick aim vector (`Aim Vector`, X/Y in degrees, roughly ±90). This is what ABP_Willie turns into arm poses.
- **`@668609` stamina burn**, using `R/L Hand Stamina Burn Rate`.
- **`@668734` pelvis roll delta** and the crossbow reload rate.
- Muscle tone (`@401500`…`@402368`): `Leg L/R Tonus`, `Upper Body Tonus` and `Head Tonus` interpolate toward values set by consciousness, stamina, drunkenness, injuries and zombie state.
- **Muscle refresh:** a random 1–2 s timer (`@402396`) sets `Force Refresh Pa Strength = true` (`@56276`). It then calls `Control Physical Animation Str(PhysicalAnimation)` (`@402665`) and clears the flag after a 0 s retriggerable delay (`@56209`). So **the game re-applies every bone's physical-animation settings about once every 1 to 2 seconds**, not every frame. A mod that changes PA settings is overwritten at that rhythm.
- **Capsule follow (`@635580`…`@643744`)**, described in §3.
- **Handle targets (`@408900`, `@414520`)**, described in §3.

## 3. What drags the capsule, and what drags the body

### 3a. Capsule follows the body (`@636822`–`@641956`)
This runs while not `Fallen` and `Free Fall Rate < 0.5`, with `Condition` true (set at `@58979` after spawn).

**Target X/Y:** a point near the hips, lerped toward the chest. It is the pelvis offset 2.5 cm sideways, lerped toward `spine_05` offset 5 cm sideways by (upper-body front tilt × consciousness and tonus). These are added on top:
- `Dodge Vector × Dodge Rate`
- `Pain Stumble Delayed` (up to 20 cm)
- `Stumble Direction`
- `Shoulders Dislocation`, clamped to 0–25 cm and reduced by consciousness and drunkenness

**Target Z:** the lower of the two foot step-end positions (`L/R Step End Position`) + `CapsuleHalfHeight × scale` + `Step Height 2` (only while climbing) + `Last Jump Power × 50 × scale` while `Jump Pressed`. When fallen with the backside on the ground, Z comes from `Ass Touches Ground Location`.

**The leash:** the capsule only moves when that target is more than **`Capsule Max Dislocation`** away. Then it is put back on the leash, at exactly that distance from the target (`@641076`–`@641773`, `K2_SetWorldLocation`, no sweep). `Capsule Max Dislocation` is recomputed each frame (`@642082`):
- 0.1 × upright-tilt factors × the capsule's last update speed × the ground-unevenness factor
- clamped to **at least 10 cm** and at most 75 cm, or half the speed when fallen
- scaled by `Get Up Rate`

That is the measured "moves him 20 cm at a time" behaviour: a 10 cm+ dead zone that grows with speed.

**When `Condition` is false**, the capsule is snapped straight onto the pelvis every frame (`@641956`).

### 3b. Body follows the capsule (the handles)
Both handles grab at spawn (`@19027`, `@19166`):
- **UpperBody** grabs `spine_05`. Each frame its target is X/Y = the capsule transformed by (0,0,55) + `Movement Dislocation` rotated by the body yaw, and Z = **the chest's own current Z** (`@409486`). It pulls the chest horizontally over the capsule and never vertically. Its stiffness and damping are set just before (`@406035`, `@408826`).
- **LowerBody** grabs `pelvis` with rotation. Its target is lerped between "the capsule position at crouch height" (capsule Z + `Crouched Height`/scale − 85) and **the pelvis's own position** (`@415692`). The alpha is (both feet within 30–75 cm of the ground) × (1 − `Fallen Rate`). So with both feet planted, the handle barely pulls and the legs carry him. With his feet in the air, the pelvis is hauled to the capsule. Its rotation target is lerped too (`@420645`), and its stiffness and damping change with `Fallen Rate` (`@410313`…`@414472`).
- A third joint, `NODE_AddPhysicsConstraintComponent-116`, is added at `@242006` and constrains **`pelvis` to the `CapsuleComponent`** directly.

**Why he flew 4–7 m:** the capsule chases the hips through a leash, and the hips are hauled back to the capsule by handles and a pelvis–capsule joint. A pose that is pinned rigidly (kinematic hands on a sword stuck in a pell) holds the hips away from the capsule. The leash then moves the capsule by up to 75 cm a frame toward a displaced target. The handles drag the body after it, and the loop runs away. To pose the body directly, **disable the capsule-follow** (keep `Condition` false and override, or set the capsule yourself after the game's tick). **Also release or retarget the two handles and the pelvis–capsule joint**, otherwise they fight the posed body.

## 4. The physics asset `Willie_PhysicsAsset_Capsules`

All 22 bodies are authored `PhysType_Kinematic` and switched to simulate at runtime. Mass overrides are 100 kg on every body except `lowerarm_*` (1.5 kg) and `hand_*` (1.0 kg). The game then calls `SetAllMassScale` and scales by `Mass Scale (Set in BP)`.

| Joint name | Child → parent | Swing1 / Swing2 / Twist limits (°) |
|---|---|---|
| UserConstraint | thigh_r → pelvis | 30 / 30 / 90 |
| UserConstraint_0 | calf_r → thigh_r | 5 / 5 / 75 |
| UserConstraint_1 | foot_r → calf_r | 30 / 30 / 45 |
| UserConstraint_2 | spine_02 → pelvis | 15 / 10 / 20 |
| UserConstraint_3 | spine_03 → spine_02 | 15 / 10 / 20 |
| UserConstraint_4 | spine_04 → spine_03 | 25 / 10 / 20 |
| UserConstraint_5 | spine_05 → spine_04 | 15 / 10 / 20 |
| UserConstraint_6 | neck_01 → spine_05 | 25 / 10 / 25 |
| UserConstraint_7 | neck_02 → neck_01 | 25 / 10 / 25 |
| UserConstraint_8 | Head → neck_02 | 25 / 10 / 25 |
| UserConstraint_9 | clavicle_r → spine_05 | 30 / 30 / 30 |
| UserConstraint_10 | upperarm_r → clavicle_r | 75 / 90 / 30 |
| UserConstraint_11 | lowerarm_r → upperarm_r | 75 / 5 / 90 |
| **UserConstraint_12** | **hand_r → lowerarm_r** (right wrist) | 45 / 75 / 90 |
| UserConstraint_13 | clavicle_l → spine_05 | 30 / 30 / 30 |
| UserConstraint_14 | upperarm_l → clavicle_l | 75 / 90 / 30 |
| UserConstraint_15 | lowerarm_l → upperarm_l | 75 / 5 / 90 |
| **UserConstraint_16** | **hand_l → lowerarm_l** (left wrist) | 45 / 75 / 90 |
| UserConstraint_17 | thigh_l → pelvis | 30 / 30 / 90 |
| UserConstraint_18 | calf_l → thigh_l | 5 / 5 / 75 |
| UserConstraint_19 | foot_l → calf_l | 30 / 30 / 45 |

All joints are linear-locked with soft limits (stiffness 50) and TwistAndSwing angular drives. The slerp stiffness is 250 for the spine, neck, clavicles and hips, and 50 for the limbs.

At spawn the game applies the joint profile **`Motor`** to all of them (`SetConstraintProfileForAll`, `@21776`). It applies the profile **`Broken`** to single joints when a bone breaks:
- `UserConstraint_18` for the left leg (`@242266`)
- `UserConstraint_0` for the right leg
- `UserConstraint_15` for the left arm
- `UserConstraint_11` for the right arm

Dislocations add extra PhysicsConstraintComponents at runtime (`@697931` and similar).

Bone names, so you don't have to guess: `pelvis spine_02..05 neck_01 neck_02 Head clavicle_l/r upperarm_l/r lowerarm_l/r hand_l/r thigh_l/r calf_l/r foot_l/r`. Sockets that are used include `weapon_lSocket`, `Sheath_Slot_Back` and `Sheath_Slot_L_1`.

## 5. Physical animation ("muscles")

- **At spawn:** `SetSkeletalMeshComponent(Mesh)`, `ApplyPhysicalAnimationProfileBelow('pelvis', 'PA')` and `SetStrengthMultiplyer(1)` (`@20984`–`@21945`).
- **`Control Physical Animation Str`** (its own function, about 57 kB of script) applies `ApplyPhysicalAnimationSettings` to **pelvis, spine_02–04, neck_01, neck_02, clavicle_l/r, upperarm_l/r, lowerarm_l/r, hand_l, thigh_l/r, calf_l/r, foot_l/r**. **`hand_r` is not in the list.**
- For every bone the settings are:
  - `bIsLocalSimulation = false` (world space)
  - **`PositionStrength = 0`, `VelocityStrength = 0`**
  - `OrientationStrength = PA Strength <bone>` (per-bone variables)
  - `AngularVelocityStrength` = tonus × consciousness × upright-tilt × grab state × `Angular Strength`
  - `MaxLinearForce` and `MaxAngularForce` from `Max Linear/Angular Force` × `Constraint Rate`, mass-scale corrected
- So **the muscles only twist bones toward the animated pose. They never pull a bone toward a position.** All positional support comes from the handles in §3b and from the joint chain.
- Finally it calls `SetStrengthMultiplyer(PA Base Strength Multiplier)`.
- **What triggers it:**
  - the 1–2 s random refresh (`Force Refresh Pa Strength`, `@402396`→`@402665`)
  - `Phys Anim Array` iteration (`@299278`, for dismembered parts)
- **`R/L Hand Toggle`** is the "hand active / hand released" switch:
  - The grab and swap events flip it (`@130230`, `@231843`, `@436903`/`@437812`).
  - `Set Up Right/Left Hand Weapon` resets it to false.
  - `Send Threat Location R` reads it.
  - `ABP_Willie` reads it as `R Hand Toggle` / `L Hand Toggle`.
- `Muscle Motor` is **not** the main muscle code. It only drives the joints of severed body parts (`Dismembered Body Constraints`) toward the DriverSkeleton pose.

## 6. Arms and the animation graph

`ABP_Willie`'s variables are filled from Willie in `BlueprintUpdateAnimation`.

**Arms:**
- `Aim Vector`, `Aim Swing Speed`, `Upper Body Aim Rotation Rate`, `Total Y Aim`
- `R/L Thrust Alpha` and `R/L Thrust Position Alpha`, `Alt Thrusting`
- `IK R Hand Location` / `IK L Hand Location`, which feed the two TwoBoneIK nodes
- `R Two Handed Grip`, `L Hand In Offhand Attached`, `Distance Between Hands 2H`
- `R/L Hand Stuck` (`R Hand Stuck Under L Armpit` and similar), `R/L Hand Grip Slide`

**Legs:**
- `Right/Left Foot Effector Location`, `R/L Knee IK Location`
- `Kicking R/L`, `Kick Rate R/L`
- `Is Crouched`, `Jump Rate`, `Pressed Jump`, `Last Jump Power`

The arm swing is therefore computed in the anim graph from a 2D aim vector, not from a hand position. Then the physics body is twisted toward it.

## 7. Weapons: attach, grip, sheath, pick-up

- Weapons are `ModularWeaponBP_C` actors, a separate simulated `BaseMesh` plus modules.
- `Set Up Right Hand Weapon(class, …)`:
  1. Spawns or takes the weapon and breaks any old `PhysicsConstraint R Hand`.
  2. Teleports `BaseMesh` so the weapon's `Grip R Hand Default Pos` (or `…Reverse`, or the L-hand position for two-handers) lands on the hand.
  3. Sets `Weapon R->Rigidity = Hands Rigidity (Gauntlets)` (0.95 seen) and the owner.
  4. Turns off collision between the weapon and selected body bones through the game's own C++ library **`CollisionDisablerFunctionLibrary:DisableCollision_SkeletalVsSingleBody`** (`@5871`). The bones are `pelvis lowerarm_r hand_r` plus a list per grip type.
  5. Adds a **PhysicsConstraintComponent from the template `NODE_AddPhysicsConstraintComponent-77`** and stores it in `PhysicsConstraint R Hand`.
- **The grip joint** (live settings):
  - `ComponentName1 = BaseMesh`, `ComponentName2 = CharacterMesh0`, `ConstraintBone2 = hand_r`
  - Re-bound to **`lowerarm_r`** for grip types 6 and 7 (`@235126`)
  - Linear X/Y/Z locked, but X is relaxed to `SetLinearXLimit(Limited, R Hand Soft Attach Distance)` for non-default grips
  - Swing and twist locked as **soft** limits (stiffness 50, damping 5)
  - Projection on (linear tolerance 5 cm)
  - **Breakable:** linear 300, angular 500
  - Linear position and velocity drives on, angular drives with stiffness 50 and damping 1
  - Collision disabled between the two bodies
  - The linear drive strength is rewritten at runtime from `Weapon R->OffhandStr` (50000) and grab and two-hand state (`@82979`, `@134478`, `@455170`…)

  So the sword hangs on a soft spring off a 1 kg hand. A kinematic or rigid hand with a heavy blade on soft angular limits will bounce. Weapon damping changes on pick-up and drop (`Set Weapon Damping Pick Up / Drop`).
- **Sheathing:**
  - `Create Sheath Constraint R/L` constrains the weapon's `BaseMesh` to `Mesh` at a sheath socket.
  - `Sheath R/L` moves `GrabPoint R/L` to the socket.
  - `Unsheath R/L` and `Unsheath R/L Event`, `Draw Weapon`, `Sheathe on Spawn`, `Check Scabbard Unsheath State`, `Release/Grab Scabbard L Hand`.
  - The scabbard itself is constrained to `pelvis` (`@3232`, `@4651`).
- **Pick-up and grab:**
  - `Pick Up Left/Right Event` and `Find Grab Location Left/Right Event` trace for a target.
  - Grabbing a non-weapon rebinds `PhysicsConstraint L/R Hand` as `(Mesh, hand_*, Grab Component *, Grab Bone *)` (`@526273`, `@538179`).
  - Release goes through `Release Grab L/R`, `Break R/L Constraint`, `Left/Right Hand Constraint Soft Release`.
- **Swap and two-hand:** `Event Force Hands Switch`, `Polearm Hands Switch`, `Attach L Hand To Offhand Position` (when `weapon_lSocket` is within 10 cm of the weapon's `L Hand Attach Point`), `Release Offhand`, `Event Reverse Grip`, `Event Slide Right Hand Along`.
- **Input events in Willie:** `Grab Left/Right`, `Swap Hands`, `Thrust`, `Thrust Gamepad`, `Jump`, `Crouch Key`/`Crouch Hold`, `Run`, `Key Forward/Back/Left/Right`, `Change Camera`, `Toggle Camera Lock`, `Undress`, `Inventory`, `Talk`, `Chain Attack`, `SloMo`, `Pause`. The aim comes from the axis events `Left/Right Guard Axis`.

## 8. Crouch, kick, thrust, jump

All four are **state variables that feed the anim graph and the capsule height**. None of them pushes the body directly.
- **Jump:** `Jump Pressed`, `Jump Rate` and `Last Jump Power` (`@230791`). It needs a foot near the ground and `Jump Rate ≥ 0.5`. The capsule target Z is raised by `Last Jump Power × 50 × scale` while held (§3a), and the handles carry the body up with it. It also sets `Constraint Rate` (`@402699`).
- **Crouch:** `Crouch Key Holding`, `Is Crouched??` and `Crouched Height`. It lowers the LowerBody handle target (§3b), and `SetCapsuleHalfHeight(90)` restores the capsule (`@18792`). Holding crouch can trigger `Dolphin Dive`.
- **Thrust:** sets `BobnWeave`, `Alt Thrusting`, `Just Thrusted` and `R/L Thrust Position Alpha` (`@240615`). It changes the swing model and the arm pose in ABP_Willie.
- **Kick:** `Kick Event` → `Kick L/R Timeline`, which drive `Kick Rate L/R` → ABP leg pose.
- **Walking:** `AddMovementInput` on the capsule (`@750351` and similar), scaled by `Walk Speed Rate Run`.

## 9. How hits and damage reach the player

1. A weapon part's `OnComponentHit` (Head, Guard, Grip, Pommel, BaseMesh or a submodule) calls `ModularWeaponBP::Collision Hit`.
2. That calls the target Willie's **`Weapon HIt(Hit Component, Weapon Velocity, Hit Result, Edge Alignment Dot, …)`**.
3. Willie calls **`Deal Complex Damage(Hit Component, Collided Component, Hit Bone, Location, Normal, Hit Velocity, Hit Impulse, Cutting Power, Stab Rate, Rigidity, …)`**.
4. That calls **`Get Damage(Impulse, Velocity, Location, Normal, bone, Raw Damage, Cutting Power, …)`**, which applies pain, `Consciousness`, per-limb health, `Leg/Arm … Broken` with the `Broken` joint profile, `Dislocate Bone`, `Dismember Function Initiate`, and blood and paint effects.

`Invulnerable` (a variable checked in `Box Vertex Paint` and elsewhere) blocks damage. Bites go through `BP_Constraint_Bite_C`, and stuck weapons through `Constraint_Weapon_Stuck_BP_C`.

## 10. Getting from launch into a fight with no input (self-test)

This is proven three times on 2026-11-16 with the installed mod's `dev_script`:

```
dev_fake_headset=1
dev_script=25:open /Game/Maps/Arenas/Map_Arena_Cellar;170:quit
```

The mod is ready at about 10 s. At 25 s it runs the console command `open`. **The player `Willie_BP_C` exists about 1.4 s later, already holding a weapon** (a sickle in the Cellar), with no menu clicks. `quit` closes the game cleanly. Other arenas seen in logs: `Map_Arena_Yard`, `Map_Arena_Slums`, `Map_Arena_Ambush_Test`. The tavern hub `Map_Hub_Tavern_Frank` spawns `NewBlueprint_C` instead of Willie.

## 11. What this changes for the body work

1. **Don't fight the capsule leash. Turn it off.** Pose the body, then put the capsule under the pelvis yourself after the game's tick, so the leash never fires. The game's own fallback when `Condition` is false is exactly "capsule = pelvis".
2. **The handles are what move the body.** `PhysicsHandle UpperBody` (spine_05) and `PhysicsHandle LowerBody` (pelvis) are retargeted every frame, and there is also a pelvis–capsule constraint. Release them (`ReleaseComponent`) or feed them the headset-derived targets, rather than adding a third force on top.
3. **The wrist is `UserConstraint_12` (right) and `UserConstraint_16` (left).**
4. **The muscles are rotation-only and get re-applied every 1–2 s,** so a mod's PA changes must be re-applied after each refresh, or `Force Refresh Pa Strength` must be watched. The right hand has no PA entry.
5. **The sword is a separate simulated body on a soft, breakable constraint to `hand_r`.** For a rigid hand, stiffen that joint (lock the angular limits hard, raise the drives) or attach the weapon kinematically to the hand. Expect the 300/500 break thresholds to snap it if the hand is forced hard against a pell.

## 12. What the script does every frame that a placed body must answer (found 2026-11-16)

All in `Willie_BP_C`'s event graph unless said otherwise; offsets are for `tools/study/kx.py`.

- **Capsule leash** (`@636941` to `@641773`): target = lerp(pelvis, spine_05) + `Dodge Vector` x
  `Dodge Rate` + `Pain Stumble Delayed` (at most 20) + `Stumble Direction` + `Shoulders Dislocation`
  clamped to 0..25. If the capsule is further from that than `Capsule Max Dislocation`, the capsule
  is set (no sweep) to within that reach. While not `Fallen` the reach is assigned directly each
  frame (`@643190`): 10 at rest, up to 75. `Kick Event` adds 50 cm forward to `Stumble Direction`
  (`@742755`), which fades at rate 5. These two `K2_SetWorldLocation` calls on the capsule are the
  only places the script moves the character.
- **Falling** (`@434189`): `Free Fall Rate` rises at 1/s towards distance-from-`Last On Ground
  Position`/200 while not (`Activate Constraint` or `Ass touching ground` or `Jump Pressed`). At 0.5
  and above the capsule path changes and leg contacts count as landings.
- **Body hits** (`BndEvt__BP_ThirdPersonCharacter_Mesh_..._ComponentHitSignature`, `@809686`): stores
  `Other Actor`, `Other Comp`, `Body Hit Normal Impulse` (clamped to 1500 unless falling), then
  gates on impulse size. Own actor is ignored. Things tagged `Weapon` are left to the weapon's own
  handler. A hit by something implementing `BPI_ComplexCollision` makes the OTHER side take the
  blow (`@797190`): that is how a kick or punch hurts an enemy, so the event must keep firing.
- **Dislocation** (`Dislocate Bone`): first line returns if `Dislocation Blocked`. Otherwise bend of
  the bone against its parent times strain; at 0.1 and above the limb's health drops each call and
  ends in `... Dislocated = true`, health 0.
- **Stuck weapons**: `ModularWeaponBP_C` spawns `Constraint_Weapon_Stuck_BP_C` actors into `Stuck
  Constraints Array` (properties `Hit Actor`, `Component 2 (Body)`); `Release All Jammed Bones`
  releases them all and calls `Temporary Disable Damage` (0.2 s).
- **`CollisionDisablerFunctionLibrary`**: the last parameter of the `DisableCollision_...` calls is
  a FloatProperty (seconds; -1 = for good).

