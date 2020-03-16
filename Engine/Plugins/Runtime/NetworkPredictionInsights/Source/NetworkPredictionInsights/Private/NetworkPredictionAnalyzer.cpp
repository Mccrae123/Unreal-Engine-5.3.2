// Copyright Epic Games, Inc. All Rights Reserved.

#include "NetworkPredictionAnalyzer.h"
#include "NetworkPredictionProvider.h"


FNetworkPredictionAnalyzer::FNetworkPredictionAnalyzer(Trace::IAnalysisSession& InSession, FNetworkPredictionProvider& InNetworkPredictionProvider)
	: Session(InSession)
	, NetworkPredictionProvider(InNetworkPredictionProvider)
{
}

void FNetworkPredictionAnalyzer::OnAnalysisBegin(const FOnAnalysisContext& Context)
{
	auto& Builder = Context.InterfaceBuilder;
	
	Builder.RouteEvent(RouteId_GameInstanceRegister, "NetworkPrediction", "GameInstanceRegister");
	Builder.RouteEvent(RouteId_SimulationCreated, "NetworkPrediction", "SimulationCreated");
	Builder.RouteEvent(RouteId_SimulationNetRole, "NetworkPrediction", "SimulationNetRole");
	Builder.RouteEvent(RouteId_SimulationNetGUID, "NetworkPrediction", "SimulationNetGUID");	
	Builder.RouteEvent(RouteId_SimulationTick, "NetworkPrediction", "SimulationTick");
	Builder.RouteEvent(RouteId_OOBStateMod, "NetworkPrediction", "OOBStateMod");
	Builder.RouteEvent(RouteId_ProduceInput, "NetworkPrediction", "ProduceInput");
	Builder.RouteEvent(RouteId_SynthInput, "NetworkPrediction", "SynthInput");
	Builder.RouteEvent(RouteId_SimulationEOF, "NetworkPrediction", "SimulationEOF");
	Builder.RouteEvent(RouteId_NetSerializeRecv, "NetworkPrediction", "NetSerializeRecv");
	Builder.RouteEvent(RouteId_NetSerializeCommit, "NetworkPrediction", "NetSerializeCommit");
	Builder.RouteEvent(RouteId_NetSerializeFault, "NetworkPrediction", "NetSerializeFault");
	Builder.RouteEvent(RouteId_InputCmd, "NetworkPrediction", "InputCmd");
	Builder.RouteEvent(RouteId_SyncState, "NetworkPrediction", "SyncState");
	Builder.RouteEvent(RouteId_AuxState, "NetworkPrediction", "AuxState");
	Builder.RouteEvent(RouteId_PieBegin, "NetworkPrediction", "PieBegin");
}

void FNetworkPredictionAnalyzer::OnAnalysisEnd()
{
}

bool FNetworkPredictionAnalyzer::OnEvent(uint16 RouteId, const FOnEventContext& Context)
{
	Trace::FAnalysisSessionEditScope _(Session);
	const auto& EventData = Context.EventData;

	auto ParseUserState = [&EventData, this](ENP_UserState Type)
	{
		int32 SimulationId = EventData.GetValue<uint32>("SimulationId");			 
		int32 Frame = EventData.GetValue<int32>("SimulationFrame");
		uint64 EngineFrame = EventData.GetValue<uint64>("EngineFrameNumber");

		NetworkPredictionProvider.WriteUserState(SimulationId, Frame, EngineFrame, Type, Session.StoreString(reinterpret_cast<const TCHAR*>(EventData.GetAttachment())));
	};
	
	
	bool bUnhandled = false;
	switch (RouteId)
	{
		case RouteId_GameInstanceRegister:
		{
			// Temp, just set version here until we are encoding it as the first piece of data
			NetworkPredictionProvider.SetNetworkPredictionTraceVersion(1);

			// This is no longer doing anything since we are essentially all actor-role based now
			// EventData.GetValue<uint32>("GameInstanceId"),
			// EventData.GetValue<bool>("IsServer")
			break;
		}
		case RouteId_SimulationCreated:
		{
			FSimulationData::FConst& ConstData = NetworkPredictionProvider.WriteSimulationCreated(EventData.GetValue<uint32>("SimulationId"));
			ConstData.DebugName = FString(reinterpret_cast<const TCHAR*>(EventData.GetAttachment()));
			ConstData.ID.NetGUID = EventData.GetValue<uint32>("NetGUID");
			ConstData.GameInstanceId = EventData.GetValue<uint32>("GameInstanceId");
			break;
		}
		case RouteId_SimulationNetRole:
		{
			NetworkPredictionProvider.WriteSimulationNetRole(	EventData.GetValue<uint32>("SimulationId"),
																EventData.GetValue<uint64>("EngineFrameNumber"),
																(ENP_NetRole)EventData.GetValue<uint8>("NetRole"));
			break;
		}

		case RouteId_SimulationNetGUID:
		{
			NetworkPredictionProvider.WriteSimulationNetGUID(EventData.GetValue<uint32>("SimulationId"), EventData.GetValue<uint32>("NetGUID"));																
			break;
		}

		case RouteId_SimulationTick:
		{
			FSimulationData::FTick TickData;
			TickData.EngineFrame = EventData.GetValue<uint64>("EngineFrameNumber");
			TickData.StartMS = EventData.GetValue<int32>("StartMS");
			TickData.EndMS = EventData.GetValue<int32>("EndMS");
			TickData.OutputFrame = EventData.GetValue<int32>("OutputFrame");

			NetworkPredictionProvider.WriteSimulationTick(EventData.GetValue<uint32>("SimulationId"), MoveTemp(TickData));
			break;
		}
		case RouteId_SimulationEOF:
		{
			FSimulationData::FEngineFrame& FrameData = NetworkPredictionProvider.WriteSimulationEOF(EventData.GetValue<uint32>("SimulationId"));
			FrameData.EngineFrame = EventData.GetValue<uint64>("EngineFrameNumber");
			FrameData.EngineFrameDeltaTime = EventData.GetValue<double>("EngineFrameDeltaTime");
			FrameData.EngineCurrentTime = EventData.GetValue<double>("EngineCurrentTime");

			FrameData.TotalProcessedSimTimeMS = EventData.GetValue<int32>("TotalProcessedMS");
			FrameData.TotalAllowedSimTimeMS = EventData.GetValue<int32>("TotalAllowedMS");
		
			FrameData.LastSentKeyframe = EventData.GetValue<int32>("LastSentKeyframe");
			FrameData.LastReceivedKeyframe = EventData.GetValue<int32>("LastReceivedKeyframe");
			break;
		}

		case RouteId_NetSerializeRecv:
		{
			FSimulationData::FNetSerializeRecv NetRecv;
			NetRecv.EngineFrame = EventData.GetValue<uint64>("EngineFrameNumber");
			NetRecv.SimTimeMS = EventData.GetValue<uint64>("ReceivedSimTimeMS");
			NetRecv.Frame = EventData.GetValue<uint64>("ReceivedFrame");
			
			NetworkPredictionProvider.WriteNetRecv(EventData.GetValue<uint32>("SimulationId"), MoveTemp(NetRecv));
			break;
		}
		
		case RouteId_NetSerializeCommit:
		{
			NetworkPredictionProvider.WriteNetCommit(EventData.GetValue<uint32>("SimulationId"));
			break;
		}

		case RouteId_NetSerializeFault:
		{
			NetworkPredictionProvider.WriteNetFault(EventData.GetValue<uint32>("SimulationId"));
			break;
		}

		case RouteId_OOBStateMod:
		{
			NetworkPredictionProvider.WriteOOBStateMod(EventData.GetValue<uint32>("SimulationId"));
			break;
		}

		case RouteId_ProduceInput:
		{
			NetworkPredictionProvider.WriteProduceInput(EventData.GetValue<uint32>("SimulationId"));
			break;
		}
		
		case RouteId_SynthInput:
		{
			NetworkPredictionProvider.WriteSynthInput(EventData.GetValue<uint32>("SimulationId"));
			break;
		}

		case RouteId_InputCmd:
		{
			ParseUserState(ENP_UserState::Input);
			break;
		}

		case RouteId_SyncState:
		{
			ParseUserState(ENP_UserState::Sync);
			break;
		}

		case RouteId_AuxState:
		{
			ParseUserState(ENP_UserState::Aux);
			break;
		}

		case RouteId_PieBegin:
		{
			NetworkPredictionProvider.WritePIEStart();
			break;
		}

		default:
			bUnhandled = true;
			break;
	}

	if (!bUnhandled)
	{
		NetworkPredictionProvider.IncrementDataCounter();
	}

	return true;
}