// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.
#pragma once

#include "UObject/Object.h"
#include "MoviePipelineSetting.generated.h"

class UMoviePipeline;
struct FSlateBrush;

/**
* A base class for all Movie Render Pipeline settings.
*/
UCLASS(Blueprintable, Abstract)
class MOVIERENDERPIPELINECORE_API UMoviePipelineSetting : public UObject
{
	GENERATED_BODY()
		
public:
	/**
	* Called when the Pipeline is initialized for the first time before the first frame is ticked.
	*/
	void OnInitializedForPipeline(UMoviePipeline* InPipeline);

	// UObject Interface
	virtual UWorld* GetWorld() const override;
	// ~UObject Interface

protected:
	UMoviePipeline* GetPipeline() const;


	virtual void OnInitializedForPipelineImpl(UMoviePipeline* InPipeline) {}

public:
#if WITH_EDITOR
	virtual FText GetDisplayText() const { return this->GetClass()->GetDisplayNameText(); }
#endif
	/** Should the Pipeline automatically create an instance of this under the hood so calling code can rely on it existing? */
	virtual bool IsRequired() const { return false; }
	
	/** Can only one of these settings objects be active in a valid pipeline? */
	virtual bool IsSolo() const { return true; }
	
	/** Is this setting valid? Return false and add a reason it's not valid to the array if not. */
	virtual bool ValidatePipeline(TArray<FText>& OutValidationErrors) const { return true; }
	
	/** What icon should this setting use when displayed in the tree list. */
	const FSlateBrush* GetDisplayIcon() { return nullptr; }
	
	/** What tooltip should be displayed for this setting when hovered in the tree list? */
	FText GetDescriptionText() { return FText(); }
	
	/** Is this setting currently enabled? Disabled settings are like they never existed. */
	bool bEnabled;

private:
	UPROPERTY(Transient)
	TWeakObjectPtr<UMoviePipeline> CachedPipeline;
};