// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	ShadowDepthRendering.cpp: Shadow depth rendering implementation
=============================================================================*/

#include "CoreMinimal.h"
#include "Stats/Stats.h"
#include "Misc/MemStack.h"
#include "RHIDefinitions.h"
#include "HAL/IConsoleManager.h"
#include "Async/TaskGraphInterfaces.h"
#include "RHI.h"
#include "HitProxies.h"
#include "ShaderParameters.h"
#include "RenderResource.h"
#include "RendererInterface.h"
#include "PrimitiveViewRelevance.h"
#include "UniformBuffer.h"
#include "Shader.h"
#include "StaticBoundShaderState.h"
#include "SceneUtils.h"
#include "Materials/Material.h"
#include "RHIStaticStates.h"
#include "PostProcess/SceneRenderTargets.h"
#include "GlobalShader.h"
#include "MaterialShaderType.h"
#include "MaterialShader.h"
#include "MeshMaterialShader.h"
#include "ShaderBaseClasses.h"
#include "ShadowRendering.h"
#include "SceneRendering.h"
#include "ScenePrivate.h"
#include "PostProcess/SceneFilterRendering.h"
#include "ScreenRendering.h"
#include "ClearQuad.h"
#include "PipelineStateCache.h"
#include "MeshPassProcessor.inl"
#include "VisualizeTexture.h"
#include "GPUScene.h"
#include "SceneTextureReductions.h"
#include "RendererModule.h"
#include "PixelShaderUtils.h"
#include "VirtualShadowMaps/VirtualShadowMapCacheManager.h"
#include "VirtualShadowMaps/VirtualShadowMapClipmap.h"

DECLARE_GPU_DRAWCALL_STAT_NAMED(ShadowDepths, TEXT("Shadow Depths"));

IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FShadowDepthPassUniformParameters, "ShadowDepthPass");
IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FMobileShadowDepthPassUniformParameters, "MobileShadowDepthPass");

template<bool bUsingVertexLayers = false>
class TScreenVSForGS : public FScreenVS
{
	DECLARE_SHADER_TYPE(TScreenVSForGS, Global);
public:

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5) && (!bUsingVertexLayers || RHISupportsVertexShaderLayer(Parameters.Platform));
	}

	TScreenVSForGS(const ShaderMetaType::CompiledShaderInitializerType& Initializer) :
		FScreenVS(Initializer)
	{
	}
	TScreenVSForGS() {}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FScreenVS::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("USING_LAYERS"), (uint32)(bUsingVertexLayers ? 1 : 0));
		if (!bUsingVertexLayers)
		{
			OutEnvironment.CompilerFlags.Add(CFLAG_VertexToGeometryShader);
		}
	}
};

IMPLEMENT_SHADER_TYPE(template<>, TScreenVSForGS<false>, TEXT("/Engine/Private/ScreenVertexShader.usf"), TEXT("MainForGS"), SF_Vertex);
IMPLEMENT_SHADER_TYPE(template<>, TScreenVSForGS<true>, TEXT("/Engine/Private/ScreenVertexShader.usf"), TEXT("MainForGS"), SF_Vertex);


static TAutoConsoleVariable<int32> CVarShadowForceSerialSingleRenderPass(
	TEXT("r.Shadow.ForceSerialSingleRenderPass"),
	0,
	TEXT("Force Serial shadow passes to render in 1 pass."),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarNaniteShadows(
	TEXT("r.Shadow.Nanite"),
	1,
	TEXT("Enables shadows from Nanite meshes."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarNaniteShadowsUseHZB(
	TEXT("r.Shadow.NaniteUseHZB"),
	1,
	TEXT("Enables HZB for Nanite shadows."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarNaniteShadowsLODBias(
	TEXT("r.Shadow.NaniteLODBias"),
	1.0f,
	TEXT("LOD bias for nanite geometry in shadows. 0 = full detail. >0 = reduced detail."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarNaniteShadowsUpdateStreaming(
	TEXT("r.Shadow.NaniteUpdateStreaming"),
	1,
	TEXT("Produce Nanite geometry streaming requests from shadow map rendering."),
	ECVF_RenderThreadSafe);

int32 GShadowUseGS = 1;
static FAutoConsoleVariableRef CVarShadowShadowUseGS(
	TEXT("r.Shadow.UseGS"),
	GShadowUseGS,
	TEXT("Use geometry shaders to render cube map shadows."),
	ECVF_RenderThreadSafe);


extern int32 GNaniteDebugFlags;
extern int32 GNaniteShowStats;

namespace Nanite
{
	extern bool IsStatFilterActive(const FString& FilterName);
	extern bool IsStatFilterActiveForLight(const FLightSceneProxy* LightProxy);
	extern FString GetFilterNameForLight(const FLightSceneProxy* LightProxy);
}

// Multiply PackedView.LODScale by return value when rendering Nanite shadows
static float ComputeNaniteShadowsLODScaleFactor()
{
	return FMath::Pow(2.0f, -CVarNaniteShadowsLODBias.GetValueOnRenderThread());
}

void SetupShadowDepthPassUniformBuffer(
	const FProjectedShadowInfo* ShadowInfo,
	FRHICommandListImmediate& RHICmdList,
	const FViewInfo& View,
	FShadowDepthPassUniformParameters& ShadowDepthPassParameters)
{
	FSceneRenderTargets& SceneRenderTargets = FSceneRenderTargets::Get(RHICmdList);
	SetupSceneTextureUniformParameters(SceneRenderTargets, View.FeatureLevel, ESceneTextureSetupMode::None, ShadowDepthPassParameters.SceneTextures);

	ShadowDepthPassParameters.ProjectionMatrix = FTranslationMatrix(ShadowInfo->PreShadowTranslation - View.ViewMatrices.GetPreViewTranslation()) * ShadowInfo->SubjectAndReceiverMatrix;	
	ShadowDepthPassParameters.ViewMatrix = ShadowInfo->ShadowViewMatrix;

	ShadowDepthPassParameters.ShadowParams = FVector4(ShadowInfo->GetShaderDepthBias(), ShadowInfo->GetShaderSlopeDepthBias(), ShadowInfo->GetShaderMaxSlopeDepthBias(), ShadowInfo->bOnePassPointLightShadow ? 1 : ShadowInfo->InvMaxSubjectDepth);
	ShadowDepthPassParameters.bClampToNearPlane = ShadowInfo->ShouldClampToNearPlane() ? 1.0f : 0.0f;

	if (ShadowInfo->bOnePassPointLightShadow)
	{
		// offset from translated world space to (pre translated) shadow space 
		const FMatrix Translation = FTranslationMatrix(ShadowInfo->PreShadowTranslation - View.ViewMatrices.GetPreViewTranslation());

		for (int32 FaceIndex = 0; FaceIndex < 6; FaceIndex++)
		{
			ShadowDepthPassParameters.ShadowViewProjectionMatrices[FaceIndex] = Translation * ShadowInfo->OnePassShadowViewProjectionMatrices[FaceIndex];
			ShadowDepthPassParameters.ShadowViewMatrices[FaceIndex] = Translation * ShadowInfo->OnePassShadowViewMatrices[FaceIndex];
		}
	}
}

void SetupShadowDepthPassUniformBuffer(
	const FProjectedShadowInfo* ShadowInfo,
	FRHICommandListImmediate& RHICmdList,
	const FViewInfo& View,
	FMobileShadowDepthPassUniformParameters& ShadowDepthPassParameters)
{
	FSceneRenderTargets& SceneRenderTargets = FSceneRenderTargets::Get(RHICmdList);
	SetupMobileSceneTextureUniformParameters(SceneRenderTargets, EMobileSceneTextureSetupMode::None, ShadowDepthPassParameters.SceneTextures);

	ShadowDepthPassParameters.ProjectionMatrix = FTranslationMatrix(ShadowInfo->PreShadowTranslation - View.ViewMatrices.GetPreViewTranslation()) * ShadowInfo->SubjectAndReceiverMatrix;
	ShadowDepthPassParameters.ViewMatrix = ShadowInfo->ShadowViewMatrix;

	ShadowDepthPassParameters.ShadowParams = FVector4(ShadowInfo->GetShaderDepthBias(), ShadowInfo->GetShaderSlopeDepthBias(), ShadowInfo->GetShaderMaxSlopeDepthBias(), ShadowInfo->InvMaxSubjectDepth);
	ShadowDepthPassParameters.bClampToNearPlane = ShadowInfo->ShouldClampToNearPlane() ? 1.0f : 0.0f;
}

class FShadowDepthShaderElementData : public FMeshMaterialShaderElementData
{
public:

	int32 LayerId;
};

/**
* A vertex shader for rendering the depth of a mesh.
*/
class FShadowDepthVS : public FMeshMaterialShader
{
	DECLARE_INLINE_TYPE_LAYOUT(FShadowDepthVS, NonVirtual);
public:

	static bool ShouldCompilePermutation(const FMeshMaterialShaderPermutationParameters& Parameters)
	{
		return false;
	}

	FShadowDepthVS(const ShaderMetaType::CompiledShaderInitializerType& Initializer) :
		FMeshMaterialShader(Initializer)
	{
		const ERHIFeatureLevel::Type FeatureLevel = GetMaxSupportedFeatureLevel((EShaderPlatform)Initializer.Target.Platform);

		if (FSceneInterface::GetShadingPath(FeatureLevel) == EShadingPath::Deferred)
		{
			PassUniformBuffer.Bind(Initializer.ParameterMap, FShadowDepthPassUniformParameters::StaticStructMetadata.GetShaderVariableName());
		}

		if (FSceneInterface::GetShadingPath(FeatureLevel) == EShadingPath::Mobile)
		{
			PassUniformBuffer.Bind(Initializer.ParameterMap, FMobileShadowDepthPassUniformParameters::StaticStructMetadata.GetShaderVariableName());
		}

		LayerId.Bind(Initializer.ParameterMap, TEXT("LayerId"));
	}

	FShadowDepthVS() {}

	void GetShaderBindings(
		const FScene* Scene,
		ERHIFeatureLevel::Type FeatureLevel,
		const FPrimitiveSceneProxy* PrimitiveSceneProxy,
		const FMaterialRenderProxy& MaterialRenderProxy,
		const FMaterial& Material,
		const FMeshPassProcessorRenderState& DrawRenderState,
		const FShadowDepthShaderElementData& ShaderElementData,
		FMeshDrawSingleShaderBindings& ShaderBindings) const
	{
		FMeshMaterialShader::GetShaderBindings(Scene, FeatureLevel, PrimitiveSceneProxy, MaterialRenderProxy, Material, DrawRenderState, ShaderElementData, ShaderBindings);

		ShaderBindings.Add(LayerId, ShaderElementData.LayerId);
	}

private:
	
	LAYOUT_FIELD(FShaderParameter, LayerId);
};

enum EShadowDepthVertexShaderMode
{
	VertexShadowDepth_PerspectiveCorrect,
	VertexShadowDepth_OutputDepth,
	VertexShadowDepth_OnePassPointLight,
	VertexShadowDepth_VSLayer,
};

static TAutoConsoleVariable<int32> CVarSupportPointLightWholeSceneShadows(
	TEXT("r.SupportPointLightWholeSceneShadows"),
	1,
	TEXT("Enables shadowcasting point lights."),
	ECVF_ReadOnly | ECVF_RenderThreadSafe);

/**
* A vertex shader for rendering the depth of a mesh.
*/
template <EShadowDepthVertexShaderMode ShaderMode, bool bUsePositionOnlyStream, bool bIsForGeometryShader = false>
class TShadowDepthVS : public FShadowDepthVS
{
	DECLARE_SHADER_TYPE(TShadowDepthVS, MeshMaterial);
public:

	TShadowDepthVS(const ShaderMetaType::CompiledShaderInitializerType& Initializer) :
		FShadowDepthVS(Initializer)
	{
	}

	TShadowDepthVS() {}

	static bool ShouldCompilePermutation(const FMeshMaterialShaderPermutationParameters& Parameters)
	{
		const EShaderPlatform Platform = Parameters.Platform;

		static const auto SupportAllShaderPermutationsVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.SupportAllShaderPermutations"));
		const bool bForceAllPermutations = SupportAllShaderPermutationsVar && SupportAllShaderPermutationsVar->GetValueOnAnyThread() != 0;
		const bool bSupportPointLightWholeSceneShadows = CVarSupportPointLightWholeSceneShadows.GetValueOnAnyThread() != 0 || bForceAllPermutations;
		const bool bRHISupportsShadowCastingPointLights = RHISupportsGeometryShaders(Platform) || RHISupportsVertexShaderLayer(Platform);

		if (bIsForGeometryShader && ShaderMode == VertexShadowDepth_VSLayer)
		{
			return false;
		}

		if (bIsForGeometryShader && (!bSupportPointLightWholeSceneShadows || !bRHISupportsShadowCastingPointLights))
		{
			return false;
		}

		//Note: This logic needs to stay in sync with OverrideWithDefaultMaterialForShadowDepth!
		return (Parameters.MaterialParameters.bIsSpecialEngineMaterial
			// Masked and WPO materials need their shaders but cannot be used with a position only stream.
			|| ((!Parameters.MaterialParameters.bWritesEveryPixelShadowPass || Parameters.MaterialParameters.bMaterialMayModifyMeshPosition) && !bUsePositionOnlyStream))
			// Only compile one pass point light shaders for feature levels >= SM5
			&& (ShaderMode != VertexShadowDepth_OnePassPointLight || IsFeatureLevelSupported(Platform, ERHIFeatureLevel::SM5))
			// Only compile position-only shaders for vertex factories that support it. (Note: this assumes that a vertex factor which supports PositionOnly, supports also PositionAndNormalOnly)
			&& (!bUsePositionOnlyStream || Parameters.VertexFactoryType->SupportsPositionOnly())
			// Don't render ShadowDepth for translucent unlit materials
			&& Parameters.MaterialParameters.bShouldCastDynamicShadows;
	}

	static void ModifyCompilationEnvironment(const FMaterialShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FShadowDepthVS::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("PERSPECTIVE_CORRECT_DEPTH"), (uint32)(ShaderMode == VertexShadowDepth_PerspectiveCorrect));
		OutEnvironment.SetDefine(TEXT("ONEPASS_POINTLIGHT_SHADOW"), (uint32)(ShaderMode == VertexShadowDepth_OnePassPointLight || ShaderMode == VertexShadowDepth_VSLayer));
		OutEnvironment.SetDefine(TEXT("USING_VERTEX_SHADER_LAYER"), (uint32)(ShaderMode == VertexShadowDepth_VSLayer));
		OutEnvironment.SetDefine(TEXT("POSITION_ONLY"), (uint32)bUsePositionOnlyStream);
		OutEnvironment.SetDefine(TEXT("IS_FOR_GEOMETRY_SHADER"), (uint32)bIsForGeometryShader);

		if (bIsForGeometryShader)
		{
			OutEnvironment.CompilerFlags.Add(CFLAG_VertexToGeometryShader);
		}
		else if (ShaderMode == VertexShadowDepth_VSLayer)
		{
			OutEnvironment.CompilerFlags.Add(CFLAG_VertexUseAutoCulling);
		}
	}
};


/**
* A Hull shader for rendering the depth of a mesh.
*/
template <EShadowDepthVertexShaderMode ShaderMode>
class TShadowDepthHS : public FBaseHS
{
	DECLARE_SHADER_TYPE(TShadowDepthHS, MeshMaterial);
public:


	TShadowDepthHS(const ShaderMetaType::CompiledShaderInitializerType& Initializer) :
		FBaseHS(Initializer)
	{}

	TShadowDepthHS() {}

	static bool ShouldCompilePermutation(const FMeshMaterialShaderPermutationParameters& Parameters)
	{
		// Re-use ShouldCache from vertex shader
		return FBaseHS::ShouldCompilePermutation(Parameters)
			&& TShadowDepthVS<ShaderMode, false>::ShouldCompilePermutation(Parameters);
	}

	static void ModifyCompilationEnvironment(const FMaterialShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		// Re-use compilation env from vertex shader

		TShadowDepthVS<ShaderMode, false>::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};

/**
* A Domain shader for rendering the depth of a mesh.
*/
template <EShadowDepthVertexShaderMode ShaderMode>
class TShadowDepthDS : public FBaseDS
{
	DECLARE_SHADER_TYPE(TShadowDepthDS, MeshMaterial);
public:

	TShadowDepthDS(const ShaderMetaType::CompiledShaderInitializerType& Initializer) :
		FBaseDS(Initializer)
	{
		const ERHIFeatureLevel::Type FeatureLevel = GetMaxSupportedFeatureLevel((EShaderPlatform)Initializer.Target.Platform);

		if (FSceneInterface::GetShadingPath(FeatureLevel) == EShadingPath::Deferred)
		{
			PassUniformBuffer.Bind(Initializer.ParameterMap, FShadowDepthPassUniformParameters::StaticStructMetadata.GetShaderVariableName());
		}

		if (FSceneInterface::GetShadingPath(FeatureLevel) == EShadingPath::Mobile)
		{
			PassUniformBuffer.Bind(Initializer.ParameterMap, FMobileShadowDepthPassUniformParameters::StaticStructMetadata.GetShaderVariableName());
		}
	}

	TShadowDepthDS() {}

	static bool ShouldCompilePermutation(const FMeshMaterialShaderPermutationParameters& Parameters)
	{
		// Re-use ShouldCache from vertex shader
		return FBaseDS::ShouldCompilePermutation(Parameters)
			&& TShadowDepthVS<ShaderMode, false>::ShouldCompilePermutation(Parameters);
	}

	static void ModifyCompilationEnvironment(const FMaterialShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		// Re-use compilation env from vertex shader
		TShadowDepthVS<ShaderMode, false>::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};

/** Geometry shader that allows one pass point light shadows by cloning triangles to all faces of the cube map. */
class FOnePassPointShadowDepthGS : public FMeshMaterialShader
{
	DECLARE_SHADER_TYPE(FOnePassPointShadowDepthGS, MeshMaterial);
public:

	static bool ShouldCompilePermutation(const FMeshMaterialShaderPermutationParameters& Parameters)
	{
		return RHISupportsGeometryShaders(Parameters.Platform) && TShadowDepthVS<VertexShadowDepth_OnePassPointLight, false, true>::ShouldCompilePermutation(Parameters);
	}

	static void ModifyCompilationEnvironment(const FMaterialShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FMeshMaterialShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("ONEPASS_POINTLIGHT_SHADOW"), 1);
		TShadowDepthVS<VertexShadowDepth_OnePassPointLight, false, true>::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}

	FOnePassPointShadowDepthGS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FMeshMaterialShader(Initializer)
	{
		const ERHIFeatureLevel::Type FeatureLevel = GetMaxSupportedFeatureLevel((EShaderPlatform)Initializer.Target.Platform);

		if (FSceneInterface::GetShadingPath(FeatureLevel) == EShadingPath::Deferred)
		{
			PassUniformBuffer.Bind(Initializer.ParameterMap, FShadowDepthPassUniformParameters::StaticStructMetadata.GetShaderVariableName());
		}

		if (FSceneInterface::GetShadingPath(FeatureLevel) == EShadingPath::Mobile)
		{
			PassUniformBuffer.Bind(Initializer.ParameterMap, FMobileShadowDepthPassUniformParameters::StaticStructMetadata.GetShaderVariableName());
		}
	}

	FOnePassPointShadowDepthGS() {}
};

IMPLEMENT_SHADER_TYPE(, FOnePassPointShadowDepthGS, TEXT("/Engine/Private/ShadowDepthVertexShader.usf"), TEXT("MainOnePassPointLightGS"), SF_Geometry);

#define IMPLEMENT_SHADOW_DEPTH_SHADERMODE_SHADERS(ShaderMode) \
	typedef TShadowDepthVS<ShaderMode, false> TShadowDepthVS##ShaderMode; \
	typedef TShadowDepthHS<ShaderMode       > TShadowDepthHS##ShaderMode; \
	typedef TShadowDepthDS<ShaderMode       > TShadowDepthDS##ShaderMode; \
	IMPLEMENT_MATERIAL_SHADER_TYPE(template<>, TShadowDepthVS##ShaderMode, TEXT("/Engine/Private/ShadowDepthVertexShader.usf"), TEXT("Main"),       SF_Vertex); \
	IMPLEMENT_MATERIAL_SHADER_TYPE(template<>, TShadowDepthHS##ShaderMode, TEXT("/Engine/Private/ShadowDepthVertexShader.usf"), TEXT("MainHull"),   SF_Hull  ); \
	IMPLEMENT_MATERIAL_SHADER_TYPE(template<>, TShadowDepthDS##ShaderMode, TEXT("/Engine/Private/ShadowDepthVertexShader.usf"), TEXT("MainDomain"), SF_Domain);

IMPLEMENT_SHADOW_DEPTH_SHADERMODE_SHADERS(VertexShadowDepth_PerspectiveCorrect);
IMPLEMENT_SHADOW_DEPTH_SHADERMODE_SHADERS(VertexShadowDepth_OutputDepth);
IMPLEMENT_SHADOW_DEPTH_SHADERMODE_SHADERS(VertexShadowDepth_OnePassPointLight);

// Position only vertex shaders.
typedef TShadowDepthVS<VertexShadowDepth_PerspectiveCorrect, true> TShadowDepthVSVertexShadowDepth_PerspectiveCorrectPositionOnly;
typedef TShadowDepthVS<VertexShadowDepth_OutputDepth,        true> TShadowDepthVSVertexShadowDepth_OutputDepthPositionOnly;
typedef TShadowDepthVS<VertexShadowDepth_OnePassPointLight,  true> TShadowDepthVSVertexShadowDepth_OnePassPointLightPositionOnly;
IMPLEMENT_MATERIAL_SHADER_TYPE(template<>, TShadowDepthVSVertexShadowDepth_PerspectiveCorrectPositionOnly, TEXT("/Engine/Private/ShadowDepthVertexShader.usf"), TEXT("PositionOnlyMain"), SF_Vertex);
IMPLEMENT_MATERIAL_SHADER_TYPE(template<>, TShadowDepthVSVertexShadowDepth_OutputDepthPositionOnly,        TEXT("/Engine/Private/ShadowDepthVertexShader.usf"), TEXT("PositionOnlyMain"), SF_Vertex);
IMPLEMENT_MATERIAL_SHADER_TYPE(template<>, TShadowDepthVSVertexShadowDepth_OnePassPointLightPositionOnly,  TEXT("/Engine/Private/ShadowDepthVertexShader.usf"), TEXT("PositionOnlyMain"), SF_Vertex);

// One pass point light VS for GS shaders.
typedef TShadowDepthVS<VertexShadowDepth_OnePassPointLight, false, true> TShadowDepthVSForGSVertexShadowDepth_OnePassPointLight;
typedef TShadowDepthVS<VertexShadowDepth_OnePassPointLight, true,  true> TShadowDepthVSForGSVertexShadowDepth_OnePassPointLightPositionOnly;
IMPLEMENT_MATERIAL_SHADER_TYPE(template<>, TShadowDepthVSForGSVertexShadowDepth_OnePassPointLight,             TEXT("/Engine/Private/ShadowDepthVertexShader.usf"), TEXT("MainForGS"),             SF_Vertex);
IMPLEMENT_MATERIAL_SHADER_TYPE(template<>, TShadowDepthVSForGSVertexShadowDepth_OnePassPointLightPositionOnly, TEXT("/Engine/Private/ShadowDepthVertexShader.usf"), TEXT("PositionOnlyMainForGS"), SF_Vertex);

// One pass point light with vertex shader layer output.
//                                                       bUsePositionOnlyStream
//                                                            | bIsForGeometryShader
//                                                            |      |
typedef TShadowDepthVS<VertexShadowDepth_VSLayer, false, false> TShadowDepthVSVertexShadowDepth_VSLayer;
typedef TShadowDepthVS<VertexShadowDepth_VSLayer, true,  false> TShadowDepthVSVertexShadowDepth_VSLayerPositionOnly;
typedef TShadowDepthVS<VertexShadowDepth_VSLayer, false, true>  TShadowDepthVSVertexShadowDepth_VSLayerGS; // not used
typedef TShadowDepthVS<VertexShadowDepth_VSLayer, true,  true>  TShadowDepthVSVertexShadowDepth_VSLayerGSPositionOnly; // not used
IMPLEMENT_MATERIAL_SHADER_TYPE(template<>, TShadowDepthVSVertexShadowDepth_VSLayer,               TEXT("/Engine/Private/ShadowDepthVertexShader.usf"), TEXT("Main"),             SF_Vertex);
IMPLEMENT_MATERIAL_SHADER_TYPE(template<>, TShadowDepthVSVertexShadowDepth_VSLayerPositionOnly,   TEXT("/Engine/Private/ShadowDepthVertexShader.usf"), TEXT("PositionOnlyMain"), SF_Vertex);
IMPLEMENT_MATERIAL_SHADER_TYPE(template<>, TShadowDepthVSVertexShadowDepth_VSLayerGS,             TEXT("/Engine/Private/ShadowDepthVertexShader.usf"), TEXT("Main"),             SF_Vertex); // not used
IMPLEMENT_MATERIAL_SHADER_TYPE(template<>, TShadowDepthVSVertexShadowDepth_VSLayerGSPositionOnly, TEXT("/Engine/Private/ShadowDepthVertexShader.usf"), TEXT("PositionOnlyMain"), SF_Vertex); // not used

/**
* A pixel shader for rendering the depth of a mesh.
*/
class FShadowDepthBasePS : public FMeshMaterialShader
{
	DECLARE_INLINE_TYPE_LAYOUT(FShadowDepthBasePS, NonVirtual);
public:

	FShadowDepthBasePS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FMeshMaterialShader(Initializer)
	{
		const ERHIFeatureLevel::Type FeatureLevel = GetMaxSupportedFeatureLevel((EShaderPlatform)Initializer.Target.Platform);

		if (FSceneInterface::GetShadingPath(FeatureLevel) == EShadingPath::Deferred)
		{
			PassUniformBuffer.Bind(Initializer.ParameterMap, FShadowDepthPassUniformParameters::StaticStructMetadata.GetShaderVariableName());
		}

		if (FSceneInterface::GetShadingPath(FeatureLevel) == EShadingPath::Mobile)
		{
			PassUniformBuffer.Bind(Initializer.ParameterMap, FMobileShadowDepthPassUniformParameters::StaticStructMetadata.GetShaderVariableName());
		}
	}

	FShadowDepthBasePS() = default;
};

enum EShadowDepthPixelShaderMode
{
	PixelShadowDepth_NonPerspectiveCorrect,
	PixelShadowDepth_PerspectiveCorrect,
	PixelShadowDepth_OnePassPointLight
};

template <EShadowDepthPixelShaderMode ShaderMode>
class TShadowDepthPS : public FShadowDepthBasePS
{
	DECLARE_SHADER_TYPE(TShadowDepthPS, MeshMaterial);
public:

	static bool ShouldCompilePermutation(const FMeshMaterialShaderPermutationParameters& Parameters)
	{
		const EShaderPlatform Platform = Parameters.Platform;

		if (!IsFeatureLevelSupported(Platform, ERHIFeatureLevel::SM5))
		{
			return (Parameters.MaterialParameters.bIsSpecialEngineMaterial
				// Only compile for masked or lit translucent materials
				|| !Parameters.MaterialParameters.bWritesEveryPixelShadowPass
				|| (Parameters.MaterialParameters.bMaterialMayModifyMeshPosition && Parameters.MaterialParameters.bIsUsedWithInstancedStaticMeshes)
				// Perspective correct rendering needs a pixel shader and WPO materials can't be overridden with default material.
				|| (ShaderMode == PixelShadowDepth_PerspectiveCorrect && Parameters.MaterialParameters.bMaterialMayModifyMeshPosition))
				&& ShaderMode != PixelShadowDepth_OnePassPointLight
				// Don't render ShadowDepth for translucent unlit materials
				&& Parameters.MaterialParameters.bShouldCastDynamicShadows;
		}

		//Note: This logic needs to stay in sync with OverrideWithDefaultMaterialForShadowDepth!
		return (Parameters.MaterialParameters.bIsSpecialEngineMaterial
			// Only compile for masked or lit translucent materials
			|| !Parameters.MaterialParameters.bWritesEveryPixelShadowPass
			|| (Parameters.MaterialParameters.bMaterialMayModifyMeshPosition && Parameters.MaterialParameters.bIsUsedWithInstancedStaticMeshes)
			// Perspective correct rendering needs a pixel shader and WPO materials can't be overridden with default material.
			|| (ShaderMode == PixelShadowDepth_PerspectiveCorrect && Parameters.MaterialParameters.bMaterialMayModifyMeshPosition))
			// Only compile one pass point light shaders for feature levels >= SM5
			&& (ShaderMode != PixelShadowDepth_OnePassPointLight || IsFeatureLevelSupported(Platform, ERHIFeatureLevel::SM5))
			// Don't render ShadowDepth for translucent unlit materials
			&& Parameters.MaterialParameters.bShouldCastDynamicShadows
			&& IsFeatureLevelSupported(Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FMaterialShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FShadowDepthBasePS::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("PERSPECTIVE_CORRECT_DEPTH"), (uint32)(ShaderMode == PixelShadowDepth_PerspectiveCorrect));
		OutEnvironment.SetDefine(TEXT("ONEPASS_POINTLIGHT_SHADOW"), (uint32)(ShaderMode == PixelShadowDepth_OnePassPointLight));
	}

	TShadowDepthPS()
	{
	}

	TShadowDepthPS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FShadowDepthBasePS(Initializer)
	{
	}
};

// typedef required to get around macro expansion failure due to commas in template argument list for TShadowDepthPixelShader
#define IMPLEMENT_SHADOWDEPTHPASS_PIXELSHADER_TYPE(ShaderMode) \
	typedef TShadowDepthPS<ShaderMode> TShadowDepthPS##ShaderMode; \
	IMPLEMENT_MATERIAL_SHADER_TYPE(template<>,TShadowDepthPS##ShaderMode,TEXT("/Engine/Private/ShadowDepthPixelShader.usf"),TEXT("Main"),SF_Pixel);

IMPLEMENT_SHADOWDEPTHPASS_PIXELSHADER_TYPE(PixelShadowDepth_NonPerspectiveCorrect);
IMPLEMENT_SHADOWDEPTHPASS_PIXELSHADER_TYPE(PixelShadowDepth_PerspectiveCorrect);
IMPLEMENT_SHADOWDEPTHPASS_PIXELSHADER_TYPE(PixelShadowDepth_OnePassPointLight);

/**
* Overrides a material used for shadow depth rendering with the default material when appropriate.
* Overriding in this manner can reduce state switches and the number of shaders that have to be compiled.
* This logic needs to stay in sync with shadow depth shader ShouldCache logic.
*/
void OverrideWithDefaultMaterialForShadowDepth(
	const FMaterialRenderProxy*& InOutMaterialRenderProxy,
	const FMaterial*& InOutMaterialResource,
	ERHIFeatureLevel::Type InFeatureLevel)
{
	// Override with the default material when possible.
	if (InOutMaterialResource->WritesEveryPixel(true) &&						// Don't override masked materials.
		!InOutMaterialResource->MaterialModifiesMeshPosition_RenderThread())	// Don't override materials using world position offset.
	{
		const FMaterialRenderProxy* DefaultProxy = UMaterial::GetDefaultMaterial(MD_Surface)->GetRenderProxy();
		const FMaterial* DefaultMaterialResource = DefaultProxy->GetMaterialNoFallback(InFeatureLevel);
		check(DefaultMaterialResource);

		// Override with the default material for opaque materials that don't modify mesh position.
		InOutMaterialRenderProxy = DefaultProxy;
		InOutMaterialResource = DefaultMaterialResource;
	}
}

bool GetShadowDepthPassShaders(
	const FMaterial& Material,
	const FVertexFactory* VertexFactory,
	ERHIFeatureLevel::Type FeatureLevel,
	bool bDirectionalLight,
	bool bOnePassPointLightShadow,
	bool bPositionOnlyVS,
	TShaderRef<FShadowDepthVS>& VertexShader,
	TShaderRef<FBaseHS>& HullShader,
	TShaderRef<FBaseDS>& DomainShader,
	TShaderRef<FShadowDepthBasePS>& PixelShader,
	TShaderRef<FOnePassPointShadowDepthGS>& GeometryShader)
{
	// Use perspective correct shadow depths for shadow types which typically render low poly meshes into the shadow depth buffer.
	// Depth will be interpolated to the pixel shader and written out, which disables HiZ and double speed Z.
	// Directional light shadows use an ortho projection and can use the non-perspective correct path without artifacts.
	// One pass point lights don't output a linear depth, so they are already perspective correct.
	const bool bUsePerspectiveCorrectShadowDepths = !bDirectionalLight && !bOnePassPointLightShadow;

	const FVertexFactoryType* VFType = VertexFactory->GetType();

	const bool bInitializeTessellationShaders =
		Material.GetTessellationMode() != MTM_NoTessellation
		&& RHISupportsTessellation(GShaderPlatformForFeatureLevel[FeatureLevel])
		&& VFType->SupportsTessellationShaders();

	FMaterialShaderTypes ShaderTypes;

	// Vertex related shaders
	if (bOnePassPointLightShadow)
	{
		if (GShadowUseGS)
		{
			if (bPositionOnlyVS)
			{
				ShaderTypes.AddShaderType<TShadowDepthVS<VertexShadowDepth_OnePassPointLight, true, true>>();
			}
			else
			{
				ShaderTypes.AddShaderType<TShadowDepthVS<VertexShadowDepth_OnePassPointLight, false, true>>();
			}

			if (RHISupportsGeometryShaders(GShaderPlatformForFeatureLevel[FeatureLevel]))
			{
				// Use the geometry shader which will clone output triangles to all faces of the cube map
				ShaderTypes.AddShaderType<FOnePassPointShadowDepthGS>();
			}

			if (bInitializeTessellationShaders)
			{
				ShaderTypes.AddShaderType<TShadowDepthHS<VertexShadowDepth_OnePassPointLight>>();
				ShaderTypes.AddShaderType<TShadowDepthDS<VertexShadowDepth_OnePassPointLight>>();
			}
		}
		else
		{
			if (bPositionOnlyVS)
			{
				ShaderTypes.AddShaderType<TShadowDepthVS<VertexShadowDepth_VSLayer, true, false>>();
			}
			else
			{
				ShaderTypes.AddShaderType<TShadowDepthVS<VertexShadowDepth_VSLayer, false, false>>();
			}
		}
	}
	else if (bUsePerspectiveCorrectShadowDepths)
	{
		if (bPositionOnlyVS)
		{
			ShaderTypes.AddShaderType<TShadowDepthVS<VertexShadowDepth_PerspectiveCorrect, true>>();
		}
		else
		{
			ShaderTypes.AddShaderType<TShadowDepthVS<VertexShadowDepth_PerspectiveCorrect, false>>();
		}

		if (bInitializeTessellationShaders)
		{
			ShaderTypes.AddShaderType<TShadowDepthHS<VertexShadowDepth_PerspectiveCorrect>>();
			ShaderTypes.AddShaderType<TShadowDepthDS<VertexShadowDepth_PerspectiveCorrect>>();
		}
	}
	else
	{

		if (bPositionOnlyVS)
		{
			ShaderTypes.AddShaderType<TShadowDepthVS<VertexShadowDepth_OutputDepth, true>>();
		}
		else
		{
			ShaderTypes.AddShaderType<TShadowDepthVS<VertexShadowDepth_OutputDepth, false>>();
		}

		if (bInitializeTessellationShaders)
		{
			ShaderTypes.AddShaderType<TShadowDepthHS<VertexShadowDepth_OutputDepth>>();
			ShaderTypes.AddShaderType<TShadowDepthDS<VertexShadowDepth_OutputDepth>>();
		}
	}

	// Pixel shaders
	const bool bNullPixelShader = Material.WritesEveryPixel(true) && !bUsePerspectiveCorrectShadowDepths && VertexFactory->SupportsNullPixelShader();
	if (!bNullPixelShader)
	{
		if (bUsePerspectiveCorrectShadowDepths)
		{
			ShaderTypes.AddShaderType<TShadowDepthPS<PixelShadowDepth_PerspectiveCorrect>>();
		}
		else if (bOnePassPointLightShadow)
		{
			ShaderTypes.AddShaderType<TShadowDepthPS<PixelShadowDepth_OnePassPointLight>>();
		}
		else
		{
			ShaderTypes.AddShaderType<TShadowDepthPS<PixelShadowDepth_NonPerspectiveCorrect>>();
		}
	}

	FMaterialShaders Shaders;
	if (!Material.TryGetShaders(ShaderTypes, VFType, Shaders))
	{
		return false;
	}

	Shaders.TryGetHullShader(HullShader);
	Shaders.TryGetDomainShader(DomainShader);
	Shaders.TryGetGeometryShader(GeometryShader);
	Shaders.TryGetVertexShader(VertexShader);
	Shaders.TryGetPixelShader(PixelShader);
	return true;
}

/*-----------------------------------------------------------------------------
FProjectedShadowInfo
-----------------------------------------------------------------------------*/

static void CheckShadowDepthMaterials(const FMaterialRenderProxy* InRenderProxy, const FMaterial* InMaterial, ERHIFeatureLevel::Type InFeatureLevel)
{
	const FMaterialRenderProxy* RenderProxy = InRenderProxy;
	const FMaterial* Material = InMaterial;
	OverrideWithDefaultMaterialForShadowDepth(RenderProxy, Material, InFeatureLevel);
	check(RenderProxy == InRenderProxy);
	check(Material == InMaterial);
}

void FProjectedShadowInfo::ClearDepth(FRHICommandList& RHICmdList, class FSceneRenderer* SceneRenderer, int32 NumColorTextures, FRHITexture** ColorTextures, FRHITexture* DepthTexture, bool bPerformClear)
{
	check(RHICmdList.IsInsideRenderPass());

	uint32 ViewportMinX = X;
	uint32 ViewportMinY = Y;
	float ViewportMinZ = 0.0f;
	uint32 ViewportMaxX = X + BorderSize * 2 + ResolutionX;
	uint32 ViewportMaxY = Y + BorderSize * 2 + ResolutionY;
	float ViewportMaxZ = 1.0f;

	int32 NumClearColors;
	bool bClearColor;
	FLinearColor Colors[2];

	// Translucent shadows use draw call clear
	check(!bTranslucentShadow);

	// Clear depth only.
	bClearColor = false;
	Colors[0] = FLinearColor::White;
	NumClearColors = FMath::Min(1, NumColorTextures);

	if (bPerformClear)
	{
		RHICmdList.SetViewport(
			ViewportMinX,
			ViewportMinY,
			ViewportMinZ,
			ViewportMaxX,
			ViewportMaxY,
			ViewportMaxZ
		);

		DrawClearQuadMRT(RHICmdList, bClearColor, NumClearColors, Colors, true, 1.0f, false, 0);
	}
}

void FProjectedShadowInfo::SetStateForView(FRHICommandList& RHICmdList) const
{
	check(bAllocated);

	RHICmdList.SetViewport(
		X + BorderSize,
		Y + BorderSize,
		0.0f,
		X + BorderSize + ResolutionX,
		Y + BorderSize + ResolutionY,
		1.0f
	);
}

void SetStateForShadowDepth(bool bOnePassPointLightShadow, FMeshPassProcessorRenderState& DrawRenderState)
{
	// Disable color writes
	DrawRenderState.SetBlendState(TStaticBlendState<CW_NONE>::GetRHI());

	if (bOnePassPointLightShadow)
	{
		// Point lights use reverse Z depth maps
		DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI());
	}
	else
	{
		DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<true, CF_LessEqual>::GetRHI());
	}
}

static TAutoConsoleVariable<int32> CVarParallelShadows(
	TEXT("r.ParallelShadows"),
	1,
	TEXT("Toggles parallel shadow rendering. Parallel rendering must be enabled for this to have an effect."),
	ECVF_RenderThreadSafe
);
static TAutoConsoleVariable<int32> CVarParallelShadowsNonWholeScene(
	TEXT("r.ParallelShadowsNonWholeScene"),
	0,
	TEXT("Toggles parallel shadow rendering for non whole-scene shadows. r.ParallelShadows must be enabled for this to have an effect."),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarRHICmdFlushRenderThreadTasksShadowPass(
	TEXT("r.RHICmdFlushRenderThreadTasksShadowPass"),
	0,
	TEXT("Wait for completion of parallel render thread tasks at the end of each shadow pass.  A more granular version of r.RHICmdFlushRenderThreadTasks. If either r.RHICmdFlushRenderThreadTasks or r.RHICmdFlushRenderThreadTasksShadowPass is > 0 we will flush."));

DECLARE_CYCLE_STAT(TEXT("Shadow"), STAT_CLP_Shadow, STATGROUP_ParallelCommandListMarkers);

class FShadowParallelCommandListSet : public FParallelCommandListSet
{
public:
	FShadowParallelCommandListSet(
		FRHICommandListImmediate& InParentCmdList,
		const FViewInfo& InView,
		bool bInCreateSceneContext,
		FProjectedShadowInfo& InProjectedShadowInfo,
		FBeginShadowRenderPassFunction InBeginShadowRenderPass)
		: FParallelCommandListSet(GET_STATID(STAT_CLP_Shadow), InView, InParentCmdList, bInCreateSceneContext)
		, ProjectedShadowInfo(InProjectedShadowInfo)
		, BeginShadowRenderPass(InBeginShadowRenderPass)
	{
		bBalanceCommands = false;
	}

	virtual ~FShadowParallelCommandListSet()
	{
		Dispatch();
	}

	virtual void SetStateOnCommandList(FRHICommandList& CmdList) override
	{
		FParallelCommandListSet::SetStateOnCommandList(CmdList);
		BeginShadowRenderPass(CmdList, false);
		ProjectedShadowInfo.SetStateForView(CmdList);
	}

private:
	FProjectedShadowInfo& ProjectedShadowInfo;
	FBeginShadowRenderPassFunction BeginShadowRenderPass;
	EShadowDepthRenderMode RenderMode;
};

class FCopyShadowMapsCubeGS : public FGlobalShader
{
	DECLARE_SHADER_TYPE(FCopyShadowMapsCubeGS, Global);
public:

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return RHISupportsGeometryShaders(Parameters.Platform) && IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	FCopyShadowMapsCubeGS(const ShaderMetaType::CompiledShaderInitializerType& Initializer) :
		FGlobalShader(Initializer)
	{
	}
	FCopyShadowMapsCubeGS() {}
};

IMPLEMENT_SHADER_TYPE(, FCopyShadowMapsCubeGS, TEXT("/Engine/Private/CopyShadowMaps.usf"), TEXT("CopyCubeDepthGS"), SF_Geometry);

class FCopyShadowMapsCubePS : public FGlobalShader
{
	DECLARE_SHADER_TYPE(FCopyShadowMapsCubePS, Global);
public:

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	FCopyShadowMapsCubePS(const ShaderMetaType::CompiledShaderInitializerType& Initializer) :
		FGlobalShader(Initializer)
	{
		ShadowDepthTexture.Bind(Initializer.ParameterMap, TEXT("ShadowDepthCubeTexture"));
		ShadowDepthSampler.Bind(Initializer.ParameterMap, TEXT("ShadowDepthSampler"));
	}
	FCopyShadowMapsCubePS() {}

	void SetParameters(FRHICommandList& RHICmdList, const FSceneView& View, IPooledRenderTarget* SourceShadowMap)
	{
		FGlobalShader::SetParameters<FViewUniformShaderParameters>(RHICmdList, RHICmdList.GetBoundPixelShader(), View.ViewUniformBuffer);

		SetTextureParameter(RHICmdList, RHICmdList.GetBoundPixelShader(), ShadowDepthTexture, ShadowDepthSampler, TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI(), SourceShadowMap->GetRenderTargetItem().ShaderResourceTexture);
	}

	LAYOUT_FIELD(FShaderResourceParameter, ShadowDepthTexture);
	LAYOUT_FIELD(FShaderResourceParameter, ShadowDepthSampler);
};

IMPLEMENT_SHADER_TYPE(, FCopyShadowMapsCubePS, TEXT("/Engine/Private/CopyShadowMaps.usf"), TEXT("CopyCubeDepthPS"), SF_Pixel);

/** */
class FCopyShadowMaps2DPS : public FGlobalShader
{
	DECLARE_SHADER_TYPE(FCopyShadowMaps2DPS, Global);
public:

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}

	FCopyShadowMaps2DPS(const ShaderMetaType::CompiledShaderInitializerType& Initializer) :
		FGlobalShader(Initializer)
	{
		ShadowDepthTexture.Bind(Initializer.ParameterMap, TEXT("ShadowDepthTexture"));
		ShadowDepthSampler.Bind(Initializer.ParameterMap, TEXT("ShadowDepthSampler"));
	}
	FCopyShadowMaps2DPS() {}

	void SetParameters(FRHICommandList& RHICmdList, const FSceneView& View, IPooledRenderTarget* SourceShadowMap)
	{
		FGlobalShader::SetParameters<FViewUniformShaderParameters>(RHICmdList, RHICmdList.GetBoundPixelShader(), View.ViewUniformBuffer);

		SetTextureParameter(RHICmdList, RHICmdList.GetBoundPixelShader(), ShadowDepthTexture, ShadowDepthSampler, TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI(), SourceShadowMap->GetRenderTargetItem().ShaderResourceTexture);
	}

	LAYOUT_FIELD(FShaderResourceParameter, ShadowDepthTexture);
	LAYOUT_FIELD(FShaderResourceParameter, ShadowDepthSampler);
};

IMPLEMENT_SHADER_TYPE(, FCopyShadowMaps2DPS, TEXT("/Engine/Private/CopyShadowMaps.usf"), TEXT("Copy2DDepthPS"), SF_Pixel);

void FProjectedShadowInfo::CopyCachedShadowMap(FRHICommandList& RHICmdList, const FMeshPassProcessorRenderState& DrawRenderState, FSceneRenderer* SceneRenderer, const FViewInfo& View)
{
	check(CacheMode == SDCM_MovablePrimitivesOnly);
	const FCachedShadowMapData& CachedShadowMapData = SceneRenderer->Scene->CachedShadowMaps.FindChecked(GetLightSceneInfo().Id);

	FGraphicsPipelineStateInitializer GraphicsPSOInit;
	DrawRenderState.ApplyToPSO(GraphicsPSOInit);
	uint32 StencilRef = DrawRenderState.GetStencilRef();

	if (CachedShadowMapData.bCachedShadowMapHasPrimitives && CachedShadowMapData.ShadowMap.IsValid())
	{
		SCOPED_DRAW_EVENT(RHICmdList, CopyCachedShadowMap);

		RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);

		GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
		// No depth tests, so we can replace the clear
		GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<true, CF_Always>::GetRHI();

		extern TGlobalResource<FFilterVertexDeclaration> GFilterVertexDeclaration;

		if (bOnePassPointLightShadow)
		{
			if (RHISupportsGeometryShaders(GShaderPlatformForFeatureLevel[SceneRenderer->FeatureLevel]))
			{
				// Set shaders and texture
				TShaderMapRef<TScreenVSForGS<false>> ScreenVertexShader(View.ShaderMap);
				TShaderMapRef<FCopyShadowMapsCubeGS> GeometryShader(View.ShaderMap);
				TShaderMapRef<FCopyShadowMapsCubePS> PixelShader(View.ShaderMap);

				GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GFilterVertexDeclaration.VertexDeclarationRHI;
				GraphicsPSOInit.BoundShaderState.VertexShaderRHI = ScreenVertexShader.GetVertexShader();
#if PLATFORM_SUPPORTS_GEOMETRY_SHADERS
				GraphicsPSOInit.BoundShaderState.GeometryShaderRHI = GeometryShader.GetGeometryShader();
#endif
				GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
				GraphicsPSOInit.PrimitiveType = PT_TriangleList;

				SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit);
				RHICmdList.SetStencilRef(StencilRef);

				PixelShader->SetParameters(RHICmdList, View, CachedShadowMapData.ShadowMap.DepthTarget.GetReference());

				DrawRectangle(
					RHICmdList,
					0, 0,
					ResolutionX, ResolutionY,
					BorderSize, BorderSize,
					ResolutionX, ResolutionY,
					FIntPoint(ResolutionX, ResolutionY),
					CachedShadowMapData.ShadowMap.GetSize(),
					ScreenVertexShader,
					EDRF_Default);
			}
			else
			{
				check(RHISupportsVertexShaderLayer(GShaderPlatformForFeatureLevel[SceneRenderer->FeatureLevel]));

				// Set shaders and texture
				TShaderMapRef<TScreenVSForGS<true>> ScreenVertexShader(View.ShaderMap);
				TShaderMapRef<FCopyShadowMapsCubePS> PixelShader(View.ShaderMap);

				GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GFilterVertexDeclaration.VertexDeclarationRHI;
				GraphicsPSOInit.BoundShaderState.VertexShaderRHI = ScreenVertexShader.GetVertexShader();
				GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
				GraphicsPSOInit.PrimitiveType = PT_TriangleList;

				SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit);
				RHICmdList.SetStencilRef(StencilRef);

				PixelShader->SetParameters(RHICmdList, View, CachedShadowMapData.ShadowMap.DepthTarget.GetReference());

				DrawRectangle(
					RHICmdList,
					0, 0,
					ResolutionX, ResolutionY,
					BorderSize, BorderSize,
					ResolutionX, ResolutionY,
					FIntPoint(ResolutionX, ResolutionY),
					CachedShadowMapData.ShadowMap.GetSize(),
					ScreenVertexShader,
					EDRF_Default,
					6);
			}
		}
		else
		{
			// Set shaders and texture
			TShaderMapRef<FScreenVS> ScreenVertexShader(View.ShaderMap);
			TShaderMapRef<FCopyShadowMaps2DPS> PixelShader(View.ShaderMap);

			GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GFilterVertexDeclaration.VertexDeclarationRHI;
			GraphicsPSOInit.BoundShaderState.VertexShaderRHI = ScreenVertexShader.GetVertexShader();
			GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
			GraphicsPSOInit.PrimitiveType = PT_TriangleList;

			SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit);
			RHICmdList.SetStencilRef(StencilRef);

			PixelShader->SetParameters(RHICmdList, View, CachedShadowMapData.ShadowMap.DepthTarget.GetReference());

			DrawRectangle(
				RHICmdList,
				0, 0,
				ResolutionX, ResolutionY,
				BorderSize, BorderSize,
				ResolutionX, ResolutionY,
				FIntPoint(ResolutionX, ResolutionY),
				CachedShadowMapData.ShadowMap.GetSize(),
				ScreenVertexShader,
				EDRF_Default);
		}
	}
}


void FProjectedShadowInfo::SetupShadowUniformBuffers(FRHICommandListImmediate& RHICmdList, FScene* Scene)
{
	const ERHIFeatureLevel::Type FeatureLevel = ShadowDepthView->FeatureLevel;
	if (FSceneInterface::GetShadingPath(FeatureLevel) == EShadingPath::Deferred)
	{
		FShadowDepthPassUniformParameters ShadowDepthPassParameters;
		SetupShadowDepthPassUniformBuffer(this, RHICmdList, *ShadowDepthView, ShadowDepthPassParameters);

		if (IsWholeSceneDirectionalShadow())
		{
			check(GetShadowDepthType() == CSMShadowDepthType);
			Scene->UniformBuffers.CSMShadowDepthPassUniformBuffer.UpdateUniformBufferImmediate(ShadowDepthPassParameters);
		}

		ShadowDepthPassUniformBuffer.UpdateUniformBufferImmediate(ShadowDepthPassParameters);

		if (DependentView)
		{
			extern TSet<IPersistentViewUniformBufferExtension*> PersistentViewUniformBufferExtensions;

			for (IPersistentViewUniformBufferExtension* Extension : PersistentViewUniformBufferExtensions)
			{
				Extension->BeginRenderView(DependentView);
			}
		}
	}
	
	// This needs to be done for both mobile and deferred
	UploadDynamicPrimitiveShaderDataForView(RHICmdList, *Scene, *ShadowDepthView);
}

void FProjectedShadowInfo::TransitionCachedShadowmap(FRHICommandListImmediate& RHICmdList, FScene* Scene)
{
	if (CacheMode == SDCM_MovablePrimitivesOnly)
	{
		const FCachedShadowMapData& CachedShadowMapData = Scene->CachedShadowMaps.FindChecked(GetLightSceneInfo().Id);
		if (CachedShadowMapData.bCachedShadowMapHasPrimitives && CachedShadowMapData.ShadowMap.IsValid())
		{
			RHICmdList.Transition(FRHITransitionInfo(CachedShadowMapData.ShadowMap.DepthTarget->GetRenderTargetItem().ShaderResourceTexture, ERHIAccess::Unknown, ERHIAccess::SRVGraphics));
		}
	}
}


void FProjectedShadowInfo::RenderDepthInner(FRHICommandListImmediate& RHICmdList, FSceneRenderer* SceneRenderer, FBeginShadowRenderPassFunction BeginShadowRenderPass, bool bDoParallelDispatch)
{
	const ERHIFeatureLevel::Type FeatureLevel = ShadowDepthView->FeatureLevel;
	FRHIUniformBuffer* PassUniformBuffer = ShadowDepthPassUniformBuffer;

	const bool bIsWholeSceneDirectionalShadow = IsWholeSceneDirectionalShadow();

	if (bIsWholeSceneDirectionalShadow)
	{
		// CSM shadow depth cached mesh draw commands are all referencing the same view uniform buffer.  We need to update it before rendering each cascade.
		ShadowDepthView->ViewUniformBuffer.UpdateUniformBufferImmediate(*ShadowDepthView->CachedViewUniformShaderParameters);
		
		if (DependentView)
		{
			extern TSet<IPersistentViewUniformBufferExtension*> PersistentViewUniformBufferExtensions;

			for (IPersistentViewUniformBufferExtension* Extension : PersistentViewUniformBufferExtensions)
			{
				Extension->BeginRenderView(DependentView);
			}
		}
	}

	if (FSceneInterface::GetShadingPath(FeatureLevel) == EShadingPath::Mobile)
	{
		FMobileShadowDepthPassUniformParameters ShadowDepthPassParameters;
		SetupShadowDepthPassUniformBuffer(this, RHICmdList, *ShadowDepthView, ShadowDepthPassParameters);
		SceneRenderer->Scene->UniformBuffers.MobileCSMShadowDepthPassUniformBuffer.UpdateUniformBufferImmediate(ShadowDepthPassParameters);
		MobileShadowDepthPassUniformBuffer.UpdateUniformBufferImmediate(ShadowDepthPassParameters);
		PassUniformBuffer = SceneRenderer->Scene->UniformBuffers.MobileCSMShadowDepthPassUniformBuffer;
	}

	FMeshPassProcessorRenderState DrawRenderState(*ShadowDepthView, PassUniformBuffer);
	SetStateForShadowDepth(bOnePassPointLightShadow, DrawRenderState);
	SetStateForView(RHICmdList);

	if (CacheMode == SDCM_MovablePrimitivesOnly)
	{
		// In parallel mode we will not have a renderpass active at this point.
		if (bDoParallelDispatch)
		{
			BeginShadowRenderPass(RHICmdList, false);
		}

		// Copy in depths of static primitives before we render movable primitives
		CopyCachedShadowMap(RHICmdList, DrawRenderState, SceneRenderer, *ShadowDepthView);

		if (bDoParallelDispatch)
		{
			RHICmdList.EndRenderPass();
		}
	}

	if (bDoParallelDispatch)
	{
		check(IsInRenderingThread());
		// Parallel encoding requires its own renderpass.
		check(RHICmdList.IsOutsideRenderPass());

		// parallel version
		bool bFlush = CVarRHICmdFlushRenderThreadTasksShadowPass.GetValueOnRenderThread() > 0
			|| CVarRHICmdFlushRenderThreadTasks.GetValueOnRenderThread() > 0;
		FScopedCommandListWaitForTasks Flusher(bFlush);

		// Dispatch commands
		{
			FShadowParallelCommandListSet ParallelCommandListSet(RHICmdList, *ShadowDepthView, !bFlush, *this, BeginShadowRenderPass);

			ShadowDepthPass.DispatchDraw(&ParallelCommandListSet, RHICmdList);
		}

		// Renderpass must be closed once we get here.
		check(RHICmdList.IsOutsideRenderPass());
	}
	else
	{
		// We must have already opened the renderpass by the time we get here.
		check(RHICmdList.IsInsideRenderPass());

		ShadowDepthPass.DispatchDraw(nullptr, RHICmdList);

		// Renderpass must still be open when we reach here
		check(RHICmdList.IsInsideRenderPass());
	}
}

void FProjectedShadowInfo::ModifyViewForShadow(FRHICommandList& RHICmdList, FViewInfo* FoundView) const
{
	FIntRect OriginalViewRect = FoundView->ViewRect;
	FoundView->ViewRect = GetViewRectForView();

	//FoundView->ViewMatrices.HackRemoveTemporalAAProjectionJitter();

	if (CascadeSettings.bFarShadowCascade)
	{
		(int32&)FoundView->DrawDynamicFlags |= (int32)EDrawDynamicFlags::FarShadowCascade;
	}

	// Don't do material texture mip biasing in shadow maps.
	FoundView->MaterialTextureMipBias = 0;

	FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(RHICmdList);
	FoundView->CachedViewUniformShaderParameters = MakeUnique<FViewUniformShaderParameters>();

	// Override the view matrix so that billboarding primitives will be aligned to the light
	FoundView->ViewMatrices.HackOverrideMatrixForShadows(TranslatedWorldToView, ViewToClip, -PreShadowTranslation);
	FoundView->PrevViewInfo.ViewMatrices.HackOverrideMatrixForShadows(TranslatedWorldToView, ViewToClip, -PreShadowTranslation);

	FBox VolumeBounds[TVC_MAX];
	FoundView->SetupUniformBufferParameters(
		SceneContext,
		VolumeBounds,
		TVC_MAX,
		*FoundView->CachedViewUniformShaderParameters);

	if (IsWholeSceneDirectionalShadow())
	{
		FScene* Scene = (FScene*)FoundView->Family->Scene;
		FoundView->ViewUniformBuffer = Scene->UniformBuffers.CSMShadowDepthViewUniformBuffer;
	}
	else
	{
		FoundView->ViewUniformBuffer = TUniformBufferRef<FViewUniformShaderParameters>::CreateUniformBufferImmediate(*FoundView->CachedViewUniformShaderParameters, UniformBuffer_SingleFrame);
	}

	// we are going to set this back now because we only want the correct view rect for the uniform buffer. For LOD calculations, we want the rendering viewrect and proj matrix.
	FoundView->ViewRect = OriginalViewRect;

	extern int32 GPreshadowsForceLowestLOD;

	if (bPreShadow && GPreshadowsForceLowestLOD)
	{
		(int32&)FoundView->DrawDynamicFlags |= EDrawDynamicFlags::ForceLowestLOD;
	}
}

FViewInfo* FProjectedShadowInfo::FindViewForShadow(FSceneRenderer* SceneRenderer) const
{
	// Choose an arbitrary view where this shadow's subject is relevant.
	FViewInfo* FoundView = NULL;
	for (int32 ViewIndex = 0; ViewIndex < SceneRenderer->Views.Num(); ViewIndex++)
	{
		FViewInfo* CheckView = &SceneRenderer->Views[ViewIndex];
		const FVisibleLightViewInfo& VisibleLightViewInfo = CheckView->VisibleLightInfos[LightSceneInfo->Id];
		FPrimitiveViewRelevance ViewRel = VisibleLightViewInfo.ProjectedShadowViewRelevanceMap[ShadowId];
		if (ViewRel.bShadowRelevance)
		{
			FoundView = CheckView;
			break;
		}
	}
	check(FoundView);
	return FoundView;
}

void FProjectedShadowInfo::RenderDepth(FRHICommandListImmediate& RHICmdList, FSceneRenderer* SceneRenderer, FBeginShadowRenderPassFunction BeginShadowRenderPass, bool bDoParallelDispatch)
{
#if WANTS_DRAW_MESH_EVENTS
	FString EventName;

	if (GetEmitDrawEvents())
	{
		GetShadowTypeNameForDrawEvent(EventName);
		EventName += FString(TEXT(" ")) + FString::FromInt(ResolutionX) + TEXT("x") + FString::FromInt(ResolutionY);
	}

	SCOPED_DRAW_EVENTF(RHICmdList, EventShadowDepthActor, *EventName);
#endif

	CONDITIONAL_SCOPE_CYCLE_COUNTER(STAT_RenderWholeSceneShadowDepthsTime, bWholeSceneShadow);
	CONDITIONAL_SCOPE_CYCLE_COUNTER(STAT_RenderPerObjectShadowDepthsTime, !bWholeSceneShadow);
	QUICK_SCOPE_CYCLE_COUNTER(STAT_RenderShadowDepth);

	RenderDepthInner(RHICmdList, SceneRenderer, BeginShadowRenderPass, bDoParallelDispatch);
}

void FProjectedShadowInfo::SetupShadowDepthView(FRHICommandListImmediate& RHICmdList, FSceneRenderer* SceneRenderer)
{
	FViewInfo* FoundView = FindViewForShadow(SceneRenderer);
	check(FoundView && IsInRenderingThread());
	FViewInfo* DepthPassView = FoundView->CreateSnapshot();
	ModifyViewForShadow(RHICmdList, DepthPassView);
	ShadowDepthView = DepthPassView;
}

void FProjectedShadowInfo::GetShadowTypeNameForDrawEvent(FString& TypeName) const
{
	const FName ParentName = ParentSceneInfo ? ParentSceneInfo->Proxy->GetOwnerName() : NAME_None;

	if (bWholeSceneShadow)
	{
		if (CascadeSettings.ShadowSplitIndex >= 0)
		{
			TypeName = FString(TEXT("WholeScene split")) + FString::FromInt(CascadeSettings.ShadowSplitIndex);
		}
		else
		{
			if (CacheMode == SDCM_MovablePrimitivesOnly)
			{
				TypeName = FString(TEXT("WholeScene MovablePrimitives"));
			}
			else if (CacheMode == SDCM_StaticPrimitivesOnly)
			{
				TypeName = FString(TEXT("WholeScene StaticPrimitives"));
			}
			else
			{
				TypeName = FString(TEXT("WholeScene"));
			}
		}
	}
	else if (bPreShadow)
	{
		TypeName = FString(TEXT("PreShadow ")) + ParentName.ToString();
	}
	else
	{
		TypeName = FString(TEXT("PerObject ")) + ParentName.ToString();
	}
}

#if WITH_MGPU
FRHIGPUMask FSceneRenderer::GetGPUMaskForShadow(FProjectedShadowInfo* ProjectedShadowInfo) const
{
	// Preshadows that are going to be cached this frame should render on all GPUs.
	if (ProjectedShadowInfo->bPreShadow)
	{
		// Multi-GPU support : Updating on all GPUs may be inefficient for AFR. Work is
		// wasted for any shadows that re-cache on consecutive frames.
		return !ProjectedShadowInfo->bDepthsCached && ProjectedShadowInfo->bAllocatedInPreshadowCache ? FRHIGPUMask::All() : AllViewsGPUMask;
	}
	// SDCM_StaticPrimitivesOnly shadows don't update every frame so we need to render
	// their depths on all possible GPUs.
	else if (ProjectedShadowInfo->CacheMode == SDCM_StaticPrimitivesOnly)
	{
		// Cached whole scene shadows shouldn't be view dependent.
		checkSlow(ProjectedShadowInfo->DependentView == nullptr);

		// Multi-GPU support : Updating on all GPUs may be inefficient for AFR. Work is
		// wasted for any shadows that re-cache on consecutive frames.
		return FRHIGPUMask::All();
	}
	else
	{
		// View dependent shadows only need to render depths on their view's GPUs.
		if (ProjectedShadowInfo->DependentView != nullptr)
		{
			return ProjectedShadowInfo->DependentView->GPUMask;
		}
		else
		{
			return AllViewsGPUMask;
		}
	}
}
#endif // WITH_MGPU


static void RenderShadowDepthAtlasNanite(
	FRDGBuilder& GraphBuilder,
	const FScene& Scene,
	const FSortedShadowMapAtlas& ShadowMapAtlas)
{
	const FIntPoint AtlasSize = ShadowMapAtlas.RenderTargets.DepthTarget->GetDesc().Extent;

	bool bWantsNearClip = false;
	bool bWantsNoNearClip = false;
	TArray<Nanite::FPackedView, SceneRenderingAllocator> PackedViews;
	TArray<Nanite::FPackedView, SceneRenderingAllocator> PackedViewsNoNearClip;
	TArray<FProjectedShadowInfo*, SceneRenderingAllocator> ShadowsToEmit;
	for (int32 ShadowIndex = 0; ShadowIndex < ShadowMapAtlas.Shadows.Num(); ShadowIndex++)
	{
		FProjectedShadowInfo* ProjectedShadowInfo = ShadowMapAtlas.Shadows[ShadowIndex];

		// TODO: We avoid rendering Nanite geometry into both movable AND static cached shadows, but has a side effect
		// that if there is *only* a movable cached shadow map (and not static), it won't rendering anything.
		// Logic around Nanite and the cached shadows is fuzzy in a bunch of places and the whole thing needs some rethinking
		// so leaving this like this for now as it is unlikely to happen in realistic scenes.
		if (!ProjectedShadowInfo->bNaniteGeometry ||
			ProjectedShadowInfo->CacheMode == SDCM_MovablePrimitivesOnly)
		{
			continue;
		}

		const FIntRect AtlasViewRect = ProjectedShadowInfo->GetViewRectForView();

		Nanite::FPackedViewParams Initializer;
		Initializer.ViewMatrices = ProjectedShadowInfo->ShadowDepthView->ViewMatrices;
		Initializer.PrevViewMatrices = Initializer.ViewMatrices;
		Initializer.ViewRect = AtlasViewRect;
		Initializer.RasterContextSize = AtlasSize;
		Initializer.LODScaleFactor = ComputeNaniteShadowsLODScaleFactor();

		// Orthographic shadow projections want depth clamping rather than clipping
		if (ProjectedShadowInfo->ShouldClampToNearPlane())
		{
			PackedViewsNoNearClip.Add(Nanite::CreatePackedView(Initializer));
		}
		else
		{
			PackedViews.Add(Nanite::CreatePackedView(Initializer));
		}


		ShadowsToEmit.Add(ProjectedShadowInfo);
	}

	if (PackedViews.Num() > 0 || PackedViewsNoNearClip.Num() > 0)
	{
		RDG_EVENT_SCOPE(GraphBuilder, "Nanite Shadows");

		// Need separate passes for near clip on/off currently
		const bool bSupportsMultiplePasses = (PackedViews.Num() > 0 && PackedViewsNoNearClip.Num() > 0);
		const bool bPrimaryContext = false;

		// NOTE: Rendering into an atlas like this is not going to work properly with HZB, but we are not currently using HZB here.
		// It might be worthwhile going through the virtual SM rendering path even for "dense" cases even just for proper handling of all the details.
		FIntRect FullAtlasViewRect(FIntPoint(0, 0), AtlasSize);
		const bool bUpdateStreaming = CVarNaniteShadowsUpdateStreaming.GetValueOnRenderThread() != 0;
		Nanite::FCullingContext CullingContext = Nanite::InitCullingContext(GraphBuilder, Scene, nullptr, FullAtlasViewRect, true, bUpdateStreaming, bSupportsMultiplePasses, false, bPrimaryContext);
		Nanite::FRasterContext RasterContext = Nanite::InitRasterContext(GraphBuilder, AtlasSize, Nanite::EOutputBufferMode::DepthOnly);

		if (PackedViews.Num() > 0)
		{
			Nanite::FRasterState RasterState;
			RasterState.bNearClip = true;

			Nanite::CullRasterize(
				GraphBuilder,
				Scene,
				PackedViews,
				CullingContext,
				RasterContext,
				RasterState,
				nullptr,	// InstanceDraws
				false		// bExtractStats TODO
			);
		}

		if (PackedViewsNoNearClip.Num() > 0)
		{
			Nanite::FRasterState RasterState;
			RasterState.bNearClip = false;

			Nanite::CullRasterize(
				GraphBuilder,
				Scene,
				PackedViewsNoNearClip,
				CullingContext,
				RasterContext,
				RasterState,
				nullptr,	// InstanceDraws
				false		// bExtractStats TODO
			);
		}

		FRDGTextureRef ShadowMap = GraphBuilder.RegisterExternalTexture(ShadowMapAtlas.RenderTargets.DepthTarget, TEXT("DepthBuffer"));
		for (FProjectedShadowInfo* ProjectedShadowInfo : ShadowsToEmit)
		{
			const FIntRect AtlasViewRect = ProjectedShadowInfo->GetViewRectForView();

			Nanite::EmitShadowMap(
				GraphBuilder,
				RasterContext,
				ShadowMap,
				AtlasViewRect,
				AtlasViewRect.Min,
				ProjectedShadowInfo->ShadowDepthView->ViewMatrices.GetProjectionMatrix(),
				ProjectedShadowInfo->GetShaderDepthBias(),
				ProjectedShadowInfo->bDirectionalLight
			);
		}
	}
}

class FCopyToCompleteShadowMapPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCopyToCompleteShadowMapPS);
	SHADER_USE_PARAMETER_STRUCT(FCopyToCompleteShadowMapPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FVector4, SourceScaleOffset)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, SourceBuffer)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};
IMPLEMENT_GLOBAL_SHADER(FCopyToCompleteShadowMapPS, "/Engine/Private/VirtualShadowMaps/CopyCompleteShadows.usf", "CopyToCompleteShadowMapPS", SF_Pixel);

static void CopyToCompleteShadowMap(
	FRDGBuilder& GraphBuilder,
	const FRDGTextureRef SourceBuffer,
	const FRDGTextureRef DestBuffer,
	const FIntRect& SourceRect,
	const FIntRect& DestRect,
	ERenderTargetLoadAction LoadAction
	)
{
	FVector4 SourceScaleOffset;
	SourceScaleOffset.X = static_cast<float>(SourceRect.Width() ) / DestRect.Width();
	SourceScaleOffset.Y = static_cast<float>(SourceRect.Height()) / DestRect.Height();
	SourceScaleOffset.Z = SourceRect.Min.X - (SourceScaleOffset.X * DestRect.Min.X);
	SourceScaleOffset.W = SourceRect.Min.Y - (SourceScaleOffset.Y * DestRect.Min.Y);

	auto* PassParameters = GraphBuilder.AllocParameters<FCopyToCompleteShadowMapPS::FParameters>();
	PassParameters->SourceBuffer = SourceBuffer;
	PassParameters->SourceScaleOffset = SourceScaleOffset;
	PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(DestBuffer, LoadAction, FExclusiveDepthStencil::DepthWrite_StencilNop);

	auto ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	auto PixelShader = ShaderMap->GetShader<FCopyToCompleteShadowMapPS>();

	FPixelShaderUtils::AddFullscreenPass(
		GraphBuilder,
		ShaderMap,
		RDG_EVENT_NAME("CopyToCompleteShadowMap"),
		PixelShader,
		PassParameters,
		DestRect,
		nullptr,
		nullptr,
		TStaticDepthStencilState<true, CF_Always>::GetRHI()
	);
}

void FSceneRenderer::RenderShadowDepthMapAtlases(FRDGBuilder& GraphBuilder)
{
	// Perform setup work on all GPUs in case any cached shadows are being updated this
	// frame. We revert to the AllViewsGPUMask for uncached shadows.
	RDG_GPU_MASK_SCOPE(GraphBuilder, FRHIGPUMask::All());

	FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(GraphBuilder.RHICmdList);

	bool bCanUseParallelDispatch = GraphBuilder.RHICmdList.IsImmediate() &&  // translucent shadows are draw on the render thread, using a recursive cmdlist (which is not immediate)
		GRHICommandList.UseParallelAlgorithms() && CVarParallelShadows.GetValueOnRenderThread();

	const bool bNaniteEnabled = 
		UseNanite(ShaderPlatform) &&
		ViewFamily.EngineShowFlags.NaniteMeshes &&
		CVarNaniteShadows.GetValueOnRenderThread() != 0;

	for (int32 AtlasIndex = 0; AtlasIndex < SortedShadowsForShadowDepthPass.ShadowMapAtlases.Num(); AtlasIndex++)
	{
		const FSortedShadowMapAtlas& ShadowMapAtlas = SortedShadowsForShadowDepthPass.ShadowMapAtlases[AtlasIndex];
		FSceneRenderTargetItem& RenderTarget = ShadowMapAtlas.RenderTargets.DepthTarget->GetRenderTargetItem();
		FIntPoint AtlasSize = ShadowMapAtlas.RenderTargets.DepthTarget->GetDesc().Extent;

		AddUntrackedAccessPass(GraphBuilder, [this, &ShadowMapAtlas , &RenderTarget, &SceneContext, AtlasIndex, AtlasSize, bCanUseParallelDispatch](FRHICommandListImmediate& RHICmdList)
		{
			GVisualizeTexture.SetCheckPoint(RHICmdList, ShadowMapAtlas.RenderTargets.DepthTarget.GetReference());

			SCOPED_DRAW_EVENTF(RHICmdList, EventShadowDepths, TEXT("Atlas%u %ux%u"), AtlasIndex, AtlasSize.X, AtlasSize.Y);

			auto BeginShadowRenderPass = [this, &RenderTarget, &SceneContext](FRHICommandList& InRHICmdList, bool bPerformClear)
			{
				check(RenderTarget.TargetableTexture->GetDepthClearValue() == 1.0f);

				ERenderTargetLoadAction DepthLoadAction = bPerformClear ? ERenderTargetLoadAction::EClear : ERenderTargetLoadAction::ELoad;

				FRHIRenderPassInfo RPInfo(RenderTarget.TargetableTexture, MakeDepthStencilTargetActions(MakeRenderTargetActions(DepthLoadAction, ERenderTargetStoreAction::EStore), ERenderTargetActions::DontLoad_DontStore), nullptr, FExclusiveDepthStencil::DepthWrite_StencilNop);

				if (!GSupportsDepthRenderTargetWithoutColorRenderTarget)
				{
					RPInfo.ColorRenderTargets[0].Action = ERenderTargetActions::DontLoad_DontStore;
					RPInfo.ColorRenderTargets[0].RenderTarget = SceneContext.GetOptionalShadowDepthColorSurface(InRHICmdList, RPInfo.DepthStencilRenderTarget.DepthStencilTarget->GetTexture2D()->GetSizeX(), RPInfo.DepthStencilRenderTarget.DepthStencilTarget->GetTexture2D()->GetSizeY());
					InRHICmdList.Transition(FRHITransitionInfo(RPInfo.ColorRenderTargets[0].RenderTarget, ERHIAccess::Unknown, ERHIAccess::RTV));
				}
				InRHICmdList.Transition(FRHITransitionInfo(RPInfo.DepthStencilRenderTarget.DepthStencilTarget, ERHIAccess::Unknown, ERHIAccess::DSVWrite));
				InRHICmdList.BeginRenderPass(RPInfo, TEXT("ShadowMapAtlases"));
			};

			TArray<FProjectedShadowInfo*, SceneRenderingAllocator> ParallelShadowPasses;
			TArray<FProjectedShadowInfo*, SceneRenderingAllocator> SerialShadowPasses;

			// Gather our passes here to minimize switching render passes
			for (int32 ShadowIndex = 0; ShadowIndex < ShadowMapAtlas.Shadows.Num(); ShadowIndex++)
			{
				FProjectedShadowInfo* ProjectedShadowInfo = ShadowMapAtlas.Shadows[ShadowIndex];

				const bool bDoParallelDispatch = bCanUseParallelDispatch &&
					(ProjectedShadowInfo->IsWholeSceneDirectionalShadow() || CVarParallelShadowsNonWholeScene.GetValueOnRenderThread());

				if (bDoParallelDispatch)
				{
					ParallelShadowPasses.Add(ProjectedShadowInfo);
				}
				else
				{
					SerialShadowPasses.Add(ProjectedShadowInfo);
				}
			}

			FLightSceneProxy* CurrentLightForDrawEvent = NULL;

		#if WANTS_DRAW_MESH_EVENTS
			FDrawEvent LightEvent;
		#endif

			if (ParallelShadowPasses.Num() > 0)
			{
				{
					// Clear before going wide.
					SCOPED_DRAW_EVENT(RHICmdList, SetShadowRTsAndClear);
					BeginShadowRenderPass(RHICmdList, true);
					RHICmdList.EndRenderPass();
				}

				for (int32 ShadowIndex = 0; ShadowIndex < ParallelShadowPasses.Num(); ShadowIndex++)
				{
					FProjectedShadowInfo* ProjectedShadowInfo = ParallelShadowPasses[ShadowIndex];
					SCOPED_GPU_MASK(RHICmdList, GetGPUMaskForShadow(ProjectedShadowInfo));

				#if WANTS_DRAW_MESH_EVENTS
					if (!CurrentLightForDrawEvent || ProjectedShadowInfo->GetLightSceneInfo().Proxy != CurrentLightForDrawEvent)
					{
						if (CurrentLightForDrawEvent)
						{
							SCOPED_GPU_MASK(RHICmdList, FRHIGPUMask::All());
							STOP_DRAW_EVENT(LightEvent);
						}

						CurrentLightForDrawEvent = ProjectedShadowInfo->GetLightSceneInfo().Proxy;
						FString LightNameWithLevel;
						GetLightNameForDrawEvent(CurrentLightForDrawEvent, LightNameWithLevel);

						SCOPED_GPU_MASK(RHICmdList, FRHIGPUMask::All());
						BEGIN_DRAW_EVENTF(
							RHICmdList,
							LightNameEvent,
							LightEvent,
							*LightNameWithLevel);
					}
				#endif

					ProjectedShadowInfo->SetupShadowUniformBuffers(RHICmdList, Scene);
					ProjectedShadowInfo->TransitionCachedShadowmap(RHICmdList, Scene);
					ProjectedShadowInfo->RenderDepth(RHICmdList, this, BeginShadowRenderPass, true);
				}
			}

		#if WANTS_DRAW_MESH_EVENTS
			if (CurrentLightForDrawEvent)
			{
				SCOPED_GPU_MASK(RHICmdList, FRHIGPUMask::All());
				STOP_DRAW_EVENT(LightEvent);
			}
		#endif

			CurrentLightForDrawEvent = nullptr;

			if (SerialShadowPasses.Num() > 0)
			{
				bool bForceSingleRenderPass = CVarShadowForceSerialSingleRenderPass.GetValueOnAnyThread() != 0;
				if (bForceSingleRenderPass)
				{
					SCOPED_GPU_MASK(RHICmdList, AllViewsGPUMask);
					BeginShadowRenderPass(RHICmdList, true);
				}

				for (int32 ShadowIndex = 0; ShadowIndex < SerialShadowPasses.Num(); ShadowIndex++)
				{
					FProjectedShadowInfo* ProjectedShadowInfo = SerialShadowPasses[ShadowIndex];
					SCOPED_GPU_MASK(RHICmdList, GetGPUMaskForShadow(ProjectedShadowInfo));

				#if WANTS_DRAW_MESH_EVENTS
					if (!CurrentLightForDrawEvent || ProjectedShadowInfo->GetLightSceneInfo().Proxy != CurrentLightForDrawEvent)
					{
						if (CurrentLightForDrawEvent)
						{
							SCOPED_GPU_MASK(RHICmdList, FRHIGPUMask::All());
							STOP_DRAW_EVENT(LightEvent);
						}

						CurrentLightForDrawEvent = ProjectedShadowInfo->GetLightSceneInfo().Proxy;
						FString LightNameWithLevel;
						GetLightNameForDrawEvent(CurrentLightForDrawEvent, LightNameWithLevel);

						SCOPED_GPU_MASK(RHICmdList, FRHIGPUMask::All());
						BEGIN_DRAW_EVENTF(
							RHICmdList,
							LightNameEvent,
							LightEvent,
							*LightNameWithLevel);
					}
				#endif

					ProjectedShadowInfo->SetupShadowUniformBuffers(RHICmdList, Scene);
					ProjectedShadowInfo->TransitionCachedShadowmap(RHICmdList, Scene);
	#if WITH_MGPU
					// In case the first shadow is view-dependent, ensure we do the clear on all GPUs.
					FRHIGPUMask GPUMaskForRenderPass = RHICmdList.GetGPUMask();
					if (ShadowIndex == 0)
					{
						// This ensures that we don't downgrade the GPU mask if the first shadow is a
						// cached whole scene shadow.
						GPUMaskForRenderPass |= AllViewsGPUMask;
					}
	#endif
					if (!bForceSingleRenderPass)
					{
						SCOPED_GPU_MASK(RHICmdList, GPUMaskForRenderPass);
						BeginShadowRenderPass(RHICmdList, ShadowIndex == 0);
					}

					ProjectedShadowInfo->RenderDepth(RHICmdList, this, BeginShadowRenderPass, false);

					if (!bForceSingleRenderPass)
					{
						RHICmdList.EndRenderPass();
					}
				}
				if (bForceSingleRenderPass)
				{
					SCOPED_GPU_MASK(RHICmdList, AllViewsGPUMask);
					RHICmdList.EndRenderPass();
				}
			}

			if (CurrentLightForDrawEvent)
			{
				SCOPED_GPU_MASK(RHICmdList, FRHIGPUMask::All());
				STOP_DRAW_EVENT(LightEvent);
				CurrentLightForDrawEvent = NULL;
			}
		});

		if (bNaniteEnabled)
		{
			RenderShadowDepthAtlasNanite(GraphBuilder, *Scene, ShadowMapAtlas);
		}

		AddUntrackedAccessPass(GraphBuilder, [&RenderTarget](FRHICommandList& RHICmdList)
		{
			RHICmdList.Transition(FRHITransitionInfo(RenderTarget.TargetableTexture, ERHIAccess::Unknown, ERHIAccess::SRVMask));
		});
	}

	// Copy/resample shadow maps into "complete" shadow maps and add Nanite geometry
	if (SortedShadowsForShadowDepthPass.CompleteShadowMapAtlases.Num() > 0)
	{
		for (const FSortedShadowMapAtlas& ShadowMapAtlas : SortedShadowsForShadowDepthPass.CompleteShadowMapAtlases)
		{
			FSceneRenderTargetItem& RenderTarget = ShadowMapAtlas.RenderTargets.DepthTarget->GetRenderTargetItem();
			FIntPoint AtlasSize = ShadowMapAtlas.RenderTargets.DepthTarget->GetDesc().Extent;
		
			FRDGTextureRef DestShadowMap = GraphBuilder.RegisterExternalTexture(ShadowMapAtlas.RenderTargets.DepthTarget, TEXT("DepthBuffer"));

			RDG_EVENT_SCOPE(GraphBuilder, "Complete Atlas %ux%u", AtlasSize.X, AtlasSize.Y);

			bool bCleared = false;
			for (int32 ShadowIndex = 0; ShadowIndex < ShadowMapAtlas.Shadows.Num(); ShadowIndex++)
			{
				FProjectedShadowInfo* ProjectedShadowInfo = ShadowMapAtlas.Shadows[ShadowIndex];
				FProjectedShadowInfo* SourceShadowInfo = ProjectedShadowInfo->CompleteShadowMapCopySource;

				check(SourceShadowInfo);
				if (SourceShadowInfo && SourceShadowInfo->RenderTargets.DepthTarget != nullptr)
				{
					FRDGTextureRef SourceShadowMap = GraphBuilder.RegisterExternalTexture(SourceShadowInfo->RenderTargets.DepthTarget, TEXT("SourceDepthBuffer"));
					const FIntRect SourceRect = SourceShadowInfo->GetViewRectForView();
					const FIntRect DestRect = ProjectedShadowInfo->GetViewRectForView();
					CopyToCompleteShadowMap(GraphBuilder, SourceShadowMap, DestShadowMap, SourceRect, DestRect, bCleared ? ERenderTargetLoadAction::ELoad : ERenderTargetLoadAction::EClear);
					bCleared = true;
				}
			}

			if (!bCleared)
			{
				// If nothing cleared it, ensure it's done before nanite rendering at least
				AddClearDepthStencilPass(GraphBuilder, DestShadowMap, true, 1.0f, false, 0);
				bCleared = true;
			}

			if (bNaniteEnabled)
			{
				RenderShadowDepthAtlasNanite(GraphBuilder, *Scene, ShadowMapAtlas);
			}
		}
	}
}

void FSceneRenderer::RenderShadowDepthMaps(FRDGBuilder& GraphBuilder)
{
	CSV_SCOPED_TIMING_STAT_EXCLUSIVE(RenderShadows);
	SCOPED_NAMED_EVENT(FSceneRenderer_RenderShadowDepthMaps, FColor::Emerald);
	FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(GraphBuilder.RHICmdList);

	RDG_EVENT_SCOPE(GraphBuilder, "ShadowDepths");
	RDG_GPU_STAT_SCOPE(GraphBuilder, ShadowDepths);

	// Perform setup work on all GPUs in case any cached shadows are being updated this
	// frame. We revert to the AllViewsGPUMask for uncached shadows.
#if WITH_MGPU
	ensure(GraphBuilder.RHICmdList.GetGPUMask() == AllViewsGPUMask);
#endif
	RDG_GPU_MASK_SCOPE(GraphBuilder, FRHIGPUMask::All());

	const bool bHasVSMShadows = SortedShadowsForShadowDepthPass.VirtualShadowMapShadows.Num() > 0;
	const bool bHasVSMClipMaps = SortedShadowsForShadowDepthPass.VirtualShadowMapClipmaps.Num() > 0;
	const bool bNaniteEnabled = UseNanite(ShaderPlatform) && ViewFamily.EngineShowFlags.NaniteMeshes;

	if (bNaniteEnabled && (bHasVSMShadows || bHasVSMClipMaps))
	{
		bool bUseHZB = (CVarNaniteShadowsUseHZB.GetValueOnRenderThread() != 0);

		if (bUseHZB)
		{
			VirtualShadowMapArray.HZBPhysical	= Scene->VirtualShadowMapArrayCacheManager->HZBPhysical;
			VirtualShadowMapArray.HZBPageTable	= Scene->VirtualShadowMapArrayCacheManager->HZBPageTable;
		}

		FVirtualShadowMapArrayCacheManager *CacheManager = Scene->VirtualShadowMapArrayCacheManager;
		const uint32 CachedFrameNumber = CacheManager->HZBFrameNumber;
		const uint32 CurrentFrameNumber = ++CacheManager->HZBFrameNumber;
		
		{
			RDG_EVENT_SCOPE(GraphBuilder, "Render Virtual Shadow Maps");

			const FIntPoint VirtualShadowSize = VirtualShadowMapArray.GetPhysicalPoolSize();
			const FIntRect VirtualShadowViewRect = FIntRect(0, 0, VirtualShadowSize.X, VirtualShadowSize.Y);

			Nanite::FRasterContext RasterContext = Nanite::InitRasterContext(GraphBuilder, VirtualShadowSize, Nanite::EOutputBufferMode::DepthOnly, false);

			VirtualShadowMapArray.ClearPhysicalMemory(GraphBuilder, RasterContext.DepthBuffer, Scene->VirtualShadowMapArrayCacheManager);

			const bool bUpdateStreaming = CVarNaniteShadowsUpdateStreaming.GetValueOnRenderThread() != 0;

			auto FilterAndRenderVirtualShadowMaps = [
				&SortedShadowsForShadowDepthPass = SortedShadowsForShadowDepthPass,
				&RasterContext,
				&VirtualShadowMapArray = VirtualShadowMapArray,
				&GraphBuilder,
				bUpdateStreaming,
				Scene = Scene,
				CacheManager = CacheManager,
				CurrentFrameNumber,
				CachedFrameNumber
			](bool bShouldClampToNearPlane, const FString &VirtualFilterName)
			{
				TArray<Nanite::FPackedView, SceneRenderingAllocator> VirtualShadowViews;
				TArray<uint32, SceneRenderingAllocator> VirtualShadowMapFlags;
				VirtualShadowMapFlags.AddZeroed(VirtualShadowMapArray.ShadowMaps.Num());

				// Add any clipmaps first to the ortho rendering pass
				if (bShouldClampToNearPlane)
				{
					for (const TSharedPtr<FVirtualShadowMapClipmap>& Clipmap : SortedShadowsForShadowDepthPass.VirtualShadowMapClipmaps)
					{
						// TODO: Decide if this sort of logic belongs here or in Nanite (as with the mip level view expansion logic)
						// We're eventually going to want to snap/quantize these rectangles/positions somewhat so probably don't want it
						// entirely within Nanite, but likely makes sense to have some sort of "multi-viewport" notion in Nanite that can
						// handle both this and mips.
						// NOTE: There's still the additional VSM view logic that runs on top of this in Nanite too (see CullRasterize variant)
						Nanite::FPackedViewParams BaseParams;
						BaseParams.ViewRect = FIntRect(0, 0, FVirtualShadowMap::VirtualMaxResolutionXY, FVirtualShadowMap::VirtualMaxResolutionXY);
						BaseParams.RasterContextSize = VirtualShadowMapArray.GetPhysicalPoolSize();
						BaseParams.LODScaleFactor = ComputeNaniteShadowsLODScaleFactor();
						BaseParams.PrevTargetLayerIndex = INDEX_NONE;
						BaseParams.TargetMipLevel = 0;
						BaseParams.TargetMipCount = 1;	// No mips for clipmaps

						for (int32 ClipmapLevelIndex = 0; ClipmapLevelIndex < Clipmap->GetLevelCount(); ++ClipmapLevelIndex)
						{
							Nanite::FPackedViewParams Params = BaseParams;
							Params.TargetLayerIndex = Clipmap->GetVirtualShadowMap(ClipmapLevelIndex)->ID;
							Params.ViewMatrices = Clipmap->GetViewMatrices(ClipmapLevelIndex);

							// TODO: Clean this up - could be stored in a single structure for the whole clipmap
							int32 AbsoluteClipmapLevel = Clipmap->GetClipmapLevel(ClipmapLevelIndex);		// NOTE: Can be negative!
							int32 HZBKey = Clipmap->GetLightSceneInfo().Id;
							FVirtualShadowMapHZBMetadata& PrevHZB = CacheManager->HZBMetadata.FindOrAdd(HZBKey);
							if (PrevHZB.FrameNumber == CachedFrameNumber)
							{
								Params.PrevTargetLayerIndex = PrevHZB.TargetLayerIndex;
								Params.PrevViewMatrices = PrevHZB.ViewMatrices;
							}
							else
							{
								Params.PrevTargetLayerIndex = INDEX_NONE;
								Params.PrevViewMatrices = Params.ViewMatrices;
							}

							PrevHZB.TargetLayerIndex = Params.TargetLayerIndex;
							PrevHZB.FrameNumber = CurrentFrameNumber;
							PrevHZB.ViewMatrices = Params.ViewMatrices;

							Nanite::FPackedView View = Nanite::CreatePackedView(Params);
							VirtualShadowViews.Add(View);
							VirtualShadowMapFlags[Params.TargetLayerIndex] = 1;
						}
					}
				}

				for (FProjectedShadowInfo* ProjectedShadowInfo : SortedShadowsForShadowDepthPass.VirtualShadowMapShadows)
				{
					if (ProjectedShadowInfo->ShouldClampToNearPlane() == bShouldClampToNearPlane && ProjectedShadowInfo->HasVirtualShadowMap())
					{
						Nanite::FPackedViewParams Params;
						Params.ViewMatrices = ProjectedShadowInfo->ShadowDepthView->ViewMatrices;
						Params.ViewRect = ProjectedShadowInfo->GetViewRectForView();
						Params.RasterContextSize = VirtualShadowMapArray.GetPhysicalPoolSize();
						Params.LODScaleFactor = ComputeNaniteShadowsLODScaleFactor();
						Params.TargetLayerIndex = ProjectedShadowInfo->VirtualShadowMap->ID;
						Params.PrevTargetLayerIndex = INDEX_NONE;
						Params.TargetMipLevel = 0;
						Params.TargetMipCount = FVirtualShadowMap::MaxMipLevels;

						int32 HZBKey = ProjectedShadowInfo->GetLightSceneInfo().Id;
						HZBKey += FMath::Max(0, ProjectedShadowInfo->CascadeSettings.ShadowSplitIndex) << 28;
						FVirtualShadowMapHZBMetadata& PrevHZB = CacheManager->HZBMetadata.FindOrAdd(HZBKey);
						if( PrevHZB.FrameNumber == CachedFrameNumber )
						{
							Params.PrevTargetLayerIndex = PrevHZB.TargetLayerIndex;
							Params.PrevViewMatrices = PrevHZB.ViewMatrices;
						}
						else
						{
							Params.PrevTargetLayerIndex = INDEX_NONE;
							Params.PrevViewMatrices = Params.ViewMatrices;
						}
						
						PrevHZB.TargetLayerIndex = Params.TargetLayerIndex;
						PrevHZB.FrameNumber = CurrentFrameNumber;
						PrevHZB.ViewMatrices = Params.ViewMatrices;

						Nanite::FPackedView View = Nanite::CreatePackedView(Params);
						VirtualShadowViews.Add(View);
						VirtualShadowMapFlags[ProjectedShadowInfo->VirtualShadowMap->ID] = 1;
					}
				}

				if (VirtualShadowViews.Num() > 0)
				{
					// Update page state for all virtual shadow maps included in this call, it is a bit crap but...
					VirtualShadowMapArray.MarkPhysicalPagesRendered(GraphBuilder, VirtualShadowMapFlags);

					Nanite::FRasterState RasterState;
					if (bShouldClampToNearPlane)
					{
						RasterState.bNearClip = false;
					}

					const bool bPrimaryContext = false;

					Nanite::FCullingContext CullingContext = Nanite::InitCullingContext(
						GraphBuilder,
						*Scene,
						VirtualShadowMapArray.HZBPhysical,
						FIntRect(),
						false,
						bUpdateStreaming,
						false,
						false,
						bPrimaryContext
					);

					const bool bExtractStats = Nanite::IsStatFilterActive(VirtualFilterName);

					Nanite::CullRasterize(
						GraphBuilder,
						*Scene,
						VirtualShadowMapArray,
						VirtualShadowViews,
						CullingContext,
						RasterContext,
						RasterState,
						bExtractStats
					);
				}
			};


			{
				RDG_EVENT_SCOPE(GraphBuilder, "Directional Lights");
				static FString VirtualFilterName = TEXT("VSM_Directional");
				FilterAndRenderVirtualShadowMaps(true, VirtualFilterName);
			}

			{
				RDG_EVENT_SCOPE(GraphBuilder, "Perspective Lights (DepthClip)");
				static FString VirtualFilterName = TEXT("VSM_Perspective");
				FilterAndRenderVirtualShadowMaps(false, VirtualFilterName);
			}


			if( bUseHZB )
			{
				RDG_EVENT_SCOPE(GraphBuilder, "BuildShadowHZB");

				FRDGTextureRef SceneDepth = GraphBuilder.RegisterExternalTexture( GSystemTextures.BlackDummy );
				FRDGTextureRef HZBPhysicalRDG = nullptr;

				// NOTE: 32-bit HZB is important to not lose precision (and thus culling efficiency) with
				// some of the shadow depth functions.
				BuildHZB(
					GraphBuilder,
					SceneDepth,
					RasterContext.DepthBuffer,
					VirtualShadowViewRect,
					/* OutClosestHZBTexture = */ nullptr,
					/* OutFurthestHZBTexture = */ &HZBPhysicalRDG,
					PF_R32_FLOAT);
			
				ConvertToExternalTexture(GraphBuilder, HZBPhysicalRDG, VirtualShadowMapArray.HZBPhysical);
			}

			ConvertToExternalTexture(GraphBuilder, RasterContext.DepthBuffer, VirtualShadowMapArray.PhysicalPagePool);
		}

		Scene->VirtualShadowMapArrayCacheManager->HZBPhysical  = VirtualShadowMapArray.HZBPhysical;
		Scene->VirtualShadowMapArrayCacheManager->HZBPageTable = VirtualShadowMapArray.PageTable;

		//GVisualizeTexture.SetCheckPoint(RHICmdList, VirtualShadowMapArray.PhysicalPagePool);
	}

	FSceneRenderer::RenderShadowDepthMapAtlases( GraphBuilder );	// Render non-VSM shadows. Must be after VSM so we can use TopMip optimization.

	const bool bUseGeometryShader = !GRHISupportsArrayIndexFromAnyShader;

	for (int32 CubemapIndex = 0; CubemapIndex < SortedShadowsForShadowDepthPass.ShadowMapCubemaps.Num(); CubemapIndex++)
	{
		const FSortedShadowMapAtlas& ShadowMap = SortedShadowsForShadowDepthPass.ShadowMapCubemaps[CubemapIndex];
		FSceneRenderTargetItem& RenderTarget = ShadowMap.RenderTargets.DepthTarget->GetRenderTargetItem();
		FIntPoint TargetSize = ShadowMap.RenderTargets.DepthTarget->GetDesc().Extent;

		check(ShadowMap.Shadows.Num() == 1);
		FProjectedShadowInfo* ProjectedShadowInfo = ShadowMap.Shadows[0];
		RDG_GPU_MASK_SCOPE(GraphBuilder, GetGPUMaskForShadow(ProjectedShadowInfo));

		AddUntrackedAccessPass(GraphBuilder, [this, ProjectedShadowInfo, &SceneContext, &ShadowMap, &RenderTarget, TargetSize](FRHICommandListImmediate& RHICmdList)
		{
			const bool bDoParallelDispatch = RHICmdList.IsImmediate() &&  // translucent shadows are draw on the render thread, using a recursive cmdlist (which is not immediate)
				GRHICommandList.UseParallelAlgorithms() && CVarParallelShadows.GetValueOnRenderThread() &&
				(ProjectedShadowInfo->IsWholeSceneDirectionalShadow() || CVarParallelShadowsNonWholeScene.GetValueOnRenderThread());

			GVisualizeTexture.SetCheckPoint(RHICmdList, ShadowMap.RenderTargets.DepthTarget.GetReference());

			FString LightNameWithLevel;
			GetLightNameForDrawEvent(ProjectedShadowInfo->GetLightSceneInfo().Proxy, LightNameWithLevel);
			SCOPED_DRAW_EVENTF(RHICmdList, EventShadowDepths, TEXT("Cubemap %s %u^2"), *LightNameWithLevel, TargetSize.X, TargetSize.Y);

			ProjectedShadowInfo->SetupShadowUniformBuffers(RHICmdList, Scene);

			auto BeginShadowRenderPass = [this, &RenderTarget, &SceneContext](FRHICommandList& InRHICmdList, bool bPerformClear)
			{
				FRHITexture* DepthTarget = RenderTarget.TargetableTexture;
				ERenderTargetLoadAction DepthLoadAction = bPerformClear ? ERenderTargetLoadAction::EClear : ERenderTargetLoadAction::ELoad;

				check(DepthTarget->GetDepthClearValue() == FClearValueBinding::DepthFar.Value.DSValue.Depth);
				FRHIRenderPassInfo RPInfo(DepthTarget, MakeDepthStencilTargetActions(MakeRenderTargetActions(DepthLoadAction, ERenderTargetStoreAction::EStore), ERenderTargetActions::DontLoad_DontStore), nullptr, FExclusiveDepthStencil::DepthWrite_StencilNop);

				if (!GSupportsDepthRenderTargetWithoutColorRenderTarget)
				{
					RPInfo.ColorRenderTargets[0].Action = ERenderTargetActions::DontLoad_DontStore;
					RPInfo.ColorRenderTargets[0].ArraySlice = -1;
					RPInfo.ColorRenderTargets[0].MipIndex = 0;
					RPInfo.ColorRenderTargets[0].RenderTarget = SceneContext.GetOptionalShadowDepthColorSurface(InRHICmdList, DepthTarget->GetTexture2D()->GetSizeX(), DepthTarget->GetTexture2D()->GetSizeY());

					InRHICmdList.Transition(FRHITransitionInfo(RPInfo.ColorRenderTargets[0].RenderTarget, ERHIAccess::Unknown, ERHIAccess::RTV));
				}
				InRHICmdList.Transition(FRHITransitionInfo(DepthTarget, ERHIAccess::Unknown, ERHIAccess::DSVWrite));
				InRHICmdList.BeginRenderPass(RPInfo, TEXT("ShadowDepthCubeMaps"));
			};

			{
				bool bDoClear = true;

				if (ProjectedShadowInfo->CacheMode == SDCM_MovablePrimitivesOnly
					&& Scene->CachedShadowMaps.FindChecked(ProjectedShadowInfo->GetLightSceneInfo().Id).bCachedShadowMapHasPrimitives)
				{
					// Skip the clear when we'll copy from a cached shadowmap
					bDoClear = false;
				}

				SCOPED_CONDITIONAL_DRAW_EVENT(RHICmdList, Clear, bDoClear);
				BeginShadowRenderPass(RHICmdList, bDoClear);
			}

			if (bDoParallelDispatch)
			{
				// In parallel mode this first pass will just be the clear.
				RHICmdList.EndRenderPass();
			}

			ProjectedShadowInfo->RenderDepth(RHICmdList, this, BeginShadowRenderPass, bDoParallelDispatch);

			if (!bDoParallelDispatch)
			{
				RHICmdList.EndRenderPass();
			}
		});

		if (bNaniteEnabled &&
			CVarNaniteShadows.GetValueOnRenderThread() &&
			ProjectedShadowInfo->bNaniteGeometry &&
			ProjectedShadowInfo->CacheMode != SDCM_MovablePrimitivesOnly		// See note in RenderShadowDepthMapAtlases
			)
		{
			FString LightName;
			GetLightNameForDrawEvent(ProjectedShadowInfo->GetLightSceneInfo().Proxy, LightName);

			{
				RDG_EVENT_SCOPE( GraphBuilder, "Nanite Cubemap %s %ux%u", *LightName, ProjectedShadowInfo->ResolutionX, ProjectedShadowInfo->ResolutionY );
				
				FRDGTextureRef RDGShadowMap = GraphBuilder.RegisterExternalTexture( ShadowMap.RenderTargets.DepthTarget, TEXT("ShadowDepthBuffer") );

				// Cubemap shadows reverse the cull mode due to the face matrices (see FShadowDepthPassMeshProcessor::AddMeshBatch)
				Nanite::FRasterState RasterState;
				RasterState.CullMode = CM_CCW;

				const bool bUpdateStreaming = CVarNaniteShadowsUpdateStreaming.GetValueOnRenderThread() != 0;

				FString CubeFilterName;
				if (GNaniteDebugFlags != 0 && GNaniteShowStats != 0)
				{
					// Get the base light filter name.
					CubeFilterName = Nanite::GetFilterNameForLight(ProjectedShadowInfo->GetLightSceneInfo().Proxy);
					CubeFilterName.Append(TEXT("_Face_"));
				}
				
				for (int32 CubemapFaceIndex = 0; CubemapFaceIndex < 6; CubemapFaceIndex++)
				{
					RDG_EVENT_SCOPE( GraphBuilder, "Face %u", CubemapFaceIndex );
					
					// We always render to a whole face at once
					const FIntRect ShadowViewRect = FIntRect(0, 0, TargetSize.X, TargetSize.Y);
					check(ProjectedShadowInfo->X == ShadowViewRect.Min.X);
					check(ProjectedShadowInfo->Y == ShadowViewRect.Min.Y);
					check(ProjectedShadowInfo->ResolutionX == ShadowViewRect.Max.X);
					check(ProjectedShadowInfo->ResolutionY == ShadowViewRect.Max.Y);
					check(ProjectedShadowInfo->BorderSize == 0);
					
					const bool bPrimaryContext = false;
					Nanite::FCullingContext CullingContext = Nanite::InitCullingContext(GraphBuilder, *Scene, nullptr, ShadowViewRect, true, bUpdateStreaming, false, false, bPrimaryContext);
					Nanite::FRasterContext RasterContext = Nanite::InitRasterContext(GraphBuilder, TargetSize, Nanite::EOutputBufferMode::DepthOnly);

					// Setup packed view
					TArray<Nanite::FPackedView, SceneRenderingAllocator> PackedViews;
					{
						FViewMatrices::FMinimalInitializer MatricesInitializer;
						MatricesInitializer.ViewOrigin = -ProjectedShadowInfo->PreShadowTranslation;
						MatricesInitializer.ViewRotationMatrix = ProjectedShadowInfo->OnePassShadowViewMatrices[CubemapFaceIndex];
						MatricesInitializer.ProjectionMatrix = ProjectedShadowInfo->OnePassShadowFaceProjectionMatrix;
						MatricesInitializer.ConstrainedViewRect = ShadowViewRect;

						Nanite::FPackedViewParams Params;
						Params.ViewMatrices = FViewMatrices(MatricesInitializer);
						Params.PrevViewMatrices = Params.ViewMatrices;	// TODO: Real prev frame matrices
						Params.ViewRect = ShadowViewRect;
						Params.RasterContextSize = TargetSize;
						Params.LODScaleFactor = ComputeNaniteShadowsLODScaleFactor();
						PackedViews.Add(Nanite::CreatePackedView(Params));
					}

					FString CubeFaceFilterName;
					if (GNaniteDebugFlags != 0 && GNaniteShowStats != 0)
					{
						CubeFaceFilterName = CubeFilterName;
						CubeFaceFilterName.AppendInt(CubemapFaceIndex);
					}

					const bool bExtractStats = Nanite::IsStatFilterActive(CubeFaceFilterName);

					Nanite::CullRasterize(
						GraphBuilder,
						*Scene,
						PackedViews,
						CullingContext,
						RasterContext,
						RasterState,
						nullptr,
						bExtractStats
					);

					Nanite::EmitCubemapShadow(
						GraphBuilder,
						RasterContext,
						RDGShadowMap,
						ShadowViewRect,
						CubemapFaceIndex,
						bUseGeometryShader);
				}
			}
		}

		AddUntrackedAccessPass(GraphBuilder, [&RenderTarget](FRHICommandList& RHICmdList)
		{
			RHICmdList.Transition(FRHITransitionInfo(RenderTarget.TargetableTexture, ERHIAccess::Unknown, ERHIAccess::SRVMask));
		});
	}

	AddUntrackedAccessPass(GraphBuilder, [this](FRHICommandListImmediate& RHICmdList)
	{
		if (SortedShadowsForShadowDepthPass.PreshadowCache.Shadows.Num() > 0)
		{
			FSceneRenderTargetItem& RenderTarget = SortedShadowsForShadowDepthPass.PreshadowCache.RenderTargets.DepthTarget->GetRenderTargetItem();

			GVisualizeTexture.SetCheckPoint(RHICmdList, SortedShadowsForShadowDepthPass.PreshadowCache.RenderTargets.DepthTarget.GetReference());

			SCOPED_DRAW_EVENT(RHICmdList, PreshadowCache);

			for (int32 ShadowIndex = 0; ShadowIndex < SortedShadowsForShadowDepthPass.PreshadowCache.Shadows.Num(); ShadowIndex++)
			{
				FProjectedShadowInfo* ProjectedShadowInfo = SortedShadowsForShadowDepthPass.PreshadowCache.Shadows[ShadowIndex];

				if (!ProjectedShadowInfo->bDepthsCached)
				{
					SCOPED_GPU_MASK(RHICmdList, GetGPUMaskForShadow(ProjectedShadowInfo));

					const bool bDoParallelDispatch = RHICmdList.IsImmediate() &&  // translucent shadows are draw on the render thread, using a recursive cmdlist (which is not immediate)
						GRHICommandList.UseParallelAlgorithms() && CVarParallelShadows.GetValueOnRenderThread() &&
						(ProjectedShadowInfo->IsWholeSceneDirectionalShadow() || CVarParallelShadowsNonWholeScene.GetValueOnRenderThread());

					ProjectedShadowInfo->SetupShadowUniformBuffers(RHICmdList, Scene);

					auto BeginShadowRenderPass = [this, ProjectedShadowInfo](FRHICommandList& InRHICmdList, bool bPerformClear)
					{
						FRHITexture* PreShadowCacheDepthZ = Scene->PreShadowCacheDepthZ->GetRenderTargetItem().TargetableTexture.GetReference();
						InRHICmdList.TransitionResources(ERHIAccess::DSVWrite, &PreShadowCacheDepthZ, 1);

						FRHIRenderPassInfo RPInfo(PreShadowCacheDepthZ, EDepthStencilTargetActions::LoadDepthNotStencil_StoreDepthNotStencil, nullptr, FExclusiveDepthStencil::DepthWrite_StencilNop);

						// Must preserve existing contents as the clear will be scissored
						InRHICmdList.BeginRenderPass(RPInfo, TEXT("ShadowDepthMaps"));
						ProjectedShadowInfo->ClearDepth(InRHICmdList, this, 0, nullptr, PreShadowCacheDepthZ, bPerformClear);
					};

					BeginShadowRenderPass(RHICmdList, true);

					if (bDoParallelDispatch)
					{
						// In parallel mode the first pass is just the clear.
						RHICmdList.EndRenderPass();
					}

					ProjectedShadowInfo->RenderDepth(RHICmdList, this, BeginShadowRenderPass, bDoParallelDispatch);

					if (!bDoParallelDispatch)
					{
						RHICmdList.EndRenderPass();
					}

					ProjectedShadowInfo->bDepthsCached = true;
				}
			}

			RHICmdList.Transition(FRHITransitionInfo(RenderTarget.TargetableTexture, ERHIAccess::Unknown, ERHIAccess::SRVMask));
		}

		for (int32 AtlasIndex = 0; AtlasIndex < SortedShadowsForShadowDepthPass.TranslucencyShadowMapAtlases.Num(); AtlasIndex++)
		{
			const FSortedShadowMapAtlas& ShadowMapAtlas = SortedShadowsForShadowDepthPass.TranslucencyShadowMapAtlases[AtlasIndex];
			FIntPoint TargetSize = ShadowMapAtlas.RenderTargets.ColorTargets[0]->GetDesc().Extent;

			SCOPED_DRAW_EVENTF(RHICmdList, EventShadowDepths, TEXT("TranslucencyAtlas%u %u^2"), AtlasIndex, TargetSize.X, TargetSize.Y);

			FSceneRenderTargetItem ColorTarget0 = ShadowMapAtlas.RenderTargets.ColorTargets[0]->GetRenderTargetItem();
			FSceneRenderTargetItem ColorTarget1 = ShadowMapAtlas.RenderTargets.ColorTargets[1]->GetRenderTargetItem();

			FRHITexture* RenderTargetArray[2] =
			{
				ColorTarget0.TargetableTexture,
				ColorTarget1.TargetableTexture
			};

			FRHIRenderPassInfo RPInfo(UE_ARRAY_COUNT(RenderTargetArray), RenderTargetArray, ERenderTargetActions::Load_Store);
			TransitionRenderPassTargets(RHICmdList, RPInfo);
			RHICmdList.BeginRenderPass(RPInfo, TEXT("RenderTranslucencyDepths"));
			{
				for (int32 ShadowIndex = 0; ShadowIndex < ShadowMapAtlas.Shadows.Num(); ShadowIndex++)
				{
					FProjectedShadowInfo* ProjectedShadowInfo = ShadowMapAtlas.Shadows[ShadowIndex];
					SCOPED_GPU_MASK(RHICmdList, GetGPUMaskForShadow(ProjectedShadowInfo));
					ProjectedShadowInfo->SetupShadowUniformBuffers(RHICmdList, Scene);
					ProjectedShadowInfo->RenderTranslucencyDepths(RHICmdList, this);
				}
			}
			RHICmdList.EndRenderPass();

			RHICmdList.Transition({
				FRHITransitionInfo(ColorTarget0.TargetableTexture, ERHIAccess::Unknown, ERHIAccess::SRVMask),
				FRHITransitionInfo(ColorTarget1.TargetableTexture, ERHIAccess::Unknown, ERHIAccess::SRVMask)
			});
		}

		bShadowDepthRenderCompleted = true;
	});
}

bool FShadowDepthPassMeshProcessor::Process(
	const FMeshBatch& RESTRICT MeshBatch,
	uint64 BatchElementMask,
	int32 StaticMeshId,
	const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy,
	const FMaterialRenderProxy& RESTRICT MaterialRenderProxy,
	const FMaterial& RESTRICT MaterialResource,
	ERasterizerFillMode MeshFillMode,
	ERasterizerCullMode MeshCullMode)
{
	const FVertexFactory* VertexFactory = MeshBatch.VertexFactory;

	TMeshProcessorShaders<
		FShadowDepthVS,
		FBaseHS,
		FBaseDS,
		FShadowDepthBasePS,
		FOnePassPointShadowDepthGS> ShadowDepthPassShaders;

	const bool bUsePositionOnlyVS =
		   VertexFactory->SupportsPositionAndNormalOnlyStream()
		&& MaterialResource.WritesEveryPixel(true)
		&& !MaterialResource.MaterialModifiesMeshPosition_RenderThread();

	if (!GetShadowDepthPassShaders(
		MaterialResource,
		VertexFactory,
		FeatureLevel,
		ShadowDepthType.bDirectionalLight,
		ShadowDepthType.bOnePassPointLightShadow,
		bUsePositionOnlyVS,
		ShadowDepthPassShaders.VertexShader,
		ShadowDepthPassShaders.HullShader,
		ShadowDepthPassShaders.DomainShader,
		ShadowDepthPassShaders.PixelShader,
		ShadowDepthPassShaders.GeometryShader))
	{
		return false;
	}

	FShadowDepthShaderElementData ShaderElementData;
	ShaderElementData.InitializeMeshMaterialData(ViewIfDynamicMeshCommand, PrimitiveSceneProxy, MeshBatch, StaticMeshId, false);

	const FMeshDrawCommandSortKey SortKey = CalculateMeshStaticSortKey(ShadowDepthPassShaders.VertexShader, ShadowDepthPassShaders.PixelShader);

	const uint32 InstanceFactor = !ShadowDepthType.bOnePassPointLightShadow || (GShadowUseGS && RHISupportsGeometryShaders(GShaderPlatformForFeatureLevel[FeatureLevel])) ? 1 : 6;
	for (uint32 i = 0; i < InstanceFactor; i++)
	{
		ShaderElementData.LayerId = i;

		BuildMeshDrawCommands(
			MeshBatch,
			BatchElementMask,
			PrimitiveSceneProxy,
			MaterialRenderProxy,
			MaterialResource,
			PassDrawRenderState,
			ShadowDepthPassShaders,
			MeshFillMode,
			MeshCullMode,
			SortKey,
			bUsePositionOnlyVS ? EMeshPassFeatures::PositionAndNormalOnly : EMeshPassFeatures::Default,
			ShaderElementData);
	}

	return true;
}

bool FShadowDepthPassMeshProcessor::TryAddMeshBatch(const FMeshBatch& RESTRICT MeshBatch,
	uint64 BatchElementMask,
	const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy,
	int32 StaticMeshId,
	const FMaterialRenderProxy& MaterialRenderProxy,
	const FMaterial& Material)
{
	const EBlendMode BlendMode = Material.GetBlendMode();
	const bool bShouldCastShadow = Material.ShouldCastDynamicShadows();

	const FMeshDrawingPolicyOverrideSettings OverrideSettings = ComputeMeshOverrideSettings(MeshBatch);
	const ERasterizerFillMode MeshFillMode = ComputeMeshFillMode(MeshBatch, Material, OverrideSettings);

	ERasterizerCullMode FinalCullMode;

	{
		const ERasterizerCullMode MeshCullMode = ComputeMeshCullMode(MeshBatch, Material, OverrideSettings);

		const bool bTwoSided = Material.IsTwoSided() || PrimitiveSceneProxy->CastsShadowAsTwoSided();
		// Invert culling order when mobile HDR == false.
		auto ShaderPlatform = GShaderPlatformForFeatureLevel[FeatureLevel];
		static auto* MobileHDRCvar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.MobileHDR"));
		check(MobileHDRCvar);
		const bool bPlatformReversesCulling = (RHINeedsToSwitchVerticalAxis(ShaderPlatform) && MobileHDRCvar->GetValueOnAnyThread() == 0);

		const bool bRenderSceneTwoSided = bTwoSided;
		const bool bReverseCullMode = XOR(bPlatformReversesCulling, ShadowDepthType.bOnePassPointLightShadow);

		FinalCullMode = bRenderSceneTwoSided ? CM_None : bReverseCullMode ? InverseCullMode(MeshCullMode) : MeshCullMode;
	}

	bool bResult = true;
	if (bShouldCastShadow
		&& ShouldIncludeDomainInMeshPass(Material.GetMaterialDomain())
		&& ShouldIncludeMaterialInDefaultOpaquePass(Material))
	{
		const FMaterialRenderProxy* EffectiveMaterialRenderProxy = &MaterialRenderProxy;
		const FMaterial* EffectiveMaterial = &Material;

		OverrideWithDefaultMaterialForShadowDepth(EffectiveMaterialRenderProxy, EffectiveMaterial, FeatureLevel);

		bResult = Process(MeshBatch, BatchElementMask, StaticMeshId, PrimitiveSceneProxy, *EffectiveMaterialRenderProxy, *EffectiveMaterial, MeshFillMode, FinalCullMode);
	}

	return bResult;
}

void FShadowDepthPassMeshProcessor::AddMeshBatch(const FMeshBatch& RESTRICT MeshBatch, uint64 BatchElementMask, const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy, int32 StaticMeshId)
{
	if (MeshBatch.CastShadow)
	{
		const FMaterialRenderProxy* MaterialRenderProxy = MeshBatch.MaterialRenderProxy;
		while (MaterialRenderProxy)
		{
			const FMaterial* Material = MaterialRenderProxy->GetMaterialNoFallback(FeatureLevel);
			if (Material)
			{
				if (TryAddMeshBatch(MeshBatch, BatchElementMask, PrimitiveSceneProxy, StaticMeshId, *MaterialRenderProxy, *Material))
				{
					break;
				}
			}
			MaterialRenderProxy = MaterialRenderProxy->GetFallback(FeatureLevel);
		}
	}
}

FShadowDepthPassMeshProcessor::FShadowDepthPassMeshProcessor(
	const FScene* Scene,
	const FSceneView* InViewIfDynamicMeshCommand,
	const TUniformBufferRef<FViewUniformShaderParameters>& InViewUniformBuffer,
	FRHIUniformBuffer* InPassUniformBuffer,
	FShadowDepthType InShadowDepthType,
	FMeshPassDrawListContext* InDrawListContext)
	: FMeshPassProcessor(Scene, Scene->GetFeatureLevel(), InViewIfDynamicMeshCommand, InDrawListContext)
	, PassDrawRenderState(FMeshPassProcessorRenderState(InViewUniformBuffer, InPassUniformBuffer))
	, ShadowDepthType(InShadowDepthType)
{
	SetStateForShadowDepth(ShadowDepthType.bOnePassPointLightShadow, PassDrawRenderState);
}

FShadowDepthType CSMShadowDepthType(true, false);

FMeshPassProcessor* CreateCSMShadowDepthPassProcessor(const FScene* Scene, const FSceneView* InViewIfDynamicMeshCommand, FMeshPassDrawListContext* InDrawListContext)
{
	FRHIUniformBuffer* PassUniformBuffer = nullptr;

	EShadingPath ShadingPath = Scene->GetShadingPath();
	if (ShadingPath == EShadingPath::Mobile)
	{
		PassUniformBuffer = Scene->UniformBuffers.MobileCSMShadowDepthPassUniformBuffer;
	}
	else //deferred
	{
		PassUniformBuffer = Scene->UniformBuffers.CSMShadowDepthPassUniformBuffer;
	}

	return new(FMemStack::Get()) FShadowDepthPassMeshProcessor(
		Scene,
		InViewIfDynamicMeshCommand,
		Scene->UniformBuffers.CSMShadowDepthViewUniformBuffer,
		PassUniformBuffer,
		CSMShadowDepthType,
		InDrawListContext);
}

FRegisterPassProcessorCreateFunction RegisterCSMShadowDepthPass(&CreateCSMShadowDepthPassProcessor, EShadingPath::Deferred, EMeshPass::CSMShadowDepth, EMeshPassFlags::CachedMeshCommands);
FRegisterPassProcessorCreateFunction RegisterMobileCSMShadowDepthPass(&CreateCSMShadowDepthPassProcessor, EShadingPath::Mobile, EMeshPass::CSMShadowDepth, EMeshPassFlags::CachedMeshCommands);
