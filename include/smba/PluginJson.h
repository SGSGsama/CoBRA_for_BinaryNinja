#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace smba {

// This is intentionally part of the small, Binary Ninja-independent protocol
// boundary.  ai_cli.py consumes log messages with this exact prefix.
inline constexpr const char* kMachineResultLogPrefix = "[SMBA AI JSON]";

struct MachineCandidateReport {
    uint64_t address = 0;
    size_t expressionIndex = 0;
    std::string before;
    std::string after;
    std::string reason;
    bool accepted = false;
    bool applied = false;
};

struct PreviewMachineResult {
    size_t accepted = 0;
    size_t applied = 0;
    std::string diagnostic;
    uint64_t functionStart = 0;
    std::vector<MachineCandidateReport> candidates;
};

struct RegistrationMachineResult {
    bool accepted = false;
    // One of: created, refreshed, refused.
    std::string action;
    // Function identity lets the external bridge reject cross-talk records.
    uint64_t functionStart = 0;
    std::string workflowBefore;
    std::optional<std::string> workflowAfter;
    std::string target;
    std::string activity;
    std::optional<std::string> reason;
};

// Produces the contents of a JSON string literal, excluding the surrounding
// quotes. Valid UTF-8 is preserved; control characters and malformed bytes
// are escaped so the surrounding formatter always emits valid JSON.
std::string EscapeJsonString(std::string_view value);

// Compact, deterministic JSON payloads appended to kMachineResultLogPrefix.
std::string FormatPreviewMachineResult(const PreviewMachineResult& result);
std::string FormatRegistrationMachineResult(const RegistrationMachineResult& result);

} // namespace smba
