// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "IPixelStreamingModule.h"
#include "RHI.h"
#include "Tickable.h"
#include "InputDevice.h"
#include "StreamerInputDevices.h"

class UPixelStreamingInput;
class SWindow;

namespace UE::PixelStreaming
{
	class FStreamer;
	class FVideoInputBackBuffer;
	class FVideoSourceGroup;

	/*
	 * This plugin allows the back buffer to be sent as a compressed video across a network.
	 */
	class FPixelStreamingModule : public IPixelStreamingModule
	{
	public:
		static IPixelStreamingModule* GetModule();

		/** IPixelStreamingModule implementation */
		virtual FReadyEvent& OnReady() override;
		virtual bool IsReady() override;
		virtual bool StartStreaming() override;
		virtual void StopStreaming() override;
		virtual TSharedPtr<IPixelStreamingStreamer> CreateStreamer(const FString& StreamerId) override;
		virtual TArray<FString> GetStreamerIds() override;
		virtual TSharedPtr<IPixelStreamingStreamer> GetStreamer(const FString& StreamerId) override;
		virtual TSharedPtr<IPixelStreamingStreamer> DeleteStreamer(const FString& StreamerId) override;
		virtual FString GetDefaultStreamerID() override;
		// These are staying on the module at the moment as theres no way of the BPs knowing which streamer they are relevant to
		virtual void AddInputComponent(UPixelStreamingInput* InInputComponent) override;
		virtual void RemoveInputComponent(UPixelStreamingInput* InInputComponent) override;
		virtual const TArray<UPixelStreamingInput*> GetInputComponents() override;
		// Don't delete, is used externally to PS
		virtual rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> CreateExternalVideoSource() override;
		virtual void ReleaseExternalVideoSource(const webrtc::VideoTrackSourceInterface* InVideoSource) override;
		virtual webrtc::VideoEncoderFactory* CreateVideoEncoderFactory() override;
		virtual void ForEachStreamer(const TFunction<void(TSharedPtr<IPixelStreamingStreamer>)>& Func) override;
		/** End IPixelStreamingModule implementation */

		virtual void RegisterCreateInputDevice(IPixelStreamingInputDevice::FCreateInputDeviceFunc& InCreateInputDevice) override;

	private:
		/** IModuleInterface implementation */
		void StartupModule() override;
		void ShutdownModule() override;
		/** End IModuleInterface implementation */

		// Own methods
		void InitDefaultStreamer();
		bool IsPlatformCompatible() const;

		virtual TSharedPtr<IInputDevice> CreateInputDevice(const TSharedRef<FGenericApplicationMessageHandler>& InMessageHandler) override;

	private:
		bool bModuleReady = false;
		static IPixelStreamingModule* PixelStreamingModule;

		FReadyEvent ReadyEvent;

		TArray<UPixelStreamingInput*> InputComponents;
		TUniquePtr<FVideoSourceGroup> ExternalVideoSourceGroup;

		mutable FCriticalSection StreamersCS;
		TMap<FString, TSharedPtr<IPixelStreamingStreamer>> Streamers;

		TSharedPtr<FStreamerInputDevices> StreamerInputDevices;
	};
} // namespace UE::PixelStreaming
