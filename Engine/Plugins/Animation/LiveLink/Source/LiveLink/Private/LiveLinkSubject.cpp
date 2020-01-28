// Copyright Epic Games, Inc. All Rights Reserved.

#include "LiveLinkSubject.h"

#include "ITimeManagementModule.h"
#include "LiveLinkFramePreProcessor.h"
#include "LiveLinkFrameTranslator.h"
#include "LiveLinkClient.h"
#include "LiveLinkLog.h"
#include "LiveLinkRole.h"
#include "LiveLinkRoleTrait.h"
#include "LiveLinkSettings.h"
#include "Misc/App.h"
#include "Templates/Sorting.h"
#include "Templates/SubclassOf.h"
#include "TimedDataInputCollection.h"
#include "TimeSynchronizationSource.h"
#include "UObject/Class.h"


FLiveLinkSubject::FLiveLinkSubject(TWeakPtr<FLiveLinkTimedDataInputGroup> InTimedDataGroup)
	: TimedDataGroup(InTimedDataGroup)
{

}

void FLiveLinkSubject::Initialize(FLiveLinkSubjectKey InSubjectKey, TSubclassOf<ULiveLinkRole> InRole, ILiveLinkClient* InLiveLinkClient)
{
	SubjectKey = InSubjectKey;
	Role = InRole;

	FrameData.Reset();
	ResetBufferStats();

	ITimeManagementModule::Get().GetTimedDataInputCollection().Add(this);
}

FLiveLinkSubject::~FLiveLinkSubject()
{
	ITimeManagementModule::Get().GetTimedDataInputCollection().Remove(this);
}

void FLiveLinkSubject::Update()
{
	// Clear all frames that are too old
	if (FrameData.Num() > CachedSettings.BufferSettings.MaxNumberOfFrameToBuffered)
	{
		const int32 NumberOfFrameToRemove = FrameData.Num() - CachedSettings.BufferSettings.MaxNumberOfFrameToBuffered;
		const int32 Count = CachedSettings.BufferSettings.bKeepAtLeastOneFrame && FrameData.Num() == NumberOfFrameToRemove ? NumberOfFrameToRemove-1 : NumberOfFrameToRemove;
		if (Count > 0)
		{
			FrameData.RemoveAt(0, Count, false);
		}
	}

	if (GetMode() == ELiveLinkSourceMode::EngineTime)
	{
		double ValidEngineTime = FApp::GetCurrentTime() - CachedSettings.BufferSettings.EngineTimeOffset - CachedSettings.BufferSettings.ValidEngineTime;
		int32 FrameIndex = 0;
		for (const FLiveLinkFrameDataStruct& SourceFrameData : FrameData)
		{
			double FrameTime = SourceFrameData.GetBaseData()->WorldTime.GetOffsettedTime();
			double OffsetTime = ValidEngineTime;
			if (FrameTime > OffsetTime)
			{
				break;
			}
			++FrameIndex;
		}

		if (FrameIndex - 1 >= 0)
		{
			const int32 Count = CachedSettings.BufferSettings.bKeepAtLeastOneFrame && FrameData.Num() == FrameIndex ? FrameIndex - 1 : FrameIndex;
			if (Count > 0)
			{
				FrameData.RemoveAt(0, Count, false);
			}
		}
	}
	else if (GetMode() == ELiveLinkSourceMode::Timecode)
	{
		if (FApp::GetCurrentFrameTime().IsSet())
		{
			const FQualifiedFrameTime CurrentSyncTime = FApp::GetCurrentFrameTime().GetValue();
			const FFrameTime CurrentFrameTimeInFrameSpace = CurrentSyncTime.ConvertTo(CachedSettings.BufferSettings.TimecodeFrameRate);
			int32 FrameIndex = 0;
			for (const FLiveLinkFrameDataStruct& SourceFrameData : FrameData)
			{
				FFrameTime UsedFrameTime = CurrentFrameTimeInFrameSpace - FFrameTime::FromDecimal(CachedSettings.BufferSettings.TimecodeFrameOffset) - CachedSettings.BufferSettings.ValidTimecodeFrame;
				FFrameTime FrameTime = SourceFrameData.GetBaseData()->MetaData.SceneTime.Time;
				if (FrameTime > UsedFrameTime)
				{
					break;
				}
				++FrameIndex;
			}

			if (FrameIndex - 1 >= 0)
			{
				const int32 Count = CachedSettings.BufferSettings.bKeepAtLeastOneFrame && FrameData.Num() == FrameIndex ? FrameIndex - 1 : FrameIndex;
				if (Count > 0)
				{
					FrameData.RemoveAt(0, Count, false);
				}
			}
		}

		// no warning if GetCurrentFrameTime is not set, the warning is done below after GetFrameAtSceneTime
	}

	// Build a snapshot for this role
	bool bSnapshotIsValid = false;
	if (FrameData.Num() > 0)
	{
		switch(GetMode())
		{
		case ELiveLinkSourceMode::Timecode:
		{
			if (FApp::GetCurrentFrameTime().IsSet())
			{
				bSnapshotIsValid = GetFrameAtSceneTime(FApp::GetCurrentFrameTime().GetValue(), FrameSnapshot);
			}
			else
			{
				static const FName NAME_InvalidRole = "LiveLinkSubject_NoCurrentFrameTime";
				FLiveLinkLog::WarningOnce(NAME_InvalidRole, SubjectKey, TEXT("Can't evaluate frame for subject '%s'. The engine doesn't have a timecode value set."), *SubjectKey.SubjectName.ToString());
			}
		}
		break;

		case ELiveLinkSourceMode::EngineTime:
		{
			bSnapshotIsValid = GetFrameAtWorldTime(FApp::GetCurrentTime(), FrameSnapshot);
			break;
		}

		case ELiveLinkSourceMode::Latest:
		default:
			bSnapshotIsValid = GetLatestFrame(FrameSnapshot);
		}
	}

	if (!bSnapshotIsValid)
	{
		// Invalidate the snapshot
		FrameSnapshot.FrameData.Reset();
	}
}

void FLiveLinkSubject::ClearFrames()
{
	FrameSnapshot.StaticData.Reset();
	FrameSnapshot.FrameData.Reset();
	FrameData.Reset();
}

bool FLiveLinkSubject::HasValidFrameSnapshot() const
{
	return FrameSnapshot.StaticData.IsValid() && FrameSnapshot.FrameData.IsValid();
}

TArray<FLiveLinkTime> FLiveLinkSubject::GetFrameTimes() const
{
	TArray<FLiveLinkTime> Result;
	Result.Reset(FrameData.Num());
	for (const FLiveLinkFrameDataStruct& Data : FrameData)
	{
		Result.Emplace(Data.GetBaseData()->WorldTime.GetOffsettedTime(), Data.GetBaseData()->MetaData.SceneTime);
	}
	return Result;
}

bool FLiveLinkSubject::EvaluateFrameAtWorldTime(double InWorldTime, TSubclassOf<ULiveLinkRole> InDesiredRole, FLiveLinkSubjectFrameData& OutFrame)
{
	if (Role == nullptr)
	{
		static const FName NAME_InvalidRole = "LiveLinkSubject_InvalidRole";
		FLiveLinkLog::ErrorOnce(NAME_InvalidRole, SubjectKey, TEXT("Can't evaluate frame for subject '%s'. No role has been set yet."), *SubjectKey.SubjectName.ToString());
		return false;
	}

	if (InDesiredRole == nullptr)
	{
		static const FName NAME_InvalidDesiredRole = "LiveLinkSubject_InvalidDesiredRole";
		FLiveLinkLog::ErrorOnce(NAME_InvalidDesiredRole, SubjectKey, TEXT("Can't evaluate frame for subject '%s'. Invalid role was received for evaluation."), *SubjectKey.SubjectName.ToString());
		return false;
	}

	if (GetMode() != ELiveLinkSourceMode::EngineTime)
	{
		static const FName NAME_EvalutationWorldTime = "LiveLinkSubject_EvalutationWorldTime";
		FLiveLinkLog::ErrorOnce(NAME_EvalutationWorldTime, SubjectKey, TEXT("Can't evaluate the subject '%s' at world time. The source mode is not set to Engine Time."), *SubjectKey.SubjectName.ToString());
		return false;
	}

	bool bSuccess = false;
	if (FrameData.Num() != 0)
	{
		if (Role == InDesiredRole || Role->IsChildOf(InDesiredRole))
		{
			GetFrameAtWorldTime(InWorldTime, OutFrame);
			bSuccess = true;
		}
		else if (SupportsRole(InDesiredRole))
		{
			FLiveLinkSubjectFrameData TmpFrameData;
			GetFrameAtWorldTime(InWorldTime, TmpFrameData);
			bSuccess = Translate(this, InDesiredRole, TmpFrameData.StaticData, TmpFrameData.FrameData, OutFrame);
		}
		else
		{
			static const FName NAME_CantTranslate = "LiveLinkSubject_CantTranslate";
			FLiveLinkLog::WarningOnce(NAME_CantTranslate, SubjectKey, TEXT("Can't evaluate frame for subject '%s'. Role '%s' is incompatible with current role '%s'."), *SubjectKey.SubjectName.ToString(), *InDesiredRole->GetName(), *Role->GetName());
		}
	}

	return bSuccess;
}

bool FLiveLinkSubject::EvaluateFrameAtSceneTime(const FQualifiedFrameTime& InSceneTime, TSubclassOf<ULiveLinkRole> InDesiredRole, FLiveLinkSubjectFrameData& OutFrame)
{
	if (Role == nullptr)
	{
		static const FName NAME_InvalidRole = "LiveLinkSubject_InvalidRole";
		FLiveLinkLog::ErrorOnce(NAME_InvalidRole, SubjectKey, TEXT("Can't evaluate frame for subject '%s'. No role has been set yet."), *SubjectKey.SubjectName.ToString());
		return false;
	}

	if (InDesiredRole == nullptr)
	{
		static const FName NAME_InvalidDesiredRole = "LiveLinkSubject_InvalidDesiredRole";
		FLiveLinkLog::ErrorOnce(NAME_InvalidDesiredRole, SubjectKey, TEXT("Can't evaluate frame for subject '%s'. Invalid role was received for evaluation."), *SubjectKey.SubjectName.ToString());
		return false;
	}

	if (GetMode() != ELiveLinkSourceMode::Timecode)
	{
		static const FName NAME_EvaluationSceneTime = "LiveLinkSubject_EvalutationSceneTime";
		FLiveLinkLog::ErrorOnce(NAME_EvaluationSceneTime, SubjectKey, TEXT("Can't evaluate the subject '%s' at scene time. The source mode is not set to Timecode."), *SubjectKey.SubjectName.ToString());
		return false;
	}

	bool bSuccess = false;
	if (FrameData.Num() != 0)
	{
		if (Role == InDesiredRole || Role->IsChildOf(InDesiredRole))
		{
			GetFrameAtSceneTime(InSceneTime, OutFrame);
			bSuccess = true;
		}
		else if (SupportsRole(InDesiredRole))
		{
			FLiveLinkSubjectFrameData TmpFrameData;
			GetFrameAtSceneTime(InSceneTime, TmpFrameData);
			bSuccess = Translate(this, InDesiredRole, TmpFrameData.StaticData, TmpFrameData.FrameData, OutFrame);
		}
		else
		{
			static const FName NAME_CantTranslate = "LiveLinkSubject_CantTranslate";
			FLiveLinkLog::WarningOnce(NAME_CantTranslate, SubjectKey, TEXT("Can't evaluate frame for subject '%s'. Role '%s' is incompatible with current role '%s'."), *SubjectKey.SubjectName.ToString(), *InDesiredRole->GetName(), *Role->GetName());
		}
	}

	return bSuccess;
}

bool FLiveLinkSubject::HasStaticData() const
{
	return StaticData.IsValid();
}

void FLiveLinkSubject::AddFrameData(FLiveLinkFrameDataStruct&& InFrameData)
{
	check(IsInGameThread());
	if (!StaticData.IsValid())
	{
		static const FName InvalidStatFrame = "LiveLinkSubject_InvalidStatFrame";
		FLiveLinkLog::WarningOnce(InvalidStatFrame, SubjectKey, TEXT("Can't add frame for subject '%s'. The static frame data is invalid."), *SubjectKey.SubjectName.ToString());
		return;
	}

	if (Role == nullptr)
	{
		return;
	}

	if (Role->GetDefaultObject<ULiveLinkRole>()->GetFrameDataStruct() != InFrameData.GetStruct())
	{
		static const FName NAME_IncompatibleRoles = "LiveLinkSubject_IncompatibleRoles";
		FLiveLinkLog::WarningOnce(NAME_IncompatibleRoles, SubjectKey, TEXT("Can't add frame for subject '%s'. The frame data is incompatible with current role '%s'."), *SubjectKey.SubjectName.ToString(), *Role->GetName());
		return;
	}

	if (!FLiveLinkRoleTrait::Validate(Role, InFrameData))
	{
		static const FName NAME_UnsupportedFrameData = "LiveLinkSubject_UnsupportedFrameData";
		FLiveLinkLog::WarningOnce(NAME_UnsupportedFrameData, SubjectKey, TEXT("Trying to add unsupported frame data type to role '%s'."), *Role->GetName());
		return;
	}

	int32 FrameIndex = INDEX_NONE;
	switch (CachedSettings.SourceMode)
	{
	case ELiveLinkSourceMode::EngineTime:
		FrameIndex = FindNewFrame_WorldTime(InFrameData.GetBaseData()->WorldTime);
		break;
	case ELiveLinkSourceMode::Timecode:
		FrameIndex = FindNewFrame_SceneTime(InFrameData.GetBaseData()->MetaData.SceneTime, InFrameData.GetBaseData()->WorldTime);
		break;
	case ELiveLinkSourceMode::Latest:
	default:
		FrameIndex = FindNewFrame_Latest(InFrameData.GetBaseData()->WorldTime);
		break;
	}
	
	if (FrameIndex >= 0)
	{
		// Before adding the new frame, test to see if we are going to increase the buffer size
		const bool bRemoveFrame = FrameData.Num() >= CachedSettings.BufferSettings.MaxNumberOfFrameToBuffered;
		if (bRemoveFrame)
		{
			--FrameIndex;
		}

		// It's possible the new frame is the frame we want to remove
		if (FrameIndex >= 0)
		{
			for (ULiveLinkFramePreProcessor::FWorkerSharedPtr PreProcessor : FramePreProcessors)
			{
				PreProcessor->PreProcessFrame(InFrameData);
			}

			if (bRemoveFrame)
			{
				FrameData.RemoveAt(0);
			}
			FrameData.Insert(MoveTemp(InFrameData), FrameIndex);

			if (CachedSettings.BufferSettings.bGenerateSubFrame && CachedSettings.SourceMode == ELiveLinkSourceMode::Timecode)
			{
				AdjustSubFrame_SceneTime(FrameIndex);
			}
		}
	}
	else 
	{
		// For some reason, the frame can't be added and is just discarded. Logs will have more info about the why
		IncreaseFrameDroppedStat();
	}

	LastPushTime = FApp::GetCurrentTime();
}

int32 FLiveLinkSubject::FindNewFrame_WorldTime(const FLiveLinkWorldTime& WorldTime) const
{
	const double ValidEngineTime = FApp::GetCurrentTime() - CachedSettings.BufferSettings.EngineTimeOffset - CachedSettings.BufferSettings.ValidEngineTime;
	const double WorldOffsettedTime = WorldTime.GetOffsettedTime();
	if (WorldOffsettedTime < ValidEngineTime)
	{
		static const FName NAME_InvalidWorldTime = "LiveLinkSubject_InvalidWorldTIme";
		FLiveLinkLog::WarningOnce(NAME_InvalidWorldTime, SubjectKey, TEXT("Trying to add a frame in which the world time has a value too low compare to the engine's time. Do you have an invalid offset? The Subject is '%s'."), *SubjectKey.SubjectName.ToString());
	}

	return FindNewFrame_WorldTimeInternal(WorldTime);
}

int32 FLiveLinkSubject::FindNewFrame_WorldTimeInternal(const FLiveLinkWorldTime& WorldTime) const
{
	int32 FrameIndex = FrameData.Num() - 1;
	const double NewFrameOffsettedTime = WorldTime.GetOffsettedTime();
	for (; FrameIndex >= 0; --FrameIndex)
	{
		const double FrameOffsettedTime = FrameData[FrameIndex].GetBaseData()->WorldTime.GetOffsettedTime();
		if (FrameOffsettedTime <= NewFrameOffsettedTime)
		{
			if (FMath::IsNearlyEqual(FrameOffsettedTime, NewFrameOffsettedTime))
			{
				static const FName NAME_SameWorldTime = "LiveLinkSubject_SameWorldTime";
				FLiveLinkLog::WarningOnce(NAME_SameWorldTime, SubjectKey, TEXT("A new frame data for subjet '%s' has the same time as a previous frame."), *SubjectKey.SubjectName.ToString());
			}
			break;
		}
	}

	return FrameIndex + 1;
}

int32 FLiveLinkSubject::FindNewFrame_SceneTime(const FQualifiedFrameTime& QualifiedFrameTime, const FLiveLinkWorldTime& WorldTime) const
{
	if (QualifiedFrameTime.Time.FloorToFrame() < 0)
	{
		static const FName NAME_NoSceneTime = "LiveLinkSubject_NoSceneTime";
		FLiveLinkLog::ErrorOnce(NAME_NoSceneTime, SubjectKey, TEXT("Trying to add a frame that does not have a valid scene time (timecode). The Subject is '%s'."), *SubjectKey.SubjectName.ToString());
		return INDEX_NONE;
	}

	if (QualifiedFrameTime.Rate != CachedSettings.BufferSettings.TimecodeFrameRate)
	{
		static const FName NAME_WrongFPS = "LiveLinkSubject_WrongFPS";
		FLiveLinkLog::ErrorOnce(NAME_WrongFPS, SubjectKey, TEXT("Trying to add a frame in which the timecode frame rate does not match with the expected frame rate. The Subject is '%s'."), *SubjectKey.SubjectName.ToString());
		return INDEX_NONE;
	}

	// If we do not have a TC set, keep buffering, the TC may be unresponsive for a moment. We do not want to loose data.
	if (FApp::GetCurrentFrameTime().IsSet())
	{
		const FQualifiedFrameTime CurrentSyncTime = FApp::GetCurrentFrameTime().GetValue();
		const FFrameTime CurrentFrameTimeInFrameSpace = CurrentSyncTime.ConvertTo(CachedSettings.BufferSettings.TimecodeFrameRate);
		const FFrameTime CurrentOffsetFrameTime = CurrentFrameTimeInFrameSpace - FFrameTime::FromDecimal(CachedSettings.BufferSettings.TimecodeFrameOffset) - CachedSettings.BufferSettings.ValidTimecodeFrame;
		if (QualifiedFrameTime.Time.AsDecimal() < CurrentOffsetFrameTime.AsDecimal())
		{
			static const FName NAME_InvalidTC = "LiveLinkSubject_InvalidTC";
			FLiveLinkLog::WarningOnce(NAME_InvalidTC, SubjectKey, TEXT("Trying to add a frame in which the timecode has a value too low compare to the engine's timecode. Do you have an invalid offset?. The Subject is '%s'."), *SubjectKey.SubjectName.ToString());
		}
	}

	if (CachedSettings.BufferSettings.bGenerateSubFrame)
	{
		// match with frame number, then look at the world time

		int32 MinInclusive = FrameData.Num() - 1;
		for (; MinInclusive >= 0; --MinInclusive)
		{
			const FFrameNumber FrameFrameNumber = FrameData[MinInclusive].GetBaseData()->MetaData.SceneTime.Time.GetFrame();
			if (QualifiedFrameTime.Time.GetFrame() > FrameFrameNumber)
			{
				break;
			}
		}
		if (MinInclusive < 0)
		{
			return 0;
		}
		++MinInclusive;
		if (MinInclusive >= FrameData.Num())
		{
			return FrameData.Num();
		}

		int32 MaxInclusive = MinInclusive;
		for (; MaxInclusive < FrameData.Num(); ++MaxInclusive)
		{
			const FFrameNumber FrameFrameNumber = FrameData[MaxInclusive].GetBaseData()->MetaData.SceneTime.Time.GetFrame();
			if (QualifiedFrameTime.Time.GetFrame() != FrameFrameNumber)
			{
				break;
			}
		}
		--MaxInclusive;

		const double NewFrameOffsettedTime = WorldTime.GetOffsettedTime();
		int32 FrameIndex = MaxInclusive;
		for (; FrameIndex >= MinInclusive; --FrameIndex)
		{
			const double FrameOffsettedTime = FrameData[FrameIndex].GetBaseData()->WorldTime.GetOffsettedTime();
			if (FrameOffsettedTime <= NewFrameOffsettedTime)
			{
				if (FMath::IsNearlyEqual(FrameOffsettedTime, NewFrameOffsettedTime))
				{
					static const FName NAME_SameWorldSceneTime = "LiveLinkSubject_SameWorldSceneTime";
					FLiveLinkLog::WarningOnce(NAME_SameWorldSceneTime, SubjectKey, TEXT("A new frame data for subjet '%s' has the same timecode and the same time as a previous frame."), *SubjectKey.SubjectName.ToString());
				}
				break;
			}
		}

		return FrameIndex + 1;
	}
	else
	{
		const double NewFrameQFTSeconds = QualifiedFrameTime.AsSeconds();
		int32 FrameIndex = FrameData.Num() - 1;
		for (; FrameIndex >= 0; --FrameIndex)
		{
			const double FrameQFTSeconds = FrameData[FrameIndex].GetBaseData()->MetaData.SceneTime.AsSeconds();
			if (FrameQFTSeconds <= NewFrameQFTSeconds)
			{
				if (FMath::IsNearlyEqual(FrameQFTSeconds, NewFrameQFTSeconds))
				{
					static const FName NAME_SameSceneTime = "LiveLinkSubject_SameSceneTime";
					FLiveLinkLog::WarningOnce(NAME_SameSceneTime, SubjectKey, TEXT("A new frame data for subjet '%s' has the same timecode as a previous frame."), *SubjectKey.SubjectName.ToString());
				}
				break;
			}
		}

		return FrameIndex + 1;
	}
}

int32 FLiveLinkSubject::FindNewFrame_Latest(const FLiveLinkWorldTime& WorldTime) const
{
	return FindNewFrame_WorldTimeInternal(WorldTime);
}

void FLiveLinkSubject::AdjustSubFrame_SceneTime(int32 InFrameIndex)
{
	// We need to generate sub frame after because network timing could affect how the frame come in LiveLink

	const double SourceFrameRate = CachedSettings.BufferSettings.SourceTimecodeFrameRate.AsDecimal(); //ie. 120
	const double TimecodeFrameRate = CachedSettings.BufferSettings.TimecodeFrameRate.AsDecimal(); //ie. 30
	float SubFrameIncrement = TimecodeFrameRate / SourceFrameRate;

	check(CachedSettings.BufferSettings.bGenerateSubFrame);
	check(FrameData[InFrameIndex].GetBaseData()->MetaData.SceneTime.Rate == CachedSettings.BufferSettings.TimecodeFrameRate);

	// find max and lower limit for TC with InFrameIndex
	int32 HigherInclusiveLimit = InFrameIndex;
	int32 LowerInclusiveLimit = InFrameIndex;

	const FFrameNumber FrameNumber = FrameData[InFrameIndex].GetBaseData()->MetaData.SceneTime.Time.FrameNumber;
	for (; LowerInclusiveLimit >= 0; --LowerInclusiveLimit)
	{
		FFrameNumber LowerFrameNumber = FrameData[LowerInclusiveLimit].GetBaseData()->MetaData.SceneTime.Time.FrameNumber;
		if (FrameNumber != LowerFrameNumber)
		{
			break;
		}
	}
	LowerInclusiveLimit = FMath::Clamp(LowerInclusiveLimit + 1, 0, FrameData.Num()-1);

	for (; HigherInclusiveLimit < FrameData.Num(); ++HigherInclusiveLimit)
	{
		FFrameNumber HigherFrameNumber = FrameData[HigherInclusiveLimit].GetBaseData()->MetaData.SceneTime.Time.FrameNumber;
		if (FrameNumber != HigherFrameNumber)
		{
			break;
		}
	}
	HigherInclusiveLimit = FMath::Clamp(HigherInclusiveLimit, LowerInclusiveLimit, FrameData.Num()-1);

	// order them by world time
	check(LowerInclusiveLimit <= HigherInclusiveLimit);
	if (LowerInclusiveLimit < HigherInclusiveLimit)
	{
		struct TLess
		{
			FORCEINLINE bool operator()(const FLiveLinkFrameDataStruct& A, const FLiveLinkFrameDataStruct& B) const
			{
				return A.GetBaseData()->WorldTime.GetOffsettedTime() < B.GetBaseData()->WorldTime.GetOffsettedTime();
			}
		};
		Sort(&FrameData[LowerInclusiveLimit], HigherInclusiveLimit - LowerInclusiveLimit + 1, TLess());

		// generate sub frame

		if (HigherInclusiveLimit - LowerInclusiveLimit >= static_cast<int32>(1.f / SubFrameIncrement))
		{
			static const FName NAME_TooManyFrameForGenerateSubFrame = "LiveLinkSubject_TooManyFrameForGenerateSubFrame";
			FLiveLinkLog::WarningOnce(NAME_TooManyFrameForGenerateSubFrame, SubjectKey, TEXT("For subjet '%s' they are too many frames with the same timecode that exist to create subframe. Check the Frame Rate?"), *SubjectKey.SubjectName.ToString());
			SubFrameIncrement = 1.f / (HigherInclusiveLimit - LowerInclusiveLimit + 1);
		}

		float CurrentIncrement = 0.0;
		for (int32 FrameIndex = LowerInclusiveLimit; FrameIndex <= HigherInclusiveLimit; ++FrameIndex)
		{
			FrameData[FrameIndex].GetBaseData()->MetaData.SceneTime.Time = FFrameTime(FrameNumber, CurrentIncrement);
			CurrentIncrement += SubFrameIncrement;
		}
	}
}

bool FLiveLinkSubject::GetFrameAtWorldTime(const double InSeconds, FLiveLinkSubjectFrameData& OutFrame)
{
	bool bResult = FrameData.Num() != 0;
	if (bResult)
	{
		if (FrameInterpolationProcessor.IsValid())
		{
			bResult = GetFrameAtWorldTime_Interpolated(InSeconds, OutFrame);
		}
		else
		{
			bResult = GetFrameAtWorldTime_Closest(InSeconds, OutFrame);
		}

		if (bResult && !OutFrame.StaticData.IsValid())
		{
			OutFrame.StaticData.InitializeWith(StaticData.GetStruct(), StaticData.GetBaseData());
		}
	}
	else if (IsBufferStatsEnabled())
	{
		IncreaseBufferOverFlowStat();
	}
	return bResult;
}

bool FLiveLinkSubject::GetFrameAtWorldTime_Closest(const double InSeconds, FLiveLinkSubjectFrameData& OutFrame)
{
	check(FrameData.Num() != 0);

	bool bOverflowDetected = false;
	bool bUnderflowDetected = false;
	bool bBuiltFrame = false;
	const double ReadTime = (InSeconds) - CachedSettings.BufferSettings.EngineTimeOffset;
	for (int32 FrameIndex = FrameData.Num() - 1; FrameIndex >= 0; --FrameIndex)
	{
		const double Time = FrameData[FrameIndex].GetBaseData()->WorldTime.GetOffsettedTime();
		if (Time <= ReadTime)
		{
			if (FrameIndex == FrameData.Num() - 1)
			{
				//Copy over the frame directly
				OutFrame.FrameData.InitializeWith(FrameData[FrameIndex]);
				bBuiltFrame = true;

				//If we tried to read above our buffer, stamp an overflow
				bUnderflowDetected = !FMath::IsNearlyEqual(Time, ReadTime);
				break;
			}
			else
			{
				const double NextTime = FrameData[FrameIndex + 1].GetBaseData()->WorldTime.GetOffsettedTime();
				const float BlendWeight = (ReadTime - NextTime) / (NextTime - Time);
				int32 CopyIndex = (BlendWeight > 0.5f) ? FrameIndex : FrameIndex + 1;
				OutFrame.FrameData.InitializeWith(FrameData[CopyIndex].GetStruct(), FrameData[CopyIndex].GetBaseData());
				bBuiltFrame = true;
				break;
			}
		}
	}

	if (!bBuiltFrame)
	{
		// Failed to find an interp point so just take oldest frame
		OutFrame.FrameData.InitializeWith(FrameData[0].GetStruct(), FrameData[0].GetBaseData());
		bUnderflowDetected = true;
	}
	
	if (IsBufferStatsEnabled())
	{
		if (bUnderflowDetected)
		{
			IncreaseBufferUnderFlowStat();
		}

		if (bOverflowDetected)
		{
			IncreaseBufferOverFlowStat();
		}

		FTimedDataInputEvaluationData EvaluationData;
		EvaluationData.DistanceToNewestSampleSeconds = FrameData[FrameData.Num()-1].GetBaseData()->WorldTime.GetOffsettedTime() - ReadTime;
		EvaluationData.DistanceToOldestSampleSeconds = ReadTime - FrameData[0].GetBaseData()->WorldTime.GetOffsettedTime();
		UpdateEvaluationData(EvaluationData);
	}

	return true;
}

bool FLiveLinkSubject::GetFrameAtWorldTime_Interpolated(const double InSeconds, FLiveLinkSubjectFrameData& OutFrame)
{
	check(FrameData.Num() != 0);

	FLiveLinkInterpolationInfo InterpolationInfo;
	const double ReadTime = InSeconds - CachedSettings.BufferSettings.EngineTimeOffset;
	FrameInterpolationProcessor->Interpolate(ReadTime, StaticData, FrameData, OutFrame, InterpolationInfo);

	VerifyInterpolationInfo(InterpolationInfo);

	return true;
}

bool FLiveLinkSubject::GetFrameAtSceneTime(const FQualifiedFrameTime& InTimeInEngineFrameRate, FLiveLinkSubjectFrameData& OutFrame)
{
	bool bResult = FrameData.Num() != 0;
	if (bResult)
	{
		if (FrameInterpolationProcessor.IsValid())
		{
			bResult = GetFrameAtSceneTime_Interpolated(InTimeInEngineFrameRate, OutFrame);
		}
		else
		{
			bResult = GetFrameAtSceneTime_Closest(InTimeInEngineFrameRate, OutFrame);
		}

		if (bResult && !OutFrame.StaticData.IsValid())
		{
			OutFrame.StaticData.InitializeWith(StaticData.GetStruct(), StaticData.GetBaseData());
		}
	}
	else if (IsBufferStatsEnabled())
	{
		IncreaseBufferOverFlowStat();
	}
	return bResult;
}

bool FLiveLinkSubject::GetFrameAtSceneTime_Closest(const FQualifiedFrameTime& InTimeInEngineFrameRate, FLiveLinkSubjectFrameData& OutFrame)
{
	check(FrameData.Num() != 0);

	bool bUnderflowDetected = false;
	bool bOverflowDetected = false;
	bool bBuiltFrame = false;

	const FFrameTime FrameOffset = FQualifiedFrameTime(FFrameTime::FromDecimal(CachedSettings.BufferSettings.TimecodeFrameOffset), CachedSettings.BufferSettings.TimecodeFrameRate).ConvertTo(InTimeInEngineFrameRate.Rate);
	const FFrameTime ReadTime = InTimeInEngineFrameRate.Time - FrameOffset;
	const FQualifiedFrameTime LookupQFrameTime = FQualifiedFrameTime(ReadTime, InTimeInEngineFrameRate.Rate);
	const double TimeInSeconds = LookupQFrameTime.AsSeconds();
	for (int32 FrameIndex = FrameData.Num() - 1; FrameIndex >= 0; --FrameIndex)
	{
		const double FrameASeconds = FrameData[FrameIndex].GetBaseData()->MetaData.SceneTime.AsSeconds();
		if (FrameASeconds <= TimeInSeconds)
		{
			if (FrameIndex == FrameData.Num() - 1)
			{
				//Copy over the frame directly
				OutFrame.FrameData.InitializeWith(FrameData[FrameIndex]);
				bBuiltFrame = true;

				//We were asking for a frame above our newest one?
				bOverflowDetected = !FMath::IsNearlyEqual(FrameASeconds, TimeInSeconds);
				break;
			}
			else
			{
				const double FrameBSeconds = FrameData[FrameIndex+1].GetBaseData()->MetaData.SceneTime.AsSeconds();
				const double BlendWeight = (TimeInSeconds - FrameASeconds) / (FrameBSeconds - FrameASeconds);
				int32 CopyIndex = (BlendWeight > 0.5) ? FrameIndex : FrameIndex + 1;
				OutFrame.FrameData.InitializeWith(FrameData[CopyIndex].GetStruct(), FrameData[CopyIndex].GetBaseData());
				bBuiltFrame = true;
				break;
			}
		}
	}

	if (!bBuiltFrame)
	{
		bUnderflowDetected = true;

		// Failed to find an interp point so just take oldest frame
		OutFrame.FrameData.InitializeWith(FrameData[0].GetStruct(), FrameData[0].GetBaseData());
	}

	if (IsBufferStatsEnabled())
	{
		if (bUnderflowDetected)
		{
			IncreaseBufferUnderFlowStat();
		}

		if (bOverflowDetected)
		{
			IncreaseBufferOverFlowStat();
		}

		FTimedDataInputEvaluationData EvaluationData;
		EvaluationData.DistanceToNewestSampleSeconds = FrameData[FrameData.Num() - 1].GetBaseData()->MetaData.SceneTime.AsSeconds() - TimeInSeconds;
		EvaluationData.DistanceToOldestSampleSeconds = TimeInSeconds - FrameData[0].GetBaseData()->MetaData.SceneTime.AsSeconds();
		UpdateEvaluationData(EvaluationData);
	}

	return true;
}

bool FLiveLinkSubject::GetFrameAtSceneTime_Interpolated(const FQualifiedFrameTime& InTimeInEngineFrameRate, FLiveLinkSubjectFrameData& OutFrame)
{
	check(FrameData.Num() != 0);

	FLiveLinkInterpolationInfo InterpolationInfo;
	const FFrameTime FrameOffset = FQualifiedFrameTime(FFrameTime::FromDecimal(CachedSettings.BufferSettings.TimecodeFrameOffset), CachedSettings.BufferSettings.TimecodeFrameRate).ConvertTo(InTimeInEngineFrameRate.Rate);
	const FFrameTime ReadTime = InTimeInEngineFrameRate.Time - FrameOffset;
	const FQualifiedFrameTime LookupQFrameTime = FQualifiedFrameTime(ReadTime, InTimeInEngineFrameRate.Rate);
	FrameInterpolationProcessor->Interpolate(LookupQFrameTime, StaticData, FrameData, OutFrame, InterpolationInfo);

	if (IsBufferStatsEnabled())
	{
		VerifyInterpolationInfo(InterpolationInfo);
	}

	return true;
}

void FLiveLinkSubject::VerifyInterpolationInfo(const FLiveLinkInterpolationInfo& InterpolationInfo)
{
	if (InterpolationInfo.bOverflowDetected)
	{
		IncreaseBufferOverFlowStat();
	}
	else if (InterpolationInfo.bUnderflowDetected)
	{
		IncreaseBufferUnderFlowStat();
	}

	FTimedDataInputEvaluationData EvaluationData;
	EvaluationData.DistanceToNewestSampleSeconds = InterpolationInfo.ExpectedEvaluationDistanceFromNewestSeconds;
	EvaluationData.DistanceToOldestSampleSeconds = InterpolationInfo.ExpectedEvaluationDistanceFromOldestSeconds;
	UpdateEvaluationData(EvaluationData);
}

bool FLiveLinkSubject::GetLatestFrame(FLiveLinkSubjectFrameData& OutFrame)
{
	bool bResult = FrameData.Num() != 0;
	if (bResult)
	{
		bool bUnderflowDetected = false;
		bool bOverflowDetected = false;

		int32 Index = FrameData.Num() - 1 - CachedSettings.BufferSettings.LatestOffset;
		if (Index >= FrameData.Num())
		{
			Index = FrameData.Num() - 1;
			bOverflowDetected = true;
		}
		else if (Index < 0)
		{
			Index = 0;
			bUnderflowDetected = true;
		}

		check(FrameData.IsValidIndex(Index));

		FLiveLinkFrameDataStruct& LastDataStruct = FrameData[Index];
		OutFrame.FrameData.InitializeWith(LastDataStruct.GetStruct(), LastDataStruct.GetBaseData());
		OutFrame.StaticData.InitializeWith(StaticData.GetStruct(), StaticData.GetBaseData());

		if (IsBufferStatsEnabled())
		{
			if (bUnderflowDetected)
			{
				IncreaseBufferUnderFlowStat();
			}

			if (bOverflowDetected)
			{
				IncreaseBufferOverFlowStat();
			}

			FTimedDataInputEvaluationData EvaluationData;
			EvaluationData.DistanceToNewestSampleSeconds = FrameData[FrameData.Num() - 1].GetBaseData()->WorldTime.GetOffsettedTime() - OutFrame.FrameData.GetBaseData()->WorldTime.GetOffsettedTime();
			EvaluationData.DistanceToOldestSampleSeconds = OutFrame.FrameData.GetBaseData()->WorldTime.GetOffsettedTime() - FrameData[0].GetBaseData()->MetaData.SceneTime.AsSeconds();
			UpdateEvaluationData(EvaluationData);
		}
	}

	return bResult;
}

void FLiveLinkSubject::ResetFrame(FLiveLinkSubjectFrameData& OutFrame) const
{
	//Allocate and copy over our static data for that frame.
	OutFrame.StaticData.InitializeWith(StaticData);

	//Only reset the frame data. Copy will be done later on depending on sampling type
	OutFrame.FrameData.Reset();
}

void FLiveLinkSubject::IncreaseFrameDroppedStat()
{
	++EvaluationStatistics.FrameDrop;
}

void FLiveLinkSubject::IncreaseBufferUnderFlowStat()
{
	++EvaluationStatistics.BufferUnderflow;
}

void FLiveLinkSubject::IncreaseBufferOverFlowStat()
{
	++EvaluationStatistics.BufferOverflow;
}

void FLiveLinkSubject::UpdateEvaluationData(const FTimedDataInputEvaluationData& EvaluationData)
{
	FScopeLock Lock(&StatisticCriticalSection);
	EvaluationStatistics.LastEvaluationData = EvaluationData;
}

void FLiveLinkSubject::SetStaticData(TSubclassOf<ULiveLinkRole> InRole, FLiveLinkStaticDataStruct&& InStaticData)
{
	check(IsInGameThread());

	if (Role == nullptr)
	{
		static const FName NAME_NoRoleForSubject = "LiveLinkSubject_NoRoleForSubject";
		FLiveLinkLog::WarningOnce(NAME_NoRoleForSubject, SubjectKey, TEXT("Setting static data for Subject '%s' before it was initialized."), *SubjectKey.SubjectName.ToString());
		return;
	}

	if(Role == InRole)
	{
		//Set initial blending processor to the role's default one. User will be able to modify it afterwards.
		FrameData.Reset();
		StaticData = MoveTemp(InStaticData);
	}
	else
	{
		static const FName NAME_DifferentRole = "LiveLinkSubject_DifferentRole";
		FLiveLinkLog::WarningOnce(NAME_DifferentRole, SubjectKey, TEXT("Subject '%s' received data of role %s but was already registered with a different role"), *SubjectKey.SubjectName.ToString(), *InRole->GetName());
	}	
}

void FLiveLinkSubject::CacheSettings(ULiveLinkSourceSettings* SourceSetting, ULiveLinkSubjectSettings* SubjectSetting)
{
	check(IsInGameThread());

	if (SourceSetting)
	{
		const bool bSourceModeChanged = SourceSetting->Mode != CachedSettings.SourceMode;
		const bool bTimecodeFrameRateChanged = SourceSetting->Mode == ELiveLinkSourceMode::Timecode && SourceSetting->BufferSettings.TimecodeFrameRate != CachedSettings.BufferSettings.TimecodeFrameRate;
		const bool bGenerateSubFrameChanged = SourceSetting->Mode == ELiveLinkSourceMode::Timecode && SourceSetting->BufferSettings.bGenerateSubFrame != CachedSettings.BufferSettings.bGenerateSubFrame;
		if (bSourceModeChanged || bTimecodeFrameRateChanged || bGenerateSubFrameChanged)
		{
			FrameData.Reset();
		}

		CachedSettings.SourceMode = SourceSetting->Mode;
		CachedSettings.BufferSettings = SourceSetting->BufferSettings;

		// Test and update values
		{
			CachedSettings.BufferSettings.MaxNumberOfFrameToBuffered = FMath::Max(CachedSettings.BufferSettings.MaxNumberOfFrameToBuffered, 1);
			if (CachedSettings.BufferSettings.bGenerateSubFrame)
			{
				const double SourceFrameRate = CachedSettings.BufferSettings.SourceTimecodeFrameRate.AsDecimal(); //ie. 120
				const double TimecodeFrameRate = CachedSettings.BufferSettings.TimecodeFrameRate.AsDecimal(); //ie. 30
				if (SourceFrameRate <= TimecodeFrameRate)
				{
					CachedSettings.BufferSettings.bGenerateSubFrame = false;

					static const FName NAME_CanGenerateSubFrame = "LiveLinkSubject_CantGenerateSubFrame";
					FLiveLinkLog::WarningOnce(NAME_CanGenerateSubFrame, SubjectKey, TEXT("Can't generate Sub Frame because the 'Timecode Frame Rate' is bigger or equal to the 'Source Timecode Frame Rate'"));

				}
			}
		}

		// Create a new or fetch the PreProcessors for this frame
		FramePreProcessors.Reset();
		for (ULiveLinkFramePreProcessor* PreProcessor : SubjectSetting->PreProcessors)
		{
			if (PreProcessor)
			{
				ULiveLinkFramePreProcessor::FWorkerSharedPtr NewPreProcessor = PreProcessor->FetchWorker();
				if (NewPreProcessor.IsValid())
				{
					FramePreProcessors.Add(NewPreProcessor);
				}
			}
		}

		// Create a new or fetch the interpolation for this frame
		FrameInterpolationProcessor.Reset();
		if (SubjectSetting->InterpolationProcessor)
		{
			FrameInterpolationProcessor = SubjectSetting->InterpolationProcessor->FetchWorker();
		}

		// Create a new or fetch the translators for this frame
		FrameTranslators.Reset();
		for (ULiveLinkFrameTranslator* Translator : SubjectSetting->Translators)
		{
			if (Translator)
			{
				ULiveLinkFrameTranslator::FWorkerSharedPtr NewTranslator = Translator->FetchWorker();
				if (NewTranslator.IsValid())
				{
					FrameTranslators.Add(NewTranslator);
				}
			}
		}
	}
}

FLiveLinkSubjectTimeSyncData FLiveLinkSubject::GetTimeSyncData()
{
	FLiveLinkSubjectTimeSyncData SyncData;
	SyncData.bIsValid = FrameData.Num() > 0;

	if (SyncData.bIsValid)
	{
		SyncData.NewestSampleTime = FrameData.Last().GetBaseData()->MetaData.SceneTime.Time;
		SyncData.OldestSampleTime = FrameData[0].GetBaseData()->MetaData.SceneTime.Time;
		SyncData.SampleFrameRate = FrameData[0].GetBaseData()->MetaData.SceneTime.Rate;
	}

	return SyncData;
}

bool FLiveLinkSubject::IsTimeSynchronized() const
{
	if (GetMode() == ELiveLinkSourceMode::Timecode)
	{
		const FLiveLinkSubjectFrameData& Snapshot = GetFrameSnapshot();
		if (Snapshot.StaticData.IsValid() && Snapshot.FrameData.IsValid() && Snapshot.FrameData.GetBaseData() && FApp::GetCurrentFrameTime().IsSet())
		{
			const FQualifiedFrameTime CurrentQualifiedFrameTime = FApp::GetCurrentFrameTime().GetValue();
			const FFrameNumber FrameDataInEngineFrameNumber = Snapshot.FrameData.GetBaseData()->MetaData.SceneTime.ConvertTo(CurrentQualifiedFrameTime.Rate).GetFrame();
			const FFrameNumber CurrentEngineFrameNumber = CurrentQualifiedFrameTime.Time.GetFrame();
			return FrameDataInEngineFrameNumber == CurrentEngineFrameNumber;
		}
	}
	return false;
}

/**
 * ITimedDataInput interface
 */
 namespace LiveLinkSubjectTimedDataInput
 {
	 ETimedDataInputEvaluationType ToTimedDataInputEvaluationType(ELiveLinkSourceMode SourceMode)
	 {
		 switch (SourceMode)
		 {
		 case ELiveLinkSourceMode::EngineTime:
			 return ETimedDataInputEvaluationType::PlatformTime;
		 case ELiveLinkSourceMode::Timecode:
			 return ETimedDataInputEvaluationType::Timecode;
		 case ELiveLinkSourceMode::Latest:
		 default:
			 return ETimedDataInputEvaluationType::None;
		 }
		 return ETimedDataInputEvaluationType::None;
	 }

	 ELiveLinkSourceMode ToLiveLinkSourceMode(ETimedDataInputEvaluationType EvaluationType)
	 {
		 switch (EvaluationType)
		 {
		 case ETimedDataInputEvaluationType::PlatformTime:
			 return ELiveLinkSourceMode::EngineTime;
		 case ETimedDataInputEvaluationType::Timecode:
			 return ELiveLinkSourceMode::Timecode;
		 case ETimedDataInputEvaluationType::None:
		 default:
			 return ELiveLinkSourceMode::Latest;
		 }
		 return ELiveLinkSourceMode::Latest;
	 }
 }

ITimedDataInputGroup* FLiveLinkSubject::GetGroup() const
{
	return TimedDataGroup.Pin().Get();
}

ETimedDataInputState FLiveLinkSubject::GetState() const
{
	bool bHasValidFrame = (FApp::GetCurrentTime() - GetLastPushTime() < GetDefault<ULiveLinkSettings>()->GetTimeWithoutFrameToBeConsiderAsInvalid());
	return (bHasValidFrame && HasValidFrameSnapshot()) ? ETimedDataInputState::Connected : ETimedDataInputState::Unresponsive;
}

FText FLiveLinkSubject::GetDisplayName() const
{
	return FText::FromName(SubjectKey.SubjectName);
}

FTimedDataInputSampleTime FLiveLinkSubject::GetOldestDataTime() const
{
	if (FrameData.Num() > 0)
	{
		return FTimedDataInputSampleTime(FrameData[0].GetBaseData()->WorldTime.GetOffsettedTime(), FrameData[0].GetBaseData()->MetaData.SceneTime);
	}
	return FTimedDataInputSampleTime();
}

FTimedDataInputSampleTime FLiveLinkSubject::GetNewestDataTime() const
{
	if (FrameData.Num() > 0)
	{
		return FTimedDataInputSampleTime(FrameData.Last().GetBaseData()->WorldTime.GetOffsettedTime(), FrameData.Last().GetBaseData()->MetaData.SceneTime);
	}
	return FTimedDataInputSampleTime();
}

TArray<FTimedDataInputSampleTime> FLiveLinkSubject::GetDataTimes() const
{
	TArray<FTimedDataInputSampleTime> Result;
	Result.Reset(FrameData.Num());
	for (const FLiveLinkFrameDataStruct& Data : FrameData)
	{
		Result.Emplace(Data.GetBaseData()->WorldTime.GetOffsettedTime(), Data.GetBaseData()->MetaData.SceneTime);
	}
	return Result;
}

ETimedDataInputEvaluationType FLiveLinkSubject::GetEvaluationType() const
{
	return LiveLinkSubjectTimedDataInput::ToTimedDataInputEvaluationType(CachedSettings.SourceMode);
}

void FLiveLinkSubject::SetEvaluationType(ETimedDataInputEvaluationType Evaluation)
{
	if (TSharedPtr<FLiveLinkTimedDataInputGroup> GroupPinned = TimedDataGroup.Pin())
	{
		GroupPinned->SetEvaluationType(LiveLinkSubjectTimedDataInput::ToLiveLinkSourceMode(Evaluation));
	}
}

double FLiveLinkSubject::GetEvaluationOffsetInSeconds() const
{
	switch (CachedSettings.SourceMode)
	{
	case ELiveLinkSourceMode::EngineTime:
		return CachedSettings.BufferSettings.EngineTimeOffset;
	case ELiveLinkSourceMode::Timecode:
		return ITimedDataInput::ConvertFrameOffsetInSecondOffset(CachedSettings.BufferSettings.TimecodeFrameOffset, CachedSettings.BufferSettings.TimecodeFrameRate);
	case ELiveLinkSourceMode::Latest:
	default:
		return CachedSettings.BufferSettings.LatestOffset;
	}
	return 0.f;
}

void FLiveLinkSubject::SetEvaluationOffsetInSeconds(double Offset)
{
	if (TSharedPtr<FLiveLinkTimedDataInputGroup> GroupPinned = TimedDataGroup.Pin())
	{
		GroupPinned->SetEvaluationOffset(CachedSettings.SourceMode, Offset);
	}
}

FFrameRate FLiveLinkSubject::GetFrameRate() const
{
	switch (CachedSettings.SourceMode)
	{
	case ELiveLinkSourceMode::EngineTime:
	case ELiveLinkSourceMode::Latest:
	default:
		return ITimedDataInput::UnknowedFrameRate;
	case ELiveLinkSourceMode::Timecode:
		return CachedSettings.BufferSettings.TimecodeFrameRate;
	}
	return ITimedDataInput::UnknowedFrameRate;
}

int32 FLiveLinkSubject::GetDataBufferSize() const
{
	return FrameData.Num();
}

void FLiveLinkSubject::SetDataBufferSize(int32 BufferSize) const
{
	if (TSharedPtr<FLiveLinkTimedDataInputGroup> GroupPinned = TimedDataGroup.Pin())
	{
		GroupPinned->SetBufferMaxSize(BufferSize);
	}
}

bool FLiveLinkSubject::IsBufferStatsEnabled() const
{
	return bIsStatLoggingEnabled;
}

void FLiveLinkSubject::SetBufferStatsEnabled(bool bEnable)
{
	if (bEnable && !bIsStatLoggingEnabled)
	{
		//When enabling stat tracking, start clean
		ResetBufferStats();
	}

	bIsStatLoggingEnabled = bEnable;
}

int32 FLiveLinkSubject::GetBufferUnderflowStat() const
{
	return EvaluationStatistics.BufferUnderflow;
}

int32 FLiveLinkSubject::GetBufferOverflowStat() const
{
	return EvaluationStatistics.BufferOverflow;
}

int32 FLiveLinkSubject::GetFrameDroppedStat() const
{
	return EvaluationStatistics.FrameDrop;
}

void FLiveLinkSubject::GetLastEvaluationData(FTimedDataInputEvaluationData& OutEvaluationData) const
{
	FScopeLock Lock(&StatisticCriticalSection);
	OutEvaluationData = EvaluationStatistics.LastEvaluationData;
}

void FLiveLinkSubject::ResetBufferStats()
{
	FScopeLock Lock(&StatisticCriticalSection);
	EvaluationStatistics.BufferUnderflow = 0;
	EvaluationStatistics.BufferOverflow = 0;
	EvaluationStatistics.FrameDrop = 0;
	EvaluationStatistics.LastEvaluationData = FTimedDataInputEvaluationData();
}

