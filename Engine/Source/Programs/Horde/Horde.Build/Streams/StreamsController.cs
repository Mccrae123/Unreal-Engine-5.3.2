// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Horde.Build.Acls;
using Horde.Build.Jobs;
using Horde.Build.Jobs.Templates;
using Horde.Build.Perforce;
using Horde.Build.Projects;
using Horde.Build.Users;
using Horde.Build.Utilities;
using Microsoft.AspNetCore.Authorization;
using Microsoft.AspNetCore.Mvc;
using OpenTracing;
using OpenTracing.Util;

namespace Horde.Build.Streams
{
	using ProjectId = StringId<IProject>;
	using StreamId = StringId<IStream>;
	using TemplateId = StringId<ITemplateRef>;

	/// <summary>
	/// Controller for the /api/v1/streams endpoint
	/// </summary>
	[ApiController]
	[Authorize]
	[Route("[controller]")]
	public class StreamsController : HordeControllerBase
	{
		private readonly StreamService _streamService;
		private readonly ICommitService _commitService;
		private readonly ITemplateCollection _templateCollection;
		private readonly IJobStepRefCollection _jobStepRefCollection;
		private readonly IUserCollection _userCollection;
		private readonly AclService _aclService;

		/// <summary>
		/// Constructor
		/// </summary>
		public StreamsController(AclService aclService, StreamService streamService, ICommitService commitService, ITemplateCollection templateCollection, IJobStepRefCollection jobStepRefCollection, IUserCollection userCollection)
		{
			_streamService = streamService;
			_commitService = commitService;
			_templateCollection = templateCollection;
			_jobStepRefCollection = jobStepRefCollection;
			_userCollection = userCollection;
			_aclService = aclService;
		}

		/// <summary>
		/// Query all the streams for a particular project.
		/// </summary>
		/// <param name="projectIds">Unique id of the project to query</param>
		/// <param name="filter">Filter for the properties to return</param>
		/// <returns>Information about all the projects</returns>
		[HttpGet]
		[Route("/api/v1/streams")]
		[ProducesResponseType(typeof(List<GetStreamResponse>), 200)]
		public async Task<ActionResult<List<object>>> GetStreamsAsync([FromQuery(Name = "ProjectId")] string[] projectIds, [FromQuery] PropertyFilter? filter = null)
		{
			ProjectId[] projectIdValues = Array.ConvertAll(projectIds, x => new ProjectId(x));

			List<IStream> streams = await _streamService.GetStreamsAsync(projectIdValues);
			ProjectPermissionsCache permissionsCache = new ProjectPermissionsCache();

			List<GetStreamResponse> responses = new List<GetStreamResponse>();
			foreach (IStream stream in streams)
			{
				if (await _streamService.AuthorizeAsync(stream, AclAction.ViewStream, User, permissionsCache))
				{
					GetStreamResponse response = await CreateGetStreamResponse(stream, permissionsCache);
					responses.Add(response);
				}
			}
			return responses.OrderBy(x => x.Order).ThenBy(x => x.Name).Select(x => PropertyFilter.Apply(x, filter)).ToList();
		}

		/// <summary>
		/// Query all the streams for a particular project.
		/// </summary>
		/// <param name="projectIds">Unique id of the project to query</param>
		/// <param name="filter">Filter for the properties to return</param>
		/// <returns>Information about all the projects</returns>
		[HttpGet]
		[Route("/api/v2/streams")]
		[ProducesResponseType(typeof(List<GetStreamResponse>), 200)]
		public async Task<ActionResult<List<object>>> GetStreamsAsyncV2([FromQuery(Name = "ProjectId")] string[] projectIds, [FromQuery] PropertyFilter? filter = null)
		{
			ProjectId[] projectIdValues = Array.ConvertAll(projectIds, x => new ProjectId(x));

			List<IStream> streams = await _streamService.GetStreamsAsync(projectIdValues);
			ProjectPermissionsCache permissionsCache = new ProjectPermissionsCache();

			List<GetStreamResponseV2> responses = new List<GetStreamResponseV2>();
			foreach (IStream stream in streams)
			{
				if (await _streamService.AuthorizeAsync(stream, AclAction.ViewStream, User, permissionsCache))
				{
					GetStreamResponseV2 response = new GetStreamResponseV2(stream, false);
					responses.Add(response);
				}
			}
			return responses.OrderBy(x => x.Id).Select(x => PropertyFilter.Apply(x, filter)).ToList();
		}

		/// <summary>
		/// Retrieve information about a specific stream.
		/// </summary>
		/// <param name="streamId">Id of the stream to get information about</param>
		/// <param name="filter">Filter for the properties to return</param>
		/// <returns>Information about the requested project</returns>
		[HttpGet]
		[Route("/api/v1/streams/{streamId}")]
		[ProducesResponseType(typeof(GetStreamResponse), 200)]
		public async Task<ActionResult<object>> GetStreamAsync(StreamId streamId, [FromQuery] PropertyFilter? filter = null)
		{
			IStream? stream = await _streamService.GetStreamAsync(streamId);
			if (stream == null)
			{
				return NotFound(streamId);
			}

			ProjectPermissionsCache permissionsCache = new ProjectPermissionsCache();
			if (!await _streamService.AuthorizeAsync(stream, AclAction.ViewStream, User, permissionsCache))
			{
				return Forbid(AclAction.ViewStream, streamId);
			}

			return PropertyFilter.Apply(await CreateGetStreamResponse(stream, permissionsCache), filter);
		}

		/// <summary>
		/// Retrieve information about a specific stream.
		/// </summary>
		/// <param name="streamId">Id of the stream to get information about</param>
		/// <param name="config">The client's cached config revision. If this matches value matches the current config version of the stream, the config object will be omitted from the response.</param>
		/// <param name="filter">Filter for the properties to return</param>
		/// <returns>Information about the requested project</returns>
		[HttpGet]
		[Route("/api/v2/streams/{streamId}/config")]
		[ProducesResponseType(typeof(GetStreamResponseV2), 200)]
		public async Task<ActionResult<object>> GetStreamAsyncV2(StreamId streamId, [FromQuery] string? config = null, [FromQuery] PropertyFilter? filter = null)
		{
			IStream? stream = await _streamService.GetStreamAsync(streamId);
			if (stream == null)
			{
				return NotFound(streamId);
			}

			ProjectPermissionsCache permissionsCache = new ProjectPermissionsCache();
			if (!await _streamService.AuthorizeAsync(stream, AclAction.ViewStream, User, permissionsCache))
			{
				return Forbid(AclAction.ViewStream, streamId);
			}

			bool includeConfig = !String.Equals(config, stream.ConfigRevision, StringComparison.Ordinal); 
			return PropertyFilter.Apply(new GetStreamResponseV2(stream, includeConfig), filter);
		}

		/// <summary>
		/// Create a stream response object, including all the templates
		/// </summary>
		/// <param name="stream">Stream to create response for</param>
		/// <param name="cache">Permissions cache</param>
		/// <returns>Response object</returns>
		async Task<GetStreamResponse> CreateGetStreamResponse(IStream stream, ProjectPermissionsCache cache)
		{
			using IScope scope = GlobalTracer.Instance.BuildSpan("CreateGetStreamResponse").StartActive();
			scope.Span.SetTag("streamId", stream.Id);
			
			bool includeAcl = stream.Acl != null && await _streamService.AuthorizeAsync(stream, AclAction.ViewPermissions, User, cache);

			List<GetTemplateRefResponse> apiTemplateRefs = new List<GetTemplateRefResponse>();
			foreach (KeyValuePair<TemplateId, ITemplateRef> pair in stream.Templates)
			{
				using IScope templateScope = GlobalTracer.Instance.BuildSpan("CreateGetStreamResponse.Template").StartActive();
				templateScope.Span.SetTag("templateName", pair.Value.Config.Name);
				
				if (await _streamService.AuthorizeAsync(stream, pair.Value, AclAction.ViewTemplate, User, cache))
				{
					ITemplate? template = await _templateCollection.GetAsync(pair.Value.Hash);
					
					if (template != null)						
					{
						ITemplateRef tref = pair.Value;

						List<GetTemplateStepStateResponse>? stepStates = null;
						if (tref.StepStates != null)
						{
							for (int i = 0; i < tref.StepStates.Count; i++)
							{
								ITemplateStep state = tref.StepStates[i];

								stepStates ??= new List<GetTemplateStepStateResponse>();

								GetThinUserInfoResponse? pausedByUserInfo = new GetThinUserInfoResponse(await _userCollection.GetCachedUserAsync(state.PausedByUserId));
								stepStates.Add(new GetTemplateStepStateResponse(state, pausedByUserInfo));
							}
						}

						bool includeTemplateAcl = pair.Value.Acl != null && await _streamService.AuthorizeAsync(stream, pair.Value, AclAction.ViewPermissions, User, cache);
						apiTemplateRefs.Add(new GetTemplateRefResponse(pair.Key, pair.Value, template, stepStates, includeTemplateAcl));
					}
				}
			}

			return stream.ToApiResponse(includeAcl, apiTemplateRefs);
		}

		/// <summary>
		/// Gets a list of changes for a stream
		/// </summary>
		/// <param name="streamId">The stream id</param>
		/// <param name="min">The starting changelist number</param>
		/// <param name="max">The ending changelist number</param>
		/// <param name="results">Number of results to return</param>
		/// <param name="tags">Tags to filter the changes returned</param>
		/// <param name="filter">The filter to apply to the results</param>
		/// <returns>Http result code</returns>
		[HttpGet]
		[Route("/api/v1/streams/{streamId}/changes")]
		[ProducesResponseType(typeof(List<GetCommitResponse>), 200)]
		public async Task<ActionResult<List<object>>> GetChangesAsync(StreamId streamId, [FromQuery] int? min = null, [FromQuery] int? max = null, [FromQuery] int results = 50, [FromQuery] string? tags = null, PropertyFilter? filter = null)
		{
			IStream? stream = await _streamService.GetStreamAsync(streamId);
			if (stream == null)
			{
				return NotFound(streamId);
			}
			if (!await _streamService.AuthorizeAsync(stream, AclAction.ViewChanges, User, null))
			{
				return Forbid(AclAction.ViewChanges, streamId);
			}

			List<CommitTag>? commitTags = null;
			if (tags != null)
			{
				commitTags = tags.Split(';', StringSplitOptions.RemoveEmptyEntries).Select(x => new CommitTag(x)).ToList();
			}

			List<ICommit> commits = await _commitService.GetCollection(stream).FindAsync(min, max, results, commitTags).ToListAsync();

			List<GetCommitResponse> responses = new List<GetCommitResponse>();
			foreach (ICommit commit in commits)
			{
				IUser? author = await _userCollection.GetCachedUserAsync(commit.AuthorId);
				responses.Add(new GetCommitResponse(commit, author!, null, null));
			}
			return responses.ConvertAll(x => PropertyFilter.Apply(x, filter));
		}

		/// <summary>
		/// Gets a list of changes for a stream
		/// </summary>
		/// <param name="streamId">The stream id</param>
		/// <param name="changeNumber">The changelist number</param>
		/// <param name="filter">The filter to apply to the results</param>
		/// <returns>Http result code</returns>
		[HttpGet]
		[Route("/api/v1/streams/{streamId}/changes/{changeNumber}")]
		[ProducesResponseType(typeof(GetCommitResponse), 200)]
		public async Task<ActionResult<object>> GetChangeDetailsAsync(StreamId streamId, int changeNumber, PropertyFilter? filter = null)
		{
			IStream? stream = await _streamService.GetStreamAsync(streamId);
			if (stream == null)
			{
				return NotFound(streamId);
			}
			if (!await _streamService.AuthorizeAsync(stream, AclAction.ViewChanges, User, null))
			{
				return Forbid(AclAction.ViewChanges, streamId);
			}

			ICommit? changeDetails = await _commitService.GetCollection(stream).GetAsync(changeNumber);
			if(changeDetails == null)
			{
				return NotFound("CL {Change} not found in stream {StreamId}", changeNumber, streamId);
			}

			IUser? author = await _userCollection.GetCachedUserAsync(changeDetails.AuthorId);
			IReadOnlyList<CommitTag> tags = await changeDetails.GetTagsAsync(HttpContext.RequestAborted);
			IReadOnlyList<string> files = await changeDetails.GetFilesAsync(CancellationToken.None);

			return PropertyFilter.Apply(new GetCommitResponse(changeDetails, author!, tags, files), filter);
		}

		/// <summary>
		/// Gets the history of a step in the stream
		/// </summary>
		/// <param name="streamId">The stream id</param>
		/// <param name="templateId"></param>
		/// <param name="step">Name of the step to search for</param>
		/// <param name="change">Maximum changelist number to return</param>
		/// <param name="count">Number of results to return</param>
		/// <param name="filter">The filter to apply to the results</param>
		/// <returns>Http result code</returns>
		[HttpGet]
		[Route("/api/v1/streams/{streamId}/history")]
		[ProducesResponseType(typeof(List<GetJobStepRefResponse>), 200)]
		public async Task<ActionResult<List<object>>> GetStepHistoryAsync(StreamId streamId, [FromQuery] string templateId, [FromQuery] string step, [FromQuery] int? change = null, [FromQuery] int count = 10, [FromQuery] PropertyFilter? filter = null)
		{
			IStream? stream = await _streamService.GetStreamAsync(streamId);
			if (stream == null)
			{
				return NotFound(streamId);
			}
			if (!await _streamService.AuthorizeAsync(stream, AclAction.ViewJob, User, null))
			{
				return Forbid(AclAction.ViewJob, streamId);
			}

			TemplateId templateIdValue = new TemplateId(templateId);

			List<IJobStepRef> steps = await _jobStepRefCollection.GetStepsForNodeAsync(streamId, templateIdValue, step, change, true, count);
			return steps.ConvertAll(x => PropertyFilter.Apply(new GetJobStepRefResponse(x), filter));
		}

		/// <summary>
		/// Deletes a stream
		/// </summary>
		/// <param name="streamId">Id of the stream to update.</param>
		/// <returns>Http result code</returns>
		[HttpDelete]
		[Route("/api/v1/streams/{streamId}")]
		public async Task<ActionResult> DeleteStreamAsync(StreamId streamId)
		{
			IStream? stream = await _streamService.GetStreamAsync(streamId);
			if (stream == null)
			{
				return NotFound(streamId);
			}
			if (!await _streamService.AuthorizeAsync(stream, AclAction.DeleteStream, User, null))
			{
				return Forbid(AclAction.DeleteStream, streamId);
			}

			await _streamService.DeleteStreamAsync(streamId);
			return new OkResult();
		}

		/// <summary>
		/// Gets a template for a stream
		/// </summary>
		/// <param name="streamId">Unique id of the stream to query</param>
		/// <param name="templateId">Unique id of the template to query</param>
		/// <param name="filter">Filter for properties to return</param>
		/// <returns>Information about all the templates</returns>
		[HttpGet]
		[Route("/api/v1/streams/{streamId}/templates/{templateId}")]
		[ProducesResponseType(typeof(List<GetTemplateResponse>), 200)]
		public async Task<ActionResult<object>> GetTemplateAsync(StreamId streamId, TemplateId templateId, [FromQuery] PropertyFilter? filter = null)
		{
			IStream? stream = await _streamService.GetStreamAsync(streamId);
			if (stream == null)
			{
				return NotFound(streamId);
			}

			ITemplateRef? templateRef;
			if (!stream.Templates.TryGetValue(templateId, out templateRef))
			{
				return NotFound(streamId, templateId);
			}
			if (!await _streamService.AuthorizeAsync(stream, templateRef, AclAction.ViewTemplate, User, null))
			{
				return Forbid(AclAction.ViewTemplate, streamId);
			}

			ITemplate? template = await _templateCollection.GetAsync(templateRef.Hash);
			if(template == null)
			{
				return NotFound();
			}

			return new GetTemplateResponse(template).ApplyFilter(filter);
		}

		/// <summary>
		/// Update a stream template ref
		/// </summary>
		[HttpPut]
		[Authorize]
		[Route("/api/v1/streams/{streamId}/templates/{templateRefId}")]
		public async Task<ActionResult> UpdateStreamTemplateRefAsync(StreamId streamId, TemplateId templateRefId, [FromBody] UpdateTemplateRefRequest update)
		{
			if (!await _aclService.AuthorizeAsync(AclAction.AdminWrite, User))
			{
				return Forbid(AclAction.AdminWrite);
			}

			IStream? stream = await _streamService.GetStreamAsync(streamId);
			if (stream == null)
			{
				return NotFound(streamId);
			}

			if (!stream.Templates.ContainsKey(templateRefId))
			{
				return NotFound(streamId, templateRefId);
			}

			await _streamService.TryUpdateTemplateRefAsync(stream, templateRefId, update.StepStates);

			return Ok();
		}
	}
}
