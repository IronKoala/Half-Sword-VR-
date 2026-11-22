"""Damage points for a converted weapon, from the model's own shape.

Half Sword decides cut, stab and blunt only from "Tip" scene components under the part that was hit
(decompile notes HalfSwordDecomp\\defaults\\weapon_damage_points.md):
  - the point nearest the impact is used (no radius);
  - its Up axis (+Z) is the way the cutting edge faces; its Forward axis (+X) is the thrust direction;
  - its tags carry the numbers: sTipNNN, sEdgeNNN (sharpness 0-100), dSharpN, dBluntN (dismemberment
    levels), rigNNN (rigidity x100), plus the flags Double (both sides cut), Piercing (can stick in),
    NoStab.
So a converted weapon gets its own points here instead of borrowing the carrier weapon's: along a
blade (following its curve) with the edge direction measured from the blade's cross-sections, one at
the point with a thrust sharpness from the tip's angle, blunt points on guard and pommel, and for
hafted weapons points chosen from the head's shape (spear, axe, hammer/mace, beak or spike).

Frame (as model_to_weapon.py writes it): x = across the blade (the edges), y = the blade's thickness,
z = along the weapon, 0 at the top of the grip, cm.

Written as <name>_points.txt, one point per line:
    <slot> x y z  fx fy fz  ux uy uz  sTip sEdge dSharp dBlunt rig  [flags...]
slot = the model part it belongs to (Head, Guard, Grip, Pommel). A point with the flag BASE is that part's
"Base" (where a stuck blade's slide depth is measured from), not a damage point.
Cutting and piercing points carry BoxCollision too: without it a stuck cut gathers no markers and cannot
dismember (decompile: Constraint_Weapon_Stuck_BP STUCK @11282, Dismemberment Check @89).
"""
import math

import numpy as np

PRESETS = {
    # vanilla values (weapon_damage_points.md §3), used as the starting point for each kind
    "blade_double": dict(sTip=98, sEdge=90, rig=80),
    "blade_single": dict(sTip=80, sEdge=95, rig=75),
    "spear": dict(sTip=75, sEdge=45, rig=75),
    "axe": dict(sTip=0, sEdge=85, rig=105),
    "blunt": dict(sTip=5, sEdge=0, rig=125),
}


def _unit(v):
    n = float(np.linalg.norm(v))
    return v / n if n > 1e-9 else v


def tip_sharpness(pos, top, center_x):
    """Thrust sharpness from the point's angle: the half width 5 cm below the tip against 5 cm.
    A needle point (<= 12 deg) gives 99, an arming sword's (~25 deg) ~85, a round end 30."""
    near = pos[(pos[:, 2] > top - 5.5) & (pos[:, 2] < top - 4.5)]
    if len(near) < 2:
        return 85, None
    half = 0.5 * float(np.ptp(near[:, 0]))
    angle = math.degrees(math.atan2(half, 5.0))
    return int(max(30, min(99, round(100 - (angle - 12) * 1.2)))), angle


def blade_sections(pos, lo, hi, step):
    """(z, centre x, half width, edge thickness on -x side, on +x side) of slabs along the blade."""
    out = []
    z = lo
    while z <= hi:
        s = pos[np.abs(pos[:, 2] - z) < 0.6 * step]
        if len(s) >= 4:
            x0, x1 = float(s[:, 0].min()), float(s[:, 0].max())
            c, h = 0.5 * (x0 + x1), 0.5 * (x1 - x0)
            band = 0.25 * h
            lo_side = s[s[:, 0] < x0 + band]
            hi_side = s[s[:, 0] > x1 - band]
            t_lo = float(np.ptp(lo_side[:, 1])) if len(lo_side) > 1 else 0.0
            t_hi = float(np.ptp(hi_side[:, 1])) if len(hi_side) > 1 else 0.0
            out.append((z, c, h, t_lo, t_hi))
        z += step
    return out


def edges_of(sections):
    """'double', or 'single+' / 'single-' (the thin side is the edge): a spine is clearly thicker."""
    lo = np.median([s[3] for s in sections]) if sections else 0
    hi = np.median([s[4] for s in sections]) if sections else 0
    if max(lo, hi) <= 0:
        return "double"
    if min(lo, hi) < 0.6 * max(lo, hi):
        return "single+" if hi < lo else "single-"
    return "double"


def point(slot, p, fwd, up, sTip, sEdge, dSharp, dBlunt, rig, flags=()):
    return dict(slot=slot, p=[float(v) for v in p], f=[float(v) for v in _unit(np.asarray(fwd, float))],
                u=[float(v) for v in _unit(np.asarray(up, float))], sTip=int(sTip), sEdge=int(sEdge),
                dSharp=int(dSharp), dBlunt=int(dBlunt), rig=int(rig), flags=list(flags))


def base_point(slot, p):
    """The part's "Base": a stuck point slides in up to its distance from the Tip + 5 cm."""
    return dict(slot=slot, p=[float(v) for v in p], f=[0.0, 0.0, 1.0], u=[-1.0, 0.0, 0.0], sTip=0, sEdge=0, dSharp=0,
                dBlunt=0, rig=0, flags=["BASE"])


def sword_points(pos, numbers, over):
    guard_top, tip = numbers["guard_top"], numbers["tip"]
    blade_len = tip - guard_top
    step = max(2.0, blade_len / 30.0)
    secs = blade_sections(pos, guard_top + 1.0, tip - 1.0, step)
    if not secs:
        return [], {}
    found = edges_of(secs)
    kind = over.get("edges") or found
    if kind in ("single", "single_other"):  # one edge, its side from the shape (or the other one)
        side_found = found if found.startswith("single") else "single+"
        kind = side_found if kind == "single" else ("single-" if side_found == "single+" else "single+")
    double = kind == "double"
    side = 1.0 if kind == "single+" else -1.0
    preset = PRESETS["blade_double" if double else "blade_single"]
    sTip, angle = tip_sharpness(pos, tip, secs[-1][1])
    sTip = over.get("sTip", sTip)
    sEdge = over.get("sEdge", preset["sEdge"])
    rig = over.get("rig", preset["rig"])
    dSharp = 2 if blade_len < 50 else 4 if blade_len >= 75 else 3
    blunt = kind == "none"
    flags = [] if blunt else (["Piercing", "BoxCollision"] + (["Double"] if double else []))
    zs = np.array([s[0] for s in secs])
    cs = np.array([s[1] for s in secs])

    def centre(z):
        return float(np.interp(z, zs, cs))

    def tangent(z):
        a, b = max(guard_top, z - 3.0), min(tip, z + 3.0)
        return _unit(np.array([centre(b) - centre(a), 0.0, b - a]))

    pts = []
    n = max(2, int(round(blade_len / 12.0)))
    for i in range(n):
        z = guard_top + blade_len * (i + 0.5) / (n + 0.5)  # spread along the blade, the last well below the tip
        f = tangent(z)
        up = np.cross([0.0, 1.0, 0.0], f) * side  # +x side for a forward along +z
        pts.append(point("Head", [centre(z), 0.0, z], f, up, 0, 0 if blunt else sEdge, dSharp, 1 if blunt else 0,
                         rig, flags))
    zt = tip - 0.1
    f = tangent(tip - 1.0)
    pts.append(point("Head", [centre(tip - 1.0), 0.0, zt], f, np.cross([0.0, 1.0, 0.0], f) * side, sTip,
                     0 if blunt else sEdge, dSharp, 1 if blunt else 0, rig, flags))
    # guard and pommel: blunt
    if numbers["guard_top"] - numbers["guard_bottom"] > 0.3:
        gz = 0.5 * (numbers["guard_bottom"] + numbers["guard_top"])
        pts.append(point("Guard", [0, 0, gz], [0, 0, 1], [-1, 0, 0], 0, 0, 0, 1, 100))
    # the grip: a plain handle, so a hit there never uses another part's values
    pts.append(point("Grip", [0, 0, 0.5 * numbers["handle_bottom"]], [0, 0, 1], [-1, 0, 0], 0, 0, 0, 0, 100, ["NoStab"]))
    if numbers["handle_bottom"] - numbers["pommel_end"] > 0.3:
        pz = 0.5 * (numbers["handle_bottom"] + numbers["pommel_end"])
        pts.append(point("Pommel", [0, 0, pz], [0, 0, -1], [-1, 0, 0], 0, 0, 0, 1, 110))
    if not blunt:  # where a stuck blade's slide depth is measured from (Base, direct child of the part)
        pts.append(base_point("Head", [centre(guard_top), 0.0, guard_top]))
    info = dict(kind="sword", edges=kind, tip_angle=None if angle is None else round(angle, 1), sTip=sTip, sEdge=sEdge,
                rig=rig, dSharp=dSharp, base=guard_top)
    return pts, info


def hafted_points(pos, numbers, over):
    """The head (z > 0) by shape: spear (long, narrow, centred), axe (reaches out to one side with a thin
    edge), hammer/mace (thick); a narrow pointed part on the other side is a beak or spike."""
    tip = numbers["tip"]
    head = pos[pos[:, 2] > 0.5]
    if len(head) < 4:
        return [], {}
    x0, x1 = float(head[:, 0].min()), float(head[:, 0].max())
    head_len = tip - 0.0
    width = x1 - x0
    pts = []
    kind = over.get("head")
    if kind is None:
        if head_len > 2.5 * width and abs(x0 + x1) < 0.3 * width:
            kind = "spear"
        else:
            far = x1 if abs(x1) >= abs(x0) else x0
            edge_band = head[np.abs(head[:, 0] - far) < 1.0]
            thin = float(np.ptp(edge_band[:, 1])) if len(edge_band) > 1 else 99.0
            kind = "axe" if abs(far) > 4.0 and thin < 1.2 else "blunt"
    info = dict(kind=kind)
    if kind == "spear":
        pr = PRESETS["spear"]
        sTip, angle = tip_sharpness(pos, tip, 0.0)
        sTip = over.get("sTip", sTip)
        sEdge, rig = over.get("sEdge", pr["sEdge"]), over.get("rig", pr["rig"])
        pts.append(point("Head", [0.5 * (x0 + x1), 0, tip - 0.1], [0, 0, 1], [-1, 0, 0], sTip, sEdge, 0, 0, rig,
                         ["Piercing", "BoxCollision", "Double"]))
        pts.append(base_point("Head", [0.5 * (x0 + x1), 0.0, 0.0]))
        info.update(tip_angle=None if angle is None else round(angle, 1), sTip=sTip, sEdge=sEdge, rig=rig)
    elif kind == "axe":
        pr = PRESETS["axe"]
        sgn = 1.0 if abs(x1) >= abs(x0) else -1.0
        far = x1 if sgn > 0 else x0
        band = head[np.abs(head[:, 0] - far) < 1.5]
        zmid = 0.5 * (float(band[:, 2].min()) + float(band[:, 2].max()))
        sEdge, rig = over.get("sEdge", pr["sEdge"]), over.get("rig", pr["rig"])
        pts.append(point("Head", [far - sgn * 1.0, 0, zmid], [0, 0, 1], [sgn, 0, 0], 0, sEdge, 4, 0, rig,
                         ["Piercing", "BoxCollision", "NoStab"]))
        # the back: beak/spike if it narrows to a point, else a hammer face
        back = head[head[:, 0] * sgn < -1.5]
        if len(back) > 3:
            bfar = float(back[:, 0].min()) if sgn > 0 else float(back[:, 0].max())
            tipband = back[np.abs(back[:, 0] - bfar) < 1.0]
            pointed = len(tipband) > 1 and np.ptp(tipband[:, 2]) < 2.5 and np.ptp(tipband[:, 1]) < 2.5
            if pointed:
                pts.append(point("Head", [bfar + sgn * 0.5, 0, float(tipband[:, 2].mean())], [-sgn, 0, 0], [0, 0, 1],
                                 50, 0, 0, 0, 115, ["Piercing"]))
            else:
                pts.append(point("Head", [bfar + sgn * 0.5, 0, float(back[:, 2].mean())], [-sgn, 0, 0], [0, 0, 1],
                                 5, 0, 0, 1, 140))
        pts.append(base_point("Head", [0.0, 0.0, zmid]))
        if tip - zmid > 6:  # a top spike
            pts.append(point("Head", [0, 0, tip - 0.1], [0, 0, 1], [-1, 0, 0], 60, 0, 0, 0, 110, ["Piercing"]))
        info.update(sEdge=sEdge, rig=rig)
    else:
        pr = PRESETS["blunt"]
        rig = over.get("rig", pr["rig"])
        zc = 0.5 * (float(head[:, 2].min()) + tip)
        pts.append(point("Head", [0, 0, tip - 0.1], [0, 0, 1], [-1, 0, 0], pr["sTip"], 0, 0, 1, rig))
        for ang in range(0, 360, 90):
            a = math.radians(ang)
            d = np.array([math.cos(a), math.sin(a), 0.0])
            r = float(np.max(head[:, :2] @ d[:2]))
            pts.append(point("Head", d * max(r - 0.5, 0.0) + [0, 0, zc], d, [0, 0, 1], pr["sTip"], 5, 0, 1, rig))
        info.update(rig=rig)
    # the haft: a plain stick
    hz = 0.5 * numbers["handle_bottom"]
    pts.append(point("Grip", [0, 0, hz], [0, 0, 1], [-1, 0, 0], 0, 0, 0, 0, 85, ["NoStab"]))
    return pts, info


def lower_end_points(pos, numbers, hafted, over):
    """A double-ended weapon's bottom head or blade (the "Pommel" part, pommel_end..handle_bottom): the same
    rules on the model turned end over end (z' = handle_bottom - z), then turned back."""
    hb = numbers["handle_bottom"]
    m = pos.copy()
    m[:, 2] = hb - pos[:, 2]
    top = float(pos[:, 2].max())
    nm = dict(numbers, tip=hb - numbers["pommel_end"], pommel_end=hb - top, handle_bottom=hb,
              guard_bottom=0.0, guard_top=0.0)
    pts, info = (hafted_points if hafted else sword_points)(m, nm, over)
    out = []
    for q in pts:
        if q["slot"] != "Head":
            continue  # the haft and the other end's points come from the top end
        q = dict(q, slot="Pommel")
        q["p"] = [q["p"][0], q["p"][1], hb - q["p"][2]]
        q["f"] = [q["f"][0], q["f"][1], -q["f"][2]]
        q["u"] = [q["u"][0], q["u"][1], -q["u"][2]]
        out.append(q)
    return out, info


def compute(pos, numbers, hafted, over=None):
    over = {k: v for k, v in (over or {}).items() if v is not None}
    pts, info = hafted_points(pos, numbers, over) if hafted else sword_points(pos, numbers, over)
    if numbers.get("double") and numbers["handle_bottom"] - numbers["pommel_end"] > 2.0:
        low, low_info = lower_end_points(pos, numbers, hafted, over)
        pts = pts + low
        info = dict(info, lower=low_info.get("kind"))
    return pts, info


def write(path, pts):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("; damage points: slot x y z  fx fy fz  ux uy uz  sTip sEdge dSharp dBlunt rig  flags (tools/damage_points.py)\n")
        for q in pts:
            f.write(" ".join([q["slot"]] + [f"{v:.2f}" for v in q["p"]] + [f"{v:.4f}" for v in q["f"] + q["u"]]
                             + [str(q[k]) for k in ("sTip", "sEdge", "dSharp", "dBlunt", "rig")] + q["flags"]) + "\n")
