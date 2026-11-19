"""Builds the end-user zip: python tools/package_custom_weapon.py <main.dll> <WeaponConverter.exe> <out.zip>

Layout: HalfSwordCustomWeapon/{dlls/main.dll, CustomWeapon.ini, enabled.txt, test_blade.obj, README.md},
WeaponConverter.exe, READ ME FIRST.txt. No third-party models go in."""
import os
import sys
import zipfile

dll, exe, out = sys.argv[1:4]
mod = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "mod", "HalfSwordCustomWeapon")
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    z.write(dll, "HalfSwordCustomWeapon/dlls/main.dll")
    for f in ("CustomWeapon.ini", "test_blade.obj", "README.md"):
        z.write(os.path.join(mod, f), "HalfSwordCustomWeapon/" + f)
    z.writestr("HalfSwordCustomWeapon/enabled.txt", "")
    z.write(exe, "WeaponConverter.exe")
    z.write(os.path.join(mod, "release", "READ ME FIRST.txt"), "READ ME FIRST.txt")
for i in zipfile.ZipFile(out).infolist():
    print(f"{i.file_size:>10}  {i.filename}")
