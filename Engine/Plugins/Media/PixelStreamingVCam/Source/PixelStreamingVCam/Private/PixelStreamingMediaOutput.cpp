// Copyright Epic Games, Inc. All Rights Reserved.

#include "PixelStreamingMediaOutput.h"
#include "PixelStreamingMediaCapture.h"
#include "IPixelStreamingModule.h"
#include "PixelStreamingPlayerId.h"
#include "IPixelStreamingStreamer.h"
#include "CineCameraComponent.h"
#include "PixelStreamingEditorModule.h"
#include "PixelStreamingEditorUtils.h"

void UPixelStreamingMediaOutput::BeginDestroy()
{
	StopStreaming();
	Streamer = nullptr;
	Super::BeginDestroy();
}

UMediaCapture* UPixelStreamingMediaOutput::CreateMediaCaptureImpl()
{
	if (!Streamer.IsValid())
	{
		IPixelStreamingModule& Module = FModuleManager::LoadModuleChecked<IPixelStreamingModule>("PixelStreaming");
		Streamer = Module.GetStreamer(Module.GetDefaultStreamerID());
	}

	Capture = nullptr;
	if (Streamer.IsValid())
	{
		Capture = NewObject<UPixelStreamingMediaCapture>();
		Capture->SetMediaOutput(this);
		Capture->OnStateChangedNative.AddUObject(this, &UPixelStreamingMediaOutput::OnCaptureStateChanged);
		Capture->OnCaptureViewportInitialized.AddUObject(this, &UPixelStreamingMediaOutput::OnCaptureViewportInitialized);
	}
	return Capture;
}

void UPixelStreamingMediaOutput::OnCaptureStateChanged()
{
	switch (Capture->GetState())
	{
		case EMediaCaptureState::Capturing:
			StartStreaming();
			break;
		case EMediaCaptureState::Stopped:
		case EMediaCaptureState::Error:
			StopStreaming();
			break;
		default:
			break;
	}
}

void UPixelStreamingMediaOutput::OnCaptureViewportInitialized()
{
	if(Streamer)
	{
		Streamer->SetTargetViewport(Capture->GetViewport()->GetViewportWidget());
	}
}

void UPixelStreamingMediaOutput::StartStreaming()
{
	if (Streamer)
	{
		FPixelStreamingEditorModule::GetModule()->SetStreamType(UE::EditorPixelStreaming::EStreamTypes::VCam);
		Streamer->SetVideoInput(Capture->GetVideoInput());

		if (!Streamer->IsStreaming())
		{
			Streamer->StartStreaming();
		}
	}
}

void UPixelStreamingMediaOutput::StopStreaming()
{
	if (Streamer)
	{
		Streamer->StopStreaming();
		Streamer->SetTargetViewport(nullptr);
		Streamer->SetTargetWindow(nullptr);
	}
}

void UPixelStreamingMediaOutput::SetSignallingServerURL(FString InURL)
{
	SignallingServerURL = InURL;
}

void UPixelStreamingMediaOutput::SetSignallingStreamID(FString InStreamID)
{
	StreamID = InStreamID;
}