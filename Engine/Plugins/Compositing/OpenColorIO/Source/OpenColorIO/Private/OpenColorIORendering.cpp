// Copyright Epic Games, Inc. All Rights Reserved.

#include "OpenColorIORendering.h"

#include "ColorSpace.h"
#include "Engine/RendererSettings.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "EngineModule.h"
#include "GlobalShader.h"
#include "Logging/LogMacros.h"
#include "Logging/MessageLog.h"
#include "OpenColorIOColorSpace.h"
#include "OpenColorIOConfiguration.h"
#include "OpenColorIOModule.h"
#include "OpenColorIOShader.h"
#include "OpenColorIOShaderType.h"
#include "OpenColorIOShared.h"
#include "OpenColorIOColorTransform.h"
#include "SceneInterface.h"
#include "ScreenPass.h"
#include "TextureResource.h"
#include "ScenePrivate.h"


namespace {
	using namespace UE::Color;

	// Static local storage to prevent color space recomputation every frame. This is viable since a WCS change requires a project relaunch.
	const FMatrix44f& GetWorkingColorSpaceToInterchangeTransform()
	{
		static FMatrix44f Transform = Transpose<float>(FColorSpaceTransform(FColorSpace::GetWorking(), FColorSpace(EColorSpace::ACESAP0)));
		return Transform;
	}

	const FMatrix44f& GetInterchangeToWorkingColorSpaceTransform()
	{
		static FMatrix44f Transform = Transpose<float>(FColorSpaceTransform(FColorSpace(EColorSpace::ACESAP0), FColorSpace::GetWorking()));
		return Transform;
	}
}

// static
void FOpenColorIORendering::AddPass_RenderThread(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const FScreenPassTexture& Input,
	const FScreenPassRenderTarget& Output,
	const FOpenColorIORenderPassResources& InPassResource,
	float InGamma)
{
	check(IsInRenderingThread());

	const FScreenPassTextureViewport InputViewport(Input);
	const FScreenPassTextureViewport OutputViewport(Output);

	if (InPassResource.ShaderResource != nullptr)
	{
		TShaderRef<FOpenColorIOPixelShader> OCIOPixelShader = InPassResource.ShaderResource->GetShader<FOpenColorIOPixelShader>();

		FOpenColorIOPixelShaderParameters* Parameters = GraphBuilder.AllocParameters<FOpenColorIOPixelShaderParameters>();
		Parameters->InputTexture = Input.Texture;
		Parameters->InputTextureSampler = TStaticSamplerState<>::GetRHI();
		OpenColorIOBindTextureResources(Parameters, InPassResource.TextureResources);

		// Apply a transform between the working color space and the interchange color space, if necessary.
		switch (InPassResource.ShaderResource->GetWorkingColorSpaceTransformType())
		{
		case EOpenColorIOWorkingColorSpaceTransform::Source:
			Parameters->WorkingColorSpaceToInterchange = GetWorkingColorSpaceToInterchangeTransform();
			break;
		case EOpenColorIOWorkingColorSpaceTransform::Destination:
			Parameters->InterchangeToWorkingColorSpace = GetInterchangeToWorkingColorSpaceTransform();
			break;

		default:
			// do nothing, shader parameter is unused.
			break;	
		}
		Parameters->Gamma = InGamma;
		Parameters->RenderTargets[0] = Output.GetRenderTargetBinding();

		AddDrawScreenPass(GraphBuilder, RDG_EVENT_NAME("OpenColorIOPass"), View, OutputViewport, InputViewport, OCIOPixelShader, Parameters);
	}
	else
	{
		// Fallback pass, printing invalid message across the viewport.
		TShaderMapRef<FOpenColorIOInvalidPixelShader> OCIOInvalidPixelShader(View.ShaderMap);
		FOpenColorIOInvalidShaderParameters* Parameters = GraphBuilder.AllocParameters<FOpenColorIOInvalidShaderParameters>();
		Parameters->InputTexture = Input.Texture;
		Parameters->InputTextureSampler = TStaticSamplerState<>::GetRHI();
		Parameters->MiniFontTexture = OpenColorIOGetMiniFontTexture();
		Parameters->RenderTargets[0] = Output.GetRenderTargetBinding();

		AddDrawScreenPass(GraphBuilder, RDG_EVENT_NAME("OpenColorIOInvalidPass"), View, OutputViewport, InputViewport, OCIOInvalidPixelShader, Parameters);
	}
}

// static
bool FOpenColorIORendering::ApplyColorTransform(UWorld* InWorld, const FOpenColorIOColorConversionSettings& InSettings, UTexture* InTexture, UTextureRenderTarget2D* OutRenderTarget)
{
	check(IsInGameThread());

	if (!ensureMsgf(InTexture, TEXT("Can't apply color transform - Invalid Input Texture")))
	{
		return false;
	}

	if (!ensureMsgf(OutRenderTarget, TEXT("Can't apply color transform - Invalid Output Texture")))
	{
		return false;
	}

	FTextureResource* InputResource = InTexture->GetResource();
	FTextureResource* OutputResource = OutRenderTarget->GetResource();
	if (!ensureMsgf(InputResource, TEXT("Can't apply color transform - Invalid Input Texture resource")))
	{
		return false;
	}

	if (!ensureMsgf(OutputResource, TEXT("Can't apply color transform - Invalid Output Texture resource")))
	{
		return false;
	}

	const ERHIFeatureLevel::Type FeatureLevel = InWorld->Scene->GetFeatureLevel();
	FOpenColorIOTransformResource* ShaderResource = nullptr;
	TSortedMap<int32, FTextureResource*> TransformTextureResources;

	if (InSettings.ConfigurationSource != nullptr)
	{
		const bool bFoundTransform = InSettings.ConfigurationSource->GetRenderResources(FeatureLevel, InSettings, ShaderResource, TransformTextureResources);

		if (bFoundTransform)
		{
			check(ShaderResource);
			if (ShaderResource->GetShaderGameThread<FOpenColorIOPixelShader>().IsNull())
			{
				ensureMsgf(false, TEXT("Can't apply display look - Shader was invalid for Resource %s"), *ShaderResource->GetFriendlyName());

				//Invalidate shader resource
				ShaderResource = nullptr;
			}
		}
	}
	
	ENQUEUE_RENDER_COMMAND(ProcessColorSpaceTransform)(
		[InputResource, OutputResource, ShaderResource, TextureResources = MoveTemp(TransformTextureResources)](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);

			FRDGTextureRef InputTexture = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(InputResource->TextureRHI, TEXT("OCIOInputTexture")));
			FRDGTextureRef OutputTexture = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(OutputResource->TextureRHI, TEXT("OCIORenderTargetTexture")));
			FIntPoint  OutputResolution = FIntPoint(OutputResource->GetSizeX(), OutputResource->GetSizeY());
			FScreenPassRenderTarget Output = FScreenPassRenderTarget(OutputTexture, FIntRect(FIntPoint::ZeroValue, OutputResolution), ERenderTargetLoadAction::EClear);


			FSceneViewFamily ViewFamily(FSceneViewFamily::ConstructionValues(nullptr, nullptr, FEngineShowFlags(ESFIM_Game))
				.SetTime(FGameTime())
				.SetGammaCorrection(1.0f));

			FSceneViewInitOptions ViewInitOptions;
			ViewInitOptions.ViewFamily = &ViewFamily;
			ViewInitOptions.SetViewRectangle(Output.ViewRect);
			ViewInitOptions.ViewOrigin = FVector::ZeroVector;
			ViewInitOptions.ViewRotationMatrix = FMatrix::Identity;
			ViewInitOptions.ProjectionMatrix = FMatrix::Identity;

			GetRendererModule().CreateAndInitSingleView(RHICmdList, &ViewFamily, &ViewInitOptions);
			const FViewInfo& View = *(const FViewInfo*)ViewFamily.Views[0];

			AddPass_RenderThread(
				GraphBuilder,
				View,
				FScreenPassTexture(InputTexture),
				Output,
				FOpenColorIORenderPassResources{ ShaderResource, TextureResources},
				1.0f); // Set Gamma to 1., since we do not have any display parameters or requirement for Gamma.

			GraphBuilder.Execute();
		}
	);

	return ShaderResource != nullptr;
}
