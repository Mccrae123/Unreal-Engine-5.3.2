// Copyright Epic Games, Inc. All Rights Reserved.

#include "Tasks/TaskPrivate.h"
#include "Tasks/Pipe.h"

#if TASKGRAPH_NEW_FRONTEND
#include "Async/TaskGraphInterfaces.h"
#endif

namespace UE::Tasks
{
	namespace Private
	{
		FExecutableTaskAllocator SmallTaskAllocator;
		FTaskEventBaseAllocator TaskEventBaseAllocator;

		void FTaskBase::Schedule()
		{
			TaskTrace::Scheduled(GetTraceId());

#if TASKGRAPH_NEW_FRONTEND
			if (IsNamedThreadTask())
			{
				ENamedThreads::Type ConversionMap[] =
				{
					ENamedThreads::GameThread,
					(ENamedThreads::Type)(ENamedThreads::GameThread | ENamedThreads::HighTaskPriority),
					(ENamedThreads::Type)(ENamedThreads::GameThread | ENamedThreads::LocalQueue),
					(ENamedThreads::Type)(ENamedThreads::GameThread | ENamedThreads::HighTaskPriority | ENamedThreads::LocalQueue),

					ENamedThreads::GetRenderThread(),
					(ENamedThreads::Type)(ENamedThreads::GetRenderThread() | ENamedThreads::HighTaskPriority),
					(ENamedThreads::Type)(ENamedThreads::GetRenderThread() | ENamedThreads::LocalQueue),
					(ENamedThreads::Type)(ENamedThreads::GetRenderThread() | ENamedThreads::HighTaskPriority | ENamedThreads::LocalQueue),

					ENamedThreads::RHIThread,
					(ENamedThreads::Type)(ENamedThreads::RHIThread | ENamedThreads::HighTaskPriority),
					(ENamedThreads::Type)(ENamedThreads::RHIThread | ENamedThreads::LocalQueue),
					(ENamedThreads::Type)(ENamedThreads::RHIThread | ENamedThreads::HighTaskPriority | ENamedThreads::LocalQueue)
				};

				FTaskGraphInterface::Get().QueueTask(static_cast<FBaseGraphTask*>(this), true, ConversionMap[(int32)ExtendedPriority - (int32)EExtendedTaskPriority::GameThreadNormalPri]);
				return;
			}
#endif

			LowLevelTasks::FScheduler::Get().TryLaunch(LowLevelTask, LowLevelTasks::EQueuePreference::GlobalQueuePreference, /*bWakeUpWorker=*/ true);
		}

		void FTaskBase::Wait()
		{
			if (IsCompleted())
			{
				return;
			}

			TaskTrace::FWaitingScope WaitingScope(GetTraceId());
			TRACE_CPUPROFILER_EVENT_SCOPE(Tasks::Wait);

			if (!IsAwaitable())
			{
				UE_LOG(LogTemp, Fatal, TEXT("Deadlock detected! A task can't be waited here, e.g. because it's being executed by the currect thread"));
				return;
			}

			if (TryRetractAndExecute())
			{
				return;
			}

			// if we are on a named thread, handle waiting in TaskGraph-specific style
			if (TryWaitOnNamedThread(*this))
			{
				return;
			}

			FEventRef CompletionEvent;
			auto WaitingTaskBody = [&CompletionEvent] { CompletionEvent->Trigger(); };
			using FWaitingTask = TExecutableTask<decltype(WaitingTaskBody)>;

			// the task is stored on the stack as we can guarantee that it's out of the system by the end of the call
			FWaitingTask WaitingTask{ TEXT("Waiting Task"), MoveTemp(WaitingTaskBody), ETaskPriority::Default /* doesn't matter*/, EExtendedTaskPriority::Inline };
			WaitingTask.AddPrerequisites(*this);

			if (WaitingTask.TryLaunch())
			{	// was executed inline
				check(WaitingTask.IsCompleted());
			}
			else
			{
				CompletionEvent->Wait();
			}

			// the waiting task will be destroyed leaving this scope, wait for the internal reference to it to be released
			while (WaitingTask.GetRefCount() != 1)
			{
				FPlatformProcess::Yield();
			}
		}

		bool FTaskBase::Wait(FTimespan InTimeout)
		{
			TaskTrace::FWaitingScope WaitingScope(GetTraceId());
			TRACE_CPUPROFILER_EVENT_SCOPE(Tasks::Wait);

			FTimeout Timeout{ InTimeout };

			if (TryRetractAndExecute())
			{
				return true;
			}

			if (GetCurrentTask() == this)
			{
				UE_LOG(LogTemp, Fatal, TEXT("A task waiting for itself detected"));
				return true;
			}

			// the event must be alive for the task and this function lifetime, we don't know which one will be finished first as waiting can 
			// time out before the waiting task is completed
			FSharedEventRef CompletionEvent;
			auto WaitingTaskBody = [CompletionEvent] { CompletionEvent->Trigger(); };
			using FWaitingTask = TExecutableTask<decltype(WaitingTaskBody)>;

			TRefCountPtr<FWaitingTask> WaitingTask{ FWaitingTask::Create(TEXT("Waiting Task"), MoveTemp(WaitingTaskBody), ETaskPriority::Default /* doesn't matter*/, EExtendedTaskPriority::Inline), /*bAddRef=*/ false };
			WaitingTask->AddPrerequisites(*this);

			if (WaitingTask->TryLaunch())
			{	// was executed inline
				check(WaitingTask->IsCompleted());
				return true;
			}

			return CompletionEvent->Wait((uint32)FMath::Clamp<int64>(Timeout.GetRemainingTime().GetTicks() / ETimespan::TicksPerMillisecond, 0, MAX_uint32));
		}

		FTaskBase* FTaskBase::TryPushIntoPipe()
		{
			return GetPipe()->PushIntoPipe(*this);
		}

		void FTaskBase::StartPipeExecution()
		{
			GetPipe()->ExecutionStarted();
		}

		void FTaskBase::FinishPipeExecution()
		{
			GetPipe()->ExecutionFinished();
		}

		void FTaskBase::ClearPipe()
		{
			GetPipe()->ClearTask(*this);
		}

		static thread_local FTaskBase* CurrentTask = nullptr;

		FTaskBase* GetCurrentTask()
		{
			return CurrentTask;
		}

		FTaskBase* ExchangeCurrentTask(FTaskBase* Task)
		{
			FTaskBase* PrevTask = CurrentTask;
			CurrentTask = Task;
			return PrevTask;
		}

		bool TryWaitOnNamedThread(FTaskBase& Task)
		{
#if TASKGRAPH_NEW_FRONTEND
			// handle waiting only on a named thread and if not called from inside a task
			FTaskGraphInterface& TaskGraph = FTaskGraphInterface::Get();
			ENamedThreads::Type CurrentThread = TaskGraph.GetCurrentThreadIfKnown();
			if (CurrentThread < ENamedThreads::ActualRenderingThread /* is a named thread? */ && !TaskGraph.IsThreadProcessingTasks(CurrentThread))
			{
				// execute other tasks of this named thread while waiting
				ETaskPriority Dummy;
				EExtendedTaskPriority ExtendedPriority;
				FBaseGraphTask::TranslatePriority(CurrentThread, Dummy, ExtendedPriority);

				auto TaskBody = [CurrentThread, &TaskGraph] { TaskGraph.RequestReturn(CurrentThread); };
				using FReturnFromNamedThreadTask = TExecutableTask<decltype(TaskBody)>;
				FReturnFromNamedThreadTask ReturnTask{ TEXT("ReturnFromNamedThreadTask"), MoveTemp(TaskBody), ETaskPriority::High, ExtendedPriority };
				ReturnTask.AddPrerequisites(Task);
				ReturnTask.TryLaunch(); // the result doesn't matter

				TaskGraph.ProcessThreadUntilRequestReturn(CurrentThread);
				return true;
			}
#endif

			return false;
		}
	}

	const TCHAR* ToString(EExtendedTaskPriority ExtendedPriority)
	{
		if (ExtendedPriority < EExtendedTaskPriority::None || ExtendedPriority >= EExtendedTaskPriority::Count)
		{
			return nullptr;
		}

		const TCHAR* ExtendedTaskPriorityToStr[] =
		{
			TEXT("None"),
			TEXT("Inline"),
			TEXT("TaskEvent"),

#if TASKGRAPH_NEW_FRONTEND
			TEXT("GameThreadNormalPri"),
			TEXT("GameThreadHiPri"),
			TEXT("GameThreadNormalPriLocalQueue"),
			TEXT("GameThreadHiPriLocalQueue"),

			TEXT("RenderThreadNormalPri"),
			TEXT("RenderThreadHiPri"),
			TEXT("RenderThreadNormalPriLocalQueue"),
			TEXT("RenderThreadHiPriLocalQueue"),

			TEXT("RHIThreadNormalPri"),
			TEXT("RHIThreadHiPri"),
			TEXT("RHIThreadNormalPriLocalQueue"),
			TEXT("RHIThreadHiPriLocalQueue")
#endif
		};
		return ExtendedTaskPriorityToStr[(int32)ExtendedPriority];
	}

	bool ToExtendedTaskPriority(const TCHAR* ExtendedPriorityStr, EExtendedTaskPriority& OutExtendedPriority)
	{
#define CONVERT_EXTENDED_TASK_PRIORITY(ExtendedTaskPriority)\
		if (FCString::Stricmp(ExtendedPriorityStr, ToString(EExtendedTaskPriority::ExtendedTaskPriority)) == 0)\
		{\
			OutExtendedPriority = EExtendedTaskPriority::ExtendedTaskPriority;\
			return true;\
		}

		CONVERT_EXTENDED_TASK_PRIORITY(None);
		CONVERT_EXTENDED_TASK_PRIORITY(Inline);
		CONVERT_EXTENDED_TASK_PRIORITY(TaskEvent);

#if TASKGRAPH_NEW_FRONTEND
		CONVERT_EXTENDED_TASK_PRIORITY(GameThreadNormalPri);
		CONVERT_EXTENDED_TASK_PRIORITY(GameThreadHiPri);
		CONVERT_EXTENDED_TASK_PRIORITY(GameThreadNormalPriLocalQueue);
		CONVERT_EXTENDED_TASK_PRIORITY(GameThreadHiPriLocalQueue);

		CONVERT_EXTENDED_TASK_PRIORITY(RenderThreadNormalPri);
		CONVERT_EXTENDED_TASK_PRIORITY(RenderThreadHiPri);
		CONVERT_EXTENDED_TASK_PRIORITY(RenderThreadNormalPriLocalQueue);
		CONVERT_EXTENDED_TASK_PRIORITY(RenderThreadHiPriLocalQueue);

		CONVERT_EXTENDED_TASK_PRIORITY(RHIThreadNormalPri);
		CONVERT_EXTENDED_TASK_PRIORITY(RHIThreadHiPri);
		CONVERT_EXTENDED_TASK_PRIORITY(RHIThreadNormalPriLocalQueue);
		CONVERT_EXTENDED_TASK_PRIORITY(RHIThreadHiPriLocalQueue);
#endif

#undef CONVERT_EXTENDED_TASK_PRIORITY

		return false;
	}

	FString FTaskPriorityCVar::CreateFullHelpText(const TCHAR* Name, const TCHAR* OriginalHelp)
	{
		TStringBuilder<1024> TaskPriorities;
		for (int i = 0; i != (int)ETaskPriority::Count; ++i)
		{
			TaskPriorities.Append(ToString((ETaskPriority)i));
			TaskPriorities.Append(TEXT(", "));
		}
		TaskPriorities.RemoveSuffix(2); // remove the last ", "

		TStringBuilder<1024> ExtendedTaskPriorities;
		for (int i = 0; i != (int)EExtendedTaskPriority::Count; ++i)
		{
			ExtendedTaskPriorities.Append(ToString((EExtendedTaskPriority)i));
			ExtendedTaskPriorities.Append(TEXT(", "));
		}
		ExtendedTaskPriorities.RemoveSuffix(2); // remove the last ", "

		return FString::Printf(
			TEXT("%s\n")
			TEXT("Arguments are task priority and extended task priority (optional) separated by a space: [TaskPriority] [ExtendedTaskPriority]\n")
			TEXT("where TaskPriority is in [%s]\n")
			TEXT("and ExtendedTaskPriority is in [%s].\n")
			TEXT("Example: \"%s %s %s\" or \"%s\"")
			, OriginalHelp, *TaskPriorities, *ExtendedTaskPriorities, Name, ToString((ETaskPriority)0), ToString((EExtendedTaskPriority)0), ToString((ETaskPriority)0));
	}

	FString FTaskPriorityCVar::ConfigStringFromPriorities(ETaskPriority InPriority, EExtendedTaskPriority InExtendedPriority)
	{
		return FString{ ToString(InPriority) } + TEXT(" ") + ToString(InExtendedPriority);
	}

	void FTaskPriorityCVar::OnSettingChanged(IConsoleVariable* InVariable)
	{
		FString PriorityStr, ExtendedPriorityStr;
		static FString Delimiter{ " " };
		if (RawSetting.Split(Delimiter, &PriorityStr, &ExtendedPriorityStr))
		{
			verify(ToTaskPriority(*PriorityStr, Priority));
			verify(ToExtendedTaskPriority(*ExtendedPriorityStr, ExtendedPriority));
		}
		else
		{
			verify(ToTaskPriority(*RawSetting, Priority));
			ExtendedPriority = EExtendedTaskPriority::None;
		}
	}
}
