"""A sword's own scabbard model, made ready for the HalfSwordCustomWeapon mod (pack.txt scabbard=own).

The scabbard model is read like a shield by model_to_weapon.py (any format it reads; its longest axis
becomes z, its widest cross direction x, textures baked the same way), then laid along the converted
sword in the sword's model frame (tools/weapon_parts.py: cm, z along the weapon, 0 at the top of the
grip, x = the blade's wide direction):
  - scaled so it is the blade's length plus 6 cm (a model's own units are often wrong),
  - its mouth (the wider end; --flip for the other end) at the top of the guard + 0.5 cm, the rest
    running up over the blade,
  - its width across the blade's width.
Writes <name>_scabbard.obj, <name>_scabbard_collision.obj (UCX_Scabbard_<n>, slabs of about 15 cm) and,
when the model has textures, <name>_scabbard_BaseColor/_Normal/_ORM.png into the weapon's folder.

    python scabbard.py <scabbard model> <converted weapon folder> <weapon name> [--flip] [--units m]
"""
import argparse
import os
import shutil
import sys
import tempfile

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import model_to_weapon  # noqa: E402
import weapon_parts as wp  # noqa: E402


def read_numbers(path):
    out = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        if "=" in line and not line.startswith((";", "#")):
            k, v = line.split("=", 1)
            try:
                out[k.strip()] = float(v)
            except ValueError:
                pass
    return out


def read_obj(path):
    v, vn, vt, f = [], [], [], []
    for line in open(path):
        if line.startswith("v "):
            v.append([float(x) for x in line[2:].split()[:3]])
        elif line.startswith("vn "):
            vn.append([float(x) for x in line[3:].split()[:3]])
        elif line.startswith("vt "):
            vt.append([float(x) for x in line[3:].split()[:2]])
        elif line.startswith("f "):
            f.append([int(p.split("/")[0]) - 1 for p in line[2:].split()[:3]])
    pos = np.array(v, dtype=np.float64)
    nrm = np.array(vn, dtype=np.float64) if vn else np.zeros_like(pos)
    uv = np.array(vt, dtype=np.float64) if vt else np.zeros((len(pos), 2))
    return pos, nrm, uv, np.array(f, dtype=np.int64)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("model")
    ap.add_argument("weapon_dir")
    ap.add_argument("name")
    ap.add_argument("--flip", action="store_true", help="the mouth is the narrower end")
    ap.add_argument("--units", choices=sorted(model_to_weapon.UNITS_CM))
    args = ap.parse_args(argv)

    nums = read_numbers(os.path.join(args.weapon_dir, args.name + ".txt"))
    if "tip" not in nums or "guard_top" not in nums:
        sys.exit(f"{args.name}.txt has no tip/guard_top: convert the sword first")
    guard_top, tip = nums["guard_top"], nums["tip"]
    blade = tip - guard_top
    if blade < 5:
        sys.exit(f"the blade is only {blade:.1f} cm: no scabbard")
    want_len = blade + 6.0

    # The weapon's blade: which way is its width (x or y in the weapon frame)?
    wpos, _, _, _ = read_obj(os.path.join(args.weapon_dir, args.name + ".obj"))
    bl = wpos[wpos[:, 2] > guard_top + 0.2 * blade]
    wide_x = len(bl) == 0 or np.ptp(bl[:, 0]) >= np.ptp(bl[:, 1])

    tmp = tempfile.mkdtemp(prefix="scabbard_")
    try:
        argv2 = [args.model, tmp, "--name", "s", "--type", "shield", "--length-cm", f"{want_len:.3f}"]
        if args.units:
            argv2 += ["--units", args.units]
        model_to_weapon.main(argv2)
        pos, nrm, uv, tris = read_obj(os.path.join(tmp, "s.obj"))
        # The shield frame: z = length, x = width, y = thickness, origin at the middle. Mouth = the end
        # whose last 8 % is wider (a chape narrows), unless --flip.
        z = pos[:, 2]
        lo, hi = z.min(), z.max()
        band = 0.08 * (hi - lo)

        def width(sel):
            return float(np.ptp(pos[sel, 0])) if sel.any() else 0.0
        mouth_low = width(z < lo + band) >= width(z > hi - band)
        if args.flip:
            mouth_low = not mouth_low
        if not mouth_low:  # turn it end over end round x: y and z change sign, the winding stays
            pos[:, 1:] *= -1
            nrm[:, 1:] *= -1
        if not wide_x:  # its width across the blade's: turn round z by 90 degrees
            pos = pos[:, [1, 0, 2]] * np.array([-1, 1, 1])
            nrm = nrm[:, [1, 0, 2]] * np.array([-1, 1, 1])
        pos[:, 2] += guard_top + 0.5 - pos[:, 2].min()
        pos[:, 0] -= 0.5 * (pos[:, 0].min() + pos[:, 0].max())
        pos[:, 1] -= 0.5 * (pos[:, 1].min() + pos[:, 1].max())

        out = os.path.join(args.weapon_dir, args.name + "_scabbard")
        wp.write_visual_obj(out + ".obj", f"{args.name} scabbard", pos, nrm, uv, tris)
        n = max(2, int(np.ceil(np.ptp(pos[:, 2]) / 15.0)))  # slabs of about 15 cm, so a curve is followed
        cuts = np.linspace(pos[:, 2].min(), pos[:, 2].max(), n + 1)
        pieces = [("Scabbard", pts, hull) for pts, hull in wp._slices(pos, tris, 2, cuts)]
        if not pieces:
            sys.exit("no collision could be made from the scabbard model")
        wp.write_collision(out + "_collision.obj", f"{args.name} scabbard", pieces)
        for m in ("_BaseColor.png", "_Normal.png", "_ORM.png"):
            src = os.path.join(tmp, "s" + m)
            if os.path.exists(src):
                shutil.copy2(src, out + m)
        w, t, L = np.ptp(pos, axis=0)
        print(f"scabbard: {L:.1f} cm long (blade {blade:.1f} cm), {w:.1f} x {t:.1f} cm; mouth at "
              f"{pos[:, 2].min():.1f} cm (guard top {guard_top:.1f}); {len(pieces)} collision pieces")
        print("RESULT scabbard done")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
