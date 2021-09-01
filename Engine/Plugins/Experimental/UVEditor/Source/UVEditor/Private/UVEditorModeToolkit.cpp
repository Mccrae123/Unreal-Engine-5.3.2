// Copyright Epic Games, Inc. All Rights Reserved.

#include "UVEditorModeToolkit.h"

#include "EditorStyleSet.h" //FEditorStyle
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Framework/MultiBox/MultiBoxExtender.h"
#include "Framework/MultiBox/MultiBoxDefs.h"
#include "IDetailsView.h"
#include "Modules/ModuleManager.h"
#include "Tools/UEdMode.h"
#include "UVEditorBackgroundPreview.h"
#include "UVEditorCommands.h"
#include "UVEditorMode.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Text/STextBlock.h"

#include "EdModeInteractiveToolsContext.h"

#define LOCTEXT_NAMESPACE "FUVEditorModeToolkit"

namespace UVEditorModeToolkitLocals
{
	/** 
	 * Support for undoing a tool start in such a way that we go back to the mode's default
	 * tool on undo.
	 */
	class FUVEditorBeginToolChange : public FToolCommandChange
	{
	public:
		virtual void Apply(UObject* Object) override
		{
			// Do nothing, since we don't allow a re-do back into a tool
		}

		virtual void Revert(UObject* Object) override
		{
			UUVEditorMode* Mode = Cast<UUVEditorMode>(Object);
			// Don't really need the check for default tool since we theoretically shouldn't
			// be issuing this transaction for starting the default tool, but still...
			if (Mode && !Mode->IsDefaultToolActive())
			{
				Mode->GetInteractiveToolsContext()->EndTool(EToolShutdownType::Cancel);
				Mode->ActivateDefaultTool();
			}
		}

		virtual bool HasExpired(UObject* Object) const override
		{
			// To not be expired, we must be in some non-default tool.
			UUVEditorMode* Mode = Cast<UUVEditorMode>(Object);
			return !(Mode && Mode->GetInteractiveToolsContext()->ToolManager->HasAnyActiveTool() && !Mode->IsDefaultToolActive());
		}

		virtual FString ToString() const override
		{
			return TEXT("FUVEditorBeginToolChange");
		}
	};
}

FUVEditorModeToolkit::FUVEditorModeToolkit()
{
	// Construct the panel that we will give in GetInlineContent().
	// This could probably be done in Init() instead, but constructor
	// makes it easy to guarantee that GetInlineContent() will always
	// be ready to work.

	SAssignNew(ToolkitWidget, SBorder)
		.HAlign(HAlign_Fill)
		.Padding(4)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SAssignNew(ToolMessageArea, STextBlock)
				.AutoWrapText(true)
			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
			.Text(LOCTEXT("UVEditorToolsLabel", "UV Editor Tools"))
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Left)
			.Padding(1.f)
			[
				SAssignNew(ToolButtonsContainer, SBorder)
				.BorderImage(FEditorStyle::GetBrush("ToolPanel.GroupBorder"))
				.Padding(FMargin(4, 2, 0, 0))
			]

			+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(2, 0, 0, 0)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SAssignNew(ToolWarningArea, STextBlock)
					.AutoWrapText(true)
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
					.ColorAndOpacity(FSlateColor(FLinearColor(0.9f, 0.15f, 0.15f))) //TODO: This probably needs to not be hardcoded
					.Text(FText::GetEmpty())
					.Visibility(EVisibility::Collapsed)
				]

				+ SVerticalBox::Slot()
				[
					SAssignNew(ToolDetailsContainer, SBorder)
					.BorderImage(FEditorStyle::GetBrush("NoBorder"))
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SAssignNew(ToolMessageArea, STextBlock)
					.AutoWrapText(true)
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
					.Text(FText::GetEmpty())
				]
			]

            // Todo: Move this out of here once we figure out how to add menus/UI to the viewport
			+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(2, 0, 0, 0)
			[
					SNew(SVerticalBox)

					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SAssignNew(ToolMessageArea, STextBlock)
						.AutoWrapText(true)
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
						.Text(LOCTEXT("UVEditorSettingsLabel", "UV Editor Settings"))
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SAssignNew(EditorDetailsContainer, SBorder)
						.BorderImage(FEditorStyle::GetBrush("NoBorder"))
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SAssignNew(BackgroundDetailsContainer, SBorder)
						.BorderImage(FEditorStyle::GetBrush("NoBorder"))
					]

			]

		];
}

FUVEditorModeToolkit::~FUVEditorModeToolkit()
{
}

void FUVEditorModeToolkit::Init(const TSharedPtr<IToolkitHost>& InitToolkitHost, TWeakObjectPtr<UEdMode> InOwningMode)
{
	FModeToolkit::Init(InitToolkitHost, InOwningMode);	

	UUVEditorMode* UVEditorMode = Cast<UUVEditorMode>(GetScriptableEditorMode());
	check(UVEditorMode);

	// Currently, there's no EToolChangeTrackingMode that reverts back to a default tool on undo (if we add that
	// support, the tool manager will need to be aware of the default tool). So, we instead opt to do our own management
	// of tool start transactions. See FUVEditorModeToolkit::OnToolStarted for how we issue the transactions.
	UVEditorMode->GetInteractiveToolsContext()->ToolManager->ConfigureChangeTrackingMode(EToolChangeTrackingMode::NoChangeTracking);

	// Build the tool palette
	const FUVEditorCommands& CommandInfos = FUVEditorCommands::Get();
	TSharedRef<FUICommandList> CommandList = GetToolkitCommands();
	FUniformToolBarBuilder ToolbarBuilder(CommandList, FMultiBoxCustomization(UVEditorMode->GetModeInfo().ToolbarCustomizationName), TSharedPtr<FExtender>(), false);
	ToolbarBuilder.SetStyle(&FEditorStyle::Get(), "PaletteToolBar");

	// TODO: Add more tools here
	ToolbarBuilder.AddToolBarButton(CommandInfos.BeginSelectTool);
	ToolbarBuilder.AddToolBarButton(CommandInfos.BeginTransformTool);
	ToolbarBuilder.AddToolBarButton(CommandInfos.BeginLayoutTool);

	// Hook in the tool palette
	ToolButtonsContainer->SetContent(ToolbarBuilder.MakeWidget());

	// Hook up the tool detail panel
	ToolDetailsContainer->SetContent(DetailsView.ToSharedRef());

	// Hook up the editor detail panel
	EditorDetailsContainer->SetContent(ModeDetailsView.ToSharedRef());

	// Hook up the background detail panel
	{
		FPropertyEditorModule& PropertyEditorModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");

		FDetailsViewArgs BackgroundDetailsViewArgs;
		BackgroundDetailsViewArgs.bAllowSearch = false;
		BackgroundDetailsViewArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;
		BackgroundDetailsViewArgs.bHideSelectionTip = true;
		BackgroundDetailsViewArgs.DefaultsOnlyVisibility = EEditDefaultsOnlyNodeVisibility::Automatic;
		BackgroundDetailsViewArgs.bShowOptions = false;
		BackgroundDetailsViewArgs.bAllowMultipleTopLevelObjects = true;

		CustomizeDetailsViewArgs(BackgroundDetailsViewArgs);		// allow subclass to customize arguments

		BackgroundDetailsView = PropertyEditorModule.CreateDetailView(BackgroundDetailsViewArgs);
		BackgroundDetailsContainer->SetContent(BackgroundDetailsView.ToSharedRef());
	}
	
	// Set up the overlay. Larely copied from ModelingToolsEditorModeToolkit, except
	// that we don't have a "complete" option since we don't have a non-tool state.
	// TODO: We could put some of the shared code in some common place.
	SAssignNew(ViewportOverlayWidget, SHorizontalBox)

	+SHorizontalBox::Slot()
	.HAlign(HAlign_Center)
	.VAlign(VAlign_Bottom)
	.Padding(FMargin(0.0f, 0.0f, 0.f, 15.f))
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::Get().GetBrush("EditorViewport.OverlayBrush"))
		.Padding(8.f)
		[
			SNew(SHorizontalBox)

			// TODO: Need to add a tool icon here the way we do in modeling mode.
			//+SHorizontalBox::Slot()
			//.AutoWidth()
			//.VAlign(VAlign_Center)
			//.Padding(FMargin(0.f, 0.f, 8.f, 0.f))
			//[
			//	SNew(SImage)
			//	.Image_Lambda([this] () { return ActiveToolIcon; })
			//]

			+SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(0.f, 0.f, 8.f, 0.f))
			[
				SNew(STextBlock)
				.Text(this, &FUVEditorModeToolkit::GetActiveToolDisplayName)
			]

			+SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(FMargin(0.0, 0.f, 2.f, 0.f))
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
				.TextStyle( FAppStyle::Get(), "DialogButtonText" )
				.Text(LOCTEXT("OverlayAccept", "Accept"))
				.ToolTipText(LOCTEXT("OverlayAcceptTooltip", "Accept/Commit the results of the active Tool [Enter]"))
				.HAlign(HAlign_Center)
				.OnClicked_Lambda([this]() { 
					GetScriptableEditorMode()->GetInteractiveToolsContext()->EndTool(EToolShutdownType::Accept);
					Cast<UUVEditorMode>(GetScriptableEditorMode())->ActivateDefaultTool();
					return FReply::Handled(); 
					})
				.IsEnabled_Lambda([this]() { return GetScriptableEditorMode()->GetInteractiveToolsContext()->CanAcceptActiveTool(); })
				.Visibility_Lambda([this]() { return GetScriptableEditorMode()->GetInteractiveToolsContext()->ActiveToolHasAccept() ? EVisibility::Visible : EVisibility::Collapsed; })
			]

			+SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(FMargin(2.0, 0.f, 0.f, 0.f))
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "Button")
				.TextStyle( FAppStyle::Get(), "DialogButtonText" )
				.Text(LOCTEXT("OverlayCancel", "Cancel"))
				.ToolTipText(LOCTEXT("OverlayCancelTooltip", "Cancel the active Tool [Esc]"))
				.HAlign(HAlign_Center)
				.OnClicked_Lambda([this]() { 
					GetScriptableEditorMode()->GetInteractiveToolsContext()->EndTool(EToolShutdownType::Cancel); 
					Cast<UUVEditorMode>(GetScriptableEditorMode())->ActivateDefaultTool();
					return FReply::Handled(); 
					})
				.IsEnabled_Lambda([this]() { return GetScriptableEditorMode()->GetInteractiveToolsContext()->CanCancelActiveTool(); })
				.Visibility_Lambda([this]() { return GetScriptableEditorMode()->GetInteractiveToolsContext()->ActiveToolHasAccept() ? EVisibility::Visible : EVisibility::Collapsed; })
			]

			// Currently haven't needed a complete button, but this is here in case that changes.
			//+SHorizontalBox::Slot()
			//.AutoWidth()
			//.Padding(FMargin(2.0, 0.f, 0.f, 0.f))
			//[
			//	SNew(SButton)
			//	.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
			//	.TextStyle( FAppStyle::Get(), "DialogButtonText" )
			//	.Text(LOCTEXT("OverlayComplete", "Complete"))
			//	.ToolTipText(LOCTEXT("OverlayCompleteTooltip", "Exit the active Tool [Enter]"))
			//	.HAlign(HAlign_Center)
			//	.OnClicked_Lambda([this]() { GetScriptableEditorMode()->GetInteractiveToolsContext()->EndTool(EToolShutdownType::Completed); return FReply::Handled(); })
			//	.IsEnabled_Lambda([this]() { return GetScriptableEditorMode()->GetInteractiveToolsContext()->CanCompleteActiveTool(); })
			//	.Visibility_Lambda([this]() { return GetScriptableEditorMode()->GetInteractiveToolsContext()->CanCompleteActiveTool() ? EVisibility::Visible : EVisibility::Collapsed; })
			//]
		]	
	];

}

FName FUVEditorModeToolkit::GetToolkitFName() const
{
	return FName("UVEditorMode");
}

FText FUVEditorModeToolkit::GetBaseToolkitName() const
{
	return NSLOCTEXT("UVEditorModeToolkit", "DisplayName", "UVEditorMode");
}


void FUVEditorModeToolkit::SetBackgroundSettings(UObject* InSettingsObject)
{
	BackgroundDetailsView->SetObject(InSettingsObject);
}


void FUVEditorModeToolkit::UpdateActiveToolProperties()
{
	UInteractiveTool* CurTool = GetScriptableEditorMode()->GetToolManager()->GetActiveTool(EToolSide::Left);
	if (CurTool != nullptr)
	{
		DetailsView->SetObjects(CurTool->GetToolProperties(true));
	}
}

void FUVEditorModeToolkit::InvalidateCachedDetailPanelState(UObject* ChangedObject)
{
	DetailsView->InvalidateCachedState();
}

void FUVEditorModeToolkit::OnToolStarted(UInteractiveToolManager* Manager, UInteractiveTool* Tool)
{
	FModeToolkit::OnToolStarted(Manager, Tool);
	
	UInteractiveTool* CurTool = GetScriptableEditorMode()->GetToolManager()->GetActiveTool(EToolSide::Left);
	CurTool->OnPropertySetsModified.AddSP(this, &FUVEditorModeToolkit::UpdateActiveToolProperties);
	CurTool->OnPropertyModifiedDirectlyByTool.AddSP(this, &FUVEditorModeToolkit::InvalidateCachedDetailPanelState);

	ActiveToolName = Tool->GetToolInfo().ToolDisplayName;

	if (GetScriptableEditorMode()->GetInteractiveToolsContext()->ActiveToolHasAccept())
	{
		// Add the accept/cancel overlay
		GetToolkitHost()->AddViewportOverlayWidget(ViewportOverlayWidget.ToSharedRef());
	}
	
	// Issue a tool start transaction unless we are starting the default tool, because we can't
	// undo or revert out of the default tool.
	UUVEditorMode* Mode = Cast<UUVEditorMode>(GetScriptableEditorMode());
	if (!Mode->IsDefaultToolActive())
	{
		Mode->GetInteractiveToolsContext()->GetTransactionAPI()->AppendChange(
			Mode, MakeUnique<UVEditorModeToolkitLocals::FUVEditorBeginToolChange>(), LOCTEXT("ActivateTool", "Activate Tool"));
	}
}

void FUVEditorModeToolkit::OnToolEnded(UInteractiveToolManager* Manager, UInteractiveTool* Tool)
{
	FModeToolkit::OnToolEnded(Manager, Tool);

	ActiveToolName = FText::GetEmpty();

	if (IsHosted())
	{
		GetToolkitHost()->RemoveViewportOverlayWidget(ViewportOverlayWidget.ToSharedRef());
	}

	UInteractiveTool* CurTool = GetScriptableEditorMode()->GetToolManager()->GetActiveTool(EToolSide::Left);
	if (CurTool)
	{
		CurTool->OnPropertySetsModified.RemoveAll(this);
		CurTool->OnPropertyModifiedDirectlyByTool.RemoveAll(this);
	}
}

#undef LOCTEXT_NAMESPACE
