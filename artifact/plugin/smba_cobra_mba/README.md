# SMBA CoBRA MBA adapter

'smba-core' recovers selected pure MLIL SSA expression trees, simplifies
arithmetic/bitwise MBA expressions with CoBRA, and proves eligible constant
comparisons with Z3. 'smba-cobra-mba' is the Binary Ninja adapter: it exposes
a read-only Preview command and a deliberately fail-closed '.mba' workflow
lifecycle. No address, function-name, or constant-value pattern list selects
candidates; eligibility is structural SSA recovery plus proof.

The source layout is:

~~~
analysis/SMBA_deobf/
├── CoBRA/                 # CoBRA source, consumed with add_subdirectory
├── binaryninja-api/       # Binary Ninja C++ API source
└── bn-cobra-mba/          # this adapter
~~~

Generated build directories, CMake caches, objects, libraries, and plugin
bundles are not source artifacts. Use out-of-tree builds (for example under
'/tmp') and the 'uvx' commands below; do not create a separate Python
environment.

## Build and test

The validated CoBRA dependency prefix is
'analysis/SMBA_deobf/CoBRA/build-deps-smba/install'. From this directory,
strict core validation is:

~~~bash
SMBA_DEPS=$PWD/../CoBRA/build-deps-smba/install
SMBA_BUILD=/tmp/smba-cobra-mba-core
uvx --from cmake cmake -S . -B $SMBA_BUILD +  -DCMAKE_BUILD_TYPE=RelWithDebInfo +  -DSMBA_BUILD_PLUGIN=OFF -DSMBA_BUILD_TESTS=ON +  -DSMBA_REQUIRE_Z3=ON -DSMBA_DEPS_PREFIX=$SMBA_DEPS
uvx --from cmake cmake --build $SMBA_BUILD --parallel
uvx --from cmake ctest --test-dir $SMBA_BUILD --output-on-failure

uv run python -m unittest discover -s tests -p 'test_*.py' -q
~~~

Run the separately configured forced-no-Z3 branch as well. It confirms that
ordinary core simplification may report 'Probabilistic' only for diagnostics,
whereas every constant-comparison predicate rejects without Z3. The adapter
and its workflow always require Z3. Full commands and the test matrix are in
[docs/BUILD_AND_TEST.md](docs/BUILD_AND_TEST.md).

## Recovery and proof boundary

Recovery starts from reachable normal MLIL roots and validates a two-way
normal/SSA expression-index mapping before following SSA def-use. It retains
the exact MLIL width: narrower values are masked at their semantic width on
'MLIL_ZX', and mixed-width operations without an explicit legal extension are
rejected. Arithmetic candidates must contain both arithmetic and bitwise
content; CoBRA must reduce cost and Z3 must prove equivalence.

Comparison roots are also structural candidates. The adapter handles signed
and unsigned equality/inequality/order relations with the exact bit-vector
width and accepts only a Z3 proof that the comparison is always '1' or always
'0'. 'MLIL_BOOL_TO_INT' is over-approximated only when it is a predicate leaf:
its result is constrained to '0' or '1', not expanded as an arbitrary integer
definition. Predicate roots take priority over accepted arithmetic descendants
so an accepted predicate replaces its complete operand tree once.

PHI definitions, load/call outputs, memory/alias-dependent definitions,
unsupported casts, width violations, cyclic/budget-exceeding expansion, and
unavailable SSA stop recovery at conservative leaves or reject the root. There
is no best-effort rewrite. In particular, '0x51ee30' remains intentionally
unresolved unless a future structural recovery/proof path supports it.

Workflow transformation re-collects candidates, selects non-overlapping
roots, copies normal MLIL into a fresh 'MediumLevelILFunction', substitutes
only accepted roots, finalizes/regenerates SSA, and calls
'AnalysisContext::SetMediumLevelILFunction' only after the full copy succeeds.
An exception discards the new function and leaves the context unchanged.

## Commands and workflow lifecycle

The plugin registers exactly two function commands:

* **SMBA CoBRA / Preview verified MBA simplifications** calls
  'smba::PreviewFunction'. It is observational: it does not create SSA,
  construct replacement IL, change MLIL, change a default setting, save a
  BNDB, or edit machine code.
* **SMBA CoBRA / Register or refresh current .mba workflow** implements the
  lifecycle below and does not change the selected workflow.

Let the current workflow be 'W'. The register/refresh command first checks
'W' for 'extension.smba.cobra.simplifyMlil', rather than inferring presence
from a '.mba' suffix. If present, it refreshes 'W' in place only when the exact
Binary Ninja 'Activity' is owned by this plugin process; a same-name foreign
activity is refused. When the activity is absent it derives literal 'W.mba';
an existing target is refreshed only if its exact activity is owned. Thus a
suffix alone is never evidence of installation.

Registered workflow topology is immutable. Ownership is the exact Binary Ninja
'Activity' object identity retained by this plugin process, not merely the
activity name. A refresh advances the activity dispatch generation and replaces
its action while retaining the same Activity object, preventing duplicate
activities or graph mutation.

'extension.smba.cobra.base' remains a compatibility workflow clone of
'core.function.metaAnalysis'; registering it never selects it. It is opt-in
through 'analysis.workflows.functionWorkflow', as is every 'W.mba' workflow.

## Dedicated AI CLI

The only public CLI form is:

~~~
ai_cli.py --target TARGET {preview,register-workflow} FUNCTION
~~~

'FUNCTION' is a '0x' address or exact function name. The CLI maps those two
subcommands one-to-one to the two menu commands above; it does not accept
JSON/file input, list capabilities, activate a workflow, reanalyze, save, or
provide generic plugin-command execution. It requires Python 3.11 or newer;
an older interpreter prints one clear version error to stderr and exits without
loading Binary Ninja.

~~~bash
AI_CLI=$PWD/artifact/plugin/smba_cobra_mba/ai_cli.py
uv run python $AI_CLI --target active preview 0x51e970
uv run python $AI_CLI --target active register-workflow target_function
~~~

Preview does not modify analysis or save. Register/refresh may update only the
workflow registry; it does not select a workflow, reanalyze, or save. Both
commands require an exact 'PluginCommand' match, direct 'is_valid', bounded
synchronous host-log capture, and a native '[SMBA AI JSON]' record whose
numeric 'function_start' exactly matches the selected function. Failures emit
JSON and exit '2'; success exits '0'.

The script, including '-h' and each subcommand help, is directory independent:
with Python 3.11 or newer it loads no Binary Ninja module until execution and
reloads itself through its absolute '__file__' path for transport. The full
contract, native-result fields, artifact closure, script inventory, and test evidence are in
[docs/AI_CLI_AND_ARTIFACT.md](docs/AI_CLI_AND_ARTIFACT.md).

Build the deterministic artifact after a plugin build:

~~~bash
uv run python scripts/build_artifact.py
uv run python scripts/build_artifact.py --verify
~~~

The artifact contains exactly 'README.md', 'ai_cli.py', the loadable
'smba-cobra-mba.dylib', and its manifest. Copy the dylib itself to the Binary
Ninja user-plugin directory; the folder is an automation bundle, not an
assumption that Binary Ninja imports adjacent Python automatically.

## Evidence and rollback

The current generic-predicate Preview evidence is
[../draft/smba_cobra_validation/preview_generic_predicates.log](../draft/smba_cobra_validation/preview_generic_predicates.log):
14 candidates were accepted. It includes true predicates at '0x51eb10' and
'0x51ebd4', false predicates at '0x51ed34' and '0x51ee90', and deliberately no
entry at '0x51ee30'. The legacy arithmetic baseline remains separate regression
coverage, not an exact current candidate count.

Keep 'input.bndb' immutable, use a disposable copy for Preview/workflow
experiments, and save an applied result only under a new output path. Preview
needs no rollback because it is read-only; for an unsaved workflow result,
close without saving and reopen the input. For a saved result, remove only the
disposable output and reopen the preserved input. Lifecycle evidence and
caveats are in [docs/VALIDATION.md](docs/VALIDATION.md) and
[docs/WORKFLOW_AND_ROLLBACK.md](docs/WORKFLOW_AND_ROLLBACK.md).

## Helper inventory

The adapter contains no persisted Python, Frida, or target-process injection
scripts. Ephemeral Binary Ninja Python helpers used for validation are
inventoried in 'docs/VALIDATION.md' with their inputs, outputs, analysis point,
usage, and risk. They drive the BN host only; they do not attach to, inject
into, or alter an Android process.
