# RQL C++ runtime (vendored)

Unmodified copy of the C++ runtime of RQL (Reflection Query Language): `cpp/include/rql/` → `include/rql/`,
`cpp/src/` → `src/`, plus the repository `LICENSE.md`.

- Source: https://github.com/IvanMurzak/RQL, commit `2f4e2639bd0a30c49555a686f2a6fd3bfc829aa5` (main, unreleased; the
  sidecar pins the matching pre-release package `com.IvanMurzak.Rql` 0.3.0-dev.2f4e263 until the cascade release).
- License: **PolyForm Shield 1.0.0** (`LICENSE.md`), not the Apache-2.0 of the rest of this plugin. Same owner;
  see `../../../ThirdPartyNotices.txt`.
- Compiled by the `UnrealMcpRql` module: `Source/UnrealMcpRql/Private/Vendor/UnrealMcpRqlVendor_*.cpp` include each
  `src/*.cpp` once (exceptions on, RTTI off, no unity build). This folder is not a module and has no Build.cs.
- Update: copy the two folders and `LICENSE.md` from a new RQL commit, change the commit above, keep the files
  unmodified. The C++ runtime has no text parser (RQL ADR-0003): the sidecar parses queries into QueryPlans.
