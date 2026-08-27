# SMBA build and test contract

This document defines the reproducible, 'uv'-managed validation commands for
the adapter. Build directories, CMake caches, objects, libraries, and plugin
bundles are generated output; keep them out of the source tree.

## Target graph

~~~
smba-core-tests ──> smba-core ──> cobra-core ──> absl, hwy
                                  └────────────> cobra-verify ──> Z3

smba-cobra-mba ──> smba-core
                 └─> binaryninjaapi ──> binaryninjacore

smba-plugin-json-tests ──> src/PluginJson.cpp
~~~

'smba-core-tests' has no Binary Ninja header, library, or runtime dependency.
The adapter is built only with 'SMBA_BUILD_PLUGIN=ON'; its transformation path
is always Z3-gated.

## Dependency prefix and core matrix

From the repository root, build the validation prefix:

~~~bash
cd analysis/SMBA_deobf/CoBRA
uvx --from cmake cmake -S dependencies -B build-deps-smba +  -DCMAKE_BUILD_TYPE=RelWithDebInfo +  -DCOBRA_ENABLE_Z3=ON -DCOBRA_BUILD_TESTS=OFF
uvx --from cmake cmake --build build-deps-smba --parallel
~~~

From 'analysis/SMBA_deobf/bn-cobra-mba', validate both core branches:

~~~bash
SMBA_DEPS=$PWD/../CoBRA/build-deps-smba/install
SMBA_BUILD=/tmp/smba-cobra-mba-core-z3
uvx --from cmake cmake -S . -B $SMBA_BUILD +  -DCMAKE_BUILD_TYPE=RelWithDebInfo +  -DSMBA_BUILD_PLUGIN=OFF -DSMBA_BUILD_TESTS=ON +  -DSMBA_REQUIRE_Z3=ON -DSMBA_DEPS_PREFIX=$SMBA_DEPS
uvx --from cmake cmake --build $SMBA_BUILD --parallel
uvx --from cmake ctest --test-dir $SMBA_BUILD --output-on-failure

SMBA_NO_Z3_BUILD=/tmp/smba-cobra-mba-core-no-z3
uvx --from cmake cmake -S . -B $SMBA_NO_Z3_BUILD +  -DCMAKE_BUILD_TYPE=RelWithDebInfo +  -DSMBA_BUILD_PLUGIN=OFF -DSMBA_BUILD_TESTS=ON +  -DSMBA_REQUIRE_Z3=OFF -DCMAKE_DISABLE_FIND_PACKAGE_Z3=ON +  -DSMBA_DEPS_PREFIX=$SMBA_DEPS
uvx --from cmake cmake --build $SMBA_NO_Z3_BUILD --parallel
uvx --from cmake ctest --test-dir $SMBA_NO_Z3_BUILD --output-on-failure
~~~

The no-Z3 matrix may report 'Probabilistic' only for the separate arithmetic
diagnostic path; every predicate proof rejects. The plugin build always needs
CoBRA's verifier:

~~~bash
SMBA_PLUGIN_BUILD=/tmp/smba-cobra-mba-plugin
uvx --from cmake cmake -S . -B $SMBA_PLUGIN_BUILD +  -DCMAKE_BUILD_TYPE=RelWithDebInfo +  -DSMBA_BUILD_PLUGIN=ON -DSMBA_BUILD_TESTS=ON +  -DSMBA_REQUIRE_Z3=ON -DSMBA_DEPS_PREFIX=$SMBA_DEPS +  -DSMBA_BINARYNINJA_API_DIR=../binaryninja-api +  -DBN_INSTALL_DIR=/Applications/Binary\ Ninja.app
uvx --from cmake cmake --build $SMBA_PLUGIN_BUILD --parallel
uvx --from cmake ctest --test-dir $SMBA_PLUGIN_BUILD --output-on-failure
~~~

Do not use installation targets unless installation into the user plugin
directory is intentional. 'SMBA_BN_ALLOW_STUBS=ON' is compile-only and never
creates a loadable plugin.

## Dedicated CLI and artifact checks

The dedicated CLI and artifact builder use only the standard library but must
run through the repository's 'uv'-managed Python 3.11+ interpreter. The CLI
rejects the system Python 3.9 before importing argparse or Binary Ninja:

~~~bash
uv run python -m unittest discover -s tests -p 'test_*.py' -q
uv run python scripts/build_artifact.py
uv run python scripts/build_artifact.py --verify
/usr/bin/python3 scripts/ai_cli.py -h  # expected: one Python 3.11+ error line
~~~

The CLI tests use fake Binary Ninja objects. They cover the two exact menu
mappings, direct 'PluginCommand.is_valid' before execution, tagged-log parsing,
strict function-start correlation/cross-talk rejection, callback failures,
directory-independent help, ordinary positional arguments, obsolete input
argument rejection, absolute-script-path external transport, and the clean
system-Python-3.9 version failure. No test
invokes a live plugin, mutates a Binary Ninja view, saves a database, or
interacts with the target process.

Artifact tests use temporary regular files and prove deterministic manifest
ordering, source/artifact byte identity, hash verification, and closure
rejection. The builder only replaces entries inside its explicit output
directory.

See [AI_CLI_AND_ARTIFACT.md](AI_CLI_AND_ARTIFACT.md) for the two-command
contract and artifact layout.

## Static documentation and API checks

The core compile commands must not mention Binary Ninja:

~~~bash
rg -n -i 'binaryninja|binaryninjacore|binaryninjaapi' +  $SMBA_BUILD/compile_commands.json
~~~

From the adapter root, reject stale workflow/candidate descriptions and
confirm the activity-first lifecycle:

~~~bash
if rg -n '\.smba-cobra|Register derivative of current workflow|five accepted|0x44442c' +  README.md docs --glob '!VALIDATION.md'; then
  echo stale workflow or candidate-count documentation >&2
  exit 1
fi
rg -n 'Register or refresh current \.mba workflow|W\.mba|ProveConstantComparison' README.md docs
if rg -n 'ends_with|\.ends_with' src/Plugin.cpp; then
  echo Plugin.cpp must not use a suffix as a workflow lifecycle condition >&2
  exit 1
fi
rg -n 'current->Contains\(smba::kActivityName\)|AddActivityToWorkflow\(current\)' src/Plugin.cpp
~~~

## Helper inventory rule

No persisted helper is required for this adapter. Any future helper, including
an ephemeral Binary Ninja Python here-doc, must be recorded in
'docs/VALIDATION.md' with purpose, inputs, outputs, hook/analysis point,
validation target, invocation, and injection risk. BN-host helpers are not
Android-process injection; state that distinction explicitly.
