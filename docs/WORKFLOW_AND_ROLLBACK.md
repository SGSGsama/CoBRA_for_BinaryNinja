# Workflow lifecycle and rollback

The plugin exposes a read-only Preview command and an explicit **Register or
refresh current .mba workflow** command. It has no direct Apply command;
transformation occurs only when an explicitly selected workflow runs.

## `.mba` lifecycle and ownership

For a current workflow `W`, the command creates at most one derivative,
`W.mba`. It clones `W`, places
`extension.smba.cobra.simplifyMlil` after
`core.function.generateMediumLevelIL`, and registers the result. It neither
selects the new workflow nor creates `W.mba.mba`.

| Current/target state | Result | Mutation |
| --- | --- | --- |
| `W` and no `W.mba` | Clone and register `W.mba` with the activity. | New registered derivative only. |
| Selected owned `W.mba` | Refresh its dispatch. | Same Activity object; generation/action only. |
| Existing `W.mba` lacks the activity | Refuse. | None. |
| Existing or selected `W.mba` has a foreign same-name activity | Refuse. | None. |
| Clone lacks the MLIL anchor or has an unowned activity | Refuse. | None. |

Registered workflow graphs are immutable. Ownership is exact Binary Ninja
`Activity` object identity held by this plugin process, not name equality. The
owned Activity holds a locked dispatch record with a generation and action;
refresh replaces the action and increments the generation without adding an
activity or altering registered topology.

`extension.smba.cobra.base` is an opt-in compatibility clone of
`core.function.metaAnalysis`. It is registered but never selected as a default
and is separate from the normal `W -> W.mba` lifecycle.

To choose a registered derivative explicitly:

```python
from binaryninja import load

with load(
    "input.bin",
    options={"analysis.workflows.functionWorkflow": "core.function.metaAnalysis.mba"},
) as bv:
    bv.update_analysis_and_wait()
    bv.create_database("output-applied.bndb")
```

Use the actual registered `W.mba` name for the binary under analysis. Selection
is an ordinary Binary Ninja setting, not a hidden default-setting change.

## Transformation boundary

An enabled `.mba` activity collects and validates normal/SSA roots, recovers
pure width-preserving dataflow, requires lower CoBRA cost and a Z3 proof for
arithmetic roots, and uses Z3 to prove constant comparison roots. It rejects
unsupported casts, PHI/load/call/memory-dependent inputs, cycles, missing SSA,
and budget-limited recovery. No no-Z3 or partially recovered result is eligible
for a workflow rewrite.

It then copies the complete function into fresh normal MLIL, substitutes only
proved non-overlapping roots, finalizes/regenerates SSA, and atomically commits
the new function only after success. A failed copy or proof leaves the original
context untouched.

## Preview and rollback

Preview is read-only: it does not create SSA, build replacement IL, replace an
expression, change a workflow/default setting, save a BNDB, or edit machine
code. It therefore needs no rollback.

For workflow experimentation, preserve the original database and use distinct
copies:

```text
input.bndb          # immutable source
experiment.bndb     # disposable workflow input
preview.log         # retained observational output
output-applied.bndb # separately saved result, when intentionally requested
```

Run Preview before selecting `W.mba` on a disposable copy. For an unsaved
transform, close without saving and reopen `input.bndb`. For a saved transform,
remove only the intentional output copy and reopen the preserved input. There
is no cross-database undo log.
