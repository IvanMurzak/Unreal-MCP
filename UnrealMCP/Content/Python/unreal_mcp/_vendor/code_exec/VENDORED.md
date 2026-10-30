# Vendored: code_exec

`__init__.py` and `runner.py` are copied verbatim from [IvanMurzak/Code-Exec](https://github.com/IvanMurzak/Code-Exec)
`python/code_exec/` at main commit **`affe6ef`** (`affe6ef280813a5e14b1aa239c6ae21af129e978`, unreleased, after 0.3.1: the
`timeoutMs` time budget with its watchdog, the `sys.setrecursionlimit` cap for Python < 3.12, the host cap
`set_max_timeout_ms`, the stop of a snippet's threads at the deadline, sliced `Thread.join` / `Condition.wait` /
`time.sleep` while a snippet runs, and an overrun past the budget reported as `Timeout`). Do not edit them here: change Code-Exec, then copy the
files again and update this note. The contract they implement is Code-Exec's `spec/contract.md` (sections 4 and 6: the runtime also clips the value, the logs and the error).

License: PolyForm Shield 1.0.0 — https://polyformproject.org/licenses/shield/1.0.0
Required Notice: Copyright (c) 2026 Ivan Murzak (https://github.com/IvanMurzak)
