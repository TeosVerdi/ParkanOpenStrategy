// The unit designer ("constructor", CHud+8 CConstructorShell 10055d30 → CConstructor 1004d720): the parts panel
// (left), the model panel with the design's slots (right), the design's 3D view, its stats, the bottom buttons
// (deliver to the plant, delete, save, load, exit), the save / load dialogs and the five recent projects the plant
// panel shares. Spec: notes/designer.md (sections below), notes/mounts-slots.md, notes/unit-stats.md.
//
// SDL-free: the HQ feeds the pointer and keys, the designer keeps the design as an economy::Assembly plus the
// panels' rows, writes schemes through the HQ's recent list, and draws into the overlay.
//
// Port decisions:
// - Files: "units/<name>.dat" (Save / Load) live in the user's preferences folder (the game folder stays read-only);
//   Load lists them together with the game's UNITS folder. temp_unit.dat (Deliver) is kept in the scheme cache.
// - The mount callout lines (§2.5) are not drawn [deferred]; the row labels use one colour (no font gradient).
// - The panels' render-state calls whose arguments are hidden (§2.1 step 4, §2.4 step 3) are left out.
#pragma once

#include "ui/hq.hpp"

namespace parkan::ui {

class HqController;

class Designer {
public:
    // One slot / catalogue row (CPartRow 0x330, §3.1).
    struct Row {
        std::string slot;        // +0xc4: slot / mount name (parts rows: the query prefix)
        std::int32_t parent = -1, mount = -1; // +0xd0 / +0xd4
        std::string label;       // +0xd8: "%s (%s)" of the item's name and code
        std::int32_t item = -1;  // +0xf0
        std::int32_t comp = -1;  // +0xf4: component id in the design
        bool model_row = false;  // +0xc0: 1 in the model panel (remove), 0 in the parts panel (add)
    };
    // A category of a panel (0xcea4 bytes): tab, ≤ 64 rows, scroll top, selection.
    struct Category {
        std::vector<Row> rows;
        int sel = 0, scroll = 0;
        bool enabled = false, active = false, clicked_row = false;
        double last_select_ms = -1e9;
    };
    // The six categories in drawing / panel order (designer.md §0): 0 chassis, 1 turrets, 2 armour, 3 internal,
    // 4 weapons, 5 ammo.
    static constexpr int kChassis = 0, kTurret = 1, kArmour = 2, kInternal = 3, kWeapon = 4, kAmmo = 5;
    struct Panel {
        std::array<Category, 6> cats;
        float x0 = 0, x1 = 183;
        ModelView preview;       // the part preview (X0 + 12, 244), (X1 − X0 − 23) × 151
        std::int32_t preview_item = -1;
        double scan_t = 0;       // the scan-line pulse timer (+0x4e000)
        int active() const;
    };

    explicit Designer(HqController& hq) : hq_(hq) {}
    bool open() const { return open_; }
    // CConstructorShell_Open 10056090(kind 1 = plant, mode "r", plant class = its size class).
    void open_for_plant(std::uint32_t plant_class);
    void close(); // CConstructorShell_Close 10056120
    // Input while open: everything is consumed.
    void lbutton_down(float vx, float vy);   // CConstructorShell_OnLButtonDown 10056420
    void key_escape();                        // CConstructorShell_OnKeyDown 100562a0 (Esc)
    void key_char(char c);                    // CConstructorShell_OnChar 100563d0 (save dialog; '\b', '\r')
    void update(double now_ms);               // CConstructorShell_Update 10056260 → CConstructor_Update 10050af0
    void draw(OverlayFrame& out, const HudContext& c, const HqSprites& sp, double now_ms); // CConstructorShell_Draw 100561e0
    std::uint32_t tooltip_at(float vx, float vy) const;

    // Model operations (§3.4–§3.8), public for the tests.
    void add_part(std::int32_t item);            // CConstructor_AddPart 10051e00
    void remove_part(int cat, int row);          // CConstructor_RemovePart 10053e70
    std::optional<mission::Scheme> build_scheme(bool* ok = nullptr) const; // CConstructor_SaveScheme's tree + validity
    void load_scheme(const mission::Scheme& s);  // CConstructor_LoadScheme 100555b0
    void build_list(int cat, const std::string& prefix, bool weapon_mode); // CPartCategory_BuildList 10048680
    const Panel& parts() const { return left_; }
    const Panel& model() const { return right_; }
    const economy::Assembly* assembly() const { return asm_ ? &*asm_ : nullptr; }
    bool has_model() const { return asm_ && !asm_->empty(); }
    std::uint32_t header() const; // the scheme header (§1.7)
    // Directories the save / load dialogs use (port: the preferences folder's units/ and the game's UNITS).
    std::function<std::vector<std::string>()> list_saved; // names without "units/" and ".dat"
    std::function<bool(const std::string& name, const mission::Scheme& s)> save_file;
    std::function<std::optional<mission::Scheme>(const std::string& name)> load_file;

private:
    HqController& hq_;
    bool open_ = false;
    std::string mode_ = "r";
    std::uint32_t plant_class_ = 0;
    Panel left_, right_;
    std::optional<economy::Assembly> asm_;
    ModelView view_;
    // The pending part action (DAT_101091b0): add from the parts panel / remove from the model panel.
    struct Pending {
        bool remove = false;
        std::int32_t item = -1;
    };
    std::optional<Pending> pending_;
    int button_fire_ = -1; // the bottom button clicked (fires on the next update)
    bool save_dialog_ = false, load_dialog_ = false, editing_ = false;
    std::string edit_text_;
    std::vector<std::string> load_rows_;
    int load_top_ = 0;
    int recent_pressed_ = -1;
    bool turret_present_ = false; // ad7f
    double now_ = 0;

    void reset(const std::string& mode);          // CConstructor_Reset 1004e410
    void set_chassis(const std::string& entry, std::int32_t item); // 10051fd0
    void attach_turret(std::int32_t item);        // 10052990
    void attach_weapon(std::int32_t item);        // 100533d0
    void attach_into(int cat, std::int32_t item); // AttachModule 10053130 / AttachArmour 10053bd0 / AttachAmmo 10053930
    void add_default_part(int cat, int row);      // "<slot>_df"
    void remove_turret(int row);                  // 10054210
    void remove_weapon(int row);                  // 10054630
    bool panel_click(Panel& p, bool model_panel, float vx, float vy); // CPartPanel_OnLButtonDown 10049fa0
    void deliver();                               // §1.7
    std::string model_of(std::int32_t item) const;
    std::string label_of(std::int32_t item) const;
    int kind_of(std::int32_t item) const;         // PartInfo_GetPartKind 1008aab0
    std::uint32_t mask_of(std::int32_t item) const; // PartInfo_GetTypeMask 1008a9b0
    void activate_tab(int cat);                   // both panels (linked tabs)
};

// PartInfo_GetPartKind / GetTypeMask over a TRF item's fields (category +0x23, subtype +0x24, byte +0x25, size
// +0x26, turret kind +0x22).
int part_kind(const mission::ResearchItem& it);
std::uint32_t part_type_mask(const mission::ResearchItem& it);
// OverlayMountNameOnModel 10090a10: the model name with its first len(slot) characters replaced by the slot name.
std::string overlay_mount_name(const std::string& model, const std::string& slot);
// StrPrefixMatchWildcardNoCase 10090b50: the first min(len) characters equal case-insensitively, '?' matching anything.
bool prefix_match(std::string_view a, std::string_view b);

} // namespace parkan::ui
