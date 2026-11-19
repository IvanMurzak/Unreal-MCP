# RQL C++ runtime (vendored)

Unmodified copy of the C++ runtime of RQL (Reflection Query Language): `cpp/include/rql/` → `include/rql/`,
`cpp/src/` → `src/`, plus the repository `LICENSE.md`.

- Source: https://github.com/IvanMurzak/RQL, commit `217e2a3659284b4e6610b27c07d26368a1238564` (`com.IvanMurzak.Rql` 0.5.0).
  RQL 0.5.0 (read stages after a read-only `call`, RQL PR #36); the release train rewrites this record to the
  published RQL commit and version.
- License: **PolyForm Shield 1.0.0** (`LICENSE.md`), not the Apache-2.0 of the rest of this plugin. Same owner;
  see `../../../ThirdPartyNotices.txt`.
- Compiled by the `UnrealMcpRql` module: `Source/UnrealMcpRql/Private/Vendor/UnrealMcpRqlVendor_*.cpp` include each
  `src/*.cpp` once (exceptions on, RTTI off, no unity build). This folder is not a module and has no Build.cs.
- Update: copy the two folders and `LICENSE.md` from a new RQL commit, change the commit above, keep the files
  unmodified. The C++ runtime has no text parser (RQL ADR-0003): the sidecar parses queries into QueryPlans.
