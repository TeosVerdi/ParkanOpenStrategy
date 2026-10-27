# Parkan port: working rules

The native macOS port of Parkan: Iron Strategy (C++20 + SDL3). Specs and the progress log live in
`~/parkan-re/notes/` (PROGRESS.md, BACKLOG.md); decompile dumps in `~/parkan-re/notes/decomp/`.

## Delegation: route work by difficulty, not by habit

The main session orchestrates. It writes briefs, reviews results and commits; it does not do tedious work itself.
Every Agent call passes `model` explicitly. OMC injects models only for its own agents, so an explicit model is the
only reliable route.

| Work | Agent (`subagent_type`) | Model |
|---|---|---|
| Tracing a feature in Ghidra across modules; unclear scope; conflicting notes; architecture; debugging a cause nobody understands | `parkan-decoder` | opus |
| Decoding one or a few known functions (addresses given), or from `notes/decomp` dumps | `parkan-reader` | sonnet |
| Implementing from a spec section, with tests | `parkan-porter` | sonnet |
| Builds, test and check runs, greps over notes or dumps, backlog scans, log and PROGRESS entries | `parkan-checker` | haiku |
| Reviewing a diff against its spec (give it the code and the spec, not your conclusion) | `oh-my-claudecode:verifier` | sonnet |
| Plans for large features (save/load, multiplayer, the 2001 ruleset) | `oh-my-claudecode:planner` | opus |
| Finding where something is in the code | `oh-my-claudecode:explore` | haiku |

Rules:
- **Ghidra:** only one Ghidra agent runs at a time; there is a single instance. Never call `save_all_programs`; save
  through `run_ghidra_script` with `SaveOpenPrograms.java` (see the agent files).
- **Pipeline per item:** decode (notes spec), then port (code and tests), then review, then commit by the main session.
  Item N's port can overlap item N+1's decode.
- **Escalation:** when a cheaper agent reports that the scope is larger than briefed, or a review fails twice,
  escalate one tier. Don't retry at the same tier.
- **Briefs:** point at files and addresses instead of pasting content. Ask for replies under 300 words.
- **Do it yourself only when quicker:** a one-line fix or a command whose output you need right now.

## Standing rules (also in each agent file)
- Fidelity: 1:1 with the original apart from the QoL changes the user asked for. Every rule comes from the decompiled
  code with its address; unverifiable points are marked `[inferred]`; no invented constants. The port follows the
  data of the edition it runs on.
- The game folders and disc images are read-only. Inside the CrossOver folders, list with Python `os.listdir`;
  `ls` hangs.
- `parkan_core` stays SDL-free; no Metal-only features (Windows and Linux come last).
- Commit only milestone commits to main, ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
  Don't push.
- Verify: `cmake --build build`; `build/parkan-tests` with no arguments, then with the RU folder, then with the ENG
  folder; `build/parkan-check` on RU and ENG. The line "unused BULL with missing data: bm_b_02" is expected.
