# Local release preparation

The public-facing templates are in `docs/`. They are copied to the root of the clean source export
and Windows package. `prepare.py` uses an explicit source allowlist instead of exporting private history.
The private checkout's original README and reverse-engineering exports are not included.

```sh
cmake -S release -B build/release -DCMAKE_TOOLCHAIN_FILE="$PWD/third_party/mingw32.cmake" -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --parallel 2
python3 release/check.py
python3 release/prepare.py --out dist/local-draft --build build/release
```

No installation into the owner's Wine wrapper, editor termination, upload, public repository creation
or messaging is performed. See `docs/RELEASE_READINESS.md` for the remaining publication decisions.
Do not make the private repository public; initialize the reviewed exported tree as a new repository.
