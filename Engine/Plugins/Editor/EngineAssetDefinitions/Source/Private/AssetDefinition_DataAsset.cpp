// Copyright Epic Games, Inc. All Rights Reserved.

#include "AssetDefinition_DataAsset.h"

#include "ClassViewerFilter.h"
#include "ContentBrowserMenuContexts.h"
#include "ObjectTools.h"
#include "SDetailsDiff.h"
#include "ToolMenu.h"
#include "ToolMenus.h"
#include "Kismet2/SClassPickerDialog.h"
#include "Engine/Engine.h"
#include "ToolMenuSection.h"

#define LOCTEXT_NAMESPACE "UAssetDefinition_DataAsset"

namespace MenuExtension_DataAsset
{
	class FNewNodeClassFilter : public IClassViewerFilter
	{
	public:
		FNewNodeClassFilter(UClass* InBaseClass)
			: BaseClass(InBaseClass)
		{
		}

		virtual bool IsClassAllowed(const FClassViewerInitializationOptions& InInitOptions, const UClass* InClass, TSharedRef< FClassViewerFilterFuncs > InFilterFuncs) override
		{
			if (InClass != nullptr)
			{
				return InClass->IsChildOf(BaseClass);
			}
			return false;
		}

		virtual bool IsUnloadedClassAllowed(const FClassViewerInitializationOptions& InInitOptions, const TSharedRef< const IUnloadedBlueprintData > InUnloadedClassData, TSharedRef< FClassViewerFilterFuncs > InFilterFuncs) override
		{
			return InUnloadedClassData->IsChildOf(BaseClass);
		}

	private:
		UClass* BaseClass;
	};

	bool IsChangeDataAssetClassVisible(const FToolMenuContext& MenuContext)
	{
		if (const UContentBrowserAssetContextMenuContext* Context = UContentBrowserAssetContextMenuContext::FindContextWithAssets(MenuContext))
		{
			for (const FContentBrowserItem& SelectedItem : Context->GetSelectedItems())
			{
				if (SelectedItem.CanEdit())
				{
					return true;
				}
			}
		}
		return false;
	}

	void ExecuteChangeDataAssetClass(const FToolMenuContext& MenuContext)
	{
		if (const UContentBrowserAssetContextMenuContext* Context = UContentBrowserAssetContextMenuContext::FindContextWithAssets(MenuContext))
		{
			const FText TitleText = LOCTEXT("DataAsset_PickNewDataAssetClass", "Pick New DataAsset Class");
			FClassViewerInitializationOptions Options;
			Options.ClassFilters.Add(MakeShared<FNewNodeClassFilter>(UDataAsset::StaticClass()));
			UClass* OutNewDataAssetClass = nullptr;
			const bool bPressedOk = SClassPickerDialog::PickClass(TitleText, Options, OutNewDataAssetClass, UDataAsset::StaticClass());

			if (bPressedOk && OutNewDataAssetClass != nullptr)
			{
				TSet<FName> EditableAssets;
				{
					const TArray<FContentBrowserItem>& SelectedItems = Context->GetSelectedItems();
					EditableAssets.Reserve(SelectedItems.Num());
					for (const FContentBrowserItem& SelectedItem : Context->GetSelectedItems())
					{
						if (SelectedItem.CanEdit())
						{
							EditableAssets.Add(SelectedItem.GetInternalPath());
						}
					}
				}
				ensure(!EditableAssets.IsEmpty());

				TArray<UDataAsset*> DataAssets = Context->LoadSelectedObjectsIf<UDataAsset>([&EditableAssets](const FAssetData& AssetData)
				{
					return EditableAssets.Contains(*AssetData.GetObjectPathString());
				});

				for (TWeakObjectPtr<UDataAsset> DataAssetPtr : DataAssets)
				{
					if (UDataAsset* OldDataAsset = DataAssetPtr.Get())
					{
						if (OldDataAsset && OldDataAsset->IsValidLowLevel())
						{
							FName ObjectName = OldDataAsset->GetFName();
							UObject* Outer = OldDataAsset->GetOuter();
							OldDataAsset->Rename(nullptr, GetTransientPackage(), REN_DoNotDirty | REN_DontCreateRedirectors);

							UObject* NewDataAsset = NewObject<UObject>(Outer, OutNewDataAssetClass, ObjectName, OldDataAsset->GetFlags());

							// Migrate Data
							{
								UEngine::FCopyPropertiesForUnrelatedObjectsParams CopyOptions;
								CopyOptions.bNotifyObjectReplacement = true;
								UEngine::CopyPropertiesForUnrelatedObjects(OldDataAsset, NewDataAsset, CopyOptions);
							}

							NewDataAsset->MarkPackageDirty();

							// Consolidate or "Replace" the old object with the new object for any living references.
							bool bShowDeleteConfirmation = false;
							TArray<UObject*> OldDataAssetArray({ (UObject*)OldDataAsset });
							ObjectTools::ConsolidateObjects(NewDataAsset, OldDataAssetArray, bShowDeleteConfirmation);
						}
					}
				}
			}
		}
	}
	
	static FDelayedAutoRegisterHelper DelayedAutoRegister(EDelayedRegisterRunPhase::EndOfEngineInit, []{ 
		UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateLambda([]()
		{
			FToolMenuOwnerScoped OwnerScoped(UE_MODULE_NAME);
			UToolMenu* Menu = UE::ContentBrowser::ExtendToolMenu_AssetContextMenu(UDataAsset::StaticClass());
	        
			FToolMenuSection& Section = Menu->FindOrAddSection("GetAssetActions");
			Section.AddDynamicEntry(NAME_None, FNewToolMenuSectionDelegate::CreateStatic([](FToolMenuSection& InSection)
			{
				{
					const TAttribute<FText> Label = LOCTEXT("DataAsset_ChangeClass", "Convert to Different DataAsset Type");
					const TAttribute<FText> ToolTip = LOCTEXT("DataAsset_ChangeClassTip", "Change the class these Data Assets are subclassed from.");
					const FSlateIcon Icon = FSlateIcon(FAppStyle::GetAppStyleSetName(), "ClassIcon.DataAsset");

					FToolUIAction UIAction;
					UIAction.ExecuteAction = FToolMenuExecuteAction::CreateStatic(&ExecuteChangeDataAssetClass);
					UIAction.IsActionVisibleDelegate = FToolMenuIsActionButtonVisible::CreateStatic(&IsChangeDataAssetClassVisible);

					InSection.AddMenuEntry("DataAsset_ChangeClass", Label, ToolTip, Icon, UIAction);
				}
			}));
		}));
	});
}


FText UAssetDefinition_DataAsset::GetAssetDisplayName(const FAssetData& AssetData) const
{
	static const FName NAME_RowStructure(TEXT("RowStructure"));
	
	if (AssetData.IsValid())
	{
		const FAssetDataTagMapSharedView::FFindTagResult RowStructureTag = AssetData.TagsAndValues.FindTag(NAME_RowStructure);
		if (RowStructureTag.IsSet())
		{
			if (UScriptStruct* FoundStruct = UClass::TryFindTypeSlow<UScriptStruct>(RowStructureTag.GetValue(), EFindFirstObjectOptions::ExactClass))
			{
				return FText::Format(LOCTEXT("DataTableWithRowType", "Data Table ({0})"), FoundStruct->GetDisplayNameText());
			}
		}
	}

	return FText::GetEmpty();
}

EAssetCommandResult UAssetDefinition_DataAsset::PerformAssetDiff(const FAssetDiffArgs& DiffArgs) const
{
	// sometimes we're comparing different revisions of one single asset (other 
	// times we're comparing two completely separate assets altogether)
	const bool bIsSingleAsset = (DiffArgs.OldAsset->GetFName() == DiffArgs.NewAsset->GetFName());
	static const FText BasicWindowTitle = LOCTEXT("NamelessDataAssetDiff", "DataAsset Diff");

	const FText WindowTitle = !bIsSingleAsset ? BasicWindowTitle : FText::Format(LOCTEXT("DataAsset Diff", "{0} - DataAsset Diff"), FText::FromString(DiffArgs.NewAsset->GetName()));

	SDetailsDiff::CreateDiffWindow(WindowTitle, DiffArgs.OldAsset, DiffArgs.NewAsset, DiffArgs.OldRevision, DiffArgs.NewRevision);
	
	return EAssetCommandResult::Handled;
}

#undef LOCTEXT_NAMESPACE
