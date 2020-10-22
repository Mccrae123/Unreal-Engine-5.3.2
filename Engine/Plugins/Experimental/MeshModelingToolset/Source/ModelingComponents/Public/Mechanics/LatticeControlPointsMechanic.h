// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "BaseBehaviors/BehaviorTargetInterfaces.h"
#include "FrameTypes.h"
#include "RectangleMarqueeMechanic.h"
#include "InteractiveToolChange.h"
#include "Snapping/PointPlanarSnapSolver.h"
#include "Spatial/GeometrySet3.h"
#include "ToolContextInterfaces.h" //FViewCameraState
#include "VectorTypes.h"
#include "VectorUtil.h"
#include "ToolDataVisualizer.h"
#include "SceneView.h"

#include "LatticeControlPointsMechanic.generated.h"

class APreviewGeometryActor;
class ULineSetComponent;
class UMouseHoverBehavior;
class UPointSetComponent;
class USingleClickInputBehavior;
class UTransformGizmo;
class UTransformProxy;

UCLASS()
class MODELINGCOMPONENTS_API ULatticeControlPointsMechanic : 
	public URectangleMarqueeMechanic, public IClickBehaviorTarget, public IHoverBehaviorTarget
{
	GENERATED_BODY()

public:

	// TODO: Snapping

	// This delegate is called every time the control points are altered.
	DECLARE_MULTICAST_DELEGATE(OnPointsChangedEvent);
	OnPointsChangedEvent OnPointsChanged;

	virtual void Initialize(const TArray<FVector3d>& Points, 
							const TArray<FVector2i>& Edges,
							const FTransform3d& LocalToWorldTransform );

	void SetWorld(UWorld* World);
	const TArray<FVector3d>& GetControlPoints() const;

	void SetCoordinateSystem(EToolContextCoordinateSystem InCoordinateSystem);
	EToolContextCoordinateSystem GetCoordinateSystem() const;

	void UpdateSetPivotMode(bool bInSetPivotMode);

	// UInteractionMechanic
	virtual void Setup(UInteractiveTool* ParentTool) override;
	virtual void Shutdown() override;
	virtual void Render(IToolsContextRenderAPI* RenderAPI) override;

	// IClickBehaviorTarget implementation
	virtual FInputRayHit IsHitByClick(const FInputDeviceRay& ClickPos) override;
	virtual void OnClicked(const FInputDeviceRay& ClickPos) override;

	// IHoverBehaviorTarget implementation
	virtual FInputRayHit BeginHoverSequenceHitTest(const FInputDeviceRay& PressPos) override;
	virtual void OnBeginHover(const FInputDeviceRay& DevicePos) override;
	virtual bool OnUpdateHover(const FInputDeviceRay& DevicePos) override;
	virtual void OnEndHover() override;
	virtual void OnUpdateModifierState(int ModifierID, bool bIsOn) override;

	// URectangleMarqueeMechanic implementation
	void OnDragRectangleStarted() final;
	void OnDragRectangleChanged(const FCameraRectangle& Rectangle) final;
	void OnDragRectangleFinished() final;

	bool bHasChanged = false;

protected:

	TArray<FVector3d> ControlPoints;
	TArray<FVector2i> LatticeEdges;
	FTransform3d LocalToWorldTransform;

	// Used for spatial queries
	FGeometrySet3 GeometrySet;

	/** Used for displaying points/segments */
	UPROPERTY()
	APreviewGeometryActor* PreviewGeometryActor;
	UPROPERTY()
	UPointSetComponent* DrawnControlPoints;
	UPROPERTY()
	ULineSetComponent* DrawnLatticeEdges;

	// Variables for drawing
	FColor NormalSegmentColor;
	FColor NormalPointColor;
	float SegmentsThickness;
	float PointsSize;
	FColor HoverColor;
	FColor SelectedColor;

	// Cache previous color while temporarily changing the color of a hovered-over point
	FColor PreHoverPointColor;

	// Support for Shift and Ctrl toggle
	bool bAddToSelectionToggle = false;
	const int32 AddToSelectionModifierId = 1;
	

	// Support for gizmo. Since the points aren't individual components, we don't actually use UTransformProxy
	// for the transform forwarding- we just use it for the callbacks.
	UPROPERTY()
	UTransformProxy* PointTransformProxy;
	UPROPERTY()
	UTransformGizmo* PointTransformGizmo;

	// Used to make it easy to tell whether the gizmo was moved by the user or by undo/redo or
	// some other change that we shouldn't respond to. Basing our movement undo/redo on the
	// gizmo turns out to be quite a pain, though may someday be easier if the transform proxy
	// is able to manage arbitrary objects.
	bool bGizmoBeingDragged = false;

	// Callbacks we'll receive from the gizmo proxy
	void GizmoTransformChanged(UTransformProxy* Proxy, FTransform Transform);
	void GizmoTransformStarted(UTransformProxy* Proxy);
	void GizmoTransformEnded(UTransformProxy* Proxy);

	// Support for hovering
	TFunction<bool(const FVector3d&, const FVector3d&)> GeometrySetToleranceTest;
	int32 HoveredPointID = -1;
	void ClearHover();

	// Support for selection
	TArray<int32> SelectedPointIDs;
	TArray<int32> CurrentDragSelection;

	// We need the selected point start positions so we can move multiple points appropriately.
	TArray<FVector3d> SelectedPointStartPositions;

	// The starting point of the gizmo is needed to determine the offset by which to move the points.
	// TODO: Replace with single FTransform?
	FVector GizmoStartPosition;
	FQuat	GizmoStartRotation;
	FVector GizmoStartScale;

	// These issue undo/redo change objects, and must therefore not be called in undo/redo code.
	void ChangeSelection(int32 NewPointID, bool AddToSelection);
	void ClearSelection();

	// All of the following do not issue undo/redo change objects.
	bool HitTest(const FInputDeviceRay& ClickPos, FInputRayHit& ResultOut);
	void SelectPoint(int32 PointID);
	bool DeselectPoint(int32 PointID);
	void UpdateGizmoLocation();
	void UpdatePointLocations(const TArray<int32>& PointIDs, const TArray<FVector3d>& NewLocations);

	void RebuildDrawables();
	void UpdateDrawables();

	// Used for expiring undo/redo changes, which compare this to their stored value and expire themselves if they do not match.
	int32 CurrentChangeStamp = 0;

	friend class FLatticeControlPointsMechanicSelectionChange;
	friend class FLatticeControlPointsMechanicMovementChange;
};


// Undo/redo support:

// Control point selection has changed
class MODELINGCOMPONENTS_API FLatticeControlPointsMechanicSelectionChange : public FToolCommandChange
{
public:
	FLatticeControlPointsMechanicSelectionChange(int32 PointIDIn, bool bAddedIn, const FTransform& PreviousTransformIn,
												 const FTransform& NewTransformIn, int32 ChangeStampIn);

	FLatticeControlPointsMechanicSelectionChange(const TArray<int32>& PointIDsIn, bool bAddedIn, const FTransform& PreviousTransformIn,
												 const FTransform& NewTransformIn, int32 ChangeStampIn);

	virtual void Apply(UObject* Object) override;
	virtual void Revert(UObject* Object) override;
	virtual bool HasExpired(UObject* Object) const override
	{
		return Cast<ULatticeControlPointsMechanic>(Object)->CurrentChangeStamp != ChangeStamp;
	}
	virtual FString ToString() const override;

protected:
	TArray<int32> PointIDs;
	bool bAdded;

	const FTransform PreviousTransform;
	const FTransform NewTransform;

	int32 ChangeStamp;
};


// Control points have moved
class MODELINGCOMPONENTS_API FLatticeControlPointsMechanicMovementChange : public FToolCommandChange
{
public:

	FLatticeControlPointsMechanicMovementChange(const TArray<int32>& PointIDsIn, 
		const TArray<FVector3d>& OriginalPositionIn,
		const TArray<FVector3d>& NewPositionIn, 
		int32 ChangeStampIn,
		bool bFirstMovementIn);

	virtual void Apply(UObject* Object) override;
	virtual void Revert(UObject* Object) override;
	virtual bool HasExpired(UObject* Object) const override
	{
		return Cast<ULatticeControlPointsMechanic>(Object)->CurrentChangeStamp != ChangeStamp;
	}
	virtual FString ToString() const override;

protected:

	TArray<int32> PointIDs;
	TArray<FVector3d> OriginalPositions;
	TArray<FVector3d> NewPositions;
	int32 ChangeStamp;
	bool bFirstMovement;
};
