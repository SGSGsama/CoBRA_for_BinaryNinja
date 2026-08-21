#include "smba/Engine.h"

#include "cobra/core/ExprCost.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureEval.h"
#include "cobra/core/Simplifier.h"

#ifdef SMBA_HAS_Z3
#include "cobra/verify/Z3Verifier.h"
#include <z3.h>
#endif

#include <sstream>
#include <utility>

namespace smba {

namespace {

bool ValidateComparisonExpr(
    const cobra::Expr& expression,
    const std::vector<std::string>& variables,
    std::string& reason
) {
    size_t expectedChildren = 0;
    switch (expression.kind) {
        case cobra::Expr::Kind::kConstant:
            return true;
        case cobra::Expr::Kind::kVariable:
            if (expression.var_index >= variables.size()) {
                reason = "expression references a variable index outside the shared variable list";
                return false;
            }
            return true;
        case cobra::Expr::Kind::kAdd:
        case cobra::Expr::Kind::kMul:
        case cobra::Expr::Kind::kAnd:
        case cobra::Expr::Kind::kOr:
        case cobra::Expr::Kind::kXor:
            expectedChildren = 2;
            break;
        case cobra::Expr::Kind::kNot:
        case cobra::Expr::Kind::kNeg:
        case cobra::Expr::Kind::kShr:
            expectedChildren = 1;
            break;
        default:
            reason = "expression contains an unsupported node kind";
            return false;
    }

    if (expression.children.size() != expectedChildren) {
        reason = "expression has an invalid child count";
        return false;
    }
    for (const auto& child : expression.children) {
        if (!child) {
            reason = "expression contains a null child";
            return false;
        }
        if (!ValidateComparisonExpr(*child, variables, reason)) {
            return false;
        }
    }
    return true;
}

bool IsSupportedComparison(ComparisonKind comparison) {
    switch (comparison) {
        case ComparisonKind::Equal:
        case ComparisonKind::NotEqual:
        case ComparisonKind::UnsignedLessThan:
        case ComparisonKind::UnsignedLessEqual:
        case ComparisonKind::UnsignedGreaterEqual:
        case ComparisonKind::UnsignedGreaterThan:
        case ComparisonKind::SignedLessThan:
        case ComparisonKind::SignedLessEqual:
        case ComparisonKind::SignedGreaterEqual:
        case ComparisonKind::SignedGreaterThan:
            return true;
        default:
            return false;
    }
}

#ifdef SMBA_HAS_Z3

Z3_ast BuildPredicateExpr(
    Z3_context context,
    const cobra::Expr& expression,
    const std::vector<Z3_ast>& variables,
    uint32_t bitWidth
) {
    const auto bitVectorSort = Z3_mk_bv_sort(context, bitWidth);
    switch (expression.kind) {
        case cobra::Expr::Kind::kConstant:
            return Z3_mk_unsigned_int64(context, expression.constant_val, bitVectorSort);
        case cobra::Expr::Kind::kVariable:
            return variables[expression.var_index];
        case cobra::Expr::Kind::kAdd:
            return Z3_mk_bvadd(
                context,
                BuildPredicateExpr(context, *expression.children[0], variables, bitWidth),
                BuildPredicateExpr(context, *expression.children[1], variables, bitWidth)
            );
        case cobra::Expr::Kind::kMul:
            return Z3_mk_bvmul(
                context,
                BuildPredicateExpr(context, *expression.children[0], variables, bitWidth),
                BuildPredicateExpr(context, *expression.children[1], variables, bitWidth)
            );
        case cobra::Expr::Kind::kAnd:
            return Z3_mk_bvand(
                context,
                BuildPredicateExpr(context, *expression.children[0], variables, bitWidth),
                BuildPredicateExpr(context, *expression.children[1], variables, bitWidth)
            );
        case cobra::Expr::Kind::kOr:
            return Z3_mk_bvor(
                context,
                BuildPredicateExpr(context, *expression.children[0], variables, bitWidth),
                BuildPredicateExpr(context, *expression.children[1], variables, bitWidth)
            );
        case cobra::Expr::Kind::kXor:
            return Z3_mk_bvxor(
                context,
                BuildPredicateExpr(context, *expression.children[0], variables, bitWidth),
                BuildPredicateExpr(context, *expression.children[1], variables, bitWidth)
            );
        case cobra::Expr::Kind::kNot:
            return Z3_mk_bvnot(
                context,
                BuildPredicateExpr(context, *expression.children[0], variables, bitWidth)
            );
        case cobra::Expr::Kind::kNeg:
            return Z3_mk_bvneg(
                context,
                BuildPredicateExpr(context, *expression.children[0], variables, bitWidth)
            );
        case cobra::Expr::Kind::kShr:
            // Expr::LogicalShr stores its count outside the bitvector domain.
            // CoBRA's ModShr returns zero for a count at least as wide as the
            // value; do not narrow a large count and accidentally wrap it.
            if (expression.constant_val >= bitWidth) {
                return Z3_mk_unsigned_int64(context, 0, bitVectorSort);
            }
            return Z3_mk_bvlshr(
                context,
                BuildPredicateExpr(context, *expression.children[0], variables, bitWidth),
                Z3_mk_unsigned_int64(context, expression.constant_val, bitVectorSort)
            );
    }
    return nullptr;
}

Z3_ast BuildComparison(
    Z3_context context,
    Z3_ast left,
    Z3_ast right,
    ComparisonKind comparison
) {
    switch (comparison) {
        case ComparisonKind::Equal:
            return Z3_mk_eq(context, left, right);
        case ComparisonKind::NotEqual:
            return Z3_mk_not(context, Z3_mk_eq(context, left, right));
        case ComparisonKind::UnsignedLessThan:
            return Z3_mk_bvult(context, left, right);
        case ComparisonKind::UnsignedLessEqual:
            return Z3_mk_bvule(context, left, right);
        case ComparisonKind::UnsignedGreaterEqual:
            return Z3_mk_bvuge(context, left, right);
        case ComparisonKind::UnsignedGreaterThan:
            return Z3_mk_bvugt(context, left, right);
        case ComparisonKind::SignedLessThan:
            return Z3_mk_bvslt(context, left, right);
        case ComparisonKind::SignedLessEqual:
            return Z3_mk_bvsle(context, left, right);
        case ComparisonKind::SignedGreaterEqual:
            return Z3_mk_bvsge(context, left, right);
        case ComparisonKind::SignedGreaterThan:
            return Z3_mk_bvsgt(context, left, right);
    }
    return nullptr;
}

std::optional<std::string> FormatModel(
    Z3_context context,
    Z3_model model,
    const std::vector<Z3_ast>& variables,
    const std::vector<std::string>& names
) {
    if (model == nullptr) {
        return std::nullopt;
    }

    std::ostringstream output;
    output << '{';
    for (size_t index = 0; index < variables.size(); ++index) {
        if (index != 0) {
            output << ", ";
        }
        Z3_ast value = nullptr;
        output << names[index] << '[' << index << "]=";
        if (!Z3_model_eval(context, model, variables[index], true, &value)
            || value == nullptr) {
            output << '?';
            continue;
        }
        const char* numeral = Z3_get_numeral_string(context, value);
        output << (numeral == nullptr ? "?" : numeral);
    }
    output << '}';
    return output.str();
}

struct SolverQuery {
    Z3_lbool status = Z3_L_UNDEF;
    std::optional<std::string> model;
};

SolverQuery QueryPredicate(
    Z3_context context,
    Z3_solver solver,
    Z3_ast assertion,
    const std::vector<Z3_ast>& variables,
    const std::vector<std::string>& names
) {
    Z3_solver_push(context, solver);
    Z3_solver_assert(context, solver, assertion);

    SolverQuery result;
    result.status = Z3_solver_check(context, solver);
    if (result.status == Z3_L_TRUE) {
        result.model = FormatModel(
            context, Z3_solver_get_model(context, solver), variables, names
        );
    }

    Z3_solver_pop(context, solver, 1);
    return result;
}

#endif

} // namespace

SimplifyResult SimplifyExpression(
    const cobra::Expr& original,
    const std::vector<std::string>& variables,
    const SimplifyOptions& options
) {
    SimplifyResult output;
    output.originalText = cobra::Render(original, variables, options.bitWidth);

    if (options.bitWidth == 0 || options.bitWidth > 64) {
        output.reason = "unsupported bit width";
        return output;
    }
    if (variables.size() > options.maxVariables || variables.size() >= 63) {
        output.status = SimplifyStatus::Rejected;
        output.reason = "variable limit exceeded";
        return output;
    }

    const auto baselineCost = cobra::ComputeCost(original).cost;
    output.originalCost = baselineCost.weighted_size;

    auto signature = cobra::EvaluateBooleanSignature(
        original, static_cast<uint32_t>(variables.size()), options.bitWidth
    );
    cobra::Options cobraOptions{
        .bitwidth = options.bitWidth,
        .max_vars = options.maxVariables,
        .spot_check = true,
    };
    cobraOptions.evaluator = cobra::Evaluator::FromExpr(original, options.bitWidth);

    auto simplified = cobra::Simplify(signature, variables, &original, cobraOptions);
    if (!simplified.has_value()) {
        output.reason = simplified.error().message;
        return output;
    }

    auto outcome = std::move(simplified.value());
    if (!outcome.expr) {
        output.reason = "CoBRA returned no expression";
        return output;
    }
    if (outcome.kind == cobra::SimplifyOutcome::Kind::kError) {
        output.reason = outcome.diag.reason;
        return output;
    }
    if (outcome.kind == cobra::SimplifyOutcome::Kind::kUnchangedUnsupported) {
        output.status = SimplifyStatus::Unchanged;
        output.reason = outcome.diag.reason;
        return output;
    }

    const auto candidateCost = cobra::ComputeCost(*outcome.expr).cost;
    output.simplifiedCost = candidateCost.weighted_size;
    if (!cobra::IsBetter(candidateCost, baselineCost)) {
        output.status = SimplifyStatus::Unchanged;
        output.reason = "candidate does not reduce CoBRA cost";
        return output;
    }

    const auto variableMap = cobra::BuildVarSupport(variables, outcome.real_vars);
    const auto fullWidth = cobra::FullWidthCheck(
        original,
        static_cast<uint32_t>(variables.size()),
        *outcome.expr,
        variableMap,
        options.bitWidth,
        options.fullWidthProbes
    );
    if (!fullWidth.passed) {
        output.status = SimplifyStatus::Rejected;
        output.reason = "full-width differential verification failed";
        return output;
    }
    output.fullWidthVerified = true;

#ifdef SMBA_HAS_Z3
    auto z3Candidate = cobra::CloneExpr(*outcome.expr);
    if (!variableMap.empty()) {
        cobra::RemapVarIndices(*z3Candidate, variableMap);
    }
    const auto proof = cobra::Z3VerifyExprs(
        original, *z3Candidate, variables, options.bitWidth, options.z3TimeoutMs
    );
    if (!proof.equivalent) {
        output.status = SimplifyStatus::Rejected;
        output.reason = proof.timed_out
            ? "Z3 proof timed out"
            : "Z3 equivalence proof failed: " + proof.counterexample;
        return output;
    }
    output.z3Verified = true;
#else
    if (options.requireZ3) {
        output.status = SimplifyStatus::Rejected;
        output.reason = "Z3 support is required but was not built";
        return output;
    }
#endif

    output.variables = std::move(outcome.real_vars);
    output.simplifiedText = cobra::Render(
        *outcome.expr, output.variables, options.bitWidth
    );
    output.expression = std::move(outcome.expr);
    if (output.z3Verified) {
        output.status = SimplifyStatus::Simplified;
        output.reason = "Z3-verified simplification";
    } else {
        // Useful for headless/offline experiments, but never an automatic
        // rewrite authorization. The BN adapter always requests Z3.
        output.status = SimplifyStatus::Probabilistic;
        output.reason = "probabilistic full-width check only (no Z3 proof)";
    }
    return output;
}

PredicateResult ProveConstantComparison(
    const cobra::Expr& left,
    const cobra::Expr& right,
    const std::vector<std::string>& variables,
    uint32_t bitWidth,
    ComparisonKind comparison,
    const SimplifyOptions& options
) {
    PredicateResult output;

    if (bitWidth == 0 || bitWidth > 64) {
        output.reason = "unsupported bit width";
        return output;
    }
    if (variables.size() > options.maxVariables || variables.size() >= 63) {
        output.status = PredicateStatus::Rejected;
        output.reason = "variable limit exceeded";
        return output;
    }
    if (!IsSupportedComparison(comparison)) {
        output.reason = "unsupported comparison kind";
        return output;
    }

    std::string validationReason;
    if (!ValidateComparisonExpr(left, variables, validationReason)
        || !ValidateComparisonExpr(right, variables, validationReason)) {
        output.status = PredicateStatus::Rejected;
        output.reason = std::move(validationReason);
        return output;
    }

#ifndef SMBA_HAS_Z3
    (void)options;
    output.status = PredicateStatus::Rejected;
    output.reason = "Z3 verifier unavailable; constant-comparison proofs require Z3";
    return output;
#else
    Z3_config configuration = Z3_mk_config();
    const auto timeout = std::to_string(options.z3TimeoutMs);
    Z3_set_param_value(configuration, "timeout", timeout.c_str());
    Z3_context context = Z3_mk_context(configuration);
    Z3_del_config(configuration);

    const auto bitVectorSort = Z3_mk_bv_sort(context, bitWidth);
    std::vector<Z3_ast> z3Variables;
    z3Variables.reserve(variables.size());
    for (size_t index = 0; index < variables.size(); ++index) {
        // The public names are labels, not an aliasing mechanism.  Indexing
        // the Z3 symbols makes duplicate or punctuation-containing labels
        // safe while both trees still share precisely the same variables.
        const auto symbolName = "smba_predicate_var_" + std::to_string(index);
        const auto symbol = Z3_mk_string_symbol(context, symbolName.c_str());
        z3Variables.push_back(Z3_mk_const(context, symbol, bitVectorSort));
    }

    const auto leftAst = BuildPredicateExpr(context, left, z3Variables, bitWidth);
    const auto rightAst = BuildPredicateExpr(context, right, z3Variables, bitWidth);
    const auto predicate = BuildComparison(context, leftAst, rightAst, comparison);

    Z3_solver solver = Z3_mk_solver(context);
    Z3_solver_inc_ref(context, solver);

    // An UNSAT predicate is a formal proof that the comparison is false for
    // every assignment.  If it is SAT, separately prove/refute the opposite
    // polarity so a NotConstant outcome is never inferred from sampling.
    const auto trueQuery = QueryPredicate(
        context, solver, predicate, z3Variables, variables
    );
    if (trueQuery.status == Z3_L_UNDEF) {
        output.status = PredicateStatus::Rejected;
        output.reason = "Z3 predicate proof returned unknown (possible timeout)";
        output.timedOut = true;
    } else if (trueQuery.status == Z3_L_FALSE) {
        output.status = PredicateStatus::ProvedConstant;
        output.constantValue = false;
        output.reason = "Z3 proved predicate is universally false";
        output.z3Verified = true;
    } else {
        const auto falsePredicate = Z3_mk_not(context, predicate);
        const auto falseQuery = QueryPredicate(
            context, solver, falsePredicate, z3Variables, variables
        );
        if (falseQuery.status == Z3_L_UNDEF) {
            output.status = PredicateStatus::Rejected;
            output.reason = "Z3 predicate proof returned unknown (possible timeout)";
            output.timedOut = true;
        } else if (falseQuery.status == Z3_L_FALSE) {
            output.status = PredicateStatus::ProvedConstant;
            output.constantValue = true;
            output.reason = "Z3 proved predicate is universally true";
            output.z3Verified = true;
        } else {
            output.status = PredicateStatus::NotConstant;
            output.reason = "predicate is satisfiable for both true and false outcomes";
            if (trueQuery.model.has_value() && falseQuery.model.has_value()) {
                output.counterexample = "true: " + *trueQuery.model
                    + "; false: " + *falseQuery.model;
            } else if (trueQuery.model.has_value()) {
                output.counterexample = "true: " + *trueQuery.model;
            } else if (falseQuery.model.has_value()) {
                output.counterexample = "false: " + *falseQuery.model;
            }
        }
    }

    Z3_solver_dec_ref(context, solver);
    Z3_del_context(context);
    return output;
#endif
}

} // namespace smba
