// Copyright Epic Games, Inc. All Rights Reserved.

#include "Net/NetDormantHolder.h"

#include "Net/DataReplication.h"
#include "Engine/NetworkObjectList.h"

namespace UE::Net::Private
{

/*-----------------------------------------------------------------------------
	FDormantObjectReplicator
-----------------------------------------------------------------------------*/

FDormantObjectReplicator::FDormantObjectReplicator(FObjectKey InObjectKey)
	: ObjectKey(InObjectKey)
	, Replicator(MakeShared<FObjectReplicator>())
{
	//nothing
}

FDormantObjectReplicator::FDormantObjectReplicator(FObjectKey InObjectKey, const TSharedRef<FObjectReplicator>& ExistingReplicator)
	: ObjectKey(InObjectKey)
	, Replicator(ExistingReplicator)
{
	//nothing
}

/*-----------------------------------------------------------------------------
	FDormantReplicatorHolder
-----------------------------------------------------------------------------*/

bool FDormantReplicatorHolder::DoesReplicatorExist(AActor* DormantActor, UObject* ReplicatedObject) const
{
	if (const FActorDormantReplicators* ActorReplicators = ActorReplicatorSet.Find(DormantActor))
	{
		const FObjectKey SubObjectKey = ReplicatedObject;

		return (ActorReplicators->DormantReplicators.Find(SubObjectKey) != nullptr);
	}

	return false;
}

TSharedPtr<FObjectReplicator> FDormantReplicatorHolder::FindReplicator(AActor* DormantActor, UObject* ReplicatedObject)
{
	TSharedPtr<FObjectReplicator> ReplicatorPtr;

	if (FActorDormantReplicators* ActorReplicators = ActorReplicatorSet.Find(DormantActor))
	{
		const FObjectKey SubObjectKey = ReplicatedObject;
		
		if (FDormantObjectReplicator* SubObjectReplicator = ActorReplicators->DormantReplicators.Find(SubObjectKey))
		{
			ReplicatorPtr = SubObjectReplicator->Replicator;
		}
	}

	return ReplicatorPtr;
}

TSharedPtr<FObjectReplicator> FDormantReplicatorHolder::FindAndRemoveReplicator(AActor* DormantActor, UObject* ReplicatedObject)
{
	TSharedPtr<FObjectReplicator> ReplicatorPtr;

	if (FActorDormantReplicators* ActorReplicators = ActorReplicatorSet.Find(DormantActor))
	{
		const FObjectKey SubObjectKey = ReplicatedObject;
		FSetElementId Index = ActorReplicators->DormantReplicators.FindId(ReplicatedObject);

		if (Index.IsValidId())
		{
			ReplicatorPtr = ActorReplicators->DormantReplicators[Index].Replicator;
			ActorReplicators->DormantReplicators.Remove(Index);
		}
	}

	return ReplicatorPtr;
}

const TSharedRef<FObjectReplicator>& FDormantReplicatorHolder::CreateAndStoreReplicator(AActor* DormantActor, UObject* ReplicatedObject, bool& bOverwroteExistingReplicator)
{
	FActorDormantReplicators& ActorReplicators = ActorReplicatorSet.FindOrAdd(FActorDormantReplicators(DormantActor));

	const FObjectKey SubObjectKey = ReplicatedObject;	

	// Add a new replicator tied to this object. 
	// If there was already a replicator for the same object in the set, it will be destroyed and overwritten by this new one.
	FSetElementId Index = ActorReplicators.DormantReplicators.Add(FDormantObjectReplicator(SubObjectKey), &bOverwroteExistingReplicator);

	return ActorReplicators.DormantReplicators[Index].Replicator;
}

void FDormantReplicatorHolder::StoreReplicator(AActor* DormantActor, UObject* ReplicatedObject, const TSharedRef<FObjectReplicator>& ObjectReplicator)
{
	FActorDormantReplicators& ActorReplicators = ActorReplicatorSet.FindOrAdd(FActorDormantReplicators(DormantActor));

	ActorReplicators.DormantReplicators.Add(FDormantObjectReplicator(ReplicatedObject, ObjectReplicator));
}

void FDormantReplicatorHolder::RemoveStoredReplicator(AActor* DormantActor, FObjectKey ReplicatedObjectKey)
{
	FSetElementId Index = ActorReplicatorSet.FindId(DormantActor);
	if (Index.IsValidId())
	{
		ActorReplicatorSet[Index].DormantReplicators.Remove(ReplicatedObjectKey);

		// Cleanup the actor entry if its not holding any other replicators
		if (ActorReplicatorSet[Index].DormantReplicators.IsEmpty())		
		{
			ActorReplicatorSet.Remove(Index);
		}
	}
}

void FDormantReplicatorHolder::CleanupAllReplicatorsOfActor(AActor* DormantActor)
{
	ActorReplicatorSet.Remove(DormantActor);
}

void FDormantReplicatorHolder::CleanupStaleObjects()
{
	for (FActorReplicatorSet::TIterator ActorSetIt = ActorReplicatorSet.CreateIterator(); ActorSetIt; ++ActorSetIt )
	{
		for (FActorDormantReplicators::FObjectReplicatorSet::TIterator ReplicatorSetIt = ActorSetIt->DormantReplicators.CreateIterator(); ReplicatorSetIt; ++ReplicatorSetIt)
		{
			FDormantObjectReplicator& DormantReplicator = *ReplicatorSetIt;

			if (!DormantReplicator.Replicator->GetWeakObjectPtr().IsValid())
			{
				ReplicatorSetIt.RemoveCurrent();
			}
		}
		
		if (ActorSetIt->DormantReplicators.IsEmpty())
		{
			ActorSetIt.RemoveCurrent();
		}
	}
}

void FDormantReplicatorHolder::CleanupStaleObjects(FNetworkObjectList& NetworkObjectList, UObject* ReferenceOwner)
{
#if UE_REPLICATED_OBJECT_REFCOUNTING
	TArray<TWeakObjectPtr<UObject>, TInlineAllocator<16>> CleanedUpObjects;

	for (FActorReplicatorSet::TIterator ActorSetIt = ActorReplicatorSet.CreateIterator(); ActorSetIt; ++ActorSetIt)
	{
		for (FActorDormantReplicators::FObjectReplicatorSet::TIterator ReplicatorSetIt = ActorSetIt->DormantReplicators.CreateIterator(); ReplicatorSetIt; ++ReplicatorSetIt)
		{
			FDormantObjectReplicator& DormantReplicator = *ReplicatorSetIt;
			TWeakObjectPtr<UObject> DormantObjectPtr = DormantReplicator.Replicator->GetWeakObjectPtr();

			if (!DormantObjectPtr.IsValid())
			{
				// If it's a subobject
				if (ActorSetIt->OwnerActor != DormantReplicator.Replicator->GetObject())
				{
					CleanedUpObjects.Add(DormantObjectPtr);
				}

				ReplicatorSetIt.RemoveCurrent();
			}
		}

		if (ActorSetIt->DormantReplicators.IsEmpty())
		{
			ActorSetIt.RemoveCurrent();
		}

		if (CleanedUpObjects.Num() > 0)
		{
			NetworkObjectList.RemoveMultipleSubObjectChannelReference(ActorSetIt->OwnerActor, CleanedUpObjects, ReferenceOwner);
			CleanedUpObjects.Reset();
		}
	}
#else
	CleanupStaleObjects();
#endif
}

void FDormantReplicatorHolder::ForEachDormantReplicator(UE::Net::FExecuteForEachDormantReplicator Function)
{
	for (const FActorDormantReplicators& ActorReplicators : ActorReplicatorSet)
	{
		for (const FDormantObjectReplicator& DormantReplicator : ActorReplicators.DormantReplicators)
		{
			Function(ActorReplicators.OwnerActor, DormantReplicator.ObjectKey, DormantReplicator.Replicator);
		}
	}
}

void FDormantReplicatorHolder::ForEachDormantReplicatorOfActor(AActor* DormantActor, UE::Net::FExecuteForEachDormantReplicator Function)
{
	if (FActorDormantReplicators* ActorReplicators = ActorReplicatorSet.Find(DormantActor))
	{
		for (const FDormantObjectReplicator& DormantReplicator : ActorReplicators->DormantReplicators)
		{
			Function(ActorReplicators->OwnerActor, DormantReplicator.ObjectKey, DormantReplicator.Replicator);
		}
	}
}

void FDormantReplicatorHolder::EmptySet()
{
	ActorReplicatorSet.Empty();
}

void FDormantReplicatorHolder::CountBytes(FArchive& Ar) const
{
	ActorReplicatorSet.CountBytes(Ar);
	for (const FActorDormantReplicators& ActorReplicators : ActorReplicatorSet)
	{
		ActorReplicators.CountBytes(Ar);
	}
}


} //end namespace UE::Net::Private