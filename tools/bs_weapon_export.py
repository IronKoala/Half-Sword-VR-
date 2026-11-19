"""Exports one Blade & Sorcery weapon (for example from the Medieval MegaPack) into the files the
HalfSwordCustomWeapon mod reads. The user supplies the mod; nothing from it ships with this repo.

    python -I tools/bs_weapon_export.py <bundle> <item json> <prefab name> <out folder>

Example:
    python -I tools/bs_weapon_export.py "Medieval MegaPack/mmp_base_groups_assets_windowsandroidmmplongsword.bundle"
        "Medieval MegaPack/Items/02 Blade/Item_Weapon_LongSwordA.json" LongSwordA out/

Writes into <out folder>:
  <prefab>.obj            the visible model
  <prefab>_collision.obj  convex collision pieces, one OBJ group per piece, named UCX_<slot>_<n>
  <prefab>.json           mass, handles, damage points, zones

Weapon frame (Unreal axes, centimetres):
  origin  = top end of the main handle (the guard end), on the handle axis
  +Z      = along the handle towards the blade (the main Handle's +Y in B&S)
  +X      = the blade's wide direction (edge), +Y = its thin direction
Triangles are written counter-clockwise from outside (the mod reverses them for Unreal).

Needs: pip install UnityPy numpy scipy
"""
import json
import os
import sys
import warnings

import numpy as np
import UnityPy
from UnityPy.helpers.MeshHelper import MeshHandler

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # python -I leaves the script folder out
import weapon_parts as wp  # noqa: E402

warnings.filterwarnings("ignore")
# B&S bundles have their Unity version stripped; the type trees inside are what matter.
UnityPy.config.FALLBACK_UNITY_VERSION = "2021.3.38f1"

bundle_path, item_path, prefab_name, out_dir = sys.argv[1:5]
env = UnityPy.load(bundle_path)
objs = {o.path_id: o for o in env.objects}
tree = {}
for pid, o in objs.items():
    if o.type.name in ("Transform", "GameObject", "MeshFilter", "MeshRenderer", "BoxCollider", "CapsuleCollider",
                       "MeshCollider", "SphereCollider", "MonoBehaviour", "Rigidbody"):
        tree[pid] = o.read_typetree()


def script_name(mb):
    s = mb["m_Script"]["m_PathID"]
    return objs[s].read().m_ClassName if s in objs else "?"


def go_name(go_pid):
    return tree[go_pid]["m_Name"]


def components(go_pid):
    return [c["component"]["m_PathID"] for c in tree[go_pid]["m_Component"]]


def transform_of(go_pid):
    for c in components(go_pid):
        if objs[c].type.name == "Transform":
            return c
    return None


# ---- the prefab's transforms ------------------------------------------------------------------
root = None
for pid, t in tree.items():
    if objs[pid].type.name == "Transform" and t["m_Father"]["m_PathID"] == 0 \
            and go_name(t["m_GameObject"]["m_PathID"]) == prefab_name:
        root = pid
if root is None:
    sys.exit(f"prefab {prefab_name} not found in bundle")


def quat_matrix(q):
    x, y, z, w = q["x"], q["y"], q["z"], q["w"]
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def local_matrix(t):
    m = np.eye(4)
    s = t["m_LocalScale"]
    m[:3, :3] = quat_matrix(t["m_LocalRotation"]) @ np.diag([s["x"], s["y"], s["z"]])
    p = t["m_LocalPosition"]
    m[:3, 3] = [p["x"], p["y"], p["z"]]
    return m


world = {}      # transform pid -> 4x4 matrix in prefab space (metres, Unity axes)
active = {}     # transform pid -> active in hierarchy
nodes = []      # (transform pid, game object pid)


def walk(tpid, parent, parent_active):
    t = tree[tpid]
    go = t["m_GameObject"]["m_PathID"]
    world[tpid] = parent @ local_matrix(t)
    active[tpid] = parent_active and bool(tree[go]["m_IsActive"])
    nodes.append((tpid, go))
    for ch in t["m_Children"]:
        walk(ch["m_PathID"], world[tpid], active[tpid])


walk(root, np.eye(4), True)

# ---- the main handle defines the frame ----------------------------------------------------------
item_json = json.load(open(item_path, encoding="utf-8-sig"))
handles = []
damagers = []
colliders = []
mass_center = None
for tpid, go in nodes:
    name = go_name(go)
    if name == "CenterMass":
        mass_center = world[tpid][:3, 3]
    for c in components(go):
        kind = objs[c].type.name
        if kind == "MonoBehaviour":
            sn = script_name(tree[c])
            if sn == "Handle" and active[tpid]:
                handles.append({"name": name, "m": world[tpid], "axisLength": tree[c]["axisLength"]})
            elif sn == "Damager" and active[tpid]:
                damagers.append({"name": name, "m": world[tpid], "direction": tree[c]["direction"],
                                 "penetrationLength": tree[c]["penetrationLength"],
                                 "penetrationDepth": tree[c]["penetrationDepth"]})
        elif kind in ("BoxCollider", "CapsuleCollider", "SphereCollider", "MeshCollider") and active[tpid] \
                and tree[c].get("m_Enabled", 1) and not tree[c].get("m_IsTrigger", 0):
            colliders.append({"name": name, "kind": kind, "m": world[tpid], "data": tree[c]})

interactables = {i["transformName"]: i["interactableId"] for i in item_json.get("Interactables") or []}
main = next((h for h in handles if h["name"] == "Handle"), None) or max(handles, key=lambda h: h["axisLength"])
axis = main["m"][:3, :3] @ np.array([0.0, 1.0, 0.0])
axis /= np.linalg.norm(axis)
handle_centre = main["m"][:3, 3]
half = main["axisLength"] / 2.0

# ---- the visible mesh ----------------------------------------------------------------------------
positions, normals, uvs, tris = [], [], [], []
mesh_names = []
material_ids = []  # the first material of each drawn mesh
for tpid, go in nodes:
    if not active[tpid]:
        continue
    mf = mr = None
    for c in components(go):
        kind = objs[c].type.name
        if kind == "MeshFilter":
            mf = c
        elif kind == "MeshRenderer" and tree[c].get("m_Enabled", 1):
            mr = c
    if mf is None or mr is None:
        continue
    mesh_pid = tree[mf]["m_Mesh"]["m_PathID"]
    if mesh_pid not in objs:
        continue
    mesh = objs[mesh_pid].read()
    mats = tree[mr].get("m_Materials", [])
    if len(mats) > 1:
        print(f"warning: {mesh.m_Name} has {len(mats)} materials; only the first one's textures are exported")
    if mats and mats[0]["m_PathID"] in objs and mats[0]["m_PathID"] not in material_ids:
        material_ids.append(mats[0]["m_PathID"])
    h = MeshHandler(mesh)
    h.process()
    m = world[tpid]
    rot = np.linalg.inv(m[:3, :3]).T
    base = len(positions)
    for i, v in enumerate(h.m_Vertices):
        positions.append(m[:3, :3] @ np.array(v) + m[:3, 3])
        n = h.m_Normals[i] if h.m_Normals else (0, 0, 1)
        n = rot @ np.array(n)
        normals.append(n / (np.linalg.norm(n) or 1))
        uvs.append(h.m_UV0[i] if h.m_UV0 else (0, 0))
    for sub in h.get_triangles():
        for t in sub:
            tris.append((base + t[0], base + t[1], base + t[2]))
    mesh_names.append(mesh.m_Name)
positions = np.array(positions)

# Edge direction: the widest spread of the blade's vertices across the axis.
rel = positions - handle_centre
along = rel @ axis
blade = rel[along > half + 0.05]
across = blade - np.outer(blade @ axis, axis)
_, _, vt = np.linalg.svd(across - across.mean(axis=0), full_matrices=False)
edge = vt[0] - (vt[0] @ axis) * axis
edge /= np.linalg.norm(edge)
thin = np.cross(axis, edge)  # det[edge, thin, axis] = +1, so the triangle winding is kept

origin = handle_centre + axis * half


def to_frame(p):
    d = np.asarray(p) - origin
    return np.array([d @ edge, d @ thin, d @ axis]) * 100.0


def dir_to_frame(v):
    return np.array([v @ edge, v @ thin, v @ axis])


frame_pos = np.array([to_frame(p) for p in positions])
frame_nrm = np.array([dir_to_frame(n) for n in normals])

# Unity's front face is clockwise as seen in its left-handed space, which is numerically
# outward = cross(b - a, c - a): the same as an OBJ counter-clockwise face, so the order is kept.
wp.write_visual_obj(f"{out_dir}/{prefab_name}.obj", f"{prefab_name} exported by tools/bs_weapon_export.py",
                    frame_pos, frame_nrm, uvs, tris)

# ---- zones and collision pieces --------------------------------------------------------------
z = frame_pos[:, 2]
handle_bottom = -main["axisLength"] * 100.0
guard_bottom, guard_top, grip_width = wp.find_guard(frame_pos, handle_bottom)
pieces = wp.build_pieces(frame_pos, tris, handle_bottom, guard_bottom, guard_top)
top = z.max()
wp.write_collision(f"{out_dir}/{prefab_name}_collision.obj", prefab_name, pieces)

# ---- data -----------------------------------------------------------------------------------------
damager_ids = {d["transformName"]: d["damagerID"] for d in item_json.get("damagers") or []}
out = {
    "source": {"bundle": bundle_path.replace("\\", "/").split("/")[-1], "item": item_json.get("id"),
               "prefab": prefab_name, "meshes": mesh_names},
    "mass_kg": item_json.get("mass"),
    "centre_of_mass_cm": to_frame(mass_center).round(2).tolist() if mass_center is not None else None,
    "length_cm": round(float(top - z.min()), 2),
    "zones_cm": {"pommel_end": round(float(z.min()), 2), "handle_bottom": round(handle_bottom, 2),
                 "handle_top": 0.0, "guard_bottom": round(float(guard_bottom), 2), "guard_top": round(float(guard_top), 2), "tip": round(float(top), 2)},
    "handles": [],
    "damagers": [],
    "pieces": [{"slot": s, "points": len(p)} for s, p, _ in pieces],
}
for h in handles:
    a = h["m"][:3, :3] @ np.array([0.0, 1.0, 0.0])
    a /= np.linalg.norm(a)
    c = to_frame(h["m"][:3, 3])
    out["handles"].append({"name": h["name"], "interactable": interactables.get(h["name"], ""),
                           "centre_cm": c.round(2).tolist(), "axis": dir_to_frame(a).round(4).tolist(),
                           "length_cm": round(h["axisLength"] * 100.0, 2)})
for d in damagers:
    fwd = d["m"][:3, :3] @ np.array([0.0, 0.0, 1.0])
    up = d["m"][:3, :3] @ np.array([0.0, 1.0, 0.0])
    out["damagers"].append({"name": d["name"], "id": damager_ids.get(d["name"], ""),
                            "position_cm": to_frame(d["m"][:3, 3]).round(2).tolist(),
                            "forward": dir_to_frame(fwd / np.linalg.norm(fwd)).round(4).tolist(),
                            "up": dir_to_frame(up / np.linalg.norm(up)).round(4).tolist(),
                            "direction": d["direction"],
                            "penetration_length_cm": round(d["penetrationLength"] * 100.0, 2),
                            "penetration_depth_cm": round(d["penetrationDepth"] * 100.0, 2)})
json.dump(out, open(f"{out_dir}/{prefab_name}.json", "w"), indent=2)
print(json.dumps(out, indent=2))
print(f"{len(frame_pos)} vertices, {len(tris)} triangles, {len(pieces)} collision pieces")

# The same numbers as flat key=value lines for the mod (it has no JSON reader).
numbers = {"mass_kg": out["mass_kg"], **out["zones_cm"]}
if out["centre_of_mass_cm"]:
    numbers["centre_of_mass_z"] = out["centre_of_mass_cm"][2]
for d in out["damagers"]:
    numbers[f"damager_{d['name'].lower()}_z"] = d["position_cm"][2]
wp.write_numbers(f"{out_dir}/{prefab_name}.txt", f"{prefab_name}: written by tools/bs_weapon_export.py", numbers)

# ---- textures --------------------------------------------------------------------------------
# The model's own maps, converted to what Unreal expects:
#   <prefab>_BaseColor.png  colour (sRGB)
#   <prefab>_Normal.png     tangent-space normal, DirectX style (green = -Y): Unity stores normal maps
#                           as "DXT5nm" (x in alpha, y in green) with OpenGL-style +Y, checked below.
#   <prefab>_ORM.png        R = ambient occlusion, G = roughness (1 - smoothness), B = metallic, from
#                           B&S's MOES map (R metallic, G occlusion, B emission mask, A smoothness).
# The normal and ORM maps are written gamma-encoded (linear -> sRGB) because the mod loads every file as
# an sRGB texture; the GPU's sRGB decode then gives back the linear values.
def save(a, name, encode):
    wp.save_rgb(f"{out_dir}/{prefab_name}_{name}.png", a, encode)


if len(material_ids) > 1:
    print(f"warning: {len(material_ids)} different materials; textures come from the first only")
if material_ids:
    mat = objs[material_ids[0]].read_typetree()
    tex = {k: v["m_Texture"]["m_PathID"] for k, v in mat["m_SavedProperties"]["m_TexEnvs"]}

    def image(key):
        pid = tex.get(key, 0)
        if not pid or pid not in objs:
            return None
        return np.asarray(objs[pid].read().image.convert("RGBA")).astype(np.float64) / 255.0

    print(f"material {mat['m_Name']}")
    base = image("_BaseMap")
    if base is None:
        base = image("_MainTex")
    if base is not None:
        save(base[:, :, :3], "BaseColor", False)
    nm = image("_BumpMap")
    if nm is not None:
        if nm[:, :, 0].std() < 0.01 and nm[:, :, 0].mean() > 0.95:  # DXT5nm: red unused (all 1)
            x, y = nm[:, :, 3] * 2 - 1, nm[:, :, 1] * 2 - 1
        else:
            x, y = nm[:, :, 0] * 2 - 1, nm[:, :, 1] * 2 - 1
        wp.save_normal_from_opengl_xy(f"{out_dir}/{prefab_name}_Normal.png", x, y)
    moes = image("_MetallicGlossMap")
    if moes is not None:
        wp.save_orm(f"{out_dir}/{prefab_name}_ORM.png", moes[:, :, 1], 1 - moes[:, :, 3], moes[:, :, 0])
