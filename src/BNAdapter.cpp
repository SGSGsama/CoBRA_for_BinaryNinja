#include "smba/PluginAPI.h"

#include "smba/Engine.h"

#include "cobra/core/Expr.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace BinaryNinja;

namespace smba {
namespace {

struct RecoveryStats {
    size_t nodes = 0;
    size_t maxDepth = 0;
    bool hasArithmetic = false;
    bool hasBitwise = false;
};

// Registered Workflow topology is immutable, but an Activity action may
// dispatch through mutable state.  Keeping the state outside the Activity
// lets a repeat registration command refresh the behavior without touching
// Binary Ninja's registered graph or adding a second same-name Activity.
struct ActivityDispatchState {
    std::mutex mutex;
    uint64_t generation = 0;
    std::function<void(Ref<AnalysisContext>)> action;
};

std::mutex g_ownedActivitiesMutex;
// Activity identity is stable across Workflow wrappers and lookups.  Do not
// key this registry by BNWorkflow*: a clone that fails registration must not
// leave a workflow-object ownership record behind.  The registered Activity's
// callback owns the dispatch state; weak entries become harmless and are
// discarded if an unregistered clone is destroyed.
std::unordered_map<BNActivity*, std::weak_ptr<ActivityDispatchState>> g_ownedActivities;

void RunMBAActivity(Ref<AnalysisContext> context) {
    auto mlil = context ? context->GetMediumLevelILFunction() : nullptr;
    if (!mlil) {
        return;
    }
    const auto report = TransformFunction(context);
    if (report.applied != 0) {
        LogInfo(
            "[SMBA] 0x%llx: applied %zu verified MBA simplification(s)",
            static_cast<unsigned long long>(context->GetFunction()->GetStart()),
            report.applied
        );
    }
}

void DispatchMBAActivity(
    const std::shared_ptr<ActivityDispatchState>& dispatch,
    Ref<AnalysisContext> context
) {
    std::function<void(Ref<AnalysisContext>)> action;
    {
        std::lock_guard lock(dispatch->mutex);
        action = dispatch->action;
    }
    if (action) {
        action(context);
    }
}

uint64_t RefreshDispatch(const std::shared_ptr<ActivityDispatchState>& dispatch) {
    std::lock_guard lock(dispatch->mutex);
    const uint64_t generation = ++dispatch->generation;
    dispatch->action = RunMBAActivity;
    return generation;
}

struct RecoveredExpression {
    std::unique_ptr<cobra::Expr> expression;
    std::unique_ptr<cobra::Expr> predicateLeft;
    std::unique_ptr<cobra::Expr> predicateRight;
    std::vector<std::string> variables;
    std::map<std::string, Variable> variableByName;
    RecoveryStats stats;
    std::string error;
};

std::string LeafName(Function* function, const SSAVariable& variable) {
    std::ostringstream stream;
    std::string base = function
        ? function->GetVariableNameOrDefault(variable.var)
        : "var";
    stream << base << "__" << variable.var.ToIdentifier() << "_" << variable.version;
    return stream.str();
}

class ExpressionRecovery {
public:
    ExpressionRecovery(MediumLevelILFunction* ssa, uint32_t bitWidth, const AnalysisLimits& limits)
        : m_ssa(ssa), m_bitWidth(bitWidth), m_limits(limits) {}

    RecoveredExpression Recover(const MediumLevelILInstruction& root) {
        RecoveredExpression result;
        m_result = &result;
        m_root = root;
        m_predicateRecovery = false;
        result.expression = Visit(root, 0);
        m_result = nullptr;
        return result;
    }

    // A comparison must be recovered in one context.  In particular, its two
    // arms share one SSA leaf table (and one variable budget), so the core
    // prover sees an equality of the same symbolic values rather than two
    // independently named copies of the same def-use graph.
    RecoveredExpression RecoverComparison(const MediumLevelILInstruction& root) {
        RecoveredExpression result;
        m_result = &result;
        m_root = root;
        m_predicateRecovery = true;
        if (root.size == 0 || root.size > 8) {
            Fail("unsupported comparison width");
        } else {
            const auto left = root.GetLeftExpr();
            const auto right = root.GetRightExpr();
            if (left.size != root.size || right.size != root.size) {
                Fail("comparison operands do not have the comparison width");
            } else {
                result.predicateLeft = Visit(left, 0);
                if (result.predicateLeft) {
                    result.predicateRight = Visit(right, 0);
                }
            }
        }
        m_predicateRecovery = false;
        m_result = nullptr;
        return result;
    }

private:
    MediumLevelILFunction* m_ssa;
    uint32_t m_bitWidth;
    const AnalysisLimits& m_limits;
    RecoveredExpression* m_result = nullptr;
    MediumLevelILInstruction m_root;
    bool m_predicateRecovery = false;
    std::set<size_t> m_activeExpressions;
    std::unordered_map<size_t, std::unique_ptr<cobra::Expr>> m_expressionCache;

    uint64_t WidthMask(uint32_t bitWidth) const {
        if (bitWidth >= 64) {
            return std::numeric_limits<uint64_t>::max();
        }
        return (uint64_t{1} << bitWidth) - 1;
    }

    std::unique_ptr<cobra::Expr> Narrow(
        std::unique_ptr<cobra::Expr> expression,
        uint32_t semanticWidth
    ) {
        if (!expression) {
            return nullptr;
        }
        if (semanticWidth == 0 || semanticWidth > m_bitWidth) {
            return Fail("invalid semantic width");
        }
        if (semanticWidth == m_bitWidth) {
            return expression;
        }
        // CoBRA's expression domain is the comparison/root width.  A
        // narrower MLIL value has modular arithmetic at its own width; mask
        // every narrow result, not merely its leaves, before it flows into a
        // wider operation.  This is the exact bitvector meaning of MLIL_ZX.
        return cobra::Expr::BitwiseAnd(
            std::move(expression), cobra::Expr::Constant(WidthMask(semanticWidth))
        );
    }

    uint32_t SemanticWidth(const MediumLevelILInstruction& instruction) {
        if (instruction.size == 0 || instruction.size > 8) {
            Fail("unsupported expression width");
            return 0;
        }
        const auto bitWidth = static_cast<uint32_t>(instruction.size * 8);
        if (bitWidth > m_bitWidth) {
            Fail("expression is wider than its recovery root");
            return 0;
        }
        return bitWidth;
    }

    std::unique_ptr<cobra::Expr> Fail(const std::string& reason) {
        if (m_result && m_result->error.empty()) {
            m_result->error = reason;
        }
        return nullptr;
    }

    bool CountNode(size_t depth) {
        if (!m_result) {
            return false;
        }
        ++m_result->stats.nodes;
        m_result->stats.maxDepth = std::max(m_result->stats.maxDepth, depth);
        if (m_result->stats.nodes > m_limits.maxNodes || depth > m_limits.maxDepth) {
            Fail("SSA expansion budget exceeded");
            return false;
        }
        return true;
    }

    std::unique_ptr<cobra::Expr> Binary(
        const MediumLevelILInstruction& instruction,
        size_t depth,
        cobra::Expr::Kind kind
    ) {
        const auto semanticWidth = SemanticWidth(instruction);
        if (semanticWidth == 0) {
            return nullptr;
        }
        const auto leftInstruction = instruction.GetLeftExpr();
        const auto rightInstruction = instruction.GetRightExpr();
        if (leftInstruction.size != instruction.size || rightInstruction.size != instruction.size) {
            return Fail("mixed-width binary operation without MLIL_ZX");
        }
        auto left = Visit(leftInstruction, depth + 1);
        auto right = Visit(rightInstruction, depth + 1);
        if (!left || !right) {
            return nullptr;
        }
        switch (kind) {
            case cobra::Expr::Kind::kAdd:
                return Narrow(cobra::Expr::Add(std::move(left), std::move(right)), semanticWidth);
            case cobra::Expr::Kind::kMul:
                return Narrow(cobra::Expr::Mul(std::move(left), std::move(right)), semanticWidth);
            case cobra::Expr::Kind::kAnd:
                return Narrow(cobra::Expr::BitwiseAnd(std::move(left), std::move(right)), semanticWidth);
            case cobra::Expr::Kind::kOr:
                return Narrow(cobra::Expr::BitwiseOr(std::move(left), std::move(right)), semanticWidth);
            case cobra::Expr::Kind::kXor:
                return Narrow(cobra::Expr::BitwiseXor(std::move(left), std::move(right)), semanticWidth);
            default:
                return Fail("internal binary operation mismatch");
        }
    }

    std::unique_ptr<cobra::Expr> VariableLeaf(
        const SSAVariable& variable,
        uint32_t semanticWidth
    ) {
        // SSA expansion can expose an older version of a variable that has
        // already been overwritten at the replacement site. Emitting that as
        // a non-SSA MLIL_VAR would silently read the current version instead.
        // Stop at impurity boundaries only when the leaf version is exactly
        // the version live at the root and its MLIL expression supplied an
        // established semantic width.
        if (semanticWidth == 0 || semanticWidth > m_bitWidth) {
            return Fail("symbolic SSA leaf has no established semantic width");
        }
        if (m_root.GetSSAVarVersion(variable.var) != variable.version) {
            return Fail("expanded SSA leaf is not live at replacement root");
        }
        const auto name = LeafName(m_ssa->GetFunction(), variable);
        auto found = std::find(m_result->variables.begin(), m_result->variables.end(), name);
        uint32_t index;
        if (found == m_result->variables.end()) {
            if (m_result->variables.size() >= m_limits.maxVariables) {
                return Fail("variable limit exceeded");
            }
            index = static_cast<uint32_t>(m_result->variables.size());
            m_result->variables.push_back(name);
            m_result->variableByName.emplace(name, variable.var);
        } else {
            index = static_cast<uint32_t>(std::distance(m_result->variables.begin(), found));
        }
        return Narrow(cobra::Expr::Variable(index), semanticWidth);
    }

    std::unique_ptr<cobra::Expr> PredicateBooleanLeaf(uint32_t semanticWidth) {
        if (!m_predicateRecovery || semanticWidth == 0 || semanticWidth > m_bitWidth) {
            return Fail("boolean conversion is unsupported outside predicate proofs");
        }
        if (m_result->variables.size() >= m_limits.maxVariables) {
            return Fail("variable limit exceeded");
        }
        // MLIL_BOOL_TO_INT is exactly 0 or 1. Its input may be a prior
        // condition whose SSA version is intentionally no longer live at the
        // comparison root. A fresh proof-only bit is a conservative
        // over-approximation: proving the predicate for every such bit is
        // sound, and this leaf is never emitted into rebuilt non-SSA MLIL.
        const auto index = static_cast<uint32_t>(m_result->variables.size());
        m_result->variables.push_back("predicate_bool_" + std::to_string(index));
        return Narrow(cobra::Expr::BitwiseAnd(
            cobra::Expr::Variable(index), cobra::Expr::Constant(1)
        ), semanticWidth);
    }

    std::unique_ptr<cobra::Expr> ExpandVariable(
        const SSAVariable& variable,
        uint32_t semanticWidth,
        size_t depth
    ) {
        if (variable.version == 0) {
            return VariableLeaf(variable, semanticWidth);
        }

        const size_t definition = m_ssa->GetSSAVarDefinition(variable);
        if (definition >= m_ssa->GetInstructionCount()) {
            return VariableLeaf(variable, semanticWidth);
        }

        const auto definingInstruction = m_ssa->GetInstruction(definition);
        if (definingInstruction.operation != MLIL_SET_VAR_SSA) {
            // PHI, call output, memory/alias definitions, and any other
            // non-single pure definition are value boundaries, never
            // operations to inline into the CoBRA tree.
            return VariableLeaf(variable, semanticWidth);
        }
        if (definingInstruction.GetDestSSAVariable() != variable) {
            return VariableLeaf(variable, semanticWidth);
        }
        const auto source = definingInstruction.GetSourceExpr();
        if (source.size != definingInstruction.size || source.size != semanticWidth / 8) {
            return VariableLeaf(variable, semanticWidth);
        }

        // When an otherwise pure definition reaches a load/call/unsupported
        // operation or cast, retain this live SSA version as one symbolic
        // leaf. Roll back leaves discovered only inside that abandoned inline
        // attempt; the node/depth work itself remains charged to the budget.
        const auto variablesBefore = m_result->variables;
        const auto variableByNameBefore = m_result->variableByName;
        const auto cacheBefore = m_expressionCache.size();
        auto expanded = Visit(source, depth + 1);
        if (expanded) {
            return expanded;
        }
        m_result->variables = variablesBefore;
        m_result->variableByName = variableByNameBefore;
        if (m_expressionCache.size() != cacheBefore) {
            // Cache entries introduced by a failed subgraph may depend on
            // temporary leaves. Keeping none is both simpler and conservative.
            m_expressionCache.clear();
        }
        m_result->error.clear();
        return VariableLeaf(variable, semanticWidth);
    }

    std::unique_ptr<cobra::Expr> Visit(const MediumLevelILInstruction& instruction, size_t depth) {
        const auto semanticWidth = SemanticWidth(instruction);
        if (semanticWidth == 0) {
            return nullptr;
        }
        if (const auto found = m_expressionCache.find(instruction.exprIndex);
            found != m_expressionCache.end()) {
            return cobra::CloneExpr(*found->second);
        }
        if (!CountNode(depth)) {
            return nullptr;
        }
        if (!m_activeExpressions.insert(instruction.exprIndex).second) {
            return Fail("cycle in SSA def-use expansion");
        }
        struct ActiveGuard {
            std::set<size_t>& set;
            size_t expression;
            ~ActiveGuard() { set.erase(expression); }
        } guard{m_activeExpressions, instruction.exprIndex};

        std::unique_ptr<cobra::Expr> recovered;
        switch (instruction.operation) {
            case MLIL_CONST:
                recovered = cobra::Expr::Constant(
                    static_cast<uint64_t>(instruction.GetConstant()) & WidthMask(semanticWidth)
                );
                break;
            case MLIL_VAR_SSA:
                recovered = ExpandVariable(
                    instruction.GetSourceSSAVariable(), semanticWidth, depth
                );
                break;
            case MLIL_VAR: {
                SSAVariable variable{instruction.GetSourceVariable(), 0};
                recovered = VariableLeaf(variable, semanticWidth);
                break;
            }
            case MLIL_ADD:
                m_result->stats.hasArithmetic = true;
                recovered = Binary(instruction, depth, cobra::Expr::Kind::kAdd);
                break;
            case MLIL_SUB: {
                m_result->stats.hasArithmetic = true;
                const auto leftInstruction = instruction.GetLeftExpr();
                const auto rightInstruction = instruction.GetRightExpr();
                if (leftInstruction.size != instruction.size || rightInstruction.size != instruction.size) {
                    return Fail("mixed-width subtraction without MLIL_ZX");
                }
                auto left = Visit(leftInstruction, depth + 1);
                auto right = Visit(rightInstruction, depth + 1);
                if (!left || !right) {
                    return nullptr;
                }
                recovered = Narrow(cobra::Expr::Add(
                    std::move(left), cobra::Expr::Negate(std::move(right))
                ), semanticWidth);
                break;
            }
            case MLIL_NEG: {
                m_result->stats.hasArithmetic = true;
                const auto source = instruction.GetSourceExpr();
                if (source.size != instruction.size) {
                    return Fail("mixed-width negate without MLIL_ZX");
                }
                if (auto child = Visit(source, depth + 1)) {
                    recovered = Narrow(cobra::Expr::Negate(std::move(child)), semanticWidth);
                }
                break;
            }
            case MLIL_MUL:
                m_result->stats.hasArithmetic = true;
                recovered = Binary(instruction, depth, cobra::Expr::Kind::kMul);
                break;
            case MLIL_AND:
                m_result->stats.hasBitwise = true;
                recovered = Binary(instruction, depth, cobra::Expr::Kind::kAnd);
                break;
            case MLIL_OR:
                m_result->stats.hasBitwise = true;
                recovered = Binary(instruction, depth, cobra::Expr::Kind::kOr);
                break;
            case MLIL_XOR:
                m_result->stats.hasBitwise = true;
                recovered = Binary(instruction, depth, cobra::Expr::Kind::kXor);
                break;
            case MLIL_NOT: {
                m_result->stats.hasBitwise = true;
                const auto source = instruction.GetSourceExpr();
                if (source.size != instruction.size) {
                    return Fail("mixed-width bitwise-not without MLIL_ZX");
                }
                if (auto child = Visit(source, depth + 1)) {
                    recovered = Narrow(cobra::Expr::BitwiseNot(std::move(child)), semanticWidth);
                }
                break;
            }
            case MLIL_ZX: {
                const auto source = instruction.GetSourceExpr();
                const auto sourceWidth = SemanticWidth(source);
                if (sourceWidth == 0 || sourceWidth >= semanticWidth) {
                    return Fail("invalid MLIL_ZX width transition");
                }
                auto child = Visit(source, depth + 1);
                // The child has already been constrained to sourceWidth.
                // Applying its precise low-bit mask again makes the widening
                // contract explicit and protects against future new leaves.
                recovered = Narrow(std::move(child), sourceWidth);
                break;
            }
            case MLIL_BOOL_TO_INT:
                recovered = PredicateBooleanLeaf(semanticWidth);
                break;
            case MLIL_LSR: {
                m_result->stats.hasBitwise = true;
                const auto valueInstruction = instruction.GetLeftExpr();
                const auto amount = instruction.GetRightExpr();
                if (valueInstruction.size != instruction.size) {
                    return Fail("mixed-width logical shift without MLIL_ZX");
                }
                if (amount.operation != MLIL_CONST) {
                    return Fail("variable logical shift is unsupported");
                }
                auto value = Visit(valueInstruction, depth + 1);
                if (!value) {
                    return nullptr;
                }
                recovered = Narrow(cobra::Expr::LogicalShr(
                    std::move(value), static_cast<uint64_t>(amount.GetConstant())
                ), semanticWidth);
                break;
            }
            case MLIL_LSL: {
                m_result->stats.hasArithmetic = true;
                const auto valueInstruction = instruction.GetLeftExpr();
                const auto amount = instruction.GetRightExpr();
                if (valueInstruction.size != instruction.size) {
                    return Fail("mixed-width left shift without MLIL_ZX");
                }
                if (amount.operation != MLIL_CONST) {
                    return Fail("variable left shift is unsupported");
                }
                const uint64_t shift = static_cast<uint64_t>(amount.GetConstant());
                if (shift >= semanticWidth) {
                    recovered = cobra::Expr::Constant(0);
                    break;
                }
                auto value = Visit(valueInstruction, depth + 1);
                if (!value) {
                    return nullptr;
                }
                recovered = Narrow(cobra::Expr::Mul(
                    std::move(value), cobra::Expr::Constant(uint64_t{1} << shift)
                ), semanticWidth);
                break;
            }
            default:
                return Fail("operation is an impurity or unsupported boundary");
        }
        if (!recovered) {
            return nullptr;
        }
        m_expressionCache.emplace(instruction.exprIndex, cobra::CloneExpr(*recovered));
        return recovered;
    }
};

bool IsCandidateRoot(BNMediumLevelILOperation operation) {
    switch (operation) {
        case MLIL_ADD:
        case MLIL_SUB:
        case MLIL_MUL:
        case MLIL_NEG:
        case MLIL_AND:
        case MLIL_OR:
        case MLIL_XOR:
        case MLIL_NOT:
        case MLIL_LSL:
        case MLIL_LSR:
            return true;
        default:
            return false;
    }
}

std::optional<ComparisonKind> ComparisonForOperation(BNMediumLevelILOperation operation) {
    switch (operation) {
        case MLIL_CMP_E:
            return ComparisonKind::Equal;
        case MLIL_CMP_NE:
            return ComparisonKind::NotEqual;
        case MLIL_CMP_ULT:
            return ComparisonKind::UnsignedLessThan;
        case MLIL_CMP_ULE:
            return ComparisonKind::UnsignedLessEqual;
        case MLIL_CMP_UGE:
            return ComparisonKind::UnsignedGreaterEqual;
        case MLIL_CMP_UGT:
            return ComparisonKind::UnsignedGreaterThan;
        case MLIL_CMP_SLT:
            return ComparisonKind::SignedLessThan;
        case MLIL_CMP_SLE:
            return ComparisonKind::SignedLessEqual;
        case MLIL_CMP_SGE:
            return ComparisonKind::SignedGreaterEqual;
        case MLIL_CMP_SGT:
            return ComparisonKind::SignedGreaterThan;
        default:
            return std::nullopt;
    }
}

bool IsRecoverableRoot(BNMediumLevelILOperation operation) {
    return IsCandidateRoot(operation) || ComparisonForOperation(operation).has_value();
}

const char* ComparisonText(ComparisonKind comparison) {
    switch (comparison) {
        case ComparisonKind::Equal: return "==";
        case ComparisonKind::NotEqual: return "!=";
        case ComparisonKind::UnsignedLessThan: return "u<";
        case ComparisonKind::UnsignedLessEqual: return "u<=";
        case ComparisonKind::UnsignedGreaterEqual: return "u>=";
        case ComparisonKind::UnsignedGreaterThan: return "u>";
        case ComparisonKind::SignedLessThan: return "s<";
        case ComparisonKind::SignedLessEqual: return "s<=";
        case ComparisonKind::SignedGreaterEqual: return "s>=";
        case ComparisonKind::SignedGreaterThan: return "s>";
    }
    return "?";
}

ExprId BuildMLIL(
    MediumLevelILFunction* function,
    const cobra::Expr& expression,
    const std::vector<std::string>& variables,
    const std::map<std::string, Variable>& variableByName,
    size_t size,
    const ILSourceLocation& location
) {
    auto child = [&](size_t index) {
        return BuildMLIL(
            function, *expression.children.at(index), variables, variableByName, size, location
        );
    };

    switch (expression.kind) {
        case cobra::Expr::Kind::kConstant:
            return function->Const(size, expression.constant_val, location);
        case cobra::Expr::Kind::kVariable: {
            const auto& name = variables.at(expression.var_index);
            return function->Var(size, variableByName.at(name), location);
        }
        case cobra::Expr::Kind::kAdd:
            return function->Add(size, child(0), child(1), location);
        case cobra::Expr::Kind::kMul:
            return function->Mult(size, child(0), child(1), location);
        case cobra::Expr::Kind::kAnd:
            return function->And(size, child(0), child(1), location);
        case cobra::Expr::Kind::kOr:
            return function->Or(size, child(0), child(1), location);
        case cobra::Expr::Kind::kXor:
            return function->Xor(size, child(0), child(1), location);
        case cobra::Expr::Kind::kNot:
            return function->Not(size, child(0), location);
        case cobra::Expr::Kind::kNeg:
            return function->Neg(size, child(0), location);
        case cobra::Expr::Kind::kShr:
            return function->LogicalShiftRight(
                size,
                child(0),
                function->Const(size, expression.constant_val, location),
                location
            );
    }
    throw std::runtime_error("unhandled CoBRA expression kind");
}

void IndexReachableExpressionTree(
    const MediumLevelILInstruction& expression,
    std::unordered_set<size_t>& reachable,
    std::unordered_set<size_t>& childOfSupportedRegion
) {
    reachable.insert(expression.exprIndex);
    for (const auto operand : expression.GetOperands()) {
        if (operand.GetType() == ExprMediumLevelOperand) {
            const auto child = operand.GetExpr();
            if (child.size == expression.size
                && IsCandidateRoot(expression.operation)
                && IsCandidateRoot(child.operation)) {
                childOfSupportedRegion.insert(child.exprIndex);
            }
            if (!reachable.contains(child.exprIndex)) {
                IndexReachableExpressionTree(
                    child, reachable, childOfSupportedRegion
                );
            }
        } else if (operand.GetType() == ExprListMediumLevelOperand) {
            for (const auto child : operand.GetExprList()) {
                if (child.size == expression.size
                    && IsCandidateRoot(expression.operation)
                    && IsCandidateRoot(child.operation)) {
                    childOfSupportedRegion.insert(child.exprIndex);
                }
                if (!reachable.contains(child.exprIndex)) {
                    IndexReachableExpressionTree(
                        child, reachable, childOfSupportedRegion
                    );
                }
            }
        }
    }
}

void MarkExpressionTree(
    const MediumLevelILInstruction& expression,
    std::unordered_set<size_t>& marked
) {
    if (!marked.insert(expression.exprIndex).second) {
        return;
    }
    for (const auto operand : expression.GetOperands()) {
        if (operand.GetType() == ExprMediumLevelOperand) {
            MarkExpressionTree(operand.GetExpr(), marked);
        } else if (operand.GetType() == ExprListMediumLevelOperand) {
            for (const auto child : operand.GetExprList()) {
                MarkExpressionTree(child, marked);
            }
        }
    }
}

struct PendingCandidate {
    CandidateReport report;
    size_t rootExpression = 0;
    size_t size = 0;
    uint32_t originalCost = 0;
    bool isPredicate = false;
    bool predicateValue = false;
    std::unique_ptr<cobra::Expr> simplified;
    std::vector<std::string> simplifiedVariables;
    std::map<std::string, Variable> variableByName;
};

bool IsNormalMLIL(MediumLevelILFunction* function) {
    if (!function) {
        return false;
    }
    auto normal = function->GetNonSSAForm();
    return normal && normal->GetObject() == function->GetObject();
}

std::vector<PendingCandidate> CollectCandidates(
    MediumLevelILFunction* function,
    const AnalysisLimits& limits,
    FunctionReport& output,
    bool allowSSAConstruction
) {
    std::vector<PendingCandidate> pending;
    if (!IsNormalMLIL(function)) {
        output.diagnostic = "normal (non-SSA) MLIL is required";
        return pending;
    }

    auto ssa = function->GetSSAForm();
    if (!ssa && allowSSAConstruction) {
        // The workflow is allowed to create an SSA cache for its own
        // analysis. Preview is intentionally not: it must be observational
        // even when a caller hands it a freshly constructed non-SSA MLIL.
        function->GenerateSSAForm();
        ssa = function->GetSSAForm();
    }
    if (!ssa) {
        output.diagnostic = allowSSAConstruction
            ? "SSA form is unavailable"
            : "SSA form is unavailable (preview does not construct it)";
        return pending;
    }

    // Only inspect expressions reachable from top-level instructions in
    // reachable MLIL blocks. GetExprCount() also contains orphaned pool
    // entries left by earlier IL construction, which must never become
    // rewrite roots.
    std::unordered_set<size_t> reachableSet;
    std::unordered_set<size_t> childOfSupportedRegion;
    for (const auto& block : function->GetBasicBlocks()) {
        for (size_t index = block->GetStart(); index < block->GetEnd(); ++index) {
            IndexReachableExpressionTree(
                function->GetInstruction(index),
                reachableSet,
                childOfSupportedRegion
            );
        }
    }
    std::vector<size_t> reachable(reachableSet.begin(), reachableSet.end());
    std::sort(reachable.begin(), reachable.end());

    size_t inspectedRoots = 0;
    for (const size_t expressionIndex : reachable) {
        if (expressionIndex >= function->GetExprCount()) {
            continue;
        }
        const auto nonSsaRoot = function->GetExpr(expressionIndex);
        if (childOfSupportedRegion.contains(expressionIndex)
            || !IsRecoverableRoot(nonSsaRoot.operation)
            || nonSsaRoot.size == 0
            || nonSsaRoot.size > 8) {
            continue;
        }
        if (++inspectedRoots > limits.maxCandidateRoots) {
            output.diagnostic = "candidate-root budget reached";
            break;
        }

        // Do not recover an expression unless the two IL views agree in both
        // directions. In particular, a one-way mapping can bind a non-SSA
        // root to a stale or folded SSA subexpression and turn a rewrite into
        // a silently different read of the current variable value.
        const size_t ssaExpressionIndex = function->GetSSAExprIndex(expressionIndex);
        if (ssaExpressionIndex >= ssa->GetExprCount()
            || ssa->GetNonSSAExprIndex(ssaExpressionIndex) != expressionIndex
            || function->GetSSAExprIndex(
                   ssa->GetNonSSAExprIndex(ssaExpressionIndex)
               ) != ssaExpressionIndex) {
            continue;
        }
        const auto ssaRoot = ssa->GetExpr(ssaExpressionIndex);
        ExpressionRecovery recovery(
            ssa, static_cast<uint32_t>(nonSsaRoot.size * 8), limits
        );

        SimplifyOptions options;
        options.bitWidth = static_cast<uint32_t>(nonSsaRoot.size * 8);
        options.maxVariables = static_cast<uint32_t>(limits.maxVariables);
        // There is no caller-controlled escape hatch here: BN rewrites are
        // authorized only by a successful SMT proof.
        options.requireZ3 = true;

        PendingCandidate candidate;
        candidate.rootExpression = expressionIndex;
        candidate.size = nonSsaRoot.size;
        candidate.report.expressionIndex = expressionIndex;
        candidate.report.address = nonSsaRoot.address;

        if (const auto comparison = ComparisonForOperation(nonSsaRoot.operation)) {
            candidate.isPredicate = true;
            auto recovered = recovery.RecoverComparison(ssaRoot);
            if (!recovered.predicateLeft || !recovered.predicateRight) {
                continue;
            }
            candidate.originalCost = static_cast<uint32_t>(std::min<size_t>(
                recovered.stats.nodes, std::numeric_limits<uint32_t>::max()
            ));
            candidate.report.before = cobra::Render(
                *recovered.predicateLeft, recovered.variables, options.bitWidth
            ) + " " + ComparisonText(*comparison) + " " + cobra::Render(
                *recovered.predicateRight, recovered.variables, options.bitWidth
            );
            auto result = ProveConstantComparison(
                *recovered.predicateLeft,
                *recovered.predicateRight,
                recovered.variables,
                options.bitWidth,
                *comparison,
                options
            );
            candidate.report.after = result.status == PredicateStatus::ProvedConstant
                ? (result.constantValue ? "1" : "0")
                : candidate.report.before;
            candidate.report.reason = std::move(result.reason);
            candidate.report.accepted =
                result.status == PredicateStatus::ProvedConstant && result.z3Verified;
            candidate.predicateValue = result.constantValue;
        } else {
            auto recovered = recovery.Recover(ssaRoot);
            if (!recovered.expression
                || recovered.stats.nodes < limits.minNodes
                || !recovered.stats.hasArithmetic
                || !recovered.stats.hasBitwise) {
                continue;
            }
            auto result = SimplifyExpression(
                *recovered.expression, recovered.variables, options
            );
            candidate.originalCost = result.originalCost;
            candidate.report.before = std::move(result.originalText);
            candidate.report.after = std::move(result.simplifiedText);
            candidate.report.reason = std::move(result.reason);
            candidate.report.accepted =
                result.status == SimplifyStatus::Simplified && result.z3Verified;
            if (candidate.report.accepted) {
                candidate.simplified = std::move(result.expression);
                candidate.simplifiedVariables = std::move(result.variables);
                candidate.variableByName = std::move(recovered.variableByName);
            }
        }
        if (candidate.report.accepted) {
            ++output.accepted;
        }
        pending.push_back(std::move(candidate));
    }

    std::sort(pending.begin(), pending.end(), [](const auto& left, const auto& right) {
        // A proved predicate replaces its complete operand tree. Give it the
        // first claim so accepted arithmetic descendants are correctly marked
        // covered. If it is not constant it is never selected, leaving those
        // descendants as independent ordinary CoBRA candidates.
        if (left.isPredicate != right.isPredicate) {
            return left.isPredicate;
        }
        if (left.originalCost != right.originalCost) {
            return left.originalCost > right.originalCost;
        }
        return left.rootExpression < right.rootExpression;
    });
    return pending;
}

void MoveReports(
    std::vector<PendingCandidate>& pending,
    FunctionReport& output
) {
    output.candidates.reserve(pending.size());
    for (auto& candidate : pending) {
        output.candidates.push_back(std::move(candidate.report));
    }
}

} // namespace

FunctionReport PreviewFunction(
    MediumLevelILFunction* function,
    const AnalysisLimits& limits
) {
    FunctionReport output;
    // Preview never calls GenerateSSAForm and never constructs a replacement
    // function. This is deliberately stronger than merely avoiding
    // ReplaceExpr: it leaves the caller's MLIL and analysis caches untouched.
    auto pending = CollectCandidates(function, limits, output, false);
    MoveReports(pending, output);
    return output;
}

FunctionReport TransformFunction(
    Ref<AnalysisContext> context,
    const AnalysisLimits& limits
) {
    FunctionReport output;
    if (!context) {
        output.diagnostic = "analysis context is unavailable";
        return output;
    }
    auto oldFunction = context->GetMediumLevelILFunction();
    // AnalysisContext supplies the normal MLIL form. The workflow may create
    // its transient SSA view to recover candidates, but all committed IL is
    // built below in a fresh non-SSA function.
    auto pending = CollectCandidates(oldFunction, limits, output, true);

    std::unordered_map<size_t, PendingCandidate*> selected;
    std::unordered_set<size_t> coveredExpressions;
    for (auto& candidate : pending) {
        if (!candidate.report.accepted) {
            continue;
        }
        if (coveredExpressions.contains(candidate.rootExpression)) {
            candidate.report.accepted = false;
            candidate.report.reason = "covered by a larger verified candidate";
            --output.accepted;
            continue;
        }
        auto root = oldFunction->GetExpr(candidate.rootExpression);
        MarkExpressionTree(root, coveredExpressions);
        selected.emplace(candidate.rootExpression, &candidate);
    }

    if (selected.empty()) {
        MoveReports(pending, output);
        return output;
    }

    Ref<MediumLevelILFunction> newFunction = new MediumLevelILFunction(
        oldFunction->GetArchitecture(),
        oldFunction->GetFunction(),
        context->GetLowLevelILFunction()
    );
    newFunction->PrepareToCopyFunction(oldFunction);

    std::unordered_set<size_t> appliedRoots;
    try {
        std::function<ExprId(const MediumLevelILInstruction&)> copyExpression;
        copyExpression = [&](const MediumLevelILInstruction& expression) -> ExprId {
            const auto found = selected.find(expression.exprIndex);
            if (found != selected.end()) {
                auto& candidate = *found->second;
                const ExprId replacement = candidate.isPredicate
                    // MLIL comparisons carry the operand width. Keep that
                    // exact width for 0/1 so consumers retain their original
                    // bitvector type rather than receiving a boolean-sized
                    // or architecture-default literal.
                    ? newFunction->Const(
                        candidate.size,
                        candidate.predicateValue ? 1 : 0,
                        ILSourceLocation(expression)
                    )
                    : BuildMLIL(
                        newFunction,
                        *candidate.simplified,
                        candidate.simplifiedVariables,
                        candidate.variableByName,
                        candidate.size,
                        ILSourceLocation(expression)
                    );
                if (appliedRoots.insert(expression.exprIndex).second) {
                    candidate.report.applied = true;
                    ++output.applied;
                }
                return replacement;
            }
            return expression.CopyTo(
                newFunction, copyExpression, ILSourceLocation(expression)
            );
        };

        for (const auto& oldBlock : oldFunction->GetBasicBlocks()) {
            newFunction->PrepareToCopyBlock(oldBlock);
            for (size_t index = oldBlock->GetStart(); index < oldBlock->GetEnd(); ++index) {
                const auto oldInstruction = oldFunction->GetInstruction(index);
                newFunction->SetCurrentAddress(
                    oldBlock->GetArchitecture(), oldInstruction.address
                );
                newFunction->AddInstruction(
                    copyExpression(oldInstruction), ILSourceLocation(oldInstruction)
                );
            }
        }
        newFunction->Finalize();
        newFunction->GenerateSSAForm();
    } catch (const std::exception& exception) {
        output.applied = 0;
        for (auto& candidate : pending) {
            candidate.report.applied = false;
        }
        output.diagnostic = std::string("copy transformation failed: ") + exception.what();
        MoveReports(pending, output);
        return output;
    }

    context->SetMediumLevelILFunction(newFunction);
    MoveReports(pending, output);
    return output;
}

bool AddActivityToWorkflow(Workflow* workflow, const std::string& anchor) {
    if (!workflow) {
        return false;
    }
    if (workflow->Contains(kActivityName)) {
        // A registered workflow cannot gain/reorder activities.  Only an
        // Activity object that this process registered is safe to refresh:
        // name equality alone could refer to another plugin's callback.
        auto activity = workflow->GetActivity(kActivityName);
        if (!activity) {
            return false;
        }
        std::shared_ptr<ActivityDispatchState> dispatch;
        {
            std::lock_guard lock(g_ownedActivitiesMutex);
            const auto found = g_ownedActivities.find(activity->GetObject());
            if (found == g_ownedActivities.end()) {
                return false;
            }
            dispatch = found->second.lock();
            if (!dispatch) {
                g_ownedActivities.erase(found);
                return false;
            }
        }
        const uint64_t generation = RefreshDispatch(dispatch);
        LogInfo(
            "[SMBA] refreshed owned MBA activity in %s (generation %llu)",
            workflow->GetName().c_str(),
            static_cast<unsigned long long>(generation)
        );
        return true;
    }
    if (workflow->IsRegistered() || !workflow->Contains(anchor)) {
        // A missing activity in a registered workflow (or a clone whose MLIL
        // anchor changed) must fail closed; the topology cannot be repaired
        // safely after registration.
        return false;
    }

    const std::string configuration = R"json({
        "name": "extension.smba.cobra.simplifyMlil",
        "title": "CoBRA MBA Simplification",
        "description": "Recover pure MLIL SSA expressions, simplify them with CoBRA, prove equivalence, and rewrite non-SSA MLIL.",
        "eligibility": {"auto": {"default": true}}
    })json";

    auto dispatch = std::make_shared<ActivityDispatchState>();
    const uint64_t generation = RefreshDispatch(dispatch);
    auto activity = workflow->RegisterActivity(
        configuration,
        [dispatch](Ref<AnalysisContext> context) { DispatchMBAActivity(dispatch, context); }
    );
    if (!activity || !workflow->InsertAfter(anchor, kActivityName)) {
        return false;
    }
    {
        std::lock_guard lock(g_ownedActivitiesMutex);
        g_ownedActivities[activity->GetObject()] = dispatch;
    }
    LogInfo(
        "[SMBA] registered owned MBA activity in %s (generation %llu)",
        workflow->GetName().c_str(),
        static_cast<unsigned long long>(generation)
    );
    return true;
}

} // namespace smba
