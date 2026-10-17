# RQL C++ runtime (vendored)

Unmodified copy of the C++ runtime of RQL (Reflection Query Language): `cpp/include/rql/` → `include/rql/`,
`cpp/src/` → `src/`, plus the repository `LICENSE.md`.

- Source: https://github.com/IvanMurzak/RQL, commit `ab2e59dc71400080d0c74c5ee11ad7db6ecae287` (`com.IvanMurzak.Rql` 0.1.1).
- License: **PolyForm Shield 1.0.0** (`LICENSE.md`), not the Apache-2.0 of the rest of this plugin. Same owner;
  see `../../../ThirdPartyNotices.txt`.
- Compiled by the `UnrealMcpRql` module: `Source/UnrealMcpRql/Private/Vendor/UnrealMcpRqlVendor_*.cpp` include each
  `src/*.cpp` once (exceptions on, RTTI off, no unity build). This folder is not a module and has no Build.cs.
- Update: copy the two folders and `LICENSE.md` from a new RQL commit, change the commit above, keep the files
  unmodified. The C++ runtime has no text parser (RQL ADR-0003): the sidecar parses queries into QueryPlans.
