// Mission-script bundle: <name>.scr (compiled flowchart), <name>.fml (formulas) and the variable
// set (<name>.var, falling back to varset.var). Spec: notes/script-vm.md. Original loader:
// ai.dll CScriptVM::LoadProgram / LoadVarset / LoadFormulas.
#pragma once

#include "core/bytes.hpp"

#include <optional>

namespace parkan::ai {

inline constexpr std::uint32_t kNone = 0xFFFFFFFF;
inline constexpr std::uint32_t kHandlerCount = 73; // checked by ai.dll ("Scripts are not up to date")

enum class Op : std::int32_t {
    Assign = -1,     // call handler w0, copy var w2, or evaluate formula w6, into var w1
    If = 0,          // var refs[0] <cmp> var refs[1]
    EndIf = 1,
    Nop = 2,
    Goto = 3,        // record refs[0]
    JumpEvent = 4,   // event refs[0]
    Stop = 5,
    SetImmediate = 6 // var w1 = w2
};

enum class Cmp : std::uint32_t { Less, Equal, Greater, LessEqual, GreaterEqual, NotEqual, None };

struct Record {
    std::uint32_t handler = kNone; // w0
    std::uint32_t dest = kNone;    // w1
    std::uint32_t source = kNone;  // w2: source variable, or immediate for SetImmediate
    Op op = Op::Assign;            // w3
    Cmp cmp = Cmp::None;           // w4
    std::vector<std::uint32_t> refs;
    std::uint32_t formula = kNone; // w6
};

struct Event {
    std::string name;
    std::uint32_t id = 0;
    std::vector<Record> records;
};

enum class VarType { Float, Dword, String };

struct VarDecl {
    VarType type = VarType::Float;
    std::string name;
    std::string default_text; // as written; evaluated by the VM
};

struct ScriptProgram {
    std::vector<Event> events;
    std::vector<VarDecl> vars;
    std::vector<std::string> formulas; // expression text of FUNCTION( , <expr>, )

    std::optional<std::uint32_t> find_event(std::string_view name) const;
};

std::vector<Event> parse_scr(std::span<const std::uint8_t> data);
std::vector<VarDecl> parse_varset(std::string_view text);
std::vector<std::string> parse_formulas(std::string_view text);

// Loads <dir>/<name>.scr/.fml and <name>.var or <dir>/varset.var.
ScriptProgram load_script_bundle(const std::filesystem::path& dir, const std::string& name);

} // namespace parkan::ai
