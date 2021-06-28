// Copyright Epic Games, Inc. All Rights Reserved.

using EpicGames.Core;
using HordeServer.Utilities;
using Microsoft.Extensions.Options;
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading.Tasks;

namespace HordeServer.Storage.Collections
{
	using NamespaceId = StringId<INamespace>;

	class BlobCollection : IBlobCollection
	{
		/// <summary>
		/// The inner storage provider
		/// </summary>
		IStorageBackend StorageBackend;

		/// <summary>
		/// Constructor
		/// </summary>
		/// <param name="StorageBackend"></param>
		public BlobCollection(IStorageBackend StorageBackend)
		{
			this.StorageBackend = StorageBackend;
		}

		/// <inheritdoc/>
		public Task<Stream?> ReadAsync(NamespaceId NamespaceId, IoHash Hash)
		{
			string Path = GetPath(NamespaceId, Hash);
			return StorageBackend.ReadAsync(Path);
		}

		/// <inheritdoc/>
		public Task WriteAsync(NamespaceId NamespaceId, IoHash Hash, Stream Stream)
		{
			string Path = GetPath(NamespaceId, Hash);
			return StorageBackend.WriteAsync(Path, Stream);
		}

		/// <inheritdoc/>
		public async Task<List<IoHash>> ExistsAsync(NamespaceId NamespaceId, IEnumerable<IoHash> Hashes)
		{
			List<Task<IoHash?>> Tasks = new List<Task<IoHash?>>();
			foreach (IoHash Hash in Hashes)
			{
				string Path = GetPath(NamespaceId, Hash);
				Tasks.Add(Task.Run(async () => await StorageBackend.ExistsAsync(Path)? (IoHash?)Hash : null));
			}

			await Task.WhenAll(Tasks);

			return Tasks.Where(x => x.Result != null).Select(x => (IoHash)x.Result!).ToList();
		}

		/// <summary>
		/// Gets the path for a blob
		/// </summary>
		/// <param name="NsId">The namespace id</param>
		/// <param name="Hash">Hash of the blob</param>
		/// <returns></returns>
		static string GetPath(NamespaceId NsId, IoHash Hash)
		{
			string HashText = Hash.ToString();
			return $"blobs/{NsId}/{HashText[0..2]}/{HashText[2..4]}/{HashText}.blob";
		}
	}
}
