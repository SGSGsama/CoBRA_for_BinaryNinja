#!/usr/bin/env python3
"""Build and verify the deterministic SMBA CoBRA plugin artifact directory."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import shutil
import sys
from typing import Any, Iterable, Mapping


ARTIFACT_SCHEMA_VERSION = "smba-cobra-mba-artifact-v1"
PLUGIN_NAME = "smba-cobra-mba"
ARTIFACT_FILES = ("README.md", "ai_cli.py", "smba-cobra-mba.dylib")


class ArtifactVerificationError(RuntimeError):
    pass


def _sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _payload_entry(path: pathlib.Path, relative_path: str) -> dict[str, Any]:
    return {
        "path": relative_path,
        "sha256": _sha256(path),
        "size": path.stat().st_size,
    }


def _clear_output(output_dir: pathlib.Path) -> None:
    """Remove only the exact, user-selected artifact directory's contents."""

    if not output_dir.exists():
        output_dir.mkdir(parents=True, exist_ok=True)
        return
    if not output_dir.is_dir():
        raise ArtifactVerificationError(f"artifact output is not a directory: {output_dir}")
    for child in output_dir.iterdir():
        if child.is_symlink() or child.is_file():
            child.unlink()
        elif child.is_dir():
            shutil.rmtree(child)
        else:
            raise ArtifactVerificationError(f"cannot safely replace artifact entry: {child}")


def _copy_deterministically(source: pathlib.Path, destination: pathlib.Path, mode: int) -> None:
    if not source.is_file() or source.is_symlink():
        raise ArtifactVerificationError(f"expected a regular input file: {source}")
    shutil.copyfile(source, destination)
    os.chmod(destination, mode)
    # A fixed timestamp plus fixed modes make directory snapshots reproducible
    # for identical inputs. Hashes remain the primary integrity assertion.
    os.utime(destination, (0, 0))


def _manifest_from_payload(output_dir: pathlib.Path) -> dict[str, Any]:
    return {
        "schema_version": ARTIFACT_SCHEMA_VERSION,
        "plugin": PLUGIN_NAME,
        "files": [
            _payload_entry(output_dir / relative_path, relative_path)
            for relative_path in ARTIFACT_FILES
        ],
    }


def _require_exact_payload_copy(
    inputs: Mapping[str, pathlib.Path], output_dir: pathlib.Path
) -> None:
    """Prove the artifact contains byte-identical source payloads before release."""

    for relative_path in ARTIFACT_FILES:
        source = inputs[relative_path]
        copied = output_dir / relative_path
        if copied.read_bytes() != source.read_bytes():
            raise ArtifactVerificationError(
                f"artifact payload differs from its source input: {relative_path}"
            )


def _all_relative_files(directory: pathlib.Path) -> list[str]:
    return sorted(
        path.relative_to(directory).as_posix()
        for path in directory.rglob("*")
        if path.is_file() or path.is_symlink()
    )


def verify_artifact(output_dir: pathlib.Path) -> dict[str, Any]:
    """Fail closed on altered hashes, unsafe links, or unexpected files."""

    output_dir = pathlib.Path(output_dir)
    manifest_path = output_dir / "manifest.json"
    if not manifest_path.is_file() or manifest_path.is_symlink():
        raise ArtifactVerificationError("artifact is missing a regular manifest.json")
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ArtifactVerificationError(f"artifact manifest is unreadable: {error}") from error
    if not isinstance(manifest, dict):
        raise ArtifactVerificationError("artifact manifest must be a JSON object")
    if manifest.get("schema_version") != ARTIFACT_SCHEMA_VERSION:
        raise ArtifactVerificationError("artifact manifest schema_version is unsupported")
    if manifest.get("plugin") != PLUGIN_NAME:
        raise ArtifactVerificationError("artifact manifest plugin does not match smba-cobra-mba")
    files = manifest.get("files")
    if not isinstance(files, list):
        raise ArtifactVerificationError("artifact manifest files must be an array")
    paths = [entry.get("path") if isinstance(entry, dict) else None for entry in files]
    if paths != list(ARTIFACT_FILES):
        raise ArtifactVerificationError("artifact manifest payload paths are not the deterministic closure")
    unexpected_directories = [
        path.relative_to(output_dir).as_posix()
        for path in output_dir.rglob("*")
        if path.is_dir() and not path.is_symlink()
    ]
    if unexpected_directories:
        raise ArtifactVerificationError("artifact contains directories outside the flat deterministic closure")
    expected_closure = sorted(["manifest.json", *ARTIFACT_FILES])
    if _all_relative_files(output_dir) != expected_closure:
        raise ArtifactVerificationError("artifact contains files outside the manifest closure")
    for entry in files:
        relative_path = entry["path"]
        path = output_dir / relative_path
        if not path.is_file() or path.is_symlink():
            raise ArtifactVerificationError(f"artifact payload is missing or unsafe: {relative_path}")
        expected_hash = entry.get("sha256")
        expected_size = entry.get("size")
        if not isinstance(expected_hash, str) or not isinstance(expected_size, int):
            raise ArtifactVerificationError(f"artifact manifest entry is invalid: {relative_path}")
        if path.stat().st_size != expected_size:
            raise ArtifactVerificationError(f"artifact payload size mismatch: {relative_path}")
        if _sha256(path) != expected_hash:
            raise ArtifactVerificationError(f"artifact payload hash mismatch: {relative_path}")
    return manifest


def build_artifact(source_root: pathlib.Path, dylib_path: pathlib.Path, output_dir: pathlib.Path) -> pathlib.Path:
    """Copy exactly the loadable dylib, CLI, and README, then self-verify."""

    source_root = pathlib.Path(source_root).resolve()
    dylib_path = pathlib.Path(dylib_path).resolve()
    output_dir = pathlib.Path(output_dir).resolve()
    inputs = {
        "README.md": source_root / "README.md",
        "ai_cli.py": source_root / "scripts" / "ai_cli.py",
        "smba-cobra-mba.dylib": dylib_path,
    }
    for relative_path, source in inputs.items():
        if not source.is_file() or source.is_symlink():
            raise ArtifactVerificationError(f"missing regular artifact input {relative_path}: {source}")

    _clear_output(output_dir)
    _copy_deterministically(inputs["README.md"], output_dir / "README.md", 0o644)
    _copy_deterministically(inputs["ai_cli.py"], output_dir / "ai_cli.py", 0o755)
    _copy_deterministically(inputs["smba-cobra-mba.dylib"], output_dir / "smba-cobra-mba.dylib", 0o755)
    _require_exact_payload_copy(inputs, output_dir)
    manifest = _manifest_from_payload(output_dir)
    manifest_path = output_dir / "manifest.json"
    manifest_path.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    os.chmod(manifest_path, 0o644)
    os.utime(manifest_path, (0, 0))
    verify_artifact(output_dir)
    return output_dir


def _default_paths() -> tuple[pathlib.Path, pathlib.Path, pathlib.Path]:
    source_root = pathlib.Path(__file__).resolve().parents[1]
    return (
        source_root,
        source_root / "build-plugin" / "smba-cobra-mba.dylib",
        source_root / "artifact" / "plugin" / "smba_cobra_mba",
    )


def main(argv: Iterable[str] | None = None) -> int:
    default_source, default_dylib, default_output = _default_paths()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", default=default_source, type=pathlib.Path)
    parser.add_argument("--dylib", default=default_dylib, type=pathlib.Path)
    parser.add_argument("--output", default=default_output, type=pathlib.Path)
    parser.add_argument("--verify", action="store_true", help="verify an existing artifact without copying files")
    arguments = parser.parse_args(argv)
    try:
        if arguments.verify:
            manifest = verify_artifact(arguments.output)
        else:
            output = build_artifact(arguments.source_root, arguments.dylib, arguments.output)
            manifest = verify_artifact(output)
    except ArtifactVerificationError as error:
        print(json.dumps({"ok": False, "error": str(error)}, sort_keys=True), file=sys.stderr)
        return 2
    print(json.dumps({"ok": True, "manifest": manifest}, sort_keys=True, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
