// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using EpicGames.Core;
using EpicGames.Horde.Storage;
using Microsoft.Extensions.Logging;

namespace Horde.Agent.Commands.Bundles
{
	[Command("bundle", "dump", "Dumps the contents of a bundle")]
	internal class DumpCommand : BundleCommandBase
	{
		[CommandLine("-Ref=")]
		public RefName RefName { get; set; } = DefaultRefName;

		[CommandLine("-Blob=")]
		public BlobLocator? BlobId { get; set; }

		public override async Task<int> ExecuteAsync(ILogger logger)
		{
			using IStorageClientOwner storeOwner = CreateStorageClient(logger);
			IStorageClient store = storeOwner.Store;

			if (BlobId == null)
			{
				NodeLocator locator = await store.ReadRefTargetAsync(RefName);
				BlobId = locator.Blob;
			}

			logger.LogInformation("Summary for blob {BlobId}", BlobId.Value);
			Bundle bundle = await store.ReadBundleAsync(BlobId.Value);

			BundleHeader header = bundle.Header;
			int packetStart = 0;

			logger.LogInformation("");
			logger.LogInformation("Imports: {NumImports}", header.Imports.Count);

			int refIdx = 0;
			foreach (BundleImport import in header.Imports)
			{
				logger.LogInformation("  From blob {BlobId} ({NumExports} nodes)", import.Locator, import.Exports.Count);
				foreach (int exportIdx in import.Exports)
				{
					logger.LogInformation("    [{Index}] IMP {BlobId}:{ExportIdx}", refIdx, import.Locator, exportIdx);
					refIdx++;
				}
			}

			logger.LogInformation("");
			logger.LogInformation("Exports: {NumExports}", header.Exports.Count);

			int packetIdx = 0;
			int packetOffset = 0;
			foreach (BundleExport export in header.Exports)
			{
				string refs = (export.References.Count == 0) ? "[ ]" : $"[ {String.Join(", ", export.References.Select(x => x.ToString()))} ]";
				logger.LogInformation("  [{Index}] EXP {ExportHash} (length: {NumBytes:n0}, packet: {PacketIdx}, refs: {Refs})", refIdx, export.Hash, export.Length, packetIdx, refs);
				refIdx++;

				packetOffset += export.Length;
				if(packetOffset >= header.Packets[packetIdx].DecodedLength)
				{
					packetIdx++;
					packetOffset = 0;
				}
			}

			logger.LogInformation("");
			logger.LogInformation("Packets: {NumPackets}", header.Packets.Count);
			for (int idx = 0; idx < header.Packets.Count; idx++)
			{
				BundlePacket packet = header.Packets[idx];
				logger.LogInformation("  PKT {Idx} (file offset: {Offset:n0}, encoded: {EncodedLength:n0}, decoded: {DecodedLength:n0}, ratio: {Ratio}%)", idx, packetStart, packet.EncodedLength, packet.DecodedLength, (int)(packet.EncodedLength * 100) / packet.DecodedLength);
				packetStart += packet.EncodedLength;
			}

			return 0;
		}
	}
}
