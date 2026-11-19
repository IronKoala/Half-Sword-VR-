"""Weapon Converter: the end-user front of tools/model_to_weapon.py (built into WeaponConverter.exe).

Drag a sword model (GLB, glTF, OBJ, ...) onto WeaponConverter.exe, or start it and pick the file.
It converts the model straight into the HalfSwordCustomWeapon mod folder of your Half Sword install
and makes it the model the mod uses (model= in CustomWeapon.ini). In the game, hold a weapon in the
right hand and press F9.
"""
import glob
import os
import re
import sys
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import model_to_weapon  # noqa: E402

UNITS = [("m", "metres", 100.0), ("cm", "centimetres", 1.0), ("mm", "millimetres", 0.1), ("in", "inches", 2.54)]


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
    for l in libs:
        key = os.path.normcase(os.path.normpath(l))
        if key not in seen:
            seen.add(key)
            out.append(l)
    return out


def find_mod_folder():
    for lib in steam_libraries():
        game = os.path.join(lib, "steamapps", "common", "Half Sword")
        for mods in glob.glob(os.path.join(game, "*", "Binaries", "Win64", "ue4ss", "Mods")):
            return os.path.join(mods, "HalfSwordCustomWeapon")
    return None


def ask(prompt, choices):
    while True:
        a = input(prompt).strip().lower()
        if a in choices:
            return a
        print("  please type one of: " + ", ".join(choices))


def set_model_in_ini(mod_dir, model_file):
    ini = os.path.join(mod_dir, "CustomWeapon.ini")
    lines = open(ini, encoding="utf-8").read().splitlines() if os.path.isfile(ini) else []
    done = False
    for i, l in enumerate(lines):
        if l.strip().startswith("model="):
            lines[i] = "model=" + model_file
            done = True
    if not done:
        lines.append("model=" + model_file)
    open(ini, "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")


def main():
    print("Half Sword weapon converter\n")
    model = sys.argv[1] if len(sys.argv) > 1 else None
    if not model:
        try:
            import tkinter
            from tkinter import filedialog
            tkinter.Tk().withdraw()
            model = filedialog.askopenfilename(title="Pick a sword model",
                                               filetypes=[("3D models", "*.glb *.gltf *.obj *.stl *.ply"), ("All files", "*.*")])
        except Exception:
            model = input("Model file: ").strip().strip('"')
    if not model or not os.path.isfile(model):
        print("No model picked.")
        return

    mod_dir = find_mod_folder()
    if mod_dir is None or not os.path.isdir(mod_dir):
        print("Could not find the HalfSwordCustomWeapon mod in your Half Sword install.")
        print("Install the mod first (see READ ME FIRST.txt), or type the mod folder's path:")
        mod_dir = input("> ").strip().strip('"')
        if not os.path.isdir(mod_dir):
            print("That folder does not exist.")
            return
    print(f"Model:      {model}")
    print(f"Mod folder: {mod_dir}\n")

    # Converted into a scratch folder first; files reach the mod folder only after the user's yes.
    import shutil
    import tempfile
    work = tempfile.mkdtemp(prefix="hs_weapon_")
    argv = [model, work]
    ext = os.path.splitext(model)[1].lower()
    if ext not in (".glb", ".gltf"):
        # The file's numbers carry no unit: show the length each unit would give and let the user pick.
        import trimesh
        import numpy as np
        b = trimesh.load(model, force="scene", process=False).bounds
        length = float(np.max(b[1] - b[0]))
        print("This file does not say what unit it uses. The model would be this long:")
        for key, word, cm in UNITS:
            print(f"  {key:>2} ({word}): {length * cm:,.1f} cm")
        argv += ["--units", ask("Which looks right for this weapon? Type m, cm, mm or in: ", [u[0] for u in UNITS])]
    mass = input("Weight in kg (press Enter to estimate it from the model): ").strip().replace(",", ".")
    if mass:
        argv += ["--mass-kg", mass]

    while True:
        print()
        try:
            model_to_weapon.main(argv)
        except SystemExit as e:
            print(f"Stopped: {e}")
            return
        name = os.path.splitext(os.path.basename(model))[0]
        print("\nCheck the lines above: guard, blade side, handle bottom, mass.")
        a = ask("Looks right? y = use it, f = the blade is on the wrong end, q = quit: ", ["y", "f", "q"])
        if a == "y":
            break
        if a == "q":
            shutil.rmtree(work, ignore_errors=True)
            print("Nothing was changed.")
            return
        for f in os.listdir(work):
            os.remove(os.path.join(work, f))
        argv = [x for x in argv if x != "--flip"] + (["--flip"] if "--flip" not in argv else [])
    for f in os.listdir(work):
        shutil.copy2(os.path.join(work, f), os.path.join(mod_dir, f))
    shutil.rmtree(work, ignore_errors=True)
    set_model_in_ini(mod_dir, name + ".obj")
    print(f"\nDone. CustomWeapon.ini now uses {name}.obj.")
    print("In Half Sword: pick up a sword in your right hand and press F9.")


if __name__ == "__main__":
    try:
        main()
    except Exception:
        traceback.print_exc()
    input("\nPress Enter to close.")
