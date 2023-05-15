// Copyright Epic Games, Inc. All Rights Reserved.

using System.Diagnostics;
using EpicGames.Core;
using EpicGames.Horde.Storage;
using EpicGames.Horde.Storage.Backends;
using EpicGames.Horde.Storage.Nodes;
using Microsoft.Extensions.Caching.Memory;
using Microsoft.Extensions.Logging;

namespace Horde.Commands.Bundles
{
	[Command("bundle", "extract", "Extracts data from a bundle to the local hard drive")]
	internal class BundleExtract : StorageCommandBase
	{
		[CommandLine("-File=")]
		public FileReference? File { get; set; }

		[CommandLine("-Ref=")]
		public string? Ref { get; set; }

		[CommandLine("-OutputDir=", Required = true)]
		public DirectoryReference OutputDir { get; set; } = null!;

		public override async Task<int> ExecuteAsync(ILogger logger)
		{
			IStorageClient store;
			NodeHandle handle;
			if (File != null)
			{
				store = new FileStorageClient(File.Directory, logger);
				handle = NodeHandle.Parse(await FileReference.ReadAllTextAsync(File));
			}
			else if (Ref != null)
			{
				store = await CreateStorageClientAsync(logger);
				handle = await store.ReadRefTargetAsync(new RefName(Ref));
			}
			else
			{
				throw new CommandLineArgumentException("Either -File=... or -Ref=... must be specified");
			}

			using MemoryCache cache = new MemoryCache(new MemoryCacheOptions());
			TreeReader reader = new TreeReader(store, cache, logger);

			Stopwatch timer = Stopwatch.StartNew();

			DirectoryNode node = await reader.ReadNodeAsync<DirectoryNode>(handle.Locator);
			await node.CopyToDirectoryAsync(reader, OutputDir.ToDirectoryInfo(), logger, CancellationToken.None);

			logger.LogInformation("Elapsed: {Time}s", timer.Elapsed.TotalSeconds);
			return 0;
		}
	}
}
