#include "smba/PluginJson.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "plugin_json_tests: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main() {
    using namespace smba;

    Expect(
        EscapeJsonString("quote \" slash \\ newline\n tab\t")
            == "quote \\\" slash \\\\ newline\\n tab\\t",
        "JSON escaping must preserve quotes, slashes, and controls"
    );
    const std::string malformedUtf8(1, static_cast<char>(0xff));
    Expect(
        EscapeJsonString(malformedUtf8) == "\\u00ff",
        "malformed bytes must be escaped to keep the machine record valid JSON"
    );

    PreviewMachineResult preview;
    preview.accepted = 1;
    preview.applied = 0;
    preview.diagnostic = "line \"one\"\n";
    preview.functionStart = 4096;
    preview.candidates.push_back({
        .address = 4112,
        .expressionIndex = 7,
        .before = "a\\b",
        .after = "x",
        .reason = "proved",
        .accepted = true,
        .applied = false,
    });
    const std::string expectedPreview =
        R"({"operation":"preview","accepted":1,"applied":0,"diagnostic":"line \"one\"\n","function_start":4096,"candidates":[{"address":4112,"expression_index":7,"before":"a\\b","after":"x","reason":"proved","accepted":true,"applied":false}]})";
    Expect(
        FormatPreviewMachineResult(preview) == expectedPreview,
        "preview machine JSON must be compact, ordered, and escaped"
    );

    RegistrationMachineResult registration;
    registration.accepted = false;
    registration.action = "refused";
    registration.functionStart = 0x401000;
    registration.workflowBefore = "foreign";
    registration.workflowAfter = std::nullopt;
    registration.target = "foreign.mba";
    registration.activity = "extension.\"owned";
    registration.reason = "foreign\nactivity";
    const std::string expectedRegistration =
        R"({"operation":"register_workflow","accepted":false,"action":"refused","function_start":4198400,"workflow_before":"foreign","workflow_after":null,"target":"foreign.mba","activity":"extension.\"owned","reason":"foreign\nactivity"})";
    Expect(
        FormatRegistrationMachineResult(registration) == expectedRegistration,
        "registration refusal JSON must retain its reason and null after-workflow"
    );

    RegistrationMachineResult created;
    created.accepted = true;
    created.action = "created";
    created.functionStart = 0x51eb10;
    created.workflowBefore = "base";
    created.workflowAfter = "base.mba";
    created.target = "base.mba";
    created.activity = "extension.smba.cobra.simplifyMlil";
    const std::string expectedCreated =
        R"({"operation":"register_workflow","accepted":true,"action":"created","function_start":5368592,"workflow_before":"base","workflow_after":"base.mba","target":"base.mba","activity":"extension.smba.cobra.simplifyMlil"})";
    Expect(
        FormatRegistrationMachineResult(created) == expectedCreated,
        "registration success JSON must name the returned owned workflow"
    );

    return 0;
}
