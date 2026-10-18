# Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
# See the LICENSE file in the repository root for more information.
"""Glue between the `python-execute` tool (C++, UnrealMcpPythonTools.cpp) and the vendored code_exec runtime.

The tool calls ``run(<base64 JSON request>)``; the response is printed as ONE line behind MARKER, which the C++ side
picks out of the captured Python log output. Unreal objects in results become their object paths.
"""

import json

import code_exec
import unreal

MARKER = "__UNREAL_MCP_PYTHON__"


def _plain(value):
    """Unreal values -> JSON-friendly ones (objects by path, structs / enums by their text)."""
    if isinstance(value, unreal.Object):
        return value.get_path_name()
    if isinstance(value, (unreal.StructBase, unreal.EnumBase)):
        return str(value)
    if isinstance(value, (list, tuple, unreal.Array)):
        return [_plain(item) for item in value]
    if isinstance(value, (dict, unreal.Map)):
        return {str(key): _plain(item) for key, item in value.items()}
    return value


def _convert(value):
    if isinstance(value, unreal.Object):
        return value.get_path_name()
    if isinstance(value, (unreal.StructBase, unreal.EnumBase)):
        return str(value)
    if isinstance(value, (list, tuple, dict, unreal.Array, unreal.Map)):
        try:
            return json.dumps(_plain(value))
        except (TypeError, ValueError):
            return str(value)
    return NotImplemented


code_exec.set_value_converter(_convert)


def run(payload_b64):
    print(MARKER + code_exec.run_b64(payload_b64))
