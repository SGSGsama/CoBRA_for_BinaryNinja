# BN CoBRA MBA adapter

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

Prerequisites:

- Git and `uv` (the commands below use `uvx` to run CMake).
- A C++23-capable compiler and its normal platform build tools.
- A matching Binary Ninja installation only when building the optional plugin.

```bash
git clone --recurse-submodules <repository-url> smba-cobra-mba
cd smba-cobra-mba
git submodule update --init --recursive
git submodule status
```

The recorded submodule revisions are part of the source boundary. Do not
replace them with copied dependency trees or an API checkout from a different
Binary Ninja release.

## Build dependencies

Build CoBRA's dependencies, including Z3, into a temporary prefix outside the
source checkout:

```bash
SMBA_REPO="$PWD"
SMBA_DEPS_BUILD="$(mktemp -d /tmp/smba-cobra-mba-deps-z3.XXXXXX)"
uvx --from cmake cmake -S "$SMBA_REPO/third_party/CoBRA/dependencies" \
  -B "$SMBA_DEPS_BUILD" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCOBRA_ENABLE_Z3=ON \
  -DCOBRA_BUILD_TESTS=OFF
uvx --from cmake cmake --build "$SMBA_DEPS_BUILD" --parallel
SMBA_DEPS_PREFIX="$SMBA_DEPS_BUILD/install"
```

Keep `SMBA_REPO` and `SMBA_DEPS_PREFIX` set in the shell used by the following
commands.

## Build and test the headless core

The core build does not require Binary Ninja:

```bash
SMBA_CORE_BUILD="$(mktemp -d /tmp/smba-cobra-mba-core-z3.XXXXXX)"
uvx --from cmake cmake -S "$SMBA_REPO" -B "$SMBA_CORE_BUILD" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DSMBA_BUILD_PLUGIN=OFF \
  -DSMBA_BUILD_TESTS=ON \
  -DSMBA_REQUIRE_Z3=ON \
  -DSMBA_DEPS_PREFIX="$SMBA_DEPS_PREFIX"
uvx --from cmake cmake --build "$SMBA_CORE_BUILD" --parallel
uvx --from cmake ctest --test-dir "$SMBA_CORE_BUILD" --output-on-failure
```

To run the complete required matrix (strict Z3 plus forced-no-Z3 safety
tests) in fresh temporary directories:

```bash
scripts/validate.sh --deps-prefix "$SMBA_DEPS_PREFIX"
```

The forced-no-Z3 branch is diagnostic only: no comparison or workflow rewrite
is authorized unless Z3 proves it.

## Build the Binary Ninja plugin

The Binary Ninja plugin build is optional and requires an installed Binary
Ninja application matching the pinned API submodule. On macOS, the default
application path can be passed explicitly:

```bash
SMBA_PLUGIN_BUILD="$(mktemp -d /tmp/smba-cobra-mba-plugin.XXXXXX)"
uvx --from cmake cmake -S "$SMBA_REPO" -B "$SMBA_PLUGIN_BUILD" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DSMBA_BUILD_PLUGIN=ON \
  -DSMBA_BUILD_TESTS=OFF \
  -DSMBA_REQUIRE_Z3=ON \
  -DSMBA_DEPS_PREFIX="$SMBA_DEPS_PREFIX" \
  -DBN_INSTALL_DIR="/Applications/Binary Ninja.app"
uvx --from cmake cmake --build "$SMBA_PLUGIN_BUILD" --parallel
```

Building does not install the plugin. After inspecting the generated install
destination, installation is an explicit optional step:

```bash
uvx --from cmake cmake --build "$SMBA_PLUGIN_BUILD" --target install
```

Restart Binary Ninja after installation. See
[`docs/BUILD_AND_TEST.md`](docs/BUILD_AND_TEST.md) for the full two-branch test
contract, static checks, and platform notes.

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
