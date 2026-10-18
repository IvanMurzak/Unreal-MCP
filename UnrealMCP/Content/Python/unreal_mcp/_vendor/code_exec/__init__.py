"""Code-Exec for Python hosts (Unreal Editor and any embedded interpreter).

Pure standard library, Python 3.10+. The contract is spec/contract.md (section 4); the shared golden cases are
fixtures/golden/python.json.

    import code_exec
    result = code_exec.run("x = 21\\nx * 2")          # {"success": True, "value": "42", ...}
    text = code_exec.run_b64(payload_base64_json)     # JSON in, JSON out: the entry point for C++ hosts
"""

from .runner import run, run_json, run_b64, set_value_converter, SNIPPET_FILE

__all__ = ["run", "run_json", "run_b64", "set_value_converter", "SNIPPET_FILE"]
__version__ = "0.1.0"
