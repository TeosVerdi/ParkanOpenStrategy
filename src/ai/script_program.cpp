#include "ai/script_program.hpp"

#include "core/paths.hpp"

#include <algorithm>
#include <cctype>

namespace parkan::ai {

std::optional<std::uint32_t> ScriptProgram::find_event(std::string_view name) const {
    for (std::size_t i = 0; i < events.size(); ++i)
        if (events[i].name == name) return std::uint32_t(i);
    return std::nullopt;
}

std::vector<Event> parse_scr(std::span<const std::uint8_t> data) {
    Reader r(data);
    if (r.u32() != kHandlerCount) throw FormatError("script built for a different handler table");
    std::vector<Event> events(r.u32());
    for (auto& ev : events) {
        ev.name = r.str(r.u32());
        if (r.u8() != 0) throw FormatError("event name not NUL-terminated");
        ev.id = r.u32();
        ev.records.resize(r.u32());
        for (auto& rec : ev.records) {
            std::uint32_t w[6];
            for (auto& x : w) x = r.u32();
            rec.handler = w[0];
            rec.dest = w[1];
            rec.source = w[2];
            rec.op = static_cast<Op>(static_cast<std::int32_t>(w[3]));
            rec.cmp = w[4] <= 6 ? static_cast<Cmp>(w[4]) : Cmp::None;
            rec.refs.resize(w[5]);
            for (auto& ref : rec.refs) ref = r.u32();
            rec.formula = r.u32();
        }
    }
    if (!r.at_end()) throw FormatError("trailing bytes in script");
    return events;
}

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

// Splits the text between "KEYWORD(" and the matching ")" on top-level commas.
std::optional<std::vector<std::string_view>> call_args(std::string_view line, std::string_view kw) {
    line = trim(line);
    if (!line.starts_with(kw)) return std::nullopt;
    line.remove_prefix(kw.size());
    line = trim(line);
    if (line.empty() || line.front() != '(') return std::nullopt;
    auto close = line.rfind(')');
    if (close == std::string_view::npos) return std::nullopt;
    line = line.substr(1, close - 1);
    std::vector<std::string_view> out;
    int depth = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '(') ++depth;
        else if (line[i] == ')') --depth;
        else if (line[i] == ',' && depth == 0) {
            out.push_back(trim(line.substr(start, i - start)));
            start = i + 1;
        }
    }
    out.push_back(trim(line.substr(start)));
    return out;
}

template <class F>
void for_each_line(std::string_view text, F&& f) {
    while (!text.empty()) {
        auto nl = text.find('\n');
        auto line = text.substr(0, nl);
        if (auto c = line.find("//"); c != std::string_view::npos) line = line.substr(0, c);
        f(line);
        if (nl == std::string_view::npos) break;
        text.remove_prefix(nl + 1);
    }
}

} // namespace

std::vector<VarDecl> parse_varset(std::string_view text) {
    std::vector<VarDecl> vars;
    for_each_line(text, [&](std::string_view line) {
        if (auto a = call_args(line, "VAR"); a && a->size() >= 3) {
            VarDecl v;
            v.type = (*a)[0] == "DWORD" ? VarType::Dword : VarType::Float;
            v.name = (*a)[1];
            v.default_text = (*a)[2];
            vars.push_back(std::move(v));
        } else if (auto s = call_args(line, "STRING"); s && s->size() >= 3) {
            vars.push_back({VarType::String, std::string((*s)[1]), std::string((*s)[2])});
        }
    });
    return vars;
}

std::vector<std::string> parse_formulas(std::string_view text) {
    std::vector<std::string> out;
    for_each_line(text, [&](std::string_view line) {
        if (auto a = call_args(line, "FUNCTION"); a && a->size() >= 2) out.emplace_back((*a)[1]);
    });
    return out;
}

ScriptProgram load_script_bundle(const std::filesystem::path& dir, const std::string& name) {
    ScriptProgram p;
    auto scr = resolve_path(dir, name + ".scr");
    if (!scr) throw std::runtime_error("missing script " + name + ".scr");
    auto bytes = read_file(*scr);
    p.events = parse_scr(bytes);

    auto var = resolve_path(dir, name + ".var");
    if (!var) var = resolve_path(dir, "varset.var");
    if (var) {
        auto b = read_file(*var);
        p.vars = parse_varset({reinterpret_cast<const char*>(b.data()), b.size()});
    }
    if (auto fml = resolve_path(dir, name + ".fml")) {
        auto b = read_file(*fml);
        p.formulas = parse_formulas({reinterpret_cast<const char*>(b.data()), b.size()});
    }
    return p;
}

} // namespace parkan::ai
