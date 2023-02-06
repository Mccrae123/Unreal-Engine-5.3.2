// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using EpicGames.Core;
using EpicGames.Horde.Storage;
using EpicGames.Horde.Storage.Nodes;
using Horde.Build.Acls;
using Horde.Build.Server;
using Horde.Build.Storage;
using Horde.Build.Utilities;
using Microsoft.AspNetCore.Authorization;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.Mvc;
using Microsoft.Extensions.Options;

namespace Horde.Build.Artifacts
{
	/// <summary>
	/// Describes an artifact
	/// </summary>
	public class GetArtifactResponse
	{
		readonly IArtifact _artifact;

		/// <inheritdoc cref="IArtifact.Id"/>
		public string Id => _artifact.Id.ToString();

		/// <inheritdoc cref="IArtifact.Name"/>
		public string Name => _artifact.Name;

		/// <inheritdoc cref="IArtifact.Type"/>
		public ArtifactType Type => _artifact.Type;

		/// <inheritdoc cref="IArtifact.Keys"/>
		public IReadOnlyList<string> Keys => _artifact.Keys;

		internal GetArtifactResponse(IArtifact artifact) => _artifact = artifact;
	}

	/// <summary>
	/// Result of an artifact search
	/// </summary>
	public class FindArtifactsResponse
	{
		/// <summary>
		/// List of artifacts matching the search criteria
		/// </summary>
		public List<GetArtifactResponse> Artifacts { get; } = new List<GetArtifactResponse>();
	}

	/// <summary>
	/// Describes a file within an artifact
	/// </summary>
	public class GetArtifactFileEntryResponse
	{
		readonly FileEntry _entry;

		/// <inheritdoc cref="FileEntry.Name"/>
		public string Name => _entry.Name.ToString();

		/// <inheritdoc cref="FileEntry.Length"/>
		public long Length => _entry.Length;

		/// <inheritdoc cref="FileEntry.Hash"/>
		public IoHash Hash => _entry.Hash;

		internal GetArtifactFileEntryResponse(FileEntry entry) => _entry = entry;
	}

	/// <summary>
	/// Describes a file within an artifact
	/// </summary>
	public class GetArtifactDirectoryEntryResponse
	{
		readonly DirectoryEntry _entry;

		/// <inheritdoc cref="FileEntry.Name"/>
		public string Name => _entry.Name.ToString();

		/// <inheritdoc cref="FileEntry.Length"/>
		public long Length => _entry.Length;

		/// <inheritdoc cref="FileEntry.Hash"/>
		public IoHash Hash => _entry.Hash;

		internal GetArtifactDirectoryEntryResponse(DirectoryEntry entry) => _entry = entry;
	}

	/// <summary>
	/// Describes a directory within an artifact
	/// </summary>
	public class GetArtifactDirectoryResponse
	{
		/// <summary>
		/// Names of sub-directories
		/// </summary>
		public List<GetArtifactDirectoryEntryResponse> Directories { get; } = new List<GetArtifactDirectoryEntryResponse>();

		/// <summary>
		/// Files within the directory
		/// </summary>
		public List<GetArtifactFileEntryResponse> Files { get; } = new List<GetArtifactFileEntryResponse>();

		internal GetArtifactDirectoryResponse(DirectoryNode directory)
		{
			Directories.AddRange(directory.Directories.Select(x => new GetArtifactDirectoryEntryResponse(x)));
			Files.AddRange(directory.Files.Select(x => new GetArtifactFileEntryResponse(x)));
		}
	}

	/// <summary>
	/// Public interface for artifacts
	/// </summary>
	[Authorize]
	[ApiController]
	public class ArtifactsController : HordeControllerBase
	{
		readonly IArtifactCollection _artifactCollection;
		readonly StorageService _storageService;
		readonly GlobalConfig _globalConfig;

		/// <summary>
		/// Constructor
		/// </summary>
		public ArtifactsController(IArtifactCollection artifactCollection, StorageService storageService, IOptionsSnapshot<GlobalConfig> globalConfig)
		{
			_artifactCollection = artifactCollection;
			_storageService = storageService;
			_globalConfig = globalConfig.Value;
		}

		/// <summary>
		/// Gets metadata about an artifact object
		/// </summary>
		/// <param name="id">Identifier of the artifact to retrieve</param>
		/// <param name="filter">Filter for returned properties</param>
		/// <returns>Information about all the artifacts</returns>
		[HttpGet]
		[Route("/api/v2/artifacts/{id}")]
		[ProducesResponseType(typeof(FindArtifactsResponse), 200)]
		public async Task<ActionResult<object>> GetArtifactAsync(ArtifactId id, [FromQuery] PropertyFilter? filter = null)
		{
			IArtifact? artifact = await _artifactCollection.GetAsync(id, HttpContext.RequestAborted);
			if (artifact == null)
			{
				return NotFound(id);
			}
			if (!_globalConfig.Authorize(artifact.AclScope, AclAction.ReadArtifact, User))
			{
				return Forbid(AclAction.ReadArtifact, artifact.AclScope);
			}

			return PropertyFilter.Apply(new GetArtifactResponse(artifact), filter);
		}

		/// <summary>
		/// Gets metadata about an artifact object
		/// </summary>
		/// <param name="id">Identifier of the artifact to retrieve</param>
		/// <param name="path">Path to fetch</param>
		/// <param name="filter">Filter for returned properties</param>
		/// <param name="cancellationToken">Cancellation token for the operation</param>
		/// <returns>Information about all the artifacts</returns>
		[HttpGet]
		[Route("/api/v2/artifacts/{id}/browse")]
		[ProducesResponseType(typeof(GetArtifactDirectoryResponse), 200)]
		public async Task<ActionResult<object>> BrowseArtifactAsync(ArtifactId id, [FromQuery] string? path = null, [FromQuery] PropertyFilter? filter = null, CancellationToken cancellationToken = default)
		{
			IArtifact? artifact = await _artifactCollection.GetAsync(id, cancellationToken);
			if (artifact == null)
			{
				return NotFound(id);
			}
			if (!_globalConfig.Authorize(artifact.AclScope, AclAction.ReadArtifact, User))
			{
				return Forbid(AclAction.ReadArtifact, artifact.AclScope);
			}

			TreeReader reader = await _storageService.GetReaderAsync(artifact.NamespaceId, cancellationToken);
			TreeNodeRef<DirectoryNode> directoryRef = await reader.ReadNodeRefAsync<DirectoryNode>(artifact.RefName, DateTime.UtcNow.AddHours(1.0), cancellationToken);

			if (path != null)
			{
				foreach (string fragment in path.Split('/'))
				{
					DirectoryNode nextDirectory = await directoryRef.ExpandAsync(reader, cancellationToken);
					if (!nextDirectory.TryGetDirectoryEntry(fragment, out DirectoryEntry? nextDirectoryEntry))
					{
						return NotFound();
					}
					directoryRef = nextDirectoryEntry;
				}
			}

			DirectoryNode directory = await directoryRef.ExpandAsync(reader, cancellationToken);
			return PropertyFilter.Apply(new GetArtifactDirectoryResponse(directory), filter);
		}

		/// <summary>
		/// Gets metadata about an artifact object
		/// </summary>
		/// <param name="ids">Artifact ids to return</param>
		/// <param name="keys">Keys to find</param>
		/// <param name="filter">Filter for returned values</param>
		/// <returns>Information about all the artifacts</returns>
		[HttpGet]
		[Route("/api/v2/artifacts")]
		[ProducesResponseType(typeof(FindArtifactsResponse), 200)]
		public async Task<ActionResult<object>> FindArtifactsAsync([FromQuery(Name = "id")] IEnumerable<ArtifactId>? ids = null, [FromQuery(Name = "key")] IEnumerable<string>? keys = null, [FromQuery] PropertyFilter? filter = null)
		{
			if ((ids == null || !ids.Any()) && (keys == null || !keys.Any()))
			{
				return BadRequest("At least one search term must be specified");
			}

			FindArtifactsResponse response = new FindArtifactsResponse();
			await foreach (IArtifact artifact in _artifactCollection.FindAsync(ids, keys, HttpContext.RequestAborted))
			{
				if (_globalConfig.Authorize(artifact.AclScope, AclAction.ReadArtifact, User))
				{
					response.Artifacts.Add(new GetArtifactResponse(artifact));
				}
			}

			return PropertyFilter.Apply(response, filter);
		}
	}
}
