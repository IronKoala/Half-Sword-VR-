"""Shared pieces of the weapon exporters (bs_weapon_export.py, model_to_weapon.py).

Everything here works in the weapon frame the HalfSwordCustomWeapon mod reads (Unreal axes, cm):
  origin = top end of the main handle (the guard end), on the handle axis
  +Z     = along the handle towards the blade
  +X     = the blade's wide direction (edge), +Y = its thin direction
Triangles are counter-clockwise seen from outside (OBJ convention; the mod reverses them for Unreal).
"""
import math

import numpy as np
from scipy.spatial import ConvexHull


def write_visual_obj(path, title, pos, nrm, uvs, tris):
    """pos, nrm: (n, 3) in the weapon frame; uvs: (n, 2) with v pointing up the image (OBJ/Unity/glTF-
    flipped convention: the mod uses 1 - v); tris: (m, 3) counter-clockwise from outside."""
    with open(path, "w", newline="\n") as f:
        f.write(f"# {title}; Unreal axes, cm, weapon frame\n")
        for p in pos:
            f.write("v %.4f %.4f %.4f\n" % tuple(p))
        for n in nrm:
            f.write("vn %.5f %.5f %.5f\n" % tuple(n))
        for uv in uvs:
            f.write("vt %.5f %.5f\n" % (uv[0], uv[1]))
        for a, b, c in tris:
            f.write(f"f {a+1}/{a+1}/{a+1} {b+1}/{b+1}/{b+1} {c+1}/{c+1}/{c+1}\n")


def width_profile(pos, lo, hi, step):
    """Width across the edge direction (X) of each slice [s, s + step) from lo to hi."""
    z = pos[:, 2]
    out = []
    for s in np.arange(lo, hi, step):
        sel = pos[(z >= s) & (z < s + step)]
        out.append((float(s), float(np.ptp(sel[:, 0])) if len(sel) else 0.0))
    return out


def find_guard(pos, handle_bottom, search_lo=-5.0, search_hi=20.0):
    """The guard: near the handle's top end, the run of slices much wider than the grip.
    Returns (guard_bottom, guard_top, grip_width)."""
    widths = [w for _, w in width_profile(pos, handle_bottom, -2.0, 1.0) if w > 0]
    grip_width = float(np.median(widths)) if widths else 0.0
    wide = [s for s, w in width_profile(pos, search_lo, search_hi, 0.25) if w > 2.0 * grip_width]
    if not wide:
        return 0.0, 0.0, grip_width
    guard_bottom, guard_top = wide[0], wide[0] + 0.25
    for s in wide:
        if s <= guard_top + 0.5:
            guard_top = s + 0.25
    return guard_bottom, guard_top, grip_width


def section_widths(pos, tris, lo, hi, step):
    """Width across X of the model's cross-section at each z = s from lo to hi, from where its edges
    cross that plane (works on low-poly models whose vertices are far apart)."""
    edges = _edges(tris)
    z = pos[:, 2]
    za, zb = z[edges[:, 0]], z[edges[:, 1]]
    out = []
    for c in np.arange(lo, hi, step):
        cross = (za - c) * (zb - c) < 0
        if not cross.any():
            out.append((float(c), 0.0))
            continue
        pa, pb = pos[edges[cross, 0]], pos[edges[cross, 1]]
        t = ((c - za[cross]) / (zb[cross] - za[cross]))[:, None]
        pts = pa + (pb - pa) * t
        out.append((float(c), float(np.ptp(pts[:, 0]))))
    return out


def _edges(tris):
    e = set()
    for a, b, c in tris:
        for u, w in ((a, b), (b, c), (c, a)):
            e.add((min(u, w), max(u, w)))
    return np.array(sorted(e))


def build_pieces(pos, tris, handle_bottom, guard_bottom, guard_top, slice_cm=15.0):
    """Convex collision pieces: pommel below the handle, the grip, the guard, then the blade in
    slices so curves are kept. Returns [(slot, points, hull)]."""
    z = pos[:, 2]
    edges = _edges(tris)

    def plane_points(c):
        # Where the mesh's edges cross the plane z = c (so a slice keeps its full outline even when
        # the model has no vertices near its ends).
        za, zb = z[edges[:, 0]], z[edges[:, 1]]
        cross = (za - c) * (zb - c) < 0
        pa, pb = pos[edges[cross, 0]], pos[edges[cross, 1]]
        t = ((c - za[cross]) / (zb[cross] - za[cross]))[:, None]
        return pa + (pb - pa) * t

    pieces = []

    def add_piece(slot, lo, hi):
        if hi <= lo:
            return
        sel = np.vstack([pos[(z >= lo) & (z <= hi)], plane_points(lo), plane_points(hi)])
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
    n_slices = max(1, int(math.ceil((top - guard_top) / slice_cm)))
    step = (top - guard_top) / n_slices
    for i in range(n_slices):
        add_piece("Head", guard_top + i * step, guard_top + (i + 1) * step)
    return pieces


def _slices(pos, tris, coord, cuts):
    """Convex hulls of a mesh cut into slabs along one coordinate (0 x, 1 y, 2 z) at the given cut
    values; each slab keeps its outline where the mesh's edges cross the cut planes."""
    if len(tris) == 0:
        return []
    edges = _edges(tris)
    c = pos[:, coord]
    ca, cb = c[edges[:, 0]], c[edges[:, 1]]

    def plane_points(v):
        cross = (ca - v) * (cb - v) < 0
        pa, pb = pos[edges[cross, 0]], pos[edges[cross, 1]]
        t = ((v - ca[cross]) / (cb[cross] - ca[cross]))[:, None]
        return pa + (pb - pa) * t

    used = np.unique(tris)
    out = []
    for lo, hi in zip(cuts[:-1], cuts[1:]):
        inside = used[(c[used] >= lo) & (c[used] <= hi)]
        sel = np.vstack([pos[inside], plane_points(lo), plane_points(hi)])
        if len(sel) < 4:
            continue
        try:
            hull = ConvexHull(sel)
        except Exception:
            continue
        out.append((sel[hull.vertices], hull))
    return out


def build_pieces_from_parts(pos, tris, part_of_vertex, slice_cm=15.0):
    """Collision pieces when the model's meshes are its parts: Pommel and Grip one hull each, the
    Guard cut in three across the edge direction (so swept quillons don't fill the grip), the blade
    ("Head") cut every slice_cm along its length."""
    pieces = []
    tri_part = part_of_vertex[tris[:, 0]]
    for slot in ("Pommel", "Grip", "Guard", "Head"):
        sub = tris[tri_part == slot]
        if len(sub) == 0:
            continue
        used = np.unique(sub)
        if slot == "Head":
            lo, hi = pos[used, 2].min(), pos[used, 2].max()
            n = max(1, int(math.ceil((hi - lo) / slice_cm)))
            hulls = _slices(pos, sub, 2, np.linspace(lo, hi, n + 1))
        elif slot == "Guard":
            lo, hi = pos[used, 0].min(), pos[used, 0].max()
            hulls = _slices(pos, sub, 0, np.linspace(lo, hi, 4))
        else:
            lo, hi = pos[used, 2].min(), pos[used, 2].max()
            hulls = _slices(pos, sub, 2, np.array([lo, hi]))
        pieces += [(slot, pts, hull) for pts, hull in hulls]
    return pieces


def write_collision(path, title, pieces):
    with open(path, "w", newline="\n") as f:
        f.write(f"# {title} convex collision pieces (UCX_<slot>_<n>)\n")
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


def write_numbers(path, title, values):
    """Flat key=value lines for the mod (it has no JSON reader)."""
    with open(path, "w", newline="\n") as f:
        f.write(f"; {title}: centimetres in the weapon frame\n")
        for k, v in values.items():
            if v is not None:
                f.write(f"{k}={v}\n")


# ---- textures -----------------------------------------------------------------------------------
# Unreal-ready maps next to the model:
#   <name>_BaseColor.png  colour (sRGB)
#   <name>_Normal.png     tangent-space normal, DirectX style (green = -Y)
#   <name>_ORM.png        R = ambient occlusion, G = roughness, B = metallic
# Normal and ORM are written gamma-encoded (linear -> sRGB) because the mod loads every file as an
# sRGB texture; the GPU's sRGB decode then gives back the linear values.
def lin_to_srgb(x):
    x = np.clip(x, 0, 1)
    return np.where(x <= 0.0031308, x * 12.92, 1.055 * np.power(x, 1 / 2.4) - 0.055)


def save_rgb(path, a, encode):
    from PIL import Image
    a = np.clip(a, 0, 1)
    if encode:
        a = lin_to_srgb(a)
    Image.fromarray((a * 255 + 0.5).astype(np.uint8), "RGB").save(path)
    print(f"wrote {path.replace(chr(92), '/').split('/')[-1]} {a.shape[1]}x{a.shape[0]}")


def save_normal_from_opengl_xy(path, x, y):
    """x, y in -1..1 of an OpenGL-style (+Y up) tangent-space map -> DirectX-style PNG."""
    zz = np.sqrt(np.clip(1 - x * x - y * y, 0, 1))
    save_rgb(path, np.dstack([x * 0.5 + 0.5, -y * 0.5 + 0.5, zz * 0.5 + 0.5]), True)


def save_orm(path, occlusion, roughness, metallic):
    save_rgb(path, np.dstack([occlusion, roughness, metallic]), True)
