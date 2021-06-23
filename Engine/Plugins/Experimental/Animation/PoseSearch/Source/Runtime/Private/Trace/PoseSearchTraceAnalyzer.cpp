﻿// Copyright Epic Games, Inc. All Rights Reserved.

#include "PoseSearchTraceAnalyzer.h"
#include "PoseSearchTraceProvider.h"
#include "TraceServices/Model/AnalysisSession.h"

namespace UE { namespace PoseSearch {

FTraceAnalyzer::FTraceAnalyzer(TraceServices::IAnalysisSession& InSession, FTraceProvider& InTraceProvider) : Session(InSession), TraceProvider(InTraceProvider)
{
}

void FTraceAnalyzer::OnAnalysisBegin(const FOnAnalysisContext& Context)
{
	FInterfaceBuilder& Builder = Context.InterfaceBuilder;

	ANSICHAR LoggerName[NAME_SIZE];
	ANSICHAR MotionMatchingStateName[NAME_SIZE];

	FTraceLogger::Name.GetPlainANSIString(LoggerName);
	FTraceMotionMatchingState::Name.GetPlainANSIString(MotionMatchingStateName);

	Builder.RouteEvent(RouteId_MotionMatchingState, LoggerName, MotionMatchingStateName);
}

bool FTraceAnalyzer::OnEvent(uint16 RouteId, EStyle Style, const FOnEventContext& Context)
{
	TraceServices::FAnalysisSessionEditScope Scope(Session);
	const FEventData& EventData = Context.EventData;

	// Gather event data values
	const double Time = Context.EventTime.AsSeconds(EventData.GetValue<uint64>("Cycle"));
	const uint16 FrameCounter = EventData.GetValue<uint16>("FrameCounter");
	const uint64 AnimInstanceId = EventData.GetValue<uint64>("AnimInstanceId");
	const int32 NodeId = EventData.GetValue<int32>("NodeId");

	switch (RouteId)
	{
		case RouteId_MotionMatchingState:
			{
				FTraceMotionMatchingStateMessage Message;
				Message.ElapsedPoseJumpTime = EventData.GetValue<float>("ElapsedPoseJumpTime");
				// Cast back to our flag type
				Message.Flags = static_cast<FTraceMotionMatchingState::EFlags>(EventData.GetValue<uint32>("Flags"));
				Message.DbPoseIdx = EventData.GetValue<int32>("DbPoseIdx");
				const TArrayView<const float> View = EventData.GetArrayView<float>("QueryVector");
				Message.QueryVector = TArray<float>(View.GetData(), View.Num());
				Message.DatabaseId = EventData.GetValue<uint64>("DatabaseId");

				// Common data
				Message.NodeId = NodeId;
				Message.AnimInstanceId = AnimInstanceId;
				Message.FrameCounter = FrameCounter;

				TraceProvider.AppendMotionMatchingState(Message, Time);
				break;
			}
		default:
			{
				// Should not happen
				checkNoEntry();
			}
	}

	return true;
}
}}
