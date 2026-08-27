#include "smba/PluginAPI.h"
#include "smba/PluginJson.h"

#include "binaryninjaapi.h"

#include <optional>
#include <string>

using namespace BinaryNinja;

namespace {

constexpr const char* kCompatibilityBaseWorkflow = "extension.smba.cobra.base";
constexpr const char* kCoreMetaAnalysisWorkflow = "core.function.metaAnalysis";

void LogMachineResult(const std::string& result) {
    LogInfo("%s%s", smba::kMachineResultLogPrefix, result.c_str());
}

void LogRegistrationResult(const smba::RegistrationMachineResult& result) {
    LogMachineResult(smba::FormatRegistrationMachineResult(result));
}

smba::RegistrationMachineResult RefusedRegistration(
    uint64_t functionStart,
    std::string workflowBefore,
    std::string target,
    std::string reason
) {
    smba::RegistrationMachineResult result;
    result.accepted = false;
    result.action = "refused";
    result.functionStart = functionStart;
    result.workflowBefore = std::move(workflowBefore);
    result.workflowAfter = std::nullopt;
    result.target = std::move(target);
    result.activity = smba::kActivityName;
    result.reason = std::move(reason);
    return result;
}

smba::RegistrationMachineResult AcceptedRegistration(
    uint64_t functionStart,
    std::string action,
    std::string workflowBefore,
    std::string workflowAfter
) {
    smba::RegistrationMachineResult result;
    result.accepted = true;
    result.action = std::move(action);
    result.functionStart = functionStart;
    result.workflowBefore = std::move(workflowBefore);
    result.target = workflowAfter;
    result.workflowAfter = std::move(workflowAfter);
    result.activity = smba::kActivityName;
    return result;
}

void RegisterCompatibilityBaseWorkflow() {
    // This name is an opt-in compatibility surface.  Another plugin or an
    // existing BNDB may already provide it, in which case this plugin must
    // neither replace its topology nor claim ownership of its Activity.
    if (Workflow::Get(kCompatibilityBaseWorkflow)) {
        LogInfo(
            "[SMBA] leaving existing compatibility workflow %s untouched",
            kCompatibilityBaseWorkflow
        );
        return;
    }

    auto source = Workflow::Get(kCoreMetaAnalysisWorkflow);
    if (!source) {
        LogError(
            "[SMBA] cannot register compatibility workflow %s: source %s is unavailable",
            kCompatibilityBaseWorkflow,
            kCoreMetaAnalysisWorkflow
        );
        return;
    }

    auto derived = source->Clone(kCompatibilityBaseWorkflow);
    if (!derived) {
        LogError(
            "[SMBA] cannot clone %s for compatibility workflow %s",
            kCoreMetaAnalysisWorkflow,
            kCompatibilityBaseWorkflow
        );
        return;
    }
    if (!smba::AddActivityToWorkflow(derived)) {
        LogError(
            "[SMBA] cannot compose owned MBA activity into compatibility workflow %s",
            kCompatibilityBaseWorkflow
        );
        return;
    }

    const std::string description =
        "{\"title\":\"" + smba::EscapeJsonString(kCompatibilityBaseWorkflow)
        + "\",\"description\":\"Opt-in clone of "
        + smba::EscapeJsonString(kCoreMetaAnalysisWorkflow)
        + " with verified CoBRA MBA simplification.\"}";
    if (!Workflow::RegisterWorkflow(derived, description)) {
        LogError(
            "[SMBA] failed to register compatibility workflow %s",
            kCompatibilityBaseWorkflow
        );
        return;
    }
    LogInfo(
        "[SMBA] registered opt-in compatibility workflow %s from %s without selecting it",
        kCompatibilityBaseWorkflow,
        kCoreMetaAnalysisWorkflow
    );
}

void LogReport(Function* function, const smba::FunctionReport& report) {
    LogInfo(
        "[SMBA] preview 0x%llx: %zu accepted",
        static_cast<unsigned long long>(function->GetStart()),
        report.accepted
    );
    if (!report.diagnostic.empty()) {
        LogWarn("[SMBA]   %s", report.diagnostic.c_str());
    }
    for (const auto& candidate : report.candidates) {
        if (!candidate.accepted && !candidate.applied) {
            continue;
        }
        LogInfo(
            "[SMBA]   0x%llx MLIL expr %zu: %s -> %s (%s)",
            static_cast<unsigned long long>(candidate.address),
            candidate.expressionIndex,
            candidate.before.c_str(),
            candidate.after.c_str(),
            candidate.reason.c_str()
        );
    }

    smba::PreviewMachineResult machine;
    machine.accepted = report.accepted;
    machine.applied = report.applied;
    machine.diagnostic = report.diagnostic;
    machine.functionStart = function->GetStart();
    machine.candidates.reserve(report.candidates.size());
    for (const auto& candidate : report.candidates) {
        machine.candidates.push_back({
            .address = candidate.address,
            .expressionIndex = candidate.expressionIndex,
            .before = candidate.before,
            .after = candidate.after,
            .reason = candidate.reason,
            .accepted = candidate.accepted,
            .applied = candidate.applied,
        });
    }
    LogMachineResult(smba::FormatPreviewMachineResult(machine));
}

void RunPreviewCommand(BinaryView*, Function* function) {
    if (!function) {
        return;
    }
    auto mlil = function->GetMediumLevelIL();
    if (!mlil) {
        LogWarn("[SMBA] function at 0x%llx has no MLIL", static_cast<unsigned long long>(function->GetStart()));
        smba::PreviewMachineResult machine;
        machine.diagnostic = "MLIL is unavailable";
        machine.functionStart = function->GetStart();
        LogMachineResult(smba::FormatPreviewMachineResult(machine));
        return;
    }
    const auto report = smba::PreviewFunction(mlil);
    LogReport(function, report);
}

bool HasMLIL(BinaryView*, Function* function) {
    return function && function->GetMediumLevelILIfAvailable();
}

void RegisterOrRefreshWorkflow(Function* function) {
    if (!function) {
        return;
    }
    auto current = function->GetWorkflow();
    if (!current) {
        LogError("[SMBA] current function has no workflow");
        LogRegistrationResult(RefusedRegistration(
            function->GetStart(),
            "",
            "",
            "current function has no workflow"
        ));
        return;
    }

    const std::string currentName = current->GetName();
    // Activity presence, not a workflow-name suffix, owns the lifecycle
    // decision.  A clone can preserve our exact Activity while another
    // plugin composes a name such as ``base.mba.dualbr``.  Conversely, a name
    // ending in .mba does not prove that it carries this process's callback.
    if (current->Contains(smba::kActivityName)) {
        if (!smba::AddActivityToWorkflow(current)) {
            LogError(
                "[SMBA] refusing current workflow %s: %s is foreign or unowned",
                currentName.c_str(),
                smba::kActivityName
            );
            LogRegistrationResult(RefusedRegistration(
                function->GetStart(),
                currentName,
                currentName,
                "current workflow contains a foreign or unowned MBA activity"
            ));
            return;
        }
        LogInfo("[SMBA] refreshed current owned MBA activity in workflow %s", currentName.c_str());
        LogRegistrationResult(AcceptedRegistration(
            function->GetStart(), "refreshed", currentName, currentName
        ));
        return;
    }

    const std::string targetName = currentName + ".mba";
    if (auto existing = Workflow::Get(targetName)) {
        if (!existing->Contains(smba::kActivityName)) {
            LogError(
                "[SMBA] refusing to modify existing %s: it lacks the owned MBA activity",
                targetName.c_str()
            );
            LogRegistrationResult(RefusedRegistration(
                function->GetStart(),
                currentName,
                targetName,
                "existing target workflow lacks the owned MBA activity"
            ));
            return;
        }
        if (!smba::AddActivityToWorkflow(existing)) {
            LogError(
                "[SMBA] refusing to refresh %s: same-name MBA activity is foreign or unowned",
                targetName.c_str()
            );
            LogRegistrationResult(RefusedRegistration(
                function->GetStart(),
                currentName,
                targetName,
                "target workflow MBA activity is foreign or unowned"
            ));
            return;
        }
        LogInfo("[SMBA] refreshed registered MBA workflow %s", targetName.c_str());
        LogRegistrationResult(AcceptedRegistration(
            function->GetStart(), "refreshed", currentName, targetName
        ));
        return;
    }

    auto derived = current->Clone(targetName);
    if (!derived || !smba::AddActivityToWorkflow(derived)) {
        LogError(
            "[SMBA] failed to compose %s after MLIL generation; source topology already contains an unowned MBA activity or has no MLIL anchor",
            targetName.c_str()
        );
        LogRegistrationResult(RefusedRegistration(
            function->GetStart(),
            currentName,
            targetName,
            "could not compose the owned MBA activity after the MLIL anchor"
        ));
        return;
    }
    const std::string description =
        "{\"title\":\"" + smba::EscapeJsonString(targetName)
        + "\",\"description\":\"Explicit derivative of " + smba::EscapeJsonString(currentName)
        + " with verified CoBRA MBA simplification.\"}";
    if (!Workflow::RegisterWorkflow(derived, description)) {
        LogError("[SMBA] failed to register composed workflow %s", targetName.c_str());
        LogRegistrationResult(RefusedRegistration(
            function->GetStart(),
            currentName,
            targetName,
            "Binary Ninja rejected workflow registration"
        ));
        return;
    }
    LogInfo(
        "[SMBA] registered %s; select it explicitly for functions/binaries that already use %s",
        targetName.c_str(),
        currentName.c_str()
    );
    LogRegistrationResult(AcceptedRegistration(
        function->GetStart(), "created", currentName, targetName
    ));
}

} // namespace

extern "C" {

BN_DECLARE_CORE_ABI_VERSION

BINARYNINJAPLUGIN bool CorePluginInit() {
    PluginCommand::RegisterForFunction(
        smba::kPreviewCommandName,
        "Analyze the current function and log verified MLIL MBA simplifications without changing IL.",
        RunPreviewCommand,
        HasMLIL
    );
    PluginCommand::RegisterForFunction(
        smba::kRegisterWorkflowCommandName,
        "Register the current workflow plus .mba, or refresh its owned MBA activity without changing workflow selection.",
        [](BinaryView*, Function* function) { RegisterOrRefreshWorkflow(function); },
        [](BinaryView*, Function* function) { return function && function->GetWorkflow(); }
    );

    RegisterCompatibilityBaseWorkflow();
    LogInfo("[SMBA] CoBRA MBA preview and register-or-refresh commands registered");
    return true;
}

} // extern "C"
