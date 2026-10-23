// The first-run data launcher (port addition; see app/shell_app.hpp). A plain look on purpose: SDL's
// built-in 8×8 debug font and flat quads, since no game data (and so no game font) may exist yet.
#include "app/shell_app.hpp"

#include "install/install.hpp"
#include "shell/ini.hpp"

#include <atomic>
#include <fstream>
#include <mutex>
#include <optional>
#include <thread>

namespace parkan {

namespace {

std::string read_all(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

std::string describe(const install::Report& r) {
    if (!r.readable()) return "cannot read: " + r.error;
    std::string s = install::edition_info(r.edition).name;
    s += " - " + std::to_string(r.present) + "/" + std::to_string(r.expected) + " files";
    std::size_t missing = 0, damaged = 0;
    for (const auto& i : r.issues)
        if (!i.user_file) (i.kind == install::FileIssue::Kind::Missing ? missing : damaged) += 1;
    if (missing) s += ", " + std::to_string(missing) + " missing";
    if (damaged) s += ", " + std::to_string(damaged) + " damaged";
    if (r.complete()) s += " - OK";
    return s;
}

struct Candidate {
    std::filesystem::path path;
    install::Report report;
};

// Dialog results arrive on another thread.
struct DialogResult {
    std::mutex mu;
    std::optional<std::filesystem::path> path;
    bool image = false;
};

void SDLCALL on_dialog(void* user, const char* const* files, int) {
    auto* d = static_cast<DialogResult*>(user);
    if (!files || !files[0]) return;
    std::lock_guard lk(d->mu);
    d->path = std::filesystem::path(files[0]);
}

} // namespace

std::filesystem::path remembered_data_dir(const std::filesystem::path& pref) {
    if (pref.empty()) return {};
    const std::string t = read_all(pref / "launcher.ini");
    const auto v = shell::ini_get(t, "launcher", "data");
    if (!v || v->empty()) return {};
    const std::filesystem::path p = *v;
    const install::Report r = install::detect(p);
    return r.complete() ? p : std::filesystem::path();
}

void remember_data_dir(const std::filesystem::path& pref, const std::filesystem::path& dir) {
    if (pref.empty()) return;
    std::string t = read_all(pref / "launcher.ini");
    t = shell::ini_set(t, "launcher", "data", dir.string());
    std::ofstream(pref / "launcher.ini", std::ios::binary) << t;
}

std::filesystem::path run_data_launcher(SDL_Window* window, const std::filesystem::path& pref, const std::filesystem::path& candidate,
                                        const std::string& shot_path) {
    SDL_Renderer* r = SDL_CreateRenderer(window, nullptr);
    if (!r) return {};
    SDL_SetRenderVSync(r, 1);
    std::vector<Candidate> cands;
    auto add_candidate = [&](const std::filesystem::path& p) {
        for (const auto& c : cands)
            if (c.path == p) return;
        std::error_code ec;
        if (p.empty() || !std::filesystem::exists(p, ec)) return;
        cands.push_back({p, install::detect(p)});
    };
    for (const auto& g : install::list_imported(pref)) add_candidate(g.dir);
    add_candidate(candidate);
    // The other GOG installs next to it (the RU and ENG releases share a parent folder).
    if (std::error_code ec; !candidate.empty() && std::filesystem::is_directory(candidate.parent_path(), ec))
        for (const auto& e : std::filesystem::directory_iterator(candidate.parent_path(), ec))
            if (e.is_directory() && e.path().filename().string().rfind("Parkan", 0) == 0) add_candidate(e.path());
    std::string status = "Choose the game's data: an installed game folder, or the game's disc image.";
    DialogResult dialog;
    // Import state (worker thread).
    std::thread worker;
    std::atomic<bool> importing{false}, cancel{false}, import_done{false};
    std::mutex prog_mu;
    install::ImportProgress progress;
    install::ImportResult import_result;
    std::filesystem::path import_target;

    std::filesystem::path chosen;
    bool running = true;
    float mx = -1, my = -1;
    auto button = [&](float x, float y, float w, float h, const std::string& label, bool clicked, bool enabled = true) {
        const bool hover = mx >= x && mx < x + w && my >= y && my < y + h;
        SDL_SetRenderDrawColor(r, enabled ? (hover ? 70 : 45) : 30, enabled ? (hover ? 110 : 80) : 30, enabled ? (hover ? 70 : 50) : 30, 255);
        const SDL_FRect rc{x, y, w, h};
        SDL_RenderFillRect(r, &rc);
        SDL_SetRenderDrawColor(r, 200, 255, 200, 255);
        SDL_RenderDebugText(r, x + 8, y + (h - 8) / 2, label.c_str());
        return enabled && hover && clicked;
    };
    while (running && chosen.empty()) {
        bool clicked = false;
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) running = false;
            if (ev.type == SDL_EVENT_MOUSE_MOTION) mx = ev.motion.x, my = ev.motion.y;
            if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev.button.button == SDL_BUTTON_LEFT) mx = ev.button.x, my = ev.button.y, clicked = true;
        }
        // A dialog answered: detect the folder or the image.
        std::optional<std::filesystem::path> picked;
        {
            std::lock_guard lk(dialog.mu);
            picked = std::exchange(dialog.path, std::nullopt);
        }
        if (picked) {
            install::Report rep = install::detect(*picked);
            status = picked->filename().string() + ": " + describe(rep);
            if (dialog.image && rep.readable() && rep.edition != install::Edition::Unknown) {
                import_target = install::edition_dir(pref, rep.edition);
                importing = true, cancel = false, import_done = false;
                worker = std::thread([&, src = *picked] {
                    import_result = install::import_game(src, import_target, [&](const install::ImportProgress& p) {
                        std::lock_guard lk(prog_mu);
                        progress = p;
                        return !cancel.load();
                    });
                    import_done = true;
                });
            } else if (!dialog.image && rep.complete()) {
                chosen = *picked;
            } else {
                cands.push_back({*picked, rep});
            }
        }
        if (import_done) {
            worker.join();
            importing = false, import_done = false;
            if (import_result.ok) {
                status = "Imported " + std::to_string(import_result.files_written) + " files, " + std::to_string(import_result.tracks_written) +
                         " music tracks.";
                cands.push_back({import_target, install::detect(import_target)});
                if (cands.back().report.complete()) chosen = import_target;
            } else {
                status = import_result.cancelled ? "Import cancelled." : "Import failed: " + import_result.error;
            }
        }

        int ww = 0, wh = 0;
        SDL_GetWindowSize(window, &ww, &wh);
        float px = 1;
        if (int ow = 0, oh = 0; SDL_GetCurrentRenderOutputSize(r, &ow, &oh) && ww > 0) px = float(ow) / float(ww);
        SDL_SetRenderScale(r, px * 1.5f, px * 1.5f);
        const float sw = float(ww) / 1.5f;
        SDL_SetRenderDrawColor(r, 12, 16, 12, 255);
        SDL_RenderClear(r);
        SDL_SetRenderDrawColor(r, 120, 255, 120, 255);
        SDL_RenderDebugText(r, 16, 16, "Parkan: Iron Strategy - game data (port launcher)");
        SDL_SetRenderDrawColor(r, 180, 180, 180, 255);
        SDL_RenderDebugText(r, 16, 36, status.c_str());
        float y = 60;
        for (const auto& c : cands) {
            std::string where = c.path.string();
            const std::size_t room = std::size_t(std::max(20.f, (sw - 48) / 8.f)) - std::min<std::size_t>(describe(c.report).size() + 4, 60);
            if (where.size() > room) where = "..." + where.substr(where.size() - room + 3);
            const std::string label = describe(c.report) + "  -  " + where;
            if (button(16, y, sw - 32, 22, label, clicked && !importing, c.report.complete())) chosen = c.path;
            y += 28;
        }
        y += 10;
        if (!importing) {
            if (button(16, y, 260, 26, "Installed game folder...", clicked)) {
                dialog.image = false;
                SDL_ShowOpenFolderDialog(on_dialog, &dialog, window, nullptr, false);
            }
            if (button(290, y, 260, 26, "Disc image (MDS/MDF/ISO/CUE)...", clicked)) {
                static const SDL_DialogFileFilter filters[] = {{"Disc images", "mds;mdf;iso;cue;bin"}, {"All files", "*"}};
                dialog.image = true;
                SDL_ShowOpenFileDialog(on_dialog, &dialog, window, filters, 2, nullptr, false);
            }
        } else {
            install::ImportProgress p;
            {
                std::lock_guard lk(prog_mu);
                p = progress;
            }
            const float k = p.bytes_total ? float(double(p.bytes_done) / double(p.bytes_total)) : 0.f;
            SDL_SetRenderDrawColor(r, 40, 40, 40, 255);
            SDL_FRect bg{16, y, sw - 140, 22};
            SDL_RenderFillRect(r, &bg);
            SDL_SetRenderDrawColor(r, 60, 200, 60, 255);
            SDL_FRect fg{16, y, (sw - 140) * k, 22};
            SDL_RenderFillRect(r, &fg);
            SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
            SDL_RenderDebugText(r, 20, y + 7, (std::to_string(int(k * 100)) + "%  " + p.item).c_str());
            if (button(sw - 110, y, 94, 22, "Cancel", clicked)) cancel = true;
        }
        if (!shot_path.empty()) {
            // --launcher --shot: this first frame to a BMP, then leave.
            SDL_Surface* surf = SDL_RenderReadPixels(r, nullptr);
            SDL_Log("launcher screenshot %s: %s", shot_path.c_str(), surf && SDL_SaveBMP(surf, shot_path.c_str()) ? "saved" : SDL_GetError());
            if (surf) SDL_DestroySurface(surf);
            break;
        }
        SDL_RenderPresent(r);
    }
    if (worker.joinable()) {
        cancel = true;
        worker.join();
    }
    SDL_DestroyRenderer(r);
    if (!chosen.empty()) remember_data_dir(pref, chosen);
    return chosen;
}

} // namespace parkan
