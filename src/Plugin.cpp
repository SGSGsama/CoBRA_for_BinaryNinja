#include "smba/PluginAPI.h"

#include "binaryninjaapi.h"

#include <string>

using namespace BinaryNinja;

namespace {

constexpr const char* kCompatibilityBaseWorkflow = "extension.smba.cobra.base";
constexpr const char* kCoreMetaAnalysisWorkflow = "core.function.metaAnalysis";

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
        "{\"title\":\"" + std::string(kCompatibilityBaseWorkflow)
        + "\",\"description\":\"Opt-in clone of " + kCoreMetaAnalysisWorkflow
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
}

void RunPreviewCommand(BinaryView*, Function* function) {
    if (!function) {
        return;
    }
    auto mlil = function->GetMediumLevelIL();
    if (!mlil) {
        LogWarn("[SMBA] function at 0x%llx has no MLIL", static_cast<unsigned long long>(function->GetStart()));
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
        return;
    }

    const std::string currentName = current->GetName();
    // A selected .mba workflow must never be derived again.  It can be
    // refreshed only when its exact Activity object was registered by this
    // plugin process; AddActivityToWorkflow verifies that identity and
    // refreshes the shared dispatch state.
    if (currentName.ends_with(".mba")) {
        if (!current->Contains(smba::kActivityName)) {
            LogError(
                "[SMBA] refusing current MBA workflow %s: it lacks %s; will not append another .mba",
                currentName.c_str(),
                smba::kActivityName
            );
            return;
        }
        if (!smba::AddActivityToWorkflow(current)) {
            LogError(
                "[SMBA] refusing current MBA workflow %s: %s is foreign or unowned; will not append another .mba",
                currentName.c_str(),
                smba::kActivityName
            );
            return;
        }
        LogInfo("[SMBA] refreshed current MBA workflow %s", currentName.c_str());
        return;
    }

    const std::string targetName = currentName + ".mba";
    if (auto existing = Workflow::Get(targetName)) {
        if (!existing->Contains(smba::kActivityName)) {
            LogError(
                "[SMBA] refusing to modify existing %s: it lacks the owned MBA activity",
                targetName.c_str()
            );
            return;
        }
        if (!smba::AddActivityToWorkflow(existing)) {
            LogError(
                "[SMBA] refusing to refresh %s: same-name MBA activity is foreign or unowned",
                targetName.c_str()
            );
            return;
        }
        LogInfo("[SMBA] refreshed registered MBA workflow %s", targetName.c_str());
        return;
    }

    auto derived = current->Clone(targetName);
    if (!derived || !smba::AddActivityToWorkflow(derived)) {
        LogError(
            "[SMBA] failed to compose %s after MLIL generation; source topology already contains an unowned MBA activity or has no MLIL anchor",
            targetName.c_str()
        );
        return;
    }
    const std::string description =
        "{\"title\":\"" + targetName
        + "\",\"description\":\"Explicit derivative of " + currentName
        + " with verified CoBRA MBA simplification.\"}";
    if (!Workflow::RegisterWorkflow(derived, description)) {
        LogError("[SMBA] failed to register composed workflow %s", targetName.c_str());
        return;
    }
    LogInfo(
        "[SMBA] registered %s; select it explicitly for functions/binaries that already use %s",
        targetName.c_str(),
        currentName.c_str()
    );
}

} // namespace

extern "C" {

BN_DECLARE_CORE_ABI_VERSION

BINARYNINJAPLUGIN bool CorePluginInit() {
    PluginCommand::RegisterForFunction(
        "SMBA CoBRA\\Preview verified MBA simplifications",
        "Analyze the current function and log verified MLIL MBA simplifications without changing IL.",
        RunPreviewCommand,
        HasMLIL
    );
    PluginCommand::RegisterForFunction(
        "SMBA CoBRA\\Register or refresh current .mba workflow",
        "Register the current workflow plus .mba, or refresh its owned MBA activity without changing workflow selection.",
        [](BinaryView*, Function* function) { RegisterOrRefreshWorkflow(function); },
        [](BinaryView*, Function* function) { return function && function->GetWorkflow(); }
    );

    RegisterCompatibilityBaseWorkflow();
    LogInfo("[SMBA] CoBRA MBA preview and register-or-refresh commands registered");
    return true;
}

} // extern "C"
