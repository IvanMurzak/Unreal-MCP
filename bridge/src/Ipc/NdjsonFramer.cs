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
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Ipc
{
    /// <summary>
    /// Pure NDJSON framing codec (docs/ARCHITECTURE.md §1.2): UTF-8 JSON, one message per
    /// <c>\n</c>-terminated line, line length capped at <see cref="IpcProtocol.MaxLineBytes"/>.
    /// Kept free of any socket dependency so the framing path is unit-tested directly (the
    /// "framing codec" xUnit row in §9.3). The decode side is a stateful accumulator: feed it raw
    /// byte chunks as they arrive off the wire and it yields complete UTF-8 lines (newline stripped),
    /// buffering partial lines across chunk boundaries.
    /// </summary>
    public sealed class NdjsonFramer
    {
        private readonly MemoryStream _buffer = new();
        private readonly int _maxLineBytes;

        public NdjsonFramer(int maxLineBytes = IpcProtocol.MaxLineBytes)
        {
            if (maxLineBytes <= 0)
                throw new ArgumentOutOfRangeException(nameof(maxLineBytes));
            _maxLineBytes = maxLineBytes;
        }

        /// <summary>
        /// Encode a single JSON message into its wire form: UTF-8 bytes followed by a single
        /// <c>\n</c>. The caller guarantees <paramref name="jsonLine"/> contains no raw newline
        /// (System.Text.Json escapes them inside strings, so a serialized object never does).
        /// </summary>
        /// <exception cref="ArgumentNullException"><paramref name="jsonLine"/> is null.</exception>
        /// <exception cref="InvalidDataException">The encoded line exceeds the cap.</exception>
        public static byte[] Encode(string jsonLine, int maxLineBytes = IpcProtocol.MaxLineBytes)
        {
            if (jsonLine == null)
                throw new ArgumentNullException(nameof(jsonLine));

            var payload = Encoding.UTF8.GetBytes(jsonLine);
            if (payload.Length > maxLineBytes)
                throw new InvalidDataException(
                    $"Outgoing IPC line is {payload.Length} bytes, exceeding the {maxLineBytes}-byte cap.");

            var framed = new byte[payload.Length + 1];
            Buffer.BlockCopy(payload, 0, framed, 0, payload.Length);
            framed[payload.Length] = (byte)'\n';
            return framed;
        }

        /// <summary>
        /// Feed a chunk of raw bytes (as read from the socket) and return every complete line it
        /// completes, in order. A trailing partial line is retained for the next call. Lines are
        /// returned with the terminating <c>\n</c> (and a tolerated trailing <c>\r</c>) stripped.
        /// </summary>
        /// <exception cref="InvalidDataException">
        /// A single un-terminated line grows past the cap — the connection must be aborted (§1.2).
        /// </exception>
        public IReadOnlyList<string> Push(ReadOnlySpan<byte> chunk)
        {
            var lines = new List<string>();
            foreach (var b in chunk)
            {
                if (b == (byte)'\n')
                {
                    lines.Add(DrainLine());
                }
                else
                {
                    _buffer.WriteByte(b);
                    if (_buffer.Length > _maxLineBytes)
                        throw new InvalidDataException(
                            $"Incoming IPC line exceeded the {_maxLineBytes}-byte cap before a newline arrived; aborting connection.");
                }
            }
            return lines;
        }

        private string DrainLine()
        {
            var bytes = _buffer.ToArray();
            _buffer.SetLength(0);

            // Tolerate CRLF: strip a trailing '\r' so a peer that writes "\r\n" still decodes cleanly.
            var len = bytes.Length;
            if (len > 0 && bytes[len - 1] == (byte)'\r')
                len--;

            return Encoding.UTF8.GetString(bytes, 0, len);
        }
    }
}
