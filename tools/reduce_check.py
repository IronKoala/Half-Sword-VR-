"""Developer check for Reduce detail: runs several reducers on one model and reports, per variant and per mesh,
triangles, time, how far the reduced surface strays from the original (shape, mm), texture error (texture
pixels at 2048), and whether any mesh lost part of its extent (the f9926fb fault: Spit 2.0's guard deleted).
Optional pictures of each variant. Not used by the converter.

Measures are rough (the closest original triangle is searched among the 16 with the nearest centres; a seam
can read as texture error), so compare variants against each other and look at the pictures.

python reduce_check.py MODEL.glb --target 150000 [--variants a,b,...] [--pictures OUTDIR]"""
import argparse
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import model_to_weapon as mtw  # noqa: E402


def fast(g, want, agg=7, border=True):
    import trimesh
    import fast_simplification
    v = np.asarray(g.vertices, dtype=np.float64)
    f = np.asarray(g.faces, dtype=np.int64)
    p2, f2, col = fast_simplification.simplify(v, f, target_count=max(want, 100), agg=agg, return_collapses=True,
                                               preserve_border=border)
    kept = np.setdiff1d(np.arange(len(v)), col[:, 1]) if len(col) else np.arange(len(v))
    if len(kept) != len(p2):
        return None
    uv = getattr(g.visual, "uv", None)
    h = trimesh.Trimesh(p2, np.asarray(f2, dtype=np.int64), vertex_normals=np.asarray(g.vertex_normals)[kept],
                        process=False)
    if uv is not None and len(uv) == len(v):
        h.visual = trimesh.visual.TextureVisuals(uv=np.asarray(uv)[kept])
    return h


def meshlab(g, want, weld=True, texture=True, **opts):
    import tempfile
    import shutil
    import trimesh
    import pymeshlab
    from PIL import Image
    v = np.asarray(g.vertices, dtype=np.float64)
    f = np.asarray(g.faces, dtype=np.int64)
    uv = np.asarray(g.visual.uv, dtype=np.float64)
    if weld:
        key, inv = np.unique(v, axis=0, return_inverse=True)
        F = inv.ravel()[f] + 1
    else:
        key, F = v, f + 1
    uk, uinv = np.unique(uv, axis=0, return_inverse=True)
    T = uinv.ravel()[f] + 1
    tmp = tempfile.mkdtemp(prefix="hs_rc_")
    try:
        Image.new("RGB", (4, 4)).save(os.path.join(tmp, "t.png"))
        open(os.path.join(tmp, "m.mtl"), "w").write("newmtl m\nmap_Kd t.png\n")
        with open(os.path.join(tmp, "m.obj"), "w") as fo:
            fo.write("mtllib m.mtl\n")
            np.savetxt(fo, key, fmt="v %.7f %.7f %.7f")
            np.savetxt(fo, uk, fmt="vt %.8f %.8f")
            fo.write("usemtl m\n")
            np.savetxt(fo, np.stack([F[:, 0], T[:, 0], F[:, 1], T[:, 1], F[:, 2], T[:, 2]], 1), fmt="f %d/%d %d/%d %d/%d")
        ms = pymeshlab.MeshSet()
        ms.load_new_mesh(os.path.join(tmp, "m.obj"))
        base = dict(targetfacenum=int(want), qualitythr=0.3, preserveboundary=True, boundaryweight=1.0,
                    optimalplacement=True, preservenormal=True, planarquadric=False)
        base.update(opts)
        if texture:
            ms.meshing_decimation_quadric_edge_collapse_with_texture(extratcoordw=1.0, **base)
        else:
            ms.meshing_decimation_quadric_edge_collapse(preservetopology=True, **base)
        m = ms.current_mesh()
        P, F2 = m.vertex_matrix(), m.face_matrix().astype(np.int64)
        W = m.wedge_tex_coord_matrix().reshape(-1, 2) if texture else None
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if W is None:
        h = trimesh.Trimesh(P, F2, process=False)
        return h
    corner = np.concatenate([F2.reshape(-1, 1), np.round(W * 1e8).astype(np.int64)], 1)
    _, first, inv2 = np.unique(corner, axis=0, return_index=True, return_inverse=True)
    h = trimesh.Trimesh(P[F2.reshape(-1)[first]], inv2.reshape(-1, 3), process=False)
    h.visual = trimesh.visual.TextureVisuals(uv=W[first])
    return h


VARIANTS = {
    "fast7": lambda g, w: fast(g, w, 7),
    "fast3": lambda g, w: fast(g, w, 3),
    "fast7_4x_ml_noweld": lambda g, w: _two(g, w, lambda h, ww: meshlab(h, ww, weld=False)),
    "ml_noweld": lambda g, w: meshlab(g, w, weld=False),
    "ml_weld_f9926fb": lambda g, w: _two(g, w, lambda h, ww: meshlab(h, ww, weld=True, planarquadric=True)),
}


def _two(g, want, second):
    h = fast(g, want * 4, 7) if want * 4 < len(g.faces) else g
    return second(h or g, want) if len(h.faces) > want * 1.02 else h


def measure(g, h, samples=20000):
    """Shape: distance from points on the reduced surface to the original surface (mm). Texture: their UV against
    the original's UV at that closest point (texture pixels at 2048; seams count as no error when either side
    fits: the UV of the original triangle hit is used)."""
    import trimesh
    pts, fi = trimesh.sample.sample_surface(h, samples, seed=1)
    from scipy.spatial import cKDTree
    otris = np.asarray(g.triangles)
    k = min(16, len(otris))
    _, cand = cKDTree(otris.mean(1)).query(pts, k=k)
    cand = cand.reshape(len(pts), k)
    best_d = np.full(len(pts), np.inf)
    cp = np.zeros_like(pts)
    tid = np.zeros(len(pts), dtype=np.int64)
    for j in range(k):  # nearest of the 16 triangles whose centres are closest (no rtree needed)
        c = trimesh.triangles.closest_point(otris[cand[:, j]], pts)
        d = np.linalg.norm(c - pts, axis=1)
        better = d < best_d
        best_d[better], cp[better], tid[better] = d[better], c[better], cand[better, j]
    dist = best_d
    shape = dist * 1000.0
    tex = np.zeros(len(pts))
    huv = getattr(h.visual, "uv", None)
    guv = getattr(g.visual, "uv", None)
    if huv is not None and guv is not None and len(huv) == len(h.vertices):
        tri = h.faces[fi]
        uv = np.einsum("ij,ijk->ik", trimesh.triangles.points_to_barycentric(h.vertices[tri], pts), np.asarray(huv)[tri])
        otri = g.faces[tid]
        ouv = np.einsum("ij,ijk->ik", trimesh.triangles.points_to_barycentric(g.vertices[otri], cp), np.asarray(guv)[otri])
        tex = np.nan_to_num(np.linalg.norm(uv - ouv, axis=1) * 2048, nan=0.0)  # degenerate triangles
    lost = np.abs(np.asarray(g.bounds) - np.asarray(h.bounds)).max() * 1000.0
    return shape, tex, lost


def picture(meshes, path):
    from PIL import Image
    import trimesh
    import weapon_converter_app as app  # renderer the converter uses
    pos = np.vstack([np.asarray(g.vertices) for _, g in meshes])
    nrm = np.vstack([np.asarray(g.vertex_normals) for _, g in meshes])
    off = np.cumsum([0] + [len(g.vertices) for _, g in meshes])
    tris = np.vstack([np.asarray(g.faces) + o for (_, g), o in zip(meshes, off)])
    uv = None
    zones = [("Head", pos[:, 2].min(), pos[:, 2].max())]
    s = app.view_scale(pos, 1600, (400, 120))
    a = app.render_view(pos, nrm, uv, tris, None, zones, "flat", 1600, 400, s)
    b = app.render_view(pos, nrm, uv, tris, None, zones, "edge", 1600, 120, s)
    im = Image.new("RGB", (1600, 520))
    im.paste(a, (0, 0))
    im.paste(b, (0, 400))
    im.save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("model")
    ap.add_argument("--target", type=int, default=150000)
    ap.add_argument("--variants", default=",".join(VARIANTS))
    ap.add_argument("--pictures")
    a = ap.parse_args()
    meshes = mtw.load_model(a.model)
    total = sum(len(g.faces) for _, g in meshes)
    print(f"{a.model}: {total:,} triangles in {len(meshes)} meshes")
    if a.pictures:
        os.makedirs(a.pictures, exist_ok=True)
        picture(meshes, os.path.join(a.pictures, "full.png"))
    for name in a.variants.split(","):
        fn = VARIANTS[name]
        t0 = time.time()
        out, rows = [], []
        for names, g in meshes:
            want = max(int(round(a.target * len(g.faces) / total)), 100)
            h = g
            if len(g.faces) >= 200 and want < len(g.faces):
                try:
                    h = fn(g, want) or g
                except Exception as e:  # noqa: BLE001
                    print(f"  {name} {names[0]}: failed: {e}")
            out.append((names, h))
            rows.append((names[0], g, h))
        dt = time.time() - t0
        tris = sum(len(h.faces) for _, h in out)
        print(f"== {name}: {tris:,} triangles, {dt:.1f} s")
        worst = []
        for nm, g, h in rows:
            if h is g or len(h.faces) == len(g.faces):
                print(f"   {nm[:44]:44s} {len(g.faces):>9,} kept")
                continue
            sh, tx, lost = measure(g, h)
            flag = "  <-- LOST EXTENT" if lost > 2.0 else ""
            print(f"   {nm[:44]:44s} {len(g.faces):>9,} -> {len(h.faces):>8,}  shape mm p99 {np.percentile(sh, 99):.2f} max {sh.max():.2f}"
                  f"  tex px median {np.median(tx):.1f} p99 {np.percentile(tx, 99):.1f} >8px {100 * (tx > 8).mean():.2f}%"
                  f"  extent change {lost:.1f} mm{flag}")
            worst.append(sh.max())
        if a.pictures:
            picture(out, os.path.join(a.pictures, f"{name}.png"))


if __name__ == "__main__":
    main()
