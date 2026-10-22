#include "ai/script_vm.hpp"

#include <bit>
#include <cstdlib>

namespace parkan::ai {

namespace {
std::uint32_t ftol(float x) { return static_cast<std::uint32_t>(static_cast<std::int64_t>(x)); }
} // namespace

float ScriptValue::get_float() const { return type == VarType::Float ? f : static_cast<float>(u); }
std::uint32_t ScriptValue::get_dword() const { return type == VarType::Float ? ftol(f) : u; }
void ScriptValue::set_float(float x) {
    if (type == VarType::Float) f = x;
    else u = ftol(x);
}
void ScriptValue::set_dword(std::uint32_t x) {
    if (type == VarType::Float) f = static_cast<float>(x);
    else u = x;
}

ScriptVM::ScriptVM(ScriptProgram program) : program_(std::move(program)) {
    vars_.resize(program_.vars.size());
    for (std::size_t i = 0; i < vars_.size(); ++i) {
        const auto& d = program_.vars[i];
        auto& v = vars_[i];
        v.type = d.type;
        const std::string& t = d.default_text;
        if (d.type == VarType::Float) v.f = std::strtof(t.c_str(), nullptr);
        else if (d.type == VarType::Dword) v.u = static_cast<std::uint32_t>(std::strtoll(t.c_str(), nullptr, 0));
    }
    auto lookup = [this](std::string_view name, std::uint32_t& id) {
        if (auto i = var_index(name)) {
            id = *i;
            return true;
        }
        return false;
    };
    formulas_.reserve(program_.formulas.size());
    for (const auto& text : program_.formulas) formulas_.push_back(compile_formula(text, lookup));
}

ScriptValue& ScriptVM::var(std::uint32_t i) {
    if (i >= vars_.size()) throw FormatError("script variable index out of range"); // original throws too
    return vars_[i];
}

std::optional<std::uint32_t> ScriptVM::var_index(std::string_view name) const {
    for (std::size_t i = 0; i < program_.vars.size(); ++i)
        if (program_.vars[i].name == name) return std::uint32_t(i);
    return std::nullopt;
}

bool ScriptVM::run_event(std::uint32_t event, ScriptHost& host) {
    if (event >= program_.events.size()) return false;
    event_ = event;
    budget_exhausted = false;
    std::size_t budget = record_budget;
    // RunEvent → RunRecords: each pass resets the IF stack and starts at record 0.
    for (;;) {
        depth_ = 0;
        record_ = 0;
        bool jumped = false;
        while (record_ < program_.events[event_].records.size()) {
            if (budget-- == 0) {
                budget_exhausted = true;
                return true;
            }
            Step s = exec(program_.events[event_].records[record_], host);
            if (s == Step::Stop) return true;
            if (s == Step::JumpedEvent) {
                jumped = true;
                break;
            }
            ++record_;
        }
        if (!jumped) return true;
    }
}

ScriptVM::Step ScriptVM::exec(const Record& rec, ScriptHost& host) {
    switch (rec.op) {
    case Op::If: {
        const auto& a = var(rec.refs.at(0));
        const auto& b = var(rec.refs.at(1));
        bool r = false;
        auto test = [&](auto x, auto y) {
            switch (rec.cmp) {
            case Cmp::Less: return x < y;
            case Cmp::Equal: return x == y;
            case Cmp::Greater: return x > y;
            case Cmp::LessEqual: return x <= y;
            case Cmp::GreaterEqual: return x >= y;
            case Cmp::NotEqual: return x != y;
            case Cmp::None: return false;
            }
            return false;
        };
        // The left operand's kind decides: DWORD compares unsigned, anything else as float.
        if (a.type == VarType::Dword) r = test(a.get_dword(), b.get_dword());
        else r = test(a.get_float(), b.get_float());
        if (if_blocked()) r = false;
        if (depth_ < if_flags_.size()) if_flags_[depth_] = r;
        ++depth_;
        return Step::Next;
    }
    case Op::EndIf:
        if (depth_ > 0) --depth_;
        return Step::Next;
    case Op::Nop:
        return Step::Next;
    case Op::Goto:
        if (if_blocked()) return Step::Next;
        depth_ = 0;
        record_ = rec.refs.at(0) - 1; // incremented after this record
        return Step::Next;
    case Op::JumpEvent:
        if (if_blocked()) return Step::Next;
        if (rec.refs.at(0) >= program_.events.size()) throw FormatError("jump to unknown event");
        event_ = rec.refs.at(0);
        return Step::JumpedEvent;
    case Op::Stop:
        return if_blocked() ? Step::Next : Step::Stop;
    case Op::SetImmediate: {
        // Runs even inside a false IF: the original (100121e2) never consults the IF stack here.
        auto& d = var(rec.dest);
        if (d.type == VarType::Dword) d.set_dword(rec.source);
        else d.set_float(std::bit_cast<float>(rec.source)); // raw bits (unused by the shipped data)
        return Step::Next;
    }
    case Op::Assign:
        // Assign is skipped inside a false IF; unknown ops run unconditionally (original quirk).
        if (if_blocked()) return Step::Next;
        break;
    default:
        break;
    }

    if (rec.handler == kNone) {
        if (rec.source != kNone) {
            var(rec.dest) = var(rec.source); // full copy, kind included (ScriptVar::Assign)
            return Step::Next;
        }
        float r = 0;
        if (rec.formula >= formulas_.size()) throw FormatError("formula index out of range");
        formulas_[rec.formula].evaluate([this](std::uint32_t i) { return var(i).get_float(); }, r);
        var(rec.dest).set_float(r);
        return Step::Next;
    }
    // The result cell is cleared before every call, so a handler that writes nothing yields 0.
    const std::uint32_t raw = host.call_handler(rec.handler, *this, rec);
    if (rec.dest == kNone) return Step::Next;
    auto& d = var(rec.dest);
    if (d.type == VarType::Float) d.set_float(std::bit_cast<float>(raw)); // raw bits into a float variable
    else d.set_dword(raw);
    return Step::Next;
}

} // namespace parkan::ai
