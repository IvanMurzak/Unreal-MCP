# Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
# See the LICENSE file in the repository root for more information.
"""Glue between the `python-execute` tool (C++, UnrealMcpPythonTools.cpp) and the vendored code_exec runtime.

The tool evaluates ``run(<base64 JSON request>)`` (EvaluateStatement) and reads the returned base64 JSON response from
the command result: base64 has nothing to escape, so its Python repr is just the string in quotes.
"""

import base64
import json

import unreal

import code_exec

try:
    import ctypes
except ImportError:  # no ctypes (embedded builds): python-execute still works, _repair_bound_method_type says what it cannot do
    ctypes = None

# Where CPython keeps tp_basicsize and tp_weaklistoffset in a PyTypeObject (64-bit, stable since Python 3.8).
_TP_BASICSIZE_OFFSET = 32
_TP_WEAKLISTOFFSET_OFFSET = 208

_BOUND_METHOD_WARNING = (
    "Engine bug not repaired in this editor: a reference kept to an `unreal` method (c = "
    "unreal.MaterialEditingLibrary.connect_material_expressions) in a snippet that defines a function or holds a reference "
    "cycle can crash the editor at a later garbage collection (level saves run them). Call such methods directly.")


def _repair_bound_method_type():
    """Works around the PythonScriptPlugin bug behind the level-save crash (docs/ARCHITECTURE.md section 11): the type of a
    bound `unreal` method inherits a ``tp_weaklistoffset`` past the end of its objects. These objects cannot hold weak
    references, so 0 ("not weak-referenceable") is what the type should have had.

    Writes only when both slots hold what Python reports for the type and the offset is past the object. Returns whether
    the type is safe."""
    closure_type = type(unreal.SystemLibrary.get_engine_version)
    if closure_type.__weakrefoffset__ < closure_type.__basicsize__:
        return True  # an engine that fixed the type
    if ctypes is None:
        return False
    address = id(closure_type)
    weaklist_offset = ctypes.c_ssize_t.from_address(address + _TP_WEAKLISTOFFSET_OFFSET)
    if (ctypes.c_ssize_t.from_address(address + _TP_BASICSIZE_OFFSET).value != closure_type.__basicsize__
            or weaklist_offset.value != closure_type.__weakrefoffset__):
        return False  # another type layout: leave it alone
    weaklist_offset.value = 0
    return True


_bound_method_type_safe = _repair_bound_method_type()


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
    if not _bound_method_type_safe:
        unreal.log_warning(_BOUND_METHOD_WARNING)
    code_exec.set_max_timeout_ms(max_timeout_ms)
    return base64.b64encode(code_exec.run_b64(payload_b64).encode("utf-8")).decode("ascii")
