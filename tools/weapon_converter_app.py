"""Weapon Converter: the end-user window around tools/model_to_weapon.py (built into WeaponConverter.exe).

Pick a weapon model (GLB, glTF, OBJ, ...), or drop it on the window or on WeaponConverter.exe. Type its
real length (and weight, if known), press Convert, check the picture, then Install. Install copies the
converted files into the HalfSwordCustomWeapon mod folder of your Half Sword install and makes it the
model the mod uses (model= in CustomWeapon.ini). Nothing reaches the mod folder before Install.
"""
import glob
import io
import json
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
MODEL_TYPES = "*.glb *.gltf *.obj *.nif *.stl *.ply *.dae *.off"
MODEL_EXTS = tuple(e[1:] for e in MODEL_TYPES.split())
TYPES = [("Detect", None), ("Sword or dagger", "sword"), ("Axe, mace, hammer or spear", "hafted")]
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


def set_model_in_ini(mod_dir, model_file):
    ini = os.path.join(mod_dir, "CustomWeapon.ini")
    lines = open(ini, encoding="utf-8").read().splitlines() if os.path.isfile(ini) else []
    done = False
    for i, line in enumerate(lines):
        if line.strip().startswith("model="):
            lines[i] = "model=" + model_file
            done = True
    if not done:
        lines.append("model=" + model_file)
    open(ini, "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")


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


def render_view(pos, nrm, uv, tris, colour_img, zones, view, width, height, s):
    """Shaded picture of the weapon lying left (pommel) to right (tip). view "flat": looking at the
    blade's flat (the edge direction is up); "edge": looking at its edge. Returns a PIL image."""
    from PIL import Image, ImageDraw
    up, depth = (0, 1) if view == "flat" else (1, 0)
    z = pos[:, 2]
    zmin = float(z.min())
    px = 10 + (z - zmin) * s
    py = height / 2 - pos[:, up] * s
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
    order = np.argsort(-pos[tris, depth].mean(axis=1))  # far side first
    xs, ys = px[tris], py[tris]
    for i in order:
        draw.polygon([(xs[i, 0], ys[i, 0]), (xs[i, 1], ys[i, 1]), (xs[i, 2], ys[i, 2])], fill=tuple(col[i]))
    return img


def zone_bar(zones, zmin, s, width, hafted, fmt_len=lambda cm: f"{cm:.0f} cm"):
    from PIL import Image, ImageDraw
    img = Image.new("RGB", (width, 26), (34, 36, 40))
    d = ImageDraw.Draw(img)
    names = {"Pommel": "butt" if hafted else "pommel", "Grip": "haft" if hafted else "grip", "Guard": "guard",
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
        self.work = tempfile.mkdtemp(prefix="hs_weapon_")
        self.events = queue.Queue()
        self.busy = False
        self.flip = False
        self.result = None  # (name, json) of the conversion now in self.work
        self.result_inputs = None
        self.raw_length = None
        self.photos = []

        root.title("Half Sword Weapon Converter")
        root.minsize(760, 560)
        root.protocol("WM_DELETE_WINDOW", self.close)
        pad = {"padx": 8, "pady": 4}
        main = ttk.Frame(root, padding=10)
        main.pack(fill="both", expand=True)
        main.columnconfigure(1, weight=1)

        ttk.Label(main, text="Model").grid(row=0, column=0, sticky="w", **pad)
        self.model_var = tk.StringVar()
        ttk.Entry(main, textvariable=self.model_var, state="readonly").grid(row=0, column=1, sticky="ew", **pad)
        ttk.Button(main, text="Browse...", command=self.browse).grid(row=0, column=2, **pad)
        self.model_info = ttk.Label(main, text="Pick a weapon model" + (", or drop one on this window." if TkinterDnD else "."))
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
        ttk.Label(form, text="Units").grid(row=3, column=0, sticky="w", **pad)
        unit_name = self.settings.get("units")
        self.units_var = tk.StringVar(value=unit_name if unit_name in UNIT_SYSTEMS else next(iter(UNIT_SYSTEMS)))
        self.units_shown = self.units_var.get()
        ttk.Combobox(form, textvariable=self.units_var, values=list(UNIT_SYSTEMS), state="readonly",
                     width=28).grid(row=3, column=1, columnspan=2, sticky="w", **pad)
        self.units_var.trace_add("write", lambda *_: self.units_changed())
        self.show_unit_labels()
        for var in (self.length_var, self.mass_var, self.type_var):
            var.trace_add("write", lambda *_: self.inputs_changed())

        ttk.Label(main, text="Mod folder").grid(row=4, column=0, sticky="w", **pad)
        self.mod_var = tk.StringVar(value=self.settings.get("mod_dir") or find_mod_folder() or "")
        ttk.Entry(main, textvariable=self.mod_var, state="readonly").grid(row=4, column=1, sticky="ew", **pad)
        ttk.Button(main, text="Change...", command=self.pick_mod).grid(row=4, column=2, **pad)
        self.mod_info = ttk.Label(main, text="")
        self.mod_info.grid(row=5, column=1, columnspan=2, sticky="w", padx=8)
        self.check_mod()

        self.convert_btn = ttk.Button(main, text="Convert", command=self.convert)
        self.convert_btn.grid(row=6, column=0, columnspan=3, pady=8)

        self.preview = ttk.Label(main, anchor="center")
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
            root.dnd_bind("<<Drop>>", lambda e: self.set_model(root.tk.splitlist(e.data)[0]))
        root.after(100, self.poll)
        if first_model:
            root.after(200, lambda: self.set_model(first_model))

    # ---- inputs ----------------------------------------------------------------------------------------
    def browse(self):
        path = filedialog.askopenfilename(title="Pick a weapon model", initialdir=self.settings.get("model_dir"),
                                          filetypes=[("3D models", MODEL_TYPES), ("All files", "*.*")])
        if path:
            self.set_model(path)

    def set_model(self, path):
        if self.busy:
            return
        if os.path.isdir(path):  # a downloaded mod folder: the model files in it
            found = sorted(os.path.join(r, f) for r, _, fs in os.walk(path) for f in fs
                           if f.lower().endswith(MODEL_EXTS))
            if not found:
                self.model_info.config(text=f"No model files ({', '.join(MODEL_EXTS)}) in that folder.")
                return
            if len(found) > 1:
                self.choose_model(path, found)
                return
            path = found[0]
        if not os.path.isfile(path):
            return
        self.settings["model_dir"] = os.path.dirname(path)
        save_settings(self.settings)
        self.model_var.set(path)
        self.flip = False
        self.raw_length = None
        self.length_var.set("")
        self.clear_result()
        for w in self.unit_buttons.winfo_children():
            w.destroy()
        self.model_info.config(text="Reading the model...")
        self.run(self.measure, path)

    def choose_model(self, folder, files):
        win = tk.Toplevel(self.root)
        win.title("Which model?")
        win.transient(self.root)
        ttk.Label(win, text="That folder has several models. Pick one:", padding=8).pack(anchor="w")
        lb = tk.Listbox(win, width=70, height=min(12, len(files)))
        for f in files:
            lb.insert("end", os.path.relpath(f, folder))
        lb.selection_set(0)
        lb.pack(fill="both", expand=True, padx=8)

        def ok(*_):
            sel = lb.curselection()
            win.destroy()
            if sel:
                self.set_model(files[sel[0]])
        lb.bind("<Double-Button-1>", ok)
        ttk.Button(win, text="Use this one", command=ok).pack(pady=8)
        win.grab_set()

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
        for var, k_old, k_new, digits in ((self.length_var, old[1], new[1], 2), (self.mass_var, old[3], new[3], 3)):
            try:
                v = float(var.get().replace(",", ".")) * k_old / k_new
                var.set(f"{v:.{digits}f}".rstrip("0").rstrip("."))
            except ValueError:
                pass
        self.converting_units = False
        self.show_unit_labels()
        if self.raw_length is not None and self.model_var.get():
            for w in self.unit_buttons.winfo_children():
                w.destroy()
            self.measured(self.model_var.get(), self.raw_length, keep_length=True)
        if self.result is not None:
            self.redraw_result()
        self.inputs_changed()

    def measure(self, path):
        raw = model_to_weapon.model_length(path)
        self.events.put(("measured", path, raw))

    def measured(self, path, raw, keep_length=False):
        if path != self.model_var.get():
            return
        self.raw_length = raw
        k = self.units()[1]
        if os.path.splitext(path)[1].lower() in (".glb", ".gltf"):
            self.model_info.config(text=f"The file makes this weapon {self.fmt_len(raw * 100, 1)} long. "
                                        "Downloaded models are often scaled wrong: check the length below.")
            if not keep_length:
                self.length_var.set(f"{raw * 100 / k:.1f}")
        else:
            self.model_info.config(text="This file does not say what unit it uses. Pick the one that gives a "
                                        "sensible length, or type the real length below:")
            units = UNITS + ([("Skyrim units", 1.42875)] if path.lower().endswith(".nif") else [])
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
        return (length, mass, dict(TYPES)[self.type_var.get()], self.flip), None

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
        if getattr(self, "converting_units", False):
            return
        if self.result is not None and not self.same_inputs(self.inputs()[0], self.result_inputs):
            self.install_btn.config(state="disabled")
            self.status.config(text="Changed: press Convert again before installing.")

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
        length, mass, kind, flip = values
        argv = [self.model_var.get(), self.work, "--length-cm", f"{length:g}"]
        if mass:
            argv += ["--mass-kg", mass]
        if kind:
            argv += ["--type", kind]
        if flip:
            argv.append("--flip")
        self.clear_result()
        self.log.delete("1.0", "end")
        self.status.config(text="Converting... (big models take a little while)")
        self.run(self.convert_worker, argv, values)

    def convert_worker(self, argv, values):
        for f in os.listdir(self.work):
            os.remove(os.path.join(self.work, f))

        class Out(io.TextIOBase):
            def write(s, text):
                self.events.put(("log", text, None))
                return len(text)

        old = sys.stdout
        sys.stdout = Out()
        try:
            model_to_weapon.main(argv)
        except SystemExit as e:
            raise RuntimeError(str(e))
        finally:
            sys.stdout = old
        name = os.path.splitext(os.path.basename(argv[0]))[0]
        info = json.load(open(os.path.join(self.work, name + ".json"), encoding="utf-8"))
        pos, nrm, uv, tris = read_obj(os.path.join(self.work, name + ".obj"))
        zn = info["zones_cm"]
        zones = [("Pommel", zn["pommel_end"], zn["handle_bottom"]), ("Grip", zn["handle_bottom"], zn["guard_bottom"]),
                 ("Guard", zn["guard_bottom"], zn["guard_top"]), ("Head", zn["guard_top"], zn["tip"])]
        colour = None
        tex = os.path.join(self.work, name + "_BaseColor.png")
        if os.path.isfile(tex):
            from PIL import Image
            colour = Image.open(tex)
            colour.load()
        width = 900
        s = view_scale(pos, width, (200, 70))
        flat = render_view(pos, nrm, uv, tris, colour, zones, "flat", width, 200, s)
        edge = render_view(pos, nrm, uv, tris, colour, zones, "edge", width, 70, s)
        views = (flat, edge, zones, float(pos[:, 2].min()), s, width)
        self.events.put(("converted", (name, info, views), values))

    def converted(self, payload, values):
        name, info, views = payload
        self.result, self.result_inputs = (name, info), values
        self.result_views = views
        self.redraw_result()
        self.flip_btn.config(state="normal")
        self.install_btn.config(state="normal")
        self.status.config(text="Check the picture, then Install.")

    def redraw_result(self):
        """Picture bar and summary text in the units shown (the conversion itself is unit-free)."""
        from PIL import Image, ImageTk
        name, info = self.result
        flat, edge, zones, zmin, s, width = self.result_views
        L = self.fmt_len
        bar = zone_bar(zones, zmin, s, width, info["type"] == "hafted", L)
        pic = Image.new("RGB", (width, flat.height + edge.height + bar.height), (34, 36, 40))
        pic.paste(flat, (0, 0))
        pic.paste(edge, (0, flat.height))
        pic.paste(bar, (0, flat.height + edge.height))
        photo = ImageTk.PhotoImage(pic)
        self.photos = [photo]
        self.preview.config(image=photo)
        zn = info["zones_cm"]
        hafted = info["type"] == "hafted"
        how = "found by the model's part names" if info["parts_by_name"] else "found from the model's shape"
        mass = self.fmt_mass(info["mass_kg"])
        if info["mass_note"] != "given":
            mass += (" (estimated: type the real weight if you know it)" if info["mass_kg"] <= 6 else
                     " (estimated, and heavier than any real weapon: the model is probably much thicker than "
                     "the real thing. Type a weight, for example " +
                     ("1.5 for a sword or 2 for an axe)" if self.units()[2] == "kg" else "3.3 for a sword or 4.4 for an axe)"))
        if hafted:
            parts = (f"haft {L(-zn['handle_bottom'])}, head {L(zn['tip'])}"
                     + (f", butt cap {L(zn['handle_bottom'] - zn['pommel_end'])}" if zn['handle_bottom'] > zn['pommel_end'] + 0.5 else ""))
        else:
            parts = (f"pommel {L(zn['handle_bottom'] - zn['pommel_end'])}, grip {L(-zn['handle_bottom'])}, "
                     f"guard {L(zn['guard_top'], 1)}, blade {L(zn['tip'] - zn['guard_top'])}")
        self.summary.config(text=f"{'Hafted weapon' if hafted else 'Sword'}, {L(zn['tip'] - zn['pommel_end'])}, "
                                 f"{mass}.\nParts ({how}): {parts}.\n"
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
        self.photos = []
        self.preview.config(image="")
        self.summary.config(text="")
        self.flip_btn.config(state="disabled")
        self.install_btn.config(state="disabled")

    # ---- installing ------------------------------------------------------------------------------------
    def install(self):
        if self.result is None or self.busy:
            return
        mod_dir = self.mod_var.get()
        if not mod_dir or not os.path.isdir(mod_dir):
            messagebox.showerror("Weapon Converter", "Pick the HalfSwordCustomWeapon mod folder first.")
            return
        name = self.result[0]
        try:
            for f in os.listdir(self.work):
                shutil.copy2(os.path.join(self.work, f), os.path.join(mod_dir, f))
            set_model_in_ini(mod_dir, name + ".obj")
        except OSError as e:
            messagebox.showerror("Weapon Converter", f"Could not write to the mod folder:\n{e}")
            return
        self.status.config(text=f"Installed: CustomWeapon.ini now uses {name}.obj.")
        messagebox.showinfo("Weapon Converter", f"{name} is installed.\n\nIn Half Sword, hold a sword in your "
                                                "right hand and press F9 to swap it for this weapon.")

    # ---- plumbing --------------------------------------------------------------------------------------
    def run(self, fn, *args):
        self.busy = True
        self.convert_btn.config(state="disabled")
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
            self.root.config(cursor="")

    def close(self):
        shutil.rmtree(self.work, ignore_errors=True)
        self.root.destroy()


def main():
    root = TkinterDnD.Tk() if TkinterDnD is not None else tk.Tk()
    try:
        ttk.Style(root).theme_use("vista")
    except tk.TclError:
        pass
    App(root, sys.argv[1] if len(sys.argv) > 1 else None)
    root.mainloop()


if __name__ == "__main__":
    main()
