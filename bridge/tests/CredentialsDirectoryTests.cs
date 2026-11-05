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
using com.IvanMurzak.Unreal.MCP.Bridge.Host;
using Xunit;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tests
{
    public class CredentialsDirectoryTests
    {
        [Fact]
        public void Unset_or_blank_keeps_the_platform_default()
        {
            Assert.Null(CredentialsDirectory.Resolve(_ => null));
            Assert.Null(CredentialsDirectory.Resolve(_ => "  "));
        }

        [Fact]
        public void An_absolute_folder_is_used_normalized()
        {
            var dir = Path.Combine(Path.GetTempPath(), "aigd-dev", "..", "aigd-dev-store");
            Assert.Equal(Path.GetFullPath(dir), CredentialsDirectory.Resolve(name => name == CredentialsDirectory.EnvName ? dir : null));
        }

        [Fact]
        public void A_relative_folder_is_refused()
        {
            Assert.Throws<ArgumentException>(() => CredentialsDirectory.Resolve(_ => "relative/store"));
        }
    }
}
