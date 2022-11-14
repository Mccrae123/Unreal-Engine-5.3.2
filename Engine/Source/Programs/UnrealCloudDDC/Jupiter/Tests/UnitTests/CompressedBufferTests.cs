// Copyright Epic Games, Inc. All Rights Reserved.

using System.Text;
using Jupiter.Implementation;
using Jupiter.Utils;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using OpenTelemetry.Trace;

namespace Jupiter.Tests.Unit
{
    [TestClass]
    public class CompressedBufferTests
    {

        [TestMethod]
        public void CompressAndDecompress()
        {
            byte[] bytes = Encoding.UTF8.GetBytes("this is a test string");

            using OodleCompressor compressor = new OodleCompressor();
            compressor.InitializeOodle();
            CompressedBufferUtils bufferUtils = new CompressedBufferUtils(compressor, TracerProvider.Default.GetTracer("TestTracer"));

            byte[] compressedBytes = bufferUtils.CompressContent(OoodleCompressorMethod.Mermaid, OoodleCompressionLevel.VeryFast, bytes);

            byte[] roundTrippedBytes = bufferUtils.DecompressContent(compressedBytes);

            CollectionAssert.AreEqual(bytes, roundTrippedBytes);
        }
    }
}
