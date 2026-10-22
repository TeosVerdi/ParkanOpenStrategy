#include "ai/formula.hpp"

#include "core/bytes.hpp"

#include <cctype>
#include <cmath>
#include <cstdlib>

namespace parkan::ai {

namespace {

// Recursive-descent parser emitting RPN.
class Parser {
public:
    Parser(std::string_view s, const std::function<bool(std::string_view, std::uint32_t&)>& lookup)
        : s_(s), lookup_(lookup) {}

    Formula run() {
        Formula f;
        out_ = &f.rpn;
        parse_or();
        skip_ws();
        if (p_ != s_.size()) fail("unexpected character");
        return f;
    }

private:
    [[noreturn]] void fail(const char* what) {
        throw FormatError(std::string("formula '") + std::string(s_) + "': " + what);
    }
    void skip_ws() {
        while (p_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[p_]))) ++p_;
    }
    bool eat(char c) {
        skip_ws();
        if (p_ < s_.size() && s_[p_] == c) {
            ++p_;
            return true;
        }
        return false;
    }
    void emit(FormulaOp op) { out_->push_back({FormulaToken::Op, op}); }

    void parse_or() {
        parse_and();
        while (eat('|')) { parse_and(); emit(FormulaOp::Or); }
    }
    void parse_and() {
        parse_add();
        while (eat('&')) { parse_add(); emit(FormulaOp::And); }
    }
    void parse_add() {
        parse_mul();
        for (;;) {
            if (eat('+')) { parse_mul(); emit(FormulaOp::Add); }
            else if (eat('-')) { parse_mul(); emit(FormulaOp::Sub); }
            else return;
        }
    }
    void parse_mul() {
        parse_pow();
        for (;;) {
            if (eat('*')) { parse_pow(); emit(FormulaOp::Mul); }
            else if (eat('/')) { parse_pow(); emit(FormulaOp::Div); }
            else return;
        }
    }
    void parse_pow() {
        parse_unary();
        if (eat('^')) { parse_pow(); emit(FormulaOp::Pow); }
    }
    void parse_unary() {
        if (eat('-')) { parse_unary(); emit(FormulaOp::Neg); return; }
        if (eat('!')) { parse_unary(); emit(FormulaOp::Not); return; }
        skip_ws();
        // N, S, B, A are prefix functions only when followed by '(' (otherwise they start a name).
        if (p_ + 1 < s_.size() && std::string_view("NSBA").find(s_[p_]) != std::string_view::npos) {
            std::size_t q = p_ + 1;
            while (q < s_.size() && std::isspace(static_cast<unsigned char>(s_[q]))) ++q;
            if (q < s_.size() && s_[q] == '(') {
                char f = s_[p_++];
                parse_unary();
                emit(f == 'N' ? FormulaOp::Normalise : f == 'S' ? FormulaOp::Significate
                     : f == 'B' ? FormulaOp::Booleanise : FormulaOp::Abs);
                return;
            }
        }
        parse_primary();
    }
    void parse_primary() {
        skip_ws();
        if (eat('(')) {
            parse_or();
            if (!eat(')')) fail("missing ')'");
            return;
        }
        if (p_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[p_])) || s_[p_] == '.')) {
            std::string num;
            while (p_ < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[p_])) || s_[p_] == '.')) num += s_[p_++];
            char* end = nullptr;
            float v = num.starts_with("0x") || num.starts_with("0X") ? float(std::strtoul(num.c_str(), &end, 16))
                                                                      : std::strtof(num.c_str(), &end);
            if (!end || *end) fail("bad number");
            out_->push_back({FormulaToken::Const, {}, 0, v});
            return;
        }
        std::size_t b = p_;
        while (p_ < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[p_])) || s_[p_] == '_')) ++p_;
        if (b == p_) fail("expected a value");
        std::uint32_t id = 0;
        if (!lookup_(s_.substr(b, p_ - b), id)) fail("unknown variable");
        out_->push_back({FormulaToken::Var, {}, id, 0});
    }

    std::string_view s_;
    std::size_t p_ = 0;
    const std::function<bool(std::string_view, std::uint32_t&)>& lookup_;
    std::vector<FormulaToken>* out_ = nullptr;
};

// x87 __ftol: truncate toward zero.
std::int32_t ftol(float x) { return static_cast<std::int32_t>(static_cast<std::int64_t>(x)); }

} // namespace

Formula compile_formula(std::string_view text, const std::function<bool(std::string_view, std::uint32_t&)>& lookup) {
    return Parser(text, lookup).run();
}

bool Formula::evaluate(const std::function<float(std::uint32_t)>& var, float& out) const {
    std::vector<float> st;
    st.reserve(16);
    for (const auto& t : rpn) {
        if (t.kind == FormulaToken::Const) { st.push_back(t.value); continue; }
        if (t.kind == FormulaToken::Var) { st.push_back(var(t.var)); continue; }
        const bool binary = t.op <= FormulaOp::Or;
        // The original pops what it can: an empty stack leaves the operand at 0.
        float a = 0, b = 0; // a = top, b = the one below (b is the left operand)
        if (binary) {
            if (!st.empty()) { a = st.back(); st.pop_back(); }
            if (!st.empty()) { b = st.back(); st.pop_back(); }
        } else if (!st.empty()) {
            b = st.back();
            st.pop_back();
        }
        float r = 0;
        switch (t.op) {
        case FormulaOp::Add: r = b + a; break;
        case FormulaOp::Sub: r = b - a; break;
        case FormulaOp::Mul: r = b * a; break;
        case FormulaOp::Div: r = a != 0 ? b / a : 0; break;
        case FormulaOp::Pow: r = std::pow(b, a); break; // original calls 10021030 (unchecked: argument order)
        case FormulaOp::And: r = (ftol(a) != 0 && ftol(b) != 0) ? 1.f : 0.f; break;
        case FormulaOp::Or: r = (ftol(a) != 0 || ftol(b) != 0) ? 1.f : 0.f; break;
        case FormulaOp::Neg: r = -b; break;
        case FormulaOp::Not: r = ftol(b) == 0 ? 1.f : 0.f; break;
        case FormulaOp::Normalise: r = b > 1 ? 1.f : b < -1 ? 0.f : (b + 1) * 0.5f; break;
        case FormulaOp::Significate: r = b > 0 ? b : 0.f; break;
        case FormulaOp::Booleanise: r = b > 0 ? 1.f : 0.f; break;
        case FormulaOp::Abs: r = b > 0 ? b : -b; break;
        }
        st.push_back(r);
    }
    if (st.size() != 1) return false;
    out = st.front();
    return true;
}

} // namespace parkan::ai
