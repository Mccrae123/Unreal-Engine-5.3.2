// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "Widgets/SMoviePipelineTabContent.h"
#include "Widgets/SMoviePipelinePanel.h"


// Analytics
#include "EngineAnalytics.h"
#include "Interfaces/IAnalyticsProvider.h"

#define LOCTEXT_NAMESPACE "SMoviePipelineTabContent"


void SMoviePipelineTabContent::Construct(const FArguments& InArgs)
{    
	// Delay one tick before opening the default pipeline setup panel.
	// this allows anything that just invoked the tab to customize it without the default UI being created
	RegisterActiveTimer(0.f, FWidgetActiveTimerDelegate::CreateSP(this, &SMoviePipelineTabContent::OnActiveTimer));
     
	if (FEngineAnalytics::IsAvailable())
	{
		FEngineAnalytics::GetProvider().RecordEvent(TEXT("MoviePipeline.PanelOpened"));
	}
}

EActiveTimerReturnType SMoviePipelineTabContent::OnActiveTimer(double InCurrentTime, float InDeltaTime)
{
	SetupForPipeline((UMoviePipelineShotConfig*)nullptr);
	return EActiveTimerReturnType::Stop;
}

void SMoviePipelineTabContent::SetupForPipeline(UMoviePipelineShotConfig* BasePreset)
{
	// Null out the tab content to ensure that all references have been cleaned up before constructing the new one
	ChildSlot [ SNullWidget::NullWidget ];

	ChildSlot
	.HAlign(HAlign_Fill)
	.VAlign(VAlign_Fill)
	[
		SAssignNew(WeakPanel, SMoviePipelinePanel)
		.BasePreset(BasePreset)
	];

	if (FEngineAnalytics::IsAvailable())
	{
		FEngineAnalytics::GetProvider().RecordEvent(TEXT("MoviePipeline.SetupForPipelineFromPreset"));
	}
}

#undef LOCTEXT_NAMESPACE // SMoviePipelineTabContent
