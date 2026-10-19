# ScmDraft Fan Edit

## Installation

1. Get the **Windows package ZIP** from the maintainer and extract the entire ZIP into a temporary folder. Prebuilt downloads are not yet attached to this repository.
2. Install **ScmDraft 2 0.9.10** and confirm it launches. Fan Edit requires a specific build; Setup checks compatibility.
3. Install **StarCraft: Remastered** locally, including its game data.
4. Close ScmDraft, run **`fan-edit-setup.exe`**, and select your existing ScmDraft executable.
5. Confirm installation and choose the StarCraft game folder containing **`Data`** when prompted.
6. Launch ScmDraft normally. In its own profile settings, also select the StarCraft game-data folder.
7. Select **Triggers → HD Graphics → Remastered**. To try mining, select **Triggers → Test Mining**, start a base, enable a boost technique, and press **Search**.

See [INSTALL.md](INSTALL.md) for the supported editor hash, manual installation, macOS/Wine setup, troubleshooting and removal.
Building from source instead? Follow [BUILDING.md](BUILDING.md).

An unofficial community extension for ScmDraft 2: HD graphics, additional terrain symmetry tools,
and animated mining analysis with searches for position, Return Cargo and cycle boosts.

**Local draft, version 0.1.0.** Publication and redistribution items are tracked in
[RELEASE_READINESS.md](RELEASE_READINESS.md). This draft is not an announcement of author endorsement.

ScmDraft 2 was created by Suicidal Insanity / Stormcoast Fortress. Fan Edit builds on that editor
through a separate plugin. The original editor remains separately owned and licensed; this source
release contains our extension, not the original ScmDraft source code.

## Features

- Remastered and Carbot terrain, units and sprites read from your own game installation.
- Additional terrain symmetry controls, mirrored terrain copying and alternate terrain selection.
- Building and unit collision-box overlays.
- Animated mining tests for Terran, Protoss and Zerg, with per-patch cycle times and income statistics.
- Timed, resumable searches for position, Return Cargo and cycle boosts.
- Command guides and translucent worker markers, with separate visibility controls.
- 1x, 2x and 4x mining playback, plus independent testing at multiple bases.

## Requirements and downloads

You supply ScmDraft 2 and a local StarCraft installation with the required game data.
HD/Carbot modes require the corresponding available assets and rights to use them.
No Blizzard graphics or original ScmDraft executable are included.

The Windows build targets Windows 10/11 (32-bit plugin, also usable on 64-bit Windows).
Only one exact **32-bit ScmDraft 0.9.10** executable is supported; see [INSTALL.md](INSTALL.md).
The Windows plugin is currently exercised under Wine on macOS. Native Windows compatibility
still needs a clean-machine check. A redistributable macOS application wrapper is not included yet.

- Users: extract the entire Windows package and follow [INSTALL.md](INSTALL.md).
- Developers: see [BUILDING.md](BUILDING.md) and [CONTRIBUTING.md](CONTRIBUTING.md).
- Credits and licensing: [CREDITS.md](CREDITS.md), [LICENSE](LICENSE), [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

StarCraft and its artwork belong to Blizzard Entertainment. This is an independent fan project,
not an official Blizzard product. Preserve the original authors' notices and licenses.
