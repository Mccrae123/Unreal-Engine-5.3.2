// Copyright Epic Games, Inc. All Rights Reserved.

#include "MediaPlateCustomization.h"

#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "EditorStyleSet.h"
#include "IDetailGroup.h"
#include "MediaPlate.h"
#include "MediaPlateComponent.h"
#include "MediaPlateEditorModule.h"
#include "MediaPlayer.h"
#include "MediaPlaylist.h"
#include "MediaSource.h"
#include "PropertyCustomizationHelpers.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SFilePathPicker.h"
#include "Widgets/Images/SImage.h"

#define LOCTEXT_NAMESPACE "FMediaPlateCustomization"

/* IDetailCustomization interface
 *****************************************************************************/

void FMediaPlateCustomization::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	// Is this the media plate editor window?
	const IDetailsView* DetailsView = DetailBuilder.GetDetailsView();
	TSharedPtr<FTabManager> HostTabManager = DetailsView->GetHostTabManager();
	bool bIsMediaPlateWindow = (HostTabManager.IsValid() == false);

	// Get style.
	const ISlateStyle* Style = nullptr;
	FMediaPlateEditorModule* EditorModule = FModuleManager::LoadModulePtr<FMediaPlateEditorModule>("MediaPlateEditor");
	if (EditorModule != nullptr)
	{
		Style = EditorModule->GetStyle().Get();
	}
	if (Style == nullptr)
	{
		Style = &FEditorStyle::Get();
	}

	IDetailCategoryBuilder& MediaPlateCategory = DetailBuilder.EditCategory("MediaPlate");

	// Get objects we are editing.
	TArray<TWeakObjectPtr<UObject>> Objects;
	DetailBuilder.GetObjectsBeingCustomized(Objects);
	MediaPlatesList.Reserve(Objects.Num());
	for (TWeakObjectPtr<UObject>& Obj : Objects)
	{
		TWeakObjectPtr<UMediaPlateComponent> MediaPlate = Cast<UMediaPlateComponent>(Obj.Get());
		if (MediaPlate.IsValid())
		{
			MediaPlatesList.Add(MediaPlate);
		}
	}

	// Set media path.
	UpdateMediaPath();
	
	// Create playlist group.
	IDetailGroup& PlaylistGroup = MediaPlateCategory.AddGroup(TEXT("Playlist"),
		LOCTEXT("Playlist", "Playlist"));
	TSharedRef<IPropertyHandle> PropertyHandle = DetailBuilder.GetProperty(
		GET_MEMBER_NAME_CHECKED(UMediaPlateComponent, MediaPlaylist));
	PlaylistGroup.HeaderProperty(PropertyHandle);

	// Add media source.
	PlaylistGroup.AddWidgetRow()
		.NameContent()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("MediaSource", "Media Source"))
			.ToolTipText(LOCTEXT("MediaSource_ToolTip", "The Media Source to play."))
			.Font(IDetailLayoutBuilder::GetDetailFont())
		]
		.ValueContent()
		[
			SNew(SObjectPropertyEntryBox)
				.AllowedClass(UMediaSource::StaticClass())
				.ObjectPath(this, &FMediaPlateCustomization::GetMediaSourcePath)
				.OnObjectChanged(this, &FMediaPlateCustomization::OnMediaSourceChanged)
		];

	// Add media path.
	FString FileTypeFilter = TEXT("All files (*.*)|*.*");
	PlaylistGroup.AddWidgetRow()
		.NameContent()
		[
			SNew(STextBlock)
				.Text(LOCTEXT("MediaPath", "Media Path"))
				.ToolTipText(LOCTEXT("MediaPath_ToolTip",
					"The path of the Media Source to play.\nChanging this will create a new media source in the level to play this path."))
				.Font(IDetailLayoutBuilder::GetDetailFont())
		]
		.ValueContent()
		[
			SNew(SFilePathPicker)
				.BrowseButtonImage(FEditorStyle::GetBrush("PropertyWindow.Button_Ellipsis"))
				.BrowseButtonStyle(FEditorStyle::Get(), "HoverHintOnly")
				.BrowseButtonToolTip(LOCTEXT("FileButtonToolTipText", "Choose a file from this computer"))
				.BrowseTitle(LOCTEXT("PropertyEditorTitle", "File picker..."))
				.FilePath(this, &FMediaPlateCustomization::HandleMediaPath)
				.FileTypeFilter(FileTypeFilter)
				.OnPathPicked(this, &FMediaPlateCustomization::HandleMediaPathPicked)
		];

	// Add media control buttons.
	MediaPlateCategory.AddCustomRow(LOCTEXT("MediaPlateControls", "MediaPlate Controls"))
		[
			SNew(SHorizontalBox)

			// Rewind button.
			+ SHorizontalBox::Slot()
				.VAlign(VAlign_Center)
				.AutoWidth()
				[
					SNew(SButton)
					.VAlign(VAlign_Center)
					.OnClicked_Lambda([this]() -> FReply
					{
						for (TWeakObjectPtr<UMediaPlateComponent>& MediaPlatePtr : MediaPlatesList)
						{
							UMediaPlateComponent* MediaPlate = MediaPlatePtr.Get();
							if (MediaPlate != nullptr)
							{
								UMediaPlayer* MediaPlayer = MediaPlate->GetMediaPlayer();
								if (MediaPlayer != nullptr)
								{
									MediaPlayer->Rewind();
								}
							}
						}
						return FReply::Handled();
					})
					[
						SNew(SImage)
						.ColorAndOpacity(FSlateColor::UseForeground())
					.Image(Style->GetBrush("MediaPlateEditor.RewindMedia.Small"))
					]
				]

			// Reverse button.
			+ SHorizontalBox::Slot()
				.VAlign(VAlign_Center)
				.AutoWidth()
				[
					SNew(SButton)
						.VAlign(VAlign_Center)
						.OnClicked_Lambda([this]() -> FReply
						{
							for (TWeakObjectPtr<UMediaPlateComponent>& MediaPlatePtr : MediaPlatesList)
							{
								UMediaPlateComponent* MediaPlate = MediaPlatePtr.Get();
								if (MediaPlate != nullptr)
								{
									UMediaPlayer* MediaPlayer = MediaPlate->GetMediaPlayer();
									if (MediaPlayer != nullptr)
									{
										MediaPlayer->SetRate(GetReverseRate(MediaPlayer));
									}
								}
							}
							return FReply::Handled();
						})
						[
							SNew(SImage)
								.ColorAndOpacity(FSlateColor::UseForeground())
								.Image(Style->GetBrush("MediaPlateEditor.ReverseMedia.Small"))
						]
				]

			// Play button.
			+ SHorizontalBox::Slot()
				.VAlign(VAlign_Center)
				.AutoWidth()
				[
					SNew(SButton)
						.VAlign(VAlign_Center)
						.OnClicked_Lambda([this]() -> FReply
						{
							for (TWeakObjectPtr<UMediaPlateComponent>& MediaPlatePtr : MediaPlatesList)
							{
								UMediaPlateComponent* MediaPlate = MediaPlatePtr.Get();
								if (MediaPlate != nullptr)
								{
									// Is the player paused or fast forwarding/rewinding?
									UMediaPlayer* MediaPlayer = MediaPlate->GetMediaPlayer();
									if ((MediaPlayer != nullptr) &&
										((MediaPlayer->IsPaused()) || 
											(MediaPlayer->IsPlaying() && (MediaPlayer->GetRate() != 1.0f))))
									{
										MediaPlayer->Play();
									}
									else
									{
										// Tell the editor module that this media plate is playing.
										FMediaPlateEditorModule* EditorModule = FModuleManager::LoadModulePtr<FMediaPlateEditorModule>("MediaPlateEditor");
										if (EditorModule != nullptr)
										{
											EditorModule->MediaPlateStartedPlayback(MediaPlate);
										}

										// Play the media.
										MediaPlate->Play();
									}
								}
							}
							return FReply::Handled();
						})
						[
							SNew(SImage)
								.ColorAndOpacity(FSlateColor::UseForeground())
								.Image(Style->GetBrush("MediaPlateEditor.PlayMedia.Small"))
						]
				]

			// Pause button.
			+ SHorizontalBox::Slot()
				.VAlign(VAlign_Center)
				.AutoWidth()
				[
					SNew(SButton)
					.VAlign(VAlign_Center)
					.OnClicked_Lambda([this]() -> FReply
					{
						for (TWeakObjectPtr<UMediaPlateComponent>& MediaPlatePtr : MediaPlatesList)
						{
							UMediaPlateComponent* MediaPlate = MediaPlatePtr.Get();
							if (MediaPlate != nullptr)
							{
								UMediaPlayer* MediaPlayer = MediaPlate->GetMediaPlayer();
								if (MediaPlayer != nullptr)
								{
									MediaPlayer->Pause();
								}
							}
						}
						return FReply::Handled();
					})
					[
						SNew(SImage)
							.ColorAndOpacity(FSlateColor::UseForeground())
							.Image(Style->GetBrush("MediaPlateEditor.PauseMedia.Small"))
					]
				]

			// Forward button.
			+ SHorizontalBox::Slot()
				.VAlign(VAlign_Center)
				.AutoWidth()
				[
					SNew(SButton)
					.VAlign(VAlign_Center)
					.OnClicked_Lambda([this]() -> FReply
					{
						for (TWeakObjectPtr<UMediaPlateComponent>& MediaPlatePtr : MediaPlatesList)
						{
							UMediaPlateComponent* MediaPlate = MediaPlatePtr.Get();
							if (MediaPlate != nullptr)
							{
								UMediaPlayer* MediaPlayer = MediaPlate->GetMediaPlayer();
								if (MediaPlayer != nullptr)
								{
									MediaPlayer->SetRate(GetForwardRate(MediaPlayer));
								}
							}
						}
						return FReply::Handled();
					})
					[
						SNew(SImage)
							.ColorAndOpacity(FSlateColor::UseForeground())
							.Image(Style->GetBrush("MediaPlateEditor.ForwardMedia.Small"))
					]
				]

			// Stop button.
			+ SHorizontalBox::Slot()
				.VAlign(VAlign_Center)
				.AutoWidth()
				[
					SNew(SButton)
						.VAlign(VAlign_Center)
						.OnClicked_Lambda([this]() -> FReply
						{
							for (TWeakObjectPtr<UMediaPlateComponent>& MediaPlatePtr : MediaPlatesList)
							{
								UMediaPlateComponent* MediaPlate = MediaPlatePtr.Get();
								if (MediaPlate != nullptr)
								{
									MediaPlate->Stop();
								}
							}
							return FReply::Handled();
						})
						[
							SNew(SImage)
								.ColorAndOpacity(FSlateColor::UseForeground())
								.Image(Style->GetBrush("MediaPlateEditor.StopMedia.Small"))
						]
				]
		];


	// Add button to open the media plate editor.
	if (bIsMediaPlateWindow == false)
	{
		MediaPlateCategory.AddCustomRow(LOCTEXT("OpenMediaPlate", "Open Media Plate"))
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
					.FillWidth(1.f)
					.Padding(0, 5, 10, 5)
					[
						SNew(SButton)
							.ContentPadding(3)
							.VAlign(VAlign_Center)
							.HAlign(HAlign_Center)
							.OnClicked(this, &FMediaPlateCustomization::OnOpenMediaPlate)
							.Text(LOCTEXT("OpenMediaPlate", "Open Media Plate"))
					]
			];
	}
}

FString FMediaPlateCustomization::GetMediaSourcePath() const
{
	FString Path;

	// Get the first media plate.
	if (MediaPlatesList.Num() > 0)
	{
		UMediaPlateComponent* MediaPlate = MediaPlatesList[0].Get();
		if (MediaPlate != nullptr)
		{
			UMediaPlaylist* Playlist = MediaPlate->MediaPlaylist;
			if (Playlist != nullptr)
			{
				// Get the first media source in the playlist.
				UMediaSource* MediaSource = Playlist->Get(0);
				if (MediaSource != nullptr)
				{
					Path = MediaSource->GetPathName();
				}
			}
		}
	}

	return Path;
}

void FMediaPlateCustomization::OnMediaSourceChanged(const FAssetData& AssetData)
{
	// Update the playlist with the new media source.
	UMediaSource* MediaSource = Cast<UMediaSource>(AssetData.GetAsset());
	for (TWeakObjectPtr<UMediaPlateComponent>& MediaPlatePtr : MediaPlatesList)
	{
		UMediaPlateComponent* MediaPlate = MediaPlatePtr.Get();
		if (MediaPlate != nullptr)
		{
			UMediaPlaylist* Playlist = MediaPlate->MediaPlaylist;
			if (Playlist != nullptr)
			{
				if (Playlist->Num() > 0)
				{
					Playlist->Replace(0, MediaSource);
				}
				else
				{
					Playlist->Add(MediaSource);
				}
				Playlist->MarkPackageDirty();
			}
		}
	}

	StopMediaPlates();
	UpdateMediaPath();
}

void FMediaPlateCustomization::UpdateMediaPath()
{
	MediaPath.Empty();

	// Get the first media plate.
	if (MediaPlatesList.Num() > 0)
	{
		UMediaPlateComponent* MediaPlate = MediaPlatesList[0].Get();
		if (MediaPlate != nullptr)
		{
			UMediaPlaylist* Playlist = MediaPlate->MediaPlaylist;
			if (Playlist != nullptr)
			{
				// Get the first media source in the playlist.
				UMediaSource* MediaSource = Playlist->Get(0);
				if (MediaSource != nullptr)
				{
					MediaPath = MediaSource->GetUrl();

					// Remove certain types.
					const FString FilePrefix(TEXT("file://"));
					const FString ImgPrefix(TEXT("img://"));
					if (MediaPath.StartsWith(FilePrefix))
					{
						MediaPath = MediaPath.RightChop(FilePrefix.Len());
					}
					else if (MediaPath.StartsWith(ImgPrefix))
					{
						MediaPath = MediaPath.RightChop(ImgPrefix.Len());
					}
				}
			}
		}
	}
}

FString FMediaPlateCustomization::HandleMediaPath() const
{
	return MediaPath;
}

void FMediaPlateCustomization::HandleMediaPathPicked(const FString& PickedPath)
{
	// Did we get something?
	if ((PickedPath.IsEmpty() == false) && (PickedPath != MediaPath))
	{
		// Stop playback.
		StopMediaPlates();

		// Set up media source for our media plates.
		for (TWeakObjectPtr<UMediaPlateComponent>& MediaPlatePtr : MediaPlatesList)
		{
			UMediaPlateComponent* MediaPlate = MediaPlatePtr.Get();
			if (MediaPlate != nullptr)
			{
				UMediaPlaylist* Playlist = MediaPlate->MediaPlaylist;
				if (Playlist != nullptr)
				{
					// Create media source for this path.
					UMediaSource* MediaSource = UMediaSource::SpawnMediaSourceForString(PickedPath, MediaPlate);
					if (MediaSource != nullptr)
					{
						if (Playlist->Num() > 0)
						{
							Playlist->Replace(0, MediaSource);
						}
						else
						{
							Playlist->Add(MediaSource);
						}
						Playlist->MarkPackageDirty();
					}
				}
			}
		}

		// Update the media path.
		UpdateMediaPath();
	}
}

FReply FMediaPlateCustomization::OnOpenMediaPlate()
{
	// Get all our objects.
	TArray<UObject*> AssetArray;
	for (TWeakObjectPtr<UMediaPlateComponent>& MediaPlatePtr : MediaPlatesList)
	{
		UMediaPlateComponent* MediaPlate = MediaPlatePtr.Get();
		if (MediaPlate != nullptr)
		{
			AssetArray.Add(MediaPlate);
		}
	}

	// Open the editor.
	if (AssetArray.Num() > 0)
	{
		GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAssets(AssetArray);
	}

	return FReply::Handled();
}

void FMediaPlateCustomization::StopMediaPlates()
{
	for (TWeakObjectPtr<UMediaPlateComponent>& MediaPlatePtr : MediaPlatesList)
	{
		UMediaPlateComponent* MediaPlate = MediaPlatePtr.Get();
		if (MediaPlate != nullptr)
		{
			MediaPlate->Stop();
		}
	}
}

float FMediaPlateCustomization::GetForwardRate(UMediaPlayer* MediaPlayer) const
{
	float Rate = MediaPlayer->GetRate();

	if (Rate < 1.0f)
	{
		Rate = 1.0f;
	}

	return 2.0f * Rate;
}

float FMediaPlateCustomization::GetReverseRate(UMediaPlayer* MediaPlayer) const
{
	float Rate = MediaPlayer->GetRate();

	if (Rate > -1.0f)
	{
		return -1.0f;
	}

	return 2.0f * Rate;
}


#undef LOCTEXT_NAMESPACE
