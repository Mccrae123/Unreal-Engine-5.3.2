// Copyright Epic Games, Inc. All Rights Reserved.
/**
* Hold the View for the Tween Widget
*/
#pragma once

#include "CoreMinimal.h"
#include "Widgets/SWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"
#include "Tools/ControlRigPose.h"
#include "Tools/ControlRigTweener.h"

class UControlRig;
class ISequencer;
class FControlRigEditModeToolkit;

class SControlRigTweenSlider : public SCompoundWidget
{
	SLATE_BEGIN_ARGS(SControlRigTweenSlider) {}
	SLATE_ARGUMENT(TSharedPtr<FBaseAnimSlider>, InAnimSlider)
	SLATE_END_ARGS()
	~SControlRigTweenSlider()
	{
	}

	void Construct(const FArguments& InArgs);
	void SetAnimSlider(TSharedPtr<FBaseAnimSlider>& InAnimSlider) { AnimSlider = InAnimSlider; }

private:


	/*
	* Delegates and Helpers
	*/
	void OnPoseBlendChanged(double ChangedVal);
	void OnPoseBlendCommited(double ChangedVal, ETextCommit::Type Type);
	void OnBeginSliderMovement();
	void OnEndSliderMovement(double NewValue);
	double OnGetPoseBlendValue() const { return PoseBlendValue; }

	bool Setup();
	double PoseBlendValue;
	bool bIsBlending;
	bool bSliderStartedTransaction;

	
	TWeakPtr<ISequencer> WeakSequencer;
	TSharedPtr<FBaseAnimSlider> AnimSlider;

};


class SControlRigTweenWidget : public SCompoundWidget
{
	SLATE_BEGIN_ARGS(SControlRigTweenWidget) {}
	SLATE_ARGUMENT(TSharedPtr<FControlRigEditModeToolkit>, InOwningToolkit)
	SLATE_END_ARGS()
		~SControlRigTweenWidget()
	{
	}

	void Construct(const FArguments& InArgs);

private:

	void OnSelectSliderTool(int32 Index);
	FText GetActiveSliderName() const;
	FText GetActiveSliderTooltip() const;



	FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override
	{
		return FReply::Handled().DetectDrag(SharedThis(this), EKeys::LeftMouseButton);
	};
	FReply OnDragDetected(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent);
	void FinishDraggingWidget(const FVector2D InLocation);

	TWeakPtr<ISequencer> WeakSequencer;
	TWeakPtr<FControlRigEditModeToolkit> OwningToolkit;
	FAnimBlendTooLManager  AnimBlendTools;

	TSharedPtr<SControlRigTweenSlider> SliderWidget;
	static int32 ActiveSlider;

};
