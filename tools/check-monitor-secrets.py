#!/usr/bin/env python3
"""Scan staged text without printing suspected secret values."""
import re
import subprocess
from pathlib import PurePosixPath

files = subprocess.check_output(["git", "ls-files", "--cached", "-z"]).decode().split("\0")
patterns = (
    re.compile(r"(?:gh[pousr]_|github_pat_)[A-Za-z0-9_]{20,}"),
    re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----"),
    re.compile(r"(?i)Bearer[ \t]+[A-Za-z0-9._~+/=-]{20,}"),
    re.compile(r"(?im)^\s*(?:PASSPORT_STATUS_TOKEN|WIFI_PASSWORD)\s*=\s*(?!PLACEHOLDER\s*$|YOUR_[A-Z_]+\s*$)([^\r\n]{8,})"),
)
failures = []
keyword_files = 0
for name in filter(None, files):
    path = PurePosixPath(name)
    if "build" in path.parts or "private" in path.parts or path.suffix in {".pem", ".key", ".crt", ".p12", ".pfx"} or (path.name == ".env") or (path.name.startswith("private_config") and path.name != "private_config.example"):
        failures.append(name + ": forbidden file category")
        continue
    blob = subprocess.check_output(["git", "show", ":" + name])
    try: content = blob.decode("utf-8")
    except UnicodeDecodeError: continue
    if re.search(r"password|token|secret|Authorization|Bearer|private.key", content, re.I): keyword_files += 1
    if any(pattern.search(content) for pattern in patterns): failures.append(name + ": possible credential (value suppressed)")
if failures:
    raise SystemExit("\n".join(failures))
print(f"Staged secret scan: PASS; {keyword_files} files contain reviewed security keywords")
