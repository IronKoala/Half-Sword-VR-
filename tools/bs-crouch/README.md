# B&S crouch animation bake (pose_crouch_anim)

Blade & Sorcery's crouch clips are Unity humanoid (muscle) clips. `Bake.cs` turns them into bone
poses for Willie's DriverSkeleton with Unity's own humanoid retargeting, in Unity 2021.3.38f1 (the
version B&S is built with):

1. Builds Willie from `willie-skeleton/skeletons/mesh-refskeleton/Willie_Lopoly.csv` (his reference
   pose is a T-pose with every body bone's rotation identity) and a humanoid Avatar for him
   (`AvatarBuilder.BuildHumanAvatar`).
2. Loads `animations_assets_all.bundle` from your B&S install and takes the clips from the `Human`
   AnimatorController.
3. Plays each clip through a PlayableGraph (foot IK on, as B&S's states have it) at 60 samples a
   second, with and without root motion, and writes every leg bone's position and rotation in
   Willie's component space (UE axes, cm) to `<clip>.csv` / `<clip>.rootmotion.csv`.

The mod reads the `.rootmotion.csv` files, renamed `<clip>.csv`, from `Mods\HalfSwordVR\bs-crouch\`.
**The baked files come from B&S's assets: own use only, never commit or share them.**

Run (batch mode, any empty Unity project with `Bake.cs` in `Assets/Editor`):

```
Unity.exe -batchmode -nographics -projectPath <project> -executeMethod Bake.Run
  -bundle "<B&S>\BladeAndSorcery_Data\StreamingAssets\Default\animations_assets_all.bundle"
  -skel "<willie-skeleton>\skeletons\mesh-refskeleton\Willie_Lopoly.csv" -out <dir>
  -clips Crouch_WalkFwd_new,Crouch_WalkBwd_new,Crouch_WalkLt_new,Crouch_WalkRt_new,Crouch_WalkLt45_new,Crouch_WalkRt45_new,Crouch_WalkLt135_new,Crouch_WalkRt135_new,Crouch_Turn90L_new,Crouch_Turn90R_new,CrouchLoop_new
  -logFile <log>
```

Blend trees copied from B&S's `Human` controller (Dual Wield / Unarmed states): moving = freeform
directional 2D over the 8 Crouch_Walk clips by local velocity (Strafe/Speed); still = 1D over
Crouch_Turn90L / CrouchLoop / Crouch_Turn90R by Turn (deg/s x 0.007, thresholds -1.58 / 0 / 1.58).
