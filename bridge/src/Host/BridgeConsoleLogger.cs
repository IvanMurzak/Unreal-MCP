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
using Microsoft.Extensions.Logging;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Host
{
    /// <summary>
    /// A dependency-free <see cref="ILoggerProvider"/> that writes one line per log to <c>stderr</c>
    /// (stdout is reserved for protocol/handshake output the parent plugin may parse). Used to surface
    /// both the bridge's own diagnostics and the reused framework's <c>Microsoft.Extensions.Logging</c>
    /// output (connection/handshake/version-mismatch) without pulling in a logging package. Never logs
    /// secrets — the IPC token travels via stdin and is never passed to a logger (§1.4).
    /// </summary>
    public sealed class BridgeConsoleLoggerProvider : ILoggerProvider
    {
        private readonly LogLevel _minLevel;
        public BridgeConsoleLoggerProvider(LogLevel minLevel = LogLevel.Information) => _minLevel = minLevel;
        public ILogger CreateLogger(string categoryName) => new BridgeConsoleLogger(categoryName, _minLevel);
        public void Dispose() { }
    }

    internal sealed class BridgeConsoleLogger : ILogger
    {
        private readonly string _category;
        private readonly LogLevel _minLevel;

        public BridgeConsoleLogger(string category, LogLevel minLevel)
        {
            _category = category;
            _minLevel = minLevel;
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
        }

        private sealed class NullScope : IDisposable
        {
            public static readonly NullScope Instance = new();
            public void Dispose() { }
        }
    }
}
