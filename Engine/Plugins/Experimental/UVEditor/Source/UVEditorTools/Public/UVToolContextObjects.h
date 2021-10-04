// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "DynamicMesh/DynamicMeshChangeTracker.h" // FDynamicMeshChange for TUniquePtr
#include "InteractiveToolManager.h"
#include "Selection/DynamicMeshSelection.h"
#include "GeometryBase.h"

#include "UVToolContextObjects.generated.h"

// TODO: Should this be spread out across multiple files?

PREDECLARE_GEOMETRY(class FDynamicMesh3);
class FToolCommandChange;
class UInputRouter;
class UWorld;
class UUVEditorToolMeshInput;

UCLASS()
class UVEDITORTOOLS_API UUVToolContextObject : public UObject
{
	GENERATED_BODY()
public:
};

/**
 * An API object meant to be stored in a context object store that allows UV editor tools
 * to emit appropriate undo/redo transactions.
 */
UCLASS()
class UVEDITORTOOLS_API UUVToolEmitChangeAPI : public UUVToolContextObject
{
	GENERATED_BODY()
public:

	void Initialize(TObjectPtr<UInteractiveToolManager> ToolManagerIn)
	{
		ToolManager = ToolManagerIn;
	}

	virtual void BeginUndoTransaction(const FText& Description)
	{
		ToolManager->BeginUndoTransaction(Description);
	}
	virtual void EndUndoTransaction()
	{
		ToolManager->EndUndoTransaction();
	}

	/**
	 * Emit a change that can be undone even if we leave the tool from which it is emitted (as
	 * long as that UV editor instance is still open). 
	 * Minor note: because we undo "out of" tools into a default tool and never out of a default tool,
	 * in practice, tool-independent changes will only ever be applied/reverted in the same tool 
	 * invocation that they were emitted or in the default tool, not in other arbitrary tools.
	 * 
	 * Since tool-independent changes usually operate on UV editor mesh input object, it is probably
	 * preferable to use EmitToolIndependentUnwrapCanonicalChange, which will set up a proper transaction
	 * for you.
	 */
	virtual void EmitToolIndependentChange(UObject* TargetObject, TUniquePtr<FToolCommandChange> Change, const FText& Description);

	/**
	 * A convenience function that is like EmitToolIndependentChange, but uses a FDynamicMeshChange
	 * that operates on the UnwrapCanonical of an input to create a change object that updates the other
	 * views and issues an OnUndoRedo broadcast on the input object.
	 */
	virtual void EmitToolIndependentUnwrapCanonicalChange(UUVEditorToolMeshInput* InputObject,
		TUniquePtr<UE::Geometry::FDynamicMeshChange> UnwrapCanonicalMeshChange, const FText& Description);

	/**
	 * Emits a change that is considered expired when the active tool does not match the tool that was active
	 * when it was emitted.
	 */
	virtual void EmitToolDependentChange(UObject* TargetObject, TUniquePtr<FToolCommandChange> Change, const FText& Description);

protected:
	TObjectPtr<UInteractiveToolManager> ToolManager = nullptr;
};

/**
 * Allows tools to interact with the 3d preview viewport, which has a separate
 * world and input router.
 */
UCLASS()
class UVEDITORTOOLS_API UUVToolLivePreviewAPI : public UUVToolContextObject
{
	GENERATED_BODY()
public:

	void Initialize(UWorld* WorldIn, UInputRouter* RouterIn);

	UWorld* GetLivePreviewWorld() { return World.Get(); }
	UInputRouter* GetLivePreviewInputRouter() { return InputRouter.Get(); }
protected:
	UPROPERTY()
	TWeakObjectPtr<UWorld> World;

	UPROPERTY()
	TWeakObjectPtr<UInputRouter> InputRouter;
};

/**
 * Allows tools to interact with buttons in the viewport (currently just gizmo controls)
 */
UCLASS()
class UVEDITORTOOLS_API UUVToolViewportButtonsAPI : public UUVToolContextObject
{
	GENERATED_BODY()
public:

	enum class EGizmoMode
	{
		Select,
		Transform
	};

	void SetGizmoButtonsEnabled(bool bOn)
	{
		bButtonsEnabled = bOn;
	}

	bool AreGizmoButtonsEnabled()
	{
		return bButtonsEnabled;
	}

	void SetGizmoMode(EGizmoMode ModeIn, bool bBroadcast = true)
	{
		GizmoMode = ModeIn;
		OnGizmoModeChange.Broadcast(GizmoMode);
	}

	EGizmoMode GetGizmoMode()
	{
		return GizmoMode;
	}

	DECLARE_MULTICAST_DELEGATE_OneParam(FOnGizmoModeChange, EGizmoMode NewGizmoMode);
	FOnGizmoModeChange OnGizmoModeChange;

protected:

	bool bButtonsEnabled = false;
	EGizmoMode GizmoMode = EGizmoMode::Select;
};


/**
 * Allows tools to interact with the assets and their UV layers
*/
UCLASS()
class UVEDITORTOOLS_API UUVToolAssetAndChannelAPI : public UUVToolContextObject
{
	GENERATED_BODY()
public:

	TArray<int32> GetCurrentChannelVisibility()
	{
		if (GetCurrentChannelVisibilityFunc)
		{
			return GetCurrentChannelVisibilityFunc();
		}
		return TArray<int32>();
	}

	void RequestChannelVisibilityChange(const TArray<int32>& ChannelPerAsset, bool bForceRebuildUnwrap=false, bool bEmitUndoTransaction=true)
	{
		if (RequestChannelVisibilityChangeFunc)
		{
			RequestChannelVisibilityChangeFunc(ChannelPerAsset, bForceRebuildUnwrap, bEmitUndoTransaction);
		}
	}

	void NotifyOfAssetChannelCountChange(int32 AssetID)
	{
		if (NotifyOfAssetChannelCountChangeFunc)
		{
			NotifyOfAssetChannelCountChangeFunc(AssetID);
		}
	}


	TUniqueFunction<TArray<int32>()> GetCurrentChannelVisibilityFunc;
	TUniqueFunction<void(const TArray<int32>&, bool, bool)> RequestChannelVisibilityChangeFunc;
	TUniqueFunction<void(int32 AssetID)> NotifyOfAssetChannelCountChangeFunc;

};

/** Stores UV mesh AABB trees */
UCLASS()
class UVEDITORTOOLS_API UUVToolAABBTreeStorage : public UUVToolContextObject
{
	GENERATED_BODY()
public:

	void Set(UE::Geometry::FDynamicMesh3* MeshKey, TSharedPtr<UE::Geometry::FDynamicMeshAABBTree3> Tree);

	TSharedPtr<UE::Geometry::FDynamicMeshAABBTree3> Get(UE::Geometry::FDynamicMesh3* MeshKey);

	void Remove(UE::Geometry::FDynamicMesh3* MeshKey);

	void RemoveByPredicate(TUniqueFunction<
		bool(const TPair<UE::Geometry::FDynamicMesh3*, TSharedPtr<UE::Geometry::FDynamicMeshAABBTree3>>&)> Predicate);

	void Empty();

protected:

	TMap<UE::Geometry::FDynamicMesh3*, TSharedPtr<UE::Geometry::FDynamicMeshAABBTree3>> AABBTrees;
};
