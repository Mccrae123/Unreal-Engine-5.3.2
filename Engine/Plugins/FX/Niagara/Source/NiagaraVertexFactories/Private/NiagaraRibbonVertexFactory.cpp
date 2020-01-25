// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	ParticleVertexFactory.cpp: Particle vertex factory implementation.
=============================================================================*/

#include "NiagaraRibbonVertexFactory.h"
#include "ParticleHelper.h"
#include "ParticleResources.h"
#include "ShaderParameterUtils.h"
#include "MeshMaterialShader.h"

IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FNiagaraRibbonUniformParameters, "NiagaraRibbonVF");
IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FNiagaraRibbonVFLooseParameters, "NiagaraRibbonVFLooseParameters");


class FNiagaraRibbonVertexFactoryShaderParameters : public FVertexFactoryShaderParameters
{
public:
	virtual void Bind(const FShaderParameterMap& ParameterMap) override
	{
	}

	virtual void Serialize(FArchive& Ar) override
	{
	}
};

/**
* Shader parameters for the beam/trail vertex factory.
*/
class FNiagaraRibbonVertexFactoryShaderParametersVS : public FNiagaraRibbonVertexFactoryShaderParameters
{
public:
	virtual void Bind(const FShaderParameterMap& ParameterMap) override
	{
		NiagaraParticleDataPosition.Bind(ParameterMap, TEXT("NiagaraParticleDataPosition"));
		NiagaraParticleDataVelocity.Bind(ParameterMap, TEXT("NiagaraParticleDataVelocity"));
		NiagaraParticleDataColor.Bind(ParameterMap, TEXT("NiagaraParticleDataColor"));
		NiagaraParticleDataWidth.Bind(ParameterMap, TEXT("NiagaraParticleDataWidth"));
		NiagaraParticleDataTwist.Bind(ParameterMap, TEXT("NiagaraParticleDataTwist"));
		NiagaraParticleDataFacing.Bind(ParameterMap, TEXT("NiagaraParticleDataFacing"));
		NiagaraParticleDataNormalizedAge.Bind(ParameterMap, TEXT("NiagaraParticleDataNormalizedAge"));
		NiagaraParticleDataMaterialRandom.Bind(ParameterMap, TEXT("NiagaraParticleDataMaterialRandom"));
		NiagaraParticleDataMaterialParam0.Bind(ParameterMap, TEXT("NiagaraParticleDataMaterialParam0"));
		NiagaraParticleDataMaterialParam1.Bind(ParameterMap, TEXT("NiagaraParticleDataMaterialParam1"));
		NiagaraParticleDataMaterialParam2.Bind(ParameterMap, TEXT("NiagaraParticleDataMaterialParam2"));
		NiagaraParticleDataMaterialParam3.Bind(ParameterMap, TEXT("NiagaraParticleDataMaterialParam3"));

		FloatDataStride.Bind(ParameterMap, TEXT("NiagaraFloatDataStride"));
	}

	virtual void Serialize(FArchive& Ar) override
	{
		Ar << NiagaraParticleDataPosition;
		Ar << NiagaraParticleDataVelocity;
		Ar << NiagaraParticleDataColor;
		Ar << NiagaraParticleDataWidth;
		Ar << NiagaraParticleDataTwist;
		Ar << NiagaraParticleDataFacing;
		Ar << NiagaraParticleDataNormalizedAge;
		Ar << NiagaraParticleDataMaterialRandom;
		Ar << NiagaraParticleDataMaterialParam0;
		Ar << NiagaraParticleDataMaterialParam1;
		Ar << NiagaraParticleDataMaterialParam2;
		Ar << NiagaraParticleDataMaterialParam3;
		Ar << FloatDataStride;
	}

	virtual void GetElementShaderBindings(
		const FSceneInterface* Scene,
		const FSceneView* View,
		const FMeshMaterialShader* Shader,
		const EVertexInputStreamType InputStreamType,
		ERHIFeatureLevel::Type FeatureLevel,
		const FVertexFactory* VertexFactory,
		const FMeshBatchElement& BatchElement,
		class FMeshDrawSingleShaderBindings& ShaderBindings,
		FVertexInputStreamArray& VertexStreams) const override
	{
		FNiagaraRibbonVertexFactory* RibbonVF = (FNiagaraRibbonVertexFactory*)VertexFactory;
		ShaderBindings.Add(Shader->GetUniformBufferParameter<FNiagaraRibbonUniformParameters>(), RibbonVF->GetRibbonUniformBuffer());
		ShaderBindings.Add(Shader->GetUniformBufferParameter<FNiagaraRibbonVFLooseParameters>(), RibbonVF->LooseParameterUniformBuffer);
		ShaderBindings.Add(NiagaraParticleDataPosition, RibbonVF->GetParticleDataFloatSRV());
		ShaderBindings.Add(NiagaraParticleDataVelocity, RibbonVF->GetParticleDataFloatSRV());
		ShaderBindings.Add(NiagaraParticleDataColor, RibbonVF->GetParticleDataFloatSRV());
		ShaderBindings.Add(NiagaraParticleDataWidth, RibbonVF->GetParticleDataFloatSRV());
		ShaderBindings.Add(NiagaraParticleDataTwist, RibbonVF->GetParticleDataFloatSRV());
		ShaderBindings.Add(NiagaraParticleDataFacing, RibbonVF->GetParticleDataFloatSRV());
		ShaderBindings.Add(NiagaraParticleDataNormalizedAge, RibbonVF->GetParticleDataFloatSRV());
		ShaderBindings.Add(NiagaraParticleDataMaterialRandom, RibbonVF->GetParticleDataFloatSRV());
		ShaderBindings.Add(NiagaraParticleDataMaterialParam0, RibbonVF->GetParticleDataFloatSRV());
		ShaderBindings.Add(NiagaraParticleDataMaterialParam1, RibbonVF->GetParticleDataFloatSRV());
		ShaderBindings.Add(NiagaraParticleDataMaterialParam2, RibbonVF->GetParticleDataFloatSRV());
		ShaderBindings.Add(NiagaraParticleDataMaterialParam3, RibbonVF->GetParticleDataFloatSRV());
		ShaderBindings.Add(FloatDataStride, RibbonVF->GetFloatDataStride());
	}

private:
	FShaderResourceParameter NiagaraParticleDataPosition;
	FShaderResourceParameter NiagaraParticleDataVelocity;
	FShaderResourceParameter NiagaraParticleDataColor;
	FShaderResourceParameter NiagaraParticleDataWidth;
	FShaderResourceParameter NiagaraParticleDataTwist;
	FShaderResourceParameter NiagaraParticleDataFacing;
	FShaderResourceParameter NiagaraParticleDataNormalizedAge;
	FShaderResourceParameter NiagaraParticleDataMaterialRandom;
	FShaderResourceParameter NiagaraParticleDataMaterialParam0;
	FShaderResourceParameter NiagaraParticleDataMaterialParam1;
	FShaderResourceParameter NiagaraParticleDataMaterialParam2;
	FShaderResourceParameter NiagaraParticleDataMaterialParam3;

	FShaderParameter FloatDataStride;
};



/**
* Shader parameters for the beam/trail vertex factory.
*/
class FNiagaraRibbonVertexFactoryShaderParametersPS : public FNiagaraRibbonVertexFactoryShaderParameters
{
public:
	virtual void Bind(const FShaderParameterMap& ParameterMap) override
	{
	}

	virtual void Serialize(FArchive& Ar) override
	{
	}

	virtual void GetElementShaderBindings(
		const FSceneInterface* Scene,
		const FSceneView* View,
		const FMeshMaterialShader* Shader,
		const EVertexInputStreamType InputStreamType,
		ERHIFeatureLevel::Type FeatureLevel,
		const FVertexFactory* VertexFactory,
		const FMeshBatchElement& BatchElement,
		class FMeshDrawSingleShaderBindings& ShaderBindings,
		FVertexInputStreamArray& VertexStreams) const override
	{
		FNiagaraRibbonVertexFactory* RibbonVF = (FNiagaraRibbonVertexFactory*)VertexFactory;
		ShaderBindings.Add(Shader->GetUniformBufferParameter<FNiagaraRibbonUniformParameters>(), RibbonVF->GetRibbonUniformBuffer());
	}
};


///////////////////////////////////////////////////////////////////////////////
/**
* The Niagara ribbon vertex declaration resource type.
*/
class FNiagaraRibbonVertexDeclaration : public FRenderResource
{
public:
	FVertexDeclarationRHIRef VertexDeclarationRHI;

	// Destructor.
	virtual ~FNiagaraRibbonVertexDeclaration() {}

	virtual void FillDeclElements(FVertexDeclarationElementList& Elements, int32& Offset)
	{
	}

	virtual void InitDynamicRHI()
	{
		FVertexDeclarationElementList Elements;
		int32	Offset = 0;
		FillDeclElements(Elements, Offset);

		// Create the vertex declaration for rendering the factory normally.
		// This is done in InitDynamicRHI instead of InitRHI to allow FNiagaraRibbonVertexFactory::InitRHI
		// to rely on it being initialized, since InitDynamicRHI is called before InitRHI.
		VertexDeclarationRHI = PipelineStateCache::GetOrCreateVertexDeclaration(Elements);
	}

	virtual void ReleaseDynamicRHI()
	{
		VertexDeclarationRHI.SafeRelease();
	}
};

/** The simple element vertex declaration. */
static TGlobalResource<FNiagaraRibbonVertexDeclaration> GNiagaraRibbonVertexDeclaration;

///////////////////////////////////////////////////////////////////////////////

bool FNiagaraRibbonVertexFactory::ShouldCompilePermutation(EShaderPlatform Platform, const class FMaterial* Material, const class FShaderType* ShaderType)
{
	return (FNiagaraUtilities::SupportsNiagaraRendering(Platform)) && (Material->IsUsedWithNiagaraRibbons() || Material->IsSpecialEngineMaterial());
}

/**
* Can be overridden by FVertexFactory subclasses to modify their compile environment just before compilation occurs.
*/
void FNiagaraRibbonVertexFactory::ModifyCompilationEnvironment(const FVertexFactoryType* Type, EShaderPlatform Platform, const class FMaterial* Material, FShaderCompilerEnvironment& OutEnvironment)
{
	FNiagaraVertexFactoryBase::ModifyCompilationEnvironment(Type, Platform, Material, OutEnvironment);

	OutEnvironment.SetDefine(TEXT("NiagaraVFLooseParameters"), TEXT("NiagaraRibbonVFLooseParameters"));
	
	OutEnvironment.SetDefine(TEXT("NIAGARA_RIBBON_FACTORY"), TEXT("1"));
}

/**
*	Initialize the Render Hardware Interface for this vertex factory
*/
void FNiagaraRibbonVertexFactory::InitRHI()
{
	SetDeclaration(GNiagaraRibbonVertexDeclaration.VertexDeclarationRHI);

	FVertexStream* VertexStream = new(Streams) FVertexStream;
	FVertexStream* DynamicParameterStream = new(Streams) FVertexStream;
	FVertexStream* DynamicParameter1Stream = new(Streams) FVertexStream;
	FVertexStream* DynamicParameter2Stream = new(Streams) FVertexStream;
	FVertexStream* DynamicParameter3Stream = new(Streams) FVertexStream;
}

FVertexFactoryShaderParameters* FNiagaraRibbonVertexFactory::ConstructShaderParameters(EShaderFrequency ShaderFrequency)
{
	if (ShaderFrequency == SF_Vertex)
	{
		return new FNiagaraRibbonVertexFactoryShaderParametersVS();
	}
	else if (ShaderFrequency == SF_Pixel)
	{
		return new FNiagaraRibbonVertexFactoryShaderParametersPS();
	}
#if RHI_RAYTRACING
	else if (ShaderFrequency == SF_Compute)
	{
		return new FNiagaraRibbonVertexFactoryShaderParametersVS();
	}
	else if (ShaderFrequency == SF_RayHitGroup)
	{
		return new FNiagaraRibbonVertexFactoryShaderParametersVS();
	}
#endif
	return NULL;
}

void FNiagaraRibbonVertexFactory::SetVertexBuffer(const FVertexBuffer* InBuffer, uint32 StreamOffset, uint32 Stride)
{
	check(Streams.Num() == 5);
	FVertexStream& VertexStream = Streams[0];
	VertexStream.VertexBuffer = InBuffer;
	VertexStream.Stride = Stride;
	VertexStream.Offset = StreamOffset;
}

void FNiagaraRibbonVertexFactory::SetDynamicParameterBuffer(const FVertexBuffer* InDynamicParameterBuffer, int32 ParameterIndex, uint32 StreamOffset, uint32 Stride)
{
	check(Streams.Num() == 5);
	FVertexStream& DynamicParameterStream = Streams[1 + ParameterIndex];
	if (InDynamicParameterBuffer)
	{
		DynamicParameterStream.VertexBuffer = InDynamicParameterBuffer;
		DynamicParameterStream.Stride = Stride;
		DynamicParameterStream.Offset = StreamOffset;
	}
	else
	{
		DynamicParameterStream.VertexBuffer = &GNullDynamicParameterVertexBuffer;
		DynamicParameterStream.Stride = 0;
		DynamicParameterStream.Offset = 0;
	}
}

///////////////////////////////////////////////////////////////////////////////

IMPLEMENT_VERTEX_FACTORY_TYPE(FNiagaraRibbonVertexFactory, "/Plugin/FX/Niagara/Private/NiagaraRibbonVertexFactory.ush", true, false, true, false, false);
