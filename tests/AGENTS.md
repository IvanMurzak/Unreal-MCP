# tests/

- `host-project/` — minimal C++ host copied into generated engine/profile-specific directories.
- `automation-inventory.json` — required Automation prefixes/minimum counts; guards against unloaded/missing suites.
- `chain-fixtures/` — existing byte-compared protocol fixtures; retain their contract.
- Extension specs stay beside their source; Python suite/release tests live in `scripts/test_extension_monorepo.py`.
- Never check in generated host Binaries/Intermediate, reports or linked plugin directories.
