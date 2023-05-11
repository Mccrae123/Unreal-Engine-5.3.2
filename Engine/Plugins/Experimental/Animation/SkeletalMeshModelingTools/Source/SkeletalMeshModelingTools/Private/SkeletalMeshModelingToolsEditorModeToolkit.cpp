// Copyright Epic Games, Inc. All Rights Reserved.

#include "SkeletalMeshModelingToolsEditorModeToolkit.h"

#include "SkeletalMeshModelingToolsCommands.h"

#include "EdModeInteractiveToolsContext.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "IDetailsView.h"
#include "InteractiveTool.h"
#include "InteractiveToolsContext.h"
#include "ModelingToolsEditorModeStyle.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "Tools/UEdMode.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Text/STextBlock.h"
#include "ModelingToolsManagerActions.h"
#include "BaseTools/MeshSurfacePointTool.h"
#include "SPrimaryButton.h"
#include "Toolkits/AssetEditorModeUILayer.h"


#define LOCTEXT_NAMESPACE "SkeletalMeshModelingToolsEditorModeToolkit"

FSkeletalMeshModelingToolsEditorModeToolkit::~FSkeletalMeshModelingToolsEditorModeToolkit()
{
	if (UEditorInteractiveToolsContext* Context = GetScriptableEditorMode()->GetInteractiveToolsContext(EToolsContextScope::EdMode))
	{
		Context->OnToolNotificationMessage.RemoveAll(this);
		Context->OnToolWarningMessage.RemoveAll(this);
	}
}

void FSkeletalMeshModelingToolsEditorModeToolkit::Init(
	const TSharedPtr<IToolkitHost>& InToolkitHost, 
	TWeakObjectPtr<UEdMode> InOwningMode
	)
{
	// Create a details view to show the tool props
	FPropertyEditorModule& PropertyEditorModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");

	FDetailsViewArgs DetailsViewArgs;

	DetailsViewArgs.bHideSelectionTip = true;
	DetailsViewArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;
	DetailsViewArgs.bAllowSearch = false;
	DetailsViewArgs.DefaultsOnlyVisibility = EEditDefaultsOnlyNodeVisibility::Automatic;
	DetailsViewArgs.bShowOptions = false;
	DetailsViewArgs.bAllowMultipleTopLevelObjects = true;

	DetailsView = PropertyEditorModule.CreateDetailView(DetailsViewArgs);

	ModeWarningArea = SNew(STextBlock)
		.AutoWrapText(true)
		.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
		.ColorAndOpacity(FSlateColor(FLinearColor(0.9f, 0.15f, 0.15f)));
	ModeWarningArea->SetText(FText::GetEmpty());
	ModeWarningArea->SetVisibility(EVisibility::Collapsed);

	ModeHeaderArea = SNew(STextBlock)
		.AutoWrapText(true)
		.Font(FCoreStyle::GetDefaultFontStyle("Bold", 12));
	ModeHeaderArea->SetText(LOCTEXT("SelectToolLabel", "Select a Tool from the Toolbar"));
	ModeHeaderArea->SetJustification(ETextJustify::Center);


	ToolWarningArea = SNew(STextBlock)
		.AutoWrapText(true)
		.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
		.ColorAndOpacity(FSlateColor(FLinearColor(0.9f, 0.15f, 0.15f)));
	ToolWarningArea->SetText(FText::GetEmpty());


	SAssignNew(ToolkitWidget, SBox)
		.HAlign(HAlign_Fill)
		.Padding(2)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Fill).Padding(5)
			[
				ModeWarningArea->AsShared()
			]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Fill).Padding(5)
			[
				ModeHeaderArea->AsShared()
			]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Fill).Padding(5)
			[
				ToolWarningArea->AsShared()
			]
			+ SVerticalBox::Slot().FillHeight(1.f).HAlign(HAlign_Fill)
			[
				DetailsView->AsShared()
			]
		];

	FModeToolkit::Init(InToolkitHost, InOwningMode);

	ClearNotification();
	ClearWarning();

	ActiveToolName = FText::GetEmpty();
	ActiveToolMessage = FText::GetEmpty();

	if (UEditorInteractiveToolsContext* Context = GetScriptableEditorMode()->GetInteractiveToolsContext(EToolsContextScope::EdMode))
	{
		Context->OnToolNotificationMessage.AddSP(this, &FSkeletalMeshModelingToolsEditorModeToolkit::PostNotification);
		Context->OnToolWarningMessage.AddSP(this, &FSkeletalMeshModelingToolsEditorModeToolkit::PostWarning);
	}

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

			+SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(0.f, 0.f, 8.f, 0.f))
			[
				SNew(SImage)
				.Image_Lambda([this] () { return ActiveToolIcon; })
			]

			+SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(0.f, 0.f, 8.f, 0.f))
			[
				SNew(STextBlock)
				.Text(this, &FSkeletalMeshModelingToolsEditorModeToolkit::GetActiveToolDisplayName)
			]

			+SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(FMargin(0.0, 0.f, 2.f, 0.f))
			[
				SNew(SPrimaryButton)
				.Text(LOCTEXT("OverlayAccept", "Accept"))
				.ToolTipText(LOCTEXT("OverlayAcceptTooltip", "Accept/Commit the results of the active Tool [Enter]"))
				.OnClicked_Lambda([this]() { GetScriptableEditorMode()->GetInteractiveToolsContext()->EndTool(EToolShutdownType::Accept); return FReply::Handled(); })
				.IsEnabled_Lambda([this]() { return GetScriptableEditorMode()->GetInteractiveToolsContext()->CanAcceptActiveTool(); })
				.Visibility_Lambda([this]() { return GetScriptableEditorMode()->GetInteractiveToolsContext()->ActiveToolHasAccept() ? EVisibility::Visible : EVisibility::Collapsed; })
			]

			+SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(FMargin(2.0, 0.f, 0.f, 0.f))
			[
				SNew(SButton)
				.TextStyle( FAppStyle::Get(), "DialogButtonText" )
				.Text(LOCTEXT("OverlayCancel", "Cancel"))
				.ToolTipText(LOCTEXT("OverlayCancelTooltip", "Cancel the active Tool [Esc]"))
				.HAlign(HAlign_Center)
				.OnClicked_Lambda([this]() { GetScriptableEditorMode()->GetInteractiveToolsContext()->EndTool(EToolShutdownType::Cancel); return FReply::Handled(); })
				.IsEnabled_Lambda([this]() { return GetScriptableEditorMode()->GetInteractiveToolsContext()->CanCancelActiveTool(); })
				.Visibility_Lambda([this]() { return GetScriptableEditorMode()->GetInteractiveToolsContext()->ActiveToolHasAccept() ? EVisibility::Visible : EVisibility::Collapsed; })
			]

			+SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(FMargin(2.0, 0.f, 0.f, 0.f))
			[
				SNew(SPrimaryButton)
				.Text(LOCTEXT("OverlayComplete", "Complete"))
				.ToolTipText(LOCTEXT("OverlayCompleteTooltip", "Exit the active Tool [Enter]"))
				.OnClicked_Lambda([this]() { GetScriptableEditorMode()->GetInteractiveToolsContext()->EndTool(EToolShutdownType::Completed); return FReply::Handled(); })
				.IsEnabled_Lambda([this]() { return GetScriptableEditorMode()->GetInteractiveToolsContext()->CanCompleteActiveTool(); })
				.Visibility_Lambda([this]() { return GetScriptableEditorMode()->GetInteractiveToolsContext()->CanCompleteActiveTool() ? EVisibility::Visible : EVisibility::Collapsed; })
			]
		]	
	];
}


FName FSkeletalMeshModelingToolsEditorModeToolkit::GetToolkitFName() const
{
	return FName("SkeletalMeshModelingToolsEditorModeToolkit");
}


FText FSkeletalMeshModelingToolsEditorModeToolkit::GetBaseToolkitName() const
{
	return LOCTEXT("DisplayName", "Skeletal Mesh Modeling Tools");
}


void FSkeletalMeshModelingToolsEditorModeToolkit::OnToolStarted(UInteractiveToolManager* Manager, UInteractiveTool* Tool)
{
	UpdateActiveToolProperties(Tool);

	Tool->OnPropertySetsModified.AddSP(this, &FSkeletalMeshModelingToolsEditorModeToolkit::UpdateActiveToolProperties, Tool);

	ModeHeaderArea->SetVisibility(EVisibility::Collapsed);
	ActiveToolName = Tool->GetToolInfo().ToolDisplayName;

	FString ActiveToolIdentifier = GetScriptableEditorMode()->GetToolManager()->GetActiveToolName(EToolSide::Left);
	ActiveToolIdentifier.InsertAt(0, ".");
	FName ActiveToolIconName = ISlateStyle::Join(FModelingToolsManagerCommands::Get().GetContextName(), TCHAR_TO_ANSI(*ActiveToolIdentifier));
	ActiveToolIcon = FModelingToolsEditorModeStyle::Get()->GetOptionalBrush(ActiveToolIconName);
	
	GetToolkitHost()->AddViewportOverlayWidget(ViewportOverlayWidget.ToSharedRef());
}


void FSkeletalMeshModelingToolsEditorModeToolkit::OnToolEnded(UInteractiveToolManager* Manager, UInteractiveTool* Tool)
{
	if (IsHosted())
	{
		GetToolkitHost()->RemoveViewportOverlayWidget(ViewportOverlayWidget.ToSharedRef());
	}
	
	if (Tool)
	{
		Tool->OnPropertySetsModified.RemoveAll(this);
	}

	ModeHeaderArea->SetVisibility(EVisibility::Visible);
	DetailsView->SetObject(nullptr);
	ActiveToolName = FText::GetEmpty();
	ClearNotification();
	ClearWarning();

}


FText FSkeletalMeshModelingToolsEditorModeToolkit::GetActiveToolDisplayName() const
{
	return ActiveToolName;
}


FText FSkeletalMeshModelingToolsEditorModeToolkit::GetActiveToolMessage() const
{
	return ActiveToolMessage;
}

static const FName EditTabName(TEXT("Edit Mesh"));
static const FName ProcessingTabName(TEXT("MeshOps"));
static const FName DeformTabName(TEXT("Deform"));
static const FName SkinWeightsTabName(TEXT("Skin"));
static const FName SkeletonTabName(TEXT("Skeleton"));


void FSkeletalMeshModelingToolsEditorModeToolkit::GetToolPaletteNames(TArray<FName>& InPaletteName) const
{
	InPaletteName = { EditTabName, ProcessingTabName, DeformTabName, SkinWeightsTabName, SkeletonTabName };
}


FText FSkeletalMeshModelingToolsEditorModeToolkit::GetToolPaletteDisplayName(FName PaletteName) const
{
	return FText::FromName(PaletteName);
}


void FSkeletalMeshModelingToolsEditorModeToolkit::BuildToolPalette(FName PaletteName, class FToolBarBuilder& ToolbarBuilder)
{
	const FModelingToolsManagerCommands& Commands = FModelingToolsManagerCommands::Get();

	if (PaletteName == EditTabName)
	{
		ToolbarBuilder.AddToolBarButton(Commands.BeginPolyEditTool);
		ToolbarBuilder.AddToolBarButton(Commands.BeginPolyDeformTool);
		ToolbarBuilder.AddToolBarButton(Commands.BeginHoleFillTool);
		ToolbarBuilder.AddToolBarButton(Commands.BeginPolygonCutTool);
	}
	else if (PaletteName == ProcessingTabName)
	{
		ToolbarBuilder.AddToolBarButton(Commands.BeginSimplifyMeshTool);
		ToolbarBuilder.AddToolBarButton(Commands.BeginRemeshMeshTool);
		ToolbarBuilder.AddToolBarButton(Commands.BeginWeldEdgesTool);
		ToolbarBuilder.AddToolBarButton(Commands.BeginRemoveOccludedTrianglesTool);
		ToolbarBuilder.AddToolBarButton(Commands.BeginProjectToTargetTool);
	}
	else if (PaletteName == DeformTabName)
	{
		ToolbarBuilder.AddToolBarButton(Commands.BeginSculptMeshTool);
		ToolbarBuilder.AddToolBarButton(Commands.BeginRemeshSculptMeshTool);
		ToolbarBuilder.AddToolBarButton(Commands.BeginSmoothMeshTool);
		ToolbarBuilder.AddToolBarButton(Commands.BeginOffsetMeshTool);
		ToolbarBuilder.AddToolBarButton(Commands.BeginMeshSpaceDeformerTool);
		ToolbarBuilder.AddToolBarButton(Commands.BeginLatticeDeformerTool);
		ToolbarBuilder.AddToolBarButton(Commands.BeginDisplaceMeshTool);
	}
	else if (PaletteName == SkinWeightsTabName)
	{
		ToolbarBuilder.AddToolBarButton(Commands.BeginAttributeEditorTool);
		ToolbarBuilder.AddToolBarButton(Commands.BeginMeshAttributePaintTool);
		ToolbarBuilder.AddToolBarButton(Commands.BeginSkinWeightsPaintTool);
		ToolbarBuilder.AddToolBarButton(Commands.BeginSkinWeightsBindingTool);
	}
	else if (PaletteName == SkeletonTabName)
	{
		ToolbarBuilder.AddToolBarButton(Commands.BeginSkeletonEditingTool);
	}
}


void FSkeletalMeshModelingToolsEditorModeToolkit::OnToolPaletteChanged(FName PaletteName)
{
}


void FSkeletalMeshModelingToolsEditorModeToolkit::PostNotification(const FText& InMessage)
{
	ClearNotification();
	
	ActiveToolMessage = InMessage;

	if (ModeUILayer.IsValid())
	{
		const FName StatusBarName = ModeUILayer.Pin()->GetStatusBarName();
		ActiveToolMessageHandle = GEditor->GetEditorSubsystem<UStatusBarSubsystem>()->PushStatusBarMessage(StatusBarName, ActiveToolMessage);
	}
}


void FSkeletalMeshModelingToolsEditorModeToolkit::ClearNotification()
{
	ActiveToolMessage = FText::GetEmpty();

	if (ModeUILayer.IsValid())
	{
		const FName StatusBarName = ModeUILayer.Pin()->GetStatusBarName();
		GEditor->GetEditorSubsystem<UStatusBarSubsystem>()->PopStatusBarMessage(StatusBarName, ActiveToolMessageHandle);
	}
	ActiveToolMessageHandle.Reset();
}


void FSkeletalMeshModelingToolsEditorModeToolkit::PostWarning(const FText& Message)
{
	ToolWarningArea->SetText(Message);
	ToolWarningArea->SetVisibility(EVisibility::Visible);
}


void FSkeletalMeshModelingToolsEditorModeToolkit::ClearWarning()
{
	ToolWarningArea->SetText(FText());
	ToolWarningArea->SetVisibility(EVisibility::Collapsed);
}


void FSkeletalMeshModelingToolsEditorModeToolkit::UpdateActiveToolProperties(UInteractiveTool* Tool)
{
	if (Tool)
	{
		DetailsView->SetObjects(Tool->GetToolProperties(true));
	}
}


#undef LOCTEXT_NAMESPACE
