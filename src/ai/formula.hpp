// Script formulas (.fml): infix text compiled to RPN once, evaluated in float.
// Original: ai.dll ScriptFormula::Evaluate (10015b30), operator table at 10047d70.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace parkan::ai {

enum class FormulaOp : std::uint8_t {
    Add, Sub, Mul, Div, Pow, And, Or,          // binary
    Neg, Not, Normalise, Significate, Booleanise, Abs // unary
};

struct FormulaToken {
    enum Kind : std::uint8_t { Const, Var, Op } kind;
    FormulaOp op{};
    std::uint32_t var = 0;
    float value = 0;
};

struct Formula {
    std::vector<FormulaToken> rpn;
    // Returns false (like the original) when the expression leaves other than one value.
    bool evaluate(const std::function<float(std::uint32_t)>& var, float& out) const;
};

// Compiles `text`. Names resolve through `lookup` (returns false for unknown names).
// Precedence, low to high: | & + - * / ^ unary (inferred: the original compiler in
// CScriptVM::LoadFormulas is not traced yet). The shipped data only uses + - * and parentheses.
Formula compile_formula(std::string_view text, const std::function<bool(std::string_view, std::uint32_t&)>& lookup);

} // namespace parkan::ai
