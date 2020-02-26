// Copyright Epic Games, Inc. All Rights Reserved.
#include "CpuProfilerTraceAnalysis.h"
#include "AnalysisServicePrivate.h"
#include "Common/Utils.h"
#include "TraceServices/Model/Threads.h"

FCpuProfilerAnalyzer::FCpuProfilerAnalyzer(Trace::IAnalysisSession& InSession, Trace::FTimingProfilerProvider& InTimingProfilerProvider)
	: Session(InSession)
	, TimingProfilerProvider(InTimingProfilerProvider)
{

}

FCpuProfilerAnalyzer::~FCpuProfilerAnalyzer()
{
	for (auto& KV : ThreadStatesMap)
	{
		FThreadState* ThreadState = KV.Value;
		delete ThreadState;
	}
}

void FCpuProfilerAnalyzer::OnAnalysisBegin(const FOnAnalysisContext& Context)
{
	auto& Builder = Context.InterfaceBuilder;

	Builder.RouteEvent(RouteId_EventSpec, "CpuProfiler", "EventSpec");
	Builder.RouteEvent(RouteId_EventBatch, "CpuProfiler", "EventBatch");
	Builder.RouteEvent(RouteId_EndCapture, "CpuProfiler", "EndCapture");
	Builder.RouteEvent(RouteId_ChannelAnnounce, "Trace", "ChannelAnnounce");
	Builder.RouteEvent(RouteId_ChannelToggle, "Trace", "ChannelToggle");
}

bool FCpuProfilerAnalyzer::OnEvent(uint16 RouteId, const FOnEventContext& Context)
{
	Trace::FAnalysisSessionEditScope _(Session);

	const auto& EventData = Context.EventData;
	switch (RouteId)
	{
	case RouteId_EventSpec:
	{
		uint32 SpecId = EventData.GetValue<uint32>("Id");
		uint8 CharSize = EventData.GetValue<uint8>("CharSize");
		if (CharSize == sizeof(ANSICHAR))
		{
			DefineScope(SpecId, Session.StoreString(StringCast<TCHAR>(reinterpret_cast<const ANSICHAR*>(EventData.GetAttachment())).Get()));
		}
		else if (CharSize == 0 || CharSize == sizeof(TCHAR)) // 0 for backwards compatibility
		{
			DefineScope(SpecId, Session.StoreString(reinterpret_cast<const TCHAR*>(EventData.GetAttachment())));
		}
		break;
	}
	case RouteId_EventBatch:
	case RouteId_EndCapture:
	{
		TotalEventSize += EventData.GetAttachmentSize();
		uint32 ThreadId = EventData.GetValue<uint32>("ThreadId");
		FThreadState& ThreadState = GetThreadState(ThreadId);
		uint64 LastCycle = ThreadState.LastCycle;
		uint64 BufferSize = EventData.GetAttachmentSize();
		const uint8* BufferPtr = EventData.GetAttachment();
		const uint8* BufferEnd = BufferPtr + BufferSize;
		while (BufferPtr < BufferEnd)
		{
			uint64 DecodedCycle = FTraceAnalyzerUtils::Decode7bit(BufferPtr);
			uint64 ActualCycle = (DecodedCycle >> 1) + LastCycle;
			LastCycle = ActualCycle;
			if (DecodedCycle & 1ull)
			{
				EventScopeState& ScopeState = ThreadState.ScopeStack.AddDefaulted_GetRef();
				ScopeState.StartCycle = ActualCycle;
				uint32 SpecId = FTraceAnalyzerUtils::Decode7bit(BufferPtr);
				uint32* FindIt = SpecIdToTimerIdMap.Find(SpecId);
				if (!FindIt)
				{
					ScopeState.EventTypeId = SpecIdToTimerIdMap.Add(SpecId, TimingProfilerProvider.AddCpuTimer(TEXT("<unknown>")));
				}
				else
				{
					ScopeState.EventTypeId = *FindIt;
				}
				Trace::FTimingProfilerEvent Event;
				Event.TimerIndex = ScopeState.EventTypeId;
				ThreadState.Timeline->AppendBeginEvent(Context.SessionContext.TimestampFromCycle(ScopeState.StartCycle), Event);
				++TotalScopeCount;
			}
			else if (ThreadState.ScopeStack.Num())
			{
				ThreadState.ScopeStack.Pop();
				ThreadState.Timeline->AppendEndEvent(Context.SessionContext.TimestampFromCycle(ActualCycle));
			}
		}
		check(BufferPtr == BufferEnd);
		if (LastCycle)
		{
			double LastTimestamp = Context.SessionContext.TimestampFromCycle(LastCycle);
			Session.UpdateDurationSeconds(LastTimestamp);
			if (RouteId == RouteId_EndCapture)
			{

				while (ThreadState.ScopeStack.Num())
				{
					ThreadState.ScopeStack.Pop();
					ThreadState.Timeline->AppendEndEvent(LastTimestamp);
				}
			}
		}
		ThreadState.LastCycle = LastCycle;
		BytesPerScope = double(TotalEventSize) / double(TotalScopeCount);
		break;
	}
	case RouteId_ChannelAnnounce:
	{
		const ANSICHAR* ChannelName = (ANSICHAR*)Context.EventData.GetAttachment();
		const uint32 ChannelId = Context.EventData.GetValue<uint32>("Id");
		if (FCStringAnsi::Stricmp(ChannelName, "cpu") == 0)
		{
			CpuChannelId = ChannelId;
		}
		break;
	}
	case RouteId_ChannelToggle:
	{
		const uint32 ChannelId = Context.EventData.GetValue<uint32>("Id");
		const bool bEnabled = Context.EventData.GetValue<bool>("IsEnabled");

		if (ChannelId == CpuChannelId && bCpuChannelState != bEnabled)
		{
			bCpuChannelState = bEnabled;
			if (bEnabled == false)
			{
				for (auto ThreadPair : ThreadStatesMap)
				{
					FThreadState& ThreadState = *ThreadPair.Value;
					const double Timestamp = Context.SessionContext.TimestampFromCycle(ThreadState.LastCycle);
					Session.UpdateDurationSeconds(Timestamp);
					while (ThreadState.ScopeStack.Num())
					{
						ThreadState.ScopeStack.Pop();
						ThreadState.Timeline->AppendEndEvent(Timestamp);
					}
				}
			}
		}
		break;
	}
	}

	return true;
}

void FCpuProfilerAnalyzer::DefineScope(uint32 SpecId, const TCHAR* Name)
{
	uint32* FindTimerIdByName = ScopeNameToTimerIdMap.Find(Name);
	if (FindTimerIdByName)
	{
		SpecIdToTimerIdMap.Add(SpecId, *FindTimerIdByName);
	}
	else
	{
		uint32* FindTimerId = SpecIdToTimerIdMap.Find(SpecId);
		if (FindTimerId)
		{
			TimingProfilerProvider.SetTimerName(*FindTimerId, Name);
			ScopeNameToTimerIdMap.Add(Name, *FindTimerId);
		}
		else
		{
			uint32 NewTimerId = TimingProfilerProvider.AddCpuTimer(Name);
			SpecIdToTimerIdMap.Add(SpecId, NewTimerId);
			ScopeNameToTimerIdMap.Add(Name, NewTimerId);
		}
	}
}

FCpuProfilerAnalyzer::FThreadState& FCpuProfilerAnalyzer::GetThreadState(uint32 ThreadId)
{
	FThreadState* ThreadState = ThreadStatesMap.FindRef(ThreadId);
	if (!ThreadState)
	{
		ThreadState = new FThreadState();
		ThreadState->Timeline = &TimingProfilerProvider.EditCpuThreadTimeline(ThreadId);
		ThreadStatesMap.Add(ThreadId, ThreadState);
	}
	return *ThreadState;
}

