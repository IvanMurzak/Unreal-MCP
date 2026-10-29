# tests/

- `host-project/` — minimal C++ host copied into generated engine/profile-specific directories.
- `smoke-host/` — `UnrealTestProject` host for the CI `connection-smoke` leg (synced into a persistent per-runner folder).
- `automation-inventory.json` — required Automation prefixes/minimum counts; guards against unloaded/missing suites.
- `chain-fixtures/` — existing byte-compared protocol fixtures; retain their contract.
- `apple-sdk/` — excerpts of each UE version's real `Engine/Config/Apple/Apple_SDK.json` (5.5 is JSONC) for `scripts/test_ci_steps.py`.
- Extension specs stay beside their source; Python suite/release tests live in `scripts/test_extension_monorepo.py`.
- Never check in generated host Binaries/Intermediate, reports or linked plugin directories.
