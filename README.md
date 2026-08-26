# SMBA CoBRA MBA adapter

`smba-core` recovers selected pure MLIL SSA expression trees, simplifies arithmetic/bitwise MBA expressions with CoBRA, and proves eligible constant comparisons with Z3. `smba-cobra-mba` is the Binary Ninja adapter: it exposes a read-only Preview command and a deliberately fail-closed `.mba` workflow lifecycle. No address, function-name, or constant-value pattern list selects candidates; eligibility is structural SSA recovery plus proof.

The source layout is:

```text
analysis/SMBA_deobf/
├── CoBRA/                 # CoBRA source, consumed with add_subdirectory
├── binaryninja-api/       # Binary Ninja C++ API source
└── bn-cobra-mba/          # this adapter
```

Generated `build-*`, `.build/`, CMake caches, objects, libraries, and plugin bundles are not source artifacts. Use out-of-tree builds (for example under `/tmp`) and the `uvx` commands below; do not create a separate Python environment.

## Build and test

The validated CoBRA dependency prefix is:

```text
analysis/SMBA_deobf/CoBRA/build-deps-smba/install
```

From `analysis/SMBA_deobf/bn-cobra-mba`, strict core validation is:

```bash
SMBA_DEPS="$PWD/../CoBRA/build-deps-smba/install"
SMBA_BUILD="/tmp/smba-cobra-mba-core"
uvx --from cmake cmake -S . -B "$SMBA_BUILD" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DSMBA_BUILD_PLUGIN=OFF -DSMBA_BUILD_TESTS=ON \
  -DSMBA_REQUIRE_Z3=ON -DSMBA_DEPS_PREFIX="$SMBA_DEPS"
uvx --from cmake cmake --build "$SMBA_BUILD" --parallel
uvx --from cmake ctest --test-dir "$SMBA_BUILD" --output-on-failure
```

Run the separately configured forced-no-Z3 branch as well; it confirms that ordinary core simplification may report `Probabilistic` only for diagnostics, whereas every constant-comparison predicate rejects without Z3. The adapter and its workflow always require Z3. Full commands and the test matrix are in [`docs/BUILD_AND_TEST.md`](docs/BUILD_AND_TEST.md).

## Recovery and proof boundary

Recovery starts from reachable normal MLIL roots and validates a two-way normal/SSA expression-index mapping before following SSA def-use. It retains the exact MLIL width: narrower values are masked at their semantic width on `MLIL_ZX`, and mixed-width operations without an explicit legal extension are rejected. Arithmetic candidates must contain both arithmetic and bitwise content; CoBRA must reduce cost and Z3 must prove equivalence.

Comparison roots are also structural candidates. The adapter handles the signed and unsigned equality/inequality/order relations using the exact bit-vector width and accepts only a Z3 proof that the comparison is always `1` or always `0`. `MLIL_BOOL_TO_INT` is deliberately over-approximated only when it is a predicate leaf: its result is constrained to `0` or `1`, not mistakenly expanded as an arbitrary integer definition. Predicate roots take priority over their accepted arithmetic descendants so an accepted predicate replaces its complete operand tree once.

PHI definitions, load/call outputs, memory/alias-dependent definitions, unsupported casts, width violations, cyclic/budget-exceeding expansion, and unavailable SSA all stop recovery at conservative leaves or reject the root. There is no best-effort rewrite. In particular, `0x51ee30` is intentionally unresolved: it is not a negative address rule and should become eligible only if a future structural recovery/proof path supports it.

Workflow transformation re-collects candidates, selects non-overlapping roots, copies all normal MLIL into a fresh `MediumLevelILFunction`, substitutes only the accepted roots during that copy, finalizes/regenerates SSA, and calls `AnalysisContext::SetMediumLevelILFunction` only after the full copy succeeds. An exception discards the new function and leaves the context unchanged.

## Commands and workflow lifecycle

The plugin registers two function commands:

* **SMBA CoBRA / Preview verified MBA simplifications** calls `smba::PreviewFunction`. It is observational: it does not create SSA, construct replacement IL, change MLIL, change a default setting, save a BNDB, or edit machine code.
* **SMBA CoBRA / Register or refresh current .mba workflow** implements the lifecycle below and does not change the selected workflow.

Let the current workflow be `W`. The command first checks `W` for
`extension.smba.cobra.simplifyMlil`, rather than inferring presence from a
`.mba` suffix. If it is present, the command refreshes `W` in place only when
its exact Binary Ninja `Activity` is owned by this plugin process; this also
works for a composed name such as `base.mba.dualbr`. A same-name foreign
activity is refused. Only when the current workflow has no MBA activity does
the command use the suffix, deriving literal `W.mba`; if that target already
exists it is refreshed only when its exact activity is owned, otherwise it is
refused. Thus a suffix alone is not evidence of installation: an
activity-absent workflow named `W.mba` is treated literally and may derive
`W.mba.mba`.

Registered workflow topology is immutable. Ownership is the exact Binary Ninja `Activity` object identity retained by this plugin process, not merely the activity name. The activity holds a locked mutable dispatch state with a generation and action: a refresh advances the generation and replaces the action while retaining the same Activity object. This is why a repeated registration can safely refresh behavior without duplicate activities or graph mutation.

`extension.smba.cobra.base` remains a compatibility workflow clone of `core.function.metaAnalysis`; registering it never selects it. It is opt-in through `analysis.workflows.functionWorkflow`, as is every `W.mba` workflow. Do not describe it as the normal derivative or as a global default.

```python
from binaryninja import load

with load(
    "input.bin",
    options={"analysis.workflows.functionWorkflow": "core.function.metaAnalysis.mba"},
) as bv:
    bv.update_analysis_and_wait()
    bv.create_database("output-applied.bndb")
```

The example explicitly selects an already registered workflow; it is not an Apply command and should use the actual `W.mba` name in the database.

## Evidence and rollback

The current generic-predicate Preview evidence is [`../draft/smba_cobra_validation/preview_generic_predicates.log`](../draft/smba_cobra_validation/preview_generic_predicates.log): 14 candidates were accepted. It includes true predicates at `0x51eb10` and `0x51ebd4`, false predicates at `0x51ed34` and `0x51ee90`, and deliberately no entry at `0x51ee30`. The legacy arithmetic baseline remains separately recorded as regression coverage, not an exact current candidate count.

Keep `input.bndb` immutable, use a disposable copy for Preview/workflow experiments, and save an applied result only under a new output path. Preview needs no rollback because it is read-only; for an unsaved workflow result, close without saving and reopen the input. For a saved result, remove only the disposable output and reopen the preserved input. The lifecycle evidence and complete caveats are in [`docs/VALIDATION.md`](docs/VALIDATION.md) and [`docs/WORKFLOW_AND_ROLLBACK.md`](docs/WORKFLOW_AND_ROLLBACK.md).

## Helper inventory

The adapter contains no persisted Python, Frida, or target-process injection scripts. Ephemeral Binary Ninja Python helpers used for validation are inventoried in `docs/VALIDATION.md` with their inputs, outputs, analysis points, usage, and risk. They drive the BN host only; they do not attach to, inject into, or alter an Android process.
