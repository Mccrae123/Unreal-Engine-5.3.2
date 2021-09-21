// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "IDetailKeyframeHandler.h"
#include "RigVMModel/RigVMGraph.h"
#include "SRigHierarchyTreeView.h"
#include "SRigSpacePickerWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

class FControlRigEditMode;
class IDetailsView;
class ISequencer;
class SControlPicker;
class SExpandableArea;
class SRigHierarchyTreeView;
class UControlRig;
class URigHierarchy;
class FToolBarBuilder;
class FEditorModeTools;

class SControlRigEditModeTools : public SCompoundWidget, public IDetailKeyframeHandler
{
public:
	SLATE_BEGIN_ARGS(SControlRigEditModeTools) {}
	SLATE_END_ARGS();

	void Construct(const FArguments& InArgs, FControlRigEditMode& InEditMode, UWorld* InWorld);

	/** Set the objects to be displayed in the details panel */
	void SetDetailsObjects(const TArray<TWeakObjectPtr<>>& InObjects);

	/** Set the sequencer we are bound to */
	void SetSequencer(TWeakPtr<ISequencer> InSequencer);

	/** Set The Control Rig we are using*/
	void SetControlRig(UControlRig* ControlRig);

	/** Returns the hierarchy currently being used */
	const URigHierarchy* GetHierarchy() const;

	// IDetailKeyframeHandler interface
	virtual bool IsPropertyKeyable(const UClass* InObjectClass, const class IPropertyHandle& PropertyHandle) const override;
	virtual bool IsPropertyKeyingEnabled() const override;
	virtual void OnKeyPropertyClicked(const IPropertyHandle& KeyedPropertyHandle) override;
	virtual bool IsPropertyAnimated(const class IPropertyHandle& PropertyHandle, UObject *ParentObject) const override;
private:
	/** Sequencer we are currently bound to */
	TWeakPtr<ISequencer> WeakSequencer;

	/** The details view we do most of our work within */
	TSharedPtr<IDetailsView> ControlDetailsView;

	/** Expander to interact with the options of the rig  */
	TSharedPtr<SExpandableArea> RigOptionExpander;
	TSharedPtr<IDetailsView> RigOptionsDetailsView;

	/** Hierarchy picker for controls*/
	TSharedPtr<SRigHierarchyTreeView> HierarchyTreeView;

	/** Hierarchy picker for controls*/
	TSharedPtr<SRigSpacePickerWidget> SpacePickerWidget;

	/** Special picker for controls, no longer used */
	TSharedPtr<SControlPicker> ControlPicker;
	TSharedPtr<SExpandableArea> PickerExpander;

	/** Storage for both sequencer and viewport rigs */
	TWeakObjectPtr<UControlRig> SequencerRig;
	TWeakObjectPtr<UControlRig> ViewportRig;

	/** Display or edit set up for property */
	bool ShouldShowPropertyOnDetailCustomization(const struct FPropertyAndParent& InPropertyAndParent) const;
	bool IsReadOnlyPropertyOnDetailCustomization(const struct FPropertyAndParent& InPropertyAndParent) const;

	/** Called when a manipulator is selected in the picker */
	void OnManipulatorsPicked(const TArray<FName>& Manipulators);

	void HandleModifiedEvent(ERigVMGraphNotifType InNotifType, URigVMGraph* InGraph, UObject* InSubject);
	void HandleSelectionChanged(TSharedPtr<FRigTreeElement> Selection, ESelectInfo::Type SelectInfo);
	void OnRigElementSelected(UControlRig* Subject, FRigControlElement* ControlElement, bool bSelected);

	const FRigControlElementCustomization* HandleGetControlElementCustomization(URigHierarchy* InHierarchy, const FRigElementKey& InControlKey);
	void HandleActiveSpaceChanged(URigHierarchy* InHierarchy, const FRigElementKey& InControlKey, const FRigElementKey& InSpaceKey);
	void HandleSpaceListChanged(URigHierarchy* InHierarchy, const FRigElementKey& InControlKey, const TArray<FRigElementKey>& InSpaceList);
	FReply HandleAddSpaceClicked();
	FReply OnBakeControlsToNewSpaceButtonClicked();

	EVisibility GetRigOptionExpanderVisibility() const;

	void OnRigOptionFinishedChange(const FPropertyChangedEvent& PropertyChangedEvent);

private:
	/** Toolbar functions and windows*/

	void MakePoseDialog();
	void MakeTweenDialog();
	void MakeSnapperDialog();
	void MakeMotionTrailDialog();
	void ToggleEditPivotMode();

	//TODO may put back void MakeSelectionSetDialog();
	//TWeakPtr<SWindow> SelectionSetWindow;

	FEditorModeTools* ModeTools = nullptr;
	FRigTreeDisplaySettings DisplaySettings;
	const FRigTreeDisplaySettings& GetDisplaySettings() const { return DisplaySettings; }
	bool bIsChangingRigHierarchy;

public:
	/** Modes Panel Header Information **/
	void CustomizeToolBarPalette(FToolBarBuilder& ToolBarBuilder);
	FText GetActiveToolName() const;
	FText GetActiveToolMessage() const;
};



