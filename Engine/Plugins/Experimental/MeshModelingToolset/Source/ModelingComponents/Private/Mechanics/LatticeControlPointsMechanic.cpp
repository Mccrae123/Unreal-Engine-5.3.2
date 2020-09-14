// Copyright Epic Games, Inc. All Rights Reserved.

#include "Mechanics/LatticeControlPointsMechanic.h"

#include "BaseBehaviors/SingleClickBehavior.h"
#include "BaseBehaviors/MouseHoverBehavior.h"
#include "BaseBehaviors/ClickDragBehavior.h"
#include "BaseGizmos/TransformGizmo.h"
#include "BaseGizmos/TransformProxy.h"
#include "Drawing/PreviewGeometryActor.h"
#include "Drawing/LineSetComponent.h"
#include "Drawing/PointSetComponent.h"
#include "InteractiveToolManager.h"
#include "InteractiveToolActionSet.h"
#include "Polyline3.h"
#include "ToolSceneQueriesUtil.h"
#include "ToolSetupUtil.h"
#include "Transforms/MultiTransformer.h"

#define LOCTEXT_NAMESPACE "ULatticeControlPointsMechanic"

static const FText LatticePointDeselectionTransactionText = LOCTEXT("LatticePointDeselection", "Lattice Point Deselection");
static const FText LatticePointSelectionTransactionText = LOCTEXT("LatticePointSelection", "Lattice Point Selection");
static const FText LatticePointMovementTransactionText = LOCTEXT("LatticePointMovement", "Lattice Point Movement");

void ULatticeControlPointsMechanic::Setup(UInteractiveTool* ParentToolIn)
{
	UInteractionMechanic::Setup(ParentToolIn);

	USingleClickInputBehavior* ClickBehavior = NewObject<USingleClickInputBehavior>();
	ClickBehavior->Initialize(this);
	ClickBehavior->Modifiers.RegisterModifier(CtrlModifierId, FInputDeviceState::IsCtrlKeyDown);
	ClickBehavior->Modifiers.RegisterModifier(ShiftModifierId, FInputDeviceState::IsShiftKeyDown);
	ParentTool->AddInputBehavior(ClickBehavior);

	UMouseHoverBehavior* HoverBehavior = NewObject<UMouseHoverBehavior>();
	HoverBehavior->Initialize(this);
	HoverBehavior->Modifiers.RegisterModifier(CtrlModifierId, FInputDeviceState::IsCtrlKeyDown);
	HoverBehavior->Modifiers.RegisterModifier(ShiftModifierId, FInputDeviceState::IsShiftKeyDown);
	ParentTool->AddInputBehavior(HoverBehavior);

	UClickDragInputBehavior* ClickDragBehavior = NewObject<UClickDragInputBehavior>(this);
	ClickDragBehavior->Initialize(this);
	// TODO: Add CTRL support for drag selection?
	ParentTool->AddInputBehavior(ClickDragBehavior);

	DrawnControlPoints = NewObject<UPointSetComponent>();
	DrawnControlPoints->SetPointMaterial(
		LoadObject<UMaterial>(nullptr, TEXT("/MeshModelingToolset/Materials/PointSetComponentMaterial")));
	DrawnLatticeEdges = NewObject<ULineSetComponent>();
	DrawnLatticeEdges->SetLineMaterial(
		LoadObject<UMaterial>(nullptr, TEXT("/MeshModelingToolset/Materials/LineSetComponentMaterial")));

	NormalPointColor = FColor::Red;
	NormalSegmentColor = FColor::Red;
	HoverColor = FColor::Green;
	SelectedColor = FColor::Yellow;
	SegmentsThickness = 1.0f;
	PointsSize = 6.0f;

	GeometrySetToleranceTest = [this](const FVector3d& Position1, const FVector3d& Position2) {
		if (CachedCameraState.bIsOrthographic)
		{
			// We could just always use ToolSceneQueriesUtil::PointSnapQuery. But in ortho viewports, we happen to know
			// that the only points that we will ever give this function will be the closest points between a ray and
			// some geometry, meaning that the vector between them will be orthogonal to the view ray. With this knowledge,
			// we can do the tolerance computation more efficiently than PointSnapQuery can, since we don't need to project
			// down to the view plane.
			// As in PointSnapQuery, we convert our angle-based tolerance to one we can use in an ortho viewport (instead of
			// dividing our field of view into 90 visual angle degrees, we divide the plane into 90 units).
			float OrthoTolerance = ToolSceneQueriesUtil::GetDefaultVisualAngleSnapThreshD() * CachedCameraState.OrthoWorldCoordinateWidth / 90.0;
			return Position1.DistanceSquared(Position2) < OrthoTolerance * OrthoTolerance;
		}
		else
		{
			return ToolSceneQueriesUtil::PointSnapQuery(CachedCameraState, Position1, Position2);
		}
	};

	UInteractiveGizmoManager* GizmoManager = GetParentTool()->GetToolManager()->GetPairedGizmoManager();
	PointTransformProxy = NewObject<UTransformProxy>(this);
	
	// TODO: Maybe don't have the gizmo's axes flip around when it crosses the origin, if possible?
	// TODO: Enable local vs world gizmo switching (UETOOL-2356)
	PointTransformGizmo = GizmoManager->CreateCustomTransformGizmo(ETransformGizmoSubElements::FullTranslateRotateScale,
																   GetParentTool());

	PointTransformProxy->OnTransformChanged.AddUObject(this, &ULatticeControlPointsMechanic::GizmoTransformChanged);
	PointTransformProxy->OnBeginTransformEdit.AddUObject(this, &ULatticeControlPointsMechanic::GizmoTransformStarted);
	PointTransformProxy->OnEndTransformEdit.AddUObject(this, &ULatticeControlPointsMechanic::GizmoTransformEnded);
	PointTransformGizmo->SetActiveTarget(PointTransformProxy);
	PointTransformGizmo->SetVisibility(false);
	PointTransformGizmo->bUseContextCoordinateSystem = false;
}

void ULatticeControlPointsMechanic::Initialize(const TArray<FVector3d>& Points, const TArray<FVector2i>& Edges)
{
	ControlPoints = Points;
	SelectedPointIDs.Empty();
	LatticeEdges = Edges;
	UpdateGizmoLocation();
	RebuildDrawables();
	++CurrentChangeStamp;		// If the lattice is potentially changing resolution, make this an undo barrier
	bHasChanged = false;
}

void ULatticeControlPointsMechanic::SetWorld(UWorld* World)
{
	// It may be unreasonable to worry about SetWorld being called more than once, but let's be safe anyway
	if (PreviewGeometryActor)
	{
		PreviewGeometryActor->Destroy();
	} 

	// We need the world so we can create the geometry actor in the right place
	FRotator Rotation(0.0f, 0.0f, 0.0f);
	FActorSpawnParameters SpawnInfo;
	PreviewGeometryActor = World->SpawnActor<APreviewGeometryActor>(FVector::ZeroVector, Rotation, SpawnInfo);

	// Attach the rendering components to the actor
	DrawnControlPoints->Rename(nullptr, PreviewGeometryActor);
	PreviewGeometryActor->SetRootComponent(DrawnControlPoints);
	if (DrawnControlPoints->IsRegistered())
	{
		DrawnControlPoints->ReregisterComponent();
	}
	else
	{
		DrawnControlPoints->RegisterComponent();
	}

	DrawnLatticeEdges->Rename(nullptr, PreviewGeometryActor); 
	DrawnLatticeEdges->AttachToComponent(DrawnControlPoints, FAttachmentTransformRules::KeepWorldTransform);
	if (DrawnLatticeEdges->IsRegistered())
	{
		DrawnLatticeEdges->ReregisterComponent();
	}
	else
	{
		DrawnLatticeEdges->RegisterComponent();
	}
}

void ULatticeControlPointsMechanic::Shutdown()
{
	if (PreviewGeometryActor)
	{
		PreviewGeometryActor->Destroy();
		PreviewGeometryActor = nullptr;
	}

	if (PointTransformGizmo)
	{
		PointTransformGizmo->Shutdown();
		PointTransformGizmo = nullptr;
	}

	UInteractiveGizmoManager* GizmoManager = GetParentTool()->GetToolManager()->GetPairedGizmoManager();
	GizmoManager->DestroyAllGizmosByOwner(GetParentTool());
}


void ULatticeControlPointsMechanic::Render(IToolsContextRenderAPI* RenderAPI)
{
	// Cache the camera state
	GetParentTool()->GetToolManager()->GetContextQueriesAPI()->GetCurrentViewState(CachedCameraState);
}

void ULatticeControlPointsMechanic::RebuildDrawables()
{
	DrawnControlPoints->Clear();
	GeometrySet.Reset();
	for (int32 PointIndex = 0; PointIndex < ControlPoints.Num(); ++PointIndex)
	{
		const FVector3d& P = ControlPoints[PointIndex];
		DrawnControlPoints->InsertPoint(PointIndex, FRenderablePoint(static_cast<FVector>(P), NormalPointColor, PointsSize));
		GeometrySet.AddPoint(PointIndex, P);
	}

	for (int32 PointID : SelectedPointIDs)
	{
		if (DrawnControlPoints->IsPointValid(PointID))
		{
			DrawnControlPoints->SetPointColor(PointID, SelectedColor);
		}
	}

	DrawnLatticeEdges->Clear();
	for (int32 EdgeIndex = 0; EdgeIndex < LatticeEdges.Num(); ++EdgeIndex)
	{
		const FVector3d& A = ControlPoints[LatticeEdges[EdgeIndex].X];
		const FVector3d& B = ControlPoints[LatticeEdges[EdgeIndex].Y];
		int32 SegmentID = DrawnLatticeEdges->AddLine(FVector(A), FVector(B), NormalSegmentColor, SegmentsThickness);
		check(SegmentID == EdgeIndex);
	}
}


void ULatticeControlPointsMechanic::UpdateDrawables()
{
	for (int32 PointIndex = 0; PointIndex < ControlPoints.Num(); ++PointIndex)
	{
		const FVector3d& P = ControlPoints[PointIndex];
		DrawnControlPoints->SetPointPosition(PointIndex, static_cast<FVector>(P));
		DrawnControlPoints->SetPointColor(PointIndex, NormalPointColor);

		GeometrySet.UpdatePoint(PointIndex, P);
	}

	for (int32 PointID : SelectedPointIDs)
	{
		if (DrawnControlPoints->IsPointValid(PointID))
		{
			DrawnControlPoints->SetPointColor(PointID, SelectedColor);
		}
	}

	for (int32 EdgeIndex = 0; EdgeIndex < LatticeEdges.Num(); ++EdgeIndex)
	{
		const FVector3d& A = ControlPoints[LatticeEdges[EdgeIndex].X];
		const FVector3d& B = ControlPoints[LatticeEdges[EdgeIndex].Y];
		DrawnLatticeEdges->SetLineStart(EdgeIndex, FVector(A));
		DrawnLatticeEdges->SetLineEnd(EdgeIndex, FVector(B));
	}
}


void ULatticeControlPointsMechanic::UpdateDrawablesForPoint(int32 PointIndex)
{
	const FVector3d& P = ControlPoints[PointIndex];
	GeometrySet.UpdatePoint(PointIndex, P);

	DrawnControlPoints->SetPointPosition(PointIndex, static_cast<FVector>(P));
	DrawnControlPoints->SetPointColor(PointIndex, NormalPointColor);

	if (DrawnControlPoints->IsPointValid(PointIndex))
	{
		DrawnControlPoints->SetPointColor(PointIndex, SelectedColor);
	}

	// TODO: Accelerate this somehow. Don't want to search over the entire set of edges any time one point changes.
	for (int32 EdgeIndex = 0; EdgeIndex < LatticeEdges.Num(); ++EdgeIndex)		
	{
		if (LatticeEdges[EdgeIndex].X == PointIndex)
		{
			DrawnLatticeEdges->SetLineStart(EdgeIndex, static_cast<FVector>(P));
		}
		else if (LatticeEdges[EdgeIndex].Y == PointIndex)
		{
			DrawnLatticeEdges->SetLineEnd(EdgeIndex, static_cast<FVector>(P));
		}
	}
}


void ULatticeControlPointsMechanic::GizmoTransformStarted(UTransformProxy* Proxy)
{
	ParentTool->GetToolManager()->BeginUndoTransaction(LatticePointMovementTransactionText);

	GizmoStartPosition = Proxy->GetTransform().GetTranslation();
	GizmoStartRotation = Proxy->GetTransform().GetRotation();
	GizmoStartScale = Proxy->GetTransform().GetScale3D();

	SelectedPointStartPositions.SetNum(SelectedPointIDs.Num());
	for (int32 i = 0; i < SelectedPointIDs.Num(); ++i)
	{
		SelectedPointStartPositions[i] = ControlPoints[SelectedPointIDs[i]];
	}

	bGizmoBeingDragged = true;
}

void ULatticeControlPointsMechanic::GizmoTransformChanged(UTransformProxy* Proxy, FTransform Transform)
{
	if (SelectedPointIDs.Num() == 0 || !bGizmoBeingDragged)
	{
		return;
	}

	bool bPointsChanged = false;

	FVector Displacement = Transform.GetTranslation() - GizmoStartPosition;
	FQuaterniond DeltaRotation = FQuaterniond(Transform.GetRotation() * GizmoStartRotation.Inverse());
	FVector DeltaScale = Transform.GetScale3D() / GizmoStartScale;

	FTransform3d DeltaTransform;
	DeltaTransform.SetScale(DeltaScale);
	DeltaTransform.SetRotation(DeltaRotation);
	DeltaTransform.SetTranslation(Transform.GetTranslation());

	// If any deltas are non-zero
	if (Displacement != FVector::ZeroVector || !DeltaRotation.EpsilonEqual(FQuaterniond::Identity(), SMALL_NUMBER) || DeltaScale != FVector::OneVector)
	{
		for (int32 i = 0; i < SelectedPointIDs.Num(); ++i)
		{
			int PointID = SelectedPointIDs[i];
			FVector3d PointPosition = SelectedPointStartPositions[i];

			// Translate to origin, scale, rotate, and translate back (DeltaTransform has "translate back" baked in.)
			PointPosition -= GizmoStartPosition;
			PointPosition = DeltaTransform.TransformPosition(PointPosition);

			ControlPoints[PointID] = PointPosition;
		}
		bPointsChanged = true;
		UpdateDrawables();
	}

	if (bPointsChanged)
	{
		OnPointsChanged.Broadcast();
	}
}

void ULatticeControlPointsMechanic::GizmoTransformEnded(UTransformProxy* Proxy)
{
	TArray<FVector3d> SelectedPointNewPositions;
	for (int32 i = 0; i < SelectedPointIDs.Num(); ++i)
	{
		SelectedPointNewPositions.Add(ControlPoints[SelectedPointIDs[i]]);
	}

	bool bFirstMovement = !bHasChanged;
	bHasChanged = true;

	ParentTool->GetToolManager()->EmitObjectChange(this, MakeUnique<FLatticeControlPointsMechanicMovementChange>(
		SelectedPointIDs, SelectedPointStartPositions, SelectedPointNewPositions,
		CurrentChangeStamp, bFirstMovement), LatticePointMovementTransactionText);

	SelectedPointStartPositions.Reset();

	// TODO: When we implement snapping
	// We may need to reset the gizmo if our snapping caused the final point position to differ from the gizmo position
	// UpdateGizmoLocation();

	ParentTool->GetToolManager()->EndUndoTransaction();		// was started in GizmoTransformStarted above

	// This just lets the tool know that the gizmo has finished moving and we've added it to the undo stack.
	// TODO: Add a different callback? "OnGizmoTransformChanged"?
	OnPointsChanged.Broadcast();

	bGizmoBeingDragged = false;
}

void ULatticeControlPointsMechanic::UpdatePointLocations(const TArray<int32>& PointIDs, const TArray<FVector3d>& NewLocations)
{
	check(NewLocations.Num() == PointIDs.Num());
	for (int i = 0; i < NewLocations.Num(); ++i)
	{
		ControlPoints[PointIDs[i]] = NewLocations[i];
	}
	UpdateDrawables();
}

bool ULatticeControlPointsMechanic::HitTest(const FInputDeviceRay& ClickPos, FInputRayHit& ResultOut)
{
	FGeometrySet3::FNearest Nearest;

	// See if we hit a point for selection
	if (GeometrySet.FindNearestPointToRay(ClickPos.WorldRay, Nearest, GeometrySetToleranceTest))
	{
		ResultOut = FInputRayHit(Nearest.RayParam);
		return true;
	}
	return false;
}

FInputRayHit ULatticeControlPointsMechanic::IsHitByClick(const FInputDeviceRay& ClickPos)
{
	FInputRayHit Result;
	HitTest(ClickPos, Result);
	return Result;
}

void ULatticeControlPointsMechanic::OnClicked(const FInputDeviceRay& ClickPos)
{
	FGeometrySet3::FNearest Nearest;

	if (GeometrySet.FindNearestPointToRay(ClickPos.WorldRay, Nearest, GeometrySetToleranceTest))
	{
		ParentTool->GetToolManager()->BeginUndoTransaction(LatticePointSelectionTransactionText);
		ChangeSelection(Nearest.ID, bAddToSelectionToggle);
		ParentTool->GetToolManager()->EndUndoTransaction();
	}

	bIsDragging = false;
}

void ULatticeControlPointsMechanic::ChangeSelection(int32 NewPointID, bool bAddToSelection)
{
	// If not adding to selection, clear it
	if (!bAddToSelection && SelectedPointIDs.Num() > 0)
	{
		TArray<int32> PointsToDeselect;

		for (int32 PointID : SelectedPointIDs)
		{
			// We check for validity here because we'd like to be able to use this function to deselect points after
			// deleting them.
			if (DrawnControlPoints->IsPointValid(PointID))
			{
				PointsToDeselect.Add(PointID);
				DrawnControlPoints->SetPointColor(PointID, NormalPointColor);
			}
		}

		ParentTool->GetToolManager()->EmitObjectChange(this, MakeUnique<FLatticeControlPointsMechanicSelectionChange>(
			PointsToDeselect, false, CurrentChangeStamp), LatticePointDeselectionTransactionText);

		SelectedPointIDs.Empty();
	}

	// We check for validity here because giving an invalid id (such as -1) with bAddToSelection == false
	// is an easy way to clear the selection.
	if ((NewPointID >= 0)  && (NewPointID < ControlPoints.Num()))
	{
		if (bAddToSelection && DeselectPoint(NewPointID))
		{
			ParentTool->GetToolManager()->EmitObjectChange(this, MakeUnique<FLatticeControlPointsMechanicSelectionChange>(
				NewPointID, false, CurrentChangeStamp), LatticePointDeselectionTransactionText);
		}
		else
		{
			SelectPoint(NewPointID);
			ParentTool->GetToolManager()->EmitObjectChange(this, MakeUnique<FLatticeControlPointsMechanicSelectionChange>(
				NewPointID, true, CurrentChangeStamp), LatticePointSelectionTransactionText);
		}
	}

	UpdateGizmoLocation();
}

void ULatticeControlPointsMechanic::UpdateGizmoLocation()
{
	if (!PointTransformGizmo)
	{
		return;
	}

	if (SelectedPointIDs.Num() == 0)
	{
		PointTransformGizmo->SetVisibility(false);
		PointTransformGizmo->ReinitializeGizmoTransform(FTransform());
	}
	else
	{
		FVector3d NewGizmoLocation(0,0,0);
		for (int32 PointID : SelectedPointIDs)
		{
			NewGizmoLocation += ControlPoints[PointID];
		}
		NewGizmoLocation /= SelectedPointIDs.Num();

		// Don't clear the gizmo rotation
		FQuat OldGizmoRotation = PointTransformProxy->GetTransform().GetRotation();

		PointTransformGizmo->ReinitializeGizmoTransform(FTransform(OldGizmoRotation, (FVector)NewGizmoLocation));

		// Clear the child scale
		FVector GizmoScale{ 1.0, 1.0, 1.0 };
		PointTransformGizmo->SetNewChildScale(GizmoScale);

		PointTransformGizmo->SetVisibility(true);
	}
}

bool ULatticeControlPointsMechanic::DeselectPoint(int32 PointID)
{
	bool PointFound = false;
	int32 IndexInSelection;
	// TODO: This might be slow if we have a lot of selected points (UETOOL-2357)
	if (SelectedPointIDs.Find(PointID, IndexInSelection))
	{
		SelectedPointIDs.RemoveAt(IndexInSelection);
		DrawnControlPoints->SetPointColor(PointID, NormalPointColor);
		PointFound = true;
	}
	
	return PointFound;
}

void ULatticeControlPointsMechanic::SelectPoint(int32 PointID)
{
	SelectedPointIDs.Add(PointID);
	DrawnControlPoints->SetPointColor(PointID, SelectedColor);
}

void ULatticeControlPointsMechanic::ClearSelection()
{
	ChangeSelection(-1, false);
}

FInputRayHit ULatticeControlPointsMechanic::BeginHoverSequenceHitTest(const FInputDeviceRay& PressPos)
{
	FInputRayHit Result;
	HitTest(PressPos, Result);
	return Result;
}

void ULatticeControlPointsMechanic::OnBeginHover(const FInputDeviceRay& DevicePos)
{
	OnUpdateHover(DevicePos);
}

void ULatticeControlPointsMechanic::ClearHover()
{
	if (HoveredPointID >= 0)
	{
		DrawnControlPoints->SetPointColor(HoveredPointID, PreHoverPointColor);
		HoveredPointID = -1;
	}
}


bool ULatticeControlPointsMechanic::OnUpdateHover(const FInputDeviceRay& DevicePos)
{
	FGeometrySet3::FNearest Nearest;

	// see if we're hovering a point for selection
	if (GeometrySet.FindNearestPointToRay(DevicePos.WorldRay, Nearest, GeometrySetToleranceTest))
	{
		// Only need to update the hover if we changed the point
		if (Nearest.ID != HoveredPointID)
		{
			ClearHover();
			HoveredPointID = Nearest.ID;
			PreHoverPointColor = DrawnControlPoints->GetPoint(HoveredPointID).Color;
			DrawnControlPoints->SetPointColor(HoveredPointID, HoverColor);
		}
	}
	else
	{
		// Not hovering anything, so done hovering
		return false;
	}

	return true;
}

void ULatticeControlPointsMechanic::OnEndHover()
{
	ClearHover();
}

// Detects Ctrl key state
void ULatticeControlPointsMechanic::OnUpdateModifierState(int ModifierID, bool bIsOn)
{
	if (ModifierID == CtrlModifierId || ModifierID == ShiftModifierId)
	{
		bAddToSelectionToggle = bIsOn;
	}
}


// ==================== IClickDragBehaviorTarget ====================

FInputRayHit ULatticeControlPointsMechanic::CanBeginClickDragSequence(const FInputDeviceRay& PressPos)
{
	FInputRayHit Dummy;

	// This is the boolean that is checked to see if a drag sequence can be started. In our case we want to begin the 
	// drag sequence even if the first ray doesn't hit anything, so set this to true.
	Dummy.bHit = true;

	return Dummy;
}

void ULatticeControlPointsMechanic::OnClickPress(const FInputDeviceRay& PressPos)
{
	if (!PressPos.bHas2D)
	{
		return;
	}

	for (int32& PointID : SelectedPointIDs)
	{
		DrawnControlPoints->SetPointColor(PointID, NormalPointColor);
	}
	CurrentDragSelection.Empty();

	UpdateGizmoLocation();

	DragStartScreenPosition = PressPos.ScreenPosition;
	DragStartWorldRay = PressPos.WorldRay;

	// Hide gizmo while dragging
	if (PointTransformGizmo)
	{
		PointTransformGizmo->SetVisibility(false);
		PointTransformGizmo->ReinitializeGizmoTransform(FTransform());
	}
}


static FVector2D PlaneCoordinates(const FVector& Point, const FPlane& Plane, const FVector& UBasisVector, const FVector& VBasisVector)
{
	float U = FVector::DotProduct(Point - Plane.GetOrigin(), UBasisVector);
	float V = FVector::DotProduct(Point - Plane.GetOrigin(), VBasisVector);
	return FVector2D{ U,V };
}

void ULatticeControlPointsMechanic::OnClickDrag(const FInputDeviceRay& DragPos)
{
	if (!DragPos.bHas2D)
	{
		return;
	}

	bIsDragging = true;
	DragCurrentScreenPosition = DragPos.ScreenPosition;
	DragCurrentWorldRay = DragPos.WorldRay;

	// Intersect the drag rays and and project lattice points all to the same plane in 3D. Then compute 2D coordinates 
	// and use an AABB test to determine which points are in the drag rectangle.

	FViewCameraState CameraState;
	GetParentTool()->GetToolManager()->GetContextQueriesAPI()->GetCurrentViewState(CameraState);

	// Create plane in front of camera
	FPlane CameraPlane(CameraState.Position + CameraState.Forward(), CameraState.Forward());

	FVector UBasisVector = CameraState.Right();
	FVector VBasisVector = CameraState.Up();

	FVector StartIntersection = FMath::RayPlaneIntersection(DragStartWorldRay.Origin, 
															DragStartWorldRay.Direction, 
															CameraPlane);
	FVector2D Start2D = PlaneCoordinates(StartIntersection, CameraPlane, UBasisVector, VBasisVector);

	FVector CurrentIntersection = FMath::RayPlaneIntersection(DragCurrentWorldRay.Origin, 
															  DragCurrentWorldRay.Direction, 
															  CameraPlane);
	FVector2D Current2D = PlaneCoordinates(CurrentIntersection, CameraPlane, UBasisVector, VBasisVector);

	FBox2D DragBox(Start2D, Start2D);
	DragBox += Current2D;	// Initialize this way so we don't have to care about min/max

	for (int32 PointID = 0; PointID < ControlPoints.Num(); ++PointID)
	{
		FVector PointPosition = DrawnControlPoints->GetPoint(PointID).Position;
		FVector PointIntersection;
		if (CameraState.bIsOrthographic)
		{
			// project directly to plane
			PointIntersection = FVector::PointPlaneProject(PointPosition, CameraPlane);
		}
		else
		{
			// intersect along the eye-to-point ray
			PointIntersection = FMath::RayPlaneIntersection(CameraState.Position, 
															PointPosition - CameraState.Position,
															CameraPlane);
		}

		FVector2D Point2D = PlaneCoordinates(PointIntersection, CameraPlane, UBasisVector, VBasisVector);
		if (DragBox.IsInside(Point2D))
		{
			CurrentDragSelection.Add(PointID);
			DrawnControlPoints->SetPointColor(PointID, SelectedColor);
		}
		else
		{
			DrawnControlPoints->SetPointColor(PointID, NormalPointColor);
		}
	}
}

void ULatticeControlPointsMechanic::OnClickRelease(const FInputDeviceRay& ReleasePos)
{
	// Deselect previous SelectedPointIDs and replace it with "drag selection" points. Do this in one Undo transaction.

	ParentTool->GetToolManager()->BeginUndoTransaction(LatticePointSelectionTransactionText);

	if (SelectedPointIDs.Num() > 0)
	{
		ParentTool->GetToolManager()->EmitObjectChange(this, MakeUnique<FLatticeControlPointsMechanicSelectionChange>(
			SelectedPointIDs, false, CurrentChangeStamp), LatticePointDeselectionTransactionText);
	}

	if (CurrentDragSelection.Num() > 0)
	{
		ParentTool->GetToolManager()->EmitObjectChange(this, MakeUnique<FLatticeControlPointsMechanicSelectionChange>(
			CurrentDragSelection, true, CurrentChangeStamp), LatticePointSelectionTransactionText);
	}

	ParentTool->GetToolManager()->EndUndoTransaction();

	SelectedPointIDs = CurrentDragSelection;
	CurrentDragSelection.Empty();

	bIsDragging = false;
	UpdateGizmoLocation();
}


void ULatticeControlPointsMechanic::OnTerminateDragSequence()
{
	// Not sure how this can happen. Pressing escape quits the tool altogether.
	CurrentDragSelection.Empty();
	bIsDragging = false;
	UpdateGizmoLocation();
}

const TArray<FVector3d>& ULatticeControlPointsMechanic::GetControlPoints() const
{
	return ControlPoints;
}

// ==================== Undo/redo object functions ====================

FLatticeControlPointsMechanicSelectionChange::FLatticeControlPointsMechanicSelectionChange(int32 PointIDIn,
	bool AddedIn, int32 ChangeStampIn)
	: PointIDs{ PointIDIn }
	, Added(AddedIn)
	, ChangeStamp(ChangeStampIn)
{}

FLatticeControlPointsMechanicSelectionChange::FLatticeControlPointsMechanicSelectionChange(const TArray<int32>& PointIDsIn, 
	bool AddedIn, int32 ChangeStampIn)
	: PointIDs(PointIDsIn)
	, Added(AddedIn)
	, ChangeStamp(ChangeStampIn)
{}

void FLatticeControlPointsMechanicSelectionChange::Apply(UObject* Object)
{
	ULatticeControlPointsMechanic* Mechanic = Cast<ULatticeControlPointsMechanic>(Object);
	
	for (int32 PointID : PointIDs)
	{
		if (Added)
		{
			Mechanic->SelectPoint(PointID);
		}
		else
		{
			Mechanic->DeselectPoint(PointID);
		}
	}

	Mechanic->UpdateGizmoLocation();
}

void FLatticeControlPointsMechanicSelectionChange::Revert(UObject* Object)
{
	ULatticeControlPointsMechanic* Mechanic = Cast<ULatticeControlPointsMechanic>(Object);
	for (int32 PointID : PointIDs)
	{
		if (Added)
		{
			Mechanic->DeselectPoint(PointID);
		}
		else
		{
			Mechanic->SelectPoint(PointID);
		}
	}
	Mechanic->UpdateGizmoLocation();
}

FString FLatticeControlPointsMechanicSelectionChange::ToString() const
{
	return TEXT("FLatticeControlPointsMechanicSelectionChange");
}


FLatticeControlPointsMechanicMovementChange::FLatticeControlPointsMechanicMovementChange(const TArray<int32>& PointIDsIn,
	const TArray<FVector3d>& OriginalPositionsIn,
	const TArray<FVector3d>& NewPositionsIn,
	int32 ChangeStampIn,
	bool bFirstMovementIn)
	: PointIDs{ PointIDsIn }
	, OriginalPositions{ OriginalPositionsIn }
	, NewPositions{ NewPositionsIn }
	, ChangeStamp(ChangeStampIn)
	, bFirstMovement(bFirstMovementIn)
{
	check(PointIDs.Num() == OriginalPositions.Num());
	check(PointIDs.Num() == NewPositions.Num());
}

void FLatticeControlPointsMechanicMovementChange::Apply(UObject* Object)
{
	ULatticeControlPointsMechanic* Mechanic = Cast<ULatticeControlPointsMechanic>(Object);
	check(PointIDs.Num() == NewPositions.Num());
	Mechanic->UpdatePointLocations(PointIDs, NewPositions);
	Mechanic->UpdateGizmoLocation();
	Mechanic->bHasChanged = false;
	Mechanic->OnPointsChanged.Broadcast();
}

void FLatticeControlPointsMechanicMovementChange::Revert(UObject* Object)
{
	ULatticeControlPointsMechanic* Mechanic = Cast<ULatticeControlPointsMechanic>(Object);
	check(PointIDs.Num() == OriginalPositions.Num());
	Mechanic->UpdatePointLocations(PointIDs, OriginalPositions);
	Mechanic->UpdateGizmoLocation();
	if (bFirstMovement)
	{
		// If we're undoing the first change, make it possible to change the lattice resolution again
		Mechanic->bHasChanged = false;
	}
	Mechanic->OnPointsChanged.Broadcast();
}

FString FLatticeControlPointsMechanicMovementChange::ToString() const
{
	return TEXT("FLatticeControlPointsMechanicMovementChange");
}

#undef LOCTEXT_NAMESPACE
