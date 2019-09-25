// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RigHierarchyDefines.h"
#include "RigBoneHierarchy.generated.h"

class UControlRig;

USTRUCT(BlueprintType)
struct CONTROLRIG_API FRigBone: public FRigElement
{
	GENERATED_BODY()

	FRigBone()
		: FRigElement()
		, ParentName(NAME_None)
		, ParentIndex(INDEX_NONE)
		, InitialTransform(FTransform::Identity)
		, GlobalTransform(FTransform::Identity)
		, LocalTransform(FTransform::Identity)
		, Dependents()
	{
	}
	virtual ~FRigBone() {}

	UPROPERTY(VisibleAnywhere, Category = FRigElement)
	FName ParentName;

	UPROPERTY(transient)
	int32 ParentIndex;

	/* Initial global transform that is saved in this rig */
	UPROPERTY(EditAnywhere, Category = FRigElement)
	FTransform InitialTransform;

	UPROPERTY(transient, VisibleAnywhere, Category = FRigElement)
	FTransform GlobalTransform;

	UPROPERTY(transient, VisibleAnywhere, Category = FRigElement)
	FTransform LocalTransform;

	/** dependent list - direct dependent for child or anything that needs to update due to this */
	UPROPERTY(transient)
	TArray<int32> Dependents;

	FORCEINLINE virtual ERigElementType GetElementType() const override
	{
		return ERigElementType::Bone;
	}

	FORCEINLINE virtual FRigElementKey GetParentElementKey() const
	{
		return FRigElementKey(ParentName, GetElementType());
	}
};

USTRUCT()
struct CONTROLRIG_API FRigBoneHierarchy
{
	GENERATED_BODY()

	FRigBoneHierarchy();
	FRigBoneHierarchy& operator= (const FRigBoneHierarchy &InOther);

	FORCEINLINE ERigElementType RigElementType() const { return ERigElementType::Bone; }

	FORCEINLINE int32 Num() const { return Bones.Num(); }
	FORCEINLINE const FRigBone& operator[](int32 InIndex) const { return Bones[InIndex]; }
	FORCEINLINE FRigBone& operator[](int32 InIndex) { return Bones[InIndex]; }
	FORCEINLINE const FRigBone& operator[](const FName& InName) const { return Bones[GetIndex(InName)]; }
	FORCEINLINE FRigBone& operator[](const FName& InName) { return Bones[GetIndex(InName)]; }

	FORCEINLINE TArray<FRigBone>::RangedForIteratorType      begin()       { return Bones.begin(); }
	FORCEINLINE TArray<FRigBone>::RangedForConstIteratorType begin() const { return Bones.begin(); }
	FORCEINLINE TArray<FRigBone>::RangedForIteratorType      end()         { return Bones.end();   }
	FORCEINLINE TArray<FRigBone>::RangedForConstIteratorType end() const   { return Bones.end();   }

	FORCEINLINE bool IsNameAvailable(const FName& InPotentialNewName) const { return GetIndex(InPotentialNewName) == INDEX_NONE; }

	FName GetSafeNewName(const FName& InPotentialNewName) const;

	FRigBone& Add(const FName& InNewName, const FName& InParentName = NAME_None, const FTransform& InInitTransform = FTransform::Identity);

	FRigBone& Add(const FName& InNewName, const FName& InParentName, const FTransform& InInitTransform, const FTransform& InLocalTransform, const FTransform& InGlobalTransform);

	FRigBone Remove(const FName& InNameToRemove);

	FName Rename(const FName& InOldName, const FName& InNewName);

	bool Reparent(const FName& InName, const FName& InNewParentName);

	FName GetName(int32 InIndex) const;

	FORCEINLINE int32 GetIndex(const FName& InName) const
	{
		if(NameToIndexMapping.Num() != Bones.Num())
		{
			return GetIndexSlow(InName);
		}

		const int32* Index = NameToIndexMapping.Find(InName);
		if (Index)
		{
			return *Index;
		}

		return INDEX_NONE;
	}

	// list of names of children - this is not cheap, and is supposed to be used only for one time set up
	int32 GetChildren(const FName& InName, TArray<int32>& OutChildren, bool bRecursively) const;

	int32 GetChildren(const int32 InIndex, TArray<int32>& OutChildren, bool bRecursively) const;

	void SetGlobalTransform(const FName& InName, const FTransform& InTransform, bool bPropagateTransform = true);

	void SetGlobalTransform(int32 InIndex, const FTransform& InTransform, bool bPropagateTransform = true);

	FTransform GetGlobalTransform(const FName& InName) const;

	FTransform GetGlobalTransform(int32 InIndex) const;

	void SetLocalTransform(const FName& InName, const FTransform& InTransform, bool bPropagateTransform = true);

	void SetLocalTransform(int32 InIndex, const FTransform& InTransform, bool bPropagateTransform = true);

	FTransform GetLocalTransform(const FName& InName) const;

	FTransform GetLocalTransform(int32 InIndex) const;

	void SetInitialTransform(const FName& InName, const FTransform& InTransform);

	void SetInitialTransform(int32 InIndex, const FTransform& InTransform);

	FTransform GetInitialTransform(const FName& InName) const;

	FTransform GetInitialTransform(int32 InIndex) const;

	// updates all of the internal caches
	void Initialize();

	// clears the hierarchy and removes all content
	void Reset();

	// resets all of the transforms back to the initial transform
	void ResetTransforms();

#if WITH_EDITOR

	bool Select(const FName& InName, bool bSelect = true);
	bool ClearSelection();
	TArray<FName> CurrentSelection() const;
	bool IsSelected(const FName& InName) const;

	FRigElementAdded OnBoneAdded;
	FRigElementRemoved OnBoneRemoved;
	FRigElementRenamed OnBoneRenamed;
	FRigElementReparented OnBoneReparented;
	FRigElementSelected OnBoneSelected;

#endif

private:

	// disable copy constructor
	FRigBoneHierarchy(const FRigBoneHierarchy& InOther) {}

	FRigHierarchyContainer* Container;

	UPROPERTY(EditAnywhere, Category = FRigBoneHierarchy)
	TArray<FRigBone> Bones;

	// can serialize fine? 
	UPROPERTY()
	TMap<FName, int32> NameToIndexMapping;

#if WITH_EDITORONLY_DATA
	UPROPERTY(transient)
	TArray<FName> Selection;
#endif

	int32 GetIndexSlow(const FName& InName) const;

	void RecalculateLocalTransform(FRigBone& InOutBone);
	void RecalculateGlobalTransform(FRigBone& InOutBone);

	void RefreshMapping();
	void Sort();

	// list of names of children - this is not cheap, and is supposed to be used only for one time set up
	int32 GetChildrenRecursive(const int32 InIndex, TArray<int32>& OutChildren, bool bRecursively) const;

	void PropagateTransform(int32 InIndex);
	
	friend struct FRigHierarchyContainer;
};
