# Parkan port: architecture

This is a native reimplementation of *Parkan: Iron Strategy* (Nikita, 2001) in C++20 + SDL3. It
loads the original data from a GOG install. The decompilation notes in `~/parkan-re/notes/` are the
specification; each module says which notes it implements.

## Build

```
cmake -S . -B build -G Ninja && cmake --build build
./build/parkan-check "<game dir>"     # parses every supported data file, prints corpus totals
open build/parkan.app                 # or ./build/parkan.app/Contents/MacOS/parkan <game dir>
./build/parkan.app/Contents/MacOS/parkan --view bases.rlb r_l_01.msh     # model viewer
./build/parkan.app/Contents/MacOS/parkan --view bases.rlb r_l_01.msh --state 39   # play motion state 39
./build/parkan.app/Contents/MacOS/parkan --view fortif.rlb fr_b_plant.msh --shot out.bmp
./build/parkan.app/Contents/MacOS/parkan --unit UNITS/AUTO/32_win_l2.dat  # unit design with turret + guns
./build/parkan.app/Contents/MacOS/parkan --terrain DATA/MAPS/Tut_2         # terrain viewer
./build/parkan-dump "<game dir>" nodes bases.rlb r_l_01.msh   # mesh node table (mount points)
./build/parkan.app/Contents/MacOS/parkan --mission MISSIONS/CAMPAIGN/CAMPAIGN.02/Mission.01   # whole mission
./build/parkan.app/Contents/MacOS/parkan --play MISSIONS/CAMPAIGN/CAMPAIGN.00/Mission.02     # drive the hero
```

**Model viewer.** Drag to orbit, scroll to zoom, L/S cycle LOD/state, `[`/`]` cycle motion states,
Space pauses the animation (`--time <s>` sets the clock for `--shot`). `--shot` renders one frame
offscreen to a BMP and exits, which is how visual changes get checked.

**Data facts settled while porting** (not in FParkan, or corrected):
- MSH key quaternions are stored W, X, Y, Z; the static pose applies the conjugate; parents come first.
- Node records are 38 bytes. The single 24-byte-node mesh (`system.rlb` MTCHECK.MSH) has its nodes dropped.
- MAT0 version 6 (every shipped material) has a 10-byte extension after the 4-byte header.
- `Land.msh` faces (type 21, 28 bytes) are: u32 flags (0x400 = detail on), u16 material tag (high byte
  Land1.wea row, 0xff = none; low byte Land2.wea row), u16 0xffff, 3 vertices, 3 neighbours, an int16
  face normal and a u16 class.
  - Type 5 is a map-wide UV (about x/256), type 18 a signed tiling UV, type 14 a per-vertex float
    layer weight.
  - Nodes: slot[0] = LOD0 face range, slot[1] = LOD1.
  - All 33 maps decode (299,450 vertices, 275,882 faces).
- Terrain shading follows notes/terrain-render.md: base Land1[lo] + overlay Land1[hi] (alpha =
  type 14) on the type-5 UV, ×2 detail Land2[row] phase 1 on the type-18 UV (face flag 0x400);
  slot ranges index the type-11 draw list. Lighting and fog are the original CPU formulas, done in
  the vertex shader (sun + fill light from sky.ske, ambient as a floor, overbright compression,
  quadratic fog).
- Sky (`mission/ske`, `scene/sky`; notes/atmosphere-render.md): per mission, `sky.ske` keyframes +
  `sky.wea` textures; procedural camera-centred dome. `--time <s>` sets the clock, `--no-fog`
  disables fog for overview shots, `--pitch <rad>` sets the camera elevation.
- `Land.map` (ArealMap areas/grid) is not loaded yet.
- Object resolution (`scene/mission_scene`):
  - A TMA resource is either an `objects.rlb` prototype or a unit `.dat`.
  - Prototype records: STAT/BTLU/INTO/EXTO/BULL/WPNS keep (lib, entry) pairs of 64 bytes with the
    mesh first and the WEAR second. FORT's first pair names another prototype (the building's agent);
    its second is the `.bas` foundation.
  - `.dat` (unit design, "scheme"; `mission/scheme`, ArealMap `CreateObjectFromScheme` 10014fd0) =
    u32 0xF0F1, u32 flags, then a node tree: {lib[32], entry[32], u32 1, i32 mount, name[32],
    u32 category, u32 child_count, children…}. The root is the base object.
  - External components (prototype 4CC `EXTO`: turrets, guns) mount on a node of their parent's
    mesh: `mount` is the node index, and the child's root node is replaced by that node (AniMesh
    `AttachChild`; Control merges CTPT the same way). Internal ones (`INTO`) use `mount` as an
    internal slot and are not drawn. `category` is not a type (it differs between files for the
    same part).
  - All 425 designs parse; 1,279 external components mount on valid nodes. `mas_l_n1.dat` and
    `tel_l_n1.dat` name prototypes missing from `objects.rlb` (unused designs).
  - Placement is T · Rz·Ry·Rx · S (iron3d 0x36610).
  - All 864 objects of the 29 missions resolve.

`ctest --test-dir build` runs the unit tests. `./build/parkan-tests <game dir>` also compiles every
formula and runs every event of every shipped script against a stub host.

**Game data.** It comes from `argv[1]`, else `$PARKAN_DATA`, else the default CrossOver path
`~/Library/Application Support/CrossOver/Bottles/ParkanIS/drive_c/GOG Games/Parkan - Iron Strategy`
(Russian GOG 2.0.0.4; `--lang en`, or no Russian install, picks `… - ENG`, English 2.0.0.3). Both
releases pass `parkan-check` and `parkan-tests`; their differences are in notes/releases.md.
Paths inside the data are resolved case-insensitively (`core/paths`).

## Modules (original binary → port directory → spec)

| Original | Port | Spec (notes/) | Status |
|---|---|---|---|
| — | `core/` bytes, paths, CP1251 | — | done |
| Ngi32 `INgiResFile`, services `CResManager` | `res/` NRes, Texm, MSH, WEAR, MAT0, archive library | ngi32-platform.md, FParkan texm/msh/materials | done: every MESH/Texm/MAT0/WEAR in the game decodes |
| Ngi32 render/sound/input, Joystick | `app/` for now (SDL3, SDL_GPU→Metal, MSL shaders) | ngi32-platform.md, input-system.md | model viewer |
| World3D | `world/` object queue, ids, messages | world3d-runtime.md, components.md | — |
| Terrain | `res/terrain` (Land.msh), `scene/render_mesh`; atmosphere/sky to do | FParkan tomes/04-world, 05-render | LOD0 renders |
| AniMesh, Control | `scene/model` (static pose) → `anim/` motion graph, mover | control-physics.md, formats-wea-cpt-ndp.md | static models render |
| Behavior | `behavior/` unit brains, orders, tasks | behaviour-data.md | — |
| ai | `ai/` script VM; `game/` world model, SuperAI per clan, MissionRuntime | script-vm.md, script-handlers-a/b.md | VM done; 61/73 handlers, 12 neutral (super_ai.hpp); wired into play |
| iron3d HUD text | `res/tft`, `ui/hud_text` (pager, objectives screen) | rsli-font.md | done for the pager and objectives; frame art inferred |
| MisLoad, ArealMap | `mission/` TMA, research | research-tree.md, mission-data.md, FParkan tma | TMA, schemes + research done; missions render |
| iron3d | `game/`, `shell/` | ui-shell.md | — |

## Rules

- Loaders are byte-exact and strict. They throw `FormatError` on anything unexpected, and
  `parkan-check` totals must match the Python reference tools in `~/parkan-re/tools`.
- Game logic follows the decompiled behaviour, including original quirks that affect gameplay. Each
  deliberate deviation is marked with a comment that says so.
- Keep platform code (SDL) out of `parkan_core`.

## Animation (`scene/model`, `res/ctld`)

- Sampling follows AniMesh `CAniNode::SampleKey` (10012880): frame = round(t − 0.5) (ties to even)
  indexes the type-19 map from the node's `anim_map_start`; the mapped key k and k+1 are lerped
  (position) and slerped (Ngi32 `QuatSlerp`) at t by their own key times. The fallback key is used
  for t < 0, a frame ≥ frame count, no map, or k ≥ the node's fallback key.
  `CAniNode::SampleBlend` (10012560) cross-fades two times (tracks A and B of a motion state).
- `CAniMesh::UpdateNodeMatrices` (10008b30) zeroes a root node's translation: root motion moves the
  body (Control's mover), not the mesh. `pose_model` does the same.
- A model's clips are the motion states of its CTLD (`<base>.ctl` next to the mesh). Track B holds
  the clip; track A is usually {0,0}. Walking is a chain of sub-states (r_l_01: states 19–23 cover
  frames 10→14). All 531 CTLD entries (1,690 states) parse byte-exactly.

## Play mode (`app/play`)

- The hero is the TMA object whose design is under `UNITS\UNITS\HERO` (iron3d uses unit type
  0x1020000). It is drawn as posable parts appended to the static scene.
- Per ≤33 ms simulation step: SDL key → `input::scan_from_hid` → `ManManager` (M1.TBL + MacBook remap)
  → `CommandState` → `UnitMotion::set_throttle`/`set_angular_command` (ExecuteBinding's vehicle
  mapping) → `UnitMotion::update` → ground offset from the feet → `pose_unit`.
- The hero uses HERO.TBL, rebound to the modern WASD layout by default (`--classic-keys` for the
  2001 one); see docs/CONTROLS.md and `input/keymap`. Mouse X turns the body (ANGLE_Z → attitude
  target), mouse Y raises the barrel (turret ANGLE_Y). JAMES_OUTER_CAMERA (V, classic C) cycles the
  OUTER presets; F10 toggles the port's debug orbit camera.
- Live units (`world/live_unit`): every TMA object whose design is under `UNITS\UNITS` (hero,
  robots, animals) is a posable model + UnitMotion + reduced FollowGround + collision (static walls,
  world bounds, unit-vs-unit spheres with the mass² split).
- Turret and cameras (`control/turret`; notes/turret-camera.md): joints play their node's keyframe
  range (t per node); the TURRET view sits at CameraCenter looking along TargetDirect (FOV 1.3,
  near 0.1, far 1000); the own unit is not drawn from it (inferred). Strafing is the attitude offset
  (IControl +0x50), added to the chassis yaw after the rate clamp and counter-rotated by the turret.
- `--walk` holds forward (for `--shot` checks); `--time` runs the simulation to that time first.
- Collision (`world/collision`; control-physics.md "Collision"): positional only. The hero's sphere
  (around its chassis bounds) is pushed out of the static objects' near-vertical faces
  (|n.z| < 0.7) and kept inside the landscape box (ContainInWorldBounds), and the push is made
  horizontal for ground states (flag 4). Unit-vs-unit (SweptSphereTOI + mass² split) is implemented
  as functions, waiting for other moving units.
