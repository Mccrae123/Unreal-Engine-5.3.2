// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Containers/Map.h"
#include "EditorConfigBase.h"
#include "Filters/SCustomTextFilterDialog.h"

#include "FilterBarConfig.generated.h"

USTRUCT()
struct FCustomTextFilterState
{
	GENERATED_BODY()
	
public:

	/* Whether the custom filter is checked, i.e visible in the filter bar */
	UPROPERTY()
	bool bIsChecked = false;

	/* Whether the custom filter is active, i.e visible and enabled in the filter bar */
	UPROPERTY()
	bool bIsActive = false;

	/* The data inside the custom text filter */
	UPROPERTY()
	FCustomTextFilterData FilterData;
	
};

USTRUCT()
struct FFilterBarSettings
{
	GENERATED_BODY()

public:

	/** Map of currently visible custom filters, along with their enabled state */
	UPROPERTY()
	TMap<FString, bool> CustomFilters;

	/** Map of currently visible asset type filters, along with their enabled state */
	UPROPERTY()
	TMap<FString, bool> TypeFilters;

	/** Array of custom text filters the user has created */
	UPROPERTY()
	TArray<FCustomTextFilterState> CustomTextFilters;

	void Empty()
	{
		CustomFilters.Empty();
		TypeFilters.Empty();
		CustomTextFilters.Empty();
	}
};

UCLASS(EditorConfig="FilterBar")
class EDITORWIDGETS_API UFilterBarConfig : public UEditorConfigBase
{
	GENERATED_BODY()
	
public:

	static void Initialize();
	static UFilterBarConfig* Get() { return Instance; }
	
	UPROPERTY(meta=(EditorConfig))
	TMap<FName, FFilterBarSettings> FilterBars;
	
private:

	static TObjectPtr<UFilterBarConfig> Instance;
};
