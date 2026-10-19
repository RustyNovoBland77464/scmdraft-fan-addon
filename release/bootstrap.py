#!/usr/bin/env python3
"""Fetch exact upstream revisions into a clean source export; never touches game data."""
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent

def run(*args):
    subprocess.run(args, check=True)

def main():
    deps = json.loads((ROOT / "release/dependencies.json").read_text())
    for name, info in deps.items():
        dest = ROOT / "third_party" / name
        if dest.exists():
            head = subprocess.check_output(["git", "-C", str(dest), "rev-parse", "HEAD"], text=True).strip()
            if head != info["commit"]:
                raise SystemExit(f"{dest}: unexpected revision; refusing to replace existing files")
            print(f"{name}: pinned revision already present")
            continue
        dest.parent.mkdir(parents=True, exist_ok=True)
        run("git", "init", str(dest))
        run("git", "-C", str(dest), "remote", "add", "origin", info["url"])
        run("git", "-C", str(dest), "fetch", "--depth=1", "origin", info["commit"])
        run("git", "-C", str(dest), "checkout", "--detach", "FETCH_HEAD")
    print("Dependencies ready. OpenBW licensing is still a pre-publication item; see RELEASE_READINESS.md.")

if __name__ == "__main__":
    main()
