# Third-party notices — draft inventory

The extension's MIT license covers only its original contributions. Do not treat this inventory
as a declaration that the combined binary has cleared redistribution review.
Exact dependency commits are in DEPENDENCIES.json / release/dependencies.json.

| Component | Use | Current status |
| --- | --- | --- |
| ScmDraft 2 | User-supplied host editor | Separately owned closed-source software; not included |
| OpenBW | Embedded mining engine | Main-code license/redistribution terms unresolved at the pinned revision |
| Asio, within OpenBW | Upstream dependency | Boost Software License 1.0; upstream notice copied into binary draft |
| CascLib | Statically linked CASC reader | MIT; upstream MIT text copied into binary draft |
| MinHook / HDE | Statically linked hooks | Upstream BSD-style license with HDE notices copied into binary draft |
| zlib, within CascLib | Statically linked decompression | zlib license in zlib.h, copied into binary draft |
| SHA-1, within CascLib | Hash implementation | Header offers GPLv2 or BSD; obtain and verify the applicable BSD text |
| AES, within CascLib | Decryption support | Headers reference OpenSSL license plus public-domain Rijndael; resolve full applicable notices |
| MD5 / lookup3, within CascLib | Hash implementations | Public-domain/permissive declarations; preserve upstream headers |
| MinGW-w64 / GCC / winpthreads | Statically linked compiler runtime pieces | Inventory exact linked components and required runtime/exception notices before publication |
| scbwfactory UI stubs | OpenBW headless adapter | Confirm maintainer ownership/provenance |
| Blizzard and Carbot assets | Loaded locally by users | Not included; not licensed by this project |

Source exports contain dependency URLs and pinned revisions, rather than vendored upstream code.
Binary drafts include available upstream license texts and component header notices in `licenses/`.
These headers do not replace the missing full terms identified above. Resolve those items before
publishing binaries, and update this document and the release checklist with the evidence.

Wine/macOS wrapper components are not included in this Windows extension package. If a macOS
wrapper is later bundled, inventory its complete runtime and distribution obligations separately.
