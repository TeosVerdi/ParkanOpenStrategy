#include "mission/research_tree.hpp"

#include "res/nres.hpp"

#include <algorithm>

namespace parkan::mission {

namespace {

const NResEntry& require(const NResArchive& a, std::uint32_t type, const char* what) {
    if (auto* e = a.find_type(type)) return *e;
    throw FormatError(std::string("research tree lacks ") + what);
}

std::vector<std::uint32_t> u32s(std::span<const std::uint8_t> d) {
    if (d.size() % 4) throw FormatError("u32 table size not a multiple of 4");
    std::vector<std::uint32_t> v(d.size() / 4);
    std::memcpy(v.data(), d.data(), d.size());
    return v;
}

// TRF2/TRF4 hold a count per item; TRF3/TRF5 (optional) the concatenated ids.
void load_lists(const NResArchive& a, std::uint32_t count_type, std::uint32_t ids_type, std::size_t n,
                std::vector<ResearchItem>& items, std::vector<std::uint32_t> ResearchItem::* field) {
    auto counts = u32s(require(a, count_type, "an edge-count table").data);
    if (counts.size() != n) throw FormatError("edge-count table size mismatch");
    const NResEntry* ids_entry = a.find_type(ids_type);
    if (!ids_entry) return; // the original also tolerates a missing id table
    auto ids = u32s(ids_entry->data);
    std::size_t p = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (p + counts[i] > ids.size()) throw FormatError("edge table overrun");
        auto& out = items[i].*field;
        out.assign(ids.begin() + p, ids.begin() + p + counts[i]);
        for (auto id : out)
            if (id >= n) throw FormatError("edge to unknown item");
        p += counts[i];
    }
    if (p != ids.size()) throw FormatError("edge table has unused ids");
}

} // namespace

ResearchTree load_research_tree(const std::filesystem::path& path) {
    auto arc = NResArchive::open(path);
    ResearchTree t;

    const auto& trf0 = require(arc, fourcc("TRF0"), "TRF0");
    const std::size_t n = trf0.attr1;
    if (trf0.attr2 != 3 || trf0.attr3 != 40 || trf0.data.size() != n * 40)
        throw FormatError("unexpected TRF0 layout");
    const auto& trf1 = require(arc, fourcc("TRF1"), "TRF1");
    if (trf1.data.size() != n) throw FormatError("TRF1 size mismatch");
    t.trf1_flag = trf1.attr2 == 1;

    auto pool = [&](const char* type) { return require(arc, fourcc(type), type).data; };
    auto codes = pool("TRF7"), names = pool("TRF8"), descs = pool("TRF9"), stats = pool("TRFA");

    for (auto models = pool("TRF6"); !models.empty();) {
        auto s = cstr_at(models, 0);
        t.models.emplace_back(s);
        models = models.subspan(s.size() + 1);
    }
    t.model_ids = u32s(require(arc, fourcc("TRFB"), "TRFB").data);
    if (t.model_ids.size() != t.models.size()) throw FormatError("TRFB/TRF6 count mismatch");

    t.items.resize(n);
    Reader r(trf0.data);
    for (std::size_t i = 0; i < n; ++i) {
        auto& it = t.items[i];
        it.research_power = r.f32();
        it.research_ore = r.f32();
        it.build_power = r.f32();
        it.build_ore = r.f32();
        it.code = cstr_at(codes, r.u32());
        it.name = cstr_at(names, r.u32());
        it.description = cstr_at(descs, r.u32());
        it.stats = cstr_at(stats, r.u32());
        it.kind = r.u32();
        it.extra = r.u32();
        it.initial_state = trf1.data[i];
    }
    load_lists(arc, fourcc("TRF2"), fourcc("TRF3"), n, t.items, &ResearchItem::prerequisites);
    load_lists(arc, fourcc("TRF4"), fourcc("TRF5"), n, t.items, &ResearchItem::unlocks);

    // The two edge sets are inverses of each other in every shipped tree.
    for (std::size_t i = 0; i < n; ++i)
        for (auto p : t.items[i].prerequisites)
            if (std::ranges::find(t.items[p].unlocks, i) == t.items[p].unlocks.end())
                throw FormatError("prerequisite without matching unlock");
    return t;
}

} // namespace parkan::mission
