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
using System.Diagnostics;
using System.IO;
using com.IvanMurzak.McpPlugin.AgentConfig;
using com.IvanMurzak.Unreal.MCP.Bridge.Host;
using Xunit;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tests
{
    /// <summary>
    /// The routing pin comes from the project folder's REAL path: an editor opened through a symlink (macOS
    /// <c>/var</c> → <c>/private/var</c>), a junction or an 8.3 name registers under the same pin as one opened through
    /// the real path, the pin the CLI and the App derive (cli-core <c>canonicalPath</c>).
    /// </summary>
    public class CanonicalProjectPathTests
    {
        [Fact]
        public void Resolve_SymlinkedProjectFolder_GetsTheRealFolderPin()
        {
            WithLinkedProject(link: CreateSymlink, (real, link) =>
            {
                var viaLink = ProjectConnectionResolver.Resolve(link, instanceId: "a");
                var viaReal = ProjectConnectionResolver.Resolve(real, instanceId: "b");

                Assert.NotEqual(ProjectIdentity.DerivePin(link), ProjectIdentity.DerivePin(real)); // the spellings differ
                Assert.Equal(viaReal.Pin, viaLink.Pin);
                Assert.Equal(viaReal.ProjectPathHash, viaLink.ProjectPathHash);
                Assert.Equal(viaReal.HostlessPort, viaLink.HostlessPort);
                // FPaths::ProjectDir() ends with '/': the same folder, the same pin.
                Assert.Equal(viaReal.Pin, ProjectConnectionResolver.Resolve(link + "/", instanceId: "c").Pin);
            });
        }

        [Fact]
        public void Resolve_MacVarTmp_GetsThePrivateVarPin_TheCliAndAppDerive()
        {
            if (!OperatingSystem.IsMacOS())
                return; // /var → /private/var is macOS's own symlink
            var name = "unreal-mcp-pin-" + Guid.NewGuid().ToString("N");
            var spelled = "/var/tmp/" + name;
            Directory.CreateDirectory(spelled);
            try
            {
                var resolved = ProjectConnectionResolver.Resolve(spelled + "/", instanceId: "a");

                // The App's own pin of the project: cli-core deriveProjectIdentityV2(realpath) (v2: '\' → '/', lowercased).
                Assert.Equal(ProjectIdentity.DerivePinV2("/private/var/tmp/" + name), resolved.Pin);
                Assert.Equal(ProjectIdentity.DeriveProjectPathHashV2("/private/var/tmp/" + name), resolved.ProjectPathHash);
                Assert.Equal("/private/var/tmp/" + name, resolved.HashInput);
            }
            finally
            {
                Directory.Delete(spelled);
            }
        }

        [Fact]
        public void Resolve_WindowsJunction_GetsTheTargetFolderPin()
        {
            if (!OperatingSystem.IsWindows())
                return;
            WithLinkedProject(link: CreateJunction, (real, link) =>
            {
                Assert.Equal(ProjectConnectionResolver.Resolve(real, "b").Pin, ProjectConnectionResolver.Resolve(link, "a").Pin);
                Assert.DoesNotContain('\\', CanonicalProjectPath.Resolve(link)); // FPaths spelling: '/' only
            });
        }

        [Theory]
        [InlineData("/home/user/my-game")]          // golden vector: absent folders keep their spelling
        [InlineData("relative/project")]
        [InlineData(@"\\server\share\game")]       // UNC: never resolved (an offline share blocks)
        [InlineData("")]
        public void Resolve_UnresolvablePath_IsKeptAsGiven(string path)
        {
            Assert.Equal(path, CanonicalProjectPath.Resolve(path));
        }

        private static void WithLinkedProject(Func<string, string, bool> link, Action<string, string> body)
        {
            var root = Path.Combine(Path.GetTempPath(), "unreal-mcp-pin-" + Guid.NewGuid().ToString("N"));
            var real = Path.Combine(root, "RealProject");
            var linked = Path.Combine(root, "LinkedProject");
            Directory.CreateDirectory(real);
            try
            {
                if (!link(linked, real))
                    return; // no privilege to create the link on this machine (Windows without developer mode)
                body(real, linked);
            }
            finally
            {
                if (Directory.Exists(linked))
                    Directory.Delete(linked); // removes the link, not its target
                Directory.Delete(root, recursive: true);
            }
        }

        private static bool CreateSymlink(string link, string target)
        {
            try
            {
                Directory.CreateSymbolicLink(link, target);
                return true;
            }
            catch (Exception ex) when (OperatingSystem.IsWindows() && ex is UnauthorizedAccessException or IOException)
            {
                return false;
            }
        }

        private static bool CreateJunction(string link, string target)
        {
            using var mklink = Process.Start(new ProcessStartInfo("cmd.exe", $"/c mklink /J \"{link}\" \"{target}\"")
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
            })!;
            mklink.WaitForExit();
            return mklink.ExitCode == 0;
        }
    }
}
