// Copyright Epic Games, Inc. All Rights Reserved.

#include "VirtualCameraActor.h"

#include "CineCameraComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameEngine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Features/IModularFeatures.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "ILiveLinkClient.h"
#include "Channels/RemoteSessionImageChannel.h"
#include "Channels/RemoteSessionInputChannel.h"
#include "ImageProviders/RemoteSessionMediaOutput.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "RemoteSession.h"
#include "Roles/LiveLinkTransformRole.h"
#include "Roles/LiveLinkTransformTypes.h"
#include "Slate/SceneViewport.h"
#include "VirtualCamera.h"
#include "VirtualCameraMovement.h"
#include "VirtualCameraSubsystem.h"
#include "VPFullScreenUserWidget.h"
#include "Widgets/SVirtualWindow.h"

#if WITH_EDITOR
#include "AssetData.h"
#include "AssetRegistryModule.h"
#include "Blueprint/UserWidget.h"
#include "Editor.h"
#include "EditorSupportDelegates.h"
#include "Editor/EditorEngine.h"
#include "IAssetViewport.h"
#include "LevelEditor.h"
#include "LevelEditorViewport.h"
#include "SceneView.h"
#include "SLevelViewport.h"
#endif

namespace
{
	static const FName AssetRegistryName(TEXT("AssetRegistry"));
	static const FName LevelEditorName(TEXT("LevelEditor"));
	static const FString SavedSettingsSlotName(TEXT("SavedVirtualCameraSettings"));
	static const TCHAR* DefaultCameraUMG = TEXT("/VirtualCamera/V2/Widgets/VCam2UI.VCam2UI_C");
	static const FName DefaultLiveLinkSubjectName(TEXT("CameraTransform"));
	static const FVector2D DefaultViewportResolution(1536, 1152);
	static const float MaxFocusTraceDistance = 1000000.0f;
	//circle of confusion constant used to calculate hyperfocal distance
	static const float CircleOfConfusion = 0.03f;

	void FindSceneViewport(TWeakPtr<SWindow>& OutInputWindow, TWeakPtr<FSceneViewport>& OutSceneViewport)
	{
#if WITH_EDITOR
		if (GIsEditor)
		{
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (Context.WorldType == EWorldType::Editor)
				{
					if (FModuleManager::Get().IsModuleLoaded(LevelEditorName))
					{
						FLevelEditorModule& LevelEditorModule = FModuleManager::GetModuleChecked<FLevelEditorModule>(LevelEditorName);
						TSharedPtr<IAssetViewport> ActiveLevelViewport = LevelEditorModule.GetFirstActiveViewport();
						if (ActiveLevelViewport.IsValid())
						{
							OutSceneViewport = ActiveLevelViewport->GetSharedActiveViewport();
							OutInputWindow = FSlateApplication::Get().FindWidgetWindow(ActiveLevelViewport->AsWidget());
						}
					}
				}
				else if (Context.WorldType == EWorldType::PIE)
				{
					FSlatePlayInEditorInfo* SlatePlayInEditorSession = GEditor->SlatePlayInEditorMap.Find(Context.ContextHandle);
					if (SlatePlayInEditorSession)
					{
						if (SlatePlayInEditorSession->DestinationSlateViewport.IsValid())
						{
							TSharedPtr<IAssetViewport> DestinationLevelViewport = SlatePlayInEditorSession->DestinationSlateViewport.Pin();
							OutSceneViewport = DestinationLevelViewport->GetSharedActiveViewport();
							OutInputWindow = FSlateApplication::Get().FindWidgetWindow(DestinationLevelViewport->AsWidget());
						}
						else if (SlatePlayInEditorSession->SlatePlayInEditorWindowViewport.IsValid())
						{
							OutSceneViewport = SlatePlayInEditorSession->SlatePlayInEditorWindowViewport;
							OutInputWindow = SlatePlayInEditorSession->SlatePlayInEditorWindow;
						}
					}
				}
			}
		}
		else
#endif
		{
			UGameEngine* GameEngine = Cast<UGameEngine>(GEngine);
			OutSceneViewport = GameEngine->SceneViewport;
			OutInputWindow = GameEngine->GameViewportWindow;
		}
	}

	bool DeprojectScreenToWorld(const FVector2D& InScreenPosition, FVector& OutWorldPosition, FVector& OutWorldDirection)
	{
		bool bSuccess = false;

		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType == EWorldType::PIE || Context.WorldType == EWorldType::Game)
			{
				APlayerController* PC = Context.OwningGameInstance->GetFirstLocalPlayerController(Context.World());
				if (PC)
				{
					bSuccess |= PC->DeprojectScreenPositionToWorld(InScreenPosition.X, InScreenPosition.Y, OutWorldPosition, OutWorldDirection);
					break;
				}
			}
#if WITH_EDITOR
			else if (Context.WorldType == EWorldType::Editor)
			{
				FLevelEditorModule& LevelEditorModule = FModuleManager::GetModuleChecked<FLevelEditorModule>(LevelEditorName);
				TSharedPtr<SLevelViewport> ActiveLevelViewport = LevelEditorModule.GetFirstActiveLevelViewport();
				if (ActiveLevelViewport.IsValid() && ActiveLevelViewport->GetActiveViewport())
				{
					FViewport* ActiveViewport = ActiveLevelViewport->GetActiveViewport();
					FLevelEditorViewportClient& LevelViewportClient = ActiveLevelViewport->GetLevelViewportClient();
					FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
						ActiveViewport,
						LevelViewportClient.GetScene(),
						LevelViewportClient.EngineShowFlags)
						.SetRealtimeUpdate(true));
					FSceneView* View = LevelViewportClient.CalcSceneView(&ViewFamily);

					const FIntPoint ViewportSize = ActiveViewport->GetSizeXY();
					const FIntRect ViewRect = FIntRect(0, 0, ViewportSize.X, ViewportSize.Y);
					const FMatrix InvViewProjectionMatrix = View->ViewMatrices.GetInvViewProjectionMatrix();
					FSceneView::DeprojectScreenToWorld(InScreenPosition, ViewRect, InvViewProjectionMatrix, OutWorldPosition, OutWorldDirection);
					bSuccess = true;
				}
			}
#endif
		}

		if (!bSuccess)
		{
			OutWorldPosition = FVector::ZeroVector;
			OutWorldDirection = FVector::ZeroVector;
		}
		return bSuccess;
	}
}

struct FVirtualCameraViewportSettings
{
	FIntPoint Size;
	FVector2D CameraPosition;
	TWeakObjectPtr<AActor> ActorLock;
	bool bRealTime;
	bool bDrawAxes;
	bool bDisableInput;
	bool bAllowCinematicControl;
};

int32 AVirtualCameraActor::PresetIndex = 1;

AVirtualCameraActor::AVirtualCameraActor(const FObjectInitializer& ObjectInitializer)
	: LiveLinkSubject{ DefaultLiveLinkSubjectName, ULiveLinkTransformRole::StaticClass() }
	, TargetDeviceResolution(DefaultViewportResolution)
	, RemoteSessionPort(IRemoteSessionModule::kDefaultPort)
	, ActorWorld(nullptr)
	, PreviousViewTarget(nullptr)
	, bAllowFocusVisualization(true)
	, FocusMethod(EVirtualCameraFocusMethod::Manual)
	, DesiredDistanceUnits(EUnit::Meters)
	, bSaveSettingsOnStopStreaming(false)
	, bIsStreaming(false)
	, ViewportSettingsBackup(nullptr)
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	// Create Components
	DefaultSceneRoot = CreateDefaultSubobject<USceneComponent>("DefaultSceneRoot");
	SetRootComponent(DefaultSceneRoot);

	SceneOffset = CreateDefaultSubobject<USceneComponent>("SceneOffset");
	SceneOffset->SetupAttachment(DefaultSceneRoot);

	CameraOffset = CreateDefaultSubobject<USceneComponent>("CameraOffset");
	CameraOffset->SetupAttachment(SceneOffset);

	RecordingCamera = CreateDefaultSubobject<UCineCameraComponent>("Recording Camera");
	RecordingCamera->SetupAttachment(CameraOffset);
	StreamedCamera = CreateDefaultSubobject<UCineCameraComponent>("Streamed Camera");
	StreamedCamera->SetupAttachment(CameraOffset);

	MovementComponent = CreateDefaultSubobject<UVirtualCameraMovement>("Movement Component");
	MediaOutput = CreateDefaultSubobject<URemoteSessionMediaOutput>("Media Output");
	CameraScreenWidget = CreateDefaultSubobject<UVPFullScreenUserWidget>("Camera UMG");
	CameraScreenWidget->SetDisplayTypes(EVPWidgetDisplayType::PostProcess, EVPWidgetDisplayType::Viewport, EVPWidgetDisplayType::PostProcess);
	CameraScreenWidget->PostProcessDisplayType.bReceiveHardwareInput = true;
}

AVirtualCameraActor::AVirtualCameraActor(FVTableHelper& Helper)
	: Super(Helper)
{
}

AVirtualCameraActor::~AVirtualCameraActor() = default;

void AVirtualCameraActor::Destroyed()
{
	if (CameraScreenWidget && CameraScreenWidget->IsDisplayed())
	{
		CameraScreenWidget->Hide();
	}

	if (RemoteSessionHost && RemoteSessionHost->IsConnected())
	{
		RemoteSessionHost->Close();
	}
}

#if WITH_EDITOR
bool AVirtualCameraActor::ShouldTickIfViewportsOnly() const
{
	return true;
}
#endif

bool AVirtualCameraActor::IsStreaming_Implementation() const
{
	return bIsStreaming;
}

bool AVirtualCameraActor::ShouldSaveSettingsOnStopStreaming_Implementation() const
{
	return bSaveSettingsOnStopStreaming;
}

void AVirtualCameraActor::SetSaveSettingsOnStopStreaming_Implementation(bool bShouldSave)
{
	bSaveSettingsOnStopStreaming = bShouldSave;
}

void AVirtualCameraActor::SetRelativeTransform_Implementation(const FTransform& InControllerTransform)
{
	SetRelativeTransformInternal(InControllerTransform);
}

FTransform AVirtualCameraActor::GetRelativeTransform_Implementation() const
{
	return StreamedCamera->GetRelativeTransform();
}

void AVirtualCameraActor::AddBlendableToCamera_Implementation(const TScriptInterface<IBlendableInterface>& InBlendableToAdd, float InWeight)
{
	UCineCameraComponent* CineCamera = GetActiveCameraComponentInternal();
	CineCamera->PostProcessSettings.AddBlendable(InBlendableToAdd, InWeight);
}

void AVirtualCameraActor::SetFocusDistance_Implementation(float InFocusDistanceCentimeters)
{
	UCineCameraComponent* CineCamera = GetActiveCameraComponentInternal();
	CineCamera->FocusSettings.ManualFocusDistance = InFocusDistanceCentimeters;
	CineCamera->FocusSettings.FocusOffset = 0.0f;
}

void AVirtualCameraActor::SetTrackedActorForFocus_Implementation(AActor* InActorToTrack, const FVector& InTrackingPointOffset)
{
	UCineCameraComponent* CineCamera = GetActiveCameraComponentInternal();
	CineCamera->FocusSettings.TrackingFocusSettings.ActorToTrack = InActorToTrack;
	CineCamera->FocusSettings.TrackingFocusSettings.RelativeOffset = InTrackingPointOffset;
}

void AVirtualCameraActor::SetFocusMethod_Implementation(EVirtualCameraFocusMethod InNewFocusMethod)
{
	FocusMethod = InNewFocusMethod;

	UCineCameraComponent* CineCamera = GetActiveCameraComponentInternal();

	switch (InNewFocusMethod)
	{
		case EVirtualCameraFocusMethod::None:
			CineCamera->FocusSettings.FocusMethod = ECameraFocusMethod::DoNotOverride;
			break;
		case EVirtualCameraFocusMethod::Auto:
			CineCamera->FocusSettings.FocusMethod = ECameraFocusMethod::Manual;
			break;
		case EVirtualCameraFocusMethod::Manual:
			CineCamera->FocusSettings.FocusMethod = ECameraFocusMethod::Manual;
			break;
		case EVirtualCameraFocusMethod::Tracking:
			CineCamera->FocusSettings.FocusMethod = ECameraFocusMethod::Tracking;
			break;
		default: // Should never be reached, but just in case new focus methods are added
			UE_LOG(LogVirtualCamera, Warning, TEXT("Specified focus method is not currently supported in Virtual Camera!"))
			break;
	}
}

EVirtualCameraFocusMethod AVirtualCameraActor::GetFocusMethod_Implementation() const
{
	return FocusMethod;
}

void AVirtualCameraActor::SetFocusVisualization_Implementation(bool bInShowFocusVisualization)
{
	UCineCameraComponent* CineCamera = GetActiveCameraComponentInternal();
	if (CineCamera->FocusSettings.FocusMethod == ECameraFocusMethod::DoNotOverride)
	{
		UE_LOG(LogVirtualCamera, Warning, TEXT("Camera focus mode is currently set to none, cannot display focus plane!"))
		return;
	}
	CineCamera->FocusSettings.bDrawDebugFocusPlane = bInShowFocusVisualization;
}

void AVirtualCameraActor::SetReticlePosition_Implementation(const FVector2D & InViewportPosition)
{
	ReticlePosition = InViewportPosition;
}

FVector2D AVirtualCameraActor::GetReticlePosition_Implementation() const
{
	return ReticlePosition;
}

void AVirtualCameraActor::UpdateHyperfocalDistance_Implementation()
{
	UCineCameraComponent* CineCamera = GetActiveCameraComponentInternal();

	//avoid division by zero
	if (CineCamera->CurrentAperture == 0.0f)
	{
		HyperfocalDistance = 0.0f;
	}
	else
	{
		//hyperfocal distance formula:((focal length ^2)/(fstop * CoC)) + focal length
		HyperfocalDistance = ((CineCamera->CurrentFocalLength * CineCamera->CurrentFocalLength) / (CineCamera->CurrentAperture * CircleOfConfusion)) + CineCamera->CurrentFocalLength;
		//convert from mm to cm
		HyperfocalDistance *= 0.1f;
	}
}

float AVirtualCameraActor::GetHyperfocalDistance_Implementation() const
{
	return HyperfocalDistance;
}

void AVirtualCameraActor::SetBeforeSetVirtualCameraTransformDelegate_Implementation(const FPreSetVirtualCameraTransform& InDelegate)
{
	OnPreSetVirtualCameraTransform = InDelegate;
}

void AVirtualCameraActor::SetOnActorClickedDelegate_Implementation(const FOnActorClickedDelegate& InDelegate)
{
	OnActorClickedDelegate = InDelegate;
}

void AVirtualCameraActor::AddOnVirtualCameraUpdatedDelegate_Implementation(const FVirtualCameraTickDelegate& InDelegate)
{
	OnVirtualCameraUpdatedDelegates.Add(InDelegate);
}

void AVirtualCameraActor::RemoveOnVirtualCameraUpdatedDelegate_Implementation(const FVirtualCameraTickDelegate& InDelegate)
{
	OnVirtualCameraUpdatedDelegates.Remove(InDelegate);
}

void AVirtualCameraActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!bIsStreaming)
	{
		return;
	}

	if (RemoteSessionHost)
	{
		RemoteSessionHost->Tick(DeltaSeconds);
	}

	if (CameraScreenWidget && CameraUMGClass)
	{
		CameraScreenWidget->Tick(DeltaSeconds);
	}

	FMinimalViewInfo ViewInfo;
	CalcCamera(DeltaSeconds, ViewInfo);

	ILiveLinkClient& LiveLinkClient = IModularFeatures::Get().GetModularFeature<ILiveLinkClient>(ILiveLinkClient::ModularFeatureName);
	FLiveLinkSubjectFrameData SubjectData;
	const bool bHasValidData = LiveLinkClient.EvaluateFrame_AnyThread(LiveLinkSubject.Subject, LiveLinkSubject.Role, SubjectData);
	if (bHasValidData)
	{
		const FLiveLinkTransformFrameData& TransformFrameData = *SubjectData.FrameData.Cast<FLiveLinkTransformFrameData>();
		FVirtualCameraTransform CameraTransform = FVirtualCameraTransform{TransformFrameData.Transform};

		// execute delegates that want to manipulate camera transform before it is set onto the root
		if (OnPreSetVirtualCameraTransform.IsBound())
		{
			FEditorScriptExecutionGuard ScriptGuard;
			CameraTransform = OnPreSetVirtualCameraTransform.Execute(CameraTransform);
		}

		SetRelativeTransformInternal(CameraTransform.Transform);
	}

	if (FocusMethod == EVirtualCameraFocusMethod::Auto)
	{
		UpdateAutoFocus();
	}

	if (OnVirtualCameraUpdatedDelegates.IsBound())
	{
		FEditorScriptExecutionGuard ScriptGuard;
		OnVirtualCameraUpdatedDelegates.Broadcast(DeltaSeconds);
	}
}

void AVirtualCameraActor::BeginPlay()
{
	Super::BeginPlay();

	UVirtualCameraSubsystem* SubSystem = GEngine->GetEngineSubsystem<UVirtualCameraSubsystem>();
	if (!SubSystem->GetVirtualCameraController())
	{
		SubSystem->SetVirtualCameraController(this);
	}

	StartStreaming();
}

void AVirtualCameraActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	Super::EndPlay(EndPlayReason);
	StopStreaming();
}

bool AVirtualCameraActor::StartStreaming()
{
	ActorWorld = GetWorld();
	if (!ActorWorld)
	{
		return false;
	}

	if (bSaveSettingsOnStopStreaming)
	{
		LoadSettings();
	}

	if (!CameraUMGClass)
	{
		FSoftClassPath DefaultUMG(DefaultCameraUMG);
		CameraUMGClass = DefaultUMG.TryLoadClass<UUserWidget>();
	}

#if WITH_EDITOR
	if (ActorWorld->WorldType == EWorldType::Editor)
	{
		ViewportSettingsBackup = MakeUnique<FVirtualCameraViewportSettings>();

		FLevelEditorModule& LevelEditorModule = FModuleManager::GetModuleChecked<FLevelEditorModule>(LevelEditorName);
		TSharedPtr<SLevelViewport> ActiveLevelViewport = LevelEditorModule.GetFirstActiveLevelViewport();
		if (ActiveLevelViewport.IsValid())
		{
			ActiveLevelViewport->GetSharedActiveViewport()->SetFixedViewportSize(TargetDeviceResolution.X, TargetDeviceResolution.Y);

			FLevelEditorViewportClient& LevelViewportClient = ActiveLevelViewport->GetLevelViewportClient();
			ViewportSettingsBackup->ActorLock = LevelViewportClient.GetActiveActorLock();
			LevelViewportClient.SetActorLock(this);

			ViewportSettingsBackup->bDrawAxes = LevelViewportClient.bDrawAxes;
			ViewportSettingsBackup->bDisableInput = LevelViewportClient.bDisableInput;
			ViewportSettingsBackup->bAllowCinematicControl = LevelViewportClient.AllowsCinematicControl();

			LevelViewportClient.SetRealtime(true);
			LevelViewportClient.bDrawAxes = false;
			LevelViewportClient.bDisableInput = true;
			LevelViewportClient.SetAllowCinematicControl(false);

			// add event listeners to stop streaming when necessary
			LevelEditorModule.OnMapChanged().AddUObject(this, &AVirtualCameraActor::OnMapChanged);
			GEditor->OnBlueprintPreCompile().AddUObject(this, &AVirtualCameraActor::OnBlueprintPreCompile);
			FEditorSupportDelegates::PrepareToCleanseEditorObject.AddUObject(this, &AVirtualCameraActor::OnPrepareToCleanseEditorObject);
			FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(AssetRegistryName);
			AssetRegistryModule.Get().OnAssetRemoved().AddUObject(this, &AVirtualCameraActor::OnAssetRemoved);
			FEditorDelegates::OnAssetsCanDelete.AddUObject(this, &AVirtualCameraActor::OnAssetsCanDelete);
		}
	}
	else
#endif
	{
		APlayerController* PlayerController = ActorWorld->GetGameInstance()->GetFirstLocalPlayerController();
		if (!PlayerController)
		{
			return false;
		}

		PreviousViewTarget = PlayerController->GetViewTarget();

		FViewTargetTransitionParams TransitionParams;
		PlayerController->SetViewTarget(this, TransitionParams);
	}
	
	// use the aspect ratio of the device we're streaming to, so the UMG and the camera capture fit together and span the device's surface
	StreamedCamera->Filmback.SensorWidth = TargetDeviceResolution.X / 100.0f;
	StreamedCamera->Filmback.SensorHeight = TargetDeviceResolution.Y / 100.0f;

	if (CameraUMGClass)
	{
		CameraScreenWidget->WidgetClass = CameraUMGClass;
		CameraScreenWidget->Display(ActorWorld);
	}

	if (IRemoteSessionModule* RemoteSession = FModuleManager::LoadModulePtr<IRemoteSessionModule>("RemoteSession"))
	{
		TArray<FRemoteSessionChannelInfo> SupportedChannels;
		SupportedChannels.Emplace(FRemoteSessionInputChannel::StaticType(), ERemoteSessionChannelMode::Read, FOnRemoteSessionChannelCreated::CreateUObject(this, &AVirtualCameraActor::OnInputChannelCreated));
		SupportedChannels.Emplace(FRemoteSessionImageChannel::StaticType(), ERemoteSessionChannelMode::Write, FOnRemoteSessionChannelCreated::CreateUObject(this, &AVirtualCameraActor::OnImageChannelCreated));

		RemoteSessionHost = RemoteSession->CreateHost(MoveTemp(SupportedChannels), RemoteSessionPort);
		if (RemoteSessionHost)
		{
			RemoteSessionHost->Tick(0.0f);
		}
	}

	SetActorTickEnabled(true);

	bIsStreaming = true;
	return true;
}

bool AVirtualCameraActor::StopStreaming()
{
	TSharedPtr<FRemoteSessionInputChannel> InputChannel = RemoteSessionHost->GetChannel<FRemoteSessionInputChannel>();
	if (InputChannel)
	{
		InputChannel->GetOnRouteTouchDownToWidgetFailedDelegate()->RemoveAll(this);
	}

	RemoteSessionHost.Reset();

	CameraScreenWidget->Hide();
	if (MediaCapture)
	{
		MediaCapture->StopCapture(true);
	}

#if WITH_EDITOR
	if (ActorWorld && ActorWorld->WorldType == EWorldType::Editor)
	{
		FLevelEditorModule& LevelEditorModule = FModuleManager::GetModuleChecked<FLevelEditorModule>(LevelEditorName);
		TSharedPtr<SLevelViewport> ActiveLevelViewport = LevelEditorModule.GetFirstActiveLevelViewport();
		if (ActiveLevelViewport.IsValid())
		{
			// restore FOV
			GCurrentLevelEditingViewportClient->ViewFOV = GCurrentLevelEditingViewportClient->FOVAngle;

			FLevelEditorViewportClient& LevelViewportClient = ActiveLevelViewport->GetLevelViewportClient();
			LevelViewportClient.SetActorLock(ViewportSettingsBackup->ActorLock.Get());
			GCurrentLevelEditingViewportClient->UpdateViewForLockedActor();

			// remove roll and pitch from camera when unbinding from actors
			GEditor->RemovePerspectiveViewRotation(true, true, false);

			LevelViewportClient.SetRealtime(ViewportSettingsBackup->bRealTime);
			LevelViewportClient.bDrawAxes = ViewportSettingsBackup->bDrawAxes;
			LevelViewportClient.bDisableInput = ViewportSettingsBackup->bDisableInput;
			LevelViewportClient.SetAllowCinematicControl(ViewportSettingsBackup->bAllowCinematicControl);

			// unlock viewport resize
			ActiveLevelViewport->GetSharedActiveViewport()->SetFixedViewportSize(0, 0);

			// remove event listeners
			FEditorDelegates::OnAssetsCanDelete.RemoveAll(this);
			LevelEditorModule.OnMapChanged().RemoveAll(this);
			if (FAssetRegistryModule* AssetRegistryModule = FModuleManager::GetModulePtr<FAssetRegistryModule>(AssetRegistryName))
			{
				AssetRegistryModule->Get().OnAssetRemoved().RemoveAll(this);
			}
			FEditorSupportDelegates::PrepareToCleanseEditorObject.RemoveAll(this);
			GEditor->OnBlueprintPreCompile().RemoveAll(this);
		}

		ViewportSettingsBackup.Reset();
	}
	else
#endif
	{
		if (PreviousViewTarget)
		{
			APlayerController* PlayerController = ActorWorld->GetGameInstance()->GetFirstLocalPlayerController();
			if (!PlayerController)
			{
				return false;
			}
			FViewTargetTransitionParams TransitionParams;
			PlayerController->SetViewTarget(PreviousViewTarget, TransitionParams);
		}
	}

	SetActorTickEnabled(false);

	bIsStreaming = false;

	if (bSaveSettingsOnStopStreaming)
	{
		SaveSettings();
	}

	return true;
}

UWorld* AVirtualCameraActor::GetControllerWorld()
{
	return GetWorld();
}

UCineCameraComponent* AVirtualCameraActor::GetStreamedCameraComponent_Implementation() const
{
	return StreamedCamera;
}

UCineCameraComponent* AVirtualCameraActor::GetRecordingCameraComponent_Implementation() const
{
	return RecordingCamera;
}

UCineCameraComponent* AVirtualCameraActor::GetActiveCameraComponent_Implementation() const
{
	return GetActiveCameraComponentInternal();
}

ULevelSequencePlaybackController* AVirtualCameraActor::GetSequenceController_Implementation() const
{
	UVirtualCameraSubsystem* SubSystem = GEngine->GetEngineSubsystem<UVirtualCameraSubsystem>();
	return SubSystem ? SubSystem->SequencePlaybackController : nullptr;
}

TScriptInterface<IVirtualCameraPresetContainer> AVirtualCameraActor::GetPresetContainer_Implementation()
{
	return this;
}

TScriptInterface<IVirtualCameraOptions> AVirtualCameraActor::GetOptions_Implementation()
{
	return this;
}

FLiveLinkSubjectRepresentation AVirtualCameraActor::GetLiveLinkRepresentation_Implementation() const
{
	return LiveLinkSubject;
}

void AVirtualCameraActor::SetLiveLinkRepresentation_Implementation(const FLiveLinkSubjectRepresentation& InLiveLinkRepresentation)
{
	LiveLinkSubject = InLiveLinkRepresentation;
}

FString AVirtualCameraActor::SavePreset_Implementation(const bool bSaveCameraSettings, const bool bSaveStabilization, const bool bSaveAxisLocking, const bool bSaveMotionScale)
{
	// Convert index to string with leading zeros
	FString PresetName = FString::Printf(TEXT("Preset-%03i"), PresetIndex);

	// Another preset has been created
	PresetIndex++;
	FVirtualCameraSettingsPreset::NextIndex++;

	FVirtualCameraSettingsPreset PresetToAdd;
	PresetToAdd.DateCreated = FDateTime::UtcNow();

	PresetToAdd.bIsCameraSettingsSaved = bSaveCameraSettings;
	PresetToAdd.bIsStabilizationSettingsSaved = bSaveStabilization;
	PresetToAdd.bIsAxisLockingSettingsSaved = bSaveAxisLocking;
	PresetToAdd.bIsMotionScaleSettingsSaved = bSaveMotionScale;

	if (StreamedCamera)
	{
		PresetToAdd.CameraSettings.FocalLength = StreamedCamera->CurrentFocalLength;
		PresetToAdd.CameraSettings.Aperture = StreamedCamera->CurrentAperture;
		PresetToAdd.CameraSettings.FilmbackWidth = StreamedCamera->Filmback.SensorWidth;
		PresetToAdd.CameraSettings.FilmbackHeight = StreamedCamera->Filmback.SensorHeight;
	}

	SettingsPresets.Add(PresetName, PresetToAdd);

	return PresetName;
}

bool AVirtualCameraActor::LoadPreset_Implementation(const FString& PresetName)
{
	FVirtualCameraSettingsPreset* LoadedPreset = SettingsPresets.Find(PresetName);

	if (LoadedPreset)
	{
		if (StreamedCamera)
		{
			if (LoadedPreset->bIsCameraSettingsSaved)
			{
				StreamedCamera->CurrentAperture = LoadedPreset->CameraSettings.Aperture;
				StreamedCamera->CurrentFocalLength = LoadedPreset->CameraSettings.FocalLength;
				StreamedCamera->Filmback.SensorWidth = LoadedPreset->CameraSettings.FilmbackWidth;
				StreamedCamera->Filmback.SensorHeight = LoadedPreset->CameraSettings.FilmbackHeight;
			}
		}
		return true;
	}

	return false;
}

int32 AVirtualCameraActor::DeletePreset_Implementation(const FString& PresetName)
{
	return SettingsPresets.Remove(PresetName);
}

TMap<FString, FVirtualCameraSettingsPreset> AVirtualCameraActor::GetSettingsPresets_Implementation()
{
	SettingsPresets.KeySort([](const FString& a, const FString& b) -> bool
	{
		return a < b;
	});

	return SettingsPresets;
}

void AVirtualCameraActor::SetDesiredDistanceUnits_Implementation(const EUnit InDesiredUnits)
{
	DesiredDistanceUnits = InDesiredUnits;
}

EUnit AVirtualCameraActor::GetDesiredDistanceUnits_Implementation()
{
	return DesiredDistanceUnits;
}

bool AVirtualCameraActor::IsFocusVisualizationAllowed_Implementation()
{
	return bAllowFocusVisualization;
}

void AVirtualCameraActor::OnImageChannelCreated(TWeakPtr<IRemoteSessionChannel> Instance, const FString& Type, ERemoteSessionChannelMode Mode)
{
	TSharedPtr<FRemoteSessionImageChannel> ImageChannel = StaticCastSharedPtr<FRemoteSessionImageChannel>(Instance.Pin());
	if (ImageChannel)
	{
		ImageChannel->SetImageProvider(nullptr);
		MediaOutput->SetImageChannel(ImageChannel);
		MediaCapture = Cast<URemoteSessionMediaCapture>(MediaOutput->CreateMediaCapture());

		TWeakPtr<SWindow> InputWindow;
		TWeakPtr<FSceneViewport> SceneViewport;
		FindSceneViewport(InputWindow, SceneViewport);
		if (TSharedPtr<FSceneViewport> PinnedSceneViewport = SceneViewport.Pin())
		{
			MediaCapture->CaptureSceneViewport(PinnedSceneViewport, FMediaCaptureOptions());
		}
	}
}

void AVirtualCameraActor::OnInputChannelCreated(TWeakPtr<IRemoteSessionChannel> Instance, const FString& Type, ERemoteSessionChannelMode Mode)
{
	TSharedPtr<FRemoteSessionInputChannel> InputChannel = StaticCastSharedPtr<FRemoteSessionInputChannel>(Instance.Pin());
	if (InputChannel)
	{
		TSharedPtr<SVirtualWindow> InputWindow = CameraScreenWidget->PostProcessDisplayType.GetSlateWindow();
		InputChannel->SetPlaybackWindow(InputWindow, nullptr);
		InputChannel->TryRouteTouchMessageToWidget(true);
		InputChannel->GetOnRouteTouchDownToWidgetFailedDelegate()->AddUObject(this, &AVirtualCameraActor::OnTouchEventOutsideUMG);
	}
}

void AVirtualCameraActor::OnTouchEventOutsideUMG(const FVector2D& InViewportPosition)
{
	FVector TraceDirection;
	FVector CameraWorldLocation;
	if (!DeprojectScreenToWorld(InViewportPosition, CameraWorldLocation, TraceDirection))
	{
		return;
	}

	FCollisionQueryParams TraceParams(SCENE_QUERY_STAT(UpdateAutoFocus), true);

	const FVector TraceEnd = CameraWorldLocation + TraceDirection * MaxFocusTraceDistance;
	const bool bHit = GetWorld()->LineTraceSingleByChannel(LastViewportTouchResult, CameraWorldLocation, TraceEnd, ECC_Visibility, TraceParams);

	if (bHit)
	{
		FEditorScriptExecutionGuard ScriptGuard;
		OnActorClickedDelegate.ExecuteIfBound(LastViewportTouchResult);
	}
}

void AVirtualCameraActor::SaveSettings()
{
	if (!StreamedCamera)
	{
		return;
	}

	UVirtualCameraSaveGame* SaveGameInstance = Cast<UVirtualCameraSaveGame>(UGameplayStatics::CreateSaveGameObject(UVirtualCameraSaveGame::StaticClass()));

	// Save focal length and aperture
	SaveGameInstance->CameraSettings.FocalLength = StreamedCamera->CurrentFocalLength;
	SaveGameInstance->CameraSettings.Aperture = StreamedCamera->CurrentAperture;
	SaveGameInstance->CameraSettings.bAllowFocusVisualization = bAllowFocusVisualization;
	SaveGameInstance->CameraSettings.DebugFocusPlaneColor = StreamedCamera->FocusSettings.DebugFocusPlaneColor;

	// Save filmback settings
	SaveGameInstance->CameraSettings.FilmbackName = StreamedCamera->GetFilmbackPresetName();
	SaveGameInstance->CameraSettings.FilmbackWidth = StreamedCamera->Filmback.SensorWidth;
	SaveGameInstance->CameraSettings.FilmbackHeight = StreamedCamera->Filmback.SensorHeight;

	// Save settings presets
	SaveGameInstance->SettingsPresets = SettingsPresets;

	// Save indices for naming
	SaveGameInstance->WaypointIndex = FVirtualCameraWaypoint::NextIndex;
	SaveGameInstance->ScreenshotIndex = FVirtualCameraScreenshot::NextIndex;
	SaveGameInstance->PresetIndex = FVirtualCameraSettingsPreset::NextIndex;

	SaveGameInstance->CameraSettings.DesiredDistanceUnits = DesiredDistanceUnits;

	// Write save file to disk
	UGameplayStatics::SaveGameToSlot(SaveGameInstance, SavedSettingsSlotName, 0);
}

void AVirtualCameraActor::LoadSettings()
{
	if (!StreamedCamera)
	{
		return;
	}

	UVirtualCameraSaveGame* SaveGameInstance = Cast<UVirtualCameraSaveGame>(UGameplayStatics::CreateSaveGameObject(UVirtualCameraSaveGame::StaticClass()));
	SaveGameInstance = Cast<UVirtualCameraSaveGame>(UGameplayStatics::LoadGameFromSlot(SavedSettingsSlotName, 0));

	if (!SaveGameInstance)
	{
		UE_LOG(LogVirtualCamera, Warning, TEXT("VirtualCamera could not find save game to load, using default settings."))
		return;
	}

	bAllowFocusVisualization = SaveGameInstance->CameraSettings.bAllowFocusVisualization;

	if (SaveGameInstance->CameraSettings.DebugFocusPlaneColor != FColor())
	{
		StreamedCamera->FocusSettings.DebugFocusPlaneColor = SaveGameInstance->CameraSettings.DebugFocusPlaneColor;
	}

	StreamedCamera->SetCurrentFocalLength(SaveGameInstance->CameraSettings.FocalLength);
	StreamedCamera->CurrentAperture = SaveGameInstance->CameraSettings.Aperture;
	StreamedCamera->Filmback.SensorWidth = SaveGameInstance->CameraSettings.FilmbackWidth;
	StreamedCamera->Filmback.SensorHeight = SaveGameInstance->CameraSettings.FilmbackHeight;

	DesiredDistanceUnits = SaveGameInstance->CameraSettings.DesiredDistanceUnits;

	// load presets, but don't overwrite existing ones
	SettingsPresets.Append(SaveGameInstance->SettingsPresets);

	// If the saved preset index is smaller than total presets, set it so that it won't overwrite existing presets.
	FVirtualCameraSettingsPreset::NextIndex = SaveGameInstance->PresetIndex;
	if (SettingsPresets.Num() > FVirtualCameraSettingsPreset::NextIndex)
	{
		FVirtualCameraSettingsPreset::NextIndex = SettingsPresets.Num();
	}

	PresetIndex = FVirtualCameraSettingsPreset::NextIndex;
}

UCineCameraComponent* AVirtualCameraActor::GetActiveCameraComponentInternal() const
{
	return StreamedCamera;
}

void AVirtualCameraActor::SetRelativeTransformInternal(const FTransform& InRelativeTransform)
{
	MovementComponent->SetLocalTransform(InRelativeTransform);
	const FTransform ModifiedTransform = MovementComponent->GetTransform();
	SceneOffset->SetRelativeTransform(ModifiedTransform);
}

void AVirtualCameraActor::UpdateAutoFocus()
{
	FVector TraceDirection;
	FVector CameraWorldLocation;
	if (!DeprojectScreenToWorld(ReticlePosition, CameraWorldLocation, TraceDirection))
	{
		return;
	}

	FCollisionQueryParams TraceParams(SCENE_QUERY_STAT(UpdateAutoFocus), true);

	const FVector TraceEnd = CameraWorldLocation + TraceDirection * MaxFocusTraceDistance;
	FHitResult Hit;
	const bool bHit = GetWorld()->LineTraceSingleByChannel(Hit, CameraWorldLocation, TraceEnd, ECC_Visibility, TraceParams);

	//we don't want to set a focus distance bigger than hyperfocal distance
	float focusDistance = (bHit && Hit.Distance < HyperfocalDistance) ? Hit.Distance : HyperfocalDistance;

	FEditorScriptExecutionGuard ScriptGuard;
	Execute_SetFocusDistance(this, focusDistance);
}

#if WITH_EDITOR
void AVirtualCameraActor::OnMapChanged(UWorld* World, EMapChangeType ChangeType)
{
	if (World == ActorWorld && ChangeType == EMapChangeType::TearDownWorld)
	{
		StopStreaming();
	}
}

void AVirtualCameraActor::OnBlueprintPreCompile(UBlueprint* Blueprint)
{
	if (Blueprint && CameraUMGClass && Blueprint->GeneratedClass == CameraUMGClass)
	{
		StopStreaming();
	}
}

void AVirtualCameraActor::OnPrepareToCleanseEditorObject(UObject* Object)
{
	if (Object == CameraScreenWidget || Object == CameraUMGClass || Object == ActorWorld || Object == MediaCapture)
	{
		StopStreaming();
	}
}

void AVirtualCameraActor::OnAssetRemoved(const FAssetData& AssetData)
{
	if (AssetData.GetPackage() == CameraUMGClass->GetOutermost())
	{
		StopStreaming();
	}
}

void AVirtualCameraActor::OnAssetsCanDelete(const TArray<UObject*>& InAssetsToDelete, FCanDeleteAssetResult& CanDeleteResult)
{
	if (CameraUMGClass)
	{
		for (UObject* Obj : InAssetsToDelete)
		{
			if (CameraUMGClass->GetOutermost() == Obj->GetOutermost())
			{
				UE_LOG(LogVirtualCamera, Warning, TEXT("Asset '%s' can't be deleted because it is currently used by the Virtual Camera Stream."), *Obj->GetPathName());
				CanDeleteResult.Set(false);
				break;
			}
		}
	}
}
#endif
