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

Skyrim Special Edition .nif files are read too (tools/nif_reader.py; textures found from the .nif's
own paths under the folders above it). Their units are Skyrim units: --units skyrim, or --length-cm.

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
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # python -I leaves the script folder out
import weapon_parts as wp  # noqa: E402
import damage_points as dp  # noqa: E402

UNITS_CM = {"m": 100.0, "cm": 1.0, "mm": 0.1, "in": 2.54,
            "skyrim": 1.42875}  # Creation Kit wiki "Unit": 1 unit = 0.5625 in
STEEL, GRIP, HAFT = 7.85, 1.0, 0.75  # g/cm3; grip = wood and leather, haft = ash/oak wood (estimates)
WOOD = 0.75  # shields: wooden boards with leather/linen and iron fittings, as one density (estimate)
PART_WORDS = [("Pommel", r"pommel|knob|butt|ferrule"), ("Guard", r"guard|cross|quillon"),
              ("Grip", r"grip|handle|hilt|haft|shaft|pole|staff|shank"),
              ("Head", r"blade|head|axe|spike|point|hammer|flange|mace|bit")]
MANY_TRIANGLES = 30000
CHAIN_WORDS = r"chain|link|ring|rope|cord"


def _rotation_to(a, b):
    """The smallest rotation taking direction a onto direction b (Rodrigues)."""
    a = a / max(np.linalg.norm(a), 1e-12)
    b = b / max(np.linalg.norm(b), 1e-12)
    v, c = np.cross(a, b), float(np.dot(a, b))
    if c < -0.999999:  # opposite: half a turn round any axis across a
        ax = np.cross(a, [1.0, 0, 0])
        if np.linalg.norm(ax) < 1e-6:
            ax = np.cross(a, [0, 1.0, 0])
        ax /= np.linalg.norm(ax)
        return 2 * np.outer(ax, ax) - np.eye(3)
    k = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + k + k @ k / (1 + c)


def crossbow_layout(positions, normals, flip):
    """A crossbow in its own frame: z = along the stock towards the front (the bow end), y = across (the
    bow's span: the axis the model is mirror-symmetric across), x = up (the side the bow sits on, which is
    the side the bolt lies on; flip turns it over). Origin: the middle of the bow (the widest slab across).
    Returns (pos, nrm, info)."""
    from scipy.spatial import cKDTree
    c = positions.mean(axis=0)
    _, _, vt = np.linalg.svd(positions - c, full_matrices=False)
    q = (positions - c) @ vt.T
    q -= 0.5 * (q.min(axis=0) + q.max(axis=0))
    pick = q[:: max(1, len(q) // 4000)]
    tree = cKDTree(pick)
    sym = []
    for k in range(3):
        m = pick.copy()
        m[:, k] *= -1
        sym.append(float(np.mean(tree.query(m)[0])) / max(float(np.ptp(pick[:, k])), 1e-9))
    span = int(np.argmin(sym))
    others = [k for k in range(3) if k != span]
    along = max(others, key=lambda k: np.ptp(q[:, k]))
    up = [k for k in others if k != along][0]
    sz, sy = vt[along], vt[span]
    # Front: the end where the model is widest across (the bow).
    t = (positions - c) @ sz
    w = (positions - c) @ sy
    lo, hi = t.min(), t.max()
    band = 0.15 * (hi - lo)
    front_w = np.ptp(w[t > hi - band]) if (t > hi - band).any() else 0
    back_w = np.ptp(w[t < lo + band]) if (t < lo + band).any() else 0
    if back_w > front_w:
        sz = -sz
    # The bow's slab: the 4 cm slab along z with the widest span.
    t = (positions - c) @ sz
    w = (positions - c) @ sy
    best, bz = -1.0, 0.0
    for z0 in np.arange(t.min(), t.max(), 1.0):
        sel = (t >= z0) & (t < z0 + 4.0)
        if sel.sum() > 2 and np.ptp(w[sel]) > best:
            best, bz = float(np.ptp(w[sel])), z0 + 2.0
    sx = np.cross(sy, sz)
    bow = (np.abs(t - bz) < 2.0)
    stock = ~bow
    # Up: the bow sits above the stock's middle line.
    if bow.any() and stock.any() and float(((positions[bow] - c) @ sx).mean()) < float(((positions[stock] - c) @ sx).mean()):
        sx, sy = -sx, -sy  # turned over round z (det stays +1)
    if flip:
        sx, sy = -sx, -sy
    basis = np.vstack([sx, sy, sz])
    pos = (positions - c) @ basis.T
    sel = np.abs(pos[:, 2] - bz) < 2.0
    origin = np.array([0.5 * (pos[sel, 0].min() + pos[sel, 0].max()), 0.5 * (pos[sel, 1].min() + pos[sel, 1].max()), bz])
    pos -= origin
    nrm = normals @ basis.T
    return pos, nrm, dict(span=best, symmetry=round(sym[span], 4))


def flail_layout(positions, normals, tris, mesh_of_vertex, meshes):
    """A flail laid out straight in the weapon frame: the staff on z <= 0 (its top end at 0), the chain from 0
    to Lc, the head beyond. Each of the three is moved as one rigid piece (a chain drawn hanging stays the
    shape it was drawn). Parts come from mesh names (chain/link/ring, grip/handle/haft/staff, head/ball/...)
    or else from the pieces of the model that do not touch: the longest is the staff, the thickest the
    head (small pieces close to it, spikes, join it), the rest the chain.
    Returns (pos, nrm, label per vertex ('Grip', 'Link', 'Head'), Lc)."""
    import re
    from scipy.sparse import coo_matrix
    from scipy.sparse.csgraph import connected_components
    # Pieces that do not touch: vertices merged by position first (UV seams split a mesh's vertices).
    _, key = np.unique(np.round(positions, 4), axis=0, return_inverse=True)
    key = key.ravel()
    a = np.concatenate([key[tris[:, 0]], key[tris[:, 1]]])
    b = np.concatenate([key[tris[:, 1]], key[tris[:, 2]]])
    n = int(key.max()) + 1
    _, comp_of_key = connected_components(coo_matrix((np.ones(len(a)), (a, b)), shape=(n, n)), directed=False)
    comp = comp_of_key[key]
    label = np.array([""] * len(positions), dtype=object)
    for k, (names, _) in enumerate(meshes):
        text = " ".join(names).lower()
        sel = mesh_of_vertex == k
        if re.search(CHAIN_WORDS, text):
            label[sel] = "Link"
        else:
            part = part_of(names)
            if part in ("Grip", "Pommel"):
                label[sel] = "Grip"
            elif part in ("Head", "Guard"):
                label[sel] = "Head"
    if not ((label == "Grip").any() and (label == "Link").any() and (label == "Head").any()):
        label[:] = ""
        info = {}
        for c in np.unique(comp):
            q = positions[comp == c]
            if len(q) < 4:
                continue
            ctr = q.mean(axis=0)
            _, sv, vt = np.linalg.svd(q - ctr, full_matrices=False)
            ext = np.ptp((q - ctr) @ vt.T, axis=0)
            info[c] = (ext, ctr)
        if len(info) < 3:
            sys.exit("a flail needs its staff, chain and head as separate pieces of the model (or meshes named "
                     "handle/staff, chain/link and head/ball)")
        staff = max(info, key=lambda c: info[c][0][0])
        rest = [c for c in info if c != staff]
        head = max(rest, key=lambda c: info[c][0][1] * info[c][0][2])  # the thickest across
        h_ext, h_ctr = info[head]
        label[comp == staff] = "Grip"
        label[comp == head] = "Head"
        for c in rest:
            if c != head:
                near = np.linalg.norm(info[c][1] - h_ctr) < 0.75 * float(h_ext[0]) + 1e-6
                label[comp == c] = "Head" if near else "Link"
        label[label == ""] = "Link"  # tiny bits
        if not (label == "Link").any():
            sys.exit("no chain found between the staff and the head")
    gp, lp, hp = positions[label == "Grip"], positions[label == "Link"], positions[label == "Head"]
    # The staff: its long axis, pointing to the end nearer the chain.
    gc = gp.mean(axis=0)
    _, _, vt = np.linalg.svd(gp - gc, full_matrices=False)
    ax, side = vt[0], vt[1]
    if np.dot(lp.mean(axis=0) - gc, ax) < 0:
        ax, side = -ax, side
    t = (gp - gc) @ ax
    top = gc + ax * float(t.max())
    basis = np.vstack([side, np.cross(ax, side), ax])  # rows x, y, z; det +1
    pos = (positions - top) @ basis.T
    nrm = normals @ basis.T
    # The chain: from its point nearest the staff's top to its point nearest the head, onto +z from 0.
    L = pos[label == "Link"]
    hc = pos[label == "Head"].mean(axis=0)
    S = L[np.argmin(np.linalg.norm(L, axis=1))]
    E = L[np.argmin(np.linalg.norm(L - hc, axis=1))]
    Lc = float(np.linalg.norm(E - S))
    if Lc < 1.0:
        sys.exit("the chain is shorter than 1 cm")
    R = _rotation_to(E - S, np.array([0, 0, 1.0]))
    sel = label == "Link"
    pos[sel] = (pos[sel] - S) @ R.T
    nrm[sel] = nrm[sel] @ R.T
    # The head: from the chain's end towards its middle, onto +z beyond the chain.
    Rh = _rotation_to(hc - E, np.array([0, 0, 1.0]))
    sel = label == "Head"
    pos[sel] = (pos[sel] - E) @ Rh.T + np.array([0, 0, Lc])
    nrm[sel] = nrm[sel] @ Rh.T
    return pos, nrm, label, Lc



def part_of(names):
    for part, pattern in PART_WORDS:
        for n in names:
            if n and re.search(pattern, n, re.I):
                return part
    return None


def gltf_spec_gloss_materials(path):
    """glTF materials written with KHR_materials_pbrSpecularGlossiness (older exporters, Sketchfab),
    as metal/rough PBRMaterials by material name. trimesh's own conversion of these gives wrong colours
    (seen on a Sketchfab GLB: grey and alpha from the wrong image), so the diffuse texture is read
    straight from the file. Spec/gloss has no metal value: metallic is taken as 1 when the specular
    colour is bright (> 0.5, a metal's), else 0; roughness = 1 - glossiness."""
    import base64
    import io
    import struct
    import trimesh
    from PIL import Image
    try:
        raw = open(path, "rb").read()
        if raw[:4] == b"glTF":
            n = struct.unpack("<I", raw[12:16])[0]
            doc = json.loads(raw[20:20 + n])
            rest = raw[20 + n:]
            glb_bin = rest[8:8 + struct.unpack("<I", rest[:4])[0]] if len(rest) >= 8 else b""
        else:
            doc = json.loads(raw.decode("utf-8"))
            glb_bin = b""
        if "KHR_materials_pbrSpecularGlossiness" not in doc.get("extensionsUsed", []):
            return {}
        buffers = []
        for b in doc.get("buffers", []):
            uri = b.get("uri")
            if uri is None:
                buffers.append(glb_bin)
            elif uri.startswith("data:"):
                buffers.append(base64.b64decode(uri.split(",", 1)[1]))
            else:
                buffers.append(open(os.path.join(os.path.dirname(path), uri), "rb").read())

        def image(tex_ref):
            if not tex_ref:
                return None
            src = doc["textures"][tex_ref["index"]].get("source")
            if src is None:
                return None
            im = doc["images"][src]
            if "bufferView" in im:
                bv = doc["bufferViews"][im["bufferView"]]
                o = bv.get("byteOffset", 0)
                data = buffers[bv["buffer"]][o:o + bv["byteLength"]]
            elif im.get("uri", "").startswith("data:"):
                data = base64.b64decode(im["uri"].split(",", 1)[1])
            else:
                data = open(os.path.join(os.path.dirname(path), im["uri"]), "rb").read()
            return Image.open(io.BytesIO(data)).convert("RGBA")

        out = {}
        for m in doc.get("materials", []):
            sg = m.get("extensions", {}).get("KHR_materials_pbrSpecularGlossiness")
            if sg is None or "name" not in m:
                continue
            spec = max(sg.get("specularFactor", [1, 1, 1]))
            out[m["name"]] = trimesh.visual.material.PBRMaterial(
                name=m["name"], baseColorTexture=image(sg.get("diffuseTexture")),
                baseColorFactor=sg.get("diffuseFactor", [1, 1, 1, 1]),
                normalTexture=image(m.get("normalTexture")), occlusionTexture=image(m.get("occlusionTexture")),
                metallicFactor=1.0 if spec > 0.5 else 0.0, roughnessFactor=1.0 - sg.get("glossinessFactor", 1.0))
        if out:
            print(f"{len(out)} specular-glossiness materials read from the file")
        return out
    except Exception as e:  # leave it to trimesh
        print(f"note: could not read the file's specular-glossiness materials ({e})")
        return {}


# Typical masses of real weapons by overall length (cm, kg): swords and daggers from dagger (0.2-0.45 kg)
# through arming sword (~1.1 kg at 90 cm) and longsword (1.1-1.8 kg at 110-125 cm) to two-handers
# (2-3.5 kg at 140-180 cm); hafted from hand axes and maces (1-1.6 kg) to spears (1.5-2.5 kg) and
# halberds/poleaxes (2.5-3.5 kg). Commonly quoted ranges for surviving originals, used only to keep the
# estimate sane; a weight the user types always wins.
TYPICAL_SWORD = [(20, 0.2), (40, 0.4), (60, 0.7), (90, 1.1), (120, 1.5), (150, 2.2), (180, 3.0), (220, 3.8)]
TYPICAL_HAFTED = [(40, 0.6), (70, 1.2), (90, 1.6), (120, 2.0), (200, 2.2), (250, 2.8), (300, 3.3)]


def typical_mass_kg(length_cm, hafted):
    pts = TYPICAL_HAFTED if hafted else TYPICAL_SWORD
    return float(np.interp(length_cm, [p[0] for p in pts], [p[1] for p in pts]))


def find_texture(model_path, tex):
    """A texture path as a Bethesda .nif stores it ("textures\\a\\b.dds", relative to the game's Data
    folder): looked for under every folder above the model, then by file name next to it."""
    rel = tex.replace("\\", "/").lstrip("/")
    d = os.path.dirname(os.path.abspath(model_path))
    while True:
        for cand in (os.path.join(d, rel), os.path.join(d, "Data", rel)):
            if os.path.isfile(cand):
                return cand
        up = os.path.dirname(d)
        if up == d:
            break
        d = up
    base = os.path.basename(rel).lower()
    for root, _, files in os.walk(os.path.dirname(os.path.abspath(model_path))):
        for f in files:
            if f.lower() == base:
                return os.path.join(root, f)
    return None


def load_nif(path):
    """Skyrim SE .nif: the weapon's shapes as Trimeshes with PBR materials made from the Skyrim maps.
    Left out: the scabbard ("Scb"), blood decals and effect-shader layers (glow overlays).
    Skyrim maps: diffuse; _n = normal (DirectX style: the meshes' stored tangent frames put +V of the
    texture, image-down, on the map's green axis) with specular strength in alpha; _m = environment
    (reflection) mask. Turned into PBR as: metallic = environment mask, roughness = 1 - 0.65 x specular
    (an approximation: Skyrim has no roughness map)."""
    import trimesh
    from PIL import Image
    import nif_reader
    out = []
    loaded = {}

    def img(tex):
        if not tex:
            return None
        if tex not in loaded:
            f = find_texture(path, tex)
            loaded[tex] = Image.open(f).convert("RGBA") if f else None
            print(f"texture {tex}: " + (os.path.basename(f) if f else "NOT FOUND (the mesh keeps the game's material colour)"))
        return loaded[tex]

    for name, parents, pos, nrm, uv, faces, tex, kind in nif_reader.read_nif(path):
        low = name.lower()
        if kind == "effect" or low == "scb" or "scabbard" in low or "blood" in low or any(p.lower() == "scb" for p in parents):
            print(f"left out: {name} ({'effect layer' if kind == 'effect' else 'scabbard' if 'scb' in low or 'scabbard' in low else 'blood decal'})")
            continue
        tex = list(tex) + [""] * (6 - len(tex))
        base, nm, env = img(tex[0]), img(tex[1]), img(tex[5])
        mat = None
        if base is not None:
            normal = mr = None
            if nm is not None:
                a = np.asarray(nm).astype(np.float64) / 255.0
                spec = a[:, :, 3]
                g = np.asarray(nm)[:, :, :3].copy()
                g[:, :, 1] = 255 - g[:, :, 1]  # DirectX -> OpenGL (PBRMaterial convention)
                normal = Image.fromarray(g, "RGB")
                rough = 1.0 - 0.65 * spec
            else:
                rough = None
            if env is not None or rough is not None:
                w, h = (nm or env).size
                metal = np.zeros((h, w))
                if env is not None:
                    metal = np.asarray(env.convert("L").resize((w, h), Image.BILINEAR)).astype(np.float64) / 255.0
                if rough is None:
                    rough = np.full((h, w), 0.5)
                mr = Image.fromarray((np.dstack([np.zeros((h, w)), rough, metal]) * 255 + 0.5).astype(np.uint8), "RGB")
            mat = trimesh.visual.material.PBRMaterial(name=name, baseColorTexture=base, normalTexture=normal,
                                                      metallicRoughnessTexture=mr, metallicFactor=1.0, roughnessFactor=1.0)
        g = trimesh.Trimesh(vertices=pos, faces=faces, vertex_normals=nrm, process=False,
                            visual=trimesh.visual.TextureVisuals(uv=uv, material=mat))
        out.append(([name] + parents, g))
    return out


def load_fbx(path):
    """Autodesk .fbx (binary or text) through tools/fbx_reader.py: every mesh where its node puts it, in
    metres from the file's own unit, one Trimesh per mesh and material, with the colour and normal maps
    the material names (embedded in the file, or files found next to it). Normal maps are taken as
    OpenGL style (Blender, Maya, Substance's default); a model whose bumps look inverted came with
    DirectX maps."""
    import io
    import trimesh
    from PIL import Image
    import fbx_reader
    parts, unit_cm = fbx_reader.load(path)
    print(f"fbx: 1 file unit = {unit_cm:g} cm, {len(parts)} mesh parts")
    cache = {}

    def image(entry):
        if not entry:
            return None
        names, content = entry
        key = (tuple(names), content is not None)
        if key in cache:
            return cache[key]
        im, how = None, "NOT FOUND (the mesh keeps the game's material colour)"
        if content:
            try:
                im, how = Image.open(io.BytesIO(content)).convert("RGBA"), "embedded"
            except OSError:
                im = None
        for name in ([] if im is not None else names):
            f = find_texture(path, name)
            if f:
                try:
                    im, how = Image.open(f).convert("RGBA"), "found"
                    break
                except OSError:
                    pass
        print(f"texture {os.path.basename((names[0] if names else '?').replace(chr(92), '/'))}: {how}")
        cache[key] = im
        return im

    def material(m):
        if m is None:
            return None
        tex = m["textures"]
        base = image(tex.get("DiffuseColor") or tex.get("Diffuse") or tex.get("Maya|baseColor") or tex.get("base_color"))
        normal = image(tex.get("NormalMap") or tex.get("Bump") or tex.get("Maya|normalCamera"))
        colour = None if base is not None or m["diffuse"] is None else list(m["diffuse"]) + [1.0]
        return trimesh.visual.material.PBRMaterial(
            name=m["name"], baseColorTexture=base, normalTexture=normal.convert("RGB") if normal else None,
            baseColorFactor=colour)

    out = []
    for names, pos, nrm, uv, tris, mat in parts:
        if len(tris) == 0:
            continue
        g = trimesh.Trimesh(vertices=pos, faces=tris, vertex_normals=nrm, process=False,
                            visual=trimesh.visual.TextureVisuals(uv=uv, material=material(mat)))
        out.append((names, g))
    return drop_scenery(out)


def load_model(path):
    """The model's meshes in its own units, each with its transform applied: [(names, Trimesh)]."""
    import trimesh
    if path.lower().endswith(".nif"):
        return load_nif(path)
    if path.lower().endswith(".fbx"):
        return load_fbx(path)
    # process=False keeps the file's vertices (merging would join UV seams). trimesh gives UVs in OBJ
    # convention (v up; it flips glTF's on load) and keeps the files' counter-clockwise fronts.
    scene = trimesh.load(path, force="scene", process=False)
    spec_gloss = gltf_spec_gloss_materials(path) if path.lower().endswith((".glb", ".gltf")) else {}
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
        if mat is not None and getattr(mat, "name", None) in spec_gloss:
            g.visual.material = mat = spec_gloss[mat.name]
        if mat is not None and getattr(mat, "name", None):
            names.append(str(mat.name))
        out.append((names, g))
    return drop_scenery(out)


SCENERY_WORDS = r"ground|floor|backdrop|studio|environment|turntable|plinth|pedestal|display stand|camera|light"


def drop_scenery(meshes):
    """Leaves out what a model's author put around the weapon for its presentation: meshes named like
    scenery (a "Studio | Ground" plane, a backdrop), and flat meshes far bigger than everything else
    (a floor plane 200 m wide made a 104 cm knife read as 20,000 cm)."""
    if len(meshes) < 2:
        return meshes
    keep, dropped = [], []
    for names, g in meshes:
        ext = np.ptp(np.asarray(g.vertices), axis=0)
        others = [np.asarray(h.vertices) for n2, h in meshes if h is not g]
        rest = float(np.ptp(np.vstack(others), axis=0).max()) if others else 0.0
        flat = ext.min() < 1e-6 * max(ext.max(), 1e-12) or len(g.vertices) <= 8
        if re.search(SCENERY_WORDS, " ".join(names), re.I) or (flat and ext.max() > 3 * rest > 0):
            dropped.append(names[0])
        else:
            keep.append((names, g))
    if dropped and keep:
        print("left out (scenery, not part of the weapon): " + ", ".join(dropped))
        return keep
    return meshes


def reduce_meshes(meshes, target):
    """The meshes simplified (quadric edge collapse, fast_simplification) to about target triangles in all,
    each mesh keeping its share. Border edges stay: a model's meshes are split at their UV seams (separate
    vertices), so seams and open edges keep their place and the texture still lines up. Each vertex left
    keeps its own UV and normal (the collapses say which original vertex it is)."""
    import trimesh
    total = sum(len(g.faces) for _, g in meshes)
    if total <= target:
        print(f"reduce detail: the model has {total:,} triangles, at most {target:,}: kept as it is")
        return meshes
    try:
        import fast_simplification
    except ImportError:
        print("reduce detail: fast_simplification is not installed: the model is kept as it is")
        return meshes
    out, after = [], 0
    for names, g in meshes:
        f = np.asarray(g.faces, dtype=np.int64)
        want = int(round(target * len(f) / total))
        if len(f) < 200 or want >= len(f):
            out.append((names, g))
            after += len(f)
            continue
        v = np.asarray(g.vertices, dtype=np.float64)
        p2, f2, col = fast_simplification.simplify(v, f, target_count=max(want, 100), return_collapses=True,
                                                   preserve_border=True)
        # The simplified mesh's vertices are the originals not collapsed away, in their order (moved to
        # the best place for the shape).
        kept = np.setdiff1d(np.arange(len(v)), col[:, 1]) if len(col) else np.arange(len(v))
        if len(kept) != len(p2):
            print(f"reduce detail: {names[0]} could not be matched up, kept as it is")
            out.append((names, g))
            after += len(f)
            continue
        nrm = np.asarray(g.vertex_normals, dtype=np.float64)[kept]
        uv = getattr(g.visual, "uv", None)
        if uv is not None and len(uv) == len(v):
            visual = trimesh.visual.TextureVisuals(uv=np.asarray(uv)[kept], material=g.visual.material)
        else:
            visual = None
        h = trimesh.Trimesh(vertices=p2, faces=np.asarray(f2, dtype=np.int64), vertex_normals=nrm, process=False)
        if visual is not None:
            h.visual = visual
        elif getattr(g.visual, "material", None) is not None:
            h.visual = trimesh.visual.TextureVisuals(material=g.visual.material)
        out.append((names, h))
        after += len(f2)
    print(f"reduce detail: {total:,} triangles -> {after:,}")
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
        if all(v is None for v in (mat.baseColorTexture, mat.normalTexture, mat.metallicRoughnessTexture,
                                   mat.baseColorFactor, mat.metallicFactor, mat.roughnessFactor)):
            # A material with nothing in it (Blender node setups that glTF cannot carry export like
            # this): plain steel instead of glTF's default white rough metal. Iron's base colour
            # 0.56/0.57/0.58 from the usual PBR reference charts; roughness/metallic from the options.
            bf = np.array([0.56, 0.57, 0.58, 1.0])
            metal_f, rough_f = metallic, roughness
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
    # Plain colours count too (a material with only a colour and roughness/metal numbers).
    has_maps = base is not None or nm is not None or mr is not None or (
        isinstance(mat, trimesh.visual.material.PBRMaterial) and
        any(v is not None for v in (mat.baseColorFactor, mat.metallicFactor, mat.roughnessFactor)))
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
    ap.add_argument("--hand-slide", action="store_true",
                    help="long handle held by guard + pommel, the guard hand slides down it in swings (e.g. the Spit)")
    ap.add_argument("--type", choices=["sword", "hafted", "double", "shield", "flail", "crossbow"],
                    help="sword: guard between grip and blade; hafted: head on the end of a haft (axe, mace, "
                         "hammer, spear); double: a blade or head on both ends of a haft. Default: detected "
                         "(sword or hafted)")
    ap.add_argument("--handle-bottom-cm", type=float)
    ap.add_argument("--preview", action="store_true", help="quick: parts, collision and numbers only (no textures, "
                    "no damage points, mass from the convex pieces); the converter's picture before Install")
    ap.add_argument("--reduce", type=int, metavar="TRIANGLES",
                    help="simplify a denser model to about this many triangles (UVs and normals kept, UV seams and "
                         "open edges left in place)")
    ap.add_argument("--turn", action="store_true", help="hafted: the head faces the other way (front and back swapped)")
    ap.add_argument("--held", choices=["one", "two", "spear"], help="hafted: the game weapon that carries it (one hand: "
                    "mace, two hands: great axe, spear); default from the length")
    ap.add_argument("--splits", help="where the parts meet, chosen by hand: 'A,B,C' = cm up from the pommel end to "
                                     "the grip's start, the guard's start and the blade's start (A<=B<=C)")
    ap.add_argument("--part-lengths", help="'pommel,grip,guard,blade' new lengths in cm; empty or 0 keeps that part "
                                           "(each part is stretched or shrunk along the weapon on its own)")
    ap.add_argument("--edges", choices=["double", "single", "single_other", "single+", "single-", "none"],
                    help="blade edges: both sides, one side (+x or -x of the frame), or none (blunt); default from the shape")
    ap.add_argument("--head-kind", choices=["spear", "axe", "blunt"], help="hafted head; default from the shape")
    ap.add_argument("--edge-sharp", type=float, help="edge sharpness 0-100 (game's arming sword 90)")
    ap.add_argument("--tip-sharp", type=float, help="thrust sharpness 0-100 (default from the point's angle)")
    ap.add_argument("--rigidity", type=float, help="rigidity x100 (arming sword 80, mace 125)")
    ap.add_argument("--balance-cm", type=float, help="balance point, cm above the top of the grip (default from the model)")
    ap.add_argument("--metallic", type=float, default=1.0)
    ap.add_argument("--roughness", type=float, default=0.35)
    args = ap.parse_args(argv)
    import trimesh  # noqa: F401

    name = args.name or os.path.splitext(os.path.basename(args.model))[0]
    ext = os.path.splitext(args.model)[1].lower()
    units = args.units or ("m" if ext in (".gltf", ".glb", ".fbx") else None)
    if units is None and not args.length_cm:
        sys.exit(f"--length-cm or --units is needed for {ext} files")
    os.makedirs(args.out, exist_ok=True)

    # ---- the model ------------------------------------------------------------------------------------
    meshes = load_model(args.model)
    if not meshes:
        sys.exit("no triangle meshes in the model")
    if args.reduce:
        meshes = reduce_meshes(meshes, args.reduce)
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
              f"it works, but each spawn of the weapon in the game takes a moment longer")
    t = (positions - centre) @ axis

    shield = args.type == "shield"
    lens_before = None
    if shield:
        # ---- a shield: z = its height (longest), x = its width, y = its thickness; origin at the middle
        # of its outline; +y = the front (the side it bulges towards: a curved shield's rim lies behind
        # its middle; flat ones go by the boss). --flip turns it round.
        sx, sz = side_a, axis
        sy = np.cross(sz, sx)  # det[x, y, z] = +1 keeps the triangle winding
        basis = np.vstack([sx, sy, sz])
        fp = (positions - centre) @ basis.T
        mid = 0.5 * (fp.min(axis=0) + fp.max(axis=0))
        fp -= mid
        half = np.maximum(0.5 * np.ptp(fp, axis=0), 1e-6)
        rad = np.sqrt((fp[:, 0] / half[0]) ** 2 + (fp[:, 2] / half[2]) ** 2)
        inner, rim = fp[rad < 0.3, 1], fp[rad > 0.8, 1]
        front = 1.0
        # Best cue: fittings. Straps, handles and their bars are small separate meshes behind the board;
        # a boss is a small mesh too but round and in the middle (and on the front), so it does not vote.
        cue = None
        if len(meshes) > 1:
            ext = {}
            for k in range(len(meshes)):
                sel = mesh_of_vertex == k
                if sel.any():
                    q = fp[sel]
                    ext[k] = (np.ptp(q[:, 0]) * np.ptp(q[:, 2]), q.mean(axis=0), np.ptp(q[:, 0]), np.ptp(q[:, 2]), int(sel.sum()))
            board = max(ext, key=lambda k: ext[k][0])
            vote = 0.0
            for k, (area, c, wx, wz, n) in ext.items():
                if k == board or area > 0.2 * ext[board][0]:
                    continue
                r = np.sqrt((c[0] / half[0]) ** 2 + (c[2] / half[2]) ** 2)
                aspect = wx / max(wz, 1e-6)
                if r < 0.25 and 0.6 < aspect < 1.6:
                    continue  # a boss
                dy = c[1] - ext[board][1][1]
                if abs(dy) > 0.3:
                    vote += n * np.sign(dy)
            if vote != 0:
                front = -1.0 if vote > 0 else 1.0  # fittings on +y: +y is the back
                cue = "straps/handles on the back"
        if cue is None and len(inner) and len(rim):
            curve = float(inner.mean() - rim.mean())  # > 0: the middle stands out on +y
            if abs(curve) > 0.05 * half[1]:
                front = 1.0 if curve > 0 else -1.0
            else:
                front = 1.0 if (inner.max() - rim.max()) >= (-inner.min() + rim.min()) else -1.0
        if args.flip:
            front = -front
        if front < 0:  # turn it round the height axis: x and y change sign, the winding stays
            basis = np.vstack([-sx, -sy, sz])
            fp = (positions - centre) @ basis.T
            fp -= 0.5 * (fp.min(axis=0) + fp.max(axis=0))
        frame_pos = fp
        frame_nrm = normals @ basis.T
        frame_nrm /= np.maximum(np.linalg.norm(frame_nrm, axis=1, keepdims=True), 1e-12)
        z = frame_pos[:, 2]
        named, hafted, vertex_part = False, False, None
        end = float(z.min())
        handle_bottom = guard_bottom = guard_top = end  # the whole shield is one part ("Head")
        w_, t_, h_ = np.ptp(frame_pos, axis=0)
        print(f"shield: {w_:.1f} cm wide, {h_:.1f} cm high, {t_:.1f} cm thick; front on the "
              f"{'+' if front > 0 else '-'} side, found by {cue or 'its curve/boss'}"
              f"{' (turned round by --flip)' if args.flip else ''}")
    elif args.type == "crossbow":
        frame_pos, frame_nrm, cb = crossbow_layout(positions, normals, args.flip)
        frame_nrm /= np.maximum(np.linalg.norm(frame_nrm, axis=1, keepdims=True), 1e-12)
        z = frame_pos[:, 2]
        named, hafted, vertex_part = False, False, None
        end = float(z.min())
        handle_bottom = guard_bottom = guard_top = end  # one part ("Head")
        print(f"crossbow: {float(np.ptp(z)):.1f} cm long, bow {cb['span']:.1f} cm across (mirror symmetry "
              f"{cb['symmetry']}); front +z, bow on +x{' (turned over by --flip)' if args.flip else ''}")
    elif args.type == "flail":
        frame_pos, frame_nrm, flail_label, chain_len = flail_layout(positions, normals, tris, mesh_of_vertex, meshes)
        frame_nrm /= np.maximum(np.linalg.norm(frame_nrm, axis=1, keepdims=True), 1e-12)
        z = frame_pos[:, 2]
        named, hafted, vertex_part = False, True, None
        end = float(z[flail_label == "Grip"].min())
        handle_bottom, guard_bottom, guard_top = end, 0.0, chain_len
        print(f"flail: staff {-end:.1f} cm, chain {chain_len:.1f} cm, head "
              f"{float(z[flail_label == 'Head'].max()) - chain_len:.1f} cm (laid out straight)")
    else:
        # ---- named parts ----------------------------------------------------------------------------------
        mesh_part = [part_of(names) for names, _ in meshes]
        named = "Head" in mesh_part and ("Grip" in mesh_part or ("Guard" in mesh_part and "Pommel" in mesh_part))
        if named:
            # Names are only trusted when they make sense: a "blade" mesh that also runs the length of the
            # handle (a full tang modelled as one piece with the blade) does not mark where the blade starts.
            head_t = t[np.isin(mesh_of_vertex, [k for k, p in enumerate(mesh_part) if p == "Head"])]
            hilt_t = t[np.isin(mesh_of_vertex, [k for k, p in enumerate(mesh_part) if p in ("Grip", "Pommel")])]
            if len(hilt_t):
                ov = min(head_t.max(), hilt_t.max()) - max(head_t.min(), hilt_t.min())
                if ov > 0.3 * max(float(np.ptp(hilt_t)), 1e-9):
                    print("part names ignored: the blade mesh runs through the handle (full tang?); finding parts from the shape")
                    named = False
        hafted = False
        vertex_part = np.array([mesh_part[k] or "" for k in mesh_of_vertex]) if named else None
        if named:
            print("parts by mesh name: " + ", ".join(f"{names[0]} -> {p or '?'}" for (names, _), p in zip(meshes, mesh_part)))
            blade_t = t[vertex_part == "Head"]
            hilt = vertex_part != "Head"
            blade_up = blade_t.mean() > t[hilt].mean() if hilt.any() else True
            hafted = args.type in ("hafted", "double") or (args.type is None and "Guard" not in mesh_part)
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
            # So does a wide part much longer than a guard (an axe's head reaches over a fifth of the length).
            long_run = (g_hi - g_lo) > 0.2 * float(np.ptp(t))
            # A long wide part on a model whose meshes or materials say "blade" is a guardless blade
            # (machete, long knife), not an axe head.
            says_blade = any(re.search(r"blade|sword|knife|machete|sabre|saber|katana|dagger", " ".join(n), re.I)
                             for n, _ in meshes)
            end_peak = at < 0.15 or at > 0.85
            hafted = args.type in ("hafted", "double") or (args.type is None and (end_peak or long_run) and not (says_blade and long_run))
            guardless = not hafted and long_run  # a sword chosen by the user, but no short wide guard in it
            if hafted or guardless:
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
                if guardless:
                    # No guard to go by: a blade is much thinner than its handle (scales, wrap), so the
                    # thickness profile splits them. The split with the biggest step between the two
                    # sides' median thickness is where the blade starts; the thinner side is the blade.
                    thk = np.column_stack([(positions - centre) @ side_b, (positions - centre) @ side_a, t])
                    th = np.array([w for _, w in wp.section_widths(thk, tris, t.min() + step / 2, t.max(), step)])
                    cs = np.array([c for c, _ in widths])
                    n_s = len(th)
                    best, k_best = -1.0, None
                    for k in range(max(1, n_s // 10), n_s - max(1, n_s // 10)):
                        a_, b_ = np.median(th[:k]), np.median(th[k:])
                        if abs(a_ - b_) > best:
                            best, k_best = abs(a_ - b_), k
                    if k_best is not None:
                        blade_up = np.median(th[k_best:]) < np.median(th[:k_best])
                        # Exact place: from the blade side, the first section thicker than halfway between
                        # the two medians.
                        t_blade = np.median(th[k_best:] if blade_up else th[:k_best])
                        t_handle = np.median(th[:k_best] if blade_up else th[k_best:])
                        half = 0.5 * (t_blade + t_handle)
                        order = range(n_s - 1, -1, -1) if blade_up else range(n_s)
                        k_edge = k_best
                        for i in order:
                            if th[i] > half:
                                k_edge = i
                                break
                        head_start = float(cs[k_edge] + (step / 2 if blade_up else -step / 2))
                        print(f"thickness: {'-' if blade_up else '+'} side {np.median(th[:k_best] if blade_up else th[k_best:]):.2f} cm "
                              f"(handle), {'+' if blade_up else '-'} side {np.median(th[k_best:] if blade_up else th[:k_best]):.2f} cm (blade)")
                g_lo = g_hi = head_start
                if guardless:
                    print(f"no guard found: the blade starts where the width changes, {head_start:.1f} cm along the axis "
                          f"(handle {haft_w:.1f} cm wide)")
                else:
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
                        # On a handle of even width the narrowest section is anywhere: then the pommel
                        # starts where the width first comes back down to the grip's, going up from its
                        # widest point.
                        for s_, w_ in between:
                            if w_ <= 1.05 * grip_w:
                                handle_bottom = s_
                                break
            print(f"grip width {grip_w:.1f} cm; handle bottom {handle_bottom:.1f} cm, pommel end {end:.1f} cm"
                  + ("" if handle_bottom > end else " (no wider pommel found: whole handle is grip)"))
            if args.type == "double" and grip_w > 0:
                # The second head or blade at the bottom end: from the widest section in the lower half of the
                # handle, up to where the width is back to the haft's (as the top head is found).
                low = [(s, w) for s, w in prof if s <= end + 0.5 * span and w > 0]
                if low:
                    s_peak, _ = max(low, key=lambda p: p[1])
                    handle_bottom = s_peak
                    for s_, w_ in prof:
                        if s_ > s_peak and 0 < w_ <= 1.3 * grip_w:
                            handle_bottom = s_
                            break
                print(f"double-ended: the lower head or blade is {end:.1f}..{handle_bottom:.1f} cm")

        # ---- part boundaries chosen by hand ----------------------------------------------------------------
        if args.splits:
            a, b, c = sorted(float(v) for v in args.splits.split(","))
            span = guard_bottom - end + (float(z.max()) - guard_bottom)
            a, b, c = (min(max(v, 0.0), span) for v in (a, b, c))
            hb, gb, gt = end + a, end + b, end + c
            frame_pos[:, 2] -= gb  # the frame's origin is the guard's start (the top of the grip)
            z = frame_pos[:, 2]
            end, handle_bottom, guard_bottom, guard_top = end - gb, hb - gb, 0.0, gt - gb
            named = False  # the parts now come from the boundaries, not the mesh names
            print(f"part boundaries chosen by hand: pommel {handle_bottom - end:.1f} cm, grip {-handle_bottom:.1f} cm, "
                  f"guard {guard_top:.1f} cm, blade {float(z.max()) - guard_top:.1f} cm")

        # ---- part lengths chosen by hand: each part stretched along the weapon on its own --------------------
        lens_before = None
        if args.part_lengths:
            want = [float(v) if v.strip() else 0.0 for v in args.part_lengths.split(",")] + [0.0] * 4
            top_ = float(z.max())
            old = [(end, handle_bottom), (handle_bottom, guard_bottom), (guard_bottom, guard_top), (guard_top, top_)]
            lens = [hi - lo for lo, hi in old]
            new_lens = [w if w > 0 and l > 0.05 else l for w, l in zip(want[:4], lens)]
            # The guard's start stays at 0: the grip and pommel grow downward, the guard and blade upward.
            nb = [-(new_lens[0] + new_lens[1]), -new_lens[1], 0.0, new_lens[2], new_lens[2] + new_lens[3]]
            zz = z.copy()
            nz = frame_nrm[:, 2].copy()
            for i, (lo, hi) in enumerate(old):
                sel = (zz >= lo - 1e-6) & (zz <= hi + 1e-6) if i == 0 else (zz > lo) & (zz <= hi + 1e-6)
                if i == 3:
                    sel = zz > lo
                k = new_lens[i] / lens[i] if lens[i] > 0.05 else 1.0
                frame_pos[sel, 2] = nb[i] + (zz[sel] - lo) * k
                nz[sel] = frame_nrm[sel, 2] / k  # a stretch along z tilts the normals toward the sides
            frame_nrm[:, 2] = nz
            frame_nrm /= np.maximum(np.linalg.norm(frame_nrm, axis=1, keepdims=True), 1e-12)
            z = frame_pos[:, 2]
            end, handle_bottom, guard_bottom, guard_top = nb[0], nb[1], 0.0, nb[3]
            lens_before = [round(l, 3) for l in lens]
            names_ = ("pommel", "grip", "guard", "blade")
            print("part lengths: " + ", ".join(f"{n} {l:.1f} -> {nl:.1f} cm" for n, l, nl in zip(names_, lens, new_lens)))

    # ---- which way a hafted head faces ----------------------------------------------------------------
    # An axe's bit (or a hammer's beak/face, the side reaching further out) goes on +x: the front when the
    # game holds it (Matthew's test 2026-11-22: a bit on -x was held facing backwards). --turn turns it round.
    if hafted and not shield and args.type not in ("flail", "crossbow"):
        hd = frame_pos[frame_pos[:, 2] > guard_top]
        if len(hd):
            reach_pos, reach_neg = float(hd[:, 0].max()), float(-hd[:, 0].min())
            # The bit side has more of the head further out: the first moment of the surface along x (area-
            # weighted triangle centres), not only the furthest point (a near-symmetric axe scored 1.152 on that).
            ht = tris[(frame_pos[tris, 2] > guard_top).all(axis=1)]
            if len(ht):
                tv = frame_pos[ht]
                ar = 0.5 * np.linalg.norm(np.cross(tv[:, 1] - tv[:, 0], tv[:, 2] - tv[:, 0]), axis=1)
                cx = tv[:, :, 0].mean(axis=1)
                moment = float((ar * cx).sum())
                spread = float((ar * np.abs(cx)).sum())
                turn = moment < -0.05 * spread and reach_neg > 2.0
            else:
                turn = reach_neg > 1.15 * reach_pos and reach_neg > 2.0
            if args.turn:
                turn = not turn
            if turn:  # half a turn round z: x and y change sign, the winding stays
                frame_pos[:, :2] *= -1
                frame_nrm[:, :2] *= -1
                print(f"head turned round so its long side ({max(reach_pos, reach_neg):.1f} cm out) faces the front"
                      + (" (--turn)" if args.turn else ""))

    # ---- which way a one-edged blade faces ----------------------------------------------------------------
    # The game's swords and knives cut with their -x side (their Tip points' Up is -X: BP_Weapon_Tool_Knife_A,
    # the arming sword's G_60 blade), and the hand holds them that way round. A one-edged blade whose edge is
    # on +x is turned half round z so the edge faces where the game swings (Matthew's test 2026-11-22: a
    # converted knife was held with its edge backwards). The edge side: the shape's (thin side), or the
    # user's choice ("the other side" swaps it).
    if not hafted and not shield and args.type not in ("flail", "crossbow", "double"):
        ztop = float(z.max())
        blade_len = ztop - guard_top
        if blade_len > 2.0:
            tv_ = frame_pos[tris]
            ar_ = 0.5 * np.linalg.norm(np.cross(tv_[:, 1] - tv_[:, 0], tv_[:, 2] - tv_[:, 0]), axis=1)
            rng_ = np.random.default_rng(1)
            pk_ = rng_.choice(len(tris), size=30000, p=ar_ / ar_.sum()) if ar_.sum() > 0 else np.zeros(0, int)
            ab_ = rng_.random((len(pk_), 2))
            ab_[ab_.sum(axis=1) > 1] = 1 - ab_[ab_.sum(axis=1) > 1]
            sm_ = tv_[pk_, 0] + ab_[:, :1] * (tv_[pk_, 1] - tv_[pk_, 0]) + ab_[:, 1:] * (tv_[pk_, 2] - tv_[pk_, 0])
            secs_ = dp.blade_sections(np.vstack([frame_pos, sm_]), guard_top + 1.0, ztop - 1.0, max(2.0, blade_len / 30.0))
            found_ = dp.edges_of(secs_) if secs_ else "double"
            want_ = args.edges or found_
            if want_ in ("single", "single_other"):
                side_ = found_ if found_.startswith("single") else "single+"
                want_ = side_ if want_ == "single" else ("single-" if side_ == "single+" else "single+")
            if want_ == "single+":  # half a turn round z: x and y change sign, the winding stays
                frame_pos[:, :2] *= -1
                frame_nrm[:, :2] *= -1
                print("one-edged blade turned round so its edge faces the front (-x, as the game's swords)")
            if want_ in ("single+", "single-"):
                args.edges = "single-"

    # ---- collision pieces -----------------------------------------------------------------------------
    if shield:
        pieces = wp.build_pieces_grid(frame_pos, tris, 3, 3)
    elif args.type == "crossbow":
        pieces = [("Head", pts, hull) for pts, hull in
                  wp._slices(frame_pos, tris, 2, np.linspace(z.min(), z.max(), max(2, int(np.ptp(z) / 15)) + 1))]
    elif args.type == "flail":
        # Staff and head in slabs of about 15 cm, the chain in slabs of about 5 cm (it bends only at the
        # game's joints, but stays slim).
        pieces = []
        tri_label = flail_label[tris[:, 0]]
        for slot, step in (("Grip", 15.0), ("Link", 5.0), ("Head", 15.0)):
            sub = tris[tri_label == slot]
            if len(sub) == 0:
                continue
            used = np.unique(sub)
            lo, hi = frame_pos[used, 2].min(), frame_pos[used, 2].max()
            k = max(1, int(math.ceil((hi - lo) / step)))
            pieces += [(slot, pts, hull) for pts, hull in wp._slices(frame_pos, sub, 2, np.linspace(lo, hi, k + 1))]
    elif named:
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
    # Materials that look the same (same texture images and factors, as when an exporter writes one
    # material per mesh) share one atlas cell, so the texture keeps its full size.
    def mat_key(m):
        if m is None:
            return None
        parts = []
        for a in ("baseColorTexture", "image", "normalTexture", "metallicRoughnessTexture", "occlusionTexture"):
            im = getattr(m, a, None)
            parts.append(None if im is None else (im.size, im.mode, hash(im.tobytes())))
        for a in ("baseColorFactor", "diffuse", "metallicFactor", "roughnessFactor"):
            v = getattr(m, a, None)
            parts.append(None if v is None else tuple(np.asarray(v, dtype=np.float64).ravel().round(4)))
        return type(m).__name__, tuple(parts)

    mats, keys, material_of_mesh = [], [], []
    for names, g in meshes:
        m = getattr(g.visual, "material", None)
        k = mat_key(m)
        if k not in keys:
            keys.append(k)
            mats.append(m)
        material_of_mesh.append(keys.index(k))
    texture_set = None
    if any(m is not None for m in mats) and not args.preview:
        def tex_size(m):
            for a in ("baseColorTexture", "image", "normalTexture", "metallicRoughnessTexture"):
                im = getattr(m, a, None)
                if im is not None:
                    return im.size
            return None
        sizes = [tex_size(m) for m in mats]
        n = len(mats)
        grid = int(math.ceil(math.sqrt(n)))
        if n == 1 and sizes[0]:
            # One material: its own proportions, longest side at most 4096 (no quality lost below that).
            w0, h0 = sizes[0]
            f = min(1.0, 4096.0 / max(w0, h0))
            cw, ch = max(1, int(round(w0 * f))), max(1, int(round(h0 * f)))
        else:
            cw = ch = min(max([max(s) for s in sizes if s] or [16]), 4096 // grid)
        maps = [material_maps(m, (cw, ch), args.metallic, args.roughness) for m in mats]
        if any(has for _, has in maps):
            Ww, Wh = grid * cw, grid * ch
            colour = np.ones((Wh, Ww, 3))
            nx, ny = np.zeros((Wh, Ww)), np.zeros((Wh, Ww))
            orm = np.dstack([np.ones((Wh, Ww)), np.full((Wh, Ww), args.roughness), np.full((Wh, Ww), args.metallic)])
            for i, ((c, x, y, o), _) in enumerate(maps):
                r0, c0 = (i // grid) * ch, (i % grid) * cw
                colour[r0:r0 + ch, c0:c0 + cw] = c
                nx[r0:r0 + ch, c0:c0 + cw] = x
                ny[r0:r0 + ch, c0:c0 + cw] = y
                orm[r0:r0 + ch, c0:c0 + cw] = o
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
                print(f"{n} materials merged into one {Ww}x{Wh} texture atlas ({grid}x{grid} cells of {cw}x{ch})")
            texture_set = (colour, nx, ny, orm)
        else:
            print("no texture maps: the mod keeps the game's material")

    wp.write_visual_obj(os.path.join(args.out, name + ".obj"), f"{name} converted by tools/model_to_weapon.py",
                        frame_pos, frame_nrm, uvs, tris)
    wp.write_collision(os.path.join(args.out, name + "_collision.obj"), name, pieces)
    if args.type == "flail":
        # One visual per moving part (the mod puts each on its own body).
        tri_label = flail_label[tris[:, 0]]
        for slot in ("Grip", "Link", "Head"):
            sub = tris[tri_label == slot]
            used = np.unique(sub)
            remap = np.full(len(frame_pos), -1)
            remap[used] = np.arange(len(used))
            wp.write_visual_obj(os.path.join(args.out, f"{name}_flail_{slot}.obj"), f"{name} flail {slot}",
                                frame_pos[used], frame_nrm[used], uvs[used], remap[sub])
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
        grams = sum(v * (WOOD if shield else grip_density if s == "Grip" else STEEL) for s, v in volume.items())
        enclosed = 0.0
        for _, g in ([] if args.preview else meshes):
            # trimesh's volume works on the triangles alone: joining the vertices first (as before) gives
            # the same number and took seconds on big models.
            enclosed += abs(float(g.volume))
        enclosed *= scale ** 3
        convex = sum(volume.values())
        if 0.2 * convex < enclosed <= convex:
            grams *= enclosed / convex
            mass_note = f"estimated: model volume {enclosed:.0f} cm3 (convex {convex:.0f}), {'wood' if shield else 'steel/grip'} densities"
        else:
            mass_note = f"estimated from convex volume {convex:.0f} cm3 (model volume {enclosed:.0f} not usable), {'wood' if shield else 'steel/grip'} densities"
        # Models are often far thicker than the real thing (stylised or blocky ones especially), so the
        # volume guess is checked against what real weapons of this type and length weigh: kept within
        # 0.6x..1.6x of that, then met halfway (geometric mean) with it.
        from_volume = grams / 1000.0
        length_cm = float(z.max() - z.min())
        if shield:  # real shields: about 10.5 kg per square metre of face (buckler 1 kg, heater 3 kg)
            w_, _, h_ = np.ptp(frame_pos, axis=0)
            typical = 10.5 * (w_ * h_ * 0.85) / 1e4
        else:
            typical = typical_mass_kg(length_cm, hafted)
        kept = min(max(from_volume, 0.6 * typical), 1.6 * typical)
        mass = round(math.sqrt(kept * typical), 3)
        mass_note += (f"; volume gives {from_volume:.2f} kg, real {'shields' if shield else 'hafted weapons' if hafted else 'swords'} of "
                      f"{length_cm:.0f} cm weigh about {typical:.2f} kg"
                      + (" (volume guess far off: the model is thicker or thinner than a real one)"
                         if kept != from_volume else ""))
    print(f"mass {mass} kg ({mass_note}); volumes cm3: " + ", ".join(f"{s} {v:.0f}" for s, v in volume.items()))

    top = float(z.max())
    numbers = {"mass_kg": float(mass), "pommel_end": round(end, 2), "handle_bottom": round(float(handle_bottom), 2),
               "handle_top": 0.0, "guard_bottom": round(float(guard_bottom), 2), "guard_top": round(float(guard_top), 2),
               "tip": round(top, 2), "hafted": 1 if hafted else 0, "double": 1 if args.type == "double" else 0,
               "hand_slide": 1 if args.hand_slide else 0, "shield": 1 if shield else 0,
               "flail": 1 if args.type == "flail" else 0, "crossbow": 1 if args.type == "crossbow" else 0}
    if args.held:
        numbers["held"] = {"one": 1, "two": 2, "spear": 3}[args.held]
    if shield:
        w_, t_, h_ = np.ptp(frame_pos, axis=0)
        numbers.update(width=round(float(w_), 2), thickness=round(float(t_), 2), height=round(float(h_), 2))
    # ---- balance and damage points --------------------------------------------------------------------
    dens = {s: (WOOD if shield else (HAFT if hafted else GRIP) if s == "Grip" else STEEL) for s in volume}
    wsum = sum(hull.volume * dens[s] for s, _, hull in pieces)
    def hull_cz(h):  # centroid height of a convex hull: tetrahedra from an inside point to each face
        P = h.points
        a = P[h.vertices].mean(axis=0)
        tri = P[h.simplices]
        v = np.abs(np.einsum("ij,ij->i", tri[:, 0] - a, np.cross(tri[:, 1] - a, tri[:, 2] - a))) / 6.0
        cz = (tri[:, :, 2].sum(axis=1) + a[2]) / 4.0
        return float((v * cz).sum() / max(v.sum(), 1e-12))
    com = sum(hull_cz(hull) * hull.volume * dens[s] for s, _, hull in pieces) / max(wsum, 1e-9)
    numbers["balance_model"] = round(com, 2)
    numbers["balance"] = round(args.balance_cm if args.balance_cm is not None else com, 2)
    over = {"edges": args.edges, "head": args.head_kind, "sEdge": args.edge_sharp, "sTip": args.tip_sharp,
            "rig": args.rigidity}
    over = {k: (int(round(v)) if isinstance(v, float) else v) for k, v in over.items() if v is not None}
    # The shape as vertices plus points spread over the surface (area-weighted, fixed seed), so a
    # low-poly model with big flat faces is measured as well as a dense one.
    tri_v = frame_pos[tris]
    area = 0.5 * np.linalg.norm(np.cross(tri_v[:, 1] - tri_v[:, 0], tri_v[:, 2] - tri_v[:, 0]), axis=1)
    rng = np.random.default_rng(1)
    pick = rng.choice(len(tris), size=30000, p=area / area.sum()) if area.sum() > 0 else np.zeros(0, int)
    ab = rng.random((len(pick), 2))
    flipab = ab.sum(axis=1) > 1
    ab[flipab] = 1 - ab[flipab]
    tv = tri_v[pick]
    samples = tv[:, 0] + ab[:, :1] * (tv[:, 1] - tv[:, 0]) + ab[:, 1:] * (tv[:, 2] - tv[:, 0])
    if args.preview:
        dpts, dinfo = [], dict(kind="preview")  # worked out at Install
    elif args.type == "crossbow":
        dpts, dinfo = [], dict(kind="crossbow")  # like the game's: no Tip points, a bash is blunt
    elif args.type == "flail":
        head_sel = np.concatenate([flail_label == "Head", flail_label[tris[pick, 0]] == "Head"])
        dpts, dinfo = dp.flail_points(np.vstack([frame_pos, samples])[head_sel], numbers, over)
    else:
        dpts, dinfo = (dp.shield_points(np.vstack([frame_pos, samples]), numbers, over) if shield else
                       dp.compute(np.vstack([frame_pos, samples]), numbers, hafted, over))
    dp.write(os.path.join(args.out, name + "_points.txt"), dpts)
    print(f"balance {numbers['balance']:.1f} cm above the grip (model {com:.1f}); damage points: {len(dpts)} "
          f"({', '.join(f'{k} {v}' for k, v in dinfo.items())})")
    wp.write_numbers(os.path.join(args.out, name + ".txt"), f"{name}: written by tools/model_to_weapon.py", numbers)
    json.dump({"source": os.path.basename(args.model), "scale": scale_note, "mass_kg": mass, "mass_note": mass_note,
               "zones_cm": numbers, "volumes_cm3": {s: round(v, 1) for s, v in volume.items()},
               "parts_by_name": named, "lengths_before_stretch_cm": lens_before, "preview": bool(args.preview),
               "type": "shield" if shield else args.type if args.type in ("flail", "crossbow") else "hafted" if hafted else "sword", "damage": dinfo, "pieces": [{"slot": s, "points": len(p)} for s, p, _ in pieces]},
              open(os.path.join(args.out, name + ".json"), "w"), indent=2)
    if shield:
        print(f"shield {numbers['width']:.1f} x {numbers['height']:.1f} cm; {len(pieces)} collision pieces")
    elif args.type == "crossbow":
        print(f"crossbow {top - end:.1f} cm long (butt {end:.1f}, bow at 0, front {top:.1f} cm); {len(pieces)} pieces; "
              "in the game the crossbow's own body, weight and string are used (the mass above is not)")
    elif hafted:
        print(f"length {top - end:.1f} cm: butt end {end:.1f}, haft {handle_bottom:.1f}..0, head 0..{top:.1f} cm; "
              f"{len(pieces)} collision pieces")
    else:
        print(f"length {top - end:.1f} cm: pommel end {end:.1f}, grip {handle_bottom:.1f}..0, guard 0..{guard_top:.1f}, "
              f"blade to {top:.1f} cm; {len(pieces)} collision pieces")


if __name__ == "__main__":
    main()
