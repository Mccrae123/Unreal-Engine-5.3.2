// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using Horde.Build.Fleet.Autoscale;
using Horde.Build;
using Horde.Build.Models;
using Horde.Build.Services;
using Horde.Build.Utilities;
using Horde.Build.Tests;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace Horde.Build.Tests.Fleet
{
	using PoolId = StringId<IPool>;
	using ProjectId = StringId<IProject>;
	using StreamId = StringId<IStream>;
	
	public class FleetManagerSpy : IFleetManager
	{
		public int ExpandPoolAsyncCallCount { get; private set; }
		public int ShrinkPoolAsyncCallCount { get; private set; }
		
		public Task ExpandPoolAsync(IPool Pool, IReadOnlyList<IAgent> Agents, int Count)
		{
			ExpandPoolAsyncCallCount++;
			return Task.CompletedTask;
		}

		public Task ShrinkPoolAsync(IPool Pool, IReadOnlyList<IAgent> Agents, int Count)
		{
			ShrinkPoolAsyncCallCount++;
			return Task.CompletedTask;
		}

		public Task<int> GetNumStoppedInstancesAsync(IPool Pool)
		{
			throw new NotImplementedException();
		}
	}

	public class PoolSizeStrategySpy : IPoolSizeStrategy
	{
		public int CallCount { get; private set; }
		public HashSet<PoolId> PoolIdsSeen { get; } = new();
		
		public Task<List<PoolSizeData>> CalcDesiredPoolSizesAsync(List<PoolSizeData> Pools)
		{
			CallCount++;
			foreach (PoolSizeData Data in Pools)
			{
				PoolIdsSeen.Add(Data.Pool.Id);
			}
			return Task.FromResult(Pools);
		}

		public string Name { get; } = "PoolSizeStrategySpy";
	}

	[TestClass]
	public class AutoscaleServiceV2Test : TestSetup
	{
		FleetManagerSpy FleetManagerSpy = new();
		
		[TestMethod]
		public async Task PerPoolStrategy()
		{
			var Service = GetAutoscaleService(FleetManagerSpy);
			
			IPool Pool1 = await PoolService.CreatePoolAsync("bogusPoolLease1", null, true, 0, 0, SizeStrategy: PoolSizeStrategy.LeaseUtilization);
			IPool Pool2 = await PoolService.CreatePoolAsync("bogusPoolLease2", null, true, 0, 0, SizeStrategy: null);
			IPool Pool3 = await PoolService.CreatePoolAsync("bogusPoolJobQueue1", null, true, 0, 0, SizeStrategy: PoolSizeStrategy.JobQueue);
			IPool Pool4 = await PoolService.CreatePoolAsync("bogusPoolJobQueue2", null, true, 0, 0, SizeStrategy: PoolSizeStrategy.JobQueue);
			IPool Pool5 = await PoolService.CreatePoolAsync("bogusPoolNoOp", null, true, 0, 0, SizeStrategy: PoolSizeStrategy.NoOp);

			PoolSizeStrategySpy LeaseUtilizationSpy = new();
			PoolSizeStrategySpy JobQueueSpy = new();
			PoolSizeStrategySpy NoOpSpy = new();
			Service.OverridePoolSizeStrategiesDuringTesting(LeaseUtilizationSpy, JobQueueSpy, NoOpSpy);

			await Service.TickLeaderAsync(CancellationToken.None);
			
			Assert.AreEqual(1, LeaseUtilizationSpy.CallCount);
			Assert.IsTrue(LeaseUtilizationSpy.PoolIdsSeen.Contains(Pool1.Id));
			Assert.IsTrue(LeaseUtilizationSpy.PoolIdsSeen.Contains(Pool2.Id));
			
			Assert.AreEqual(1, JobQueueSpy.CallCount);
			Assert.IsTrue(JobQueueSpy.PoolIdsSeen.Contains(Pool3.Id));
			Assert.IsTrue(JobQueueSpy.PoolIdsSeen.Contains(Pool4.Id));
			
			Assert.AreEqual(1, NoOpSpy.CallCount);
			Assert.IsTrue(NoOpSpy.PoolIdsSeen.Contains(Pool5.Id));
		}

		[TestMethod]
		public async Task ScaleOutCooldown()
		{
			AutoscaleServiceV2 Service = GetAutoscaleService(FleetManagerSpy);
			IPool Pool = await PoolService.CreatePoolAsync("testPool", null, true, 0, 0, SizeStrategy: PoolSizeStrategy.NoOp);

			// First scale-out will succeed
			await Service.ResizePools(new() { new PoolSizeData(Pool, new List<IAgent>(), 1, "Testing") });
			Assert.AreEqual(1, FleetManagerSpy.ExpandPoolAsyncCallCount);
			
			// Cannot scale-out due to cool-down
			await Service.ResizePools(new() { new PoolSizeData(Pool, new List<IAgent>(), 2, "Testing") });
			Assert.AreEqual(1, FleetManagerSpy.ExpandPoolAsyncCallCount);

			// Wait some time and then try again
			await Clock.AdvanceAsync(TimeSpan.FromHours(2));
			await Service.ResizePools(new() { new PoolSizeData(Pool, new List<IAgent>(), 2, "Testing") });
			Assert.AreEqual(2, FleetManagerSpy.ExpandPoolAsyncCallCount);
		}
		
		[TestMethod]
		public async Task ScaleInCooldown()
		{
			AutoscaleServiceV2 Service = GetAutoscaleService(FleetManagerSpy);
			IPool Pool = await PoolService.CreatePoolAsync("testPool", null, true, 0, 0, SizeStrategy: PoolSizeStrategy.NoOp);
			IAgent Agent1 = await CreateAgentAsync(Pool);
			IAgent Agent2 = await CreateAgentAsync(Pool);

			// First scale-out will succeed
			await Service.ResizePools(new() { new PoolSizeData(Pool, new () { Agent1, Agent2 }, 1, "Testing") });
			Assert.AreEqual(1, FleetManagerSpy.ShrinkPoolAsyncCallCount);
			
			// Cannot scale-out due to cool-down
			await Service.ResizePools(new() { new PoolSizeData(Pool, new () { Agent1 }, 0, "Testing") });
			Assert.AreEqual(1, FleetManagerSpy.ShrinkPoolAsyncCallCount);

			// Wait some time and then try again
			await Clock.AdvanceAsync(TimeSpan.FromHours(2));
			await Service.ResizePools(new() { new PoolSizeData(Pool, new () { Agent1 }, 0, "Testing") });
			Assert.AreEqual(2, FleetManagerSpy.ShrinkPoolAsyncCallCount);
		}

		private async Task<List<PoolSizeData>> GetPoolSizeData()
		{
			IPool Pool1 = await PoolService.CreatePoolAsync("bogusPool1", null, true, 0, 0);
			IAgent Agent1 = await CreateAgentAsync(Pool1);
			IAgent Agent2 = await CreateAgentAsync(Pool1);

			return new List<PoolSizeData>
			{
				new (Pool1, new List<IAgent> { Agent1, Agent2 }, null)
			};
		}

		private AutoscaleServiceV2 GetAutoscaleService(IFleetManager FleetManager)
		{
			ILogger<AutoscaleServiceV2> Logger = ServiceProvider.GetRequiredService<ILogger<AutoscaleServiceV2>>();
			IOptions<ServerSettings> ServerSettingsOpt = ServiceProvider.GetRequiredService<IOptions<ServerSettings>>();

			LeaseUtilizationStrategy LeaseUtilizationStrategy = new(AgentCollection, PoolCollection, LeaseCollection, Clock);
			JobQueueStrategy JobQueueStrategy = new(JobCollection, GraphCollection, StreamService, Clock);
			AutoscaleServiceV2 Service = new AutoscaleServiceV2(LeaseUtilizationStrategy,JobQueueStrategy, new NoOpPoolSizeStrategy(), AgentCollection, PoolCollection, FleetManager, new NoOpDogStatsd(), Clock, ServerSettingsOpt, Logger);
			return Service;
		}

	}
}