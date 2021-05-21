// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	SystemTextures.cpp: System textures implementation.
=============================================================================*/

#include "SystemTextures.h"
#include "Math/RandomStream.h"
#include "Math/Sobol.h"
#include "RenderTargetPool.h"
#include "ClearQuad.h"
#include "LTC.h"

/*-----------------------------------------------------------------------------
SystemTextures
-----------------------------------------------------------------------------*/

RDG_REGISTER_BLACKBOARD_STRUCT(FRDGSystemTextures);

const FRDGSystemTextures& FRDGSystemTextures::Create(FRDGBuilder& GraphBuilder)
{
	const auto Register = [&](const TRefCountPtr<IPooledRenderTarget>& RenderTarget)
	{
		return TryRegisterExternalTexture(GraphBuilder, RenderTarget, ERenderTargetTexture::ShaderResource, ERDGTextureFlags::ReadOnly);
	};

	auto& SystemTextures = GraphBuilder.Blackboard.Create<FRDGSystemTextures>();
	SystemTextures.White = Register(GSystemTextures.WhiteDummy);
	SystemTextures.Black = Register(GSystemTextures.BlackDummy);
	SystemTextures.BlackAlphaOne = Register(GSystemTextures.BlackAlphaOneDummy);
	SystemTextures.MaxFP16Depth = Register(GSystemTextures.MaxFP16Depth);
	SystemTextures.DepthDummy = Register(GSystemTextures.DepthDummy);
	SystemTextures.StencilDummy = Register(GSystemTextures.StencilDummy);
	SystemTextures.Green = Register(GSystemTextures.GreenDummy);
	SystemTextures.DefaultNormal8Bit = Register(GSystemTextures.DefaultNormal8Bit);
	SystemTextures.MidGrey = Register(GSystemTextures.MidGreyDummy);
	SystemTextures.VolumetricBlack = Register(GSystemTextures.VolumetricBlackDummy);
	SystemTextures.StencilDummySRV = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::CreateWithPixelFormat(SystemTextures.DepthDummy, PF_X24_G8));
	return SystemTextures;
}

const FRDGSystemTextures& FRDGSystemTextures::Get(FRDGBuilder& GraphBuilder)
{
	const FRDGSystemTextures* SystemTextures = GraphBuilder.Blackboard.Get<FRDGSystemTextures>();
	checkf(SystemTextures, TEXT("FRDGSystemTextures were not initialized. Call FRDGSystemTextures::Create() first."));
	return *SystemTextures;
}

bool FRDGSystemTextures::IsValid(FRDGBuilder& GraphBuilder)
{
	return GraphBuilder.Blackboard.Get<FRDGSystemTextures>() != nullptr;
}

/** The global render targets used for scene rendering. */
TGlobalResource<FSystemTextures> GSystemTextures;

void FSystemTextures::InitializeTextures(FRHICommandListImmediate& RHICmdList, const ERHIFeatureLevel::Type InFeatureLevel)
{
	// When we render to system textures it should occur on all GPUs since this only
	// happens once on startup (or when the feature level changes).
	SCOPED_GPU_MASK(RHICmdList, FRHIGPUMask::All());

	// if this is the first call initialize everything
	if (FeatureLevelInitializedTo == ERHIFeatureLevel::Num)
	{
		InitializeCommonTextures(RHICmdList);
		InitializeFeatureLevelDependentTextures(RHICmdList, InFeatureLevel);
	}
	// otherwise, if we request a higher feature level, we might need to initialize those textures that depend on the feature level
	else if (InFeatureLevel > FeatureLevelInitializedTo)
	{
		InitializeFeatureLevelDependentTextures(RHICmdList, InFeatureLevel);
	}
	// there's no needed setup for those feature levels lower or identical to the current one
}

void FSystemTextures::InitializeCommonTextures(FRHICommandListImmediate& RHICmdList)
{
	// First initialize textures that are common to all feature levels. This is always done the first time we come into this function, as doesn't care about the
	// requested feature level

		// Create a WhiteDummy texture
		{
			FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(FIntPoint(1, 1), PF_B8G8R8A8, FClearValueBinding::White, TexCreate_HideInVisualizeTexture, TexCreate_RenderTargetable | TexCreate_NoFastClear | TexCreate_ShaderResource, false));
			Desc.AutoWritable = false;
			GRenderTargetPool.FindFreeElement(RHICmdList, Desc, WhiteDummy, TEXT("WhiteDummy"), ERenderTargetTransience::NonTransient);

			RHICmdList.Transition(FRHITransitionInfo(WhiteDummy->GetRenderTargetItem().TargetableTexture, ERHIAccess::SRVMask, ERHIAccess::RTV));

			FRHIRenderPassInfo RPInfo(WhiteDummy->GetRenderTargetItem().TargetableTexture, ERenderTargetActions::Clear_Store);
			RHICmdList.BeginRenderPass(RPInfo, TEXT("WhiteDummy"));
			RHICmdList.EndRenderPass();
			RHICmdList.CopyToResolveTarget(WhiteDummy->GetRenderTargetItem().TargetableTexture, WhiteDummy->GetRenderTargetItem().ShaderResourceTexture, FResolveParams());

			WhiteDummySRV = RHICreateShaderResourceView((FRHITexture2D*)WhiteDummy->GetRenderTargetItem().ShaderResourceTexture.GetReference(), 0);
		}

		// Create a BlackDummy texture
		{
			FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(FIntPoint(1, 1), PF_B8G8R8A8, FClearValueBinding::Transparent, TexCreate_HideInVisualizeTexture, TexCreate_RenderTargetable | TexCreate_NoFastClear | TexCreate_ShaderResource, false));
			Desc.AutoWritable = false;
			GRenderTargetPool.FindFreeElement(RHICmdList, Desc, BlackDummy, TEXT("BlackDummy"), ERenderTargetTransience::NonTransient);

			RHICmdList.Transition(FRHITransitionInfo(BlackDummy->GetRenderTargetItem().TargetableTexture, ERHIAccess::SRVMask, ERHIAccess::RTV));

			FRHIRenderPassInfo RPInfo(BlackDummy->GetRenderTargetItem().TargetableTexture, ERenderTargetActions::Clear_Store);
			RHICmdList.BeginRenderPass(RPInfo, TEXT("BlackDummy"));
			RHICmdList.EndRenderPass();
			RHICmdList.CopyToResolveTarget(BlackDummy->GetRenderTargetItem().TargetableTexture, BlackDummy->GetRenderTargetItem().ShaderResourceTexture, FResolveParams());
		}
	
		// Create a texture that is a single UInt32 value set to 0
		{
			FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(FIntPoint(1,1), PF_R32_UINT, FClearValueBinding::Transparent, TexCreate_HideInVisualizeTexture, TexCreate_RenderTargetable | TexCreate_NoFastClear | TexCreate_ShaderResource, false));
			Desc.AutoWritable = false;
			GRenderTargetPool.FindFreeElement(RHICmdList, Desc, ZeroUIntDummy, TEXT("ZeroUIntDummy"), ERenderTargetTransience::NonTransient);

			RHICmdList.Transition(FRHITransitionInfo(ZeroUIntDummy->GetRenderTargetItem().TargetableTexture, ERHIAccess::SRVMask, ERHIAccess::RTV));

			FRHIRenderPassInfo RPInfo(ZeroUIntDummy->GetRenderTargetItem().TargetableTexture, ERenderTargetActions::Clear_Store);
			RHICmdList.BeginRenderPass(RPInfo, TEXT("ClearZeroUIntDummy"));
			RHICmdList.EndRenderPass();
			RHICmdList.CopyToResolveTarget(ZeroUIntDummy->GetRenderTargetItem().TargetableTexture, ZeroUIntDummy->GetRenderTargetItem().ShaderResourceTexture, FResolveParams());
		}

		// Create a texture that is a single 4xUInt16 (UShort) value set to 0
		{
			FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(FIntPoint(1, 1), PF_R16G16B16A16_UINT, FClearValueBinding::Transparent, TexCreate_HideInVisualizeTexture, TexCreate_RenderTargetable | TexCreate_NoFastClear | TexCreate_ShaderResource, false));
			Desc.AutoWritable = false;
			GRenderTargetPool.FindFreeElement(RHICmdList, Desc, ZeroUShort4Dummy, TEXT("ZeroUShort4Dummy"), ERenderTargetTransience::NonTransient);

			RHICmdList.Transition(FRHITransitionInfo(ZeroUShort4Dummy->GetRenderTargetItem().TargetableTexture, ERHIAccess::SRVMask, ERHIAccess::RTV));

			FRHIRenderPassInfo RPInfo(ZeroUShort4Dummy->GetRenderTargetItem().TargetableTexture, ERenderTargetActions::Clear_Store);
			RHICmdList.BeginRenderPass(RPInfo, TEXT("ClearZeroUShort4Dummy"));
			RHICmdList.EndRenderPass();
			RHICmdList.CopyToResolveTarget(ZeroUShort4Dummy->GetRenderTargetItem().TargetableTexture, ZeroUShort4Dummy->GetRenderTargetItem().ShaderResourceTexture, FResolveParams());
		}

		// Create a BlackAlphaOneDummy texture
		{
			FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(FIntPoint(1, 1), PF_B8G8R8A8, FClearValueBinding::Black, TexCreate_HideInVisualizeTexture, TexCreate_RenderTargetable | TexCreate_NoFastClear | TexCreate_ShaderResource, false));
			Desc.AutoWritable = false;
			GRenderTargetPool.FindFreeElement(RHICmdList, Desc, BlackAlphaOneDummy, TEXT("BlackAlphaOneDummy"), ERenderTargetTransience::NonTransient);

			RHICmdList.Transition(FRHITransitionInfo(BlackAlphaOneDummy->GetRenderTargetItem().TargetableTexture, ERHIAccess::SRVMask, ERHIAccess::RTV));

			FRHIRenderPassInfo RPInfo(BlackAlphaOneDummy->GetRenderTargetItem().TargetableTexture, ERenderTargetActions::Clear_Store);
			RHICmdList.BeginRenderPass(RPInfo, TEXT("BlackAlphaOneDummy"));
			RHICmdList.EndRenderPass();
			RHICmdList.CopyToResolveTarget(BlackAlphaOneDummy->GetRenderTargetItem().TargetableTexture, BlackAlphaOneDummy->GetRenderTargetItem().ShaderResourceTexture, FResolveParams());
		}

		// Create a GreenDummy texture
		{
			FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(FIntPoint(1, 1), PF_B8G8R8A8, FClearValueBinding::Green, TexCreate_HideInVisualizeTexture, TexCreate_RenderTargetable | TexCreate_NoFastClear | TexCreate_ShaderResource, false));
			Desc.AutoWritable = false;
			GRenderTargetPool.FindFreeElement(RHICmdList, Desc, GreenDummy, TEXT("GreenDummy"), ERenderTargetTransience::NonTransient);

			RHICmdList.Transition(FRHITransitionInfo(GreenDummy->GetRenderTargetItem().TargetableTexture, ERHIAccess::SRVMask, ERHIAccess::RTV));

			FRHIRenderPassInfo RPInfo(GreenDummy->GetRenderTargetItem().TargetableTexture, ERenderTargetActions::Clear_Store);
			RHICmdList.BeginRenderPass(RPInfo, TEXT("GreenDummy"));
			RHICmdList.EndRenderPass();
			RHICmdList.CopyToResolveTarget(GreenDummy->GetRenderTargetItem().TargetableTexture, GreenDummy->GetRenderTargetItem().ShaderResourceTexture, FResolveParams());
		}

		// Create a DefaultNormal8Bit texture
		{
			FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(FIntPoint(1, 1), PF_B8G8R8A8, FClearValueBinding::DefaultNormal8Bit, TexCreate_HideInVisualizeTexture, TexCreate_RenderTargetable | TexCreate_NoFastClear | TexCreate_ShaderResource, false));
			Desc.AutoWritable = false;
			GRenderTargetPool.FindFreeElement(RHICmdList, Desc, DefaultNormal8Bit, TEXT("DefaultNormal8Bit"), ERenderTargetTransience::NonTransient);

			RHICmdList.Transition(FRHITransitionInfo(DefaultNormal8Bit->GetRenderTargetItem().TargetableTexture, ERHIAccess::SRVMask, ERHIAccess::RTV));

			FRHIRenderPassInfo RPInfo(DefaultNormal8Bit->GetRenderTargetItem().TargetableTexture, ERenderTargetActions::Clear_Store);
			RHICmdList.BeginRenderPass(RPInfo, TEXT("DefaultNormal8Bit"));
			RHICmdList.EndRenderPass();
			RHICmdList.CopyToResolveTarget(DefaultNormal8Bit->GetRenderTargetItem().TargetableTexture, DefaultNormal8Bit->GetRenderTargetItem().ShaderResourceTexture, FResolveParams());
		}

		// Create the PerlinNoiseGradient texture
		{
			FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(FIntPoint(128, 128), PF_B8G8R8A8, FClearValueBinding::None, TexCreate_HideInVisualizeTexture, TexCreate_None | TexCreate_NoFastClear | TexCreate_ShaderResource, false));
			Desc.AutoWritable = false;
			GRenderTargetPool.FindFreeElement(RHICmdList, Desc, PerlinNoiseGradient, TEXT("PerlinNoiseGradient"), ERenderTargetTransience::NonTransient);
			// Write the contents of the texture.
			uint32 DestStride;
			uint8* DestBuffer = (uint8*)RHICmdList.LockTexture2D((FTexture2DRHIRef&)PerlinNoiseGradient->GetRenderTargetItem().ShaderResourceTexture, 0, RLM_WriteOnly, DestStride, false);
			// seed the pseudo random stream with a good value
			FRandomStream RandomStream(12345);
			// Values represent float3 values in the -1..1 range.
			// The vectors are the edge mid point of a cube from -1 .. 1
			static uint32 gradtable[] =
			{
				0x88ffff, 0xff88ff, 0xffff88,
				0x88ff00, 0xff8800, 0xff0088,
				0x8800ff, 0x0088ff, 0x00ff88,
				0x880000, 0x008800, 0x000088,
			};
			for (int32 y = 0; y < Desc.Extent.Y; ++y)
			{
				for (int32 x = 0; x < Desc.Extent.X; ++x)
				{
				uint32* Dest = (uint32*)(DestBuffer + x * sizeof(uint32) + y * DestStride);

					// pick a random direction (hacky way to overcome the quality issues FRandomStream has)
					*Dest = gradtable[(uint32)(RandomStream.GetFraction() * 11.9999999f)];
				}
			}
			RHICmdList.UnlockTexture2D((FTexture2DRHIRef&)PerlinNoiseGradient->GetRenderTargetItem().ShaderResourceTexture, 0, false);
		}

	if (GPixelFormats[PF_FloatRGBA].Supported)
	{
		FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(FIntPoint(1, 1), PF_FloatRGBA, FClearValueBinding(FLinearColor(65500.0f, 65500.0f, 65500.0f, 65500.0f)), TexCreate_HideInVisualizeTexture, TexCreate_RenderTargetable | TexCreate_NoFastClear | TexCreate_ShaderResource, false));
		Desc.AutoWritable = false;
		GRenderTargetPool.FindFreeElement(RHICmdList, Desc, MaxFP16Depth, TEXT("MaxFP16Depth"), ERenderTargetTransience::NonTransient);

		RHICmdList.Transition(FRHITransitionInfo(MaxFP16Depth->GetRenderTargetItem().TargetableTexture, ERHIAccess::SRVMask, ERHIAccess::RTV));

		FRHIRenderPassInfo RPInfo(MaxFP16Depth->GetRenderTargetItem().TargetableTexture, ERenderTargetActions::Clear_Store);
		RHICmdList.BeginRenderPass(RPInfo, TEXT("MaxFP16Depth"));
		RHICmdList.EndRenderPass();
		RHICmdList.CopyToResolveTarget(MaxFP16Depth->GetRenderTargetItem().TargetableTexture, MaxFP16Depth->GetRenderTargetItem().ShaderResourceTexture, FResolveParams());
	}

	// Create dummy 1x1 depth texture		
	{
		FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(FIntPoint(1, 1), PF_DepthStencil, FClearValueBinding::DepthFar, TexCreate_None, TexCreate_DepthStencilTargetable, false));
		Desc.AutoWritable = false;
		GRenderTargetPool.FindFreeElement(RHICmdList, Desc, DepthDummy, TEXT("DepthDummy"), ERenderTargetTransience::NonTransient);

		RHICmdList.Transition(FRHITransitionInfo(DepthDummy->GetRenderTargetItem().TargetableTexture, ERHIAccess::SRVMask, ERHIAccess::DSVWrite));

		FRHIRenderPassInfo RPInfo(DepthDummy->GetRenderTargetItem().TargetableTexture, EDepthStencilTargetActions::ClearDepthStencil_StoreDepthStencil, nullptr, FExclusiveDepthStencil::DepthWrite_StencilWrite);
		RHICmdList.BeginRenderPass(RPInfo, TEXT("DepthDummy"));
		RHICmdList.EndRenderPass();
		RHICmdList.CopyToResolveTarget(DepthDummy->GetRenderTargetItem().TargetableTexture, DepthDummy->GetRenderTargetItem().ShaderResourceTexture, FResolveParams());
	}

	// Create a dummy stencil SRV.
	{
		FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(FIntPoint(1, 1), PF_R8G8B8A8_UINT, FClearValueBinding::White, TexCreate_HideInVisualizeTexture, TexCreate_RenderTargetable | TexCreate_NoFastClear, false));
		Desc.AutoWritable = false;
		GRenderTargetPool.FindFreeElement(RHICmdList, Desc, StencilDummy, TEXT("StencilDummy"), ERenderTargetTransience::NonTransient);

		RHICmdList.Transition(FRHITransitionInfo(StencilDummy->GetRenderTargetItem().TargetableTexture, ERHIAccess::SRVMask, ERHIAccess::RTV));

		FRHIRenderPassInfo RPInfo(StencilDummy->GetRenderTargetItem().TargetableTexture, ERenderTargetActions::Clear_Store);
		RHICmdList.BeginRenderPass(RPInfo, TEXT("StencilDummy"));
		RHICmdList.EndRenderPass();
		RHICmdList.CopyToResolveTarget(StencilDummy->GetRenderTargetItem().TargetableTexture, StencilDummy->GetRenderTargetItem().ShaderResourceTexture, FResolveParams());

		StencilDummySRV = RHICreateShaderResourceView((FRHITexture2D*)StencilDummy->GetRenderTargetItem().ShaderResourceTexture.GetReference(), 0);
	}

	if (GPixelFormats[PF_FloatRGBA].Supported)
	{
		// PF_FloatRGBA to encode exactly the 0.5.
		FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(FIntPoint(1, 1), PF_FloatRGBA, FClearValueBinding(FLinearColor(0.5f, 0.5f, 0.5f, 0.5f)), TexCreate_HideInVisualizeTexture, TexCreate_RenderTargetable | TexCreate_NoFastClear | TexCreate_ShaderResource, false));
		Desc.AutoWritable = false;
		GRenderTargetPool.FindFreeElement(RHICmdList, Desc, MidGreyDummy, TEXT("MidGreyDummy"), ERenderTargetTransience::NonTransient);

		RHICmdList.Transition(FRHITransitionInfo(MidGreyDummy->GetRenderTargetItem().TargetableTexture, ERHIAccess::SRVMask, ERHIAccess::RTV));

		FRHIRenderPassInfo RPInfo(MidGreyDummy->GetRenderTargetItem().TargetableTexture, ERenderTargetActions::Clear_Store);
		RHICmdList.BeginRenderPass(RPInfo, TEXT("MidGreyDummy"));
		RHICmdList.EndRenderPass();
		RHICmdList.CopyToResolveTarget(MidGreyDummy->GetRenderTargetItem().TargetableTexture, MidGreyDummy->GetRenderTargetItem().ShaderResourceTexture, FResolveParams());
	}
}

void FSystemTextures::InitializeFeatureLevelDependentTextures(FRHICommandListImmediate& RHICmdList, const ERHIFeatureLevel::Type InFeatureLevel)
{
	// this function will be called every time the feature level will be updated and some textures require a minimum feature level to exist
	// the below declared variable (CurrentFeatureLevel) will guard against reinitialization of those textures already created in a previous call
	// if FeatureLevelInitializedTo has its default value (ERHIFeatureLevel::Num) it means that setup was never performed and all textures are invalid
	// thus CurrentFeatureLevel will be set to ERHIFeatureLevel::ES2_REMOVED to validate all 'is valid' branching conditions below
    ERHIFeatureLevel::Type CurrentFeatureLevel = FeatureLevelInitializedTo == ERHIFeatureLevel::Num ? ERHIFeatureLevel::ES2_REMOVED : FeatureLevelInitializedTo;

		// Create the SobolSampling texture
	if (CurrentFeatureLevel < ERHIFeatureLevel::ES3_1 && InFeatureLevel >= ERHIFeatureLevel::ES3_1 && GPixelFormats[PF_R16_UINT].Supported)
	{
		FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(FIntPoint(32, 16), PF_R16_UINT, FClearValueBinding::None, TexCreate_HideInVisualizeTexture, TexCreate_NoFastClear | TexCreate_ShaderResource, false));
		Desc.AutoWritable = false;
		GRenderTargetPool.FindFreeElement(RHICmdList, Desc, SobolSampling, TEXT("SobolSampling"));
		// Write the contents of the texture.
		uint32 DestStride;
		uint8* DestBuffer = (uint8*)RHICmdList.LockTexture2D((FTexture2DRHIRef&)SobolSampling->GetRenderTargetItem().ShaderResourceTexture, 0, RLM_WriteOnly, DestStride, false);

		uint16 *Dest;
		for (int y = 0; y < 16; ++y)
		{
			Dest = (uint16*)(DestBuffer + y * DestStride);

			// 16x16 block starting at 0,0 = Sobol X,Y from bottom 4 bits of cell X,Y
			for (int x = 0; x < 16; ++x, ++Dest)
			{
				*Dest = FSobol::ComputeGPUSpatialSeed(x, y, /* Index = */ 0);
			}

			// 16x16 block starting at 16,0 = Sobol X,Y from 2nd 4 bits of cell X,Y
			for (int x = 0; x < 16; ++x, ++Dest)
			{
				*Dest = FSobol::ComputeGPUSpatialSeed(x, y, /* Index = */ 1);
			}
		}
		RHICmdList.UnlockTexture2D((FTexture2DRHIRef&)SobolSampling->GetRenderTargetItem().ShaderResourceTexture, 0, false);
	}

	// Create a VolumetricBlackDummy texture
	if (CurrentFeatureLevel < ERHIFeatureLevel::SM5 && InFeatureLevel >= ERHIFeatureLevel::SM5)
	{
		FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::CreateVolumeDesc(1, 1, 1, PF_B8G8R8A8, FClearValueBinding::Transparent, TexCreate_HideInVisualizeTexture, TexCreate_ShaderResource | TexCreate_RenderTargetable | TexCreate_NoFastClear, false));
		Desc.AutoWritable = false;
		GRenderTargetPool.FindFreeElement(RHICmdList, Desc, VolumetricBlackDummy, TEXT("VolumetricBlackDummy"), ERenderTargetTransience::NonTransient);

		const uint8 BlackBytes[4] = { 0, 0, 0, 0 };
		FUpdateTextureRegion3D Region(0, 0, 0, 0, 0, 0, Desc.Extent.X, Desc.Extent.Y, Desc.Depth);
		RHICmdList.UpdateTexture3D(
			(FTexture3DRHIRef&)VolumetricBlackDummy->GetRenderTargetItem().ShaderResourceTexture,
			0,
			Region,
			Desc.Extent.X * sizeof(BlackBytes),
			Desc.Extent.X * Desc.Extent.Y * sizeof(BlackBytes),
			BlackBytes);

		// UpdateTexture3D before and after state is currently undefined
		RHICmdList.Transition(FRHITransitionInfo(VolumetricBlackDummy->GetTargetableRHI(), ERHIAccess::Unknown, ERHIAccess::SRVMask));
	}

	if (CurrentFeatureLevel < ERHIFeatureLevel::SM5 && InFeatureLevel >= ERHIFeatureLevel::SM5)
	{
		FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::CreateVolumeDesc(1, 1, 1, PF_B8G8R8A8, FClearValueBinding::Transparent, TexCreate_HideInVisualizeTexture, TexCreate_ShaderResource | TexCreate_RenderTargetable | TexCreate_NoFastClear, false));
		Desc.AutoWritable = false;
		GRenderTargetPool.FindFreeElement(RHICmdList, Desc, HairLUT0, TEXT("HairLUT0"), ERenderTargetTransience::NonTransient);

		// Init with dummy textures. The texture will be initialize with real values if needed
		const uint8 BlackBytes[4] = { 0, 0, 0, 0 };
		FUpdateTextureRegion3D Region(0, 0, 0, 0, 0, 0, Desc.Extent.X, Desc.Extent.Y, Desc.Depth);
		RHICmdList.UpdateTexture3D((FTexture3DRHIRef&)HairLUT0->GetRenderTargetItem().ShaderResourceTexture, 0, Region, Desc.Extent.X * sizeof(BlackBytes), Desc.Extent.X * Desc.Extent.Y * sizeof(BlackBytes), BlackBytes);

		// UpdateTexture3D before and after state is currently undefined
		RHICmdList.Transition(FRHITransitionInfo(HairLUT0->GetRenderTargetItem().ShaderResourceTexture, ERHIAccess::Unknown, ERHIAccess::SRVMask));
		HairLUT1 = HairLUT0;
		HairLUT2 = HairLUT0;
	}

	// The PreintegratedGF maybe used on forward shading inluding mobile platorm, intialize it anyway.
	{
		// for testing, with 128x128 R8G8 we are very close to the reference (if lower res is needed we might have to add an offset to counter the 0.5f texel shift)
		const bool bReference = false;

		EPixelFormat Format = PF_R8G8;
		// for low roughness we would get banding with PF_R8G8 but for low spec it could be used, for now we don't do this optimization
		if (GPixelFormats[PF_G16R16].Supported)
		{
			Format = PF_G16R16;
		}

		FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(FIntPoint(128, 32), Format, FClearValueBinding::None, TexCreate_None, TexCreate_ShaderResource, false));
		Desc.AutoWritable = false;
		if (bReference)
		{
			Desc.Extent.X = 128;
			Desc.Extent.Y = 128;
		}

		GRenderTargetPool.FindFreeElement(RHICmdList, Desc, PreintegratedGF, TEXT("PreintegratedGF"), ERenderTargetTransience::NonTransient);
		// Write the contents of the texture.
		uint32 DestStride;
		uint8* DestBuffer = (uint8*)RHICmdList.LockTexture2D((FTexture2DRHIRef&)PreintegratedGF->GetRenderTargetItem().ShaderResourceTexture, 0, RLM_WriteOnly, DestStride, false);

		// x is NoV, y is roughness
		for (int32 y = 0; y < Desc.Extent.Y; y++)
		{
			float Roughness = (float)(y + 0.5f) / Desc.Extent.Y;
			float m = Roughness * Roughness;
			float m2 = m * m;

			for (int32 x = 0; x < Desc.Extent.X; x++)
			{
				float NoV = (float)(x + 0.5f) / Desc.Extent.X;

				FVector3f V;
				V.X = FMath::Sqrt(1.0f - NoV * NoV);	// sin
				V.Y = 0.0f;
				V.Z = NoV;								// cos

				float A = 0.0f;
				float B = 0.0f;
				float C = 0.0f;

				const uint32 NumSamples = 128;
				for (uint32 i = 0; i < NumSamples; i++)
				{
					float E1 = (float)i / NumSamples;
					float E2 = (double)ReverseBits(i) / (double)0x100000000LL;

					{
						float Phi = 2.0f * PI * E1;
						float CosPhi = FMath::Cos(Phi);
						float SinPhi = FMath::Sin(Phi);
						float CosTheta = FMath::Sqrt((1.0f - E2) / (1.0f + (m2 - 1.0f) * E2));
						float SinTheta = FMath::Sqrt(1.0f - CosTheta * CosTheta);

						FVector3f H(SinTheta * FMath::Cos(Phi), SinTheta * FMath::Sin(Phi), CosTheta);
						FVector3f L = 2.0f * (V | H) * H - V;

						float NoL = FMath::Max(L.Z, 0.0f);
						float NoH = FMath::Max(H.Z, 0.0f);
						float VoH = FMath::Max(V | H, 0.0f);

						if (NoL > 0.0f)
						{
							float Vis_SmithV = NoL * (NoV * (1 - m) + m);
							float Vis_SmithL = NoV * (NoL * (1 - m) + m);
							float Vis = 0.5f / (Vis_SmithV + Vis_SmithL);

							float NoL_Vis_PDF = NoL * Vis * (4.0f * VoH / NoH);
							float Fc = 1.0f - VoH;
							Fc *= FMath::Square(Fc*Fc);
							A += NoL_Vis_PDF * (1.0f - Fc);
							B += NoL_Vis_PDF * Fc;
						}
					}

					{
						float Phi = 2.0f * PI * E1;
						float CosPhi = FMath::Cos(Phi);
						float SinPhi = FMath::Sin(Phi);
						float CosTheta = FMath::Sqrt(E2);
						float SinTheta = FMath::Sqrt(1.0f - CosTheta * CosTheta);

						FVector3f L(SinTheta * FMath::Cos(Phi), SinTheta * FMath::Sin(Phi), CosTheta);
						FVector3f H = (V + L).GetUnsafeNormal();

						float NoL = FMath::Max(L.Z, 0.0f);
						float NoH = FMath::Max(H.Z, 0.0f);
						float VoH = FMath::Max(V | H, 0.0f);

						float FD90 = 0.5f + 2.0f * VoH * VoH * Roughness;
						float FdV = 1.0f + (FD90 - 1.0f) * pow(1.0f - NoV, 5);
						float FdL = 1.0f + (FD90 - 1.0f) * pow(1.0f - NoL, 5);
						C += FdV * FdL;// * ( 1.0f - 0.3333f * Roughness );
					}
				}
				A /= NumSamples;
				B /= NumSamples;
				C /= NumSamples;

				if (Desc.Format == PF_A16B16G16R16)
				{
					uint16* Dest = (uint16*)(DestBuffer + x * 8 + y * DestStride);
					Dest[0] = (int32)(FMath::Clamp(A, 0.0f, 1.0f) * 65535.0f + 0.5f);
					Dest[1] = (int32)(FMath::Clamp(B, 0.0f, 1.0f) * 65535.0f + 0.5f);
					Dest[2] = (int32)(FMath::Clamp(C, 0.0f, 1.0f) * 65535.0f + 0.5f);
				}
				else if (Desc.Format == PF_G16R16)
				{
					uint16* Dest = (uint16*)(DestBuffer + x * 4 + y * DestStride);
					Dest[0] = (int32)(FMath::Clamp(A, 0.0f, 1.0f) * 65535.0f + 0.5f);
					Dest[1] = (int32)(FMath::Clamp(B, 0.0f, 1.0f) * 65535.0f + 0.5f);
				}
				else
				{
					check(Desc.Format == PF_R8G8);

					uint8* Dest = (uint8*)(DestBuffer + x * 2 + y * DestStride);
					Dest[0] = (int32)(FMath::Clamp(A, 0.0f, 1.0f) * 255.f + 0.5f);
					Dest[1] = (int32)(FMath::Clamp(B, 0.0f, 1.0f) * 255.f + 0.5f);
				}
			}
		}
		RHICmdList.UnlockTexture2D((FTexture2DRHIRef&)PreintegratedGF->GetRenderTargetItem().ShaderResourceTexture, 0, false);
	}

	if (CurrentFeatureLevel < ERHIFeatureLevel::SM5 && InFeatureLevel >= ERHIFeatureLevel::SM5)
	{
	    // Create the PerlinNoise3D texture (similar to http://prettyprocs.wordpress.com/2012/10/20/fast-perlin-noise/)
	    {
		    uint32 Extent = 16;
    
		    const uint32 Square = Extent * Extent;
    
		    FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::CreateVolumeDesc(Extent, Extent, Extent, PF_B8G8R8A8, FClearValueBinding::None, TexCreate_ShaderResource | TexCreate_HideInVisualizeTexture | TexCreate_NoTiling, TexCreate_ShaderResource, false));
		    Desc.AutoWritable = false;
		    GRenderTargetPool.FindFreeElement(RHICmdList, Desc, PerlinNoise3D, TEXT("PerlinNoise3D"), ERenderTargetTransience::NonTransient);
		    // Write the contents of the texture.
		    TArray<uint32> DestBuffer;
    
		    DestBuffer.AddZeroed(Extent * Extent * Extent);
		    // seed the pseudo random stream with a good value
		    FRandomStream RandomStream(0x1234);
		    // Values represent float3 values in the -1..1 range.
		    // The vectors are the edge mid point of a cube from -1 .. 1
		    // -1:0 0:7f 1:fe, can be reconstructed with * 512/254 - 1
		    // * 2 - 1 cannot be used because 0 would not be mapped
			    static uint32 gradtable[] =
		    {
			    0x7ffefe, 0xfe7ffe, 0xfefe7f,
			    0x7ffe00, 0xfe7f00, 0xfe007f,
			    0x7f00fe, 0x007ffe, 0x00fe7f,
			    0x7f0000, 0x007f00, 0x00007f,
		    };
		    // set random directions
		    {
				    for (uint32 z = 0; z < Extent - 1; ++z)
			    {
					    for (uint32 y = 0; y < Extent - 1; ++y)
				    {
						    for (uint32 x = 0; x < Extent - 1; ++x)
					    {
						    uint32& Value = DestBuffer[x + y * Extent + z * Square];
    
						    // pick a random direction (hacky way to overcome the quality issues FRandomStream has)
						    Value = gradtable[(uint32)(RandomStream.GetFraction() * 11.9999999f)];
					    }
				    }
			    }
		    }
		    // replicate a border for filtering
		    {
			    uint32 Last = Extent - 1;
    
				    for (uint32 z = 0; z < Extent; ++z)
			    {
					    for (uint32 y = 0; y < Extent; ++y)
				    {
					    DestBuffer[Last + y * Extent + z * Square] = DestBuffer[0 + y * Extent + z * Square];
				    }
			    }
				for (uint32 z = 0; z < Extent; ++z)
				{
					for (uint32 x = 0; x < Extent; ++x)
					{
					    DestBuffer[x + Last * Extent + z * Square] = DestBuffer[x + 0 * Extent + z * Square];
				    }
			    }
				for (uint32 y = 0; y < Extent; ++y)
				{
					for (uint32 x = 0; x < Extent; ++x)
				    {
					    DestBuffer[x + y * Extent + Last * Square] = DestBuffer[x + y * Extent + 0 * Square];
				    }
			    }
		    }
		    // precompute gradients
			{
			    uint32* Dest = DestBuffer.GetData();
    
				for (uint32 z = 0; z < Desc.Depth; ++z)
			    {
					for (uint32 y = 0; y < (uint32)Desc.Extent.Y; ++y)
				    {
						for (uint32 x = 0; x < (uint32)Desc.Extent.X; ++x)
					    {
						    uint32 Value = *Dest;
    
						    // todo: check if rgb order is correct
						    int32 r = Value >> 16;
						    int32 g = (Value >> 8) & 0xff;
						    int32 b = Value & 0xff;
    
						    int nx = (r / 0x7f) - 1;
						    int ny = (g / 0x7f) - 1;
						    int nz = (b / 0x7f) - 1;
    
						    int32 d = nx * x + ny * y + nz * z;
    
						    // compress in 8bit
						    uint32 a = d + 127;
    
						    *Dest++ = Value | (a << 24);
					    }
				    }
			    }
		    }
    
		    FUpdateTextureRegion3D Region(0, 0, 0, 0, 0, 0, Desc.Extent.X, Desc.Extent.Y, Desc.Depth);
    
		    RHICmdList.UpdateTexture3D(
			    (FTexture3DRHIRef&)PerlinNoise3D->GetRenderTargetItem().ShaderResourceTexture,
			    0,
			    Region,
			    Desc.Extent.X * sizeof(uint32),
			    Desc.Extent.X * Desc.Extent.Y * sizeof(uint32),
			    (const uint8*)DestBuffer.GetData());
		} // end Create the PerlinNoise3D texture

		// GTAO Randomization texture	
		{

		    {
			    FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(FIntPoint(LTC_Size, LTC_Size), PF_FloatRGBA, FClearValueBinding::None, TexCreate_FastVRAM, TexCreate_ShaderResource, false));
			    Desc.AutoWritable = false;
    
			    GRenderTargetPool.FindFreeElement(RHICmdList, Desc, LTCMat, TEXT("LTCMat"));
			    // Write the contents of the texture.
			    uint32 DestStride;
			    uint8* DestBuffer = (uint8*)RHICmdList.LockTexture2D((FTexture2DRHIRef&)LTCMat->GetRenderTargetItem().ShaderResourceTexture, 0, RLM_WriteOnly, DestStride, false);
    
				    for (int32 y = 0; y < Desc.Extent.Y; ++y)
			    {
					    for (int32 x = 0; x < Desc.Extent.X; ++x)
				    {
					    uint16* Dest = (uint16*)(DestBuffer + x * 4 * sizeof(uint16) + y * DestStride);
    
						    for (int k = 0; k < 4; k++)
							    Dest[k] = FFloat16(LTC_Mat[4 * (x + y * LTC_Size) + k]).Encoded;
				    }
			    }
			    RHICmdList.UnlockTexture2D((FTexture2DRHIRef&)LTCMat->GetRenderTargetItem().ShaderResourceTexture, 0, false);
		    }
    
		    {
			    FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(FIntPoint(LTC_Size, LTC_Size), PF_G16R16F, FClearValueBinding::None, TexCreate_FastVRAM, TexCreate_ShaderResource, false));
			    Desc.AutoWritable = false;
    
			    GRenderTargetPool.FindFreeElement(RHICmdList, Desc, LTCAmp, TEXT("LTCAmp"));
			    // Write the contents of the texture.
			    uint32 DestStride;
			    uint8* DestBuffer = (uint8*)RHICmdList.LockTexture2D((FTexture2DRHIRef&)LTCAmp->GetRenderTargetItem().ShaderResourceTexture, 0, RLM_WriteOnly, DestStride, false);
    
				for (int32 y = 0; y < Desc.Extent.Y; ++y)
			    {
					for (int32 x = 0; x < Desc.Extent.X; ++x)
				    {
					    uint16* Dest = (uint16*)(DestBuffer + x * 2 * sizeof(uint16) + y * DestStride);
    
						for (int k = 0; k < 2; k++)
						    Dest[k] = FFloat16(LTC_Amp[4 * (x + y * LTC_Size) + k]).Encoded;
				    }
			    }
			    RHICmdList.UnlockTexture2D((FTexture2DRHIRef&)LTCAmp->GetRenderTargetItem().ShaderResourceTexture, 0, false);
		    }
		} // end Create the GTAO  randomization texture
	} // end if (FeatureLevelInitializedTo < ERHIFeatureLevel::SM5 && InFeatureLevel >= ERHIFeatureLevel::SM5)

	// Create the SSAO randomization texture
	static const auto MobileAmbientOcclusionCVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.Mobile.AmbientOcclusion"));
	if ((CurrentFeatureLevel < ERHIFeatureLevel::SM5 && InFeatureLevel >= ERHIFeatureLevel::SM5) ||
		(CurrentFeatureLevel < ERHIFeatureLevel::ES3_1 && InFeatureLevel >= ERHIFeatureLevel::ES3_1 && MobileAmbientOcclusionCVar != nullptr && MobileAmbientOcclusionCVar->GetValueOnAnyThread()>0))
	{
		{
			float g_AngleOff1 = 127;
			float g_AngleOff2 = 198;
			float g_AngleOff3 = 23;

			FColor Bases[16];

			for (int32 Pos = 0; Pos < 16; ++Pos)
			{
				// distribute rotations over 4x4 pattern
						//			int32 Reorder[16] = { 0, 8, 2, 10, 12, 6, 14, 4, 3, 11, 1, 9, 15, 5, 13, 7 };
				int32 Reorder[16] = { 0, 11, 7, 3, 10, 4, 15, 12, 6, 8, 1, 14, 13, 2, 9, 5 };
				int32 w = Reorder[Pos];

				// ordered sampling of the rotation basis (*2 is missing as we use mirrored samples)
				float ww = w / 16.0f * PI;

				// randomize base scale
				float lenm = 1.0f - (FMath::Sin(g_AngleOff2 * w * 0.01f) * 0.5f + 0.5f) * g_AngleOff3 * 0.01f;
				float s = FMath::Sin(ww) * lenm;
				float c = FMath::Cos(ww) * lenm;

				Bases[Pos] = FColor(FMath::Quantize8SignedByte(c), FMath::Quantize8SignedByte(s), 0, 0);
			}

			{
				// could be PF_V8U8 to save shader instructions but that doesn't work on all hardware
				FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(FIntPoint(64, 64), PF_R8G8, FClearValueBinding::None, TexCreate_HideInVisualizeTexture, TexCreate_NoFastClear | TexCreate_ShaderResource, false));
				Desc.AutoWritable = false;
				GRenderTargetPool.FindFreeElement(RHICmdList, Desc, SSAORandomization, TEXT("SSAORandomization"), ERenderTargetTransience::NonTransient);
				// Write the contents of the texture.
				uint32 DestStride;
				uint8* DestBuffer = (uint8*)RHICmdList.LockTexture2D((FTexture2DRHIRef&)SSAORandomization->GetRenderTargetItem().ShaderResourceTexture, 0, RLM_WriteOnly, DestStride, false);

				for (int32 y = 0; y < Desc.Extent.Y; ++y)
				{
					for (int32 x = 0; x < Desc.Extent.X; ++x)
					{
						uint8* Dest = (uint8*)(DestBuffer + x * sizeof(uint16) + y * DestStride);

						uint32 Index = (x % 4) + (y % 4) * 4;

						Dest[0] = Bases[Index].R;
						Dest[1] = Bases[Index].G;
					}
				}
			}
			RHICmdList.UnlockTexture2D((FTexture2DRHIRef&)SSAORandomization->GetRenderTargetItem().ShaderResourceTexture, 0, false);
		}
	}
		
	static const auto MobileGTAOPreIntegratedTextureTypeCVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.Mobile.GTAOPreIntegratedTextureType"));

	if (CurrentFeatureLevel < ERHIFeatureLevel::ES3_1 && InFeatureLevel >= ERHIFeatureLevel::ES3_1 && MobileGTAOPreIntegratedTextureTypeCVar && MobileGTAOPreIntegratedTextureTypeCVar->GetValueOnAnyThread() > 0)
	{
		uint32 Extent = 16; // should be consistent with LUTSize in PostprocessMobile.usf

		const uint32 Square = Extent * Extent;

		bool bGTAOPreIngegratedUsingVolumeLUT = MobileGTAOPreIntegratedTextureTypeCVar->GetValueOnAnyThread() == 2;

		FPooledRenderTargetDesc Desc;
		if (bGTAOPreIngegratedUsingVolumeLUT)
		{
			Desc = FPooledRenderTargetDesc(FPooledRenderTargetDesc::CreateVolumeDesc(Extent, Extent, Extent, PF_R16F, FClearValueBinding::None, TexCreate_HideInVisualizeTexture | TexCreate_NoTiling | TexCreate_ShaderResource, TexCreate_ShaderResource, false));
		}
		else
		{
			Desc = FPooledRenderTargetDesc(FPooledRenderTargetDesc::Create2DDesc(FIntPoint(Square, Extent), PF_R16F, FClearValueBinding::None, TexCreate_HideInVisualizeTexture | TexCreate_NoTiling | TexCreate_ShaderResource, TexCreate_ShaderResource, false));
		}
		
		Desc.AutoWritable = false;
		GRenderTargetPool.FindFreeElement(RHICmdList, Desc, GTAOPreIntegrated, TEXT("GTAOPreIntegrated"), ERenderTargetTransience::NonTransient);

		// Write the contents of the texture.
		TArray<FFloat16> TempBuffer;
		TempBuffer.AddZeroed(Extent * Extent * Extent);

		FFloat16* DestBuffer = nullptr;

		if (bGTAOPreIngegratedUsingVolumeLUT)
		{
			DestBuffer = TempBuffer.GetData();
		}
		else
		{
			uint32 DestStride;
			DestBuffer = (FFloat16*)RHICmdList.LockTexture2D((FTexture2DRHIRef&)GTAOPreIntegrated->GetRenderTargetItem().ShaderResourceTexture, 0, RLM_WriteOnly, DestStride, false);
		}

		for (uint32 z = 0; z < Extent; ++z)
		{
			for (uint32 y = 0; y < Extent; ++y)
			{
				for (uint32 x = 0; x < Extent; ++x)
				{
					uint32 DestBufferIndex = 0;

					if (bGTAOPreIngegratedUsingVolumeLUT)
					{
						DestBufferIndex = x + y * Extent + z * Square;
					}
					else
					{
						DestBufferIndex = (x + z * Extent) + y * Square;
					}
					FFloat16& Value = DestBuffer[DestBufferIndex];

					float cosAngle1 = ((x + 0.5f) / (Extent) - 0.5f) * 2;
					float cosAngle2 = ((y + 0.5f) / (Extent) - 0.5f) * 2;
					float cosAng = ((z + 0.5f) / (Extent) - 0.5f) * 2;

					float Gamma = FMath::Acos(cosAng) - HALF_PI;
					float CosGamma = FMath::Cos(Gamma);
					float SinGamma = cosAng * -2.0f;

					float Angle1 = FMath::Acos(cosAngle1);
					float Angle2 = FMath::Acos(cosAngle2);
					// clamp to normal hemisphere 
					Angle1 = Gamma + FMath::Max(-Angle1 - Gamma, -(HALF_PI));
					Angle2 = Gamma + FMath::Min(Angle2 - Gamma, (HALF_PI));

					float AO = (0.25f *
						((Angle1 * SinGamma + CosGamma - cos((2.0 * Angle1) - Gamma)) +
						(Angle2 * SinGamma + CosGamma - cos((2.0 * Angle2) - Gamma))));

					Value = AO;
				}
			}
		}

		if (bGTAOPreIngegratedUsingVolumeLUT)
		{
			FUpdateTextureRegion3D Region(0, 0, 0, 0, 0, 0, Desc.Extent.X, Desc.Extent.Y, Desc.Depth);

			RHICmdList.UpdateTexture3D(
				(FTexture3DRHIRef&)GTAOPreIntegrated->GetRenderTargetItem().ShaderResourceTexture,
				0,
				Region,
				Desc.Extent.X * sizeof(FFloat16),
				Desc.Extent.X * Desc.Extent.Y * sizeof(FFloat16),
				(const uint8*)DestBuffer);
		}
		else
		{
			RHICmdList.UnlockTexture2D((FTexture2DRHIRef&)GTAOPreIntegrated->GetRenderTargetItem().ShaderResourceTexture, 0, false);
		}
	}

	// Initialize textures only once.
	FeatureLevelInitializedTo = InFeatureLevel;
}

void FSystemTextures::ReleaseDynamicRHI()
{
	WhiteDummySRV.SafeRelease();
	WhiteDummy.SafeRelease();
	BlackDummy.SafeRelease();
	BlackAlphaOneDummy.SafeRelease();
	PerlinNoiseGradient.SafeRelease();
	PerlinNoise3D.SafeRelease();
	SobolSampling.SafeRelease();
	SSAORandomization.SafeRelease();
	GTAOPreIntegrated.SafeRelease();
	PreintegratedGF.SafeRelease();
	HairLUT0.SafeRelease();
	HairLUT1.SafeRelease();
	HairLUT2.SafeRelease();
	LTCMat.SafeRelease();
	LTCAmp.SafeRelease();
	MaxFP16Depth.SafeRelease();
	DepthDummy.SafeRelease();
	GreenDummy.SafeRelease();
	DefaultNormal8Bit.SafeRelease();
	VolumetricBlackDummy.SafeRelease();
	ZeroUIntDummy.SafeRelease();
	ZeroUShort4Dummy.SafeRelease();
	MidGreyDummy.SafeRelease();
	StencilDummy.SafeRelease();
	StencilDummySRV.SafeRelease();
	GTAOPreIntegrated.SafeRelease();

	DefaultTextures.Empty();
	DefaultBuffers.Empty();
	HashDefaultTextures.Clear();
	HashDefaultBuffers.Clear();

	GRenderTargetPool.FreeUnusedResources();

	// Indicate that textures will need to be reinitialized.
	FeatureLevelInitializedTo = ERHIFeatureLevel::Num;
}

FRDGTextureRef FSystemTextures::GetBlackDummy(FRDGBuilder& GraphBuilder) const
{
	return GraphBuilder.RegisterExternalTexture(BlackDummy, TEXT("BlackDummy"));
}

FRDGTextureRef FSystemTextures::GetBlackAlphaOneDummy(FRDGBuilder& GraphBuilder) const
{
	return GraphBuilder.RegisterExternalTexture(BlackAlphaOneDummy, TEXT("BlackAlphaOneDummy"));
}

FRDGTextureRef FSystemTextures::GetWhiteDummy(FRDGBuilder& GraphBuilder) const
{
	return GraphBuilder.RegisterExternalTexture(WhiteDummy, TEXT("WhiteDummy"));
}

FRDGTextureRef FSystemTextures::GetMaxFP16Depth(FRDGBuilder& GraphBuilder) const
{
	return GraphBuilder.RegisterExternalTexture(MaxFP16Depth, TEXT("MaxFP16Depth"));
}

FRDGTextureRef FSystemTextures::GetDepthDummy(FRDGBuilder& GraphBuilder) const
{
	return GraphBuilder.RegisterExternalTexture(DepthDummy, TEXT("DepthDummy"));
}

FRDGTextureRef FSystemTextures::GetStencilDummy(FRDGBuilder& GraphBuilder) const
{
	return GraphBuilder.RegisterExternalTexture(StencilDummy, TEXT("StencilDummy"));
}

FRDGTextureRef FSystemTextures::GetGreenDummy(FRDGBuilder& GraphBuilder) const
{
	return GraphBuilder.RegisterExternalTexture(GreenDummy, TEXT("GreenDummy"));
}

FRDGTextureRef FSystemTextures::GetDefaultNormal8Bit(FRDGBuilder& GraphBuilder) const
{
	return GraphBuilder.RegisterExternalTexture(DefaultNormal8Bit, TEXT("DefaultNormal8Bit"));
}

FRDGTextureRef FSystemTextures::GetMidGreyDummy(FRDGBuilder& GraphBuilder) const
{
	return GraphBuilder.RegisterExternalTexture(MidGreyDummy, TEXT("MidGreyDummy"));
}

FRDGTextureRef FSystemTextures::GetVolumetricBlackDummy(FRDGBuilder& GraphBuilder) const
{
	return GraphBuilder.RegisterExternalTexture(VolumetricBlackDummy, TEXT("VolumetricBlackDummy"));
}

FRDGTextureRef FSystemTextures::GetZeroUIntDummy(FRDGBuilder& GraphBuilder) const
{
	return GraphBuilder.RegisterExternalTexture(ZeroUIntDummy, TEXT("ZeroUIntDummy"));
}

FRDGTextureRef FSystemTextures::GetZeroUShort4Dummy(FRDGBuilder& GraphBuilder) const
{
	return GraphBuilder.RegisterExternalTexture(ZeroUShort4Dummy, TEXT("ZeroUShort4Dummy"));
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////
// Default textures

bool operator !=(const FDefaultTextureKey& A, const FDefaultTextureKey& B)
{
	return A.Format != B.Format ||
		A.Dimension != B.Dimension ||
		A.ValueAsUInt[0] != B.ValueAsUInt[0] ||
		A.ValueAsUInt[1] != B.ValueAsUInt[1] ||
		A.ValueAsUInt[2] != B.ValueAsUInt[2] ||
		A.ValueAsUInt[3] != B.ValueAsUInt[3];
}

template<typename T>
static FDefaultTextureKey GetDefaultTextureKey(EPixelFormat Format, const T& In)
{
	FDefaultTextureKey Out;
	const uint32 Size = sizeof(T);
	const uint32* InAsUInt = (const uint32*)&In;
	Out.ValueAsUInt[0] = InAsUInt[0];
	Out.ValueAsUInt[1] = Size > 4  ? InAsUInt[1] : 0u;
	Out.ValueAsUInt[2] = Size > 8  ? InAsUInt[2] : 0u;
	Out.ValueAsUInt[3] = Size > 12 ? InAsUInt[3] : 0u;
	Out.Format = Format;
	return Out;
}

// Convert from X to 4 components data float/uint/int. Supported input are:
// * float
// * int32
// * uint32
// * FVector2D
// * FIntPoint
// * FVector
// * FVector4
// * FUintVector4
// * FClearValueBinding
FIntVector4		ToVector(int32 Value)						{ return FIntVector4(Value, Value, Value, Value); }
FVector4		ToVector(float Value)						{ return FVector4(Value, Value, Value, Value); }
FUintVector4	ToVector(uint32 Value)						{ return FUintVector4(Value, Value, Value, Value); }
FVector4		ToVector(const FVector & Value)				{ return FVector4(Value.X, Value.Y, Value.Z, 0); }
FVector4		ToVector(const FVector4 & Value)			{ return Value; }
FVector4		ToVector(const FVector2D& Value)			{ return FVector4(Value.X, Value.Y, 0, 0); }
FIntVector4		ToVector(const FIntPoint& Value)			{ return FIntVector4(Value.X, Value.Y, 0, 0); }
FUintVector4	ToVector(const FUintVector4& Value)			{ return Value; }
FVector4		ToVector(const FClearValueBinding & Value)	{ return FVector4(Value.Value.Color[0], Value.Value.Color[1], Value.Value.Color[2], Value.Value.Color[3]); }

template <typename TInputType>	struct TFormatConversionTraits				{ /*Error*/ };
template <>						struct TFormatConversionTraits<FVector4>	{ typedef float  Type; };
template <>						struct TFormatConversionTraits<FUintVector4>{ typedef uint32 Type; };
template <>						struct TFormatConversionTraits<FIntVector4> { typedef int32  Type; };

enum class EDefaultInputType
{
	Typed,
	UNorm,
	SNorm,
	UNorm10,
	UNorm11,
	UNorm2
};

// Convert input type into the final type. This function manages UNorm/SNorm type by assuming if the input if float, its value is normalized in [0..1].
template <typename TInType, typename TOutType, EDefaultInputType InputFormatType>
TOutType ConvertInputFormat(const TInType& In)
{
	return TOutType(In);
}
template<> uint32 ConvertInputFormat<float, uint32, EDefaultInputType::UNorm>(const float& In) { return FMath::Clamp(In,  0.f, 1.f) * float(MAX_uint32); }
template<>  int32 ConvertInputFormat<float,  int32, EDefaultInputType::SNorm>(const float& In) { return FMath::Clamp(In, -1.f, 1.f) * float(MAX_int32); }
template<> uint16 ConvertInputFormat<float, uint16, EDefaultInputType::UNorm>(const float& In) { return FMath::Clamp(In,  0.f, 1.f) * MAX_uint16; }
template<>  int16 ConvertInputFormat<float,  int16, EDefaultInputType::SNorm>(const float& In) { return FMath::Clamp(In, -1.f, 1.f) * MAX_int16; }
template<>  uint8 ConvertInputFormat<float,  uint8, EDefaultInputType::UNorm>(const float& In) { return FMath::Clamp(In,  0.f, 1.f) * MAX_uint8; }
template<>   int8 ConvertInputFormat<float,   int8, EDefaultInputType::SNorm>(const float& In) { return FMath::Clamp(In, -1.f, 1.f) * MAX_int8; }

template<> uint32 ConvertInputFormat<float, uint32, EDefaultInputType::UNorm10>(const float& In) { return FMath::Clamp(In, 0.f, 1.f) * 1024u; }
template<> uint32 ConvertInputFormat<float, uint32, EDefaultInputType::UNorm11>(const float& In) { return FMath::Clamp(In, 0.f, 1.f) * 2048u; }
template<> uint32 ConvertInputFormat<float, uint32, EDefaultInputType::UNorm2> (const float& In) { return FMath::Clamp(In, 0.f, 1.f) * 3u; }

// 4 components conversion with swizzling
template <EDefaultInputType InputFormatType, typename TInType, typename TOutType, uint32 SwizzleX, uint32 SwizzleY, uint32 SwizzleZ, uint32 SwizzleW>
void FormatData(const TInType& In, uint8* Out, uint32& OutByteCount)
{
	TOutType* OutTyped = (TOutType*)Out;
	OutTyped[0] = ConvertInputFormat<typename TFormatConversionTraits<TInType>::Type, TOutType, InputFormatType>(In[SwizzleX]);
	OutTyped[1] = ConvertInputFormat<typename TFormatConversionTraits<TInType>::Type, TOutType, InputFormatType>(In[SwizzleY]);
	OutTyped[2] = ConvertInputFormat<typename TFormatConversionTraits<TInType>::Type, TOutType, InputFormatType>(In[SwizzleZ]);
	OutTyped[3] = ConvertInputFormat<typename TFormatConversionTraits<TInType>::Type, TOutType, InputFormatType>(In[SwizzleW]);
	OutByteCount = 4 * sizeof(TOutType);
}

// 3 components conversion with swizzling
template <EDefaultInputType InputFormatType, typename TInType, typename TOutType, uint32 SwizzleX, uint32 SwizzleY, uint32 SwizzleZ>
void FormatData(const TInType& In, uint8* Out, uint32& OutByteCount)
{
	TOutType* OutTyped = (TOutType*)Out;
	OutTyped[0] = ConvertInputFormat<typename TFormatConversionTraits<TInType>::Type, TOutType, InputFormatType>(In[SwizzleX]);
	OutTyped[1] = ConvertInputFormat<typename TFormatConversionTraits<TInType>::Type, TOutType, InputFormatType>(In[SwizzleY]);
	OutTyped[2] = ConvertInputFormat<typename TFormatConversionTraits<TInType>::Type, TOutType, InputFormatType>(In[SwizzleZ]);
	OutByteCount = 3 * sizeof(TOutType);
}

// 2 components conversion with swizzling
template <EDefaultInputType InputFormatType, typename TInType, typename TOutType, uint32 SwizzleX, uint32 SwizzleY>
void FormatData(const TInType& In, uint8* Out, uint32& OutByteCount)
{
	TOutType* OutTyped = (TOutType*)Out;
	OutTyped[0] = ConvertInputFormat<typename TFormatConversionTraits<TInType>::Type, TOutType, InputFormatType>(In[SwizzleX]);
	OutTyped[1] = ConvertInputFormat<typename TFormatConversionTraits<TInType>::Type, TOutType, InputFormatType>(In[SwizzleY]);
	OutByteCount = 2 * sizeof(TOutType);
}

// 1 component conversion
template <EDefaultInputType InputFormatType, typename TInType, typename TOutType>
void FormatData(const TInType& In, uint8* Out, uint32& OutByteCount)
{
	TOutType* OutTyped = (TOutType*)Out;
	OutTyped[0] = ConvertInputFormat<typename TFormatConversionTraits<TInType>::Type, TOutType, InputFormatType>(In[0]);
	OutByteCount = 4;
}

template <typename TInType>
void FormatData111110(const TInType& In, uint8* Out, uint32& OutByteCount)
{
	uint32* OutTyped = (uint32*)Out;
	*OutTyped = 
		 (2048u & ConvertInputFormat<typename TFormatConversionTraits<TInType>::Type, uint32, EDefaultInputType::UNorm11>(In[0]))     |
		((2048u & ConvertInputFormat<typename TFormatConversionTraits<TInType>::Type, uint32, EDefaultInputType::UNorm11>(In[1]))<<11)|
		((1024u & ConvertInputFormat<typename TFormatConversionTraits<TInType>::Type, uint32, EDefaultInputType::UNorm10>(In[2]))<<22);
	OutByteCount = 4;
}

template <typename TInType>
void FormatData1010102(const TInType& In, uint8* Out, uint32& OutByteCount)
{
	uint32* OutTyped = (uint32*)Out;
	*OutTyped =
		 (1024u & ConvertInputFormat<typename TFormatConversionTraits<TInType>::Type, uint32, EDefaultInputType::UNorm10>(In[0]))        |
		((1024u & ConvertInputFormat<typename TFormatConversionTraits<TInType>::Type, uint32, EDefaultInputType::UNorm10>(In[1])) << 10) |
		((1024u & ConvertInputFormat<typename TFormatConversionTraits<TInType>::Type, uint32, EDefaultInputType::UNorm10>(In[2])) << 20) |
		((   3u & ConvertInputFormat<typename TFormatConversionTraits<TInType>::Type, uint32, EDefaultInputType::UNorm2> (In[3])) << 30);
	OutByteCount = 4;
}

template<typename TInType>
void InitializeData(const TInType& InData, EPixelFormat InFormat, uint8* OutData, uint32& OutByteCount)
{
	// If a new format is added insure that it is either supported here, or at least flagged as not supported
	static_assert(PF_MAX == 72);

	switch (InFormat)
	{
		// 32bits
		case PF_R32G32B32A32_UINT:		{ FormatData<EDefaultInputType::Typed, TInType,	uint32,  0, 1, 2, 3>	(InData, OutData, OutByteCount); } break;
		case PF_A32B32G32R32F:			{ FormatData<EDefaultInputType::Typed, TInType,	float,   3, 2, 1, 0>	(InData, OutData, OutByteCount); } break;
		case PF_R32G32_UINT:			{ FormatData<EDefaultInputType::Typed, TInType,	uint32,  0, 1>			(InData, OutData, OutByteCount); } break;
		case PF_G32R32F:				{ FormatData<EDefaultInputType::Typed, TInType,	float,   1, 0>			(InData, OutData, OutByteCount); } break;
		case PF_R32_UINT:				{ FormatData<EDefaultInputType::Typed, TInType,	uint32>					(InData, OutData, OutByteCount); } break;
		case PF_R32_SINT:				{ FormatData<EDefaultInputType::Typed, TInType,	int32>					(InData, OutData, OutByteCount); } break;
		case PF_R32_FLOAT:				{ FormatData<EDefaultInputType::Typed, TInType,	float>					(InData, OutData, OutByteCount); } break;

		// 16bits
		case PF_R16G16B16A16_UINT:		{ FormatData<EDefaultInputType::Typed, TInType,	uint16,   0, 1, 2, 3>	(InData, OutData, OutByteCount); } break;
		case PF_R16G16B16A16_SINT:		{ FormatData<EDefaultInputType::Typed, TInType,	int16,    0, 1, 2, 3>	(InData, OutData, OutByteCount); } break;
		case PF_R16G16B16A16_UNORM:		{ FormatData<EDefaultInputType::UNorm, TInType,	uint16,   0, 1, 2, 3>	(InData, OutData, OutByteCount); } break;
		case PF_R16G16B16A16_SNORM:		{ FormatData<EDefaultInputType::SNorm, TInType,	int16,    0, 1, 2, 3>	(InData, OutData, OutByteCount); } break;
		case PF_A16B16G16R16:			{ FormatData<EDefaultInputType::UNorm, TInType,	uint16,   3, 2, 1, 0>	(InData, OutData, OutByteCount); } break;
		case PF_FloatRGBA:				{ FormatData<EDefaultInputType::Typed, TInType,	FFloat16, 0, 1, 2, 3>	(InData, OutData, OutByteCount); } break;
		case PF_R16G16_UINT:			{ FormatData<EDefaultInputType::Typed, TInType,	uint16,   0, 1>			(InData, OutData, OutByteCount); } break;
		case PF_G16R16:					{ FormatData<EDefaultInputType::UNorm, TInType,	uint16,   1, 0>			(InData, OutData, OutByteCount); } break;
		case PF_G16R16F:				{ FormatData<EDefaultInputType::Typed, TInType,	FFloat16, 0, 1>			(InData, OutData, OutByteCount); } break;
		case PF_G16R16F_FILTER:			{ FormatData<EDefaultInputType::Typed, TInType,	FFloat16, 0, 1>			(InData, OutData, OutByteCount); } break;
		case PF_R16F_FILTER:			{ FormatData<EDefaultInputType::Typed, TInType,	FFloat16>				(InData, OutData, OutByteCount); } break;
		case PF_R16F:					{ FormatData<EDefaultInputType::Typed, TInType,	FFloat16>				(InData, OutData, OutByteCount); } break;
		case PF_G16:					{ FormatData<EDefaultInputType::UNorm, TInType,	uint16>					(InData, OutData, OutByteCount); } break;
		case PF_R16_UINT:				{ FormatData<EDefaultInputType::Typed, TInType,	uint16>					(InData, OutData, OutByteCount); } break;
		case PF_R16_SINT:				{ FormatData<EDefaultInputType::Typed, TInType,	int16>					(InData, OutData, OutByteCount); } break;

		// 8bits
		case PF_B8G8R8A8:				{ FormatData<EDefaultInputType::UNorm, TInType,	uint8,    2, 1, 0, 3>	(InData, OutData, OutByteCount); } break;
		case PF_R8G8B8A8:				{ FormatData<EDefaultInputType::UNorm, TInType,	uint8,    0, 1, 2, 3>	(InData, OutData, OutByteCount); } break;
		case PF_A8R8G8B8:				{ FormatData<EDefaultInputType::UNorm, TInType,	uint8,    3, 2, 1, 0>	(InData, OutData, OutByteCount); } break;
		case PF_R8G8B8A8_UINT:			{ FormatData<EDefaultInputType::Typed, TInType,	uint8,    0, 1, 2, 3>	(InData, OutData, OutByteCount); } break;
		case PF_R8G8B8A8_SNORM:			{ FormatData<EDefaultInputType::SNorm, TInType,	int8,     0, 1, 2, 3>	(InData, OutData, OutByteCount); } break;
		case PF_R8G8:					{ FormatData<EDefaultInputType::UNorm, TInType,	uint8,    0, 1>			(InData, OutData, OutByteCount); } break;
		case PF_R8_UINT:				{ FormatData<EDefaultInputType::Typed, TInType,	uint8>					(InData, OutData, OutByteCount); } break;
		case PF_R8:						{ FormatData<EDefaultInputType::UNorm, TInType,	uint8>					(InData, OutData, OutByteCount); } break;
		case PF_G8:						{ FormatData<EDefaultInputType::UNorm, TInType,	uint8>					(InData, OutData, OutByteCount); } break;
		case PF_L8:						{ FormatData<EDefaultInputType::UNorm, TInType,	uint8>					(InData, OutData, OutByteCount); } break;
		case PF_A1:						{ FormatData<EDefaultInputType::UNorm, TInType,	uint8>					(InData, OutData, OutByteCount); } break;
		case PF_A8:						{ FormatData<EDefaultInputType::UNorm, TInType,	uint8>					(InData, OutData, OutByteCount); } break;

		// Depth/Stencil. Since these texture will only be used as SRV, we handle them as regular float/float16.
		case PF_D24:					{ FormatData<EDefaultInputType::Typed, TInType,	float>					(InData, OutData, OutByteCount); } break;
		case PF_DepthStencil:			{ FormatData<EDefaultInputType::Typed, TInType,	float>					(InData, OutData, OutByteCount); } break;
		case PF_ShadowDepth:			{ FormatData<EDefaultInputType::Typed, TInType,	FFloat16>				(InData, OutData, OutByteCount); } break;

		// Custom
		case PF_FloatRGB:				{ FormatData111110<TInType>	(InData, OutData, OutByteCount); } break;
		case PF_A2B10G10R10:			{ FormatData1010102<TInType>(InData, OutData, OutByteCount); } break;
		case PF_FloatR11G11B10:			{ FormatData111110<TInType>	(InData, OutData, OutByteCount); } break;
			return;

		// Not supported
		case PF_R5G6B5_UNORM:
		case PF_BC5:
		case PF_V8U8:
		case PF_PVRTC2:
		case PF_PVRTC4:
		case PF_UYVY:
		case PF_DXT1:
		case PF_DXT3:
		case PF_DXT5:
		case PF_BC4:
		case PF_ATC_RGB:
		case PF_ATC_RGBA_E:
		case PF_ATC_RGBA_I:
		case PF_X24_G8:
		case PF_ETC1:
		case PF_ETC2_RGB:
		case PF_ETC2_RGBA:
		case PF_ASTC_4x4:
		case PF_ASTC_6x6:
		case PF_ASTC_8x8:
		case PF_ASTC_10x10:
		case PF_ASTC_12x12:
		case PF_BC6H:
		case PF_BC7:
		case PF_XGXR8:
		case PF_PLATFORM_HDR_0:
		case PF_PLATFORM_HDR_1:
		case PF_PLATFORM_HDR_2:
		case PF_NV12:
		case PF_ETC2_R11_EAC:
		case PF_ETC2_RG11_EAC:
		case PF_Unknown:
		case PF_MAX:
			OutByteCount = 0;
			return;
	}
}

template <typename DataType>
void SetDefaultTextureData2D(FRHITexture2D* Texture, const DataType& InData)
{
	uint8 SrcData[16] = { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, };
	uint32 SrcByteCount = 0;
	const EPixelFormat Format = Texture->GetFormat();
	InitializeData(ToVector(InData), Format, SrcData, SrcByteCount);

	uint32 DestStride;
	uint8* Dest = (uint8*)RHILockTexture2D(Texture, 0, RLM_WriteOnly, DestStride, false);
	FMemory::Memcpy(Dest, SrcData, SrcByteCount);
	RHIUnlockTexture2D(Texture, 0, false);
}

template <typename DataType>
void SetDefaultTextureData2DArray(FRHITexture2DArray* Texture, const DataType& InData)
{
	uint8 SrcData[16] = { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, };
	uint32 SrcByteCount = 0;
	const EPixelFormat Format = Texture->GetFormat();
	InitializeData(ToVector(InData), Format, SrcData, SrcByteCount);

	uint32 DestStride;
	uint8* Dest = (uint8*)RHILockTexture2DArray(Texture, 0, 0, RLM_WriteOnly, DestStride, false);
	FMemory::Memcpy(Dest, SrcData, SrcByteCount);
	RHIUnlockTexture2DArray(Texture, 0, 0, false);
}

template <typename DataType>
void SetDefaultTextureData3D(FRHICommandListImmediate& RHICmdList, FRHITexture3D* Texture, const DataType& InData)
{
	uint8 SrcData[16] = { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, };
	uint32 SrcByteCount = 0;
	const EPixelFormat Format = Texture->GetFormat();
	InitializeData(ToVector(InData), Format, SrcData, SrcByteCount);

	FUpdateTextureRegion3D Region(0, 0, 0, 0, 0, 0, 1, 1, 1);
	RHICmdList.UpdateTexture3D(
		Texture,
		0,
		Region,
		SrcByteCount,
		SrcByteCount,
		SrcData);

	// UpdateTexture3D before and after state is currently undefined
	RHICmdList.Transition(FRHITransitionInfo(Texture, ERHIAccess::Unknown, ERHIAccess::SRVMask));
}

template <typename DataType>
void SetDefaultTextureDataCube(FRHITextureCube* Texture, const DataType& InData)
{
	uint8 SrcData[16] = { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, };
	uint32 SrcByteCount = 0;
	const EPixelFormat Format = Texture->GetFormat();
	InitializeData(ToVector(InData), Format, SrcData, SrcByteCount);

	for (uint32 FaceIt = 0; FaceIt < 6; ++FaceIt)
	{
		uint32 DestStride;
		uint8* Dest = (uint8*)RHILockTextureCubeFace(Texture, FaceIt, 0u, 0u, RLM_WriteOnly, DestStride, false);
		FMemory::Memcpy(Dest, SrcData, SrcByteCount);
		RHIUnlockTextureCubeFace(Texture, FaceIt, 0, 0, false);
	}
}

template<typename TClearValue>
FRDGTextureRef GetInternalDefaultTexture(
	FRDGBuilder& GraphBuilder, 
	TArray<FDefaultTexture>& DefaultTextures,
	FHashTable& HashDefaultTextures,
	ETextureDimension Dimension, 
	EPixelFormat Format, 
	TClearValue Value)
{
	// Check this is a valid format
	check(Format != PF_Unknown && Format != PF_MAX && GPixelFormats[Format].BlockSizeX == 1 && GPixelFormats[Format].BlockSizeY == 1 && GPixelFormats[Format].BlockSizeZ == 1);

	// Convert Depth/Stencil format to float/float16 since these texture will only be used as SRV
	if (Format == PF_D24 || Format == PF_DepthStencil)	{ Format = PF_R32_FLOAT; }
	if (Format == PF_ShadowDepth)						{ Format = PF_R32_FLOAT; }

	const FDefaultTextureKey Key = GetDefaultTextureKey(Format, Value);
	const uint32 Hash = Murmur32({uint32(Key.Dimension), uint32(Key.Format), Key.ValueAsUInt[0], Key.ValueAsUInt[1], Key.ValueAsUInt[2], Key.ValueAsUInt[3]});

	uint32 Index = HashDefaultTextures.First(Hash);
	while (HashDefaultTextures.IsValid(Index) && DefaultTextures[Index].Key != Key)
	{
		Index = HashDefaultTextures.Next(Index);
		check(DefaultTextures[Index].Hash == Hash); //Sanitycheck
	}

	if (HashDefaultTextures.IsValid(Index) && DefaultTextures[Index].Texture != nullptr)
	{
		return GraphBuilder.RegisterExternalTexture(DefaultTextures[Index].Texture);
	}

	FDefaultTexture Entry;
	Entry.Key = Key;
	Entry.Hash = Hash;
	Entry.Texture = nullptr;
	
	{
		if (Dimension == ETextureDimension::Texture2D)
		{
			FRHIResourceCreateInfo CreateInfo(TEXT("DefaultTexture2D"));
			FTexture2DRHIRef Texture = RHICreateTexture2D(1, 1, Format, 1, 1, TexCreate_ShaderResource, CreateInfo);
			SetDefaultTextureData2D(Texture, Value);
			Entry.Texture = CreateRenderTarget(Texture, CreateInfo.DebugName);
		}
		else if (Dimension == ETextureDimension::Texture2DArray)
		{
			FRHIResourceCreateInfo CreateInfo(TEXT("DefaultTexture2DArray"));
			FTexture2DArrayRHIRef Texture = RHICreateTexture2DArray(1, 1, 1, Format, 1, 1, TexCreate_ShaderResource, CreateInfo);
			SetDefaultTextureData2DArray(Texture, Value);
			Entry.Texture = CreateRenderTarget(Texture, CreateInfo.DebugName);
		}
		else if (Dimension == ETextureDimension::Texture3D)
		{
			FRHIResourceCreateInfo CreateInfo(TEXT("DefaultTexture3D"));
			FTexture3DRHIRef Texture = RHICreateTexture3D(1, 1, 1, Format, 1, TexCreate_ShaderResource, CreateInfo);
			SetDefaultTextureData3D(GraphBuilder.RHICmdList, Texture, Value);
			Entry.Texture = CreateRenderTarget(Texture, CreateInfo.DebugName);
		}
		else if (Dimension == ETextureDimension::TextureCube)
		{
			FRHIResourceCreateInfo CreateInfo(TEXT("DefaultTextureCube"));
			FTextureCubeRHIRef Texture = RHICreateTextureCube(1, Format, 1, TexCreate_ShaderResource, CreateInfo);
			SetDefaultTextureDataCube(Texture, Value);
			Entry.Texture = CreateRenderTarget(Texture, CreateInfo.DebugName);
		}
		else if (Dimension == ETextureDimension::TextureCubeArray)
		{
			FRHIResourceCreateInfo CreateInfo(TEXT("DefaultTextureCubeArray"));
			FTextureCubeRHIRef Texture = RHICreateTextureCubeArray(1, 1, Format, 1, TexCreate_ShaderResource, CreateInfo);
			SetDefaultTextureDataCube(Texture, Value);
			Entry.Texture = CreateRenderTarget(Texture, CreateInfo.DebugName);
		}
		else
		{
			return nullptr;
		}
	}

	Index = DefaultTextures.Add(Entry);
	HashDefaultTextures.Add(Hash, Index);
	return GraphBuilder.RegisterExternalTexture(Entry.Texture);
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////
// Default Buffers

template<typename T>
static FDefaultBufferKey GetDefaultBufferKey(uint32 NumBytePerElement, bool bIsStructuredBuffer, const T* In)
{
	FDefaultBufferKey Out;
	if (In)
	{
		const uint32* InAsUInt = (const uint32*)In;
		Out.ValueAsUInt[0] = InAsUInt[0];
		Out.ValueAsUInt[1] = NumBytePerElement > 4  ? InAsUInt[1] : 0u;
		Out.ValueAsUInt[2] = NumBytePerElement > 8  ? InAsUInt[2] : 0u;
		Out.ValueAsUInt[3] = NumBytePerElement > 12 ? InAsUInt[3] : 0u;
	}

	Out.NumBytePerElement	= NumBytePerElement;
	Out.bIsStructuredBuffer = bIsStructuredBuffer;
	return Out;
}

bool operator !=(const FDefaultBufferKey& A, const FDefaultBufferKey& B)
{
	return A.NumBytePerElement != B.NumBytePerElement ||
		A.bIsStructuredBuffer != B.bIsStructuredBuffer ||
		A.ValueAsUInt[0] != B.ValueAsUInt[0] ||
		A.ValueAsUInt[1] != B.ValueAsUInt[1] ||
		A.ValueAsUInt[2] != B.ValueAsUInt[2] ||
		A.ValueAsUInt[3] != B.ValueAsUInt[3];
}

template<typename TClearValue>
FRDGBufferRef GetInternalDefaultBuffer(
	FRDGBuilder& GraphBuilder, 
	TArray<FDefaultBuffer>& DefaultBuffers,
	FHashTable& HashDefaultBuffers,
	uint32 NumBytePerElement,
	bool bIsStructuredBuffer,
	const TClearValue* Value)
{
	// Buffer key
	const uint32 NumElements = 1;
	const FDefaultBufferKey Key = GetDefaultBufferKey(NumBytePerElement, bIsStructuredBuffer, Value);
	const uint32 Hash = Murmur32({uint32(Key.bIsStructuredBuffer ? 0x20000000u : 0x10000000u) | Key.NumBytePerElement, Key.ValueAsUInt[0], Key.ValueAsUInt[1], Key.ValueAsUInt[2], Key.ValueAsUInt[3] });

	// Find exsting buffer ("fast" path)
	uint32 Index = HashDefaultBuffers.First(Hash);
	while (HashDefaultBuffers.IsValid(Index) && DefaultBuffers[Index].Key != Key)
	{
		Index = HashDefaultBuffers.Next(Index);
		check(DefaultBuffers[Index].Hash == Hash); //Sanitycheck
	}

	if (HashDefaultBuffers.IsValid(Index) && DefaultBuffers[Index].Buffer != nullptr)
	{
		return GraphBuilder.RegisterExternalBuffer(DefaultBuffers[Index].Buffer);
	}

	// Adding new buffer if there is no fit (slow path)
	FRDGBufferRef Buffer = nullptr; 
	if (bIsStructuredBuffer)
	{
		Buffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(NumBytePerElement, NumElements), TEXT("DefaultStructuredBuffer"));
	}
	else
	{
		Buffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(NumBytePerElement, NumElements), TEXT("DefaultBuffer"));
	}

	FRDGBufferUploader BufferUploader;

	// Initialize the entire buffer with the provided data
	if (Value)
	{
		BufferUploader.Upload(GraphBuilder, Buffer, Value, NumElements * NumBytePerElement);
	}
	// Initialize buffer to 0
	else
	{
		TArray<uint8> DefaultValue;
		DefaultValue.Init(0u, NumElements * NumBytePerElement);
		BufferUploader.Upload(GraphBuilder, Buffer, DefaultValue.GetData(), DefaultValue.Num());
	}

	BufferUploader.Submit(GraphBuilder);

	FDefaultBuffer Entry;
	Entry.Key = Key;
	Entry.Hash = Hash;
	Entry.Buffer = GraphBuilder.ConvertToExternalBuffer(Buffer);

	Index = DefaultBuffers.Add(Entry);
	HashDefaultBuffers.Add(Hash, Index);
	return Buffer;
}

FVector4 GetClearBindingValue(EPixelFormat Format, FClearValueBinding Value)
{
	if (IsDepthOrStencilFormat(Format))
	{
		return FVector4(Value.Value.DSValue.Depth, Value.Value.DSValue.Depth, Value.Value.DSValue.Depth, Value.Value.DSValue.Depth);
	}
	else
	{
		return FVector4(Value.Value.Color[0], Value.Value.Color[1], Value.Value.Color[2], Value.Value.Color[3]);
	}
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Textures 

FRDGTextureRef FSystemTextures::GetDefaultTexture2D(FRDGBuilder& GraphBuilder, EPixelFormat Format, float Value)												{ return GetInternalDefaultTexture(GraphBuilder, DefaultTextures, HashDefaultTextures, ETextureDimension::Texture2D, Format, Value); }
FRDGTextureRef FSystemTextures::GetDefaultTexture2D(FRDGBuilder& GraphBuilder, EPixelFormat Format, uint32 Value)												{ return GetInternalDefaultTexture(GraphBuilder, DefaultTextures, HashDefaultTextures, ETextureDimension::Texture2D, Format, Value); }
FRDGTextureRef FSystemTextures::GetDefaultTexture2D(FRDGBuilder& GraphBuilder, EPixelFormat Format, const FVector& Value)										{ return GetInternalDefaultTexture(GraphBuilder, DefaultTextures, HashDefaultTextures, ETextureDimension::Texture2D, Format, Value); }
FRDGTextureRef FSystemTextures::GetDefaultTexture2D(FRDGBuilder& GraphBuilder, EPixelFormat Format, const FVector4& Value)										{ return GetInternalDefaultTexture(GraphBuilder, DefaultTextures, HashDefaultTextures, ETextureDimension::Texture2D, Format, Value); }
FRDGTextureRef FSystemTextures::GetDefaultTexture2D(FRDGBuilder& GraphBuilder, EPixelFormat Format, const FUintVector4& Value)									{ return GetInternalDefaultTexture(GraphBuilder, DefaultTextures, HashDefaultTextures, ETextureDimension::Texture2D, Format, Value); }
FRDGTextureRef FSystemTextures::GetDefaultTexture2D(FRDGBuilder& GraphBuilder, EPixelFormat Format, const FClearValueBinding& Value)							{ return GetInternalDefaultTexture(GraphBuilder, DefaultTextures, HashDefaultTextures, ETextureDimension::Texture2D, Format, GetClearBindingValue(Format, Value)); }

FRDGTextureRef FSystemTextures::GetDefaultTexture(FRDGBuilder& GraphBuilder, ETextureDimension Dimension, EPixelFormat Format, float Value)						{ return GetInternalDefaultTexture(GraphBuilder, DefaultTextures, HashDefaultTextures, Dimension, Format, Value); }
FRDGTextureRef FSystemTextures::GetDefaultTexture(FRDGBuilder& GraphBuilder, ETextureDimension Dimension, EPixelFormat Format, uint32 Value)					{ return GetInternalDefaultTexture(GraphBuilder, DefaultTextures, HashDefaultTextures, Dimension, Format, Value); }
FRDGTextureRef FSystemTextures::GetDefaultTexture(FRDGBuilder& GraphBuilder, ETextureDimension Dimension, EPixelFormat Format, const FVector2D& Value)			{ return GetInternalDefaultTexture(GraphBuilder, DefaultTextures, HashDefaultTextures, Dimension, Format, Value); }
FRDGTextureRef FSystemTextures::GetDefaultTexture(FRDGBuilder& GraphBuilder, ETextureDimension Dimension, EPixelFormat Format, const FIntPoint& Value)			{ return GetInternalDefaultTexture(GraphBuilder, DefaultTextures, HashDefaultTextures, Dimension, Format, Value); }
FRDGTextureRef FSystemTextures::GetDefaultTexture(FRDGBuilder& GraphBuilder, ETextureDimension Dimension, EPixelFormat Format, const FVector& Value)			{ return GetInternalDefaultTexture(GraphBuilder, DefaultTextures, HashDefaultTextures, Dimension, Format, Value); }
FRDGTextureRef FSystemTextures::GetDefaultTexture(FRDGBuilder& GraphBuilder, ETextureDimension Dimension, EPixelFormat Format, const FVector4& Value)			{ return GetInternalDefaultTexture(GraphBuilder, DefaultTextures, HashDefaultTextures, Dimension, Format, Value); }
FRDGTextureRef FSystemTextures::GetDefaultTexture(FRDGBuilder& GraphBuilder, ETextureDimension Dimension, EPixelFormat Format, const FUintVector4& Value)		{ return GetInternalDefaultTexture(GraphBuilder, DefaultTextures, HashDefaultTextures, Dimension, Format, Value); }
FRDGTextureRef FSystemTextures::GetDefaultTexture(FRDGBuilder& GraphBuilder, ETextureDimension Dimension, EPixelFormat Format, const FClearValueBinding& Value)	{ return GetInternalDefaultTexture(GraphBuilder, DefaultTextures, HashDefaultTextures, Dimension, Format, GetClearBindingValue(Format, Value)); }

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Buffers

// Default init to 0
FRDGBufferRef FSystemTextures::GetDefaultBuffer(FRDGBuilder& GraphBuilder, uint32 NumBytePerElement)										{ return GetInternalDefaultBuffer(GraphBuilder, DefaultBuffers, HashDefaultBuffers, NumBytePerElement, false, (uint32*)nullptr); }
FRDGBufferRef FSystemTextures::GetDefaultStructuredBuffer(FRDGBuilder& GraphBuilder, uint32 NumBytePerElement)								{ return GetInternalDefaultBuffer(GraphBuilder, DefaultBuffers, HashDefaultBuffers, NumBytePerElement, true,  (uint32*)nullptr); }

// Default value of an element
FRDGBufferRef FSystemTextures::GetDefaultBuffer(FRDGBuilder& GraphBuilder, uint32 NumBytePerElement, float Value)							{ return GetInternalDefaultBuffer(GraphBuilder, DefaultBuffers, HashDefaultBuffers, NumBytePerElement, false/* Vertex */, &Value); }
FRDGBufferRef FSystemTextures::GetDefaultBuffer(FRDGBuilder& GraphBuilder, uint32 NumBytePerElement, uint32 Value)							{ return GetInternalDefaultBuffer(GraphBuilder, DefaultBuffers, HashDefaultBuffers, NumBytePerElement, false/* Vertex */, &Value); }
FRDGBufferRef FSystemTextures::GetDefaultBuffer(FRDGBuilder& GraphBuilder, uint32 NumBytePerElement, const FVector& Value)					{ return GetInternalDefaultBuffer(GraphBuilder, DefaultBuffers, HashDefaultBuffers, NumBytePerElement, false/* Vertex */, &Value); }
FRDGBufferRef FSystemTextures::GetDefaultBuffer(FRDGBuilder& GraphBuilder, uint32 NumBytePerElement, const FVector4& Value)					{ return GetInternalDefaultBuffer(GraphBuilder, DefaultBuffers, HashDefaultBuffers, NumBytePerElement, false/* Vertex */, &Value); }
FRDGBufferRef FSystemTextures::GetDefaultBuffer(FRDGBuilder& GraphBuilder, uint32 NumBytePerElement, const FUintVector4& Value)				{ return GetInternalDefaultBuffer(GraphBuilder, DefaultBuffers, HashDefaultBuffers, NumBytePerElement, false/* Vertex */, &Value); }

FRDGBufferRef FSystemTextures::GetDefaultStructuredBuffer(FRDGBuilder& GraphBuilder, uint32 NumBytePerElement, float Value)					{ return GetInternalDefaultBuffer(GraphBuilder, DefaultBuffers, HashDefaultBuffers, NumBytePerElement, true /* Structured */, &Value); }
FRDGBufferRef FSystemTextures::GetDefaultStructuredBuffer(FRDGBuilder& GraphBuilder, uint32 NumBytePerElement, uint32 Value)				{ return GetInternalDefaultBuffer(GraphBuilder, DefaultBuffers, HashDefaultBuffers, NumBytePerElement, true /* Structured */, &Value); }
FRDGBufferRef FSystemTextures::GetDefaultStructuredBuffer(FRDGBuilder& GraphBuilder, uint32 NumBytePerElement, const FVector& Value)		{ return GetInternalDefaultBuffer(GraphBuilder, DefaultBuffers, HashDefaultBuffers, NumBytePerElement, true /* Structured */, &Value); }
FRDGBufferRef FSystemTextures::GetDefaultStructuredBuffer(FRDGBuilder& GraphBuilder, uint32 NumBytePerElement, const FVector4& Value)		{ return GetInternalDefaultBuffer(GraphBuilder, DefaultBuffers, HashDefaultBuffers, NumBytePerElement, true /* Structured */, &Value); }
FRDGBufferRef FSystemTextures::GetDefaultStructuredBuffer(FRDGBuilder& GraphBuilder, uint32 NumBytePerElement, const FUintVector4& Value)	{ return GetInternalDefaultBuffer(GraphBuilder, DefaultBuffers, HashDefaultBuffers, NumBytePerElement, true /* Structured */, &Value); }
