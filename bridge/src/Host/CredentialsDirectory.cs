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
using Microsoft.Extensions.Logging;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Host
{
    /// <summary>
    /// Explicit machine-credential store location for development/test editors, the twin of the
    /// Godot addon's <c>GODOT_MCP_CREDENTIALS_DIR</c>. A path, never a token: the bridge still reads
    /// (and refreshes) the credential store-read (design 04), only from this folder instead of the
    /// OS user profile. Needed where the profile .NET resolves is not the one the launching App
    /// signed in to: the AI Game Dev App's local dev stand swaps HOME/USERPROFILE to a dev profile,
    /// which <c>Environment.GetFolderPath(UserProfile)</c> ignores on Windows.
    /// The sidecar inherits the editor's environment, so the launcher sets it on the editor.
    /// </summary>
    public static class CredentialsDirectory
    {
        public const string EnvName = "UNREAL_MCP_CREDENTIALS_DIR";

        /// <summary>The folder from <see cref="EnvName"/>, or null for the platform default. Only absolute paths.</summary>
        public static string? Resolve(Func<string, string?>? getEnv = null)
        {
            var directory = (getEnv ?? Environment.GetEnvironmentVariable)(EnvName);
            if (string.IsNullOrWhiteSpace(directory))
                return null;
            if (!Path.IsPathFullyQualified(directory))
                throw new ArgumentException($"{EnvName} must be an absolute path.");
            return Path.GetFullPath(directory);
        }

        /// <summary>
        /// <see cref="Resolve"/> for the sidecar's start: an invalid value is logged and ignored (the platform
        /// default is used) rather than thrown, since a throw before the IPC handshake would leave the editor
        /// restarting a sidecar that dies at once, with nothing in the log saying why.
        /// </summary>
        public static string? ResolveOrDefault(ILogger? logger, Func<string, string?>? getEnv = null)
        {
            try
            {
                return Resolve(getEnv);
            }
            catch (ArgumentException e)
            {
                logger?.LogError("{Message} Using the default machine credential store.", e.Message);
                return null;
            }
        }
    }
}
