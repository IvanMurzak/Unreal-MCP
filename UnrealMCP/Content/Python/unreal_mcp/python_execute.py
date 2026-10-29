# Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
# See the LICENSE file in the repository root for more information.
"""Glue between the `python-execute` tool (C++, UnrealMcpPythonTools.cpp) and the vendored code_exec runtime.

The tool evaluates ``run(<base64 JSON request>)`` (EvaluateStatement) and reads the returned base64 JSON response from
the command result: base64 has nothing to escape, so its Python repr is just the string in quotes.
"""

import base64
import json

import unreal

from unreal_mcp._vendor import code_exec


def _plain(value):
    """Unreal values -> JSON-friendly ones: objects by path, structs by UE text, enums by str(), containers
    recursively (str() of a struct or an object is its repr, which carries a memory address)."""
    if isinstance(value, unreal.Object):
        return value.get_path_name()
    if isinstance(value, unreal.StructBase):
        return value.export_text()
    if isinstance(value, unreal.EnumBase):
        return str(value)
    if isinstance(value, (list, tuple, unreal.Array, unreal.FixedArray, unreal.Set)):
        return [_plain(item) for item in value]
    if isinstance(value, (dict, unreal.Map)):
        return {str(_plain(key)): _plain(item) for key, item in value.items()}
    return value


def _convert(value):
    plain = _plain(value)
    if plain is value:
        return NotImplemented  # nothing Unreal-specific: code_exec's default conversion applies
    if isinstance(plain, (list, dict)):
        try:
            return json.dumps(plain)
        except (TypeError, ValueError):
            return str(value)
    return plain


code_exec.set_value_converter(_convert)
# python-execute's default 'timeout' (30 s; the tool always sends its budget, this covers a request without one).
code_exec.set_default_timeout_ms(30000)


def run(payload_b64, max_timeout_ms=0):
    """Runs one request. ``max_timeout_ms``: the highest budget this call may take (what is left of the tool timeout less
    a margin; code_exec turns a longer ``timeoutMs`` into it), 0 = no cap."""
    code_exec.set_max_timeout_ms(max_timeout_ms)
    return base64.b64encode(code_exec.run_b64(payload_b64).encode("utf-8")).decode("ascii")
