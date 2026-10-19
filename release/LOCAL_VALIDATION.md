# Local release preparation checks — 2026-10-19

Passed on the development Mac:

- Full standalone 32-bit MinGW build of scmhd.sdp, minesim.exe and fan-edit-setup.exe.
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
