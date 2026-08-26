# Workflow lifecycle, Preview, and rollback

The plugin has a read-only Preview command and an explicit **Register or refresh current .mba workflow** command. It has no direct Apply command; application occurs only when an explicitly selected workflow runs.

## `.mba` lifecycle and ownership

For a current workflow `W`, the command first asks whether `W` contains
`extension.smba.cobra.simplifyMlil`; it never uses a suffix as evidence that
the plugin is installed. If that activity is present, it can refresh only the
exact Activity object owned by this plugin process, in place and regardless of
the workflow name. If it is absent, the command derives literal `W.mba`, adds
the activity after `core.function.generateMediumLevelIL`, and registers it.
The command never changes the selected workflow or a default setting.

| Current/target state | Result | Mutation |
| --- | --- | --- |
| Current `W` contains the exact owned activity (for example `base.mba.dualbr`) | Refresh `W` in place. | Same Activity object; generation/action only. |
| Current `W` contains a same-name foreign activity | Refuse. | None. |
| Current `W` lacks the activity and no `W.mba` exists | Clone/register `W.mba` with the MBA activity. | New registered derivative only. |
| Current `W` lacks the activity and existing `W.mba` has the exact owned activity | Refresh target `W.mba`. | Same Activity object; generation/action only. |
| Current `W` lacks the activity and existing `W.mba` lacks it or has a foreign same-name activity | Refuse. | None. |
| Current name ends in `.mba` but lacks the activity | Treat it as literal `W`; derive `W.mba` if safe. | Suffix alone is never ownership evidence. |
| Clone lacks the MLIL anchor or already contains unowned activity | Refuse. | None. |

Registered workflow graphs are immutable. Ownership is checked by exact `Activity` object identity retained in the process, not activity-name equality: a same-name callback from another plugin is foreign. The owned Activity uses a locked mutable dispatch record containing a generation counter and action. Refreshing advances the generation and replaces the action, while retaining both the exact Activity and its registered topology. This permits a safe repeat command without an extra activity or topology mutation.

`extension.smba.cobra.base` is retained only as an opt-in compatibility clone of `core.function.metaAnalysis`; it is registered but not selected as a default. It is separate from the normal `W -> W.mba` lifecycle.

To select a registered derivative explicitly:

```python
from binaryninja import load

with load(
    "input.bin",
    options={"analysis.workflows.functionWorkflow": "core.function.metaAnalysis.mba"},
) as bv:
    bv.update_analysis_and_wait()
    bv.create_database("output-applied.bndb")
```

Use the real `W.mba` name for the selected binary. The selection is an ordinary Binary Ninja setting, not a hidden command or default-setting change by the plugin.

## Preview boundary

**SMBA CoBRA / Preview verified MBA simplifications** calls `smba::PreviewFunction` on normal MLIL and any pre-existing SSA form. It does not generate SSA, construct a replacement function, replace an expression, change a workflow/default setting, save a BNDB, or edit machine code. If SSA is absent it reports that Preview will not construct it.

The durable current evidence is `analysis/SMBA_deobf/draft/smba_cobra_validation/preview_generic_predicates.log`: 14 accepted generic candidates, with `0x51eb10`/`0x51ebd4` true and `0x51ed34`/`0x51ee90` false. `0x51ee30` does not appear by design because its unsupported dataflow remains conservative/unresolved. These are structural recovery results—not address/function/constant pattern matches—and Preview evidence never proves that MLIL changed.

## Workflow transformation boundary

When the selected `.mba` activity runs, it:

1. Collects reachable normal-MLIL roots and validates bidirectional normal/SSA expression-index correspondence.
2. Recovers pure SSA def-use with exact widths; `MLIL_ZX` applies the narrow semantic-width mask and unsupported/mixed-width casts reject.
3. Sends qualifying arithmetic/bitwise expressions to CoBRA and comparison roots to exact Z3 constant-comparison proof for every signed/unsigned relation. Predicate-only `MLIL_BOOL_TO_INT` is conservatively modelled as `0` or `1`.
4. Rejects or stops at PHI, load/call output, memory/alias, unsupported cast, cyclic, unavailable-SSA, or budget-limited dataflow. There is no fallback replacement or heuristic matching.
5. Selects non-overlapping proved roots (predicate roots before descendants), copies the whole function into fresh normal MLIL, rebuilds selected roots, finalizes/regenerates SSA, and atomically commits only after success via `AnalysisContext::SetMediumLevelILFunction`.

For arithmetic, CoBRA must reduce cost and Z3 must prove equivalence. For a predicate, Z3 proves that the exact bit-vector comparison is constantly `0` or `1`; signedness and width are part of the proof. A failed copy/rebuild discards the new function and leaves the original context untouched. No-Z3 diagnostic results are never eligible for the activity.

## Copy discipline and rollback

Keep the source database and all experiment artifacts distinct:

```text
input.bndb          # immutable source
preview-copy.bndb   # disposable Preview/workflow input
preview.log         # retained read-only candidate evidence
output-applied.bndb # separately saved workflow result, if intentionally saved
```

Run Preview on a disposable copy first. Only then select `W.mba` on a separate copy and save under a new filename. For an unsaved transform, close without saving and reopen `input.bndb`; for a saved transform, remove only `output-applied.bndb` and reopen the preserved source. There is no cross-database undo log. See `workflow_register_refresh.log` for evidence that the registration command did not save or install anything.
