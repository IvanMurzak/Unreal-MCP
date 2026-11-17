using System;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Host
{
    /// <summary>
    /// The real path of a project folder, the one spelling every client derives the project's routing pin from.
    /// The editor reports the folder it was opened with (<c>FPaths::ProjectDir()</c>), which can be a symlink
    /// (macOS <c>/var</c> → <c>/private/var</c>), a Windows junction, a <c>subst</c> drive or an 8.3 short name; hashed
    /// as given, two spellings of one folder register under two pins, and a client that derives the pin from the real
    /// path (the CLI and the App: cli-core <c>canonicalPath</c> = Node <c>realpathSync.native</c>) misses the editor.
    /// </summary>
    internal static class CanonicalProjectPath
    {
        /// <summary>
        /// <paramref name="projectRoot"/> with every symlink / junction resolved (realpath on macOS and Linux,
        /// <c>GetFinalPathNameByHandle</c> on Windows, which also expands 8.3 names), or <paramref name="projectRoot"/>
        /// unchanged when it is not an existing absolute folder, is a UNC path (resolving one blocks for the SMB timeout
        /// while the share is offline; cli-core leaves it as given too) or cannot be resolved. Windows results use
        /// <c>/</c> like <c>FPaths</c> does, so the v1 pin (separators kept) stays equal to the v2 one. Never throws.
        /// </summary>
        public static string Resolve(string projectRoot)
        {
            if (string.IsNullOrEmpty(projectRoot) || !Path.IsPathFullyQualified(projectRoot) || IsUncPath(projectRoot))
                return projectRoot;
            try
            {
                if (!Directory.Exists(projectRoot))
                    return projectRoot;
                var real = OperatingSystem.IsWindows() ? WindowsFinalPath(projectRoot) : UnixRealPath(projectRoot);
                return string.IsNullOrEmpty(real) ? projectRoot : real;
            }
            catch (Exception)
            {
                return projectRoot;
            }
        }

        private static bool IsUncPath(string path) =>
            path.StartsWith(@"\\", StringComparison.Ordinal) || path.StartsWith("//", StringComparison.Ordinal);

        // ── POSIX ─────────────────────────────────────────────────────────────────────────────────────────────────

        [DllImport("libc", EntryPoint = "realpath", SetLastError = true)]
        private static extern IntPtr RealPath([MarshalAs(UnmanagedType.LPUTF8Str)] string path, IntPtr resolved);

        [DllImport("libc", EntryPoint = "free")]
        private static extern void Free(IntPtr pointer);

        private static string? UnixRealPath(string path)
        {
            var buffer = RealPath(path, IntPtr.Zero); // NULL → libc allocates the result
            if (buffer == IntPtr.Zero)
                return null;
            try { return Marshal.PtrToStringUTF8(buffer); }
            finally { Free(buffer); }
        }

        // ── Windows ───────────────────────────────────────────────────────────────────────────────────────────────

        private const uint FileShareAll = 0x1 | 0x2 | 0x4;     // FILE_SHARE_READ | WRITE | DELETE
        private const uint OpenExisting = 3;
        private const uint FileFlagBackupSemantics = 0x02000000; // required to open a directory handle

        [DllImport("kernel32.dll", EntryPoint = "CreateFileW", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern SafeFileHandle CreateFile(string name, uint access, uint share, IntPtr security,
            uint disposition, uint flags, IntPtr template);

        [DllImport("kernel32.dll", EntryPoint = "GetFinalPathNameByHandleW", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern uint GetFinalPathNameByHandle(SafeFileHandle file, char[] buffer, uint length, uint flags);

        private static string? WindowsFinalPath(string path)
        {
            using var handle = CreateFile(path, 0, FileShareAll, IntPtr.Zero, OpenExisting, FileFlagBackupSemantics, IntPtr.Zero);
            if (handle.IsInvalid)
                return null;
            var buffer = new char[512];
            var length = GetFinalPathNameByHandle(handle, buffer, (uint)buffer.Length, 0); // FILE_NAME_NORMALIZED | VOLUME_NAME_DOS
            if (length >= buffer.Length) // too small: length is the size needed, terminator included
            {
                buffer = new char[length];
                length = GetFinalPathNameByHandle(handle, buffer, (uint)buffer.Length, 0);
            }
            if (length == 0 || length >= buffer.Length)
                return null;
            var final = new string(buffer, 0, (int)length);
            // \\?\C:\x → C:\x; \\?\UNC\server\share\x (a junction onto a share) → \\server\share\x.
            if (final.StartsWith(@"\\?\UNC\", StringComparison.OrdinalIgnoreCase))
                final = @"\\" + final.Substring(8);
            else if (final.StartsWith(@"\\?\", StringComparison.Ordinal))
                final = final.Substring(4);
            return final.Replace('\\', '/');
        }
    }
}
