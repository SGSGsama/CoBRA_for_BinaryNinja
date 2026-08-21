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

The initial standalone validation record is appended after the fresh build
matrix completes. See [`BUILD_AND_TEST.md`](BUILD_AND_TEST.md) for the stable
commands and [`WORKFLOW_AND_ROLLBACK.md`](WORKFLOW_AND_ROLLBACK.md) for the
operational safety boundary.
