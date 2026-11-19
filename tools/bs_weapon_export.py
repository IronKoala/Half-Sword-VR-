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
import math
import sys
import warnings

import numpy as np
import UnityPy
from scipy.spatial import ConvexHull
from UnityPy.helpers.MeshHelper import MeshHandler

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

with open(f"{out_dir}/{prefab_name}.obj", "w", newline="\n") as f:
    f.write(f"# {prefab_name} exported by tools/bs_weapon_export.py; Unreal axes, cm, weapon frame\n")
    for p in frame_pos:
        f.write("v %.4f %.4f %.4f\n" % tuple(p))
    for n in frame_nrm:
        f.write("vn %.5f %.5f %.5f\n" % tuple(n))
    for uv in uvs:
        f.write("vt %.5f %.5f\n" % (uv[0], uv[1]))
    for a, b, c in tris:
        # Unity's front face is clockwise as seen in its left-handed space, which is numerically
        # outward = cross(b - a, c - a): the same as an OBJ counter-clockwise face. Keep the order.
        f.write(f"f {a+1}/{a+1}/{a+1} {b+1}/{b+1}/{b+1} {c+1}/{c+1}/{c+1}\n")

# ---- zones and collision pieces --------------------------------------------------------------
z = frame_pos[:, 2]
handle_bottom = -main["axisLength"] * 100.0
# The guard: near the handle's top end, the run of slices much wider than the grip.
def width_at(lo, hi):
    sel = frame_pos[(z >= lo) & (z < hi)]
    return float(np.ptp(sel[:, 0])) if len(sel) else 0.0


grip_width = float(np.median([w for w in (width_at(s, s + 1) for s in np.arange(handle_bottom, -2, 1)) if w > 0]))
bins = [(s, width_at(s, s + 0.25)) for s in np.arange(-5.0, 20.0, 0.25)]
wide = [s for s, w in bins if w > 2.0 * grip_width]
if wide:
    guard_bottom, guard_top = wide[0], wide[0] + 0.25
    for s in wide:
        if s <= guard_top + 0.5:
            guard_top = s + 0.25
else:
    guard_bottom = guard_top = 0.0
# Pieces: pommel below the handle, the grip, the guard, then the blade in slices so curves are kept.
pieces = []


edges = set()
for a, b, c in tris:
    for u, w in ((a, b), (b, c), (c, a)):
        edges.add((min(u, w), max(u, w)))
edges = np.array(sorted(edges))


def plane_points(c):
    """Where the mesh's edges cross the plane z = c (so a slice keeps its full outline even when the
    model has no vertices near its ends)."""
    za, zb = z[edges[:, 0]], z[edges[:, 1]]
    cross = (za - c) * (zb - c) < 0
    pa, pb = frame_pos[edges[cross, 0]], frame_pos[edges[cross, 1]]
    t = ((c - za[cross]) / (zb[cross] - za[cross]))[:, None]
    return pa + (pb - pa) * t


def add_piece(slot, lo, hi):
    sel = np.vstack([frame_pos[(z >= lo) & (z <= hi)], plane_points(lo), plane_points(hi)])
    if len(sel) < 4:
        return
    try:
        hull = ConvexHull(sel)
    except Exception:
        return
    pieces.append((slot, sel[hull.vertices], hull))


add_piece("Pommel", z.min(), handle_bottom)
add_piece("Grip", handle_bottom, guard_bottom)
add_piece("Guard", guard_bottom, guard_top)
top = z.max()
n_slices = max(1, int(math.ceil((top - guard_top) / 15.0)))
step = (top - guard_top) / n_slices
for i in range(n_slices):
    add_piece("Head", guard_top + i * step, guard_top + (i + 1) * step)

with open(f"{out_dir}/{prefab_name}_collision.obj", "w", newline="\n") as f:
    f.write(f"# {prefab_name} convex collision pieces (UCX_<slot>_<n>)\n")
    count = {}
    vbase = 0
    for slot, pts, hull in pieces:
        count[slot] = count.get(slot, 0) + 1
        f.write(f"g UCX_{slot}_{count[slot]}\n")
        index = {}
        for k, vi in enumerate(hull.vertices):
            index[vi] = vbase + k + 1
            f.write("v %.4f %.4f %.4f\n" % tuple(hull.points[vi]))
        centre = hull.points[hull.vertices].mean(axis=0)
        for s in hull.simplices:
            a, b, c = hull.points[s[0]], hull.points[s[1]], hull.points[s[2]]
            if np.dot(np.cross(b - a, c - a), a - centre) < 0:
                s = (s[0], s[2], s[1])
            f.write(f"f {index[s[0]]} {index[s[1]]} {index[s[2]]}\n")
        vbase += len(hull.vertices)

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
with open(f"{out_dir}/{prefab_name}.txt", "w", newline="\n") as f:
    f.write(f"; {prefab_name}: written by tools/bs_weapon_export.py, centimetres in the weapon frame\n")
    f.write(f"mass_kg={out['mass_kg']}\n")
    for k, v in out["zones_cm"].items():
        f.write(f"{k}={v}\n")
    if out["centre_of_mass_cm"]:
        f.write(f"centre_of_mass_z={out['centre_of_mass_cm'][2]}\n")
    for d in out["damagers"]:
        f.write(f"damager_{d['name'].lower()}_z={d['position_cm'][2]}\n")
