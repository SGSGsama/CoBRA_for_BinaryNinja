#pragma once

#include "binaryninjaapi.h"
#include "mediumlevelilinstruction.h"

#include <cstddef>
#include <string>
#include <vector>

namespace smba {

inline constexpr const char* kActivityName = "extension.smba.cobra.simplifyMlil";

struct AnalysisLimits {
    size_t maxNodes = 350;
    size_t maxDepth = 40;
    size_t minNodes = 7;
    size_t maxVariables = 16;
    size_t maxCandidateRoots = 128;
};

struct CandidateReport {
    size_t expressionIndex = 0;
    uint64_t address = 0;
    std::string before;
    std::string after;
    std::string reason;
    bool accepted = false;
    bool applied = false;
};

struct FunctionReport {
    std::vector<CandidateReport> candidates;
    std::string diagnostic;
    size_t accepted = 0;
    size_t applied = 0;
};

// Read-only preview API. This accepts normal (non-SSA) MLIL only and never
// mutates the supplied function. Automatic application is deliberately
// restricted to TransformFunction so all edits flow through AnalysisContext.
FunctionReport PreviewFunction(
    BinaryNinja::MediumLevelILFunction* function,
    const AnalysisLimits& limits = {}
);

// Workflow-only copy transformation. Every applied candidate must have a Z3
// proof; the old MLIL is copied into a new function and committed atomically
// through AnalysisContext::SetMediumLevelILFunction.
FunctionReport TransformFunction(
    BinaryNinja::Ref<BinaryNinja::AnalysisContext> context,
    const AnalysisLimits& limits = {}
);

// Register the shared activity in an unregistered workflow and place it after
// the requested anchor.  For an already registered workflow this performs an
// in-place refresh only when the existing Activity object is the exact object
// owned by this plugin process: registered workflow topology is immutable, so
// a missing or foreign same-name activity fails closed.  The refresh updates
// the Activity's thread-safe mutable dispatch state without registering a
// duplicate Activity or changing workflow selection/defaults.
bool AddActivityToWorkflow(
    BinaryNinja::Workflow* workflow,
    const std::string& anchor = "core.function.generateMediumLevelIL"
);

} // namespace smba
