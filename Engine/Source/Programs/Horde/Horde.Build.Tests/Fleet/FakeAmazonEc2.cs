// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.Collections.Generic;
using System.Linq;
using System.Net;
using System.Threading;
using System.Threading.Tasks;
using Amazon.EC2;
using Amazon.EC2.Model;
using Moq;

namespace Horde.Build.Tests.Fleet;

/// <summary>
/// Fake implementation of the IAmazonEC2 interface
/// Simplifies testing of external EC2 API calls.
/// Uses a mock to avoid implementing the entire interface. Even if shimmed, it becomes many lines of no-op code.
/// </summary>
public class FakeAmazonEc2
{
	public static readonly InstanceState StatePending = new() { Code = 0, Name = "pending" };
	public static readonly InstanceState StateStopped = new() { Code = 80, Name = "stopped" };
	public const string AzUsEast1A = "us-east-1a";
	public const string AzUsEast1B = "us-east-1b";
	
	private readonly Mock<IAmazonEC2> _mock;
	private readonly Dictionary<string, Instance> _instances = new();
	private int _instanceIdCounter;
	
	internal IReadOnlyDictionary<string, Instance> Instances => _instances;

	private readonly Dictionary<string, Dictionary<string, int>> _availabilityZoneCapacity = new()
	{
		{AzUsEast1A, new () },
		{AzUsEast1B, new () }
	};

	public FakeAmazonEc2()
	{
		_mock = new Mock<IAmazonEC2>(MockBehavior.Strict);
		_mock
			.Setup(x => x.StartInstancesAsync(It.IsAny<StartInstancesRequest>(), It.IsAny<CancellationToken>()))
			.Returns(StartInstancesAsync);
		
		_mock
			.Setup(x => x.DescribeInstancesAsync(It.IsAny<DescribeInstancesRequest>(), It.IsAny<CancellationToken>()))
			.Returns(DescribeInstancesAsync);
		
		_mock
			.Setup(x => x.ModifyInstanceAttributeAsync(It.IsAny<ModifyInstanceAttributeRequest>(), It.IsAny<CancellationToken>()))
			.Returns(ModifyInstanceAttributeAsync);
	}

	public IAmazonEC2 Get()
	{
		return _mock.Object;
	}

	public void SetCapacity(string availabilityZone, string instanceType, int capacity)
	{
		Dictionary<string, int> temp = _availabilityZoneCapacity.GetValueOrDefault(availabilityZone, new());
		temp[instanceType] = capacity;
		_availabilityZoneCapacity[availabilityZone] = temp;
	}

	public Instance AddInstance(InstanceState state, InstanceType type,
		string availabilityZone = AzUsEast1A, DateTime? launchTime = null)
	{
		Instance i = new()
		{
			InstanceId = "bogus-instance-" + _instanceIdCounter++,
			State = state, InstanceType = type,
			Placement = new Placement(availabilityZone),
			LaunchTime = launchTime ?? DateTime.UnixEpoch
		};
		_instances.Add(i.InstanceId, i);
		return i;
	}
	
	public Instance? GetInstance(string instanceId)
	{
		return _instances[instanceId];
	}

	public int GetStoppedInstanceCount() { return _instances.Values.Count(x => x.State == StateStopped); }
	public int GetPendingInstanceCount() { return _instances.Values.Count(x => x.State == StatePending); }

	private Task<DescribeInstancesResponse> DescribeInstancesAsync(DescribeInstancesRequest request, CancellationToken cancellationToken)
	{
		return Task.FromResult(new DescribeInstancesResponse
		{
			Reservations = new () { new Reservation { Instances = _instances.Values.ToList() } },
			HttpStatusCode = HttpStatusCode.OK
		});
	}
	
	private Task<StartInstancesResponse> StartInstancesAsync(StartInstancesRequest request, CancellationToken cancellationToken)
	{
		List<InstanceStateChange> stateChanges = new();
		
		// Check if capacity in each AZ can handle this start instance request.
		// If not, abort with an exception prior attempting to start them and modifying in-memory state.
		Dictionary<string, Dictionary<string, int>> azInstanceTypeCapacities = new (_availabilityZoneCapacity);
		foreach (string instanceId in request.InstanceIds)
		{
			if (!_instances.ContainsKey(instanceId))
			{
				throw new ArgumentException("Unknown instance ID " + instanceId);
			}

			string az = _instances[instanceId].Placement.AvailabilityZone;
			string instanceType = _instances[instanceId].InstanceType;
			int capacity = azInstanceTypeCapacities[az].GetValueOrDefault(instanceType, 0);
			
			if (capacity <= 0)
			{
				throw new AmazonEC2Exception("Insufficient capacity.");
			}

			azInstanceTypeCapacities[az][instanceType] = capacity - 1;
		}
		
		foreach (string instanceId in request.InstanceIds)
		{
			if (_instances[instanceId].State.Name == StateStopped.Name)
			{
				stateChanges.Add(new()
				{
					InstanceId = instanceId,
					CurrentState = StatePending,
					PreviousState = _instances[instanceId].State
				});
				_instances[instanceId].State = StatePending;
			}
		}
		
		return Task.FromResult(new StartInstancesResponse
		{
			StartingInstances = stateChanges,
			HttpStatusCode = HttpStatusCode.OK
		});
	}
	
	private Task<ModifyInstanceAttributeResponse> ModifyInstanceAttributeAsync(ModifyInstanceAttributeRequest request, CancellationToken cancellationToken)
	{
		if (!_instances.ContainsKey(request.InstanceId))
		{
			throw new ArgumentException("Unknown instance ID " + request.InstanceId);			
		}

		_instances[request.InstanceId].InstanceType = request.InstanceType;
		
		return Task.FromResult(new ModifyInstanceAttributeResponse()
		{
			ContentLength = 0,
			HttpStatusCode = HttpStatusCode.OK
		});
	}
}