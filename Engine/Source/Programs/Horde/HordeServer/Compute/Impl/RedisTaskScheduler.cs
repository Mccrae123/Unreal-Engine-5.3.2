// Copyright Epic Games, Inc. All Rights Reserved.

using EpicGames.Core;
using EpicGames.Horde.Compute;
using EpicGames.Redis;
using EpicGames.Serialization;
using HordeServer.Models;
using HordeServer.Services;
using HordeServer.Storage;
using HordeServer.Utilities;
using Microsoft.Extensions.Caching.Memory;
using Microsoft.Extensions.Logging;
using StackExchange.Redis;
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Threading;
using System.Threading.Channels;
using System.Threading.Tasks;

namespace HordeServer.Compute.Impl
{
	using NamespaceId = StringId<INamespace>;
	using Condition = StackExchange.Redis.Condition;

	/// <summary>
	/// Interface for a queue of compute operations, each of which may have different requirements for the executing machines.
	/// </summary>
	/// <typeparam name="TQueueId">Type used to identify a particular queue</typeparam>
	/// <typeparam name="TTask">Type used to describe a task to be performed</typeparam>
	interface ITaskScheduler<TQueueId, TTask> 
		where TQueueId : struct 
		where TTask : class
	{
		/// <summary>
		/// Adds a task to a queue
		/// </summary>
		/// <param name="QueueId">The queue identifier</param>
		/// <param name="TaskId">The task to add</param>
		Task EnqueueAsync(TQueueId QueueId, TTask TaskId);

		/// <summary>
		/// Inserts a previously dequeued task back at the front of the queue
		/// </summary>
		/// <param name="QueueId">The queue identifier</param>
		/// <param name="Task">The task to add</param>
		Task RequeueAsync(TQueueId QueueId, TTask Task);

		/// <summary>
		/// Dequeue any task from a particular queue
		/// </summary>
		/// <param name="QueueId">The queue to remove a task from</param>
		/// <returns>Information about the task to be executed</returns>
		Task<TTask?> DequeueAsync(TQueueId QueueId);

		/// <summary>
		/// Dequeues a task that the given agent can execute
		/// </summary>
		/// <param name="Predicate">Predicate for determining which queues can be removed from</param>
		/// <param name="Token">Cancellation token for the operation. Will return a null entry rather than throwing an exception.</param>
		/// <returns>Information about the task to be executed</returns>
		Task<(TQueueId, TTask)?> DequeueAsync(Func<TQueueId, ValueTask<bool>> Predicate, CancellationToken Token = default);

		/// <summary>
		/// Gets hashes of all the inactive task queues
		/// </summary>
		/// <returns></returns>
		Task<List<TQueueId>> GetInactiveQueuesAsync();
	}

	/// <summary>
	/// Implementation of <see cref="ITaskScheduler{TQueue, TTask}"/> using Redis for storage
	/// </summary>
	/// <typeparam name="TQueueId">Type used to identify a particular queue</typeparam>
	/// <typeparam name="TTask">Type used to describe a task to be performed</typeparam>
	class RedisTaskScheduler<TQueueId, TTask> : ITaskScheduler<TQueueId, TTask>, IDisposable 
		where TQueueId : struct 
		where TTask : class
	{
		class Listener
		{
			public Func<TQueueId, ValueTask<bool>> Predicate;
			public TaskCompletionSource<(TQueueId, TTask)?> CompletionSource { get; }

			public Listener(Func<TQueueId, ValueTask<bool>> Predicate)
			{
				this.Predicate = Predicate;
				this.CompletionSource = new TaskCompletionSource<(TQueueId, TTask)?>();
			}
		}

		IDatabase Redis;
		RedisKey BaseKey;
		RedisSet<TQueueId> QueueIndex;
		RedisHash<TQueueId, DateTime> ActiveQueues; // Queues which are actively being dequeued from
		ReadOnlyHashSet<TQueueId> LocalActiveQueues = new HashSet<TQueueId>();
		Stopwatch ResetActiveQueuesTimer = Stopwatch.StartNew();

		List<Listener> Listeners = new List<Listener>();
		RedisChannel<TQueueId> NewQueueChannel;
		Task QueueUpdateTask;
		CancellationTokenSource CancellationSource = new CancellationTokenSource();
		ILogger Logger;

		/// <summary>
		/// Constructor
		/// </summary>
		/// <param name="Redis">The Redis database instance</param>
		/// <param name="BaseKey">Base key for all keys used by this scheduler</param>
		/// <param name="Logger"></param>
		public RedisTaskScheduler(IDatabase Redis, RedisKey BaseKey, ILogger Logger)
		{
			this.Redis = Redis;
			this.BaseKey = BaseKey;
			this.QueueIndex = new RedisSet<TQueueId>(BaseKey.Append("index"));
			this.ActiveQueues = new RedisHash<TQueueId, DateTime>(BaseKey.Append("active"));
			this.NewQueueChannel = new RedisChannel<TQueueId>(BaseKey.Append("new_queues").ToString());
			this.Logger = Logger;

			QueueUpdateTask = Task.Run(() => UpdateQueuesAsync(CancellationSource.Token));
		}

		/// <inheritdoc/>
		public async ValueTask DisposeAsync()
		{
			if (!QueueUpdateTask.IsCompleted)
			{
				CancellationSource.Cancel();
				await QueueUpdateTask;
			}
			CancellationSource.Dispose();
		}

		/// <inheritdoc/>
		public void Dispose()
		{
			DisposeAsync().AsTask().Wait();
		}

		/// <summary>
		/// Gets the key for a list of tasks for a particular queue
		/// </summary>
		/// <param name="QueueId">The queue identifier</param>
		/// <returns></returns>
		RedisKey GetQueueKey(TQueueId QueueId)
		{
			return BaseKey.Append(RedisSerializer.Serialize(QueueId).AsKey());
		}

		/// <summary>
		/// Gets the key for a list of tasks for a particular queue
		/// </summary>
		/// <param name="QueueId">The queue identifier</param>
		/// <returns></returns>
		RedisList<TTask> GetQueue(TQueueId QueueId)
		{
			return new RedisList<TTask>(GetQueueKey(QueueId));
		}

		/// <summary>
		/// Adds a queue to the index, atomically checking that the queue exists
		/// </summary>
		/// <param name="QueueId">The queue to add to the index</param>
		async ValueTask AddQueueToIndexAsync(TQueueId QueueId)
		{
			ITransaction Transaction = Redis.CreateTransaction();
			Transaction.AddCondition(Condition.KeyExists(GetQueueKey(QueueId)));
			_ = Transaction.SetAddAsync(QueueIndex, QueueId);
			await Transaction.ExecuteAsync();

			await Redis.PublishAsync(NewQueueChannel, QueueId);
		}

		/// <summary>
		/// Removes a queue from the index, atomically checking that it is empty
		/// </summary>
		/// <param name="QueueId">The queue to remove</param>
		async ValueTask RemoveQueueFromIndexAsync(TQueueId QueueId)
		{
			ITransaction Transaction = Redis.CreateTransaction();
			Transaction.AddCondition(Condition.KeyNotExists(GetQueueKey(QueueId)));
			_ = Transaction.SetRemoveAsync(QueueIndex, QueueId);
			await Transaction.ExecuteAsync(CommandFlags.FireAndForget);
		}

		/// <summary>
		/// Adds a task to a particular queue, creating and adding that queue to the index if necessary
		/// </summary>
		/// <param name="QueueId">The queue to add the task to</param>
		/// <param name="Task">Task to be scheduled</param>
		public async Task EnqueueAsync(TQueueId QueueId, TTask Task)
		{
			RedisList<TTask> List = GetQueue(QueueId);

			long NewLength = await Redis.ListRightPushAsync(List, Task);
			if (NewLength == 1)
			{
				await AddQueueToIndexAsync(QueueId);
			}
		}

		/// <summary>
		/// Adds an item to the front of a particular queue, creating and adding that queue to the index if necessary
		/// </summary>
		/// <param name="QueueId">The queue to add the task to</param>
		/// <param name="Task">Task to be scheduled</param>
		public async Task RequeueAsync(TQueueId QueueId, TTask Task)
		{
			RedisList<TTask> List = GetQueue(QueueId);

			long NewLength = await Redis.ListLeftPushAsync(List, Task);
			if (NewLength == 1)
			{
				await AddQueueToIndexAsync(QueueId);
			}
		}

		/// <summary>
		/// Dequeues an item for execution by the given agent
		/// </summary>
		/// <param name="Predicate">Predicate for queues that tasks can be removed from</param>
		/// <param name="Token">Cancellation token for waiting for an item</param>
		/// <returns>The dequeued item, or null if no item is available</returns>
		public async Task<(TQueueId, TTask)?> DequeueAsync(Func<TQueueId, ValueTask<bool>> Predicate, CancellationToken Token = default)
		{
			// Compare against all the list of cached queues to see if we can dequeue something from any of them
			Listener? Listener = null;
			try
			{
				TQueueId[] Queues = await Redis.SetMembersAsync(QueueIndex);
				while (!Token.IsCancellationRequested)
				{
					// Try to dequeue an item from the list
					(TQueueId, TTask)? Entry = await TryAssignToLocalAgentAsync(Queues, Predicate);
					if (Entry != null)
					{
						return Entry;
					}

					// Create and register a listener for this waiter 
					if (Listener == null)
					{
						Listener = new Listener(Predicate);
						lock (Listeners)
						{
							Listeners.Add(Listener);
						}
					}
					else
					{
						using (IDisposable Registration = Token.Register(() => Listener.CompletionSource.TrySetResult(null)))
						{
							return await Listener.CompletionSource.Task;
						}
					}
				}
			}
			finally
			{
				if (Listener != null)
				{
					lock (Listeners)
					{
						Listeners.Remove(Listener);
					}
				}
			}
			return null;
		}

		/// <summary>
		/// Attempts to dequeue a task from a set of queue
		/// </summary>
		/// <param name="QueueIds">The current array of queues</param>
		/// <param name="Predicate">Predicate for queues that tasks can be removed from</param>
		/// <returns>The dequeued item, or null if no item is available</returns>
		async Task<(TQueueId, TTask)?> TryAssignToLocalAgentAsync(TQueueId[] QueueIds, Func<TQueueId, ValueTask<bool>> Predicate)
		{
			foreach (TQueueId QueueId in QueueIds)
			{
				if (await Predicate(QueueId))
				{
					TTask? Task = await DequeueAsync(QueueId);
					if (Task != null)
					{
						return (QueueId, Task);
					}
				}
			}
			return null;
		}

		/// <summary>
		/// Dequeues the item at the front of a queue
		/// </summary>
		/// <param name="QueueId">The queue to dequeue from</param>
		/// <returns>The dequeued item, or null if the queue is empty</returns>
		public async Task<TTask?> DequeueAsync(TQueueId QueueId)
		{
			await AddActiveQueue(QueueId);

			TTask? Item = await Redis.ListLeftPopAsync(GetQueue(QueueId));
			if (Item == null)
			{
				await RemoveQueueFromIndexAsync(QueueId);
			}
			return Item;
		}

		/// <summary>
		/// Marks a queue key as being actively monitored, preventing it being returned by <see cref="GetInactiveQueuesAsync"/>
		/// </summary>
		/// <param name="QueueId">The queue key</param>
		/// <returns></returns>
		async ValueTask AddActiveQueue(TQueueId QueueId)
		{
			// Periodically clear out the set of active keys
			TimeSpan ResetTime = TimeSpan.FromSeconds(10.0);
			if (ResetActiveQueuesTimer.Elapsed > ResetTime)
			{
				lock (ResetActiveQueuesTimer)
				{
					if (ResetActiveQueuesTimer.Elapsed > ResetTime)
					{
						LocalActiveQueues = new HashSet<TQueueId>();
						ResetActiveQueuesTimer.Reset();
					}
				}
			}

			// Check if the set of active keys already contains the key we're adding. In order to optimize the 
			// common case under heavy load where the key is in the set, updating it creates a full copy of it. Any
			// readers can thus access it without the need for any locking.
			ReadOnlyHashSet<TQueueId> LocalActiveKeysCopy = LocalActiveQueues;
			if (!LocalActiveKeysCopy.Contains(QueueId))
			{
				for (; ; )
				{
					HashSet<TQueueId> NewLocalActiveKeys = new HashSet<TQueueId>(LocalActiveKeysCopy);
					if (!NewLocalActiveKeys.Add(QueueId))
					{
						break;
					}
					if (Interlocked.CompareExchange(ref LocalActiveQueues, NewLocalActiveKeys, LocalActiveKeysCopy) == LocalActiveKeysCopy)
					{
						await Redis.HashSetAsync(ActiveQueues, QueueId, DateTime.UtcNow);
						break;
					}
				}
			}
		}

		/// <summary>
		/// Find any inactive keys
		/// </summary>
		/// <returns></returns>
		public async Task<List<TQueueId>> GetInactiveQueuesAsync()
		{
			HashSet<TQueueId> Keys = new HashSet<TQueueId>(await Redis.SetMembersAsync(QueueIndex));
			HashSet<TQueueId> InvalidKeys = new HashSet<TQueueId>();

			DateTime MinTime = DateTime.UtcNow - TimeSpan.FromMinutes(1.0);

			HashEntry<TQueueId, DateTime>[] Entries = await Redis.HashGetAllAsync(ActiveQueues);
			foreach (HashEntry<TQueueId, DateTime> Entry in Entries)
			{
				if (Entry.Value < MinTime)
				{
					InvalidKeys.Add(Entry.Name);
				}
				else
				{
					Keys.Remove(Entry.Name);
				}
			}

			if (InvalidKeys.Count > 0)
			{
				await Redis.HashDeleteAsync(ActiveQueues, InvalidKeys.ToArray());
			}

			return Keys.ToList();
		}

		async Task UpdateQueuesAsync(CancellationToken CancellationToken)
		{
			Channel<TQueueId> NewQueues = Channel.CreateUnbounded<TQueueId>();

			ISubscriber Subscriber = Redis.Multiplexer.GetSubscriber();
			await using var _ = await Subscriber.SubscribeAsync(NewQueueChannel, (_, v) => NewQueues.Writer.TryWrite(v));

			while (await NewQueues.Reader.WaitToReadAsync(CancellationToken))
			{
				HashSet<TQueueId> NewQueueIds = new HashSet<TQueueId>();
				while (NewQueues.Reader.TryRead(out TQueueId QueueId))
				{
					NewQueueIds.Add(QueueId);
				}
				foreach (TQueueId NewQueueId in NewQueueIds)
				{
					await TryDispatchToNewQueueAsync(NewQueueId);
				}
			}
		}

		async Task<bool> TryDispatchToNewQueueAsync(TQueueId QueueId)
		{
			RedisList<TTask> Queue = GetQueue(QueueId);

			// Find a local listener that can execute the work
			(TQueueId QueueId, TTask TaskId)? Entry = null;
			try
			{
				for (; ; )
				{
					Listener? Listener = null;

					// Look for a listener that can execute the task
					HashSet<Listener> CheckedListeners = new HashSet<Listener>();
					while(Listener == null)
					{
						// Find up to 10 listeners we haven't seen before
						List<Listener> NewListeners = new List<Listener>();
						lock (Listeners)
						{
							NewListeners.AddRange(Listeners.Where(x => !x.CompletionSource.Task.IsCompleted && CheckedListeners.Add(x)).Take(10));
						}
						if (NewListeners.Count == 0)
						{
							return false;
						}

						// Check predicates for each one against the new queue
						foreach (Listener NewListener in NewListeners)
						{
							if (await NewListener.Predicate(QueueId))
							{
								Listener = NewListener;
								break;
							}
						}
					}

					// Pop an entry from the queue
					if (Entry == null)
					{
						TTask? Task = await DequeueAsync(QueueId);
						if (Task == null)
						{
							return false;
						}
						Entry = (QueueId, Task);
					}

					// Assign it to the listener
					if (Listener.CompletionSource.TrySetResult(Entry))
					{
						Entry = null;
					}
				}
			}
			finally
			{
				if (Entry != null)
				{
					await RequeueAsync(Entry.Value.QueueId, Entry.Value.TaskId);
				}
			}
		}
	}
}
