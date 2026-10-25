#include "ui/designer.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace parkan::ui {

namespace {

bool in(float x, float y, float x0, float y0, float x1, float y1) { return x >= x0 && x < x1 && y >= y0 && y < y1; }
bool icontains(std::string_view s, std::string_view part) {
    return std::search(s.begin(), s.end(), part.begin(), part.end(), [](char a, char b) {
               return std::tolower((unsigned char)a) == std::tolower((unsigned char)b);
           }) != s.end();
}

// The category tabs (§2.2): frame x0..x1 (y 16..53) and icon x (y 25; the right armour icon at 26), per panel side;
// icon source on `icons` and the three colours (disabled, normal, active).
struct TabDef {
    float fl0, fl1, fr0, fr1, icon_l, icon_r, iu, iv;
    std::uint32_t colour[3];
};
constexpr TabDef kTabs[6] = {
    {8, 35, 465, 492, 9, 466, 48, 72, {0xff404064u, 0xff9696e6u, 0xffdcdcffu}},     // 0 chassis
    {35, 61, 492, 518, 36, 493, 72, 72, {0xff404064u, 0xff9696e6u, 0xffdcdcffu}},   // 1 turrets
    {101, 128, 558, 585, 102, 559, 193, 97, {0xff404b4bu, 0xff78c8c8u, 0xffd2e6e6u}}, // 2 armour
    {128, 154, 585, 611, 129, 586, 120, 72, {0xff404b4bu, 0xff78c8c8u, 0xffd2e6e6u}}, // 3 internal
    {61, 87, 518, 544, 62, 519, 96, 72, {0xff404064u, 0xff9696e6u, 0xffdcdcffu}},   // 4 weapons
    {154, 180, 611, 637, 155, 612, 206, 192, {0xff404b4bu, 0xff78c8c8u, 0xffd2e6e6u}}, // 5 ammo
};
constexpr std::uint32_t kTabText[6] = {6216, 6218, 6217, 6221, 6219, 6220};
// CPartPanel_OnLButtonDown's category order (§3.3) and CModelPanel_Update's (§3.1).
constexpr int kClickOrder[6] = {0, 1, 3, 4, 5, 2};

// The bottom buttons (§2.4): icon at (x, 450) 24×24, the frame rect, the icon source and tooltips.
struct ButtonDef {
    float fx0, fx1, ix;
    HudPage page;
    float iu, iv;
    std::uint32_t tip;
};
constexpr ButtonDef kButtons[5] = {
    {194, 229, 201, HudPage::Page1, 176, 231, 6222}, // 0 deliver
    {229, 264, 231, HudPage::Page1, 200, 231, 1554}, // 1 delete
    {269, 304, 278, HudPage::Page1, 152, 231, 1555}, // 2 save
    {304, 339, 308, HudPage::Page1, 128, 231, 1556}, // 3 load
    {405, 446, 413, HudPage::Icons, 192, 72, 1557},  // 4 exit
};

} // namespace

bool prefix_match(std::string_view a, std::string_view b) {
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i)
        if (a[i] != '?' && b[i] != '?' && std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}

std::string overlay_mount_name(const std::string& model, const std::string& slot) {
    std::string r = model;
    for (std::size_t i = 0; i < slot.size() && i < r.size(); ++i) r[i] = slot[i];
    return r;
}

int part_kind(const mission::ResearchItem& it) {
    const std::uint32_t cat = it.kind >> 24, sub = it.extra & 0xff, b25 = (it.extra >> 8) & 0xff;
    switch (cat) {
    case 9: return sub == 0x20 ? 0 : sub == 0x21 ? 1 : -1;
    case 8: return b25 == 0x51 ? 5 : (b25 == 0x50 || b25 == 0x52 || b25 == 0x54) ? 1 : b25 == 0x53 ? 2 : -1;
    case 0xc: return 2;
    case 0xb: return sub == 0x45 ? 6 : sub == 0x44 ? 7 : 3;
    case 0xa: return 4;
    default: return -1;
    }
}

std::uint32_t part_type_mask(const mission::ResearchItem& it) {
    const std::uint32_t cat = it.kind >> 24, sub = it.extra & 0xff, size = (it.extra >> 16) & 0xff, tk = (it.kind >> 16) & 0xff;
    if (cat == 9 && sub == 0x21) {
        switch (tk) {
        case 3: return 0x1002000;
        case 4: return 0x1004000;
        case 5: return 0x1010000;
        case 6: return 0x1020000;
        default: return 0x1008000;
        }
    }
    if (cat == 8) {
        switch (sub) {
        case 0x10: return 0x80000040;
        case 0x11: return size == 1 ? 0x80010000 : size == 2 ? 0x80020000 : size == 3 ? 0x80040000 : 0;
        case 0x12: return 0x80000400;
        case 0x13: return 0x80000004;
        case 0x14: return 0x80000010;
        case 0x15: return 0x80000008;
        case 0x18: return 0x80000200;
        case 0x19: return 0x80001000;
        case 0x1a: return 0x80000002;
        case 0x1d: return 0x80100000;
        case 0x1e: return 0x80200000;
        default: return 0;
        }
    }
    return 0;
}

int Designer::Panel::active() const {
    for (int i = 0; i < 6; ++i)
        if (cats[std::size_t(i)].active) return i;
    return 0;
}

// --- catalogue ---

std::string Designer::model_of(std::int32_t item) const {
    const economy::Research* r = hq_.economy() ? hq_.economy()->research(hq_.clan()) : nullptr;
    const mission::ResearchItem* it = r ? r->item(item) : nullptr;
    return it && it->model_index() < r->tree().models.size() ? r->tree().models[it->model_index()] : std::string();
}

std::string Designer::label_of(std::int32_t item) const {
    // CPartRow_SetPart 10047540: "%s (%s)" of the name (TRF8) and the code (TRF7) when both exist.
    const economy::Research* r = hq_.economy() ? hq_.economy()->research(hq_.clan()) : nullptr;
    const mission::ResearchItem* it = r ? r->item(item) : nullptr;
    if (!it) return {};
    return it->code.empty() ? it->name : it->name + " (" + it->code + ")";
}

int Designer::kind_of(std::int32_t item) const {
    const economy::Research* r = hq_.economy() ? hq_.economy()->research(hq_.clan()) : nullptr;
    const mission::ResearchItem* it = r ? r->item(item) : nullptr;
    return it ? part_kind(*it) : -1;
}

std::uint32_t Designer::mask_of(std::int32_t item) const {
    const economy::Research* r = hq_.economy() ? hq_.economy()->research(hq_.clan()) : nullptr;
    const mission::ResearchItem* it = r ? r->item(item) : nullptr;
    return it ? part_type_mask(*it) : 0;
}

void Designer::build_list(int cat, const std::string& prefix, bool weapon_mode) {
    // CPartCategory_BuildList 10048680 on the parts panel's category.
    Category& c = left_.cats[std::size_t(cat)];
    c.rows.clear(), c.scroll = 0;
    const economy::Research* r = hq_.economy() ? hq_.economy()->research(hq_.clan()) : nullptr;
    if (!r) return;
    const bool only_available = !hq_.host().full_research_tree;
    std::vector<std::string> prefixes;
    if (weapon_mode) {
        const std::string s = prefix.size() >= 2 ? prefix.substr(prefix.size() - 2) : prefix;
        if (s.size() == 2 && std::tolower((unsigned char)s[1]) == 'r') prefixes = {"e_gun_" + std::string(1, s[0]) + "c", "e_gun_" + std::string(1, s[0]) + "l"};
        else prefixes = {"e_gun_" + s};
    } else if (plant_class_ == 0 || prefix.empty() || std::tolower((unsigned char)prefix[0]) != 'r') {
        prefixes = {prefix};
    } else {
        static const char* kClass[4] = {"r_t", "r_l", "r_m", "r_b"};
        for (std::uint32_t k = 0; k < std::min<std::uint32_t>(plant_class_, 4); ++k) prefixes.push_back(kClass[k]);
    }
    // PartCatalog_QueryByPrefix(List) 1008aba0 / 1008adc0: every TRF6 model matching a prefix; its item when
    // researched and present (or always with the full tree); per prefix sorted by item and de-duplicated.
    std::vector<std::int32_t> items;
    const auto& tree = r->tree();
    for (const std::string& p : prefixes) {
        std::vector<std::int32_t> part;
        for (std::size_t m = 0; m < tree.models.size() && m < tree.model_ids.size(); ++m) {
            if (!prefix_match(tree.models[m], p)) continue;
            const std::int32_t item = std::int32_t(tree.model_ids[m] >> 16);
            if (!r->valid(item)) continue;
            const std::uint8_t st = r->state(item);
            if (!only_available || ((st & economy::kResResearched) && (st & economy::kResPresent))) part.push_back(item);
        }
        std::sort(part.begin(), part.end());
        part.erase(std::unique(part.begin(), part.end()), part.end());
        items.insert(items.end(), part.begin(), part.end());
    }
    if (items.size() > 64) items.resize(64);
    // The exchange sort that only orders codes of equal length (strcmpi) — the original's quirk.
    for (std::size_t i = 0; i < items.size(); ++i)
        for (std::size_t j = i + 1; j < items.size(); ++j) {
            const std::string &a = r->item(items[i])->code, &b = r->item(items[j])->code;
            if (a.empty() || b.empty() || a.size() != b.size()) continue;
            std::string la = a, lb = b;
            for (char& ch : la) ch = char(std::tolower((unsigned char)ch));
            for (char& ch : lb) ch = char(std::tolower((unsigned char)ch));
            if (la > lb) std::swap(items[i], items[j]);
        }
    // Rows only for items whose display model has a description (objects.dlb); the count stays (empty rows).
    for (std::int32_t item : items) {
        Row row;
        const std::string model = model_of(item);
        if (!hq_.host().described || hq_.host().described(model)) {
            row.label = label_of(item), row.slot = prefix, row.mount = 0, row.item = item;
        }
        c.rows.push_back(std::move(row));
    }
    if (c.sel >= int(c.rows.size())) c.sel = 0;
    c.clicked_row = !c.rows.empty(); // the selected row is pressed (the left preview follows)
}

// --- lifecycle ---

void Designer::open_for_plant(std::uint32_t plant_class) {
    if (open_) return;
    plant_class_ = plant_class, open_ = true;
    reset("r");
}

void Designer::close() {
    open_ = false;
    save_dialog_ = load_dialog_ = editing_ = false;
    left_.preview = {}, right_.preview = {}, view_ = {};
    left_.preview_item = right_.preview_item = -1;
}

void Designer::reset(const std::string& mode) {
    // CConstructor_Reset 1004e410 + CPartsPanel_Load / CModelPanel_Load.
    mode_ = mode;
    save_dialog_ = load_dialog_ = editing_ = false, edit_text_ = "default";
    asm_.reset();
    view_ = {};
    pending_.reset(), button_fire_ = -1, recent_pressed_ = -1, turret_present_ = false;
    left_ = {}, right_ = {};
    left_.x0 = 0, left_.x1 = 183, right_.x0 = 457, right_.x1 = 640;
    Row chassis;
    chassis.slot = mode, chassis.model_row = true;
    right_.cats[kChassis].rows.push_back(chassis);
    right_.cats[kChassis].enabled = left_.cats[kChassis].enabled = true;
    right_.cats[kChassis].clicked_row = true; // processed on the next update (§1.3)
    activate_tab(kChassis);
}

void Designer::activate_tab(int cat) {
    for (Panel* p : {&left_, &right_})
        for (int i = 0; i < 6; ++i) p->cats[std::size_t(i)].active = i == cat;
}

// --- the model (§3.4 – §3.8) ---

void Designer::add_part(std::int32_t item) {
    if (item < 0) return;
    if (!has_model()) {
        set_chassis(model_of(item), item);
        return;
    }
    switch (kind_of(item)) {
    case 1: attach_turret(item); break;
    case 2: attach_weapon(item); break;
    case 3: attach_into(kInternal, item); break;
    case 4: attach_into(kAmmo, item); break;
    case 7: attach_into(kArmour, item); break;
    default: break;
    }
}

void Designer::add_default_part(int cat, int row) {
    // Every slot whose name has a "<slot>_df" part in the catalogue (only available) gets it.
    const economy::Research* r = hq_.economy() ? hq_.economy()->research(hq_.clan()) : nullptr;
    if (!r) return;
    Category& c = right_.cats[std::size_t(cat)];
    const std::int32_t item = r->find_item(c.rows[std::size_t(row)].slot + "_df");
    if (!r->valid(item)) return;
    const std::uint8_t st = r->state(item);
    if (!(st & economy::kResResearched) || !(st & economy::kResPresent)) return;
    c.sel = row;
    attach_into(cat, item);
}

void Designer::set_chassis(const std::string& entry, std::int32_t item) {
    // CConstructor_SetChassis 10051fd0.
    if (!hq_.host().library) return;
    asm_.emplace(*hq_.host().library);
    if (!asm_->set_root(entry)) {
        asm_.reset();
        return;
    }
    Category& ch = right_.cats[kChassis];
    ch.rows.at(0).item = item, ch.rows[0].label = label_of(item), ch.rows[0].comp = 0;
    // Turret mounts: the chassis' named mesh nodes.
    Category& tur = right_.cats[kTurret];
    for (std::int32_t i = 0; i < asm_->part_count(); ++i)
        if (const std::string* n = asm_->part_name(i)) tur.rows.push_back({*n, 0, i, {}, -1, -1, true});
    if (!tur.rows.empty()) {
        tur.enabled = left_.cats[kTurret].enabled = true, tur.sel = 0, tur.clicked_row = true;
        activate_tab(kTurret);
    }
    // Internal slots: armour (i_arm_*) and modules; i_brn_* skipped; defaults attached in device order.
    for (std::int32_t j = 0; j < asm_->device_count(); ++j) {
        const std::string* n = asm_->device_name(j);
        if (!n || icontains(*n, "i_brn_")) continue;
        const int cat = icontains(*n, "i_arm_") ? kArmour : kInternal;
        Category& c = right_.cats[std::size_t(cat)];
        c.rows.push_back({*n, 0, j, {}, -1, -1, true});
        add_default_part(cat, int(c.rows.size()) - 1);
    }
    for (int cat : {kInternal, kArmour})
        if (!right_.cats[std::size_t(cat)].rows.empty()) {
            right_.cats[std::size_t(cat)].enabled = left_.cats[std::size_t(cat)].enabled = true;
            if (cat == kInternal) right_.cats[std::size_t(cat)].sel = 0, right_.cats[std::size_t(cat)].clicked_row = true;
        }
}

void Designer::attach_turret(std::int32_t item) {
    // CConstructor_AttachTurret 10052990.
    Category& tur = right_.cats[kTurret];
    if (tur.sel < 0 || tur.sel >= int(tur.rows.size())) return;
    Row& slot = tur.rows[std::size_t(tur.sel)];
    const std::int32_t nodes_before = asm_->part_count(), items_before = asm_->device_count();
    const std::int32_t id = asm_->attach(overlay_mount_name(model_of(item), slot.slot), slot.parent, slot.mount);
    if (id < 0) return;
    turret_present_ = true;
    slot.item = item, slot.label = label_of(item), slot.comp = id;
    tur.sel = (tur.sel + 1) % int(tur.rows.size()), tur.clicked_row = true;
    Category& wpn = right_.cats[kWeapon];
    int ins = 0;
    for (std::int32_t i = 1; i <= asm_->count_parts(id); ++i)
        if (const std::string* n = asm_->part_name(nodes_before + i - 1))
            wpn.rows.insert(wpn.rows.begin() + ins++, Row{*n, id, i, {}, -1, -1, true});
    if (ins > 0) {
        wpn.enabled = left_.cats[kWeapon].enabled = true, wpn.sel = 0, wpn.clicked_row = true;
        activate_tab(kWeapon);
    }
    Category& mod = right_.cats[kInternal];
    for (std::int32_t j = 0; j < asm_->count_devices(id); ++j) {
        const std::string* n = asm_->device_name(items_before + j);
        if (!n || icontains(*n, "i_brn_")) continue;
        mod.rows.push_back({*n, id, j, {}, -1, -1, true});
        add_default_part(kInternal, int(mod.rows.size()) - 1);
    }
    if (!mod.rows.empty()) mod.sel = 0, mod.clicked_row = true, mod.enabled = left_.cats[kInternal].enabled = true;
}

void Designer::attach_weapon(std::int32_t item) {
    // CConstructor_AttachWeapon 100533d0.
    Category& wpn = right_.cats[kWeapon];
    if (wpn.sel < 0 || wpn.sel >= int(wpn.rows.size())) return;
    const int at = wpn.sel;
    Row& slot = wpn.rows[std::size_t(at)];
    const std::int32_t id = asm_->attach(model_of(item), slot.parent, slot.mount);
    if (id < 0) return;
    slot.item = item, slot.label = label_of(item), slot.comp = id;
    wpn.sel = (wpn.sel + 1) % int(wpn.rows.size()), wpn.clicked_row = true;
    if (asm_->count_devices(id) > 0) {
        const std::string* clip = asm_->device_name(asm_->first_device(id));
        int pos = 0;
        for (int k = 0; k < at; ++k) pos += wpn.rows[std::size_t(k)].item >= 0;
        Category& am = right_.cats[kAmmo];
        Row r{clip ? *clip : std::string(), id, 0, clip ? std::string() : hq_.host().text(6166), -1, -1, true};
        am.rows.insert(am.rows.begin() + std::min<std::size_t>(std::size_t(pos), am.rows.size()), r);
        if (clip) add_default_part(kAmmo, pos);
        am.enabled = left_.cats[kAmmo].enabled = true, am.sel = 0, am.clicked_row = true;
    }
}

void Designer::attach_into(int cat, std::int32_t item) {
    // AttachModule / AttachArmour / AttachAmmo: into the selected row (the previous component is not detached).
    Category& c = right_.cats[std::size_t(cat)];
    if (c.sel < 0 || c.sel >= int(c.rows.size()) || !asm_) return;
    Row& slot = c.rows[std::size_t(c.sel)];
    const std::int32_t id = asm_->attach(model_of(item), slot.parent, slot.mount);
    if (id < 0) return;
    slot.item = item, slot.label = label_of(item), slot.comp = id;
    c.sel = (c.sel + 1) % int(c.rows.size()), c.clicked_row = true;
    right_.preview = {}, right_.preview_item = -1;
}

void Designer::remove_weapon(int row) {
    Category& wpn = right_.cats[kWeapon];
    Row& w = wpn.rows[std::size_t(row)];
    Category& am = right_.cats[kAmmo];
    for (int k = int(am.rows.size()) - 1; k >= 0; --k)
        if (am.rows[std::size_t(k)].parent == w.comp) {
            if (am.rows[std::size_t(k)].comp >= 0) asm_->detach(am.rows[std::size_t(k)].comp);
            am.rows.erase(am.rows.begin() + k);
        }
    asm_->detach(w.comp);
    w.item = -1, w.label.clear(), w.comp = -1;
}

void Designer::remove_turret(int row) {
    // CConstructor_RemoveTurret 10054210: its modules and weapons (from the end), then the turret.
    turret_present_ = false;
    Category& tur = right_.cats[kTurret];
    const std::int32_t id = tur.rows[std::size_t(row)].comp;
    Category& mod = right_.cats[kInternal];
    for (int k = int(mod.rows.size()) - 1; k >= 0; --k)
        if (mod.rows[std::size_t(k)].parent == id) {
            if (mod.rows[std::size_t(k)].comp >= 0) asm_->detach(mod.rows[std::size_t(k)].comp);
            mod.rows.erase(mod.rows.begin() + k);
        }
    Category& wpn = right_.cats[kWeapon];
    for (int k = int(wpn.rows.size()) - 1; k >= 0; --k)
        if (wpn.rows[std::size_t(k)].parent == id) {
            if (wpn.rows[std::size_t(k)].comp >= 0) remove_weapon(k);
            wpn.rows.erase(wpn.rows.begin() + k);
        }
    asm_->detach(id);
    Row& t = tur.rows[std::size_t(row)];
    t.item = -1, t.label.clear(), t.comp = -1;
    for (Category* c : {&mod, &wpn})
        if (c->sel >= int(c->rows.size())) c->sel = 0;
}

void Designer::remove_part(int cat, int row) {
    // CConstructor_RemovePart 10053e70.
    Category& c = right_.cats[std::size_t(cat)];
    if (row < 0 || row >= int(c.rows.size()) || c.rows[std::size_t(row)].item < 0 || !asm_) return;
    const int kind = kind_of(c.rows[std::size_t(row)].item);
    if (cat == kChassis || kind == 0 || kind == 5) {
        reset(mode_); // the whole design goes ("r_" etc. in the original)
        return;
    }
    if (kind == 1) remove_turret(row);
    else if (kind == 2) {
        remove_weapon(row);
    } else {
        asm_->detach(c.rows[std::size_t(row)].comp);
        c.rows[std::size_t(row)].item = -1, c.rows[std::size_t(row)].label.clear(), c.rows[std::size_t(row)].comp = -1;
    }
    right_.preview = {}, right_.preview_item = -1;
}

std::uint32_t Designer::header() const {
    // §1.7: 0x20000000 for an 'a…' chassis slot; else the chassis item's mask, the first turret's when the chassis
    // item is of category 9.
    const Row& ch = right_.cats[kChassis].rows.at(0);
    if (!ch.slot.empty() && std::tolower((unsigned char)ch.slot[0]) == 'a') return 0x20000000;
    std::uint32_t mask = mask_of(ch.item);
    const economy::Research* r = hq_.economy() ? hq_.economy()->research(hq_.clan()) : nullptr;
    if (const mission::ResearchItem* it = r ? r->item(ch.item) : nullptr; it && (it->kind >> 24) == 9) {
        const auto& tur = right_.cats[kTurret].rows;
        mask = tur.empty() ? 0 : mask_of(tur[0].item);
    }
    return mask;
}

std::optional<mission::Scheme> Designer::build_scheme(bool* ok) const {
    // CConstructor_SaveScheme 100548d0: the tree from the model panel's rows; ok = every node researched and present.
    if (ok) *ok = false;
    const Row& ch = right_.cats[kChassis].rows.at(0);
    const std::string root_model = model_of(ch.item);
    if (root_model.empty()) return std::nullopt;
    auto node = [&](const std::string& entry, const Row& r, std::uint32_t slot) {
        mission::SchemeNode n;
        n.lib = "objects.rlb", n.entry = entry, n.one = 1, n.mount = r.mount, n.name = cp1251_to_utf8(r.label.substr(0, 31)), n.category = slot;
        return n;
    };
    mission::Scheme s;
    s.flags = header();
    s.root = node(root_model, ch, 0);
    s.root.mount = -1;
    for (const Row& m : right_.cats[kInternal].rows)
        if (m.item >= 0 && m.parent == 0) s.root.children.push_back(node(model_of(m.item), m, 3));
    for (const Row& a : right_.cats[kArmour].rows)
        if (a.item >= 0 && a.parent == 0) s.root.children.push_back(node(model_of(a.item), a, 2));
    for (const Row& t : right_.cats[kTurret].rows) {
        if (t.item < 0) continue;
        mission::SchemeNode tn = node(overlay_mount_name(model_of(t.item), t.slot), t, 1);
        for (const Row& m : right_.cats[kInternal].rows)
            if (m.item >= 0 && m.parent == t.comp) tn.children.push_back(node(model_of(m.item), m, 3));
        for (const Row& w : right_.cats[kWeapon].rows) {
            if (w.item < 0 || w.parent != t.comp) continue;
            mission::SchemeNode wn = node(model_of(w.item), w, 4);
            for (const Row& a : right_.cats[kAmmo].rows)
                if (a.item >= 0 && a.parent == w.comp) {
                    wn.children.push_back(node(model_of(a.item), a, 5));
                    break;
                }
            tn.children.push_back(std::move(wn));
        }
        s.root.children.push_back(std::move(tn));
    }
    if (ok) {
        const economy::Research* r = hq_.economy() ? hq_.economy()->research(hq_.clan()) : nullptr;
        *ok = r && r->build_cost(s).ok;
    }
    return s;
}

void Designer::load_scheme(const mission::Scheme& s) {
    // CConstructor_LoadScheme 100555b0: the chassis through AddPart, then the root children by slot; internal parts
    // matched by order (quirk).
    reset(mode_);
    right_.cats[kChassis].clicked_row = false;
    const economy::Research* r = hq_.economy() ? hq_.economy()->research(hq_.clan()) : nullptr;
    if (!r) return;
    add_part(r->find_item(s.root.entry));
    if (!has_model()) return;
    int module_n = 0, armour_n = 0;
    for (const auto& c : s.root.children) {
        if (c.category == 1) {
            auto& tur = right_.cats[kTurret];
            for (int k = int(tur.rows.size()) - 1; k >= 0; --k)
                if (tur.rows[std::size_t(k)].mount == c.mount) {
                    tur.sel = k;
                    break;
                }
            add_part(r->find_item(c.entry));
            const std::int32_t tid = tur.rows.empty() ? -1 : tur.rows[std::size_t((tur.sel + int(tur.rows.size()) - 1) % int(tur.rows.size()))].comp;
            for (const auto& g : c.children) {
                if (g.category == 3) {
                    right_.cats[kInternal].sel = module_n++ % std::max<int>(1, int(right_.cats[kInternal].rows.size()));
                    add_part(r->find_item(g.entry));
                } else if (g.category == 4) {
                    auto& wpn = right_.cats[kWeapon];
                    for (int k = 0; k < int(wpn.rows.size()); ++k)
                        if (wpn.rows[std::size_t(k)].parent == tid && wpn.rows[std::size_t(k)].mount == g.mount) wpn.sel = k;
                    const int wi = wpn.sel;
                    add_part(r->find_item(g.entry));
                    for (const auto& a : g.children) {
                        auto& am = right_.cats[kAmmo];
                        for (int k = 0; k < int(am.rows.size()); ++k)
                            if (wi < int(wpn.rows.size()) && am.rows[std::size_t(k)].parent == wpn.rows[std::size_t(wi)].comp) am.sel = k;
                        add_part(r->find_item(a.entry));
                    }
                }
            }
        } else if (c.category == 2) {
            right_.cats[kArmour].sel = armour_n++ % std::max<int>(1, int(right_.cats[kArmour].rows.size()));
            add_part(r->find_item(c.entry));
        } else if (c.category == 3) {
            right_.cats[kInternal].sel = module_n++ % std::max<int>(1, int(right_.cats[kInternal].rows.size()));
            add_part(r->find_item(c.entry));
        }
    }
    activate_tab(kChassis);
}

void Designer::deliver() {
    // §1.7: temp_unit.dat → the recent list (pushed when valid), the plant panel selects project 0; else pager 2111.
    bool ok = false;
    const auto s = build_scheme(&ok);
    if (s && ok) {
        hq_.add_project({"UNITS\\temp_unit.dat", *s});
        hq_.select_project(0);
    } else if (hq_.host().pager && hq_.host().text) {
        hq_.host().pager(hq_.host().text(1541), hq_.host().text(2111));
    }
    close();
}

// --- input ---

bool Designer::panel_click(Panel& p, bool model_panel, float vx, float vy) {
    // CPartPanel_OnLButtonDown 10049fa0 → CPartCategory_OnLButtonDown 10048370.
    const bool preview_click = in(vx, vy, p.x0 + 8, 244, p.x1 - 8, 396);
    if (preview_click && hq_.host().sound) hq_.host().sound("BUTTON_CLICK");
    for (int ci : kClickOrder) {
        Category& c = p.cats[std::size_t(ci)];
        const TabDef& t = kTabs[ci];
        const float ix = model_panel ? t.icon_r : t.icon_l;
        if (c.enabled && in(vx, vy, ix, 25, ix + 24, 49)) {
            if (!c.active) activate_tab(ci);
            return true;
        }
        if (!c.active) continue;
        for (int k = 0; k < 6; ++k) {
            const int i = c.scroll + k;
            if (i >= int(c.rows.size())) break;
            const float y = 65 + 23.f * float(k);
            if (!in(vx, vy, p.x0, y, p.x0 + 180, y + 21) && !(preview_click && i == c.sel)) continue;
            const Row& row = c.rows[std::size_t(i)];
            if (i != c.sel) {
                c.sel = i, c.last_select_ms = now_, c.clicked_row = true;
            } else if (now_ - c.last_select_ms > 200) {
                if (!pending_) pending_ = Pending{model_panel, row.item};
            }
            return true;
        }
    }
    // The scroll buttons (side+147 / side+162, y 203..224).
    Category& a = p.cats[std::size_t(p.active())];
    if (a.rows.size() > 6) {
        if (in(vx, vy, p.x0 + 147, 203, p.x0 + 162, 224)) {
            a.scroll = std::max(0, a.scroll - 1);
            return true;
        }
        if (in(vx, vy, p.x0 + 162, 203, p.x0 + 180, 224)) {
            if (a.scroll + 6 < int(a.rows.size())) ++a.scroll;
            return true;
        }
    }
    return in(vx, vy, p.x0, 0, p.x1, 61);
}

void Designer::lbutton_down(float vx, float vy) {
    // CConstructor_OnLButtonDown 100508f0, then the two panels.
    if (save_dialog_) {
        if (vx > 270 && vx < 370 && vy > 410 && vy < 430) editing_ = !editing_;
        return;
    }
    if (load_dialog_) {
        for (int k = 0; k < 4 && load_top_ + k < int(load_rows_.size()); ++k)
            if (in(vx, vy, 200, 320 + 22.f * float(k), 442, 341 + 22.f * float(k))) {
                const std::string name = load_rows_[std::size_t(load_top_ + k)];
                load_dialog_ = false;
                if (load_file)
                    if (auto s = load_file(name)) load_scheme(*s);
                return;
            }
        if (in(vx, vy, 378, 413, 391, 426)) load_top_ = std::max(0, load_top_ - 1);
        if (in(vx, vy, 392, 413, 405, 426) && load_top_ + 4 < int(load_rows_.size())) ++load_top_;
        return;
    }
    if (mode_ == "r")
        for (int i = 0; i < int(hq_.projects().size()); ++i)
            if (i != recent_pressed_ && in(vx, vy, 200 + 30.f * float(i), 270, 224 + 30.f * float(i), 294)) {
                // Acts at once: the tree from memory, the chassis tabs.
                recent_pressed_ = i;
                const mission::Scheme s = hq_.projects()[std::size_t(i)].scheme;
                const int keep = i;
                load_scheme(s);
                recent_pressed_ = keep;
                return;
            }
    for (int b = 0; b < 5; ++b)
        if (in(vx, vy, kButtons[b].ix, 450, kButtons[b].ix + 24, 474)) {
            if (b == 0 && !(turret_present_ && has_model())) return; // disabled unless a turret is in and the stats pass
            if (b == 2 && !has_model()) return;
            button_fire_ = b;
            return;
        }
    if (!panel_click(left_, false, vx, vy)) panel_click(right_, true, vx, vy);
}

void Designer::key_escape() {
    if (load_dialog_) load_dialog_ = false;
    else if (save_dialog_) save_dialog_ = editing_ = false;
    else close();
}

void Designer::key_char(char c) {
    // CEditLine_OnChar 10045d50: BS deletes, Enter ends editing, other printable chars appended up to 16.
    if (!save_dialog_ || !editing_) return;
    if (c == '\b') {
        if (!edit_text_.empty()) edit_text_.pop_back();
    } else if (c == '\r' || c == '\n') {
        editing_ = false;
    } else if ((unsigned char)c >= 0x20 && edit_text_.size() < 16) {
        edit_text_ += c;
    }
}

void Designer::update(double now_ms) {
    now_ = now_ms;
    if (!open_) return;
    // The bottom buttons fire on the update after the click.
    if (button_fire_ >= 0) {
        const int b = button_fire_;
        button_fire_ = -1;
        switch (b) {
        case 0: deliver(); return;
        case 1: reset(mode_); break;
        case 2: save_dialog_ = editing_ = true, edit_text_.clear(); break;
        case 3:
            load_rows_ = list_saved ? list_saved() : std::vector<std::string>{};
            load_dialog_ = !load_rows_.empty(), load_top_ = 0;
            break;
        case 4: close(); return;
        default: break;
        }
    }
    if (save_dialog_ && !editing_) {
        if (const auto s = build_scheme(); s && save_file) save_file(edit_text_, *s);
        save_dialog_ = false;
    }
    // The pending part action (§3.3), by the model panel's active tab.
    if (pending_) {
        const Pending p = *pending_;
        pending_.reset();
        const int tab = right_.active();
        Category& c = right_.cats[std::size_t(tab)];
        const bool empty_slot = c.sel >= 0 && c.sel < int(c.rows.size()) && c.rows[std::size_t(c.sel)].item < 0;
        if (!p.remove) {
            if (tab == kChassis ? !has_model() : (tab == kTurret || tab == kWeapon) ? empty_slot : true) add_part(p.item);
        } else if (tab == kChassis ? has_model() : (tab == kTurret || tab == kWeapon) ? !empty_slot : false) {
            remove_part(tab, c.sel);
        }
    }
    // CModelPanel_Update: the row just clicked fills the parts panel and the right preview.
    for (int ci : kClickOrder) {
        Category& c = right_.cats[std::size_t(ci)];
        if (!c.clicked_row) continue;
        c.clicked_row = false;
        if (c.sel < 0 || c.sel >= int(c.rows.size())) continue;
        const Row& row = c.rows[std::size_t(c.sel)];
        build_list(ci, ci == kChassis ? mode_ : row.slot, ci == kWeapon);
        if (row.item >= 0) right_.preview_item = row.item;
    }
    for (int ci = 0; ci < 6; ++ci) {
        Category& c = left_.cats[std::size_t(ci)];
        if (!c.clicked_row) continue;
        c.clicked_row = false;
        if (c.sel < int(c.rows.size()) && c.rows[std::size_t(c.sel)].item >= 0) left_.preview_item = c.rows[std::size_t(c.sel)].item;
    }
}

std::uint32_t Designer::tooltip_at(float vx, float vy) const {
    if (!open_) return 0;
    for (int b = 0; b < 5; ++b)
        if (in(vx, vy, kButtons[b].ix, 450, kButtons[b].ix + 24, 474)) return kButtons[b].tip;
    if (mode_ == "r")
        for (int i = 0; i < int(hq_.projects().size()); ++i)
            if (in(vx, vy, 200 + 30.f * float(i), 270, 224 + 30.f * float(i), 294)) return 1558;
    for (int ci = 0; ci < 6; ++ci)
        for (const float ix : {kTabs[ci].icon_l, kTabs[ci].icon_r})
            if (in(vx, vy, ix, 25, ix + 24, 49)) return kTabText[ci];
    return 0;
}

// --- drawing (§2) ---

void Designer::draw(OverlayFrame& out, const HudContext& c, const HqSprites&, double now_ms) {
    if (!open_) return;
    const VirtualScreen& vs = c.vs;
    auto spr = [&](HudPage pg, float u, float v, float w, float h, float x0, float y0, float x1, float y1, std::uint32_t argb = 0xffffffffu) {
        draw_sprite(out, vs, c.tex, Sprite{pg, u, v, w, h}, x0, y0, x1, y1, argb);
    };
    auto text = [&](float x, float y, const std::string& t, std::uint32_t argb) { draw_text(out, vs, c.font, std::round(x), std::round(y), t, argb); };
    auto text_w = [&](const std::string& t) { return c.font.font ? text_width_px(vs, *c.font.font, t) / vs.sx() : 0.f; };
    const auto str = [&](std::uint32_t id) { return hq_.host().text ? hq_.host().text(id) : std::string(); };
    const float lh = c.font.font ? std::round(float(c.font.font->height + 2) * vs.glyph_scale / vs.sy()) : 10.f;
    // The whole screen is black first.
    fill_rect(out, vs, 0, 0, 640, 480, 0xff000000u);
    // §2.1 the panels' frames, previews and lists.
    auto glow = [&](float x0, float y0, float x1, float y1, std::uint32_t argb, float u, float v) { spr(HudPage::Page7, u, v, 128, 128, x0, y0, x1, y1, argb); };
    for (Panel* pp : {&left_, &right_}) {
        Panel& p = *pp;
        const float X0 = p.x0, X1 = p.x1;
        fill_rect(out, vs, X0 + 11, 61, X1 - 11, 228, 0x3c329632u);
        spr(HudPage::Page4, 0, 44, 183, 61, X0, 0, X1, 61);
        fill_rect(out, vs, X0 + 11, 244, X1 - 11, 396, 0x3c329632u);
        glow(X0 + 11, 244, X1 - 11, 396, 0xff006400u, 128, 128);
        spr(HudPage::Page4, 121, 144, 14, 16, X0, 228, X0 + 14, 244);
        spr(HudPage::Page4, 150, 144, 14, 16, X1 - 14, 228, X1, 244);
        float x = X0 + 14;
        for (; x < X1 - 27; x += 13) spr(HudPage::Page4, 136, 145, 12, 14, x, 229, x + 13, 243);
        spr(HudPage::Page4, 136, 145, 12, 14, X1 - 27, 229, X1 - 14, 243);
        spr(HudPage::Page4, 0, 144, 20, 84, X0, 396, X0 + 20, 480);
        spr(HudPage::Page4, 54, 144, 20, 84, X1 - 20, 396, X1, 480);
        for (x = X0 + 20; x < X1 - 52; x += 32) spr(HudPage::Page4, 21, 145, 31, 83, x, 397, x + 32, 480);
        spr(HudPage::Page4, 21, 145, 31, 83, X1 - 52, 397, X1 - 20, 480);
        // The preview of the selected part, else 5081 in red.
        if (p.preview_item >= 0 && hq_.host().part_mesh) {
            p.preview.mesh = hq_.host().part_mesh(model_of(p.preview_item));
            draw_model_view(out, vs, p.preview, X0 + 12, 244, X1 - X0 - 23, 151, now_ms);
        } else {
            text(X0 + 15, 415, str(5081), 0xffff0000u);
        }
        // The tabs.
        const bool right = &p == &right_;
        for (int ci = 0; ci < 6; ++ci) {
            const Category& cat = p.cats[std::size_t(ci)];
            const TabDef& t = kTabs[ci];
            const float f0 = right ? t.fr0 : t.fl0, f1 = right ? t.fr1 : t.fl1, ix = right ? t.icon_r : t.icon_l;
            const float iy = right && ci == kArmour ? 26 : 25;
            spr(HudPage::Page4, cat.active ? 27.f : 0.f, 106, 26, 37, f0, 16, f1, 53);
            spr(HudPage::Icons, t.iu, t.iv, 24, 24, ix, iy, ix + 24, iy + 24, t.colour[!cat.enabled ? 0 : cat.active ? 2 : 1]);
        }
        // The active category's rows (§2.3).
        const Category& a = p.cats[std::size_t(p.active())];
        const float side = p.x0;
        if (a.rows.empty()) text(10, 6, str(1115), 0xff009600u);
        else text(side + 10, 6, str(right ? 3050 : 3049), 0xffffff00u);
        for (int k = 0; k < 6; ++k) {
            const int i = a.scroll + k;
            if (i >= int(a.rows.size())) break;
            const float y = 65 + 23.f * float(k);
            const bool sel = i == a.sel;
            spr(HudPage::Page4, 0, 22, 180, 21, side, y, side + 180, y + 21);
            spr(HudPage::Page1, sel ? 195.f : 178.f, 141, 17, 21, side, y, side + 17, y + 21);
            text(side + 10, y + 7, a.rows[std::size_t(i)].label, sel ? 0xffffffffu : 0xff9696fau);
        }
        if (a.rows.size() > 6) {
            spr(HudPage::Page4, 0, 0, 130, 21, side + 5, 203, side + 147, 224);
            spr(HudPage::Page1, 178, 141, 17, 21, side, 203, side + 17, 224);
            spr(HudPage::Page1, 216, 78, 15, 21, side + 147, 203, side + 162, 224);
            spr(HudPage::Icons, 154, 0, 11, 11, side + 148, 207, side + 161, 220);
            spr(HudPage::Page1, 231, 78, 18, 21, side + 162, 203, side + 180, 224);
            spr(HudPage::Icons, 154, 11, 11, 11, side + 162, 206, side + 175, 219);
        }
    }
    // §2.4 the model view and the bottom panel.
    fill_rect(out, vs, 195, 10, 445, 301, 0x3c329632u);
    fill_rect(out, vs, 202, 310, 438, 440, 0x3c329632u);
    glow(195, 10, 320, 155, 0xff009b00u, 0, 128);
    glow(445, 10, 320, 155, 0xff009b00u, 0, 128);
    glow(195, 301, 320, 155, 0xff009b00u, 0, 128);
    glow(445, 301, 320, 155, 0xff009b00u, 0, 128);
    spr(HudPage::Page4, 131, 0, 14, 11, 185, 0, 199, 11);
    for (float x = 199; x < 433; x += 8) spr(HudPage::Page4, 146, 0, 8, 10, x, 0, x + 8, 10);
    spr(HudPage::Page4, 146, 0, 8, 10, 433, 0, 441, 10);
    spr(HudPage::Page4, 155, 0, 14, 11, 441, 0, 455, 11);
    spr(HudPage::Page4, 75, 169, 8, 43, 185, 437, 194, 480);
    spr(HudPage::Page4, 154, 169, 5, 43, 263, 437, 269, 480);
    spr(HudPage::Page4, 75, 213, 67, 43, 338, 437, 405, 480);
    spr(HudPage::Page4, 182, 213, 9, 43, 446, 437, 455, 480);
    spr(HudPage::Page4, 234, 137, 9, 119, 185, 315, 194, 437);
    spr(HudPage::Page4, 74, 144, 19, 16, 185, 300, 204, 316);
    for (float x = 204; x < 432; x += 8) spr(HudPage::Page4, 96, 145, 8, 13, x, 301, x + 8, 314);
    spr(HudPage::Page4, 96, 145, 8, 13, 433, 301, 441, 314);
    spr(HudPage::Page4, 105, 144, 15, 15, 440, 300, 455, 315);
    if (!save_dialog_ && !load_dialog_) {
        spr(HudPage::Page3, 41, 140, 32, 115, 195, 320, 227, 435);
        for (float x = 227; x < 359; x += 64) spr(HudPage::Page3, 74, 140, 64, 115, x, 320, x + 64, 435);
        spr(HudPage::Page3, 74, 140, 64, 115, 359, 320, 423, 435);
        spr(HudPage::Page3, 139, 140, 32, 115, 423, 320, 455, 435);
    }
    if (!has_model()) {
        const std::string h = str(mode_ == "r" ? 1107 : 1114);
        text(320 - text_w(h) / 2, 15, h, 0xff00ff00u);
        if (!load_dialog_) {
            const float lh1 = c.font.font ? std::round(float(c.font.font->height + 1) * vs.glyph_scale / vs.sy()) : 9.f;
            int k = 0;
            for (std::uint32_t id : {3044u, 3045u, 3048u}) {
                const std::string t = str(id);
                text(325 - text_w(t), 341 + lh1 * float(k++), t, 0xff00ff00u);
            }
        }
    } else {
        bool ok = false;
        const auto s = build_scheme(&ok);
        if (s && !save_dialog_ && !load_dialog_ && hq_.host().design_stats)
            if (const auto st = hq_.host().design_stats(*s)) {
                // DrawUnitStats(220, 330, model, title) — the title is the design's "%s-%s %s".
                const std::string title = economy::unit_debug_code(st->size_class, st->chassis_type, s->flags) + "-X " +
                                          str(economy::unit_type_name_id(s->flags));
                text(220, 331, title, 0xffffff00u);
                float yl = 330 + 1 + lh + 3;
                auto line = [&](std::uint32_t label, const std::string& value, std::uint32_t ink, const std::string& unit) {
                    text(220, yl, str(label), 0xff00ff00u);
                    text(220 + 80 + std::round(52 - text_w(value)), yl, value, ink);
                    text(220 + 135, yl, unit, 0xff00ff00u);
                    yl += lh;
                };
                char b[64];
                std::snprintf(b, sizeof b, "%-.f / %-.f", st->mass * 0.001, st->payload_free * 0.001);
                line(5069, b, st->valid() ? 0xffb4b4ffu : 0xffff0000u, str(6206));
                std::snprintf(b, sizeof b, "%-.f", st->speed * 3.6);
                line(5070, b, 0xffb4b4ffu, str(6176));
                const HqStatNormalize& nm = hq_.host().stat_norm;
                auto pct = [&](float v, float lo, float hi) {
                    return v >= hi ? std::string("100") : v > lo ? std::to_string(int(std::lround(100.f / (hi - lo) * (v - lo)))) : std::string("0");
                };
                line(5071, pct(st->defence, nm.defence_min, nm.defence_max), 0xffb4b4ffu, "%");
                line(5072, pct(st->offence, nm.offence_min, nm.offence_max), 0xffb4b4ffu, "%");
                std::snprintf(b, sizeof b, "%-.f", st->radar.value_or(0.f));
                line(5073, b, 0xffb4b4ffu, str(6178));
            }
        if (s && hq_.host().design_mesh) view_.mesh = hq_.host().design_mesh(*s);
        draw_model_view(out, vs, view_, 195, 10, 250, 290, now_ms);
        // The prompt by the model panel's active tab.
        const int tab = right_.active();
        const std::uint32_t prompt = tab == kTurret ? 3054 : tab == kWeapon ? 3055 : tab == kChassis ? 0 : 6242;
        if (prompt) {
            const std::string t = str(prompt);
            text(320 - text_w(t) / 2, 15, t, 0xff00ff00u);
        }
    }
    // The scan lines over the model view.
    {
        const float t = float(std::fmod(now_ms, 250.0) / 1000.0);
        const std::uint32_t g = std::uint32_t(std::clamp(std::lround(t * 200 + 150), 0l, 255l));
        for (float x = 195; x < 445; x += 83)
            for (float y = 10; y < 300; y += 72) spr(HudPage::Page3, 172, 140, 64, 64, x, y, x + 83, y + 72, 0xff000000u | g << 8);
    }
    // The dialogs.
    if (save_dialog_) {
        const std::string t = str(3056);
        text(320 - text_w(t) / 2, 326, t, 0xff00ff00u);
        fill_rect(out, vs, 270, 410, 370, 430, 0xff000000u);
        draw_rect_outline(out, vs, 270, 410, 370, 430, 0xff9696fau);
        const std::uint32_t ink = editing_ ? 0xffffffffu : 0xff323232u;
        text(320 - text_w(edit_text_) / 2, 414, edit_text_, ink);
        if (editing_ && std::fmod(now_ms, 1000.0) < 500) {
            const float cx = 320 + text_w(edit_text_) / 2;
            fill_rect(out, vs, cx + 1, 413, cx + 6, 427, 0xff9600ffu);
        }
    }
    if (load_dialog_)
        for (int k = 0; k < 4 && load_top_ + k < int(load_rows_.size()); ++k) {
            const float y = 320 + 22.f * float(k);
            spr(HudPage::Page4, 0, 22, 180, 21, 200, y, 442, y + 21);
            text(210, y + 7, load_rows_[std::size_t(load_top_ + k)], 0xff9696fau);
        }
    // The recent projects (mode 'r') and the five buttons (diffuse 0xff9b9bff; disabled halved).
    if (mode_ == "r")
        for (int i = 0; i < int(hq_.projects().size()); ++i)
            spr(HudPage::Icons, 216, 96, 24, 24, 200 + 30.f * float(i), 270, 224 + 30.f * float(i), 294,
                i == recent_pressed_ ? 0xffffffffu : 0xff80ff80u);
    for (int b = 0; b < 5; ++b) {
        const ButtonDef& d = kButtons[b];
        const bool enabled = b == 0 ? turret_present_ && has_model() : b == 2 ? has_model() : true;
        std::uint32_t diffuse = 0xff9b9bffu;
        if (!enabled) diffuse = ((diffuse >> 1) & 0x7f7f7fu) | 0xff000000u;
        if (b == 4) spr(HudPage::Page4, 141, 213, 41, 43, d.fx0, 437, d.fx1, 480);
        else spr(HudPage::Page4, b % 2 == 0 ? 84.f : 119.f, 169, 35, 43, d.fx0, 437, d.fx1, 480);
        spr(d.page, d.iu, d.iv, 24, 24, d.ix, 450, d.ix + 24, 474, diffuse);
    }
}

} // namespace parkan::ui
