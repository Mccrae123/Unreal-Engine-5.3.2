// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Templates/SubclassOf.h"

#include "PCGActorSelector.generated.h"

class AActor;
class UPCGComponent;
class UWorld;

UENUM()
enum class EPCGActorSelection : uint8
{
	ByTag,
	ByName,
	ByClass
};

UENUM()
enum class EPCGActorFilter : uint8
{
	/** This actor (either the original PCG actor or the partition actor if partitioning is enabled). */
	Self,
	/** The parent of this actor in the hierarchy. */
	Parent,
	/** The top most parent of this actor in the hierarchy. */
	Root,
	/** All actors in world. */
	AllWorldActors,
	/** The source PCG actor (rather than the generated partition actor). */
	Original,
};

USTRUCT(BlueprintType)
struct FPCGActorSelectorSettings
{
	GENERATED_BODY()

	/** Which actors to consider. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings)
	EPCGActorFilter ActorFilter = EPCGActorFilter::Self;

	/** Whether to consider child actors. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (EditCondition = "ActorFilter!=EPCGActorFilter::AllWorldActors", EditConditionHides))
	bool bIncludeChildren = false;

	/** Enables/disables fine-grained actor filtering options. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (EditCondition = "ActorFilter!=EPCGActorFilter::AllWorldActors && bIncludeChildren", EditConditionHides))
	bool bDisableFilter = false;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (EditCondition = "(ActorFilter==EPCGActorFilter::AllWorldActors || (bIncludeChildren && !bDisableFilter))", EditConditionHides))
	EPCGActorSelection ActorSelection = EPCGActorSelection::ByTag;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (EditCondition = "(ActorFilter==EPCGActorFilter::AllWorldActors || (bIncludeChildren && !bDisableFilter)) && ActorSelection==EPCGActorSelection::ByTag", EditConditionHides))
	FName ActorSelectionTag;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (EditCondition = "(ActorFilter==EPCGActorFilter::AllWorldActors || (bIncludeChildren && !bDisableFilter)) && ActorSelection==EPCGActorSelection::ByName", EditConditionHides))
	FName ActorSelectionName;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (EditCondition = "(ActorFilter==EPCGActorFilter::AllWorldActors || (bIncludeChildren && !bDisableFilter)) && ActorSelection==EPCGActorSelection::ByClass", EditConditionHides))
	TSubclassOf<AActor> ActorSelectionClass;

	/** If true processes all matching actors, otherwise returns data from first match. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (EditCondition = "ActorFilter==EPCGActorFilter::AllWorldActors && ActorSelection!=EPCGActorSelection::ByName", EditConditionHides))
	bool bSelectMultiple = false;
};

namespace PCGActorSelector
{
	TArray<AActor*> FindActors(const FPCGActorSelectorSettings& Settings, const UPCGComponent* InComponent);
	AActor* FindActor(const FPCGActorSelectorSettings& InSettings, UPCGComponent* InComponent);
}
