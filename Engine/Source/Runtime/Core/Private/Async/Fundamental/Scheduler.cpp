// Copyright Epic Games, Inc. All Rights Reserved.

#include "Async/Fundamental/Scheduler.h"
#include "Async/Fundamental/Task.h"
#include "Logging/LogMacros.h"
#include "Misc/ScopeLock.h" 

namespace LowLevelTasks
{
	DEFINE_LOG_CATEGORY(LowLevelTasks);

	thread_local FScheduler::FLocalQueueType* FScheduler::LocalQueue = nullptr;
	thread_local FTask* FScheduler::ActiveTask = nullptr;

	FScheduler FScheduler::Singleton;

	FScheduler::FLocalQueueInstaller::FLocalQueueInstaller(FScheduler& Scheduler)
	{
		RegisteredLocalQueue = LocalQueue == nullptr;
		if (RegisteredLocalQueue)
		{
			LocalQueue = FLocalQueueType::AllocateLocalQueue(Scheduler.QueueRegistry);
		}
	}

	FScheduler::FLocalQueueInstaller::~FLocalQueueInstaller()
	{
		if (RegisteredLocalQueue)
		{
			FLocalQueueType::DeleteLocalQueue(LocalQueue);
			LocalQueue = nullptr;
		}
	}

	TUniquePtr<FThread> FScheduler::CreateWorker(FLocalQueueType* ExternalWorkerLocalQueue, EThreadPriority Priority, bool bIsForkable)
	{
		uint32 WorkerId = NextWorkerId++;
		const uint32 WaitTimes[8] = { 23, 31, 41, 37, 47, 29, 19, 43 };
		uint32 WaitTime = WaitTimes[WorkerId % 8];
		uint64 ThreadAffinityMask = FPlatformAffinity::GetTaskGraphThreadMask();
		return MakeUnique<FThread>
		(
			*FString::Printf(TEXT("Task Worker #%d"), WorkerId),
			[this, ExternalWorkerLocalQueue, WaitTime]
			{ 
				FSleepEvent Event;
				WorkerMain(&Event, ExternalWorkerLocalQueue, WaitTime);
			}, 0, Priority, ThreadAffinityMask
		);
	}

	void FScheduler::StartWorkers(uint32 NumWorkers, EThreadPriority Priority, bool bIsForkable)
	{
		NumWorkers = NumWorkers == 0 ? FPlatformMisc::NumberOfWorkerThreadsToSpawn() : NumWorkers;

		uint32 OldActiveWorkers = ActiveWorkers.load(std::memory_order_relaxed);
		if(OldActiveWorkers == 0 && FPlatformProcess::SupportsMultithreading() && ActiveWorkers.compare_exchange_strong(OldActiveWorkers, NumWorkers, std::memory_order_relaxed))
		{
			UE::Trace::ThreadGroupBegin(TEXT("Task Workers"));
			FScopeLock Lock(&WorkerThreadsCS);
			check(!WorkerThreads.Num());
			check(!WorkerLocalQueues.Num());
			check(NextWorkerId == 0);

			WorkerThreads.Reserve(NumWorkers);
			WorkerLocalQueues.Reserve(NumWorkers);
			for (uint32 WorkerId = 0; WorkerId < NumWorkers; ++WorkerId)
			{
				WorkerLocalQueues.Emplace(QueueRegistry);
				WorkerThreads.Add(CreateWorker(&WorkerLocalQueues.Last(), Priority, bIsForkable));
			}
			UE::Trace::ThreadGroupEnd();
		}
	}

	void FScheduler::StopWorkers()
	{
		uint32 OldActiveWorkers = ActiveWorkers.load(std::memory_order_relaxed);
		if(OldActiveWorkers != 0 && ActiveWorkers.compare_exchange_strong(OldActiveWorkers, 0, std::memory_order_relaxed))
		{
			FScopeLock Lock(&WorkerThreadsCS);
			while (WakeUpWorker())
			{
			}
			for (TUniquePtr<FThread>& Thread : WorkerThreads)
			{
				Thread->Join();
			}
			NextWorkerId = 0;
			WorkerThreads.Reset();
			WorkerLocalQueues.Reset();
			for (FTask* Task = QueueRegistry.Dequeue(); Task != nullptr; Task = QueueRegistry.Dequeue())
			{
				Task->ExecuteTask();
			}
		}
	}

	void FScheduler::LaunchInternal(FTask& Task, EQueuePreference QueuePreference)
	{
		if (ActiveWorkers.load(std::memory_order_relaxed))
		{
			if (LocalQueue && QueuePreference != EQueuePreference::GlobalQueuePreference)
			{
				if (LocalQueue->Enqueue(&Task, uint32(Task.GetPriority())))
				{
					WakeUpWorker();
				}
			}
			else
			{
				if (QueueRegistry.Enqueue(&Task, uint32(Task.GetPriority())))
				{
					WakeUpWorker();
				}
			}
		}
		else
		{
			Task.ExecuteTask();
		}
	}

	const FTask* FScheduler::GetActiveTask() const
	{
		return ActiveTask;
	}

	template<FTask* (FScheduler::FLocalQueueType::*DequeueFunction)()>
	FORCEINLINE_DEBUGGABLE bool FScheduler::TryExecuteTaskFrom(FLocalQueueType* Queue, FQueueRegistry::FOutOfWork& OutOfWork)
	{
		FTask* Task = (Queue->*DequeueFunction)();
		if (Task)
		{	
			OutOfWork.Stop();
			FTask* OldTask = ActiveTask;
			ActiveTask = Task;
			Task->ExecuteTask();
			ActiveTask = OldTask;
			return true;
		}
		return false;
	}

	void FScheduler::WorkerMain(FSleepEvent* WorkerEvent, FLocalQueueType* ExternalWorkerLocalQueue, uint32 WaitCycles)
	{
		FMemory::SetupTLSCachesOnCurrentThread();

		checkSlow(LocalQueue == nullptr);
		if(ExternalWorkerLocalQueue)
		{
			LocalQueue = ExternalWorkerLocalQueue;
		}
		else
		{
			LocalQueue = FLocalQueueType::AllocateLocalQueue(QueueRegistry);
		}
		FLocalQueueType* WorkerLocalQueue = LocalQueue;

		bool Drowsing = false;
		uint32 WaitCount = 0;
		FQueueRegistry::FOutOfWork OutOfWork = QueueRegistry.GetOutOfWorkScope();
		while (true)
		{
			while(TryExecuteTaskFrom<&FLocalQueueType::DequeueLocal>(WorkerLocalQueue, OutOfWork)
			   || TryExecuteTaskFrom<&FLocalQueueType::DequeueGlobal>(WorkerLocalQueue, OutOfWork))
			{			
				Drowsing = false;
				WaitCount = 0;
			}

			while(TryExecuteTaskFrom<&FLocalQueueType::DequeueLocal>(WorkerLocalQueue, OutOfWork)
			   || TryExecuteTaskFrom<&FLocalQueueType::DequeueSteal>(WorkerLocalQueue, OutOfWork))
			{
				Drowsing = false;
				WaitCount = 0;
			}

			if (ActiveWorkers.load(std::memory_order_relaxed) == 0)
			{
				break;
			}

			if (WaitCount < WorkerSpinCycles)
			{
				OutOfWork.Start();			
				for (uint32 i = 0; i < WaitCycles; i++)
				{
					FPlatformProcess::Yield();
				}
				WaitCount++;
				continue;
			}

			TrySleeping(WorkerEvent, OutOfWork, WaitCount, Drowsing);
		}

		while (WakeUpWorker())
		{
		}

		FLocalQueueType::DeleteLocalQueue(WorkerLocalQueue, ExternalWorkerLocalQueue != nullptr);
		LocalQueue = nullptr;

		FMemory::ClearAndDisableTLSCachesOnCurrentThread();
	}

	void FScheduler::BusyWaitInternal(const FConditional& Conditional)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FScheduler::BusyWaitInternal);

		checkSlow(LocalQueue != nullptr);
		check(ActiveWorkers.load(std::memory_order_relaxed));
		FLocalQueueType* WorkerLocalQueue = LocalQueue;

		uint32 WaitCount = 0;
		FQueueRegistry::FOutOfWork OutOfWork = QueueRegistry.GetOutOfWorkScope();
		while (true)
		{
			while(TryExecuteTaskFrom<&FLocalQueueType::DequeueLocal>(WorkerLocalQueue, OutOfWork)
			   || TryExecuteTaskFrom<&FLocalQueueType::DequeueGlobal>(WorkerLocalQueue, OutOfWork))
			{
				if (Conditional())
				{
					return;
				}
				WaitCount = 0;
			}

			while(TryExecuteTaskFrom<&FLocalQueueType::DequeueLocal>(WorkerLocalQueue, OutOfWork)
			   || TryExecuteTaskFrom<&FLocalQueueType::DequeueSteal>(WorkerLocalQueue, OutOfWork))
			{
				if (Conditional())
				{
					return;
				}
				WaitCount = 0;
			}

			if (Conditional())
			{
				return;
			}

			if (WaitCount < WorkerSpinCycles)
			{
				FPlatformProcess::Yield();
				FPlatformProcess::Yield();
				WaitCount++;
			}
			else
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(BusyWaitInternal::SleepNoStats);
				FPlatformProcess::SleepNoStats(0);
				WaitCount = 0;
			}
		}
	}
}