# Blade & Sorcery melee combat, read in full, and how Half Sword VR differs

Written 2026-11-24 for Matthew's request: a broad read of everything in B&S (ThunderRoad) that touches melee, then a list of differences from our mod that other threads can act on.

## Sources and legend

- **B&S code:** ILSpy decompile of `ThunderRoad.dll` (game build of 2026-10-14) in `HalfSwordDecomp\bs-decomp\ThunderRoad\` (private, never in a repo). File:line references below point there (`ThunderRoad\` sub-folder unless noted).
- **B&S data:** the whole `StreamingAssets\Default\bas.jsondb`, extracted to `HalfSwordDecomp\bs\basjson-full\` (private). Flat tables made from it: `HalfSwordDecomp\private\bsan\damagers_table.txt`, `modifiers_table.txt`, `items_table.txt`, `brain_med.txt`, `game_flat.txt`, `hm.txt` (NPC creature), `pm.txt` (player creature).
- **Our mod:** branch `claude/project-thread-wo2o0x` tip 74331c5 (contains the installed 0b2b73c), `mod/HalfSwordVR/src/config.hpp` and `dllmain.cpp`. Copy in `HalfSwordDecomp\private\hsvr-src-74331c5\`.
- **Half Sword game rules:** `HalfSwordDecomp\internals-map.md` and the `defaults\*.md` notes.
- **Earlier B&S notes this builds on (not repeated here):** `bs_vs_hsvr_hold.md` (hold joint), `bs_hold_measured.md` and `bs_full_measured.md` (logged in play), `bs_handles_halfswording.md` and `bs_handle_geometry.md` (handles, sliding, half-swording), `bs_body_physics.md` (Player Self Collision), `bs_haptics.md`, `bs_throwing.md`, `bs_holders.md`.

Tags: **[code]** read in the decompile. **[json]** read in the game data. **[derived]** my arithmetic from code + data. **[inferred]** a reasoned guess, not checked. **[ours]** read in our source or project notes. **[HS]** Half Sword's own game logic from our decompile notes.

---

## TL;DR

1. **B&S has no melee swing assist of any kind.** Nothing adds force or speed to a held weapon. The only velocity writes are the throw multiplier (×2 above 2 m/s) and restoring the weapon's pre-impact velocity when it penetrates or slices (so a cut does not stall). [code]
2. **B&S damage is speed-only.** Damage = the damager's speed curve (impact speed at the contact point, before contact) × tier × material × body part. Weapon mass is not in the formula. Half Sword is impulse-based, which is why we needed hit assist and stab assist and B&S did not. [code + json]
3. **Most blade damage in B&S comes from entering flesh, not from the impact.** A sword cut does ~1-2 damage on impact but 15 × part multiplier on entering (neck ×32 = instant kill). A thrust adds a "deep" bonus past 2-10 cm (head ×32 at 4 cm). [code + json]
4. **Every blow counts in B&S.** The only repeat filter is 0.1 s per damager per target collider. Half Sword's per-bone filters turned out to be the same kind of thing for weapons: the 11-23 logs show they clear within ~0.1-0.2 s and dropped no separate weapon blow (item 1).
5. **Reactions are an animation table.** Impact speed picks a push level per material (e.g. blunt on flesh 4 / 7.5 / 9 / 12 / 24 m/s). The NPC's state and the body part pick light/medium/full stagger or a ragdoll fall. Live NPCs get no extra shove from weapons; only dead or fallen ones do. [code + json]
6. **Parry is NPC-side only.** An NPC whose attacking weapon hits your weapon faster than 1 m/s aborts and recoils (1.5 s cooldown). Your hard blow on a blocking NPC can stagger him, and if he falls, he drops his weapon. You never recoil or stagger. [code + json]
7. **NPCs are disarmed by:** a blunt/punch hit on the hand at exactly push level 2; a stab that sticks in the hand or forearm; an arm hit at level ≥2 while you also hold his weapon; falling while blocking; and a neck grab (one weapon after 2 s, both after 5 s). The player is never disarmed by hits. [code + json]
8. **The player takes 5-15 damage per hit, clamped** (100 HP). No stagger, no knockdown. [code + json]
9. **No stamina** anywhere in B&S. [code]
10. **Haptics we lack:** B&S buzzes 0.9 for every 1 cm a held blade moves in or out of a wound. [code + json]

---

## Prioritized difference list (for other threads)

Effort: S = a day or less, M = a few days, R = research needed first.

| # | Area | B&S | Half Sword VR now | Suggested change | Effort | Evidence |
|---|---|---|---|---|---|---|
| 1 | Repeated blows (**checked 11-24: no fix needed**, see `privateehiteport.md`: in 151 weapon blows over the three 11-23 sessions none was dropped; both gates clear within ~0.1-0.2 s) | Every hit counts. Only a 0.1 s debounce per damager per collider (`Damager.TryHit` Damager.cs:665; `hitDelayByCollider` 0.1 in every damager JSON). | The game drops a hit on a bone unless it beats the last one (`Deal Complex Damage`: \|imp\|·(cut+1) ≥ Last Complex Damage Impulse; `Get Damage`: ≥ Last Damage Taken·(DrawCut+1)). `pose_fist_new_punch_counts` clears both, but **for fists only** (dllmain.cpp ~10300). Blunt weapons have cut ≈ 0, so repeated mace/hammer/pommel blows on one bone face the same gate punches did (38 of 58 dropped). | First count in a playtest log how many weapon hits on a person were dropped by those two gates. If many: extend the "new blow counts" clear to held weapons (new blow = > 0.1-0.25 s since that weapon's last contact on that person). | S (after a log check) | B&S [code+json]; HS gate [HS]; that weapons are hit [inferred] |
| 2 | Wound haptics | While a held weapon is in a wound, one hand buzzes 0.9 for each 1 cm the depth changes, in or out (`Damager.UpdatePenetration` Damager.cs:356-372; `haptics.penetrationPeriod` 0.01, `penetrationIntensity` 0.9). | No wound buzz found (`hit_buzz` covers impacts and scrapes only). | Add a buzz tick per cm of depth change while Stuck In Body, on the holding hand. | S | [code+json], [ours] |
| 3 | Disarming NPCs by hits | Hand hit at push level exactly 2 → drop (shield hand excepted); a pierce that penetrates the hand/forearm → that hand drops; arm/hand hit at level ≥2 while the player holds the same weapon → drop (`BrainModuleEquipment.OnDamage` :208-288; HumanMedium values `grabDisarmPushLevel` 2, `handHitDisarmPushLevel` 2). Level 2 = blunt 9-12 m/s, punch 6-12 m/s on a bare hand; slashes never reach level 2 on flesh. | `disarm` = tug of war only (grip strength ×3 for 0.25 s). Half Sword's own hit-to-drop rule for NPCs is not mapped. | First check whether Half Sword already drops weapons on hand hits (log a few hand hits). If not: a stab stuck in his hand/forearm, or a hard blunt/fist blow on his weapon hand, makes him let go through the game's own release. | R then M | [code+json]; HS side unknown |
| 4 | Choke / grapple rules | Neck grab = choke: NPC plays a choke animation, drops one weapon after 2 s and both after 5 s, can't stand up. Neck grab while he is idle or fully staggered → he falls. Leg grab during his attack/stagger → falls. Lifting the neck above standing height → falls; lifting torso/shoulder/head → your hand is let go. His head pulled below his hips → falls. Released while moving > 2 m/s → falls. He turns and steps to follow your grab (`BrainModuleGrabbed` :120-250, `HandleRagdoll.ManagedUpdate` :117-225, `Interactable_Human*.json`). | Body grab is physical (`pose_grab_body_*`, joint_break), falls come from Half Sword's balance. No weapon drop while choked. | Add "choked = drops weapon after a hold time" (reuse the disarm path). The fall rules are probably covered by Half Sword's physics; check in play before copying them. | M | [code+json] |
| 5 | Blocking interrupts an attack | An attacking NPC whose weapon meets the player's weapon at > 1 m/s stops the attack and plays a recoil (or a riposte, 10-15% chance), then can't be recoiled again for 1.5 s (`BrainModuleMelee.OnWeaponCollision` :837-962, `BrainModuleHitReaction.StaggerCoroutine` :446-560). A blocking NPC hit hard enough by a weapon whose AI data allows push transfer staggers, and if he falls, drops his weapon (`BrainModuleDefense.OnWeaponCollision` :604-700). | Not handled by the mod. Half Sword has `Just Parried Rate` (damps the attacker's impulse ×0.5) but no blueprint writes it, so it never fires [HS]. | Log whether a Half Sword NPC's swing continues after a VR block. If it does and feels wrong, research a way to cut his attack short (needs Half Sword AI mapping first). | R | [code+json]; HS [HS] |
| 6 | Hits through the shield | A hit on the shield-holding arm or hand is cancelled when a ray along the weapon's last-frame path crosses the held shield (`Damager.ShieldSweep` Damager.cs:909-972). | No such check. Half Sword weapons use CCD, so it may already be rare. | Only if Matthew reports arm hits behind his shield: skip damage on his shield arm when the blow's path crossed his shield. | S | [code] |
| 7 | Own weapon vs own body | Default: the held weapon ignores the whole body **except the feet and the free hand** (`Item.RefreshCollision` Item.cs:3424, 3500+; see `bs_body_physics.md`). With the Self Collision option, it also hits torso, head and legs. | `pose_hold_ignore_body` ignores the whole body, feet and free hand included. | Optional: let the free hand and feet touch his own weapon (part of the "spear brace / force field" plan). | M | [code], [ours] |
| 8 | Player hit damage | Each hit on the player is clamped to 5-15 HP (×tier player multiplier, ×difficulty); 100 HP; never staggered (the player brain has no hit-reaction module) (`Creature.Damage` Creature.cs:3592-3625; `Brain_Player.json`). | Half Sword's own rules: consciousness, limb health, bleeding, knock-down (`player_takes_damage`, injuries). | None unless Matthew wants an arcade option. Listed so it is a conscious choice. | — | [code+json] |
| 9 | Open-hand and forearm hits | The empty hand is a 1 kg dynamic body on the same springs as a held weapon, and always carries the `Punch` damager, fist or not (`PlayerLink.AttachRagdollPart`, `RagdollPart.SetBodyDamager` :782, `PlayerHand` :201/351). Arms and torso don't collide with NPCs at all (Avatar layer ignored, PlayerHand.cs:185-193). | Fists hit as the game's fist weapon; open-hand slap (`pose_fist_slap`); elbows (`pose_elbow_strikes`); posed body contact scaled by closing speed. | None needed. Ours is broader. | — | [code], [ours] |
| 10 | Kick | Button kick: a sphere cast (r 0.2 m, 2 m) picks the target; only below 3 m/s walking; foot extends 0.2 s, holds 0.1, returns 0.4 (`PlayerFoot` :337-410, `Player.kickLocomotionMaxSpeed`). The `Kick` damage modifier has very low push thresholds (1 / 1.5 / 2 / 3 / 18 m/s on flesh), so almost any kick staggers or drops him. | `kick_on_a` → the game's own Kick Event. | None. | — | [code+json] |
| 11 | Swing assist | None (see §1). | `hit_assist` (3 kg × strength × closing speed added to the impulse), `stab_assist` (4 kg × speed, or a 60 N push for 0.1 s), `pose_feed_swing_speed`, whoosh threshold ×0.6. | Keep. They make up for Half Sword's impulse-based damage, which B&S doesn't have. | — | [code], [ours] |
| 12 | Edge angle | A slash damager only cuts when the impact direction is within 35° of the blade's cutting direction (sideways) and 90° (vertical), and the surface normal within 60°; otherwise it counts as a blunt hit for 0.5 base damage (`Damager.CheckAngles` :482-540; `SwordSlash1H.json`). | Half Sword's continuous cutting rate (edge alignment × sharpness) [HS]. | None. | — | [code+json], [HS] |
| 13 | Collision detection | Held weapons: Discrete; thrown, NPC attacks, kicks: ContinuousDynamic (`Game.json collisionDetection`, Item.cs:2894). | Half Sword weapons have CCD on [HS]. | None. | — | [json+code] |

Not differences worth acting on: dismemberment (B&S slices only when the blade penetrates a non-kinematic part at ≥ 12 m/s × tier factor; Half Sword has its own), knockout fields (B&S data has `knockoutAllowedMinVelocity` etc. but no code reads them, DamageModifierData.cs:142-144), NPC hands being immune while they attack (Creature.cs:3567).

---

## 1. Swing assist, velocity, strength and weight

- **No melee assist.** Every velocity/force write on items was checked (`grep AddForce|velocity =` over Item, Handle, PlayerHand, RagdollHand, CollisionHandler, Damager, Creature, Ragdoll). The only writes are: throw ×`throwMultiplier` (Item.cs:2975, Game.json `throwMultiplier` 2, `throwVelocity` 2 m/s); buoyancy (Item.cs:1380); recoil for guns (Item.AddRecoil); telekinesis; and two "keep going" writes in `Damager` (below). [code]
- **Keep going on entry.** When a blade penetrates, its velocity is set back to its last pre-contact linear velocity × `penetrationInitialVelocityMultiplier` (1.0 in every base damager) and its angular velocity restored (Damager.cs:1256-1257). When it slices a limb off, the same restore happens (Damager.cs:781-782). So the contact never robs the swing. [code+json] In our mod the exact hold drives the weapon's velocity from the controller each frame, which has the same effect while held. [ours, inferred]
- **"Aim assist" exists only for skills and throws** (`Creature.AimAssist`, `SkillDiveStrike`, `SkillRangedExpert`), not for held melee. [code]
- **Mass and strength.** Hold drives never use the item mass (see `bs_vs_hsvr_hold.md`). The player has no strength stat; only skills and status effects scale the joint forces (`Creature.jointForceMultipliers`). Damage never uses mass either (§3). Mass matters only through physics contact, the force caps (weight feel), the NPC "push attack" (§6) and the extra shove on dead/fallen bodies. [code]
- **Item feel data** ([json], `items_table.txt`): most weapons set `overrideMassAndDrag` with linear drag 1.0 (daggers, rapiers, spears 0.4) and angular drag 0.05. Typical masses: dagger 0.65, rapier 0.85, short sword 1.25, longsword 2.0, greatsword 4.0, 1H mace 1.5, maul 8, halberd 3.85, spears 1.2-1.65, round shield 3.0, buckler 1.0 kg. Tiers 0-5 set the damage and dismember multipliers.

## 2. Hit detection

- **Impact speed** (CollisionHandler.cs:671-679, 602-632): the point velocity of the source at the contact, from its **last FixedUpdate** linear + angular velocity (i.e. just before the contact), minus the target's point velocity the same way. Not the solver impulse. [code]
- **Who processes item-vs-item:** only the faster of the two (CollisionHandler.cs:755). Ragdoll targets are always processed. [code]
- **Ignored contacts** (CollisionHandler.cs:681-785): locomotion layers; ragdoll parts hit by dropped/moving items or by the player's hand/foot layer from their own side; a standing-up ragdoll; a fallen/dead ragdoll hitting itself; items slower than 0.25 m/s unless held by the player (`collisionEnterVelocityRange` 0.25-15); ragdoll parts slower than `collisionMinVelocity` 2 m/s (these only "touch" for stealth). [code+json]
- **Intensity** for effects/haptics = inverse-lerp of impact speed over 0.25-15 m/s, or over the damager's own range if set. [code+json]
- **Up to 10 simultaneous contacts per body**, a contact is dropped after 5 frames without a stay. [json]
- **Which damager:** each collider group has damagers sorted Pierce, Slash, Blunt (CollisionHandler.cs:295-313); the first one whose `TryHit` passes wins (CollisionInstance.cs:731-770). So a tip hit is tried as a thrust first, then as a cut, then blunt. [code]
- **Who can be hurt by whom** (`CollisionInstance.ShouldDamage` :586-620): the player is hurt by anything except his own held item, unless Player Self Collision is on. [code]

## 3. Damage

**Formula** (`Damager.TryHit` Damager.cs:656-851, `Creature.Damage` Creature.cs:3558-3650) [code]:

```
base   = damager.velocityDamageCurve(impact speed)          (bad angle: badAngleDamage, blunt type)
damage = base × (thrown ? throwedMultiplier : 1) × tier.damageMultiplier × material.damageMultiplier
              × item skill multiplier × bodyPart.damageMultiplier
player victim: × tier.playerDamageMultiplier, then clamp(playerMinDamage 5, playerMaxDamage 15), × difficulty
NPC victim with physics off: × physicsOffDamageMult (0.75)
```

Gates before damage: impact speed ≥ the material modifier's `minVelocity` (if it can penetrate, the lower of that and the penetration minimum); for slashes and thrusts, the angle check (Damager.cs:503-540) or a blunt fallback.

**Damager data** ([json], all 42 in `damagers_table.txt`), base curves are 2-key curves (speed m/s → base):

| damager | curve | tier dmg ×(0..5) | angles H/V/N | penetration | addForce (dead/fallen only) |
|---|---|---|---|---|---|
| SwordSlash1H | 2→1, 20→2 | 0.5 / 0.7 / 0.9 / 1 / 1.1 / 1.1 | 35 / 90 / 60 | yes, 15 dmg on entry, slides, held damper in 10000 / out 100 | 0 |
| SwordSlash2H | 2→1, 20→2 | 0.7 … 1.3 | 35 / 90 / 60 | yes, 15 | 0 |
| SwordPierce | 0→1, 20→10 | 1 | 35 / 35 / 60 | yes, 5 deep, skewer 15, held damper in 1000 / out 500 | 0 |
| DaggerPierce | 0→1, 20→5 | 1 | 35 / 35 / 60 | 7.5 deep, held in 500 / out 250; thrown ×3 | 0 |
| AxeSlash1H / 2H | 2→1, 20→3 | 0.5…1.1 / 0.7…1.3 | 45 / 80 / 80 | 25 entry, no slide, held in 0 / out 10000 (axe sticks) | 50 accel 0.15 s / 100 impulse |
| Mace1H / Mace2H / MaceMassive | 2→1, 20→5 | 1.5…2.55 / 2.25…3 / 2.6…4 | any | none | 105 / 200 / 200 accel |
| WarpickPierce | 0→1, 20→10 | 1.25 | 45 / 45 / 60 | 20, pierces plate | 0 |
| Punch | 2→1, 20→5 | 1 | any | none | 100 accel 0.15 s |
| Kick | 2→1, 20→5 | 1 | any | none | 400 accel 0.15 s |
| Shield / ShieldStrong | 2→1, 20→2 / 20→5 | 0.5…0.85 / 0.65…1 | any | none | 100 / 400 accel |
| HandleHeavy / HandleLight | 2→1, 20→5 / 20→2 | 1 | any | none | 65 / 30 |

All melee damagers: `hitDelayByCollider` 0.1 s; player min/max damage 5 / 15.

**Material modifiers** ([json], `modifiers_table.txt`; first matching entry for the weapon's tier, imbue-only rows skipped):

| modifier → target | minV (m/s) | damage × | penetrates | pen. min V | push levels (m/s) |
|---|---|---|---|---|---|
| Slash → flesh | 3 | 1 | yes (depth cap 3 mm, see §4) | 3 | 3 / 8 |
| Slash → leather | 3 | 0.25-0.35 | tier ≥1, min 5-8 m/s | | 3 / 9 |
| Slash → chainmail, wood | 8 | 0.1 | no | | 12 / 15 |
| Slash → plate, metal, stone | 8 | 0.2 | no | | 8 / 12 |
| SlashAxe → flesh / leather / chain / plate | 3 / 3-4 / 3-4 / 3 | 1 / 0.8 / 0.8 / 0.8 | flesh ≥ 6 m/s; leather ≥ 8-10 and chain ≥ 10-12 (tier ≥ 1 only); plate never | | 3/6/9/14/24 on flesh |
| Pierce → flesh | 1 | 1 | yes, min 0.5-1 m/s, damper ×3 (tier 0) … ×0.5 (tier 4+) | | 6 / 15 |
| Pierce → leather | 3 | 1 | min 3-6 | | 6 / 18 |
| Pierce → chainmail | 3 | 0.1 (tier 0), 1 (tier ≥1) | tier ≥1, min 3 | | 9 / 18 |
| Pierce → plate, metal, stone | 12 | 0.05 | only above 50 m/s (never) | | |
| PierceWarpick → plate | 4 | 1 | yes, min 4 | | 8/10/14/18 |
| Blunt → flesh | 1 | 1 | | | 4 / 7.5 / 9 / 12 / 24 |
| Blunt → leather / chainmail | 4 | 0.5 | | | 8 / 10 / 12 / 16 / 24 |
| Blunt → plate | 4 | 0.5 (tier 0), 0.66 | | | 9 / 11-13 / 13-16 / 16-19 / 24 |
| Punch → flesh / leather / chain / plate | 2 / 3 / 4.5 / 4.5 | 0.5 / 0.4 / 0.3 / 0.2 | | | 2/4/6/12/24 on flesh |
| Kick → flesh / plate | 1 / 6 | 1 / 1.5 | | | 1 / 1.5 / 2 / 3 / 18 on flesh |

**Body parts** ([json], NPC `Creature_HumanMale`, 50 HP; player `Creature_PlayerDefaultMale`, 100 HP):

| part | NPC damage × | NPC deep pierce depth / × | NPC slash entry × | player damage × |
|---|---|---|---|---|
| head | 3 | 4 cm / 32 | 2 | 1.5 |
| neck | 1 | 2 cm / 32 | 32 | 1.5 |
| torso | 1 | 10 cm / 2 | 1 | 1 |
| arms | 0.5 | 10 cm / 0.5 | 0.5 | 0.5 |
| legs | 0.5 | 10 cm / 0.5 | 0.5 | 0.5 |
| hands, feet | 0.25 | | 0.25 | 0.5 |

Head, neck and torso slices kill outright (`sliceForceKill`).

**Worked examples** (NPC, bare flesh, linear interpolation of the 2-key curves assumed) [derived]:
- Tier-2 longsword cut at 8 m/s on the torso: impact 1.33 × 0.9 = 1.2, plus 15 on entering = **~16** (a third of his 50 HP). Same cut on the neck: 15 × 32 = **480, dead**.
- Tier-2 longsword thrust at 5 m/s into the chest: impact 3.25, then 5 × 2 = 10 once it is 10 cm deep. Into the head: 3.25 × 3, then 5 × 32 = **160 at 4 cm, dead**.
- Tier-2 1H mace at 8 m/s: 2.33 × 2.4 = **5.6** torso, **16.8** head (×3); on plate ×0.66 and only above 4 m/s.
- Punch at 6 m/s: 1.89 × 0.5 = **0.9** torso, 2.8 head. Punches do almost nothing in B&S except push levels.

**Half Sword difference** [HS]: damage is armour-cut/blunt over impulse or velocity × rigidity × cutting rate, then `Get Damage` spreads it over health, consciousness, bleeding, limb health and tonus; weapon mass and the arm behind it matter through the impulse. That is why our `hit_assist` and `stab_assist` exist. B&S never needed them.

## 4. Penetration and lodging

`Damager.Penetrate` (Damager.cs:1245-1385), `UpdatePenetration` (:256-390), `RefreshJointDrive` (:413-480), `UnPenetrate` (:1404-1490) [code], values [json]:

- **When:** the hit passes `CheckPenetration` (damager allows it, the material modifier allows it, the damager has a depth > 0, not an NPC's held weapon unless conditions say so, impact ≥ penetration min speed, target collider group allows it). Also by **pressure** (slow push, `TryHitByPressure` :1000-1078): contact push "force" (collision impulse ÷ weapon mass per step) ≥ `penetrationPressureForceCurve(relative speed)` × material multiplier. Sword pierce curve 0 m/s → 5, 0.5 → 1.5, 1 → 0.25: a still press needs a big shove, a 1 m/s push needs very little. Only for player-held weapons. [code+json; the "per step" reading is derived]
- **How:** a ConfigurableJoint from the weapon to the struck body (or a kinematic stand-in for scenery). Sideways locked, twist locked; **along the blade** a linear limit = the damager's prefab depth (pierce) or the material's speed-to-depth curve. Slash on flesh: depth cap 3 mm whatever the speed (the curve ends at 0.003 and clamps), so a "cut" is a shallow lodge that can **slide along the edge** (y free, slide damper 100) and comes out when it slides off the edge's length. Pitch allowed ±30° at the surface, shrinking to 0 at 10 cm deep (`penetrationShortDepth`).
- **Damping along the blade** (the feel of pushing in / pulling out): while held, `penetrationHeldDamperIn` / `Out` (sword pierce 1000 / 500, dagger 500 / 250, warpick 500 / 1500, axe 0 / 10000, sword slash 10000 / 100); free: `penetrationDamper` (100-5000); all × the material's `penetrationDamperMultiplier` (pierce on flesh tier 0 ×3 … tier 4+ ×0.5; chainmail ×3-6). Thrown items use a temp damper (in 0, out 5000) for 0.5 s. No spring: the blade stays where it is put.
- **Collisions** between the weapon and the whole struck ragdoll are ignored while it is in, and the weapon is parented to the creature.
- **Damage on entry:** slash = `penetrationDamage` × part slash multiplier at once; pierce = the same × part deep multiplier once past the part's deep depth; skewer (tip came out the far side) = `penetrationSkewerDamage` × part multiplier. Item damage multipliers apply.
- **Out:** when depth goes ≤ 0 while decreasing, when a slash slides past its length, or for exit-on-max-depth damagers when the tip leaves the body. Releasing the hand does not pull it out; the item stays and dangles. On the player's own release, `PreventPierceSelf` blocks re-penetration for 1 s (see `bs_vs_hsvr_hold.md`).
- **Hand letting go while stuck:** the stuck rule in `bs_vs_hsvr_hold.md` §2 (hand drops when the item is penetrating and the hand is past arm's length).

**Ours** [ours]: the game's own stuck constraint (Constraint_Weapon_Stuck_BP) does the lodge; the mod holds young sticks open, sets wound in/out speed limits (`stab_wound_in_cms` 120, `_out_cms` 60, head ×0.6), yank-out past 35 cm for 0.25 s, cut follow-through 10 cm past, drag the body when stuck. These are speed limits rather than B&S's dampers but serve the same purpose. Missing: the depth buzz (item 2).

## 5. Reactions, stagger, knock-down

- **Push level** = the highest material `pushLevels` entry the impact speed exceeds (strictly), + item skill bonus (Damager.cs:974-998). [code]
- **NPC response** (`BrainModuleHitReaction.TryPush` :268-316, table in `Brain_HumanMedium.json`) [code+json]. Effects: light (0.2) / medium (0.6) / full stagger animation, or Destabilize (ragdoll fall). Selected by push level, body part, and the NPC's state:

| level | any part: idle / combat / attacking / jumping | head or legs: idle / combat / attacking / jumping |
|---|---|---|
| 0 | light / none / none (hand: light) / none | same |
| 1 | medium / light / medium / none | full / medium / full / fall |
| 2 | full / light / medium / fall | fall / full / full / fall |
| 3 | full / medium / full / fall | fall / full / fall / fall |
| 4 | fall / fall / fall / fall | fall |

  A hit while he is already staggered usually escalates (medium → full → fall). A standing-up NPC hit at any level cancels his get-up.
- **Extra shove:** `addForce` is applied **only when the hit creature is not Alive-standing** (Damager.cs:840-843: dead or destabilized), for the damager's duration, on the struck part's region. Live, standing NPCs move only from contact physics and the stagger animation. [code]
- **Ragdoll pins** (prefab defaults, Ragdoll.cs:398-410): spring 1000 / damper 50 / max 1000 N; rotation 800 / 50 / 100 N·m; fallen ×0.5 rotation spring. While attacking, NPC arms get ×2 spring, ×10 max force (`armSpringMultiplier` 2, `armMaxForceMultiplier` 10). NPC ragdoll physics is only on within 5 m of the player (`physicTogglePlayerRadius`). [code+json; pin defaults may be overridden in prefabs]
- **Ours vs Half Sword:** Half Sword uses pain stumble, consciousness and a balance-tilt "Fallen" state [HS]. The mod adds nothing here; no change suggested.

## 6. Parry, block, weapon vs weapon

- **No parry for the player.** The player's brain has only Move, Sightable, Player, Follow, Speak, LookAt (`Brain_Player.json`): no hit reaction, so the player never recoils or staggers. [json]
- **NPC attack hits your weapon** (`BrainModuleMelee.OnWeaponCollision` :837-962): if his weapon meets a non-creature object (a dropped item) he recoils at level 0/1; if it meets an enemy's weapon at > `recoilMinWeaponSpeed` 1 m/s, he stops the attack and recoils (`Parry` animation), with a 10-15% chance to chain a riposte instead (`recoilNothingRiposteChance`), +5-12% per consecutive riposte, and a 1.5 s cooldown before the next recoil (`parryRecoilCooldown`). [code+json]
- **NPC push attacks** (shield bashes, kicks; `attack.pushForce` > 0): if they hit your items, the masses of the hit items are summed (shields ×1.5); above 2.99 kg you are not pushed, below it your locomotion gets the push as a velocity change (BrainModuleMelee.cs:984-1030). [code+json]
- **NPC blocking** (`BrainModuleDefense`): he predicts the threat from your weapon (parry circle radius 0.6 m at +0.35 m, detection 5 m, ±60°) and moves his weapon/shield there by IK (shield 0.75/0.8 weight; one-hand weapon rotation only). When your weapon hits his blocking weapon: if your weapon's AI data `transferPushToBlocker` is set, the push level of your blow (vs a substitute block material) decides a torso stagger; if that makes him fall he **drops his weapon** (`disarmOnBlockDestabilize`). Otherwise he recoils or ripostes (shield: 30% riposte). [code+json]
- **Weapon vs weapon physics:** two weapons are two joint-held rigid bodies; the faster one's damager runs (breakables can take damage). No clash rule beyond the AI above. [code]
- **Ours / Half Sword:** Half Sword AI parries via threat traces and `Parry Rate`; `Just Parried Rate` would halve the attacker's impulse but nothing ever raises it [HS]. Whether a VR block interrupts a Half Sword NPC attack is unknown (item 5).

## 7. Disarm

Covered in item 3 and §6. Player side: B&S never disarms the player by hits. The only player losses are the stuck rule, a hand 6 m from the shoulder, and climb grips (`gripDisableChance` 0.25 on daggers/rapiers only drops a **climbing** grip, RagdollHandClimb.cs:652). [code+json]

Ours: `disarm` (tug of war on an NPC's weapon, grip strength × 3 for 0.25 s). Half Sword's own grip-slip (`Grab Release Rate`, soft release) runs for the player too [HS].

## 8. Free hand, punch, kick, body contact

- **Empty hand** (`PlayerLink`): becomes a dynamic body (gravity off, mass set to 1 kg) jointed to the controller with the hold's springs (5000 N/m, max 3000 N; rotation 1000, max 250 N·m) plus a locomotion velocity correction (130); it collides with NPCs, items and walls and carries the `Punch` damager. When it grabs, it is detached. [code+json; 1 kg derived from the mass multiplier 1/mass]
- **Punch events** (for skills): fist + hand moving > 3 m/s within 20° of pointing direction (RagdollHand.cs:600-623). Damage does not need a fist. [code]
- **Feet:** with foot tracking, the feet carry `KickTracked`; the button kick uses `Kick` (§table). [code+json]
- **Body:** the player's posed body is kinematic on the Avatar layer, and Avatar vs NPC/Ragdoll collisions are switched off while the player is kinematic (PlayerHand.cs:185-193). So only hands, feet and held items touch NPCs. [code]
- **Ours:** fists via the game's fist weapon, slaps, elbow strikes, posed-body contact scaled by closing speed (`player_body_contact_speed`). Broader than B&S.

## 9. Grappling

See item 4 for the rules. Per-part grab data ([json], `Interactable_Human*.json`):

| part | behaviour | lift | forced fall when grabbed during | turns/steps |
|---|---|---|---|---|
| neck | Choke | fall | idle/patrol, full stagger, electrocuted | yes, height follows |
| mouth | Muffle | your hand lets go | | faces you |
| skull | | your hand lets go | | yes |
| torso | | your hand lets go | idle/patrol, full stagger | faces you |
| shoulder | | your hand lets go | | yes |
| legs | | fall | idle/patrol, attack, stagger, full stagger | no |
| wrist | | your hand lets go | electrocuted | steps |
| hand | none | none | | no |

Released with the grabbed part moving > 2 m/s → he falls (`grabThrowMinVelocity`). Grabbed NPCs can't stand up. [code+json]

## 10. Shields, two-handing, polearms, sliding, throwing, haptics, holsters

- **Shields:** handle data `disableHandCollider` true (the hand's own collider is off while holding); `ShieldSweep` (item 6); shield bash damagers `Shield` / `ShieldStrong` (blunt, 100 / 400 accel shove on fallen targets); shield counts ×1.5 against NPC push attacks; NPC shield block via IK. Handles otherwise default (rotation ×1, Slerp). [code+json]
- **Two-handing, polearms, half-swording, sliding:** see `bs_vs_hsvr_hold.md` and `bs_handles_halfswording.md`. Polearms are ordinary long handles; the halberd has dagger-pierce top and bottom spikes, an axe-slash back spike and an `AxeSlash2H` blade; the bec de corbin has a warpick back. [json]
- **Throwing:** `bs_throwing.md`. **Haptics:** `bs_haptics.md` (plus item 2 here). **Holsters:** `bs_holders.md`.
- **Stamina:** none in B&S (`grep stamina|fatigue` finds nothing in ThunderRoad). Focus is magic/slow-motion only. [code]

## Open questions

- Prefab-only values not readable without the base-game bundles: per-weapon `penetrationDepth` / `penetrationLength` of each damager transform, collider shapes, NPC ragdoll pin overrides.
- Whether Unity's item `maxAngularVelocity` is raised project-wide (not set in code; `PhysicBodyForceApplier` only).
- Half Sword side for items 1, 3 and 5 needs a log or a decompile check before building.
