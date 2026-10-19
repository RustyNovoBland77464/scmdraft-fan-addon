#!/usr/bin/env python3
"""Create local draft source/binary ZIPs from an explicit file allowlist. Does not publish."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parent.parent
DOCUMENTATION_MEDIA = {
    "docs/media/hd-graphics.png", "docs/media/terrain-mirroring.png",
    "docs/media/hitboxes.png", "docs/media/mining-boosts.png",
}
DOCS = {"README.md": "README.md", "INSTALL.md": "INSTALL.md", "BUILDING.md": "BUILDING.md",
        "CREDITS.md": "CREDITS.md", "LICENSE": "LICENSE", "THIRD_PARTY_NOTICES.md": "THIRD_PARTY_NOTICES.md",
        "RELEASE_READINESS.md": "RELEASE_READINESS.md", "CHANGELOG.md": "CHANGELOG.md",
        "CONTRIBUTING.md": "CONTRIBUTING.md"}
BAD_TEXT = re.compile(b"/" + rb"Users/[^/\s]+/|/" + rb"home/[^/\s]+/|-----BEGIN (?:RSA |OPENSSH )?PRIVATE KEY-----|gh[pousr]_[A-Za-z0-9]{30,}")

def copy(src, dest):
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dest)

def audit_source(tree):
    for path in tree.rglob("*"):
        if not path.is_file():
            continue
        rel = path.relative_to(tree)
        if path.is_symlink() or any(part in {".git", "re", "sample", "shots", "studies", "__pycache__"} for part in rel.parts):
            raise ValueError(f"Forbidden source path: {rel}")
        documentation_image = rel.as_posix() in DOCUMENTATION_MEDIA
        if documentation_image and not path.read_bytes().startswith(b"\x89PNG\r\n\x1a\n"):
            raise ValueError(f"Invalid documentation PNG: {rel}")
        if not documentation_image and path.suffix.lower() in {".exe", ".dll", ".sdp", ".mpq", ".scx", ".scm", ".chk", ".png", ".dds", ".anim", ".dmp"}:
            raise ValueError(f"Binary/game file in source export: {rel}")
        if BAD_TEXT.search(path.read_bytes()):
            raise ValueError(f"Private path or credential-like text: {rel}")

def hashes(tree):
    return {p.relative_to(tree).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(tree.rglob("*")) if p.is_file() and p.name != "SHA256SUMS.txt"}

def seal(tree):
    (tree / "SHA256SUMS.txt").write_text("".join(f"{digest}  {name}\n" for name, digest in hashes(tree).items()))
    archive = Path(str(tree) + ".zip")
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
        for path in sorted(tree.rglob("*")):
            if path.is_file():
                z.write(path, path.relative_to(tree.parent))
    print(archive)
    return archive

def notices(tree):
    files = {
        "CascLib-MIT.txt": "third_party/CascLib/LICENSE",
        "MinHook-BSD.txt": "third_party/minhook/LICENSE.txt",
        "Asio-Boost.txt": "third_party/openbw/deps/asio/LICENSE_1_0.txt",
        # Preserve embedded component declarations too; unresolved full texts are tracked in readiness.
        "CascLib-zlib-header.txt": "third_party/CascLib/src/zlib/zlib.h",
        "CascLib-SHA1-header.txt": "third_party/CascLib/src/hashes/sha1.h",
        "CascLib-MD5-header.txt": "third_party/CascLib/src/hashes/md5.h",
        "CascLib-AES-header.txt": "third_party/CascLib/src/overwatch/aes.h",
        "CascLib-lookup3-notice.txt": "third_party/CascLib/src/jenkins/lookup.h",
    }
    for dest, src in files.items():
        copy(ROOT / src, tree / "licenses" / dest)

def documents(tree):
    for src, dest in DOCS.items():
        copy(ROOT / "release/docs" / src, tree / dest)
    copy(ROOT / "release/dependencies.json", tree / "DEPENDENCIES.json")
    for rel in sorted(DOCUMENTATION_MEDIA):
        copy(ROOT / rel, tree / rel)

def prepare(out, build=None):
    if build:
        # Validate the complete payload before creating any export directories.
        for name in ("scmhd.sdp", "minesim.exe", "fan-edit-setup.exe"):
            data = (build / name).read_bytes()
            if len(data) < 64 or data[:2] != b"MZ":
                raise ValueError(f"Not a Windows payload: {name}")
            pe = struct.unpack_from("<I", data, 0x3c)[0]
            if pe + 6 > len(data) or data[pe:pe+4] != b"PE\0\0" or struct.unpack_from("<H", data, pe+4)[0] != 0x14c:
                raise ValueError(f"Not a 32-bit x86 PE payload: {name}")
    # Never erase an existing export or write into the source checkout.
    out = out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    trees = [out / "scmdraft-fan-edit-0.1.0-draft-source"]
    if build:
        trees.append(out / "scmdraft-fan-edit-0.1.0-draft-windows-x86")
    if any(t.exists() or Path(str(t) + ".zip").exists() for t in trees):
        raise ValueError("Output already exists. Choose a fresh --out folder; existing packages are preserved.")
    source = trees[0]
    source.mkdir()
    for folder in ("hd/src", "hd/tests", "release"):
        for path in sorted((ROOT / folder).rglob("*")):
            if path.is_file() and not path.is_symlink() and "__pycache__" not in path.parts and path.suffix != ".pyc":
                copy(path, source / path.relative_to(ROOT))
    for path in ("minesim/minesim.cpp", "minesim/boost_test.cpp", "minesim/boost_study.h", "third_party/mingw32.cmake"):
        copy(ROOT / path, source / path)
    documents(source)
    (source / ".gitignore").write_text("/build/\n/dist/\n/third_party/CascLib/\n/third_party/minhook/\n/third_party/openbw/\n__pycache__/\n*.log\n.DS_Store\n")
    copy(ROOT / "release/ci.yml", source / ".github/workflows/build.yml")
    revision = subprocess.check_output(["git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True).strip() if (ROOT / ".git").exists() else "exported-source"
    (source / "SOURCE_MANIFEST.json").write_text(json.dumps({"version": "0.1.0-draft", "base_revision": revision,
        "includes_working_tree": True, "status": "LOCAL DRAFT — publication items in RELEASE_READINESS.md",
        "files": hashes(source)}, indent=2) + "\n")
    audit_source(source)
    archives = [seal(source)]
    if build:
        binary = trees[1]
        binary.mkdir()
        documents(binary)
        notices(binary)
        copy(source / "SOURCE_MANIFEST.json", binary / "SOURCE_MANIFEST.json")
        for name in ("scmhd.sdp", "minesim.exe"):
            copy(build / name, binary / "plugins" / name)
        copy(build / "fan-edit-setup.exe", binary / "fan-edit-setup.exe")
        (binary / "DRAFT-NOT-FOR-DISTRIBUTION.txt").write_text(
            "Local release candidate. Resolve RELEASE_READINESS.md before publishing.\n"
            "No original ScmDraft executable or Blizzard assets are included.\n")
        archives.append(seal(binary))
    (out / "SHA256SUMS.txt").write_text("".join(f"{hashlib.sha256(a.read_bytes()).hexdigest()}  {a.name}\n" for a in archives))
    return source

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--build", type=Path, help="CMake build directory containing the three Windows release files")
    args = parser.parse_args()
    prepare(args.out, args.build.resolve() if args.build else None)

if __name__ == "__main__":
    main()
