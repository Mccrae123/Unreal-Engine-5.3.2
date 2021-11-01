// Copyright Epic Games, Inc. All Rights Reserved.

#include "UVEditorModeToolkit.h"

#include "EditorStyleSet.h" //FEditorStyle
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Framework/MultiBox/MultiBoxExtender.h"
#include "Framework/MultiBox/MultiBoxDefs.h"
#include "IDetailsView.h"
#include "Modules/ModuleManager.h"
#include "SPrimaryButton.h"
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
			return !(Mode && Mode->GetInteractiveToolsContext() 
				&& Mode->GetInteractiveToolsContext()->ToolManager
				&& Mode->GetInteractiveToolsContext()->ToolManager->HasAnyActiveTool() 
				&& !Mode->IsDefaultToolActive());
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
	UEditorInteractiveToolsContext* InteractiveToolsContext = UVEditorMode->GetInteractiveToolsContext();
	check(InteractiveToolsContext);

	// Currently, there's no EToolChangeTrackingMode that reverts back to a default tool on undo (if we add that
	// support, the tool manager will need to be aware of the default tool). So, we instead opt to do our own management
	// of tool start transactions. See FUVEditorModeToolkit::OnToolStarted for how we issue the transactions.
	InteractiveToolsContext->ToolManager->ConfigureChangeTrackingMode(EToolChangeTrackingMode::NoChangeTracking);

	// Build the tool palette
	const FUVEditorCommands& CommandInfos = FUVEditorCommands::Get();
	TSharedRef<FUICommandList> CommandList = GetToolkitCommands();
	FUniformToolBarBuilder ToolbarBuilder(CommandList, FMultiBoxCustomization(UVEditorMode->GetModeInfo().ToolbarCustomizationName), TSharedPtr<FExtender>(), false);
	ToolbarBuilder.SetStyle(&FEditorStyle::Get(), "PaletteToolBar");

	// TODO: Add more tools here
	ToolbarBuilder.AddToolBarButton(CommandInfos.BeginSelectTool);
	ToolbarBuilder.AddToolBarButton(CommandInfos.BeginLayoutTool);
	ToolbarBuilder.AddToolBarButton(CommandInfos.BeginParameterizeMeshTool);
	ToolbarBuilder.AddToolBarButton(CommandInfos.BeginChannelEditTool);
	ToolbarBuilder.AddToolBarButton(CommandInfos.BeginSeamTool);
	ToolbarBuilder.AddToolBarButton(CommandInfos.BeginRecomputeUVsTool);

	// Hook in the tool palette
	ToolButtonsContainer->SetContent(ToolbarBuilder.MakeWidget());

	// Hook up the tool detail panel
	ToolDetailsContainer->SetContent(DetailsView.ToSharedRef());
	
	// Set up the overlay. Largely copied from ModelingToolsEditorModeToolkit.
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
				SNew(SPrimaryButton)
				.Text(LOCTEXT("OverlayAccept", "Accept"))
				.ToolTipText(LOCTEXT("OverlayAcceptTooltip", "Accept/Commit the results of the active Tool [Enter]"))
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

			+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(FMargin(2.0, 0.f, 0.f, 0.f))
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
				.TextStyle(FAppStyle::Get(), "DialogButtonText")
				.Text(LOCTEXT("OverlayComplete", "Complete"))
				.ToolTipText(LOCTEXT("OverlayCompleteTooltip", "Exit the active Tool [Enter]"))
				.HAlign(HAlign_Center)
				.OnClicked_Lambda([this]() { 
					GetScriptableEditorMode()->GetInteractiveToolsContext()->EndTool(EToolShutdownType::Completed); 
					Cast<UUVEditorMode>(GetScriptableEditorMode())->ActivateDefaultTool();
					return FReply::Handled(); 
					})
				.IsEnabled_Lambda([this]() {
					UUVEditorMode* Mode = Cast<UUVEditorMode>(GetScriptableEditorMode());
					return GetScriptableEditorMode()->GetInteractiveToolsContext()->CanCompleteActiveTool();
				})
				.Visibility_Lambda([this]() { return GetScriptableEditorMode()->GetInteractiveToolsContext()->CanCompleteActiveTool() ? EVisibility::Visible : EVisibility::Collapsed; })
			]
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

TSharedRef<SWidget> FUVEditorModeToolkit::CreateChannelMenu()
{
	UUVEditorMode* Mode = Cast<UUVEditorMode>(GetScriptableEditorMode());

	const bool bShouldCloseWindowAfterMenuSelection = true;
	FMenuBuilder MenuBuilder(bShouldCloseWindowAfterMenuSelection, nullptr);

	// For each asset, create a submenu labeled with its name
	const TArray<FString>& AssetNames = Mode->GetAssetNames();
	for (int32 AssetID = 0; AssetID < AssetNames.Num(); ++AssetID)
	{
		MenuBuilder.AddSubMenu(
			FText::AsCultureInvariant(AssetNames[AssetID]), // label
			FText(), // tooltip
			FNewMenuDelegate::CreateWeakLambda(Mode, [this, Mode, AssetID](FMenuBuilder& SubMenuBuilder)
		{

			// Inside each submenu, create a button for each channel
			int32 NumChannels = Mode->GetNumUVChannels(AssetID);
			for (int32 Channel = 0; Channel < NumChannels; ++Channel)
			{
				SubMenuBuilder.AddMenuEntry(
					FText::Format(LOCTEXT("ChannelLabel", "UV Channel {0}"), Channel), // label
					FText(), // tooltip
					FSlateIcon(),
					FUIAction(
						FExecuteAction::CreateWeakLambda(Mode, [Mode, AssetID, Channel]()
						{
							Mode->RequestUVChannelChange(AssetID, Channel);

							// A bit of a hack to force the menu to close if the checkbox is clicked (which usually doesn't
							// close the menu)
							FSlateApplication::Get().DismissAllMenus();
						}), 
						FCanExecuteAction::CreateWeakLambda(Mode, []() { return true; }),
						FIsActionChecked::CreateWeakLambda(Mode, [Mode, AssetID, Channel]()
						{
							return Mode->GetDisplayedChannel(AssetID) == Channel;
						})),
					NAME_None,
					EUserInterfaceActionType::RadioButton);
			}
		}));
	}

	return MenuBuilder.MakeWidget();
}

TSharedRef<SWidget> FUVEditorModeToolkit::CreateBackgroundSettingsWidget()
{
	TSharedRef<SBorder> BackgroundDetailsContainer =
		SNew(SBorder)
		.BorderImage(FEditorStyle::GetBrush("NoBorder"));

	TSharedRef<SWidget> Widget = SNew(SBorder)
		.HAlign(HAlign_Fill)
		.Padding(4)
		[
			SNew(SBox)			
				.MinDesiredWidth(500)				
			[
				BackgroundDetailsContainer				
			]
		];

	FPropertyEditorModule& PropertyEditorModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");

	FDetailsViewArgs BackgroundDetailsViewArgs;
	BackgroundDetailsViewArgs.bAllowSearch = false;
	BackgroundDetailsViewArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;
	BackgroundDetailsViewArgs.bHideSelectionTip = true;
	BackgroundDetailsViewArgs.DefaultsOnlyVisibility = EEditDefaultsOnlyNodeVisibility::Automatic;
	BackgroundDetailsViewArgs.bShowOptions = false;
	BackgroundDetailsViewArgs.bAllowMultipleTopLevelObjects = false;

	UUVEditorMode* Mode = Cast<UUVEditorMode>(GetScriptableEditorMode());
	TSharedRef<IDetailsView> BackgroundDetailsView = PropertyEditorModule.CreateDetailView(BackgroundDetailsViewArgs);
	BackgroundDetailsView->SetObject(Mode->GetBackgroundSettingsObject());
	BackgroundDetailsContainer->SetContent(BackgroundDetailsView);

	return Widget;
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

	UUVEditorMode* Mode = Cast<UUVEditorMode>(GetScriptableEditorMode());
	if (!Mode->IsDefaultToolActive())
	{
		// Issue a tool start transaction unless we are starting the default tool, because we can't
		// undo or revert out of the default tool.
		Mode->GetInteractiveToolsContext()->GetTransactionAPI()->AppendChange(
			Mode, MakeUnique<UVEditorModeToolkitLocals::FUVEditorBeginToolChange>(), LOCTEXT("ActivateTool", "Activate Tool"));

		// Add the accept/cancel overlay. Again, unless we're in the default tool, which we don't leave
		// except by activating another tool.
		GetToolkitHost()->AddViewportOverlayWidget(ViewportOverlayWidget.ToSharedRef());
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
