// Copyright Epic Games, Inc. All Rights Reserved.

using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using EpicGames.Horde.Storage;
using Horde.Build.Acls;
using Horde.Build.Utilities;

namespace Horde.Build.Artifacts
{
	/// <summary>
	/// Interface for a collection of artifacts
	/// </summary>
	public interface IArtifactCollection
	{
		/// <summary>
		/// Creates a new artifact
		/// </summary>
		/// <param name="name">Description for the artifact</param>
		/// <param name="type">Type identifier for the artifact</param>
		/// <param name="keys">Keys for the artifact</param>
		/// <param name="namespaceId">Namespace containing the data</param>
		/// <param name="refPrefix">Prefix for the ref name. Object id will be appended to this string.</param>
		/// <param name="scopeName">Inherited scope used for permissions</param>
		/// <param name="cancellationToken">Cancellation token for the operation</param>
		/// <returns>The new log file document</returns>
		Task<IArtifact> AddAsync(string name, ArtifactType type, IEnumerable<string> keys, NamespaceId namespaceId, string? refPrefix, AclScopeName scopeName, CancellationToken cancellationToken = default);

		/// <summary>
		/// Finds artifacts with the given keys
		/// </summary>
		/// <param name="ids">Ids to search for</param>
		/// <param name="keys">Keys to search for</param>
		/// <param name="cancellationToken">Cancellation token for the operation</param>
		/// <returns>Sequence of artifacts</returns>
		IAsyncEnumerable<IArtifact> FindAsync(IEnumerable<ArtifactId>? ids = null, IEnumerable<string>? keys = null, CancellationToken cancellationToken = default);

		/// <summary>
		/// Gets an artifact by ID
		/// </summary>
		/// <param name="artifactId">Unique id of the artifact</param>
		/// <param name="cancellationToken">Cancellation token for the operation</param>
		/// <returns>The artifact document</returns>
		Task<IArtifact?> GetAsync(ArtifactId artifactId, CancellationToken cancellationToken = default);
	}
}
