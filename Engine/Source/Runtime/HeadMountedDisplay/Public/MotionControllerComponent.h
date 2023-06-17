// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Components/PrimitiveComponent.h"
#include "SceneViewExtension.h"
#include "IMotionController.h"
#include "LateUpdateManager.h"
#include "IIdentifiableXRDevice.h" // for FXRDeviceId
#include "MotionControllerComponent.generated.h"

class FPrimitiveSceneInfo;
class FRHICommandListImmediate;
class FSceneView;
class FSceneViewFamily;
class UXRDeviceVisualizationComponent;

UCLASS(Blueprintable, meta = (BlueprintSpawnableComponent), ClassGroup = MotionController, MinimalAPI)
class UMotionControllerComponent : public UPrimitiveComponent
{
	GENERATED_UCLASS_BODY()

	/** Which player index this motion controller should automatically follow */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, BlueprintSetter = SetAssociatedPlayerIndex, Category = "MotionController")
	int32 PlayerIndex;

	/** Defines which pose this component should receive from the OpenXR Runtime. Left/Right MotionSource is the same as LeftGrip/RightGrip. See OpenXR specification for details on poses. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, BlueprintSetter = SetTrackingMotionSource, Category = "MotionController")
	FName MotionSource;

	/** If false, render transforms within the motion controller hierarchy will be updated a second time immediately before rendering. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MotionController")
	uint32 bDisableLowLatencyUpdate : 1;

	/** The tracking status for the device (e.g. full tracking, inertial tracking only, no tracking) */
	UPROPERTY(BlueprintReadOnly, Category = "MotionController")
	ETrackingStatus CurrentTrackingStatus;

	/** Used to automatically render a model associated with the set hand. */
	UE_DEPRECATED(5.2, "bDisplayDeviceModel is deprecated. Please use the XRDeviceVisualizationComponent for rendering instead.")
	UPROPERTY(EditAnywhere, BlueprintReadWrite, BlueprintSetter = SetShowDeviceModel, Category = "Visualization", meta = (DeprecatedProperty, DeprecationMessage = "bDisplayDeviceModel is deprecated. Please use the XRDeviceVisualizationComponent for rendering instead."))
	bool bDisplayDeviceModel;

	/** Determines the source of the desired model. By default, the active XR system(s) will be queried and (if available) will provide a model for the associated device. NOTE: this may fail if there's no default model; use 'Custom' to specify your own. */
	UE_DEPRECATED(5.2, "DisplayModelSource is deprecated. Please use the XRDeviceVisualizationComponent for rendering instead.")
	UPROPERTY(EditAnywhere, BlueprintReadWrite, BlueprintSetter = SetDisplayModelSource, Category = "Visualization", meta = (editcondition = "bDisplayDeviceModel", DeprecatedProperty, DeprecationMessage = "DisplayModelSource is deprecated. Please use the XRDeviceVisualizationComponent for rendering instead."))
	FName DisplayModelSource;

	static HEADMOUNTEDDISPLAY_API FName CustomModelSourceId;

	/** A mesh override that'll be displayed attached to this MotionController. */
	UE_DEPRECATED(5.2, "CustomDisplayMesh is deprecated. Please use the XRDeviceVisualizationComponent for rendering instead.")
	UPROPERTY(EditAnywhere, BlueprintReadWrite, BlueprintSetter = SetCustomDisplayMesh, Category = "Visualization", meta = (editcondition = "bDisplayDeviceModel", DeprecatedProperty, DeprecationMessage = "CustomDisplayMesh is deprecated. Please use the XRDeviceVisualizationComponent for rendering instead."))
	TObjectPtr<UStaticMesh> CustomDisplayMesh;

	/** Material overrides for the specified display mesh. */
	UE_DEPRECATED(5.2, "DisplayMeshMaterialOverrides is deprecated. Please use the XRDeviceVisualizationComponent for rendering instead.")
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Visualization", meta = (editcondition = "bDisplayDeviceModel", DeprecatedProperty, DeprecationMessage = "DisplayMeshMaterialOverrides is deprecated. Please use the XRDeviceVisualizationComponent for rendering instead."))
	TArray<TObjectPtr<UMaterialInterface>> DisplayMeshMaterialOverrides;

	UE_DEPRECATED(5.2, "SetShowDeviceModel is deprecated. Please use the XRDeviceVisualizationComponent for rendering instead.")
	UFUNCTION(BlueprintSetter, meta = (DeprecatedFunction, DeprecationMessage = "SetShowDeviceModel is deprecated. Please use the XRDeviceVisualizationComponent for rendering instead."))
	HEADMOUNTEDDISPLAY_API void SetShowDeviceModel(const bool bShowControllerModel);

	UE_DEPRECATED(5.2, "SetDisplayModelSource is deprecated. Please use the XRDeviceVisualizationComponent for rendering instead.")
	UFUNCTION(BlueprintSetter, meta = (DeprecatedFunction, DeprecationMessage = "SetDisplayModelSource is deprecated. Please use the XRDeviceVisualizationComponent for rendering instead."))
	HEADMOUNTEDDISPLAY_API void SetDisplayModelSource(const FName NewDisplayModelSource);

	UE_DEPRECATED(5.2, "SetCustomDisplayMesh is deprecated. Please use the XRDeviceVisualizationComponent for rendering instead.")
	UFUNCTION(BlueprintSetter, meta = (DeprecatedFunction, DeprecationMessage = "SetCustomDisplayMesh is deprecated. Please use the XRDeviceVisualizationComponent for rendering instead."))
	HEADMOUNTEDDISPLAY_API void SetCustomDisplayMesh(UStaticMesh* NewDisplayMesh);

	/** Whether or not this component had a valid tracked device this frame */
	UFUNCTION(BlueprintPure, Category = "MotionController")
	bool IsTracked() const
	{
		return bTracked;
	}

	UFUNCTION(BlueprintSetter, meta = (DeprecatedFunction, DeprecationMessage = "Please use the Motion Source property instead of Hand"))
	HEADMOUNTEDDISPLAY_API void SetTrackingSource(const EControllerHand NewSource);

	UFUNCTION(BlueprintGetter, meta = (DeprecatedFunction, DeprecationMessage = "Please use the Motion Source property instead of Hand"))
	HEADMOUNTEDDISPLAY_API EControllerHand GetTrackingSource() const;

	UFUNCTION(BlueprintSetter)
	HEADMOUNTEDDISPLAY_API void SetTrackingMotionSource(const FName NewSource);

	HEADMOUNTEDDISPLAY_API FName GetTrackingMotionSource();

	UFUNCTION(BlueprintSetter)
	HEADMOUNTEDDISPLAY_API void SetAssociatedPlayerIndex(const int32 NewPlayer);

	HEADMOUNTEDDISPLAY_API void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	HEADMOUNTEDDISPLAY_API void BeginDestroy() override;

	// The following private properties/members are now deprecated and will be removed in later versions.
	HEADMOUNTEDDISPLAY_API void RefreshDisplayComponent(const bool bForceDestroy = false);
	HEADMOUNTEDDISPLAY_API void PostLoad() override;

	/** Callback for asynchronous display model loads (to set materials, etc.) */
	HEADMOUNTEDDISPLAY_API void OnDisplayModelLoaded(UPrimitiveComponent* DisplayComponent);

	UPROPERTY(Transient, BlueprintReadOnly, Category = Visualization, meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UPrimitiveComponent> DisplayComponent;

	enum class EModelLoadStatus : uint8
	{
		Unloaded,
		Pending,
		InProgress,
		Complete
	};
	EModelLoadStatus DisplayModelLoadState = EModelLoadStatus::Unloaded;

	FXRDeviceId DisplayDeviceId;

#if WITH_EDITOR
	int32 PreEditMaterialCount = 0;
#endif

public:
	//~ UObject interface
	HEADMOUNTEDDISPLAY_API virtual void Serialize(FArchive& Ar) override;

#if WITH_EDITOR
	HEADMOUNTEDDISPLAY_API virtual void PreEditChange(FProperty* PropertyAboutToChange) override;
	HEADMOUNTEDDISPLAY_API virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif 

	//~ UActorComponent interface
	HEADMOUNTEDDISPLAY_API virtual void OnRegister() override;
	HEADMOUNTEDDISPLAY_API virtual void InitializeComponent() override;
	HEADMOUNTEDDISPLAY_API virtual void OnComponentDestroyed(bool bDestroyingHierarchy) override;

	// Delegate for activation of XRDeviceVisualizationComponent
	DECLARE_MULTICAST_DELEGATE_OneParam(FActivateVisualizationComponent, bool);
	static HEADMOUNTEDDISPLAY_API FActivateVisualizationComponent OnActivateVisualizationComponent;

protected:
	//~ Begin UActorComponent Interface.
	HEADMOUNTEDDISPLAY_API virtual void CreateRenderState_Concurrent(FRegisterComponentContext* Context) override;
	HEADMOUNTEDDISPLAY_API virtual void SendRenderTransform_Concurrent() override;
	//~ End UActorComponent Interface.

	// Cached Motion Controller that can be read by GetParameterValue. Only valid for the duration of OnMotionControllerUpdated
	IMotionController* InUseMotionController;

	/** Blueprint Implementable function for responding to updated data from a motion controller (so we can use custom parameter values from it) */
	UFUNCTION(BlueprintImplementableEvent, Category = "Motion Controller Update")
	HEADMOUNTEDDISPLAY_API void OnMotionControllerUpdated();

	// Returns the value of a custom parameter on the current in use Motion Controller (see member InUseMotionController). Only valid for the duration of OnMotionControllerUpdated 
	UFUNCTION(BlueprintCallable, Category = "Motion Controller Update")
	HEADMOUNTEDDISPLAY_API float GetParameterValue(FName InName, bool& bValueFound);

	UFUNCTION(BlueprintCallable, Category = "Motion Controller Update")
	HEADMOUNTEDDISPLAY_API FVector GetHandJointPosition(int jointIndex, bool& bValueFound);

private:

	/** Whether or not this component had a valid tracked controller associated with it this frame*/
	bool bTracked;

	/** Whether or not this component has authority within the frame*/
	bool bHasAuthority;

	/** If true, the Position and Orientation args will contain the most recent controller state */
	HEADMOUNTEDDISPLAY_API bool PollControllerState(FVector& Position, FRotator& Orientation, float WorldToMetersScale);

	HEADMOUNTEDDISPLAY_API void OnModularFeatureUnregistered(const FName& Type, class IModularFeature* ModularFeature);
	IMotionController* PolledMotionController_GameThread;
	IMotionController* PolledMotionController_RenderThread;
	FCriticalSection PolledMotionControllerMutex;


	FTransform RenderThreadRelativeTransform;
	FVector RenderThreadComponentScale;

	/** View extension object that can persist on the render thread without the motion controller component */
	class FViewExtension : public FSceneViewExtensionBase
	{
	public:
		FViewExtension(const FAutoRegister& AutoRegister, UMotionControllerComponent* InMotionControllerComponent);
		virtual ~FViewExtension() {}

		/** ISceneViewExtension interface */
		virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {}
		virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {}
		virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override;
		virtual void PreRenderView_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView) override {}
		virtual void PreRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily) override;
		virtual int32 GetPriority() const override { return -10; }
		virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const;

	private:
		friend class UMotionControllerComponent;

		/** Motion controller component associated with this view extension */
		UMotionControllerComponent* MotionControllerComponent;
		FLateUpdateManager LateUpdate;
	};
	TSharedPtr< FViewExtension, ESPMode::ThreadSafe > ViewExtension;	
};
