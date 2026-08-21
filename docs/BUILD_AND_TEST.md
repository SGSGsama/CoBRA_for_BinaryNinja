# Build and test contract

All commands below use `uvx` to obtain CMake. Build directories, dependency
prefixes, objects, libraries, and plugin modules are generated outputs and
must stay outside this checkout. No separate Python virtual environment is
required or used.

## Bootstrap pinned dependencies

Initialize the Gitlinks before configuring:

```bash
git submodule update --init --recursive
git submodule status
```

From the repository root, create a reusable Z3-enabled dependency prefix in a
temporary directory. The chosen build directory is intentionally outside the
repository and can be removed when no longer needed.

```bash
SMBA_REPO="$PWD"
SMBA_DEPS_BUILD="$(mktemp -d /tmp/smba-cobra-mba-deps-z3.XXXXXX)"
uvx --from cmake cmake -S "$SMBA_REPO/third_party/CoBRA/dependencies" \
  -B "$SMBA_DEPS_BUILD" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCOBRA_ENABLE_Z3=ON \
  -DCOBRA_BUILD_TESTS=OFF
uvx --from cmake cmake --build "$SMBA_DEPS_BUILD" --parallel
SMBA_DEPS_PREFIX="$SMBA_DEPS_BUILD/install"
```

The prefix must contain CMake package configs for absl and highway. It also
contains Z3 when enabled. The top-level configure checks this boundary before
it adds CoBRA, so an incomplete prefix fails with an actionable error.

## Required headless test matrix

Run both branches from the repository root. Each uses a new out-of-tree build
directory; neither needs a Binary Ninja installation.

```bash
SMBA_Z3_BUILD="$(mktemp -d /tmp/smba-cobra-mba-core-z3.XXXXXX)"
uvx --from cmake cmake -S "$SMBA_REPO" -B "$SMBA_Z3_BUILD" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DSMBA_BUILD_PLUGIN=OFF \
  -DSMBA_BUILD_TESTS=ON \
  -DSMBA_REQUIRE_Z3=ON \
  -DSMBA_DEPS_PREFIX="$SMBA_DEPS_PREFIX"
uvx --from cmake cmake --build "$SMBA_Z3_BUILD" --parallel
uvx --from cmake ctest --test-dir "$SMBA_Z3_BUILD" --output-on-failure

SMBA_NO_Z3_BUILD="$(mktemp -d /tmp/smba-cobra-mba-core-no-z3.XXXXXX)"
uvx --from cmake cmake -S "$SMBA_REPO" -B "$SMBA_NO_Z3_BUILD" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DSMBA_BUILD_PLUGIN=OFF \
  -DSMBA_BUILD_TESTS=ON \
  -DSMBA_REQUIRE_Z3=OFF \
  -DCMAKE_DISABLE_FIND_PACKAGE_Z3=ON \
  -DSMBA_DEPS_PREFIX="$SMBA_DEPS_PREFIX"
uvx --from cmake cmake --build "$SMBA_NO_Z3_BUILD" --parallel
uvx --from cmake ctest --test-dir "$SMBA_NO_Z3_BUILD" --output-on-failure
```

The Z3 branch must prove constant predicates and set `z3Verified`. In the
forced-no-Z3 branch all predicate proofs must reject, regardless of the
runtime `requireZ3` option; only a separate ordinary arithmetic diagnostic may
be `Probabilistic`. This verifies that proof failure cannot become rewrite
authority.

`scripts/validate.sh --deps-prefix "$SMBA_DEPS_PREFIX"` runs this exact core
matrix in fresh temporary directories. It intentionally has no install action.

## Binary Ninja plugin build

Use the pinned Binary Ninja API submodule and a Binary Ninja installation
matching it. On macOS, an application at `/Applications/Binary Ninja.app` is
passed explicitly as follows:

```bash
SMBA_PLUGIN_BUILD="$(mktemp -d /tmp/smba-cobra-mba-plugin.XXXXXX)"
uvx --from cmake cmake -S "$SMBA_REPO" -B "$SMBA_PLUGIN_BUILD" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DSMBA_BUILD_PLUGIN=ON \
  -DSMBA_BUILD_TESTS=OFF \
  -DSMBA_REQUIRE_Z3=ON \
  -DSMBA_DEPS_PREFIX="$SMBA_DEPS_PREFIX" \
  -DBN_INSTALL_DIR="/Applications/Binary Ninja.app"
uvx --from cmake cmake --build "$SMBA_PLUGIN_BUILD" --parallel
```

The plugin build is Z3-only. `SMBA_BN_ALLOW_STUBS=ON` is compile-only and does
not yield a loadable plugin. Installation is a separate user decision; this
repository does not invoke it. If installation is intended, inspect the
generated install destination first, then run the explicit target yourself:

```bash
uvx --from cmake cmake --build "$SMBA_PLUGIN_BUILD" --target install
```

## Static checks

The headless core compile database must not mention Binary Ninja:

```bash
if rg -n -i 'binaryninja|binaryninjacore|binaryninjaapi' \
  "$SMBA_Z3_BUILD/compile_commands.json"; then
  echo "headless core unexpectedly depends on Binary Ninja" >&2
  exit 1
fi
```

Verify first-party source and documentation do not carry sample-specific
terms before release:

```bash
SMBA_FORBIDDEN_PATTERN='mt''guard|0x51''e970|0x444''42c|project'' evidence|reverse_for_fun''_mt'
if rg -n -i "$SMBA_FORBIDDEN_PATTERN" \
  --glob '!third_party/**' .; then
  echo "sample-specific text found in distributable files" >&2
  exit 1
fi
```
