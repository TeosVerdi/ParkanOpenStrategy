#include "ui/campaign.hpp"

#include "core/bytes.hpp"
#include "core/paths.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace parkan::ui {

namespace {

// `descr`'s first line, as the dispatcher's getline(…, 0x80, '\n') reads it.
std::optional<std::string> read_title(const std::filesystem::path& game_dir, const std::string& rel) {
    const auto p = resolve_path(game_dir, rel);
    if (!p || !std::filesystem::is_regular_file(*p)) return std::nullopt;
    const Bytes b = read_file(*p);
    std::string t(reinterpret_cast<const char*>(b.data()), b.size());
    if (auto nl = t.find('\n'); nl != std::string::npos) t.resize(nl);
    if (t.size() > 127) t.resize(127);
    if (!t.empty() && t.back() == '\r') t.pop_back(); // port: a CRLF file would keep the CR in the original
    return t;
}

std::string two_digits(int n) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%02d", n);
    return buf;
}

bool iequal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

// The `NN` after the last '.' of `s`, or −1.
int number_after_last_dot(std::string_view s) {
    const auto d = s.rfind('.');
    if (d == std::string_view::npos || d + 2 >= s.size() || !std::isdigit((unsigned char)s[d + 1]) ||
        !std::isdigit((unsigned char)s[d + 2]))
        return -1;
    return (s[d + 1] - '0') * 10 + (s[d + 2] - '0');
}

} // namespace

std::vector<Campaign> scan_campaigns(const std::filesystem::path& game_dir) {
    std::vector<Campaign> out;
    for (int c = 0; c < 100; ++c) {
        Campaign cp;
        cp.path = "missions/campaign/campaign." + two_digits(c); // "%s%02d"
        const auto title = read_title(game_dir, cp.path + "/descr");
        if (!title) break;
        cp.title = *title;
        for (int m = 1; m < 100; ++m) {
            CampaignMission mi;
            mi.path = cp.path + "/mission." + two_digits(m) + "/"; // "%s%02d/"
            const auto t = read_title(game_dir, mi.path + "descr");
            if (!t) break;
            mi.title = *t;
            cp.missions.push_back(std::move(mi));
        }
        out.push_back(std::move(cp));
    }
    return out;
}

CampaignProgress parse_dispatcher_ini(std::string_view text) {
    CampaignProgress p;
    std::string section;
    while (!text.empty()) {
        const auto nl = text.find('\n');
        std::string_view line = trim(text.substr(0, nl));
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        if (line.empty() || line.front() == ';') continue;
        if (line.front() == '[') {
            const auto e = line.find(']');
            section = std::string(line.substr(1, e == std::string_view::npos ? std::string_view::npos : e - 1));
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string_view key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        const int v = std::atoi(std::string(value).c_str());
        if (iequal(section, "COMMON") && iequal(key, "ALL_AVAILABLE")) p.all_available = v == 1; // +0x24 == 1
        else if (iequal(section, "COMPLETE") && v != 0) {
            std::string k(key);
            for (char& ch : k) ch = char(std::tolower((unsigned char)ch)); // [inferred] keys compare case-blind
            p.complete.insert(k);
        }
    }
    return p;
}

std::string mission_key(std::string_view path) {
    std::string k(path);
    for (char& c : k)
        if (!std::isalnum((unsigned char)c)) c = '_';
    return k;
}

std::string set_mission_complete(std::string ini, std::string_view mission_path) {
    const std::string key = mission_key(mission_path), entry = key + "=1";
    // Find the [COMPLETE] section and, inside it, the key.
    std::size_t pos = 0, section_end = std::string::npos;
    bool in_section = false;
    while (pos <= ini.size()) {
        const std::size_t nl = ini.find('\n', pos), end = nl == std::string::npos ? ini.size() : nl;
        const std::string_view line = trim(std::string_view(ini).substr(pos, end - pos));
        if (!line.empty() && line.front() == '[') {
            if (in_section) break; // the next section starts: append before it
            const auto e = line.find(']');
            in_section = iequal(line.substr(1, e == std::string_view::npos ? std::string_view::npos : e - 1), "COMPLETE");
            if (in_section) section_end = nl == std::string::npos ? ini.size() : nl + 1;
        } else if (in_section) {
            if (!line.empty()) section_end = nl == std::string::npos ? ini.size() : nl + 1;
            const auto eq = line.find('=');
            if (eq != std::string_view::npos && iequal(trim(line.substr(0, eq)), key)) {
                std::size_t line_end = end;
                if (line_end > pos && ini[line_end - 1] == '\r') --line_end;
                ini.replace(pos, line_end - pos, entry);
                return ini;
            }
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    if (section_end == std::string::npos) {
        if (!ini.empty() && ini.back() != '\n') ini += "\r\n";
        return ini + "[COMPLETE]\r\n" + entry + "\r\n";
    }
    std::string insert = entry + "\r\n";
    if (section_end > 0 && ini[section_end - 1] != '\n') insert = "\r\n" + insert;
    ini.insert(section_end, insert);
    return ini;
}

void apply_progress(std::vector<Campaign>& campaigns, const CampaignProgress& p) {
    bool campaign_available = true;
    for (Campaign& c : campaigns) {
        c.available = campaign_available;
        // The shell only opens the mission list of an available campaign; the port folds that in.
        bool mission_available = c.available, all_complete = true;
        for (CampaignMission& m : c.missions) {
            m.available = mission_available;
            m.complete = m.available && (p.all_available || p.complete.count(mission_key(m.path)));
            all_complete = all_complete && m.complete;
            mission_available = m.complete;
        }
        // BuildCampaignList: ALL_AVAILABLE makes an available campaign complete without looking inside; an
        // empty campaign counts as complete.
        c.complete = c.available && (p.all_available || all_complete);
        campaign_available = c.complete;
    }
}

std::optional<std::string> next_mission(const std::vector<Campaign>& campaigns, std::string_view path) {
    const std::string key = mission_key(path);
    bool found = false;
    for (const Campaign& c : campaigns)
        for (const CampaignMission& m : c.missions) {
            if (found) return m.available ? std::optional<std::string>(m.path) : std::nullopt;
            found = iequal(mission_key(m.path), key);
        }
    return std::nullopt;
}

ShellTarget shell_target_after_game(std::string_view path, int mode, bool new_mission, int game_type) {
    ShellTarget t; // the main menu is shown first in every case
    const auto contains = [&](std::string_view needle) { return path.find(needle) != std::string_view::npos; };
    if (!new_mission) { // loaded from a save: the load form, whatever the mode
        t.form = ShellTarget::LoadGame;
        return t;
    }
    if (mode == 3) {
        if (game_type == 1) t.form = ShellTarget::LoadGame;
        return t;
    }
    if (mode != 1 || (game_type != 1 && game_type != 4)) return t;
    // CShell_OpenCampaignMissionList 10009b70: "…campaign.CC/mission.MM/" → campaign CC, list row MM − 1.
    if (contains("campaign") || contains("CAMPAIGN")) {
        std::string s(path);
        const int m = number_after_last_dot(s);
        if (auto d = s.rfind('.'); d != std::string::npos) s.resize(d);
        t.form = ShellTarget::CampaignMissionList, t.campaign = number_after_last_dot(s), t.mission = m < 0 ? -1 : m - 1;
    } else if (contains("single") || contains("SINGLE")) {
        t.form = ShellTarget::SingleList, t.mission = number_after_last_dot(path); // 10009ea0, not traced
        if (t.mission > 0) --t.mission;
    }
    return t;
}

} // namespace parkan::ui
