#!/usr/bin/env python3
"""Host-only tests; never opens device ports."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
idf = Path(os.environ["IDF_PATH"])
json_dir = idf / "components/json/cJSON"
with tempfile.TemporaryDirectory(prefix="passport-monitor-tests-") as directory:
    binary = Path(directory) / ("state.exe" if os.name == "nt" else "state")
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-I"+str(root/"main"), "-I"+str(json_dir), str(root/"tests/test_server_state.c"),
        str(root/"main/server_state.c"), str(json_dir/"cJSON.c"), "-lm", "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
subprocess.run([sys.executable, str(root/"tests/test_status_agent.py")], check=True)
