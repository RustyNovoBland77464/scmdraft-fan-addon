# Faster cycles on unchanged routes

Return Cargo can reduce a complete mining cycle without shortening the worker's route.
The old search required both improvements. It rejected stable 150-frame cycles on the
Protoss end patch shown as 149/167 in the reference image.

Acceptance now uses sustained cycle speed. Route stability, command delivery on every
measured trip, validation in separate time blocks and whole-base throughput checks remain.
Walking distance is still recorded for diagnostics. Cargo and cycle searches share this rule.

## Recorded fixtures

Game data and maps are supplied locally and are not included in the repository.
Map SHA-256:

- `(4)Attitude 1.0.scx`: `b4d0fd7214468a803cde8f532943ab320a06ea6a119dd745fb4bc42ef721a75f`
- `(4)Octagon1.0.scx`: `6aa644933c7e76bdbe305517cf7fd67bc641ee64d903a43ee8ef9c4c6867fe05`

The target is field index 0, offset (-96, +160) from its Nexus.

| Map | Spawn | Base index | Patch pixels | Cargo-only holdout |
| --- | --- | --- | --- | --- |
| Attitude | top left | 3 | 192,432 | 150 frames |
| Attitude | bottom left | 2 | 192,3984 | 150 frames |
| Octagon | top left | 3 | 192,432 | 150 frames |
| Octagon | bottom left | 1 | 192,3984 | 150 frames |

Each case used nine mineral probes and three gas probes. The independent holdout ran
32,768 frames with 8,192 warmup frames; every one of 163 measured trips received the
command. Cargo-only controls averaged 165.123288 frames. On Attitude's top-left spawn,
both control and boosted walking distance were 83.261297 pixels per measured return leg.
Direct pickup-delay trials at 19 and 20 frames also produced stable 150-frame cycles on
both Attitude left spawns despite the unchanged route distance.

All four cases also passed position → cargo → cycle with 2,400 settling frames. A boost
already supplied by an earlier technique does not need another command in later stages.
The earlier Zerg top-patch regression on Attitude's two right spawns remained at 150 frames.

## Reproduce

Build `minesim/boost_test.cpp` with the same OpenBW/CascLib includes and libraries as
`minesim/minesim.cpp`; see `release/CMakeLists.txt`. With `CASC`, `MAP` and `BASE`
set to local fixture paths and one of the indices above:

```sh
BOOST_RETURN_CARGO=1 BOOST_GAS=1 BOOST_REQUIRE_FIELD_FRAMES=151 \
  ./boost-test "$CASC" "$MAP" p "$BASE"

BOOST_RETURN_CARGO=1 BOOST_CYCLE=1 BOOST_WITH_POSITION=1 BOOST_GAS=1 \
  BOOST_SETTLE_FRAMES=2400 BOOST_REQUIRE_FIELD_FRAMES=151 \
  ./boost-test "$CASC" "$MAP" p "$BASE"
```

`BOOST_REGRESSION_FIELD` selects a different field (default 0). These integration runs
finish the implemented search frontier rather than imposing a wall-clock budget.
`python3 release/check.py` runs the asset-free acceptance regression on every build.
