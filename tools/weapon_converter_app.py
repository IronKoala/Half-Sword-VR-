"""Weapon Converter: the end-user window around tools/model_to_weapon.py (built into WeaponConverter.exe).

Pick a weapon model (GLB, glTF, OBJ, ...), or drop it on the window or on WeaponConverter.exe. Type its
real length (and weight, if known), press Preview, check the picture, then Install (the full conversion runs then). Install copies the
converted files into the HalfSwordCustomWeapon mod folder of your Half Sword install and makes it the
model the mod uses (model= in CustomWeapon.ini). Nothing reaches the mod folder before Install.
"""
import glob
import io
import json
import multiprocessing
import os
import queue
import re
import shutil
import sys
import tempfile
import threading
import traceback

# A windowed exe has no console: anything printed must not crash it.
for _s in ("stdout", "stderr"):
    if getattr(sys, _s) is None:
        setattr(sys, _s, io.StringIO())

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np  # noqa: E402
import tkinter as tk  # noqa: E402
from tkinter import filedialog, messagebox, ttk  # noqa: E402

import model_to_weapon  # noqa: E402

try:  # drag and drop onto the window (optional package)
    from tkinterdnd2 import DND_FILES, TkinterDnD
except Exception:
    TkinterDnD = None

UNITS = [("metres", 100.0), ("centimetres", 1.0), ("millimetres", 0.1), ("inches", 2.54)]
MODEL_TYPES = "*.glb *.gltf *.fbx *.obj *.nif *.stl *.ply *.dae *.off"
MODEL_EXTS = tuple(e[1:] for e in MODEL_TYPES.split())
TYPES = [("Detect", None), ("Sword or dagger", "sword"), ("Axe, mace, hammer or spear", "hafted"),
         ("Double-ended (head or blade at both ends)", "double"), ("Shield", "shield"),
         ("Flail (staff, chain and head)", "flail"), ("Crossbow", "crossbow")]
# How a shield is held: which of the game's shields carries it (its grip is that shield's own).
SHIELD_GRIPS = [("Fist grip (buckler)", "shield_buckler"), ("Arm straps (targe)", "shield_targe"),
                ("Arm straps, big (pavise)", "shield_pavise")]
# Hand spots the user can place on the picture (written as spot_<key>= in the model's .txt, cm along the
# weapon; the mod uses them instead of its own placement). The mod's names for the game's markers:
# main = Grip R Hand Default Pos, off = Grip L Hand Off Hand, half_main/half_off = Grip R/L Hand Alt Pos
# (half-swording), reverse = Grip R Hand Default Pos Reverse (reverse/Mordhau grip).
SPOTS = [("main", "Main hand", "#e74c3c"), ("off", "Off hand", "#3498db"), ("half_main", "Half-sword main", "#e67e22"),
         ("half_off", "Half-sword off", "#1abc9c"), ("reverse", "Reverse grip", "#9b59b6")]
PARTS = ("pommel", "grip", "guard", "blade")
# Damage stats (tools/damage_points.py): "Detect" lets the converter read them from the model's shape.
EDGES = [("Detect", None), ("Both edges", "double"), ("One edge (side from the model)", "single"),
         ("One edge (the other side)", "single_other"), ("No edge (blunt)", "none")]
HELD = [("Detect (by length)", None), ("One hand (like a mace)", "one"), ("Two hands (like a great axe)", "two"),
        ("Spear grip", "spear")]
HEADS = [("Detect", None), ("Spear point", "spear"), ("Axe blade", "axe"), ("Blunt (mace, hammer)", "blunt")]
# Which of the game's swords carries a sword (pack.txt carrier=): its grip, hand spots and how many hands the
# game lets hold it come from that sword. "Detect" picks by length (under 55 cm a dagger) and grip length.
SWORD_CLASSES = [("Detect (by length)", None), ("Dagger (one hand)", "dagger"), ("One-handed sword", "arming"),
                 ("Bastard sword (hand and a half)", "bastard"), ("Longsword / greatsword (two hands)", "longsword")]
# Swords' scabbards (pack.txt scabbard=): one of the game's 8 (by blade length: short sword 25 .. long sword 110),
# the best-fitting of them, the user's own model (tools/scabbard.py), or none (worn on the belt).
SCABBARDS = [("Game scabbard, best fit", "fit"), ("Game: short sword 25", "SS25"), ("Game: short sword 45", "SS45"),
             ("Game: arming sword 60", "AS60"), ("Game: arming sword 70", "AS70"), ("Game: long sword 75", "LS75"),
             ("Game: bastard sword 85", "BS85"), ("Game: bastard sword 100", "BS100"), ("Game: long sword 110", "LS110"),
             ("Own model...", "own"), ("None (on the belt)", "none")]
RANKS = ["Beggar", "Peasant", "Commoner", "Militia", "Soldier", "Veteran", "Man-at-arms", "Knight"]
STAT_FIELDS = (("edge_sharp", "Edge sharpness"), ("tip_sharp", "Point sharpness"), ("rigidity", "Rigidity"))
ZONE_COLOURS = {"Pommel": "#b07a3c", "Grip": "#7a4a2a", "Guard": "#c9a227", "Head": "#8fa3b8"}
# Display units. Everything inside stays centimetres and kilograms; only what is shown and typed changes.
UNIT_SYSTEMS = {"Metric (cm, kg)": ("cm", 1.0, "kg", 1.0), "Imperial (inches, pounds)": ("in", 2.54, "lb", 0.45359237)}
SETTINGS = os.path.join(os.environ.get("APPDATA") or os.path.expanduser("~"), "HalfSwordWeaponConverter.json")


# ---- finding the mod --------------------------------------------------------------------------------
def steam_libraries():
    roots = []
    try:
        import winreg
        for hive, key in ((winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam"),
                          (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Valve\Steam")):
            try:
                with winreg.OpenKey(hive, key) as k:
                    for value in ("SteamPath", "InstallPath"):
                        try:
                            roots.append(winreg.QueryValueEx(k, value)[0])
                        except OSError:
                            pass
            except OSError:
                pass
    except ImportError:
        pass
    libs = []
    for r in roots:
        libs.append(r)
        vdf = os.path.join(r, "steamapps", "libraryfolders.vdf")
        if os.path.isfile(vdf):
            for m in re.finditer(r'"path"\s+"([^"]+)"', open(vdf, encoding="utf-8", errors="replace").read()):
                libs.append(m.group(1).replace("\\\\", "\\"))
    seen, out = set(), []
    for lib in libs:
        key = os.path.normcase(os.path.normpath(lib))
        if key not in seen:
            seen.add(key)
            out.append(lib)
    return out


def find_mod_folder():
    for lib in steam_libraries():
        game = os.path.join(lib, "steamapps", "common", "Half Sword")
        for mods in glob.glob(os.path.join(game, "*", "Binaries", "Win64", "ue4ss", "Mods")):
            return os.path.join(mods, "HalfSwordCustomWeapon")
    return None


def write_pack(mod_dir, it):
    """One weapon = one folder weapons/<id>/ in the mod: the converted files and pack.txt, which says how the
    weapon enters the game. The mod adds every pack it finds as a new weapon (no game weapon is changed)."""
    name = it["result"][0]
    if not os.path.isfile(os.path.join(it["work"], name + ".obj")):
        raise OSError(f"{it['name']}: its converted files are missing (convert it again); the installed copy was left as it was")
    folder = os.path.join(mod_dir, "weapons", it["name"])
    if os.path.isdir(folder):
        shutil.rmtree(folder)
    os.makedirs(folder)
    for f in os.listdir(it["work"]):
        shutil.copy2(os.path.join(it["work"], f), os.path.join(folder, f))
    title = re.sub(r"[\r\n=]+", " ", it["title"]).strip() or name
    lines = ["; written by WeaponConverter: how this weapon enters Half Sword",
             "title=" + title,
             "model=" + name + ".obj",
             "free_play=" + ("1" if it["free_play"] else "0"),
             "progression=" + ("1" if it["progression"] else "0"),
             "tier=" + str(it.get("tier", 3)),
             "price=" + str(it.get("price", 0))]
    kind = it["result"][1].get("type")
    if kind == "shield":
        lines.append("carrier=" + it.get("shield_grip", "shield_buckler"))
    elif kind not in ("hafted", "double", "flail", "crossbow"):
        if it.get("sword_class"):
            lines.append("carrier=" + it["sword_class"])
        scab = it.get("scabbard", "fit")
        if scab == "own":
            model = it.get("scabbard_model")
            ok = False
            if model and os.path.exists(model):
                import scabbard
                try:
                    scabbard.main([model, folder, name] + (["--flip"] if it.get("scabbard_flip") else []))
                    ok = True
                except SystemExit as e:
                    print(f"scabbard model not used: {e}")
            if not ok:
                print("no usable scabbard model: the best-fitting game scabbard is used")
                scab = "fit"
        lines.append("scabbard=" + scab)
    open(os.path.join(folder, "pack.txt"), "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")


def load_settings():
    try:
        return json.load(open(SETTINGS, encoding="utf-8"))
    except Exception:
        return {}


def save_settings(values):
    try:
        json.dump(values, open(SETTINGS, "w", encoding="utf-8"), indent=1)
    except Exception:
        pass


# ---- the preview picture ----------------------------------------------------------------------------
def read_obj(path):
    """Positions, normals, uvs (one per vertex, as write_visual_obj writes them) and triangles."""
    v, vn, vt, f = [], [], [], []
    with open(path) as fh:
        for line in fh:
            if line.startswith("v "):
                v.append(line[2:])
            elif line.startswith("vn "):
                vn.append(line[3:])
            elif line.startswith("vt "):
                vt.append(line[3:])
            elif line.startswith("f "):
                f.append(" ".join(p.split("/")[0] for p in line[2:].split()))
    pos = np.loadtxt(v, ndmin=2)
    nrm = np.loadtxt(vn, ndmin=2) if vn else np.zeros_like(pos)
    uv = np.loadtxt(vt, ndmin=2) if vt else np.zeros((len(pos), 2))
    tris = np.loadtxt(f, dtype=np.int64, ndmin=2) - 1
    return pos, nrm, uv, tris


def view_scale(pos, width, heights):
    """Pixels per cm that fits the weapon's length in width and its flat and edge views in heights."""
    span = max(float(np.ptp(pos[:, 2])), 1e-6)
    s = (width - 20) / span
    for axis, h in zip((0, 1), heights):
        s = min(s, (h / 2 - 6) / (float(np.abs(pos[:, axis]).max()) or 1.0))
    return s


def render_view(pos, nrm, uv, tris, colour_img, zones, view, width, height, s, from_plus=False):
    """Shaded picture of the weapon lying left (pommel) to right (tip). view "flat": looking at the
    blade's flat (the edge direction is up); "edge": looking at its edge. Returns a PIL image."""
    from PIL import Image, ImageDraw
    up, depth = (0, 1) if view == "flat" else (1, 0)
    z = pos[:, 2]
    zmin = float(z.min())
    px = 10 + (z - zmin) * s
    py = height / 2 - pos[:, up] * s
    if len(tris) > 80000:  # a preview of a huge model: its 80000 biggest triangles (drawing each one is slow)
        tv = pos[tris]
        ar = np.linalg.norm(np.cross(tv[:, 1] - tv[:, 0], tv[:, 2] - tv[:, 0]), axis=1)
        tris = tris[np.argsort(-ar)[:80000]]
    img = Image.new("RGB", (width, height), (34, 36, 40))
    draw = ImageDraw.Draw(img)
    # Colour per triangle: the texture at its centre, else its zone's colour; simple light from the viewer.
    if colour_img is not None:
        tex = np.asarray(colour_img.convert("RGB")).astype(np.float64) / 255.0
        th, tw = tex.shape[:2]
        c_uv = uv[tris].mean(axis=1)
        cx = np.clip((np.mod(c_uv[:, 0], 1.0) * (tw - 1)).astype(int), 0, tw - 1)
        cy = np.clip(((1 - np.mod(c_uv[:, 1], 1.0)) * (th - 1)).astype(int), 0, th - 1)
        base = tex[cy, cx]
    else:
        cz = z[tris].mean(axis=1)
        base = np.zeros((len(tris), 3))
        for slot, lo, hi in zones:
            rgb = np.array([int(ZONE_COLOURS[slot][i:i + 2], 16) for i in (1, 3, 5)]) / 255.0
            base[(cz >= lo) & (cz <= hi)] = rgb
    n = nrm[tris].mean(axis=1)
    n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-9)
    light = np.zeros(3)
    light[depth], light[up], light[2] = -0.8, 0.45, 0.2
    light /= np.linalg.norm(light)
    shade = 0.35 + 0.65 * np.abs(n @ light)
    col = (np.clip(base * shade[:, None], 0, 1) * 255).astype(int)
    # far side first (seen from -depth; from_plus: from +depth, a shield's front)
    order = np.argsort((1 if from_plus else -1) * pos[tris, depth].mean(axis=1))
    xs, ys = px[tris], py[tris]
    for i in order:
        draw.polygon([(xs[i, 0], ys[i, 0]), (xs[i, 1], ys[i, 1]), (xs[i, 2], ys[i, 2])], fill=tuple(col[i]))
    return img


def zone_bar(zones, zmin, s, width, hafted, fmt_len=lambda cm: f"{cm:.0f} cm", double=False, flail=False):
    from PIL import Image, ImageDraw
    img = Image.new("RGB", (width, 26), (34, 36, 40))
    d = ImageDraw.Draw(img)
    names = {"Pommel": "lower head" if double else "butt" if hafted else "pommel",
             "Grip": "staff" if flail else "haft" if hafted else "grip", "Guard": "chain" if flail else "guard",
             "Head": "head" if hafted else "blade"}
    for slot, lo, hi in zones:
        x0, x1 = 10 + (lo - zmin) * s, 10 + (hi - zmin) * s
        if x1 - x0 < 1:
            continue
        d.rectangle([x0, 2, x1, 12], fill=ZONE_COLOURS[slot])
        label = f"{names[slot]} {fmt_len(hi - lo)}"
        tw = d.textlength(label)
        if tw < x1 - x0 + 30:
            d.text(((x0 + x1) / 2 - tw / 2, 13), label, fill=(220, 220, 220))
    return img


# ---- the window -------------------------------------------------------------------------------------
class App:
    def __init__(self, root, first_model=None):
        self.root = root
        self.settings = load_settings()
        self.work_root = tempfile.mkdtemp(prefix="hs_weapon_")
        self.work = self.work_root
        self.events = queue.Queue()
        self.busy = False
        self.flip = False
        self.result = None  # (name, json) of the conversion now in self.work
        self.result_inputs = None
        self.raw_length = None
        self.photos = []
        self.result_views = None
        # Several models at once: each one is an item (its own inputs, work folder and result). The editor
        # below always shows the selected item; self.work/flip/raw_length/result* belong to it.
        self.items = []
        self.cur = None
        self.quiet = False  # True while the editor is being filled in by code
        self.batch_running = False

        root.title("Half Sword Weapon Converter")
        root.minsize(760, 560)
        root.protocol("WM_DELETE_WINDOW", self.close)
        pad = {"padx": 8, "pady": 4}
        self.batch = ttk.Frame(root, padding=(10, 10, 10, 0))
        ttk.Label(self.batch, text="Weapons (pick one to check or change it)").pack(anchor="w")
        cols = ("length", "weight", "status")
        self.tree = ttk.Treeview(self.batch, columns=cols, height=6, selectmode="browse")
        self.tree.heading("#0", text="Model")
        self.tree.column("#0", width=330)
        for c, w in zip(cols, (90, 90, 330)):
            self.tree.heading(c, text=c.capitalize())
            self.tree.column(c, width=w, anchor="w")
        self.tree.pack(fill="x")
        self.tree.bind("<<TreeviewSelect>>", lambda e: self.tree_selected())
        bb = ttk.Frame(self.batch)
        bb.pack(fill="x", pady=(4, 0))
        ttk.Button(bb, text="Add models...", command=self.browse).pack(side="left")
        ttk.Button(bb, text="Remove", command=self.remove_item).pack(side="left", padx=6)
        self.install_all_btn = ttk.Button(bb, text="Install all converted", command=self.install_all)
        self.install_all_btn.pack(side="right")
        self.convert_all_btn = ttk.Button(bb, text="Preview all", command=self.convert_all)
        self.convert_all_btn.pack(side="right", padx=6)
        main = ttk.Frame(root, padding=10)
        main.pack(fill="both", expand=True)
        main.columnconfigure(1, weight=1)
        self.main = main

        ttk.Label(main, text="Model").grid(row=0, column=0, sticky="w", **pad)
        self.model_var = tk.StringVar()
        ttk.Entry(main, textvariable=self.model_var, state="readonly").grid(row=0, column=1, sticky="ew", **pad)
        ttk.Button(main, text="Browse...", command=self.browse).grid(row=0, column=2, **pad)
        self.model_info = ttk.Label(main, text="Pick weapon models" + (", or drop them on this window (one or many)." if TkinterDnD else "."))
        self.model_info.grid(row=1, column=1, columnspan=2, sticky="w", padx=8)
        self.unit_buttons = ttk.Frame(main)
        self.unit_buttons.grid(row=2, column=1, columnspan=2, sticky="w", padx=8)

        form = ttk.Frame(main)
        form.grid(row=3, column=0, columnspan=3, sticky="ew", pady=(6, 0))
        self.length_label = ttk.Label(form, text="Real length (cm)")
        self.length_label.grid(row=0, column=0, sticky="w", **pad)
        self.length_var = tk.StringVar()
        ttk.Entry(form, textvariable=self.length_var, width=10).grid(row=0, column=1, sticky="w", **pad)
        self.length_hint = ttk.Label(form, text="", foreground="#777")
        self.length_hint.grid(row=0, column=2, sticky="w", **pad)
        self.mass_label = ttk.Label(form, text="Weight (kg)")
        self.mass_label.grid(row=1, column=0, sticky="w", **pad)
        self.mass_var = tk.StringVar()
        ttk.Entry(form, textvariable=self.mass_var, width=10).grid(row=1, column=1, sticky="w", **pad)
        ttk.Label(form, text="leave empty to estimate it from the model", foreground="#777").grid(row=1, column=2, sticky="w", **pad)
        ttk.Label(form, text="Weapon type").grid(row=2, column=0, sticky="w", **pad)
        self.type_var = tk.StringVar(value=TYPES[0][0])
        ttk.Combobox(form, textvariable=self.type_var, values=[t for t, _ in TYPES], state="readonly",
                     width=28).grid(row=2, column=1, columnspan=2, sticky="w", **pad)
        self.slide_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(form, text="Hand slides down the handle in swings", variable=self.slide_var).grid(
            row=4, column=1, columnspan=2, sticky="w", **pad)
        ttk.Label(form, text="Grip", foreground="#000").grid(row=4, column=0, sticky="w", **pad)
        ttk.Label(form, text="only for odd long-handled swords like the Spit: main hand by the guard, off hand at the "
                             "pommel, main hand slides down when swinging. Leave off for normal swords.",
                  foreground="#777", wraplength=560).grid(row=5, column=1, columnspan=2, sticky="w", padx=8)
        ttk.Label(form, text="Hand spots").grid(row=6, column=0, sticky="w", **pad)
        spots = ttk.Frame(form)
        spots.grid(row=6, column=1, columnspan=2, sticky="w", **pad)
        self.spot_buttons = {}
        form.columnconfigure(2, weight=1)
        for i, (key, label, _) in enumerate(SPOTS):
            b = ttk.Button(spots, text=f"{label}: auto", command=lambda k=key: self.start_placing(k), state="disabled")
            b.grid(row=i // 3, column=i % 3, sticky="ew", padx=(0, 4), pady=1)
            b.bind("<Button-3>", lambda e, k=key: self.clear_spot(k))
            self.spot_buttons[key] = b
        ttk.Label(form, text="optional: press one, then click on the picture where that hand should hold "
                             "(measured up from the pommel end). Right-click a button to go back to auto.",
                  foreground="#777", wraplength=640).grid(
            row=7, column=1, columnspan=2, sticky="w", padx=8)
        self.placing = None
        ttk.Label(form, text="Part lengths").grid(row=8, column=0, sticky="w", **pad)
        lens = ttk.Frame(form)
        lens.grid(row=8, column=1, columnspan=2, sticky="w", **pad)
        self.len_vars = {}
        for part in PARTS:
            ttk.Label(lens, text=part.capitalize()).pack(side="left", padx=(0, 3))
            v = tk.StringVar()
            ttk.Entry(lens, textvariable=v, width=7).pack(side="left", padx=(0, 10))
            v.trace_add("write", lambda *_, p=part: self.part_length_typed(p))
            self.len_vars[part] = v
        ttk.Button(lens, text="Reset parts", command=self.reset_parts).pack(side="left")
        ttk.Label(form, text="type a new length to stretch or shrink just that part; drag the white marks between the "
                             "coloured parts under the picture to move where one part ends and the next begins.",
                  foreground="#777", wraplength=640).grid(row=9, column=1, columnspan=2, sticky="w", padx=8)
        ttk.Label(form, text="In the game").grid(row=10, column=0, sticky="w", **pad)
        game = ttk.Frame(form)
        game.grid(row=10, column=1, columnspan=2, sticky="w", **pad)
        ttk.Label(game, text="Name").pack(side="left", padx=(0, 3))
        self.title_var = tk.StringVar()
        ttk.Entry(game, textvariable=self.title_var, width=26).pack(side="left", padx=(0, 10))
        self.free_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(game, text="Free play", variable=self.free_var).pack(side="left", padx=(0, 6))
        self.prog_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(game, text="Progression", variable=self.prog_var).pack(side="left", padx=(0, 10))
        ttk.Label(game, text="Shield held by").pack(side="left", padx=(0, 3))
        self.shield_var = tk.StringVar(value=SHIELD_GRIPS[0][0])
        ttk.Combobox(game, textvariable=self.shield_var, values=[t for t, _ in SHIELD_GRIPS], state="readonly",
                     width=22).pack(side="left")
        ttk.Label(game, text="Sword class").pack(side="left", padx=(10, 3))
        self.class_var = tk.StringVar(value=SWORD_CLASSES[0][0])
        ttk.Combobox(game, textvariable=self.class_var, values=[t for t, _ in SWORD_CLASSES], state="readonly",
                     width=30).pack(side="left")
        self.class_var.trace_add("write", lambda *_: getattr(self, "result", None) and self.redraw_result())
        game2 = ttk.Frame(form)
        game2.grid(row=11, column=1, columnspan=2, sticky="w", **pad)
        ttk.Label(game2, text="Progression: sold from rank").pack(side="left", padx=(0, 3))
        self.rank_var = tk.StringVar(value=RANKS[3])
        ttk.Combobox(game2, textvariable=self.rank_var, values=RANKS, state="readonly", width=12).pack(side="left", padx=(0, 10))
        ttk.Label(game2, text="price (crowns)").pack(side="left", padx=(0, 3))
        self.price_var = tk.StringVar()
        ttk.Entry(game2, textvariable=self.price_var, width=7).pack(side="left", padx=(0, 10))
        ttk.Label(game2, text="Scabbard").pack(side="left", padx=(0, 3))
        self.scab_var = tk.StringVar(value=SCABBARDS[0][0])
        scab_box = ttk.Combobox(game2, textvariable=self.scab_var, values=[t for t, _ in SCABBARDS], state="readonly",
                                width=24)
        scab_box.pack(side="left", padx=(0, 4))
        scab_box.bind("<<ComboboxSelected>>", self.scabbard_chosen)
        self.scab_flip_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(game2, text="mouth is the narrow end", variable=self.scab_flip_var).pack(side="left")
        ttk.Label(form, text="Install adds the weapon to the game as a new weapon: in free play's weapon list, "
                             "and/or for sale at the merchant in progression once you reach that rank (price "
                             "empty: by rank).",
                  foreground="#777", wraplength=640).grid(row=15, column=1, columnspan=2, sticky="w", padx=8)
        ttk.Label(form, text="Damage").grid(row=12, column=0, sticky="w", **pad)
        dmg = ttk.Frame(form)
        dmg.grid(row=12, column=1, columnspan=2, sticky="w", **pad)
        ttk.Label(dmg, text="Edges").pack(side="left", padx=(0, 3))
        self.edges_var = tk.StringVar(value=EDGES[0][0])
        ttk.Combobox(dmg, textvariable=self.edges_var, values=[t for t, _ in EDGES], state="readonly",
                     width=27).pack(side="left", padx=(0, 10))
        ttk.Label(dmg, text="Head").pack(side="left", padx=(0, 3))
        self.head_var = tk.StringVar(value=HEADS[0][0])
        ttk.Combobox(dmg, textvariable=self.head_var, values=[t for t, _ in HEADS], state="readonly",
                     width=20).pack(side="left", padx=(0, 10))
        ttk.Label(dmg, text="Held with").pack(side="left", padx=(0, 3))
        self.held_var = tk.StringVar(value=HELD[0][0])
        ttk.Combobox(dmg, textvariable=self.held_var, values=[t for t, _ in HELD], state="readonly",
                     width=26).pack(side="left", padx=(0, 6))
        self.held_var.trace_add("write", lambda *_: getattr(self, "result", None) and self.redraw_result())
        self.turn_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(dmg, text="head faces the other way", variable=self.turn_var).pack(side="left")
        dmg2 = ttk.Frame(form)
        dmg2.grid(row=13, column=1, columnspan=2, sticky="w", **pad)
        self.stat_vars = {}
        for key, label in STAT_FIELDS:
            ttk.Label(dmg2, text=label).pack(side="left", padx=(0, 3))
            v = tk.StringVar()
            ttk.Entry(dmg2, textvariable=v, width=6).pack(side="left", padx=(0, 10))
            self.stat_vars[key] = v
        self.balance_label = ttk.Label(dmg2, text="Balance point (cm above the grip)")
        self.balance_label.pack(side="left", padx=(0, 3))
        self.balance_var = tk.StringVar()
        ttk.Entry(dmg2, textvariable=self.balance_var, width=7).pack(side="left")
        self.stats_hint = ttk.Label(form, text="leave empty to take them from the model's shape. Sharpness 0-100 "
                                                "(the game's arming sword: edge 90, point 98); rigidity: arming sword "
                                                "80, axe 105, mace 125. Edges = which edge cuts (Edges is for "
                                                "blades, Head for axes, maces and spears).",
                                    foreground="#777", wraplength=640)
        self.stats_hint.grid(row=14, column=1, columnspan=2, sticky="w", padx=8)
        ttk.Label(form, text="Units").grid(row=3, column=0, sticky="w", **pad)
        unit_name = self.settings.get("units")
        self.units_var = tk.StringVar(value=unit_name if unit_name in UNIT_SYSTEMS else next(iter(UNIT_SYSTEMS)))
        self.units_shown = self.units_var.get()
        ttk.Combobox(form, textvariable=self.units_var, values=list(UNIT_SYSTEMS), state="readonly",
                     width=28).grid(row=3, column=1, columnspan=2, sticky="w", **pad)
        self.units_var.trace_add("write", lambda *_: self.units_changed())
        self.show_unit_labels()
        for var in (self.length_var, self.mass_var, self.type_var, self.slide_var, self.edges_var, self.head_var, self.held_var, self.turn_var,
                    self.balance_var, *self.stat_vars.values()):
            var.trace_add("write", lambda *_: self.inputs_changed())

        ttk.Label(main, text="Mod folder").grid(row=4, column=0, sticky="w", **pad)
        self.mod_var = tk.StringVar(value=self.settings.get("mod_dir") or find_mod_folder() or "")
        ttk.Entry(main, textvariable=self.mod_var, state="readonly").grid(row=4, column=1, sticky="ew", **pad)
        ttk.Button(main, text="Change...", command=self.pick_mod).grid(row=4, column=2, **pad)
        self.mod_info = ttk.Label(main, text="")
        self.mod_info.grid(row=5, column=1, columnspan=2, sticky="w", padx=8)
        self.check_mod()

        self.convert_btn = ttk.Button(main, text="Preview", command=self.convert)
        self.convert_btn.grid(row=6, column=0, columnspan=3, pady=8)

        self.preview = ttk.Label(main, anchor="center")
        self.preview.bind("<Button-1>", self.preview_clicked)
        self.preview.bind("<B1-Motion>", self.preview_dragged)
        self.preview.bind("<ButtonRelease-1>", self.preview_released)
        self.dragging = None
        self.preview.grid(row=7, column=0, columnspan=3, sticky="nsew")
        main.rowconfigure(7, weight=1)
        self.summary = ttk.Label(main, text="", justify="left", wraplength=880)
        self.summary.grid(row=8, column=0, columnspan=3, sticky="w", **pad)

        buttons = ttk.Frame(main)
        buttons.grid(row=9, column=0, columnspan=3, sticky="ew")
        self.flip_btn = ttk.Button(buttons, text="Blade is on the wrong end", command=self.toggle_flip, state="disabled")
        self.flip_btn.pack(side="left", padx=8)
        self.details_btn = ttk.Button(buttons, text="Show details", command=self.toggle_details)
        self.details_btn.pack(side="left", padx=8)
        self.install_btn = ttk.Button(buttons, text="Install into Half Sword", command=self.install, state="disabled")
        self.install_btn.pack(side="right", padx=8)
        self.status = ttk.Label(main, text="", foreground="#555")
        self.status.grid(row=10, column=0, columnspan=3, sticky="w", **pad)

        self.log = tk.Text(main, height=10, wrap="word", font=("Consolas", 9))
        self.log.tag_configure("error", foreground="#c0392b")
        self.details_shown = False

        if TkinterDnD is not None:
            root.drop_target_register(DND_FILES)
            root.dnd_bind("<<Drop>>", lambda e: self.add_models(root.tk.splitlist(e.data)))
        root.after(100, self.poll)
        if first_model:
            models = first_model if isinstance(first_model, (list, tuple)) else [first_model]
            root.after(200, lambda: self.add_models(models))

    # ---- inputs ----------------------------------------------------------------------------------------
    def browse(self):
        paths = filedialog.askopenfilenames(title="Pick weapon models (one or many)", initialdir=self.settings.get("model_dir"),
                                            filetypes=[("3D models", MODEL_TYPES), ("All files", "*.*")])
        if paths:
            self.add_models(paths)

    def set_model(self, path):  # one model (kept for callers and tests)
        self.add_models([path])

    def add_models(self, paths):
        """Adds every model file given; a folder adds every model file inside it."""
        found = []
        for path in paths:
            if os.path.isdir(path):  # e.g. a downloaded mod folder
                found += sorted(os.path.join(r, f) for r, _, fs in os.walk(path) for f in fs
                                if f.lower().endswith(MODEL_EXTS))
            elif os.path.isfile(path) and path.lower().endswith(MODEL_EXTS):
                found.append(path)
        known = {os.path.normcase(os.path.abspath(it["path"])) for it in self.items}
        new = [f for f in found if os.path.normcase(os.path.abspath(f)) not in known]
        if not new:
            self.status.config(text=("Those are already in the list." if found else
                                     f"No model files ({', '.join(MODEL_EXTS)}) there."))
            return
        self.settings["model_dir"] = os.path.dirname(new[0])
        save_settings(self.settings)
        names = {it["name"].lower() for it in self.items}
        first = len(self.items)
        for f in new:
            base = re.sub(r"[^A-Za-z0-9_.-]+", "_", os.path.splitext(os.path.basename(f))[0]) or "weapon"
            name, n = base, 2
            while name.lower() in names:  # two files called sword.glb in different folders
                name, n = f"{base}_{n}", n + 1
            names.add(name.lower())
            item = {"path": f, "name": name, "raw": None, "length_cm": None, "mass_kg": None,
                    "type": TYPES[0][0], "flip": False, "slide": False, "spots": {}, "splits": None, "part_len": {},
                    "title": base.replace("_", " ").strip() or "Weapon", "stats": {}, "free_play": True, "progression": False, "work": tempfile.mkdtemp(prefix="w_", dir=self.work_root),
                    "result": None, "result_inputs": None, "views": None, "status": "reading the model...",
                    "iid": f"item{id(f)}_{len(self.items)}"}
            self.items.append(item)
            self.tree.insert("", "end", iid=item["iid"], text=os.path.basename(f), values=("", "", item["status"]))
        if len(self.items) > 1 and not self.batch.winfo_ismapped():
            self.batch.pack(fill="x", before=self.main)
        self.select(self.items[first])
        self.run(self.measure_all, [it["path"] for it in self.items[first:]])

    def scabbard_chosen(self, _event=None):
        """Own model: pick its file (any model type the converter reads); cancelled = back to best fit."""
        if dict(SCABBARDS).get(self.scab_var.get()) != "own" or self.cur is None:
            return
        path = filedialog.askopenfilename(title="Scabbard model", filetypes=[("3D models", MODEL_TYPES), ("All files", "*.*")])
        if not path:
            self.scab_var.set(SCABBARDS[0][0])
            return
        self.cur["scabbard_model"] = path
        self.scab_var.set("Own model...")
        self.status.config(text="Scabbard: " + os.path.basename(path) + " (fitted to the blade at Install)")

    def item_of(self, path):
        return next((it for it in self.items if it["path"] == path), None)

    def remove_item(self):
        it = self.cur
        if it is None or self.busy:
            return
        i = self.items.index(it)
        self.items.remove(it)
        self.tree.delete(it["iid"])
        shutil.rmtree(it["work"], ignore_errors=True)
        self.cur = None
        if self.items:
            self.select(self.items[min(i, len(self.items) - 1)])
        else:
            self.quiet = True
            self.model_var.set("")
            self.length_var.set("")
            self.mass_var.set("")
            self.quiet = False
            self.clear_result()
            self.model_info.config(text="Pick weapon models.")

    # ---- the editor shows one item ------------------------------------------------------------------
    def tree_selected(self):
        sel = self.tree.selection()
        it = next((x for x in self.items if x["iid"] in sel), None)
        if it is not None and it is not self.cur:
            self.select(it)

    def save_editor(self):
        it = self.cur
        if it is None:
            return
        _, lk, _, mk = self.units()
        for key, var, k in (("length_cm", self.length_var, lk), ("mass_kg", self.mass_var, mk)):
            try:
                it[key] = float(var.get().replace(",", ".")) * k
            except ValueError:
                it[key] = None
        it["type"] = self.type_var.get()
        it["slide"] = self.slide_var.get()
        it["title"] = self.title_var.get().strip() or it["title"]
        it["free_play"], it["progression"] = self.free_var.get(), self.prog_var.get()
        it["shield_grip"] = dict(SHIELD_GRIPS).get(self.shield_var.get(), SHIELD_GRIPS[0][1])
        it["tier"] = RANKS.index(self.rank_var.get()) if self.rank_var.get() in RANKS else 3
        it["scabbard"] = dict(SCABBARDS).get(self.scab_var.get(), "fit")
        it["scabbard_flip"] = self.scab_flip_var.get()
        it["sword_class"] = dict(SWORD_CLASSES).get(self.class_var.get())
        try:
            it["price"] = max(0, int(float(self.price_var.get().replace(",", "."))))
        except ValueError:
            it["price"] = 0
        it["stats"] = self.read_stats()[0] or it.get("stats", {})
        it.update(flip=self.flip, raw=self.raw_length, result=self.result, result_inputs=self.result_inputs,
                  views=self.result_views)
        self.refresh_row(it)

    def select(self, it):
        if self.cur is not None and self.cur is not it:
            self.save_editor()
        self.cur = it
        if self.tree.selection() != (it["iid"],):
            self.tree.selection_set(it["iid"])
            self.tree.see(it["iid"])
        self.work = it["work"]
        self.flip, self.raw_length = it["flip"], it["raw"]
        self.result, self.result_inputs, self.result_views = it["result"], it["result_inputs"], it["views"]
        _, lk, _, mk = self.units()
        self.quiet = True
        self.model_var.set(it["path"])
        self.length_var.set("" if it["length_cm"] is None else f"{it['length_cm'] / lk:.2f}".rstrip("0").rstrip("."))
        self.mass_var.set("" if it["mass_kg"] is None else f"{it['mass_kg'] / mk:.3f}".rstrip("0").rstrip("."))
        self.type_var.set(it["type"])
        self.slide_var.set(it["slide"])
        self.title_var.set(it["title"])
        self.free_var.set(it["free_play"])
        self.prog_var.set(it["progression"])
        self.shield_var.set(next((t for t, v in SHIELD_GRIPS if v == it.get("shield_grip")), SHIELD_GRIPS[0][0]))
        self.rank_var.set(RANKS[it.get("tier", 3)])
        self.scab_var.set(next((t for t, v in SCABBARDS if v == it.get("scabbard", "fit")), SCABBARDS[0][0]))
        self.scab_flip_var.set(bool(it.get("scabbard_flip")))
        self.class_var.set(next((t for t, v in SWORD_CLASSES if v == it.get("sword_class")), SWORD_CLASSES[0][0]))
        self.price_var.set(str(it["price"]) if it.get("price") else "")
        st = it.get("stats", {})
        self.edges_var.set(next(t for t, v in EDGES if v == st.get("edges")))
        self.head_var.set(next(t for t, v in HEADS if v == st.get("head")))
        self.held_var.set(next((t for t, v in HELD if v == st.get("held")), HELD[0][0]))
        self.turn_var.set(bool(st.get("turn")))
        for key, v in self.stat_vars.items():
            v.set("" if st.get(key) is None else f"{st[key]:g}")
        self.balance_var.set("" if st.get("balance_cm") is None else
                             f"{st['balance_cm'] / lk:.2f}".rstrip("0").rstrip("."))
        self.quiet = False
        self.show_model_info()
        self.show_part_lengths()
        if self.result is not None:
            self.redraw_result()
            self.flip_btn.config(state="normal")
            self.install_btn.config(state="normal")
            self.status.config(text="Check the picture, then Install.")
            self.inputs_changed()
        else:
            self.schedule_first_preview()
            self.photos = []
            self.preview.config(image="")
            self.summary.config(text="")
            self.flip_btn.config(state="disabled")
            self.install_btn.config(state="disabled")
            self.status.config(text="" if it["raw"] is None else "Check the length, then Convert.")
            self.placing = None
            self.refresh_spot_buttons()

    def refresh_row(self, it):
        if not self.tree.exists(it["iid"]):
            return
        self.tree.item(it["iid"], values=("" if it["length_cm"] is None else self.fmt_len(it["length_cm"], 1),
                                          "estimate" if it["mass_kg"] is None else self.fmt_mass(it["mass_kg"]),
                                          self.status_text(it)))

    def status_text(self, it):
        if it["status"] in ("converted", "installed") and it["result"] is not None:
            return f"{it['status']}: {self.short_summary(it['result'][1])}"
        if it["status"] == "length out of range" and it["length_cm"] is not None:
            return f"the file says {self.fmt_len(it['length_cm'])}: type its real length"
        return it["status"]

        # ---- units -----------------------------------------------------------------------------------------
    def units(self):
        return UNIT_SYSTEMS[self.units_var.get()]

    def fmt_len(self, cm, digits=0):
        u, k, _, _ = self.units()
        if u == "in" and digits == 0:
            digits = 1  # a whole inch is too coarse for guards and pommels
        return f"{cm / k:,.{digits}f} {u}"

    def fmt_mass(self, kg):
        _, _, u, k = self.units()
        return f"{kg / k:.2f} {u}"

    def show_unit_labels(self):
        lu, _, mu, _ = self.units()
        self.length_label.config(text=f"Real length ({lu})")
        self.mass_label.config(text=f"Weight ({mu})")
        self.balance_label.config(text=f"Balance point ({lu} above the grip)")
        self.length_hint.config(text="arming sword ~90, longsword 110-125, greatsword 140-180, spear 200+" if lu == "cm"
                                else "arming sword ~35, longsword 43-49, greatsword 55-71, spear 79+")

    def units_changed(self):
        old, new = UNIT_SYSTEMS[self.units_shown], self.units()
        self.units_shown = self.units_var.get()
        self.settings["units"] = self.units_shown
        save_settings(self.settings)
        # Convert what is already typed, then relabel everything. Half-converted fields must not count as
        # a change, so the check runs once at the end.
        self.converting_units = True
        for var, k_old, k_new, digits in ((self.length_var, old[1], new[1], 2), (self.mass_var, old[3], new[3], 3),
                                         (self.balance_var, old[1], new[1], 3)):
            try:
                v = float(var.get().replace(",", ".")) * k_old / k_new
                var.set(f"{v:.{digits}f}".rstrip("0").rstrip("."))
            except ValueError:
                pass
        self.converting_units = False
        self.show_unit_labels()
        self.show_model_info()
        self.show_part_lengths()
        if self.result is not None:
            self.redraw_result()
        self.inputs_changed()
        for it in self.items:
            self.refresh_row(it)

    def measure_all(self, paths):
        for path in paths:
            try:
                raw = model_to_weapon.model_length(path)
            except Exception as e:  # one broken file must not stop the rest
                self.events.put(("measure_failed", path, str(e)))
                continue
            self.events.put(("measured", path, raw))

    def has_units(self, path):
        return os.path.splitext(path)[1].lower() in (".glb", ".gltf", ".fbx")

    def measured(self, path, raw):
        it = self.item_of(path)
        if it is None:
            return
        it["raw"] = raw
        if self.has_units(path) and it["length_cm"] is None:
            it["length_cm"] = round(raw * 100, 1)
        it["status"] = "ready to convert" if it["length_cm"] is not None else "needs its real length"
        if it is self.cur:
            self.raw_length = raw
            if it["length_cm"] is not None and not self.length_var.get():
                self.quiet = True
                self.length_var.set(f"{it['length_cm'] / self.units()[1]:.1f}")
                self.quiet = False
            self.show_model_info()
            self.status.config(text="Check the length; the preview comes by itself.")
            self.schedule_first_preview()
        self.refresh_row(it)

    def show_model_info(self):
        for w in self.unit_buttons.winfo_children():
            w.destroy()
        it, raw = self.cur, self.raw_length
        if it is None:
            return
        if raw is None:
            self.model_info.config(text="Reading the model...")
            return
        k = self.units()[1]
        if self.has_units(it["path"]):
            self.model_info.config(text=f"The file makes this weapon {self.fmt_len(raw * 100, 1)} long. "
                                        "Downloaded models are often scaled wrong: check the length below.")
        else:
            self.model_info.config(text="This file does not say what unit it uses. Pick the one that gives a "
                                        "sensible length, or type the real length below:")
            units = UNITS + ([("Skyrim units", 1.42875)] if it["path"].lower().endswith(".nif") else [])
            for word, cm in units:
                ttk.Button(self.unit_buttons, text=f"{word}: {self.fmt_len(raw * cm, 1)}",
                           command=lambda v=raw * cm / k: self.length_var.set(f"{v:.1f}")).pack(side="left", padx=(0, 6))

    def pick_mod(self):
        path = filedialog.askdirectory(title="The HalfSwordCustomWeapon folder (in ...\\ue4ss\\Mods)",
                                       initialdir=self.mod_var.get() or None)
        if path:
            self.mod_var.set(os.path.normpath(path))
            self.settings["mod_dir"] = self.mod_var.get()
            save_settings(self.settings)
            self.check_mod()

    def check_mod(self):
        d = self.mod_var.get()
        if not d or not os.path.isdir(d):
            self.mod_info.config(text="Half Sword's HalfSwordCustomWeapon mod was not found. Install the mod first "
                                      "(see READ ME FIRST.txt), or pick its folder.", foreground="#c0392b")
        elif not os.path.isfile(os.path.join(d, "enabled.txt")):
            self.mod_info.config(text="Note: the mod is switched off (no enabled.txt in its folder), so the game "
                                      "will not use the weapon until it is switched on.", foreground="#b9770e")
        else:
            self.mod_info.config(text="", foreground="")

    def read_stats(self):
        """The damage settings typed in the editor: ({key: value or None}, problem)."""
        lu, lk, _, _ = self.units()
        st = {"edges": dict(EDGES)[self.edges_var.get()], "head": dict(HEADS)[self.head_var.get()],
              "held": dict(HELD)[self.held_var.get()], "turn": bool(self.turn_var.get())}  # noqa
        for key, label in STAT_FIELDS:
            t = self.stat_vars[key].get().strip().replace(",", ".")
            if not t:
                st[key] = None
                continue
            try:
                st[key] = float(t)
            except ValueError:
                return None, f"{label}: type a number or leave it empty."
            top = 200 if key == "rigidity" else 100
            if not 0 <= st[key] <= top:
                return None, f"{label} should be between 0 and {top}."
        t = self.balance_var.get().strip().replace(",", ".")
        st["balance_cm"] = None
        if t:
            try:
                st["balance_cm"] = round(float(t) * lk, 1)
            except ValueError:
                return None, f"Balance point: type how far above the top of the grip it is ({lu}), or leave it empty."
        return st, None

    @staticmethod
    def stats_key(st):
        st = st or {}
        return tuple(st.get(k) for k in ("edges", "head", "edge_sharp", "tip_sharp", "rigidity", "balance_cm", "held")) + \
            (bool(st.get("turn")),)

    def inputs(self):
        """(length cm, mass kg text or "", type, flip): always metric inside, whatever is shown."""
        lu, lk, mu, mk = self.units()
        try:
            length = round(float(self.length_var.get().replace(",", ".")) * lk, 2)
        except ValueError:
            return None, ("Type the weapon's real length in centimetres, for example 120." if lu == "cm" else
                          "Type the weapon's real length in inches, for example 47.")
        if not 10 <= length <= 400:
            return None, f"The length should be between {self.fmt_len(10)} and {self.fmt_len(400)}."
        mass = self.mass_var.get().strip().replace(",", ".")
        if mass:
            try:
                kg = float(mass) * mk
            except ValueError:
                return None, ("Type the weight in kilograms, for example 1.8, or leave it empty." if mu == "kg" else
                              "Type the weight in pounds, for example 4, or leave it empty.")
            if not 0.05 <= kg <= 50:
                return None, f"The weight should be between {self.fmt_mass(0.05)} and {self.fmt_mass(50)}."
            mass = f"{kg:.4f}"
        it = self.cur
        st, problem = self.read_stats()
        if problem:
            return None, problem
        return (length, mass, dict(TYPES)[self.type_var.get()], self.flip, self.slide_var.get(),
                it["splits"] if it else None, self.lens_tuple(it), self.stats_key(st)), None

    def same_inputs(self, a, b):
        """Equal apart from unit-conversion rounding (switching units must not ask for a new Convert)."""
        if a is None or b is None:
            return a is b
        if a[2:] != b[2:] or bool(a[1]) != bool(b[1]):
            return False
        if abs(a[0] - b[0]) > 0.05:
            return False
        return not a[1] or abs(float(a[1]) - float(b[1])) <= 0.005

    def inputs_changed(self):
        if getattr(self, "converting_units", False) or self.quiet:
            return
        if self.cur is not None:
            self.save_editor()
        if self.result is not None and not self.same_inputs(self.inputs()[0], self.result_inputs):
            self.install_btn.config(state="disabled")
            self.status.config(text="Changed: press Preview again before installing.")
        self.schedule_first_preview()

    def schedule_first_preview(self):
        """The first picture comes by itself, a second after the length is known (no Preview press needed)."""
        if getattr(self, "first_preview_job", None):
            self.root.after_cancel(self.first_preview_job)
        self.first_preview_job = self.root.after(1000, self.first_preview)

    def first_preview(self):
        self.first_preview_job = None
        it = self.cur
        if it is None or it["result"] is not None or it.get("auto_previewed"):
            return
        if self.busy:  # still measuring or converting something else: try again shortly
            self.first_preview_job = self.root.after(1000, self.first_preview)
            return
        values, problem = self.inputs()
        if problem:
            return
        it["auto_previewed"] = True
        self.convert()

    # ---- converting ------------------------------------------------------------------------------------
    def convert(self):
        if self.busy:
            return
        if not self.model_var.get():
            self.status.config(text="Pick a model first.")
            return
        values, problem = self.inputs()
        if problem:
            self.status.config(text=problem)
            return
        self.clear_result()
        self.log.delete("1.0", "end")
        self.status.config(text="Making the preview...")
        it = self.cur
        it["status"] = "previewing..."
        self.refresh_row(it)
        # A quick pass (parts, collision, numbers; no textures or damage points): change things and preview
        # again as often as needed; the full conversion runs once, at Install (Matthew 2026-11-22).
        self.run(self.convert_worker, self.argv_for(it, values) + ["--preview"], values, it)

    def argv_for(self, it, values):
        length, mass, kind, flip, slide, splits, lens, stats = values
        argv = [it["path"], it["work"], "--length-cm", f"{length:g}", "--name", it["name"]]
        if mass:
            argv += ["--mass-kg", mass]
        if kind:
            argv += ["--type", kind]
        if flip:
            argv.append("--flip")
        if slide:
            argv.append("--hand-slide")
        if splits:
            argv += ["--splits", ",".join(f"{v * length:.2f}" for v in splits)]  # shares of the length -> cm
        if any(lens):
            argv += ["--part-lengths", ",".join(f"{v:.2f}" if v else "" for v in lens)]
        edges, head, edge_sharp, tip_sharp, rig, balance = stats[:6]
        held, turn = (stats[6], stats[7]) if len(stats) >= 8 else (None, False)
        if held:
            argv += ["--held", held]
        if turn:
            argv.append("--turn")
        for flag, v in (("--edges", edges), ("--head-kind", head)):
            if v:
                argv += [flag, v]
        for flag, v in (("--edge-sharp", edge_sharp), ("--tip-sharp", tip_sharp), ("--rigidity", rig),
                        ("--balance-cm", balance)):
            if v is not None:
                argv += [flag, f"{v:g}"]
        return argv

    def item_values(self, it):
        """The conversion inputs of an item that is not in the editor, or (None, problem)."""
        if it["length_cm"] is None:
            return None, "needs its real length"
        if not 10 <= it["length_cm"] <= 400:
            return None, "length out of range"
        mass = "" if it["mass_kg"] is None else f"{it['mass_kg']:.4f}"
        if mass and not 0.05 <= it["mass_kg"] <= 50:
            return None, "weight out of range"
        return (round(it["length_cm"], 2), mass, dict(TYPES)[it["type"]], it["flip"], it["slide"],
                it["splits"], self.lens_tuple(it), self.stats_key(it.get("stats"))), None

    def convert_all(self):
        if self.busy:
            return
        self.save_editor()
        todo, skipped = [], 0
        for it in self.items:
            if it["result"] is not None and self.same_inputs(self.item_values(it)[0], it["result_inputs"]):
                continue  # already converted with these settings
            values, problem = self.item_values(it)
            if problem:
                it["status"] = problem
                skipped += 1
            else:
                todo.append((it, values))
                it["status"] = "waiting..."
            self.refresh_row(it)
        if not todo:
            self.status.config(text="Nothing to convert." + (f" {skipped} need a length first." if skipped else ""))
            return
        self.log.delete("1.0", "end")
        self.status.config(text=f"Converting {len(todo)} weapons..." + (f" ({skipped} skipped: no length)" if skipped else ""))
        self.batch_running = True
        self.run(self.convert_all_worker, todo)

    def convert_all_worker(self, todo):
        for n, (it, values) in enumerate(todo, 1):
            self.events.put(("progress", it, f"converting ({n} of {len(todo)})..."))
            try:
                self.convert_worker(self.argv_for(it, values) + ["--preview"], values, it)
            except Exception as e:
                self.events.put(("item_failed", it, f"{e}"))
                self.events.put(("log", traceback.format_exc(), None))

    def convert_worker(self, argv, values, it):
        for f in os.listdir(it["work"]):
            os.remove(os.path.join(it["work"], f))

        # The conversion runs in a process of its own: a big model takes a few GB while converting, and a
        # process that ends gives all of it back (in this one it stayed taken, conversion after conversion).
        ctx = multiprocessing.get_context("spawn")
        q = ctx.Queue()
        proc = ctx.Process(target=convert_in_child, args=(argv, q), daemon=True)
        proc.start()
        problem, finished = None, False
        while not finished:
            try:
                kind, text = q.get(timeout=0.5)
            except queue.Empty:
                if not proc.is_alive():
                    problem = problem or f"the converter stopped unexpectedly (exit code {proc.exitcode})"
                    break
                continue
            if kind == "log":
                self.events.put(("log", text, None))
            else:
                problem, finished = (text if kind == "error" else None), True
        proc.join()
        q.close()
        q.join_thread()
        if problem:
            raise RuntimeError(problem)
        name, work = it["name"], it["work"]
        info = json.load(open(os.path.join(work, name + ".json"), encoding="utf-8"))
        pos, nrm, uv, tris = read_obj(os.path.join(work, name + ".obj"))
        zn = info["zones_cm"]
        zones = [("Pommel", zn["pommel_end"], zn["handle_bottom"]), ("Grip", zn["handle_bottom"], zn["guard_bottom"]),
                 ("Guard", zn["guard_bottom"], zn["guard_top"]), ("Head", zn["guard_top"], zn["tip"])]
        colour = None
        tex = os.path.join(work, name + "_BaseColor.png")
        if os.path.isfile(tex):
            from PIL import Image
            colour = Image.open(tex)
            colour.load()
        width = 900
        shield = info.get("type") == "shield"
        hf, he = (360, 90) if shield else (200, 70)
        s = view_scale(pos, width, (hf, he))
        flat = render_view(pos, nrm, uv, tris, colour, zones, "flat", width, hf, s, from_plus=shield)
        edge = render_view(pos, nrm, uv, tris, colour, zones, "edge", width, he, s)
        views = (flat, edge, zones, float(pos[:, 2].min()), s, width)
        self.events.put(("converted", (it, name, info, views), values))

    def converted(self, payload, values):
        it, name, info, views = payload
        it.update(result=(name, info), result_inputs=values, views=views)
        it.pop("pending_b", None)
        self.write_spots(it)
        it["status"] = "converted"
        self.refresh_row(it)
        if it is not self.cur:
            return
        self.result, self.result_inputs = (name, info), values
        self.result_views = views
        self.redraw_result()
        self.show_part_lengths()
        self.flip_btn.config(state="normal")
        self.install_btn.config(state="normal")
        self.status.config(text="Check the picture, then Install.")

    # ---- hand spots ---------------------------------------------------------------------------------
    # Kept as a share of the weapon's length (0 = pommel end, 1 = tip) so a new length keeps them in place.
    def spot_z(self, it, key):
        if it is None or it["result"] is None or key not in it["spots"]:
            return None
        zn = it["result"][1]["zones_cm"]
        return zn["pommel_end"] + it["spots"][key] * (zn["tip"] - zn["pommel_end"])

    def write_spots(self, it):
        if it["result"] is None:
            return
        name = it["result"][0]
        path = os.path.join(it["work"], name + ".txt")
        try:
            lines = [l for l in open(path, encoding="utf-8").read().splitlines() if not l.startswith("spot_")]
        except OSError:
            return
        for key, _, _ in SPOTS:
            z = self.spot_z(it, key)
            if z is not None:
                lines.append(f"spot_{key}={z:.2f}")
        with open(path, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")

    def refresh_spot_buttons(self):
        it = self.cur
        for key, label, _ in SPOTS:
            b = self.spot_buttons[key]
            z = self.spot_z(it, key)
            if it is None or it["result"] is None:
                b.config(text=f"{label}: auto", state="disabled")
                continue
            zn = it["result"][1]["zones_cm"]
            txt = "auto" if z is None else f"{self.fmt_len(z - zn['pommel_end'], 1)} up"
            b.config(text=("> " if self.placing == key else "") + f"{label}: {txt}", state="normal")

    def start_placing(self, key):
        if self.result is None:
            return
        self.placing = None if self.placing == key else key
        label = dict((k, l) for k, l, _ in SPOTS)[key]
        self.status.config(text=f"Click on the picture where the {label.lower()} should hold." if self.placing else "")
        self.refresh_spot_buttons()

    def clear_spot(self, key):
        it = self.cur
        if it is None or key not in it["spots"]:
            return
        del it["spots"][key]
        self.write_spots(it)
        self.redraw_result()
        self.status.config(text="Back to auto. Press Install to use it in the game.")

    def preview_clicked(self, event):
        it = self.cur
        if it is None or it["result"] is None or not self.result_views or self.busy:
            return
        if not self.placing:
            # Near a line between two parts (within 8 pixels): drag it.
            s = self.result_views[4]
            z = self.event_z(event)
            b = self.boundaries()
            near = [(abs(z - bz) * s, i) for i, bz in enumerate(b)]
            d, i = min(near)
            if d <= 8:
                # Lines on top of each other (no guard): the one on the side of the click.
                same = [j for dj, j in near if abs(dj - d) < 1e-6]
                if len(same) > 1:
                    i = same[0] if z < b[i] else same[-1]
                self.dragging = (i, b[i])
            return
        flat, edge, zones, zmin, s, width = self.result_views
        pw, ph = getattr(self, "pic_size", (width, 0))
        x = event.x - (self.preview.winfo_width() - pw) / 2
        z = zmin + (x - 10) / s
        zn = it["result"][1]["zones_cm"]
        if not zn["pommel_end"] - 1 <= z <= zn["tip"] + 1:
            self.status.config(text="That is off the weapon: click on it.")
            return
        z = min(max(z, zn["pommel_end"]), zn["tip"])
        it["spots"][self.placing] = (z - zn["pommel_end"]) / (zn["tip"] - zn["pommel_end"])
        label = dict((k, l) for k, l, _ in SPOTS)[self.placing]
        self.placing = None
        self.write_spots(it)
        self.redraw_result()
        self.status.config(text=f"{label} placed. Press Install to use it in the game.")

    # ---- part boundaries and lengths ----------------------------------------------------------------
    def lens_tuple(self, it):
        if it is None:
            return (0.0, 0.0, 0.0, 0.0)
        return tuple(round(it["part_len"].get(p, 0.0), 2) for p in PARTS)

    def result_part_lengths(self, info):
        zn = info["zones_cm"]
        return {"pommel": zn["handle_bottom"] - zn["pommel_end"], "grip": zn["guard_bottom"] - zn["handle_bottom"],
                "guard": zn["guard_top"] - zn["guard_bottom"], "blade": zn["tip"] - zn["guard_top"]}

    def show_part_lengths(self):
        it = self.cur
        k = self.units()[1]
        have = self.result_part_lengths(it["result"][1]) if it is not None and it["result"] is not None else {}
        self.quiet = True
        for p in PARTS:
            v = it["part_len"].get(p) if it is not None else None
            if v is None:
                v = have.get(p)
            self.len_vars[p].set("" if v is None else f"{v / k:.1f}")
        self.quiet = False

    def part_length_typed(self, part):
        it = self.cur
        if self.quiet or it is None or it["result"] is None:
            return
        try:
            cm = float(self.len_vars[part].get().replace(",", ".")) * self.units()[1]
        except ValueError:
            return
        now = self.result_part_lengths(it["result"][1])[part]
        if part not in it["part_len"] and abs(cm - now) < 0.06 * self.units()[1]:
            return  # just the length shown (unit rounding)
        if now < 0.05:
            self.status.config(text=f"This weapon has no {part} to stretch.")
            return
        it["part_len"][part] = max(0.5, cm)
        self.inputs_changed()

    def reset_parts(self):
        it = self.cur
        if it is None:
            return
        it["splits"], it["part_len"] = None, {}
        it.pop("pending_b", None)
        self.show_part_lengths()
        self.inputs_changed()
        self.status.config(text="Parts back to what the converter found. Press Preview.")

    def boundaries(self):
        """The part boundaries of the picture shown, cm along the weapon: pommel|grip, grip|guard, guard|blade."""
        if self.cur.get("pending_b"):  # lines moved since the last Convert
            return list(self.cur["pending_b"])
        zn = self.cur["result"][1]["zones_cm"]
        return [zn["handle_bottom"], zn["guard_bottom"], zn["guard_top"]]

    def event_z(self, event):
        flat, edge, zones, zmin, s, width = self.result_views
        pw, ph = getattr(self, "pic_size", (width, 0))
        x = event.x - (self.preview.winfo_width() - pw) / 2
        return zmin + (x - 10) / s

    def preview_dragged(self, event):
        if self.dragging is None or not self.result_views:
            return
        i, _ = self.dragging
        b = self.boundaries()
        zn = self.cur["result"][1]["zones_cm"]
        z = self.event_z(event)
        lo = b[i - 1] if i > 0 else zn["pommel_end"]
        hi = b[i + 1] if i < 2 else zn["tip"]
        self.dragging = (i, min(max(z, lo), hi))
        self.redraw_result()
        names = ("pommel | grip", "grip | guard", "guard | blade")
        self.status.config(text=f"Moving {names[i]}: {self.fmt_len(self.dragging[1] - zn['pommel_end'], 1)} up from the pommel end")

    def preview_released(self, event):
        if self.dragging is None:
            return
        i, z = self.dragging
        self.dragging = None
        it = self.cur
        zn = it["result"][1]["zones_cm"]
        b = self.boundaries()
        if abs(b[i] - z) < 0.05:
            self.redraw_result()
            return
        b[i] = z
        it["pending_b"] = list(b)  # shown until the next Convert, so several lines can be moved first
        # The splits go to the converter before any stretching, so a picture with stretched parts is
        # mapped back to the model's own lengths, part by part.
        p0 = zn["pommel_end"]
        shown = [p0, zn["handle_bottom"], zn["guard_bottom"], zn["guard_top"], zn["tip"]]  # the picture's own
        raw_lens = it["result"][1].get("lengths_before_stretch_cm")
        if raw_lens:
            raw = [p0]
            for l in raw_lens:
                raw.append(raw[-1] + l)
            def unstretch(v):
                for j in range(4):
                    lo, hi = shown[j], shown[j + 1]
                    if v <= hi + 1e-6 or j == 3:
                        k = raw_lens[j] / (hi - lo) if hi - lo > 1e-6 else 0.0
                        return raw[j] + (v - lo) * k
            b = [unstretch(v) for v in b]
        span = sum(raw_lens) if raw_lens else zn["tip"] - p0
        it["part_len"] = {}  # lengths typed earlier went with the old boundaries
        # Kept as shares of the whole length, so a new real length moves the lines with it.
        it["splits"] = tuple(round((v - p0) / span, 5) for v in b)
        self.show_part_lengths()
        # No Convert per line (Matthew 2026-11-22: moving pommel and guard took two waits): move them all, then
        # press Preview once.
        self.redraw_result()
        self.inputs_changed()
        self.status.config(text="Line moved. Move any others, then press Preview.")

    def short_summary(self, info):
        zn = info["zones_cm"]
        return (f"{info['type']}, {self.fmt_len(zn['tip'] - zn['pommel_end'])}, "
                f"{self.fmt_mass(info['mass_kg'])}{' (estimated)' if info['mass_note'] != 'given' else ''}"
                f"{self.sword_class_text(info)}")

    def sword_class_text(self, info):
        """For a sword: which game sword carries it (the choice, or what the mod picks by itself: kind_for in
        mod/HalfSwordCustomWeapon/src/main.cpp, 55 cm overall and the 14 cm two_hand_grip_cm default)."""
        if info.get("type") in ("hafted", "double"):
            names = {"one": "held in one hand (mace carrier)", "two": "held in two hands (great axe carrier)",
                     "spear": "spear grip (spear carrier)"}
            chosen = dict(HELD).get(self.held_var.get())
            if chosen:
                return f", {names[chosen]}"
            zn = info["zones_cm"]
            total = zn["tip"] - zn["pommel_end"]
            k = "one" if total < 90 else "spear" if info["type"] == "double" or total >= 160 else "two"
            return f", {names[k]} (detected by length; change it under Held with)"
        if info.get("type") != "sword":
            return ""
        names = {"dagger": "dagger", "arming": "one-handed sword", "bastard": "bastard sword",
                 "longsword": "longsword/greatsword"}
        chosen = dict(SWORD_CLASSES).get(self.class_var.get())
        if chosen:
            return f", game class {names[chosen]}"
        zn = info["zones_cm"]
        total, grip = zn["tip"] - zn["pommel_end"], -zn.get("handle_bottom", zn["pommel_end"])
        k = "dagger" if total < 55 else "arming" if grip < 14 else "bastard" if grip < 20 else "longsword"
        return f", game class {names[k]} (detected)"

    def damage_text(self, info):
        """What the converter found or was told for the damage, in words."""
        dm = info.get("damage") or {}
        zn = info["zones_cm"]
        if not dm:
            return ""
        kind = dm.get("kind")
        if kind == "preview":
            return "\nDamage: worked out at Install (this is the quick preview: no textures yet)."
        if kind == "sword":
            edges = {"double": "both edges cut", "single+": "one edge cuts", "single-": "one edge cuts",
                     "none": "no edge (blunt)"}.get(dm.get("edges"), "")
            what = (edges if dm.get("edges") == "none" else f"{edges}, edge sharpness {dm.get('sEdge')}") + \
                f", point {dm.get('sTip')}"
            if dm.get("tip_angle") is not None:
                what += f" (point angle {dm['tip_angle']:.0f} degrees)"
        elif kind == "shield":
            what = f"shield bash, point {dm.get('sTip')}"
        elif str(kind).startswith("flail"):
            what = (f"{dm.get('spikes')} spikes" if dm.get("spikes") else "round head (blunt)")
        elif kind == "spear":
            what = f"spear point {dm.get('sTip')}, edges {dm.get('sEdge')}"
        elif kind == "axe":
            what = f"{'double-bit axe (both blades cut)' if dm.get('double_bit') else 'axe blade'}, edge sharpness {dm.get('sEdge')}"
        else:
            what = "blunt head"
        what += f", rigidity {dm.get('rig')}"
        bal = zn.get("balance")
        if bal is not None:
            what += f"; balance point {self.fmt_len(abs(bal), 1)} {'above' if bal >= 0 else 'below'} the top of the grip"
        return "\nDamage: " + what + "."

    def redraw_result(self):
        """Picture bar and summary text in the units shown (the conversion itself is unit-free)."""
        from PIL import Image, ImageTk
        name, info = self.result
        flat, edge, zones, zmin, s, width = self.result_views
        L = self.fmt_len
        bar = zone_bar(zones, zmin, s, width, info["type"] in ("hafted", "flail"), L, bool(info["zones_cm"].get("double")),
                       info["type"] == "flail")
        pic = Image.new("RGB", (width, flat.height + edge.height + bar.height), (34, 36, 40))
        pic.paste(flat, (0, 0))
        pic.paste(edge, (0, flat.height))
        pic.paste(bar, (0, flat.height + edge.height))
        self.pic_size = pic.size
        zn = info["zones_cm"]
        from PIL import ImageDraw
        d = ImageDraw.Draw(pic)
        for key, label, colour in SPOTS:
            z = self.spot_z(self.cur, key)
            if z is None:
                continue
            x = 10 + (z - zmin) * s
            d.line([(x, 4), (x, flat.height + edge.height - 2)], fill=colour, width=3)
            d.text((x + 4, 4 + 12 * [k for k, _, _ in SPOTS].index(key)), label, fill=colour)
        if self.cur is not None and self.cur["result"] is not None:
            y0 = flat.height + edge.height
            for i, bz in enumerate(self.boundaries()):
                if self.dragging is not None and self.dragging[0] == i:
                    bz = self.dragging[1]
                    xd = 10 + (bz - zmin) * s
                    d.line([(xd, 0), (xd, pic.size[1])], fill=(255, 255, 255), width=2)
                x = 10 + (bz - zmin) * s
                d.rectangle([x - 3, y0, x + 3, y0 + 14], fill=(240, 240, 240))
        self.refresh_spot_buttons()
        photo = ImageTk.PhotoImage(pic)
        self.photos = [photo]
        self.preview.config(image=photo)
        zn = info["zones_cm"]
        hafted = info["type"] == "hafted"
        how = ("set by hand" if self.cur is not None and (self.cur["splits"] or self.cur["part_len"]) else
               "found by the model's part names" if info["parts_by_name"] else "found from the model's shape")
        mass = self.fmt_mass(info["mass_kg"])
        if info["mass_note"] != "given":
            mass += (" (estimated: type the real weight if you know it)" if info["mass_kg"] <= 6 else
                     " (estimated, and heavier than any real weapon: the model is probably much thicker than "
                     "the real thing. Type a weight, for example " +
                     ("1.5 for a sword or 2 for an axe)" if self.units()[2] == "kg" else "3.3 for a sword or 4.4 for an axe)"))
        if info["type"] == "crossbow":
            self.summary.config(text=f"Crossbow, {L(zn['tip'] - zn['pommel_end'])} long. In the game your model is "
                                     "the look; the body, bow arms and string that shoot are the game's crossbow's "
                                     "(its weight too).\nThe picture shows it from the side, front on the right, "
                                     "bow on top. If it is upside down, press \"Blade is on the wrong end\".")
            return
        if info["type"] == "flail":
            self.summary.config(text=f"Flail, {L(zn['tip'] - zn['pommel_end'])}, {mass}.\nStaff {L(-zn['pommel_end'])}, "
                                     f"chain {L(zn['guard_top'])}, head {L(zn['tip'] - zn['guard_top'])} (laid out "
                                     "straight; in the game the chain swings at the staff and at the head)."
                                     + self.damage_text(info))
            return
        if info["type"] == "shield":
            self.summary.config(text=f"Shield, {L(zn['width'])} wide, {L(zn['height'])} high, {mass}."
                                     + self.damage_text(info) + "\nThe picture shows its front, then its side with "
                                     "the front at the top. If the front is the back, press \"Blade is on the "
                                     "wrong end\".")
            return
        if zn.get("double"):
            parts = (f"lower head {L(zn['handle_bottom'] - zn['pommel_end'])}, haft {L(-zn['handle_bottom'])}, "
                     f"upper head {L(zn['tip'])}")
        elif hafted:
            parts = (f"haft {L(-zn['handle_bottom'])}, head {L(zn['tip'])}"
                     + (f", butt cap {L(zn['handle_bottom'] - zn['pommel_end'])}" if zn['handle_bottom'] > zn['pommel_end'] + 0.5 else ""))
        else:
            parts = (f"pommel {L(zn['handle_bottom'] - zn['pommel_end'])}, grip {L(-zn['handle_bottom'])}, "
                     f"guard {L(zn['guard_top'], 1)}, blade {L(zn['tip'] - zn['guard_top'])}")
        self.summary.config(text=f"{'Double-ended weapon' if zn.get('double') else 'Hafted weapon' if hafted else 'Sword'}, {L(zn['tip'] - zn['pommel_end'])}, "
                                 f"{mass}.\nParts ({how}): {parts}."
                                 + (" Main hand slides down the handle in swings." if zn.get("hand_slide") else "")
                                 + self.damage_text(info) + "\n"
                                 "The handle is on the left and the " + ("head" if hafted else "blade") +
                                 " on the right. If the picture shows it the other way round, press "
                                 "\"Blade is on the wrong end\".")

    def toggle_flip(self):
        self.flip = not self.flip
        self.convert()

    def toggle_details(self):
        self.details_shown = not self.details_shown
        if self.details_shown:
            self.log.grid(row=11, column=0, columnspan=3, sticky="nsew", padx=8, pady=4)
        else:
            self.log.grid_remove()
        self.details_btn.config(text="Hide details" if self.details_shown else "Show details")

    def clear_result(self):
        self.result = None
        self.result_inputs = None
        self.result_views = None
        if self.cur is not None:
            self.cur.update(result=None, result_inputs=None, views=None)
        self.photos = []
        self.preview.config(image="")
        self.summary.config(text="")
        self.flip_btn.config(state="disabled")
        self.install_btn.config(state="disabled")
        self.placing = None
        if hasattr(self, "spot_buttons"):
            self.refresh_spot_buttons()

    # ---- installing ------------------------------------------------------------------------------------
    def full_worker(self, todo):
        """The full conversion (textures, damage points, real volume) of weapons shown as previews."""
        for n, (it, values) in enumerate(todo, 1):
            self.events.put(("progress", it, f"converting for the game ({n} of {len(todo)})..."))
            it["full_ok"] = False
            self.convert_worker(self.argv_for(it, values), values, it)  # raises on failure
            it["full_ok"] = True

    def needs_full(self, items):
        """(item, inputs) to convert for the game: previews, and items changed since (lines moved, ...)."""
        out = []
        for it in items:
            if it["result"] is None:
                continue
            values = self.inputs()[0] if it is self.cur else self.item_values(it)[0]
            values = values or it["result_inputs"]
            if (it["result"][1].get("preview") or it.get("full_ok") is False
                    or not self.same_inputs(values, it["result_inputs"])):
                out.append((it, values))
        for it, _ in out:
            it["full_ok"] = False
        return out

    def install(self):
        if self.result is None or self.busy:
            return
        self.save_editor()
        todo = self.needs_full([self.cur])
        if todo:
            self.pending_install = ("one", [self.cur])
            self.status.config(text="Converting for the game (textures, damage)... then it installs.")
            self.run(self.full_worker, todo)
            return
        self.finish_install_one(self.cur)

    def finish_install_one(self, it):
        if self.copy_items([it]):
            title = it["title"]
            where = self.where_text(it)
            self.status.config(text=f"Installed: {title} ({where}).")
            messagebox.showinfo("Weapon Converter", f"{title} is installed as a new weapon ({where}).\n\n"
                                "Start Half Sword (or go back to its menu) to find it.")

    def where_text(self, it):
        places = [p for p, on in (("free play", it["free_play"]), ("progression merchant", it["progression"])) if on]
        return " and ".join(places) if places else "in neither free play nor progression: tick one to use it"

    def install_all(self):
        if self.busy:
            return
        self.save_editor()
        done = [it for it in self.items if it["result"] is not None]
        if not done:
            self.status.config(text="Preview the weapons first (Preview all).")
            return
        todo = self.needs_full(done)
        if todo:
            self.pending_install = ("all", done)
            self.status.config(text=f"Converting {len(todo)} weapons for the game... then they install.")
            self.run(self.full_worker, todo)
            return
        self.finish_install_all(done)

    def finish_install_all(self, done):
        if self.copy_items(done):
            left = len(self.items) - len(done)
            self.status.config(text=f"Installed {len(done)} weapons." + (f" {left} not converted yet." if left else ""))
            messagebox.showinfo("Weapon Converter", f"{len(done)} weapons are installed as new weapons.\n\n"
                                "Start Half Sword (or go back to its menu) to find them.")

    def copy_items(self, items):
        mod_dir = self.mod_var.get()
        if not mod_dir or not os.path.isdir(mod_dir):
            messagebox.showerror("Weapon Converter", "Pick the HalfSwordCustomWeapon mod folder first.")
            return False
        self.save_editor()
        try:
            for it in items:
                write_pack(mod_dir, it)
                it["status"] = "installed"
                self.refresh_row(it)
        except OSError as e:
            messagebox.showerror("Weapon Converter", f"Could not write to the mod folder:\n{e}")
            return False
        return True

    # ---- plumbing --------------------------------------------------------------------------------------
    def run(self, fn, *args):
        self.busy = True
        self.convert_btn.config(state="disabled")
        self.convert_all_btn.config(state="disabled")
        self.install_all_btn.config(state="disabled")
        self.root.config(cursor="watch")

        def worker():
            try:
                fn(*args)
            except Exception as e:
                self.events.put(("failed", e, traceback.format_exc()))
            finally:
                self.events.put(("done", None, None))
        threading.Thread(target=worker, daemon=True).start()

    def poll(self):
        try:
            while True:
                kind, a, b = self.events.get_nowait()
                try:
                    self.handle(kind, a, b)
                except Exception as e:
                    self.log.insert("end", traceback.format_exc(), "error")
                    self.status.config(text=f"That did not work: {e}")
        except queue.Empty:
            pass
        self.root.after(100, self.poll)

    def handle(self, kind, a, b):
        if kind == "log":
            self.log.insert("end", a)
            self.log.see("end")
        elif kind == "measured":
            self.measured(a, b)
        elif kind == "measure_failed":
            it = self.item_of(a)
            if it is not None:
                it["status"] = f"cannot read it: {b}"
                self.refresh_row(it)
                if it is self.cur:
                    self.model_info.config(text=f"That model could not be read: {b}")
        elif kind == "progress":
            a["status"] = b
            self.refresh_row(a)
            self.status.config(text=f"{a['name']}: {b}")
        elif kind == "item_failed":
            a["status"] = f"failed: {b}"
            self.refresh_row(a)
        elif kind == "converted":
            self.converted(a, b)
        elif kind == "failed":
            self.log.insert("end", b, "error")
            self.status.config(text=f"That did not work: {a}")
            if not self.details_shown:
                self.toggle_details()
        elif kind == "done":
            self.busy = False
            self.convert_btn.config(state="normal")
            self.convert_all_btn.config(state="normal")
            self.install_all_btn.config(state="normal")
            pend, self.pending_install = getattr(self, "pending_install", None), None
            if pend:
                which, items = pend
                if all(it.get("full_ok", True) and it["result"] is not None and not it["result"][1].get("preview")
                       for it in items):
                    if which == "one":
                        self.finish_install_one(items[0])
                    else:
                        self.finish_install_all(items)
                else:
                    self.status.config(text="The conversion for the game did not finish: nothing was installed.")
            if self.batch_running:
                self.batch_running = False
                ok = sum(1 for it in self.items if it["result"] is not None)
                self.status.config(text=f"{ok} of {len(self.items)} weapons converted. Pick one in the list to check "
                                        "its picture, then \"Install all converted\".")
            self.root.config(cursor="")

    def close(self):
        shutil.rmtree(self.work_root, ignore_errors=True)
        self.root.destroy()


def convert_in_child(argv, q):
    """Runs model_to_weapon in the conversion process; its printout and the outcome go back through q."""
    class Out(io.TextIOBase):
        def write(s, text):
            q.put(("log", text))
            return len(text)
    sys.stdout = sys.stderr = Out()  # the windowed exe has no console to print to
    try:
        model_to_weapon.main(argv)
        q.put(("done", None))
    except SystemExit as e:
        q.put(("done", None) if e.code in (None, 0) else ("error", str(e.code)))
    except Exception:
        q.put(("error", traceback.format_exc()))


def main():
    multiprocessing.freeze_support()  # the exe starts itself again for each conversion
    root = TkinterDnD.Tk() if TkinterDnD is not None else tk.Tk()
    try:
        ttk.Style(root).theme_use("vista")
    except tk.TclError:
        pass
    App(root, sys.argv[1:] or None)  # several files dropped on the exe arrive as several arguments
    root.mainloop()


if __name__ == "__main__":
    main()
