// Copyright Epic Games, Inc. All Rights Reserved.

#include "RemoteControlEntity.h"

#include "Algo/Transform.h"
#include "RemoteControlBinding.h"
#include "RemoteControlPreset.h"

TArray<UObject*> FRemoteControlEntity::GetBoundObjects() const
{
	TArray<UObject*> ResolvedObjects;
	ResolvedObjects.Reserve(Bindings.Num());
	Algo::TransformIf(Bindings, ResolvedObjects,
		[](TWeakObjectPtr<URemoteControlBinding> WeakBinding) { return WeakBinding.IsValid(); },
		[](TWeakObjectPtr<URemoteControlBinding> WeakBinding) { return WeakBinding->Resolve(); });

	return ResolvedObjects.FilterByPredicate([](const UObject* Object){ return !!Object; });
}

const TArray<TWeakObjectPtr<URemoteControlBinding>>& FRemoteControlEntity::GetBindings() const
{
	return Bindings;
}

const TMap<FName, FString>& FRemoteControlEntity::GetMetadata() const
{
	return UserMetadata;
}

void FRemoteControlEntity::RemoveMetadataEntry(FName Key)
{
	UserMetadata.Remove(Key);
	OnEntityModifiedDelegate.ExecuteIfBound(Id);
}

void FRemoteControlEntity::SetMetadataValue(FName Key, FString Value)
{
	UserMetadata.FindOrAdd(Key) = Value;
	OnEntityModifiedDelegate.ExecuteIfBound(Id);
}

void FRemoteControlEntity::BindObject(UObject* InObjectToBind)
{
	if (Bindings.Num() && InObjectToBind)
	{
		if (URemoteControlBinding* Binding = Bindings[0].Get())
		{
			Binding->Modify();
			Binding->SetBoundObject(InObjectToBind);
			OnEntityModifiedDelegate.ExecuteIfBound(Id);
		}
	}
}

bool FRemoteControlEntity::IsBound() const
{
	return GetBoundObjects().Num() > 0;
}

bool FRemoteControlEntity::operator==(const FRemoteControlEntity& InEntity) const
{
	return Id == InEntity.Id;
}

bool FRemoteControlEntity::operator==(FGuid InEntityId) const
{
	return Id == InEntityId;
}

FRemoteControlEntity::FRemoteControlEntity(URemoteControlPreset* InPreset, FName InLabel, const TArray<URemoteControlBinding*>& InBindings)
	: Owner(InPreset)
	, Label(InLabel)
	, Id(FGuid::NewGuid())
{
	Bindings.Append(InBindings);
}

FName FRemoteControlEntity::Rename(FName NewLabel)
{
	if (URemoteControlPreset* Preset = Owner.Get())
	{
		Preset->Modify();
		FName NewName = Preset->RenameExposedEntity(Id, NewLabel);
		OnEntityModifiedDelegate.ExecuteIfBound(Id);
		return NewName;
	}

	checkNoEntry();
	return NAME_None;
}

uint32 GetTypeHash(const FRemoteControlEntity& InEntity)
{
	return GetTypeHash(InEntity.Id);
}