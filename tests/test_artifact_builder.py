from __future__ import annotations

import importlib.util
import json
import pathlib
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILDER_PATH = ROOT / "scripts" / "build_artifact.py"


def load_module():
    name = "smba_artifact_builder_test_module"
    sys.modules.pop(name, None)
    spec = importlib.util.spec_from_file_location(name, BUILDER_PATH)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


class ArtifactBuilderTests(unittest.TestCase):
    def test_builds_closed_manifest_with_deterministic_relative_paths_and_hashes(self):
        builder = load_module()
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir) / "source"
            (root / "scripts").mkdir(parents=True)
            (root / "scripts" / "ai_cli.py").write_text("print('cli')\n", encoding="utf-8")
            (root / "README.md").write_text("README\n", encoding="utf-8")
            dylib = root / "build-plugin" / "smba-cobra-mba.dylib"
            dylib.parent.mkdir()
            dylib.write_bytes(b"dylib-bytes\x00")
            output = pathlib.Path(temp_dir) / "artifact" / "plugin" / "smba_cobra_mba"

            builder.build_artifact(root, dylib, output)

            manifest_path = output / "manifest.json"
            self.assertTrue(manifest_path.is_file())
            first_manifest_text = manifest_path.read_text(encoding="utf-8")
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            self.assertEqual(manifest["plugin"], "smba-cobra-mba")
            self.assertEqual(
                [entry["path"] for entry in manifest["files"]],
                ["README.md", "ai_cli.py", "smba-cobra-mba.dylib"],
            )
            self.assertEqual(
                sorted(path.name for path in output.iterdir()),
                ["README.md", "ai_cli.py", "manifest.json", "smba-cobra-mba.dylib"],
            )
            self.assertEqual(
                (output / "ai_cli.py").read_bytes(),
                (root / "scripts" / "ai_cli.py").read_bytes(),
            )
            self.assertEqual(builder.verify_artifact(output), manifest)

            builder.build_artifact(root, dylib, output)
            self.assertEqual(manifest_path.read_text(encoding="utf-8"), first_manifest_text)

            (output / "unexpected.txt").write_text("not in manifest", encoding="utf-8")
            with self.assertRaises(builder.ArtifactVerificationError):
                builder.verify_artifact(output)
            (output / "unexpected.txt").unlink()
            (output / "unexpected-directory").mkdir()
            with self.assertRaises(builder.ArtifactVerificationError):
                builder.verify_artifact(output)


if __name__ == "__main__":
    unittest.main(verbosity=2)
