// Copyright Epic Games, Inc. All Rights Reserved.

#include "ContrastAdaptiveImageGenerator.h"
#include "StereoRenderTargetManager.h"
#include "GlobalShader.h"
#include "ShaderParameterUtils.h"
#include "RenderGraphUtils.h"
#include "RHI.h"
#include "RHIDefinitions.h"
#include "RenderTargetPool.h"
#include "SystemTextures.h"
#include "SceneView.h"
#include "IEyeTracker.h"
#include "IHeadMountedDisplay.h"
#include "IXRTrackingSystem.h"
#include "Engine/Engine.h"
#include "UnrealClient.h"
#include "PostProcess/PostProcessTonemap.h"
#include "DataDrivenShaderPlatformInfo.h"

/**
 * Contrast Adaptive Shading (CAS) is a Tier 2 Variable Rate Shading method which generates a VRS image by examining the contrast from the previous frame.
 * An image is generated which designates lower shading rates for areas of lower contrast in which reductions are unlikely to be noticed.
 * This image is then reprojected and rescaled in accordance with camera movement and dynamic resolution changes before being provided to the manager.
 */


/**
 * CAS Parameters
 */

TAutoConsoleVariable<int32> CVarVRSContrastAdaptiveShading(
	TEXT("r.VRS.ContrastAdaptiveShading"),
	0,
	TEXT("Enables using Variable Rate Shading based on the luminance from the previous frame's post process output \n"),
	ECVF_RenderThreadSafe);
TAutoConsoleVariable<float> CVarVRSEdgeThreshold(
	TEXT("r.VRS.ContrastAdaptiveShading.EdgeThreshold"),
	0.2,
	TEXT(""),
	ECVF_RenderThreadSafe);
TAutoConsoleVariable<float> CVarVRSConservativeEdgeThreshold(
	TEXT("r.VRS.ContrastAdaptiveShading.ConservativeEdgeThreshold"),
	0.1,
	TEXT(""),
	ECVF_RenderThreadSafe);
TAutoConsoleVariable<float> CVarVRS_HDR10CorrectionMultiplier(
	TEXT("r.VRS.ContrastAdaptiveShading.HDR10CorrectionMultiplier"),
	0.55,
	TEXT("Approximation multiplier to account for how perceptual values are spread out in SDR vs HDR10\n"),
	ECVF_RenderThreadSafe);

/**
 * Pass Settings
 */

TAutoConsoleVariable<int32> CVarVRSBasePass(
	TEXT("r.VRS.BasePass"),
	2,
	TEXT("Enables Variable Rate Shading for the base pass\n")
	TEXT("0: Disabled")
	TEXT("1: Full")
	TEXT("2: Conservative (default)"),
	ECVF_RenderThreadSafe);
TAutoConsoleVariable<int32> CVarVRSTranslucency(
	TEXT("r.VRS.Translucency"),
	1,
	TEXT("Enable VRS with translucency rendering.\n")
	TEXT("0: Disabled")
	TEXT("1: Full (default)")
	TEXT("2: Conservative"),
	ECVF_RenderThreadSafe);
TAutoConsoleVariable<int32> CVarVRSNaniteEmitGBuffer(
	TEXT("r.VRS.NaniteEmitGBuffer"),
	2,
	TEXT("Enable VRS with Nanite EmitGBuffer rendering.\n")
	TEXT("0: Disabled")
	TEXT("1: Full")
	TEXT("2: Conservative (default)"),
	ECVF_RenderThreadSafe);
TAutoConsoleVariable<int32> CVarVRS_SSAO(
	TEXT("r.VRS.SSAO"),
	2,
	TEXT("Enable VRS with SSAO rendering.\n")
	TEXT("0: Disabled")
	TEXT("1: Full")
	TEXT("2: Conservative (default)"),
	ECVF_RenderThreadSafe);
TAutoConsoleVariable<int32> CVarVRS_SSR(
	TEXT("r.VRS.SSR"),
	2,
	TEXT("Enable VRS with SSR (PS) rendering.\n")
	TEXT("0: Disabled")
	TEXT("1: Full")
	TEXT("2: Conservative (default)"),
	ECVF_RenderThreadSafe);
TAutoConsoleVariable<int32> CVarVRSReflectionEnvironmentSky(
	TEXT("r.VRS.ReflectionEnvironmentSky"),
	2,
	TEXT("Enable VRS with ReflectionEnvironmentAndSky (PS) rendering.\n")
	TEXT("0: Disabled")
	TEXT("1: Full")
	TEXT("2: Conservative (default)"),
	ECVF_RenderThreadSafe);
TAutoConsoleVariable<int32> CVarVRSLightFunctions(
	TEXT("r.VRS.LightFunctions"),
	1,
	TEXT("Enables Variable Rate Shading for light functions\n")
	TEXT("0: Disabled")
	TEXT("1: Full (default)")
	TEXT("2: Conservative"),
	ECVF_RenderThreadSafe);
TAutoConsoleVariable<int32> CVarVRSDecals(
	TEXT("r.VRS.Decals"),
	2,
	TEXT("Enables Variable Rate Shading for decals\n")
	TEXT("0: Disabled")
	TEXT("1: Full")
	TEXT("2: Conservative (default)"),
	ECVF_RenderThreadSafe);

/**
 * Debug Settings 
 */

int GVRSDebugForceRate = -1;
FAutoConsoleVariableRef CVarVRSDebugForceRate(
	TEXT("r.VRS.ContrastAdaptiveShading.Debug.ForceRate"),
	GVRSDebugForceRate,
	TEXT("-1 : None, 0 : Force 1x1, 1 : Force 1x2, 4 : Force 2x1, 5: Force 2x2"));

TAutoConsoleVariable<int32> CVarVRSPreview(
	TEXT("r.VRS.Preview"),
	0,
	TEXT("Show a debug visualiation of the SRI texture.")
	TEXT("0 - off, 1 - the SRI texture, 2- the conservative SRI texture, 3 - the unscaled SRI texture"),
	ECVF_RenderThreadSafe);

/**
 * Shaders
 */

 //---------------------------------------------------------------------------------------------
class FCalculateShadingRateImageCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCalculateShadingRateImageCS);
	SHADER_USE_PARAMETER_STRUCT(FCalculateShadingRateImageCS, FGlobalShader);

	class FThreadGroupX : SHADER_PERMUTATION_SPARSE_INT("THREADGROUP_SIZEX", 8, 16);
	class FThreadGroupY : SHADER_PERMUTATION_SPARSE_INT("THREADGROUP_SIZEY", 8, 16);
	class FForceRate : SHADER_PERMUTATION_SPARSE_INT("FORCE_RATE", -1, 0, 1, 4, 5);

	using FPermutationDomain = TShaderPermutationDomain<FThreadGroupX, FThreadGroupY, FForceRate>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, LuminanceTexture)
		SHADER_PARAMETER(FVector4f, ViewRect)
		SHADER_PARAMETER(float, EdgeThreshold)
		SHADER_PARAMETER(float, ConservativeEdgeThreshold)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, VariableRateShadingTexture)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5)
			&& FDataDrivenShaderPlatformInfo::GetSupportsVariableRateShading(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("COMPUTE_SHADER"), 1);
	}

	static void InitParameters(
		FParameters& Parameters,
		FRDGTextureRef Luminance,
		const FIntRect& ViewRect,
		bool bIsHDR10,
		FRDGTextureUAV* ShadingRateImage)
	{
		Parameters.LuminanceTexture = Luminance;
		Parameters.ViewRect = FVector4f(ViewRect.Min.X, ViewRect.Min.Y, ViewRect.Max.X, ViewRect.Max.Y);
		const float cEdgeThresholdCorrectionValue = bIsHDR10 ? CVarVRS_HDR10CorrectionMultiplier.GetValueOnRenderThread() : 1.0;
		Parameters.EdgeThreshold = cEdgeThresholdCorrectionValue * CVarVRSEdgeThreshold.GetValueOnRenderThread();
		Parameters.ConservativeEdgeThreshold = cEdgeThresholdCorrectionValue * CVarVRSConservativeEdgeThreshold.GetValueOnRenderThread();
		Parameters.VariableRateShadingTexture = ShadingRateImage;
	}
};
IMPLEMENT_GLOBAL_SHADER(FCalculateShadingRateImageCS, "/Engine/Private/VariableRateShading/VRSShadingRateCalculate.usf", "CalculateShadingRateImage", SF_Compute);

//---------------------------------------------------------------------------------------------
class FRescaleVariableRateShadingCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRescaleVariableRateShadingCS);
	SHADER_USE_PARAMETER_STRUCT(FRescaleVariableRateShadingCS, FGlobalShader);

	static constexpr uint32 ThreadGroupSize = 8;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTextures)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, InputSRITexture)
		SHADER_PARAMETER(FVector2f, SRIViewRectMin)
		SHADER_PARAMETER(FVector2f, SRIViewRectMax)
		SHADER_PARAMETER(FVector2f, TextureDimensions)
		SHADER_PARAMETER(FVector2f, InvTextureDimensions)
		SHADER_PARAMETER(FVector2f, ScaledSRIDimensions)
		SHADER_PARAMETER(FVector2f, ScaledUVOffset)
		SHADER_PARAMETER(float, InvDynamicResolutionScale)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, ScaledSRITexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, ScaledConservativeSRITexture)
	END_SHADER_PARAMETER_STRUCT()

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), ThreadGroupSize);
	}

	static void InitParameters(
		FParameters& Parameters,
		const FMinimalSceneTextures& SceneTextures,
		const FViewInfo& ViewInfo,
		FRDGTextureRef InputSRITexture,
		FVector2f ViewRectMin,
		FVector2f ViewRectMax,
		FVector2f ScaledSRIDimensions,
		FVector2f TextureDimensions,
		FVector2f ScaledUVOffset,
		float DynamicResolutionScale,
		FRDGTextureUAVRef ScaledSRIUAV,
		FRDGTextureUAVRef ScaledConservateSRIUAV)
	{
		Parameters.SceneTextures = SceneTextures.UniformBuffer;
		Parameters.View = ViewInfo.ViewUniformBuffer;
		Parameters.InputSRITexture = InputSRITexture;
		Parameters.SRIViewRectMin = ViewRectMin;
		Parameters.SRIViewRectMax = ViewRectMax;
		Parameters.TextureDimensions = TextureDimensions;
		Parameters.InvTextureDimensions = FVector2f(1.0f / TextureDimensions.X, 1.0f / TextureDimensions.Y);
		Parameters.ScaledSRIDimensions = ScaledSRIDimensions;
		Parameters.ScaledUVOffset = ScaledUVOffset;
		Parameters.InvDynamicResolutionScale = 1.0f / DynamicResolutionScale;
		Parameters.ScaledSRITexture = ScaledSRIUAV;
		Parameters.ScaledConservativeSRITexture = ScaledConservateSRIUAV;
	}
};
IMPLEMENT_GLOBAL_SHADER(FRescaleVariableRateShadingCS, "/Engine/Private/VariableRateShading/VRSShadingRateReproject.usf", "RescaleVariableRateShading", SF_Compute);

class FDebugVariableRateShadingCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FDebugVariableRateShadingCS);
	SHADER_USE_PARAMETER_STRUCT(FDebugVariableRateShadingCS, FGlobalShader);

	static constexpr uint32 ThreadGroupSize = 8;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, VariableRateShadingTextureIn)
		SHADER_PARAMETER(FVector4f, ViewRect)
		SHADER_PARAMETER(float, DynamicResolutionScale)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, SceneColorOut)
	END_SHADER_PARAMETER_STRUCT()

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZEX"), ThreadGroupSize);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZEY"), ThreadGroupSize);
		OutEnvironment.SetDefine(TEXT("COMPUTE_SHADER"), 1);
	}

	static void InitParameters(
		FParameters& Parameters,
		FRDGTextureRef VariableRateShadingTexture,
		const FIntRect& ViewRect,
		float DynamicResolutionScale,
		FRDGTextureUAVRef SceneColorUAV)
	{
		Parameters.VariableRateShadingTextureIn = VariableRateShadingTexture;
		Parameters.ViewRect = FVector4f(ViewRect.Min.X, ViewRect.Min.Y, ViewRect.Max.X, ViewRect.Max.Y);
		Parameters.DynamicResolutionScale = DynamicResolutionScale;
		Parameters.SceneColorOut = SceneColorUAV;
	}
};

IMPLEMENT_GLOBAL_SHADER(FDebugVariableRateShadingCS, "/Engine/Private/VariableRateShading/VRSShadingRateCalculate.usf", "PreviewVariableRateShadingTextureCS", SF_Compute);

//---------------------------------------------------------------------------------------------
using FDebugVariableRateShadingVS = FScreenPassVS;

//---------------------------------------------------------------------------------------------
class FDebugVariableRateShadingPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FDebugVariableRateShadingPS);
	SHADER_USE_PARAMETER_STRUCT(FDebugVariableRateShadingPS, FGlobalShader);

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("COMPUTE_SHADER"), 0);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, VariableRateShadingTextureIn)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()
		
	static void InitParameters(
			FParameters& Parameters,
			FRDGTextureRef VariableRateShadingTexture,
			FRDGTextureRef OutputSceneColor)
	{
		Parameters.VariableRateShadingTextureIn = VariableRateShadingTexture;
		Parameters.RenderTargets[0] = FRenderTargetBinding(OutputSceneColor, ERenderTargetLoadAction::ELoad);
	}
};

IMPLEMENT_GLOBAL_SHADER(FDebugVariableRateShadingPS, "/Engine/Private/VariableRateShading/VRSShadingRateCalculate.usf", "PreviewVariableRateShadingTexturePS", SF_Pixel);

/**
 * Helper Functions and Structures
 */

namespace ESRITextureType
{
	enum Type
	{
		None = 0,
		ScaledSRIForRender,
		ScaledConservativeSRIForRender,
		ConstructedSRI,
		Num
	};
	bool IsInBounds(int32 TypeAsInt)
	{
		return TypeAsInt >= 0 && TypeAsInt < ESRITextureType::Num;
	}
	bool IsInBounds(ESRITextureType::Type TextureType)
	{
		return IsInBounds(static_cast<int32>(TextureType));
	}
	bool IsValidShadingRateTexture(int32 TextureType)
	{
		return IsInBounds(TextureType) && TextureType != ESRITextureType::None && TextureType != ESRITextureType::ConstructedSRI;
	}
	bool IsValidShadingRateTexture(ESRITextureType::Type TextureType)
	{
		return IsValidShadingRateTexture(static_cast<int32>(TextureType));
	}
	ESRITextureType::Type GetTextureType(FVariableRateShadingImageManager::EVRSPassType PassType)
	{
		static struct FStaticData
		{
			TAutoConsoleVariable<int32>*CVarByPassType[FVariableRateShadingImageManager::EVRSPassType::Num] = {};
			FStaticData()
			{
				CVarByPassType[FVariableRateShadingImageManager::EVRSPassType::BasePass] = &CVarVRSBasePass;
				CVarByPassType[FVariableRateShadingImageManager::EVRSPassType::TranslucencyAll] = &CVarVRSTranslucency;
				CVarByPassType[FVariableRateShadingImageManager::EVRSPassType::NaniteEmitGBufferPass] = &CVarVRSNaniteEmitGBuffer;
				CVarByPassType[FVariableRateShadingImageManager::EVRSPassType::SSAO] = &CVarVRS_SSAO;
				CVarByPassType[FVariableRateShadingImageManager::EVRSPassType::SSR] = &CVarVRS_SSR;
				CVarByPassType[FVariableRateShadingImageManager::EVRSPassType::ReflectionEnvironmentAndSky] = &CVarVRSReflectionEnvironmentSky;
				CVarByPassType[FVariableRateShadingImageManager::EVRSPassType::LightFunctions] = &CVarVRSLightFunctions;
				CVarByPassType[FVariableRateShadingImageManager::EVRSPassType::Decals] = &CVarVRSDecals;
			}
		} StaticData;
		const int32 PassTypeAsInt = static_cast<int32>(PassType);
		if (PassTypeAsInt >= 0 && PassTypeAsInt < EMeshPass::Num)
		{
			auto* CVar = StaticData.CVarByPassType[PassTypeAsInt];
			if (CVar)
			{
				int32 TextureTypeAsInt = CVar->GetValueOnRenderThread();
				if (IsValidShadingRateTexture(TextureTypeAsInt))
				{
					return static_cast<ESRITextureType::Type>(TextureTypeAsInt);
				}
			}
		}
		return ESRITextureType::None;
	}
};

namespace ESRIPreviewType
{
	enum Type
	{
		Off,
		Projected,
		ProjectedConservative,
		BeforeReprojection,
		Num
	};
	static const TCHAR* Names[] = {
		TEXT("Off"),
		TEXT("Projected"),
		TEXT("ProjectedConservative"),
		TEXT("BeforeReprojection"),
	};
	static const TCHAR* GetName(Type PreviewType)
	{
		if (PreviewType >= Off && PreviewType < Num)
		{
			return Names[static_cast<int32>(PreviewType)];
		}

		return TEXT("Invalid Type");
	}
};

static const TCHAR* ShadingRateTextureName = TEXT("ShadingRateTexture");
static const TCHAR* ScaledShadingRateTextureName = TEXT("ScaledShadingRateTexture");
static const TCHAR* ScaledConservativeShadingRateTextureName = TEXT("ConservativeScaledShadingRateTexture");

struct RENDERER_API FVRSTextures
{
	// Returns an FVRSTextures created immutable instance from the builder blackboard. Asserts if none was created.
	static const FVRSTextures& Get(FRDGBuilder& GraphBuilder)
	{
		const FVRSTextures* VRSTextures = GraphBuilder.Blackboard.Get<FVRSTextures>();
			checkf(VRSTextures, TEXT("FVRSTextures was unexpectedly not initialized."));
		return *VRSTextures;
	}
	static const bool IsInitialized(FRDGBuilder& GraphBuilder)
	{
		const FVRSTextures* VRSTextures = GraphBuilder.Blackboard.Get<FVRSTextures>();
		return VRSTextures != nullptr;
	}
	void Create(FRDGBuilder& GraphBuilder, const FSceneViewFamily& ViewFamily)
	{
		FRDGTextureDesc ConstructedSRIDesc = CreateSRIDesc(ViewFamily, false);
		ConstructedSRI = GraphBuilder.CreateTexture(ConstructedSRIDesc, ShadingRateTextureName);
		FRDGTextureDesc ScaledSRIDesc = CreateSRIDesc(ViewFamily, true);
		ScaledSRI = GraphBuilder.CreateTexture(ScaledSRIDesc, ScaledShadingRateTextureName);
		ScaledConservativeSRI = GraphBuilder.CreateTexture(ScaledSRIDesc, ScaledConservativeShadingRateTextureName);
	}
	FRDGTextureRef ConstructedSRI;
	FRDGTextureRef ScaledSRI;
	FRDGTextureRef ScaledConservativeSRI;
private:
	static FRDGTextureDesc CreateSRIDesc(const FSceneViewFamily& ViewFamily, bool bIsForDynResScaled)
	{
		FIntPoint TileSize = FVariableRateShadingImageManager::GetSRITileSize();
		FIntPoint ViewTargetExtents = (bIsForDynResScaled)
			? FSceneTexturesConfig::Get().Extent
			: ViewFamily.RenderTarget->GetSizeXY();
		FIntPoint SRIDimensions = FMath::DivideAndRoundUp(ViewTargetExtents, TileSize);
		return FRDGTextureDesc::Create2D(
			SRIDimensions,
			GRHIVariableRateShadingImageFormat,
			EClearBinding::ENoneBound,
			ETextureCreateFlags::DisableDCC |
			ETextureCreateFlags::ShaderResource |
			ETextureCreateFlags::UAV);
	}
};
RDG_REGISTER_BLACKBOARD_STRUCT(FVRSTextures);

static EDisplayOutputFormat GetDisplayOutputFormat(const FSceneView& View)
{
	FTonemapperOutputDeviceParameters Parameters = GetTonemapperOutputDeviceParameters(*View.Family);
	return (EDisplayOutputFormat)Parameters.OutputDevice;
}

static bool IsHDR10(const EDisplayOutputFormat& OutputFormat)
{
	return OutputFormat == EDisplayOutputFormat::HDR_ACES_1000nit_ST2084 ||
		OutputFormat == EDisplayOutputFormat::HDR_ACES_2000nit_ST2084;
}

static bool IsContrastAdaptiveShadingEnabled()
{
	return GRHISupportsAttachmentVariableRateShading && GRHIAttachmentVariableRateShadingEnabled && (CVarVRSContrastAdaptiveShading.GetValueOnRenderThread() != 0);
}

static FIntRect GetPostProcessOutputRect(const FViewInfo& ViewInfo)
{
	// If TAA/TSR is enabled, upscaling is done at the start of post-processing so the final output will match UnscaledViewRect. Otherwise use the dynamically rescale view rect since
	// the secondary upscale will happen after post processing
	return ViewInfo.PrimaryScreenPercentageMethod == EPrimaryScreenPercentageMethod::TemporalUpscale ? ViewInfo.UnscaledViewRect.Scale(ViewInfo.Family->SecondaryViewFraction) : ViewInfo.ViewRect;
}

bool AddCreateShadingRateImagePass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View)
{
	//------------------------------------------------------------------------------------------------
	// Do some sanity checks for early out
	if (!IsContrastAdaptiveShadingEnabled() || !FVariableRateShadingImageManager::IsVRSCompatibleWithView(View) || !View.PrevViewInfo.LuminanceHistory)
	{
		// Shading Rate Image unsupported
		return false;
	}
	FRDGTextureRef Luminance = GraphBuilder.RegisterExternalTexture(View.PrevViewInfo.LuminanceHistory);
	const FVRSTextures& VRSTextures = FVRSTextures::Get(GraphBuilder);
	// Complete early out sanity checks
	//------------------------------------------------------------------------------------------------

	{
		FCalculateShadingRateImageCS::FPermutationDomain PermutationVector;

		const FIntPoint TileSize = FVariableRateShadingImageManager::GetSRITileSize();
		PermutationVector.Set<FCalculateShadingRateImageCS::FThreadGroupX>(TileSize.X);
		PermutationVector.Set<FCalculateShadingRateImageCS::FThreadGroupY>(TileSize.Y);

		// Set an override rate if we're in a debug mode
		int32 ForceRate = GVRSDebugForceRate;
		if (ForceRate != 0 &&
			ForceRate != 1 &&
			ForceRate != 4 &&
			ForceRate != 5)
		{
			ForceRate = -1;
		}

		PermutationVector.Set<FCalculateShadingRateImageCS::FForceRate>(ForceRate);

		TShaderMapRef<FCalculateShadingRateImageCS> ComputeShader(View.ShaderMap, PermutationVector);
		auto* PassParameters = GraphBuilder.AllocParameters<FCalculateShadingRateImageCS::FParameters>();
		EDisplayOutputFormat OutputDisplayFormat = GetDisplayOutputFormat(View);
		FIntRect PostProcessRect = View.UnscaledViewRect.Scale(View.Family->SecondaryViewFraction);
		FCalculateShadingRateImageCS::InitParameters(
			*PassParameters,
			Luminance,
			PostProcessRect,
			IsHDR10(OutputDisplayFormat),
			GraphBuilder.CreateUAV(VRSTextures.ConstructedSRI));
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("CreateShadingRateImage"),
			ERDGPassFlags::AsyncCompute | ERDGPassFlags::NeverCull,
			ComputeShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(PostProcessRect.Size(), TileSize));
	}

	return true;
}

void AddPrepareImageBasedVRSPass(
	FRDGBuilder& GraphBuilder,
	const FMinimalSceneTextures& SceneTextures,
	const FSceneViewFamily& ViewFamily)
{
	SCOPED_DRAW_EVENT(GraphBuilder.RHICmdList, ScaleVariableRateShadingTexture);

	const FVRSTextures& VRSTextures = FVRSTextures::Get(GraphBuilder);
	FRDGTextureRef VariableRateShadingImage = VRSTextures.ConstructedSRI;

	FIntPoint TileSize = FVariableRateShadingImageManager::GetSRITileSize();

	FIntPoint TextureSize = VRSTextures.ScaledSRI->Desc.Extent;
	FVector2f TextureDimensions(TextureSize.X, TextureSize.Y);

	for (int32 ViewIndex = 0; ViewIndex < ViewFamily.Views.Num(); ViewIndex++)
	{
		const FSceneView* View = ViewFamily.Views[ViewIndex];
		RDG_EVENT_SCOPE_CONDITIONAL(GraphBuilder, ViewFamily.Views.Num() > 1, "View%d", ViewIndex);
		check(View->bIsViewInfo);
		const FViewInfo* ViewInfo = static_cast<const FViewInfo*>(View);
		if (View->bCameraCut || !FVariableRateShadingImageManager::IsVRSCompatibleWithView(*ViewInfo) || !ViewInfo->PrevViewInfo.LuminanceHistory)
		{
			break;
		}
		FIntPoint SrcBufferSize = FSceneTexturesConfig::Get().Extent;

		TShaderMapRef<FRescaleVariableRateShadingCS> RescaleVariableRateShadingCS(ViewInfo->ShaderMap);

		int32 ViewportWidth = ViewInfo->ViewRect.Width();
		int32 ViewportHeight = ViewInfo->ViewRect.Height();

		int32 ScaledTilesWide = FMath::DivideAndRoundUp(ViewportWidth, TileSize.X);
		int32 ScaledTilesHigh = FMath::DivideAndRoundUp(ViewportHeight, TileSize.Y);
		FVector2f ScaledSRIDimensions(ScaledTilesWide, ScaledTilesHigh);

		FIntRect PostProcessRect = GetPostProcessOutputRect(*ViewInfo);

		FVector2f SRIViewRectMin(
			static_cast<float>(FMath::DivideAndRoundDown(PostProcessRect.Min.X, TileSize.X)),
			static_cast<float>(FMath::DivideAndRoundDown(PostProcessRect.Min.Y, TileSize.Y)));

		FVector2f SRIViewRectMax(
			static_cast<float>(FMath::DivideAndRoundUp(PostProcessRect.Max.X, TileSize.X)),
			static_cast<float>(FMath::DivideAndRoundUp(PostProcessRect.Max.Y, TileSize.Y)));

		FVector2f UVOffset(
			(float)ViewInfo->ViewRect.Min.X / (float)SrcBufferSize.X,
			(float)ViewInfo->ViewRect.Min.Y / (float)SrcBufferSize.Y);

		float DynamicResolutionScale = (float)ViewportWidth / (float)PostProcessRect.Width();

		auto PassParameters = GraphBuilder.AllocParameters<FRescaleVariableRateShadingCS::FParameters>();

		FRescaleVariableRateShadingCS::InitParameters(
			*PassParameters,
			SceneTextures,
			*ViewInfo,
			VariableRateShadingImage,
			SRIViewRectMin,
			SRIViewRectMax,
			ScaledSRIDimensions,
			TextureDimensions,
			UVOffset,
			DynamicResolutionScale,
			GraphBuilder.CreateUAV(VRSTextures.ScaledSRI),
			GraphBuilder.CreateUAV(VRSTextures.ScaledConservativeSRI));

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("PrepareImageBasedVRS"),
			ERDGPassFlags::AsyncCompute | ERDGPassFlags::NeverCull,
			RescaleVariableRateShadingCS,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(FIntPoint(ScaledTilesWide, ScaledTilesHigh), FRescaleVariableRateShadingCS::ThreadGroupSize));
	}
}

void FContrastAdaptiveImageGenerator::VRSDebugPreview(FRDGBuilder& GraphBuilder, const FSceneViewFamily& ViewFamily, FRDGTextureRef OutputSceneColor)
{
	RDG_EVENT_SCOPE(GraphBuilder, "VariableRateShading");
	for (int32 ViewIndex = 0; ViewIndex < ViewFamily.Views.Num(); ViewIndex++)
	{
		check(ViewFamily.Views[ViewIndex]->bIsViewInfo);
		const FViewInfo& View = *static_cast<const FViewInfo*>(ViewFamily.Views[ViewIndex]);
		RDG_GPU_MASK_SCOPE(GraphBuilder, View.GPUMask);
		RDG_EVENT_SCOPE_CONDITIONAL(GraphBuilder, ViewFamily.Views.Num() > 1, "View%d", ViewIndex);

		//------------------------------------------------------------------------------------------------
		// Do some sanity checks for early out
		if (!OutputSceneColor)
		{
			return;
		}

		ESRIPreviewType::Type PreviewType = static_cast<ESRIPreviewType::Type>(CVarVRSPreview.GetValueOnRenderThread());

		if (PreviewType == ESRIPreviewType::Off || !FVRSTextures::IsInitialized(GraphBuilder))
		{
			return;
		}

		const FVRSTextures& VRSTextures = FVRSTextures::Get(GraphBuilder);
		FRDGTextureRef PreviewTexture = nullptr;
		switch (PreviewType)
		{
		case ESRIPreviewType::BeforeReprojection:
			PreviewTexture = VRSTextures.ConstructedSRI;
			break;
		case ESRIPreviewType::Projected:
			PreviewTexture = VRSTextures.ScaledSRI;
			break;
		case ESRIPreviewType::ProjectedConservative:
			PreviewTexture = VRSTextures.ScaledConservativeSRI;
			break;
		};

		if (!PreviewTexture)
		{
			// We never rendered to this texture this frame, so aborting
			return;
		}
		// Complete early out sanity checks
		//------------------------------------------------------------------------------------------------

		auto& RHICmdList = GraphBuilder.RHICmdList;

		SCOPED_DRAW_EVENT(RHICmdList, VRSDebugPreview);

		bool bUseRescaledTexture = PreviewType != ESRIPreviewType::BeforeReprojection;

		FIntRect SrcViewRect = bUseRescaledTexture ? View.ViewRect : GetPostProcessOutputRect(View);

		const FIntRect& DestViewRect = View.UnscaledViewRect;

		TShaderMapRef<FDebugVariableRateShadingVS> VertexShader(View.ShaderMap);
		TShaderMapRef<FDebugVariableRateShadingPS> PixelShader(View.ShaderMap);

		auto* PassParameters = GraphBuilder.AllocParameters<FDebugVariableRateShadingPS::FParameters>();

		FDebugVariableRateShadingPS::InitParameters(
			*PassParameters,
			PreviewTexture,
			OutputSceneColor);

		FRHIBlendState* BlendState =
			TStaticBlendState<CW_RGB, BO_Add, BF_SourceAlpha, BF_InverseSource1Alpha>::GetRHI();
		FRHIDepthStencilState* DepthStencilState = FScreenPassPipelineState::FDefaultDepthStencilState::GetRHI();

		EScreenPassDrawFlags DrawFlags = EScreenPassDrawFlags::AllowHMDHiddenAreaMask;

		FIntRect ScaledSrcRect = FIntRect::DivideAndRoundUp(SrcViewRect, FVariableRateShadingImageManager::GetSRITileSize());

		const FScreenPassTextureViewport InputViewport = FScreenPassTextureViewport(PreviewTexture, ScaledSrcRect);
		const FScreenPassTextureViewport OutputViewport(OutputSceneColor, DestViewRect);

		AddDrawScreenPass(
			GraphBuilder,
			RDG_EVENT_NAME("Display Debug : %s", ESRIPreviewType::GetName(PreviewType)),
			View,
			OutputViewport,
			InputViewport,
			VertexShader,
			PixelShader,
			BlendState,
			DepthStencilState,
			PassParameters,
			DrawFlags);
	}
}

/**
 * Interface Functions
 */

FRDGTextureRef FContrastAdaptiveImageGenerator::GetImage(FRDGBuilder& GraphBuilder, const FViewInfo& ViewInfo, FVariableRateShadingImageManager::EVRSPassType PassType)
{
	if (FVRSTextures::IsInitialized(GraphBuilder))
	{
		auto TextureType = ESRITextureType::GetTextureType(PassType);
		const FVRSTextures& VRSTextures = FVRSTextures::Get(GraphBuilder);
		return (TextureType == ESRITextureType::ScaledSRIForRender) ? VRSTextures.ScaledSRI : VRSTextures.ScaledConservativeSRI;
	}

	return nullptr;
}

void FContrastAdaptiveImageGenerator::PrepareImages(FRDGBuilder& GraphBuilder, const FSceneViewFamily& ViewFamily, const FMinimalSceneTextures& SceneTextures)
{
	RDG_EVENT_SCOPE(GraphBuilder, "VariableRateShading");
	bool bIsAnyViewVRSCompatible = false;
	for (const FSceneView* View : ViewFamily.Views)
	{
		check(View->bIsViewInfo);
		auto ViewInfo = static_cast<const FViewInfo*>(View);
		if (!View->bCameraCut && FVariableRateShadingImageManager::IsVRSCompatibleWithView(*ViewInfo) && ViewInfo->PrevViewInfo.LuminanceHistory)
		{
			bIsAnyViewVRSCompatible = true;
		}
	}
	bool bPrepareImageBasedVRS = IsContrastAdaptiveShadingEnabled() && bIsAnyViewVRSCompatible;
	if (!bPrepareImageBasedVRS)
	{
		return;
	}

	FVRSTextures& VRSTextures = GraphBuilder.Blackboard.Create<FVRSTextures>();
	VRSTextures.Create(GraphBuilder, ViewFamily);

	for (int32 ViewIndex = 0; ViewIndex < ViewFamily.Views.Num(); ViewIndex++)
	{
		check(ViewFamily.Views[ViewIndex]->bIsViewInfo);
		const FViewInfo& View = *(FViewInfo*)ViewFamily.Views[ViewIndex];
		RDG_GPU_MASK_SCOPE(GraphBuilder, View.GPUMask);
		RDG_EVENT_SCOPE_CONDITIONAL(GraphBuilder, ViewFamily.Views.Num() > 1, "View%d", ViewIndex);
		AddCreateShadingRateImagePass(GraphBuilder, View);
	}
	AddPrepareImageBasedVRSPass(GraphBuilder, SceneTextures, ViewFamily);
}

bool FContrastAdaptiveImageGenerator::IsEnabledForView(const FSceneView& View) const
{
	EDisplayOutputFormat DisplayOutputFormat = GetDisplayOutputFormat(View);
	bool bCompatibleWithOutputType = (DisplayOutputFormat == EDisplayOutputFormat::SDR_sRGB) || IsHDR10(DisplayOutputFormat);

	return IsContrastAdaptiveShadingEnabled() && !View.bIsSceneCapture && bCompatibleWithOutputType;
}

