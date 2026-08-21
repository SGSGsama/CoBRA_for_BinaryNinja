#pragma once

#include "cobra/core/Expr.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace smba {

struct SimplifyOptions {
    uint32_t bitWidth = 64;
    uint32_t maxVariables = 16;
    uint32_t fullWidthProbes = 64;
    uint32_t z3TimeoutMs = 750;
    bool requireZ3 = true;
};

enum class SimplifyStatus {
    Simplified,
    Probabilistic,
    Unchanged,
    Rejected,
    Error,
};

struct SimplifyResult {
    SimplifyStatus status = SimplifyStatus::Error;
    std::unique_ptr<cobra::Expr> expression;
    std::vector<std::string> variables;
    std::string originalText;
    std::string simplifiedText;
    std::string reason;
    bool fullWidthVerified = false;
    bool z3Verified = false;
    uint32_t originalCost = 0;
    uint32_t simplifiedCost = 0;
};

// The predicate is evaluated over the exact modular bitvector domain selected
// by ProveConstantComparison's bitWidth argument.  The unsigned and signed
// variants therefore differ only in their ordering semantics, never in the
// representation of either expression.
enum class ComparisonKind {
    Equal,
    NotEqual,
    UnsignedLessThan,
    UnsignedLessEqual,
    UnsignedGreaterEqual,
    UnsignedGreaterThan,
    SignedLessThan,
    SignedLessEqual,
    SignedGreaterEqual,
    SignedGreaterThan,
};

enum class PredicateStatus {
    ProvedConstant,
    NotConstant,
    Rejected,
    Error,
};

struct PredicateResult {
    PredicateStatus status = PredicateStatus::Error;
    // Meaningful only when status is ProvedConstant.
    bool constantValue = false;
    std::string reason;
    bool z3Verified = false;
    bool timedOut = false;
    // For a non-constant predicate this contains concrete satisfying input
    // assignments for both truth values when Z3 can provide them.
    std::optional<std::string> counterexample;
};

// Pure CoBRA boundary. It has no Binary Ninja dependency and is shared by
// offline tests, plugin commands, and Workflow activities.
SimplifyResult SimplifyExpression(
    const cobra::Expr& original,
    const std::vector<std::string>& variables,
    const SimplifyOptions& options
);

// Prove whether a comparison of two CoBRA expression trees has one boolean
// value for every assignment to the shared variables.  bitWidth controls the
// complete bitvector semantics; SimplifyOptions supplies the variable limit,
// Z3 timeout, and existing policy settings.  This API never accepts
// probabilistic sampling as proof.
PredicateResult ProveConstantComparison(
    const cobra::Expr& left,
    const cobra::Expr& right,
    const std::vector<std::string>& variables,
    uint32_t bitWidth,
    ComparisonKind comparison,
    const SimplifyOptions& options
);

} // namespace smba
