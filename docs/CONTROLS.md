# Controls

The port's default is a **modern layout** (WASD). `parkan --play … --classic-keys` uses the original
2001 layout instead (with the keys a MacBook lacks moved, as in `tools/mac_keymap.py`).

Both layouts are the original binding tables with keys substituted when they load
(`src/input/keymap.cpp`), so every command keeps the original's behaviour (ramps, one-shots, mouse
smoothing). As in the original, a mission compiles `HERO.TBL` and `M1.TBL` from the options menu's
lists (`hero.man`, `table_1.man`) and takes the interface keys from `addition.man`
(`src/input/man_tables.cpp`). The game folder is not modified.

Two port additions in the modern layout: **Option** is a bindable key (the original ignored Alt), and
**sprint**: each movement binding walks at half its original throttle and has a Shift twin with the
full value, so Shift switches gait while a movement key is held (the original's modifier handling).

## Menus (the game shell)

As in the original (iron3d's GUI, notes/shell.md):

| Input | Effect |
|---|---|
| Mouse over a control | selects it (the same as keyboard focus); it blinks and the `i_hilit` sound plays |
| Left click (on press), **Enter**, **Space** | activate the selected button, list row or checkbox |
| **Up / Down**, **Tab** | previous / next control (Up/Down inside a list move between its rows) |
| **Left / Right** | sliders and selectors (held: auto-repeat every 0.1 s); bar buttons |
| **Esc** | back (the main menu asks to quit) |
| **Y / N** | answer the quit question |
| Any key or left click | skip the current intro movie (each movie separately) |
| Esc, Space or a click | leave the credits |
| **Esc** during a briefing | skip it (no other input works while it plays) |

Key bindings (Options → Controls): click a row or press Enter to capture the next key or mouse button
(Esc included, as in the original); Delete or a right click clears it. Conflicting keys show in red and
disable OK. OK saves the edited `ui_*.man` files and the four merged in-game lists to the user folder,
and missions use them from then on. Saved bindings are used as shown in the menu: the modern layout
no longer applies, and only the keys a MacBook lacks (keypad, Page Up/Down) still move. To go back to
a layout, delete `addition.man`, `table_1.man`, `table_2.man` and `hero.man` from the user folder
(`~/Library/Application Support/Parkan`).

## Driving (hero and warbots)

| Action | Modern | Classic |
|---|---|---|
| Walk forward / back | **W / S** (half throttle) | ↑ / ↓ (full throttle) |
| Strafe left / right | **A / D** (half throttle) | ← / → |
| Sprint | **hold Shift** with W/S/A/D (full throttle) | — |
| Turn (hero) | mouse left/right | mouse left/right |
| Turn (warbots, keyboard) | **Q / E** | `,` / `.` |
| Up / down (flyers) | **Space / Left Ctrl** | `'` / `/` (PgUp/PgDn) |
| Cruise: full speed (latches) | **C** | `\` (Num *) |
| Cruise: faster / slower | `=` / `-` | `=` / `-` (Num + / −) |
| Cruise: stop | **X** | Backspace (Num /) |
| Aim (barrel up/down) | mouse up/down | mouse up/down |
| Turret left/right (warbots) | mouse left/right | mouse left/right |
| Lock turret to the body | L | L (Num 5) |
| Free look | **hold Option** + mouse; release recentres | hold Left Shift + mouse |
| Recentre free look | Option + right mouse | Left Shift + right mouse |
| Fire selected weapons (live in `--play`) | left mouse | left mouse |
| Toggle weapon 1–8 / select all (live; the hero starts with weapon 1) | 1–8 / 0 | 1–8 / 0 |
| Infrared / camouflage / repair | N / H / R | N / H / R |

## Interface (recorded for the interface layer; not all are live in `--play` yet)

| Command | Modern | Classic |
|---|---|---|
| Outer (third-person) camera, cycles 4 positions | **V** | C |
| Zoom (live: the sight FOV steps to 0.2 rad) | **right mouse** | Z |
| Aim at target (live: selects the unit under the sight) | **T** | right mouse |
| Select target / enemy / friend (live: radar-detected units) | Tab / **G** / F | Tab / E / F |
| Autopilot | **P** | A |
| Wingman menu | `` ` `` | `` ` `` |
| Satellite map, map transparency (live) | M, `[` `]` | M, `[` `]` |
| Cockpit instruments on/off (live) | U | U |
| Mission objectives (live in `--play`) | **O** | F12 |
| Quick save / load | **F5 / F9** | F7 / F8 |
| Chat terminal / new message | F4 / **Y** | F4 / F5 |
| Help, pager, game menu | F1, F2, F3 | F1, F2, F3 |
| HQ camera move / up-down / rotate | **WASD / Space, Ctrl / Q, E** | arrows / `'` `/` / `,` `.` |
| Confirm / back | Enter / Esc | Enter / Esc |

Port-only: **F10** toggles a free orbit camera around the hero (debugging aid). The cockpit camera is
stabilised against the walk clips' pelvis twist (the turret is mounted under the chassis root, which
the walk keys yaw by up to ~10°); `--camera-sway` restores the data-driven sway.
