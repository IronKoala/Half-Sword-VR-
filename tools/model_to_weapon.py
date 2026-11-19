"""Converts a 3D weapon model (OBJ, glTF/GLB, and other formats trimesh reads) into the files the
HalfSwordCustomWeapon mod reads. The user supplies the model; nothing ships with this repo.

    python -I tools/model_to_weapon.py <model> <out folder> [options]

Options:
  --name NAME         base name of the written files (default: the model's file name)
  --length-cm L       the weapon's real overall length; the model is scaled to it. Downloaded models
                      often carry an arbitrary scale, so this is the safest way to size one.
  --units U           m, cm, mm or in, when not giving --length-cm. glTF/GLB are metres by the glTF
                      spec; for other formats one of the two is required (a file's numbers carry no unit).
  --mass-kg KG        the weapon's mass. Without it the mass is estimated (model volume, steel
                      7.85 g/cm3 for blade, guard and pommel, 1.0 g/cm3 for the grip) and marked as such.
  --flip              the blade points the other way from what was detected
  --handle-bottom-cm  where the grip ends and the pommel starts, in cm below the guard
  --metallic M        for models without PBR maps: metallic (default 1) ...
  --roughness R       ... and roughness (default 0.35) written into the ORM map

How the parts are found (all printed, so a wrong guess shows):
  - If the model's meshes are named after the parts (blade, guard/cross/quillon, grip/handle,
    pommel), those meshes ARE the parts.
  - Otherwise from the shape: the long axis (principal axis), the guard (the widest run of slices),
    the blade (the longer side of the guard), the pommel (where the handle widens again near its end).
  - A widest part right at one end is a head on a haft (axe, mace, hammer, spear; --type hafted forces
    it): the head starts where the cross-section narrows back to the haft's width. No guard.
  The edge direction is the widest spread of the blade's (or head's) cross-sections.

Several materials are merged into one texture atlas (one cell per material, UVs moved to match).

Writes into <out folder>: <name>.obj, <name>_collision.obj, <name>.txt, <name>.json and, when the
model has textures, <name>_BaseColor.png, <name>_Normal.png, <name>_ORM.png.

Needs: pip install trimesh numpy scipy pillow
"""
import argparse
import json
import math
import os
import re
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # python -I leaves the script folder out
import weapon_parts as wp  # noqa: E402

UNITS_CM = {"m": 100.0, "cm": 1.0, "mm": 0.1, "in": 2.54}
STEEL, GRIP, HAFT = 7.85, 1.0, 0.75  # g/cm3; grip = wood and leather, haft = ash/oak wood (estimates)
PART_WORDS = [("Pommel", r"pommel|knob|butt|ferrule"), ("Guard", r"guard|cross|quillon"),
              ("Grip", r"grip|handle|hilt|haft|shaft|pole|staff|shank"),
              ("Head", r"blade|head|axe|spike|point|hammer|flange|mace|bit")]
MANY_TRIANGLES = 30000


def part_of(names):
    for part, pattern in PART_WORDS:
        for n in names:
            if n and re.search(pattern, n, re.I):
                return part
    return None


def load_model(path):
    """The model's meshes in its own units, each with its transform applied: [(names, Trimesh)]."""
    import trimesh
    # process=False keeps the file's vertices (merging would join UV seams). trimesh gives UVs in OBJ
    # convention (v up; it flips glTF's on load) and keeps the files' counter-clockwise fronts.
    scene = trimesh.load(path, force="scene", process=False)
    out = []
    for node in scene.graph.nodes_geometry:
        xf, geom_name = scene.graph[node]
        g = scene.geometry[geom_name]
        if not isinstance(g, trimesh.Trimesh) or len(g.faces) == 0:
            continue
        g = g.copy()
        g.apply_transform(xf)
        names = [str(node), str(geom_name)]
        mat = getattr(g.visual, "material", None)
        if mat is not None and getattr(mat, "name", None):
            names.append(str(mat.name))
        out.append((names, g))
    return out


def model_length(path):
    meshes = load_model(path)
    pts = np.vstack([np.asarray(g.vertices) for _, g in meshes])
    c = pts.mean(axis=0)
    _, _, vt = np.linalg.svd(pts - c, full_matrices=False)
    return float(np.ptp((pts - c) @ vt[0]))


def material_maps(mat, size, metallic, roughness):
    """(colour rgb, normal x, normal y [OpenGL style], ORM) arrays of size (w, h) for one material,
    or None when it has no texture at all."""
    import trimesh
    from PIL import Image
    w, h = size

    def img(i):
        if i is None:
            return None
        return np.asarray(i.convert("RGBA").resize((w, h), Image.BILINEAR)).astype(np.float64) / 255.0

    def factor(v, n):
        if v is None:
            return np.ones(n)
        v = np.asarray(v, dtype=np.float64).ravel()
        return v / 255.0 if v.max() > 1.0 else v

    if isinstance(mat, trimesh.visual.material.PBRMaterial):
        base, nm, mr, occ = img(mat.baseColorTexture), img(mat.normalTexture), img(mat.metallicRoughnessTexture), img(mat.occlusionTexture)
        bf = factor(mat.baseColorFactor, 4)
        metal_f = 1.0 if mat.metallicFactor is None else float(mat.metallicFactor)
        rough_f = 1.0 if mat.roughnessFactor is None else float(mat.roughnessFactor)
    else:  # OBJ/MTL and the like: a colour map at most
        base = img(getattr(mat, "image", None)) if mat is not None else None
        bf = factor(getattr(mat, "diffuse", None), 4) if mat is not None else np.ones(4)
        nm = mr = occ = None
        metal_f, rough_f = metallic, roughness
    colour = (base[:, :, :3] if base is not None else np.ones((h, w, 3))) * bf[:3]
    if nm is not None:
        nx, ny = nm[:, :, 0] * 2 - 1, nm[:, :, 1] * 2 - 1
    else:
        nx, ny = np.zeros((h, w)), np.zeros((h, w))
    o = occ[:, :, 0] if occ is not None else np.ones((h, w))
    if mr is not None:
        r, m = mr[:, :, 1] * rough_f, mr[:, :, 2] * metal_f
    else:
        r, m = np.full((h, w), rough_f), np.full((h, w), metal_f)
    has_maps = base is not None or nm is not None or mr is not None
    return (colour, nx, ny, np.dstack([o, r, m])), has_maps


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("model")
    ap.add_argument("out")
    ap.add_argument("--name")
    ap.add_argument("--length-cm", type=float)
    ap.add_argument("--units", choices=sorted(UNITS_CM))
    ap.add_argument("--mass-kg", type=float)
    ap.add_argument("--flip", action="store_true")
    ap.add_argument("--type", choices=["sword", "hafted"],
                    help="sword: guard between grip and blade; hafted: head on the end of a haft (axe, mace, "
                         "hammer, spear). Default: detected")
    ap.add_argument("--handle-bottom-cm", type=float)
    ap.add_argument("--metallic", type=float, default=1.0)
    ap.add_argument("--roughness", type=float, default=0.35)
    args = ap.parse_args(argv)
    import trimesh  # noqa: F401

    name = args.name or os.path.splitext(os.path.basename(args.model))[0]
    ext = os.path.splitext(args.model)[1].lower()
    units = args.units or ("m" if ext in (".gltf", ".glb") else None)
    if units is None and not args.length_cm:
        sys.exit(f"--length-cm or --units is needed for {ext} files")
    os.makedirs(args.out, exist_ok=True)

    # ---- the model ------------------------------------------------------------------------------------
    meshes = load_model(args.model)
    if not meshes:
        sys.exit("no triangle meshes in the model")
    positions, normals, uvs, tris, mesh_of_vertex = [], [], [], [], []
    for k, (_, g) in enumerate(meshes):
        base = sum(len(p) for p in positions)
        positions.append(np.asarray(g.vertices, dtype=np.float64))
        normals.append(np.asarray(g.vertex_normals, dtype=np.float64))
        uv = getattr(g.visual, "uv", None)
        uvs.append(np.asarray(uv, dtype=np.float64) if uv is not None and len(uv) == len(g.vertices)
                   else np.zeros((len(g.vertices), 2)))
        tris.append(np.asarray(g.faces) + base)
        mesh_of_vertex.append(np.full(len(g.vertices), k))
    positions = np.vstack(positions)
    normals = np.vstack(normals)
    uvs = np.vstack(uvs)
    tris = np.vstack(tris)
    mesh_of_vertex = np.concatenate(mesh_of_vertex)

    centre = positions.mean(axis=0)
    _, _, vt = np.linalg.svd(positions - centre, full_matrices=False)
    axis, side_a, side_b = vt[0], vt[1], vt[2]
    raw_len = float(np.ptp((positions - centre) @ axis))
    if args.length_cm:
        scale = args.length_cm / raw_len
        scale_note = f"scaled to the given length {args.length_cm:g} cm"
    else:
        scale = UNITS_CM[units]
        scale_note = f"units {units}"
    positions = positions * scale
    centre = centre * scale
    print(f"{name}: {len(meshes)} meshes, {len(positions)} vertices, {len(tris)} triangles; "
          f"length {raw_len * scale:.1f} cm ({scale_note})")
    if len(tris) > MANY_TRIANGLES:
        print(f"note: {len(tris)} triangles is a lot for a game weapon (Half Sword's are a few thousand); "
              f"it works, but pressing F9 takes a moment longer")
    t = (positions - centre) @ axis

    # ---- named parts ----------------------------------------------------------------------------------
    mesh_part = [part_of(names) for names, _ in meshes]
    named = "Head" in mesh_part and ("Guard" in mesh_part or "Grip" in mesh_part)
    hafted = False
    vertex_part = np.array([mesh_part[k] or "" for k in mesh_of_vertex]) if named else None
    if named:
        print("parts by mesh name: " + ", ".join(f"{names[0]} -> {p or '?'}" for (names, _), p in zip(meshes, mesh_part)))
        blade_t = t[vertex_part == "Head"]
        hilt = vertex_part != "Head"
        blade_up = blade_t.mean() > t[hilt].mean() if hilt.any() else True
        hafted = args.type == "hafted" or (args.type is None and "Guard" not in mesh_part)
    else:
        # ---- the guard from the shape --------------------------------------------------------------
        # Widths of cross-sections (where the mesh's edges cross each plane), so models whose vertices
        # are far apart (a plain cylinder has them only at its ends) still measure right.
        step = max(0.25, float(np.ptp(t)) / 400.0)
        tmp = np.column_stack([(positions - centre) @ side_a, (positions - centre) @ side_b, t])
        # (centre of each cross-section, its width); bounds below are put at the last narrow section
        # either side, so a cut plane never lands inside the guard or head.
        widths = wp.section_widths(tmp, tris, t.min() + step / 2, t.max(), step, both=True)
        w_max = max(w for _, w in widths)
        peak = max(range(len(widths)), key=lambda i: widths[i][1])
        i0 = i1 = peak  # the run of sections around the widest one above 60 % of its width
        # Then on through the guard's taper: while the width still falls steeply (a blade or grip
        # narrows far more slowly than 2 % of the guard's width per section).
        def wider(a, b):
            return widths[a][1] > 0.6 * w_max or widths[a][1] < widths[b][1] - 0.02 * w_max
        while i0 > 0 and wider(i0 - 1, i0):
            i0 -= 1
        while i1 < len(widths) - 1 and wider(i1 + 1, i1):
            i1 += 1
        g_lo, g_hi = widths[i0][0] - step, widths[i1][0] + step
        blade_up = (t.max() - g_hi) > (g_lo - t.min())
        # The widest part right at one end means a head on a haft, not a guard.
        at = (widths[peak][0] - t.min()) / max(float(np.ptp(t)), 1e-9)
        hafted = args.type == "hafted" or (args.type is None and (at < 0.15 or at > 0.85))
        if hafted:
            blade_up = at > 0.5
            middle = [w for s_, w in widths if w > 0 and 0.3 <= (s_ - t.min()) / np.ptp(t) <= 0.7]
            haft_w = float(np.median(middle)) if middle else 0.0
            # Walk from the widest slice towards the haft; the head stops where the width is back to
            # (nearly) the haft's. (Starting at the widest slice, not the model's end, because the haft
            # often pokes out past the head, as on an axe.)
            idx = range(peak, -1, -1) if blade_up else range(peak, len(widths))
            head_start = widths[idx[-1]][0]
            for i in idx:
                s_, w = widths[i]
                if w > 0 and w <= 1.3 * haft_w:
                    head_start = s_
                    break
            g_lo = g_hi = head_start
            print(f"head on a haft: widest {w_max:.1f} cm at the {'+' if blade_up else '-'} end, haft {haft_w:.1f} cm wide, "
                  f"head starts {head_start:.1f} cm along the axis")
        else:
            print(f"guard found {g_lo:.1f}..{g_hi:.1f} cm along the axis (widest {w_max:.1f} cm)")
    if args.flip:
        blade_up = not blade_up
    print(f"{'head' if hafted else 'blade'} on the {'+' if blade_up else '-'} side{' (flipped by --flip)' if args.flip else ''}")
    if not blade_up:
        axis = -axis
        t = -t
        if not named:
            g_lo, g_hi = -g_hi, -g_lo

    # Where the handle's top end is (the frame's origin) and how long the guard is.
    if named and hafted:
        origin_t = float(t[vertex_part == "Head"].min())  # where the head starts on the haft
        guard_len = 0.0
        print(f"head on a haft (no guard): head starts {origin_t:.1f} cm along the axis")
    elif named:
        if (vertex_part == "Grip").any():
            top_of_grip = float(t[vertex_part == "Grip"].max())
        else:
            top_of_grip = float(t[vertex_part == "Guard"].min())
        guard_t = t[vertex_part == "Guard"] if (vertex_part == "Guard").any() else np.array([top_of_grip])
        origin_t = top_of_grip
        guard_len = max(0.0, float(t[vertex_part == "Head"].min()) - origin_t)
        if (vertex_part == "Guard").any():
            guard_len = max(guard_len, float(guard_t.max()) - origin_t)
    else:
        origin_t = g_lo
        guard_len = g_hi - g_lo

    # ---- edge direction and the frame -----------------------------------------------------------------
    in_blade = (vertex_part == "Head") if named else (t > origin_t + guard_len + 1.0)
    blade = positions[in_blade] - centre
    across = blade - np.outer(blade @ axis, axis)
    _, _, vt2 = np.linalg.svd(across - across.mean(axis=0), full_matrices=False)
    edge = vt2[0] - (vt2[0] @ axis) * axis
    edge /= np.linalg.norm(edge)
    thin = np.cross(axis, edge)  # det[edge, thin, axis] = +1, so the triangle winding is kept
    # The origin sits on the blade's centre line (in case the model is off-centre across the axis).
    blade_mid = blade.mean(axis=0)
    blade_mid = blade_mid - (blade_mid @ axis) * axis
    origin = centre + blade_mid + axis * origin_t
    basis = np.vstack([edge, thin, axis])
    frame_pos = (positions - origin) @ basis.T
    frame_nrm = normals @ basis.T
    frame_nrm /= np.maximum(np.linalg.norm(frame_nrm, axis=1, keepdims=True), 1e-12)
    z = frame_pos[:, 2]
    guard_bottom, guard_top = 0.0, guard_len

    # ---- grip and pommel ------------------------------------------------------------------------------
    end = float(z.min())
    if args.handle_bottom_cm is not None:
        handle_bottom = -abs(args.handle_bottom_cm)
        print(f"handle bottom {handle_bottom:.1f} cm (given)")
    elif named and (vertex_part == "Grip").any():
        handle_bottom = float(z[vertex_part == "Grip"].min())
        print(f"handle bottom {handle_bottom:.1f} cm (bottom of the grip mesh), pommel end {end:.1f} cm")
    else:
        span = -end
        prof = wp.section_widths(frame_pos, tris, end + 0.25, guard_bottom - 0.25, 0.25)
        mid = [w for s, w in prof if end + 0.4 * span <= s <= end + 0.8 * span and w > 0]
        grip_w = float(np.median(mid)) if mid else 0.0
        # The pommel: the widest section in the handle's last quarter, if clearly wider than the
        # grip. It starts at the narrowest section (the waist) between that and the handle's middle.
        tail = [(s, w) for s, w in prof if s <= end + 0.25 * span]
        handle_bottom = end
        if tail and grip_w > 0:
            s_max, w_tail = max(tail, key=lambda p: p[1])
            if w_tail > 1.15 * grip_w:
                between = [(s, w) for s, w in prof if s_max < s <= end + 0.5 * span and w > 0]
                if between:
                    handle_bottom = min(between, key=lambda p: p[1])[0]
        print(f"grip width {grip_w:.1f} cm; handle bottom {handle_bottom:.1f} cm, pommel end {end:.1f} cm"
              + ("" if handle_bottom > end else " (no wider pommel found: whole handle is grip)"))

    # ---- collision pieces -----------------------------------------------------------------------------
    if named:
        # Each part from its own meshes; a mesh with no recognised name goes by where it sits.
        parts = vertex_part.copy()
        unknown = parts == ""
        parts[unknown & (z <= handle_bottom)] = "Pommel"
        parts[unknown & (z > handle_bottom) & (z <= guard_bottom)] = "Grip"
        parts[unknown & (z > guard_bottom) & (z <= guard_top)] = "Guard"
        parts[unknown & (z > guard_top)] = "Head"
        pieces = wp.build_pieces_from_parts(frame_pos, tris, parts)
    else:
        pieces = wp.build_pieces(frame_pos, tris, handle_bottom, guard_bottom, guard_top)

    # ---- textures (before writing the model: an atlas moves the UVs) ----------------------------------
    mats, mat_tris = [], []
    for names, g in meshes:
        m = getattr(g.visual, "material", None)
        idx = next((i for i, x in enumerate(mats) if x is m), None)
        if idx is None:
            mats.append(m)
            mat_tris.append(0)
            idx = len(mats) - 1
        mat_tris[idx] += len(g.faces)
    material_of_mesh = [next(i for i, x in enumerate(mats) if x is getattr(g.visual, "material", None)) for _, g in meshes]
    texture_set = None
    if any(m is not None for m in mats):
        def tex_size(m):
            for a in ("baseColorTexture", "image", "normalTexture", "metallicRoughnessTexture"):
                im = getattr(m, a, None)
                if im is not None:
                    return im.size
            return None
        sizes = [tex_size(m) for m in mats]
        cell = max([max(s) for s in sizes if s] or [16])
        n = len(mats)
        grid = int(math.ceil(math.sqrt(n)))
        cell = min(cell, 4096 // grid)
        maps = [material_maps(m, (cell, cell), args.metallic, args.roughness) for m in mats]
        if any(has for _, has in maps):
            W = grid * cell
            colour = np.ones((W, W, 3))
            nx, ny = np.zeros((W, W)), np.zeros((W, W))
            orm = np.dstack([np.ones((W, W)), np.full((W, W), args.roughness), np.full((W, W), args.metallic)])
            for i, ((c, x, y, o), _) in enumerate(maps):
                r0, c0 = (i // grid) * cell, (i % grid) * cell
                colour[r0:r0 + cell, c0:c0 + cell] = c
                nx[r0:r0 + cell, c0:c0 + cell] = x
                ny[r0:r0 + cell, c0:c0 + cell] = y
                orm[r0:r0 + cell, c0:c0 + cell] = o
            if n > 1:
                # UVs into each material's cell (v up: image row 0 is the top of the atlas).
                for k, (names, g) in enumerate(meshes):
                    sel = mesh_of_vertex == k
                    i = material_of_mesh[k]
                    uv = uvs[sel]
                    if uv.size and (uv.min() < -0.01 or uv.max() > 1.01):
                        print(f"warning: {names[0]} has texture coordinates outside 0..1 (tiling); "
                              "they are wrapped, which can show seams in the atlas")
                        uv = np.mod(uv, 1.0)
                    uv = np.clip(uv, 0.0, 1.0)
                    col, row = i % grid, i // grid
                    uvs[sel, 0] = (col + uv[:, 0]) / grid
                    uvs[sel, 1] = (grid - 1 - row + uv[:, 1]) / grid
                print(f"{n} materials merged into one {W}x{W} texture atlas ({grid}x{grid} cells of {cell})")
            texture_set = (colour, nx, ny, orm)
        else:
            print("no texture maps: the mod keeps the game's material")

    wp.write_visual_obj(os.path.join(args.out, name + ".obj"), f"{name} converted by tools/model_to_weapon.py",
                        frame_pos, frame_nrm, uvs, tris)
    wp.write_collision(os.path.join(args.out, name + "_collision.obj"), name, pieces)
    if texture_set is not None:
        colour, nx, ny, orm = texture_set
        wp.save_rgb(os.path.join(args.out, name + "_BaseColor.png"), colour, False)
        wp.save_normal_from_opengl_xy(os.path.join(args.out, name + "_Normal.png"), nx, ny)
        wp.save_rgb(os.path.join(args.out, name + "_ORM.png"), orm, True)

    # ---- mass -----------------------------------------------------------------------------------------
    volume = {}
    for slot, _, hull in pieces:
        volume[slot] = volume.get(slot, 0.0) + hull.volume
    if args.mass_kg:
        mass, mass_note = args.mass_kg, "given"
    else:
        # Convex pieces overstate the volume (fullers, tapers, curves). The model's own enclosed volume
        # (signed tetrahedra) scales them down when it is plausible.
        grip_density = HAFT if hafted else GRIP
        grams = sum(v * (grip_density if s == "Grip" else STEEL) for s, v in volume.items())
        enclosed = 0.0
        for _, g in meshes:
            gm = g.copy()
            gm.merge_vertices(merge_tex=True, merge_norm=True)
            enclosed += abs(float(gm.volume))
        enclosed *= scale ** 3
        convex = sum(volume.values())
        if 0.2 * convex < enclosed <= convex:
            grams *= enclosed / convex
            mass_note = f"estimated: model volume {enclosed:.0f} cm3 (convex {convex:.0f}), steel/grip densities"
        else:
            mass_note = f"estimated from convex volume {convex:.0f} cm3 (model volume {enclosed:.0f} not usable), steel/grip densities"
        mass = round(grams / 1000.0, 3)
    print(f"mass {mass} kg ({mass_note}); volumes cm3: " + ", ".join(f"{s} {v:.0f}" for s, v in volume.items()))

    top = float(z.max())
    numbers = {"mass_kg": float(mass), "pommel_end": round(end, 2), "handle_bottom": round(float(handle_bottom), 2),
               "handle_top": 0.0, "guard_bottom": round(float(guard_bottom), 2), "guard_top": round(float(guard_top), 2),
               "tip": round(top, 2)}
    wp.write_numbers(os.path.join(args.out, name + ".txt"), f"{name}: written by tools/model_to_weapon.py", numbers)
    json.dump({"source": os.path.basename(args.model), "scale": scale_note, "mass_kg": mass, "mass_note": mass_note,
               "zones_cm": numbers, "volumes_cm3": {s: round(v, 1) for s, v in volume.items()},
               "parts_by_name": named, "type": "hafted" if hafted else "sword", "pieces": [{"slot": s, "points": len(p)} for s, p, _ in pieces]},
              open(os.path.join(args.out, name + ".json"), "w"), indent=2)
    if hafted:
        print(f"length {top - end:.1f} cm: butt end {end:.1f}, haft {handle_bottom:.1f}..0, head 0..{top:.1f} cm; "
              f"{len(pieces)} collision pieces")
    else:
        print(f"length {top - end:.1f} cm: pommel end {end:.1f}, grip {handle_bottom:.1f}..0, guard 0..{guard_top:.1f}, "
              f"blade to {top:.1f} cm; {len(pieces)} collision pieces")


if __name__ == "__main__":
    main()
