// Copyright Epic Games, Inc. All Rights Reserved.

#include "Views/Viewport/DisplayClusterConfiguratorSCSEditorViewport.h"
#include "Views/Viewport/DisplayClusterConfiguratorSCSEditorViewportClient.h"
#include "DisplayClusterConfiguratorCommands.h"

#include "BlueprintEditor.h"
#include "SSCSEditor.h"
#include "EditorViewportCommands.h"
#include "STransformViewportToolbar.h"
#include "SEditorViewportToolBarMenu.h"
#include "SViewportToolBar.h"
#include "AssetEditorViewportLayout.h"
#include "ViewportTabContent.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Slate/SceneViewport.h"

#define LOCTEXT_NAMESPACE "DisplayClusterSCSEditorViewport"

class SDisplayClusterConfiguratorSCSEditorViewportToolBar : public SViewportToolBar
{
public:
	SLATE_BEGIN_ARGS(SDisplayClusterConfiguratorSCSEditorViewportToolBar){}
		SLATE_ARGUMENT(TWeakPtr<SEditorViewport>, EditorViewport)
	SLATE_END_ARGS()

	/** Constructs this widget with the given parameters */
	void Construct(const FArguments& InArgs)
	{
		EditorViewport = InArgs._EditorViewport;

		static const FName DefaultForegroundName("DefaultForeground");

		this->ChildSlot
		[
			SNew(SBorder)
			.BorderImage(FEditorStyle::GetBrush("NoBorder"))
			.ColorAndOpacity(this, &SViewportToolBar::OnGetColorAndOpacity)
			.ForegroundColor(FEditorStyle::GetSlateColor(DefaultForegroundName))
			[
				SNew(SHorizontalBox)
				+SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(2.0f, 2.0f)
				[
					SNew(SEditorViewportToolbarMenu)
					.ParentToolBar(SharedThis(this))
					.Cursor(EMouseCursor::Default)
					.Image("EditorViewportToolBar.MenuDropdown")
					.OnGetMenuContent(this, &SDisplayClusterConfiguratorSCSEditorViewportToolBar::GeneratePreviewMenu)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(2.0f, 2.0f)
				[
					SNew( SEditorViewportToolbarMenu )
					.ParentToolBar( SharedThis( this ) )
					.Cursor( EMouseCursor::Default )
					.Label(this, &SDisplayClusterConfiguratorSCSEditorViewportToolBar::GetCameraMenuLabel)
					.LabelIcon(this, &SDisplayClusterConfiguratorSCSEditorViewportToolBar::GetCameraMenuLabelIcon)
					.OnGetMenuContent(this, &SDisplayClusterConfiguratorSCSEditorViewportToolBar::GenerateCameraMenu)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(2.0f, 2.0f)
				[
					SNew( SEditorViewportToolbarMenu )
					.ParentToolBar( SharedThis( this ) )
					.Cursor( EMouseCursor::Default )
					.Label(this, &SDisplayClusterConfiguratorSCSEditorViewportToolBar::GetViewMenuLabel)
					.LabelIcon(this, &SDisplayClusterConfiguratorSCSEditorViewportToolBar::GetViewMenuLabelIcon)
					.OnGetMenuContent(this, &SDisplayClusterConfiguratorSCSEditorViewportToolBar::GenerateViewMenu)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(2.0f, 2.0f)
				[
					SNew( SEditorViewportToolbarMenu )
					.ParentToolBar( SharedThis( this ) )
					.Cursor( EMouseCursor::Default )
					.Label(this, &SDisplayClusterConfiguratorSCSEditorViewportToolBar::GetViewportsMenuLabel)
					.OnGetMenuContent(this, &SDisplayClusterConfiguratorSCSEditorViewportToolBar::GenerateViewportsMenu)
				]
				+ SHorizontalBox::Slot()
				.Padding( 3.0f, 1.0f )
				.HAlign( HAlign_Right )
				[
					SNew(STransformViewportToolBar)
					.Viewport(EditorViewport.Pin().ToSharedRef())
					.CommandList(EditorViewport.Pin()->GetCommandList())
				]
			]
		];

		SViewportToolBar::Construct(SViewportToolBar::FArguments());
	}

	/** Creates the preview menu */
	TSharedRef<SWidget> GeneratePreviewMenu() const
	{
		TSharedPtr<const FUICommandList> CommandList = EditorViewport.IsValid()? EditorViewport.Pin()->GetCommandList(): nullptr;

		const bool bInShouldCloseWindowAfterMenuSelection = true;

		FMenuBuilder PreviewOptionsMenuBuilder(bInShouldCloseWindowAfterMenuSelection, CommandList);
		{
			PreviewOptionsMenuBuilder.BeginSection("BlueprintEditorPreviewOptions", NSLOCTEXT("BlueprintEditor", "PreviewOptionsMenuHeader", "Preview Viewport Options"));
			{
				PreviewOptionsMenuBuilder.AddMenuEntry(FDisplayClusterConfiguratorCommands::Get().ResetCamera);
				PreviewOptionsMenuBuilder.AddMenuEntry(FDisplayClusterConfiguratorCommands::Get().ShowFloor);
				PreviewOptionsMenuBuilder.AddMenuEntry(FDisplayClusterConfiguratorCommands::Get().ShowGrid);
				PreviewOptionsMenuBuilder.AddMenuEntry(FDisplayClusterConfiguratorCommands::Get().ShowOrigin);
				{
					PreviewOptionsMenuBuilder.AddSubMenu(
						LOCTEXT("nDisplayConfigLayout", "Layouts"),
						LOCTEXT("nDisplayConfigsSubMenu", "Layouts"), 
						FNewMenuDelegate::CreateSP(this, &SDisplayClusterConfiguratorSCSEditorViewportToolBar::GenerateViewportConfigsMenu));
				}
			}
			PreviewOptionsMenuBuilder.EndSection();
		}

		return PreviewOptionsMenuBuilder.MakeWidget();
	}

	FText GetCameraMenuLabel() const
	{
		if(EditorViewport.IsValid())
		{
			return GetCameraMenuLabelFromViewportType(EditorViewport.Pin()->GetViewportClient()->GetViewportType());
		}

		return NSLOCTEXT("BlueprintEditor", "CameraMenuTitle_Default", "Camera");
	}

	const FSlateBrush* GetCameraMenuLabelIcon() const
	{
		if(EditorViewport.IsValid())
		{
			return GetCameraMenuLabelIconFromViewportType( EditorViewport.Pin()->GetViewportClient()->GetViewportType() );
		}

		return FEditorStyle::GetBrush(NAME_None);
	}

	TSharedRef<SWidget> GenerateCameraMenu() const
	{
		TSharedPtr<const FUICommandList> CommandList = EditorViewport.IsValid()? EditorViewport.Pin()->GetCommandList(): nullptr;

		const bool bInShouldCloseWindowAfterMenuSelection = true;
		FMenuBuilder CameraMenuBuilder(bInShouldCloseWindowAfterMenuSelection, CommandList);

		CameraMenuBuilder.AddMenuEntry(FEditorViewportCommands::Get().Perspective);

		CameraMenuBuilder.BeginSection("LevelViewportCameraType_Ortho", NSLOCTEXT("BlueprintEditor", "CameraTypeHeader_Ortho", "Orthographic"));
			CameraMenuBuilder.AddMenuEntry(FEditorViewportCommands::Get().Top);
			CameraMenuBuilder.AddMenuEntry(FEditorViewportCommands::Get().Bottom);
			CameraMenuBuilder.AddMenuEntry(FEditorViewportCommands::Get().Left);
			CameraMenuBuilder.AddMenuEntry(FEditorViewportCommands::Get().Right);
			CameraMenuBuilder.AddMenuEntry(FEditorViewportCommands::Get().Front);
			CameraMenuBuilder.AddMenuEntry(FEditorViewportCommands::Get().Back);
		CameraMenuBuilder.EndSection();

		return CameraMenuBuilder.MakeWidget();
	}

	FText GetViewMenuLabel() const
	{
		FText Label = NSLOCTEXT("BlueprintEditor", "ViewMenuTitle_Default", "View");

		if (EditorViewport.IsValid())
		{
			switch (EditorViewport.Pin()->GetViewportClient()->GetViewMode())
			{
			case VMI_Lit:
				Label = NSLOCTEXT("BlueprintEditor", "ViewMenuTitle_Lit", "Lit");
				break;

			case VMI_Unlit:
				Label = NSLOCTEXT("BlueprintEditor", "ViewMenuTitle_Unlit", "Unlit");
				break;

			case VMI_BrushWireframe:
				Label = NSLOCTEXT("BlueprintEditor", "ViewMenuTitle_Wireframe", "Wireframe");
				break;
			}
		}

		return Label;
	}

	const FSlateBrush* GetViewMenuLabelIcon() const
	{
		static FName LitModeIconName("EditorViewport.LitMode");
		static FName UnlitModeIconName("EditorViewport.UnlitMode");
		static FName WireframeModeIconName("EditorViewport.WireframeMode");

		FName Icon = NAME_None;

		if (EditorViewport.IsValid())
		{
			switch (EditorViewport.Pin()->GetViewportClient()->GetViewMode())
			{
			case VMI_Lit:
				Icon = LitModeIconName;
				break;

			case VMI_Unlit:
				Icon = UnlitModeIconName;
				break;

			case VMI_BrushWireframe:
				Icon = WireframeModeIconName;
				break;
			}
		}

		return FEditorStyle::GetBrush(Icon);
	}

	TSharedRef<SWidget> GenerateViewMenu() const
	{
		TSharedPtr<const FUICommandList> CommandList = EditorViewport.IsValid() ? EditorViewport.Pin()->GetCommandList() : nullptr;

		const bool bInShouldCloseWindowAfterMenuSelection = true;
		FMenuBuilder ViewMenuBuilder(bInShouldCloseWindowAfterMenuSelection, CommandList);

		ViewMenuBuilder.AddMenuEntry(FEditorViewportCommands::Get().LitMode, NAME_None, NSLOCTEXT("BlueprintEditor", "LitModeMenuOption", "Lit"));
		ViewMenuBuilder.AddMenuEntry(FEditorViewportCommands::Get().UnlitMode, NAME_None, NSLOCTEXT("BlueprintEditor", "UnlitModeMenuOption", "Unlit"));
		ViewMenuBuilder.AddMenuEntry(FEditorViewportCommands::Get().WireframeMode, NAME_None, NSLOCTEXT("BlueprintEditor", "WireframeModeMenuOption", "Wireframe"));

		return ViewMenuBuilder.MakeWidget();
	}

	FText GetViewportsMenuLabel() const
	{
		FText Label = NSLOCTEXT("BlueprintEditor", "ViewportsMenuTitle_Default", "Viewports");
		return Label;
	}

	TSharedRef<SWidget> GenerateViewportsMenu() const
	{
		const TSharedPtr<const FUICommandList> CommandList = EditorViewport.IsValid() ? EditorViewport.Pin()->GetCommandList() : nullptr;

		const bool bInShouldCloseWindowAfterMenuSelection = true;
		FMenuBuilder ViewportsMenuBuilder(bInShouldCloseWindowAfterMenuSelection, CommandList);

		ViewportsMenuBuilder.AddMenuEntry(FDisplayClusterConfiguratorCommands::Get().ShowPreview);
		ViewportsMenuBuilder.AddMenuEntry(FDisplayClusterConfiguratorCommands::Get().Show3DViewportNames);
		return ViewportsMenuBuilder.MakeWidget();
	}

	
	void GenerateViewportConfigsMenu(FMenuBuilder& MenuBuilder) const
	{
		check(EditorViewport.IsValid());
		TSharedPtr<const FUICommandList> CommandList = EditorViewport.Pin()->GetCommandList();

		MenuBuilder.BeginSection("nDisplayEditorViewportOnePaneConfigs", LOCTEXT("OnePaneConfigHeader", "One Pane"));
		{
			FToolBarBuilder OnePaneButton(CommandList, FMultiBoxCustomization::None);
			OnePaneButton.SetLabelVisibility(EVisibility::Collapsed);
			OnePaneButton.SetStyle(&FEditorStyle::Get(), "ViewportLayoutToolbar");

			OnePaneButton.AddToolBarButton(FEditorViewportCommands::Get().ViewportConfig_OnePane);

			MenuBuilder.AddWidget(
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					OnePaneButton.MakeWidget()
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1)
				[
					SNullWidget::NullWidget
				],
				FText::GetEmpty(), true
				);
		}
		MenuBuilder.EndSection();

		MenuBuilder.BeginSection("nDisplayEditorViewportTwoPaneConfigs", LOCTEXT("TwoPaneConfigHeader", "Two Panes"));
		{
			FToolBarBuilder TwoPaneButtons(CommandList, FMultiBoxCustomization::None);
			TwoPaneButtons.SetLabelVisibility(EVisibility::Collapsed);
			TwoPaneButtons.SetStyle(&FEditorStyle::Get(), "ViewportLayoutToolbar");

			TwoPaneButtons.AddToolBarButton(FEditorViewportCommands::Get().ViewportConfig_TwoPanesH, NAME_None, FText());
			TwoPaneButtons.AddToolBarButton(FEditorViewportCommands::Get().ViewportConfig_TwoPanesV, NAME_None, FText());

			MenuBuilder.AddWidget(
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					TwoPaneButtons.MakeWidget()
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1)
				[
					SNullWidget::NullWidget
				],
				FText::GetEmpty(), true
				);
		}
		MenuBuilder.EndSection();

		MenuBuilder.BeginSection("nDisplayEditorViewportThreePaneConfigs", LOCTEXT("ThreePaneConfigHeader", "Three Panes"));
		{
			FToolBarBuilder ThreePaneButtons(CommandList, FMultiBoxCustomization::None);
			ThreePaneButtons.SetLabelVisibility(EVisibility::Collapsed);
			ThreePaneButtons.SetStyle(&FEditorStyle::Get(), "ViewportLayoutToolbar");

			ThreePaneButtons.AddToolBarButton(FEditorViewportCommands::Get().ViewportConfig_ThreePanesLeft, NAME_None, FText());
			ThreePaneButtons.AddToolBarButton(FEditorViewportCommands::Get().ViewportConfig_ThreePanesRight, NAME_None, FText());
			ThreePaneButtons.AddToolBarButton(FEditorViewportCommands::Get().ViewportConfig_ThreePanesTop, NAME_None, FText());
			ThreePaneButtons.AddToolBarButton(FEditorViewportCommands::Get().ViewportConfig_ThreePanesBottom, NAME_None, FText());

			MenuBuilder.AddWidget(
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					ThreePaneButtons.MakeWidget()
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1)
				[
					SNullWidget::NullWidget
				],
				FText::GetEmpty(), true
				);
		}
		MenuBuilder.EndSection();

		MenuBuilder.BeginSection("nDisplayEditorViewportFourPaneConfigs", LOCTEXT("FourPaneConfigHeader", "Four Panes"));
		{
			FToolBarBuilder FourPaneButtons(CommandList, FMultiBoxCustomization::None);
			FourPaneButtons.SetLabelVisibility(EVisibility::Collapsed);
			FourPaneButtons.SetStyle(&FEditorStyle::Get(), "ViewportLayoutToolbar");

			FourPaneButtons.AddToolBarButton(FEditorViewportCommands::Get().ViewportConfig_FourPanes2x2, NAME_None, FText());
			FourPaneButtons.AddToolBarButton(FEditorViewportCommands::Get().ViewportConfig_FourPanesLeft, NAME_None, FText());
			FourPaneButtons.AddToolBarButton(FEditorViewportCommands::Get().ViewportConfig_FourPanesRight, NAME_None, FText());
			FourPaneButtons.AddToolBarButton(FEditorViewportCommands::Get().ViewportConfig_FourPanesTop, NAME_None, FText());
			FourPaneButtons.AddToolBarButton(FEditorViewportCommands::Get().ViewportConfig_FourPanesBottom, NAME_None, FText());

			MenuBuilder.AddWidget(
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					FourPaneButtons.MakeWidget()
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1)
				[
					SNullWidget::NullWidget
				],
				FText::GetEmpty(), true
				);
		}
		MenuBuilder.EndSection();
	}

private:
	/** Reference to the parent viewport */
	TWeakPtr<SEditorViewport> EditorViewport;
};

void SDisplayClusterConfiguratorSCSEditorViewport::Construct(const FArguments& InArgs)
{
	bIsActiveTimerRegistered = false;

	// Save off the Blueprint editor reference, we'll need this later
	BlueprintEditorPtr = InArgs._BlueprintEditor;
	OwnerTab = InArgs._OwningTab;
	
	SAssetEditorViewport::Construct(SAssetEditorViewport::FArguments());

	// Restore last used feature level
	if (ViewportClient.IsValid())
	{
		UWorld* World = ViewportClient->GetPreviewScene()->GetWorld();
		if (World != nullptr)
		{
			World->ChangeFeatureLevel(GWorld->FeatureLevel);
		}
	}

	// Use a delegate to inform the attached world of feature level changes.
	UEditorEngine* Editor = (UEditorEngine*)GEngine;
	PreviewFeatureLevelChangedHandle = Editor->OnPreviewFeatureLevelChanged().AddLambda([this](ERHIFeatureLevel::Type NewFeatureLevel)
	{
		if (ViewportClient.IsValid())
		{
			UWorld* World = ViewportClient->GetPreviewScene()->GetWorld();
			if (World != nullptr)
			{
				World->ChangeFeatureLevel(NewFeatureLevel);

				// Refresh the preview scene. Don't change the camera.
				RequestRefresh(false);
			}
		}
	});

	// Refresh the preview scene
	RequestRefresh(true);
}

SDisplayClusterConfiguratorSCSEditorViewport::~SDisplayClusterConfiguratorSCSEditorViewport()
{
	UEditorEngine* Editor = (UEditorEngine*)GEngine;
	Editor->OnPreviewFeatureLevelChanged().Remove(PreviewFeatureLevelChangedHandle);

	if (ViewportClient.IsValid())
	{
		// Reset this to ensure it's no longer in use after destruction
		ViewportClient->Viewport = nullptr;
	}
	OwnerTab.Reset();
}

void SDisplayClusterConfiguratorSCSEditorViewport::Invalidate()
{
	ViewportClient->Invalidate();
}

void SDisplayClusterConfiguratorSCSEditorViewport::RequestRefresh(bool bResetCamera, bool bRefreshNow)
{
	if (bRefreshNow)
	{
		if (ViewportClient.IsValid())
		{
			ViewportClient->InvalidatePreview(bResetCamera);
		}
	}
	else
	{
		// Defer the update until the next tick. This way we don't accidentally spawn the preview actor in the middle of a transaction, for example.
		if (!bIsActiveTimerRegistered)
		{
			bIsActiveTimerRegistered = true;
			RegisterActiveTimer(0.f, FWidgetActiveTimerDelegate::CreateSP(this, &SDisplayClusterConfiguratorSCSEditorViewport::DeferredUpdatePreview, bResetCamera));
		}
	}
}

void SDisplayClusterConfiguratorSCSEditorViewport::SetOwnerTab(TSharedRef<SDockTab> Tab)
{
	OwnerTab = Tab;
}

TSharedPtr<SDockTab> SDisplayClusterConfiguratorSCSEditorViewport::GetOwnerTab() const
{
	return OwnerTab.Pin();
}

void SDisplayClusterConfiguratorSCSEditorViewport::OnComponentSelectionChanged()
{
	// When the component selection changes, make sure to invalidate hit proxies to sync with the current selection
	SceneViewport->Invalidate();
}

TSharedRef<FEditorViewportClient> SDisplayClusterConfiguratorSCSEditorViewport::MakeEditorViewportClient()
{
	FPreviewScene* PreviewScene = BlueprintEditorPtr.Pin()->GetPreviewScene();

	// Construct a new viewport client instance.
	ViewportClient = MakeShareable(new FDisplayClusterConfiguratorSCSEditorViewportClient(BlueprintEditorPtr, PreviewScene, SharedThis(this)));
	ViewportClient->SetRealtime(true);
	ViewportClient->bSetListenerPosition = false;
	ViewportClient->VisibilityDelegate.BindSP(this, &SDisplayClusterConfiguratorSCSEditorViewport::IsVisible);

	return ViewportClient.ToSharedRef();
}

TSharedPtr<SWidget> SDisplayClusterConfiguratorSCSEditorViewport::MakeViewportToolbar()
{
	return
		SNew(SDisplayClusterConfiguratorSCSEditorViewportToolBar)
		.EditorViewport(SharedThis(this))
		.IsEnabled(FSlateApplication::Get().GetNormalExecutionAttribute());
}

void SDisplayClusterConfiguratorSCSEditorViewport::PopulateViewportOverlays(TSharedRef<SOverlay> Overlay)
{
	SEditorViewport::PopulateViewportOverlays(Overlay);

	// add the feature level display widget
	Overlay->AddSlot()
		.VAlign(VAlign_Bottom)
		.HAlign(HAlign_Right)
		.Padding(5.0f)
		[
			BuildFeatureLevelWidget()
		];
}

void SDisplayClusterConfiguratorSCSEditorViewport::BindCommands()
{
	TSharedPtr<FBlueprintEditor> BlueprintEditor = BlueprintEditorPtr.Pin();
	CommandList->Append(BlueprintEditor->GetSCSEditor()->CommandList.ToSharedRef());
	CommandList->Append(BlueprintEditor->GetToolkitCommands());
	
	const FDisplayClusterConfiguratorCommands& Commands = FDisplayClusterConfiguratorCommands::Get();

	// Toggle camera lock on/off
	CommandList->MapAction(
		Commands.ResetCamera,
		FExecuteAction::CreateSP(ViewportClient.Get(), &FDisplayClusterConfiguratorSCSEditorViewportClient::ResetCamera));

	CommandList->MapAction(
		Commands.ShowFloor,
		FExecuteAction::CreateSP(ViewportClient.Get(), &FDisplayClusterConfiguratorSCSEditorViewportClient::ToggleShowFloor),
		FCanExecuteAction(),
		FIsActionChecked::CreateSP(ViewportClient.Get(), &FDisplayClusterConfiguratorSCSEditorViewportClient::GetShowFloor));

	CommandList->MapAction(
		Commands.ShowGrid,
		FExecuteAction::CreateSP(ViewportClient.Get(), &FDisplayClusterConfiguratorSCSEditorViewportClient::ToggleShowGrid),
		FCanExecuteAction(),
		FIsActionChecked::CreateSP(ViewportClient.Get(), &FDisplayClusterConfiguratorSCSEditorViewportClient::GetShowGrid));
	
	CommandList->MapAction(
		Commands.ShowOrigin,
		FExecuteAction::CreateSP(ViewportClient.Get(), &FDisplayClusterConfiguratorSCSEditorViewportClient::ToggleShowOrigin),
		FCanExecuteAction(),
		FIsActionChecked::CreateSP(ViewportClient.Get(), &FDisplayClusterConfiguratorSCSEditorViewportClient::GetShowOrigin));

	CommandList->MapAction(
		Commands.ShowPreview,
		FExecuteAction::CreateSP(ViewportClient.Get(), &FDisplayClusterConfiguratorSCSEditorViewportClient::ToggleShowPreview),
		FCanExecuteAction(),
		FIsActionChecked::CreateSP(ViewportClient.Get(), &FDisplayClusterConfiguratorSCSEditorViewportClient::GetShowPreview));

	CommandList->MapAction(
		Commands.Show3DViewportNames,
		FExecuteAction::CreateSP(ViewportClient.Get(), &FDisplayClusterConfiguratorSCSEditorViewportClient::ToggleShowViewportNames),
		FCanExecuteAction::CreateSP(ViewportClient.Get(), &FDisplayClusterConfiguratorSCSEditorViewportClient::CanToggleViewportNames),
		FIsActionChecked::CreateSP(ViewportClient.Get(), &FDisplayClusterConfiguratorSCSEditorViewportClient::GetShowViewportNames));
	
	SAssetEditorViewport::BindCommands();
}

void SDisplayClusterConfiguratorSCSEditorViewport::OnFocusViewportToSelection()
{
	ViewportClient->FocusViewportToSelection();
}

EActiveTimerReturnType SDisplayClusterConfiguratorSCSEditorViewport::DeferredUpdatePreview(double InCurrentTime,
                                                                                           float InDeltaTime, bool bResetCamera)
{
	if (ViewportClient.IsValid())
	{
		ViewportClient->InvalidatePreview(bResetCamera);
	}

	bIsActiveTimerRegistered = false;
	return EActiveTimerReturnType::Stop;
}

#undef LOCTEXT_NAMESPACE
