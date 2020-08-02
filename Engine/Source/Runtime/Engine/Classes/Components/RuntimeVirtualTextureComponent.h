// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SceneComponent.h"
#include "RuntimeVirtualTextureComponent.generated.h"

class URuntimeVirtualTexture;
class UTexture2D;
class UVirtualTextureBuilder;

/** Component used to place a URuntimeVirtualTexture in the world. */
UCLASS(Blueprintable, ClassGroup = Rendering, HideCategories = (Activation, Collision, Cooking, Mobility, LOD, Object, Physics, Rendering))
class ENGINE_API URuntimeVirtualTextureComponent : public USceneComponent
{
	GENERATED_UCLASS_BODY()

protected:
	/** The virtual texture object to use. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, NonPIEDuplicateTransient, Category = VirtualTexture)
	URuntimeVirtualTexture* VirtualTexture = nullptr;

	/** Texture object containing streamed low mips. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, NonPIEDuplicateTransient, Category = VirtualTextureBuild)
	UVirtualTextureBuilder* StreamingTexture = nullptr;

	/** Number of low mips to serialize and stream for the virtual texture. This can reduce rendering update cost. */
	UPROPERTY(EditAnywhere, Category = VirtualTextureBuild, meta = (UIMin = "0", UIMax = "6", DisplayName = "Num Streaming Mips"))
	int32 StreamLowMips = 0;

	/** Enable Crunch texture compression for the streaming low mips. Generic ZLib compression is used when Crunch is disabled. */
	UPROPERTY(EditAnywhere, AdvancedDisplay, Category = VirtualTextureBuild, meta = (DisplayName = "Enable Crunch"))
	bool bEnableCompressCrunch = false;

	/** Use any streaming low mips when rendering in editor. Set true to view and debug the baked streaming low mips. */
	UPROPERTY(EditAnywhere, AdvancedDisplay, Category = VirtualTextureBuild, meta = (DisplayName = "View Streaming Mips in Editor"))
	bool bUseStreamingLowMipsInEditor = false;

	/** Texture object containing min and max height. Only valid if the virtual texture contains a compatible height layer. This can be useful for ray marching against the height. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, NonPIEDuplicateTransient, Category = VirtualTextureBuild)
	UTexture2D* MinMaxTexture = nullptr;

	/** Actor to align rotation to. If set this actor is always included in the bounds calculation. */
	UPROPERTY(EditAnywhere, Category = TransformFromBounds)
	TSoftObjectPtr<AActor> BoundsAlignActor = nullptr;

	/** If the Bounds Align Actor is a Landscape then this will snap the bounds so that virtual texture texels align with landscape vertex positions. */
	UPROPERTY(EditAnywhere, AdvancedDisplay, Category = TransformFromBounds, meta = (DisplayName = "Snap To Landscape"))
	bool bSnapBoundsToLandscape;

#if WITH_EDITOR
	FDelegateHandle PieEndDelegateHandle;
#endif

public:
	/**
	 * This function marks an area of the runtime virtual texture as dirty.
	 * @param WorldBounds : The world space bounds of the pages to invalidate.
	 */
	UFUNCTION(BlueprintCallable, Category = "VirtualTexture")
	void Invalidate(FBoxSphereBounds const& WorldBounds);

	/** Get the runtime virtual texture object on this component. */
	URuntimeVirtualTexture* GetVirtualTexture() const { return VirtualTexture; }

	/** Get the streaming virtual texture object on this component. */
	UVirtualTextureBuilder* GetStreamingTexture() const { return StreamingTexture; }

	/** Public getter for virtual texture streaming low mips */
	int32 NumStreamingMips() const { return FMath::Clamp(StreamLowMips, 0, 6); }

	/** Get if we want to use any streaming low mips on this component. */
	bool IsStreamingLowMips() const;

	/** Public getter for crunch compression flag. */
	bool IsCrunchCompressed() const { return bEnableCompressCrunch; }

#if WITH_EDITOR
	/** Set a new asset to hold the low mip streaming texture. This should only be called directly before setting data to the new asset. */
	void SetStreamingTexture(UVirtualTextureBuilder* InTexture) { StreamingTexture = InTexture; }
	/** Initialize the low mip streaming texture with the passed in size and data. */
	void InitializeStreamingTexture(uint32 InSizeX, uint32 InSizeY, uint8* InData);
#endif

	/** Returns true if a MinMax height texture is relevant for this virtual texture type. */
	bool IsMinMaxTextureEnabled() const;

	/** Get the streaming MinMax height texture on this component. */
	UTexture2D* GetMinMaxTexture() { return IsMinMaxTextureEnabled() ? MinMaxTexture : nullptr; }

#if WITH_EDITOR
	/** Set a new asset to hold the MinMax height texture. This should only be called directly before setting data to the new asset. */
	void SetMinMaxTexture(UTexture2D* InTexture) { MinMaxTexture = InTexture; }
	/** Initialize the MinMax height texture with the passed in size and data. */
	void InitializeMinMaxTexture(uint32 InSizeX, uint32 InSizeY, uint32 InNumMips, uint8* InData);
#endif

#if WITH_EDITOR
	/** Get the BoundsAlignActor on this component. */
	TSoftObjectPtr<AActor>& GetBoundsAlignActor() { return BoundsAlignActor; }
	/** Get if SnapBoundsToLandscape is set on this component. */
	bool GetSnapBoundsToLandscape() const { return bSnapBoundsToLandscape; }
#endif
	/** Get a translation to account for any vertex sample offset from the use of bSnapBoundsToLandscape. */
	FTransform GetTexelSnapTransform() const;

protected:
	//~ Begin UObject Interface
#if WITH_EDITOR
	virtual bool CanEditChange(const FProperty* InProperty) const override;
#endif
	//~ End UObject Interface

	//~ Begin UActorComponent Interface
	virtual void CreateRenderState_Concurrent(FRegisterComponentContext* Context) override;
	virtual void SendRenderTransform_Concurrent() override;
	virtual void DestroyRenderState_Concurrent() override;
#if WITH_EDITOR
	virtual void CheckForErrors() override;
#endif
	//~ End UActorComponent Interface

	//~ Begin USceneComponent Interface
#if WITH_EDITOR
	virtual void OnRegister() override;
	virtual void OnUnregister() override;
#endif
	virtual bool IsVisible() const override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	//~ End USceneComponent Interface

protected:
	/** Calculate a hash used to determine if the StreamingTexture contents are valid for use. The hash doesn't include whether the contents are up to date. */
	uint64 CalculateStreamingTextureSettingsHash() const;
	/** Returns true if the StreamingTexure contents are valid for use. */
	bool IsStreamingTextureValid() const;
	/** Returns true if the MinMaxTexture contents are valid for use. */
	bool IsMinMaxTextureValid() const;

public:
	/** Scene proxy object. Managed by the scene but stored here. */
	class FRuntimeVirtualTextureSceneProxy* SceneProxy;
};
