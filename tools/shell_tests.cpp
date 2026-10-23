// Shell tests (notes/shell.md): the intro videos (AVI container, Microsoft Video 1), and — on a game
// folder — every intro decoded frame by frame. Linked into parkan-tests.
#include "core/bytes.hpp"
#include "core/paths.hpp"
#include "res/avi.hpp"
#include "res/msvideo1.hpp"
#include "res/swav.hpp"
#include "shell/briefing.hpp"
#include "shell/ini.hpp"
#include "shell/shell.hpp"
#include "res/textres.hpp"
#include "ui/campaign.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

using namespace parkan;

namespace {

int g_fail = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            ++g_fail;                                                               \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                           \
    } while (0)

std::uint64_t fnv1a(const Bytes& b) {
    std::uint64_t h = 1469598103934665603ull;
    for (std::uint8_t c : b) h = (h ^ c) * 1099511628211ull;
    return h;
}

void put16(Bytes& b, std::uint16_t v) { b.push_back(std::uint8_t(v)), b.push_back(std::uint8_t(v >> 8)); }
void put32(Bytes& b, std::uint32_t v) { put16(b, std::uint16_t(v)), put16(b, std::uint16_t(v >> 16)); }
void put4(Bytes& b, const char* s) { b.insert(b.end(), s, s + 4); }
Bytes chunk(const char* id, const Bytes& body) {
    Bytes b;
    put4(b, id);
    put32(b, std::uint32_t(body.size()));
    b.insert(b.end(), body.begin(), body.end());
    if (body.size() & 1) b.push_back(0);
    return b;
}
Bytes list(const char* type, const Bytes& body, const char* id = "LIST") {
    Bytes inner;
    put4(inner, type);
    inner.insert(inner.end(), body.begin(), body.end());
    return chunk(id, inner);
}
Bytes cat(std::initializer_list<Bytes> parts) {
    Bytes b;
    for (const Bytes& p : parts) b.insert(b.end(), p.begin(), p.end());
    return b;
}

// A 8×4 RGB555 movie: frame 0 fills block 0 red and gives block 1 a 2-colour pattern; frame 1 skips both.
Bytes synthetic_avi() {
    Bytes avih;
    for (std::uint32_t v : {40000u, 0u, 0u, 0x10u, 2u, 0u, 2u, 0u, 8u, 4u, 0u, 0u, 0u, 0u}) put32(avih, v);
    Bytes strh;
    put4(strh, "vids"), put4(strh, "msvc");
    for (std::uint32_t v : {0u, 0u, 0u, 1u, 25u, 0u, 2u, 0u, 0u, 0u, 0u, 0u}) put32(strh, v);
    Bytes strf;
    put32(strf, 40), put32(strf, 8), put32(strf, 4), put16(strf, 1), put16(strf, 16), put4(strf, "CRAM");
    for (int i = 0; i < 5; ++i) put32(strf, 0);
    Bytes ash;
    put4(ash, "auds"), put32(ash, 0);
    for (std::uint32_t v : {0u, 0u, 0u, 4u, 88200u, 0u, 3u, 0u, 0u, 4u}) put32(ash, v);
    Bytes asf;
    put16(asf, 1), put16(asf, 2), put32(asf, 22050), put32(asf, 88200), put16(asf, 4), put16(asf, 16);
    Bytes f0; // block 0: fill 0x7C00 (red); block 1: mask 0x00FF, colours green / blue
    put16(f0, 0x7C00 | 0x8000), put16(f0, 0x00FF), put16(f0, 0x03E0), put16(f0, 0x001F), put16(f0, 0);
    Bytes f1;
    put16(f1, 0x8400 | 2); // skip 2 blocks
    Bytes pcm;
    for (int i = 0; i < 6; ++i) put16(pcm, std::uint16_t(i * 100));
    const Bytes hdrl = list("hdrl", cat({chunk("avih", avih), list("strl", cat({chunk("strh", strh), chunk("strf", strf)})),
                                         list("strl", cat({chunk("strh", ash), chunk("strf", asf)}))}));
    const Bytes movi = list("movi", cat({chunk("01wb", pcm), list("rec ", cat({chunk("00dc", f0)})), chunk("JUNK", Bytes(3)),
                                         chunk("00dc", f1)}));
    return list("AVI ", cat({hdrl, chunk("JUNK", Bytes(5)), movi, chunk("idx1", Bytes(16))}), "RIFF");
}

void test_avi_synthetic() {
    const Bytes file = synthetic_avi();
    const AviFile a = parse_avi(file);
    CHECK(a.width == 8 && a.height == 4 && a.total_frames == 2 && a.us_per_frame == 40000 && a.has_index);
    CHECK(a.video_stream == 0 && a.audio_stream == 1 && a.streams.size() == 2 && a.fps() == 25);
    CHECK(a.video.width == 8 && a.video.height == 4 && a.video.bit_count == 16 && a.video.compression == "CRAM");
    CHECK(a.audio.format_tag == 1 && a.audio.channels == 2 && a.audio.sample_rate == 22050 && a.audio.bits == 16);
    CHECK(a.frame_count() == 2 && a.audio_chunks.size() == 1);
    const auto pcm = avi_audio_pcm16(a, file);
    CHECK(pcm.size() == 6 && pcm[5] == 500);

    MsVideo1Decoder d(8, 4, 16);
    d.decode(std::span(file).subspan(a.video_chunks[0].offset, a.video_chunks[0].size));
    Bytes rgba;
    d.to_rgba(rgba);
    auto px = [&](int x, int y) { return &rgba[(std::size_t(y) * 8 + std::size_t(x)) * 4]; };
    CHECK(px(0, 0)[0] == 255 && px(0, 0)[1] == 0 && px(3, 3)[0] == 255);
    // Mask 0x00FF: bits 0..7 = the block's two bottom rows (DIB order) → colour 0 (green); top rows colour 1.
    CHECK(px(4, 3)[1] == 255 && px(4, 3)[2] == 0 && px(7, 2)[1] == 255);
    CHECK(px(4, 0)[2] == 255 && px(4, 0)[1] == 0 && px(7, 1)[2] == 255);
    const auto before = rgba;
    d.decode(std::span(file).subspan(a.video_chunks[1].offset, a.video_chunks[1].size));
    d.to_rgba(rgba);
    CHECK(rgba == before); // skipped blocks keep their pixels

    // Eight colours: bit 15 of colour 0 set; quadrant (x & 2) + ((y & 2) << 1) selects the pair.
    Bytes f8;
    put16(f8, 0x7FFF); // pixel 15 (top right) clear → colour 1 of its pair
    const std::uint16_t c[8] = {0x8000 | 0x7C00, 0x0000, 0x03E0, 0x0000, 0x001F, 0x0000, 0x7FFF, 0x0000};
    for (std::uint16_t v : c) put16(f8, v);
    MsVideo1Decoder e(4, 4, 16);
    e.decode(f8);
    const auto& p = e.pixels();
    CHECK(p[0] == (0x8000 | 0x7C00) && p[2] == 0x03E0 && p[2 * 4] == 0x001F && p[2 * 4 + 2] == 0x7FFF);
    // Truncated data is an error.
    bool threw = false;
    try {
        MsVideo1Decoder t(8, 4, 16);
        t.decode(std::span(f8).first(5));
    } catch (const FormatError&) {
        threw = true;
    }
    CHECK(threw);
    // 8-bit: a 1-colour block (B in 0x80..0x8F) and the palette.
    MsVideo1Decoder g(4, 4, 8);
    g.palette = {{0, 0, 0, 255}, {10, 20, 30, 255}};
    const Bytes f9{0x01, 0x80};
    g.decode(f9);
    g.to_rgba(rgba);
    CHECK(rgba[0] == 10 && rgba[1] == 20 && rgba[2] == 30);
    // A RIFF that claims more than the file holds is rejected.
    Bytes bad = file;
    bad[4] = 0xff, bad[5] = 0xff;
    threw = false;
    try {
        parse_avi(bad);
    } catch (const FormatError&) {
        threw = true;
    }
    CHECK(threw);
}

// Every intro video of the release: container, stream formats, every frame decoded.
void test_corpus_videos(const std::filesystem::path& game) {
    struct Expect {
        const char* path;
        std::size_t frames;
        double fps;
        std::uint64_t mid[2]; // FNV-1a of the RGBA middle frame (RU, ENG; is_logo.avi differs: the logo's subtitle)
    };
    const Expect videos[] = {{"DATA/nikita.avi", 219, 24, {0xf612a5f0fb6f7693ull, 0xf612a5f0fb6f7693ull}},
                             {"DATA/is_logo.avi", 226, 24, {0xcc666bb3f5bd424dull, 0xacf84921b1f0ec99ull}},
                             {"DATA/montecristo.avi", 202, 25, {0xd51bba328fbe172bull, 0xd51bba328fbe172bull}}};
    for (const Expect& v : videos) {
        const auto p = resolve_path(game, v.path);
        if (!p) {
            std::printf("  %s: absent\n", v.path);
            continue;
        }
        try {
            const Bytes file = read_file(*p);
            const AviFile a = parse_avi(file);
            CHECK(a.width == 640 && a.height == 480 && a.video.bit_count == 16 && a.video.compression == "CRAM");
            CHECK(a.frame_count() == v.frames && a.total_frames == v.frames && a.fps() == v.fps);
            CHECK(a.audio.format_tag == 1 && a.audio.channels == 2 && a.audio.sample_rate == 22050 && a.audio.bits == 16);
            const auto pcm = avi_audio_pcm16(a, file);
            const AviStreamHeader& as = a.streams[std::size_t(a.audio_stream)];
            CHECK(pcm.size() / 2 == as.length);
            MsVideo1Decoder d(640, 480, 16);
            Bytes rgba;
            std::uint64_t mid = 0, last = 0;
            std::size_t n = 0;
            for (const AviChunk& c : a.video_chunks) {
                if (c.kind == "pc") continue;
                d.decode(std::span(file).subspan(c.offset, c.size));
                if (++n == v.frames / 2) d.to_rgba(rgba), mid = fnv1a(rgba);
            }
            d.to_rgba(rgba);
            last = fnv1a(rgba);
            CHECK(n == v.frames && rgba.size() == 640u * 480 * 4);
            CHECK(mid == v.mid[0] || mid == v.mid[1]);
            std::printf("  %s: %zu frames %.0f fps, %.2f s audio; frame %zu %016llx, last %016llx\n", v.path, n, a.fps(),
                        double(pcm.size() / 2) / 22050, v.frames / 2, (unsigned long long)mid, (unsigned long long)last);
        } catch (const std::exception& ex) {
            ++g_fail;
            std::printf("  FAILED %s: %s\n", v.path, ex.what());
        }
    }
}

void test_briefing_parse() {
    const char* cfg = "object WayPoint0\n CameraX = 1.5\n CameraY = 2.0\n CameraZ = 3.0\n TargetX = 4.0\n TargetY = 5.0\n"
                      " TargetZ = 6.0\n EdgeType = \"spline\"\n WaitType = \"flyaround\"\n EdgeTime = 5.0\n WaypointTime = 0.000\n"
                      " RotateTime = 4.0\n FadeTime = 3.024\n ZoomTime = 0.000\n WaitForText = false\n WaitForSound = true\n"
                      " WaitForTime = true\n WaitForClick = false\n TextResID = \"T01_T01\"\n SoundResID = \"T01_T02\"\n"
                      " NoisePercent = 0\n FadePercent = 100\n ZoomOn = false\n NightVisionOn = true\n LoopIndex = -1\nend\n";
    const auto w = shell::parse_briefing_cfg(cfg);
    CHECK(w.size() == 1);
    if (w.size() == 1) {
        CHECK(w[0].camera.x == 1.5f && w[0].target.z == 6.f && w[0].edge == shell::BriefingWaypoint::Edge::Spline);
        CHECK(w[0].wait == shell::BriefingWaypoint::Wait::FlyAround && std::abs(w[0].rotate_speed - 6.2831855f / 4) < 1e-6f);
        CHECK(w[0].fade == 1.f && w[0].noise == 0 && w[0].night_vision && w[0].zoom == 0 && w[0].loop_index == -1);
        CHECK(w[0].text_id == "T01_T01" && w[0].sound_id == "T01_T02" && w[0].wait_for_sound && !w[0].wait_for_click);
    }
    // A missing key or a mistyped value is "Invalid property".
    std::string bad = cfg;
    bad.replace(bad.find("NoisePercent = 0"), 16, "NoisePercent = 0.0");
    bool threw = false;
    try {
        shell::parse_briefing_cfg(bad);
    } catch (const FormatError&) {
        threw = true;
    }
    CHECK(threw);
    // flyaround needs RotateTime != 0.
    bad = cfg;
    bad.replace(bad.find("RotateTime = 4.0"), 16, "RotateTime = 0.0");
    threw = false;
    try {
        shell::parse_briefing_cfg(bad);
    } catch (const FormatError&) {
        threw = true;
    }
    CHECK(threw);
    const auto s = shell::parse_briefing_setup("object briefing\n filename = \"briefing.cfg\"\n cd_track = 3\nend\n"
                                               "object mission\n only_briefing = true\nend\n"
                                               "object briefing_sounds\n desc = \"resource\"\n library = \"voices.lib\"\n"
                                               " MIS_OBJ = \"vc_mis_obj1.wav\"\nend\n");
    CHECK(s.filename == "briefing.cfg" && s.cd_track == 3 && s.only_briefing && s.sounds.size() == 1 && s.sounds.at("MIS_OBJ") == "vc_mis_obj1.wav");
}

// Every campaign mission's briefing: mission.cfg's briefing objects and briefing.cfg (20 files, 378
// waypoints in both releases); every voice key resolves in briefing_sounds and voices.lib.
void test_corpus_briefings(const std::filesystem::path& game) {
    std::size_t files = 0, points = 0, only = 0, voices = 0, missing_voices = 0;
    for (const auto& c : ui::scan_campaigns(game))
        for (const auto& m : c.missions) {
            const auto dir = resolve_path(game, m.path);
            CHECK(dir.has_value());
            if (!dir) continue;
            const auto cfg = resolve_path(*dir, "mission.cfg");
            if (!cfg) continue;
            const Bytes mb = read_file(*cfg);
            const auto setup = shell::parse_briefing_setup({reinterpret_cast<const char*>(mb.data()), mb.size()});
            only += setup.only_briefing;
            if (setup.filename.empty()) continue;
            const auto bf = resolve_path(*dir, setup.filename);
            CHECK(bf.has_value());
            if (!bf) continue;
            try {
                const Bytes b = read_file(*bf);
                const auto w = shell::parse_briefing_cfg({reinterpret_cast<const char*>(b.data()), b.size()});
                ++files;
                points += w.size();
                CHECK(!w.empty());
                for (const auto& p : w) {
                    if (p.sound_id.empty()) continue;
                    ++voices;
                    if (!setup.sounds.count(p.sound_id)) ++missing_voices;
                }
            } catch (const std::exception& ex) {
                ++g_fail;
                std::printf("  FAILED %s: %s\n", bf->string().c_str(), ex.what());
            }
        }
    CHECK(files == 20 && points == 378 && only == 2);
    std::printf("  briefings: %zu files, %zu waypoints, %zu only_briefing missions; %zu voice keys, %zu not in briefing_sounds\n", files,
                points, only, voices, missing_voices);
}

// The CD-ripped music header the audio engine streams (MUSIC/TrackNN.wav from src/install).
void test_music_wav() {
    Bytes h;
    put4(h, "RIFF"), put32(h, 36 + 4000), put4(h, "WAVE"), put4(h, "fmt "), put32(h, 16), put16(h, 1), put16(h, 2);
    put32(h, 44100), put32(h, 176400), put16(h, 4), put16(h, 16), put4(h, "data"), put32(h, 4000);
    CHECK(h.size() == 44 && music_wav_data_size(h, 44 + 4000) == 4000u);
    CHECK(music_wav_data_size(h, 44 + 1002) == 1000u); // a short file: whole frames that exist
    Bytes mono = h;
    mono[22] = 1;
    CHECK(!music_wav_data_size(mono, 4044));
    Bytes rate = h;
    rate[24] = 0x22, rate[25] = 0x56; // 22050
    CHECK(!music_wav_data_size(rate, 4044));
    CHECK(!music_wav_data_size(std::span(h).first(40), 4044));
}

void test_ini() {
    std::string t = "[CS]\r\nDISPLAY_WIDTH=1024\r\nSFX_VOLUME = 25\r\n\r\n[MULTIPLAYER]\r\nLOGIN=GUEST\r\n";
    CHECK(shell::ini_get(t, "cs", "sfx_volume") == "25" && shell::ini_get_int(t, "CS", "DISPLAY_WIDTH", 0) == 1024);
    CHECK(!shell::ini_get(t, "CS", "LOGIN") && shell::ini_get_int(t, "CS", "MISSING", 7) == 7);
    t = shell::ini_set(t, "CS", "SFX_VOLUME", "40");
    CHECK(shell::ini_get(t, "CS", "SFX_VOLUME") == "40" && t.find("SFX_VOLUME=40\r\n\r\n[MULTIPLAYER]") != std::string::npos);
    t = shell::ini_set(t, "CS", "SUBTITLES", "0"); // appended after the section's last entry
    CHECK(t.find("SFX_VOLUME=40\r\nSUBTITLES=0\r\n") != std::string::npos);
    t = shell::ini_set(t, "NEW", "A", "1");
    CHECK(shell::ini_get(t, "NEW", "A") == "1" && shell::ini_get(t, "MULTIPLAYER", "LOGIN") == "GUEST");
}

// A shell without game data: strings are "S<id>", no textures; the clock advances 40 ms per frame.
struct ShellRig {
    shell::GuiEnv env;
    std::unique_ptr<shell::Shell> sh;
    ShellRig(int mode, const shell::LaunchBlock& last = {}, std::filesystem::path game = "/nonexistent", shell::Shell::Config base = {}) {
        base.game_dir = std::move(game);
        if (!base.str) base.str = [](int id) { return "S" + std::to_string(id); };
        base.intro = false;
        sh = std::make_unique<shell::Shell>(std::move(base), mode, last);
        env.ctrls = &sh->config().ctrls;
        env.texture = [](std::string_view) { return std::int32_t(-1); };
        sh->start(env);
    }
    void frames(int n) {
        for (int i = 0; i < n; ++i) {
            env.now_ms += 40;
            sh->update(env);
            OverlayFrame f;
            sh->draw(f, env);
        }
    }
    void key(int vk) { sh->key_down(env, vk), sh->key_up(env, vk), frames(1); }
};

void test_wipe() {
    // ScreenTransition: steps = (target − rowLvl·7 − colLvl·8 − init) / tick; 25 ms per step at most one per frame.
    const int expect[16] = {29, 29, 31, 31, 45, 45, 45, 45, 29, 29, 31, 31, 45, 45, 45, 45};
    for (int e = 0; e < 16; ++e) {
        shell::ScreenTransition t;
        t.start(e, 0);
        shell::GuiEnv env;
        int frames = 0;
        while (!t.done() && frames < 200) {
            env.now_ms += 40;
            OverlayFrame f;
            t.draw(f, env);
            ++frames;
        }
        CHECK(t.done() && frames == expect[e] + 1);
        CHECK(shell::ScreenTransition::inverse(shell::ScreenTransition::inverse(e)) == e);
        // A finished reveal is clear, a finished cover black.
        CHECK(t.level(e % 8, (e * 3) % 8) == (e < 8 ? 0 : 15));
    }
    shell::ScreenTransition t;
    CHECK(t.start(0x10, 0) == 1 && t.start(0x11, 1) == 8 && shell::ScreenTransition::inverse(2) == 11);
    t.start(0, 0); // reveal bottom→top: the bottom row starts clearer
    CHECK(t.level(7, 0) == 15 && t.level(0, 0) == 15);
}

void test_navigation() {
    ShellRig r(1);
    CHECK(r.sh->current() == shell::kMainMenu);
    CHECK(r.sh->form(shell::kMainMenu)->focus() == 2); // OnShow: "Начать игру"
    r.key(shell::kVkReturn);
    CHECK(r.sh->current() == shell::kSingleGame && r.sh->form(shell::kSingleGame)->focus() == 1);
    r.key(shell::kVkReturn);
    CHECK(r.sh->current() == shell::kCampaignList);
    r.key(shell::kVkEscape);
    CHECK(r.sh->current() == shell::kSingleGame);
    // The difficulty selector: Down twice skips the disabled multiplayer button; Left lowers, GAME_LEVEL is written on leaving.
    r.key(shell::kVkDown);
    CHECK(r.sh->form(shell::kSingleGame)->focus() == 2);
    r.key(shell::kVkDown);
    CHECK(r.sh->form(shell::kSingleGame)->focus() == 4);
    r.key(shell::kVkLeft);
    r.key(shell::kVkEscape);
    CHECK(r.sh->current() == shell::kMainMenu && r.sh->setting("GAME_LEVEL", -1) == 0 && r.sh->launch().difficulty == 0);
    // Options → audio → Esc reverts; the main menu's Esc asks to quit; N returns, Y quits after the cover wipe.
    r.sh->form(shell::kMainMenu)->on_command(r.env, {4, 0, 0, 0});
    CHECK(r.sh->current() == shell::kOptions);
    r.key(shell::kVkReturn); // focus 1: video
    CHECK(r.sh->current() == shell::kVideoOptions);
    r.key(shell::kVkEscape);
    r.key(shell::kVkEscape);
    CHECK(r.sh->current() == shell::kMainMenu);
    r.key(shell::kVkEscape);
    CHECK(r.sh->current() == shell::kAskQuit && r.sh->form(shell::kAskQuit)->focus() == 1);
    r.sh->char_in(r.env, 'n');
    CHECK(r.sh->current() == shell::kMainMenu);
    r.key(shell::kVkEscape);
    r.sh->char_in(r.env, 'y');
    CHECK(r.sh->exit_code() == 0 && !r.sh->finished());
    r.frames(60);
    CHECK(r.sh->finished() && r.sh->wipe().level(3, 3) == 15);
    // Sounds: hover/focus HILIGHT, activation BUTTON_CLICK, the exit MISSION_START.
    CHECK(std::count(r.env.sounds.begin(), r.env.sounds.end(), "BUTTON_CLICK") >= 3);
    CHECK(std::count(r.env.sounds.begin(), r.env.sounds.end(), "HILIGHT") >= 3);
    CHECK(!r.env.sounds.empty() && r.env.sounds.back() == "MISSION_START");
}

// shell_ctrls.cfg / menu_resources.cfg of the release, and the C00M01 briefing timeline (notes/shell.md E §8).
void test_corpus_shell_data(const std::filesystem::path& game) {
    const auto ctrls_path = resolve_path(game, "UI/shell_ctrls.cfg"), res_path = resolve_path(game, "UI/menu_resources.cfg");
    CHECK(ctrls_path && res_path);
    if (!ctrls_path || !res_path) return;
    const Bytes cb = read_file(*ctrls_path), rb = read_file(*res_path);
    const auto c = shell::parse_shell_ctrls({reinterpret_cast<const char*>(cb.data()), cb.size()});
    const auto r = shell::parse_menu_resources({reinterpret_cast<const char*>(rb.data()), rb.size()});
    CHECK(c.main_menu_track == -1 && c.focus_flash_interval == 0.25f && c.credits_scroll_interval == 0.02f);
    CHECK(c.logo2_param == "data\\nikita.avi" && c.splash_param == "data\\is_logo.avi");
    CHECK(c.logo1_param.empty() || c.logo1_param == "data\\montecristo.avi");
    CHECK(c.faders.count("logo2") && c.faders.at("logo2").sound == "LOGO_NIKITA" && c.faders.at("splash").show == 6.f);
    CHECK(c.backgrounds.at("main_menu") == "Main" && c.backgrounds.at("mission_list") == "Pl00" && c.backgrounds.at("credits") == "Crdt");
    CHECK(c.text_flash.on_bright == 0xff64ff64u && c.text_flash.selected_bright == 0xffffffffu && c.text_flash.off_bright == 0xff646464u);
    CHECK(c.sprites.count("tile") && c.sprites.at("lamp_highlight").w == 58 && c.sprites.at("arrow_highlight_v").rot == 1);
    CHECK(r.textures.at("ui_shell2") == 13 && r.backgrounds.at("crdt_dd") == 31 && r.sounds.at("BUTTON_CLICK") == "i_click1.wav");
    // Every sprite names a loaded page; every form background has its five pieces.
    for (const auto& [name, sp] : c.sprites) CHECK(r.textures.count(sp.texture));
    for (const auto& [form, bg] : c.backgrounds) {
        std::string b = bg;
        for (char& ch : b) ch = char(std::tolower((unsigned char)ch));
        for (const char* suffix : {"_lu", "_ru", "_ld", "_rd", "_side"}) CHECK(r.backgrounds.count(b + suffix));
    }
    std::printf("  shell_ctrls: %zu sprites, %zu backgrounds, logo1 '%s'\n", c.sprites.size(), c.backgrounds.size(), c.logo1_param.c_str());

    // The tutorial briefing: black, fade-in 5.0–8.0 s, the jump at WP2 cuts at 11.555 s, WP3 at 13.064 s.
    const auto dir = resolve_path(game, "MISSIONS/CAMPAIGN/CAMPAIGN.00/Mission.01");
    const auto bf = dir ? resolve_path(*dir, "briefing.cfg") : std::nullopt;
    CHECK(bf.has_value());
    if (!bf) return;
    const Bytes b = read_file(*bf);
    shell::BriefingPath path(shell::parse_briefing_cfg({reinterpret_cast<const char*>(b.data()), b.size()}));
    std::uint32_t t = 1000; // an arbitrary clock origin
    path.start(t);
    auto at = [&](std::uint32_t ms) {
        while (t < 1000 + ms) t += 10, path.update(t);
    };
    const auto first = path.take_sounds();
    CHECK(first.size() == 1 && first[0] == "T01_T01" && path.index() == 0 && path.fade() == 1.f);
    // The timings below are the RU file's (the ENG voices are longer, so its edges are too).
    const auto& w = path.waypoints();
    if (w.size() < 4 || std::abs(w[1].edge_time - 5.045f) > 1e-3f) {
        at(400000);
        CHECK(path.done());
        return;
    }
    at(6500);
    CHECK(path.index() == 1 && std::abs(path.fade() - 0.5f) < 0.02f);
    at(11000);
    CHECK(path.index() == 2);
    at(11400);
    CHECK(path.fade() > 0.85f); // dipping to black before the cut
    at(13100);
    CHECK(path.index() == 3 && path.fade() == 0.f);
    at(140000);
    CHECK(path.done());
}

// The real campaign tree: the lists, a mission start's launch block, and the return after a game.
void test_corpus_navigation(const std::filesystem::path& game) {
    shell::Shell::Config base;
    TextRes strings;
    if (auto p = resolve_path(game, "iron3d.dll")) strings = TextRes::from_pe(read_file(*p));
    base.str = [&strings](int id) { return utf8_to_cp1251(strings.get(std::uint32_t(id))); };
    ShellRig r(1, {}, game, base);
    r.key(shell::kVkReturn);
    r.key(shell::kVkReturn);
    CHECK(r.sh->current() == shell::kCampaignList);
    r.key(shell::kVkReturn); // row 0 (selected by OnShow): campaign 00
    CHECK(r.sh->current() == shell::kMissionList && r.sh->launch().game_type == 4);
    r.key(shell::kVkReturn); // mission 01
    CHECK(r.sh->exit_code() == 1 && r.sh->launch().path == "missions/campaign/campaign.00/mission.01/" && r.sh->launch().row == 0);
    CHECK(r.sh->launch().mode == 1 && r.sh->launch().new_mission);
    // After a game in campaign 01 mission 02: main → single → campaigns → mission list, history [-1, 10, 20, 22].
    shell::LaunchBlock last;
    last.game_type = 1, last.mode = 1, last.path = "missions/campaign/campaign.01/mission.02/";
    shell::Shell::Config all = base;
    all.dispatcher = "[COMMON]\r\nALL_AVAILABLE=1\r\n";
    ShellRig a(2, last, game, all);
    CHECK(a.sh->current() == shell::kMissionList);
    CHECK((a.sh->history() == std::vector<int>{-1, shell::kMainMenu, shell::kSingleGame, shell::kCampaignList}));
    a.key(shell::kVkReturn); // the preselected row: mission 02 again
    CHECK(a.sh->launch().path == last.path && a.sh->launch().game_type == 1);
    // Single missions: both listed, the first available.
    const auto singles = a.sh->single_missions();
    CHECK(singles.size() == 2 && singles[0].available && singles[0].path == "missions/single.01/");
    // Failed + L: the load-game form.
    last.mode = 3;
    ShellRig l(2, last, game, base);
    CHECK(l.sh->current() == shell::kLoadGame);
    std::printf("  shell navigation: %zu campaigns, %zu single missions\n", a.sh->campaigns().size(), singles.size());
}

} // namespace

int shell_unit_tests() {
    g_fail = 0;
    test_avi_synthetic();
    test_briefing_parse();
    test_ini();
    test_music_wav();
    test_wipe();
    test_navigation();
    return g_fail;
}

int shell_corpus_tests(const std::filesystem::path& game) {
    g_fail = 0;
    std::printf("shell\n");
    test_corpus_videos(game);
    test_corpus_briefings(game);
    test_corpus_navigation(game);
    test_corpus_shell_data(game);
    return g_fail;
}
