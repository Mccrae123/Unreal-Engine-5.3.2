// Copyright Epic Games, Inc. All Rights Reservekd.

#include "IKRigDefinitionDetails.h"
#include "Widgets/Input/SButton.h"
#include "AssetData.h"
#include "DetailLayoutBuilder.h"
#include "DetailCategoryBuilder.h"
#include "IDetailsView.h"

#include "IKRigDefinition.h"
#include "IKRigController.h"
#include "IKRigSolver.h"
#include "IKRigConstraint.h"

#include "ScopedTransaction.h"
#include "PropertyCustomizationHelpers.h"
#include "Kismet2/SClassPickerDialog.h"
#include "Animation/Skeleton.h"
#include "ClassViewerFilter.h"
#include "Engine/SkeletalMesh.h"
#include "Modules/ModuleManager.h"
#include "Widgets/Input/SEditableTextBox.h"

#define LOCTEXT_NAMESPACE	"IKRigDefinitionDetails"

TSharedRef<IDetailCustomization> FIKRigDefinitionDetails::MakeInstance()
{
	return MakeShareable(new FIKRigDefinitionDetails);
}

FIKRigDefinitionDetails::~FIKRigDefinitionDetails()
{
	if (ObjectChangedDelegate.IsValid())
	{
		FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(ObjectChangedDelegate);
	}
}
void FIKRigDefinitionDetails::CustomizeDetails(const TSharedPtr<IDetailLayoutBuilder>& DetailBuilder)
{
	DetailBuilderWeakPtr = DetailBuilder;
	CustomizeDetails(*DetailBuilder);
}

void FIKRigDefinitionDetails::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	const TArray< TWeakObjectPtr<UObject> >& SelectedObjectsList = DetailBuilder.GetSelectedObjects();
	TArray< TWeakObjectPtr<UIKRigDefinition> > SelectedIKRigDefinitions;

	for (auto SelectionIt = SelectedObjectsList.CreateConstIterator(); SelectionIt; ++SelectionIt)
	{
		if (UIKRigDefinition* TestIKRigDefinition = Cast<UIKRigDefinition>(SelectionIt->Get()))
		{
			SelectedIKRigDefinitions.Add(TestIKRigDefinition);
		}
	}

	// we only support 1 asset for now
	if (SelectedIKRigDefinitions.Num() > 1)
	{
		return;
	}

	IKRigDefinition = SelectedIKRigDefinitions[0];

	if (!IKRigDefinition.IsValid())
	{
		return;
	}

	// create controller
	IKRigController = UIKRigController::GetControllerByRigDefinition(IKRigDefinition.Get());
	
	ObjectChangedDelegate = FCoreUObjectDelegates::OnObjectPropertyChanged.AddRaw(this, &FIKRigDefinitionDetails::OnObjectPostEditChange);
	/////////////////////////////////////////////////////////////////////////////////
	// skeleton set up
	/////////////////////////////////////////////////////////////////////////////////
	IDetailCategoryBuilder& HierarchyCategory = DetailBuilder.EditCategory("Hierarchy");
 
	SelectedAsset = IKRigDefinition->SourceAsset.Get();

	HierarchyCategory.AddCustomRow(FText::FromString("ChangeSkeleton"))
	.NameContent()
	[
		SNew(STextBlock)
		.Font(IDetailLayoutBuilder::GetDetailFont())
		.Text(LOCTEXT("SelectSourceSkeleton", "Souce Skeleton"))
	]
	.ValueContent()
	[
		SNew(SBorder)
		.BorderImage(FCoreStyle::Get().GetBrush("ToolPanel.GroupBorder"))
		.BorderBackgroundColor(FLinearColor::Gray) // Darken the outer border
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			.Padding(2, 2)
			[
				SNew(SBox)
				.WidthOverride(300)
				[
					SNew(SObjectPropertyEntryBox)
					.ObjectPath(this, &FIKRigDefinitionDetails::GetCurrentSourceAsset)
					.OnShouldFilterAsset(this, &FIKRigDefinitionDetails::ShouldFilterAsset)
					.OnObjectChanged(this, &FIKRigDefinitionDetails::OnAssetSelected)
					.AllowClear(false)
					.DisplayUseSelected(true)
					.DisplayBrowse(true)
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			.Padding(2, 2)
			[
				SNew(SButton)
				.ContentPadding(3)
				.IsEnabled(this, &FIKRigDefinitionDetails::CanImport)
				.OnClicked(this, &FIKRigDefinitionDetails::OnImportHierarchy)
				.ToolTipText(LOCTEXT("OnImportHierarchyTooltip", "Change Skeleton Data with Selected Asset. This replaces existing skeleton."))
				[
					SNew(STextBlock)
					.Font(IDetailLayoutBuilder::GetDetailFont())
					.Text(LOCTEXT("UpdateHierarchyTitle", "Update"))
				]
			]
		]
	];

 	IDetailCategoryBuilder& SolverCategory = DetailBuilder.EditCategory("Solver");

	SolverCategory.AddCustomRow(FText::FromString("AddSolver"))
	.NameContent()
	[
		SNullWidget::NullWidget
	]
	.ValueContent()
	[
		SNew(SButton)
		.ContentPadding(3)
		.OnClicked(this, &FIKRigDefinitionDetails::OnShowSolverClassPicker)
		.ToolTipText(LOCTEXT("OnShowSolverListTooltip", "Select Solver to Add"))
		[
			SNew(STextBlock)
			.Font(IDetailLayoutBuilder::GetDetailFont())
			.Text(LOCTEXT("ShowSolverList", "Add Solver"))
		]
	];

	IDetailCategoryBuilder& ConstraintCategory = DetailBuilder.EditCategory("Constraint");
	ConstraintCategory.AddCustomRow(FText::FromString("AddConstraint"))
	.NameContent()
	[
		SNullWidget::NullWidget
	]
	.ValueContent()
	[
		SNew(SButton)
		.ContentPadding(3)
		.OnClicked(this, &FIKRigDefinitionDetails::OnShowConstraintClassPicker)
		.ToolTipText(LOCTEXT("OnShowConstraintListTooltip", "Select Constraint to Add"))
		[
			SNew(STextBlock)
			.Font(IDetailLayoutBuilder::GetDetailFont())
			.Text(LOCTEXT("ShowConstraintList", "Add Constraint"))
		]
	];

	GoalPropertyHandle = DetailBuilder.GetProperty(TEXT("IKGoals"));

	TArray<FName> GoalNames;
	IKRigController->QueryGoals(GoalNames);

	GoalListNames.SetNumZeroed(GoalNames.Num());

	for (int32 Index = 0; Index < GoalNames.Num(); ++Index)
	{
		GoalListNames[Index] = MakeShareable(new FGoalNameListItem(GoalNames[Index]));
	}

	// I need to think about goal modified event OR just IKRigAssetMOdified event to update this
	// for now i'm commenting it out
// 	IDetailCategoryBuilder& GoalPropertyGroup = DetailBuilder.EditCategory("Goals");
// 	GoalPropertyGroup.AddCustomRow(LOCTEXT("GoalsTitleLabel", "Goals"))
// 	.NameContent()
// 	[
// 		GoalPropertyHandle->CreatePropertyNameWidget()
// 	]
// 	.ValueContent()
// 	[
// 		SAssignNew(GoalListView, SListView<FGoalNameListItemPtr>)
// 		.ListItemsSource(&GoalListNames)
// 		.OnGenerateRow(this, &FIKRigDefinitionDetails::OnGenerateWidgetForGoals)
// 	];

//	GoalPropertyHandle->MarkHiddenByCustomization();
}

class FIKRigClassFilter : public IClassViewerFilter
{
public:
	/** All children of these classes will be included unless filtered out by another setting. */
	TSet< const UClass* > AllowedChildrenOfClasses;

	TSet< const UClass* > DisallowedClasses;

	/** Disallowed class flags. */
	EClassFlags DisallowedClassFlags;

	virtual bool IsClassAllowed(const FClassViewerInitializationOptions& InInitOptions, const UClass* InClass, TSharedRef< FClassViewerFilterFuncs > InFilterFuncs) override
	{
		return !InClass->HasAnyClassFlags(DisallowedClassFlags) && InFilterFuncs->IfInClassesSet(DisallowedClasses, InClass) == EFilterReturn::Failed
			&& InFilterFuncs->IfInChildOfClassesSet(AllowedChildrenOfClasses, InClass) != EFilterReturn::Failed;
	}

	virtual bool IsUnloadedClassAllowed(const FClassViewerInitializationOptions& InInitOptions, const TSharedRef< const IUnloadedBlueprintData > InUnloadedClassData, TSharedRef< FClassViewerFilterFuncs > InFilterFuncs) override
	{
		return !InUnloadedClassData->HasAnyClassFlags(DisallowedClassFlags) && InFilterFuncs->IfInClassesSet(DisallowedClasses, InUnloadedClassData) == EFilterReturn::Failed
			&& InFilterFuncs->IfInChildOfClassesSet(AllowedChildrenOfClasses, InUnloadedClassData) != EFilterReturn::Failed;
	}
};

// choose class for them
UClass* SelectClass(UClass* ClassType, const FText& TitleText)
{
	FClassViewerModule& ClassViewerModule = FModuleManager::LoadModuleChecked<FClassViewerModule>("ClassViewer");
	FClassViewerInitializationOptions Options;
	Options.Mode = EClassViewerMode::ClassPicker;
	Options.DisplayMode = EClassViewerDisplayMode::TreeView;
	Options.bShowObjectRootClass = false;
	Options.bExpandRootNodes = true;
	Options.bShowUnloadedBlueprints = true;
	TSharedPtr<FIKRigClassFilter> Filter = MakeShareable(new FIKRigClassFilter);
	Options.ClassFilter = Filter;

	Filter->DisallowedClassFlags = CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists | CLASS_Transient;
	Filter->AllowedChildrenOfClasses.Add(ClassType);
	Filter->DisallowedClasses.Add(ClassType);

	UClass* ChosenClass = nullptr;
	const bool bPressedOk = SClassPickerDialog::PickClass(TitleText, Options, ChosenClass, ClassType);

	if (bPressedOk)
	{
		return ChosenClass;
	}

	return nullptr;
}



FReply FIKRigDefinitionDetails::OnShowSolverClassPicker()
{
	UClass* ChosenClass = SelectClass(UIKRigSolver::StaticClass(), LOCTEXT("SelectSolverClass", "Select Solver Class"));
	if (ChosenClass)
	{
		IKRigController->AddSolver(ChosenClass);
	}

	return FReply::Handled();
}

FReply FIKRigDefinitionDetails::OnShowConstraintClassPicker()
{
	UClass* ChosenClass = SelectClass(UIKRigConstraint::StaticClass(), LOCTEXT("SelectConstraintClass", "Select Constraint Class"));
	if (ChosenClass)
	{
		IKRigController->AddConstraint(ChosenClass);
	}

	return FReply::Handled();
}

bool FIKRigDefinitionDetails::CanImport() const
{
	return (SelectedAsset.IsValid());
}

FString FIKRigDefinitionDetails::GetCurrentSourceAsset() const
{
	return GetPathNameSafe(SelectedAsset.IsValid()? SelectedAsset.Get() : nullptr);
}

bool FIKRigDefinitionDetails::ShouldFilterAsset(const FAssetData& AssetData)
{
	return (AssetData.AssetClass != USkeletalMesh::StaticClass()->GetFName() && AssetData.AssetClass != USkeleton::StaticClass()->GetFName());
}

void FIKRigDefinitionDetails::OnAssetSelected(const FAssetData& AssetData)
{
	SelectedAsset = AssetData.GetAsset();
}

FReply FIKRigDefinitionDetails::OnImportHierarchy()
{
	if (SelectedAsset.IsValid())
	{
		FScopedTransaction Transaction(LOCTEXT("UpdateSkeleton", "Update Skeleton"));
		IKRigDefinition->Modify();

		const FReferenceSkeleton* RefSkeleton = nullptr;
		if (SelectedAsset->IsA(USkeleton::StaticClass()))
		{
			IKRigDefinition->SourceAsset = SelectedAsset.Get();
			RefSkeleton = &(CastChecked<USkeleton>(SelectedAsset)->GetReferenceSkeleton());
		}
		else if (SelectedAsset->IsA(USkeletalMesh::StaticClass()))
		{
			IKRigDefinition->SourceAsset = SelectedAsset.Get();
			RefSkeleton = &(CastChecked<USkeletalMesh>(SelectedAsset)->GetRefSkeleton());
		}

		if (RefSkeleton)
		{
			IKRigController->SetSkeleton(*RefSkeleton);
		}

		// Raw because we don't want to keep alive the details builder when calling the force refresh details
		IDetailLayoutBuilder* DetailLayoutBuilder = DetailBuilderWeakPtr.Pin().Get();
		if (DetailLayoutBuilder)
		{
			DetailLayoutBuilder->ForceRefreshDetails();
		}
	}

	return FReply::Handled();
}

void FIKRigDefinitionDetails::OnObjectPostEditChange(UObject* Object, FPropertyChangedEvent& InPropertyChangedEvent)
{
// 	if (Object == IKRigDefinition || Object->GetOuter())
// 	{
// 		IDetailLayoutBuilder* DetailLayoutBuilder = DetailBuilderWeakPtr.Pin().Get();
// 		if (DetailLayoutBuilder)
// 		{
// 			DetailLayoutBuilder->ForceRefreshDetails();
// 		}
// 	}
}

TSharedRef<ITableRow> FIKRigDefinitionDetails::OnGenerateWidgetForGoals(FGoalNameListItemPtr InItem, const TSharedRef<STableViewBase>& OwnerTable)
{
	return SNew(STableRow<FGoalNameListItemPtr>, OwnerTable)
		[
			SNew(SHorizontalBox)
			+SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SEditableTextBox)
				.Text(this, &FIKRigDefinitionDetails::GetGoalNameText, InItem)
				.OnTextCommitted(this, &FIKRigDefinitionDetails::HandleGoalNameChanged, InItem)
				.SelectAllTextWhenFocused(true)
				.RevertTextOnEscape(true)
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		];
}

void FIKRigDefinitionDetails::HandleGoalNameChanged(const FText& NewName, ETextCommit::Type CommitType, FGoalNameListItemPtr InItem)
{
	//if (CommitType == ETextCommit::OnEnter)
	if (IKRigController)
	{
		if (!NewName.IsEmptyOrWhitespace())
		{
			const FName NewFName = FName(*NewName.ToString());
			if (InItem->DisplayName != NewFName)
			{
				IKRigController->RenameGoal(InItem->GoalName, NewFName);
				InItem->GoalName = NewFName; // if you rename to the same as others, you'll reduce the number of goals
				InItem->DisplayName = NewFName;

				// refresh?

			}
		}
	}
}

FText FIKRigDefinitionDetails::GetGoalNameText(FGoalNameListItemPtr InItem) const
{
	return FText::FromName(InItem->DisplayName);
}
#undef LOCTEXT_NAMESPACE
