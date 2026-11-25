// Bakes B&S humanoid (muscle) clips onto Willie's DriverSkeleton (Willie_Lopoly) with Unity's own
// humanoid retargeting: a Willie avatar is built from his reference pose (a T-pose, every body bone's
// rotation identity), each clip is played on it through a PlayableGraph (foot IK on, as B&S's states
// have it), and every frame's bones are written out in Willie's component space (UE axes, cm).
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using UnityEditor;
using UnityEngine;
using UnityEngine.Animations;
using UnityEngine.Playables;

public static class Bake
{
    static string Arg(string name)
    {
        var a = Environment.GetCommandLineArgs();
        for (int i = 0; i < a.Length - 1; ++i) if (a[i] == name) return a[i + 1];
        return null;
    }
    static readonly CultureInfo C = CultureInfo.InvariantCulture;
    // UE (x = his left, y = forward, z = up, in Willie's component) <-> Unity (x right, y up, z forward)
    static Vector3 ToUnity(double x, double y, double z) => new Vector3((float)-x, (float)z, (float)y) * 0.01f;
    static Vector3 ToUE(Vector3 u) => new Vector3(-u.x, u.z, u.y) * 100f;
    static Quaternion QToUnity(double x, double y, double z, double w) => new Quaternion((float)-x, (float)z, (float)y, (float)w);
    static Quaternion QToUE(Quaternion q) => new Quaternion(-q.x, q.z, q.y, q.w);

    static readonly string[] Out = { "pelvis", "spine_01", "thigh_l", "calf_l", "foot_l", "ball_l", "thigh_r", "calf_r", "foot_r", "ball_r" };

    public static void Run()
    {
        try { RunInner(); EditorApplication.Exit(0); }
        catch (Exception e) { Debug.LogError("BAKE FAILED: " + e); EditorApplication.Exit(1); }
    }

    static string F(float v) => v.ToString("R", C);

    static void RunInner()
    {
        string bundle = Arg("-bundle"), skel = Arg("-skel"), outDir = Arg("-out"), names = Arg("-clips");
        Directory.CreateDirectory(outDir);
        var log = new StringBuilder();
        // ---- Willie ----
        var root = new GameObject("Willie");
        var byName = new Dictionary<string, Transform>();
        var skeleton = new List<SkeletonBone>();
        skeleton.Add(new SkeletonBone { name = "Willie", position = Vector3.zero, rotation = Quaternion.identity, scale = Vector3.one });
        foreach (var line in File.ReadAllLines(skel).Skip(1))
        {
            var f = line.Split(',');
            string name = f[1];
            int parent = int.Parse(f[2], C);
            var go = new GameObject(name);
            go.transform.SetParent(parent < 0 ? root.transform : byName[f[3]], false);
            Func<int, double> D = i => double.Parse(f[i], C);
            go.transform.localPosition = ToUnity(D(11), D(12), D(13));
            go.transform.localRotation = QToUnity(D(4), D(5), D(6), D(7));
            byName[name] = go.transform;
            skeleton.Add(new SkeletonBone { name = name, position = go.transform.localPosition, rotation = go.transform.localRotation, scale = Vector3.one });
        }
        var map = new Dictionary<string, string>
        {
            {"Hips","pelvis"},{"Spine","spine_02"},{"Chest","spine_03"},{"UpperChest","spine_05"},{"Neck","neck_01"},{"Head","head"},
            {"LeftShoulder","clavicle_l"},{"LeftUpperArm","upperarm_l"},{"LeftLowerArm","lowerarm_l"},{"LeftHand","hand_l"},
            {"RightShoulder","clavicle_r"},{"RightUpperArm","upperarm_r"},{"RightLowerArm","lowerarm_r"},{"RightHand","hand_r"},
            {"LeftUpperLeg","thigh_l"},{"LeftLowerLeg","calf_l"},{"LeftFoot","foot_l"},{"LeftToes","ball_l"},
            {"RightUpperLeg","thigh_r"},{"RightLowerLeg","calf_r"},{"RightFoot","foot_r"},{"RightToes","ball_r"},
        };
        var hd = new HumanDescription
        {
            human = map.Select(kv => new HumanBone { humanName = HumanTrait.BoneName.First(b => b.Replace(" ", "") == kv.Key), boneName = kv.Value, limit = new HumanLimit { useDefaultValues = true } }).ToArray(),
            skeleton = skeleton.ToArray(),
            upperArmTwist = 0.5f, lowerArmTwist = 0.5f, upperLegTwist = 0.5f, lowerLegTwist = 0.5f, armStretch = 0.05f, legStretch = 0.05f, feetSpacing = 0f, hasTranslationDoF = false,
        };
        var avatar = AvatarBuilder.BuildHumanAvatar(root, hd);
        log.AppendLine("avatar valid=" + avatar.isValid + " human=" + avatar.isHuman);
        if (!avatar.isValid || !avatar.isHuman) throw new Exception("avatar not valid");
        var anim = root.AddComponent<Animator>();
        anim.avatar = avatar;
        anim.applyRootMotion = false;
        anim.cullingMode = AnimatorCullingMode.AlwaysAnimate;
        log.AppendLine("humanScale=" + F(anim.humanScale));

        var rest = new StringBuilder("bone,x,y,z\n");
        foreach (var b in Out) { var p = ToUE(root.transform.InverseTransformPoint(byName[b].position)); rest.AppendLine(b + "," + F(p.x) + "," + F(p.y) + "," + F(p.z)); }
        File.WriteAllText(Path.Combine(outDir, "rest.csv"), rest.ToString());

        // ---- clips ----
        var ab = AssetBundle.LoadFromFile(bundle);
        if (!ab) throw new Exception("bundle did not load");
        var wanted = new HashSet<string>(names.Trim('"').Split(';', ',').Select(s => s.Trim()));
        var all = ab.LoadAllAssets<AnimationClip>();
        var allNames = ab.GetAllAssetNames();
        log.AppendLine("wanted: " + string.Join("|", wanted) + "; bundle assets " + allNames.Length + ", clips loaded " + all.Length + "; first: " + string.Join("|", allNames.Take(5)));
        var clips = all.Where(c => wanted.Contains(c.name)).ToList();
        var everything = ab.LoadAllAssets();
        log.AppendLine("all objects " + everything.Length + "; non-clips: " + string.Join("|", everything.Where(o => !(o is AnimationClip)).Select(o => o.GetType().Name + ":" + o.name)));
        foreach (var c in Resources.FindObjectsOfTypeAll<AnimationClip>())
            if (c && wanted.Contains(c.name) && !clips.Any(k => k.name == c.name)) clips.Add(c);
        foreach (var rac in Resources.FindObjectsOfTypeAll<RuntimeAnimatorController>())
        {
            log.AppendLine("controller " + rac.name + " clips " + rac.animationClips.Length);
            foreach (var c in rac.animationClips)
                if (c && wanted.Contains(c.name) && !clips.Any(k => k.name == c.name)) clips.Add(c);
        }
        if (clips.Count == 0)
            foreach (var n in allNames.Where(n => n.Contains("crouch")))
            {
                var c = ab.LoadAsset<AnimationClip>(n);
                log.AppendLine("  by path " + n + " -> " + (c ? c.name : "null"));
                if (c && wanted.Contains(c.name)) clips.Add(c);
            }
        log.AppendLine("clips found " + clips.Count + " of " + wanted.Count);
        foreach (var clip in clips)
        {
            log.AppendLine(clip.name + ": length " + F(clip.length) + " fps " + F(clip.frameRate) + " loop " + clip.isLooping + " human " + clip.isHumanMotion
                + " avgSpeed(UE cm/s) " + ToUE(clip.averageSpeed) + " avgAngular " + F(clip.averageAngularSpeed) + " apparent " + F(clip.apparentSpeed));
            foreach (bool rootMotion in new[] { false, true })
            {
                root.transform.position = Vector3.zero; root.transform.rotation = Quaternion.identity;
                anim.applyRootMotion = rootMotion;
                anim.Rebind();
                var graph = PlayableGraph.Create("bake");
                graph.SetTimeUpdateMode(DirectorUpdateMode.Manual);
                var output = AnimationPlayableOutput.Create(graph, "out", anim);
                var cp = AnimationClipPlayable.Create(graph, clip);
                cp.SetApplyFootIK(true);
                output.SetSourcePlayable(cp);
                graph.Play();
                int frames = Mathf.Max(2, Mathf.RoundToInt(clip.length * 60f)); // 60 samples a second
                var sb = new StringBuilder("t,root_x,root_y,root_z,root_yaw");
                foreach (var b in Out) sb.Append("," + b + "_x," + b + "_y," + b + "_z," + b + "_qx," + b + "_qy," + b + "_qz," + b + "_qw");
                sb.AppendLine();
                float dt = clip.length / frames;
                cp.SetTime(0); cp.SetTime(0);
                graph.Evaluate(0f);
                for (int i = 0; i <= frames; ++i)
                {
                    if (i > 0) graph.Evaluate(dt);
                    var rt = root.transform;
                    var rp = ToUE(rt.position);
                    sb.Append(F(i * dt) + "," + F(rp.x) + "," + F(rp.y) + "," + F(rp.z) + "," + F(rt.eulerAngles.y));
                    foreach (var b in Out)
                    {
                        var t = byName[b];
                        var p = ToUE(rt.InverseTransformPoint(t.position));
                        var q = QToUE(Quaternion.Inverse(rt.rotation) * t.rotation);
                        sb.Append("," + F(p.x) + "," + F(p.y) + "," + F(p.z) + "," + F(q.x) + "," + F(q.y) + "," + F(q.z) + "," + F(q.w));
                    }
                    sb.AppendLine();
                }
                graph.Destroy();
                File.WriteAllText(Path.Combine(outDir, clip.name + (rootMotion ? ".rootmotion.csv" : ".csv")), sb.ToString());
            }
        }
        ab.Unload(true);
        File.WriteAllText(Path.Combine(outDir, "bake-log.txt"), log.ToString());
    }
}
