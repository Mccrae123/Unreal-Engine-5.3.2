// Copyright Epic Games, Inc. All Rights Reserved.

#include "FramePerformanceProvider.h"

#include "EngineGlobals.h"
#include "Features/IModularFeatures.h"
#include "IStageDataProvider.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"
#include "RenderCore.h"
#include "StageDataProviderModule.h"
#include "StageMessages.h"
#include "StageMonitoringSettings.h"
#include "Stats/StatsData.h"
#include "VPSettings.h"

FFramePerformanceProvider::FFramePerformanceProvider()
{
	//Verify if conditions are met to enable frame performance messages
	const UStageMonitoringSettings* Settings = GetDefault<UStageMonitoringSettings>();
	if (!Settings->ProviderSettings.FramePerformanceSettings.bUseRoleFiltering || GetDefault<UVPSettings>()->GetRoles().HasAny(Settings->ProviderSettings.FramePerformanceSettings.SupportedRoles))
	{
		FCoreDelegates::OnEndFrame.AddRaw(this, &FFramePerformanceProvider::OnEndFrame);
	}

#if STATS
	//Verify if conditions are met to enable sending hitch messages
	if (!Settings->ProviderSettings.HitchDetectionSettings.bUseRoleFiltering || GetDefault<UVPSettings>()->GetRoles().HasAny(Settings->ProviderSettings.HitchDetectionSettings.SupportedRoles))
	{
		// Subscribe to Stats provider to verify hitches
		StatsMasterEnableAdd();
		FStatsThreadState& Stats = FStatsThreadState::GetLocalState();
		Stats.NewFrameDelegate.AddRaw(this, &FFramePerformanceProvider::CheckHitches);
	}
#endif //STATS
}

FFramePerformanceProvider::~FFramePerformanceProvider()
{
	//Cleanup what could have been registered
	FCoreDelegates::OnEndFrame.RemoveAll(this);

#if STATS
	StatsMasterEnableSubtract();
	FStatsThreadState& Stats = FStatsThreadState::GetLocalState();
	Stats.NewFrameDelegate.RemoveAll(this);
#endif //STATS
}

void FFramePerformanceProvider::OnEndFrame()
{
	UpdateFramePerformance();
}

void FFramePerformanceProvider::CheckHitches(int64 Frame)
{
#if STATS
	// when synced, this time will be the full time of the frame, whereas the above don't include any waits
	const FStatsThreadState& Stats = FStatsThreadState::GetLocalState();
	const float GameThreadTimeWithWaits = (float)FPlatformTime::ToMilliseconds64(Stats.GetFastThreadFrameTime(Frame, EThreadType::Game));
	const float RenderThreadTimeWithWaits = (float)FPlatformTime::ToMilliseconds64(Stats.GetFastThreadFrameTime(Frame, EThreadType::Renderer));
	const float FullFrameTime = FMath::Max(GameThreadTimeWithWaits, RenderThreadTimeWithWaits);

	// check for hitch (if application not backgrounded)
	const float TimeThreshold = GetDefault<UStageMonitoringSettings>()->ProviderSettings.HitchDetectionSettings.TargetFrameRate.AsInterval() * 1000.0f;
	if (FullFrameTime > TimeThreshold)
	{
		const float GameThreadTime = FPlatformTime::ToMilliseconds(GGameThreadTime);
		const float RenderThreadTime = FPlatformTime::ToMilliseconds(GRenderThreadTime);
		const float GPUTime = FPlatformTime::ToMilliseconds(GGPUFrameTime);

		UE_LOG(LogStageDataProvider, VeryVerbose, TEXT("Hitch detected: FullFrameTime=%f, GameThreadTimeWithWaits=%f, RenderThreadTimeWithWaits=%f, Threshold=%f, GameThreadTime=%f, RenderThreadTime=%f"), FullFrameTime, GameThreadTimeWithWaits, RenderThreadTimeWithWaits, TimeThreshold, GameThreadTime, RenderThreadTime);

		IStageDataProvider::SendMessage<FHitchDetectionMessage>(EStageMessageFlags::None, GameThreadTimeWithWaits, RenderThreadTimeWithWaits, GameThreadTime, RenderThreadTime, GPUTime, TimeThreshold);
	}
#endif //STATS
}

void FFramePerformanceProvider::UpdateFramePerformance()
{
	const double CurrentTime = FApp::GetCurrentTime();
	if (CurrentTime - LastFramePerformanceSent >= GetDefault<UStageMonitoringSettings>()->ProviderSettings.FramePerformanceSettings.UpdateInterval)
	{
		LastFramePerformanceSent = CurrentTime;

		const float GameThreadTime = FPlatformTime::ToMilliseconds(GGameThreadTime);
		const float RenderThreadTime = FPlatformTime::ToMilliseconds(GRenderThreadTime);
		const float GPUTime = FPlatformTime::ToMilliseconds(GGPUFrameTime);
		IStageDataProvider::SendMessage<FFramePerformanceProviderMessage>(EStageMessageFlags::None, GameThreadTime, RenderThreadTime, GPUTime);
	}
}
