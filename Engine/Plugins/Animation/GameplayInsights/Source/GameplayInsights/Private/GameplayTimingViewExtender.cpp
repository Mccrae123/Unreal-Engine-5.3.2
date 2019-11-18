// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "GameplayTimingViewExtender.h"
#include "Insights/ITimingViewSession.h"
#include "GameplaySharedData.h"
#include "AnimationSharedData.h"
#include "UObject/WeakObjectPtr.h"

#if WITH_ENGINE
#include "Engine/World.h"
#include "Editor/EditorEngine.h"
#endif

void FGameplayTimingViewExtender::OnBeginSession(Insights::ITimingViewSession& InSession)
{
	FPerSessionData* PerSessionData = PerSessionDataMap.Find(&InSession);
	if(PerSessionData == nullptr)
	{
		PerSessionData = &PerSessionDataMap.Add(&InSession);
		PerSessionData->GameplaySharedData = new FGameplaySharedData();
		PerSessionData->AnimationSharedData = new FAnimationSharedData(*PerSessionData->GameplaySharedData);
	}

	PerSessionData->GameplaySharedData->OnBeginSession(InSession);
	PerSessionData->AnimationSharedData->OnBeginSession(InSession);
}

void FGameplayTimingViewExtender::OnEndSession(Insights::ITimingViewSession& InSession)
{
	FPerSessionData* PerSessionData = PerSessionDataMap.Find(&InSession);
	if(PerSessionData != nullptr)
	{
		PerSessionData->GameplaySharedData->OnEndSession(InSession);
		PerSessionData->AnimationSharedData->OnEndSession(InSession);

		delete PerSessionData->GameplaySharedData;
		PerSessionData->GameplaySharedData = nullptr;
		delete PerSessionData->AnimationSharedData;
		PerSessionData->AnimationSharedData = nullptr;
	}

	PerSessionDataMap.Remove(&InSession);
}

void FGameplayTimingViewExtender::Tick(Insights::ITimingViewSession& InSession, const Trace::IAnalysisSession& InAnalysisSession)
{
	FPerSessionData* PerSessionData = PerSessionDataMap.Find(&InSession);
	if(PerSessionData != nullptr)
	{
		PerSessionData->GameplaySharedData->Tick(InSession, InAnalysisSession);
		PerSessionData->AnimationSharedData->Tick(InSession, InAnalysisSession);
	}
}

void FGameplayTimingViewExtender::ExtendFilterMenu(Insights::ITimingViewSession& InSession, FMenuBuilder& InMenuBuilder)
{
	FPerSessionData* PerSessionData = PerSessionDataMap.Find(&InSession);
	if(PerSessionData != nullptr)
	{
		PerSessionData->GameplaySharedData->ExtendFilterMenu(InMenuBuilder);
		PerSessionData->AnimationSharedData->ExtendFilterMenu(InMenuBuilder);
	}
}

#if WITH_ENGINE

static UWorld* GetWorldToVisualize()
{
	UWorld* World = nullptr;

#if WITH_EDITOR
	UEditorEngine* EditorEngine = Cast<UEditorEngine>(GEngine);
	if (GIsEditor && EditorEngine != nullptr && World == nullptr)
	{
		// lets use PlayWorld during PIE/Simulate and regular world from editor otherwise, to draw debug information
		World = EditorEngine->PlayWorld != nullptr ? EditorEngine->PlayWorld : EditorEngine->GetEditorWorldContext().World();
	}

#endif
	if (!GIsEditor && World == nullptr)
	{
		World = GEngine->GetWorld();
	}

	return World;
}

#endif

void FGameplayTimingViewExtender::TickVisualizers(float DeltaTime)
{
#if WITH_ENGINE
	UWorld* WorldToVisualize = GetWorldToVisualize();
	if(WorldToVisualize)
	{
		for(auto& PerSessionData : PerSessionDataMap)
		{
			PerSessionData.Value.AnimationSharedData->DrawPoses(WorldToVisualize);
		}
	}
#endif
}