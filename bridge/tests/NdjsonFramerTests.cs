/*
┌───────────────────────────────────────────────────────────────────┐
│  Author: Ivan Murzak (https://github.com/IvanMurzak)              │
│  Repository: GitHub (https://github.com/IvanMurzak/Unreal-MCP)    │
│  Copyright (c) 2026 Ivan Murzak                                   │
│  Licensed under the Apache License, Version 2.0.                  │
│  See the LICENSE file in the project root for more information.   │
└───────────────────────────────────────────────────────────────────┘
*/

using System.IO;
using System.Text;
using com.IvanMurzak.Unreal.MCP.Bridge.Ipc;
using Xunit;

namespace com.IvanMurzak.Unreal.MCP.Bridge.Tests
{
    /// <summary>The "framing codec" xUnit target (docs/ARCHITECTURE.md §9.3).</summary>
    public class NdjsonFramerTests
    {
        [Fact]
        public void Encode_AppendsSingleNewline()
        {
            var framed = NdjsonFramer.Encode("{\"type\":\"ping\"}");
            Assert.Equal((byte)'\n', framed[^1]);
            Assert.Equal("{\"type\":\"ping\"}", Encoding.UTF8.GetString(framed, 0, framed.Length - 1));
        }

        [Fact]
        public void Encode_RejectsOversizeLine()
        {
            var huge = new string('x', 32);
            Assert.Throws<InvalidDataException>(() => NdjsonFramer.Encode(huge, maxLineBytes: 8));
        }

        [Fact]
        public void Push_SplitsMultipleLinesInOneChunk()
        {
            var framer = new NdjsonFramer();
            var lines = framer.Push(Encoding.UTF8.GetBytes("a\nb\nc\n"));
            Assert.Equal(new[] { "a", "b", "c" }, lines);
        }

        [Fact]
        public void Push_BuffersPartialLineAcrossChunks()
        {
            var framer = new NdjsonFramer();
            Assert.Empty(framer.Push(Encoding.UTF8.GetBytes("{\"par")));
            Assert.Empty(framer.Push(Encoding.UTF8.GetBytes("tial\":1")));
            var lines = framer.Push(Encoding.UTF8.GetBytes("}\n"));
            Assert.Equal(new[] { "{\"partial\":1}" }, lines);
        }

        [Fact]
        public void Push_ToleratesCarriageReturn()
        {
            var framer = new NdjsonFramer();
            var lines = framer.Push(Encoding.UTF8.GetBytes("hello\r\n"));
            Assert.Equal(new[] { "hello" }, lines);
        }

        [Fact]
        public void Push_AbortsWhenLineExceedsCap()
        {
            var framer = new NdjsonFramer(maxLineBytes: 4);
            Assert.Throws<InvalidDataException>(() => framer.Push(Encoding.UTF8.GetBytes("abcdefgh")));
        }

        [Fact]
        public void RoundTrip_EncodeThenPush_YieldsOriginal()
        {
            var framer = new NdjsonFramer();
            var framed = NdjsonFramer.Encode("{\"type\":\"tool-call\",\"requestId\":\"x\"}");
            var lines = framer.Push(framed);
            Assert.Single(lines);
            Assert.Equal("{\"type\":\"tool-call\",\"requestId\":\"x\"}", lines[0]);
        }
    }
}
