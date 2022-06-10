﻿// Copyright Epic Games, Inc. All Rights Reserved.
#include "StringsAnalyzer.h"
#include "../Common/Utils.h"
#include "TraceServices/Model/Definitions.h"
#include "TraceServices/Model/Strings.h"

namespace TraceServices
{
	
/////////////////////////////////////////////////////////////////////////////////////////
void FStringsAnalyzer::OnAnalysisBegin(const FOnAnalysisContext& Context)
{
	Context.InterfaceBuilder.RouteEvent(RouteId_StaticString, "Strings", "StaticString");
	Context.InterfaceBuilder.RouteEvent(RouteId_FName, "Strings", "FName");
	Context.InterfaceBuilder.RouteEvent(RouteId_StaticStringNoSync, "Strings", "StaticStringNoSync");
	Context.InterfaceBuilder.RouteEvent(RouteId_FNameNoSync, "Strings", "FNameNoSync");
}

/////////////////////////////////////////////////////////////////////////////////////////
bool FStringsAnalyzer::OnEvent(uint16 RouteId, EStyle Style, const FOnEventContext& Context)
{
	switch (RouteId)
	{
		// The layout of these two events is identical so they can be handled using the same code.
	case RouteId_StaticString:
	case RouteId_FName:
	case RouteId_StaticStringNoSync:
	case RouteId_FNameNoSync:
		{
			const FEventData& EventData = Context.EventData;
			IDefinitionProvider* DefinitionProvider = GetDefinitionProvider(Session);

			FWideStringView DisplayWide;
			FAnsiStringView DisplayAnsi;
			const TCHAR* Display = nullptr;
			if (EventData.GetString("DisplayWide", DisplayWide) && DisplayWide.Len() > 0)
			{
				Display = Session.StoreString(DisplayWide);
			}
			else if (EventData.GetString("DisplayAnsi", DisplayAnsi) && DisplayAnsi.Len() > 0)
			{
				const FString Str(DisplayAnsi);
				Display = Session.StoreString(Str);
			}
			else
			{
				UE_LOG(LogTraceServices, Warning, TEXT("Empty string definition detected."));
				return true;
			}

			FStringDefinition* Instance = DefinitionProvider->Create<FStringDefinition>();
			Instance->Display = Display;

			if (RouteId == RouteId_FName)
			{
				auto Id = EventData.GetDefinitionId<uint32>();
				// Overwrite type id of Strings.FNameNoSync to Strings.FNames type id.
				//Id.RefTypeId = ???;
				DefinitionProvider->Register<FStringDefinition>(Instance, Id);
			}
			else
			{
				auto Id = EventData.GetDefinitionId<uint64>();
				// Overwrite type id of Strings.StaticStringNoSync to Strings.StaticString type id.
				//Id.RefTypeId = ???;
				DefinitionProvider->Register<FStringDefinition>(Instance, Id);
			}
		}
		break;
	default: ;
	}
	return true;
}

}
