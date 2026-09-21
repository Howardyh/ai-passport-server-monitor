#!/usr/bin/env python3
"""Render the actual product UI with pinned LVGL, fixed heap and synthetic state.
Requires a completed IDF dependency resolution, native C/C++ compilers and CMake.
No device operations; PPM screenshots remain under build/ui-validation.
"""
import os
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / "build/ui-validation"
out.mkdir(parents=True, exist_ok=True)
args = ["cmake", "-S", str(root / "tests/ui_host"), "-B", str(out), "-G", "Ninja"]
for env, cmake in (("CC", "CMAKE_C_COMPILER"), ("CXX", "CMAKE_CXX_COMPILER")):
    if os.environ.get(env):
        args.append("-D" + cmake + "=" + os.environ[env])
subprocess.run(args, check=True)
subprocess.run(["cmake", "--build", str(out), "-j", "2"], check=True)
subprocess.run([str(out / ("render.exe" if os.name == "nt" else "render"))], cwd=out, check=True)
print("LVGL host rendering: PASS (synthetic data; hardware validation pending)")
