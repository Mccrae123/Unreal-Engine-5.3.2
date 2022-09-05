// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.Collections.Generic;
using System.ComponentModel.DataAnnotations;
using System.Diagnostics.CodeAnalysis;
using System.Linq;
using EpicGames.Core;
using Horde.Build.Acls;
using Horde.Build.Agents.Pools;
using Horde.Build.Issues;
using Horde.Build.Jobs.Graphs;
using Horde.Build.Jobs.Schedules;
using Horde.Build.Jobs.Templates;
using Horde.Build.Perforce;
using Horde.Build.Server;
using Horde.Build.Utilities;
using HordeCommon;

namespace Horde.Build.Streams
{
	using PoolId = StringId<IPool>;
	using TemplateRefId = StringId<TemplateRef>;
	using WorkflowId = StringId<WorkflowConfig>;

	/// <summary>
	/// How to replicate data for this stream
	/// </summary>
	public enum ContentReplicationMode
	{
		/// <summary>
		/// No content will be replicated for this stream
		/// </summary>
		None,

		/// <summary>
		/// Only replicate depot path and revision data for each file
		/// </summary>
		RevisionsOnly,

		/// <summary>
		/// Replicate full stream contents to storage
		/// </summary>
		Full,
	}

	/// <summary>
	/// Config for a stream
	/// </summary>
	[JsonSchema("https://unrealengine.com/horde/stream")]
	[JsonSchemaCatalog("Horde Stream", "Horde stream configuration file", new[] { "*.stream.json", "Streams/*.json" })]
	public class StreamConfig
	{
		/// <summary>
		/// Name of the stream
		/// </summary>
		[Required]
		public string Name { get; set; } = String.Empty;

		/// <summary>
		/// The perforce cluster containing the stream
		/// </summary>
		public string ClusterName { get; set; } = PerforceCluster.DefaultName;

		/// <summary>
		/// Order for this stream
		/// </summary>
		public int Order { get; set; } = 128;

		/// <summary>
		/// Notification channel for all jobs in this stream
		/// </summary>
		public string? NotificationChannel { get; set; }

		/// <summary>
		/// Notification channel filter for this template. Can be Success|Failure|Warnings
		/// </summary>
		public string? NotificationChannelFilter { get; set; }

		/// <summary>
		/// Channel to post issue triage notifications
		/// </summary>
		public string? TriageChannel { get; set; }

		/// <summary>
		/// Legacy name for the default preflight template
		/// </summary>
		[Obsolete("Use DefaultPreflight instead")]
		public string? DefaultPreflightTemplate
		{
			get => DefaultPreflight?.TemplateId?.ToString();
			set => DefaultPreflight = (value == null)? null : new DefaultPreflightConfig { TemplateId = new TemplateRefId(value) };
		}

		/// <summary>
		/// Default template for running preflights
		/// </summary>
		public DefaultPreflightConfig? DefaultPreflight { get; set; }

		/// <summary>
		/// List of tabs to show for the new stream
		/// </summary>
		public List<TabConfig> Tabs { get; set; } = new List<TabConfig>();

		/// <summary>
		/// Map of agent name to type
		/// </summary>
		public Dictionary<string, AgentConfig> AgentTypes { get; set; } = new Dictionary<string, AgentConfig>();

		/// <summary>
		/// Map of workspace name to type
		/// </summary>
		public Dictionary<string, WorkspaceConfig> WorkspaceTypes { get; set; } = new Dictionary<string, WorkspaceConfig>();

		/// <summary>
		/// List of templates to create
		/// </summary>
		public List<TemplateRefConfig> Templates { get; set; } = new List<TemplateRefConfig>();

		/// <summary>
		/// Custom permissions for this object
		/// </summary>
		public AclConfig? Acl { get; set; }

		/// <summary>
		/// Pause stream builds until specified date
		/// </summary>
		public DateTime? PausedUntil { get; set; }

		/// <summary>
		/// Reason for pausing builds of the stream
		/// </summary>
		public string? PauseComment { get; set; }

		/// <summary>
		/// How to replicate data from VCS to Horde Storage.
		/// </summary>
		public ContentReplicationMode ReplicationMode { get; set; }

		/// <summary>
		/// Filter for paths to be replicated to storage, as a Perforce wildcard relative to the root of the workspace.
		/// </summary>
		public string? ReplicationFilter { get; set; }

		/// <summary>
		/// Stream to use for replication, if different to the default.
		/// </summary>
		public string? ReplicationStream { get; set; }

		/// <summary>
		/// Workflows for dealing with new issues
		/// </summary>
		public List<WorkflowConfig> Workflows { get; set; } = new List<WorkflowConfig>();

		/// <summary>
		/// Tokens to create for each job step
		/// </summary>
		public List<TokenConfig> Tokens { get; set; } = new List<TokenConfig>(); 

		/// <summary>
		/// Tries to find a template with the given id
		/// </summary>
		/// <param name="templateRefId"></param>
		/// <param name="templateConfig"></param>
		/// <returns></returns>
		public bool TryGetTemplate(TemplateRefId templateRefId, [NotNullWhen(true)] out TemplateRefConfig? templateConfig)
		{
			templateConfig = Templates.FirstOrDefault(x => x.Id == templateRefId);
			return templateConfig != null;
		}

		/// <summary>
		/// Tries to find a workflow with the given id
		/// </summary>
		/// <param name="workflowId"></param>
		/// <param name="workflowConfig"></param>
		/// <returns></returns>
		public bool TryGetWorkflow(WorkflowId workflowId, [NotNullWhen(true)] out WorkflowConfig? workflowConfig)
		{
			workflowConfig = Workflows.FirstOrDefault(x => x.Id == workflowId);
			return workflowConfig != null;
		}
	}


	/// <summary>
	/// Information about a page to display in the dashboard for a stream
	/// </summary>
	[JsonKnownTypes(typeof(JobsTabConfig))]
	public abstract class TabConfig
	{
		/// <summary>
		/// Title of this page
		/// </summary>
		[Required]
		public string Title { get; set; } = null!;
	}

	/// <summary>
	/// Describes a job page
	/// </summary>
	[JsonDiscriminator("Jobs")]
	public class JobsTabConfig : TabConfig
	{
		/// <summary>
		/// Whether to show job names on this page
		/// </summary>
		public bool ShowNames { get; set; }

		/// <summary>
		/// Names of jobs to include on this page. If there is only one name specified, the name column does not need to be displayed.
		/// </summary>
		public List<string>? JobNames { get; set; }

		/// <summary>
		/// List of job template names to show on this page.
		/// </summary>
		public List<TemplateRefId>? Templates { get; set; }

		/// <summary>
		/// Columns to display for different types of aggregates
		/// </summary>
		public List<JobsTabColumnConfig>? Columns { get; set; }
	}

	/// <summary>
	/// Type of a column in a jobs tab
	/// </summary>
	public enum JobsTabColumnType
	{
		/// <summary>
		/// Contains labels
		/// </summary>
		Labels,

		/// <summary>
		/// Contains parameters
		/// </summary>
		Parameter
	}

	/// <summary>
	/// Describes a column to display on the jobs page
	/// </summary>
	public class JobsTabColumnConfig
	{
		/// <summary>
		/// The type of column
		/// </summary>
		public JobsTabColumnType Type { get; set; } = JobsTabColumnType.Labels;

		/// <summary>
		/// Heading for this column
		/// </summary>
		[Required]
		public string Heading { get; set; } = null!;

		/// <summary>
		/// Category of aggregates to display in this column. If null, includes any aggregate not matched by another column.
		/// </summary>
		public string? Category { get; set; }

		/// <summary>
		/// Parameter to show in this column
		/// </summary>
		public string? Parameter { get; set; }

		/// <summary>
		/// Relative width of this column.
		/// </summary>
		public int? RelativeWidth { get; set; }
	}

	/// <summary>
	/// Mapping from a BuildGraph agent type to a set of machines on the farm
	/// </summary>
	public class AgentConfig
	{
		/// <summary>
		/// Pool of agents to use for this agent type
		/// </summary>
		[Required]
		public PoolId Pool { get; set; }

		/// <summary>
		/// Name of the workspace to sync
		/// </summary>
		public string? Workspace { get; set; }

		/// <summary>
		/// Path to the temporary storage dir
		/// </summary>
		public string? TempStorageDir { get; set; }

		/// <summary>
		/// Environment variables to be set when executing the job
		/// </summary>
		public Dictionary<string, string>? Environment { get; set; }

		/// <summary>
		/// Creates an API response object from this stream
		/// </summary>
		/// <returns>The response object</returns>
		public HordeCommon.Rpc.GetAgentTypeResponse ToRpcResponse()
		{
			HordeCommon.Rpc.GetAgentTypeResponse response = new HordeCommon.Rpc.GetAgentTypeResponse();
			if (TempStorageDir != null)
			{
				response.TempStorageDir = TempStorageDir;
			}
			if (Environment != null)
			{
				response.Environment.Add(Environment);
			}
			return response;
		}
	}

	/// <summary>
	/// Information about a workspace type
	/// </summary>
	public class WorkspaceConfig
	{
		/// <summary>
		/// Name of the Perforce server cluster to use
		/// </summary>
		public string? Cluster { get; set; }

		/// <summary>
		/// The Perforce server and port (eg. perforce:1666)
		/// </summary>
		public string? ServerAndPort { get; set; }

		/// <summary>
		/// User to log into Perforce with (defaults to buildmachine)
		/// </summary>
		public string? UserName { get; set; }

		/// <summary>
		/// Password to use to log into the workspace
		/// </summary>
		public string? Password { get; set; }

		/// <summary>
		/// Identifier to distinguish this workspace from other workspaces. Defaults to the workspace type name.
		/// </summary>
		public string? Identifier { get; set; }

		/// <summary>
		/// Override for the stream to sync
		/// </summary>
		public string? Stream { get; set; }

		/// <summary>
		/// Custom view for the workspace
		/// </summary>
		public List<string>? View { get; set; }

		/// <summary>
		/// Whether to use an incrementally synced workspace
		/// </summary>
		public bool Incremental { get; set; }

		/// <summary>
		/// Whether to use the AutoSDK
		/// </summary>
		public bool UseAutoSdk { get; set; } = true;
	}

	/// <summary>
	/// Query selecting the base changelist to use
	/// </summary>
	public class ChangeQueryConfig
	{
		/// <summary>
		/// The template id to query
		/// </summary>
		public TemplateRefId? TemplateId { get; set; }

		/// <summary>
		/// The target to query
		/// </summary>
		public string? Target { get; set; }

		/// <summary>
		/// Whether to match a job that produced warnings
		/// </summary>
		public List<JobStepOutcome>? Outcomes { get; set; }
	}

	/// <summary>
	/// Specifies defaults for running a preflight
	/// </summary>
	public class DefaultPreflightConfig
	{
		/// <summary>
		/// The template id to query
		/// </summary>
		public TemplateRefId? TemplateId { get; set; }

		/// <summary>
		/// Query for the change to use
		/// </summary>
		public ChangeQueryConfig? Change { get; set; }
	}

	/// <summary>
	/// Trigger for another template
	/// </summary>
	public class ChainedJobTemplateConfig
	{
		/// <summary>
		/// Name of the target that needs to complete before starting the other template
		/// </summary>
		[Required]
		public string Trigger { get; set; } = String.Empty;

		/// <summary>
		/// Id of the template to trigger
		/// </summary>
		[Required]
		public string TemplateId { get; set; } = String.Empty;
	}

	/// <summary>
	/// Parameters to create a template within a stream
	/// </summary>
	public class TemplateRefConfig : TemplateConfig
	{
		TemplateRefId _id;

		/// <summary>
		/// Optional identifier for this ref. If not specified, an id will be generated from the name.
		/// </summary>
		public TemplateRefId Id
		{
			get => _id.IsEmpty ? TemplateRefId.Sanitize(Name) : _id;
			set => _id = value;
		}

		/// <summary>
		/// Whether to show badges in UGS for these jobs
		/// </summary>
		public bool ShowUgsBadges { get; set; }

		/// <summary>
		/// Whether to show alerts in UGS for these jobs
		/// </summary>
		public bool ShowUgsAlerts { get; set; }

		/// <summary>
		/// Notification channel for this template. Overrides the stream channel if set.
		/// </summary>
		public string? NotificationChannel { get; set; }

		/// <summary>
		/// Notification channel filter for this template. Can be Success|Failure|Warnings
		/// </summary>
		public string? NotificationChannelFilter { get; set; }

		/// <summary>
		/// Triage channel for this template. Overrides the stream channel if set.
		/// </summary>
		public string? TriageChannel { get; set; }

		/// <summary>
		/// Workflow to user for this stream
		/// </summary>
		public WorkflowId? WorkflowId
		{
			get => Annotations.WorkflowId;
			set => Annotations.WorkflowId = value;
		}

		/// <summary>
		/// Default annotations to apply to nodes in this template
		/// </summary>
		public NodeAnnotations Annotations { get; set; } = new NodeAnnotations();

		/// <summary>
		/// Schedule to execute this template
		/// </summary>
		public ScheduleConfig? Schedule { get; set; }

		/// <summary>
		/// List of chained job triggers
		/// </summary>
		public List<ChainedJobTemplateConfig>? ChainedJobs { get; set; }

		/// <summary>
		/// The ACL for this template
		/// </summary>
		public AclConfig? Acl { get; set; }
	}

	/// <summary>
	/// Parameters to create a new schedule
	/// </summary>
	public class SchedulePatternConfig
	{
		/// <summary>
		/// Days of the week to run this schedule on. If null, the schedule will run every day.
		/// </summary>
		public List<string>? DaysOfWeek { get; set; }

		/// <summary>
		/// Time during the day for the first schedule to trigger. Measured in minutes from midnight.
		/// </summary>
		public int MinTime { get; set; }

		/// <summary>
		/// Time during the day for the last schedule to trigger. Measured in minutes from midnight.
		/// </summary>
		public int? MaxTime { get; set; }

		/// <summary>
		/// Interval between each schedule triggering
		/// </summary>
		public int? Interval { get; set; }

		/// <summary>
		/// Constructs a model object from this request
		/// </summary>
		/// <returns>Model object</returns>
		public SchedulePattern ToModel()
		{
			return new SchedulePattern(DaysOfWeek?.ConvertAll(x => Enum.Parse<DayOfWeek>(x)), MinTime, MaxTime, Interval);
		}
	}

	/// <summary>
	/// Gate allowing a schedule to trigger.
	/// </summary>
	public class ScheduleGateConfig
	{
		/// <summary>
		/// The template containing the dependency
		/// </summary>
		[Required]
		public string TemplateId { get; set; } = String.Empty;

		/// <summary>
		/// Target to wait for
		/// </summary>
		[Required]
		public string Target { get; set; } = String.Empty;

		/// <summary>
		/// Constructs a model object
		/// </summary>
		/// <returns>New model object.</returns>
		public ScheduleGate ToModel()
		{
			return new ScheduleGate(new TemplateRefId(TemplateId), Target);
		}
	}

	/// <summary>
	/// Parameters to create a new schedule
	/// </summary>
	public class ScheduleConfig
	{
		/// <summary>
		/// Roles to impersonate for this schedule
		/// </summary>
		public List<AclClaimConfig>? Claims { get; set; }

		/// <summary>
		/// Whether the schedule should be enabled
		/// </summary>
		public bool Enabled { get; set; }

		/// <summary>
		/// Maximum number of builds that can be active at once
		/// </summary>
		public int MaxActive { get; set; }

		/// <summary>
		/// Maximum number of changes the schedule can fall behind head revision. If greater than zero, builds will be triggered for every submitted changelist until the backlog is this size.
		/// </summary>
		public int MaxChanges { get; set; }

		/// <summary>
		/// Whether the build requires a change to be submitted
		/// </summary>
		public bool RequireSubmittedChange { get; set; } = true;

		/// <summary>
		/// Gate allowing the schedule to trigger
		/// </summary>
		public ScheduleGateConfig? Gate { get; set; }

		/// <summary>
		/// The types of changes to run for
		/// </summary>
		public List<ChangeContentFlags>? Filter { get; set; }

		/// <summary>
		/// Files that should cause the job to trigger
		/// </summary>
		public List<string>? Files { get; set; }

		/// <summary>
		/// Parameters for the template
		/// </summary>
		public Dictionary<string, string> TemplateParameters { get; set; } = new Dictionary<string, string>();

		/// <summary>
		/// New patterns for the schedule
		/// </summary>
		public List<SchedulePatternConfig> Patterns { get; set; } = new List<SchedulePatternConfig>();

		/// <summary>
		/// Constructs a model object
		/// </summary>
		/// <param name="currentTimeUtc">The current time</param>
		/// <returns>New model object</returns>
		public Schedule ToModel(DateTime currentTimeUtc)
		{
			return new Schedule(currentTimeUtc, Enabled, MaxActive, MaxChanges, RequireSubmittedChange, Gate?.ToModel(), Filter, Files, TemplateParameters, Patterns.ConvertAll(x => x.ToModel()));
		}
	}

	/// <summary>
	/// Configuration for allocating access tokens for each job
	/// </summary>
	public class TokenConfig
	{
		/// <summary>
		/// URL to request tokens from
		/// </summary>
		[Required]
		public Uri Url { get; set; } = null!;

		/// <summary>
		/// Client id to use to request a new token
		/// </summary>
		[Required]
		public string ClientId { get; set; } = String.Empty;

		/// <summary>
		/// Client secret to request a new access token
		/// </summary>
		[Required]
		public string ClientSecret { get; set; } = String.Empty;

		/// <summary>
		/// Environment variable to set with the access token
		/// </summary>
		[Required]
		public string EnvVar { get; set; } = String.Empty;
	}
}
