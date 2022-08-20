// Copyright Epic Games, Inc. All Rights Reserved.

#include "AnimationModifiersModule.h"
#include "IAssetTools.h"

#include "Animation/AnimSequence.h"
#include "AnimationModifier.h"

#include "SAnimationModifiersTab.h"
#include "AnimationModifierDetailCustomization.h"
#include "AnimationModifierHelpers.h"
#include "AnimationModifiersTabSummoner.h"

#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h" 
#include "SAnimationModifierContentBrowserWindow.h"
#include "ScopedTransaction.h" 
#include "Framework/Application/SlateApplication.h"
#include "Interfaces/IMainFrameModule.h"

#include "AnimationModifierSettings.h"

#include "AnimationModifiersAssetUserData.h"
#include "AssetToolsModule.h"
#include "ContentBrowserMenuContexts.h"
#include "ToolMenuDelegates.h"
#include "ToolMenus.h"

#define LOCTEXT_NAMESPACE "AnimationModifiersModule"

IMPLEMENT_MODULE(FAnimationModifiersModule, AnimationModifiers);

void FAnimationModifiersModule::StartupModule()
{
	// Register class/struct customizations
	FPropertyEditorModule& PropertyEditorModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
	PropertyEditorModule.RegisterCustomClassLayout("AnimationModifier", FOnGetDetailCustomizationInstance::CreateStatic(&FAnimationModifierDetailCustomization::MakeInstance));
	
	// Add application mode extender
	Extender = FWorkflowApplicationModeExtender::CreateRaw(this, &FAnimationModifiersModule::ExtendApplicationMode);
	FWorkflowCentricApplication::GetModeExtenderList().Add(Extender);

	// Register delegates during PostEngineInit as this module is part of preload phase and GEditor is not valid yet
	FCoreDelegates::OnPostEngineInit.AddLambda([this]()
	{
		if (GEditor)
		{
			IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();			
			AssetTools.RegisterAssetTypeActions(MakeShareable(&AssetAction));
			
			GEditor->GetEditorSubsystem<UImportSubsystem>()->OnAssetPostImport.AddRaw(this, &FAnimationModifiersModule::OnAssetPostImport);
			GEditor->GetEditorSubsystem<UImportSubsystem>()->OnAssetReimport.AddRaw(this, &FAnimationModifiersModule::OnAssetPostReimport);	
			RegisterMenus();
		}
	});
}

TSharedRef<FApplicationMode> FAnimationModifiersModule::ExtendApplicationMode(const FName ModeName, TSharedRef<FApplicationMode> InMode)
{
	// For skeleton and animation editor modes add our custom tab factory to it
	if (ModeName == TEXT("SkeletonEditorMode") || ModeName == TEXT("AnimationEditorMode"))
	{
		InMode->AddTabFactory(FCreateWorkflowTabFactory::CreateStatic(&FAnimationModifiersTabSummoner::CreateFactory));
		RegisteredApplicationModes.Add(InMode);
	}
	
	return InMode;
}

void FAnimationModifiersModule::RegisterMenus()
{	
	UToolMenus* ToolMenus = UToolMenus::Get();
	UToolMenu* Menu = ToolMenus->ExtendMenu("ContentBrowser.AssetContextMenu.AnimSequence");
	if (!Menu)
	{
		return;
	}

	FToolMenuSection& Section = Menu->FindOrAddSection("GetAssetActions");

	Section.AddDynamicEntry("AnimModifierActions",
		FNewToolMenuSectionDelegate::CreateLambda([this](FToolMenuSection& InSection)
		{
			const UContentBrowserAssetContextMenuContext* Context = InSection.FindContext<UContentBrowserAssetContextMenuContext>();
			if (!Context)
			{
				return;
			}

			TArray<TWeakObjectPtr<UAnimSequence>> Sequences;
			Algo::TransformIf(Context->SelectedObjects, Sequences, [](const TWeakObjectPtr<UObject>& Object)
			{
				return Cast<UAnimSequence>(Object.Get());
			},
			[](const TWeakObjectPtr<UObject>& Object)
			{
				return TWeakObjectPtr<UAnimSequence>(Cast<UAnimSequence>(Object.Get()));
			});
			
			const FNewMenuDelegate MenuDelegate = FNewMenuDelegate::CreateLambda([Sequences, this](FMenuBuilder& MenuBuilder)
			{
				MenuBuilder.AddMenuEntry(
					LOCTEXT("AnimSequence_AddAnimationModifier", "Add Modifiers"),
					LOCTEXT("AnimSequence_AddAnimationModifierTooltip", "Add new animation modifier(s)."),
				   FSlateIcon(FAppStyle::GetAppStyleSetName(), "ClassIcon.AnimationModifier"),
					FUIAction(FExecuteAction::CreateLambda([Sequences, this]()
					{
						TArray<UAnimSequence*> AnimSequences;

						Algo::TransformIf(Sequences, AnimSequences, 
						[](const TWeakObjectPtr<UAnimSequence>& WeakAnimSequence)
						{
							return WeakAnimSequence.Get() && WeakAnimSequence->IsA<UAnimSequence>();
						},
						[](const TWeakObjectPtr<UAnimSequence>& WeakAnimSequence)
						{
							return WeakAnimSequence.Get();
						});

						ShowAddAnimationModifierWindow(AnimSequences);
					}))
				);
			
				MenuBuilder.AddMenuEntry(
					LOCTEXT("AnimSequence_ApplyAnimationModifier", "Apply Modifiers"),
					LOCTEXT("AnimSequence_ApplyAnimationModifierTooltip", "Applies all contained animation modifier(s)."),
					FSlateIcon(FAppStyle::GetAppStyleSetName(), "ClassIcon.AnimationModifier"),
					FUIAction(FExecuteAction::CreateLambda([Sequences, this]()
					{
						TArray<UAnimSequence*> AnimSequences;
						Algo::TransformIf(Sequences, AnimSequences, 
						[](const TWeakObjectPtr<UAnimSequence>& WeakAnimSequence)
						{
							return WeakAnimSequence.Get() && WeakAnimSequence->IsA<UAnimSequence>();
						},
						[](const TWeakObjectPtr<UAnimSequence>& WeakAnimSequence)
						{
							return WeakAnimSequence.Get();
						});
						
						ApplyAnimationModifiers(AnimSequences);
					}))
				);

				MenuBuilder.AddMenuEntry(
					LOCTEXT("AnimSequence_ApplyOutOfDataAnimationModifier", "Apply out-of-date Modifiers"),
					LOCTEXT("AnimSequence_ApplyOutOfDataAnimationModifierTooltip", "Applies all contained animation modifier(s), if they are out of date."),
					FSlateIcon(FAppStyle::GetAppStyleSetName(), "ClassIcon.AnimationModifier"),
					FUIAction(FExecuteAction::CreateLambda([Sequences, this]()
					{
					TArray<UAnimSequence*> AnimSequences;
					Algo::TransformIf(Sequences, AnimSequences, 
					[](const TWeakObjectPtr<UAnimSequence>& WeakAnimSequence)
					{
						return WeakAnimSequence.Get() && WeakAnimSequence->IsA<UAnimSequence>();
					},
					[](const TWeakObjectPtr<UAnimSequence>& WeakAnimSequence)
					{
						return WeakAnimSequence.Get();
					});

					ApplyAnimationModifiers(AnimSequences, false);
					}))
				);
			});

			IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
			if (AssetTools.IsAssetClassSupported(UAnimationModifier::StaticClass()))
			{
				InSection.AddSubMenu("AnimSequence_AnimationModifiers", LOCTEXT("AnimSequence_AnimationModifiers", "Animation Modifier(s)"),
				LOCTEXT("AnimSequence_AnimationModifiersTooltip", "Animation Modifier actions"),
					FNewToolMenuChoice(MenuDelegate),
					false,
					FSlateIcon(FAppStyle::GetAppStyleSetName(), "ClassIcon.AnimationModifier")
				);
			}
		})
	);
}

void FAnimationModifiersModule::OnAssetPostImport(UFactory* ImportFactory, UObject* ImportedObject)
{
	// Check whether or not the imported asset is a AnimSequence
	if (UAnimSequence* AnimationSequence = Cast<UAnimSequence>(ImportedObject))
	{
		// Check whether or not there are any default modifiers which should be added to the new sequence
		const TArray<TSubclassOf<UAnimationModifier>>& DefaultModifiers = GetDefault<UAnimationModifierSettings>()->DefaultAnimationModifiers;
		if (DefaultModifiers.Num())
		{
			UAnimationModifiersAssetUserData* AssetUserData = FAnimationModifierHelpers::RetrieveOrCreateModifierUserData(AnimationSequence);			
			for (TSubclassOf<UAnimationModifier> ModifierClass : DefaultModifiers)
			{
				if (ModifierClass.Get())
				{
					UObject* Outer = AssetUserData;
					UAnimationModifier* Processor = FAnimationModifierHelpers::CreateModifierInstance(Outer, *ModifierClass);
					AssetUserData->Modify();
					AssetUserData->AddAnimationModifier(Processor);
				}
			}

			if (GetDefault<UAnimationModifierSettings>()->bApplyAnimationModifiersOnImport)
			{
				ApplyAnimationModifiers({AnimationSequence});
			}
		}
	}
}

void FAnimationModifiersModule::OnAssetPostReimport(UObject* ReimportedObject)
{
	// Check whether or not the reimported asset is a AnimSequence
	if (UAnimSequence* AnimationSequence = Cast<UAnimSequence>(ReimportedObject))
	{
		// Check whether or not any contained modifiers should be applied 
		if (GetDefault<UAnimationModifierSettings>()->bApplyAnimationModifiersOnImport)
		{			
			ApplyAnimationModifiers({AnimationSequence});
		}
	}
}

void FAnimationModifiersModule::ShutdownModule()
{
	// Make sure we unregister the class layout 
	FPropertyEditorModule* PropertyEditorModule = FModuleManager::GetModulePtr<FPropertyEditorModule>("PropertyEditor");
	if (PropertyEditorModule)
	{
		PropertyEditorModule->UnregisterCustomClassLayout("AnimationModifier");
	}

	// Remove extender delegate
	FWorkflowCentricApplication::GetModeExtenderList().RemoveAll([this](FWorkflowApplicationModeExtender& StoredExtender) { return StoredExtender.GetHandle() == Extender.GetHandle(); });

	// During shutdown clean up all factories from any modes which are still active/alive
	for (TWeakPtr<FApplicationMode> WeakMode : RegisteredApplicationModes)
	{
		if (WeakMode.IsValid())
		{
			TSharedPtr<FApplicationMode> Mode = WeakMode.Pin();
			Mode->RemoveTabFactory(FAnimationModifiersTabSummoner::AnimationModifiersName);
		}
	}
		
	FAssetToolsModule* AssetToolsModulePtr = FModuleManager::GetModulePtr<FAssetToolsModule>("AssetTools");
	if (AssetToolsModulePtr)
	{		
		AssetToolsModulePtr->Get().UnregisterAssetTypeActions(MakeShareable(&AssetAction));
	}

	RegisteredApplicationModes.Empty();

	if (GEditor)
	{
		GEditor->GetEditorSubsystem<UImportSubsystem>()->OnAssetPostImport.RemoveAll(this);
	}
}

void FAnimationModifiersModule::ShowAddAnimationModifierWindow(const TArray<UAnimSequence*>& InSequences)
{
	TSharedPtr<SAnimationModifierContentBrowserWindow> WindowContent;

	TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(LOCTEXT("WindowTitle", "Add Animation Modifier(s)"))
		.SizingRule(ESizingRule::UserSized)
		.ClientSize(FVector2D(500, 500));

	Window->SetContent
	(
		SAssignNew(WindowContent, SAnimationModifierContentBrowserWindow)
		.WidgetWindow(Window)
		.AnimSequences(InSequences)
	);

	TSharedPtr<SWindow> ParentWindow;

	if (FModuleManager::Get().IsModuleLoaded("MainFrame"))
	{
		IMainFrameModule& MainFrame = FModuleManager::LoadModuleChecked<IMainFrameModule>("MainFrame");
		ParentWindow = MainFrame.GetParentWindow();
	}

	FSlateApplication::Get().AddModalWindow(Window, ParentWindow, false);
}

void FAnimationModifiersModule::ApplyAnimationModifiers(const TArray<UAnimSequence*>& InSequences, bool bForceApply /*= true*/)
{
	const FScopedTransaction Transaction(LOCTEXT("UndoAction_ApplyModifiers", "Applying Animation Modifier(s) to Animation Sequence(s)"));
	
	// Iterate over each Animation Sequence and all of its contained modifiers, applying each one
	UE::Anim::FApplyModifiersScope Scope;
	TArray<UAnimationModifiersAssetUserData*> AssetUserData;
	for (UAnimSequence* AnimationSequence : InSequences)
	{
		if (AnimationSequence)
		{
			UAnimationModifiersAssetUserData* UserData = AnimationSequence->GetAssetUserData<UAnimationModifiersAssetUserData>();
			if (UserData)
			{
				AnimationSequence->Modify();
				const TArray<UAnimationModifier*>& ModifierInstances = UserData->GetAnimationModifierInstances();
				for (UAnimationModifier* Modifier : ModifierInstances)
				{
					if (bForceApply || !Modifier->IsLatestRevisionApplied())
					{
						Modifier->ApplyToAnimationSequence(AnimationSequence);
					}
				}
			}
		}		
	}
}

#undef LOCTEXT_NAMESPACE // "AnimationModifiersModule"
