// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Containers/Array.h"
#include "Trace/Analysis.h"
#include "Trace/Analyzer.h"

namespace Trace
{

class FStreamReader;

////////////////////////////////////////////////////////////////////////////////
class FAnalysisEngine
	: public IAnalyzer
{
public:
	struct				FDispatch;
	struct				FEventDataInfo;
						FAnalysisEngine(TArray<IAnalyzer*>&& InAnalyzers);
						~FAnalysisEngine();
	bool				OnData(FStreamReader& Reader);

private:
	typedef bool (FAnalysisEngine::*ProtocolHandlerType)();

	struct FRoute
	{
		uint32			Hash;
		int16			Count;
		uint16			Id;
		uint16			AnalyzerIndex;
		uint16			_Unused0;
	};

	class				FDispatchBuilder;
	virtual bool		OnEvent(uint16 RouteId, const FOnEventContext& Context) override;
	void				OnNewTrace(const FOnEventContext& Context);
	void				OnTiming(const FOnEventContext& Context);
	void				OnNewEventInternal(const FOnEventContext& Context);
	void				OnNewEventProtocol0(FDispatchBuilder& Builder, const void* EventData);

	bool				EstablishTransport(FStreamReader& Reader);
	bool				OnDataProtocol0();
	bool				AddDispatch(FDispatch* Dispatch);
	template <typename ImplType>
	void				ForEachRoute(const FDispatch* Dispatch, ImplType&& Impl);
	void				AddRoute(uint16 AnalyzerIndex, uint16 Id, const ANSICHAR* Logger, const ANSICHAR* Event);
	void				AddRoute(uint16 AnalyzerIndex, uint16 Id, uint32 Hash);
	void				RetireAnalyzer(IAnalyzer* Analyzer);
	FSessionContext		SessionContext;
	TArray<FRoute>		Routes;
	TArray<IAnalyzer*>	Analyzers;
	TArray<FDispatch*>	Dispatches;
	class FTransport*	Transport = nullptr;
	ProtocolHandlerType	ProtocolHandler = nullptr;
};

} // namespace Trace
