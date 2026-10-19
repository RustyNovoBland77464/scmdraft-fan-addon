import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location("prepare", Path(__file__).resolve().parents[1] / "prepare.py")
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)

class PackageChecks(unittest.TestCase):
    def test_source_export_excludes_private_material_and_preserves_existing_export(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = prepare.prepare(Path(tmp))
            names = {p.relative_to(source).as_posix() for p in source.rglob("*") if p.is_file()}
            self.assertIn("hd/src/live.cpp", names)
            self.assertIn("release/CMakeLists.txt", names)
            self.assertIn(".github/workflows/build.yml", names)
            self.assertTrue(prepare.DOCUMENTATION_MEDIA.issubset(names))
            self.assertFalse(any(n.startswith(("re/", "tools/", "experiments/", "minesim/studies/")) for n in names))
            self.assertFalse((source / ".git").exists())
            self.assertFalse((source / "third_party/openbw").exists())
            archive = Path(str(source) + ".zip")
            self.assertTrue(archive.exists())
            self.assertEqual(archive.name, "scmdraft-fan-edit-0.1.0-draft-source.zip")
            before = hashlib.sha256(archive.read_bytes()).hexdigest()
            with self.assertRaises(ValueError):
                prepare.prepare(Path(tmp))
            self.assertEqual(before, hashlib.sha256(archive.read_bytes()).hexdigest())
            with zipfile.ZipFile(archive) as z:
                self.assertIn(source.name + "/README.md", z.namelist())
            for line in (source / "SHA256SUMS.txt").read_text().splitlines():
                digest, name = line.split("  ", 1)
                self.assertEqual(hashlib.sha256((source / name).read_bytes()).hexdigest(), digest)

    def test_audit_rejects_game_files_private_paths_and_credentials(self):
        for name, data in (("map.scx", b"map"), ("art.png", b"image"), ("decomp.c", b"// " + b"/" + b"Users/private/project"),
                           ("token.txt", b"ghp_" + b"x" * 36)):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                (root / name).write_bytes(data)
                with self.assertRaises(ValueError):
                    prepare.audit_source(root)

    def test_audit_rejects_decompilation_directory(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "re/out").mkdir(parents=True)
            (root / "re/out/export.c").write_text("example")
            with self.assertRaises(ValueError):
                prepare.audit_source(root)

    def test_documentation_media_requires_png(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            p = root / "docs/media/hd-graphics.png"
            p.parent.mkdir(parents=True)
            p.write_bytes(b"not a PNG")
            with self.assertRaises(ValueError):
                prepare.audit_source(root)

    def test_incomplete_binary_payload_does_not_create_export(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build = root / "build"
            build.mkdir()
            out = root / "out"
            with self.assertRaises(FileNotFoundError):
                prepare.prepare(out, build)
            self.assertFalse(out.exists())
            (build / "scmhd.sdp").write_text("not a PE file")
            with self.assertRaises(ValueError):
                prepare.prepare(out, build)
            self.assertFalse(out.exists())

if __name__ == "__main__":
    unittest.main()
