// Copyright Epic Games, Inc. All Rights Reserved.

#include "PixelStreamingMessageHandler.h"
#include "InputStructures.h"
#include "PixelStreamingProtocol.h"
#include "PixelStreamingModule.h"
#include "JavaScriptKeyCodes.inl"
#include "Settings.h"
#include "Layout/ArrangedChildren.h"
#include "Layout/WidgetPath.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SViewport.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Utils.h"
#include "PixelStreamingInputComponent.h"
#include "Engine/Engine.h"
#include "Framework/Application/SlateUser.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Misc/CoreMiscDefines.h"

DECLARE_LOG_CATEGORY_EXTERN(LogPixelStreamingMessageHandler, Log, VeryVerbose);
DEFINE_LOG_CATEGORY(LogPixelStreamingMessageHandler);

// TODO: Gesture recognition is moving to the browser, so add handlers for the gesture events.
// The gestures supported will be swipe, pinch,

namespace UE::PixelStreaming
{
    FPixelStreamingMessageHandler::FPixelStreamingMessageHandler(TSharedPtr<FPixelStreamingApplicationWrapper> InApplicationWrapper, const TSharedPtr<FGenericApplicationMessageHandler>& InTargetHandler)
    : TargetViewport(nullptr)
    , NumActiveTouches(0)
    , bIsMouseActive(false)
    , MessageHandler(InTargetHandler)
    , PixelStreamingModule(FPixelStreamingModule::GetModule())
    , PixelStreamerApplicationWrapper(InApplicationWrapper)
    , FocusedPos(FVector2D(-1.0f, -1.0f))
    , UnfocusedPos(FVector2D(-1.0f, -1.0f))
    {
        RegisterHandler("KeyPress", [this](FMemoryReader Ar) { HandleOnKeyChar(Ar); });
        RegisterHandler("KeyUp", [this](FMemoryReader Ar) { HandleOnKeyUp(Ar); });
        RegisterHandler("KeyDown", [this](FMemoryReader Ar) { HandleOnKeyDown(Ar); });

        RegisterHandler("TouchStart", [this](FMemoryReader Ar) { HandleOnTouchStarted(Ar); });
        RegisterHandler("TouchMove", [this](FMemoryReader Ar) { HandleOnTouchMoved(Ar); });
        RegisterHandler("TouchEnd", [this](FMemoryReader Ar) { HandleOnTouchEnded(Ar); });

        RegisterHandler("GamepadAnalog", [this](FMemoryReader Ar) { HandleOnControllerAnalog(Ar); });
        RegisterHandler("GamepadButtonPressed", [this](FMemoryReader Ar) { HandleOnControllerButtonPressed(Ar); });
        RegisterHandler("GamepadButtonReleased", [this](FMemoryReader Ar) { HandleOnControllerButtonReleased(Ar); });

        RegisterHandler("MouseEnter", [this](FMemoryReader Ar) { HandleOnMouseEnter(Ar); });
        RegisterHandler("MouseLeave", [this](FMemoryReader Ar) { HandleOnMouseLeave(Ar); });
        RegisterHandler("MouseUp", [this](FMemoryReader Ar) { HandleOnMouseUp(Ar); });
        RegisterHandler("MouseDown", [this](FMemoryReader Ar) { HandleOnMouseDown(Ar); });
        RegisterHandler("MouseMove", [this](FMemoryReader Ar) { HandleOnMouseMove(Ar); });
        RegisterHandler("MouseWheel", [this](FMemoryReader Ar) { HandleOnMouseWheel(Ar); });
		RegisterHandler("MouseDouble", [this](FMemoryReader Ar) { HandleOnMouseDoubleClick(Ar); });

        RegisterHandler("Command", [this](FMemoryReader Ar) { HandleCommand(Ar); });
        RegisterHandler("UIInteraction", [this](FMemoryReader Ar) { HandleUIInteraction(Ar); });
    }

    FPixelStreamingMessageHandler::~FPixelStreamingMessageHandler()
    {
    }

    void FPixelStreamingMessageHandler::RegisterHandler(const FString& MessageType, const TFunction<void(FMemoryReader)>& Handler)
    {
        TMap<FString, Protocol::FPixelStreamingInputMessage> Protocol = PixelStreamingModule->GetProtocol().ToStreamerProtocol;
        DispatchTable.FindOrAdd(Protocol.Find(MessageType)->Id, Handler);
    }

    void FPixelStreamingMessageHandler::Tick(const float InDeltaTime)
    {
        FMessage Message;
		while (Messages.Dequeue(Message))
		{
            FMemoryReader Ar(Message.Data);
            (*Message.Handler)(Ar);
        }
    }

	void FPixelStreamingMessageHandler::OnMessage(const webrtc::DataBuffer& Buffer)
	{
		using namespace Protocol;

		const uint8* Data = Buffer.data.data();
		uint32 Size = (uint32)Buffer.data.size();
        if(sizeof(uint8) > Size)
        {
            UE_LOG(LogPixelStreamingMessageHandler, Warning, TEXT("Buffer size is too small to extract message type. Buffer size (bytes): %d"), Size);
            return;
        }
		const uint8 MsgType = *Data;
		Data += sizeof(uint8);
		Size -= sizeof(uint8);

        TFunction<void(FMemoryReader)>* Handler = DispatchTable.Find(MsgType);
        if (Handler != nullptr)
        {
            TArray<uint8> MessageData(Data, Size);
            FMessage Message = {
                Handler,   // The function to call
                MessageData // The message data
            };
            Messages.Enqueue(Message);
        }
        else
        {
            UE_LOG(LogPixelStreamingMessageHandler, Warning, TEXT("No handler registered for message with id %d"), MsgType);
        }
    }

	void FPixelStreamingMessageHandler::SetTargetWindow(TWeakPtr<SWindow> InWindow)
	{
		TargetWindow = InWindow;
	}

	TWeakPtr<SWindow> FPixelStreamingMessageHandler::GetTargetWindow()
	{
		return TargetWindow;
	}

	void FPixelStreamingMessageHandler::SetTargetScreenSize(TWeakPtr<FIntPoint> InScreenSize)
	{
		TargetScreenSize = InScreenSize;
	}

	TWeakPtr<FIntPoint> FPixelStreamingMessageHandler::GetTargetScreenSize()
	{
		return TargetScreenSize;
	}

	void FPixelStreamingMessageHandler::SetTargetViewport(TWeakPtr<SViewport> InViewport)
	{
		TargetViewport = InViewport;
	}

	TWeakPtr<SViewport> FPixelStreamingMessageHandler::GetTargetViewport()
	{
		return TargetViewport;
	}

	void FPixelStreamingMessageHandler::SetTargetHandler(const TSharedPtr<FGenericApplicationMessageHandler>& InTargetHandler)
	{
		MessageHandler = InTargetHandler;
	}

    void FPixelStreamingMessageHandler::HandleOnKeyChar(FMemoryReader Ar)
    {
        TPayloadOneParam<TCHAR> Payload(Ar);
        UE_LOG(LogPixelStreamingMessageHandler, Verbose, TEXT("KEY_PRESSED: Character = '%c'"), Payload.Param1);
        // A key char event is never repeated, so set it to false. It's value  
        // ultimately doesn't matter as this paramater isn't used later
        MessageHandler->OnKeyChar(Payload.Param1, false);
    }

    void FPixelStreamingMessageHandler::HandleOnKeyDown(FMemoryReader Ar)
    {
        TPayloadTwoParam<uint8, uint8> Payload(Ar);

		bool bIsRepeat = Payload.Param2 != 0;
		const FKey* AgnosticKey = JavaScriptKeyCodeToFKey[Payload.Param1];
		if (FilterKey(*AgnosticKey))
		{
			const uint32* KeyPtr;
			const uint32* CharacterPtr;
			FInputKeyManager::Get().GetCodesFromKey(*AgnosticKey, KeyPtr, CharacterPtr);
			uint32 Key = KeyPtr ? *KeyPtr : 0;
			uint32 Character = CharacterPtr ? *CharacterPtr : 0;

			UE_LOG(LogPixelStreamingMessageHandler, Verbose, TEXT("KEY_DOWN: Key = %d; Character = %d; IsRepeat = %s"), Key, Character, bIsRepeat ? TEXT("True") : TEXT("False"));
			MessageHandler->OnKeyDown((int32)Key, (int32)Character, bIsRepeat);
		}
	}

    void FPixelStreamingMessageHandler::HandleOnKeyUp(FMemoryReader Ar)
    {
        TPayloadOneParam<uint8> Payload(Ar);
        const FKey* AgnosticKey = JavaScriptKeyCodeToFKey[Payload.Param1];
        if (FilterKey(*AgnosticKey))
		{
			const uint32* KeyPtr;
			const uint32* CharacterPtr;
			FInputKeyManager::Get().GetCodesFromKey(*AgnosticKey, KeyPtr, CharacterPtr);
			uint32 Key = KeyPtr ? *KeyPtr : 0;
			uint32 Character = CharacterPtr ? *CharacterPtr : 0;

			UE_LOG(LogPixelStreamingMessageHandler, Verbose, TEXT("KEY_UP: Key = %d; Character = %d"), Key, Character);
			MessageHandler->OnKeyUp((int32)Key, (int32)Character, false);
		}
	}

    void FPixelStreamingMessageHandler::HandleOnTouchStarted(FMemoryReader Ar)
    {
        TPayloadOneParam<uint8> Payload(Ar);

		uint8 NumTouches = Payload.Param1;
		for (uint8 TouchIdx = 0; TouchIdx < NumTouches; TouchIdx++)
		{
			//                 PosX    PoxY    IDX   Force  Valid
			TPayloadFiveParam<uint16, uint16, uint8, uint8, uint8> Touch(Ar);
			// If Touch is valid
			if (Touch.Param5 != 0)
			{
				if (NumActiveTouches == 0 && !bIsMouseActive)
				{
					FSlateApplication::Get().OnCursorSet();
					// Make sure the application is active.
					FSlateApplication::Get().ProcessApplicationActivationEvent(true);

					FVector2D OldCursorLocation = PixelStreamerApplicationWrapper->WrappedApplication->Cursor->GetPosition();
					PixelStreamerApplicationWrapper->Cursor->SetPosition(OldCursorLocation.X, OldCursorLocation.Y);
					FSlateApplication::Get().OverridePlatformApplication(PixelStreamerApplicationWrapper);
				}

				//                                                                           convert range from 0,65536 -> 0,1
				FVector2D TouchLocation = ConvertFromNormalizedScreenLocation(FVector2D(Touch.Param1 / uint16_MAX, Touch.Param2 / uint16_MAX));

				// We must update the user cursor position explicitly before updating the application cursor position
				// as if there's a delta between them, when the touch event is started it will trigger a move
				// resulting in a large 'drag' across the screen
				TSharedPtr<FSlateUser> User = FSlateApplication::Get().GetCursorUser();
				User->SetCursorPosition(TouchLocation);
				PixelStreamerApplicationWrapper->Cursor->SetPosition(TouchLocation.X, TouchLocation.Y);
				PixelStreamerApplicationWrapper->WrappedApplication->Cursor->SetPosition(TouchLocation.X, TouchLocation.Y);

				UE_LOG(LogPixelStreamingMessageHandler, Verbose, TEXT("TOUCH_START: TouchIndex = %d; Pos = (%d, %d); CursorPos = (%d, %d); Force = %.3f"), Touch.Param3, Touch.Param1, Touch.Param2, static_cast<int>(TouchLocation.X), static_cast<int>(TouchLocation.Y), Touch.Param4 / 255.0f);
				MessageHandler->OnTouchStarted(PixelStreamerApplicationWrapper->GetWindowUnderCursor(), TouchLocation, Touch.Param4 / 255.0f, Touch.Param3, 0); // TODO: ControllerId?

				NumActiveTouches++;
			}
		}
		
		FindFocusedWidget();
    }

    void FPixelStreamingMessageHandler::HandleOnTouchMoved(FMemoryReader Ar)
    {
        TPayloadOneParam<uint8> Payload(Ar);

		uint8 NumTouches = Payload.Param1;
		for (uint8 TouchIdx = 0; TouchIdx < NumTouches; TouchIdx++)
		{
			//                 PosX    PoxY    IDX   Force  Valid
			TPayloadFiveParam<uint16, uint16, uint8, uint8, uint8> Touch(Ar);
			// If Touch is valid
			if (Touch.Param5 != 0)
			{
				//                                                                           convert range from 0,65536 -> 0,1
				FVector2D TouchLocation = ConvertFromNormalizedScreenLocation(FVector2D(Touch.Param1 / uint16_MAX, Touch.Param2 / uint16_MAX));
                UE_LOG(LogPixelStreamingMessageHandler, Verbose, TEXT("TOUCH_MOVE: TouchIndex = %d; Pos = (%d, %d); CursorPos = (%d, %d); Force = %.3f"), Touch.Param3, Touch.Param1, Touch.Param2, static_cast<int>(TouchLocation.X), static_cast<int>(TouchLocation.Y),  Touch.Param4 / 255.0f);
                MessageHandler->OnTouchMoved(TouchLocation, Touch.Param4 / 255.0f, Touch.Param3, 0); // TODO: ControllerId?
            }
        }
    }

    void FPixelStreamingMessageHandler::HandleOnTouchEnded(FMemoryReader Ar)
    {
        TPayloadOneParam<uint8> Payload(Ar);
        uint8 NumTouches = Payload.Param1;
        for(uint8 TouchIdx = 0; TouchIdx < NumTouches; TouchIdx++)
        {
            //                 PosX    PoxY    IDX   Force  Valid
            TPayloadFiveParam<uint16, uint16, uint8, uint8, uint8> Touch(Ar);
			// Always allowing the "up" events regardless of in or outside the valid region so
			// states aren't stuck "down". Might want to uncomment this if it causes other issues.
			// if(Touch.Param5 != 0)
			{
				//                                                                           convert range from 0,65536 -> 0,1
				FVector2D TouchLocation = ConvertFromNormalizedScreenLocation(FVector2D(Touch.Param1 / uint16_MAX, Touch.Param2 / uint16_MAX));

				UE_LOG(LogPixelStreamingMessageHandler, Verbose, TEXT("TOUCH_END: TouchIndex = %d; Pos = (%d, %d); CursorPos = (%d, %d)"), Touch.Param3, Touch.Param1, Touch.Param2, static_cast<int>(TouchLocation.X), static_cast<int>(TouchLocation.Y));
				MessageHandler->OnTouchEnded(TouchLocation, Touch.Param3, 0); // TODO: ControllerId?
				NumActiveTouches--;
			}
		}

		// If there's no remaing touches, and there is also no mouse over the player window
		// then set the platform application back to its default. We need to set it back to default
		// so that people using the editor (if editor streaming) can click on buttons outside the target window
		// and also have the correct cursor (pixel streaming forces default cursor)
		if (NumActiveTouches == 0 && !bIsMouseActive)
		{
			FVector2D OldCursorLocation = PixelStreamerApplicationWrapper->Cursor->GetPosition();
			PixelStreamerApplicationWrapper->WrappedApplication->Cursor->SetPosition(OldCursorLocation.X, OldCursorLocation.Y);
			FSlateApplication::Get().OverridePlatformApplication(PixelStreamerApplicationWrapper->WrappedApplication);
		}
	}


	void FPixelStreamingMessageHandler::HandleOnControllerAnalog(FMemoryReader Ar)
	{
		TPayloadThreeParam<uint8, uint8, double> Payload(Ar);

		FInputDeviceId ControllerId = FInputDeviceId::CreateFromInternalId((int32)Payload.Param1);
		FGamepadKeyNames::Type Button = ConvertAxisIndexToGamepadAxis(Payload.Param2);
		float AnalogValue = (float)Payload.Param3;
		FPlatformUserId UserId = IPlatformInputDeviceMapper::Get().GetPrimaryPlatformUser();

		UE_LOG(LogPixelStreamingMessageHandler, Verbose, TEXT("GAMEPAD_ANALOG: ControllerId = %d; KeyName = %s; AnalogValue = %.4f;"), ControllerId.GetId(), *Button.ToString(), AnalogValue);
        MessageHandler->OnControllerAnalog(Button, UserId, ControllerId, AnalogValue);
	}

    void FPixelStreamingMessageHandler::HandleOnControllerButtonPressed(FMemoryReader Ar)
    {
        TPayloadThreeParam<uint8, uint8, uint8> Payload(Ar);

        FInputDeviceId ControllerId = FInputDeviceId::CreateFromInternalId((int32) Payload.Param1);
        FGamepadKeyNames::Type Button = ConvertButtonIndexToGamepadButton(Payload.Param2);
        bool bIsRepeat = Payload.Param3 != 0;
        FPlatformUserId UserId = IPlatformInputDeviceMapper::Get().GetPrimaryPlatformUser();
        
        UE_LOG(LogPixelStreamingMessageHandler, Verbose, TEXT("GAMEPAD_PRESSED: ControllerId = %d; KeyName = %s; IsRepeat = %s;"), ControllerId.GetId(), *Button.ToString(), bIsRepeat ? TEXT("True") : TEXT("False"));
        MessageHandler->OnControllerButtonPressed(Button, UserId, ControllerId, bIsRepeat);
    }

    void FPixelStreamingMessageHandler::HandleOnControllerButtonReleased(FMemoryReader Ar)
    {
        TPayloadTwoParam<uint8, uint8> Payload(Ar);

        FInputDeviceId ControllerId = FInputDeviceId::CreateFromInternalId((int32) Payload.Param1);
        FGamepadKeyNames::Type Button = ConvertButtonIndexToGamepadButton(Payload.Param2);
        FPlatformUserId UserId = IPlatformInputDeviceMapper::Get().GetPrimaryPlatformUser();

        UE_LOG(LogPixelStreamingMessageHandler, Verbose, TEXT("GAMEPAD_RELEASED: ControllerId = %d; KeyName = %s;"), ControllerId.GetId(), *Button.ToString());
        MessageHandler->OnControllerButtonReleased(Button, UserId, ControllerId, false);
    }

    /**
     * Mouse events
     */
    void FPixelStreamingMessageHandler::HandleOnMouseEnter(FMemoryReader Ar)
    {
        if(NumActiveTouches == 0 && !bIsMouseActive)
        {
            FSlateApplication::Get().OnCursorSet();
            FSlateApplication::Get().OverridePlatformApplication(PixelStreamerApplicationWrapper);
            // Make sure the application is active.
            FSlateApplication::Get().ProcessApplicationActivationEvent(true);
        }
        
        bIsMouseActive = true;
        UE_LOG(LogPixelStreamingMessageHandler, Verbose, TEXT("MOUSE_ENTER"));
    }

    void FPixelStreamingMessageHandler::HandleOnMouseLeave(FMemoryReader Ar)
    {
        if(NumActiveTouches == 0)
        {
            // Restore normal application layer if there is no active touches and MouseEnter hasn't been triggered
		    FSlateApplication::Get().OverridePlatformApplication(PixelStreamerApplicationWrapper->WrappedApplication);
        }
        bIsMouseActive = false;
	    UE_LOG(LogPixelStreamingMessageHandler, Verbose, TEXT("MOUSE_LEAVE"));
    }

    void FPixelStreamingMessageHandler::HandleOnMouseUp(FMemoryReader Ar)
    {
        TPayloadThreeParam<uint8, uint16, uint16> Payload(Ar);

        EMouseButtons::Type Button = static_cast<EMouseButtons::Type>(Payload.Param1);
        UE_LOG(LogPixelStreamingMessageHandler, Verbose, TEXT("MOUSE_UP: Button = %d"), Button);
        if(Button != EMouseButtons::Type::Invalid)
        {
            MessageHandler->OnMouseUp(Button);
        }
    }

    void FPixelStreamingMessageHandler::HandleOnMouseDown(FMemoryReader Ar)
    {
        TPayloadThreeParam<uint8, uint16, uint16> Payload(Ar);
        //                                                                           convert range from 0,65536 -> 0,1
        FVector2D ScreenLocation = ConvertFromNormalizedScreenLocation(FVector2D(Payload.Param2 / uint16_MAX, Payload.Param3 / uint16_MAX));
        EMouseButtons::Type Button = static_cast<EMouseButtons::Type>(Payload.Param1);

		UE_LOG(LogPixelStreamingMessageHandler, Verbose, TEXT("MOUSE_DOWN: Button = %d; Pos = (%.4f, %.4f)"), Button, ScreenLocation.X, ScreenLocation.Y);
		// Force window focus
		FSlateApplication::Get().ProcessApplicationActivationEvent(true);
		MessageHandler->OnMouseDown(PixelStreamerApplicationWrapper->GetWindowUnderCursor(), Button, ScreenLocation);
	}

    void FPixelStreamingMessageHandler::HandleOnMouseMove(FMemoryReader Ar)
    {
        TPayloadFourParam<uint16, uint16, int16, int16> Payload(Ar);
        //                                                                           convert range from 0,65536 -> 0,1
        FIntPoint ScreenLocation = ConvertFromNormalizedScreenLocation(FVector2D(Payload.Param1 / uint16_MAX, Payload.Param2 / uint16_MAX));
        //                                                                 convert range from -32,768 to 32,767 -> -1,1
        FIntPoint Delta = ConvertFromNormalizedScreenLocation(FVector2D(Payload.Param3 / int16_MAX, Payload.Param4 / int16_MAX), false);

		FSlateApplication::Get().OnCursorSet();
		UE_LOG(LogPixelStreamingMessageHandler, Verbose, TEXT("MOUSE_MOVE: Pos = (%d, %d); Delta = (%d, %d)"), ScreenLocation.X, ScreenLocation.Y, Delta.X, Delta.Y);
		PixelStreamerApplicationWrapper->Cursor->SetPosition(ScreenLocation.X, ScreenLocation.Y);
		MessageHandler->OnRawMouseMove(Delta.X, Delta.Y);
	}

    void FPixelStreamingMessageHandler::HandleOnMouseWheel(FMemoryReader Ar)
    {
        TPayloadThreeParam<int16, uint16, uint16> Payload(Ar);
         //                                                                           convert range from 0,65536 -> 0,1
        FIntPoint ScreenLocation = ConvertFromNormalizedScreenLocation(FVector2D(Payload.Param2 / uint16_MAX, Payload.Param3 / uint16_MAX));
        const float SpinFactor = 1 / 120.0f;
        MessageHandler->OnMouseWheel(Payload.Param1 * SpinFactor, ScreenLocation);
        UE_LOG(LogPixelStreamingMessageHandler, Verbose, TEXT("MOUSE_WHEEL: Delta = %d; Pos = (%d, %d)"), Payload.Param1, ScreenLocation.X, ScreenLocation.Y);
    }

	void FPixelStreamingMessageHandler::HandleOnMouseDoubleClick(FMemoryReader Ar)
	{
		TPayloadThreeParam<uint8, uint16, uint16> Payload(Ar);
        //                                                                           convert range from 0,65536 -> 0,1
        FVector2D ScreenLocation = ConvertFromNormalizedScreenLocation(FVector2D(Payload.Param2 / uint16_MAX, Payload.Param3 / uint16_MAX));
        EMouseButtons::Type Button = static_cast<EMouseButtons::Type>(Payload.Param1);

		UE_LOG(LogPixelStreamingMessageHandler, Verbose, TEXT("MOUSE_DOWN: Button = %d; Pos = (%.4f, %.4f)"), Button, ScreenLocation.X, ScreenLocation.Y);
		// Force window focus
		FSlateApplication::Get().ProcessApplicationActivationEvent(true);
		MessageHandler->OnMouseDoubleClick(PixelStreamerApplicationWrapper->GetWindowUnderCursor(), Button, ScreenLocation);
	}

    /**
     * Command handling
     */
    void FPixelStreamingMessageHandler::HandleCommand(FMemoryReader Ar)
    {
        FString Res;
        Res.GetCharArray().SetNumUninitialized(Ar.TotalSize() / 2 + 1);
        Ar.Serialize(Res.GetCharArray().GetData(), Ar.TotalSize());

		FString Descriptor = Res.Mid(MessageHeaderOffset);
		UE_LOG(LogPixelStreamingMessageHandler, Verbose, TEXT("Command: %s"), *Descriptor);

		bool bSuccess = false;

		FString ConsoleCommand;
		UE::PixelStreaming::ExtractJsonFromDescriptor(Descriptor, TEXT("ConsoleCommand"), ConsoleCommand, bSuccess);
		if (bSuccess && UE::PixelStreaming::Settings::CVarPixelStreamingAllowConsoleCommands.GetValueOnAnyThread())
		{
			GEngine->Exec(GEngine->GetWorld(), *ConsoleCommand);
			return;
		}

		/**
		 * Allowed Console commands
		 */
		FString WidthString;
		FString HeightString;
		UE::PixelStreaming::ExtractJsonFromDescriptor(Descriptor, TEXT("Resolution.Width"), WidthString, bSuccess);
		if (bSuccess)
		{
			UE::PixelStreaming::ExtractJsonFromDescriptor(Descriptor, TEXT("Resolution.Height"), HeightString, bSuccess);

			if (bSuccess)
			{
				int Width = FCString::Atoi(*WidthString);
				int Height = FCString::Atoi(*HeightString);
				if (Width < 1 || Height < 1)
				{
					return;
				}

				FString ChangeResCommand = FString::Printf(TEXT("r.SetRes %dx%d"), Width, Height);
				GEngine->Exec(GEngine->GetWorld(), *ChangeResCommand);
				return;
			}
		}

		FString StatFPSString;
		UE::PixelStreaming::ExtractJsonFromDescriptor(Descriptor, TEXT("Stat.FPS"), StatFPSString, bSuccess);
		if (bSuccess)
		{
			FString StatFPSCommand = FString::Printf(TEXT("stat fps"));
			GEngine->Exec(GEngine->GetWorld(), *StatFPSCommand);
			return;
		}

		/**
		 * Encoder Settings
		 */
		FString MinQPString;
		UE::PixelStreaming::ExtractJsonFromDescriptor(Descriptor, TEXT("Encoder.MinQP"), MinQPString, bSuccess);
		if (bSuccess)
		{
			int MinQP = FCString::Atoi(*MinQPString);
			UE::PixelStreaming::Settings::CVarPixelStreamingEncoderMinQP->Set(MinQP, ECVF_SetByCommandline);
			return;
		}

		FString MaxQPString;
		UE::PixelStreaming::ExtractJsonFromDescriptor(Descriptor, TEXT("Encoder.MaxQP"), MaxQPString, bSuccess);
		if (bSuccess)
		{
			int MaxQP = FCString::Atoi(*MaxQPString);
			UE::PixelStreaming::Settings::CVarPixelStreamingEncoderMaxQP->Set(MaxQP, ECVF_SetByCommandline);
			return;
		}

		/**
		 * WebRTC Settings
		 */
		FString FPSString;
		UE::PixelStreaming::ExtractJsonFromDescriptor(Descriptor, TEXT("WebRTC.Fps"), FPSString, bSuccess);
		if (bSuccess)
		{
			int FPS = FCString::Atoi(*FPSString);
			UE::PixelStreaming::Settings::CVarPixelStreamingWebRTCFps->Set(FPS, ECVF_SetByCommandline);
			return;
		}

		FString MinBitrateString;
		UE::PixelStreaming::ExtractJsonFromDescriptor(Descriptor, TEXT("WebRTC.MinBitrate"), MinBitrateString, bSuccess);
		if (bSuccess)
		{
			int MinBitrate = FCString::Atoi(*MinBitrateString);
			UE::PixelStreaming::Settings::CVarPixelStreamingWebRTCMinBitrate->Set(MinBitrate, ECVF_SetByCommandline);
			return;
		}

		FString MaxBitrateString;
		UE::PixelStreaming::ExtractJsonFromDescriptor(Descriptor, TEXT("WebRTC.MaxBitrate"), MaxBitrateString, bSuccess);
		if (bSuccess)
		{
			int MaxBitrate = FCString::Atoi(*MaxBitrateString);
			UE::PixelStreaming::Settings::CVarPixelStreamingWebRTCMaxBitrate->Set(MaxBitrate, ECVF_SetByCommandline);
			return;
		}
	}

    /**
     * UI Interaction handling
     */
    void FPixelStreamingMessageHandler::HandleUIInteraction(FMemoryReader Ar)
    {
        FString Res;
        Res.GetCharArray().SetNumUninitialized(Ar.TotalSize() / 2 + 1);
        Ar.Serialize(Res.GetCharArray().GetData(), Ar.TotalSize());

		FString Descriptor = Res.Mid(MessageHeaderOffset);

		UE_LOG(LogPixelStreamingMessageHandler, Verbose, TEXT("UIInteraction: %s"), *Descriptor);
		for (UPixelStreamingInput* InputComponent : PixelStreamingModule->GetInputComponents())
		{
			InputComponent->OnInputEvent.Broadcast(Descriptor);
		}
	}

	FIntPoint FPixelStreamingMessageHandler::ConvertFromNormalizedScreenLocation(const FVector2D& ScreenLocation, bool bIncludeOffset)
	{
		FIntPoint OutVector((int32)ScreenLocation.X, (int32)ScreenLocation.Y);

		TSharedPtr<SWindow> ApplicationWindow = TargetWindow.Pin();
		if (ApplicationWindow.IsValid())
		{
			FVector2D WindowOrigin = ApplicationWindow->GetPositionInScreen();
			if (TargetViewport.IsValid())
			{
				TSharedPtr<SViewport> ViewportWidget = TargetViewport.Pin();

				if (ViewportWidget.IsValid())
				{
					FGeometry InnerWindowGeometry = ApplicationWindow->GetWindowGeometryInWindow();

					// Find the widget path relative to the window
					FArrangedChildren JustWindow(EVisibility::Visible);
					JustWindow.AddWidget(FArrangedWidget(ApplicationWindow.ToSharedRef(), InnerWindowGeometry));

					FWidgetPath PathToWidget(ApplicationWindow.ToSharedRef(), JustWindow);
					if (PathToWidget.ExtendPathTo(FWidgetMatcher(ViewportWidget.ToSharedRef()), EVisibility::Visible))
					{
						FArrangedWidget ArrangedWidget = PathToWidget.FindArrangedWidget(ViewportWidget.ToSharedRef()).Get(FArrangedWidget::GetNullWidget());

						FVector2D WindowClientOffset = ArrangedWidget.Geometry.GetAbsolutePosition();
						FVector2D WindowClientSize = ArrangedWidget.Geometry.GetAbsoluteSize();

						FVector2D OutTemp = bIncludeOffset ? WindowOrigin + WindowClientOffset + (ScreenLocation * WindowClientSize) : (ScreenLocation * WindowClientSize);
						UE_LOG(LogPixelStreamingMessageHandler, Verbose, TEXT("%.4f, %.4f"), ScreenLocation.X, ScreenLocation.Y);
						OutVector = FIntPoint((int32)OutTemp.X, (int32)OutTemp.Y);
					}
				}
			}
			else
			{
				FVector2D SizeInScreen = ApplicationWindow->GetSizeInScreen();
				FVector2D OutTemp = SizeInScreen * ScreenLocation;
				OutVector = FIntPoint((int32)OutTemp.X, (int32)OutTemp.Y);
			}
		}
		else if(TargetScreenSize.IsValid())
		{
			FIntPoint SizeInScreen = *(TargetScreenSize.Pin());
			FVector2D OutTemp = FVector2D(SizeInScreen) * ScreenLocation;
			OutVector = FIntPoint((int32)OutTemp.X, (int32)OutTemp.Y);
		}

		return OutVector;
	}

	bool FPixelStreamingMessageHandler::FilterKey(const FKey& Key)
	{
		for (auto&& FilteredKey : UE::PixelStreaming::Settings::FilteredKeys)
		{
			if (FilteredKey == Key)
				return false;
		}
		return true;
	}

	FGamepadKeyNames::Type FPixelStreamingMessageHandler::ConvertAxisIndexToGamepadAxis(uint8 AnalogAxis)
	{
		switch (AnalogAxis)
		{
			case 1:
			{
				return FGamepadKeyNames::LeftAnalogX;
			}
			break;
			case 2:
			{
				return FGamepadKeyNames::LeftAnalogY;
			}
			break;
			case 3:
			{
				return FGamepadKeyNames::RightAnalogX;
			}
			break;
			case 4:
			{
				return FGamepadKeyNames::RightAnalogY;
			}
			break;
			case 5:
			{
				return FGamepadKeyNames::LeftTriggerAnalog;
			}
			break;
			case 6:
			{
				return FGamepadKeyNames::RightTriggerAnalog;
			}
			break;
			default:
			{
				return FGamepadKeyNames::Invalid;
			}
			break;
		}
	}

	FGamepadKeyNames::Type FPixelStreamingMessageHandler::ConvertButtonIndexToGamepadButton(uint8 ButtonIndex)
	{
		switch (ButtonIndex)
		{
			case 0:
			{
				return FGamepadKeyNames::FaceButtonBottom;
			}
			case 1:
			{
				return FGamepadKeyNames::FaceButtonRight;
			}
			break;
			case 2:
			{
				return FGamepadKeyNames::FaceButtonLeft;
			}
			break;
			case 3:
			{
				return FGamepadKeyNames::FaceButtonTop;
			}
			break;
			case 4:
			{
				return FGamepadKeyNames::LeftShoulder;
			}
			break;
			case 5:
			{
				return FGamepadKeyNames::RightShoulder;
			}
			break;
			// Buttons 6 and 7 are mapped as analog axis as they are the triggers
			case 8:
			{
				return FGamepadKeyNames::SpecialLeft;
			}
			break;
			case 9:
			{
				return FGamepadKeyNames::SpecialRight;
			}
			case 10:
			{
				return FGamepadKeyNames::LeftThumb;
			}
			break;
			case 11:
			{
				return FGamepadKeyNames::RightThumb;
			}
			break;
			case 12:
			{
				return FGamepadKeyNames::DPadUp;
			}
			break;
			case 13:
			{
				return FGamepadKeyNames::DPadDown;
			}
			case 14:
			{
				return FGamepadKeyNames::DPadLeft;
			}
			break;
			case 15:
			{
				return FGamepadKeyNames::DPadRight;
			}
			break;
			default:
			{
				return FGamepadKeyNames::Invalid;
			}
			break;
		}
	}

	void FPixelStreamingMessageHandler::FindFocusedWidget()
	{
		FSlateApplication::Get().ForEachUser([this](FSlateUser& User) {
			TSharedPtr<SWidget> FocusedWidget = User.GetFocusedWidget();

			static FName SEditableTextType(TEXT("SEditableText"));
			static FName SMultiLineEditableTextType(TEXT("SMultiLineEditableText"));
			bool bEditable = FocusedWidget && (FocusedWidget->GetType() == SEditableTextType || FocusedWidget->GetType() == SMultiLineEditableTextType);

			// Check to see if the focus has changed.
			FVector2D Pos = bEditable ? FocusedWidget->GetCachedGeometry().GetAbsolutePosition() : UnfocusedPos;
			if (Pos != FocusedPos)
			{
				FocusedPos = Pos;

				// Tell the browser that the focus has changed.
				TSharedPtr<FJsonObject> JsonObject = MakeShareable(new FJsonObject);
				JsonObject->SetStringField(TEXT("command"), TEXT("onScreenKeyboard"));
				JsonObject->SetBoolField(TEXT("showOnScreenKeyboard"), bEditable);

				if (bEditable)
				{
					FVector2D NormalizedLocation;
					TSharedPtr<SWindow> ApplicationWindow = TargetWindow.Pin();
					if (ApplicationWindow.IsValid())
					{
						FVector2D WindowOrigin = ApplicationWindow->GetPositionInScreen();
						if (TargetViewport.IsValid())
						{
							TSharedPtr<SViewport> ViewportWidget = TargetViewport.Pin();

							if (ViewportWidget.IsValid())
							{
								FGeometry InnerWindowGeometry = ApplicationWindow->GetWindowGeometryInWindow();

								// Find the widget path relative to the window
								FArrangedChildren JustWindow(EVisibility::Visible);
								JustWindow.AddWidget(FArrangedWidget(ApplicationWindow.ToSharedRef(), InnerWindowGeometry));

								FWidgetPath PathToWidget(ApplicationWindow.ToSharedRef(), JustWindow);
								if (PathToWidget.ExtendPathTo(FWidgetMatcher(ViewportWidget.ToSharedRef()), EVisibility::Visible))
								{
									FArrangedWidget ArrangedWidget = PathToWidget.FindArrangedWidget(ViewportWidget.ToSharedRef()).Get(FArrangedWidget::GetNullWidget());

									FVector2D WindowClientOffset = ArrangedWidget.Geometry.GetAbsolutePosition();
									FVector2D WindowClientSize = ArrangedWidget.Geometry.GetAbsoluteSize();

									Pos = Pos - WindowClientOffset;
									NormalizedLocation = FVector2D(Pos / WindowClientSize);
								}
							}
						}
						else
						{
							FVector2D SizeInScreen = ApplicationWindow->GetSizeInScreen();
							NormalizedLocation = FVector2D(Pos / SizeInScreen);
						}
					}

					NormalizedLocation *= uint16_MAX;
					// ConvertToNormalizedScreenLocation(Pos, NormalizedLocation);
					JsonObject->SetNumberField(TEXT("x"), static_cast<uint16>(NormalizedLocation.X));
					JsonObject->SetNumberField(TEXT("y"), static_cast<uint16>(NormalizedLocation.Y));
				}

				FString Descriptor;
				TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> JsonWriter = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Descriptor);
				FJsonSerializer::Serialize(JsonObject.ToSharedRef(), JsonWriter);

				PixelStreamingModule->ForEachStreamer([&Descriptor, this](TSharedPtr<IPixelStreamingStreamer> Streamer){
					Streamer->SendPlayerMessage(PixelStreamingModule->GetProtocol().FromStreamerProtocol.Find("Command")->Id, Descriptor);
				});
			}
		});
    }
} // namespace UE::PixelStreaming
