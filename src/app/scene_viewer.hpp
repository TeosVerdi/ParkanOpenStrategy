// Interactive viewer for one RenderMesh (a model or a map's terrain): checks the asset pipeline end
// to end on the GPU before the game renderer exists.
#pragma once

#include "scene/render_mesh.hpp"
#include "scene/sky.hpp"

#include <SDL3/SDL.h>

#include <functional>

namespace parkan {

// Per-frame sky, filled by ViewerOptions::sky: the lighting and the dome's vertex colours.
struct SkyFrame {
    SceneLighting lighting;
    Vec3 cloud_color{1, 1, 1};
    double ms = 0; // sky clock (UV scrolling)
};

struct ViewerOptions {
    SceneLighting lighting;
    std::string title;
    std::string shot_path; // render one frame offscreen to this BMP and return
    float pitch = 0.4f;    // initial camera elevation (radians)
    float distance = 2.5f; // initial distance in bounding radii
    // Optional: rebuild the mesh for a model state (0..2) and LOD (0..4); keys S and L cycle them.
    std::function<RenderMesh(int state, int lod)> reload;
    // Optional: called every frame with the elapsed seconds to update the mesh transforms (animation).
    // Space pauses it.
    std::function<void(double seconds, std::vector<Mat4>& transforms)> animate;
    double start_time = 0; // animation clock at the first frame (and for --shot)
    // Optional: '[' and ']' call this with -1/+1 (e.g. to switch motion states); returns the title suffix.
    std::function<std::string(int step)> cycle;
    // Optional sky: textures (nebula, stars, clouds) and a per-frame update given the elapsed seconds,
    // the view direction and the dome to colour. When set, it also supplies the lighting.
    SkyTextures sky_textures;
    std::function<void(double seconds, Vec3 forward, SkyDome& dome, SkyFrame& out)> sky;
    // Optional: every SDL event goes here first (game input).
    std::function<void(const SDL_Event&)> on_event;
    // Optional: after `animate`, may move the orbit centre (a camera following a unit).
    std::function<void(Vec3& centre)> follow;
    float orbit_distance = 0; // initial orbit distance in world units; 0 = from the mesh bounds
    // Optional: a game camera. When it returns true the orbit camera is replaced for this frame.
    struct Camera {
        Vec3 eye, forward, up{0, 0, 1};
        float fov_x = 1.3f; // horizontal field of view (radians)
        float znear = 0.1f, zfar = 1000.f;
    };
    std::function<bool(Camera&)> camera;
    bool relative_mouse = false; // capture the mouse (game mouse look)
};

// Mouse drag orbits, the wheel zooms, Esc quits.
int run_scene_viewer(SDL_Window* window, RenderMesh mesh, const ViewerOptions& opt);

} // namespace parkan
