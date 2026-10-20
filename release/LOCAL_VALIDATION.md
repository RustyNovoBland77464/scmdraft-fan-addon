# Local release preparation checks — 2026-10-19

Passed on the development Mac:

- Full standalone 32-bit MinGW build of scmhd.sdp, minesim.exe and fan-addon-setup.exe.
- Clean allowlisted source export, fresh network fetch of all three pinned upstream dependencies,
  and a full build from that exported tree without the development wrapper, local static libraries
  or game files as build inputs.
- Existing asset-free native ANIM/texture/alpha, map-edge sampling, mining playback and overlay-layout regressions.
- Source/package checks reject game-file extensions, decompiler directories, private home paths
  and credential-like strings; exported checksums match; repeated output paths preserve prior archives.
- PE import/export inspection: plugin API exports present; Windows system/UCRT DLL imports only,
  with C++/GCC/thread runtimes statically linked. No developer home path found in plugin strings.

Not yet established:

- Native Windows setup/update/rollback/removal and folder-chooser acceptance testing.
- Full GUI acceptance testing of the new Setup executable under Wine.
- Authenticode signing, antivirus reputation, public CI execution or a redistributable macOS wrapper.
- Complete dependency redistribution clearance; see docs/RELEASE_READINESS.md.

The working editor installation was not updated or terminated by the release build/check scripts.
The packaged original-editor SHA-256 was obtained from the existing local reference executable.

## 2026-10-20 — New menu and Fan Add-on 0.1.1

- A hidden Win32 host under Wine loaded the actual built plugin and checked delayed command registration,
  moving all eight commands, preserving the native Triggers command, and creating exactly one New menu.
- Both hitbox checkmarks started unchecked and changed independently through real WM_COMMAND/menu-update messages.
- Portable shortcut tests cover exact modifiers, all five chords and preservation of existing native Ctrl+Shift chords.
- The full cross-build, existing portable regression checks and embedded installer payload verification passed.
- The running editor was not stopped or updated. The menu harness uses its own hidden window and private plugin copy.

The hidden-host test source and build/run command are in `release/tests/menu_check.cpp`.

## 2026-10-20 — Sustained-speed boost acceptance, 0.1.2

- Production cargo searches on Attitude and Octagon, both left spawns, accepted the end-patch boost. Each held 150 frames over an independent 32,768-frame replay (8,192-frame warmup), with Return Cargo delivered on all 163 measured trips.
- Position → cargo → cycle combinations passed the same holdout on all four spawns after 2,400 settling frames, with nine mineral workers and three gas workers.
- Attitude Zerg, both right spawns, retained the earlier top-patch regression: 150-frame validated and holdout cycles, with all 108 commands delivered.
- Portable tests exercise the actual acceptance function: faster equal/longer routes pass; shorter routes without a speed gain, unstable routes, missing commands and a non-improving late block fail.
- Full 32-bit cross-build, portable/package checks and embedded installer byte verification passed.

Reproduction settings and fixture identifiers are in `release/BOOST_REGRESSIONS.md`.
