# SMBA validation manifest

Validation date: 2026-08-20 (Asia/Shanghai). Generated outputs named below are evidence paths outside the source tree; no source C++ was changed for this documentation/evidence update.

The 2026-08-26 activity-first lifecycle repair supersedes the old suffix-based
interpretation of the workflow probe below. This document retains the original
observations as historical evidence; current usage is defined by
[`WORKFLOW_AND_ROLLBACK.md`](WORKFLOW_AND_ROLLBACK.md), where activity presence
and exact ownership—not a `.mba` suffix—select refresh versus derivation.

## Reproducible headless validation

Use the `uvx` CMake/CTest commands in [`BUILD_AND_TEST.md`](BUILD_AND_TEST.md) for both required core branches:

| Branch | Required result |
| --- | --- |
| Z3-enabled core | Arithmetic simplification plus generic constant-comparison tests pass; accepted predicates are `ProvedConstant` and `z3Verified`. |
| Forced no-Z3 core | Core tests pass; predicate proofs fail closed and no predicate is classified as probabilistic/verified. |

The core predicate coverage includes true/false constants, non-constant comparisons, signed-versus-unsigned ordering, wide logical shifts, invalid width/variable inputs, and Z3-unavailable behavior. Plugin/workflow builds require Z3 and are not validated by the no-Z3 diagnostic configuration.

## Durable runtime evidence

The concise evidence artifacts are intentionally retained rather than raw multi-megabyte Binary Ninja analysis output:

| Artifact | What it establishes |
| --- | --- |
| `analysis/SMBA_deobf/draft/smba_cobra_validation/preview_generic_predicates.log` | Read-only Preview accepted 14 generic candidates. `0x51eb10` and `0x51ebd4` are proved true; `0x51ed34` and `0x51ee90` are proved false; `0x51ee30` has no candidate. |
| `/tmp/smba-disposable-legacy-preview.log` (source evidence) | The legacy arithmetic candidates at `0x444478`, `0x444464`, `0x4444f4`, `0x4444f0`, and `0x44475c` remain recoverable, together with additional safe candidates. It is regression context, not the current exact count. |
| `analysis/SMBA_deobf/draft/smba_cobra_validation/workflow_register_refresh.log` | `core.function.metaAnalysis.mba` registered while selection remained `core.function.metaAnalysis`; repeat refresh retained Activity `0x8de49eb30`, advanced generation `1 -> 2`, and fail-closed for missing or foreign activities. The appended post-install smoke retained Activity `0x9cad37b70` in a fresh BN process. |

The generic Preview proves neither workflow application nor any write: Preview is read-only and does not create SSA, write MLIL, change a default setting, save a BNDB, or edit machine code. `0x51ee30` is intentionally unresolved by current conservative recovery; it is not filtered by an address-specific rule.

## Workflow lifecycle check

The workflow record documents the following observations:

1. From `core.function.metaAnalysis`, registration created `core.function.metaAnalysis.mba` without changing the selected workflow.
2. Repeating the command refreshed the exact owned Activity object `0x8de49eb30`; its mutable dispatch generation advanced from `1` to `2` while the registered workflow topology stayed immutable.
3. In this pre-repair probe, a target with no MBA activity was refused and a foreign same-name Activity was also refused; neither path was repaired or renamed, so this particular probe created no `.mba.mba` derivative. The current activity-first contract instead treats an activity-absent suffix-shaped current name literally and derives only after the absence check; foreign present activities remain refused.
4. Source and disposable BNDB SHA-256 values matched before/after, and that isolated probe made no save or installation. A separate post-install smoke loaded the installed dylib in a fresh BN process, repeated the command on a read-only raw ELF view, retained Activity `0x9cad37b70`, kept selection/default unchanged, and left the ELF SHA-256 unchanged.

`extension.smba.cobra.base` remains an unselected compatibility clone. Normal operation is opt-in selection of the explicitly registered `W.mba` workflow.

## Recovery/proof assertions covered by the evidence

Candidate selection is structural, not address/function/constant matching. Normal/SSA root mappings must agree in both directions; SSA def-use recovery preserves exact widths with `MLIL_ZX` masks. Arithmetic candidates require CoBRA cost reduction plus a Z3 proof; comparison candidates use exact signed/unsigned Z3 proofs for constant truth values. `MLIL_BOOL_TO_INT` is over-approximated only in predicate context as `0`/`1`; PHI, load/call, memory/alias, unsupported casts, cycles, and resource limits are conservative leaves or rejection paths.

Workflow transformation commits only a successful fresh normal-MLIL copy after finalization and SSA regeneration. The documented behavior is therefore consistent with read-only Preview and fail-closed registration, but the evidence does not claim that every possible MLIL operation or an interactive GUI reload has been dynamically exercised.

## Ephemeral Binary Ninja Python helper inventory

No helper file was persisted in the repository. Validation used narrow, ephemeral BN Python here-doc snippets in the Binary Ninja host; each was discarded after producing the retained log excerpt.

| Helper | Purpose / inputs | Outputs / analysis point | Invocation and risk |
| --- | --- | --- | --- |
| Generic Preview collector | Disposable BNDB, rebuilt adapter, selected functions. | Preview report lines, accepted count, truth values, and absence of `0x51ee30`. | Passed to BN Python on standard input; read-only BN-host analysis, no Android attach/injection. |
| Workflow register/refresh probe | Disposable BNDB, `core.function.metaAnalysis`, repeated registration command, controlled missing/foreign activity cases. | Registered/selected workflow names, Activity identity, dispatch generation/action, refusal paths. | Passed to BN Python on standard input; exercises BN workflow objects only, no Android attach/injection. |
| SHA-256 / save guard | Source and disposable BNDB paths. | Matching SHA-256 before/after and confirmation of no save/install. | Host-side file hash and BN state observation; no target-process interaction. |

These helpers have no hooks into the Android target, no Frida usage, no device injection, and no machine-code modification. A future persistent or ephemeral helper must extend this inventory with its exact input/output, analysis point, validation objective, invocation, and injection risk.
