"""Snippet runner: spec/contract.md sections 4 and 6. Standard library only, Python 3.10+."""

import ast
import base64
import builtins
import collections
import io
import json
import _thread
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
_SLICE_SECONDS = 0.05  # a blocking wait of the snippet's threads (join, Event.wait, time.sleep, ...) looks at the budget this often
_MARGIN_SECONDS = 0.05  # a snippet ending this long before its deadline was never stopped: no need to wait for the watchdog

# Python < 3.12 has no C stack check: a snippet that raises the recursion limit and then recurses in C (repr / json.dumps
# of a deeply nested list) overflows the C stack and ends the host. A snippet's sys.setrecursionlimit is capped there
# (spec section 4). 3000 is conservative for Unreal's game thread (12 MB stack): in UnrealEditor 5.7 (Python 3.11.8)
# every C recursion path measured survived at 4000; the costliest (a sort key that sorts again) ended it at 4500.
MAX_RECURSION_LIMIT = 3000
_CAPPED = sys.version_info < (3, 12)
_max_recursion_limit = MAX_RECURSION_LIMIT
_SET_RECURSION_LIMIT = sys.setrecursionlimit  # bound once: the runner restores limits with it, whatever a snippet replaced
# Bound once: the runner's own threads sleep and look at the clock with these, whatever a snippet (or the runner) replaced.
_sleep, _monotonic, _is_alive = time.sleep, time.monotonic, threading.Thread.is_alive

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
    agent_files = {SNIPPET_FILE} | {path for path, _ in files}
    threads = _SnippetThreads(lambda filename: filename in agent_files or _under(filename, prefixes))
    watchdog = _Watchdog(timeout_ms, threads)
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
            if watchdog.fired:  # it outran its budget in a call nothing could interrupt (a C call, a lock): no success
                raise threads.stop_type(
                    "The snippet ran longer than its time budget of %d ms (timeoutMs): it finished only after %d ms, blocked "
                    "in a call that cannot be interrupted (a C call, a lock, an engine call)." % (timeout_ms, _ms(executing)))
            threads.disarm()
        result["value"] = _convert(value)
        result["success"] = True
    except BaseException as error:  # SystemExit, KeyboardInterrupt, ...: a snippet must never take the host down
        _make_room(saved)
        watchdog.stop()
        if isinstance(error, SnippetThreadStopped) and threads.stopping:
            # A thread stopped at the deadline handed its stop on (Future.result): the run ran out of time.
            error = threads.stop_type().with_traceback(error.__traceback__)
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
        if self.args:
            return str(self.args[0])
        return "The snippet ran longer than its time budget of %d ms (timeoutMs) and was stopped." % type(self).budget_ms


class _Watchdog:
    """Stops a snippet that outruns its time budget: a timer thread raises :class:`SnippetTimeout` in the snippet's thread
    (``PyThreadState_SetAsyncExc``), again every ``_REFIRE_SECONDS`` while the snippet catches it. Python delivers it
    between bytecodes, so pure Python loops stop; a call into C (an engine call) finishes first. At the deadline the
    threads the snippet started are stopped too (:meth:`_SnippetThreads.deadline_passed`): a snippet waiting for one of
    them (``join``, ``Future.result``) gets it back. A snippet's thread inside a sliced wait (:func:`_sliced`) gets no
    asynchronous stop: the wait raises it itself between two slices, where the lock it waits with is held again.

    No lock is ever taken in the snippet's thread (a stop delivered right after acquiring one would leave it held):
    :meth:`stop` clears a flag, waits for the timer thread to end if it may have raised, then drops a stop raised but not
    delivered yet. None reaches the host after the run."""

    def __init__(self, timeout_ms, threads):
        self._timeout_ms = timeout_ms
        self._threads = threads
        self._thread_id = None
        self._deadline = 0.0
        self._armed = self._fired = self._exited = False

    def start(self):
        if not self._timeout_ms or _SET_ASYNC_EXC is None:
            return
        stop = type("SnippetTimeout", (SnippetTimeout,), {"budget_ms": self._timeout_ms})
        self._thread_id = threading.get_ident()
        self._deadline = _monotonic() + self._timeout_ms / 1000.0
        self._threads.arm(self._deadline, stop)
        self._armed = True
        _start_untracked(threading.Thread(target=self._watch, args=(stop,), name="code_exec watchdog", daemon=True))

    def _watch(self, stop):
        try:
            deadline = self._deadline
            while self._armed:
                now = _monotonic()
                if now < deadline:
                    _sleep(min(_POLL_SECONDS, deadline - now))
                elif self._armed:
                    first = not self._fired
                    self._fired = True
                    if not self._threads.main_waiting:
                        _async_exc(self._thread_id, stop)
                    if first:
                        self._threads.deadline_passed()
                    deadline = now + _REFIRE_SECONDS
        finally:
            self._exited = True

    @property
    def fired(self):
        """Whether the deadline passed while the snippet still ran."""
        return self._fired

    def stop(self):
        while self._thread_id is not None:
            try:
                self._armed = False
                # Before the deadline (and never fired) the timer cannot raise any more: it looks at the flag before it
                # would. Otherwise wait until it is gone, then drop what it may have raised.
                if self._fired or _monotonic() >= self._deadline - _MARGIN_SECONDS:
                    while not self._exited:
                        _sleep(0.001)
                    _async_exc(self._thread_id, None)
                self._thread_id = None
            except SnippetTimeout:  # delivered while disarming: disarm again
                pass


class SnippetThreadStopped(BaseException):
    """Raised in a thread the snippet started once the snippet has ended or ran out of its time budget (spec section 4):
    threads may not outlive it."""

    def __str__(self):
        return ("The snippet has ended (or ran out of its time budget), so this thread it started was stopped: join "
                "threads before the snippet returns.")


# Runs whose snippet is running. While there is one, the runner stands in for threading.Thread.start (tracking) and for
# the blocking waits (sliced): each entry is (owner, attribute, stand-in). A host that replaced one of them meanwhile
# keeps the stand-in in its chain: that one is then never installed again (_kept), its original stays in _originals.
_thread_runs = []
_originals = {}
_kept = set()
_thread_run = {}  # thread ident -> the innermost run it belongs to (its snippet's thread, or a thread the run started)


def _original(name):
    """What the stand-in ``name`` stands in for (the attribute itself while it is not installed)."""
    original = _originals.get(name)
    if original is None:
        owner, attribute, _ = _PATCHES[name]
        original = getattr(owner, attribute)
    return original


def _install():
    for name, (owner, attribute, stand_in) in _PATCHES.items():
        if name not in _kept:
            _originals[name] = getattr(owner, attribute)
            setattr(owner, attribute, stand_in)


def _uninstall():
    for name, (owner, attribute, stand_in) in _PATCHES.items():
        if name in _kept:
            continue
        if getattr(owner, attribute) is stand_in:
            setattr(owner, attribute, _originals.pop(name))
        else:
            _kept.add(name)


def _dispatch_start(thread, *args, **kwargs):
    """``threading.Thread.start`` while snippets run: a thread started from a snippet's thread (or one of its threads) is
    that run's."""
    run = _thread_run.get(threading.get_ident())
    if run is not None and run.active:
        return run.start_tracked(thread, args, kwargs)
    return _original("start")(thread, *args, **kwargs)


def _start_untracked(thread):
    """Starts one of the runner's own threads: never a snippet's."""
    _original("start")(thread)


def _long(timeout):
    """Whether a wait of ``timeout`` seconds (None = no bound) is long enough to slice. Anything else (a short or
    negative one, NaN, infinity, a type Python's own rejects) goes to Python's own wait unchanged."""
    if timeout is None:
        return True
    return isinstance(timeout, (int, float)) and _SLICE_SECONDS < timeout < float("inf")


def _run_to_slice(timeout):
    """The run whose rules a wait of ``timeout`` seconds on this thread follows when it is sliced, else None: the thread
    belongs to a run that may stop it and the wait is long (:func:`_long`)."""
    ident = threading.get_ident()
    run = _thread_run.get(ident) if _thread_run else None
    return run if run is not None and _long(timeout) and run.may_stop(ident) else None


def _slices(run, timeout, wait):
    """Waits ``wait(seconds)`` (returns whether the wait is over; ``None`` = no bound) for at most ``timeout`` seconds
    (``None`` = no bound) in slices of at most ``_SLICE_SECONDS``. Stops at the first slice after which the thread is due
    a stop (:meth:`_SnippetThreads.due`). Returns ``(done, stop)``: what the last slice returned, and the stop not raised
    yet (the caller raises it once its state is consistent again), or None."""
    ident = threading.get_ident()
    end = None if timeout is None else _monotonic() + timeout
    while True:
        if run.ended and not run.stops(ident):
            # Its run is over and nothing of the agent's is left on its stack: wait the rest as Python does.
            return wait(None if end is None else max(0.0, end - _monotonic())), None
        left = _SLICE_SECONDS if end is None else max(0.0, min(_SLICE_SECONDS, end - _monotonic()))
        done, stop = run.wait_slice(ident, wait, left)
        if stop is not None or done or (end is not None and _monotonic() >= end):
            return done, stop


def _sliced(timeout, wait):
    """A blocking wait on a snippet's thread, sliced (:func:`_slices`) when :func:`_run_to_slice` says so, so a wait that
    never ends cannot outlast the time budget; any other wait is Python's own. Returns what the last slice returned."""
    run = _run_to_slice(timeout)
    if run is None:
        return wait(timeout)
    done, stop = _slices(run, timeout, wait)
    if stop is not None:
        raise stop
    return done


def _sliced_sleep(seconds):
    """``time.sleep`` while snippets run (spec section 4)."""
    sleep = _original("sleep")
    if seconds is None:  # Python's own TypeError, not a wait without bound
        return sleep(seconds)
    _sliced(seconds, lambda left: sleep(left) or False)


def _sliced_join(thread, timeout=None):
    """``threading.Thread.join`` while snippets run (spec section 4)."""
    join = _original("join")

    def once(left):
        join(thread, left)  # raises as Python's own does: not started, the current thread
        return not _is_alive(thread)

    _sliced(timeout, once)


def _sliced_wait(condition, timeout=None):
    """``threading.Condition.wait`` while snippets run (spec section 4): ``Event.wait``, ``Semaphore.acquire``,
    ``queue.Queue.get`` / ``put``, ``concurrent.futures.Future.result`` and ``Barrier.wait`` wait through it. Sliced, it
    is Python's own ``Condition.wait`` with the waiter lock acquired in slices: ONE waiter for the whole wait, so a
    ``notify`` is never lost between two slices; a stop due between two is raised once the lock is held again."""
    run = _run_to_slice(timeout)
    if run is None:
        return _original("wait")(condition, timeout)
    if not condition._is_owned():
        raise RuntimeError("cannot wait on un-acquired lock")
    waiter = _thread.allocate_lock()
    waiter.acquire()
    condition._waiters.append(waiter)
    saved_state = condition._release_save()
    gotit = False
    try:
        gotit, stop = _slices(run, timeout, lambda left: waiter.acquire() if left is None else waiter.acquire(True, left))
    finally:  # restore state no matter what, as Python's own does
        condition._acquire_restore(saved_state)
        if not gotit:
            try:
                condition._waiters.remove(waiter)
            except ValueError:
                pass
    if stop is not None:
        raise stop
    return gotit


_PATCHES = {
    "start": (threading.Thread, "start", _dispatch_start),
    "join": (threading.Thread, "join", _sliced_join),
    "wait": (threading.Condition, "wait", _sliced_wait),
    "sleep": (time, "sleep", _sliced_sleep),
}


class _Tracked:
    __slots__ = ("thread", "starting", "done", "cancelled", "dismissed")

    def __init__(self, thread):
        self.thread = thread
        self.starting = self.done = self.cancelled = self.dismissed = False


class _SnippetThreads:
    """One run's threads: ``threading.Thread.start`` called from the snippet's thread, or from one of those threads,
    while the snippet runs. At the deadline and when it ends, a ``threading.Timer`` not fired yet is cancelled (at the
    end) and every other one still alive gets :class:`SnippetThreadStopped` (``PyThreadState_SetAsyncExc``) whenever the
    agent's own code (``is_agent_file``) is the frame it runs, again every ``_REFIRE_SECONDS`` while that code is on its
    stack; one inside a sliced wait raises it there itself. A thread with none of it on its stack (a pool worker waiting
    for work) is left alone once the run has ended. The thread swallows the stop: nothing is printed."""

    def __init__(self, is_agent_file):
        self._is_agent_file = is_agent_file
        self._tracked = {}  # id(thread) -> _Tracked; only ever added to, so the stopper thread can read it any time
        self._previous = {}  # thread ident -> the run it belonged to before this one (nested runs)
        self._stopper = None
        self.main_ident = None
        self.active = self.ended = self.stopping = False
        self.main_waiting = False  # the snippet's thread is inside a sliced wait: it raises the timeout itself
        self.deadline, self.stop_type = 0.0, None

    def start(self):
        if _SET_ASYNC_EXC is None:
            return
        if not _thread_runs:
            _install()
        _thread_runs.append(self)
        self.main_ident = threading.get_ident()
        self._enter(self.main_ident)
        self.active = True

    def arm(self, deadline, stop_type):
        self.deadline, self.stop_type = deadline, stop_type

    def _enter(self, ident):
        self._previous[ident] = _thread_run.get(ident)
        _thread_run[ident] = self

    def _leave(self, ident):
        previous = self._previous.pop(ident, None)
        if _thread_run.get(ident) is self:
            if previous is None:
                _thread_run.pop(ident, None)
            else:
                _thread_run[ident] = previous

    def _chain(self, ident):
        """This run and the runs ``ident`` belonged to before it (nested runs), innermost first."""
        run = self
        while run is not None:
            yield run
            run = run._previous.get(ident)

    def wait_slice(self, ident, wait, seconds):
        """One slice of a sliced wait of thread ``ident``: ``(done, the stop it is due or None)``."""
        mains = [run for run in self._chain(ident) if run.main_ident == ident]
        for run in mains:
            run.main_waiting = True
        try:
            done = wait(seconds)
        finally:
            for run in mains:
                run.main_waiting = False
        # Also when the wait is over: what it waited for may have been stopped by the deadline.
        stop = next((stop for stop in (run.due(ident) for run in self._chain(ident)) if stop is not None), None)
        return done, stop

    def due(self, ident):
        """The stop thread ``ident`` is due between two slices of a wait, or None: the run's timeout in its snippet's
        thread once the deadline has passed, :class:`SnippetThreadStopped` in a thread it started once it is stopping."""
        if ident == self.main_ident:
            if self.stop_type is not None and _monotonic() >= self.deadline:
                return self.stop_type()
        elif self.stops(ident):
            return SnippetThreadStopped()
        return None

    def disarm(self):
        """The snippet has finished in time: what runs after it (the host's value converter) is no longer budgeted."""
        self.stop_type = None

    def stops(self, ident):
        """Whether thread ``ident`` (one the run started) is to stop: the run is stopping and the agent's code is on the
        calling thread's stack."""
        return self.stopping and ident != self.main_ident and self._agent_frame(sys._getframe(1)) is not None

    def may_stop(self, ident):
        """Whether a wait of thread ``ident`` may ever have to stop: a thread the run (or an outer run) started, or the
        snippet's thread of a run with a time budget. Others are not worth slicing."""
        return any(run.main_ident != ident or run.stop_type is not None for run in self._chain(ident))

    def _agent_frame(self, frame):
        """The innermost frame of the agent's code from ``frame`` outwards, or None."""
        while frame is not None and not self._is_agent_file(frame.f_code.co_filename):
            frame = frame.f_back
        return frame

    def start_tracked(self, thread, args, kwargs):
        start = _original("start")
        if id(thread) in self._tracked:  # started twice: the second start raises as Python's own does
            return start(thread, *args, **kwargs)
        entry = self._tracked[id(thread)] = _Tracked(thread)
        run = thread.run

        def tracked():
            ident = threading.get_ident()
            self._enter(ident)
            try:
                try:
                    run()
                finally:
                    while True:  # a stop raised while leaving must not outlive the thread either
                        try:
                            entry.done = True
                            self._leave(ident)
                            _async_exc(ident, None)
                            break
                        except SnippetThreadStopped:
                            pass
            except SnippetThreadStopped:
                pass

        thread.run = tracked
        entry.starting = True
        try:
            return start(thread, *args, **kwargs)
        finally:
            entry.starting = False

    def deadline_passed(self):
        """The watchdog's thread, at the deadline: the threads the snippet started stop now, not only once it has ended
        (the snippet may be waiting for one of them). Never raises."""
        try:
            self.stopping = True
            if self._stop_running():
                self._watch_on()
        except BaseException:
            pass

    def stop(self, logs):
        """Never raises: the host's state is restored after it, whatever the snippet's threads do."""
        if not self.active:
            return
        self.active = False
        try:
            self.ended = self.stopping = True
            _thread_runs.remove(self)
            if not _thread_runs:
                _uninstall()
            self._leave(self.main_ident)
            entries = list(self._tracked.values())
            for entry in entries:
                timer = entry.thread
                if isinstance(timer, threading.Timer) and not timer.finished.is_set():
                    threading.Timer.cancel(timer)
                    entry.cancelled = True
            alive = [entry for entry in entries
                     if not entry.cancelled and (entry.starting or _is_alive(entry.thread))]
            if alive:
                logs.add("warning", "%d thread(s) the snippet started were still running when it ended: each is stopped "
                                    "while it runs the snippet's code. Join threads before the snippet returns." % len(alive))
            if self._stop_running():
                self._watch_on()
        except BaseException:  # a snippet's Thread subclass that raises: the rest of the run still ends cleanly
            pass

    def _watch_on(self):
        """Starts the stopper thread (once): it raises the stop again every ``_REFIRE_SECONDS``."""
        if self._stopper is None:
            self._stopper = threading.Thread(target=self._watch, name="code_exec thread stopper", daemon=True)
            _start_untracked(self._stopper)

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
            top = frames.get(thread.ident)
            if self._agent_frame(top) is None or not _is_alive(thread):
                # Ended, never started, or none of the agent's code left on its stack: once the run has ended, for good.
                # Before (at the deadline) it may be between two tasks or not run the agent's code yet: look again.
                entry.dismissed = self.ended
                watching = watching or not self.ended
                continue
            watching = True
            if self._is_agent_file(top.f_code.co_filename) and not entry.done:
                _async_exc(thread.ident, SnippetThreadStopped)
        return watching

    def _watch(self):
        try:
            while True:
                _sleep(_REFIRE_SECONDS)
                if not self._stop_running() and self.ended:
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
