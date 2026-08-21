# Architecture and proof boundary

## Target graph

```text
smba-core-tests -> smba-core -> cobra-core -> absl, highway
                                    `-> cobra-verify -> Z3

smba-cobra-mba -> smba-core
               `-> binaryninjaapi -> Binary Ninja core installation
```

`smba-core` is a pure CoBRA boundary and has no Binary Ninja dependency.
`smba-core-tests` is deliberately headless: its compile commands must not pull
Binary Ninja headers, libraries, or runtime state. The plugin target is built
only when `SMBA_BUILD_PLUGIN=ON` and requires the CoBRA Z3 verifier.

## Candidate recovery

Recovery starts at reachable normal MLIL roots and validates a two-way normal
and SSA expression-index mapping before following SSA def-use. It preserves the
exact MLIL width: `MLIL_ZX` is masked at the semantic narrow width, while
mixed-width operations without an explicit legal extension are rejected.
Arithmetic candidates require both arithmetic and bitwise content.

PHI definitions, load/call results, memory or alias-dependent values,
unsupported casts, cyclic definitions, unavailable SSA, and resource-budget
exhaustion stop recovery at conservative leaves or reject the root. There is
no pattern-based exception and no best-effort rewrite.

## Simplification and proof

CoBRA must return a lower-cost expression. The core then runs a full-width
differential check and, when Z3 is available, an exact bit-vector equivalence
proof. Without the verifier, a successful differential check is reported only
as `Probabilistic`; it is useful for offline diagnostics but is never rewrite
authority.

Comparison roots use exact signed or unsigned bit-vector relations. A root is
accepted only when Z3 proves that it is true for every assignment or false for
every assignment. `MLIL_BOOL_TO_INT` is over-approximated only when it is a
predicate leaf, where its value is constrained to `0` or `1`. Predicate roots
take priority over accepted arithmetic descendants so a chosen predicate
replaces its whole operand tree once.

`SMBA_REQUIRE_Z3` is a configure-time availability gate. The plugin separately
requires the actual `cobra-verify` target even when the optional core-only gate
is off; no plugin or workflow configuration can fall back to probabilistic
approval.

## Commit discipline

The workflow activity re-collects candidates, selects non-overlapping proved
roots, copies all normal MLIL into a new `MediumLevelILFunction`, substitutes
only the selected roots while copying, finalizes and regenerates SSA, then
calls `AnalysisContext::SetMediumLevelILFunction`. An exception or any failed
stage discards the new function and leaves the original analysis context
unchanged.

Preview calls the same conservative recovery path but is observational. It
does not manufacture SSA or replacement IL, mutate the supplied function,
write a database, or edit binary bytes.
