// Copyright Epic Games, Inc. All Rights Reserved.

#include "PCGManagedResource.h"
#include "PCGComponent.h"

#include "Components/InstancedStaticMeshComponent.h"

// By default, if it is not a hard release, we mark the resource unused.
bool UPCGManagedResource::Release(bool bHardRelease, TSet<TSoftObjectPtr<AActor>>& /*OutActorsToDelete*/)
{
	if (!bHardRelease)
	{
		bIsMarkedUnused = true;
		return false;
	}

	return true;
}

bool UPCGManagedResource::ReleaseIfUnused(TSet<TSoftObjectPtr<AActor>>& OutActorsToDelete)
{
	if (bIsMarkedUnused)
	{
		Release(true, OutActorsToDelete);
		return true;
	}

	return false;
}

void UPCGManagedActors::PostEditImport()
{
	// In this case, the managed actors won't be copied along the actor/component,
	// So we just have to "forget" the actors.
	Super::PostEditImport();
	GeneratedActors.Reset();
}

bool UPCGManagedActors::Release(bool bHardRelease, TSet<TSoftObjectPtr<AActor>>& OutActorsToDelete)
{
	if (!Super::Release(bHardRelease, OutActorsToDelete))
	{
		return false;
	}

	OutActorsToDelete.Append(GeneratedActors);

	// Cleanup recursively
	TInlineComponentArray<UPCGComponent*, 1> ComponentsToCleanup;

	for (TSoftObjectPtr<AActor> GeneratedActor : GeneratedActors)
	{
		if (GeneratedActor.IsValid())
		{
			GeneratedActor.Get()->GetComponents<UPCGComponent>(ComponentsToCleanup);

			for (UPCGComponent* Component : ComponentsToCleanup)
			{
				Component->CleanupInternal(/*bRemoveComponents=*/false, OutActorsToDelete);
			}

			ComponentsToCleanup.Reset();
		}
	}

	GeneratedActors.Reset();
	return true;
}

bool UPCGManagedActors::ReleaseIfUnused(TSet<TSoftObjectPtr<AActor>>& OutActorsToDelete)
{
	return Super::ReleaseIfUnused(OutActorsToDelete) || GeneratedActors.IsEmpty();
}

void UPCGManagedComponent::PostEditImport()
{
	Super::PostEditImport();

	// Rehook components from the original to the locally duplicated components
	UPCGComponent* OwningComponent = Cast<UPCGComponent>(GetOuter());
	AActor* Actor = OwningComponent ? OwningComponent->GetOwner() : nullptr;

	bool bFoundMatch = false;

	if (Actor && GeneratedComponent.IsValid())
	{
		TInlineComponentArray<UActorComponent*, 16> Components;
		Actor->GetComponents(Components);

		for (UActorComponent* Component : Components)
		{
			if (Component && Component->GetFName() == GeneratedComponent->GetFName())
			{
				GeneratedComponent = Component;
				bFoundMatch = true;
			}
		}

		if (!bFoundMatch)
		{
			// Not quite clear what to do when we have a component that cannot be remapped.
			// Maybe we should check against guids instead?
			GeneratedComponent.Reset();
		}
	}
	else
	{
		// Somewhat irrelevant case, if we don't have an actor or a component, there's not a lot we can do.
		GeneratedComponent.Reset();
	}
}

bool UPCGManagedComponent::Release(bool bHardRelease, TSet<TSoftObjectPtr<AActor>>& /*OutActorsToDelete*/)
{
	const bool bSupportsComponentReset = SupportsComponentReset();
	const bool bDeleteComponent = bHardRelease || !bSupportsComponentReset;

	if (GeneratedComponent.IsValid())
	{
		if (bDeleteComponent)
		{
			GeneratedComponent->DestroyComponent();
		}
		else
		{
			// We can only mark it unused if we can reset the component.
			bIsMarkedUnused = true;
		}
	}

	return bDeleteComponent;
}

bool UPCGManagedComponent::ReleaseIfUnused(TSet<TSoftObjectPtr<AActor>>& OutActorsToDelete)
{
	return Super::ReleaseIfUnused(OutActorsToDelete) || !GeneratedComponent.IsValid();
}

void UPCGManagedComponent::MarkAsUsed()
{
	if (!bIsMarkedUnused)
	{
		return;
	}

	// Can't reuse a resource if we can't reset it. Make sure we never take this path in this case.
	check(SupportsComponentReset());

	ResetComponent();
	bIsMarkedUnused = false;
}

bool UPCGManagedISMComponent::ReleaseIfUnused(TSet<TSoftObjectPtr<AActor>>& OutActorsToDelete)
{
	if (Super::ReleaseIfUnused(OutActorsToDelete) || !GetComponent())
	{
		return true;
	}
	else if (GetComponent()->GetInstanceCount() == 0)
	{
		GeneratedComponent->DestroyComponent();
		return true;
	}
	else
	{
		return false;
	}
}

void UPCGManagedISMComponent::ResetComponent()
{
	if (UInstancedStaticMeshComponent * ISMC = GetComponent())
	{
		ISMC->ClearInstances();
		ISMC->UpdateBounds();
	}
}

UInstancedStaticMeshComponent* UPCGManagedISMComponent::GetComponent() const
{
	return Cast<UInstancedStaticMeshComponent>(GeneratedComponent.Get());
}