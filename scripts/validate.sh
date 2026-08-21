#!/usr/bin/env bash
# Run the required headless validation matrix in fresh out-of-tree directories.
# This script deliberately never configures the Binary Ninja plugin or runs an
# install target; see docs/BUILD_AND_TEST.md for those separate user actions.
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: scripts/validate.sh --deps-prefix PATH

PATH is the install directory created by CoBRA's dependency superbuild with
COBRA_ENABLE_Z3=ON. CMake is always invoked with uvx and build output is kept
in fresh temporary directories.
EOF
}

deps_prefix=""
while (($#)); do
  case "$1" in
    --deps-prefix)
      (($# >= 2)) || { usage >&2; exit 2; }
      deps_prefix="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

[[ -n "$deps_prefix" ]] || { usage >&2; exit 2; }
[[ -d "$deps_prefix" ]] || { echo "missing dependency prefix: $deps_prefix" >&2; exit 2; }

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
z3_build="$(mktemp -d "${TMPDIR:-/tmp}/smba-cobra-mba-core-z3.XXXXXX")"
no_z3_build="$(mktemp -d "${TMPDIR:-/tmp}/smba-cobra-mba-core-no-z3.XXXXXX")"

cleanup() {
  rm -rf "$z3_build" "$no_z3_build"
}
trap cleanup EXIT

uvx --from cmake cmake -S "$repo_root" -B "$z3_build" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DSMBA_BUILD_PLUGIN=OFF \
  -DSMBA_BUILD_TESTS=ON \
  -DSMBA_REQUIRE_Z3=ON \
  -DSMBA_DEPS_PREFIX="$deps_prefix"
uvx --from cmake cmake --build "$z3_build" --parallel
uvx --from cmake ctest --test-dir "$z3_build" --output-on-failure

uvx --from cmake cmake -S "$repo_root" -B "$no_z3_build" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DSMBA_BUILD_PLUGIN=OFF \
  -DSMBA_BUILD_TESTS=ON \
  -DSMBA_REQUIRE_Z3=OFF \
  -DCMAKE_DISABLE_FIND_PACKAGE_Z3=ON \
  -DSMBA_DEPS_PREFIX="$deps_prefix"
uvx --from cmake cmake --build "$no_z3_build" --parallel
uvx --from cmake ctest --test-dir "$no_z3_build" --output-on-failure

if rg -n -i 'binaryninja|binaryninjacore|binaryninjaapi' \
  "$z3_build/compile_commands.json"; then
  echo "headless core unexpectedly depends on Binary Ninja" >&2
  exit 1
fi

forbidden_pattern='mt''guard|0x51''e970|0x444''42c|project'' evidence|reverse_for_fun''_mt'
if rg -n -i "$forbidden_pattern" \
  --glob '!third_party/**' "$repo_root"; then
  echo "sample-specific text found in first-party files" >&2
  exit 1
fi

echo "SMBA standalone headless validation passed"
