// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using Amazon.EC2.Model;
using EpicGames.Core;
using EpicGames.Horde.Storage;
using Horde.Build.Acls;
using Horde.Build.Utilities;
using Microsoft.AspNetCore.Authorization;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.Mvc;
using Microsoft.Extensions.Primitives;

namespace Horde.Build.Storage
{
	/// <summary>
	/// Response from uploading a bundle
	/// </summary>
	public class WriteBlobResponse
	{
		/// <summary>
		/// Locator for the uploaded bundle
		/// </summary>
		public BlobLocator Locator { get; set; }

		/// <summary>
		/// URL to upload the blob to.
		/// </summary>
		public Uri? UploadUrl { get; set; }

		/// <summary>
		/// Flag for whether the client could use a redirect instead (ie. not post content to the server, and get an upload url back).
		/// </summary>
		public bool? SupportsRedirects { get; set; }
	}

	/// <summary>
	/// Request object for writing a ref
	/// </summary>
	public class WriteRefRequest
	{
		/// <summary>
		/// Locator for the target blob
		/// </summary>
		public BlobLocator Locator { get; set; }

		/// <summary>
		/// Export index for the ref
		/// </summary>
		public int ExportIdx { get; set; }

		/// <summary>
		/// Options for the ref
		/// </summary>
		public RefOptions? Options { get; set; }
	}

	/// <summary>
	/// Response object for reading a ref
	/// </summary>
	public class ReadRefResponse
	{
		/// <summary>
		/// Locator for the target blob
		/// </summary>
		public BlobLocator Blob { get; set; }

		/// <summary>
		/// Export index for the ref
		/// </summary>
		public int ExportIdx { get; set; }

		/// <summary>
		/// Constructor
		/// </summary>
		public ReadRefResponse(NodeLocator target)
		{
			Blob = target.Blob;
			ExportIdx = target.ExportIdx;
		}
	}

	/// <summary>
	/// Controller for the /api/v1/storage endpoint
	/// </summary>
	[Authorize]
	[ApiController]
	[Route("[controller]")]
	public class StorageController : HordeControllerBase
	{
		readonly StorageService _storageService;

		/// <summary>
		/// Constructor
		/// </summary>
		/// <param name="storageService"></param>
		public StorageController(StorageService storageService)
		{
			_storageService = storageService;
		}

		/// <summary>
		/// Uploads data to the storage service. 
		/// </summary>
		/// <param name="namespaceId">Namespace to fetch from</param>
		/// <param name="file">Data to be uploaded. May be null, in which case the server may return a separate url.</param>
		/// <param name="prefix">Prefix for the uploaded file</param>
		/// <param name="cancellationToken">Cancellation token for the operation</param>
		[HttpPost]
		[Route("/api/v1/storage/{namespaceId}/blobs")]
		public async Task<ActionResult<WriteBlobResponse>> WriteBlobAsync(NamespaceId namespaceId, IFormFile? file, [FromForm] string? prefix = default, CancellationToken cancellationToken = default)
		{
			if (!await _storageService.AuthorizeAsync(namespaceId, User, AclAction.WriteBlobs, null, cancellationToken))
			{
				return Forbid(AclAction.WriteBlobs);
			}

			IStorageClientImpl client = await _storageService.GetClientAsync(namespaceId, cancellationToken);
			if (file == null)
			{
				(BlobLocator Locator, Uri UploadUrl)? result = await client.GetWriteRedirectAsync(prefix ?? String.Empty, cancellationToken);
				if (result == null)
				{
					return new WriteBlobResponse { SupportsRedirects = false };
				}
				else
				{
					return new WriteBlobResponse { Locator = result.Value.Locator, UploadUrl = result.Value.UploadUrl };
				}
			}
			else
			{
				using (Stream stream = file.OpenReadStream())
				{
					BlobLocator locator = await client.WriteBlobAsync(stream, prefix: (prefix == null) ? Utf8String.Empty : new Utf8String(prefix), cancellationToken: cancellationToken);
					return new WriteBlobResponse { Locator = locator, SupportsRedirects = client.SupportsRedirects? (bool?)true : null };
				}
			}
		}

		/// <summary>
		/// Writes a blob to storage. Exposed as a public utility method to allow other routes with their own authentication methods to wrap their own authentication/redirection.
		/// </summary>
		/// <param name="storageService">The storage service</param>
		/// <param name="namespaceId">Namespace to write the blob to</param>
		/// <param name="file">File to be written</param>
		/// <param name="prefix">Prefix for uploaded blobs</param>
		/// <param name="cancellationToken">Cancellation token</param>
		/// <returns>Information about the written blob, or redirect information</returns>
		public static async Task<ActionResult<WriteBlobResponse>> WriteBlobAsync(StorageService storageService, NamespaceId namespaceId, IFormFile? file, [FromForm] string? prefix = default, CancellationToken cancellationToken = default)
		{
			IStorageClientImpl client = await storageService.GetClientAsync(namespaceId, cancellationToken);
			if (file == null)
			{
				(BlobLocator Locator, Uri UploadUrl)? result = await client.GetWriteRedirectAsync(prefix ?? String.Empty, cancellationToken);
				if (result == null)
				{
					return new WriteBlobResponse { SupportsRedirects = false };
				}
				else
				{
					return new WriteBlobResponse { Locator = result.Value.Locator, UploadUrl = result.Value.UploadUrl };
				}
			}
			else
			{
				using (Stream stream = file.OpenReadStream())
				{
					BlobLocator locator = await client.WriteBlobAsync(stream, prefix: (prefix == null) ? Utf8String.Empty : new Utf8String(prefix), cancellationToken: cancellationToken);
					return new WriteBlobResponse { Locator = locator, SupportsRedirects = client.SupportsRedirects ? (bool?)true : null };
				}
			}
		}

		/// <summary>
		/// Retrieves data from the storage service. 
		/// </summary>
		/// <param name="namespaceId">Namespace to fetch from</param>
		/// <param name="locator">Bundle to retrieve</param>
		/// <param name="offset">Offset of the data.</param>
		/// <param name="length">Length of the data to return.</param>
		/// <param name="cancellationToken">Cancellation token for the operation</param>
		[HttpGet]
		[Route("/api/v1/storage/{namespaceId}/blobs/{*locator}")]
		public async Task<ActionResult> ReadBlobAsync(NamespaceId namespaceId, BlobLocator locator, [FromQuery] int? offset = null, [FromQuery] int? length = null, CancellationToken cancellationToken = default)
		{
			if (!await _storageService.AuthorizeAsync(namespaceId, User, AclAction.ReadBlobs, null, cancellationToken))
			{
				return Forbid(AclAction.WriteBlobs);
			}

			IStorageClientImpl client = await _storageService.GetClientAsync(namespaceId, cancellationToken);

			Uri? redirectUrl = await client.GetReadRedirectAsync(locator, cancellationToken);
			if (redirectUrl != null)
			{
				return Redirect(redirectUrl.ToString());
			}

#pragma warning disable CA2000 // Dispose objects before losing scope
			// TODO: would be better to use the range header here, but seems to require a lot of plumbing to convert unseekable AWS streams into a format that works with range processing.
			Stream stream;
			if (offset == null && length == null)
			{
				stream = await client.ReadBlobAsync(locator, cancellationToken);
			}
			else if (offset != null && length != null)
			{
				stream = await client.ReadBlobRangeAsync(locator, offset.Value, length.Value, cancellationToken);
			}
			else
			{
				return BadRequest("Offset and length must both be specified as query parameters for ranged reads");
			}
			return File(stream, "application/octet-stream");
#pragma warning restore CA2000 // Dispose objects before losing scope
		}

		/// <summary>
		/// Writes a ref to the storage service.
		/// </summary>
		/// <param name="namespaceId">Namespace to write to</param>
		/// <param name="refName">Name of the ref</param>
		/// <param name="request">Request for the ref to write</param>
		/// <param name="cancellationToken">Cancellation token for the operation</param>
		[HttpPut]
		[Route("/api/v1/storage/{namespaceId}/refs/{*refName}")]
		public async Task<ActionResult> WriteRefAsync(NamespaceId namespaceId, RefName refName, [FromBody] WriteRefRequest request, CancellationToken cancellationToken)
		{
			if (!await _storageService.AuthorizeAsync(namespaceId, User, AclAction.WriteRefs, null, cancellationToken))
			{
				return Forbid(AclAction.WriteBlobs);
			}

			IStorageClient client = await _storageService.GetClientAsync(namespaceId, cancellationToken);
			NodeLocator target = new NodeLocator(request.Locator, request.ExportIdx);
			await client.WriteRefTargetAsync(refName, target, request.Options, cancellationToken);

			return Ok();
		}

		/// <summary>
		/// Uploads data to the storage service. 
		/// </summary>
		/// <param name="namespaceId"></param>
		/// <param name="refName"></param>
		/// <param name="cancellationToken"></param>
		[HttpGet]
		[Route("/api/v1/storage/{namespaceId}/refs/{*refName}")]
		public async Task<ActionResult<ReadRefResponse>> ReadRefAsync(NamespaceId namespaceId, RefName refName, CancellationToken cancellationToken)
		{
			if (!await _storageService.AuthorizeAsync(namespaceId, User, AclAction.ReadRefs, null, cancellationToken))
			{
				return Forbid(AclAction.ReadRefs);
			}

			IStorageClient client = await _storageService.GetClientAsync(namespaceId, cancellationToken);

			NodeLocator target = await client.TryReadRefTargetAsync(refName, cancellationToken: cancellationToken);
			if (!target.IsValid())
			{
				return NotFound();
			}

			return new ReadRefResponse(target);
		}
	}
}
