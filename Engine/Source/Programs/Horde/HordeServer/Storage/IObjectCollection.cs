// Copyright Epic Games, Inc. All Rights Reserved.

using EpicGames.Core;
using EpicGames.Serialization;
using HordeCommon;
using HordeServer.Models;
using HordeServer.Services;
using HordeServer.Utilities;
using MongoDB.Bson;
using MongoDB.Bson.Serialization.Attributes;
using MongoDB.Driver;
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text.Json;
using System.Threading.Tasks;

namespace HordeServer.Storage
{
	using NamespaceId = StringId<INamespace>;

	/// <summary>
	/// Interface for a collection of objects
	/// </summary>
	public interface IObjectCollection
	{
		/// <summary>
		/// Adds an item to storage
		/// </summary>
		/// <param name="NsId">The namespace id</param>
		/// <param name="Hash">Hash of the object</param>
		/// <param name="Data">The object data</param>
		Task AddAsync(NamespaceId NsId, IoHash Hash, CbObject Data);

		/// <summary>
		/// Reads an object from storage
		/// </summary>
		/// <param name="NsId">The namespace id</param>
		/// <param name="Hash">Hash of the object</param>
		/// <returns>Stream for the blob, or null if it does not exist</returns>
		Task<CbObject?> GetAsync(NamespaceId NsId, IoHash Hash);

		/// <summary>
		/// Determines which of a set of blobs exist
		/// </summary>
		/// <param name="NsId">The namespace id</param>
		/// <param name="Hashes"></param>
		/// <returns></returns>
		Task<List<IoHash>> ExistsAsync(NamespaceId NsId, IEnumerable<IoHash> Hashes);
	}

	/// <summary>
	/// Extension methods for <see cref="IObjectCollection"/>
	/// </summary>
	public static class ObjectCollectionExtensions
	{
		/// <summary>
		/// Test whether a single blob exists
		/// </summary>
		/// <param name="Collection"></param>
		/// <param name="NamespaceId"></param>
		/// <param name="Hash"></param>
		/// <returns></returns>
		public static async Task<bool> ExistsAsync(this IObjectCollection Collection, NamespaceId NamespaceId, IoHash Hash)
		{
			List<IoHash> Hashes = await Collection.ExistsAsync(NamespaceId, new[] { Hash });
			return Hashes.Count > 0;
		}
	}
}
