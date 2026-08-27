#!/usr/bin/env python3
"""Dedicated command-line bridge for the two SMBA CoBRA Binary Ninja menus.

The ordinary-Python entry point sends a narrow bn py exec program to an
already-open Binary Ninja target. The program reloads this exact script by
absolute path and invokes one fixed in-process entry point: Preview or
Register/refresh. There is no generic plugin-command execution interface.
"""

from __future__ import annotations

import sys as _version_sys


if _version_sys.version_info < (3, 11):
    print("ai_cli.py requires Python 3.11 or newer", file=_version_sys.stderr)
    raise SystemExit(1)


import argparse
import ctypes
import json
import pathlib
import subprocess
from contextlib import AbstractContextManager
from dataclasses import dataclass
from types import SimpleNamespace
from typing import Any, Iterable, Mapping, Sequence


PLUGIN_NAME = "smba-cobra-mba"
MACHINE_RESULT_PREFIX = "[SMBA AI JSON]"
PREVIEW_COMMAND_NAME = "SMBA CoBRA\\Preview verified MBA simplifications"
REGISTER_WORKFLOW_COMMAND_NAME = "SMBA CoBRA\\Register or refresh current .mba workflow"
PREVIEW_OPERATION = "preview"
REGISTER_WORKFLOW_OPERATION = "register-workflow"
NATIVE_REGISTER_WORKFLOW_OPERATION = "register_workflow"


class CommandError(ValueError):
    """An expected command failure returned as a machine-readable response."""

    def __init__(
        self,
        code: str,
        message: str,
        result: Mapping[str, Any] | None = None,
        logs: Sequence[str] | None = None,
    ):
        super().__init__(message)
        self.code = code
        self.message = message
        self.result = dict(result or {})
        self.logs = list(logs or [])


@dataclass(frozen=True)
class FunctionSelector:
    """A strict address or an exact Binary Ninja function name."""

    start: int | None = None
    name: str | None = None


def _error(code: str, message: str) -> dict[str, str]:
    return {"code": code, "message": message}


def _response(
    operation: str,
    *,
    ok: bool = False,
    accepted: bool = False,
    modified: bool = False,
    function: Mapping[str, Any] | None = None,
    command: str | None = None,
    result: Mapping[str, Any] | None = None,
    logs: Sequence[str] | None = None,
    error: Mapping[str, str] | None = None,
) -> dict[str, Any]:
    """Build the small, stable output envelope used by both fixed commands."""

    return {
        "plugin": PLUGIN_NAME,
        "operation": operation,
        "ok": bool(ok),
        "accepted": bool(accepted),
        # "modified" means selected-function analysis changed. Workflow
        # registration can update the workflow registry while remaining false.
        "modified": bool(modified),
        "function": dict(function) if function is not None else None,
        "command": command,
        "result": dict(result or {}),
        "logs": list(logs or []),
        "error": dict(error) if error is not None else None,
    }


def _parse_function_argument(value: Any) -> FunctionSelector:
    """Interpret only a 0x address as an address; all else is an exact name."""

    if not isinstance(value, str) or not value:
        raise CommandError(
            "invalid_function",
            "FUNCTION must be a non-empty 0x address or exact function name",
        )
    if value[:2].lower() != "0x":
        return FunctionSelector(name=value)
    try:
        start = int(value, 16)
    except ValueError as error:
        raise CommandError(
            "invalid_function",
            "FUNCTION 0x address is not a valid non-negative integer",
        ) from error
    if start < 0:
        raise CommandError(
            "invalid_function",
            "FUNCTION 0x address must be non-negative",
        )
    return FunctionSelector(start=start)


def _format_start(value: int) -> str:
    return f"0x{value:x}"


def _function_summary(function: Any) -> dict[str, Any]:
    return {
        "start": _format_start(int(function.start)),
        "name": str(function.name),
    }


def _resolve_exact_function(view: Any, selector: FunctionSelector) -> Any:
    """Fail closed on missing or ambiguous function identities."""

    functions = list(view.functions)
    if selector.start is not None:
        matches = [
            function for function in functions if int(function.start) == selector.start
        ]
        description = _format_start(selector.start)
    else:
        assert selector.name is not None
        matches = [
            function for function in functions if str(function.name) == selector.name
        ]
        description = selector.name
    if not matches:
        raise CommandError(
            "function_not_found", f"no exact function matches {description!r}"
        )
    if len(matches) != 1:
        raise CommandError(
            "function_ambiguous", f"multiple functions exactly match {description!r}"
        )
    return matches[0]


def _machine_function_start(record: Mapping[str, Any]) -> int | None:
    """Return a machine record's strict numeric function identity."""

    value = record.get("function_start")
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        return None
    return value


def _load_binary_ninja_api() -> SimpleNamespace:
    # Keep imports here: --help and external argument handling must not need
    # Binary Ninja installed or running.
    from binaryninja import PluginCommand, PluginCommandContext
    from binaryninja import _binaryninjacore as core

    return SimpleNamespace(
        PluginCommand=PluginCommand,
        PluginCommandContext=PluginCommandContext,
        core=core,
    )


def _decode_log_message(value: Any) -> str:
    if isinstance(value, str):
        return value
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace")
    if value is None:
        return ""
    if isinstance(value, (ctypes.c_char_p, ctypes.c_void_p)):
        try:
            pointer = ctypes.cast(value, ctypes.c_char_p)
            raw = pointer.value
            if raw is not None:
                return raw.decode("utf-8", errors="replace")
        except (OSError, TypeError, ValueError):
            pass
    return str(value)


class _LogCapture(AbstractContextManager["_LogCapture"]):
    """Capture only the selected synchronous menu command's host logs."""

    def __init__(self, core: Any):
        self._core = core
        self.messages: list[str] = []
        self._listener: Any | None = None
        self._callbacks: list[Any] = []
        self._registered = False

    def _append_message(
        self,
        _context: Any,
        _session: Any,
        _level: Any,
        message: Any,
        _logger: Any,
        _tid: Any,
    ) -> None:
        self.messages.append(_decode_log_message(message))

    def _append_stack_message(
        self,
        _context: Any,
        _session: Any,
        _level: Any,
        stack: Any,
        message: Any,
        _logger: Any,
        _tid: Any,
    ) -> None:
        decoded_message = _decode_log_message(message)
        decoded_stack = _decode_log_message(stack)
        self.messages.append(decoded_message)
        if decoded_stack and decoded_stack != decoded_message:
            self.messages.append(decoded_stack)

    @staticmethod
    def _close(_context: Any) -> None:
        return None

    def _minimum_log_level(self, _context: Any) -> int:
        level = getattr(getattr(self._core, "LogLevel", None), "DebugLog", 0)
        return int(getattr(level, "value", level))

    def __enter__(self) -> "_LogCapture":
        listener = self._core.BNLogListener()
        callback_types = dict(type(listener)._fields_)
        self._callbacks = [
            callback_types["log"](self._append_message),
            callback_types["logWithStackTrace"](self._append_stack_message),
            callback_types["close"](self._close),
            callback_types["getLogLevel"](self._minimum_log_level),
        ]
        listener.context = None
        listener.log = self._callbacks[0]
        listener.logWithStackTrace = self._callbacks[1]
        listener.close = self._callbacks[2]
        listener.getLogLevel = self._callbacks[3]
        self._listener = listener
        self._core.BNRegisterLogListener(ctypes.byref(listener))
        self._registered = True
        update = getattr(self._core, "BNUpdateLogListeners", None)
        if update is not None:
            update()
        return self

    def __exit__(self, exc_type: Any, exc: Any, traceback: Any) -> bool:
        if self._registered and self._listener is not None:
            self._core.BNUnregisterLogListener(ctypes.byref(self._listener))
            update = getattr(self._core, "BNUpdateLogListeners", None)
            if update is not None:
                update()
        self._registered = False
        return False


def parse_machine_records(lines: Iterable[str]) -> list[dict[str, Any]]:
    """Extract compact native protocol objects while preserving host logs."""

    records: list[dict[str, Any]] = []
    for line in lines:
        text = _decode_log_message(line)
        if not text.startswith(MACHINE_RESULT_PREFIX):
            continue
        try:
            parsed = json.loads(text[len(MACHINE_RESULT_PREFIX) :].strip())
        except json.JSONDecodeError:
            continue
        if isinstance(parsed, dict):
            records.append(parsed)
    return records


def _find_exact_command(plugin_command_type: Any, name: str) -> Any:
    # This deliberately avoids PluginCommand.get_valid_list: affected Binary
    # Ninja versions access context.project before the exact function command.
    try:
        matches = [
            command
            for command in plugin_command_type
            if getattr(command, "name", None) == name
        ]
    except Exception as error:
        raise CommandError(
            "command_enumeration_failed",
            "could not enumerate registered plugin commands: "
            f"{type(error).__name__}: {error}",
        ) from error
    if not matches:
        raise CommandError(
            "command_not_found",
            f"required SMBA menu command is not registered: {name}",
        )
    if len(matches) != 1:
        raise CommandError(
            "command_ambiguous",
            f"multiple plugin commands share the exact name: {name}",
        )
    return matches[0]


def _make_function_context(api: Any, view: Any, function: Any) -> Any:
    # Constructing PluginCommandContext(view) accesses view.project in the
    # affected bindings. Only function-command fields are required here.
    context = api.PluginCommandContext(None)
    context.view = view
    context.function = function
    context.address = int(function.start)
    context.length = 0
    return context


def _invoke_function_command(
    api: Any,
    view: Any,
    function: Any,
    command_name: str,
    expected_native_operation: str,
    capture_factory: Any,
) -> tuple[list[str], dict[str, Any]]:
    """Call one known menu command and return its selected machine record."""

    command = _find_exact_command(api.PluginCommand, command_name)
    context = _make_function_context(api, view, function)
    validator = getattr(command, "is_valid", None)
    if not callable(validator):
        raise CommandError(
            "command_validation_unavailable",
            f"{command_name} does not expose a validity check",
        )
    try:
        valid = bool(validator(context))
    except Exception as error:
        raise CommandError(
            "command_validation_exception",
            f"{command_name}.is_valid raised {type(error).__name__}: {error}",
            {"command": command_name, "function_start": int(function.start)},
        ) from error
    if not valid:
        raise CommandError(
            "command_not_valid",
            f"{command_name} is not valid for the selected function",
            {
                "command": command_name,
                "function_start": int(function.start),
                "valid": False,
            },
        )

    capture: Any | None = None
    try:
        with capture_factory(api.core) as active_capture:
            capture = active_capture
            command.execute(context)
    except Exception as error:
        logs = [] if capture is None else list(capture.messages)
        raise CommandError(
            "command_exception",
            f"{command_name} raised {type(error).__name__}: {error}",
            {
                "command": command_name,
                "exception_type": type(error).__name__,
                "exception": str(error),
            },
            logs,
        ) from error

    logs = list(capture.messages)
    for line in logs:
        if "Unhandled Python exception" in _decode_log_message(line):
            raise CommandError(
                "command_exception",
                f"{command_name} reported an unhandled Python callback exception",
                {"command": command_name, "exception": _decode_log_message(line)},
                logs,
            )

    operation_records = [
        record
        for record in parse_machine_records(logs)
        if record.get("operation") == expected_native_operation
    ]
    expected_function_start = int(function.start)
    matching = [
        record
        for record in operation_records
        if _machine_function_start(record) == expected_function_start
    ]
    if not matching:
        if operation_records:
            observed = [record.get("function_start") for record in operation_records]
            raise CommandError(
                "machine_result_function_mismatch",
                f"{command_name} emitted records for a different or invalid function",
                {
                    "expected_function_start": expected_function_start,
                    "observed_function_starts": observed,
                },
                logs,
            )
        raise CommandError(
            "machine_result_missing",
            f"{command_name} did not emit its machine result",
            logs=logs,
        )
    return logs, matching[-1]


def _failure_response(
    operation: str,
    error: CommandError,
    *,
    function: Mapping[str, Any] | None = None,
    command: str | None = None,
) -> dict[str, Any]:
    return _response(
        operation,
        function=function,
        command=command,
        result=error.result,
        logs=error.logs,
        error=_error(error.code, error.message),
    )


def run_preview(
    function_argument: str,
    view: Any,
    _api: Any | None = None,
    _capture_factory: Any | None = None,
) -> dict[str, Any]:
    """Run only the registered read-only Preview menu command in Binary Ninja."""

    function_summary: Mapping[str, Any] | None = None
    try:
        selector = _parse_function_argument(function_argument)
        api = _api if _api is not None else _load_binary_ninja_api()
        capture_factory = _capture_factory if _capture_factory is not None else _LogCapture
        function = _resolve_exact_function(view, selector)
        function_summary = _function_summary(function)
        logs, machine_result = _invoke_function_command(
            api,
            view,
            function,
            PREVIEW_COMMAND_NAME,
            PREVIEW_OPERATION,
            capture_factory,
        )
        return _response(
            PREVIEW_OPERATION,
            ok=True,
            accepted=True,
            modified=False,
            function=function_summary,
            command=PREVIEW_COMMAND_NAME,
            result=machine_result,
            logs=logs,
        )
    except CommandError as error:
        return _failure_response(
            PREVIEW_OPERATION,
            error,
            function=function_summary,
            command=PREVIEW_COMMAND_NAME if function_summary is not None else None,
        )
    except Exception as error:
        return _response(
            PREVIEW_OPERATION,
            function=function_summary,
            command=PREVIEW_COMMAND_NAME if function_summary is not None else None,
            error=_error("runtime_error", f"{type(error).__name__}: {error}"),
        )


def run_register_workflow(
    function_argument: str,
    view: Any,
    _api: Any | None = None,
    _capture_factory: Any | None = None,
) -> dict[str, Any]:
    """Run only the Register/refresh menu; never select or analyze a workflow."""

    function_summary: Mapping[str, Any] | None = None
    try:
        selector = _parse_function_argument(function_argument)
        api = _api if _api is not None else _load_binary_ninja_api()
        capture_factory = _capture_factory if _capture_factory is not None else _LogCapture
        function = _resolve_exact_function(view, selector)
        function_summary = _function_summary(function)
        logs, machine_result = _invoke_function_command(
            api,
            view,
            function,
            REGISTER_WORKFLOW_COMMAND_NAME,
            NATIVE_REGISTER_WORKFLOW_OPERATION,
            capture_factory,
        )
        accepted = machine_result.get("accepted") is True
        action = machine_result.get("action")
        if not accepted or action not in {"created", "refreshed"}:
            reason = machine_result.get("reason")
            message = (
                reason
                if isinstance(reason, str) and reason
                else "SMBA register/refresh menu command refused"
            )
            return _response(
                REGISTER_WORKFLOW_OPERATION,
                function=function_summary,
                command=REGISTER_WORKFLOW_COMMAND_NAME,
                result=machine_result,
                logs=logs,
                error=_error("command_refused", message),
            )
        return _response(
            REGISTER_WORKFLOW_OPERATION,
            ok=True,
            accepted=True,
            modified=False,
            function=function_summary,
            command=REGISTER_WORKFLOW_COMMAND_NAME,
            result=machine_result,
            logs=logs,
        )
    except CommandError as error:
        return _failure_response(
            REGISTER_WORKFLOW_OPERATION,
            error,
            function=function_summary,
            command=(
                REGISTER_WORKFLOW_COMMAND_NAME
                if function_summary is not None
                else None
            ),
        )
    except Exception as error:
        return _response(
            REGISTER_WORKFLOW_OPERATION,
            function=function_summary,
            command=(
                REGISTER_WORKFLOW_COMMAND_NAME
                if function_summary is not None
                else None
            ),
            error=_error("runtime_error", f"{type(error).__name__}: {error}"),
        )


def _external_program(
    source_path: pathlib.Path, entry_point: str, function_argument: str
) -> str:
    """Produce a fixed in-process call with no shell interpolation."""

    if entry_point not in {"run_preview", "run_register_workflow"}:
        raise ValueError(f"unsupported dedicated entry point: {entry_point}")
    return "\n".join(
        [
            "import runpy as _smba_runpy",
            f"_smba_namespace = _smba_runpy.run_path({str(source_path)!r})",
            f"result = _smba_namespace[{entry_point!r}]({function_argument!r}, bv)",
        ]
    )


def _invoke_external(
    entry_point: str,
    operation: str,
    function_argument: str,
    target: str,
    *,
    runner: Any = None,
) -> dict[str, Any]:
    """Use the external BN transport for one statically selected entry point."""

    try:
        _parse_function_argument(function_argument)
        if not isinstance(target, str) or not target:
            raise CommandError(
                "invalid_target",
                "--target must be a non-empty Binary Ninja target selector",
            )
    except CommandError as error:
        return _failure_response(operation, error)

    command = [
        "bn",
        "py",
        "exec",
        "--target",
        target,
        "--format",
        "json",
        "--code",
        _external_program(pathlib.Path(__file__).resolve(), entry_point, function_argument),
    ]
    try:
        completed = (runner or subprocess.run)(
            command,
            check=False,
            capture_output=True,
            text=True,
        )
    except OSError as error:
        return _response(
            operation,
            result={"target": target},
            error=_error("transport_unavailable", f"could not start bn: {error}"),
        )
    if completed.returncode != 0:
        return _response(
            operation,
            result={
                "target": target,
                "returncode": completed.returncode,
                "stderr": (completed.stderr or "").strip(),
            },
            error=_error("transport_failed", "bn py exec returned a non-zero status"),
        )
    try:
        envelope = json.loads(completed.stdout)
    except (TypeError, json.JSONDecodeError) as error:
        return _response(
            operation,
            result={"target": target, "stdout": (completed.stdout or "").strip()},
            error=_error(
                "transport_invalid_json",
                f"bn py exec did not return JSON: {error}",
            ),
        )
    response = envelope.get("result") if isinstance(envelope, Mapping) else None
    required = {
        "plugin",
        "operation",
        "ok",
        "accepted",
        "modified",
        "function",
        "command",
        "result",
        "logs",
        "error",
    }
    if (
        not isinstance(response, Mapping)
        or response.get("plugin") != PLUGIN_NAME
        or response.get("operation") != operation
        or not required.issubset(response)
    ):
        return _response(
            operation,
            result={"target": target},
            error=_error(
                "transport_invalid_response",
                "bn py exec response does not satisfy the dedicated SMBA contract",
            ),
        )
    return dict(response)


def invoke_external_preview(
    function_argument: str, target: str, *, runner: Any = None
) -> dict[str, Any]:
    """Run the dedicated Preview transport path."""

    return _invoke_external(
        "run_preview",
        PREVIEW_OPERATION,
        function_argument,
        target,
        runner=runner,
    )


def invoke_external_register_workflow(
    function_argument: str, target: str, *, runner: Any = None
) -> dict[str, Any]:
    """Run the dedicated Register/refresh transport path."""

    return _invoke_external(
        "run_register_workflow",
        REGISTER_WORKFLOW_OPERATION,
        function_argument,
        target,
        runner=runner,
    )


class _ArgumentParser(argparse.ArgumentParser):
    """Keep command errors machine-readable while retaining argparse help."""

    def error(self, message: str) -> None:
        raise CommandError("invalid_cli_arguments", message)


def _build_parser() -> _ArgumentParser:
    parser = _ArgumentParser(
        description=(
            "Run exactly one SMBA CoBRA Binary Ninja menu command for one "
            "function. Requires Python 3.11 or newer. Output is one JSON object. "
            "Each command does not save a BNDB."
        ),
        epilog=(
            "Examples:\n"
            "  ai_cli.py --target active preview 0x51e970\n"
            "  ai_cli.py --target active register-workflow target_function"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--target",
        required=True,
        metavar="TARGET",
        help="already-open Binary Ninja target selector, for example active",
    )
    commands = parser.add_subparsers(
        dest="subcommand",
        required=True,
        title="commands",
        metavar="{preview,register-workflow}",
        parser_class=_ArgumentParser,
    )
    function_help = "0x address or exact function name (never a substring)"
    commands.add_parser(
        PREVIEW_OPERATION,
        help="run the read-only Preview menu; does not modify analysis or save",
        description=(
            "Run the exact SMBA CoBRA Preview menu command. It does not modify "
            "analysis and does not save a BNDB. Requires Python 3.11 or newer.\n\n"
            "Example: ai_cli.py --target active preview 0x51e970"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    ).add_argument("function", metavar="FUNCTION", help=function_help)
    commands.add_parser(
        REGISTER_WORKFLOW_OPERATION,
        help="register/refresh a workflow; does not select, reanalyze, or save",
        description=(
            "Run the exact SMBA CoBRA Register or refresh current .mba workflow "
            "menu command. It may register or refresh a workflow, but does not "
            "select a workflow, does not reanalyze, and does not save a BNDB. "
            "Requires Python 3.11 or newer.\n\n"
            "Example: ai_cli.py --target active register-workflow target_function"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    ).add_argument("function", metavar="FUNCTION", help=function_help)
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = _build_parser()
    try:
        arguments = parser.parse_args(argv)
        if arguments.subcommand == PREVIEW_OPERATION:
            response = invoke_external_preview(arguments.function, arguments.target)
        else:
            assert arguments.subcommand == REGISTER_WORKFLOW_OPERATION
            response = invoke_external_register_workflow(
                arguments.function, arguments.target
            )
    except CommandError as error:
        response = _failure_response("invalid", error)
    print(json.dumps(response, sort_keys=True, separators=(",", ":")))
    return 0 if response["ok"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
