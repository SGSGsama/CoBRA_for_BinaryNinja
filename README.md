# SMBA CoBRA MBA adapter

`smba-core` recovers selected pure MLIL SSA expression trees, reduces mixed
boolean-arithmetic expressions with CoBRA, and verifies eligible rewrites with
Z3. `smba-cobra-mba` is the Binary Ninja adapter: it provides a read-only
Preview command and an opt-in `.mba` workflow activity. Candidate selection is
structural; it has no address, function-name, or constant-value allowlist.

The repository is standalone. Its only source dependencies are pinned Git
submodules under `third_party/`:

```text
include/       public core and Binary Ninja-facing interfaces
src/           CoBRA boundary and Binary Ninja adapter
tests/         headless core tests
docs/          reproducible build, safety, and validation records
scripts/       optional validation automation
third_party/   pinned CoBRA and Binary Ninja API submodules
```

## Start here

```bash
git clone --recurse-submodules <repository-url> smba-cobra-mba
cd smba-cobra-mba
git submodule status
```

The recorded submodule revisions are part of the source boundary. Do not
replace them with copied dependency trees or an API checkout from a different
Binary Ninja release. Build the dependency prefix and run the required Z3 and
forced-no-Z3 test branches using the `uvx` commands in
[`docs/BUILD_AND_TEST.md`](docs/BUILD_AND_TEST.md). The latter branch is a
diagnostic-only check: no comparison or workflow rewrite is authorized unless
Z3 proves it.

The Binary Ninja plugin build is optional and requires an installed Binary
Ninja application. It is documented separately in
[`docs/BUILD_AND_TEST.md`](docs/BUILD_AND_TEST.md); configure and build it
before deciding whether to run its install target. This repository never runs
installation implicitly.

## Safety model

Preview is observational. It does not create SSA, construct replacement IL,
change a workflow/default setting, save a database, or edit machine code.
Application occurs only in an explicitly selected registered `.mba` workflow.
The workflow copies the function into fresh MLIL and commits only after
recovery, proof, and regeneration succeed; failure discards the new function.

[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) describes the recovery and proof
boundary, while [`docs/WORKFLOW_AND_ROLLBACK.md`](docs/WORKFLOW_AND_ROLLBACK.md)
defines workflow ownership, lifecycle, and rollback. Validation commands and
the current reproducibility record are in
[`docs/VALIDATION.md`](docs/VALIDATION.md).

## License status

No license has been selected for this repository. Redistribution and use terms
are unspecified until the repository owner supplies an explicit license.
Third-party submodules retain their own upstream licensing and notices.
