from __future__ import annotations

import importlib.util
import io
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from types import SimpleNamespace


ROOT = pathlib.Path(__file__).resolve().parents[1]
CLI_PATH = ROOT / "scripts" / "ai_cli.py"


def load_module():
    name = "smba_ai_cli_test_module"
    sys.modules.pop(name, None)
    spec = importlib.util.spec_from_file_location(name, CLI_PATH)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


class FakeFunction:
    def __init__(self, start, name):
        self.start = start
        self.name = name
        self.reanalyze_calls = 0

    def reanalyze(self):
        self.reanalyze_calls += 1


class FakeView:
    def __init__(self, functions):
        self.functions = functions
        self.update_calls = 0

    def update_analysis_and_wait(self):
        self.update_calls += 1


class FakeContext:
    def __init__(self, view):
        if view is not None:
            raise AssertionError("context must be initialized without view.project access")
        self.view = None
        self.function = None
        self.address = 0
        self.length = 0


class FakeCapture:
    def __init__(self):
        self.messages = []

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, traceback):
        return False


class FakeCaptureFactory:
    def __init__(self):
        self.last = None

    def __call__(self, core):
        del core
        self.last = FakeCapture()
        return self.last


class FakeCommand:
    def __init__(self, name, emit=None, *, valid=True, error=None, valid_error=None):
        self.name = name
        self.emit = emit
        self.valid = valid
        self.error = error
        self.valid_error = valid_error
        self.valid_contexts = []
        self.contexts = []

    def is_valid(self, context):
        self.valid_contexts.append(context)
        if self.valid_error is not None:
            raise self.valid_error
        return self.valid

    def execute(self, context):
        self.contexts.append(context)
        if self.emit:
            self.emit()
        if self.error is not None:
            raise self.error


class FakeCompleted:
    def __init__(self, returncode, stdout="", stderr=""):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


class AiCliTests(unittest.TestCase):
    def setUp(self):
        self.cli = load_module()
        self.function = FakeFunction(0x401000, "target_function")
        self.view = FakeView([self.function])

    def api(self, commands):
        return SimpleNamespace(
            PluginCommand=commands,
            PluginCommandContext=FakeContext,
            core=object(),
        )

    @staticmethod
    def preview_record(start=0x401000, diagnostic=""):
        return (
            '[SMBA AI JSON]{"operation":"preview","accepted":0,"applied":0,'
            f'"diagnostic":{json.dumps(diagnostic)},"function_start":{start},"candidates":[]}}'
        )

    @staticmethod
    def register_record(*, accepted=True, action="created", start=0x401000, reason=None):
        record = {
            "operation": "register_workflow",
            "accepted": accepted,
            "action": action,
            "function_start": start,
            "workflow_before": "core.function.metaAnalysis",
            "workflow_after": "core.function.metaAnalysis.mba" if accepted else None,
            "target": "core.function.metaAnalysis.mba",
            "activity": "extension.smba.cobra.simplifyMlil",
        }
        if reason is not None:
            record["reason"] = reason
        return "[SMBA AI JSON]" + json.dumps(record, separators=(",", ":"))

    def test_preview_strictly_uses_the_preview_menu_command_and_selected_function(self):
        capture_factory = FakeCaptureFactory()
        wrong = FakeCommand("SMBA CoBRA\\Preview verified MBA simplifications (wrong)")
        expected_name = "SMBA CoBRA\\Preview verified MBA simplifications"

        def emit():
            capture_factory.last.messages.extend(
                ["[SMBA] preview human log", self.preview_record()]
            )

        expected = FakeCommand(expected_name, emit)
        response = self.cli.run_preview(
            "0x401000",
            self.view,
            _api=self.api([wrong, expected]),
            _capture_factory=capture_factory,
        )

        self.assertTrue(response["ok"])
        self.assertTrue(response["accepted"])
        self.assertFalse(response["modified"])
        self.assertEqual(response["operation"], "preview")
        self.assertEqual(response["command"], expected_name)
        self.assertEqual(len(expected.contexts), 1)
        self.assertEqual(len(wrong.contexts), 0)
        self.assertIs(expected.contexts[0].view, self.view)
        self.assertIs(expected.contexts[0].function, self.function)
        self.assertEqual(response["result"]["function_start"], self.function.start)

    def test_preview_checks_is_valid_before_execute(self):
        command = FakeCommand(
            "SMBA CoBRA\\Preview verified MBA simplifications", valid=False
        )
        response = self.cli.run_preview(
            "0x401000", self.view, _api=self.api([command])
        )
        self.assertFalse(response["ok"])
        self.assertEqual(response["error"]["code"], "command_not_valid")
        self.assertEqual(len(command.valid_contexts), 1)
        self.assertEqual(command.contexts, [])

    def test_exact_function_name_rejects_substrings(self):
        near_match = FakeFunction(0x402000, "target_function_extra")
        self.view.functions.append(near_match)
        capture_factory = FakeCaptureFactory()
        command = FakeCommand(
            "SMBA CoBRA\\Preview verified MBA simplifications",
            lambda: capture_factory.last.messages.append(self.preview_record()),
        )

        response = self.cli.run_preview(
            "target_function",
            self.view,
            _api=self.api([command]),
            _capture_factory=capture_factory,
        )

        self.assertTrue(response["ok"])
        self.assertIs(command.contexts[0].function, self.function)

    def test_function_start_cross_talk_fails_closed(self):
        capture_factory = FakeCaptureFactory()

        def emit():
            capture_factory.last.messages.append(
                self.preview_record(0x402000, "other-function")
            )

        response = self.cli.run_preview(
            "0x401000",
            self.view,
            _api=self.api([FakeCommand("SMBA CoBRA\\Preview verified MBA simplifications", emit)]),
            _capture_factory=capture_factory,
        )
        self.assertFalse(response["ok"])
        self.assertEqual(response["error"]["code"], "machine_result_function_mismatch")
        self.assertIn("other-function", response["logs"][0])

    def test_callback_failures_include_the_synchronous_log_window(self):
        capture_factory = FakeCaptureFactory()

        def emit():
            capture_factory.last.messages.append("before callback failure")

        response = self.cli.run_preview(
            "0x401000",
            self.view,
            _api=self.api(
                [FakeCommand("SMBA CoBRA\\Preview verified MBA simplifications", emit, error=RuntimeError("boom"))]
            ),
            _capture_factory=capture_factory,
        )
        self.assertFalse(response["ok"])
        self.assertEqual(response["error"]["code"], "command_exception")
        self.assertEqual(response["logs"], ["before callback failure"])

    def test_register_workflow_strictly_maps_the_menu_command_without_analysis_or_save(self):
        capture_factory = FakeCaptureFactory()
        wrong = FakeCommand("SMBA CoBRA\\Register or refresh current .mba workflow (wrong)")
        expected_name = "SMBA CoBRA\\Register or refresh current .mba workflow"
        expected = FakeCommand(
            expected_name,
            lambda: capture_factory.last.messages.append(self.register_record()),
        )

        response = self.cli.run_register_workflow(
            "0x401000",
            self.view,
            _api=self.api([wrong, expected]),
            _capture_factory=capture_factory,
        )

        self.assertTrue(response["ok"])
        self.assertTrue(response["accepted"])
        self.assertFalse(response["modified"])
        self.assertEqual(response["operation"], "register-workflow")
        self.assertEqual(response["command"], expected_name)
        self.assertEqual(len(expected.contexts), 1)
        self.assertEqual(len(wrong.contexts), 0)
        self.assertEqual(self.function.reanalyze_calls, 0)
        self.assertEqual(self.view.update_calls, 0)
        self.assertNotIn("saved", response["result"])

    def test_register_workflow_refusal_is_returned_without_follow_on_work(self):
        capture_factory = FakeCaptureFactory()
        command = FakeCommand(
            "SMBA CoBRA\\Register or refresh current .mba workflow",
            lambda: capture_factory.last.messages.append(
                self.register_record(accepted=False, action="refused", reason="foreign activity")
            ),
        )
        response = self.cli.run_register_workflow(
            "0x401000",
            self.view,
            _api=self.api([command]),
            _capture_factory=capture_factory,
        )
        self.assertFalse(response["ok"])
        self.assertEqual(response["error"]["code"], "command_refused")
        self.assertEqual(self.function.reanalyze_calls, 0)
        self.assertEqual(self.view.update_calls, 0)

    def test_machine_record_parser_ignores_human_and_malformed_lines(self):
        records = self.cli.parse_machine_records(
            [
                "[SMBA] human log",
                "[SMBA AI JSON]{not-json}",
                '[SMBA AI JSON]{"operation":"preview","accepted":1}',
            ]
        )
        self.assertEqual(records, [{"operation": "preview", "accepted": 1}])

    def test_external_preview_transport_uses_an_absolute_script_path_and_no_request_object(self):
        captured = {}
        expected = {
            "plugin": "smba-cobra-mba",
            "operation": "preview",
            "ok": True,
            "accepted": True,
            "modified": False,
            "function": {"start": "0x401000", "name": "target_function"},
            "command": "SMBA CoBRA\\Preview verified MBA simplifications",
            "result": {},
            "logs": [],
            "error": None,
        }

        def runner(argv, **kwargs):
            captured["argv"] = argv
            captured["kwargs"] = kwargs
            return FakeCompleted(0, stdout=json.dumps({"result": expected}))

        response = self.cli.invoke_external_preview(
            "0x401000", "active", runner=runner
        )

        self.assertTrue(response["ok"])
        self.assertEqual(captured["argv"][:7], ["bn", "py", "exec", "--target", "active", "--format", "json"])
        program = captured["argv"][-1]
        self.assertIn(str(CLI_PATH.resolve()), program)
        self.assertIn("run_preview", program)
        self.assertNotIn("request_json", program)

    def test_cli_uses_normal_arguments_and_never_prompts_for_json(self):
        captured = {}

        def fake_invoke(function_argument, target):
            captured["function_argument"] = function_argument
            captured["target"] = target
            return {
                "plugin": "smba-cobra-mba",
                "operation": "preview",
                "ok": True,
                "accepted": True,
                "modified": False,
                "function": None,
                "command": None,
                "result": {},
                "logs": [],
                "error": None,
            }

        self.cli.invoke_external_preview = fake_invoke
        output = io.StringIO()
        with redirect_stdout(output):
            exit_code = self.cli.main(
                ["--target", "fake-target", "preview", "0x401000"]
            )
        self.assertEqual(exit_code, 0)
        self.assertEqual(captured, {"function_argument": "0x401000", "target": "fake-target"})
        self.assertTrue(json.loads(output.getvalue())["ok"])

    def test_help_is_directory_independent_and_subcommands_explain_safety(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            for argv, phrases in (
                (["-h"], ["preview", "register-workflow", "does not save", "Python 3.11 or newer"]),
                (["preview", "-h"], ["does not modify analysis", "does not save", "Example", "Python 3.11 or newer"]),
                (["register-workflow", "-h"], ["does not select", "does not reanalyze", "does not save", "Python 3.11 or newer"]),
            ):
                completed = subprocess.run(
                    [sys.executable, str(CLI_PATH.resolve()), *argv],
                    cwd=temp_dir,
                    check=False,
                    capture_output=True,
                    text=True,
                )
                self.assertEqual(completed.returncode, 0, completed.stderr)
                for phrase in phrases:
                    self.assertIn(phrase, completed.stdout)

    def test_system_python_39_is_rejected_with_a_single_clear_version_error(self):
        system_python = pathlib.Path("/usr/bin/python3")
        if not system_python.is_file():
            self.skipTest("no system Python executable is available")
        version = subprocess.run(
            [str(system_python), "--version"],
            check=False,
            capture_output=True,
            text=True,
        )
        if not (version.stdout + version.stderr).startswith("Python 3.9."):
            self.skipTest("system Python is not 3.9")

        completed = subprocess.run(
            [str(system_python), str(CLI_PATH.resolve()), "-h"],
            check=False,
            capture_output=True,
            text=True,
        )

        self.assertNotEqual(completed.returncode, 0)
        self.assertEqual(completed.stdout, "")
        self.assertEqual(
            completed.stderr,
            "ai_cli.py requires Python 3.11 or newer\n",
        )
        self.assertNotIn("Traceback", completed.stderr)

    def test_legacy_request_arguments_are_rejected_by_argparse_without_starting_bn(self):
        completed = subprocess.run(
            [
                sys.executable,
                str(CLI_PATH.resolve()),
                "--target",
                "active",
                "--request-json",
                '{"operation":"preview"}',
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(completed.returncode, 2)
        self.assertNotIn("transport_", completed.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
