"""Snippet runner: spec/contract.md section 4. Standard library only, Python 3.10+."""

import ast
import base64
import builtins
import io
import json
import os
import sys
import time
import traceback
from contextlib import redirect_stderr, redirect_stdout

SNIPPET_FILE = "snippet.py"

_value_converter = None


def set_value_converter(converter):
    """Host hook: ``converter(value) -> value | NotImplemented``. NotImplemented falls back to the default."""
    global _value_converter
    _value_converter = converter


def run(code, include=(), include_root=None, imports=()):
    """Execute ``code`` and return the contract's response as a dict (never raises for snippet errors)."""
    started = time.perf_counter()
    result = _empty()

    if not isinstance(code, str) or not code.strip():
        return _fail(result, "InvalidRequest", "code is empty")
    if isinstance(imports, str):
        return _fail(result, "InvalidRequest", "imports must be a list of module names")

    root = os.path.abspath(include_root or os.getcwd())
    try:
        folders, files = _resolve_include(include, root)
        sources = [(path, _read(full)) for path, full in files]
    except (OSError, ValueError) as error:  # UnicodeDecodeError (an include that is not UTF-8) is a ValueError
        return _fail(result, "InvalidRequest", str(error))

    try:
        file_code = [compile(source, path, "exec") for path, source in sources]
        tree = ast.parse(code, SNIPPET_FILE, "exec")
        last = None
        if tree.body and isinstance(tree.body[-1], ast.Expr):
            last = ast.Expression(tree.body.pop().value)
        body = compile(tree, SNIPPET_FILE, "exec")
        tail = compile(last, SNIPPET_FILE, "eval") if last is not None else None
    except SyntaxError as error:
        result["diagnostics"].append({
            "file": error.filename or SNIPPET_FILE,
            "line": error.lineno or 0,
            "column": error.offset or 0,
            "id": type(error).__name__,
            "severity": "error",
            "message": error.msg,
        })
        result["compileMs"] = _ms(started)
        return _fail(result, "Compile", "Compilation failed")
    except ValueError as error:  # null bytes in the source raise ValueError, not SyntaxError, on Python 3.10
        result["compileMs"] = _ms(started)
        return _fail(result, "Compile", str(error))
    result["compileMs"] = _ms(started)

    executing = time.perf_counter()
    scope = {"__name__": "__code_exec__", "__builtins__": builtins}
    out, err = _LogStream(result["logs"], "info"), _LogStream(result["logs"], "error")
    prefixes = [_prefix(folder) for folder in folders]
    saved_path = list(sys.path)
    saved_bytecode = sys.dont_write_bytecode
    try:
        sys.path[:0] = folders
        if folders:
            # Helpers are edited between runs, often within the same second and at the same size, which a cached
            # .pyc cannot tell apart: never write bytecode for them, and forget the folders' directory listings.
            sys.dont_write_bytecode = True
            for folder in folders:
                sys.path_importer_cache.pop(folder, None)
        for name in imports or ():
            scope.setdefault(name.split(".")[0], __import__(name))  # like `import a.b`: binds the top package
        with redirect_stdout(out), redirect_stderr(err):
            for compiled in file_code:
                exec(compiled, scope)
            exec(body, scope)
            value = eval(tail, scope) if tail is not None else None
        result["value"] = _convert(value)
        result["success"] = True
    except (Exception, SystemExit) as error:  # a snippet must never take the host down
        result["errorKind"] = "Runtime"
        result["error"] = _format_error(error, root, prefixes, {path for path, _ in files})
    finally:
        out.flush()
        err.flush()
        sys.path[:] = saved_path
        sys.dont_write_bytecode = saved_bytecode
        _forget_modules(prefixes)
    result["executeMs"] = _ms(executing)
    return result


def run_json(payload):
    """JSON request (spec section 1 plus Python-only ``imports`` / ``includeRoot``) -> JSON response."""
    try:
        request = json.loads(payload)
    except ValueError as error:
        return _invalid("request is not JSON: %s" % error)
    if not isinstance(request, dict):
        return _invalid("request must be a JSON object")
    return json.dumps(run(
        request.get("code", ""),
        include=request.get("include") or (),
        include_root=request.get("includeRoot"),
        imports=request.get("imports") or (),
    ))


def run_b64(payload_b64):
    """Same as :func:`run_json`, with the request base64-encoded: hosts pass it through one string literal."""
    try:
        payload = base64.b64decode(payload_b64).decode("utf-8")
    except (ValueError, UnicodeDecodeError) as error:
        return _invalid("request is not base64 UTF-8: %s" % error)
    return run_json(payload)


def _empty():
    return {
        "success": False, "value": None, "logs": [], "errorKind": "None", "error": None, "diagnostics": [],
        "autoImported": [], "compileMs": 0, "executeMs": 0, "cacheHit": False,
    }


def _fail(result, kind, message):
    result["success"] = False
    result["errorKind"] = kind
    result["error"] = message
    return result


def _invalid(message):
    return json.dumps(_fail(_empty(), "InvalidRequest", message))


def _ms(since):
    return int((time.perf_counter() - since) * 1000)


def _read(path):
    try:
        with io.open(path, "r", encoding="utf-8") as handle:
            return handle.read()
    except UnicodeDecodeError as error:
        raise ValueError("include is not UTF-8: %s (%s)" % (path, error)) from None


def _resolve_include(include, root):
    """Folders go on sys.path (import by name); files run in the snippet's namespace first, in order."""
    if isinstance(include, str):
        raise ValueError("include must be a list of paths")
    folders, files, seen = [], [], set()
    for entry in include or ():
        if not isinstance(entry, str) or not entry.strip():
            raise ValueError("include entry is empty")
        full = os.path.abspath(entry if os.path.isabs(entry) else os.path.join(root, entry))
        if full in seen:
            continue
        seen.add(full)
        if os.path.isdir(full):
            folders.append(full)
        elif os.path.isfile(full):
            files.append((_relative(full, root), full))
        else:
            raise FileNotFoundError("include not found: %s" % entry)
    return folders, files


def _prefix(folder):
    return os.path.normcase(os.path.join(folder, ""))


def _under(path, prefixes):
    path = os.path.normcase(os.path.abspath(path))
    return any(path.startswith(prefix) for prefix in prefixes)


def _relative(path, root):
    relative = os.path.relpath(path, root) if _under(path, [_prefix(root)]) else path
    return relative.replace("\\", "/")


def _forget_modules(prefixes):
    """Drop modules imported from include folders, so an edited helper is re-imported on the next run."""
    if not prefixes:
        return
    for name, module in list(sys.modules.items()):
        location = getattr(module, "__file__", None)
        if location and _under(location, prefixes):
            del sys.modules[name]


def _convert(value):
    if _value_converter is not None:
        converted = _value_converter(value)
        if converted is not NotImplemented:
            return converted
    if value is None or isinstance(value, str):
        return value
    if isinstance(value, (dict, list, tuple)):
        try:
            return json.dumps(value)
        except (TypeError, ValueError):
            pass
    return str(value)


def _format_error(error, root, prefixes, include_files):
    """``Type: message`` plus only the frames the agent wrote (snippet.py, include files, include folders)."""
    frames = []
    for frame in traceback.extract_tb(error.__traceback__):
        if frame.filename == SNIPPET_FILE or frame.filename in include_files:
            frames.append(frame)
        elif _under(frame.filename, prefixes):
            frame.filename = _relative(os.path.abspath(frame.filename), root)
            frames.append(frame)
    lines = ["%s: %s" % (type(error).__name__, error)]
    lines.extend(line.rstrip("\n") for line in traceback.format_list(frames))
    return "\n".join(lines)


class _LogStream(io.TextIOBase):
    """Collects written text as one log entry per line."""

    def __init__(self, sink, level):
        super().__init__()
        self._sink = sink
        self._level = level
        self._buffer = ""

    def writable(self):
        return True

    def write(self, text):
        *lines, self._buffer = (self._buffer + text).split("\n")
        for line in lines:
            self._sink.append({"level": self._level, "message": line.rstrip("\r")})
        return len(text)

    def flush(self):
        if self._buffer:
            self._sink.append({"level": self._level, "message": self._buffer})
            self._buffer = ""
