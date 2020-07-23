// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Async/TaskGraphInterfaces.h"
#include "CoreMinimal.h"
#include "InterchangeManager.h"
#include "Stats/Stats.h"
#include "UObject/WeakObjectPtrTemplates.h"

namespace Interchange
{

class FTaskTranslator
{
private:
	int32 SourceIndex = INDEX_NONE;
	TWeakPtr<Interchange::FImportAsyncHelper> WeakAsyncHelper;

public:
	FTaskTranslator(int32 InSourceIndex, TWeakPtr<Interchange::FImportAsyncHelper> InAsyncHelper)
		: SourceIndex(InSourceIndex)
		, WeakAsyncHelper(InAsyncHelper)
	{
	}

	static FORCEINLINE ENamedThreads::Type GetDesiredThread()
	{
		return ENamedThreads::AnyBackgroundThreadNormalTask;
	}
	static FORCEINLINE ESubsequentsMode::Type GetSubsequentsMode()
	{
		return ESubsequentsMode::TrackSubsequents;
	}

	FORCEINLINE TStatId GetStatId() const
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(FTaskTranslator, STATGROUP_TaskGraphTasks);
	}

	void DoTask(ENamedThreads::Type CurrentThread, const FGraphEventRef& MyCompletionGraphEvent);
};


} // End namespace Interchange
