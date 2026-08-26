# SMBA build/test contract

This document defines the reproducible, `uv`-managed validation commands for the adapter. Build directories, CMake caches, objects, libraries, and plugin bundles are generated outputs and must remain out of the source tree.

## Target graph

```text
smba-core-tests ──> smba-core ──> cobra-core ──> absl, hwy
                                  └────────────> cobra-verify ──> Z3

smba-cobra-mba ──> smba-core
                 └─> binaryninjaapi ──> binaryninjacore (Binary Ninja install)
```

`smba-core-tests` has no Binary Ninja header, library, or runtime dependency. The adapter is built only with `SMBA_BUILD_PLUGIN=ON`; its transformation path is always Z3-gated.

## Dependency prefix

From the repository root, build/rebuild the validation prefix with `uvx`:

```bash
cd analysis/SMBA_deobf/CoBRA
uvx --from cmake cmake -S dependencies -B build-deps-smba \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCOBRA_ENABLE_Z3=ON -DCOBRA_BUILD_TESTS=OFF
uvx --from cmake cmake --build build-deps-smba --parallel
```

The expected prefix is `analysis/SMBA_deobf/CoBRA/build-deps-smba/install`. It supplies `absl`, `hwy`, and CoBRA's Z3 verifier. Do not replace these commands with system CMake/Python or an unmanaged virtual environment.

## Core test matrix

Run both branches from `analysis/SMBA_deobf/bn-cobra-mba`:

```bash
SMBA_DEPS="$PWD/../CoBRA/build-deps-smba/install"
SMBA_BUILD="/tmp/smba-cobra-mba-core-z3"
uvx --from cmake cmake -S . -B "$SMBA_BUILD" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DSMBA_BUILD_PLUGIN=OFF -DSMBA_BUILD_TESTS=ON \
  -DSMBA_REQUIRE_Z3=ON -DSMBA_DEPS_PREFIX="$SMBA_DEPS"
uvx --from cmake cmake --build "$SMBA_BUILD" --parallel
uvx --from cmake ctest --test-dir "$SMBA_BUILD" --output-on-failure

SMBA_NO_Z3_BUILD="/tmp/smba-cobra-mba-core-no-z3"
uvx --from cmake cmake -S . -B "$SMBA_NO_Z3_BUILD" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DSMBA_BUILD_PLUGIN=OFF -DSMBA_BUILD_TESTS=ON \
  -DSMBA_REQUIRE_Z3=OFF -DCMAKE_DISABLE_FIND_PACKAGE_Z3=ON \
  -DSMBA_DEPS_PREFIX="$SMBA_DEPS"
uvx --from cmake cmake --build "$SMBA_NO_Z3_BUILD" --parallel
uvx --from cmake ctest --test-dir "$SMBA_NO_Z3_BUILD" --output-on-failure
```

The core tests cover arithmetic simplification, unchanged constants, invalid width/variable limits, and generalized predicate proof: true and false constant comparisons, non-constant rejection, signed/unsigned distinctions, wide logical shifts, and invalid predicate inputs. The Z3 build must record `ProvedConstant` and `z3Verified` for proved predicates. In the no-Z3 build, all predicate proofs reject; the only `Probabilistic` result is the separate ordinary arithmetic diagnostic path, never a predicate or workflow rewrite.

`SMBA_REQUIRE_Z3` is a configure-time fail-closed gate, not a runtime proof-policy setting. The adapter sets `SimplifyOptions.requireZ3=true` internally, and `AnalysisLimits` contains resource budgets only.

## Plugin build

The plugin requires CoBRA's verifier even if the core-only no-Z3 configuration is useful for diagnostics:

```bash
SMBA_PLUGIN_BUILD="/tmp/smba-cobra-mba-plugin"
uvx --from cmake cmake -S . -B "$SMBA_PLUGIN_BUILD" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DSMBA_BUILD_PLUGIN=ON -DSMBA_BUILD_TESTS=OFF \
  -DSMBA_REQUIRE_Z3=ON -DSMBA_DEPS_PREFIX="$SMBA_DEPS" \
  -DSMBA_BINARYNINJA_API_DIR=../binaryninja-api \
  -DBN_INSTALL_DIR="/Applications/Binary Ninja.app"
uvx --from cmake cmake --build "$SMBA_PLUGIN_BUILD" --parallel
```

Run `uvx --from cmake cmake --build "$SMBA_PLUGIN_BUILD" --target install` only when installation into the user's plugin directory is intentional. `SMBA_BN_ALLOW_STUBS=ON` is compile-only and never produces a loadable plugin.

## Static documentation/API checks

The core compile commands must not mention Binary Ninja:

```bash
rg -n -i 'binaryninja|binaryninjacore|binaryninjaapi' \
  "$SMBA_BUILD/compile_commands.json"
```

From the adapter root, check the current public terms and reject obsolete derivative/count claims:

```bash
if rg -n '\.smba-cobra|Register derivative of current workflow|five accepted|0x44442c' \
  README.md docs --glob '!BUILD_AND_TEST.md' --glob '!VALIDATION.md'; then
  echo "stale workflow or candidate-count documentation" >&2
  exit 1
fi
rg -n 'Register or refresh current \.mba workflow|W\.mba|ProveConstantComparison|preview_generic_predicates\.log|workflow_register_refresh\.log' README.md docs

# The current-function lifecycle is activity-first. A suffix may construct a
# derivative name only after the activity-absence branch, never route refresh.
if rg -n 'ends_with|\.ends_with' src/Plugin.cpp; then
  echo "Plugin.cpp must not use a suffix as a workflow lifecycle condition" >&2
  exit 1
fi
rg -n 'current->Contains\(smba::kActivityName\)|AddActivityToWorkflow\(current\)' src/Plugin.cpp
```

The first search is intentionally negative. Historical arithmetic-site data belongs only in the explicitly marked legacy baseline evidence, never as the current Preview total.

## Helper inventory rule

No persisted helper is required for this adapter. Any future helper—also an ephemeral BN Python here-doc—must be recorded in `docs/VALIDATION.md` with purpose, inputs, outputs, hook/analysis point, validation target, invocation, and injection risk. BN-host helpers are not Android-process injection; state that distinction explicitly.
