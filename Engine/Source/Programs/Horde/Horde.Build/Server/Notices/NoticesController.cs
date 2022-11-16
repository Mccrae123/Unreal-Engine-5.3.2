// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Horde.Build.Acls;
using Horde.Build.Users;
using Horde.Build.Utilities;
using HordeCommon;
using Microsoft.AspNetCore.Authorization;
using Microsoft.AspNetCore.Mvc;
using Microsoft.Extensions.Options;
using MongoDB.Bson;

namespace Horde.Build.Server.Notices
{
	/// <summary>
	/// Controller for the /api/v1/notices endpoint
	/// </summary>
	[ApiController]
	[Authorize]
	[Route("[controller]")]
	public class NoticesController : ControllerBase
	{
		/// <summary>
		/// The acl service singleton
		/// </summary>
		readonly AclService _aclService;

		/// <summary>
		/// Reference to the user collection
		/// </summary>
		private readonly IUserCollection _userCollection;

		/// <summary>
		/// Notice service
		/// </summary>
		private readonly NoticeService _noticeService;

		/// <summary>
		/// cached globals
		/// </summary>
		readonly LazyCachedValue<Task<IGlobals>> _cachedGlobals;

		/// <summary>
		/// cached notices
		/// </summary>
		readonly LazyCachedValue<Task<List<INotice>>> _cachedNotices;

		/// <summary>
		/// Time information
		/// </summary>
		readonly IClock _clock;

		/// <summary>
		/// Constructor
		/// </summary>
		/// <param name="globalsService">The globals service singleton</param>
		/// <param name="noticeService">The notice service singleton</param>
		/// <param name="aclService">The acl service singleton</param>
		/// <param name="userCollection">The user collection singleton</param>
		/// <param name="clock"></param>
		public NoticesController(GlobalsService globalsService, NoticeService noticeService, AclService aclService, IUserCollection userCollection, IClock clock)
		{			
			_aclService = aclService;
			_userCollection = userCollection;
			_noticeService = noticeService;
			_cachedGlobals = new LazyCachedValue<Task<IGlobals>>(async () => await globalsService.GetAsync(), TimeSpan.FromSeconds(30.0));
			_cachedNotices = new LazyCachedValue<Task<List<INotice>>>(() => noticeService.GetNoticesAsync(), TimeSpan.FromMinutes(1));
			_clock = clock;
		}

		/// <summary>
		/// Add a status message
		/// </summary>
		/// <param name="request"></param>
		/// <returns></returns>
		[HttpPost("/api/v1/notices")]
		public async Task<ActionResult> AddNoticeAsync(CreateNoticeRequest request)
		{
			if (!await _aclService.AuthorizeAsync(AclAction.AdminWrite, User))
			{
				return Forbid();
			}

			INotice? notice = await _noticeService.AddNoticeAsync(request.Message, User.GetUserId(), request.StartTime, request.FinishTime);

			return notice == null ? NotFound() : Ok();

		}

		/// <summary>
		/// Update a status message
		/// </summary>
		/// <param name="request"></param>
		/// <returns></returns>
		[HttpPut("/api/v1/notices")]
		public async Task<ActionResult> UpdateNoticeAsync(UpdateNoticeRequest request)
		{
			if (!await _aclService.AuthorizeAsync(AclAction.AdminWrite, User))
			{
				return Forbid();
			}

			return await _noticeService.UpdateNoticeAsync(new ObjectId(request.Id), request.Message, request.StartTime, request.FinishTime) ? Ok() : NotFound();
		}

		/// <summary>
		/// Remove a manually added status message
		/// </summary>
		/// <returns></returns>
		[HttpDelete("/api/v1/notices/{id}")]
		public async Task<ActionResult> DeleteNoticeAsync(string id)
		{
			if (!await _aclService.AuthorizeAsync(AclAction.AdminWrite, User))
			{
				return Forbid();
			}

			await _noticeService.RemoveNoticeAsync(new ObjectId(id));
			
			return Ok();
		}

		/// <summary>
		/// Gets the current status messages
		/// </summary>
		/// <returns>The status messages</returns>
		[HttpGet("/api/v1/notices")]
		public async Task<List<GetNoticeResponse>> GetNoticesAsync()
		{
			List<GetNoticeResponse> messages = new List<GetNoticeResponse>();

			IGlobals globals = await _cachedGlobals.GetCached();

			DateTimeOffset now = TimeZoneInfo.ConvertTime(new DateTimeOffset(_clock.UtcNow), _clock.TimeZone);
						
			foreach (ScheduledDowntime schedule in globals.Config.Downtime)
			{
				DateTimeOffset start = TimeZoneInfo.ConvertTimeBySystemTimeZoneId(schedule.GetNext(now).StartTime, "UTC");
				DateTimeOffset finish = TimeZoneInfo.ConvertTimeBySystemTimeZoneId(schedule.GetNext(now).FinishTime, "UTC");
				messages.Add(new GetNoticeResponse() { ScheduledDowntime = true, StartTime = new DateTime(start.Ticks, DateTimeKind.Utc), FinishTime = new DateTime(finish.Ticks, DateTimeKind.Utc), Active = schedule.IsActive(now) });
			}

			// Add user notices
			List<INotice> notices = await _cachedNotices.GetCached();

			for (int i = 0; i < notices.Count; i++)
			{
				INotice notice = notices[i];
				GetThinUserInfoResponse? userInfo = null;

				if (notice.UserId != null)
				{
					userInfo = new GetThinUserInfoResponse(await _userCollection.GetCachedUserAsync(notice.UserId));
				}

				messages.Add(new GetNoticeResponse() { Id = notice.Id.ToString(), Active = true, Message = notice.Message, CreatedByUser = userInfo});
			}

			return messages;
			
		}
	}
}
