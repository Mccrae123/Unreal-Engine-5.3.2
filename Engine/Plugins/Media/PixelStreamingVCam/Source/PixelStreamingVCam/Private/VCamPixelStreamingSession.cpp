// Copyright Epic Games, Inc. All Rights Reserved.

#include "VCamPixelStreamingSession.h"
#include "VCamOutputComposure.h"
#include "PixelStreamingServers.h"
#include "Editor/EditorPerformanceSettings.h"
#include "Containers/UnrealString.h"
#include "Async/Async.h"
#include "Modules/ModuleManager.h"
#include "Math/Vector4.h"
#include "Math/Matrix.h"
#include "Math/TransformNonVectorized.h"
#include "Serialization/MemoryReader.h"
#include "VCamPixelStreamingSubsystem.h"
#include "VCamPixelStreamingLiveLink.h"
#include "PixelStreamingVCamLog.h"
#include "IPixelStreamingModule.h"
#include "PixelStreamingProtocol.h"

namespace VCamPixelStreamingSession
{
	static const FName LevelEditorName(TEXT("LevelEditor"));
	static const FSoftClassPath EmptyUMGSoftClassPath(TEXT("/VCamCore/Assets/VCam_EmptyVisibleUMG.VCam_EmptyVisibleUMG_C"));
} // namespace VCamPixelStreamingSession

void UVCamPixelStreamingSession::Initialize()
{
	Super::Initialize();
}

void UVCamPixelStreamingSession::Deinitialize()
{
	if (MediaOutput)
	{
		MediaOutput->ConditionalBeginDestroy();
	}
	MediaOutput = nullptr;
	Super::Deinitialize();
}

void UVCamPixelStreamingSession::Activate()
{
	if (UVCamPixelStreamingSubsystem* PixelStreamingSubsystem = UVCamPixelStreamingSubsystem::Get())
	{
		PixelStreamingSubsystem->RegisterActiveOutputProvider(this);
	}

	// If we don't have a UMG assigned, we still need to create an empty 'dummy' UMG in order to properly route the input back from the RemoteSession device
	if (UMGClass == nullptr)
	{
		bUsingDummyUMG = true;
		UMGClass = VCamPixelStreamingSession::EmptyUMGSoftClassPath.TryLoadClass<UUserWidget>();
	}

	if (!bInitialized)
	{
		UE_LOG(LogPixelStreamingVCam, Warning, TEXT("Trying to start Pixel Streaming, but has not been initialized yet"));
		SetActive(false);
		return;
	}

	if (MediaOutput == nullptr)
	{
		MediaOutput = NewObject<UPixelStreamingMediaOutput>(GetTransientPackage(), UPixelStreamingMediaOutput::StaticClass());
	}

	UEditorPerformanceSettings* Settings = GetMutableDefault<UEditorPerformanceSettings>();
	bOldThrottleCPUWhenNotForeground = Settings->bThrottleCPUWhenNotForeground;
	if (PreventEditorIdle)
	{
		Settings->bThrottleCPUWhenNotForeground = false;
		Settings->PostEditChange();
	}

	if (StartSignallingServer)
	{
		if (UVCamPixelStreamingSubsystem* PixelStreamingSubsystem = UVCamPixelStreamingSubsystem::Get())
		{
			PixelStreamingSubsystem->LaunchSignallingServer(PortNumber, HttpPort);
		}
	}

	MediaOutput->SetSignallingServerURL(FString::Printf(TEXT("%s:%s"), *IP, *FString::FromInt(PortNumber)));
	UE_LOG(LogPixelStreamingVCam, Log, TEXT("Activating PixelStreaming VCam Session. Endpoint: %s:%s"), *IP, *FString::FromInt(PortNumber));

	MediaCapture = Cast<UPixelStreamingMediaCapture>(MediaOutput->CreateMediaCapture());

	FMediaCaptureOptions Options;
	Options.bResizeSourceBuffer = true;

	// If we are rendering from a ComposureOutputProvider, get the requested render target and use that instead of the viewport
	if (UVCamOutputComposure* ComposureProvider = Cast<UVCamOutputComposure>(GetOtherOutputProviderByIndex(FromComposureOutputProviderIndex)))
	{
		if (ComposureProvider->FinalOutputRenderTarget)
		{
			MediaCapture->CaptureTextureRenderTarget2D(ComposureProvider->FinalOutputRenderTarget, Options);
			UE_LOG(LogPixelStreamingVCam, Log, TEXT("PixelStreaming set with ComposureRenderTarget"));
		}
		else
		{
			UE_LOG(LogPixelStreamingVCam, Warning, TEXT("PixelStreaming Composure usage was requested, but the specified ComposureOutputProvider has no FinalOutputRenderTarget set"));
		}
	}
	else
	{
		TWeakPtr<FSceneViewport> SceneViewport = GetTargetSceneViewport();
		if (TSharedPtr<FSceneViewport> PinnedSceneViewport = SceneViewport.Pin())
		{
			MediaCapture->CaptureSceneViewport(PinnedSceneViewport, Options);
			UE_LOG(LogPixelStreamingVCam, Log, TEXT("PixelStreaming set with viewport"));
		}
	}

	if(MediaOutput->GetStreamer())
	{
		IPixelStreamingModule& PixelStreamingModule = IPixelStreamingModule::Get();
		
		Protocol::EPixelStreamingMessageDirection MessageDirection = Protocol::EPixelStreamingMessageDirection::ToStreamer;

		typedef Protocol::EPixelStreamingMessageTypes EType;
		Protocol::FPixelStreamingInputMessage Message = Protocol::FPixelStreamingInputMessage(100, 72, {
			// 4x4 Transform
			EType::Float, EType::Float, EType::Float, EType::Float,
			EType::Float, EType::Float, EType::Float, EType::Float,
			EType::Float, EType::Float, EType::Float, EType::Float,
			EType::Float, EType::Float, EType::Float, EType::Float,
			// Timestamp
			EType::Double
		});

		const TFunction<void(FMemoryReader)>& Handler = [this](FMemoryReader Ar) { 
			// The buffer contains the transform matrix stored as 16 floats
			FMatrix ARKitMatrix;
			for (int32 Row = 0; Row < 4; ++Row)
			{
				float Col0, Col1, Col2, Col3;
				Ar << Col0 << Col1 << Col2 << Col3;
				ARKitMatrix.M[Row][0] = Col0;
				ARKitMatrix.M[Row][1] = Col1;
				ARKitMatrix.M[Row][2] = Col2;
				ARKitMatrix.M[Row][3] = Col3;
			}
			ARKitMatrix.DiagnosticCheckNaN();

			// Extract timestamp
			double Timestamp;
			Ar << Timestamp;


			/**
			 * Code to control the level editor viewport
			 */
			// for (FLevelEditorViewportClient* LevelVC : GEditor->GetLevelViewportClients())
			// {
			// 	if (LevelVC && LevelVC->IsPerspective())
			// 	{
			// 		LevelVC->SetViewLocation(Translation);
			// 		LevelVC->SetViewRotation(ModifiedRotation);
			// 	}
			// }
		};

		PixelStreamingModule.RegisterMessage(MessageDirection, "ARKitTransform", Message, Handler);
	}
	Super::Activate();
}

void UVCamPixelStreamingSession::StopSignallingServer()
{
	if (UVCamPixelStreamingSubsystem* PixelStreamingSubsystem = UVCamPixelStreamingSubsystem::Get())
	{
		PixelStreamingSubsystem->StopSignallingServer();
	}
}

void UVCamPixelStreamingSession::Deactivate()
{
	if (UVCamPixelStreamingSubsystem* PixelStreamingSubsystem = UVCamPixelStreamingSubsystem::Get())
	{
		PixelStreamingSubsystem->UnregisterActiveOutputProvider(this);
	}

	if (MediaCapture)
	{

		if (MediaOutput && MediaOutput->GetStreamer())
		{
			// Shutting streamer down before closing signalling server prevents an ugly websocket disconnect showing in the log
			MediaOutput->GetStreamer()->StopStreaming();
			StopSignallingServer();
		}

		MediaCapture->StopCapture(true);
		MediaCapture = nullptr;
	}
	else
	{
		// There is not media capture we defensively clean up the signalling server if it exists.
		StopSignallingServer();
	}

	Super::Deactivate();
	if (bUsingDummyUMG)
	{
		UMGClass = nullptr;
		bUsingDummyUMG = false;
	}

	UEditorPerformanceSettings* Settings = GetMutableDefault<UEditorPerformanceSettings>();
	Settings->bThrottleCPUWhenNotForeground = bOldThrottleCPUWhenNotForeground;
	Settings->PostEditChange();
}

void UVCamPixelStreamingSession::Tick(const float DeltaTime)
{
	Super::Tick(DeltaTime);
}

#if WITH_EDITOR
void UVCamPixelStreamingSession::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	FProperty* Property = PropertyChangedEvent.MemberProperty;
	if (Property && PropertyChangedEvent.ChangeType != EPropertyChangeType::Interactive)
	{
		static FName NAME_IP = GET_MEMBER_NAME_CHECKED(UVCamPixelStreamingSession, IP);
		static FName NAME_PortNumber = GET_MEMBER_NAME_CHECKED(UVCamPixelStreamingSession, PortNumber);
		static FName NAME_FromComposureOutputProviderIndex = GET_MEMBER_NAME_CHECKED(UVCamPixelStreamingSession, FromComposureOutputProviderIndex);
		static FName NAME_StartSignallingServer = GET_MEMBER_NAME_CHECKED(UVCamPixelStreamingSession, StartSignallingServer);
		
		// If any of these properties change then deactivate streaming so user is forced to restart streaming
		if ((Property->GetFName() == NAME_IP) || 
			(Property->GetFName() == NAME_PortNumber) || 
			(Property->GetFName() == NAME_FromComposureOutputProviderIndex) || 
			(Property->GetFName() == NAME_StartSignallingServer))
		{
			SetActive(false);
		}
	}
	Super::PostEditChangeProperty(PropertyChangedEvent);
}
#endif

IMPLEMENT_MODULE(FDefaultModuleImpl, PixelStreamingVCam)
