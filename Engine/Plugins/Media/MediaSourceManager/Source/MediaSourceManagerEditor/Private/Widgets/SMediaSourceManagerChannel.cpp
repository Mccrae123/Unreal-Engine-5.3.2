// Copyright Epic Games, Inc. All Rights Reserved.

#include "Widgets/SMediaSourceManagerChannel.h"

#include "ContentBrowserModule.h"
#include "DragAndDrop/AssetDragDropOp.h"
#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Framework/Notifications/NotificationManager.h"
#include "IContentBrowserSingleton.h"
#include "IMediaIOCoreDeviceProvider.h"
#include "IMediaIOCoreModule.h"
#include "Inputs/MediaSourceManagerInputMediaSource.h"
#include "MediaIOPermutationsSelectorBuilder.h"
#include "MediaPlayer.h"
#include "MediaSource.h"
#include "MediaSourceManagerChannel.h"
#include "Styling/AppStyle.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SMediaSourceManagerTexture.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "SMediaSourceManagerChannel"

SMediaSourceManagerChannel::~SMediaSourceManagerChannel()
{
	if (ChannelPtr != nullptr)
	{
		ChannelPtr->OnInputPropertyChanged.RemoveAll(this);
	}

	DismissErrorNotification();
}

void SMediaSourceManagerChannel::Construct(const FArguments& InArgs,
	UMediaSourceManagerChannel* InChannel)
{
	ChannelPtr = InChannel;

	ChildSlot
		[
			SNew(SHorizontalBox)

			// Name of channel.
			+ SHorizontalBox::Slot()
				.FillWidth(0.11f)
				.Padding(2)
				.HAlign(HAlign_Left)
				[
					SNew(STextBlock)
						.Text(FText::FromString(ChannelPtr->Name))
				]

			// Set input.
			+ SHorizontalBox::Slot()
				.FillWidth(0.6f)
				.Padding(2)
				.HAlign(HAlign_Left)
				[
					SNew(SComboButton)
						.OnGetMenuContent(this, &SMediaSourceManagerChannel::CreateAssignInputMenu)
						.ContentPadding(2)
						.ButtonContent()
						[
							SAssignNew(InputNameTextBlock, STextBlock)
								.ToolTipText(LOCTEXT("Assign_ToolTip",
									"Assign an input to this channel."))
						]
				]

			// Input warning icon.
			+ SHorizontalBox::Slot()
				.AutoWidth()
				.HAlign(HAlign_Left)
				.VAlign(VAlign_Center)
				.Padding(4.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SImage)
						.Image(FCoreStyle::Get().GetBrush("Icons.Warning"))
						.ToolTipText(LOCTEXT("InputWarning", "Input has incorrect settings."))
						.Visibility(this,
							&SMediaSourceManagerChannel::HandleInputWarningIconVisibility)
				]

			// Out texture
			+ SHorizontalBox::Slot()
				.FillWidth(0.11f)
				.Padding(2)
				.HAlign(HAlign_Left)
				[
					SNew(SMediaSourceManagerTexture, ChannelPtr.Get())
				]
		];

	Refresh();

	// Start playing.
	if (ChannelPtr != nullptr)
	{
		ChannelPtr->OnInputPropertyChanged.AddSP(this, &SMediaSourceManagerChannel::Refresh);
		ChannelPtr->Play();
	}
}

void SMediaSourceManagerChannel::OnDragEnter(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent)
{
}

void SMediaSourceManagerChannel::OnDragLeave(const FDragDropEvent& DragDropEvent)
{
}

FReply SMediaSourceManagerChannel::OnDragOver(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent)
{
	// Is this an asset drop?
	if (TSharedPtr<FAssetDragDropOp> AssetDragDrop = DragDropEvent.GetOperationAs<FAssetDragDropOp>())
	{
		return FReply::Handled();
	}

	return FReply::Unhandled();
}

FReply SMediaSourceManagerChannel::OnDrop(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent)
{
	// Is this an asset drop?
	if (TSharedPtr<FAssetDragDropOp> AssetDragDrop = DragDropEvent.GetOperationAs<FAssetDragDropOp>())
	{
		for (const FAssetData& Asset : AssetDragDrop->GetAssets())
		{
			// Is this a media source?
			UMediaSource* MediaSource = Cast<UMediaSource>(Asset.GetAsset());
			if (MediaSource != nullptr)
			{
				AssignMediaSourceInput(MediaSource);
				break;
			}
		}
		
		return FReply::Handled();
	}

	return FReply::Unhandled();
}


TSharedRef<SWidget> SMediaSourceManagerChannel::CreateAssignInputMenu()
{
	FMenuBuilder MenuBuilder(true, NULL);

	// Add current asset options.
	UMediaSourceManagerChannel* Channel = ChannelPtr.Get();
	if ((Channel != nullptr) && (Channel->Input != nullptr))
	{
		MenuBuilder.BeginSection(NAME_None, LOCTEXT("CurrentAsset", "Current Asset"));

		// Edit.
		FUIAction EditAction(FExecuteAction::CreateSP(this, &SMediaSourceManagerChannel::OnEditInput));
		MenuBuilder.AddMenuEntry(LOCTEXT("Edit", "Edit"),
			LOCTEXT("EditToolTip", "Edit this asset"),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Edit"), EditAction);

		// Clear.
		FUIAction ClearAction(FExecuteAction::CreateSP(this, &SMediaSourceManagerChannel::ClearInput));
		MenuBuilder.AddMenuEntry(LOCTEXT("Clear", "Clear"),
			LOCTEXT("ClearToolTip", "Clears the asset set on this field"),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "GenericCommands.Delete"), ClearAction);

		MenuBuilder.EndSection();
	}

	// Get all Media IO device providers.
	IMediaIOCoreModule& MediaIOCoreModule = IMediaIOCoreModule::Get();
	TConstArrayView<IMediaIOCoreDeviceProvider*> DeviceProviders =
		MediaIOCoreModule.GetDeviceProviders();

	// Loop through each provider.
	for (IMediaIOCoreDeviceProvider* DeviceProvider : DeviceProviders)
	{
		if (DeviceProvider != nullptr)
		{
			// Start menu section.
			FName ProviderName = DeviceProvider->GetFName();
			MenuBuilder.BeginSection(ProviderName, FText::FromName(ProviderName));

			// Go over all connections.
			TArray<FMediaIOConnection> Connections = DeviceProvider->GetConnections();
			for (const FMediaIOConnection& Connection : Connections)
			{
				// Add this connection.
				FText DeviceName = FText::FromName(Connection.Device.DeviceName);
				FText LinkName = FMediaIOPermutationsSelectorBuilder::GetLabel(
					FMediaIOPermutationsSelectorBuilder::NAME_TransportType,
					Connection);
				FText MenuText;
				if (DeviceProvider->ShowInputTransportInSelector())
				{
					MenuText = FText::Format(LOCTEXT("Connection", "{0}: {1}"),
						DeviceName, LinkName);
				}
				else
				{
					MenuText = DeviceName;
				}

				FUIAction AssignMediaIOInputAction(FExecuteAction::CreateSP(this,
					&SMediaSourceManagerChannel::AssignMediaIOInput, DeviceProvider, Connection));
				MenuBuilder.AddMenuEntry(MenuText, FText(), FSlateIcon(),
					AssignMediaIOInputAction);
			}

			MenuBuilder.EndSection();
		}
	}

	// Add assets.
	MenuBuilder.BeginSection(NAME_None, LOCTEXT("MediaSourceAssets", "Media Source Assets"));
	auto SubMenuCallback = [this](FMenuBuilder& SubMenuBuilder)
	{
		SubMenuBuilder.AddWidget(BuildMediaSourcePickerWidget(), FText::GetEmpty(), true);
	};
	MenuBuilder.AddSubMenu(
		LOCTEXT("SelectAsset", "Select Asset"),
		LOCTEXT("SelectAsset_ToolTip", "Select an existing Media Source asset."),
		FNewMenuDelegate::CreateLambda(SubMenuCallback)
	);
	MenuBuilder.EndSection();

	return MenuBuilder.MakeWidget();
}

TSharedRef<SWidget> SMediaSourceManagerChannel::BuildMediaSourcePickerWidget()
{
	FAssetPickerConfig AssetPickerConfig;
	{
		AssetPickerConfig.OnAssetSelected = FOnAssetSelected::CreateRaw(this,
			&SMediaSourceManagerChannel::AddMediaSource);
		AssetPickerConfig.OnAssetEnterPressed = FOnAssetEnterPressed::CreateRaw(this,
			&SMediaSourceManagerChannel::AddMediaSourceEnterPressed);
		AssetPickerConfig.bAllowNullSelection = false;
		AssetPickerConfig.InitialAssetViewType = EAssetViewType::List;
		AssetPickerConfig.Filter.bRecursiveClasses = true;
		AssetPickerConfig.Filter.ClassPaths.Add(UMediaSource::StaticClass()->GetClassPathName());
		AssetPickerConfig.SaveSettingsName = TEXT("MediaSourceManagerAssetPicker");
	}

	FContentBrowserModule& ContentBrowserModule = FModuleManager::Get().LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));

	TSharedRef<SBox> Picker = SNew(SBox)
		.WidthOverride(300.0f)
		.HeightOverride(300.f)
		[
			ContentBrowserModule.Get().CreateAssetPicker(AssetPickerConfig)
		];

	return Picker;
}

void SMediaSourceManagerChannel::AddMediaSource(const FAssetData& AssetData)
{
	FSlateApplication::Get().DismissAllMenus();

	// Get media source from the asset..
	UObject* SelectedObject = AssetData.GetAsset();
	if (SelectedObject)
	{
		UMediaSource* MediaSource = Cast<UMediaSource>(AssetData.GetAsset());
		if (MediaSource != nullptr)
		{
			AssignMediaSourceInput(MediaSource);
		}
	}
}

void SMediaSourceManagerChannel::AddMediaSourceEnterPressed(const TArray<FAssetData>& AssetData)
{
	if (AssetData.Num() > 0)
	{
		AddMediaSource(AssetData[0].GetAsset());
	}
}

void SMediaSourceManagerChannel::ClearInput()
{
	UMediaSourceManagerChannel* Channel = ChannelPtr.Get();
	if (Channel != nullptr)
	{
		// Clear input on channel.
		Channel->Modify();
		Channel->Input = nullptr;

		// Stop player.
		UMediaPlayer* MediaPlayer = Channel->GetMediaPlayer();
		if (MediaPlayer != nullptr)
		{
			MediaPlayer->Close();
		}
		Refresh();
	}
}

void SMediaSourceManagerChannel::AssignMediaSourceInput(UMediaSource* MediaSource)
{
	UMediaSourceManagerChannel* Channel = ChannelPtr.Get();
	if (Channel != nullptr)
	{
		// Assign to channel.
		Channel->Modify();
		UMediaSourceManagerInputMediaSource* Input = NewObject<UMediaSourceManagerInputMediaSource>(Channel);
		Input->MediaSource = MediaSource;
		Channel->Input = Input;
		Channel->Play();

		Refresh();
	}
}

void SMediaSourceManagerChannel::AssignMediaIOInput(IMediaIOCoreDeviceProvider* DeviceProvider,
	FMediaIOConnection Connection)
{
	UMediaSourceManagerChannel* Channel = ChannelPtr.Get();
	if (Channel != nullptr)
	{
		// Create media source.
		FMediaIOConfiguration Configuration;
		Configuration = DeviceProvider->GetDefaultConfiguration();
		Configuration.MediaConnection = Connection;
		UMediaSource* MediaSource = DeviceProvider->CreateMediaSource(Configuration, Channel);
		if (MediaSource != nullptr)
		{
			AssignMediaSourceInput(MediaSource);
		}
		else
		{
			// Failed to create media source.
			// Remove any existing error.
			DismissErrorNotification();

			// Inform the user.
			FNotificationInfo Info(LOCTEXT("FailedToCreateMediaSource", "Failed to create a Media Source."));
			Info.bFireAndForget = false;
			Info.bUseLargeFont = false;
			Info.bUseThrobber = false;
			Info.FadeOutDuration = 0.25f;
			Info.ButtonDetails.Add(FNotificationButtonInfo(LOCTEXT("Dismiss", "Dismiss"), LOCTEXT("DismissToolTip", "Dismiss this notification."),
				FSimpleDelegate::CreateSP(this, &SMediaSourceManagerChannel::DismissErrorNotification)));

			ErrorNotificationPtr = FSlateNotificationManager::Get().AddNotification(Info);
			TSharedPtr<SNotificationItem> ErrorNotification = ErrorNotificationPtr.Pin();
			if (ErrorNotification != nullptr)
			{
				ErrorNotification->SetCompletionState(SNotificationItem::CS_Pending);
			}
		}
	}
}

void SMediaSourceManagerChannel::DismissErrorNotification()
{
	TSharedPtr<SNotificationItem> ErrorNotification = ErrorNotificationPtr.Pin();
	if (ErrorNotification != nullptr)
	{
		ErrorNotification->ExpireAndFadeout();
		ErrorNotification.Reset();
	}
}

void SMediaSourceManagerChannel::OnEditInput()
{
	// Get our input.
	TArray<UObject*> AssetArray;
	UMediaSourceManagerChannel* Channel = ChannelPtr.Get();
	if (Channel != nullptr)
	{
		UMediaSourceManagerInput* Input = Channel->Input;
		if (Input != nullptr)
		{
			UMediaSource* MediaSource = Input->GetMediaSource();
			if (MediaSource != nullptr)
			{
				AssetArray.Add(MediaSource);
			}
		}
	}

	// Open the editor.
	if (AssetArray.Num() > 0)
	{
		GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAssets(AssetArray);
	}
}

EVisibility SMediaSourceManagerChannel::HandleInputWarningIconVisibility() const
{
	return bIsInputValid ? EVisibility::Hidden : EVisibility::Visible;
}

void SMediaSourceManagerChannel::Refresh()
{
	// Get channel.
	UMediaSourceManagerChannel* Channel = ChannelPtr.Get();
	if (Channel != nullptr)
	{
		FText InputName = LOCTEXT("AssignInput", "Assign Input");

		// Get input.
		UMediaSourceManagerInput* Input = Channel->Input;
		UMediaSource* MediaSource = nullptr;
		if (Input != nullptr)
		{
			InputName = FText::FromString(Input->GetDisplayName());
			MediaSource = Input->GetMediaSource();;
		}

		// Update input widgets.
		InputNameTextBlock->SetText(InputName);

		// Is the input valid?
		bIsInputValid = true;
		if (MediaSource != nullptr)
		{
			bIsInputValid = MediaSource->Validate();
		}
	}
}


#undef LOCTEXT_NAMESPACE
