// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once
#include "LevelSnapshotsEditorProjectSettings.h"
#include "Dialogs/CustomDialog.h"

class ULevelSnapshotsEditorProjectSettings;
DECLARE_DELEGATE_TwoParams(FCloseCreationFormDelegate, bool, FText)

struct FLevelSnapshotsEditorCreationForm
{
	static TSharedRef<SWindow> MakeAndShowCreationWindow(
		const FCloseCreationFormDelegate& CallOnClose, ULevelSnapshotsEditorProjectSettings* InProjectSettings);
};

class SLevelSnapshotsEditorCreationForm : public SCustomDialog
{
public:

	SLATE_BEGIN_ARGS(SLevelSnapshotsEditorCreationForm)
	{}
	

	SLATE_END_ARGS()

	void Construct(
		const FArguments& InArgs, TWeakPtr< SWindow > InWidgetWindow, const FCloseCreationFormDelegate& CallOnClose, ULevelSnapshotsEditorProjectSettings* InProjectSettings);

	~SLevelSnapshotsEditorCreationForm();

	FText GetNameOverrideText() const;

	void SetNameOverrideText(const FText& InNewText, ETextCommit::Type InCommitType);

	FText GetDescriptionText() const;

	void SetDescriptionText(const FText& InNewText, ETextCommit::Type InCommitType);

	FText GetPathOverrideText() const;

	void SetPathOverrideText(const FText& InNewText, ETextCommit::Type InCommitType);

	EVisibility GetNameDiffersFromDefaultAsVisibility() const;

	EVisibility GetDirDiffersFromDefaultAsVisibility() const;

	FReply OnResetNameClicked();
	
	FReply OnResetDirClicked();

	FReply OnCreateButtonPressed();

private:
	
	TWeakPtr< SWindow > WidgetWindow;
	TSharedPtr<SWidget> ResetPathButton;

	TWeakObjectPtr<ULevelSnapshotsEditorProjectSettings> ProjectSettingsObjectPtr;

	bool bNameDiffersFromDefault = false;
	bool bDirDiffersFromDefault = false;

	FText DescriptionText;

	FCloseCreationFormDelegate CallOnCloseDelegate;

	bool bWasCreateSnapshotPressed = false;
};
