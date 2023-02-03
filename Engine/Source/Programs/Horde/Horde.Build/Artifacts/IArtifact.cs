// Copyright Epic Games, Inc. All Rights Reserved.

using System.Collections.Generic;
using EpicGames.Horde.Storage;
using Horde.Build.Acls;

namespace Horde.Build.Artifacts
{
	/// <summary>
	/// Information about an artifact
	/// </summary>
	public interface IArtifact
	{
		/// <summary>
		/// Identifier for the Artifact. Randomly generated.
		/// </summary>
		public ArtifactId Id { get; }

		/// <summary>
		/// Name for this artifact object.
		/// </summary>
		public string Name { get; }

		/// <summary>
		/// Type of artifact
		/// </summary>
		public ArtifactType Type { get; }

		/// <summary>
		/// Keys used to collate artifacts
		/// </summary>
		public IReadOnlyList<string> Keys { get; }

		/// <summary>
		/// Storage namespace containing the data
		/// </summary>
		public NamespaceId NamespaceId { get; }

		/// <summary>
		/// Name of the ref containing the root data object
		/// </summary>
		public RefName RefName { get; }

		/// <summary>
		/// Permissions scope for this object
		/// </summary>
		public AclScopeName AclScope { get; }
	}
}
