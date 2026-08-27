#include "smba/PluginJson.h"

#include <cstdint>
#include <string>

namespace smba {

namespace {

void AppendHexByte(std::string& output, uint8_t value) {
    constexpr char kHex[] = "0123456789abcdef";
    output += "\\u00";
    output += kHex[(value >> 4) & 0x0f];
    output += kHex[value & 0x0f];
}

bool IsContinuation(uint8_t value) {
    return (value & 0xc0) == 0x80;
}

// Return the number of bytes in a valid UTF-8 scalar at offset, or zero for
// malformed/truncated input. JSON permits UTF-8 directly, but not invalid
// byte sequences, so malformed bytes are escaped one by one below.
size_t ValidUtf8Length(std::string_view value, size_t offset) {
    const auto at = [&value](size_t index) {
        return static_cast<uint8_t>(value[index]);
    };
    const uint8_t first = at(offset);
    const size_t remaining = value.size() - offset;
    if (first >= 0xc2 && first <= 0xdf) {
        return remaining >= 2 && IsContinuation(at(offset + 1)) ? 2 : 0;
    }
    if (first == 0xe0) {
        return remaining >= 3 && at(offset + 1) >= 0xa0 && at(offset + 1) <= 0xbf
                && IsContinuation(at(offset + 2))
            ? 3
            : 0;
    }
    if ((first >= 0xe1 && first <= 0xec) || (first >= 0xee && first <= 0xef)) {
        return remaining >= 3 && IsContinuation(at(offset + 1)) && IsContinuation(at(offset + 2))
            ? 3
            : 0;
    }
    if (first == 0xed) {
        return remaining >= 3 && at(offset + 1) >= 0x80 && at(offset + 1) <= 0x9f
                && IsContinuation(at(offset + 2))
            ? 3
            : 0;
    }
    if (first == 0xf0) {
        return remaining >= 4 && at(offset + 1) >= 0x90 && at(offset + 1) <= 0xbf
                && IsContinuation(at(offset + 2)) && IsContinuation(at(offset + 3))
            ? 4
            : 0;
    }
    if (first >= 0xf1 && first <= 0xf3) {
        return remaining >= 4 && IsContinuation(at(offset + 1)) && IsContinuation(at(offset + 2))
                && IsContinuation(at(offset + 3))
            ? 4
            : 0;
    }
    if (first == 0xf4) {
        return remaining >= 4 && at(offset + 1) >= 0x80 && at(offset + 1) <= 0x8f
                && IsContinuation(at(offset + 2)) && IsContinuation(at(offset + 3))
            ? 4
            : 0;
    }
    return 0;
}

void AppendQuoted(std::string& output, std::string_view value) {
    output += '"';
    output += EscapeJsonString(value);
    output += '"';
}

void AppendBoolean(std::string& output, bool value) {
    output += value ? "true" : "false";
}

void AppendUnsigned(std::string& output, uint64_t value) {
    output += std::to_string(value);
}

} // namespace

std::string EscapeJsonString(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (size_t index = 0; index < value.size();) {
        const uint8_t byte = static_cast<uint8_t>(value[index]);
        switch (byte) {
        case '"':
            escaped += "\\\"";
            ++index;
            break;
        case '\\':
            escaped += "\\\\";
            ++index;
            break;
        case '\b':
            escaped += "\\b";
            ++index;
            break;
        case '\f':
            escaped += "\\f";
            ++index;
            break;
        case '\n':
            escaped += "\\n";
            ++index;
            break;
        case '\r':
            escaped += "\\r";
            ++index;
            break;
        case '\t':
            escaped += "\\t";
            ++index;
            break;
        default:
            if (byte < 0x20) {
                AppendHexByte(escaped, byte);
                ++index;
            } else if (byte < 0x80) {
                escaped += static_cast<char>(byte);
                ++index;
            } else if (const size_t utf8Length = ValidUtf8Length(value, index); utf8Length != 0) {
                escaped.append(value.substr(index, utf8Length));
                index += utf8Length;
            } else {
                AppendHexByte(escaped, byte);
                ++index;
            }
            break;
        }
    }
    return escaped;
}

std::string FormatPreviewMachineResult(const PreviewMachineResult& result) {
    std::string output;
    output.reserve(160 + result.candidates.size() * 120);
    output += "{\"operation\":\"preview\",\"accepted\":";
    AppendUnsigned(output, result.accepted);
    output += ",\"applied\":";
    AppendUnsigned(output, result.applied);
    output += ",\"diagnostic\":";
    AppendQuoted(output, result.diagnostic);
    output += ",\"function_start\":";
    AppendUnsigned(output, result.functionStart);
    output += ",\"candidates\":[";
    for (size_t index = 0; index < result.candidates.size(); ++index) {
        if (index != 0) {
            output += ',';
        }
        const auto& candidate = result.candidates[index];
        output += "{\"address\":";
        AppendUnsigned(output, candidate.address);
        output += ",\"expression_index\":";
        AppendUnsigned(output, candidate.expressionIndex);
        output += ",\"before\":";
        AppendQuoted(output, candidate.before);
        output += ",\"after\":";
        AppendQuoted(output, candidate.after);
        output += ",\"reason\":";
        AppendQuoted(output, candidate.reason);
        output += ",\"accepted\":";
        AppendBoolean(output, candidate.accepted);
        output += ",\"applied\":";
        AppendBoolean(output, candidate.applied);
        output += '}';
    }
    output += "]}";
    return output;
}

std::string FormatRegistrationMachineResult(const RegistrationMachineResult& result) {
    std::string output;
    output.reserve(180 + result.workflowBefore.size() + result.target.size() + result.activity.size());
    output += "{\"operation\":\"register_workflow\",\"accepted\":";
    AppendBoolean(output, result.accepted);
    output += ",\"action\":";
    AppendQuoted(output, result.action);
    output += ",\"function_start\":";
    AppendUnsigned(output, result.functionStart);
    output += ",\"workflow_before\":";
    AppendQuoted(output, result.workflowBefore);
    output += ",\"workflow_after\":";
    if (result.workflowAfter) {
        AppendQuoted(output, *result.workflowAfter);
    } else {
        output += "null";
    }
    output += ",\"target\":";
    AppendQuoted(output, result.target);
    output += ",\"activity\":";
    AppendQuoted(output, result.activity);
    if (result.reason || result.action == "refused") {
        output += ",\"reason\":";
        AppendQuoted(output, result.reason.value_or(""));
    }
    output += '}';
    return output;
}

} // namespace smba
