# Contributing

See BUILDING.md. Use C++17 and the supported 32-bit toolchain. Keep changes focused and describe
the user-visible result and how it was checked. Original contributions use the project's MIT license;
identify any imported code and preserve its actual license and attribution.

Do not submit the original editor, decompiler dumps, Blizzard/Carbot assets, private maps or credentials.
Use synthetic fixtures for portable tests. Review logs for personal paths before sharing them.

The editor renders on a worker thread. Shared plugin state uses the drawing lock; never hold that
lock while calling back into ScmDraft. Editor calls belong on the UI thread. Hook addresses are
specific to the supported executable; do not bypass compatibility checks for an unverified build.

For bug reports include the extension version, host executable hash, OS/Wine version, graphics mode,
steps to reproduce and expected behavior. Attach maps only when you have permission to share them.
For mining results include race, base, worker counts, enabled boosts and search budgets.
