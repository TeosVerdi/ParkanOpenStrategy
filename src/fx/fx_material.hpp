// Effect materials: a Material.lib (MAT0) material sampled at an animation phase, the way the effect
// sprites see it (notes/effects.md §5).
//
// - CFxManager_GetEffectMaterial (Effect 10006400) wraps the name in a one-row WEAR text
//   ("1\r\n0 %s\r\n") and hands it to IMatManager::ParseWear; materials come from the global
//   Material.lib whatever the archive field says.
// - Each draw asks World3D GetMaterialPhaseAt (10003680) for phase p in [0, 1]: time =
//   p × end time of the last key of animation block 0; the key containing that time is
//   interpolated with the next one by the block's blend mask (FUN_10003030; mask bit 1 =
//   emissive, 2 = diffuse, 4 = specular, 0x10 = opacity), and its texture frame is used as is.
// - The sprite then overwrites the phase's opacity with its own alpha (FxBillboard_Draw 10009930).
// - Blend: material category (NRes attr1 >> 2 & 0xF) through CShade's translate table
//   (InitAlphaBlendModeTranslateTable, Terrain 1002a620): 1 → preset 4 alpha blend, 2 → 2 additive,
//   3 → 3 multiply, 4 → 5 2× multiply; 0 is opaque.
#pragma once

#include "res/texm.hpp"
#include "scene/model.hpp"

#include <map>

namespace parkan {
class AssetLibrary;
}

namespace parkan::fx {

enum class FxBlend : std::uint8_t { Opaque, Alpha, Additive, Multiply, Multiply2x };

struct FxMaterialSample {
    float rgb[3] = {1, 1, 1}; // emissive colour (effect materials carry their colour there)
    float diffuse[3] = {0, 0, 0};
    float opacity = 1;
    float uv0[2] = {0, 0}, uv1[2] = {1, 1}; // the frame's rectangle in the texture
    std::int32_t texture = -1;              // FxMaterialLibrary::textures index
};

struct FxMaterial {
    std::string name;
    FxBlend blend = FxBlend::Opaque;
    struct Phase {
        float emissive[3]{}, diffuse[3]{}, specular[3]{}, opacity = 1;
        int frame = -1;          // texture page, -1 = whole texture
        std::int32_t texture = -1;
    };
    std::vector<Phase> phases;
    struct Key {
        std::uint16_t phase = 0, end_ms = 0;
    };
    std::vector<Key> keys;         // animation block 0
    std::uint32_t blend_mask = 0;  // block header >> 3
    // Phase p in [0, 1] (outside: 0.5, as GetMaterialPhaseAt does).
    FxMaterialSample sample(float p, const std::vector<ModelTexture>& textures,
                            const std::vector<std::vector<TexmPageRect>>& pages) const;
};

class FxMaterialLibrary {
public:
    explicit FxMaterialLibrary(AssetLibrary& lib) : lib_(lib) {}
    // Index of the material, -1 when neither it nor DEFAULT exists. Cached by name.
    std::int32_t get(const std::string& name);
    const FxMaterial& material(std::int32_t i) const { return materials_[std::size_t(i)]; }
    FxMaterialSample sample(std::int32_t i, float phase) const {
        return materials_[std::size_t(i)].sample(phase, textures, pages);
    }
    std::vector<ModelTexture> textures;             // RGBA level 0 of every texture used
    std::vector<std::vector<TexmPageRect>> pages;   // per texture: Texm "Page" frames

private:
    std::int32_t texture(const std::string& name);
    AssetLibrary& lib_;
    std::vector<FxMaterial> materials_;
    std::map<std::string, std::int32_t> by_name_, tex_by_name_;
};

} // namespace parkan::fx
