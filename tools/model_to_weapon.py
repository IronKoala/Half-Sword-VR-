"""Converts a 3D sword model (OBJ, glTF/GLB, and other formats trimesh reads) into the files the
HalfSwordCustomWeapon mod reads. The user supplies the model; nothing ships with this repo.

    python -I tools/model_to_weapon.py <model> <out folder> [options]

Options:
  --name NAME         base name of the written files (default: the model's file name)
  --units U           m, cm, mm or in. glTF/GLB are metres by the glTF spec; for other formats it
                      is required, because a file's numbers do not say their unit.
  --mass-kg KG        the weapon's mass. Without it the mass is estimated from the collision volume
                      (steel 7.85 g/cm3 for blade, guard and pommel, 1.0 g/cm3 for the grip) and the
                      estimate is marked as such.
  --flip              the blade points the other way from what was detected
  --handle-bottom-cm  where the grip ends and the pommel starts, in cm below the guard
  --metallic M        for models without PBR maps: metallic (default 1) ...
  --roughness R       ... and roughness (default 0.35) written into the ORM map

What it finds (all printed, so a wrong guess shows):
  the long axis (principal axis of the vertices), the guard (the widest run of slices), which side
  is the blade (the longer side of the guard), the grip and pommel (the pommel is where the handle
  side widens again near its end), and the edge direction (widest spread of the blade's cross-
  sections). Swords and other guard-plus-blade weapons only, for now.

Writes into <out folder>: <name>.obj, <name>_collision.obj, <name>.txt, <name>.json and, when the
model has a material, <name>_BaseColor.png, <name>_Normal.png, <name>_ORM.png.

Needs: pip install trimesh numpy scipy pillow
"""
import argparse
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # python -I leaves the script folder out
import weapon_parts as wp  # noqa: E402

UNITS_CM = {"m": 100.0, "cm": 1.0, "mm": 0.1, "in": 2.54}
STEEL, GRIP = 7.85, 1.0  # g/cm3; the grip figure stands for wood and leather (an estimate)

ap = argparse.ArgumentParser()
ap.add_argument("model")
ap.add_argument("out")
ap.add_argument("--name")
ap.add_argument("--units", choices=sorted(UNITS_CM))
ap.add_argument("--mass-kg", type=float)
ap.add_argument("--flip", action="store_true")
ap.add_argument("--handle-bottom-cm", type=float)
ap.add_argument("--metallic", type=float, default=1.0)
ap.add_argument("--roughness", type=float, default=0.35)
args = ap.parse_args()

import trimesh  # noqa: E402

name = args.name or os.path.splitext(os.path.basename(args.model))[0]
ext = os.path.splitext(args.model)[1].lower()
units = args.units or ("m" if ext in (".gltf", ".glb") else None)
if units is None:
    sys.exit(f"--units is needed for {ext} files (m, cm, mm or in)")
os.makedirs(args.out, exist_ok=True)

# ---- the model ------------------------------------------------------------------------------------
# process=False keeps the file's vertices as they are (merging would join UV seams). trimesh gives
# UVs in OBJ convention (v up; it flips glTF's on load) and keeps the files' counter-clockwise fronts.
scene = trimesh.load(args.model, force="scene", process=False)
meshes = []
for node in scene.graph.nodes_geometry:
    xf, geom_name = scene.graph[node]
    g = scene.geometry[geom_name]
    if not isinstance(g, trimesh.Trimesh) or len(g.faces) == 0:
        continue
    g = g.copy()
    g.apply_transform(xf)
    meshes.append(g)
if not meshes:
    sys.exit("no triangle meshes in the model")
positions, normals, uvs, tris = [], [], [], []
for g in meshes:
    base = sum(len(p) for p in positions)
    positions.append(np.asarray(g.vertices, dtype=np.float64))
    normals.append(np.asarray(g.vertex_normals, dtype=np.float64))
    uv = getattr(g.visual, "uv", None)
    uvs.append(np.asarray(uv, dtype=np.float64) if uv is not None and len(uv) == len(g.vertices)
               else np.zeros((len(g.vertices), 2)))
    tris.append(np.asarray(g.faces) + base)
positions = np.vstack(positions) * UNITS_CM[units]
normals = np.vstack(normals)
uvs = np.vstack(uvs)
tris = np.vstack(tris)
print(f"{name}: {len(meshes)} meshes, {len(positions)} vertices, {len(tris)} triangles, units {units}")

# ---- axis, guard, blade side ----------------------------------------------------------------------
centre = positions.mean(axis=0)
_, _, vt = np.linalg.svd(positions - centre, full_matrices=False)
axis, side_a, side_b = vt[0], vt[1], vt[2]
t = (positions - centre) @ axis


def slice_widths(t, step):
    lo, hi = t.min(), t.max()
    out = []
    for s in np.arange(lo, hi, step):
        sel = positions[(t >= s) & (t < s + step)] - centre
        if len(sel) == 0:
            out.append((s, 0.0))
            continue
        out.append((s, float(max(np.ptp(sel @ side_a), np.ptp(sel @ side_b)))))
    return out


step = max(0.25, float(np.ptp(t)) / 400.0)
widths = slice_widths(t, step)
w_max = max(w for _, w in widths)
peak = max(range(len(widths)), key=lambda i: widths[i][1])
# The guard: the run of slices around the widest one that stay above 60 % of its width.
i0 = i1 = peak
while i0 > 0 and widths[i0 - 1][1] > 0.6 * w_max:
    i0 -= 1
while i1 < len(widths) - 1 and widths[i1 + 1][1] > 0.6 * w_max:
    i1 += 1
g_lo, g_hi = widths[i0][0], widths[i1][0] + step
blade_up = (t.max() - g_hi) > (g_lo - t.min())
if args.flip:
    blade_up = not blade_up
print(f"guard found {g_lo:.1f}..{g_hi:.1f} cm along the axis (model length {np.ptp(t):.1f} cm, widest {w_max:.1f} cm); "
      f"blade on the {'+' if blade_up else '-'} side{' (flipped by --flip)' if args.flip else ''}")
if not blade_up:
    axis = -axis
    t = -t
    g_lo, g_hi = -g_hi, -g_lo
guard_len = g_hi - g_lo

# ---- edge direction and the frame -----------------------------------------------------------------
blade = positions[t > g_hi + 1.0] - centre
across = blade - np.outer(blade @ axis, axis)
_, _, vt2 = np.linalg.svd(across - across.mean(axis=0), full_matrices=False)
edge = vt2[0] - (vt2[0] @ axis) * axis
edge /= np.linalg.norm(edge)
thin = np.cross(axis, edge)  # det[edge, thin, axis] = +1, so the triangle winding is kept
origin = centre + axis * g_lo  # the guard's bottom = the top end of the handle
basis = np.vstack([edge, thin, axis])
frame_pos = (positions - origin) @ basis.T
frame_nrm = normals @ basis.T
frame_nrm /= np.maximum(np.linalg.norm(frame_nrm, axis=1, keepdims=True), 1e-12)
z = frame_pos[:, 2]
guard_bottom, guard_top = 0.0, guard_len

# ---- grip and pommel ------------------------------------------------------------------------------
if args.handle_bottom_cm is not None:
    handle_bottom = -abs(args.handle_bottom_cm)
    print(f"handle bottom {handle_bottom:.1f} cm (given)")
else:
    end = float(z.min())
    span = -end
    prof = wp.section_widths(frame_pos, tris, end + 0.25, guard_bottom - 0.25, 0.25)
    mid = [w for s, w in prof if end + 0.4 * span <= s <= end + 0.8 * span and w > 0]
    grip_w = float(np.median(mid)) if mid else 0.0
    # The pommel: the widest section in the handle's last quarter, if clearly wider than the grip.
    # It starts at the narrowest section (the waist) between that and the middle of the handle.
    tail = [(s, w) for s, w in prof if s <= end + 0.25 * span]
    handle_bottom = end
    if tail and grip_w > 0:
        s_max, w_max_p = max(tail, key=lambda p: p[1])
        if w_max_p > 1.15 * grip_w:
            between = [(s, w) for s, w in prof if s_max < s <= end + 0.5 * span and w > 0]
            if between:
                handle_bottom = min(between, key=lambda p: p[1])[0]
    print(f"grip width {grip_w:.1f} cm; handle bottom {handle_bottom:.1f} cm, pommel end {end:.1f} cm"
          + ("" if handle_bottom > end else " (no wider pommel found: whole handle is grip)"))

wp.write_visual_obj(os.path.join(args.out, name + ".obj"), f"{name} converted by tools/model_to_weapon.py",
                    frame_pos, frame_nrm, uvs, tris)
pieces = wp.build_pieces(frame_pos, tris, handle_bottom, guard_bottom, guard_top)
wp.write_collision(os.path.join(args.out, name + "_collision.obj"), name, pieces)

# ---- mass -----------------------------------------------------------------------------------------
volume = {}
for slot, _, hull in pieces:
    volume[slot] = volume.get(slot, 0.0) + hull.volume
if args.mass_kg:
    mass, mass_note = args.mass_kg, "given"
else:
    # Convex pieces overstate the volume (fullers, tapers). The model's own enclosed volume (signed
    # tetrahedra, after joining split vertices) is used to scale them down when it is plausible.
    grams = sum(v * (GRIP if s == "Grip" else STEEL) for s, v in volume.items())
    joined = trimesh.util.concatenate(meshes)
    joined.merge_vertices(merge_tex=True, merge_norm=True)
    enclosed = abs(float(joined.volume)) * UNITS_CM[units] ** 3
    convex = sum(volume.values())
    if 0.2 * convex < enclosed <= convex:
        grams *= enclosed / convex
        mass_note = f"estimated: model volume {enclosed:.0f} cm3 (convex {convex:.0f}), steel/grip densities"
    else:
        mass_note = f"estimated from convex volume {convex:.0f} cm3 (model volume {enclosed:.0f} not usable), steel/grip densities"
    mass = round(grams / 1000.0, 3)
print(f"mass {mass} kg ({mass_note}); volumes cm3: " + ", ".join(f"{s} {v:.0f}" for s, v in volume.items()))

top = float(z.max())
numbers = {"mass_kg": float(mass), "pommel_end": round(float(z.min()), 2), "handle_bottom": round(float(handle_bottom), 2),
           "handle_top": 0.0, "guard_bottom": round(float(guard_bottom), 2), "guard_top": round(float(guard_top), 2),
           "tip": round(top, 2)}
wp.write_numbers(os.path.join(args.out, name + ".txt"), f"{name}: written by tools/model_to_weapon.py", numbers)
json.dump({"source": os.path.basename(args.model), "units": units, "mass_kg": mass, "mass_note": mass_note,
           "zones_cm": numbers, "volumes_cm3": {s: round(v, 1) for s, v in volume.items()},
           "pieces": [{"slot": s, "points": len(p)} for s, p, _ in pieces]},
          open(os.path.join(args.out, name + ".json"), "w"), indent=2)
print(f"zones cm: {numbers}; {len(pieces)} collision pieces")

# ---- textures -------------------------------------------------------------------------------------
# One material: the one on the most triangles (a model with several would need its maps merged).
mats = {}
for g in meshes:
    m = getattr(g.visual, "material", None)
    if m is not None:
        mats[id(m)] = (mats.get(id(m), (m, 0))[0], mats.get(id(m), (m, 0))[1] + len(g.faces))
if len(mats) > 1:
    print(f"warning: {len(mats)} materials; textures come from the one on the most triangles")
if mats:
    from PIL import Image
    mat = max(mats.values(), key=lambda p: p[1])[0]

    def rgba(img):
        return None if img is None else np.asarray(img.convert("RGBA")).astype(np.float64) / 255.0

    def factor(v, n):
        if v is None:
            return np.ones(n)
        v = np.asarray(v, dtype=np.float64).ravel()
        return v / 255.0 if v.max() > 1.0 else v

    if isinstance(mat, trimesh.visual.material.PBRMaterial):
        base = rgba(mat.baseColorTexture)
        bf = factor(mat.baseColorFactor, 4)
        nm = rgba(mat.normalTexture)
        mr = rgba(mat.metallicRoughnessTexture)
        occ = rgba(mat.occlusionTexture)
        metal_f = 1.0 if mat.metallicFactor is None else float(mat.metallicFactor)
        rough_f = 1.0 if mat.roughnessFactor is None else float(mat.roughnessFactor)
        if mr is None:  # glTF: no texture means the factors alone (defaults 1)
            metal_c, rough_c = metal_f, rough_f
    else:  # OBJ/MTL and the like: a colour map at most
        base = rgba(getattr(mat, "image", None))
        bf = factor(getattr(mat, "diffuse", None), 4)
        nm = mr = occ = None
        metal_c, rough_c = args.metallic, args.roughness
        print(f"no PBR maps: metallic {metal_c}, roughness {rough_c} (--metallic / --roughness)")
    if base is None and nm is None and mr is None:
        print("material has no maps: no textures written, the mod keeps the game's material")
        sys.exit(0)
    size = (base.shape[1], base.shape[0]) if base is not None else (4, 4)

    def fit(a):
        if a is None or (a.shape[1], a.shape[0]) == size:
            return a
        img = Image.fromarray((a * 255 + 0.5).astype(np.uint8), "RGBA").resize(size, Image.BILINEAR)
        return np.asarray(img).astype(np.float64) / 255.0

    colour = (base[:, :, :3] if base is not None else np.ones((size[1], size[0], 3))) * bf[:3]
    wp.save_rgb(os.path.join(args.out, name + "_BaseColor.png"), colour, False)
    if nm is not None:  # glTF normal maps are OpenGL style (+Y up)
        nm = fit(nm)
        wp.save_normal_from_opengl_xy(os.path.join(args.out, name + "_Normal.png"), nm[:, :, 0] * 2 - 1, nm[:, :, 1] * 2 - 1)
    else:  # flat, so the game material's own normal map is replaced too
        wp.save_normal_from_opengl_xy(os.path.join(args.out, name + "_Normal.png"), np.zeros((4, 4)), np.zeros((4, 4)))
    shape = (size[1], size[0])
    o = fit(occ)[:, :, 0] if occ is not None else np.ones(shape)
    if mr is not None:
        mr = fit(mr)
        r, m = mr[:, :, 1] * rough_f, mr[:, :, 2] * metal_f
    else:
        r, m = np.full(shape, rough_c), np.full(shape, metal_c)
    wp.save_orm(os.path.join(args.out, name + "_ORM.png"), o, r, m)
else:
    print("no material: the mod will keep the game's material")
