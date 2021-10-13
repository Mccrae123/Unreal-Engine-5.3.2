// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "IHasPersonaToolkit.h"
#include "IKRetargetEditorController.h"
#include "IKRetargetMode.h"
#include "IPersonaPreviewScene.h"
#include "PersonaAssetEditorToolkit.h"
#include "EditorUndoClient.h"

class IAnimationSequenceBrowser;
class UIKRetargetSkeletalMeshComponent;
class IDetailsView;
class FGGAssetEditorToolbar;
class UIKRetargeter;
class FIKRetargetToolbar;
class SIKRetargetViewportTabBody;
class FIKRetargetPreviewScene;
struct FIKRetargetPose;
class SEditableTextBox;

namespace IKRetargetEditorModes
{
	extern const FName IKRetargetEditorMode;
}

class FIKRetargetEditor :
	public FPersonaAssetEditorToolkit,
	public IHasPersonaToolkit,
	public FGCObject,
	public FEditorUndoClient,
	public FTickableEditorObject
{
public:

	FIKRetargetEditor();
	virtual ~FIKRetargetEditor() override;

	void InitAssetEditor(
		const EToolkitMode::Type Mode,
		const TSharedPtr< IToolkitHost >& InitToolkitHost,
		UIKRetargeter* Asset);

	/** FAssetEditorToolkit interface */
	virtual void RegisterTabSpawners(const TSharedRef<FTabManager>& InTabManager) override;
	virtual void UnregisterTabSpawners(const TSharedRef<FTabManager>& InTabManager) override;
	virtual FName GetToolkitFName() const override;
	virtual FText GetBaseToolkitName() const override;
	virtual FText GetToolkitName() const override;
	virtual FLinearColor GetWorldCentricTabColorScale() const override;
	virtual FString GetWorldCentricTabPrefix() const override;
	/** END FAssetEditorToolkit interface */
	
	/** FGCObject interface */
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	/** END FGCObject interface */

	//** FTickableEditorObject Interface
	virtual void Tick(float DeltaTime) override;
	virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Always; }
	virtual TStatId GetStatId() const override;
	//~ END FTickableEditorObject Interface

	/** IHasPersonaToolkit interface */
	virtual TSharedRef<IPersonaToolkit> GetPersonaToolkit() const override { return EditorController->PersonaToolkit.ToSharedRef(); }
	/** END IHasPersonaToolkit interface */

	TSharedRef<FIKRetargetEditorController> GetController() const {return EditorController;};

private:

	/** toolbar */
	void BindCommands();
	void ExtendToolbar();
	void FillToolbar(FToolBarBuilder& ToolbarBuilder);
	/** END toolbar */
	
	/** preview scene setup */
	void HandlePreviewSceneCreated(const TSharedRef<IPersonaPreviewScene>& InPersonaPreviewScene);
	void HandlePreviewMeshChanged(USkeletalMesh* InOldSkeletalMesh, USkeletalMesh* InNewSkeletalMesh);
	void HandleDetailsCreated(const TSharedRef<class IDetailsView>& InDetailsView);
	void OnFinishedChangingDetails(const FPropertyChangedEvent& PropertyChangedEvent);
	void SetupAnimInstance();
	/** END preview scene setup */

	/** edit retarget pose */
	void HandleEditPose() const;
	bool CanEditPose() const;
	bool IsEditingPose() const;
	/** END edit retarget pose*/

	/** new/delete retarget pose */
	void HandleNewPose();
	FReply CreateNewPose();
	void HandleDeletePose();
	bool CanDeletePose() const;
	void HandleResetPose();
	TSharedPtr<SWindow> NewPoseWindow;
	TSharedPtr<SEditableTextBox> NewPoseEditableText;
	/** END new/delete retarget pose */

	TArray<TSharedPtr<FName>> PoseNames;
	FText GetCurrentPoseName() const;
	void OnPoseSelected(TSharedPtr<FName> InPoseName, ESelectInfo::Type SelectInfo);
	/* END edit reference pose */

	/* export animation*/
	void ExportAnimation() const;
	/* END edit reference pose */

	void HandleSourceOrTargetIKRigAssetChanged();
	
	/** centralized management of across all views */
	TSharedRef<FIKRetargetEditorController> EditorController;
	
	friend FIKRetargetMode;
};
