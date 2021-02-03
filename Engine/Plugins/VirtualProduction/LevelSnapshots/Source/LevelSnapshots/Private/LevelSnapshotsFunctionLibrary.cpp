// Copyright Epic Games, Inc. All Rights Reserved.

#include "LevelSnapshotsFunctionLibrary.h"

#include "EngineUtils.h"
#include "LevelSnapshot.h"
#include "LevelSnapshotFilters.h"
#include "Engine/LevelStreaming.h"

#if WITH_EDITOR
#include "DiffUtils.h"
#include "UnrealEdGlobals.h"
#include "Editor/UnrealEdEngine.h"
#endif

ULevelSnapshot* ULevelSnapshotsFunctionLibrary::TakeLevelSnapshot(const UObject* WorldContextObject, const FName& NewSnapshotName)
{
	UWorld* TargetWorld = nullptr;
	if (WorldContextObject)
	{
		TargetWorld = WorldContextObject->GetWorld();
	}

	if (TargetWorld)
	{
		UE_LOG(LogTemp, Warning, TEXT("Snapshot taken in World Type - %d"), TargetWorld->WorldType);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("Snapshot taken with no valid World set"));
		return nullptr;
	}

	ULevelSnapshot* NewSnapshot = NewObject<ULevelSnapshot>(GetTransientPackage(), NewSnapshotName);

	NewSnapshot->SnapshotWorld(TargetWorld);

	return NewSnapshot;
};

void ULevelSnapshotsFunctionLibrary::ApplySnapshotToWorld(const UObject* WorldContextObject, const ULevelSnapshot* Snapshot, const ULevelSnapshotFilter* Filter /*= nullptr*/)
{
	UWorld* TargetWorld = nullptr;
	if (WorldContextObject)
	{
		TargetWorld = WorldContextObject->GetWorld();
	}

	if (!TargetWorld)
	{
		return;
	}

	{
#if WITH_EDITOR
		FScopedTransaction Transaction(FText::FromString("Loading Level Snapshot."));
#endif // #if WITH_EDITOR
		for (TActorIterator<AActor> It(TargetWorld, AActor::StaticClass(), EActorIteratorFlags::SkipPendingKill); It; ++It)
		{
			AActor* Actor = *It;

#if WITH_EDITOR
			// For now only snapshot the actors which would be visible in the scene outliner to avoid complications with special hidden actors
			if (!Actor->IsListedInSceneOutliner())
			{
				continue;
			}
#endif // #if WITH_EDITOR

			for (const TPair<FString, FLevelSnapshot_Actor>& SnapshotPair : Snapshot->ActorSnapshots)
			{
				const FString& SnapshotPathName = SnapshotPair.Key;
				const FLevelSnapshot_Actor& ActorSnapshot = SnapshotPair.Value;

				// See if the Snapshot is for the same actor
				if (ActorSnapshot.CorrespondsTo(Actor))
				{
					ActorSnapshot.Deserialize(Actor, Filter);
					break;
				}
			}
		}
	}

	// If we're in the editor then update the gizmos locations as they can get out of sync if any of the deserialized actors were selected
#if WITH_EDITOR
	if (GUnrealEd)
	{
		GUnrealEd->UpdatePivotLocationForSelection();
	}
#endif
}

void PrintObjectDifferences(const AActor* A, const AActor* B)
{
#if WITH_EDITOR
	if (!A || !B)
	{
		return;
	}

	UE_LOG(LogTemp, Warning, TEXT("\t--Calculating Differences--"));

	TArray<FSingleObjectDiffEntry> DifferingProperties;
	DiffUtils::CompareUnrelatedObjects(A, B, DifferingProperties);
	for (FSingleObjectDiffEntry& DifferingProperty : DifferingProperties)
	{
		FString PropertyDisplayName = DifferingProperty.Identifier.ToDisplayName();
		UE_LOG(LogTemp, Warning, TEXT("\tProperty Difference: %s"), *PropertyDisplayName);
	}
#endif // #if WITH_EDITOR
}

void ULevelSnapshotsFunctionLibrary::TestDeserialization(const ULevelSnapshot* Snapshot, AActor* TestActor)
{
	if (!TestActor)
	{
		return;
	}

	if (Snapshot && Snapshot->ActorSnapshots.Num() > 0)
	{
		for (const TPair<FString, FLevelSnapshot_Actor>& SnapshotPair : Snapshot->ActorSnapshots)
		{
			const FString& SnapshotPathName = SnapshotPair.Key;
			const FLevelSnapshot_Actor& ActorSnapshot = SnapshotPair.Value;

			// See if the Snapshot is for the same actor
			if (SnapshotPathName == TestActor->GetPathName())
			{
				UE_LOG(LogTemp, Warning, TEXT("Found matching snapshot!"));

				UE_LOG(LogTemp, Warning, TEXT("\tOld Transform: %s"), *TestActor->GetActorLocation().ToString());
				ActorSnapshot.Deserialize(TestActor);
				UE_LOG(LogTemp, Warning, TEXT("\tNew Transform: %s"), *TestActor->GetActorLocation().ToString());
			}
		}
	}
}

void ULevelSnapshotsFunctionLibrary::DiffSnapshots(const ULevelSnapshot* FirstSnapshot, const ULevelSnapshot* SecondSnapshot, TMap<FString, FLevelSnapshot_ActorDiff>& DiffResults)
{
#if WITH_EDITOR
	if (!FirstSnapshot || !SecondSnapshot)
	{
		UE_LOG(LogTemp, Warning, TEXT("Unable to Diff snapshots as at least one snapshot was invalid"));
		return;
	}

	DiffResults.Empty();
	
	for (const TPair<FString, FLevelSnapshot_Actor>& FirstSnapshotPair : FirstSnapshot->ActorSnapshots)
	{
		const FString& FirstSnapshotPathName = FirstSnapshotPair.Key;
		const FLevelSnapshot_Actor& FirstActorSnapshot = FirstSnapshotPair.Value;

		if (const FLevelSnapshot_Actor* SecondActorSnapshot = SecondSnapshot->ActorSnapshots.Find(FirstSnapshotPathName))
		{
			UE_LOG(LogTemp, Warning, TEXT("Found Matching Actor: %s"), *FirstSnapshotPathName);

			AActor* FirstActor = FirstActorSnapshot.GetDeserializedActor();
			AActor* SecondActor = SecondActorSnapshot->GetDeserializedActor();

			TArray<FString> ModifiedProperties;

			if (FirstActor && SecondActor)
			{
				TArray<FSingleObjectDiffEntry> DifferingProperties;
				DiffUtils::CompareUnrelatedObjects(FirstActor, SecondActor, DifferingProperties);
				for (FSingleObjectDiffEntry& DifferingProperty : DifferingProperties)
				{
					if (DifferingProperty.DiffType == EPropertyDiffType::PropertyValueChanged)
					{
						ModifiedProperties.Emplace(DifferingProperty.Identifier.ToDisplayName());
					}
				}
				FirstActor->Destroy();
				SecondActor->Destroy();
			}

			if (ModifiedProperties.Num())
			{
				DiffResults.Add(FirstSnapshotPathName, { ModifiedProperties });
			}
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("%s exists in the First snapshot but not the Second."), *FirstSnapshotPathName);
		}
	}

	for (const TPair<FString, FLevelSnapshot_Actor>& SecondSnapshotPair : SecondSnapshot->ActorSnapshots)
	{
		const FString& SecondSnapshotPathName = SecondSnapshotPair.Key;
		const FLevelSnapshot_Actor& SecondActorSnapshot = SecondSnapshotPair.Value;

		if (!FirstSnapshot->ActorSnapshots.Find(SecondSnapshotPathName))
		{
			UE_LOG(LogTemp, Warning, TEXT("%s exists in the Second snapshot but not the First."), *SecondSnapshotPathName);
		}
	}
#endif // #if WITH_EDITOR
}
