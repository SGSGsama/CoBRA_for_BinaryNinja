# Validation record

This file records the standalone repository validation commands and exact
dependency revisions. It intentionally contains no sample inputs, binary
database paths, candidate addresses, or runtime evidence claims.

## Required records

Every release validation must record:

1. The repository commit under test and `git status --short` result.
2. Exact `git submodule status` lines for CoBRA and Binary Ninja API.
3. The complete `uvx` bootstrap, Z3 core, forced-no-Z3 core, and optional
   plugin build commands.
4. Whether `install` was intentionally run. Normal validation must record
   that it was not run.
5. The static first-party-term scan and the headless compile-database check.

## Initial standalone validation — 2026-08-21 (Asia/Shanghai)

The tested source baseline is the initial standalone commit
`f7ae98c978d964ed95d9d223403ff589fef013b3`. The first commit had a clean
`git status --short`; this record is committed separately so the exact tested
baseline hash can be recorded without a self-referential commit hash.

Pinned top-level submodules at validation time:

```text
 af44b8ad7c8ec548d431463475e93b0118e77a29 third_party/CoBRA
 aa25bfcfd36532ec3850558a58444df7727e297b third_party/binaryninja-api
```

The required Binary Ninja API nested `vendor/fmt` submodule was initialized at
`40626af88bd7df9a5fb80be7b25ac85b122d6c21`. The normal recursive bootstrap
command in [`BUILD_AND_TEST.md`](BUILD_AND_TEST.md) performs that initialization
from a clean clone.

Environment: macOS arm64, AppleClang 21.0.0.21000101, CMake 4.4.2 via
`uvx --from cmake`, and Binary Ninja at `/Applications/Binary Ninja.app`.

### Commands and outcomes

The dependency prefix was rebuilt from the pinned CoBRA source, outside the
repository:

```bash
REPO_ROOT=$(pwd)
uvx --from cmake cmake \
  -S "$REPO_ROOT/third_party/CoBRA/dependencies" \
  -B /tmp/smba-cobra-mba-deps-z3 \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCOBRA_ENABLE_Z3=ON \
  -DCOBRA_BUILD_TESTS=OFF
uvx --from cmake cmake --build /tmp/smba-cobra-mba-deps-z3 --parallel
```

Strict Z3 core configuration, build, and test all succeeded:

```bash
REPO_ROOT=$(pwd)
uvx --from cmake cmake -S "$REPO_ROOT" \
  -B /tmp/smba-cobra-mba-core-z3 \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DSMBA_BUILD_PLUGIN=OFF -DSMBA_BUILD_TESTS=ON -DSMBA_REQUIRE_Z3=ON \
  -DSMBA_DEPS_PREFIX=/tmp/smba-cobra-mba-deps-z3/install
uvx --from cmake cmake --build /tmp/smba-cobra-mba-core-z3 --parallel
uvx --from cmake ctest --test-dir /tmp/smba-cobra-mba-core-z3 --output-on-failure
```

`smba-core-tests` passed (1/1) and printed `SMBA core tests passed (Z3 proof
path)`. Its compile database contained no Binary Ninja terms.

The forced-no-Z3 configuration, build, and test also succeeded:

```bash
REPO_ROOT=$(pwd)
uvx --from cmake cmake -S "$REPO_ROOT" \
  -B /tmp/smba-cobra-mba-core-no-z3 \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DSMBA_BUILD_PLUGIN=OFF -DSMBA_BUILD_TESTS=ON -DSMBA_REQUIRE_Z3=OFF \
  -DCMAKE_DISABLE_FIND_PACKAGE_Z3=ON \
  -DSMBA_DEPS_PREFIX=/tmp/smba-cobra-mba-deps-z3/install
uvx --from cmake cmake --build /tmp/smba-cobra-mba-core-no-z3 --parallel
uvx --from cmake ctest --test-dir /tmp/smba-cobra-mba-core-no-z3 --output-on-failure
```

`smba-core-tests` passed (1/1) and printed the no-Z3 diagnostic-path result;
its comparison-proof tests confirmed rejection in both relaxed and strict
runtime modes. The reusable helper was exercised as well:

```bash
scripts/validate.sh --deps-prefix /tmp/smba-cobra-mba-deps-z3/install
```

It performed both branches in fresh `mktemp` directories and passed its
headless compile-database and first-party-term checks.

The Binary Ninja plugin configured and built successfully against the installed
application (the temporary directory was created with `mktemp -d`):

```bash
REPO_ROOT=$(pwd)
uvx --from cmake cmake -S "$REPO_ROOT" \
  -B /tmp/smba-cobra-mba-plugin-standalone.NxOpKS \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DSMBA_BUILD_PLUGIN=ON -DSMBA_BUILD_TESTS=OFF -DSMBA_REQUIRE_Z3=ON \
  -DSMBA_DEPS_PREFIX=/tmp/smba-cobra-mba-deps-z3/install \
  -DBN_INSTALL_DIR=/Applications/Binary\ Ninja.app
uvx --from cmake cmake --build /tmp/smba-cobra-mba-plugin-standalone.NxOpKS --parallel
```

The build produced `smba-cobra-mba.dylib`. No `install` target was invoked, no
installed plugin was changed, and generated dependencies/build outputs remain
outside the repository.

No C++ source repair was required for the pinned official dependencies. The
only standalone packaging adjustments were replacing parent-layout defaults
with `third_party/` paths and removing sample-specific documentation; the
upstream Binary Ninja API additionally requires its existing `vendor/fmt`
nested submodule to be initialized before configuring.
