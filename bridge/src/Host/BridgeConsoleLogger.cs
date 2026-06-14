/*
┌───────────────────────────────────────────────────────────────────┐
│  Author: Ivan Murzak (https://github.com/IvanMurzak)              │
│  Repository: GitHub (https://github.com/IvanMurzak/Unreal-MCP)    │
│  Copyright (c) 2026 Ivan Murzak                                   │
│  Licensed under the Apache License, Version 2.0.                  │
│  See the LICENSE file in the project root for more information.   │
└───────────────────────────────────────────────────────────────────┘
*/

using System;
using System.IO;
using System.Text;
using Microsoft.Extensions.Logging;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Host
{
    /// <summary>
    /// A dependency-free <see cref="ILoggerProvider"/> that writes one line per log to <c>stderr</c>
    /// (stdout is reserved for protocol/handshake output the parent plugin may parse). Used to surface
    /// both the bridge's own diagnostics and the reused framework's <c>Microsoft.Extensions.Logging</c>
    /// output (connection/handshake/version-mismatch) without pulling in a logging package. Never logs
    /// secrets — the IPC token travels via stdin and is never passed to a logger (§1.4).
    ///
    /// When a <c>logFilePath</c> is supplied (issue #69 — the plugin passes <c>log-file=&lt;path&gt;</c>),
    /// every formatted line is ALSO teed to that file (append mode, thread-safe) so the cloud-connection
    /// lifecycle is visible even though the plugin discards the child's stderr. The file sink degrades
    /// silently to stderr-only if the path is unwritable — logging must never crash the sidecar.
    /// </summary>
    public sealed class BridgeConsoleLoggerProvider : ILoggerProvider
    {
        private readonly LogLevel _minLevel;
        private readonly BridgeLogFileSink? _fileSink;

        public BridgeConsoleLoggerProvider(LogLevel minLevel = LogLevel.Information, string? logFilePath = null)
        {
            _minLevel = minLevel;
            _fileSink = BridgeLogFileSink.TryCreate(logFilePath);
        }

        public ILogger CreateLogger(string categoryName) => new BridgeConsoleLogger(categoryName, _minLevel, _fileSink);

        public void Dispose() => _fileSink?.Dispose();
    }

    /// <summary>
    /// Thread-safe append-only file tee for the bridge log. Created once per provider; shared by every
    /// logger it hands out. Constructed only via <see cref="TryCreate"/>, which returns <c>null</c> (→
    /// stderr-only) for a null/blank/unwritable path so the logging path can never throw (issue #69).
    /// </summary>
    internal sealed class BridgeLogFileSink : IDisposable
    {
        private readonly object _gate = new();
        private readonly string _path;
        private bool _disabled;

        private BridgeLogFileSink(string path) => _path = path;

        /// <summary>
        /// Returns a sink for <paramref name="logFilePath"/>, or <c>null</c> when no path is given or the
        /// path/parent-directory cannot be prepared. Never throws.
        /// </summary>
        public static BridgeLogFileSink? TryCreate(string? logFilePath)
        {
            if (string.IsNullOrWhiteSpace(logFilePath))
                return null;

            try
            {
                var fullPath = Path.GetFullPath(logFilePath);
                var dir = Path.GetDirectoryName(fullPath);
                if (!string.IsNullOrEmpty(dir))
                    Directory.CreateDirectory(dir);
                return new BridgeLogFileSink(fullPath);
            }
            catch
            {
                // Unwritable / invalid path → silently degrade to stderr-only.
                return null;
            }
        }

        /// <summary>Appends one line to the file. Swallows every failure (degrade to stderr-only).</summary>
        public void WriteLine(string line)
        {
            if (_disabled)
                return;

            lock (_gate)
            {
                if (_disabled)
                    return;
                try
                {
                    File.AppendAllText(_path, line + Environment.NewLine, Encoding.UTF8);
                }
                catch
                {
                    // The file became unwritable mid-run (deleted dir, permissions, full disk): stop
                    // trying and never throw out of the logging path.
                    _disabled = true;
                }
            }
        }

        public void Dispose() { /* File.AppendAllText opens+closes per write; nothing to hold open. */ }
    }

    internal sealed class BridgeConsoleLogger : ILogger
    {
        private readonly string _category;
        private readonly LogLevel _minLevel;
        private readonly BridgeLogFileSink? _fileSink;

        public BridgeConsoleLogger(string category, LogLevel minLevel, BridgeLogFileSink? fileSink = null)
        {
            _category = category;
            _minLevel = minLevel;
            _fileSink = fileSink;
        }

        public IDisposable BeginScope<TState>(TState state) where TState : notnull => NullScope.Instance;
        public bool IsEnabled(LogLevel logLevel) => logLevel >= _minLevel && logLevel != LogLevel.None;

        public void Log<TState>(LogLevel logLevel, EventId eventId, TState state, Exception? exception, Func<TState, Exception?, string> formatter)
        {
            if (!IsEnabled(logLevel))
                return;

            var message = formatter(state, exception);
            var line = $"[unreal-mcp-bridge] {logLevel.ToString().ToLowerInvariant()} {_category}: {message}";
            if (exception != null)
                line += $" | {exception.GetType().Name}: {exception.Message}";
            Console.Error.WriteLine(line);
            _fileSink?.WriteLine(line);
        }

        private sealed class NullScope : IDisposable
        {
            public static readonly NullScope Instance = new();
            public void Dispose() { }
        }
    }
}
