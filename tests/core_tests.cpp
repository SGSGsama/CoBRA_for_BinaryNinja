#include "smba/Engine.h"

#include "cobra/core/Expr.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct TestContext {
    int failures = 0;

    void Expect(bool condition, std::string_view message) {
        if (condition) {
            return;
        }
        ++failures;
        std::cerr << "[FAIL] " << message << '\n';
    }
};

smba::SimplifyOptions OfflineOptions() {
    smba::SimplifyOptions options;
    // The test suite must run without a Z3 install.  The production/plugin
    // path always requires a Z3 proof; this explicitly exercises the offline
    // differential-probe path only.
    options.requireZ3 = false;
    options.fullWidthProbes = 16;
    return options;
}

std::unique_ptr<cobra::Expr> XorSelf() {
    return cobra::Expr::BitwiseXor(
        cobra::Expr::Variable(0), cobra::Expr::Variable(0)
    );
}

void SimplifiableIdentity(TestContext& tests) {
    auto expression = XorSelf();
    const auto result = smba::SimplifyExpression(
        *expression, {"x"}, OfflineOptions()
    );

#ifdef SMBA_HAS_Z3
    tests.Expect(
        result.status == smba::SimplifyStatus::Simplified,
        "x ^ x should be accepted after a Z3 proof"
    );
    tests.Expect(
        result.z3Verified,
        "with Z3, x ^ x records a successful equivalence proof"
    );
#else
    tests.Expect(
        result.status == smba::SimplifyStatus::Probabilistic,
        "without Z3, x ^ x must be labelled probabilistic rather than proved"
    );
    tests.Expect(
        !result.z3Verified,
        "without Z3, x ^ x must not claim an equivalence proof"
    );
#endif
    tests.Expect(result.expression != nullptr, "simplified identity has an AST");
    tests.Expect(
        result.simplifiedText == "0",
        "x ^ x should render as the constant zero"
    );
    tests.Expect(
        result.fullWidthVerified,
        "offline simplification must pass its differential-probe gate"
    );
#ifndef SMBA_HAS_Z3
    tests.Expect(
        result.reason == "probabilistic full-width check only (no Z3 proof)",
        "without Z3, the result explains that probes are not a proof"
    );
#endif
}

void UnchangedExpression(TestContext& tests) {
    auto expression = cobra::Expr::Constant(42);
    const auto result = smba::SimplifyExpression(
        *expression, {}, OfflineOptions()
    );

    tests.Expect(
        result.status == smba::SimplifyStatus::Unchanged,
        "a constant expression must not be rewritten when its cost is unchanged"
    );
    tests.Expect(
        result.originalText == "42",
        "unchanged expression keeps its original rendering"
    );
}

void InvalidBitWidth(TestContext& tests) {
    auto expression = cobra::Expr::Variable(0);

    auto zero_width = OfflineOptions();
    zero_width.bitWidth = 0;
    const auto zero_result = smba::SimplifyExpression(
        *expression, {"x"}, zero_width
    );
    tests.Expect(
        zero_result.status == smba::SimplifyStatus::Error,
        "zero-bit expressions must be rejected before CoBRA evaluation"
    );
    tests.Expect(
        zero_result.reason == "unsupported bit width",
        "zero-bit rejection reports the unsupported bit width"
    );

    auto oversized = OfflineOptions();
    oversized.bitWidth = 65;
    const auto oversized_result = smba::SimplifyExpression(
        *expression, {"x"}, oversized
    );
    tests.Expect(
        oversized_result.status == smba::SimplifyStatus::Error,
        "bit widths above 64 must be rejected before CoBRA evaluation"
    );
}

void InvalidVariableLimit(TestContext& tests) {
    auto expression = cobra::Expr::Variable(0);
    auto options = OfflineOptions();
    options.maxVariables = 1;

    const auto result = smba::SimplifyExpression(
        *expression, {"x", "unused"}, options
    );
    tests.Expect(
        result.status == smba::SimplifyStatus::Rejected,
        "a variable list over maxVariables must be rejected"
    );
    tests.Expect(
        result.reason == "variable limit exceeded",
        "variable-limit rejection reports the limit reason"
    );
}

void StrictZ3Mode(TestContext& tests) {
    auto expression = XorSelf();
    auto options = OfflineOptions();
    options.requireZ3 = true;
    const auto result = smba::SimplifyExpression(*expression, {"x"}, options);

#ifdef SMBA_HAS_Z3
    tests.Expect(
        result.status == smba::SimplifyStatus::Simplified,
        "strict mode accepts a candidate only when the Z3 verifier is linked"
    );
    tests.Expect(result.z3Verified, "strict mode records a successful Z3 proof");
#else
    tests.Expect(
        result.status == smba::SimplifyStatus::Rejected,
        "strict mode rejects candidates when cobra-verify is unavailable"
    );
    tests.Expect(
        result.reason == "Z3 support is required but was not built",
        "strict mode reports the missing Z3 verifier"
    );
#endif
}

void AlwaysTrueUnsignedRangeComparison(TestContext& tests) {
    auto left = cobra::Expr::BitwiseOr(
        cobra::Expr::BitwiseAnd(cobra::Expr::Variable(0), cobra::Expr::Constant(1)),
        cobra::Expr::Constant(0x5e06624500000000ULL)
    );
    auto right = cobra::Expr::Constant(0x51f318f400000000ULL);
    const auto result = smba::ProveConstantComparison(
        *left,
        *right,
        {"x"},
        64,
        smba::ComparisonKind::UnsignedGreaterThan,
        OfflineOptions()
    );

#ifdef SMBA_HAS_Z3
    tests.Expect(
        result.status == smba::PredicateStatus::ProvedConstant,
        "the fixed unsigned range predicate must be formally constant"
    );
    tests.Expect(result.constantValue, "the unsigned range predicate is always true");
    tests.Expect(result.z3Verified, "constant predicates must record their Z3 proof");
#else
    tests.Expect(
        result.status == smba::PredicateStatus::Rejected,
        "without Z3, an always-true predicate must not be claimed as proved"
    );
    tests.Expect(
        result.reason == "Z3 verifier unavailable; constant-comparison proofs require Z3",
        "the unavailable predicate verifier reports an explicit reason"
    );
#endif
}

void AlwaysFalseFixedBitEqualityComparison(TestContext& tests) {
    auto left = cobra::Expr::BitwiseOr(
        cobra::Expr::BitwiseAnd(
            cobra::Expr::Variable(0), cobra::Expr::Constant(1ULL << 63U)
        ),
        cobra::Expr::Constant(0x5e06624500000000ULL)
    );
    auto right = cobra::Expr::Constant(0x5e06624500000001ULL);
    const auto result = smba::ProveConstantComparison(
        *left, *right, {"x"}, 64, smba::ComparisonKind::Equal, OfflineOptions()
    );

#ifdef SMBA_HAS_Z3
    tests.Expect(
        result.status == smba::PredicateStatus::ProvedConstant,
        "a fixed low-bit equality with a variable sign bit must be formally constant"
    );
    tests.Expect(!result.constantValue, "the fixed low-bit equality is always false");
    tests.Expect(result.z3Verified, "an always-false comparison needs an UNSAT proof");
#else
    tests.Expect(
        result.status == smba::PredicateStatus::Rejected,
        "without Z3, an always-false predicate must not be claimed as proved"
    );
#endif
}

void NonConstantComparison(TestContext& tests) {
    auto left = cobra::Expr::BitwiseAnd(cobra::Expr::Variable(0), cobra::Expr::Constant(1));
    auto right = cobra::Expr::Constant(0);
    const auto result = smba::ProveConstantComparison(
        *left, *right, {"x"}, 64, smba::ComparisonKind::Equal, OfflineOptions()
    );

#ifdef SMBA_HAS_Z3
    tests.Expect(
        result.status == smba::PredicateStatus::NotConstant,
        "x & 1 == 0 must be recognized as satisfiable both ways"
    );
    tests.Expect(!result.z3Verified, "a non-constant outcome is not a constant proof");
    tests.Expect(
        result.counterexample.has_value(),
        "a satisfiable-both-ways predicate should expose Z3 model evidence"
    );
#else
    tests.Expect(
        result.status == smba::PredicateStatus::Rejected,
        "without Z3, a non-constant predicate must not receive a probabilistic classification"
    );
#endif
}

void SignedAndUnsignedComparisonsDiffer(TestContext& tests) {
    auto left = cobra::Expr::Constant(0x80);
    auto right = cobra::Expr::Constant(1);
    const auto unsignedResult = smba::ProveConstantComparison(
        *left,
        *right,
        {},
        8,
        smba::ComparisonKind::UnsignedLessThan,
        OfflineOptions()
    );
    const auto signedResult = smba::ProveConstantComparison(
        *left, *right, {}, 8, smba::ComparisonKind::SignedLessThan, OfflineOptions()
    );

#ifdef SMBA_HAS_Z3
    tests.Expect(
        unsignedResult.status == smba::PredicateStatus::ProvedConstant
            && !unsignedResult.constantValue,
        "0x80 < 1 is false in an 8-bit unsigned comparison"
    );
    tests.Expect(
        signedResult.status == smba::PredicateStatus::ProvedConstant
            && signedResult.constantValue,
        "0x80 < 1 is true in an 8-bit signed comparison"
    );
    tests.Expect(
        unsignedResult.z3Verified && signedResult.z3Verified,
        "both signedness outcomes must be formally verified"
    );
#else
    tests.Expect(
        unsignedResult.status == smba::PredicateStatus::Rejected
            && signedResult.status == smba::PredicateStatus::Rejected,
        "without Z3, signed and unsigned predicate proofs both reject"
    );
#endif
}

void LogicalShiftMatchesCobraSemantics(TestContext& tests) {
    auto left = cobra::Expr::LogicalShr(cobra::Expr::Variable(0), 256);
    auto right = cobra::Expr::Constant(0);
    const auto result = smba::ProveConstantComparison(
        *left, *right, {"x"}, 8, smba::ComparisonKind::Equal, OfflineOptions()
    );

#ifdef SMBA_HAS_Z3
    tests.Expect(
        result.status == smba::PredicateStatus::ProvedConstant && result.constantValue,
        "a wide logical shift follows CoBRA's zero-result semantics instead of wrapping"
    );
#else
    tests.Expect(
        result.status == smba::PredicateStatus::Rejected,
        "without Z3, wide-shift predicates still fail closed"
    );
#endif
}

void InvalidPredicateInputs(TestContext& tests) {
    auto variable = cobra::Expr::Variable(0);
    auto zeroWidth = smba::ProveConstantComparison(
        *variable,
        *cobra::Expr::Constant(0),
        {"x"},
        0,
        smba::ComparisonKind::Equal,
        OfflineOptions()
    );
    tests.Expect(
        zeroWidth.status == smba::PredicateStatus::Error,
        "zero-bit predicate comparisons must be rejected before verification"
    );
    tests.Expect(
        zeroWidth.reason == "unsupported bit width",
        "zero-bit predicate comparisons report the unsupported width"
    );

    auto invalidVariable = cobra::Expr::Variable(1);
    auto badIndex = smba::ProveConstantComparison(
        *invalidVariable,
        *cobra::Expr::Constant(0),
        {"x"},
        64,
        smba::ComparisonKind::Equal,
        OfflineOptions()
    );
    tests.Expect(
        badIndex.status == smba::PredicateStatus::Rejected,
        "AST variable indices outside the shared name list must reject"
    );
    tests.Expect(
        badIndex.reason == "expression references a variable index outside the shared variable list",
        "out-of-range AST variables report their exact validation failure"
    );

    auto noVariables = OfflineOptions();
    noVariables.maxVariables = 0;
    auto tooManyVariables = smba::ProveConstantComparison(
        *variable,
        *cobra::Expr::Constant(0),
        {"x"},
        64,
        smba::ComparisonKind::Equal,
        noVariables
    );
    tests.Expect(
        tooManyVariables.status == smba::PredicateStatus::Rejected,
        "predicate variable names over maxVariables must reject"
    );
    tests.Expect(
        tooManyVariables.reason == "variable limit exceeded",
        "predicate variable-limit rejection reports the limit reason"
    );
}

void VerifierUnavailableComparisonPolicy(TestContext& tests) {
#ifndef SMBA_HAS_Z3
    auto left = cobra::Expr::Variable(0);
    auto right = cobra::Expr::Constant(0);

    auto relaxed = OfflineOptions();
    const auto relaxedResult = smba::ProveConstantComparison(
        *left, *right, {"x"}, 64, smba::ComparisonKind::Equal, relaxed
    );
    auto strict = relaxed;
    strict.requireZ3 = true;
    const auto strictResult = smba::ProveConstantComparison(
        *left, *right, {"x"}, 64, smba::ComparisonKind::Equal, strict
    );

    tests.Expect(
        relaxedResult.status == smba::PredicateStatus::Rejected
            && strictResult.status == smba::PredicateStatus::Rejected,
        "comparison proofs reject without Z3 in both relaxed and strict modes"
    );
    tests.Expect(
        relaxedResult.reason == "Z3 verifier unavailable; constant-comparison proofs require Z3"
            && strictResult.reason
                == "Z3 verifier unavailable; constant-comparison proofs require Z3",
        "both no-Z3 predicate modes explain that a verifier is unavailable"
    );
#else
    (void)tests;
#endif
}

} // namespace

int main() {
    TestContext tests;
    SimplifiableIdentity(tests);
    UnchangedExpression(tests);
    InvalidBitWidth(tests);
    InvalidVariableLimit(tests);
    StrictZ3Mode(tests);
    AlwaysTrueUnsignedRangeComparison(tests);
    AlwaysFalseFixedBitEqualityComparison(tests);
    NonConstantComparison(tests);
    SignedAndUnsignedComparisonsDiffer(tests);
    LogicalShiftMatchesCobraSemantics(tests);
    InvalidPredicateInputs(tests);
    VerifierUnavailableComparisonPolicy(tests);

    if (tests.failures != 0) {
        std::cerr << tests.failures << " SMBA core test(s) failed\n";
        return EXIT_FAILURE;
    }
#ifdef SMBA_HAS_Z3
    std::cout << "SMBA core tests passed (Z3 proof path)\n";
#else
    std::cout << "SMBA core tests passed (probabilistic diagnostic path; no Z3 proof)\n";
#endif
    return EXIT_SUCCESS;
}
