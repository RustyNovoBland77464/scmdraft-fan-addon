# Release readiness — private preview 0.1.1

A private preview installer is available to repository collaborators. Public distribution is still pending
the checks below. The maintainer is contacting the original creator separately.

## Before publishing source

- [ ] Record the original creator's response concerning the name and extension approach.
- [ ] Establish OpenBW main-code licensing and confirm how our integration may be distributed.
- [ ] Confirm the scbwfactory stubs' ownership and licensing provenance.
- [ ] Complete review of original contributions for copied proprietary code or other imported material.
- [ ] Review the clean source export, chosen MIT scope, credits, and automated private-path/secret checks.
- [ ] Review applicable Blizzard terms for local game-data use; do not represent asset exclusion as blanket permission.
- [ ] Choose the public repository location, maintainer contact, issue policy and final project branding.

The source export deliberately omits the original executable, decompiler outputs, private Git history,
game data, private maps, screenshots, research logs and local automation tools.

## Before publishing binaries

- [ ] Resolve all source-stage items above.
- [ ] Complete CascLib bundled-component notices (especially SHA-1 BSD and legacy OpenSSL AES terms).
- [ ] Inventory MinGW/GCC/winpthreads runtime licensing and required notices/source provision.
- [ ] Test native Windows installation, update, failure recovery and removal on a clean machine.
- [x] Add a user-facing local game-folder picker shared by graphics and simulation, with automatic-detection fallback.
- [ ] Verify access to the exact supported original editor download; document required Microsoft runtimes.
- [ ] Smoke-test real maps: load, edit, undo, save/reopen, SD/HD/Carbot, hitboxes, mining and each boost technique.
- [ ] Review final ZIP contents, manifests, documentation and antivirus results; decide whether to code-sign.
- [ ] Set final version and release notes, rebuild, rerun checks and regenerate checksums.

Original ScmDraft binaries remain excluded unless their owner explicitly permits redistribution.
A macOS app wrapper is a separate packaging task with its own runtime audit and clean-machine tests.

## Prepared implementation

- [x] Standalone build independent of the development Wine wrapper.
- [x] Source export from explicit paths, with private history/decompiler/game-data exclusions.
- [x] Pinned dependency bootstrap and CI build definition without automatic uploads.
- [x] Windows setup executable with exact host hash verification, staging, backups and copy-failure rollback.
- [x] Portable runtime layout with dependency notices; no original editor or game assets bundled.
- [x] Installation/removal, build, credits, license scope and contributor documentation.
- [x] Draft package and ZIP SHA-256 manifests.

See LOCAL_VALIDATION.md in the development release directory for actual checks performed;
items marked prepared describe implementation, not complete native-Windows acceptance testing.
