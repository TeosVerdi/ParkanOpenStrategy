// Mission-script interpreter. Spec: notes/script-vm.md.
// Original: ai.dll CScriptVM (RunEvent 10011f10, RunRecords 10011fb0, ExecRecord 10012020),
// variables ScriptVar::{Get,Set}{Float,Dword} (10013190 / 10013570 / 10013650 / 10013770).
#pragma once

#include "ai/formula.hpp"
#include "ai/script_program.hpp"

#include <array>

namespace parkan::ai {

// A script variable. Only the two kinds the shipped varsets use are modelled.
struct ScriptValue {
    VarType type = VarType::Float;
    union {
        float f;
        std::uint32_t u;
    };
    ScriptValue() : u(0) {}

    float get_float() const;          // DWORD reads as an unsigned integer
    std::uint32_t get_dword() const;  // float truncates toward zero (x87 __ftol)
    void set_float(float x);          // into a DWORD: truncates toward zero
    void set_dword(std::uint32_t x);  // into a float: (float)(unsigned)x
};

class ScriptVM;

// Implements the 73 handlers (notes/script-handlers-a.md, -b.md). A handler reads its operands from
// rec.refs (variable indices) through the VM and returns the raw 32-bit result.
class ScriptHost {
public:
    virtual ~ScriptHost() = default;
    virtual std::uint32_t call_handler(std::uint32_t index, ScriptVM& vm, const Record& rec) = 0;
};

class ScriptVM {
public:
    explicit ScriptVM(ScriptProgram program);

    const ScriptProgram& program() const { return program_; }
    ScriptValue& var(std::uint32_t i);
    std::optional<std::uint32_t> var_index(std::string_view name) const;

    // Runs an event to completion (STOP or the end of its records), following event jumps.
    // Returns false for an unknown event.
    bool run_event(std::uint32_t event, ScriptHost& host);

    // Deviation from the original, which has no limit: stop an event after this many records so a
    // script loop cannot hang the game. Large enough never to trigger on the shipped scripts.
    std::size_t record_budget = 1'000'000;
    bool budget_exhausted = false;

private:
    enum class Step { Next, Stop, JumpedEvent };
    Step exec(const Record& rec, ScriptHost& host);
    bool if_blocked() const { return depth_ > 0 && !if_flags_[depth_ - 1]; }

    ScriptProgram program_;
    std::vector<ScriptValue> vars_;
    std::vector<Formula> formulas_;
    std::uint32_t event_ = 0;
    std::uint32_t record_ = 0;
    // Original: flags at +0x4c, depth at +0x6c (32 bytes of room before the depth field).
    std::array<bool, 32> if_flags_{};
    std::uint32_t depth_ = 0;
};

} // namespace parkan::ai
