// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	StrandHairVertexFactory.cpp: Strand hair vertex factory implementation
=============================================================================*/

#include "HairStrandsVertexFactory.h"
#include "SceneView.h"
#include "MeshBatch.h"
#include "ShaderParameterUtils.h"
#include "Rendering/ColorVertexBuffer.h"
#include "MeshMaterialShader.h"
#include "HairStrandsInterface.h"

static float GStrandHairWidth = 0.0f;
static FAutoConsoleVariableRef CVarStrandHairWidth(TEXT("r.HairStrands.StrandWidth"), GStrandHairWidth, TEXT("Width of hair strand"));

float FHairStrandsVertexFactory::GetMaxStrandRadius() const
{
	return GStrandHairWidth > 0 ? GStrandHairWidth * 0.5f : Data.MaxStrandRadius;
}

#define OPTIMIZE_OFF 0

#if OPTIMIZE_OFF
	#pragma optimize("", off)
#endif

/////////////////////////////////////////////////////////////////////////////////////////

template<typename T> inline void BindParam(FMeshDrawSingleShaderBindings& ShaderBindings, const FShaderResourceParameter& Param, T* Value)	{ if (Param.IsBound() && Value) ShaderBindings.Add(Param, Value); }
template<typename T> inline void BindParam(FMeshDrawSingleShaderBindings& ShaderBindings, const FShaderParameter& Param, const T& Value)	{ if (Param.IsBound()) ShaderBindings.Add(Param, Value); }

class FHairStrandsVertexFactoryShaderParameters : public FVertexFactoryShaderParameters
{
public:

	FShaderParameter Radius;
	FShaderParameter Length;
	FShaderParameter RadiusAtDepth1_Primary;
	FShaderParameter RadiusAtDepth1_Velocity;
	FShaderParameter WorldOffset;
	FShaderParameter Density;

	FShaderResourceParameter PositionBuffer;
	FShaderResourceParameter PreviousPositionBuffer;
	FShaderResourceParameter AttributeBuffer;
	FShaderResourceParameter TangentBuffer;

	virtual void Bind(const FShaderParameterMap& ParameterMap) override
	{
		Radius.Bind(ParameterMap, TEXT("HairStrandsVF_Radius"));
		Length.Bind(ParameterMap, TEXT("HairStrandsVF_Length"));
		RadiusAtDepth1_Primary.Bind(ParameterMap, TEXT("HairStrandsVF_RadiusAtDepth1_Primary"));
		RadiusAtDepth1_Velocity.Bind(ParameterMap, TEXT("HairStrandsVF_RadiusAtDepth1_Velocity"));
		WorldOffset.Bind(ParameterMap, TEXT("HairStrandsVF_WorldOffset"));
		Density.Bind(ParameterMap, TEXT("HairStrandsVF_Density"));	

		PositionBuffer.Bind(ParameterMap, TEXT("HairStrandsVF_PositionBuffer"));
		PreviousPositionBuffer.Bind(ParameterMap, TEXT("HairStrandsVF_PreviousPositionBuffer"));
		AttributeBuffer.Bind(ParameterMap, TEXT("HairStrandsVF_AttributeBuffer"));
		TangentBuffer.Bind(ParameterMap, TEXT("HairStrandsVF_TangentBuffer"));
	}

	virtual void Serialize(FArchive& Ar) override
	{
		Ar << Radius;
		Ar << Length;
		Ar << RadiusAtDepth1_Primary;
		Ar << RadiusAtDepth1_Velocity;
		Ar << WorldOffset;
		Ar << Density;

		Ar << PositionBuffer;
		Ar << PreviousPositionBuffer;
		Ar << AttributeBuffer;
		Ar << TangentBuffer;
	}

	virtual void GetElementShaderBindings(
		const FSceneInterface* Scene,
		const FSceneView* View,
		const FMeshMaterialShader* Shader,
		const EVertexInputStreamType InputStreamType,
		ERHIFeatureLevel::Type FeatureLevel,
		const FVertexFactory* VertexFactory,
		const FMeshBatchElement& BatchElement,
		FMeshDrawSingleShaderBindings& ShaderBindings,
		FVertexInputStreamArray& VertexStreams
	) const override
	{
		const FHairStrandsVertexFactory* VF = static_cast<const FHairStrandsVertexFactory*>(VertexFactory);
		
		const FMinHairRadiusAtDepth1 MinRadiusAtDepth1 = ComputeMinStrandRadiusAtDepth1(
			FIntPoint(View->UnconstrainedViewRect.Width(), View->UnconstrainedViewRect.Height()),
			View->FOV,
			GetHairVisibilitySampleCount(),
			0.f);

		BindParam(ShaderBindings, PositionBuffer, VF->GetPositionSRV());
		BindParam(ShaderBindings, PreviousPositionBuffer, VF->GetPreviousPositionSRV());
		BindParam(ShaderBindings, AttributeBuffer, VF->GetAttributeSRV());
		BindParam(ShaderBindings, TangentBuffer, VF->GetTangentSRV());
		BindParam(ShaderBindings, Radius, VF->GetMaxStrandRadius());
		BindParam(ShaderBindings, Length, VF->GetMaxStrandLength());
		BindParam(ShaderBindings, WorldOffset, VF->GetWorldOffset());
		BindParam(ShaderBindings, Density, VF->GetHairDensity());
		BindParam(ShaderBindings, RadiusAtDepth1_Primary, MinRadiusAtDepth1.Primary);
		BindParam(ShaderBindings, RadiusAtDepth1_Velocity, MinRadiusAtDepth1.Velocity);		
	}
};

/**
 * Should we cache the material's shadertype on this platform with this vertex factory? 
 */
bool FHairStrandsVertexFactory::ShouldCompilePermutation(EShaderPlatform Platform, const class FMaterial* Material, const class FShaderType* ShaderType)
{
	return (Material->GetMaterialDomain() == MD_Surface && Material->IsUsedWithHairStrands() && Platform == EShaderPlatform::SP_PCD3D_SM5) || Material->IsSpecialEngineMaterial();
}

void FHairStrandsVertexFactory::ModifyCompilationEnvironment(const FVertexFactoryType* Type, EShaderPlatform Platform, const FMaterial* Material, FShaderCompilerEnvironment& OutEnvironment)
{
	OutEnvironment.SetDefine(TEXT("VF_SUPPORTS_PRIMITIVE_SCENE_DATA"), Type->SupportsPrimitiveIdStream() && UseGPUScene(Platform, GetMaxSupportedFeatureLevel(Platform)));
	OutEnvironment.SetDefine(TEXT("VF_STRAND_HAIR"), TEXT("1"));
}

void FHairStrandsVertexFactory::ValidateCompiledResult(const FVertexFactoryType* Type, EShaderPlatform Platform, const FShaderParameterMap& ParameterMap, TArray<FString>& OutErrors)
{
	if (Type->SupportsPrimitiveIdStream() 
		&& UseGPUScene(Platform, GetMaxSupportedFeatureLevel(Platform)) 
		&& ParameterMap.ContainsParameterAllocation(FPrimitiveUniformShaderParameters::StaticStructMetadata.GetShaderVariableName()))
	{
		OutErrors.AddUnique(*FString::Printf(TEXT("Shader attempted to bind the Primitive uniform buffer even though Vertex Factory %s computes a PrimitiveId per-instance.  This will break auto-instancing.  Shaders should use GetPrimitiveData(PrimitiveId).Member instead of Primitive.Member."), Type->GetName()));
	}
}

void FHairStrandsVertexFactory::SetData(const FDataType& InData)
{
	check(IsInRenderingThread());
	Data = InData;
	UpdateRHI();
}

/**
* Copy the data from another vertex factory
* @param Other - factory to copy from
*/
void FHairStrandsVertexFactory::Copy(const FHairStrandsVertexFactory& Other)
{
	FHairStrandsVertexFactory* VertexFactory = this;
	const FDataType* DataCopy = &Other.Data;
	ENQUEUE_RENDER_COMMAND(FHairStrandsVertexFactoryCopyData)(
		[VertexFactory, DataCopy](FRHICommandListImmediate& RHICmdList)
		{
			VertexFactory->Data = *DataCopy;
		});
	BeginUpdateResourceRHI(this);
}

void FHairStrandsVertexFactory::InitRHI()
{
	bNeedsDeclaration = false;
	bSupportsManualVertexFetch = true;

	// We create different streams based on feature level
	check(HasValidFeatureLevel());

	// VertexFactory needs to be able to support max possible shader platform and feature level
	// in case if we switch feature level at runtime.
	const bool bCanUseGPUScene = UseGPUScene(GMaxRHIShaderPlatform, GMaxRHIFeatureLevel);

	FVertexDeclarationElementList Elements;
	SetPrimitiveIdStreamIndex(EVertexInputStreamType::Default, -1);
	if (GetType()->SupportsPrimitiveIdStream() && bCanUseGPUScene)
	{
		// When the VF is used for rendering in normal mesh passes, this vertex buffer and offset will be overridden
		Elements.Add(AccessStreamComponent(FVertexStreamComponent(&GPrimitiveIdDummy, 0, 0, sizeof(uint32), VET_UInt, EVertexStreamUsage::Instancing), 13));
		SetPrimitiveIdStreamIndex(EVertexInputStreamType::Default, Elements.Last().StreamIndex);
		bNeedsDeclaration = true;
	}

	check(Streams.Num() > 0);

	InitDeclaration(Elements);
	check(IsValidRef(GetDeclaration()));
}

FVertexFactoryShaderParameters* FHairStrandsVertexFactory::ConstructShaderParameters(EShaderFrequency ShaderFrequency)
{
	if (ShaderFrequency == SF_Vertex)
	{
		return new FHairStrandsVertexFactoryShaderParameters();
	}

#if RHI_RAYTRACING
	if (ShaderFrequency == SF_RayHitGroup)
	{
		return new FHairStrandsVertexFactoryShaderParameters();
	}
#endif // RHI_RAYTRACING

	return NULL;
}

void FHairStrandsVertexFactory::ReleaseRHI()
{
	FVertexFactory::ReleaseRHI();
}


IMPLEMENT_VERTEX_FACTORY_TYPE_EX(FHairStrandsVertexFactory,"/Engine/Private/HairStrands/HairStrandsVertexFactory.ush",true,false,true,true,true,true,true);

#if OPTIMIZE_OFF
	#pragma optimize("", on)
#endif