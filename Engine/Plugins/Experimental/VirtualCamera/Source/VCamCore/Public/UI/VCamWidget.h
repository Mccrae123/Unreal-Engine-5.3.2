// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VCamComponent.h"
#include "UI/VCamConnectionStructs.h"
#include "Blueprint/UserWidget.h"

#include "VCamWidget.generated.h"

class UInputAction;
class UVCamModifier;

/*
 * A wrapper widget class that contains a set of VCam Connections
 * 
 * If you add a widget deriving from UVCamWidget to an Overlay Widget for a VCam Output Provider then when the
 * Overlay is created by the Provider it will also call InitializeConnections with the owning VCam Component.
 */
UCLASS(Abstract)
class VCAMCORE_API UVCamWidget : public UUserWidget
{
	GENERATED_BODY()
public:
	/*
	 * The VCam Connections associated with this Widget
	 * 
	 * Each Connection has a unique name associated with it and any connection related event
	 * will provide this name as one of its arguments.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VCam Connections")
	TMap<FName, FVCamConnection> Connections;

	/*
	 * Determines whether this widget will be automatically registered to receive input when the connections are initialized
	 *
	 * Note: This property is only read during Initialize so toggling at runtime will not have any effect
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="VCam Input")
	bool bRegisterForInput = true;

	/*
	 * If this widget is registered for input then this input mapping context will be added to the input system
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="VCam Input", meta=(EditCondition="bRegisterForInput"))
	TObjectPtr<UInputMappingContext> InputMappingContext = nullptr;

	/*
	 * If this widget is registered for input then this property defines the priority that the input mapping context is added at
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="VCam Input", meta=(EditCondition="bRegisterForInput"))
	int32 InputContextPriority = 0;
	
	/*
	 * Event called when a specific connection has been updated
	 * 
	 * The connection is not guaranteed to succeed so "Did Connect Successfully" should be checked before using
	 * the connected modifier or action
	 */
	UFUNCTION(BlueprintImplementableEvent, Category="VCam Connections")
	void OnConnectionUpdated(FName ConnectionName, bool bDidConnectSuccessfully, FName ModifierConnectionPointName, UVCamModifier* ConnectedModifier, UInputAction* ConnectedAction);

	/*
	 * Iterate all VCam Connections within the widget and attempt to connect them using the provided VCam Component
	 */
	UFUNCTION(BlueprintCallable, Category="VCam Connections")
	void InitializeConnections(UVCamComponent* VCam);
};