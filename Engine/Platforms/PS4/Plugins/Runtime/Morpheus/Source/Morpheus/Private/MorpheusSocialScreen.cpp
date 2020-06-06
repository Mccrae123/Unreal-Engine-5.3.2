// Copyright Epic Games, Inc. All Rights Reserved.

#include "MorpheusHMD.h"
#include "PipelineStateCache.h"
#include "ClearQuad.h"
#include "ScreenRendering.h"
#include "TextureResource.h"
#include "Misc/CoreDelegates.h"
#include "HeadMountedDisplayFunctionLibrary.h"
#include "Engine/Texture.h"
#include "DefaultSpectatorScreenController.h"
#include "CommonRenderResources.h"
#include "RGBAToYUV420Shader.h"

#if PLATFORM_PS4
#include <common_dialog.h>
#include <message_dialog.h>
#include <libsysmodule.h>
#include <social_screen.h>
#include <social_screen_dialog.h>
#endif //PLATFORM_PS4

#if HAS_MORPHEUS_HMD_SDK

bool FMorpheusHMD::SocialScreenStartup()
{
#if PLATFORM_PS4
	if (!bEnableSocialScreenSeparateMode)
	{
		UE_LOG(LogHMD, Log, TEXT("SocialScreenStartup() bEnableSocialScreenSeparateMode is false, so the social screen will be in MirrorMode."));
		DesiredSocialScreenState = ESocialScreenState::MirrorMode;
		return true;
	}

	SpectatorScreenController = MakeUnique<FDefaultSpectatorScreenController>(this);
	
	check(DesiredSocialScreenState == ESocialScreenState::Constructed);
	int32 ModuleHandle = sceSysmoduleLoadModule(SCE_SYSMODULE_SOCIAL_SCREEN_DIALOG);
	if (ModuleHandle < 0)
	{
		UE_LOG(LogHMD, Warning, TEXT("sceSysmoduleLoadModule for SCE_SYSMODULE_SOCIAL_SCREEN_DIALOG failed: 0x%08X  SocialScreen will stay in mirrored mode."), ModuleHandle);
		DesiredSocialScreenState = ESocialScreenState::Failed;
		return false;
	}

	int32_t Ret = sceCommonDialogInitialize();
	if (Ret != SCE_OK && Ret != SCE_COMMON_DIALOG_ERROR_ALREADY_SYSTEM_INITIALIZED) // it is ok if some other system already initialized this
	{
		UE_LOG(LogHMD, Error, TEXT("sceCommonDialogInitialize() failed. Error code 0x%x   SocialScreen will stay in mirrored mode."), Ret);
		DesiredSocialScreenState = ESocialScreenState::Failed;
		return false;
	}

	// The Social Screen module must be loaded before calling sceVideoOutOpen() with SCE_VIDEO_OUT_BUS_TYPE_AUX
	ModuleHandle = sceSysmoduleLoadModule(SCE_SYSMODULE_SOCIAL_SCREEN);
	if (ModuleHandle < 0)
	{
		UE_LOG(LogHMD, Warning, TEXT("sceSysmoduleLoadModule for SCE_SYSMODULE_SOCIAL_SCREEN failed: 0x%08X  SocialScreen will stay in mirrored mode."), ModuleHandle);
		DesiredSocialScreenState = ESocialScreenState::Failed;
		return false;
	}
	Ret = sceSocialScreenInitialize();
	if (Ret != SCE_OK)
	{
		UE_LOG(LogHMD, Error, TEXT("sceSocialScreenInitialize() failed. Error code 0x%x   SocialScreen will stay in mirrored mode."), Ret);
		DesiredSocialScreenState = ESocialScreenState::Failed;
		return false;
	}

	UE_LOG(LogHMD, Log, TEXT("FMorpheusHMD::SocialScreenStartup()calling GnmBridge::CreateSocialScreenBackBuffers()."));
	GnmBridge::CreateSocialScreenBackBuffers();

	UE_LOG(LogHMD, Log, TEXT("SocialScreenStartup() succeeded."));
	DesiredSocialScreenState = ESocialScreenState::MirrorMode;
	return true;
#endif //PLATFORM_PS4
	return false;
}

void FMorpheusHMD::SocialScreenShutdown()
{
#if PLATFORM_PS4
	if (!SpectatorScreenController)
	{
		return;
	}

	check(DesiredSocialScreenState != ESocialScreenState::Shutdown);
	DesiredSocialScreenState = ESocialScreenState::Shutdown;

	sceSocialScreenSetMode(SCE_SOCIAL_SCREEN_MODE_MIRRORING);
	sceSocialScreenCloseSeparateMode();
	sceSocialScreenTerminate();
	sceSocialScreenDialogTerminate();

	UE_LOG(LogHMD, Log, TEXT("SocialScreenShutdown() completed."));
#endif //PLATFORM_PS4
}

void FMorpheusHMD::SocialScreen_BeginRenderViewFamily()
{
#if PLATFORM_PS4
	if (!SpectatorScreenController)
	{
		return;
	}

	SpectatorScreenController->BeginRenderViewFamily();
#endif
}

void FMorpheusHMD::SocialScreen_BeginRendering_RenderThread()
{
#if PLATFORM_PS4
	if (!SpectatorScreenController)
	{
		return;
	}

	UpdateSpectatorScreenMode_RenderThread();

	EPS4SocialScreenOutputMode DesiredOutputMode = EPS4SocialScreenOutputMode::Mirroring;
	if (DesiredSocialScreenState == ESocialScreenState::SeparateMode30FPS)
	{
		DesiredOutputMode = EPS4SocialScreenOutputMode::Separate30FPS;
	}
	//else if (DesiredSocialScreenState == ESocialScreenState::SeparateMode60FPS)
	//{
	//	DesiredOutputMode = EPS4SocialScreenOutputMode::Separate60FPS;
	//}

	const EPS4SocialScreenOutputMode CurrentOutputMode = GnmBridge::GetSocialScreenOutputMode();

	bSocialScreenOverriddenToMirror_RenderThread = FMorpheusHMD::SocialScreenOverrideToMirrorCount.GetValue() > 0;
	if (bSocialScreenOverriddenToMirror_RenderThread)
	{
		if (DesiredOutputMode != EPS4SocialScreenOutputMode::Mirroring && CurrentOutputMode != EPS4SocialScreenOutputMode::Mirroring)
		{
			UE_LOG(LogHMD, Log, TEXT("SocialScreen_PreRenderViewFamily_RenderThread() switching social screen output mode to Mirroring because Override is active."));
		}

		DesiredOutputMode = EPS4SocialScreenOutputMode::Mirroring;
	}

	if (CurrentOutputMode != DesiredOutputMode)
	{
		UE_LOG(LogHMD, Log, TEXT("SocialScreen_PreRenderViewFamily_RenderThread() switching social screen output mode to %i."), static_cast<uint32>(DesiredOutputMode));
		GnmBridge::ChangeSocialScreenOutputMode(DesiredOutputMode);
	}
#endif //PLATFORM_PS4
}

void FMorpheusHMD::UpdateSpectatorScreenMode_RenderThread()
{
#if PLATFORM_PS4
	check(IsInRenderingThread());
	check(SpectatorScreenController);

	const ESpectatorScreenMode OldMode = SpectatorScreenController->GetSpectatorScreenMode();
	
	SpectatorScreenController->UpdateSpectatorScreenMode_RenderThread();

	const ESpectatorScreenMode NewMode = SpectatorScreenController->GetSpectatorScreenMode();

	if (NewMode != OldMode)
	{
		switch (NewMode)
		{
		case ESpectatorScreenMode::SingleEyeLetterboxed:
		case ESpectatorScreenMode::Undistorted:
		case ESpectatorScreenMode::SingleEye:
		case ESpectatorScreenMode::Texture:
		case ESpectatorScreenMode::TexturePlusEye:
			DesiredSocialScreenState = ESocialScreenState::SeparateMode30FPS;
			break;
		default:
			// Note on PSVR SingleEyeCroppedToFill maps to the api and hardware supported vr view mirroring.
			// We also failsafe to that for all unsupported modes.
			if (NewMode != ESpectatorScreenMode::SingleEyeCroppedToFill)
			{
				UE_LOG(LogHMD, Warning, TEXT("UpdateSpectatorScreenMode_RenderThread tried to set mode %i, but that mode is not supported on PSVR.  It will behave like SingleEyeCroppedToFill."), static_cast<uint32>(NewMode))
			}
			DesiredSocialScreenState = ESocialScreenState::MirrorMode;
		}
	}
#endif //PLATFORM_PS4
}

void FMorpheusHMD::RenderSocialScreen_RenderThread(FRHICommandListImmediate& RHICmdList, FRHITexture2D* BackBuffer, FTexture2DRHIRef RenderTarget, FVector2D WindowSize) const
{
#if PLATFORM_PS4
	check(IsInRenderingThread());

	if (!SpectatorScreenController)
	{
		return;
	}

	const bool bShouldRenderSocialScreenThisFrame = GnmBridge::ShouldRenderSocialScreenThisFrame();
	if (bShouldRenderSocialScreenThisFrame)
	{
		const FGnmAuxBuffer& AuxBuffer = GnmBridge::GetSocialScreenAuxBuffer();
		check(SpectatorScreenController);
		SpectatorScreenController->RenderSpectatorScreen_RenderThread(RHICmdList, AuxBuffer.RenderBuffer, RenderTarget, WindowSize);

		SCOPED_NAMED_EVENT_TEXT("FGnmManager::TranslateRGBToYUV()", FColor::Magenta);

		//UE_LOG(LogSony, Log, TEXT("TranslateRGBToYUV() getting target with index %i for frame %i"), CurrentAuxBufferIndex, FrameCounter);
		RHICmdList.TransitionResource(EResourceTransitionAccess::ERWBarrier, EResourceTransitionPipeline::EGfxToCompute, AuxBuffer.UAV);

		TShaderMapRef< FRGBAToYUV420CS > ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		RHICmdList.SetComputeShader(ComputeShader.GetComputeShader());

		//TODO gamma handling, analogous to this (from sony sample)
		//sce::Gnm::Texture tex;
		//tex.initFromRenderTarget(&m_stereoRenderTarget[Hmd::kStereoEyeRight].GetRenderTarget(), false);
		//sce::Gnm::DataFormat dFormat = tex.getDataFormat();
		//// We have to use kTextureTypeUNorm for source texture to invalidate degamma. 
		//if (dFormat.getTextureChannelType() != sce::Gnm::kTextureChannelTypeUNorm)
		//{
		//	tex.setDataFormat(sce::Gnm::DataFormat::build(dFormat.getSurfaceFormat(), sce::Gnm::kTextureChannelTypeUNorm, dFormat.getChannel(0), dFormat.getChannel(1), dFormat.getChannel(2), dFormat.getChannel(3)));
		//}
		//auxSrtData->srcTex = tex;

		const float TargetHeight = AuxBuffer.VideoOutBuffer->GetSizeY() * 2 / 3; // undo the YUV size expansion
		const float TargetWidth = AuxBuffer.VideoOutBuffer->GetSizeX();
		const float TextureScale = AuxBuffer.RenderBuffer->GetSizeX() / (float)AuxBuffer.VideoOutBuffer->GetSizeX();
		const float ScaleFactorX = 1.f * TextureScale / AuxBuffer.RenderBuffer->GetSizeX(); // normalized 1 pixel size for x axis
		const float ScaleFactorY = 1.f * TextureScale / AuxBuffer.RenderBuffer->GetSizeY(); // normalized 1 pixel size for y axis
		// offset to adjust to src texture center area
		const float TextureYOffset = (float)(AuxBuffer.RenderBuffer->GetSizeY() - TargetHeight * TextureScale) / 2.f;
		ComputeShader->SetParameters(RHICmdList, AuxBuffer.RenderBuffer, AuxBuffer.UAV, TargetHeight, ScaleFactorX, ScaleFactorY, TextureYOffset);

		const uint32 ThreadGroupCountX = ((uint32)TargetWidth / 2) / 32;	// 32 matches value in .usf
		const uint32 ThreadGroupCountY = ((uint32)TargetHeight / 2) / 2;	// 2 matches value in .usf
		const uint32 ThreadGroupCountZ = 1;									// 1 matches value in .usf
		DispatchComputeShader(RHICmdList, ComputeShader, ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);

		ComputeShader->UnbindBuffers(RHICmdList);

		RHICmdList.TransitionResource(EResourceTransitionAccess::ERWBarrier, EResourceTransitionPipeline::EComputeToGfx, AuxBuffer.UAV);
	}
#endif //PLATFORM_PS4
}

bool FMorpheusHMD::IsSpectatorScreenActive() const
{
	if (!SpectatorScreenController)
	{
		return false;
	}

	if (IsSocialScreenOverriddenToMirror())
	{
		return false;
	}

	const ESpectatorScreenMode CurrentMode = SpectatorScreenController->GetSpectatorScreenMode();
	return CurrentMode != ESpectatorScreenMode::Disabled && CurrentMode != ESpectatorScreenMode::SingleEyeCroppedToFill && CurrentMode != ESpectatorScreenMode::Distorted;
}

FIntRect FMorpheusHMD::GetFullFlatEyeRect_RenderThread(FTexture2DRHIRef EyeTexture) const
{
	// The sub-rect of the left eye texture that looked pretty flat.
	static FVector2D SrcNormRectMin(0.05f, 0.25f);
	static FVector2D SrcNormRectMax(0.45f, 0.75f);
	return FIntRect(EyeTexture->GetSizeX() * SrcNormRectMin.X, EyeTexture->GetSizeY() * SrcNormRectMin.Y, EyeTexture->GetSizeX() * SrcNormRectMax.X, EyeTexture->GetSizeY() * SrcNormRectMax.Y);
}


void FMorpheusHMD::CopyTexture_RenderThread(FRHICommandListImmediate& RHICmdList, FRHITexture2D* SrcTexture, FIntRect SrcRect, FRHITexture2D* DstTexture, FIntRect DstRect, bool bClearBlack, bool bNoAlpha) const
{
	check(IsInRenderingThread());

	const uint32 ViewportWidth = DstRect.Width();
	const uint32 ViewportHeight = DstRect.Height();
	const FIntPoint TargetSize(ViewportWidth, ViewportHeight);

	const float SrcTextureWidth = SrcTexture->GetSizeX();
	const float SrcTextureHeight = SrcTexture->GetSizeY();
	float U = 0.f, V = 0.f, USize = 1.f, VSize = 1.f;
	if (!SrcRect.IsEmpty())
	{
		U = SrcRect.Min.X / SrcTextureWidth;
		V = SrcRect.Min.Y / SrcTextureHeight;
		USize = SrcRect.Width() / SrcTextureWidth;
		VSize = SrcRect.Height() / SrcTextureHeight;
	}

	FRHITexture* SrcTextureRHI = SrcTexture;
	RHICmdList.TransitionResources(EResourceTransitionAccess::EReadable, &SrcTextureRHI, 1);

	FRHIRenderPassInfo RPInfo(DstTexture, ERenderTargetActions::Load_Store);
	RHICmdList.BeginRenderPass(RPInfo, TEXT("CopyTexture"));
	{
		if (bClearBlack)
		{
			const FIntRect ClearRect(0, 0, DstTexture->GetSizeX(), DstTexture->GetSizeY());
			RHICmdList.SetViewport(ClearRect.Min.X, ClearRect.Min.Y, 0, ClearRect.Max.X, ClearRect.Max.Y, 1.0f);
			DrawClearQuad(RHICmdList, FLinearColor::Black);
		}

		RHICmdList.SetViewport(DstRect.Min.X, DstRect.Min.Y, 0, DstRect.Max.X, DstRect.Max.Y, 1.0f);

		FGraphicsPipelineStateInitializer GraphicsPSOInit;
		RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);

		// for mirror window
		GraphicsPSOInit.BlendState = bNoAlpha ? TStaticBlendState<>::GetRHI() : TStaticBlendState<CW_RGBA, BO_Add, BF_SourceAlpha, BF_InverseSourceAlpha, BO_Add, BF_One, BF_InverseSourceAlpha>::GetRHI();

		GraphicsPSOInit.RasterizerState = TStaticRasterizerState<>::GetRHI();
		GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();

		const auto FeatureLevel = GMaxRHIFeatureLevel;
		auto ShaderMap = GetGlobalShaderMap(FeatureLevel);

		TShaderMapRef<FScreenVS> VertexShader(ShaderMap);
		TShaderMapRef<FScreenPS> PixelShader(ShaderMap);

		GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GFilterVertexDeclaration.VertexDeclarationRHI;
		GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
		GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
		GraphicsPSOInit.PrimitiveType = PT_TriangleList;

		SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit);

		const bool bSameSize = DstRect.Size() == SrcRect.Size();
		if (bSameSize)
		{
			PixelShader->SetParameters(RHICmdList, TStaticSamplerState<SF_Point>::GetRHI(), SrcTextureRHI);
		}
		else
		{
			PixelShader->SetParameters(RHICmdList, TStaticSamplerState<SF_Bilinear>::GetRHI(), SrcTextureRHI);
		}

		RendererModule->DrawRectangle(
			RHICmdList,
			0, 0,
			ViewportWidth, ViewportHeight,
			U, V,
			USize, VSize,
			TargetSize,
			FIntPoint(1, 1),
			VertexShader,
			EDRF_Default);
	}
	RHICmdList.EndRenderPass();
}

// Because we can show the hmd setup dialog during the FMorpheusHMD constructor
// we can't use SharedPointers to tie all this state together.  Instead using a static refcounter.
FThreadSafeCounter FMorpheusHMD::SocialScreenOverrideToMirrorCount;

FMorpheusHMD::FSocialScreenOverrideReceipt::FSocialScreenOverrideReceipt()
{
	int32 NewValue = FMorpheusHMD::SocialScreenOverrideToMirrorCount.Increment();
	check(NewValue < 10); // Suggests a receipt leak.  If not just raise the value.
}
FMorpheusHMD::FSocialScreenOverrideReceipt::~FSocialScreenOverrideReceipt()
{
	int32 NewValue = FMorpheusHMD::SocialScreenOverrideToMirrorCount.Decrement();
	check(NewValue >= 0);
}	

TSharedPtr<FMorpheusHMD::FSocialScreenOverrideReceipt, ESPMode::ThreadSafe> FMorpheusHMD::AcquireSocialScreenOverrideReceipt()
{
	return MakeShared<FMorpheusHMD::FSocialScreenOverrideReceipt, ESPMode::ThreadSafe>();
}

bool FMorpheusHMD::IsSocialScreenOverriddenToMirror() const
{
	if (IsInRenderingThread())
	{
		return bSocialScreenOverriddenToMirror_RenderThread;
	}
	else
	{
		return SocialScreenOverrideToMirrorCount.GetValue() > 0;
	}
}

#endif