# Install ScmDraft Fan Edit

This package adds files to an existing ScmDraft installation. It does not include the original
editor, game data, Microsoft runtimes or a Wine runtime. Close ScmDraft before installing or updating.

## Supported editor

ScmDraft 2 Beta 0.9.10, 32-bit, PE timestamp `0x5EF3C9F5`.
The executable's SHA-256 must be:

```
751060e3e09eba3ce043d54a1f39ac1143ee9d6df0f9444cf0a2caf3de28f72e
```

Other builds with the same version label may differ. Setup rejects a mismatching file.
Obtain the editor from its author: https://www.stormcoast-fortress.net/cntt/software/scmdraft/download/
That page is a reference; we have not confirmed that every linked download is the supported build.
Keep your original download and any editor/runtime installation instructions.

## Windows installer

[Download the standalone installer](https://github.com/RustyNovoBland77464/scmdraft-fan-edit/releases/download/v0.1.0-preview.1/scmdraft-fan-edit-0.1.0-setup.exe). GitHub sign-in and repository access are required.

1. Run the downloaded `.exe` and select the executable in your existing ScmDraft installation.
2. Confirm the displayed destination. Use a writable editor folder, outside Program Files if necessary.
3. Optionally choose your local StarCraft game folder when prompted.
4. Start ScmDraft normally. Fan Edit loads as `plugins/scmhd.sdp`.

The installer contains the plugin, mining tool and notices. It unpacks them to a temporary folder,
installs them, then removes the temporary copy. No companion ZIP or internet connection is needed after download.

Setup backs up existing Fan Edit files into a unique `plugins/sfe*.tmp` folder before replacement.
It does not change the original editor executable, maps or game files. Dependency/license notices
are installed as `plugins/ScmDraft-Fan-Edit-NOTICES.txt`.
Choosing a game folder saves only Fan Edit's per-user `StarcraftPath` setting in
`HKEY_CURRENT_USER\Software\ScmDraft HD`. Skipping the chooser keeps the existing setting/detection.
If a copy fails, Setup attempts to restore any files it replaced and reports the backup location.

## Portable ZIP

[Download the portable package](https://github.com/RustyNovoBland77464/scmdraft-fan-edit/releases/download/v0.1.0-preview.1/scmdraft-fan-edit-0.1.0-windows-x86.zip) and extract the whole ZIP.
Run `fan-edit-setup.exe` from it, or manually copy the three files in `plugins/` into your editor's
`plugins` folder, backing up older copies first. Keep the package's documentation and license files.

## Your StarCraft installation

In ScmDraft's profile/settings dialog, select your StarCraft installation and the appropriate
game-data version/autodetection. Choose the game installation containing its `Data` folder,
not your Documents/StarCraft maps folder.

Fan Edit uses the folder chosen in Setup for both HD graphics and mining. If no custom folder is
saved, it checks Blizzard's Windows registry entries, then `C:\Program Files (x86)\StarCraft`.
ScmDraft's own profile still needs its game-data location set separately. If the game moves,
rerun Setup to choose its new location. The chooser checks for a Data directory; opening the
actual CASC storage and available graphics happens when the plugin uses that installation.

Graphics are read locally. They are not uploaded or bundled with Fan Edit. Do not copy extracted
game graphics into the source tree or release package.

## Use

- **Triggers → HD Graphics: Off / Remastered / Carbot** selects artwork.
- **Triggers → Test Mining** enables the mining controls around bases.
- Start a base, select boost techniques, then press **Search**. Adjust the budget or **Continue search**.
- **Guides** controls which boost markers appear. **Speed** selects 1x, 2x or 4x playback.
- Building/unit hitbox options and mirrored-copy tools are also in the Triggers menu.
- Terrain symmetry controls appear in the terrain panel; **Copy terrain…** selects alternate terrain.

## macOS with an existing Wine installation

Use the Windows payload inside the same Wine prefix as your working 32-bit ScmDraft installation.
The manual copy goes into that prefix's editor `plugins` folder. StarCraft data must be reachable
through Wine's paths and registry. This package does not install, duplicate or reconfigure a wrapper.

## Troubleshooting and removal

- Missing menus: check plugin location, supported executable and the `plugins/scmhd.log` diagnostic log.
- Missing HD art: rerun Setup to choose the correct local game folder, then restart ScmDraft.
- Read errors: verify game files through the official game launcher, then restart the editor.
- Uninstall: close the editor and remove `scmhd.sdp`, `minesim.exe` and `ScmDraft-Fan-Edit-NOTICES.txt`. If you had older versions,
  restore the copies in Setup's backup folder. Keep other plugins such as TrigEdit.
  Optional cleanup: remove `StarcraftPath` from `HKEY_CURRENT_USER\Software\ScmDraft HD`.

Logs can contain local paths and map details. Review them before attaching to a public issue.
