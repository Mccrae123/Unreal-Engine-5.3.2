// CopyRight 1998-2016 Epic Games, Inc. All Rights Reserved.

/*=============================================================================
ModifiedWBoundaryMask.cpp: Shaders and code for rendering ModifiedW boundary mask
=============================================================================*/

#include "RendererPrivate.h"
#include "GlobalShader.h"
#include "RHIStaticStates.h"
#include "DeferredShadingRenderer.h"
#include "PipelineStateCache.h"

/**
* ModifiedW will cause scene objects that were originally outside of the viewing frustum to become visible due to the W component
* of their vertex positions. However, those objects will still not be visible in the final render after reversing the transform back 
* into linear space. Thus there is no reason to spend valuable time and resources rendering them in the first place. The boundary 
* mask is an inverse "guard band" of sorts that extends from the edges of the visible screen space viewport to some arbitrarily far
* boundaries. By rendering this mask first with depth=near, the GPU will automatically reject pixels in areas not visible in the 
* final image. It serves a similar purpose to the HMD hidden area mask, and should be used together with said mask in a similar 
* fashion when rendering in VR. Use of the boundary mask is essential to see performance improvements with Lens-Matched Shading.
*/

// see ModifiedWBoundaryMask.usf for shader implementations
class FModifiedWBoundaryMaskVS : public FGlobalShader
{
	DECLARE_SHADER_TYPE(FModifiedWBoundaryMaskVS, Global);
public:
	static bool ShouldCache(EShaderPlatform Platform)
	{
		return IsFeatureLevelSupported(Platform, ERHIFeatureLevel::SM5);
	}

	FModifiedWBoundaryMaskVS()
	{
	}

	/** Initialization constructor. */
	FModifiedWBoundaryMaskVS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FGlobalShader(Initializer)
	{
	}
};

IMPLEMENT_SHADER_TYPE(, FModifiedWBoundaryMaskVS, TEXT("ModifiedWBoundaryMask"), TEXT("VSMain"), SF_Vertex);

class FModifiedWBoundaryMaskFGS : public FGlobalShader
{
	DECLARE_SHADER_TYPE(FModifiedWBoundaryMaskFGS, Global);
public:
	static bool ShouldCache(EShaderPlatform Platform)
	{
		static const auto CVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("vr.LensMatchedShading"));
		static const bool bLMSShaders = CVar->GetValueOnAnyThread() != 0;

		return IsFeatureLevelSupported(Platform, ERHIFeatureLevel::SM5) && RHISupportsFastGeometryShaders(Platform) && bLMSShaders;
	}

	FModifiedWBoundaryMaskFGS()
	{
	}

	/** Initialization constructor. */
	FModifiedWBoundaryMaskFGS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FGlobalShader(Initializer)
	{
	}

	static bool IsFastGeometryShader()
	{
		return true;
	}
};

IMPLEMENT_SHADER_TYPE(, FModifiedWBoundaryMaskFGS, TEXT("ModifiedWBoundaryMask"), TEXT("FGSMain"), SF_Geometry);

// rendering the mask itself, should call before depth prepass and any scene rendering
void FSceneRenderer::RenderModifiedWBoundaryMask(FRHICommandListImmediate& RHICmdList, FGraphicsPipelineStateInitializer &GraphicsPSOInit)
{
	// since we only render to the depth buffer, no pixel shader required
	const auto ShaderMap = GetGlobalShaderMap(FeatureLevel);
	TShaderMapRef< FModifiedWBoundaryMaskVS > VertexShader(ShaderMap);
	TShaderMapRef< FModifiedWBoundaryMaskFGS > GeometryShader(ShaderMap);

	GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GetVertexDeclarationFVector4();
	GraphicsPSOInit.BoundShaderState.VertexShaderRHI = GETSAFERHISHADER_VERTEX(*VertexShader);
	GraphicsPSOInit.BoundShaderState.GeometryShaderRHI = GETSAFERHISHADER_GEOMETRY(*GeometryShader);
	SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit);

	// no vertex buffer needed as we compute it in VS
	RHICmdList.SetStreamSource(0, NULL, 0, 0);

	// mask is two triangles covering the screen which will be warped into octagon shape
	RHICmdList.DrawPrimitive(PT_TriangleList,
		/*BaseVertexIndex=*/ 0,
		/*NumPrimitives=*/ 2,
		/*NumInstances=*/ 1
		);
}