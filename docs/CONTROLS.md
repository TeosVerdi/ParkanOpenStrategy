# Controls

The port's default is a **modern layout** (WASD). `parkan --play … --classic-keys` uses the original
2001 layout instead (with the keys a MacBook lacks moved, as in `tools/mac_keymap.py`).

Both layouts are the original binding tables (`HERO.TBL`, `M1.TBL`, `M2.TBL`) with keys substituted
when they load (`src/input/keymap.cpp`), so every command keeps the original's behaviour (ramps,
one-shots, mouse smoothing). The game folder is not modified.

Two port additions in the modern layout: **Option** is a bindable key (the original ignored Alt), and
**sprint**: each movement binding walks at half its original throttle and has a Shift twin with the
full value, so Shift switches gait while a movement key is held (the original's modifier handling).

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
| Fire selected weapons | left mouse | left mouse |
| Select weapon 1–8 / all | 1–8 / 0 | 1–8 / 0 |
| Infrared / camouflage / repair | N / H / R | N / H / R |

## Interface (recorded for the interface layer; not all are live in `--play` yet)

| Command | Modern | Classic |
|---|---|---|
| Outer (third-person) camera, cycles 4 positions | **V** | C |
| Zoom | **right mouse** | Z |
| Aim at target | **T** | right mouse |
| Select target / enemy / friend | Tab / **G** / F | Tab / E / F |
| Autopilot | **P** | A |
| Wingman menu | `` ` `` | `` ` `` |
| Satellite map, map transparency | M, `[` `]` | M, `[` `]` |
| Mission objectives | **O** | F12 |
| Quick save / load | **F5 / F9** | F7 / F8 |
| Chat terminal / new message | F4 / **Y** | F4 / F5 |
| Help, pager, game menu | F1, F2, F3 | F1, F2, F3 |
| HQ camera move / up-down / rotate | **WASD / Space, Ctrl / Q, E** | arrows / `'` `/` / `,` `.` |
| Confirm / back | Enter / Esc | Enter / Esc |

Port-only: **F10** toggles a free orbit camera around the hero (debugging aid).
