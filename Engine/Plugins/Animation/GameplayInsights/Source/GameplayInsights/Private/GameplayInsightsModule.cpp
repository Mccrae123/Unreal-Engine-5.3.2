// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "Modules/ModuleInterface.h"
#include "Features/IModularFeatures.h"
#include "GameplayTraceModule.h"
#include "GameplayTimingViewExtender.h"
#include "Insights/ITimingViewExtender.h"
#include "Containers/Ticker.h"
#include "Modules/ModuleManager.h"
#include "Framework/Multibox/MultiboxBuilder.h"

#if WITH_ENGINE
#include "Engine/Engine.h"
#endif

class FGameplayInsightsModule : public IModuleInterface
{
public:
	// IModuleInterface interface
	virtual void StartupModule() override
	{
		IModularFeatures::Get().RegisterModularFeature(Trace::ModuleFeatureName, &GameplayTraceModule);
		IModularFeatures::Get().RegisterModularFeature(Insights::TimingViewExtenderFeatureName, &GameplayTimingViewExtender);

		TickerHandle = FTicker::GetCoreTicker().AddTicker(TEXT("GameplayInsights"), 0.0f, [this](float DeltaTime)
		{
			GameplayTimingViewExtender.TickVisualizers(DeltaTime);
			return true;
		});
	}

	virtual void ShutdownModule() override
	{
		FTicker::GetCoreTicker().RemoveTicker(TickerHandle);

		IModularFeatures::Get().UnregisterModularFeature(Trace::ModuleFeatureName, &GameplayTraceModule);
		IModularFeatures::Get().UnregisterModularFeature(Insights::TimingViewExtenderFeatureName, &GameplayTimingViewExtender);
	}

	FGameplayTraceModule GameplayTraceModule;
	FGameplayTimingViewExtender GameplayTimingViewExtender;

	FDelegateHandle TickerHandle;
};

IMPLEMENT_MODULE(FGameplayInsightsModule, GameplayInsights);
