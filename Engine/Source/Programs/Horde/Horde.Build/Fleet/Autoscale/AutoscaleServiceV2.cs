// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using HordeCommon;
using HordeServer;
using HordeServer.Collections;
using HordeServer.Models;
using HordeServer.Services;
using HordeServer.Utilities;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using OpenTracing;
using OpenTracing.Util;
using StatsdClient;

namespace Horde.Build.Fleet.Autoscale
{
	using PoolId = StringId<IPool>;

	/// <summary>
	/// Service for managing the autoscaling of agent pools
	/// </summary>
	public sealed class AutoscaleServiceV2 : IHostedService, IDisposable
	{
		private IPoolSizeStrategy LeaseUtilizationStrategy;
		private IPoolSizeStrategy JobQueueStrategy;
		private IPoolSizeStrategy NoOpStrategy;
		private readonly IAgentCollection AgentCollection;
		private readonly IPoolCollection PoolCollection;
		private readonly IFleetManager FleetManager;
		private readonly IDogStatsd DogStatsd;
		private readonly IClock Clock;
		private readonly ITicker Ticker;
		private readonly ServerSettings Settings;
		private readonly TimeSpan DefaultScaleOutCooldown;
		private readonly TimeSpan DefaultScaleInCooldown;
		private readonly ILogger<AutoscaleServiceV2> Logger;

		/// <summary>
		/// Constructor
		/// </summary>
		public AutoscaleServiceV2(LeaseUtilizationStrategy LeaseUtilizationStrategy, JobQueueStrategy JobQueueStrategy, NoOpPoolSizeStrategy NoOpStrategy, IAgentCollection AgentCollection, IPoolCollection PoolCollection, IFleetManager FleetManager, IDogStatsd DogStatsd, IClock Clock, IOptions<ServerSettings> Settings, ILogger<AutoscaleServiceV2> Logger)
		{
			this.LeaseUtilizationStrategy = LeaseUtilizationStrategy;
			this.JobQueueStrategy = JobQueueStrategy;
			this.NoOpStrategy = NoOpStrategy;
			this.AgentCollection = AgentCollection;
			this.PoolCollection = PoolCollection;
			this.FleetManager = FleetManager;
			this.DogStatsd = DogStatsd;
			this.Clock = Clock;
			this.Ticker = Clock.AddSharedTicker<AutoscaleServiceV2>(TimeSpan.FromMinutes(5.0), TickLeaderAsync, Logger);
			this.Settings = Settings.Value;
			this.Logger = Logger;
			this.DefaultScaleOutCooldown = TimeSpan.FromSeconds(Settings.Value.AgentPoolScaleOutCooldownSeconds);
			this.DefaultScaleInCooldown = TimeSpan.FromSeconds(Settings.Value.AgentPoolScaleInCooldownSeconds);
		}

		/// <inheritdoc/>
		public Task StartAsync(CancellationToken CancellationToken) => Ticker.StartAsync();

		/// <inheritdoc/>
		public Task StopAsync(CancellationToken CancellationToken) => Ticker.StopAsync();

		/// <inheritdoc/>
		public void Dispose() => Ticker.Dispose();

		internal async ValueTask TickLeaderAsync(CancellationToken StoppingToken)
		{
			Logger.LogInformation("Autoscaling pools...");
			Stopwatch Stopwatch = Stopwatch.StartNew();
			using IScope _ = GlobalTracer.Instance.BuildSpan("AutoscaleService").StartActive();
			
			// Group pools by strategy type to ensure they can be called once (more optimal)
			Dictionary<PoolSizeStrategy, List<PoolSizeData>> PoolSizeDataByStrategy = await GetPoolSizeDataByStrategyType();
			foreach ((var StrategyType, var CurrentData) in PoolSizeDataByStrategy)
			{
				List<PoolSizeData> NewData = await GetPoolSizeStrategy(StrategyType).CalcDesiredPoolSizesAsync(CurrentData);
				await ResizePools(NewData);
			}
			
			Stopwatch.Stop();
			Logger.LogInformation("Autoscaling pools took {ElapsedTime} ms", Stopwatch.ElapsedMilliseconds);
		}

		internal async Task<Dictionary<PoolSizeStrategy, List<PoolSizeData>>> GetPoolSizeDataByStrategyType()
		{
			List<IAgent> Agents = await AgentCollection.FindAsync(Status: AgentStatus.Ok);
			List<IAgent> GetAgentsInPool(PoolId PoolId) => Agents.FindAll(a => a.GetPools().Any(p => p == PoolId));
			List<IPool> Pools = await PoolCollection.GetAsync();

			Dictionary<PoolSizeStrategy, List<PoolSizeData>> Result = new();
			foreach (PoolSizeStrategy StrategyType in Enum.GetValues<PoolSizeStrategy>())
			{
				Result[StrategyType] = Pools
					.Where(x => (x.SizeStrategy ?? Settings.DefaultAgentPoolSizeStrategy) == StrategyType)
					.Select(x => new PoolSizeData(x, GetAgentsInPool(x.Id), null))
					.ToList();
			}

			return Result;
		}

		internal async Task ResizePools(List<PoolSizeData> PoolSizeDatas)
		{
			foreach (PoolSizeData PoolSizeData in PoolSizeDatas.OrderByDescending(x => x.Agents.Count))
			{
				IPool Pool = PoolSizeData.Pool;

				if (!Pool.EnableAutoscaling || PoolSizeData.DesiredAgentCount == null) continue;

				int CurrentAgentCount = PoolSizeData.Agents.Count;
				int DesiredAgentCount = PoolSizeData.DesiredAgentCount.Value;
				int DeltaAgentCount = DesiredAgentCount - CurrentAgentCount;

				Logger.LogInformation("{PoolName,-48} Current={Current,4} Target={Target,4} Delta={Delta,4} Status={Status}", Pool.Name, CurrentAgentCount, DesiredAgentCount, DeltaAgentCount, PoolSizeData.StatusMessage);
				
				try
				{
					using IScope Scope = GlobalTracer.Instance.BuildSpan("ScalingPool").StartActive();
					Scope.Span.SetTag("poolName", Pool.Name);
					Scope.Span.SetTag("current", CurrentAgentCount);
					Scope.Span.SetTag("desired", DesiredAgentCount);
					Scope.Span.SetTag("delta", DeltaAgentCount);

					if (DeltaAgentCount > 0)
					{
						TimeSpan ScaleOutCooldown = Pool.ScaleOutCooldown ?? DefaultScaleOutCooldown;
						bool IsCoolingDown = Pool.LastScaleUpTime != null && Pool.LastScaleUpTime + ScaleOutCooldown > Clock.UtcNow;
						Scope.Span.SetTag("isCoolingDown", IsCoolingDown);
						if (!IsCoolingDown)
						{
							await FleetManager.ExpandPoolAsync(Pool, PoolSizeData.Agents, DeltaAgentCount);
							await PoolCollection.TryUpdateAsync(Pool, LastScaleUpTime: Clock.UtcNow);
						}
						else
						{
							TimeSpan? CooldownTimeLeft = Pool.LastScaleUpTime + DefaultScaleOutCooldown - Clock.UtcNow;
							Logger.LogDebug("Cannot scale out {PoolName}, it's cooling down for another {TimeLeft} secs", Pool.Name, CooldownTimeLeft?.TotalSeconds);
						}
					}

					if (DeltaAgentCount < 0)
					{
						TimeSpan ScaleInCooldown = Pool.ScaleInCooldown ?? DefaultScaleInCooldown;
						bool IsCoolingDown = Pool.LastScaleDownTime != null && Pool.LastScaleDownTime + ScaleInCooldown > Clock.UtcNow;
						Scope.Span.SetTag("isCoolingDown", IsCoolingDown);
						if (!IsCoolingDown)
						{
							await FleetManager.ShrinkPoolAsync(Pool, PoolSizeData.Agents, -DeltaAgentCount);
							await PoolCollection.TryUpdateAsync(Pool, LastScaleDownTime: Clock.UtcNow);
						}
						else
						{
							TimeSpan? CooldownTimeLeft = Pool.LastScaleDownTime + DefaultScaleInCooldown - Clock.UtcNow;
							Logger.LogDebug("Cannot scale in {PoolName}, it's cooling down for another {TimeLeft} secs", Pool.Name, CooldownTimeLeft?.TotalSeconds);
						}
					}
				}
				catch (Exception Ex)
				{
					Logger.LogError(Ex, "Failed to scale {PoolName}:\n{Exception}", Pool.Name, Ex);
					continue;
				}

				DogStatsd.Gauge("agentpools.autoscale.target", DesiredAgentCount, tags: new []{"pool:" + Pool.Name});
				DogStatsd.Gauge("agentpools.autoscale.current", CurrentAgentCount, tags: new []{"pool:" + Pool.Name});
			}
		}

		/// <summary>
		/// Backdoor for tests to override strategies with test doubles
		/// These cannot supplied in the constructor since the DI requires concrete implementations.
		/// </summary>
		/// <param name="LeaseUtilizationStrategy"></param>
		/// <param name="JobQueueStrategy"></param>
		/// <param name="NoOpStrategy"></param>
		internal void OverridePoolSizeStrategiesDuringTesting(IPoolSizeStrategy LeaseUtilizationStrategy, IPoolSizeStrategy JobQueueStrategy, IPoolSizeStrategy NoOpStrategy)
		{
			this.LeaseUtilizationStrategy = LeaseUtilizationStrategy;
			this.JobQueueStrategy = JobQueueStrategy;
			this.NoOpStrategy = NoOpStrategy;
		}
		
		private IPoolSizeStrategy GetPoolSizeStrategy(PoolSizeStrategy Type)
		{
			return Type switch
			{
				PoolSizeStrategy.LeaseUtilization => LeaseUtilizationStrategy,
				PoolSizeStrategy.JobQueue => JobQueueStrategy,
				PoolSizeStrategy.NoOp => NoOpStrategy,
				_ => NoOpStrategy,
			};
		}
	}
}
