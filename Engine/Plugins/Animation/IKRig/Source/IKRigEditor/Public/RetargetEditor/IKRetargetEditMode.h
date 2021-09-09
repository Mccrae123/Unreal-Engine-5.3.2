// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Retargeter/IKRetargeter.h"
#include "IPersonaEditMode.h"

class FIKRetargetEditorController;
class FIKRetargetEditor;
class FIKRetargetPreviewScene;

class FIKRetargetEditMode : public IPersonaEditMode
{
public:
	static FName ModeName;
	
	FIKRetargetEditMode() = default;

	/** glue for all the editor parts to communicate */
	void SetEditorController(const TSharedPtr<FIKRetargetEditorController> InEditorController) { EditorController = InEditorController; };

	/** IPersonaEditMode interface */
	virtual bool GetCameraTarget(FSphere& OutTarget) const override;
	virtual class IPersonaPreviewScene& GetAnimPreviewScene() const override;
	virtual void GetOnScreenDebugInfo(TArray<FText>& OutDebugInfo) const override;
	/** END IPersonaEditMode interface */

	/** FEdMode interface */
	virtual void Tick(FEditorViewportClient* ViewportClient, float DeltaTime) override;
	virtual void Render(const FSceneView* View, FViewport* Viewport, FPrimitiveDrawInterface* PDI) override;
	virtual void DrawHUD(FEditorViewportClient* ViewportClient, FViewport* Viewport, const FSceneView* View, FCanvas* Canvas) override;
	virtual bool IsCompatibleWith(FEditorModeID OtherModeID) const override { return true; }
	virtual bool AllowWidgetMove() override;
	virtual bool ShouldDrawWidget() const override;
	virtual bool UsesTransformWidget() const override;
	virtual bool UsesTransformWidget(UE::Widget::EWidgetMode CheckMode) const override;
	virtual FVector GetWidgetLocation() const override;
	virtual bool HandleClick(FEditorViewportClient* InViewportClient, HHitProxy* HitProxy, const FViewportClick& Click) override;
	virtual bool StartTracking(FEditorViewportClient* InViewportClient, FViewport* InViewport) override;
	virtual bool EndTracking(FEditorViewportClient* InViewportClient, FViewport* InViewport) override;
	virtual bool InputDelta(FEditorViewportClient* InViewportClient, FViewport* InViewport, FVector& InDrag, FRotator& InRot, FVector& InScale) override;
	virtual bool GetCustomDrawingCoordinateSystem(FMatrix& InMatrix, void* InData) override;
	virtual bool GetCustomInputCoordinateSystem(FMatrix& InMatrix, void* InData) override;
	/** END FEdMode interface */

	bool IsBoneSelected(const FName& BoneName) const;

	void HandleBoneSelectedInViewport(const FName& BoneName, bool bReplace);

private:
	/** The hosting app */
	TWeakPtr<FIKRetargetEditorController> EditorController;

	void DrawBoneGizmo(
		FPrimitiveDrawInterface* PDI,
		const FTransform& BoneTransform,
		const FVector& Start,
		const FVector& End,
		float Size,
		float Thickness,
		bool bIsSelected) const;

	/** chain viewport selection state */
	TArray<FName> SelectedBones;
	bool bRotatingBones = false;
};
