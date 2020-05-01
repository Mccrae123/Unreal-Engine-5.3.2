// Copyright Epic Games, Inc. All Rights Reserved.

#include "Components/RuntimeVirtualTextureComponent.h"

#include "Components/PrimitiveComponent.h"
#include "GameFramework/Actor.h"
#include "SceneInterface.h"
#include "VT/RuntimeVirtualTexture.h"
#include "VT/VirtualTextureBuilder.h"

URuntimeVirtualTextureComponent::URuntimeVirtualTextureComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
	, SceneProxy(nullptr)
{
}

bool URuntimeVirtualTextureComponent::IsVisible() const
{
	return Super::IsVisible() && UseVirtualTexturing(GetScene()->GetFeatureLevel());
}

void URuntimeVirtualTextureComponent::CreateRenderState_Concurrent(FRegisterComponentContext* Context)
{
	if (ShouldRender() && VirtualTexture != nullptr)
	{
		// This will modify the URuntimeVirtualTexture and allocate its VT
		GetScene()->AddRuntimeVirtualTexture(this);
	}

	Super::CreateRenderState_Concurrent(Context);
}

void URuntimeVirtualTextureComponent::SendRenderTransform_Concurrent()
{
	if (ShouldRender() && VirtualTexture != nullptr)
	{
		// This will modify the URuntimeVirtualTexture and allocate its VT
		GetScene()->AddRuntimeVirtualTexture(this);
	}

	Super::SendRenderTransform_Concurrent();
}

void URuntimeVirtualTextureComponent::DestroyRenderState_Concurrent()
{
	// This will modify the URuntimeVirtualTexture and free its VT
	GetScene()->RemoveRuntimeVirtualTexture(this);

	Super::DestroyRenderState_Concurrent();
}

FBoxSphereBounds URuntimeVirtualTextureComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	// Bounds are based on the unit box centered on the origin
	return FBoxSphereBounds(FBox(FVector(-.5f, -.5f, -1.f), FVector(.5f, .5f, 1.f))).TransformBy(LocalToWorld);
}

FTransform URuntimeVirtualTextureComponent::GetVirtualTextureTransform() const
{
	// Transform is based on bottom left of the URuntimeVirtualTextureComponent unit box (which is centered on the origin)
	return FTransform(FVector(-0.5f, -0.5f, 0.f)) * GetComponentTransform();
}

uint64 URuntimeVirtualTextureComponent::CalculateStreamingTextureSettingsHash() const
{
	// Shouldn't need to call this when there is VirtualTexture == nullptr
	check(VirtualTexture != nullptr);

	// If a setting change can cause the streaming texture to no longer be valid then it should be included in this hash.
	union FPackedSettings
	{
		uint64 PackedValue;
		struct
		{
			uint32 MaterialType : 4;
			uint32 TileSize : 12;
			uint32 TileBorderSize : 4;
			uint32 StreamLowMips : 4;
			uint32 LODGroup : 8;
			uint32 CompressTextures : 1;
			uint32 SinglePhysicalSpace : 1;
			uint32 EnableCompressCrunch : 1;
		};
	};

	FPackedSettings Settings;
	Settings.PackedValue = 0;
	Settings.MaterialType = (uint32)VirtualTexture->GetMaterialType();
	Settings.TileSize = (uint32)VirtualTexture->GetTileSize();
	Settings.TileBorderSize = (uint32)VirtualTexture->GetTileBorderSize();
	Settings.StreamLowMips = (uint32)StreamLowMips;
	Settings.LODGroup = (uint32)VirtualTexture->GetLODGroup();
	Settings.CompressTextures = (uint32)VirtualTexture->GetCompressTextures();
	Settings.SinglePhysicalSpace = (uint32)VirtualTexture->GetSinglePhysicalSpace();
	Settings.EnableCompressCrunch = (uint32)bEnableCompressCrunch;

	return Settings.PackedValue;
}

bool URuntimeVirtualTextureComponent::IsStreamingTextureValid() const
{
	return VirtualTexture != nullptr && StreamingTexture != nullptr && StreamingTexture->Texture != nullptr && StreamingTexture->BuildHash == CalculateStreamingTextureSettingsHash();
}

bool URuntimeVirtualTextureComponent::IsStreamingLowMips() const
{
#if WITH_EDITOR
	if (!bUseStreamingLowMipsInEditor)
	{
		return false;
	}
#endif
	return StreamLowMips > 0 && IsStreamingTextureValid();
}

#if WITH_EDITOR

void URuntimeVirtualTextureComponent::InitializeStreamingTexture(uint32 InSizeX, uint32 InSizeY, uint8* InData)
{
	if (VirtualTexture != nullptr && StreamingTexture != nullptr)
	{
		// Release current runtime virtual texture producer.
		// It may reference data inside the old StreamingTexture which could be garbage collected any time from now.
		VirtualTexture->Release();

		FVirtualTextureBuildDesc BuildDesc;
		BuildDesc.bSinglePhysicalSpace = VirtualTexture->GetSinglePhysicalSpace();

		BuildDesc.TileSize = VirtualTexture->GetTileSize();
		BuildDesc.TileBorderSize = VirtualTexture->GetTileBorderSize();
		BuildDesc.LODGroup = VirtualTexture->GetLODGroup();
		BuildDesc.bCrunchCompressed = bEnableCompressCrunch;

		BuildDesc.LayerCount = VirtualTexture->GetLayerCount();
		check(BuildDesc.LayerCount <= RuntimeVirtualTexture::MaxTextureLayers);
		BuildDesc.LayerFormats.AddDefaulted(BuildDesc.LayerCount);
		BuildDesc.LayerFormatSettings.AddDefaulted(BuildDesc.LayerCount);

		for (int32 Layer = 0; Layer < BuildDesc.LayerCount; Layer++)
		{
			const EPixelFormat LayerFormat = VirtualTexture->GetLayerFormat(Layer);
			BuildDesc.LayerFormats[Layer] = LayerFormat == PF_G16 ? TSF_G16 : TSF_BGRA8;

			BuildDesc.LayerFormatSettings[Layer].CompressionSettings = LayerFormat == PF_BC5 ? TC_Normalmap : TC_Default;
			BuildDesc.LayerFormatSettings[Layer].CompressionNone = LayerFormat == PF_B8G8R8A8 || LayerFormat == PF_G16;
			BuildDesc.LayerFormatSettings[Layer].CompressionNoAlpha = LayerFormat == PF_DXT1 || LayerFormat == PF_BC5;
			BuildDesc.LayerFormatSettings[Layer].CompressionYCoCg = VirtualTexture->IsLayerYCoCg(Layer);
			BuildDesc.LayerFormatSettings[Layer].SRGB = VirtualTexture->IsLayerSRGB(Layer);
		}

		BuildDesc.BuildHash = CalculateStreamingTextureSettingsHash();

		BuildDesc.InSizeX = InSizeX;
		BuildDesc.InSizeY = InSizeY;
		BuildDesc.InData = InData;

		StreamingTexture->Modify();
		StreamingTexture->BuildTexture(BuildDesc);

		// Trigger refresh of the runtime virtual texture producer.
		VirtualTexture->PostEditChange();
	}
}

#endif
