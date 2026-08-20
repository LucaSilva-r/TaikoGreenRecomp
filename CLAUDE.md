# CLAUDE.md

Project overview, architecture, build/run, debug tooling, and current bug
state: see @AGENTS.md — read it first.

## Claude Code specifics

- **Never `pkill -f taiko_boot`** (or any pattern containing `taiko_boot` /
  `wine.*EBOOT`) in a Bash call whose command line also spells the pattern —
  `pkill -f` matches your own shell's cmdline and self-kills (exit 144). Use a
  bracket pattern in a command that doesn't otherwise contain the literal:
  `pkill -f 'taiko_bo[o]t.exe'`, and don't mention the exe path elsewhere in
  the same command.
- Launch the game with Bash `run_in_background: true` running `./run-taiko.sh`;
  it blocks until the game exits. The user watches the window live — they are
  the only one who can confirm what actually appeared on screen, so ask rather
  than inferring it from the log. For a repeatable capture use
  `RSX_BATCH_CAPTURE=<file> RSX_BATCH_CAPTURE_FRAMES=N` and replay the `.rsxb`
  offline with `build-linux/rsx_replay`.
- `grep` on `build/taiko.log` needs `-a` (NUL bytes make grep treat it as
  binary and print nothing).
- Rebuild is `cmake --build build` — only touched runtime/backend files
  recompile; linking taiko_boot.exe takes ~30 s.
- **Configure with `-DTAIKO_COMPILE_JOBS=8`** on this machine. The default is a
  conservative 4 because each lifted chunk needs ~3 GB at `-O2`; with 46 GB of
  RAM, 8 jobs (~24 GB) is comfortable and roughly halves a full rebuild. Do not
  go to `nproc` (16 here) — that is the setting that used to wedge the machine.
  It is a cache variable, so set it at configure time:
  `cmake -S . -B build -DTAIKO_COMPILE_JOBS=8 ...`
- Guest code lives in `src/recomp/ppu_recomp_*.cpp` as `func_00XXXXXX`;
  resolve addresses with `ghidra_out/symbols.json` (OPD entries map
  `PTR_.opd.FUN_xxx` → function). `cia` values in `[WAIT]` logs are OPD
  addresses, not code addresses.
