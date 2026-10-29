"""Snippet runner: spec/contract.md sections 4 and 6. Standard library only, Python 3.10+."""

import ast
import base64
import builtins
import collections
import io
import json
import linecache
import operator
import os
import sys
import threading
import time
import tokenize
import traceback
from contextlib import redirect_stderr, redirect_stdout

try:  # bound once: a snippet that patches ctypes cannot break disarming
    import ctypes
    _SET_ASYNC_EXC = ctypes.pythonapi.PyThreadState_SetAsyncExc
    _THREAD_ID, _PY_OBJECT = ctypes.c_ulong, ctypes.py_object
except (ImportError, AttributeError):  # no ctypes / no pythonapi (embedded builds): the time budget is not enforced
    _SET_ASYNC_EXC = None

SNIPPET_FILE = "snippet.py"

# Output limits (spec section 6), in characters; 0 = no limit.
MAX_VALUE_CHARS = 20000
MAX_LOG_CHARS = 20000
MAX_LOG_LINE_CHARS = 2000
MAX_ERROR_CHARS = 20000
_LOG_ENTRY_CHARS = 12  # what one log entry costs on top of its message ("[warning] " and a line break, as hosts print it)
_FRAMES_KEPT = 10  # a traceback keeps the first and the last _FRAMES_KEPT of the agent's frames
_MAX_CAUSES = 5  # chained exceptions (__cause__ / __context__) reported after the error itself

# Time budget (spec section 1, timeoutMs) of a request that sets none, in milliseconds; 0 = no bound.
TIMEOUT_MS = 120000
_default_timeout_ms = TIMEOUT_MS
_max_timeout_ms = 0  # the highest budget a request may ask for (set_max_timeout_ms); 0 = no cap
_REFIRE_SECONDS = 0.1  # a snippet that catches the stop is stopped again this often
_POLL_SECONDS = 0.02  # how often the watchdog looks whether the snippet ended
_MARGIN_SECONDS = 0.05  # a snippet ending this long before its deadline was never stopped: no need to wait for the watchdog

# Python < 3.12 has no C stack check: a snippet that raises the recursion limit and then recurses in C (repr / json.dumps
# of a deeply nested list) overflows the C stack and ends the host. A snippet's sys.setrecursionlimit is capped there
# (spec section 4). 3000 is conservative for Unreal's game thread (12 MB stack): in UnrealEditor 5.7 (Python 3.11.8)
# every C recursion path measured survived at 4000; the costliest (a sort key that sorts again) ended it at 4500.
MAX_RECURSION_LIMIT = 3000
_CAPPED = sys.version_info < (3, 12)
_max_recursion_limit = MAX_RECURSION_LIMIT
_SET_RECURSION_LIMIT = sys.setrecursionlimit  # bound once: the runner restores limits with it, whatever a snippet replaced

# JSON request field -> run() keyword argument (integers).
_INT_FIELDS = {
    "timeoutMs": "timeout_ms",
    "maxValueChars": "max_value_chars",
    "maxLogChars": "max_log_chars",
    "maxLogLineChars": "max_log_line_chars",
    "maxErrorChars": "max_error_chars",
}

_value_converter = None


def _is_count(value):
    """An integer >= 0 (not a bool): what every limit and budget setting takes."""
    return isinstance(value, int) and not isinstance(value, bool) and value >= 0


def set_value_converter(converter):
    """Host hook: ``converter(value) -> value | NotImplemented``. NotImplemented falls back to the default."""
    global _value_converter
    _value_converter = converter


def set_default_timeout_ms(timeout_ms):
    """Host hook: the time budget of a request without ``timeout_ms`` / ``timeoutMs`` (default 120000); 0 = no bound."""
    global _default_timeout_ms
    if not _is_count(timeout_ms):
        raise ValueError("timeout_ms must be an integer >= 0 (0 = no bound)")
    _default_timeout_ms = timeout_ms


def set_max_timeout_ms(timeout_ms):
    """Host hook: the highest time budget a request may ask for (a longer ``timeout_ms`` / ``timeoutMs``, and a run that
    would have no bound, get this one); 0 = no cap (default)."""
    global _max_timeout_ms
    if not _is_count(timeout_ms):
        raise ValueError("timeout_ms must be an integer >= 0 (0 = no cap)")
    _max_timeout_ms = timeout_ms


def _budget(timeout_ms):
    """The request's budget, else the host default, at most the host's cap; 0 = no bound."""
    budget = timeout_ms or _default_timeout_ms
    if _max_timeout_ms and (not budget or budget > _max_timeout_ms):
        budget = _max_timeout_ms
    return budget


def set_max_recursion_limit(limit):
    """Host hook: the highest recursion limit a snippet may set with ``sys.setrecursionlimit`` on Python < 3.12 (default
    3000, a limit the host set itself above it stays allowed); 0 = no cap. Python 3.12+ never cap."""
    global _max_recursion_limit
    if not _is_count(limit):
        raise ValueError("limit must be an integer >= 0 (0 = no cap)")
    _max_recursion_limit = limit


def run(code, include=(), include_root=None, imports=(), max_value_chars=MAX_VALUE_CHARS, max_log_chars=MAX_LOG_CHARS,
        max_log_line_chars=MAX_LOG_LINE_CHARS, max_error_chars=MAX_ERROR_CHARS, include_outside_root=False, timeout_ms=0):
    """Execute ``code`` and return the contract's response as a dict (never raises for snippet errors).

    ``include_outside_root``: the host accepts includes outside ``include_root`` (absolute paths, ``..``).
    ``timeout_ms``: the snippet's time budget; 0 = the host default (:func:`set_default_timeout_ms`), at most the host's
    cap (:func:`set_max_timeout_ms`). A snippet still running when it is spent is stopped with a ``Timeout`` error, and
    threads it started are stopped when it ends (spec section 4).
    """
    result = _empty()
    limits = (max_value_chars, max_log_chars, max_log_line_chars, max_error_chars)
    if not all(_is_count(limit) for limit in limits):
        return _fail(result, "InvalidRequest", "output limits must be integers >= 0 (0 = no limit)")
    if not _is_count(timeout_ms):
        return _fail(result, "InvalidRequest", "timeoutMs must be an integer >= 0 (0 = the host's default)")
    logs = _Logs(max_log_chars, max_log_line_chars)
    try:
        _execute(result, logs, code, include, include_root, imports, include_outside_root, _budget(timeout_ms))
    except BaseException as error:  # the runner itself failed (a snippet wrecked the interpreter state): still answer
        result["success"], result["value"] = False, None
        _fail(result, "Runtime", "%s: %s (while running the snippet)" % (type(error).__name__, _message(error)))

    result["logs"] = logs.entries()
    value, error = result["value"], result["error"]
    if isinstance(value, str):
        result["value"] = _clip(value, max_value_chars, "return a slice or a summary")
    if isinstance(error, str):
        result["error"] = _clip(error, max_error_chars, "raise a shorter message")
    changed = (("value", result["value"] is not value), ("logs", logs.left_out or logs.clipped), ("error", result["error"] is not error))
    result["truncated"] = [field for field, truncated in changed if truncated]
    return result


def _execute(result, logs, code, include, include_root, imports, include_outside_root, timeout_ms):
    started = time.perf_counter()
    if not isinstance(code, str) or not code.strip():
        return _fail(result, "InvalidRequest", "code is empty")
    if not isinstance(imports, (list, tuple)) or not all(isinstance(name, str) and name for name in imports):
        return _fail(result, "InvalidRequest", "imports must be a list of module names")
    if include_root is not None and not isinstance(include_root, str):
        return _fail(result, "InvalidRequest", "includeRoot must be a path")

    root = os.path.abspath(include_root or os.getcwd())
    try:
        folders, files = _resolve_include(include, root, include_outside_root)
        sources = [(path, _read(full)) for path, full in files]
    except (OSError, ValueError) as error:
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
    except (ValueError, RecursionError, MemoryError) as error:  # null bytes (3.10), a too deeply nested expression
        result["compileMs"] = _ms(started)
        return _fail(result, "Compile", "%s: %s" % (type(error).__name__, error))
    result["compileMs"] = _ms(started)

    executing = time.perf_counter()
    scope = {"__name__": "__code_exec__", "__builtins__": builtins}
    out, err = _LogStream(logs, "info"), _LogStream(logs, "error")
    prefixes = [_prefix(folder) for folder in folders]
    saved = _HostState(code, sources)
    watchdog = _Watchdog(timeout_ms)
    agent_files = {SNIPPET_FILE} | {path for path, _ in files}
    threads = _SnippetThreads(lambda filename: filename in agent_files or _under(filename, prefixes))
    try:
        sys.path[:0] = folders
        if folders:
            # Helpers are edited between runs, often within the same second and at the same size, which a cached
            # .pyc cannot tell apart: never write bytecode for them, and forget the folders' directory listings.
            sys.dont_write_bytecode = True
            for folder in folders:
                sys.path_importer_cache.pop(folder, None)
        for name in imports:
            scope.setdefault(name.split(".")[0], __import__(name))  # like `import a.b`: binds the top package
        with redirect_stdout(out), redirect_stderr(err):
            saved.cap_recursion_limit(logs)
            threads.start()
            watchdog.start()
            for compiled in file_code:
                exec(compiled, scope)
            exec(body, scope)
            value = eval(tail, scope) if tail is not None else None
            _make_room(saved)
            watchdog.stop()
        result["value"] = _convert(value)
        result["success"] = True
    except BaseException as error:  # SystemExit, KeyboardInterrupt, ...: a snippet must never take the host down
        _make_room(saved)
        watchdog.stop()
        result["errorKind"] = "Timeout" if isinstance(error, SnippetTimeout) else "Runtime"
        try:
            result["error"] = _format_error(error, root, prefixes, {path for path, _ in files})
        except BaseException:  # the traceback itself cannot be read
            result["error"] = "%s: %s" % (type(error).__name__, _message(error))
    finally:
        try:
            _make_room(saved)
            watchdog.stop()
        finally:
            out.flush()
            err.flush()
            threads.stop(logs)
            saved.restore(prefixes)
    result["executeMs"] = _ms(executing)
    return result


def _make_room(saved):
    """Gives the runner back the host's recursion limit if the snippet lowered it: disarming needs a few frames."""
    try:
        _SET_RECURSION_LIMIT(max(sys.getrecursionlimit(), saved.recursion_limit))
    except BaseException:  # a snippet replaced sys.getrecursionlimit: the restore step reports nothing either
        pass


class _RecursionCap:
    """``sys.setrecursionlimit`` while a snippet runs on Python < 3.12: a limit past ``cap`` is set to ``cap``, with one
    warning per run (spec section 4). It keeps an agent from ending the host by accident; it is no sandbox."""

    def __init__(self, cap, logs):
        self._cap, self._logs, self._warned = cap, logs, False
        self.active = True  # a reference the snippet kept (`from sys import setrecursionlimit`) caps nothing after the run

    def __call__(self, limit):
        limit = operator.index(limit)  # what Python's own accepts (numpy integers too); anything else is its TypeError
        if self.active and limit > self._cap:
            if not self._warned:
                self._warned = True
                self._logs.add("warning", "sys.setrecursionlimit(%d) was capped at %d: on Python 3.11 and older a higher "
                                          "limit can overflow the C stack and end the host" % (limit, self._cap))
            limit = self._cap
        _SET_RECURSION_LIMIT(limit)  # a limit too low raises as Python's own does


class SnippetTimeout(BaseException):
    """Raised in the snippet's thread when its time budget is spent. A ``BaseException``: ``except Exception`` in the
    snippet does not swallow it. Each run raises its own subclass, which knows that run's budget."""

    budget_ms = 0

    def __str__(self):
        return "The snippet ran longer than its time budget of %d ms (timeoutMs) and was stopped." % type(self).budget_ms


class _Watchdog:
    """Stops a snippet that outruns its time budget: a timer thread raises :class:`SnippetTimeout` in the snippet's thread
    (``PyThreadState_SetAsyncExc``), again every ``_REFIRE_SECONDS`` while the snippet catches it. Python delivers it
    between bytecodes, so pure Python loops stop; a call into C (``time.sleep``, an engine call) finishes first.

    No lock is ever taken in the snippet's thread (a stop delivered right after acquiring one would leave it held):
    :meth:`stop` clears a flag, waits for the timer thread to end if it may have raised, then drops a stop raised but not
    delivered yet. None reaches the host after the run."""

    def __init__(self, timeout_ms):
        self._timeout_ms = timeout_ms
        self._thread_id = None
        self._deadline = 0.0
        self._armed = self._fired = self._exited = False

    def start(self):
        if not self._timeout_ms or _SET_ASYNC_EXC is None:
            return
        stop = type("SnippetTimeout", (SnippetTimeout,), {"budget_ms": self._timeout_ms})
        self._thread_id = threading.get_ident()
        self._deadline = time.monotonic() + self._timeout_ms / 1000.0
        self._armed = True
        _start_untracked(threading.Thread(target=self._watch, args=(stop,), name="code_exec watchdog", daemon=True))

    def _watch(self, stop):
        try:
            deadline = self._deadline
            while self._armed:
                now = time.monotonic()
                if now < deadline:
                    time.sleep(min(_POLL_SECONDS, deadline - now))
                elif self._armed:
                    self._fired = True
                    _async_exc(self._thread_id, stop)
                    deadline = now + _REFIRE_SECONDS
        finally:
            self._exited = True

    def stop(self):
        while self._thread_id is not None:
            try:
                self._armed = False
                # Before the deadline (and never fired) the timer cannot raise any more: it looks at the flag before it
                # would. Otherwise wait until it is gone, then drop what it may have raised.
                if self._fired or time.monotonic() >= self._deadline - _MARGIN_SECONDS:
                    while not self._exited:
                        time.sleep(0.001)
                    _async_exc(self._thread_id, None)
                self._thread_id = None
            except SnippetTimeout:  # delivered while disarming: disarm again
                pass


class SnippetThreadStopped(BaseException):
    """Raised in a thread the snippet started, once the snippet has ended (spec section 4): threads may not outlive it."""

    def __str__(self):
        return "The snippet has ended, so this thread it started was stopped: join threads before the snippet returns."


# Runs whose snippet is running, and the threading.Thread.start the dispatcher stands in for while there is one. A host
# that replaced threading.Thread.start meanwhile keeps the dispatcher in its chain: it is then never installed again.
_thread_runs = []
_saved_start = None
_dispatcher_kept = False


def _dispatch_start(thread, *args, **kwargs):
    """``threading.Thread.start`` while snippets run: a thread started from a snippet's thread (or one of its threads) is
    that run's."""
    for runs in list(_thread_runs):
        if threading.get_ident() in runs.owners:
            return runs.start_tracked(thread, args, kwargs)
    return _saved_start(thread, *args, **kwargs)


def _start_untracked(thread):
    """Starts one of the runner's own threads: never a snippet's."""
    (_saved_start if _dispatcher_kept or threading.Thread.start is _dispatch_start else threading.Thread.start)(thread)


class _Tracked:
    __slots__ = ("thread", "starting", "done", "cancelled", "dismissed")

    def __init__(self, thread):
        self.thread = thread
        self.starting = self.done = self.cancelled = self.dismissed = False


class _SnippetThreads:
    """The threads a snippet starts: ``threading.Thread.start`` called from the snippet's thread, or from one of those
    threads, while the snippet runs. When it ends, a ``threading.Timer`` not fired yet is cancelled and every other one
    still alive gets :class:`SnippetThreadStopped` (``PyThreadState_SetAsyncExc``) whenever the agent's own code
    (``is_agent_file``) is the frame it runs, again every ``_REFIRE_SECONDS`` while that code is on its stack. A thread with
    none of it on its stack (a pool worker waiting for work) is left alone from then on. The thread swallows the stop:
    nothing is printed."""

    def __init__(self, is_agent_file):
        self._is_agent_file = is_agent_file
        self._tracked = {}  # id(thread) -> _Tracked; only ever added to, so the stopper thread can read it any time
        self.owners = set()
        self._active = False

    def start(self):
        global _saved_start
        if _SET_ASYNC_EXC is None:
            return
        self.owners.add(threading.get_ident())
        if not _thread_runs and not _dispatcher_kept:
            _saved_start, threading.Thread.start = threading.Thread.start, _dispatch_start
        _thread_runs.append(self)
        self._active = True

    def start_tracked(self, thread, args, kwargs):
        if id(thread) in self._tracked:  # started twice: the second start raises as Python's own does
            return _saved_start(thread, *args, **kwargs)
        entry = self._tracked[id(thread)] = _Tracked(thread)
        run, owners = thread.run, self.owners

        def tracked():
            ident = threading.get_ident()
            owners.add(ident)
            try:
                try:
                    run()
                finally:
                    while True:  # a stop raised while leaving must not outlive the thread either
                        try:
                            entry.done = True
                            owners.discard(ident)
                            _async_exc(ident, None)
                            break
                        except SnippetThreadStopped:
                            pass
            except SnippetThreadStopped:
                pass

        thread.run = tracked
        entry.starting = True
        try:
            return _saved_start(thread, *args, **kwargs)
        finally:
            entry.starting = False

    def stop(self, logs):
        """Never raises: the host's state is restored after it, whatever the snippet's threads do."""
        global _dispatcher_kept
        if not self._active:
            return
        self._active = False
        try:
            _thread_runs.remove(self)
            if not _thread_runs and not _dispatcher_kept:
                if threading.Thread.start is _dispatch_start:
                    threading.Thread.start = _saved_start
                else:
                    _dispatcher_kept = True
            self.owners.discard(threading.get_ident())
            entries = list(self._tracked.values())
            for entry in entries:
                timer = entry.thread
                if isinstance(timer, threading.Timer) and not timer.finished.is_set():
                    threading.Timer.cancel(timer)
                    entry.cancelled = True
            alive = [entry for entry in entries
                     if not entry.cancelled and (entry.starting or threading.Thread.is_alive(entry.thread))]
            if alive:
                logs.add("warning", "%d thread(s) the snippet started were still running when it ended: each is stopped "
                                    "while it runs the snippet's code. Join threads before the snippet returns." % len(alive))
            if self._stop_running():
                _start_untracked(threading.Thread(target=self._watch, name="code_exec thread stopper", daemon=True))
        except BaseException:  # a snippet's Thread subclass that raises: the rest of the run still ends cleanly
            pass

    def _stop_running(self):
        """Raises the stop in the tracked threads running the agent's code; returns whether any is still to watch."""
        frames = sys._current_frames()
        watching = False
        for entry in list(self._tracked.values()):
            if entry.done or entry.cancelled or entry.dismissed:
                continue
            if entry.starting:  # started from a thread of the snippet just as it ended
                watching = True
                continue
            thread = entry.thread
            top = frame = frames.get(thread.ident)
            while frame is not None and not self._is_agent_file(frame.f_code.co_filename):
                frame = frame.f_back
            if frame is None or not threading.Thread.is_alive(thread):
                entry.dismissed = True  # ended, never started, or none of the agent's code left on its stack
                continue
            watching = True
            if self._is_agent_file(top.f_code.co_filename) and not entry.done:
                _async_exc(thread.ident, SnippetThreadStopped)
        return watching

    def _watch(self):
        try:
            while True:
                time.sleep(_REFIRE_SECONDS)
                if not self._stop_running():
                    break
        except BaseException:  # interpreter shutdown
            pass


def _async_exc(thread_id, exception):
    _SET_ASYNC_EXC(_THREAD_ID(thread_id), _PY_OBJECT(exception) if exception is not None else None)


# JSON request field -> the type it must have when present (spec section 1: a wrong type is InvalidRequest).
_FIELD_TYPES = {"include": list, "imports": list, "includeRoot": str, "includeOutsideRoot": bool}


def run_json(payload):
    """JSON request (spec section 1 plus Python-only ``imports`` / ``includeRoot``) -> JSON response."""
    try:
        request = json.loads(payload)
    except Exception as error:  # ValueError, RecursionError (deep nesting), TypeError (not a string)
        return _invalid("request is not JSON: %s" % _message(error))
    if not isinstance(request, dict):
        return _invalid("request must be a JSON object")
    for field, kind in _FIELD_TYPES.items():
        if request.get(field) is not None and not isinstance(request[field], kind):
            return _invalid("%s must be a %s" % (field, kind.__name__))
    numbers = {name: request[field] for field, name in _INT_FIELDS.items() if request.get(field) is not None}
    response = run(
        request.get("code", ""),
        include=request.get("include") or (),
        include_root=request.get("includeRoot"),
        imports=request.get("imports") or (),
        include_outside_root=request.get("includeOutsideRoot") is True,
        **numbers)
    try:
        return json.dumps(response, default=str)  # a host converter's odd value must not fail the response
    except Exception:  # non-string keys, a cycle, a str() that raises
        response["value"] = _message(response["value"])
        return json.dumps(response, default=str)


def run_b64(payload_b64):
    """Same as :func:`run_json`, with the request base64-encoded: hosts pass it through one string literal."""
    try:
        payload = base64.b64decode(payload_b64).decode("utf-8")
    except Exception as error:  # ValueError, UnicodeDecodeError, TypeError (not a string)
        return _invalid("request is not base64 UTF-8: %s" % error)
    return run_json(payload)


def _empty():
    return {
        "success": False, "value": None, "logs": [], "errorKind": "None", "error": None, "diagnostics": [],
        "autoImported": [], "compileMs": 0, "executeMs": 0, "cacheHit": False, "truncated": [],
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


def _clip(text, limit, hint):
    """Spec section 6: the first ``limit`` characters plus a note saying how much was shown (``text`` itself if it fits)."""
    if not limit or len(text) <= limit:
        return text
    return "%s... [truncated: %d of %d characters shown; %s]" % (text[:limit], limit, len(text), hint)


def _read(path):
    """Reads an include as Python reads a source file: UTF-8, or the encoding its BOM / coding declaration names."""
    try:
        with tokenize.open(path) as handle:
            return handle.read()
    except (SyntaxError, UnicodeDecodeError, LookupError) as error:  # bad / unknown / non-text coding declaration
        raise ValueError("include is not readable as Python source: %s (%s)" % (path, error)) from None


def _resolve_include(include, root, outside_root):
    """Folders go on sys.path (import by name); ``.py`` files run in the snippet's namespace first, in order. Both must
    lie inside ``root`` unless the host allows ``outside_root``."""
    root_prefix = _prefix(root)  # normcased, ends with a separator: _prefix(path) starts with it for the root and below
    if not isinstance(include, (list, tuple)):
        raise ValueError("include must be a list of paths")
    folders, files, seen = [], [], set()
    for entry in include:
        if not isinstance(entry, str):
            raise ValueError("include entry is not a path: %r" % (entry,))
        if not entry.strip():
            raise ValueError("include entry is empty")
        full = os.path.abspath(entry if os.path.isabs(entry) else os.path.join(root, entry))
        if not outside_root and not _prefix(full).startswith(root_prefix):
            raise ValueError("include is outside the include root: %s" % entry)
        if os.path.normcase(full) in seen:
            continue
        seen.add(os.path.normcase(full))
        if os.path.isdir(full):
            folders.append(full)
        elif os.path.isfile(full):
            if not full.lower().endswith(".py"):
                raise ValueError("include is not a .py file: %s" % entry)
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


class _HostState:
    """What a run may change in the interpreter and restores afterwards, each step on its own: a snippet that broke one
    of them (``del sys.path``) must not keep the others from being restored, nor make the runner raise."""

    def __init__(self, code, sources):
        self.path = list(sys.path)
        self.bytecode = sys.dont_write_bytecode
        self.cwd = os.getcwd()
        self.recursion_limit = sys.getrecursionlimit()
        self.set_recursion_limit, self.get_recursion_limit = sys.setrecursionlimit, sys.getrecursionlimit
        self.cap = None
        self.modules = set(sys.modules)
        # The agent's own sources, so tracebacks show their lines (and never a same-named file elsewhere on sys.path).
        lines = dict(sources)
        lines[SNIPPET_FILE] = code
        self.saved_lines = {path: linecache.cache.get(path) for path in lines}
        for path, source in lines.items():
            linecache.cache[path] = (len(source), None, source.splitlines(True), path)

    def cap_recursion_limit(self, logs):
        """Python < 3.12: the snippet's ``sys.setrecursionlimit`` sets at most the host's cap (restored by :meth:`restore`)."""
        if _CAPPED and _max_recursion_limit:
            self.cap = sys.setrecursionlimit = _RecursionCap(max(_max_recursion_limit, self.recursion_limit), logs)

    def restore(self, prefixes):
        steps = (
            lambda: _SET_RECURSION_LIMIT(self.recursion_limit),  # first: the other steps need the stack
            self._restore_recursion_functions,
            self._restore_path,
            lambda: sys.__setattr__("dont_write_bytecode", self.bytecode),
            lambda: os.chdir(self.cwd),
            lambda: self._forget_modules(prefixes),
            self._forget_lines,
        )
        for step in steps:
            try:
                step()
            except BaseException:  # nothing left to restore it from (the folder is gone, ...): keep restoring the rest
                pass

    def _restore_recursion_functions(self):
        if self.cap is not None:
            self.cap.active = False
        sys.setrecursionlimit, sys.getrecursionlimit = self.set_recursion_limit, self.get_recursion_limit

    def _restore_path(self):
        if isinstance(getattr(sys, "path", None), list):
            sys.path[:] = self.path  # same list object: hosts may hold a reference to it
        else:
            sys.path = self.path

    def _forget_modules(self, prefixes):
        """Drop the modules this run imported from include folders, so an edited helper is re-imported next time. Only
        those: a host module that happens to live under an include folder stays."""
        if not prefixes:
            return
        for name in set(sys.modules) - self.modules:
            try:
                location = sys.modules[name].__file__
            except BaseException:
                continue
            if isinstance(location, str) and _under(location, prefixes):
                del sys.modules[name]

    def _forget_lines(self):
        for path, previous in self.saved_lines.items():
            if previous is None:
                linecache.cache.pop(path, None)
            else:
                linecache.cache[path] = previous



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
    """``Type: message`` plus only the frames the agent wrote (snippet.py, include files, include folders), then the
    exceptions it was raised from (``__cause__``) or while handling (``__context__``), at most _MAX_CAUSES of them."""
    lines = _format_one(error, root, prefixes, include_files)
    seen = {id(error)}
    while True:
        cause, label = _cause(error)
        if cause is None or id(cause) in seen:
            break
        if len(seen) > _MAX_CAUSES:
            lines.append("... more chained exception(s) left out")
            break
        seen.add(id(cause))
        first, *rest = _format_one(cause, root, prefixes, include_files)
        lines.append("%s: %s" % (label, first))
        lines.extend(rest)
        error = cause
    return "\n".join(lines)


def _cause(error):
    if error.__cause__ is not None:
        return error.__cause__, "Caused by"
    if error.__context__ is not None and not error.__suppress_context__:
        return error.__context__, "Raised while handling"
    return None, None


def _format_one(error, root, prefixes, include_files):
    frames = []
    # An explicit limit: a snippet's sys.tracebacklimit must not hide its own frames.
    walked = traceback.walk_tb(error.__traceback__)
    for frame in traceback.StackSummary.extract(walked, limit=sys.maxsize, lookup_lines=False):
        if frame.filename == SNIPPET_FILE or frame.filename in include_files:
            frames.append(frame)
        elif _under(frame.filename, prefixes):
            frame.filename = _relative(os.path.abspath(frame.filename), root)
            frames.append(frame)
    lines = ["%s: %s" % (type(error).__name__, _message(error))]
    if len(frames) > 2 * _FRAMES_KEPT:
        left_out = len(frames) - 2 * _FRAMES_KEPT
        lines.extend(_format_frames(frames[:_FRAMES_KEPT]))
        lines.append("  ... %d frame(s) left out" % left_out)
        lines.extend(_format_frames(frames[-_FRAMES_KEPT:]))
    else:
        lines.extend(_format_frames(frames))
    return lines


def _format_frames(frames):
    # One frame at a time: format_list would fold repeated frames, and the kept frames are exactly the first and last 10.
    return [traceback.format_list([frame])[0].rstrip("\n") for frame in frames]


def _message(value):
    """``str(value)``, or a placeholder when its own ``__str__`` raises."""
    try:
        return str(value)
    except BaseException:
        return "<unprintable message>"


class _Logs:
    """The log budget of spec section 6, applied as lines arrive, so a runaway print loop cannot grow memory: the
    first lines up to 3/4 of the budget, then the most recent ones that still fit."""

    def __init__(self, max_chars, max_line_chars):
        self._max = max_chars
        self._max_line = max_line_chars
        self._head, self._tail = [], collections.deque()
        self._head_open = True
        self._head_used = self._tail_used = 0
        self.left_out = 0
        self.clipped = False

    def add(self, level, message):
        clipped = _clip(message, self._max_line, "print less")
        self.clipped = self.clipped or clipped is not message
        entry = {"level": level, "message": clipped}
        if not self._max:
            self._head.append(entry)
            return
        size = _cost(entry)
        if self._head_open and self._head_used + size <= self._max * 3 // 4:
            self._head.append(entry)
            self._head_used += size
            return
        self._head_open = False
        self._tail.append(entry)
        self._tail_used += size
        while self._tail and self._head_used + self._tail_used > self._max:
            self._tail_used -= _cost(self._tail.popleft())
            self.left_out += 1

    def entries(self):
        if not self.left_out:
            return self._head + list(self._tail)
        marker = {"level": "warning", "message": "... %d log line(s) left out (output over %d characters): print less, "
                                                 "or return a summary" % (self.left_out, self._max)}
        return self._head + [marker] + list(self._tail)


def _cost(entry):
    return len(entry["message"]) + _LOG_ENTRY_CHARS


class _LogStream(io.TextIOBase):
    """Collects written text as one log entry per line."""

    def __init__(self, logs, level):
        super().__init__()
        self._logs = logs
        self._level = level
        self._parts = []

    def writable(self):
        return True

    def write(self, text):
        if not isinstance(text, str):
            raise TypeError("write() argument must be str, not %s" % type(text).__name__)
        *ended, rest = text.split("\n")
        for piece in ended:
            self._parts.append(piece)
            self._logs.add(self._level, "".join(self._parts).rstrip("\r"))
            self._parts = []
        if rest:
            self._parts.append(rest)
        return len(text)

    def flush(self):
        if self._parts:
            self._logs.add(self._level, "".join(self._parts))
            self._parts = []
