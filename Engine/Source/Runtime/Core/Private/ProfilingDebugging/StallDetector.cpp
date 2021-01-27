// Copyright Epic Games, Inc. All Rights Reserved.
#include "ProfilingDebugging/StallDetector.h"

#if STALL_DETECTOR

#include "HAL/ExceptionHandling.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformStackWalk.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformTLS.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

// force normal behavior in the face of debug configuration and debugger attached
#define STALL_DETECTOR_DEBUG 0

// use the heart beat clock to account for process suspend
#define STALL_DETECTOR_HEART_BEAT_CLOCK 1

#if STALL_DETECTOR_DEBUG && _MSC_VER
#pragma optimize( "", off )
#endif // STALL_DETECTOR_DEBUG && _MSC_VER

#if STALL_DETECTOR_HEART_BEAT_CLOCK
 #include "HAL/ThreadHeartBeat.h"
#endif // STALL_DETECTOR_HEART_BEAT_CLOCK

DEFINE_LOG_CATEGORY(LogStall);

/**
* The reference count for the resources for this API
**/
static uint32 InitCount = 0;

/**
* Stall Detector Thread
**/

namespace UE
{
	class FStallDetectorRunnable : public FRunnable
	{
	public:
		FStallDetectorRunnable();

		// FRunnable implementation
		virtual uint32 Run() override;
		virtual void Stop() override
		{
			StopThread = true;
		}
		virtual void Exit() override
		{
			Stop();
		}

		bool GetStartedThread()
		{
			return StartedThread;
		}
		
#if STALL_DETECTOR_HEART_BEAT_CLOCK
		FThreadHeartBeatClock& GetClock()
		{
			return Clock;
		}
#endif

	private:
		bool StartedThread;
		bool StopThread;

#if STALL_DETECTOR_HEART_BEAT_CLOCK
		FThreadHeartBeatClock Clock;
#endif
	};

	static FStallDetectorRunnable* Runnable = nullptr;
	static FRunnableThread* Thread = nullptr;
}

UE::FStallDetectorRunnable::FStallDetectorRunnable()
	: StartedThread(false)
	, StopThread(false)
#if STALL_DETECTOR_HEART_BEAT_CLOCK
	, Clock(50.0/1000.0) // the clamped time interval that each tick of the clock can possibly advance
#endif
{
}

uint32 UE::FStallDetectorRunnable::Run()
{
	while (!StopThread)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FStallDetectorRunnable::Run);

#if STALL_DETECTOR_HEART_BEAT_CLOCK
		Clock.Tick();
#endif

		// Clock has been ticked
		StartedThread = true;

		// Use this timestamp to try to avoid marginal triggering
		double Seconds = FStallDetector::Seconds();

		// Check the detectors
		{
			FScopeLock ScopeLock(&FStallDetector::GetCriticalSection());
			for (FStallDetector* Detector : FStallDetector::GetInstances())
			{
				Detector->Check(false, Seconds);
			}
		}

		// Sleep a an interval, the resolution at which we want to detect an overage
		FPlatformProcess::Sleep(0.005);
	}

	return 0;
}

/**
* Stall Detector Stats
**/

FCriticalSection UE::FStallDetectorStats::CriticalSection;
TSet<UE::FStallDetectorStats*> UE::FStallDetectorStats::Instances;
std::atomic<uint32> UE::FStallDetectorStats::TotalTriggeredCount;
std::atomic<uint32> UE::FStallDetectorStats::TotalReportedCount;

UE::FStallDetectorStats::FStallDetectorStats(const TCHAR* InName, const double InBudgetSeconds, const EStallDetectorReportingMode InReportingMode)
	: Name(InName)
	, BudgetSeconds(InBudgetSeconds)
	, ReportingMode(InReportingMode)
	, TriggerCount(0)
	, OverageSeconds(0.0)
{
	// Add at the end of construction
	FScopeLock ScopeLock(&CriticalSection);
	Instances.Add(this);
}

UE::FStallDetectorStats::~FStallDetectorStats()
{
	// Remove at the beginning of destruction
	FScopeLock ScopeLock(&CriticalSection);
	Instances.Remove(this);
}

/**
* Stall Detector
**/

FCriticalSection UE::FStallDetector::CriticalSection;
TSet<UE::FStallDetector*> UE::FStallDetector::Instances;

UE::FStallDetector::FStallDetector(FStallDetectorStats& InStats)
	: Stats(InStats)
	, bPersistent(false)
	, Triggered(false)
{
	check(InitCount);

	ThreadId = FPlatformTLS::GetCurrentThreadId();
	StartSeconds = FStallDetector::Seconds();

	// Add at the end of construction
	FScopeLock ScopeLock(&CriticalSection);
	Instances.Add(this);
}

UE::FStallDetector::~FStallDetector()
{
	// Remove at the beginning of destruction
	{
		FScopeLock ScopeLock(&CriticalSection);
		Instances.Remove(this);
	}

	if (!bPersistent)
	{
		Check(true);
	}
}

void UE::FStallDetector::Check(bool bIsComplete, double InWhenToCheckSeconds)
{
	double CheckSeconds = InWhenToCheckSeconds;
	if (InWhenToCheckSeconds == 0.0)
	{
		CheckSeconds = FStallDetector::Seconds();
	}

	double DeltaSeconds = CheckSeconds - StartSeconds;
	double OverageSeconds = DeltaSeconds - Stats.BudgetSeconds;

	if (Triggered)
	{
		if (bIsComplete)
		{
			Stats.OverageSeconds += OverageSeconds;

#if STALL_DETECTOR_DEBUG
			FString OverageString = FString::Printf(TEXT("[FStallDetector] [%s] Overage of %f\n"), Stats.Name, OverageSeconds);
			FPlatformMisc::LocalPrint(OverageString.GetCharArray().GetData());
#endif
			UE_LOG(LogStall, Display, TEXT("Stall detector '%s' exceeded budget of %fs, and completed in %fs"), Stats.Name, Stats.BudgetSeconds, OverageSeconds);
		}
	}
	else
	{
		if (OverageSeconds > 0.0)
		{
			bool PreviousTriggered = false;
			if (Triggered.compare_exchange_strong(PreviousTriggered, true, std::memory_order_acquire, std::memory_order_relaxed))
			{
#if STALL_DETECTOR_DEBUG
				FString OverageString = FString::Printf(TEXT("[FStallDetector] [%s] Triggered at %f\n"), Stats.Name, CheckSeconds);
				FPlatformMisc::LocalPrint(OverageString.GetCharArray().GetData());
#endif
				Stats.TriggerCount++;
				OnStallDetected(ThreadId, DeltaSeconds);
			}
		}
	}
}

void UE::FStallDetector::CheckAndReset()
{
	double CheckSeconds = FStallDetector::Seconds();

	// if this is the first call to CheckAndReset
	if (!bPersistent)
	{
		// never the first call again, because the timespan between construction and the first call isn't valid don't perform a check
		bPersistent = true;
	}
	else
	{
		// only perform the check on the second call
		Check(true, CheckSeconds);
	}

	StartSeconds = CheckSeconds;
	Triggered = false;
}

void UE::FStallDetector::OnStallDetected(uint32 InThreadId, const double InElapsedSeconds)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FStallDetector::OnStallDetected);

	Stats.TotalTriggeredCount++;

	//
	// Determine if we want to undermine the specified reporting mode
	//

	EStallDetectorReportingMode ReportingMode = Stats.ReportingMode;

	bool bDisableReporting = false;
#if UE_BUILD_DEBUG
	bDisableReporting |= true; // Do not generate a report in debug configurations due to performance characteristics
#endif
	bDisableReporting |= FPlatformMisc::IsDebuggerPresent(); // Do not generate a report if we detect the debugger mucking with things

	if (bDisableReporting)
	{
#if !STALL_DETECTOR_DEBUG
		ReportingMode = EStallDetectorReportingMode::Never;
#endif
	}

	//
	// Resolve reporting mode to whether we should send a report for this call
	//

	bool bSendReport = false;
	switch (ReportingMode)
	{
	case EStallDetectorReportingMode::First:
		bSendReport = Stats.TriggerCount == 1;
		break;

	case EStallDetectorReportingMode::Always:
		bSendReport = true;
		break;

	default:
		break;
	}

	//
	// Send the report
	//

	if (bSendReport)
	{
		Stats.TotalReportedCount++;
		const int NumStackFramesToIgnore = FPlatformTLS::GetCurrentThreadId() == InThreadId ? 2 : 0;
		ReportStall(Stats.Name, InThreadId, NumStackFramesToIgnore);
		UE_LOG(LogStall, Warning, TEXT("Stall detector '%s' exceeded budget of %fs, and was reported"), Stats.Name, Stats.BudgetSeconds);
	}
	else
	{
		UE_LOG(LogStall, Warning, TEXT("Stall detector '%s' exceeded budget of %fs seconds"), Stats.Name, Stats.BudgetSeconds);
	}
}

double UE::FStallDetector::Seconds()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FStallDetector::Seconds);

	check(InitCount);

	double Result;

#if STALL_DETECTOR_HEART_BEAT_CLOCK
	Result = Runnable->GetClock().Seconds();
#else
	Result = FPlatformTime::Seconds();
#endif

#if STALL_DETECTOR_DEBUG
	static double ClockStartSeconds = Result;
	static double PlatformStartSeconds = FPlatformTime::Seconds();
	double ClockDelta = Result - ClockStartSeconds;
	double PlatformDelta = FPlatformTime::Seconds() - PlatformStartSeconds;
	double Drift = PlatformDelta - ClockDelta;
	static double LastDrift = Drift;
	double DriftDelta = Drift - LastDrift;
	if (DriftDelta > 0.001)
	{
		FString ResultString = FString::Printf(TEXT("[FStallDetector] Thread %5d / Platform: %f / Clock: %f / Drift: %f (%f)\n"), FPlatformTLS::GetCurrentThreadId(), PlatformDelta, ClockDelta, Drift, DriftDelta);
		FPlatformMisc::LocalPrint(ResultString.GetCharArray().GetData());
		LastDrift = Drift;
	}
#endif

	return Result;
}

void UE::FStallDetector::Startup()
{
	if (++InitCount == 1)
	{
		check(FPlatformTime::GetSecondsPerCycle());

		// Cannot be a global due to clock member
		Runnable = new FStallDetectorRunnable();

		if (Thread == nullptr)
		{
			Thread = FRunnableThread::Create(Runnable, TEXT("StallDetectorThread"));
			check(Thread);

			// Poll until we have ticked the clock
			while (!Runnable->GetStartedThread())
			{
				FPlatformProcess::YieldThread();
			}
		}
	}
}

void UE::FStallDetector::Shutdown()
{
	if (--InitCount == 0)
	{
		delete Thread;
		Thread = nullptr;

		delete Runnable;
		Runnable = nullptr;
	}
}

#endif // STALL_DETECTOR