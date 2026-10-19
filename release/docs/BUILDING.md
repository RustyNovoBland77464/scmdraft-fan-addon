# Build from source

The release build writes only to its build directory. It never installs into an editor or restarts one.
Use Python 3.9+, Git, CMake 3.16+, Make, and a **32-bit MinGW-w64 compiler with POSIX thread support**.
The C++ sources require C++17. Native MSVC and 64-bit plugin builds are not supported.

On macOS the development toolchain is Homebrew `mingw-w64` and `cmake`.
On Ubuntu the CI uses `cmake g++-mingw-w64-i686-posix`.

From the source root:

```sh
python3 release/bootstrap.py
cmake -S release -B build/release \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/third_party/mingw32.cmake" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --parallel 2
```

Dependencies are pinned in `release/dependencies.json`. Bootstrap checks out the exact revisions;
it refuses to replace an existing dependency at another revision. Upstream code is fetched from
its owners, not bundled into this source ZIP. No game files are required to compile.

Outputs: `scmhd.sdp`, `minesim.exe`, `fan-edit-setup.exe`, and the developer-only `installer-check.exe`.
The plugin embeds the live simulator. The separate minesim executable supports offline CLI analysis.
Its CLI help is printed by running it without arguments; game-data/map inputs are supplied locally.

Run the asset-free native regression checks with a host C++ compiler:

```sh
python3 release/check.py
```

`minesim/boost_test.cpp` contains integration tests requiring your local game installation and maps;
those assets are deliberately absent. Do not add private maps or extracted graphics to fix CI inputs.

Create local draft packages in a new output folder:

```sh
python3 release/prepare.py --out dist/local-draft --build build/release
```

This exports an allowlisted source tree with no private Git history, upstream decompilations or game assets,
and a Windows ZIP containing Setup plus the two runtime files. SHA-256 manifests accompany the files
and ZIPs. Existing packages are never overwritten. This command does not publish, sign or upload anything.
Review `RELEASE_READINESS.md` before distributing the output. The source audit is a useful guard,
not a substitute for reviewing code provenance, secrets and release contents.

GitHub's included workflow compiles and checks source on pushes and pull requests. Artifact uploading
and release publication are intentionally absent until redistribution questions are settled.
