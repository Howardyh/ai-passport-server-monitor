#!/usr/bin/env python3
"""Native Windows adapter for the official firmware gate; no device operations."""
import datetime
import os
from pathlib import Path
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
# The launcher is Git Bash, but this adapter and all build tools run as native
# Windows processes. Do not pass Bash's platform marker into native idf.py.
if os.name != "nt":
    raise SystemExit("This adapter is for native Windows Python only")
os.environ.pop("MSYSTEM", None)
idf = Path(os.environ["IDF_PATH"])
idf_py = idf / "tools/idf.py"
version = subprocess.check_output([sys.executable, str(idf_py), "--version"], text=True)
if "ESP-IDF v5.5.3" not in version:
    raise SystemExit("ESP-IDF 5.5.3 is required")
build = root / "build" / ("validation-" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))
build.mkdir(parents=True, exist_ok=False)
env = dict(os.environ, SDKCONFIG_DEFAULTS=str(root / "sdkconfig.defaults"), PYTHONDONTWRITEBYTECODE="1")


def run(*args):
    subprocess.run([sys.executable, *map(str, args)], cwd=root, env=env, check=True)


run(idf_py, "-B", build, "-D", f"SDKCONFIG={build / 'sdkconfig'}", "build")
run(idf_py, "-B", build, "merge-bin", "-o", build / "FoloToy-AI-Passport-full.bin")
run(root / "tools/verify_firmware.py", build)
run(root / "tools/archive_firmware.py", "create", build, "--archive-root", root / "build/firmware")
shutil.copy2(build / "FoloToy-AI-Passport-full.bin", root / "build/FoloToy-AI-Passport-full.bin")
run(idf_py, "-B", build, "size")
print("Firmware build: PASS")
print("Native validation directory:", build)
