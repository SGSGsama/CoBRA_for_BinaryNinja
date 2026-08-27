# SMBA dedicated CLI and deterministic artifact contract

'scripts/ai_cli.py' is a narrow, non-interactive interface to exactly two
SMBA CoBRA Binary Ninja function-menu commands. It controls the Binary Ninja
host only; it neither attaches to, injects into, nor patches the Android
target process.

## Command line

The complete public grammar is:

~~~
ai_cli.py --target TARGET {preview,register-workflow} FUNCTION
~~~

'FUNCTION' is either a non-negative '0x' address or a nonempty exact Binary
Ninja function name. An address matches only a function with the identical
start; a name uses exact equality, never a substring or first-match fallback.
Zero matches and multiple matches fail closed.

Python 3.11 or newer is required. The gate runs immediately after the required
future-annotations declaration and before argparse, Binary Ninja, or any other
version-sensitive import. Under Python 3.9, including for '-h', stderr is only
'ai_cli.py requires Python 3.11 or newer', stdout is empty, and the process
exits nonzero without a traceback.

~~~bash
AI_CLI="$PWD/artifact/plugin/smba_cobra_mba/ai_cli.py"

# Preview is read-only: no selected-function analysis change and no BNDB save.
uv run python "$AI_CLI" --target active preview 0x51e970

# This only registers or refreshes the current .mba workflow. It does not
# select a workflow, reanalyze, or save a BNDB.
uv run python "$AI_CLI" --target active register-workflow target_function
~~~

There are no JSON/file input switches, capability listing, workflow-selection,
reanalysis, save, or generic plugin-command execution modes. In particular,
the former JSON input and activation interfaces are intentionally rejected by
argparse.

All help paths are local, state the Python 3.11 minimum, and require neither
'bn' nor a Binary Ninja runtime:

~~~bash
uv run python "$AI_CLI" -h
uv run python "$AI_CLI" preview -h
uv run python "$AI_CLI" register-workflow -h
~~~

The two subcommand helps state their analysis/save behavior and show an
example. The transport reloads the script from 'Path(__file__).resolve()', so
the same absolute artifact path works from '/tmp' or another unrelated
directory; only 'bn' on 'PATH', the script itself, and the explicit arguments
are needed.

The CLI prints one compact JSON result and exits '0' only when 'ok' is true;
command refusals, invalid input, and transport failures exit '2'. Output keys
are 'plugin', 'operation', 'ok', 'accepted', 'modified', 'function', 'command',
'result', 'logs', and 'error'. 'modified: false' means neither command changed
the selected function's analysis; register/refresh may still update Binary
Ninja's workflow registry as its explicit menu action.

## Exact mappings and safety boundaries

| CLI subcommand | Exact Binary Ninja menu command | Analysis / save effect |
| --- | --- | --- |
| 'preview' | 'SMBA CoBRA\Preview verified MBA simplifications' | Observational only; does not modify analysis or save. |
| 'register-workflow' | 'SMBA CoBRA\Register or refresh current .mba workflow' | Registers or refreshes only; does not select a workflow, reanalyze, or save. |

Inside Binary Ninja the script exposes only 'run_preview(function_argument,
view)' and 'run_register_workflow(function_argument, view)'. Each fixed path
iterates registered 'PluginCommand' entries only to locate its own exact menu
name, builds a function-only 'PluginCommandContext(None)', calls
'is_valid(context)', and then executes that one command. It never calls
'get_valid_list', which can access 'context.project' on affected Binary Ninja
versions.

A short-lived 'BNLogListener' surrounds that synchronous call. Callback
references remain alive until unregister, direct callback exceptions become
structured failures, and an 'Unhandled Python exception' log is also treated
as failure. This is a bounded host-log capture, not target-process
instrumentation.

## Native result correlation

Human-readable '[SMBA]' messages remain unchanged. The native plugin emits one
compact machine result beginning with exactly:

~~~
[SMBA AI JSON]
~~~

Preview records use native operation 'preview'; register/refresh records use
native operation 'register_workflow'. The CLI preserves all captured log lines,
parses only valid tagged JSON objects, filters to the fixed operation, and
accepts a record only when numeric 'function_start' exactly equals the selected
function's start. Cross-talk, missing, malformed, or wrong-function records
fail closed.

Preview records carry 'accepted', 'applied', 'diagnostic', 'function_start',
and candidate data. Register records carry 'accepted', 'action'
('created', 'refreshed', or 'refused'), 'function_start', workflow names,
target, activity, and refusal reason where applicable. The CLI reports a
register record as successful only for 'accepted: true' with 'created' or
'refreshed'; it performs no follow-on action.

## Artifact build and verification

Build a loadable dylib first, then build the closed artifact directory:

~~~bash
cd analysis/SMBA_deobf/bn-cobra-mba
uv run python scripts/build_artifact.py
uv run python scripts/build_artifact.py --verify
~~~

The builder resolves defaults relative to its own '__file__', copies only
regular source payloads, verifies each copied payload is byte-identical to its
source input, fixes modes/timestamps, writes a deterministic manifest, and
then verifies the closure:

~~~
artifact/plugin/smba_cobra_mba/
├── README.md
├── ai_cli.py
├── manifest.json
└── smba-cobra-mba.dylib
~~~

'manifest.json' has schema 'smba-cobra-mba-artifact-v1' and deterministic
SHA-256/size entries for the three payloads. Verification rejects links,
unexpected files/directories, wrong order, sizes, or hashes. Rebuilding
replaces entries only inside the explicitly selected output directory.

Copy the **'smba-cobra-mba.dylib' itself** to the Binary Ninja user-plugin
directory. The artifact directory is a deterministic automation bundle, not a
claim that Binary Ninja imports the adjacent Python script automatically.

## Script and test inventory

| Path | Inputs / output | Validation target | Host-state / injection risk |
| --- | --- | --- | --- |
| 'scripts/ai_cli.py' | Target, one fixed subcommand, and exact function identity; JSON result. | Exact menu lookup, 'is_valid', command execution, tagged-log correlation. | No Android injection. Preview is read-only; register/refresh changes only workflow registration and neither analyzes nor saves. |
| 'scripts/build_artifact.py' | Source root, dylib, output; artifact and manifest. | Closed deterministic copy, byte identity, hash/mode/closure verification. | Deletes only entries in the selected artifact output while rebuilding; no BN or target interaction. |
| 'tests/test_ai_cli.py' | Fake BN objects, subprocess help, and fake transport. | Fixed-menu mapping, validity check, start correlation, callback failure, CLI grammar, absolute-path transport. | No live BN command, database, or target interaction. |
| 'tests/test_artifact_builder.py' | Temporary regular files. | Manifest ordering, source/artifact byte identity, and closure rejection. | Temporary filesystem only. |
| 'tests/plugin_json_tests.cpp' | Fixed C++ protocol fixtures. | Native JSON escaping and deterministic Preview/register result encodings. | Headless; no BN runtime. |

Run the source checks through 'uv':

~~~bash
uv run python -m unittest discover -s tests -p 'test_*.py' -q
uv run python scripts/build_artifact.py
uv run python scripts/build_artifact.py --verify
~~~

The CMake/CTest matrix remains in [BUILD_AND_TEST.md](BUILD_AND_TEST.md).
