#!/usr/bin/env python3
"""Run existing asset-free regressions and package policy tests; no editor restart."""
from pathlib import Path
import os
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="fan-edit-check-") as tmp:
    for src in sorted((ROOT / "hd/tests").glob("*_test.cpp")):
        exe = Path(tmp) / src.stem
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-O2", str(src), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
subprocess.run(["python3", "-m", "unittest", "discover", "-s", str(ROOT / "release/tests"), "-v"], check=True)
