// The mission sky (Terrain.dll CAtmosphere/CSky): a procedural dome coloured from sky.ske, the scene
// lighting and fog derived from it, and the sky.wea textures. Spec: notes/atmosphere-render.md.
#pragma once

#include "mission/ske.hpp"
#include "scene/render_mesh.hpp"

namespace parkan {

// Scene lighting and fog as the shaders consume them.
struct SceneLighting {
    Vec3 sun_dir = normalize(Vec3{-0.4f, 0.5f, -0.8f}); // direction the light travels
    Vec3 sun_color{1, 1, 1};
    Vec3 fill_dir{0, 0, 1};
    Vec3 fill_color{0, 0, 0};
    Vec3 ambient{0.2f, 0.2f, 0.2f}; // CShader default; a floor, not an addend
    Vec3 fog_color{0.18f, 0.2f, 0.24f};
    float fog_start = 0, fog_end = 0; // fog_end 0 = no fog
    Vec3 clear_color{0.18f, 0.2f, 0.24f};
    // The camera's flag 0x20 for this scene (notes/infrared-camera.md): the lit shader converts the material and
    // ambient colours; fog and clear colours are converted by night_vision_lighting.
    bool night_vision = false;
};

// BuildSkyDome (10047a90): Z-up spherical cap, h = 10000, cap angle π/4, 5 rings, `slices` slices
// (2^AtmSkyDetail, default 16), apex first. uv0 = (x/R, y/R) (nebula), uv1 = ×15 (stars); the cloud
// layer uses ×3. Positions are relative to the camera.
struct SkyDome {
    static constexpr int kRings = 5;
    static constexpr float kHeight = 10000.f, kCloudDrop = 5000.f;
    int slices = 16;
    float radius = 0; // R = h / (2 sin²(cap/2)) ≈ 34142
    std::vector<RenderVertex> vertices;
    std::vector<std::uint32_t> indices;
};
SkyDome build_sky_dome(int slices = 16);

// UpdateSkyDome (10048b90): per-vertex ARGB from the zenith/high/mid/low colours, interpolated
// around the azimuth; the rim gets `rim` (the fog colour, as RenderSkyDome writes it).
void colour_sky_dome(SkyDome& dome, const mission::AtmosphereState& a, std::uint32_t rim);

// Fog colour: the horizon colour in the viewing direction (RenderSkyDome step 1). Computed from the
// dome's quadrant layout and the view direction (the native code uses the camera yaw).
std::uint32_t horizon_colour(const mission::AtmosphereState& a, Vec3 forward);

// Lighting for terrain and models: sun light A along the body's path, fill light B opposite, the
// ambient floor and the fog (start/end × 700 from the ske).
SceneLighting scene_lighting(const mission::AtmosphereState& a, Vec3 forward);

// Night vision (infrared-camera.md §4.3): RenderSkyDome 10048110 passes every dome vertex colour and the fog colour
// through ConvertToNightVisionColor; the fog colour is also the clear colour. The nebula constant colour becomes
// 0xFF00FF00 (SkyDome nebula_tint).
void night_vision_dome(SkyDome& dome);
void night_vision_lighting(SceneLighting& l);
constexpr float kNightVisionNebulaTint[4] = {0, 1, 0, 1};

Vec3 argb_to_rgb(std::uint32_t argb);

// sky.wea rows 0 (nebula), 1 (stars), 2 (clouds): phase-0 textures through Material.lib.
struct SkyTextures {
    std::vector<ModelTexture> textures; // [0] nebula, [1] stars, [2] clouds (empty rgba = missing)
};
SkyTextures load_sky_textures(AssetLibrary& lib, std::string_view mission_dir);

} // namespace parkan
